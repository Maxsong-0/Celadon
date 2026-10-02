/*
  ==============================================================================

    GlazeConstants.cpp
    出厂标定快照（三种风格的偏好与约束）与快照的保存 / 读取（设计规格-GLAZE-决策与校准第 1～5 节、时间线与保存第 5 节）。

    三种风格的出厂值（全部待 Max 试听标定；改完 version + 1）：

      |                       | 温润 warm | 冲击 punchy | 通透 clean | 来由 |
      |-----------------------|-----------|-------------|------------|------|
      | L_style（LUFS）        | −8.5      | −9          | −10        | 规格第 2 节给定的风格偏好 |
      | 分配 Heat/Glue/Clip/Limit | .10/.30/.10/.60 | .05/.10/.35/.50 | .01/.15/.04/.80 | v0.6.2：Heat 以并联染色为主，限幅器承担主要增响 |
      | Heat 上限 / 削峰上限    | 70 / 2 dB | 50 / 1.5 dB | 20 / 1 dB | v0.6.2 并联 Heat 重新标定 |
      | Glue 平均 / p95 GR     | 2.5 / 4   | 1.0 / 2.0   | 1.5 / 2.5  | 冲击「Glue 很轻」、通透「轻 Glue」 |
      | Clip 深度 / 活动占比    | 3 / 1 %   | 6 / 3 %     | 1.5 / 0.3 % | 冲击「Clip 为主」；活动占比按实测拟合反推到与深度上限相当 |
      | Limit 平均/p95/峰值 GR  | 1.0/2.5/4 | 0.8/2.0/3   | 1.5/3.5/6  | 通透「Limit 为主」 |
      | Recover / Tame 上限     | 35 / 65   | 30 / 55     | 20 / 45    | 规格第 5 节「通透风格上限更低」；v0.4 固定偏移 40 / 40 作量级参考 |
      | Tone 上限 / 参考斜率    | 0.75 / −3.5 | 0.75 / −2.5 | 0.75 / −2.5 | 规格第 5 节 ±0.75 dB × GLAZE；温润容许更暗，提亮更少 |
      | 修坯低切目标 f_t（Hz）  | 16        | 18          | 20         | ADR-012 / 根治修订 3.3：只切听不到的次声，不碰 30 Hz 以上的音乐超低音；温润留得最多 |
      | 修坯加宽风格系数 s_w    | 0.4       | 0.6         | 1.0        | 同上第 2 节③：宽度是口味项，通透最开阔、温润最收敛 |
      | 自动 Heat 底量          | 12        | 12          | 12         | T-032 R6-01（算法 / 标定 8）：v7 前是一个全局值 12；挪成风格常数让风格差异可标定，出厂先都 12，差异待 Max 试听（12 / 8 / 4 只是候选） |

    分配权重的用法（见 GlazePlanner.cpp）：Heat / Clip / Limit 三项归一后分摊「超限估计」（削峰）；Glue 实测不降峰均比，
    它的一项解释为「每 dB 超限估计给 Glue 多少 dB 平均 GR」（压紧动态、增加密度）。算法 8 起（T-032 R6-01）Heat 只承担换算表里
    该电平真能削掉的份额（不到 Constants::heatUsefulCrestDb 就只留底量），出厂残差 Heat 表下自动 Heat 因此恒为底量 × 强度：
    温润 GLAZE 100 从 70 降到 12、默认 35 % 从 24.5 降到 4.2（可闻变化，待 Max 试听）。

    修坯 Clay 的全局常数（阈值、地板频率、分频点范围、加宽上限、相关度下限、ClayGuard 时间常数、ADR-012 的代价系数与
    收单收益斜坡）默认值就是出厂值，只写在 GlazeConstants.h；这里只写两项风格常数。快照读写时全部 clay* 字段都是「可缺省」的：
    旧快照（v0.1.6.2 及以前）没有它们 → 按 0 读 → 决策器的 Clay 三项全部中性（GlazePlanner.cpp planClay 的约定），
    旧工程打开声音不变。v0.1.7.0 开发期存的快照有 clay* 却没有 ADR-012 的代价常数 → 代价两端读成 0 / 0 →「判断不了」→
    同样三项中性（不会按旧口径的次声占比去低切）；重新聆听后换用出厂快照。
    ADR-012 删掉的 clayMonoSideRatioLowDb / HighDb、clayMonoCorrelationLow / High 不再写；开发期快照里留着的这几个属性
    读时直接忽略（读取只认字段表里的名字）。

  ==============================================================================
*/

#include "GlazeConstants.h"

#include <cstring>
#include <limits>

