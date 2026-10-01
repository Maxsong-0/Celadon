/*
  ==============================================================================

    GlazePlanner.h
    GLAZE 决策器（设计规格-GLAZE-决策与校准 第 1～5、8 节）：纯函数，同输入必同输出，无状态、不分配。

    输入：画像、风格、GLAZE 量、用户手动参数、标定快照、置信度缩放、约束反馈给出的增益上限。
    输出：方案（两个目标 + 各参数自动增量 + 风格权重 + 是否受限）。

    保证（测试按性质验证）：
      - GLAZE ≤ 0 或 NaN → 自动增量全为 0、目标 = 输入响度（+ 手动链的预计净增益；手动中性且输入不超 Ceiling 时它就是 0）；
      - 随 GLAZE 单调（各增量与 effectiveGain 不减）；
      - 永不超过各模块上限；自动增量叠加手动值后仍在参数范围内；
      - 输入已不低于风格偏好 → 仍可请求少量推进（出厂满刻度 3 dB），有效提升服从峰值与动态约束；
      - 轻量频谱修正与染色保持可用，具体动作受当前频谱、置信度、风格上限和动态预算约束；
      - 画像无效 / 置信度低 → 按置信度收紧正向增益与处理量；
      - 手动设置已超约束 → 撤回**该模块**的自动增量并标 limitedByManual（界面提示），不反向改手动值；其中手动已让限幅器超约束、
        Limit 旁通、约束反馈上限低于手动链净增益这三种「电平类自动全部撤回」另标 automaticLevelWithdrawn（T-032 R6-02）：手动
        Heat / Glue / Clip 单独超预算时自动 Gain 照常推响，编排器的画像失配、保护层、校准与约束反馈照常工作（都只看
        automaticLevelWithdrawn）；手动单独已超预算的那几项记在 manualBudgetMask（T-032 A11，位的含义见 GlazeTypes.h），保护层与
        上游监视不核对它们、只核对自动部分。手动没超预算（手动链单独的用量都不超风格上限）时 manualBudgetMask = 0；
      - 算法 8 起（T-032，按快照的 algorithmVersion 分流，旧快照逐位不变）：自动 Heat 只承担换算表里该电平真能削掉的峰均比份额、
        削不到 Constants::heatUsefulCrestDb 就只留风格底量 StyleConstants::automaticHeatFloor × 强度（出厂残差 Heat 表下恒为底量，
        与音乐无关的「顶到风格上限」不再出现）；满强度增益不超过 Limit Gain 量程（24 − 手动 Gain）能交付的净增益，超出即受约束。

    方案字段的口径（v0.6 A2 定，编排器 / 校准器 / 界面按此使用）：
      - requestedGainDb / effectiveGainDb 是整条链的预计净增益（输出响度 − 输入响度），含手动设置本身的贡献：
          requested = 手动链净增益 + GLAZE × 满强度请求（风格与 GLAZE 想要的）
          effective = 手动链净增益 + 强度 × 受约束的满强度增益（置信度、约束、maxGainDb 之后）
        校准器用 effectiveGainDb 作「目标净提升」：手动改了，目标跟着手动走，校准不会去抵消手动修改（规格第 8 节）。
        GLAZE ≤ 0 时没有方案（增量全 0、也不校准），两者都等于手动链的预计净增益：GLAZE 从 0 拧起来时净增益连续、单调。
      - requested / effectiveTargetLufs = 画像测到的输入响度（没测到用保守默认值）+ 对应的净增益；界面「目标」写 effective。
      - maxGainDb 与 effectiveGainDb 同一口径（整链净增益上限）。低于手动链净增益 → 自动全部撤回、limitedByManual 与 automaticLevelWithdrawn。
      - limitedByConstraints：满强度请求因模块约束降低过、或约束反馈 / 叠加手动后的核对压低了强度（置信度收紧不算）。
      - limitGainDb 不含校准偏差与保护偏移（二者由编排器另加）；各增量叠加手动值后仍在参数范围内（applyIncrements 钳位）。

    修坯 Clay（v0.1.7.0，算法 7；设计规格-Clay-分析决策与验收第 2 节、ADR-009；ADR-012 / 设计规格-Clay-根治修订 3.3 修订）：
    方案另给 Plan::clay——「自动」态下交给 ClayModule 的绝对值（低切频率、高频加宽；收单自动态恒为中性，ADR-015），不是叠加在手动值上的
    增量（三态合成见 ClayGuard.h resolveClayParams）。口径与保证（GlazePlanner.cpp planClay，测试按性质验证）：
      - 只看画像的立体声 / 次声特征与「先试后用」量到的峰值代价、GLAZE 与风格权重（f_t、s_w 用方案的 weights 混合）；与手动值、
        约束反馈 maxGainDb、强度 κ 都无关，也不进响度 / 峰值的前向模型（Clay 对响度的影响由校准器吸收）；反过来画像有没有 Clay
        特征也不改变方案的其余字段（逐位）；
      - 需要度：低切看真次声占比（< 16 Hz）、加宽看高频侧中比；收单自动态不做（ADR-015：收单只改立体声，单声道回放不变）；
        每项再乘代价系数 f = ramp(代价, clayCostZeroDb, clayCostFullDb)（出厂 ≤ 0.2 dB 照做、≥ 0.6 dB 不做、其间线性）；
      - GLAZE ≤ 0 或 NaN、没有画像 → ClayPlan{}（全中性）；某项所需特征无效（旧画像）或不可信、所需代价无效（没试过）、
        代价常数不是合法的反向斜坡（旧快照）→ 该项中性；
      - costLimitedMask：某项按需要度本应动作、但 f < 1 让它减量或否决时置位（clayCostLimited*），只给界面解释，不影响处理；
      - 低切、加宽都随 GLAZE 单调不减；收单比例恒为 0、分频点恒为默认 80 Hz；
      - 不超上限：低切为 0 或在 [地板, f_t] 内、加宽 0 ～ clayWidthMaxDb 且按画像预测的加宽后高频相关度不低于
        clayMinCorrelationAfterWidth；
        再都钳进 ClayModule 的参数范围；
      - 旧快照（clay* 常数缺字段按 0 读）→ 两项全部中性、costLimitedMask 为 0；
      - 置信度低的画像不比同一份画像置信度拉满时给得多（makePlan 的封顶照样覆盖 Clay 各项）。

    曲风与交付（T-038，设计规格-曲风与交付 第 3.1、4.1、4.2 节；ADR-026）：PlanInputs::genre / delivery 是方案源自己的两个开关。
      - 曲风只换音色目标：House 子类（Deep / Tech / Melodic-Progressive）在风格权重混合之后，用该子类的 GenreConstants 覆盖 Tone 参考
        斜率、修坯低切目标、自动加宽系数（三参数的 blendConstants），自动 Tone 往亮的上限改用 toneMaxBrightDb（toneBrightCapDb；往暗仍是
        toneMaxDb，同样 × GLAZE × κ）。动态预算（Heat / Glue / Clip / Limit 各上限、四个份额、Recover / Tame 上限）与偏好响度一律不动，
        自动风格的权重（resolveAutomaticWeights）也不看曲风——同一份输入下 House 与通用的方案只有 Tone 增量与 Clay 可能不同；
      - 交付只换目标响度：标准 = L_style − f(LRA) 钳在 minPreferred ～ maxPreferred（原路径）；流媒体 / 俱乐部 = 该档目标 + 风格偏移
        （混合后的 L_style − DeliveryConstants::styleOffsetReferenceLufs）− f(LRA)，钳在该档的范围里。其余步骤照旧：requested / effective
        目标与增益、约束、校准目标都按这个 L_pref 走（流媒体的 −2 dBTP 由链在交给编排器之前封进 manual 的 Ceiling，决策器照用）；
      - 通用 + 标准（以及越界的枚举值）不读任何曲风 / 交付常数，方案与 main 逐位相同。

    性能：makePlan 只有定长局部变量，不分配；典型一次约 1 µs（GLAZE 连续自动化时编排器可每块调一次，见 IMPL-A2 实测）。

  ==============================================================================
*/

