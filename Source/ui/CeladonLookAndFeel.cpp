/*
  ==============================================================================

    CeladonLookAndFeel.cpp
    v0.1.8.0 浅色外观：字体选择、提示气泡、缩放把手、数值输入框。控件本身（旋钮、滑条、分段选择……）都是自绘组件，
    画法在各自的文件里，这里只管 JUCE 内部控件会来问的那几样。

  ==============================================================================
*/

#include "CeladonLookAndFeel.h"

#include <array>
#include <cmath>
#include <vector>

namespace celadon::ui
{
    namespace
    {
        // ---- 提示气泡 ----
        constexpr float tooltipPadding     = 12.0f;
        constexpr float tooltipMaxWidth    = 360.0f;
        constexpr float tooltipCorner      = 8.0f;
        constexpr float tooltipTitleHeight = 12.5f;
        constexpr float tooltipBodyHeight  = 11.5f;
        constexpr float tooltipLineGap     = 4.0f;       // 行与行之间的额外间距（在字号本身之外）
        constexpr int   tooltipOffsetX     = 16;         // 相对鼠标热点的偏移：避开箭头光标
        constexpr int   tooltipOffsetY     = 20;
        constexpr int   tooltipFlipMargin  = 8;          // 翻到鼠标上方时与热点的距离

        // ---- Label ----
        constexpr float labelFontHeight        = 11.0f;
        constexpr float defaultLabelFontHeight = 15.0f;  // juce::Label 构造时的默认字号（juce_Label.h）

        //==============================================================================
        // 界面字体族（见头文件）。只查一次字体列表，之后用缓存。
        juce::String findInterfaceTypefaceName()
        {
            const auto installed = juce::Font::findAllTypefaceNames();

            for (const auto* candidate : { "PingFang SC", "Microsoft YaHei UI", "Noto Sans CJK SC", "Noto Sans SC", "Source Han Sans SC" })
                if (installed.contains (candidate))
                    return candidate;

            return juce::Font::getSystemUIFontName();
        }

        juce::String findMonoTypefaceName()
        {
            const auto installed = juce::Font::findAllTypefaceNames();

            for (const auto* candidate : { "Menlo", "SF Mono", "Consolas", "DejaVu Sans Mono", "Liberation Mono" })
                if (installed.contains (candidate))
                    return candidate;

            return juce::Font::getDefaultMonospacedFontName();
        }

        // 每种字重按候选样式名依次找，一个都没有就用常规——宁可不粗 / 不细，也不能换字体族。
        // macOS 按样式名取字重，名字必须与 CoreText 列出的一致（JUCE 找不到时会退回列表第 0 项，那可能是 Ultralight）。
        juce::String findStyle (const juce::String& family, std::initializer_list<const char*> candidates)
        {
            const auto styles = juce::Font::findAllTypefaceStyles (family);

            for (const auto* candidate : candidates)
                if (styles.contains (candidate))
                    return candidate;

            return "Regular";
        }

        struct Family
        {
            juce::String name;
            std::array<juce::String, 4> styles;   // 按 Weight 下标
        };

        const Family& interfaceFamily()
        {
            static const Family family = []
            {
                Family f;
                f.name = findInterfaceTypefaceName();
                f.styles[(size_t) Weight::light]    = findStyle (f.name, { "Light", "DemiLight", "Regular" });
                f.styles[(size_t) Weight::regular]  = findStyle (f.name, { "Regular", "Normal", "Book" });
                // 没有 Medium / Semibold 的字体族（Windows 的微软雅黑 UI 只有 Light / Regular / Bold）按常规：500 离 400 近、离 700 远。
                f.styles[(size_t) Weight::medium]   = findStyle (f.name, { "Medium", "Semibold", "DemiBold" });
                f.styles[(size_t) Weight::semibold] = findStyle (f.name, { "Semibold", "DemiBold", "Bold", "Medium" });
                return f;
            }();

            return family;
        }

        const Family& monoFamily()
        {
            static const Family family = []
            {
                Family f;
                f.name = findMonoTypefaceName();
                const auto regular = findStyle (f.name, { "Regular", "Book", "Normal" });
                f.styles[(size_t) Weight::light]    = regular;
                f.styles[(size_t) Weight::regular]  = regular;
                f.styles[(size_t) Weight::medium]   = findStyle (f.name, { "Medium", "Regular", "Book" });
                f.styles[(size_t) Weight::semibold] = findStyle (f.name, { "Semibold", "Bold", "Medium", "Regular" });
                return f;
            }();

            return family;
        }

        juce::Font makeFont (const Family& family, float height, Weight weight, std::vector<juce::String> fallbacks = {})
        {
            // 不能用不带名字的 FontOptions (height)——那是「默认无衬线」占位符，macOS 上落到 Lucida Grande。
            // FontOptions 默认 fallbackEnabled = true：字体里没有的字先按 fallbacks 找，再回退到系统字体。
            return juce::Font (juce::FontOptions (family.name, family.styles[(size_t) weight], juce::jmax (1.0f, height))
                                   .withFallbacks (std::move (fallbacks)));
        }

