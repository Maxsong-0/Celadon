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
    正在进行的反相晚最多 0.15 s 重新计保持期，下限与保持期本身照快照接着走，听感不变）。

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
        // 只提示、不改声音；与上面的反相保护分开，不进 ClayGuardState、不存档。
        //   view = 分析器此刻的实时视图（读慢路 liveLowCorrelation）；seconds = 有效音频秒数（同 update）；
        //   eligible = 编排器判定此刻在 House 档的满编段里；requiredSeconds = 条件须连续成立多久才提示（编排器按 4 小节换成秒，
        //   拿不到拍点时传 Constants::house.phaseHintFallbackSeconds）。不 eligible 时提示熄灭、计时清零。
        struct HouseHint
        {
            bool active = false;
            float monoLossDb = 0.0f;   // 10·log10((1 + ρ) / 2)，≤ 0；active 为 false 时 0
        };

        void updateHouseHint (const ShortTermView& view, double seconds, bool eligible, double requiredSeconds) noexcept;
        const HouseHint& getHouseHint() const noexcept   { return houseHint; }

    private:
        ClayGuardState state;
        HouseHint houseHint;
        double houseHintPendingSeconds = 0.0;   // House 提示条件已连续成立的有效音频秒数
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

        // 反相保护推进 seconds 秒：conditionHolds = 本块反相触发条件成立（还要连续成立满 phaseMinSeconds 才算触发）。
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
    // 前 15 ～ 30 s 里试；这里把「试」延伸到整首歌：ClayModule 每 10 ms 一格报告「不经守护时相关峰值会超出界 B 多少 dB」，与试用的
    // 代价同一单位，门槛 = 试用「照做」的上限 0.2 dB（clayCostFullDb 出厂值）。为什么不用守护的回收量 mean(1 − g)：改动量大、
    // 超出却很小时（低切的相位残差）g 也会被拉得很低，回收量夸大了伤害（合成素材 60 / 110 Hz + 噪声：超出平均 0.11 dB，回收量却
    // 足以三次降档）。
    //   平滑：按有效音乐秒数的一阶平滑（时间常数 smoothingSeconds）；冻结（seconds ≤ 0：静音、停播、旁通）时不动。
    //   触发：平滑值连续 sustainSeconds 高于 threshold，且距上次下调已过 cooldownSeconds（方案过渡 3 s + 1 s：新方案生效后再判断）
    //         → update 返回 true，调用方（编排器）把自动 Clay 的缩放降一档（nextScale：1 → 0.5 → 0.25 → 0）并开方案过渡；
    //         本单元同时清零平滑值与计时，在新方案下重新判断。
    //   纯算术、不分配；块长只影响判断时刻的量化（最多一个切片），与 GlazeGuard 按块读统计同一口径。
    struct ClayPressureMonitor
    {
        static constexpr double smoothingSeconds = 2.0;
        static constexpr double threshold = 0.2;        // dB
        static constexpr double maxPressureDb = 60.0;   // 输入钳位
        static constexpr double sustainSeconds = 1.0;
        static constexpr double cooldownSeconds = 4.0;

        // pressure = 本段代价格的平均（dB，非有限按 0，钳到 0 ～ 60）；seconds = 本段的有效音乐秒数。返回 true = 该降一档。
        bool update (double pressure, double seconds) noexcept;
        void reset() noexcept;

        // 下一档缩放：> 0.5 → 0.5，> 0.25 → 0.25，其余 → 0（非有限 → 0）。
        static float nextScale (float scale) noexcept;

        double smoothed = 0.0;               // 平滑后的代价（dB）
        double aboveSeconds = 0.0;           // 平滑值连续高于门槛的有效音乐秒数
        double sinceStep = cooldownSeconds;  // 距上次下调的有效音乐秒数（初值 = 冷却已满）
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
