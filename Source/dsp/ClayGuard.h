/*
  ==============================================================================

    ClayGuard.h
    修坯 Clay 的实时保护与三态合成（设计规格-Clay-分析决策与验收 第 3 节、ADR-009）。

    ClayGuard（GLAZE 单元，编排器持有，音频线程每块调一次；纯算术、无锁、不分配）：
      - 反相提示（ADR-012 的反相保护，ADR-015 起只提示、不再改声音）：触发条件 = 慢路 liveLowCorrelation（300 ms）< clayGuardPhaseCorrelation
        （出厂 −0.2：单声道回放低音损失 > 4 dB，真破坏），或（慢路 < clayGuardPhaseFastGate（出厂 0.1，ADR-014）且快路
        liveLowCorrelationFast（120 ms）< clayGuardPhaseCorrelation）。条件须连续成立 clayGuardPhaseMinSeconds（出厂 0.15 s，
        有效音频秒数、块长无关）才算触发（去掉瞬态）；触发后提示强度 phaseFloor 在 clayGuardPhaseAttackSeconds（0.1 s）内升到 1；
        条件消失后保持 clayGuardPhaseHoldSeconds（2 s），再用 clayGuardPhaseReleaseSeconds（2 s）线性退回 0。phaseActive = phaseFloor > 0
        （界面「收单」灯琥珀 +「低频反相」提示）。**输出 monoAmount 不变**（ADR-015）：收单只去掉 S，单声道回放听的是 M、救不回来，
        反相段里收满只会让立体声听众丢低音（真歌 will we be okay 的反相段 −1 ～ −2.5 dB，单声道只差 0.04 dB）。
      - 宽度保护：用实时 2 ～ 16 kHz 的 P_M、P_S 预测加宽后的相关度 ρ' = (P_M − γ²P_S) / (P_M + γ²P_S)（γ = 10^(w/20)），
        允许的最大 w = 10·log10(k · P_M / P_S)，k 由 clayMinCorrelationAfterWidth 解出（0.25 → k = 0.6），下限 0（保护从不自动收窄）。
        widthCapDb 以 clayGuardWidthAttackSeconds（0.2 s）跟随到允许值（下降方向），以 clayGuardWidthReleaseSeconds（2 s）回升；
        输出 widthDb = min(方案值, widthCapDb)。
      - 只可能「减小 widthDb」；lowCutHz、monoHz、monoAmount 原样透传；costLimitedMask 原样透传（只给界面解释）。
      - 冻结：seconds = 0（静音 / 宿主旁通 / 停播块）或实时视图无效时，状态不动（保持期、资格期计时都不走）。
      - active = false（GLAZE ≤ 0）：状态直接回中性（ClayGuardState{}，资格期计时清零），输出 = 输入。
    时间一律按有效音频秒数（与 GlazeGuard 同口径）。状态可存进执行快照（getState / setState）；「条件已连续成立多久」的
    资格期计时不在 ClayGuardState 里（该结构随执行快照保存、格式已定），不存档：setState / reset 后从 0 起算（只会让恢复后
    正在进行的反相晚最多 0.15 s 重新计保持期，提示强度与保持期本身照快照接着走，听感不变）。

    resolveClayParams（纯函数）：把用户三态设置与自动值合成交给 ClayModule 的实际参数——
      自动 → 自动值（glazePercent ≤ 0 时取中性：lowCutHz 0、monoAmount 0、widthDb 0）；关 → 中性；
      手动 → 用户的值（低切 = user.lowCutHz；收单 = user.monoHz + monoAmount 1；加宽 = user.widthDb），GLAZE 为 0 也生效。
      bypass 取 user.bypass。结果按 ClayModule 的参数范围钳制。

  ==============================================================================
*/

#pragma once

#include "ChainParams.h"
#include "GlazeConstants.h"
#include "GlazeTypes.h"
#include "MusicAnalyzer.h"

namespace celadon::glaze
{
    class ClayGuard
    {
    public:
        ClayGuard() = default;

        // 常数变化时调（编排器在方案 / 标定快照切换时调）；只拷贝需要的几个数，不分配。
        void setConstants (const Constants& constants) noexcept;

        // 状态回中性（ClayGuardState{}，资格期计时清零）。
        void reset() noexcept;

        // 每块一次。planned = 方案（含过渡混合）给出的「自动」态值；view = 分析器此刻的实时视图；
        // seconds = 用于推进保护时间的有效音频时长（静音 / 旁通 / 停播传 0 → 冻结）；active = GLAZE > 0。
        // 返回经保护修改后的自动值。
        ClayPlan update (const ClayPlan& planned, const ShortTermView& view, double seconds, bool active) noexcept;

