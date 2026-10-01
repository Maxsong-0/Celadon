/*
  ==============================================================================

    GlazePlannerTests.cpp
    v0.6 GLAZE：A2 决策器、标定常数、画像与已提交版本存取的测试（04-协调/2026-09-25-v0.6-GLAZE实现计划）。

    按性质测（设计规格-GLAZE-界面与实现第 5 节第 2 条）：GLAZE 0 / NaN 不动任何参数、随 GLAZE 单调、叠加后在参数范围内、
    各模块预算与上限、已够响仍可温和推响、超限降目标、约束反馈、风格分配方向、置信度收紧、受手动限制、逐位确定、0 分配；
    另用真实的 Balance / Color / Limit 模块核对方案在合成素材上的实际用量与净增益（前馈模型对得上、不超上限），
    以及保存 / 读取的往返逐位相同与坏数据拒收。只按 GlazePlanner.h / GlazeConstants.h / GlazeProfile.h 的公开接口写。

    v0.1.7.0 追加（T-012 A5，04-协调/2026-09-26-v0.1.7.0-修坯实现计划）：修坯 Clay 的决策 Plan.clay（设计规格-Clay-分析决策与验收
    第 2 节、第 6 节第 4 条）——公式手算值、GLAZE 0 / 缺特征中性、随 GLAZE 单调与上限、分频点选择、加宽相关度上限、风格混合、
    与电平方案互不影响、置信度封顶、逐位确定与 0 分配；出厂常数新值与标定快照的 clay* 字段读写（旧快照缺字段按 0 读）。

    ADR-012 修坯根治（T-012 阶段 4，设计规格-Clay-根治修订 3.1 / 3.3）改写与追加：真次声阈值 −40 → −28 dB、风格低切目标
    16 / 18 / 20 Hz、收单需要度改按单声道回放低音损失、先试后用的代价系数（0.1 / 0.4 / 0.8 dB 三档、收单代价的对数频率插值、
    代价无效 → 该项中性）、costLimitedMask 的位；删掉的四个旧常数不再进快照；画像格式 2 的代价字段与方案 / 执行快照的
    costLimitedMask 存取、画像号（格式 1 不变）。

    T-038 曲风与交付（设计规格-曲风与交付 第 3.1、4.1、4.2 节；ADR-026）追加：阶段 1 的存档口径（画像格式 3、参考条件、快照常数），
    阶段 2 的决策器——House 子类在风格混合之后覆盖 Tone 参考 / 低切目标 / 加宽系数、Tone 往亮封顶 +0.4、动态预算与通用档逐位相同、
    流媒体 / 俱乐部的目标（风格偏移、LRA 修正、该档钳位）、通用 + 标准不读任何新常数、新档位下的单调 / 范围 / 0 分配。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/BalanceModule.h"
#include "../Source/dsp/ColorModule.h"
#include "../Source/dsp/GlazeCalibrator.h"
#include "../Source/dsp/GlazeConstants.h"
#include "../Source/dsp/GlazePlanner.h"
#include "../Source/dsp/GlazeProfile.h"
#include "../Source/dsp/LimiterModule.h"
#include "../Source/dsp/LoudnessMeter.h"
#include "../Source/ui/HelpText.h"   // celadon::utf8：中文字面量必须经它转换

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>

namespace celadon::test
{
    namespace
    {
        using namespace celadon::glaze;

        constexpr double sampleRate = 48000.0;
        constexpr int numChannels = 2;
        constexpr int blockSize = 512;

        juce::dsp::ProcessSpec makeSpec()
        {
            return { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        }

        //==============================================================================
        bool sameBits (float a, float b) noexcept
        {
            std::uint32_t x, y;
            std::memcpy (&x, &a, sizeof (x));
            std::memcpy (&y, &b, sizeof (y));
            return x == y;
        }

        bool sameIncrements (const AutoIncrements& a, const AutoIncrements& b) noexcept
        {
            return sameBits (a.balanceRecover, b.balanceRecover) && sameBits (a.balanceTame, b.balanceTame)
                && sameBits (a.colorToneDb, b.colorToneDb) && sameBits (a.colorGlue, b.colorGlue)
                && sameBits (a.colorHeat, b.colorHeat) && sameBits (a.limitGainDb, b.limitGainDb)
                && sameBits (a.limitClip, b.limitClip);
        }

        bool samePlan (const Plan& a, const Plan& b) noexcept
        {
            return sameBits (a.requestedTargetLufs, b.requestedTargetLufs) && sameBits (a.effectiveTargetLufs, b.effectiveTargetLufs)
                && sameBits (a.requestedGainDb, b.requestedGainDb) && sameBits (a.effectiveGainDb, b.effectiveGainDb)
                && sameBits (a.estimatedExcessDb, b.estimatedExcessDb) && sameIncrements (a.increments, b.increments)
                && sameBits (a.weights.warm, b.weights.warm) && sameBits (a.weights.punchy, b.weights.punchy)
                && sameBits (a.weights.clean, b.weights.clean)
                && a.limitedByConstraints == b.limitedByConstraints && a.limitedByManual == b.limitedByManual
                && a.automaticLevelWithdrawn == b.automaticLevelWithdrawn && a.manualBudgetMask == b.manualBudgetMask
                && a.algorithmVersion == b.algorithmVersion && a.constantsVersion == b.constantsVersion;
        }

        bool sameParams (const ChainParams& a, const ChainParams& b) noexcept
        {
            return sameBits (a.balance.recover, b.balance.recover) && sameBits (a.balance.tame, b.balance.tame)
                && sameBits (a.balance.automaticRecover, b.balance.automaticRecover)
                && sameBits (a.balance.automaticTame, b.balance.automaticTame)
                && sameBits (a.balance.brighten, b.balance.brighten) && a.balance.speed == b.balance.speed && a.balance.bypass == b.balance.bypass
                && sameBits (a.color.toneDb, b.color.toneDb) && sameBits (a.color.glue, b.color.glue) && sameBits (a.color.heat, b.color.heat)
                && sameBits (a.color.mixPercent, b.color.mixPercent) && sameBits (a.color.outputDb, b.color.outputDb) && a.color.bypass == b.color.bypass
                && sameBits (a.limit.gainDb, b.limit.gainDb) && sameBits (a.limit.ceilingDb, b.limit.ceilingDb)
                && sameBits (a.limit.releaseMs, b.limit.releaseMs) && a.limit.autoRelease == b.limit.autoRelease
                && sameBits (a.limit.lookaheadMs, b.limit.lookaheadMs) && sameBits (a.limit.clip, b.limit.clip)
                && a.limit.listen == b.limit.listen && a.limit.bypass == b.limit.bypass
                && sameBits (a.glazePercent, b.glazePercent) && a.glazeStyle == b.glazeStyle;
        }

        bool allIncrementsZero (const AutoIncrements& inc) noexcept
        {
            return sameIncrements (inc, AutoIncrements {});
        }

        //==============================================================================
        void setFeature (Feature& f, float value, float confidence = 1.0f)
        {
            f.value = value;
            f.valid = true;
            f.confidence = confidence;
            f.effectiveSeconds = 20.0;
        }

        // 一份「听够了」的画像：I、TP、LRA、斜率、起音、占比都有效，27 段全可信、没有频谱偏离证据。
        MusicProfile makeProfile (float lufs, float truePeak, float lra = 5.0f, float confidence = 1.0f)
        {
            MusicProfile p;
            setFeature (p.integratedLufs, lufs, confidence);
            setFeature (p.truePeakDbtp, truePeak, confidence);
            setFeature (p.plrDb, truePeak - lufs, confidence);
            setFeature (p.loudnessRangeLu, lra, confidence);
            setFeature (p.spectralSlopeDbPerOct, -2.5f, confidence);
            setFeature (p.lowShare, 0.3f, confidence);
            setFeature (p.highShare, 0.08f, confidence);
            setFeature (p.onsetDensityPerSecond, 2.0f, confidence);
            setFeature (p.onsetStrengthDb, 6.0f, confidence);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                p.bandLongTermDb[b] = -30.0f - 0.5f * (float) b;
                p.bandConfidence[b] = confidence;
            }

            p.effectiveSeconds = 20.0;
            p.representativeness = confidence;
            return p;
        }

        ChainParams defaultManual()
        {
            ChainParams m;
            m.limit.ceilingDb = -1.0f;
            return m;
        }

        Plan planFor (const MusicProfile* profile, Style style, float glaze, const ChainParams& manual = defaultManual(),
                      float maxGainDb = 1.0e9f, const StyleWeights* locked = nullptr)
        {
            PlanInputs in;
            in.profile = profile;
            in.style = style;
            in.glazePercent = glaze;
            in.manual = manual;
            in.maxGainDb = maxGainDb;
            in.lockedWeights = locked;
            return makePlan (in);
        }

        //==============================================================================
        // 可复现的随机场景（性质扫描用）：画像、手动值（一半是默认值）、风格、约束反馈。
        struct Scenario
        {
            MusicProfile profile;
            bool hasProfile = true;
            ChainParams manual;
            Style style = Style::automatic;
            float maxGainDb = 1.0e9f;
        };

        Scenario randomScenario (juce::Random& r)
        {
            auto range = [&r] (float lo, float hi) { return lo + (hi - lo) * r.nextFloat(); };

            Scenario s;
            const auto lufs = range (-32.0f, -5.0f);
            const auto confidence = r.nextFloat() < 0.3f ? range (0.0f, 1.0f) : 1.0f;
            s.profile = makeProfile (lufs, lufs + range (3.0f, 20.0f), range (0.0f, 20.0f), confidence);
            setFeature (s.profile.spectralSlopeDbPerOct, range (-6.0f, 1.0f), range (0.0f, 1.0f));
            setFeature (s.profile.onsetDensityPerSecond, range (0.0f, 6.0f), range (0.0f, 1.0f));
            setFeature (s.profile.onsetStrengthDb, range (0.0f, 14.0f), range (0.0f, 1.0f));

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                s.profile.bandConfidence[b] = range (0.0f, 1.0f);
                s.profile.tameEvidenceDb[b] = r.nextFloat() < 0.2f ? range (0.0f, 4.0f) : 0.0f;
                s.profile.recoverEvidenceDb[b] = r.nextFloat() < 0.2f ? range (0.0f, 4.0f) : 0.0f;
            }

            s.hasProfile = r.nextFloat() > 0.05f;
            s.style = (Style) r.nextInt (4);
            s.maxGainDb = r.nextFloat() < 0.2f ? range (-3.0f, 12.0f) : 1.0e9f;
            s.manual = defaultManual();

            if (r.nextBool())
            {
                auto& m = s.manual;
                m.limit.ceilingDb = range (-3.0f, 0.0f);
                m.limit.gainDb = r.nextBool() ? range (0.0f, 10.0f) : 0.0f;
                m.limit.clip = r.nextFloat() < 0.3f ? range (0.0f, 100.0f) : 0.0f;
                m.color.glue = r.nextFloat() < 0.3f ? range (0.0f, 100.0f) : 0.0f;
                m.color.heat = r.nextFloat() < 0.3f ? range (0.0f, 60.0f) : 0.0f;
                m.color.toneDb = r.nextFloat() < 0.3f ? range (-3.0f, 3.0f) : 0.0f;
                m.color.mixPercent = r.nextFloat() < 0.2f ? range (0.0f, 100.0f) : 100.0f;
                m.color.outputDb = r.nextFloat() < 0.2f ? range (-6.0f, 6.0f) : 0.0f;
                m.balance.recover = r.nextFloat() < 0.3f ? range (0.0f, 100.0f) : 0.0f;
                m.balance.tame = r.nextFloat() < 0.3f ? range (0.0f, 100.0f) : 0.0f;
                m.color.bypass = r.nextFloat() < 0.05f;
                m.limit.bypass = r.nextFloat() < 0.05f;
                m.balance.bypass = r.nextFloat() < 0.05f;
            }

            return s;
        }

        Plan planFor (const Scenario& s, float glaze)
        {
            return planFor (s.hasProfile ? &s.profile : nullptr, s.style, glaze, s.manual, s.maxGainDb);
        }

        //==============================================================================
        // 类音乐合成素材（与 GlazeBehaviourReport 同一类：120 BPM 底鼓 + 军鼓噪声 + 贝斯正弦 + 限带三和弦，PLR 约 13 dB），
        // 决策器的模型常数就是在这类素材上实测拟合的。
        Signal makeMusic (double seconds)
        {
            const auto n = (int) (seconds * sampleRate);
            auto signal = makeSilence (numChannels, n);
            const auto beat = (int) (0.5 * sampleRate);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            juce::Random random (20260925);

            for (int start = 0, index = 0; start < n; start += beat, ++index)
            {
                const auto length = juce::jmin ((int) (0.35 * sampleRate), n - start);
                double phase = 0.0, lowpass[2] {};

                for (int i = 0; i < length; ++i)
                {
                    const auto t = i / sampleRate;

                    if (index % 2 == 0)   // 底鼓：165 → 48 Hz 扫频、100 ms 衰减
                    {
                        phase += twoPi * (48.0 + 117.0 * std::exp (-t / 0.025)) / sampleRate;
                        const auto value = (float) (std::exp (-t / 0.10) * juce::jmin (1.0, t / 0.002) * std::sin (phase));

                        for (auto& channel : signal)
                            channel[(size_t) (start + i)] += value;
                    }
                    else                  // 军鼓：6 kHz 低通噪声 70 ms 衰减 + 185 Hz 鼓身
                    {
                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            lowpass[ch] += 0.55 * (random.nextDouble() * 2.0 - 1.0 - lowpass[ch]);
                            const auto body = 0.5 * std::sin (twoPi * 185.0 * t) * std::exp (-t / 0.04);
                            signal[(size_t) ch][(size_t) (start + i)] += (float) ((0.6 * lowpass[ch] * std::exp (-t / 0.07) + body)
                                                                                   * juce::jmin (1.0, t / 0.001));
                        }
                    }
                }
            }

            // 贝斯（55 Hz 八分音符）与三和弦（A 小三和弦，每个音 6 个谐波的锯齿）。
            const auto eighth = beat / 2;

            for (int i = 0; i < n; ++i)
            {
                const auto t = i / sampleRate;
                const auto noteTime = (double) (i % eighth) / sampleRate;
                auto value = 0.45 * std::exp (-noteTime / 0.35) * juce::jmin (1.0, noteTime / 0.003) * std::sin (twoPi * 55.0 * t);

                for (auto note : { 220.0, 261.63, 329.63 })
                    for (int k = 1; k <= 6; ++k)
                        value += 0.18 * (k % 2 == 1 ? 1.0 : -1.0) * std::sin (twoPi * k * note * t) / k * 0.6366;

                for (auto& channel : signal)
                    channel[(size_t) i] += (float) value;
            }

            return signal;
        }

        float integratedLufsOf (const Signal& signal)
        {
            LoudnessMeter meter;
            meter.prepare (makeSpec());
            juce::AudioBuffer<float> block (numChannels, blockSize);
            const auto n = (int) signal.front().size();

            for (int start = 0; start < n; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, n - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, signal[(size_t) ch].data() + start, length);

                meter.process (block);
            }

            return meter.getIntegratedLufs();
        }

        Signal atLufs (const Signal& source, float lufs)
        {
            auto signal = source;
            scale (signal, juce::Decibels::decibelsToGain (lufs - integratedLufsOf (signal)));
            return signal;
        }

        // 素材的画像（分析器还在别的线里做，这里用表直接量 I 与真峰值；LRA 取合成素材的量级）。
        MusicProfile profileOf (const Signal& signal)
        {
            const auto lufs = integratedLufsOf (signal);
            return makeProfile (lufs, toDb (truePeakOf (signal)), 4.0f);
        }

        // 用真实模块（Balance → Color → Limit，与 CeladonChain 同一顺序）跑一遍实际参数，量净增益与各模块统计。
        struct ChainRun
        {
            float netDb = 0.0f, outputTruePeakDb = 0.0f;
            float glueMeanDb = 0.0f, glueP95Db = 0.0f;
            float clipDepthMaxDb = 0.0f, clipActivityMean = 0.0f;
            float limitPeakDb = 0.0f, limitMeanDb = 0.0f, limitP95Db = 0.0f;
        };

        float percentile95 (std::vector<float> values)
        {
            std::sort (values.begin(), values.end());
            return values[(size_t) juce::jlimit (0, (int) values.size() - 1, (int) std::ceil (0.95 * (double) values.size()) - 1)];
        }

        ChainRun runChain (const Signal& input, const ChainParams& actual)
        {
            BalanceModule balance;
            ColorModule color;
            LimiterModule limiter;

            balance.setParams (actual.balance);
            color.setParams (actual.color);
            limiter.setParams (actual.limit);
            balance.prepare (makeSpec());
            color.prepare (makeSpec());
            limiter.prepare (makeSpec());
            balance.setParams (actual.balance);
            color.setParams (actual.color);
            limiter.setParams (actual.limit);

            auto padded = input;
            appendSilence (padded, color.getLatencySamples() + limiter.getLatencySamples());
            auto output = padded;

            juce::AudioBuffer<float> block (numChannels, blockSize);
            std::vector<float> glueMeans, limitMeans;
            ChainRun run;
            double activity = 0.0;
            const auto n = (int) padded.front().size();

            for (int start = 0; start < n; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, n - start);
                block.setSize (numChannels, length, false, false, true);

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, padded[(size_t) ch].data() + start, length);

                balance.process (block);
                color.process (block);
                limiter.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, output[(size_t) ch].begin() + start);

                const auto& colorStats = color.getBlockStats();
                const auto& limitStats = limiter.getBlockStats();
                glueMeans.push_back (colorStats.glueGrMeanDb);
                limitMeans.push_back (limitStats.grMeanDb);
                run.limitPeakDb = juce::jmax (run.limitPeakDb, limitStats.grMaxDb);
                run.clipDepthMaxDb = juce::jmax (run.clipDepthMaxDb, limitStats.clipDepthMaxDb);
                activity += limitStats.clipActiveFraction;
            }

            auto mean = [] (const std::vector<float>& v)
            {
                double sum = 0.0;

                for (auto x : v)
                    sum += x;

                return (float) (sum / (double) v.size());
            };

            run.netDb = integratedLufsOf (output) - integratedLufsOf (padded);
            run.outputTruePeakDb = toDb (truePeakOf (output));
            run.glueMeanDb = mean (glueMeans);
            run.glueP95Db = percentile95 (glueMeans);
            run.limitMeanDb = mean (limitMeans);
            run.limitP95Db = percentile95 (limitMeans);
            run.clipActivityMean = (float) (activity / (double) glueMeans.size());
            return run;
        }
    }

    //==============================================================================
    class GlazePlannerTests  : public juce::UnitTest
    {
    public:
        GlazePlannerTests() : juce::UnitTest ("GlazePlanner", "Celadon") {}

        void runTest() override
        {
            testGlazeZeroLeavesParametersUntouched();
            testMonotonicInGlaze();
            testActualValuesStayInRange();
            testModuleBudgets();
            testAlreadyLoudStillRequestsDrive();
            testGentleColorWithHeadroom();
            testBidirectionalTone();
            testExcessLowersTarget();
            testMaxGainFeedback();
            testStyleDirections();
            testHeatOnlyClaimsRealCrestReduction();   // T-032 R6-01
            testGainRangeBoundsEffectiveGain();       // T-032 R6-06
            testGlueLowFrequencyRisk();
            testLowConfidenceTightens();
            testConfidenceNeverMoreAggressive();
            testManualSweepIsContinuous();
            testClipRampsFromGlazeZero();
            testManualLimits();
            testManualBudgetMask();                   // T-032 R6-02 收尾（A11）
            testDeterminism();
            testAutomaticWeights();
            testBlendConstants();
            testRealChainWithinLimits();
            testMappingsMatchModules();
            testProfileId();
            testProfileAndStateRoundTrip();
            testBrokenDataRejected();
            testConstantsRoundTrip();
            testOddSnapshots();
            testNoAllocation();
            testGenreDeliveryStorage();               // T-038 阶段 1（存档口径）
            testGenreAndDeliveryPlanning();           // T-038 阶段 2（决策器：曲风覆盖、Tone 往亮上限、交付目标）

            // v0.1.7.0 修坯 Clay（A5；ADR-012 根治修订 P3 改写 / 追加）
            testClayFormulas();
            testClayCostFactor();
            testClayAutoMonoAlwaysNeutral();
            testClayNeutralCases();
            testClayMonotonicAndBounded();
            testClayWidthCap();
            testClayStyleBlend();
            testClayIndependentOfLevelPlan();
            testClayConfidence();
            testClayConstants();
            testClayCostStorage();
            testClayDeterminismAndNoAllocation();
        }

    private:
        //==============================================================================
        void testGlazeZeroLeavesParametersUntouched()
        {
            beginTest ("GLAZE zero or NaN leaves every parameter untouched");

            // 依据：设计规格-GLAZE-决策与校准第 8 节「GLAZE 0 等于 v0.5 的手动处理结果」；GlazePlanner.h「GLAZE ≤ 0 或 NaN → 增量全为 0、目标 = 输入响度」。
            const auto profile = makeProfile (-20.0f, -6.0f);

            // 手动值里故意放 −0、超范围的值：GLAZE 0 时必须原样透传（逐位），不做钳位、不做 0 + x 运算。
            auto manual = defaultManual();
            manual.color.toneDb = -0.0f;
            manual.balance.recover = 150.0f;
            manual.limit.gainDb = 3.25f;
            manual.color.glue = 42.5f;

            const auto reference = planFor (&profile, Style::automatic, 0.0f, manual);

            for (auto glaze : { 0.0f, -0.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity() })
                for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                {
                    const auto plan = planFor (&profile, style, glaze, manual);
                    expect (allIncrementsZero (plan.increments), "GLAZE " + juce::String (glaze) + utf8 (" 产生了自动增量"));
                    expect (sameBits (plan.requestedGainDb, reference.requestedGainDb) && sameBits (plan.effectiveGainDb, reference.effectiveGainDb),
                            utf8 ("GLAZE ≤ 0 的净增益应只取决于手动链"));
                    expect (sameBits (plan.requestedGainDb, plan.effectiveGainDb));
                    expectEquals (plan.effectiveTargetLufs, profile.integratedLufs.value + plan.effectiveGainDb);
                    expect (! plan.limitedByConstraints && ! plan.limitedByManual);
                    expect (sameParams (applyIncrements (manual, plan.increments), manual), utf8 ("零增量叠加后应与手动值逐位相同"));
                }

            // 手动全默认、输入不超 Ceiling：净增益精确为 0，目标 = 输入响度（v0.5 的手动结果就是原样）。
            const auto neutral = planFor (&profile, Style::warm, 0.0f);
            expect (sameBits (neutral.effectiveGainDb, 0.0f) && sameBits (neutral.requestedGainDb, 0.0f), utf8 ("手动中性时 GLAZE 0 的净增益应为 0"));
            expectEquals (neutral.effectiveTargetLufs, profile.integratedLufs.value, utf8 ("手动中性时 GLAZE 0 的目标应等于输入响度"));

            // 未学习（没有画像）时同样全零；目标 = 保守默认值。
            const auto unlearned = planFor (nullptr, Style::automatic, 0.0f);
            expect (allIncrementsZero (unlearned.increments));
            expectEquals (unlearned.effectiveTargetLufs, factoryConstants().unlearnedInputLufs);

            // 增量里的 NaN / ±Inf / −0 同样不动手动值。
            AutoIncrements odd;
            odd.colorToneDb = -0.0f;
            odd.colorGlue = std::numeric_limits<float>::quiet_NaN();
            odd.limitGainDb = std::numeric_limits<float>::infinity();
            odd.balanceTame = -std::numeric_limits<float>::infinity();
            expect (sameParams (applyIncrements (manual, odd), manual), utf8 ("非有限增量不应改动手动值"));
        }

        //==============================================================================
        void testBidirectionalTone()
        {
            beginTest ("automatic Tone follows both spectral directions, with a dead zone and conservative signed caps");
            for (auto style : { Style::warm, Style::punchy, Style::clean })
            {
                const auto& caps = constantsFor (factoryConstants(), style);
                for (auto difference : { -4.0f, -0.1f, 0.0f, 0.1f, 4.0f })
                {
                    auto p = makeProfile (-20.0f, -8.0f);
                    setFeature (p.spectralSlopeDbPerOct, caps.toneReferenceSlopeDbPerOct - difference);
                    const auto full = planFor (&p, style, 100.0f).increments.colorToneDb;
                    const auto half = planFor (&p, style, 50.0f).increments.colorToneDb;
                    expect (std::abs (full) <= caps.toneMaxDb);
                    expectWithinAbsoluteError (half, full * 0.5f, 1.0e-6f);
                    if (std::abs (difference) <= automaticToneSlopeDeadbandDbPerOct)
                        expectEquals (full, 0.0f);
                    else
                        expect (full * difference > 0.0f, "dark material brightens; bright material softens");
                    auto lastMagnitude = std::abs (full);
                    for (auto confidence : { 0.6f, 0.3f, 0.0f })
                    {
                        p.spectralSlopeDbPerOct.confidence = confidence;
                        const auto tone = planFor (&p, style, 100.0f).increments.colorToneDb;
                        expect (std::abs (tone) <= lastMagnitude + 1.0e-6f && tone * full >= 0.0f);
                        lastMagnitude = std::abs (tone);
                    }
                    p.spectralSlopeDbPerOct.valid = false;
                    expectEquals (planFor (&p, style, 100.0f).increments.colorToneDb, 0.0f);
                }
            }

            auto manual = defaultManual();
            manual.color.toneDb = -2.9f;
            manual.balance.tame = 96.0f;
            AutoIncrements inc;
            inc.colorToneDb = -0.75f;
            inc.balanceTame = 20.0f;
            const auto effective = applyIncrements (manual, inc);
            expectEquals (effective.color.toneDb, -3.0f);
            expectEquals (effective.balance.automaticTame, 4.0f, "automatic Tame is the actual share remaining after clamping");
            expect (sameParams (applyIncrements (manual, {}), manual));
        }

        void testMonotonicInGlaze()
        {
            beginTest ("Processing magnitudes and effective gain never decrease as GLAZE rises");

            // 依据：设计规格-GLAZE 验收总表第 2 条「随 GLAZE 单调」；决策与校准第 4 节（约束边界上也不许倒退）。
            juce::Random random (0x5eed01);
            int failures = 0, checked = 0;

            for (int trial = 0; trial < 150; ++trial)
            {
                const auto scenario = randomScenario (random);
                Plan previous = planFor (scenario, 0.0f);

                for (int step = 1; step <= 200; ++step)
                {
                    const auto plan = planFor (scenario, 0.5f * (float) step);
                    const auto& a = plan.increments;
                    const auto& b = previous.increments;

                    const bool ok = a.balanceRecover >= b.balanceRecover && a.balanceTame >= b.balanceTame
                                 && std::abs (a.colorToneDb) >= std::abs (b.colorToneDb) && a.colorToneDb * b.colorToneDb >= 0.0f
                                 && a.colorGlue >= b.colorGlue && a.colorHeat >= b.colorHeat
                                 && a.limitGainDb >= b.limitGainDb && a.limitClip >= b.limitClip
                                 && plan.requestedGainDb >= previous.requestedGainDb && plan.effectiveGainDb >= previous.effectiveGainDb
                                 && plan.effectiveTargetLufs >= previous.effectiveTargetLufs && plan.requestedTargetLufs >= previous.requestedTargetLufs;

                    if (! ok && ++failures <= 5)
                        logMessage (utf8 ("  非单调：场景 ") + juce::String (trial) + utf8 ("，GLAZE ") + juce::String (0.5f * (float) step));

                    ++checked;
                    previous = plan;
                }
            }

            expectEquals (failures, 0, juce::String (checked) + utf8 (" 个 GLAZE 台阶里有非单调"));
        }

        //==============================================================================
        void testActualValuesStayInRange()
        {
            beginTest ("Actual parameters stay inside their ranges");

            // 依据：决策与校准第 8 节「实际值 = clamp(手动值 + 自动增量, 下限, 上限)」；范围与 Parameters.cpp 一致。
            juce::Random random (0x5eed02);
            int failures = 0;

            for (int trial = 0; trial < 150; ++trial)
            {
                const auto scenario = randomScenario (random);

                for (auto glaze : { 10.0f, 35.0f, 70.0f, 100.0f })
                {
                    const auto plan = planFor (scenario, glaze);
                    const auto p = applyIncrements (scenario.manual, plan.increments);
                    const auto inRange = [] (float v, float lo, float hi) { return v >= lo && v <= hi; };

                    const bool ok = inRange (p.balance.recover, 0.0f, 100.0f) && inRange (p.balance.tame, 0.0f, 100.0f)
                                 && inRange (p.balance.automaticRecover, 0.0f, p.balance.recover)
                                 && inRange (p.color.toneDb, -3.0f, 3.0f) && inRange (p.color.glue, 0.0f, 100.0f)
                                 && inRange (p.color.heat, 0.0f, 100.0f) && inRange (p.limit.gainDb, 0.0f, 24.0f)
                                 && inRange (p.limit.clip, 0.0f, 100.0f)
                                 && std::isfinite (plan.effectiveGainDb) && std::isfinite (plan.effectiveTargetLufs);

                    failures += ok ? 0 : 1;

                    // 不受 GLAZE 影响的参数原样保留（设计规格-GLAZE-决策与校准第 5 节）。
                    expect (sameBits (p.limit.ceilingDb, scenario.manual.limit.ceilingDb) && sameBits (p.limit.lookaheadMs, scenario.manual.limit.lookaheadMs)
                            && sameBits (p.limit.releaseMs, scenario.manual.limit.releaseMs) && sameBits (p.color.mixPercent, scenario.manual.color.mixPercent)
                            && sameBits (p.color.outputDb, scenario.manual.color.outputDb) && sameBits (p.balance.brighten, scenario.manual.balance.brighten)
                            && p.balance.speed == scenario.manual.balance.speed && p.limit.listen == scenario.manual.limit.listen
                            && p.color.bypass == scenario.manual.color.bypass && p.limit.bypass == scenario.manual.limit.bypass);
                }
            }

            expectEquals (failures, 0, utf8 ("叠加手动值后出了参数范围"));

            // 极端增量被钳到范围两端。
            AutoIncrements huge;
            huge.balanceRecover = huge.balanceTame = huge.colorGlue = huge.colorHeat = huge.limitClip = 1000.0f;
            huge.colorToneDb = 1000.0f;
            huge.limitGainDb = 1000.0f;
            const auto top = applyIncrements (defaultManual(), huge);
            expectEquals (top.balance.recover, 100.0f);
            expectEquals (top.balance.automaticRecover, 100.0f);
            expectEquals (top.color.toneDb, 3.0f);
            expectEquals (top.limit.gainDb, 24.0f);
            expectEquals (top.limit.clip, 100.0f);

            AutoIncrements negative;
            negative.colorToneDb = -1000.0f;
            negative.limitGainDb = -1000.0f;
            const auto bottom = applyIncrements (defaultManual(), negative);
            expectEquals (bottom.color.toneDb, -3.0f);
            expectEquals (bottom.limit.gainDb, 0.0f);

            auto manualRecover = defaultManual();
            manualRecover.balance.recover = 97.0f;
            AutoIncrements automatic;
            automatic.balanceRecover = 8.0f;
            const auto clamped = applyIncrements (manualRecover, automatic);
            expectEquals (clamped.balance.recover, 100.0f);
            expectEquals (clamped.balance.automaticRecover, 3.0f, "only the actual automatic portion is safety gated");
            expect (sameParams (applyIncrements (clamped, AutoIncrements {}), clamped), "zero increments preserve every field");
        }

        //==============================================================================
        void testModuleBudgets()
        {
            beginTest ("Auto increments respect each module budget");

            // 依据：决策与校准第 4 节（Heat 驱动上限）、第 5 节（Recover / Tame 上限、Tone 上限 0.75 dB × GLAZE、提亮只由 Tone 分配）。
            const auto& c = factoryConstants();
            juce::Random random (0x5eed03);

            for (int trial = 0; trial < 120; ++trial)
            {
                auto scenario = randomScenario (random);
                scenario.manual = defaultManual();   // 自动部分本身的预算：手动为中性时看得最清楚
                scenario.hasProfile = true;

                for (auto glaze : { 25.0f, 60.0f, 100.0f })
                {
                    const auto plan = planFor (scenario, glaze);
                    const auto& inc = plan.increments;
                    const auto g = glaze / 100.0f;
                    const auto sc = blendConstants (c, plan.weights);

                    expect (inc.colorHeat <= sc.heatDriveCap + 1.0e-3f, utf8 ("Heat 超过驱动上限"));
                    expect (inc.balanceRecover <= sc.recoverCap * g + 1.0e-3f, utf8 ("Recover 超过上限 × GLAZE"));
                    expect (inc.balanceTame <= sc.tameCap * g + 1.0e-3f, utf8 ("Tame 超过上限 × GLAZE"));
                    expect (std::abs (inc.colorToneDb) <= sc.toneMaxDb * g + 1.0e-4f, utf8 ("Tone 超出 ±0.75 dB × GLAZE"));
                    expect (inc.colorGlue >= 0.0f && inc.limitClip >= 0.0f && inc.limitGainDb >= 0.0f);
                }
            }

            // 已学习段没有偏离时仍给实时 Balance 少量权限；证据足才到上限。
            auto profile = makeProfile (-18.0f, -5.0f);
            const auto none = planFor (&profile, Style::warm, 100.0f);
            expectEquals (none.increments.balanceRecover, c.automaticRecoverFloor);
            expectEquals (none.increments.balanceTame, c.automaticTameFloor);
            const auto half = planFor (&profile, Style::warm, 50.0f);
            expectEquals (half.increments.balanceRecover, 0.5f * c.automaticRecoverFloor);
            expectEquals (half.increments.balanceTame, 0.5f * c.automaticTameFloor);

            const auto initial = planFor (nullptr, Style::automatic, 100.0f);
            expectWithinAbsoluteError (initial.increments.balanceRecover, c.automaticRecoverFloor * c.unlearnedStrength, 1.0e-5f);
            expectWithinAbsoluteError (initial.increments.balanceTame, c.automaticTameFloor * c.unlearnedStrength, 1.0e-5f);
            auto bypassed = defaultManual();
            bypassed.balance.bypass = true;
            const auto bypassedPlan = planFor (&profile, Style::warm, 100.0f, bypassed);
            expectEquals (bypassedPlan.increments.balanceRecover, 0.0f);
            expectEquals (bypassedPlan.increments.balanceTame, 0.0f);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                profile.recoverEvidenceDb[b] = 1.2f;
                profile.tameEvidenceDb[b] = 1.2f;
            }

            const auto some = planFor (&profile, Style::warm, 100.0f);
            expectEquals (some.increments.balanceTame, c.warm.tameCap, utf8 ("证据足够时 Tame 应到上限"));
            expectWithinAbsoluteError (some.increments.balanceRecover, c.warm.recoverCap * 1.2f / c.recoverEvidenceFullDb, 1.0e-3f);
            expect (some.increments.balanceRecover < c.warm.recoverCap, utf8 ("同样的证据 Recover 应给得比上限少（比 Tame 保守）"));

            // Tone：比参考斜率暗 → 提亮；比参考亮 → 收暗，两个方向均保留容忍区（Brighten 不自动）。
            setFeature (profile.spectralSlopeDbPerOct, c.warm.toneReferenceSlopeDbPerOct - 3.0f);
            expectEquals (planFor (&profile, Style::warm, 100.0f).increments.colorToneDb, c.warm.toneMaxDb);
            expectWithinAbsoluteError (planFor (&profile, Style::warm, 40.0f).increments.colorToneDb, c.warm.toneMaxDb * 0.4f, 1.0e-3f);
            setFeature (profile.spectralSlopeDbPerOct, c.warm.toneReferenceSlopeDbPerOct + 1.0f);
            expectWithinAbsoluteError (planFor (&profile, Style::warm, 100.0f).increments.colorToneDb,
                                       -(1.0f - automaticToneSlopeDeadbandDbPerOct) * c.toneDbPerSlopeDb, 1.0e-6f);
        }

        //==============================================================================
        void testAlreadyLoudStillRequestsDrive()
        {
            beginTest ("Already loud material retains a bounded monotone drive request");

            const auto minimumDrive = factoryConstants().minimumFullDriveDb;
            for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
            {
                const auto loud = makeProfile (-6.0f, -1.5f, 3.0f);   // 比三种风格的偏好都响，峰值在 Ceiling 以下
                auto previousGain = 0.0f;
                for (auto glaze : { 0.0f, 10.0f, 25.0f, 50.0f, 75.0f, 100.0f })
                {
                    const auto plan = planFor (&loud, style, glaze);
                    expectWithinAbsoluteError (plan.requestedGainDb, minimumDrive * glaze * 0.01f, 1.0e-4f);
                    expect (plan.effectiveGainDb >= previousGain - 1.0e-4f);
                    expect (plan.effectiveGainDb <= plan.requestedGainDb + 1.0e-4f, "module constraints still bound the push");
                    previousGain = plan.effectiveGainDb;
                }
                expect (previousGain > 0.0f, "a hot master is not a dead region of the macro");
            }

            // 目标差距不足 3 dB 时提供最小推响请求；更安静的素材保留原来的响度规划。
            const auto atPreference = makeProfile (-10.0f, -4.0f, 3.0f);
            expectEquals (planFor (&atPreference, Style::clean, 100.0f).requestedGainDb, minimumDrive);

            const auto quieter = makeProfile (-12.0f, -6.0f, 3.0f);
            expectWithinAbsoluteError (planFor (&quieter, Style::clean, 100.0f).requestedGainDb, minimumDrive, 1.0e-4f);
            expectWithinAbsoluteError (planFor (&quieter, Style::clean, 50.0f).requestedGainDb, 0.5f * minimumDrive, 1.0e-4f);
            const auto quiet = makeProfile (-18.0f, -12.0f, 3.0f);
            expectWithinAbsoluteError (planFor (&quiet, Style::clean, 100.0f).requestedGainDb, 8.0f, 1.0e-4f);
        }

        void testGentleColorWithHeadroom()
        {
            beginTest ("Gentle color is modeled even when there is no peak-reduction request");
            const auto profile = makeProfile (-18.0f, -12.0f, 3.0f);
            const auto plan = planFor (&profile, Style::clean, 100.0f);
            expectEquals (plan.estimatedExcessDb, 0.0f);
            expect (plan.increments.colorHeat > 0.0f);
            expect (plan.increments.colorHeat <= factoryConstants().clean.automaticHeatFloor);   // T-032：底量是风格常数
            expectWithinAbsoluteError (plan.effectiveGainDb, 8.0f, 1.0e-3f, "color compensation preserves the planned net gain");

            auto manual = defaultManual();
            manual.color.bypass = true;
            expectEquals (planFor (&profile, Style::clean, 100.0f, manual).increments.colorHeat, 0.0f);
            manual.color.bypass = false;
            manual.color.mixPercent = 0.0f;
            expectEquals (planFor (&profile, Style::clean, 100.0f, manual).increments.colorHeat, 0.0f);

            // 零净增益没有补偿正向染色的空间：底量必须让位，不能凭空增加响度。
            const auto lowLevel = makeProfile (-24.0f, -18.0f, 3.0f);   // Heat 换算表这一行的轻微染色增加响度。
            auto constants = factoryConstants();
            constants.minimumFullDriveDb = 0.0f;
            constants.clean.preferredLufs = -24.0f;
            constants.minPreferredLufs = -30.0f;
            // 人为提供正向增益的染色模型，明确验证不可补偿时的退让；新并联曲线在这个电平几乎中性。
            constants.heatLoudnessDb.fill (1.0f);
            for (int level = 0; level < heatTableLevels; ++level)
                constants.heatLoudnessDb[(size_t) (level * heatTableAmounts)] = 0.0f;
            PlanInputs input;
            input.profile = &lowLevel;
            input.style = Style::clean;
            input.glazePercent = 100.0f;
            input.manual = defaultManual();
            input.constants = &constants;
            const auto noBudget = makePlan (input);
            expectEquals (noBudget.effectiveGainDb, 0.0f);
            expectEquals (noBudget.increments.colorHeat, 0.0f, "withdraw the floor when compensation is infeasible");

            beginTest ("parallel Heat with no crest capacity does not block downstream loudness headroom");
            auto noHeatCrest = factoryConstants();
            noHeatCrest.heatLoudnessDb.fill (0.0f);
            noHeatCrest.heatPeakDb.fill (0.0f);
            const auto peakyQuiet = makeProfile (-24.0f, -12.0f, 3.0f);
            input.profile = &peakyQuiet;
            input.constants = &noHeatCrest;
            for (auto style : { Style::warm, Style::punchy, Style::clean })
            {
                input.style = style;
                const auto available = makePlan (input);
                expect (available.effectiveGainDb > 11.5f, "usable limiter headroom must survive a non-clipping Heat curve");
                expect (available.effectiveGainDb <= available.requestedGainDb + 1.0e-4f);
            }
            noHeatCrest.warm.shareHeat = 1.0f;
            noHeatCrest.warm.shareClip = noHeatCrest.warm.shareLimit = 0.0f;
            input.style = Style::warm;
            const auto heatOnly = makePlan (input);
            expect (std::isfinite (heatOnly.effectiveGainDb) && std::isfinite (heatOnly.effectiveTargetLufs),
                    "a Heat-only allocation must not multiply infinite downstream capacity by zero");
            expect (heatOnly.effectiveGainDb <= 11.0f + 1.0e-3f,
                    "zero-capacity Heat alone cannot provide additional peak headroom");
        }

        //==============================================================================
        void testExcessLowersTarget()
        {
            beginTest ("Excess beyond the module limits lowers the target instead of squashing");

            // 依据：决策与校准第 4 节「超限就降低请求增益、重新求满足全部约束的方案」；GLAZE 验收总表第 3 条「超限时降低目标而不是硬压」。
            for (auto style : { Style::warm, Style::punchy, Style::clean })
            {
                const auto moderate = makeProfile (-22.0f, -13.0f);  // PLR 9：温和并联 Heat 不再承担旧式重削峰
                const auto peaky = makeProfile (-22.0f, -2.0f);      // PLR 20：同样的响度、峰值高得多

                const auto easy = planFor (&moderate, style, 100.0f);
                const auto hard = planFor (&peaky, style, 100.0f);

                expect (! easy.limitedByConstraints, utf8 ("PLR 9 的素材不该受约束"));
                expectEquals (easy.effectiveGainDb, easy.requestedGainDb);

                expect (hard.limitedByConstraints, utf8 ("PLR 20 的素材应受约束"));
                expect (hard.estimatedExcessDb > easy.estimatedExcessDb, utf8 ("峰值更高 → 超限估计更大"));
                expect (hard.effectiveGainDb < hard.requestedGainDb - 1.0f, utf8 ("超限时目标应降低"));
                expect (hard.effectiveTargetLufs < hard.requestedTargetLufs);
                expect (hard.effectiveGainDb < easy.effectiveGainDb, utf8 ("同样响度下，峰值高的素材推得更少"));
            }
        }

        //==============================================================================
        void testMaxGainFeedback()
        {
            beginTest ("Constraint feedback caps the net gain");

            // 依据：决策与校准第 4 节（整链实际结果验证方案 → 超限就降低请求增益）；PlanInputs::maxGainDb 与 effectiveGainDb 同一口径。
            const auto profile = makeProfile (-20.0f, -7.0f);

            for (auto style : { Style::warm, Style::punchy, Style::clean })
            {
                const auto base = planFor (&profile, style, 80.0f);
                expect (base.effectiveGainDb > 3.0f);

                const auto capped = planFor (&profile, style, 80.0f, defaultManual(), base.effectiveGainDb - 3.0f);
                expect (capped.effectiveGainDb <= base.effectiveGainDb - 3.0f + 1.0e-4f, utf8 ("净增益应不超过 maxGainDb"));
                expect (capped.limitedByConstraints);
                expect (! capped.limitedByManual);
                expect (capped.increments.limitGainDb <= base.increments.limitGainDb && capped.increments.colorHeat <= base.increments.colorHeat
                        && capped.increments.colorGlue <= base.increments.colorGlue && capped.increments.limitClip <= base.increments.limitClip,
                        utf8 ("约束反馈应让各模块一起退，而不是只砍一个"));

                const auto loose = planFor (&profile, style, 80.0f, defaultManual(), base.effectiveGainDb + 5.0f);
                expect (samePlan (loose, base), utf8 ("上限宽松时方案应与不设上限逐位相同"));

                // 上限低于手动链本身（这里手动中性 = 0 dB）：自动全部撤回，标受手动设置限制。
                const auto belowManual = planFor (&profile, style, 80.0f, defaultManual(), -1.0f);
                expect (belowManual.limitedByManual);
                expectEquals (belowManual.increments.limitGainDb, 0.0f);
                expectEquals (belowManual.increments.colorHeat, 0.0f);
                expectEquals (belowManual.increments.colorGlue, 0.0f);
                expectEquals (belowManual.increments.limitClip, 0.0f);

                const auto nan = planFor (&profile, style, 80.0f, defaultManual(), std::numeric_limits<float>::quiet_NaN());
                expectEquals (nan.increments.limitGainDb, 0.0f, utf8 ("maxGainDb 为 NaN 时不许再加电平"));
            }
        }

        //==============================================================================
        void testStyleDirections()
        {
            beginTest ("Styles lean on the modules the spec names");

            // 依据：决策与校准第 1 节：温润 Heat / Glue 为主、冲击 Clip 为主、通透 Limit 为主；自动 = 画像锁定权重的混合。
            const auto profile = makeProfile (-18.0f, -4.0f, 5.0f);
            const auto warm = planFor (&profile, Style::warm, 100.0f);
            const auto punchy = planFor (&profile, Style::punchy, 100.0f);
            const auto clean = planFor (&profile, Style::clean, 100.0f);

            // T-032 R6-01：出厂残差 Heat 表几乎不削峰，自动 Heat 恒为风格底量 × 强度（出厂三种风格都 12，风格差异待 Max 标定），
            // 原来的「温润 > 冲击 > 通透」只靠 HeatRow::inverse 把 Heat 顶到 heatDriveCap 的缺陷成立。风格间的 Heat 取向由驱动上限体现。
            const auto& fc = factoryConstants();
            expect (fc.warm.heatDriveCap > fc.punchy.heatDriveCap && fc.punchy.heatDriveCap > fc.clean.heatDriveCap,
                    utf8 ("Heat 驱动上限：温润 > 冲击 > 通透"));
            expect (warm.increments.colorHeat > 0.0f && warm.increments.colorHeat <= fc.warm.automaticHeatFloor + 1.0e-3f
                        && punchy.increments.colorHeat > 0.0f && punchy.increments.colorHeat <= fc.punchy.automaticHeatFloor + 1.0e-3f
                        && clean.increments.colorHeat > 0.0f && clean.increments.colorHeat <= fc.clean.automaticHeatFloor + 1.0e-3f,
                    utf8 ("Heat：三种风格都只给自己的底量（出厂表削不了峰）"));
            expect (warm.increments.colorGlue > clean.increments.colorGlue && warm.increments.colorGlue > punchy.increments.colorGlue,
                    utf8 ("Glue：温润最多"));
            expect (punchy.increments.limitClip > warm.increments.limitClip && punchy.increments.limitClip > clean.increments.limitClip,
                    utf8 ("Clip：冲击的削波阈值最低（参数最大）"));

            // 真实链上：限幅器的量通透最大、削波深度冲击最大。
            const auto music = atLufs (makeMusic (8.0), -18.0f);
            const auto measured = profileOf (music);
            const auto run = [&] (Style style)
            {
                const auto plan = planFor (&measured, style, 100.0f);
                return runChain (music, applyIncrements (defaultManual(), plan.increments));
            };

            const auto warmRun = run (Style::warm);
            const auto punchyRun = run (Style::punchy);
            const auto cleanRun = run (Style::clean);
            logMessage (utf8 ("  限幅峰值 GR 温润 / 冲击 / 通透：") + juce::String (warmRun.limitPeakDb, 2) + " / " + juce::String (punchyRun.limitPeakDb, 2)
                        + " / " + juce::String (cleanRun.limitPeakDb, 2) + utf8 (" dB；削波深度 ") + juce::String (warmRun.clipDepthMaxDb, 2) + " / "
                        + juce::String (punchyRun.clipDepthMaxDb, 2) + " / " + juce::String (cleanRun.clipDepthMaxDb, 2) + " dB");
            expect (cleanRun.limitPeakDb > warmRun.limitPeakDb && cleanRun.limitPeakDb > punchyRun.limitPeakDb, utf8 ("通透应以 Limit 为主"));
            expect (punchyRun.clipDepthMaxDb > warmRun.clipDepthMaxDb && punchyRun.clipDepthMaxDb > cleanRun.clipDepthMaxDb, utf8 ("冲击应以 Clip 为主"));
            expect (warmRun.glueMeanDb > punchyRun.glueMeanDb && warmRun.glueMeanDb > cleanRun.glueMeanDb, utf8 ("温润的 Glue 压得最多"));

            // 自动：锁定独热权重 = 对应的离散风格（逐位）；混合权重落在两者之间。
            const StyleWeights allWarm { 1.0f, 0.0f, 0.0f }, allClean { 0.0f, 0.0f, 1.0f }, half { 0.5f, 0.0f, 0.5f };
            expect (samePlan (planFor (&profile, Style::automatic, 100.0f, defaultManual(), 1.0e9f, &allWarm), warm));
            expect (samePlan (planFor (&profile, Style::automatic, 100.0f, defaultManual(), 1.0e9f, &allClean), clean));

            const auto mixed = planFor (&profile, Style::automatic, 100.0f, defaultManual(), 1.0e9f, &half);
            // T-032：底量三种风格相同时混合方案的 Heat 也是底量（blendConstants 对逐位相同的底量直接取原值）；上限按权重混合。
            expect (mixed.increments.colorHeat <= juce::jmax (warm.increments.colorHeat, clean.increments.colorHeat) + 1.0e-6f
                        && mixed.increments.colorHeat >= juce::jmin (warm.increments.colorHeat, clean.increments.colorHeat) - 1.0e-6f);
            expectWithinAbsoluteError (blendConstants (fc, half).heatDriveCap, 0.5f * (fc.warm.heatDriveCap + fc.clean.heatDriveCap), 1.0e-4f);
            expectWithinAbsoluteError (mixed.weights.warm, 0.5f, 1.0e-6f);
            expect (mixed.requestedGainDb < warm.requestedGainDb && mixed.requestedGainDb > clean.requestedGainDb,
                    utf8 ("混合风格的偏好响度应在两种风格之间"));
        }

        //==============================================================================
        // T-032 R6-01（算法 8）：自动 Heat 只承担换算表里该电平、上限以内真能削掉的峰均比；削不到 heatUsefulCrestDb 就只留风格底量。
        void testHeatOnlyClaimsRealCrestReduction()
        {
            beginTest ("Automatic Heat only takes the crest reduction the Heat table can deliver (algorithm 8)");

            // 依据：设计规格-GLAZE 7.1「自动 Heat 12（底量）」、7.4「Heat 的削峰份额改为可选贡献」；决策与校准第 4 节「约束是上限，
            //       不是必须花完的额度」、第 1 节对 v0.4「与音乐无关」的批评。出厂残差 Heat 表几乎不削峰（crest 列 ≤ 0.00023 dB），
            //       修前 HeatRow::inverse「达不到就给表的最大 Heat」让自动 Heat 只要超限估计 > 0 就顶到 heatDriveCap（70 / 50 / 20）。
            const auto& c = factoryConstants();

            // 1. 性质扫描：3 风格 × I × PLR × 置信度 × GLAZE。自动 Heat ≤ 风格底量 × GLAZE × κ；两份方案都不受约束时（强度 = GLAZE × κ
            //    落网格），与「换算表的响度 / 峰值两列清零」逐位相同——削峰那条路根本没走，Heat 与表里 0.0002 dB 级的残差无关。
            auto flatTable = c;
            flatTable.heatLoudnessDb.fill (0.0f);
            flatTable.heatPeakDb.fill (0.0f);

            int cases = 0, overFloor = 0, compared = 0, differs = 0, withoutFloor = 0;
            float highest = 0.0f;

            for (auto style : { Style::warm, Style::punchy, Style::clean })
                for (auto lufs : { -24.0f, -18.0f, -14.0f, -10.0f, -7.0f })
                    for (auto plr : { 8.0f, 12.0f, 16.0f })
                        for (auto confidence : { 1.0f, 0.5f })
                            for (auto glaze : { 35.0f, 100.0f })
                            {
                                const auto profile = makeProfile (lufs, lufs + plr, 5.0f, confidence);
                                const auto bound = constantsFor (c, style).automaticHeatFloor * glaze * 0.01f * confidenceScale (&profile, c);

                                PlanInputs in;
                                in.profile = &profile;
                                in.style = style;
                                in.glazePercent = glaze;
                                in.manual = defaultManual();
                                in.constants = &c;
                                const auto plan = makePlan (in);
                                in.constants = &flatTable;
                                const auto flat = makePlan (in);

                                ++cases;
                                highest = juce::jmax (highest, plan.increments.colorHeat);
                                overFloor += plan.increments.colorHeat <= bound + 1.0e-3f ? 0 : 1;

                                if (! plan.limitedByConstraints && ! flat.limitedByConstraints)
                                {
                                    ++compared;
                                    differs += sameBits (plan.increments.colorHeat, flat.increments.colorHeat) ? 0 : 1;
                                    withoutFloor += plan.increments.colorHeat > 0.0f ? 0 : 1;
                                }
                            }

            logMessage (utf8 ("  Heat 扫描：") + juce::String (cases) + utf8 (" 份方案，最大自动 Heat ") + juce::String (highest, 3)
                        + utf8 ("，其中 ") + juce::String (compared) + utf8 (" 份两边都不受约束、逐位对照"));
            expectEquals (overFloor, 0, utf8 ("自动 Heat 不应超过风格底量 × GLAZE × κ（修前顶到 heatDriveCap）"));
            expect (compared >= cases / 4, utf8 ("扫描里要有足够多不受约束的方案可比"));
            expectEquals (differs, 0, utf8 ("出厂表削不了峰：Heat 应与换算表清零时逐位相同"));
            expectEquals (withoutFloor, 0, utf8 ("Color 开着、GLAZE > 0：底量照给"));

            // 2. 超限估计过 0 处连续（修前温润、I −24：TP −16.6 → Heat 12、TP −16.4 → 70，一步跳 58 个单位；GLAZE 35 时 4.2 → 24.5）。
            for (auto glaze : { 35.0f, 100.0f })
            {
                float previous = 0.0f, largestStep = 0.0f;

                for (int i = 0; i <= 60; ++i)
                {
                    const auto profile = makeProfile (-24.0f, -16.8f + 0.01f * (float) i);
                    const auto heat = planFor (&profile, Style::warm, glaze).increments.colorHeat;

                    if (i > 0)
                        largestStep = juce::jmax (largestStep, std::abs (heat - previous));

                    previous = heat;
                }

                logMessage (utf8 ("  温润 I −24，TP −16.8 → −16.2 扫描，GLAZE ") + juce::String (glaze, 0) + utf8 ("：相邻两步 Heat 最大变化 ")
                            + juce::String (largestStep, 4));
                expect (largestStep <= 0.01f, utf8 ("超限估计过 0 时自动 Heat 不许跳变"));
            }

            // 3. 版本门是唯一开关：同一份出厂常数标成算法 7（v0.1.7.0 ～ v0.1.8.5 存的快照），方案走旧分支、Heat 照旧顶到上限（旧工程
            //    逐位不变；改前改后的摘要对照见 IMPL-A7 报告）；旧分支不读 heatUsefulCrestDb（旧快照读成 0 也一样）。算法 8 的坏快照里
            //    门槛 ≤ 0、非有限都按「削不了」，不会退回顶到上限。
            {
                auto v7 = c;
                v7.version = 7;
                v7.algorithmVersion = 7;
                const auto hot = makeProfile (-18.0f, -6.0f);

                PlanInputs in;
                in.profile = &hot;
                in.style = Style::warm;
                in.glazePercent = 100.0f;
                in.manual = defaultManual();
                in.constants = &v7;
                const auto legacy = makePlan (in);
                expect (legacy.increments.colorHeat > c.warm.automaticHeatFloor + 10.0f, utf8 ("算法 7 的快照照旧（旧分支原样）"));

                auto v7WithoutThreshold = v7;
                v7WithoutThreshold.heatUsefulCrestDb = 0.0f;
                in.constants = &v7WithoutThreshold;
                expect (samePlan (makePlan (in), legacy), utf8 ("旧分支不看 heatUsefulCrestDb"));

                in.constants = &c;
                const auto current = makePlan (in);
                expect (current.increments.colorHeat > 0.0f && current.increments.colorHeat <= c.warm.automaticHeatFloor + 1.0e-3f,
                        utf8 ("算法 8：同一素材只给底量"));

                for (auto threshold : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
                {
                    auto broken = c;
                    broken.heatUsefulCrestDb = threshold;
                    in.constants = &broken;
                    expect (makePlan (in).increments.colorHeat <= c.warm.automaticHeatFloor + 1.0e-3f,
                            utf8 ("门槛 ≤ 0 或非有限按「削不了」：") + juce::String (threshold));
                }
            }

            // 4. 换算表真能削峰时，新规则照常让 Heat 承担它那一份（不是一律只给底量），而且与旧规则逐位相同（表能交付份额时两者本来
            //    一致）；门槛抬到这张表能削的量之上 → 只留底量。人为的削峰型表：峰值随 Heat 线性下降、Heat 100 削 1 dB，响度不变。
            {
                auto shaving = c;

                for (int level = 0; level < heatTableLevels; ++level)
                    for (int j = 0; j < heatTableAmounts; ++j)
                    {
                        const auto index = (size_t) (level * heatTableAmounts + j);
                        shaving.heatLoudnessDb[index] = 0.0f;
                        shaving.heatPeakDb[index] = -0.01f * shaving.heatTableAmount[(size_t) j];
                    }

                auto shavingV7 = shaving;
                shavingV7.version = 7;
                shavingV7.algorithmVersion = 7;
                auto shavingAboveThreshold = shaving;
                shavingAboveThreshold.heatUsefulCrestDb = 5.0f;

                const auto hot = makeProfile (-18.0f, -6.0f);
                PlanInputs in;
                in.profile = &hot;
                in.style = Style::warm;
                in.glazePercent = 100.0f;
                in.manual = defaultManual();
                in.constants = &shaving;
                const auto withShaving = makePlan (in);
                in.constants = &shavingV7;
                const auto oldRule = makePlan (in);
                in.constants = &shavingAboveThreshold;
                const auto aboveThreshold = makePlan (in);

                logMessage (utf8 ("  削峰型表：温润 I −18 / TP −6，Heat 新规则 ") + juce::String (withShaving.increments.colorHeat, 3)
                            + utf8 ("、旧规则 ") + juce::String (oldRule.increments.colorHeat, 3) + utf8 ("、门槛 5 dB ")
                            + juce::String (aboveThreshold.increments.colorHeat, 3));
                expect (withShaving.increments.colorHeat > c.warm.automaticHeatFloor + 1.0f, utf8 ("表能削峰时 Heat 照常承担它那一份"));
                expect (withShaving.increments.colorHeat < c.warm.heatDriveCap - 1.0f, utf8 ("只承担份额，不顶到上限"));
                expect (sameIncrements (withShaving.increments, oldRule.increments), utf8 ("表能交付份额时新旧规则逐位相同"));
                expect (aboveThreshold.increments.colorHeat <= c.warm.automaticHeatFloor + 1.0e-3f, utf8 ("能削的量不到门槛：只留底量"));
            }

            // 5. 底量是风格常数（算法 8 起；出厂三种风格都 12，风格差异待 Max 试听标定）：标成 12 / 8 / 4 时各风格照自己的底量给，
            //    温润 > 冲击 > 通透；混合权重按权重混合。
            {
                auto tuned = c;
                tuned.warm.automaticHeatFloor = 12.0f;
                tuned.punchy.automaticHeatFloor = 8.0f;
                tuned.clean.automaticHeatFloor = 4.0f;
                const auto profile = makeProfile (-24.0f, -16.0f);

                const auto planOf = [&] (Style style, const StyleWeights* locked)
                {
                    PlanInputs in;
                    in.profile = &profile;
                    in.style = style;
                    in.glazePercent = 100.0f;
                    in.manual = defaultManual();
                    in.constants = &tuned;
                    in.lockedWeights = locked;
                    return makePlan (in);
                };

                const StyleWeights half { 0.5f, 0.0f, 0.5f };
                const auto warm = planOf (Style::warm, nullptr);
                const auto punchy = planOf (Style::punchy, nullptr);
                const auto clean = planOf (Style::clean, nullptr);
                const auto mixed = planOf (Style::automatic, &half);
                expect (! warm.limitedByConstraints && ! punchy.limitedByConstraints && ! clean.limitedByConstraints && ! mixed.limitedByConstraints,
                        utf8 ("这份素材不受约束：强度都是 1"));
                expectEquals (warm.increments.colorHeat, 12.0f);
                expectEquals (punchy.increments.colorHeat, 8.0f);
                expectEquals (clean.increments.colorHeat, 4.0f);
                expectWithinAbsoluteError (mixed.increments.colorHeat, 8.0f, 1.0e-4f, utf8 ("半温润半通透：底量按权重混合"));
            }
        }

        //==============================================================================
        // T-032 R6-06（算法 8）：满强度增益不超过 Limit Gain 量程（24 − 手动 Gain）能交付的净增益。
        void testGainRangeBoundsEffectiveGain()
        {
            beginTest ("The planned net gain never claims more than the Limit Gain range (24 dB minus manual Gain) can deliver (algorithm 8)");

            // 依据：GlazePlanner.h「effectiveGainDb … 校准器用它作目标净提升」；参数范围 Limit Gain 0 ～ 24 dB（applyIncrements 钳位）。
            // 修前 G_full′ 不受量程约束：I −40 LUFS 请求 30 dB 时目标照写，Gain 实际钳在 24，校准目标永远追不上（每首都「部分校准」）；
            // 手动 Gain + 自动请求 > 24 dB 同样虚报。
            // 「能交付的净增益」= 同一前向模型下、实际交给模块的参数（手动 + 自动增量，钳位之后）的整链预计净增益——GLAZE 0 的方案
            // 就是「手动链的预计净增益」，把实际参数当手动值喂进去即得（只用公开接口，不复制决策器的内部算式）。
            const auto deliverable = [] (const MusicProfile& profile, Style style, const ChainParams& manual, const Plan& plan)
            {
                return planFor (&profile, style, 0.0f, applyIncrements (manual, plan.increments)).effectiveGainDb;
            };

            // 1. 请求放不进量程的两类：极安静素材（手动中性，请求 > 24 dB）与手动 Gain + 自动请求 > 24 dB。后一类取峰值低、超限估计 0
            //    的素材（没有自动 Glue 补偿、整链不进限幅器）：这样满强度就用满量程，核对的只是量程这一条（手动 Gain 把峰值推进限幅器时
            //    强度另被约束压低，那是既有的约束核对，不在本条）。
            struct Case { float lufs, truePeak, manualGainDb; };
            const Case ranged[] = { { -40.0f, -28.0f, 0.0f }, { -46.0f, -34.0f, 0.0f }, { -52.0f, -40.0f, 0.0f },
                                    { -30.0f, -26.0f, 12.0f }, { -36.0f, -32.0f, 6.0f }, { -28.0f, -26.0f, 18.0f } };
            int atRange = 0;

            for (const auto& k : ranged)
                for (auto style : { Style::warm, Style::punchy, Style::clean })
                {
                    const auto profile = makeProfile (k.lufs, k.truePeak);
                    auto manual = defaultManual();
                    manual.limit.gainDb = k.manualGainDb;
                    const auto plan = planFor (&profile, style, 100.0f, manual);
                    const auto actual = applyIncrements (manual, plan.increments);
                    const auto delivered = deliverable (profile, style, manual, plan);
                    const auto full = actual.limit.gainDb >= 24.0f - 1.0e-3f;
                    atRange += full ? 1 : 0;

                    logMessage (utf8 ("  I ") + juce::String (k.lufs, 0) + " / TP " + juce::String (k.truePeak, 0) + utf8 ("，手动 Gain ")
                                + juce::String (k.manualGainDb, 0) + utf8 ("，风格 ") + juce::String ((int) style) + utf8 ("：请求 ")
                                + juce::String (plan.requestedGainDb, 2) + utf8 ("、目标 ") + juce::String (plan.effectiveGainDb, 2)
                                + utf8 (" dB，实际 Limit Gain ") + juce::String (actual.limit.gainDb, 2) + utf8 ("、能交付 ")
                                + juce::String (delivered, 2) + utf8 (" dB，受约束 ") + juce::String ((int) plan.limitedByConstraints));

                    expect (full, utf8 ("这些素材满强度应用满 Limit Gain 量程"));
                    expect (plan.limitedByConstraints, utf8 ("请求放不进量程应标受约束"));
                    expect (plan.effectiveGainDb <= delivered + 0.5f, utf8 ("目标净提升不应超过量程能交付的净增益"));
                    expect (plan.effectiveGainDb < plan.requestedGainDb, utf8 ("目标低于请求"));
                }

            expectEquals (atRange, 18);

            // 2. 量程以内（I −30 / TP −24、手动中性：请求 20 dB，超限估计 0）：新规则不起作用——不受约束、目标 = 请求，与修前一样。
            {
                const auto profile = makeProfile (-30.0f, -24.0f);
                const auto plan = planFor (&profile, Style::clean, 100.0f);
                expect (! plan.limitedByConstraints, utf8 ("量程以内不受约束"));
                expect (sameBits (plan.effectiveGainDb, plan.requestedGainDb), utf8 ("量程以内目标 = 请求（逐位）"));
                expect (applyIncrements (defaultManual(), plan.increments).limit.gainDb < 24.0f - 1.0f);
            }

            // 3. 连续：手动 Gain 从 0 扫到 16 dB（I −30 / TP −26，通透：请求 20 dB），跨过「请求刚好放满量程」的边界时目标不跳；
            //    目标随手动 Gain 单调不减（量程每少 1 dB、手动多 1 dB，净增益最多持平）。
            {
                const auto profile = makeProfile (-30.0f, -26.0f);
                float previous = 0.0f, largestStep = 0.0f;
                bool monotone = true;

                for (int i = 0; i <= 64; ++i)
                {
                    auto manual = defaultManual();
                    manual.limit.gainDb = 0.25f * (float) i;
                    const auto target = planFor (&profile, Style::clean, 100.0f, manual).effectiveGainDb;

                    if (i > 0)
                    {
                        largestStep = juce::jmax (largestStep, std::abs (target - previous));
                        monotone = monotone && target >= previous - 1.0e-3f;
                    }

                    previous = target;
                }

                logMessage (utf8 ("  手动 Gain 0 → 16 dB 扫描（每步 0.25 dB）：目标相邻两步最大变化 ") + juce::String (largestStep, 4) + " dB");
                expect (largestStep <= 0.3f, utf8 ("跨过量程边界时目标不跳"));
                expect (monotone, utf8 ("目标随手动 Gain 单调不减"));
            }

            // 4. 任务书原例（I −24 / 手动 Gain 12 / 通透）只记录：真实峰值下手动 Gain 已把峰值推近限幅器，强度由既有的约束核对压低、
            //    满强度用不到量程顶；目标与能交付量之差来自「手动链净增益 + 强度 × G_full′」的叠加近似（校准器收口），与量程无关。
            {
                const auto profile = makeProfile (-24.0f, -14.0f);
                auto manual = defaultManual();
                manual.limit.gainDb = 12.0f;
                const auto plan = planFor (&profile, Style::clean, 100.0f, manual);
                logMessage (utf8 ("  任务书原例 I −24 / TP −14 / 手动 Gain 12：目标 ") + juce::String (plan.effectiveGainDb, 2) + utf8 (" dB，实际 Limit Gain ")
                            + juce::String (applyIncrements (manual, plan.increments).limit.gainDb, 2) + utf8 ("，能交付 ")
                            + juce::String (deliverable (profile, Style::clean, manual, plan), 2) + utf8 (" dB，受约束 ")
                            + juce::String ((int) plan.limitedByConstraints));
                expect (applyIncrements (manual, plan.increments).limit.gainDb <= 24.0f);
            }
        }

        //==============================================================================
        void testGlueLowFrequencyRisk()
        {
            beginTest ("Intermittent heavy lows ease the automatic Glue, sustained bass does not");

            // 依据：分析器第 4 节「低频占比对 Glue 只作风险提示，与低频随时间的变化一起判断；持续低音与间歇重底鼓不走同一策略」；
            // GlazeConstants「Glue 的低频风险」：风险 = 占比坡度 × 起伏坡度 × 可信度，自动 Glue 份额 × (1 − 0.5 × 风险)。
            const auto base = makeProfile (-18.0f, -4.0f, 5.0f);   // 低频占比 0.3、没有低频起伏特征：没有风险

            auto sustained = base;
            setFeature (sustained.lowShare, 0.6f, 1.0f);
            setFeature (sustained.lowBandRangeDb, 3.0f, 1.0f);

            auto kicks = sustained;
            setFeature (kicks.lowBandRangeDb, 18.0f, 1.0f);

            auto lightLows = kicks;
            setFeature (lightLows.lowShare, 0.2f, 1.0f);

            auto unsure = kicks;
            unsure.lowBandRangeDb.confidence = 0.0f;

            const auto baseline = planFor (&base, Style::warm, 100.0f);
            const auto kickPlan = planFor (&kicks, Style::warm, 100.0f);

            expect (baseline.increments.colorGlue > 0.0f, utf8 ("温润满 GLAZE 在这份素材上会用自动 Glue"));
            expect (samePlan (planFor (&sustained, Style::warm, 100.0f), baseline), utf8 ("持续低音：与没有低频特征时方案逐位相同"));
            expect (samePlan (planFor (&lightLows, Style::warm, 100.0f), baseline), utf8 ("起伏大但低频不重：不折"));
            // 起伏特征不可信：风险本身按可信度收小，但方案还要受「置信度拉满的同一份方案」封顶（v0.6 验收 F-05），
            // 所以不会比可信时更激进——Glue 与可信时一样被折。
            expect (planFor (&unsure, Style::warm, 100.0f).increments.colorGlue <= planFor (&kicks, Style::warm, 100.0f).increments.colorGlue + 1.0e-4f,
                    utf8 ("起伏特征不可信：不比可信时用更多 Glue"));
            expect (kickPlan.increments.colorGlue < baseline.increments.colorGlue,
                    utf8 ("间歇重底鼓：自动 Glue 变轻（") + juce::String (baseline.increments.colorGlue, 2) + " -> "
                        + juce::String (kickPlan.increments.colorGlue, 2) + ")");
            expect (kickPlan.increments.colorGlue > 0.0f, utf8 ("只打折不关掉：Glue 仍按风格给密度"));

            // 折扣与 GLAZE 无关：随 GLAZE 单调照旧成立。
            auto previous = -1.0f;
            auto monotonic = true;

            for (int step = 0; step <= 100; step += 5)
            {
                const auto glue = planFor (&kicks, Style::warm, (float) step).increments.colorGlue;
                monotonic = monotonic && glue >= previous;
                previous = glue;
            }

            expect (monotonic, utf8 ("间歇重底鼓时 Glue 增量仍随 GLAZE 单调不减"));
        }

        //==============================================================================
        // 依据：决策与校准第 2 节「置信度低时正向增益与处理量按比例收紧」；v0.6 验收 F-05（随机手动值 + 置信度封顶 0.75、
        //       即无播放头宿主的情形，净增益曾比满置信度多 22 dB）。同一份画像、同一风格：置信度随机压低后，每一项增量与
        //       净增益都不超过置信度拉满时的方案。
        void testConfidenceNeverMoreAggressive()
        {
            beginTest ("A less confident profile never plans more than the same profile fully trusted");

            juce::Random rng (606);
            int checked = 0, violations = 0;
            auto worstGainDb = 0.0f;

            for (int trial = 0; trial < 300; ++trial)
            {
                const auto lufs = -36.0f + 28.0f * rng.nextFloat();
                const auto truePeak = juce::jmin (6.0f, lufs + 4.0f + 16.0f * rng.nextFloat());
                const auto confidence = 0.75f * rng.nextFloat();
                auto unsure = makeProfile (lufs, truePeak, 2.0f + 14.0f * rng.nextFloat(), confidence);
                setFeature (unsure.lowShare, rng.nextFloat() * 0.8f, confidence);
                setFeature (unsure.lowBandRangeDb, rng.nextFloat() * 25.0f, confidence);
                setFeature (unsure.spectralSlopeDbPerOct, -6.0f + 5.0f * rng.nextFloat(), confidence);

                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    unsure.tameEvidenceDb[b] = rng.nextFloat() * 2.0f;
                    unsure.recoverEvidenceDb[b] = rng.nextFloat() * 2.0f;
                }

                auto sure = unsure;

                for (auto* f : { &sure.integratedLufs, &sure.loudnessRangeLu, &sure.plrDb, &sure.truePeakDbtp, &sure.spectralSlopeDbPerOct,
                                 &sure.lowShare, &sure.highShare, &sure.lowBandRangeDb, &sure.onsetDensityPerSecond, &sure.onsetStrengthDb })
                    f->confidence = 1.0f;

                for (auto& band : sure.bandConfidence)
                    band = 1.0f;

                sure.representativeness = 1.0f;

                auto manual = defaultManual();
                manual.color.glue = rng.nextBool() ? 0.0f : 100.0f * rng.nextFloat();
                manual.color.heat = rng.nextBool() ? 0.0f : 60.0f * rng.nextFloat();
                manual.limit.gainDb = rng.nextBool() ? 0.0f : 10.0f * rng.nextFloat();
                manual.limit.clip = rng.nextBool() ? 0.0f : 60.0f * rng.nextFloat();

                // 含「自动、权重没锁」（首次聆听中的候选方案）：它的权重随置信度重新判断，也不许超过满置信度时的方案。
                for (auto style : { Style::warm, Style::punchy, Style::clean, Style::automatic })
                {
                    const auto glaze = 100.0f * rng.nextFloat();
                    const auto low = planFor (&unsure, style, glaze, manual);
                    const auto high = planFor (&sure, style, glaze, manual);
                    const auto& a = low.increments;
                    const auto& b = high.increments;
                    ++checked;

                    const bool ok = low.effectiveGainDb <= high.effectiveGainDb + 1.0e-4f
                                 && a.colorGlue <= b.colorGlue + 1.0e-4f && a.colorHeat <= b.colorHeat + 1.0e-4f
                                 && a.limitGainDb <= b.limitGainDb + 1.0e-4f && a.limitClip <= b.limitClip + 1.0e-4f
                                 && std::abs (a.colorToneDb) <= std::abs (b.colorToneDb) + 1.0e-5f && a.colorToneDb * b.colorToneDb >= 0.0f
                                 && a.balanceRecover <= b.balanceRecover + 1.0e-4f
                                 && a.balanceTame <= b.balanceTame + 1.0e-4f;

                    if (! ok)
                    {
                        ++violations;
                        worstGainDb = juce::jmax (worstGainDb, low.effectiveGainDb - high.effectiveGainDb);
                    }
                }
            }

            logMessage ("  " + juce::String (checked) + " comparisons, " + juce::String (violations) + " where the unsure plan did more (worst net gain +"
                        + juce::String (worstGainDb, 2) + " dB)");
            expectEquals (violations, 0, utf8 ("置信度低的方案不许比置信度满时更激进"));
        }

        //==============================================================================
        // 依据：决策第 8 节「算法值为底，旋钮是微调」；v0.6 验收 F-02（手动 Heat 接近风格上限时，所有模块共用的强度被一起压低，
        //       越过上限的那一刻又一步加回：净增益 21.8 → 1.0 → 21.8 dB）。Heat / Glue 的上限只收它们自己的增量，
        //       所以手动旋钮从 0 扫到 100，净增益一格一格地走，不会跳。
        void testManualSweepIsContinuous()
        {
            beginTest ("Turning a manual Heat or Glue knob never makes the planned loudness jump");

            // 另外两条（钉住「逐模块钳住」本身）：被扫的那个模块，自动增量只随手动值往下走、每格最多少 1.5（自动只填
            // 「上限 − 手动值」的余量：余量连续地降到 0，而不是先不动、到撤回点一步归零）；手动 + 自动 Heat 不超过风格的驱动上限。
            const auto& c = factoryConstants();
            const std::pair<float, float> materials[] = { { -32.5f, -14.0f }, { -22.0f, -8.0f }, { -14.0f, -2.0f } };
            auto worstStepDb = 0.0f, worstIncrementDrop = 0.0f, worstIncrementRise = 0.0f, worstHeatOverCap = 0.0f;
            juce::String worstWhere;

            for (const auto& levels : materials)
                for (auto style : { Style::warm, Style::punchy, Style::clean })
                    for (int knob = 0; knob < 2; ++knob)
                    {
                        const auto profile = makeProfile (levels.first, levels.second, 5.0f);
                        const auto driveCap = constantsFor (c, style).heatDriveCap;
                        auto previous = std::numeric_limits<float>::quiet_NaN();
                        auto previousIncrement = std::numeric_limits<float>::quiet_NaN();

                        for (int value = 0; value <= 100; ++value)
                        {
                            auto manual = defaultManual();
                            (knob == 0 ? manual.color.heat : manual.color.glue) = (float) value;
                            const auto plan = planFor (&profile, style, 100.0f, manual);
                            const auto net = plan.effectiveGainDb;
                            const auto increment = knob == 0 ? plan.increments.colorHeat : plan.increments.colorGlue;

                            if (std::isfinite (previous) && std::abs (net - previous) > worstStepDb)
                            {
                                worstStepDb = std::abs (net - previous);
                                worstWhere = juce::String (levels.first, 1) + " LUFS, style " + juce::String ((int) style)
                                           + (knob == 0 ? ", Heat " : ", Glue ") + juce::String (value);
                            }

                            if (std::isfinite (previousIncrement))
                            {
                                worstIncrementDrop = juce::jmax (worstIncrementDrop, previousIncrement - increment);
                                worstIncrementRise = juce::jmax (worstIncrementRise, increment - previousIncrement);
                            }

                            if (knob == 0 && (float) value < driveCap)
                                worstHeatOverCap = juce::jmax (worstHeatOverCap, (float) value + plan.increments.colorHeat - driveCap);

                            previous = net;
                            previousIncrement = increment;
                        }
                    }

            logMessage ("  largest net-gain step per knob unit: " + juce::String (worstStepDb, 3) + " dB (" + worstWhere + "); swept module's auto increment "
                        + "drops at most " + juce::String (worstIncrementDrop, 3) + ", rises at most " + juce::String (worstIncrementRise, 3)
                        + " per unit; manual + auto Heat over the drive cap by at most " + juce::String (worstHeatOverCap, 3));
            expect (worstStepDb <= 1.0f, utf8 ("手动 Heat / Glue 每动一格，方案净增益的变化不超过 1 dB"));
            expect (worstIncrementDrop <= 1.5f, utf8 ("被扫模块的自动增量连续地让位（每格最多少 1.5），不在撤回点一步归零"));
            expect (worstIncrementRise <= 1.0e-3f, utf8 ("手动值越大，同一模块的自动增量不会反而变多"));
            expect (worstHeatOverCap <= 0.01f, utf8 ("手动 + 自动 Heat 不超过风格的驱动上限"));
        }

        //==============================================================================
        // 依据：GlazePlanner.cpp clipRampFor「最小爬坡」；v0.6 验收 F-06（输入峰值已超 Ceiling 的热素材，GLAZE 离开 0 的第一格
        //       Clip 增量就一步到位 79，削波深度 0 → 7.4 dB）。
        void testClipRampsFromGlazeZero()
        {
            beginTest ("On hot material Clip ramps in from GLAZE 0 instead of jumping");

            const auto hot = makeProfile (-4.5f, 9.5f, 4.0f);
            auto previous = 0.0f, worstStep = 0.0f;

            for (int tenth = 1; tenth <= 100; ++tenth)   // GLAZE 0.1 % ～ 10 %，宿主能送的最小步长 0.1 %
            {
                const auto clip = planFor (&hot, Style::punchy, 0.1f * (float) tenth).increments.limitClip;
                worstStep = juce::jmax (worstStep, clip - previous);
                previous = clip;
            }

            logMessage ("  largest Clip step per 0.1 % of GLAZE on hot material: " + juce::String (worstStep, 2));
            expect (planFor (&hot, Style::punchy, 0.1f).increments.limitClip <= 5.0f, utf8 ("GLAZE 0.1 % 时 Clip 只动一点"));
            expect (worstStep <= 5.0f, utf8 ("GLAZE 每走 0.1 %，Clip 最多动 5"));
        }

        //==============================================================================
        void testLowConfidenceTightens()
        {
            beginTest ("Low confidence tightens gain and processing");

            // 依据：决策与校准第 2 节「置信度低时，正向增益与校准额度一起按比例收紧」；分析器第 6 节「置信不足时向保守默认值收拢」；
            //       时间线第 2 节「未学习：低置信度方案，随学习逐步放开」。
            const auto& c = factoryConstants();

            const std::pair<float, float> materials[] = { { -22.0f, -9.0f }, { -18.0f, -5.0f }, { -14.0f, -2.0f }, { -20.0f, -9.0f }, { -16.0f, 0.0f } };

            for (const auto& levels : materials)
                for (auto style : { Style::warm, Style::punchy, Style::clean })
                {
                    auto sure = makeProfile (levels.first, levels.second, 5.0f, 1.0f);
                    setFeature (sure.spectralSlopeDbPerOct, -4.5f);

                    for (size_t b = 0; b < (size_t) numBands; ++b)
                        sure.tameEvidenceDb[b] = 0.8f;

                    Plan previous = planFor (&sure, style, 100.0f);

                    for (auto confidence : { 0.6f, 0.3f, 0.1f, 0.0f })
                    {
                        auto unsure = makeProfile (levels.first, levels.second, 5.0f, confidence);
                        setFeature (unsure.spectralSlopeDbPerOct, -4.5f, confidence);

                        for (size_t b = 0; b < (size_t) numBands; ++b)
                            unsure.tameEvidenceDb[b] = 0.8f;

                        const auto plan = planFor (&unsure, style, 100.0f);
                        expect (plan.effectiveGainDb <= previous.effectiveGainDb + 1.0e-3f, utf8 ("置信度越低，正向净增益不应越大"));
                        expect (plan.increments.colorGlue <= previous.increments.colorGlue + 1.0e-3f);
                        expect (plan.increments.colorToneDb <= previous.increments.colorToneDb + 1.0e-4f);
                        expect (plan.increments.balanceTame <= previous.increments.balanceTame + 1.0e-3f);
                        previous = plan;
                    }

                    // 置信度为 0 时只剩底强度：净增益不超过「保守默认输入响度」下请求的 unlearnedStrength 倍。
                    const auto sc = constantsFor (c, style);
                    const auto floorGain = c.unlearnedStrength * juce::jmax (0.0f, sc.preferredLufs - juce::jmax (levels.first, c.unlearnedInputLufs));
                    expect (previous.effectiveGainDb <= floorGain + 1.0e-3f, utf8 ("置信度 0 的方案应被收紧到底强度"));
                }

            // 未学习（没有画像）：全程固定的保守方案——正向、很小、只取决于常数。
            const auto unlearned = planFor (nullptr, Style::automatic, 100.0f);
            const auto expected = c.unlearnedStrength * (c.clean.preferredLufs - c.unlearnedInputLufs);
            expect (unlearned.effectiveGainDb > 0.0f && unlearned.effectiveGainDb <= expected + 1.0e-3f, utf8 ("未学习时应是小而正的保守方案"));
            expectEquals (unlearned.weights.clean, 1.0f, utf8 ("未学习的自动风格应是通透"));
            expect (samePlan (unlearned, planFor (nullptr, Style::automatic, 100.0f)), utf8 ("保守方案应固定不变"));

            // 强度系数 κ（编排器给校准器的修正范围也用它）：未学习 = 底强度，满置信 = 1，中间单调。
            expectEquals (confidenceScale (nullptr, c), c.unlearnedStrength);
            const auto sureProfile = makeProfile (-20.0f, -7.0f, 5.0f, 1.0f);
            const auto halfProfile = makeProfile (-20.0f, -7.0f, 5.0f, 0.4f);
            expectEquals (confidenceScale (&sureProfile, c), 1.0f);
            expect (confidenceScale (&halfProfile, c) > c.unlearnedStrength && confidenceScale (&halfProfile, c) < 1.0f);

            // 代表性（同一段反复循环不增加）同样收紧。
            auto looped = makeProfile (-20.0f, -7.0f);
            const auto full = planFor (&looped, Style::clean, 100.0f);
            looped.representativeness = 0.0f;
            expect (planFor (&looped, Style::clean, 100.0f).effectiveGainDb < full.effectiveGainDb - 1.0f);
        }

        //==============================================================================
        void testManualLimits()
        {
            beginTest ("Manual settings beyond a limit withdraw the auto part");

            // 依据：决策与校准第 8 节「手动部分本身已超约束时，自动先撤回自己的增量，界面显示受手动设置限制，不偷偷反向改旋钮」；
            //       「绝不自动抵消你的修改」。
            const auto profile = makeProfile (-18.0f, -5.0f);

            // 手动 Heat 超过当前温润驱动上限：Heat 自动增量撤回，手动值原样。
            auto heavyHeat = defaultManual();
            heavyHeat.color.heat = factoryConstants().warm.heatDriveCap + 10.0f;
            const auto heatPlan = planFor (&profile, Style::warm, 100.0f, heavyHeat);
            expect (heatPlan.limitedByManual, utf8 ("手动 Heat 超上限应标受手动限制"));
            expectEquals (heatPlan.increments.colorHeat, 0.0f);
            expectEquals (applyIncrements (heavyHeat, heatPlan.increments).color.heat, heavyHeat.color.heat, utf8 ("不应反向改手动 Heat"));
            expect (heatPlan.increments.limitGainDb > 0.0f, utf8 ("其它模块照常工作"));
            // T-032 R6-02：只撤回 Heat 的自动增量、自动 Gain 仍在推响 → 电平类自动没有全部撤回，编排器的保护层 / 校准照常。
            expect (! heatPlan.automaticLevelWithdrawn, utf8 ("手动 Heat 超染色预算不算电平类自动全部撤回"));

            // 手动 Gain 12 dB 推一个热混音：限幅器只靠手动就超上限 → 所有动电平的自动增量撤回，手动 Gain 不减。
            const auto hot = makeProfile (-12.0f, -2.0f);
            auto heavyGain = defaultManual();
            heavyGain.limit.gainDb = 12.0f;
            const auto gainPlan = planFor (&hot, Style::punchy, 100.0f, heavyGain);
            expect (gainPlan.limitedByManual);
            expect (gainPlan.automaticLevelWithdrawn, utf8 ("手动已让限幅器超约束：电平类自动全部撤回"));
            expectEquals (gainPlan.increments.limitGainDb, 0.0f);
            expectEquals (gainPlan.increments.colorHeat, 0.0f);
            expectEquals (gainPlan.increments.colorGlue, 0.0f);
            expectEquals (gainPlan.increments.limitClip, 0.0f);
            expectEquals (applyIncrements (heavyGain, gainPlan.increments).limit.gainDb, 12.0f);

            // 手动 Glue 100 压一个响的素材（GR 远超上限）：Glue 自动撤回。
            const auto loud = makeProfile (-10.0f, -1.5f);
            auto heavyGlue = defaultManual();
            heavyGlue.color.glue = 100.0f;
            const auto gluePlan = planFor (&loud, Style::warm, 100.0f, heavyGlue);
            expect (gluePlan.limitedByManual);
            expectEquals (gluePlan.increments.colorGlue, 0.0f);
            expect (! gluePlan.automaticLevelWithdrawn, utf8 ("手动 Glue 超预算同样不算电平类自动全部撤回"));

            // Limit 旁通：自动推不了响度（没有限幅器兜底），标受手动限制。
            auto bypassed = defaultManual();
            bypassed.limit.bypass = true;
            const auto bypassPlan = planFor (&profile, Style::clean, 100.0f, bypassed);
            expect (bypassPlan.limitedByManual);
            expect (bypassPlan.automaticLevelWithdrawn, utf8 ("Limit 旁通：电平类自动全部撤回"));
            expectEquals (bypassPlan.increments.limitGainDb, 0.0f);

            // 约束反馈上限低于手动链净增益：自动全部撤回，两个标志都置（GlazePlanner.h「maxGainDb 低于手动链净增益」）。
            auto someGain = defaultManual();
            someGain.limit.gainDb = 6.0f;
            const auto manualNet = planFor (&profile, Style::clean, 0.0f, someGain).effectiveGainDb;   // GLAZE 0 = 手动链的预计净增益
            const auto cappedPlan = planFor (&profile, Style::clean, 100.0f, someGain, manualNet - 1.0f);
            expect (cappedPlan.limitedByManual && cappedPlan.automaticLevelWithdrawn && cappedPlan.limitedByConstraints);
            expect (allIncrementsZero (cappedPlan.increments) || (cappedPlan.increments.limitGainDb == 0.0f && cappedPlan.increments.colorGlue == 0.0f
                                                                  && cappedPlan.increments.colorHeat == 0.0f && cappedPlan.increments.limitClip == 0.0f),
                    utf8 ("动电平的自动增量全为 0"));
            expect (! planFor (&profile, Style::clean, 100.0f, someGain).automaticLevelWithdrawn, utf8 ("不设上限时不撤回"));

            // limitPlanTo 对新标志同样取或（T-032 R6-02 核验第 2 点）：置信度低的方案按「置信度拉满」的方案逐项封顶。手动 Glue 90 让手动链
            // 净增益随决策用的输入响度变（置信度低 → 输入响度往更响一侧收拢 → 侧链检测电平高、GR 多、净增益小），上限取在两者之间：
            // 低置信方案自己不撤回，封顶方案撤回——合起来必须撤回（两个标志都置）。
            {
                auto glueHeavy = defaultManual();
                glueHeavy.color.glue = 90.0f;
                const auto unsure = makeProfile (-24.0f, -12.0f, 5.0f, 0.5f);
                const auto sure = makeProfile (-24.0f, -12.0f, 5.0f, 1.0f);
                const auto unsureNet = planFor (&unsure, Style::clean, 0.0f, glueHeavy).effectiveGainDb;
                const auto sureNet = planFor (&sure, Style::clean, 0.0f, glueHeavy).effectiveGainDb;
                logMessage (utf8 ("  手动 Glue 90：低置信时手动链净增益 ") + juce::String (unsureNet, 3) + utf8 (" dB，置信拉满 ") + juce::String (sureNet, 3) + " dB");
                expect (sureNet > unsureNet + 0.1f, utf8 ("构造前提：两种置信度下手动链净增益不同"));

                const auto between = 0.5f * (unsureNet + sureNet);
                const auto boundPlan = planFor (&unsure, Style::clean, 100.0f, glueHeavy, between);
                expect (boundPlan.automaticLevelWithdrawn && boundPlan.limitedByManual, utf8 ("封顶方案全部撤回：合起来也全部撤回"));
                expectEquals (boundPlan.increments.limitGainDb, 0.0f, utf8 ("取小之后自动 Limit Gain 为 0"));
            }

            // 输入本身已超 Ceiling（手动全默认）：不是手动的问题，方案照常按风格分摊峰值，也不标受手动限制。
            const auto clipped = makeProfile (-10.0f, 3.0f);
            const auto clippedPlan = planFor (&clipped, Style::punchy, 100.0f);
            expect (! clippedPlan.limitedByManual, utf8 ("输入本身超 Ceiling 不算受手动限制"));
            expect (clippedPlan.increments.limitClip > 0.0f, utf8 ("冲击风格应让 Clip 接过输入自己的超限"));

            // 手动在上限以内：自动叠加在手动之上，手动改动不会被抵消（手动 Heat + 5 → 实际 Heat 也 + 5）。
            auto someHeat = defaultManual();
            someHeat.color.heat = 5.0f;
            const auto base = planFor (&profile, Style::warm, 60.0f);
            const auto withManual = planFor (&profile, Style::warm, 60.0f, someHeat);
            expect (! withManual.limitedByManual);
            expectWithinAbsoluteError (applyIncrements (someHeat, withManual.increments).color.heat,
                                       applyIncrements (defaultManual(), base.increments).color.heat + 5.0f, 1.0e-3f);
        }

        //==============================================================================
        // T-032 R6-02 收尾（A11）：手动链单独的用量（estimateManualChainUsage）与「手动链单独已超预算」的项（Plan::manualBudgetMask）。
        // 保护层与上游监视每项的有效上限 = max(风格上限, 手动单独用量)：编排器把置位的项按不设约束核对，其余照风格上限——所以两条最要紧：
        // 手动没超预算时掩码必为 0（上限逐位同修前），置了位的项自动增量必为 0（Heat / Glue / Clip；Clip 另有决策器不再按 Clip 预算限制
        // 自动 Gain）。
        void testManualBudgetMask()
        {
            beginTest ("manual-only usage and the budget items the automatic part no longer answers for (T-032 R6-02 closing)");

            // 依据：任务书 BRIEF-A11 第 1 条「每项约束的有效上限 = max(风格上限, 手动单独用量)，手动没超预算时逐位等于风格上限」；
            //       GlazeTypes.h manualBudget* 的判定；决策与校准第 8 节「手动部分本身已超约束时，自动先撤回自己的增量」。
            const auto& fc = factoryConstants();
            const auto usageFor = [] (const MusicProfile* profile, Style style, const ChainParams& manual, const StyleWeights* locked)
            {
                PlanInputs in;
                in.profile = profile;
                in.style = style;
                in.glazePercent = 100.0f;
                in.manual = manual;
                in.lockedWeights = locked;
                return estimateManualChainUsage (in);
            };

            // makePlan 封顶用的「置信度拉满」画像（GlazePlanner.cpp makePlan 同一口径：有效特征、27 段与代表性拉到 fullConfidence）；
            // raised = 有没有哪一项真被拉了（没有就不封顶）。
            const auto raisedProfile = [&fc] (const MusicProfile& profile, bool& raised)
            {
                const auto full = fc.fullConfidence > 0.0f ? fc.fullConfidence : 1.0f;
                auto confident = profile;
                raised = false;

                const auto raise = [&] (float& confidence)
                {
                    if (! (confidence >= full))
                    {
                        confidence = full;
                        raised = true;
                    }
                };

                for (auto* feature : { &confident.integratedLufs, &confident.loudnessRangeLu, &confident.plrDb, &confident.truePeakDbtp,
                                       &confident.spectralSlopeDbPerOct, &confident.lowShare, &confident.highShare, &confident.lowBandRangeDb,
                                       &confident.onsetDensityPerSecond, &confident.onsetStrengthDb })
                    if (feature->valid)
                        raise (feature->confidence);

                for (auto& band : confident.bandConfidence)
                    raise (band);

                raise (confident.representativeness);
                return confident;
            };

            // 1. 手动中性：用量（Glue / Heat / Clip）与掩码都是 0；各风格、GLAZE 0 ～ 100 的方案掩码都是 0。
            {
                const auto profile = makeProfile (-14.0f, -3.0f);
                bool allZero = true;

                for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                {
                    const auto u = usageFor (&profile, style, defaultManual(), nullptr);
                    allZero = allZero && u.overBudgetMask == 0 && u.heatAmount == 0.0f && u.glueGrMeanDb == 0.0f && u.glueGrP95Db == 0.0f
                                      && u.clipDepthDb == 0.0f && u.clipActivity == 0.0f;

                    for (auto glaze : { 0.0f, 0.1f, 35.0f, 100.0f })
                        allZero = allZero && planFor (&profile, style, glaze).manualBudgetMask == 0 && planFor (nullptr, style, glaze).manualBudgetMask == 0;
                }

                expect (allZero, utf8 ("手动中性：手动链单独的 Glue / Heat / Clip 用量与掩码都是 0"));
            }

            // 2. 性质扫描（随机画像 / 手动值 / 风格 / 约束反馈，GLAZE 35 与 100）：
            //    a. 各位的判定：Heat ⟺ 手动 Heat > 驱动上限；Glue ⟺ 手动单独的 Glue 均值或 p95 GR > 上限（不加容差；决策器按同一模型的
            //       glueCap 判，上限两侧 10⁻³ dB 以内的浮点边界不比）；Clip ⟺ 手动单独的削波深度 > 上限 + 0.05 dB 或活动 > 上限 × 1.02
            //       （决策器撤回自动 Clip 的同一判定）。上限按方案权重混合；
            //    b. 手动链单独的用量都不超风格上限 ⟹ 掩码为 0——掩码为 0 时编排器交给保护层的上限逐位是混合常数，与修前相同；
            //    c. 方案的位：Heat / Glue / Clip 置位时该项自动增量为 0；
            //    d. 置信度封顶按位取或（limitPlanTo）：方案的掩码 = 本方案手动链单独的位 | 置信拉满（锁定本方案权重）的 | 「自动」且没锁
            //       权重时再 | 置信拉满且按它重判权重的——逐位相等；GLAZE 0 的方案为 0。
            {
                juce::Random random (0xa11b0d9e);
                int scenarios = 0, withBits = 0, mismatches = 0, zeroMaskOverCap = 0, badIncrements = 0, badMerge = 0, mergedMore = 0;
                int heatBits = 0, glueBits = 0, clipBits = 0;

                for (int i = 0; i < 600; ++i)
                {
                    auto s = randomScenario (random);

                    if (random.nextBool())   // 多给一些手动超预算的组合
                    {
                        s.manual.color.heat = random.nextFloat() * 100.0f;
                        s.manual.color.glue = random.nextFloat() * 100.0f;
                        s.manual.limit.clip = random.nextFloat() < 0.5f ? random.nextFloat() * 100.0f : 0.0f;
                        s.manual.limit.gainDb = random.nextFloat() * 12.0f;
                    }

                    const auto* profile = s.hasProfile ? &s.profile : nullptr;
                    const auto u = usageFor (profile, s.style, s.manual, nullptr);

                    for (auto glaze : { 35.0f, 100.0f })
                    {
                        const auto plan = planFor (s, glaze);
                        const auto caps = blendConstants (fc, plan.weights);
                        const auto heat = juce::jlimit (0.0f, 100.0f, std::isfinite (s.manual.color.heat) ? s.manual.color.heat : 0.0f);
                        const auto heatOver = heat > caps.heatDriveCap;
                        const auto glueOverBy = juce::jmax (u.glueGrMeanDb - caps.glueGrMeanCapDb, u.glueGrP95Db - caps.glueGrP95CapDb);
                        const auto glueOver = glueOverBy > 0.0f;
                        const auto glueEdge = std::abs (glueOverBy) < 1.0e-3f;
                        const auto clipOver = u.clipDepthDb > caps.clipDepthCapDb + 0.05f || u.clipActivity > caps.clipActivityCap * 1.02f;
                        const auto expected = (heatOver ? manualBudgetHeat : 0) | (glueOver ? manualBudgetGlue : 0) | (clipOver ? manualBudgetClip : 0);
                        const auto compared = glueEdge ? (int) (manualBudgetHeat | manualBudgetClip) : (int) manualBudgetAll;
                        const auto own = (int) u.overBudgetMask;

                        ++scenarios;
                        withBits += own != 0 ? 1 : 0;
                        heatBits += (own & manualBudgetHeat) != 0 ? 1 : 0;
                        glueBits += (own & manualBudgetGlue) != 0 ? 1 : 0;
                        clipBits += (own & manualBudgetClip) != 0 ? 1 : 0;
                        mismatches += (own & compared) != (expected & compared) ? 1 : 0;

                        const auto withinCaps = ! heatOver && glueOverBy <= -1.0e-3f
                                              && u.clipDepthDb <= caps.clipDepthCapDb && u.clipActivity <= caps.clipActivityCap;
                        zeroMaskOverCap += withinCaps && own != 0 ? 1 : 0;

                        badIncrements += ((plan.manualBudgetMask & manualBudgetHeat) != 0 && plan.increments.colorHeat != 0.0f)
                                      || ((plan.manualBudgetMask & manualBudgetGlue) != 0 && plan.increments.colorGlue != 0.0f)
                                      || ((plan.manualBudgetMask & manualBudgetClip) != 0 && plan.increments.limitClip != 0.0f) ? 1 : 0;

                        auto merged = own;

                        if (profile != nullptr)
                        {
                            bool raised = false;
                            const auto confident = raisedProfile (*profile, raised);

                            if (raised)
                            {
                                merged |= usageFor (&confident, Style::automatic, s.manual, &plan.weights).overBudgetMask;

                                if (s.style == Style::automatic)   // randomScenario 不锁权重：makePlan 再按满置信度重判的权重封一次
                                    merged |= usageFor (&confident, Style::automatic, s.manual, nullptr).overBudgetMask;
                            }
                        }

                        badMerge += (int) plan.manualBudgetMask != merged ? 1 : 0;
                        mergedMore += merged != own ? 1 : 0;
                    }

                    badMerge += planFor (s, 0.0f).manualBudgetMask != 0 ? 1 : 0;
                }

                logMessage ("  manual budget scan: " + juce::String (scenarios) + " plans, " + juce::String (withBits) + " with bits (heat / glue / clip "
                            + juce::String (heatBits) + " / " + juce::String (glueBits) + " / " + juce::String (clipBits) + "), "
                            + juce::String (mergedMore) + " gained a bit from the full-confidence cap");
                expect (heatBits > 20 && glueBits > 20 && clipBits > 5, utf8 ("扫描覆盖了三种位（前提）"));
                expectEquals (mismatches, 0, utf8 ("各位按手动链单独的用量与风格上限判定"));
                expectEquals (zeroMaskOverCap, 0, utf8 ("手动单独的用量都不超风格上限时掩码为 0"));
                expectEquals (badIncrements, 0, utf8 ("置位的项：自动增量为 0"));
                expectEquals (badMerge, 0, utf8 ("封顶合并：三位都取或，与各份方案的手动链单独用量逐位相符；GLAZE 0 为 0"));
            }

            // 3. Heat：四种风格下手动 Heat 0 ～ 100 每 0.25 扫描，位恰在「超过风格驱动上限」时置（置信度满、权重锁定：方案的位 = 手动链单独的位），
            //    置位后自动 Heat 为 0，自动 Gain 照常。
            {
                const auto profile = makeProfile (-20.0f, -8.0f);
                int wrong = 0, gainGone = 0;

                for (auto style : { Style::warm, Style::punchy, Style::clean })
                {
                    const auto cap = blendConstants (fc, weightsFor (style, {})).heatDriveCap;

                    for (float heat = 0.0f; heat <= 100.0f; heat += 0.25f)
                    {
                        auto manual = defaultManual();
                        manual.color.heat = heat;
                        const auto plan = planFor (&profile, style, 100.0f, manual);
                        const auto set = (plan.manualBudgetMask & manualBudgetHeat) != 0;
                        wrong += set != (heat > cap) || set != ((usageFor (&profile, style, manual, nullptr).overBudgetMask & manualBudgetHeat) != 0)
                                 || (set && plan.increments.colorHeat != 0.0f) ? 1 : 0;
                        gainGone += set && ! plan.automaticLevelWithdrawn && ! (plan.increments.limitGainDb > 0.0f) ? 1 : 0;
                    }
                }

                expectEquals (wrong, 0, utf8 ("Heat 位恰在手动 Heat 超过风格驱动上限时置，置位后自动 Heat 为 0"));
                expectEquals (gainGone, 0, utf8 ("只有手动 Heat 超预算时自动 Gain 照常推响"));
            }

            // 4. Glue：通透、响素材上把手动 Glue 从 0 往上扫，位从「手动单独 GR 刚超上限」那一格起置、之后一直置；上限附近「超了但没超过
            //    决策器撤回的容差」那一段（不标受手动限制）同样置位——那里自动 Glue 的余量已是 0，Glue GR 全是手动的。
            {
                const auto profile = makeProfile (-12.0f, -2.0f);
                const StyleWeights clean { 0.0f, 0.0f, 1.0f };
                const auto caps = blendConstants (fc, clean);
                int nonMonotone = 0, wrongIncrement = 0, slivers = 0, wrongBit = 0, firstSet = -1;
                bool previous = false;

                for (int step = 0; step <= 4000; ++step)
                {
                    auto manual = defaultManual();
                    manual.color.glue = (float) step * 0.025f;
                    const auto plan = planFor (&profile, Style::clean, 100.0f, manual);
                    const auto u = usageFor (&profile, Style::clean, manual, nullptr);
                    const auto set = (plan.manualBudgetMask & manualBudgetGlue) != 0;

                    nonMonotone += previous && ! set ? 1 : 0;
                    wrongIncrement += set && plan.increments.colorGlue != 0.0f ? 1 : 0;

                    if (set && firstSet < 0)
                        firstSet = step;

                    const auto overBy = juce::jmax (u.glueGrMeanDb - caps.glueGrMeanCapDb, u.glueGrP95Db - caps.glueGrP95CapDb);
                    slivers += set && overBy > 0.0f && overBy <= 0.05f && ! plan.limitedByManual ? 1 : 0;
                    wrongBit += std::abs (overBy) >= 1.0e-3f && set != (overBy > 0.0f) ? 1 : 0;
                    previous = set;
                }

                logMessage ("  Glue bit from manual Glue " + juce::String ((float) firstSet * 0.025f, 3) + ", " + juce::String (slivers)
                            + " steps over the cap within the withdrawal tolerance");
                expect (firstSet > 0, utf8 ("响素材上手动 Glue 够大时置 Glue 位（前提）"));
                expectEquals (wrongBit, 0, utf8 ("Glue 位 ⟺ 手动单独的 GR 超上限（边界 10⁻³ dB 以内不比）"));
                expectEquals (nonMonotone, 0, utf8 ("Glue 位随手动 Glue 单调"));
                expectEquals (wrongIncrement, 0, utf8 ("置位后自动 Glue 为 0"));
                expect (slivers > 0, utf8 ("超上限但在撤回容差以内（不标受手动限制）也置位：自动 Glue 余量已是 0"));
            }

            // 5. Clip：手动 Clip 100（阈值 = Ceiling）+ 手动 Gain 5 dB 把 TP −3 的素材压进削波器：深度超出通透的 Clip 预算、决策器撤回自动 Clip，
            //    限幅器没超（软削波已把峰值收住）——Clip 位置、电平类自动没有全部撤回、自动 Gain 照常推。手动 Clip 20 没超：不置位。
            {
                const auto profile = makeProfile (-14.0f, -3.0f);
                auto heavy = defaultManual();
                heavy.limit.clip = 100.0f;
                heavy.limit.gainDb = 5.0f;
                const auto plan = planFor (&profile, Style::clean, 100.0f, heavy);
                const auto u = usageFor (&profile, Style::clean, heavy, nullptr);
                logMessage ("  manual Clip 100 + Gain 5: clip depth " + juce::String (u.clipDepthDb, 2) + " dB, limiter peak GR "
                            + juce::String (u.limitGrPeakDb, 2) + " dB, automatic Limit Gain " + juce::String (plan.increments.limitGainDb, 2) + " dB");
                expect ((plan.manualBudgetMask & manualBudgetClip) != 0 && plan.limitedByManual && ! plan.automaticLevelWithdrawn,
                        utf8 ("手动 Clip 单独超预算：Clip 位、受手动限制，电平类自动没有全部撤回"));
                expectEquals (plan.increments.limitClip, 0.0f, utf8 ("自动 Clip 撤回"));
                expect (plan.increments.limitGainDb > 0.0f, utf8 ("自动 Gain 照常推（决策器此时只按限幅器核对）"));

                auto light = defaultManual();
                light.limit.clip = 20.0f;
                expectEquals ((int) planFor (&profile, Style::clean, 100.0f, light).manualBudgetMask, 0, utf8 ("手动 Clip 没超预算：不置位"));
            }

            // 6. 置信度封顶的合并（limitPlanTo，三位都取或）。置信度低时决策用的输入响度往「更响」一侧收，Glue 侧链检测电平高、GR 多：
            //    a. 手动 Glue 让低置信的手动链超 Glue 上限、置信拉满的没超 → 方案置 Glue 位（自动 Glue 取小后为 0）；
            //    b. 同一收拢让 Glue 压低峰值、削波更浅：手动 Glue + Clip + Gain 让置信拉满的手动链超 Clip 预算、低置信的没超 → 方案同样置
            //       Clip 位、自动 Clip 为 0。不置（取与）的话保护层按通透的 Clip 上限核对，而更准的那份模型说手动单独的削波已经超了：保护层
            //       退不掉手动的那份削波，退到底、约束反馈把自动增益磨光。
            {
                const StyleWeights clean { 0.0f, 0.0f, 1.0f };
                const auto unsure = makeProfile (-24.0f, -12.0f, 5.0f, 0.5f);
                const auto sure = makeProfile (-24.0f, -12.0f, 5.0f, 1.0f);

                bool glueCase = false, clipCase = false, glueMerged = true, clipMerged = true;

                for (int step = 0; step <= 1000 && ! glueCase; ++step)
                {
                    auto manual = defaultManual();
                    manual.color.glue = (float) step * 0.1f;
                    const auto own = usageFor (&unsure, Style::clean, manual, &clean).overBudgetMask;
                    const auto bound = usageFor (&sure, Style::clean, manual, &clean).overBudgetMask;

                    if ((own & manualBudgetGlue) != 0 && (bound & manualBudgetGlue) == 0)
                    {
                        glueCase = true;
                        glueMerged = (planFor (&unsure, Style::clean, 100.0f, manual, 1.0e9f, &clean).manualBudgetMask & manualBudgetGlue) != 0;
                    }
                }

                for (float glue : { 40.0f, 60.0f, 80.0f, 100.0f })
                {
                    for (int step = 0; step <= 480 && ! clipCase; ++step)
                    {
                        auto manual = defaultManual();
                        manual.color.glue = glue;
                        manual.limit.clip = 100.0f;
                        manual.limit.gainDb = (float) step * 0.05f;
                        const auto own = usageFor (&unsure, Style::clean, manual, &clean);
                        const auto bound = usageFor (&sure, Style::clean, manual, &clean);

                        if ((own.overBudgetMask & manualBudgetClip) == 0 && (bound.overBudgetMask & manualBudgetClip) != 0)
                        {
                            clipCase = true;
                            const auto plan = planFor (&unsure, Style::clean, 100.0f, manual, 1.0e9f, &clean);
                            clipMerged = (plan.manualBudgetMask & manualBudgetClip) != 0 && plan.increments.limitClip == 0.0f;
                            logMessage ("  merge case: manual Glue " + juce::String (glue, 0) + " + Clip 100 + Gain " + juce::String (manual.limit.gainDb, 2)
                                        + " dB: clip depth unsure " + juce::String (own.clipDepthDb, 3) + " / sure " + juce::String (bound.clipDepthDb, 3) + " dB");
                        }
                    }
                }

                expect (glueCase && clipCase, utf8 ("找到了两种合并方向的构造（前提）"));
                expect (glueMerged, utf8 ("Glue 位取或：低置信的手动链超了，方案就置位"));
                expect (clipMerged, utf8 ("Clip 位同样取或：置信拉满的封顶方案的手动链超了、低置信的没超，方案置位且自动 Clip 为 0"));
            }
        }

        //==============================================================================
        void testDeterminism()
        {
            beginTest ("Same inputs give bit-identical plans");

            // 依据：设计规格-GLAZE-界面与实现第 2 节「GlazePlanner：纯函数，同输入必同输出」；GLAZE 验收总表第 2 条。
            juce::Random random (0x5eed04);
            std::vector<Scenario> scenarios;

            for (int i = 0; i < 40; ++i)
                scenarios.push_back (randomScenario (random));

            std::vector<Plan> first;

            for (const auto& s : scenarios)
                for (auto glaze : { 0.0f, 12.5f, 50.0f, 99.9f, 100.0f })
                    first.push_back (planFor (s, glaze));

            // 倒序、换一份画像拷贝（不同地址）再算一遍。
            size_t index = first.size();

            for (auto s = scenarios.rbegin(); s != scenarios.rend(); ++s)
            {
                const auto copy = *s;

                for (auto glaze : { 100.0f, 99.9f, 50.0f, 12.5f, 0.0f })
                    expect (samePlan (planFor (copy, glaze), first[--index]), utf8 ("同一输入两次的方案不逐位相同"));
            }

            const auto profile = makeProfile (-17.0f, -3.0f);
            expect (sameBits (resolveAutomaticWeights (profile, factoryConstants()).punchy, resolveAutomaticWeights (profile, factoryConstants()).punchy));
            expect (computeProfileId (profile) == computeProfileId (MusicProfile (profile)));
        }

        //==============================================================================
        void testAutomaticWeights()
        {
            beginTest ("Automatic weights sum to one and lean clean when unsure");

            // 依据：决策与校准第 1 节「自动按画像特征连续混合三种风格的权重；置信不足时偏向通透（最保守）」。
            const auto& c = factoryConstants();
            auto check = [this] (const StyleWeights& w)
            {
                expect (w.warm >= 0.0f && w.punchy >= 0.0f && w.clean >= 0.0f);
                expectWithinAbsoluteError (w.warm + w.punchy + w.clean, 1.0f, 1.0e-6f);
            };

            auto drums = makeProfile (-16.0f, -2.0f, 4.0f);
            setFeature (drums.onsetStrengthDb, 11.0f);
            setFeature (drums.onsetDensityPerSecond, 4.0f);
            const auto drumWeights = resolveAutomaticWeights (drums, c);
            check (drumWeights);
            expect (drumWeights.punchy > drumWeights.warm && drumWeights.punchy > drumWeights.clean, utf8 ("鼓点驱动的素材应偏冲击"));
            const auto fullDrumTotal = 3.0f * c.autoBaseWeight + 1.0f;
            expectWithinAbsoluteError (drumWeights.warm, c.autoBaseWeight / fullDrumTotal, 1.0e-6f);
            expectWithinAbsoluteError (drumWeights.punchy, (c.autoBaseWeight + 1.0f) / fullDrumTotal, 1.0e-6f);
            expectWithinAbsoluteError (drumWeights.clean, c.autoBaseWeight / fullDrumTotal, 1.0e-6f,
                                       "the fully confident mapping stays compatible");

            // 首次提交时 LRA 尚未观察满 60 s，但起音证据已经可靠；不能因此锁定通透。
            auto earlyDrums = drums;
            earlyDrums.loudnessRangeLu.confidence = 0.2f;
            earlyDrums.loudnessRangeLu.effectiveSeconds = 12.0;
            const auto earlyDrumWeights = resolveAutomaticWeights (earlyDrums, c);
            check (earlyDrumWeights);
            expectEquals (earlyDrumWeights.punchy, drumWeights.punchy, "early LRA cannot veto reliable drum evidence");
            expectEquals (earlyDrumWeights.clean, drumWeights.clean);
            earlyDrums.loudnessRangeLu.valid = false;
            earlyDrums.loudnessRangeLu.value = std::numeric_limits<float>::quiet_NaN();
            const auto noRangeWeights = resolveAutomaticWeights (earlyDrums, c);
            check (noRangeWeights);
            expectEquals (noRangeWeights.punchy, drumWeights.punchy, "unavailable LRA is not evidence against a rhythmic mix");

            // 已观察到大动态时，LRA 按自身信任度逐步增加通透取向，而不是缩小其他所有证据。
            auto dynamicDrums = drums;
            setFeature (dynamicDrums.loudnessRangeLu, 15.0f, 0.2f);
            const auto earlyDynamicWeights = resolveAutomaticWeights (dynamicDrums, c);
            dynamicDrums.loudnessRangeLu.confidence = 1.0f;
            const auto fullDynamicWeights = resolveAutomaticWeights (dynamicDrums, c);
            check (earlyDynamicWeights);
            check (fullDynamicWeights);
            expect (earlyDynamicWeights.clean > drumWeights.clean);
            expect (fullDynamicWeights.clean > earlyDynamicWeights.clean);
            expect (fullDynamicWeights.punchy < earlyDynamicWeights.punchy);

            auto dynamic = makeProfile (-24.0f, -8.0f, 15.0f);
            setFeature (dynamic.onsetStrengthDb, 2.0f);
            setFeature (dynamic.onsetDensityPerSecond, 0.8f);
            const auto dynamicWeights = resolveAutomaticWeights (dynamic, c);
            check (dynamicWeights);
            expect (dynamicWeights.clean > dynamicWeights.warm && dynamicWeights.clean > dynamicWeights.punchy, utf8 ("动态大的素材应偏通透"));

            auto sustained = makeProfile (-16.0f, -6.0f, 3.0f);
            setFeature (sustained.onsetStrengthDb, 1.0f);
            setFeature (sustained.onsetDensityPerSecond, 0.3f);
            const auto sustainedWeights = resolveAutomaticWeights (sustained, c);
            check (sustainedWeights);
            expect (sustainedWeights.warm > sustainedWeights.punchy && sustainedWeights.warm > sustainedWeights.clean, utf8 ("持续型素材应偏温润"));

            // 置信度为 0：全部通透；置信度越低越偏通透。
            auto unsure = drums;
            unsure.onsetStrengthDb.confidence = unsure.onsetDensityPerSecond.confidence = unsure.loudnessRangeLu.confidence = 0.0f;
            expectEquals (resolveAutomaticWeights (unsure, c).clean, 1.0f);

            auto half = drums;
            half.onsetStrengthDb.confidence = half.onsetDensityPerSecond.confidence = half.loudnessRangeLu.confidence = 0.4f;
            const auto halfWeights = resolveAutomaticWeights (half, c);
            check (halfWeights);
            expect (halfWeights.clean > drumWeights.clean && halfWeights.punchy < drumWeights.punchy);

            // 两个起音特征任一个缺失或不可信时，长期 LRA 再可靠也不凭空猜测染色取向。
            auto invalidOnset = drums;
            invalidOnset.onsetStrengthDb.valid = false;
            expectEquals (resolveAutomaticWeights (invalidOnset, c).clean, 1.0f);
            invalidOnset = drums;
            invalidOnset.onsetDensityPerSecond.valid = false;
            expectEquals (resolveAutomaticWeights (invalidOnset, c).clean, 1.0f);
            invalidOnset = drums;
            invalidOnset.onsetDensityPerSecond.confidence = 0.0f;
            expectEquals (resolveAutomaticWeights (invalidOnset, c).clean, 1.0f);

            // 无效特征按「没有」处理，不当 0 dB 的真实读数；结果照样和为 1。
            MusicProfile empty;
            expectEquals (resolveAutomaticWeights (empty, c).clean, 1.0f);
        }

        //==============================================================================
        void testBlendConstants()
        {
            beginTest ("Blended constants interpolate between the styles");

            // 依据：GlazePlanner.h：三种风格的偏好与约束按权重线性混合；离散风格是独热。
            const auto& c = factoryConstants();
            const auto sameStyle = [] (const StyleConstants& a, const StyleConstants& b)
            {
                return sameBits (a.preferredLufs, b.preferredLufs) && sameBits (a.heatDriveCap, b.heatDriveCap)
                    && sameBits (a.glueGrMeanCapDb, b.glueGrMeanCapDb) && sameBits (a.glueGrP95CapDb, b.glueGrP95CapDb)
                    && sameBits (a.clipDepthCapDb, b.clipDepthCapDb) && sameBits (a.clipActivityCap, b.clipActivityCap)
                    && sameBits (a.limitGrMeanCapDb, b.limitGrMeanCapDb) && sameBits (a.limitGrP95CapDb, b.limitGrP95CapDb)
                    && sameBits (a.limitGrPeakCapDb, b.limitGrPeakCapDb) && sameBits (a.recoverCap, b.recoverCap)
                    && sameBits (a.tameCap, b.tameCap) && sameBits (a.toneMaxDb, b.toneMaxDb)
                    && sameBits (a.shareHeat, b.shareHeat) && sameBits (a.shareGlue, b.shareGlue)
                    && sameBits (a.shareClip, b.shareClip) && sameBits (a.shareLimit, b.shareLimit)
                    && sameBits (a.heatPeakReductionCapDb, b.heatPeakReductionCapDb)
                    && sameBits (a.toneReferenceSlopeDbPerOct, b.toneReferenceSlopeDbPerOct);
            };

            expect (sameStyle (blendConstants (c, weightsFor (Style::warm, {})), c.warm));
            expect (sameStyle (blendConstants (c, weightsFor (Style::punchy, {})), c.punchy));
            expect (sameStyle (blendConstants (c, weightsFor (Style::clean, {})), c.clean));
            expect (sameStyle (constantsFor (c, Style::punchy), c.punchy));

            const auto half = blendConstants (c, { 0.5f, 0.0f, 0.5f });
            expectWithinAbsoluteError (half.preferredLufs, 0.5f * (c.warm.preferredLufs + c.clean.preferredLufs), 1.0e-5f);
            expectWithinAbsoluteError (half.heatDriveCap, 0.5f * (c.warm.heatDriveCap + c.clean.heatDriveCap), 1.0e-4f);
            expectWithinAbsoluteError (half.limitGrPeakCapDb, 0.5f * (c.warm.limitGrPeakCapDb + c.clean.limitGrPeakCapDb), 1.0e-5f);

            // 自动权重的清洗：负数与非有限值按 0、全 0 → 通透、其余归一。
            const auto odd = weightsFor (Style::automatic, { -1.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f });
            expectEquals (odd.clean, 1.0f);
            const auto scaled = weightsFor (Style::automatic, { 2.0f, 1.0f, 1.0f });
            expectWithinAbsoluteError (scaled.warm, 0.5f, 1.0e-6f);
            expectWithinAbsoluteError (scaled.punchy + scaled.clean, 0.5f, 1.0e-6f);
            expectEquals (weightsFor (Style::warm, { 0.0f, 0.0f, 1.0f }).warm, 1.0f, utf8 ("离散风格不看自动权重"));
        }

        //==============================================================================
        void testRealChainWithinLimits()
        {
            beginTest ("Plans stay within the style limits on the real modules");

            // 依据：决策与校准第 4 节「永不超过各模块上限」（约束是上限，按各模块自己的量）、第 3 节（effectiveTarget 是校准的目标，
            //       前馈要对得上）；ADR-004 输出真峰值 ≤ Ceiling + 0.01 dB。用真实模块在类音乐素材上量，裕量给实测统计口径的差别。
            const auto& c = factoryConstants();
            const auto source = makeMusic (8.0);

            for (auto lufs : { -20.0f, -14.0f })
            {
                const auto music = atLufs (source, lufs);
                const auto profile = profileOf (music);

                for (auto style : { Style::warm, Style::punchy, Style::clean })
                    for (auto glaze : { 50.0f, 100.0f })
                    {
                        const auto plan = planFor (&profile, style, glaze);
                        const auto run = runChain (music, applyIncrements (defaultManual(), plan.increments));
                        const auto& sc = constantsFor (c, style);
                        const auto label = juce::String (lufs) + utf8 (" LUFS 风格 ") + juce::String ((int) style) + " GLAZE " + juce::String (glaze);

                        logMessage ("  " + label + utf8 ("：目标净增益 ") + juce::String (plan.effectiveGainDb, 2) + utf8 (" dB，实测 ") + juce::String (run.netDb, 2)
                                    + utf8 ("；Glue 均 / p95 ") + juce::String (run.glueMeanDb, 2) + " / " + juce::String (run.glueP95Db, 2)
                                    + utf8 ("；Clip 深度 ") + juce::String (run.clipDepthMaxDb, 2) + utf8 ("、活动 ") + juce::String (run.clipActivityMean, 4)
                                    + utf8 ("；Limit 峰值 / p95 / 均 ") + juce::String (run.limitPeakDb, 2) + " / " + juce::String (run.limitP95Db, 2)
                                    + " / " + juce::String (run.limitMeanDb, 2));

                        expect (std::abs (run.netDb - plan.effectiveGainDb) <= 0.5f, label + utf8 ("：前馈净增益与实测差超过 0.5 dB"));
                        expect (run.outputTruePeakDb <= -1.0f + 0.01f, label + utf8 ("：输出真峰值超过 Ceiling"));
                        expect (run.glueMeanDb <= sc.glueGrMeanCapDb + 0.3f && run.glueP95Db <= sc.glueGrP95CapDb + 0.5f, label + utf8 ("：Glue 超上限"));
                        expect (run.clipDepthMaxDb <= sc.clipDepthCapDb + 0.5f && run.clipActivityMean <= sc.clipActivityCap * 1.5f + 1.0e-3f,
                                label + utf8 ("：Clip 超上限"));
                        expect (run.limitPeakDb <= sc.limitGrPeakCapDb + 0.5f && run.limitP95Db <= sc.limitGrP95CapDb + 0.5f
                                && run.limitMeanDb <= sc.limitGrMeanCapDb + 0.3f, label + utf8 ("：Limit 超上限"));
                    }
            }
        }

        //==============================================================================
        void testMappingsMatchModules()
        {
            beginTest ("Planner mappings match the real modules");

            // 依据：GlazeConstants.h【映射】项必须与模块源码一致（Glue 补偿、Clip 阈值），换了模块映射这里会先报。
            const auto& c = factoryConstants();

            // Glue：远低于阈值的正弦只吃自动补偿 = −阈值 × (1 − 1/比率) × 系数。
            for (auto glue : { 50.0f, 100.0f })
            {
                ColorModule color;
                ColorModule::Params p;
                p.glue = glue;
                color.setParams (p);
                color.prepare (makeSpec());
                color.setParams (p);

                auto sine = makeSine (numChannels, sampleRate, 1000.0, juce::Decibels::decibelsToGain (-50.0f), 1.0);
                const auto input = sine;
                juce::AudioBuffer<float> block (numChannels, blockSize);
                const auto n = (int) sine.front().size();

                for (int start = 0; start < n; start += blockSize)
                {
                    const auto length = juce::jmin (blockSize, n - start);
                    block.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, sine[(size_t) ch].data() + start, length);

                    color.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, sine[(size_t) ch].begin() + start);
                }

                const auto [inLow, inHigh] = steadyTruePeakRange (input);
                const auto [outLow, outHigh] = steadyTruePeakRange (sine, n / 2);
                juce::ignoreUnused (inLow, outLow);
                const auto expected = -c.glueThresholdAtFullDb * glue / 100.0f * (1.0f - 1.0f / c.glueRatio) * c.glueMakeupFraction;
                expectWithinAbsoluteError (toDb (outHigh) - toDb (inHigh), expected, 0.05f, utf8 ("Glue 补偿与决策器的映射不一致"));
            }

            // Clip：阈值 = +12 dBTP → Ceiling 按 dB 线性；0 dBFS 正弦的削波深度 = 0 − 阈值（统计口径同 BlockStats）。
            for (auto clip : { 95.0f, 100.0f })
            {
                LimiterModule limiter;
                LimiterModule::Params p;
                p.clip = clip;
                p.ceilingDb = -1.0f;
                limiter.setParams (p);
                limiter.prepare (makeSpec());
                limiter.setParams (p);

                const auto sine = makeSine (numChannels, sampleRate, 997.0, 1.0f, 0.5);
                runLimiter (limiter, sine, blockSize);
                const auto threshold = c.clipOffThresholdDb + (p.ceilingDb - c.clipOffThresholdDb) * clip / 100.0f;
                expectWithinAbsoluteError (limiter.getBlockStats().clipDepthMaxDb, 0.0f - threshold, 0.1f, utf8 ("Clip 阈值映射与决策器不一致"));
            }
        }

        //==============================================================================
        void testProfileId()
        {
            beginTest ("Profile id follows the quantized content");

            // 依据：GlazeProfile.h「画像内容的哈希（逐字段量化后 FNV-1a；同画像同值）」——校准参考条件用它判断「画像变了没有」。
            auto profile = makeProfile (-18.003f, -5.0f);
            const auto id = computeProfileId (profile);
            expect (id == computeProfileId (MusicProfile (profile)));

            auto tiny = profile;
            tiny.integratedLufs.value = -18.004f;   // 量化步长 0.01 dB 以内
            expect (computeProfileId (tiny) == id, utf8 ("量化步长以内的变化不应改变画像号"));

            auto changed = profile;
            changed.integratedLufs.value = -18.03f;
            expect (computeProfileId (changed) != id);

            auto flag = profile;
            flag.onsetStrengthDb.valid = false;
            expect (computeProfileId (flag) != id, utf8 ("有效标志变了画像号应变"));

            auto band = profile;
            band.tameEvidenceDb[13] = 0.5f;
            expect (computeProfileId (band) != id);

            auto withNan = profile;
            withNan.plrDb.value = std::numeric_limits<float>::quiet_NaN();
            expect (computeProfileId (withNan) != id);
            expect (computeProfileId (withNan) == computeProfileId (MusicProfile (withNan)), utf8 ("NaN 也应确定地编码"));
        }

        //==============================================================================
        static MusicProfile oddProfile()
        {
            // 每个字段都不同，并夹上 NaN、−0、非规格化数、±Inf 与大数：往返必须逐位相同。
            MusicProfile p;
            Feature* features[] = { &p.integratedLufs, &p.loudnessRangeLu, &p.plrDb, &p.truePeakDbtp, &p.spectralSlopeDbPerOct,
                                    &p.lowShare, &p.highShare, &p.lowBandRangeDb, &p.onsetDensityPerSecond, &p.onsetStrengthDb };
            float value = -23.456f;

            for (auto* f : features)
            {
                f->value = value;
                f->valid = value < 0.0f;
                f->effectiveSeconds = 12.345678901234 + (double) value;
                f->confidence = 0.123f - value * 0.001f;
                value += 3.21f;
            }

            p.truePeakDbtp.value = -0.0f;
            p.plrDb.value = std::numeric_limits<float>::quiet_NaN();
            p.lowShare.value = 1.0e-40f;   // 非规格化
            p.highShare.value = std::numeric_limits<float>::infinity();
            p.onsetStrengthDb.effectiveSeconds = -std::numeric_limits<double>::infinity();

            // 格式 2 的 Clay 特征与 ADR-012 的三类峰值代价（画像默认格式 2，这些字段同样进 XML）：同样各不相同、夹上特殊位模式。
            Feature* clayFeatures[] = { &p.subShareDb, &p.lowSideToMidDb, &p.lowCorrelation, &p.highSideToMidDb, &p.highCorrelation,
                                        &p.lowCutPeakCostDb, &p.monoPeakCostDb[0], &p.monoPeakCostDb[1], &p.monoPeakCostDb[2],
                                        &p.monoPeakCostDb[3], &p.widthPeakCostDb };

            for (auto* f : clayFeatures)
            {
                f->value = value;
                f->valid = value > 0.0f;
                f->effectiveSeconds = 3.25 + (double) value;
                f->confidence = 0.5f + value * 0.0001f;
                value += 1.75f;
            }

            p.lowCutPeakCostDb.value = std::numeric_limits<float>::quiet_NaN();
            p.monoPeakCostDb[2].value = -0.0f;
            p.monoPeakCostDb[3].confidence = 1.0e-41f;   // 非规格化
            p.widthPeakCostDb.effectiveSeconds = std::numeric_limits<double>::infinity();

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                p.bandLongTermDb[b] = -40.0f + 1.1f * (float) b;
                p.tameEvidenceDb[b] = 0.01f * (float) (b * b);
                p.recoverEvidenceDb[b] = b % 3 == 0 ? -0.0f : 1.0e30f / (float) (b + 1);
                p.bandConfidence[b] = 1.0f / (float) (b + 2);
                p.bandSideToMidDb[b] = -20.0f + 0.7f * (float) b;
                p.bandCorrelation[b] = 1.0f - 0.07f * (float) b;
                p.bandStereoConfidence[b] = 0.25f * (float) (b % 4);
            }

            p.effectiveSeconds = 29.999999999;
            p.representativeness = 0.875f;
            return p;
        }

        void testProfileAndStateRoundTrip()
        {
            beginTest ("Profile and committed state survive a save and load bit-exactly");

            // 依据：时间线与保存第 5 节（保存什么）、第 6 节（重开工程立即恢复、同一起点可复现）；GlazeProfile.h 往返逐位相同。
            const auto profile = oddProfile();

            // 走完整的「写成文本 → 解析 → 读取」路径（工程保存就是文本）。
            const auto text = toXml (profile)->toString();
            const auto parsed = juce::parseXML (text);
            expect (parsed != nullptr);

            MusicProfile restored = makeProfile (-9.0f, -1.0f);
            expect (fromXml (*parsed, restored));
            expect (bitwiseEqual (restored, profile), utf8 ("画像往返不逐位相同"));
            expect (toXml (restored)->toString() == text);
            expect (std::isnan (restored.plrDb.value) && std::signbit (restored.truePeakDbtp.value) && restored.lowShare.value == 1.0e-40f);
            expect (std::isnan (restored.lowCutPeakCostDb.value) && std::signbit (restored.monoPeakCostDb[2].value)
                        && restored.monoPeakCostDb[3].confidence == 1.0e-41f && std::isinf (restored.widthPeakCostDb.effectiveSeconds),
                    utf8 ("ADR-012 的峰值代价同样逐位往返（NaN、−0、非规格化、+Inf）"));

            // 已提交版本：有画像 / 未学习两种，方案来自真实 makePlan，校准含全部参考条件，常数改过几项。
            for (auto hasProfile : { true, false })
            {
                CommittedState state;
                state.hasProfile = hasProfile;
                state.profile = profile;
                state.profileId = computeProfileId (profile);
                state.lockedWeights = { 0.25f, 0.5f, 0.25f };
                const auto realProfile = makeProfile (-19.0f, -4.0f);
                state.plan = planFor (&realProfile, Style::automatic, 63.0f, defaultManual(), 1.0e9f, &state.lockedWeights);
                state.calibration.gainBiasDb = -0.8125f;
                state.calibration.valid = true;
                state.calibration.partial = hasProfile;
                state.calibration.budgetLimited = ! hasProfile;
                state.calibration.reference = { 0xdeadbeefu, Style::punchy, 63.0f, 0x12345678u, -1.5f, 3.5f, 44100.0 };
                state.constants = factoryConstants();
                state.constants.version = 7;
                state.constants.warm.preferredLufs = -8.25f;
                state.constants.heatPeakDb[17] = -0.0f;

                const auto stateText = toXml (state)->toString();
                const auto stateXml = juce::parseXML (stateText);
                CommittedState back;
                expect (stateXml != nullptr && fromXml (*stateXml, back));
                expect (bitwiseEqual (back, state), utf8 ("已提交版本往返不逐位相同"));
                expect (samePlan (back.plan, state.plan));
                expect (back.calibration.reference.style == Style::punchy && back.calibration.reference.sampleRate == 44100.0);
                expect (back.hasProfile == hasProfile && back.profileId == state.profileId);
                expect (bitwiseEqual (back.constants, state.constants));
            }
        }

        //==============================================================================
        // T-038（设计规格-曲风与交付 第 6 节、设计规格-曲风-House学习方式 第 5 节）：画像格式 3 的门控结果、校准参考条件里的
        // 曲风与交付、标定快照的曲风 / House / 交付常数——三处存档口径。通用 + 标准的存档必须与 main 逐字节相同（不写新属性）。
        void testGenreDeliveryStorage()
        {
            beginTest ("T-038: house gate round-trips in format-3 profiles; format-2 profiles write and hash none of it");
            {
                auto profile = oddProfile();
                profile.formatVersion = clayProfileFormatVersion;
                const auto idBefore = computeProfileId (profile);
                const auto textBefore = toXml (profile)->toString();

                profile.houseGate.valid = true;          // 格式 2：门控结果不写、不进画像号
                profile.houseGate.referenceLufs = -7.25f;
                expect (computeProfileId (profile) == idBefore, utf8 ("格式 2 的画像号不含门控结果"));
                expect (toXml (profile)->toString() == textBefore);

                profile.formatVersion = houseProfileFormatVersion;
                profile.houseGate = { true, SegmentMode::bars, -7.25f, 0.41f, 3.0f, 0.6f, 71, 23, 137.5, 44.25, 1 };
                const auto xml3 = toXml (profile);
                MusicProfile back = makeProfile (-9.0f, -1.0f);
                expect (fromXml (*xml3, back));
                expect (bitwiseEqual (back, profile), utf8 ("格式 3 画像往返不逐位相同"));
                expect (back.houseGate.valid && back.houseGate.mode == SegmentMode::bars && back.houseGate.fullSegments == 23
                            && back.houseGate.heardSeconds == 137.5 && back.houseGate.reopenCount == 1);
                expect (computeProfileId (back) != idBefore, utf8 ("格式 3 的门控结果进画像号"));

                expectRejected<MusicProfile> (*xml3, [] (juce::XmlElement& x) { x.setAttribute ("houseGateMode", 3); }, back,
                                              utf8 ("分段方式越界"));
                expectRejected<MusicProfile> (*xml3, [] (juce::XmlElement& x) { x.removeAttribute ("houseFullSegments"); }, back,
                                              utf8 ("格式 3 缺门控字段"));

                MusicProfile fromFormat2 = back;          // 格式 2 读入：门控结果清成默认（House 档下视为锁定条件未满足）
                expect (fromXml (*juce::parseXML (textBefore), fromFormat2));
                expect (! fromFormat2.houseGate.valid && fromFormat2.houseGate.heardSegments == 0 && fromFormat2.formatVersion == 2);
            }

            beginTest ("T-038: calibration reference stores genre and delivery only when not General / Standard");
            {
                CommittedState state;
                state.calibration.valid = true;
                state.calibration.reference = { 0xdeadbeefu, Style::punchy, 63.0f, 0x12345678u, -1.5f, 3.5f, 44100.0 };
                state.constants = factoryConstants();

                const auto plainText = toXml (state)->toString();
                expect (! plainText.contains (" genre=") && ! plainText.contains (" delivery="), utf8 ("通用 + 标准不写新属性"));

                state.calibration.reference.genre = Genre::techHouse;
                state.calibration.reference.delivery = Delivery::club;
                const auto xml = toXml (state);
                CommittedState back;
                expect (fromXml (*xml, back));
                expect (bitwiseEqual (back, state));
                expect (back.calibration.reference.genre == Genre::techHouse && back.calibration.reference.delivery == Delivery::club);

                CommittedState legacy;                     // 缺属性 = 通用 / 标准
                expect (fromXml (*juce::parseXML (plainText), legacy));
                expect (legacy.calibration.reference.genre == Genre::general && legacy.calibration.reference.delivery == Delivery::standard);

                expectRejected<CommittedState> (*xml, [] (juce::XmlElement& x) { x.getChildByName ("CALIBRATION")->setAttribute ("genre", 4); },
                                                back, utf8 ("曲风越界"));
                expectRejected<CommittedState> (*xml, [] (juce::XmlElement& x) { x.getChildByName ("CALIBRATION")->setAttribute ("delivery", 3); },
                                                back, utf8 ("交付越界"));

                auto a = state.calibration.reference, b = a;
                expect (GlazeCalibrator::referenceMatches (a, b));
                b.delivery = Delivery::streaming;
                expect (! GlazeCalibrator::referenceMatches (a, b), utf8 ("换交付 → 参考条件不再适用"));
                b = a;
                b.genre = Genre::deepHouse;
                expect (! GlazeCalibrator::referenceMatches (a, b), utf8 ("换曲风 → 参考条件不再适用"));
            }

            beginTest ("T-038: constants without genre / house / delivery fields read the factory values; present but broken is rejected");
            {
                const auto& factory = factoryConstants();
                expectEquals (factory.deepHouse.toneReferenceSlopeDbPerOct, -3.5f);
                expectEquals (factory.techHouse.toneReferenceSlopeDbPerOct, -2.5f);
                expectEquals (factory.melodicProgressive.toneReferenceSlopeDbPerOct, -3.0f);
                for (const auto* g : { &factory.deepHouse, &factory.techHouse, &factory.melodicProgressive })
                    expect (g->toneMaxBrightDb == 0.4f && g->clayLowCutTargetHz == 25.0f && g->clayWidthStyleFactor == 0.0f);
                expect (genreConstantsFor (factory, Genre::general) == nullptr);
                expect (genreConstantsFor (factory, Genre::techHouse) == &factory.techHouse);
                expect (factory.house.lockSegments == 64 && factory.house.requiredFullSegments == 8 && factory.delivery.clubTargetLufs == -7.0f);

                const auto good = toXml (factory);
                expect (good->hasAttribute ("deepHouse_toneReferenceSlopeDbPerOct") && good->hasAttribute ("house_lockSegments")
                            && good->hasAttribute ("delivery_streamingTruePeakCapDbtp"));

                juce::XmlElement legacy (*good);
                juce::StringArray drop;
                for (int i = 0; i < legacy.getNumAttributes(); ++i)
                {
                    const auto attribute = legacy.getAttributeName (i);
                    if (attribute.startsWith ("deepHouse_") || attribute.startsWith ("techHouse_") || attribute.startsWith ("melodicProgressive_")
                        || attribute.startsWith ("house_") || attribute.startsWith ("delivery_"))
                        drop.add (attribute);
                }
                for (const auto& attribute : drop)
                    legacy.removeAttribute (attribute);

                auto out = factory;                         // 先弄乱，确认缺字段时取的是出厂值，不是沿用 out 里原来的值
                out.house.lockSegments = 7;
                out.techHouse.toneReferenceSlopeDbPerOct = 9.0f;
                out.delivery.clubMaxLufs = 0.0f;
                expect (fromXml (legacy, out));
                expect (bitwiseEqual (out, factory), utf8 ("旧快照缺 T-038 字段时按出厂值读"));

                expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("house_lockSegments", "x"); }, factory,
                                           utf8 ("House 常数存在但损坏"));
            }
        }

        //==============================================================================
        // ---- T-038 阶段 2（设计规格-曲风与交付 第 3.1、4.1、4.2 节；ADR-026）：决策器的曲风覆盖、Tone 往亮上限与交付目标 ----
        // 出厂起点值（GlazeConstants.h / .cpp）：House 子类的 Tone 参考斜率 Deep −3.5 / Tech −2.5 / Melodic-Progressive −3.0 dB/oct、往亮上限
        // +0.4 dB、低切目标 25 Hz、加宽系数 0；风格（温润 / 冲击 / 通透）的参考斜率 −3.5 / −2.5 / −2.5、Tone 上限都是 0.75 dB、偏好响度
        // −8.5 / −9 / −10；交付 流媒体 −10（−12 ～ −8）、俱乐部 −7（−9 ～ −5），风格偏移参考 −9（= 冲击）。期望值按规格公式手算，注释写出
        // 算式；置信度满、GLAZE 100 时强度 GLAZE × κ = 1。

        static Plan planWithGenre (const MusicProfile* profile, Style style, float glaze, Genre genre, Delivery delivery,
                                   const Constants* constants = nullptr, const StyleWeights* locked = nullptr)
        {
            PlanInputs in;
            in.profile = profile;
            in.style = style;
            in.genre = genre;
            in.delivery = delivery;
            in.glazePercent = glaze;
            in.manual = defaultManual();
            in.constants = constants;
            in.lockedWeights = locked;
            return makePlan (in);
        }

        static PlanInputs scenarioInputs (const Scenario& s, float glaze, Genre genre, Delivery delivery, const Constants* constants = nullptr)
        {
            PlanInputs in;
            in.profile = s.hasProfile ? &s.profile : nullptr;
            in.style = s.style;
            in.genre = genre;
            in.delivery = delivery;
            in.glazePercent = glaze;
            in.manual = s.manual;
            in.maxGainDb = s.maxGainDb;
            in.constants = constants;
            return in;
        }

        static Plan planWithGenre (const Scenario& s, float glaze, Genre genre, Delivery delivery, const Constants* constants = nullptr)
        {
            return makePlan (scenarioInputs (s, glaze, genre, delivery, constants));
        }

        // StyleConstants 的全部字段逐位相同（字段数由 static_assert 守住：加了字段要在这里补上，「其余字段不变」才查得全）。
        static bool sameStyleConstants (const StyleConstants& a, const StyleConstants& b) noexcept
        {
            static_assert (sizeof (StyleConstants) == 21 * sizeof (float), "StyleConstants gained a field: compare it here too");

            return sameBits (a.preferredLufs, b.preferredLufs) && sameBits (a.heatDriveCap, b.heatDriveCap)
                && sameBits (a.glueGrMeanCapDb, b.glueGrMeanCapDb) && sameBits (a.glueGrP95CapDb, b.glueGrP95CapDb)
                && sameBits (a.clipDepthCapDb, b.clipDepthCapDb) && sameBits (a.clipActivityCap, b.clipActivityCap)
                && sameBits (a.limitGrMeanCapDb, b.limitGrMeanCapDb) && sameBits (a.limitGrP95CapDb, b.limitGrP95CapDb)
                && sameBits (a.limitGrPeakCapDb, b.limitGrPeakCapDb) && sameBits (a.recoverCap, b.recoverCap)
                && sameBits (a.tameCap, b.tameCap) && sameBits (a.toneMaxDb, b.toneMaxDb)
                && sameBits (a.shareHeat, b.shareHeat) && sameBits (a.shareGlue, b.shareGlue)
                && sameBits (a.shareClip, b.shareClip) && sameBits (a.shareLimit, b.shareLimit)
                && sameBits (a.heatPeakReductionCapDb, b.heatPeakReductionCapDb)
                && sameBits (a.toneReferenceSlopeDbPerOct, b.toneReferenceSlopeDbPerOct)
                && sameBits (a.clayLowCutTargetHz, b.clayLowCutTargetHz) && sameBits (a.clayWidthStyleFactor, b.clayWidthStyleFactor)
                && sameBits (a.automaticHeatFloor, b.automaticHeatFloor);
        }

        // 方案里除 Tone 增量以外的字段逐位相同（samePlan 本来就不比 Clay）。
        static bool sameExceptTone (const Plan& a, const Plan& b) noexcept
        {
            auto copy = a;
            copy.increments.colorToneDb = b.increments.colorToneDb;
            return samePlan (copy, b);
        }

        static bool sameUsage (const ManualChainUsage& a, const ManualChainUsage& b) noexcept
        {
            return sameBits (a.heatAmount, b.heatAmount) && sameBits (a.glueGrMeanDb, b.glueGrMeanDb) && sameBits (a.glueGrP95Db, b.glueGrP95Db)
                && sameBits (a.clipDepthDb, b.clipDepthDb) && sameBits (a.clipActivity, b.clipActivity)
                && sameBits (a.limitGrPeakDb, b.limitGrPeakDb) && sameBits (a.limitGrMeanDb, b.limitGrMeanDb)
                && sameBits (a.limitGrP95Db, b.limitGrP95Db) && a.overBudgetMask == b.overBudgetMask;
        }

        static juce::String genreLabel (Genre genre, Style style)
        {
            return utf8 ("曲风 ") + juce::String ((int) genre) + utf8 ("、风格 ") + juce::String ((int) style);
        }

        void testGenreAndDeliveryPlanning()
        {
            const auto& c = factoryConstants();
            const Genre houses[] { Genre::deepHouse, Genre::techHouse, Genre::melodicProgressive };
            const Style styles[] { Style::automatic, Style::warm, Style::punchy, Style::clean };
            const StyleWeights mixedWeights { 0.2f, 0.5f, 0.3f };
            const auto nan = std::numeric_limits<float>::quiet_NaN();
            const auto inf = std::numeric_limits<float>::infinity();

            // 弄坏的曲风常数与交付常数（NaN、±∞、颠倒的范围、离谱的目标）：旧路径一个都不许读，新路径照样给有限、在范围内的方案。
            auto badGenre = factoryConstants();

            for (auto* g : { &badGenre.deepHouse, &badGenre.techHouse, &badGenre.melodicProgressive })
            {
                g->toneReferenceSlopeDbPerOct = nan;
                g->toneMaxBrightDb = inf;
                g->clayLowCutTargetHz = 60.0f;
                g->clayWidthStyleFactor = 1.0f;
            }

            auto badDelivery = factoryConstants();
            badDelivery.delivery.streamingTargetLufs = nan;
            badDelivery.delivery.streamingMinLufs = -5.0f;    // 颠倒的范围
            badDelivery.delivery.streamingMaxLufs = -9.0f;
            badDelivery.delivery.clubTargetLufs = inf;
            badDelivery.delivery.clubMinLufs = nan;
            badDelivery.delivery.clubMaxLufs = -inf;
            badDelivery.delivery.styleOffsetReferenceLufs = 40.0f;

            auto badBoth = badGenre;
            badBoth.delivery = badDelivery.delivery;

            beginTest ("T-038: House genres override the Tone reference, low-cut target and width factor after the style blend; General overrides nothing");
            {
                // blendConstants 三参数版：House 只换三项音色字段（不参与混合，与风格无关），其余 18 项（动态预算、偏好响度、Tone 往暗上限、
                // Heat 底量）逐位等于两参数版；通用档与越界的曲风值一项都不换。
                for (const auto& w : { weightsFor (Style::warm, {}), weightsFor (Style::punchy, {}), weightsFor (Style::clean, {}), mixedWeights })
                {
                    const auto plain = blendConstants (c, w);
                    expect (sameStyleConstants (blendConstants (c, w, Genre::general), plain), utf8 ("通用档不覆盖任何一项"));
                    expect (sameStyleConstants (blendConstants (c, w, static_cast<Genre> (9)), plain), utf8 ("越界的曲风值按通用"));

                    for (auto genre : houses)
                    {
                        const auto& own = *genreConstantsFor (c, genre);
                        auto expected = plain;
                        expected.toneReferenceSlopeDbPerOct = own.toneReferenceSlopeDbPerOct;
                        expected.clayLowCutTargetHz = own.clayLowCutTargetHz;
                        expected.clayWidthStyleFactor = own.clayWidthStyleFactor;
                        expect (sameStyleConstants (blendConstants (c, w, genre), expected),
                                utf8 ("House 只覆盖三项音色字段：曲风 ") + juce::String ((int) genre));
                    }
                }

                expectEquals (blendConstants (c, weightsFor (Style::punchy, {}), Genre::deepHouse).toneReferenceSlopeDbPerOct, -3.5f);
                expectEquals (blendConstants (c, mixedWeights, Genre::techHouse).toneReferenceSlopeDbPerOct, -2.5f);
                expectEquals (blendConstants (c, weightsFor (Style::warm, {}), Genre::melodicProgressive).toneReferenceSlopeDbPerOct, -3.0f);
                expectEquals (blendConstants (c, weightsFor (Style::clean, {}), Genre::melodicProgressive).clayLowCutTargetHz, 25.0f);
                expectEquals (blendConstants (c, weightsFor (Style::clean, {}), Genre::techHouse).clayWidthStyleFactor, 0.0f);

                // 方案的 Tone 参考：画像斜率比子类参考暗 1 dB/oct（超出容忍区 0.65）→ 往亮 (1 − 0.35) × 0.5 = 0.325 dB；亮 1 dB/oct → −0.325 dB；
                // 正好等于参考 → 0（死区）。四种风格同一个值——温润的参考 −3.5、冲击 / 通透的 −2.5 在 House 档都不再用。
                const auto inside = (1.0f - automaticToneSlopeDeadbandDbPerOct) * c.toneDbPerSlopeDb;

                for (auto genre : houses)
                {
                    const auto reference = genreConstantsFor (c, genre)->toneReferenceSlopeDbPerOct;

                    for (auto style : styles)
                    {
                        const auto toneAt = [genre, style] (float slope)
                        {
                            auto p = makeProfile (-20.0f, -8.0f);
                            setFeature (p.spectralSlopeDbPerOct, slope);
                            return planWithGenre (&p, style, 100.0f, genre, Delivery::standard).increments.colorToneDb;
                        };

                        const auto label = genreLabel (genre, style);
                        expectWithinAbsoluteError (toneAt (reference - 1.0f), inside, 1.0e-6f, label + utf8 ("：暗 1 dB/oct → 往亮"));
                        expectWithinAbsoluteError (toneAt (reference + 1.0f), -inside, 1.0e-6f, label + utf8 ("：亮 1 dB/oct → 往暗"));
                        expectEquals (toneAt (reference), 0.0f, label + utf8 ("：等于子类参考 → 不动"));
                    }
                }

                // 对照：通用档照旧按风格的参考。斜率 −4.5 的画像：通用 + 冲击（参考 −2.5）差 2 dB/oct → (2 − 0.35) × 0.5 = 0.825，顶到 0.75；
                // Deep + 冲击（参考 −3.5）只差 1 → 0.325。
                auto darkMix = makeProfile (-20.0f, -8.0f);
                setFeature (darkMix.spectralSlopeDbPerOct, -4.5f);
                expectWithinAbsoluteError (planFor (&darkMix, Style::punchy, 100.0f).increments.colorToneDb, 0.75f, 1.0e-6f, utf8 ("通用 + 冲击按风格参考"));
                expectWithinAbsoluteError (planWithGenre (&darkMix, Style::punchy, 100.0f, Genre::deepHouse, Delivery::standard).increments.colorToneDb,
                                           inside, 1.0e-6f, utf8 ("Deep + 冲击按子类参考"));

                // 修坯：clayProfile（三项需要度都是 0.5、代价 0）。House 低切 = 10 · (25 / 10)^0.5 = 15.8114 Hz、不加宽、不收单，四种风格相同；
                // 真次声 −28 dB（need 1，k = 1）→ 低切正好是目标 25 Hz。通用档照旧按风格（testClayFormulas：12.6491 / 13.4164 / 14.1421 Hz，
                // 加宽 0.3 / 0.45 / 0.75 dB）。
                const auto clay = clayProfile();
                auto subsonic = clayProfile();
                subsonic.subShareDb.value = -28.0f;

                for (auto genre : houses)
                    for (auto style : styles)
                    {
                        const auto label = genreLabel (genre, style);
                        const auto house = planWithGenre (&clay, style, 100.0f, genre, Delivery::standard).clay;
                        expectWithinAbsoluteError (house.lowCutHz, 15.811388f, 1.0e-4f, label + utf8 ("：低切按 25 Hz 目标"));
                        expectEquals (house.widthDb, 0.0f, label + utf8 ("：House 不自动加宽"));
                        expectEquals (house.monoAmount, 0.0f, label + utf8 ("：自动不收单（ADR-015）"));
                        expectEquals (maskOf (house), 0, label + utf8 ("：代价 0 dB → 不置代价位"));
                        expectWithinAbsoluteError (planWithGenre (&subsonic, style, 100.0f, genre, Delivery::standard).clay.lowCutHz, 25.0f, 1.0e-4f,
                                                   label + utf8 ("：need 1 → 低切 = 子类目标"));
                    }

                expect (sameClay (planWithGenre (&clay, Style::warm, 100.0f, Genre::general, Delivery::standard).clay, planFor (&clay, Style::warm, 100.0f).clay),
                        utf8 ("通用档的修坯照旧"));
                expectWithinAbsoluteError (planFor (&clay, Style::clean, 100.0f).clay.widthDb, 0.75f, 1.0e-6f, utf8 ("阳性对照：通用 + 通透会加宽"));

                // 代价位：加宽试过会抬高峰值 0.8 dB（系数 0）时，通用档「本应加宽、被代价否决」置加宽位；House 本来就不加宽，不置这一位
                // （界面不该说「试过会抬高峰值」）。低切代价 0.4 dB（系数 0.5）两边都置低切位。
                auto costly = clayProfile();
                setClayCosts (costly, 0.4f, 0.0f, 0.8f);
                expectEquals (maskOf (planFor (&costly, Style::clean, 100.0f).clay), (int) (clayCostLimitedLowCut | clayCostLimitedWidth));
                expectEquals (maskOf (planWithGenre (&costly, Style::clean, 100.0f, Genre::deepHouse, Delivery::standard).clay), (int) clayCostLimitedLowCut);
            }

            beginTest ("T-038: a House genre changes only the automatic Tone and Clay; level decisions and the manual-chain usage match General");
            {
                // 规格第 2 节「曲风只换目标、不换流程」：动态预算（Heat / Glue / Clip / Limit 上限、份额、Recover / Tame 上限）与偏好响度归风格与
                // 交付。同一份输入下，House 与通用的方案除 Tone 增量与 Clay 外逐位相同（增量、目标、权重、各受限标志、manualBudgetMask、版本）；
                // 手动链单独的用量（保护层与上游监视的口径）也逐位相同，且与交付无关。
                juce::Random random (0x5eed7038);
                int differences = 0, usageDifferences = 0, toneDiffers = 0, clayDiffers = 0;

                for (int trial = 0; trial < 60; ++trial)
                {
                    auto scenario = randomScenario (random);
                    addRandomClayFeatures (scenario.profile, random);
                    const auto generalUsage = estimateManualChainUsage (scenarioInputs (scenario, 60.0f, Genre::general, Delivery::standard));

                    for (auto delivery : { Delivery::standard, Delivery::streaming, Delivery::club })
                    {
                        for (auto glaze : { 0.0f, 25.0f, 70.0f, 100.0f })
                        {
                            const auto general = planWithGenre (scenario, glaze, Genre::general, delivery);

                            for (auto genre : houses)
                            {
                                const auto house = planWithGenre (scenario, glaze, genre, delivery);
                                differences += sameExceptTone (house, general) ? 0 : 1;
                                toneDiffers += sameBits (house.increments.colorToneDb, general.increments.colorToneDb) ? 0 : 1;
                                clayDiffers += sameClay (house.clay, general.clay) ? 0 : 1;
                            }
                        }

                        for (auto genre : { Genre::general, Genre::deepHouse, Genre::techHouse, Genre::melodicProgressive })
                            usageDifferences += sameUsage (estimateManualChainUsage (scenarioInputs (scenario, 60.0f, genre, delivery)), generalUsage) ? 0 : 1;
                    }
                }

                expectEquals (differences, 0, utf8 ("House 档只许改 Tone 与 Clay"));
                expectEquals (usageDifferences, 0, utf8 ("手动链单独的用量与曲风、交付无关"));
                expect (toneDiffers > 0 && clayDiffers > 0, utf8 ("阳性对照：曲风确实改了 Tone 与 Clay"));
            }

            beginTest ("T-038: House Tone brightens by at most toneMaxBrightDb (+0.4 dB) x GLAZE x kappa; darkening keeps toneMaxDb");
            {
                // 画像比子类参考暗 4 dB/oct：满强度要往亮 (4 − 0.35) × 0.5 = 1.825 dB → 通用档会顶到 toneMaxDb 0.75，House 顶到 +0.4；
                // 亮 4 dB/oct：往暗 −1.825 → 照旧顶到 −0.75。缩放口径与 toneMaxDb 相同：GLAZE 50 → 一半；置信度 0.4（κ = 0.675、斜率按可信度
                // 0.5 向参考收拢，仍要 0.825 > 0.4）→ 0.4 × κ。
                for (auto genre : houses)
                {
                    const auto reference = genreConstantsFor (c, genre)->toneReferenceSlopeDbPerOct;
                    auto dark = makeProfile (-20.0f, -8.0f);
                    setFeature (dark.spectralSlopeDbPerOct, reference - 4.0f);
                    auto bright = makeProfile (-20.0f, -8.0f);
                    setFeature (bright.spectralSlopeDbPerOct, reference + 4.0f);
                    auto unsureDark = makeProfile (-20.0f, -8.0f, 5.0f, 0.4f);
                    setFeature (unsureDark.spectralSlopeDbPerOct, reference - 4.0f, 0.4f);
                    const auto kappa = confidenceScale (&unsureDark, c);

                    for (auto style : styles)
                    {
                        const auto label = genreLabel (genre, style);
                        const auto toneOf = [genre, style] (const MusicProfile& p, float glaze)
                        {
                            return planWithGenre (&p, style, glaze, genre, Delivery::standard).increments.colorToneDb;
                        };

                        expectWithinAbsoluteError (toneOf (dark, 100.0f), 0.4f, 1.0e-6f, label + utf8 ("：往亮封顶 +0.4"));
                        expectWithinAbsoluteError (toneOf (dark, 50.0f), 0.2f, 1.0e-6f, label + utf8 ("：往亮 × GLAZE"));
                        expectWithinAbsoluteError (toneOf (unsureDark, 100.0f), 0.4f * kappa, 1.0e-6f, label + utf8 ("：往亮 × κ"));
                        expectWithinAbsoluteError (toneOf (bright, 100.0f), -0.75f, 1.0e-6f, label + utf8 ("：往暗仍是 toneMaxDb"));
                        expectWithinAbsoluteError (toneOf (bright, 50.0f), -0.375f, 1.0e-6f, label + utf8 ("：往暗 × GLAZE"));
                    }
                }

                // Tech House 的参考 −2.5 与冲击 / 通透相同：同一份画像下，往暗与上限以内的修正和通用档逐位相同，只有往亮的上限不同。
                // 斜率 1.5 / −1.5 / −2.5 / −3.0 / −3.5 → 满强度 −0.75（往暗封顶）/ −0.325 / 0（死区）/ +0.075 / +0.325。
                for (auto style : { Style::punchy, Style::clean })
                {
                    for (auto slope : { 1.5f, -1.5f, -2.5f, -3.0f, -3.5f })
                    {
                        auto p = makeProfile (-20.0f, -8.0f);
                        setFeature (p.spectralSlopeDbPerOct, slope);
                        expect (sameBits (planWithGenre (&p, style, 80.0f, Genre::techHouse, Delivery::standard).increments.colorToneDb,
                                          planFor (&p, style, 80.0f).increments.colorToneDb),
                                utf8 ("Tech 与通用同参考：上限以内逐位相同，斜率 ") + juce::String (slope));
                    }

                    auto veryDark = makeProfile (-20.0f, -8.0f);
                    setFeature (veryDark.spectralSlopeDbPerOct, -6.5f);
                    expectWithinAbsoluteError (planFor (&veryDark, style, 100.0f).increments.colorToneDb, 0.75f, 1.0e-6f, utf8 ("通用档往亮仍到 0.75"));
                    expectWithinAbsoluteError (planWithGenre (&veryDark, style, 100.0f, Genre::techHouse, Delivery::standard).increments.colorToneDb, 0.4f,
                                               1.0e-6f, utf8 ("Tech 往亮到 0.4"));
                }

                // 快照里的往亮上限（Deep、温润，参考 −3.5）：0、负数、NaN → 不许往亮（往暗照旧 −0.75）；0.6 → 0.6（House 档往亮的上限就是
                // 这个字段，与 toneMaxDb 无关）；+∞ → 不设上限（与 toneMaxDb 本身的口径一致），满强度 1.825。
                // 风格的 toneMaxDb ≤ 0 → 整项 Tone 都不动（曲风只换往亮的边界，不重开风格关掉的 Tone）。
                auto deepDark = makeProfile (-20.0f, -8.0f);
                setFeature (deepDark.spectralSlopeDbPerOct, -3.5f - 4.0f);
                auto deepBright = makeProfile (-20.0f, -8.0f);
                setFeature (deepBright.spectralSlopeDbPerOct, -3.5f + 4.0f);

                const struct { float cap, resolved, tone; } brightCaps[] =
                {
                    { 0.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { nan, 0.0f, 0.0f }, { 0.6f, 0.6f, 0.6f }, { inf, inf, 1.825f }
                };

                for (const auto& b : brightCaps)
                {
                    auto snapshot = factoryConstants();
                    snapshot.deepHouse.toneMaxBrightDb = b.cap;
                    const auto label = utf8 ("往亮上限 ") + juce::String (b.cap);
                    expectEquals (toneBrightCapDb (blendConstants (snapshot, weightsFor (Style::warm, {}), Genre::deepHouse), snapshot, Genre::deepHouse),
                                  b.resolved, label);
                    expectWithinAbsoluteError (planWithGenre (&deepDark, Style::warm, 100.0f, Genre::deepHouse, Delivery::standard, &snapshot).increments.colorToneDb,
                                               b.tone, 1.0e-5f, label);
                    expectWithinAbsoluteError (planWithGenre (&deepBright, Style::warm, 100.0f, Genre::deepHouse, Delivery::standard, &snapshot).increments.colorToneDb,
                                               -0.75f, 1.0e-6f, label + utf8 ("：往暗不受影响"));
                }

                auto noTone = factoryConstants();
                noTone.warm.toneMaxDb = 0.0f;
                expectEquals (planWithGenre (&deepDark, Style::warm, 100.0f, Genre::deepHouse, Delivery::standard, &noTone).increments.colorToneDb, 0.0f,
                              utf8 ("风格关掉 Tone 时 House 也不往亮"));
                expectEquals (planWithGenre (&deepBright, Style::warm, 100.0f, Genre::deepHouse, Delivery::standard, &noTone).increments.colorToneDb, 0.0f,
                              utf8 ("风格关掉 Tone 时 House 也不往暗"));

                // toneBrightCapDb：通用档（及越界的曲风值）就是混合后的 toneMaxDb（逐位），House 子类是 +0.4。
                for (const auto& w : { weightsFor (Style::warm, {}), mixedWeights })
                {
                    const auto plain = blendConstants (c, w);
                    expect (sameBits (toneBrightCapDb (plain, c, Genre::general), plain.toneMaxDb), utf8 ("通用档往亮上限 = toneMaxDb"));
                    expect (sameBits (toneBrightCapDb (plain, c, static_cast<Genre> (9)), plain.toneMaxDb), utf8 ("越界的曲风值按通用"));

                    for (auto genre : houses)
                        expectEquals (toneBrightCapDb (blendConstants (c, w, genre), c, genre), 0.4f);
                }
            }

            beginTest ("T-038: streaming / club aim at the delivery target plus the style offset, minus the LRA correction, clamped to the delivery range");
            {
                // 输入 −24 LUFS、真峰值 −14、全可信：离各档目标都远多于 3 dB 的最小推响量，GLAZE 100 时 requestedTargetLufs 就是 L_pref
                // （= 输入 + 请求；手动中性、峰值在 Ceiling 下，手动链净增益 0）。
                //   风格偏移 = L_style − (−9)：温润 +0.5、冲击 0、通透 −1 → 流媒体 −9.5 / −10 / −11、俱乐部 −6.5 / −7 / −8；标准照旧 −8.5 / −9 / −10；
                //   LRA 12（修正 (12 − 6) / 3 = 2）→ 流媒体 −11.5 / −12 / −13 → −12（下限）、俱乐部 −8.5 / −9 / −10 → −9；
                //   LRA 18（修正 4）→ 流媒体全是 −12、俱乐部全是 −9（标准 −12.5 / −13 / −14 仍在 −14 ～ −7 里）。
                const auto requestedTarget = [] (float lra, Style style, Delivery delivery, const Constants* constants, const StyleWeights* locked)
                {
                    const auto p = makeProfile (-24.0f, -14.0f, lra);
                    return planWithGenre (&p, style, 100.0f, Genre::general, delivery, constants, locked).requestedTargetLufs;
                };

                const struct { Style style; float lra, standard, streaming, club; } table[] =
                {
                    { Style::warm,    5.0f,  -8.5f,  -9.5f, -6.5f },
                    { Style::punchy,  5.0f,  -9.0f, -10.0f, -7.0f },
                    { Style::clean,   5.0f, -10.0f, -11.0f, -8.0f },
                    { Style::warm,   12.0f, -10.5f, -11.5f, -8.5f },
                    { Style::punchy, 12.0f, -11.0f, -12.0f, -9.0f },
                    { Style::clean,  12.0f, -12.0f, -12.0f, -9.0f },
                    { Style::warm,   18.0f, -12.5f, -12.0f, -9.0f },
                    { Style::punchy, 18.0f, -13.0f, -12.0f, -9.0f },
                    { Style::clean,  18.0f, -14.0f, -12.0f, -9.0f }
                };

                for (const auto& row : table)
                {
                    const auto label = utf8 ("风格 ") + juce::String ((int) row.style) + utf8 ("、LRA ") + juce::String (row.lra);
                    expectWithinAbsoluteError (requestedTarget (row.lra, row.style, Delivery::standard, nullptr, nullptr), row.standard, 1.0e-4f,
                                               label + utf8 ("：标准"));
                    expectWithinAbsoluteError (requestedTarget (row.lra, row.style, Delivery::streaming, nullptr, nullptr), row.streaming, 1.0e-4f,
                                               label + utf8 ("：流媒体"));
                    expectWithinAbsoluteError (requestedTarget (row.lra, row.style, Delivery::club, nullptr, nullptr), row.club, 1.0e-4f,
                                               label + utf8 ("：俱乐部"));
                }

                // 自动风格：偏移按权重混合。锁定 {0.2, 0.5, 0.3} → L_style = 0.2 × (−8.5) + 0.5 × (−9) + 0.3 × (−10) = −9.2，偏移 −0.2 →
                // 流媒体 −10.2、俱乐部 −7.2、标准 −9.2。
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::automatic, Delivery::streaming, nullptr, &mixedWeights), -10.2f, 1.0e-4f);
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::automatic, Delivery::club, nullptr, &mixedWeights), -7.2f, 1.0e-4f);
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::automatic, Delivery::standard, nullptr, &mixedWeights), -9.2f, 1.0e-4f);

                // 上限：温润的偏好改成 −4（偏移 +5）→ 流媒体 −5 钳到 −8、俱乐部 −2 钳到 −5、标准 −4 钳到 maxPreferred −7。
                auto loudStyle = factoryConstants();
                loudStyle.warm.preferredLufs = -4.0f;
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::warm, Delivery::streaming, &loudStyle, nullptr), -8.0f, 1.0e-4f, utf8 ("流媒体上限"));
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::warm, Delivery::club, &loudStyle, nullptr), -5.0f, 1.0e-4f, utf8 ("俱乐部上限"));
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::warm, Delivery::standard, &loudStyle, nullptr), -7.0f, 1.0e-4f, utf8 ("标准上限"));

                // 钳位只用该档的范围：标准档的 minPreferred / maxPreferred 改成 −30 / −20，标准被钳到 −20，流媒体 / 俱乐部不受影响（−10 / −7）。
                auto narrow = factoryConstants();
                narrow.minPreferredLufs = -30.0f;
                narrow.maxPreferredLufs = -20.0f;
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::punchy, Delivery::standard, &narrow, nullptr), -20.0f, 1.0e-4f);
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::punchy, Delivery::streaming, &narrow, nullptr), -10.0f, 1.0e-4f,
                                           utf8 ("流媒体不看 minPreferred / maxPreferred"));
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::punchy, Delivery::club, &narrow, nullptr), -7.0f, 1.0e-4f,
                                           utf8 ("俱乐部不看 minPreferred / maxPreferred"));

                // 档位常数取自快照：流媒体目标 −11、风格偏移参考 −8.5（温润 = 0）→ 冲击偏移 −0.5 → 流媒体 −11.5、俱乐部 −7.5；温润俱乐部 −7。
                auto custom = factoryConstants();
                custom.delivery.streamingTargetLufs = -11.0f;
                custom.delivery.styleOffsetReferenceLufs = -8.5f;
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::punchy, Delivery::streaming, &custom, nullptr), -11.5f, 1.0e-4f);
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::punchy, Delivery::club, &custom, nullptr), -7.5f, 1.0e-4f);
                expectWithinAbsoluteError (requestedTarget (5.0f, Style::warm, Delivery::club, &custom, nullptr), -7.0f, 1.0e-4f);

                // 方案字段口径一致（GlazePlanner.h「方案字段的口径」）：目标 = 画像 I + 对应的净增益，effective 不超过 requested；GLAZE 50 的请求
                // 是满强度的一半；GLAZE 0 没有方案，三档逐位相同。有效增益随请求单调：俱乐部 ≥ 标准 ≥ 流媒体（受同一份风格预算约束）。
                const auto profile = makeProfile (-24.0f, -14.0f, 5.0f);
                const auto atZero = planWithGenre (&profile, Style::punchy, 0.0f, Genre::general, Delivery::standard);

                for (auto delivery : { Delivery::standard, Delivery::streaming, Delivery::club })
                {
                    const auto label = utf8 ("交付 ") + juce::String ((int) delivery);
                    const auto full = planWithGenre (&profile, Style::punchy, 100.0f, Genre::general, delivery);
                    const auto half = planWithGenre (&profile, Style::punchy, 50.0f, Genre::general, delivery);
                    expect (sameBits (full.requestedTargetLufs, -24.0f + full.requestedGainDb), label + utf8 ("：requested 目标 = I + 请求增益"));
                    expect (sameBits (full.effectiveTargetLufs, -24.0f + full.effectiveGainDb), label + utf8 ("：effective 目标 = I + 有效增益"));
                    expect (full.effectiveGainDb <= full.requestedGainDb + 1.0e-4f && full.effectiveTargetLufs <= full.requestedTargetLufs + 1.0e-4f,
                            label + utf8 ("：effective 不超过 requested"));
                    expectWithinAbsoluteError (half.requestedGainDb, 0.5f * full.requestedGainDb, 1.0e-4f, label + utf8 ("：请求随 GLAZE 线性"));
                    expect (samePlan (planWithGenre (&profile, Style::punchy, 0.0f, Genre::general, delivery), atZero), label + utf8 ("：GLAZE 0 与交付无关"));
                }

                const auto club = planWithGenre (&profile, Style::punchy, 100.0f, Genre::general, Delivery::club);
                const auto standard = planWithGenre (&profile, Style::punchy, 100.0f, Genre::general, Delivery::standard);
                const auto streaming = planWithGenre (&profile, Style::punchy, 100.0f, Genre::general, Delivery::streaming);
                expect (club.requestedTargetLufs > standard.requestedTargetLufs && standard.requestedTargetLufs > streaming.requestedTargetLufs);
                expect (club.effectiveGainDb >= standard.effectiveGainDb - 1.0e-3f && standard.effectiveGainDb >= streaming.effectiveGainDb - 1.0e-3f,
                        utf8 ("有效增益随交付目标单调"));

                // 交付只换目标响度：Tone、Clay、Recover / Tame、风格权重与交付无关（两版母带的音色决策一致，规格第 4.3 节）。
                auto voiced = clayProfile();
                setFeature (voiced.spectralSlopeDbPerOct, -4.5f);

                for (auto genre : { Genre::general, Genre::deepHouse })
                {
                    const auto reference = planWithGenre (&voiced, Style::automatic, 80.0f, genre, Delivery::standard);

                    for (auto delivery : { Delivery::streaming, Delivery::club })
                    {
                        const auto other = planWithGenre (&voiced, Style::automatic, 80.0f, genre, delivery);
                        expect (sameBits (other.increments.colorToneDb, reference.increments.colorToneDb) && sameClay (other.clay, reference.clay)
                                    && sameBits (other.increments.balanceRecover, reference.increments.balanceRecover)
                                    && sameBits (other.increments.balanceTame, reference.increments.balanceTame)
                                    && sameBits (other.weights.warm, reference.weights.warm) && sameBits (other.weights.punchy, reference.weights.punchy)
                                    && sameBits (other.weights.clean, reference.weights.clean),
                                utf8 ("交付不改音色决策：曲风 ") + juce::String ((int) genre) + utf8 ("、交付 ") + juce::String ((int) delivery));
                    }

                    expect (reference.increments.colorToneDb != 0.0f && ! isNeutral (reference.clay), utf8 ("阳性对照：这份画像会动 Tone 与 Clay"));
                }
            }

            // 集成时追加（A 报告「需 Max 定」一条，主进程按规格「允许范围」定为硬上限）：最少推响量不许把流媒体 / 俱乐部的目标推过该档上限；
            // 输入已经比上限响时不推。标准档照旧保留最少推响量（已够响仍温和推响）。
            beginTest ("T-038: the minimum drive never pushes a streaming / club target past the delivery maximum");
            {
                const auto target = [] (float inputLufs, Delivery delivery)
                {
                    const auto p = makeProfile (inputLufs, inputLufs + 6.0f, 5.0f);
                    return planWithGenre (&p, Style::punchy, 100.0f, Genre::general, delivery, nullptr, nullptr).requestedTargetLufs;
                };

                expect (target (-9.0f, Delivery::streaming) <= -8.0f + 1.0e-4f, utf8 ("流媒体：输入 −9，目标不超过 −8"));
                expect (target (-7.0f, Delivery::streaming) <= -7.0f + 1.0e-4f, utf8 ("流媒体：输入已比上限响，不再推"));
                expect (target (-6.0f, Delivery::club) <= -5.0f + 1.0e-4f, utf8 ("俱乐部：输入 −6，目标不超过 −5"));
                expect (target (-4.0f, Delivery::club) <= -4.0f + 1.0e-4f, utf8 ("俱乐部：输入已比上限响，不再推"));
                expect (target (-9.0f, Delivery::standard) > -9.0f + 1.0f, utf8 ("标准档照旧保留最少推响量"));
            }

            beginTest ("T-038: General + Standard never reads the genre or delivery constants; explicit defaults equal default PlanInputs");
            {
                // 通用 + 标准用哪份快照都与出厂逐位相同（含 Clay），也与不设 genre / delivery 的 PlanInputs 逐位相同；标准档（任何曲风）不读
                // 交付常数；通用档（任何交付）不读曲风常数；越界的枚举值（曲风 9 / −1、交付 5）按通用 / 标准。
                juce::Random random (0x5eed703b);
                int mismatches = 0, checked = 0;

                for (int trial = 0; trial < 40; ++trial)
                {
                    auto scenario = randomScenario (random);
                    addRandomClayFeatures (scenario.profile, random);

                    for (auto glaze : { 0.0f, 30.0f, 100.0f })
                    {
                        PlanInputs plain;    // 不设曲风与交付：默认就是通用 + 标准
                        plain.profile = scenario.hasProfile ? &scenario.profile : nullptr;
                        plain.style = scenario.style;
                        plain.glazePercent = glaze;
                        plain.manual = scenario.manual;
                        plain.maxGainDb = scenario.maxGainDb;
                        const auto reference = makePlan (plain);

                        const auto compare = [&mismatches, &checked] (const Plan& a, const Plan& b)
                        {
                            mismatches += samePlan (a, b) && sameClay (a.clay, b.clay) ? 0 : 1;
                            ++checked;
                        };

                        compare (planWithGenre (scenario, glaze, Genre::general, Delivery::standard), reference);
                        compare (planWithGenre (scenario, glaze, Genre::general, Delivery::standard, &badBoth), reference);
                        compare (planWithGenre (scenario, glaze, static_cast<Genre> (9), static_cast<Delivery> (5)), reference);
                        compare (planWithGenre (scenario, glaze, static_cast<Genre> (-1), Delivery::standard, &badBoth), reference);

                        for (auto genre : houses)
                            compare (planWithGenre (scenario, glaze, genre, Delivery::standard, &badDelivery),
                                     planWithGenre (scenario, glaze, genre, Delivery::standard));

                        for (auto delivery : { Delivery::streaming, Delivery::club })
                            compare (planWithGenre (scenario, glaze, Genre::general, delivery, &badGenre),
                                     planWithGenre (scenario, glaze, Genre::general, delivery));
                    }
                }

                expectEquals (mismatches, 0, juce::String (checked) + utf8 (" 组对照里有不逐位相同的"));
            }

            beginTest ("T-038: every genre and delivery keeps the planner properties: monotone in GLAZE, within ranges, Tone inside its signed caps");
            {
                // 依据：GlazePlanner.h 的保证（随 GLAZE 单调、叠加手动后在参数范围内、方案值有限）在新档位里照样成立；Tone 增量在
                // [−toneMaxDb, toneBrightCapDb] × GLAZE × κ 之内（出厂 House 往亮 ≤ 0.4 × GLAZE × κ）。坏的曲风 / 交付常数（badBoth）同样。
                juce::Random random (0x5eed7039);
                int failures = 0, checked = 0;
                const Constants* snapshots[] { &c, &badBoth };

                for (const auto* snapshot : snapshots)
                    for (int trial = 0; trial < 25; ++trial)
                    {
                        const auto scenario = randomScenario (random);
                        const auto kappa = confidenceScale (scenario.hasProfile ? &scenario.profile : nullptr, *snapshot);

                        for (auto genre : { Genre::general, Genre::deepHouse, Genre::techHouse, Genre::melodicProgressive })
                            for (auto delivery : { Delivery::standard, Delivery::streaming, Delivery::club })
                            {
                                auto previous = planWithGenre (scenario, 0.0f, genre, delivery, snapshot);

                                for (int step = 1; step <= 40; ++step)
                                {
                                    const auto glazePercent = 2.5f * (float) step;
                                    const auto plan = planWithGenre (scenario, glazePercent, genre, delivery, snapshot);
                                    const auto& a = plan.increments;
                                    const auto& b = previous.increments;
                                    const auto sc = blendConstants (*snapshot, plan.weights, genre);
                                    const auto strength = glazePercent * 0.01f * kappa;
                                    const auto p = applyIncrements (scenario.manual, a);

                                    const bool monotone = a.balanceRecover >= b.balanceRecover && a.balanceTame >= b.balanceTame
                                                       && std::abs (a.colorToneDb) >= std::abs (b.colorToneDb) && a.colorToneDb * b.colorToneDb >= 0.0f
                                                       && a.colorGlue >= b.colorGlue && a.colorHeat >= b.colorHeat
                                                       && a.limitGainDb >= b.limitGainDb && a.limitClip >= b.limitClip
                                                       && plan.requestedGainDb >= previous.requestedGainDb && plan.effectiveGainDb >= previous.effectiveGainDb
                                                       && plan.effectiveTargetLufs >= previous.effectiveTargetLufs
                                                       && plan.requestedTargetLufs >= previous.requestedTargetLufs;
                                    const bool toneInside = a.colorToneDb <= toneBrightCapDb (sc, *snapshot, genre) * strength + 1.0e-5f
                                                         && a.colorToneDb >= -sc.toneMaxDb * strength - 1.0e-5f
                                                         && (snapshot != &c || ! isHouse (genre) || a.colorToneDb <= 0.4f * strength + 1.0e-5f);
                                    const bool inRange = std::isfinite (plan.requestedGainDb) && std::isfinite (plan.effectiveGainDb)
                                                      && std::isfinite (plan.requestedTargetLufs) && std::isfinite (plan.effectiveTargetLufs)
                                                      && std::isfinite (plan.estimatedExcessDb) && std::isfinite (a.colorToneDb)
                                                      && std::isfinite (plan.clay.lowCutHz) && std::isfinite (plan.clay.widthDb)
                                                      && p.limit.gainDb >= 0.0f && p.limit.gainDb <= 24.0f && p.color.heat >= 0.0f && p.color.heat <= 100.0f
                                                      && p.color.glue >= 0.0f && p.color.glue <= 100.0f && p.limit.clip >= 0.0f && p.limit.clip <= 100.0f
                                                      && p.color.toneDb >= -3.0f && p.color.toneDb <= 3.0f;

                                    if (! (monotone && toneInside && inRange) && ++failures <= 5)
                                        logMessage (utf8 ("  不满足：快照 ") + (snapshot == &c ? "factory" : "bad") + utf8 ("，曲风 ") + juce::String ((int) genre)
                                                    + utf8 ("，交付 ") + juce::String ((int) delivery) + ", GLAZE " + juce::String (glazePercent)
                                                    + (monotone ? "" : " monotone") + (toneInside ? "" : " tone") + (inRange ? "" : " range"));

                                    ++checked;
                                    previous = plan;
                                }
                            }
                    }

                expectEquals (failures, 0, juce::String (checked) + utf8 (" 个方案里有不满足性质的"));
            }

            beginTest ("T-038: House and delivery planning do not allocate");
            {
                // 依据：GlazePlanner.h「无状态、不分配」（编排器可能每块调 makePlan）；口径同 testNoAllocation（计数先读进局部量再断言）。
                juce::Random random (0x5eed703a);
                std::vector<Scenario> scenarios;

                for (int i = 0; i < 12; ++i)
                {
                    auto scenario = randomScenario (random);
                    addRandomClayFeatures (scenario.profile, random);
                    scenarios.push_back (scenario);
                }

                const StyleWeights locked { 0.3f, 0.3f, 0.4f };
                float sink = 0.0f;
                const auto before = allocationCountOnThisThread();

                for (const auto& s : scenarios)
                    for (auto genre : { Genre::general, Genre::deepHouse, Genre::techHouse, Genre::melodicProgressive })
                        for (auto delivery : { Delivery::standard, Delivery::streaming, Delivery::club })
                            for (auto glaze : { 0.0f, 33.0f, 100.0f })
                            {
                                PlanInputs in;
                                in.profile = s.hasProfile ? &s.profile : nullptr;
                                in.style = s.style;
                                in.genre = genre;
                                in.delivery = delivery;
                                in.glazePercent = glaze;
                                in.manual = s.manual;
                                in.maxGainDb = s.maxGainDb;
                                in.lockedWeights = glaze > 50.0f ? &locked : nullptr;
                                const auto plan = makePlan (in);
                                const auto actual = applyIncrements (s.manual, plan.increments);
                                const auto sc = blendConstants (c, plan.weights, genre);
                                const auto usage = estimateManualChainUsage (in);
                                sink += actual.limit.gainDb + actual.color.toneDb + plan.clay.lowCutHz + sc.clayLowCutTargetHz
                                      + toneBrightCapDb (sc, c, genre) + usage.limitGrPeakDb;
                            }

                const auto allocationsInPath = allocationCountOnThisThread() - before;

                expectEquals ((int) (allocationsInPath), 0, utf8 ("曲风与交付的决策路径不应分配内存"));
                expect (std::isfinite (sink));
            }
        }

        //==============================================================================
        // 对一份正确的 XML 做一处破坏，读取必须失败且 out 逐位不变。
        template <typename T>
        void expectRejected (const juce::XmlElement& good, const std::function<void (juce::XmlElement&)>& damage, const T& sentinel,
                             const juce::String& what)
        {
            auto broken = std::make_unique<juce::XmlElement> (good);
            damage (*broken);
            T out = sentinel;
            expect (! fromXml (*broken, out), what + utf8 ("：坏数据应读取失败"));
            expect (bitwiseEqual (out, sentinel), what + utf8 ("：读取失败时 out 应不变"));
        }

        void testBrokenDataRejected()
        {
            beginTest ("Broken or newer saved data is rejected without touching the output");

            // 依据：GlazeProfile.h / GlazeConstants.h「读取失败返回 false，out 不变」「读到未知的更高 formatVersion 按读取失败处理」；
            //       时间线与保存第 5 节（读失败 = 未学习，编排器据此处理）。
            const auto profileXml = toXml (oddProfile());
            const auto sentinel = makeProfile (-11.0f, -2.0f);

            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setTagName ("NOT_A_PROFILE"); }, sentinel, utf8 ("节点名不对"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", profileFormatVersion + 1); }, sentinel, utf8 ("更高的格式版本"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", 0); }, sentinel, utf8 ("格式版本 0"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", "1x"); }, sentinel, utf8 ("格式版本不是整数"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.removeAttribute ("formatVersion"); }, sentinel, utf8 ("缺格式版本"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.removeAttribute ("truePeakDbtp_value"); }, sentinel, utf8 ("缺字段"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setAttribute ("integratedLufs_value", "c1bb"); }, sentinel, utf8 ("位数不对"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setAttribute ("integratedLufs_value", "c1bb0g00"); }, sentinel, utf8 ("非十六进制字符"));
            expectRejected<MusicProfile> (*profileXml, [] (juce::XmlElement& x) { x.setAttribute ("lowShare_valid", "2"); }, sentinel, utf8 ("布尔值不是 0 / 1"));
            expectRejected<MusicProfile> (*profileXml,
                                          [] (juce::XmlElement& x) { x.setAttribute ("bandConfidence", x.getStringAttribute ("bandConfidence").dropLastCharacters (8)); },
                                          sentinel, utf8 ("27 段数组少一段"));

            CommittedState state;
            state.hasProfile = true;
            state.profile = oddProfile();
            state.plan = planFor (&sentinel, Style::warm, 50.0f);
            const auto stateXml = toXml (state);
            CommittedState stateSentinel;
            stateSentinel.hasProfile = true;
            stateSentinel.profile = sentinel;
            stateSentinel.constants.version = 42;

            expectRejected<CommittedState> (*stateXml, [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", stateFormatVersion + 1); }, stateSentinel, utf8 ("状态格式版本更高"));
            expectRejected<CommittedState> (*stateXml, [] (juce::XmlElement& x) { x.removeChildElement (x.getChildByName ("PLAN"), true); }, stateSentinel, utf8 ("缺方案"));
            expectRejected<CommittedState> (*stateXml, [] (juce::XmlElement& x) { x.getChildByName ("CALIBRATION")->setAttribute ("style", 7); }, stateSentinel, utf8 ("风格越界"));
            expectRejected<CommittedState> (*stateXml,
                                            [] (juce::XmlElement& x) { x.getChildByName (profileTag)->setAttribute ("formatVersion", profileFormatVersion + 1); },
                                            stateSentinel, utf8 ("画像子节点版本更高"));
            expectRejected<CommittedState> (*stateXml,
                                            [] (juce::XmlElement& x) { x.getChildByName (constantsTag)->setAttribute ("formatVersion", constantsFormatVersion + 1); },
                                            stateSentinel, utf8 ("标定快照版本更高"));
            expectRejected<CommittedState> (*stateXml, [] (juce::XmlElement& x) { x.getChildByName ("PLAN")->removeAttribute ("limitGainDb"); }, stateSentinel, utf8 ("方案缺字段"));
            expectRejected<CommittedState> (*stateXml, [] (juce::XmlElement& x) { x.setAttribute ("profileId", "xyz"); }, stateSentinel, utf8 ("画像号非法"));

            // 方案算法版本比本版本新也照样读进来（照用保存的增量，时间线与保存第 5 节）。
            auto newerAlgorithm = std::make_unique<juce::XmlElement> (*stateXml);
            newerAlgorithm->getChildByName ("PLAN")->setAttribute ("algorithmVersion", currentAlgorithmVersion + 5);
            CommittedState newer;
            expect (fromXml (*newerAlgorithm, newer), utf8 ("方案算法版本不同不应拒收"));
            expectEquals (newer.plan.algorithmVersion, currentAlgorithmVersion + 5);

            // T-032 R6-02：PLAN 的 automaticLevelWithdrawn 可缺省——v0.1.8.5 及以前的存档没有它，缺省取同一方案的 limitedByManual
            //（旧语义 = 保守）；属性在但不是 0 / 1 照样拒收。
            expectRejected<CommittedState> (*stateXml, [] (juce::XmlElement& x) { x.getChildByName ("PLAN")->setAttribute ("automaticLevelWithdrawn", "2"); },
                                            stateSentinel, utf8 ("automaticLevelWithdrawn 不是 0 / 1"));

            for (auto limited : { false, true })
            {
                auto legacyPlan = std::make_unique<juce::XmlElement> (*stateXml);
                auto* planNode = legacyPlan->getChildByName ("PLAN");
                planNode->setAttribute ("limitedByManual", limited ? "1" : "0");
                planNode->setAttribute ("automaticLevelWithdrawn", "0");   // 先证明属性在时按属性读……
                CommittedState explicitState;
                expect (fromXml (*legacyPlan, explicitState) && explicitState.plan.limitedByManual == limited && ! explicitState.plan.automaticLevelWithdrawn);
                planNode->removeAttribute ("automaticLevelWithdrawn");     // ……缺属性才取 limitedByManual
                CommittedState legacyState;
                expect (fromXml (*legacyPlan, legacyState), utf8 ("旧存档缺 automaticLevelWithdrawn 不应拒收"));
                expect (legacyState.plan.limitedByManual == limited && legacyState.plan.automaticLevelWithdrawn == limited,
                        utf8 ("缺属性时 automaticLevelWithdrawn 取 limitedByManual"));
            }

            // 执行快照的 PLAN 与已提交版本同一字段表（GlazeProfile.cpp visitPlan）：缺属性同样取 limitedByManual，属性在就按属性读。
            {
                ExecutionState execution;
                execution.valid = true;
                execution.hasProfile = true;
                execution.profile = sentinel;
                execution.constants = factoryConstants();
                execution.plan = state.plan;
                execution.plan.limitedByManual = true;
                execution.plan.automaticLevelWithdrawn = false;
                execution.reference.glazePercent = 50.0f;
                execution.reference.sampleRate = 48000.0;
                const auto executionXml = toXml (execution);

                ExecutionState explicitBack;
                expect (fromXml (*executionXml, explicitBack) && explicitBack.plan.limitedByManual && ! explicitBack.plan.automaticLevelWithdrawn,
                        utf8 ("执行快照：属性在时按属性读"));

                auto legacyExecution = std::make_unique<juce::XmlElement> (*executionXml);
                legacyExecution->getChildByName ("PLAN")->removeAttribute ("automaticLevelWithdrawn");
                ExecutionState legacyBack;
                expect (fromXml (*legacyExecution, legacyBack) && legacyBack.plan.automaticLevelWithdrawn,
                        utf8 ("执行快照缺属性：取 limitedByManual（保守）"));
            }

            // T-032 R6-02 收尾（A11）：PLAN 的 manualBudgetMask（十进制位掩码）。属性在：按属性读、往返逐位；缺属性（A11 之前的存档）：
            // 受手动设置限制的方案三位全置（分不出是哪一项超了，不把可能是手动造成的超出当成自动的），否则 0；不是 0 ～ 255 的整数：
            // 整份拒收。未定义的位：已提交版本照读（编排器只认三个已定义的位，与 clayCostLimitedMask 同一做法），执行快照整份拒收。
            {
                for (auto mask : { 0, 1, 2, 4, 5, 7 })
                {
                    auto masked = state;
                    masked.plan.manualBudgetMask = (std::uint8_t) mask;
                    CommittedState back;
                    expect (fromXml (*toXml (masked), back) && back.plan.manualBudgetMask == mask && bitwiseEqual (back, masked),
                            utf8 ("manualBudgetMask 往返逐位：") + juce::String (mask));
                }

                for (auto limited : { false, true })
                {
                    auto legacyPlan = std::make_unique<juce::XmlElement> (*stateXml);
                    auto* planNode = legacyPlan->getChildByName ("PLAN");
                    planNode->setAttribute ("limitedByManual", limited ? "1" : "0");
                    planNode->setAttribute ("manualBudgetMask", 2);
                    CommittedState explicitState;
                    expect (fromXml (*legacyPlan, explicitState) && explicitState.plan.manualBudgetMask == 2, utf8 ("属性在时按属性读"));
                    planNode->removeAttribute ("manualBudgetMask");
                    CommittedState legacyState;
                    expect (fromXml (*legacyPlan, legacyState), utf8 ("旧存档缺 manualBudgetMask 不应拒收"));
                    expectEquals ((int) legacyState.plan.manualBudgetMask, limited ? (int) manualBudgetAll : 0,
                                  utf8 ("缺属性：受手动限制的方案三位全置，否则 0"));
                }

                for (const auto* bad : { "256", "-1", "x", "2.5" })
                    expectRejected<CommittedState> (*stateXml, [bad] (juce::XmlElement& x) { x.getChildByName ("PLAN")->setAttribute ("manualBudgetMask", bad); },
                                                    stateSentinel, utf8 ("manualBudgetMask 非法：") + bad);

                for (auto undefined : { 8, 15 })
                {
                    auto future = std::make_unique<juce::XmlElement> (*stateXml);
                    future->getChildByName ("PLAN")->setAttribute ("manualBudgetMask", undefined);
                    CommittedState read;
                    expect (fromXml (*future, read) && read.plan.manualBudgetMask == undefined,
                            utf8 ("已提交版本里未定义的位照读：") + juce::String (undefined));
                }

                ExecutionState execution;
                execution.valid = true;
                execution.hasProfile = true;
                execution.profile = sentinel;
                execution.constants = factoryConstants();
                execution.plan = state.plan;
                execution.plan.limitedByManual = true;
                execution.plan.manualBudgetMask = manualBudgetGlue;
                execution.reference.glazePercent = 50.0f;
                execution.reference.sampleRate = 48000.0;
                const auto executionXml = toXml (execution);

                ExecutionState back;
                expect (fromXml (*executionXml, back) && back.plan.manualBudgetMask == manualBudgetGlue, utf8 ("执行快照：属性在时按属性读"));

                auto legacyExecution = std::make_unique<juce::XmlElement> (*executionXml);
                legacyExecution->getChildByName ("PLAN")->removeAttribute ("manualBudgetMask");
                ExecutionState legacyBack;
                expect (fromXml (*legacyExecution, legacyBack) && legacyBack.plan.manualBudgetMask == manualBudgetAll,
                        utf8 ("执行快照缺属性：受手动限制 → 三位全置"));

                for (const auto* bad : { "8", "256" })
                {
                    auto broken = std::make_unique<juce::XmlElement> (*executionXml);
                    broken->getChildByName ("PLAN")->setAttribute ("manualBudgetMask", bad);
                    ExecutionState rejected;
                    expect (! fromXml (*broken, rejected), utf8 ("执行快照 manualBudgetMask 非法应拒收：") + bad);
                }
            }
        }

        //==============================================================================
        void testConstantsRoundTrip()
        {
            beginTest ("Calibration constants round-trip and reject unknown versions");

            // 依据：时间线与保存第 5 节「标定快照：生成方案用到的全部常数与换算表，连同版本号」「常数调整不改变旧工程」。
            const auto& factory = factoryConstants();
            expectEquals (factory.algorithmVersion, currentAlgorithmVersion);
            expectEquals (Constants {}.algorithmVersion, currentAlgorithmVersion);
            expectEquals (Plan {}.algorithmVersion, currentAlgorithmVersion, "a neutral, not-yet-evaluated plan is current, not legacy");
            expect (factory.version >= 1);
            expectEquals (factory.warm.preferredLufs, -8.5f);
            expectEquals (factory.punchy.preferredLufs, -9.0f);
            expectEquals (factory.clean.preferredLufs, -10.0f);
            expect (factory.clean.recoverCap < factory.warm.recoverCap && factory.clean.tameCap < factory.warm.tameCap, utf8 ("通透的 Recover / Tame 上限应更低"));

            for (auto* source : { &factory, (const Constants*) nullptr })
            {
                Constants original = source != nullptr ? *source : Constants {};

                if (source == nullptr)
                {
                    original.version = 3;
                    original.clean.limitGrPeakCapDb = 5.5f;
                    original.heatLoudnessDb[44] = -10.25f;
                    original.minListenSeconds = 14.999999999;
                }

                const auto text = toXml (original)->toString();
                const auto xml = juce::parseXML (text);
                Constants back;
                back.version = 99;
                expect (xml != nullptr && fromXml (*xml, back));
                expect (bitwiseEqual (back, original), utf8 ("标定快照往返不逐位相同"));
            }

            auto oneUlp = factory;
            oneUlp.heatPeakDb[7] = std::nextafter (oneUlp.heatPeakDb[7], 0.0f);
            expect (! bitwiseEqual (oneUlp, factory), utf8 ("差 1 ulp 也应判为不同"));

            const auto good = toXml (factory);
            const auto sentinel = Constants {};
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("formatVersion", constantsFormatVersion + 1); }, sentinel, utf8 ("快照格式版本更高"));
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.removeAttribute ("clean_limitGrPeakCapDb"); }, sentinel, utf8 ("快照缺字段"));
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("calibrationRounds", "2.5"); }, sentinel, utf8 ("整数字段非法"));
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setTagName ("GLAZE_OTHER"); }, sentinel, utf8 ("节点名不对"));
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("automaticHeatFloor", "oops"); }, sentinel,
                                       "a present but malformed optional field is not a legacy snapshot");
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("warm_automaticHeatFloor", "oops"); }, sentinel,
                                       "a present but malformed per-style heat floor is rejected");
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("heatUsefulCrestDb", "oops"); }, sentinel,
                                       "a present but malformed heatUsefulCrestDb is rejected");

            // T-032 R6-01：出厂 v8 快照写三个风格底量，旧的全局 automaticHeatFloor 照写（= 三者中最小的，旧版本插件只认它）。
            expect (factory.version >= 8 && currentAlgorithmVersion >= 8, utf8 ("T-032：标定 / 算法版本升到 8"));
            expectEquals (factory.heatUsefulCrestDb, 0.1f);
            expect (good->hasAttribute ("warm_automaticHeatFloor") && good->hasAttribute ("punchy_automaticHeatFloor")
                        && good->hasAttribute ("clean_automaticHeatFloor") && good->hasAttribute ("automaticHeatFloor") && good->hasAttribute ("heatUsefulCrestDb"));
            expect (good->getStringAttribute ("automaticHeatFloor") == good->getStringAttribute ("clean_automaticHeatFloor"),
                    utf8 ("出厂三种底量相同：全局字段就是它"));

            // v7 及以前的快照只有全局 automaticHeatFloor（例如 9）：三种风格都按它读，混合权重下逐位等于原值。
            {
                auto v7 = toXml (factory);
                v7->setAttribute ("version", 7);
                v7->setAttribute ("algorithmVersion", 7);
                for (const auto* field : { "warm_automaticHeatFloor", "punchy_automaticHeatFloor", "clean_automaticHeatFloor", "heatUsefulCrestDb" })
                    v7->removeAttribute (field);
                const float nine = 9.0f;
                std::uint32_t nineBits = 0;
                std::memcpy (&nineBits, &nine, sizeof (nineBits));
                v7->setAttribute ("automaticHeatFloor", juce::String::toHexString ((int) nineBits).paddedLeft ('0', 8));
                Constants fromV7 = factory;
                expect (fromXml (*v7, fromV7), utf8 ("v7 快照应读得回"));
                expect (sameBits (fromV7.warm.automaticHeatFloor, 9.0f) && sameBits (fromV7.punchy.automaticHeatFloor, 9.0f)
                            && sameBits (fromV7.clean.automaticHeatFloor, 9.0f), utf8 ("三种风格都取旧的全局底量"));
                expectEquals (fromV7.heatUsefulCrestDb, 0.0f, utf8 ("旧快照缺 heatUsefulCrestDb 按 0 读（算法 7 不进新分支，缺省不是兼容手段）"));
                expect (sameBits (blendConstants (fromV7, { 0.12f, 0.88f, 0.0f }).automaticHeatFloor, 9.0f)
                            && sameBits (blendConstants (fromV7, { 0.3f, 0.3f, 0.4f }).automaticHeatFloor, 9.0f),
                        utf8 ("三者逐位相同时混合直接取原值，不经加权求和"));
                Constants v7RoundTrip;
                expect (fromXml (*toXml (fromV7), v7RoundTrip) && bitwiseEqual (v7RoundTrip, fromV7), utf8 ("v7 读回再存往返逐位相同"));

                auto tuned = factory;   // Max 标定后三者不同：按权重线性混合，全局字段取最小
                tuned.warm.automaticHeatFloor = 12.0f;
                tuned.punchy.automaticHeatFloor = 8.0f;
                tuned.clean.automaticHeatFloor = 4.0f;
                expectWithinAbsoluteError (blendConstants (tuned, { 0.5f, 0.0f, 0.5f }).automaticHeatFloor, 8.0f, 1.0e-5f);
                const auto tunedXml = toXml (tuned);
                expect (tunedXml->getStringAttribute ("automaticHeatFloor") == tunedXml->getStringAttribute ("clean_automaticHeatFloor"),
                        utf8 ("全局字段 = 三者中最小的（旧版本读到偏保守的那份）"));
                Constants tunedBack;
                expect (fromXml (*tunedXml, tunedBack) && bitwiseEqual (tunedBack, tuned));
            }

            // 旧版没有这些字段：从任意调用方初值读回都必须关掉新底量，不能继承当前出厂值。
            auto legacyXml = toXml (factory);
            legacyXml->setAttribute ("algorithmVersion", 1);
            legacyXml->setAttribute ("version", 1);
            for (const auto* field : { "minimumFullDriveDb", "automaticRecoverFloor", "automaticTameFloor", "automaticHeatFloor",
                                       "warm_automaticHeatFloor", "punchy_automaticHeatFloor", "clean_automaticHeatFloor", "heatUsefulCrestDb" })
                legacyXml->removeAttribute (field);
            auto legacy = factory;
            expect (fromXml (*legacyXml, legacy));
            expectEquals (legacy.minimumFullDriveDb, 0.0f);
            expectEquals (legacy.automaticRecoverFloor, 0.0f);
            expectEquals (legacy.automaticTameFloor, 0.0f);
            expectEquals (legacy.warm.automaticHeatFloor, 0.0f);
            expectEquals (legacy.punchy.automaticHeatFloor, 0.0f);
            expectEquals (legacy.clean.automaticHeatFloor, 0.0f);
            expectEquals (legacy.heatUsefulCrestDb, 0.0f);
            expectEquals (legacy.algorithmVersion, 1);
            expect (legacy.algorithmVersion != currentAlgorithmVersion, "the controller can recognise an old saved plan");
            Constants legacyRoundTrip;
            expect (fromXml (*toXml (legacy), legacyRoundTrip));
            expect (bitwiseEqual (legacyRoundTrip, legacy));

            const auto hot = makeProfile (-6.0f, -1.5f, 3.0f);
            PlanInputs legacyInput;
            legacyInput.profile = &hot;
            legacyInput.style = Style::clean;
            legacyInput.glazePercent = 100.0f;
            legacyInput.manual = defaultManual();
            legacyInput.constants = &legacy;
            const auto legacyPlan = makePlan (legacyInput);
            expectEquals (legacyPlan.requestedGainDb, 0.0f);
            expectEquals (legacyPlan.increments.colorHeat, 0.0f);
            expectEquals (legacyPlan.increments.balanceRecover, 0.0f);
            expectEquals (legacyPlan.increments.balanceTame, 0.0f);

            // 方案带上生成它的常数版本与算法版本。
            auto tuned = factory;
            tuned.version = 12;
            PlanInputs in;
            const auto profile = makeProfile (-18.0f, -5.0f);
            in.profile = &profile;
            in.glazePercent = 40.0f;
            in.constants = &tuned;
            const auto plan = makePlan (in);
            expectEquals (plan.constantsVersion, 12);
            expectEquals (plan.algorithmVersion, currentAlgorithmVersion);
        }

        //==============================================================================
        void testOddSnapshots()
        {
            beginTest ("Odd calibration snapshots still give finite plans");

            // 依据：时间线与保存第 5 节（标定快照随工程保存，读取只查格式）：取值坏掉的快照（NaN、全 0 的占位）不许让方案
            //       出非有限值、也不许把参数推出范围；结构体默认值 Constants{} 本身也是一份可用的快照。
            Constants placeholder;   // 三种风格都是阶段 0 的占位（只有 Limit、均值上限 3 dB）

            Constants broken = factoryConstants();
            broken.heatLoudnessDb[20] = std::numeric_limits<float>::quiet_NaN();
            broken.heatPeakDb[21] = std::numeric_limits<float>::infinity();
            broken.warm.preferredLufs = std::numeric_limits<float>::quiet_NaN();
            broken.warm.shareClip = std::numeric_limits<float>::quiet_NaN();
            broken.punchy.limitGrPeakCapDb = -std::numeric_limits<float>::infinity();
            broken.limitLossCoeff = std::numeric_limits<float>::quiet_NaN();
            broken.unlearnedInputLufs = std::numeric_limits<float>::quiet_NaN();

            Constants zeros = factoryConstants();
            zeros.glueRatio = 1.0f;
            zeros.glueKneeDb = 0.0f;
            zeros.clipKneeRatio = 0.0f;
            zeros.fullConfidence = 0.0f;
            zeros.limitMeanCoeff = 0.0f;
            zeros.clipActivityCoeff = 0.0f;
            zeros.heatTableTruePeakDbtp.fill (0.0f);
            zeros.heatTableAmount.fill (0.0f);

            juce::Random random (0x5eed06);
            int failures = 0;

            for (const auto* snapshot : { &placeholder, &broken, &zeros })
                for (int trial = 0; trial < 40; ++trial)
                {
                    const auto scenario = randomScenario (random);

                    for (auto glaze : { 0.0f, 30.0f, 100.0f })
                    {
                        PlanInputs in;
                        in.profile = scenario.hasProfile ? &scenario.profile : nullptr;
                        in.style = scenario.style;
                        in.glazePercent = glaze;
                        in.manual = scenario.manual;
                        in.constants = snapshot;
                        const auto plan = makePlan (in);
                        const auto& inc = plan.increments;
                        const auto p = applyIncrements (scenario.manual, inc);

                        const bool ok = std::isfinite (plan.requestedGainDb) && std::isfinite (plan.effectiveGainDb)
                                     && std::isfinite (plan.requestedTargetLufs) && std::isfinite (plan.effectiveTargetLufs)
                                     && std::isfinite (plan.estimatedExcessDb)
                                     && std::isfinite (inc.balanceRecover) && std::isfinite (inc.balanceTame) && std::isfinite (inc.colorToneDb)
                                     && std::isfinite (inc.colorGlue) && std::isfinite (inc.colorHeat) && std::isfinite (inc.limitGainDb)
                                     && std::isfinite (inc.limitClip)
                                     && p.limit.gainDb >= 0.0f && p.limit.gainDb <= 24.0f && p.color.heat >= 0.0f && p.color.heat <= 100.0f
                                     && p.color.glue >= 0.0f && p.color.glue <= 100.0f && p.limit.clip >= 0.0f && p.limit.clip <= 100.0f;
                        failures += ok ? 0 : 1;
                    }
                }

            expectEquals (failures, 0, utf8 ("坏快照让方案出了非有限值或出了范围"));
        }

        //==============================================================================
        void testNoAllocation()
        {
            beginTest ("makePlan and the helpers do not allocate");

            // 依据：GlazePlanner.h「无状态、不分配」；界面与实现第 4 节「音频线程 0 分配」（编排器可能每块调 makePlan）。
            juce::Random random (0x5eed05);
            std::vector<Scenario> scenarios;

            for (int i = 0; i < 20; ++i)
                scenarios.push_back (randomScenario (random));

            const auto& c = factoryConstants();   // 函数内静态量先初始化（它本身也不分配）
            const StyleWeights locked { 0.3f, 0.3f, 0.4f };
            float sink = 0.0f;

            const auto before = allocationCountOnThisThread();

            for (const auto& s : scenarios)
                for (auto glaze : { 0.0f, 33.0f, 100.0f })
                {
                    PlanInputs in;
                    in.profile = s.hasProfile ? &s.profile : nullptr;
                    in.style = s.style;
                    in.glazePercent = glaze;
                    in.manual = s.manual;
                    in.maxGainDb = s.maxGainDb;
                    in.lockedWeights = glaze > 50.0f ? &locked : nullptr;
                    const auto plan = makePlan (in);
                    const auto actual = applyIncrements (s.manual, plan.increments);
                    const auto weights = resolveAutomaticWeights (s.profile, c);
                    const auto blended = blendConstants (c, weightsFor (s.style, weights));
                    sink += actual.limit.gainDb + blended.preferredLufs + (float) (computeProfileId (s.profile) & 1u);
                }

            // 先把计数读进局部量再断言：函数实参的求值顺序不确定（GCC 从右往左），提示文字的 juce::String 会先构造、被算进计数。

            const auto allocationsInPath = allocationCountOnThisThread() - before;

            expectEquals ((int) (allocationsInPath), 0, utf8 ("决策器路径不应分配内存"));
            expect (std::isfinite (sink));
        }

        //==============================================================================
        // ---- v0.1.7.0 修坯 Clay（T-012 A5；ADR-012 根治修订 T-012 阶段 4 P3）：决策器 Plan.clay 与标定快照的 clay* 常数 ----
        // 依据：设计规格-Clay-分析决策与验收第 2 节（公式）、第 6 节第 4 条（决策的验收）；ADR-009；ADR-012 / 设计规格-Clay-根治修订
        // 3.1 / 3.3（真次声阈值、收单按单声道回放损失、代价系数、costLimitedMask、风格低切目标）。画像的立体声 / 次声特征与峰值代价
        // 由测试直接给值（分析器的测量归 MusicAnalyzerTests）；期望值按规格公式手算，注释写出算式。出厂常数（GlazeConstants.h / .cpp）：
        //   清理强度 min(1, g / 0.5)；真次声占比 −40 → −28 dB；地板 10 Hz、门槛 k 0.001；收单需要度 ramp(10·log10((1 + ρ) / 2), −0.5, −3)；
        //   逐段「S/M > −15 dB 或 ρ < 0.3」；分频点 60 ～ 160 Hz、默认 80 Hz；高频 S/M −10 → −18 dB；加宽上限 1.5 dB；
        //   加宽后相关度下限 0.25；代价系数 ramp(代价, 0.6, 0.2)；风格（温润 / 冲击 / 通透）低切目标 16 / 18 / 20 Hz、加宽系数 0.4 / 0.6 / 1.0。
        //   特征置信度按 trustOf 口径（÷ 满置信 0.8，钳到 1）：置信度 1 → c = 1。

        static bool sameClay (const ClayPlan& a, const ClayPlan& b) noexcept
        {
            return sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz)
                && sameBits (a.monoAmount, b.monoAmount) && sameBits (a.widthDb, b.widthDb)
                && a.costLimitedMask == b.costLimitedMask;
        }

        // 中性 = 与 ClayPlan{} 逐位相同（低切 0、收单 0 且分频点保持默认 80 Hz、加宽 0、没有代价位）。
        static bool isNeutral (const ClayPlan& clay) noexcept
        {
            return sameClay (clay, ClayPlan {});
        }

        // 三项的值都是中性（不看 costLimitedMask：代价把三项都否决时值是中性、位却全置）。
        static bool hasNeutralValues (const ClayPlan& clay) noexcept
        {
            auto values = clay;
            values.costLimitedMask = 0;
            return isNeutral (values);
        }

        static int maskOf (const ClayPlan& clay) noexcept
        {
            return static_cast<int> (clay.costLimitedMask);
        }

        static constexpr int allCostBits = clayCostLimitedLowCut | clayCostLimitedMono | clayCostLimitedWidth;
        static constexpr int autoCostBits = clayCostLimitedLowCut | clayCostLimitedWidth;   // ADR-015：决策器只可能置这两位

        static bool sameBitsDouble (double a, double b) noexcept
        {
            std::uint64_t x, y;
            std::memcpy (&x, &a, sizeof (x));
            std::memcpy (&y, &b, sizeof (y));
            return x == y;
        }

        // 给画像填先试后用的峰值代价（ADR-012）：低切、四个收单候选（同一个值）、加宽；有效、有效时长 20 s、同一置信度。
        static void setClayCosts (MusicProfile& profile, float lowCutCostDb, float monoCostDb, float widthCostDb, float confidence = 1.0f)
        {
            setFeature (profile.lowCutPeakCostDb, lowCutCostDb, confidence);

            for (auto& cost : profile.monoPeakCostDb)
                setFeature (cost, monoCostDb, confidence);

            setFeature (profile.widthPeakCostDb, widthCostDb, confidence);
        }

        // 给画像填修坯用的五个特征（同一置信度）与全 0 dB 的峰值代价（代价系数 1：公式测试不受代价影响，代价另测）；逐段立体声
        // 统计全「窄」且有统计（S/M −30 dB、ρ 0.95、可信度 1）：没有段满足「S/M > −15 dB 或 ρ < 0.3」，收单分频点落在默认 80 Hz。
        // 低频 S/M 与高频 ρ 现在决策器都不用（ADR-012 后收单只看低频 ρ），照样给值，测「给了也不影响」。
        static void setClayFeatures (MusicProfile& profile, float subShareDb, float lowSideToMidDb, float lowCorrelation,
                                     float highSideToMidDb, float highCorrelation, float confidence = 1.0f)
        {
            setFeature (profile.subShareDb, subShareDb, confidence);
            setFeature (profile.lowSideToMidDb, lowSideToMidDb, confidence);
            setFeature (profile.lowCorrelation, lowCorrelation, confidence);
            setFeature (profile.highSideToMidDb, highSideToMidDb, confidence);
            setFeature (profile.highCorrelation, highCorrelation, confidence);
            setClayCosts (profile, 0.0f, 0.0f, 0.0f, confidence);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                profile.bandSideToMidDb[b] = -30.0f;
                profile.bandCorrelation[b] = 0.95f;
                profile.bandStereoConfidence[b] = 1.0f;
            }
        }

        // 收单需要度正好 0.5 的低频相关度：单声道回放损失 10·log10((1 + ρ) / 2) = −0.5 − 2.5 · 0.5 = −1.75 dB
        // → ρ = 2 · 10^(−0.175) − 1 = 0.33668783（float；double 里算出的损失回到 float 正好是 −1.75，需要度 (−1.75 + 0.5) / −2.5 = 0.5）。
        static constexpr float monoNeedHalfCorrelation = 0.33668783f;

        // 三项都要动一半的画像：真次声占比 −34 dB（need (−34 + 40) / (−28 + 40) = 0.5）；低频 ρ 0.33668783（need 0.5，见上）；
        // 高频 S/M −14 dB（need (−14 + 10) / (−18 + 10) = 0.5）；代价全 0 dB（系数 1）。其余特征同 makeProfile（全可信）。
        static MusicProfile clayProfile()
        {
            auto profile = makeProfile (-18.0f, -5.0f);
            setClayFeatures (profile, -34.0f, -14.0f, monoNeedHalfCorrelation, -14.0f, 0.9234f);
            return profile;
        }

        // 用指定的标定快照出方案（手动值中性、不设约束反馈）。
        static Plan clayPlanWith (const MusicProfile* profile, Style style, float glaze, const Constants& constants,
                                  const StyleWeights* locked = nullptr)
        {
            PlanInputs in;
            in.profile = profile;
            in.style = style;
            in.glazePercent = glaze;
            in.manual = defaultManual();
            in.constants = &constants;
            in.lockedWeights = locked;
            return makePlan (in);
        }

        // 按画像的高频 S/M 预测加宽 widthDb 之后的高频相关度（第 2 节③）：ρ' = (1 − γ²r) / (1 + γ²r)，γ² = 10^(w / 10)、
        // r = 10^(S/M / 10)。double 精度、直接按定义算（与决策器在 dB 域反解上限的算法无关）。
        static double predictedCorrelation (float widthDb, float sideToMidDb)
        {
            const auto x = std::pow (10.0, ((double) widthDb + (double) sideToMidDb) / 10.0);
            return (1.0 - x) / (1.0 + x);
        }

        // 随机补上 Clay 特征：值跨过各斜坡两侧，置信度与有效标志随机；逐段统计随机，约一半的段没有统计。
        // 每条语句只从 r 取一次数（实参求值顺序不定，不把两次取数放进同一个实参列表，换编译器结果也一样）。
        static void addRandomClayFeatures (MusicProfile& profile, juce::Random& r)
        {
            const auto range = [&r] (float lo, float hi) { return lo + (hi - lo) * r.nextFloat(); };
            const auto randomFeature = [&r, &range] (Feature& f, float lo, float hi)
            {
                const auto value = range (lo, hi);
                const auto trust = r.nextFloat() < 0.3f ? range (0.0f, 1.0f) : 1.0f;
                setFeature (f, value, trust);
                f.valid = r.nextFloat() > 0.1f;
            };

            randomFeature (profile.subShareDb, -46.0f, -22.0f);    // 真次声斜坡 −40 → −28 dB 两侧
            randomFeature (profile.lowSideToMidDb, -30.0f, 0.0f);
            randomFeature (profile.lowCorrelation, -1.05f, 1.0f);  // 收单需要度 ρ 0.78 → 0 两侧，含 ρ ≤ −1（−∞ 一支）
            randomFeature (profile.highSideToMidDb, -25.0f, 0.0f);
            randomFeature (profile.highCorrelation, -0.2f, 1.0f);

            // 峰值代价跨过代价斜坡 0.2 → 0.6 dB 两侧（照做 / 减量 / 否决都有），约一成无效（没试过）。
            randomFeature (profile.lowCutPeakCostDb, -0.5f, 1.0f);

            for (auto& cost : profile.monoPeakCostDb)
                randomFeature (cost, -0.5f, 1.0f);

            randomFeature (profile.widthPeakCostDb, -0.5f, 1.0f);

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                profile.bandSideToMidDb[b] = range (-35.0f, 0.0f);
                profile.bandCorrelation[b] = range (-0.5f, 1.0f);
                profile.bandStereoConfidence[b] = r.nextBool() ? 0.0f : range (0.0f, 1.0f);
            }
        }

        //==============================================================================
        void testClayFormulas()
        {
            beginTest ("Clay auto values follow the spec formulas");

            // 画像见 clayProfile：三项的 need 都是 0.5，特征可信度 1（c = 1），代价全 0 dB（代价系数 1、不置代价位）。
            // ① k = 0.5 · 1 · min(1, g / 0.5) · 1：GLAZE 100 → k = 0.5 → lowCutHz = 10 · (f_t / 10)^0.5 = 12.6491 / 13.4164 / 14.1421 Hz
            //    （温润 16 / 冲击 18 / 通透 20 Hz，ADR-012）；GLAZE 25 → 强度 0.5、k = 0.25 → 10 · (f_t / 10)^0.25 = 11.2468 / 11.5829 / 11.8921 Hz。
            // ② ADR-015：自动态不收单——monoAmount 恒 0、分频点恒为默认 80 Hz（收单只改立体声，单声道回放不变）。
            // ③ widthDb = 1.5 · (−14 + 10) / (−18 + 10) · 1 · s_w · g · 1 = 0.75 · s_w · g：GLAZE 100 → 0.3 / 0.45 / 0.75 dB，GLAZE 25 → 四分之一；
            //    相关度上限 10·log10(0.6) + 14 = 11.78 dB 不起作用。
            // GLAZE 50 起清理项强度已满（min(1, 0.5 / 0.5) = 1）：低切与收单与 GLAZE 100 逐位相同，加宽只到一半（口味项随 g 线性）。
            const auto profile = clayProfile();

            const struct { Style style; float widthFactor, lowCutFull, lowCutQuarter; } expectations[] =
            {
                { Style::warm,   0.4f, 12.649111f, 11.246827f },
                { Style::punchy, 0.6f, 13.416408f, 11.582922f },
                { Style::clean,  1.0f, 14.142136f, 11.892071f }
            };

            for (const auto& e : expectations)
            {
                const auto label = utf8 ("风格 ") + juce::String ((int) e.style);
                const auto full = planFor (&profile, e.style, 100.0f).clay;
                expectWithinAbsoluteError (full.lowCutHz, e.lowCutFull, 1.0e-4f, label + utf8 ("：低切 GLAZE 100"));
                expectEquals (full.monoAmount, 0.0f, label + utf8 ("：ADR-015 自动不收单（GLAZE 100）"));
                expectEquals (full.monoHz, ClayPlan {}.monoHz, label + utf8 ("：分频点是中性默认值"));
                expectWithinAbsoluteError (full.widthDb, 0.75f * e.widthFactor, 1.0e-6f, label + utf8 ("：加宽 GLAZE 100"));
                expectEquals (maskOf (full), 0, label + utf8 ("：代价 0 dB → 不置代价位"));

                const auto quarter = planFor (&profile, e.style, 25.0f).clay;
                expectWithinAbsoluteError (quarter.lowCutHz, e.lowCutQuarter, 1.0e-4f, label + utf8 ("：低切 GLAZE 25"));
                expectEquals (quarter.monoAmount, 0.0f, label + utf8 ("：ADR-015 自动不收单（GLAZE 25）"));
                expectWithinAbsoluteError (quarter.widthDb, 0.25f * 0.75f * e.widthFactor, 1.0e-6f, label + utf8 ("：加宽 GLAZE 25"));

                const auto half = planFor (&profile, e.style, 50.0f).clay;
                expect (sameBits (half.lowCutHz, full.lowCutHz) && sameBits (half.monoAmount, full.monoAmount),
                        label + utf8 ("：GLAZE 50 起清理项应给满"));
                expectWithinAbsoluteError (half.widthDb, 0.5f * full.widthDb, 1.0e-6f, label + utf8 ("：加宽随 GLAZE 线性"));
            }

            // ① 真次声阈值（ADR-012：< 16 Hz 的能量占比，−40 → −28 dB）与门槛，通透 f_t 20 Hz：−28 dB（need 1）→ k = 1 → lowCutHz = f_t
            //    = 20 Hz；更高（−16 dB）仍是 need 1；−34 dB → need 0.5 → 10 · 2^0.5 = 14.1421 Hz；−40 dB 与更低（−45 dB）→ need 0 → 关；
            //    −39.99 dB → k = 0.01 / 12 = 0.00083 ≤ 0.001 → 仍关；−39.9 dB → k = 0.1 / 12 = 0.0083 → 10 · 2^0.0083 = 10.0579 Hz
            //    （k 刚过门槛时从 0 跳到略高于地板 10 Hz：10 Hz 的 4 阶高通在 20 Hz 只衰减 0.02 dB）。
            const auto lowCutAt = [] (float subShareDb)
            {
                auto p = clayProfile();
                p.subShareDb.value = subShareDb;
                return planFor (&p, Style::clean, 100.0f).clay.lowCutHz;
            };

            expectWithinAbsoluteError (lowCutAt (-28.0f), 20.0f, 1.0e-4f, utf8 ("need 1：低切 = 风格目标"));
            expectWithinAbsoluteError (lowCutAt (-16.0f), 20.0f, 1.0e-4f);
            expectWithinAbsoluteError (lowCutAt (-34.0f), 14.142136f, 1.0e-4f, utf8 ("need 0.5：k = 0.5"));
            expectEquals (lowCutAt (-40.0f), 0.0f, utf8 ("need 0：不低切"));
            expectEquals (lowCutAt (-45.0f), 0.0f, utf8 ("真次声低于 −40 dB：不低切"));
            expectEquals (lowCutAt (-39.99f), 0.0f, utf8 ("k ≤ 0.001：不低切"));
            expectWithinAbsoluteError (lowCutAt (-39.9f), 10.0579f, 1.0e-3f, utf8 ("k 刚过门槛：从地板 10 Hz 起步"));

            // ② ADR-015：自动收单恒为 0——不管低频多宽、多反相（ρ 0.99 ～ −1.2，含越界），收单都救不回单声道回放的低音，
            //    只会让立体声听众丢低音。
            const auto monoAt = [] (float correlation)
            {
                auto p = clayProfile();
                p.lowCorrelation.value = correlation;
                return planFor (&p, Style::clean, 100.0f).clay.monoAmount;
            };

            for (auto rho : { 0.99f, 0.8f, 0.78f, 0.5f, 0.0f, -0.5f, -1.0f, -1.2f })
                expectEquals (monoAt (rho), 0.0f, utf8 ("ADR-015：自动不收单，低频 ρ ") + juce::String (rho));

            // 低频 S/M 不影响方案（收单自动态不做）：从 −14 dB 改成 0 dB 或 −40 dB，方案逐位不变。
            for (auto sideToMid : { 0.0f, -40.0f })
            {
                auto sideOnly = clayProfile();
                sideOnly.lowSideToMidDb.value = sideToMid;
                expect (sameClay (planFor (&sideOnly, Style::clean, 100.0f).clay, planFor (&profile, Style::clean, 100.0f).clay),
                        utf8 ("低频 S/M 不影响收单：") + juce::String (sideToMid));
            }

            // ③ 两端：高频 S/M −18 dB（need 1）→ 通透 GLAZE 100 = 1.5 dB（自动加宽的上限）；−25 dB 仍是 1.5；
            //    −10 dB（need 0）与更宽的 −6 dB → 0（第 6 节第 4 条「高频侧中比 ≥ −10 dB 时不加宽」）。
            const auto widthAt = [] (float sideToMidDb)
            {
                auto p = clayProfile();
                p.highSideToMidDb.value = sideToMidDb;
                return planFor (&p, Style::clean, 100.0f).clay.widthDb;
            };

            expectEquals (widthAt (-18.0f), 1.5f);
            expectEquals (widthAt (-25.0f), 1.5f);
            expectEquals (widthAt (-10.0f), 0.0f);
            expectEquals (widthAt (-6.0f), 0.0f);

            // c 的口径（trustOf：置信度 ÷ 满置信 0.8，钳到 1）：特征置信度 0.4 → c = 0.5 →
            //    ① k = 0.5 · 0.5 · 1 = 0.25 → 通透 11.8921 Hz；② 收单 0（ADR-015）；③ 1.5 · 0.5 · 0.5 · 1.0 = 0.375 dB。
            //    置信度 0.8 已算满：与置信度 1 逐位相同。低频特征的置信度不影响（收单自动态不做）。代价的置信度不参与（只看有效）：
            //    代价置信度 0.4 与 1 逐位相同。
            auto unsure = clayProfile();
            setClayFeatures (unsure, -34.0f, -14.0f, monoNeedHalfCorrelation, -14.0f, 0.9234f, 0.4f);
            const auto unsurePlan = planFor (&unsure, Style::clean, 100.0f).clay;
            expectWithinAbsoluteError (unsurePlan.lowCutHz, 11.892071f, 1.0e-4f, utf8 ("置信度 0.4：低切"));
            expectEquals (unsurePlan.monoAmount, 0.0f, utf8 ("置信度 0.4：收单仍是 0（ADR-015）"));
            expectWithinAbsoluteError (unsurePlan.widthDb, 0.375f, 1.0e-6f, utf8 ("置信度 0.4：加宽"));

            auto atFull = clayProfile();
            setClayFeatures (atFull, -34.0f, -14.0f, monoNeedHalfCorrelation, -14.0f, 0.9234f, 0.8f);
            expect (sameClay (planFor (&atFull, Style::clean, 100.0f).clay, planFor (&profile, Style::clean, 100.0f).clay),
                    utf8 ("置信度到满置信 0.8 即算满"));

            auto mixedTrust = clayProfile();
            mixedTrust.lowSideToMidDb.confidence = 0.0f;
            mixedTrust.lowCorrelation.confidence = 0.2f;
            expect (sameClay (planFor (&mixedTrust, Style::clean, 100.0f).clay, planFor (&profile, Style::clean, 100.0f).clay),
                    utf8 ("低频特征的可信度不影响方案（收单自动态不做）"));

            auto costTrust = clayProfile();
            setClayCosts (costTrust, 0.0f, 0.0f, 0.0f, 0.4f);
            expect (sameClay (planFor (&costTrust, Style::clean, 100.0f).clay, planFor (&profile, Style::clean, 100.0f).clay),
                    utf8 ("代价的置信度不参与"));

            // 不需要修坯的混音（真次声 −40 dB、低频 ρ 0.99、高频 S/M −6 dB）：GLAZE 100 各风格都全中性——代价再高也不置位
            // （「按需要度本应动作」才谈得上被代价减量）。
            auto tidy = makeProfile (-18.0f, -5.0f);
            setClayFeatures (tidy, -40.0f, -30.0f, 0.99f, -6.0f, 0.6f);

            for (auto costDb : { 0.0f, 0.8f })
            {
                setClayCosts (tidy, costDb, costDb, costDb);

                for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                    expect (isNeutral (planFor (&tidy, style, 100.0f).clay), utf8 ("不需要修坯的混音应全中性、不置位，代价 ") + juce::String (costDb));
            }
        }

        //==============================================================================
        void testClayCostFactor()
        {
            beginTest ("Clay try-before-use: the peak cost scales or vetoes each item and marks costLimitedMask");

            // 依据：ADR-012、设计规格-Clay-根治修订 3.3：代价系数 f(代价) = ramp(代价, 0.6, 0.2)——≤ 0.2 dB 照做（1）、0.2 ～ 0.6 dB
            //       线性减量、≥ 0.6 dB 不做（0）；低切 k、收单比例、加宽的原公式各乘自己的 f；某项「按需要度本应 > 0、但 f < 1 使它减小
            //       或为 0」→ 置位（bit0 低切、bit1 收单、bit2 加宽）；代价无效 → 该项中性（不置位）。
            // 画像：三项需要度都给满（真次声 −28 dB、ρ 0、高频 S/M −18 dB），通透、GLAZE 100、c = 1：
            //   低切 k = f → lowCutHz = 10 · 2^f；加宽 = 1.5 · f（相关度上限 10·log10(0.6) + 18 = 15.8 dB 不起作用）；
            //   收单自动态不做（ADR-015），它的代价测了也不用、不置位。三档：
            //   代价 0.1 dB → f = 1 → 20 Hz / 1.5 dB，不置位；
            //   代价 0.4 dB → f = (0.4 − 0.6) / (0.2 − 0.6) = 0.5 → 10 · 2^0.5 = 14.1421 Hz / 0.75 dB，置低切、加宽位（减量）；
            //   代价 0.8 dB → f = 0 → 关 / 0，置低切、加宽位（否决）。
            const auto clayAt = [] (float lowCutCostDb, float monoCostDb, float widthCostDb)
            {
                auto p = clayProfile();
                p.subShareDb.value = -28.0f;
                p.lowCorrelation.value = 0.0f;
                p.highSideToMidDb.value = -18.0f;
                setClayCosts (p, lowCutCostDb, monoCostDb, widthCostDb);
                return planFor (&p, Style::clean, 100.0f).clay;
            };

            const auto cheap = clayAt (0.1f, 0.1f, 0.1f);
            expectEquals (cheap.lowCutHz, 20.0f, utf8 ("代价 0.1 dB：低切照做"));
            expectEquals (cheap.monoAmount, 0.0f, utf8 ("ADR-015：自动不收单"));
            expectEquals (cheap.widthDb, 1.5f, utf8 ("代价 0.1 dB：加宽照做"));
            expectEquals (maskOf (cheap), 0, utf8 ("代价 0.1 dB：不置位"));

            const auto halved = clayAt (0.4f, 0.4f, 0.4f);
            expectWithinAbsoluteError (halved.lowCutHz, 14.142136f, 1.0e-4f, utf8 ("代价 0.4 dB：低切 k 减半"));
            expectEquals (halved.monoAmount, 0.0f, utf8 ("ADR-015：自动不收单"));
            expectWithinAbsoluteError (halved.widthDb, 0.75f, 1.0e-6f, utf8 ("代价 0.4 dB：加宽减半"));
            expectEquals (halved.monoHz, ClayPlan {}.monoHz);
            expectEquals (maskOf (halved), clayCostLimitedLowCut | clayCostLimitedWidth, utf8 ("代价 0.4 dB：低切、加宽置位（减量），收单不置"));

            const auto vetoed = clayAt (0.8f, 0.8f, 0.8f);
            expect (hasNeutralValues (vetoed), utf8 ("代价 0.8 dB：三项都否决（值与 ClayPlan{} 逐位相同）"));
            expectEquals (maskOf (vetoed), clayCostLimitedLowCut | clayCostLimitedWidth, utf8 ("代价 0.8 dB：低切、加宽置位（否决）"));

            // 逐项独立：只有一项贵时只动那一项、只置那一位，另两项与全便宜时逐位相同。
            const struct { float lowCut, mono, width; int bit; } single[] =
            {
                { 0.8f, 0.0f, 0.0f, clayCostLimitedLowCut },
                { 0.0f, 0.0f, 0.8f, clayCostLimitedWidth }
            };

            for (const auto& s : single)
            {
                const auto clay = clayAt (s.lowCut, s.mono, s.width);
                const auto what = utf8 ("只有位 ") + juce::String (s.bit) + utf8 (" 的一项贵");
                expectEquals (maskOf (clay), s.bit, what);
                expect (s.bit == clayCostLimitedLowCut ? clay.lowCutHz == 0.0f : sameBits (clay.lowCutHz, cheap.lowCutHz), what + utf8 ("：低切"));
                expect (s.bit == clayCostLimitedMono ? (clay.monoAmount == 0.0f && sameBits (clay.monoHz, ClayPlan {}.monoHz))
                                                     : (sameBits (clay.monoAmount, cheap.monoAmount) && sameBits (clay.monoHz, cheap.monoHz)),
                        what + utf8 ("：收单"));
                expect (s.bit == clayCostLimitedWidth ? clay.widthDb == 0.0f : sameBits (clay.widthDb, cheap.widthDb), what + utf8 ("：加宽"));
            }

            // 收单的代价再高也不影响方案、不置位（ADR-015：收单自动态不做）。
            expect (sameClay (clayAt (0.1f, 0.8f, 0.1f), cheap), utf8 ("收单代价 0.8 dB：方案与全便宜时逐位相同"));

            // 斜坡的形状（以加宽为例，原值 1.5 dB）：两端（0.2 dB 照做、0.6 dB 不做）、负代价（峰值反而变低）照做、中段线性
            // （0.3 → 0.75 倍、0.5 → 0.25 倍）；代价从 −0.5 扫到 1 dB，各项都单调不增。
            const auto widthAtCost = [&clayAt] (float costDb) { return clayAt (0.0f, 0.0f, costDb).widthDb; };
            expectEquals (widthAtCost (0.2f), 1.5f);
            expectEquals (maskOf (clayAt (0.0f, 0.0f, 0.2f)), 0, utf8 ("代价正好 0.2 dB：f = 1，不置位"));
            expectEquals (widthAtCost (0.6f), 0.0f);
            expectEquals (widthAtCost (-0.5f), 1.5f, utf8 ("峰值反而变低：照做"));
            expectWithinAbsoluteError (widthAtCost (0.3f), 1.125f, 1.0e-6f);
            expectWithinAbsoluteError (widthAtCost (0.5f), 0.375f, 1.0e-6f);

            ClayPlan previous = clayAt (-0.5f, -0.5f, -0.5f);
            int increases = 0;

            for (int step = -49; step <= 100; ++step)
            {
                const auto costDb = 0.01f * (float) step;
                const auto clay = clayAt (costDb, costDb, costDb);
                increases += (clay.lowCutHz > previous.lowCutHz || clay.monoAmount > previous.monoAmount || clay.widthDb > previous.widthDb) ? 1 : 0;
                previous = clay;
            }

            expectEquals (increases, 0, utf8 ("代价越高，三项都不增"));

            // 「本应 > 0」才置位：GLAZE 0.02（g = 0.0002、清理强度 0.0004 → k = 0.0004 不过门槛 0.001、加宽 1.5 · 0.0002）
            // ——低切本来就不开，贵也不置低切位；加宽本来就动（再小也 > 0），被否决照样置位；收单自动态不做，从不置位。
            {
                auto p = clayProfile();
                p.subShareDb.value = -28.0f;
                p.lowCorrelation.value = 0.0f;
                p.highSideToMidDb.value = -18.0f;
                setClayCosts (p, 0.8f, 0.8f, 0.8f);
                const auto faint = planFor (&p, Style::clean, 0.02f).clay;
                expect (hasNeutralValues (faint), utf8 ("GLAZE 0.02、代价 0.8 dB：三项都不动"));
                expectEquals (maskOf (faint), (int) clayCostLimitedWidth, utf8 ("低切本来就不开：不置低切位；收单从不置位"));

                setClayCosts (p, 0.0f, 0.0f, 0.0f);
                const auto faintCheap = planFor (&p, Style::clean, 0.02f).clay;
                expect (faintCheap.lowCutHz == 0.0f && faintCheap.widthDb > 0.0f && maskOf (faintCheap) == 0,
                        utf8 ("阳性对照：同一 GLAZE 代价 0 时加宽确实在动、低切确实不开"));
            }

            // 代价无效（没试过：有效块不满 10 s，或 NaN）→ 该项中性、不置位；另两项不受影响。
            const auto spoiledCost = [] (int how)
            {
                auto p = clayProfile();
                auto& f = p.lowCutPeakCostDb;

                if (how == 0)
                    f.valid = false;
                else
                    f.value = std::numeric_limits<float>::quiet_NaN();

                return planFor (&p, Style::clean, 100.0f).clay;
            };

            const auto baseProfile = clayProfile();
            const auto base = planFor (&baseProfile, Style::clean, 100.0f).clay;

            for (int how = 0; how < 2; ++how)
            {
                const auto clay = spoiledCost (how);
                expect (clay.lowCutHz == 0.0f && maskOf (clay) == 0 && sameBits (clay.monoAmount, base.monoAmount)
                            && sameBits (clay.widthDb, base.widthDb),
                        utf8 ("低切代价无效：只有低切中性、不置位，方式 ") + juce::String (how));
            }

            // 代价常数判断不了（旧快照读成 0 / 0、两端颠倒、非有限）→ 三项都中性、不置位；阳性对照：出厂常数三项都动。
            auto zeroZero = factoryConstants();
            zeroZero.clayCostFullDb = 0.0f;
            zeroZero.clayCostZeroDb = 0.0f;
            auto swapped = factoryConstants();
            swapped.clayCostFullDb = 0.6f;
            swapped.clayCostZeroDb = 0.2f;
            auto notFinite = factoryConstants();
            notFinite.clayCostZeroDb = std::numeric_limits<float>::infinity();
            const auto costlyProfile = clayProfile();

            expect (! hasNeutralValues (clayPlanWith (&costlyProfile, Style::clean, 100.0f, factoryConstants()).clay),
                    utf8 ("阳性对照：出厂代价常数下低切、加宽都动"));

            for (const auto* snapshot : { &zeroZero, &swapped, &notFinite })
                expect (isNeutral (clayPlanWith (&costlyProfile, Style::clean, 100.0f, *snapshot).clay),
                        utf8 ("代价常数判断不了：三项中性、不置位"));

            // 标定改了斜坡两端照样按公式：Zero 1.0 / Full 0.0 dB → 代价 0.4 dB 的 f = (0.4 − 1) / (0 − 1) = 0.6。
            auto wider = factoryConstants();
            wider.clayCostFullDb = 0.0f;
            wider.clayCostZeroDb = 1.0f;
            auto priced = clayProfile();
            priced.highSideToMidDb.value = -18.0f;
            setClayCosts (priced, 0.0f, 0.0f, 0.4f);
            expectWithinAbsoluteError (clayPlanWith (&priced, Style::clean, 100.0f, wider).clay.widthDb, 0.9f, 1.0e-6f,
                                       utf8 ("斜坡两端来自快照：加宽 1.5 × 0.6"));
        }

        //==============================================================================
        // ADR-015（2026-09-27 Max 试听「不自然、有点假」）：自动态的收单恒为中性。收单只去掉低频的侧声道 S，单声道回放听的是 M、
        // 收不收单都一样——所以不管低频多宽、多反相、收单代价多便宜、逐段统计里有多少宽段，自动方案的 monoAmount 都是 0、分频点都是
        // ClayPlan 的中性默认值，也不置收单的代价位。随机 3000 个画像 × 风格 × GLAZE（含宽 / 反相低频、全宽的逐段统计、负代价）逐位核对；
        // 阳性对照：同一批方案里低切或加宽确实在动（说明画像与 GLAZE 足以让 Clay 动作）。
        void testClayAutoMonoAlwaysNeutral()
        {
            beginTest ("ADR-015: the automatic plan never monos the bass (any correlation, crossover statistics or mono cost)");

            juce::Random r (20260927);
            int violations = 0, active = 0;

            for (int trial = 0; trial < 3000; ++trial)
            {
                auto profile = clayProfile();
                profile.lowCorrelation.value = -1.2f + 2.2f * r.nextFloat();
                profile.lowSideToMidDb.value = -30.0f + 30.0f * r.nextFloat();

                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    profile.bandSideToMidDb[b] = -5.0f * r.nextFloat();        // 全是「宽段」
                    profile.bandCorrelation[b] = -0.5f + 0.7f * r.nextFloat();
                    profile.bandStereoConfidence[b] = 1.0f;
                }

                const auto monoCost = -1.0f + 1.5f * r.nextFloat();

                for (auto& cost : profile.monoPeakCostDb)
                    setFeature (cost, monoCost, 1.0f);

                const auto style = (Style) r.nextInt (4);
                const auto glaze = 100.0f * r.nextFloat();
                const auto clay = planFor (&profile, style, glaze).clay;

                if (! (clay.monoAmount == 0.0f && sameBits (clay.monoHz, ClayPlan {}.monoHz)
                       && (clay.costLimitedMask & clayCostLimitedMono) == 0))
                    ++violations;

                active += (clay.lowCutHz > 0.0f || clay.widthDb > 0.0f) ? 1 : 0;
            }

            expectEquals (violations, 0, utf8 ("自动方案的收单恒为中性、不置收单代价位"));
            expect (active > 1000, utf8 ("阳性对照：低切或加宽确实在动，") + juce::String (active));
        }

        //==============================================================================
        void testClayNeutralCases()
        {
            beginTest ("Clay stays neutral at GLAZE 0, without a profile, and for missing or untrusted features");

            // 依据：第 2 节「任一所需特征 valid = false → 该项自动值为 0（旧画像即如此）；g = 0 → 三项全为 0」、第 6 节第 4 条；
            //       设计规格-Clay 第 7 节「旧画像没有立体声 / 超低频特征 → Clay 自动增量为 0 → 打开后声音不变」；
            //       GlazeTypes.h ClayPlan 的中性值（收单为 0 时分频点保持默认 80 Hz：中性 = 与 ClayPlan{} 逐位相同）。
            const auto profile = clayProfile();

            // 阳性对照：同一份画像 GLAZE 100 各风格三项都动。
            for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
            {
                const auto active = planFor (&profile, style, 100.0f).clay;
                expect (active.lowCutHz > 0.0f && active.widthDb > 0.0f, utf8 ("阳性对照：GLAZE 100 低切、加宽都应动作（收单自动态不做，ADR-015）"));
            }

            for (auto glaze : { 0.0f, -0.0f, -5.0f, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity() })
                for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                    expect (isNeutral (planFor (&profile, style, glaze).clay), "GLAZE " + juce::String (glaze) + utf8 ("：Clay 应全中性"));

            expect (isNeutral (planFor (nullptr, Style::automatic, 100.0f).clay), utf8 ("未学习（没有画像）：Clay 应全中性"));

            // 旧画像（格式 1：读入后立体声 / 超低频特征全部 valid = false、数组全 0）：GLAZE 100 各风格都全中性。
            auto legacy = makeProfile (-18.0f, -5.0f);
            legacy.formatVersion = 1;

            for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                expect (isNeutral (planFor (&legacy, style, 100.0f).clay), utf8 ("旧画像的 Clay 自动值应为 0"));

            // 逐个特征失效（0 = 无效、1 = 置信度 0、2 = 有效但数值 NaN）：只让用到它的那一项回中性，另两项逐位不变。
            // 低切只用真次声占比；收单只用低频 ρ（ADR-012：需要度改按单声道回放损失，低频 S/M 不再用）；加宽只用高频 S/M（上限也由它
            // 反推）；低频 S/M、高频 ρ 不是所需特征。
            const auto reference = planFor (&profile, Style::clean, 100.0f).clay;
            const auto spoiled = [] (Feature MusicProfile::* member, int how)
            {
                auto p = clayProfile();
                auto& f = p.*member;

                if (how == 0)
                    f.valid = false;
                else if (how == 1)
                    f.confidence = 0.0f;
                else
                    f.value = std::numeric_limits<float>::quiet_NaN();

                return planFor (&p, Style::clean, 100.0f).clay;
            };

            for (int how = 0; how < 3; ++how)
            {
                const auto what = utf8 ("，失效方式 ") + juce::String (how);
                const auto noSub = spoiled (&MusicProfile::subShareDb, how);
                expect (sameBits (noSub.lowCutHz, 0.0f), utf8 ("真次声占比失效：不低切") + what);
                expect (sameBits (noSub.monoAmount, reference.monoAmount) && sameBits (noSub.monoHz, reference.monoHz)
                            && sameBits (noSub.widthDb, reference.widthDb),
                        utf8 ("真次声占比失效：另两项不变") + what);

                const auto noMono = spoiled (&MusicProfile::lowCorrelation, how);
                expect (sameBits (noMono.monoAmount, 0.0f) && sameBits (noMono.monoHz, ClayPlan {}.monoHz),
                        utf8 ("低频 ρ 失效：不收单") + what);
                expect (sameBits (noMono.lowCutHz, reference.lowCutHz) && sameBits (noMono.widthDb, reference.widthDb),
                        utf8 ("低频 ρ 失效：另两项不变") + what);

                const auto noHigh = spoiled (&MusicProfile::highSideToMidDb, how);
                expect (sameBits (noHigh.widthDb, 0.0f), utf8 ("高频 S/M 失效：不加宽") + what);
                expect (sameBits (noHigh.lowCutHz, reference.lowCutHz) && sameBits (noHigh.monoAmount, reference.monoAmount),
                        utf8 ("高频 S/M 失效：另两项不变") + what);

                for (auto member : { &MusicProfile::lowSideToMidDb, &MusicProfile::highCorrelation })
                    expect (sameClay (spoiled (member, how), reference), utf8 ("低频 S/M、高频 ρ 不是所需特征：三项都不变") + what);
            }

            // 峰值代价失效（ADR-012「代价无效 → 该项中性」）：0 = 无效、1 = 置信度 0（代价不看置信度：不影响）、2 = 数值 NaN。
            // 低切只看低切代价、加宽只看加宽代价；收单只看分频点用到的候选（逐段全窄 → 默认 80 Hz = 第 2 个候选点，只用它）。
            const auto spoiledCost = [] (int which, int how)
            {
                auto p = clayProfile();
                auto& f = which == 0 ? p.lowCutPeakCostDb : (which == 1 ? p.widthPeakCostDb : p.monoPeakCostDb[(size_t) (which - 2)]);

                if (how == 0)
                    f.valid = false;
                else if (how == 1)
                    f.confidence = 0.0f;
                else
                    f.value = std::numeric_limits<float>::quiet_NaN();

                return planFor (&p, Style::clean, 100.0f).clay;
            };

            for (int how = 0; how < 3; ++how)
            {
                const auto what = utf8 ("，代价失效方式 ") + juce::String (how);

                if (how == 1)
                {
                    for (int which = 0; which < 6; ++which)
                        expect (sameClay (spoiledCost (which, how), reference), utf8 ("代价的置信度不参与：三项都不变，第 ") + juce::String (which) + what);

                    continue;
                }

                const auto noLowCutCost = spoiledCost (0, how);
                expect (noLowCutCost.lowCutHz == 0.0f && maskOf (noLowCutCost) == 0 && sameBits (noLowCutCost.monoAmount, reference.monoAmount)
                            && sameBits (noLowCutCost.widthDb, reference.widthDb),
                        utf8 ("低切代价失效：只有低切中性、不置位") + what);

                const auto noWidthCost = spoiledCost (1, how);
                expect (noWidthCost.widthDb == 0.0f && maskOf (noWidthCost) == 0 && sameBits (noWidthCost.lowCutHz, reference.lowCutHz)
                            && sameBits (noWidthCost.monoAmount, reference.monoAmount),
                        utf8 ("加宽代价失效：只有加宽中性、不置位") + what);

                const auto noMonoCost = spoiledCost (2 + 1, how);   // 80 Hz 候选
                expect (noMonoCost.monoAmount == 0.0f && sameBits (noMonoCost.monoHz, ClayPlan {}.monoHz) && maskOf (noMonoCost) == 0
                            && sameBits (noMonoCost.lowCutHz, reference.lowCutHz) && sameBits (noMonoCost.widthDb, reference.widthDb),
                        utf8 ("分频点所用的收单代价失效：只有收单中性、不置位") + what);

                for (int unused : { 2 + 0, 2 + 2, 2 + 3 })   // 60 / 120 / 160 Hz 候选：80 Hz 的分频点用不到
                    expect (sameClay (spoiledCost (unused, how), reference), utf8 ("用不到的收单候选失效：不影响") + what);
            }

            // 没试过（三类代价全都没有，例如候选的有效块不满 10 s）：需要度再高也三项全中性、不置位——没试过就不用。
            auto untried = clayProfile();
            untried.lowCutPeakCostDb = Feature {};
            untried.monoPeakCostDb.fill (Feature {});
            untried.widthPeakCostDb = Feature {};

            for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                expect (isNeutral (planFor (&untried, style, 100.0f).clay), utf8 ("没试过（代价全无效）：三项中性、不置位"));

            // 逐段统计全没有（bandStereoConfidence 0；没有统计的段 S/M、ρ 都记 0，看起来很「宽」但不许当宽段）：
            // 收单照样按标量特征给，分频点取默认值。
            auto noBands = clayProfile();
            noBands.bandStereoConfidence.fill (0.0f);
            noBands.bandSideToMidDb.fill (0.0f);
            noBands.bandCorrelation.fill (0.0f);
            const auto noBandPlan = planFor (&noBands, Style::clean, 100.0f).clay;
            expect (sameBits (noBandPlan.monoAmount, reference.monoAmount) && sameBits (noBandPlan.monoHz, factoryConstants().clayMonoDefaultHz),
                    utf8 ("没有逐段统计：照样收单，分频点取默认值"));
        }

        //==============================================================================
        void testClayMonotonicAndBounded()
        {
            beginTest ("Clay values never decrease as GLAZE rises and stay inside their caps");

            // 依据：第 6 节第 4 条「对 g 单调；各项不超上限；加宽后预测 ρ' ≥ 0.25」、第 2 节（分频点只由逐段统计定，收单开着时不随
            //       GLAZE 挪）；ClayModule 的参数范围。随机场景复用 randomScenario（画像 / 风格 / 手动值 / 约束反馈都随机）并补上随机 Clay
            //       特征；每三个场景有一个换用「加宽上限会起作用」的快照（相关度下限 0.94、窄的起点 0 dB），上限的定量核对见 testClayWidthCap。
            auto tight = factoryConstants();
            tight.clayMinCorrelationAfterWidth = 0.94f;
            tight.clayWidthNarrowStartDb = 0.0f;

            // ADR-012 追加：代价位只有定义过的三位、低切 / 加宽置位时该项代价确实有效且 > clayCostFullDb（f < 1）；同一场景把代价
            //       全改成 0 dB（f = 1）重算，三项都不小于实际方案（代价只减不增）。
            juce::Random random (0x5eedc1a7);
            int failures = 0, active = 0, checked = 0;

            for (int trial = 0; trial < 120; ++trial)
            {
                auto scenario = randomScenario (random);
                addRandomClayFeatures (scenario.profile, random);
                const auto& constants = trial % 3 == 0 ? tight : factoryConstants();
                auto cheapProfile = scenario.profile;
                setClayCosts (cheapProfile, 0.0f, 0.0f, 0.0f);
                ClayPlan previous;
                auto crossover = 0.0f;   // 这一轮第一次开收单时的分频点（之后必须逐位不变）

                for (int step = 0; step <= 200; ++step)
                {
                    PlanInputs in;
                    in.profile = scenario.hasProfile ? &scenario.profile : nullptr;
                    in.style = scenario.style;
                    in.glazePercent = 0.5f * (float) step;
                    in.manual = scenario.manual;
                    in.maxGainDb = scenario.maxGainDb;
                    in.constants = &constants;
                    const auto plan = makePlan (in);
                    const auto& clay = plan.clay;
                    const auto sc = blendConstants (constants, plan.weights);
                    ++checked;

                    auto cheapIn = in;
                    cheapIn.profile = scenario.hasProfile ? &cheapProfile : nullptr;
                    const auto cheap = makePlan (cheapIn).clay;

                    // 单调不减。
                    auto ok = clay.lowCutHz >= previous.lowCutHz && clay.monoAmount >= previous.monoAmount && clay.widthDb >= previous.widthDb;

                    // 上限：低切 0 或在 [地板, 风格混合的 f_t] 内（且在 ClayModule 的 10 ～ 60 Hz 内）；收单 0 ～ 1；加宽 0 ～ 1.5 dB，
                    // 且按画像预测的加宽后相关度不低于下限（容差只给 float 舍入）。
                    ok = ok && (clay.lowCutHz == 0.0f
                                || (clay.lowCutHz >= constants.clayLowCutFloorHz && clay.lowCutHz <= sc.clayLowCutTargetHz + 1.0e-3f
                                    && clay.lowCutHz >= ClayModule::minLowCutHz && clay.lowCutHz <= ClayModule::maxLowCutHz));
                    ok = ok && clay.monoAmount >= 0.0f && clay.monoAmount <= 1.0f;
                    ok = ok && clay.widthDb >= 0.0f && clay.widthDb <= constants.clayWidthMaxDb;

                    if (clay.widthDb > 0.0f)
                        ok = ok && predictedCorrelation (clay.widthDb, scenario.profile.highSideToMidDb.value)
                                       >= (double) constants.clayMinCorrelationAfterWidth - 1.0e-4;

                    // 分频点：收单开着时在 [60, 160] Hz 且整轮逐位不变；收单为 0 时保持默认值（中性）。
                    if (clay.monoAmount > 0.0f)
                    {
                        if (crossover == 0.0f)
                            crossover = clay.monoHz;

                        ok = ok && sameBits (clay.monoHz, crossover)
                                && clay.monoHz >= constants.clayMonoMinHz && clay.monoHz <= constants.clayMonoMaxHz;
                    }
                    else
                    {
                        ok = ok && sameBits (clay.monoHz, ClayPlan {}.monoHz);
                    }

                    // GLAZE 0 或没有画像：全中性（含代价位为 0）。
                    if (step == 0 || ! scenario.hasProfile)
                        ok = ok && isNeutral (clay);

                    // 代价位与「代价只减不增」。
                    const auto& stereo = scenario.profile;
                    ok = ok && (maskOf (clay) & ~allCostBits) == 0;

                    if ((clay.costLimitedMask & clayCostLimitedLowCut) != 0)
                        ok = ok && stereo.lowCutPeakCostDb.valid && stereo.lowCutPeakCostDb.value > constants.clayCostFullDb;

                    if ((clay.costLimitedMask & clayCostLimitedWidth) != 0)
                        ok = ok && stereo.widthPeakCostDb.valid && stereo.widthPeakCostDb.value > constants.clayCostFullDb;

                    ok = ok && clay.lowCutHz <= cheap.lowCutHz && clay.monoAmount <= cheap.monoAmount && clay.widthDb <= cheap.widthDb;

                    if (! ok && ++failures <= 5)
                        logMessage (utf8 ("  Clay 违反单调或上限：场景 ") + juce::String (trial) + utf8 ("，GLAZE ") + juce::String (in.glazePercent));

                    active += hasNeutralValues (clay) ? 0 : 1;
                    previous = clay;
                }
            }

            logMessage ("  " + juce::String (checked) + " GLAZE steps, " + juce::String (active) + " with Clay active");
            expectEquals (failures, 0, utf8 ("Clay 应随 GLAZE 单调不减、不超上限"));
            expect (active > checked / 4, utf8 ("阳性对照：随机场景里应有足够多的 Clay 动作"));
        }

        //==============================================================================
        void testClayWidthCap()
        {
            beginTest ("Automatic widening respects the predicted correlation floor");

            // 依据：第 2 节③「按画像预测加宽后的高频相关度 ρ' = (P_M − γ²P_S) / (P_M + γ²P_S) ≥ 0.25（γ = 10^(widthDb/20)），
            //       即 γ² ≤ 0.6 · P_M / P_S；取两者较小」、第 6 节第 4 条（高频侧中比 ≥ −10 dB 时不加宽；加宽后预测 ρ' ≥ 0.25；不超上限）。
            //       出厂常数下这个上限碰不到（要加宽须 S/M < −10 dB，此时上限 > 10·log10(0.6) + 10 = 7.8 dB ≫ 1.5 dB）：先扫一遍出厂常数，
            //       再把相关度下限标到 0.94 让它起作用（k = (1 − 0.94) / (1 + 0.94) = 0.030928，上限 = 10·log10(k) − S/M = −15.0965 − S/M）。
            const auto& c = factoryConstants();
            int widened = 0;

            for (int tenth = -300; tenth <= 0; ++tenth)   // 高频 S/M −30 ～ 0 dB，每 0.1 dB（整数相除：−100 / 10 正好是 −10 dB 边界）
            {
                auto scanned = clayProfile();
                const auto sideToMid = (float) tenth / 10.0f;
                scanned.highSideToMidDb.value = sideToMid;
                const auto width = planFor (&scanned, Style::clean, 100.0f).clay.widthDb;

                if (sideToMid >= c.clayWidthNarrowStartDb)
                {
                    expectEquals (width, 0.0f, utf8 ("高频侧中比 ≥ −10 dB 时不加宽：") + juce::String (sideToMid));
                }
                else
                {
                    widened += width > 0.0f ? 1 : 0;
                    expect (width <= c.clayWidthMaxDb && predictedCorrelation (width, sideToMid) >= (double) c.clayMinCorrelationAfterWidth,
                            utf8 ("加宽不超 1.5 dB、加宽后预测相关度 ≥ 0.25：") + juce::String (sideToMid));
                }
            }

            expectEquals (widened, 200, utf8 ("阳性对照：S/M −30 ～ −10.1 dB 的 200 个点都应加宽"));

            // 相关度下限 0.94：高频 S/M −16 dB，need = (−16 + 10) / (−18 + 10) = 0.75 → 想要 1.5 · 0.75 = 1.125 dB（通透、GLAZE 100、c = 1）；
            // 上限 = −15.0965 + 16 = 0.9035 dB → 取上限，此时预测 ρ' 正好 = 0.94。
            auto tight = factoryConstants();
            tight.clayMinCorrelationAfterWidth = 0.94f;
            auto narrow = clayProfile();
            narrow.highSideToMidDb.value = -16.0f;
            const auto cappedWidth = clayPlanWith (&narrow, Style::clean, 100.0f, tight).clay.widthDb;
            expectWithinAbsoluteError (cappedWidth, 0.90350f, 1.0e-4f, utf8 ("加宽应被相关度上限封顶"));
            expectWithinAbsoluteError (predictedCorrelation (cappedWidth, -16.0f), 0.94, 1.0e-5, utf8 ("封顶处预测相关度 = 下限"));

            // 阳性对照：上限不起作用时就是公式值——出厂下限 0.25 给 1.125 dB；下限 0.94、GLAZE 50 想要 0.5625 dB < 上限，照给。
            expectWithinAbsoluteError (planFor (&narrow, Style::clean, 100.0f).clay.widthDb, 1.125f, 1.0e-6f);
            expectWithinAbsoluteError (clayPlanWith (&narrow, Style::clean, 50.0f, tight).clay.widthDb, 0.5625f, 1.0e-6f);

            // 上限为负（下限 0.97：10·log10(0.03 / 1.97) + 16 = −2.17 dB）→ 不加宽（下限 0：自动从不收窄）。
            tight.clayMinCorrelationAfterWidth = 0.97f;
            expectEquals (clayPlanWith (&narrow, Style::clean, 100.0f, tight).clay.widthDb, 0.0f);

            // 下限 ≥ 1 或非有限：一点都不加；≤ −1：不设上限（= 公式值 1.125 dB）。
            tight.clayMinCorrelationAfterWidth = 1.0f;
            expectEquals (clayPlanWith (&narrow, Style::clean, 100.0f, tight).clay.widthDb, 0.0f);
            tight.clayMinCorrelationAfterWidth = std::numeric_limits<float>::quiet_NaN();
            expectEquals (clayPlanWith (&narrow, Style::clean, 100.0f, tight).clay.widthDb, 0.0f);
            tight.clayMinCorrelationAfterWidth = -1.0f;
            expectWithinAbsoluteError (clayPlanWith (&narrow, Style::clean, 100.0f, tight).clay.widthDb, 1.125f, 1.0e-6f);

            // 加宽上限 clayWidthMaxDb 本身：标到 1 dB → S/M −18 dB（need 1）时给 1 dB 而不是 1.5；标到 0 → 不加宽。
            auto smaller = factoryConstants();
            smaller.clayWidthMaxDb = 1.0f;
            auto veryNarrow = clayProfile();
            veryNarrow.highSideToMidDb.value = -18.0f;
            expectEquals (clayPlanWith (&veryNarrow, Style::clean, 100.0f, smaller).clay.widthDb, 1.0f);
            smaller.clayWidthMaxDb = 0.0f;
            expectEquals (clayPlanWith (&veryNarrow, Style::clean, 100.0f, smaller).clay.widthDb, 0.0f);
            expectEquals (planFor (&veryNarrow, Style::clean, 100.0f).clay.widthDb, 1.5f, utf8 ("阳性对照：出厂上限 1.5 dB"));
        }

        //==============================================================================
        void testClayStyleBlend()
        {
            beginTest ("Clay targets follow the style weights");

            // 依据：第 2 节：f_t = w·(16, 18, 20) Hz（ADR-012 / 根治修订 3.3 改）、s_w = w·(0.4, 0.6, 1.0)，w = 方案的风格权重（离散风格
            //       独热；自动 = 画像锁定的混合权重，与其他各项同一份，GlazePlanner.h blendConstants）。
            const auto& c = factoryConstants();

            // blendConstants：离散风格逐位就是那一种；温润 / 通透各半 → f_t = 0.5·16 + 0.5·20 = 18 Hz、s_w = 0.5·0.4 + 0.5·1.0 = 0.7。
            for (auto style : { Style::warm, Style::punchy, Style::clean })
            {
                const auto& own = constantsFor (c, style);
                const auto blended = blendConstants (c, weightsFor (style, {}));
                expect (sameBits (blended.clayLowCutTargetHz, own.clayLowCutTargetHz) && sameBits (blended.clayWidthStyleFactor, own.clayWidthStyleFactor),
                        utf8 ("离散风格的 Clay 风格常数应逐位就是那一种"));
            }

            const auto halfHalf = blendConstants (c, { 0.5f, 0.0f, 0.5f });
            expectWithinAbsoluteError (halfHalf.clayLowCutTargetHz, 18.0f, 1.0e-5f);
            expectWithinAbsoluteError (halfHalf.clayWidthStyleFactor, 0.7f, 1.0e-6f);

            // 方案：锁定 {0.5, 0, 0.5} → 低切 10 · (18 / 10)^0.5 = 13.4164 Hz、加宽 1.5 · 0.5 · 0.7 = 0.525 dB、收单 0.5（与风格无关）；
            // 锁定独热 {0, 1, 0} = 冲击（Clay 逐位相同）。
            const auto profile = clayProfile();
            const StyleWeights warmClean { 0.5f, 0.0f, 0.5f }, allPunchy { 0.0f, 1.0f, 0.0f };
            const auto mixed = planFor (&profile, Style::automatic, 100.0f, defaultManual(), 1.0e9f, &warmClean).clay;
            expectWithinAbsoluteError (mixed.lowCutHz, 13.416408f, 1.0e-4f);
            expectWithinAbsoluteError (mixed.widthDb, 0.525f, 1.0e-6f);
            expectEquals (mixed.monoAmount, 0.0f, utf8 ("ADR-015：自动不收单，与风格无关"));
            expect (sameClay (planFor (&profile, Style::automatic, 100.0f, defaultManual(), 1.0e9f, &allPunchy).clay,
                              planFor (&profile, Style::punchy, 100.0f).clay),
                    utf8 ("锁定独热权重 = 对应的离散风格"));

            // 自动且权重没锁：用这份画像判出的权重（plan.weights），f_t、s_w 按它混合。
            const auto automatic = planFor (&profile, Style::automatic, 100.0f);
            const auto sc = blendConstants (c, automatic.weights);
            expectWithinAbsoluteError (automatic.clay.lowCutHz, 10.0f * std::sqrt (sc.clayLowCutTargetHz / 10.0f), 1.0e-4f);
            expectWithinAbsoluteError (automatic.clay.widthDb, 0.75f * sc.clayWidthStyleFactor, 1.0e-6f);

            // 方向：低切与加宽都是 温润 < 冲击 < 通透（温润留得最多、通透最开阔）。
            const auto warm = planFor (&profile, Style::warm, 100.0f).clay;
            const auto punchy = planFor (&profile, Style::punchy, 100.0f).clay;
            const auto clean = planFor (&profile, Style::clean, 100.0f).clay;
            expect (warm.lowCutHz < punchy.lowCutHz && punchy.lowCutHz < clean.lowCutHz, utf8 ("低切：温润 < 冲击 < 通透"));
            expect (warm.widthDb < punchy.widthDb && punchy.widthDb < clean.widthDb, utf8 ("加宽：温润 < 冲击 < 通透"));
        }

        //==============================================================================
        void testClayIndependentOfLevelPlan()
        {
            beginTest ("Clay ignores manual settings and loudness feedback, and never changes the level plan");

            // 依据：第 2 节「响度与峰值模型不改：Clay 对 K 加权响度影响 < 0.2 LU，由现有校准器吸收」；ADR-009（Clay 的自动值是绝对值，
            //       三态在编排器里合成，决策器不看抽屉设置）。所以：① 手动值、抽屉设置、约束反馈 maxGainDb 都不改变 Clay；
            //       ② 画像多了 Clay 特征，方案其余字段（增量、目标、权重、受限标志、版本）与没有时逐位相同。
            const auto profile = clayProfile();
            const auto reference = planFor (&profile, Style::warm, 70.0f);
            expect (! isNeutral (reference.clay), utf8 ("阳性对照：这份画像 GLAZE 70 会修坯"));
            expect (reference.effectiveGainDb > 1.0f, utf8 ("阳性对照：这份画像 GLAZE 70 会推响"));

            auto heavy = defaultManual();
            heavy.limit.gainDb = 12.0f;
            heavy.color.heat = 80.0f;
            auto bypassed = defaultManual();
            bypassed.limit.bypass = true;
            bypassed.color.bypass = true;
            bypassed.balance.bypass = true;
            auto drawer = defaultManual();
            drawer.claySettings.lowCutMode = ClayMode::off;
            drawer.claySettings.widthMode = ClayMode::manual;
            drawer.claySettings.widthDb = -3.0f;
            drawer.claySettings.bypass = true;
            drawer.clay.lowCutHz = 60.0f;

            for (const auto* manual : { &heavy, &bypassed, &drawer })
                expect (sameClay (planFor (&profile, Style::warm, 70.0f, *manual).clay, reference.clay), utf8 ("手动值与抽屉设置不改变 Clay 的自动值"));

            expect (planFor (&profile, Style::warm, 70.0f, heavy).limitedByManual && planFor (&profile, Style::warm, 70.0f, bypassed).limitedByManual,
                    utf8 ("阳性对照：这些手动值确实改了电平方案"));

            for (auto maxGain : { -3.0f, reference.effectiveGainDb - 1.0f, std::numeric_limits<float>::quiet_NaN() })
            {
                const auto capped = planFor (&profile, Style::warm, 70.0f, defaultManual(), maxGain);
                expect (sameClay (capped.clay, reference.clay), utf8 ("约束反馈不改变 Clay"));
                expect (capped.effectiveGainDb < reference.effectiveGainDb - 0.5f, utf8 ("阳性对照：约束反馈确实压低了净增益"));
            }

            // ② 随机场景：同一份画像加上 / 不加 Clay 特征，方案除 clay 以外逐位相同（samePlan 不比 clay）。
            juce::Random random (0x5eedc1a8);
            int differences = 0, active = 0;

            for (int trial = 0; trial < 60; ++trial)
            {
                const auto scenario = randomScenario (random);
                auto stereo = scenario;
                addRandomClayFeatures (stereo.profile, random);

                for (auto glaze : { 0.0f, 20.0f, 55.0f, 100.0f })
                {
                    const auto plain = planFor (scenario, glaze);
                    const auto withClay = planFor (stereo, glaze);
                    differences += samePlan (plain, withClay) ? 0 : 1;
                    active += isNeutral (withClay.clay) ? 0 : 1;
                    expect (isNeutral (plain.clay));
                }
            }

            expectEquals (differences, 0, utf8 ("Clay 特征不应改变方案的其余字段"));
            expect (active > 0, utf8 ("阳性对照：加上的 Clay 特征确实让 Clay 动作"));
        }

        //==============================================================================
        void testClayConfidence()
        {
            beginTest ("A less confident profile never cleans or widens more than the same profile fully trusted");

            // 依据：决策与校准第 2 节「置信度低时处理量按比例收紧」、v0.6 验收 F-05 的同一原则（GlazePlanner.cpp makePlan 的封顶）：
            //       Clay 三项随各自特征的可信度单调；「自动」且权重没锁时，置信度低 → 权重偏通透（低切目标、加宽系数都最大），由
            //       「置信度拉满、权重重判」的方案逐项封顶。同一份画像（逐段统计不动）置信度随机压低，任何风格下三项都不超过拉满时。
            juce::Random rng (0x5eedc1a9);
            int checked = 0, violations = 0, tighter = 0;

            for (int trial = 0; trial < 200; ++trial)
            {
                const auto lufs = -30.0f + 20.0f * rng.nextFloat();
                const auto lra = 2.0f + 14.0f * rng.nextFloat();
                const auto confidence = 0.75f * rng.nextFloat();
                auto unsure = makeProfile (lufs, lufs + 12.0f, lra, confidence);
                const auto onsetStrength = 14.0f * rng.nextFloat();
                const auto onsetDensity = 6.0f * rng.nextFloat();
                setFeature (unsure.onsetStrengthDb, onsetStrength, confidence);
                setFeature (unsure.onsetDensityPerSecond, onsetDensity, confidence);

                const auto sub = -44.0f + 20.0f * rng.nextFloat();       // 真次声斜坡 −40 → −28 dB 两侧
                const auto lowSide = -26.0f + 20.0f * rng.nextFloat();
                const auto lowRho = -0.2f + 1.2f * rng.nextFloat();      // 收单需要度 ρ 0.78 → 0 两侧
                const auto highSide = -22.0f + 16.0f * rng.nextFloat();
                const auto clayConfidence = rng.nextFloat();
                setClayFeatures (unsure, sub, lowSide, lowRho, highSide, 0.5f, clayConfidence);
                // 峰值代价（ADR-012）两份画像相同：代价系数不看置信度，拉满前后一样；取值多半落在照做 / 减量区，少数否决。
                const auto lowCutCost = -0.3f + rng.nextFloat();
                const auto monoCost = -0.3f + rng.nextFloat();
                const auto widthCost = -0.3f + rng.nextFloat();
                setClayCosts (unsure, lowCutCost, monoCost, widthCost, clayConfidence);
                const auto wideBand = rng.nextInt (7);   // 某一个低频段偏宽：分频点不是默认值
                unsure.bandSideToMidDb[(size_t) wideBand] = -5.0f;

                auto sure = unsure;

                for (auto* f : { &sure.integratedLufs, &sure.loudnessRangeLu, &sure.plrDb, &sure.truePeakDbtp, &sure.spectralSlopeDbPerOct,
                                 &sure.lowShare, &sure.highShare, &sure.lowBandRangeDb, &sure.onsetDensityPerSecond, &sure.onsetStrengthDb,
                                 &sure.subShareDb, &sure.lowSideToMidDb, &sure.lowCorrelation, &sure.highSideToMidDb, &sure.highCorrelation })
                    f->confidence = 1.0f;

                for (auto& band : sure.bandConfidence)
                    band = 1.0f;

                sure.representativeness = 1.0f;

                for (auto style : { Style::warm, Style::punchy, Style::clean, Style::automatic })
                {
                    const auto glaze = 100.0f * rng.nextFloat();
                    const auto low = planFor (&unsure, style, glaze).clay;
                    const auto high = planFor (&sure, style, glaze).clay;
                    ++checked;

                    const bool ok = low.lowCutHz <= high.lowCutHz + 1.0e-4f && low.monoAmount <= high.monoAmount + 1.0e-6f
                                 && low.widthDb <= high.widthDb + 1.0e-6f
                                 && (! (low.monoAmount > 0.0f) || ! (high.monoAmount > 0.0f) || sameBits (low.monoHz, high.monoHz));
                    violations += ok ? 0 : 1;
                    tighter += (low.lowCutHz < high.lowCutHz || low.monoAmount < high.monoAmount || low.widthDb < high.widthDb) ? 1 : 0;
                }
            }

            logMessage ("  " + juce::String (checked) + " comparisons, " + juce::String (tighter) + " where the unsure plan cleaned or widened less");
            expectEquals (violations, 0, utf8 ("置信度低的画像不许比拉满时修得更多"));
            expect (tighter > checked / 4, utf8 ("阳性对照：置信度确实收紧了 Clay"));
        }

        //==============================================================================
        void testClayConstants()
        {
            beginTest ("Clay constants: factory values, lossless snapshots, legacy snapshots read as neutral");

            // 依据：第 2 节（常数取值；「决策器的新常数进入出厂标定快照（GlazeConstants，随工程保存）」）；设计规格-Clay 第 7 节
            //       （旧工程打开声音不变）；GlazeConstants.h「旧快照缺这些字段按 0 读」、格式版本不变（与 minimumFullDriveDb 等同一做法）；
            //       时间线与保存第 5 节（快照往返逐位相同；常数调整 version + 1）。
            const auto& factory = factoryConstants();

            // ---- 出厂值：风格常数与规格表一致；快照版本随新常数升到 ≥ 7；结构体默认的风格常数是中性占位 0 ----
            // 低切目标 16 / 18 / 20 Hz（ADR-012 / 根治修订 3.3：只切听不到的次声），且都不高于分析器低切候选的 20 Hz（代价按 20 Hz 量，
            // 实际低切更低、抬峰值只会更少）。
            expectEquals (factory.warm.clayLowCutTargetHz, 16.0f);
            expectEquals (factory.punchy.clayLowCutTargetHz, 18.0f);
            expectEquals (factory.clean.clayLowCutTargetHz, 20.0f);
            expectEquals (factory.warm.clayWidthStyleFactor, 0.4f);
            expectEquals (factory.punchy.clayWidthStyleFactor, 0.6f);
            expectEquals (factory.clean.clayWidthStyleFactor, 1.0f);
            expect (factory.version >= 7, utf8 ("加入修坯常数后，出厂标定快照版本应 ≥ 7"));
            expect (factory.algorithmVersion == currentAlgorithmVersion && currentAlgorithmVersion >= 7, utf8 ("修坯 Clay 属于算法 7"));
            expectEquals (StyleConstants {}.clayLowCutTargetHz, 0.0f, utf8 ("结构体默认的风格常数是中性占位"));
            expectEquals (StyleConstants {}.clayWidthStyleFactor, 0.0f);

            // 全局常数（默认值即出厂值）与规格第 2、3 节及根治修订第 3 节逐项一致。
            expectEquals (factory.clayGlazeFullAt, 0.5f);
            expectEquals (factory.claySubShareLowDb, -40.0f);
            expectEquals (factory.claySubShareHighDb, -28.0f);
            expectEquals (factory.clayLowCutFloorHz, 10.0f);
            expectEquals (factory.clayLowCutMinK, 0.001f);
            expectEquals (factory.clayMonoBandSideRatioDb, -15.0f);
            expectEquals (factory.clayMonoBandCorrelation, 0.3f);
            expectEquals (factory.clayMonoMinHz, 60.0f);
            expectEquals (factory.clayMonoMaxHz, 160.0f);
            expectEquals (factory.clayMonoDefaultHz, 80.0f);
            expectEquals (factory.clayWidthNarrowStartDb, -10.0f);
            expectEquals (factory.clayWidthNarrowFullDb, -18.0f);
            expectEquals (factory.clayWidthMaxDb, 1.5f);
            expectEquals (factory.clayMinCorrelationAfterWidth, 0.25f);
            expectEquals (factory.clayGuardPhaseCorrelation, -0.2f, utf8 ("ADR-012：慢路 ρ < −0.2 才算反相（单声道回放损失 > 4 dB）"));
            expectEquals (factory.clayGuardPhaseAttackSeconds, 0.1);
            expectEquals (factory.clayGuardPhaseHoldSeconds, 2.0);
            expectEquals (factory.clayGuardPhaseReleaseSeconds, 2.0);
            expectEquals (factory.clayGuardWidthAttackSeconds, 0.2);
            expectEquals (factory.clayGuardWidthReleaseSeconds, 2.0);
            // ADR-012 新常数（根治修订 3.3 / 3.4）：代价系数 ramp(代价, 0.6, 0.2)、收单需要度 ramp(损失, −0.5, −3)、反相快路门 0.1（ADR-014 由 0.3 改）、
            // 最短持续 0.15 s。
            expectEquals (factory.clayCostFullDb, 0.2f);
            expectEquals (factory.clayCostZeroDb, 0.6f);
            expectEquals (factory.clayMonoLossLowDb, -0.5f);
            expectEquals (factory.clayMonoLossHighDb, -3.0f);
            expectEquals (factory.clayGuardPhaseFastGate, 0.1f);   // ADR-014：0.3 → 0.1
            expectEquals (factory.clayGuardPhaseMinSeconds, 0.15);

            // ---- 快照读写 ----
            // 字段表独立于 GlazeConstants.cpp 的访问器：逐项核对「全部 32 个 clay* 字段都进了快照、都能按位读回、缺了都按 0 读」。
            // ADR-012 删掉了 clayMonoSideRatioLowDb / HighDb、clayMonoCorrelationLow / High（4 个），加了代价系数两端、收单损失两端、
            // 反相快路门（float 5 个）与最短持续秒数（double 1 个）：30 − 4 + 6 = 32。
            float Constants::* const floatFields[] =
            {
                &Constants::clayGlazeFullAt, &Constants::claySubShareLowDb, &Constants::claySubShareHighDb, &Constants::clayLowCutFloorHz,
                &Constants::clayLowCutMinK, &Constants::clayMonoBandSideRatioDb,
                &Constants::clayMonoBandCorrelation, &Constants::clayMonoMinHz, &Constants::clayMonoMaxHz, &Constants::clayMonoDefaultHz,
                &Constants::clayWidthNarrowStartDb, &Constants::clayWidthNarrowFullDb, &Constants::clayWidthMaxDb,
                &Constants::clayMinCorrelationAfterWidth, &Constants::clayGuardPhaseCorrelation,
                &Constants::clayCostFullDb, &Constants::clayCostZeroDb, &Constants::clayMonoLossLowDb, &Constants::clayMonoLossHighDb,
                &Constants::clayGuardPhaseFastGate
            };
            double Constants::* const doubleFields[] =
            {
                &Constants::clayGuardPhaseAttackSeconds, &Constants::clayGuardPhaseHoldSeconds, &Constants::clayGuardPhaseReleaseSeconds,
                &Constants::clayGuardWidthAttackSeconds, &Constants::clayGuardWidthReleaseSeconds, &Constants::clayGuardPhaseMinSeconds
            };
            float StyleConstants::* const styleFields[] = { &StyleConstants::clayLowCutTargetHz, &StyleConstants::clayWidthStyleFactor };
            StyleConstants Constants::* const styleSlots[] = { &Constants::warm, &Constants::punchy, &Constants::clean };
            constexpr int clayFieldCount = 20 + 6 + 3 * 2;

            // 每个 clay* 字段各给一个与出厂值、结构体默认值都不同的值（1000 / 500 起步）。
            const auto distinct = [&] (Constants snapshot)
            {
                auto value = 1000.5f;

                for (auto field : floatFields)
                {
                    snapshot.*field = value;
                    value += 1.25f;
                }

                auto seconds = 500.125;

                for (auto field : doubleFields)
                {
                    snapshot.*field = seconds;
                    seconds += 0.375;
                }

                for (auto slot : styleSlots)
                    for (auto field : styleFields)
                    {
                        (snapshot.*slot).*field = value;
                        value += 1.25f;
                    }

                return snapshot;
            };

            const auto sameClayFields = [&] (const Constants& a, const Constants& b)
            {
                auto same = true;

                for (auto field : floatFields)
                    same = same && sameBits (a.*field, b.*field);

                for (auto field : doubleFields)
                    same = same && sameBitsDouble (a.*field, b.*field);

                for (auto slot : styleSlots)
                    for (auto field : styleFields)
                        same = same && sameBits ((a.*slot).*field, (b.*slot).*field);

                return same;
            };

            // 写：32 个 clay* 属性都在（风格字段带 warm_ / punchy_ / clean_ 前缀）；读：走「文本 → 解析 → 读取」，逐项按位读回
            // （调用方初值是结构体默认值，与 original 的每个 clay* 字段都不同）。
            const auto original = distinct (factory);
            const auto parsedXml = juce::parseXML (toXml (original)->toString());
            expect (parsedXml != nullptr);

            if (parsedXml == nullptr)
                return;

            int clayAttributes = 0;

            for (int i = 0; i < parsedXml->getNumAttributes(); ++i)
                clayAttributes += parsedXml->getAttributeName (i).contains ("clay") ? 1 : 0;

            expectEquals (clayAttributes, clayFieldCount, utf8 ("全部 clay* 字段都应写进快照"));

            Constants back;
            expect (fromXml (*parsedXml, back), utf8 ("含 clay* 字段的快照应能读入"));
            expect (sameClayFields (back, original) && bitwiseEqual (back, original), utf8 ("每个 clay* 字段都应逐位读回"));

            // 特殊位模式（NaN、−0、非规格化数、±Inf）同样逐位往返。
            auto odd = original;
            odd.clayMonoBandCorrelation = std::numeric_limits<float>::quiet_NaN();
            odd.clayMinCorrelationAfterWidth = -0.0f;
            odd.clayLowCutMinK = 1.0e-40f;
            odd.clean.clayWidthStyleFactor = std::numeric_limits<float>::infinity();
            odd.clayGuardPhaseHoldSeconds = -0.0;
            odd.clayGuardWidthReleaseSeconds = std::numeric_limits<double>::quiet_NaN();
            odd.clayGuardPhaseAttackSeconds = -std::numeric_limits<double>::infinity();
            odd.clayCostZeroDb = -0.0f;
            odd.clayMonoLossHighDb = std::numeric_limits<float>::quiet_NaN();
            odd.clayGuardPhaseMinSeconds = std::numeric_limits<double>::denorm_min();
            Constants oddBack;
            expect (fromXml (*toXml (odd), oddBack) && sameClayFields (oddBack, odd) && bitwiseEqual (oddBack, odd), utf8 ("特殊位模式应逐位往返"));

            // 差 1 ulp 也判为不同（float、double、风格字段各一例，另加 ADR-012 的两个新字段）：bitwiseEqual 覆盖了新字段。
            auto oneUlp = factory;
            oneUlp.clayMonoMaxHz = std::nextafter (oneUlp.clayMonoMaxHz, 1000.0f);
            expect (! bitwiseEqual (oneUlp, factory));
            oneUlp = factory;
            oneUlp.clayGuardPhaseHoldSeconds = std::nextafter (oneUlp.clayGuardPhaseHoldSeconds, 10.0);
            expect (! bitwiseEqual (oneUlp, factory));
            oneUlp = factory;
            oneUlp.punchy.clayLowCutTargetHz = std::nextafter (oneUlp.punchy.clayLowCutTargetHz, 0.0f);
            expect (! bitwiseEqual (oneUlp, factory));
            oneUlp = factory;
            oneUlp.clayCostFullDb = std::nextafter (oneUlp.clayCostFullDb, 1.0f);
            expect (! bitwiseEqual (oneUlp, factory));
            oneUlp = factory;
            oneUlp.clayGuardPhaseMinSeconds = std::nextafter (oneUlp.clayGuardPhaseMinSeconds, 1.0);
            expect (! bitwiseEqual (oneUlp, factory));

            // ADR-012 删掉的四个常数不再写；开发期快照里残留的这几个属性读时忽略（与没有它们的快照读出来逐位相同）。
            const char* const removedFields[] = { "clayMonoSideRatioLowDb", "clayMonoSideRatioHighDb", "clayMonoCorrelationLow",
                                                  "clayMonoCorrelationHigh" };
            const auto factoryXml = toXml (factory);

            for (const auto* removed : removedFields)
                expect (! factoryXml->hasAttribute (removed), utf8 ("删掉的常数不再写进快照：") + removed);

            auto strayXml = toXml (factory);

            for (const auto* removed : removedFields)
                strayXml->setAttribute (removed, "c1a00000");   // −20 的 IEEE 位：开发期快照里就是这样的值

            Constants stray;
            expect (fromXml (*strayXml, stray) && bitwiseEqual (stray, factory), utf8 ("残留的旧常数属性读时忽略"));

            // ---- 旧快照：去掉全部 clay* 属性（v0.1.6.2 及以前存的快照就是这样）----
            auto legacyXml = toXml (factory);
            juce::StringArray clayNames;

            for (int i = 0; i < legacyXml->getNumAttributes(); ++i)
                if (legacyXml->getAttributeName (i).contains ("clay"))
                    clayNames.add (legacyXml->getAttributeName (i));

            for (const auto& attribute : clayNames)
                legacyXml->removeAttribute (attribute);

            expectEquals (clayNames.size(), clayFieldCount);

            // 调用方初值每个 clay* 字段都非零：读回必须全是 +0（不继承调用方初值），其余字段与出厂快照逐位相同；格式版本不升。
            auto legacy = distinct (factory);
            expect (fromXml (*legacyXml, legacy), utf8 ("缺 clay* 字段的旧快照应能读入"));

            auto zeroed = factory;

            for (auto field : floatFields)
                zeroed.*field = 0.0f;

            for (auto field : doubleFields)
                zeroed.*field = 0.0;

            for (auto slot : styleSlots)
                for (auto field : styleFields)
                    (zeroed.*slot).*field = 0.0f;

            expect (sameClayFields (legacy, zeroed), utf8 ("缺字段的 clay* 常数应按 0 读"));
            expect (bitwiseEqual (legacy, zeroed), utf8 ("其余字段应与出厂快照逐位相同"));
            expectEquals (constantsFormatVersion, 1, utf8 ("加 clay* 字段不升快照格式版本"));

            Constants legacyRoundTrip;
            expect (fromXml (*toXml (legacy), legacyRoundTrip) && bitwiseEqual (legacyRoundTrip, legacy), utf8 ("旧快照读入后再存再读逐位稳定"));

            // 旧快照 + 带全部 Clay 特征的新画像：GLAZE 100 各风格三项全中性（旧工程按保存时的快照出方案，Clay 自动值为 0）。
            // 阳性对照：出厂快照、同一份画像三项都动。
            const auto profile = clayProfile();

            for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
            {
                expect (isNeutral (clayPlanWith (&profile, style, 100.0f, legacy).clay), utf8 ("旧快照的 Clay 自动值应为 0"));
                const auto fresh = clayPlanWith (&profile, style, 100.0f, factory).clay;
                expect (fresh.lowCutHz > 0.0f && fresh.widthDb > 0.0f, utf8 ("阳性对照：出厂快照会修坯（低切、加宽；收单自动态不做）"));
            }

            // v0.1.7.0 开发期的快照：有 clay* 却没有 ADR-012 的 6 个字段 → 那 6 个按 0 读（代价两端 0 / 0 判断不了、收单损失两端
            // 0 / 0 不需要）→ 三项都中性、不置代价位（不会按旧口径去低切），其余 clay* 照读。
            auto devXml = toXml (factory);
            const char* const adr012Fields[] = { "clayCostFullDb", "clayCostZeroDb", "clayMonoLossLowDb", "clayMonoLossHighDb",
                                                 "clayGuardPhaseFastGate", "clayGuardPhaseMinSeconds" };

            for (const auto* field : adr012Fields)
                devXml->removeAttribute (field);

            auto dev = distinct (factory);
            expect (fromXml (*devXml, dev), utf8 ("缺 ADR-012 字段的开发期快照应能读入"));
            expect (dev.clayCostFullDb == 0.0f && dev.clayCostZeroDb == 0.0f && dev.clayMonoLossLowDb == 0.0f && dev.clayMonoLossHighDb == 0.0f
                        && dev.clayGuardPhaseFastGate == 0.0f && dev.clayGuardPhaseMinSeconds == 0.0,
                    utf8 ("缺的 ADR-012 字段按 0 读"));
            expect (sameBits (dev.clayGlazeFullAt, factory.clayGlazeFullAt) && sameBits (dev.clean.clayLowCutTargetHz, factory.clean.clayLowCutTargetHz),
                    utf8 ("其余 clay* 照读"));

            for (auto style : { Style::automatic, Style::warm, Style::punchy, Style::clean })
                expect (isNeutral (clayPlanWith (&profile, style, 100.0f, dev).clay), utf8 ("开发期快照：代价判断不了，三项中性、不置位"));

            // 存在但损坏的 clay* 字段不是旧快照：整份拒收、out 不变（float、double、风格字段、ADR-012 新字段各一例）。
            const auto good = toXml (factory);
            const auto sentinel = Constants {};
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("clayGlazeFullAt", "zz"); }, sentinel,
                                       "malformed clayGlazeFullAt");
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("clayGuardPhaseHoldSeconds", "4000000000"); }, sentinel,
                                       "clayGuardPhaseHoldSeconds with 10 hex digits instead of 16");
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("clean_clayWidthStyleFactor", "1.0"); }, sentinel,
                                       "non-hex clean_clayWidthStyleFactor");
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("clayCostZeroDb", "0.6"); }, sentinel,
                                       "non-hex clayCostZeroDb");
            expectRejected<Constants> (*good, [] (juce::XmlElement& x) { x.setAttribute ("clayGuardPhaseMinSeconds", "3e19999a"); }, sentinel,
                                       "clayGuardPhaseMinSeconds with 8 hex digits instead of 16");
        }

        //==============================================================================
        void testClayCostStorage()
        {
            beginTest ("Peak costs and costLimitedMask are saved, hashed and read back exactly");

            // 依据：设计规格-Clay-根治修订 3.2「代价随画像保存」、3.3 ClayPlan::costLimitedMask（经 Status 给界面）；GlazeProfile.h
            //       （格式 2 字段严格、格式 1 读入后清成「不知道」、computeProfileId 只在格式 ≥ 2 时哈希 Clay 字段且接在最后——格式 1 的
            //       画像号与 v0.1.6.2 逐位相同；方案 / 执行快照缺属性 = 中性，属性在但损坏照样拒收）。

            // ---- 1. 画像：代价的四个子字段都写、逐位读回 ----
            auto profile = clayProfile();
            setFeature (profile.lowCutPeakCostDb, 0.3125f, 0.75f);
            profile.lowCutPeakCostDb.effectiveSeconds = 12.5;

            for (size_t i = 0; i < profile.monoPeakCostDb.size(); ++i)
                setFeature (profile.monoPeakCostDb[i], -0.25f + 0.375f * (float) i, 0.5f + 0.125f * (float) i);

            profile.monoPeakCostDb[2].valid = false;
            setFeature (profile.widthPeakCostDb, -0.0f, 1.0f);

            const auto profileXml = toXml (profile);

            for (const auto* attribute : { "lowCutPeakCostDb_value", "lowCutPeakCostDb_valid", "lowCutPeakCostDb_seconds",
                                           "lowCutPeakCostDb_confidence", "monoPeakCostDb0_value", "monoPeakCostDb3_confidence",
                                           "widthPeakCostDb_valid", "widthPeakCostDb_seconds" })
                expect (profileXml->hasAttribute (attribute), utf8 ("格式 2 画像应写代价属性：") + attribute);

            {
                const auto parsed = juce::parseXML (profileXml->toString());
                MusicProfile back = makeProfile (-9.0f, -1.0f);   // 初值没有代价（全无效）：读回的必须来自 XML
                expect (parsed != nullptr && fromXml (*parsed, back), utf8 ("带代价的格式 2 画像应能读入"));
                expect (bitwiseEqual (back, profile), utf8 ("代价往返逐位相同"));
                expect (sameBits (back.lowCutPeakCostDb.value, 0.3125f) && back.lowCutPeakCostDb.effectiveSeconds == 12.5
                            && ! back.monoPeakCostDb[2].valid && std::signbit (back.widthPeakCostDb.value)
                            && sameBits (back.monoPeakCostDb[3].confidence, 0.875f),
                        utf8 ("逐项核对几个代价子字段"));
            }

            // 格式 2 缺任一代价属性 → 整份拒收、out 不变（与格式 2 其他字段同样严格）。
            const auto profileSentinel = makeProfile (-11.0f, -2.0f);

            for (const auto* attribute : { "lowCutPeakCostDb_value", "monoPeakCostDb1_valid", "monoPeakCostDb3_seconds", "widthPeakCostDb_confidence" })
            {
                const juce::String missing (attribute);
                expectRejected<MusicProfile> (*profileXml, [missing] (juce::XmlElement& x) { x.removeAttribute (missing); }, profileSentinel,
                                              utf8 ("格式 2 缺代价属性 ") + missing);
            }

            // ---- 2. 格式 1：不写代价；读入后代价全清成「不知道」（调用方初值带着有效代价也清掉）----
            auto legacyProfile = profile;
            legacyProfile.formatVersion = 1;
            const auto legacyXml = toXml (legacyProfile);
            expect (! legacyXml->hasAttribute ("lowCutPeakCostDb_value") && ! legacyXml->hasAttribute ("monoPeakCostDb0_value")
                        && ! legacyXml->hasAttribute ("widthPeakCostDb_value"),
                    utf8 ("格式 1 画像不写代价"));

            {
                auto readBack = profile;   // 初值的代价都有效
                expect (fromXml (*legacyXml, readBack) && readBack.formatVersion == 1, utf8 ("格式 1 画像应能读入"));
                auto untried = true;

                for (const auto* f : { &readBack.lowCutPeakCostDb, &readBack.monoPeakCostDb[0], &readBack.monoPeakCostDb[1],
                                       &readBack.monoPeakCostDb[2], &readBack.monoPeakCostDb[3], &readBack.widthPeakCostDb })
                    untried = untried && ! f->valid && f->value == 0.0f && f->confidence == 0.0f && f->effectiveSeconds == 0.0;

                expect (untried, utf8 ("格式 1：代价全是「不知道」"));
            }

            // ---- 3. 画像号：格式 2 时代价的值 / 有效标志 / 时长 / 置信度、任一收单候选变了都变；格式 1 时 Clay 与代价字段怎么变都不变 ----
            {
                const auto id = computeProfileId (profile);
                int unchanged = 0;

                const std::function<void (MusicProfile&)> changes[] =
                {
                    [] (MusicProfile& m) { m.lowCutPeakCostDb.value += 0.02f; },
                    [] (MusicProfile& m) { m.lowCutPeakCostDb.valid = ! m.lowCutPeakCostDb.valid; },
                    [] (MusicProfile& m) { m.lowCutPeakCostDb.effectiveSeconds += 0.5; },
                    [] (MusicProfile& m) { m.lowCutPeakCostDb.confidence += 0.01f; },
                    [] (MusicProfile& m) { m.monoPeakCostDb[0].value += 0.02f; },
                    [] (MusicProfile& m) { m.monoPeakCostDb[1].value += 0.02f; },
                    [] (MusicProfile& m) { m.monoPeakCostDb[2].valid = true; },
                    [] (MusicProfile& m) { m.monoPeakCostDb[3].value += 0.02f; },
                    [] (MusicProfile& m) { m.widthPeakCostDb.value = 0.5f; },
                    [] (MusicProfile& m) { m.widthPeakCostDb.valid = false; }
                };

                for (const auto& change : changes)
                {
                    auto changed = profile;
                    change (changed);
                    unchanged += computeProfileId (changed) == id ? 1 : 0;
                }

                expectEquals (unchanged, 0, utf8 ("格式 2：任一代价子字段变了画像号都应变"));

                auto binned = profile;
                binned.monoPeakCostDb[1].value = 0.1234f;
                auto tiny = binned;
                tiny.monoPeakCostDb[1].value = 0.1244f;   // 量化步长 0.01 dB 以内（12.34 与 12.44 都量化成 12）
                expect (computeProfileId (tiny) == computeProfileId (binned), utf8 ("代价的量化步长以内不改画像号"));

                const auto legacyId = computeProfileId (legacyProfile);
                auto legacyChanged = legacyProfile;

                for (const auto& change : changes)
                    change (legacyChanged);

                legacyChanged.subShareDb.value += 5.0f;
                legacyChanged.lowCorrelation.valid = ! legacyChanged.lowCorrelation.valid;
                legacyChanged.bandStereoConfidence[4] = 0.5f;
                expect (computeProfileId (legacyChanged) == legacyId, utf8 ("格式 1：Clay 与代价字段不进画像号（与 v0.1.6.2 相同）"));
                expect (legacyId != id, utf8 ("阳性对照：格式号本身在哈希里"));
            }

            // ---- 4. 方案的 costLimitedMask：已提交版本往返逐位；缺属性 → 0；属性在但不是 0 ～ 255 的整数 → 拒收 ----
            auto limitedProfile = clayProfile();
            setClayCosts (limitedProfile, 0.4f, 0.4f, 0.4f);   // 三项都减量：位 = 7
            const auto limitedPlan = planFor (&limitedProfile, Style::clean, 100.0f);
            expectEquals (maskOf (limitedPlan.clay), autoCostBits, utf8 ("阳性对照：这份方案低切、加宽都有代价位"));

            CommittedState state;
            state.hasProfile = true;
            state.profile = limitedProfile;
            state.profileId = computeProfileId (limitedProfile);
            state.plan = limitedPlan;
            state.constants = factoryConstants();
            const auto stateXml = toXml (state);
            expect (stateXml->getChildByName ("PLAN") != nullptr && stateXml->getChildByName ("PLAN")->getStringAttribute ("clayCostLimitedMask") == "5",
                    utf8 ("方案写十进制的代价位（低切 1 + 加宽 4）"));

            {
                CommittedState back;
                const auto parsed = juce::parseXML (stateXml->toString());
                expect (parsed != nullptr && fromXml (*parsed, back) && bitwiseEqual (back, state), utf8 ("已提交版本连代价位往返逐位"));
                expectEquals (maskOf (back.plan.clay), autoCostBits);
            }

            {
                auto withoutMask = std::make_unique<juce::XmlElement> (*stateXml);
                withoutMask->getChildByName ("PLAN")->removeAttribute ("clayCostLimitedMask");
                CommittedState back;
                back.plan.clay.costLimitedMask = 5;   // 初值非 0：缺属性时必须读成 0，不能继承
                expect (fromXml (*withoutMask, back) && back.plan.clay.costLimitedMask == 0, utf8 ("ADR-012 之前存的方案：代价位读成 0"));
                expect (sameBits (back.plan.clay.lowCutHz, state.plan.clay.lowCutHz), utf8 ("其余 Clay 值照读"));
            }

            CommittedState stateSentinel;
            stateSentinel.hasProfile = true;
            stateSentinel.profile = profileSentinel;
            stateSentinel.constants.version = 42;

            for (const auto* bad : { "x", "256", "-1", "3.5", "" })
            {
                const juce::String value (bad);
                expectRejected<CommittedState> (*stateXml, [value] (juce::XmlElement& x) { x.getChildByName ("PLAN")->setAttribute ("clayCostLimitedMask", value); },
                                                stateSentinel, utf8 ("方案代价位损坏：「") + value + utf8 ("」"));
            }

            // ---- 5. 执行快照：plan.clay 与 liveClay 的代价位往返；缺属性 → 0；损坏或含未定义的位 → 整份拒收 ----
            ExecutionState execution;
            execution.valid = true;
            execution.hasProfile = true;
            execution.profile = limitedProfile;
            execution.constants = factoryConstants();
            execution.plan = limitedPlan;
            execution.reference.glazePercent = 100.0f;
            execution.reference.sampleRate = 48000.0;
            execution.liveClay = limitedPlan.clay;
            execution.liveClay.costLimitedMask = static_cast<std::uint8_t> (clayCostLimitedMono | clayCostLimitedWidth);
            const auto executionXml = toXml (execution);

            {
                ExecutionState back;
                const auto parsed = juce::parseXML (executionXml->toString());
                expect (parsed != nullptr && fromXml (*parsed, back), utf8 ("带代价位的执行快照应能读入"));
                expectEquals (maskOf (back.liveClay), clayCostLimitedMono | clayCostLimitedWidth, utf8 ("执行快照的 liveClay 代价位"));
                expectEquals (maskOf (back.plan.clay), autoCostBits, utf8 ("执行快照的方案代价位"));
                expect (sameClay (back.liveClay, execution.liveClay) && sameClay (back.plan.clay, execution.plan.clay), utf8 ("Clay 值逐位往返"));
            }

            {
                auto withoutMask = std::make_unique<juce::XmlElement> (*executionXml);
                withoutMask->removeAttribute ("liveClayCostLimitedMask");
                withoutMask->getChildByName ("PLAN")->removeAttribute ("clayCostLimitedMask");
                ExecutionState back;
                expect (fromXml (*withoutMask, back) && back.liveClay.costLimitedMask == 0 && back.plan.clay.costLimitedMask == 0,
                        utf8 ("ADR-012 之前存的执行快照：两处代价位都读成 0"));
            }

            const auto rejectsExecution = [&executionXml] (const std::function<void (juce::XmlElement&)>& damage)
            {
                auto broken = std::make_unique<juce::XmlElement> (*executionXml);
                damage (*broken);
                ExecutionState back;
                back.gainBiasDb = 1.25f;   // 读取失败时 out 不变
                return ! fromXml (*broken, back) && back.gainBiasDb == 1.25f;
            };

            expect (rejectsExecution ([] (juce::XmlElement& x) { x.setAttribute ("liveClayCostLimitedMask", "y"); }), utf8 ("liveClay 代价位不是整数：拒收"));
            expect (rejectsExecution ([] (juce::XmlElement& x) { x.setAttribute ("liveClayCostLimitedMask", "8"); }), utf8 ("liveClay 代价位含未定义的位：拒收"));
            expect (rejectsExecution ([] (juce::XmlElement& x) { x.getChildByName ("PLAN")->setAttribute ("clayCostLimitedMask", "9"); }),
                    utf8 ("执行快照方案的代价位含未定义的位：拒收"));
            expect (! rejectsExecution ([] (juce::XmlElement& x) { x.setAttribute ("liveClayCostLimitedMask", "4"); }),
                    utf8 ("阳性对照：合法的代价位照读"));
        }

        //==============================================================================
        void testClayDeterminismAndNoAllocation()
        {
            beginTest ("Clay planning is bit-exact and allocation-free");

            // 依据：第 6 节第 4 条「纯函数（同输入逐位同输出）」；GlazePlanner.h「无状态、不分配」（编排器可能每块调 makePlan）。
            juce::Random random (0x5eedc1aa);
            std::vector<Scenario> scenarios;

            for (int i = 0; i < 30; ++i)
            {
                auto scenario = randomScenario (random);
                addRandomClayFeatures (scenario.profile, random);
                scenarios.push_back (scenario);
            }

            int mismatches = 0, active = 0;

            for (const auto& s : scenarios)
                for (auto glaze : { 0.0f, 7.5f, 50.0f, 99.9f, 100.0f })
                {
                    const auto first = planFor (s, glaze);
                    const auto copy = s;   // 不同地址的画像拷贝
                    const auto second = planFor (copy, glaze);
                    mismatches += sameClay (first.clay, second.clay) && samePlan (first, second) ? 0 : 1;
                    active += isNeutral (first.clay) ? 0 : 1;
                }

            expectEquals (mismatches, 0, utf8 ("同一输入两次的方案（含 Clay）应逐位相同"));
            expect (active > 0, utf8 ("阳性对照：这些场景里有 Clay 动作"));

            const auto& c = factoryConstants();
            float sink = 0.0f;
            const auto before = allocationCountOnThisThread();

            for (const auto& s : scenarios)
                for (auto glaze : { 0.0f, 33.0f, 100.0f })
                {
                    PlanInputs in;
                    in.profile = s.hasProfile ? &s.profile : nullptr;
                    in.style = s.style;
                    in.glazePercent = glaze;
                    in.manual = s.manual;
                    in.maxGainDb = s.maxGainDb;
                    in.constants = &c;
                    const auto plan = makePlan (in);
                    sink += plan.clay.lowCutHz + plan.clay.monoHz * plan.clay.monoAmount + plan.clay.widthDb
                          + static_cast<float> (plan.clay.costLimitedMask);
                }

            // 先把计数读进局部量再断言：函数实参的求值顺序不确定（GCC 从右往左），提示文字的 juce::String 会先构造、被算进计数。

            const auto allocationsInPath = allocationCountOnThisThread() - before;

            expectEquals ((int) (allocationsInPath), 0, utf8 ("带 Clay 特征的决策路径不应分配内存"));
            expect (std::isfinite (sink));
        }
    };

    static GlazePlannerTests glazePlannerTests;
}
