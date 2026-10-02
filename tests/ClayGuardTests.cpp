/*
  ==============================================================================

    ClayGuardTests.cpp
    v0.1.7.0 修坯 Clay 线 A2 的测试（04-协调/2026-09-26-v0.1.7.0-修坯实现计划；设计规格-Clay-分析决策与验收 第 3 节、第 6 节第 5 条）：
      - ClayGuard 反相提示（T-012 阶段 4 按 ADR-012 / 设计规格-Clay-根治修订 3.4 写；ADR-015 起只提示、不改收单）：触发 = 慢路 ρ < −0.2，
        或（慢路 < 门且快路 < −0.2）；条件须连续成立 0.15 s（资格期：瞬态不触发、条件一断清零、冻结时保持、setState / reset / 不活跃后
        从 0 起算）；触发后 100 ms 内提示强度 phaseFloor 到 1（从判定起 0.25 s）、条件消失后保持 2 s、再 2 s 线性退回 0；保持期 / 释放中
        再触发；phaseActive = phaseFloor > 0；输出的 monoAmount 始终 = 方案值；
      - 宽度保护：实时高频预测相关度将低于 0.25 时 200 ms 内把加宽收到允许值（下限 0，从不收窄）、2 s 放回；
      - 冻结（seconds ≤ 0 / 非有限；视图某一路无效只冻那一路）、active = false 回中性、setState 钳制与恢复后逐位接续；
      - 随机扫描：monoAmount 原样（ADR-015）、只减 widthDb、lowCutHz / monoHz / 代价 mask 原样、状态在合法范围、标志与数值一致（带阳性对照计数）；
      - 块长一致：同一段视图脚本按 17 ～ 8192 样本的块（含随机块长）切分，状态与输出在 float 舍入容差内一致；
      - 常数：自定义阈值 / 快路门 / 最短持续 / 时长 / ρ_min 生效；旧快照缺字段（全 0）与非有限常数不产生非有限值；
        缺省构造 = 出厂常数 = 规格数字；
      - resolveClayParams：27 种三态组合 × GLAZE 0 / 50 × Bypass（含每项三态 = 9 种单项情形）、钳制与非有限、GLAZE NaN / 负、
        纯函数、「手动」「关」不受保护改动；
      - update / setConstants / setState / reset / resolveClayParams（以及构造）不经 operator new 分配。

    只按 ClayGuard.h 的公开接口写；实时视图（ShortTermView）手工构造，不经过分析器。时间按 48 kHz 样本数折算，
    默认 10 ms（480 样本）一块。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ClayGuard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::ClayGuard;
        using glaze::ClayGuardState;
        using glaze::ClayPressureMonitor;
        using glaze::ClayPlan;
        using glaze::ShortTermView;

        constexpr double clayRate = 48000.0;
        constexpr int gridSamples = 480;   // 10 ms 一格（48 kHz）

        // 规格的保护时长与阈值（设计规格-Clay-根治修订 3.4 / ADR-012：慢路 < −0.2，或慢路 < 0.3 且快路 < −0.2；连续成立 0.15 s；
        // 攻击 100 ms、保持 2 s、释放 2 s；宽度攻击 200 ms、释放 2 s；ρ_min 0.25）。与 GlazeConstants.h 的出厂默认值相同
        //（testConstantsAndDefaults 核对）；这里显式写出，下面的时间断言以它们为依据。
        glaze::Constants specConstants()
        {
            glaze::Constants c;
            c.clayGuardPhaseCorrelation = -0.2f;
            c.clayGuardPhaseFastGate = 0.3f;
            c.clayGuardPhaseMinSeconds = 0.15;
            c.clayGuardPhaseAttackSeconds = 0.1;
            c.clayGuardPhaseHoldSeconds = 2.0;
            c.clayGuardPhaseReleaseSeconds = 2.0;
            c.clayGuardWidthAttackSeconds = 0.2;
            c.clayGuardWidthReleaseSeconds = 2.0;
            c.clayMinCorrelationAfterWidth = 0.25f;
            return c;
        }

        // 实时视图：低频相关度慢路（lowValid）与高频 2 ～ 16 kHz 的 P_M / P_S（highValid）；快路默认无效（要用时 withFast）。
        // ClayGuard 只读这几项。
        ShortTermView makeView (float lowCorrelation, bool lowValid, float highMid, float highSide, bool highValid)
        {
            ShortTermView view;
            view.liveLowCorrelation.value = lowCorrelation;
            view.liveLowCorrelation.valid = lowValid;
            view.liveLowCorrelation.effectiveSeconds = 1.0;
            view.liveLowCorrelation.confidence = 1.0f;
            view.liveHighMidPower = highMid;
            view.liveHighSidePower = highSide;
            view.liveHighValid = highValid;
            view.musicActive = true;
            return view;
        }

        // 同一视图再给低频相关度的 120 ms 快路（ADR-012：只在慢路 < clayGuardPhaseFastGate 时参与判定）。
        ShortTermView withFast (ShortTermView view, float fastCorrelation, bool fastValid = true)
        {
            view.liveLowCorrelationFast.value = fastCorrelation;
            view.liveLowCorrelationFast.valid = fastValid;
            view.liveLowCorrelationFast.effectiveSeconds = 1.0;
            view.liveLowCorrelationFast.confidence = 1.0f;
            return view;
        }

        // P_S = 1 时让出厂 ρ_min 0.25（k = 0.6）下的允许加宽恰为 allowedDb 的 P_M：10·log10(0.6 · P_M) = allowedDb。
        float midPowerForAllowedDb (double allowedDb)
        {
            return static_cast<float> (std::pow (10.0, allowedDb / 10.0) / 0.6);
        }

        // 常用视图。高频「窄」：P_M = 1、P_S = 0.01 → 允许 10·log10(60) = 17.8 dB（远高于任何方案）；
        // 「宽」：允许 0.4 dB；「很宽」：P_M = 0.5、P_S = 1 → 0.6 · 0.5 = 0.3 < 1 → 允许 0；「单声道」：P_S = 0 → 不限。
        ShortTermView correlatedNarrow()   { return makeView (0.8f, true, 1.0f, 0.01f, true); }
        ShortTermView invertedNarrow()     { return makeView (-0.6f, true, 1.0f, 0.01f, true); }
        ShortTermView correlatedWide()     { return makeView (0.8f, true, midPowerForAllowedDb (0.4), 1.0f, true); }
        ShortTermView invertedWide()       { return makeView (-0.6f, true, midPowerForAllowedDb (0.4), 1.0f, true); }
        ShortTermView correlatedVeryWide() { return makeView (0.8f, true, 0.5f, 1.0f, true); }
        ShortTermView correlatedMonoHigh() { return makeView (0.8f, true, 1.0f, 0.0f, true); }

        // 「自动」态方案值：四项都不是中性值（lowCutHz / monoHz 用来核对原样透传）。
        ClayPlan plannedAuto()
        {
            ClayPlan p;
            p.lowCutHz = 22.0f;
            p.monoHz = 100.0f;
            p.monoAmount = 0.3f;
            p.widthDb = 1.2f;
            return p;
        }

        std::uint32_t bitsOf (float value)
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &value, sizeof (bits));
            return bits;
        }

        bool sameState (const ClayGuardState& a, const ClayGuardState& b)
        {
            return bitsOf (a.phaseFloor) == bitsOf (b.phaseFloor) && bitsOf (a.phaseHoldSeconds) == bitsOf (b.phaseHoldSeconds)
                && bitsOf (a.widthCapDb) == bitsOf (b.widthCapDb) && a.phaseActive == b.phaseActive && a.widthActive == b.widthActive;
        }

        bool samePlan (const ClayPlan& a, const ClayPlan& b)
        {
            return bitsOf (a.lowCutHz) == bitsOf (b.lowCutHz) && bitsOf (a.monoHz) == bitsOf (b.monoHz)
                && bitsOf (a.monoAmount) == bitsOf (b.monoAmount) && bitsOf (a.widthDb) == bitsOf (b.widthDb)
                && a.costLimitedMask == b.costLimitedMask;
        }

        // 反相一路什么都没做（下限 0、没有保持期、不介入）；宽度一路不管。
        bool phaseIdle (const ClayGuardState& s)
        {
            return s.phaseFloor == 0.0f && s.phaseHoldSeconds == 0.0f && ! s.phaseActive;
        }

        bool sameParams (const ClayModule::Params& a, const ClayModule::Params& b)
        {
            return bitsOf (a.lowCutHz) == bitsOf (b.lowCutHz) && bitsOf (a.monoHz) == bitsOf (b.monoHz)
                && bitsOf (a.monoAmount) == bitsOf (b.monoAmount) && bitsOf (a.widthDb) == bitsOf (b.widthDb)
                && a.bypass == b.bypass;
        }

        juce::String describe (const ClayModule::Params& p)
        {
            return "lowCut " + juce::String (p.lowCutHz, 3) + " monoHz " + juce::String (p.monoHz, 3)
                 + " amount " + juce::String (p.monoAmount, 3) + " width " + juce::String (p.widthDb, 3)
                 + (p.bypass ? " bypass" : "");
        }

        // 每块输出的极值（检查「从不收窄 / 只增只减」在每一块都成立，而不只是段末）。
        struct Extremes
        {
            float minMono = std::numeric_limits<float>::max();
            float maxMono = std::numeric_limits<float>::lowest();
            float minWidth = std::numeric_limits<float>::max();
            float maxWidth = std::numeric_limits<float>::lowest();
            int passThroughMismatches = 0;   // lowCutHz / monoHz 与方案不逐位相同的块数

            void add (const ClayPlan& output, const ClayPlan& planned)
            {
                minMono = juce::jmin (minMono, output.monoAmount);
                maxMono = juce::jmax (maxMono, output.monoAmount);
                minWidth = juce::jmin (minWidth, output.widthDb);
                maxWidth = juce::jmax (maxWidth, output.widthDb);

                if (bitsOf (output.lowCutHz) != bitsOf (planned.lowCutHz) || bitsOf (output.monoHz) != bitsOf (planned.monoHz))
                    ++passThroughMismatches;
            }
        };

        // 同一视图与方案连续跑 seconds 秒（blockSamples 一块，最后一块可短），返回最后一块的输出。
        ClayPlan feed (ClayGuard& guard, const ClayPlan& planned, const ShortTermView& view, double seconds,
                       Extremes* extremes = nullptr, int blockSamples = gridSamples)
        {
            auto result = planned;
            const auto total = static_cast<int> (std::lround (seconds * clayRate));

            for (int done = 0; done < total;)
            {
                const auto length = juce::jmin (blockSamples, total - done);
                result = guard.update (planned, view, static_cast<double> (length) / clayRate, true);
                done += length;

                if (extremes != nullptr)
                    extremes->add (result, planned);
            }

            return result;
        }

        //==============================================================================
        // 块长一致性的脚本：按「音乐时间」分段，每段一种视图与方案；段长是 10 ms 的整数倍（格点在各段里对齐）。
        struct Segment
        {
            int samples = 0;
            ShortTermView view;
            ClayPlan planned;
        };

        struct Trace
        {
            std::vector<ClayGuardState> gridStates;      // 每个 10 ms 格点（只在 cutAtGrid 时记）
            std::vector<ClayPlan> gridOutputs;
            std::vector<ClayGuardState> segmentStates;   // 每段结束时
            std::vector<ClayPlan> segmentOutputs;
        };

        // 块长由 nextBlock 给；块一律在段边界切开（视图在段边界变），cutAtGrid 时还在每个 10 ms 格点切开，
        // 这样同一格点在任何块长下都是一个块边界，可以逐格比较。
        Trace runScript (ClayGuard& guard, const std::vector<Segment>& script, const std::function<int()>& nextBlock, bool cutAtGrid)
        {
            Trace trace;

            for (const auto& segment : script)
            {
                auto output = segment.planned;

                for (int done = 0; done < segment.samples;)
                {
                    auto length = juce::jmin (segment.samples - done, juce::jmax (1, nextBlock()));

                    if (cutAtGrid)
                        length = juce::jmin (length, gridSamples - done % gridSamples);

                    output = guard.update (segment.planned, segment.view, static_cast<double> (length) / clayRate, true);
                    done += length;

                    if (cutAtGrid && done % gridSamples == 0)
                    {
                        trace.gridStates.push_back (guard.getState());
                        trace.gridOutputs.push_back (output);
                    }
                }

                trace.segmentStates.push_back (guard.getState());
                trace.segmentOutputs.push_back (output);
            }

            return trace;
        }

        int samplesFor (double seconds)
        {
            return static_cast<int> (std::lround (seconds * clayRate / static_cast<double> (gridSamples))) * gridSamples;
        }

        // 走遍反相资格期 / 攻击 / 保持 / 释放、瞬态不触发、快路门内触发 / 门外不触发、宽度收回 / 放回 / 方案变化、两路冻结的
        // 11.9 s 脚本（段末状态见 testBlockSizeConsistency）。
        std::vector<Segment> blockScript()
        {
            const auto first = plannedAuto();   // monoAmount 0.3、widthDb 1.2
            auto second = first;
            second.widthDb = 0.8f;

            const auto lowInvalidNarrow = makeView (0.8f, false, 1.0f, 0.01f, true);
            const auto bothInvalid = makeView (-0.9f, false, 1.0f, 1.0f, false);
            const auto fastInvertedNarrow = withFast (makeView (0.1f, true, 1.0f, 0.01f, true), -0.5f);   // 慢路在门内（0.1 < 0.3）、快路反相
            const auto gatedFastWide = withFast (makeView (0.5f, true, midPowerForAllowedDb (0.4), 1.0f, true), -0.9f);   // 慢路在门外

            return {
                { samplesFor (0.50), correlatedNarrow(),   first  },   // 1 静止：下限 0，上限 = 允许值 17.8 dB
                { samplesFor (0.30), invertedWide(),       first  },   // 2 反相：资格期 0.15 s，再攻击 0.1 s 到 1；宽度 0.133 s 收到 0.4 dB
                { samplesFor (1.00), correlatedVeryWide(), first  },   // 3 保持 2 → 1 s；宽度 0.4 → 0（6 dB/s）
                { samplesFor (3.00), correlatedNarrow(),   first  },   // 4 保持用完 1 s + 释放 2 s → 0；宽度 2 s 放回 1.2 → 静止
                { samplesFor (0.10), invertedNarrow(),     first  },   // 5 瞬态：0.1 s < 0.15 s，不触发（下限 0、没有保持期）
                { samplesFor (0.10), correlatedNarrow(),   first  },   // 6 条件断开：资格期清零
                { samplesFor (0.20), fastInvertedNarrow,   first  },   // 7 快路在门内触发：资格期 0.15 s 后攻击 0.05 s → 0.5，保持 2 s
                { samplesFor (1.50), gatedFastWide,        second },   // 8 慢路在门外，快路再负也不触发：保持 2 → 0.5 s；方案 0.8 dB：0.1 s 收到 0.4
                { samplesFor (0.50), bothInvalid,          second },   // 9 两路都无效：整段冻结
                { samplesFor (0.70), lowInvalidNarrow,     second },   // 10 低频无效（反相一路冻结）；宽度 0.4 → 0.68（0.4 dB/s）
                { samplesFor (4.00), correlatedMonoHigh(), first  },   // 11 保持 0.5 s + 释放 1 s（0.5 → 0）；宽度 0.68 → 1.2 后回到不限
            };
        }
    }

    class ClayGuardTests  : public juce::UnitTest
    {
    public:
        ClayGuardTests() : juce::UnitTest ("ClayGuard", "Celadon") {}

        void runTest() override
        {
            testPhaseGuardTiming();
            testPhaseTriggerCondition();
            testPhaseQualification();
            testPhaseGuardRetrigger();
            testPhaseGuardWithoutPlannedMono();
            testWidthGuardRetractAndRelease();
            testWidthGuardNeverNarrows();
            testFreeze();
            testInactiveReturnsToNeutral();
            testMonotonicPropertyScan();
            testBlockSizeConsistency();
            testSetStateClampsAndResumesBitExact();
            testCustomConstants();
            testLegacyAndNonFiniteConstants();
            testConstantsAndDefaults();
            testPressureMonitor();
            testPressureMonitorBlendLoss();
            testScaleClayPlan();

            testResolveCombinations();
            testResolveClampsAndPurity();
            testManualAndOffIgnoreGuard();

            testDoesNotAllocate();
        }

    private:
        //==============================================================================
        // 1. 反相提示时序（10 ms 一块；ADR-015 起只提示、不改收单）：低频同相 1 s 不动（提示强度 0）；反相 0.14 s 仍在资格期（强度 0、
        //    不亮灯）；满 0.15 s 起攻击：0.2 s → 强度 0.5（线性 1 / 0.1 s）、保持期 2 s；0.25 s → 1；条件消失后 1.99 s 仍是 1（保持 2 s）；
        //    此后 1 s 释放到 0.5、2 s 到 0 → 灯灭。全程每一块输出都 = 方案（收单 0.3 不动，lowCutHz / monoHz 原样）。
        // 依据：设计规格-Clay-根治修订 3.4 的时序（攻击 0.1 s、保持 2 s、释放 2 s）；ADR-015「反相只提示」；ClayGuard.h 文件头。
        void testPhaseGuardTiming()
        {
            beginTest ("phase warning: inverted bass must persist 0.15 s, then reaches 1 within 100 ms, holds 2 s, fades over 2 s; mono never changes");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const auto planned = plannedAuto();
            Extremes extremes;
            const auto level = [&guard] { return guard.getState().phaseFloor; };

            auto out = feed (guard, planned, correlatedNarrow(), 1.0, &extremes);
            expectEquals (out.monoAmount, 0.3f, "correlated bass: plan untouched");
            expectEquals (out.widthDb, 1.2f, "narrow highs: plan untouched");
            expect (! guard.getState().phaseActive && ! guard.getState().widthActive);
            expectEquals (level(), 0.0f);

            out = feed (guard, planned, invertedNarrow(), 0.14, &extremes);
            expectEquals (level(), 0.0f, "no attack before the condition has held 0.15 s");
            expect (! guard.getState().phaseActive, "qualifying does not light the warning (no amber lamp for transients)");

            out = feed (guard, planned, invertedNarrow(), 0.06, &extremes);
            expectWithinAbsoluteError (level(), 0.5f, 1.0e-4f, "linear attack: half way 50 ms after qualifying (0.2 s in)");
            expectEquals (guard.getState().phaseHoldSeconds, 2.0f, "hold re-armed to 2 s while triggered");
            expect (guard.getState().phaseActive, "the warning is on");
            expectEquals (out.monoAmount, 0.3f, "ADR-015: the warning never changes the mono amount");

            out = feed (guard, planned, invertedNarrow(), 0.05, &extremes);
            expectWithinAbsoluteError (level(), 1.0f, 1.0e-4f, "full level 100 ms after qualifying (0.25 s in)");

            out = feed (guard, planned, invertedNarrow(), 1.9, &extremes);
            expectEquals (level(), 1.0f);

            out = feed (guard, planned, correlatedNarrow(), 1.99, &extremes);
            expectEquals (level(), 1.0f, "still held 1.99 s after the inversion ended");
            expectWithinAbsoluteError (guard.getState().phaseHoldSeconds, 0.01f, 1.0e-4f);
            expect (guard.getState().phaseActive, "the warning stays on through the hold");

            // 保持期按块取回 float，200 块累计舍入 < 1e-5 s：这一块末尾保持期用完（或只剩舍入量），释放至多走了舍入量。
            out = feed (guard, planned, correlatedNarrow(), 0.01, &extremes);
            expectWithinAbsoluteError (level(), 1.0f, 1.0e-4f, "hold just ended, release not started yet");
            expectLessOrEqual (guard.getState().phaseHoldSeconds, 1.0e-4f);

            out = feed (guard, planned, correlatedNarrow(), 1.0, &extremes);
            expectWithinAbsoluteError (level(), 0.5f, 1.0e-3f, "linear release: half way after 1 s");
            expect (guard.getState().phaseActive, "still fading: the warning is on");

            out = feed (guard, planned, correlatedNarrow(), 1.0, &extremes);
            expectLessOrEqual (level(), 1.0e-4f, "released after 2 s");

            out = feed (guard, planned, correlatedNarrow(), 0.1, &extremes);
            expectEquals (level(), 0.0f);
            expect (! guard.getState().phaseActive, "released: the warning is off");
            expectEquals (out.widthDb, 1.2f);

            expectEquals (extremes.passThroughMismatches, 0, "lowCutHz / monoHz pass through bit-exactly in every block");
            expectEquals (extremes.minMono, 0.3f, "ADR-015: the mono amount stays at the plan in every block");
            expectEquals (extremes.maxMono, 0.3f);
            expectEquals (extremes.minWidth, 1.2f, "the width guard stayed out of it (narrow highs)");
        }

        //==============================================================================
        // 1b. 触发条件（每种视图从新的保护单元跑 0.5 s = 资格期 0.15 s + 攻击 0.1 s 之后还有余量）：
        //     慢路 −0.25 触发；慢路 −0.15（没到 −0.2，阳性对照的反面）不触发；慢路 0.25（门内）+ 快路 −0.3 触发；
        //     慢路 0.35（门外）+ 快路 −0.9 不触发——宽低音上快路的估计散布再大也拉不动保护（审核 V1-01 的根治）；
        //     门内但快路 −0.15、快路无效、快路 NaN 都不触发（没有加速证据，按慢路那一支判定）；边界值不算（严格小于）；
        //     慢路无效时即使快路 −0.9 也不动（这一路冻结）。
        // 依据：设计规格-Clay-根治修订 3.4「慢路 ρ < −0.2，或（慢路 ρ < 0.3 且快路 ρ < −0.2）」；ClayGuard.cpp 文件头。
        void testPhaseTriggerCondition()
        {
            beginTest ("phase trigger: slow < -0.2, or slow < 0.3 with fast < -0.2; a negative fast path above the gate never triggers");

            const auto planned = plannedAuto();
            const auto triggers = [&planned] (const ShortTermView& view)
            {
                ClayGuard guard;
                guard.setConstants (specConstants());
                const auto out = feed (guard, planned, view, 0.5);
                return guard.getState().phaseFloor > 0.0f && guard.getState().phaseActive
                    && bitsOf (out.monoAmount) == bitsOf (planned.monoAmount);   // ADR-015：只提示，收单不动
            };

            const auto slowOnly = [] (float slow) { return makeView (slow, true, 1.0f, 0.01f, true); };

            expect (triggers (slowOnly (-0.25f)), "slow path below -0.2 triggers");
            expect (! triggers (slowOnly (-0.15f)), "slow path at -0.15 (above -0.2) does not");
            expect (! triggers (slowOnly (-0.2f)), "the threshold itself does not (strictly below)");
            expect (triggers (withFast (slowOnly (0.25f), -0.3f)), "slow inside the gate (0.25 < 0.3) + fast below -0.2 triggers");
            expect (! triggers (withFast (slowOnly (0.35f), -0.9f)), "slow outside the gate: however negative the fast path, no trigger");
            expect (! triggers (withFast (slowOnly (0.3f), -0.9f)), "the gate itself is outside (strictly below)");
            expect (! triggers (withFast (slowOnly (0.25f), -0.15f)), "inside the gate but fast above -0.2: no trigger");
            expect (! triggers (withFast (slowOnly (0.25f), -0.9f, false)), "an invalid fast path gives no evidence");
            expect (! triggers (withFast (slowOnly (0.25f), std::numeric_limits<float>::quiet_NaN())), "a NaN fast path gives no evidence");
            expect (triggers (withFast (slowOnly (-0.25f), 0.9f)), "the slow branch alone is enough (fast path irrelevant)");

            {
                ClayGuard guard;
                guard.setConstants (specConstants());
                const auto out = feed (guard, planned, withFast (makeView (0.1f, false, 1.0f, 0.01f, true), -0.9f), 1.0);
                expect (samePlan (out, planned) && phaseIdle (guard.getState()),
                        "slow path invalid: the phase half is frozen even if the fast path looks inverted");
            }
        }

        //==============================================================================
        // 1c. 资格期（10 ms 一块）：
        //     a) 反相 0.14 s → 同相 0.01 s → 反相 0.14 s：两段都没满 0.15 s，一直不触发；再反相 0.02 s（连续 0.16 s）→ 0.01 s 的攻击 = 0.1；
        //     b) 资格期里冻结（seconds = 0、慢路无效）计时保持：反相 0.1 s → 冻结 → 反相 0.1 s = 连续 0.2 s → 0.5；
        //        阳性对照：中间换成一块同相 → 清零 → 最后仍是 0；
        //     c) setState / reset / 不活跃后从 0 起算：A 反相 0.1 s 后 B.setState (A)，两者再反相 0.1 s → A 0.5、B 0（资格期没存档）；
        //        B 再 0.1 s → 0.5（B 比 A 晚 0.1 s 满资格）；reset / active = false 同理；
        //     d) 从判定（条件开始成立）起 a = 1 恰好 0.15 + 0.1 = 0.25 s：0.24 s 时 0.9、0.25 s 时 1（设计规格-Clay-根治修订 4(c) 在保护
        //        单元这一层的口径；整链从反相音频出现起还要加分析窗与平滑，见 ClayGlazeTests）；最短持续 0 = 旧行为（0.1 s 到 1）；
        //     e) 满资格的时刻落在块中间：一整块 0.2 s 与 7 样本的小块结果相同（0.5）；在保持期里一整块 0.2 s 反相（先耗保持、
        //        释放 0.05 s、再攻击 0.05 s）与 10 ms 块相同。
        // 依据：设计规格-Clay-根治修订 3.4「条件须连续成立 0.15 s 才开始攻击」；ClayGuard.h「资格期计时不存档，setState / reset 后从 0 起算」。
        void testPhaseQualification()
        {
            beginTest ("phase qualification: 0.15 s continuous, broken by a clear block, kept through freezes, restarted by setState / reset / inactive");

            const auto planned = plannedAuto();
            const auto freshGuard = []
            {
                ClayGuard g;
                g.setConstants (specConstants());
                return g;
            };

            // a) 瞬态。
            {
                auto guard = freshGuard();
                feed (guard, planned, invertedNarrow(), 0.14);
                expectEquals (guard.getState().phaseFloor, 0.0f);
                feed (guard, planned, correlatedNarrow(), 0.01);
                feed (guard, planned, invertedNarrow(), 0.14);
                expect (phaseIdle (guard.getState()), "two 0.14 s bursts split by one clear block never trigger");
                feed (guard, planned, invertedNarrow(), 0.02);
                expectWithinAbsoluteError (guard.getState().phaseFloor, 0.1f, 1.0e-4f, "0.16 s continuous: 10 ms of attack");
                // ADR-015：标志 = 提示强度 > 0（与方案值无关）；满资格一开始攻击就亮。
                expect (guard.getState().phaseActive, "level 0.1 > 0: the warning is on");
            }

            // b) 冻结保持计时；同相清零（阳性对照）。
            for (const auto clearInBetween : { false, true })
            {
                auto guard = freshGuard();
                feed (guard, planned, invertedNarrow(), 0.1);

                for (int k = 0; k < 5; ++k)
                    guard.update (planned, invertedNarrow(), 0.0, true);

                feed (guard, planned, makeView (-0.9f, false, 1.0f, 0.01f, true), 0.5);   // 慢路无效：反相一路冻结

                if (clearInBetween)
                    feed (guard, planned, correlatedNarrow(), 0.01);

                feed (guard, planned, invertedNarrow(), 0.1);

                if (clearInBetween)
                    expectEquals (guard.getState().phaseFloor, 0.0f, "a clear block restarts the qualification");
                else
                    expectWithinAbsoluteError (guard.getState().phaseFloor, 0.5f, 1.0e-4f,
                                               "freezes (seconds 0, invalid slow path) keep the qualifying time: 0.1 + 0.1 s continuous");
            }

            // c) setState / reset / 不活跃。
            {
                auto a = freshGuard();
                auto b = freshGuard();
                feed (a, planned, invertedNarrow(), 0.1);
                b.setState (a.getState());
                expect (sameState (a.getState(), b.getState()), "the saved state itself is identical");
                feed (a, planned, invertedNarrow(), 0.1);
                feed (b, planned, invertedNarrow(), 0.1);
                expectWithinAbsoluteError (a.getState().phaseFloor, 0.5f, 1.0e-4f, "the running guard qualified 50 ms ago");
                expectEquals (b.getState().phaseFloor, 0.0f, "the restored guard starts qualifying from 0 (not saved)");
                feed (b, planned, invertedNarrow(), 0.1);
                expectWithinAbsoluteError (b.getState().phaseFloor, 0.5f, 1.0e-4f, "... and triggers 0.1 s later");

                auto c = freshGuard();
                feed (c, planned, invertedNarrow(), 0.1);
                c.reset();
                feed (c, planned, invertedNarrow(), 0.1);
                expectEquals (c.getState().phaseFloor, 0.0f, "reset restarts the qualification");

                auto d = freshGuard();
                feed (d, planned, invertedNarrow(), 0.1);
                d.update (planned, invertedNarrow(), 0.01, false);
                feed (d, planned, invertedNarrow(), 0.1);
                expectEquals (d.getState().phaseFloor, 0.0f, "an inactive block (GLAZE 0) restarts the qualification");
            }

            // d) 从判定起 0.25 s 到 1；最短持续 0 = 立即攻击。
            {
                auto guard = freshGuard();
                auto out = feed (guard, planned, invertedNarrow(), 0.24);
                expectWithinAbsoluteError (guard.getState().phaseFloor, 0.9f, 1.0e-4f, "0.24 s after the condition started: 0.9");
                out = feed (guard, planned, invertedNarrow(), 0.01);
                expectWithinAbsoluteError (guard.getState().phaseFloor, 1.0f, 1.0e-4f, "0.25 s = 0.15 s qualifying + 0.1 s attack: level 1");
                expectEquals (out.monoAmount, planned.monoAmount, "ADR-015: the mono amount stays at the plan");

                auto immediate = specConstants();
                immediate.clayGuardPhaseMinSeconds = 0.0;
                ClayGuard old;
                old.setConstants (immediate);
                out = feed (old, planned, invertedNarrow(), 0.05);
                expectWithinAbsoluteError (old.getState().phaseFloor, 0.5f, 1.0e-5f, "minimum duration 0: the attack starts at once");
            }

            // e) 满资格落在块中间：块长无关。
            {
                auto whole = freshGuard();
                auto fine = freshGuard();
                whole.update (planned, invertedNarrow(), 0.2, true);
                feed (fine, planned, invertedNarrow(), 0.2, nullptr, 7);
                expectWithinAbsoluteError (whole.getState().phaseFloor, 0.5f, 1.0e-5f, "one 0.2 s block: 0.15 s qualifying + 0.05 s attack");
                expectWithinAbsoluteError (fine.getState().phaseFloor, whole.getState().phaseFloor, 1.0e-4f, "7-sample blocks: the same");

                // 保持期里：保持剩 0.1 s，一整块 0.2 s 反相 = 前 0.15 s 未触发（耗 0.1 s 保持 + 释放 0.05 s → 0.975）+ 攻击 0.05 s → 1。
                auto heldWhole = freshGuard();
                auto heldFine = freshGuard();

                for (auto* g : { &heldWhole, &heldFine })
                {
                    feed (*g, planned, invertedNarrow(), 0.3);
                    feed (*g, planned, correlatedNarrow(), 1.9);
                }

                expectWithinAbsoluteError (heldWhole.getState().phaseHoldSeconds, 0.1f, 1.0e-4f, "fixture: 0.1 s of hold left");
                heldWhole.update (planned, invertedNarrow(), 0.2, true);
                feed (heldFine, planned, invertedNarrow(), 0.2);
                expectWithinAbsoluteError (heldWhole.getState().phaseFloor, 1.0f, 1.0e-4f);
                expectWithinAbsoluteError (heldWhole.getState().phaseFloor, heldFine.getState().phaseFloor, 1.0e-4f);
                expectEquals (heldWhole.getState().phaseHoldSeconds, 2.0f, "re-armed after qualifying");

                // 同上但只反相 0.12 s（没满资格）：保持照耗、释放 0.02 s → 0.99，不重新拉满保持期。
                auto shortBurst = freshGuard();
                feed (shortBurst, planned, invertedNarrow(), 0.3);
                feed (shortBurst, planned, correlatedNarrow(), 1.9);
                shortBurst.update (planned, invertedNarrow(), 0.12, true);
                expectWithinAbsoluteError (shortBurst.getState().phaseFloor, 0.99f, 1.0e-4f, "a transient during the hold does not re-arm it");
                expectEquals (shortBurst.getState().phaseHoldSeconds, 0.0f);
            }
        }

        // 2. 再触发（也要连续成立 0.15 s）：保持期里的 0.1 s 瞬态不重新拉满保持期；连续 0.2 s 的反相重新拉满（1.5 s 后仍是 1）；
        //    释放到 0.5 时再反相 0.17 s → 前 0.15 s 照常释放到 0.425，满资格后从此刻的下限按攻击速率升 0.02 s → 0.625（不从 0 重来）。
        // 依据：设计规格-Clay-根治修订 3.4（条件须连续成立 0.15 s 才开始攻击）；「条件消失后保持 2 s」从最后一次成立起算；攻击是
        //    phaseFloor 的线性上升。
        void testPhaseGuardRetrigger()
        {
            beginTest ("phase warning: re-triggering also needs 0.15 s; it re-arms the full hold and resumes the attack from the current level");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const auto planned = plannedAuto();

            feed (guard, planned, invertedNarrow(), 0.3);
            expectEquals (guard.getState().phaseFloor, 1.0f, "fixture: fully protecting");
            feed (guard, planned, correlatedNarrow(), 1.0);
            expectWithinAbsoluteError (guard.getState().phaseHoldSeconds, 1.0f, 1.0e-4f);

            feed (guard, planned, invertedNarrow(), 0.1);
            expectWithinAbsoluteError (guard.getState().phaseHoldSeconds, 0.9f, 1.0e-4f, "a 0.1 s transient does not re-arm the hold");
            feed (guard, planned, correlatedNarrow(), 0.05);

            feed (guard, planned, invertedNarrow(), 0.2);
            expectEquals (guard.getState().phaseHoldSeconds, 2.0f, "0.2 s continuous: hold re-armed");

            auto out = feed (guard, planned, correlatedNarrow(), 1.5);
            expectEquals (guard.getState().phaseFloor, 1.0f, "1.5 s after the re-trigger: still held");
            expectEquals (out.monoAmount, planned.monoAmount, "ADR-015: the mono amount stays at the plan");
            expectWithinAbsoluteError (guard.getState().phaseHoldSeconds, 0.5f, 1.0e-4f);

            feed (guard, planned, correlatedNarrow(), 0.5);
            out = feed (guard, planned, correlatedNarrow(), 1.0);
            expectWithinAbsoluteError (guard.getState().phaseFloor, 0.5f, 1.0e-3f);

            out = feed (guard, planned, invertedNarrow(), 0.17);
            expectWithinAbsoluteError (guard.getState().phaseFloor, 0.625f, 1.0e-3f,
                                       "0.15 s more release (0.425), then the attack resumes from there at 10 per second");
            expectEquals (guard.getState().phaseHoldSeconds, 2.0f);
        }

        // 3. 方案没开收单（ClayPlan{}：monoAmount 0、默认 80 Hz、不加宽——ADR-015 起自动方案恒如此）：反相提示照样在 0.15 s + 0.1 s 内
        //    到 1、亮灯；输出与方案逐位相同（收单仍是 0、分频点 80 Hz）；宽度方案 0 → 宽度保护什么都不做。
        // 依据：ADR-015「反相只提示、不改收单」；ClayGuard.h 文件头。
        void testPhaseGuardWithoutPlannedMono()
        {
            beginTest ("phase warning with the automatic plan (no mono): the lamp lights, the output stays the plan bit for bit");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const ClayPlan neutralPlan {};

            auto out = feed (guard, neutralPlan, correlatedWide(), 0.5);
            expect (samePlan (out, neutralPlan), "nothing to do on correlated bass");

            out = feed (guard, neutralPlan, invertedWide(), 0.26);
            expectEquals (guard.getState().phaseFloor, 1.0f, "the warning reaches full level");
            expect (samePlan (out, neutralPlan), "ADR-015: the output is still the plan (mono 0, crossover 80 Hz)");
            expectEquals (out.widthDb, 0.0f, "no planned widening: the width guard never goes below 0");
            expect (guard.getState().phaseActive && ! guard.getState().widthActive);
        }

        //==============================================================================
        // 4. 宽度保护（方案 +1.2 dB，10 ms 一块）：高频窄 → 不动；变宽（允许 0.4 dB）→ 按 1.2 dB / 0.2 s 收回：100 ms 时 0.6、
        //    133 ms 起停在 0.4，此时预测相关度 ρ' = 0.25（刚好守住下限）；更宽（允许 0）→ 收到 0 且每块都 ≥ 0；
        //    高频变回单声道（P_S = 0，不限）→ 按 1.2 dB / 2 s 放开：1 s 时 0.6，2 s 后回到 1.2 且上限回到「不限」。
        // 依据：设计规格-Clay-分析决策与验收 §3「widthDb 收到允许的最大值 10·log10(0.6·P_M/P_S)（下限 0）；攻击 200 ms，释放 2 s」、
        //       §2「ρ' = (P_M − γ²P_S)/(P_M + γ²P_S) ≥ 0.25」；ClayGuard.cpp 文件头「速率按方案加宽量定」。
        void testWidthGuardRetractAndRelease()
        {
            beginTest ("width guard: retracts widening to the allowed amount within 200 ms, never below 0, releases over 2 s");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const auto planned = plannedAuto();
            Extremes extremes;

            auto out = feed (guard, planned, correlatedNarrow(), 1.0, &extremes);
            expectEquals (out.widthDb, 1.2f);
            expect (! guard.getState().widthActive);

            out = feed (guard, planned, correlatedWide(), 0.1, &extremes);
            expectWithinAbsoluteError (out.widthDb, 0.6f, 1.0e-4f, "linear retraction: 1.2 dB per 200 ms");
            expect (guard.getState().widthActive);

            out = feed (guard, planned, correlatedWide(), 0.1, &extremes);
            expectWithinAbsoluteError (out.widthDb, 0.4f, 1.0e-5f, "settled on the allowed width");

            {
                // 加宽后的预测相关度（与决策器同一公式）：刚好在下限 0.25。
                const auto view = correlatedWide();
                const auto gammaSquared = std::pow (10.0, static_cast<double> (out.widthDb) / 10.0);
                const auto mid = static_cast<double> (view.liveHighMidPower);
                const auto side = static_cast<double> (view.liveHighSidePower);
                const auto predicted = (mid - gammaSquared * side) / (mid + gammaSquared * side);
                expectGreaterOrEqual (predicted, 0.25 - 1.0e-4, "predicted correlation after widening stays >= 0.25");
                expectLessOrEqual (predicted, 0.25 + 1.0e-4, "and the guard does not over-retract");
            }

            out = feed (guard, planned, correlatedVeryWide(), 0.2, &extremes);
            expectEquals (out.widthDb, 0.0f, "already below the correlation floor: widening removed, never narrowed");

            out = feed (guard, planned, correlatedMonoHigh(), 1.0, &extremes);
            expectWithinAbsoluteError (out.widthDb, 0.6f, 1.0e-4f, "linear release: 1.2 dB per 2 s");
            expect (guard.getState().widthActive);

            out = feed (guard, planned, correlatedMonoHigh(), 1.05, &extremes);
            expectEquals (out.widthDb, 1.2f, "fully released");
            expect (! guard.getState().widthActive);
            expectEquals (guard.getState().widthCapDb, glaze::clayWidthUncappedDb, "cap back to 'uncapped' (P_S = 0)");

            expectGreaterOrEqual (extremes.minWidth, 0.0f, "the guard never narrows, in any block");
            expectLessOrEqual (extremes.maxWidth, 1.2f, "and never exceeds the plan");
            expectEquals (extremes.minMono, 0.3f, "phase guard stayed out of it (correlated bass)");
            expectEquals (extremes.passThroughMismatches, 0);
        }

        // 5. 从不收窄：方案不加宽（0）或是负值（自动不会给，防呆）时，最宽的高频下输出也原样；P_M = 0（高频全是侧声道）→ 允许 0。
        // 依据：设计规格-Clay §8「自动收窄（负宽度只能手动）」不做；分析决策与验收 §3「（下限 0）」。
        void testWidthGuardNeverNarrows()
        {
            beginTest ("width guard never narrows: zero or negative planned width passes through; P_M = 0 allows 0");

            ClayGuard guard;
            guard.setConstants (specConstants());

            auto flat = plannedAuto();
            flat.widthDb = 0.0f;
            auto out = feed (guard, flat, correlatedVeryWide(), 1.0);
            expectEquals (out.widthDb, 0.0f);
            expect (! guard.getState().widthActive);

            auto narrowing = plannedAuto();
            narrowing.widthDb = -1.0f;
            out = feed (guard, narrowing, correlatedVeryWide(), 1.0);
            expectEquals (out.widthDb, -1.0f, "a negative plan value is left alone");

            // 新的保护（上限从「不限」出发）：方案 1.2 dB 在 0.2 s 内收到 0。
            ClayGuard fresh;
            fresh.setConstants (specConstants());
            const auto pureSide = makeView (0.8f, true, 0.0f, 1.0f, true);
            out = feed (fresh, plannedAuto(), pureSide, 0.3);
            expectEquals (out.widthDb, 0.0f, "anti-phase highs: no widening allowed");
            expect (fresh.getState().widthActive);
        }

        //==============================================================================
        // 6. 冻结：先让两路都介入（反相 + 宽，0.2 s：资格期 0.15 s + 攻击 0.05 s → 下限 0.5；宽度已收到 0.4）。seconds = 0 / 负 /
        //    NaN / ±∞ → 状态逐位不动，输出按当前状态改方案；低频相关度无效（或 NaN）→ 反相一路不动（保持期不走）、宽度一路照走
        //   （很宽的高频：0.4 → 0）；高频无效（或功率 NaN / 负 / ∞）→ 宽度一路不动、反相一路照走；两路都无效 → 整体不动。
        //    冻结期间资格期计时同样不动（testPhaseQualification b）。
        // 依据：ClayGuard.h「冻结：seconds = 0（静音 / 宿主旁通 / 停播块）或实时视图无效时，状态不动（保持期、资格期计时都不走）」。
        void testFreeze()
        {
            beginTest ("freeze: seconds <= 0 / non-finite freezes everything; an invalid view half freezes only that half");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const auto planned = plannedAuto();

            const auto engaged = feed (guard, planned, invertedWide(), 0.2);
            const auto before = guard.getState();
            expect (before.phaseActive && before.widthActive, "positive control: both guards engaged");
            expectWithinAbsoluteError (before.phaseFloor, 0.5f, 1.0e-4f);
            expectWithinAbsoluteError (before.widthCapDb, 0.4f, 1.0e-5f);

            const double frozenSeconds[] { 0.0, -0.01, std::numeric_limits<double>::quiet_NaN(),
                                           std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };

            for (const auto frozen : frozenSeconds)
            {
                const auto out = guard.update (planned, invertedWide(), frozen, true);
                expect (sameState (guard.getState(), before), "state unchanged for seconds = " + juce::String (frozen));
                expect (samePlan (out, engaged), "output from the current state for seconds = " + juce::String (frozen));
            }

            // 低频无效：反相一路冻结（下限停在 0.5、保持期不走），宽度一路照走（很宽的高频，允许 0：0.4 → 0）。
            const auto lowInvalid = makeView (-0.9f, false, 0.5f, 1.0f, true);
            auto out = feed (guard, planned, lowInvalid, 0.1);
            expectEquals (guard.getState().phaseFloor, before.phaseFloor, "phase floor frozen");
            expectEquals (guard.getState().phaseHoldSeconds, before.phaseHoldSeconds, "hold does not run while the bass view is invalid");
            expectEquals (out.widthDb, 0.0f, "width half keeps working");

            const auto lowNaN = makeView (std::numeric_limits<float>::quiet_NaN(), true, 0.5f, 1.0f, true);
            feed (guard, planned, lowNaN, 0.1);
            expectEquals (guard.getState().phaseHoldSeconds, before.phaseHoldSeconds, "NaN correlation = invalid");
            expectEquals (guard.getState().phaseFloor, before.phaseFloor);

            // 高频无效：宽度一路冻结（即使低频同相、高频看起来很窄），反相一路照走（保持 2 → 1.5）。
            const auto capBefore = guard.getState().widthCapDb;
            const auto highInvalid = makeView (0.8f, true, 1.0f, 0.01f, false);
            out = feed (guard, planned, highInvalid, 0.5);
            expectEquals (guard.getState().widthCapDb, capBefore, "width cap frozen");
            expectWithinAbsoluteError (guard.getState().phaseHoldSeconds, 1.5f, 1.0e-4f, "phase half keeps working");

            const float badPowers[][2] { { std::numeric_limits<float>::quiet_NaN(), 1.0f }, { 1.0f, std::numeric_limits<float>::quiet_NaN() },
                                         { -1.0f, 1.0f }, { 1.0f, -1.0f }, { std::numeric_limits<float>::infinity(), 1.0f },
                                         { 1.0f, std::numeric_limits<float>::infinity() } };

            for (const auto& powers : badPowers)
            {
                const auto view = makeView (0.8f, true, powers[0], powers[1], true);
                feed (guard, planned, view, 0.05);
                expectEquals (guard.getState().widthCapDb, capBefore, "non-finite / negative high powers freeze the width half");
            }

            // 两路都无效：整体不动。
            const auto frozenState = guard.getState();
            out = feed (guard, planned, makeView (-0.9f, false, 1.0f, 1.0f, false), 1.0);
            expect (sameState (guard.getState(), frozenState), "both halves invalid: nothing moves");
            expect (std::isfinite (out.monoAmount) && std::isfinite (out.widthDb));
        }

        // 7. active = false（GLAZE ≤ 0）：状态直接回 ClayGuardState{}（逐位），输出逐位 = 输入（连 NaN 都原样）；
        //    之后重新 active 从中性开始（同相 / 窄 → 输出 = 方案）；reset 同样回中性。
        // 依据：ClayGuard.h「active = false（GLAZE ≤ 0）：状态直接回中性（ClayGuardState{}），输出 = 输入」。
        void testInactiveReturnsToNeutral()
        {
            beginTest ("inactive (GLAZE <= 0) resets the state to neutral and returns the plan untouched; reset() does the same");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const auto planned = plannedAuto();

            feed (guard, planned, invertedWide(), 0.3);
            expect (guard.getState().phaseActive && guard.getState().widthActive, "positive control: engaged");

            auto odd = planned;
            odd.monoAmount = std::numeric_limits<float>::quiet_NaN();
            odd.widthDb = 2.5f;
            const auto out = guard.update (odd, invertedWide(), 0.01, false);
            expect (samePlan (out, odd), "inactive: output is the input, bit for bit");
            expect (sameState (guard.getState(), ClayGuardState {}), "inactive: state is neutral");

            const auto again = guard.update (planned, correlatedNarrow(), 0.01, true);
            expect (samePlan (again, planned), "re-activated guard starts from neutral");

            feed (guard, planned, invertedWide(), 0.3);
            guard.reset();
            expect (sameState (guard.getState(), ClayGuardState {}), "reset: neutral");
        }

        //==============================================================================
        // 8. 随机扫描（2 万块）：方案（含越界 / 负值、代价 mask）、视图（慢路 / 快路各自有效 / 无效、ρ ∈ [−1, 1]、P_M / P_S 含 0）、
        //    块长（含 0 / 负）、active 都随机：每块 monoAmount ≥ 方案、widthDb ≤ 方案、lowCutHz / monoHz / costLimitedMask 原样、
        //    输出有限；active 时 monoAmount ≤ max(方案, 1)、widthDb ≥ min(方案, 0)（从不收窄）、状态在合法范围、两个标志与数值一致；
        //    inactive 时输出逐位 = 输入、状态中性。阳性对照：保护确实抬过 mono、收过 width 各数百次（扫描不是空转——视图每块都换，
        //    反相要连着几块才满 0.15 s 资格期，满了以后保持 2 s，所以仍有大量介入的块）。
        // 依据：ClayGuard 任务书「只可能增加 monoAmount、减小 widthDb（测试断言）」；ClayGuardState 各字段的注释。
        void testMonotonicPropertyScan()
        {
            beginTest ("random scan: mono passes through (ADR-015), only lowers widthDb, passes lowCutHz / monoHz, state stays legal");

            juce::Random random (20260926);
            ClayGuard guard;
            guard.setConstants (specConstants());

            int violations = 0, warned = 0, loweredWidth = 0;

            for (int k = 0; k < 20000; ++k)
            {
                ClayPlan planned;
                planned.lowCutHz = random.nextInt (4) == 0 ? 0.0f : 10.0f + 50.0f * random.nextFloat();
                planned.monoHz = 40.0f + 260.0f * random.nextFloat();
                planned.monoAmount = random.nextInt (5) == 0 ? 0.0f : -0.2f + 1.4f * random.nextFloat();
                planned.widthDb = random.nextInt (5) == 0 ? 0.0f : -3.0f + 6.0f * random.nextFloat();
                planned.costLimitedMask = static_cast<std::uint8_t> (random.nextInt (8));

                const auto lowValid = random.nextInt (5) != 0;
                const auto rho = -1.0f + 2.0f * random.nextFloat();
                const auto fastValid = random.nextInt (2) == 0;
                const auto fastRho = -1.0f + 2.0f * random.nextFloat();
                const auto highValid = random.nextInt (5) != 0;
                const auto mid = random.nextInt (10) == 0 ? 0.0f : 2.0f * random.nextFloat();
                const auto side = random.nextInt (10) == 0 ? 0.0f : 2.0f * random.nextFloat();
                const auto view = withFast (makeView (rho, lowValid, mid, side, highValid), fastRho, fastValid);

                auto seconds = 0.001 + 0.1 * random.nextDouble();
                const auto kind = random.nextInt (20);

                if (kind == 0)
                    seconds = 0.0;
                else if (kind == 1)
                    seconds = -0.01;

                const auto active = random.nextInt (40) != 0;
                const auto out = guard.update (planned, view, seconds, active);
                const auto& s = guard.getState();

                auto ok = bitsOf (out.lowCutHz) == bitsOf (planned.lowCutHz) && bitsOf (out.monoHz) == bitsOf (planned.monoHz)
                       && out.costLimitedMask == planned.costLimitedMask
                       && bitsOf (out.monoAmount) == bitsOf (planned.monoAmount) && out.widthDb <= planned.widthDb
                       && std::isfinite (out.monoAmount) && std::isfinite (out.widthDb);

                if (active)
                {
                    ok = ok && out.widthDb >= juce::jmin (planned.widthDb, 0.0f)
                            && s.phaseFloor >= 0.0f && s.phaseFloor <= 1.0f
                            && s.phaseHoldSeconds >= 0.0f && s.phaseHoldSeconds <= 2.0f
                            && s.widthCapDb >= 0.0f && s.widthCapDb <= glaze::clayWidthUncappedDb
                            && s.phaseActive == (s.phaseFloor > 0.0f)
                            && s.widthActive == (s.widthCapDb < planned.widthDb);
                }
                else
                {
                    ok = ok && samePlan (out, planned) && sameState (s, ClayGuardState {});
                }

                // 阳性对照：反相提示亮过（ADR-015 起它只提示）；宽度保护真的把正的方案加宽收小（负的方案值被抬到 0 不算）。
                if (active && s.phaseActive)
                    ++warned;

                if (planned.widthDb > 0.0f && out.widthDb < planned.widthDb)
                    ++loweredWidth;

                if (! ok && violations++ < 5)
                    logMessage ("  violation at step " + juce::String (k));
            }

            logMessage ("  phase warning in " + juce::String (warned) + " blocks, lowered width in " + juce::String (loweredWidth));
            expectEquals (violations, 0);
            expectGreaterThan (warned, 300, "positive control: the phase warning fired");
            expectGreaterThan (loweredWidth, 300, "positive control: the width guard acted");
        }

        //==============================================================================
        // 9. 块长一致：blockScript（11.9 s）按 10 ms 一块跑出参考轨迹；
        //    (a) 17 / 333 / 随机 1 ～ 4096 样本的块（在 10 ms 格点处也切开）→ 1190 个格点的下限、保持期、输出逐格比较；
        //    (b) 2048 / 8192 / 随机 1 ～ 8192 样本的块（只在段边界切开，块可跨过满资格的时刻、保持期末尾、攻击与释放的终点）
        //        → 11 个段末比较。
        //    容差 2e-3（下限 / monoAmount / widthDb dB）与 4e-3 s（保持期）：状态每块取回 float（≤ 半个 ulp），最坏的 17 样本块
        //    约 3.4 万次更新里在动的部分累计 < 7e-4；而「保持期末尾 / 满资格那一块不拆分」之类的块长相关错误在 8192 样本块上
        //    会差到 0.08 以上（8192 样本 = 0.17 s，比资格期还长）。
        //    参考轨迹自身的阳性对照：段末值与脚本注释（手算）一致，冻结段前后逐位相同，最后回到中性。
        // 依据：ClayGuard 任务书「块长（不同 seconds 切分）一致性容差」「保证块长无关到可接受精度」；设计规格-Clay-根治修订 3.4
        //    「连续成立 0.15 s（按有效音频秒数、块长无关）」。
        void testBlockSizeConsistency()
        {
            beginTest ("trajectory is block-size independent within float rounding (17 .. 8192-sample and random blocks)");

            const auto script = blockScript();

            ClayGuard referenceGuard;
            referenceGuard.setConstants (specConstants());
            const auto reference = runScript (referenceGuard, script, [] { return gridSamples; }, true);

            expectEquals (static_cast<int> (reference.gridStates.size()), 1190);
            expectEquals (static_cast<int> (reference.segmentStates.size()), 11);

            if (reference.segmentStates.size() != 11)
                return;

            const auto& ends = reference.segmentOutputs;
            const auto& endStates = reference.segmentStates;
            expectEquals (endStates[1].phaseFloor, 1.0f, "segment 2: qualified after 0.15 s, attack done by 0.25 s");
            expectEquals (ends[1].monoAmount, 0.3f, "segment 2: ADR-015, the mono amount stays at the plan");
            expectWithinAbsoluteError (ends[1].widthDb, 0.4f, 1.0e-5f, "segment 2: width retracted to the allowed 0.4 dB");
            expectEquals (ends[2].widthDb, 0.0f, "segment 3: very wide highs -> no widening");
            expectWithinAbsoluteError (endStates[2].phaseHoldSeconds, 1.0f, 1.0e-4f, "segment 3: 1 s of hold left");
            expectLessOrEqual (endStates[3].phaseFloor, 1.0e-3f, "segment 4: released");
            expectEquals (ends[3].widthDb, 1.2f, "segment 4: width released");
            expect (phaseIdle (endStates[4]) && ends[4].monoAmount == 0.3f, "segment 5: a 0.1 s inversion is a transient (no trigger)");
            expect (phaseIdle (endStates[5]), "segment 6: still idle");
            expectWithinAbsoluteError (endStates[6].phaseFloor, 0.5f, 1.0e-4f, "segment 7: triggered through the fast path inside the gate");
            expectEquals (endStates[6].phaseHoldSeconds, 2.0f, "segment 7: hold armed");
            expectWithinAbsoluteError (endStates[7].phaseHoldSeconds, 0.5f, 1.0e-4f, "segment 8: fast path outside the gate does not re-arm");
            expectWithinAbsoluteError (endStates[7].phaseFloor, 0.5f, 1.0e-4f, "segment 8: level held");
            expectWithinAbsoluteError (ends[7].widthDb, 0.4f, 1.0e-5f, "segment 8: new plan 0.8 dB retracted to 0.4");
            expect (sameState (endStates[8], endStates[7]), "segment 9: both halves invalid -> frozen");
            expectWithinAbsoluteError (ends[9].widthDb, 0.68f, 1.0e-4f, "segment 10: width releases at 0.8 dB / 2 s while the bass half is frozen");
            expectEquals (endStates[9].phaseHoldSeconds, endStates[8].phaseHoldSeconds, "segment 10: bass half frozen");
            expect (sameState (endStates[10], ClayGuardState {}), "segment 11: back to neutral");

            auto compare = [this] (const std::vector<ClayGuardState>& runStates, const std::vector<ClayPlan>& runOutputs,
                                   const std::vector<ClayGuardState>& refStates, const std::vector<ClayPlan>& refOutputs,
                                   const juce::String& label)
            {
                expectEquals (static_cast<int> (runStates.size()), static_cast<int> (refStates.size()), label + ": point count");

                if (runStates.size() != refStates.size() || runOutputs.size() != refOutputs.size())
                    return;

                float floorError = 0.0f, holdError = 0.0f, monoError = 0.0f, widthError = 0.0f;

                for (size_t i = 0; i < runStates.size(); ++i)
                {
                    floorError = juce::jmax (floorError, std::abs (runStates[i].phaseFloor - refStates[i].phaseFloor));
                    holdError = juce::jmax (holdError, std::abs (runStates[i].phaseHoldSeconds - refStates[i].phaseHoldSeconds));
                    monoError = juce::jmax (monoError, std::abs (runOutputs[i].monoAmount - refOutputs[i].monoAmount));
                    widthError = juce::jmax (widthError, std::abs (runOutputs[i].widthDb - refOutputs[i].widthDb));
                }

                logMessage ("  " + label + ": max |d floor| " + juce::String (floorError, 8) + ", |d hold| " + juce::String (holdError, 8)
                            + " s, |d mono| " + juce::String (monoError, 8) + ", |d width| " + juce::String (widthError, 8) + " dB");
                expectLessOrEqual (floorError, 2.0e-3f, label + ": phase floor");
                expectLessOrEqual (holdError, 4.0e-3f, label + ": hold");
                expectLessOrEqual (monoError, 2.0e-3f, label + ": monoAmount");
                expectLessOrEqual (widthError, 2.0e-3f, label + ": widthDb");
            };

            const int gridBlocks[] { 17, 333 };

            for (const auto blockSize : gridBlocks)
            {
                ClayGuard guard;
                guard.setConstants (specConstants());
                const auto trace = runScript (guard, script, [blockSize] { return blockSize; }, true);
                compare (trace.gridStates, trace.gridOutputs, reference.gridStates, reference.gridOutputs,
                         "grid, block " + juce::String (blockSize));
            }

            {
                juce::Random random (4096);
                ClayGuard guard;
                guard.setConstants (specConstants());
                const auto trace = runScript (guard, script, [&random] { return 1 + random.nextInt (4096); }, true);
                compare (trace.gridStates, trace.gridOutputs, reference.gridStates, reference.gridOutputs, "grid, random 1..4096");
            }

            const int longBlocks[] { 2048, 8192 };

            for (const auto blockSize : longBlocks)
            {
                ClayGuard guard;
                guard.setConstants (specConstants());
                const auto trace = runScript (guard, script, [blockSize] { return blockSize; }, false);
                compare (trace.segmentStates, trace.segmentOutputs, reference.segmentStates, reference.segmentOutputs,
                         "segment ends, block " + juce::String (blockSize));
            }

            for (int seed = 0; seed < 3; ++seed)
            {
                juce::Random random (8192 + seed);
                ClayGuard guard;
                guard.setConstants (specConstants());
                const auto trace = runScript (guard, script, [&random] { return 1 + random.nextInt (8192); }, false);
                compare (trace.segmentStates, trace.segmentOutputs, reference.segmentStates, reference.segmentOutputs,
                         "segment ends, random 1..8192 seed " + juce::String (seed));
            }
        }

        //==============================================================================
        // 10. setState：越界 / 非有限的值钳到合法范围（下限 0 ～ 1、保持期 ≥ 0 且有限、上限 0 ～ 不限；NaN 回中性），与数值矛盾的标志去掉；
        //     合法值逐位原样。恢复后接续：A 跑到脚本一半，B（新建、同常数）setState(A 的状态)，两者接着跑 → 每块输出与状态逐位相同。
        // 依据：ClayGuard 任务书「setState 钳到合法范围（phaseFloor 0～1、hold ≥ 0、widthCapDb ≤ uncapped 且 ≥ 0）」；
        //       设计规格-Clay §7「执行快照……保证保存重开的可复现口径」。
        void testSetStateClampsAndResumesBitExact()
        {
            beginTest ("setState clamps to the legal range, drops contradictory flags, restores legal values exactly and resumes bit-exactly");

            const auto qnan = std::numeric_limits<float>::quiet_NaN();
            const auto posInf = std::numeric_limits<float>::infinity();

            auto restoredFrom = [] (float floorValue, float holdValue, float capValue, bool phaseFlag, bool widthFlag)
            {
                ClayGuardState input;
                input.phaseFloor = floorValue;
                input.phaseHoldSeconds = holdValue;
                input.widthCapDb = capValue;
                input.phaseActive = phaseFlag;
                input.widthActive = widthFlag;

                ClayGuard guard;
                guard.setState (input);
                return guard.getState();
            };

            auto s = restoredFrom (-0.5f, -1.0f, -2.0f, true, true);
            expectEquals (s.phaseFloor, 0.0f);
            expectEquals (s.phaseHoldSeconds, 0.0f);
            expectEquals (s.widthCapDb, 0.0f);
            expect (! s.phaseActive, "no floor: cannot be protecting");
            expect (s.widthActive, "cap 0 may well be limiting");

            s = restoredFrom (1.7f, qnan, 500.0f, true, true);
            expectEquals (s.phaseFloor, 1.0f);
            expectEquals (s.phaseHoldSeconds, 0.0f);
            expectEquals (s.widthCapDb, glaze::clayWidthUncappedDb);
            expect (s.phaseActive);
            expect (! s.widthActive, "uncapped: cannot be limiting");

            s = restoredFrom (qnan, posInf, qnan, true, true);
            expect (sameState (s, ClayGuardState {}), "NaN / infinite values fall back to neutral");

            s = restoredFrom (posInf, 5.0e9f, -posInf, false, false);
            expectEquals (s.phaseFloor, 1.0f);
            expect (std::isfinite (s.phaseHoldSeconds) && s.phaseHoldSeconds > 0.0f && s.phaseHoldSeconds <= 1.0e6f);
            expectEquals (s.widthCapDb, 0.0f);

            ClayGuardState legal;
            legal.phaseFloor = 0.37f;
            legal.phaseHoldSeconds = 1.25f;
            legal.widthCapDb = 0.8f;
            legal.phaseActive = true;
            legal.widthActive = true;
            expect (sameState (restoredFrom (legal.phaseFloor, legal.phaseHoldSeconds, legal.widthCapDb, true, true), legal),
                    "legal values restore bit-exactly");

            // 恢复后接续：前 6 段 A 单独跑（第 6 段同相：资格期计时为 0，与 B 恢复后的起点相同——资格期不存档，见 ClayGuard.h），
            // 之后 B 从 A 的状态接手，两者同步跑完剩下的段。
            const auto script = blockScript();
            ClayGuard original, resumed;
            original.setConstants (specConstants());
            resumed.setConstants (specConstants());

            const std::vector<Segment> head (script.begin(), script.begin() + 6);
            runScript (original, head, [] { return 333; }, false);
            resumed.setState (original.getState());
            expect (sameState (resumed.getState(), original.getState()));

            int mismatches = 0, blocks = 0;

            for (size_t segmentIndex = 6; segmentIndex < script.size(); ++segmentIndex)
            {
                const auto& segment = script[segmentIndex];

                for (int done = 0; done < segment.samples; done += 333)
                {
                    const auto seconds = static_cast<double> (juce::jmin (333, segment.samples - done)) / clayRate;
                    const auto a = original.update (segment.planned, segment.view, seconds, true);
                    const auto b = resumed.update (segment.planned, segment.view, seconds, true);

                    if (! (samePlan (a, b) && sameState (original.getState(), resumed.getState())))
                        ++mismatches;

                    ++blocks;
                }
            }

            // 后 5 段 0.2 + 1.5 + 0.5 + 0.7 + 4.0 s 按 333 样本一块（段末短块）= 29 + 217 + 73 + 101 + 577 = 997 块。
            expectEquals (blocks, 997);
            expectEquals (mismatches, 0, "restored guard continues bit-exactly");
        }

        //==============================================================================
        // 11. 自定义常数（阈值 −0.4、快路门 0.5、最短持续 0.05 s、攻击 50 ms、保持 1 s、释放 0.5 s、ρ_min 0.5、宽度攻击 0.1 s /
        //     释放 1 s），5 ms 一块：
        //       - 阈值：慢路 −0.3 不触发（阳性对照：出厂阈值 −0.2 会触发）；
        //       - 快路门：慢路 0.4、快路 −0.5 触发（0.4 < 门 0.5 且 −0.5 < −0.4）；阳性对照：出厂门 0.3 下同一视图不触发；
        //       - 时序：慢路 −0.5 在 0.045 s 时仍在资格期；满 0.05 s 后攻击 50 ms：0.075 s 时 0.5、0.1 s 时 1；保持 1 s 后 0.25 s
        //         释放到 0.5、0.5 s 到方案值；
        //       - 宽度 k = (1 − 0.5)/(1 + 0.5) = 1/3：P_M = 3·10^0.05、P_S = 1 → 允许 0.5 dB，1.2 dB / 0.1 s 收回，50 ms 时 0.6、
        //         60 ms 起停在 0.5（阳性对照：出厂 k = 0.6 下同一视图允许 3.05 dB，不收）。
        // 依据：ClayGuard.h「k 由 clayMinCorrelationAfterWidth 解出」；setConstants 只拷贝规定的 9 个数（ADR-012 加快路门与最短持续）。
        void testCustomConstants()
        {
            beginTest ("custom constants: threshold, fast gate, minimum duration, attack / hold / release and the correlation floor all take effect");

            auto constants = specConstants();
            constants.clayGuardPhaseCorrelation = -0.4f;
            constants.clayGuardPhaseFastGate = 0.5f;
            constants.clayGuardPhaseMinSeconds = 0.05;
            constants.clayGuardPhaseAttackSeconds = 0.05;
            constants.clayGuardPhaseHoldSeconds = 1.0;
            constants.clayGuardPhaseReleaseSeconds = 0.5;
            constants.clayGuardWidthAttackSeconds = 0.1;
            constants.clayGuardWidthReleaseSeconds = 1.0;
            constants.clayMinCorrelationAfterWidth = 0.5f;

            constexpr int fiveMs = 240;
            const auto planned = plannedAuto();
            // 跑完后的反相提示强度（ADR-015：提示不改收单，所以看状态）。
            const auto runFresh = [&planned] (const glaze::Constants& c, const ShortTermView& view, double seconds)
            {
                ClayGuard fresh;
                fresh.setConstants (c);
                feed (fresh, planned, view, seconds, nullptr, fiveMs);
                return fresh.getState().phaseFloor;
            };
            const auto level = [] (const ClayGuard& g) { return g.getState().phaseFloor; };

            const auto slowAtMinus03 = makeView (-0.3f, true, 1.0f, 0.01f, true);
            expectEquals (runFresh (specConstants(), slowAtMinus03, 0.3), 1.0f, "positive control: the factory threshold -0.2 triggers on -0.3");
            expectEquals (runFresh (constants, slowAtMinus03, 1.0), 0.0f, "-0.3 is above the custom threshold -0.4");

            const auto gatedFast = withFast (makeView (0.4f, true, 1.0f, 0.01f, true), -0.5f);
            expectEquals (runFresh (specConstants(), gatedFast, 1.0), 0.0f, "positive control: slow 0.4 is outside the factory gate 0.3");
            expectEquals (runFresh (constants, gatedFast, 0.2), 1.0f, "slow 0.4 is inside the custom gate 0.5: the fast path triggers");

            ClayGuard guard;
            guard.setConstants (constants);
            const auto inverted = makeView (-0.5f, true, 1.0f, 0.01f, true);
            auto out = feed (guard, planned, inverted, 0.045, nullptr, fiveMs);
            expectEquals (level (guard), 0.0f, "0.045 s: still inside the custom 0.05 s qualification");
            out = feed (guard, planned, inverted, 0.03, nullptr, fiveMs);
            expectWithinAbsoluteError (level (guard), 0.5f, 1.0e-4f, "50 ms attack after qualifying: half way 25 ms later");
            out = feed (guard, planned, inverted, 0.025, nullptr, fiveMs);
            expectWithinAbsoluteError (level (guard), 1.0f, 1.0e-4f);
            expectEquals (out.monoAmount, planned.monoAmount, "ADR-015: the mono amount stays at the plan");
            expectEquals (guard.getState().phaseHoldSeconds, 1.0f);

            const auto calm = makeView (0.8f, true, 1.0f, 0.01f, true);
            out = feed (guard, planned, calm, 1.0, nullptr, fiveMs);
            expectWithinAbsoluteError (level (guard), 1.0f, 1.0e-4f, "1 s hold");
            out = feed (guard, planned, calm, 0.25, nullptr, fiveMs);
            expectWithinAbsoluteError (level (guard), 0.5f, 1.0e-3f, "0.5 s release: half way at 0.25 s");
            out = feed (guard, planned, calm, 0.3, nullptr, fiveMs);
            expectEquals (level (guard), 0.0f);

            const auto wide = makeView (0.8f, true, static_cast<float> (3.0 * std::pow (10.0, 0.05)), 1.0f, true);
            {
                ClayGuard factoryGuard;
                factoryGuard.setConstants (specConstants());
                const auto factoryOut = feed (factoryGuard, planned, wide, 0.3, nullptr, fiveMs);
                expectEquals (factoryOut.widthDb, 1.2f, "positive control: with k = 0.6 this view allows 3.05 dB");
            }

            out = feed (guard, planned, wide, 0.05, nullptr, fiveMs);
            expectWithinAbsoluteError (out.widthDb, 0.6f, 1.0e-4f, "0.1 s attack: 1.2 dB per 100 ms");
            out = feed (guard, planned, wide, 0.05, nullptr, fiveMs);
            expectWithinAbsoluteError (out.widthDb, 0.5f, 1.0e-4f, "k = 1/3 allows 0.5 dB");

            out = feed (guard, planned, calm, 0.5, nullptr, fiveMs);
            expectWithinAbsoluteError (out.widthDb, 0.5f + 0.6f, 1.0e-3f, "1 s release: 1.2 dB per second");
        }

        // 12. 旧快照缺字段（9 个数全 0）：阈值 0、快路门 0、最短持续 0、时长 0 = 立即——反相一块就到 1、同相一块就回方案（保持 0、
        //     释放立即）；ρ_min 0 → k = 1，宽度一块就到允许值 10·log10(P_M / P_S)。非有限常数（NaN / ±∞）与负时长按 0 读：轨迹与
        //     全 0 逐位相同，且全程有限。
        // 依据：GlazeConstants.h「旧快照缺这些字段按 0 读」；ClayGuard.cpp makeTuning「非有限的值一律按 0 读」。
        void testLegacyAndNonFiniteConstants()
        {
            beginTest ("legacy (all-zero) and non-finite constants: instant response, identical trajectories, always finite");

            glaze::Constants zeros;
            zeros.clayGuardPhaseCorrelation = 0.0f;
            zeros.clayGuardPhaseFastGate = 0.0f;
            zeros.clayGuardPhaseMinSeconds = 0.0;
            zeros.clayGuardPhaseAttackSeconds = 0.0;
            zeros.clayGuardPhaseHoldSeconds = 0.0;
            zeros.clayGuardPhaseReleaseSeconds = 0.0;
            zeros.clayGuardWidthAttackSeconds = 0.0;
            zeros.clayGuardWidthReleaseSeconds = 0.0;
            zeros.clayMinCorrelationAfterWidth = 0.0f;

            const auto planned = plannedAuto();
            ClayGuard legacy;
            legacy.setConstants (zeros);

            auto out = legacy.update (planned, invertedNarrow(), 1.0 / clayRate, true);
            expectEquals (legacy.getState().phaseFloor, 1.0f, "zero attack: one sample is enough");
            expectEquals (out.monoAmount, 0.3f, "ADR-015: the mono amount stays at the plan");
            out = legacy.update (planned, correlatedNarrow(), 1.0 / clayRate, true);
            expectEquals (legacy.getState().phaseFloor, 0.0f, "zero hold and release: back at once");
            expect (! legacy.getState().phaseActive);

            const auto wide = makeView (0.8f, true, static_cast<float> (std::pow (10.0, 0.05)), 1.0f, true);
            out = legacy.update (planned, wide, 1.0 / clayRate, true);
            expectWithinAbsoluteError (out.widthDb, 0.5f, 1.0e-5f, "k = 1: allowed 10*log10(P_M / P_S) at once");

            const auto script = blockScript();
            ClayGuard zeroGuard;
            zeroGuard.setConstants (zeros);
            const auto reference = runScript (zeroGuard, script, [] { return 333; }, true);

            const auto nanDouble = std::numeric_limits<double>::quiet_NaN();
            const auto infDouble = std::numeric_limits<double>::infinity();
            const auto nanFloat = std::numeric_limits<float>::quiet_NaN();
            const auto infFloat = std::numeric_limits<float>::infinity();

            glaze::Constants broken[3];
            broken[0] = zeros;
            broken[0].clayGuardPhaseCorrelation = broken[0].clayGuardPhaseFastGate = nanFloat;
            broken[0].clayGuardPhaseMinSeconds = nanDouble;
            broken[0].clayGuardPhaseAttackSeconds = broken[0].clayGuardPhaseHoldSeconds = broken[0].clayGuardPhaseReleaseSeconds = nanDouble;
            broken[0].clayGuardWidthAttackSeconds = broken[0].clayGuardWidthReleaseSeconds = nanDouble;
            broken[1] = zeros;
            broken[1].clayGuardPhaseCorrelation = broken[1].clayGuardPhaseFastGate = infFloat;
            broken[1].clayGuardPhaseMinSeconds = infDouble;
            broken[1].clayGuardPhaseAttackSeconds = broken[1].clayGuardPhaseHoldSeconds = broken[1].clayGuardPhaseReleaseSeconds = infDouble;
            broken[1].clayGuardWidthAttackSeconds = broken[1].clayGuardWidthReleaseSeconds = infDouble;
            broken[2] = zeros;
            broken[2].clayGuardPhaseFastGate = -infFloat;
            broken[2].clayGuardPhaseMinSeconds = -5.0;
            broken[2].clayGuardPhaseAttackSeconds = broken[2].clayGuardPhaseHoldSeconds = broken[2].clayGuardPhaseReleaseSeconds = -5.0;
            broken[2].clayGuardWidthAttackSeconds = broken[2].clayGuardWidthReleaseSeconds = -infDouble;

            for (int variant = 0; variant < 3; ++variant)
            {
                ClayGuard guard;
                guard.setConstants (broken[variant]);
                const auto trace = runScript (guard, script, [] { return 333; }, true);
                int mismatches = 0, nonFinite = 0;

                for (size_t i = 0; i < trace.gridOutputs.size() && i < reference.gridOutputs.size(); ++i)
                {
                    if (! (samePlan (trace.gridOutputs[i], reference.gridOutputs[i]) && sameState (trace.gridStates[i], reference.gridStates[i])))
                        ++mismatches;

                    if (! (std::isfinite (trace.gridOutputs[i].monoAmount) && std::isfinite (trace.gridOutputs[i].widthDb)
                           && std::isfinite (trace.gridStates[i].widthCapDb)))
                        ++nonFinite;
                }

                expectEquals (static_cast<int> (trace.gridOutputs.size()), static_cast<int> (reference.gridOutputs.size()));
                expectEquals (mismatches, 0, "broken constants behave like missing ones, variant " + juce::String (variant));
                expectEquals (nonFinite, 0);
            }

            // ρ_min 与决策器共用 GlazeConstants.h 的 clayWidthPowerScale（T-012 阶段 2 统一口径）：1 或非有限（NaN）→ k = 0，
            // 不许任何加宽（P_S > 0 时，坏快照从严）；−1 或 −∞ → k = +∞，不设上限（上限停在 clayWidthUncappedDb，仍有限）。
            for (const auto strictValue : { 1.0f, nanFloat })
            {
                auto strict = specConstants();
                strict.clayMinCorrelationAfterWidth = strictValue;
                ClayGuard strictGuard;
                strictGuard.setConstants (strict);
                out = feed (strictGuard, planned, correlatedNarrow(), 0.3);
                expectEquals (out.widthDb, 0.0f, "rho_min " + juce::String (strictValue) + ": no automatic widening while there is any side signal");
            }

            for (const auto looseValue : { -1.0f, -infFloat })
            {
                auto loose = specConstants();
                loose.clayMinCorrelationAfterWidth = looseValue;
                ClayGuard looseGuard;
                looseGuard.setConstants (loose);
                out = feed (looseGuard, planned, correlatedVeryWide(), 0.3);
                expectEquals (out.widthDb, 1.2f, "rho_min " + juce::String (looseValue) + ": no cap, the plan passes");
                expect (std::isfinite (looseGuard.getState().widthCapDb));
            }
        }

        // 13. 缺省构造 = 出厂常数：不调 setConstants 的 ClayGuard 与 setConstants(factoryConstants()) 的轨迹逐位相同；
        //     出厂的 9 个数就是规格的数字（改标定时同步改 specConstants 与本条）。
        // 依据：ClayGuard.h「缺省 = 出厂值：Constants 的 clay* 字段默认值就是出厂值」；设计规格-Clay-分析决策与验收 §3 表、
        //    设计规格-Clay-根治修订 3.4（−0.2、0.15 s）与 ADR-014（快路门 0.1）。
        // ADR-014 / ADR-028 修坯压力反馈的第一触发量（ClayGuard.h ClayPressureMonitor；输入 = ClayModule 报的 100 ms 峰值块，T-044 起
        // 与「先试后用」同口径）：
        //   - 代价 = 20·log10(P99 处理后 / P99 干路)，P99 = numpy 默认的线性插值百分位（与 MusicAnalyzer::percentileOfTop 同式）：与存下
        //     全部块、排序后算的参照逐位相同（n = 100、137、1000、12600）；n = 13000 时两边按 exactBlocks 的同一个秩估计（参照取全部块
        //     降序后同一位置）；
        //   - 构造 / reset 之后走完的第一块不攒（它开始时监视器还没看到前面的音频）；有效块不满 100（10 s）不判：99 块时 costReady 为假；
        //   - 每切片一块（0.1 s）、处理后 = 干路 × 10^(0.3/20)：第一块作废 + 100 块攒满，代价 0.3 dB 再持续 1 s → 约 11.1 s 返回 true；
        //     返回时累计清空；冷却 4 s 里的块不攒、跨冷却结束的那块也不攒 → 下一次约在 4 + 0.1 + 10 + 1 = 15.2 s 之后；
        //   - 0.15 dB 恒定永不触发；处理后比干路低 1 dB 永不触发、代价为负；
        //   - 有效性：music = false 的切片里的块不攒；跨切片的块要每片都有效（中途一片 music = false → 那一块不攒、之后的照常）；
        //     dropped 的块不攒、之后的照常；非有限 / 负的块峰值不攒；干路全 0 → 不判；处理后全 0 → 按 −120 dB 封底；
        //   - seconds ≤ 0 的切片：块照攒、代价照算，但持续与冷却的计时不走；reset 清空一切、冷却回到已满；
        //   - nextScale：1 → 0.5 → 0.25 → 0 → 0，NaN → 0。
        // 依据：ClayGuard.h ClayPressureMonitor、MusicAnalyzer.cpp 第「五」节（先试的 P99 定义）、ADR-028、BUG-011。
        static double referencePercentile (std::vector<double> values, int countForPosition)
        {
            std::sort (values.begin(), values.end(), std::greater<double>());   // 降序：第 j 小（0 起）= values[count − 1 − j]
            const auto position = 0.99 * (double) (countForPosition - 1);
            const auto lower = (int) std::floor (position);
            const auto below = values[(size_t) (countForPosition - 1 - lower)];

            if (lower + 1 >= countForPosition)
                return below;

            const auto above = values[(size_t) (countForPosition - 2 - lower)];
            return below + (above - below) * (position - (double) lower);
        }

        void testPressureMonitor()
        {
            beginTest ("Clay pressure monitor (ADR-014 / ADR-028): the P99 peak-block cost, same formula as the trial, above 0.2 dB for 1 s steps the scale down");

            using Blocks = ClayPressureMonitor::PeakBlocks;
            constexpr double slice = 0.1;

            // 每切片走完一块：干路峰值按固定种子在 0.3 ～ 0.6 之间（真素材每块不同），处理后 = 干路 × 10^(ratioDb / 20)。
            const auto feedOne = [] (ClayPressureMonitor& m, juce::Random& rng, double ratioDb, bool music = true, double seconds = 0.1)
            {
                const auto dry = 0.3f + 0.3f * rng.nextFloat();
                const std::array<float, 2> pair { (float) ((double) dry * std::pow (10.0, ratioDb / 20.0)), dry };
                return m.update (Blocks { pair.data(), 1, 0 }, music, seconds);
            };

            const auto feedUntilStep = [&feedOne] (ClayPressureMonitor& m, juce::Random& rng, double ratioDb, double maxSeconds)
            {
                auto t = 0.0;

                for (int k = 1; (double) k * slice <= maxSeconds + 1.0e-9; ++k)
                {
                    t = (double) k * slice;

                    if (feedOne (m, rng, ratioDb))
                        return t;
                }

                return -1.0;
            };

            // 1. 与先试同式：存下全部块排序后的参照逐位相同；超出 exactBlocks 按同一个秩估计。
            {
                juce::Random rng (4401);
                ClayPressureMonitor monitor;
                std::vector<double> wets, drys;
                const std::array<float, 2> prime { 0.9f, 0.9f };
                monitor.update (Blocks { prime.data(), 1, 0 }, true, 0.0);
                expectEquals (monitor.blockCount, 0, "the first block after construction is not kept (it began before the monitor saw anything)");

                for (int n = 1; n <= 13000; ++n)
                {
                    const auto dry = 0.05f + 0.9f * rng.nextFloat();
                    const auto wet = dry * (0.8f + 0.5f * rng.nextFloat());   // 处理后与干路各自排序（同先试），不是逐块成对比
                    const std::array<float, 2> pair { wet, dry };
                    monitor.update (Blocks { pair.data(), 1, 0 }, true, 0.0);   // seconds 0：只攒块，不推进计时、不触发
                    wets.push_back ((double) wet);
                    drys.push_back ((double) dry);

                    if (n == 99)
                        expect (! monitor.costReady() && monitor.costDb() == 0.0, "99 valid blocks (< 10 s): no cost yet");

                    if (n == 100 || n == 137 || n == 1000 || n == ClayPressureMonitor::exactBlocks || n == 13000)
                    {
                        const auto count = juce::jmin (n, ClayPressureMonitor::exactBlocks);
                        const auto expected = 20.0 * std::log10 (referencePercentile (wets, count) / referencePercentile (drys, count));
                        expect (monitor.costReady(), "ready at " + juce::String (n) + " blocks");
                        expectEquals (monitor.costDb(), expected, "P99 cost at " + juce::String (n) + " blocks equals the sorted reference bit for bit");
                    }
                }

                expectEquals (monitor.blockCount, 13000);
            }

            // 2. 持续 0.3 dB：第 100 块起代价 0.3 dB，再连续 1 s → 约 11.0 s 降档；冷却 4 s 不攒 + 重攒 10 s + 持续 1 s → 下一次约 15 s 后。
            {
                juce::Random rng (4402);
                ClayPressureMonitor monitor;
                const auto first = feedUntilStep (monitor, rng, 0.3, 30.0);
                logMessage ("  0.3 dB peak cost: first step after " + juce::String (first, 2) + " s");
                expectWithinAbsoluteError (first, 11.1, 0.15, "a constant 0.3 dB cost: first block dropped, 10 s to accumulate, then 1 s above 0.2 dB");
                expect (monitor.blockCount == 0 && ! monitor.costReady() && monitor.aboveSeconds == 0.0, "a step clears the accumulation");

                const auto second = feedUntilStep (monitor, rng, 0.3, 40.0);
                logMessage ("  second step after another " + juce::String (second, 2) + " s");
                expectWithinAbsoluteError (second, 15.2, 0.2, "the next step: 4 s cooldown (blocks not kept) + the straddling block + 10 s + 1 s");
            }

            // 3. 门槛之下 / 处理后更低：永不触发。
            {
                juce::Random rng (4403);
                ClayPressureMonitor low, quieter;
                expectEquals (feedUntilStep (low, rng, 0.15, 40.0), -1.0, "0.15 dB sustained stays below the 0.2 dB threshold: never steps");
                expectWithinAbsoluteError (low.costDb(), 0.15, 1.0e-5);
                expectEquals (feedUntilStep (quieter, rng, -1.0, 40.0), -1.0, "the processing lowers the top peaks: never steps");
                expect (quieter.costReady() && quieter.costDb() < -0.99, "the cost is negative: " + juce::String (quieter.costDb(), 3));
            }

            // 4. 有效性。
            {
                juce::Random rng (4404);
                ClayPressureMonitor m;
                const std::array<float, 2> pair { 0.5f, 0.4f };
                m.update (Blocks { pair.data(), 1, 0 }, true, slice);
                expectEquals (m.blockCount, 0, "the first block after construction is not kept");

                for (int k = 0; k < 50; ++k)
                    m.update (Blocks { pair.data(), 1, 0 }, true, slice);

                for (int k = 0; k < 50; ++k)
                    m.update (Blocks { pair.data(), 1, 0 }, false, 0.0);

                expectEquals (m.blockCount, 50, "blocks completed outside effective music are not kept");

                m.update (Blocks { pair.data(), 1, 0 }, true, slice);
                expectEquals (m.blockCount, 50, "the block straddling the gap (part of it outside music) is not kept either");
                m.update (Blocks { pair.data(), 1, 0 }, true, slice);
                expectEquals (m.blockCount, 51, "the next whole block is kept");

                m.update (Blocks {}, true, 0.05);    // 半块在有效音乐里……
                m.update (Blocks {}, false, 0.0);    // ……中途有一片不在
                m.update (Blocks { pair.data(), 1, 0 }, true, 0.05);
                expectEquals (m.blockCount, 51, "a block spanning several slices must be in music in every one of them");

                const std::array<float, 4> two { 0.5f, 0.4f, 0.5f, 0.4f };
                m.update (Blocks { pair.data(), 1, 0 }, true, slice);
                m.update (Blocks { two.data(), 2, 3 }, true, slice);
                expectEquals (m.blockCount, 54, "reported blocks are kept, dropped ones are not");
                m.update (Blocks { pair.data(), 1, 0 }, true, slice);
                expectEquals (m.blockCount, 55, "the block after a slice with drops is kept (it lies wholly in the next slice)");

                const auto nan = std::numeric_limits<float>::quiet_NaN();
                const std::array<float, 2> badWet { nan, 0.4f }, badDry { 0.5f, -1.0f }, infWet { std::numeric_limits<float>::infinity(), 0.4f };
                m.update (Blocks { badWet.data(), 1, 0 }, true, slice);
                m.update (Blocks { badDry.data(), 1, 0 }, true, slice);
                m.update (Blocks { infWet.data(), 1, 0 }, true, slice);
                expectEquals (m.blockCount, 55, "non-finite or negative block peaks are not kept");
                m.update (Blocks { nullptr, 3, 0 }, true, slice);
                expectEquals (m.blockCount, 55, "a null block pointer reports nothing");

                ClayPressureMonitor silentDry, silentWet;
                const std::array<float, 2> zeroDry { 0.0f, 0.0f }, zeroWet { 0.0f, 0.5f };

                for (int k = 0; k < 150; ++k)
                {
                    silentDry.update (Blocks { zeroDry.data(), 1, 0 }, true, slice);
                    silentWet.update (Blocks { zeroWet.data(), 1, 0 }, true, slice);
                }

                expect (! silentDry.costReady(), "all-zero dry peaks: no cost (no division by zero)");
                expectWithinAbsoluteError (silentWet.costDb(), -120.0, 1.0e-9, "all-zero processed peaks: floored at −120 dB");

                // seconds ≤ 0：块照攒、代价照算，计时不走。
                ClayPressureMonitor frozen;
                const auto before = frozen.sinceStep;

                for (int k = 0; k < 150; ++k)
                    expect (! feedOne (frozen, rng, 1.0, true, 0.0), "no decision while the slices carry no seconds");

                expect (frozen.costReady() && frozen.costDb() > 0.99, "blocks still accumulate");
                expect (frozen.aboveSeconds == 0.0 && frozen.sinceStep == before, "the sustain and cooldown clocks do not move");

                frozen.reset();
                expect (frozen.blockCount == 0 && ! frozen.costReady() && frozen.aboveSeconds == 0.0
                            && frozen.sinceStep == ClayPressureMonitor::cooldownSeconds && ! frozen.openBlockClean,
                        "reset clears everything and leaves the cooldown satisfied");
            }

            expectEquals (ClayPressureMonitor::nextScale (1.0f), 0.5f);
            expectEquals (ClayPressureMonitor::nextScale (0.5f), 0.25f);
            expectEquals (ClayPressureMonitor::nextScale (0.25f), 0.0f);
            expectEquals (ClayPressureMonitor::nextScale (0.0f), 0.0f);
            expectEquals (ClayPressureMonitor::nextScale (std::numeric_limits<float>::quiet_NaN()), 0.0f);
        }

        // T-043（ADR-027）压力反馈的第二个触发量：守护的混合损失（ClayGuard.h ClayPressureMonitor）。
        //   - 两份能量按 blendSeconds 一阶平滑（2 s）后求比；本该 / 实际的比恒定时平滑后的比从第一段起就恒等于它（同一个衰减乘在
        //     两边）：恒定 0.2 dB > 0.15 dB → 连续 1 s 后（约 1.00 s，冷却初值已满）返回 true；返回时两份平滑能量与计时清零；
        //   - 冷却与第一条共用：混合损失触发之后，混合损失要等满 4 s；峰值块代价（第一条，ADR-028）冷却期里的块不攒，要 4 s + 重攒
        //     10 s + 持续 1 s；
        //   - 0.14 dB 恒定永不触发；能量非有限 / 为负、blendSeconds ≤ 0 / 非有限 → 这一段不计（冻结）；reset 清零；
        //   - 混合损失从未超过门槛时，与只有第一条的轨迹逐位相同（默认参数 = 显式传 0）；
        //   - 第一条的门在 seconds：seconds ≤ 0 的段只推进混合损失的平滑与计时，不判断触发。
        void testPressureMonitorBlendLoss()
        {
            beginTest ("Clay pressure monitor, second trigger (T-043): smoothed guard blend loss above 0.15 dB for 1 s steps down, sharing the cooldown");

            using Blocks = ClayPressureMonitor::PeakBlocks;
            constexpr double slice = 0.01;
            constexpr double intended = 1.0e-3;
            const auto outputFor = [] (double lossDb) { return intended * std::pow (10.0, -lossDb / 10.0); };
            const auto feedBlendUntilStep = [&outputFor] (ClayPressureMonitor& m, double lossDb, double maxSeconds)
            {
                for (double t = slice; t <= maxSeconds + 1.0e-9; t += slice)
                    if (m.update (Blocks {}, true, slice, intended, outputFor (lossDb), slice))
                        return t;

                return -1.0;
            };

            ClayPressureMonitor monitor;
            const auto first = feedBlendUntilStep (monitor, 0.2, 10.0);
            logMessage ("  0.2 dB blend loss: first step after " + juce::String (first, 3) + " s");
            expectWithinAbsoluteError (first, 1.0, 2.0 * slice, "a constant 0.2 dB loss is above 0.15 dB from the first slice: step after 1 s");
            expect (monitor.blendIntendedSmoothed == 0.0 && monitor.blendOutputSmoothed == 0.0 && monitor.blendAboveSeconds == 0.0,
                    "a step clears the blend smoother and its sustain count");

            const auto second = feedBlendUntilStep (monitor, 0.2, 10.0);
            expectWithinAbsoluteError (second, 4.0, 2.0 * slice, "the next step waits for the shared 4 s cooldown");

            // 共用冷却：混合损失触发之后，峰值块代价 0.5 dB（每 0.1 s 一块）冷却期里不攒，要 4 s + 10 s + 1 s。
            ClayPressureMonitor shared;
            expect (feedBlendUntilStep (shared, 0.3, 5.0) > 0.0, "positive control: the blend trigger fires");
            auto peakStep = -1.0;
            juce::Random rng (4405);

            for (int k = 1; k <= 300 && peakStep < 0.0; ++k)
            {
                const auto dry = 0.3f + 0.3f * rng.nextFloat();
                const std::array<float, 2> pair { (float) ((double) dry * std::pow (10.0, 0.5 / 20.0)), dry };

                if (shared.update (Blocks { pair.data(), 1, 0 }, true, 0.1))
                    peakStep = 0.1 * (double) k;
            }

            expectWithinAbsoluteError (peakStep, 15.2, 0.2, "a peak-cost step after a blend step waits for the cooldown, then re-accumulates 10 s");

            ClayPressureMonitor below;
            expectEquals (feedBlendUntilStep (below, 0.14, 30.0), -1.0, "0.14 dB sustained stays below the 0.15 dB threshold: never steps");
            expectWithinAbsoluteError (below.blendLossDb(), 0.14, 1.0e-9, "blendLossDb reads the smoothed loss");

            // 冻结与坏值：这一段不计。
            ClayPressureMonitor frozen;
            frozen.update (Blocks {}, true, slice, intended, outputFor (0.2), slice);
            const auto i0 = frozen.blendIntendedSmoothed, o0 = frozen.blendOutputSmoothed, a0 = frozen.blendAboveSeconds;
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            frozen.update (Blocks {}, true, slice, intended, outputFor (0.2), 0.0);
            frozen.update (Blocks {}, true, slice, intended, outputFor (0.2), -1.0);
            frozen.update (Blocks {}, true, slice, intended, outputFor (0.2), nan);
            frozen.update (Blocks {}, true, slice, nan, outputFor (0.2), slice);
            frozen.update (Blocks {}, true, slice, intended, -1.0, slice);
            frozen.update (Blocks {}, true, slice, std::numeric_limits<double>::infinity(), outputFor (0.2), slice);
            expect (frozen.blendIntendedSmoothed == i0 && frozen.blendOutputSmoothed == o0 && frozen.blendAboveSeconds == a0,
                    "blendSeconds <= 0 / NaN and non-finite or negative energies leave the blend state untouched");

            ClayPressureMonitor empty;
            expectEquals (empty.blendLossDb(), 0.0, "no energy yet: 0 dB");
            empty.update (Blocks {}, true, slice, intended, outputFor (0.3), slice);
            expect (empty.blendLossDb() > 0.29, "positive control: one slice gives the loss");
            empty.reset();
            expect (empty.blendIntendedSmoothed == 0.0 && empty.blendOutputSmoothed == 0.0 && empty.blendAboveSeconds == 0.0
                        && empty.blendLossDb() == 0.0, "reset clears the blend state");

            // 第一条不受影响：默认参数与显式传 0 的两条轨迹逐位相同（触发时刻、代价、计时）。
            ClayPressureMonitor defaults, zeros;
            auto stepsDefault = 0, stepsZero = 0;
            juce::Random wobble (4406);

            for (int k = 0; k < 2000; ++k)
            {
                const auto dry = 0.3f + 0.3f * wobble.nextFloat();
                const auto ratioDb = 0.25 + 0.2 * std::sin (0.01 * (double) k);
                const std::array<float, 2> pair { (float) ((double) dry * std::pow (10.0, ratioDb / 20.0)), dry };
                stepsDefault += defaults.update (Blocks { pair.data(), 1, 0 }, true, 0.1) ? 1 : 0;
                stepsZero += zeros.update (Blocks { pair.data(), 1, 0 }, true, 0.1, 0.0, 0.0, 0.0) ? 1 : 0;
                expect (defaults.cost == zeros.cost && defaults.aboveSeconds == zeros.aboveSeconds && defaults.sinceStep == zeros.sinceStep
                        && defaults.blockCount == zeros.blockCount);
            }

            expectEquals (stepsZero, stepsDefault);
            expect (stepsDefault > 0, "positive control: the peak-cost trigger fires on this trajectory");

            // seconds ≤ 0 的段：混合损失照样平滑、计时（> 1 s），但不判断；下一段 seconds > 0 时立即触发。
            ClayPressureMonitor gated;

            for (int k = 0; k < 150; ++k)
                expect (! gated.update (Blocks {}, true, 0.0, intended, outputFor (0.2), slice), "no decision while the slice has no seconds");

            expect (gated.blendAboveSeconds >= 1.0, "the blend sustain still counts");
            expect (gated.update (Blocks {}, true, slice, intended, outputFor (0.2), slice), "the first slice with seconds steps down");
        }

        // ADR-014 scaleClayPlan（压力反馈的缩放作用在自动方案上）：方案 = 低切 20 Hz、收单 0.8 @ 120 Hz、加宽 1.2 dB、原有代价位 = 加宽，地板 10 Hz：
        //   scale 1 → 逐位原样；0.5 → 低切 10 · 2^0.5 = 14.1421 Hz（k 域减半）、收单 0.4 @ 120 Hz、加宽 0.6 dB，三位都置上；
        //   0 → 低切关、收单 0 且分频点回 80 Hz、加宽 0，三位都置上；NaN → 按 0；
        //   中性方案缩了也还是中性、不置位；低切正好在地板上时缩放不改频率（但置位）。
        void testScaleClayPlan()
        {
            beginTest ("scaleClayPlan (ADR-014): scales mono / width linearly and the low cut in the k domain, marking the cost bits");

            ClayPlan plan;
            plan.lowCutHz = 20.0f;
            plan.monoHz = 120.0f;
            plan.monoAmount = 0.8f;
            plan.widthDb = 1.2f;
            plan.costLimitedMask = glaze::clayCostLimitedWidth;
            const auto all = (int) (glaze::clayCostLimitedLowCut | glaze::clayCostLimitedMono | glaze::clayCostLimitedWidth);

            expect (samePlan (glaze::scaleClayPlan (plan, 1.0f, 10.0f), plan), "scale 1: unchanged, bit for bit");

            const auto half = glaze::scaleClayPlan (plan, 0.5f, 10.0f);
            expectWithinAbsoluteError (half.lowCutHz, 14.142136f, 1.0e-4f, "scale 0.5: low cut halves k");
            expectWithinAbsoluteError (half.monoAmount, 0.4f, 1.0e-7f);
            expectEquals (half.monoHz, 120.0f);
            expectWithinAbsoluteError (half.widthDb, 0.6f, 1.0e-7f);
            expectEquals ((int) half.costLimitedMask, all, "scale 0.5: all three bits");

            for (const auto zero : { 0.0f, std::numeric_limits<float>::quiet_NaN() })
            {
                const auto off = glaze::scaleClayPlan (plan, zero, 10.0f);
                expect (off.lowCutHz == 0.0f && off.monoAmount == 0.0f && off.widthDb == 0.0f && off.monoHz == ClayPlan {}.monoHz,
                        "scale 0 / NaN: every item neutral, crossover back to the default");
                expectEquals ((int) off.costLimitedMask, all);
            }

            expect (samePlan (glaze::scaleClayPlan (ClayPlan {}, 0.25f, 10.0f), ClayPlan {}), "a neutral plan stays neutral with no bits");

            ClayPlan atFloor;
            atFloor.lowCutHz = 10.0f;
            const auto floorScaled = glaze::scaleClayPlan (atFloor, 0.5f, 10.0f);
            expectEquals (floorScaled.lowCutHz, 10.0f, "a low cut at the floor keeps its frequency");
            expectEquals ((int) floorScaled.costLimitedMask, (int) glaze::clayCostLimitedLowCut);
        }

        void testConstantsAndDefaults()
        {
            beginTest ("a default-constructed guard uses the factory constants, which are the spec's numbers");

            const auto& factory = glaze::factoryConstants();
            expectEquals (factory.clayGuardPhaseCorrelation, -0.2f);
            expectEquals (factory.clayGuardPhaseFastGate, 0.1f);   // ADR-014：0.3 → 0.1
            expectEquals (factory.clayGuardPhaseMinSeconds, 0.15);
            expectEquals (factory.clayGuardPhaseAttackSeconds, 0.1);
            expectEquals (factory.clayGuardPhaseHoldSeconds, 2.0);
            expectEquals (factory.clayGuardPhaseReleaseSeconds, 2.0);
            expectEquals (factory.clayGuardWidthAttackSeconds, 0.2);
            expectEquals (factory.clayGuardWidthReleaseSeconds, 2.0);
            expectEquals (factory.clayMinCorrelationAfterWidth, 0.25f);

            const auto script = blockScript();
            ClayGuard untouched, configured;
            configured.setConstants (factory);
            const auto a = runScript (untouched, script, [] { return 512; }, true);
            const auto b = runScript (configured, script, [] { return 512; }, true);

            int mismatches = 0;

            for (size_t i = 0; i < a.gridOutputs.size() && i < b.gridOutputs.size(); ++i)
                if (! (samePlan (a.gridOutputs[i], b.gridOutputs[i]) && sameState (a.gridStates[i], b.gridStates[i])))
                    ++mismatches;

            expectEquals (static_cast<int> (a.gridOutputs.size()), static_cast<int> (b.gridOutputs.size()));
            expectEquals (mismatches, 0);
        }

        //==============================================================================
        // 14. resolveClayParams：27 种三态组合 × GLAZE 0 / 50 × Bypass 开关 = 108 种，逐项核对（每项只看自己的三态，
        //     所以同时覆盖了「每项三态 = 9 种单项情形」）：
        //       低切  自动 → GLAZE 50：自动值 22.5 Hz；GLAZE 0：0（关）｜关 → 0｜手动 → 33 Hz（GLAZE 0 也生效）
        //       收单  自动 → 110 Hz × 0.7；GLAZE 0：中性 80 Hz × 0｜关 → 中性｜手动 → 150 Hz × 1
        //       加宽  自动 → +0.9 dB；GLAZE 0：0｜关 → 0｜手动 → −2 dB（负值 = 收窄，只能手动）
        //       Bypass = 用户的开关。
        // 依据：设计规格-Clay §5 三态表；ClayGuard.h resolveClayParams 说明；ClayGuard.cpp「monoHz 取中性默认」的理由。
        void testResolveCombinations()
        {
            beginTest ("resolveClayParams: 27 tri-state combinations x GLAZE 0 / 50 x bypass; manual works at GLAZE 0, off is always neutral");

            ClayPlan autoValues;
            autoValues.lowCutHz = 22.5f;
            autoValues.monoHz = 110.0f;
            autoValues.monoAmount = 0.7f;
            autoValues.widthDb = 0.9f;

            ClaySettings user;
            user.lowCutHz = 33.0f;
            user.monoHz = 150.0f;
            user.widthDb = -2.0f;

            const ClayModule::Params neutral {};
            expectEquals (neutral.lowCutHz, 0.0f);
            expectEquals (neutral.monoHz, 80.0f);
            expectEquals (neutral.monoAmount, 0.0f);
            expectEquals (neutral.widthDb, 0.0f);

            const ClayMode modes[] { ClayMode::automatic, ClayMode::off, ClayMode::manual };
            int checked = 0, failures = 0;

            for (const auto glazePercent : { 0.0f, 50.0f })
                for (const auto lowCutMode : modes)
                    for (const auto monoMode : modes)
                        for (const auto widthMode : modes)
                            for (const auto bypassOn : { false, true })
                            {
                                user.lowCutMode = lowCutMode;
                                user.monoMode = monoMode;
                                user.widthMode = widthMode;
                                user.bypass = bypassOn;

                                const auto autoOn = glazePercent > 0.0f;
                                ClayModule::Params expected;
                                expected.lowCutHz = lowCutMode == ClayMode::manual ? 33.0f
                                                  : (lowCutMode == ClayMode::automatic && autoOn ? 22.5f : 0.0f);
                                expected.monoHz = monoMode == ClayMode::manual ? 150.0f
                                                : (monoMode == ClayMode::automatic && autoOn ? 110.0f : 80.0f);
                                expected.monoAmount = monoMode == ClayMode::manual ? 1.0f
                                                    : (monoMode == ClayMode::automatic && autoOn ? 0.7f : 0.0f);
                                expected.widthDb = widthMode == ClayMode::manual ? -2.0f
                                                 : (widthMode == ClayMode::automatic && autoOn ? 0.9f : 0.0f);
                                expected.bypass = bypassOn;

                                const auto actual = glaze::resolveClayParams (user, autoValues, glazePercent);
                                ++checked;

                                if (! sameParams (actual, expected))
                                {
                                    ++failures;
                                    logMessage ("  GLAZE " + juce::String (glazePercent) + " modes " + juce::String (static_cast<int> (lowCutMode))
                                                + juce::String (static_cast<int> (monoMode)) + juce::String (static_cast<int> (widthMode))
                                                + ": got " + describe (actual) + ", expected " + describe (expected));
                                }
                            }

            expectEquals (checked, 108);
            expectEquals (failures, 0);

            // 最要紧的两条单独再写一遍：抽屉默认 + GLAZE 0 → 全中性（整链逐位不变的前提）；全手动 + GLAZE 0 → 严格等于旋钮值。
            const ClaySettings drawerDefault {};
            expect (sameParams (glaze::resolveClayParams (drawerDefault, autoValues, 0.0f), neutral), "default drawer at GLAZE 0 is neutral");

            ClaySettings allManual = user;
            allManual.lowCutMode = allManual.monoMode = allManual.widthMode = ClayMode::manual;
            allManual.bypass = false;
            const auto manualAtZero = glaze::resolveClayParams (allManual, autoValues, 0.0f);
            expectEquals (manualAtZero.lowCutHz, 33.0f);
            expectEquals (manualAtZero.monoHz, 150.0f);
            expectEquals (manualAtZero.monoAmount, 1.0f);
            expectEquals (manualAtZero.widthDb, -2.0f);
        }

        // 15. 钳制与防呆：自动值越界 → 钳到 ClayModule 范围（低切 5 → 10、100 → 60；分频 20 → 40、500 → 300；比例 1.5 → 1、−0.2 → 0；
        //     宽度 4 → 3、−5 → −3）；低切 0 保持 0、负值 → 0（关）；非有限 → 中性；手动值越界同样钳；GLAZE NaN / 负 → 自动项中性；
        //     同一输入两次调用逐位相同（纯函数）。
        // 依据：ClayGuard 任务书「结果钳到 ClayModule 的 min/max 常量（lowCutHz 为 0 时保持 0）」「纯函数」。
        void testResolveClampsAndPurity()
        {
            beginTest ("resolveClayParams clamps to ClayModule ranges, keeps low cut 0 = off, maps non-finite to neutral and is pure");

            const ClaySettings allAuto {};   // 三态缺省都是「自动」
            auto resolveAuto = [&allAuto] (float lowCutValue, float monoHzValue, float amountValue, float widthValue, float glazeValue)
            {
                ClayPlan values;
                values.lowCutHz = lowCutValue;
                values.monoHz = monoHzValue;
                values.monoAmount = amountValue;
                values.widthDb = widthValue;
                return glaze::resolveClayParams (allAuto, values, glazeValue);
            };

            auto p = resolveAuto (5.0f, 20.0f, 1.5f, 4.0f, 50.0f);
            expectEquals (p.lowCutHz, ClayModule::minLowCutHz);
            expectEquals (p.monoHz, ClayModule::minMonoHz);
            expectEquals (p.monoAmount, 1.0f);
            expectEquals (p.widthDb, ClayModule::maxWidthDb);

            p = resolveAuto (100.0f, 500.0f, 0.5f, -5.0f, 50.0f);
            expectEquals (p.lowCutHz, ClayModule::maxLowCutHz);
            expectEquals (p.monoHz, ClayModule::maxMonoHz);
            expectEquals (p.monoAmount, 0.5f);
            expectEquals (p.widthDb, ClayModule::minWidthDb);

            p = resolveAuto (0.0f, 120.0f, 0.5f, 1.0f, 50.0f);
            expectEquals (p.lowCutHz, 0.0f, "low cut 0 = off stays 0 (not clamped up to 10 Hz)");
            expectEquals (p.monoHz, 120.0f);
            expectEquals (p.monoAmount, 0.5f);
            expectEquals (p.widthDb, 1.0f);

            p = resolveAuto (-3.0f, 120.0f, 0.5f, 1.0f, 50.0f);
            expectEquals (p.lowCutHz, 0.0f, "negative low cut = off");

            // 收单比例为 0（方案本身为 0，或负值钳到 0）：分频点没有意义，交中性 80 Hz——决策器给的 120 Hz、旧快照缺字段时的
            // 0 Hz（钳制本会给 40 Hz）都不会让模块的分频点动起来。−0 归成 +0。
            p = resolveAuto (22.0f, 120.0f, 0.0f, 1.0f, 50.0f);
            expectEquals (p.monoHz, 80.0f, "amount 0: crossover meaningless -> neutral 80 Hz");
            p = resolveAuto (22.0f, 0.0f, -0.2f, 1.0f, 50.0f);
            expectEquals (p.monoAmount, 0.0f);
            expectEquals (p.monoHz, 80.0f, "negative amount clamps to 0 -> neutral crossover, not the clamped 40 Hz");
            p = resolveAuto (22.0f, 120.0f, -0.0f, -0.0f, 50.0f);
            expect (bitsOf (p.monoAmount) == bitsOf (0.0f), "-0 amount becomes +0");
            expect (bitsOf (p.widthDb) == bitsOf (0.0f), "-0 width becomes +0");

            const auto qnan = std::numeric_limits<float>::quiet_NaN();
            const auto posInf = std::numeric_limits<float>::infinity();
            const ClayModule::Params neutral {};

            expect (sameParams (resolveAuto (qnan, qnan, qnan, qnan, 50.0f), neutral), "NaN automatic values -> neutral");
            expect (sameParams (resolveAuto (posInf, posInf, posInf, posInf, 50.0f), neutral), "infinite automatic values -> neutral");
            expect (sameParams (resolveAuto (22.0f, 110.0f, 0.7f, 0.9f, qnan), neutral), "GLAZE NaN counts as 0");
            expect (sameParams (resolveAuto (22.0f, 110.0f, 0.7f, 0.9f, -10.0f), neutral), "negative GLAZE counts as 0");

            ClaySettings manual;
            manual.lowCutMode = manual.monoMode = manual.widthMode = ClayMode::manual;
            manual.lowCutHz = 70.0f;
            manual.monoHz = 30.0f;
            manual.widthDb = 3.5f;
            p = glaze::resolveClayParams (manual, ClayPlan {}, 0.0f);
            expectEquals (p.lowCutHz, ClayModule::maxLowCutHz);
            expectEquals (p.monoHz, ClayModule::minMonoHz);
            expectEquals (p.widthDb, ClayModule::maxWidthDb);

            manual.lowCutHz = 5.0f;
            manual.monoHz = 400.0f;
            manual.widthDb = -4.0f;
            p = glaze::resolveClayParams (manual, ClayPlan {}, 0.0f);
            expectEquals (p.lowCutHz, ClayModule::minLowCutHz);
            expectEquals (p.monoHz, ClayModule::maxMonoHz);
            expectEquals (p.widthDb, ClayModule::minWidthDb);

            ClaySettings mixed;
            mixed.lowCutMode = ClayMode::manual;
            mixed.monoMode = ClayMode::off;
            mixed.widthMode = ClayMode::automatic;
            mixed.bypass = true;
            ClayPlan values;
            values.lowCutHz = 18.0f;
            values.monoHz = 90.0f;
            values.monoAmount = 0.4f;
            values.widthDb = 0.6f;
            const auto first = glaze::resolveClayParams (mixed, values, 37.5f);
            const auto second = glaze::resolveClayParams (mixed, values, 37.5f);
            expect (sameParams (first, second), "pure: same inputs, same bits");
            expect (first.bypass);
        }

        // 16. 「手动」「关」态不受保护改动：先让两路保护都到位（自动 monoAmount 被拉到 1、自动加宽被收到 0），再按三态合成——
        //     自动项用保护后的值；手动项严格等于旋钮值（收单 200 Hz × 1、加宽 +2.5 dB）；关的项永远中性。
        // 依据：设计规格-Clay-分析决策与验收 §6 第 5 条「『手动』『关』态不受保护改动」；ADR-009 第 4 条。
        void testManualAndOffIgnoreGuard()
        {
            beginTest ("the guard only changes automatic items: manual and off modes ignore an engaged guard");

            ClayGuard guard;
            guard.setConstants (specConstants());
            const auto planned = plannedAuto();
            const auto guarded = feed (guard, planned, makeView (-0.8f, true, 0.5f, 1.0f, true), 0.5);
            expect (guard.getState().phaseActive, "positive control: phase warning on");
            expectEquals (guarded.monoAmount, 0.3f, "ADR-015: the warning does not change the automatic mono amount");
            expectEquals (guarded.widthDb, 0.0f, "positive control: width guard engaged");

            ClaySettings user;
            user.lowCutHz = 30.0f;
            user.monoHz = 200.0f;
            user.widthDb = 2.5f;

            auto p = glaze::resolveClayParams (user, guarded, 50.0f);
            expectEquals (p.lowCutHz, 22.0f, "automatic low cut: the guard never touches it");
            expectEquals (p.monoHz, 100.0f);
            expectEquals (p.monoAmount, 0.3f, "automatic mono: the plan value, untouched by the phase warning");
            expectEquals (p.widthDb, 0.0f, "automatic width: protected value");

            user.lowCutMode = user.monoMode = user.widthMode = ClayMode::manual;
            p = glaze::resolveClayParams (user, guarded, 50.0f);
            expectEquals (p.lowCutHz, 30.0f);
            expectEquals (p.monoHz, 200.0f);
            expectEquals (p.monoAmount, 1.0f);
            expectEquals (p.widthDb, 2.5f, "manual widening is not retracted by the width guard");

            user.lowCutMode = user.monoMode = user.widthMode = ClayMode::off;
            p = glaze::resolveClayParams (user, guarded, 50.0f);
            expect (sameParams (p, ClayModule::Params {}), "off: neutral although both guards are engaged");
        }

        //==============================================================================
        // 17. 构造、update（各种视图 / 块长 / active）、setConstants、setState、getState、reset、resolveClayParams 都不经 operator new 分配。
        // 依据：实现计划「音频线程 0 分配 0 锁」（编排器在音频线程每块调 update 与 resolveClayParams）。
        void testDoesNotAllocate()
        {
            beginTest ("construction, update, setConstants, setState, reset and resolveClayParams do not allocate");

            const auto constants = specConstants();
            const auto planned = plannedAuto();
            ClaySettings user;
            user.monoMode = ClayMode::manual;

            float sink = 0.0f;
            const auto before = allocationCountOnThisThread();

            ClayGuard guard;
            guard.setConstants (constants);

            for (int k = 0; k < 5000; ++k)
            {
                const auto view = withFast (makeView (k % 3 == 0 ? -0.5f : (k % 3 == 1 ? 0.7f : 0.2f), k % 11 != 0,
                                                      1.0f + static_cast<float> (k % 5), static_cast<float> (k % 4), k % 13 != 0),
                                            k % 2 == 0 ? -0.6f : 0.4f, k % 7 != 0);
                const auto seconds = static_cast<double> (1 + (k * 37) % 2048) / clayRate;
                const auto out = guard.update (planned, view, k % 17 == 0 ? 0.0 : seconds, k % 97 != 0);
                sink += out.monoAmount + out.widthDb;

                if (k % 500 == 0)
                    guard.setConstants (constants);

                if (k % 700 == 0)
                    guard.setState (guard.getState());

                if (k % 1300 == 0)
                    guard.reset();

                user.widthMode = k % 2 == 0 ? ClayMode::automatic : ClayMode::off;
                const auto params = glaze::resolveClayParams (user, out, static_cast<float> (k % 100));
                sink += params.monoAmount + params.widthDb;
            }

            // 先把计数读进局部量再断言：函数实参的求值顺序不确定（GCC 从右往左），提示文字的 juce::String 会先构造、被算进计数。

            const auto allocationsInPath = allocationCountOnThisThread() - before;

            expectEquals (static_cast<juce::int64> (allocationsInPath), static_cast<juce::int64> (0));
            expect (std::isfinite (sink));
        }
    };

    static ClayGuardTests clayGuardTests;
}
