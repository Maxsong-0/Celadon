/*
  ==============================================================================

    LevelMeters.cpp

  ==============================================================================
*/

#include "LevelMeters.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr float barTop = 34.0f, barHeight = 188.0f, barWidth = 8.0f;
        constexpr float fallDbPerSecond = 20.0f;
        constexpr double holdSeconds = 1.2;
        constexpr float holdReleasePerSecond = 1.2f;   // 保持线停够之后向表条靠拢的速度（指数，1 / s）
        constexpr float reductionFullScale = 12.0f;
        constexpr double overWarningSeconds = 3.0;     // 超过 Ceiling 之后橙色保持这么久

        float proportionOfDb (float db) noexcept   { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); }
        float proportionOfGr (float db) noexcept   { return juce::jlimit (0.0f, 1.0f, db / reductionFullScale); }

        juce::String peakText (float db)
        {
            if (db <= -59.95f)
                return juce::String::charToString ((juce::juce_wchar) 0x2212) + juce::String::charToString ((juce::juce_wchar) 0x221e);

            const auto magnitude = juce::String (std::abs (db), 1);

            if (db <= -0.05f)
                return juce::String::charToString ((juce::juce_wchar) 0x2212) + magnitude;

            return (db >= 0.05f ? "+" : "") + magnitude;
        }
    }

    //==============================================================================
    void MeterBallistics::push (float reading, double elapsedSeconds) noexcept
    {
        const auto dt = (float) juce::jlimit (0.0, 0.25, elapsedSeconds);
        const auto floor = reduction ? 0.0f : -120.0f;
        const auto value = std::isfinite (reading) ? juce::jmax (floor, reading) : floor;

        level = juce::jmax (value, level - fallDbPerSecond * dt, floor);

        if (value >= hold)
        {
            hold = value;
            holdAge = 0.0;
        }
        else
        {
            holdAge += (double) dt;

            if (holdAge > holdSeconds)
                hold += (level - hold) * juce::jmin (1.0f, holdReleasePerSecond * dt);
        }

        // 衰减表 0.0 也是有效读数（「此刻没在压」）；峰值表要有信号才算。
        if (reduction || value > -119.0f)
        {
            if (! maximumValid || value > maximum)
                maximum = value;

            maximumValid = true;
        }
    }

    void MeterBallistics::resetHold() noexcept
    {
        hold = level;
        holdAge = 0.0;
        maximum = reduction ? 0.0f : -120.0f;
        maximumValid = false;
    }

    //==============================================================================
    bool LevelMeterPanel::Shown::operator== (const Shown& other) const noexcept
    {
        return juce::approximatelyEqual (level, other.level) && juce::approximatelyEqual (hold, other.hold)
            && juce::approximatelyEqual (grLevel, other.grLevel) && juce::approximatelyEqual (grHold, other.grHold)
            && peakText == other.peakText && grText == other.grText && over == other.over && planLimited == other.planLimited;
    }

    LevelMeterPanel::LevelMeterPanel (Kind k)
        : kind (k)
    {
        setSize (width, height);
    }

    void LevelMeterPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        repaint();
    }

    LevelMeterPanel::Shown LevelMeterPanel::computeShown() const
    {
        // 表条位置按半个逻辑像素量化（190 px 高的表，比这更细的变化看不出来）：变了才重画。
        const auto quantise = [] (float proportion) { return std::round (proportion * barHeight * 2.0f) / (barHeight * 2.0f); };

        Shown s;
        s.level = quantise (proportionOfDb (peak.getLevel()));
        s.hold = quantise (proportionOfDb (peak.getHold()));
        s.peakText = peak.hasMaximum() ? peakText (peak.getMaximum()) : peakText (-120.0f);
        s.over = overRecently;

        if (kind == Kind::outputAndReduction)
        {
            s.grLevel = quantise (proportionOfGr (reduction.getLevel()));
            s.grHold = quantise (proportionOfGr (reduction.getHold()));
            s.grText = juce::String (juce::jmax (0.0f, reduction.getMaximum()), 1);
            s.planLimited = planLimited;
        }

        return s;
    }

    void LevelMeterPanel::pushReadings (float peakDb, float reductionDb, float ceilingDb, bool isPlanLimited, double elapsedSeconds)
    {
        peak.push (peakDb, elapsedSeconds);

        if (kind == Kind::outputAndReduction)
        {
            reduction.push (reductionDb, elapsedSeconds);
            ceiling = ceilingDb;
            planLimited = isPlanLimited;

            if (std::isfinite (peakDb) && std::isfinite (ceilingDb) && peakDb > ceilingDb + 0.05f)
                overAge = 0.0;
            else
                overAge += juce::jlimit (0.0, 0.25, elapsedSeconds);

            overRecently = overAge < overWarningSeconds;
        }

        const auto next = computeShown();

        if (! (next == shown))
        {
            shown = next;
            repaint();
        }
    }

    void LevelMeterPanel::resetHolds()
    {
        peak.resetHold();
        reduction.resetHold();
        overAge = overWarningSeconds;
        overRecently = false;
        shown = computeShown();
        repaint();
    }

    void LevelMeterPanel::mouseUp (const juce::MouseEvent& e)
    {
        if (e.mouseWasClicked() && e.position.y > 222.0f)
        {
            resetHolds();

            if (onResetClicked != nullptr)
                onResetClicked();
        }
    }

    juce::String LevelMeterPanel::getTooltip()
    {
        if (kind == Kind::input)
            return help::tooltipFor (uiid::meterIn, language);

        return help::tooltipFor (getMouseXYRelative().x < 30 ? uiid::meterOut : uiid::meterGr, language);
    }

    void LevelMeterPanel::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();

        g.setColour (palette::panel);
        g.fillRoundedRectangle (bounds, 12.0f);
        g.setColour (palette::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 11.5f, 1.0f);

        const auto titleFont = withLetterSpacing (uiFont (11.0f, Weight::semibold), kind == Kind::input ? 1.0f : 0.0f);

        // 峰值表条：从下往上长，渐变下浅上深（填充自己的高度里）。
        const auto drawPeakBar = [&] (float x, bool warning)
        {
            const juce::Rectangle<float> bar (x, barTop, barWidth, barHeight);
            g.setColour (palette::track);
            g.fillRoundedRectangle (bar, 4.0f);

            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 4.0f);
            g.reduceClipRegion (clip);

            const auto fillHeight = barHeight * shown.level;

            if (fillHeight > 0.25f)
            {
                const auto fill = bar.withTop (bar.getBottom() - fillHeight);
                g.setGradientFill (juce::ColourGradient (palette::c300, 0.0f, fill.getBottom(), palette::c700, 0.0f, fill.getY(), false));
                g.fillRect (fill);
            }

            if (shown.hold > 0.0f)
            {
                const auto y = bar.getBottom() - barHeight * shown.hold;
                g.setColour (warning ? palette::warning : palette::ink);
                g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 2.0f, y - 1.0f), barWidth, 2.0f);
            }
        };

        if (kind == Kind::input)
        {
            g.setFont (titleFont);
            g.setColour (palette::ink2);
            g.drawText (tr (Text::meterIn, language), juce::Rectangle<float> (0.0f, 10.0f, bounds.getWidth(), 14.0f), juce::Justification::centred, false);

            drawPeakBar (16.0f, false);

            // 刻度：0 / −6 / −12 / −24 / −48
            g.setFont (monoFont (10.0f));
            g.setColour (palette::ink3);

            for (const auto db : { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
            {
                const auto y = barTop + barHeight * (1.0f - proportionOfDb (db)) - 6.0f;
                const auto text = db == 0.0f ? juce::String ("0") : juce::String::charToString ((juce::juce_wchar) 0x2212) + juce::String (juce::roundToInt (-db));
                g.drawText (text, juce::Rectangle<float> (29.0f, y, 30.0f, 12.0f), juce::Justification::centredLeft, false);
            }

            g.setFont (monoFont (11.0f, Weight::medium));
            g.setColour (palette::ink);
            g.drawText (shown.peakText, juce::Rectangle<float> (0.0f, 231.0f, bounds.getWidth(), 16.0f), juce::Justification::centred, false);
            return;
        }

        // ---- OUT + GR ----
        g.setFont (titleFont);
        g.setColour (palette::ink2);
        g.drawText (tr (Text::meterOut, language), juce::Rectangle<float> (1.0f, 10.0f, 32.0f, 14.0f), juce::Justification::centred, false);
        g.drawText (tr (Text::meterGr, language), juce::Rectangle<float> (27.0f, 10.0f, 32.0f, 14.0f), juce::Justification::centred, false);

        // 超限小灯
        g.setColour (shown.over ? palette::warning : palette::track);
        g.fillRoundedRectangle (13.0f, 28.0f, barWidth, 3.0f, 1.5f);

        drawPeakBar (13.0f, shown.over);

        // GR 表条：从上往下长，上深下浅；方案受限时橙色。
        {
            const juce::Rectangle<float> bar (39.0f, barTop, barWidth, barHeight);
            g.setColour (palette::track);
            g.fillRoundedRectangle (bar, 4.0f);

            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 4.0f);
            g.reduceClipRegion (clip);

            const auto fillHeight = barHeight * shown.grLevel;

            if (fillHeight > 0.25f)
            {
                const auto fill = bar.withHeight (fillHeight);
                g.setGradientFill (shown.planLimited
                                       ? juce::ColourGradient (palette::warningText, 0.0f, fill.getY(), palette::warning, 0.0f, fill.getBottom(), false)
                                       : juce::ColourGradient (palette::c900, 0.0f, fill.getY(), palette::c600, 0.0f, fill.getBottom(), false));
                g.fillRect (fill);
            }

            if (shown.grHold > 0.0f)
            {
                const auto y = bar.getY() + barHeight * shown.grHold;
                g.setColour (palette::ink);
                g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 2.0f, y - 1.0f), barWidth, 2.0f);
            }
        }

        const auto valueFont = withLetterSpacing (monoFont (10.5f, Weight::medium), -0.3f);
        g.setFont (valueFont);
        g.setColour (shown.over ? palette::warningText : palette::ink);
        g.drawText (shown.peakText, juce::Rectangle<float> (2.0f, 231.0f, 30.0f, 16.0f), juce::Justification::centred, false);
        g.setColour (shown.planLimited ? palette::warningText : palette::c800);
        g.drawText (shown.grText, juce::Rectangle<float> (31.0f, 231.0f, 24.0f, 16.0f), juce::Justification::centred, false);
    }
}
