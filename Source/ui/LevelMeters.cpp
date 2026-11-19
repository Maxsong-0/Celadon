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
        constexpr float panelBarTop = 34.0f, panelBarHeight = 166.0f, barWidth = 8.0f;   // V4 面板
        constexpr float deckBarTop = 18.0f, deckBarWidth = 6.0f;                          // V5 卡片里的表：表条长 = 高度 − 40
        constexpr float deckInputCentre = 22.0f, deckOutputCentre = 15.0f, deckReductionCentre = 43.0f;   // 各列中线
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

    LevelMeterPanel::LevelMeterPanel (Kind k, Style s)
        : kind (k), style (s)
    {
        if (style == Style::deck)
            setSize (kind == Kind::input ? deckInputWidth : deckOutputWidth, height);   // 高由 SpectrumDeck 再定
        else
            setSize (width, height);
    }

    float LevelMeterPanel::barTop() const noexcept
    {
        return style == Style::deck ? deckBarTop : panelBarTop;
    }

    float LevelMeterPanel::barLength() const noexcept
    {
        // deck：标签一行（0 ～ 14）、空 4、表条、空 4、数字一行（16 高）、底下留 2。
        return style == Style::deck ? juce::jmax (1.0f, (float) getHeight() - 40.0f) : panelBarHeight;
    }

    void LevelMeterPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        repaint();
    }

    LevelMeterPanel::Shown LevelMeterPanel::computeShown() const
    {
        // 表条位置按半个逻辑像素量化（190 px 高的表，比这更细的变化看不出来）：变了才重画。
        const auto barHeight = barLength();
        const auto quantise = [barHeight] (float proportion) { return std::round (proportion * barHeight * 2.0f) / (barHeight * 2.0f); };

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
        // 表条以下（数字那一行）才算点数字：V4 面板是最下面 38 px，deck 是最下面 22 px。
        if (e.mouseWasClicked() && e.position.y >= barTop() + barLength())
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

        return help::tooltipFor (getMouseXYRelative().x < getWidth() / 2 ? uiid::meterOut : uiid::meterGr, language);
    }

    void LevelMeterPanel::resized()
    {
        // deck 的高度会换（266 / 92）：表条长变了，量化好的位置按新长度重算。V4 面板的大小不变，照旧等第一次读数。
        if (style == Style::deck)
            shown = computeShown();
    }

    void LevelMeterPanel::paintDeck (juce::Graphics& g)
    {
        // V5（规格 2.3）：直接画在卡片的白底上。一列 = 标签（说明档，三级灰）、6 px 宽圆头表条、数字（说明档等宽）。
        const auto top = barTop(), length = barLength();
        const auto valueY = top + length + 4.0f;
        const auto labelFont = uiFont (TextSize::caption);
        const auto valueFont = withLetterSpacing (monoFont (TextSize::caption, Weight::medium), -0.3f);

        // 一根表条：fromTop = 衰减（从上往下长）。填充平涂；保持线 2 px 深色。
        const auto drawBar = [&] (float centreX, float level, float hold, bool fromTop, juce::Colour fillColour)
        {
            const juce::Rectangle<float> bar (centreX - 0.5f * deckBarWidth, top, deckBarWidth, length);
            g.setColour (palette::track);
            g.fillRoundedRectangle (bar, 0.5f * deckBarWidth);

            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 0.5f * deckBarWidth);
            g.reduceClipRegion (clip);

            const auto fillHeight = length * level;

            if (fillHeight > 0.25f)
            {
                g.setColour (fillColour);
                g.fillRect (fromTop ? bar.withHeight (fillHeight) : bar.withTop (bar.getBottom() - fillHeight));
            }

            if (hold > 0.0f)
            {
                const auto y = fromTop ? bar.getY() + length * hold : bar.getBottom() - length * hold;
                g.setColour (palette::ink);
                g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 2.0f, y - 1.0f), deckBarWidth, 2.0f);
            }
        };

        const auto drawLabel = [&] (Text text, float centreX, float boxWidth)
        {
            g.setFont (labelFont);
            g.setColour (palette::ink3);
            drawTextLine (g, tr (text, language), juce::Rectangle<float> (centreX - 0.5f * boxWidth, 0.0f, boxWidth, 14.0f), juce::Justification::centred);
        };

        const auto drawValue = [&] (const juce::String& text, juce::Rectangle<float> box, juce::Colour colour)
        {
            g.setFont (valueFont);
            g.setColour (colour);
            drawTextLine (g, text, box, juce::Justification::centred);
        };

        if (kind == Kind::input)
        {
            drawLabel (Text::meterIn, deckInputCentre, (float) getWidth());
            drawBar (deckInputCentre, shown.level, shown.hold, false, palette::c500);
            drawValue (shown.peakText, juce::Rectangle<float> (0.0f, valueY, (float) getWidth(), 16.0f), palette::ink);
            return;
        }

        // 输出 + 衰减：数字的两格 30 / 26 宽（「−59.9」要 30），表条在各自那一格的中线上。
        drawLabel (Text::meterOut, deckOutputCentre, 30.0f);
        drawLabel (Text::meterGr, deckReductionCentre, 26.0f);
        drawBar (deckOutputCentre, shown.level, shown.hold, false, shown.over ? palette::warning : palette::c500);
        drawBar (deckReductionCentre, shown.grLevel, shown.grHold, true, shown.planLimited ? palette::warning : palette::ink);
        drawValue (shown.peakText, juce::Rectangle<float> (0.0f, valueY, 30.0f, 16.0f), shown.over ? palette::warningText : palette::ink);
        drawValue (shown.grText, juce::Rectangle<float> (30.0f, valueY, 26.0f, 16.0f), shown.planLimited ? palette::warningText : palette::ink2);
    }

    void LevelMeterPanel::paint (juce::Graphics& g)
    {
        if (style == Style::deck)
        {
            paintDeck (g);
            return;
        }

        const auto bounds = getLocalBounds().toFloat();

        // 圆角 14：与频谱面板、模块卡片同一排同一个圆角（T-111 / 4-25，原 12；编辑器静态层的阴影一起改）。
        g.setColour (palette::panel);
        g.fillRoundedRectangle (bounds, 14.0f);
        g.setColour (palette::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 13.5f, 1.0f);

        const auto titleFont = withLetterSpacing (uiFont (TextSize::caption, Weight::semibold), kind == Kind::input ? 1.0f : 0.0f);

        // 峰值表条：从下往上长，渐变下浅上深（填充自己的高度里）。
        const auto drawPeakBar = [&] (float x, bool warning)
        {
            const juce::Rectangle<float> bar (x, panelBarTop, barWidth, panelBarHeight);
            g.setColour (palette::track);
            g.fillRoundedRectangle (bar, 4.0f);

            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 4.0f);
            g.reduceClipRegion (clip);

            const auto fillHeight = panelBarHeight * shown.level;

            if (fillHeight > 0.25f)
            {
                const auto fill = bar.withTop (bar.getBottom() - fillHeight);
                g.setGradientFill (juce::ColourGradient (palette::c300, 0.0f, fill.getBottom(), palette::c700, 0.0f, fill.getY(), false));
                g.fillRect (fill);
            }

            if (shown.hold > 0.0f)
            {
                const auto y = bar.getBottom() - panelBarHeight * shown.hold;
                g.setColour (warning ? palette::warning : palette::ink);
                g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 2.0f, y - 1.0f), barWidth, 2.0f);
            }
        };

        if (kind == Kind::input)
        {
            g.setFont (titleFont);
            g.setColour (palette::ink2);
            drawTextLine (g, tr (Text::meterIn, language), juce::Rectangle<float> (0.0f, 10.0f, bounds.getWidth(), 14.0f), juce::Justification::centred);

            drawPeakBar (16.0f, false);

            // 刻度：0 / −6 / −12 / −24 / −48
            g.setFont (monoFont (TextSize::micro));
            g.setColour (palette::ink3);

            for (const auto db : { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
            {
                const auto y = panelBarTop + panelBarHeight * (1.0f - proportionOfDb (db)) - 6.0f;
                const auto text = db == 0.0f ? juce::String ("0") : juce::String::charToString ((juce::juce_wchar) 0x2212) + juce::String (juce::roundToInt (-db));
                drawTextLine (g, text, juce::Rectangle<float> (29.0f, y, 30.0f, 12.0f), juce::Justification::centredLeft);
            }

            // 表下的读数与 OUT / GR 同一档（说明档等宽）：表只有 60 px 宽，OUT 那一格 30 px 放得下「−59.9」（T-112）。
            g.setFont (monoFont (TextSize::caption, Weight::medium));
            g.setColour (palette::ink);
            drawTextLine (g, shown.peakText, juce::Rectangle<float> (0.0f, (float) height - 29.0f, bounds.getWidth(), 16.0f), juce::Justification::centred);
            return;
        }

        // ---- OUT + GR ----
        g.setFont (titleFont);
        g.setColour (palette::ink2);
        drawTextLine (g, tr (Text::meterOut, language), juce::Rectangle<float> (1.0f, 10.0f, 32.0f, 14.0f), juce::Justification::centred);
        drawTextLine (g, tr (Text::meterGr, language), juce::Rectangle<float> (27.0f, 10.0f, 32.0f, 14.0f), juce::Justification::centred);

        // 超限小灯
        g.setColour (shown.over ? palette::warning : palette::track);
        g.fillRoundedRectangle (13.0f, 28.0f, barWidth, 3.0f, 1.5f);

        drawPeakBar (13.0f, shown.over);

        // GR 表条：从上往下长，上深下浅；方案受限时橙色。
        {
            const juce::Rectangle<float> bar (39.0f, panelBarTop, barWidth, panelBarHeight);
            g.setColour (palette::track);
            g.fillRoundedRectangle (bar, 4.0f);

            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 4.0f);
            g.reduceClipRegion (clip);

            const auto fillHeight = panelBarHeight * shown.grLevel;

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
                const auto y = bar.getY() + panelBarHeight * shown.grHold;
                g.setColour (palette::ink);
                g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 2.0f, y - 1.0f), barWidth, 2.0f);
            }
        }

        const auto valueFont = withLetterSpacing (monoFont (TextSize::caption, Weight::medium), -0.3f);
        g.setFont (valueFont);
        g.setColour (shown.over ? palette::warningText : palette::ink);
        drawTextLine (g, shown.peakText, juce::Rectangle<float> (2.0f, (float) height - 29.0f, 30.0f, 16.0f), juce::Justification::centred);
        g.setColour (shown.planLimited ? palette::warningText : palette::c800);
        drawTextLine (g, shown.grText, juce::Rectangle<float> (31.0f, (float) height - 29.0f, 24.0f, 16.0f), juce::Justification::centred);
    }
}
