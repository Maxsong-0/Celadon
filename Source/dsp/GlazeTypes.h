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
    inline constexpr int currentAlgorithmVersion = 8;

    // 与本版本「行为相同」的最早算法版本。7 只在 6 上加了 Clay，而 6 保存的画像（格式 1）没有 Clay 所需特征 → Clay 中性，
    // 其余自动增量的算法没变；所以算法 6 保存的方案与执行快照照常采用、不进「照用旧增量」模式，v0.1.6.2 工程打开后
    // **在约束反馈第一次触发之前**交给模块的参数逐位不变（tests/LegacyFixtureTests 用 v0.1.6.2 真实写出的存档核对）；触发之后按修复后的
    // 上限公式（applyFeedback 不再把校准偏差算两次）与旧版有意不同（审核 V2-01）。输出因 Clay 线性相位收单整体晚 D 个样本（ADR-012）
    // （T-012 阶段 2 主进程裁决，A6 报告「请主进程裁决」一条）。
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

        double effectiveSeconds = 0.0;   // 累计有效音频时长（新增样本折算）
        float representativeness = 0.0f; // 代表性置信度 0 ～ 1：同一段反复循环不增加
        int formatVersion = 2;           // 画像数据格式版本（保存 / 读取用）；2 = 含上面的立体声 / 超低频字段（1 = v0.6～0.1.6.2）
    };

    // 三种风格的混合权重（和为 1）。「自动」按画像算出后随画像一起锁定。
    struct StyleWeights
    {
        float warm = 0.0f;
        float punchy = 0.0f;
        float clean = 1.0f;
    };

    // 先试后用的收单候选分频点（Hz，递增；ADR-012）：分析器按它们各测一个代价，决策器按对数频率在相邻两点间插值。
    inline constexpr std::array<float, 4> clayMonoCandidateHz { 60.0f, 80.0f, 120.0f, 160.0f };

    // 因「试过会抬高峰值」被减量 / 否决的项（ClayPlan::costLimitedMask、Status::clayCostLimitedMask 的位）。
    inline constexpr std::uint8_t clayCostLimitedLowCut = 1u, clayCostLimitedMono = 2u, clayCostLimitedWidth = 4u;

    // v0.1.7.0 修坯 Clay 的自动值（设计规格-Clay-分析决策与验收 第 2 节）。与 AutoIncrements 不同，这是「自动」态下
    // 交给 ClayModule 的绝对值（不是叠加在手动值上的增量，ADR-009 三态），已含 GLAZE 与置信度缩放；g = 0 时全为中性值。
    // 中性值：lowCutHz = 0（关）、monoAmount = 0、widthDb = 0；monoHz 在 monoAmount = 0 时无意义（保持默认 80）。
    struct ClayPlan
    {
        float lowCutHz = 0.0f;     // 0 = 关；否则 10 ～ 60 Hz
        float monoHz = 80.0f;      // 60 ～ 160 Hz（自动）；手动可 40 ～ 300
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
        float requestedTargetLufs = 0.0f;   // 风格与 GLAZE 想要的响度
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
        float clayPressureDb = 0.0f;      // 本块走完的修坯代价格的平均：不经守护时相关峰值会超出界多少 dB（ADR-014 压力反馈）
        double clayPressureSeconds = 0.0; // 这些代价格的总时长（本块没走完一格为 0）
        bool musicActive = false;         // 本块输入在有效信号门限之上（给「有效音乐」统计用）
    };

    // 校准参考条件里手动参数的口径（BUG-004，ADR-018）。
    inline constexpr std::uint8_t legacyConditionsFormat = 1;       // 只有 manualParamsHash：v0.1.8.0 及以前的存档
    inline constexpr std::uint8_t continuousConditionsFormat = 2;   // 另有离散哈希与三个对数刻度值，比较按新口径

    // 校准的参考条件：任一改变即失效（设计规格-GLAZE-决策与校准 第 6 节）。比较见 GlazeCalibrator::referenceMatches，
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
    };

    enum class Phase : int { unlearned = 0, listening = 1, planTransition = 2, calibrating = 3, locked = 4 };

    // 给界面的状态快照（编排器每块写入原子副本，界面线程读）。
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
        // ---- v0.1.7.0 修坯 Clay（ClayIndicator 与抽屉的实际值小圆点读这些；都是上一块交给 ClayModule 的实际值）----
        float clayLowCutHz = 0.0f;           // 0 = 关
        float clayMonoHz = 80.0f;
        float clayMonoAmount = 0.0f;
        float clayWidthDb = 0.0f;
        bool clayPhaseGuardActive = false;   // 反相保护介入中（指示灯「单」变琥珀）
        bool clayWidthGuardActive = false;   // 宽度保护介入中（指示灯「宽」变琥珀）
        bool clayNeedsRelearn = false;       // 有自动态的项，但已提交画像缺立体声 / 超低频特征（旧工程）：提示点「重新聆听」
        std::uint8_t clayCostLimitedMask = 0;// ADR-012：自动态下哪几项因「试过会抬高峰值」被减量 / 否决（clayCostLimited* 位）
    };
}
