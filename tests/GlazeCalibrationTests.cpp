/*
  ==============================================================================

    GlazeCalibrationTests.cpp
    v0.6 GLAZE 线 A3 的测试（04-协调/2026-09-25-v0.6-GLAZE实现计划）：
      - Color / Limit 每块统计（BlockStats）在已知信号下数值正确、覆盖整次调用、不分配；
      - 保护层 GlazeGuard：按样本时间（与块长无关）以 1 dB/s 退让、0.3 dB/s 恢复，静音不计入，接近上限停止恢复，
        退到下限标「方案受限」，峰值约束要持续才退让，p95 / 占比约束，未配置的约束不生效，不分配；
      - 校准器 GlazeCalibrator：用「已知偏差」的简化链模型（输出子块 = 输入子块 × 链的净增益）验证收敛、子窗作废、
        按方向防积分饱和、回馈用实际执行量、两轮结果与 partial / budgetLimited、参考条件逐项失效、不分配。

    只按各单元头文件的公开接口写；校准器的输入直接构造 LoudnessMeter::SubBlock 序列，不经过表与链。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ColorModule.h"
#include "../Source/dsp/GlazeCalibrator.h"
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

        // 保护层用规格的出厂时长与速率（−3 dB、1 dB/s、0.3 dB/s、1 s、3 s、5 s），与 GlazeConstants 的缺省一致。
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

        //==============================================================================
        // 校准器的简化链模型：入口子块响度按脚本（默认 −14 LUFS），出口子块 = 对应入口子块 × 链的净提升，
        //   净提升 = 目标 − 已知偏差 + 灵敏度 × 实际施加的偏差 + 随音乐的起伏 + （模拟中断期间的）坏数据。
        // 编排器的行为：把校准器的建议钳到执行器范围，按 rampDbPerSecond 平滑施加（0 = 下一块立即到位），实际值回传。
        using glaze::GlazeCalibrator;

        constexpr int subBlockSamples = 4800;   // 48 kHz 下表的 100 ms 子块

        glaze::Constants calConstants (int rounds = 2)
        {
            glaze::Constants c;
            c.calibrationSettleSeconds = 3.0;
            c.calibrationWindowSeconds = 10.0;
            c.calibrationRounds = rounds;
            return c;
        }

        struct CalModel
        {
            float targetDb = 6.0f;
            double deviationDb = 1.3;
            double sensitivity = 1.0;
            float limitDb = 3.0f;
            int latencySamples = 250;
            float actuatorMin = -100.0f, actuatorMax = 100.0f;
            double rampDbPerSecond = 2.0;
            std::function<double (std::int64_t)> offsetDb;          // 随音乐（时间）变的净提升起伏
            std::function<double (std::int64_t)> inputLufs;         // 入口子块响度
            std::function<double (std::int64_t)> corruptOutputDb;   // 出口额外加的量（坏数据）
            std::function<bool (std::int64_t)> guardActive;
            std::function<void (std::int64_t, GlazeCalibrator&)> beforeBlock;   // 每块 process 之前（例如在这里 invalidate）
            std::uint64_t* allocationsInCalls = nullptr;   // 非空时累计 begin / process / 读结果期间经 operator new 的分配次数
        };

        struct CalOutcome
        {
            glaze::Calibration result;
            GlazeCalibrator::State state = GlazeCalibrator::State::idle;
            GlazeCalibrator::Progress progress;
            double finishedAtSeconds = -1.0;
            bool sawCollecting = false;
            float finalApplied = 0.0f;
        };

        double lufsToMeanSquare (double lufs)   { return std::pow (10.0, (lufs + 0.691) / 10.0); }

        CalOutcome runCalibration (GlazeCalibrator& calibrator, const CalModel& model, double seconds,
                                   const std::function<int()>& nextBlock, int rounds = 2)
        {
            const auto total = (std::int64_t) (seconds * testRate);
            const auto shift = (std::int64_t) std::llround ((double) model.latencySamples / subBlockSamples);

            std::vector<double> inputEnergies;
            inputEnergies.reserve ((size_t) (total / subBlockSamples + 2));
            std::vector<LoudnessMeter::SubBlock> ins, outs;
            ins.reserve (64);
            outs.reserve (64);

            calibrator.prepare (testRate, 4096);
            calibrator.setConstants (calConstants (rounds));
            glaze::CalibrationReference reference;
            reference.sampleRate = testRate;
            auto counted = [&model] (auto&& call)
            {
                const auto before = allocationCountOnThisThread();
                call();

                if (model.allocationsInCalls != nullptr)
                    *model.allocationsInCalls += allocationCountOnThisThread() - before;
            };

            counted ([&] { calibrator.begin (model.targetDb, model.limitDb, reference, model.latencySamples); });

            CalOutcome outcome;
            float applied = 0.0f;
            std::int64_t t = 0;

            while (t < total)
            {
                const auto n = (int) juce::jmin ((std::int64_t) juce::jmax (1, nextBlock()), total - t);

                if (model.beforeBlock)
                    counted ([&] { model.beforeBlock (t, calibrator); });

                ins.clear();
                outs.clear();

                // 本块里完成的子块：结束时刻 e = k·L − 1 落在 [t, t + n)。
                for (auto k = t / subBlockSamples + 1; k * subBlockSamples - 1 < t + n; ++k)
                {
                    const auto end = k * subBlockSamples - 1;

                    if (end < t)
                        continue;

                    const auto inEnergy = lufsToMeanSquare (model.inputLufs ? model.inputLufs (end) : -14.0);
                    inputEnergies.push_back (inEnergy);
                    ins.push_back ({ inEnergy, 0.0f, end });

                    const auto source = k - 1 - shift;   // 链延迟：这个出口子块主要来自 shift 个子块之前的入口

                    if (source >= 0)
                    {
                        auto netDb = (double) model.targetDb - model.deviationDb + model.sensitivity * (double) applied;
                        if (model.offsetDb)         netDb += model.offsetDb (end);
                        if (model.corruptOutputDb)  netDb += model.corruptOutputDb (end);
                        outs.push_back ({ inputEnergies[(size_t) source] * std::pow (10.0, netDb / 10.0), 0.0f, end });
                    }
                }

                const auto guard = model.guardActive ? model.guardActive (t) : false;
                float suggestion = 0.0f;

                counted ([&]
                {
                    calibrator.process (ins.data(), (int) ins.size(), outs.data(), (int) outs.size(), applied, guard, n);
                    suggestion = calibrator.getResult().gainBiasDb;
                    juce::ignoreUnused (calibrator.getProgress());
                });

                t += n;

                outcome.sawCollecting = outcome.sawCollecting || calibrator.getState() == GlazeCalibrator::State::collecting;

                if (calibrator.getState() == GlazeCalibrator::State::finished && outcome.finishedAtSeconds < 0.0)
                    outcome.finishedAtSeconds = (double) t / testRate;

                // 编排器：把建议钳到执行器范围，平滑施加。
                const auto wanted = juce::jlimit (model.actuatorMin, model.actuatorMax, suggestion);

                if (model.rampDbPerSecond <= 0.0)
                {
                    applied = wanted;
                }
                else
                {
                    const auto step = (float) (model.rampDbPerSecond * n / testRate);
                    applied = wanted > applied ? juce::jmin (wanted, applied + step) : juce::jmax (wanted, applied - step);
                }
            }

            outcome.result = calibrator.getResult();
            outcome.state = calibrator.getState();
            outcome.progress = calibrator.getProgress();
            outcome.finalApplied = applied;
            return outcome;
        }

        std::function<int()> fixedBlocks (int size)   { return [size] { return size; }; }
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

            testCalibratorConverges();
            testCalibratorOneRoundWhenAlreadyClose();
            testCalibratorPartialWhenSensitivityIsLow();
            testCalibratorInterruptionDropsOnlyCurrentWindow();
            testCalibratorDirectionalAntiWindup();
            testCalibratorUsesExecutedBias();
            testCalibratorBudgetLimited();
            testCalibratorSilenceAndGuardPauses();
            testCalibratorOppositeSubWindows();
            testCalibratorReferenceMatching();
            testCalibratorReferenceMatchingContinuousConditions();
            testCalibratorBadInputs();
            testCalibratorDoesNotAllocate();
            testCalibratorAndGuardDoNotFight();
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
        // 8. 退让时机与速率：Limit 3 s 均值 5 dB（上限 3）持续，按 10 ms 时隙：前 1 s 不动（第 100 个时隙结束时 3 s 内刚满 1 s），
        //    之后每秒 −1 dB：音乐时间 2 s 时 −1.00、3.5 s 时 −2.50、4 s 起封顶 −3 并标「方案受限」；退让期间 isRetreating。
        // 依据：设计规格-GLAZE-决策与校准 §7「3 s 窗内持续超出 ≥ 1 s，以 1 dB/s 退让」「退到 −3 dB 仍超约束：方案受限」。
        void testGuardRetreatTiming()
        {
            beginTest ("guard: retreats at 1 dB/s once a constraint has been exceeded for 1 s, caps at -3 dB and flags plan limited");

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

            expect (feed (1.0, 5.0f));
            expectWithinAbsoluteError (guard.getProtectionDb(), -1.0f, 1.0e-5f);
            expect (! guard.isPlanLimited());

            feed (1.5, 5.0f);
            expectWithinAbsoluteError (guard.getProtectionDb(), -2.5f, 1.0e-5f);

            feed (1.0, 5.0f);
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

        // 11. 恢复：Limit 峰值 8 dB（上限 6）持续 2 s → 第 101 ～ 200 个时隙退让到 −1.00；峰值归 0 后 100 ms 保持期里还多退 9 格
        //     （−1.09），之后连续 5 s（500 格）安全才以 0.3 dB/s 恢复（每格 +0.003），回到 0 为止、不超过 0。
        //     同一脚本在安全等待期间插 3 s 静音：g_保护与恢复起点（按音乐时间）完全不变。
        // 依据：设计规格 §7「连续 ≥ 5 s 有效、安全的音乐之后以 0.3 dB/s 恢复」「静音、停播不计入恢复时间」「回到 0 表示恢复参考方案」。
        void testGuardRecoveryAndSilence()
        {
            beginTest ("guard: recovers at 0.3 dB/s after 5 s of safe music, silence does not count, never above 0");

            for (auto withSilence : { false, true })
            {
                GlazeGuard guard;
                guard.prepare (testRate);
                guard.setLimits (limitPeakCap (6.0f), guardConstants());

                auto slot = [&guard] (float peakDb)
                {
                    auto stats = limitPeakStats (peakDb);
                    stats.numSamples = 480;
                    return guard.process (stats);
                };

                for (int k = 0; k < 200; ++k)
                    slot (8.0f);

                expectWithinAbsoluteError (guard.getProtectionDb(), -1.0f, 1.0e-5f);

                for (int k = 0; k < 9; ++k)
                    slot (0.0f);

                expectWithinAbsoluteError (guard.getProtectionDb(), -1.09f, 1.0e-5f, "peak hold keeps it over for 9 more slots");

                bool changedEarly = false;

                for (int k = 0; k < 500; ++k)
                {
                    if (withSilence && k == 250)
                    {
                        ModuleStats silent;   // musicActive = false
                        silent.numSamples = (int) (3.0 * testRate);
                        silent.limitGrMaxDb = 20.0f;   // 静音块里的统计一律不看
                        guard.process (silent);
                    }

                    slot (0.0f);
                    changedEarly = changedEarly || guard.getProtectionDb() != -1.09f;
                }

                expect (! changedEarly, "no recovery before 5 s of safe music");

                const auto first = slot (0.0f);
                expectWithinAbsoluteError (first, -1.087f, 1.0e-5f);
                expect (guard.isRecovering() && guard.isEngaged());

                for (int k = 0; k < 199; ++k)
                    slot (0.0f);

                expectWithinAbsoluteError (guard.getProtectionDb(), -1.09f + 0.6f, 1.0e-4f, "0.3 dB/s for 2 s");

                for (int k = 0; k < 1000; ++k)
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

        // 13. 方案受限：持续超出退到 −3 后标记；约束解除、5 s 安全后开始恢复，标记随之清除。
        // 依据：设计规格 §7「退到 −3 dB 仍超约束：标方案受限」。
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

            run (350, 20.0f);   // 1 s 触发 + 2.5 s 退让 → −2.5
            expect (! guard.isPlanLimited());
            run (60, 20.0f);    // 到 −3
            expect (guard.isPlanLimited());
            expectEquals (guard.getProtectionDb(), -3.0f);

            run (400, 0.0f);    // 安全 4 s：还没恢复，标记保持
            expect (guard.isPlanLimited());
            run (120, 0.0f);    // 过了 5 s：开始恢复
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

        //==============================================================================
        // 18. 收敛：已知偏差 +1.3 dB（灵敏度 1）→ 第一轮测出 e = 1.3、建议 1.3，第二轮验证 |e| ≈ 0 → 有效、非 partial、偏差 1.3（±0.001），
        //     两轮完成、约 27 s（settle 3 s + 窗 10 s，两轮 + 施加斜坡）结束；偏差 −2.0 同理。随机块长 1 ～ 4096 结果逐位相同、
        //     结束时刻相差 < 0.5 s；链延迟 7000 样本（出口子块比入口晚一个子块）、立即施加也都收敛。
        // 依据：设计规格-GLAZE-决策与校准 §6（每轮顺序、第二轮验证第一轮、输入输出按链延迟对齐）；界面与实现 §5.3「收敛到容差内」。
        void testCalibratorConverges()
        {
            beginTest ("calibrator: converges to a known chain deviation in two rounds, independent of block size and latency");

            CalModel model;
            GlazeCalibrator calibrator;
            const auto reference = runCalibration (calibrator, model, 40.0, fixedBlocks (512));

            logMessage ("  +1.3 dB deviation: bias " + juce::String (reference.result.gainBiasDb, 5) + " dB, finished at "
                        + juce::String (reference.finishedAtSeconds, 2) + " s, rounds " + juce::String (reference.progress.round));

            expect (reference.state == GlazeCalibrator::State::finished && reference.sawCollecting);
            expect (reference.result.valid && ! reference.result.partial && ! reference.result.budgetLimited);
            expectWithinAbsoluteError (reference.result.gainBiasDb, 1.3f, 0.001f);
            expectEquals (reference.progress.round, 2);
            expect (reference.finishedAtSeconds > 25.0 && reference.finishedAtSeconds < 30.0);

            for (int trial = 0; trial < 5; ++trial)
            {
                juce::Random random (77 + trial);
                GlazeCalibrator other;
                const auto run = runCalibration (other, model, 40.0, [&random] { return 1 + random.nextInt (4096); });

                expect (run.result.valid && ! run.result.partial);
                expectEquals (run.result.gainBiasDb, reference.result.gainBiasDb, "trial " + juce::String (trial));
                expectLessThan (std::abs (run.finishedAtSeconds - reference.finishedAtSeconds), 0.5);
            }

            auto negative = model;
            negative.deviationDb = -2.0;
            const auto down = runCalibration (calibrator, negative, 40.0, fixedBlocks (512));
            expect (down.result.valid && ! down.result.partial);
            expectWithinAbsoluteError (down.result.gainBiasDb, -2.0f, 0.001f);

            auto late = model;
            late.latencySamples = 7000;
            const auto delayed = runCalibration (calibrator, late, 40.0, fixedBlocks (256));
            expect (delayed.result.valid && ! delayed.result.partial, "latency longer than a sub-block");
            expectWithinAbsoluteError (delayed.result.gainBiasDb, 1.3f, 0.001f);

            auto instant = model;
            instant.rampDbPerSecond = 0.0;
            const auto immediate = runCalibration (calibrator, instant, 40.0, fixedBlocks (1024));
            expect (immediate.result.valid && ! immediate.result.partial);
            expectWithinAbsoluteError (immediate.result.gainBiasDb, 1.3f, 0.001f);
        }

        // 19. 已经在容差内（偏差 0.1 dB）：第一轮的测量本身就验证了当前偏差（0），一轮结束，有效。
        // 依据：§6「算一次修正」；收敛容差 0.25 dB（GlazeCalibrator.cpp）。
        void testCalibratorOneRoundWhenAlreadyClose()
        {
            beginTest ("calibrator: a deviation inside the tolerance is verified in one round");

            CalModel model;
            model.deviationDb = 0.1;
            GlazeCalibrator calibrator;
            const auto run = runCalibration (calibrator, model, 30.0, fixedBlocks (512));

            expect (run.result.valid && ! run.result.partial);
            expectEquals (run.result.gainBiasDb, 0.0f);
            expectEquals (run.progress.round, 1);
            expect (run.finishedAtSeconds > 12.0 && run.finishedAtSeconds < 15.0);
            expectWithinAbsoluteError (run.progress.lastRoundErrorDb, 0.1f, 1.0e-3f);
        }

        // 20. 第二轮仍有明显偏差 → 保守的 partial：链对偏差的灵敏度只有 0.6（限幅吃掉一部分），
        //     偏差 +1.0：第二轮还差 +0.4 → 不再往上推，保持 1.0；偏差 −1.0：第二轮还响 0.4 → 允许往下修到 −1.4。
        // 依据：§6「第二轮主要验证第一轮；仍有明显偏差……接受保守、未完全校准的结果」。
        void testCalibratorPartialWhenSensitivityIsLow()
        {
            beginTest ("calibrator: a remaining deviation after round two ends as a conservative partial result");

            CalModel model;
            model.sensitivity = 0.6;
            model.deviationDb = 1.0;
            GlazeCalibrator calibrator;

            const auto up = runCalibration (calibrator, model, 40.0, fixedBlocks (512));
            expect (up.result.valid && up.result.partial && ! up.result.budgetLimited);
            expectWithinAbsoluteError (up.result.gainBiasDb, 1.0f, 0.001f, "too quiet: keep the verified bias, do not push further up");
            expectWithinAbsoluteError (up.progress.lastRoundErrorDb, 0.4f, 0.01f);

            model.deviationDb = -1.0;
            const auto down = runCalibration (calibrator, model, 40.0, fixedBlocks (512));
            expect (down.result.valid && down.result.partial);
            expectWithinAbsoluteError (down.result.gainBiasDb, -1.4f, 0.01f, "too loud: going down is allowed");
        }

        // 21. 子窗被打断只作废当前子窗：第一轮第 3 个子窗采到一半（9.0 s）时中断，此后 1 s 的出口数据是坏的（+10 dB）。
        //     中断后已完成的 2 个子窗保留、当前子窗清零；结果仍是 1.3 dB（坏数据一个都没进），结束时刻比「整轮重来」早；
        //     对照：同样的坏数据不作废 → 结果明显偏离（证明这段数据确实会影响结果）。
        // 依据：§6「当前子窗作废；已完成且条件一致的子窗保留」。
        void testCalibratorInterruptionDropsOnlyCurrentWindow()
        {
            beginTest ("calibrator: an interruption drops only the current sub-window");

            // 不用 const：lambda 必须按值捕获它们（const 整数常量不必捕获，严格警告会报「多余的捕获」）。
            std::int64_t corruptFrom = (std::int64_t) (9.0 * testRate), corruptTo = (std::int64_t) (10.0 * testRate);

            CalModel model;
            model.corruptOutputDb = [corruptFrom, corruptTo] (std::int64_t end)
            {
                return (end >= corruptFrom && end < corruptTo) ? 10.0 : 0.0;
            };

            GlazeCalibrator::Progress atInterrupt, beforeInterrupt;
            bool interrupted = false;

            model.beforeBlock = [&] (std::int64_t t, GlazeCalibrator& calibrator)
            {
                if (! interrupted && t >= corruptFrom)
                {
                    interrupted = true;
                    beforeInterrupt = calibrator.getProgress();
                    calibrator.invalidateCurrentWindow();
                    atInterrupt = calibrator.getProgress();
                }
            };

            GlazeCalibrator calibrator;
            const auto run = runCalibration (calibrator, model, 45.0, fixedBlocks (512));

            expect (interrupted);
            expectEquals (beforeInterrupt.completedSubWindows, 2);
            expectGreaterThan (beforeInterrupt.currentSubWindowSeconds, 0.5);
            expectEquals (atInterrupt.completedSubWindows, 2, "completed sub-windows are kept");
            expectEquals (atInterrupt.currentSubWindowSeconds, 0.0, "the current one is dropped");

            expect (run.result.valid && ! run.result.partial);
            expectWithinAbsoluteError (run.result.gainBiasDb, 1.3f, 0.001f, "none of the corrupt data got in");
            expectLessThan (run.finishedAtSeconds, 34.0, "earlier than restarting the whole round");

            auto uncaught = model;
            uncaught.beforeBlock = nullptr;
            const auto polluted = runCalibration (calibrator, uncaught, 45.0, fixedBlocks (512));
            expectGreaterThan (std::abs (polluted.result.gainBiasDb - 1.3f), 0.3f, "control: without the interruption the data does pollute");
        }

        // 22. 按方向防积分饱和（修正范围 ±1 dB）：
        //     - 偏差 +2：建议钳在 +1，第二轮仍差 +1 → 结束在 +1（不超上限）；
        //     - 同样到了上限，但第二轮时音乐变得更「吃」增益（+2.8 dB）→ 测到偏响 −1.8 → 允许从上限往下退到 −0.8；
        //     - 偏差 −2：钳在 −1，第二轮仍偏响 → 保守结果仍不低于 −1（下限同理）。
        // 依据：§6「防积分饱和按方向：到上限只禁止继续往上，仍允许往下退；下限反之」。
        void testCalibratorDirectionalAntiWindup()
        {
            beginTest ("calibrator: anti-windup is directional at the correction limits");

            CalModel model;
            model.limitDb = 1.0f;
            model.deviationDb = 2.0;
            GlazeCalibrator calibrator;

            const auto capped = runCalibration (calibrator, model, 40.0, fixedBlocks (512));
            expect (capped.result.valid && capped.result.partial);
            expectWithinAbsoluteError (capped.result.gainBiasDb, 1.0f, 1.0e-5f, "held at the upper limit");
            expectWithinAbsoluteError (capped.progress.lastRoundErrorDb, 1.0f, 0.01f);

            auto changing = model;
            changing.offsetDb = [] (std::int64_t end) { return end >= (std::int64_t) (14.0 * testRate) ? 2.8 : 0.0; };
            const auto backDown = runCalibration (calibrator, changing, 40.0, fixedBlocks (512));
            expect (backDown.result.valid && backDown.result.partial);
            expectWithinAbsoluteError (backDown.result.gainBiasDb, -0.8f, 0.01f, "at the upper limit it may still go down");

            auto lower = model;
            lower.deviationDb = -2.0;
            const auto floor = runCalibration (calibrator, lower, 40.0, fixedBlocks (512));
            expect (floor.result.valid && floor.result.partial);
            expectWithinAbsoluteError (floor.result.gainBiasDb, -1.0f, 1.0e-5f, "never below the lower limit");
        }

        // 23. 回馈用实际执行的控制量：执行器最多只能执行 +0.6 dB（例如 Gain 已到头），建议 +1.5 → 实际停在 0.6。
        //     校准器核对出「向上饱和」，把建议改回 0.6（不继续累积），第二轮按 0.6 算：仍差 +0.9 → 结束在 0.6；
        //     三轮常数下也在第二轮就结束（往上已推不动）；音乐变得更吃增益时，从 0.6 往下修到 −0.5，三轮下第三轮验证通过（有效）。
        // 依据：§6「回馈用实际执行的控制量（钳位、叠加手动之后）」「防积分饱和按方向」。
        void testCalibratorUsesExecutedBias()
        {
            beginTest ("calibrator: feedback uses the executed bias, a saturated actuator is not wound up");

            CalModel model;
            model.deviationDb = 1.5;
            model.actuatorMax = 0.6f;
            GlazeCalibrator calibrator;

            const auto run = runCalibration (calibrator, model, 40.0, fixedBlocks (512));
            expect (run.result.valid && run.result.partial);
            expect (run.progress.blockedUp && ! run.progress.blockedDown);
            expectWithinAbsoluteError (run.result.gainBiasDb, 0.6f, 1.0e-5f);

            const auto threeRounds = runCalibration (calibrator, model, 60.0, fixedBlocks (512), 3);
            expect (threeRounds.result.valid && threeRounds.result.partial);
            expectEquals (threeRounds.progress.round, 2, "no third round when nothing can change");
            expectWithinAbsoluteError (threeRounds.result.gainBiasDb, 0.6f, 1.0e-5f);

            auto changing = model;
            changing.offsetDb = [] (std::int64_t end) { return end >= (std::int64_t) (14.0 * testRate) ? 2.0 : 0.0; };
            const auto downTwo = runCalibration (calibrator, changing, 40.0, fixedBlocks (512));
            expect (downTwo.result.valid && downTwo.result.partial);
            expectWithinAbsoluteError (downTwo.result.gainBiasDb, -0.5f, 0.01f, "blocked upwards, still free downwards");

            const auto downThree = runCalibration (calibrator, changing, 60.0, fixedBlocks (512), 3);
            expect (downThree.result.valid && ! downThree.result.partial, "third round verifies the executed-based correction");
            expectWithinAbsoluteError (downThree.result.gainBiasDb, -0.5f, 0.01f);
        }

        // 24. 预算受限：保护层从 5 s 起一直介入 → 不采集，累计 10 s 有效音乐后结束为 budgetLimited（有效、偏差 min(已施加, 0) = 0）；
        //     第二轮（已施加 +1.3）时保护层长时间介入 → 同样结束，正向修正不保留（0）。
        // 依据：§6「保护层持续触发时结束为预算受限，不无限等待」。
        void testCalibratorBudgetLimited()
        {
            beginTest ("calibrator: a persistently engaged guard ends the calibration as budget-limited");

            CalModel model;
            model.guardActive = [] (std::int64_t t) { return t >= (std::int64_t) (5.0 * testRate); };
            GlazeCalibrator calibrator;

            const auto early = runCalibration (calibrator, model, 40.0, fixedBlocks (512));
            expect (early.result.valid && early.result.budgetLimited && ! early.result.partial);
            expectEquals (early.result.gainBiasDb, 0.0f);
            expect (early.finishedAtSeconds > 14.5 && early.finishedAtSeconds < 15.6, "10 s of music with the guard engaged");

            auto late = model;
            late.guardActive = [] (std::int64_t t) { return t >= (std::int64_t) (20.0 * testRate) && t < (std::int64_t) (34.0 * testRate); };
            const auto second = runCalibration (calibrator, late, 45.0, fixedBlocks (512));
            expect (second.result.valid && second.result.budgetLimited);
            expectEquals (second.result.gainBiasDb, 0.0f, "no positive correction is kept when budget-limited");
        }

        // 25. 静音与短暂的保护介入只让采集暂停：入口 −90 LUFS 5 s（门限以下，不推进采集）→ 结束晚约 5 s、结果不变；
        //     保护层介入 0.5 s → 子窗作废、等屏障后继续，结果不变，也不会结束为预算受限。
        // 依据：§6（块选择口径：两侧都在绝对门限以上；中断只作废当前子窗）。
        void testCalibratorSilenceAndGuardPauses()
        {
            beginTest ("calibrator: silence and a brief guard episode only pause the collection");

            GlazeCalibrator calibrator;
            const auto plain = runCalibration (calibrator, CalModel {}, 45.0, fixedBlocks (512));

            CalModel quiet;
            quiet.inputLufs = [] (std::int64_t end)
            {
                return (end >= (std::int64_t) (6.0 * testRate) && end < (std::int64_t) (11.0 * testRate)) ? -90.0 : -14.0;
            };

            const auto withSilence = runCalibration (calibrator, quiet, 45.0, fixedBlocks (512));
            expect (withSilence.result.valid && ! withSilence.result.partial);
            expectWithinAbsoluteError (withSilence.result.gainBiasDb, 1.3f, 0.001f);
            expectWithinAbsoluteError (withSilence.finishedAtSeconds - plain.finishedAtSeconds, 5.0, 0.3, "silence does not count");

            CalModel brief;
            brief.guardActive = [] (std::int64_t t) { return t >= (std::int64_t) (6.0 * testRate) && t < (std::int64_t) (6.5 * testRate); };

            const auto withGuard = runCalibration (calibrator, brief, 45.0, fixedBlocks (512));
            expect (withGuard.result.valid && ! withGuard.result.partial && ! withGuard.result.budgetLimited);
            expectWithinAbsoluteError (withGuard.result.gainBiasDb, 1.3f, 0.001f);
            expectGreaterThan (withGuard.finishedAtSeconds, plain.finishedAtSeconds);
            expectLessThan (withGuard.progress.guardSeconds, 1.0);
        }

        // 26. 各子窗修正方向相反：链的净提升随段落 ±1.2 dB 交替（每 5 s）→ 单一偏差对不齐 → partial（有效、保守、|偏差| 小）。
        // 依据：§6「各子窗修正方向相反，就接受保守、未完全校准的结果」。
        void testCalibratorOppositeSubWindows()
        {
            beginTest ("calibrator: sub-windows disagreeing in direction end as partial");

            CalModel model;
            model.deviationDb = 0.0;
            model.offsetDb = [] (std::int64_t end) { return ((end / (std::int64_t) (5.0 * testRate)) % 2 == 0) ? 1.2 : -1.2; };

            GlazeCalibrator calibrator;
            const auto run = runCalibration (calibrator, model, 45.0, fixedBlocks (512));

            logMessage ("  alternating +/-1.2 dB: bias " + juce::String (run.result.gainBiasDb, 3) + " dB, rounds "
                        + juce::String (run.progress.round) + ", last e " + juce::String (run.progress.lastRoundErrorDb, 3));

            expect (run.state == GlazeCalibrator::State::finished);
            expect (run.result.valid && run.result.partial);
            expectLessThan (std::abs (run.result.gainBiasDb), 0.5f);
        }

        // 27. 参考条件逐项比较：完全相同 → 一致；浮点表示误差（1e-5）→ 仍一致；画像、风格、GLAZE、手动参数哈希、Ceiling、Lookahead、
        //     采样率任一改变 → 失效；NaN → 失效。
        // 依据：§6「校准结果绑定参考条件……任一改变即失效」。
        void testCalibratorReferenceMatching()
        {
            beginTest ("calibrator: reference conditions are compared item by item");

            glaze::CalibrationReference base;
            base.profileId = 42;
            base.style = glaze::Style::warm;
            base.glazePercent = 60.0f;
            base.manualParamsHash = 0xabcdu;
            base.ceilingDb = -1.0f;
            base.lookaheadMs = 2.0f;
            base.sampleRate = 48000.0;

            expect (GlazeCalibrator::referenceMatches (base, base));

            auto jitter = base;
            jitter.glazePercent = 60.00001f;
            jitter.ceilingDb = -1.00001f;
            expect (GlazeCalibrator::referenceMatches (base, jitter), "float representation noise is not a change");

            std::vector<std::pair<const char*, std::function<void (glaze::CalibrationReference&)>>> changes {
                { "profile",   [] (glaze::CalibrationReference& r) { r.profileId += 1; } },
                { "style",     [] (glaze::CalibrationReference& r) { r.style = glaze::Style::clean; } },
                { "glaze",     [] (glaze::CalibrationReference& r) { r.glazePercent = 60.5f; } },
                { "manual",    [] (glaze::CalibrationReference& r) { r.manualParamsHash += 1; } },
                { "ceiling",   [] (glaze::CalibrationReference& r) { r.ceilingDb = -0.9f; } },
                { "lookahead", [] (glaze::CalibrationReference& r) { r.lookaheadMs = 3.0f; } },
                { "rate",      [] (glaze::CalibrationReference& r) { r.sampleRate = 44100.0; } },
                { "nan",       [] (glaze::CalibrationReference& r) { r.glazePercent = std::numeric_limits<float>::quiet_NaN(); } },
            };

            for (const auto& change : changes)
            {
                auto other = base;
                change.second (other);
                expect (! GlazeCalibrator::referenceMatches (base, other), change.first);
                expect (! GlazeCalibrator::referenceMatches (other, base), change.first);
            }
        }

        // 27b（T-026，BUG-004）. 新口径的参考条件：两份都是新口径 → 离散哈希 + 三个对数刻度值按相对 1e-4 比，旧口径哈希不看
        //     （往返漂移让它翻转也仍一致）；有一份是旧口径（旧存档读回、没能升格）→ 只比旧口径哈希。非有限值：两边都非有限才一致。
        // 依据：BUG-004、ADR-018；ParameterRanges.h（往返漂移最多约 1.2e-5，显示精度最小一格 1e-3）。
        void testCalibratorReferenceMatchingContinuousConditions()
        {
            beginTest ("calibrator: log-scale values compare within a relative 1e-4, the old exact hash only against legacy references");

            glaze::CalibrationReference base;
            base.profileId = 42;
            base.style = glaze::Style::warm;
            base.glazePercent = 60.0f;
            base.ceilingDb = -1.0f;
            base.lookaheadMs = 2.0f;
            base.sampleRate = 48000.0;
            base.conditionsFormat = glaze::continuousConditionsFormat;
            base.manualParamsHash = 0xabcdu;
            base.discreteParamsHash = 0x1234u;
            base.limitReleaseMs = 300.0f;
            base.clayLowCutHz = 0.0f;
            base.clayMonoHz = 120.0f;

            expect (GlazeCalibrator::referenceMatches (base, base));

            auto drifted = base;
            drifted.manualParamsHash = 0xabceu;                   // 旧口径哈希翻了（BUG-004 的现象）
            drifted.limitReleaseMs = 300.0f * (1.0f + 5.0e-5f);   // 远大于往返漂移，仍在 1e-4 之内
            drifted.clayMonoHz = std::nextafter (120.0f, 200.0f);
            expect (GlazeCalibrator::referenceMatches (base, drifted) && GlazeCalibrator::referenceMatches (drifted, base),
                    "drift within a relative 1e-4 and a flipped old hash are not a change");

            const auto nan = std::numeric_limits<float>::quiet_NaN();
            std::vector<std::pair<const char*, std::function<void (glaze::CalibrationReference&)>>> changes {
                { "discrete",            [] (glaze::CalibrationReference& r) { r.discreteParamsHash += 1; } },
                { "release +2e-4",       [] (glaze::CalibrationReference& r) { r.limitReleaseMs *= 1.0f + 2.0e-4f; } },
                { "release one display step", [] (glaze::CalibrationReference& r) { r.limitReleaseMs += 1.0f; } },
                { "low cut counted",     [] (glaze::CalibrationReference& r) { r.clayLowCutHz = 25.0f; } },
                { "mono -2e-4",          [] (glaze::CalibrationReference& r) { r.clayMonoHz *= 1.0f - 2.0e-4f; } },
                { "release NaN",         [nan] (glaze::CalibrationReference& r) { r.limitReleaseMs = nan; } },
                { "legacy, other hash",  [] (glaze::CalibrationReference& r) { r.conditionsFormat = glaze::legacyConditionsFormat;
                                                                               r.manualParamsHash += 1; } },
            };

            for (const auto& change : changes)
            {
                auto other = base;
                change.second (other);
                expect (! GlazeCalibrator::referenceMatches (base, other), change.first);
                expect (! GlazeCalibrator::referenceMatches (other, base), change.first);
            }

            // 有一份是旧口径：只比旧口径哈希（新口径的项不看）。
            auto legacy = base;
            legacy.conditionsFormat = glaze::legacyConditionsFormat;
            legacy.discreteParamsHash = 0;
            legacy.limitReleaseMs = legacy.clayMonoHz = 0.0f;
            expect (GlazeCalibrator::referenceMatches (base, legacy) && GlazeCalibrator::referenceMatches (legacy, base),
                    "legacy against new: the same old hash matches");
            expect (! GlazeCalibrator::referenceMatches (drifted, legacy), "legacy against new: a flipped old hash does not");

            // 非有限值：两边都非有限才一致（沿用旧口径哈希里 NaN / ±Inf 同一记号的口径）。
            auto nanA = base, nanB = base;
            nanA.limitReleaseMs = nan;
            nanB.limitReleaseMs = std::numeric_limits<float>::infinity();
            expect (GlazeCalibrator::referenceMatches (nanA, nanB), "non-finite on both sides: the same condition");
        }

        // 28. 坏输入：目标不是有限值 → 立即结束、无效、偏差 0；出口子块出现 NaN（当作中断）→ 结果仍有限且有效；
        //     begin 之前 process 什么都不做；reset 回到 idle、结果清空。
        // 依据：实现计划「不删 NaN 保护」；GlazeCalibrator.h（begin / reset 的约定）。
        void testCalibratorBadInputs()
        {
            beginTest ("calibrator: bad inputs never produce NaN, non-finite targets end immediately as invalid");

            GlazeCalibrator calibrator;
            calibrator.prepare (testRate, 512);
            expect (calibrator.getState() == GlazeCalibrator::State::idle);

            calibrator.process (nullptr, 0, nullptr, 0, std::numeric_limits<float>::quiet_NaN(), true, 512);
            expect (calibrator.getState() == GlazeCalibrator::State::idle, "process before begin does nothing");

            calibrator.begin (std::numeric_limits<float>::quiet_NaN(), 3.0f, {}, 100);
            expect (calibrator.getState() == GlazeCalibrator::State::finished);
            expect (! calibrator.getResult().valid);
            expectEquals (calibrator.getResult().gainBiasDb, 0.0f);

            CalModel model;
            model.corruptOutputDb = [] (std::int64_t end)
            {
                return (end >= (std::int64_t) (8.0 * testRate) && end < (std::int64_t) (8.5 * testRate))
                           ? std::numeric_limits<double>::quiet_NaN() : 0.0;
            };

            const auto run = runCalibration (calibrator, model, 45.0, fixedBlocks (512));
            expect (std::isfinite (run.result.gainBiasDb));
            expect (run.result.valid && ! run.result.partial);
            expectWithinAbsoluteError (run.result.gainBiasDb, 1.3f, 0.001f);

            calibrator.reset();
            expect (calibrator.getState() == GlazeCalibrator::State::idle);
            expect (! calibrator.getResult().valid);
        }

        // 29. begin / process / invalidateCurrentWindow / 读结果与进度不经 operator new 分配（完整跑两轮，含中断与施加斜坡）。
        // 依据：实现计划「音频线程 0 分配」。
        void testCalibratorDoesNotAllocate()
        {
            beginTest ("calibrator does not allocate");

            std::uint64_t allocations = 0;
            CalModel model;
            model.allocationsInCalls = &allocations;
            model.beforeBlock = [] (std::int64_t t, GlazeCalibrator& calibrator)
            {
                if (t / 512 == (std::int64_t) (8.0 * testRate) / 512)
                    calibrator.invalidateCurrentWindow();
            };

            GlazeCalibrator calibrator;
            const auto run = runCalibration (calibrator, model, 45.0, fixedBlocks (512));

            expect (run.result.valid);
            expectEquals ((juce::int64) allocations, (juce::int64) 0);
        }

        // 30. 校准器与保护层按推荐方式接在一起（guardActive = guard.isEngaged()）：链比目标少 2.5 dB，Limit 3 s 均值 GR = 1.5 + 偏差 + g_保护
        //     （上限 3 dB）。第一轮建议 +2.5 → GR 超限 → 保护层退让并停在上限附近 → 校准器不采集（不把退让当偏差补回去）、
        //     累计 10 s 后结束为预算受限、偏差回 0 → GR 回到安全区，保护层 5 s 后恢复到 0。全程建议从不超过 +2.5（没有与保护层互相顶）。
        // 依据：§6「保护层改了实际增益：当前子窗作废」「保护层持续触发时结束为预算受限」；§7「G_实际 = G_参考 + g_保护」。
        void testCalibratorAndGuardDoNotFight()
        {
            beginTest ("calibrator and guard wired together: no tug of war, calibration ends budget-limited, guard recovers");

            GlazeGuard guard;
            guard.prepare (testRate);
            guard.setLimits (limitMeanCap (3.0f), guardConstants());

            GlazeCalibrator calibrator;
            calibrator.prepare (testRate, statsBlock);
            calibrator.setConstants (calConstants());
            glaze::CalibrationReference reference;
            reference.sampleRate = testRate;
            calibrator.begin (6.0f, 3.0f, reference, 250);

            std::vector<LoudnessMeter::SubBlock> ins, outs;
            float applied = 0.0f, highestSuggestion = -10.0f, lowestGuard = 0.0f;
            double finishedAt = -1.0;
            std::int64_t t = 0;
            const auto inEnergy = lufsToMeanSquare (-14.0);

            for (; t < (std::int64_t) (60.0 * testRate); t += statsBlock)
            {
                const auto g = guard.getProtectionDb();   // 本块施加的 g_保护

                ModuleStats stats = limitMeanStats (juce::jmax (0.0f, 1.5f + applied + g));
                stats.numSamples = statsBlock;
                guard.process (stats);

                ins.clear();
                outs.clear();

                for (auto k = t / subBlockSamples + 1; k * subBlockSamples - 1 < t + statsBlock; ++k)
                {
                    const auto end = k * subBlockSamples - 1;

                    if (end < t)
                        continue;

                    ins.push_back ({ inEnergy, 0.0f, end });
                    outs.push_back ({ inEnergy * std::pow (10.0, (6.0 - 2.5 + (double) applied + (double) g) / 10.0), 0.0f, end });
                }

                calibrator.process (ins.data(), (int) ins.size(), outs.data(), (int) outs.size(), applied, guard.isEngaged(), statsBlock);

                if (calibrator.getState() == GlazeCalibrator::State::finished && finishedAt < 0.0)
                    finishedAt = (double) (t + statsBlock) / testRate;

                const auto wanted = calibrator.getResult().gainBiasDb;
                highestSuggestion = juce::jmax (highestSuggestion, wanted);
                lowestGuard = juce::jmin (lowestGuard, guard.getProtectionDb());

                const auto step = (float) (2.0 * statsBlock / testRate);
                applied = wanted > applied ? juce::jmin (wanted, applied + step) : juce::jmax (wanted, applied - step);
            }

            const auto& result = calibrator.getResult();
            logMessage ("  finished at " + juce::String (finishedAt, 2) + " s, lowest guard " + juce::String (lowestGuard, 3)
                        + " dB, guard now " + juce::String (guard.getProtectionDb(), 3) + " dB");

            expect (result.valid && result.budgetLimited);
            expectEquals (result.gainBiasDb, 0.0f);
            expectLessOrEqual (highestSuggestion, 2.5f + 0.01f, "never tried to undo the guard's retreat");
            expect (lowestGuard < -0.9f && lowestGuard > -1.6f, "the guard retreated only as far as needed");
            expectEquals (guard.getProtectionDb(), 0.0f, "and came back once the correction was dropped");
            expect (finishedAt > 20.0 && finishedAt < 32.0);
        }
    };

    static GlazeCalibrationTests glazeCalibrationTests;
}