namespace celadon::glaze
{
    namespace
    {
        Constants makeFactoryConstants() noexcept
        {
            Constants c;   // A2 追加的字段（模型、映射、换算表）的默认值就是出厂值，这里只写风格与阶段 0 字段
            // 7 = v0.1.7.0：快照加入修坯 Clay 的常数（本文件的两项风格常数 + GlazeConstants.h 的 clay* 字段）。ADR-012 的改动
            // （低切目标、次声阈值、收单收益、代价系数）发生在 v0.1.7.0 发布之前，仍属版本 7（与算法 7 / 画像格式 2 同一口径）。
            // 8 = T-032（2026-09-29）：自动 Heat 底量挪成风格常数（三种风格出厂 12）、新常数 heatUsefulCrestDb（GlazeConstants.h，
            // 默认值即出厂值 0.1 dB）；与算法 8 同一口径（GlazeTypes.h currentAlgorithmVersion）。
            c.version = 8;
            c.algorithmVersion = currentAlgorithmVersion;

            auto& warm = c.warm;
            warm.preferredLufs = -8.5f;
            warm.heatDriveCap = 70.0f;
            warm.heatPeakReductionCapDb = 2.0f;
            warm.glueGrMeanCapDb = 2.5f;
            warm.glueGrP95CapDb = 4.0f;
            warm.clipDepthCapDb = 3.0f;
            warm.clipActivityCap = 0.01f;
            warm.limitGrMeanCapDb = 1.0f;
            warm.limitGrP95CapDb = 2.5f;
            warm.limitGrPeakCapDb = 4.0f;
            warm.recoverCap = 35.0f;
            warm.tameCap = 65.0f;
            warm.toneMaxDb = 0.75f;
            warm.toneReferenceSlopeDbPerOct = -3.5f;
            warm.shareHeat = 0.10f;
            warm.shareGlue = 0.30f;
            warm.shareClip = 0.10f;
            warm.shareLimit = 0.60f;
            warm.clayLowCutTargetHz = 16.0f;
            warm.clayWidthStyleFactor = 0.4f;
            warm.automaticHeatFloor = 12.0f;   // T-032：三种风格先同为 12（v7 前的全局底量），差异待 Max 试听标定

            auto& punchy = c.punchy;
            punchy.preferredLufs = -9.0f;
            punchy.heatDriveCap = 50.0f;
            punchy.heatPeakReductionCapDb = 1.5f;
            punchy.glueGrMeanCapDb = 1.0f;
            punchy.glueGrP95CapDb = 2.0f;
            punchy.clipDepthCapDb = 6.0f;
            punchy.clipActivityCap = 0.03f;
            punchy.limitGrMeanCapDb = 0.8f;
            punchy.limitGrP95CapDb = 2.0f;
            punchy.limitGrPeakCapDb = 3.0f;
            punchy.recoverCap = 30.0f;
            punchy.tameCap = 55.0f;
            punchy.toneMaxDb = 0.75f;
            punchy.toneReferenceSlopeDbPerOct = -2.5f;
            punchy.shareHeat = 0.05f;
            punchy.shareGlue = 0.10f;
            punchy.shareClip = 0.35f;
            punchy.shareLimit = 0.50f;
            punchy.clayLowCutTargetHz = 18.0f;
            punchy.clayWidthStyleFactor = 0.6f;
            punchy.automaticHeatFloor = 12.0f;

            auto& clean = c.clean;
            clean.preferredLufs = -10.0f;
            clean.heatDriveCap = 20.0f;
            clean.heatPeakReductionCapDb = 1.0f;
            clean.glueGrMeanCapDb = 1.5f;
            clean.glueGrP95CapDb = 2.5f;
            clean.clipDepthCapDb = 1.5f;
            clean.clipActivityCap = 0.003f;
            clean.limitGrMeanCapDb = 1.5f;
            clean.limitGrP95CapDb = 3.5f;
            clean.limitGrPeakCapDb = 6.0f;
            clean.recoverCap = 20.0f;
            clean.tameCap = 45.0f;
            clean.toneMaxDb = 0.75f;
            clean.toneReferenceSlopeDbPerOct = -2.5f;
            clean.shareHeat = 0.01f;
            clean.shareGlue = 0.15f;
            clean.shareClip = 0.04f;
            clean.shareLimit = 0.80f;
            clean.clayLowCutTargetHz = 20.0f;   // = 分析器低切候选的截止频率（代价按 20 Hz 量，三种风格的目标都不高于它）
            clean.clayWidthStyleFactor = 1.0f;
            clean.automaticHeatFloor = 12.0f;

            // T-038 三个 House 子类（设计规格-曲风与交付 第 3.1 节）：Tone 参考斜率 2026-10-01 按 Jamendo 45 首标定（每类 15 首的 House 门控
            // 画像谱斜率中位数 −2.21 / −2.19 / −2.24，取到 0.25 → 三类都 −2.25；Max 选定。起点值 −3.5 / −2.5 / −3.0 让几乎每首 Deep /
            // Melodic 都被往暗推，报告 docs/validation/2026-10-01-T038-calibration/REPORT.md）。Tone 往亮 +0.4 dB、低切 25 Hz、不自动加宽
            // 三类相同。四项都取 GenreConstants 的结构体默认值；House 共用常数与交付三档的出厂值同样就是结构体默认值。
            c.deepHouse = GenreConstants {};
            c.techHouse = GenreConstants {};
            c.melodicProgressive = GenreConstants {};

            // 阶段 0 的全局字段按结构体默认值即为出厂值：
            //   - 目标：L_pref 钳在 −14 ～ −7（规格原文）；LRA 修正 f(LRA) = (LRA − 6 LU) × 1/3 dB（6 LU 以下不修正：动态不大的歌
            //     不因此提高目标；每多 3 LU 目标降 1 dB）——【设计】待 Max 试听标定；
            //   - 满置信 0.8（置信度到此即不再收紧）——【设计】待标定；
            //   - 聆听 15 ～ 30 s、LRA 60 s 稳定、校准 3 s / 10 s / ≤ 3 dB / 两轮、保护 −3 dB / 1 dB/s / 0.3 dB/s / 1 s / 3 s / 5 s、
            //     方案过渡 3 s：规格原文给定。
            return c;
        }

