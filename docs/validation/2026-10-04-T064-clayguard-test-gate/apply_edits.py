#!/usr/bin/env python3
"""T-064：ClayGuard 测试的快路门常数与断言（Max 2026-10-04 19:4x 同意做；拍板档 2026-10-04-T-060-反相提示注释清理待Max拍板 第 1、2、3 条）。
只改测试：① specConstants() 的快路门 0.3 → 出厂 0.1（ADR-014）并重设门内用例、加出厂门边界用例；② ADR-015 后恒真的断言换成真能验的；
③ 字符串里的旧名（phase guard / guard floor / guard acts …）改成 ADR-015 的说法。每条先断言 old 恰好出现一次（对不上就停，一个文件都不写）。
用法（仓库根）：python3 docs/validation/2026-10-04-T064-clayguard-test-gate/apply_edits.py [--dry-run]"""
import pathlib, sys

dry = "--dry-run" in sys.argv
CG, CGL, GC, HT = (pathlib.Path("tests") / n for n in ("ClayGuardTests.cpp", "ClayGlazeTests.cpp", "GlazeControllerTests.cpp", "HelpTextTests.cpp"))
ch = []   # (文件, 名字, old, new)
def add(path, name, old, new): ch.append((path, name, old, new))

# ================= ClayGuardTests.cpp =================
add(CG, "specConstants 注释与快路门",
"""        // 规格的反相提示 / 宽度保护时长与阈值（设计规格-Clay-根治修订 3.4 / ADR-012：慢路 < −0.2，或慢路 < 0.3 且快路 < −0.2；连续成立 0.15 s；
        // 攻击 100 ms、保持 2 s、释放 2 s；宽度攻击 200 ms、释放 2 s；ρ_min 0.25）。除快路门外与 GlazeConstants.h 的出厂默认值相同
        //（testConstantsAndDefaults 核对）。快路门仍是根治修订 3.4 的 0.3，ADR-014 起出厂为 0.1；门内的用例（1b、blockScript 第 7 段）
        // 按 0.3 设计。这里显式写出，下面的时间断言以它们为依据。
        glaze::Constants specConstants()
        {
            glaze::Constants c;
            c.clayGuardPhaseCorrelation = -0.2f;
            c.clayGuardPhaseFastGate = 0.3f;
""",
"""        // 反相提示 / 宽度保护的时长与阈值（设计规格-Clay-根治修订 3.4 / ADR-012 / ADR-014：慢路 < −0.2，或慢路 < 0.1 且快路 < −0.2；
        // 连续成立 0.15 s；攻击 100 ms、保持 2 s、释放 2 s；宽度攻击 200 ms、释放 2 s；ρ_min 0.25）。快路门根治修订 3.4 写的是 0.3，
        // ADR-014 起出厂为 0.1，这里用出厂值：与 GlazeConstants.h 的出厂默认值逐位相同（testConstantsAndDefaults 核对 9 个数），
        // 所以下面各用例就是在出厂常数下跑；门内的用例（1b、blockScript 第 7 段）按 0.1 设计。这里显式写出，下面的时间断言以它们为依据。
        glaze::Constants specConstants()
        {
            glaze::Constants c;
            c.clayGuardPhaseCorrelation = -0.2f;
            c.clayGuardPhaseFastGate = 0.1f;
""")

add(CG, "Extremes 记提示强度与亮灯块数",
"""            int passThroughMismatches = 0;   // lowCutHz / monoHz 与方案不逐位相同的块数

            void add (const ClayPlan& output, const ClayPlan& planned)
            {
                minMono = juce::jmin (minMono, output.monoAmount);
""",
"""            int passThroughMismatches = 0;   // lowCutHz / monoHz 与方案不逐位相同的块数
            float maxPhaseFloor = 0.0f;      // 反相提示强度（ADR-015：只提示）的最大值
            int phaseActiveBlocks = 0;       // 反相提示亮着的块数

            void add (const ClayPlan& output, const ClayPlan& planned, const ClayGuardState& state)
            {
                maxPhaseFloor = juce::jmax (maxPhaseFloor, state.phaseFloor);
                phaseActiveBlocks += state.phaseActive ? 1 : 0;
                minMono = juce::jmin (minMono, output.monoAmount);
""")
add(CG, "feed 把状态交给 Extremes",
"                    extremes->add (result, planned);",
"                    extremes->add (result, planned, guard.getState());")

