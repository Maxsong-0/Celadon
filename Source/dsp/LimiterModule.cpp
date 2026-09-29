/*
  ==============================================================================

    LimiterModule.cpp

  ==============================================================================
*/

#include "LimiterModule.h"

#include "AtomicHelpers.h"

#include <cmath>
#include <cstdint>

// MSVC：VS2022（v143）的 /fp:precise 默认不融合乘加；更早的工具集（v142）配 /arch:AVX2 时 /fp:precise 下仍可能融合，
// 这里写死关掉（ADR-013：各路径逐位相同的前提）。clang-cl 不认这个 pragma，由 /clang:-ffp-contract=off 与源码守卫管。
#if defined (_MSC_VER) && ! defined (__clang__)
 #pragma fp_contract (off)
#endif

// v0.6 每块统计里 Clip 削波活动的只读扫描（4× 域，每块数千个样本）按 4 路并行做；非 ARM / x86 平台走标量回退。
// 三条路径是同一组比较与取大，结果逐位相同（计数是整数；取大忽略 NaN）。CELADON_FORCE_SCALAR_SIMD 只给验证用。
#if ! defined (CELADON_FORCE_SCALAR_SIMD) && defined (__ARM_NEON) && defined (__aarch64__)
 #define CELADON_LIMITER_STATS_NEON 1
 #include <arm_neon.h>
#elif ! defined (CELADON_FORCE_SCALAR_SIMD) && (defined (__SSE2__) || defined (_M_X64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 2))
 #define CELADON_LIMITER_STATS_SSE2 1
 #include <emmintrin.h>
#endif

namespace celadon
{
    namespace
    {
        constexpr double smoothingSeconds       = 0.02;   // 参数平滑 20 ms
        constexpr double fadeInSeconds          = 0.01;   // Lookahead 变动后的淡入
        constexpr float  clipOffThresholdDb     = 12.0f;  // Clip 刚打开时的削波阈值（+12 dBTP ≈ 不削）
        constexpr float  fastReleaseMs          = 30.0f;  // Auto 快段
        constexpr float  slowTargetSmoothingMs  = 50.0f;  // Auto 慢段跟的是「50 ms 平滑后的需求」
        constexpr float  slowReleaseMinMs       = 100.0f; // Auto 慢段释放下限
        constexpr float  slowReleaseMaxMs       = 800.0f; // Auto 慢段释放上限
        constexpr float  averageWindowMs        = 500.0f; // 「最近 500 ms 的平均衰减」
        constexpr float  deepReductionDb        = 12.0f;  // 平均衰减达到这么深时慢段释放取上限（初版，试听后调）
        constexpr int    adaptIntervalSamples   = 32;     // 慢段释放时间每 32 样本重算一次
        constexpr float  maxLookaheadMs         = 10.0f;
        constexpr double lowFrequencyCutoffHz   = 150.0;
        constexpr double lowFrequencyMinHz      = 25.0;
        constexpr double lowStableSeconds       = 0.12;
        constexpr double lowReleaseMaxMs        = 140.0;
        constexpr int    lowConsistentPeriods   = 3;
        // 短过载：需求增益回到 0.93（约 −0.6 dB）之前，连续压住的时间短于 70 ms，就视为鼓点而不是持续响度。
        constexpr float  transientReturnGain    = 0.93f;
        constexpr double transientMaxSeconds    = 0.070;

        // 8× / 64 抽头检测器相对独立 16× / 128 抽头 sinc，在短宽带噪声网格中低估最多 0.3039 dB。
        // 需求增益预留 0.35 dB，避免为此再加一条高开销的过采样链；这是经验重建余量，不是所有波形的数学保证。
        // 只作用在限幅需求：用户 Ceiling、Clip 阈值、输出硬钳位都不变，低于内部阈值的信号也不加固定衰减。
        constexpr float detectorSafetyGain = 0.9605058184f;   // 10^(−0.35 / 20)，音频线程只做一次乘法

        // 试听差值除以 Gain 之前的下限：Gain 参数最低 0 dB，这里只防 −100 dB 以下被换算成 0 时除零。
        constexpr float minimumDifferenceGain = 1.0e-6f;

        // v0.6 每块统计。GR 均值的每样本下限 120 dB（增益 1e-6），与表的 −120 dB 下限一致；
        // 连乘的积小于 1e-200 就把 2 的幂次移出（此后再乘一个 ≥ 1e-6 的增益也到不了 double 的下溢区）。
        constexpr float  statsMinGain           = 1.0e-6f;
        constexpr double statsRenormalizeBelow  = 1.0e-200;
        constexpr double decibelsPerNeper       = 8.6858896380650365;   // 20 / ln(10)：ln(增益) → dB
        constexpr double naturalLogOf2          = 0.69314718055994531;  // ln(2)：frexp 移出的幂次 → ln

        // 时间常数 → 一阶系数：每样本 y = target + (y − target) · coeff。
        double timeConstantCoeff (double sampleRate, double milliseconds)
        {
            return std::exp (-1.0 / (milliseconds * 0.001 * sampleRate));
        }

        // 已经非常接近 1 且目标就是 1 时直接落到 1.0：恢复后移动平均才能回到精确的 1.0，
        // null 才重新是零。double 下包络能一路收敛到 1e-11 量级，这个阈值一定会被穿过。
        double settle (double value, double target) noexcept
        {
            return (target >= 1.0 && value > 1.0 - 1.0e-9) ? 1.0 : value;
        }

        // 带释放的一阶跟随：向下瞬时跟随，向上按 coeff 指数回升。
        double followWithRelease (double current, double target, double coeff) noexcept
        {
            if (target <= current)
                return target;

            return settle (target + (current - target) * coeff, target);
        }
    }

    //==============================================================================
    void LimiterModule::SlidingMinimum::prepare (int maxWindow)
    {
        values.assign ((size_t) (maxWindow + 1), 1.0f);
        indices.assign ((size_t) (maxWindow + 1), 0);
        clear();
    }

    void LimiterModule::SlidingMinimum::clear() noexcept
    {
        head = 0;
        count = 0;
        sampleIndex = 0;
    }