        const ClayGuardState& getState() const noexcept   { return state; }
        void setState (const ClayGuardState& restored) noexcept;   // 执行快照恢复（钳到合法范围；资格期计时从 0 起算）

        // ---- T-038 House 档反相提示（设计规格-曲风-House学习方式 第 9 节）----
        // 只提示、不改声音；与上面 update 的反相提示分开，不进 ClayGuardState、不存档。
        //   view = 分析器此刻的实时视图（读慢路 liveLowCorrelation）；seconds = 有效音频秒数（同 update）；
        //   eligible = 编排器判定此刻在 House 档的满编段里；requiredSeconds = 条件须连续成立多久才提示（编排器按 4 小节换成秒，
        //   拿不到拍点时传 Constants::house.phaseHintFallbackSeconds）。不 eligible 时提示熄灭、计时清零；条件消失后先保持
        //   clayGuardPhaseHoldSeconds（出厂 2 s，同 update 的反相提示）再熄灭。
        struct HouseHint
        {
            bool active = false;
            float monoLossDb = 0.0f;   // 10·log10((1 + ρ) / 2)，≤ 0：亮起时按公式算，之后 1 s 一阶平滑，保持期不更新，ρ 钳到 [−0.99, 1]；
                                       // active 为 false 时 0
        };

        void updateHouseHint (const ShortTermView& view, double seconds, bool eligible, double requiredSeconds) noexcept;
        const HouseHint& getHouseHint() const noexcept   { return houseHint; }

    private:
        ClayGuardState state;
        HouseHint houseHint;
        double houseHintPendingSeconds = 0.0;   // 没亮时 = 条件已连续成立的有效音频秒数（资格期）；亮着时 = 剩余保持秒数
        float houseHintCorrelation = 0.6f;      // setConstants 从 Constants::house.phaseHintCorrelation 拷贝
        // ---- 由实现代理 A2 补全（T-012）：从 Constants 拷贝的阈值与时间常数等 ----

        // 反相触发条件已连续成立的有效音频秒数（封顶 phaseMinSeconds）。不在 ClayGuardState 里、不存档（见文件头）。
        double phasePendingSeconds = 0.0;

        // setConstants 拷贝并清洗后的保护常数（口径见 ClayGuard.cpp 文件头）：非有限值按 0 读（与旧快照缺字段同口径），
        // 时长钳到 [0, 1e6] 秒（0 = 立即），相关度阈值与快路门钳到 [−1, 1]。
        struct Tuning
        {
            float phaseCorrelation = 0.0f;      // clayGuardPhaseCorrelation：慢路（或门内的快路）低频相关度低于它 → 反相
            float phaseFastGate = 0.0f;         // clayGuardPhaseFastGate：慢路低于它时，快路才参与判定
            double phaseMinSeconds = 0.0;       // clayGuardPhaseMinSeconds：条件须连续成立这么久才开始攻击（0 = 立即）
            double phaseAttackSeconds = 0.0;    // phaseFloor 0 → 1 的时间（线性）
            double phaseHoldSeconds = 0.0;      // 条件消失后的保持时间
            double phaseReleaseSeconds = 0.0;   // phaseFloor 1 → 0 的时间（线性）
            double widthAttackSeconds = 0.0;    // 宽度上限从方案值收到 0 的时间（线性，见 .cpp「宽度保护」）
            double widthReleaseSeconds = 0.0;   // 宽度上限从 0 回到方案值的时间
            double widthPowerScale = 1.0;       // k = (1 − ρ_min) / (1 + ρ_min)（ρ_min = clayMinCorrelationAfterWidth）：γ² ≤ k · P_M / P_S
        };

        static Tuning makeTuning (const Constants& constants) noexcept;

        // 本块实时视图给反相一路的判定：不知道（慢路无效 → 这一路冻结）/ 条件不成立 / 条件成立。
        enum class PhaseInput { unknown, clear, inverted };
        PhaseInput phaseInputOf (const ShortTermView& view) const noexcept;