#pragma once

#include "ChainParams.h"
#include "GlazeConstants.h"
#include "GlazeTypes.h"

namespace celadon::glaze
{
    struct PlanInputs
    {
        const MusicProfile* profile = nullptr;   // 可为空（未学习）：按低置信度保守方案
        Style style = Style::automatic;
        // T-038（设计规格-曲风与交付 第 3、4 节）：方案源自己的曲风与交付（方案过渡时新旧两份方案各用各的，不读 manual 里的下标）。
        // 通用 + 标准 = 与 main 逐位相同的方案。
        Genre genre = Genre::general;
        Delivery delivery = Delivery::standard;
        float glazePercent = 0.0f;               // 0 ～ 100
        ChainParams manual;             // 用户在各模块旋钮上的值（手动基准）
        const Constants* constants = nullptr;    // 标定快照（为空时用出厂常数）
        float maxGainDb = 1.0e9f;                // 约束反馈给出的净提升上限（保护层 / 实测超限时降低）；默认不限
        const StyleWeights* lockedWeights = nullptr;   // 画像锁定的风格权重（自动风格用它，不重新判断）
    };

    // 按画像算「自动」风格的混合权重（锁定画像时调一次，之后随画像保存）。置信不足时偏向通透（最保守）。
    StyleWeights resolveAutomaticWeights (const MusicProfile&, const Constants&) noexcept;

    // 本方案实际用的权重：离散风格是独热（温润 = {1, 0, 0} …），「自动」用画像锁定的 automaticWeights。
    StyleWeights weightsFor (Style, const StyleWeights& automaticWeights) noexcept;

    // 三种风格的偏好与约束按权重混合（各项线性加权）。保护层用它作生效的约束上限，决策器生成方案也用它。
    StyleConstants blendConstants (const Constants&, const StyleWeights&) noexcept;