    float LimiterModule::SlidingMinimum::push (float value) noexcept
    {
        const auto capacity = (int) values.size();

        // 队尾比新值大（或相等）的元素以后永远不可能成为最小值，丢掉。
        while (count > 0)
        {
            const auto back = (head + count - 1) % capacity;

            if (values[(size_t) back] < value)
                break;

            --count;
        }

        const auto slot = (head + count) % capacity;
        values[(size_t) slot] = value;
        indices[(size_t) slot] = sampleIndex;
        ++count;

        // 队头已经滑出窗口的丢掉。
        while (indices[(size_t) head] <= sampleIndex - window)
        {
            head = (head + 1) % capacity;
            --count;
        }

        ++sampleIndex;
        return values[(size_t) head];
    }

    //==============================================================================
    void LimiterModule::MovingAverage::prepare (int maxLength)
    {
        ring.assign ((size_t) juce::jmax (1, maxLength), 1.0f);
        clear();
    }

    void LimiterModule::MovingAverage::clear() noexcept
    {
        std::fill (ring.begin(), ring.end(), 1.0f);
        writeIndex = 0;
        sum = (double) length;
    }

    float LimiterModule::MovingAverage::push (float value) noexcept
    {
        sum += (double) value - (double) ring[(size_t) writeIndex];
        ring[(size_t) writeIndex] = value;

        if (++writeIndex == length)
        {
            writeIndex = 0;

            // 每转一圈整段重算一次：累加误差清零，全是 1.0 时结果精确等于 1.0。
            sum = 0.0;
            for (int i = 0; i < length; ++i)
                sum += (double) ring[(size_t) i];
        }

        return (float) juce::jmin (1.0, sum / (double) length);
    }

    //==============================================================================
    void LimiterModule::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate   = spec.sampleRate;
        maxBlockSize = juce::jmax (1, (int) spec.maximumBlockSize);
        numChannels  = (int) juce::jmax ((juce::uint32) 1, spec.numChannels);

        // Clip 级：4×（2 级半带）FIR 等纹波，线性相位。
        // 两级参数与 JUCE 的「最高质量」预设相同，只把第 2 级下采样阻带从 −65 dB 改成 −70 dB：
        // 这样两级滤波器的延迟加起来正好是整数（48k 下 60 样本），不需要再补一级 Thiran 全通做
        // 小数延迟——那一级全通会让湿路高频相位和整数延迟的干路对不齐，交叉淡化时高频凹陷（复核 V-09）。
        // v0.5 起用自写的块式半带过采样器（与 Color 的 Heat 共用）：系数由同一个 JUCE 设计函数按同样参数生成，
        // 线性相位、整数延迟不变，输出与 juce::dsp::Oversampling 差 < −130 dBFS，FIR 部分快 5～10 倍。
        clipOversampler = std::make_unique<HalfBandOversampler> ((size_t) numChannels);
        clipOversampler->clearOversamplingStages();
        clipOversampler->addOversamplingStage (0.05f, -90.0f, 0.06f, -75.0f);
        clipOversampler->addOversamplingStage (0.10f, -80.0f, 0.12f, -70.0f);
        clipOversampler->initProcessing ((size_t) maxBlockSize);

        const auto exactClipLatency = clipOversampler->getLatencyInSamples();
        jassert (exactClipLatency == std::floor (exactClipLatency));   // 必须是整数，否则干湿对不齐
        clipLatency = juce::roundToInt (exactClipLatency);
        clipper.setCurve (Saturator::Curve::SoftClip);

        // Clip 级空闲跳过的历史与预滚工作区（这里分配，音频线程上不分配）。
        clipPrerollLength = juce::jmax (minClipPrerollSamples, 4 * clipLatency);
        // 预滚要覆盖过采样器的全部记忆（FIR 状态只取决于最近这么多个输入样本），重开时才与一直在跑逐位相同。
        jassert (clipOversampler->getMemoryInSamples() <= clipPrerollLength);
        clipIdleHistory.setSize (numChannels, clipPrerollLength);
        clipPrerollScratch.setSize (numChannels, juce::jmin (maxBlockSize, clipPrerollLength));
        clipIdleSkippedSamples = 0;

        const auto maxLookahead = juce::roundToInt (maxLookaheadMs * 0.001 * sampleRate);
        const auto maxCoreDelay = maxLookahead + holdSamples - 1;

        clipDryDelays.resize ((size_t) numChannels);
        coreDelays.resize ((size_t) numChannels);
        bypassDryDelays.resize ((size_t) numChannels);

        for (auto& d : clipDryDelays)   { d.prepare (clipLatency);                d.setDelay (clipLatency); }
        for (auto& d : coreDelays)        d.prepare (maxCoreDelay);
        for (auto& d : bypassDryDelays)   d.prepare (clipLatency + maxCoreDelay);
        ceilingDelay.prepare (maxCoreDelay);
        gainDelay.prepare (clipLatency + maxCoreDelay);
        delayedGainScratch.assign ((size_t) maxBlockSize, 1.0f);

        clipDryScratch.setSize (numChannels, maxBlockSize);
        bypassDryScratch.setSize (numChannels, maxBlockSize);

        detector.prepare (numChannels);
        detectorPeaks.setSize (numChannels, maxBlockSize);
        requiredGainHold.prepare (maxLookahead + holdSamples);
        attackRamp.prepare (maxLookahead);
        attackShape.prepare (maxLookahead);
        transientMaxSamples = juce::jmax (1, juce::roundToInt (sampleRate * transientMaxSeconds));

        gainSmoothed.reset (sampleRate, smoothingSeconds);
        ceilingSmoothed.reset (sampleRate, smoothingSeconds);
        clipThresholdSmoothed.reset (sampleRate * (double) clipOversampler->getOversamplingFactor(), smoothingSeconds);
        listenGainSmoothed.reset (sampleRate, smoothingSeconds);
        clipMixSmoothed.reset (sampleRate, smoothingSeconds);
        bypassMixSmoothed.reset (sampleRate, smoothingSeconds);
        fadeInSmoothed.reset (sampleRate, fadeInSeconds);