        // 反相提示推进 seconds 秒：conditionHolds = 本块反相触发条件成立（还要连续成立满 phaseMinSeconds 才算触发）。
        void advancePhase (bool conditionHolds, double seconds) noexcept;
        // 宽度保护推进 seconds 秒：allowedDb = 此刻允许的最大加宽（0 ～ clayWidthUncappedDb）；plannedWidthDb = 方案值（已清洗）。
        void advanceWidth (float allowedDb, float plannedWidthDb, double seconds) noexcept;
        // 用实时高频 P_M、P_S 算允许的最大加宽 10·log10(k · P_M / P_S)，下限 0；P_S = 0 → clayWidthUncappedDb（不限）。
        float allowedWidthDb (const ShortTermView& view) const noexcept;

        // 缺省 = 出厂值：Constants 的 clay* 字段默认值就是出厂值（GlazeConstants.h「只在这里写一次」），这里不重写数字。
        Tuning tuning = makeTuning (Constants{});
    };

    ClayModule::Params resolveClayParams (const ClaySettings& user, const ClayPlan& autoValues, float glazePercent) noexcept;

    //==============================================================================
    // 修坯压力反馈（ADR-014）。ClayModule 的峰值守护保证修坯不抬峰值，但守护若一直在把修坯的改动量拉回来（真歌 WTF：92 % 的样本
    // g < 1、每块最小 g 的中位数 0.13），说明这首歌的这一项自动修坯弊大于利——一直硬拉会在低音上形成深调制。「先试后用」只在聆听的
    // 前 15 ～ 30 s 里试；这里把「试」延伸到整首歌。为什么不用守护的回收量 mean(1 − g)：同相的移除（去直流、收单）回收多也不伤响度，
    // 回收量分不出伤害；伤害分两种、各量各的——顶峰真被抬（第一触发量）与混合抵消（第二触发量，T-043）。
    //   第一触发量（T-044 / ADR-028 起与「先试后用」同口径）：ClayModule 报告的 100 ms 峰值块（①② 处理后 max |y|、干路 max |x|，
    //   不经守护）按有效音乐累计，代价 = 20·log10(P99 处理后 / P99 干路)——与 MusicAnalyzer 第「五」节的先试代价逐式相同（线性插值
    //   百分位、每个序列只留最大的若干块、处理后按干路的 −120 dB 封底），只是候选换成此刻真在用的①②、窗口从聆听区间延伸到「自上次
    //   清零起的整段」。门槛 = 先试「照做」的上限 0.2 dB（clayCostFullDb 出厂值）。T-044 之前这里累计的是「每 10 ms 一格超出界 B 多少
    //   dB 的平均」：没超界的格记 0，被大量稀释，比先试的口径小约 4 倍（真歌 deep：先试口径的 10 s 窗代价中位 +0.35 dB、64 % 的窗
    //   > 0.2；旧量 2 s 平均中位 0.083、> 0.2 的时间 0.1 %），整首顶峰真被抬 0.5 ～ 0.7 dB 的歌（somggggg、deep）从不降档（BUG-011）。
    //   为什么是整段累计、不是滑动窗：限幅器只在整首最响的那些峰上工作，安静段的局部峰抬一点不增加它的负担（那里守护的拉锯归第二
    //   触发量管）；滑动 10 s / 30 s 窗会在整首顶峰其实下降的歌（Jamendo tech：整首 −0.09 dB）的安静段里找出 +0.2 dB 的窗、把低切
    //   一路降到关。块的有效：要整块都在有效音乐里（跨切片的块要每片都是），且不在降档后的冷却期里（方案还在过渡，块峰值不代表新方案）。
    //   只留最大的 topCount 个块，exactBlocks（21 分钟有效音乐）以内 P99 精确，超出后两边按同一个秩（从大往小第 126 个附近）估计——
    //   偏向更高的百分位、两边同秩，仍是顶峰的比较。
    //   证据分两档（两档的数都是先试自己的常数）：代价 ≥ vetoThreshold（0.6 dB，先试「不做」的线 clayCostZeroDb）——满 minimumBlocks
    //   （10 s，先试的最短时长 trialMinimumSeconds）就算；代价在 threshold 与 vetoThreshold 之间（先试只会「减量」的区间）——要满
    //   confirmBlocks（512 块 = 51.2 s，先试的记录容量 trialCapacity）才算。为什么：累计的块少时 P99 就是最大的那三四块，一段鼓点或一个
    //   drop 就能把它推过 0.2 dB，而降档是单向的；真歌 Jamendo tech（整首顶峰 −0.09 dB）只要 10 s 就判时在 34 s 被一个新进来的大块推到
    //   +0.25 dB、连降两档（woqu、emotapes 同理，都发生在头 10 s）；满 512 块后 P99 至少落在第 5 大块之后，这几首的累计代价最高 +0.14 dB。
    //   代价 ≥ 0.6 dB 的歌（WTF +1.2、will we be okay +1.9、somggggg +0.7）10 s 的证据就够，与 T-044 之前一样快；0.2 ～ 0.6 dB 的
    //   （Jamendo deep +0.51）多等 41 s。
    //   触发：上面的条件连续 sustainSeconds（有效音乐秒数）成立，且距上次下调已过 cooldownSeconds → update 返回 true，调用方（编排器）
    //         把自动 Clay 的缩放降一档（nextScale：1 → 0.5 → 0.25 → 0）并开方案过渡；本单元同时清空累计与计时，在新方案下重攒。
    //   纯算术、不分配；块长只影响判断时刻的量化（最多一个切片），与 GlazeGuard 按块读统计同一口径。
    //   T-043 第二个触发量（ADR-027）：守护的混合损失——守护把低切拉回干路时与干路相位不同处互相抵消，顶峰没被抬、守护却几乎
    //   一直在拉的歌（woqu：T-044 之前的超出量从未持续过 0.2 dB，现在的整段块峰值代价也不到 0.2 dB；混合损失 0.2 dB）上面那条量不到。ClayModule::getLastProcessBlend 的两份 K 加权
    //   能量按它们覆盖的有效音乐秒数一阶平滑（时间常数同 smoothingSeconds），损失 = 10·log10(本该 / 实际) 连续 sustainSeconds 高于
    //   blendThreshold 也降一档；两个触发量共用冷却与档位，任一触发都把两边一起清零。blendThreshold = 修坯门槛响度容差 0.1 LU 的
    //   1.5 倍：先试后用已把方案频率封在预测损失 clayBlendLossLimitDb（出厂 0.09 dB，GlazePlanner capLowCutForBlend），这里只接住
    //   学习区间之外更糟的段落；正常歌 2 s 平滑后的起伏（真歌 somggggg_mp3 0.07 ～ 0.12 dB）碰不到它。混合损失从未超过门槛时，返回值
    //   与只有第一条时逐位相同。
    struct ClayPressureMonitor
    {
        static constexpr double threshold = 0.2;        // dB：先试「照做」的上限（clayCostFullDb 出厂值）
        static constexpr double vetoThreshold = 0.6;    // dB：先试「不做」的线（clayCostZeroDb 出厂值）
        static constexpr double percentile = 0.99;      // 同 MusicAnalyzer 的 trialPercentile
        static constexpr int minimumBlocks = 100;       // 有效块满 10 s（100 × 0.1 s，同 trialMinimumSeconds）才有代价；≥ vetoThreshold 就算
        static constexpr int confirmBlocks = 512;       // 代价在 threshold 与 vetoThreshold 之间时要满 51.2 s（同 trialCapacity）才算
        static constexpr int topCount = 128;            // 每个序列只留最大的这么多块
        static constexpr int exactBlocks = 100 * (topCount - 2);   // 12600 块（21 分钟有效音乐）以内 P99 精确
        static constexpr double floorRatio = 1.0e-6;    // 处理后的 P99 按干路 P99 的 −120 dB 封底（同 trialPeakFloorRatio）
        static constexpr double sustainSeconds = 1.0;
        static constexpr double cooldownSeconds = 4.0;  // 方案过渡 3 s + 1 s：新方案生效后再攒、再判断
        static constexpr double smoothingSeconds = 2.0; // 混合损失的平滑（T-043）
        static constexpr double blendThreshold = 0.15;  // dB（T-043）