    // T-038：风格混合之后再按曲风覆盖音色字段（设计规格-曲风与交付 第 3.1 节）——决策器生成方案实际用的常数。House 子类只替换
    // toneReferenceSlopeDbPerOct、clayLowCutTargetHz、clayWidthStyleFactor 三项；通用档（以及越界的曲风值）一项都不覆盖，逐位等于
    // 两参数版。其余字段（动态预算、偏好响度、Tone 往暗上限 toneMaxDb）两版恒逐位相同，所以保护层与上游监视用哪一版都一样。
    StyleConstants blendConstants (const Constants&, const StyleWeights&, Genre) noexcept;

    // T-038 集成（Max 2026-10-01「俱乐部档放宽预算」）：在三参数版之上，俱乐部档把风格里生效的（> 0 且有限）限幅三项上限各加
    // DeliveryConstants::clubLimitGr*ExtraDb；其余交付与其余字段逐位等于三参数版。方案（planningContext）与编排器的保护层 / 上游监视
    // 都用这一版（同一方案源的曲风与交付），运行时不会把方案许可的限幅当成超预算退让。纯函数、不分配。
    StyleConstants blendConstants (const Constants&, const StyleWeights&, Genre, Delivery) noexcept;

    // T-038：自动 Tone 满强度增量（乘 GLAZE × κ 之前）往亮的上限。通用档（以及越界的曲风值）= sc.toneMaxDb，与往暗对称（逐位同 main）；
    // House 子类 = 该子类的 GenreConstants::toneMaxBrightDb（≤ 0 或 NaN = 不许往亮，同「预算类上限」的约定）。往暗一律是 −sc.toneMaxDb；
    // sc.toneMaxDb ≤ 0 时整项 Tone 照旧不动（曲风只换往亮的边界，不重开风格关掉的 Tone）。sc = 上面任一版的混合结果。
    // 方案的 Tone 按 [−toneMaxDb, 本值] × GLAZE × κ 封顶；编排器运行中的 Tone 跟随应按同一范围封顶。纯函数、不分配。
    float toneBrightCapDb (const StyleConstants& sc, const Constants&, Genre) noexcept;

    // 生成方案。
    Plan makePlan (const PlanInputs&) noexcept;

    // 实际值 = clamp(手动值 + 自动增量)，各参数在自己的映射域叠加。返回交给模块的参数（不含校准偏差与保护偏移）。
    ChainParams applyIncrements (const ChainParams& manual, const AutoIncrements&) noexcept;

    // ---- v0.6 A2 追加 ----

    // 画像的强度系数 κ ∈ [底强度, 1]（makePlan 里正向增益与处理量就按它收紧）：置信度 = 输入响度、真峰值、代表性三者里
    // 最没把握的那个。规格要求「正向增益与校准额度一起按比例收紧」——编排器给校准器的修正范围用
    // calibrationMaxDb × GLAZE / 100 × confidenceScale(...)，与方案同一个 κ。profile 为空 = 未学习（κ = 底强度）。纯函数、不分配。
    float confidenceScale (const MusicProfile* profile, const Constants&) noexcept;

    // ---- T-032 R6-02 收尾（A11）----

    // 手动链单独的用量：只有手动值、没有任何自动增量时，按决策器同一前向模型（同一份环境：风格权重、按置信度收拢的特征、手动的非电平
    // 设置）估的各约束量，口径与 ModuleStats / 保护层 / 上游监视一致；overBudgetMask = 手动单独已超出风格预算的项（manualBudget* 位，
    // 判定见 GlazeTypes.h 与 GlazePlanner.cpp manualBudgetMaskFor）。保护层与上游监视每项的有效上限 = max(风格上限, 手动单独用量)：
    // 置位的项自动部分恒为 0，编排器按「不设约束」核对；其余项手动单独用量不超风格上限，有效上限就是风格上限（逐位）。
    struct ManualChainUsage
    {
        float heatAmount = 0.0f;                        // 手动 Heat（0 ～ 100；上游监视把它与风格驱动上限换算到同一驱动量域比）
        float glueGrMeanDb = 0.0f, glueGrP95Db = 0.0f;  // Glue 均值 / p95 GR
        float clipDepthDb = 0.0f, clipActivity = 0.0f;  // Clip 深度 / 活动占比
        float limitGrPeakDb = 0.0f, limitGrMeanDb = 0.0f, limitGrP95Db = 0.0f;
        std::uint8_t overBudgetMask = 0;
    };

    // GLAZE 与 maxGainDb 不参与。makePlan 在 GLAZE > 0 时用同一份计算填 Plan::manualBudgetMask（置信度拉满的封顶方案按位取或，见
    // GlazePlanner.cpp limitPlanTo），所以置信度满（或没有可拉满的置信度）时两者相等。纯函数、不分配；编排器不在音频线程逐块调它——
    // 掩码随方案重算（evaluate）一起算好存在方案里。
    ManualChainUsage estimateManualChainUsage (const PlanInputs&) noexcept;
}
