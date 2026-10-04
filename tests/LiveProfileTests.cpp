/*
  ==============================================================================

    LiveProfileTests.cpp
    T-048 阶段 1（修订：回看重判）：实时画像 LiveProfile（LiveProfile.h 文件头；LiveProfile.cpp 文件头；设计规格-实时GLAZE-主段与增益
    第 1～3、5 节；BRIEF 第 3 节；阶段 1 修订任务书）。

    a. 等价性锚点：分析器 House 档处理一段前奏 + drop，段落账存下的段（试算拷进 liveTrial* / liveBlend*）交给遗忘关掉、参考冻结为
       账本最终的 S_ref / lowRef、斜坡宽极小（硬门）的 LiveProfile，画像与分析器的 House 候选画像逐项比；不冻结时算出的参考也等于账本的。
    b. breakdown（m′ = 0）里画像、M、c 逐位不动；新 drop 来了画像向它移动。
    c. m 的公式（两个因子、斜坡端点、冷启动、被动型 / 响度型）；进账判断 = 上一份画像的 mainnessFor，这一份的判断 = 发布后的 mainnessFor。
    r. 回看重判（REPORT-C 第 5 节第 1～3 条）：低音重的前奏不再定住 lowRef；冷启动落在淡入小节上不卡住；前奏被重判出主段、drop 来了 I 很快跟上。
    d. W_mem / M0 换算、长时间无主段、换歌软遗忘（含「轻的那一支要电平稳」：起伏 1.9 vs 2.1 LU、淡出不触发）。
    e. 状态 XML 往返逐位（窗口、还没发布的段、事件过的时钟）；坏数据冷启动并返回 false——含有限但越出物理上界的量（P1F）；400 份随机损坏的
       状态：读回的那些之后的读数全是有限值。
    f. 同一段序列结果逐位相同（另一实例、reset、调用次数与时刻：advanceTo 每 7 样本 / 重复 / 一次跳到底 / 队列满，含窗口已满 512 段时
       队列满）；整链块长 4096 / 512 / 480 / 32 / 7 逐位；窗口外的老段丢掉之后画像只取决于窗口；音频线程接口不分配。
    g. 耗时：每段入队、每片（均值 / 最大），整遍同步重算；状态大小。
    另：提交记录环形缓冲、坏段、常数取值表、发布端兜底（实时入口交来荒谬的段时画像里不出 ∞ / NaN）。

    合成段（makeSegment）各字段按 SegmentLedger::deriveSegment 与分析器的记账口径自洽地造（一小节 124 BPM、48 kHz）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/BalanceModule.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/dsp/LiveProfile.h"
#include "../Source/dsp/LoudnessMeter.h"
#include "../Source/dsp/MusicAnalyzer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::Feature;
        using glaze::LiveCommitRecord;
        using glaze::LiveConstants;
        using glaze::LiveDiagnostics;
        using glaze::LiveProfile;
        using glaze::LiveSegmentJudgement;
        using glaze::MainSectionModel;
        using glaze::MusicProfile;
        using glaze::SegmentLedger;
        using Segment = glaze::SegmentLedger::Segment;

        constexpr double rate = 48000.0;
        constexpr double tempoBpm = 124.0;
        constexpr std::int64_t barSamples = 92903;   // 124 BPM 4/4 一小节（48 kHz，取整）
        constexpr int subBlockSamples = 4800;        // IN 表 100 ms 子块
        constexpr int hopSamples = 480;              // 合成段的 Balance 帧（实时画像由段还原 hop，取多少都行）
        constexpr int numBands = glaze::numBands;
        const auto infinity = std::numeric_limits<double>::infinity();

        //==============================================================================
        // 逐位比较
        bool sameFloat (float a, float b)     { return std::memcmp (&a, &b, sizeof (float)) == 0; }
        bool sameDouble (double a, double b)  { return std::memcmp (&a, &b, sizeof (double)) == 0; }

        bool sameFeature (const Feature& a, const Feature& b)
        {
            return sameFloat (a.value, b.value) && a.valid == b.valid && sameDouble (a.effectiveSeconds, b.effectiveSeconds)
                && sameFloat (a.confidence, b.confidence);
        }

        template <typename Array>
        bool sameFloats (const Array& a, const Array& b)
        {
            for (size_t i = 0; i < a.size(); ++i)
                if (! sameFloat (a[i], b[i]))
                    return false;

            return true;
        }

        // 画像逐位相同（逐字段）；withRange = false 时不比 LRA。
        bool sameProfile (const MusicProfile& a, const MusicProfile& b, bool withRange = true)
        {
            const Feature MusicProfile::* const features[] = {
                &MusicProfile::integratedLufs, &MusicProfile::plrDb, &MusicProfile::truePeakDbtp, &MusicProfile::spectralSlopeDbPerOct,
                &MusicProfile::lowShare, &MusicProfile::highShare, &MusicProfile::lowBandRangeDb, &MusicProfile::onsetDensityPerSecond,
                &MusicProfile::onsetStrengthDb, &MusicProfile::subShareDb, &MusicProfile::lowSideToMidDb, &MusicProfile::lowCorrelation,
                &MusicProfile::highSideToMidDb, &MusicProfile::highCorrelation, &MusicProfile::lowCutPeakCostDb, &MusicProfile::widthPeakCostDb };

            for (const auto member : features)
                if (! sameFeature (a.*member, b.*member))
                    return false;

            for (size_t i = 0; i < a.monoPeakCostDb.size(); ++i)
                if (! sameFeature (a.monoPeakCostDb[i], b.monoPeakCostDb[i]))
                    return false;

            for (size_t i = 0; i < a.lowCutBlendLossDb.size(); ++i)
                if (! sameFeature (a.lowCutBlendLossDb[i], b.lowCutBlendLossDb[i]))
                    return false;

            return (! withRange || sameFeature (a.loudnessRangeLu, b.loudnessRangeLu))
                && sameFloats (a.bandLongTermDb, b.bandLongTermDb) && sameFloats (a.tameEvidenceDb, b.tameEvidenceDb)
                && sameFloats (a.recoverEvidenceDb, b.recoverEvidenceDb) && sameFloats (a.bandConfidence, b.bandConfidence)
                && sameFloats (a.bandSideToMidDb, b.bandSideToMidDb) && sameFloats (a.bandCorrelation, b.bandCorrelation)
                && sameFloats (a.bandStereoConfidence, b.bandStereoConfidence)
                && sameDouble (a.effectiveSeconds, b.effectiveSeconds) && sameFloat (a.representativeness, b.representativeness)
                && a.houseGate.valid == b.houseGate.valid && a.formatVersion == b.formatVersion;
        }

        // 参考、M、c 逐位相同（breakdown 不变性比这几项：窗口里多了段，窗口的统计照常变）。
        bool sameMain (const LiveDiagnostics& a, const LiveDiagnostics& b)
        {
            return a.referenceValid == b.referenceValid && sameFloat (a.referenceLufs, b.referenceLufs)
                && a.lowReferenceValid == b.lowReferenceValid && sameFloat (a.lowReference, b.lowReference)
                && sameDouble (a.mainMassSeconds, b.mainMassSeconds) && sameFloat (a.confidence, b.confidence);
        }

        // 诊断逐位相同。最后几项工作量统计（pendingSegments、lastPassSlices、catchUps、forcedPublications）随交段与 advanceTo 的先后变，不比。
        bool sameDiagnostics (const LiveDiagnostics& a, const LiveDiagnostics& b, bool withCounters = true)
        {
            const auto derived = sameMain (a, b) && a.windowSegments == b.windowSegments && sameFloat (a.oldestMainShare, b.oldestMainShare)
                              && sameFloat (a.oldestReferenceShare, b.oldestReferenceShare);

            return derived && (! withCounters
                               || (a.commits == b.commits && sameDouble (a.activeSeconds, b.activeSeconds)
                                   && sameDouble (a.secondsSinceMain, b.secondsSinceMain) && a.timeDecayActive == b.timeDecayActive
                                   && a.newProgramCount == b.newProgramCount && a.droppedSegments == b.droppedSegments
                                   && a.publishTime == b.publishTime));
        }

        bool sameRecord (const LiveCommitRecord& a, const LiveCommitRecord& b, bool withSequence = true)
        {
            return (! withSequence || a.sequence == b.sequence) && a.start == b.start && a.end == b.end && a.publishTime == b.publishTime
                && sameDouble (a.nominalSeconds, b.nominalSeconds) && a.mode == b.mode && sameDouble (a.effectiveSeconds, b.effectiveSeconds)
                && a.loudnessValid == b.loudnessValid && sameFloat (a.loudnessLufs, b.loudnessLufs) && a.lowShareValid == b.lowShareValid
                && sameFloat (a.lowShare, b.lowShare) && sameFloat (a.admissionLoudnessFactor, b.admissionLoudnessFactor)
                && sameFloat (a.admissionLowFactor, b.admissionLowFactor) && sameFloat (a.admissionMainness, b.admissionMainness)
                && sameFloat (a.loudnessFactor, b.loudnessFactor) && sameFloat (a.lowFactor, b.lowFactor) && sameFloat (a.mainness, b.mainness)
                && sameDouble (a.weightSeconds, b.weightSeconds) && a.referenceValid == b.referenceValid
                && sameFloat (a.referenceLufs, b.referenceLufs) && a.lowReferenceValid == b.lowReferenceValid
                && sameFloat (a.lowReference, b.lowReference) && a.mainLevelValid == b.mainLevelValid
                && sameFloat (a.mainLevelLufs, b.mainLevelLufs) && sameDouble (a.mainMassSeconds, b.mainMassSeconds)
                && sameFloat (a.confidence, b.confidence) && a.timeDecay == b.timeDecay && a.newProgram == b.newProgram
                && a.newProgramVote == b.newProgramVote && a.newProgramRangeValid == b.newProgramRangeValid
                && sameFloat (a.newProgramRangeLu, b.newProgramRangeLu);
        }

        //==============================================================================
        // 口径同 MusicAnalyzer.cpp 的几个小函数（测试里的参照计算用）。
        double lufsOf (double meanSquare)   { return -0.691 + 10.0 * std::log10 (meanSquare); }

        int firstBinAtOrAbove (double gate)
        {
            const auto position = (gate + 70.0) * 10.0 - 0.5;
            return position <= 0.0 ? 0 : position >= 800.0 ? 800 : (int) std::ceil (position);
        }

        double binCentre (int bin)   { return -70.0 + ((double) bin + 0.5) / 10.0; }

        float fractionOf (double numerator, double fullScale)
        {
            return fullScale > 0.0 ? (float) juce::jlimit (0.0, 1.0, numerator / fullScale) : 0.0f;
        }

        //==============================================================================
        // 合成段：一小节的读数，各字段按 SegmentLedger::deriveSegment 与分析器的记账口径自洽地造。
        struct Spec
        {
            double loudnessLufs = -8.03;   // 段响度（有效子块均方的 LUFS）；取在格中间，免得落在 0.1 LU 格边上
            double lowShare = 0.6;         // 低频（段 0 ～ 4）功率份额
            double slopeDbPerOct = -3.0;   // 27 段电平的斜率
            float truePeak = 0.7f;         // 段真峰值（线性）
            double trialPeak = 0.6;        // 原始 100 ms 块峰值的水平（线性）
            int events = 8;                // 起音事件
            int effectiveSubBlocks = 19;   // 有效子块（19 = 1.9 s，一小节 1.94 s）
            std::uint32_t seed = 1;        // 逐块的小起伏
        };

        Spec dropSpec()        { return {}; }
        Spec introSpec()       { Spec s; s.loudnessLufs = -14.03; s.lowShare = 0.5; s.slopeDbPerOct = -2.0; s.truePeak = 0.45f; s.trialPeak = 0.35; s.seed = 2; return s; }
        Spec breakdownSpec()   { Spec s; s.loudnessLufs = -20.03; s.lowShare = 0.1; s.slopeDbPerOct = -4.5; s.truePeak = 0.2f; s.trialPeak = 0.15; s.events = 2; s.seed = 3; return s; }

        Segment makeSegment (const Spec& spec, std::int64_t index)
        {
            Segment s;
            juce::Random random ((juce::int64) spec.seed * 7919 + index);
            const auto noise = [&random] { return (double) random.nextFloat() - 0.5; };

            s.mode = glaze::SegmentMode::bars;
            s.positioned = true;
            s.key = index * 64;
            s.start = index * barSamples;
            s.end = s.start + barSamples;
            s.nominalLength = barSamples;

            // 子块流
            const auto meanSquare = std::pow (10.0, (spec.loudnessLufs + 0.691) / 10.0);
            const auto n = spec.effectiveSubBlocks;
            s.subBlocks = juce::jmax (n, 19);
            s.effectiveSubBlocks = n;
            s.effectiveEnergy = meanSquare * (double) n;
            s.truePeak = spec.truePeak;
            s.gatingCount = n;
            s.rangeCount = n;

            for (int k = 0; k < n; ++k)
            {
                s.gatingPower[(size_t) k] = (float) (meanSquare * (1.0 + 0.2 * noise()));
                const auto shortTerm = spec.loudnessLufs + 0.6 * noise();
                s.rangeBin[(size_t) k] = (std::uint16_t) juce::jlimit (0, 799, (int) std::floor ((shortTerm + 70.0) * 10.0));
                s.rangeEnergy += std::pow (10.0, (shortTerm + 0.691) / 10.0);
            }

            // 帧流：每帧 hopSamples；frameSeconds 逐帧累加（同分析器 noteHouseMusicFrame）。
            const auto frames = n * 10;
            s.frames = frames;

            for (int f = 0; f < frames; ++f)
                s.frameSeconds += (double) hopSamples / rate;

            std::array<double, (size_t) numBands> shape {};
            double low = 0.0, rest = 0.0;

            for (int b = 0; b < numBands; ++b)
            {
                shape[(size_t) b] = std::pow (10.0, spec.slopeDbPerOct * ((double) b / 3.0) / 10.0) * (1.0 + 0.05 * noise());
                (b <= 4 ? low : rest) += shape[(size_t) b];
            }

            const auto lowScale = spec.lowShare / (1.0 - spec.lowShare) * rest / low;
            double shapeTotal = 0.0;

            for (int b = 0; b <= 4; ++b)
                shape[(size_t) b] *= lowScale;

            for (auto x : shape)
                shapeTotal += x;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto perFrame = meanSquare * shape[b] / shapeTotal;
                s.powerSum[b] = perFrame * (double) frames;
                s.validWeight[b] = (double) frames;
                s.stereoPowerSum[b] = s.powerSum[b];
                s.stereoCrossSum[b] = s.powerSum[b] * (b <= 5 ? 0.95 : 0.7);
                s.stereoWeight[b] = (double) frames;
            }

            s.tameSum[10] = 1.5 * (double) frames;      // 段 10 的 Tame 证据 1.5 dB
            s.recoverSum[15] = 2.0 * (double) frames;   // 段 15 的 Recover 证据，缺失帧占九成
            s.deficitWeight[15] = 0.9 * (double) frames;
            s.lowStereoWeight = s.highStereoWeight = (double) frames;

            const auto lowDb = 10.0 * std::log10 (meanSquare * spec.lowShare);
            const auto centre = juce::jlimit (8, 271, (int) std::floor ((lowDb + 120.0) / 0.5));
            s.lowRangeFirst = 280;
            s.lowRangeLast = -1;

            for (int f = 0; f < frames; ++f)
            {
                const auto bin = centre + (f * 7) % 17 - 8;
                ++s.lowRangeCount[(size_t) bin];
                s.lowRangeFirst = juce::jmin (s.lowRangeFirst, bin);
                s.lowRangeLast = juce::jmax (s.lowRangeLast, bin);
            }

            s.fluxFrames = frames + 1;
            s.musicFluxFrames = frames;
            s.fluxEvents = juce::jmax (0, spec.events - 1);
            s.fluxSum = 2.0 * (double) frames;

            // 样本流
            s.onsetEffectiveSamples = (std::int64_t) n * subBlockSamples;
            s.onsetEvents = spec.events;
            s.strengthCount = spec.events;

            for (int k = 0; k < spec.events; ++k)
                s.strengthBin[(size_t) k] = (std::uint8_t) (30 + (k * 3) % 11);

            s.midEnergy = meanSquare * (double) s.onsetEffectiveSamples;
            s.subEnergy = s.midEnergy * 1.0e-4;

            // 试算：每一路最大的 8 个块峰值（降序）。候选的水平：低切 +0.4 dB、收单 0 / 0 / +0.1 / +0.2 dB、加宽 +0.25 dB。
            constexpr std::array<double, 7> ratios { 1.0, 1.047, 1.0, 1.0, 1.0116, 1.0233, 1.0292 };
            s.liveTrialBlocks = n;

            for (size_t series = 0; series < ratios.size(); ++series)
            {
                auto value = spec.trialPeak * ratios[series] * (1.0 + 0.01 * noise());

                for (size_t k = 0; k < 8; ++k)
                {
                    s.liveTrialTop[series][k] = value;
                    value *= 0.995 - 0.003 * ((double) random.nextFloat());
                }
            }

            for (size_t c = 0; c < 2; ++c)
            {
                s.liveBlendIntended[c] = meanSquare * (double) s.onsetEffectiveSamples;
                s.liveBlendOutput[c] = s.liveBlendIntended[c] * (c == 0 ? 0.995 : 0.98);
            }

            // 定稿时算出（同 SegmentLedger::deriveSegment）
            s.effectiveSeconds = (double) n * (double) subBlockSamples / rate;
            s.heard = n > 0 && s.effectiveSeconds >= 0.5 * (double) s.nominalLength / rate;
            s.loudnessValid = n > 0;
            s.loudnessLufs = n > 0 ? (float) lufsOf (s.effectiveEnergy / (double) n) : 0.0f;
            double total = 0.0, lowPower = 0.0;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                total += s.powerSum[b];

                if (b <= 4)
                    lowPower += s.powerSum[b];
            }

            s.lowShareValid = s.frameSeconds >= 0.5 && total > 0.0;
            s.lowShare = s.lowShareValid ? (float) (lowPower / total) : 0.0f;
            return s;
        }

        // 一段读数改了响度 / 低频占比的段（m 公式的用例用）。
        Segment segmentReading (std::int64_t index, double loudnessLufs, double lowShare)
        {
            auto spec = dropSpec();
            spec.loudnessLufs = loudnessLufs;
            spec.lowShare = lowShare;
            spec.seed = (std::uint32_t) (index + 11);
            return makeSegment (spec, index);
        }

        // 准备好的实时画像（48 kHz）。LiveProfile 不可拷贝也不可移动，所以就地准备。
        void setUp (LiveProfile& live, const LiveConstants& constants = {}, MainSectionModel model = MainSectionModel::fourOnFloor)
        {
            live.prepare (rate);
            live.setConstants (constants, model);
        }

        // 交一段并让它发布（时间推到它的发布时刻）。
        void feed (LiveProfile& live, const Segment& segment)
        {
            live.segmentFinalized (segment);
            live.advanceTo (live.getNextPublicationTime());
        }

        void feedPinned (LiveProfile& live, const Segment& segment, float mainness)
        {
            live.commitWithMainness (segment, mainness);
            live.advanceTo (live.getNextPublicationTime());
        }

        LiveCommitRecord lastRecord (const LiveProfile& live)
        {
            LiveCommitRecord records[LiveProfile::commitCapacity];
            std::uint64_t cursor = 0;
            const auto count = std::abs (live.readCommits (cursor, records, LiveProfile::commitCapacity));
            return count > 0 ? records[count - 1] : LiveCommitRecord {};
        }

        std::vector<LiveSegmentJudgement> judgementsOf (const LiveProfile& live)
        {
            std::vector<LiveSegmentJudgement> out ((size_t) LiveProfile::windowCapacity);
            out.resize ((size_t) live.readJudgements (out.data(), (int) out.size()));
            return out;
        }

        // 两份窗口判断逐位相同（序号、起止、m′、权重）。
        bool sameJudgementLists (const std::vector<LiveSegmentJudgement>& a, const std::vector<LiveSegmentJudgement>& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t k = 0; k < a.size(); ++k)
                if (a[k].sequence != b[k].sequence || a[k].start != b[k].start || a[k].end != b[k].end
                    || ! sameFloat (a[k].mainness, b[k].mainness) || ! sameDouble (a[k].weightSeconds, b[k].weightSeconds))
                    return false;

            return true;
        }

        // 已发布的这一份里第一个非有限 / 越界的读数（画像的全部特征与逐段数组、诊断里跟着画像走的 M、c、参考、份额；withCounters 时另看
        // 计数器 activeSeconds / secondsSinceMain）；全都正常时返回空串。
        juce::String firstBadReading (const LiveProfile& live, bool withCounters = true)
        {
            const auto& p = live.getProfile();
            const Feature MusicProfile::* const features[] = {
                &MusicProfile::integratedLufs, &MusicProfile::loudnessRangeLu, &MusicProfile::plrDb, &MusicProfile::truePeakDbtp,
                &MusicProfile::spectralSlopeDbPerOct, &MusicProfile::lowShare, &MusicProfile::highShare, &MusicProfile::lowBandRangeDb,
                &MusicProfile::onsetDensityPerSecond, &MusicProfile::onsetStrengthDb, &MusicProfile::subShareDb, &MusicProfile::lowSideToMidDb,
                &MusicProfile::lowCorrelation, &MusicProfile::highSideToMidDb, &MusicProfile::highCorrelation, &MusicProfile::lowCutPeakCostDb,
                &MusicProfile::widthPeakCostDb };
            std::vector<const Feature*> all;

            for (const auto member : features)
                all.push_back (&(p.*member));

            for (const auto& f : p.monoPeakCostDb)
                all.push_back (&f);

            for (const auto& f : p.lowCutBlendLossDb)
                all.push_back (&f);

            for (size_t i = 0; i < all.size(); ++i)
            {
                const auto& f = *all[i];

                if (! std::isfinite (f.value) || ! std::isfinite (f.effectiveSeconds) || ! (f.confidence >= 0.0f && f.confidence <= 1.0f))
                    return "feature " + juce::String ((int) i) + ": value " + juce::String (f.value) + ", seconds " + juce::String (f.effectiveSeconds)
                           + ", confidence " + juce::String (f.confidence);
            }

            for (size_t b = 0; b < (size_t) numBands; ++b)
                if (! std::isfinite (p.bandLongTermDb[b]) || ! std::isfinite (p.tameEvidenceDb[b]) || ! std::isfinite (p.recoverEvidenceDb[b])
                    || ! (p.bandConfidence[b] >= 0.0f && p.bandConfidence[b] <= 1.0f) || ! std::isfinite (p.bandSideToMidDb[b])
                    || ! std::isfinite (p.bandCorrelation[b]) || ! (p.bandStereoConfidence[b] >= 0.0f && p.bandStereoConfidence[b] <= 1.0f))
                    return "band " + juce::String ((int) b) + ": long-term " + juce::String (p.bandLongTermDb[b]) + ", tame "
                           + juce::String (p.tameEvidenceDb[b]) + ", recover " + juce::String (p.recoverEvidenceDb[b]) + ", side / mid "
                           + juce::String (p.bandSideToMidDb[b]);

            const auto d = live.getDiagnostics();

            if (! std::isfinite (p.effectiveSeconds) || ! (p.representativeness >= 0.0f && p.representativeness <= 1.0f)
                || ! std::isfinite (d.mainMassSeconds) || ! (d.confidence >= 0.0f && d.confidence <= 1.0f) || ! std::isfinite (d.referenceLufs)
                || ! std::isfinite (d.lowReference)
                || (withCounters && (! std::isfinite (d.activeSeconds) || ! std::isfinite (d.secondsSinceMain)))
                || ! (d.oldestMainShare >= 0.0f && d.oldestMainShare <= 1.0f + 1.0e-6f)
                || ! (d.oldestReferenceShare >= 0.0f && d.oldestReferenceShare <= 1.0f + 1.0e-6f))
                return "diagnostics: M " + juce::String (d.mainMassSeconds) + ", c " + juce::String (d.confidence) + ", active "
                       + juce::String (d.activeSeconds);

            return {};
        }

        // 状态 XML 里一段的二进制布局（LiveProfile.cpp writeEntry）：从 at 起这一段各字段的字节位置，按 blob 里存的计数逐项走出来
        // （坏数据用例改字段用）。
        struct EntryLayout
        {
            size_t start = 0, end = 0, nominalLength = 0, effectiveSeconds = 0, loudnessLufs = 0, lowShare = 0, gatingCount = 0, rangeCount = 0;
            size_t rangeEnergy = 0, frames = 0, spectrum = 0, lowStereoWeight = 0, lowRangeFirst = 0, fluxFrames = 0, fluxEvents = 0;
            size_t onsetSamples = 0, onsetEvents = 0, strengthCount = 0, subEnergy = 0, midEnergy = 0, trialBlocks = 0, blend = 0;
            size_t subBlockLength = 0, hopLength = 0, admittedFlag = 0, admissionMainness = 0, newProgramRangeLu = 0, mainStamp = 0;
            size_t referenceStamp = 0, next = 0;

            // 8 张谱表按 LiveProfile.cpp SpectrumTable 的顺序：0 功率、1 Tame、2 Recover、3 缺失、4 有效、5 立体声功率、6 交叉、7 立体声权重。
            size_t spectrumAt (int table, int band) const   { return spectrum + (size_t) (table * numBands + band) * 8; }
        };

        EntryLayout layoutOfEntry (const juce::MemoryBlock& blob, size_t at)
        {
            const auto int32At = [&blob] (size_t offset)
            {
                std::int32_t x = 0;
                std::memcpy (&x, (const char*) blob.getData() + offset, sizeof (x));
                return (int) x;
            };

            EntryLayout l;
            l.start = at;                                  // start、end、nominalLength：int64 × 3
            l.end = at + 8;
            l.nominalLength = at + 16;
            l.effectiveSeconds = at + 31;                  // mode / usable / pinned（字节 × 3）、pinnedMainness（float）之后
            l.loudnessLufs = at + 40;                      // loudnessValid（字节）之后
            l.lowShare = at + 45;                          // lowShareValid（字节）之后；再后面是 truePeak（float）
            l.gatingCount = at + 53;
            l.rangeCount = l.gatingCount + 4 + 4 * (size_t) int32At (l.gatingCount);
            l.rangeEnergy = l.rangeCount + 4 + 2 * (size_t) int32At (l.rangeCount);
            l.frames = l.rangeEnergy + 8;
            l.spectrum = l.frames + 8;
            l.lowStereoWeight = l.spectrum + 8 * (size_t) numBands * 8;   // 再后面是 highStereoWeight
            l.lowRangeFirst = l.lowStereoWeight + 16;                     // lowRangeFirst、lowRangeLast、各格的计数（16 位）
            l.fluxFrames = l.lowRangeFirst + 8 + 2 * (size_t) juce::jmax (0, int32At (l.lowRangeFirst + 4) - int32At (l.lowRangeFirst) + 1);
            l.fluxEvents = l.fluxFrames + 8;
            l.onsetSamples = l.fluxFrames + 16;
            l.onsetEvents = l.fluxFrames + 24;
            l.strengthCount = l.fluxFrames + 32;
            l.subEnergy = l.strengthCount + 4 + (size_t) int32At (l.strengthCount);
            l.midEnergy = l.subEnergy + 8;
            l.trialBlocks = l.midEnergy + 8;
            l.blend = l.trialBlocks + 4 + 7 * 2 * (size_t) juce::jlimit (0, 8, int32At (l.trialBlocks));   // 7 路 × min(块数, 8) 个格号
            l.subBlockLength = l.blend + 2 * 16;                          // 两个候选 × (本该, 实际)
            l.hopLength = l.subBlockLength + 4;
            l.admittedFlag = l.hopLength + 4;
            l.admissionMainness = l.admittedFlag + 1 + 8;                 // 响度因子、低频因子、m（float × 3）
            l.newProgramRangeLu = l.admittedFlag + 1 + 12 + 2;            // 票、标志（字节 × 2）之后
            l.mainStamp = l.newProgramRangeLu + 4;
            l.referenceStamp = l.mainStamp + 8;
            l.next = ((const char*) blob.getData())[l.admittedFlag] != 0 ? l.referenceStamp + 8 : l.admittedFlag + 1;
            return l;
        }

        // 同 LiveProfile.cpp hexOf：状态 XML 里标量的写法（IEEE 位模式的 16 位十六进制）。
        juce::String hexOfDouble (double x)
        {
            std::uint64_t bits = 0;
            std::memcpy (&bits, &x, sizeof (bits));
            char digits[16];

            for (int i = 15; i >= 0; --i, bits >>= 4)
                digits[i] = "0123456789abcdef"[bits & 15u];

            return juce::String (digits, 16);
        }

        // 遗忘全关（等价性）：W_mem、参考遗忘、无主段衰减、换歌都取 +∞；M0 取极小，c = 1（House 画像的代表性此时也是 1）。
        LiveConstants noForgetting()
        {
            LiveConstants c;

            for (auto* model : { &c.fourOnFloor, &c.loudness, &c.passive })
            {
                model->memoryBars = 0.0;
                model->memorySeconds = infinity;
                model->confidenceBars = 0.0;
                model->confidenceSeconds = 1.0e-9;
            }

            c.referenceMemorySeconds = infinity;
            c.noMainSeconds = infinity;
            c.newProgramLu = std::numeric_limits<float>::infinity();
            return c;
        }

        // 每发布一份记一份快照（监听者；容量事先留好：回调里不分配）。
        struct Publication
        {
            LiveCommitRecord record;
            MusicProfile profile;
            LiveDiagnostics diagnostics;
        };

        struct Recorder final : LiveProfile::PublicationListener
        {
            Recorder (const LiveProfile& source, size_t capacity) : live (source)   { publications.reserve (capacity); }

            void livePublished (const LiveCommitRecord& record) noexcept override
            {
                if (publications.size() < publications.capacity())
                    publications.push_back ({ record, live.getProfile(), live.getDiagnostics() });
                else
                    overflow = true;
            }

            const LiveProfile& live;
            std::vector<Publication> publications;
            bool overflow = false;
        };

        bool samePublications (const std::vector<Publication>& a, const std::vector<Publication>& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
                if (! sameRecord (a[i].record, b[i].record) || ! sameProfile (a[i].profile, b[i].profile)
                    || ! sameDiagnostics (a[i].diagnostics, b[i].diagnostics))
                    return false;

            return true;
        }

        //==============================================================================
        // 分析器测试台（与链里的顺序相同：IN 表 → processInput → Balance → processSpectrum），宿主给恒定 124 BPM 的拍点。
        // 时间轴只由分析器报（processInput / processSpectrum 末尾的 advanceTo = 实时段落账交齐的位置），测试台不报——整链用例考的就是这一条路。
        struct AnalyzerRig
        {
            LoudnessMeter meter;
            BalanceModule balance;
            glaze::MusicAnalyzer analyzer;
            std::int64_t fed = 0;

            explicit AnalyzerRig (int maximumBlock = 512)
            {
                const juce::dsp::ProcessSpec spec { rate, (juce::uint32) maximumBlock, 2u };
                const BalanceModule::Params params {};
                meter.prepare (spec);
                balance.setParams (params);
                balance.prepare (spec);
                balance.setParams (params);
                analyzer.prepare (spec);
            }

            static glaze::HostInfo tempoHost (std::int64_t position)
            {
                glaze::HostInfo info;
                info.hasPlayState = true;
                info.isPlaying = true;
                info.hasPosition = true;
                info.positionSamples = position;
                info.hasTempo = true;
                info.bpm = tempoBpm;
                info.hasPpq = true;
                info.ppqPosition = (double) position * tempoBpm / (60.0 * rate);
                info.hasBarStart = true;
                info.ppqLastBarStart = std::floor (info.ppqPosition / 4.0) * 4.0;
                info.hasTimeSignature = true;
                return info;
            }

            void feed (const Signal& input, int blockSize)
            {
                const auto numSamples = (int) input.front().size();
                juce::AudioBuffer<float> block (2, blockSize);

                for (int start = 0; start < numSamples; start += blockSize)
                {
                    const auto length = juce::jmin (blockSize, numSamples - start);
                    block.setSize (2, length, false, false, true);

                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    analyzer.setHostInfo (tempoHost (fed));
                    meter.process (block);
                    analyzer.processInput (block, meter);
                    balance.process (block);
                    analyzer.processSpectrum (balance);
                    fed += length;
                }
            }
        };

        // 合成 House（124 BPM、4/4，两声道）：前奏只有底鼓（120 → 50 Hz 下滑）与反拍踩镲；drop 底鼓更响，加 55 Hz 低音（4 个谐波）
        // 与和声垫（左右相位不同）。末尾多 extraSeconds 秒 drop，让最后一小节交齐、定稿。
        Signal makeIntroDrop (int introBars, int dropBars, double extraSeconds)
        {
            const auto beat = 60.0 / tempoBpm;
            const auto dropStart = (double) introBars * 4.0 * beat;
            const auto numSamples = (int) std::lround (((double) (introBars + dropBars) * 4.0 * beat + extraSeconds) * rate);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            auto signal = makeSilence (2, numSamples);

            for (int ch = 0; ch < 2; ++ch)
            {
                juce::Random random (17 + ch);
                double previous = 0.0;

                for (int i = 0; i < numSamples; ++i)
                {
                    const auto t = (double) i / rate;
                    const auto drop = t >= dropStart;
                    const auto inBeat = std::fmod (t, beat);
                    const auto offBeat = std::fmod (t + 0.5 * beat, beat);
                    const auto kick = (drop ? 0.5 : 0.35) * std::exp (-inBeat / 0.12)
                                    * std::sin (twoPi * (50.0 * inBeat + 2.1 * (1.0 - std::exp (-inBeat / 0.03))));
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                    const auto hat = 0.09 * std::exp (-offBeat / 0.025) * (white - previous);
                    previous = white;
                    double tonal = 0.0;

                    if (drop)
                    {
                        for (int h = 1; h <= 4; ++h)
                            tonal += 0.22 / (double) h * std::sin (twoPi * 55.0 * (double) h * t);

                        tonal += 0.05 * (std::sin (twoPi * 220.0 * t + 0.6 * ch) + std::sin (twoPi * 329.63 * t + 0.6 * ch));
                    }

                    signal[(size_t) ch][(size_t) i] = (float) (kick + hat + tonal);
                }
            }

            return signal;
        }

        // 存下的段拷一份、把学习期的试算拷进实时那几项（House 门控那一份账本的 live* 只在学习期间记，与 trial* 同值；BRIEF 等价性用例的做法）。
        Segment withLiveTrials (const Segment& stored)
        {
            auto segment = stored;
            segment.liveTrialBlocks = stored.trialBlocks;
            segment.liveTrialTop = stored.trialTop;
            segment.liveBlendIntended = stored.blendIntended;
            segment.liveBlendOutput = stored.blendOutput;
            return segment;
        }

        // 一串段（交来的顺序、时刻都按 makeSegment 的小节网格）。
        std::vector<Segment> mixedSequence (int count, std::uint32_t seedBase = 100)
        {
            std::vector<Segment> segments;

            for (int i = 0; i < count; ++i)
            {
                const auto phase = (i / 8) % 4;
                auto spec = phase == 0 ? introSpec() : phase == 2 ? breakdownSpec() : dropSpec();
                spec.seed = seedBase + (std::uint32_t) i;
                segments.push_back (makeSegment (spec, i));
            }

            return segments;
        }
    }

    //==============================================================================
    class LiveProfileTests  : public juce::UnitTest
    {
    public:
        LiveProfileTests() : juce::UnitTest ("LiveProfile", "Celadon") {}

        void runTest() override
        {
            testHouseEquivalence();
            testBreakdownLeavesMainUntouched();
            testMainnessFormula();
            testRejudgeLowReference();
            testRejudgeFadeInColdStart();
            testRejudgeIntroLeavesMain();
            testMemoryUnits();
            testNoMainDecay();
            testNewProgram();
            testNewProgramLevelStable();
            testModelsAndFormat();
            testConstantsTable();
            testStateRoundTrip();
            testBadState();
            testCorruptStateStaysFinite();
            testRestoreRateConversionOverflow();
            testPublishGuard();
            testDeterminismAndReset();
            testCallPatterns();
            testAnalyzerToProfileBlockSizes();
            testWindowEviction();
            testFullQueueWithFullWindow();
            testNoAllocation();
            testCommitRing();
            testBadSegment();
            testTiming();
        }

    private:
        //==============================================================================
        // a. 等价性锚点（LiveProfile.h 文件头第四节）。分析器 House 档 16 小节前奏 + 16 小节 drop，学习中（试算跑着）；lockSegments = 1
        // 让 House 的代表性只看不重复素材（= 1），实时画像的 M0 取极小（c = 1）——两边的置信度都只剩各特征的观测充分度，可以逐位比。
        // 实时画像：遗忘与事件全关、斜坡宽 1e-6（硬门）、参考冻结为账本最终的 S_ref / lowRef，每段的 m′ 就是账本的 full（逐段核对）。
        // 容差（其余逐位）：
        //   - I / PLR ≤ 1e-4（两遍门限都逐块做，同 House 的公式与求和顺序，实测逐位）；
        //   - 试算代价 ≤ 0.01 dB：每路 P99 由 0.01 dB 一格的直方图按格中心插值，每个 P99 ≤ 半格 0.005 dB；
        //   - LRA：4/4 型主段加权，与 House（只用满编段）逐位；响度型用全部段，对照是测试里按 House 同一公式在全部存下的段上算的参照，逐位；
        //   - 按设计不同、单独核对的字段：effectiveSeconds（= M = 满编段有效时长之和）、formatVersion（2 / 3）、houseGate（不填）。
        void testHouseEquivalence()
        {
            beginTest ("a. equivalence anchor: forgetting off, references frozen to the ledger's, hard gates -> the analyzer's House profile");

            AnalyzerRig rig;
            glaze::HouseConstants house;
            house.lockSegments = 1;
            rig.analyzer.setHouseConstants (house);
            rig.analyzer.setGenre (glaze::Genre::deepHouse);
            rig.analyzer.startLearning();
            rig.feed (makeIntroDrop (16, 16, 1.0), 512);

            const auto& ledger = rig.analyzer.getSegmentLedger();
            const auto& expected = rig.analyzer.getCandidateProfile();
            const auto& gate = expected.houseGate;

            auto hard = noForgetting();
            hard.fourOnFloor.loudnessRampLu = 1.0e-6f;
            hard.fourOnFloor.lowRampWidth = 1.0e-6f;

            LiveProfile live;
            setUp (live, hard);
            live.setFrozenReferencesForTesting (true, gate.referenceLufs, gate.lowShareReference);
            LiveProfile computed;   // 同样的常数，参考不冻结（自己按窗口算）
            setUp (computed, hard);
            int fullSegments = 0, fullTrialBlocks = 0;
            auto allHeard = true;
            auto nearestGate = 1.0e9;

            for (int i = 0; i < ledger.getNumStored(); ++i)
            {
                const auto& stored = ledger.getStored (i);
                live.segmentFinalized (withLiveTrials (stored));
                computed.segmentFinalized (withLiveTrials (stored));
                live.flush();
                computed.flush();
                allHeard = allHeard && stored.heard;

                if (stored.loudnessValid)
                    nearestGate = juce::jmin (nearestGate, std::abs ((double) stored.loudnessLufs - ((double) gate.referenceLufs - 3.0)));

                if (stored.lowShareValid)
                    nearestGate = juce::jmin (nearestGate, std::abs ((double) stored.lowShare / (double) gate.lowShareReference - 0.6));

                if (stored.full)
                {
                    ++fullSegments;
                    fullTrialBlocks += stored.trialBlocks;
                }
            }

            const auto& p = live.getProfile();
            logMessage ("  stored " + juce::String (ledger.getNumStored()) + ", full " + juce::String (fullSegments) + " (trial blocks "
                        + juce::String (fullTrialBlocks) + "); House I " + juce::String (expected.integratedLufs.value, 4) + ", live I "
                        + juce::String (p.integratedLufs.value, 4) + "; S_ref ledger " + juce::String (gate.referenceLufs, 2) + ", computed "
                        + juce::String (computed.getDiagnostics().referenceLufs, 2) + "; lowRef ledger " + juce::String (gate.lowShareReference, 4)
                        + ", computed " + juce::String (computed.getDiagnostics().lowReference, 4) + "; nearest reading to a gate "
                        + juce::String (nearestGate, 4));

            // 用例的前提：drop 都是满编段；试算块在 10 s ～ 512 块之间（House 不走「不够 10 s 改用全部段」与 512 块截断）；代表性 = 1；
            // 没有一段的读数贴着门（硬门的斜坡宽 1e-6：贴着门的段 m′ 会是 0.5，House 是 1）。
            expect (gate.valid && fullSegments >= 14 && fullSegments <= 17, "the full segments are the drop bars");
            expect (fullTrialBlocks >= 100 && fullTrialBlocks <= 512, "full-segment trial blocks within 10 s ... 512 blocks");
            expect (sameFloat (expected.representativeness, 1.0f) && sameFloat (p.representativeness, 1.0f), "both representativeness 1");
            expect (nearestGate >= 1.0e-3, "no reading within 1e-3 of either gate");

            // 每段的 m′ = 账本的 full（硬门、冻结参考）。
            {
                const auto judgements = judgementsOf (live);
                auto same = judgements.size() == (size_t) ledger.getNumStored();

                for (size_t i = 0; same && i < judgements.size(); ++i)
                    same = sameFloat (judgements[i].mainness, ledger.getStored ((int) i).full ? 1.0f : 0.0f);

                expect (same, "every segment's m' is the ledger's full (1 / 0)");
            }

            // 不冻结时算出的参考 = 账本的（全部段都已听时，同一口径：S_ref = 各段段响度按有效时长加权的 P95 格中心；lowRef = 过响度门的段的
            // lowShare 的 P95），画像也就与冻结的逐位相同。
            if (allHeard)
            {
                const auto d = computed.getDiagnostics();
                expect (d.referenceValid && sameFloat (d.referenceLufs, gate.referenceLufs), "computed S_ref = the ledger's");
                expect (d.lowReferenceValid && sameFloat (d.lowReference, gate.lowShareReference), "computed lowRef = the ledger's");
                expect (sameProfile (computed.getProfile(), p), "the computed-reference profile = the frozen-reference profile");
            }

            const auto iDiff = std::abs (p.integratedLufs.value - expected.integratedLufs.value);
            expect (p.integratedLufs.valid && expected.integratedLufs.valid && iDiff <= 1.0e-4f, "I within 1e-4 LU: " + juce::String (iDiff, 7));
            logMessage ("  I: House " + juce::String (expected.integratedLufs.value, 7) + ", live " + juce::String (p.integratedLufs.value, 7)
                        + (sameFloat (p.integratedLufs.value, expected.integratedLufs.value) ? " (bitwise)" : " (not bitwise)"));
            expect (sameDouble (p.integratedLufs.effectiveSeconds, expected.integratedLufs.effectiveSeconds)
                        && sameFloat (p.integratedLufs.confidence, expected.integratedLufs.confidence),
                    "I effective seconds (M = full seconds) and confidence bitwise");
            const auto plrDiff = std::abs (p.plrDb.value - expected.plrDb.value);
            expect (p.plrDb.valid && plrDiff <= 1.0e-4f && sameDouble (p.plrDb.effectiveSeconds, expected.plrDb.effectiveSeconds)
                        && sameFloat (p.plrDb.confidence, expected.plrDb.confidence),
                    "PLR within 1e-4 dB (= true peak - I): " + juce::String (plrDiff, 7));
            expect (sameFeature (p.truePeakDbtp, expected.truePeakDbtp), "true peak bitwise (largest full segment)");

            // LRA，4/4 型（Max 2026-10-02「按曲风分口径」）：主段加权 = House 只用满编段，逐位。
            expect (sameFeature (p.loudnessRangeLu, expected.loudnessRangeLu) && expected.loudnessRangeLu.valid,
                    "4/4 LRA bitwise (main-weighted = House, full segments only)");

            // LRA，响度型：全部段上的参照（House 同一公式，测试里在全部存下的段上算）。换模型只改读数口径、不动窗口，所以同一实例换过去就能比。
            {
                std::array<std::uint64_t, 800> counts {};
                double energy = 0.0;
                std::uint64_t total = 0;

                for (int i = 0; i < ledger.getNumStored(); ++i)
                {
                    const auto& stored = ledger.getStored (i);

                    for (int k = 0; k < stored.rangeCount; ++k)
                        ++counts[(size_t) stored.rangeBin[(size_t) k]];

                    energy += stored.rangeEnergy;
                    total += (std::uint64_t) stored.rangeCount;
                }

                Feature reference;
                const auto seconds = (double) total * (double) subBlockSamples / rate;
                const auto first = firstBinAtOrAbove (lufsOf (energy / (double) total) - 20.0);
                std::uint64_t gated = 0;

                for (int bin = first; bin < 800; ++bin)
                    gated += counts[(size_t) bin];

                const auto lowIndex = (std::uint64_t) ((double) (gated - 1) * 0.10 + 0.5);
                const auto highIndex = (std::uint64_t) ((double) (gated - 1) * 0.95 + 0.5);
                std::uint64_t cumulative = 0;
                double low = 0.0, high = 0.0;
                auto haveLow = false;

                for (int bin = first; bin < 800; ++bin)
                {
                    cumulative += counts[(size_t) bin];

                    if (! haveLow && cumulative > lowIndex)
                    {
                        low = binCentre (bin);
                        haveLow = true;
                    }

                    if (cumulative > highIndex)
                    {
                        high = binCentre (bin);
                        break;
                    }
                }

                reference.value = (float) (high - low);
                reference.valid = gated >= 10;
                reference.effectiveSeconds = seconds;

                const auto before = live.getProfile();
                live.setConstants (hard, MainSectionModel::loudness);
                const auto loudnessModel = live.getProfile().loudnessRangeLu;
                live.setConstants (hard, MainSectionModel::fourOnFloor);
                logMessage ("  LRA: 4/4 (full segments) " + juce::String (before.loudnessRangeLu.value, 2) + " LU, loudness model (all segments) "
                            + juce::String (loudnessModel.value, 2) + " LU");
                expect (loudnessModel.valid == reference.valid && sameFloat (loudnessModel.value, reference.value)
                            && sameDouble (loudnessModel.effectiveSeconds, reference.effectiveSeconds),
                        "loudness-model LRA over all segments bitwise (House formula)");
                expect (sameProfile (before, live.getProfile()), "switching the model back gives the same profile bitwise");
            }

            // 频谱、立体声、起音、次声：逐位。
            const Feature MusicProfile::* const bitwise[] = {
                &MusicProfile::spectralSlopeDbPerOct, &MusicProfile::lowShare, &MusicProfile::highShare, &MusicProfile::lowBandRangeDb,
                &MusicProfile::onsetDensityPerSecond, &MusicProfile::onsetStrengthDb, &MusicProfile::subShareDb, &MusicProfile::lowSideToMidDb,
                &MusicProfile::lowCorrelation, &MusicProfile::highSideToMidDb, &MusicProfile::highCorrelation };
            const char* const names[] = { "slope", "lowShare", "highShare", "lowBandRange", "onset density", "onset strength", "sub share",
                                          "low S/M", "low correlation", "high S/M", "high correlation" };

            for (size_t i = 0; i < std::size (bitwise); ++i)
                expect (sameFeature (p.*bitwise[i], expected.*bitwise[i]) && (expected.*bitwise[i]).valid, juce::String (names[i]) + " bitwise");

            expect (sameFloats (p.bandLongTermDb, expected.bandLongTermDb) && sameFloats (p.tameEvidenceDb, expected.tameEvidenceDb)
                        && sameFloats (p.recoverEvidenceDb, expected.recoverEvidenceDb) && sameFloats (p.bandConfidence, expected.bandConfidence),
                    "27-band levels, Tame / Recover evidence, band confidence bitwise");
            expect (sameFloats (p.bandSideToMidDb, expected.bandSideToMidDb) && sameFloats (p.bandCorrelation, expected.bandCorrelation)
                        && sameFloats (p.bandStereoConfidence, expected.bandStereoConfidence),
                    "per-band stereo bitwise");

            // 试算：代价 ≤ 0.01 dB，有效性 / 有效时长 / 置信度逐位；混合损失整项逐位。
            auto worstCost = 0.0f;
            const auto checkCost = [&] (const Feature& a, const Feature& b, const juce::String& label)
            {
                const auto diff = std::abs (a.value - b.value);
                worstCost = juce::jmax (worstCost, diff);
                logMessage ("  " + label + ": House " + juce::String (b.value, 4) + " dB, live " + juce::String (a.value, 4) + " (diff "
                            + juce::String (diff, 5) + ")");
                expect (a.valid && b.valid && diff <= 0.01f && sameDouble (a.effectiveSeconds, b.effectiveSeconds)
                            && sameFloat (a.confidence, b.confidence),
                        label + " within 0.01 dB");
            };

            checkCost (p.lowCutPeakCostDb, expected.lowCutPeakCostDb, "low-cut cost");
            checkCost (p.widthPeakCostDb, expected.widthPeakCostDb, "width cost");

            for (size_t i = 0; i < p.monoPeakCostDb.size(); ++i)
                checkCost (p.monoPeakCostDb[i], expected.monoPeakCostDb[i], "mono cost " + juce::String ((int) i));

            logMessage ("  worst trial-cost difference " + juce::String (worstCost, 5) + " dB");

            for (size_t c = 0; c < p.lowCutBlendLossDb.size(); ++c)
            {
                const auto& a = p.lowCutBlendLossDb[c];
                const auto& b = expected.lowCutBlendLossDb[c];
                // T-049（main 9c18ff9）起 House 也给混合损失置信度 = min(代表性, 试算观测充分度)，与实时画像同一口径：整项逐位。
                expect (a.valid && b.valid && sameFeature (a, b), "blend loss " + juce::String ((int) c) + " bitwise (confidence included)");
                expect (sameFloat (a.confidence, fractionOf (a.effectiveSeconds, 10.0)),
                        "blend loss confidence = min(c, observation) like the general profile");
            }

            // 按设计不同的几项。
            expect (sameDouble (p.effectiveSeconds, gate.fullSeconds), "effectiveSeconds = main mass M = the full segments' effective seconds");
            expect (p.formatVersion == glaze::clayProfileFormatVersion && ! p.houseGate.valid, "format 2, no houseGate");
            expect (expected.formatVersion == glaze::houseProfileFormatVersion);
        }

        //==============================================================================
        // b. breakdown（m′ = 0）里什么都不动（规格第 3 节；主段时钟只按此刻的 m′ 走）。出厂常数、4/4 型：24 小节 drop 之后 8 小节 breakdown
        // （比 drop 轻 12 LU、低频占比 0.1：两个因子都是 0；8 小节 15.5 s，不到无主段的 60 s；更轻又没有低音，不算「换歌」）——每一份画像
        // （4/4 型的 LRA 也是主段加权）、M、c、参考都与 breakdown 之前逐位相同；之后 16 小节更响、更多低频的新 drop：I 与 lowShare 向它移动。
        void testBreakdownLeavesMainUntouched()
        {
            beginTest ("b. a breakdown (m' = 0) leaves the published profile, M, c and the references bit-identical; a new drop moves it");

            LiveProfile live;
            setUp (live);
            std::int64_t index = 0;

            for (int i = 0; i < 24; ++i)
                feed (live, makeSegment (dropSpec(), index++));

            const auto before = live.getProfile();
            const auto diagnosticsBefore = live.getDiagnostics();
            auto allZero = true, unchanged = true;

            for (int i = 0; i < 8; ++i)
            {
                feed (live, makeSegment (breakdownSpec(), index++));
                allZero = allZero && sameFloat (lastRecord (live).mainness, 0.0f);
                unchanged = unchanged && sameProfile (before, live.getProfile(), true) && sameMain (diagnosticsBefore, live.getDiagnostics());
            }

            const auto diagnosticsAfter = live.getDiagnostics();
            expect (allZero, "the breakdown bars get m' = 0");
            expect (unchanged, "every published profile (the 4/4 LRA included), M, c and the references stay bit-identical");
            expect (! diagnosticsAfter.timeDecayActive && diagnosticsAfter.newProgramCount == 0);

            auto louder = dropSpec();
            louder.loudnessLufs = -6.03;
            louder.lowShare = 0.7;
            louder.seed = 9;

            for (int i = 0; i < 16; ++i)
                feed (live, makeSegment (louder, index++));

            const auto& moved = live.getProfile();
            logMessage ("  I " + juce::String (before.integratedLufs.value, 2) + " -> " + juce::String (moved.integratedLufs.value, 2)
                        + " LUFS, lowShare " + juce::String (before.lowShare.value, 3) + " -> " + juce::String (moved.lowShare.value, 3));
            expect (moved.integratedLufs.value > before.integratedLufs.value + 0.5f && moved.integratedLufs.value < -5.9f,
                    "I moves toward the new drop");
            expect (moved.lowShare.value > before.lowShare.value + 0.03f && moved.lowShare.value < 0.7f, "lowShare moves toward the new drop");
        }

        //==============================================================================
        // c. 主段度（规格第 2 节；LiveProfile.h 文件头第一节）。
        void testMainnessFormula()
        {
            beginTest ("c. mainness: two soft gates, ramp end points, cold start, passive / loudness models; admission = previous mainnessFor");

            const LiveConstants factory;
            const auto& ff = factory.fourOnFloor;

            // 冷启动：以这一段自己为参考 → ramp(ΔG) × ramp(1 − g_low) = 1；读不出响度 → 0。
            {
                LiveProfile live;
                setUp (live);
                expect (sameFloat (live.mainnessFor (true, -30.0f, true, 0.05f), 1.0f), "cold start: the first segment is main");
                expect (sameFloat (live.mainnessFor (false, -8.0f, true, 0.6f), 0.0f), "no effective audio: m = 0");

                auto c = factory;
                c.fourOnFloor.gateWidthLu = 0.0f;
                live.setConstants (c, MainSectionModel::fourOnFloor);
                expectWithinAbsoluteError (live.mainnessFor (true, -30.0f, true, 0.05f), 0.5f, 1.0e-6f, "cold start with dG = 0: ramp(0) = 0.5");

                // 第一段的参考就是它自己（不用特例）：出厂常数下进账与这一份画像里都是 1。
                LiveProfile first;
                setUp (first);
                feed (first, segmentReading (0, -30.0, 0.05));
                const auto r = lastRecord (first);
                expect (sameFloat (r.admissionMainness, 1.0f) && sameFloat (r.mainness, 1.0f) && r.referenceValid && r.lowReferenceValid,
                        "the first published segment is main both at admission and in its own profile");
            }

            LiveProfile live;
            setUp (live);

            for (int i = 0; i < 10; ++i)
                feed (live, segmentReading (i, -8.03, 0.6037));

            const auto d = live.getDiagnostics();
            const auto sRef = d.referenceLufs;
            const auto lowRef = d.lowReference;
            expect (d.referenceValid && sameFloat (sRef, -8.05f), "S_ref = the P95 bin centre (-8.1 ... -8.0 LUFS bin)");
            expect (d.lowReferenceValid && std::abs (lowRef - 0.6035f) < 1.0e-6f, "lowRef = the P95 bin centre (0.603 ... 0.604)");

            const auto g = ff.gateWidthLu, w = ff.loudnessRampLu, gLow = ff.lowGateFactor, wLow = ff.lowRampWidth;
            const auto m = [&] (float loudness, float share) { return live.mainnessFor (true, loudness, true, share); };

            expectWithinAbsoluteError (m (sRef - g, lowRef), 0.5f, 1.0e-5f, "L = S_ref - dG: loudness factor 0.5 (low factor 1)");
            expectWithinAbsoluteError (m (sRef - g + 0.5f * w, lowRef), 1.0f, 1.0e-5f, "ramp top end");
            expectWithinAbsoluteError (m (sRef - g - 0.5f * w, lowRef), 0.0f, 1.0e-5f, "ramp bottom end");
            expectWithinAbsoluteError (m (sRef - g + 0.25f * w, lowRef), 0.75f, 1.0e-5f, "ramp is linear in between");
            expectWithinAbsoluteError (m (sRef, lowRef * gLow), 0.5f, 1.0e-5f, "lowShare = lowRef x g_low: low factor 0.5");
            expectWithinAbsoluteError (m (sRef, lowRef * (gLow + 0.5f * wLow)), 1.0f, 1.0e-5f, "low ramp top end");
            expectWithinAbsoluteError (m (sRef, lowRef * (gLow - 0.5f * wLow)), 0.0f, 1.0e-5f, "low ramp bottom end");
            expectWithinAbsoluteError (m (sRef - g, lowRef * gLow), 0.25f, 1.0e-5f, "soft AND: the two factors multiply");
            expect (sameFloat (live.mainnessFor (true, sRef, false, 0.0f), 0.0f), "4/4: lowShare unreadable -> m = 0");

            // 进账判断 = 交来之前的 mainnessFor（上一份已发布画像的参考）；这一份画像对它的判断 = 发布之后的 mainnessFor（这一份的参考）；
            // w′ = m′ × 有效时长。
            {
                auto same = true;
                const std::array<std::pair<double, double>, 6> readings { { { -9.5, 0.55 }, { -11.2, 0.6 }, { -8.0, 0.36 }, { -10.9, 0.4 },
                                                                           { -14.0, 0.62 }, { -7.5, 0.7 } } };
                std::int64_t index = 10;

                for (const auto& reading : readings)
                {
                    const auto segment = segmentReading (index++, reading.first, reading.second);
                    const auto admission = live.mainnessFor (segment.loudnessValid, segment.loudnessLufs, segment.lowShareValid, segment.lowShare);
                    feed (live, segment);
                    const auto judged = live.mainnessFor (segment.loudnessValid, segment.loudnessLufs, segment.lowShareValid, segment.lowShare);
                    const auto record = lastRecord (live);
                    same = same && sameFloat (record.admissionMainness, admission) && sameFloat (record.mainness, judged)
                        && sameFloat (record.loudnessFactor * record.lowFactor, judged)
                        && sameDouble (record.weightSeconds, (double) judged * segment.effectiveSeconds);
                }

                expect (same, "admission m = mainnessFor before; m' = mainnessFor after publication; w' = m' x effective seconds");
            }

            // 响度型：只看响度（ΔG 6、斜坡 3、P90），低频读不读得出都一样；被动型：有效音频 m = 1。
            {
                live.setConstants (factory, MainSectionModel::loudness);
                const auto s90 = live.getDiagnostics().referenceLufs;
                const auto& lm = factory.loudness;
                expectWithinAbsoluteError (live.mainnessFor (true, s90 - lm.gateWidthLu, false, 0.0f), 0.5f, 1.0e-5f, "loudness model: 0.5 at S_ref - dG");
                expect (sameFloat (live.mainnessFor (true, s90 - lm.gateWidthLu, false, 0.0f), live.mainnessFor (true, s90 - lm.gateWidthLu, true, 0.01f)),
                        "loudness model ignores lowShare");

                live.setConstants (factory, MainSectionModel::passive);
                expect (sameFloat (live.mainnessFor (true, -60.0f, false, 0.0f), 1.0f) && sameFloat (live.mainnessFor (false, 0.0f, true, 0.5f), 0.0f),
                        "passive model: m = 1 for effective audio, 0 without");
            }
        }

        //==============================================================================
        // r1. lowRef 按此刻的 S_ref 选段（REPORT-C 第 5 节第 2 条，tech 0268597）：16 小节低音很重的前奏（lowShare 0.45），之后响 4 LU、
        // 低频占比只有 0.2 的 drop。前奏一掉到 S_ref − ΔG 之下就不再参与 lowRef，drop 不再被低频门挡：第 4 个 drop 小节起 m′ = 1，
        // 前奏全部重判为 0，I 就是 drop 的。
        void testRejudgeLowReference()
        {
            beginTest ("r. rejudge: a bass-heavy intro no longer pins lowRef; the drop passes the low gate within 4 bars");

            LiveProfile live;
            setUp (live);
            std::int64_t index = 0;
            auto intro = introSpec();
            intro.loudnessLufs = -12.03;
            intro.lowShare = 0.45;

            for (int i = 0; i < 16; ++i)
                feed (live, makeSegment (intro, index++));

            auto drop = dropSpec();
            drop.loudnessLufs = -8.03;
            drop.lowShare = 0.2;
            std::vector<float> dropMainness;

            for (int i = 0; i < 12; ++i)
            {
                feed (live, makeSegment (drop, index++));
                dropMainness.push_back (lastRecord (live).mainness);
            }

            const auto judgements = judgementsOf (live);
            auto introOut = true, dropIn = true;

            for (const auto& j : judgements)
            {
                if (j.sequence < 16)
                    introOut = introOut && sameFloat (j.mainness, 0.0f) && j.weightSeconds == 0.0;
                else
                    dropIn = dropIn && sameFloat (j.mainness, 1.0f);
            }

            const auto d = live.getDiagnostics();
            logMessage ("  drop m' at publication: " + juce::String (dropMainness[0], 3) + ", " + juce::String (dropMainness[1], 3) + ", "
                        + juce::String (dropMainness[2], 3) + ", " + juce::String (dropMainness[3], 3) + "; lowRef " + juce::String (d.lowReference, 4)
                        + ", I " + juce::String (live.getProfile().integratedLufs.value, 3));
            expect (dropMainness[3] >= 0.99f && dropMainness.back() >= 0.99f, "from the 4th drop bar on the drop is main at its publication");
            expect (introOut && dropIn, "in the final profile the intro is rejudged out (m' = 0, weight 0) and every drop bar is main");
            expect (d.lowReferenceValid && std::abs (d.lowReference - 0.2f) < 0.01f, "lowRef is the drop's lowShare");
            expect (std::abs (live.getProfile().integratedLufs.value + 8.03f) < 0.2f, "I is the drop's level");
        }

        // r2. 冷启动落在淡入小节上不卡住（REPORT-C 第 5 节第 3 条，tech 0343381）：第 1 小节 −34 LUFS 的淡入（lowShare 0.15），之后只有镲
        // （−20 LUFS、lowShare 0.03）。第 2 小节一来 S_ref 就移到镲上、淡入掉出门外，lowRef 换成镲的，镲按自己的口径过两道门。
        void testRejudgeFadeInColdStart()
        {
            beginTest ("r. rejudge: a cold start on a fade-in bar does not stick; the next bars take over the references");

            LiveProfile live;
            setUp (live);
            feed (live, segmentReading (0, -34.03, 0.15));
            const auto fadeIn = live.getProfile().integratedLufs.value;
            std::vector<float> levels, mainnessOfHats;

            for (int i = 1; i <= 6; ++i)
            {
                auto hats = dropSpec();
                hats.loudnessLufs = -20.03;
                hats.lowShare = 0.03;
                hats.seed = (std::uint32_t) (40 + i);
                feed (live, makeSegment (hats, i));
                levels.push_back (live.getProfile().integratedLufs.value);
                mainnessOfHats.push_back (lastRecord (live).mainness);
            }

            logMessage ("  I after the fade-in " + juce::String (fadeIn, 2) + ", then " + juce::String (levels[0], 2) + ", " + juce::String (levels[1], 2)
                        + ", ... " + juce::String (levels.back(), 2) + " LUFS; hats m' " + juce::String (mainnessOfHats[0], 3) + ", "
                        + juce::String (mainnessOfHats[1], 3));
            expect (std::abs (fadeIn + 34.03f) < 0.2f, "the fade-in alone is the first profile (cold start = its own reference)");
            expect (mainnessOfHats[0] >= 0.99f && std::abs (levels[0] + 20.03f) < 0.3f, "the first hats bar is main and I jumps to it at once");
            expect (judgementsOf (live).front().mainness == 0.0f, "the fade-in bar is rejudged out");
        }

        // r3. 前奏被重判出主段（REPORT-C 第 5 节第 1 条、M4）：32 小节前奏（−12 LUFS、低频 0.5），之后响 3.85 LU 的 drop。冷启动时前奏是
        // 主段；drop 一来 S_ref 移上去、前奏掉出门外，第一个 drop 的 4 小节之后 I 离 drop 不到 1 LU（改前按进账遗忘要几十小节），前奏不再推
        // 主段时钟（它的权重是 0）。c 在 drop 来时随 M 掉下来再爬回去（M 只剩 drop 的质量）。
        void testRejudgeIntroLeavesMain()
        {
            beginTest ("r. rejudge: the intro taken as main at cold start drops out when the drop comes; I is within 1 LU after 4 drop bars");

            LiveProfile live;
            setUp (live);
            std::int64_t index = 0;
            auto intro = introSpec();
            intro.loudnessLufs = -11.88;

            for (int i = 0; i < 32; ++i)
                feed (live, makeSegment (intro, index++));

            const auto introLevel = live.getProfile().integratedLufs.value;
            const auto introConfidence = live.getDiagnostics().confidence;
            std::vector<float> levels, confidences;

            for (int i = 0; i < 16; ++i)
            {
                feed (live, makeSegment (dropSpec(), index++));
                levels.push_back (live.getProfile().integratedLufs.value);
                confidences.push_back (live.getDiagnostics().confidence);
            }

            const auto dropLevel = levels.back();
            logMessage ("  intro I " + juce::String (introLevel, 2) + " (c " + juce::String (introConfidence, 3) + "); drop bars 1 ... 4: I "
                        + juce::String (levels[0], 2) + ", " + juce::String (levels[1], 2) + ", " + juce::String (levels[2], 2) + ", "
                        + juce::String (levels[3], 2) + " (c " + juce::String (confidences[3], 3) + "); after 16 bars I " + juce::String (dropLevel, 2)
                        + " (c " + juce::String (confidences.back(), 3) + ")");
            expect (introConfidence > 0.9f, "the intro alone was main (cold start) and confident");
            expect (std::abs (levels[3] - dropLevel) <= 1.0f && std::abs (dropLevel + 8.03f) < 0.2f, "4 drop bars in, I is within 1 LU of the drop's");

            auto introWeight = 0.0;

            for (const auto& j : judgementsOf (live))
                if (j.sequence < 32)
                    introWeight += j.weightSeconds;

            expect (introWeight == 0.0, "the intro carries no weight once rejudged");
            expect (confidences.back() > 0.8f, "c climbs back above 0.8 within 16 drop bars");
        }

        //==============================================================================
        // W_mem 与 M0 的单位（规格第 3 节；LiveConstants 注释）：按小节分段时 = 小节数 × 段的标称长度，2 秒段时 = 秒数常数。两段主段之后
        // M = w1 × exp(−w2 ÷ W_mem) + w2（第一段被比它新的主段进账遗忘），c = 1 − exp(−M ÷ M0)；representativeness = c、effectiveSeconds = M。
        void testMemoryUnits()
        {
            beginTest ("W_mem and M0 follow the segment mode: bars x the bar length, or the seconds constants for 2-second segments");

            const LiveConstants factory;
            const auto& ff = factory.fourOnFloor;

            for (const auto mode : { glaze::SegmentMode::bars, glaze::SegmentMode::seconds })
            {
                LiveProfile live;
                setUp (live);
                auto first = makeSegment (dropSpec(), 0);
                auto second = makeSegment (dropSpec(), 1);

                if (mode == glaze::SegmentMode::seconds)
                {
                    for (auto* segment : { &first, &second })
                    {
                        segment->mode = mode;
                        segment->nominalLength = 96000;
                    }
                }

                const auto nominal = (double) first.nominalLength / rate;
                const auto memory = mode == glaze::SegmentMode::bars ? ff.memoryBars * nominal : ff.memorySeconds;
                const auto m0 = mode == glaze::SegmentMode::bars ? ff.confidenceBars * nominal : ff.confidenceSeconds;

                feed (live, first);
                const auto w1 = lastRecord (live).weightSeconds;
                feed (live, second);
                const auto w2 = lastRecord (live).weightSeconds;
                const auto expectedMass = w1 * std::exp (-w2 / memory) + w2;
                const auto d = live.getDiagnostics();
                const auto label = juce::String (mode == glaze::SegmentMode::bars ? "bars" : "2-second segments");
                expect (w1 > 1.8 && w2 > 1.8, label + ": both segments are main");
                expect (std::abs (d.mainMassSeconds - expectedMass) <= 1.0e-12 * expectedMass, label + ": M = w1 x exp(-w2 / W_mem) + w2");
                expectWithinAbsoluteError (d.confidence, (float) (1.0 - std::exp (-expectedMass / m0)), 1.0e-6f, label + ": c = 1 - exp(-M / M0)");
                expect (sameFloat (live.getProfile().representativeness, d.confidence) && sameDouble (live.getProfile().effectiveSeconds, d.mainMassSeconds),
                        label + ": representativeness = c, effectiveSeconds = M");

                const auto judgements = judgementsOf (live);
                expect (judgements.size() == 2 && std::abs (judgements[0].weightSeconds - w1 * std::exp (-w2 / memory)) <= 1.0e-12 * w1
                            && sameDouble (judgements[1].weightSeconds, w2),
                        label + ": the judgements carry each segment's forgotten weight");
            }
        }

        //==============================================================================
        // d1. 长时间无主段（规格第 3 节）：16 小节主段之后，安静小节（m′ = 0）的有效音频累计到 60 s 之前 M 逐位不动；越过 60 s 的那一段只按
        // 越过的部分衰减（τ 120 s），之后每段按自己的有效时长衰减；再来一个主段，时间衰减停。换歌关掉（隔离）。
        void testNoMainDecay()
        {
            beginTest ("d. no main section for 60 s: main mass decays by effective time (tau 120 s) from the 60 s mark on, until a main bar");

            LiveConstants c;
            c.newProgramLu = std::numeric_limits<float>::infinity();
            LiveProfile live;
            setUp (live, c);
            std::int64_t index = 0;

            for (int i = 0; i < 16; ++i)
                feed (live, makeSegment (dropSpec(), index++));

            auto quiet = breakdownSpec();
            quiet.loudnessLufs = -30.03;
            auto frozenOk = true, decayOk = true, sawDecay = false;

            for (int i = 0; i < 40; ++i)
            {
                const auto before = live.getDiagnostics();
                const auto segment = makeSegment (quiet, index++);
                feed (live, segment);
                const auto after = live.getDiagnostics();
                const auto record = lastRecord (live);
                const auto excess = juce::jmax (0.0, before.secondsSinceMain + segment.effectiveSeconds - juce::jmax (before.secondsSinceMain, c.noMainSeconds));

                if (excess <= 0.0)
                {
                    frozenOk = frozenOk && sameDouble (before.mainMassSeconds, after.mainMassSeconds) && ! record.timeDecay;
                }
                else
                {
                    sawDecay = true;
                    const auto expectedMass = before.mainMassSeconds * std::exp (-excess / c.noMainDecaySeconds);
                    decayOk = decayOk && record.timeDecay && std::abs (after.mainMassSeconds - expectedMass) <= 1.0e-12 * expectedMass;
                }
            }

            expect (frozenOk, "before 60 s without a main bar the main mass is frozen");
            expect (sawDecay && decayOk, "after the 60 s mark it decays by exp(-excess / 120 s)");
            expect (live.getDiagnostics().timeDecayActive);

            feed (live, makeSegment (dropSpec(), index++));
            expect (! lastRecord (live).timeDecay && ! live.getDiagnostics().timeDecayActive && live.getDiagnostics().secondsSinceMain == 0.0,
                    "a main bar stops the time decay");
        }

        // d2. 换歌软遗忘（规格第 3 节；Max 2026-10-02「响的算、轻的要有低音」）。每段 1.9 s，第 11 段累计 20.9 s ≥ 20 s 时触发：全部更老的段
        // × 0.2（M 正好 × 0.2）；第 10 段（19 s）不触发；中间夹一段只差 2 LU 的，清零重计。breakdown（轻 7 LU、没有低音）永不触发；
        // 响度型里更轻的段永不触发；只响 5.9 LU 的永不触发。无主段衰减关掉。
        // 「响」的那一支要 I 跟不上新素材才数得满 20 s：回看重判下更响的新素材一两小节就当上 S_ref、成了主段（I 立即跟上），这一支实际上
        // 用不上（用例 4 核对）；所以用冻结参考（只给测试用）把 I 钉在旧 drop 上来考这一支的规则本身。
        void testNewProgram()
        {
            beginTest ("d. new program: louder by > 6 LU, or (4/4) quieter by > 6 LU with full bass, for 20 s scales every older mass by 0.2");

            LiveConstants c;
            c.noMainSeconds = infinity;
            const auto keep = (double) c.newProgramKeep;   // float 0.2f：换成 double 是 0.2000000030

            struct Run
            {
                int triggeredAt = -1;
                double massBefore = 0.0, massAfter = 0.0;
            };

            // 前 16 小节 drop 立起主段，返回此刻的 I。
            const auto prime = [&] (LiveProfile& live, MainSectionModel model, std::int64_t& index)
            {
                setUp (live, c, model);

                for (int i = 0; i < 16; ++i)
                    feed (live, makeSegment (dropSpec(), index++));

                return (double) live.getProfile().integratedLufs.value;
            };

            const auto runBars = [&] (LiveProfile& live, const Spec& spec, int bars, std::int64_t& index)
            {
                Run run;

                for (int i = 0; i < bars; ++i)
                {
                    const auto before = live.getDiagnostics().mainMassSeconds;
                    feed (live, makeSegment (spec, index++));

                    if (lastRecord (live).newProgram && run.triggeredAt < 0)
                    {
                        run.triggeredAt = i + 1;
                        run.massBefore = before;
                        run.massAfter = live.getDiagnostics().mainMassSeconds;
                    }
                }

                return run;
            };

            // 1. 4/4 型：轻 7 LU、低音满（lowShare 同 drop）、电平稳——像换成一首更轻的歌。这些段 m′ = 0（在 S_ref − ΔG 之下），M 只在触发时变。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::fourOnFloor, index);
                auto quieterWithBass = dropSpec();
                quieterWithBass.loudnessLufs = level - 7.0;

                auto run = runBars (live, quieterWithBass, 10, index);
                expect (run.triggeredAt < 0 && live.getDiagnostics().newProgramCount == 0, "quieter with bass: 10 bars (19 s) do not trigger");
                expect (lastRecord (live).newProgramVote == 2, "they vote 'quieter with bass'");

                feed (live, segmentReading (index++, level - 2.0, 0.6));   // 只差 2 LU：不算，清零
                expect (lastRecord (live).newProgramVote == 0);
                run = runBars (live, quieterWithBass, 10, index);
                expect (run.triggeredAt < 0, "a bar that does not count restarts the count: 10 more bars still do not trigger");

                run = runBars (live, quieterWithBass, 1, index);
                expect (run.triggeredAt == 1 && live.getDiagnostics().newProgramCount == 1, "quieter with bass: the 11th consecutive bar (20.9 s) triggers");
                expect (std::abs (run.massAfter - keep * run.massBefore) <= 1.0e-12 * run.massBefore, "every older mass x newProgramKeep (main mass exactly)");
                expect (lastRecord (live).newProgramRangeValid && lastRecord (live).newProgramRangeLu <= 0.01f, "the level-stable range was measured (flat)");
            }

            // 2. 4/4 型：breakdown（轻 7 LU、没有低音）永不触发——规格「breakdown 里什么都不动」。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::fourOnFloor, index);
                auto breakdown = breakdownSpec();
                breakdown.loudnessLufs = level - 7.0;
                runBars (live, breakdown, 40, index);
                expect (live.getDiagnostics().newProgramCount == 0, "a 40-bar breakdown (7 LU quieter, no bass) never triggers");
            }

            // 3. 「响」的那一支（I 钉住：冻结参考在旧 drop 上，响 7 LU、低频 0.1 的段过不了冻结的低频门，m′ = 0）：第 11 段触发；之后只响
            // 5.9 LU（不超过 6）的永不触发。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::fourOnFloor, index);
                const auto d = live.getDiagnostics();
                live.setFrozenReferencesForTesting (true, d.referenceLufs, d.lowReference);
                auto louder = dropSpec();
                louder.lowShare = 0.1;
                louder.loudnessLufs = level + 7.0;
                const auto run = runBars (live, louder, 11, index);
                expect (run.triggeredAt == 11 && live.getDiagnostics().newProgramCount == 1, "louder by 7 LU (I held): the 11th bar triggers");

                auto nearby = louder;
                nearby.loudnessLufs = (double) live.getProfile().integratedLufs.value + 5.9;
                runBars (live, nearby, 40, index);
                expect (live.getDiagnostics().newProgramCount == 1, "louder by 5.9 LU never triggers");
            }

            // 4. 不冻结时，响 7 LU 的新素材一两小节就当上 S_ref、成了主段，I 立即跟上，「响」的那一支数不满 20 s——回看重判自己就把旧素材挤出去了。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::fourOnFloor, index);
                auto louder = dropSpec();
                louder.loudnessLufs = level + 7.0;
                runBars (live, louder, 3, index);
                const auto followed = live.getProfile().integratedLufs.value;
                runBars (live, louder, 17, index);
                logMessage ("  louder by 7 LU, references free: I after 3 bars " + juce::String (followed, 2) + " LUFS (new material "
                            + juce::String (level + 7.0, 2) + ")");
                expect (std::abs ((double) followed - (level + 7.0)) < 0.5 && live.getDiagnostics().newProgramCount == 0,
                        "references free: the louder material is main within 3 bars and never needs the soft forget");
            }

            // 5. 响度型：轻 7 LU、低音满也永不触发（「轻」的那一支只给 4/4 型）。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::loudness, index);
                auto quieterWithBass = dropSpec();
                quieterWithBass.loudnessLufs = level - 7.0;
                runBars (live, quieterWithBass, 40, index);
                expect (live.getDiagnostics().newProgramCount == 0, "loudness model: quieter segments never trigger");
            }
        }

        // d3. 「轻的那一支要电平稳」（Max 2026-10-02；newProgramRangeLu 起点 2 LU）：轻 7 LU、低音满的一串，段响度在两个值之间交替——
        // 起伏 1.9 LU 时第 11 段（20.9 s）触发，2.1 LU 时一直不触发；一路往下掉的淡出（每小节 −0.3 LU：20 s 里掉 3 LU）不触发，
        // 同样的淡出在 newProgramRangeLu = ∞（改前）时第 11 段触发。
        void testNewProgramLevelStable()
        {
            beginTest ("d. new program, quieter branch: only a level-stable run counts (range 1.9 LU triggers, 2.1 LU does not, a fade-out never)");

            const auto runAlternating = [this] (double rangeLu, double rangeLimit, int bars, bool fade)
            {
                LiveConstants c;
                c.noMainSeconds = infinity;
                c.newProgramRangeLu = (float) rangeLimit;
                LiveProfile live;
                setUp (live, c);
                std::int64_t index = 0;

                for (int i = 0; i < 16; ++i)
                    feed (live, makeSegment (dropSpec(), index++));

                const auto level = (double) live.getProfile().integratedLufs.value;
                int triggeredAt = -1;
                float lastRange = 0.0f;

                for (int i = 0; i < bars; ++i)
                {
                    auto spec = dropSpec();
                    spec.loudnessLufs = fade ? level - 7.0 - 0.3 * (double) i : level - 7.0 - (i % 2 == 0 ? 0.0 : rangeLu);
                    spec.seed = (std::uint32_t) (200 + i);
                    feed (live, makeSegment (spec, index++));
                    const auto record = lastRecord (live);

                    if (record.newProgramRangeValid)
                        lastRange = record.newProgramRangeLu;

                    if (record.newProgram && triggeredAt < 0)
                        triggeredAt = i + 1;
                }

                logMessage ("  " + juce::String (fade ? "fade-out -0.3 LU/bar" : "alternating " + juce::String (rangeLu, 1) + " LU")
                            + ", limit " + juce::String (rangeLimit, 1) + ": triggered at bar " + juce::String (triggeredAt) + ", last range "
                            + juce::String (lastRange, 3) + " LU");
                return triggeredAt;
            };

            expect (runAlternating (1.9, 2.0, 30, false) == 11, "range 1.9 LU: triggers at the 11th bar (20.9 s)");
            expect (runAlternating (2.1, 2.0, 30, false) < 0, "range 2.1 LU: never triggers");
            expect (runAlternating (0.0, 2.0, 30, true) < 0, "a fade-out going down 0.3 LU per bar never triggers");
            expect (runAlternating (0.0, infinity, 30, true) == 11, "the same fade-out triggers with newProgramRangeLu = inf (the old rule)");
        }

        //==============================================================================
        // 主段模型与格式字段：4/4 型的 I 用主段加权的门块；换成响度型时（窗口保留）I 立即换成全部段的 BS.1770 积分（遗忘关掉时等于测试里
        // 按定义逐块算的参照）；格式 2、不填 houseGate；commitWithMainness 钳位并在每次重判都用钉死的值。
        void testModelsAndFormat()
        {
            beginTest ("models: 4/4 I is main-weighted, the loudness model's I is every segment's BS.1770 integral; format 2; pinned mainness");

            LiveProfile live;
            setUp (live, noForgetting());
            std::vector<Segment> segments;
            std::int64_t index = 0;

            for (int i = 0; i < 12; ++i)
                segments.push_back (makeSegment (dropSpec(), index++));

            for (int i = 0; i < 12; ++i)   // drop 之后的前奏：按 drop 的 S_ref 判，m′ = 0
                segments.push_back (makeSegment (introSpec(), index++));

            for (const auto& segment : segments)
                feed (live, segment);

            const auto mainI = live.getProfile().integratedLufs.value;
            live.setConstants (noForgetting(), MainSectionModel::loudness);
            const auto allI = live.getProfile().integratedLufs.value;

            std::vector<double> blocks;

            for (const auto& segment : segments)
                for (int k = 0; k < segment.gatingCount; ++k)
                    blocks.push_back ((double) segment.gatingPower[(size_t) k]);

            double sum = 0.0;

            for (auto x : blocks)
                sum += x;

            const auto gate = lufsOf (sum / (double) blocks.size()) - 10.0;
            const auto gatePower = std::pow (10.0, 0.1 * (gate + 0.691));
            double gated = 0.0;
            int count = 0;

            for (auto x : blocks)
                if (x >= gatePower)
                {
                    gated += x;
                    ++count;
                }

            const auto reference = lufsOf (gated / (double) count);
            logMessage ("  4/4 (main) I " + juce::String (mainI, 3) + ", loudness-model (all) I " + juce::String (allI, 3) + ", reference "
                        + juce::String (reference, 3));
            expect (mainI > allI + 1.0f, "the main-weighted I is the drop's, above the all-segment integral");
            expect (std::abs ((double) allI - reference) <= 1.0e-4, "loudness model: the all-segment BS.1770 integral (per block, like House)");

            expect (live.getProfile().formatVersion == glaze::clayProfileFormatVersion && ! live.getProfile().houseGate.valid);

            live.setConstants (noForgetting(), MainSectionModel::fourOnFloor);
            feedPinned (live, makeSegment (dropSpec(), index++), 2.0f);
            const auto high = lastRecord (live).mainness;
            feedPinned (live, makeSegment (dropSpec(), index++), -1.0f);
            const auto low = lastRecord (live).mainness;
            feedPinned (live, makeSegment (dropSpec(), index++), std::numeric_limits<float>::quiet_NaN());
            const auto nan = lastRecord (live).mainness;
            feedPinned (live, makeSegment (introSpec(), index++), 0.7f);
            expect (sameFloat (high, 1.0f) && sameFloat (low, 0.0f) && sameFloat (nan, 0.0f), "commitWithMainness clamps to 0 ... 1, NaN -> 0");

            feed (live, makeSegment (dropSpec(), index++));
            const auto judgements = judgementsOf (live);
            expect (judgements.size() >= 3 && sameFloat (judgements[judgements.size() - 2].mainness, 0.7f)
                        && sameFloat (judgements[judgements.size() - 3].mainness, 0.0f),
                    "pinned segments keep their mainness through later rejudging");
        }

        // 公开常数的取值表（LiveProfile.h visitLiveConstantPairs）：键数、每个坏值换成出厂值、合法的 ∞ 原样保留。
        void testConstantsTable()
        {
            beginTest ("constants table: 37 keys; sanitiseLiveConstants replaces every out-of-domain value by the factory value, keeps legal inf");

            const LiveConstants factory;
            int keys = 0;
            visitLiveConstants (factory, [&keys] (const char*, const char*, const auto&, glaze::LiveConstantDomain) { ++keys; });
            expect (keys == 37, "37 keys: " + juce::String (keys));

            // 每个键单独坏一次：NaN 一律坏；-1 在 ≥ 0 / > 0 / [0, 1] / (0, 1] 的范围里坏。
            LiveConstants damaged;
            visitLiveConstants (damaged, [] (const char*, const char*, auto& field, glaze::LiveConstantDomain domain)
            {
                using Field = std::remove_reference_t<decltype (field)>;
                field = domain == glaze::LiveConstantDomain::finite || domain == glaze::LiveConstantDomain::notNan
                            ? std::numeric_limits<Field>::quiet_NaN() : (Field) -1;
            });

            const auto clean = glaze::sanitiseLiveConstants (damaged);
            auto allFactory = true;
            glaze::visitLiveConstantPairs (clean, factory, [&allFactory] (const char*, const char*, const auto& a, const auto& b, glaze::LiveConstantDomain)
            {
                allFactory = allFactory && std::memcmp (&a, &b, sizeof (a)) == 0;
            });

            expect (allFactory, "every damaged field went back to its factory value");

            auto legal = factory;
            legal.noMainSeconds = infinity;
            legal.fourOnFloor.memorySeconds = infinity;
            legal.newProgramRangeLu = std::numeric_limits<float>::infinity();
            legal.loudness.memoryBars = -infinity;
            const auto kept = glaze::sanitiseLiveConstants (legal);
            expect (std::isinf (kept.noMainSeconds) && std::isinf (kept.fourOnFloor.memorySeconds) && std::isinf (kept.newProgramRangeLu)
                        && std::isinf (kept.loudness.memoryBars) && kept.loudness.memoryBars < 0.0,
                    "legal infinities are kept");

            auto illegal = factory;
            illegal.publishDelaySeconds = infinity;
            illegal.fourOnFloor.gateWidthLu = std::numeric_limits<float>::infinity();
            const auto fixed = glaze::sanitiseLiveConstants (illegal);
            expect (fixed.publishDelaySeconds == factory.publishDelaySeconds && fixed.fourOnFloor.gateWidthLu == factory.fourOnFloor.gateWidthLu,
                    "inf where it is not allowed goes back to the factory value");
        }

        //==============================================================================
        // e. 状态 XML 往返（LiveProfile.h：读回后已发布的那一份逐位等于保存时）。四种状态：出厂常数 40 段；事件过（换歌、无主段衰减：两个
        // 事件时钟 ≠ 0）；窗口挤满（600 段，前 88 段已丢）；保存时还有没发布的段（读回时并进来 = 原件发布完之后）。之后两边再交同样的段
        // 仍逐位相同（状态完整）；另经过 XML 文本往返一次；冷启动；跨采样率。
        void testStateRoundTrip()
        {
            beginTest ("e. state XML round trip is bit-exact (events, a full window, pending segments); the restored copy evolves identically");

            LiveConstants eventful;
            eventful.noMainSeconds = 5.0;          // 时间衰减很快就开始
            eventful.newProgramSeconds = 4.0;      // 换歌很快就触发

            struct Variant
            {
                const char* name;
                LiveConstants constants;
                int segments;
                int pending;
            };

            const Variant variants[] = { { "factory constants, 40 bars", LiveConstants {}, 40, 0 },
                                         { "events (new program, time decay)", eventful, 60, 0 },
                                         { "full window (600 bars)", LiveConstants {}, 600, 0 },
                                         { "3 segments not yet published", LiveConstants {}, 40, 3 } };

            for (const auto& variant : variants)
            {
                LiveProfile a;
                setUp (a, variant.constants);
                auto segments = mixedSequence (variant.segments + variant.pending + 10);
                auto louder = dropSpec();
                louder.loudnessLufs = 0.97;
                louder.lowShare = 0.1;

                if (variant.constants.newProgramSeconds < 10.0)   // 事件那一组：每 8 段夹 3 段「响 9 LU」的
                    for (size_t i = 0; i < segments.size(); ++i)
                        if (i % 8 >= 5)
                            segments[i] = makeSegment (louder, (std::int64_t) i);

                for (int i = 0; i < variant.segments; ++i)
                    feed (a, segments[(size_t) i]);

                for (int i = 0; i < variant.pending; ++i)
                    a.segmentFinalized (segments[(size_t) (variant.segments + i)]);

                const auto xml = a.createStateXml();
                const auto text = xml->toString();
                logMessage ("  state XML: " + juce::String (text.getNumBytesAsUTF8()) + " bytes (" + variant.name + "; window "
                            + juce::String (a.getDiagnostics().windowSegments) + ", new programs " + juce::String ((int) a.getDiagnostics().newProgramCount)
                            + ")");

                LiveProfile b;
                setUp (b, variant.constants);
                expect (b.restoreStateXml (*xml), juce::String (variant.name) + ": restore succeeds");

                a.flush();   // 原件把还没发布的段发布完（读回的副本已经并进来了）
                const auto judgementsA = judgementsOf (a), judgementsB = judgementsOf (b);
                auto sameJudgements = judgementsA.size() == judgementsB.size();

                for (size_t k = 0; sameJudgements && k < judgementsA.size(); ++k)
                    sameJudgements = judgementsA[k].sequence == judgementsB[k].sequence && sameFloat (judgementsA[k].mainness, judgementsB[k].mainness)
                                  && sameDouble (judgementsA[k].weightSeconds, judgementsB[k].weightSeconds);

                expect (sameProfile (a.getProfile(), b.getProfile()) && sameDiagnostics (a.getDiagnostics(), b.getDiagnostics(), false) && sameJudgements,
                        juce::String (variant.name) + ": restored profile, references, M, c and judgements bit-identical");
                expect (a.getDiagnostics().commits == b.getDiagnostics().commits
                            && a.getDiagnostics().newProgramCount == b.getDiagnostics().newProgramCount
                            && sameDouble (a.getDiagnostics().secondsSinceMain, b.getDiagnostics().secondsSinceMain),
                        juce::String (variant.name) + ": counters restored");

                auto parsed = juce::parseXML (text);
                LiveProfile viaText;
                setUp (viaText, variant.constants);
                expect (parsed != nullptr && viaText.restoreStateXml (*parsed) && sameProfile (a.getProfile(), viaText.getProfile()),
                        juce::String (variant.name) + ": round trip through the XML text");

                auto same = true;

                for (int i = 0; i < 10; ++i)
                {
                    const auto& segment = segments[(size_t) (variant.segments + variant.pending + i)];
                    feed (a, segment);
                    feed (b, segment);
                    same = same && sameProfile (a.getProfile(), b.getProfile()) && sameDiagnostics (a.getDiagnostics(), b.getDiagnostics(), false)
                        && sameRecord (lastRecord (a), lastRecord (b), false);
                }

                expect (same, juce::String (variant.name) + ": after restoring, the same segments give the same results (the state is complete)");
            }

            LiveProfile empty;
            setUp (empty);
            LiveProfile copy;
            setUp (copy);
            expect (copy.restoreStateXml (*empty.createStateXml()) && sameProfile (empty.getProfile(), copy.getProfile())
                        && sameDiagnostics (empty.getDiagnostics(), copy.getDiagnostics()),
                    "the cold state round-trips too");

            // 采样率不同：按样本计的量换算到新采样率（同一段时长），画像的时长与比值不变（只差换算的舍入）。
            {
                LiveProfile a;
                setUp (a);

                for (int i = 0; i < 20; ++i)
                    feed (a, makeSegment (dropSpec(), i));

                LiveProfile b;
                b.prepare (2.0 * rate);
                b.setConstants (LiveConstants {}, MainSectionModel::fourOnFloor);
                expect (b.restoreStateXml (*a.createStateXml()), "restores at another sample rate");
                const auto& pa = a.getProfile();
                const auto& pb = b.getProfile();
                expect (std::abs (pa.onsetDensityPerSecond.value - pb.onsetDensityPerSecond.value) <= 1.0e-5f
                            && std::abs (pa.lowShare.effectiveSeconds - pb.lowShare.effectiveSeconds) <= 1.0e-9
                            && std::abs (pa.loudnessRangeLu.effectiveSeconds - pb.loudnessRangeLu.effectiveSeconds) <= 1.0e-9
                            && std::abs (pa.lowCutPeakCostDb.effectiveSeconds - pb.lowCutPeakCostDb.effectiveSeconds) <= 1.0e-9
                            && std::abs (pa.integratedLufs.value - pb.integratedLufs.value) <= 1.0e-5f,
                        "sample-based quantities are converted: durations and rates are unchanged");
            }
        }

        // e. 坏数据：标签 / 格式版本不对、缺属性、标量坏、计数坏、窗口与段数对不上、base64 坏、段的二进制截短 / 多出 / 里面有越界的格号或
        // NaN → false，冷启动（之前的窗口也清掉）；之后再交同样的段与全新的实例逐位相同。P1F：有限但越出物理上界的量（试算块数 ±1e9、
        // 1e300 的各种累计、比一段还长的时长、印记与时钟对不上、荒谬的采样率与计数器……）同样 false、冷启动，之后的读数全是有限值。
        void testBadState()
        {
            beginTest ("e. bad state data (version, attributes, scalars, counts, base64, truncated / extra / corrupt segment bytes, out-of-bounds magnitudes) -> cold start, false");

            LiveProfile source;
            setUp (source);

            for (int i = 0; i < 12; ++i)
                feed (source, makeSegment (dropSpec(), i));

            const auto good = source.createStateXml();
            LiveProfile cold;
            setUp (cold);
            const auto coldProfile = cold.getProfile();

            // 段的二进制：解出来、改、再编回去。
            const auto blobOf = [] (const juce::XmlElement& x)
            {
                juce::MemoryOutputStream decoded;
                juce::Base64::convertFromBase64 (decoded, x.getStringAttribute ("segments"));
                return decoded.getMemoryBlock();
            };

            const auto setBlob = [] (juce::XmlElement& x, const juce::MemoryBlock& block)
            {
                x.setAttribute ("segments", juce::Base64::toBase64 (block.getData(), block.getSize()));
            };

            // 第一段某个字段写成 value（P1F：越出物理上界的量级，字节位置按 writeEntry 的布局走出来）。
            const auto first = layoutOfEntry (blobOf (*good), 0);

            const auto poke = [&] (auto value, size_t offset)
            {
                return [&blobOf, &setBlob, value, offset] (juce::XmlElement& x)
                {
                    auto block = blobOf (x);
                    block.copyFrom (&value, (int) offset, sizeof (value));
                    setBlob (x, block);
                };
            };

            struct Damage
            {
                const char* what;
                std::function<void (juce::XmlElement&)> apply;
            };

            const std::vector<Damage> damages {
                { "unknown format version", [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", 1); } },
                { "wrong tag", [] (juce::XmlElement& x) { x.setTagName ("LIVE_PROFILE_X"); } },
                { "missing attribute", [] (juce::XmlElement& x) { x.removeAttribute ("mainClock"); } },
                { "NaN scalar", [] (juce::XmlElement& x) { x.setAttribute ("activeSeconds", "7ff8000000000000"); } },
                { "infinite scalar", [] (juce::XmlElement& x) { x.setAttribute ("runSeconds", "7ff0000000000000"); } },
                { "negative clock", [] (juce::XmlElement& x) { x.setAttribute ("mainClock", "bff0000000000000"); } },
                { "short hex", [] (juce::XmlElement& x) { x.setAttribute ("referenceClock", "3ff00000"); } },
                { "bad count", [] (juce::XmlElement& x) { x.setAttribute ("commits", "-3"); } },
                { "window does not match the commits", [] (juce::XmlElement& x) { x.setAttribute ("window", 11); } },
                { "too many pending", [] (juce::XmlElement& x) { x.setAttribute ("pending", 99); } },
                { "garbage base64", [] (juce::XmlElement& x) { x.setAttribute ("segments", "!!!not base64!!!"); } },
                { "truncated segments",
                  [&] (juce::XmlElement& x) { auto block = blobOf (x); block.setSize (block.getSize() - 5); setBlob (x, block); } },
                { "extra bytes after the segments",
                  [&] (juce::XmlElement& x) { auto block = blobOf (x); block.append ("zz", 2); setBlob (x, block); } },
                { "NaN inside a segment (effective seconds of the first)",
                  [&] (juce::XmlElement& x)
                  {
                      auto block = blobOf (x);
                      const double nan = std::numeric_limits<double>::quiet_NaN();
                      block.copyFrom (&nan, 8 * 3 + 3 + 4, sizeof (nan));   // start, end, nominal、mode / usable / pinned、pinnedMainness 之后
                      setBlob (x, block);
                  } },
                { "a range-bin index out of range inside a segment",
                  [&] (juce::XmlElement& x)
                  {
                      auto block = blobOf (x);
                      // 第一段：…、truePeak 之后 gatingCount（19）、19 个门块、rangeCount、第一个格号。
                      const size_t offset = 8 * 3 + 3 + 4 + 8 + 1 + 4 + 1 + 4 + 4 + 4 + 19 * 4 + 4;
                      const std::uint16_t badBin = 900;
                      block.copyFrom (&badBin, (int) offset, sizeof (badBin));
                      setBlob (x, block);
                  } },
                // ---- P1F：有限但越出物理上界的量（LiveProfile.cpp 文件头第六节「量级」）。VAL1 F5：负的试算块数原来先进 deriveEntry、
                // 整数乘法溢出（未定义行为）才被挡下；VAL1 F1：1e300 原来被收下，下一份画像的 tameEvidenceDb 成了 ∞。----
                { "trial blocks -1e9 (VAL1 F5)", poke ((std::int32_t) -1000000000, first.trialBlocks) },
                { "trial blocks INT_MIN", poke (std::numeric_limits<std::int32_t>::min(), first.trialBlocks) },
                { "trial blocks +1e9 (a segment has at most 130)", poke ((std::int32_t) 1000000000, first.trialBlocks) },
                { "Tame sum 1e300 in band 15 (VAL1 F1)", poke (1.0e300, first.spectrumAt (1, 15)) },
                { "effective seconds 1e300", poke (1.0e300, first.effectiveSeconds) },
                { "effective seconds 13 s (a segment is at most 12.8 s)", poke (13.0, first.effectiveSeconds) },
                { "frames 1e300", poke (1.0e300, first.frames) },
                { "band power 1e300 in band 3", poke (1.0e300, first.spectrumAt (0, 3)) },
                { "valid weight above the frame count", poke (1.0e6, first.spectrumAt (4, 7)) },
                { "a cross power beyond the power (band 3)", poke (-1.0e300, first.spectrumAt (6, 3)) },
                { "short-term energy 1e300", poke (1.0e300, first.rangeEnergy) },
                { "onset samples 1e300", poke (1.0e300, first.onsetSamples) },
                { "onset events 1e300", poke (1.0e300, first.onsetEvents) },
                { "flux events above the flux frames", poke (1.0e6, first.fluxEvents) },
                { "mid energy 1e300", poke (1.0e300, first.midEnergy) },
                { "blend energy 1e300", poke (1.0e300, first.blend) },
                { "segment loudness 1e30 LUFS", poke (1.0e30f, first.loudnessLufs) },
                { "low share 1.5", poke (1.5f, first.lowShare) },
                { "a segment 2^40 samples long", poke ((std::int64_t) 1 << 40, first.end) },
                { "a nominal length of 2^40 samples", poke ((std::int64_t) 1 << 40, first.nominalLength) },
                { "a segment that ends before it starts", poke ((std::int64_t) -5, first.end) },
                { "hop length 2^30 samples", poke ((std::int32_t) 1 << 30, first.hopLength) },
                { "admission mainness 2", poke (2.0f, first.admissionMainness) },
                { "a main stamp 1e300 (past the clock)", poke (1.0e300, first.mainStamp) },
                { "a main stamp above the next segment's", poke (1.0, first.mainStamp) },
                { "a main clock other than the newest stamp", [] (juce::XmlElement& x) { x.setAttribute ("mainClock", hexOfDouble (1.0)); } },
                { "a reference clock without new programs", [] (juce::XmlElement& x) { x.setAttribute ("referenceClock", hexOfDouble (1.0)); } },
                { "sample rate 1e300", [] (juce::XmlElement& x) { x.setAttribute ("sampleRate", hexOfDouble (1.0e300)); } },
                { "sample rate 1 Hz (below 8 kHz)", [] (juce::XmlElement& x) { x.setAttribute ("sampleRate", hexOfDouble (1.0)); } },
                { "active seconds 1e300", [] (juce::XmlElement& x) { x.setAttribute ("activeSeconds", hexOfDouble (1.0e300)); } },
                { "seconds since main above the active seconds",
                  [] (juce::XmlElement& x) { x.setAttribute ("secondsSinceMain", hexOfDouble (x.getIntAttribute ("commits") * 13.0)); } },
                { "more new programs than commits", [] (juce::XmlElement& x) { x.setAttribute ("newProgramCount", "13"); } } };

            // 读档失败后再交同样的段，必须与全新的实例逐位相同：读到一半的数据也清干净了。
            std::vector<Segment> after;

            for (int i = 0; i < 20; ++i)
                after.push_back (makeSegment (i % 4 == 0 ? breakdownSpec() : dropSpec(), 100 + i));

            LiveProfile fresh;
            setUp (fresh);

            for (const auto& segment : after)
                feed (fresh, segment);

            for (const auto& damage : damages)
            {
                auto xml = std::make_unique<juce::XmlElement> (*good);
                damage.apply (*xml);

                LiveProfile target;
                setUp (target);

                for (int i = 0; i < 5; ++i)
                    feed (target, makeSegment (introSpec(), i));

                const auto restored = target.restoreStateXml (*xml);
                expect (! restored && target.getDiagnostics().commits == 0 && sameProfile (target.getProfile(), coldProfile),
                        juce::String (damage.what) + ": false and cold");

                for (const auto& segment : after)
                    feed (target, segment);

                expect (sameProfile (target.getProfile(), fresh.getProfile()) && sameDiagnostics (target.getDiagnostics(), fresh.getDiagnostics()),
                        juce::String (damage.what) + ": nothing of the half-read state is left");
                const auto bad = firstBadReading (target);
                expect (bad.isEmpty(), juce::String (damage.what) + ": every later reading is finite and in range " + bad);
            }

            // 阳性对照：正常的状态照样读回（上面改的都是同一份状态的一个字段）。
            LiveProfile copy;
            setUp (copy);
            expect (copy.restoreStateXml (*good) && sameProfile (copy.getProfile(), source.getProfile()), "the undamaged state restores");
        }

        // e. 随机损坏的状态（P1F；VAL1 的随机对照 P4 固化进来）：400 份——1 ～ 6 个随机字节、一个 8 字节位置写成极端的有限值、截短、
        // 改计数属性、改标量属性——读档不崩；读回 false 的是冷启动；读回 true 的（损坏小到量级都在物理上界之内）之后再交段、发布，
        // 每个读数仍是有限值、置信度在 [0, 1]。出厂常数与「事件常发」（时钟、印记不为 0）各一半。
        void testCorruptStateStaysFinite()
        {
            beginTest ("e. 400 randomly corrupted states: no crash; rejected ones cold-start; accepted ones keep every later reading finite");

            LiveConstants eventful;
            eventful.noMainSeconds = 5.0;
            eventful.newProgramSeconds = 4.0;
            LiveProfile cold;
            setUp (cold);
            juce::Random rng (20261004);
            int accepted = 0, bad = 0, notCold = 0;
            juce::String firstBad;

            for (int trial = 0; trial < 400; ++trial)
            {
                const auto constants = trial % 2 == 0 ? LiveConstants {} : eventful;
                const auto segments = mixedSequence (60, (std::uint32_t) (1500 + trial));
                const auto count = 40 + rng.nextInt (15);
                LiveProfile source;
                setUp (source, constants);
                std::int64_t time = 0;

                for (int i = 0; i < count; ++i)   // 时刻随机落后：存档时常有还没发布的段
                {
                    source.segmentFinalized (segments[(size_t) i]);
                    time = juce::jmax (time, segments[(size_t) i].end + (std::int64_t) rng.nextInt (90000));
                    source.advanceTo (time);
                }

                auto xml = source.createStateXml();
                juce::MemoryOutputStream decoded;
                juce::Base64::convertFromBase64 (decoded, xml->getStringAttribute ("segments"));
                auto block = decoded.getMemoryBlock();
                const auto mode = rng.nextInt (7);
                juce::String detail ("mode " + juce::String (mode));

                if (mode <= 2)   // 1 ～ 6 个随机字节换成随机值
                {
                    for (int k = 0; k < 1 + rng.nextInt (6); ++k)
                        block[(size_t) rng.nextInt ((int) block.getSize())] = (char) rng.nextInt (256);
                }
                else if (mode == 3)   // 一个 8 字节的位置（不一定对齐字段）写成极端的有限值
                {
                    const double extremes[] = { 1.0e308, -1.0e308, 1.0e-320, 4.0e9, 1.0e19, 1.0e300 };
                    const auto value = extremes[rng.nextInt (6)];
                    const auto at = rng.nextInt ((int) block.getSize() - 8);
                    block.copyFrom (&value, at, sizeof (value));
                    detail << " value " << juce::String (value, 3) << " at byte " << juce::String (at);
                }
                else if (mode == 4)   // 截短
                {
                    block.setSize ((size_t) rng.nextInt ((int) block.getSize()));
                }
                else if (mode == 5)   // 计数属性
                {
                    const char* names[] = { "commits", "window", "pending", "runStart", "newProgramCount" };
                    xml->setAttribute (names[rng.nextInt (5)], juce::String ((juce::int64) rng.nextInt (1000)));
                }
                else   // 标量属性写成极端的有限值
                {
                    const char* names[] = { "sampleRate", "mainClock", "referenceClock", "secondsSinceMain", "runSeconds", "activeSeconds" };
                    const double extremes[] = { 1.0e300, 1.0e-300, 4.0e9, 3.0, 96000.0 };
                    xml->setAttribute (names[rng.nextInt (6)], hexOfDouble (extremes[rng.nextInt (5)]));
                }

                xml->setAttribute ("segments", juce::Base64::toBase64 (block.getData(), block.getSize()));
                LiveProfile target;
                setUp (target, constants);

                if (! target.restoreStateXml (*xml))
                {
                    const auto d = target.getDiagnostics();
                    notCold += d.commits == 0 && d.pendingSegments == 0 && sameProfile (target.getProfile(), cold.getProfile()) ? 0 : 1;
                    continue;
                }

                ++accepted;

                for (int i = count; i < (int) segments.size(); ++i)
                    feed (target, segments[(size_t) i]);

                target.flush();
                const auto problem = firstBadReading (target);

                if (problem.isNotEmpty())
                {
                    ++bad;

                    if (firstBad.isEmpty())
                        firstBad = "trial " + juce::String (trial) + " " + detail + ": " + problem;
                }
            }

            logMessage ("  400 corrupted states: " + juce::String (accepted) + " accepted (damage within the bounds), " + juce::String (400 - accepted)
                        + " rejected" + (firstBad.isEmpty() ? juce::String() : "; first bad reading: " + firstBad));
            expect (notCold == 0, "every rejected state leaves a cold instance (" + juce::String (notCold) + " did not)");
            expect (bad == 0, "every accepted state keeps the later readings finite and in range (" + juce::String (bad) + " did not)");
        }

        // 读档换采样率时的越界检查（LiveProfile.cpp convertEntryRate；验收代理 VAL3 发现这个拒收分支没有用例，删掉检查全部用例照过）：
        // 窗口里每段的 start / end 写成接近 int64 上端、段长不变——entryWithinBounds 只看段长、不看绝对值，同采样率读回照收（不换算，无害）；
        // 读进 96 k 实例要把时间戳 × 2，越出 int64，必须整份拒收并冷启动，不能带着溢出的时间戳进窗口。
        void testRestoreRateConversionOverflow()
        {
            beginTest ("state: timestamps near the int64 limit are kept at the same rate and rejected when the restore has to convert the rate");

            LiveProfile source;
            setUp (source);

            constexpr int segmentCount = 12;

            for (int i = 0; i < segmentCount; ++i)
                feed (source, makeSegment (dropSpec(), i));

            auto xml = source.createStateXml();
            juce::MemoryOutputStream decoded;
            juce::Base64::convertFromBase64 (decoded, xml->getStringAttribute ("segments"));
            auto block = decoded.getMemoryBlock();

            constexpr std::int64_t nearLimit = 9000000000000000000LL;   // 9.0e18 < 2^63 ≈ 9.22e18；× 2 必然越界
            size_t at = 0;

            for (int i = 0; i < segmentCount; ++i)
            {
                const auto layout = layoutOfEntry (block, at);
                std::int64_t start = 0, end = 0;
                std::memcpy (&start, (const char*) block.getData() + layout.start, sizeof (start));
                std::memcpy (&end, (const char*) block.getData() + layout.end, sizeof (end));
                const std::int64_t newStart = nearLimit + (std::int64_t) i * 100000, newEnd = newStart + (end - start);   // 段长不变、首尾不交叠
                block.copyFrom (&newStart, (int) layout.start, sizeof (newStart));
                block.copyFrom (&newEnd, (int) layout.end, sizeof (newEnd));
                at = layout.next;
            }

            xml->setAttribute ("segments", juce::Base64::toBase64 (block.getData(), block.getSize()));

            LiveProfile sameRate;
            setUp (sameRate);
            expect (sameRate.restoreStateXml (*xml) && sameRate.getDiagnostics().commits == segmentCount,
                    "at the same rate nothing is converted, so the near-limit timestamps are kept");

            LiveProfile converted;
            converted.prepare (2.0 * rate);
            converted.setConstants (LiveConstants {}, MainSectionModel::fourOnFloor);
            const auto restored = converted.restoreStateXml (*xml);
            const auto d = converted.getDiagnostics();
            logMessage ("  near-int64 timestamps restored into a " + juce::String (2.0 * rate / 1000.0) + " kHz instance: returned "
                        + juce::String ((int) restored) + ", commits " + juce::String ((int) d.commits));
            expect (! restored && d.commits == 0 && d.windowSegments == 0,
                    "a rate conversion that would overflow the timestamps rejects the whole state and leaves the instance cold");
        }

        // 发布端兜底（P1F，LiveProfile.cpp guardPublishedResult）：不经读档。实时入口只查有限与非负（分析器交来的量有物理上限），由它交来
        // 有限但荒谬的段，让累计、比值与 double → float 真的算出非有限值——发布出去的每个读数仍是有限值，出事的几项按「没有」发布：
        //   一、局部：第 15 段 Tame 和 1e300（比值转 float 溢出）、第 3 段交叉功率 −1e300（S/M 的比值溢出：段 3 与低频组）、两段 10 Hz 候选
        //       的本该能量 1e308（相加溢出：混合损失）。出事的逐段数组按「这一段没有统计」、特征无效；其余读数与没出事的对照实例逐位相同。
        //   二、整份：遗忘全关、两段有效时长 1e308（主段质量 M 相加溢出；这两段的段响度与低频占比也写成 1e30，格号换算不出未定义行为）
        //       → 整份同冷启动的空画像；reset 之后一切照常。
        // 这两种输入都要实时入口收下荒谬的量才到得了兜底；以后入口若也按读档的物理上界拒收，这里改成直接构造（兜底本身不变）。
        void testPublishGuard()
        {
            beginTest ("publication guard: absurd but finite live segments never publish inf / NaN; the affected readings are published as missing");

            LiveProfile live, reference;
            setUp (live);
            setUp (reference);

            for (int i = 0; i < 12; ++i)   // 12 段：试算满 10 s，混合损失有效
            {
                auto segment = makeSegment (dropSpec(), i);
                feed (reference, segment);

                if (i >= 10)
                    segment.liveBlendIntended[0] = 1.0e308;

                if (i == 11)
                {
                    segment.tameSum[15] = 1.0e300;
                    segment.stereoCrossSum[3] = -1.0e300;
                }

                feed (live, segment);
            }

            const auto& p = live.getProfile();
            const auto& q = reference.getProfile();
            const auto bad = firstBadReading (live);
            expect (bad.isEmpty(), "every published reading is finite " + bad);
            expect (sameFloat (p.tameEvidenceDb[15], 0.0f) && sameFloat (p.recoverEvidenceDb[15], 0.0f) && sameFloat (p.bandConfidence[15], 0.0f),
                    "band 15 (Tame ratio overflowed float) is published as having no evidence");
            expect (sameFloat (p.bandSideToMidDb[3], 0.0f) && sameFloat (p.bandCorrelation[3], 0.0f) && sameFloat (p.bandStereoConfidence[3], 0.0f)
                        && ! p.lowSideToMidDb.valid,
                    "band 3 S/M (ratio overflowed) has no stereo statistics; the low-band S/M feature is invalid");
            expect (! p.lowCutBlendLossDb[0].valid && p.lowCutBlendLossDb[1].valid, "the 10 Hz blend loss (energy sum overflowed) is invalid; 20 Hz is not");

            // 没出事的读数与对照实例逐位相同（兜底只动出事的那几项）。
            auto untouched = sameFeature (p.integratedLufs, q.integratedLufs) && sameFeature (p.loudnessRangeLu, q.loudnessRangeLu)
                          && sameFeature (p.plrDb, q.plrDb) && sameFeature (p.truePeakDbtp, q.truePeakDbtp)
                          && sameFeature (p.spectralSlopeDbPerOct, q.spectralSlopeDbPerOct) && sameFeature (p.lowShare, q.lowShare)
                          && sameFeature (p.highShare, q.highShare) && sameFeature (p.lowBandRangeDb, q.lowBandRangeDb)
                          && sameFeature (p.onsetDensityPerSecond, q.onsetDensityPerSecond) && sameFeature (p.onsetStrengthDb, q.onsetStrengthDb)
                          && sameFeature (p.subShareDb, q.subShareDb) && sameFeature (p.highSideToMidDb, q.highSideToMidDb)
                          && sameFeature (p.highCorrelation, q.highCorrelation) && sameFeature (p.lowCutPeakCostDb, q.lowCutPeakCostDb)
                          && sameFeature (p.widthPeakCostDb, q.widthPeakCostDb) && sameFeature (p.lowCutBlendLossDb[1], q.lowCutBlendLossDb[1])
                          && sameFloats (p.bandLongTermDb, q.bandLongTermDb) && sameDouble (p.effectiveSeconds, q.effectiveSeconds)
                          && sameFloat (p.representativeness, q.representativeness);

            for (size_t c = 0; c < p.monoPeakCostDb.size(); ++c)
                untouched = untouched && sameFeature (p.monoPeakCostDb[c], q.monoPeakCostDb[c]);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                if (b != 15)
                    untouched = untouched && sameFloat (p.tameEvidenceDb[b], q.tameEvidenceDb[b]) && sameFloat (p.recoverEvidenceDb[b], q.recoverEvidenceDb[b])
                             && sameFloat (p.bandConfidence[b], q.bandConfidence[b]);

                if (b != 3)
                    untouched = untouched && sameFloat (p.bandSideToMidDb[b], q.bandSideToMidDb[b]) && sameFloat (p.bandCorrelation[b], q.bandCorrelation[b])
                             && sameFloat (p.bandStereoConfidence[b], q.bandStereoConfidence[b]);
            }

            expect (untouched, "every other reading equals the reference instance bit for bit");

            // 二、整份不可信：主段质量 M 溢出 → 空画像（同冷启动），每一份发布都是有限值；reset 之后与全新实例逐位相同。
            LiveProfile overflow;
            setUp (overflow, noForgetting());
            Recorder recorder (overflow, 16);
            overflow.setPublicationListener (&recorder);

            for (int i = 0; i < 6; ++i)
            {
                auto segment = makeSegment (dropSpec(), i);

                if (i >= 4)
                {
                    segment.effectiveSeconds = 1.0e308;
                    segment.loudnessLufs = 1.0e30f;   // 荒谬的段响度 / 低频占比：格号换算先钳再转整数（clampedBin），不出未定义行为
                    segment.lowShare = 1.0e30f;
                }

                feed (overflow, segment);
                const auto reading = firstBadReading (overflow, false);   // 计数器 activeSeconds 照样相加溢出：兜底只管画像
                expect (reading.isEmpty(), "publication " + juce::String (i) + " is finite " + reading);
            }

            LiveProfile cold;
            setUp (cold, noForgetting());
            expect (sameProfile (overflow.getProfile(), cold.getProfile()) && overflow.getDiagnostics().mainMassSeconds == 0.0
                        && overflow.getDiagnostics().confidence == 0.0f,
                    "once M overflows the published profile is the empty (cold) one");
            expect (! recorder.overflow && recorder.publications.size() == 6 && std::isfinite (recorder.publications.back().record.mainMassSeconds)
                        && ! recorder.publications.back().record.mainLevelValid,
                    "the commit record carries the guarded values too");

            overflow.reset();
            LiveProfile fresh;
            setUp (fresh, noForgetting());

            for (int i = 0; i < 6; ++i)
            {
                feed (overflow, makeSegment (dropSpec(), 10 + i));
                feed (fresh, makeSegment (dropSpec(), 10 + i));
            }

            expect (sameProfile (overflow.getProfile(), fresh.getProfile()) && sameDiagnostics (overflow.getDiagnostics(), fresh.getDiagnostics(), false),
                    "after reset nothing of the overflow is left");
        }

        // f. 队列满、已发布的窗口也满（512 段）：一直不调 advanceTo、连交 80 段，segmentFinalized 就地发布腾格（LiveProfile.cpp makeRoomInQueue）——
        // 新段只盖比已发布窗口更老的格。每一份发布（就地发布的也算：提交记录、画像、诊断）与「逐段喂」的对照实例逐位相同；连交完的那一刻
        // 已发布窗口的逐段判断与对照实例在同样段数时相同。（VAL1 变异：容量判断改成 ≥ queueCapacity + 1，新段就盖掉已发布窗口最老的一格；
        // 原有的「队列满」只发生在窗口不到 512 时，杀不掉它。）
        void testFullQueueWithFullWindow()
        {
            beginTest ("f. a full queue with a full 512-segment window: forced publications leave the published window intact (== fed one by one)");

            const auto segments = mixedSequence (512 + 8 + 80, 4242);
            LiveProfile a, b;
            setUp (a);
            setUp (b);
            Recorder recordA (a, segments.size() + 8), recordB (b, segments.size() + 8);
            a.setPublicationListener (&recordA);
            b.setPublicationListener (&recordB);

            for (int i = 0; i < 520; ++i)
                feed (a, segments[(size_t) i]);

            expect (a.getDiagnostics().windowSegments == LiveProfile::windowCapacity, "window full before the burst");

            for (int i = 520; i < 600; ++i)
                a.segmentFinalized (segments[(size_t) i]);   // 不调 advanceTo：队列装满、就地发布

            const auto d = a.getDiagnostics();
            expect (d.forcedPublications >= 48 && d.pendingSegments == LiveProfile::queueCapacity,
                    "the full queue forced " + juce::String ((int) d.forcedPublications) + " publications");

            const auto commits = (int) d.commits;

            for (int i = 0; i < commits; ++i)
                feed (b, segments[(size_t) i]);

            expect (sameJudgementLists (judgementsOf (a), judgementsOf (b)) && sameProfile (a.getProfile(), b.getProfile()),
                    "right after the burst the published window and profile equal an instance fed the same " + juce::String (commits) + " segments");

            a.flush();

            for (int i = commits; i < (int) segments.size(); ++i)
                feed (b, segments[(size_t) i]);

            expect (! recordA.overflow && ! recordB.overflow && recordA.publications.size() == segments.size()
                        && samePublications (recordA.publications, recordB.publications),
                    "every publication (the forced ones included) is bit-identical to feeding one by one");
            expect (sameJudgementLists (judgementsOf (a), judgementsOf (b)), "and so is the final window");
        }

        //==============================================================================
        // f. 同一段序列结果逐位相同：另一个实例一次交完；同一个实例交到一半 reset、再从头交一遍；中间穿插读数不改变结果。提交序号跨 reset 单调。
        void testDeterminismAndReset()
        {
            beginTest ("f. the same segment sequence gives bit-identical results: another instance, after a reset, with reads in between");

            const auto segments = mixedSequence (60);
            LiveProfile a;
            setUp (a);

            for (const auto& segment : segments)
                feed (a, segment);

            LiveProfile b;
            setUp (b);

            for (size_t i = 0; i < 30; ++i)
                feed (b, segments[i]);

            b.segmentFinalized (segments[30]);   // 还没发布的一段也随 reset 丢掉
            b.reset();
            LiveProfile cold;
            setUp (cold);
            expect (b.getDiagnostics().commits == 0 && b.getDiagnostics().pendingSegments == 0 && sameProfile (b.getProfile(), cold.getProfile()),
                    "reset is a cold start");

            std::uint64_t cursor = 0;
            LiveCommitRecord records[LiveProfile::commitCapacity];
            LiveSegmentJudgement judgements[8];

            for (const auto& segment : segments)
            {
                feed (b, segment);
                (void) b.getProfile();
                (void) b.getDiagnostics();
                (void) b.readCommits (cursor, records, 4);
                (void) b.readJudgements (judgements, 8);
            }

            expect (sameProfile (a.getProfile(), b.getProfile()) && sameDiagnostics (a.getDiagnostics(), b.getDiagnostics()),
                    "bit-identical to a fresh instance fed the same segments");
            expect (lastRecord (b).sequence == 89, "commit sequence numbers keep counting across reset");
        }

        // f. 结果与 advanceTo 的调用次数、时刻无关（SegmentSink::advanceTo 的约定）：同一串段，advanceTo（1）正好在各段的发布时刻、（2）每 7 个
        // 样本、（3）每次都调两遍、（4）每 10 段才一次跳过好几份、（5）一直不调直到最后 flush（队列满时 segmentFinalized 就地发布）——每一份
        // 发布（提交记录全部字段、画像、诊断）逐位相同、份数相同。
        void testCallPatterns()
        {
            beginTest ("f. publications are bit-identical whatever advanceTo is called with: at due times, every 7 samples, twice, in jumps, never (flush)");

            const auto segments = mixedSequence (48);

            const auto run = [&segments] (int pattern, std::vector<Publication>& out)
            {
                LiveProfile live;
                setUp (live);
                Recorder recorder (live, segments.size() + 4);
                live.setPublicationListener (&recorder);
                std::int64_t time = 0;

                for (size_t i = 0; i < segments.size(); ++i)
                {
                    const auto& segment = segments[i];

                    // 分析器在段尾之后才交段（这里在段尾 + 0.2 s）：先把时间推到那里，再交。
                    const auto arrival = segment.end + (std::int64_t) (0.2 * rate);

                    if (pattern == 1)
                    {
                        for (; time + 7 <= arrival; time += 7)
                            live.advanceTo (time + 7);
                    }
                    else if (pattern == 2)
                    {
                        live.advanceTo (arrival);
                        live.advanceTo (arrival);
                    }
                    else if (pattern == 3)
                    {
                        if (i % 10 == 9)
                            live.advanceTo (arrival);
                    }
                    else if (pattern == 0)
                    {
                        live.advanceTo (arrival);
                    }

                    live.segmentFinalized (segment);

                    if (pattern == 0)
                        live.advanceTo (live.getNextPublicationTime());
                }

                if (pattern == 1)
                    for (const auto end = segments.back().end + (std::int64_t) (2.0 * rate); time < end; time += 7)
                        live.advanceTo (time + 7);

                live.flush();
                out = recorder.publications;
                return ! recorder.overflow;
            };

            std::vector<Publication> reference;
            expect (run (0, reference) && reference.size() == segments.size(), "pattern 0 (at due times): one publication per segment");
            const char* const names[] = { "", "every 7 samples", "every call twice", "jumps every 10 segments", "never (flush at the end)" };

            for (int pattern = 1; pattern <= 4; ++pattern)
            {
                std::vector<Publication> other;
                const auto ok = run (pattern, other);
                expect (ok && samePublications (reference, other), juce::String ("advanceTo ") + names[pattern] + ": publications bit-identical");
            }
        }

        // f. 整条串起来：分析器实时模式 → 实时段落账 → SegmentSink → LiveProfile，宿主块长 4096 / 512 / 480 / 32 / 7 下每一份发布（提交记录、
        // 画像、诊断）与最终状态（经 createStateXml）逐位相同。时间轴只由分析器报（交齐位置），测试台不报。
        void testAnalyzerToProfileBlockSizes()
        {
            beginTest ("analyzer live mode -> live ledger -> sink -> LiveProfile: every publication is bit-identical for host blocks 4096 / 512 / 480 / 32 / 7");

            const auto track = makeIntroDrop (8, 8, 1.0);
            juce::String referenceState;
            std::vector<Publication> referencePublications;

            for (const auto blockSize : { 4096, 512, 480, 32, 7 })
            {
                AnalyzerRig rig (4096);
                LiveProfile live;
                setUp (live);
                Recorder recorder (live, 64);
                live.setPublicationListener (&recorder);
                rig.analyzer.setGenre (glaze::Genre::deepHouse);
                rig.analyzer.setSegmentSink (&live);
                rig.analyzer.setLiveMode (true);
                rig.feed (track, blockSize);
                live.flush();

                const auto state = live.createStateXml()->toString();

                if (referenceState.isEmpty())
                {
                    referenceState = state;
                    referencePublications = recorder.publications;
                    expect (referencePublications.size() >= 14, "the run published the bars: " + juce::String ((int) referencePublications.size()));
                    expect (live.getProfile().integratedLufs.valid, "the live profile has an I");

                    // P1F：分析器真交出来的段过得了读档的物理上界（LiveProfile.cpp 文件头第六节「量级」）——同采样率读回逐位相同，
                    // 换到 44.1 k / 96 k 也读得回（分析器以后改了记账口径、越出这些上界时这里先报出来）。
                    const auto parsed = juce::parseXML (state);

                    for (const auto restoreRate : { rate, 44100.0, 96000.0 })
                    {
                        LiveProfile copy;
                        copy.prepare (restoreRate);
                        copy.setConstants (LiveConstants {}, MainSectionModel::fourOnFloor);
                        const auto restored = parsed != nullptr && copy.restoreStateXml (*parsed);
                        expect (restored && (restoreRate != rate || sameProfile (copy.getProfile(), live.getProfile())),
                                "the analyzer's state restores at " + juce::String (restoreRate) + " Hz"
                                    + (restoreRate == rate ? " bit-identical" : ""));
                    }
                }
                else
                {
                    expect (samePublications (recorder.publications, referencePublications),
                            "every publication bit-identical for block " + juce::String (blockSize));
                    expect (state == referenceState, "final live profile state bit-identical for block " + juce::String (blockSize));
                }
            }
        }

        // 窗口外的老段直接丢弃（LiveProfile.cpp 文件头第五节）：事件关掉时画像只取决于窗口——600 段之后的画像与「只交最后 512 段」的
        // 全新实例逐位相同；诊断报窗口 512、丢掉 88 段、最老一段的份额。
        void testWindowEviction()
        {
            beginTest ("old segments beyond the 512-segment window are dropped; with events off the profile depends only on the window");

            LiveConstants c;
            c.noMainSeconds = infinity;
            c.newProgramLu = std::numeric_limits<float>::infinity();
            const auto segments = mixedSequence (600, 300);
            LiveProfile all;
            setUp (all, c);

            for (const auto& segment : segments)
                feed (all, segment);

            LiveProfile last;
            setUp (last, c);

            for (size_t i = segments.size() - (size_t) LiveProfile::windowCapacity; i < segments.size(); ++i)
                feed (last, segments[i]);

            const auto d = all.getDiagnostics();
            logMessage ("  window " + juce::String (d.windowSegments) + ", dropped " + juce::String ((int) d.droppedSegments) + ", oldest share of M "
                        + juce::String (d.oldestMainShare, 8) + ", of the references " + juce::String (d.oldestReferenceShare, 6));
            expect (d.windowSegments == LiveProfile::windowCapacity && d.droppedSegments == 88, "window 512, 88 dropped");
            expect (sameProfile (all.getProfile(), last.getProfile()) && sameDiagnostics (all.getDiagnostics(), last.getDiagnostics(), false),
                    "the profile equals a fresh instance fed only the window's 512 segments");
            expect (d.oldestMainShare >= 0.0f && d.oldestMainShare < 1.0e-3f && d.oldestReferenceShare < 0.01f, "the oldest segment's shares are small");
        }

        // f. 音频线程接口不经 operator new 分配（TestHelpers.h allocationCountOnThisThread）：入队、advanceTo（分片、发布、补完欠的片）、
        // 队列满时的就地发布、flush、读数、提交记录、判断、reset、钉死的主段度、监听者回调。
        void testNoAllocation()
        {
            beginTest ("f. segmentFinalized / advanceTo / flush / reads / reset do not allocate (slices, publications, catch-up, a full queue)");

            LiveConstants c;
            c.noMainSeconds = 5.0;                // 时间衰减很快就开始
            c.newProgramSeconds = 4.0;            // 换歌很快就触发
            LiveProfile live;
            setUp (live, c);
            Recorder recorder (live, 4096);
            live.setPublicationListener (&recorder);

            // 每 8 段：2 段 breakdown、3 段 drop、3 段「响 9 LU、没有低音」（换歌软遗忘真走到）。
            auto louder = dropSpec();
            louder.loudnessLufs = 0.97;
            louder.lowShare = 0.1;
            std::vector<Segment> segments;

            for (int i = 0; i < 640; ++i)
                segments.push_back (makeSegment (i % 8 < 2 ? breakdownSpec() : i % 8 < 5 ? dropSpec() : louder, i));

            LiveCommitRecord records[LiveProfile::commitCapacity];
            LiveSegmentJudgement judgements[16];
            std::uint64_t cursor = 0;
            float sink = 0.0f;
            auto sawNewProgram = false;
            std::int64_t time = 0;
            const auto before = allocationCountOnThisThread();

            for (size_t i = 0; i < segments.size(); ++i)
            {
                if (i % 7 == 3)
                    live.commitWithMainness (segments[i], 0.4f);
                else
                    live.segmentFinalized (segments[i]);

                // 前 200 段：每 512 样本一次（分片）；200 ～ 260：不调（队列满：就地发布）；之后一次跳到发布时刻（补完欠的片）。
                if (i < 200)
                    for (; time < segments[i].end + 24000; time += 512)
                        live.advanceTo (time);
                else if (i >= 260)
                    live.advanceTo (live.getNextPublicationTime());

                sink += live.mainnessFor (true, -9.0f, true, 0.5f) + live.getProfile().integratedLufs.value + live.getDiagnostics().confidence;
                const auto read = live.readCommits (cursor, records, LiveProfile::commitCapacity);
                sink += (float) live.readJudgements (judgements, 16);

                for (int k = 0; k < std::abs (read); ++k)
                    sawNewProgram = sawNewProgram || records[k].newProgram;

                if (i == 100)
                    live.reset();
            }

            live.flush();
            const auto after = allocationCountOnThisThread();
            expect (after == before, "no allocation: " + juce::String ((juce::int64) (after - before)));
            expect (sawNewProgram, "the run went through the new-program soft forget");
            expect (! recorder.overflow && std::isfinite (sink));
        }

        //==============================================================================
        // 提交记录的环形缓冲（同 SegmentLedger::readEvents）：容量 64；落后超过容量跳到能读到的最早一条、返回负的条数；cursor 超前钳回。
        void testCommitRing()
        {
            beginTest ("commit records: a 64-entry ring read by cursor; a lagging reader skips to the oldest and gets a negative count");

            LiveProfile live;
            setUp (live);

            for (int i = 0; i < 100; ++i)
                feed (live, makeSegment (dropSpec(), i));

            LiveCommitRecord records[LiveProfile::commitCapacity];
            std::uint64_t cursor = 0;
            const auto first = live.readCommits (cursor, records, 10);
            expect (first == -10 && cursor == 46 && records[0].sequence == 36, "lagging: skipped to sequence 36, -10 returned");
            const auto second = live.readCommits (cursor, records, LiveProfile::commitCapacity);
            expect (second == 54 && cursor == 100 && records[53].sequence == 99, "then the rest in order");
            expect (live.readCommits (cursor, records, 8) == 0, "nothing new");

            std::uint64_t ahead = 500;
            expect (live.readCommits (ahead, records, 8) == 0 && ahead == 100, "a cursor ahead of the total is clamped");

            const auto segment = makeSegment (dropSpec(), 100);
            live.segmentFinalized (segment);
            expect (live.getNextPublicationTime() == segment.end + 24000 && live.getDiagnostics().pendingSegments == 1,
                    "a new segment is due at its end + 0.5 s");
            live.advanceTo (segment.end + 23999);
            expect (live.getDiagnostics().commits == 100, "not published one sample before its due time");
            live.advanceTo (segment.end + 24000);
            const auto record = lastRecord (live);
            expect (record.sequence == 100 && record.start == 100 * barSamples && record.end == 101 * barSamples
                        && record.publishTime == segment.end + 24000 && record.mode == glaze::SegmentMode::bars
                        && std::abs (record.nominalSeconds - (double) barSamples / rate) < 1.0e-12 && record.mainLevelValid && record.referenceValid,
                    "published exactly at its due time; the record carries the segment, its judgement and the profile");
            expect (live.getDiagnostics().publishTime == segment.end + 24000 && live.getNextPublicationTime() == LiveProfile::noPublication);
        }

        // 坏段（累计量里有 NaN）：只记一条 m = 0、有效时长 0 的提交，画像不变。
        void testBadSegment()
        {
            beginTest ("a segment with non-finite data is recorded with m = 0 and leaves the profile unchanged");

            LiveProfile live;
            setUp (live);

            for (int i = 0; i < 8; ++i)
                feed (live, makeSegment (dropSpec(), i));

            const auto before = live.getProfile();
            auto bad = makeSegment (dropSpec(), 8);
            bad.powerSum[3] = std::numeric_limits<double>::quiet_NaN();
            feed (live, bad);
            const auto record = lastRecord (live);
            expect (sameFloat (record.mainness, 0.0f) && record.effectiveSeconds == 0.0, "recorded with m = 0");
            expect (sameProfile (before, live.getProfile()), "the profile is untouched");

            bad = makeSegment (dropSpec(), 9);
            bad.rangeCount = 1000;   // 越出定长数组
            feed (live, bad);
            expect (sameProfile (before, live.getProfile()), "an out-of-range count is rejected too");
        }

        //==============================================================================
        // g. 耗时（写进 REPORT-A2）：每段入队（segmentFinalized）、每片（advanceTo 每次只跨一个网格点：至多一片）、一遍同步重算（setConstants）；
        // 4/4 型 2000 段（窗口挤满之后）与响度型 700 段（多一个全部段权重的 exp）。护栏只防百倍退化（例如每片重扫 0.58 MB 的试算直方图），
        // 不当性能门槛：均值 ≤ 200 µs、最大 ≤ 20 ms（含调度抖动）；每一遍都在发布时刻之前按网格做完（不用补完）。
        void testTiming()
        {
            beginTest ("g. per-enqueue and per-slice time (mean / max, us), a synchronous full pass; wide guards against a 100x regression");

            timeModel (MainSectionModel::fourOnFloor, 2000, true);
            timeModel (MainSectionModel::loudness, 700, false);
        }

        void timeModel (MainSectionModel model, int count, bool measurePasses)
        {
            LiveProfile live;
            setUp (live, LiveConstants {}, model);
            std::vector<Segment> segments;

            for (int i = 0; i < count; ++i)
            {
                const auto phase = (i / 16) % 4;
                auto spec = phase == 0 ? introSpec() : phase == 2 ? breakdownSpec() : dropSpec();
                spec.seed = (std::uint32_t) (100 + i);
                segments.push_back (makeSegment (spec, i));
            }

            using Clock = std::chrono::steady_clock;   // 纳秒级（juce 的高精度计时在 Linux 上只到微秒）
            const auto microsecondsSince = [] (Clock::time_point start) { return std::chrono::duration<double, std::micro> (Clock::now() - start).count(); };
            const auto grid = (std::int64_t) std::llround (LiveProfile::sliceGridSeconds * rate);
            std::vector<double> enqueueTimes, sliceTimes;
            enqueueTimes.reserve (segments.size());
            sliceTimes.reserve (segments.size() * 400);
            std::vector<double> passCalls;
            passCalls.reserve (256);
            std::int64_t time = 0;
            int maxSlicesPerPass = 0;

            for (const auto& segment : segments)
            {
                // 段尾 + 0.2 s 交来（分析器的定稿延迟），之后每个网格点调一次 advanceTo：每次至多一片。
                const auto arrival = segment.end + (std::int64_t) (0.2 * rate);

                for (; time + grid <= arrival; time += grid)
                    live.advanceTo (time + grid);

                auto start = Clock::now();
                live.segmentFinalized (segment);
                enqueueTimes.push_back (microsecondsSince (start));
                const auto due = live.getNextPublicationTime();

                passCalls.clear();

                for (; time + grid < due; time += grid)
                {
                    start = Clock::now();
                    live.advanceTo (time + grid);
                    passCalls.push_back (microsecondsSince (start));
                }

                // 发布时刻那一次：这一遍要是还没做完，就在这里补完（catchUps 计数；不该发生：一遍的片数少于 0.3 s 的网格点）。
                live.advanceTo (due);
                time = due;

                // 交段之后每个网格点正好一片，直到这一遍做完：前 lastPassSlices 次调用就是这一遍的片（之后的网格点是空的，不记）。
                const auto used = live.getDiagnostics().lastPassSlices;
                maxSlicesPerPass = juce::jmax (maxSlicesPerPass, used);

                for (size_t k = 0; k < passCalls.size() && k < (size_t) used; ++k)
                    sliceTimes.push_back (passCalls[k]);
            }

            const auto lateFull = live.getDiagnostics().catchUps;

            const auto stats = [] (std::vector<double> times)
            {
                std::sort (times.begin(), times.end());
                double total = 0.0;

                for (auto x : times)
                    total += x;

                const auto at = [&times] (double share) { return times[(size_t) std::floor (share * (double) (times.size() - 1))]; };
                return std::array<double, 5> { total / (double) times.size(), at (0.5), at (0.99), at (0.999), times.back() };
            };

            const auto enqueue = stats (enqueueTimes);
            const auto slices = stats (sliceTimes);
            const juce::String label (model == MainSectionModel::fourOnFloor ? "4/4" : "loudness");
            logMessage ("  [" + label + ", " + juce::String (count) + " bars] enqueue (segmentFinalized): mean " + juce::String (enqueue[0], 2)
                        + " us, median " + juce::String (enqueue[1], 2) + ", P99 " + juce::String (enqueue[2], 2) + ", P99.9 " + juce::String (enqueue[3], 2)
                        + ", max " + juce::String (enqueue[4], 1));
            logMessage ("  [" + label + "] slice (one per grid point while a pass is running, " + juce::String ((int) sliceTimes.size())
                        + " calls): mean " + juce::String (slices[0], 2) + " us, median " + juce::String (slices[1], 2) + ", P99 " + juce::String (slices[2], 2)
                        + ", P99.9 " + juce::String (slices[3], 2) + ", max " + juce::String (slices[4], 1));
            logMessage ("  [" + label + "] slices used by one pass: up to " + juce::String (maxSlicesPerPass) + " (of "
                        + juce::String ((int) ((0.5 - 0.2) / LiveProfile::sliceGridSeconds)) + " grid points before the due time); passes finished late: "
                        + juce::String ((juce::int64) lateFull));

            if (measurePasses)
            {
                // 一遍同步重算（setConstants 重算已发布的那一份）的耗时随窗口段数：取 5 次里最短的（标定工作量估计用：斜率 = 每段，截距 = 固定）。
                juce::String line ("  synchronous full pass (setConstants) by window size:");

                for (const auto size : { 1, 64, 256, 512 })
                {
                    LiveProfile sized;
                    setUp (sized, LiveConstants {}, model);

                    for (int i = 0; i < size; ++i)
                        feed (sized, segments[(size_t) i]);

                    auto best = 1.0e30;

                    for (int repeat = 0; repeat < 5; ++repeat)
                    {
                        const auto passStart = Clock::now();
                        sized.setConstants (LiveConstants {}, model);
                        best = juce::jmin (best, microsecondsSince (passStart));
                    }

                    line << " " << size << " segments " << juce::String (best, 1) << " us;";
                }

                logMessage (line);
                logMessage ("  state XML after the " + juce::String (count) + " bars: " + juce::String (live.createStateXml()->toString().getNumBytesAsUTF8())
                            + " bytes");

                // 每段的耗时按种类（标定 LiveProfile.cpp 的工作量估计）：窗口 512 段 drop，主段度钉成 1（全部主段）或 0（全部非主段），两种模型。
                juce::String kinds ("  per-segment cost of a 512-segment pass (us / segment):");

                for (const auto kindModel : { MainSectionModel::fourOnFloor, MainSectionModel::loudness })
                    for (const auto pinned : { 0.0f, 1.0f })
                    {
                        LiveProfile sized;
                        setUp (sized, LiveConstants {}, kindModel);

                        for (int i = 0; i < 512; ++i)
                            feedPinned (sized, makeSegment (dropSpec(), i), pinned);

                        auto best = 1.0e30;

                        for (int repeat = 0; repeat < 5; ++repeat)
                        {
                            const auto passStart = Clock::now();
                            sized.setConstants (LiveConstants {}, kindModel);
                            best = juce::jmin (best, microsecondsSince (passStart));
                        }

                        kinds << " " << (kindModel == MainSectionModel::fourOnFloor ? "4/4" : "loudness") << (pinned > 0.5f ? " main " : " non-main ")
                              << juce::String (best / 512.0, 4) << ";";
                    }

                logMessage (kinds);
            }

            expect (lateFull == 0, label + ": every pass finished on the slice grid before its due time");
            expect (enqueue[0] <= 200.0 && slices[0] <= 200.0, label + ": means within the 200 us guard");
            expect (enqueue[4] <= 20000.0 && slices[4] <= 20000.0, label + ": maxima within the 20 ms guard");
        }
    };

    static LiveProfileTests liveProfileTests;
}
