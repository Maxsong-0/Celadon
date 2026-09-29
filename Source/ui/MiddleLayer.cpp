/*
  ==============================================================================

    MiddleLayer.cpp

  ==============================================================================
*/

#include "MiddleLayer.h"

#include "CeladonLookAndFeel.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        void drawLockIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
        {
            // 设计稿 8 × 9：锁身 7 × 4.5（圆角 1）+ 锁梁半圆。
            g.setColour (colour);
            g.fillRoundedRectangle (area.getX() + 0.5f, area.getY() + 4.0f, 7.0f, 4.5f, 1.0f);
            juce::Path shackle;
            shackle.startNewSubPath (area.getX() + 2.0f, area.getY() + 4.0f);
            shackle.lineTo (area.getX() + 2.0f, area.getY() + 2.6f);
            shackle.addCentredArc (area.getX() + 4.0f, area.getY() + 2.6f, 2.0f, 2.0f, 0.0f,
                                   -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, false);
            shackle.lineTo (area.getX() + 6.0f, area.getY() + 4.0f);
            g.strokePath (shackle, juce::PathStrokeType (1.1f));
        }
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

        // 阶段标签：文字与颜色
        juce::String chipLabel;
        auto chipBackground = palette::c100, chipForeground = palette::c700;

        switch (chip)
        {
            case Chip::locked:      chipLabel = tr (Text::chipLocked, language);      break;
            case Chip::listening:   chipLabel = tr (Text::chipListening, language);   break;
            case Chip::calibrating: chipLabel = tr (Text::chipCalibrating, language); break;
            case Chip::unlearned:   chipLabel = tr (Text::chipUnlearned, language);   chipBackground = palette::groove; chipForeground = palette::ink3; break;
            case Chip::paused:      chipLabel = tr (Text::chipPaused, language);      chipBackground = palette::groove; chipForeground = palette::ink3; break;
            case Chip::limited:     chipLabel = tr (Text::chipLimited, language);     chipBackground = palette::warningSoft; chipForeground = palette::warningText; break;
            case Chip::off:         break;
        }

        const auto hasIcon = chip == Chip::locked;
        const auto chipWidth = chipLabel.isEmpty() ? 0.0f : 7.0f + (hasIcon ? 8.0f + 4.0f : 0.0f) + textWidth (chipFont, chipLabel) + 7.0f;
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

        auto chipX = chipArea.getX() + 7.0f;

        if (hasIcon)
        {
            drawLockIcon (g, { chipX, chipArea.getCentreY() - 4.5f, 8.0f, 9.0f }, chipForeground);
            chipX += 8.0f + 4.0f;
        }

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

    void PillButton::setText (const juce::String& newText)
    {
        if (text == newText)
            return;

        text = newText;
        repaint();
    }

    void PillButton::setLook (Look newLook)
    {
        if (look == newLook)
            return;

        look = newLook;
        repaint();
    }

    void PillButton::setDot (bool shouldShowDot)
    {
        if (dot == shouldShowDot)
            return;

        dot = shouldShowDot;
        repaint();
    }

    int PillButton::getNaturalWidth() const
    {
        const auto font = uiFont (icon == Icon::relearn ? 12.0f : 11.5f, Weight::medium);
        return (int) std::ceil (12.0f + 12.0f + 6.0f + textWidth (font, text) + (dot ? 12.0f : 0.0f) + 12.0f);
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
        const auto contentWidth = 12.0f + 6.0f + textWidth (font, text) + (dot ? 12.0f : 0.0f);
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

        if (dot)
        {
            x += textWidth (font, text) + 6.0f;
            g.setColour (palette::warning);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ x + 3.0f, centreY }));
        }
    }

    //==============================================================================
    CornerNote::CornerNote()
    {
        setInterceptsMouseClicks (false, false);
    }

    void CornerNote::setLines (const juce::String& first, const juce::String& second, bool isHint)
    {
        if (first == line1 && second == line2 && isHint == hint)
            return;

        line1 = first;
        line2 = second;
        hint = isHint;
        repaint();
    }

    void CornerNote::paint (juce::Graphics& g)
    {
        g.setFont (uiFont (11.0f));
        g.setColour (hint ? palette::c700 : palette::ink3);
        g.drawText (line1, juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), 16.0f), juce::Justification::centredLeft, false);
        g.drawText (line2, juce::Rectangle<float> (0.0f, 16.0f, (float) getWidth(), 16.0f), juce::Justification::centredLeft, false);
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
}
