/*
  ==============================================================================

    GlazeConstants.h
    GLAZE 的标定快照：生成方案用到的全部常数与换算表，带版本号（设计规格-GLAZE-时间线与保存 第 5 节）。

      - 出厂常数只在 GlazeConstants 这一处（每个数只写一次，分工见下）；Max 试听后调常数时 version + 1；
      - 快照随工程保存：旧工程按它保存时的快照生成方案，常数调整不改变旧工程；
      - 序列化用 juce::XmlElement（juce_core，无头测试可用），处理器再挂进 APVTS 状态树。

    数值只写一次：
      - 阶段 0 定下的字段，结构体里的默认值是「未配置」的占位（别的单元的测试可能依赖，不改），出厂值写在 GlazeConstants.cpp；
      - v0.6 A2 追加的 Constants 字段（模型系数、模块映射、换算表），默认值就是出厂值，只在本文件写一次；
      - v0.6 A2 追加的 StyleConstants 字段，默认值是「未配置」的中性值，三种风格的出厂值写在 GlazeConstants.cpp。
    所以 Constants{} 本身也是一份可用的快照（只是三种风格都是占位）；生效的数值一律取 factoryConstants()。

    上限的约定（与 GlazeGuard 相同）：模块「效果」类上限（Glue / Limit 的 GR、Clip 深度与活动占比）≤ 0 或非有限值
    = 该项不设约束；「预算」类上限（Heat 驱动、Recover / Tame 增量、Tone 增量）≤ 0 = 自动不许动这一项。

  ==============================================================================
*/

#pragma once