        fastReleaseCoeff = timeConstantCoeff (sampleRate, fastReleaseMs);
        slowTargetCoeff  = timeConstantCoeff (sampleRate, slowTargetSmoothingMs);
        averageCoeff     = timeConstantCoeff (sampleRate / adaptIntervalSamples, averageWindowMs);
        lowFrequencyStates.assign ((size_t) numChannels, LowFrequencyState {});
        lowpassAlpha = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * lowFrequencyCutoffHz / sampleRate);
        lowFastAlpha = 1.0 - timeConstantCoeff (sampleRate, 20.0);
        lowSlowAlpha = 1.0 - timeConstantCoeff (sampleRate, 120.0);
        lowReleaseEnterAlpha = 1.0 - timeConstantCoeff (sampleRate / adaptIntervalSamples, 80.0);
        lowReleaseExitAlpha = 1.0 - timeConstantCoeff (sampleRate / adaptIntervalSamples, 40.0);

        lookaheadSamples = lookaheadSamplesFor (params.lookaheadMs);
        reset();
    }

    void LimiterModule::reset()
    {
        if (sampleRate <= 0.0)
            return;

        clipOversampler->reset();

        for (auto& d : clipDryDelays)
            d.clear();

        resetClipIdleState();
        detector.reset();
        resetLimiterState();

        // 平滑器直接落到当前目标，不从旧值滑过来。
        const auto gain = juce::Decibels::decibelsToGain (params.gainDb);
        const auto ceiling = juce::Decibels::decibelsToGain (params.ceilingDb);

        gainSmoothed.setCurrentAndTargetValue (gain);
        gainDelay.fill (gain);   // 「延迟后的 Gain」立即等于当前值（resetLimiterState 填的是平滑器落位之前的旧值）
        ceilingSmoothed.setCurrentAndTargetValue (ceiling);
        clipThresholdSmoothed.setCurrentAndTargetValue (
            juce::Decibels::decibelsToGain (juce::jmap (params.clip, 0.0f, 100.0f, clipOffThresholdDb, params.ceilingDb)));
        listenGainSmoothed.setCurrentAndTargetValue (params.listen ? 1.0f / gain : 1.0f);
        clipMixSmoothed.setCurrentAndTargetValue (params.clip > 0.0f ? 1.0f : 0.0f);
        bypassMixSmoothed.setCurrentAndTargetValue (params.bypass ? 1.0f : 0.0f);
        fadeInSmoothed.setCurrentAndTargetValue (1.0f);
        fadeInHoldSamples = 0;

        activeAutoRelease = params.autoRelease;
        updateReleaseCoefficients();
        maxGainReductionDb.store (0.0f);

        stats = {};
        blockStats = {};
    }

    void LimiterModule::resetClipIdleState() noexcept
    {
        // 过采样器刚清零 =「吃过无穷多个 0」的状态；历史也清零，就等于已经空闲了一个预滚长度（0 削波后还是 0）：
        // Clip = 0 时从第一块起就能跳过，重开时从这份历史预滚出来的状态仍与一直在跑的逐位相同。
        clipIdleHistory.clear();
        clipIdleHistoryWrite = 0;
        clipIdleSamples = clipPrerollLength;
        clipStageSkipping = false;
    }

    void LimiterModule::restartWithFadeIn (int extraHoldSamples)
    {
        reset();
        startFadeIn (extraHoldSamples);
    }

    void LimiterModule::startFadeIn (int extraHoldSamples)
    {
        // 延迟线刚清空，前 getLatencySamples() 个输出本来就是静音；之后再 10 ms 淡入。
        fadeInHoldSamples = getLatencySamples() + juce::jmax (0, extraHoldSamples);
        fadeInSmoothed.setCurrentAndTargetValue (0.0f);
        fadeInSmoothed.setTargetValue (1.0f);
    }

    void LimiterModule::resetLimiterState()
    {
        const auto coreDelay = coreDelaySamples();

        for (auto& d : coreDelays)       { d.setDelay (coreDelay); d.clear(); }
        for (auto& d : bypassDryDelays)  { d.setDelay (clipLatency + coreDelay); d.clear(); }
        ceilingDelay.setDelay (coreDelay);
        ceilingDelay.clear();
        gainDelay.setDelay (clipLatency + coreDelay);   // 与旁通干路同长：试听差值按它还原「加了 Gain 的输入」
        gainDelay.fill (gainSmoothed.getCurrentValue());

        requiredGainHold.setWindow (lookaheadSamples + holdSamples);
        requiredGainHold.clear();
        configureAttackShape();
        attackRamp.clear();
        attackShape.clear();

        envelopeManual = envelopeFast = envelopeSlow = slowTarget = lastEnvelope = 1.0;
        deepReductionSamples = 0;
        transientReleaseSamples = 0;
        averageReductionDb = 0.0;
        adaptCounter = 0;
        resetLowFrequencyRelease();
        lookaheadChangePending = false;
    }

    //==============================================================================
    void LimiterModule::setParams (const Params& newParams)
    {
        params = newParams;

        if (sampleRate <= 0.0)
            return;

        const auto gain = juce::Decibels::decibelsToGain (params.gainDb);

        gainSmoothed.setTargetValue (gain);
        ceilingSmoothed.setTargetValue (juce::Decibels::decibelsToGain (params.ceilingDb));

        // 削波阈值随 Clip 从 +12 dBTP（几乎不削）线性（按 dB）降到 Ceiling。
        clipThresholdSmoothed.setTargetValue (
            juce::Decibels::decibelsToGain (juce::jmap (params.clip, 0.0f, 100.0f, clipOffThresholdDb, params.ceilingDb)));

        // Clip = 0 时整条削波路淡出，走等长延迟的干路：这样才能精确 null（ADR-004）。
        clipMixSmoothed.setTargetValue (params.clip > 0.0f ? 1.0f : 0.0f);

        // Listen：减掉推进去的分贝，在等响度下对比。
        listenGainSmoothed.setTargetValue (params.listen ? 1.0f / gain : 1.0f);
        bypassMixSmoothed.setTargetValue (params.bypass ? 1.0f : 0.0f);

        manualReleaseCoeff = timeConstantCoeff (sampleRate, params.releaseMs);

        const auto newLookahead = lookaheadSamplesFor (params.lookaheadMs);

        if (newLookahead != lookaheadSamples)
        {
            lookaheadSamples = newLookahead;
            lookaheadChangePending = true;
        }
    }

    int LimiterModule::getLatencySamples() const noexcept
    {
        return clipLatency + coreDelaySamples();
    }

    float LimiterModule::getAndResetGainReductionDb() noexcept
    {
        return maxGainReductionDb.exchange (0.0f);
    }

    int LimiterModule::lookaheadSamplesFor (float milliseconds) const noexcept
    {
        const auto clamped = juce::jlimit (1.0f, maxLookaheadMs, milliseconds);
        return juce::jmax (1, juce::roundToInt (clamped * 0.001 * sampleRate));
    }

    void LimiterModule::updateReleaseCoefficients() noexcept
    {
        manualReleaseCoeff = timeConstantCoeff (sampleRate, params.releaseMs);
        slowReleaseCoeff   = timeConstantCoeff (sampleRate, slowReleaseMinMs);
    }

    void LimiterModule::applyLookaheadChange()
    {
        // 前瞻长度一变，延迟线、滑动最小值、移动平均的长度全都变了，只能整体重置。
        // 重置后前面一段输出是静音（延迟线是空的），再 10 ms 淡入，避免咔哒。
        resetLimiterState();
        startFadeIn (0);
    }

    //==============================================================================
    void LimiterModule::process (juce::AudioBuffer<float>& io, juce::AudioBuffer<float>* difference)
    {
        stats = {};

        if (difference != nullptr)
        {
            jassert (difference->getNumChannels() >= io.getNumChannels() && difference->getNumSamples() >= io.getNumSamples());

            // 本级不处理的声道（没 prepare 时是全部）原样通过：没有削掉任何东西，差值为 0。
            const auto processedChannels = sampleRate > 0.0 ? juce::jmin (numChannels, io.getNumChannels()) : 0;

            for (int ch = processedChannels; ch < io.getNumChannels(); ++ch)
                difference->clear (ch, 0, io.getNumSamples());
        }

        if (sampleRate <= 0.0 || io.getNumSamples() == 0)
        {
            blockStats = {};
            return;
        }

        if (lookaheadChangePending)
            applyLookaheadChange();

        if (params.autoRelease != activeAutoRelease)
        {
            // 切换释放模式时，新模式的状态从当前包络接着走，不跳变。
            activeAutoRelease = params.autoRelease;
            envelopeManual = envelopeFast = envelopeSlow = slowTarget = lastEnvelope;
            resetLowFrequencyRelease(); // 切回Auto从当前音频重新判断，不沿用旧低频证据
        }

        for (int start = 0; start < io.getNumSamples(); start += maxBlockSize)
            processChunk (io, start, juce::jmin (maxBlockSize, io.getNumSamples() - start), difference);

        finishBlockStats (io.getNumSamples());
    }

    void LimiterModule::finishBlockStats (int numSamples) noexcept
    {
        // GR 的 dB 域均值 = −(20 / ln 10) · Σ ln g / n，Σ ln g = ln(积) + 幂次 · ln 2：整块只取一次对数。
        // 全程 g = 1 时积恰为 1.0、幂次为 0，均值是精确的 +0。
        const auto logSum = std::log (stats.grProduct) + (double) stats.grExponent * naturalLogOf2;

        blockStats.grMeanDb = logSum < 0.0 ? (float) (-logSum * decibelsPerNeper / (double) numSamples) : 0.0f;
        blockStats.grMaxDb  = stats.minGain < 1.0f ? -juce::Decibels::gainToDecibels (stats.minGain, -120.0f) : 0.0f;

        blockStats.clipActiveFraction = stats.clipTotal > 0 ? (float) ((double) stats.clipCount / (double) stats.clipTotal) : 0.0f;
        blockStats.clipDepthMaxDb     = stats.clipPeakRatio > 1.0f ? (float) (20.0 * std::log10 ((double) stats.clipPeakRatio)) : 0.0f;
    }

    void LimiterModule::processChunk (juce::AudioBuffer<float>& io, int startSample, int numSamples, juce::AudioBuffer<float>* difference)
    {
        const auto numChannelsToUse = juce::jmin (numChannels, io.getNumChannels());
        auto* const* channels = io.getArrayOfWritePointers();

        // 1. 旁通用的干路：原始输入走「与整条处理链等长」的延迟。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            const auto* in = channels[ch] + startSample;
            auto* dry = bypassDryScratch.getWritePointer (ch);
            auto& delay = bypassDryDelays[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
                dry[i] = delay.pushPop (in[i]);
        }

        // 2. Gain（两个声道用同一个平滑后的增益）。每个样本的 Gain 同时送进与干路等长的延迟线（试听差值用，只读不改音频）。
        for (int i = 0; i < numSamples; ++i)
        {
            const auto gain = gainSmoothed.getNextValue();
            delayedGainScratch[(size_t) i] = gainDelay.pushPop (gain);

            for (int ch = 0; ch < numChannelsToUse; ++ch)
                channels[ch][startSample + i] *= gain;
        }

        // 3. Clip 级。
        processClipStage (io, startSample, numSamples, numChannelsToUse);

        // 4. 前瞻限幅 + 硬钳位。
        processLimiterCore (io, startSample, numSamples, numChannelsToUse);

        // 5. Listen、Lookahead 变动后的淡入、旁通交叉淡化；要试听差值时同一个循环里顺带写出差值（不改输出）。
        auto* const* differenceChannels = difference != nullptr ? difference->getArrayOfWritePointers() : nullptr;
        const auto differenceCeiling = ceilingSmoothed.getCurrentValue();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto listenGain = listenGainSmoothed.getNextValue();

            float fade = 1.0f;
            if (fadeInHoldSamples > 0)
            {
                --fadeInHoldSamples;
                fade = 0.0f;
            }
            else
            {
                fade = fadeInSmoothed.getNextValue();
            }

            const auto bypassMix = bypassMixSmoothed.getNextValue();

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& out = channels[ch][startSample + i];
                const auto dry = bypassDryScratch.getReadPointer (ch)[i];

                if (differenceChannels != nullptr)
                {
                    // 干路 × 同样延迟过的 Gain = 当初送进 Clip 级的样本（逐位相同）；减去湿路、除以 Gain 回到推响前的电平。
                    // 旁通时本级没有削掉任何东西、淡入保持段里没有输出：差值跟着交叉淡化与淡入归零。
                    const auto gain = delayedGainScratch[(size_t) i];
                    // Match the separately rounded multiply used before Clip. A fused multiply-subtract
                    // keeps sub-ULP product bits and would create a residual even when out is identical.
                    const float gainedDry = dry * gain;
                    const auto removed = gain > minimumDifferenceGain ? (gainedDry - out) / gain : 0.0f;
                    differenceChannels[ch][startSample + i] = juce::jlimit (-differenceCeiling, differenceCeiling, removed)
                                                              * (1.0f - bypassMix) * fade;
                }

                const auto processed = out * listenGain;

                float mixed;
                if (bypassMix <= 0.0f)       mixed = processed;
                else if (bypassMix >= 1.0f)  mixed = dry;   // 完全旁通：逐位等于延迟后的输入
                else                         mixed = processed + (dry - processed) * bypassMix;

                out = mixed * fade;
            }
        }
    }

    void LimiterModule::processClipStage (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        auto* const* channels = io.getArrayOfWritePointers();

        // Clip 级空闲 = Clip 为 0、干湿淡化已走完、削波阈值也不在滑动：这时湿路输出被整段丢弃（逐位取干路）。
        // 这三个平滑器在块内不会变（setParams 只在块与块之间调），块开头判断一次对整块成立。
        // 只在所有声道都在时才算空闲：跳过期间每个声道的过采样器都「本该」在跑，重开时才能全部预滚回来。
        // 逐位相同的前提是每块声道数 = prepare 时的声道数（处理器一直如此）。少声道的块本来就特殊：JUCE 的多级
        // 过采样器在这种块里会让内层级拿上一块残留的数据推进缺席声道的状态——这里只保证它照样安全。
        const auto idle = clipIdleSkipEnabled && numChannelsToUse == numChannels
                       && ! clipMixSmoothed.isSmoothing() && clipMixSmoothed.getCurrentValue() <= 0.0f
                       && ! clipThresholdSmoothed.isSmoothing();

        // v0.6 统计：本段在 4× 域的样本总数全部计入分母（空闲跳过、湿路被丢弃的部分按「没在削」计）；
        // 只有湿路真正进输出（Clip 开着，或开 / 关的 20 ms 淡化中）时才数被削样本——跳过与不跳过统计相同。
        stats.clipTotal += (juce::int64) numSamples * (juce::int64) clipOversampler->getOversamplingFactor()
                             * (juce::int64) numChannelsToUse;
        const auto wetReachesOutput = clipMixSmoothed.isSmoothing() || clipMixSmoothed.getCurrentValue() > 0.0f;

        if (idle)
            recordClipIdleHistory (channels, startSample, numSamples);

        // 干路：等长（= 过采样滤波器延迟）延迟，Clip = 0 时就走它。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
        {
            const auto* in = channels[ch] + startSample;
            auto* dry = clipDryScratch.getWritePointer (ch);
            auto& delay = clipDryDelays[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
                dry[i] = delay.pushPop (in[i]);
        }

        if (idle)
        {
            if (clipIdleSamples >= clipPrerollLength)
            {
                // 已经连续空闲了至少一个预滚长度：过采样器的状态随时能由历史重建，本块只走干路、不跑过采样。
                if (! clipStageSkipping)
                {
                    clipStageSkipping = true;
                    clipIdleThreshold = clipThresholdSmoothed.getCurrentValue();   // 空闲期间恒定（没在滑动）
                }

                for (int ch = 0; ch < numChannelsToUse; ++ch)
                    juce::FloatVectorOperations::copy (channels[ch] + startSample, clipDryScratch.getReadPointer (ch), numSamples);

                clipIdleSkippedSamples += numSamples;
                return;
            }

            // 刚进入空闲、还不够一个预滚长度：照常跑，同时攒历史。
            clipIdleSamples = juce::jmin (clipPrerollLength, clipIdleSamples + numSamples);
        }
        else
        {
            clipIdleSamples = 0;

            // 从跳过回到工作：先把跳过期间「本该」跑过的最近一段输入预滚进过采样器，状态就与一直在跑的逐位相同。
            if (clipStageSkipping)
            {
                prerollClipOversampler();
                clipStageSkipping = false;
            }
        }

        // 湿路：Clip 从 0 打开时滤波器里已经是连续的历史（一直在跑，或刚预滚过），不会爆音。
        juce::dsp::AudioBlock<float> block (io);
        auto subBlock = block.getSubsetChannelBlock (0, (size_t) numChannelsToUse)
                             .getSubBlock ((size_t) startSample, (size_t) numSamples);

        runClipOversampled (subBlock, true, wetReachesOutput);

        // 干湿交叉淡化（只在 Clip 开 / 关的 20 ms 里真正混合）。
        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = clipMixSmoothed.getNextValue();

            if (mix >= 1.0f)
                continue;   // 全湿：subBlock 里已经是削波后的信号

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& sample = channels[ch][startSample + i];
                const auto dry = clipDryScratch.getReadPointer (ch)[i];
                sample = (mix <= 0.0f) ? dry : dry + (sample - dry) * mix;
            }
        }
    }

    void LimiterModule::runClipOversampled (juce::dsp::AudioBlock<float>& block, bool followSmoothedThreshold, bool collectStats) noexcept
    {
        auto upsampled = clipOversampler->processSamplesUp (block);

        if (collectStats && followSmoothedThreshold && clipThresholdSmoothed.isSmoothing())
        {
            // 阈值在滑（Clip 刚拧动的 20 ms）：统计与削波在同一个循环里、对同一个逐样本阈值。平滑器与削波器的调用
            // 顺序、每个样本的运算都与下面的原循环相同，只多读一次削波前的样本，输出逐位相同。
            for (size_t i = 0; i < upsampled.getNumSamples(); ++i)
            {
                const auto threshold = clipThresholdSmoothed.getNextValue();
                clipper.setThreshold (threshold);

                for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
                {
                    auto* data = upsampled.getChannelPointer (ch);
                    const auto magnitude = std::abs (data[i]);

                    if (magnitude > threshold)   // NaN 比较为假，不计
                    {
                        ++stats.clipCount;
                        stats.clipPeakRatio = juce::jmax (stats.clipPeakRatio, magnitude / threshold);
                    }

                    data[i] = clipper.processSample (data[i]);
                }
            }
        }
        else
        {
            // 阈值不变（绝大多数时候，每个样本拿到的就是目标值）：削波前先整段只读扫一遍，再照原样削波。
            if (collectStats)
                accumulateClipStats (upsampled, clipThresholdSmoothed.getTargetValue());

            for (size_t i = 0; i < upsampled.getNumSamples(); ++i)
            {
                if (followSmoothedThreshold)
                    clipper.setThreshold (clipThresholdSmoothed.getNextValue());

                for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
                {
                    auto* data = upsampled.getChannelPointer (ch);
                    data[i] = clipper.processSample (data[i]);
                }
            }
        }

        clipOversampler->processSamplesDown (block);
    }

    void LimiterModule::accumulateClipStats (const juce::dsp::AudioBlock<float>& upsampled, float threshold) noexcept
    {
        // 只读：数 |x| > T 的样本、取 max |x|（NaN 两样都不计）。T 在这一段里不变，比值只在最后算一次。
        const auto numUpsampled = (int) upsampled.getNumSamples();
        float peak = 0.0f;
        juce::int64 count = 0;

        for (size_t ch = 0; ch < upsampled.getNumChannels(); ++ch)
        {
            const auto* data = upsampled.getChannelPointer (ch);
            int i = 0;

           #if CELADON_LIMITER_STATS_NEON
            // 4 组独立的累加器：取大与计数都有跨迭代依赖，一组的话每 4 个样本要等一次取大的延迟。
            // 取大、计数与顺序无关，分几组算、最后怎么合并，结果都逐位相同。
            const auto limit = vdupq_n_f32 (threshold);
            float32x4_t peaks[4] = { vdupq_n_f32 (0.0f), vdupq_n_f32 (0.0f), vdupq_n_f32 (0.0f), vdupq_n_f32 (0.0f) };
            uint32x4_t counts[4] = { vdupq_n_u32 (0), vdupq_n_u32 (0), vdupq_n_u32 (0), vdupq_n_u32 (0) };

            for (; i + 16 <= numUpsampled; i += 16)
            {
                for (int k = 0; k < 4; ++k)
                {
                    const auto magnitude = vabsq_f32 (vld1q_f32 (data + i + 4 * k));
                    peaks[k]  = vmaxnmq_f32 (peaks[k], magnitude);                    // maxnm：一边是 NaN 时取另一边
                    counts[k] = vsubq_u32 (counts[k], vcgtq_f32 (magnitude, limit));  // 比较为真 = 全 1（即 −1），减去 = 加 1
                }
            }

            for (; i + 4 <= numUpsampled; i += 4)
            {
                const auto magnitude = vabsq_f32 (vld1q_f32 (data + i));
                peaks[0]  = vmaxnmq_f32 (peaks[0], magnitude);
                counts[0] = vsubq_u32 (counts[0], vcgtq_f32 (magnitude, limit));
            }

            const auto peaksAll  = vmaxnmq_f32 (vmaxnmq_f32 (peaks[0], peaks[1]), vmaxnmq_f32 (peaks[2], peaks[3]));
            const auto countsAll = vaddq_u32 (vaddq_u32 (counts[0], counts[1]), vaddq_u32 (counts[2], counts[3]));
            peak = juce::jmax (peak, vmaxnmvq_f32 (peaksAll));
            count += (juce::int64) vaddvq_u32 (countsAll);
           #elif CELADON_LIMITER_STATS_SSE2
            const auto limit = _mm_set1_ps (threshold);
            const auto absMask = _mm_castsi128_ps (_mm_set1_epi32 (0x7fffffff));
            auto peaks = _mm_setzero_ps();
            auto counts = _mm_setzero_si128();

            for (; i + 4 <= numUpsampled; i += 4)
            {
                const auto magnitude = _mm_and_ps (_mm_loadu_ps (data + i), absMask);
                peaks  = _mm_max_ps (magnitude, peaks);   // maxps 在任一边是 NaN 时取第二个操作数：NaN 不进峰值
                counts = _mm_sub_epi32 (counts, _mm_castps_si128 (_mm_cmpgt_ps (magnitude, limit)));
            }

            alignas (16) float peakLanes[4];
            alignas (16) std::int32_t countLanes[4];
            _mm_store_ps (peakLanes, peaks);
            _mm_store_si128 (reinterpret_cast<__m128i*> (countLanes), counts);

            for (int lane = 0; lane < 4; ++lane)
            {
                peak = juce::jmax (peak, peakLanes[lane]);
                count += (juce::int64) (std::uint32_t) countLanes[lane];
            }
           #endif

            for (; i < numUpsampled; ++i)
            {
                const auto magnitude = std::abs (data[i]);

                if (magnitude > peak)       peak = magnitude;
                if (magnitude > threshold)  ++count;
            }
        }

        stats.clipCount += count;

        if (peak > threshold)
            stats.clipPeakRatio = juce::jmax (stats.clipPeakRatio, peak / threshold);
    }

    void LimiterModule::recordClipIdleHistory (const float* const* channels, int startSample, int numSamples) noexcept
    {
        // 只有最后 clipPrerollLength 个样本留得下来，更早的直接跳过。
        const auto length = clipPrerollLength;
        const auto first = juce::jmax (0, numSamples - length);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* in = channels[ch] + startSample;
            auto* ring = clipIdleHistory.getWritePointer (ch);
            auto write = (clipIdleHistoryWrite + first) % length;

            for (int i = first; i < numSamples; ++i)
            {
                ring[write] = in[i];

                if (++write == length)
                    write = 0;
            }
        }

        clipIdleHistoryWrite = (clipIdleHistoryWrite + numSamples) % length;
    }

    void LimiterModule::prerollClipOversampler() noexcept
    {
        // 过采样器清零，再把最近 clipPrerollLength 个 Clip 级输入按原顺序送一遍：上采样 → 削波（空闲期间的恒定阈值，
        // 与一直在跑时用的是同一个值）→ 下采样，输出丢掉。按 maxBlockSize 分段，与正常处理的块一样大。
        clipOversampler->reset();
        clipper.setThreshold (clipIdleThreshold);

        const auto length = clipPrerollLength;
        const auto segment = clipPrerollScratch.getNumSamples();

        for (int done = 0; done < length; done += segment)
        {
            const auto n = juce::jmin (segment, length - done);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* ring = clipIdleHistory.getReadPointer (ch);
                auto* dest = clipPrerollScratch.getWritePointer (ch);
                auto read = (clipIdleHistoryWrite + done) % length;   // clipIdleHistoryWrite 是最旧的样本

                for (int i = 0; i < n; ++i)
                {
                    dest[i] = ring[read];

                    if (++read == length)
                        read = 0;
                }
            }

            juce::dsp::AudioBlock<float> block (clipPrerollScratch);
            auto subBlock = block.getSubsetChannelBlock (0, (size_t) numChannels).getSubBlock (0, (size_t) n);
            runClipOversampled (subBlock, false, false);   // 预滚的输出丢掉，不进统计
        }
    }

    void LimiterModule::processLimiterCore (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse)
    {
        auto* const* channels = io.getArrayOfWritePointers();

        // 1) 真峰值检测整段先算完（按块，与逐样本逐位相同）：检测器只读本段的输入，
        //    下面的逐样本循环才原地改写样本，所以与「边检测边改写」读到的是同一批值。
        for (int ch = 0; ch < numChannelsToUse; ++ch)
            detector.processBlock (ch, channels[ch] + startSample, detectorPeaks.getWritePointer (ch), numSamples);

        const auto* const* peaks = detectorPeaks.getArrayOfReadPointers();
        float minGain = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto ceiling = ceilingSmoothed.getNextValue();
            if (activeAutoRelease && lowFrequencyReleaseEnabled)
                observeLowFrequency (channels, startSample + i, numChannelsToUse);

            // 左右联动取最大 → 需求增益。
            float peak = 0.0f;
            for (int ch = 0; ch < numChannelsToUse; ++ch)
                peak = juce::jmax (peak, peaks[ch][i]);

            const auto detectorCeiling = ceiling * detectorSafetyGain;
            const auto required = peak > detectorCeiling ? detectorCeiling / peak : 1.0f;

            // 2) 前瞻：最近 Lookahead + 保持段 内的最小需求。
            const auto held = requiredGainHold.push (required);

            // 3) 释放包络（向下瞬时、向上指数回升）。
            const auto envelope = nextReleaseEnvelope (held);

            // 4) 攻击整形：两级滑动平均的总建立时间仍等于 Lookahead，峰值到达那一刻降到位；拐角是三角窗。
            const auto gain = attackShape.push (attackRamp.push (envelope));
            minGain = juce::jmin (minGain, gain);

            // v0.6 统计（只读 gain）：GR 的 dB 域均值靠增益连乘，整块只取一次对数；g = 1 不乘（乘了也不变）。
            if (gain < 1.0f)
            {
                stats.grProduct *= (double) (gain > statsMinGain ? gain : statsMinGain);

                if (stats.grProduct < statsRenormalizeBelow)
                {
                    int exponent = 0;
                    stats.grProduct = std::frexp (stats.grProduct, &exponent);
                    stats.grExponent += exponent;
                }
            }

            // 5) 音频走等长延迟后乘增益；6) 硬钳位保险（理论上不触发）。
            const auto ceilingAtOutput = ceilingDelay.pushPop (ceiling);

            for (int ch = 0; ch < numChannelsToUse; ++ch)
            {
                auto& sample = channels[ch][startSample + i];
                const auto delayed = coreDelays[(size_t) ch].pushPop (sample);
                sample = juce::jlimit (-ceilingAtOutput, ceilingAtOutput, delayed * gain);
            }
        }

        // 旁通时限幅没有作用到输出上（输出是干路），表不报数（2026-09-24 夜间评审 R1-03）。
        if (minGain < 1.0f && ! params.bypass)
            atomicMax (maxGainReductionDb, -juce::Decibels::gainToDecibels (minGain, -120.0f));

        stats.minGain = juce::jmin (stats.minGain, minGain);   // 统计照报内部处理量（旁通与否，见 BlockStats）
    }

    void LimiterModule::resetLowFrequencyRelease() noexcept
    {
        for (auto& state : lowFrequencyStates) state = LowFrequencyState {};
        lowEnergyFast = lowEnergySlow = fullEnergyFast = lowPeriodSamples = 0.0;
        lowStableSamples = 0;
        lowFrequencyReleaseMs = fastReleaseMs;
        fastReleaseCoeff = timeConstantCoeff (sampleRate, fastReleaseMs);
    }

    void LimiterModule::observeLowFrequency (const float* const* channels, int index, int channelsToUse) noexcept
    {
        double lowEnergy = 0.0, fullEnergy = 0.0, largestLow = 0.0;
        lowPeriodSamples = 0.0;
        const auto maxPeriod = (int) (sampleRate / lowFrequencyMinHz);
        for (int ch = 0; ch < channelsToUse; ++ch)
        {
            auto& state = lowFrequencyStates[(size_t) ch];
            const auto sample = (double) channels[ch][index];
            const auto x = std::isfinite (sample) ? sample : 0.0; // 观察器不保留异常状态，音频保护仍由整链负责
            state.low1 += lowpassAlpha * (x - state.low1);
            state.low2 += lowpassAlpha * (state.low1 - state.low2);
            state.crossingAge = juce::jmin (state.crossingAge + 1, maxPeriod + 1);
            if (state.previous <= 0.0 && state.low2 > 0.0)
            {
                const auto period = (double) state.crossingAge;
                if (period >= sampleRate / lowFrequencyCutoffHz && period <= maxPeriod)
                {
                    // 能量稳定不代表有稳定周期：低频噪声和快速换音也可能持续很响。
                    // 三个相邻周期都在5%内，且不偏离慢参考8%，才交给后面的120ms持续门。
                    // 失配立即清证据并以新周期重新开始；不会把旧音高一路平滑成可信的新音高。
                    const auto consistent = state.previousPeriodSamples > 0.0
                        && std::abs (period - state.previousPeriodSamples) <= 0.05 * state.previousPeriodSamples
                        && std::abs (period - state.periodSamples) <= 0.08 * state.periodSamples;
                    state.consistentPeriods = consistent ? juce::jmin (lowConsistentPeriods, state.consistentPeriods + 1) : 0;
                    state.periodSamples = consistent ? state.periodSamples + 0.2 * (period - state.periodSamples) : period;
                    state.previousPeriodSamples = period;
                }
                else
                {
                    state.periodSamples = 0.0;
                    state.previousPeriodSamples = 0.0;
                    state.consistentPeriods = 0;
                }
                state.crossingAge = 0;
            }
            if (state.crossingAge > maxPeriod)
            {
                state.periodSamples = state.previousPeriodSamples = 0.0;
                state.consistentPeriods = 0;
            }
            state.previous = state.low2;
            const auto energy = state.low2 * state.low2;
            lowEnergy += energy;
            fullEnergy += x * x;
            // 按当前低频更强的一侧取周期；不做L+R合并，纯Side低音也有同样保护。
            if (energy > largestLow && state.periodSamples > 0.0 && state.consistentPeriods >= lowConsistentPeriods)
            {
                largestLow = energy;
                lowPeriodSamples = state.periodSamples;
            }
        }
        lowEnergyFast += lowFastAlpha * (lowEnergy - lowEnergyFast);
        lowEnergySlow += lowSlowAlpha * (lowEnergy - lowEnergySlow);
        fullEnergyFast += lowFastAlpha * (fullEnergy - fullEnergyFast);
    }

    void LimiterModule::updateLowFrequencyRelease() noexcept
    {
        // 静音尾巴不进入double denormal；更新节拍固定，不依赖宿主块长。
        for (auto& state : lowFrequencyStates)
        {
            if (std::abs (state.low1) < 1.0e-30) state.low1 = 0.0;
            if (std::abs (state.low2) < 1.0e-30) state.low2 = 0.0;
        }
        if (lowEnergyFast < 1.0e-30) lowEnergyFast = 0.0;
        if (lowEnergySlow < 1.0e-30) lowEnergySlow = 0.0;
        if (fullEnergyFast < 1.0e-30) fullEnergyFast = 0.0;
        // 持续且稳定的低频才介入：衰减鼓击的20ms能量会明显低于120ms平均，不能累满持续门。
        const auto stable = lowFrequencyReleaseEnabled && lowPeriodSamples > 0.0 && lowEnergyFast > 1.0e-8
                         && lowEnergyFast >= 0.35 * fullEnergyFast
                         && lowEnergyFast >= 0.65 * lowEnergySlow && lowEnergyFast <= 1.5 * lowEnergySlow;
        const auto needed = juce::jmax (1, juce::roundToInt (sampleRate * lowStableSeconds));
        lowStableSamples = stable ? juce::jmin (needed, lowStableSamples + adaptIntervalSamples)
                                  : juce::jmax (0, lowStableSamples - 2 * adaptIntervalSamples);
        const auto targetMs = lowStableSamples >= needed
                            ? juce::jlimit ((double) fastReleaseMs, lowReleaseMaxMs, 4000.0 * lowPeriodSamples / sampleRate)
                            : (double) fastReleaseMs;
        if (targetMs == lowFrequencyReleaseMs) return;
        const auto alpha = targetMs > lowFrequencyReleaseMs ? lowReleaseEnterAlpha : lowReleaseExitAlpha;
        lowFrequencyReleaseMs += alpha * (targetMs - lowFrequencyReleaseMs);
        if (targetMs == fastReleaseMs && lowFrequencyReleaseMs - fastReleaseMs < 0.001)
            lowFrequencyReleaseMs = fastReleaseMs;
        fastReleaseCoeff = timeConstantCoeff (sampleRate, lowFrequencyReleaseMs);
    }

    void LimiterModule::configureAttackShape() noexcept
    {
        // 两级长度相加再减 1 等于 Lookahead：阶跃响应与原来的单级矩形窗在同一拍到达终点。
        // Lookahead = 1 时两级都是 1，推动一次就是恒等。
        const auto total = juce::jmax (1, lookaheadSamples);
        const auto shape = juce::jmax (1, total / 2);
        const auto ramp = juce::jmax (1, total - shape + 1);
        attackRamp.setLength (ramp);
        attackShape.setLength (shape);
    }

    float LimiterModule::nextReleaseEnvelope (float target) noexcept
    {
        if (! activeAutoRelease)
        {
            envelopeManual = followWithRelease (envelopeManual, target, manualReleaseCoeff);
            lastEnvelope = envelopeManual;
            return (float) lastEnvelope;
        }

        // 快段：基准30ms；持续、周期稳定的低频存在时，控制路温和延长，孤立鼓击仍用基准。
        envelopeFast = followWithRelease (envelopeFast, target, fastReleaseCoeff);

        // 慢段：跟的是「需求经 50 ms 平滑」后的版本，单个瞬态几乎拉不动它；
        // 持续响的段落它才会沉下去，并按最近的平均衰减自适应放慢释放，减少泵感。
        slowTarget = settle (target + (slowTarget - target) * slowTargetCoeff, target);
        envelopeSlow = followWithRelease (envelopeSlow, slowTarget, slowReleaseCoeff);

        // 持续过载会把 deepReductionSamples 顶满并取消瞬态松开。短过载一结束，慢包络在半个窗口内不允许低于快包络，
        // 鼓点后面的声音按快释放回来；慢包络仍留给真正持续的响段。
        if (target < transientReturnGain)
        {
            deepReductionSamples = juce::jmin (deepReductionSamples + 1, transientMaxSamples + 1);

            if (deepReductionSamples >= transientMaxSamples)
                transientReleaseSamples = 0;
        }
        else
        {
            if (deepReductionSamples > 0 && deepReductionSamples < transientMaxSamples)
                transientReleaseSamples = juce::jmax (transientReleaseSamples, transientMaxSamples / 2);

            deepReductionSamples = 0;
        }

        if (transientReleaseSamples > 0)
        {
            --transientReleaseSamples;
            envelopeSlow = juce::jmax (envelopeSlow, envelopeFast);
            slowTarget = juce::jmax (slowTarget, (double) envelopeFast);
        }

        lastEnvelope = juce::jmin (envelopeFast, envelopeSlow);

        if (++adaptCounter >= adaptIntervalSamples)
        {
            adaptCounter = 0;
            updateLowFrequencyRelease();

            const auto reductionDb = -juce::Decibels::gainToDecibels (lastEnvelope, -120.0);
            averageReductionDb = reductionDb + (averageReductionDb - reductionDb) * averageCoeff;

            const auto depth = juce::jlimit (0.0, 1.0, averageReductionDb / (double) deepReductionDb);
            slowReleaseCoeff = timeConstantCoeff (sampleRate, juce::jmap (depth, (double) slowReleaseMinMs, (double) slowReleaseMaxMs));
        }

        return (float) lastEnvelope;
    }
}
