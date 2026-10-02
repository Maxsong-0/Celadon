/*
  ==============================================================================

    GlazeProfile.cpp
    画像哈希与已提交版本的保存 / 读取（设计规格-GLAZE-时间线与保存第 5 节）。

    XML 结构（浮点按位写十六进制，见 GlazeConstants.h 的 xmlio）：
      <GLAZE_STATE formatVersion hasProfile profileId lockedWarm lockedPunchy lockedClean>
        <GLAZE_PROFILE formatVersion …画像全部字段…/>
        <PLAN …方案全部字段…/>
        <CALIBRATION …校准值、有效性与参考条件…/>
        <GLAZE_CONSTANTS formatVersion …标定快照全部字段…/>
      </GLAZE_STATE>
    未学习（hasProfile = false）时子节点照样写全：读回来与存进去逐位相同，编排器不必区分。

    v0.1.7.0 修坯 Clay（设计规格-Clay 第 7 节、ADR-009；A6）：
      - 画像格式 2：GLAZE_PROFILE 多存立体声 / 超低频特征（visitProfileClay）。画像按它自己的 formatVersion 写——格式 1 的
        旧画像重存仍是格式 1，不凭空补出一组「无效」的新字段（否则重存一次画像号就变了）；读格式 1 时新字段一律「不知道」
        （valid = false、数组 0，就算读进来的目标原本有值也清掉），决策器据此给 Clay 自动值 0，旧工程打开声音不变。
        格式 2 起新字段与其他字段同样严格：缺一个就整份拒收。
      - computeProfileId：新字段只在格式 ≥ 2 时进哈希，而且接在原有字段之后——格式 1 的画像号与 v0.1.6.2 逐位相同。
        画像号在校准参考条件里：它若变了，旧工程重开后校准不适用、偏差撤回，声音就变了。
      - PLAN 多存 Clay 四项（「自动」态的绝对值，Plan::clay）；EXECUTION 多存 liveClay（保护之后实际执行的自动值）与
        ClayGuard 状态。旧存档缺这些属性 = 中性（ClayPlan{} / ClayGuardState{}）；属性在但损坏照样拒收（与标定快照的
        可选字段同一口径）。执行快照里的 Clay 值另按 ClayModule 的参数范围核对，坏值整份拒收（与其他执行量同一做法）。
    ADR-012 修坯根治（设计规格-Clay-根治修订第 3 节；v0.1.7.0 未发布，画像格式仍为 2，字段在发布前追加）：
      - 画像格式 2 追加「先试后用」的三类峰值代价（lowCutPeakCostDb、monoPeakCostDb0 ～ 3、widthPeakCostDb），与格式 2 的
        其他字段同样严格（缺一个整份拒收——开发期存的格式 2 画像没有它们、次声占比也是旧口径，读不进就按未学习、重新聆听）；
        格式 1 读入后同样清成「不知道」。computeProfileId 把它们接在格式 2 字段的最后（格式 1 的画像号仍与 v0.1.6.2 逐位相同）。
      - PLAN 多存 clayCostLimitedMask、EXECUTION 多存 liveClayCostLimitedMask（ClayPlan::costLimitedMask，十进制）：缺属性 = 0；
        属性在但不是 0 ～ 255 的整数照样拒收；执行快照另核对只含已定义的三位。
    BUG-004（T-026，ADR-018）：CALIBRATION 与执行快照的 REFERENCE 按新口径另存 conditionsFormat="2"、discreteParamsHash 与
      三个对数刻度值（limitReleaseMs、clayLowCutHz、clayMonoHz）；口径号为 2 时四项缺一即拒收，口径号不是 1 / 2 拒收。
      旧口径的参考（旧存档读回、还没升格）一个新属性也不写，存出来与修前逐字相同；manualParamsHash 两种口径都写（旧口径哈希）。
    T-032 R6-02（2026-09-29）：PLAN 多存 automaticLevelWithdrawn（电平类自动全部撤回；编排器的画像失配看它，见 GlazeTypes.h）。
      缺属性 = 取同一节点里的 limitedByManual（旧语义，保守）；在但不是 0 / 1 拒收。执行快照的 PLAN 同一字段表。
    T-032 R6-02 收尾（A11）：PLAN 多存 manualBudgetMask（手动链单独已超预算的项，保护层与上游监视不核对它们，见 GlazeTypes.h；十进制）。
      缺属性 = 受手动设置限制的方案三位全置、否则 0；在但不是 0 ～ 255 的整数拒收。已提交版本里未定义的位照读（编排器只认三个已定义的位，
      与 clayCostLimitedMask 同一做法）；执行快照的 PLAN 含未定义的位整份拒收（与其他执行量同一做法）。

  ==============================================================================
*/

#include "GlazeProfile.h"

#include "ClayModule.h"

#include <cmath>
#include <limits>

namespace celadon::glaze
{
    namespace
    {
        juce::String join (const char* prefix, const char* field)
        {
            return juce::String (prefix) + "_" + field;
        }