        //==============================================================================
        // StyleConstants 与 Constants 的字段表：写与读共用（xmlio 说明见头文件）。新增字段只在这里加一行。
        template <typename Visitor, typename StyleT>
        void visitStyle (Visitor& v, const juce::String& prefix, StyleT& s)
        {
            auto name = [&prefix] (const char* fieldName) { return prefix + "_" + fieldName; };

            v.field (name ("preferredLufs").toRawUTF8(), s.preferredLufs);
            v.field (name ("heatDriveCap").toRawUTF8(), s.heatDriveCap);
            v.field (name ("glueGrMeanCapDb").toRawUTF8(), s.glueGrMeanCapDb);
            v.field (name ("glueGrP95CapDb").toRawUTF8(), s.glueGrP95CapDb);
            v.field (name ("clipDepthCapDb").toRawUTF8(), s.clipDepthCapDb);
            v.field (name ("clipActivityCap").toRawUTF8(), s.clipActivityCap);
            v.field (name ("limitGrMeanCapDb").toRawUTF8(), s.limitGrMeanCapDb);
            v.field (name ("limitGrP95CapDb").toRawUTF8(), s.limitGrP95CapDb);
            v.field (name ("limitGrPeakCapDb").toRawUTF8(), s.limitGrPeakCapDb);
            v.field (name ("recoverCap").toRawUTF8(), s.recoverCap);
            v.field (name ("tameCap").toRawUTF8(), s.tameCap);
            v.field (name ("toneMaxDb").toRawUTF8(), s.toneMaxDb);
            v.field (name ("shareHeat").toRawUTF8(), s.shareHeat);
            v.field (name ("shareGlue").toRawUTF8(), s.shareGlue);
            v.field (name ("shareClip").toRawUTF8(), s.shareClip);
            v.field (name ("shareLimit").toRawUTF8(), s.shareLimit);
            v.field (name ("heatPeakReductionCapDb").toRawUTF8(), s.heatPeakReductionCapDb);
            v.field (name ("toneReferenceSlopeDbPerOct").toRawUTF8(), s.toneReferenceSlopeDbPerOct);
            // v0.1.7.0 修坯 Clay：旧快照没有 → 按 0 读（该风格不低切、不加宽；见 visitConstants 末尾的说明）。
            v.optionalField (name ("clayLowCutTargetHz").toRawUTF8(), s.clayLowCutTargetHz, 0.0f);
            v.optionalField (name ("clayWidthStyleFactor").toRawUTF8(), s.clayWidthStyleFactor, 0.0f);
            // 自动 Heat 底量（算法 8 起是风格常数）不在这里：它的旧快照默认值是旧的全局字段，见 visitHeatFloor。
        }

        // 算法 8（T-032 R6-01）：自动 Heat 底量从 Constants 的一个全局值挪成三种风格各一个。存与读分开写（同 GlazeProfile.cpp 的
        // visitConditionsFormat）：
        //   - 写：三个风格字段 warm_ / punchy_ / clean_automaticHeatFloor，另把三者中最小的写成旧的全局 automaticHeatFloor——
        //     v0.1.8.5 及以前的插件读新存档只认全局字段（旧算法只有一个底量），取最小偏保守；也让「新版本写出的存档带齐旧字段表」
        //     的契约（tests/ClayGlazeTests legacyAttributes）继续成立。全局字段由三个风格字段决定，往返仍逐位相同；
        //   - 读：先读全局字段（v0.1.6.2 ～ v0.1.8.5 的快照有它；更早的没有 → 0），再读三个风格字段，缺失时都取全局值——旧快照
        //     三种风格的底量逐位等于原来的全局值（blendConstants 对三者逐位相同的底量直接取原值，混合权重下也逐位不变）。
        //     存在但损坏照样拒收（xmlio 的约定）。
        // T-038：三个 House 子类、House 共用常数、交付三档。旧快照（T-038 之前）没有这些字段 → 按出厂值读（factory 参数）：
        // 通用 + 标准用不到它们，旧工程逐位不变；旧工程切到 House 时用此刻的出厂值。写的一方忽略 factory。
        template <typename Visitor, typename GenreT>
        void visitGenre (Visitor& v, const juce::String& prefix, GenreT& g, const GenreConstants& factory)
        {
            auto name = [&prefix] (const char* fieldName) { return prefix + "_" + fieldName; };

            v.optionalField (name ("toneReferenceSlopeDbPerOct").toRawUTF8(), g.toneReferenceSlopeDbPerOct, factory.toneReferenceSlopeDbPerOct);
            v.optionalField (name ("toneMaxBrightDb").toRawUTF8(), g.toneMaxBrightDb, factory.toneMaxBrightDb);
            // 属性名不带「clay」：tests/GlazePlannerTests 的修坯快照测试按属性名里的 clay 数 32 个修坯字段，曲风覆盖的两项不算在内。
            v.optionalField (name ("lowCutTargetHz").toRawUTF8(), g.clayLowCutTargetHz, factory.clayLowCutTargetHz);
            v.optionalField (name ("widthStyleFactor").toRawUTF8(), g.clayWidthStyleFactor, factory.clayWidthStyleFactor);
        }

