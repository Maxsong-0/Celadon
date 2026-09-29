/*
  ==============================================================================

    LocalisationTests.cpp
    界面语言（v0.1.8.0）的自动测试：Localisation 表每个键都有一行、顺序与枚举一致、两种语言都非空且是合法 UTF-8、
    英文里没有混进中文；占位符替换；语言代号往返。GLAZE 状态行每一种情形都有英文、英文不混中文、
    按「 · 」拆出的段拼回去就是整行文字。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/GlazeStatusText.h"
#include "../Source/ui/Localisation.h"

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

            beginTest ("every GLAZE status has an English text; segments rebuild the text");
            {
                using namespace ui::glazestatus;

                std::vector<View> views;

                for (int kind = 0; kind <= (int) Kind::profileMismatch; ++kind)
                {
                    View view;
                    view.kind = (Kind) kind;
                    views.push_back (view);

                    view.glazeActive = true;
                    view.listenedSeconds = 9;
                    view.requiredSeconds = 15;
                    view.hasTarget = true;
                    view.targetTenthsLufs = -96;
                    view.yieldedTenthsDb = 15;
                    view.resolvedStyle = glaze::Style::warm;
                    view.pausedByTransport = kind % 2 == 0;
                    views.push_back (view);

                    view.olderPlan = true;
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

                View locked;
                locked.kind = Kind::locked;
                locked.glazeActive = true;
                locked.resolvedStyle = glaze::Style::warm;
                locked.hasTarget = true;
                locked.targetTenthsLufs = -96;
                expectEquals (textFor (locked, Language::english), utf8 ("Locked \xc2\xb7 Auto\xe2\x86\x92Warm \xc2\xb7 Target \xe2\x88\x92" "9.6 LUFS"));
                expectEquals (segmentsFor (locked, Language::chinese).size(), 3);
                expectEquals (styleName (glaze::Style::punchy, Language::english), juce::String ("Punchy"));
                expectEquals (styleName (glaze::Style::punchy), utf8 ("冲击"));
            }
        }
    };

    static LocalisationTests localisationTests;
}
