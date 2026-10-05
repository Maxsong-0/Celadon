/*
  ==============================================================================

    GlazeStatusTextTests.cpp
    T-048 阶段 2：状态行只剩草图第 1 节的七条，加上 GLAZE ≤ 0 的「只用手动设置」。
    聆听 / 锁定 / 校准 / 重新聆听 / House 锁定进度的文案不再是产品。

    生产 glazestatus::classify 仍是阶段 1 的签名（代理 C 未合入）。七条对着 tests/Phase2Contract.h。
    两条必须与现状逐字相同的，另外拿生产函数核对：受手动设置限制、House 反相提示。
    「正在听原声」是等响 A/B，不是学习，留在 Localisation。

    classify 不再收 listenOn（它只为了「试听不暂停校准」）。

  ==============================================================================
*/

#include "Phase2Contract.h"
#include "TestHelpers.h"

#include "../Source/ui/GlazeStatusText.h"
#include "../Source/ui/Localisation.h"

#include <cmath>
#include <limits>

namespace celadon::test
{
    namespace
    {
        namespace gs = ui::glazestatus;
        using glaze::Style;
        using phase2::Kind;
        using phase2::Status;

        Status following()
        {
            Status status;
            status.confidence = 1.0f;
            status.currentMainness = 1.0f;
            status.resolvedStyle = Style::warm;
            status.effectiveTargetLufs = -9.6f;
            return status;
        }

        juce::String show (const Status& status, float glaze = 50.0f, Style style = Style::automatic)
        {
            return phase2::textFor (phase2::classify (status, glaze, style));
        }

        phase2::Tone toneOf (const Status& status, float glaze = 50.0f, Style style = Style::automatic)
        {
            return phase2::toneFor (phase2::classify (status, glaze, style).kind);
        }

        Kind kindOf (const Status& status, float glaze = 50.0f, Style style = Style::automatic)
        {
            return phase2::classify (status, glaze, style).kind;
        }

        juce::String showEnglish (const Status& status, float glaze = 50.0f, Style style = Style::automatic)
        {
            return phase2::textFor (phase2::classify (status, glaze, style), Language::english);
        }
    }

    class GlazeStatusTextTests  : public juce::UnitTest
    {
    public:
        GlazeStatusTextTests() : juce::UnitTest ("GlazeStatusText", "Celadon") {}