        template <typename Visitor, typename HouseT>
        void visitHouse (Visitor& v, HouseT& h, const HouseConstants& f)
        {
            v.optionalField ("house_gateWidthLu", h.gateWidthLu, f.gateWidthLu);
            v.optionalField ("house_lowGateFactor", h.lowGateFactor, f.lowGateFactor);
            v.optionalField ("house_lockSegments", h.lockSegments, f.lockSegments);
            v.optionalField ("house_requiredFullSegments", h.requiredFullSegments, f.requiredFullSegments);
            v.optionalField ("house_fallbackSegmentSeconds", h.fallbackSegmentSeconds, f.fallbackSegmentSeconds);
            v.optionalField ("house_fallbackLockSeconds", h.fallbackLockSeconds, f.fallbackLockSeconds);
            v.optionalField ("house_fallbackFullSeconds", h.fallbackFullSeconds, f.fallbackFullSeconds);
            v.optionalField ("house_fastTransitionSeconds", h.fastTransitionSeconds, f.fastTransitionSeconds);
            v.optionalField ("house_reopenAboveProfileLu", h.reopenAboveProfileLu, f.reopenAboveProfileLu);
            v.optionalField ("house_reopenSegments", h.reopenSegments, f.reopenSegments);
            v.optionalField ("house_maxReopens", h.maxReopens, f.maxReopens);
            v.optionalField ("house_beatReleaseDivisor", h.beatReleaseDivisor, f.beatReleaseDivisor);
            v.optionalField ("house_beatMinBpm", h.beatMinBpm, f.beatMinBpm);
            v.optionalField ("house_beatMaxBpm", h.beatMaxBpm, f.beatMaxBpm);
            v.optionalField ("house_phaseHintCorrelation", h.phaseHintCorrelation, f.phaseHintCorrelation);
            v.optionalField ("house_phaseHintSegments", h.phaseHintSegments, f.phaseHintSegments);
            v.optionalField ("house_phaseHintFallbackSeconds", h.phaseHintFallbackSeconds, f.phaseHintFallbackSeconds);
        }

        template <typename Visitor, typename DeliveryT>
        void visitDelivery (Visitor& v, DeliveryT& d, const DeliveryConstants& f)
        {
            v.optionalField ("delivery_streamingTargetLufs", d.streamingTargetLufs, f.streamingTargetLufs);
            v.optionalField ("delivery_streamingMinLufs", d.streamingMinLufs, f.streamingMinLufs);
            v.optionalField ("delivery_streamingMaxLufs", d.streamingMaxLufs, f.streamingMaxLufs);
            v.optionalField ("delivery_streamingTruePeakCapDbtp", d.streamingTruePeakCapDbtp, f.streamingTruePeakCapDbtp);
            v.optionalField ("delivery_clubTargetLufs", d.clubTargetLufs, f.clubTargetLufs);
            v.optionalField ("delivery_clubMinLufs", d.clubMinLufs, f.clubMinLufs);
            v.optionalField ("delivery_clubMaxLufs", d.clubMaxLufs, f.clubMaxLufs);
            v.optionalField ("delivery_styleOffsetReferenceLufs", d.styleOffsetReferenceLufs, f.styleOffsetReferenceLufs);
            v.optionalField ("delivery_clubLimitGrMeanExtraDb", d.clubLimitGrMeanExtraDb, f.clubLimitGrMeanExtraDb);
            v.optionalField ("delivery_clubLimitGrP95ExtraDb", d.clubLimitGrP95ExtraDb, f.clubLimitGrP95ExtraDb);
            v.optionalField ("delivery_clubLimitGrPeakExtraDb", d.clubLimitGrPeakExtraDb, f.clubLimitGrPeakExtraDb);
        }

        float legacyHeatFloorOf (const Constants& c) noexcept
        {
            return juce::jmin (juce::jmin (c.warm.automaticHeatFloor, c.punchy.automaticHeatFloor), c.clean.automaticHeatFloor);
        }

        void visitHeatFloor (xmlio::Writer& w, const Constants& c)
        {
            w.field ("automaticHeatFloor", legacyHeatFloorOf (c));
            w.field ("warm_automaticHeatFloor", c.warm.automaticHeatFloor);
            w.field ("punchy_automaticHeatFloor", c.punchy.automaticHeatFloor);
            w.field ("clean_automaticHeatFloor", c.clean.automaticHeatFloor);
        }

        void visitHeatFloor (xmlio::Reader& r, Constants& c)
        {
            float legacy = 0.0f;
            r.optionalField ("automaticHeatFloor", legacy, 0.0f);
            r.optionalField ("warm_automaticHeatFloor", c.warm.automaticHeatFloor, legacy);
            r.optionalField ("punchy_automaticHeatFloor", c.punchy.automaticHeatFloor, legacy);
            r.optionalField ("clean_automaticHeatFloor", c.clean.automaticHeatFloor, legacy);
        }

