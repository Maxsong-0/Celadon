/*
  ==============================================================================

    HelpTextTests.cpp
    提示文案覆盖测试：ParameterIds.h 里的每个参数、每个界面元素、每个 Balance 占位
    都必须在 HelpText 里有三行非空文案；气泡恰好三行；条目无重复、无多余。

    v0.1.7.0 追加 HelpTextClayTests（文件末尾）：修坯 7 个参数与两个界面 ID 的文案、三灯指示的颜色类别、
    气泡第 2 行的实时值（规格示例逐字对照、为 0 / 收窄 / 保护 / 旁通、显示精度、非有限值、第 3 行的重新聆听提示）。
    T-012 阶段 4（ADR-012）追加：第 3 行的「先试后用」代价说明（跳过 / 减量、多项、优先级、只算自动态、保护介入时不写、旁通清零）。

    被测代码只依赖 juce_core（HelpText.cpp 进无头测试编译；HelpText.h 另带 dsp/GlazeTypes.h 的纯数据类型）。测试本身借
    TestHelpers.h 的 utf8Contains / utf8StartsWith（审核 V3-01：juce::String 的 contains / startsWith 对中文 needle 只比前 1/3
    的字节，断言形同虚设），所以跟其他测试一样带上 juce_dsp。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ParameterIds.h"
#include "../Source/ui/HelpText.h"

#include <cstring>
#include <limits>

namespace celadon::test
{
    class HelpTextTests  : public juce::UnitTest
    {
    public:
        HelpTextTests() : juce::UnitTest ("HelpText", "Celadon") {}

        void runTest() override
        {
            beginTest ("every parameter and UI element has a three-line entry");
            {
                for (auto* id : param::allParameterIds)
                    checkEntry (id);

                for (auto* id : uiid::allUiIds)
                    checkEntry (id);
            }

            beginTest ("entry ids are unique");
            {
                const auto& entries = help::allEntries();

                for (int i = 0; i < entries.size(); ++i)
                    for (int j = i + 1; j < entries.size(); ++j)
                        expect (std::strcmp (entries[i].id, entries[j].id) != 0,
                                "duplicate entry id: " + juce::String (entries[i].id));
            }

            beginTest ("entry count equals the two id lists combined (no stray entries)");
            {
                const auto expected = juce::numElementsInArray (param::allParameterIds)
                                    + juce::numElementsInArray (uiid::allUiIds);

                expectEquals (help::allEntries().size(), expected);
            }

            beginTest ("unknown id finds nothing and yields an empty tooltip");
            {
                expect (help::find ("no_such_id") == nullptr);
                expect (help::tooltipFor ("no_such_id").isEmpty());
            }
        }

    private:
        // 一个 id 的全部检查：能找到、三行非空、气泡恰好三行、转换后是合法 Unicode 且不为空。
        void checkEntry (const char* id)
        {
            const juce::String label (id);   // id 全是 ASCII，可直接构造
            const auto* entry = help::find (id);

            expect (entry != nullptr, "missing help entry for " + label);

            if (entry == nullptr)
                return;

            expect (std::strcmp (entry->id, id) == 0, "find returned a different entry for " + label);

            checkLine (entry->title,  label + " title");
            checkLine (entry->effect, label + " effect");
            checkLine (entry->advice, label + " advice");

            const auto tooltip = help::tooltipFor (id);
            expect (tooltip.isNotEmpty(), "empty tooltip for " + label);
            expectEquals (juce::StringArray::fromLines (tooltip).size(), 3, "tooltip line count for " + label);
            expect (! tooltip.containsChar ((juce::juce_wchar) 0xFFFD), "tooltip contains U+FFFD for " + label);
            expect (! tooltip.endsWithChar ('\n'), "tooltip has a trailing newline for " + label);

            // v0.1.8.0 英文三行：同样的格式要求；不带语言参数的旧接口仍是中文；英文里不能混进中文字或全角标点（抄漏的行）。
            checkLine (entry->titleEn,  label + " English title");
            checkLine (entry->effectEn, label + " English effect");
            checkLine (entry->adviceEn, label + " English advice");

            const auto english = help::tooltipFor (id, Language::english);
            expectEquals (juce::StringArray::fromLines (english).size(), 3, "English tooltip line count for " + label);
            expectEquals (help::tooltipFor (id, Language::chinese), tooltip, "the Chinese overload equals the legacy call for " + label);
            expect (english != tooltip, "English tooltip is still Chinese for " + label);
            expect (! containsCjk (english), "English tooltip contains CJK text for " + label);
        }

        // 中日韩统一表意文字、中文标点（U+3000 ～ U+303F）、全角形式（U+FF00 ～ U+FFEF）。
        static bool containsCjk (const juce::String& text)
        {
            for (auto p = text.getCharPointer(); ! p.isEmpty(); ++p)
            {
                const auto c = (juce::uint32) *p;

                if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFFEF))
                    return true;
            }

            return false;
        }

        // 单行文案：源字节非空且是合法 UTF-8；转成 juce::String 后长度 > 0、不含替换符、不含换行。
        void checkLine (const char* text, const juce::String& what)
        {
            expect (text != nullptr && text[0] != '\0', "empty line: " + what);

            if (text == nullptr)
                return;

            expect (juce::CharPointer_UTF8::isValidString (text, (int) std::strlen (text)),
                    "invalid UTF-8 bytes: " + what);

            const auto converted = utf8 (text);
            expect (converted.length() > 0, "converted line is empty: " + what);
            expect (! converted.containsChar ((juce::juce_wchar) 0xFFFD), "line contains U+FFFD: " + what);
            expect (! converted.containsChar ('\n'), "line contains a newline: " + what);
        }
    };

    static HelpTextTests helpTextTests;

    //==============================================================================
    // v0.1.7.0 修坯（设计规格-Clay 第 5、6 节；ADR-009）：文案、三灯的颜色类别、气泡第 2 行的实时值。
    // 只按 HelpText.h 的公开接口写（clayReadoutFor / clayLampFor / clayLiveLine / clayIndicatorTooltip）。
    namespace
    {
        // 修坯测试用的状态快照：只填 clay* 字段（其余保持默认——与修坯的显示无关）。
        glaze::Status clayStatus (float lowCutHz, float monoHz, float monoAmount, float widthDb,
                                  bool phaseGuard = false, bool widthGuard = false, bool needsRelearn = false)
        {
            glaze::Status snapshot;
            snapshot.clayLowCutHz         = lowCutHz;
            snapshot.clayMonoHz           = monoHz;
            snapshot.clayMonoAmount       = monoAmount;
            snapshot.clayWidthDb          = widthDb;
            snapshot.clayPhaseGuardActive = phaseGuard;
            snapshot.clayWidthGuardActive = widthGuard;
            snapshot.clayNeedsRelearn     = needsRelearn;
            return snapshot;
        }

        // 默认用户设置 = 三项都「自动」、不旁通（参数默认值）。
        juce::String liveLineFor (const glaze::Status& snapshot, const help::ClayUserState& user = {})
        {
            return help::clayLiveLine (help::clayReadoutFor (snapshot, user));
        }

        help::ClayLamp lampFor (const glaze::Status& snapshot, help::ClayStage stage, const help::ClayUserState& user = {})
        {
            return help::clayLampFor (help::clayReadoutFor (snapshot, user), stage);
        }

        // 气泡的第 index 行（0 起）。
        juce::String tooltipLine (const juce::String& tooltip, int index)
        {
            return juce::StringArray::fromLines (tooltip)[index];
        }

        // ADR-012：同一份快照另带「先试后用」的代价 mask（glaze::clayCostLimited* 位）。
        glaze::Status withCostMask (glaze::Status snapshot, unsigned int mask)
        {
            snapshot.clayCostLimitedMask = static_cast<std::uint8_t> (mask);
            return snapshot;
        }

        // 气泡第 3 行。
        juce::String thirdLineFor (const glaze::Status& snapshot, const help::ClayUserState& user = {})
        {
            return tooltipLine (help::clayIndicatorTooltip (help::clayReadoutFor (snapshot, user)), 2);
        }

        constexpr unsigned int lowCutBit = glaze::clayCostLimitedLowCut;
        constexpr unsigned int monoBit   = glaze::clayCostLimitedMono;
        constexpr unsigned int widthBit  = glaze::clayCostLimitedWidth;
    }

    class HelpTextClayTests  : public juce::UnitTest
    {
    public:
        HelpTextClayTests() : juce::UnitTest ("HelpTextClay", "Celadon") {}

        void runTest() override
        {
            using help::ClayLamp;
            using help::ClayStage;

            // 依据：设计规格-Clay 第 5 节（抽屉每行的名字 低切 / 收单 / 加宽、三态 自动 / 关 / 手动、「Bypass 修坯」；
            // 指示灯气泡第 1 行「修坯 · 立体声像与超低频清理」）与第 6 节（7 个参数 + ui_clay_indicator 各有三行；v0.1.8.0 起
            // 修坯都在 CLAY 卡片上，顶栏「高级」ui_advanced 改为打开「高级」面板）。
            // 覆盖与三行格式由上面 HelpTextTests 的 allParameterIds / allUiIds 循环检查；这里查的是「文案说的是对的那个控件」。
            beginTest ("Clay entries name their control");
            {
                struct Expectation
                {
                    const char* id;
                    const char* titleStart;
                };

                const Expectation expectations[] =
                {
                    { param::clayLowCutMode, "低切" }, { param::clayLowCutHz, "低切" },
                    { param::clayMonoMode,   "收单" }, { param::clayMonoHz,   "收单" },
                    { param::clayWidthMode,  "加宽" }, { param::clayWidthDb,  "加宽" },
                    { param::clayBypass,     "Bypass" },
                    { uiid::clayIndicator,   "修坯" },
                };

                for (const auto& expectation : expectations)
                {
                    const auto* entry = help::find (expectation.id);
                    expect (entry != nullptr, "missing Clay help entry for " + juce::String (expectation.id));

                    if (entry != nullptr)
                        expect (utf8StartsWith (utf8 (entry->title), expectation.titleStart),
                                "title of " + juce::String (expectation.id) + " does not start with its control name");
                }

                if (const auto* indicator = help::find (uiid::clayIndicator))
                    expectEquals (utf8 (indicator->title), utf8 ("修坯 · 立体声像与超低频清理"));   // 规格第 5 节原文

                // 三个三态参数的第 2 行把三种状态都讲到（ADR-009 第 4 条：自动 / 关 / 手动）。
                for (const auto* id : { param::clayLowCutMode, param::clayMonoMode, param::clayWidthMode })
                    if (const auto* entry = help::find (id))
                        for (const auto* state : { "自动：", "关：", "手动：" })
                            expect (utf8Contains (utf8 (entry->effect), state),
                                    juce::String (id) + " effect line does not explain one of the three states");

                // 三个手动值参数都说明「自动」态手动值不生效（规格第 5 节；v0.1.8.0 起手动值在 CLAY 卡片上、「手动」时才可拖）。
                for (const auto* id : { param::clayLowCutHz, param::clayMonoHz, param::clayWidthDb })
                    if (const auto* entry = help::find (id))
                        expect (utf8Contains (utf8 (entry->effect), "「自动」时手动值不生效"),
                                juce::String (id) + " effect line does not say the manual value is inactive in automatic mode");

                // 阳性对照（审核 V3-01）：字节精确的辅助真的会拦下「只有首字相同」的错字——juce::String::contains / startsWith
                // 在这里会误报命中（只比前 1/3 的字节），换成它们上面几条断言就形同虚设。
                expect (! utf8Contains (utf8 ("自选：永远不切"), "自动："), "utf8Contains: same first character is not a match");
                expect (! utf8StartsWith (utf8 ("低音很宽的歌"), "低切"), "utf8StartsWith: same first bytes are not a match");
                expect (utf8Contains (utf8 ("关：永远不切"), "关：") && utf8StartsWith (utf8 ("低切 · 清理"), "低切"),
                        "utf8Contains / utf8StartsWith: real matches are found");
            }

            // 依据：设计规格-Clay 第 5 节的气泡示例「低切 25 Hz · 120 Hz 以下收单 80% · 高频加宽 +0.8 dB · 反相保护中」，ADR-015 起最后一段
            // 改为「低频反相：单声道播放会丢低音」（只提示、不改声音）——实际值 低切 25 Hz、收单分频 120 Hz × 比例 0.8（= 80 %）、
            // 加宽 +0.8 dB，反相提示亮着，三项都「自动」。
            beginTest ("Clay live line reproduces the spec example word for word");
            {
                expectEquals (liveLineFor (clayStatus (25.0f, 120.0f, 0.8f, 0.8f, true)),
                              utf8 ("低切 25 Hz · 120 Hz 以下收单 80% · 高频加宽 +0.8 dB · 低频反相：单声道播放会丢低音"));
            }

            beginTest ("Clay live line: stages at zero, narrowing, both guards, bypass");
            {
                // 三项实际值都为 0（GLAZE 0 且抽屉默认、旧工程）：规格「各项为 0 时写「未低切」等简洁说法」。收单比例为 0 时
                // 分频点无意义，不写（快照里的 80 Hz 是默认值）。
                expectEquals (liveLineFor (clayStatus (0.0f, 80.0f, 0.0f, 0.0f)), utf8 ("未低切 · 未收单 · 未加宽"));

                // 手动收窄 −0.5 dB（负宽度只有「手动」给得出，规格第 4 节 ③）：写「收窄」+ 绝对值，不写双重否定的负号。
                expectEquals (liveLineFor (clayStatus (0.0f, 80.0f, 0.0f, -0.5f)), utf8 ("未低切 · 未收单 · 高频收窄 0.5 dB"));

                // 宽度保护把自动加宽收到 0：仍写「宽度保护中」（灯是琥珀，见下一组）；反相提示与宽度保护同时出现按 反相 → 宽度 排。
                const auto guarded = clayStatus (25.0f, 120.0f, 1.0f, 0.0f, true, true);
                expectEquals (liveLineFor (guarded),
                              utf8 ("低切 25 Hz · 120 Hz 以下收单 100% · 未加宽 · 低频反相：单声道播放会丢低音 · 宽度保护中"));

                // 旁通：同一份快照（阳性对照是上一条的完整读数），旁通后只写「修坯已旁通」。
                help::ClayUserState bypassed;
                bypassed.bypassed = true;
                expectEquals (liveLineFor (guarded, bypassed), utf8 ("修坯已旁通"));
            }

            // 依据：设计规格-Clay 第 5 节：实际值非 0 → 梅子青（on）；该项保护介入 → 琥珀（guard：反相保护对「单」、宽度保护对「宽」）；
            // 否则 35% 暗（off）。低切没有保护。
            beginTest ("Clay lamps follow actual values and guard flags");
            {
                const auto idle = clayStatus (0.0f, 80.0f, 0.0f, 0.0f);
                expect (lampFor (idle, ClayStage::lowCut) == ClayLamp::off);
                expect (lampFor (idle, ClayStage::mono)   == ClayLamp::off);
                expect (lampFor (idle, ClayStage::width)  == ClayLamp::off);

                const auto working = clayStatus (25.0f, 120.0f, 0.8f, 0.8f);
                expect (lampFor (working, ClayStage::lowCut) == ClayLamp::on);
                expect (lampFor (working, ClayStage::mono)   == ClayLamp::on);
                expect (lampFor (working, ClayStage::width)  == ClayLamp::on);

                // 保护压过「亮」：宽度保护把加宽收到 0 dB 时灯仍是琥珀（「想加宽、被保护拦住」）。
                const auto guarded = clayStatus (25.0f, 120.0f, 1.0f, 0.0f, true, true);
                expect (lampFor (guarded, ClayStage::lowCut) == ClayLamp::on);
                expect (lampFor (guarded, ClayStage::mono)   == ClayLamp::guard);
                expect (lampFor (guarded, ClayStage::width)  == ClayLamp::guard);

                // 负宽度（手动收窄）也是「在处理」。
                expect (lampFor (clayStatus (0.0f, 80.0f, 0.0f, -0.5f), ClayStage::width) == ClayLamp::on);

                // 旁通：同一份快照（阳性对照 = 上面的 on / guard）三灯全暗。
                help::ClayUserState bypassed;
                bypassed.bypassed = true;
                expect (lampFor (guarded, ClayStage::lowCut, bypassed) == ClayLamp::off);
                expect (lampFor (guarded, ClayStage::mono,   bypassed) == ClayLamp::off);
                expect (lampFor (guarded, ClayStage::width,  bypassed) == ClayLamp::off);
            }

            // 依据：ADR-009 第 4 条、设计规格-Clay-分析决策与验收 第 3 节：保护只管「自动」态的项，「手动」「关」态不受保护改动——
            // 即使快照里保护单元标着介入，界面也不把它算到手动的项上。
            beginTest ("Clay guards only count for stages in automatic mode");
            {
                const auto guarded = clayStatus (25.0f, 120.0f, 1.0f, 0.8f, true, true);

                // 阳性对照：同一快照、三项都「自动」→ 琥珀、写保护。
                expect (lampFor (guarded, ClayStage::mono)  == ClayLamp::guard);
                expect (lampFor (guarded, ClayStage::width) == ClayLamp::guard);

                help::ClayUserState manual;
                manual.monoAuto  = false;
                manual.widthAuto = false;

                // 收单与加宽改成非自动：实际值仍非 0 → 亮（梅子青），不是琥珀；第 2 行不写保护。
                expect (lampFor (guarded, ClayStage::mono,  manual) == ClayLamp::on);
                expect (lampFor (guarded, ClayStage::width, manual) == ClayLamp::on);
                expectEquals (liveLineFor (guarded, manual), utf8 ("低切 25 Hz · 120 Hz 以下收单 100% · 高频加宽 +0.8 dB"));
            }

            // 依据：HelpText.h ClayReadout 的约定：Hz 取整、收单比例取整百分比、加宽 0.1 dB；实际值非 0 时显示值至少 1 个单位
            // （灯亮着，文字不写 0）；相等的读数 = 画面一个像素都不变（灯不重画、气泡不重拼）。
            beginTest ("Clay display precision: non-zero never shows as zero, jitter below precision is ignored");
            {
                // 收单比例 0.001（× 100 = 0.1，四舍五入是 0）：灯亮，文字写 1%。
                const auto tinyMono = clayStatus (0.0f, 120.0f, 0.001f, 0.0f);
                expectEquals (liveLineFor (tinyMono), utf8 ("未低切 · 120 Hz 以下收单 1% · 未加宽"));
                expect (lampFor (tinyMono, ClayStage::mono) == ClayLamp::on);

                // 加宽 ±0.02 dB（× 10 = 0.2，四舍五入是 0）：写 ±0.1。
                expectEquals (liveLineFor (clayStatus (0.0f, 80.0f, 0.0f, 0.02f)),  utf8 ("未低切 · 未收单 · 高频加宽 +0.1 dB"));
                expectEquals (liveLineFor (clayStatus (0.0f, 80.0f, 0.0f, -0.02f)), utf8 ("未低切 · 未收单 · 高频收窄 0.1 dB"));

                // 取整：低切 24.6 Hz → 25；加宽 1.46 dB（× 10 = 14.6）→ 1.5。
                expectEquals (liveLineFor (clayStatus (24.6f, 80.0f, 0.0f, 1.46f)), utf8 ("低切 25 Hz · 未收单 · 高频加宽 +1.5 dB"));

                // 显示精度以下的抖动：25.2 / 25.3 Hz 都显示 25，120.3 / 119.8 Hz 都是 120，0.801 / 0.804 都是 80 %，
                // 0.81 / 0.84 dB 都是 0.8 → 读数相等。
                expect (help::clayReadoutFor (clayStatus (25.2f, 120.3f, 0.801f, 0.81f), {})
                         == help::clayReadoutFor (clayStatus (25.3f, 119.8f, 0.804f, 0.84f), {}));

                // 阳性对照：低切 25.2 → 26.0 Hz 过了 1 Hz 精度，读数不等。
                expect (help::clayReadoutFor (clayStatus (25.2f, 120.0f, 0.8f, 0.8f), {})
                         != help::clayReadoutFor (clayStatus (26.0f, 120.0f, 0.8f, 0.8f), {}));

                // 收单比例为 0 时分频点不进读数（换分频点不重拼）；旁通时实际值不进读数（旁通期间实际值怎么变都不重拼）。
                expect (help::clayReadoutFor (clayStatus (0.0f, 80.0f, 0.0f, 0.0f), {})
                         == help::clayReadoutFor (clayStatus (0.0f, 160.0f, 0.0f, 0.0f), {}));

                help::ClayUserState bypassed;
                bypassed.bypassed = true;
                expect (help::clayReadoutFor (clayStatus (25.0f, 120.0f, 0.8f, 0.8f, true), bypassed)
                         == help::clayReadoutFor (clayStatus (40.0f, 150.0f, 0.5f, -1.0f), bypassed));
            }

            // 非有限的实际值（数据源出错）按 0 处理：不亮、写「未…」，不把 NaN 写进文字。
            beginTest ("Clay non-finite actual values count as off");
            {
                const auto notANumber = std::numeric_limits<float>::quiet_NaN();
                const auto infinite   = std::numeric_limits<float>::infinity();
                const auto broken     = clayStatus (notANumber, notANumber, notANumber, infinite);

                expectEquals (liveLineFor (broken), utf8 ("未低切 · 未收单 · 未加宽"));
                expect (lampFor (broken, ClayStage::lowCut) == ClayLamp::off);
                expect (lampFor (broken, ClayStage::mono)   == ClayLamp::off);
                expect (lampFor (broken, ClayStage::width)  == ClayLamp::off);
            }

            // 依据：设计规格-Clay 第 5 节：悬停气泡沿用三行格式，第 2 行是实时值；旧画像没有立体声特征时第 3 行写
            // 「点『重新聆听』后启用自动修坯」（单独成行时引号按本文件的写法用「」）。
            beginTest ("Clay tooltip: three lines, table title and advice, relearn hint on old projects");
            {
                const auto* entry = help::find (uiid::clayIndicator);
                expect (entry != nullptr);

                if (entry != nullptr)
                {
                    const auto working = clayStatus (25.0f, 120.0f, 0.8f, 0.8f, true);
                    const auto tip = help::clayIndicatorTooltip (help::clayReadoutFor (working, {}));

                    expectEquals (juce::StringArray::fromLines (tip).size(), 3);
                    expectEquals (tooltipLine (tip, 0), utf8 (entry->title));
                    expectEquals (tooltipLine (tip, 1), liveLineFor (working));
                    expectEquals (tooltipLine (tip, 2), utf8 (entry->advice));

                    // 旧工程（已提交画像缺立体声 / 超低频特征）且有「自动」态的项：第 3 行换成重新聆听的提示。
                    const auto oldProject = clayStatus (0.0f, 80.0f, 0.0f, 0.0f, false, false, true);
                    const auto oldTip = help::clayIndicatorTooltip (help::clayReadoutFor (oldProject, {}));

                    expectEquals (juce::StringArray::fromLines (oldTip).size(), 3);
                    expectEquals (tooltipLine (oldTip, 1), utf8 ("未低切 · 未收单 · 未加宽"));
                    expectEquals (tooltipLine (oldTip, 2), utf8 ("点「重新聆听」后启用自动修坯。"));
                    expect (utf8StartsWith (tooltipLine (oldTip, 2), help::clayRelearnHint()));

                    // 三项都不是「自动」：重新聆听也不改变声音，不提示（阳性对照 = 上一条有自动态时的提示）。
                    help::ClayUserState noneAutomatic;
                    noneAutomatic.lowCutAuto = false;
                    noneAutomatic.monoAuto   = false;
                    noneAutomatic.widthAuto  = false;
                    expectEquals (tooltipLine (help::clayIndicatorTooltip (help::clayReadoutFor (oldProject, noneAutomatic)), 2),
                                  utf8 (entry->advice));

                    // 正在重新聆听：已提交的仍是旧画像，但「点重新聆听」已经没有意义，不提示（T-012 阶段 2，A6 报告备注）。
                    auto relearning = oldProject;
                    relearning.relearnInProgress = true;
                    expectEquals (tooltipLine (help::clayIndicatorTooltip (help::clayReadoutFor (relearning, {})), 2),
                                  utf8 (entry->advice));
                }
            }

            // 依据：设计规格-Clay-根治修订 3.5「某项因代价被减量 / 否决时写『本曲试过：{低切 / 收单 / 加宽}会抬高峰值，已自动{减量 / 跳过}』
            // （优先级低于『点重新聆听』提示）」；任务书「跳过 = 该项实际值为 0；多项用『、』连接」。两种都有时分别点名（HelpText.h）。
            beginTest ("Clay tooltip line 3 (ADR-012): tried-and-limited items explain themselves, skipped vs reduced, several joined");
            {
                const auto* entry = help::find (uiid::clayIndicator);
                expect (entry != nullptr);
                const auto advice = entry != nullptr ? utf8 (entry->advice) : juce::String();

                // 阳性对照：没有被代价减量 / 否决的项 → 第 3 行是文案表的建议；代价说明为空串。
                const auto working = clayStatus (18.0f, 120.0f, 0.4f, 0.6f);
                expectEquals (thirdLineFor (working), advice, "nothing cost-limited: the table's advice");
                expect (help::clayCostLine (help::clayReadoutFor (working, {})).isEmpty(), "no cost line without a mask");

                // 单项：低切被否决（实际值 0 → 跳过）/ 被减量（实际值 14 Hz → 减量）。
                expectEquals (thirdLineFor (withCostMask (clayStatus (0.0f, 120.0f, 0.4f, 0.6f), lowCutBit)),
                              utf8 ("本曲试过：低切会抬高峰值，已自动跳过。"));
                expectEquals (thirdLineFor (withCostMask (clayStatus (14.0f, 120.0f, 0.4f, 0.6f), lowCutBit)),
                              utf8 ("本曲试过：低切会抬高峰值，已自动减量。"));

                // 多项同一种：按处理顺序（低切 → 收单 → 加宽）用「、」连接。
                expectEquals (thirdLineFor (withCostMask (clayStatus (0.0f, 80.0f, 0.0f, 0.0f), lowCutBit | monoBit | widthBit)),
                              utf8 ("本曲试过：低切、收单、加宽会抬高峰值，已自动跳过。"));
                expectEquals (thirdLineFor (withCostMask (clayStatus (14.0f, 120.0f, 0.3f, 0.5f), widthBit | lowCutBit)),
                              utf8 ("本曲试过：低切、加宽会抬高峰值，已自动减量。"));

                // 两种都有：收单被否决、加宽被减量。
                expectEquals (thirdLineFor (withCostMask (clayStatus (18.0f, 80.0f, 0.0f, 0.5f), monoBit | widthBit)),
                              utf8 ("本曲试过：收单、加宽会抬高峰值，已自动跳过收单，减量加宽。"));

                // clayCostLine 本身不带句末「。」（与 clayRelearnHint 一样由气泡补）。
                expectEquals (help::clayCostLine (help::clayReadoutFor (withCostMask (clayStatus (0.0f, 80.0f, 0.0f, 0.0f), monoBit), {})),
                              utf8 ("本曲试过：收单会抬高峰值，已自动跳过"));

                // 「跳过」按量化读数判：收单比例 0.001（显示 1 %，灯亮）算减量，不是跳过。
                expectEquals (thirdLineFor (withCostMask (clayStatus (0.0f, 120.0f, 0.001f, 0.0f), monoBit)),
                              utf8 ("本曲试过：收单会抬高峰值，已自动减量。"));

                // 三行格式不变（第 2 行照旧是实时值）。
                const auto tip = help::clayIndicatorTooltip (help::clayReadoutFor (withCostMask (clayStatus (0.0f, 120.0f, 0.4f, 0.6f), lowCutBit), {}));
                expectEquals (juce::StringArray::fromLines (tip).size(), 3);
                expectEquals (tooltipLine (tip, 1), utf8 ("未低切 · 120 Hz 以下收单 40% · 高频加宽 +0.6 dB"));
                expect (utf8StartsWith (tooltipLine (tip, 2), "本曲试过："), "line 3 starts with the cost explanation");
            }

            // 依据：同上「优先级低于『点重新聆听』提示」；ADR-009 第 4 条（「关 / 手动」项不经决策器、保护只管自动态）；
            // HelpText.h ClayReadout 代价标志的约定（该项保护介入时不写代价、旁通时全清零）。
            beginTest ("Clay tooltip line 3 (ADR-012): relearn hint first, only automatic items, not while that item's guard acts, cleared by bypass");
            {
                const auto* entry = help::find (uiid::clayIndicator);
                expect (entry != nullptr);
                const auto advice = entry != nullptr ? utf8 (entry->advice) : juce::String();

                // 旧工程提示压过代价说明（阳性对照：同一 mask、不提示重新聆听时写代价）。
                const auto withRelearn = withCostMask (clayStatus (0.0f, 80.0f, 0.0f, 0.0f, false, false, true), lowCutBit);
                expectEquals (thirdLineFor (withRelearn), utf8 ("点「重新聆听」后启用自动修坯。"));
                auto noRelearn = withRelearn;
                noRelearn.clayNeedsRelearn = false;
                expectEquals (thirdLineFor (noRelearn), utf8 ("本曲试过：低切会抬高峰值，已自动跳过。"));

                // 只算「自动」态的项：低切改成「手动」→ 不再写它；还有自动态的加宽被减量 → 只写加宽。
                const auto both = withCostMask (clayStatus (30.0f, 120.0f, 0.4f, 0.5f), lowCutBit | widthBit);
                help::ClayUserState lowCutManual;
                lowCutManual.lowCutAuto = false;
                expectEquals (thirdLineFor (both, lowCutManual), utf8 ("本曲试过：加宽会抬高峰值，已自动减量。"));
                lowCutManual.widthAuto = false;
                expectEquals (thirdLineFor (both, lowCutManual), advice, "no automatic item left: the table's advice");

                // 反相提示亮着时不写收单的代价（旧快照里可能还带着收单的代价位；ADR-015 起决策器不再置它）：第 2 行已写「低频反相」。
                const auto guardedMono = withCostMask (clayStatus (0.0f, 80.0f, 1.0f, 0.0f, true, false), monoBit);
                expectEquals (thirdLineFor (guardedMono), advice, "mono guard acting: no cost line for mono");
                expect (utf8Contains (liveLineFor (guardedMono), "低频反相"), "line 2 says why instead");
                auto guardOver = guardedMono;
                guardOver.clayPhaseGuardActive = false;
                guardOver.clayMonoAmount = 0.0f;
                expectEquals (thirdLineFor (guardOver), utf8 ("本曲试过：收单会抬高峰值，已自动跳过。"), "positive control: guard released");

                const auto guardedWidth = withCostMask (clayStatus (0.0f, 80.0f, 0.0f, 0.0f, false, true), widthBit);
                expectEquals (thirdLineFor (guardedWidth), advice, "width guard acting: no cost line for width");

                // 旁通：什么都没在处理，代价说明一并清零（第 2 行「修坯已旁通」）。
                help::ClayUserState bypassed;
                bypassed.bypassed = true;
                const auto limited = withCostMask (clayStatus (0.0f, 120.0f, 0.4f, 0.6f), lowCutBit | monoBit | widthBit);
                expectEquals (thirdLineFor (limited, bypassed), advice);
                expect (! help::clayReadoutFor (limited, bypassed).lowCutCostLimited, "bypass clears the cost flags");

                // 读数相等 = 气泡一个字都不会变：mask 变了读数就不等（气泡按新读数重拼）；灯的颜色类别不看 mask。
                const auto plain = clayStatus (14.0f, 120.0f, 0.4f, 0.6f);
                const auto masked = withCostMask (plain, lowCutBit);
                expect (help::clayReadoutFor (plain, {}) != help::clayReadoutFor (masked, {}), "a new mask is a new readout");
                expect (help::clayReadoutFor (masked, {}) == help::clayReadoutFor (withCostMask (plain, lowCutBit), {}));
                expect (lampFor (plain, ClayStage::lowCut) == lampFor (masked, ClayStage::lowCut)
                        && lampFor (plain, ClayStage::mono) == lampFor (masked, ClayStage::mono)
                        && lampFor (plain, ClayStage::width) == lampFor (masked, ClayStage::width),
                        "the lamps ignore the cost mask (a reduced item still works, a skipped one is simply off)");

                // 无效位（高于三个有效位）不产生任何说明。
                expectEquals (thirdLineFor (withCostMask (plain, 0xf8u)), advice, "undefined mask bits are ignored");
            }

            // 依据：ADR-014 T-032 修订（2026-09-29，研究 R4-01 / 核验 V4）——峰值守护只管低切与收单，加宽的增量不经守护（ClayModule.h
            // 文件头④）。三处文案不能再说「任何状态下峰值处都会自动让一点」「修坯从不让峰值变高」（旧文案：加宽在正常电平下其实被
            // 守护回收约 95 %、界面却说只让一点）；中英文都要讲到「加宽不经守护」「低切 / 收单在会抬峰值的瞬间让开」。指示灯管自动与手动
            // 两种状态，「由先试后用把关」只对自动加宽成立（手动加宽只有末级限幅器兜），所以要写明是「自动加宽」。
            // 阳性对照：同一组断言对改前的 HelpText.cpp（e6b445b）全部不过（T-032 报告记录）。
            beginTest ("Clay texts (T-032): the peak guard covers low cut and mono only; width is not guarded");
            {
                const auto* lowCut = help::find (param::clayLowCutHz);
                const auto* widthMode = help::find (param::clayWidthMode);
                const auto* indicator = help::find (uiid::clayIndicator);
                expect (lowCut != nullptr && widthMode != nullptr && indicator != nullptr, "the three Clay entries exist");

                if (lowCut != nullptr && widthMode != nullptr && indicator != nullptr)
                {
                    expect (utf8Contains (utf8 (widthMode->effect), "加宽不经峰值守护"), "width mode: says width is not guarded");
                    expect (! utf8Contains (utf8 (widthMode->effect), "让一点"), "width mode: no longer claims it yields a little at peaks");
                    expect (juce::String (widthMode->effectEn).contains ("not run through the peak guard")
                              && ! juce::String (widthMode->effectEn).contains ("yields a little"),
                            "width mode (English): says width is not guarded, no longer claims it yields a little");

                    expect (utf8Contains (utf8 (indicator->effect), "低切与收单从不让峰值变高")
                              && utf8Contains (utf8 (indicator->effect), "加宽不经守护"),
                            "indicator: the guard covers low cut and mono, width is not guarded");
                    expect (! utf8Contains (utf8 (indicator->effect), "修坯从不让峰值变高"), "indicator: no longer claims Clay as a whole never raises peaks");
                    expect (juce::String (indicator->effectEn).contains ("Low cut and mono never make peaks higher")
                              && juce::String (indicator->effectEn).contains ("width is not guarded")
                              && ! juce::String (indicator->effectEn).contains ("Clay never makes peaks higher"),
                            "indicator (English): same scope");
                    expect (utf8Contains (utf8 (indicator->effect), "自动加宽由先试后用把关")
                              && juce::String (indicator->effectEn).contains ("automatic width is kept in check by the trial"),
                            "indicator: only the automatic width goes through the trial (a manual width is what you set)");

                    expect (utf8Contains (utf8 (lowCut->advice), "峰值守护会把低切让回去") && ! utf8Contains (utf8 (lowCut->advice), "让一点"),
                            "low cut advice: the guard yields the low cut at peaks it would raise");
                    expect (juce::String (lowCut->adviceEn).contains ("the peak guard yields the low cut")
                              && ! juce::String (lowCut->adviceEn).contains ("yields a little"),
                            "low cut advice (English): same");
                }
            }
        }
    };

    static HelpTextClayTests helpTextClayTests;
}