        //==============================================================================
        // 字段表（写与读共用；新增字段只在这里加一行）。
        template <typename Visitor, typename FeatureT>
        void visitFeature (Visitor& v, const char* prefix, FeatureT& f)
        {
            v.field (join (prefix, "value").toRawUTF8(), f.value);
            v.field (join (prefix, "valid").toRawUTF8(), f.valid);
            v.field (join (prefix, "seconds").toRawUTF8(), f.effectiveSeconds);
            v.field (join (prefix, "confidence").toRawUTF8(), f.confidence);
        }

        template <typename Visitor, typename ProfileT>
        void visitProfile (Visitor& v, ProfileT& p)
        {
            visitFeature (v, "integratedLufs", p.integratedLufs);
            visitFeature (v, "loudnessRangeLu", p.loudnessRangeLu);
            visitFeature (v, "plrDb", p.plrDb);
            visitFeature (v, "truePeakDbtp", p.truePeakDbtp);
            visitFeature (v, "spectralSlopeDbPerOct", p.spectralSlopeDbPerOct);
            visitFeature (v, "lowShare", p.lowShare);
            visitFeature (v, "highShare", p.highShare);
            visitFeature (v, "lowBandRangeDb", p.lowBandRangeDb);
            visitFeature (v, "onsetDensityPerSecond", p.onsetDensityPerSecond);
            visitFeature (v, "onsetStrengthDb", p.onsetStrengthDb);

            v.field ("bandLongTermDb", p.bandLongTermDb);
            v.field ("tameEvidenceDb", p.tameEvidenceDb);
            v.field ("recoverEvidenceDb", p.recoverEvidenceDb);
            v.field ("bandConfidence", p.bandConfidence);

            v.field ("effectiveSeconds", p.effectiveSeconds);
            v.field ("representativeness", p.representativeness);
        }

        // 收单代价的属性前缀：第 i 个 = clayMonoCandidateHz[i]（60 / 80 / 120 / 160 Hz）的候选（GlazeTypes.h MusicProfile）。
        constexpr const char* monoPeakCostNames[] = { "monoPeakCostDb0", "monoPeakCostDb1", "monoPeakCostDb2", "monoPeakCostDb3" };
        static_assert (sizeof (monoPeakCostNames) / sizeof (monoPeakCostNames[0])
                           == std::tuple_size<decltype (MusicProfile::monoPeakCostDb)>::value,
                       "每个收单候选的代价都要有属性名");

        // 画像格式 2（v0.1.7.0 修坯 Clay，设计规格-Clay-分析决策与验收 1.3；ADR-012 追加三类峰值代价）追加的字段：只在
        // formatVersion ≥ clayProfileFormatVersion 时写 / 读（格式 1 的 XML 里根本没有它们）。
        template <typename Visitor, typename ProfileT>
        void visitProfileClay (Visitor& v, ProfileT& p)
        {
            visitFeature (v, "subShareDb", p.subShareDb);
            visitFeature (v, "lowSideToMidDb", p.lowSideToMidDb);
            visitFeature (v, "lowCorrelation", p.lowCorrelation);
            visitFeature (v, "highSideToMidDb", p.highSideToMidDb);
            visitFeature (v, "highCorrelation", p.highCorrelation);

            v.field ("bandSideToMidDb", p.bandSideToMidDb);
            v.field ("bandCorrelation", p.bandCorrelation);
            v.field ("bandStereoConfidence", p.bandStereoConfidence);

            // 先试后用的峰值代价（ADR-012）：与上面同样严格，格式 2 缺一个就整份拒收。
            visitFeature (v, "lowCutPeakCostDb", p.lowCutPeakCostDb);

            for (std::size_t i = 0; i < p.monoPeakCostDb.size(); ++i)
                visitFeature (v, monoPeakCostNames[i], p.monoPeakCostDb[i]);

            visitFeature (v, "widthPeakCostDb", p.widthPeakCostDb);
        }

        // 画像格式 3（T-038 House 档门控结果，设计规格-曲风-House学习方式 第 5 节）：只在 formatVersion ≥ houseProfileFormatVersion 时
        // 写 / 读，与格式 2 的字段同样严格（缺一个就整份拒收）。分段方式是枚举，按 0 ～ 2 的小整数存，存在但越界照样拒收。
        void visitSegmentMode (xmlio::Writer& w, const SegmentMode& mode)
        {
            w.field ("houseGateMode", static_cast<std::uint8_t> (mode));
        }

        void visitSegmentMode (xmlio::Reader& r, SegmentMode& mode)
        {
            std::uint8_t value = 0;
            r.field ("houseGateMode", value);

            if (value > static_cast<std::uint8_t> (SegmentMode::seconds))
                r.ok = false;
            else
                mode = static_cast<SegmentMode> (value);
        }

        template <typename Visitor, typename ProfileT>
        void visitProfileHouse (Visitor& v, ProfileT& p)
        {
            auto& g = p.houseGate;
            v.field ("houseGateValid", g.valid);
            visitSegmentMode (v, g.mode);
            v.field ("houseReferenceLufs", g.referenceLufs);
            v.field ("houseLowShareReference", g.lowShareReference);
            v.field ("houseGateWidthLu", g.gateWidthLu);
            v.field ("houseLowGateFactor", g.lowGateFactor);
            v.field ("houseHeardSegments", g.heardSegments);
            v.field ("houseFullSegments", g.fullSegments);
            v.field ("houseHeardSeconds", g.heardSeconds);
            v.field ("houseFullSeconds", g.fullSeconds);
            v.field ("houseReopenCount", g.reopenCount);
        }