        void runTest() override
        {
            beginTest ("the seven phase-2 lines, plus GLAZE 0, show the sketch text and colour");
            {
                expectEquals (show (following(), 0.0f), utf8 ("GLAZE 0 · 只用手动设置"));
                expect (toneOf (following(), 0.0f) == phase2::Tone::dim);
                expectEquals (showEnglish (following(), 0.0f), utf8 ("GLAZE 0 · manual settings only"));

                auto manual = following();
                manual.limitedByManual = true;
                expectEquals (show (manual), utf8 ("受手动设置限制"));
                expect (toneOf (manual) == phase2::Tone::amber);

                auto hinted = following();
                hinted.houseActive = true;
                hinted.housePhaseHint = true;
                hinted.housePhaseMonoLossDb = -1.62f;
                expectEquals (show (hinted), utf8 ("俱乐部单声道会丢低频（约 1.6 dB），请回混音修"));
                expect (toneOf (hinted) == phase2::Tone::amber);

                auto cold = following();
                cold.confidence = 0.2f;
                cold.coldExport = true;
                expectEquals (show (cold), utf8 ("导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出"));
                expect (toneOf (cold) == phase2::Tone::amber);

                auto guard = following();
                guard.guardEngaged = true;
                guard.guardCause = phase2::GuardCause::budget;
                guard.protectionDb = -1.5f;
                expectEquals (show (guard), utf8 ("受限 · 预算 · 已退 1.5 dB"));
                expect (toneOf (guard) == phase2::Tone::amber);

                auto starting = following();
                starting.confidence = 0.2f;
                expectEquals (show (starting), utf8 ("起步 · 还没听到主段落（偏保守）"));
                expect (toneOf (starting) == phase2::Tone::accent);

                auto holding = following();
                holding.currentMainness = 0.2f;
                expectEquals (show (holding), utf8 ("保持 · 本段较轻"));
                expect (toneOf (holding) == phase2::Tone::dim);

                expectEquals (show (following()), utf8 ("跟随 · 自动→温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expect (toneOf (following()) == phase2::Tone::dim);

                auto house = following();
                house.houseActive = true;
                expectEquals (show (house), utf8 ("House · 跟随 · 自动→温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
            }

            beginTest ("priority is manual, house hint, cold export, guard, starting, holding, following");
            {
                Status everything = following();
                everything.limitedByManual = true;
                everything.houseActive = true;
                everything.housePhaseHint = true;
                everything.coldExport = true;
                everything.guardEngaged = true;
                everything.guardCause = phase2::GuardCause::contrast;
                everything.confidence = 0.1f;
                everything.currentMainness = 0.1f;
                expect (kindOf (everything) == Kind::manualLimit);

                everything.limitedByManual = false;
                expect (kindOf (everything) == Kind::housePhaseHint);

                everything.housePhaseHint = false;
                expect (kindOf (everything) == Kind::coldExport);

                everything.coldExport = false;
                expect (kindOf (everything) == Kind::guardRelief);

                everything.guardEngaged = false;
                expect (kindOf (everything) == Kind::starting);

                everything.confidence = 0.8f;
                expect (kindOf (everything) == Kind::holding);

                everything.currentMainness = 0.9f;
                expect (kindOf (everything) == Kind::following);

                everything.houseActive = false;
                everything.housePhaseHint = true;
                expect (kindOf (everything) == Kind::following, "houseActive false: the hint is ignored");
            }

            beginTest ("GLAZE 0 replaces every amber and plum line; NaN and negative GLAZE count as 0");
            {
                Status noisy = following();
                noisy.limitedByManual = true;
                noisy.houseActive = true;
                noisy.housePhaseHint = true;
                noisy.coldExport = true;
                noisy.guardEngaged = true;
                noisy.confidence = 0.0f;
                noisy.currentMainness = 0.0f;
                expectEquals (show (noisy, 0.0f), utf8 ("GLAZE 0 · 只用手动设置"));
                expectEquals (show (noisy, std::numeric_limits<float>::quiet_NaN()), utf8 ("GLAZE 0 · 只用手动设置"));
                expectEquals (show (noisy, -5.0f), utf8 ("GLAZE 0 · 只用手动设置"));
            }

            beginTest ("guard text names budget, delivery peak, contrast and PSR; a zero retreat omits the decibels");
            {
                auto guard = following();
                guard.guardEngaged = true;
                guard.protectionDb = -1.5f;

                guard.guardCause = phase2::GuardCause::deliveryPeak;
                expectEquals (show (guard), utf8 ("受限 · 交付峰值 · 已退 1.5 dB"));
                guard.guardCause = phase2::GuardCause::contrast;
                expectEquals (show (guard), utf8 ("受限 · 保对比 · 已退 1.5 dB"));
                guard.guardCause = phase2::GuardCause::psr;
                expectEquals (show (guard), utf8 ("受限 · PSR · 已退 1.5 dB"));
                expectEquals (showEnglish (guard), utf8 ("Limited · PSR · backed off 1.5 dB"));

                guard.protectionDb = 0.0f;
                expectEquals (show (guard), utf8 ("受限 · PSR"));
                guard.protectionDb = std::numeric_limits<float>::quiet_NaN();
                expectEquals (show (guard), utf8 ("受限 · PSR"));
            }

            beginTest ("following line: style, House prefix, U+2212 target, unknown target omitted");
            {
                expectEquals (show (following(), 50.0f, Style::warm), utf8 ("跟随 · 温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (show (following(), 50.0f, Style::punchy), utf8 ("跟随 · 冲击 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (show (following(), 50.0f, Style::clean), utf8 ("跟随 · 通透 · 目标 \xe2\x88\x92" "9.6 LUFS"));

                auto leaning = following();
                leaning.resolvedStyle = Style::clean;
                expectEquals (show (leaning, 0.0f), utf8 ("GLAZE 0 · 只用手动设置"));
                expectEquals (show (leaning), utf8 ("跟随 · 自动→通透 · 目标 \xe2\x88\x92" "9.6 LUFS"));

                auto noLean = following();
                noLean.resolvedStyle = Style::automatic;
                expectEquals (show (noLean), utf8 ("跟随 · 自动 · 目标 \xe2\x88\x92" "9.6 LUFS"));

                auto target = following();
                target.effectiveTargetLufs = -9.64f;
                expectEquals (show (target), utf8 ("跟随 · 自动→温润 · 目标 \xe2\x88\x92" "9.6 LUFS"));
                target.effectiveTargetLufs = -9.66f;
                expectEquals (show (target), utf8 ("跟随 · 自动→温润 · 目标 \xe2\x88\x92" "9.7 LUFS"));
                target.effectiveTargetLufs = std::numeric_limits<float>::quiet_NaN();
                expectEquals (show (target), utf8 ("跟随 · 自动→温润"));
            }

            beginTest ("views compare equal only when the shown line cannot differ");
            {
                auto a = following();
                auto b = following();
                b.effectiveTargetLufs = -9.61f;
                expect (phase2::classify (a, 50.0f, Style::automatic) == phase2::classify (b, 80.0f, Style::automatic));
                b.effectiveTargetLufs = -9.7f;
                expect (phase2::classify (a, 50.0f, Style::automatic) != phase2::classify (b, 50.0f, Style::automatic));
                expect (phase2::classify (following(), 50.0f, Style::automatic) != phase2::classify (following(), 50.0f, Style::warm));

                auto hinted = following();
                hinted.houseActive = true;
                hinted.housePhaseHint = true;
                hinted.housePhaseMonoLossDb = -1.62f;
                auto hintedSame = hinted;
                hintedSame.housePhaseMonoLossDb = -1.58f;
                expect (phase2::classify (hinted, 40.0f, Style::clean) == phase2::classify (hintedSame, 90.0f, Style::warm));
                hintedSame.housePhaseMonoLossDb = -1.71f;
                expect (phase2::classify (hinted, 50.0f, Style::automatic) != phase2::classify (hintedSame, 50.0f, Style::automatic));
            }

            beginTest ("every phase-2 combination yields a non-empty line in both languages");
            {
                const phase2::GuardCause causes[] = { phase2::GuardCause::none, phase2::GuardCause::budget,
                                                      phase2::GuardCause::deliveryPeak, phase2::GuardCause::contrast,
                                                      phase2::GuardCause::psr };

                for (int flags = 0; flags < 32; ++flags)
                    for (auto cause : causes)
                        for (float glaze : { 0.0f, 50.0f })
                        {
                            Status status = following();
                            status.limitedByManual = (flags & 1) != 0;
                            status.houseActive = (flags & 2) != 0;
                            status.housePhaseHint = (flags & 4) != 0;
                            status.coldExport = (flags & 8) != 0;
                            status.guardEngaged = (flags & 16) != 0;
                            status.guardCause = cause;
                            status.confidence = (flags & 8) != 0 ? 0.2f : 0.9f;
                            status.currentMainness = (flags & 1) != 0 ? 0.2f : 0.9f;
                            status.protectionDb = -1.5f;
                            status.housePhaseMonoLossDb = -1.62f;

                            for (const auto language : { Language::chinese, Language::english })
                            {
                                const auto text = phase2::textFor (phase2::classify (status, glaze, Style::automatic), language);
                                expect (text.isNotEmpty() && ! text.containsChar ((juce::juce_wchar) 0xFFFD), text);
                                expect (phase2::segmentsFor (phase2::classify (status, glaze, Style::automatic), language).size() >= 1);
                            }
                        }
            }

            beginTest ("the legacy listen notice is session-only copy, not one of the seven lines");
            {
                expect (juce::String::fromUTF8 (phase2::legacySessionNoticeZh) != show (following()));
                expect (juce::String::fromUTF8 (phase2::legacySessionNoticeZh) != show (following(), 0.0f));
                expectEquals (juce::String (phase2::legacySessionNoticeEn),
                              juce::String ("The old listen result was discarded; the sound will change."));
            }

            beginTest ("production copy that phase 2 keeps: manual limit, house phase hint, listening to the original");
            {
                glaze::Status manual;
                manual.limitedByManual = true;
                manual.phase = glaze::Phase::locked;
                manual.calibrationValid = true;
                manual.profileReady = true;
                manual.planReady = true;
                expectEquals (gs::textFor (gs::classify (manual, 50.0f, Style::automatic, false)), utf8 ("受手动设置限制"));

                glaze::Status hinted = manual;
                hinted.limitedByManual = false;
                hinted.houseActive = true;
                hinted.housePhaseHint = true;
                hinted.housePhaseMonoLossDb = -1.62f;
                hinted.resolvedStyle = Style::warm;
                hinted.effectiveTargetLufs = -9.6f;
                const auto productionHint = gs::textFor (gs::classify (hinted, 50.0f, Style::automatic, false));

                Status phase2Hint = following();
                phase2Hint.houseActive = true;
                phase2Hint.housePhaseHint = true;
                phase2Hint.housePhaseMonoLossDb = -1.62f;
                expectEquals (show (phase2Hint), productionHint);
                expectEquals (showEnglish (phase2Hint),
                              gs::textFor (gs::classify (hinted, 50.0f, Style::automatic, false), Language::english));

                expectEquals (ui::tr (ui::Text::listeningOriginal, Language::chinese), utf8 ("正在听原声"));
                expectEquals (ui::tr (ui::Text::listeningOriginal, Language::english), juce::String ("Listening to original"));

                expect (gs::styleFromParameterValue (0.0f) == Style::automatic);
                expect (gs::styleFromParameterValue (3.0f) == Style::clean);
                expect (gs::styleFromParameterValue (std::numeric_limits<float>::quiet_NaN()) == Style::automatic);
            }
        }
    };

    static GlazeStatusTextTests glazeStatusTextTests;
}
