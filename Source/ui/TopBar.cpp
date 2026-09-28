/*
  ==============================================================================

    TopBar.cpp

  ==============================================================================
*/

#include "TopBar.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        const juce::String& separator()
        {
            static const juce::String text = juce::String (" ") + juce::String::charToString ((juce::juce_wchar) 0x00b7) + " ";
            return text;
        }

        juce::Font pillFont (bool medium)   { return uiFont (12.0f, medium ? Weight::medium : Weight::regular); }

        void drawWarningIcon (juce::Graphics& g, juce::Rectangle<float> area)
        {
            juce::Path triangle;
            triangle.addTriangle (area.getCentreX(), area.getY() + 1.0f, area.getRight() - 0.5f, area.getBottom() - 1.0f,
                                  area.getX() + 0.5f, area.getBottom() - 1.0f);
            g.setColour (palette::warning);
            g.fillPath (triangle);
            g.strokePath (triangle, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour (juce::Colours::white);
            g.drawLine (area.getCentreX(), area.getY() + 4.6f, area.getCentreX(), area.getY() + 7.4f, 1.4f);
            g.fillEllipse (juce::Rectangle<float> (1.6f, 1.6f).withCentre ({ area.getCentreX(), area.getY() + 9.1f }));
        }
    }

    //==============================================================================
    bool StatusPill::Content::operator== (const Content& other) const
    {
        return warning == other.warning && segments == other.segments && accentSegment == other.accentSegment
            && accentColour == other.accentColour && dotColour == other.dotColour && pulse == other.pulse;
    }

    StatusPill::StatusPill()
    {
        content.dotColour = palette::c500;
    }

    StatusPill::~StatusPill()
    {
        stopTimer();
    }

    void StatusPill::setContent (const Content& newContent)
    {
        if (newContent == content)
            return;

        content = newContent;

        if (content.pulse && ! reduceMotion)
            startTimerHz (30);
        else
            stopTimer();

        repaint();
    }

    void StatusPill::setReduceMotion (bool shouldReduceMotion)
    {
        reduceMotion = shouldReduceMotion;

        if (content.pulse && ! reduceMotion)
            startTimerHz (30);
        else
            stopTimer();

        repaint();
    }

    void StatusPill::timerCallback()
    {
        // 圆点呼吸：2.4 s 一个周期（设计稿 pulse 动画）。
        pulsePhase = std::fmod (pulsePhase + 1.0f / (30.0f * 2.4f), 1.0f);
        repaint();
    }

    int StatusPill::getPreferredWidth() const
    {
        auto total = content.warning ? 13.0f + 7.0f : 7.0f + 7.0f;

        for (int i = 0; i < content.segments.size(); ++i)
        {
            total += textWidth (pillFont (content.warning || i == content.accentSegment), content.segments[i]);

            if (i > 0)
                total += textWidth (pillFont (false), separator());
        }

        return juce::jlimit (350, 560, (int) std::ceil (total + 40.0f));
    }

    void StatusPill::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto radius = bounds.getHeight() * 0.5f;

        g.setColour (palette::shadowTint.withAlpha (0.05f));
        g.fillRoundedRectangle (bounds.translated (0.0f, 1.0f), radius);
        g.setColour (content.warning ? palette::warningSoft : palette::panel);
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (content.warning ? palette::warningBorder : palette::segmentBorder);
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        // 内容整体居中：先量宽度。
        const auto sepWidth = textWidth (pillFont (false), separator());
        auto contentWidth = content.warning ? 13.0f + 7.0f : 7.0f + 7.0f;

        for (int i = 0; i < content.segments.size(); ++i)
            contentWidth += textWidth (pillFont (content.warning || i == content.accentSegment), content.segments[i]) + (i > 0 ? sepWidth : 0.0f);

        auto x = bounds.getCentreX() - contentWidth * 0.5f;
        const auto centreY = bounds.getCentreY();

        if (content.warning)
        {
            drawWarningIcon (g, { x, centreY - 6.0f, 13.0f, 12.0f });
            x += 13.0f + 7.0f;
        }
        else
        {
            const auto alpha = content.pulse && ! reduceMotion ? 0.35f + 0.65f * (0.5f + 0.5f * std::cos (pulsePhase * juce::MathConstants<float>::twoPi)) : 1.0f;
            const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x + 3.5f, centreY });
            g.setColour (content.dotColour.withAlpha (0.18f * alpha));
            g.fillEllipse (dot.expanded (3.0f));
            g.setColour (content.dotColour.withMultipliedAlpha (alpha));
            g.fillEllipse (dot);
            x += 7.0f + 7.0f;
        }

        for (int i = 0; i < content.segments.size(); ++i)
        {
            if (i > 0)
            {
                g.setFont (pillFont (false));
                g.setColour (content.warning ? palette::warningText : palette::separator);
                g.drawText (separator(), juce::Rectangle<float> (x, 0.0f, sepWidth + 1.0f, bounds.getHeight() + 1.0f), juce::Justification::centredLeft, false);
                x += sepWidth;
            }

            const auto accent = i == content.accentSegment;
            const auto font = pillFont (content.warning || accent);
            const auto w = textWidth (font, content.segments[i]);
            g.setFont (font);
            g.setColour (content.warning ? palette::warningText : (accent ? content.accentColour : palette::ink));
            g.drawText (content.segments[i], juce::Rectangle<float> (x, 0.0f, w + 2.0f, bounds.getHeight() + 1.0f), juce::Justification::centredLeft, false);
            x += w;
        }
    }

    //==============================================================================
    IconButton::IconButton (Icon i)
        : icon (i)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void IconButton::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (active ? palette::c100 : (isHovering() ? palette::groove : palette::panel));
        g.fillEllipse (bounds);
        g.setColour (active ? palette::relearnBorder : palette::controlBorder);
        g.drawEllipse (bounds, 1.0f);

        const auto centre = bounds.getCentre();

        if (icon == Icon::settings)
        {
            // 齿轮：虚线圆（齿）+ 实线小圆（设计稿）。
            juce::Path teeth, dashed;
            teeth.addEllipse (juce::Rectangle<float> (9.2f, 9.2f).withCentre (centre));
            const float dashes[] = { 2.1f, 1.52f };
            juce::PathStrokeType (2.2f).createDashedStroke (dashed, teeth, dashes, 2);
            g.setColour (palette::ink2);
            g.fillPath (dashed);
            g.drawEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (centre), 1.4f);
        }
        else
        {
            g.setFont (monoFont (12.0f));
            g.setColour (palette::ink2);
            g.drawText ("?", bounds, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    TopBar::TopBar()
    {
        addAndMakeVisible (pill);

        languageSelector.setOptions ({ juce::String::fromUTF8 ("中"), "EN" });
        languageSelector.setEqualWidths (false);
        languageSelector.setFont (uiFont (11.0f, Weight::medium));
        languageSelector.onSelect = [this] (int index)
        {
            if (onLanguageChange != nullptr)
                onLanguageChange (index == 1 ? Language::english : Language::chinese);
        };
        addAndMakeVisible (languageSelector);

        settingsButton.onClick = [this] { if (onSettings != nullptr) onSettings(); };
        helpButton.onClick = [this] { if (onHelp != nullptr) onHelp(); };
        addAndMakeVisible (settingsButton);
        addAndMakeVisible (helpButton);
    }

    void TopBar::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        languageSelector.setSelected (language == Language::english ? 1 : 0);
        settingsButton.setTooltip (tr (Text::settings, language));
        helpButton.setTooltip (tr (Text::help, language));
        pill.setTooltip (help::tooltipFor (uiid::glazeStatus, language));
        repaint();
    }

    void TopBar::setLatency (double milliseconds)
    {
        const auto text = tr (Text::latency, language) + " " + juce::String (milliseconds, 1) + " ms";

        if (text == latencyText)
            return;

        latencyText = text;
        repaint();
    }

    void TopBar::setStatus (const StatusPill::Content& content)
    {
        pill.setContent (content);
        const auto w = pill.getPreferredWidth();

        if (w != pill.getWidth())
            pill.setBounds (600 - w / 2, 12, w, 28);
    }

    void TopBar::resized()
    {
        const auto w = pill.getPreferredWidth();
        pill.setBounds (600 - w / 2, 12, w, 28);

        helpButton.setBounds (1180 - 28, 12, 28, 28);
        settingsButton.setBounds (helpButton.getX() - 10 - 28, 12, 28, 28);
        const auto languageWidth = languageSelector.getNaturalWidth();
        languageSelector.setBounds (settingsButton.getX() - 10 - languageWidth, 13, languageWidth, 26);
    }

    void TopBar::paint (juce::Graphics& g)
    {
        // 品牌：青色描边圆环（带淡光晕）+ CELADON + 副标题
        const juce::Point<float> ring (28.0f, 26.0f);
        g.setColour (palette::c500.withAlpha (0.14f));
        g.drawEllipse (juce::Rectangle<float> (22.0f, 22.0f).withCentre (ring), 3.0f);
        g.setColour (palette::c500);
        g.drawEllipse (juce::Rectangle<float> (13.5f, 13.5f).withCentre (ring), 2.5f);

        const auto brand = withLetterSpacing (uiFont (14.0f, Weight::semibold), 3.0f);
        g.setFont (brand);
        g.setColour (palette::ink);
        g.drawText ("CELADON", juce::Rectangle<float> (46.0f, 0.0f, 140.0f, 52.0f), juce::Justification::centredLeft, false);

        const auto subtitleX = 46.0f + textWidth (brand, "CELADON") + 10.0f;
        g.setFont (withLetterSpacing (uiFont (11.0f), 1.0f));
        g.setColour (palette::ink3);
        g.drawText (tr (Text::brandSubtitle, language), juce::Rectangle<float> (subtitleX, 0.0f, 160.0f, 52.0f), juce::Justification::centredLeft, false);

        // 延迟（等宽 11 px），紧挨在语言选择左边
        g.setFont (monoFont (11.0f));
        g.setColour (palette::ink3);
        g.drawText (latencyText, juce::Rectangle<float> ((float) languageSelector.getX() - 10.0f - 160.0f, 0.0f, 160.0f, 52.0f),
                    juce::Justification::centredRight, false);
    }
}