        // 格式 1 的旧画像没有 Clay 特征：读入后一律「不知道」（GlazeTypes.h：valid = false、数组全 0）。读取是在调用方给的
        // out 上覆盖字段，这几项格式 1 里读不到，必须显式清掉，不能沿用 out 里原来的值。
        void clearClayFeatures (MusicProfile& p) noexcept
        {
            p.subShareDb = Feature {};
            p.lowSideToMidDb = Feature {};
            p.lowCorrelation = Feature {};
            p.highSideToMidDb = Feature {};
            p.highCorrelation = Feature {};
            p.bandSideToMidDb.fill (0.0f);
            p.bandCorrelation.fill (0.0f);
            p.bandStereoConfidence.fill (0.0f);
            p.lowCutPeakCostDb = Feature {};
            p.monoPeakCostDb.fill (Feature {});
            p.widthPeakCostDb = Feature {};
        }

        template <typename Visitor, typename PlanT>
        void visitPlan (Visitor& v, PlanT& plan)
        {
            v.field ("requestedTargetLufs", plan.requestedTargetLufs);
            v.field ("effectiveTargetLufs", plan.effectiveTargetLufs);
            v.field ("requestedGainDb", plan.requestedGainDb);
            v.field ("effectiveGainDb", plan.effectiveGainDb);
            v.field ("estimatedExcessDb", plan.estimatedExcessDb);

            v.field ("balanceRecover", plan.increments.balanceRecover);
            v.field ("balanceTame", plan.increments.balanceTame);
            v.field ("colorToneDb", plan.increments.colorToneDb);
            v.field ("colorGlue", plan.increments.colorGlue);
            v.field ("colorHeat", plan.increments.colorHeat);
            v.field ("limitGainDb", plan.increments.limitGainDb);
            v.field ("limitClip", plan.increments.limitClip);

            // v0.1.7.0（算法 7）：Clay「自动」态的四个绝对值。旧版本存的方案没有这些属性 = 中性（ClayPlan{}：低切关、
            // 收单 0、加宽 0）；属性在但损坏照样拒收。
            const ClayPlan neutralClay {};
            v.optionalField ("clayLowCutHz", plan.clay.lowCutHz, neutralClay.lowCutHz);
            v.optionalField ("clayMonoHz", plan.clay.monoHz, neutralClay.monoHz);
            v.optionalField ("clayMonoAmount", plan.clay.monoAmount, neutralClay.monoAmount);
            v.optionalField ("clayWidthDb", plan.clay.widthDb, neutralClay.widthDb);
            // ADR-012：哪几项因代价被减量 / 否决（只给界面解释）。ADR-012 之前存的方案没有 = 0。
            v.optionalField ("clayCostLimitedMask", plan.clay.costLimitedMask, neutralClay.costLimitedMask);

            v.field ("weightWarm", plan.weights.warm);
            v.field ("weightPunchy", plan.weights.punchy);
            v.field ("weightClean", plan.weights.clean);

            v.field ("limitedByConstraints", plan.limitedByConstraints);
            v.field ("limitedByManual", plan.limitedByManual);
            // T-032 R6-02：电平类自动全部撤回。v0.1.8.5 及以前的存档没有这个属性：缺省取刚读到的 limitedByManual（旧语义 = 保守）——
            // 旧方案的「受手动限制」分不出是哪一种，按全撤读，看本标志的门不会比修前更早开工（算法 < 6 的存档进「照用保存的方案」
            // 模式，用的就是读回的方案；缺省 false 会让它们对一份「自动全撤」的旧方案照常开工）；兼容算法的存档下一块就重算，缺省值
            // 无影响。属性在但不是 0 / 1 照样拒收。
            v.optionalField ("automaticLevelWithdrawn", plan.automaticLevelWithdrawn, plan.limitedByManual);
            // T-032 R6-02 收尾（A11）：手动链单独已超预算的项（GlazeTypes.h manualBudget*，十进制位掩码）。A11 之前的存档没有这个属性：
            // 受手动设置限制的方案分不出是哪一项超了，三项全按「手动已超」读（保护层与上游监视不把可能是手动造成的超出当成自动的，
            // 只核对限幅器；v0.1.8.5 及以前的存档这时 automaticLevelWithdrawn 也按 limitedByManual 读，保护层本来就整体停着），不受限的读 0
            // （与修前逐位同）。属性在但不是 0 ～ 255 的整数照样拒收；未定义的位已提交版本照读（编排器只认三个已定义的位），执行快照拒收
            // （fromXml (ExecutionState) 另核）。
            v.optionalField ("manualBudgetMask", plan.manualBudgetMask, plan.limitedByManual ? manualBudgetAll : std::uint8_t { 0 });
            v.field ("algorithmVersion", plan.algorithmVersion);
            v.field ("constantsVersion", plan.constantsVersion);
        }