        //==============================================================================
        // 气泡文字：按 '\n' 分行，每行单独排版（第 1 行中粗，其余常规），行间加 tooltipLineGap。
        // 行内超过最大宽度就自动换行：JUCE 的 TextLayout 按 Unicode 换行规则找断点，中文逐字、英文按词。
        struct TooltipLayout
        {
            std::vector<juce::TextLayout> rows;
            std::vector<float> rowHeights;
            float width  = 0.0f;
            float height = 0.0f;
        };

        TooltipLayout layoutTooltip (const juce::String& text)
        {
            TooltipLayout result;
            const auto maxTextWidth = tooltipMaxWidth - tooltipPadding * 2.0f;
            const auto lines = juce::StringArray::fromLines (text);

            for (int i = 0; i < lines.size(); ++i)
            {
                const auto font = i == 0 ? uiFont (tooltipTitleHeight, Weight::medium) : uiFont (tooltipBodyHeight);

                juce::AttributedString attributed;
                attributed.setJustification (juce::Justification::topLeft);
                attributed.setWordWrap (juce::AttributedString::byWord);
                attributed.setLineSpacing (2.0f);
                attributed.append (lines[i], font, i == 0 ? palette::ink : palette::ink2);

                juce::TextLayout layout;
                layout.createLayout (attributed, maxTextWidth);

                // 空行也占一行的高度，气泡不会因为某行没字而塌掉。
                const auto rowHeight = juce::jmax (layout.getHeight(), font.getHeight());

                result.width  = juce::jmax (result.width, layout.getWidth());
                result.height += rowHeight + (i > 0 ? tooltipLineGap : 0.0f);
                result.rowHeights.push_back (rowHeight);
                result.rows.push_back (std::move (layout));
            }

            return result;
        }
    }

    //==============================================================================
    juce::Colour cyanAt (float x) noexcept
    {
        // 与设计稿同一组色标：0 → #A3E0E8，0.35 → #33B3C4，0.7 → #127F90，1 → #0A4A57。
        struct Stop { float at; juce::uint8 r, g, b; };
        static constexpr Stop stops[] = { { 0.0f, 163, 224, 232 }, { 0.35f, 51, 179, 196 }, { 0.7f, 18, 127, 144 }, { 1.0f, 10, 74, 87 } };

        x = std::isfinite (x) ? juce::jlimit (0.0f, 1.0f, x) : 0.0f;

        size_t i = 0;

        while (i < std::size (stops) - 2 && x > stops[i + 1].at)
            ++i;

        const auto& a = stops[i];
        const auto& b = stops[i + 1];
        const auto u = (x - a.at) / (b.at - a.at);
        const auto mix = [u] (juce::uint8 from, juce::uint8 to) noexcept
        {
            return (juce::uint8) juce::roundToInt ((float) from + ((float) to - (float) from) * u);
        };

        return juce::Colour (mix (a.r, b.r), mix (a.g, b.g), mix (a.b, b.b));
    }

    juce::Font uiFont (float height, Weight weight)
    {
        return makeFont (interfaceFamily(), height, weight);
    }

    juce::Font monoFont (float height, Weight weight)
    {
       #if JUCE_WINDOWS
        // Consolas 没有中文：中文先回退到界面字体族（微软雅黑 UI，Consolas 用到的 Regular / Bold 它都有），与界面其余中文同一款字。
        // 不指定的话 Windows 按系统显示语言选回退字体（日文系统会拿日文字形写中文）。
        // 只在 Windows 上指定：JUCE 的首选回退只换字体族、不换字重名，macOS 上 Menlo 的「Bold」苹方里没有，会落到苹方的第一种
        // 字重再加假粗体——那边 CoreText 按字重配的级联回退更好。
        return makeFont (monoFamily(), height, weight, { interfaceFamily().name });
       #else
        return makeFont (monoFamily(), height, weight);
       #endif
    }

    juce::Font celadonFont (float height, bool medium)
    {
        return uiFont (height, medium ? Weight::medium : Weight::regular);
    }

    float textWidth (const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (font, text);
    }

    juce::Font withLetterSpacing (const juce::Font& font, float pixels)
    {
        const auto height = font.getHeight();
        return height > 0.0f ? font.withExtraKerningFactor (pixels / height) : font;
    }

