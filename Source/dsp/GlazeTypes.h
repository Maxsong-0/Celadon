/*
  ==============================================================================

    GlazeTypes.h
    GLAZE 智能总旋钮（v0.6，设计规格-GLAZE、ADR-008）各单元共用的数据类型。
    只放数据结构与枚举，不含行为；分析器、决策器、校准器、保护层、编排器都只通过这些类型交换数据。

    约定：
      - 所有响度单位 LUFS / LU，增益与电平 dB，时间一律按「有效音频样本」折算的秒（不按回调次数、不按现实时间）；
      - 「不知道」用 valid = false 表示，绝不用 0 冒充；
      - 这些结构都是平凡可拷贝的定长数据（std::array），音频线程拷贝不分配。

  ==============================================================================
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace celadon::glaze
{
    // 方案默认值和出厂常数共用版本，GLAZE 0 学完后保存的空方案也必须属于当前算法。
    // 7 = v0.1.7.0 修坯 Clay（设计规格-Clay、ADR-009）：画像加立体声 / 超低频特征，方案加 Clay 四项。
    // 8 = T-032（2026-09-29，设计规格-GLAZE 7.4「版本」段）：R6-01 自动 Heat 只承担换算表里该电平真能削掉的峰均比份额、削不到
    //     Constants::heatUsefulCrestDb 就只留风格底量 StyleConstants::automaticHeatFloor × 强度（原来 HeatRow::inverse「达不到就给最大」
    //     让残差 Heat 表下的自动 Heat 与音乐无关地顶到 70 / 50 / 20）；R6-06 满强度增益按 Limit Gain 量程（24 − 手动 Gain）回算。
    //     两条新规则都按**快照**的 Constants::algorithmVersion ≥ 8 分流（GlazePlanner.cpp makeFullPlan / completeFullPlan，与 ≥ 3 那一处
    //     同一写法）：算法 6 / 7 的存档重开时 committed.constants 就是存档里的快照，方案走旧分支、增量逐位同存档。
    // T-038（2026-10-01，曲风与交付）**不升**本常数：新行为全部按参数「曲风 ≠ 通用」或「交付 ≠ 标准」分流，不靠算法版本区分；
    //     升号会让 0.1.8.x 存的方案（算法 8）与重算出的方案在 sameCalibrationPlan 里版本不等，旧工程重开走另一条路径、不再与 main
    //     逐位相同（设计规格-曲风与交付 第 6 节，2026-10-01 阶段 1 更正）。
    inline constexpr int currentAlgorithmVersion = 8;

    // 与本版本「行为相同」的最早算法版本。7 只在 6 上加了 Clay，而 6 保存的画像（格式 1）没有 Clay 所需特征 → Clay 中性，
    // 其余自动增量的算法没变；所以算法 6 保存的方案与执行快照照常采用、不进「照用旧增量」模式，v0.1.6.2 工程打开后
    // **在约束反馈第一次触发之前**交给模块的参数逐位不变（tests/LegacyFixtureTests 用 v0.1.6.2 真实写出的存档核对）；触发之后按修复后的
    // 上限公式（applyFeedback 不再把校准偏差算两次）与旧版有意不同（审核 V2-01）。输出因 Clay 整体晚 D + G 个样本
    // （收单 D，ADR-012；守护前瞻 G，ADR-014）（T-012 阶段 2 主进程裁决，A6 报告「请主进程裁决」一条）。
    // 8 改了既有增量的行为（自动 Heat、满强度增益），但按快照的 algorithmVersion 分流：旧存档重开时用存档里的快照重算，走旧分支、
    // 结果逐位同存档，所以「行为相同」仍成立，本常数保持 6（T-032 核验 V6 第 1 节第 2 点：升版不丢校准 / 执行快照——calibrationApplicable
    // 只比参考条件、executionHeld 只在参考条件变时释放）。以后升算法版本时，若改了既有增量的行为**且不能按快照版本分流**，才把本常数跟着升。
    inline constexpr int oldestCompatibleAlgorithmVersion = 6;

    constexpr bool isCompatibleAlgorithm (int version) noexcept
    {
        return version >= oldestCompatibleAlgorithmVersion && version <= currentAlgorithmVersion;
    }
    inline constexpr int numBands = 27;   // 与 BalanceModule::numBands 相同（1/3 倍频程，40 Hz ～ 16 kHz）

    // 风格：顺序 = 参数 glaze_style 的选项顺序（AudioParameterChoice 下标）。
    enum class Style : int { automatic = 0, warm = 1, punchy = 2, clean = 3 };

    // T-038 曲风与交付（设计规格-曲风与交付 第 2 节、ADR-026）。顺序 = 参数 glaze_genre / glaze_delivery 的选项下标；
    // 两个参数不可自动化，以后只许在末尾追加选项（APVTS 按下标存状态，旧工程不会错位）。
    enum class Genre : int { general = 0, deepHouse = 1, techHouse = 2, melodicProgressive = 3 };
    inline constexpr int numGenres = 4;
    enum class Delivery : int { standard = 0, streaming = 1, club = 2 };
    inline constexpr int numDeliveries = 3;

    // 三个 House 子类共用学习方式、限幅跟拍、反相提示（设计规格-曲风-House学习方式）；通用档一律走原有路径。
    constexpr bool isHouse (Genre genre) noexcept   { return genre != Genre::general; }

    // 参数原始下标（处理器已取整钳位；无头调用可能越界）→ 枚举。越界按默认（通用 / 标准），与读不到参数时同一口径。
    constexpr Genre genreFromIndex (int index) noexcept
    {
        return index >= 0 && index < numGenres ? static_cast<Genre> (index) : Genre::general;
    }

    constexpr Delivery deliveryFromIndex (int index) noexcept
    {
        return index >= 0 && index < numDeliveries ? static_cast<Delivery> (index) : Delivery::standard;
    }

    // 不连续点：三种各有自己的清理规则（设计规格-GLAZE-时间线与保存 第 3 节）。
    enum class Discontinuity : int { transportJump = 0, loopWrap = 1, hostReset = 2 };

    // 一个特征 = 数值 + 有效标志 + 有效观测时长 + 置信度（0 ～ 1）。
    struct Feature
    {
        float value = 0.0f;
        bool valid = false;
        double effectiveSeconds = 0.0;
        float confidence = 0.0f;
    };

    // T-038 按小节记账的分段方式（SegmentLedger）：没有分段 / 按宿主小节 / 宿主没给拍点时按 2 秒。
    enum class SegmentMode : std::uint8_t { none = 0, bars = 1, seconds = 2 };

    // T-038 House 档门控结果（设计规格-曲风-House学习方式 第 2、3、5 节）。画像格式 3 才存；不存逐段数据。
    // 「小节」在分 2 秒段时按段数计（heardSegments / fullSegments 同一口径），时长另记秒数，锁定条件按 mode 选用哪一组。
    struct HouseGateSummary
    {
        bool valid = false;                  // House 档门控算出过结果（false = 旧画像或通用档画像）
        SegmentMode mode = SegmentMode::none;
        float referenceLufs = 0.0f;          // S_ref：已听各段响度的第 95 百分位（按时长加权）
        float lowShareReference = 0.0f;      // 低频参考：已过响度门的各段 lowShare 的第 95 百分位
        float gateWidthLu = 0.0f;            // 本次门控用的 ΔG（快照值）
        float lowGateFactor = 0.0f;          // 本次门控用的低频门系数（快照值）
        int heardSegments = 0;               // 不重复的已听段数
        int fullSegments = 0;                // 满编段数（过了两道门）
        double heardSeconds = 0.0;           // 不重复的已听有效音频秒数
        double fullSeconds = 0.0;            // 满编段的有效音频秒数
        int reopenCount = 0;                 // 锁定后已重开次数（设计规格-曲风-House学习方式 第 4 节，上限见 Constants::house）
    };

    // 画像：从入口音乐测得的整首歌特征（设计规格-GLAZE-分析器）。一首歌一份，随工程保存。
    struct MusicProfile
    {
        Feature integratedLufs;          // I（绝对门限 −70、相对门限 −10）
        Feature loudnessRangeLu;         // LRA（P95 − P10，相对门限 −20）；置信度单列（前 60 s 未稳定）
        Feature plrDb;                   // 同一区间的真峰值最大值 − I
        Feature truePeakDbtp;            // 测量区间内入口真峰值最大值
        Feature spectralSlopeDbPerOct;   // 27 段长期电平的对数频率线性拟合
        Feature lowShare;                // 100 Hz 段及以下的功率份额（0 ～ 1）
        Feature highShare;               // 6.3 kHz 段及以上的功率份额（0 ～ 1）
        Feature lowBandRangeDb;          // 低频（100 Hz 段及以下）逐帧电平的 P90 − P10（dB）：持续低音小、间歇的重底鼓大，
                                         // 与 lowShare 一起给 Glue 的风险判断（分析器第 4 节「持续低音与间歇重底鼓不走同一策略」）
        Feature onsetDensityPerSecond;   // 起音事件密度
        Feature onsetStrengthDb;         // 起音事件强度（相对突起）

        std::array<float, (std::size_t) numBands> bandLongTermDb {};      // 27 段长期电平原始统计（段内总功率口径）
        std::array<float, (std::size_t) numBands> tameEvidenceDb {};      // 超出容忍区间的正偏离累计（Tame 证据）
        std::array<float, (std::size_t) numBands> recoverEvidenceDb {};   // 超出容忍区间的负偏离累计（Recover 证据）
        std::array<float, (std::size_t) numBands> bandConfidence {};      // 每段可信度 0 ～ 1

        // ---- 格式版本 2（v0.1.7.0 修坯 Clay，设计规格-Clay-分析决策与验收 第 1 节）----
        // 全部在入口测点测（原始输入）。格式版本 1 的旧画像没有这些字段：读入后全部 valid = false、数组全 0，
        // 决策器据此给 Clay 自动值 0（旧工程打开声音不变）。
        Feature subShareDb;              // 真次声占比 10·log10(E_<16Hz / E_全频)：入口样本流上 M 的 8 阶 Butterworth 16 Hz 低通能量比
                                         // （ADR-012 / 设计规格-Clay-根治修订 3.1：原 30 Hz 4 阶测到的主要是底鼓基音漏入）
        Feature lowSideToMidDb;          // 10·log10(Σ P̄_S / Σ P̄_M)，40 ～ 125 Hz 共 6 段，功率域时间平均
        Feature lowCorrelation;          // Σ X̄ / Σ bandPower̄，同上 6 段（−1 ～ 1）
        Feature highSideToMidDb;         // 同 lowSideToMidDb，2 ～ 16 kHz 共 10 段
        Feature highCorrelation;         // 同 lowCorrelation，2 ～ 16 kHz
        std::array<float, (std::size_t) numBands> bandSideToMidDb {};     // 逐段 S/M（dB）；该段没有有效统计时为 0 且 bandStereoConfidence = 0
        std::array<float, (std::size_t) numBands> bandCorrelation {};     // 逐段相关度 −1 ～ 1；同上
        std::array<float, (std::size_t) numBands> bandStereoConfidence {};// 逐段立体声统计可信度 0 ～ 1（观测充分度口径同 bandConfidence）
        // 先试后用（ADR-012，设计规格-Clay-根治修订 3.2）：学习区间内把候选处理跑在原始输入上，量 100 ms 块峰值 P99 的变化（dB，
        // 正 = 峰值变高）。有效块满 10 s 才 valid。收单按 clayMonoCandidateHz 四个分频点各一个。
        Feature lowCutPeakCostDb;                                    // IIR 4 阶 20 Hz 低切
        std::array<Feature, 4> monoPeakCostDb {};                    // 线性相位收单 a = 1，分频点 = clayMonoCandidateHz[i]
        Feature widthPeakCostDb;                                     // 2 kHz 高架 +1.5 dB（侧声道）
        // T-043（ADR-027）：低切候选（频率 = clayBlendCandidateHz[i]）过 ClayModule 的峰值守护，守护的混合损失
        // 10·log10(本该 / 实际)（K 加权，dB；正 = 守护把低切拉回干路时与干路相位不同、互相抵消，成品比低切本该给的更轻）。
        // 与上面的代价同一学习区间、同一批有效块，同时有效。决策器按两点的幂律插值，把低切频率封顶在预测损失 = 门槛处。
        // 画像里是可选字段：旧画像没有 = 不知道（valid = false），决策器不据它封顶。
        std::array<Feature, 2> lowCutBlendLossDb {};

        double effectiveSeconds = 0.0;   // 累计有效音频时长（新增样本折算）
        float representativeness = 0.0f; // 代表性置信度 0 ～ 1：同一段反复循环不增加

        // ---- 格式版本 3（T-038 House 档，设计规格-曲风-House学习方式 第 5 节）----
        // 只有 House 档学出的画像才是格式 3（houseGate.valid = true）；通用档学出的画像仍是格式 2，与 main 逐字节相同。
        // 读格式 1 / 2 时 houseGate 保持默认（valid = false）：House 档下视为「锁定条件未满足」、继续聆听，不清空。
        HouseGateSummary houseGate;

        int formatVersion = 2;           // 画像数据格式版本（保存 / 读取用）；2 = 含上面的立体声 / 超低频字段（1 = v0.6～0.1.6.2）；
                                         // 3 = 另有 houseGate（T-038，只在 House 档）
    };

    // 三种风格的混合权重（和为 1）。「自动」按画像算出后随画像一起锁定。
    struct StyleWeights
    {
        float warm = 0.0f;
        float punchy = 0.0f;
        float clean = 1.0f;
    };

    // 先试后用的收单候选分频点（Hz，递增；ADR-012）：分析器按它们各测一个代价、随画像保存；ADR-015 起决策器不用
    // （原先按对数频率在相邻两点间插值）。
    inline constexpr std::array<float, 4> clayMonoCandidateHz { 60.0f, 80.0f, 120.0f, 160.0f };

    // 守护混合损失的试用候选（T-043，ADR-027）：低切频率，升序。10 Hz = 出厂地板（clayLowCutFloorHz），20 Hz = 风格目标的上限
    // （f_t 出厂 16 / 18 / 20 Hz），也是峰值代价那个低切候选（分析器直接用它的输出）。两点之间按幂律 L ∝ f^q 插值（log-log 直线）：
    // 低切在 f ≫ fc 处的相位差 ∝ fc / f，混合损失 ∝ 相位差²，渐近 q = 2；真歌（5 首，10 / 12 / 14 / 16 / 20 Hz 实测）q 在 1.5 ～ 1.8，
    // 两点幂律在 12 / 14 Hz 处与实测差 ≤ 0.006 dB（三点分段线性的精度，CPU 只要两份守护）。
    inline constexpr std::array<float, 2> clayBlendCandidateHz { 10.0f, 20.0f };
    static_assert (clayBlendCandidateHz.size() == std::tuple_size<decltype (MusicProfile::lowCutBlendLossDb)>::value,
                   "每个混合损失候选都要有一个画像特征");

    // 因「试过会抬高峰值」被减量 / 否决的项（ClayPlan::costLimitedMask、Status::clayCostLimitedMask 的位）。
    inline constexpr std::uint8_t clayCostLimitedLowCut = 1u, clayCostLimitedMono = 2u, clayCostLimitedWidth = 4u;

    // v0.1.7.0 修坯 Clay 的自动值（设计规格-Clay-分析决策与验收 第 2 节）。与 AutoIncrements 不同，这是「自动」态下
    // 交给 ClayModule 的绝对值（不是叠加在手动值上的增量，ADR-009 三态），已含 GLAZE 与置信度缩放；g = 0 时全为中性值。
    // 中性值：lowCutHz = 0（关）、monoAmount = 0、widthDb = 0；monoHz 在 monoAmount = 0 时无意义（保持默认 80）。
    struct ClayPlan
    {
        float lowCutHz = 0.0f;     // 0 = 关；否则 10 ～ 60 Hz
        float monoHz = 80.0f;      // 自动方案恒为本默认值（ADR-015 起自动不收单；v0.1.7.0 时自动为 60 ～ 160 Hz）；手动可 40 ～ 300
        float monoAmount = 0.0f;   // 0 ～ 1
        float widthDb = 0.0f;      // 自动 0 ～ +1.5 dB（只加不减）
        std::uint8_t costLimitedMask = 0;   // ADR-012：哪几项因代价被减量 / 否决（clayCostLimited* 位）；只给界面解释用，不影响处理
    };

    // v0.1.7.0 ClayGuard 的运行状态（设计规格-Clay-分析决策与验收 第 3 节）。放在这里而不是 ClayGuard.h：执行快照
    // （GlazeProfile.h 的 ExecutionState）要保存它，避免 GlazeProfile.h → ClayGuard.h → MusicAnalyzer.h 的包含链。
    inline constexpr float clayWidthUncappedDb = 100.0f;   // 宽度保护未介入时的上限（大于任何合法 widthDb）
    struct ClayGuardState
    {
        float phaseFloor = 0.0f;               // 0 ～ 1：反相提示的强度（已含攻击 / 释放；ADR-015 起不再作用于收单）
        float phaseHoldSeconds = 0.0f;         // 反相条件消失后剩余的保持时间（秒，有效音频样本口径）
        float widthCapDb = clayWidthUncappedDb;// 宽度保护此刻允许的自动 widthDb 上限（已含攻击 / 释放）
        bool phaseActive = false;              // 低频反相提示中（phaseFloor > 0；ADR-015）
        bool widthActive = false;              // 宽度保护正在介入（widthCapDb < 自动方案值）
    };

    // 方案给出的自动增量（叠加在手动值上，再钳到参数范围）。单位与对应参数相同。
    struct AutoIncrements
    {
        float balanceRecover = 0.0f;   // 0 ～ 100
        float balanceTame = 0.0f;      // 0 ～ 100
        float colorToneDb = 0.0f;      // Tone 参数单位：跷跷板两个搁架各自的 dB（不是 dB / oct）
        float colorGlue = 0.0f;        // 0 ～ 100
        float colorHeat = 0.0f;        // 0 ～ 100
        float limitGainDb = 0.0f;      // dB（不含校准偏差与保护偏移，二者由编排器另加）
        float limitClip = 0.0f;        // 0 ～ 100
    };

    // T-032 R6-02 收尾（A11）：Plan::manualBudgetMask 的位——手动链单独的用量（只有手动值、没有自动增量时）已超出这一项的风格预算。
    // 保护层与上游监视每项的有效上限 = max(风格上限, 手动单独用量)；置了位的项上自动部分恒为 0，实测用量就是手动单独用量，这个 max
    // 永不越限，编排器把它直接置为「不设约束」（GlazeController.cpp releaseManualBudget），不拿前向模型的估计当上限；没置位的项
    // 有效上限就是风格上限（逐位）：
    //   - Heat：手动 Heat 超过风格的 Heat 驱动上限（只有上游监视核对 Heat）。自动 Heat 增量此时恒为 0（余量 max(0, heatCap − 手动) = 0，
    //     heatCap ≤ 驱动上限；撤回的更是 0），Heat 驱动全是手动的；
    //   - Glue：手动 Glue 超过这份素材上两个 GR 上限都不超的最大 Glue（决策器前向模型的 glueCap；即手动链单独的 Glue 均值或 p95 GR 超过
    //     风格上限）。自动 Glue 增量此时恰为 0（撤回、或余量 max(0, glueCap − 手动) = 0），Glue GR 全是手动的——保护层退让 Limit Gain、
    //     约束反馈降方案都减不了它；
    //   - Clip：手动 Clip 单独超预算、决策器已撤回自动 Clip（与撤回同一判定，带决策器的容差）。决策器此时不再按 Clip 预算限制自动 Gain
    //     （GlazePlanner.cpp feasible，v0.6 F-02 逐模块钳住）：自动 Gain 推深的削波是方案许可的，保护层与决策器一致、不核对 Clip 深度 /
    //     活动两项（按 max(风格上限, 手动单独深度) 核对会与决策器顶牛，自动 Gain 一推就「超」，约束反馈照样把自动增益磨光）；
    //     限幅器三项照常核对。
    //   置信度封顶（GlazePlanner.cpp limitPlanTo）三位都取或：任一份方案的手动链单独超了，取小之后那一项的自动增量就是 0。
    //   位的判定与手动链单独的用量见 GlazePlanner.h estimateManualChainUsage；编排器怎样用见 GlazeController.cpp updateGuardLimits。
    inline constexpr std::uint8_t manualBudgetHeat = 1u, manualBudgetGlue = 2u, manualBudgetClip = 4u;
    inline constexpr std::uint8_t manualBudgetAll = static_cast<std::uint8_t> (manualBudgetHeat | manualBudgetGlue | manualBudgetClip);

    // 方案：决策器（纯函数）的输出。
    struct Plan
    {
        float requestedTargetLufs = 0.0f;   // 风格、交付（T-038）与 GLAZE 想要的响度
        float effectiveTargetLufs = 0.0f;   // 经各模块约束后允许追求的响度
        float requestedGainDb = 0.0f;       // G_request
        float effectiveGainDb = 0.0f;       // 约束后的净提升目标
        float estimatedExcessDb = 0.0f;     // 初始峰值超限估计 E_est（只作难度指标）
        AutoIncrements increments;
        ClayPlan clay;                      // v0.1.7.0（算法 7）：Clay「自动」态的值；旧算法方案读入时为中性值
        StyleWeights weights;
        bool limitedByConstraints = false;  // 目标因约束被降低过
        bool limitedByManual = false;       // 手动设置已超约束，该模块的自动增量已撤回（任一模块；界面「受手动设置限制」看它）
        // T-032 R6-02：电平类自动**全部**撤回——手动已让限幅器超约束、Limit 旁通、或约束反馈上限低于手动链净增益（都属 limitedByManual）。
        // 只有这时才没有自动加上去的增益；手动 Heat / Glue / Clip 单独超预算时只撤回那一项，自动 Gain 与其余模块照常展开（v0.6 F-02 逐模块
        // 钳住）。编排器的画像失配、保护层、上游监视、校准偏差与校准轮、约束反馈都只在本标志为真时停（R6-02 收尾 A11；limitedByManual
        // 只给界面）：手动单独超预算的那一项由 manualBudgetMask 交代，保护层与上游监视不核对它，只核对自动部分。
        // 存档缺本属性（v0.1.8.5 及以前）按 limitedByManual 读（旧语义 = 保守）。
        bool automaticLevelWithdrawn = false;
        // T-032 R6-02 收尾（A11）：手动链单独已超出风格预算的项（manualBudget* 位，定义见上）。编排器按它把保护层与上游监视的这几项
        // 置为「不设约束」，其余项照风格上限——修前两者按「手动 + 自动」的总量核对，把手动造成的超出当成自动的超限，白白退让自动 Gain、
        // 约束反馈把净增益上限磨到手动链（107 实测，IMPL-A7 第 3 节）。GLAZE ≤ 0 的方案为 0；手动没超预算时为 0，保护层与上游监视的上限
        // 逐位同修前。只是方案的附注：决策器的其余输出不看它（有它没它逐位相同）。存档缺本属性（A11 之前）：受手动设置限制的方案三位
        // 全置（旧方案分不出是哪一项超了，不把可能是手动造成的超出当成自动的），否则 0。
        std::uint8_t manualBudgetMask = 0;
        int algorithmVersion = currentAlgorithmVersion;
        int constantsVersion = 1;
    };

    // 模块每块统计（音频线程产出；给保护层与校准器核对「已经处理了多少」）。
    struct ModuleStats
    {
        int numSamples = 0;
        float glueGrMeanDb = 0.0f;        // 本块 Glue 压缩量均值（按 dB）
        float glueGrMaxDb = 0.0f;
        float limitGrMeanDb = 0.0f;       // 本块限幅压缩量均值（按 dB）
        float limitGrMaxDb = 0.0f;
        float clipActiveFraction = 0.0f;  // 本块被削样本占比（4× 域）
        float clipDepthMaxDb = 0.0f;      // 本块最大削波深度
        float heatDrive = 0.0f;           // 本块 Heat 实际驱动量
        float outputTruePeakDbtp = -100.0f;
        double clayPressureSeconds = 0.0; // 本块走完的修坯 10 ms 时钟格的总时长（压力反馈的计时，ADR-014；本块没走完一格为 0）
        const float* clayPeakBlocks = nullptr;   // 本块走完的修坯 100 ms 峰值块 [①② 处理后 max |y|, 干路 max |x|] × clayPeakBlockCount
        int clayPeakBlockCount = 0;              // （压力反馈第一触发量的输入，ADR-028；指向 ClayModule 内部，endBlock 之内有效）
        int clayPeakBlocksDropped = 0;           // 一次 process 装不下、没报上来的块（按无效处理）
        double clayBlendIntendedEnergy = 0.0;   // 修坯守护的混合损失测量（T-043，ADR-027；ClayModule::getLastProcessBlend）：本块「本该输出」
        double clayBlendOutputEnergy = 0.0;     // 与实际输出的 K 加权能量；损失 dB = 10·log10(本该 / 实际)
        double clayBlendSeconds = 0.0;          // 量过的时长（①② 不作用的段不量，为 0）
        double clayGuardChangeEnergy = 0.0;     // 修坯守护的保留比统计（T-074，设计 R4-03；ClayModule::getLastProcessGuardKeep）：本块①② 的改动量
        double clayGuardKeptEnergy = 0.0;       // Σd² 与守护收回一部分之后剩下的 Σ(g·d)²（不作用的格不累计，为 0）；给界面的「守护在回收」指示
        bool musicActive = false;         // 本块输入在有效信号门限之上（给「有效音乐」统计用）
    };

    // 校准参考条件里手动参数的口径（BUG-004，ADR-018）。
    inline constexpr std::uint8_t legacyConditionsFormat = 1;       // 只有 manualParamsHash：v0.1.8.0 及以前的存档
    inline constexpr std::uint8_t continuousConditionsFormat = 2;   // 另有离散哈希与三个对数刻度值，比较按新口径

    // 校准的参考条件：任一改变即失效（设计规格-GLAZE-决策与校准 第 6 节）。比较见 referenceMatches（GlazeProfile.h），
    // 手动参数这一部分由 setManualConditions（GlazeController.h）填。
    struct CalibrationReference
    {
        std::uint32_t profileId = 0;
        Style style = Style::automatic;
        float glazePercent = 0.0f;
        // 影响声音的手动参数（量化到 1e-4 后）与约束反馈上限的哈希，旧口径：含下面三个对数刻度值，逐位同 v0.1.8.0。
        // 新口径的两份参考比较时不看它（对数刻度值往返后漂几个 ulp，取整会在边界翻转）；有一份是旧存档时只能比它。
        std::uint32_t manualParamsHash = 0;
        float ceilingDb = -1.0f;
        float lookaheadMs = 2.0f;
        double sampleRate = 0.0;

        // ---- 新口径（conditionsFormat = continuousConditionsFormat）才有的项 ----
        std::uint8_t conditionsFormat = legacyConditionsFormat;
        std::uint32_t discreteParamsHash = 0; // 同 manualParamsHash，但不含下面三个对数刻度值
        float limitReleaseMs = 0.0f;          // 三个对数刻度值（ParameterRanges.h）：按相对容差比
        float clayLowCutHz = 0.0f;            // Clay 两项只在旧口径也计入时有值（Clay 非默认设置、该项「手动」），否则 0
        float clayMonoHz = 0.0f;

        // ---- T-038：曲风与交付（设计规格-曲风与交付 第 4.3、5 节）。换了任一项，方案重算、重新校准。----
        // 不进上面两个哈希（旧存档的哈希逐位不变）；存档只在不是默认值时写这两个属性，缺属性 = 通用 / 标准。
        Genre genre = Genre::general;
        Delivery delivery = Delivery::standard;
    };

    struct Calibration
    {
        float gainBiasDb = 0.0f;
        bool valid = false;
        bool partial = false;          // 保守、未完全校准
        bool budgetLimited = false;    // 保护层持续触发而结束
        CalibrationReference reference;
    };

    // 宿主信息（处理器每块从 AudioPlayHead 读；拿不到的项 has* = false）。编排器据此识别跳播 / 循环回绕、离线导出。
    struct HostInfo
    {
        bool offline = false;            // 宿主离线渲染；已有执行状态继续按同一音频样本时间运行。
        bool hasPlayState = false;
        bool isPlaying = true;
        bool hasPosition = false;
        std::int64_t positionSamples = 0;
        bool hasLoop = false;
        bool looping = false;
        std::int64_t loopStartSamples = 0;
        std::int64_t loopEndSamples = 0;

        // ---- T-038：拍点（House 档按小节记账、限幅跟拍用；设计规格-曲风-House学习方式 第 2.1、8 节）----
        // 都是本块起点的值；拿不到的项 has* = false，数值保持 0（不当真）。PPQ 按四分音符计；拍号 = 分子 / 分母。
        bool hasTempo = false;
        double bpm = 0.0;
        bool hasPpq = false;
        double ppqPosition = 0.0;
        bool hasBarStart = false;
        double ppqLastBarStart = 0.0;
        bool hasTimeSignature = false;
        int timeSignatureNumerator = 4;
        int timeSignatureDenominator = 4;
    };

    enum class Phase : int { unlearned = 0, listening = 1, planTransition = 2, calibrating = 3, locked = 4 };

    // T-048 阶段 2（Q8：PSR 单独一项）。三个守门各出一份 ≤ 0 dB 的退让，编排器取最小的那一份。
    enum class GuardCause : std::uint8_t { none = 0, budget = 1, deliveryPeak = 2, contrast = 3, psr = 4 };

    struct GuardOutput
    {
        float reliefDb = 0.0f;                 // ≤ 0
        GuardCause cause = GuardCause::none;
        bool engaged = false;                  // 正在退，或退到底仍超

        // 既有调用把退让当 float 用（轨迹、累加）。新代码读 cause / engaged。
        operator float() const noexcept { return reliefDb; }
    };

    // 给界面的状态快照（编排器每块写入原子副本，界面线程读）。
    // 阶段 2 不再跑聆听 / 锁定 / 校准状态机。下面 phase 起、到 House 锁定进度为止的旧字段仍留着，
    // 好让正在改状态行的界面先编过：它们不再被状态机驱动（phase 停在 unlearned，聆听计数为 0）。
    // 界面七条以本结构末尾的新字段为准。
    struct Status
    {
        Phase phase = Phase::unlearned;
        bool profileReady = false;
        bool planReady = false;
        bool calibrationValid = false;
        float listenedSeconds = 0.0f;
        float requiredSeconds = 15.0f;
        float effectiveTargetLufs = 0.0f;
        float protectionDb = 0.0f;           // g_保护，∈ [−3, 0]
        bool planLimited = false;            // 「方案受限」
        bool limitedByManual = false;        // 「受手动设置限制」
        bool calibrationPaused = false;      // 宿主旁通中
        bool relearnInProgress = false;      // 候选版本在建
        Style resolvedStyle = Style::clean;  // 自动时此刻偏向哪种
        bool unlearnedOfflineWarning = false;

        // v0.6 A4 追加：已提交方案是旧算法版本存的，此刻照用保存的自动增量（时间线与保存第 5 节「界面提示一次」要用；
        // 拧 GLAZE、换风格或重新聆听后按新算法重算，这一项回到 false）。
        bool savedPlanFromOlderAlgorithm = false;
        bool profileMismatch = false;        // 已提交画像与此刻的音乐严重不符（例如画像只听到安静前奏）：建议重新聆听
        bool waitingForPlayback = false;     // 宿主报告已停止播放：聆听与校准都在等音乐（状态行写「校准暂停（未播放）」）
        // ---- v0.1.7.0 修坯 Clay（ClayIndicator 与 CLAY 卡片每行的灯和数值读这些；都是上一块交给 ClayModule 的实际值）----
        float clayLowCutHz = 0.0f;           // 0 = 关
        float clayMonoHz = 80.0f;
        float clayMonoAmount = 0.0f;
        float clayWidthDb = 0.0f;
        bool clayPhaseGuardActive = false;   // 低频反相提示中（指示灯「单」变琥珀；ADR-015 起只提示、不改声音）
        bool clayWidthGuardActive = false;   // 宽度保护介入中（指示灯「宽」变琥珀）
        // T-074（设计 R4-03）：峰值守护把低切 / 收单的改动量明显收回（保留比 Σ(g·d)² / Σd² < 75 % 持续 ≥ 1 s 才亮，≥ 85 % 持续 ≥ 1 s 才熄）。
        // 手动 / 自动都报（手动项恰恰没有别的反馈）；只说①②（加宽不进守护）；灯与数值读界面的实际值，这里只给「回收中」与保留比。
        bool clayPeakGuardActive = false;
        float clayPeakGuardKept = 1.0f;      // 平滑后的保留比 0 ～ 1（没有改动时 1）；亮着时气泡写成「实际只剩约 N %」
        bool clayNeedsRelearn = false;       // 有自动态的项，但已提交画像缺立体声 / 超低频特征（旧工程）：提示点「重新聆听」
        std::uint8_t clayCostLimitedMask = 0;// ADR-012：自动态下哪几项因「试过会抬高峰值」被减量 / 否决（clayCostLimited* 位）

        // ---- T-038 House 档（设计规格-曲风-House学习方式 第 3、7、9 节；编排器填，界面状态行与 CLAY 卡片读）----
        // houseActive = 此刻曲风是 House 子类；为 false 时下面各项保持默认，界面照通用档显示。
        bool houseActive = false;
        SegmentMode houseSegmentMode = SegmentMode::none;
        int houseHeardSegments = 0;          // 不重复的已听小节（分 2 秒段时为段数）
        int houseTargetSegments = 64;        // 锁定所需（小节；分 2 秒段时界面改读下面两项秒数）
        int houseFullSegments = 0;           // 满编段
        int houseRequiredFullSegments = 8;
        float houseHeardSeconds = 0.0f;
        float houseTargetSeconds = 124.0f;
        bool houseMainSectionHeard = false;  // 满编段已够（锁定条件第 2 条）
        bool houseLockPending = false;       // 锁定条件还没满足：方案是暂定的
        bool houseLedgerFull = false;        // 段落账满了（SegmentLedger::maxSegments），之后的新段不再计入（第 2.1 节「状态行写明」）
        bool houseOfflineListening = false;  // House 档还在聆听时出现过离线导出（锁住到锁定为止）：成品中途换过方案（评审 F7，Max 2026-10-01「加状态提示」）
        bool housePhaseHint = false;         // House 档反相提示（低频相关度 < 0.6 持续够久）
        float housePhaseMonoLossDb = 0.0f;   // 提示里写的单声道低频损失（dB，≤ 0）

        // ---- T-048 阶段 2 连续调度（草图第 4 节；界面七条只读这些，不再另做一套 Phase）----
        float confidence = 0.0f;             // c。第 3、5 条
        float currentMainness = 1.0f;        // 正在播的这一段的 m。第 6 条
        bool coldExport = false;             // 离线且 c < 0.5。第 3 条
        GuardCause guardCause = GuardCause::none;
        bool guardEngaged = false;           // 正在退，或已经退到底仍超。第 4 条
        // 旧工程读档后提示一次（Q1）。不写入 LIVE，只活在本次打开里；成功读回 v2 后清掉。
        bool legacySessionDiscarded = false;
    };
}
