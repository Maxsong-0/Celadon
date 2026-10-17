/*
  ==============================================================================

    LocalisationTests.cpp
    界面语言（v0.1.8.0）的自动测试：Localisation 表每个键都有一行、顺序与枚举一致、两种语言都非空且是合法 UTF-8、
    英文里没有混进中文；占位符替换；语言代号往返。GLAZE 状态行每一种情形都有英文、英文不混中文、
    按「 · 」拆出的段拼回去就是整行文字。T-038：曲风 / 交付两个选择的文字与参数选项一一对应、House 暂定期间的提示。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/GlazeStatusText.h"
#include "../Source/ui/Localisation.h"

#include <cmath>
#include <cstring>

namespace celadon::test
{
    namespace
    {
        bool containsCjk (const juce::String& text)
        {
            for (auto p = text.getCharPointer(); ! p.isEmpty(); ++p)
            {
                const auto c = (juce::uint32) *p;

                if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF))
                    return true;
            }

            return false;
        }

        bool validUtf8 (const char* text)
        {
            return text != nullptr && text[0] != '\0' && juce::CharPointer_UTF8::isValidString (text, (int) std::strlen (text));
        }
    }

    class LocalisationTests  : public juce::UnitTest
    {
    public:
        LocalisationTests() : juce::UnitTest ("Localisation", "Celadon") {}

        void runTest() override
        {
            using ui::Text;

            beginTest ("every text key has one row in enum order, both languages present");
            {
                int count = 0;
                const auto* entries = ui::textEntries (count);
                expectEquals (count, (int) Text::numTexts);

                for (int i = 0; i < count; ++i)
                {
                    const auto& entry = entries[i];
                    const auto label = "key " + juce::String (i);

                    expectEquals ((int) entry.key, i, "table order must follow the enum: " + label);
                    expect (validUtf8 (entry.chinese), "Chinese text missing or invalid: " + label);
                    expect (validUtf8 (entry.english), "English text missing or invalid: " + label);

                    const auto english = ui::tr (entry.key, Language::english);
                    const auto chinese = ui::tr (entry.key, Language::chinese);
                    expect (english.isNotEmpty() && chinese.isNotEmpty(), label);
                    expect (! containsCjk (english), "English text contains CJK: " + label + " " + english);
                    expect (! english.containsChar ((juce::juce_wchar) 0xFFFD) && ! chinese.containsChar ((juce::juce_wchar) 0xFFFD), label);
                }
            }

            beginTest ("key hints name this platform's keys (Mac: Command / Option; Windows and Linux: Ctrl / Alt)");
            {
                // 精调看 isCommandDown()（Mac 的 ⌘、别处的 Ctrl），输入看 Alt（Mac 键盘上印 Option）：提示只写本平台的键。
                const juce::String command = juce::String::charToString ((juce::juce_wchar) 0x2318);

                for (const auto language : { Language::chinese, Language::english })
                {
                    const auto fine = ui::tr (Text::hintDrag, language) + " " + ui::tr (Text::hintDial, language) + " "
                                    + ui::tr (Text::helpFine, language);
                    const auto type = ui::tr (Text::hintClick, language) + " " + ui::tr (Text::helpType, language);
                    const auto label = juce::String (language == Language::english ? "en" : "zh") + ": " + fine + " | " + type;

                   #if JUCE_MAC
                    expect (fine.contains (command) && ! fine.contains ("Ctrl"), label);
                    expect (type.contains ("Option") && ! type.contains ("Alt"), label);
                   #else
                    expect (fine.contains ("Ctrl") && ! fine.contains (command), label);
                    expect (type.contains ("Alt") && ! type.contains ("Option"), label);
                   #endif
                }
            }

            beginTest ("placeholders are replaced in order");
            {
                expectEquals (ui::trFormat (Text::screenSize, Language::english, { "2560", "1440" }),
                              utf8 ("2560 \xc3\x97 1440 screen"));
                expectEquals (ui::trFormat (Text::warnTruePeak, Language::chinese, { "+0.3", utf8 ("\xe2\x88\x92" "1.0") }),
                              utf8 ("真峰值 +0.3 dBTP · 超出上限 \xe2\x88\x92" "1.0"));
            }

            beginTest ("language codes round-trip; unknown codes fall back to Chinese");
            {
                for (const auto language : { Language::chinese, Language::english })
                    expect (languageFromCode (languageCode (language)) == language);

                expect (languageFromCode ("EN") == Language::english);
                expect (languageFromCode ("fr") == Language::chinese);
                expect (languageFromCode ("") == Language::chinese);
            }

            beginTest ("typed numbers from Chinese IMEs are normalised to ASCII (full-width digits and commas, ideographic full stop, minus signs)");
            {
                using ui::normaliseTypedNumber;
                expectEquals (normaliseTypedNumber (utf8 ("3\xe3\x80\x82" "5")), juce::String ("3.5"));                       // 3。5
                expectEquals (normaliseTypedNumber (utf8 ("\xef\xbc\x91\xef\xbc\x92\xef\xbc\x8e\xef\xbc\x95")),         // １２．５
                              juce::String ("12.5"));
                expectEquals (normaliseTypedNumber (utf8 ("\xef\xbc\x8d" "3")), juce::String ("-3"));                          // －3
                expectEquals (normaliseTypedNumber (utf8 ("\xe2\x88\x92" "1.0 dB")), juce::String ("-1.0 dB"));               // −1.0 dB
                expectEquals (normaliseTypedNumber (utf8 ("\xef\xbc\x8b" "6")), juce::String ("+6"));                          // ＋6
                expectEquals (normaliseTypedNumber (utf8 ("1\xef\xbc\x8c" "000")), juce::String ("1,000"));                    // 1，000
                expectEquals (normaliseTypedNumber ("25 Hz"), juce::String ("25 Hz"), "ASCII passes through unchanged");
            }

            beginTest ("typed values: IME text, decimal commas, thousands commas and ambiguous input");
            {
                using ui::parseTypedNumber;

                const auto expectValue = [this] (const juce::String& text, double expected)
                {
                    const auto parsed = parseTypedNumber (text);
                    expect (parsed.has_value() && std::abs (*parsed - expected) < 1.0e-9,
                            "\"" + text + "\" -> " + (parsed.has_value() ? juce::String (*parsed) : juce::String ("nothing")) + ", expected " + juce::String (expected));
                };
                const auto expectNothing = [this] (const juce::String& text)
                {
                    const auto parsed = parseTypedNumber (text);
                    expect (! parsed.has_value(), "\"" + text + "\" should be rejected, got " + (parsed.has_value() ? juce::String (*parsed) : juce::String()));
                };

                expectValue ("60", 60.0);
                expectValue ("+6", 6.0);
                expectValue (".5", 0.5);
                expectValue ("-1.0 dB", -1.0);
                expectValue ("50%", 50.0);
                expectValue (utf8 ("3\xe3\x80\x82" "5"), 3.5);                                         // 3。5
                expectValue (utf8 ("\xef\xbc\x91\xef\xbc\x92\xef\xbc\x8e\xef\xbc\x95 dB"), 12.5);         // １２．５ dB
                expectValue (utf8 ("\xe2\x88\x92" "0.3 dB"), -0.3);                                     // −0.3 dB

                // 小数逗号（德、法、俄……）
                expectValue ("1,5", 1.5);
                expectValue ("-0,3", -0.3);
                expectValue ("0,125", 0.125);
                expectValue ("2,25 ms", 2.25);
                expectValue (utf8 ("\xe2\x88\x92" "1,5 dB"), -1.5);                                     // −1,5 dB

                // 千位逗号（英文）
                expectValue ("1,000 ms", 1000.0);
                expectValue (utf8 ("1\xef\xbc\x8c" "000"), 1000.0);                                     // 1，000
                expectValue ("12,345,678", 12345678.0);

                // 说不清的：不改参数
                expectNothing ("");
                expectNothing ("dB");
                expectNothing ("-");
                expectNothing ("3.5.1");
                expectNothing ("1.000,5");
                expectNothing ("1,000.5");
                expectNothing ("1,5,0");
                expectNothing ("3-5");
                expectNothing ("5-");
            }

            beginTest ("every GLAZE status has an English text; segments rebuild the text");
            {
                using namespace ui::glazestatus;

                std::vector<View> views;

                for (int kind = 0; kind <= (int) Kind::following; ++kind)
                {
                    View view;
                    view.kind = (Kind) kind;
                    views.push_back (view);

                    view.hasTarget = true;
                    view.targetTenthsLufs = -96;
                    view.yieldedTenthsDb = 15;
                    view.guardCause = GuardCause::budget;
                    view.resolvedStyle = glaze::Style::warm;
                    view.chosenStyle = glaze::Style::automatic;
                    view.housePrefix = true;
                    view.houseLossTenthsDb = 16;
                    view.styleSegment = 2;
                    views.push_back (view);

                    view.guardCause = GuardCause::psr;
                    view.housePrefix = false;
                    views.push_back (view);
                }

                const auto dot = utf8 (" \xc2\xb7 ");

                for (const auto& view : views)
                {
                    const auto label = "kind " + juce::String ((int) view.kind);
                    const auto english = textFor (view, Language::english);

                    expect (english.isNotEmpty(), label);
                    expect (! containsCjk (english), "English status contains CJK: " + english);
                    expectEquals (textFor (view, Language::chinese), textFor (view), label);

                    for (const auto language : { Language::chinese, Language::english })
                        expectEquals (segmentsFor (view, language).joinIntoString (dot), textFor (view, language), label);
                }

                View following;
                following.kind = Kind::following;
                following.resolvedStyle = glaze::Style::warm;
                following.hasTarget = true;
                following.targetTenthsLufs = -96;
                following.styleSegment = 1;
                expectEquals (textFor (following, Language::english), utf8 ("Following \xc2\xb7 Auto\xe2\x86\x92Warm \xc2\xb7 Target \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (segmentsFor (following, Language::chinese).size(), 3);
                expectEquals (styleName (glaze::Style::punchy, Language::english), juce::String ("Punchy"));
                expectEquals (styleName (glaze::Style::punchy), utf8 ("冲击"));
            }

            // T-038：曲风 / 交付两个选择的说明格与选项（顺序 = 参数 glaze_genre / glaze_delivery 的下标）与 House 暂定期间的提示。
            beginTest ("genre and delivery labels follow the parameter option order in both languages; the House tip reads as in the spec");
            {
                const Text genre[] = { Text::genreGeneral, Text::genreDeep, Text::genreTech, Text::genreMelodic };
                const Text delivery[] = { Text::deliveryStandard, Text::deliveryStreaming, Text::deliveryClub };
                expectEquals ((int) std::size (genre), glaze::numGenres, "one label per genre option");
                expectEquals ((int) std::size (delivery), glaze::numDeliveries, "one label per delivery option");

                expectEquals (ui::tr (Text::genreCaption, Language::chinese), utf8 ("曲风"));
                expectEquals (ui::tr (Text::deliveryCaption, Language::chinese), utf8 ("交付"));
                expectEquals (ui::tr (Text::genreCaption, Language::english), juce::String ("Genre"));
                expectEquals (ui::tr (Text::deliveryCaption, Language::english), juce::String ("Delivery"));
                expectEquals (ui::tr (Text::genreGeneral, Language::chinese), utf8 ("通用"));
                expectEquals (ui::tr (Text::genreMelodic, Language::english), juce::String ("Melodic"));
                expectEquals (ui::tr (Text::deliveryStreaming, Language::chinese), utf8 ("流媒体"));
                expectEquals (ui::tr (Text::deliveryClub, Language::english), juce::String ("Club"));

                for (const auto language : { Language::chinese, Language::english })
                {
                    juce::StringArray seen;

                    for (const auto key : genre)       seen.addIfNotAlreadyThere (ui::tr (key, language));
                    for (const auto key : delivery)    seen.addIfNotAlreadyThere (ui::tr (key, language));

                    expectEquals (seen.size(), (int) (std::size (genre) + std::size (delivery)), "labels must be distinct within a language");
                }

                expectEquals (ui::tr (Text::legacyDiscarded, Language::chinese), utf8 ("旧的聆听结果已丢弃，声音会变"));
                expectEquals (ui::tr (Text::legacyDiscarded, Language::english),
                              juce::String ("The old listening result was discarded, so the sound will change"));
            }
        }
    };

    static LocalisationTests localisationTests;
}