add(CG, "blockScript 第 7、8 段的视图",
"""            // 第 7 段的慢路 0.1 只在 specConstants 的门 0.3 之内；ADR-014 起出厂门是 0.1（严格小于），出厂常数下这一段不触发。
            const auto fastInvertedNarrow = withFast (makeView (0.1f, true, 1.0f, 0.01f, true), -0.5f);   // 慢路在门内（0.1 < 0.3）、快路反相
            const auto gatedFastWide = withFast (makeView (0.5f, true, midPowerForAllowedDb (0.4), 1.0f, true), -0.9f);   // 慢路在门外
""",
"""            const auto fastInvertedNarrow = withFast (makeView (0.05f, true, 1.0f, 0.01f, true), -0.5f);   // 慢路在出厂门内（0.05 < 0.1）、快路反相
            // 第 8 段的慢路 0.25：在根治修订 3.4 的旧门 0.3 之内、在出厂门 0.1 之外——门退回 0.3 时这一段会触发，第 8 段的段末断言会败。
            const auto gatedFastWide = withFast (makeView (0.25f, true, midPowerForAllowedDb (0.4), 1.0f, true), -0.9f);   // 慢路在出厂门外
""")

add(CG, "1b 注释",
"""        //     慢路 −0.25 触发；慢路 −0.15（没到 −0.2，阳性对照的反面）不触发；慢路 0.25（门内）+ 快路 −0.3 触发；
        //     慢路 0.35（门外）+ 快路 −0.9 不触发——宽低音上快路的估计散布再大也触发不了提示（审核 V1-01 的根治）；
        //     门内但快路 −0.15、快路无效、快路 NaN 都不触发（没有加速证据，按慢路那一支判定）；边界值不算（严格小于）；
        //     慢路无效时即使快路 −0.9 也不动（这一路冻结）。
        // 依据：设计规格-Clay-根治修订 3.4「慢路 ρ < −0.2，或（慢路 ρ < 0.3 且快路 ρ < −0.2）」；ClayGuard.cpp 文件头。
        //    门按 specConstants 的 0.3（即根治修订 3.4 的值）；ADR-014 起出厂门是 0.1，上面「门内」的慢路 0.25 在出厂门外。
        void testPhaseTriggerCondition()
        {
            beginTest ("phase trigger: slow < -0.2, or slow < 0.3 with fast < -0.2; a negative fast path above the gate never triggers");
""",
"""        //     慢路 −0.25 触发；慢路 −0.15（没到 −0.2，阳性对照的反面）不触发；慢路 0.05（出厂门 0.1 之内）+ 快路 −0.3 触发；
        //     慢路 0.35（门外）+ 快路 −0.9 不触发——宽低音上快路的估计散布再大也触发不了提示（审核 V1-01 的根治）；
        //     根治修订 3.4 的旧门 0.3 之内、出厂门之外的慢路（0.15 / 0.25）+ 快路 −0.9 同样不触发（ADR-014 把门从 0.3 收到 0.1）；
        //     门内但快路 −0.15、快路无效、快路 NaN 都不触发（没有加速证据，按慢路那一支判定）；边界值不算（严格小于：阈值 −0.2、门 0.1）；
        //     慢路无效时即使快路 −0.9 也不动（这一路冻结）。
        // 依据：设计规格-Clay-根治修订 3.4「慢路 ρ < −0.2，或（慢路 ρ < 门 且快路 ρ < −0.2）」与 ADR-014（门 0.3 → 0.1）；ClayGuard.cpp 文件头。
        //    门取 specConstants 的 0.1（= 出厂值）；缺省构造的保护单元自己的门边界在 testConstantsAndDefaults 里另测。
        void testPhaseTriggerCondition()
        {
            beginTest ("phase trigger: slow < -0.2, or slow < 0.1 (factory gate) with fast < -0.2; a negative fast path above the gate never triggers");
""")
add(CG, "1b 断言",
"""            expect (triggers (withFast (slowOnly (0.25f), -0.3f)), "slow inside the gate (0.25 < 0.3) + fast below -0.2 triggers");
            expect (! triggers (withFast (slowOnly (0.35f), -0.9f)), "slow outside the gate: however negative the fast path, no trigger");
            expect (! triggers (withFast (slowOnly (0.3f), -0.9f)), "the gate itself is outside (strictly below)");
            expect (! triggers (withFast (slowOnly (0.25f), -0.15f)), "inside the gate but fast above -0.2: no trigger");
            expect (! triggers (withFast (slowOnly (0.25f), -0.9f, false)), "an invalid fast path gives no evidence");
            expect (! triggers (withFast (slowOnly (0.25f), std::numeric_limits<float>::quiet_NaN())), "a NaN fast path gives no evidence");
""",
"""            expect (triggers (withFast (slowOnly (0.05f), -0.3f)), "slow inside the gate (0.05 < 0.1) + fast below -0.2 triggers");
            expect (triggers (withFast (slowOnly (std::nextafter (0.1f, 0.0f)), -0.3f)), "just below the gate is still inside");
            expect (! triggers (withFast (slowOnly (0.35f), -0.9f)), "slow outside the gate: however negative the fast path, no trigger");
            expect (! triggers (withFast (slowOnly (0.25f), -0.9f)), "slow 0.25 was inside the old gate 0.3 (spec 3.4) but is outside the factory gate 0.1 (ADR-014)");
            expect (! triggers (withFast (slowOnly (0.15f), -0.9f)), "slow 0.15: outside the factory gate");
            expect (! triggers (withFast (slowOnly (0.1f), -0.9f)), "the gate itself is outside (strictly below)");
            expect (! triggers (withFast (slowOnly (0.05f), -0.15f)), "inside the gate but fast above -0.2: no trigger");
            expect (! triggers (withFast (slowOnly (0.05f), -0.9f, false)), "an invalid fast path gives no evidence");
            expect (! triggers (withFast (slowOnly (0.05f), std::numeric_limits<float>::quiet_NaN())), "a NaN fast path gives no evidence");
""")
add(CG, "1b 慢路无效的冻结",
"                const auto out = feed (guard, planned, withFast (makeView (0.1f, false, 1.0f, 0.01f, true), -0.9f), 1.0);",
"                const auto out = feed (guard, planned, withFast (makeView (0.05f, false, 1.0f, 0.01f, true), -0.9f), 1.0);")

