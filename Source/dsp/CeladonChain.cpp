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
    }

    void CeladonChain::prepare (const juce::dsp::ProcessSpec& spec)
    {
        // 四个模块用同一份 spec 整体重建（采样率 / 块大小 / 声道数）。模块统一接口（设计规格-总体）约定
        // prepare 末尾自己 reset，所以这里不必再清一遍。各处都按信号顺序写，读起来与 process 一致。
        balance.prepare (spec);
        clay.prepare (spec);
        color.prepare (spec);
        limiter.prepare (spec);

        // 两块表与编排器一起重建：两块表同时 reset，子块网格对齐（校准器按时间戳配对入口与出口）。
        // IN 表开着 LUFS（v0.5 为省 CPU 关掉过）：GLAZE 的分析器与校准器都要读它的 100 ms 子块，关着就永远听不完。
        inputMeter.prepare (spec);
        outputMeter.prepare (spec);
        controller.prepare (spec);
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

        // 编排器按宿主 reset 清运行状态（分析窗口、校准当前子窗、保护偏移），已提交的画像与方案保留。
        controller.hostReset();
        resetListenMeasurement (false);

        // 限幅器淡入之前输出与差值都是静音：试听差值的交叉淡化直接落到目标，不必再淡。
        differenceMix.setCurrentAndTargetValue (differenceMix.getTargetValue());
    }

    void CeladonChain::setHostInfo (const glaze::HostInfo& info) noexcept
    {
        controller.setHostInfo (info);
    }

    void CeladonChain::setParams (const Params& newParams, bool hostBypassed, bool compareOriginal, bool auditionDifference)
    {
        manual = newParams;

        // 听原声（A / B 的 B，v0.1.8.0）与宿主旁通走同一条干路：模块全旁通、编排器暂停校准（出口此刻是原声，校准器量它
        // 就会把「原声的响度」当成方案的结果）、Listen 的测量冻结。两者只在 Listen 的监听增益上不同（updateListenTarget）。
        const auto dryPath = hostBypassed || compareOriginal;

        // Listen 增益在输出测点之后独立施加，不反馈到方案、校准、保护层或响度估计。
        effective = controller.beginBlock (newParams, dryPath);

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
        listenRequested = newParams.limit.listen;
        const auto bypassChanged = hostIsBypassed != dryPath;
        hostIsBypassed = dryPath;
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
        const auto clayPressure  = clay.getLastProcessPressure();   // ADR-014：修坯峰值守护的代价格
        stats.clayPressureDb      = clayPressure.meanDb;
        stats.clayPressureSeconds = clayPressure.seconds;
        stats.musicActive = false;   // 编排器按分析器的有效信号门限填

        controller.endBlock (balance, inputMeter, outputMeter, stats, getLatencySamples(), true);
        processListen (io);

        if (withDifference)
            applyDifferenceAudition (io, nonFinite);
        else if (differenceInvolved())
            differenceMix.skip (io.getNumSamples());   // 声道数对不上（不该发生）：不试听，但交叉淡化照样走完，不卡在「在用」

        return nonFinite;
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
