/*
  ==============================================================================

    Phase2OrchestratorTests.cpp
    T-048 阶段 2 · 测试代理 D。连续编排器的契约（Phase2Contract.h）加上生产代码里已经能核对的几条：
    makePlan 仍是纯函数、空画像走低置信度、lockedWeights 只是慢状态输入、收单代价停算后方案仍不收单、
    HouseConstants 的锁定 / 重开字段留着（Q12）、软遗忘沿用阶段 1 的 6 LU / 20 s / ×0.2 再加起伏 ≤ 2 LU（Q5）。

    不覆盖：仍要等代理 A/B 删掉的 GlazeController / MusicAnalyzer 学习路径（那些用例还在各自的文件里，
    因为生产符号还在；合入时按本文件的契约改驱动，不要把 phase / calibrationValid 加回来）。

  ==============================================================================
*/

#include "Phase2Contract.h"
#include "TestHelpers.h"

#include "../Source/dsp/GlazeConstants.h"
#include "../Source/dsp/GlazePlanner.h"

#include <cmath>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::PlanInputs;
        using glaze::Style;

        glaze::Feature known (float value)
        {
            glaze::Feature feature;
            feature.value = value;
            feature.valid = true;
            feature.effectiveSeconds = 30.0;
            feature.confidence = 1.0f;
            return feature;
        }
    }

    class Phase2OrchestratorTests  : public juce::UnitTest
    {
    public:
        Phase2OrchestratorTests() : juce::UnitTest ("Phase2Orchestrator", "Celadon") {}

        void runTest() override
        {
            testRatesRatchetAndTrim();
            testGuardTakesTheMostNegative();
            testOutputLevelGateIgnoresHostReset();
            testOneSecondGridAndModelSwitch();
            testLegacyStateDropsListenNodes();
            testPlannerReadsLiveProfileNotALock();
            testHouseLockFieldsStayAndSoftForgetStays();
        }

    private:
        void testRatesRatchetAndTrim()
        {
            beginTest ("Q7/Q8: gain rises at 0.2 dB/s, falls at 1 dB/s, deadband 0.1/0.3; ratchet blocks rises only");

            phase2::GainServo rising;
            rising.stepGain (2.0f, 1.0f, 1.0f);
            expectWithinAbsoluteError (rising.appliedDb, 0.2f, 1.0e-5f);

            phase2::GainServo falling;
            falling.appliedDb = 2.0f;
            falling.stepGain (0.0f, 1.0f, 1.0f);
            expectWithinAbsoluteError (falling.appliedDb, 1.0f, 1.0e-5f);

            phase2::GainServo held;
            held.appliedDb = 1.0f;
            held.stepGain (1.05f, 1.0f, 1.0f);
            expect (held.inDeadband);
            expectWithinAbsoluteError (held.appliedDb, 1.0f, 1.0e-5f);
            held.stepGain (1.4f, 1.0f, 1.0f);
            expect (! held.inDeadband);
            expectWithinAbsoluteError (held.appliedDb, 1.2f, 1.0e-5f);

            phase2::GainServo ratchet;
            ratchet.stepGain (4.0f, 10.0f, 0.4f);
            expectWithinAbsoluteError (ratchet.appliedDb, 0.0f, 1.0e-5f, "m < 0.5: net gain must not rise");
            ratchet.appliedDb = 1.5f;
            ratchet.stepGain (-1.0f, 1.0f, 0.2f);
            expectWithinAbsoluteError (ratchet.appliedDb, 0.5f, 1.0e-5f, "the same ratchet still allows a fall at 1 dB/s");

            phase2::GainServo glue;
            glue.stepToward (3.0f, 1.0f, phase2::gainUpDbPerSecond, phase2::gainDownDbPerSecond);
            expectWithinAbsoluteError (glue.appliedDb, 0.2f, 1.0e-5f, "Glue / Heat / Clip / Tone are not ratcheted");

            phase2::TrimIntegrator trim;
            trim.step (10.0f, 1.0f, false);
            expectWithinAbsoluteError (trim.trimDb, phase2::trimMaxDbPerSecond, 1.0e-5f);
            trim.step (10.0f, 1.0f, true);
            expectWithinAbsoluteError (trim.trimDb, phase2::trimMaxDbPerSecond, 1.0e-5f, "budget retreat freezes the integrator");

            for (int i = 0; i < 200; ++i)
                trim.step (10.0f, 1.0f, false);

            expectWithinAbsoluteError (trim.trimDb, phase2::trimClampDb, 1.0e-4f);
            trim.step (-100.0f, 1000.0f, false);
            expectWithinAbsoluteError (trim.trimDb, -phase2::trimClampDb, 1.0e-4f);
        }

        void testGuardTakesTheMostNegative()
        {
            beginTest ("Q8: the three guards plus PSR contribute the most negative relief, and PSR stays its own cause");

            const auto quiet = phase2::minGuard ({ { 0.0f, phase2::GuardCause::budget, false },
                                                   { 0.0f, phase2::GuardCause::contrast, false },
                                                   { 0.0f, phase2::GuardCause::psr, false } });
            expectEquals (quiet.reliefDb, 0.0f);
            expect (! quiet.engaged);

            const auto contrast = phase2::minGuard ({ { -0.4f, phase2::GuardCause::budget, true },
                                                      { -1.2f, phase2::GuardCause::contrast, true },
                                                      { -0.2f, phase2::GuardCause::deliveryPeak, false } });
            expectWithinAbsoluteError (contrast.reliefDb, -1.2f, 1.0e-5f);
            expect (contrast.cause == phase2::GuardCause::contrast && contrast.engaged);

            const auto psr = phase2::minGuard ({ { 0.0f, phase2::GuardCause::budget, false },
                                                 { -0.5f, phase2::GuardCause::psr, true } });
            expect (psr.cause == phase2::GuardCause::psr);
            expectWithinAbsoluteError (psr.reliefDb, -0.5f, 1.0e-5f);

            const auto floored = phase2::minGuard ({ { 0.0f, phase2::GuardCause::budget, false },
                                                     { 0.0f, phase2::GuardCause::psr, true } });
            expect (floored.engaged && floored.cause == phase2::GuardCause::none);
            expectEquals (floored.reliefDb, 0.0f);

            const auto tied = phase2::minGuard ({ { -0.4f, phase2::GuardCause::budget, true },
                                                   { -0.4f, phase2::GuardCause::contrast, true } });
            expect (tied.engaged && tied.cause == phase2::GuardCause::budget);
            expectWithinAbsoluteError (tied.reliefDb, -0.4f, 1.0e-5f);

            const auto recovering = phase2::minGuard ({ { -2.0f, phase2::GuardCause::budget, false },
                                                        { -0.4f, phase2::GuardCause::contrast, true } });
            expect (recovering.engaged && recovering.cause == phase2::GuardCause::contrast);
            expectWithinAbsoluteError (recovering.reliefDb, -0.4f, 1.0e-5f);

            const auto clamped = phase2::minGuard ({ { 1.5f, phase2::GuardCause::budget, false } });
            expectEquals (clamped.reliefDb, 0.0f, "relief is never positive");
        }

        void testOutputLevelGateIgnoresHostReset()
        {
            beginTest ("Q9: output main-section level ignores m < 0.5 and host reset, and clears only on cold start");

            phase2::OutputLevelTracker tracker;
            tracker.prepare (48000.0);
            const float quiet[] = { 0.1f, 0.1f, 0.1f, 0.1f };
            tracker.noteMainness (0.2f);
            tracker.processOutput (quiet, 4);
            expect (! tracker.hasLevel());

            tracker.noteMainness (0.8f);
            tracker.processOutput (quiet, 4);
            expect (tracker.hasLevel());
            const auto level = tracker.mainLevelLufs();
            tracker.hostReset();
            expect (tracker.hasLevel());
            expectWithinAbsoluteError (tracker.mainLevelLufs(), level, 1.0e-5f);
            tracker.clear();
            expect (! tracker.hasLevel());
        }

        void testOneSecondGridAndModelSwitch()
        {
            beginTest ("Q3/Q4: the 1 s grid follows the analyzer clock, and a genre model waits for the next slice");

            expect (phase2::samplesPerGrid (48000.0) == 48000);
            expect (! phase2::crossedOneSecondGrid (0, 47999, 48000.0));
            expect (phase2::crossedOneSecondGrid (47999, 48000, 48000.0));

            std::int64_t cursor = 0;
            auto crossings = 0;

            for (int block = 0; block < 200; ++block)
            {
                const auto next = cursor + 333;

                if (phase2::crossedOneSecondGrid (cursor, next, 48000.0))
                    ++crossings;

                cursor = next;
            }

            expectEquals (crossings, 1, "a 333-sample block still crosses the 1 s grid once in 200 blocks");

            phase2::ModelSwitch model;
            model.request (2);
            expect (model.waiting());
            expectEquals (model.published, 0);
            model.advanceSlice();
            expect (! model.waiting());
            expectEquals (model.published, 2);
        }

        void testLegacyStateDropsListenNodes()
        {
            beginTest ("Q1/Q10/Q11: v1 listen nodes cold-start with a session notice; v2 LIVE keeps a diagnostic constants version only");

            juce::XmlElement legacy { "GLAZE_STATE" };
            legacy.setAttribute ("formatVersion", 1);
            legacy.createNewChildElement ("GLAZE_PROFILE");
            legacy.createNewChildElement ("PLAN");
            legacy.createNewChildElement ("CALIBRATION");
            legacy.createNewChildElement ("EXECUTION");
            legacy.createNewChildElement ("GLAZE_CONSTANTS");

            const auto opened = phase2::interpretState (legacy);
            expect (opened.coldStart && opened.sessionNotice && opened.planUsesFactoryConstants);
            expectEquals (juce::String::fromUTF8 (phase2::legacySessionNoticeZh), utf8 ("旧的聆听结果已丢弃，声音会变"));

            const auto saved = phase2::saveLiveTree (glaze::currentAlgorithmVersion);
            expect (saved != nullptr);
            expect (! phase2::treeWritesLegacyNotice (*saved));
            expect (saved->getStringAttribute ("formatVersion") == "2");
            const auto* live = saved->getChildByName ("LIVE");
            expect (live != nullptr);
            expect (live->getStringAttribute ("compressed") == "1");
            expect (live->getIntAttribute ("constantsVersion") == glaze::currentAlgorithmVersion);
            expect (live->getChildByName ("LIVE_PROFILE") != nullptr);
            expect (live->getChildByName ("CONTROL") != nullptr);
            expect (live->getChildByName ("BUDGET_GUARD") != nullptr);
            expect (live->getChildByName ("CONTRAST_GUARD") != nullptr);
            expect (live->getChildByName ("PSR_GUARD") != nullptr);
            expect (live->getChildByName ("OUTPUT_LEVEL") != nullptr);
            expect (live->getChildByName ("CLAY") != nullptr);
            expect (! phase2::interpretState (*saved).coldStart);
            expect (! phase2::interpretState (*saved).sessionNotice);

            juce::XmlElement newer { "GLAZE_STATE" };
            newer.setAttribute ("formatVersion", 3);
            newer.createNewChildElement ("LIVE");
            const auto future = phase2::interpretState (newer);
            expect (future.coldStart && ! future.sessionNotice);

            juce::XmlElement constantsOnly { "GLAZE_STATE" };
            constantsOnly.setAttribute ("formatVersion", 2);
            auto* liveWithConstants = constantsOnly.createNewChildElement ("LIVE");
            liveWithConstants->setAttribute ("constantsVersion", 4);
            liveWithConstants->createNewChildElement ("GLAZE_CONSTANTS");
            const auto ignored = phase2::interpretState (constantsOnly);
            expect (! ignored.coldStart && ignored.planUsesFactoryConstants);
            expectEquals (ignored.constantsVersion, 4);
        }

        void testPlannerReadsLiveProfileNotALock()
        {
            beginTest ("planner: null profile is finite, lockedWeights are an input, invalid mono costs stay neutral, caps unchanged");

            const auto& factory = glaze::factoryConstants();
            expectWithinAbsoluteError (factory.clayCostFullDb, 0.2f, 1.0e-6f);
            expectWithinAbsoluteError (factory.clayCostZeroDb, 0.6f, 1.0e-6f);
            expectWithinAbsoluteError (factory.clayBlendLossLimitDb, 0.09f, 1.0e-6f);

            PlanInputs empty;
            empty.glazePercent = 70.0f;
            const auto conservative = glaze::makePlan (empty);
            expect (std::isfinite (conservative.effectiveGainDb) && std::isfinite (conservative.effectiveTargetLufs));
            expectEquals (conservative.clay.monoAmount, 0.0f);
            expectWithinAbsoluteError (glaze::confidenceScale (nullptr, factory), factory.unlearnedStrength, 1.0e-5f);

            glaze::MusicProfile profile;
            profile.integratedLufs = known (-14.0f);
            profile.truePeakDbtp = known (-3.0f);
            profile.representativeness = 1.0f;
            profile.monoPeakCostDb[0].valid = false;
            profile.monoPeakCostDb[1].valid = false;
            profile.monoPeakCostDb[2].valid = false;
            profile.monoPeakCostDb[3].valid = false;

            PlanInputs trusted;
            trusted.profile = &profile;
            trusted.glazePercent = 100.0f;
            const auto full = glaze::makePlan (trusted);
            expectEquals (full.clay.monoAmount, 0.0f);
            expectWithinAbsoluteError (full.clay.monoHz, 80.0f, 1.0e-4f);

            profile.representativeness = 0.0f;
            const auto low = glaze::confidenceScale (&profile, factory);
            profile.representativeness = 1.0f;
            const auto high = glaze::confidenceScale (&profile, factory);
            expect (low < high);

            glaze::StyleWeights warm;
            warm.warm = 1.0f;
            warm.clean = 0.0f;
            glaze::StyleWeights clean;
            clean.clean = 1.0f;
            PlanInputs weighted = trusted;
            weighted.style = Style::automatic;
            weighted.lockedWeights = &warm;
            const auto warmPlan = glaze::makePlan (weighted);
            weighted.lockedWeights = &clean;
            const auto cleanPlan = glaze::makePlan (weighted);
            expect (warmPlan.weights.warm != cleanPlan.weights.warm);
            expectWithinAbsoluteError (warmPlan.weights.warm + warmPlan.weights.punchy + warmPlan.weights.clean, 1.0f, 1.0e-4f);
        }

        void testHouseLockFieldsStayAndSoftForgetStays()
        {
            beginTest ("Q5/Q12: House lock fields stay at the factory values, and soft-forget keeps 6 LU / 20 s / 0.2 / 2 LU");

            const auto& house = glaze::factoryConstants().house;
            expectEquals (house.lockSegments, 64);
            expectEquals (house.requiredFullSegments, 8);
            expectWithinAbsoluteError (house.fastTransitionSeconds, 0.5, 1.0e-9);
            expectWithinAbsoluteError (house.reopenAboveProfileLu, 3.0f, 1.0e-6f);
            expectEquals (house.reopenSegments, 4);
            expectEquals (house.maxReopens, 2);

            const glaze::LiveConstants live;
            expectWithinAbsoluteError (live.newProgramLu, 6.0f, 1.0e-6f);
            expectWithinAbsoluteError (live.newProgramSeconds, 20.0, 1.0e-9);
            expectWithinAbsoluteError (live.newProgramKeep, 0.2f, 1.0e-6f);
            expectWithinAbsoluteError (live.newProgramRangeLu, 2.0f, 1.0e-6f);
            expectWithinAbsoluteError (live.mainThreshold, 0.5f, 1.0e-6f);
        }
    };

    static Phase2OrchestratorTests phase2OrchestratorTests;
}
