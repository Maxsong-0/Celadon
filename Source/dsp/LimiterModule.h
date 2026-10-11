/*
  ==============================================================================

    LimiterModule.h
    Limit 模块：前瞻真峰值限幅器 + 可选 Clip 级（设计规格-Limit、ADR-004）。

    信号流（每块）：

      输入 ─┬─ × Gain → Clip 级（自带 4× 过采样，Clip = 0 时淡出成干路）
            │         → 前瞻限幅（8× 真峰值检测，1× 施加增益）→ 硬钳位 → Listen
            │                                                         │
            └──────── 等长延迟的干路 ─────────── Bypass 交叉淡化 ←──────┘

    性能（v0.5）：
      - 真峰值检测按块算（TruePeakDetector::processBlock，与逐样本逐位相同）；
      - Clip 级空闲跳过：Clip = 0 且淡出走完后，湿路输出本来就被整段丢弃，这时不跑 4× 过采样，只走干路；
        空闲期间记下最近的 Clip 级输入，Clip 重新打开的那一块先把它们「预滚」进清空的过采样器——
        两级半带都是有限记忆的 FIR、削波没有记忆，预滚够长，状态就与一直在跑的逐位相同，输出也逐位相同。

    真峰值重建余量：需求增益从 Ceiling − 0.35 dB 开始限制，补偿有限长度检测器在独立宽带测试中的重建差异。
    这是经验余量，不保证任意波形经任意重建器都绝不超限；用户 Ceiling、Clip 阈值和输出硬钳位保持原值。
    Gain = 0、Clip = 0 时，低于内部阈值且释放已恢复的信号仍精确直通（除延迟）。

    Auto 释放保留快/慢双包络，另在低频持续、连续周期一致时给快释放设置周期下限，减少低音调制高频。
    观察器只在1×控制路运行，不增加音频延迟；孤立衰减鼓击不满足持续门，手动Release不受影响。

    攻击整形仍在原来的前瞻长度内走完，所以峰值那一刻的增益与原来的矩形滑动平均同时到位；
    矩形平均换成两级串联（三角窗），拐角连续，稳态增益不变。短于约 70 ms 的过载结束后，慢包络跟着快包络松开，
    不把鼓点后面的声音按住；持续过载仍由慢包络压住泵感。手动 Release 不走这条瞬态分支。

    试听差值（v0.1.8.0，界面 LIMIT 卡片的耳机）：process 可顺带写出「本级削掉的部分」——
      差值 = (加了 Gain 的输入 − 本级湿路输出) ÷ Gain，按推响前的电平（Gain、Clip、限幅、硬钳位一起算）。
      「加了 Gain 的输入」由等长干路（旁通用的那条，= 输入延迟整个本级延迟）乘同样延迟过的逐样本 Gain 重建，
      与当初送进 Clip 级的样本逐位相同：没有衰减、Clip = 0 时差值恰为 0。纯增益衰减时差值 = 输入 ×（1 − 衰减倍数），
      不会比输入响；再按此刻的 Ceiling 硬钳位兜底。旁通、淡入保持段里差值跟着交叉淡化 / 淡入归零。
      只多一条 Gain 延迟线（一直在跑，打开时不必预热）；不传差值缓冲时输出、状态、统计与原来逐位相同。

    渲染计数（T-096 (a)，插件不调用）：BlockStats 末三项与 getAndResetRenderCountersForTesting——硬钳位改了值的样本数（一直在数）、
      限幅差值能量与入口能量（只在渲染工具打开时累计）。都只读已有的量：输出、状态与原有统计逐位不变。

    只依赖 juce_dsp，可在 Linux 上无头编译做自动测试。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "DelayLine.h"
#include "HalfBandOversampler.h"
#include "Saturator.h"
#include "TruePeak.h"

#include <atomic>
#include <memory>
#include <vector>

namespace celadon
{
    class LimiterModule
    {
    public:
        // setParams 不信任外部值（T-032 R8-02，2026-09-29）：APVTS 与链只钳有限值，工程文件里的 "nan" 会原样读回——
        // 非有限（NaN / ±Inf）的字段回下面的默认值，有限值钳进注释里的范围（范围内的值原样保留、逐位不变）。
        struct Params
        {
            float gainDb      = 0.0f;     // 0 ～ +24
            float ceilingDb   = -1.0f;    // 参数 −3 ～ 0 dBTP；链扣掉编码余量（≤ 1.5 dB）后最低交 −4.5，模块按 −4.5 ～ 0 钳
            float releaseMs   = 100.0f;   // 10 ～ 1000，Auto 开时无效
            bool  autoRelease = true;
            float lookaheadMs = 2.0f;     // 1 ～ 10，决定延迟（这里只拦非有限值；范围与取整由 lookaheadSamplesFor 负责）
            float clip        = 0.0f;     // 0 ～ 100
            bool  listen      = false;
            bool  bypass      = false;
            // T-038 限幅跟拍（设计规格-曲风-House学习方式 第 8 节）：Auto 释放的慢段释放时间上限（ms）。0 或非有限 = 不封顶（逐位同现在）；
            // 正值时慢段释放取 min(自适应值, 本项)，下限仍是 slowReleaseMinMs。链只在 House 档、Auto 释放、宿主 BPM 合理时给正值。
            // 不是宿主参数；手动 Release 与短过载快释放分支都不看它。
            float beatReleaseCapMs = 0.0f;
        };

        // 峰值附近增益保持平坦的样本数：覆盖真峰值插值器的全部抽头（ADR-004）。
        static constexpr int holdSamples = TruePeakDetector::tapsPerPhase - 1;

        LimiterModule() = default;

        void prepare (const juce::dsp::ProcessSpec& spec);

        // 清空全部状态，下一个样本起立即出声（prepare 时用）。
        void reset();

        // 清空全部状态后先静音一个延迟长度再 10 ms 淡入：用于宿主 Reset、NaN 恢复等
        // 播放中途的重置，避免延迟线清空后信号从中间硬切回来的咔哒。
        // extraHoldSamples：前级模块（如 Color）同时被重置时，把它的延迟也算进静音段。
        void restartWithFadeIn (int extraHoldSamples = 0);

        // 音频线程每块调一次。Lookahead 变了会在下一次 process() 开头重置限幅器状态。
        // 非有限的字段回默认值、越界的有限值钳进范围（见 Params）；无分配、无锁。
        void setParams (const Params& newParams);

        // 原地处理；样本数可以超过 prepare 时的块大小（内部会切块）。
        // difference 非空时同时写出试听差值（见文件头）：声道数、样本数都不少于 io 的；本级没处理的声道写 0。
        // 传不传 difference，io 的输出、内部状态与统计都逐位相同。
        void process (juce::AudioBuffer<float>& io, juce::AudioBuffer<float>* difference = nullptr);

        // 当前设置下的总延迟（样本）。setParams 之后立即反映新的 Lookahead。
        int getLatencySamples() const noexcept;

        // 自上次读取以来的最大增益衰减（dB，正数），读完清零。界面线程调用，无锁。
        float getAndResetGainReductionDb() noexcept;

        // 上一次 process() 这一块的统计（v0.6 GLAZE 保护层与约束核对用；设计规格-GLAZE-分析器第 1 节「模块内部」测点）。
        // 只在调用 process 的线程上读。空闲跳过的部分按「没在工作」计（Clip 跳过时两项 Clip 统计为 0）。
        // 统计只读处理过程中已有的量，不改音频路径：输出、GR 表读数、空闲跳过行为与没有统计时逐位相同。
        // 描述的是本级自己的处理量，与 Bypass / Listen / 淡入无关（旁通时内部照常限幅，这里照报；表不报数，见 processLimiterCore）；
        // 旁通时是否据此退让由 GLAZE 编排器决定。一次 process 的样本数超过 prepare 的块大小时，统计覆盖整个调用（各内部分段合并）。
        struct BlockStats
        {
            float grMeanDb = 0.0f;             // 限幅增益衰减按样本在 dB 域的均值（正数）= −20·log10(本块增益的几何平均)；每样本下限 −120 dB（同表）
            float grMaxDb = 0.0f;              // 本块最大增益衰减（正数），与同一块 GR 表读数同一公式
            float clipActiveFraction = 0.0f;   // Clip 级 4× 域里被削样本的占比（0 ～ 1）：|x| > 削波阈值 T 的样本数（各声道合计）/ 本块 4× 样本总数；
                                               // T = 软削波曲线的封顶阈值（0.7·T 起软拐、渐近封顶在 T）；湿路被整段丢弃（Clip 0 淡出走完）时不计
            float clipDepthMaxDb = 0.0f;       // 本块最大削波深度：超出削波门限的量 max 20·log10(|x| / T)（dB，正数；没有超出时为 0）

            // ---- T-096 (a) 渲染工具的只读计数（ADR-034 A2 / B6；插件不读）----
            juce::int64 hardClipSamples = 0;   // 输出硬钳位真的改了值的样本数（各声道合计）：|延迟后 × 增益| > 此刻的 Ceiling。一直在数，理论上恒为 0
            double limitDiffEnergy = 0.0;      // Σ(加了 Gain 的输入 − 本级湿路输出)²（各声道合计）：与试听差值同一个差，只是按推响后的电平、
                                               // 不除以 Gain、不钳位。只在 setRenderCountersEnabledForTesting (true) 时累计，否则为 0
            double limitInputEnergy = 0.0;     // Σ(加了 Gain 的输入)²（各声道合计；同上，只在打开时累计）
        };

        const BlockStats& getBlockStats() const noexcept   { return blockStats; }

        // T-096 (a)：自上次读取以来各次 process 的上面三项之和，读完清零。为什么另给一份：链在 GLAZE 活跃时把宿主块按 10 ms 时隙切片，
        // getBlockStats() 只剩最后一片；渲染工具（celadon_render --trace）要按宿主块读整块的量。只在调用 process 的线程上读；插件不调用。
        struct RenderCounters
        {
            juce::int64 hardClipSamples = 0;
            double limitDiffEnergy = 0.0;
            double limitInputEnergy = 0.0;
        };

        RenderCounters getAndResetRenderCountersForTesting() noexcept;

        // true = 每段顺带累计两份能量（limitDiffEnergy / limitInputEnergy）。默认 false、插件从不打开：插件的音频线程不多做这份运算。
        // 硬钳位计数不受它管（一次比较，一直在数）。只读已有的量，不改输出、状态与其余统计；音频线程上调（与 process 同一线程）。
        void setRenderCountersEnabledForTesting (bool shouldCount) noexcept   { renderCountersEnabled = shouldCount; }

        // 只给测试用：false = 关掉 Clip 级空闲跳过，过采样器永远照常跑——当作逐位对照的参照实现。
        // 音频线程上调（与 process 同一线程）；插件本身从不调用。
        void setClipIdleSkipEnabledForTesting (bool shouldSkipWhenIdle) noexcept   { clipIdleSkipEnabled = shouldSkipWhenIdle; }

        // 测试 / 基准用：自 prepare 以来因 Clip 级空闲而没跑过采样的样本数（证明跳过真的发生了）。
        juce::int64 getClipIdleSkippedSamples() const noexcept                     { return clipIdleSkippedSamples; }

        // 测试参照：prepare之前关闭，恢复原30ms快段的双包络；插件始终启用。
        void setLowFrequencyReleaseEnabledForTesting (bool enabled) noexcept { lowFrequencyReleaseEnabled = enabled; }
        float getAutoFastReleaseMsForTesting() const noexcept { return (float) lowFrequencyReleaseMs; }

        // 只给测试用（T-032 R1-03）：输出硬钳位用的 Ceiling 延迟线接下来依次交出的值（共 coreDelay 个，最早的在前）。
        // 在副本上推，不改本对象；会分配，不在音频线程调。重置（prepare / reset / restartWithFadeIn / 换前瞻）之后
        // 这些值应是重置那一刻的 Ceiling，而不是 0——重置同时清空了音频延迟线，这一点从输出上看不出来，只能这样查。
        std::vector<float> getPendingHardClipCeilingsForTesting() const;

        //==============================================================================
        // 滑动最小值：返回最近 window 个输入里的最小值。van Herk / Gil-Werman 块算法（T-032 R1-02，2026-09-29）：
        // 输入按 window 长的块存；prefix = 当前块到本样本为止的前缀最小值，suffix[j] = 上一块从 j 到块尾的后缀最小值
        //（块满时一次算完 window 次取小，均摊每样本 1 次）；窗口 = 当前块的前缀 ∪ 上一块的后缀，所以每样本恒定 2 次取小、
        // 无整数取模、无数据相关的循环、与窗长无关。旧实现是单调队列（每样本 2 ～ 3 次取模 + 两个数据相关的 while），
        // 核验在 107 测得 W = 159 时 9.4 ～ 13.7 ns / 样本、本算法约 2.8 ns（控制路每样本一次，48 kHz 下约省 0.03 % 个核）。
        // 初始 / clear() 后 suffix 全是 +∞ 哨兵：窗口里还没被填满的部分不参与，与旧实现「只看已推入的样本」严格一致，不依赖输入 ≤ 1。
        // 取小是选择运算、不做算术，相等时取哪个都是同一位模式（输入是需求增益：≥ +0，不会有 −0，由比较产生、不会是 NaN）。
        // 唯一的差别在冲零模式：DAZ 开着时 minss 把非规格化的操作数当 0 返回，旧实现返回原来的位；但下游第一步就把它转成 double
        //（cvtss2sd 同样受 DAZ 管）或拿它比较，两边得到的都是 0，输出不变。需求增益要非规格化，峰值得超过约 1e38，实际不会出现。
        // LimiterTests 保留旧实现作参照，在随机窗长上与它、与暴力 std::min_element 逐样本对照。
        // 放在 public 只为单元测试能直接拿它对照；插件代码只经 LimiterModule 用它。
        struct SlidingMinimum
        {
            void prepare (int maxWindow);                  // 分配（不在音频线程调用）
            void setWindow (int newWindow) noexcept;       // 1 ≤ newWindow ≤ prepare 时的 maxWindow（越界钳进来）；改窗同时清空
            void clear() noexcept;
            float push (float value) noexcept;

            std::vector<float> block;    // 当前块的输入（长 maxWindow，只用前 window 个）
            std::vector<float> suffix;   // 上一块的后缀最小值（长 maxWindow + 1），suffix[window] 是 +∞ 哨兵
            int window = 1, position = 0;
            float prefix = 0.0f;
        };

    private:
        //==============================================================================
        // 长度 length 的移动平均：把阶梯状的增益需求变成一条无棱角的线性斜坡（攻击整形）。
        struct MovingAverage
        {
            void prepare (int maxLength);
            void setLength (int newLength) noexcept   { length = newLength; }
            void clear() noexcept;
            float push (float value) noexcept;

            std::vector<float> ring;
            int length = 1, writeIndex = 0;
            double sum = 0.0;
        };

        //==============================================================================
        void processChunk (juce::AudioBuffer<float>& io, int startSample, int numSamples, juce::AudioBuffer<float>* difference);
        void processClipStage (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);
        void processLimiterCore (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);

        // Clip 级湿路：过采样 → 削波 → 下采样（原地）。followSmoothedThreshold = false 时沿用 clipper 当前的阈值（预滚用）。
        // collectStats = true 时顺带统计 4× 域的削波活动（只读削波前的样本，不改处理）；预滚与湿路被丢弃时不统计。
        void runClipOversampled (juce::dsp::AudioBlock<float>& block, bool followSmoothedThreshold, bool collectStats) noexcept;
        void accumulateClipStats (const juce::dsp::AudioBlock<float>& upsampled, float threshold) noexcept;
        void accumulateRenderEnergies (const juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse) noexcept;
        void finishBlockStats (int numSamples) noexcept;
        void recordClipIdleHistory (const float* const* channels, int startSample, int numSamples) noexcept;
        void prerollClipOversampler() noexcept;
        void resetClipIdleState() noexcept;
        float nextReleaseEnvelope (float target) noexcept;
        void observeLowFrequency (const float* const* channels, int index, int channelsToUse) noexcept;
        void updateLowFrequencyRelease() noexcept;
        void resetLowFrequencyRelease() noexcept;
        void configureAttackShape() noexcept;

        void applyLookaheadChange();
        void resetLimiterState();
        void startFadeIn (int extraHoldSamples);
        void updateReleaseCoefficients() noexcept;
        int lookaheadSamplesFor (float milliseconds) const noexcept;
        int coreDelaySamples() const noexcept   { return lookaheadSamples + holdSamples - 1; }

        //==============================================================================
        Params params;
        double sampleRate = 0.0;
        int maxBlockSize = 0;
        int numChannels = 0;

        // Clip 级
        std::unique_ptr<HalfBandOversampler> clipOversampler;   // v0.5：自写块式半带（同系数、同延迟，与 JUCE 差 < −130 dBFS）
        int clipLatency = 0;
        Saturator clipper;
        std::vector<DelayLine> clipDryDelays;
        juce::AudioBuffer<float> clipDryScratch;

        // Clip 级空闲跳过（见文件头）。预滚长度至少 256 个样本：两级半带的记忆约为冲激响应长度
        // ≈ 2 × 延迟 + 1 = 121 个样本（研究 P4 实测预滚 64 不够、128 已逐位相同），256 留一倍余量；
        // 换成别的过采样器时按 4 × 延迟放大。
        static constexpr int minClipPrerollSamples = 256;
        int clipPrerollLength = minClipPrerollSamples;
        juce::AudioBuffer<float> clipIdleHistory;     // [声道][clipPrerollLength] 环形：最近的空闲期 Clip 级输入
        int clipIdleHistoryWrite = 0;                 // 下一个写入位置 = 最旧样本的位置
        int clipIdleSamples = 0;                      // 连续空闲的样本数（封顶 clipPrerollLength）
        bool clipStageSkipping = false;               // 正在跳过：过采样器状态已过期，重开前必须预滚
        float clipIdleThreshold = 1.0f;               // 空闲期间的削波阈值（恒定），预滚时用
        juce::AudioBuffer<float> clipPrerollScratch;
        bool clipIdleSkipEnabled = true;
        juce::int64 clipIdleSkippedSamples = 0;

        // 限幅器核心
        TruePeakDetector detector;
        juce::AudioBuffer<float> detectorPeaks;       // [声道][maxBlockSize]：本段每个样本的真峰值
        SlidingMinimum requiredGainHold;
        MovingAverage attackRamp;
        MovingAverage attackShape;   // 第二级滑动平均：与 attackRamp 串联后总建立时间仍等于 Lookahead
        std::vector<DelayLine> coreDelays;
        DelayLine ceilingDelay;
        int lookaheadSamples = 1;
        bool lookaheadChangePending = false;

        // 释放包络：全部用 double。float 在 1.0 附近的步长太粗，包络回升到离 1 还差
        // 约 2.98e-8·τ·fs 时每步增量被舍入吃掉，会永远卡在 1 以下（ADR-004 复核 V-05）。
        bool activeAutoRelease = true;
        double envelopeManual = 1.0, envelopeFast = 1.0, envelopeSlow = 1.0, slowTarget = 1.0;
        double lastEnvelope = 1.0;
        double averageReductionDb = 0.0;
        double manualReleaseCoeff = 0.0, fastReleaseCoeff = 0.0, slowReleaseCoeff = 0.0;
        double slowTargetCoeff = 0.0, averageCoeff = 0.0;
        int adaptCounter = 0;

        struct LowFrequencyState
        {
            double low1 = 0.0, low2 = 0.0, previous = 0.0, periodSamples = 0.0;
            double previousPeriodSamples = 0.0;
            int crossingAge = 0, consistentPeriods = 0;
        };
        std::vector<LowFrequencyState> lowFrequencyStates;
        bool lowFrequencyReleaseEnabled = true;
        double lowpassAlpha = 0.0, lowFastAlpha = 0.0, lowSlowAlpha = 0.0;
        double lowReleaseEnterAlpha = 0.0, lowReleaseExitAlpha = 0.0;
        double lowEnergyFast = 0.0, lowEnergySlow = 0.0, fullEnergyFast = 0.0;
        double lowPeriodSamples = 0.0, lowFrequencyReleaseMs = 30.0;
        int lowStableSamples = 0;
        int deepReductionSamples = 0;
        int transientReleaseSamples = 0;
        int transientMaxSamples = 1;

        // 旁通
        std::vector<DelayLine> bypassDryDelays;
        juce::AudioBuffer<float> bypassDryScratch;

        // 试听差值：逐样本的平滑后 Gain 延迟整个本级延迟，与 bypassDryScratch 的干路对齐（见文件头）。
        DelayLine gainDelay;
        std::vector<float> delayedGainScratch;   // [maxBlockSize]：本段每个输出样本对应的 Gain

        // 20 ms 平滑（设计规格-总体「参数」）
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gainSmoothed;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> ceilingSmoothed;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> clipThresholdSmoothed;   // Clip 过采样后的 4× 采样率
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> listenGainSmoothed;
        juce::SmoothedValue<float> clipMixSmoothed;
        juce::SmoothedValue<float> bypassMixSmoothed;

        // Lookahead 变动后的静音保持 + 10 ms 淡入，避免重置时咔哒。
        juce::SmoothedValue<float> fadeInSmoothed;
        int fadeInHoldSamples = 0;

        std::atomic<float> maxGainReductionDb { 0.0f };

        // v0.6 每块统计：process 开头清零，处理中只做累加（读已有的量），process 末尾算成 blockStats。
        struct StatsAccumulator
        {
            double grProduct = 1.0;          // Π 增益（只乘 < 1 的；double，积小于 1e-200 时用 frexp 把 2 的幂次移出，防下溢）
            int grExponent = 0;              // 移出的 2 的幂次之和：Π 增益 = grProduct · 2^grExponent
            float minGain = 1.0f;            // 本块最小增益（→ grMaxDb，与表同一公式）
            juce::int64 clipCount = 0;       // 4× 域 |x| > T 的样本数（各声道合计）
            juce::int64 clipTotal = 0;       // 4× 域样本总数（各声道合计，含空闲跳过、湿路被丢弃的部分）
            float clipPeakRatio = 0.0f;      // 4× 域 max |x| / T
            juce::int64 hardClipSamples = 0; // T-096 (a)：硬钳位改了值的样本数
            double diffEnergy = 0.0;         // T-096 (a)：Σ(加了 Gain 的输入 − 湿路输出)²（只在 renderCountersEnabled 时累计）
            double inputEnergy = 0.0;        // T-096 (a)：Σ(加了 Gain 的输入)²（同上）
        };

        StatsAccumulator stats;
        BlockStats blockStats;

        // T-096 (a) 渲染计数：开关与「自上次读取以来」的累计（见 getAndResetRenderCountersForTesting）。
        bool renderCountersEnabled = false;
        RenderCounters renderCounters;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterModule)
    };
}
