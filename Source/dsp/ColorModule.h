/*
  ==============================================================================

    ColorModule.h
    Color 模块：总线压缩与母带并联染色（设计规格-Color，含 2026-09-26 热输入修订）。

    信号流（每块）：

      输入 ─┬─ Tone（100 Hz 低搁架 −Tone / 8 kHz 高搁架 +Tone，double 精度）
      │   → Glue（前馈总线压缩：侧链 100 Hz 高通、左右联动峰值、2:1、6 dB 软拐点、
      │           攻击 20 ms、释放 50 ～ 500 ms 按近 500 ms 平均压缩量自适应、自动补偿；Glue = 0 彻底关闭。
      │           联动包络仍决定压缩量和表读数。低 / 高两侧若自己更轻，增益最多比联动增益高 3.2 dB，
      │           响的一侧保持原来的压缩。三路增益相等时退回整段同一个增益。）
            │   → Heat（原声延迟 64 样本 + 非线性残差：M/S → 150 Hz 高通 → 同一 8× Tube(x)−x
            │           → 5 Hz 去直流 → M/S 解码；Heat = 0 时 20 ms 淡出到等长延迟原声）
            │   → Mix（干路 = 本模块输入经等长延迟）→ Output
            │                                                         │
            └──────── 等长延迟的干路 ─────────── Bypass 交叉淡化 ←──────┘

    Heat 的 h = Heat / 100：Mid 驱动 1+h、不对称 0.04h、残差比例 0.25h；Side 驱动 1+0.5h、
    严格奇对称，残差比例再乘 0.75。原声不经过支路高通；纯 Side 不生成 Mid、mono 不生成 Side。
    旧 −18 dB 曲线标定表仅作轻微残差 makeup，不补偿或改变低频原声的电平。
    延迟固定 = Heat 过采样延迟（整数 64 样本，与采样率、参数都无关）。
    只依赖 juce_dsp，可无头编译做自动测试。

    沿用 v0.5 性能优化（研究 P3 R1 ～ R4），已适配当前残差拓扑：
      - Heat 的 8× 过采样换成块式 SIMD 半带 FIR（HalfBandOversampler：同系数、同线性相位、同延迟 64）；
      - Tube 在驱动量不变的一段上整段调用 Saturator::processTubeBlock（4 路并行有理式 tanh）；
      - 空闲整段跳过：Tone 归零且状态落到 0、Glue 完全关掉、Mix / Output / Bypass 都静止、Heat 湿路静止在 0
        持续 ≥ 256 样本时，各自整段跳过，都与不跳过逐位相同（含 ±0 与 NaN / Inf 输入；Heat 重开时用最近 256 个
        高通后的 M/S 输入预热过采样器）；150 Hz 高通一直运行，空闲时 5 Hz 直流阻断吃已有 2× 链的近似残差；
      - 三条 64 样本延迟按块拷贝；ARM 上 Tone 搁架与 Glue 侧链高通两声道打包（与标量写法逐位相同）；
      - Heat 残差 makeup 表与采样率无关，进程内只标定一次。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "HalfBandOversampler.h"
#include "Saturator.h"

#include <atomic>
#include <memory>
#include <vector>

namespace celadon
{
    class ColorModule
    {
    public:
        struct Params
        {
            float toneDb     = 0.0f;     // −3 ～ +3
            float glue       = 0.0f;     // 0 ～ 100
            float heat       = 0.0f;     // 0 ～ 100
            float mixPercent = 100.0f;   // 0 ～ 100
            float outputDb   = 0.0f;     // −6 ～ +6
            bool  bypass     = false;
        };

        ColorModule() = default;

        void prepare (const juce::dsp::ProcessSpec& spec);

        // 清空全部状态（prepare 时、宿主 Reset、NaN 恢复时调用）；平滑器直接落到当前目标。
        void reset();

        // 音频线程每块调一次。
        void setParams (const Params& newParams);

        // 原地处理；样本数可以超过 prepare 时的块大小（内部切块）。
        void process (juce::AudioBuffer<float>& io);

        // 固定延迟（样本），与参数无关。
        int getLatencySamples() const noexcept;

        // Glue 自上次读取以来的最大压缩量（dB，正数），读完清零。界面线程调用，无锁。
        float getAndResetGainReductionDb() noexcept;

        // 上一次 process() 这一块的统计（v0.6 GLAZE 保护层与约束核对用；设计规格-GLAZE-分析器第 1 节「模块内部」测点）。
        // 只在调用 process 的线程上读。空闲跳过的级按「没在工作」计。
        // 统计只读处理过程中已有的量，不改音频路径：输出、GR 表读数、空闲跳过行为与没有统计时逐位相同。
        // 描述的是本级自己的处理量，与本模块的 Mix / Output / Bypass 无关（旁通时 Glue 照常在算，这里照报；表不报数）。
        // 一次 process 的样本数超过 prepare 的块大小时，统计覆盖整个调用（各内部分段合并）。
        struct BlockStats
        {
            float glueGrMeanDb = 0.0f;   // Glue 压缩量按样本在 dB 域的均值（正数）：实际作用的压缩量 = 压缩量 × Glue 开关淡化（同表）
            float glueGrMaxDb = 0.0f;    // 本块 Glue 最大压缩量（正数），与同一块 GR 表读数同一个量
            float heatDrive = 0.0f;      // 本块 Heat 实际驱动量（湿路混合后的有效 Heat，0 ～ 25）：逐样本「驱动这个湿路样本的
                                         // Heat 值 × Heat 湿路比例」的均值（湿路 = 0.25 × Heat / 100；空闲跳过时 0）
        };

        const BlockStats& getBlockStats() const noexcept   { return blockStats; }

        //==============================================================================
        // 测试 / 基准用。默认开着空闲整段跳过；关掉后每一级都照常逐样本处理，用来对照：任何参数变化、任何输入下
        // 跳过与不跳过的输出都逐位相同（Heat 湿路重开时靠预热复原过采样器）。可在两块之间随时切换（与 process 同一线程）。
        // 唯一例外：开着 FTZ / DAZ（宿主 processBlock 里的 ScopedNoDenormals）且输入里有非规格化数（|x| < 1.2e-38）时，
        // 被跳过的级不做算术，不会把这些样本冲成 0——差别 < 1.2e-38（约 −758 dBFS）；不含非规格化数的输入仍逐位相同。
        void setIdleSkippingEnabled (bool shouldSkip) noexcept;

        // 测试用。默认开着：Heat 与湿路淡化都落到 0 时，5 Hz 直流阻断改吃「近似残差」（见 .cpp processHeat）。
        // 关掉 = 直流阻断永远吃真实8×残差，这时 Heat 湿路不能空闲跳过；只用来量代理近似的差别。
        void setHeatDcProxyEnabled (bool shouldUseProxy) noexcept;
        // Test baseline: leaves the original dry + nonlinear residual topology.
        void setHeatHighFrequencyProtectionEnabled (bool enabled) noexcept { heatGuardEnabled = enabled; }
        float getHeatHighFrequencyGainForTesting() const noexcept { return (float) heatGuardGain; }

        // 最近处理的一段里哪些级走了空闲跳过（测试用：确认跳过路径真的走到了；与 process 同一线程调用）。
        struct IdleState
        {
            bool tone    = false;   // Tone 整段跳过
            bool glue    = false;   // Glue 只跑侧链高通
            bool heatWet = false;   // Heat 湿路（过采样 + Tube）整段跳过
            bool output  = false;   // Mix / Output / Bypass 三个平滑器都静止，按常数处理
        };

        IdleState getLastIdleState() const noexcept;

    private:
        //==============================================================================
        // 转置直接 II 型双二阶（double 精度）：系数按 a0 归一，状态每声道一份。
        // Tone 的两个搁架与 Glue 的侧链高通都用它（float 下 100 Hz 附近的系数误差偏大）。
        struct BiquadCoefficients
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        struct BiquadState
        {
            double s1 = 0.0, s2 = 0.0;
        };

        // 整数样本延迟，按块拷贝（prepare 时分配，处理时零分配）：缓冲 = [最近 delay 个输入 | 本块临时区]。
        class BlockDelay
        {
        public:
            void prepare (int delaySamples, int maxBlockSize);
            void fill (float value) noexcept;                                       // 历史整段填成同一个值
            void process (const float* input, float* output, int numSamples) noexcept;   // output[i] = input[i − delay]

        private:
            std::vector<float> buffer;
            int delay = 0;
        };

        static double processBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept;
        static void snapToZero (double& value) noexcept;
        static BiquadCoefficients designShelf (double fs, double frequency, double gainDb, double q, bool isHighShelf);
        static BiquadCoefficients designHighpass (double fs, double frequency, double q);
        static BiquadCoefficients designLowpass (double fs, double frequency, double q);
        static void configureTube (Saturator& saturator, float heat) noexcept;
        static const float* heatCompensationTable();

        //==============================================================================
        void processChunk (juce::AudioBuffer<float>& io, int startSample, int numSamples);
        void processTone (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);
        void processGlue (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);
        void processHeat (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);
        void processOutput (juce::AudioBuffer<float>& io, int startSample, int numSamples, int numChannelsToUse);

        bool isToneIdle (int numChannelsToUse) const noexcept;
        void computeSidechainPeaks (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept;
        void analyseGlueBands (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept;
        bool isHeatWetIdleCondition() const noexcept;
        void recordHeatHistory (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept;
        void copyHeatHistory (int readPosition, int length, int numChannelsToUse) noexcept;
        void primeHeatOversampler (int numChannelsToUse) noexcept;
        void primeHeatProxy (int numChannelsToUse) noexcept;
        void runHeatProxy (int numSamples, int numChannelsToUse) noexcept;
        void processHeatWhileIdle (float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept;
        void prepareHeatInput (const float* const* channels, int startSample, int numSamples, int numChannelsToUse) noexcept;
        void processResidualBlock (float* data, int numSamples, const Saturator& shaper) noexcept;
        void protectHeatResidual (float* const* channels, int index, int dryIndex, int channelCount, float mix) noexcept;
        void processTubeSegments (juce::dsp::AudioBlock<float>& upsampled) noexcept;
        double dcBlock (double x, double& state) const noexcept;   // 5 Hz 一阶高通的一步，湿路与空闲近似共用

        void applyParamTargets (bool immediately);
        void updateToneCoefficients (float toneDb);
        void updateGlueMapping (float glue) noexcept;
        void restartGlueFromOff() noexcept;   // Glue 从完全关掉重新打开：映射落到新值、压缩量包络清零
        void updateHeatDrive (float heat) noexcept;
        double nextGainReductionDb (double target) noexcept;
        float heatCompensationFor (float heat) const noexcept;

        //==============================================================================
        Params params;
        double sampleRate = 0.0;
        int maxBlockSize = 0;
        int numChannels = 0;

        // Tone：100 Hz 低搁架（−Tone）+ 8 kHz 高搁架（+Tone），系数每 16 样本按平滑值重算。
        BiquadCoefficients lowShelf, highShelf;
        std::vector<BiquadState> lowShelfStates, highShelfStates;
        float toneForCoefficients = 0.0f;   // 当前系数对应的 Tone 值，没变就不重算
        int toneUpdateCounter = 0;          // 跨块延续，结果与块大小无关

        // Glue：侧链高通 + 前馈增益计算 + dB 域包络。
        BiquadCoefficients sidechainHighpass;
        std::vector<BiquadState> sidechainStates;
        std::vector<double> sidechainPeaks; // 本块每个样本的侧链峰值（左右取大）
        // 分频只产生相对联动增益的差：检测器吃 100 Hz 高通后的低/高分裂，音频分裂用同一组系数的另一套状态。
        BiquadCoefficients glueLowpass, glueHighpass;
        std::vector<BiquadState> bandDetectorHpStates, bandDetectorLowStates, bandDetectorHighStates;
        std::vector<BiquadState> audioLowStates, audioHighStates;
        std::vector<double> glueLowPeaks, glueMidPeaks, glueHighPeaks;
        std::vector<double> glueAudioLow, glueAudioHigh;   // [声道 × maxBlockSize + 样本]
        double lowReductionDb = 0.0, midReductionDb = 0.0, highReductionDb = 0.0;
        double lowBandAttackCoeff = 0.0, highBandAttackCoeff = 0.0;
        float glueForMapping = -1.0f;       // 当前阈值 / 补偿对应的 Glue 值
        double thresholdDb = 0.0;           // 阈值：0 → 0 dBFS，100 → −24 dBFS
        double kneeStartLinear = 0.0;       // 阈值 − 拐点宽度 / 2 的线性幅度：低于它压缩量必为 0，不用算 log
        double makeupDb = 0.0;              // 自动补偿 (0 − 阈值)·(1 − 1/比率)·0.5
        double gainReductionDb = 0.0;       // 平滑后的压缩量（正数，dB）
        double averageReductionDb = 0.0;    // 最近 500 ms 的平均压缩量，决定释放时间
        double attackCoeff = 0.0, releaseCoeff = 0.0, averageCoeff = 0.0;
        int adaptCounter = 0;

        // Heat：仅高通后的 M/S 非线性残差过采样，原声沿固定 64 样本干路保留。
        std::unique_ptr<HalfBandOversampler> heatOversampler;
        int heatLatency = 0;                // = getLatencySamples()，整数 64
        int oversamplingFactor = 1;
        Saturator tube, sideTube;
        BiquadCoefficients heatHighpass;
        std::vector<BiquadState> heatHighpassStates; // 1×，包括 Heat0 时也推进，不能靠有限历史重建 IIR
        std::vector<double> heatGuardLow;
        double heatGuardLowCoeff = 0.0, heatGuardPowerCoeff = 0.0;
        double heatGuardAttackCoeff = 0.0, heatGuardReleaseCoeff = 0.0;
        double heatGuardDryPower = 0.0, heatGuardHighPower = 0.0;
        double heatGuardTarget = 1.0, heatGuardGain = 1.0;
        int heatGuardCounter = 0;
        bool heatGuardEnabled = true;
        juce::AudioBuffer<float> heatInputScratch;   // 高通后的 M/S（mono = Mid），也是预热历史的唯一来源
        std::vector<float> heatResidualScratch;     // 8× 曲线整形前的输入副本，所有声道顺序共用
        float heatForTube = -1.0f;          // 当前驱动量对应的 Heat 值
        int heatUpdateCounter = 0;          // 过采样域计数器，跨块延续
        const float* heatCompensation = nullptr;   // h = 0 … 100 的轻微残差 makeup（进程内只标定一次）
        std::vector<BlockDelay> heatDryDelays;
        BlockDelay heatValueDelay;          // 平滑后的 Heat 值延迟 heatLatency，残差makeup与湿路对齐
        juce::AudioBuffer<float> heatDryScratch;
        std::vector<float> heatScratch;             // 本块每个样本平滑后的 Heat 值
        std::vector<float> heatDelayedScratch;      // 同上经 heatLatency 延迟
        double dcBlockerB0 = 1.0, dcBlockerB1 = 0.0, dcBlockerA1 = 0.0;   // 一阶高通（转置直接 II 型，一个状态）
        std::vector<double> dcBlockerStates;

        // Heat 湿路空闲跳过：最近 heatPrimeLength 个高通后的 M/S 输入，重开时预热同一个过采样器。
        juce::AudioBuffer<float> heatHistory;
        int heatHistoryWrite = 0;           // 下一个写入位置 = 最旧样本的位置
        int heatIdleSamples = 0;            // 此前已处理的「湿路静止在0」连续样本数（封顶heatPrimeLength）
        bool heatWetIdle = false;           // 湿路正在跳过
        juce::AudioBuffer<float> heatWorkScratch;   // 预热与空闲时直流阻断近似驱动的临时区

        // 近似残差：Heat 与湿路淡化都落到0时驱动5 Hz直流阻断（见processHeat）。
        // Heat 过采样的第 1 级（2×，同一组滤波器）来回 + Heat0 的 Tube(x)−x，补齐直流增益并延迟到64。
        std::unique_ptr<HalfBandOversampler> heatProxyOversampler;
        std::vector<BlockDelay> heatProxyAlignDelays;
        Saturator proxyTube;                // 固定为 Heat 0 的 Tube（即 tanh）
        float heatProxyUpGain = 1.0f, heatProxyDownGain = 1.0f;
        bool heatProxyRunning = false;      // Heat 目标为 0 时才跑；开始跑的那一块先预热

        // 干路 = 本模块输入经等长（= Heat 过采样）延迟。Mix 的干路与 Bypass 的干路是同一个信号，共用一条延迟线。
        std::vector<BlockDelay> inputDelays;
        juce::AudioBuffer<float> inputDelayedScratch;

        // 20 ms 平滑（设计规格-总体「参数」）
        juce::SmoothedValue<float> toneSmoothed;
        juce::SmoothedValue<float> glueSmoothed;
        juce::SmoothedValue<float> glueOnSmoothed;   // Glue 开关的 20 ms 淡入淡出（0 = 关、1 = 开），见 processGlue
        juce::SmoothedValue<float> heatSmoothed;
        juce::SmoothedValue<float> heatMixSmoothed;
        juce::SmoothedValue<float> mixSmoothed;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGainSmoothed;
        juce::SmoothedValue<float> bypassMixSmoothed;

        bool idleSkippingEnabled = true;
        bool heatDcProxyEnabled = true;
        IdleState lastIdleState;

        std::atomic<float> maxGainReductionDb { 0.0f };

        // v0.6 每块统计：process 开头清零，处理中只做累加（读已有的量），process 末尾算成 blockStats。
        struct StatsAccumulator
        {
            double glueReductionSumDb = 0.0;   // Σ 实际作用的 Glue 压缩量（dB，逐样本）
            double glueReductionMaxDb = 0.0;
            double heatDriveSum = 0.0;         // Σ 有效 Heat（逐样本）
        };

        StatsAccumulator stats;
        BlockStats blockStats;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColorModule)
    };
}