        // n ≤ exactBlocks 时，P99 的下侧顺序统计量从大往小数是第 n − 1 − ⌊0.99·(n − 1)⌋ ≤ 0.01·(n − 1) + 1 个（0 起），上侧再大一个。
        static_assert ((1.0 - percentile) * (double) (exactBlocks - 1) + 1.0 < (double) (topCount - 1),
                       "留的最大值个数必须覆盖 exactBlocks 以内 P99 用到的两个顺序统计量");

        // 本切片走完的峰值块（ModuleStats::clayPeakBlocks / clayPeakBlockCount / clayPeakBlocksDropped）：按时间顺序成对存
        // [①② 处理后 max |y|, 干路 max |x|]；dropped = 模块一次 process 装不下、没报上来的块（按无效）。
        struct PeakBlocks
        {
            const float* pairs = nullptr;
            int count = 0;
            int dropped = 0;
        };

        // blocks = 本切片走完的峰值块；music = 本切片在有效音乐里（不在时它的块与跨进来的块都不计）；seconds = 本切片的有效音乐秒数
        // （10 ms 时钟格，不在有效音乐里时调用方传 0：计时冻结）。blendIntended / blendOutput = 本切片守护混合损失测量的两份 K 加权
        // 能量，blendSeconds = 它们覆盖的有效音乐秒数（不在有效音乐里时调用方传 0：这一段的能量不计、混合损失的平滑冻结）；非有限 /
        // 负能量这一段不计。返回 true = 该降一档。
        bool update (const PeakBlocks& blocks, bool music, double seconds,
                     double blendIntended = 0.0, double blendOutput = 0.0, double blendSeconds = 0.0) noexcept;
        void reset() noexcept;
        void clearPeakCost() noexcept;   // 清空第一触发量的累计、代价与持续计时（降档、reset 时；不动冷却与混合损失）