        template <typename Visitor, typename ConstantsT>
        void visitConstants (Visitor& v, ConstantsT& c)
        {
            v.field ("version", c.version);
            v.field ("algorithmVersion", c.algorithmVersion);

            visitStyle (v, "warm", c.warm);
            visitStyle (v, "punchy", c.punchy);
            visitStyle (v, "clean", c.clean);

            v.field ("lraReferenceLu", c.lraReferenceLu);
            v.field ("lraSlopeDbPerLu", c.lraSlopeDbPerLu);
            v.field ("minPreferredLufs", c.minPreferredLufs);
            v.field ("maxPreferredLufs", c.maxPreferredLufs);
            v.field ("ceilingForEstimateDb", c.ceilingForEstimateDb);
            v.field ("fullConfidence", c.fullConfidence);
            v.field ("minListenSeconds", c.minListenSeconds);
            v.field ("maxListenSeconds", c.maxListenSeconds);
            v.field ("lraStableSeconds", c.lraStableSeconds);
            v.field ("calibrationSettleSeconds", c.calibrationSettleSeconds);
            v.field ("calibrationWindowSeconds", c.calibrationWindowSeconds);
            v.field ("calibrationMaxDb", c.calibrationMaxDb);
            v.field ("calibrationRounds", c.calibrationRounds);
            v.field ("guardMinDb", c.guardMinDb);
            v.field ("guardAttackDbPerSecond", c.guardAttackDbPerSecond);
            v.field ("guardReleaseDbPerSecond", c.guardReleaseDbPerSecond);
            v.field ("guardTriggerSeconds", c.guardTriggerSeconds);
            v.field ("guardWindowSeconds", c.guardWindowSeconds);
            v.field ("guardRecoverAfterSeconds", c.guardRecoverAfterSeconds);
            v.field ("planTransitionSeconds", c.planTransitionSeconds);

            v.field ("unlearnedInputLufs", c.unlearnedInputLufs);
            v.field ("unlearnedPlrDb", c.unlearnedPlrDb);
            v.field ("unlearnedStrength", c.unlearnedStrength);
            v.field ("autoBaseWeight", c.autoBaseWeight);
            v.field ("autoTransientStrengthLowDb", c.autoTransientStrengthLowDb);
            v.field ("autoTransientStrengthHighDb", c.autoTransientStrengthHighDb);
            v.field ("autoTransientDensityLow", c.autoTransientDensityLow);
            v.field ("autoTransientDensityHigh", c.autoTransientDensityHigh);
            v.field ("autoDynamicLraLowLu", c.autoDynamicLraLowLu);
            v.field ("autoDynamicLraHighLu", c.autoDynamicLraHighLu);
            v.field ("autoBrightRangeDbPerOct", c.autoBrightRangeDbPerOct);
            v.field ("autoWarmSustainWeight", c.autoWarmSustainWeight);
            v.field ("autoWarmBrightWeight", c.autoWarmBrightWeight);
            v.field ("autoPunchyDynamicDamping", c.autoPunchyDynamicDamping);
            v.field ("glueThresholdAtFullDb", c.glueThresholdAtFullDb);
            v.field ("glueRatio", c.glueRatio);
            v.field ("glueKneeDb", c.glueKneeDb);
            v.field ("glueMakeupFraction", c.glueMakeupFraction);
            v.field ("glueDetectorOffsetDb", c.glueDetectorOffsetDb);
            v.field ("glueP95OffsetDb", c.glueP95OffsetDb);
            v.field ("glueLoudnessPerGrDb", c.glueLoudnessPerGrDb);
            v.field ("gluePeakPerGrDb", c.gluePeakPerGrDb);
            v.field ("glueLowRiskShareLow", c.glueLowRiskShareLow);
            v.field ("glueLowRiskShareHigh", c.glueLowRiskShareHigh);
            v.field ("glueLowRiskRangeLowDb", c.glueLowRiskRangeLowDb);
            v.field ("glueLowRiskRangeHighDb", c.glueLowRiskRangeHighDb);
            v.field ("glueLowRiskMaxCut", c.glueLowRiskMaxCut);
            v.field ("mismatchThresholdLu", c.mismatchThresholdLu);
            v.field ("mismatchReleaseLu", c.mismatchReleaseLu);
            v.field ("mismatchSustainSeconds", c.mismatchSustainSeconds);
            v.field ("mismatchTransitionSeconds", c.mismatchTransitionSeconds);
            v.field ("mismatchCooldownSeconds", c.mismatchCooldownSeconds);
            v.field ("heatTableTruePeakDbtp", c.heatTableTruePeakDbtp);
            v.field ("heatTableAmount", c.heatTableAmount);
            v.field ("heatLoudnessDb", c.heatLoudnessDb);
            v.field ("heatPeakDb", c.heatPeakDb);
            v.field ("clipOffThresholdDb", c.clipOffThresholdDb);
            v.field ("clipKneeRatio", c.clipKneeRatio);
            v.field ("clipActivityCoeff", c.clipActivityCoeff);
            v.field ("clipActivityExponent", c.clipActivityExponent);
            v.field ("limitLossCoeff", c.limitLossCoeff);
            v.field ("limitLossExponent", c.limitLossExponent);
            v.field ("clipLossFloor", c.clipLossFloor);
            v.field ("limitMeanCoeff", c.limitMeanCoeff);
            v.field ("limitP95KneeDb", c.limitP95KneeDb);
            v.field ("clipRampMinFraction", c.clipRampMinFraction);
            v.field ("clipMinimumDepthDb", c.clipMinimumDepthDb);
            v.field ("toneDbPerSlopeDb", c.toneDbPerSlopeDb);
            v.field ("recoverEvidenceFullDb", c.recoverEvidenceFullDb);
            v.field ("tameEvidenceFullDb", c.tameEvidenceFullDb);
            v.optionalField ("minimumFullDriveDb", c.minimumFullDriveDb, 0.0f);
            v.optionalField ("automaticRecoverFloor", c.automaticRecoverFloor, 0.0f);
            v.optionalField ("automaticTameFloor", c.automaticTameFloor, 0.0f);
            visitHeatFloor (v, c);   // 算法 8：三个风格字段 + 旧的全局字段（见上）
            v.optionalField ("liveHeatMaxReduction", c.liveHeatMaxReduction, 0.0f);
            v.optionalField ("liveGlueMaxReduction", c.liveGlueMaxReduction, 0.0f);
            v.optionalField ("liveColorDensityDeadbandDb", c.liveColorDensityDeadbandDb, 1.5f);
            v.optionalField ("liveColorDensityRangeDb", c.liveColorDensityRangeDb, 4.0f);
            v.optionalField ("liveColorBrightDeadbandDbPerOct", c.liveColorBrightDeadbandDbPerOct, 0.6f);
            v.optionalField ("liveColorBrightRangeDbPerOct", c.liveColorBrightRangeDbPerOct, 1.5f);
            v.optionalField ("liveColorAttackPerSecond", c.liveColorAttackPerSecond, 0.10f);
            v.optionalField ("liveColorReleasePerSecond", c.liveColorReleasePerSecond, 0.04f);

            // v0.1.7.0 修坯 Clay（设计规格-Clay-分析决策与验收第 2～3 节）：与 minimumFullDriveDb 等同一做法——格式版本不变，
            // 旧快照没有这些字段 → 一律按 0 读（float 字段默认值写 0.0f、double 字段写 0.0，见 xmlio::Writer::optionalField）。
            // 全 0 的 Clay 常数让决策器的三项全部中性（clayGlazeFullAt 0 → 不清理、clayWidthMaxDb 0 → 不加宽、风格低切目标 0 → 不低切），
            // 旧工程按它保存时的快照生成方案，Clay 自动值因此为 0，打开声音不变。
            v.optionalField ("clayGlazeFullAt", c.clayGlazeFullAt, 0.0f);
            v.optionalField ("claySubShareLowDb", c.claySubShareLowDb, 0.0f);
            v.optionalField ("claySubShareHighDb", c.claySubShareHighDb, 0.0f);
            v.optionalField ("clayLowCutFloorHz", c.clayLowCutFloorHz, 0.0f);
            v.optionalField ("clayLowCutMinK", c.clayLowCutMinK, 0.0f);
            v.optionalField ("clayMonoBandSideRatioDb", c.clayMonoBandSideRatioDb, 0.0f);
            v.optionalField ("clayMonoBandCorrelation", c.clayMonoBandCorrelation, 0.0f);
            v.optionalField ("clayMonoMinHz", c.clayMonoMinHz, 0.0f);
            v.optionalField ("clayMonoMaxHz", c.clayMonoMaxHz, 0.0f);
            v.optionalField ("clayMonoDefaultHz", c.clayMonoDefaultHz, 0.0f);
            v.optionalField ("clayWidthNarrowStartDb", c.clayWidthNarrowStartDb, 0.0f);
            v.optionalField ("clayWidthNarrowFullDb", c.clayWidthNarrowFullDb, 0.0f);
            v.optionalField ("clayWidthMaxDb", c.clayWidthMaxDb, 0.0f);
            v.optionalField ("clayMinCorrelationAfterWidth", c.clayMinCorrelationAfterWidth, 0.0f);
            v.optionalField ("clayGuardPhaseCorrelation", c.clayGuardPhaseCorrelation, 0.0f);
            v.optionalField ("clayGuardPhaseAttackSeconds", c.clayGuardPhaseAttackSeconds, 0.0);
            v.optionalField ("clayGuardPhaseHoldSeconds", c.clayGuardPhaseHoldSeconds, 0.0);
            v.optionalField ("clayGuardPhaseReleaseSeconds", c.clayGuardPhaseReleaseSeconds, 0.0);
            v.optionalField ("clayGuardWidthAttackSeconds", c.clayGuardWidthAttackSeconds, 0.0);
            v.optionalField ("clayGuardWidthReleaseSeconds", c.clayGuardWidthReleaseSeconds, 0.0);

            // ADR-012 修坯根治（设计规格-Clay-根治修订第 3 节）：同上，缺字段一律按 0 读。代价两端 0 / 0 不是合法的反向斜坡 →
            // 决策器按「判断不了」让三项中性、不标代价位；收单损失两端 0 / 0 → 不需要收单。ClayGuard 的快路门与最短持续秒数
            // 读成 0 时怎样解释归 ClayGuard（ClayGuard.cpp 的常数清洗）；开发期快照才会缺它们，正式发布的旧快照连 Clay 都是中性。
            v.optionalField ("clayCostFullDb", c.clayCostFullDb, 0.0f);
            v.optionalField ("clayCostZeroDb", c.clayCostZeroDb, 0.0f);
            v.optionalField ("clayMonoLossLowDb", c.clayMonoLossLowDb, 0.0f);
            v.optionalField ("clayMonoLossHighDb", c.clayMonoLossHighDb, 0.0f);
            v.optionalField ("clayGuardPhaseFastGate", c.clayGuardPhaseFastGate, 0.0f);
            v.optionalField ("clayGuardPhaseMinSeconds", c.clayGuardPhaseMinSeconds, 0.0);

            // 算法 8（T-032 R6-01）：Heat 承担削峰的门槛。旧快照缺字段按 0 读——它们的 algorithmVersion < 8，决策器永远不进用到它的
            // 分支（版本门是唯一开关，缺省值不是兼容手段；GlazeConstants.h 的说明）。
            v.optionalField ("heatUsefulCrestDb", c.heatUsefulCrestDb, 0.0f);

            // T-043（ADR-027）：守护混合损失的封顶门槛。缺字段按 0 读 → 决策器「判断不了」→ 不封顶（旧快照不变）。
            v.optionalField ("clayBlendLossLimitDb", c.clayBlendLossLimitDb, 0.0f);

            // T-038 曲风与交付（见 visitGenre 上方说明）。出厂值取一份局部副本，避免读快照时与 factoryConstants() 的静态初始化互相依赖。
            const auto factory = makeFactoryConstants();
            visitGenre (v, "deepHouse", c.deepHouse, factory.deepHouse);
            visitGenre (v, "techHouse", c.techHouse, factory.techHouse);
            visitGenre (v, "melodicProgressive", c.melodicProgressive, factory.melodicProgressive);
            visitHouse (v, c.house, factory.house);
            visitDelivery (v, c.delivery, factory.delivery);
        }

