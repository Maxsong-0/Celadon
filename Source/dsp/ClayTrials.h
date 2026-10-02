/*
  ==============================================================================

    ClayTrials.h
    修坯 Clay 的先试后用（ADR-012，设计规格-Clay-根治修订 3.2；T-043 守护混合损失候选，ADR-027）。T-048 阶段 1 从 MusicAnalyzer 的
    「五」节原样抽出：算法、每一步的运算顺序与结果都不变（逐位），分析器在原来的位置、按原来的顺序调它。

    一、为什么：最小相位 IIR 在低频的相位旋转会把已经很密 / 削平的波形的峰值重新错开冒出来（真歌试渲染：低切让成品歌峰值 +4 dB、
       末级限幅多压、响度反而 −0.9 LU）；决策不能只看特征，要先量「这首歌真做了之后峰值怎样」。把候选处理跑在链入口的原始样本上，
       量 100 ms 块峰值 P99 的变化（峰值代价）与守护的混合损失。

    二、候选（各自单独作用在原始输入上；系数公式与 ClayModule 一致，double）：
         0      低切：L、R 各两级 RBJ 高通（4 阶 Butterworth，Q 同 ClayModule），20 Hz（自动低切目标的上限）；
         1 ～ 4 收单：线性相位收单 a = 1，分频点 clayMonoCandidateHz（60 / 80 / 120 / 160 Hz），与 Clay 同一个 LinearPhaseLowpass：
                S' = S 延迟 D_c − LP(S)、M 延迟 D_c，L' = M_d + S'、R' = M_d − S'。D_c = 2(L_c − 1) 是候选自己的群延迟——
                只需与 LP 的输出对齐，不必用 Clay 的最大延迟；
         5      加宽：S 过 2 kHz 高架（斜率 S = 1）+1.5 dB（自动加宽上限），L' = M + S''、R' = M − S''。
       max(|M + x|, |M − x|) = |M| + |x|，浮点下也精确（较大的那个本来就是 |M| + |x| 的一次舍入），收单与加宽直接按它算块峰值。
       混合损失候选（T-043）：低切（频率 clayBlendCandidateHz：10 / 20 Hz；20 Hz 那个直接用上面低切候选的输出）+ ClayModule 的峰值守护
       （ClayModule::PeakGuard，与链里同一份实现；不搭整个 ClayModule——收单低通、加宽高架、延迟线都用不上）。守护的抽头给出逐输出
       样本的两份 K 加权能量（「本该」= g 当 1 时、「实际」= 守护后），按「输出样本号 − 守护前瞻 G」归到块——与收单候选按 D_c 对齐
       同一做法。守护按从起跑算的 64 样本网格切段与定「量不量」，求和顺序与宿主怎么切块无关。

    三、块网格与交齐：
       - 块 = 整数个起音组（分析器的组网格：从重启起算、与块边界无关；44.1 / 48 / 96 / 192 kHz 下正好 100 ms = blockSeconds），
         块号 k 的块在网格上是 [k·块长, (k + 1)·块长)。块里每个组都「算数」才是有效块（noteGroup 交来；两种口径见第五节），另外候选
         从零状态起跑后 warmUpSeconds（250 ms）内开始的块不算（IIR 起跑瞬态：20 Hz 那级高通振铃约 21 ms 的时间常数，250 ms 后低于 −100 dB）。
       - 候选输出晚 D 个样本：它的第 k 块取输出的 [k·块长 + D, (k + 1)·块长 + D)，与原始的第 k 块是同一段音频（延迟补偿）——
         纯单声道素材（S = 0）收单后逐位就是延迟了的原始，所以代价精确为 0。
       - 原始、6 个峰值候选、2 个混合损失候选都交了这一块才交齐（writers = 9 个写者），交齐的块交给 BlockReceiver（按块号先后、每块一次）。
         最长的延迟短于一块，任何时刻最多两块没交齐：2 个记录槽按块号奇偶轮用。

    四、调用顺序（分析器 runOnsets / processGroup 里；每个样本仍是「起音 / 真次声 → 候选 → （组末）结组」的先后，与逐样本处理逐位相同）：
         prepare (sampleRate, groupLength)      消息线程：系数、延迟、块网格；按采样率分配收单低通、M / S 延迟环、两份守护。之后要求重新起跑。
         requestRestart()                       下一次 process 之前从零状态重新起跑（开始学习、组网格重启、候选恢复运行时）。
         每一段（同一个组里连续的 ≤ maxSegmentSamples 个样本）：
             process (left, right, stereo, length, position, receiver)    position = 段首在组网格上的样本序号（重新起跑时用来对齐块网格）
         每个组结束（groupsSinceRestart 已经 + 1，按组网格的有效门判定这一组算不算）：
             noteGroup (groupsSinceRestart, countsLearning, countsLive, receiver)，然后 snapStates()（组末 IIR 状态归零，防非规格化数）
       不在跑的时候（分析器：实时模式关着且不在学习）两者都不调；恢复运行前 requestRestart。坏样本（NaN / Inf）：只跑它之前的样本，
       候选从下一段起从零重来（自己置 restartPending）。

    五、交齐的块（Block）：原始与各候选的块峰值、两个混合损失候选的两份 K 加权能量、块号，另有两种有效口径——
         valid      学习口径（= 原来的 TrialRecord::valid）：候选已过预热，块里每个组都在学习区间内且在有效音频门上；
         liveValid  实时口径（T-048）：候选已过预热，块里每个组都在有效音频门上（不看学习区间）。
       哪些块进哪一份累计由调用方定（分析器：学习口径且交齐时还在学习的块进学习累计与 House 段落账，实时口径的块进实时段落账）。

    六、累计与代价（静态函数，纯逻辑，分析器的学习累计、House 段落账与画像合并共用）：
         insertTop / accumulate  每一路只留最大的 topCount（8）个块峰值（有序插入）、混合损失两份能量求和；
         percentileOfTop         线性插值的 P99（定义同 numpy.percentile 默认；逐位口径以「存全部 + nth_element + 同一插值」为准——numpy 的
                                 lerp 在 t ≥ 0.5 时换了写法，末位可能不同，审核 V4-04）：位置 0.99·(n − 1) 两侧的顺序统计量插值。n ≤ capacity（512）
                                 时 P99 只用到从大往小数第 7 个以内的值，所以每一路只留最大的 8 个就能逐位得到与「存下全部块峰值再 std::nth_element」
                                 相同的结果——后者每记一块要对 7 × 最多 512 个数做选择，192 kHz / 32 样本块下是单块约 10 µs 的尖峰；
         costsOf                 代价 = 20·log10(P99 候选) − 20·log10(P99 原始)（dB，正 = 峰值变高；候选按原始的 −120 dB 封底）；
                                 混合损失 = 10·log10(Σ本该 / Σ实际)（两份都 > 0 才知道）；有效块满 minimumSeconds（10 s）才有效，
                                 观测充分度 = 块时长 / fullSeconds（封顶 1）。confidence 由调用方按代表性定。

    逐位约定：运算顺序、舍入点（float 存低切输出、double 累计）与原 MusicAnalyzer 逐字相同；结果只取决于送进来的样本序列、组网格位置与
    noteGroup 的判定，与宿主怎么切块无关。依赖 ADR-013（不合并乘加：GCC -ffp-contract=off，任何构建不许 -ffast-math）。
    实时约束：prepare 分配全部定长状态；process / noteGroup / snapStates / requestRestart 不分配、不加锁、不抛异常。

  ==============================================================================
*/