add(CG, "856 fixture 文字",
'expectEquals (guard.getState().phaseFloor, 1.0f, "fixture: fully protecting");',
'expectEquals (guard.getState().phaseFloor, 1.0f, "fixture: the phase warning is at full level");')

add(CG, "954 恒真断言换成真能验的",
"""            expectEquals (extremes.minMono, 0.3f, "phase guard stayed out of it (correlated bass)");
            expectEquals (extremes.passThroughMismatches, 0);
""",
"""            // 反相一路在相关低音上一块都没动：提示强度 0、灯没亮。（原来这里比 minMono == 0.3，但 ADR-015 起收单恒等于方案，
            // 反相提示亮了它也等于 0.3，这条验不出「反相一路没动」。）
            expectEquals (extremes.maxPhaseFloor, 0.0f, "the phase warning stayed out of it (correlated bass): level 0 in every block");
            expectEquals (extremes.phaseActiveBlocks, 0, "and the lamp never lit");
            expectEquals (extremes.minMono, 0.3f, "ADR-015: the mono amount stays at the plan");
            expectEquals (extremes.maxMono, 0.3f);
            expectEquals (extremes.passThroughMismatches, 0);
""")

add(CG, "1003 阳性对照文字",
'expect (before.phaseActive && before.widthActive, "positive control: both guards engaged");',
'expect (before.phaseActive && before.widthActive, "positive control: the phase warning is on and the width guard is engaged");')

add(CG, "1312 文字",
'expect (! s.phaseActive, "no floor: cannot be protecting");',
'expect (! s.phaseActive, "no warning level: the lamp cannot be on");')

add(CG, "2174 文字",
'expect (sameParams (p, ClayModule::Params {}), "off: neutral although both guards are engaged");',
'expect (sameParams (p, ClayModule::Params {}), "off: neutral although the phase warning is on and the width guard is engaged");')

add(CG, "1418 出厂门文字",
'"positive control: slow 0.4 is outside the factory gate 0.3");',
'"positive control: slow 0.4 is outside the factory gate 0.1");')

add(CG, "1566 注释",
"""        //     出厂的 9 个数就是规格的数字（改标定时同步改 specConstants 与本条；快路门例外：specConstants 仍是 3.4 的 0.3，见其注释）。""",
"""        //     出厂的 9 个数就是规格的数字，specConstants() 与它们逐位相同（改标定时同步改 specConstants 与本条，否则本条先败）。""")