        //==============================================================================
        // 十六进制：固定位数、小写；解析严格（只认 0-9 a-f A-F、位数必须正好）。
        juce::String hexOf (std::uint64_t bits, int digits)
        {
            static constexpr char alphabet[] = "0123456789abcdef";
            char text[17] {};

            for (int i = digits - 1; i >= 0; --i)
            {
                text[i] = alphabet[bits & 0xfu];
                bits >>= 4;
            }

            return juce::String (text, (size_t) digits);
        }

        bool parseHex (juce::String::CharPointerType text, int digits, std::uint64_t& out) noexcept
        {
            std::uint64_t value = 0;

            for (int i = 0; i < digits; ++i)
            {
                const auto c = text.getAndAdvance();
                int nibble;

                if (c >= '0' && c <= '9')         nibble = (int) (c - '0');
                else if (c >= 'a' && c <= 'f')    nibble = (int) (c - 'a') + 10;
                else if (c >= 'A' && c <= 'F')    nibble = (int) (c - 'A') + 10;
                else                              return false;

                value = (value << 4) | (std::uint64_t) nibble;
            }

            out = value;
            return true;
        }

        std::uint32_t bitsOf (float value) noexcept
        {
            std::uint32_t bits;
            std::memcpy (&bits, &value, sizeof (bits));
            return bits;
        }