#pragma once

#include "ClayModule.h"
#include "GlazeTypes.h"
#include "LinearPhaseLowpass.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace celadon::glaze
{
    //==============================================================================
    // 修坯分析的双二阶（转置直接 II 型，double；系数按 a0 归一）：分析器的真次声低通与 ClayTrials 的候选共用
    // （原 MusicAnalyzer 的 makeLowpass / makeHighpass / makeHighShelf / runBiquad，逐字搬来）。
    struct ClayBiquad
    {
        struct Coefficients
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        struct State
        {
            double s1 = 0.0, s2 = 0.0;
        };

        // 长时间静音里一阶 / 二阶状态会一路衰减进非规格化数（x86 没开 FTZ 时慢几十倍）：低于 −300 dB 就归零
        // （与分析器的 snapThreshold、各模块的 snapToZero 同一门槛；这么小的值对任何读数都没有影响）。
        static constexpr double snapThreshold = 1.0e-30;

        // RBJ 二阶低通（Audio EQ Cookbook 的 LPF），按 a0 归一。1 − cos w0 写成 2·sin²(w0 / 2)：16 Hz / 192 kHz 时 1 − cos w0 ≈ 1.4e-7，
        // 直接相减会丢掉一半以上的有效数字。四级（Q = 1 / (2·cos((2k − 1)·π/16))）串联 = 双线性变换的 8 阶 Butterworth：
        // |H|² = 1 / (1 + (tan(πf / fs) / tan(πf₀ / fs))¹⁶)（测试据此算解析值）。
        static Coefficients lowpass (double rate, double frequency, double q) noexcept
        {
            const auto w0 = juce::MathConstants<double>::twoPi * frequency / rate;
            const auto halfSine = std::sin (0.5 * w0);
            const auto oneMinusCos = 2.0 * halfSine * halfSine;
            const auto alpha = std::sin (w0) / (2.0 * q);
            const auto a0 = 1.0 + alpha;

            Coefficients c;
            c.b0 = 0.5 * oneMinusCos / a0;
            c.b1 = oneMinusCos / a0;
            c.b2 = c.b0;
            c.a1 = -2.0 * std::cos (w0) / a0;
            c.a2 = (1.0 - alpha) / a0;
            return c;
        }

        // RBJ 二阶高通：与 ClayModule::designHighpass 同式（频率钳在 0.45 fs 以下）。先试后用的低切候选用它，两级 Q 同 ClayModule。
        static Coefficients highpass (double rate, double frequency, double q) noexcept
        {
            const auto w0 = juce::MathConstants<double>::twoPi * juce::jmin (frequency, 0.45 * rate) / rate;
            const auto cosW0 = std::cos (w0);
            const auto alpha = std::sin (w0) / (2.0 * q);
            const auto a0 = 1.0 + alpha;

            Coefficients c;
            c.b0 = (1.0 + cosW0) * 0.5 / a0;
            c.b1 = -(1.0 + cosW0) / a0;
            c.b2 = c.b0;
            c.a1 = -2.0 * cosW0 / a0;
            c.a2 = (1.0 - alpha) / a0;
            return c;
        }

        // RBJ 高架（斜率 S = 1）：与 ClayModule::designWidthShelf 同式——A = 10^(dB/40)，平台增益 A²，α = sin(w0)/2 · √2
        // （S = 1 时根号里恒为 2）；频率钳在 0.45 fs 以下。先试后用的加宽候选用它。
        static Coefficients highShelf (double rate, double frequency, double gainDb) noexcept
        {
            const auto w0 = juce::MathConstants<double>::twoPi * juce::jmin (frequency, 0.45 * rate) / rate;
            const auto cosW0 = std::cos (w0);
            const auto alpha = std::sin (w0) / 2.0 * std::sqrt (2.0);
            const auto a = std::pow (10.0, gainDb / 40.0);
            const auto aPlus1 = a + 1.0;
            const auto aMinus1 = a - 1.0;
            const auto twoSqrtAAlpha = 2.0 * std::sqrt (a) * alpha;

            const auto b0 =        a * (aPlus1 + aMinus1 * cosW0 + twoSqrtAAlpha);
            const auto b1 = -2.0 * a * (aMinus1 + aPlus1 * cosW0);
            const auto b2 =        a * (aPlus1 + aMinus1 * cosW0 - twoSqrtAAlpha);
            const auto a0 =            (aPlus1 - aMinus1 * cosW0 + twoSqrtAAlpha);
            const auto a1 =      2.0 * (aMinus1 - aPlus1 * cosW0);
            const auto a2 =            (aPlus1 - aMinus1 * cosW0 - twoSqrtAAlpha);

            Coefficients c;
            c.b0 = b0 / a0;
            c.b1 = b1 / a0;
            c.b2 = b2 / a0;
            c.a1 = a1 / a0;
            c.a2 = a2 / a0;
            return c;
        }

        static double run (double x, const Coefficients& c, State& s) noexcept
        {
            const auto y = c.b0 * x + s.s1;
            s.s1 = c.b1 * x - c.a1 * y + s.s2;
            s.s2 = c.b2 * x - c.a2 * y;
            return y;
        }

        static void snap (State& s) noexcept
        {
            if (std::abs (s.s1) < snapThreshold)
                s.s1 = 0.0;

            if (std::abs (s.s2) < snapThreshold)
                s.s2 = 0.0;
        }
    };

    //==============================================================================
    class ClayTrials
    {
    public:
        // ---- 口径常数（分析器的学习累计、SegmentLedger 的逐段记账与 House 画像合并都按它们；段落账的同名常数由分析器 static_assert 对齐）----
        static constexpr int numMono = (int) clayMonoCandidateHz.size();     // 收单候选（分频点 = clayMonoCandidateHz）
        static constexpr int numCandidates = 2 + numMono;                    // 峰值候选：低切、收单 ×4、加宽
        static constexpr int lowCutSlot = 0, firstMonoSlot = 1, widthSlot = 1 + numMono;
        static constexpr int numSeries = 1 + numCandidates;                  // 累计的路数：[0] 原始、[1 + c] 候选 c
        static constexpr int numBlend = (int) clayBlendCandidateHz.size();   // 守护混合损失候选（低切频率 = clayBlendCandidateHz）
        static constexpr int sharedCutBlend = 1;                             // 这个混合损失候选的低切 = 峰值代价的低切候选：直接用它的输出
        static constexpr int writers = 1 + numCandidates + numBlend;         // 交齐一块要的写入：原始 + 峰值候选 + 混合损失候选

        static constexpr double blockSeconds = 0.1;        // 块长的目标（取整数个组）
        static constexpr double warmUpSeconds = 0.25;      // 候选从零状态起跑后这么久之内开始的块不算（= 分析器起音的重启预热，分析器 static_assert）
        static constexpr int capacity = 512;               // 一份累计最多并入的块数（51.2 s：聆听最长 30 s ≈ 300 块，余量给有效门口径的差别）
        static constexpr int topCount = 8;                 // 每一路留最大的这么多个块峰值
        static constexpr double percentile = 0.99;
        static constexpr double minimumSeconds = 10.0;     // 有效块满这么久代价才有效
        static constexpr double fullSeconds = 10.0;        // 观测充分所需的有效块时长
        static constexpr double lowCutHz = 20.0;           // 低切候选（自动低切目标的上限）
        static constexpr int cutStages = 2;                // 低切候选：两级 RBJ 高通（4 阶 Butterworth）
        static constexpr std::array<double, (size_t) cutStages> cutQ { 0.54119610014619698, 1.3065629648763766 };   // 同 ClayModule 的 cutFirstQ / cutSecondQ
        static constexpr double widthShelfHz = 2000.0;     // 同 ClayModule::widthShelfHz
        static constexpr double widthDb = 1.5;             // 自动加宽上限（GlazeConstants 的 clayWidthMaxDb 出厂值）
        static constexpr double peakFloorRatio = 1.0e-6;   // 候选 P99 按原始 P99 的 −120 dB 封底（例如低切把纯直流削成 0），代价不出 −∞
        static constexpr int maxSegmentSamples = 128;      // process 一次最多的样本数（= 分析器的最大组长）

        static_assert (clayBlendCandidateHz[(size_t) sharedCutBlend] == (float) lowCutHz, "共用的低切候选频率必须相同");

        // n ≤ 容量时，P99 的下侧顺序统计量从大往小数是第 n − 1 − ⌊0.99·(n − 1)⌋ ≤ 0.01·(n − 1) + 1 个（0 起），上侧再大一个。
        static_assert ((1.0 - percentile) * (double) (capacity - 1) + 1.0 < (double) (topCount - 1),
                       "每一路留的最大值个数必须覆盖容量内 P99 用到的两个顺序统计量");

        using Top = std::array<double, (size_t) topCount>;          // 一路最大的块峰值，降序（前 min(块数, topCount) 个有效）
        using TopSeries = std::array<Top, (size_t) numSeries>;      // [0] 原始、[1 + c] 候选 c
        using BlendSums = std::array<double, (size_t) numBlend>;    // 每个混合损失候选一份能量和

        // 交齐的一块（第五节）。
        struct Block
        {
            std::int64_t index = -1;                                  // 块号（组网格上从重启起数；块起点 = 网格原点 + index × 块长）
            double original = 0.0;                                    // 原始块峰值 max(|L|, |R|)
            std::array<double, (size_t) numCandidates> candidates {}; // 各峰值候选（延迟补偿后同一段音频）的块峰值，下标 = 候选槽
            BlendSums blendIntended {}, blendOutput {};               // 各混合损失候选这一块的两份 K 加权能量（本该 / 实际）
            bool valid = false;                                       // 学习口径（第五节）
            bool liveValid = false;                                   // 实时口径（第五节）
        };

        // 交齐的块的接收者：每交齐一块调一次（按块号先后；无效的块也交，两个口径的标志在 Block 里）。在 process / noteGroup 里调，
        // 实现不许分配、加锁，也不许回调本对象。
        class BlockReceiver
        {
        public:
            virtual ~BlockReceiver() = default;
            virtual void trialBlockCompleted (const Block& block) noexcept = 0;
        };

        // 一份累计按 P99 算出的代价（字段名同画像；confidence 都是 0，由调用方按代表性定）。
        struct Costs
        {
            Feature originalP99Db;                                   // 原始块峰值的 P99（dBFS）：代价的基准（AuxiliaryFeatures::trialPeakP99Db）
            Feature lowCutPeakCostDb;
            std::array<Feature, (size_t) numMono> monoPeakCostDb {};
            Feature widthPeakCostDb;
            std::array<Feature, (size_t) numBlend> blendLossDb {};   // 画像的 lowCutBlendLossDb
            float measured = 0.0f;                                   // 观测充分度 = 块时长 / fullSeconds（封顶 1）
        };

        ClayTrials() = default;

        // 消息线程：按采样率与组长定系数、延迟、块网格，分配收单低通、M / S 延迟环与两份守护。之后要求重新起跑（下一次 process 前清零）。
        void prepare (double sampleRate, int groupLength);

        // ---- 音频线程（见文件头第四节的调用顺序）----
        void requestRestart() noexcept           { restartPending = true; }
        bool isRestartPending() const noexcept   { return restartPending; }

        // 一段 = 同一个组里连续的 length（≤ maxSegmentSamples）个样本。left / right = 原始 L / R（单声道时传同一个声道、stereo = false）；
        // position = 段首在组网格上的样本序号（从重启起算）。
        void process (const float* left, const float* right, bool stereo, int length, std::int64_t position, BlockReceiver& receiver) noexcept;

        // 原始一侧：一个组结束（groupsSinceRestart = 组网格上已结束的组数，含这一组）。countsLearning / countsLive = 这一组按两种口径
        // 算不算（第五节）；块的最后一组结组时交出原始的这一块。等着重新起跑时不记。
        void noteGroup (std::int64_t groupsSinceRestart, bool countsLearning, bool countsLive, BlockReceiver& receiver) noexcept;

        // 组末：候选 IIR 状态里低于门槛的归零（与真次声低通同一门槛、同一网格，结果仍与分块无关）。
        void snapStates() noexcept;

        // 还没交齐的最早一块的起点（组网格上从重启起算的样本位置）。ran = 组网格上已跑过的样本数（组数 × 组长 + 当前组已填的样本）：
        // 第 k 块的最后一笔在网格位置 (k + 1)·块长 + 最长延迟交齐。分析器拿它判段落账的段什么时候交齐了先试后用的数据。
        std::int64_t pendingBlockStart (std::int64_t ran) const noexcept;

        int getBlockLength() const noexcept      { return blockLength; }   // 块长（样本）
        int getBlockGroups() const noexcept      { return blockGroups; }   // 块长（组）
        int getMaximumDelay() const noexcept     { return maxDelay; }      // 各写者里最长的延迟（收单 D_c 与守护前瞻 G 取大）
        int getDelay (int slot) const noexcept   { return delays[(size_t) slot]; }   // 峰值候选的延迟（低切 / 加宽 0，收单 D_c）
        int getGuardDelay() const noexcept       { return guardDelay; }    // 混合损失候选的延迟（守护前瞻 G）
        std::int64_t getWarmUpSamples() const noexcept   { return warmUpSamples; }

        // ---- 累计与代价（第六节；纯函数）----
        // top[0 .. filled) 降序、最多留 topCount 个：插入 value，挤掉最小的。
        static void insertTop (Top& top, int filled, double value) noexcept;
        // 一块并进一份累计：每一路插进最大的 topCount 个（filled = 并入之前的块数），两份能量各自求和。调用方自己把块数 + 1。
        static void accumulate (TopSeries& top, BlendSums& intended, BlendSums& output, int filled, const Block& block) noexcept;
        // n（≥ 1）块的线性插值 P99：top 降序，第 j 小（0 起）= top[n − 1 − j]。
        static double percentileOfTop (const Top& top, int n) noexcept;
        // count 块的累计（块长 blockSamples 个样本、采样率 rate）算出的代价与观测充分度。
        static Costs costsOf (const TopSeries& top, const BlendSums& intended, const BlendSums& output, int count,
                              int blockSamples, double rate) noexcept;

    private:
        // 一块的交齐记录：各写者写一次，写满 writers 次就交出去，再腾出槽位。
        struct Record
        {
            Block block;
            int writes = 0;
        };

        void restart (std::int64_t position) noexcept;
        Record& recordFor (std::int64_t index) noexcept;
        void countWrite (Record& record, BlockReceiver& receiver) noexcept;

        // ---- 配置（prepare）----
        double sampleRate = 48000.0;
        int blockGroups = 1, blockLength = 1;                    // 一块 = blockGroups 个整组 = blockLength 个样本
        std::int64_t warmUpSamples = 0;                          // 候选从零状态起跑后，这么多样本之内开始的块不算
        std::array<ClayBiquad::Coefficients, (size_t) cutStages> cutCoefficients {};   // 低切：两级高通（L、R 同一组系数）
        ClayBiquad::Coefficients shelfCoefficients;              // 加宽：S 的高架
        std::array<int, (size_t) numMono> monoLength {};         // 收单：各分频点的滑动平均长度 L_c
        std::array<int, (size_t) numCandidates> delays {};       // 各峰值候选输出相对原始的延迟：低切 / 加宽 0，收单 D_c = 2(L_c − 1)
        std::array<std::array<ClayBiquad::Coefficients, (size_t) cutStages>, (size_t) numBlend> blendCut {};   // 混合损失候选的低切
        int guardDelay = 0;                                      // 守护前瞻 G（两个混合损失候选相同）
        int maxDelay = 0;                                        // 各写者里最长的延迟

        // ---- 候选的滤波器状态（每次起跑清零；收单低通与 M / S 延迟环随采样率定长，prepare 里分配，音频线程只清零）----
        std::array<LinearPhaseLowpass, (size_t) numMono> monoLowpass;
        std::vector<double> midHistory, sideHistory;             // M、S 的延迟环（2 的幂长度 > 最长的 D_c）
        int historyMask = 0;
        int historyWrite = 0;
        std::array<std::array<ClayBiquad::State, (size_t) cutStages>, 2> cutState {};   // [声道][级]
        ClayBiquad::State shelfState;
        bool restartPending = true;      // 下一个样本前先从零状态重新起跑
        std::int64_t runSamples = 0;     // 起跑以来跑过的样本数（封顶在预热长度：只用来判断「过了预热」）

        // ---- 块网格上的记账：原始的当前块（随组推进，块末在 noteGroup 里交），各候选的当前块（按自己的延迟对齐，按样本倒数）----
        double originalMax = 0.0;        // 原始当前块的峰值 max(|L|, |R|)
        bool blockWarm = false;          // 原始当前块开始时候选已过预热
        bool blockValid = false;         // 原始当前块到目前为止的每个组都按学习口径算数
        bool blockLiveValid = false;     // 同上，实时口径
        std::array<double, (size_t) numCandidates> candidateMax {};          // 各候选当前块的峰值
        std::array<int, (size_t) numCandidates> candidateLeft {};            // 各候选当前块还差几个输出交齐
        std::array<std::int64_t, (size_t) numCandidates> candidateBlock {};  // 各候选当前块的块号（与原始同一个网格）
        std::array<Record, 2> records {};                                    // 按块号奇偶轮用（最长延迟 < 块长）

        // ---- 一段（≤ maxSegmentSamples）的逐样本暂存：M、S、各收单候选的 LP(S)、各候选的逐样本峰值。定长，不分配 ----
        std::array<double, (size_t) maxSegmentSamples> midScratch {}, sideScratch {};
        std::array<std::array<double, (size_t) maxSegmentSamples>, (size_t) numMono> lowScratch {};
        std::array<std::array<double, (size_t) maxSegmentSamples>, (size_t) numCandidates> peakScratch {};

        // ---- 混合损失候选（T-043）：守护在 prepare 时分配，音频线程上只清零 ----
        std::array<ClayModule::PeakGuard, (size_t) numBlend> guards;
        std::array<std::array<std::array<ClayBiquad::State, (size_t) cutStages>, 2>, (size_t) numBlend> blendCutState {};   // [候选][声道][级]
        int guardGridLeft = 0;                                               // 到下一个 64 样本网格点还有几个样本（从起跑算）
        std::array<std::array<float, (size_t) maxSegmentSamples>, 2> cutOut {};   // 峰值代价低切候选本段的 L / R 输出
        std::array<std::array<std::array<float, (size_t) ClayModule::PeakGuard::maxSegmentLength>, 2>, (size_t) numBlend> blendIo {};
        std::array<std::array<double, (size_t) maxSegmentSamples>, (size_t) numBlend> guardIntended {}, guardOutput {};
        BlendSums blendIntendedBlock {}, blendOutputBlock {};                // 各混合损失候选当前块的累计
        int blendLeft = 0;                                                   // 当前块还差几个输出交齐（两个候选延迟相同，共用）
        std::int64_t blendBlock = 0;                                         // 当前块的块号（与原始同一个网格）

        JUCE_DECLARE_NON_COPYABLE (ClayTrials)
    };
}
