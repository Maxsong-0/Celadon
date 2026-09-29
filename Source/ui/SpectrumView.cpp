/*
  ==============================================================================

    SpectrumView.cpp

  ==============================================================================
*/

#include "SpectrumView.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        const juce::String& minusSign()
        {
            static const juce::String sign = juce::String::charToString ((juce::juce_wchar) 0x2212);
            return sign;
        }

        // 带符号的数字（负号用 U+2212）；0 附近不写正负号。
        juce::String signedText (float value, int decimals, bool plusSign = false)
        {
            const auto threshold = 0.5f * std::pow (10.0f, (float) -decimals);
            const auto magnitude = juce::String (std::abs (value), decimals);

            if (value <= -threshold)
                return minusSign() + magnitude;

            return (plusSign && value >= threshold ? "+" : "") + magnitude;
        }

        // 读数里的音名：A4 = 440 Hz，音分向零截断（与设计稿一致）。
        juce::String noteText (float frequency, Language language)
        {
            static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const auto midi = 69.0 + 12.0 * std::log2 ((double) frequency / 440.0);
            const auto nearest = (int) std::lround (midi);
            const auto cents = (int) std::trunc ((midi - (double) nearest) * 100.0);
            const auto name = juce::String (names[((nearest % 12) + 12) % 12]) + juce::String ((int) std::floor ((double) nearest / 12.0) - 1);

            return name + (cents >= 0 ? " + " : " " + minusSign() + " ") + juce::String (std::abs (cents)) + " " + tr (Text::cents, language);
        }

        juce::String frequencyText (float frequency)
        {
            return frequency >= 1000.0f ? juce::String (frequency / 1000.0f, 2) + "kHz" : juce::String (frequency, 2) + "Hz";
        }

        // 画一个小的「重新计量」图标（顺时针的圆箭头）。
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
    }

    //==============================================================================
    LoudnessHud::LoudnessHud()
    {
        setSize (214, 88);
    }

    void LoudnessHud::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        shownKey.clear();
        repaint();
    }

    juce::Rectangle<float> LoudnessHud::resetIconBounds() const
    {
        const auto titleWidth = textWidth (withLetterSpacing (uiFont (10.5f, Weight::semibold), 1.0f), tr (Text::loudness, language));
        return { 12.0f + titleWidth + 5.0f, 7.0f, 14.0f, 14.0f };
    }

    void LoudnessHud::setValues (const Values& newValues)
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

    void LoudnessHud::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();

        g.setColour (juce::Colours::white.withAlpha (0.86f));
        g.fillRoundedRectangle (bounds, 10.0f);
        g.setColour (palette::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 9.5f, 1.0f);

        const auto mono = monoFont (10.5f);
        const auto dash = juce::String::charToString ((juce::juce_wchar) 0x2014);
        const auto lufsText = [&] (float lufs) { return hasReading (lufs) ? signedText (lufs, 1) : dash; };
        constexpr float left = 12.0f, right = 202.0f;

        // ① 标题 + 重新计量图标 + 真峰值
        {
            const auto titleFont = withLetterSpacing (uiFont (10.5f, Weight::semibold), 1.0f);
            g.setFont (titleFont);
            g.setColour (palette::ink2);
            g.drawText (tr (Text::loudness, language), juce::Rectangle<float> (left, 7.5f, 120.0f, 13.0f), juce::Justification::centredLeft, false);
            drawResetIcon (g, resetIconBounds(), resetHovered ? palette::c600 : palette::ink3);

            const auto over = values.truePeakMaxDb > values.ceilingDb + 0.05f;
            const auto peakValue = values.truePeakMaxDb > -99.0f ? signedText (values.truePeakMaxDb, 1, true) : dash;
            const auto valueWidth = textWidth (mono, peakValue);
            g.setFont (mono);
            g.setColour (over ? palette::warningText : palette::ink);
            g.drawText (peakValue, juce::Rectangle<float> (right - valueWidth, 7.5f, valueWidth + 1.0f, 13.0f), juce::Justification::centredRight, false);
            g.setColour (palette::ink2);
            g.drawText (tr (Text::truePeakShort, language) + " ", juce::Rectangle<float> (right - valueWidth - 80.0f, 7.5f, 80.0f, 13.0f),
                        juce::Justification::centredRight, false);
        }

        // ② 整体响度大字 + LUFS-I + 短期
        {
            const auto big = withLetterSpacing (uiFont (26.0f, Weight::light), -1.0f);
            const auto text = lufsText (values.integratedLufs);
            const auto bigWidth = textWidth (big, text);
            const auto baseline = 21.0f + 23.5f;

            g.setColour (palette::ink);
            g.setFont (big);
            g.drawSingleLineText (text, juce::roundToInt (left), juce::roundToInt (baseline));

            g.setFont (monoFont (10.0f));
            g.setColour (palette::ink2);
            g.drawSingleLineText ("LUFS-I", juce::roundToInt (left + bigWidth + 5.0f), juce::roundToInt (baseline));

            const auto shortValue = lufsText (values.shortTermLufs);
            const auto shortWidth = textWidth (mono, shortValue);
            g.setFont (mono);
            g.setColour (palette::ink);
            g.drawSingleLineText (shortValue, juce::roundToInt (right - shortWidth), juce::roundToInt (baseline));
            g.setColour (palette::ink2);
            const auto label = tr (Text::shortTermShort, language) + " ";
            g.drawSingleLineText (label, juce::roundToInt (right - shortWidth - textWidth (mono, label)), juce::roundToInt (baseline));
        }

        // ③ 响度条：−20 ～ −4 LUFS；虚线 = 流媒体 −14；三角 = GLAZE 的目标
        {
            constexpr float barLeft = left, barWidth = 190.0f, top = 53.0f;
            const auto xOfLufs = [&] (float lufs) { return barLeft + juce::jlimit (0.0f, 1.0f, (lufs + 20.0f) / 16.0f) * barWidth; };

            g.setColour (palette::track);
            g.fillRoundedRectangle (barLeft, top + 5.0f, barWidth, 3.0f, 1.5f);

            if (hasReading (values.integratedLufs))
            {
                const auto fillRight = xOfLufs (values.integratedLufs);
                g.setGradientFill (juce::ColourGradient (palette::c200, barLeft, 0.0f, palette::c700, barLeft + barWidth, 0.0f, false));
                g.fillRoundedRectangle (barLeft, top + 5.0f, juce::jmax (0.0f, fillRight - barLeft), 3.0f, 1.5f);
            }

            const auto stream = xOfLufs (-14.0f);
            const float dashes[] = { 1.5f, 1.0f };
            g.setColour (palette::c800);
            g.drawDashedLine (juce::Line<float> (stream, top + 2.5f, stream, top + 9.0f), dashes, 2, 1.2f);

            if (values.hasTarget)
            {
                const auto x = xOfLufs (values.targetLufs);
                juce::Path marker;
                marker.addTriangle (x, top + 4.5f, x - 3.0f, top, x + 3.0f, top);
                g.setColour (palette::ink);
                g.fillPath (marker);
            }
        }

        // ④ 峰响比 · 归一 · 流媒体参考
        {
            const auto row = juce::Rectangle<float> (left, 66.0f, 190.0f, 13.0f);
            const auto plr = hasReading (values.integratedLufs) && values.truePeakMaxDb > -99.0f
                                 ? juce::String (values.truePeakMaxDb - values.integratedLufs, 1) : dash;
            const auto norm = hasReading (values.integratedLufs) ? signedText (-14.0f - values.integratedLufs, 1, true) : dash;

            g.setFont (mono);
            auto x = row.getX();

            const auto pair = [&] (const juce::String& name, const juce::String& value, juce::Colour valueColour)
            {
                g.setColour (palette::ink2);
                g.drawText (name + " ", juce::Rectangle<float> (x, row.getY(), 80.0f, row.getHeight()), juce::Justification::centredLeft, false);
                x += textWidth (mono, name + " ");
                g.setColour (valueColour);
                g.drawText (value, juce::Rectangle<float> (x, row.getY(), 60.0f, row.getHeight()), juce::Justification::centredLeft, false);
                x += textWidth (mono, value);
            };

            pair (tr (Text::peakToLoudness, language), plr, palette::ink);
            x = row.getX() + 76.0f;
            pair (tr (Text::normalisation, language), norm, palette::c800);

            const auto streamText = tr (Text::streamingReference, language);
            const auto streamWidth = textWidth (mono, streamText);
            const float dashes[] = { 1.5f, 1.0f };
            g.setColour (palette::c800);
            g.drawDashedLine (juce::Line<float> (row.getRight() - streamWidth - 5.0f, row.getY() + 2.5f,
                                                 row.getRight() - streamWidth - 5.0f, row.getY() + 10.5f), dashes, 2, 1.2f);
            g.setColour (palette::ink2);
            g.drawText (streamText, row, juce::Justification::centredRight, false);
        }
    }

    void LoudnessHud::mouseUp (const juce::MouseEvent& e)
    {
        if (e.mouseWasClicked() && resetIconBounds().expanded (3.0f).contains (e.position) && onReset != nullptr)
            onReset();
    }

    void LoudnessHud::mouseMove (const juce::MouseEvent& e)
    {
        const auto hovered = resetIconBounds().expanded (3.0f).contains (e.position);

        if (hovered != resetHovered)
        {
            resetHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void LoudnessHud::mouseExit (const juce::MouseEvent&)
    {
        if (resetHovered)
        {
            resetHovered = false;
            repaint();
        }
    }

    juce::String LoudnessHud::getTooltip()
    {
        const auto position = getMouseXYRelative().toFloat();

        if (resetIconBounds().expanded (3.0f).contains (position))
            return help::tooltipFor (uiid::resetMeters, language);

        if (position.y < 21.0f && position.x > 110.0f)
            return help::tooltipFor (uiid::meterOut, language);

        if (position.y >= 21.0f && position.y < 52.0f)
            return help::tooltipFor (position.x > 130.0f ? uiid::shortTerm : uiid::integrated, language);

        return help::tooltipFor (uiid::integrated, language);
    }

    //==============================================================================
    SpectrumView::LegendToggle::LegendToggle (Swatch s)
        : swatch (s)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void SpectrumView::LegendToggle::setText (const juce::String& newText)
    {
        text = newText;
        repaint();
    }

    int SpectrumView::LegendToggle::getNaturalWidth() const
    {
        return (int) std::ceil (8.0f + 10.0f + 5.0f + textWidth (uiFont (11.0f), text) + 8.0f);
    }

    void SpectrumView::LegendToggle::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto alpha = on ? 1.0f : 0.45f;

        g.setColour (juce::Colours::white.withAlpha (0.85f * (isHovering() ? 1.0f : 0.95f)));
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (palette::controlBorder.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

        const auto centreY = bounds.getCentreY();
        juce::Rectangle<float> chip (8.0f, centreY - (swatch == Swatch::gradient ? 3.5f : 0.75f), 10.0f, swatch == Swatch::gradient ? 7.0f : 1.5f);

        if (swatch == Swatch::gradient)
            g.setGradientFill (juce::ColourGradient (palette::c700.withMultipliedAlpha (alpha), chip.getX(), 0.0f,
                                                     juce::Colour (0xff8fd9e3).withMultipliedAlpha (alpha), chip.getRight(), 0.0f, false));
        else
            g.setColour ((swatch == Swatch::input ? palette::c600 : palette::c900).withMultipliedAlpha (alpha));

        g.fillRoundedRectangle (chip, 1.0f);

        g.setFont (uiFont (11.0f));
        g.setColour (palette::ink2.withMultipliedAlpha (alpha));
        g.drawText (text, juce::Rectangle<float> (23.0f, 0.0f, bounds.getWidth() - 23.0f, (float) getHeight()), juce::Justification::centredLeft, false);
    }

    //==============================================================================
    SpectrumView::SlopeControl::SlopeControl()
    {
        choices.setOptions ({ "0", "3", "4.5" });
        choices.setEqualWidths (false);
        choices.setFont (monoFont (10.5f));
        choices.setSelected (2);
        choices.onSelect = [this] (int) { if (onChange != nullptr) onChange(); };
        addAndMakeVisible (choices);
    }

    void SpectrumView::SlopeControl::setLabel (const juce::String& newLabel)
    {
        label = newLabel;
        resized();
        repaint();
    }

    float SpectrumView::SlopeControl::getSlope() const noexcept
    {
        constexpr float slopes[] = { 0.0f, 3.0f, 4.5f };
        return slopes[juce::jlimit (0, 2, choices.getSelected())];
    }

    int SpectrumView::SlopeControl::getNaturalWidth() const
    {
        return (int) std::ceil (8.0f + textWidth (uiFont (11.0f), label) + 4.0f + (float) choices.getNaturalWidth()
                                + 6.0f + textWidth (monoFont (10.0f), "dB/oct") + 4.0f);
    }

    void SpectrumView::SlopeControl::resized()
    {
        const auto labelWidth = textWidth (uiFont (11.0f), label);
        choices.setBounds ((int) std::ceil (8.0f + labelWidth + 4.0f), 3, choices.getNaturalWidth(), getHeight() - 6);
    }

    void SpectrumView::SlopeControl::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (palette::controlBorder);
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

        g.setFont (uiFont (11.0f));
        g.setColour (palette::ink2);
        g.drawText (label, juce::Rectangle<float> (8.0f, 0.0f, 60.0f, (float) getHeight()), juce::Justification::centredLeft, false);

        g.setFont (monoFont (10.0f));
        g.setColour (palette::ink3);
        g.drawText ("dB/oct", juce::Rectangle<float> ((float) choices.getRight() + 6.0f, 0.0f, 50.0f, (float) getHeight()),
                    juce::Justification::centredLeft, false);
    }

    //==============================================================================
    SpectrumView::SpectrumView()
    {
        setSize (width, height);
        setMouseCursor (juce::MouseCursor::CrosshairCursor);

        const auto points = (size_t) SpectrumAnalyser::numPoints;
        output.assign (points, SpectrumAnalyser::floorDb);
        input.assign (points, SpectrumAnalyser::floorDb);
        average.assign (points, SpectrumAnalyser::floorDb);
        peak.assign (points, SpectrumAnalyser::floorDb);

        for (auto* toggle : { &outputToggle, &inputToggle, &peakToggle })
        {
            addAndMakeVisible (*toggle);
            toggle->onClick = [this, toggle] { toggle->setOn (! toggle->isOn()); repaint(); };
        }

        inputToggle.setOn (false);
        slopeControl.onChange = [this] { updatePeakReadout(); repaint(); };
        addAndMakeVisible (slopeControl);

        addAndMakeVisible (hud);
        setLanguage (Language::chinese);
    }

    void SpectrumView::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        outputToggle.setText (tr (Text::showOutput, language));
        inputToggle.setText (tr (Text::showInput, language));
        peakToggle.setText (tr (Text::showPeak, language));
        slopeControl.setLabel (tr (Text::slope, language));
        hud.setLanguage (language);

        // 读数框按最长的读数定宽，读数变化时后面的开关不跟着左右跳。
        readoutWidth = (int) std::ceil (textWidth (monoFont (11.5f, Weight::semibold),
                                                    minusSign() + "88.88dB | 88.88kHz | C#8 " + minusSign() + " 88 " + tr (Text::cents, language)) + 22.0f);
        layoutControls();
        repaint();
    }

    void SpectrumView::resized()
    {
        layoutControls();
    }

    void SpectrumView::layoutControls()
    {
        auto x = 48 + readoutWidth + 6;

        for (auto* toggle : { &outputToggle, &inputToggle, &peakToggle })
        {
            toggle->setBounds (x, 13, toggle->getNaturalWidth(), 24);
            x += toggle->getWidth() + 6;
        }

        slopeControl.setBounds (x, 13, slopeControl.getNaturalWidth(), 24);
        hud.setTopLeftPosition (806, 12);
    }

    float SpectrumView::xOf (float frequency) noexcept
    {
        return SpectrumAnalyser::positionOf (frequency) * (float) width;
    }

    float SpectrumView::yOf (float db) noexcept
    {
        return juce::jlimit (0.0f, (float) height, (topDb - db) / (topDb - bottomDb) * (float) height);
    }

    float SpectrumView::tiltAt (float frequency) const noexcept
    {
        return slopeControl.getSlope() * std::log2 (frequency / 1000.0f);
    }

    void SpectrumView::setSpectrum (const SpectrumAnalyser& analyser)
    {
        // 几条曲线都没有看得出的变化（没有音乐、曲线已经落到底）就不重画：停着的时候不白占 CPU。
        const auto differs = [] (const std::vector<float>& shown, const std::vector<float>& next)
        {
            if (shown.size() != next.size())
                return true;

            for (size_t i = 0; i < shown.size(); ++i)
                if (std::abs (shown[i] - next[i]) > 0.02f)
                    return true;

            return false;
        };

        const auto& nextOutput = analyser.getDisplayDb (SpectrumAnalyser::Stream::output);
        const auto& nextInput = analyser.getDisplayDb (SpectrumAnalyser::Stream::input);
        const auto changed = differs (output, nextOutput) || differs (input, nextInput)
                          || differs (average, analyser.getAverageDb()) || differs (peak, analyser.getPeakDb());

        if (! changed)
            return;

        output  = nextOutput;
        input   = nextInput;
        average = analyser.getAverageDb();
        peak    = analyser.getPeakDb();

        // 鼠标不在面板上时，读数跟着此刻最响的峰（每 8 帧换一次，免得数字跳得看不清）。
        if (++framesSincePeakReadout >= 8)
            updatePeakReadout();

        repaint();
    }

    void SpectrumView::updatePeakReadout()
    {
        framesSincePeakReadout = 0;
        auto best = SpectrumAnalyser::floorDb - 100.0f;

        for (int i = 0; i < SpectrumAnalyser::numPoints; ++i)
        {
            const auto frequency = SpectrumAnalyser::frequencyAt (i);

            if (frequency < 30.0f || frequency > 16000.0f)
                continue;

            const auto value = output[(size_t) i] + tiltAt (frequency);

            if (value > best)
            {
                best = value;
                peakFrequency = frequency;
            }
        }

        peakDb = best;
    }

    void SpectrumView::setClayMarkers (float lowCutHz, float monoHz, bool active)
    {
        if (juce::approximatelyEqual (lowCutHz, clayLowCutHz) && juce::approximatelyEqual (monoHz, clayMonoHz) && active == clayActive)
            return;

        clayLowCutHz = lowCutHz;
        clayMonoHz = monoHz;
        clayActive = active;
        repaint();
    }

    juce::String SpectrumView::readoutText() const
    {
        const auto frequency = hoverFrequency.value_or (peakFrequency);
        float db = peakDb;

        if (hoverFrequency.has_value())
        {
            const auto position = SpectrumAnalyser::positionOf (frequency) * (float) (SpectrumAnalyser::numPoints - 1);
            const auto below = juce::jlimit (0, SpectrumAnalyser::numPoints - 1, (int) std::floor (position));
            const auto above = juce::jmin (SpectrumAnalyser::numPoints - 1, below + 1);
            const auto t = position - (float) below;
            db = output[(size_t) below] + (output[(size_t) above] - output[(size_t) below]) * t + tiltAt (frequency);
        }

        if (db <= SpectrumAnalyser::floorDb + 1.0f)
            return juce::String::charToString ((juce::juce_wchar) 0x2014) + "dB | " + frequencyText (frequency) + " | " + noteText (frequency, language);

        return signedText (db, 2) + "dB | " + frequencyText (frequency) + " | " + noteText (frequency, language);
    }

    void SpectrumView::paint (juce::Graphics& g)
    {
        const auto w = (float) width, h = (float) height;
        juce::Path panel;
        panel.addRoundedRectangle (getLocalBounds().toFloat(), 14.0f);

        g.setGradientFill (juce::ColourGradient (juce::Colours::white, 0.0f, 0.0f, palette::spectrumFloor, 0.0f, h, false));
        g.fillPath (panel);

        {
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (panel);

            // ---- 网格 ----
            g.setColour (juce::Colour (0xff0d3b45).withAlpha (0.055f));

            for (const auto f : { 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 200.0f, 300.0f, 400.0f, 500.0f, 600.0f, 700.0f,
                                  800.0f, 900.0f, 2000.0f, 3000.0f, 4000.0f, 5000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f })
                g.fillRect (std::round (xOf (f)), 0.0f, 1.0f, h);

            for (float db = -12.0f; db >= -72.0f; db -= 12.0f)
                g.fillRect (0.0f, std::round (yOf (db)), w, 1.0f);

            g.setColour (juce::Colour (0xff0d3b45).withAlpha (0.12f));

            for (const auto f : { 100.0f, 1000.0f, 10000.0f })
                g.fillRect (std::round (xOf (f)), 0.0f, 1.0f, h);

            // ---- 修坯：低切以下打斜线 ----
            const auto showClay = clayActive && clayLowCutHz > 0.0f;

            if (showClay)
            {
                juce::Graphics::ScopedSaveState hatchState (g);
                const auto right = xOf (clayLowCutHz);
                g.reduceClipRegion (juce::Rectangle<int> (0, 0, (int) std::ceil (right), height));
                g.setColour (palette::c800.withAlpha (0.16f));

                for (float x = -h; x < right; x += 5.0f * std::sqrt (2.0f))
                    g.drawLine (x, h, x + h, 0.0f, 1.2f);
            }

            // ---- 频谱：OUT 填充 + 竖纹 + 渐隐 + 轮廓 + 平均线 ----
            const auto numPoints = SpectrumAnalyser::numPoints;
            const auto pointX = [] (int i) { return (float) i / (float) (SpectrumAnalyser::numPoints - 1) * (float) width; };

            const auto buildLine = [&] (const std::vector<float>& values, bool closed)
            {
                juce::Path path;

                if (closed)
                    path.startNewSubPath (0.0f, h);

                for (int i = 0; i < numPoints; ++i)
                {
                    const auto point = juce::Point<float> (pointX (i), yOf (values[(size_t) i] + tiltAt (SpectrumAnalyser::frequencyAt (i))));

                    if (i == 0 && ! closed)
                        path.startNewSubPath (point);
                    else
                        path.lineTo (point);
                }

                if (closed)
                {
                    path.lineTo (w, h);
                    path.closeSubPath();
                }

                return path;
            };

            {
                const auto outputAlpha = outputToggle.isOn() ? 1.0f : 0.12f;
                const auto fill = buildLine (output, true);

                juce::ColourGradient gradient (juce::Colour (0xff0a3f4b), 0.0f, 0.0f, juce::Colour (0xff8fd9e3), w, 0.0f, false);
                gradient.addColour (0.25, juce::Colour (0xff0e6473));
                gradient.addColour (0.5,  juce::Colour (0xff1590a3));
                gradient.addColour (0.7,  juce::Colour (0xff2aafc0));
                gradient.addColour (0.85, juce::Colour (0xff5dc6d3));
                gradient.multiplyOpacity (0.62f * outputAlpha);
                g.setGradientFill (gradient);
                g.fillPath (fill);

                {
                    juce::Graphics::ScopedSaveState fillState (g);
                    g.reduceClipRegion (fill);
                    g.setColour (juce::Colours::white.withAlpha (0.18f * outputAlpha));

                    for (float x = 0.0f; x < w; x += 2.0f)
                        g.fillRect (x, 0.0f, 0.8f, h);

                    g.setGradientFill (juce::ColourGradient (palette::spectrumFloor.withAlpha (0.0f), 0.0f, 40.0f,
                                                             palette::spectrumFloor.withAlpha (0.92f * outputAlpha), 0.0f, h, false));
                    g.fillRect (0.0f, 0.0f, w, h);
                }

                g.setColour (palette::c700.withAlpha (0.7f * outputAlpha));
                g.strokePath (buildLine (output, false), juce::PathStrokeType (0.8f));
                g.setColour (juce::Colours::white.withAlpha (0.9f * outputAlpha));
                g.strokePath (buildLine (average, false), juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // ---- IN（虚线）与峰值 ----
            if (inputToggle.isOn())
            {
                juce::Path dashed;
                const float dashes[] = { 3.0f, 2.0f };
                juce::PathStrokeType (1.1f).createDashedStroke (dashed, buildLine (input, false), dashes, 2);
                g.setColour (palette::c600.withAlpha (0.75f));
                g.fillPath (dashed);
            }

            if (peakToggle.isOn())
            {
                g.setColour (palette::c700.withAlpha (0.45f));
                g.strokePath (buildLine (peak, false), juce::PathStrokeType (1.0f));
            }

            // ---- 修坯的频率标记 ----
            if (clayActive)
            {
                const auto marker = [&] (float frequency, const juce::String& label)
                {
                    if (frequency <= 0.0f)
                        return;

                    // 低于 20 Hz 的低切（自动低切常在 16 Hz 附近）画不出竖线：只在左边缘（避开 dB 刻度）写标签。
                    const auto inRange = frequency >= SpectrumAnalyser::minFrequency;
                    const auto x = std::round (xOf (juce::jmax (frequency, SpectrumAnalyser::minFrequency))) + 0.5f;
                    const float dashes[] = { 3.0f, 3.0f };

                    if (inRange)
                    {
                        g.setColour (palette::c800.withAlpha (0.45f));
                        g.drawDashedLine (juce::Line<float> (x, 40.0f, x, h), dashes, 2, 1.0f);
                    }

                    const auto font = uiFont (10.0f);
                    const auto labelWidth = textWidth (font, label) + 10.0f;
                    const juce::Rectangle<float> chip (juce::jmax (x + 3.5f, 38.0f), 46.0f, labelWidth, 15.0f);
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.fillRoundedRectangle (chip, 4.0f);
                    g.setFont (font);
                    g.setColour (palette::c800);
                    g.drawText (label, chip, juce::Justification::centred, false);
                };

                marker (clayLowCutHz, tr (Text::markLowCut, language) + " " + juce::String (juce::roundToInt (clayLowCutHz)));
                marker (clayMonoHz, tr (Text::markMono, language) + " " + juce::String (juce::roundToInt (clayMonoHz)));
            }

            // ---- 悬停光标 ----
            if (hoverFrequency.has_value())
            {
                const auto x = xOf (*hoverFrequency);
                const float dashes[] = { 2.0f, 3.0f };
                g.setColour (palette::c900.withAlpha (0.45f));
                g.drawDashedLine (juce::Line<float> (x, 0.0f, x, h), dashes, 2, 1.0f);

                const auto position = SpectrumAnalyser::positionOf (*hoverFrequency) * (float) (numPoints - 1);
                const auto below = juce::jlimit (0, numPoints - 1, (int) std::floor (position));
                const auto above = juce::jmin (numPoints - 1, below + 1);
                const auto t = position - (float) below;
                const auto db = output[(size_t) below] + (output[(size_t) above] - output[(size_t) below]) * t + tiltAt (*hoverFrequency);
                const auto dot = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x, yOf (db) });
                g.setColour (juce::Colours::white);
                g.fillEllipse (dot);
                g.setColour (palette::c900);
                g.drawEllipse (dot, 1.6f);
            }

            // ---- 刻度文字 ----
            g.setFont (monoFont (10.5f));
            g.setColour (palette::ink2);

            for (const auto& [frequency, label] : { std::pair<float, const char*> { 100.0f, "100Hz" }, { 1000.0f, "1kHz" }, { 10000.0f, "10kHz" } })
                g.drawText (label, juce::Rectangle<float> (xOf (frequency) + 5.0f, (float) height - 20.0f, 60.0f, 14.0f), juce::Justification::centredLeft, false);

            const auto dbFont = monoFont (10.0f);

            for (float db = -12.0f; db >= -72.0f; db -= 12.0f)
            {
                const auto text = minusSign() + juce::String (juce::roundToInt (-db));
                const auto chip = juce::Rectangle<float> (5.0f, std::round (yOf (db)) - 7.0f, textWidth (dbFont, text) + 6.0f, 14.0f);
                g.setColour (juce::Colours::white.withAlpha (0.8f));
                g.fillRoundedRectangle (chip, 3.0f);
                g.setFont (dbFont);
                g.setColour (palette::ink2);
                g.drawText (text, chip, juce::Justification::centred, false);
            }

            // ---- 读数框 ----
            {
                const juce::Rectangle<float> box (48.0f, 12.0f, (float) readoutWidth, 26.0f);
                g.setColour (palette::shadowTint.withAlpha (0.07f));
                g.fillRoundedRectangle (box.translated (0.0f, 2.0f).expanded (1.5f), 8.0f);
                g.setColour (juce::Colours::white);
                g.fillRoundedRectangle (box, 7.0f);
                g.setColour (palette::controlBorder);
                g.drawRoundedRectangle (box.reduced (0.5f), 6.5f, 1.0f);
                g.setFont (monoFont (11.5f, Weight::semibold));
                g.setColour (palette::ink);
                g.drawText (readoutText(), box.reduced (10.0f, 0.0f), juce::Justification::centredLeft, false);
            }
        }

        g.setColour (palette::panelBorder);
        g.strokePath (panel, juce::PathStrokeType (1.0f));
    }

    void SpectrumView::mouseMove (const juce::MouseEvent& e)
    {
        const auto u = juce::jlimit (0.0f, 1.0f, e.position.x / (float) width);
        hoverFrequency = SpectrumAnalyser::minFrequency * std::pow (SpectrumAnalyser::maxFrequency / SpectrumAnalyser::minFrequency, u);
        repaint();
    }

    void SpectrumView::mouseExit (const juce::MouseEvent&)
    {
        hoverFrequency.reset();
        repaint();
    }
}
