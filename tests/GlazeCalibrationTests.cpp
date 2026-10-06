/*
  ==============================================================================

    GlazeCalibrationTests.cpp
    T-048 阶段 2 删掉整份校准器用例（GlazeCalibrator 随聆听 / 锁定状态机删除）。
    本文件留下与校准器无关、生产类还在的两部分：
      - Color / Limit 每块统计（BlockStats）；
      - 保护层 GlazeGuard。退让不再读 Constants 里的 1 / 0.3 dB/s，改走 T-047：
        超限 0→3 dB 映射 0.3→6 dB/s，退让深度 0→下限映射回程 0.15→0.3 dB/s，地板仍是 −3 dB。
        触发仍是「3 s 窗内累计超出 ≥ 1 s」，恢复仍要连续 5 s 安全音乐。
    process 返回 GuardOutput（可隐式当成退让 dB）。PSR 单列在 tests/Phase2OrchestratorTests.cpp。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ColorModule.h"
#include "../Source/dsp/GlazeGuard.h"
#include "../Source/dsp/LimiterModule.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::GlazeGuard;
        using glaze::ModuleStats;

        constexpr double testRate = 48000.0;
        constexpr int statsBlock = 512;

        juce::dsp::ProcessSpec statsSpec (int numChannels = 2)
        {
            return { testRate, (juce::uint32) statsBlock, (juce::uint32) numChannels };
        }

        // 把 signal 的 [start, start + length) 拷进 block（block 先按 length 调整大小）。
        void loadBlock (juce::AudioBuffer<float>& block, const Signal& signal, int start, int length)
        {
            block.setSize ((int) signal.size(), length, false, false, true);

            for (int ch = 0; ch < (int) signal.size(); ++ch)
                block.copyFrom (ch, 0, signal[(size_t) ch].data() + start, length);
        }

        double db (double linear)   { return 20.0 * std::log10 (linear); }

        //==============================================================================
        // 保护层的输入脚本：按「音乐时间」分段，每段一种统计；块在段边界处切开，块长由 nextBlock 给。
        struct GuardSegment
        {
            int samples = 0;
            ModuleStats stats;
        };

        ModuleStats musicStats()
        {
            ModuleStats s;
            s.musicActive = true;
            return s;
        }

        ModuleStats limitMeanStats (float grDb)
        {
            auto s = musicStats();
            s.limitGrMeanDb = grDb;
            s.limitGrMaxDb = grDb;
            return s;
        }

        ModuleStats limitPeakStats (float peakDb)
        {
            auto s = musicStats();
            s.limitGrMaxDb = peakDb;
            return s;
        }

        glaze::StyleConstants noCaps()
        {
            glaze::StyleConstants c;
            c.glueGrMeanCapDb = c.glueGrP95CapDb = 0.0f;
            c.clipDepthCapDb = c.clipActivityCap = 0.0f;
            c.limitGrMeanCapDb = c.limitGrP95CapDb = c.limitGrPeakCapDb = 0.0f;
            return c;
        }

        glaze::StyleConstants limitMeanCap (float capDb)
        {
            auto c = noCaps();
            c.limitGrMeanCapDb = capDb;
            return c;
        }

        glaze::StyleConstants limitPeakCap (float capDb)
        {
            auto c = noCaps();
            c.limitGrPeakCapDb = capDb;
            return c;
        }

        // 时长与地板仍读 Constants（−3 dB、触发 1 s、窗 3 s、恢复等待 5 s）。
        // 速率不读 guardAttack / guardRelease：GlazeGuard 内部按 T-047 变速率。
        glaze::Constants guardConstants()
        {
            glaze::Constants c;
            c.guardMinDb = -3.0f;
            c.guardAttackDbPerSecond = 1.0f;
            c.guardReleaseDbPerSecond = 0.3f;
            c.guardTriggerSeconds = 1.0;
            c.guardWindowSeconds = 3.0;
            c.guardRecoverAfterSeconds = 5.0;
            return c;
        }

        struct GuardTrace
        {
            std::vector<std::int64_t> musicEnd;   // 每块结束时累计的音乐样本数
            std::vector<float> protection;        // 每块返回的 g_保护
        };

        // silenceEvery > 0：每隔这么多块插一个同样长的静音块（musicActive = false，不推进音乐时间）。
        GuardTrace runGuard (GlazeGuard& guard, const std::vector<GuardSegment>& script, const std::function<int()>& nextBlock,
                             int silenceEvery = 0)
        {
            GuardTrace trace;
            std::int64_t music = 0;
            int blockCounter = 0;

            for (const auto& segment : script)
            {
                for (int done = 0; done < segment.samples;)
                {
                    const auto length = juce::jmin (segment.samples - done, juce::jmax (1, nextBlock()));

                    if (silenceEvery > 0 && ++blockCounter % silenceEvery == 0)
                    {
                        ModuleStats silent = segment.stats;
                        silent.musicActive = false;
                        silent.numSamples = length;
                        const auto before = guard.getProtectionDb();
                        const auto during = guard.process (silent);

                        if (during != before)   // 静音块不许改 g_保护（记成一个不可能的值让调用方的比较失败）
                        {
                            trace.musicEnd.push_back (-1);
                            trace.protection.push_back (std::numeric_limits<float>::quiet_NaN());
                        }
                    }

                    auto stats = segment.stats;
                    stats.numSamples = length;
                    const auto g = guard.process (stats);

                    done += length;
                    music += length;
                    trace.musicEnd.push_back (music);
                    trace.protection.push_back (g);
                }
            }

            return trace;
        }

        // 同一脚本按 1 个时隙（10 ms = 480 样本）一块跑出的参考轨迹：第 k 个时隙结束后的 g_保护。
        std::vector<float> slotReference (const std::vector<GuardSegment>& script)
        {
            GlazeGuard guard;
            guard.prepare (testRate);
            guard.setLimits (limitMeanCap (3.0f), guardConstants());
            auto trace = runGuard (guard, script, [] { return 480; });
            return trace.protection;
        }

    }

    class GlazeCalibrationTests  : public juce::UnitTest
    {
    public:
        GlazeCalibrationTests() : juce::UnitTest ("GlazeCalibration", "Celadon") {}

        void runTest() override
        {
            testLimiterStatsKnownSine();
            testLimiterStatsQuietAndIdle();
            testLimiterClipStats();
            testStatsCoverWholeProcessCall();
            testColorGlueStats();
            testColorHeatDrive();
            testModuleStatsDoNotAllocate();

            testGuardRetreatTiming();
            testGuardBlockSizeIndependent();
            testGuardActualTransientStatistics();
            testGuardStopsRetreatWhenSatisfied();
            testGuardRecoveryAndSilence();
            testGuardRecoveryStopsNearLimit();
            testGuardPlanLimited();
            testGuardPeakNeedsPersistence();
            testGuardP95AndClipConstraints();
            testGuardInactiveWithoutLimits();
            testGuardDoesNotAllocate();

        }

    private:
        void testGuardActualTransientStatistics()
        {
            beginTest ("actual limiter transient statistics retain identical Guard slots across host callback sizes");
            constexpr double sr = 48000.0;
            constexpr int total = 48000 * 9;
            const auto render = [&] (int hostBlock)
            {
                LimiterModule limiter;
                LimiterModule::Params params;
                params.gainDb = 12.0f;
                params.autoRelease = false;
                params.releaseMs = 10.0f;
                limiter.setParams (params);
                limiter.prepare ({ sr, 2048, 2 });
                GlazeGuard guard;
                guard.prepare (sr);
                auto caps = noCaps();
                caps.limitGrP95CapDb = 0.6f;
                guard.setLimits (caps, guardConstants());
                std::vector<float> trajectory;
                juce::AudioBuffer<float> buffer (2, 2048);
                for (int hostStart = 0; hostStart < total; hostStart += hostBlock)
                {
                    const auto hostEnd = juce::jmin (total, hostStart + hostBlock);
                    for (int position = hostStart; position < hostEnd;)
                    {
                        const auto boundary = guard.getSamplesUntilSlotBoundary();
                        const auto n = juce::jmin (hostEnd - position, boundary);
                        buffer.setSize (2, n, false, false, true);
                        for (int i = 0; i < n; ++i)
                        {
                            const auto sample = position + i;
                            const auto phase = sample % 6000;
                            const auto x = 0.012f * (float) std::sin (juce::MathConstants<double>::twoPi * 997.0 * sample / sr)
                                         + (sample < 5 * 48000 && phase >= 477 && phase < 485 ? 0.9f : 0.0f);
                            buffer.setSample (0, i, x);
                            buffer.setSample (1, i, x);
                        }
                        limiter.process (buffer);
                        const auto actual = limiter.getBlockStats();
                        auto stats = musicStats();
                        stats.numSamples = n;
                        stats.limitGrMeanDb = actual.grMeanDb;
                        stats.limitGrMaxDb = actual.grMaxDb;
                        guard.process (stats);
                        if (n == boundary) trajectory.push_back (guard.getProtectionDb());
                        position += n;
                    }
                }
                return trajectory;
            };
            const auto reference = render (32);
            expect (*std::min_element (reference.begin(), reference.end()) < -0.1f,
                    "fixture must exercise protective retreat from actual short transients");
            for (const auto block : { 333, 512, 2048 })
            {
                const auto actual = render (block);
                expectEquals ((int) actual.size(), (int) reference.size());
                float error = 0.0f;
                for (size_t i = 0; i < juce::jmin (actual.size(), reference.size()); ++i)
                    error = juce::jmax (error, std::abs (actual[i] - reference[i]));
                logMessage ("  real transient Guard trace block=" + juce::String (block) + ", max error=" + juce::String (error, 8));
                expect (error <= 1.0e-5f, "callback partition cannot smear the actual limiter GR across slots");
            }
        }

        //==============================================================================
        // 1. Limit：0 dBFS 1 kHz 正弦、Ceiling −1 dBTP、Gain 0：稳态每块 GR 含新增的 0.35 dB 重建保护余量，
        //    均值 = 最大值 = 期望值（±0.01 dB）；grMaxDb 与同一块的 GR 表读数逐位相同。
        // 依据：设计规格-GLAZE-分析器 §1「模块内部」测点；LimiterModule.h BlockStats。
        void testLimiterStatsKnownSine()
        {
            beginTest ("limit stats: sine over the ceiling reads the expected GR, max equals the meter");

            LimiterModule::Params p;
            p.gainDb = 0.0f;
            p.ceilingDb = -1.0f;

            LimiterModule limiter;
            limiter.setParams (p);
            limiter.prepare (statsSpec());
            limiter.setParams (p);

            const auto input = makeSine (2, testRate, 1000.0, 1.0f, 2.0);
            const auto truePeak = (double) steadyTruePeakRange (input).second;
            const auto expected = db (truePeak) - (double) p.ceilingDb + 0.35;

            juce::AudioBuffer<float> block;
            int checked = 0, meterMismatch = 0;
            double worstMean = 0.0, worstMax = 0.0;

            for (int start = 0; start + statsBlock <= (int) input.front().size(); start += statsBlock)
            {
                loadBlock (block, input, start, statsBlock);
                limiter.process (block);

                const auto& stats = limiter.getBlockStats();
                const auto meter = limiter.getAndResetGainReductionDb();

                if (stats.grMaxDb != meter)
                    ++meterMismatch;

                if (start >= (int) (0.5 * testRate))   // 前 0.5 s 让包络走到稳态
                {
                    worstMean = juce::jmax (worstMean, std::abs ((double) stats.grMeanDb - expected));
                    worstMax  = juce::jmax (worstMax,  std::abs ((double) stats.grMaxDb - expected));
                    expect (stats.grMeanDb <= stats.grMaxDb + 1.0e-4f, "mean must not exceed max");
                    expectEquals (stats.clipActiveFraction, 0.0f);
                    expectEquals (stats.clipDepthMaxDb, 0.0f);
                    ++checked;
                }
            }

            logMessage ("  expected GR " + juce::String (expected, 4) + " dB; worst |mean - expected| "
                        + juce::String (worstMean, 5) + ", worst |max - expected| " + juce::String (worstMax, 5));

            expect (checked > 100);
            expect (expected > 1.25 && expected < 1.45, "a 0 dBFS sine needs about 1.35 dB of GR including reconstruction headroom");
            expectLessThan (worstMean, 0.01);
            expectLessThan (worstMax, 0.01);
            expectEquals (meterMismatch, 0);
        }

        // 2. 低于 Ceiling：GR 两项精确为 0；Clip 0（空闲跳过）与「关掉跳过、湿路淡出为 0」两种情况 Clip 统计都为 0；
        //    旁通时表不报数，统计照报内部限幅量。
        // 依据：LimiterModule.h BlockStats「空闲跳过的部分按没在工作计」「与 Bypass 无关」。
        void testLimiterStatsQuietAndIdle()
        {
            beginTest ("limit stats: zero when not limiting, clip stats zero when the clip stage is idle, bypass still reports");

            const auto quiet = makeSine (2, testRate, 1000.0, 0.5f, 0.5);

            for (auto skip : { true, false })
            {
                LimiterModule::Params p;
                LimiterModule limiter;
                limiter.setParams (p);
                limiter.prepare (statsSpec());
                limiter.setParams (p);
                limiter.setClipIdleSkipEnabledForTesting (skip);

                juce::AudioBuffer<float> block;
                bool allZero = true;

                for (int start = 0; start + statsBlock <= (int) quiet.front().size(); start += statsBlock)
                {
                    loadBlock (block, quiet, start, statsBlock);
                    limiter.process (block);
                    const auto& stats = limiter.getBlockStats();
                    allZero = allZero && stats.grMeanDb == 0.0f && stats.grMaxDb == 0.0f
                                      && stats.clipActiveFraction == 0.0f && stats.clipDepthMaxDb == 0.0f;
                }

                expect (allZero, skip ? "idle skip on" : "idle skip off");
            }

            // 旁通：内部照常限幅（统计 > 0），表不报数。
            {
                LimiterModule::Params p;
                p.gainDb = 6.0f;
                p.bypass = true;

                LimiterModule limiter;
                limiter.setParams (p);
                limiter.prepare (statsSpec());
                limiter.setParams (p);

                const auto loud = makeSine (2, testRate, 1000.0, 1.0f, 0.5);
                juce::AudioBuffer<float> block;
                float lastMean = 0.0f, meterMax = 0.0f;

                for (int start = 0; start + statsBlock <= (int) loud.front().size(); start += statsBlock)
                {
                    loadBlock (block, loud, start, statsBlock);
                    limiter.process (block);
                    lastMean = limiter.getBlockStats().grMeanDb;
                    meterMax = juce::jmax (meterMax, limiter.getAndResetGainReductionDb());
                }

                expectGreaterThan (lastMean, 6.0f);
                expectEquals (meterMax, 0.0f);
            }
        }

        // 3. Clip = 100（削波阈值 = Ceiling = −1 dBTP）、Gain 0、0 dBFS 1 kHz 正弦：4× 域 |x| > T 的占比
        //    = 1 − (2/π)·asin(T/A) ≈ 0.2995（±0.01），深度 = 20·log10(A/T) ≈ 1 dB（±0.05）；Clip 拧动的 20 ms（阈值在滑的循环）
        //    统计也有限、在 [0, 1]；拧回 0、空闲后两项回到 0。
        // 依据：LimiterModule.h BlockStats（削波阈值 T 的定义）；设计规格-GLAZE-决策与校准 §4「Clip：削波深度与活动占比」。
        void testLimiterClipStats()
        {
            beginTest ("limit stats: clip activity and depth match the sine geometry");

            LimiterModule::Params p;
            p.clip = 100.0f;
            p.ceilingDb = -1.0f;

            LimiterModule limiter;
            limiter.setParams (p);
            limiter.prepare (statsSpec());
            limiter.setParams (p);

            const auto input = makeSine (2, testRate, 1000.0, 1.0f, 1.5);
            const auto threshold = std::pow (10.0, (double) p.ceilingDb / 20.0);
            const auto expectedFraction = 1.0 - (2.0 / juce::MathConstants<double>::pi) * std::asin (threshold / 1.0);
            const auto expectedDepth = -db (threshold);

            juce::AudioBuffer<float> block;
            double worstFraction = 0.0, worstDepth = 0.0;
            int checked = 0;

            for (int start = 0; start + statsBlock <= (int) (0.6 * testRate); start += statsBlock)
            {
                loadBlock (block, input, start, statsBlock);
                limiter.process (block);

                if (start >= (int) (0.1 * testRate))
                {
                    const auto& stats = limiter.getBlockStats();
                    worstFraction = juce::jmax (worstFraction, std::abs ((double) stats.clipActiveFraction - expectedFraction));
                    worstDepth = juce::jmax (worstDepth, std::abs ((double) stats.clipDepthMaxDb - expectedDepth));
                    ++checked;
                }
            }

            logMessage ("  expected fraction " + juce::String (expectedFraction, 4) + ", depth " + juce::String (expectedDepth, 4)
                        + " dB; worst errors " + juce::String (worstFraction, 5) + " / " + juce::String (worstDepth, 5) + " dB");

            expect (checked > 20);
            expectLessThan (worstFraction, 0.01);
            expectLessThan (worstDepth, 0.05);

            // Clip 100 → 50：阈值滑动的 20 ms 走逐样本统计的循环。
            p.clip = 50.0f;
            limiter.setParams (p);
            bool sane = true;

            for (int start = (int) (0.6 * testRate); start + statsBlock <= (int) (0.9 * testRate); start += statsBlock)
            {
                loadBlock (block, input, start, statsBlock);
                limiter.process (block);
                const auto& stats = limiter.getBlockStats();
                sane = sane && std::isfinite (stats.clipActiveFraction) && stats.clipActiveFraction >= 0.0f
                            && stats.clipActiveFraction <= 1.0f && std::isfinite (stats.clipDepthMaxDb) && stats.clipDepthMaxDb >= 0.0f;
            }

            expect (sane, "stats stay finite and in range while the clip threshold glides");

            // Clip 拧回 0：淡出走完、空闲后两项为 0。
            p.clip = 0.0f;
            limiter.setParams (p);

            for (int start = (int) (0.9 * testRate); start + statsBlock <= (int) (1.5 * testRate); start += statsBlock)
            {
                loadBlock (block, input, start, statsBlock);
                limiter.process (block);
            }

            expectEquals (limiter.getBlockStats().clipActiveFraction, 0.0f);
            expectEquals (limiter.getBlockStats().clipDepthMaxDb, 0.0f);
        }

        // 4. 一次 process 的样本数是块上限的 3 倍（内部切 3 段）：统计覆盖整个调用——最大值 = 分三次调的最大值，
        //    均值 = 三次均值的平均（两个模块、Limit 与 Color 各项）。
        // 依据：LimiterModule.h / ColorModule.h BlockStats「统计覆盖整个调用」。
        void testStatsCoverWholeProcessCall()
        {
            beginTest ("block stats cover the whole process call when it is split into chunks");

            auto input = makeNoise (2, 3 * statsBlock * 40, 0.6f, 7);
            const auto sine = makeSine (2, testRate, 220.0, 0.6f, (double) input.front().size() / testRate);

            for (size_t ch = 0; ch < input.size(); ++ch)
                for (size_t i = 0; i < input[ch].size(); ++i)
                    input[ch][i] += sine[ch][i];

            LimiterModule::Params lp;
            lp.gainDb = 8.0f;
            lp.clip = 60.0f;

            ColorModule::Params cp;
            cp.glue = 80.0f;
            cp.heat = 50.0f;

            LimiterModule wholeLimiter, splitLimiter;
            ColorModule wholeColor, splitColor;

            for (auto* l : { &wholeLimiter, &splitLimiter })   { l->setParams (lp); l->prepare (statsSpec()); l->setParams (lp); }
            for (auto* c : { &wholeColor, &splitColor })       { c->setParams (cp); c->prepare (statsSpec()); c->setParams (cp); }

            juce::AudioBuffer<float> whole, part;
            double worst = 0.0;
            int mismatchedMax = 0;

            for (int start = 0; start + 3 * statsBlock <= (int) input.front().size(); start += 3 * statsBlock)
            {
                loadBlock (whole, input, start, 3 * statsBlock);
                auto wholeForColor = whole;
                wholeLimiter.process (whole);
                wholeColor.process (wholeForColor);

                double limitMean = 0.0, clipFraction = 0.0, glueMean = 0.0, heat = 0.0;
                float limitMax = 0.0f, clipDepth = 0.0f, glueMax = 0.0f;

                for (int k = 0; k < 3; ++k)
                {
                    loadBlock (part, input, start + k * statsBlock, statsBlock);
                    auto partForColor = part;
                    splitLimiter.process (part);
                    splitColor.process (partForColor);

                    const auto& ls = splitLimiter.getBlockStats();
                    const auto& cs = splitColor.getBlockStats();
                    limitMean += ls.grMeanDb / 3.0;   clipFraction += ls.clipActiveFraction / 3.0;
                    glueMean += cs.glueGrMeanDb / 3.0; heat += cs.heatDrive / 3.0;
                    limitMax = juce::jmax (limitMax, ls.grMaxDb);
                    clipDepth = juce::jmax (clipDepth, ls.clipDepthMaxDb);
                    glueMax = juce::jmax (glueMax, cs.glueGrMaxDb);
                }

                const auto& wl = wholeLimiter.getBlockStats();
                const auto& wc = wholeColor.getBlockStats();

                worst = juce::jmax (worst, std::abs (wl.grMeanDb - limitMean));
                worst = juce::jmax (worst, std::abs (wl.clipActiveFraction - clipFraction));
                worst = juce::jmax (worst, std::abs (wc.glueGrMeanDb - glueMean));
                worst = juce::jmax (worst, std::abs (wc.heatDrive - heat) / 100.0);

                if (wl.grMaxDb != limitMax || wl.clipDepthMaxDb != clipDepth || wc.glueGrMaxDb != glueMax)
                    ++mismatchedMax;
            }

            expectLessThan (worst, 1.0e-4);
            expectEquals (mismatchedMax, 0);
        }

        // 5. Glue = 100（阈值 −24 dBFS、2:1）、输入 −12 dBFS 的 6 kHz 方波（过 100 Hz 侧链高通后幅度基本不变）：稳态压缩量
        //    = (−12.04 + 24)·0.5 ≈ 5.98 dB，每块均值 ≈ 最大值 ≈ 期望（±0.15 dB：高通在 6 / 18 kHz 两个分量上的相移不同，
        //    逐样本侧链峰值有百分之几的起伏，攻击比释放快，稳态压缩量偏向起伏的上沿，实测差约 0.1 dB）；最大值与表读数逐位相同；
        //    旁通时表为 0、统计照报；Glue 拧到 0 淡出走完后两项为 0。
        // 依据：ColorModule.h BlockStats；设计规格-Color「Glue」（阈值映射、2:1、软拐点 6 dB）。
        void testColorGlueStats()
        {
            beginTest ("color stats: glue GR on a steady square wave matches the static curve, max equals the meter");

            ColorModule::Params p;
            p.glue = 100.0f;

            ColorModule color;
            color.setParams (p);
            color.prepare (statsSpec());
            color.setParams (p);

            const auto amplitude = 0.25f;
            const auto input = makeSquare (2, testRate, 6000.0, amplitude, 3.0);
            const auto expected = (db (amplitude) + 24.0) * 0.5;

            juce::AudioBuffer<float> block;
            double worstMean = 0.0, worstMax = 0.0;
            int meterMismatch = 0, position = 0;

            for (; position + statsBlock <= (int) testRate; position += statsBlock)
            {
                loadBlock (block, input, position, statsBlock);
                color.process (block);

                const auto& stats = color.getBlockStats();

                if (stats.glueGrMaxDb != color.getAndResetGainReductionDb())
                    ++meterMismatch;

                if (position >= (int) (0.5 * testRate))
                {
                    worstMean = juce::jmax (worstMean, std::abs ((double) stats.glueGrMeanDb - expected));
                    worstMax  = juce::jmax (worstMax,  std::abs ((double) stats.glueGrMaxDb - expected));
                    expectEquals (stats.heatDrive, 0.0f);
                }
            }

            logMessage ("  expected glue GR " + juce::String (expected, 3) + " dB; worst |mean - expected| "
                        + juce::String (worstMean, 4) + ", worst |max - expected| " + juce::String (worstMax, 4));

            expectLessThan (worstMean, 0.15);
            expectLessThan (worstMax, 0.15);
            expectEquals (meterMismatch, 0);

            // 旁通：表为 0，统计照报。
            p.bypass = true;
            color.setParams (p);
            float meter = 0.0f, lastMean = 0.0f;

            for (; position + statsBlock <= (int) (1.5 * testRate); position += statsBlock)
            {
                loadBlock (block, input, position, statsBlock);
                color.process (block);
                meter = juce::jmax (meter, color.getAndResetGainReductionDb());
                lastMean = color.getBlockStats().glueGrMeanDb;
            }

            expectGreaterThan (lastMean, 5.0f);
            expectEquals (meter, 0.0f, "bypassed: the meter shows nothing (the stats still describe the internal work)");

            // Glue 拧到 0：淡出走完（空闲跳过）后两项为 0。
            p.bypass = false;
            p.glue = 0.0f;
            color.setParams (p);

            for (; position + statsBlock <= (int) (2.0 * testRate); position += statsBlock)
            {
                loadBlock (block, input, position, statsBlock);
                color.process (block);
            }

            expectEquals (color.getBlockStats().glueGrMeanDb, 0.0f);
            expectEquals (color.getBlockStats().glueGrMaxDb, 0.0f);
        }

        // 6. Heat 实际驱动量 = Heat × 湿路淡化：Heat 60 → 9（15% 湿），Heat 5 → 0.0625（1.25% 湿），Heat 0 → 0（空闲）；
        //    过渡中介于两端之间。
        // 依据：ColorModule.h BlockStats「湿路混合后的有效 Heat」。
        void testColorHeatDrive()
        {
            beginTest ("color stats: heat drive is the effective (wet-faded) heat");

            const auto input = makeNoise (2, (int) (2.0 * testRate), 0.3f, 3);

            ColorModule::Params p;
            ColorModule color;
            color.setParams (p);
            color.prepare (statsSpec());
            color.setParams (p);

            juce::AudioBuffer<float> block;
            int position = 0;

            auto runFor = [&] (double seconds)
            {
                const auto end = position + (int) (seconds * testRate);
                float lo = 1.0e9f, hi = -1.0e9f;

                for (; position + statsBlock <= end; position += statsBlock)
                {
                    loadBlock (block, input, position % ((int) input.front().size() - statsBlock), statsBlock);
                    color.process (block);
                    lo = juce::jmin (lo, color.getBlockStats().heatDrive);
                    hi = juce::jmax (hi, color.getBlockStats().heatDrive);
                }

                return std::make_pair (lo, hi);
            };

            runFor (0.2);
            expectEquals (color.getBlockStats().heatDrive, 0.0f, "heat 0 (idle)");

            p.heat = 60.0f;
            color.setParams (p);
            const auto rising = runFor (0.05);
            runFor (0.2);
            expectWithinAbsoluteError (color.getBlockStats().heatDrive, 9.0f, 1.0e-5f, "heat 60, 15% wet");
            expect (rising.first >= 0.0f && rising.second <= 9.00001f);

            p.heat = 5.0f;
            color.setParams (p);
            runFor (0.2);
            expectWithinAbsoluteError (color.getBlockStats().heatDrive, 0.0625f, 1.0e-5f, "heat 5 → 1.25% wet");

            p.heat = 0.0f;
            color.setParams (p);
            runFor (0.2);
            expectEquals (color.getBlockStats().heatDrive, 0.0f, "back to 0 (idle)");
        }

        // 7. 带统计的 process 与读统计不经 operator new 分配（Glue / Heat / Clip / 限幅都在工作，也包括阈值滑动的块）。
        // 依据：实现计划「实时与正确性硬规则：音频线程 0 分配」。
        void testModuleStatsDoNotAllocate()
        {
            beginTest ("modules with block stats do not allocate on the audio path");

            LimiterModule::Params lp;
            lp.gainDb = 10.0f;
            lp.clip = 70.0f;
            LimiterModule limiter;
            limiter.setParams (lp);
            limiter.prepare (statsSpec());
            limiter.setParams (lp);

            ColorModule::Params cp;
            cp.glue = 70.0f;
            cp.heat = 40.0f;
            cp.toneDb = 1.0f;
            ColorModule color;
            color.setParams (cp);
            color.prepare (statsSpec());
            color.setParams (cp);

            const auto input = makeNoise (2, 3 * statsBlock, 0.8f, 11);
            juce::AudioBuffer<float> block (2, 3 * statsBlock);   // 3 倍块长：内部切段
            float sink = 0.0f;

            const auto before = allocationCountOnThisThread();

            for (int round = 0; round < 20; ++round)
            {
                if (round == 10)
                {
                    lp.clip = 30.0f;   // 阈值滑动的循环
                    limiter.setParams (lp);
                    cp.heat = 0.0f;
                    color.setParams (cp);
                }

                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data(), 3 * statsBlock);

                color.process (block);
                limiter.process (block);
                sink += color.getBlockStats().glueGrMeanDb + limiter.getBlockStats().clipActiveFraction;
            }

            // 先把计数读进局部量再断言：函数实参的求值顺序不确定（GCC 从右往左），提示文字的 juce::String 会先构造、被算进计数。

            const auto allocationsInPath = allocationCountOnThisThread() - before;

            expectEquals ((juce::int64) (allocationsInPath), (juce::int64) 0);
            expect (std::isfinite (sink));
        }

        //==============================================================================
        // 8. 退让时机与速率：Limit 均值 5 dB（上限 3，超出 2 dB）持续。前 1 s 不动；
        //    之后按 T-047：0.3 + (2/3)×(6−0.3) = 4.1 dB/s。0.5 s 退到约 −2.05，再继续就封顶 −3 并标「方案受限」。
        void testGuardRetreatTiming()
        {
            beginTest ("guard: T-047 retreat starts after 1 s over, 2 dB overshoot is 4.1 dB/s, floor is -3 dB");

            GlazeGuard guard;
            guard.prepare (testRate);
            guard.setLimits (limitMeanCap (3.0f), guardConstants());

            auto feed = [&guard] (double seconds, float grDb)
            {
                bool retreated = false;

                for (int done = 0; done < (int) std::lround (seconds * testRate); done += 480)
                {
                    auto stats = limitMeanStats (grDb);
                    stats.numSamples = 480;
                    guard.process (stats);
                    retreated = retreated || guard.isRetreating();
                }

                return retreated;
            };

            expect (! feed (1.0, 5.0f), "no retreat during the first second");
            expectEquals (guard.getProtectionDb(), 0.0f);

            expect (feed (0.5, 5.0f));
            expectWithinAbsoluteError (guard.getProtectionDb(), -2.05f, 1.0e-3f);
            expect (! guard.isPlanLimited());

            expect (feed (1.0, 5.0f));
            expectEquals (guard.getProtectionDb(), -3.0f);
            expect (guard.isPlanLimited(), "still over at the floor -> plan limited");
            expect (guard.isEngaged());

            guard.reset();
            expectEquals (guard.getProtectionDb(), 0.0f);
            expect (! guard.isPlanLimited() && ! guard.isEngaged() && ! guard.isRetreating());
        }

        // 9. 块长无关：同一段按音乐时间写好的统计脚本（超出 → 安全 → 再超出 → 安全，含退让与恢复），块长随机 1 ～ 4096、
        //    中间随机插静音块，每块返回的 g_保护与「每块一个时隙」的参考轨迹在同一音乐时刻逐位相同；静音块从不改 g_保护。
        // 依据：实现计划「时间常数与时长按样本数折算，宿主块长会变」；设计规格 §7「静音、停播不计入」。
        void testGuardBlockSizeIndependent()
        {
            beginTest ("guard: protection trajectory is identical for any block size (sample-time based), silence never changes it");

            const std::vector<GuardSegment> script {
                { (int) (2.5 * testRate), limitMeanStats (5.0f) },
                { (int) (7.0 * testRate), limitMeanStats (0.0f) },
                { (int) (1.8 * testRate), limitMeanStats (4.2f) },
                { (int) (9.0 * testRate), limitMeanStats (1.0f) },
            };

            const auto reference = slotReference (script);
            expect (reference.size() > 1000);
            expectLessThan (*std::min_element (reference.begin(), reference.end()), -1.0f, "the script must retreat");
            expectGreaterThan (reference.back(), *std::min_element (reference.begin(), reference.end()), "and recover");

            for (int trial = 0; trial < 6; ++trial)
            {
                juce::Random random (1000 + trial);
                GlazeGuard guard;
                guard.prepare (testRate);
                guard.setLimits (limitMeanCap (3.0f), guardConstants());

                const auto trace = runGuard (guard, script, [&random] { return 1 + random.nextInt (4096); }, trial % 2 == 0 ? 3 : 0);

                int mismatches = 0;

                for (size_t k = 0; k < trace.protection.size(); ++k)
                {
                    const auto end = trace.musicEnd[k];

                    if (end < 0)
                    {
                        ++mismatches;   // 静音块改了 g_保护
                        continue;
                    }

                    const auto slotsDone = (size_t) (end / 480);
                    const auto expected = slotsDone == 0 ? 0.0f : reference[slotsDone - 1];

                    if (trace.protection[k] != expected)
                        ++mismatches;
                }

                expectEquals (mismatches, 0, "trial " + juce::String (trial));
            }
        }

        // 10. 约束一满足就停：闭环模型「Limit 均值 GR = 5 + g_保护」（上限 3，需要退 2 dB）。统计窗只留 g_保护相差 ≤ 0.5 dB 的时隙，
        //     所以最终停在 −2.0 ～ −2.5 dB（多退 ≤ 0.5 dB，而不是 3 s 窗滞后带来的 1.5 dB 以上），之后停住（回差区内不恢复）。
        // 依据：设计规格 §7「G_实际 = G_参考 + g_保护」「恢复中再接近上限（留回差）就停止恢复」。
        void testGuardStopsRetreatWhenSatisfied()
        {
            beginTest ("guard: retreat stops once the constraint is satisfied (small overshoot), then holds");

            GlazeGuard guard;
            guard.prepare (testRate);
            guard.setLimits (limitMeanCap (3.0f), guardConstants());

            float atEight = 0.0f;

            for (int block = 0; block < (int) (20.0 * testRate) / statsBlock; ++block)
            {
                auto stats = limitMeanStats (juce::jmax (0.0f, 5.0f + guard.getProtectionDb()));
                stats.numSamples = statsBlock;
                guard.process (stats);

                if (block == (int) (8.0 * testRate) / statsBlock)
                    atEight = guard.getProtectionDb();
            }

            const auto settled = guard.getProtectionDb();
            logMessage ("  settled at " + juce::String (settled, 3) + " dB (needed -2.0 dB)");

            expect (settled <= -2.0f && settled >= -2.5f, "settles within 0.5 dB beyond the needed retreat");
            expectWithinAbsoluteError (settled, atEight, 1.0e-6f, "and holds there");
            expect (! guard.isPlanLimited());
        }

        // 11. 恢复：峰值 8 dB（上限 6，超出 2 dB）持续 2 s，按 4.1 dB/s 退到地板 −3。
        //     峰值归 0 后 100 ms 保持期仍算超出，停在 −3。再连续 5 s 安全才开始恢复；
        //     地板上的回程是 0.3 dB/s（第一格 +0.003），最后回到 0、不超过 0。
        //     安全等待中插入 3 s 静音：g_保护不动，恢复也不提前。
        void testGuardRecoveryAndSilence()
        {
            beginTest ("guard: recovery waits 5 s of safe music, silence does not count, floor recovers at 0.3 dB/s");

            for (auto withSilence : { false, true })
            {
                GlazeGuard guard;
                guard.prepare (testRate);
                guard.setLimits (limitPeakCap (6.0f), guardConstants());

                auto slot = [&guard] (float peakDb)
                {
                    auto stats = limitPeakStats (peakDb);
                    stats.numSamples = 480;
                    return (float) guard.process (stats);
                };

                for (int k = 0; k < 200; ++k)
                    slot (8.0f);

                expectEquals (guard.getProtectionDb(), -3.0f);
                expect (guard.isPlanLimited());

                for (int k = 0; k < 9; ++k)
                    slot (0.0f);

                expectEquals (guard.getProtectionDb(), -3.0f, "peak hold keeps the floor for 9 more slots");

                bool changedEarly = false;

                for (int k = 0; k < 500; ++k)
                {
                    if (withSilence && k == 250)
                    {
                        ModuleStats silent;   // musicActive = false
                        silent.numSamples = (int) (3.0 * testRate);
                        silent.limitGrMaxDb = 20.0f;   // 静音块里的统计一律不看
                        guard.process (silent);
                        expectEquals (guard.getProtectionDb(), -3.0f, "silence does not move protection");
                    }

                    slot (0.0f);
                    changedEarly = changedEarly || guard.getProtectionDb() != -3.0f;
                }

                expect (! changedEarly, "no recovery before 5 s of safe music");

                const auto first = slot (0.0f);
                expectWithinAbsoluteError (first, -2.997f, 1.0e-4f, "at the floor the return rate is 0.3 dB/s");
                // engaged = 正在退，或退到底仍超。回程不是 engaged。
                expect (guard.isRecovering());
                expect (! guard.isEngaged());

                for (int k = 0; k < 4000; ++k)
                    slot (0.0f);

                expectEquals (guard.getProtectionDb(), 0.0f, "back to the reference, never above");
                expect (! guard.isEngaged());
            }
        }

        // 12. 恢复中接近上限就停：先退到 −2 dB，再换成闭环模型「峰值 GR = 6.2 + g_保护」（上限 6、回差线 5.5）：
        //     5 s 后开始恢复，升到 g_保护 ≈ −0.7（峰值进入回差区）就停住，不再回到超限（−0.2）、也不来回拉锯。
        // 依据：设计规格 §7「恢复中再接近上限（留回差）就停止恢复」（回差 0.5 dB，见 GlazeGuard.cpp）。
        void testGuardRecoveryStopsNearLimit()
        {
            beginTest ("guard: recovery stops inside the hysteresis band below the limit");

            GlazeGuard guard;
            guard.prepare (testRate);
            guard.setLimits (limitPeakCap (6.0f), guardConstants());

            for (int k = 0; k < 300; ++k)   // 1 s 后退 2 s → −2.0
            {
                auto stats = limitPeakStats (9.0f);
                stats.numSamples = 480;
                guard.process (stats);
            }

            // 9 dB 的尖峰要 100 ms 才从保持窗里出去：先喂 10 格安全值（这 9 格仍在退让，之后停在 −2.09 附近）。
            for (int k = 0; k < 10; ++k)
            {
                auto stats = limitPeakStats (0.0f);
                stats.numSamples = 480;
                guard.process (stats);
            }

            std::vector<float> trajectory;

            for (int block = 0; block < (int) (30.0 * testRate) / statsBlock; ++block)
            {
                auto stats = limitPeakStats (6.2f + guard.getProtectionDb());
                stats.numSamples = statsBlock;
                trajectory.push_back (guard.process (stats));
            }

            const auto peak = std::max_element (trajectory.begin(), trajectory.end());
            const auto highest = *peak;
            const auto lowestAfter = *std::min_element (peak, trajectory.end());

            logMessage ("  recovery stopped at " + juce::String (highest, 3) + " dB (hysteresis line at -0.7 dB)");

            expect (highest > -0.8f && highest <= -0.69f, "stops at the hysteresis line, below the limit (-0.2)");
            expectEquals (lowestAfter, highest, "and stays there (no hunting)");
            expectEquals (trajectory.back(), highest);
        }

        // 13. 方案受限：峰值 20 dB（上限 6，超出封顶，退让 6 dB/s）很快到 −3 并锁存；
        //    约束解除后要满 5 s 安全音乐才开始恢复，标记随之清除。
        void testGuardPlanLimited()
        {
            beginTest ("guard: plan-limited flag latches at the floor and clears once recovery starts");

            GlazeGuard guard;
            guard.prepare (testRate);
            guard.setLimits (limitPeakCap (6.0f), guardConstants());

            auto run = [&guard] (int slots, float peakDb)
            {
                for (int k = 0; k < slots; ++k)
                {
                    auto stats = limitPeakStats (peakDb);
                    stats.numSamples = 480;
                    guard.process (stats);
                }
            };

            run (100, 20.0f);   // 触发窗刚满，还没退
            expect (! guard.isPlanLimited());
            expectEquals (guard.getProtectionDb(), 0.0f);

            run (80, 20.0f);    // 6 dB/s，不到一秒就到地板
            expect (guard.isPlanLimited());
            expectEquals (guard.getProtectionDb(), -3.0f);

            run (10, 0.0f);     // 峰值保持窗走完
            run (490, 0.0f);    // 安全还不满 5 s
            expect (guard.isPlanLimited());
            expectEquals (guard.getProtectionDb(), -3.0f);

            run (20, 0.0f);     // 跨过 5 s：开始恢复
            expect (! guard.isPlanLimited());
            expectGreaterThan (guard.getProtectionDb(), -3.0f);
        }

        // 14. 峰值要「持续」才退：每秒一个 10 dB 尖峰块（上限 6）→ 3 s 内只累计约 0.3 s 超出，20 s 都不退；
        //     每 100 ms 一个尖峰 → 超出连成片，1 s 后开始退。
        // 依据：设计规格 §7「持续超出 ≥ 1 s」；GlazeGuard.cpp「峰值类按最近 100 ms 的最大值判」。
        void testGuardPeakNeedsPersistence()
        {
            beginTest ("guard: isolated peaks do not trigger a retreat, dense ones do");

            for (auto spacingSeconds : { 1.0, 0.1 })
            {
                GlazeGuard guard;
                guard.prepare (testRate);
                guard.setLimits (limitPeakCap (6.0f), guardConstants());

                const auto spacingBlocks = juce::jmax (1, (int) std::lround (spacingSeconds * testRate / statsBlock));

                for (int block = 0; block < (int) (20.0 * testRate) / statsBlock; ++block)
                {
                    auto stats = limitPeakStats (block % spacingBlocks == 0 ? 10.0f : 0.0f);
                    stats.numSamples = statsBlock;
                    guard.process (stats);
                }

                if (spacingSeconds > 0.5)
                    expectEquals (guard.getProtectionDb(), 0.0f, "one spike per second");
                else
                    expectLessThan (guard.getProtectionDb(), -1.0f, "a spike every 100 ms");
            }
        }

        // 15. p95 与 Clip 约束：
        //     - Glue：均值上限 3、p95 上限 4；10 % 的时隙 6 dB、其余 1 dB（均值 1.5，p95 = 6）→ 退让；3 % 的时隙 6 dB（p95 = 1）→ 不退；
        //     - Clip 活动占比上限 5 %：持续 8 % → 退让，3 % → 不退；Clip 深度上限 2 dB：持续 3 dB → 退让。
        // 依据：设计规格-GLAZE-决策与校准 §4 约束表（Glue 均值与 p95、Clip 深度与活动占比）、§7。
        void testGuardP95AndClipConstraints()
        {
            beginTest ("guard: glue p95, clip activity and clip depth constraints");

            auto runCase = [] (const glaze::StyleConstants& caps, const std::function<ModuleStats (int)>& statsForSlot)
            {
                GlazeGuard guard;
                guard.prepare (testRate);
                guard.setLimits (caps, guardConstants());

                for (int k = 0; k < 800; ++k)   // 8 s
                {
                    auto stats = statsForSlot (k);
                    stats.numSamples = 480;
                    guard.process (stats);
                }

                return guard.getProtectionDb();
            };

            auto glueCaps = noCaps();
            glueCaps.glueGrMeanCapDb = 3.0f;
            glueCaps.glueGrP95CapDb = 4.0f;

            auto gluePattern = [] (int everyN)
            {
                return [everyN] (int k)
                {
                    auto s = musicStats();
                    s.glueGrMeanDb = (k % everyN == 0) ? 6.0f : 1.0f;
                    s.glueGrMaxDb = s.glueGrMeanDb;
                    return s;
                };
            };

            expectLessThan (runCase (glueCaps, gluePattern (10)), -1.0f, "10 % of slots above the p95 cap");
            expectEquals (runCase (glueCaps, gluePattern (33)), 0.0f, "3 % of slots above: p95 is fine");

            auto activityCaps = noCaps();
            activityCaps.clipActivityCap = 0.05f;

            auto activity = [] (float fraction)
            {
                return [fraction] (int)
                {
                    auto s = musicStats();
                    s.clipActiveFraction = fraction;
                    return s;
                };
            };

            expectLessThan (runCase (activityCaps, activity (0.08f)), -1.0f, "clip activity 8 % > 5 %");
            expectEquals (runCase (activityCaps, activity (0.03f)), 0.0f, "clip activity 3 %");

            auto depthCaps = noCaps();
            depthCaps.clipDepthCapDb = 2.0f;

            expectLessThan (runCase (depthCaps, [] (int) { auto s = musicStats(); s.clipDepthMaxDb = 3.0f; return s; }), -1.0f,
                            "clip depth 3 dB > 2 dB");
        }

        // 16. 不生效的情形：setLimits 之前、上限全 ≤ 0（未配置）、统计里有 NaN / Inf：g_保护恒为 0、不出 NaN。
        // 依据：GlazeGuard.h（上限 ≤ 0 表示该项不设约束；调用 setLimits 之前不做判定）。
        void testGuardInactiveWithoutLimits()
        {
            beginTest ("guard: inactive before limits are set or when no cap is configured; bad stats never produce NaN");

            GlazeGuard unset;
            unset.prepare (testRate);

            GlazeGuard uncapped;
            uncapped.prepare (testRate);
            uncapped.setLimits (noCaps(), guardConstants());

            GlazeGuard poisoned;
            poisoned.prepare (testRate);
            poisoned.setLimits (limitMeanCap (3.0f), guardConstants());

            for (int k = 0; k < 1000; ++k)
            {
                auto loud = limitMeanStats (12.0f);
                loud.glueGrMeanDb = loud.clipDepthMaxDb = 12.0f;
                loud.clipActiveFraction = 1.0f;
                loud.numSamples = 480;
                unset.process (loud);
                uncapped.process (loud);

                auto bad = limitMeanStats (k % 2 == 0 ? std::numeric_limits<float>::quiet_NaN() : -std::numeric_limits<float>::infinity());
                bad.numSamples = 480;
                poisoned.process (bad);
            }

            expectEquals (unset.getProtectionDb(), 0.0f);
            expectEquals (uncapped.getProtectionDb(), 0.0f);
            expectEquals (poisoned.getProtectionDb(), 0.0f, "NaN / -Inf read as 0 dB");
        }

        // 17. process / setLimits（上限不变与改变）不经 operator new 分配。
        // 依据：实现计划「音频线程 0 分配」。
        void testGuardDoesNotAllocate()
        {
            beginTest ("guard does not allocate");

            GlazeGuard guard;
            guard.prepare (testRate);
            auto caps = limitMeanCap (3.0f);
            caps.glueGrP95CapDb = 4.0f;
            caps.limitGrPeakCapDb = 6.0f;
            guard.setLimits (caps, guardConstants());

            float sink = 0.0f;
            const auto before = allocationCountOnThisThread();

            for (int k = 0; k < 3000; ++k)
            {
                auto stats = limitMeanStats ((float) (k % 7));
                stats.glueGrMeanDb = (float) (k % 5);
                stats.numSamples = 1 + (k * 37) % 2048;
                sink += guard.process (stats);

                if (k % 500 == 0)
                {
                    caps.limitGrMeanCapDb = 3.0f + (float) (k % 3);
                    guard.setLimits (caps, guardConstants());
                }
            }

            // 先把计数读进局部量再断言：函数实参的求值顺序不确定（GCC 从右往左），提示文字的 juce::String 会先构造、被算进计数。

            const auto allocationsInPath = allocationCountOnThisThread() - before;

            expectEquals ((juce::int64) (allocationsInPath), (juce::int64) 0);
            expect (std::isfinite (sink));
        }

    };

    static GlazeCalibrationTests glazeCalibrationTests;
}