        // 校准参考条件的口径号（BUG-004，ADR-018）。旧口径（1）不写这个属性：旧存档读回来再存出去逐字不变，旧版本读到的也是
        // 它认识的全部属性。读：缺属性 = 旧口径（v0.1.8.0 及以前的存档）；在但不是 1 / 2 → 整份拒收。
        void visitConditionsFormat (xmlio::Writer& w, const std::uint8_t& format)
        {
            if (format != legacyConditionsFormat)
                w.field ("conditionsFormat", format);
        }

        void visitConditionsFormat (xmlio::Reader& r, std::uint8_t& format)
        {
            r.optionalField ("conditionsFormat", format, legacyConditionsFormat);

            if (format != legacyConditionsFormat && format != continuousConditionsFormat)
                r.ok = false;
        }

        // T-038：参考条件里的曲风与交付（ADR-026）。同 visitConditionsFormat：是默认值（通用 / 标准）时不写属性——旧存档读回来再存
        // 出去逐字不变，通用 + 标准的参考条件也与 main 逐字节相同（整份状态不是：标定快照多写曲风 / House / 交付三组常数，评审 F8；
        // 没有逻辑比较快照的字节，行为逐位相同）。读：缺属性 = 默认值；在但越界 → 整份拒收。
        void visitGenreDelivery (xmlio::Writer& w, const CalibrationReference& reference)
        {
            if (reference.genre != Genre::general)
                w.field ("genre", static_cast<std::uint8_t> (reference.genre));

            if (reference.delivery != Delivery::standard)
                w.field ("delivery", static_cast<std::uint8_t> (reference.delivery));
        }

        void visitGenreDelivery (xmlio::Reader& r, CalibrationReference& reference)
        {
            std::uint8_t genre = 0, delivery = 0;
            r.optionalField ("genre", genre, std::uint8_t { 0 });
            r.optionalField ("delivery", delivery, std::uint8_t { 0 });

            if (genre >= static_cast<std::uint8_t> (numGenres) || delivery >= static_cast<std::uint8_t> (numDeliveries))
            {
                r.ok = false;
                return;
            }

            reference.genre = static_cast<Genre> (genre);
            reference.delivery = static_cast<Delivery> (delivery);
        }

        template <typename Visitor, typename CalibrationT>
        void visitCalibration (Visitor& v, CalibrationT& c)
        {
            v.field ("gainBiasDb", c.gainBiasDb);
            v.field ("valid", c.valid);
            v.field ("partial", c.partial);
            v.field ("budgetLimited", c.budgetLimited);

            v.field ("profileId", c.reference.profileId);
            v.field ("style", c.reference.style);
            v.field ("glazePercent", c.reference.glazePercent);
            v.field ("manualParamsHash", c.reference.manualParamsHash);
            v.field ("ceilingDb", c.reference.ceilingDb);
            v.field ("lookaheadMs", c.reference.lookaheadMs);
            v.field ("sampleRate", c.reference.sampleRate);

            // BUG-004（ADR-018）：新口径的参考另存口径号与离散哈希、三个对数刻度值；四项同样严格（口径号为 2 时缺一个就整份拒收）。
            visitConditionsFormat (v, c.reference.conditionsFormat);

            if (c.reference.conditionsFormat == continuousConditionsFormat)
            {
                v.field ("discreteParamsHash", c.reference.discreteParamsHash);
                v.field ("limitReleaseMs", c.reference.limitReleaseMs);
                v.field ("clayLowCutHz", c.reference.clayLowCutHz);
                v.field ("clayMonoHz", c.reference.clayMonoHz);
            }

            visitGenreDelivery (v, c.reference);   // T-038（见上）
        }

        // 读一个整数属性：必须存在、是十进制整数、在 [minimum, maximum] 内。
        bool readVersion (const juce::XmlElement& xml, int minimum, int maximum, int& out)
        {
            int value = 0;
            xmlio::Reader reader (xml);
            reader.field ("formatVersion", value);

            if (! reader.ok || value < minimum || value > maximum)
                return false;

            out = value;
            return true;
        }

        //==============================================================================
        // 画像哈希：FNV-1a（32 位），每个字段先量化成 32 位整数再按小端字节喂进去，与平台字节序无关。
        // 量化步长：响度 / 电平 / 斜率 / 证据 0.01（dB、LU、dB/oct），占比与置信度 1e-4，时长 0.01 s，起音密度 0.001 次/秒。
        // 非有限值各有固定编码（NaN、+Inf、−Inf 互不相同，也不与任何有限值相撞）。
        class Fnv1a
        {
        public:
            void addWord (std::uint32_t word) noexcept
            {
                for (int i = 0; i < 4; ++i)
                {
                    hash ^= (word >> (8 * i)) & 0xffu;
                    hash *= 16777619u;
                }
            }

            void addQuantized (double value, double step) noexcept
            {
                constexpr std::int64_t nanCode = std::numeric_limits<std::int32_t>::min();
                constexpr std::int64_t negativeInfinityCode = nanCode + 1;
                constexpr std::int64_t positiveInfinityCode = std::numeric_limits<std::int32_t>::max();

                std::int64_t code;

                if (std::isnan (value))
                    code = nanCode;
                else if (std::isinf (value))
                    code = value > 0.0 ? positiveInfinityCode : negativeInfinityCode;
                else
                {
                    // 先钳位再取整：超出 32 位范围的值落在两端（这种值本身就是坏数据），且不与三个特殊编码相撞。
                    const auto scaled = juce::jlimit ((double) negativeInfinityCode + 1.0, (double) positiveInfinityCode - 1.0, value / step);
                    code = (std::int64_t) std::llround (scaled);
                }

                addWord ((std::uint32_t) (std::int32_t) code);
            }