        // 第一触发量此刻的代价（dB，与先试同式）：有效块满 minimumBlocks 且干路 P99 > 0 才有（costReady），否则 0。
        bool costReady() const noexcept   { return costValid; }
        double costDb() const noexcept    { return costValid ? cost : 0.0; }

        // 此刻的代价算不算「该降」（两档证据，见上）：≥ vetoThreshold，或 > threshold 且有效块满 confirmBlocks。
        bool costCalls() const noexcept
        {
            return costValid && (cost >= vetoThreshold || (cost > threshold && blockCount >= confirmBlocks));
        }

        // 平滑后的混合损失（dB）：两份平滑能量都 > 0 才有，否则 0。
        double blendLossDb() const noexcept;

        // 下一档缩放：> 0.5 → 0.5，> 0.25 → 0.25，其余 → 0（非有限 → 0）。
        static float nextScale (float scale) noexcept;

        // 线性插值的 P99（定义同 numpy.percentile 默认，与 MusicAnalyzer 的 percentileOfTop 逐式相同）：top 降序、count 个有效值
        // （count ≤ exactBlocks，调用方保证），位置 0.99·(count − 1) 两侧的顺序统计量插值。第 j 小（0 起）= top[count − 1 − j]。
        static double percentileOfTop (const std::array<double, (size_t) topCount>& top, int count) noexcept;

        std::array<double, (size_t) topCount> topWet {};   // 自上次清零起有效块里①② 处理后峰值最大的 topCount 个（降序）
        std::array<double, (size_t) topCount> topDry {};   // 干路峰值最大的 topCount 个（降序；与上面各自排序，同先试）
        int blockCount = 0;                  // 自上次清零起的有效块数
        bool openBlockClean = false;         // 正在走的那一块到此为止都在有效音乐里（初值 / 清零后为假：跨过断点的块不算）
        bool costValid = false;
        double cost = 0.0;                   // 第一触发量的代价（dB）
        double aboveSeconds = 0.0;           // costCalls() 连续成立的有效音乐秒数
        double sinceStep = cooldownSeconds;  // 距上次下调的有效音乐秒数（初值 = 冷却已满）
        double blendIntendedSmoothed = 0.0;  // 混合损失的两份平滑能量（T-043）
        double blendOutputSmoothed = 0.0;
        double blendAboveSeconds = 0.0;      // 平滑后的混合损失连续高于 blendThreshold 的有效音乐秒数
    };

    // 压力反馈的缩放作用在自动方案上（编排器在方案（含过渡混合）交给 ClayGuard 之前乘，纯函数、不分配）：
    //   scale = 1（或 ≥ 1）→ 原样返回（逐位）；非有限 → 按 0；钳到 0 ～ 1。
    //   收单比例、加宽 × scale；收单比例成 0 时分频点回中性默认（ClayPlan{}.monoHz，同决策器）。
    //   低切在「k」的域里缩：决策器的 lowCutHz = 地板 · (目标 / 地板)^k，所以 k × scale ⇔ lowCutHz' = 地板 · (lowCutHz / 地板)^scale
    //   （double 里算；lowCutHz 不高于地板时不动）；scale = 0 → 关（0）。地板非有限或 ≤ 0 时按 ClayModule::minLowCutHz。
    //   代价位：原本不为 0、被 scale < 1 缩小的项置「因代价减量 / 否决」的位（界面说「本曲试过：…会抬高峰值，已自动减量 / 跳过」，
    //   这里的「试」是整首歌里守护的实测）；原有的位保留。
    ClayPlan scaleClayPlan (const ClayPlan& plan, float scale, float lowCutFloorHz) noexcept;
}
