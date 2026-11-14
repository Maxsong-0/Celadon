/*
  ==============================================================================

    GlazeStatusTextTests.cpp
    T-048 阶段 2 状态行：六条里取第一条，GLAZE 0 只写手动，导出冷启动与守护原因（T-108 起只剩预算与限幅两种，House 前缀与反相提示删掉）。

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

            beginTest ("the six lines: first match wins, amber then plum then grey-green");
            {
                auto reading = ready();
                reading.limitedByManual = true;
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
                expect (kindOf (reading) == Kind::coldExport, "2 cold export");
                expectEquals (show (reading), utf8 ("导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出"));
                expectEquals (gs::segmentsFor (gs::classify (reading, 50.0f, Style::automatic), Language::chinese).size(), 1);

                reading.coldExport = false;
                expect (kindOf (reading) == Kind::guardLimited, "3 guard");
                expectEquals (show (reading), utf8 ("受限 · 预算 · 已退 1.5 dB"));
                expect (gs::toneFor (Kind::guardLimited) == gs::Tone::amber);

                reading.guardEngaged = false;
                expect (kindOf (reading) == Kind::starting, "4 low confidence");
                expectEquals (show (reading), utf8 ("起步 · 还没听到主段落（偏保守）"));
                expect (gs::toneFor (Kind::starting) == gs::Tone::accent);

                reading.confidence = 0.5f;
                expect (kindOf (reading) == Kind::holding, "5 light section; c == 0.5 is no longer starting");
                expectEquals (show (reading), utf8 ("保持 · 本段较轻"));
                expect (gs::toneFor (Kind::holding) == gs::Tone::dim);

                reading.currentMainness = 0.5f;
                expect (kindOf (reading) == Kind::following, "6 m == 0.5 is not held");
                expectEquals (show (reading), utf8 ("跟随 · 自动→温润 · 目标 −9.6 LUFS"));
                expect (gs::toneFor (Kind::following) == gs::Tone::dim);
            }

            beginTest ("guard cause names, a zero relief omits the dB, Following carries the style segment");
            {
                auto reading = ready();
                reading.guardEngaged = true;
                reading.protectionDb = -1.54f;

                reading.guardCause = GuardCause::budget;
                expectEquals (show (reading), utf8 ("受限 · 预算 · 已退 1.5 dB"));
                expectEquals (gs::textFor (gs::classify (reading, 50.0f, Style::automatic), Language::english),
                              utf8 ("Limited \xc2\xb7 Budget \xc2\xb7 backed off 1.5 dB"));
                reading.guardCause = GuardCause::limiterPeak;
                expectEquals (show (reading), utf8 ("受限 · 限幅 · 已退 1.5 dB"));
                expectEquals (gs::textFor (gs::classify (reading, 50.0f, Style::automatic), Language::english),
                              utf8 ("Limited \xc2\xb7 Limiter \xc2\xb7 backed off 1.5 dB"));

                reading.guardCause = GuardCause::none;
                reading.protectionDb = 0.0f;
                expectEquals (show (reading), utf8 ("受限"));

                reading.guardEngaged = false;
                reading.confidence = 0.2f;
                expect (utf8StartsWith (show (reading), "起步"));

                reading.confidence = 1.0f;
                reading.currentMainness = 0.2f;
                expectEquals (show (reading), utf8 ("保持 · 本段较轻"));

                reading.currentMainness = 1.0f;
                const auto following = gs::classify (reading, 50.0f, Style::automatic);
                expect (following.styleSegment == 1);
                expectEquals (gs::textFor (following), utf8 ("跟随 · 自动→温润 · 目标 −9.6 LUFS"));
                expectEquals (gs::textFor (following, Language::english),
                              utf8 ("Following \xc2\xb7 Auto\xe2\x86\x92Warm \xc2\xb7 Target \xe2\x88\x92" "9.6 LUFS"));

                const auto punchy = gs::classify (reading, 50.0f, Style::punchy);
                expectEquals (gs::textFor (punchy), utf8 ("跟随 · 冲击 · 目标 −9.6 LUFS"));
                expect (punchy.styleSegment == 1 && gs::toneFor (punchy.kind) == gs::Tone::dim);

                reading.effectiveTargetLufs = std::numeric_limits<float>::quiet_NaN();
                expectEquals (show (reading, 50.0f, Style::warm), utf8 ("跟随 · 温润"));
            }

            beginTest ("non-finite confidence starts; non-finite mainness does not hold");
            {
                auto reading = ready();
                reading.confidence = std::numeric_limits<float>::quiet_NaN();
                expect (kindOf (reading) == Kind::starting);

                reading.confidence = 1.0f;
                reading.currentMainness = std::numeric_limits<float>::quiet_NaN();
                expect (kindOf (reading) == Kind::following);
            }

            beginTest ("readingFromStatus copies the phase-2 fields; planLimited does not stand in for guardEngaged");
            {
                Status status;
                status.limitedByManual = true;
                status.protectionDb = -1.5f;
                status.effectiveTargetLufs = -9.6f;
                status.resolvedStyle = Style::warm;
                status.confidence = 0.8f;
                status.currentMainness = 0.9f;
                status.guardCause = glaze::GuardCause::budget;
                status.guardEngaged = true;
                status.planLimited = false;

                const auto reading = gs::readingFromStatus (status);
                expect (reading.limitedByManual);
                expect (std::abs (reading.protectionDb - (-1.5f)) < 1.0e-5f);
                expect (std::abs (reading.effectiveTargetLufs - (-9.6f)) < 1.0e-5f);
                expect (std::abs (reading.confidence - 0.8f) < 1.0e-5f);
                expect (std::abs (reading.currentMainness - 0.9f) < 1.0e-5f);
                expect (reading.resolvedStyle == Style::warm);
                expect (reading.guardEngaged && reading.guardCause == GuardCause::budget);
                expect (kindOf (reading) == Kind::limitedByManual, "manual still outranks the guard line");

                auto guarded = reading;
                guarded.limitedByManual = false;
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

            // T-111 / 3-05、4-27：顶部胶囊最窄约 495 px，英文那句原来 113 个字符、截掉的正是要用户做的那一步。
            beginTest ("the English cold-export line fits the pill and keeps the action");
            {
                gs::Reading reading;
                reading.coldExport = true;
                const auto english = gs::textFor (gs::classify (reading, 50.0f, Style::automatic), Language::english);
                expect (english.length() <= 60, english + " (" + juce::String (english.length()) + " characters)");
                expect (english.contains ("play it once, then export"), english);
            }

            // T-111 / 4-06 橙色规则：同一时刻只有一件事拿到橙色，优先级与顶部胶囊一致。
            beginTest ("attentionOwner picks one issue: non-finite > true peak > GLAZE amber > CLAY guard > old-project notice");
            {
                using gs::Attention;
                gs::AttentionInputs inputs;
                expect (gs::attentionOwner (inputs) == Attention::none);

                inputs.legacyNotice = true;
                expect (gs::attentionOwner (inputs) == Attention::legacyNotice);
                inputs.clayGuard = true;
                expect (gs::attentionOwner (inputs) == Attention::clayGuard);
                inputs.glazeAmber = true;
                expect (gs::attentionOwner (inputs) == Attention::glaze);
                inputs.truePeakOver = true;
                expect (gs::attentionOwner (inputs) == Attention::truePeak);
                inputs.nonFinite = true;
                expect (gs::attentionOwner (inputs) == Attention::nonFinite);

                // 只有低一级的那件事成立时，它照样拿得到橙色（不因为别的事「曾经」成立而被压住）。
                gs::AttentionInputs onlyClay;
                onlyClay.clayGuard = true;
                expect (gs::attentionOwner (onlyClay) == Attention::clayGuard);
            }
        }
    };

    static GlazeStatusTextTests glazeStatusTextTests;
}