            void addFlag (bool flag) noexcept         { addWord (flag ? 1u : 0u); }
            void addInteger (int value) noexcept      { addWord ((std::uint32_t) value); }

            std::uint32_t get() const noexcept        { return hash; }

        private:
            std::uint32_t hash = 2166136261u;
        };

        constexpr double levelStep = 0.01;
        constexpr double fractionStep = 1.0e-4;
        constexpr double secondsStep = 0.01;
        constexpr double densityStep = 0.001;

        void addFeature (Fnv1a& h, const Feature& f, double valueStep) noexcept
        {
            h.addQuantized ((double) f.value, valueStep);
            h.addFlag (f.valid);
            h.addQuantized (f.effectiveSeconds, secondsStep);
            h.addQuantized ((double) f.confidence, fractionStep);
        }

        template <size_t N>
        void addArray (Fnv1a& h, const std::array<float, N>& values, double step) noexcept
        {
            for (auto value : values)
                h.addQuantized ((double) value, step);
        }
    }

    //==============================================================================
    std::uint32_t computeProfileId (const MusicProfile& p) noexcept
    {
        Fnv1a h;
        h.addInteger (p.formatVersion);

        addFeature (h, p.integratedLufs, levelStep);
        addFeature (h, p.loudnessRangeLu, levelStep);
        addFeature (h, p.plrDb, levelStep);
        addFeature (h, p.truePeakDbtp, levelStep);
        addFeature (h, p.spectralSlopeDbPerOct, levelStep);
        addFeature (h, p.lowShare, fractionStep);
        addFeature (h, p.highShare, fractionStep);
        addFeature (h, p.lowBandRangeDb, levelStep);
        addFeature (h, p.onsetDensityPerSecond, densityStep);
        addFeature (h, p.onsetStrengthDb, levelStep);

        addArray (h, p.bandLongTermDb, levelStep);
        addArray (h, p.tameEvidenceDb, levelStep);
        addArray (h, p.recoverEvidenceDb, levelStep);
        addArray (h, p.bandConfidence, fractionStep);

        h.addQuantized (p.effectiveSeconds, secondsStep);
        h.addQuantized ((double) p.representativeness, fractionStep);

        // v0.1.7.0 修坯 Clay 的特征：只在格式 ≥ 2 时加，而且接在最后——格式 1 的画像号与 v0.1.6.2 逐位相同（旧工程校准
        // 参考条件里的画像号仍然对得上，校准照旧适用、声音不变）。量化步长沿用上面的口径：S/M 与超低频占比是电平（0.01 dB），
        // 相关度（−1 ～ 1）与置信度按占比（1e-4）。
        if (p.formatVersion >= clayProfileFormatVersion)
        {
            addFeature (h, p.subShareDb, levelStep);
            addFeature (h, p.lowSideToMidDb, levelStep);
            addFeature (h, p.lowCorrelation, fractionStep);
            addFeature (h, p.highSideToMidDb, levelStep);
            addFeature (h, p.highCorrelation, fractionStep);

            addArray (h, p.bandSideToMidDb, levelStep);
            addArray (h, p.bandCorrelation, fractionStep);
            addArray (h, p.bandStereoConfidence, fractionStep);

            // ADR-012 的三类峰值代价（dB，电平口径 0.01 dB）：接在格式 2 字段的最后。代价决定 Clay 做不做、做多少，属于画像内容，
            // 变了校准就该失效（与其他 Clay 特征同理）；格式 1 仍然一项都不加。
            addFeature (h, p.lowCutPeakCostDb, levelStep);

            for (const auto& cost : p.monoPeakCostDb)
                addFeature (h, cost, levelStep);

            addFeature (h, p.widthPeakCostDb, levelStep);
        }

        // T-038 House 档门控结果：只在格式 ≥ 3 时加，接在最后——格式 1 / 2 的画像号与 main 逐位相同。门控结果决定画像取了哪些段，
        // 属于画像内容，变了校准就该失效。响度按电平口径，占比与系数按占比口径，时长按秒口径，计数按整数。
        if (p.formatVersion >= houseProfileFormatVersion)
        {
            const auto& g = p.houseGate;
            h.addInteger (g.valid ? 1 : 0);
            h.addInteger (static_cast<int> (g.mode));
            h.addQuantized ((double) g.referenceLufs, levelStep);
            h.addQuantized ((double) g.lowShareReference, fractionStep);
            h.addQuantized ((double) g.gateWidthLu, levelStep);
            h.addQuantized ((double) g.lowGateFactor, fractionStep);
            h.addInteger (g.heardSegments);
            h.addInteger (g.fullSegments);
            h.addQuantized (g.heardSeconds, secondsStep);
            h.addQuantized (g.fullSeconds, secondsStep);
            h.addInteger (g.reopenCount);
        }

        return h.get();
    }

