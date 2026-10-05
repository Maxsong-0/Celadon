/*
  ==============================================================================

    GlazeStatusTextTests.cpp
    T-048 阶段 2 状态行：七条里取第一条，GLAZE 0 只写手动，导出冷启动与守护原因，House 前缀只加在「跟随」上。

    被测代码只依赖 juce_core。中文断言用 TestHelpers.h 的 utf8StartsWith / utf8Contains。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/GlazeStatusText.h"

#include <cmath>
#include <limits>

namespace celadon::test
{
    namespace
    {
        namespace gs = ui::glazestatus;
        using glaze::Status;
        using glaze::Style;
        using gs::GuardCause;
        using gs::Kind;
        using gs::Reading;

        juce::String show (const Reading& reading, float glaze = 50.0f, Style style = Style::automatic)
        {
            return gs::textFor (gs::classify (reading, glaze, style));
        }

        Kind kindOf (const Reading& reading, float glaze = 50.0f, Style style = Style::automatic)
        {
            return gs::classify (reading, glaze, style).kind;
        }

        Reading ready()
        {
            Reading reading;
            reading.confidence = 1.0f;
            reading.currentMainness = 1.0f;
            reading.effectiveTargetLufs = -9.6f;
            reading.resolvedStyle = Style::warm;
            return reading;
        }
    }

    class GlazeStatusTextTests  : public juce::UnitTest
    {
    public:
        GlazeStatusTextTests() : juce::UnitTest ("GlazeStatusText", "Celadon") {}

        void runTest() override
        {
            beginTest ("GLAZE 0 is only the manual line, even when every amber flag is set");
            {
                auto reading = ready();
                reading.limitedByManual = true;
                reading.houseActive = true;
                reading.housePhaseHint = true;
                reading.coldExport = true;
                reading.guardEngaged = true;
                reading.confidence = 0.0f;
                reading.currentMainness = 0.0f;

                expect (kindOf (reading, 0.0f) == Kind::manualOnly);
                expect (kindOf (reading, std::numeric_limits<float>::quiet_NaN()) == Kind::manualOnly);
                expectEquals (show (reading, 0.0f), utf8 ("GLAZE 0 · 只用手动设置"));
                expectEquals (gs::textFor (gs::classify (reading, 0.0f, Style::automatic), Language::english),
                              utf8 ("GLAZE 0 \xc2\xb7 manual settings only"));
                expect (gs::toneFor (Kind::manualOnly) == gs::Tone::dim);
            }

            beginTest ("the seven lines: first match wins, amber then plum then grey-green");
            {
                auto reading = ready();
                reading.limitedByManual = true;
                reading.houseActive = true;
                reading.housePhaseHint = true;
                reading.housePhaseMonoLossDb = -1.6f;
                reading.coldExport = true;
                reading.guardEngaged = true;
                reading.guardCause = GuardCause::budget;
                reading.protectionDb = -1.5f;
                reading.confidence = 0.2f;
                reading.currentMainness = 0.2f;

                expect (kindOf (reading) == Kind::limitedByManual, "1 manual");
                expectEquals (show (reading), utf8 ("受手动设置限制"));
                expect (gs::toneFor (Kind::limitedByManual) == gs::Tone::amber);

                reading.limitedByManual = false;
                expect (kindOf (reading) == Kind::housePhaseHint, "2 phase hint");
                expectEquals (show (reading), utf8 ("俱乐部单声道会丢低频（约 1.6 dB），请回混音修"));

                reading.housePhaseHint = false;
                expect (kindOf (reading) == Kind::coldExport, "3 cold export");
                expectEquals (show (reading), utf8 ("导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出"));
                expectEquals (gs::segmentsFor (gs::classify (reading, 50.0f, Style::automatic), Language::chinese).size(), 1);

                reading.coldExport = false;
                expect (kindOf (reading) == Kind::guardLimited, "4 guard");
                expectEquals (show (reading), utf8 ("受限 · 预算 · 已退 1.5 dB"));
                expect (gs::toneFor (Kind::guardLimited) == gs::Tone::amber);

                reading.guardEngaged = false;
                expect (kindOf (reading) == Kind::starting, "5 low confidence");
                expectEquals (show (reading), utf8 ("起步 · 还没听到主段落（偏保守）"));
                expect (gs::toneFor (Kind::starting) == gs::Tone::accent);

                reading.confidence = 0.5f;
                expect (kindOf (reading) == Kind::holding, "6 light section; c == 0.5 is no longer starting");
                expectEquals (show (reading), utf8 ("保持 · 本段较轻"));
                expect (gs::toneFor (Kind::holding) == gs::Tone::dim);

                reading.currentMainness = 0.5f;
                reading.houseActive = false;
                expect (kindOf (reading) == Kind::following, "7 m == 0.5 is not held");
                expectEquals (show (reading), utf8 ("跟随 · 自动→温润 · 目标 −9.6 LUFS"));
                expect (gs::toneFor (Kind::following) == gs::Tone::dim);
            }

            beginTest ("guard cause names, a zero relief omits the dB, House prefixes only Following");
            {
                auto reading = ready();
                reading.guardEngaged = true;
                reading.protectionDb = -1.54f;

                reading.guardCause = GuardCause::deliveryPeak;
                expectEquals (show (reading), utf8 ("受限 · 交付峰值 · 已退 1.5 dB"));
                reading.guardCause = GuardCause::contrast;
                expectEquals (show (reading), utf8 ("受限 · 保对比 · 已退 1.5 dB"));
                reading.guardCause = GuardCause::psr;
                expectEquals (gs::textFor (gs::classify (reading, 50.0f, Style::automatic), Language::english),
                              utf8 ("Limited \xc2\xb7 PSR \xc2\xb7 backed off 1.5 dB"));

                reading.guardCause = GuardCause::none;
                reading.protectionDb = 0.0f;
                expectEquals (show (reading), utf8 ("受限"));

                reading.guardEngaged = false;
                reading.confidence = 0.2f;
                reading.houseActive = true;
                expect (utf8StartsWith (show (reading), "起步"), "starting has no House prefix");

                reading.confidence = 1.0f;
                reading.currentMainness = 0.2f;
                expectEquals (show (reading), utf8 ("保持 · 本段较轻"));

                reading.currentMainness = 1.0f;
                const auto house = gs::classify (reading, 50.0f, Style::automatic);
                expect (house.housePrefix && house.styleSegment == 2);
                expectEquals (gs::textFor (house), utf8 ("House · 跟随 · 自动→温润 · 目标 −9.6 LUFS"));
                expectEquals (gs::textFor (house, Language::english),
                              utf8 ("House \xc2\xb7 Following \xc2\xb7 Auto\xe2\x86\x92Warm \xc2\xb7 Target \xe2\x88\x92" "9.6 LUFS"));

                const auto punchy = gs::classify (reading, 50.0f, Style::punchy);
                expectEquals (gs::textFor (punchy), utf8 ("House · 跟随 · 冲击 · 目标 −9.6 LUFS"));
                expect (punchy.styleSegment == 2 && gs::toneFor (punchy.kind) == gs::Tone::dim);

                reading.houseActive = false;
                reading.effectiveTargetLufs = std::numeric_limits<float>::quiet_NaN();
                expectEquals (show (reading, 50.0f, Style::warm), utf8 ("跟随 · 温润"));
            }

            beginTest ("non-finite confidence starts; non-finite mainness does not hold; phase hint needs House");
            {
                auto reading = ready();
                reading.confidence = std::numeric_limits<float>::quiet_NaN();
                expect (kindOf (reading) == Kind::starting);

                reading.confidence = 1.0f;
                reading.currentMainness = std::numeric_limits<float>::quiet_NaN();
                expect (kindOf (reading) == Kind::following);

                reading.housePhaseHint = true;
                reading.houseActive = false;
                expect (kindOf (reading) == Kind::following, "hint without House is ignored");

                reading.houseActive = true;
                expect (kindOf (reading) == Kind::housePhaseHint);
            }

            beginTest ("readingFromStatus copies the phase-2 fields; planLimited does not stand in for guardEngaged");
            {
                Status status;
                status.limitedByManual = true;
                status.protectionDb = -1.5f;
                status.effectiveTargetLufs = -9.6f;
                status.resolvedStyle = Style::warm;
                status.houseActive = true;
                status.housePhaseHint = true;
                status.housePhaseMonoLossDb = -1.6f;
                status.confidence = 0.8f;
                status.currentMainness = 0.9f;
                status.guardCause = glaze::GuardCause::budget;
                status.guardEngaged = true;
                status.planLimited = false;

                const auto reading = gs::readingFromStatus (status);
                expect (reading.limitedByManual && reading.houseActive && reading.housePhaseHint);
                expect (std::abs (reading.protectionDb - (-1.5f)) < 1.0e-5f);
                expect (std::abs (reading.effectiveTargetLufs - (-9.6f)) < 1.0e-5f);
                expect (std::abs (reading.housePhaseMonoLossDb - (-1.6f)) < 1.0e-5f);
                expect (std::abs (reading.confidence - 0.8f) < 1.0e-5f);
                expect (std::abs (reading.currentMainness - 0.9f) < 1.0e-5f);
                expect (reading.resolvedStyle == Style::warm);
                expect (reading.guardEngaged && reading.guardCause == GuardCause::budget);
                expect (kindOf (reading) == Kind::limitedByManual, "manual still outranks the guard line");

                auto guarded = reading;
                guarded.limitedByManual = false;
                guarded.housePhaseHint = false;
                expectEquals (show (guarded), utf8 ("受限 · 预算 · 已退 1.5 dB"));

                Status legacyFlag;
                legacyFlag.planLimited = true;
                legacyFlag.protectionDb = -1.5f;
                legacyFlag.confidence = 1.0f;
                legacyFlag.currentMainness = 1.0f;
                legacyFlag.effectiveTargetLufs = -9.6f;
                legacyFlag.resolvedStyle = Style::warm;
                const auto ignored = gs::readingFromStatus (legacyFlag);
                expect (! ignored.guardEngaged, "Status::guardEngaged is the flag the line reads");
                expect (kindOf (ignored) == Kind::following);
            }

            beginTest ("a view compares equal only when the painted quantities match");
            {
                const auto a = gs::classify (ready(), 50.0f, Style::automatic);
                auto nudged = ready();
                nudged.effectiveTargetLufs = -9.64f;
                const auto b = gs::classify (nudged, 50.0f, Style::automatic);
                expect (a == b, "−9.64 and −9.6 paint the same tenth");

                nudged.effectiveTargetLufs = -9.2f;
                expect (a != gs::classify (nudged, 50.0f, Style::automatic));
                expectEquals (gs::segmentsFor (a, Language::chinese).joinIntoString (utf8 (" · ")), gs::textFor (a));
            }

            beginTest ("style names and the parameter index");
            {
                expectEquals (gs::styleName (Style::punchy), utf8 ("冲击"));
                expectEquals (gs::styleName (Style::punchy, Language::english), juce::String ("Punchy"));
                expect (gs::styleFromParameterValue (2.4f) == Style::punchy);
                expect (gs::styleFromParameterValue (std::numeric_limits<float>::quiet_NaN()) == Style::automatic);
                expect (gs::styleFromParameterValue (99.0f) == Style::clean);
            }
        }
    };

    static GlazeStatusTextTests glazeStatusTextTests;
}