add(CG, "blockSize compare：monoError 换成「输出收单 = 方案」",
"""                float floorError = 0.0f, holdError = 0.0f, monoError = 0.0f, widthError = 0.0f;

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
""",
"""                // 脚本里每一段的方案收单都是 plannedAuto().monoAmount；ADR-015 起输出收单逐位 = 方案（原来比「本次轨迹对参考轨迹」的差，
                // 两边恒等于方案，差恒为 0，验不出任何东西）。
                const auto plannedMonoBits = bitsOf (plannedAuto().monoAmount);
                float floorError = 0.0f, holdError = 0.0f, widthError = 0.0f;
                int monoOffPlan = 0;

                for (size_t i = 0; i < runStates.size(); ++i)
                {
                    floorError = juce::jmax (floorError, std::abs (runStates[i].phaseFloor - refStates[i].phaseFloor));
                    holdError = juce::jmax (holdError, std::abs (runStates[i].phaseHoldSeconds - refStates[i].phaseHoldSeconds));
                    monoOffPlan += bitsOf (runOutputs[i].monoAmount) == plannedMonoBits ? 0 : 1;
                    widthError = juce::jmax (widthError, std::abs (runOutputs[i].widthDb - refOutputs[i].widthDb));
                }

                logMessage ("  " + label + ": max |d floor| " + juce::String (floorError, 8) + ", |d hold| " + juce::String (holdError, 8)
                            + " s, mono != plan in " + juce::String (monoOffPlan) + " outputs, |d width| " + juce::String (widthError, 8) + " dB");
                expectLessOrEqual (floorError, 2.0e-3f, label + ": phase floor");
                expectLessOrEqual (holdError, 4.0e-3f, label + ": hold");
                expectEquals (monoOffPlan, 0, label + ": ADR-015, the mono amount equals the plan in every output");
                expectLessOrEqual (widthError, 2.0e-3f, label + ": widthDb");
""")

add(CG, "testConstantsAndDefaults：spec = factory + 缺省构造的出厂门边界",
"""            expectEquals (factory.clayMinCorrelationAfterWidth, 0.25f);

            const auto script = blockScript();
            ClayGuard untouched, configured;
""",
"""            expectEquals (factory.clayMinCorrelationAfterWidth, 0.25f);

            // specConstants()（本文件各用例用的常数）就是出厂的 9 个数，逐位相同；改标定时两处一起改，否则这里先败。
            {
                const auto spec = specConstants();
                expect (bitsOf (spec.clayGuardPhaseCorrelation) == bitsOf (factory.clayGuardPhaseCorrelation), "specConstants: phase threshold");
                expect (bitsOf (spec.clayGuardPhaseFastGate) == bitsOf (factory.clayGuardPhaseFastGate), "specConstants: fast gate");
                expectEquals (spec.clayGuardPhaseMinSeconds, factory.clayGuardPhaseMinSeconds, "specConstants: qualification");
                expectEquals (spec.clayGuardPhaseAttackSeconds, factory.clayGuardPhaseAttackSeconds, "specConstants: attack");
                expectEquals (spec.clayGuardPhaseHoldSeconds, factory.clayGuardPhaseHoldSeconds, "specConstants: hold");
                expectEquals (spec.clayGuardPhaseReleaseSeconds, factory.clayGuardPhaseReleaseSeconds, "specConstants: release");
                expectEquals (spec.clayGuardWidthAttackSeconds, factory.clayGuardWidthAttackSeconds, "specConstants: width attack");
                expectEquals (spec.clayGuardWidthReleaseSeconds, factory.clayGuardWidthReleaseSeconds, "specConstants: width release");
                expect (bitsOf (spec.clayMinCorrelationAfterWidth) == bitsOf (factory.clayMinCorrelationAfterWidth), "specConstants: rho_min");
            }

            // 出厂快路门 0.1（ADR-014；根治修订 3.4 写的是 0.3）：缺省构造的保护单元（不调 setConstants）自己守住门的边界——
            // 慢路 0.05 在门内、快路 −0.3 → 亮；慢路正好 0.1（门本身，严格小于）、0.15、0.25（旧门 0.3 之内）+ 快路 −0.9 → 不亮。
            // 这几条不经 specConstants，所以以后改了 specConstants 也不会让门边界失去对出厂常数的覆盖。
            {
                const auto lights = [] (float slow, float fast)
                {
                    ClayGuard fresh;
                    feed (fresh, plannedAuto(), withFast (makeView (slow, true, 1.0f, 0.01f, true), fast), 0.5);
                    return fresh.getState().phaseActive && fresh.getState().phaseFloor > 0.0f;
                };

                expect (lights (0.05f, -0.3f), "factory gate 0.1: slow 0.05 + fast -0.3 lights the warning");
                expect (lights (std::nextafter (0.1f, 0.0f), -0.3f), "factory gate: just below 0.1 is still inside");
                expect (! lights (0.1f, -0.9f), "factory gate: 0.1 itself is outside (strictly below)");
                expect (! lights (0.15f, -0.9f), "factory gate: slow 0.15 is outside");
                expect (! lights (0.25f, -0.9f), "factory gate: slow 0.25 was inside the old gate 0.3 (spec 3.4), outside since ADR-014");
            }

            const auto script = blockScript();
            ClayGuard untouched, configured;
""")