    //==============================================================================
    std::unique_ptr<juce::XmlElement> toXml (const MusicProfile& profile)
    {
        auto xml = std::make_unique<juce::XmlElement> (profileTag);
        xml->setAttribute ("formatVersion", profile.formatVersion);

        xmlio::Writer writer (*xml);
        visitProfile (writer, profile);

        // 按画像自己的格式写：格式 1 的旧画像重存仍是格式 1（没有 Clay 特征），画像号也就不变。
        if (profile.formatVersion >= clayProfileFormatVersion)
            visitProfileClay (writer, profile);

        if (profile.formatVersion >= houseProfileFormatVersion)
            visitProfileHouse (writer, profile);

        return xml;
    }

    bool fromXml (const juce::XmlElement& xml, MusicProfile& out)
    {
        int format = 0;

        if (! xml.hasTagName (profileTag) || ! readVersion (xml, 1, profileFormatVersion, format))
            return false;

        MusicProfile parsed = out;   // 全部读成功才整体交出去：失败时 out 不变
        parsed.formatVersion = format;

        xmlio::Reader reader (xml);
        visitProfile (reader, parsed);

        if (format >= clayProfileFormatVersion)
            visitProfileClay (reader, parsed);   // 格式 2 起 Clay 特征与其他字段同样必须齐全
        else
            clearClayFeatures (parsed);          // 格式 1：没有这些特征 =「不知道」

        if (format >= houseProfileFormatVersion)
            visitProfileHouse (reader, parsed);  // 格式 3：House 门控结果同样必须齐全
        else
            parsed.houseGate = HouseGateSummary {};   // 格式 1 / 2：没有门控结果（House 档下视为锁定条件未满足）

        if (! reader.ok)
            return false;

        out = parsed;
        return true;
    }

    std::unique_ptr<juce::XmlElement> toXml (const CommittedState& state)
    {
        auto xml = std::make_unique<juce::XmlElement> (stateTag);
        xml->setAttribute ("formatVersion", stateFormatVersion);

        {
            xmlio::Writer writer (*xml);
            writer.field ("hasProfile", state.hasProfile);
            writer.field ("profileId", state.profileId);
            writer.field ("lockedWarm", state.lockedWeights.warm);
            writer.field ("lockedPunchy", state.lockedWeights.punchy);
            writer.field ("lockedClean", state.lockedWeights.clean);
        }

        xml->addChildElement (toXml (state.profile).release());

        auto* plan = xml->createNewChildElement ("PLAN");
        xmlio::Writer planWriter (*plan);
        visitPlan (planWriter, state.plan);

        auto* calibration = xml->createNewChildElement ("CALIBRATION");
        xmlio::Writer calibrationWriter (*calibration);
        visitCalibration (calibrationWriter, state.calibration);

        xml->addChildElement (toXml (state.constants).release());
        return xml;
    }

    bool fromXml (const juce::XmlElement& xml, CommittedState& out)
    {
        int format = 0;

        if (! xml.hasTagName (stateTag) || ! readVersion (xml, 1, stateFormatVersion, format))
            return false;

        CommittedState parsed = out;   // 全部读成功才整体交出去：失败时 out 不变

        xmlio::Reader reader (xml);
        reader.field ("hasProfile", parsed.hasProfile);
        reader.field ("profileId", parsed.profileId);
        reader.field ("lockedWarm", parsed.lockedWeights.warm);
        reader.field ("lockedPunchy", parsed.lockedWeights.punchy);
        reader.field ("lockedClean", parsed.lockedWeights.clean);

        if (! reader.ok)
            return false;

        // 四个子节点都必须在、且各只认第一个同名节点。
        const auto* profile = xml.getChildByName (profileTag);
        const auto* plan = xml.getChildByName ("PLAN");
        const auto* calibration = xml.getChildByName ("CALIBRATION");
        const auto* constants = xml.getChildByName (constantsTag);

        if (profile == nullptr || plan == nullptr || calibration == nullptr || constants == nullptr)
            return false;

        if (! fromXml (*profile, parsed.profile) || ! fromXml (*constants, parsed.constants))
            return false;

        xmlio::Reader planReader (*plan);
        visitPlan (planReader, parsed.plan);

        xmlio::Reader calibrationReader (*calibration);
        visitCalibration (calibrationReader, parsed.calibration);

        if (! planReader.ok || ! calibrationReader.ok)
            return false;

        out = parsed;
        return true;
    }

