/*
  ==============================================================================

    HelpTextTests.cpp
    提示文案覆盖测试：ParameterIds.h 里的每个参数、每个界面元素、每个 Balance 占位
    都必须在 HelpText 里有三行非空文案；气泡恰好三行；条目无重复、无多余。

    v0.1.7.0 追加 HelpTextClayTests（文件末尾）：修坯 7 个参数与两个界面 ID 的文案、三灯指示的颜色类别、
    气泡第 2 行的实时值（规格示例逐字对照、为 0 / 收窄 / 反相提示与宽度保护 / 旁通、显示精度、非有限值、第 3 行的重新聆听提示）。
    T-012 阶段 4（ADR-012）追加：第 3 行的「先试后用」代价说明（跳过 / 减量、多项、优先级、只算自动态、保护介入或反相提示时不写、旁通清零）。
    T-038 追加 HelpTextHouseTests（文件末尾）：曲风 / 交付文案、House 反相提示的文字与读数（不按三态与旁通筛）、House 暂定期间的悬停气泡。

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

            // 依据：设计规格-Clay 第 5 节（CLAY 卡片每行的名字 低切 / 收单 / 加宽、三态 自动 / 关 / 手动、「Bypass 修坯」；
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
                // 三项实际值都为 0（GLAZE 0 且三态默认、旧工程）：规格「各项为 0 时写「未低切」等简洁说法」。收单比例为 0 时
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

            // 依据：设计规格-Clay 第 5 节：实际值非 0 → 亮（on，「自动」紫圈、「手动」青色实心）；该项保护介入或反相提示亮着 → 琥珀（guard：反相提示对「单」、
            // 宽度保护对「宽」；ADR-015 起反相一路只提示、不改声音）；否则 35% 暗（off）。低切没有保护。
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

                // 保护 / 反相提示压过「亮」：宽度保护把加宽收到 0 dB 时灯仍是琥珀（「想加宽、被保护拦住」）。
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

            // 依据：T-074（设计 R4-03，核验 V4）：峰值守护深度回收低切 / 收单（Status::clayPeakGuardActive：保留比 < 50 % 持续 ≥ 1 s）→
            // 低切、收单两盏灯琥珀；气泡第 2 行最后写「峰值守护回收中：低切 / 收单实际只剩约 N%」（N 取整到 5；只在做着的那几级里点名）；
            // 手动 / 自动都报（不按三态筛，手动项恰恰没有别的反馈）；低切与收单实际值都为 0 时没有东西可回收、不亮；加宽不经守护、不受影响；旁通清零。
            beginTest ("Clay peak guard recovery indicator (T-074): amber lamps, the live line says how much is left, any mode, bypass clears it");
            {
                const auto with = [] (glaze::Status snapshot, bool active, float kept)
                {
                    snapshot.clayPeakGuardActive = active;
                    snapshot.clayPeakGuardKept = kept;
                    return snapshot;
                };

                const auto working = clayStatus (25.0f, 120.0f, 0.8f, 0.8f);   // 低切、收单、加宽都在做
                const auto plainLine = utf8 ("低切 25 Hz · 120 Hz 以下收单 80% · 高频加宽 +0.8 dB");

                // 阳性对照：不亮时灯是 on、气泡没有这一段（即使保留比读数很低：灯由「回收中」决定，不由读数决定）。
                expectEquals (liveLineFor (with (working, false, 0.2f)), plainLine);
                expect (lampFor (with (working, false, 0.2f), ClayStage::lowCut) == ClayLamp::on);
                expect (lampFor (with (working, false, 0.2f), ClayStage::mono)   == ClayLamp::on);

                // 亮：低切、收单琥珀（加宽照旧）；写保留比（0.42 → 取整到 5 % = 40）。
                const auto recovering = with (working, true, 0.42f);
                expect (lampFor (recovering, ClayStage::lowCut) == ClayLamp::guard);
                expect (lampFor (recovering, ClayStage::mono)   == ClayLamp::guard);
                expect (lampFor (recovering, ClayStage::width)  == ClayLamp::on);
                expectEquals (liveLineFor (recovering), plainLine + utf8 (" · 峰值守护回收中：低切 / 收单实际只剩约 40%"));
                expectEquals (help::clayLiveLine (help::clayReadoutFor (recovering, {}), Language::english),
                              utf8 ("Low cut 25 Hz \xc2\xb7 Mono 80% below 120 Hz \xc2\xb7 High width +0.8 dB \xc2\xb7 Peak guard pulling back low cut / mono: only about 40% left"));

                // 只点名做着的那几级：只开低切 → 「低切」；只开收单 → 「收单」。
                expectEquals (liveLineFor (with (clayStatus (25.0f, 80.0f, 0.0f, 0.0f), true, 0.5f)),
                              utf8 ("低切 25 Hz · 未收单 · 未加宽 · 峰值守护回收中：低切实际只剩约 50%"));
                expectEquals (liveLineFor (with (clayStatus (0.0f, 120.0f, 1.0f, 0.0f), true, 0.5f)),
                              utf8 ("未低切 · 120 Hz 以下收单 100% · 未加宽 · 峰值守护回收中：收单实际只剩约 50%"));
                expect (lampFor (with (clayStatus (25.0f, 80.0f, 0.0f, 0.0f), true, 0.5f), ClayStage::lowCut) == ClayLamp::guard);
                expect (lampFor (with (clayStatus (25.0f, 80.0f, 0.0f, 0.0f), true, 0.5f), ClayStage::mono)   == ClayLamp::off);
                expect (lampFor (with (clayStatus (0.0f, 120.0f, 1.0f, 0.0f), true, 0.5f), ClayStage::lowCut) == ClayLamp::off);
                expect (lampFor (with (clayStatus (0.0f, 120.0f, 1.0f, 0.0f), true, 0.5f), ClayStage::mono)   == ClayLamp::guard);

                // 低切与收单都没在做（只有加宽）：没有东西可回收，即使快照标着「回收中」也不亮、不写。
                const auto widthOnly = with (clayStatus (0.0f, 80.0f, 0.0f, 0.8f), true, 0.3f);
                expect (lampFor (widthOnly, ClayStage::width) == ClayLamp::on && lampFor (widthOnly, ClayStage::lowCut) == ClayLamp::off);
                expectEquals (liveLineFor (widthOnly), utf8 ("未低切 · 未收单 · 高频加宽 +0.8 dB"));

                // 手动 / 关 / 自动都报：三态一律不筛（对照：反相提示与宽度保护按三态筛，见下一条测试）。
                help::ClayUserState manual;
                manual.lowCutAuto = false;
                manual.monoAuto = false;
                manual.widthAuto = false;
                expect (lampFor (recovering, ClayStage::lowCut, manual) == ClayLamp::guard);
                expect (lampFor (recovering, ClayStage::mono, manual)   == ClayLamp::guard);
                expectEquals (liveLineFor (recovering, manual), liveLineFor (recovering));

                // 与反相提示、宽度保护同时出现：反相 → 宽度 → 峰值守护。
                const auto all = with (clayStatus (25.0f, 120.0f, 1.0f, 0.0f, true, true), true, 0.25f);
                expectEquals (liveLineFor (all),
                              utf8 ("低切 25 Hz · 120 Hz 以下收单 100% · 未加宽 · 低频反相：单声道播放会丢低音 · 宽度保护中 · 峰值守护回收中：低切 / 收单实际只剩约 25%"));

                // 旁通：同一份快照（阳性对照 = 上面的 guard），旁通后只写「修坯已旁通」、三灯全暗。
                help::ClayUserState bypassed;
                bypassed.bypassed = true;
                expectEquals (liveLineFor (recovering, bypassed), utf8 ("修坯已旁通"));
                expect (lampFor (recovering, ClayStage::lowCut, bypassed) == ClayLamp::off && lampFor (recovering, ClayStage::mono, bypassed) == ClayLamp::off);

                // 保留比取整到 5 %、钳到 0 ～ 100；非有限按 100（不写「只剩 0」）；读数在亮着时才有意义，灭着时记 100。
                const auto keptPercent = [&] (float kept) { return help::clayReadoutFor (with (working, true, kept), {}).peakGuardKeptPercent; };
                expectEquals (keptPercent (0.47f), 45);
                expectEquals (keptPercent (0.48f), 50);
                expectEquals (keptPercent (0.0f), 0);
                expectEquals (keptPercent (1.5f), 100);
                expectEquals (keptPercent (-0.2f), 0);
                expectEquals (keptPercent (std::numeric_limits<float>::quiet_NaN()), 100);
                expectEquals (help::clayReadoutFor (with (working, false, 0.1f), {}).peakGuardKeptPercent, 100);

                // 读数相等才不重画：保留比只在 5 % 的台阶上变；亮灭一变读数就不等。
                expect (help::clayReadoutFor (with (working, true, 0.41f), {}) == help::clayReadoutFor (with (working, true, 0.42f), {}),
                        "kept ratio within the same 5 % step: identical readout (no repaint)");
                expect (help::clayReadoutFor (with (working, true, 0.42f), {}) != help::clayReadoutFor (with (working, true, 0.50f), {}));
                expect (help::clayReadoutFor (with (working, true, 0.42f), {}) != help::clayReadoutFor (with (working, false, 0.42f), {}));

                // 灯条目的第 3 行（界面上显示的那行）讲了新灯色。
                expect (utf8Contains (thirdLineFor (recovering), "低切 / 收单灯琥珀 = 峰值守护"), "the lamp help explains the amber low cut / mono lamps");
                expect (tooltipLine (help::clayIndicatorTooltip (help::clayReadoutFor (recovering, {}), Language::english), 2).contains ("amber low cut / mono = the peak guard"),
                        "the English lamp help explains the amber low cut / mono lamps");
            }

            // 依据：ADR-009 第 4 条、设计规格-Clay-分析决策与验收 第 3 节：宽度保护只管「自动」态的项，「手动」「关」态不受保护改动；
            // 反相提示只在收单「自动」时报（ADR-015；「手动」「关」= 用户接管）——即使快照里标着介入 / 提示，界面也不把它算到手动的项上。
            beginTest ("Clay guards only count for stages in automatic mode");
            {
                const auto guarded = clayStatus (25.0f, 120.0f, 1.0f, 0.8f, true, true);

                // 阳性对照：同一快照、三项都「自动」→ 琥珀、写反相提示与宽度保护。
                expect (lampFor (guarded, ClayStage::mono)  == ClayLamp::guard);
                expect (lampFor (guarded, ClayStage::width) == ClayLamp::guard);

                help::ClayUserState manual;
                manual.monoAuto  = false;
                manual.widthAuto = false;

                // 收单与加宽改成非自动：实际值仍非 0 → 亮（「手动」是青色实心），不是琥珀；第 2 行不写反相提示与宽度保护。
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

            // 依据：设计规格-Clay 第 5 节：悬停气泡沿用三行格式，第 2 行是实时值。
            // T-048：旧画像不再在气泡里写「点重新聆听」（那一句改由编辑器提示一次）。
            beginTest ("Clay tooltip: three lines, table title and advice; old projects do not ask to relearn");
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

                    const auto oldProject = clayStatus (0.0f, 80.0f, 0.0f, 0.0f, false, false, true);
                    const auto oldTip = help::clayIndicatorTooltip (help::clayReadoutFor (oldProject, {}));

                    expectEquals (juce::StringArray::fromLines (oldTip).size(), 3);
                    expectEquals (tooltipLine (oldTip, 1), utf8 ("未低切 · 未收单 · 未加宽"));
                    expectEquals (tooltipLine (oldTip, 2), utf8 (entry->advice));
                    expect (! utf8Contains (oldTip, "重新聆听"), oldTip);
                }
            }

            // 依据：设计规格-Clay-根治修订 3.5「某项因代价被减量 / 否决时写『本曲试过：{低切 / 收单 / 加宽}会抬高峰值，已自动{减量 / 跳过}』
            // （优先级低于『点重新聆听』提示）」；任务书「跳过 = 该项实际值为 0；多项用『、』连接」。两种都有时分别点名（HelpText.h）。
            // T-043（ADR-027）：低切也会因峰值守护的混合损失被封顶，含低切的那句原因写「会抬高峰值或削弱低音」；不含低切的照旧。
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
                              utf8 ("本曲试过：低切会抬高峰值或削弱低音，已自动跳过。"));
                expectEquals (thirdLineFor (withCostMask (clayStatus (14.0f, 120.0f, 0.4f, 0.6f), lowCutBit)),
                              utf8 ("本曲试过：低切会抬高峰值或削弱低音，已自动减量。"));

                // 多项同一种：按处理顺序（低切 → 收单 → 加宽）用「、」连接。
                expectEquals (thirdLineFor (withCostMask (clayStatus (0.0f, 80.0f, 0.0f, 0.0f), lowCutBit | monoBit | widthBit)),
                              utf8 ("本曲试过：低切、收单、加宽会抬高峰值或削弱低音，已自动跳过。"));
                expectEquals (thirdLineFor (withCostMask (clayStatus (14.0f, 120.0f, 0.3f, 0.5f), widthBit | lowCutBit)),
                              utf8 ("本曲试过：低切、加宽会抬高峰值或削弱低音，已自动减量。"));

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

            // 依据：ADR-009 第 4 条（「关 / 手动」项不经决策器、宽度保护只管自动态）；
            // HelpText.h ClayReadout 代价标志的约定（加宽在宽度保护介入、收单在反相提示亮着时不写代价，旁通时全清零）。
            beginTest ("Clay tooltip line 3 (ADR-012): only automatic items, not while the mono warning is on or the width guard acts, cleared by bypass");
            {
                const auto* entry = help::find (uiid::clayIndicator);
                expect (entry != nullptr);
                const auto advice = entry != nullptr ? utf8 (entry->advice) : juce::String();

                // 旧的 clayNeedsRelearn 不再压过代价说明。
                const auto withOldFlag = withCostMask (clayStatus (0.0f, 80.0f, 0.0f, 0.0f, false, false, true), lowCutBit);
                expectEquals (thirdLineFor (withOldFlag), utf8 ("本曲试过：低切会抬高峰值或削弱低音，已自动跳过。"));

                // 只算「自动」态的项：低切改成「手动」→ 不再写它；还有自动态的加宽被减量 → 只写加宽。
                const auto both = withCostMask (clayStatus (30.0f, 120.0f, 0.4f, 0.5f), lowCutBit | widthBit);
                help::ClayUserState lowCutManual;
                lowCutManual.lowCutAuto = false;
                expectEquals (thirdLineFor (both, lowCutManual), utf8 ("本曲试过：加宽会抬高峰值，已自动减量。"));
                lowCutManual.widthAuto = false;
                expectEquals (thirdLineFor (both, lowCutManual), advice, "no automatic item left: the table's advice");

                // 反相提示亮着时不写收单的代价（旧快照里可能还带着收单的代价位；ADR-015 起决策器不再置它）：第 2 行已写「低频反相」。
                const auto guardedMono = withCostMask (clayStatus (0.0f, 80.0f, 1.0f, 0.0f, true, false), monoBit);
                expectEquals (thirdLineFor (guardedMono), advice, "mono warning on: no cost line for mono");
                expect (utf8Contains (liveLineFor (guardedMono), "低频反相"), "line 2 says why instead");
                auto guardOver = guardedMono;
                guardOver.clayPhaseGuardActive = false;
                guardOver.clayMonoAmount = 0.0f;
                expectEquals (thirdLineFor (guardOver), utf8 ("本曲试过：收单会抬高峰值，已自动跳过。"), "positive control: warning off");

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
            // 守护回收约 95 %、界面却说只让一点）；中英文都要讲到「加宽不经峰值守护」「低切 / 收单在会抬峰值的瞬间让开」。指示灯管自动与手动
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
                              && utf8Contains (utf8 (indicator->effect), "加宽不经峰值守护"),
                            "indicator: the guard covers low cut and mono, width is not run through the peak guard");
                    expect (! utf8Contains (utf8 (indicator->effect), "修坯从不让峰值变高"), "indicator: no longer claims Clay as a whole never raises peaks");
                    expect (juce::String (indicator->effectEn).contains ("Low cut and mono never make peaks higher")
                              && juce::String (indicator->effectEn).contains ("width is not run through the peak guard")
                              && ! juce::String (indicator->effectEn).contains ("Clay never makes peaks higher"),
                            "indicator (English): same scope");
                    expect (utf8Contains (utf8 (indicator->effect), "自动加宽由先试后用把关")
                              && juce::String (indicator->effectEn).contains ("automatic width is kept in check by the trial"),
                            "indicator: only the automatic width goes through the trial (a manual width is what you set)");
                    // T-068（评审 F2）：自动加宽运行中还有宽度保护（盯相关度，真会改声音、会点亮琥珀加宽灯），首句不能只提先试后用。
                    expect (utf8Contains (utf8 (indicator->effect), "运行中还有宽度保护盯着相关度")
                              && juce::String (indicator->effectEn).contains ("while playing, by a width guard that watches correlation"),
                            "indicator: automatic width also has the live width guard");

                    expect (utf8Contains (utf8 (lowCut->advice), "峰值守护会把低切让回去") && ! utf8Contains (utf8 (lowCut->advice), "让一点"),
                            "low cut advice: the guard yields the low cut at peaks it would raise");
                    expect (juce::String (lowCut->adviceEn).contains ("the peak guard yields the low cut")
                              && ! juce::String (lowCut->adviceEn).contains ("yields a little"),
                            "low cut advice (English): same");
                }
            }

            // 依据：ADR-015（自动不收单）与 T-061：灯的底稿第 2 行把三项并列时，收单要写明「按你的需要」，不然读起来像与低切、加宽一样
            // 默认会做；措辞与官网首页（home.json「按你的需要 / when you ask for it」）和说明页首句一致。
            // 阳性对照：对改前的 HelpText.cpp（fbe91f3）这两条都不过（旧句「收单（低频收成单声道）」「mono (lows to mono)」）。
            beginTest ("Clay indicator (ADR-015): mono is described as on request, not as an automatic move");
            {
                const auto* indicator = help::find (uiid::clayIndicator);
                expect (indicator != nullptr, "the Clay indicator entry exists");

                if (indicator != nullptr)
                {
                    expect (utf8Contains (utf8 (indicator->effect), "收单（按你的需要把低频收成单声道）")
                              && ! utf8Contains (utf8 (indicator->effect), "收单（低频收成单声道）"),
                            "indicator: mono only on request");
                    expect (juce::String (indicator->effectEn).contains ("mono (lows to mono when you ask for it)")
                              && ! juce::String (indicator->effectEn).contains ("mono (lows to mono)"),
                            "indicator (English): same");
                }
            }

            // 依据：ADR-015（自动不收单）与 T-066：收单频率这一行在「自动」下不收单、不显示收单频率；原句「这里显示的是此刻的实际值」
            // 读起来像自动也有一个收单频率。官网 clay.md 的收单频率行与它同源。
            // 阳性对照：对改前的 HelpText.cpp（5620af1）这两条都不过。
            beginTest ("Clay mono frequency (ADR-015): Auto does not mono, so no frequency is shown");
            {
                const auto* entry = help::find (param::clayMonoHz);
                expect (entry != nullptr, "the mono frequency entry exists");

                if (entry != nullptr)
                {
                    expect (utf8Contains (utf8 (entry->effect), "（自动不收单，这里不显示收单频率）")
                              && ! utf8Contains (utf8 (entry->effect), "这里显示的是此刻的实际值"),
                            "mono frequency: Auto shows no frequency");
                    expect (juce::String (entry->effectEn).contains ("Auto never folds the lows to mono, so no mono frequency is shown here")
                              && ! juce::String (entry->effectEn).contains ("the display shows the actual value"),
                            "mono frequency (English): same");
                }
            }
        }
    };

    static HelpTextClayTests helpTextClayTests;

    //==============================================================================
    // T-038 曲风与交付（设计规格-曲风与交付 第 2 ～ 5 节、设计规格-曲风-House学习方式 第 7、9 节）：两个参数的文案说的是对的、
    // House 反相提示的文字（一行版给状态行、两行版给 CLAY 卡片）与读数、House 暂定期间 GLAZE 状态的悬停气泡。
    class HelpTextHouseTests  : public juce::UnitTest
    {
    public:
        HelpTextHouseTests() : juce::UnitTest ("HelpTextHouse", "Celadon") {}

        void runTest() override
        {
            beginTest ("genre and delivery entries name the House sub-genres and the delivery targets in both languages");
            {
                const auto* genre = help::find (param::glazeGenre);
                const auto* delivery = help::find (param::glazeDelivery);
                expect (genre != nullptr && delivery != nullptr, "both entries exist");

                if (genre != nullptr && delivery != nullptr)
                {
                    expect (utf8StartsWith (utf8 (genre->title), "曲风") && utf8StartsWith (utf8 (delivery->title), "交付"));
                    expect (juce::String (genre->titleEn).startsWith ("Genre") && juce::String (delivery->titleEn).startsWith ("Delivery"));

                    for (const auto* word : { "Deep House", "Tech House", "Melodic-Progressive", "25 Hz" })
                        expect (utf8Contains (utf8 (genre->effect), word) && utf8Contains (utf8 (genre->effectEn), word),
                                "genre effect names " + juce::String (word));

                    for (const auto* target : { "\xe2\x88\x92" "10 LUFS", "\xe2\x88\x92" "2 dBTP", "\xe2\x88\x92" "7 LUFS" })
                        expect (utf8Contains (utf8 (delivery->effect), target) && utf8Contains (utf8 (delivery->effectEn), target),
                                "delivery effect names " + utf8 (target));

                    expect (utf8Contains (utf8 (delivery->advice), "drop") && juce::String (delivery->adviceEn).contains ("drop"),
                            "delivery advice: play the drop once after switching before exporting");
                }
            }

            beginTest ("House phase hint: the spec sentence in both languages, a two-line card version, no number when the loss is unknown");
            {
                expectEquals (help::housePhaseHintText (16, Language::chinese), utf8 ("俱乐部单声道会丢低频（约 1.6 dB），请回混音修"));
                expectEquals (help::housePhaseHintText (16, Language::english), juce::String ("Club mono loses low end (about 1.6 dB), fix it in the mix"));
                expectEquals (help::housePhaseHintText (0, Language::chinese), utf8 ("俱乐部单声道会丢低频，请回混音修"));
                expectEquals (help::housePhaseHintText (0, Language::english), juce::String ("Club mono loses low end, fix it in the mix"));
                expectEquals (help::housePhaseHintText (231, Language::chinese), utf8 ("俱乐部单声道会丢低频（约 23.1 dB），请回混音修"));

                const auto zh = help::housePhaseHintLines (16, Language::chinese);
                const auto en = help::housePhaseHintLines (16, Language::english);
                expectEquals (zh.size(), 2);
                expectEquals (en.size(), 2);
                expectEquals (zh[0], utf8 ("俱乐部单声道会丢低频（约 1.6 dB）"));
                expectEquals (zh[1], utf8 ("请回混音修"));
                expectEquals (en[0], juce::String ("Club mono loses low end (about 1.6 dB)"));
                expectEquals (en[1], juce::String ("Fix it in the mix"));

                // 损失 → 显示的 0.1 dB 整数：|x| × 10 四舍五入，至少 1、最多 999；非有限 → 0（不写数）。
                expectEquals (help::houseLossTenthsFor (-1.62f), 16);
                expectEquals (help::houseLossTenthsFor (-0.97f), 10);
                expectEquals (help::houseLossTenthsFor (-0.001f), 1);
                expectEquals (help::houseLossTenthsFor (-23.0f), 230);
                expectEquals (help::houseLossTenthsFor (-1.0e6f), 999);
                expectEquals (help::houseLossTenthsFor (std::numeric_limits<float>::quiet_NaN()), 0);
                expectEquals (help::houseLossTenthsFor (-std::numeric_limits<float>::infinity()), 0);
            }

            beginTest ("Clay readout: the House hint shows whatever the Clay modes or bypass (it is about the mix); houseActive false hides it");
            {
                glaze::Status hinted;
                hinted.clayLowCutHz = 25.0f;
                hinted.clayMonoHz = 80.0f;
                hinted.houseActive = true;
                hinted.housePhaseHint = true;
                hinted.housePhaseMonoLossDb = -1.62f;

                help::ClayUserState bypassed, monoOff;
                bypassed.bypassed = true;
                monoOff.monoAuto = false;

                for (const auto& user : { help::ClayUserState {}, bypassed, monoOff })
                {
                    const auto readout = help::clayReadoutFor (hinted, user);
                    expect (readout.housePhaseHint && readout.housePhaseLossTenthsDb == 16, "shown with the loss in tenths");
                }

                auto general = hinted;
                general.houseActive = false;
                expect (! help::clayReadoutFor (general, {}).housePhaseHint, "not House: never shown");

                auto quiet = hinted;
                quiet.housePhaseHint = false;
                const auto plain = help::clayReadoutFor (quiet, {});
                const auto withHint = help::clayReadoutFor (hinted, {});
                expect (! plain.housePhaseHint && plain.housePhaseLossTenthsDb == 0);
                expect (plain != withHint, "the hint is part of the readout (the card repaints when it changes)");
                expect (help::clayLampFor (plain, help::ClayStage::mono) == help::clayLampFor (withHint, help::ClayStage::mono),
                        "the mono lamp keeps its own meaning (the ADR-015 phase warning), the hint has its own box");

                // 气泡第 3 行：说为什么要回混音。旧的 clayNeedsRelearn 不再插到它前面。
                const auto* entry = help::find (uiid::clayIndicator);
                const auto third = juce::StringArray::fromLines (help::clayIndicatorTooltip (withHint))[2];
                const auto thirdEn = juce::StringArray::fromLines (help::clayIndicatorTooltip (withHint, Language::english))[2];
                expect (utf8StartsWith (third, "俱乐部单声道会丢低频（约 1.6 dB）") && utf8Contains (third, "收单救不回单声道回放"),
                        "line 3 explains why to fix the mix: " + third);
                expect (thirdEn.startsWith ("Club mono loses low end (about 1.6 dB)") && thirdEn.contains ("mono-ing cannot help"),
                        "English line 3: " + thirdEn);
                expect (entry != nullptr && juce::StringArray::fromLines (help::clayIndicatorTooltip (plain))[2] == utf8 (entry->advice),
                        "positive control: without the hint line 3 is the table's advice");

                auto oldProject = hinted;
                oldProject.clayNeedsRelearn = true;
                expect (utf8StartsWith (juce::StringArray::fromLines (help::clayIndicatorTooltip (help::clayReadoutFor (oldProject, {})))[2],
                                        "俱乐部单声道会丢低频（约 1.6 dB）"),
                        "the phase hint stays ahead of the retired relearn flag");
            }

            beginTest ("GLAZE status tooltip describes the live line and the one-time old-project note, without relearn or calibration");
            {
                const auto zh = help::tooltipFor (uiid::glazeStatus, Language::chinese);
                const auto en = help::tooltipFor (uiid::glazeStatus, Language::english);
                expect (utf8Contains (zh, "旧的聆听结果已丢弃") && ! utf8Contains (zh, "重新聆听") && ! utf8Contains (zh, "校准"), zh);
                expect (en.contains ("old listening result was discarded") && ! en.containsIgnoreCase ("relearn") && ! en.containsIgnoreCase ("calibrat"), en);

                auto cjk = false;

                for (auto p = en.getCharPointer(); ! p.isEmpty(); ++p)
                    cjk = cjk || (juce::uint32) *p >= 0x3000;

                expect (! cjk, "English tooltip contains CJK: " + en);
            }
        }
    };

    static HelpTextHouseTests helpTextHouseTests;
}
