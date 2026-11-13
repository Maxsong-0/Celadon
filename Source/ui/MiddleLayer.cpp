/*
  ==============================================================================

    MiddleLayer.cpp

  ==============================================================================
*/

#include "MiddleLayer.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        // 带符号的数字（负号用 U+2212）；0 附近不写正负号。
        juce::String signedText (float value, int decimals, bool plusSign = false)
        {
            const auto threshold = 0.5f * std::pow (10.0f, (float) -decimals);
            const auto magnitude = juce::String (std::abs (value), decimals);

            if (value <= -threshold)
                return unicodeMinus() + magnitude;

            return (plusSign && value >= threshold ? "+" : "") + magnitude;
        }

        // 画一个小的「重新计量」图标（顺时针的圆箭头）。T-107 起从频谱仪的响度小窗搬到右侧板。
        void drawResetIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
        {
            const auto centre = area.getCentre();
            const auto radius = area.getWidth() * 0.36f;
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::degreesToRadians (40.0f), juce::degreesToRadians (330.0f), true);
            g.setColour (colour);
            g.strokePath (arc, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const auto tip = knob::pointAt (centre, radius, juce::degreesToRadians (40.0f));
            juce::Path head;
            head.addTriangle (tip.translated (-2.6f, -1.2f), tip.translated (1.6f, -2.2f), tip.translated (0.6f, 2.0f));
            g.fillPath (head);
        }

        // 侧板的白底（与原来的响度小窗同款：白 0.86、1 px 边、圆角 12、无投影）。
        void drawSidePanelBox (juce::Graphics& g, float width)
        {
            const juce::Rectangle<float> box (0.0f, (float) sidepanel::boxTop, width, (float) (sidepanel::height - sidepanel::boxTop));
            g.setColour (juce::Colours::white.withAlpha (0.86f));
            g.fillRoundedRectangle (box, 12.0f);
            g.setColour (palette::panelBorder);
            g.drawRoundedRectangle (box.reduced (0.5f), 11.5f, 1.0f);
        }

        // 单行文字放进 width：先原样，放不下就把横向压到最多 0.86（等宽数字仍对齐），再不行才截断。
        void drawFittedLine (juce::Graphics& g, juce::Font font, const juce::String& text, juce::Rectangle<float> area,
                             juce::Justification justification)
        {
            const auto natural = textWidth (font, text);

            if (natural > area.getWidth() && natural > 0.0f)
                font = font.withHorizontalScale (juce::jmax (0.86f, area.getWidth() / natural));

            g.setFont (font);
            g.drawText (text, area, justification, true);
        }

        // 侧板文字里的紫色（T-105 3-10：#6A4DFF 当文字在白底上对比度不够，文字单独用 #5438D9）。
        const juce::Colour violetText { 0xff5438d9 };
    }

    //==============================================================================
    GlazeTitle::GlazeTitle()
    {
        setInterceptsMouseClicks (true, false);
    }

    void GlazeTitle::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        repaint();
    }

    void GlazeTitle::setChip (Chip newChip)
    {
        if (chip == newChip)
            return;

        chip = newChip;
        repaint();
    }

    void GlazeTitle::paint (juce::Graphics& g)
    {
        const auto h = (float) getHeight();
        const auto titleFont = withLetterSpacing (uiFont (12.0f, Weight::semibold), 3.0f);
        const auto subtitleFont = uiFont (12.0f);
        const auto chipFont = uiFont (11.0f, Weight::medium);
        const juce::String title ("GLAZE");
        const auto subtitle = tr (Text::glazeSubtitle, language);

        // 阶段标签：文字与颜色。琥珀（要动手）用警告色；起步用青；稳态用浅青；GLAZE 0 用灰。
        juce::String chipLabel;
        auto chipBackground = palette::c100, chipForeground = palette::c700;

        switch (chip)
        {
            case Chip::manual:      chipLabel = tr (Text::chipManual, language);      chipBackground = palette::groove; chipForeground = palette::ink3; break;
            case Chip::manualLimit: chipLabel = tr (Text::chipManualLimit, language); chipBackground = palette::warningSoft; chipForeground = palette::warningText; break;
            case Chip::coldExport:  chipLabel = tr (Text::chipExport, language);      chipBackground = palette::warningSoft; chipForeground = palette::warningText; break;
            case Chip::limited:     chipLabel = tr (Text::chipLimited, language);     chipBackground = palette::warningSoft; chipForeground = palette::warningText; break;
            case Chip::starting:    chipLabel = tr (Text::chipStarting, language);    break;
            case Chip::holding:     chipLabel = tr (Text::chipHolding, language);     break;
            case Chip::following:   chipLabel = tr (Text::chipFollowing, language);   break;
        }

        const auto chipWidth = chipLabel.isEmpty() ? 0.0f : 7.0f + textWidth (chipFont, chipLabel) + 7.0f;
        const auto titleWidth = textWidth (titleFont, title);
        const auto subtitleWidth = textWidth (subtitleFont, subtitle);
        const auto total = titleWidth + 9.0f + subtitleWidth + (chipWidth > 0.0f ? 9.0f + chipWidth : 0.0f);
        auto x = ((float) getWidth() - total) * 0.5f;

        g.setFont (titleFont);
        g.setColour (palette::ink);
        g.drawText (title, juce::Rectangle<float> (x, 0.0f, titleWidth + 4.0f, h), juce::Justification::centredLeft, false);
        x += titleWidth + 9.0f;

        g.setFont (subtitleFont);
        g.setColour (palette::ink2);
        g.drawText (subtitle, juce::Rectangle<float> (x, 0.0f, subtitleWidth + 2.0f, h), juce::Justification::centredLeft, false);
        x += subtitleWidth + 9.0f;

        if (chipWidth <= 0.0f)
            return;

        const juce::Rectangle<float> chipArea (x, (h - 19.0f) * 0.5f, chipWidth, 19.0f);
        g.setColour (chipBackground);
        g.fillRoundedRectangle (chipArea, 5.0f);

        const auto chipX = chipArea.getX() + 7.0f;

        g.setFont (chipFont);
        g.setColour (chipForeground);
        g.drawText (chipLabel, juce::Rectangle<float> (chipX, chipArea.getY(), chipWidth, chipArea.getHeight()), juce::Justification::centredLeft, false);
    }

    //==============================================================================
    PillButton::PillButton (Icon i)
        : icon (i)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool PillButton::setText (const juce::String& newText)
    {
        if (text == newText)
            return false;

        text = newText;
        repaint();
        return true;
    }

    void PillButton::setLook (Look newLook)
    {
        if (look == newLook)
            return;

        look = newLook;
        repaint();
    }

    int PillButton::getNaturalWidth() const
    {
        const auto font = uiFont (icon == Icon::relearn ? 12.0f : 11.5f, Weight::medium);
        return (int) std::ceil (12.0f + 12.0f + 6.0f + textWidth (font, text) + 12.0f);
    }

    void PillButton::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        juce::Colour background, border, foreground;

        switch (look)
        {
            case Look::normal:    background = palette::panel;               border = palette::relearnBorder;   foreground = palette::c700;        break;
            case Look::off:       background = juce::Colour (0xfff3f6f7);    border = palette::controlBorder;   foreground = palette::ink3;        break;
            case Look::attention: background = palette::warningSoft;         border = palette::warning;         foreground = palette::warningText; break;
        }

        if (isHovering())
            background = background.interpolatedWith (border, 0.12f);

        g.setColour (background);
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (border);
        g.drawRoundedRectangle (bounds, 8.0f, 1.0f);

        const auto font = uiFont (icon == Icon::relearn ? 12.0f : 11.5f, Weight::medium);
        const auto contentWidth = 12.0f + 6.0f + textWidth (font, text);
        auto x = bounds.getCentreX() - contentWidth * 0.5f;
        const auto centreY = bounds.getCentreY();

        g.setColour (foreground);

        if (icon == Icon::relearn)
        {
            // 圆箭头（设计稿 11 × 11）
            const juce::Point<float> centre (x + 5.5f, centreY);
            juce::Path arrow;
            arrow.addCentredArc (centre.x, centre.y, 3.6f, 3.6f, 0.0f, juce::degreesToRadians (90.0f), juce::degreesToRadians (395.0f), true);
            g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path head;
            head.startNewSubPath (centre.x + 3.8f, centre.y - 4.0f);
            head.lineTo (centre.x + 3.8f, centre.y - 1.6f);
            head.lineTo (centre.x + 1.4f, centre.y - 1.6f);
            g.strokePath (head, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else
        {
            // 等号：两道横线（设计稿 12 × 10）
            g.drawLine (x, centreY - 1.5f, x + 10.0f, centreY - 1.5f, 1.4f);
            g.drawLine (x, centreY + 1.5f, x + 10.0f, centreY + 1.5f, 1.4f);
        }

        x += 12.0f + 6.0f;
        g.setFont (font);
        g.drawText (text, juce::Rectangle<float> (x, 0.0f, textWidth (font, text) + 2.0f, (float) getHeight()), juce::Justification::centredLeft, false);
    }

    //==============================================================================
    CornerNote::CornerNote()
    {
        setInterceptsMouseClicks (false, false);
    }

    void CornerNote::setLines (const juce::String& first, const juce::String& second, bool isHint, bool isWarning)
    {
        if (first == line1 && second == line2 && isHint == hint && isWarning == warning)
            return;

        line1 = first;
        line2 = second;
        hint = isHint;
        warning = isWarning;
        repaint();
    }

    void CornerNote::paint (juce::Graphics& g)
    {
        // T-107：底排高 30，两行各 15。
        const auto lineHeight = (float) getHeight() * 0.5f;
        g.setFont (uiFont (11.0f));
        g.setColour (hint ? palette::c700 : (warning ? palette::warningText : palette::ink3));   // 两条旧工程提示同时在时第一行也是警告
        g.drawText (line1, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), lineHeight), juce::Justification::centredLeft, false);
        g.setColour (hint ? palette::c700 : (warning ? palette::warningText : palette::ink3));
        g.drawText (line2, juce::Rectangle<float> (0.0f, lineHeight, (float) getWidth(), lineHeight), juce::Justification::centredLeft, false);
    }

    //==============================================================================
    ColourLegend::ColourLegend()
    {
        setInterceptsMouseClicks (false, false);
    }

    void ColourLegend::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        repaint();
    }

    int ColourLegend::getNaturalWidth() const
    {
        const auto font = uiFont (11.0f);
        auto total = 0.0f;

        for (const auto key : { Text::legendManual, Text::legendAuto, Text::legendAttention })
            total += 12.0f + 5.0f + textWidth (font, tr (key, language)) + 14.0f;

        return (int) std::ceil (total - 14.0f);
    }

    void ColourLegend::paint (juce::Graphics& g)
    {
        const auto font = uiFont (11.0f);
        const auto centreY = (float) getHeight() * 0.5f;
        auto x = (float) getWidth() - (float) getNaturalWidth();

        const auto item = [&] (Text key, int kind)
        {
            const juce::Rectangle<float> swatch (x, centreY - 1.5f, 12.0f, 3.0f);

            if (kind == 0)
                g.setGradientFill (juce::ColourGradient (palette::c200, swatch.getX(), 0.0f, palette::c800, swatch.getRight(), 0.0f, false));
            else
                g.setColour (kind == 1 ? palette::violet : palette::warning);

            g.fillRoundedRectangle (swatch, 1.5f);
            x += 12.0f + 5.0f;

            const auto text = tr (key, language);
            g.setFont (font);
            g.setColour (palette::ink2);
            g.drawText (text, juce::Rectangle<float> (x, 0.0f, textWidth (font, text) + 2.0f, (float) getHeight()), juce::Justification::centredLeft, false);
            x += textWidth (font, text) + 14.0f;
        };

        item (Text::legendManual, 0);
        item (Text::legendAuto, 1);
        item (Text::legendAttention, 2);
    }

    //==============================================================================
    bool GlazeNowPanel::Row::operator== (const Row& other) const noexcept
    {
        return juce::exactlyEqual (bar, other.bar) && badge == other.badge && text == other.text
            && automatic == other.automatic && bypassed == other.bypassed;
    }

    bool GlazeNowPanel::Values::operator== (const Values& other) const noexcept
    {
        return rows == other.rows && statusLine == other.statusLine && statusWarning == other.statusWarning && details == other.details;
    }

    GlazeNowPanel::GlazeNowPanel()
    {
        setInterceptsMouseClicks (true, false);
    }

    void GlazeNowPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        repaint();
    }

    void GlazeNowPanel::setValues (const Values& newValues)
    {
        if (newValues == values)
            return;

        values = newValues;
        repaint();
    }

    void GlazeNowPanel::paint (juce::Graphics& g)
    {
        using namespace sidepanel;
        const auto width = (float) getWidth();
        const auto left = (float) padX, right = width - (float) padX;

        // 表头：画在标题行里、不带底。
        g.setColour (palette::ink2);
        drawFittedLine (g, uiFont (11.0f, Weight::medium), tr (Text::sideNowTitle, language),
                        { left, 0.0f, right - left, (float) headerHeight }, juce::Justification::centredLeft);

        drawSidePanelBox (g, width);

        // 四行模块：第 i 行在面板内 y = boxTop + 10 + 25 i（画布 344 + 25 i），高 22。
        static const char* const numbers[] = { "01", "02", "03", "04" };
        static const char* const names[] = { "BALANCE", "CLAY", "COLOR", "LIMIT" };
        const auto numberFont = monoFont (10.0f);
        const auto nameFont = uiFont (11.0f, Weight::semibold);
        const auto valueFont = monoFont (11.0f);
        const auto barX = left + 80.0f, barWidth = 96.0f, textX = left + 188.0f;

        for (size_t i = 0; i < values.rows.size(); ++i)
        {
            const auto& row = values.rows[i];
            const auto y = (float) boxTop + 10.0f + 25.0f * (float) i;
            const auto centreY = y + 11.0f;

            if (row.bypassed)
                g.beginTransparencyLayer (0.35f);

            g.setFont (numberFont);
            g.setColour (palette::c600);
            g.drawText (numbers[i], juce::Rectangle<float> (left, y, 18.0f, 22.0f), juce::Justification::centredLeft, false);
            g.setFont (nameFont);
            g.setColour (palette::ink);
            g.drawText (names[i], juce::Rectangle<float> (left + 18.0f, y, barX - left - 22.0f, 22.0f), juce::Justification::centredLeft, true);

            if (row.bar >= 0.0f)
            {
                const juce::Rectangle<float> track (barX, centreY - 3.0f, barWidth, 6.0f);
                g.setColour (palette::track);
                g.fillRoundedRectangle (track, 3.0f);

                if (row.bar > 0.0f)
                {
                    g.setColour (palette::violet);
                    g.fillRoundedRectangle (track.withWidth (juce::jmax (6.0f, barWidth * juce::jmin (1.0f, row.bar))), 3.0f);
                }
            }
            else if (row.badge.isNotEmpty())
            {
                g.setFont (uiFont (10.5f));
                g.setColour (palette::ink3);
                g.drawText (row.badge, juce::Rectangle<float> (barX, y, barWidth, 22.0f), juce::Justification::centredLeft, true);
            }

            const auto hasText = row.text.isNotEmpty();
            g.setColour (hasText ? (row.automatic ? violetText : palette::ink2) : palette::ink3);
            drawFittedLine (g, valueFont, hasText ? row.text : juce::String::charToString ((juce::juce_wchar) 0x2014),
                            { textX, y, right - textX, 22.0f }, juce::Justification::centredLeft);

            if (row.bypassed)
                g.endTransparencyLayer();
        }

        // 分隔线（画布 y 448）与状态区（画布 y 454 ～ 506）：不截断的状态句（最多两行）+ 把握 / 主段 / 保护。
        g.setColour (palette::divider);
        g.fillRect (left, (float) boxTop + 114.0f, right - left, 1.0f);

        if (values.statusLine.isNotEmpty())
        {
            juce::AttributedString status;
            status.setWordWrap (juce::AttributedString::WordWrap::byChar);
            status.append (values.statusLine, uiFont (11.5f), values.statusWarning ? palette::warningText : palette::ink);
            juce::TextLayout layout;
            layout.createLayout (status, right - left);
            layout.draw (g, { left, (float) boxTop + 121.0f, right - left, 34.0f });
        }

        if (values.details.isNotEmpty())
        {
            g.setColour (palette::ink3);
            drawFittedLine (g, monoFont (10.5f), values.details, { left, (float) boxTop + 158.0f, right - left, 14.0f },
                            juce::Justification::centredLeft);
        }
    }

    //==============================================================================
    DeliveryPanel::DeliveryPanel()
    {
        setInterceptsMouseClicks (true, false);
    }

    void DeliveryPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        shownKey.clear();
        repaint();
    }

    void DeliveryPanel::setValues (const Values& newValues)
    {
        values = newValues;

        // 只有显示出来的文字（一位小数）或标记位置变了才重画。
        juce::String key;
        key << juce::String (values.integratedLufs, 1) << '|' << juce::String (values.shortTermLufs, 1) << '|'
            << juce::String (values.truePeakMaxDb, 1) << '|' << juce::String (values.ceilingDb, 1) << '|'
            << (values.hasTarget ? juce::String (values.targetLufs, 1) : juce::String ("-"));

        if (key == shownKey)
            return;

        shownKey = key;
        repaint();
    }

    void DeliveryPanel::pushHistory (const HistoryPoint& point)
    {
        history[(size_t) historyHead] = point;
        historyHead = (historyHead + 1) % historyLength;
        historyCount = juce::jmin (historyCount + 1, historyLength);
        repaint (contentArea().withTrimmedTop (76.0f).getSmallestIntegerContainer());
    }

    void DeliveryPanel::clearHistory()
    {
        historyHead = 0;
        historyCount = 0;
        repaint();
    }

    juce::Rectangle<float> DeliveryPanel::contentArea() const
    {
        // 面板内容区：左右各 12，上从面板上沿下 10（画布 y 344）、高 162（到画布 y 506）。
        return { (float) sidepanel::padX, (float) sidepanel::boxTop + 10.0f, (float) getWidth() - 2.0f * (float) sidepanel::padX, 162.0f };
    }

    juce::Rectangle<float> DeliveryPanel::resetIconBounds() const
    {
        // 面板内容区右上角（峰响比那一列标签的右边；标签很短，碰不到）。
        const auto content = contentArea();
        return { content.getRight() - 14.0f, content.getY() + 2.0f, 14.0f, 14.0f };
    }

    void DeliveryPanel::paint (juce::Graphics& g)
    {
        using namespace sidepanel;
        const auto width = (float) getWidth();
        const auto content = contentArea();
        const auto left = content.getX(), right = content.getRight(), top = content.getY();
        const auto dash = juce::String::charToString ((juce::juce_wchar) 0x2014);
        const auto lufsText = [&] (float lufs) { return hasReading (lufs) ? signedText (lufs, 1) : dash; };

        // 表头：「目标 −17.4 LUFS」右对齐。目标用深色（紫留给自动加量）。
        {
            const auto text = values.hasTarget ? trFormat (Text::sideTarget, language, { signedText (values.targetLufs, 1) })
                                               : trFormat (Text::sideTarget, language, { dash });
            g.setColour (values.hasTarget ? palette::ink : palette::ink3);
            drawFittedLine (g, monoFont (11.0f, Weight::medium), text,
                            { left, 0.0f, right - left, (float) headerHeight },
                            juce::Justification::centredRight);
        }

        drawSidePanelBox (g, width);

        // ① 第一排（画布 y 344 ～ 384）：整体响度大字 + LUFS-I + 重新计量；右边三列各 80 px：短期、真峰值、峰响比。
        {
            const auto big = withLetterSpacing (uiFont (30.0f, Weight::light), -1.0f);
            const auto text = lufsText (values.integratedLufs);
            const auto baseline = juce::roundToInt (top + 30.0f);

            g.setColour (palette::ink);
            g.setFont (big);
            g.drawSingleLineText (text, juce::roundToInt (left), baseline);

            g.setFont (monoFont (10.0f));
            g.setColour (palette::ink3);
            g.drawSingleLineText ("LUFS-I", juce::roundToInt (left + textWidth (big, text) + 5.0f), baseline);
            drawResetIcon (g, resetIconBounds(), resetHovered ? palette::c600 : palette::ink3);

            const auto over = values.truePeakMaxDb > values.ceilingDb + 0.05f;
            const auto plr = hasReading (values.integratedLufs) && values.truePeakMaxDb > -99.0f
                                 ? juce::String (values.truePeakMaxDb - values.integratedLufs, 1) : dash;
            const struct { Text label; juce::String value; juce::Colour colour; } columns[] =
            {
                { Text::sideShortTerm, lufsText (values.shortTermLufs), palette::ink },
                { Text::sideTruePeak, values.truePeakMaxDb > -99.0f ? signedText (values.truePeakMaxDb, 1, true) : dash,
                  over ? palette::warningText : palette::ink },
                { Text::peakToLoudness, plr, palette::ink },
            };

            auto x = right - 240.0f;

            for (const auto& column : columns)
            {
                g.setColour (palette::ink3);
                drawFittedLine (g, uiFont (10.5f), tr (column.label, language), { x, top + 2.0f, 76.0f, 14.0f }, juce::Justification::centredLeft);
                g.setColour (column.colour);
                g.setFont (monoFont (13.0f, Weight::medium));
                g.drawText (column.value, juce::Rectangle<float> (x, top + 18.0f, 78.0f, 18.0f), juce::Justification::centredLeft, false);
                x += 80.0f;
            }
        }

        // ② 响度条（画布 y 396 ～ 402）：−24 ～ −6 LUFS。短期是一根深色竖线；目标三角在条上方、−14 虚线在条下方（两者不再叠在一起）；
        //    条下最左写归一量。
        {
            constexpr float lowLufs = -24.0f, highLufs = -6.0f;
            const auto barTop = top + 52.0f, barWidth = right - left;
            const auto xOfLufs = [&] (float lufs) { return left + juce::jlimit (0.0f, 1.0f, (lufs - lowLufs) / (highLufs - lowLufs)) * barWidth; };

            g.setColour (palette::track);
            g.fillRoundedRectangle (left, barTop, barWidth, 6.0f, 3.0f);

            if (hasReading (values.integratedLufs))
            {
                g.setGradientFill (juce::ColourGradient (palette::c200, left, 0.0f, palette::c700, right, 0.0f, false));
                g.fillRoundedRectangle (left, barTop, juce::jmax (6.0f, xOfLufs (values.integratedLufs) - left), 6.0f, 3.0f);
            }

            if (hasReading (values.shortTermLufs))
            {
                g.setColour (palette::ink);
                g.fillRect (xOfLufs (values.shortTermLufs) - 1.0f, barTop - 2.0f, 2.0f, 10.0f);
            }

            if (values.hasTarget)
            {
                const auto x = xOfLufs (values.targetLufs);
                juce::Path marker;
                marker.addTriangle (x - 4.0f, barTop - 9.0f, x + 4.0f, barTop - 9.0f, x, barTop - 3.0f);
                g.setColour (palette::ink);
                g.fillPath (marker);
            }

            const auto stream = xOfLufs (-14.0f);
            const float dashes[] = { 1.5f, 1.0f };
            g.setColour (palette::c800);
            g.drawDashedLine (juce::Line<float> (stream, barTop + 7.0f, stream, barTop + 13.0f), dashes, 2, 1.2f);
            g.setFont (monoFont (9.5f));
            g.setColour (palette::ink3);
            g.drawText (unicodeMinus() + "14", juce::Rectangle<float> (stream + 3.0f, barTop + 7.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);

            const auto mono = monoFont (10.5f);
            const auto normLabel = tr (Text::normalisation, language) + " ";
            const auto norm = hasReading (values.integratedLufs) ? signedText (-14.0f - values.integratedLufs, 1, true) : dash;
            g.setFont (mono);
            g.setColour (palette::ink3);
            g.drawText (normLabel, juce::Rectangle<float> (left, barTop + 8.0f, 80.0f, 13.0f), juce::Justification::centredLeft, false);
            g.setColour (palette::c800);
            g.drawText (norm, juce::Rectangle<float> (left + textWidth (mono, normLabel), barTop + 8.0f, 60.0f, 13.0f), juce::Justification::centredLeft, false);
        }

        // ③ 最近 60 秒（画布 y 420 ～ 506）：响度道（画布 y 420 ～ 478，−30 ～ −6 LUFS）画 OUT 短期响度与目标虚线；增益道（画布 y 482 ～ 502，
        //    0 ～ +12 dB）画 GLAZE 自动增益（紫），守护退让的那一刻在增益道画 2 px 橙线。最新的点在右缘。
        //    （T-105 6.2 写的是响度道 64 px、增益道 14 px：+3 dB 的增益只有 3 px 高、看不出来，效果图里改成 58 / 20。）
        {
            const juce::Rectangle<float> lane (left, top + 76.0f, right - left, 58.0f);
            const juce::Rectangle<float> gainLane (left, top + 138.0f, right - left, 20.0f);
            constexpr float laneLow = -30.0f, laneHigh = -6.0f, gainHigh = 12.0f;
            const auto yOfLufs = [&] (float lufs) { return lane.getBottom() - juce::jlimit (0.0f, 1.0f, (lufs - laneLow) / (laneHigh - laneLow)) * lane.getHeight(); };
            const auto step = lane.getWidth() / (float) (historyLength - 1);

            g.setColour (palette::spectrumFloor);
            g.fillRoundedRectangle (lane, 4.0f);
            g.setColour (palette::violetSoft.withAlpha (0.7f));
            g.fillRoundedRectangle (gainLane, 3.0f);

            // 增益道的说明垫在填充下面（左边；最新的数据在右边）。
            g.setFont (uiFont (9.5f));
            g.setColour (violetText.withAlpha (0.8f));
            g.drawText (tr (Text::sideAutoGain, language), gainLane.reduced (5.0f, 0.0f), juce::Justification::centredLeft, false);

            const auto pointAt = [&] (int age) -> const HistoryPoint&   // age 0 = 最新
            {
                return history[(size_t) ((historyHead - 1 - age + 2 * historyLength) % historyLength)];
            };

            if (historyCount == 0)
            {
                g.setFont (uiFont (10.5f));
                g.setColour (palette::ink4);
                g.drawText (tr (Text::sideWaiting, language), lane, juce::Justification::centred, false);
            }
            else
            {
                juce::Path line, area, gain;
                bool open = false;

                for (int age = historyCount - 1; age >= 0; --age)
                {
                    const auto& point = pointAt (age);
                    const auto x = right - (float) age * step;

                    if (hasReading (point.shortTermLufs))
                    {
                        const auto y = yOfLufs (point.shortTermLufs);

                        if (! open)
                        {
                            line.startNewSubPath (x, y);
                            area.startNewSubPath (x, lane.getBottom());
                            area.lineTo (x, y);
                            open = true;
                        }
                        else
                        {
                            line.lineTo (x, y);
                            area.lineTo (x, y);
                        }

                        if (age == 0 || ! hasReading (pointAt (age - 1).shortTermLufs))
                        {
                            area.lineTo (x, lane.getBottom());
                            area.closeSubPath();
                            open = false;
                        }
                    }

                    const auto gainHeight = juce::jlimit (0.0f, 1.0f, point.autoGainDb / gainHigh) * gainLane.getHeight();

                    if (age == historyCount - 1)
                        gain.startNewSubPath (x, gainLane.getBottom());

                    gain.lineTo (x, gainLane.getBottom() - gainHeight);

                    if (age == 0)
                    {
                        gain.lineTo (x, gainLane.getBottom());
                        gain.closeSubPath();
                    }
                }

                {
                    juce::Graphics::ScopedSaveState saved (g);
                    g.reduceClipRegion (lane.toNearestInt());
                    g.setColour (palette::c300.withAlpha (0.22f));
                    g.fillPath (area);
                    g.setColour (palette::c600);
                    g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }

                g.setColour (palette::violet.withAlpha (0.55f));
                g.fillPath (gain);

                g.setColour (palette::warning);

                for (int age = historyCount - 1; age >= 0; --age)
                    if (pointAt (age).guardEngaged && (age == historyCount - 1 || ! pointAt (age + 1).guardEngaged))
                        g.fillRect (right - (float) age * step - 1.0f, gainLane.getY(), 2.0f, gainLane.getHeight());
            }

            if (values.hasTarget)
            {
                const auto y = std::round (yOfLufs (values.targetLufs)) + 0.5f;
                const float dashes[] = { 3.0f, 2.0f };
                g.setColour (palette::ink.withAlpha (0.7f));
                g.drawDashedLine (juce::Line<float> (lane.getX() + 2.0f, y, lane.getRight() - 2.0f, y), dashes, 2, 1.0f);
            }

            // 两道的说明：响度道左上、目标图例右上；增益道右边。
            const auto small = uiFont (9.5f);
            g.setFont (small);
            g.setColour (palette::ink3);
            g.drawText (tr (Text::sideHistory, language), lane.reduced (5.0f, 3.0f).withHeight (12.0f), juce::Justification::centredLeft, false);

            const auto legend = tr (Text::sideTargetLegend, language);
            const auto legendWidth = textWidth (small, legend);
            const auto legendRight = lane.getRight() - 5.0f;
            const float dashes[] = { 3.0f, 2.0f };
            g.drawText (legend, juce::Rectangle<float> (legendRight - legendWidth - 1.0f, lane.getY() + 3.0f, legendWidth + 2.0f, 12.0f),
                        juce::Justification::centredRight, false);
            g.setColour (palette::ink.withAlpha (0.7f));
            g.drawDashedLine (juce::Line<float> (legendRight - legendWidth - 18.0f, lane.getY() + 9.5f, legendRight - legendWidth - 5.0f, lane.getY() + 9.5f),
                              dashes, 2, 1.0f);
        }
    }

    void DeliveryPanel::mouseUp (const juce::MouseEvent& e)
    {
        if (e.mouseWasClicked() && resetIconBounds().expanded (3.0f).contains (e.position) && onReset != nullptr)
            onReset();
    }

    void DeliveryPanel::mouseMove (const juce::MouseEvent& e)
    {
        const auto hovered = resetIconBounds().expanded (3.0f).contains (e.position);

        if (hovered != resetHovered)
        {
            resetHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint (resetIconBounds().expanded (4.0f).getSmallestIntegerContainer());
        }
    }

    void DeliveryPanel::mouseExit (const juce::MouseEvent&)
    {
        if (resetHovered)
        {
            resetHovered = false;
            repaint (resetIconBounds().expanded (4.0f).getSmallestIntegerContainer());
        }
    }

    juce::String DeliveryPanel::getTooltip()
    {
        const auto position = getMouseXYRelative().toFloat();
        const auto content = contentArea();

        if (resetIconBounds().expanded (3.0f).contains (position))
            return help::tooltipFor (uiid::resetMeters, language);

        if (position.y < content.getY() + 40.0f)
        {
            const auto columns = content.getRight() - 240.0f;

            if (position.x >= columns + 80.0f && position.x < columns + 160.0f)
                return help::tooltipFor (uiid::meterOut, language);

            if (position.x >= columns && position.x < columns + 80.0f)
                return help::tooltipFor (uiid::shortTerm, language);
        }

        return help::tooltipFor (uiid::integrated, language);
    }
}