# ================= ClayGlazeTests.cpp =================
add(CGL, "statusText 日志文字",
'                 + ", phase guard " + juce::String (static_cast<int> (s.clayPhaseGuardActive))',
'                 + ", phase warning " + juce::String (static_cast<int> (s.clayPhaseGuardActive))')
add(CGL, "路径统计日志文字",
'+ ", off " + juce::String (offBlocks) + ", bypass " + juce::String (bypassBlocks) + ", phase protection "',
'+ ", off " + juce::String (offBlocks) + ", bypass " + juce::String (bypassBlocks) + ", phase warning "')
add(CGL, "测试名：宽低音从不触发",
'beginTest ("wide stereo bass (long-term rho ~0.8) and mono kick + independent wide pad: the phase guard never triggers in 30 s");',
'beginTest ("wide stereo bass (long-term rho ~0.8) and mono kick + independent wide pad: the phase warning never lights in 30 s");')
add(CGL, "事件日志文字",
'logMessage ("  [" + label + "] phase-guard events " + juce::String (events)',
'logMessage ("  [" + label + "] phase-warning events " + juce::String (events)')
add(CGL, "阳性对照文字",
'expect (events >= 1 && largestFloor > 0.99f, label + ": the guard does trigger (the count is not vacuous)");',
'expect (events >= 1 && largestFloor > 0.99f, label + ": the warning does light (the count is not vacuous)");')
add(CGL, "阴性文字",
'expect (events == 0 && largestFloor == 0.0f, label + ": no phase-guard trigger in 30 s");',
'expect (events == 0 && largestFloor == 0.0f, label + ": no phase warning in 30 s");')

# ================= GlazeControllerTests.cpp =================
add(GC, "负的提示强度",
'expect (rejects (bad), "a negative guard floor");',
'expect (rejects (bad), "a negative phase warning level (phaseFloor)");')
add(GC, "整链经过的路径",
'"the run went through the phase guard, the width guard, a restored first block and a plan transition");',
'"the run went through the phase warning, the width guard, a restored first block and a plan transition");')

# ================= HelpTextTests.cpp =================
add(HT, "测试名 478",
'beginTest ("Clay tooltip line 3 (ADR-012): relearn hint first, only automatic items, not while that item\'s guard acts, cleared by bypass");',
'beginTest ("Clay tooltip line 3 (ADR-012): relearn hint first, only automatic items, not while the mono warning is on or the width guard acts, cleared by bypass");')
add(HT, "收单 501",
'expectEquals (thirdLineFor (guardedMono), advice, "mono guard acting: no cost line for mono");',
'expectEquals (thirdLineFor (guardedMono), advice, "mono warning on: no cost line for mono");')
add(HT, "收单 506",
'expectEquals (thirdLineFor (guardOver), utf8 ("本曲试过：收单会抬高峰值，已自动跳过。"), "positive control: guard released");',
'expectEquals (thirdLineFor (guardOver), utf8 ("本曲试过：收单会抬高峰值，已自动跳过。"), "positive control: warning off");')

texts, bad = {}, 0
for path, name, old, new in ch:
    text = texts.setdefault(path, path.read_text(encoding="utf-8"))
    n = text.count(old)
    print(f"{'OK ' if n == 1 else '*** '} {path.name}: {name}（匹配 {n} 处）")
    if n != 1:
        bad += 1
        continue
    texts[path] = text.replace(old, new, 1)
if bad:
    sys.exit(f"{bad} 条对不上，一个文件都没写")
if not dry:
    for path, text in texts.items():
        path.write_text(text, encoding="utf-8")
    print(f"已写入 {len(texts)} 个文件")