    std::unique_ptr<juce::XmlElement> toXml (const ExecutionState& state)
    {
        auto xml = std::make_unique<juce::XmlElement> (executionTag);
        xml->setAttribute ("formatVersion", 1);
        xmlio::Writer writer (*xml);
        writer.field ("valid", state.valid);
        writer.field ("hasProfile", state.hasProfile);
        writer.field ("provisional", state.provisional);
        writer.field ("requiresCalibration", state.requiresCalibration);
        writer.field ("gainBiasDb", state.gainBiasDb);
        writer.field ("protectionDb", state.protectionDb);
        writer.field ("liveToneOffsetDb", state.liveToneOffsetDb);
        writer.field ("liveTame", state.liveTame);
        writer.field ("liveHeatReduction", state.liveHeatReduction);
        writer.field ("liveGlueReduction", state.liveGlueReduction);
        // v0.1.7.0 修坯 Clay：最近一块实际执行的「自动」态值（过渡混合 + 保护之后、三态合成之前）与 ClayGuard 状态。
        // 读取时缺属性 = 中性（旧快照），见 fromXml。
        writer.field ("liveClayLowCutHz", state.liveClay.lowCutHz);
        writer.field ("liveClayMonoHz", state.liveClay.monoHz);
        writer.field ("liveClayMonoAmount", state.liveClay.monoAmount);
        writer.field ("liveClayWidthDb", state.liveClay.widthDb);
        writer.field ("liveClayCostLimitedMask", state.liveClay.costLimitedMask);   // ADR-012：只给界面解释
        writer.field ("clayGuardPhaseFloor", state.clayGuard.phaseFloor);
        writer.field ("clayGuardPhaseHoldSeconds", state.clayGuard.phaseHoldSeconds);
        writer.field ("clayGuardWidthCapDb", state.clayGuard.widthCapDb);
        writer.field ("clayGuardPhaseActive", state.clayGuard.phaseActive);
        writer.field ("clayGuardWidthActive", state.clayGuard.widthActive);
        // 复用既有严格序列化格式；这里只借用 reference 容器，不宣称有有效校准。
        Calibration reference;
        reference.reference = state.reference;
        auto* conditions = xml->createNewChildElement ("REFERENCE");
        xmlio::Writer referenceWriter (*conditions);
        visitCalibration (referenceWriter, reference);
        xml->addChildElement (toXml (state.profile).release());
        xml->addChildElement (toXml (state.constants).release());
        auto* plan = xml->createNewChildElement ("PLAN");
        xmlio::Writer planWriter (*plan);
        visitPlan (planWriter, state.plan);
        return xml;
    }

