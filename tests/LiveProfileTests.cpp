/*
  ==============================================================================

    LiveProfileTests.cpp
    T-048 阶段 1：实时画像 LiveProfile（LiveProfile.h；LiveProfile.cpp 文件头；设计规格-实时GLAZE-主段与增益 第 1～3、5 节；BRIEF 第 3 节）。

    a. 等价性：分析器 House 档处理一段前奏 + drop，段落账存下的段（试算拷进 liveTrial* / liveBlend*）逐段 commitWithMainness(full ? 1 : 0)
       交给遗忘关掉的 LiveProfile，画像与分析器的 House 候选画像逐项比；容差逐项写在用例里（多数逐位）。
    b. 遗忘不变性：主段之后交一串 m = 0 的 breakdown，主段画像、M、c 逐位不变；再交新主段，画像向它移动。
    c. m 的公式（两个因子、斜坡端点、冷启动、被动型 / 响度型）；mainnessFor 与 segmentFinalized 同一口径。
    d. 长时间无主段的时间衰减、换歌软遗忘（触发与不触发的边界）。
    e. 状态 XML 往返逐位相同（含重规范过、缩放因子 ≠ 1 的状态，与经过 XML 文本的往返）；坏数据冷启动并返回 false。
    f. 同一段序列：另一个实例、reset 后重交，结果逐位相同；音频线程接口不分配。
    g. 每次并入的耗时（均值 / 最大，µs，写进 REPORT-A）；只设防百倍退化的宽护栏。
    另：提交记录环形缓冲、坏段不进累计、主段模型切换与格式字段。

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
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
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

        // 画像逐位相同（逐字段）；withRange = false 时不比 LRA（全部段的统计，breakdown 里照常变）。
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

        bool sameDiagnostics (const LiveDiagnostics& a, const LiveDiagnostics& b)
        {
            return a.commits == b.commits && a.referenceValid == b.referenceValid && sameFloat (a.referenceLufs, b.referenceLufs)
                && a.lowReferenceValid == b.lowReferenceValid && sameFloat (a.lowReference, b.lowReference)
                && sameDouble (a.mainMassSeconds, b.mainMassSeconds) && sameFloat (a.confidence, b.confidence)
                && sameDouble (a.activeSeconds, b.activeSeconds) && sameDouble (a.secondsSinceMain, b.secondsSinceMain)
                && a.timeDecayActive == b.timeDecayActive && a.newProgramCount == b.newProgramCount;
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

        LiveCommitRecord lastRecord (const LiveProfile& live)
        {
            LiveCommitRecord records[LiveProfile::commitCapacity];
            std::uint64_t cursor = 0;
            const auto count = std::abs (live.readCommits (cursor, records, LiveProfile::commitCapacity));
            return count > 0 ? records[count - 1] : LiveCommitRecord {};
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

        //==============================================================================
        // 分析器测试台（与链里的顺序相同：IN 表 → processInput → Balance → processSpectrum），宿主给恒定 124 BPM 的拍点。
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

        // 存下的段拷一份、把学习期的试算拷进实时那几项（阶段 1 的分析器还不填 liveTrial*；BRIEF 等价性用例的做法）。
        Segment withLiveTrials (const Segment& stored)
        {
            auto segment = stored;
            segment.liveTrialBlocks = stored.trialBlocks;
            segment.liveTrialTop = stored.trialTop;
            segment.liveBlendIntended = stored.blendIntended;
            segment.liveBlendOutput = stored.blendOutput;
            return segment;
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
            testMemoryUnits();
            testNoMainDecay();
            testNewProgram();
            testModelsAndFormat();
            testStateRoundTrip();
            testBadState();
            testDeterminismAndReset();
            testAnalyzerToProfileBlockSizes();
            testNoAllocation();
            testCommitRing();
            testBadSegment();
            testCommitTiming();
        }

    private:
        //==============================================================================
        // 整条串起来（主进程 2026-10-02 加，代理 A 的跨文件请求 4）：分析器实时模式 → 实时段落账 → SegmentSink → LiveProfile，宿主块长
        // 4096 / 512 / 480 / 32 / 7 下实时画像的状态（全部累计，经 createStateXml）逐位相同、并入段数相同。用例 f 只证明「段序列相同 ⟹
        // 结果逐位相同」，分析器那边证明了「段序列与块长无关」（MusicAnalyzerLive 用例 c）；这一条把两头接起来核对。
        void testAnalyzerToProfileBlockSizes()
        {
            beginTest ("analyzer live mode -> live ledger -> sink -> LiveProfile: the live profile is bit-identical for host blocks 4096 / 512 / 480 / 32 / 7");

            const auto track = makeIntroDrop (8, 8, 1.0);
            juce::String reference;
            std::uint64_t referenceCommits = 0;

            for (const auto blockSize : { 4096, 512, 480, 32, 7 })
            {
                AnalyzerRig rig (4096);
                LiveProfile live;
                setUp (live);
                rig.analyzer.setGenre (glaze::Genre::deepHouse);
                rig.analyzer.setSegmentSink (&live);
                rig.analyzer.setLiveMode (true);
                rig.feed (track, blockSize);

                const auto state = live.createStateXml()->toString();
                const auto commits = live.getDiagnostics().commits;

                if (reference.isEmpty())
                {
                    reference = state;
                    referenceCommits = commits;
                    expect (commits >= 14, "the run committed the bars: " + juce::String ((juce::int64) commits));
                    expect (live.getProfile().integratedLufs.valid, "the live profile has an I");
                }
                else
                {
                    expect (commits == referenceCommits, "same number of commits for block " + juce::String (blockSize));
                    expect (state == reference, "live profile state bit-identical for block " + juce::String (blockSize));
                }
            }
        }

        //==============================================================================
        // a. 等价性（LiveProfile.h「等价性（测试锚点）」）。分析器 House 档 16 小节前奏 + 16 小节 drop，学习中（试算跑着）；lockSegments = 1
        // 让 House 的代表性只看不重复素材（= 1），实时画像的 M0 取极小（c = 1）——两边的置信度都只剩各特征的观测充分度，可以逐位比。
        // 容差（其余逐位）：
        //   - I：实时画像的相对门按 0.1 LU 格中心判、门上能量按格求和（House 逐块比、逐块求和）。先断言没有门块离相对门不到 0.1 LU
        //     （否则门边那一格的门块可能归属不同，差可达 0.05 LU 量级），之后只剩求和顺序（double 末位）与 float 舍入：1e-4 LU；PLR 同；
        //   - 试算代价：每路 P99 由 0.01 dB 一格的直方图按格中心插值，每个 P99 ≤ 半格 0.005 dB，代价 ≤ 0.01 dB；
        //   - LRA：4/4 型主段加权，与 House（只用满编段）逐位；响度型用全部段，对照是测试里按 House 同一公式在全部存下的段上算的参照，逐位；
        //   - 按设计不同、单独核对的字段：effectiveSeconds（= M = 满编段有效时长之和）、formatVersion（2 / 3）、houseGate（不填）、
        //     混合损失的 confidence（houseConfidences 没给，实时画像照通用画像给）。
        void testHouseEquivalence()
        {
            beginTest ("a. equivalence: forgetting off, the ledger's stored segments with m = full give the analyzer's House profile");

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

            LiveProfile live;
            setUp (live, noForgetting());
            int fullSegments = 0, fullTrialBlocks = 0;
            auto allHeard = true;

            for (int i = 0; i < ledger.getNumStored(); ++i)
            {
                const auto& stored = ledger.getStored (i);
                live.commitWithMainness (withLiveTrials (stored), stored.full ? 1.0f : 0.0f);
                allHeard = allHeard && stored.heard;

                if (stored.full)
                {
                    ++fullSegments;
                    fullTrialBlocks += stored.trialBlocks;
                }
            }

            const auto& p = live.getProfile();
            logMessage ("  stored " + juce::String (ledger.getNumStored()) + ", full " + juce::String (fullSegments) + " (trial blocks "
                        + juce::String (fullTrialBlocks) + "); House I " + juce::String (expected.integratedLufs.value, 4) + ", live I "
                        + juce::String (p.integratedLufs.value, 4) + "; S_ref ledger " + juce::String (gate.referenceLufs, 2) + ", live "
                        + juce::String (live.getDiagnostics().referenceLufs, 2));

            // 用例的前提：drop 都是满编段；试算块在 10 s ～ 512 块之间（House 不走「不够 10 s 改用全部段」与 512 块截断）；代表性 = 1。
            expect (gate.valid && fullSegments >= 14 && fullSegments <= 17, "the full segments are the drop bars");
            expect (fullTrialBlocks >= 100 && fullTrialBlocks <= 512, "full-segment trial blocks within 10 s ... 512 blocks");
            expect (sameFloat (expected.representativeness, 1.0f) && sameFloat (p.representativeness, 1.0f), "both representativeness 1");

            // I：先确认门边那一格没有门块（满编段的门块按 House 的逐块口径求相对门）。
            {
                double total = 0.0;
                std::uint64_t count = 0;

                for (int i = 0; i < ledger.getNumStored(); ++i)
                    if (ledger.getStored (i).full)
                        for (int k = 0; k < ledger.getStored (i).gatingCount; ++k)
                        {
                            total += (double) ledger.getStored (i).gatingPower[(size_t) k];
                            ++count;
                        }

                const auto relativeGate = lufsOf (total / (double) count) - 10.0;
                auto nearest = 1.0e9;

                for (int i = 0; i < ledger.getNumStored(); ++i)
                    if (ledger.getStored (i).full)
                        for (int k = 0; k < ledger.getStored (i).gatingCount; ++k)
                            nearest = juce::jmin (nearest, std::abs (lufsOf ((double) ledger.getStored (i).gatingPower[(size_t) k]) - relativeGate));

                logMessage ("  nearest gating block to the -10 LU relative gate: " + juce::String (nearest, 3) + " LU");
                expect (nearest >= 0.1, "no gating block within one 0.1 LU bin of the relative gate");
            }

            const auto iDiff = std::abs (p.integratedLufs.value - expected.integratedLufs.value);
            expect (p.integratedLufs.valid && expected.integratedLufs.valid && iDiff <= 1.0e-4f, "I within 1e-4 LU: " + juce::String (iDiff, 7));
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

            // LRA，响度型：全部段上的参照（House 同一公式，测试里在全部存下的段上算）。换模型只改读数口径、不动累计，所以同一实例换过去就能比。
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

                // 换到响度型读一次再换回来（setConstants 只按新模型重算读数，累计不动；换回后画像与换之前逐位相同，后面的比较照用 p）。
                // 只比数值、有效与时长：置信度按响度型的 M0 算，不在这一条的范围里。
                live.setConstants (noForgetting(), MainSectionModel::loudness);
                const auto loudnessModel = live.getProfile().loudnessRangeLu;
                live.setConstants (noForgetting(), MainSectionModel::fourOnFloor);
                logMessage ("  LRA: 4/4 (full segments) " + juce::String (p.loudnessRangeLu.value, 2) + " LU, loudness model (all segments) "
                            + juce::String (loudnessModel.value, 2) + " LU");
                expect (loudnessModel.valid == reference.valid && sameFloat (loudnessModel.value, reference.value)
                            && sameDouble (loudnessModel.effectiveSeconds, reference.effectiveSeconds),
                        "loudness-model LRA over all segments bitwise (House formula)");
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

            // 试算：代价 ≤ 0.01 dB，有效性 / 有效时长 / 置信度逐位；混合损失数值逐位。
            const auto checkCost = [&] (const Feature& a, const Feature& b, const juce::String& label)
            {
                const auto diff = std::abs (a.value - b.value);
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

            for (size_t c = 0; c < p.lowCutBlendLossDb.size(); ++c)
            {
                const auto& a = p.lowCutBlendLossDb[c];
                const auto& b = expected.lowCutBlendLossDb[c];
                expect (a.valid && b.valid && sameFloat (a.value, b.value) && sameDouble (a.effectiveSeconds, b.effectiveSeconds),
                        "blend loss " + juce::String ((int) c) + " bitwise");
                expect (sameFloat (b.confidence, 0.0f) && sameFloat (a.confidence, fractionOf (a.effectiveSeconds, 10.0)),
                        "blend loss confidence: House leaves 0, live gives min(c, observation) like the general profile");
            }

            // 按设计不同的几项。
            expect (sameDouble (p.effectiveSeconds, gate.fullSeconds), "effectiveSeconds = main mass M = the full segments' effective seconds");
            expect (p.formatVersion == glaze::clayProfileFormatVersion && ! p.houseGate.valid, "format 2, no houseGate");
            expect (expected.formatVersion == glaze::houseProfileFormatVersion);

            // S_ref 与段落账同口径（已听各段、按有效时长加权的 P95 格中心）：全部段都已听时逐位相同。
            if (allHeard)
                expect (sameFloat (live.getDiagnostics().referenceLufs, gate.referenceLufs), "S_ref same as the ledger's");
        }

        //==============================================================================
        // b. 遗忘不变性（规格第 3 节「breakdown（w ≈ 0）里什么都不动」）。出厂常数、4/4 型：24 小节 drop 之后 8 小节 breakdown（比 drop
        // 轻 12 LU、低频占比 0.1：两个因子都是 0，m = 0；8 小节 15.5 s，不到无主段的 60 s；更轻又没有低音，不算「换歌」）——画像（4/4 型
        // 的 LRA 也是主段加权）、M、c 都逐位不变；之后 16 小节更响、更多低频的新 drop：I 与 lowShare 向它移动。
        void testBreakdownLeavesMainUntouched()
        {
            beginTest ("b. a breakdown (m = 0) leaves the main profile, main mass and confidence bit-identical; a new drop moves it");

            LiveProfile live;
            setUp (live);
            std::int64_t index = 0;

            for (int i = 0; i < 24; ++i)
                live.segmentFinalized (makeSegment (dropSpec(), index++));

            const auto before = live.getProfile();
            const auto diagnosticsBefore = live.getDiagnostics();
            auto allZero = true;

            for (int i = 0; i < 8; ++i)
            {
                live.segmentFinalized (makeSegment (breakdownSpec(), index++));
                allZero = allZero && sameFloat (lastRecord (live).mainness, 0.0f);
            }

            const auto after = live.getProfile();
            const auto diagnosticsAfter = live.getDiagnostics();
            expect (allZero, "the breakdown bars get m = 0");
            expect (sameProfile (before, after, true), "the main profile (the 4/4 LRA included) is bit-identical");
            expect (sameDouble (diagnosticsBefore.mainMassSeconds, diagnosticsAfter.mainMassSeconds)
                        && sameFloat (diagnosticsBefore.confidence, diagnosticsAfter.confidence),
                    "main mass and confidence bit-identical");
            expect (! diagnosticsAfter.timeDecayActive && diagnosticsAfter.newProgramCount == 0);

            auto louder = dropSpec();
            louder.loudnessLufs = -6.03;
            louder.lowShare = 0.7;
            louder.seed = 9;

            for (int i = 0; i < 16; ++i)
                live.segmentFinalized (makeSegment (louder, index++));

            const auto& moved = live.getProfile();
            logMessage ("  I " + juce::String (before.integratedLufs.value, 2) + " -> " + juce::String (moved.integratedLufs.value, 2)
                        + " LUFS, lowShare " + juce::String (before.lowShare.value, 3) + " -> " + juce::String (moved.lowShare.value, 3));
            expect (moved.integratedLufs.value > before.integratedLufs.value + 0.5f && moved.integratedLufs.value < -6.0f,
                    "I moves toward the new drop");
            expect (moved.lowShare.value > before.lowShare.value + 0.03f && moved.lowShare.value < 0.7f, "lowShare moves toward the new drop");
        }

        //==============================================================================
        // c. 主段度（规格第 2 节；LiveProfile.cpp 文件头第二节）。
        void testMainnessFormula()
        {
            beginTest ("c. mainness: two soft gates, ramp end points, cold start, passive / loudness models; mainnessFor = segmentFinalized");

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
            }

            LiveProfile live;
            setUp (live);

            for (int i = 0; i < 10; ++i)
                live.segmentFinalized (segmentReading (i, -8.03, 0.6037));

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

            // segmentFinalized 用的就是 mainnessFor 的口径：先按此刻的参考算，再并入，提交记录里的 m 逐位相同。
            {
                auto same = true;
                const std::array<std::pair<double, double>, 6> readings { { { -9.5, 0.55 }, { -11.2, 0.6 }, { -8.0, 0.36 }, { -10.9, 0.4 },
                                                                           { -14.0, 0.62 }, { -7.5, 0.7 } } };
                std::int64_t index = 10;

                for (const auto& reading : readings)
                {
                    const auto segment = segmentReading (index++, reading.first, reading.second);
                    const auto predicted = live.mainnessFor (segment.loudnessValid, segment.loudnessLufs, segment.lowShareValid, segment.lowShare);
                    live.segmentFinalized (segment);
                    const auto record = lastRecord (live);
                    same = same && sameFloat (record.mainness, predicted) && sameFloat (record.loudnessFactor * record.lowFactor, predicted)
                        && sameDouble (record.weightSeconds, (double) predicted * segment.effectiveSeconds);
                }

                expect (same, "segmentFinalized commits exactly mainnessFor's m (and w = m x effective seconds)");
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
        // W_mem 与 M0 的单位（规格第 3 节；LiveConstants 注释）：按小节分段时 = 小节数 × 段的标称长度，2 秒段时 = 秒数常数。两段主段之后
        // M = w1 × exp(−w2 ÷ W_mem) + w2（按进账遗忘：先遗忘再加），c = 1 − exp(−M ÷ M0)；representativeness = c、effectiveSeconds = M。
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

                live.segmentFinalized (first);
                const auto w1 = lastRecord (live).weightSeconds;
                live.segmentFinalized (second);
                const auto w2 = lastRecord (live).weightSeconds;
                const auto expectedMass = w1 * std::exp (-w2 / memory) + w2;
                const auto d = live.getDiagnostics();
                const auto label = juce::String (mode == glaze::SegmentMode::bars ? "bars" : "2-second segments");
                expect (w1 > 1.8 && w2 > 1.8, label + ": both segments are main");
                expect (std::abs (d.mainMassSeconds - expectedMass) <= 1.0e-12 * expectedMass, label + ": M = w1 x exp(-w2 / W_mem) + w2");
                expectWithinAbsoluteError (d.confidence, (float) (1.0 - std::exp (-expectedMass / m0)), 1.0e-6f, label + ": c = 1 - exp(-M / M0)");
                expect (sameFloat (live.getProfile().representativeness, d.confidence) && sameDouble (live.getProfile().effectiveSeconds, d.mainMassSeconds),
                        label + ": representativeness = c, effectiveSeconds = M");
            }
        }

        //==============================================================================
        // d1. 长时间无主段（规格第 3 节）：16 小节主段之后，安静小节（m = 0）的有效音频累计到 60 s 之前 M 逐位不动；越过 60 s 的那一段只按
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
                live.segmentFinalized (makeSegment (dropSpec(), index++));

            auto quiet = breakdownSpec();
            quiet.loudnessLufs = -30.03;
            auto frozenOk = true, decayOk = true, sawDecay = false;

            for (int i = 0; i < 40; ++i)
            {
                const auto before = live.getDiagnostics();
                const auto segment = makeSegment (quiet, index++);
                live.segmentFinalized (segment);
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

            live.segmentFinalized (makeSegment (dropSpec(), index++));
            expect (! lastRecord (live).timeDecay && ! live.getDiagnostics().timeDecayActive && live.getDiagnostics().secondsSinceMain == 0.0,
                    "a main bar stops the time decay");
        }

        // d2. 换歌软遗忘（规格第 3 节；Max 2026-10-02「响的算、轻的要有低音」）：「像换了一首歌」的段——比 I 响 6 LU 以上，或 4/4 型里
        // 轻 6 LU 以上但低频因子 ≥ 0.5——每段 1.9 s，第 11 段累计 20.9 s ≥ 20 s 时触发：全部累计 × 0.2（M 正好 × 0.2）；第 10 段（19 s）
        // 不触发；中间夹一段只差 2 LU 的，清零重计。breakdown（轻 7 LU、没有低音）永不触发；响度型里更轻的段永不触发；只响 5.9 LU 的永不触发。
        // 「响」的那几段低频占比取 0.1：4/4 型 m = 0、不进主段，I 不跟着它走（真实的更响新歌会进主段、I 自己很快跟上，用不着软遗忘）。
        // 无主段衰减关掉。
        void testNewProgram()
        {
            beginTest ("d. new program: louder by > 6 LU, or (4/4) quieter by > 6 LU with full bass, for 20 s scales every mass by 0.2; breakdowns never do");

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
                    live.segmentFinalized (makeSegment (dropSpec(), index++));

                return (double) live.getProfile().integratedLufs.value;
            };

            const auto runBars = [&] (LiveProfile& live, const Spec& spec, int bars, std::int64_t& index)
            {
                Run run;

                for (int i = 0; i < bars; ++i)
                {
                    const auto before = live.getDiagnostics().mainMassSeconds;
                    live.segmentFinalized (makeSegment (spec, index++));

                    if (lastRecord (live).newProgram && run.triggeredAt < 0)
                    {
                        run.triggeredAt = i + 1;
                        run.massBefore = before;
                        run.massAfter = live.getDiagnostics().mainMassSeconds;
                    }
                }

                return run;
            };

            // 1. 4/4 型：轻 7 LU、低音满（lowShare 同 drop）——像换成一首更轻的歌。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::fourOnFloor, index);
                auto quieterWithBass = dropSpec();
                quieterWithBass.loudnessLufs = level - 7.0;

                auto run = runBars (live, quieterWithBass, 10, index);
                expect (run.triggeredAt < 0 && live.getDiagnostics().newProgramCount == 0, "quieter with bass: 10 bars (19 s) do not trigger");

                live.segmentFinalized (segmentReading (index++, level - 2.0, 0.6));   // 只差 2 LU：不算，清零
                run = runBars (live, quieterWithBass, 10, index);
                expect (run.triggeredAt < 0, "a bar that does not count restarts the count: 10 more bars still do not trigger");

                run = runBars (live, quieterWithBass, 1, index);
                expect (run.triggeredAt == 1 && live.getDiagnostics().newProgramCount == 1, "quieter with bass: the 11th consecutive bar (20.9 s) triggers");
                expect (std::abs (run.massAfter - keep * run.massBefore) <= 1.0e-12 * run.massBefore, "every mass x newProgramKeep (main mass exactly)");
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

            // 3. 4/4 型：响 7 LU 的第 11 段触发；之后只响 5.9 LU（不超过 6）的永不触发。
            {
                LiveProfile live;
                std::int64_t index = 0;
                const auto level = prime (live, MainSectionModel::fourOnFloor, index);
                auto louder = dropSpec();
                louder.lowShare = 0.1;
                louder.loudnessLufs = level + 7.0;
                const auto run = runBars (live, louder, 11, index);
                expect (run.triggeredAt == 11 && live.getDiagnostics().newProgramCount == 1, "louder by 7 LU: the 11th bar triggers");

                auto nearby = louder;
                nearby.loudnessLufs = (double) live.getProfile().integratedLufs.value + 5.9;
                runBars (live, nearby, 40, index);
                expect (live.getDiagnostics().newProgramCount == 1, "louder by 5.9 LU never triggers");
            }

            // 4. 响度型：轻 7 LU、低音满也永不触发（更轻的新材料交给长时间无主段的时间衰减）。
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

        //==============================================================================
        // 主段模型与格式字段：4/4 型的 I 用主段加权的门块；换成响度型时（已有累计保留）I 立即换成全部段的 BS.1770 积分（遗忘关掉时
        // 等于测试里按定义逐块算的参照，门边那一格空时只差求和顺序）；格式 2、不填 houseGate；commitWithMainness 钳位。
        void testModelsAndFormat()
        {
            beginTest ("models: 4/4 I is main-weighted, the loudness model's I is every segment's BS.1770 integral; format 2; mainness clamped");

            LiveProfile live;
            setUp (live, noForgetting());
            std::vector<Segment> segments;
            std::int64_t index = 0;

            for (int i = 0; i < 12; ++i)
                segments.push_back (makeSegment (dropSpec(), index++));

            for (int i = 0; i < 12; ++i)   // drop 之后的前奏：按 drop 的 S_ref 判，m = 0
                segments.push_back (makeSegment (introSpec(), index++));

            for (const auto& segment : segments)
                live.segmentFinalized (segment);

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
            double gated = 0.0, nearest = 1.0e9;
            int count = 0;

            for (auto x : blocks)
            {
                nearest = juce::jmin (nearest, std::abs (lufsOf (x) - gate));

                if (lufsOf (x) >= gate)
                {
                    gated += x;
                    ++count;
                }
            }

            const auto reference = lufsOf (gated / (double) count);
            logMessage ("  4/4 (main) I " + juce::String (mainI, 3) + ", loudness-model (all) I " + juce::String (allI, 3) + ", reference "
                        + juce::String (reference, 3));
            expect (mainI > allI + 1.0f, "the main-weighted I is the drop's, above the all-segment integral");
            expect (nearest >= 0.1 && std::abs ((double) allI - reference) <= 1.0e-4, "loudness model: the all-segment BS.1770 integral");

            expect (live.getProfile().formatVersion == glaze::clayProfileFormatVersion && ! live.getProfile().houseGate.valid);

            live.setConstants (noForgetting(), MainSectionModel::fourOnFloor);
            live.commitWithMainness (makeSegment (dropSpec(), index++), 2.0f);
            const auto high = lastRecord (live).mainness;
            live.commitWithMainness (makeSegment (dropSpec(), index++), -1.0f);
            const auto low = lastRecord (live).mainness;
            live.commitWithMainness (makeSegment (dropSpec(), index++), std::numeric_limits<float>::quiet_NaN());
            const auto nan = lastRecord (live).mainness;
            expect (sameFloat (high, 1.0f) && sameFloat (low, 0.0f) && sameFloat (nan, 0.0f), "commitWithMainness clamps to 0 ... 1, NaN -> 0");
        }

        //==============================================================================
        // e. 状态 XML 往返（LiveProfile.h：读回后 getProfile / getDiagnostics 逐位等于保存时）。三种状态：出厂常数下 40 段（缩放因子 ≠ 1）、
        // W_mem 极小（每段都重规范）、冷启动；之后两边再交同样的段仍逐位相同（状态完整）；另经过 XML 文本往返一次。
        void testStateRoundTrip()
        {
            beginTest ("e. state XML round trip is bit-exact (forgetting scale != 1, renormalised, empty); the restored copy evolves identically");

            LiveConstants tiny;
            tiny.fourOnFloor.memoryBars = 0.0;
            tiny.fourOnFloor.memorySeconds = 0.02;   // 每个主段小节 × exp(−95)：缩放因子低于 1e-30，每段都重规范
            tiny.referenceMemorySeconds = 0.02;

            const LiveConstants* const variants[] = { nullptr, &tiny };   // 出厂常数 / 每段都重规范

            for (const auto* constants : variants)
            {
                const auto c = constants != nullptr ? *constants : LiveConstants {};
                LiveProfile a;
                setUp (a, c);
                std::int64_t index = 0;

                for (int i = 0; i < 40; ++i)
                {
                    const auto spec = i % 10 < 2 ? introSpec() : i % 10 < 4 ? breakdownSpec() : dropSpec();
                    a.segmentFinalized (makeSegment (spec, index++));
                }

                const auto xml = a.createStateXml();
                const auto text = xml->toString();
                logMessage ("  state XML: " + juce::String (text.getNumBytesAsUTF8()) + " bytes ("
                            + (constants != nullptr ? juce::String ("renormalised every bar") : juce::String ("factory constants, 40 bars")) + ")");

                LiveProfile b;
                setUp (b, c);
                expect (b.restoreStateXml (*xml), "restore succeeds");
                expect (sameProfile (a.getProfile(), b.getProfile()) && sameDiagnostics (a.getDiagnostics(), b.getDiagnostics()),
                        "restored profile and diagnostics bit-identical");

                auto parsed = juce::parseXML (text);
                LiveProfile viaText;
                setUp (viaText, c);
                expect (parsed != nullptr && viaText.restoreStateXml (*parsed) && sameProfile (a.getProfile(), viaText.getProfile()),
                        "round trip through the XML text");

                auto same = true;

                for (int i = 0; i < 10; ++i)
                {
                    const auto segment = makeSegment (i % 3 == 0 ? breakdownSpec() : dropSpec(), index++);
                    a.segmentFinalized (segment);
                    b.segmentFinalized (segment);
                    same = same && sameProfile (a.getProfile(), b.getProfile()) && sameDiagnostics (a.getDiagnostics(), b.getDiagnostics());
                }

                expect (same, "after restoring, the same segments give the same results (the state is complete)");
                expect (std::isfinite (a.getProfile().integratedLufs.value) && a.getProfile().integratedLufs.valid);
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
                    a.segmentFinalized (makeSegment (dropSpec(), i));

                LiveProfile b;
                b.prepare (2.0 * rate);
                b.setConstants (LiveConstants {}, MainSectionModel::fourOnFloor);
                expect (b.restoreStateXml (*a.createStateXml()), "restores at another sample rate");
                const auto& pa = a.getProfile();
                const auto& pb = b.getProfile();
                expect (std::abs (pa.onsetDensityPerSecond.value - pb.onsetDensityPerSecond.value) <= 1.0e-5f
                            && std::abs (pa.lowShare.effectiveSeconds - pb.lowShare.effectiveSeconds) <= 1.0e-9
                            && std::abs (pa.loudnessRangeLu.effectiveSeconds - pb.loudnessRangeLu.effectiveSeconds) <= 1.0e-9
                            && std::abs (pa.lowCutPeakCostDb.effectiveSeconds - pb.lowCutPeakCostDb.effectiveSeconds) <= 1.0e-9,
                        "sample-based quantities are converted: durations and rates are unchanged");
            }
        }

        // e. 坏数据：标签 / 格式版本不对、缺属性、数组长度不对、非有限值、该非负的是负数、非法字符、缩放因子越界 → false，冷启动
        // （之前的累计也清掉）。
        void testBadState()
        {
            beginTest ("e. bad state data (version, missing / short / non-finite / negative / garbage values, scale) -> cold start, false");

            LiveProfile source;
            setUp (source);

            for (int i = 0; i < 12; ++i)
                source.segmentFinalized (makeSegment (dropSpec(), i));

            const auto good = source.createStateXml();
            LiveProfile cold;
            setUp (cold);
            const auto coldProfile = cold.getProfile();

            struct Damage
            {
                const char* what;
                std::function<void (juce::XmlElement&)> apply;
            };

            const std::vector<Damage> damages {
                { "unknown format version", [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", 2); } },
                { "wrong tag", [] (juce::XmlElement& x) { x.setTagName ("LIVE_PROFILE_X"); } },
                { "missing attribute", [] (juce::XmlElement& x) { x.removeAttribute ("powerSum"); } },
                { "array too short", [] (juce::XmlElement& x) { x.setAttribute ("powerSum", "z26"); } },
                { "array too long", [] (juce::XmlElement& x) { x.setAttribute ("lowRangeCount", "z281"); } },
                { "NaN", [] (juce::XmlElement& x) { x.setAttribute ("mainMass", "7ff8000000000000"); } },
                { "infinity", [] (juce::XmlElement& x) { x.setAttribute ("activeSeconds", "7ff0000000000000"); } },
                { "negative mass", [] (juce::XmlElement& x) { x.setAttribute ("mainMass", "bff0000000000000"); } },
                { "garbage", [] (juce::XmlElement& x) { x.setAttribute ("frames", "12345g7890abcdef"); } },
                { "short hex", [] (juce::XmlElement& x) { x.setAttribute ("frames", "3ff00000"); } },
                { "scale 0", [] (juce::XmlElement& x) { x.setAttribute ("mainScale", "z1"); } },
                { "scale 2", [] (juce::XmlElement& x) { x.setAttribute ("referenceScale", "4000000000000000"); } },
                { "bad count", [] (juce::XmlElement& x) { x.setAttribute ("commits", "-3"); } },
                { "bad mode", [] (juce::XmlElement& x) { x.setAttribute ("lastMode", "7"); } },
                { "trial histogram damaged at its end (read halfway)",
                  [] (juce::XmlElement& x) { x.setAttribute ("trialWeights", x.getStringAttribute ("trialWeights") + " 12zz"); } } };

            // 读档失败后再交同样的段，必须与全新的实例逐位相同：读到一半的数据（例如已经读进来的试算直方图）也清干净了。
            std::vector<Segment> after;

            for (int i = 0; i < 20; ++i)
                after.push_back (makeSegment (i % 4 == 0 ? breakdownSpec() : dropSpec(), 100 + i));

            LiveProfile fresh;
            setUp (fresh);

            for (const auto& segment : after)
                fresh.segmentFinalized (segment);

            for (const auto& damage : damages)
            {
                auto xml = std::make_unique<juce::XmlElement> (*good);
                damage.apply (*xml);

                LiveProfile target;
                setUp (target);

                for (int i = 0; i < 5; ++i)
                    target.segmentFinalized (makeSegment (introSpec(), i));

                const auto restored = target.restoreStateXml (*xml);
                expect (! restored && target.getDiagnostics().commits == 0 && sameProfile (target.getProfile(), coldProfile),
                        juce::String (damage.what) + ": false and cold");

                for (const auto& segment : after)
                    target.segmentFinalized (segment);

                expect (sameProfile (target.getProfile(), fresh.getProfile()) && sameDiagnostics (target.getDiagnostics(), fresh.getDiagnostics()),
                        juce::String (damage.what) + ": nothing of the half-read state is left");
            }
        }

        //==============================================================================
        // f. 同一段序列结果逐位相同：另一个实例一次交完；同一个实例交到一半 reset、再从头交一遍；中间穿插读数不改变结果。提交序号跨 reset 单调。
        void testDeterminismAndReset()
        {
            beginTest ("f. the same segment sequence gives bit-identical results: another instance, after a reset, with reads in between");

            std::vector<Segment> segments;

            for (int i = 0; i < 60; ++i)
            {
                const auto phase = (i / 8) % 4;
                const auto spec = phase == 0 ? introSpec() : phase == 2 ? breakdownSpec() : dropSpec();
                segments.push_back (makeSegment (spec, i));
            }

            LiveProfile a;
            setUp (a);

            for (const auto& segment : segments)
                a.segmentFinalized (segment);

            LiveProfile b;
            setUp (b);

            for (size_t i = 0; i < 30; ++i)
                b.segmentFinalized (segments[i]);

            b.reset();
            LiveProfile cold;
            setUp (cold);
            expect (b.getDiagnostics().commits == 0 && sameProfile (b.getProfile(), cold.getProfile()), "reset is a cold start");

            std::uint64_t cursor = 0;
            LiveCommitRecord records[LiveProfile::commitCapacity];

            for (const auto& segment : segments)
            {
                b.segmentFinalized (segment);
                (void) b.getProfile();
                (void) b.getDiagnostics();
                (void) b.readCommits (cursor, records, 4);
            }

            expect (sameProfile (a.getProfile(), b.getProfile()) && sameDiagnostics (a.getDiagnostics(), b.getDiagnostics()),
                    "bit-identical to a fresh instance fed the same segments");
            expect (lastRecord (b).sequence == 89, "commit sequence numbers keep counting across reset");
        }

        // f. 音频线程接口不经 operator new 分配（TestHelpers.h allocationCountOnThisThread）：并入（含重规范、换歌、时间衰减）、读数、
        // 提交记录、reset。
        void testNoAllocation()
        {
            beginTest ("f. segmentFinalized / commitWithMainness / reads / reset do not allocate (including renormalisation)");

            LiveConstants c;
            c.fourOnFloor.memoryBars = 0.0;
            c.fourOnFloor.memorySeconds = 0.02;   // 每段重规范
            c.noMainSeconds = 5.0;                // 时间衰减很快就开始
            c.newProgramSeconds = 4.0;            // 换歌很快就触发
            LiveProfile live;
            setUp (live, c);

            // 每 8 段：2 段 breakdown、3 段 drop、3 段「响 9 LU、没有低音」（m = 0、I 不动，3 × 1.9 s ≥ 4 s：换歌软遗忘真走到）。
            auto louder = dropSpec();
            louder.loudnessLufs = 0.97;
            louder.lowShare = 0.1;
            std::vector<Segment> segments;

            for (int i = 0; i < 40; ++i)
                segments.push_back (makeSegment (i % 8 < 2 ? breakdownSpec() : i % 8 < 5 ? dropSpec() : louder, i));

            LiveCommitRecord records[LiveProfile::commitCapacity];
            std::uint64_t cursor = 0;
            float sink = 0.0f;
            auto sawNewProgram = false;
            const auto before = allocationCountOnThisThread();

            for (size_t i = 0; i < segments.size(); ++i)
            {
                if (i % 7 == 3)
                    live.commitWithMainness (segments[i], 0.4f);
                else
                    live.segmentFinalized (segments[i]);

                sink += live.mainnessFor (true, -9.0f, true, 0.5f) + live.getProfile().integratedLufs.value + live.getDiagnostics().confidence;
                const auto read = live.readCommits (cursor, records, LiveProfile::commitCapacity);

                for (int k = 0; k < std::abs (read); ++k)
                    sawNewProgram = sawNewProgram || records[k].newProgram;

                if (i == 20)
                    live.reset();
            }

            const auto after = allocationCountOnThisThread();
            expect (after == before, "no allocation: " + juce::String ((juce::int64) (after - before)));
            expect (sawNewProgram, "the run went through the new-program soft forget");
            expect (std::isfinite (sink));
        }

        //==============================================================================
        // 提交记录的环形缓冲（同 SegmentLedger::readEvents）：容量 64；落后超过容量跳到能读到的最早一条、返回负的条数；cursor 超前钳回。
        void testCommitRing()
        {
            beginTest ("commit records: a 64-entry ring read by cursor; a lagging reader skips to the oldest and gets a negative count");

            LiveProfile live;
            setUp (live);

            for (int i = 0; i < 100; ++i)
                live.segmentFinalized (makeSegment (dropSpec(), i));

            LiveCommitRecord records[LiveProfile::commitCapacity];
            std::uint64_t cursor = 0;
            const auto first = live.readCommits (cursor, records, 10);
            expect (first == -10 && cursor == 46 && records[0].sequence == 36, "lagging: skipped to sequence 36, -10 returned");
            const auto second = live.readCommits (cursor, records, LiveProfile::commitCapacity);
            expect (second == 54 && cursor == 100 && records[53].sequence == 99, "then the rest in order");
            expect (live.readCommits (cursor, records, 8) == 0, "nothing new");

            std::uint64_t ahead = 500;
            expect (live.readCommits (ahead, records, 8) == 0 && ahead == 100, "a cursor ahead of the total is clamped");

            live.segmentFinalized (makeSegment (dropSpec(), 100));
            const auto record = lastRecord (live);
            expect (record.sequence == 100 && record.start == 100 * barSamples && record.end == 101 * barSamples
                        && record.mode == glaze::SegmentMode::bars && std::abs (record.nominalSeconds - (double) barSamples / rate) < 1.0e-12
                        && record.mainLevelValid && record.referenceValid,
                    "the record carries the segment, its weight and the profile after the commit");
        }

        // 坏段（累计量里有 NaN）：只记一条 m = 0、有效时长 0 的提交，任何累计都不碰。
        void testBadSegment()
        {
            beginTest ("a segment with non-finite data is recorded with m = 0 and touches no accumulation");

            LiveProfile live;
            setUp (live);

            for (int i = 0; i < 8; ++i)
                live.segmentFinalized (makeSegment (dropSpec(), i));

            const auto before = live.getProfile();
            auto bad = makeSegment (dropSpec(), 8);
            bad.powerSum[3] = std::numeric_limits<double>::quiet_NaN();
            live.segmentFinalized (bad);
            const auto record = lastRecord (live);
            expect (sameFloat (record.mainness, 0.0f) && record.effectiveSeconds == 0.0, "recorded with m = 0");
            expect (sameProfile (before, live.getProfile()), "the profile is untouched");

            bad = makeSegment (dropSpec(), 9);
            bad.rangeCount = 1000;   // 越出定长数组
            live.segmentFinalized (bad);
            expect (sameProfile (before, live.getProfile()), "an out-of-range count is rejected too");
        }

        //==============================================================================
        // g. 每次并入的耗时（音频线程上的一次 segmentFinalized）。护栏：均值 ≤ 500 µs、最大 ≤ 20 ms——107 实测均值约几 µs（REPORT-A），
        // 护栏只防百倍退化（例如每段重扫 0.58 MB 的试算直方图），不当性能门槛；最大值含操作系统的调度抖动，所以放得更宽。
        void testCommitTiming()
        {
            beginTest ("g. per-commit time (mean / max, us) with a wide guard against a 100x regression");

            LiveProfile live;
            setUp (live);
            std::vector<Segment> segments;

            for (int i = 0; i < 2000; ++i)
            {
                const auto phase = (i / 16) % 4;
                auto spec = phase == 0 ? introSpec() : phase == 2 ? breakdownSpec() : dropSpec();
                spec.seed = (std::uint32_t) (100 + i);
                segments.push_back (makeSegment (spec, i));
            }

            double total = 0.0, worst = 0.0;
            const auto ticksPerMicrosecond = (double) juce::Time::getHighResolutionTicksPerSecond() / 1.0e6;
            std::vector<double> times;
            times.reserve (segments.size());

            for (const auto& segment : segments)
            {
                const auto start = juce::Time::getHighResolutionTicks();
                live.segmentFinalized (segment);
                const auto elapsed = (double) (juce::Time::getHighResolutionTicks() - start) / ticksPerMicrosecond;
                total += elapsed;
                worst = juce::jmax (worst, elapsed);
                times.push_back (elapsed);
            }

            const auto mean = total / (double) segments.size();
            std::sort (times.begin(), times.end());
            const auto at = [&times] (double share) { return times[(size_t) std::floor (share * (double) (times.size() - 1))]; };
            logMessage ("  per commit: median " + juce::String (at (0.5), 2) + " us, P99 " + juce::String (at (0.99), 2) + " us, P99.9 "
                        + juce::String (at (0.999), 2) + " us");

            const auto resetStart = juce::Time::getHighResolutionTicks();
            live.reset();
            logMessage ("  reset (cold start, clears every accumulation): "
                        + juce::String ((double) (juce::Time::getHighResolutionTicks() - resetStart) / ticksPerMicrosecond, 1) + " us");

            for (const auto& segment : segments)   // 状态大小按跑满 2000 小节之后量
                live.segmentFinalized (segment);

            // 重规范那一次（W_mem 极小，每段都重规范）的耗时，单独量。
            LiveConstants tiny;
            tiny.fourOnFloor.memoryBars = 0.0;
            tiny.fourOnFloor.memorySeconds = 0.02;
            LiveProfile renormalising;
            setUp (renormalising, tiny);
            double renormTotal = 0.0, renormWorst = 0.0;

            for (size_t i = 0; i < 200; ++i)
            {
                const auto start = juce::Time::getHighResolutionTicks();
                renormalising.segmentFinalized (segments[i]);
                const auto elapsed = (double) (juce::Time::getHighResolutionTicks() - start) / ticksPerMicrosecond;
                renormTotal += elapsed;
                renormWorst = juce::jmax (renormWorst, elapsed);
            }

            logMessage ("  per commit: mean " + juce::String (mean, 2) + " us, max " + juce::String (worst, 1) + " us (2000 bars); "
                        + "renormalising every bar: mean " + juce::String (renormTotal / 200.0, 1) + " us, max " + juce::String (renormWorst, 1) + " us");
            logMessage ("  state XML after the 2000 bars: " + juce::String (live.createStateXml()->toString().getNumBytesAsUTF8()) + " bytes");
            expect (mean <= 500.0, "mean per commit within the 500 us guard");
            expect (worst <= 20000.0, "max per commit within the 20 ms guard");
        }
    };

    static LiveProfileTests liveProfileTests;
}