        std::uint64_t bitsOf (double value) noexcept
        {
            std::uint64_t bits;
            std::memcpy (&bits, &value, sizeof (bits));
            return bits;
        }

        float floatFromBits (std::uint32_t bits) noexcept
        {
            float value;
            std::memcpy (&value, &bits, sizeof (value));
            return value;
        }

        double doubleFromBits (std::uint64_t bits) noexcept
        {
            double value;
            std::memcpy (&value, &bits, sizeof (value));
            return value;
        }

        // 整数：可选负号 + 1 ～ 10 位数字，且在 int 范围内。
        bool parseInt (const juce::String& text, int& out) noexcept
        {
            auto p = text.getCharPointer();
            bool negative = false;

            if (*p == '-')
            {
                negative = true;
                ++p;
            }

            std::int64_t value = 0;
            int digits = 0;

            while (! p.isEmpty())
            {
                const auto c = p.getAndAdvance();

                if (c < '0' || c > '9' || ++digits > 10)
                    return false;

                value = value * 10 + (std::int64_t) (c - '0');
            }

            if (digits == 0)
                return false;

            value = negative ? -value : value;

            if (value < (std::int64_t) std::numeric_limits<int>::min() || value > (std::int64_t) std::numeric_limits<int>::max())
                return false;

            out = (int) value;
            return true;
        }
    }

    //==============================================================================
    namespace xmlio
    {
        void Writer::field (const char* name, const float& value)          { element.setAttribute (name, hexOf (bitsOf (value), 8)); }
        void Writer::field (const char* name, const double& value)         { element.setAttribute (name, hexOf (bitsOf (value), 16)); }
        void Writer::field (const char* name, const int& value)            { element.setAttribute (name, juce::String (value)); }
        void Writer::field (const char* name, const bool& value)           { element.setAttribute (name, value ? "1" : "0"); }
        void Writer::field (const char* name, const std::uint32_t& value)  { element.setAttribute (name, hexOf (value, 8)); }
        void Writer::field (const char* name, const Style& value)          { element.setAttribute (name, juce::String ((int) value)); }
        void Writer::optionalField (const char* name, const float& value, float) { field (name, value); }
        void Writer::optionalField (const char* name, const double& value, double) { field (name, value); }
        void Writer::field (const char* name, const std::uint8_t& value)   { element.setAttribute (name, juce::String (static_cast<int> (value))); }
        void Writer::optionalField (const char* name, const std::uint8_t& value, std::uint8_t) { field (name, value); }
        void Writer::optionalField (const char* name, const bool& value, bool) { field (name, value); }
        void Writer::optionalField (const char* name, const int& value, int) { field (name, value); }

        void Writer::writeFloats (const char* name, const float* values, int count)
        {
            juce::String text;
            text.preallocateBytes ((size_t) count * 8 + 1);

            for (int i = 0; i < count; ++i)
                text << hexOf (bitsOf (values[i]), 8);

            element.setAttribute (name, text);
        }

        void Reader::field (const char* name, float& value)
        {
            const auto text = element.getStringAttribute (name);
            std::uint64_t bits = 0;

            if (ok && element.hasAttribute (name) && text.length() == 8 && parseHex (text.getCharPointer(), 8, bits))
                value = floatFromBits ((std::uint32_t) bits);
            else
                ok = false;
        }

        void Reader::optionalField (const char* name, float& value, float legacyDefault)
        {
            if (element.hasAttribute (name))
                field (name, value);   // 存在但损坏仍拒收，只有真正缺失才走旧快照默认值。
            else
                value = legacyDefault;
        }

        void Reader::optionalField (const char* name, double& value, double legacyDefault)
        {
            if (element.hasAttribute (name))
                field (name, value);   // 同上：存在但损坏（位数不对、非十六进制）仍拒收。
            else
                value = legacyDefault;
        }