    bool fromXml (const juce::XmlElement& xml, ExecutionState& out)
    {
        int version = 0;
        if (! xml.hasTagName (executionTag) || ! readVersion (xml, 1, 1, version))
            return false;
        ExecutionState parsed;
        xmlio::Reader reader (xml);
        reader.field ("valid", parsed.valid);
        reader.field ("hasProfile", parsed.hasProfile);
        reader.field ("provisional", parsed.provisional);
        if (xml.hasAttribute ("requiresCalibration"))
            reader.field ("requiresCalibration", parsed.requiresCalibration);
        reader.field ("gainBiasDb", parsed.gainBiasDb);
        reader.field ("protectionDb", parsed.protectionDb);
        reader.field ("liveToneOffsetDb", parsed.liveToneOffsetDb);
        reader.field ("liveTame", parsed.liveTame);
        if (xml.hasAttribute ("liveHeatReduction"))
            reader.field ("liveHeatReduction", parsed.liveHeatReduction);
        if (xml.hasAttribute ("liveGlueReduction"))
            reader.field ("liveGlueReduction", parsed.liveGlueReduction);
        // v0.1.7.0 修坯 Clay：旧快照（v0.1.6.2 之前存的）没有这些属性 = 中性；属性在但损坏照样拒收。
        const ClayPlan neutralClay {};
        const ClayGuardState neutralGuard {};
        reader.optionalField ("liveClayLowCutHz", parsed.liveClay.lowCutHz, neutralClay.lowCutHz);
        reader.optionalField ("liveClayMonoHz", parsed.liveClay.monoHz, neutralClay.monoHz);
        reader.optionalField ("liveClayMonoAmount", parsed.liveClay.monoAmount, neutralClay.monoAmount);
        reader.optionalField ("liveClayWidthDb", parsed.liveClay.widthDb, neutralClay.widthDb);
        reader.optionalField ("liveClayCostLimitedMask", parsed.liveClay.costLimitedMask, neutralClay.costLimitedMask);
        reader.optionalField ("clayGuardPhaseFloor", parsed.clayGuard.phaseFloor, neutralGuard.phaseFloor);
        reader.optionalField ("clayGuardPhaseHoldSeconds", parsed.clayGuard.phaseHoldSeconds, neutralGuard.phaseHoldSeconds);
        reader.optionalField ("clayGuardWidthCapDb", parsed.clayGuard.widthCapDb, neutralGuard.widthCapDb);
        if (xml.hasAttribute ("clayGuardPhaseActive"))
            reader.field ("clayGuardPhaseActive", parsed.clayGuard.phaseActive);
        if (xml.hasAttribute ("clayGuardWidthActive"))
            reader.field ("clayGuardWidthActive", parsed.clayGuard.widthActive);
        const auto* conditions = xml.getChildByName ("REFERENCE");
        const auto* profile = xml.getChildByName (profileTag);
        const auto* constants = xml.getChildByName (constantsTag);
        const auto* plan = xml.getChildByName ("PLAN");
        if (! reader.ok || conditions == nullptr || profile == nullptr || constants == nullptr || plan == nullptr
            || ! fromXml (*profile, parsed.profile) || ! fromXml (*constants, parsed.constants))
            return false;
        Calibration reference;
        xmlio::Reader referenceReader (*conditions);
        visitCalibration (referenceReader, reference);
        parsed.reference = reference.reference;
        xmlio::Reader planReader (*plan);
        visitPlan (planReader, parsed.plan);
        const auto& inc = parsed.plan.increments;
        const auto& weights = parsed.plan.weights;
        const auto& planClay = parsed.plan.clay;
        const auto& heardClay = parsed.liveClay;
        const auto& guardState = parsed.clayGuard;
        for (const auto value : { parsed.gainBiasDb, parsed.protectionDb, parsed.liveToneOffsetDb, parsed.liveTame,
                                 parsed.liveHeatReduction, parsed.liveGlueReduction,
                                 inc.balanceRecover, inc.balanceTame, inc.colorToneDb, inc.colorGlue, inc.colorHeat,
                                 inc.limitGainDb, inc.limitClip, parsed.reference.glazePercent,
                                 parsed.reference.ceilingDb, parsed.reference.lookaheadMs,
                                 parsed.reference.limitReleaseMs, parsed.reference.clayLowCutHz, parsed.reference.clayMonoHz,
                                 parsed.plan.requestedTargetLufs, parsed.plan.effectiveTargetLufs,
                                 parsed.plan.requestedGainDb, parsed.plan.effectiveGainDb, parsed.plan.estimatedExcessDb,
                                 weights.warm, weights.punchy, weights.clean,
                                 planClay.lowCutHz, planClay.monoHz, planClay.monoAmount, planClay.widthDb,
                                 heardClay.lowCutHz, heardClay.monoHz, heardClay.monoAmount, heardClay.widthDb,
                                 guardState.phaseFloor, guardState.phaseHoldSeconds, guardState.widthCapDb })
            if (! std::isfinite (value))
                return false;
        // Clay 值必须是 ClayModule 能直接接受的实际值（编排器写快照前已按同一范围收拾过，正常快照一定满足）：低切 0（关）
        // 或 10 ～ 60 Hz、分频 40 ～ 300 Hz、收单比例 0 ～ 1、加宽 −3 ～ +3 dB；代价位（ADR-012）只许 GlazeTypes.h 定义的三位。
        // 保护状态只核对符号与比例范围（上限由 ClayGuard::setState 自己再钳）。
        const auto validClay = [] (const ClayPlan& c)
        {
            constexpr int definedCostBits = clayCostLimitedLowCut | clayCostLimitedMono | clayCostLimitedWidth;

            return (c.lowCutHz == 0.0f || (c.lowCutHz >= ClayModule::minLowCutHz && c.lowCutHz <= ClayModule::maxLowCutHz))
                && c.monoHz >= ClayModule::minMonoHz && c.monoHz <= ClayModule::maxMonoHz
                && c.monoAmount >= 0.0f && c.monoAmount <= 1.0f
                && c.widthDb >= ClayModule::minWidthDb && c.widthDb <= ClayModule::maxWidthDb
                && (c.costLimitedMask & ~definedCostBits) == 0;
        };
        if (! validClay (planClay) || ! validClay (heardClay)
            || guardState.phaseFloor < 0.0f || guardState.phaseFloor > 1.0f
            || guardState.phaseHoldSeconds < 0.0f || guardState.widthCapDb < 0.0f)
            return false;
        const auto percent = [] (float value) { return value >= 0.0f && value <= 100.0f; };
        if (! referenceReader.ok || ! planReader.ok || parsed.protectionDb > 0.0f || parsed.protectionDb < -24.0f
            || std::abs (parsed.gainBiasDb) > 24.0f || std::abs (parsed.liveToneOffsetDb) > 6.0f
            || ! percent (parsed.liveTame) || ! percent (parsed.reference.glazePercent)
            || parsed.liveHeatReduction < 0.0f || parsed.liveHeatReduction > 1.0f
            || parsed.liveGlueReduction < 0.0f || parsed.liveGlueReduction > 1.0f
            || ! percent (inc.balanceRecover) || ! percent (inc.balanceTame) || ! percent (inc.colorGlue)
            || ! percent (inc.colorHeat) || ! percent (inc.limitClip) || std::abs (inc.colorToneDb) > 3.0f
            || inc.limitGainDb < 0.0f || inc.limitGainDb > 24.0f
            || weights.warm < 0.0f || weights.warm > 1.0f || weights.punchy < 0.0f || weights.punchy > 1.0f
            || weights.clean < 0.0f || weights.clean > 1.0f || ! (weights.warm + weights.punchy + weights.clean > 0.0f)
            || ! (parsed.reference.sampleRate > 0.0)
            || ! std::isfinite (parsed.reference.sampleRate))
            return false;
        // T-032 A11：手动链单独超预算的项只许 GlazeTypes.h 定义的三位（与上面 Clay 代价位同一做法；已提交版本照读、编排器只认这三位）。
        if ((parsed.plan.manualBudgetMask & ~manualBudgetAll) != 0)
            return false;
        out = parsed;
        return true;
    }

    bool bitwiseEqual (const MusicProfile& a, const MusicProfile& b)
    {
        return toXml (a)->isEquivalentTo (toXml (b).get(), false);
    }

    bool bitwiseEqual (const CommittedState& a, const CommittedState& b)
    {
        return toXml (a)->isEquivalentTo (toXml (b).get(), false);
    }
}
