/*
  ==============================================================================

    ClayModule.h
    Clay 修坯模块：立体声像 + 自动清理（设计规格-Clay 第 4 节、ADR-009；第②级按 ADR-012 / 设计规格-Clay-根治修订 第 2 节
    改为线性相位收单）。信号链 Balance → Clay → Color → Limit。

    信号流（系数与状态 double；固定延迟 D + G——①～③ 的 D、④ 的 G，与参数无关）：

      L, R ─→ ① 低切：L/R 各一个 4 阶 Butterworth 高通（两级 RBJ 高通，Q = 0.5412 / 1.3066；IIR），lowCutHz = 0 时不作用
           ─→ M = (L + R) / 2，S = (L − R) / 2
           ─→ M_d = M 延迟 D，S_d = S 延迟 D
           ─→ ② 线性相位收单：S_low = LP_L(S) 再延迟 D − 2(L − 1)，S' = S_d − a · S_low
                 （LP_L = LinearPhaseLowpass：4 个长度 L 的滑动平均级联，L = lengthFor(fs, monoHz)，群延迟 2(L − 1)；a = monoAmount）
           ─→ ③ 加宽：S'' = HS(S')（RBJ 高架，f0 = 2 kHz，斜率 S = 1，增益 widthDb；IIR）——只用来算增量 δ = S'' − S'
           ─→ L' = M_d + S'，R' = M_d − S'（①② 的输出 y；旁通淡化在这里，与干路 = 输入延迟 D 混合；δ 随旁通一起淡出）
           ─→ ④ 峰值守护（ADR-014；2026-09-29 T-032 修订：只管①②）：y = ①② 的输出、x = 输入延迟 D（干路）、±δ = ③ 的增量，
                 输出 x + g·(y − x) ± δ 再延迟 G；g ∈ [0, 1] 左右共用，保证每个输出样本 |out ∓ δ| ≤ B = max(E, τ·P, F)（见下）

    - 核心延迟 D = LinearPhaseLowpass::groupDelayFor (maxLengthFor (fs))（L_max 按手动收单下限 40 Hz 算）：44.1 / 48 / 96 / 192 kHz
      为 528 / 574 / 1150 / 2302 样本（约 12 ms）。④ 再加前瞻 G = W + A（44.1 / 48 / 96 / 192 kHz 为 485 / 528 / 1056 / 2112）。
      getLatencySamples() = latencyFor (fs) = D + G（1013 / 1102 / 2206 / 4414，约 23 ms；prepare 后恒定、与参数无关），
      链把它算进总延迟。
    - ④ 峰值守护（ADR-014）。为什么：①～③ 都会改变波形（低切的相位旋转、收单去掉侧声道低音、加宽抬高侧声道），在已经很密 /
      限过幅的歌上会让局部峰值变高，末级限幅器就多压、响度反而下降（真歌门槛：低切 10.8 Hz 让 GR 最大 +0.94 dB，收单 +1.06 dB）。
      「先试后用」只在聆听的前 15 ～ 30 s 里试，前奏安静、副歌才密的歌试不出来。守护在每个样本上直接保证低切 / 收单不抬高要紧的
      峰值（T-032 起③ 加宽不进守护，理由见下）：
        E = 干路两声道 |x| 在 ±W（8 ms）内的最大值（局部峰值；W 盖住低切在 30 Hz 以上造成的峰值前移）；
        P = E 的峰值包络（瞬时上、1 s 时间常数指数下）；τ = −6 dB：比最近的峰值低 6 dB 以上的样本碰不到末级限幅
            （GLAZE 的限幅 GR 最大在 5 dB 以内），随便处理；
        F = −40 dBFS：绝对地板，低电平的细节（线性相位低通的前后振铃、冲激的尾巴）不管；
        需求 g_req = 让 |x + g·(y − x)| ≤ B 的最大 g（两声道取小；|y| ≤ B 时为 1）；
        g = 「g_req 在 [n, n + A] 的最小值 → 线性释放（100 ms 从 0 回到 1）→ 长 A + 1 的滑动平均」，A = 3 ms——与 LimiterModule
            同一种前瞻 / 释放 / 攻击整形，数学上保证 g(n) ≤ g_req(n)（滑动平均窗里的每一项都 ≤ g_req(n)），不会漏过任何峰。
      只回收①② 的改动量 y − x，不动干路本身：g = 0 时输出就是干路 + δ（低切 / 收单在这一刻让开），任何时刻低切 / 收单都不会让它
      比「不修坯」更响、更挤。
      y 与 x 逐位相同（旁通、三项为 0 的逐位纯延迟）且 δ = 0 时输出逐位 = y，与 g 无关；g 精确为 1 且 δ = 0 时输出逐位 = y；
      δ ≠ 0 时输出 = float(x + g·(y − x) + δ)（δ 为 0 时不做那次加法：(−0) + (+0) = +0，无条件相加会打破 ±0 的逐位契约）。
      T-032 修订（2026-09-29，研究 R4-01 / 核验 V4，待 Max 复核）：为什么③ 不进守护——界 B 在干路自己就是 ±W 窗最大值的那些样本上
      没有余量（B = E = |x|）；凡「在峰值处同向增加」的处理在那里 g_req 恰为 0（与增加多少无关），再经 100 ms 线性释放追不上密信号里
      每约 16 ms 一个的局部峰值，g 从不回到 1。加宽让 L = M + γS 在 S 与 M 同号处变大，而 |L| 成为局部最大时两者几乎总同号，于是
      加宽在正常电平（> −40 dBFS）下被回收 85 ～ 95 %（宽粉噪峰值 −12 dBFS 手动 +3 dB，标称 +2.97 dB，实测只剩 +0.25 ～ +0.36 dB；
      纯侧声道内容 0.00 dB），界面却照样显示在做，压力格（只量 max|y|/B）也看不见它。低切是去能量，越界只来自相位错开，回收小得多
      （25 Hz 对 15 Hz 次声：标称 −17.8、实际 −16.6 dB），守护对它合理。收单在局部峰值样本上同样零余量、效果大半被回收（M / S 独立
      粉噪 S/M −10 dB：标称 −22.6、实际 −5.8 dB），T-032 不改它，要不要也拆出守护待 Max 定（ClayTests 的夹具钉住了现值）。
      所以守护只管①②：③ 的增量 δ = S'' − S'（解码后 L 加 δ、R 减 δ）不进守护、在守护输出上原样加回，界改为 |out ∓ δ| ≤ B；
      ③ 的峰值风险由「先试后用」的代价 widthPeakCostDb、宽度保护的相关度上限与自动最多 +1.5 dB 控制（根治修订 §1 的原意：
      「加宽……影响小，交给先试后用」）。widthDb = 0（高架逐位直通且无余振）时 δ 精确为 0，模块输出与修订前逐位相同（唯一的例外是
      NaN 的符号位：输入里的 NaN 与 ±Inf 造出的 NaN 在同一次加法里相遇时传出哪一个，IEEE 754 不规定、编译器可以交换加法的两边，
      T-032 实现报告第 4 节）；①② 都不作用时
      y 与 x 逐位相同、守护不动作，输出 = x ± δ——加宽不进守护统计、不进代价格。放弃的替代：给 E 加 0.5 ～ 1 dB 余量能让加宽恢复，
      但低切的守护同时被削弱（低音密素材 25 Hz 低切的压力 0.49 → 0.10 dB，压力反馈不再降档），正是 ADR-014 要防的场景。
      为什么要延迟：最小相位 IIR 收单在低频的相位旋转会把已经很密的波形的峰值抬高 1 ～ 4 dB（ADR-012）；线性相位的互补高通
      S_d − S_low 只削低频的侧声道、不错开峰。低切与加宽仍是零延迟 IIR（线性相位低切要上百毫秒延迟，见根治修订第 5 节），
      只是跟着 M / S 一起延迟 D 对齐。
    - 时刻：① 在延迟之前，它的输出选择 / 淡化作用在输入时刻，要再过 D 个样本才到输出；② 的 a、③ 的增益、旁通淡化都作用在
      输出时刻（延迟之后）。
    - 平滑：低切频率在对数域 20 ms 平滑、每 64 样本（从 reset 起的固定网格）重算系数；a 与 widthDb 线性 20 ms 平滑。
      分频点（整数 L）变化时：在网格点把第二套低通 reset 成新 L，用 S 的历史预热 4(L − 1) 个样本——分摊到之后的实时样本里
      （每个样本多喂 2 个，2(L − 1) 个样本后追上，T-020：一次喂完会在一块里多出 4(L − 1) 次滑动和），追上后的网格点起两套输出
      20 ms 线性交叉淡化；预热或淡化中来的新目标等这次切换走完、在之后的网格点再处理（拖旋钮 = 一串切换）。
    - 影子运行：三级一直在跑——低切、低通（含切换）、高架、延迟线，旁通时也跑。任一级从 0 开到非 0 时状态已就绪，没有起跳瞬态。
    - 逐位纯延迟：三项实际值都为 0 且静止（低切的淡化也已走出延迟线）时，①～③ 的输出 = 输入逐位延迟 D——L、R 各走一条 float
      纯延迟线，不做 M/S 往返（(L + R)/2 + (L − R)/2 在 double 里不保证逐位还原 L）；④ 见到 y 与 x 逐位相同就原样取 y，再过 G 的
      float 环——整个模块输出 = 输入逐位延迟 D + G。某级不作用时它的频率怎么变都不算（例如 a = 0 时 monoHz 在变、低通在切换）。
      GLAZE 0 且抽屉默认 → Clay 是 D + G 样本纯延迟：整链 = 旧链在 Clay 的位置插一段 D + G 样本纯延迟
      （设计规格-Clay-根治修订 第 2 节「GLAZE 0 且抽屉默认」、ADR-014）。
    - 单声道（1 声道）：只做 ①，再延迟 D，④ 按单声道守护；② ③ 跳过。
    - 旁通：干路 = 同一条纯延迟线（延迟补偿式旁通，与其他模块一致），与处理后信号 20 ms 交叉淡化；③ 的增量 δ 随之乘 (1 − bypass)
      淡出；旁通前后延迟都是 D + G（完全旁通时 y 与 x 逐位相同、δ = 0，④ 原样通过）。
      reset 清全部滤波器状态与延迟线、平滑器落到目标。
    只依赖 juce_dsp（LinearPhaseLowpass 只依赖 juce_core），可无头编译做自动测试；prepare 分配，音频线程 0 分配 0 锁。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "LinearPhaseLowpass.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace celadon
{
    class ClayModule
    {
    public:
        // 参数范围（setParams 里钳制，模块不信任外部值）。
        static constexpr float minLowCutHz = 10.0f;
        static constexpr float maxLowCutHz = 60.0f;
        static constexpr float minMonoHz   = 40.0f;    // = LinearPhaseLowpass::minCutoffHz：决定最长的低通与固定延迟 D
        static constexpr float maxMonoHz   = 300.0f;
        static constexpr float minWidthDb  = -3.0f;
        static constexpr float maxWidthDb  = 3.0f;
        static constexpr float widthShelfHz = 2000.0f;

        struct Params
        {
            float lowCutHz   = 0.0f;     // 0 = 关；否则钳到 10 ～ 60 Hz
            float monoHz     = 80.0f;    // 钳到 40 ～ 300 Hz（monoAmount = 0 时不影响输出）
            float monoAmount = 0.0f;     // 0 ～ 1
            float widthDb    = 0.0f;     // −3 ～ +3（S 的 2 kHz 高架增益；负 = 收窄）
            bool  bypass     = false;
        };

        ClayModule() = default;

        // 消息线程：按采样率分配延迟线与两套低通（按最长的 L 分配），末尾 reset。
        void prepare (const juce::dsp::ProcessSpec& spec);

        // 清空全部状态（prepare 时、宿主 Reset、NaN 恢复时调用）：滤波器、低通、延迟线清零，平滑器直接落到当前目标。不分配。
        void reset();

        // 音频线程每块调一次（链在 Balance 之后、Color 之前调 process）。
        void setParams (const Params& newParams);

        // 原地处理；样本数可以超过 prepare 时的块大小（内部切块）。
        void process (juce::AudioBuffer<float>& io);

        // 固定延迟 = 核心延迟 D + 峰值守护前瞻 G（样本）：prepare 后恒定、与参数和旁通无关（44.1 / 48 / 96 / 192 kHz 为
        // 1013 / 1102 / 2206 / 4414）；没 prepare（或采样率无效）时为 0——那时 process 什么都不做。
        int getLatencySamples() const noexcept   { return latencySamples; }

        // 按采样率算的固定延迟 D + G（与 prepare 之后的 getLatencySamples() 相同；采样率无效时为 0）。测试与离线工具用。
        static int latencyFor (double sampleRate) noexcept;

        // 峰值守护的诊断统计（只给测试 / 离线工具；音频线程读、读完清零）：自上次读取以来 g < 1 的输出样本数与最小的 g。
        struct PeakGuardStats
        {
            std::int64_t guardedSamples = 0;
            float minGain = 1.0f;
        };

        PeakGuardStats getAndResetPeakGuardStats() noexcept;

        // 上一次 process() 里走完的「代价格」（从 reset 起每 10 ms 一格的固定网格）：每格 = 低切 / 收单（①②）若不经守护、会让相关峰值
        // 超出界 B 多少 dB（max |y| / B 折成 dB，≤ 0 记 0；y = ①② 的输出——T-032 起③ 加宽不进守护，也不计在这里）——与「先试后用」的
        // 代价同一单位（ADR-014 压力反馈的输入：链每个切片交给编排器）。
        struct PressureReport
        {
            float meanDb = 0.0f;      // 这些格的平均
            double seconds = 0.0;     // 这些格的总时长（格数 × 格长 / fs）
        };

        PressureReport getLastProcessPressure() const noexcept;

        // 只给测试 / 诊断：此刻起（参数不再变的话）输出是否 = 输入逐位延迟 D + G。即：未在旁通淡化中，并且要么完全旁通，
        // 要么三项实际值为 0、平滑与淡化都已静止、延迟线里也已没有低切作用过的样本（单声道模块只看 ①）；
        // 另外峰值守护的前瞻环里也已没有「①② 处理后 ≠ 干路」（逐位）或③ 的增量 δ ≠ 0 的样本。
        bool isPassThrough() const noexcept;

    private:
        // ---- 实现（v0.1.7.0 T-012 代理 A1；阶段 4 根治修订由代理 P1 改为线性相位收单 + 固定延迟 D）----
        //
        // 实现要点（细节见 ClayModule.cpp 各函数的注释）：
        //  - 逐样本一条路径：① 两声道各两级高通 → 按 cutMix 选择 / 淡化 → M/S → 写延迟线与 S 历史环 → ② 低通（淡化中两套）→
        //    S' = S_d − a·S_low → 解码 L' = M_d ± S' → 旁通淡化（干路 = float 纯延迟线）→ ④；③ S' 的高架只用来算 δ = S'' − S'
        //    （T-032：不进守护），按段暂存 ±δ·(1 − bypass) 交给 ④ 在守护后的输出上加回。
        //  - 延迟线共用一个读写位置（容量正好 D：先读出 D 个样本之前写的，再写本样本）：L / R 输入原样（float）、M（double）、
        //    「本样本的低切是否参与」的标记。S 另有长 2D + 1 的历史环：S_d、两套低通各自对齐后的输入、切换时的预热都从它读。
        //  - 低通 LP_L 的输出比输入晚 2(L − 1)；把它的输入取成 S 延迟 D − 2(L − 1)（从历史环读），输出就正好对齐 S_d——
        //    与「LP 之后再延迟」数学上相同，但不需要按 L 变长的第二条延迟线，两套低通切换时也各自对齐。
        //  - ①② 的输出逐位取纯延迟的条件（逐样本）：延迟后的这个样本没经过低切（标记为 0）、a 此刻为 0；完全旁通时同样取纯延迟线。
        //    高架此刻逐位直通且无余振时 δ 精确为 0（显式取 0，不算 S'' − S'：±Inf 样本会得 NaN）——三者都满足才是整个模块的逐位纯延迟。
        //    段首高架就已逐位直通且无余振（widthIdle）时整段都是这样（直通时不跑高架，状态保持 0）：这种段走 processStereo<false>，
        //    整段不碰③，④ 也收到「δ 全为 0」——中性、只开①② 时逐样本做的事与 T-032 之前相同（性能对照见 T-032 实现报告）。
        //  - 系数在从 reset 起每 64 样本一格的固定网格上重算；低通切换也只在网格点开始 / 收尾；平滑 / 淡化都按「目标改变以来的
        //    样本数」直接算值——输出与块长无关（参数在同一样本位置改变时，任何块长逐位相同；前提：编译器不合并乘加，ADR-013）。

        //==============================================================================
        // 转置直接 II 型双二阶（double 精度，与 BalanceModule / ColorModule 同一写法）：系数按 a0 归一。
        struct BiquadCoefficients
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        struct BiquadState
        {
            double s1 = 0.0, s2 = 0.0;
        };

        // 固定时长（20 ms）的线性斜坡。值按「目标改变以来走过的样本数」直接算、不逐样本累加：同一个绝对样本位置上的值
        // 与切块方式、逐样本取值还是整段跳过都无关。语义同 juce::SmoothedValue 线性型：目标改变后的第 1 个样本已走 1 步，
        // 第 length 个样本精确落在目标上。
        struct LinearRamp
        {
            double from = 0.0;        // 本次斜坡的起点
            double to = 0.0;          // 目标
            int length = 1;           // 斜坡长度（样本，≥ 1；prepare 时按 20 ms 定）
            int position = 1;         // 目标改变以来已走过的样本数，封顶 length（= length 即已到达）
            double stepScale = 1.0;   // 1 / length

            void setLength (int numSamples) noexcept;          // 同时落到目标
            void setTarget (double newTarget) noexcept;        // 与当前目标相同则不重启（宿主每块都会把同一个值再设一遍）
            void restart (double start, double target) noexcept;   // 无条件从 start 起走一段完整斜坡到 target
            void snapToTarget() noexcept;
            void advance (int numSamples) noexcept;
            double currentValue() const noexcept;              // 最近处理过的那个样本用的值（新斜坡的起点）
            double valueAt (int offset) const noexcept;        // 接下来第 offset 个样本（0 起）用的值
            bool reachesTargetAt (int offset) const noexcept   { return position + offset + 1 >= length; }
            bool isRamping() const noexcept                    { return position < length; }
        };

        // 频率参数在对数域平滑：ln f 走 LinearRamp；走完时直接给目标频率本身（不经 exp (ln f) 的舍入）。只用于低切。
        struct FrequencyRamp
        {
            explicit FrequencyRamp (double initialHz) noexcept;

            void setTarget (double hz) noexcept;               // hz > 0（调用方已钳制）
            void snapToTarget() noexcept                       { logHz.snapToTarget(); }
            void advance (int numSamples) noexcept             { logHz.advance (numSamples); }
            double valueAt (int offset) const noexcept;

            LinearRamp logHz;
            double targetHz = 0.0;
        };

        static double processBiquad (double x, const BiquadCoefficients& c, BiquadState& s) noexcept;
        static void snapState (BiquadState& s, double threshold) noexcept;   // 低于门槛归零；非有限整级清零
        static BiquadCoefficients designHighpass (double fs, double frequency, double q) noexcept;
        BiquadCoefficients designWidthShelf (double gainDb) const noexcept;

        //==============================================================================
        void applyTargets() noexcept;                  // params → 各斜坡的目标、收单低通的目标长度
        void designCutFilters (double frequency) noexcept;
        void designWidthFilter (double gainDb) noexcept;
        void updateCoefficients() noexcept;            // 网格点：按平滑值重算系数、状态归零 / 清洗、收单低通的切换
        void updateLowpassSwitch() noexcept;           // 网格点：淡化走完就让新低通转正；预热追上就开始淡化；目标长度变了就开始下一次切换
        void startLowpassSwitch (int newLength) noexcept;   // reset 另一套低通到新长度、记下欠着的预热样本数（在处理循环里分摊喂）
        void advanceRamps (int numSamples) noexcept;
        void clearDelayLines() noexcept;
        bool stagesIdle (int channelsToUse) const noexcept;   // 三级此刻都不作用、延迟线里也没有低切过的样本，且整段保持
        bool widthIdle() const noexcept;                      // ③ 此刻逐位直通且无余振：本段每个样本的 δ 都精确为 0（见 process）

        // 原地；numSamples ≤ coefficientUpdateInterval。widthActive = false 只用于 widthIdle() 的段：整段不碰③（不跑高架、不算 δ），
        // 逐样本做的事与 T-032 之前相同；true 逐样本判断高架是否直通并算 δ。两份对 widthIdle() 的段输出逐位相同（δ 全为 0）。
        template <bool widthActive>
        void processStereo (float* left, float* right, int numSamples) noexcept;
        void processMono (float* samples, int numSamples) noexcept;                // 原地；同上

        //==============================================================================
        // ④ 峰值守护（ADR-014，算法见文件头）。逐样本、状态与块长无关；prepare 按采样率分配，process / reset 不分配。
        // T-024 起按段处理（状态在段内放局部变量），g 路径静止在全 1 时跳过（见 ClayModule.cpp 的 PeakGuard::processBlock），
        // 输出、统计、代价格与改前逐样本的实现逐位相同。
        struct PeakGuard
        {
            void prepare (double fs, int channelCount);
            void reset() noexcept;

            // 送进一段样本（核心输出时刻，段长 ≤ 64）：dry[c] = 干路（输入延迟 D），io[c] 进 = ①② 与旁通淡化之后、
            // 出 = 各自 G 个样本之前那个样本的守护后输出 + 那个样本的 δ（③ 的增量，T-032 起不进守护、在这里加回；为 0 时不做加法，
            // 保住 ±0 的逐位）。delta[c] = 本段各样本的 δ；nullptr = 本段全为 0（单声道恒为 nullptr）。callerChannels = 调用方的
            // 声道数（1 或 2），不超过 prepare 的声道数时按它处理（立体声模块偶尔收到单声道缓冲也不越界）。不叫 numChannels：外层
            // ClayModule 有同名成员（clang -Wshadow）。
            void process (const float* const* dry, float* const* io, const float* const* delta, int callerChannels, int numSamples) noexcept;

            // 前瞻环（最近 G + 1 个样本）里还有「处理后 ≠ 干路」（逐位）或 δ ≠ 0 的样本：它们还要再输出出去（isPassThrough 用）。
            bool hasProcessedInRing() const noexcept   { return juce::jmax (lastProcessed, lastIncrement) >= sampleCount - ringLength; }

            static int windowFor (double fs) noexcept;     // W
            static int attackFor (double fs) noexcept;     // A

            // withIncrement = false：本段送来的 δ 全为 0、且本段要输出的样本都晚于最近一个 δ ≠ 0 的样本——整段不碰 δ（与 T-032 之前的
            // 实现逐字相同：中性、只开①② 时不为 δ 多花一点）；true：存 δ、输出时加回。两条路对同一段输入的输出逐位相同（见 process）。
            template <int numChannels, bool withIncrement>
            void processBlock (const float* const* dry, float* const* io, const float* const* delta, int numSamples) noexcept;

            int channels = 2;
            int window = 0;                    // W
            int attack = 0;                    // A
            int lookahead = 0;                 // G = W + A
            double envelopeRetain = 0.0;       // P 每样本的衰减系数
            double releaseStep = 1.0;          // g 的线性释放：每样本最多回升这么多

            // 前瞻环（长 G + 1：最新样本 m 到输出样本 m − G）：一格放同一样本两声道的干路、①② 处理后与 ③ 的增量 δ，读写都只碰一处。
            // δ 存 float（一格 24 字节，T-024 的缓存足迹不翻倍；δ 是小增量，float 量化远在可闻之外，且 δ ≠ 0 时输出本来就没有逐位契约）；
            // δ 只在带 δ 的那条路里写（不带 δ 的那条路里格上的 δ 是旧值，读的时候按 incrementsFrom 当 0）。
            struct Frame
            {
                std::array<float, 2> dry {}, wet {}, delta {};
            };

            std::vector<Frame> ring;
            int ringLength = 1;
            int writeIndex = 0;
            std::int64_t lastProcessed = std::numeric_limits<std::int64_t>::min();   // 最近一个「处理后 ≠ 干路」（逐位）的样本号；prepare / reset 后在环之前
            std::int64_t lastIncrement = std::numeric_limits<std::int64_t>::min();   // 最近一个 δ ≠ 0 的样本号（T-032）；同上
            std::int64_t incrementsFrom = 0;   // 带 δ 的那条路这一轮连续跑的起点：从这个样本号起环里的格都存了 δ，更早的样本 δ 都是 0
            bool incrementsWritten = false;    // 上一段走的是带 δ 的那条路

            // 单调队列（固定容量的环，容量取 2 的幂、下标按位与回绕）：两个分段滑动窗（见下）里完整段的最值，每段进一次。
            // 处理一段时队首位置与个数放在局部的 View 里（留在寄存器），段尾写回。
            struct Entry
            {
                std::int64_t index = 0;
                double value = 0.0;
            };

            struct MonotonicQueue
            {
                void prepare (int capacityAtLeast);
                void clear() noexcept                 { head = 0; count = 0; }

                std::vector<Entry> entries;
                int mask = 0, head = 0, count = 0;
            };

            struct QueueView
            {
                Entry* entries;
                int mask, head, count;

                void pushMax (std::int64_t index, double value) noexcept;   // 单调递减（队首 = 最大）
                void pushMin (std::int64_t index, double value) noexcept;   // 单调递增（队首 = 最小）
                void dropThrough (std::int64_t last) noexcept;              // 丢掉编号 ≤ last 的（可以丢空）
                double front() const noexcept         { return entries[head].value; }
            };

            // 两个滑动窗都分段算（T-024；段长 S = 2^segmentShift ≤ min(2W, A)、封顶 64，按从 reset 起的样本号 m 对齐，两个窗共用段界）：
            // 窗 = 最老样本所在段的「从它到段尾」+ 中间的完整段 + 当前段的「从段首到最新样本」。
            //   局部峰值 E：干路量级在 [m − 2W, m] 的最大；需求的前瞻最小 h：g_req 在 [m − A, m] 的最小（g_req 按算出它的样本号 m 存）。
            int segmentShift = 0;
            std::vector<double> magnitudes;    // 量级环（长 2 的幂 > 2W + S）：当前段存原始量级，段一完整就地换成「到段尾的最大」
            int magnitudeMask = 0;
            double prefixMax = 0.0;            // 当前段从段首到最新样本的最大
            MonotonicQueue peakSegments;       // 完整段的最大值（编号 = 段号）
            std::vector<double> requirements;  // g_req 环（长 2 的幂 > A + S）：同上，段一完整就地换成「到段尾的最小」
            int requirementMask = 0;
            double prefixMin = 1.0;            // 当前段从段首到最新样本的最小
            MonotonicQueue requirementSegments;   // 完整段的最小值（编号 = 段号）

            std::vector<double> releaseRing;   // 长 A + 1：释放后的 r，滑动平均用
            int releaseIndex = 0;
            double releaseSum = 0.0;
            int releaseBelowOne = 0;           // 窗里 r < 1 的个数：0 时 g 精确为 1
            double released = 1.0;             // 上一个 r
            bool gainIdle = true;              // g 路径静止在全 1（releaseBelowOne == 0 且 released == 1）：需求为 1 的样本可以跳过它
            double envelope = 0.0;             // P
            std::int64_t sampleCount = 0;      // 从 reset 起的样本号

            PeakGuardStats stats;

            // 代价格（ADR-014 压力反馈）：格长 round(0.01 fs)，在 k（界 B 已知的样本）的时间轴上、从 reset 起的固定网格。
            int costBlockLength = 1;
            int costBlockPosition = 0;
            double costBlockMaxRatio = 0.0;    // 本格 max |y| / B（未守护的处理后相对界）
            double pressureSumDb = 0.0;        // 本次 process() 走完的格的 dB 之和（每次 process 开头清零）
            int pressureBlocks = 0;
        };

        //==============================================================================
        Params params;                          // setParams 钳制后的实际值
        double sampleRate = 0.0;
        int numChannels = 0;                    // prepare 时的声道数，钳到 1 ～ 2（插件只做 1 进 1 出与 2 进 2 出；多出的声道不处理）
        int samplesUntilCoefficientUpdate = 0;  // 到下一个系数重算网格点还有几个样本（从 reset 起每 64 样本一格，跨块延续）
        int coreDelay = 0;                      // D：①～③ 的延迟线长度（prepare 定，之后不变）
        int latencySamples = 0;                 // D + G（上报的固定延迟）

        // 延迟线（各长 D，共用读写位置 delayPosition）。
        std::array<std::vector<float>, 2> dryDelay;     // L / R 输入原样：逐位纯延迟与旁通的干路
        std::vector<double> midDelay;                   // M（单声道：① 选择之后的样本）
        std::vector<std::uint8_t> cutFlagDelay;         // 1 = 该输入样本的 ① 输出选择不在干路（低切参与了）
        int delayPosition = 0;                          // 本样本读出（D 个样本之前写的）并写入的位置
        int cutSamplesInDelay = 0;                      // cutFlagDelay 里 1 的个数（isPassThrough 用）

        std::vector<double> sideHistory;                // S 的历史环，长 2D + 1（最早要读到 D + 2(L − 1) ≤ 2D 个样本之前）
        int sideWritePosition = 0;                      // 下一个 S 写入的位置

        // ① 低切：L/R 各两级高通（Q1、Q2），两声道同一组系数。
        std::array<BiquadCoefficients, 2> cutCoefficients {};
        std::array<std::array<BiquadState, 2>, 2> cutStates {};   // [声道][级]
        double cutHzForCoefficients = 0.0;      // 当前系数对应的截止频率，没变就不重算
        FrequencyRamp cutFrequencyRamp { static_cast<double> (minLowCutHz) };   // 影子滤波器的截止频率：关着时停在上次开着的频率（初始 10 Hz）
        LinearRamp cutMixRamp;                  // 输出选择：0 = 未低切的干路、1 = 低切后（逐样本取值，输入时刻）

        // ② 线性相位收单：两套低通轮流用（activeLowpass 是正在用的那套；切换时另一套重建成新长度、预热、两套淡化）。
        std::array<LinearPhaseLowpass, 2> lowpasses;
        int activeLowpass = 0;
        int lowpassMaxLength = 2;               // LinearPhaseLowpass::maxLengthFor (fs)
        int targetLowpassLength = 2;            // lengthFor (fs, params.monoHz)，钳到 [2, lowpassMaxLength]
        bool lowpassWarming = false;            // 另一套正在分摊预热（输出还不用）
        int lowpassBacklog = 0;                 // 预热还欠着的历史样本数（到 0 = 已追上，下一个网格点开始淡化）
        bool lowpassFading = false;             // 正在从 activeLowpass 淡化到另一套
        LinearRamp lowpassFade;                 // 0 = 正在用的那套、1 = 新的那套（逐样本取值）
        LinearRamp monoAmountRamp;              // a（逐样本取值，输出时刻）

        // ③ 加宽：S' 的 2 kHz 高架（斜率 S = 1）。
        double shelfCosW0 = 1.0;                // 与增益无关的部分（prepare 时按采样率定）
        double shelfAlpha = 0.0;
        BiquadCoefficients shelfCoefficients;
        BiquadState shelfState;
        double widthDbForCoefficients = 0.0;    // 当前系数对应的增益（dB）；精确为 0 = 系数逐位直通
        LinearRamp widthRamp;                   // widthDb（网格点取值）

        // 旁通：与处理后信号 20 ms 交叉淡化（0 = 处理后、1 = 干路；逐样本取值，输出时刻）。
        LinearRamp bypassRamp;

        // ④ 峰值守护。
        PeakGuard peakGuard;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClayModule)
    };
}