        void Reader::field (const char* name, double& value)
        {
            const auto text = element.getStringAttribute (name);
            std::uint64_t bits = 0;

            if (ok && element.hasAttribute (name) && text.length() == 16 && parseHex (text.getCharPointer(), 16, bits))
                value = doubleFromBits (bits);
            else
                ok = false;
        }

        void Reader::field (const char* name, int& value)
        {
            int parsed = 0;

            if (ok && element.hasAttribute (name) && parseInt (element.getStringAttribute (name), parsed))
                value = parsed;
            else
                ok = false;
        }

        void Reader::field (const char* name, bool& value)
        {
            const auto text = element.getStringAttribute (name);

            if (ok && element.hasAttribute (name) && (text == "0" || text == "1"))
                value = text == "1";
            else
                ok = false;
        }

        void Reader::field (const char* name, std::uint32_t& value)
        {
            const auto text = element.getStringAttribute (name);
            std::uint64_t bits = 0;

            if (ok && element.hasAttribute (name) && text.length() == 8 && parseHex (text.getCharPointer(), 8, bits))
                value = (std::uint32_t) bits;
            else
                ok = false;
        }

        void Reader::field (const char* name, std::uint8_t& value)
        {
            int parsed = 0;

            if (ok && element.hasAttribute (name) && parseInt (element.getStringAttribute (name), parsed) && parsed >= 0 && parsed <= 255)
                value = static_cast<std::uint8_t> (parsed);
            else
                ok = false;
        }

        void Reader::optionalField (const char* name, std::uint8_t& value, std::uint8_t legacyDefault)
        {
            if (element.hasAttribute (name))
                field (name, value);   // 同 float：存在但损坏（不是整数、越出 0 ～ 255）仍拒收。
            else
                value = legacyDefault;
        }

        void Reader::optionalField (const char* name, bool& value, bool legacyDefault)
        {
            if (element.hasAttribute (name))
                field (name, value);   // 同上：存在但不是 0 / 1 仍拒收。
            else
                value = legacyDefault;
        }

        void Reader::optionalField (const char* name, int& value, int legacyDefault)
        {
            if (element.hasAttribute (name))
                field (name, value);   // 同上：存在但不是十进制整数仍拒收。
            else
                value = legacyDefault;
        }

        void Reader::field (const char* name, Style& value)
        {
            int parsed = 0;

            if (ok && element.hasAttribute (name) && parseInt (element.getStringAttribute (name), parsed)
                   && parsed >= (int) Style::automatic && parsed <= (int) Style::clean)
                value = (Style) parsed;
            else
                ok = false;
        }

        void Reader::readFloats (const char* name, float* values, int count)
        {
            const auto text = element.getStringAttribute (name);

            if (! ok || ! element.hasAttribute (name) || text.length() != count * 8)
            {
                ok = false;
                return;
            }

            auto p = text.getCharPointer();

            for (int i = 0; i < count; ++i)
            {
                std::uint64_t bits = 0;

                if (! parseHex (p, 8, bits))
                {
                    ok = false;
                    return;
                }

                p += 8;
                values[i] = floatFromBits ((std::uint32_t) bits);
            }
        }
    }

    //==============================================================================
    const Constants& factoryConstants() noexcept
    {
        static const Constants constants = makeFactoryConstants();
        return constants;
    }

    const StyleConstants& constantsFor (const Constants& constants, Style concreteStyle) noexcept
    {
        switch (concreteStyle)
        {
            case Style::warm:   return constants.warm;
            case Style::punchy: return constants.punchy;
            case Style::clean:
            case Style::automatic:
            default:            return constants.clean;   // 自动不对应单一常数（决策器按权重混合）；这里只作兜底
        }
    }

    const GenreConstants* genreConstantsFor (const Constants& constants, Genre genre) noexcept
    {
        switch (genre)
        {
            case Genre::deepHouse:          return &constants.deepHouse;
            case Genre::techHouse:          return &constants.techHouse;
            case Genre::melodicProgressive: return &constants.melodicProgressive;
            case Genre::general:
            default:                        return nullptr;
        }
    }

    std::unique_ptr<juce::XmlElement> toXml (const Constants& constants)
    {
        auto xml = std::make_unique<juce::XmlElement> (constantsTag);
        xml->setAttribute ("formatVersion", constantsFormatVersion);

        xmlio::Writer writer (*xml);
        visitConstants (writer, constants);
        return xml;
    }

    bool fromXml (const juce::XmlElement& xml, Constants& out)
    {
        if (! xml.hasTagName (constantsTag))
            return false;

        // 格式版本：缺、非法、或比本版本高（新版插件存的快照可能多出本版不认识的字段）→ 读取失败。
        int format = 0;

        if (! xml.hasAttribute ("formatVersion") || ! parseInt (xml.getStringAttribute ("formatVersion"), format)
            || format < 1 || format > constantsFormatVersion)
            return false;

        // 先读进副本，全部字段都读成功才整体交出去：失败时 out 不变。
        Constants parsed = out;
        xmlio::Reader reader (xml);
        visitConstants (reader, parsed);

        if (! reader.ok)
            return false;

        out = parsed;
        return true;
    }

    bool bitwiseEqual (const Constants& a, const Constants& b)
    {
        // 字段一律按位写成十六进制，两份 XML 属性逐项相等 ⟺ 每个字段逐位相等。
        return toXml (a)->isEquivalentTo (toXml (b).get(), false);
    }
}