    //==============================================================================
    CeladonLookAndFeel::CeladonLookAndFeel()
        : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (palette::background,    // windowBackground
                                                                     palette::panel,         // widgetBackground
                                                                     palette::panel,         // menuBackground
                                                                     palette::controlBorder, // outline
                                                                     palette::ink,           // defaultText
                                                                     palette::c500,          // defaultFill
                                                                     palette::panel,         // highlightedText
                                                                     palette::c600,          // highlightedFill
                                                                     palette::ink))          // menuText
    {
        const auto transparent = juce::Colours::transparentBlack;

        setColour (juce::ResizableWindow::backgroundColourId, palette::background);
        setColour (juce::DocumentWindow::textColourId, palette::ink);

        setColour (juce::Label::backgroundColourId, transparent);
        setColour (juce::Label::textColourId, palette::ink);
        setColour (juce::Label::outlineColourId, transparent);

        // 直接输入数值的小文本框（Alt 单击旋钮 / 滑条 / 数值时出现）。
        setColour (juce::TextEditor::backgroundColourId, palette::panel);
        setColour (juce::TextEditor::textColourId, palette::ink);
        setColour (juce::TextEditor::highlightColourId, palette::c200.withAlpha (0.6f));
        setColour (juce::TextEditor::highlightedTextColourId, palette::ink);
        setColour (juce::TextEditor::outlineColourId, palette::c400);
        setColour (juce::TextEditor::focusedOutlineColourId, palette::c400);
        setColour (juce::CaretComponent::caretColourId, palette::c600);

        // 提示气泡
        setColour (juce::TooltipWindow::backgroundColourId, palette::panel);
        setColour (juce::TooltipWindow::textColourId, palette::ink);
        setColour (juce::TooltipWindow::outlineColourId, palette::controlBorder);

        // 弹出菜单（宿主右键菜单之类由 JUCE 画时用得到）
        setColour (juce::PopupMenu::backgroundColourId, palette::panel);
        setColour (juce::PopupMenu::textColourId, palette::ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::c100);
        setColour (juce::PopupMenu::highlightedTextColourId, palette::ink);

        // 「默认无衬线」占位符换成界面字体族：只对经过默认 LookAndFeel 解析、又没指定名字的字体生效（JUCE 内部控件之类）；
        // 自己的文字一律走 uiFont / monoFont。
        setDefaultSansSerifTypefaceName (interfaceFamily().name);
    }

    CeladonLookAndFeel::~CeladonLookAndFeel() = default;

    //==============================================================================
    juce::Rectangle<int> CeladonLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                               juce::Rectangle<int> parentArea)
    {
        // screenPos 与 parentArea 在同一坐标系里：有父组件时是父组件坐标，否则是屏幕坐标（TooltipWindow 负责换算）。
        const auto layout = layoutTooltip (tipText);
        const auto w = (int) std::ceil (layout.width  + tooltipPadding * 2.0f);
        const auto h = (int) std::ceil (layout.height + tooltipPadding * 2.0f);

        // 首选鼠标右下方；下方放不下就翻到上方，免得光标压住文字；最后无论如何不超出父区域。
        const auto x = screenPos.x + tooltipOffsetX;
        auto y = screenPos.y + tooltipOffsetY;

        if (y + h > parentArea.getBottom())
            y = screenPos.y - h - tooltipFlipMargin;

        return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
    }

    void CeladonLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
    {
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);

        // 编辑器把 TooltipWindow 设成了透明（setOpaque (false)），圆角外的四角由父组件画，这里不填底色。
        g.setColour (palette::panel);
        g.fillRoundedRectangle (bounds, tooltipCorner);

        g.setColour (palette::controlBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), tooltipCorner, 1.0f);

        const auto layout = layoutTooltip (text);
        auto rowY = tooltipPadding;

        for (size_t i = 0; i < layout.rows.size(); ++i)
        {
            layout.rows[i].draw (g, juce::Rectangle<float> (tooltipPadding, rowY, layout.width, layout.rowHeights[i]));
            rowY += layout.rowHeights[i] + tooltipLineGap;
        }
    }

    //==============================================================================
    juce::Font CeladonLookAndFeel::getLabelFont (juce::Label& label)
    {
        // 没显式设过字体的 Label（还是 juce::Label 构造时的默认：15 px、默认无衬线占位名）统一成 11 px 界面字体；
        // 调用方 setFont 过别的就原样尊重。
        const auto requested = label.getFont();

        const bool isJuceDefault = juce::approximatelyEqual (requested.getHeight(), defaultLabelFontHeight)
                                    && requested.getTypefaceName() == juce::Font::getDefaultSansSerifFontName();

        return isJuceDefault ? uiFont (labelFontHeight) : requested;
    }

    juce::Font CeladonLookAndFeel::getPopupMenuFont()
    {
        return uiFont (13.0f);
    }

    //==============================================================================
    void CeladonLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        // 设计稿：20 × 20 里三道斜线（M17 8 L8 17、M17 12 L12 17、M17 16 L16 17），1.4 px 圆头，灰；悬停 / 拖动换青色。
        const auto scale = (float) juce::jmin (w, h) / 20.0f;
        g.setColour ((isMouseOver || isMouseDragging) ? palette::c600 : palette::ink4);

        const juce::PathStrokeType stroke (1.4f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        for (const auto start : { 8.0f, 12.0f, 16.0f })
        {
            juce::Path line;
            line.startNewSubPath ((float) w - 3.0f * scale, (float) h - (20.0f - start) * scale);
            line.lineTo ((float) w - (20.0f - start) * scale, (float) h - 3.0f * scale);
            g.strokePath (line, stroke);
        }
    }

    //==============================================================================
    void CeladonLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
    {
        g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height), 5.0f);
    }

    void CeladonLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
    {
        g.setColour (editor.findColour (juce::TextEditor::focusedOutlineColourId));
        g.drawRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height).reduced (0.75f), 5.0f, 1.5f);
    }
}