#include "GlazeTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace celadon::glaze
{
    // 标定快照的数据格式版本（XML 结构，不是常数取值的版本）：读到比它高的格式 → 读取失败（按未学习处理）。
    inline constexpr int constantsFormatVersion = 1;
    // 算法 v4 的宽带容忍区；属于算法语义，旧算法保存的计划不重新解释。
    inline constexpr float automaticToneSlopeDeadbandDbPerOct = 0.35f;

    // Heat 换算表的格点（设计规格-GLAZE-决策与校准 第 5 节「驱动 → 非线性程度」表）。
    inline constexpr int heatTableLevels = 5;    // Heat 入口真峰值格点数
    inline constexpr int heatTableAmounts = 9;   // Heat 量格点数
    inline constexpr int heatTableSize = heatTableLevels * heatTableAmounts;

    // 一种风格的偏好与约束（设计规格-GLAZE-决策与校准 第 1～5 节）。
    struct StyleConstants
    {
        float preferredLufs = -10.0f;        // L_style：待试听标定的风格偏好
        // 各模块约束（上限，不是必须花完的额度）
        float heatDriveCap = 0.0f;           // Heat 参数上限（0 ～ 100）
        float glueGrMeanCapDb = 0.0f;        // 有效音乐内 Glue 平均 GR 上限
        float glueGrP95CapDb = 0.0f;         // 有效音乐内 Glue p95 GR 上限
        float clipDepthCapDb = 0.0f;         // Clip 削波深度上限
        float clipActivityCap = 0.0f;        // Clip 削波活动占比上限（0 ～ 1）
        float limitGrMeanCapDb = 3.0f;       // 3 s 窗平均 GR 上限
        float limitGrP95CapDb = 0.0f;
        float limitGrPeakCapDb = 0.0f;
        float recoverCap = 0.0f;             // Balance Recover 自动增量上限（0 ～ 100）
        float tameCap = 0.0f;                // Balance Tame 自动增量上限（0 ～ 100）
        float toneMaxDb = 0.75f;             // Tone 自动增量绝对值上限（× GLAZE）；T-038 House 曲风往亮改用 GenreConstants::toneMaxBrightDb
                                             // （GlazePlanner toneBrightCapDb），往暗仍是本项
        // 分配权重：初始方案里超限估计按此分给四个模块（只影响初始方案，最终以约束与实测为准）
        float shareHeat = 0.0f, shareGlue = 0.0f, shareClip = 0.0f, shareLimit = 1.0f;

        // ---- v0.6 A2 追加（默认值是中性占位，与上面的 heatDriveCap = 0 一致；各风格的出厂值见 GlazeConstants.cpp）----
        // Heat「标定后的非线性程度」上限：Heat 入口真峰值处被削掉的量（dB，查 Heat 换算表）。与 heatDriveCap 同时生效。
        float heatPeakReductionCapDb = 0.0f;
        // Tone 双向修正的参考频谱斜率（dB / 倍频程，Balance 段内总功率口径：粉噪 = 0）。
        float toneReferenceSlopeDbPerOct = -3.0f;
        // ---- v0.1.7.0 修坯 Clay（设计规格-Clay-分析决策与验收 第 2 节）：默认值是中性占位（0 = 该风格不低切 / 不加宽），
        // 三种风格的出厂值写在 GlazeConstants.cpp（温润 / 冲击 / 通透：低切目标 16 / 18 / 20 Hz（ADR-012：只切听不到的次声）、
        // 加宽系数 0.4 / 0.6 / 1.0）。
        // 决策器按方案的风格权重线性混合（blendConstants）：f_t = Σ w·clayLowCutTargetHz、s_w = Σ w·clayWidthStyleFactor。
        // 低切目标 ≤ Constants::clayLowCutFloorHz → 该风格不低切；加宽系数按 0 ～ 1 使用（GlazePlanner.cpp planClay）。
        float clayLowCutTargetHz = 0.0f;
        float clayWidthStyleFactor = 0.0f;
        // ---- 算法 8（T-032 R6-01，2026-09-29）：一旋钮自动 Heat 的底量（满强度请求，照常按 GLAZE / 置信度缩放、再与 Heat 有效上限取小），
        // v7 及以前是 Constants 里的一个全局值（v0.1.6.2 起 12）。挪成风格常数是为了让「风格间的 Heat 差异」变成 Max 试听时可标定的
        // 常数（原来的差异靠 HeatRow::inverse 把 Heat 顶到 heatDriveCap 的缺陷成立）；出厂三种风格先都填 12，差异待 Max 标定
        //（GlazeConstants.cpp）。默认值是中性占位 0（不加底量）。存档：写 warm_ / punchy_ / clean_automaticHeatFloor 三个字段，另把
        // 三者中最小的写成旧的全局 automaticHeatFloor（旧版本插件读新存档只认它，取最小偏保守）；读旧快照（没有风格字段）时三种风格
        // 都取全局值，blendConstants 对三者逐位相同的底量直接取原值（不经加权求和）——旧工程混合权重下也逐位不变。
        float automaticHeatFloor = 0.0f;
    };

    // ---- T-038 曲风与交付（设计规格-曲风与交付 第 3、4 节，设计规格-曲风-House学习方式；ADR-026）----
    // 默认值就是出厂值（只在这里写一次）。2026-10-01 按 Jamendo 45 首标定（规格第 3.2 节，报告 docs/validation/2026-10-01-T038-calibration/）：
    // Tone 参考斜率改成 −2.25（三类相同）；门控 ΔG / 低频门系数、64 / 8 小节、重开条件数据支持起点值、未改；其余待 Max 试听再调。
    // 标定快照存取同其他字段；旧快照缺字段按出厂值读
    // （GlazeConstants.cpp visitGenre / visitHouse / visitDelivery）——通用 + 标准用不到这些字段，旧工程逐位不变。

    // 一个 House 子类覆盖的音色字段（在风格权重混合之后覆盖；通用档不覆盖任何一项）。
    struct GenreConstants
    {
        float toneReferenceSlopeDbPerOct = -2.25f;  // 覆盖 StyleConstants::toneReferenceSlopeDbPerOct（2026-10-01 标定，三类相同）
        float toneMaxBrightDb = 0.4f;               // Tone 往亮的上限（往暗仍用 StyleConstants::toneMaxDb）
        float clayLowCutTargetHz = 25.0f;           // 覆盖 StyleConstants::clayLowCutTargetHz（ADR-026 第 5 条）
        float clayWidthStyleFactor = 0.0f;          // 覆盖 StyleConstants::clayWidthStyleFactor：House 档不自动加宽
    };

    // 三个 House 子类共用的学习方式、锁定条件、限幅跟拍、反相提示常数（设计规格-曲风-House学习方式）。
    struct HouseConstants
    {
        float gateWidthLu = 3.0f;                   // 响度门 ΔG：段响度 ≥ S_ref − 本项（第 2.3 节）
        float lowGateFactor = 0.6f;                 // 低频门：段 lowShare ≥ 低频参考 × 本项
        int lockSegments = 64;                      // 锁定条件 1：不重复的已听小节（第 3.1 节）
        int requiredFullSegments = 8;               // 锁定条件 2：满编段小节
        double fallbackSegmentSeconds = 2.0;        // 宿主没给拍点时每段秒数（第 2.1 节）
        double fallbackLockSeconds = 124.0;         // 分 2 秒段时的锁定条件 1（不重复的有效音频秒数）
        double fallbackFullSeconds = 16.0;          // 分 2 秒段时的锁定条件 2
        double fastTransitionSeconds = 0.5;         // 新响段进来时的方案过渡（第 3.2 节；与 mismatchTransitionSeconds 同速）
        float reopenAboveProfileLu = 3.0f;          // 锁定后重开：段响度 ≥ 画像 I + 本项（第 4 节）
        int reopenSegments = 4;                     //   且连续这么多小节
        int maxReopens = 2;                         //   每份画像最多重开次数
        float beatReleaseDivisor = 3.0f;            // 限幅慢段释放上限 = 一拍 ÷ 本项（第 8 节）
        float beatMinBpm = 100.0f;                  // 宿主 BPM 在 [Min, Max] 里才封顶
        float beatMaxBpm = 140.0f;
        float phaseHintCorrelation = 0.6f;          // 反相提示：慢路低频相关度 < 本项（第 9 节）
        int phaseHintSegments = 4;                  //   持续这么多小节
        double phaseHintFallbackSeconds = 8.0;      //   宿主没给拍点时改为持续这么多秒
    };

    // T-048 实时画像的常数（设计规格-实时GLAZE-主段与增益 第 2、3、6 节）。阶段 1 插件不读，只给 LiveProfile、测试与渲染工具用。
    // 注释里 ① = 库内已标定（T-038），③ = 起点值，阶段 1 在 109 上做因果重放标定后改。不进 Constants（不进标定快照与存档）。
    struct LiveModelConstants
    {
        float gateWidthLu = 3.0f;            // 响度门 ΔG：段响度在 S_ref − ΔG 处 m 的响度因子 = 0.5
        float loudnessRampLu = 1.5f;         // 响度门斜坡宽：因子 = clamp(0.5 + (L_seg − (S_ref − ΔG)) ÷ 本项, 0, 1)
        float lowGateFactor = 0.6f;          // 低频门系数（只 4/4 型用；≤ 0 = 不用低频门）
        float lowRampWidth = 0.15f;          // 低频门斜坡宽（lowShare ÷ lowRef 的单位）
        float referencePercentile = 0.95f;   // S_ref = 全部段段响度（按有效时长加权）的这个分位；lowRef 同分位
        double memoryBars = 48.0;            // W_mem：按小节分段时 = 本项 × 小节秒数（≤ 0 = 不按小节，用 memorySeconds）
        double memorySeconds = 90.0;         // W_mem：2 秒段、或 memoryBars ≤ 0 时
        double confidenceBars = 8.0;         // M0：按小节分段时 = 本项 × 小节秒数（≤ 0 = 用 confidenceSeconds）
        double confidenceSeconds = 16.0;     // M0：2 秒段、或 confidenceBars ≤ 0 时
    };

    struct LiveConstants
    {
        // 4/4 型（House 三类；以后 Techno、EDM / Trance、DnB / Bass）：ΔG 3、低频门 0.6、P95、M0 8 小节 ①；斜坡宽、W_mem ③
        LiveModelConstants fourOnFloor { 3.0f, 1.5f, 0.6f, 0.15f, 0.95f, 48.0, 90.0, 8.0, 16.0 };
        // 响度型（通用；以后 Hip-Hop / Pop、Rock / Metal）：只用响度门 ③
        LiveModelConstants loudness { 6.0f, 3.0f, 0.0f, 0.15f, 0.90f, 0.0, 90.0, 0.0, 20.0 };
        // 被动型（以后 Acoustic / Jazz、Classical、Ambient）：有效音频一律 m = 1，门与分位不用 ③
        LiveModelConstants passive { 0.0f, 1.0f, 0.0f, 0.15f, 0.90f, 0.0, 90.0, 0.0, 20.0 };

        double referenceMemorySeconds = 180.0;   // S_ref / lowRef / LRA 的直方图按有效音频时长遗忘的时间常数 ③
        float mainThreshold = 0.5f;              // 「主段」的 m 门：无主段判定、以后的棘轮都用它
        double noMainSeconds = 60.0;             // 连续这么多有效音频没有 m ≥ mainThreshold 的段 → 主段累计改按有效音频时长衰减 ③
        double noMainDecaySeconds = 120.0;       //   衰减的时间常数 ③
        float newProgramLu = 6.0f;               // 换歌软遗忘：段响度与主段电平差超过本项的段 ③
        double newProgramSeconds = 20.0;         //   连续累计这么多有效音频 ③
        float newProgramKeep = 0.2f;             //   全部累计质量 × 本项 ③
        // T-048 阶段 1 代理 A 加（LiveProfile.cpp 文件头第四节「真峰值」）：实时画像的真峰值 =「其上至少有这么多主段质量（秒，即 m × 有效时长、
        // 按进账遗忘）」的最高一格里记下的段最大值。为什么要它：House 画像取满编段的最大值，最大值本身不会遗忘，一个孤立的热峰值会永远
        // 定住真峰值；按质量设门，旧峰值随遗忘淡出，m 很小的段（breakdown 里的冲击音）也不能单独定主段的真峰值。为什么 0.5 s：不大于一个
        // 「已听」的满编段的 w（至少半小节有效音频，124 BPM 约 1 s），遗忘关掉时与 House 逐位相同；一个满主段小节（w ≈ 1.94 s）的峰值约
        // ln(1.94 ÷ 0.5) ≈ 1.4 个 W_mem 的新主段之后退出读数；m < 0.26 的单个小节不够单独定真峰值。≤ 0 = 只取最大值（不随遗忘退出）。③
        double peakTailSeconds = 0.5;
        // T-048 阶段 1 修订（Max 2026-10-02「轻的那一支要电平稳」）：换歌软遗忘「比 I 轻 newProgramLu 以上且低频因子 ≥ 0.5」那一支，只有
        // 最近累计满 newProgramSeconds 的那一串段的段响度起伏（最大 − 最小）≤ 本项才算——带低音的淡出一路往下掉，不算换歌（REPORT-C
        // 第 5 节第 4 条：deep 0944904、tech 0244162 的歌尾）。「响 newProgramLu 以上」那一支不看它。+∞ = 不要求电平稳（回到改前）。③
        float newProgramRangeLu = 2.0f;
        // T-048 阶段 1 修订（回看重判）：第 k 段的实时画像在「段尾 + 本项」这一刻发布（分析器时间轴，LiveProfile.h 文件头第二节）。
        // 要盖住段定稿的延迟（先试后用的块交齐约 0.1 s + 候选延迟 + Balance 帧晚一块）与一遍回看重算的分片；越长决策器拿到新段越晚。③
        double publishDelaySeconds = 0.5;
    };

    // 交付三档（设计规格-曲风与交付 第 4.1 节）。「标准」不用这里的任何一项：仍按风格 preferredLufs 与 min / maxPreferredLufs。
    struct DeliveryConstants
    {
        float streamingTargetLufs = -10.0f;
        float streamingMinLufs = -12.0f;
        float streamingMaxLufs = -8.0f;
        float streamingTruePeakCapDbtp = -2.0f;     // 有效 Ceiling 不高于本项（Spotify / SoundCloud：响于 −14 LUFS 时 < −2 dBTP）
        float clubTargetLufs = -7.0f;
        float clubMinLufs = -9.0f;
        float clubMaxLufs = -5.0f;
        float styleOffsetReferenceLufs = -9.0f;     // 风格偏移 = 风格混合后的 preferredLufs − 本项（冲击 = 0）
        // 俱乐部档放宽的限幅预算（Max 2026-10-01：风格预算内推不到 −7 LUFS 时「俱乐部档放宽预算」）：只在俱乐部档、只对风格里
        // 生效的（> 0）限幅三项各加这么多 dB，方案与运行时保护层 / 上游监视用同一套（GlazePlanner blendConstants 四参数版）。
        // Heat / Glue / Clip 预算不动。出厂值 2026-10-02 起为 0（T-045，ADR-029）：三种风格的预算本身已各放宽 +1 / +2 / +3 dB，正好等于原来的
        // 俱乐部预算，再叠一层会让合成 House 曲 breakdown → drop 落差少 2.2 LU（门槛 1.5）；Max 选「不再叠加」。机制保留，快照里仍可设。
        float clubLimitGrMeanExtraDb = 0.0f;
        float clubLimitGrP95ExtraDb = 0.0f;
        float clubLimitGrPeakExtraDb = 0.0f;
    };

    struct Constants
    {
        int version = 1;             // 标定快照版本
        int algorithmVersion = currentAlgorithmVersion;    // 方案算法版本（与常数分开：算法升级走兼容策略）

        StyleConstants warm, punchy, clean;

        // T-038：三个 House 子类、House 共用常数、交付三档。出厂值见 GlazeConstants.cpp makeFactory（子类的音色起点值）。
        GenreConstants deepHouse, techHouse, melodicProgressive;
        HouseConstants house;
        DeliveryConstants delivery;

        // 目标：L_pref = clamp(L_style − lraSlope × max(0, LRA − lraReferenceLu), minPreferred, maxPreferred)
        float lraReferenceLu = 6.0f;
        float lraSlopeDbPerLu = 1.0f / 3.0f;
        float minPreferredLufs = -14.0f;
        float maxPreferredLufs = -7.0f;
        float ceilingForEstimateDb = -1.0f;   // E_est 用的 Ceiling（实际以用户 Ceiling 为准）

        // 置信度：低于此值时正向增益与校准额度按比例收紧
        float fullConfidence = 0.8f;

        // 学习（有效音频秒数）
        double minListenSeconds = 15.0;
        double maxListenSeconds = 30.0;
        double lraStableSeconds = 60.0;

        // 校准
        double calibrationSettleSeconds = 3.0;   // 方案过渡后等检测器覆盖（另加链延迟）
        double calibrationWindowSeconds = 10.0;
        float calibrationMaxDb = 3.0f;            // 修正范围上限（再按 GLAZE 与置信度缩放）
        int calibrationRounds = 2;

        // 保护层
        float guardMinDb = -3.0f;
        float guardAttackDbPerSecond = 1.0f;
        float guardReleaseDbPerSecond = 0.3f;
        double guardTriggerSeconds = 1.0;        // 3 s 窗内持续超出多久才退让
        double guardWindowSeconds = 3.0;
        double guardRecoverAfterSeconds = 5.0;   // 连续有效、安全的音乐多久后才恢复

        // 过渡
        double planTransitionSeconds = 3.0;

        // ---- v0.6 A2 追加：默认值就是出厂值（只在这里写一次）----
        // 标注：【实测】= 用真实模块在合成素材上量出来拟合的（scratch 实验，数字见 IMPL-A2）；【映射】= 与模块源码的
        // 参数映射相同（GlazePlannerTests 用真实模块核对）；【设计】= 按规格定的初值。除【映射】外都待 Max 试听标定。

        // 未学习 / 置信不足时收拢到的保守默认值（设计规格-GLAZE-分析器第 6 节「不知道不当 0」、时间线第 2 节「低置信度方案」）。
        float unlearnedInputLufs = -14.0f;       // 【设计】假设素材已经偏响（常见未母带混音 −18 ～ −12）：请求增益小
        float unlearnedPlrDb = 9.0f;             // 【设计】假设素材已经偏密（真峰值 = 输入响度 + 本项）：超限估计小、削峰处理少
        float unlearnedStrength = 0.35f;         // 【设计】置信度为 0 时的强度（正向增益与处理量的比例），随置信度线性放开到 1

        // 「自动」风格的连续混合（resolveAutomaticWeights，设计规格-GLAZE-决策与校准第 1 节）。全部【设计】。
        float autoBaseWeight = 0.15f;                 // 三种风格的底权重：没有明显特征时接近均分，再按置信度偏向通透
        float autoTransientStrengthLowDb = 3.0f;      // 起音强度在此之下不算「有冲劲」（冲击权重为 0）
        float autoTransientStrengthHighDb = 9.0f;     // 起音强度到此算满
        float autoTransientDensityLow = 0.5f;         // 起音密度（次 / 秒）在此之下不算「鼓点驱动」
        float autoTransientDensityHigh = 3.0f;        // 到此算满（120 BPM 的八分音符 = 4 次 / 秒）
        float autoDynamicLraLowLu = 5.0f;             // LRA 在此之下不算「动态大」（通透权重为 0）
        float autoDynamicLraHighLu = 12.0f;           // 到此算满（原声、古典一类）
        float autoBrightRangeDbPerOct = 1.5f;         // 比通透的参考斜率亮这么多，温润（压亮、加厚）的额外权重到满
        float autoWarmSustainWeight = 0.7f;           // 温润：不靠鼓点、动态不大的持续型素材给的权重
        float autoWarmBrightWeight = 0.3f;            // 温润：偏亮给的额外权重
        float autoPunchyDynamicDamping = 0.5f;        // 冲击：动态大时权重打的折扣（动态大的留给通透）

        // Glue 模块的参数映射【映射】（ColorModule.cpp：阈值 = −24 × Glue / 100，2:1，软拐点 6 dB，补偿 = −阈值 × 0.5 × 0.5）
        float glueThresholdAtFullDb = -24.0f;
        float glueRatio = 2.0f;
        float glueKneeDb = 6.0f;
        float glueMakeupFraction = 0.5f;         // 自动补偿 = (0 − 阈值) × (1 − 1/比率) × 本项
        // Glue 在音乐上的行为【实测】（类音乐 PLR 13 与粉噪，−24 ～ −10 LUFS，Glue 10 ～ 100，块 512 统计）：
        //   平均 GR ≈ 静态曲线（2:1、6 dB 拐点）在「侧链检测电平 ≈ 入口 I − 0.5 dB」处的值（两种素材误差 < 0.3 dB）；
        //   低频占比对检测电平影响很小（类音乐 0.61、粉噪 0.20，偏差只差 0.4 dB），所以不进公式，只作风险提示（分析器第 4 节）。
        //   Glue 不降峰均比：真峰值下降比响度下降还少（20 ms 攻击放过瞬态），平均 GR 大时 PLR 反而升高 0.3 ～ 0.9 dB。
        float glueDetectorOffsetDb = -0.5f;      // 侧链检测电平（响段）≈ 入口 I + 本项
        float glueP95OffsetDb = 1.5f;            // p95 GR 按「检测电平 + 本项」算（类音乐实测 1.5、粉噪 0.3，取偏保守的一侧）
        float glueLoudnessPerGrDb = 1.05f;       // 响度损失 = 本项 × 平均 GR（类音乐 1.07、粉噪 1.00）
        float gluePeakPerGrDb = 0.95f;           // 真峰值下降 = 本项 × 平均 GR（类音乐 0.94、粉噪 1.03）

        // Glue 的低频风险（分析器第 4 节：低频占比只作风险提示，持续低音与间歇重底鼓不走同一策略）。全部【设计】。
        // 风险 = 占比坡度 × 起伏坡度 × 两项特征可信度的较小者；自动 Glue 份额 × (1 − glueLowRiskMaxCut × 风险)。
        // 间歇的重底鼓（低频占比高、逐帧起伏大）一下下推动侧链，Glue 容易泵；持续低音起伏小，侧链还有 100 Hz 高通，不折。
        // 类音乐合成素材的低频占比约 0.6、粉噪 0.2（A2 实测）；持续低音起伏几 dB，间歇底鼓十几 dB 以上（A1 实测）。
        float glueLowRiskShareLow = 0.35f;       // 低频占比在此之下不算「低频重」
        float glueLowRiskShareHigh = 0.55f;      // 到此算满
        float glueLowRiskRangeLowDb = 6.0f;      // 低频逐帧起伏在此之下算「持续」
        float glueLowRiskRangeHighDb = 14.0f;    // 到此算「间歇的重底鼓」
        float glueLowRiskMaxCut = 0.5f;          // 风险满时自动 Glue 份额最多打掉的比例（保留一半：Glue 仍按风格给密度，只是更轻）

        // 画像失配（设计规格-GLAZE-实现修订 第 3 节；v0.6 验收 F-01）。全部【设计】，待试听标定。
        // 有效音乐的 M 比方案所用画像的 I 高出 mismatchThresholdLu、累计满 mismatchSustainSeconds（低于 mismatchReleaseLu 清零）→
        // 按「这一段当画像」重算方案，它的净增益作为上限，mismatchTransitionSeconds 过渡；两次之间至少隔 mismatchCooldownSeconds。
        float mismatchThresholdLu = 10.0f;       // 段落起伏一般 2 ～ 6 LU，10 LU 以上才算「画像没听到这种音乐」
        float mismatchReleaseLu = 8.0f;
        double mismatchSustainSeconds = 1.0;
        double mismatchTransitionSeconds = 0.5;  // 比方案过渡（3 s）快：响段进来时每多一秒都是重压
        double mismatchCooldownSeconds = 2.0;

        // Heat 换算表（Heat 入口真峰值 × Heat 量 → 响度变化 / 真峰值变化，dB；含响度补偿、Mix 100 %、Tube）
        std::array<float, (size_t) heatTableLevels> heatTableTruePeakDbtp { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f };
        std::array<float, (size_t) heatTableAmounts> heatTableAmount { 0.0f, 5.0f, 10.0f, 15.0f, 20.0f, 30.0f, 40.0f, 60.0f, 100.0f };
        // [电平格点 × heatTableAmounts + Heat 格点]。实测（48 kHz、块 512）：tests/GlazeBehaviourReport 的类音乐素材
        // （底鼓 + 军鼓 + 贝斯 + 和弦，PLR 13 dB）按真峰值缩放到各格点，只开 Heat，量整体响度与真峰值的变化。
        // 算法6复测：保留原声，150 Hz高通后的M/S残差染色及高频保护，Mix 100 %、Output 0；每行相对 Heat 0。
        // 证据：docs/validation/2026-09-26-intelligent/heat-table.log。该低频主导素材未触发高频保护，45点与v5相同；改映射仍须复测。
        std::array<float, (size_t) heatTableSize> heatLoudnessDb
        {
             0.00000f, -0.00001f, -0.00001f, -0.00001f, -0.00002f, -0.00003f, -0.00004f, -0.00005f, -0.00006f,    // -18 dBTP
             0.00000f, -0.00002f, -0.00004f, -0.00006f, -0.00009f, -0.00014f, -0.00021f, -0.00037f, -0.00081f,    // -12 dBTP
             0.00000f, -0.00007f, -0.00016f, -0.00026f, -0.00037f, -0.00063f, -0.00094f, -0.00176f, -0.00422f,    // -6 dBTP
             0.00000f, -0.00029f, -0.00063f, -0.00102f, -0.00146f, -0.00251f, -0.00379f, -0.00711f, -0.01702f,    // +0 dBTP
             0.00000f, -0.00105f, -0.00226f, -0.00364f, -0.00519f, -0.00881f, -0.01313f, -0.02383f, -0.05284f    // +6 dBTP
        };
        std::array<float, (size_t) heatTableSize> heatPeakDb
        {
             0.00000f,  0.00000f,  0.00000f, -0.00000f, -0.00000f, -0.00001f, -0.00003f, -0.00008f, -0.00029f,    // -18 dBTP
             0.00000f,  0.00000f,  0.00001f,  0.00001f,  0.00001f, -0.00000f, -0.00002f, -0.00009f, -0.00040f,    // -12 dBTP
             0.00000f,  0.00001f,  0.00003f,  0.00004f,  0.00005f,  0.00006f,  0.00008f,  0.00005f, -0.00023f,    // -6 dBTP
             0.00000f,  0.00005f,  0.00011f,  0.00017f,  0.00023f,  0.00035f,  0.00047f,  0.00071f,  0.00109f,    // +0 dBTP
             0.00000f,  0.00015f,  0.00030f,  0.00045f,  0.00061f,  0.00093f,  0.00124f,  0.00184f,  0.00274f    // +6 dBTP
        };

        // Clip 模块的参数映射【映射】（LimiterModule.cpp：阈值 dB = jmap (Clip, 0 → +12, 100 → Ceiling)；Saturator SoftClip）
        float clipOffThresholdDb = 12.0f;        // Clip = 0 时的削波阈值（dBTP）；到 100 线性降到 Ceiling
        float clipKneeRatio = 0.7f;              // SoftClip：阈值 × 本项以下完全线性，之上 tanh 拐点渐近封顶在阈值
        // Clip 与限幅器的行为【实测】（类音乐与粉噪，−18 / −14 LUFS，Gain 2 ～ 12 dB，Clip 0 / 60 / 80 / 90 / 100，块 512 统计）
        float clipActivityCoeff = 2.0e-4f;       // 削波活动占比（4× 域被削样本）≈ 本项 × 深度^指数（类音乐拟合；粉噪低深度时更少）
        float clipActivityExponent = 2.8f;
        float limitLossCoeff = 0.04f;            // 只有限幅器时响度损失 ≈ 本项 × E^指数，E = 限幅入口超出 Ceiling 的量
        float limitLossExponent = 2.0f;          //   （类音乐 E 4 / 6 / 8：0.68 / 1.35 / 2.39；粉噪 0.61 / 2.07 / 3.98）
        float clipLossFloor = 0.3f;              // 超出全部由 Clip 削掉时，损失只剩只有限幅器时的这个比例（中间按限幅器份额线性）
        float limitMeanCoeff = 0.035f;           // 限幅平均 GR ≈ 本项 × 峰值 GR²（峰值 4 dB：类音乐 0.34、粉噪 0.62）
        float limitP95KneeDb = 3.0f;             // 限幅 p95 GR ≈ 峰值² / (峰值 + 本项)（比实测略高，偏保守）
        float clipRampMinFraction = 0.05f;       // 【设计】Clip 参数随强度从 0 爬到方案值的区间下限（防阶跃，见 GlazePlanner.cpp）
        float clipMinimumDepthDb = 0.25f;        // 【设计】分到 Clip 的削波深度小于它就不开 Clip（零点几 dB 的超出交给限幅器）

        // Tone 与 Balance【设计】
        float toneDbPerSlopeDb = 0.5f;           // 超出斜率容忍区后每 dB / oct 的双向 Tone 修正量
        float recoverEvidenceFullDb = 1.5f;      // Recover 证据（27 段按置信加权的均方根，dB）到此 → 增量到上限
        float tameEvidenceFullDb = 1.0f;         // Tame 证据同上（Recover 比 Tame 保守：同样的证据给得更少）

        // 一旋钮自动母带：即使学习段已够响或没有谱偏离，仍保留少量推响、实时频谱处理权限和染色。
        // 这些都是满强度的请求，随后照常按 GLAZE / 置信度缩放并受模块预算限制；缺字段的旧快照按 0 读取。
        // 自动 Heat 的底量从算法 8 起是风格常数（StyleConstants::automaticHeatFloor，出厂 12 / 12 / 12）。
        float minimumFullDriveDb = 3.0f;
        float automaticRecoverFloor = 8.0f;
        float automaticTameFloor = 24.0f;

        // ---- 算法 8（T-032 R6-01）：自动 Heat 承担削峰的门槛（dB）【设计】----
        // makeFullPlan 只让 Heat 承担它在入口真峰值处、有效上限以内真能削掉的峰均比（HeatRow 的 crest 列，与 capacityAt 同一口径）；
        // 能削的量不到本项就不为削峰开 Heat，只留风格底量。残差 Heat（算法 6 起的换算表）几乎不削峰（出厂表 crest 列 ≤ 0.00023 dB），
        // 原来 HeatRow::inverse「达不到就给表的最大 Heat」让自动 Heat 与音乐无关地顶到 heatDriveCap（70 / 50 / 20，在 E = 0 处跳 58 个单位）。
        // 门槛不能靠「缺省 0 = 旧行为」兼容：阈值 0 时 −18 dBTP 一行会从 0.00008 dB 的 crest 反查出 Heat 70——新规则只由快照的
        // algorithmVersion ≥ 8 开关，本项写进 v8 出厂值并可缺省序列化（旧快照读成 0，但它们永远不进新分支）。≤ 0 或非有限按「削不了」。
        float heatUsefulCrestDb = 0.1f;

        // v6：只收回已规划的自动染色，既不增加额外染色也不碰手动部分。
        float liveHeatMaxReduction = 0.5f;
        float liveGlueMaxReduction = 0.35f;
        float liveColorDensityDeadbandDb = 1.5f; // 当前 PSR 比画像 PLR 少这么多才视为更密集
        float liveColorDensityRangeDb = 4.0f;
        float liveColorBrightDeadbandDbPerOct = 0.6f;
        float liveColorBrightRangeDbPerOct = 1.5f;
        float liveColorAttackPerSecond = 0.10f; // 自动份额 / 秒
        float liveColorReleasePerSecond = 0.04f;
        // ---- v0.1.7.0 修坯 Clay（设计规格-Clay-分析决策与验收 第 2～3 节）：默认值就是出厂值（只在这里写一次）；全部【设计】，
        // 待 Max 真歌试听标定。旧快照缺这些字段按 0 读（Clay 自动项因此为 0；重新聆听后换用出厂快照）。
        // ramp(x, x0, x1) = clamp((x − x0) / (x1 − x0), 0, 1)（x1 < x0 即反向斜坡）；g = GLAZE / 100。
        // 决策器对取值的约定（GlazePlanner.cpp planClay；旧快照全按 0 读时三项因此全部中性）：
        //   clayGlazeFullAt ≤ 0 或非有限 → 不自动低切（ADR-015 起收单本来就恒为 0，不看它）；clayWidthMaxDb ≤ 0 → 不自动加宽；
        //   clayLowCutFloorHz ≤ 0 或风格低切目标 ≤ 它 → 不低切；斜坡两端重合（例如都是 0）→ 该项「不需要」（不当阶跃）；
        //   clayMinCorrelationAfterWidth ≥ 1 或非有限 → 一点都不加宽，≤ −1 → 不设相关度上限；
        //   代价系数（ADR-012）只认真正的反向斜坡 clayCostZeroDb > clayCostFullDb（都有限）：否则（旧快照读成 0 / 0、两端颠倒）
        //   按「判断不了」处理——三项都中性、也不标 costLimitedMask（与画像的代价特征无效同一处理，见 planClay）。
        // 三项结果最后都钳进 ClayModule 的参数范围（低切 0 或 10 ～ 60 Hz、分频点 40 ～ 300 Hz、宽度 ≤ +3 dB）。
        float clayGlazeFullAt = 0.5f;                 // 低切：强度 = min(1, g / 本项)（清理不是口味项，GLAZE 50 起给满；ADR-015 前收单也用它）
        float claySubShareLowDb = -40.0f;             // 真次声占比（< 16 Hz）need = ramp(subShareDb, 本项, High)（ADR-012）
        float claySubShareHighDb = -28.0f;
        float clayLowCutFloorHz = 10.0f;              // lowCutHz = 本项 · (f_t / 本项)^k，k = need · c · 强度 · 代价系数；k ≤ clayLowCutMinK → 关
        float clayLowCutMinK = 0.001f;
        // 已退役的 7 个自动收单常数（T-063，2026-10-04）：clayMonoBandSideRatioDb、clayMonoBandCorrelation、clayMonoMinHz、clayMonoMaxHz、
        // clayMonoDefaultHz、clayMonoLossLowDb、clayMonoLossHighDb。ADR-015（0.1.7.1）起自动态不收单，决策器不再读它们；现在连成员带
        // 快照字段一起删了：新存档不再写这 7 个属性，旧存档里残留的读时忽略（读取只认字段表里的名字）。名字不得复用——旧存档里带着它们，
        // 复用会把旧值读进新字段（GlazePlannerTests 的「旧存档带着 7 个自动收单属性」用例守着）。现行自动方案收单比例恒为 0、分频点恒为
        // ClayPlan{} 的 80 Hz。更早（ADR-012）删掉的 clayMonoSideRatioLowDb / HighDb、clayMonoCorrelationLow / High 同理：收单需要度改由收益
        // 定后，宽低音（ρ ≈ 0.8）单声道回放只少约 0.46 dB 低音，不值得收。
        float clayWidthNarrowStartDb = -10.0f;        // need_w = ramp(highSideToMidDb, 本项, Full)（越窄越需要）
        float clayWidthNarrowFullDb = -18.0f;
        float clayWidthMaxDb = 1.5f;                  // widthDb = 本项 · need_w · c · g · 风格系数（随 g 线性），再 × 代价系数
        float clayMinCorrelationAfterWidth = 0.25f;   // 加宽后预测高频相关度下限（决策器按画像、ClayGuard 按实时视图）
        // ClayGuard
        // 慢路低频相关度低于本项 → 反相提示（修坯「收单」灯那条；House 档的 housePhaseHint 另看 house.phaseHintCorrelation）。
        // ADR-012 定 −0.2：单声道回放低音损失 > 4 dB；ADR-015 起只提示、不改声音。
        float clayGuardPhaseCorrelation = -0.2f;
        double clayGuardPhaseAttackSeconds = 0.1;
        double clayGuardPhaseHoldSeconds = 2.0;
        double clayGuardPhaseReleaseSeconds = 2.0;
        double clayGuardWidthAttackSeconds = 0.2;
        double clayGuardWidthReleaseSeconds = 2.0;
        // ---- ADR-012 修坯根治（设计规格-Clay-根治修订 第 3 节）：默认值就是出厂值；【设计】，待真歌标定 ----
        // 代价 = 画像里「候选处理跑在原始输入上」量到的 100 ms 块峰值 P99 变化（dB，GlazeTypes.h MusicProfile 的 *PeakCostDb）；
        // 代价系数 f = ramp(代价, clayCostZeroDb, clayCostFullDb)（反向斜坡），乘在低切、加宽的需要度之后（ADR-015 起收单自动态不做，
        // 收单代价照测不用）。快照读写同其他 clay*：缺字段按 0 读——代价两端 0 / 0 → 判断不了 → 三项中性。（ADR-012 的收单损失两端
        // clayMonoLossLowDb / HighDb 已随 ADR-015 的退役一起删，见上；决策器算 monoLossDb 的代码已在 T-018 删除。ClayGuard 的 House 反相提示
        // （ClayGuard.cpp houseMonoLossDb）用的是同一个公式，但亮起的门限只读 house.phaseHintCorrelation，与这两项无关。）
        float clayCostFullDb = 0.2f;                  // 先试后用：代价 ≤ 本项 → 照做（系数 1）
        float clayCostZeroDb = 0.6f;                  // 代价 ≥ 本项 → 不做（系数 0）；两者之间线性
        // 慢路 < 本项时，快路 < clayGuardPhaseCorrelation 也算触发。ADR-014（2026-09-26 真歌门槛）：0.3 → 0.1——慢路在 0.1 ～ 0.3
        // 徘徊的宽低音（ST、Sometimes：慢路从未 < −0.2）会被 120 ms 快路的估计散布各误触发一次；0.1 把它们压到 0 次，真反相的歌
        // 保护时长不变，整体反相的判定只晚约 0.05 s（慢路过 0.1 约 0.18 s，仍在验收 4(c) 的 0.5 s 之内）。
        float clayGuardPhaseFastGate = 0.1f;
        double clayGuardPhaseMinSeconds = 0.15;       // 触发条件须连续成立这么久才开始攻击
        // ---- T-043 修坯峰值守护的混合损失（ADR-027）：默认值就是出厂值 ----
        // 守护把低切拉回干路时，与干路相位不同处互相抵消，直接是响度损失（真歌 woqu −0.2 LU）。画像里两个候选频率的混合损失
        // （MusicProfile::lowCutBlendLossDb，clayBlendCandidateHz 10 / 20 Hz）按幂律预测（两端之外照同一条线外推）；低切频率封顶在
        // 「预测损失 = 本项」处，地板频率处已超 → 不低切（GlazePlanner.cpp planClay）。0.09 = 修坯门槛的响度容差 0.1 LU 减去试用
        // （原始输入）对链内实测约 ±10 % 的偏差（真歌：woqu 链内比试用高 3 ～ 6 %，somggggg / mp3 低约 10 %）——封在 0.1 上，woqu 这类
        // 歌链内会擦着门槛。【设计】，待 Max 试听标定。缺字段按 0 读 / ≤ 0 / 非有限 → 判断不了 → 不封顶（与代价两端 0 / 0 的「中性」
        // 不同：这是在已有决策上加的限制，判断不了就不加——旧快照、旧画像一样都不变）。
        float clayBlendLossLimitDb = 0.09f;
    };

    // 修坯 Clay 加宽的相关度约束（设计规格-Clay-分析决策与验收第 2 节③、第 3 节）：加宽 γ = 10^(w/20) 后预测的高频相关度
    // ρ' = (P_M − γ²P_S) / (P_M + γ²P_S) ≥ ρ_min ⟺ γ² ≤ k · P_M / P_S，k = (1 − ρ_min) / (1 + ρ_min)（ρ_min 0.25 → 0.6）。
    // 决策器（按画像）与 ClayGuard（按实时视图）共用这一处（T-012 阶段 2，A5 报告建议）。ρ_min ≥ 1 或非有限 → 0（不许加宽，
    // 坏快照从严）；ρ_min ≤ −1 → +∞（不设上限）。
    inline double clayWidthPowerScale (float minCorrelation) noexcept
    {
        if (! (minCorrelation < 1.0f))   // 含 NaN
            return 0.0;

        if (! (minCorrelation > -1.0f))
            return std::numeric_limits<double>::infinity();

        return (1.0 - static_cast<double> (minCorrelation)) / (1.0 + static_cast<double> (minCorrelation));
    }

    // 出厂常数（GlazeConstants.cpp）。
    const Constants& factoryConstants() noexcept;

    // 某风格的常数（automatic 不对应单一常数：由决策器按权重混合）。
    const StyleConstants& constantsFor (const Constants&, Style concreteStyle) noexcept;

    // T-038：某曲风覆盖的音色字段；通用档返回 nullptr（不覆盖）。
    const GenreConstants* genreConstantsFor (const Constants&, Genre) noexcept;

    // 保存 / 读取（读取失败返回 false，out 不变）。
    std::unique_ptr<juce::XmlElement> toXml (const Constants&);
    bool fromXml (const juce::XmlElement&, Constants& out);

    // ---- v0.6 A2 追加 ----

    // 标定快照在 XML 里的节点名。
    inline constexpr const char* constantsTag = "GLAZE_CONSTANTS";

    // 两份快照逐位相同（浮点按位比较：NaN、±0 都区分）。测试与编排器（消息线程）判断「快照变了没有」用；会分配，别在音频线程调。
    bool bitwiseEqual (const Constants& a, const Constants& b);

    //==============================================================================
    // 存取的内部工具（GlazeConstants.cpp 与 GlazeProfile.cpp 共用，别的单元不要依赖）：
    //   - float / double 写成 IEEE 位模式的十六进制（8 / 16 位）：往返逐位相同（含 NaN、±0），与宿主的区域设置
    //     （小数点是逗号的系统）无关；整数写十进制，布尔写 0 / 1，std::uint32_t 写 8 位十六进制，std::uint8_t（位掩码）写十进制 0 ～ 255；
    //   - 读取严格：缺属性、位数不对、有非法字符都算失败（ok = false），调用方据此整体放弃、不改 out。
    //     唯一例外是 optionalField：属性真正缺失（旧快照没有这个字段）→ 取 legacyDefault；存在但损坏照样失败。
    //   - 同一个 visit 模板既写又读（写的一方传 const 对象），字段表只列一次，存与读不会漏项或错位。
    namespace xmlio
    {
        class Writer
        {
        public:
            explicit Writer (juce::XmlElement& target) noexcept : element (target) {}

            void field (const char* name, const float& value);
            void field (const char* name, const double& value);
            void field (const char* name, const int& value);
            void field (const char* name, const bool& value);
            void field (const char* name, const std::uint32_t& value);
            void field (const char* name, const Style& value);
            void optionalField (const char* name, const float& value, float legacyDefault);
            // v0.1.7.0 A5 追加：double 字段（ClayGuard 的时间常数）同样可以「旧快照缺字段按默认值读」。
            // 默认值字面量的类型必须与字段一致（float 字段写 0.0f、double 字段写 0.0）：混用时 float / double 两个重载
            // 一样好，编译器报二义性。
            void optionalField (const char* name, const double& value, double legacyDefault);
            // ADR-012 追加：小整数（ClayPlan::costLimitedMask 这类位掩码），写十进制 0 ～ 255。字段类型必须正好是 std::uint8_t
            // （别的整数类型会落到 int / float 重载上）。
            void field (const char* name, const std::uint8_t& value);
            void optionalField (const char* name, const std::uint8_t& value, std::uint8_t legacyDefault);
            // T-032 R6-02 追加：布尔字段（Plan::automaticLevelWithdrawn）同样可以「旧存档缺属性按默认值读」；默认值可以是另一个
            // 已读到的字段（visitPlan 里传 plan.limitedByManual），写的一方忽略它。字段类型必须正好是 bool。
            void optionalField (const char* name, const bool& value, bool legacyDefault);
            // T-038 追加：整数字段（House 常数里的小节数）同样可以「旧快照缺字段按默认值读」。字段类型必须正好是 int。
            void optionalField (const char* name, const int& value, int legacyDefault);

            template <size_t N>
            void field (const char* name, const std::array<float, N>& values)
            {
                writeFloats (name, values.data(), (int) N);
            }

        private:
            void writeFloats (const char* name, const float* values, int count);
            juce::XmlElement& element;
        };

        class Reader
        {
        public:
            explicit Reader (const juce::XmlElement& source) noexcept : element (source) {}

            void field (const char* name, float& value);
            void field (const char* name, double& value);
            void field (const char* name, int& value);
            void field (const char* name, bool& value);
            void field (const char* name, std::uint32_t& value);
            void field (const char* name, Style& value);
            void optionalField (const char* name, float& value, float legacyDefault);
            void optionalField (const char* name, double& value, double legacyDefault);   // v0.1.7.0 A5 追加（同 Writer）
            // ADR-012 追加（同 Writer）：十进制整数且在 0 ～ 255 内，否则失败。
            void field (const char* name, std::uint8_t& value);
            void optionalField (const char* name, std::uint8_t& value, std::uint8_t legacyDefault);
            void optionalField (const char* name, bool& value, bool legacyDefault);   // T-032 追加（同 Writer）：存在但不是 0 / 1 仍拒收
            void optionalField (const char* name, int& value, int legacyDefault);     // T-038 追加（同 Writer）：存在但不是十进制整数仍拒收

            template <size_t N>
            void field (const char* name, std::array<float, N>& values)
            {
                readFloats (name, values.data(), (int) N);
            }

            bool ok = true;

        private:
            void readFloats (const char* name, float* values, int count);
            const juce::XmlElement& element;
        };
    }
}
