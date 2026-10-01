/*
  ==============================================================================

    GlazeStatusTextTests.cpp
    GLAZE 状态行「只显示最要紧的一个」的判定与文字（Source/ui/GlazeStatusText）：规格状态表的八种情形逐字对照、
    优先级、GLAZE 为 0 时只显示学习状态、「校准暂停」何时出现、秒数与数字的格式、View 相等只看显示内容。

    被测代码只依赖 juce_core（与 HelpText 一样进无头测试编译）。只按 GlazeStatusText.h 的公开接口写。中文前缀断言用
    TestHelpers.h 的 utf8StartsWith（审核 V3-01：juce::String::startsWith 对中文 needle 只比前 1/3 的字节）。

    T-038 House 档（设计规格-曲风-House学习方式 第 7、9 节）：house* 字段为默认值（houseActive = false）时逐字同通用档；锁定条件没满足
    时按小节 / 秒写进度（三种文字逐字对照、封顶、坏值）；反相提示是琥珀行（只压不过手动设置、方案受限、画像失配，记下被压的一行，
    GLAZE 0 不显示）；暂定期间的悬停提示何时出现。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/GlazeStatusText.h"
#include "../Source/ui/HelpText.h"

#include <cmath>
#include <limits>

namespace celadon::test
{
    namespace
    {
        namespace gs = ui::glazestatus;
        using glaze::Phase;
        using glaze::Status;
        using glaze::Style;

        // 各状态的典型快照（与编排器会给出的组合一致）。
        Status unlearned()   { return {}; }

        Status listening (float listened = 9.4f, float required = 15.0f)
        {
            Status s;
            s.phase = Phase::listening;
            s.listenedSeconds = listened;
            s.requiredSeconds = required;
            return s;
        }

        Status learned (Phase phase)
        {
            Status s;
            s.phase = phase;
            s.profileReady = true;
            s.planReady = true;
            s.resolvedStyle = Style::warm;
            s.effectiveTargetLufs = -9.6f;
            return s;
        }

        Status locked()
        {
            auto s = learned (Phase::locked);
            s.calibrationValid = true;
            return s;
        }

        juce::String show (const Status& s, float glaze = 50.0f, Style style = Style::automatic, bool listenOn = false)
        {
            return gs::textFor (gs::classify (s, glaze, style, listenOn));
        }

        gs::Tone toneOf (const Status& s, float glaze = 50.0f, Style style = Style::automatic, bool listenOn = false)
        {
            return gs::toneFor (gs::classify (s, glaze, style, listenOn).kind);
        }

        gs::Kind kindOf (const Status& s, float glaze = 50.0f, Style style = Style::automatic, bool listenOn = false)
        {
            return gs::classify (s, glaze, style, listenOn).kind;
        }

        juce::String showEnglish (const Status& s, float glaze = 50.0f, Style style = Style::automatic)
        {
            return gs::textFor (gs::classify (s, glaze, style, false), Language::english);
        }

        // T-038 House 档锁定条件还没满足（编排器给的样子）：按小节、已听 heard、主段落 full。
        Status housePending (Status s, int heard, int full)
        {
            s.houseActive = true;
            s.houseLockPending = true;
            s.houseSegmentMode = glaze::SegmentMode::bars;
            s.houseHeardSegments = heard;
            s.houseTargetSegments = 64;
            s.houseFullSegments = full;
            s.houseRequiredFullSegments = 8;
            return s;
        }

        // 同上，宿主没给拍点：按秒。
        Status housePendingSeconds (Status s, float heardSeconds)
        {
            s.houseActive = true;
            s.houseLockPending = true;
            s.houseSegmentMode = glaze::SegmentMode::seconds;
            s.houseHeardSeconds = heardSeconds;
            s.houseTargetSeconds = 124.0f;
            return s;
        }

        // House 反相提示亮着（编排器把 ClayGuard::getHouseHint 写进 Status）。
        Status houseHinted (Status s, float lossDb = -1.62f)
        {
            s.houseActive = true;
            s.housePhaseHint = true;
            s.housePhaseMonoLossDb = lossDb;
            return s;
        }
    }

    class GlazeStatusTextTests  : public juce::UnitTest
    {
    public:
        GlazeStatusTextTests() : juce::UnitTest ("GlazeStatusText", "Celadon") {}

        void runTest() override
        {
            // 依据：设计规格-GLAZE-界面与实现 第 1 节状态表（文字示例与颜色逐字对照）。
            beginTest ("the eight spec states show the spec text and colour");
            {
                expectEquals (show (unlearned()), utf8 ("未学习 · 播放后自动聆听（导出将用保守方案）"));
                expect (toneOf (unlearned()) == gs::Tone::dim);

                expectEquals (show (listening()), utf8 ("聆听中 9 / 15 s"));
                expect (toneOf (listening()) == gs::Tone::accent);

                expectEquals (show (learned (Phase::calibrating)), utf8 ("校准中"));
                expect (toneOf (learned (Phase::calibrating)) == gs::Tone::accent);

                expectEquals (show (locked()), utf8 ("已锁定 · 自动→温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expect (toneOf (locked()) == gs::Tone::dim);

                auto awaiting = locked();
                awaiting.calibrationValid = false;
                expectEquals (show (awaiting), utf8 ("已锁定 · 待校准（条件已变，稳定后自动校准）"));
                expect (toneOf (awaiting) == gs::Tone::dim);

                auto limited = locked();
                limited.planLimited = true;
                limited.protectionDb = -1.5f;
                expectEquals (show (limited), utf8 ("方案受限 · 已退让 1.5 dB"));
                expect (toneOf (limited) == gs::Tone::amber);

                auto manual = locked();
                manual.limitedByManual = true;
                expectEquals (show (manual), utf8 ("受手动设置限制"));
                expect (toneOf (manual) == gs::Tone::amber);

                auto paused = learned (Phase::calibrating);
                paused.calibrationPaused = true;
                expectEquals (show (paused, 50.0f, Style::automatic, true), utf8 ("校准暂停（旁通）"));
                expect (toneOf (paused, 50.0f, Style::automatic, true) == gs::Tone::dim);
            }

            // 依据：同上「只显示最要紧的一个」；GlazeStatusText.h 优先级表（琥珀 ＞ 梅子青 ＞ 灰青，琥珀之内先写手动设置）。
            beginTest ("priority: manual limit, plan limit, listening, unlearned, pause, calibrating, awaiting, locked");
            {
                auto everything = learned (Phase::calibrating);
                everything.limitedByManual = true;
                everything.planLimited = true;
                everything.relearnInProgress = true;
                everything.calibrationPaused = true;
                expect (kindOf (everything) == gs::Kind::limitedByManual);

                everything.limitedByManual = false;
                expect (kindOf (everything) == gs::Kind::planLimited);

                everything.planLimited = false;
                everything.profileMismatch = true;
                expect (kindOf (everything) == gs::Kind::listening, "relearn in progress shows listening (it is already doing what the mismatch asks)");

                everything.relearnInProgress = false;
                expect (kindOf (everything) == gs::Kind::profileMismatch, "a profile mismatch outranks the calm states");
                expectEquals (show (everything), utf8 ("与聆听时的音乐差很多 · 建议重新聆听"));
                expect (toneOf (everything) == gs::Tone::amber);
                expect (kindOf (everything, 0.0f) != gs::Kind::profileMismatch, "GLAZE 0: nothing automatic to lower, no note");

                everything.profileMismatch = false;
                expect (kindOf (everything) == gs::Kind::calibrationPaused);

                everything.calibrationPaused = false;
                expect (kindOf (everything) == gs::Kind::calibrating);

                auto amberWhileUnlearned = unlearned();
                amberWhileUnlearned.limitedByManual = true;
                expect (kindOf (amberWhileUnlearned) == gs::Kind::limitedByManual, "amber also outranks unlearned");

                auto relearnWhileLocked = locked();
                relearnWhileLocked.relearnInProgress = true;
                relearnWhileLocked.listenedSeconds = 3.0f;
                expectEquals (show (relearnWhileLocked), utf8 ("聆听中 3 / 15 s"));

                expect (kindOf (learned (Phase::planTransition)) == gs::Kind::calibrating, "plan transition reads as calibrating");
            }

            // 依据：v0.6 验收后 Max 报告「一直显示在校准」——宿主停止播放时没有音乐，校准原地不动，状态行不该写「校准中」。
            beginTest ("a stopped host shows the calibration as paused, not as calibrating forever");
            {
                auto stopped = learned (Phase::calibrating);
                stopped.waitingForPlayback = true;
                expectEquals (show (stopped), utf8 ("校准暂停（未播放）"));
                expect (toneOf (stopped) == gs::Tone::dim);

                auto stoppedTransition = learned (Phase::planTransition);
                stoppedTransition.waitingForPlayback = true;
                expectEquals (show (stoppedTransition), utf8 ("校准暂停（未播放）"));

                auto stoppedLocked = locked();
                stoppedLocked.waitingForPlayback = true;
                expect (utf8StartsWith (show (stoppedLocked), "已锁定"), "locked with a valid calibration: nothing is waiting");

                auto stoppedListen = learned (Phase::calibrating);
                stoppedListen.waitingForPlayback = true;
                stoppedListen.calibrationPaused = true;
                expectEquals (show (stoppedListen, 50.0f, Style::automatic, true), utf8 ("校准暂停（旁通）"), "an explicit pause reason comes first");

                expect (gs::classify (stopped, 50.0f, Style::automatic, false) != gs::classify (learned (Phase::calibrating), 50.0f, Style::automatic, false));
            }

            // 依据：同上「GLAZE 为 0 时不显示标记，状态行只显示学习状态」。
            beginTest ("GLAZE 0 shows only the learning state");
            {
                expectEquals (show (unlearned(), 0.0f), utf8 ("未学习 · 播放后自动聆听"));
                expectEquals (show (listening(), 0.0f), utf8 ("聆听中 9 / 15 s"));
                expectEquals (show (locked(), 0.0f), utf8 ("已锁定 · 自动→温润"));

                auto noisy = learned (Phase::calibrating);
                noisy.limitedByManual = true;
                noisy.planLimited = true;
                noisy.calibrationPaused = true;
                noisy.unlearnedOfflineWarning = true;
                expectEquals (show (noisy, 0.0f), utf8 ("已锁定 · 自动→温润"), "no amber, no calibration at GLAZE 0");

                auto exported = unlearned();
                exported.unlearnedOfflineWarning = true;
                expectEquals (show (exported, 0.0f), utf8 ("未学习 · 播放后自动聆听"));
                expectEquals (show (exported, 50.0f), utf8 ("未学习，导出使用保守方案"));

                // NaN 与负数的 GLAZE 当作 0（与音频线程的判断同一写法）。
                expectEquals (show (locked(), std::numeric_limits<float>::quiet_NaN()), utf8 ("已锁定 · 自动→温润"));
                expectEquals (show (locked(), -5.0f), utf8 ("已锁定 · 自动→温润"));
            }

            // 全链 Listen 在计量之后；只有宿主旁通暂停校准，Listen 不改变状态显示。
            beginTest ("calibration pause shows only while calibration would run, with the right reason");
            {
                auto paused = learned (Phase::calibrating);
                paused.calibrationPaused = true;
                expectEquals (show (paused, 50.0f, Style::automatic, false), utf8 ("校准暂停（旁通）"));
                expectEquals (show (learned (Phase::calibrating), 50.0f, Style::automatic, true), utf8 ("校准中"), "Listen does not pause calibration");
                expectEquals (show (paused, 50.0f, Style::automatic, true), utf8 ("校准暂停（旁通）"), "simultaneous Listen does not hide host bypass");

                auto awaitingPaused = locked();
                awaitingPaused.calibrationValid = false;
                awaitingPaused.calibrationPaused = true;
                expect (kindOf (awaitingPaused, 50.0f, Style::automatic, true) == gs::Kind::calibrationPaused);

                auto lockedPaused = locked();
                lockedPaused.calibrationPaused = true;
                expect (kindOf (lockedPaused, 50.0f, Style::automatic, true) == gs::Kind::locked, "nothing to pause when locked and valid");

                auto listeningPaused = listening();
                listeningPaused.calibrationPaused = true;
                expect (kindOf (listeningPaused, 50.0f, Style::automatic, true) == gs::Kind::listening);
            }

            // 依据：同上「聆听中 9 / 15 s」；设计规格-GLAZE-时间线与保存 第 2 节（满 15 s 且置信度够才结束，最长 30 s）。
            beginTest ("listening seconds: floor, capped at the requirement, robust to bad values");
            {
                expectEquals (show (listening (9.99f)), utf8 ("聆听中 9 / 15 s"));
                expectEquals (show (listening (15.0f)), utf8 ("聆听中 15 / 15 s"));
                expectEquals (show (listening (17.0f)), utf8 ("聆听中 15 / 15 s"), "never shows 17 / 15");
                expectEquals (show (listening (22.0f, 30.4f)), utf8 ("聆听中 22 / 30 s"));
                expectEquals (show (listening (0.0f)), utf8 ("聆听中 0 / 15 s"));
                expectEquals (show (listening (std::numeric_limits<float>::quiet_NaN())), utf8 ("聆听中 0 / 15 s"));
                expectEquals (show (listening (-3.0f)), utf8 ("聆听中 0 / 15 s"));
                expectEquals (show (listening (4.0f, std::numeric_limits<float>::quiet_NaN())), utf8 ("聆听中 4 s"));
                expectEquals (show (listening (4.0f, 0.0f)), utf8 ("聆听中 4 s"));
                expectEquals (show (listening (1.0e30f, 1.0e30f)), utf8 ("聆听中 999 / 999 s"), "absurd values stay bounded");
            }

            // 依据：同上「目标写 effectiveTarget」「−9.6 LUFS」「已退让 1.5 dB」。
            beginTest ("numbers: U+2212 minus, one decimal, unknown target omitted");
            {
                auto s = locked();
                s.effectiveTargetLufs = -9.64f;
                expectEquals (show (s), utf8 ("已锁定 · 自动→温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                s.effectiveTargetLufs = -9.66f;
                expectEquals (show (s), utf8 ("已锁定 · 自动→温润 · 目标 \xe2\x88\x92" "9.7 LUFS"));
                s.effectiveTargetLufs = -14.02f;
                expectEquals (show (s), utf8 ("已锁定 · 自动→温润 · 目标 \xe2\x88\x92" "14.0 LUFS"));
                s.effectiveTargetLufs = std::numeric_limits<float>::quiet_NaN();
                expectEquals (show (s), utf8 ("已锁定 · 自动→温润"));
                s.effectiveTargetLufs = std::numeric_limits<float>::infinity();
                expectEquals (show (s), utf8 ("已锁定 · 自动→温润"));

                auto limited = locked();
                limited.planLimited = true;
                limited.protectionDb = -3.0f;
                expectEquals (show (limited), utf8 ("方案受限 · 已退让 3.0 dB"));
                limited.protectionDb = 0.0f;
                expectEquals (show (limited), utf8 ("方案受限"), "no yield to report");
                limited.protectionDb = std::numeric_limits<float>::quiet_NaN();
                expectEquals (show (limited), utf8 ("方案受限"));
            }

            // 依据：同上「选『自动』时状态行写出它此刻偏向哪种（如『自动→温润』）」；风格胶囊顺序 = 参数 glaze_style 选项顺序。
            beginTest ("style text: automatic shows its lean, a chosen style shows only itself");
            {
                expectEquals (show (locked(), 50.0f, Style::warm), utf8 ("已锁定 · 温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (show (locked(), 50.0f, Style::punchy), utf8 ("已锁定 · 冲击 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (show (locked(), 50.0f, Style::clean), utf8 ("已锁定 · 通透 · 目标 \xe2\x88\x92" "9.6 LUFS"));

                auto leaningClean = locked();
                leaningClean.resolvedStyle = Style::clean;
                expectEquals (show (leaningClean, 0.0f), utf8 ("已锁定 · 自动→通透"));

                auto noLean = locked();
                noLean.resolvedStyle = Style::automatic;
                expectEquals (show (noLean, 0.0f), utf8 ("已锁定 · 自动"));

                expect (gs::styleFromParameterValue (0.0f) == Style::automatic);
                expect (gs::styleFromParameterValue (1.0f) == Style::warm);
                expect (gs::styleFromParameterValue (2.0f) == Style::punchy);
                expect (gs::styleFromParameterValue (3.0f) == Style::clean);
                expect (gs::styleFromParameterValue (7.0f) == Style::clean);
                expect (gs::styleFromParameterValue (-2.0f) == Style::automatic);
                expect (gs::styleFromParameterValue (std::numeric_limits<float>::quiet_NaN()) == Style::automatic);

                expectEquals (gs::styleName (Style::automatic), utf8 ("自动"));
                expectEquals (gs::styleName (Style::warm), utf8 ("温润"));
                expectEquals (gs::styleName (Style::punchy), utf8 ("冲击"));
                expectEquals (gs::styleName (Style::clean), utf8 ("通透"));
            }

            // 依据：设计规格-GLAZE-时间线与保存 第 5 节「版本不同时照用保存的自动增量数值……并在界面提示一次」：
            // 读到旧算法存的方案时，已锁定那一行把风格换成「沿用旧版方案」（目标照写）；GLAZE 0 不提示；
            // 更要紧的状态照样压过它；提示与否算进 View 相等（换上 / 撤下都要重画）。
            beginTest ("a plan kept from an older algorithm is noted on the locked line");
            {
                auto older = locked();
                older.savedPlanFromOlderAlgorithm = true;
                expectEquals (show (older), utf8 ("已锁定 · 沿用旧版方案 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (show (older, 50.0f, Style::warm), utf8 ("已锁定 · 沿用旧版方案 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expect (toneOf (older) == gs::Tone::dim);
                expectEquals (show (older, 0.0f), show (locked(), 0.0f), "GLAZE 0: no note (nothing automatic is applied)");

                auto olderAndLimited = older;
                olderAndLimited.limitedByManual = true;
                expectEquals (show (olderAndLimited), utf8 ("受手动设置限制"));

                expect (gs::classify (older, 50.0f, Style::automatic, false) != gs::classify (locked(), 50.0f, Style::automatic, false));
            }

            // 依据：设计规格-GLAZE-界面与实现 第 4 节 + 设计规格-界面 第 6 节（显示内容不变就不重画）：View 相等 ⇔ 画面不变。
            beginTest ("views compare equal when the shown line cannot differ");
            {
                const auto a = gs::classify (listening (9.2f), 0.0f, Style::automatic, false);
                const auto b = gs::classify (listening (9.8f), 80.0f, Style::clean, true);
                expect (a == b, "listening: GLAZE, style, Listen and sub-second progress do not matter");

                auto lockedA = locked();
                auto lockedB = locked();
                lockedB.listenedSeconds = 12.0f;
                lockedB.protectionDb = -0.4f;
                lockedB.effectiveTargetLufs = -9.61f;
                expect (gs::classify (lockedA, 50.0f, Style::automatic, false) == gs::classify (lockedB, 60.0f, Style::automatic, true));

                lockedB.effectiveTargetLufs = -9.7f;
                expect (gs::classify (lockedA, 50.0f, Style::automatic, false) != gs::classify (lockedB, 50.0f, Style::automatic, false),
                        "a shown digit changed");

                expect (gs::classify (locked(), 50.0f, Style::automatic, false) != gs::classify (locked(), 50.0f, Style::warm, false),
                        "chosen style changed");
            }

            // 依据：同上状态表：每种情形都有非空文字与三种颜色之一；风格名与胶囊上的字一致。
            beginTest ("every combination yields a non-empty line");
            {
                const Phase phases[] = { Phase::unlearned, Phase::listening, Phase::planTransition, Phase::calibrating, Phase::locked };

                for (auto phase : phases)
                    for (int flags = 0; flags < 128; ++flags)
                        for (float glaze : { 0.0f, 50.0f })
                        {
                            Status s;
                            s.phase = phase;
                            s.calibrationValid   = (flags & 1) != 0;
                            s.planLimited        = (flags & 2) != 0;
                            s.limitedByManual    = (flags & 4) != 0;
                            s.calibrationPaused  = (flags & 8) != 0;
                            s.relearnInProgress  = (flags & 16) != 0;
                            s.unlearnedOfflineWarning = (flags & 32) != 0;
                            s.profileMismatch    = (flags & 64) != 0;
                            s.effectiveTargetLufs = -9.6f;
                            s.protectionDb = -1.0f;

                            const auto view = gs::classify (s, glaze, Style::automatic, (flags & 8) != 0);
                            const auto text = gs::textFor (view);
                            expect (text.isNotEmpty() && ! text.containsChar ((juce::juce_wchar) 0xFFFD),
                                    "empty or broken text for phase " + juce::String ((int) phase) + " flags " + juce::String (flags));

                            // T-038：同一组合再叠上 House 的四种情形（按小节暂定、按秒暂定、反相提示、二者都有）。
                            for (int house = 0; house < 4; ++house)
                            {
                                auto h = house == 1 ? housePendingSeconds (s, 52.0f) : housePending (s, 23, flags % 3 == 0 ? 0 : 8);
                                h.houseLockPending = house != 2;
                                h.housePhaseHint = house >= 2;
                                h.housePhaseMonoLossDb = -1.62f;

                                for (const auto language : { Language::chinese, Language::english })
                                {
                                    const auto houseText = gs::textFor (gs::classify (h, glaze, Style::automatic, false), language);
                                    expect (houseText.isNotEmpty() && ! houseText.containsChar ((juce::juce_wchar) 0xFFFD),
                                            "empty or broken House text for phase " + juce::String ((int) phase) + " flags " + juce::String (flags));
                                }
                            }
                        }
            }

            // 依据：设计规格-曲风-House学习方式 第 7 节状态行三种文字（逐字对照）；第 3.2 节暂定方案；Status 的 house* 字段说明。
            beginTest ("House: bar / second progress while the lock is pending (spec texts), capped and robust; defaults leave the general line untouched");
            {
                // houseActive = false：其余 house* 字段写了什么都不看，逐字同通用档。
                auto general = listening();
                general.houseLockPending = true;
                general.housePhaseHint = true;
                general.houseHeardSegments = 23;
                expectEquals (show (general), show (listening()), "houseActive false: the House fields are ignored");
                expect (gs::classify (general, 50.0f, Style::automatic, false) == gs::classify (listening(), 50.0f, Style::automatic, false));

                const auto noMain = housePending (listening(), 23, 0);
                expectEquals (show (noMain), utf8 ("House · 已听 23 / 64 小节 · 还没听到主段落"));
                expectEquals (showEnglish (noMain), utf8 ("House \xc2\xb7 23 / 64 bars \xc2\xb7 main section not heard yet"));
                expect (toneOf (noMain) == gs::Tone::accent);
                expect (kindOf (noMain) == gs::Kind::houseListening);
                expectEquals (gs::segmentsFor (gs::classify (noMain, 50.0f, Style::automatic, false), Language::chinese).size(), 3);

                const auto withMain = housePending (listening(), 41, 8);
                expectEquals (show (withMain), utf8 ("House · 已听 41 / 64 小节 · 主段落已听 8 小节"));
                expectEquals (showEnglish (withMain), utf8 ("House \xc2\xb7 41 / 64 bars \xc2\xb7 8 bars of main section"));
                expectEquals (showEnglish (housePending (listening(), 41, 1)), utf8 ("House \xc2\xb7 41 / 64 bars \xc2\xb7 1 bar of main section"));

                const auto seconds = housePendingSeconds (listening(), 52.7f);
                expectEquals (show (seconds), utf8 ("House · 已听 52 / 124 秒（宿主没给拍点）"));
                expectEquals (showEnglish (seconds), utf8 ("House \xc2\xb7 52 / 124 s (no tempo from host)"));
                expect (toneOf (seconds) == gs::Tone::accent);

                // 封顶：小节够了、主段落还不够时停在 64 / 64；坏值不炸。
                expectEquals (show (housePending (listening(), 80, 3)), utf8 ("House · 已听 64 / 64 小节 · 主段落已听 3 小节"));
                expectEquals (show (housePendingSeconds (listening(), 300.0f)), utf8 ("House · 已听 124 / 124 秒（宿主没给拍点）"));
                expectEquals (show (housePendingSeconds (listening(), std::numeric_limits<float>::quiet_NaN())), utf8 ("House · 已听 0 / 124 秒（宿主没给拍点）"));
                expectEquals (show (housePendingSeconds (listening(), -4.0f)), utf8 ("House · 已听 0 / 124 秒（宿主没给拍点）"));

                auto noTarget = housePending (listening(), 23, 0);
                noTarget.houseTargetSegments = 0;
                expectEquals (show (noTarget), utf8 ("House · 已听 23 小节 · 还没听到主段落"), "no target: only what was heard");
                noTarget.houseHeardSegments = -5;
                expectEquals (show (noTarget), utf8 ("House · 已听 0 小节 · 还没听到主段落"));

                auto none = housePending (listening(), 0, 0);
                none.houseSegmentMode = glaze::SegmentMode::none;
                expectEquals (show (none), utf8 ("House · 已听 0 / 64 小节 · 还没听到主段落"), "no segment yet reads as bars");

                // 锁定条件没满足时，暂定方案在过渡 / 已锁定的阶段里也写进度；还没开始播放（未学习）时照通用档；GLAZE 0 也写（学习状态）。
                expect (kindOf (housePending (learned (Phase::planTransition), 30, 0)) == gs::Kind::houseListening);
                expect (kindOf (housePending (locked(), 30, 8)) == gs::Kind::houseListening);
                expectEquals (show (housePending (unlearned(), 0, 0)), show (unlearned()), "not started yet: the general unlearned line");
                expectEquals (show (noMain, 0.0f), utf8 ("House · 已听 23 / 64 小节 · 还没听到主段落"), "GLAZE 0 still shows the learning state");

                auto relearnPending = housePending (locked(), 12, 0);
                relearnPending.relearnInProgress = true;
                expect (kindOf (relearnPending) == gs::Kind::houseListening);

                // 琥珀照样压过它；锁定条件满足后（houseLockPending = false）照通用档。
                auto manual = housePending (listening(), 23, 0);
                manual.limitedByManual = true;
                expect (kindOf (manual) == gs::Kind::limitedByManual);
                auto lockedHouse = housePending (locked(), 64, 8);
                lockedHouse.houseLockPending = false;
                expectEquals (show (lockedHouse), show (locked()), "locked House plan: the general locked line");

                // View 相等只看显示内容：已听小节变了要重画，与 House 无关的量不算。
                expect (gs::classify (housePending (listening(), 23, 0), 50.0f, Style::automatic, false)
                        != gs::classify (housePending (listening(), 24, 0), 50.0f, Style::automatic, false), "a shown bar count changed");
                auto otherSeconds = housePending (listening(), 23, 0);
                otherSeconds.houseHeardSeconds = 99.0f;
                expect (gs::classify (otherSeconds, 50.0f, Style::automatic, false) == gs::classify (noMain, 60.0f, Style::clean, true),
                        "fields not on the line do not change the view");
            }

            // 依据：设计规格-曲风-House学习方式 第 9 节（CLAY 卡片与 GLAZE 状态行写「俱乐部单声道会丢低频（约 x dB），请回混音修」）。
            beginTest ("House phase hint: amber line, outranked only by manual limit / plan limit / mismatch, remembers the covered line, hidden at GLAZE 0");
            {
                const auto hinted = houseHinted (locked());
                expectEquals (show (hinted), utf8 ("俱乐部单声道会丢低频（约 1.6 dB），请回混音修"));
                expectEquals (showEnglish (hinted), juce::String ("Club mono loses low end (about 1.6 dB), fix it in the mix"));
                expect (toneOf (hinted) == gs::Tone::amber);
                expectEquals (gs::segmentsFor (gs::classify (hinted, 50.0f, Style::automatic, false), Language::chinese).size(), 1);

                const auto view = gs::classify (hinted, 50.0f, Style::automatic, false);
                expect (view.kind == gs::Kind::housePhaseHint && view.underlyingKind == gs::Kind::locked, "remembers the locked line it covers");
                expect (gs::classify (houseHinted (housePending (listening(), 41, 8)), 50.0f, Style::automatic, false).underlyingKind
                            == gs::Kind::houseListening, "covers the House progress while the lock is pending");
                expect (gs::classify (houseHinted (learned (Phase::calibrating)), 50.0f, Style::automatic, false).underlyingKind
                            == gs::Kind::calibrating);

                auto manual = houseHinted (locked());
                manual.limitedByManual = true;
                expect (kindOf (manual) == gs::Kind::limitedByManual, "manual limit outranks the hint");
                auto limited = houseHinted (locked());
                limited.planLimited = true;
                expect (kindOf (limited) == gs::Kind::planLimited, "plan limit outranks the hint");
                auto mismatch = houseHinted (locked());
                mismatch.profileMismatch = true;
                expect (kindOf (mismatch) == gs::Kind::profileMismatch, "a profile mismatch outranks the hint");

                expect (kindOf (hinted, 0.0f) == gs::Kind::locked, "GLAZE 0: no amber line");
                auto general = hinted;
                general.houseActive = false;
                expect (kindOf (general) == gs::Kind::locked, "houseActive false: no hint");

                // 损失的写法：0.1 dB、四舍五入、至少 0.1；读不到（非有限）时不写数；荒唐值封顶。
                expectEquals (show (houseHinted (locked(), -0.97f)), utf8 ("俱乐部单声道会丢低频（约 1.0 dB），请回混音修"));
                expectEquals (show (houseHinted (locked(), -0.001f)), utf8 ("俱乐部单声道会丢低频（约 0.1 dB），请回混音修"));
                expectEquals (show (houseHinted (locked(), std::numeric_limits<float>::quiet_NaN())), utf8 ("俱乐部单声道会丢低频，请回混音修"));
                expectEquals (showEnglish (houseHinted (locked(), std::numeric_limits<float>::infinity())),
                              juce::String ("Club mono loses low end, fix it in the mix"));
                expectEquals (show (houseHinted (locked(), -1.0e6f)), utf8 ("俱乐部单声道会丢低频（约 99.9 dB），请回混音修"));

                expect (gs::classify (houseHinted (locked(), -1.62f), 50.0f, Style::automatic, false)
                        != gs::classify (houseHinted (locked(), -1.71f), 50.0f, Style::automatic, false), "a shown loss digit changed");
                expect (gs::classify (houseHinted (locked(), -1.62f), 50.0f, Style::automatic, false)
                        == gs::classify (houseHinted (locked(), -1.58f), 70.0f, Style::warm, true), "same shown loss, same view");
            }

            // 依据：设计规格-曲风-House学习方式 第 7 节「气泡提示（暂定期间）：从 drop 开始播放能学得更快」。
            beginTest ("House learning tip applies whenever the House lock is pending, whatever line is shown");
            {
                Status s;
                expect (! gs::houseLearningTipApplies (s));
                s.houseLockPending = true;
                expect (! gs::houseLearningTipApplies (s), "not House: no tip");
                s.houseActive = true;
                expect (gs::houseLearningTipApplies (s), "House and the lock is pending (even before playback)");
                s.limitedByManual = true;
                expect (gs::houseLearningTipApplies (s), "an amber line on top does not hide the tip");
                s.houseLockPending = false;
                expect (! gs::houseLearningTipApplies (s), "locked: no tip");
            }
        }
    };

    static GlazeStatusTextTests glazeStatusTextTests;
}
