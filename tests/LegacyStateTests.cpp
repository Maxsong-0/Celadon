/*
  ==============================================================================

    LegacyStateTests.cpp
    读旧工程时的状态树迁移（Source/LegacyState.h）。

    T-108（ADR-039 决定 6）：曲风 / 交付两个 PARAM 节点读档时摘掉，有一个不是默认值（下标 0 = 通用 / 标准）就提示一次。
    截图工具（celadon_ui_snapshot --only check）用真处理器 + 编辑器查整条路；它不在 CI 里，这里在 celadon_tests 里钉住判定本身：
    五种旧存档、取整边界、坏值、重复节点，节点确实摘掉、别的节点一个不动（T-108 独立复查 F7）。

  ==============================================================================
*/

#include <juce_core/juce_core.h>

#include "../Source/LegacyState.h"

namespace celadon::test
{
    namespace
    {
        // 一份 APVTS 根元素：两个现行参数（GLAZE 62、风格 1）+ 一个不相干的子元素，再按需加两个旧 PARAM 节点（nullptr = 不加）。
        std::unique_ptr<juce::XmlElement> makeState (const char* genre, const char* delivery)
        {
            auto xml = std::make_unique<juce::XmlElement> ("PARAMETERS");

            const auto addParam = [&xml] (const char* id, const char* value)
            {
                auto* node = xml->createNewChildElement ("PARAM");
                node->setAttribute ("id", id);

                if (value != nullptr)
                    node->setAttribute ("value", value);
            };

            addParam (param::glazeAmount, "62.0");

            if (genre != nullptr)
                addParam (param::legacy::glazeGenre, genre);

            addParam (param::glazeStyle, "1.0");

            if (delivery != nullptr)
                addParam (param::legacy::glazeDelivery, delivery);

            xml->createNewChildElement ("UNRELATED")->setAttribute ("id", param::legacy::glazeGenre);   // 不是 PARAM：不许碰
            return xml;
        }

        int countLegacyParams (const juce::XmlElement& xml)
        {
            auto count = 0;

            for (auto* node : xml.getChildIterator())
                if (node->hasTagName ("PARAM")
                    && (node->getStringAttribute ("id") == param::legacy::glazeGenre || node->getStringAttribute ("id") == param::legacy::glazeDelivery))
                    ++count;

            return count;
        }

        // 摘完之后剩下的应当正好是「GLAZE、风格、UNRELATED」三个元素，按原顺序、原值。
        bool othersUntouched (const juce::XmlElement& xml)
        {
            if (xml.getNumChildElements() != 3)
                return false;

            const auto* glaze = xml.getChildElement (0);
            const auto* style = xml.getChildElement (1);
            const auto* other = xml.getChildElement (2);

            return glaze->hasTagName ("PARAM") && glaze->getStringAttribute ("id") == param::glazeAmount && glaze->getStringAttribute ("value") == "62.0"
                && style->hasTagName ("PARAM") && style->getStringAttribute ("id") == param::glazeStyle && style->getStringAttribute ("value") == "1.0"
                && other->hasTagName ("UNRELATED") && other->getStringAttribute ("id") == param::legacy::glazeGenre;
        }
    }

    class LegacyStateTests  : public juce::UnitTest
    {
    public:
        LegacyStateTests() : juce::UnitTest ("LegacyState", "Celadon") {}

        void runTest() override
        {
            testFiveArchetypes();
            testValueEdges();
            testDuplicateAndRerun();
        }

    private:
        void expectCase (const char* genre, const char* delivery, bool expectedNonDefault, const juce::String& name)
        {
            auto xml = makeState (genre, delivery);
            const auto nonDefault = legacystate::removeGenreDeliveryNodes (*xml);

            expect (nonDefault == expectedNonDefault, name + ": " + (expectedNonDefault ? "should report a non-default genre / delivery" : "should stay silent"));
            expectEquals (countLegacyParams (*xml), 0, name + ": both legacy PARAM nodes are removed");
            expect (othersUntouched (*xml), name + ": the other nodes keep their order and values");
        }

        void testFiveArchetypes()
        {
            beginTest ("old projects: House / streaming / club report a change, General + Standard and no nodes stay silent; nodes always removed");
            expectCase ("1", "0", true, "House (genre 1)");
            expectCase ("3", "0", true, "Melodic-Progressive (genre 3)");
            expectCase ("0", "1", true, "streaming (delivery 1)");
            expectCase (nullptr, "2", true, "club (delivery 2, no genre node)");
            expectCase ("0", "0", false, "General + Standard");
            expectCase (nullptr, nullptr, false, "no genre / delivery nodes");
            expectCase ("0", nullptr, false, "only a default genre node");
        }

        void testValueEdges()
        {
            beginTest ("old projects: the stored index is rounded like AudioParameterChoice::getIndex; non-finite counts as changed, a missing value as default");
            expectCase ("0.4", "0", false, "genre 0.4 rounds to General");
            expectCase ("-0.4", "0", false, "genre -0.4 rounds to General");
            expectCase ("0.6", "0", true, "genre 0.6 rounds to Deep House");
            expectCase ("0", "1.6", true, "delivery 1.6 rounds to club");
            expectCase ("nan", "0", true, "genre NaN");
            expectCase ("0", "inf", true, "delivery +Inf");
            expectCase ("", "", false, "empty value attributes");

            auto missing = makeState (nullptr, nullptr);
            auto* node = missing->createNewChildElement ("PARAM");
            node->setAttribute ("id", param::legacy::glazeDelivery);   // 没有 value 属性
            expect (! legacystate::removeGenreDeliveryNodes (*missing), "a node without a value attribute reads as the default");
            expectEquals (countLegacyParams (*missing), 0, "the node without a value attribute is removed too");
            expect (othersUntouched (*missing));
        }

        void testDuplicateAndRerun()
        {
            beginTest ("old projects: duplicate legacy nodes are all removed (any non-default counts); a second pass changes nothing");
            auto xml = makeState ("0", "0");
            auto* extra = xml->createNewChildElement ("PARAM");
            extra->setAttribute ("id", param::legacy::glazeGenre);
            extra->setAttribute ("value", "2");
            expectEquals (countLegacyParams (*xml), 3);
            expect (legacystate::removeGenreDeliveryNodes (*xml), "a non-default duplicate still counts");
            expectEquals (countLegacyParams (*xml), 0);
            expect (othersUntouched (*xml));

            const auto before = xml->toString();
            expect (! legacystate::removeGenreDeliveryNodes (*xml), "re-running on a migrated tree reports nothing");
            expectEquals (xml->toString(), before, "re-running on a migrated tree leaves it unchanged");
        }
    };

    static LegacyStateTests legacyStateTests;
}
