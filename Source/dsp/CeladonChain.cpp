/*
  ==============================================================================

    CeladonChain.cpp

  ==============================================================================
*/

#include "CeladonChain.h"

#include <cmath>

namespace celadon
{
    namespace
    {
        bool containsNonFinite (const juce::AudioBuffer<float>& buffer) noexcept
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* data = buffer.getReadPointer (ch);

                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    if (! std::isfinite (data[i]))
                        return true;
            }

            return false;
        }

        // T-038 House 档「目标频谱 = 自动」的形状斜率（dB / 倍频程；设计规格-曲风与交付 第 3.1 节起点值，待 Max 试听调）。
        // 显式形状只用于回写实际参数（targetMode 必须是一种显式形状）；斜率另经 targetSlopeResolved* 无损回喂。
        float houseTargetSlopeFor (glaze::Genre genre) noexcept
        {
            switch (genre)
            {
                case glaze::Genre::deepHouse:          return -1.5f;
                case glaze::Genre::techHouse:          return -0.75f;
                case glaze::Genre::melodicProgressive: return -1.25f;
                case glaze::Genre::general:
                default:                               return -0.75f;
            }
        }

        TargetMode houseTargetShapeFor (glaze::Genre genre) noexcept
        {
            return houseTargetSlopeFor (genre) <= -1.25f ? TargetMode::warm : TargetMode::neutral;
        }
    }

    float CeladonChain::beatReleaseCapFor (glaze::Genre genre, bool autoRelease) const noexcept
    {
        const auto& house = glaze::factoryConstants().house;

        if (! glaze::isHouse (genre) || ! autoRelease || ! hostTempoValid || ! hostPlaying
            || ! (house.beatReleaseDivisor > 0.0f) || hostBpm < (double) house.beatMinBpm || hostBpm > (double) house.beatMaxBpm)
            return 0.0f;

        const auto capMs = 60000.0 / hostBpm / (double) house.beatReleaseDivisor;   // 一拍（四分音符）÷ 本项
        return std::isfinite (capMs) && capMs > 0.0 ? (float) capMs : 0.0f;
    }

    CeladonChain::CeladonChain()
    {
        controller.attachLiveProfile (&liveProfile);
    }

    void CeladonChain::syncGlazeLiveModel (glaze::Genre genre) noexcept
    {
        controller.notePendingModel (genre);
        controller.syncLiveModel();
    }

    void CeladonChain::prepare (const juce::dsp::ProcessSpec& spec)
    {
        // 换采样率时音频已停：先把 LIVE 留在内存里，两边 prepare（会冷启动）之后立刻读回。
        std::unique_ptr<juce::XmlElement> savedLive;
        if (controller.hasPrepared())
            savedLive = controller.createStateXml();

        // 四个模块用同一份 spec 整体重建（采样率 / 块大小 / 声道数）。模块统一接口（设计规格-总体）约定
        // prepare 末尾自己 reset，所以这里不必再清一遍。各处都按信号顺序写，读起来与 process 一致。
        balance.prepare (spec);
        clay.prepare (spec);
        clay.setGuardKeepReporting (true);   // T-074：守护保留比统计只由链里这一份报（分析器的先试候选不开，不多花 CPU）
        color.prepare (spec);
        limiter.prepare (spec);

        // 两块表与编排器一起重建：两块表同时 reset，子块网格对齐（校准器按时间戳配对入口与出口）。
        // IN 表开着 LUFS（v0.5 为省 CPU 关掉过）：GLAZE 的分析器与校准器都要读它的 100 ms 子块，关着就永远听不完。
        inputMeter.prepare (spec);
        outputMeter.prepare (spec);
        liveProfile.prepare (spec.sampleRate);
        controller.prepare (spec);

        if (savedLive != nullptr)
            controller.restoreStateXml (savedLive.get());

        controller.syncLiveModel();
        reference.prepare ({ spec.sampleRate, spec.maximumBlockSize, 2 });
        releaseSafety.prepare (spec.sampleRate);
        listenSampleRate = spec.sampleRate;
        listenGain.reset (listenSampleRate, 0.02);
        resetListenMeasurement (true);

        // 试听差值：差值缓冲按块大小分配（音频线程上不分配，大块时切片）；交叉淡化 20 ms，reset 直接落到此刻的目标。
        differenceBuffer.setSize ((int) juce::jmax ((juce::uint32) 1, spec.numChannels),
                                  (int) juce::jmax ((juce::uint32) 1, spec.maximumBlockSize));
        differenceBuffer.clear();
        differenceMix.reset (spec.sampleRate, 0.02);
    }

    void CeladonChain::reset()
    {
        balance.reset();
        clay.reset();
        color.reset();
        limiter.reset();

        // 发行端只把抖动随机序列回到固定种子（导出可复现）；预听增益、记住的读数与 previewActive 都保留（T-032 核验漏项）——
        // 两块表不清、整体读数还在，预听衰减接着生效。改前这里整个 reset：同规格 prepareToPlay 之后监听先回到 0 dB、再 50 ms 滑回。
        releaseSafety.resetDitherSequence();
        publishedPreviewGain.store (releaseSafety.getPreviewGainDb(), std::memory_order_relaxed);
        controller.hostReset();
        resetListenMeasurement (false);
        differenceMix.setCurrentAndTargetValue (differenceMix.getTargetValue());
    }

    void CeladonChain::restartWithFadeIn()
    {
        // 播放中途整条链清空：Balance、Clay 与 Color 清空后，它们延迟线里的头「Balance + Clay + Color 的延迟」个输出是静音，
        // 静音再流进同样刚清空的 Limit。Limit 的淡入门在链的最末端，只知道自己的延迟，所以把前三级的延迟
        // 作为额外保持段传给它——整条链的「空延迟」全部过完再 10 ms 淡入，不会从中间硬切回来。
        // Balance 零延迟（规格「v1 不加前瞻」），Clay 是固定的 D + G（线性相位收单 D + 峰值守护前瞻 G，48 kHz 为 574 + 528 = 1102
        // 样本，ADR-012 / ADR-014）：都按它们自己上报的延迟相加，Clay 的 D + G 因此整段落在淡入之前的静音里。
        // Clay 必须一起清（v0.1.7.0）：低切高通最低 10 Hz（衰减 60 dB 要约 0.3 s）、收单低通与 D、G 两段延迟线里存着重置前的音乐
        // （峰值守护的包络与增益也是重置前的状态），不清的话它们（或 NaN）会在淡入之后接着响出来。
        balance.reset();
        clay.reset();
        color.reset();
        limiter.restartWithFadeIn (balance.getLatencySamples() + clay.getLatencySamples() + color.getLatencySamples());

        // 发行端同 reset()：只把抖动随机序列回到固定种子，预听增益保留——淡入之后接着按原来的衰减监听，不先爆响再滑回。
        releaseSafety.resetDitherSequence();
        publishedPreviewGain.store (releaseSafety.getPreviewGainDb(), std::memory_order_relaxed);

        // 编排器按宿主 reset 清运行状态（分析窗口、校准当前子窗、保护偏移），已提交的画像与方案保留。
        controller.hostReset();
        resetListenMeasurement (false);

        // 限幅器淡入之前输出与差值都是静音：试听差值的交叉淡化直接落到目标，不必再淡。
        differenceMix.setCurrentAndTargetValue (differenceMix.getTargetValue());
    }

    void CeladonChain::setHostInfo (const glaze::HostInfo& info) noexcept
    {
        offline = info.offline;
        if (offline) previewRequested.store (false, std::memory_order_relaxed);
        hostTempoValid = info.hasTempo && std::isfinite (info.bpm) && info.bpm > 0.0;
        hostBpm = hostTempoValid ? info.bpm : 0.0;
        hostPlaying = offline || ! info.hasPlayState || info.isPlaying;   // 同编排器 hostPlaying()
        controller.setHostInfo (info);
    }

    void CeladonChain::setParams (const Params& requested, bool hostBypassed, bool compareOriginal, bool auditionDifference)
    {
        // GLAZE 的非有限值按 0（ChainParams 的默认 = 严格手动；T-032 R8-02）：编排器与切片只把「> 0」当作打开，+Inf 会被当成 100 %。
        // 有限值原样通过（整份拷贝，逐位不变）；处理器的 readParams 早已把非有限值换成默认值，这里管的是无头调用。
        auto newParams = requested;

        if (! std::isfinite (newParams.glazePercent))
            newParams.glazePercent = 0.0f;

        manual = newParams;

        // 听原声（A / B 的 B，v0.1.8.0）与宿主旁通走同一条干路：模块全旁通、编排器暂停校准（出口此刻是原声，校准器量它
        // 就会把「原声的响度」当成方案的结果）、Listen 的测量冻结。两者只在 Listen 的监听增益上不同（updateListenTarget）。
        const auto dryPath = hostBypassed || compareOriginal;

        // Listen 增益在输出测点之后独立施加，不反馈到方案、校准、保护层或响度估计。
        auto adjusted = newParams;
        auto& mastering = adjusted.mastering;
        const auto unit = [] (float value) { return std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value * 0.01f) : 0.0f; };
        // 已解析的强度（0 ～ 1）：回喂链的实际参数带着它（MasteringFeatures.h，T-032 R10-01），模式不是「关」时直接用、不再经乘法；
        // 插件从不设（−1 = 未设），走原来的算式——两个三元式的未设分支与改动前是同一个表达式，逐位不变。非有限按未设。
        const auto isResolved = [] (float value) { return std::isfinite (value) && value >= 0.0f; };
        // 非有限的百分比与预听目标按结构默认值算（T-032 R8-02：与处理器 readParams、各模块守卫同一口径——非有限 = 该字段的默认值）；
        // 有限值原样交给 unit，与改动前同一个表达式。GLAZE 的非有限值由 unit 按 0 算（ChainParams 的默认就是 0）。
        constexpr MasteringSettings neutralMastering {};
        const auto finiteOr = [] (float value, float fallback) { return std::isfinite (value) ? value : fallback; };
        const auto glaze = unit (newParams.glazePercent);
        auto& advanced = adjusted.balance;
        advanced.harshAmount = mastering.harshMode == HarshMode::off ? 0.0f
                               : isResolved (mastering.harshResolved) ? juce::jmin (1.0f, mastering.harshResolved)
                               : unit (finiteOr (mastering.harshAmount, neutralMastering.harshAmount))
                                   * (mastering.harshMode == HarshMode::automatic ? glaze : 1.0f);
        advanced.transientPreserve = unit (finiteOr (mastering.transientPreserve, neutralMastering.transientPreserve));
        advanced.targetAmount = mastering.targetMode == TargetMode::off ? 0.0f
                                : isResolved (mastering.targetResolved) ? juce::jmin (1.0f, mastering.targetResolved)
                                : unit (finiteOr (mastering.targetAmount, neutralMastering.targetAmount))
                                    * (mastering.targetMode == TargetMode::automatic ? glaze : 1.0f);
        // T-038：House 档「目标频谱 = 自动」按曲风取形状斜率（设计规格-曲风与交付 第 3.1 节；与按风格的映射一样写在链里、不进标定快照）。
        // 通用档照旧按风格；回喂的实际参数带着解析好的斜率时直接用（MasteringFeatures.h targetSlopeResolved*）。
        const auto genre = glaze::genreFromIndex (newParams.glazeGenre);
        const auto houseAutomaticTarget = mastering.targetMode == TargetMode::automatic && glaze::isHouse (genre);
        auto mode = mastering.targetMode;
        if (mode == TargetMode::automatic)
            mode = houseAutomaticTarget ? houseTargetShapeFor (genre)
                 : newParams.glazeStyle == 1 ? TargetMode::warm : (newParams.glazeStyle == 3 ? TargetMode::bright : TargetMode::neutral);
        // Per-octave energy: neutral follows a gentle downward tilt, warm/bright bracket it.
        advanced.targetSlopeDbPerOct = mastering.targetSlopeResolvedSet && std::isfinite (mastering.targetSlopeResolvedDbPerOct)
                                           ? mastering.targetSlopeResolvedDbPerOct
                                       : houseAutomaticTarget ? houseTargetSlopeFor (genre)
                                       : mode == TargetMode::warm ? -1.5f : (mode == TargetMode::bright ? 0.0f : -0.75f);
        const auto referenceStatus = reference.getSnapshot();
        advanced.hasTargetReference = mode == TargetMode::reference && referenceStatus.hasReference;
        advanced.targetReferenceDb = referenceStatus.relativeDb;
        if (mode == TargetMode::reference && ! referenceStatus.hasReference) advanced.targetAmount = 0.0f;
        // Ceiling 在链里先扣编码余量、再交给编排器与限幅器（下两行）：非有限值要在扣之前换成默认值（T-032 R8-02，与 GLAZE / 高级母带
        // 同一口径），否则限幅器自己的守卫只能把「扣过余量的 NaN」换成不含余量的默认值，与「该字段取默认值」的链差一个余量；
        // getEffectiveParams() 回写的 Ceiling 也因此总是有限。有限值原样通过，逐位不变。
        if (! std::isfinite (adjusted.limit.ceilingDb))
            adjusted.limit.ceilingDb = LimiterModule::Params {}.ceilingDb;

        // T-078：旁通 / 干路时界面显示这份用户 Ceiling（不扣余量、不封顶）。有限值替换已经做过。
        const auto userCeilingDb = adjusted.limit.ceilingDb;

        const auto margin = std::isfinite (mastering.codecMarginDb) ? juce::jlimit (0.0f, 1.5f, mastering.codecMarginDb) : 0.0f;
        if (margin > 0.0f) adjusted.limit.ceilingDb = juce::jlimit (-4.5f, 0.0f, adjusted.limit.ceilingDb - margin);
        // T-038 交付「流媒体」：有效 Ceiling 不高于 −2 dBTP（设计规格-曲风与交付 第 4.1 节；用户 Ceiling 更低时照用户的）。
        // 与编码余量一样在交给编排器之前扣，校准参考条件里的 Ceiling 因此跟着变。标准 / 俱乐部不动，逐位同现在。
        if (glaze::deliveryFromIndex (newParams.glazeDelivery) == glaze::Delivery::streaming)
        {
            const auto cap = glaze::factoryConstants().delivery.streamingTruePeakCapDbtp;
            if (std::isfinite (cap))
                adjusted.limit.ceilingDb = juce::jmin (adjusted.limit.ceilingDb, juce::jlimit (-4.5f, 0.0f, cap));
        }
        effective = controller.beginBlock (adjusted, dryPath);
        // Saved GLAZE execution snapshots predate these user-controlled additions.
        // 回写的是交给模块的实际值（CeladonChain.h getEffectiveParams；T-032 R10-01）：h、t 写成算好的那个 float，推响不刺的模式改手动
        //（关仍是关）、目标频谱的模式改成解析出的显式形状（「参考」还没学好 → 关，强度本来就是 0），编码余量置 0（已扣进 ceilingDb）。
        // 把 effective 原样再喂给链（GLAZE 0）时逐位得到同样的 h、t、Ceiling。
        effective.mastering = mastering;
        effective.mastering.harshMode = mastering.harshMode == HarshMode::off ? HarshMode::off : HarshMode::manual;
        effective.mastering.harshResolved = advanced.harshAmount;
        effective.mastering.targetMode = mode == TargetMode::reference && ! referenceStatus.hasReference ? TargetMode::off : mode;
        effective.mastering.targetResolved = advanced.targetAmount;
        if (houseAutomaticTarget)
        {
            effective.mastering.targetSlopeResolvedSet = true;
            effective.mastering.targetSlopeResolvedDbPerOct = advanced.targetSlopeDbPerOct;
        }
        effective.mastering.codecMarginDb = 0.0f;
        effective.balance.harshAmount = advanced.harshAmount;
        effective.balance.transientPreserve = advanced.transientPreserve;
        effective.balance.targetAmount = advanced.targetAmount;
        effective.balance.targetSlopeDbPerOct = advanced.targetSlopeDbPerOct;
        effective.balance.hasTargetReference = advanced.hasTargetReference;
        effective.balance.targetReferenceDb = advanced.targetReferenceDb;
        effective.limit.ceilingDb = adjusted.limit.ceilingDb;
        // T-038 限幅跟拍（设计规格-曲风-House学习方式 第 8 节）：回喂的实际参数已带正的上限时照用（同决策重放），否则按此刻的曲风、
        // 释放方式与宿主速度算；条件不满足为 0（不封顶，逐位同现在）。常数取出厂值（链不持有标定快照，同上面的目标频谱映射）。
        if (! (std::isfinite (effective.limit.beatReleaseCapMs) && effective.limit.beatReleaseCapMs > 0.0f))
            effective.limit.beatReleaseCapMs = beatReleaseCapFor (genre, effective.limit.autoRelease);
        releaseAllowed = ! dryPath && ! auditionDifference;
        if (! releaseAllowed || offline) previewRequested.store (false, std::memory_order_relaxed);
        const auto nextPreview = releaseAllowed && ! offline && previewRequested.load (std::memory_order_relaxed);
        const auto leaving = previewActive && ! nextPreview;
        const auto previewGainNow = releaseSafety.getCurrentPreviewGain();
        const auto previewGainPending = previewGainNow != 1.0;   // 预听增益还没回到 1（刚退出，或退出斜坡还在走）

        if (nextPreview && ! previewActive)
        {
            // 进入预听：把此刻送出去的整个监听增益（Listen × 可能还没走完的预听退出斜坡）交给预听，Listen 归一——
            // 两条各自在动的斜坡不叠加。预听增益已经是 1 时（通常情况）与改动前逐位相同。
            releaseSafety.setCurrentPreviewGain ((double) listenGain.getCurrentValue() * previewGainNow);
            listenGain.setCurrentAndTargetValue (1.0f);
        }
        else if (! nextPreview)
        {
            // 预听此刻关着（这一块刚退出，或早已退出；T-032 R8-01 / R10-03）：
            //   - 在线、链照常出母带（releaseAllowed）、Listen 关：ReleaseSafety 自己按 50 ms 斜坡回 1，走完精确落 1、转回逐位旁通；
            //   - 离线：立即归一，导出从第一个样本起就不带预听，也不带斜坡尾巴；
            //   - 其余在线情形（宿主旁通 / 听原声 / 试听差值时 releaseSafety.process 不再被调，它的斜坡走不动；Listen 开时 Listen 要接手
            //     监听增益）：先把此刻的衰减交给 Listen 的平滑器，再归一，随后 updateListenTarget 在 20 ms 内平滑到 Listen 的目标
            //     （Listen 关或干路 = 1），applyListenGain 在所有路径上都执行，斜坡落得了地。乘法平滑器的起点不能是 0，下限 1e-6。
            // 宿主旁通 / 听原声 / 试听差值期间每块都归一（与改动前相同：抖动随机序列也每块回到固定种子）。
            const auto rampsItself = releaseAllowed && ! offline && ! newParams.limit.listen;

            if (! releaseAllowed || ((leaving || previewGainPending) && ! rampsItself))
            {
                if (! offline && (leaving || previewGainPending))
                    listenGain.setCurrentAndTargetValue (juce::jmax (1.0e-6f, (float) ((double) listenGain.getCurrentValue() * previewGainNow)));

                releaseSafety.reset();
            }
        }
        previewActive = nextPreview;
        const auto bits = releaseAllowed ? (mastering.dither == DitherMode::bits16 ? 16 : (mastering.dither == DitherMode::bits24 ? 24 : 0)) : 0;
        releaseSafety.setParams (previewActive, finiteOr (mastering.previewTargetLufs, neutralMastering.previewTargetLufs), bits);
        const auto limitIdle = dryPath || adjusted.limit.bypass;
        publishedCodecMargin.store (limitIdle ? 0.0f : margin, std::memory_order_relaxed);
        // 干活时发布交给限幅器的 Ceiling（余量、钳位、流媒体封顶都已算进，与 effective.limit.ceilingDb 相同）；
        // 旁通 / 干路发布用户 Ceiling，界面因此不把没在用的封顶画成超限。
        publishedEffectiveCeiling.store (limitIdle ? userCeilingDb : adjusted.limit.ceilingDb, std::memory_order_relaxed);
        publishedDither.store (bits == 16 ? 1 : (bits == 24 ? 2 : 0), std::memory_order_relaxed);

        // 宿主旁通不另走一条路，而是把每个模块都置为旁通：各模块自带等长延迟的干路与 20 ms 交叉淡化
        // （Balance 零延迟，干路就是输入本身；Clay 的干路是 D 样本纯延迟线、再与湿路一起过峰值守护的 G 样本延迟，共 D + G），
        // 整条链旁通后输出仍是延迟补偿过的干信号——延迟不变、切换无咔哒，与模块自己的 Bypass 完全一致。
        // Clay 的实际参数（v0.1.7.0）：effective.clay 由编排器 beginBlock 按三态合成（自动 = 方案 + 实时保护，GLAZE 0 为中性；
        // 关 = 中性；手动 = 旋钮值，GLAZE 0 也生效；bypass = 用户的 Clay Bypass），见 ChainParams.h 文件头。
        // newParams.clay 不读：处理器只填 claySettings。
        auto balanceParams = effective.balance;
        auto clayParams    = effective.clay;
        auto colorParams   = effective.color;
        auto limitParams   = effective.limit;

        balanceParams.bypass = balanceParams.bypass || dryPath;
        clayParams.bypass    = clayParams.bypass    || dryPath;
        colorParams.bypass   = colorParams.bypass   || dryPath;
        limitParams.bypass   = limitParams.bypass   || dryPath;
        limitParams.listen = false;   // 独立 Limiter 仍支持旧 Listen；整链使用实测的全链监听补偿。

        balance.setParams (balanceParams);
        clay.setParams (clayParams);
        color.setParams (colorParams);
        limiter.setParams (limitParams);
        listenRequested = newParams.limit.listen && ! previewActive;
        const auto bypassChanged = hostIsBypassed != dryPath;
        hostIsBypassed = dryPath;

        // 听原声期间 OUT 表的整体响度暂停积分（T-032 R8-03；设计规格-总体 2026-09-28 修订「三种临时监听……表与 GLAZE 照常按母带算」）：
        // 出口此刻是原声，积进去就会把原声混进母带的整体读数，预听归一化也跟着偏。短期响度、真峰值、子块环照常（校准与 Listen 的配对不经直方图）。
        // 宿主旁通接在同一个开关上，但要等 Max 确认口径（pauseIntegrationOnHostBypass，见 CeladonChain.h）：现在宿主旁通时照旧积分。
        outputMeter.setIntegrationPaused (compareOriginal || (hostBypassed && pauseIntegrationOnHostBypass));
        listenComparing = compareOriginal && ! hostBypassed;
        if (listenSampleRate > 0.0 && (bypassChanged || listenLatency != getLatencySamples()))
            resetListenMeasurement (false);
        updateListenTarget();

        // 试听差值：LIMIT 此刻真的在处理才听得到它削掉的部分（limitParams.bypass 已含听原声与宿主旁通）。
        differenceMix.setTargetValue (auditionDifference && ! limitParams.bypass ? 1.0f : 0.0f);
    }

    bool CeladonChain::process (juce::AudioBuffer<float>& io)
    {
        // Deliver spectral evidence once per host callback, before its input.
        // Guard's internal statistics partitions must not change which frames
        // are visible at the next host parameter update (including restores).
        controller.analyzeSpectrum (balance);
        lastProcessSliceCount = 1;
        // 试听差值要把限幅器的差值整段存下来：只在试听期间、宿主块又超过差值缓冲（= prepare 的块大小）时才按它切片。
        const auto glazeActive = manual.glazePercent > 0.0f;
        const auto sliceLimit = differenceInvolved() && differenceBuffer.getNumSamples() > 0 ? differenceBuffer.getNumSamples() : io.getNumSamples();
        if ((! glazeActive && io.getNumSamples() <= sliceLimit) || io.getNumSamples() <= 0)
            return processSlice (io);
        // Never smear a host block's reduction mean/max across multiple Guard
        // slots. Small callbacks contribute their true partial statistics;
        // large callbacks are split exactly at the current active-music slot.
        // This changes neither sample timing nor the reported audio latency.
        bool nonFinite = false;
        lastProcessSliceCount = 0;
        for (int offset = 0; offset < io.getNumSamples();)
        {
            ++lastProcessSliceCount;
            // Keep the host-block parameter contract: every slice uses the
            // same executed settings. Only statistics advance here; a newly
            // computed correction becomes audible at the next setParams.
            auto length = io.getNumSamples() - offset;
            if (glazeActive)
                length = juce::jmin (length, juce::jmax (1, controller.getGuardSamplesUntilBoundary()));
            length = juce::jmin (length, juce::jmax (1, sliceLimit));
            juce::AudioBuffer<float> slice (io.getArrayOfWritePointers(), io.getNumChannels(), offset, length);
            nonFinite = processSlice (slice) || nonFinite;
            offset += length;
        }
        if (nonFinite) io.clear();
        return nonFinite;
    }

    bool CeladonChain::processSlice (juce::AudioBuffer<float>& io)
    {
        // 入口测点：IN 表先测（真峰值给界面，K 加权子块给分析器与校准器），分析器在模块处理之前读入口原始样本。
        inputMeter.process (io);
        controller.analyzeInput (io, inputMeter);

        // 顺序固定（设计规格-总体；v0.1.7.0 起 ADR-009）：先修频谱问题，再修声像与超低频，再加密度与谐波，最后管峰值。
        // 反过来会互相打架：限幅器先压掉的东西，后面的模块再想抬也抬不回来；Balance 若放在 Color 之后，它看到的是
        // 已加了谐波与压缩的谱，修正会跟着 Heat / Glue 的动作抖。
        // Clay 夹在 Balance 与 Color 之间（设计规格-Clay 第 3 节）：Balance 的分析帧（GLAZE 画像的频谱与立体声证据）取自它自己的
        // 入口 = 链入口原始信号，Clay 放到 Balance 前面就会让画像看到 Clay 自己的结果、形成闭环；放到 Color 之后，Glue 检测、Heat、
        // Clip、限幅器就都得吃 30 Hz 以下白占余量、加重互调的能量。
        balance.process (io);
        clay.process (io);
        color.process (io);

        // 试听差值（v0.1.8.0）：限幅器顺带写出本级削掉的部分；下面所有测点照常读母带，最后才换到输出上。
        const auto withDifference = differenceInvolved()
                                 && io.getNumChannels() <= differenceBuffer.getNumChannels()
                                 && io.getNumSamples() <= differenceBuffer.getNumSamples();
        limiter.process (io, withDifference ? &differenceBuffer : nullptr);

        // 输出有 NaN / Inf（内部溢出）：置零，清空整条链（NaN 会留在延迟线和滤波器里）并淡入恢复。
        // 两块表不清：它们只吃过有限值，整体响度不该因此丢失。（v0.5 在处理器里做，挪进链里让无头测试也覆盖。）
        const auto nonFinite = containsNonFinite (io);

        if (nonFinite)
        {
            io.clear();
            restartWithFadeIn();
        }

        outputMeter.process (io);

        // 模块内部测点（保护层、约束反馈）与最终输出的真峰值，同一块的量一起交给编排器。
        glaze::ModuleStats stats;
        stats.numSamples = io.getNumSamples();

        const auto& colorStats = color.getBlockStats();
        stats.glueGrMeanDb = colorStats.glueGrMeanDb;
        stats.glueGrMaxDb  = colorStats.glueGrMaxDb;
        stats.heatDrive    = colorStats.heatDrive;

        const auto& limitStats = limiter.getBlockStats();
        stats.limitGrMeanDb      = limitStats.grMeanDb;
        stats.limitGrMaxDb       = limitStats.grMaxDb;
        stats.clipActiveFraction = limitStats.clipActiveFraction;
        stats.clipDepthMaxDb     = limitStats.clipDepthMaxDb;

        stats.outputTruePeakDbtp = juce::Decibels::gainToDecibels (outputMeter.getLastBlockTruePeakLinear(), -100.0f);
        const auto clayPressure  = clay.getLastProcessPressure();   // ADR-014 / ADR-028：压力反馈的计时与峰值块
        stats.clayPressureSeconds   = clayPressure.seconds;
        stats.clayPeakBlocks        = clayPressure.peakBlocks;
        stats.clayPeakBlockCount    = clayPressure.numPeakBlocks;
        stats.clayPeakBlocksDropped = clayPressure.droppedPeakBlocks;
        const auto clayBlend      = clay.getLastProcessBlend();      // T-043：守护的混合损失（ADR-027）
        stats.clayBlendIntendedEnergy = clayBlend.intendedEnergy;
        stats.clayBlendOutputEnergy   = clayBlend.outputEnergy;
        stats.clayBlendSeconds        = clayBlend.seconds;
        const auto clayKeep       = clay.getLastProcessGuardKeep();  // T-074：守护保留比统计（界面的「守护在回收」指示）
        stats.clayGuardChangeEnergy = clayKeep.changeEnergy;
        stats.clayGuardKeptEnergy   = clayKeep.keptEnergy;
        stats.musicActive = false;   // 编排器按分析器的有效信号门限填

        controller.endBlock (balance, inputMeter, outputMeter, stats, getLatencySamples(), true, &io);
        processListen (io);

        if (withDifference)
            applyDifferenceAudition (io, nonFinite);
        else if (differenceInvolved())
            differenceMix.skip (io.getNumSamples());   // 声道数对不上（不该发生）：不试听，但交叉淡化照样走完，不卡在「在用」

        if (releaseAllowed && ! differenceInvolved() && ! nonFinite)
            releaseSafety.process (io, outputMeter.getIntegratedLufs());
        publishedPreviewGain.store (releaseSafety.getPreviewGainDb(), std::memory_order_relaxed);
        return nonFinite;
    }

    MasteringStatus CeladonChain::getMasteringStatus() const noexcept
    {
        MasteringStatus result;
        const auto ref = reference.getSnapshot();
        result.referenceState = static_cast<ReferenceState> (ref.state);
        result.referenceSeconds = ref.seconds;
        result.hasReference = ref.hasReference;
        result.sidechainConnected = ref.sidechainConnected;
        result.previewEnabled = isReleasePreviewEnabled();
        result.harshReductionDb = balance.getHarshReductionDb();
        result.previewGainDb = publishedPreviewGain.load (std::memory_order_relaxed);
        result.codecMarginDb = publishedCodecMargin.load (std::memory_order_relaxed);
        result.effectiveCeilingDb = publishedEffectiveCeiling.load (std::memory_order_relaxed);
        result.dither = static_cast<DitherMode> (publishedDither.load (std::memory_order_relaxed));
        balance.getTargetCorrectionDb (result.targetCorrectionDb);
        return result;
    }

    void CeladonChain::applyDifferenceAudition (juce::AudioBuffer<float>& io, bool outputCleared)
    {
        // 测点都已读过母带：这里只改送给宿主的监听信号。输出刚因非有限值被清零时，差值同样作废。
        const auto numSamples = io.getNumSamples();
        const auto numChannelsToMix = io.getNumChannels();

        if (outputCleared)
            differenceBuffer.clear (0, numSamples);

        auto* const* out = io.getArrayOfWritePointers();
        const auto* const* removed = differenceBuffer.getArrayOfReadPointers();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = differenceMix.getNextValue();

            if (mix <= 0.0f)
                continue;

            for (int ch = 0; ch < numChannelsToMix; ++ch)
                out[ch][i] = mix >= 1.0f ? removed[ch][i] : out[ch][i] + (removed[ch][i] - out[ch][i]) * mix;
        }

        // 兜底：差值里若有非有限值（上游溢出、母带却恰好被钳住），这一段静音，不把 NaN 送给宿主。
        if (containsNonFinite (io))
            io.clear();
    }

    void CeladonChain::resetListenMeasurement (bool resetGain)
    {
        listenLatency = getLatencySamples();
        listenHasEstimate = false;
        listenInputEnergy = listenOutputEnergy = 0.0;
        listenInputHistory.fill ({});
        listenWindowIn.fill (0.0);
        listenWindowOut.fill (0.0);
        listenWindowWrite = listenWindowCount = 0;
        // 表不随宿主 reset 清空；丢弃旧子块，并避开跨 reset 的部分窗口与延迟尾巴。
        std::array<LoudnessMeter::SubBlock, LoudnessMeter::subBlockRingCapacity> discarded;
        listenInputCursor = listenOutputCursor = 0;
        inputMeter.readSubBlocksSince (listenInputCursor, discarded.data(), (int) discarded.size());
        outputMeter.readSubBlocksSince (listenOutputCursor, discarded.data(), (int) discarded.size());
        listenIgnoreThrough = outputMeter.getProcessedSamples() + listenLatency + 2 * outputMeter.getSubBlockLength();
        if (resetGain)
        {
            listenEstimateDb = 0.0f;
            listenOriginalDb = 0.0f;
            listenGain.setCurrentAndTargetValue (1.0f);
            listenDifferenceValid.store (false, std::memory_order_relaxed);
            listenDifferenceDb.store (0.0f, std::memory_order_relaxed);
        }
    }

    void CeladonChain::updateListenTarget()
    {
        // 只衰减：较轻的处理结果不会被抬高，避免监听补偿制造新的峰值超限。
        // 听原声（A / B 的 B）时同一条规矩反过来用：原声比处理结果响才衰减原声；原声更轻时原样播放——A 那边已经由
        // listenEstimateDb 衰减到原声的响度，两边照样等响。宿主旁通（listenComparing 为 false）仍不补偿。
        auto db = 0.0f;

        if (listenRequested)
            db = ! hostIsBypassed ? listenEstimateDb : (listenComparing ? listenOriginalDb : 0.0f);

        listenGain.setTargetValue (juce::Decibels::decibelsToGain (db));
    }

    void CeladonChain::applyListenGain (juce::AudioBuffer<float>& io, int start, int length)
    {
        if (! listenGain.isSmoothing() && listenGain.getCurrentValue() == 1.0f)
            return;   // Listen 关着时保持原有逐位路径。
        for (int i = start; i < start + length; ++i)
        {
            const auto gain = listenGain.getNextValue();
            for (int ch = 0; ch < io.getNumChannels(); ++ch)
                io.getWritePointer (ch)[i] *= gain;
        }
    }

    void CeladonChain::processListen (juce::AudioBuffer<float>& io)
    {
        constexpr int capacity = LoudnessMeter::subBlockRingCapacity;
        auto& in = listenInputRead;
        auto& out = listenOutputRead;
        const auto inputs = inputMeter.readSubBlocksSince (listenInputCursor, in.data(), capacity);
        const auto outputs = outputMeter.readSubBlocksSince (listenOutputCursor, out.data(), capacity);
        if (inputs < 0 || outputs < 0)
        {
            // 超大宿主块或计量中断导致环形数据丢失：保持已有监听增益，等连续新窗口，不能配错旧数据。
            resetListenMeasurement (false);
            applyListenGain (io, 0, io.getNumSamples());
            return;
        }
        const auto blockLength = inputMeter.getSubBlockLength();
        for (int i = 0; i < inputs; ++i)
            listenInputHistory[(size_t) ((in[(size_t) i].endSample / blockLength) % capacity)] = in[(size_t) i];
        const auto inputAt = [&] (std::int64_t end) -> const LoudnessMeter::SubBlock*
        {
            if (end <= 0) return nullptr;
            const auto& entry = listenInputHistory[(size_t) ((end / blockLength) % capacity)];
            return entry.endSample == end ? &entry : nullptr;
        };
        const auto latencyBlocks = listenLatency / blockLength;
        const auto fraction = (double) (listenLatency % blockLength) / blockLength;
        const auto startSample = outputMeter.getProcessedSamples() - io.getNumSamples();
        int applied = 0;
        for (int i = 0; i < outputs; ++i)
        {
            const auto& output = out[(size_t) i];
            const auto offset = (int) juce::jlimit ((std::int64_t) 0, (std::int64_t) io.getNumSamples(), output.endSample - startSample);
            applyListenGain (io, applied, offset - applied);
            applied = offset;
            if (hostIsBypassed || output.endSample <= listenIgnoreThrough) continue;
            const auto* current = inputAt (output.endSample - (std::int64_t) latencyBlocks * blockLength);
            const auto* previous = inputAt (output.endSample - (std::int64_t) (latencyBlocks + 1) * blockLength);
            if (current == nullptr || (fraction > 0.0 && previous == nullptr)) continue;
            // 延迟移动一个 100 ms 均方窗口：按重叠比例近似；不能重建窗口内瞬态的位置。
            const auto input = (1.0 - fraction) * current->meanSquare + (previous != nullptr ? fraction * previous->meanSquare : 0.0);
            if (! std::isfinite (input) || ! std::isfinite (output.meanSquare))
                continue;
            // 必须先累计完整窗口，静音也进入：不能把跨 100 ms 边界的一次鼓点的部分 IN 与完整 OUT 比较。
            const auto newest = listenWindowWrite;
            listenWindowIn[(size_t) newest] = input;
            listenWindowOut[(size_t) newest] = output.meanSquare;
            listenWindowWrite = (listenWindowWrite + 1) % listenWindowBlocks;
            listenWindowCount = juce::jmin (listenWindowBlocks, listenWindowCount + 1);
            if (listenWindowCount < listenWindowBlocks) continue;
            double windowInput = 0.0, windowOutput = 0.0;
            for (int j = 0; j < listenWindowBlocks; ++j)
            {
                windowInput += listenWindowIn[(size_t) j];
                windowOutput += listenWindowOut[(size_t) j];
            }
            // 绝对门限约 −60 LUFS；静音与长尾冻结，不把噪底误当需补偿的音乐。
            if (windowInput < 1.0e-6 * listenWindowBlocks || windowOutput < 1.0e-6 * listenWindowBlocks)
                continue;
            // 延迟插值只能近似窗口两端。边缘某子块占总能量 >20% 时，等该瞬态移进窗口内部；
            // 持续信号每块约10%，孤立边缘脉冲接近100%。这道可靠性门避免错误深衰减冻结到下一击。
            const auto oldest = listenWindowWrite;
            if (juce::jmax (listenWindowIn[(size_t) oldest], listenWindowIn[(size_t) newest]) > 0.2 * windowInput
                || juce::jmax (listenWindowOut[(size_t) oldest], listenWindowOut[(size_t) newest]) > 0.2 * windowOutput)
                continue;
            if (! listenHasEstimate)
            {
                listenInputEnergy = windowInput;
                listenOutputEnergy = windowOutput;
                listenHasEstimate = true;
            }
            else
            {
                // 1 s 窗口之外每 100 ms 平滑，约 0.28 s 时间常数；监听开着也持续估计，便于边调边听。
                listenInputEnergy += 0.3 * (windowInput - listenInputEnergy);
                listenOutputEnergy += 0.3 * (windowOutput - listenOutputEnergy);
            }
            const auto measuredDb = (float) (10.0 * std::log10 (listenInputEnergy / listenOutputEnergy));
            listenEstimateDb = juce::jlimit (-24.0f, 0.0f, measuredDb);
            listenOriginalDb = juce::jlimit (-24.0f, 0.0f, -measuredDb);

            // 界面读的是「处理结果 − 原声」：measuredDb 是原声 − 处理结果，取反。只写原子量，不改任何音频路径。
            listenDifferenceDb.store (juce::jlimit (-24.0f, 24.0f, -measuredDb), std::memory_order_relaxed);
            listenDifferenceValid.store (true, std::memory_order_relaxed);
            updateListenTarget();
        }
        applyListenGain (io, applied, io.getNumSamples() - applied);
    }

    int CeladonChain::getLatencySamples() const noexcept
    {
        // 总延迟 = 各模块之和：Balance 零延迟（分析滞后不算延迟）、Clay 固定 D + G（线性相位收单 D + 峰值守护前瞻 G，prepare 后
        // 不变，ADR-012 / ADR-014）、Color 固定（Heat 过采样滤波器）、Limit 随 Lookahead 变；都在 setParams 后立即反映。
        // 48 kHz / Lookahead 2 ms：282 + 1102 = 1384 样本；96 kHz：378 + 2206 = 2584；192 kHz：4984（T-016 逐级基准实测）
        //（设计规格-Clay-根治修订 第 6 节）。
        return balance.getLatencySamples() + clay.getLatencySamples() + color.getLatencySamples() + limiter.getLatencySamples();
    }

    float CeladonChain::getAndResetLimitGainReductionDb() noexcept
    {
        return limiter.getAndResetGainReductionDb();
    }

    float CeladonChain::getAndResetGlueGainReductionDb() noexcept
    {
        return color.getAndResetGainReductionDb();
    }

    void CeladonChain::getBalanceBandGainsDb (BalanceModule::BandGains& out) const noexcept
    {
        // 直接转发：模块内每段一个原子变量、每 hop 更新，界面线程读不持锁、不阻塞音频线程。
        balance.getBandGainsDb (out);
    }
}
