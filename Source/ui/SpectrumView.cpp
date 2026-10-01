/*
  ==============================================================================

    SpectrumView.cpp

  ==============================================================================
*/

#include "SpectrumView.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <array>
#include <cmath>
#include <initializer_list>

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

        // 频率刻度文字（100Hz / 1kHz / 10kHz）的框：刻度线右边 5 px、底边上方。
        juce::Rectangle<float> frequencyLabelBounds (float frequency)
        {
            return { SpectrumView::xOf (frequency) + 5.0f, (float) SpectrumView::height - 20.0f, 60.0f, 14.0f };
        }

        // dB 刻度的一个标签：文字与白色圆角底框（面板左边缘）。
        struct DbLabel
        {
            juce::String text;
            juce::Rectangle<float> chip;
        };

        DbLabel dbLabelFor (float db, const juce::Font& font)
        {
            auto text = minusSign() + juce::String (juce::roundToInt (-db));
            const juce::Rectangle<float> chip (5.0f, std::round (SpectrumView::yOf (db)) - 7.0f, textWidth (font, text) + 6.0f, 14.0f);
            return { std::move (text), chip };
        }

        // OUT 填充按响度上色（T-036，像 MiniMeters）：u = 那一段曲线顶在面板里的高度（0 = 底边、1 = 顶边）。安静 = 灰青、半透明；
        // 越响越亮越饱和，最响是亮薄荷青。色带只按 u 取，查 256 级的表（第一次用时生成）。
        juce::Colour levelColour (float u)
        {
            static const std::array<juce::Colour, 256> table = []
            {
                juce::ColourGradient ramp (juce::Colour (0xff8aa3aa), 0.0f, 0.0f, juce::Colour (0xff6fe6cc), 1.0f, 0.0f, false);
                ramp.addColour (0.30, juce::Colour (0xff3f8e9b));
                ramp.addColour (0.55, palette::c500);
                ramp.addColour (0.78, juce::Colour (0xff2fc0c6));

                std::array<juce::Colour, 256> colours;

                for (size_t k = 0; k < colours.size(); ++k)
                {
                    const auto position = (double) k / (double) (colours.size() - 1);
                    colours[k] = ramp.getColourAtPosition (position).withAlpha ((float) (0.55 + 0.30 * position));
                }

                return colours;
            }();

            return table[(size_t) juce::jlimit (0, 255, juce::roundToInt (u * 255.0f))];
        }

        // 读数框的影子：往下 2 px、四周外扩 1.5 px。
        juce::Rectangle<float> readoutShadowBounds (juce::Rectangle<float> box)
        {
            return box.translated (0.0f, 2.0f).expanded (1.5f);
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
        {
            // OUT 的色块：与填充同一条「按响度上色」的色带，左安静右响（不透明，色块太小，半透明看不清）。
            juce::ColourGradient ramp (levelColour (0.2f).withAlpha (alpha), chip.getX(), 0.0f, levelColour (1.0f).withAlpha (alpha), chip.getRight(), 0.0f, false);
            ramp.addColour (0.5, levelColour (0.6f).withAlpha (alpha));
            g.setGradientFill (ramp);
        }
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

        // 每点的横坐标与频率只跟点号有关（T-032 R9-10）：算一次，与原来画图时逐点现算的是同一算式。
        for (int i = 0; i < SpectrumAnalyser::numPoints; ++i)
        {
            pointX[(size_t) i] = (float) i / (float) (SpectrumAnalyser::numPoints - 1) * (float) width;
            pointFrequency[(size_t) i] = SpectrumAnalyser::frequencyAt (i);
        }

        // dB 刻度的底框位置与宽度（字是数字，不随语言变）：量一次，paint 里不再每帧量字宽。
        {
            const auto font = monoFont (10.0f);
            auto db = -12.0f;

            for (auto& chip : dbChips)
            {
                chip = dbLabelFor (db, font).chip;
                db -= 12.0f;
            }
        }

        for (auto* toggle : { &outputToggle, &inputToggle, &peakToggle })
        {
            addAndMakeVisible (*toggle);
            toggle->onClick = [this, toggle] { toggle->setOn (! toggle->isOn()); repaint(); };
        }

        inputToggle.setOn (false);
        slopeControl.onChange = [this] { updateTilt(); updatePeakReadout (output); refreshReadout(); repaint(); };
        addAndMakeVisible (slopeControl);

        addAndMakeVisible (hud);

        // 频谱一秒重画 60 / 120 次（T-036）：图例开关、斜率、响度小窗都压在面板上，曲线一动就在脏区里——各自缓存成图，
        // 自己没变时只贴图，不再每帧重排文字（它们自己 repaint 时缓存作废、下次重画）。用项目自己的对齐缓存（Controls.h，
        // T-032 A9）：按实际像素比生成、整数对齐贴图；JUCE 自带的 setBufferedToImage 在小数像素比（界面 70 % / 115 %、
        // Windows 125 % / 150 %）下每帧双线性重采样，字发糊。
        for (auto* child : std::initializer_list<juce::Component*> { &outputToggle, &inputToggle, &peakToggle, &slopeControl, &hud })
            setBufferedToAlignedImage (*child);

        updateTilt();
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
        shownReadout.clear();
        readoutString.clear();
        invalidate (Layer::readout);
        refreshReadout();
        updateMarkerLabels();
        layoutControls();
        repaint();
    }

    void SpectrumView::resized()
    {
        panel.clear();
        panel.addRoundedRectangle (getLocalBounds().toFloat(), 14.0f);
        invalidate (Layer::back);
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

    void SpectrumView::updateTilt()
    {
        // 斜率只在换档时变（T-032 R9-10）：每点的斜率（dB）算一次，画曲线、找峰只做加法。与 tiltAt 同一算式，逐位相同。
        const auto slope = slopeControl.getSlope();

        for (size_t i = 0; i < tilt.size(); ++i)
            tilt[i] = slope * std::log2 (pointFrequency[i] / 1000.0f);

        curvePathsValid = false;
        inputDashesValid = false;
    }

    void SpectrumView::setSpectrum (SpectrumAnalyser& analyser, double elapsedSeconds)
    {
        // IN 虚线关着时让计算器跳过 IN 那路的加窗 + FFT（T-032 R7-05）。刚打开时它要到下一次 update 才有 IN 的曲线：这之前不画 IN。
        analyser.setInputSpectrumEnabled (inputToggle.isOn());
        const auto nextInputReady = analyser.hasInputSpectrum();

        if (std::isfinite (elapsedSeconds) && elapsedSeconds > 0.0)
            secondsSinceReadout += elapsedSeconds;

        // 几条曲线在屏上都没有看得出的变化（没有音乐、曲线已经落到底、只在面板底边以下起伏）就不重建、不重画：
        // 按加了斜率、钳到面板里的纵坐标比（0.1 个逻辑像素），比的是上一次画出来的那组值，慢慢漂也会在累计过门槛时重画。
        const auto differs = [this] (const std::vector<float>& shown, const std::vector<float>& next)
        {
            if (shown.size() != next.size())
                return true;

            for (size_t i = 0; i < shown.size(); ++i)
                if (std::abs (yOf (shown[i] + tilt[i]) - yOf (next[i] + tilt[i])) > 0.1f)
                    return true;

            return false;
        };

        const auto& nextOutput = analyser.getDisplayDb (SpectrumAnalyser::Stream::output);
        const auto& nextInput = analyser.getDisplayDb (SpectrumAnalyser::Stream::input);
        const auto changed = nextInputReady != inputReady
                          || differs (output, nextOutput) || differs (input, nextInput)
                          || differs (average, analyser.getAverageDb()) || differs (peak, analyser.getPeakDb());

        if (changed)
        {
            inputReady = nextInputReady;
            output  = nextOutput;
            input   = nextInput;
            average = analyser.getAverageDb();
            peak    = analyser.getPeakDb();
            inputDashesValid = false;
            buildCurvePaths();
            repaintCurves();
        }

        // 读数（鼠标不在面板上时是此刻最响的峰，悬停时是光标处）的 dB 约 0.27 s 换一次（原来 30 帧时每 8 帧；T-036 起悬停处也按这个
        // 节奏，鼠标移动时立即）：两位小数一秒变几十次读不清，读数框也不必每帧重画。用计算器此刻的值，不受上面 0.1 像素门槛影响。
        if (secondsSinceReadout >= 8.0 / 30.0)
        {
            updatePeakReadout (nextOutput);

            if (hoverFrequency.has_value())
                hoverDb = curveDbAt (nextOutput, *hoverFrequency);

            refreshReadout();
        }
    }

    juce::Rectangle<float> SpectrumView::curveRegion() const
    {
        // 此刻几条曲线（含描边外伸）占的区域：OUT 的填充一直到底边，所以区域总是从最高的曲线顶到面板底。
        // 描边超出路径包围盒的上限同 paint：尖角接头最多伸出 2 × 线宽，再加 1 像素抗锯齿。
        const auto strokeArea = [] (const juce::Path& path, float thickness) { return path.getBounds().expanded (2.0f * thickness + 1.0f); };
        // 峰值线与 IN 线不论开关都算进去：开关切换那一帧是整面重画，之后的「旧区域」也要盖住它们可能画过的地方。
        auto area = outputFill.getBounds().getUnion (strokeArea (outputLine, 0.8f)).getUnion (strokeArea (averageLine, 1.4f))
                                          .getUnion (strokeArea (peakLine, 1.0f));

        if (! inputLine.isEmpty())
            area = area.getUnion (strokeArea (inputLine, 1.1f));

        return area.getIntersection (getLocalBounds().toFloat());
    }

    void SpectrumView::repaintCurves()
    {
        // 曲线变了（T-036）：只重画新旧曲线合起来的区域；悬停时光标圆点跟着曲线走，补上光标那一列（读数框由 refreshReadout 管）。
        // 图例、斜率、响度小窗在脏区里的部分从各自的缓存图贴（构造函数里 setBufferedToAlignedImage）。
        const auto area = curveRegion();
        repaint (area.getUnion (curveArea).getSmallestIntegerContainer());
        curveArea = area;

        if (hoverFrequency.has_value())
            repaint (cursorBounds());
    }

    float SpectrumView::curveDbAt (const std::vector<float>& values, float frequency) noexcept
    {
        // 某个频率上一条曲线的值（未加斜率）：相邻两点之间按对数频率线性插值。
        const auto position = juce::jlimit (0.0f, 1.0f, SpectrumAnalyser::positionOf (frequency)) * (float) (SpectrumAnalyser::numPoints - 1);
        const auto below = juce::jlimit (0, SpectrumAnalyser::numPoints - 1, (int) std::floor (position));
        const auto above = juce::jmin (SpectrumAnalyser::numPoints - 1, below + 1);
        const auto t = position - (float) below;
        return values[(size_t) below] + (values[(size_t) above] - values[(size_t) below]) * t;
    }

    void SpectrumView::refreshReadout()
    {
        // 读数框的文字（缓存成串，paint 里与缓存图里的文字比）：变了才重拼、才重画读数框。
        const auto frequency = hoverFrequency.value_or (peakFrequency);
        const auto db = hoverFrequency.has_value() ? hoverDb + tiltAt (frequency) : peakDb;
        auto text = readoutTextFor (frequency, db);

        if (text == readoutString)
            return;

        readoutString = std::move (text);
        repaint (layerArea (Layer::readout));
    }

    void SpectrumView::updatePeakReadout (const std::vector<float>& values)
    {
        secondsSinceReadout = 0.0;
        auto best = SpectrumAnalyser::floorDb - 100.0f;

        for (size_t i = 0; i < pointFrequency.size(); ++i)
        {
            const auto frequency = pointFrequency[i];

            if (frequency < 30.0f || frequency > 16000.0f)
                continue;

            const auto value = values[i] + tilt[i];

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

        // 低切以下的斜线画在底层缓存里：只有斜线变了（出现 / 消失，或低切频率变了）才重生成；只改收单频率不动底层。
        const auto hatchBefore = clayActive && clayLowCutHz > 0.0f;
        const auto hatchAfter = active && lowCutHz > 0.0f;

        if (hatchBefore != hatchAfter || (hatchAfter && ! juce::approximatelyEqual (lowCutHz, clayLowCutHz)))
            invalidate (Layer::back);

        clayLowCutHz = lowCutHz;
        clayMonoHz = monoHz;
        clayActive = active;
        updateMarkerLabels();
        repaint();
    }

    void SpectrumView::updateMarkerLabels()
    {
        // 标签文字与宽度只随频率与语言变：在这里算好，paint 里不再每帧拼串、量宽。
        const auto font = uiFont (10.0f);
        lowCutLabel = tr (Text::markLowCut, language) + " " + juce::String (juce::roundToInt (clayLowCutHz));
        monoLabel = tr (Text::markMono, language) + " " + juce::String (juce::roundToInt (clayMonoHz));
        lowCutLabelWidth = textWidth (font, lowCutLabel);
        monoLabelWidth = textWidth (font, monoLabel);
    }

    juce::Rectangle<float> SpectrumView::readoutBox() const noexcept
    {
        return { 48.0f, 12.0f, (float) readoutWidth, 26.0f };
    }

    juce::String SpectrumView::readoutTextFor (float frequency, float db) const
    {
        if (db <= SpectrumAnalyser::floorDb + 1.0f)
            return juce::String::charToString ((juce::juce_wchar) 0x2014) + "dB | " + frequencyText (frequency) + " | " + noteText (frequency, language);

        return signedText (db, 2) + "dB | " + frequencyText (frequency) + " | " + noteText (frequency, language);
    }

    juce::Rectangle<int> SpectrumView::cursorBounds() const
    {
        // 悬停光标：1 px 竖虚线 + 8 px 圆点（1.6 px 描边），都在光标横坐标 ±6 px 以内。
        if (! hoverFrequency.has_value())
            return {};

        const auto x = xOf (*hoverFrequency);
        return juce::Rectangle<float> (x - 6.0f, 0.0f, 12.0f, (float) height).getSmallestIntegerContainer();
    }

    void SpectrumView::setHoverFrequency (std::optional<float> frequency)
    {
        // 悬停只改光标与读数（T-032 R9-04 ③）：只重画新旧光标列，读数文字变了再重画读数框；paint 里与这些区域不相交的整块直接跳过。
        // 鼠标移动时读数立即换到新位置（之后按 8 / 30 s 的节奏刷新，见 setSpectrum）。
        repaint (cursorBounds());
        hoverFrequency = frequency;
        repaint (cursorBounds());

        if (hoverFrequency.has_value())
            hoverDb = curveDbAt (output, *hoverFrequency);

        refreshReadout();
    }

    //==============================================================================
    void SpectrumView::buildLine (const std::vector<float>& values, juce::Path& path, int step) const
    {
        // 加了斜率的一条曲线（横坐标、斜率都来自预算表）。step > 1：每隔 step 个点取一个（最后一个点总在）。
        const auto stride = juce::jmax (1, step);
        const auto last = SpectrumAnalyser::numPoints - 1;
        path.clear();
        path.preallocateSpace (3 * (SpectrumAnalyser::numPoints / stride + 2));

        for (int i = 0;; i = juce::jmin (last, i + stride))
        {
            const auto point = juce::Point<float> (pointX[(size_t) i], yOf (values[(size_t) i] + tilt[(size_t) i]));

            if (i == 0)
                path.startNewSubPath (point);
            else
                path.lineTo (point);

            if (i == last)
                break;
        }
    }

    void SpectrumView::buildCurvePaths()
    {
        // 数据或斜率变了才重建（悬停时只重画光标与读数，不再重建）。OUT 的填充与轮廓同一组点：填充接到右下角、左下角再闭合
        // （T-032 R9-04 ④）。平均线本身已沿频率平滑过两遍（±13 点），隔一个点取一个看不出差别，描边少一半的段。
        buildLine (output, outputLine);
        buildLine (output, outputFill);   // 直接建（clear 保留容量），不拷贝 outputLine：播放中每帧建一次，拷贝就是一次分配
        outputFill.lineTo ((float) width, (float) height);
        outputFill.lineTo (0.0f, (float) height);
        outputFill.closeSubPath();
        buildLine (average, averageLine, 2);
        buildEnvelope (peak, peakLine);

        if (inputReady)
            buildLine (input, inputLine);
        else
            inputLine.clear();

        buildColumnLevels();
        curvePathsValid = true;
    }

    void SpectrumView::buildEnvelope (const std::vector<float>& values, juce::Path& path) const
    {
        // 峰值线：每两个点取较高的那个（横坐标也取它的）——高频一片细密的保持值折线段少一半，尖峰一个不丢。
        const auto last = SpectrumAnalyser::numPoints - 1;
        path.clear();
        path.preallocateSpace (3 * (SpectrumAnalyser::numPoints / 2 + 2));

        for (int i = 0; i <= last; i += 2)
        {
            auto chosen = i;

            if (i < last && values[(size_t) i + 1] + tilt[(size_t) i + 1] > values[(size_t) i] + tilt[(size_t) i])
                chosen = i + 1;

            const auto point = juce::Point<float> (pointX[(size_t) chosen], yOf (values[(size_t) chosen] + tilt[(size_t) chosen]));

            if (i == 0)
                path.startNewSubPath (point);
            else
                path.lineTo (point);
        }
    }

    void SpectrumView::buildColumnLevels()
    {
        // 按响度上色（T-036）：每个逻辑像素的颜色位置 = 它 ±2 像素里曲线顶的最高处（0 = 底边、1 = 顶边）——高频一片细密的尖峰
        // 按包络上色，不随单个点闪。设备像素在两个逻辑像素之间线性插值（prepareOutputRow）。
        constexpr int reach = 2;
        const auto last = SpectrumAnalyser::numPoints - 1;

        for (int i = 0; i <= last; ++i)
        {
            auto best = SpectrumAnalyser::floorDb - 100.0f;

            for (int j = juce::jmax (0, i - reach); j <= juce::jmin (last, i + reach); ++j)
                best = juce::jmax (best, output[(size_t) j] + tilt[(size_t) j]);

            columnLevel[(size_t) i] = 1.0f - yOf (best) / (float) height;
        }

        outputRowValid = false;
    }

    void SpectrumView::prepareOutputRow (float pixelScale)
    {
        // OUT 填充的一行（T-036）：按响度上色的颜色，再把白色竖纹（与竖纹瓦片同一份覆盖率）按预乘的 over 盖上去——原来是
        // 横向渐变、竖纹瓦片两遍填充，现在一遍：一行图纵向平铺，一次 fillPath。曲线变了（或像素比变了）才重建，约 2000 个像素。
        if (outputRowValid && juce::approximatelyEqual (outputRowScale, pixelScale) && outputRow.isValid())
            return;

        prepareStripes (pixelScale, juce::Colours::white.withAlpha (0.18f));
        const auto rowWidth = (int) stripeAlpha.size();

        if (! outputRow.isValid() || outputRow.getWidth() != rowWidth)
            outputRow = juce::Image (juce::Image::ARGB, rowWidth, 1, true);

        juce::Image::BitmapData row (outputRow, juce::Image::BitmapData::writeOnly);
        const auto last = SpectrumAnalyser::numPoints - 1;

        for (int x = 0; x < rowWidth; ++x)
        {
            const auto logical = juce::jlimit (0.0f, (float) last, ((float) x + 0.5f) / pixelScale * (float) last / (float) width);
            const auto i = juce::jmin (last - 1, (int) logical);
            const auto u = columnLevel[(size_t) i] + (columnLevel[(size_t) i + 1] - columnLevel[(size_t) i]) * (logical - (float) i);
            const auto fill = levelColour (u);
            const auto fillAlpha = fill.getFloatAlpha();
            const auto stripe = stripeAlpha[(size_t) x];
            const auto alpha = stripe + fillAlpha * (1.0f - stripe);

            if (alpha <= 0.0f)
            {
                row.setPixelColour (x, 0, juce::Colours::transparentBlack);
                continue;
            }

            const auto over = [&] (float channel) { return (stripe + channel * fillAlpha * (1.0f - stripe)) / alpha; };
            row.setPixelColour (x, 0, juce::Colour::fromFloatRGBA (over (fill.getFloatRed()), over (fill.getFloatGreen()),
                                                                   over (fill.getFloatBlue()), alpha));
        }

        outputRowScale = pixelScale;
        outputRowValid = true;
    }

    //==============================================================================
    juce::Rectangle<int> SpectrumView::layerArea (Layer which) const
    {
        const auto padded = [] (juce::Rectangle<float> area) { return area.getSmallestIntegerContainer().expanded (1); };

        switch (which)
        {
            case Layer::back:
                // 四周多留 1 个逻辑像素：组件原点落在小数设备像素上时（界面缩放 70 % 等），整张图吸附到最近的整数像素，
                // 面板边缘那一列 / 一行只被部分覆盖的像素仍在图里，不会露出底下的背景。原点在整数像素上时多出的一圈在面板裁剪之外。
                return getLocalBounds().expanded (1);

            case Layer::frequencyText:
            {
                juce::Rectangle<float> area;

                for (const auto frequency : { 100.0f, 1000.0f, 10000.0f })
                    area = area.getUnion (frequencyLabelBounds (frequency));

                return padded (area);
            }

            case Layer::dbLabels:
            {
                juce::Rectangle<float> area;

                for (const auto& chip : dbChips)
                    area = area.getUnion (chip);

                return padded (area);
            }

            case Layer::readout:
                return padded (readoutShadowBounds (readoutBox()).getUnion (readoutBox()));

            case Layer::count:
                break;
        }

        return {};
    }

    void SpectrumView::paintLayer (Layer which, juce::Graphics& g) const
    {
        const auto w = (float) width, h = (float) height;

        switch (which)
        {
            case Layer::back:
            {
                // 先把裁剪区换成边表（整张图满覆盖）：原来网格是在面板圆角的边表裁剪区里 fillRect 的，小数像素比下细线边缘走
                // EdgeTable (Rectangle<float>) 的抗锯齿；新图默认的矩形列表裁剪区走另一套浮点矩形抗锯齿
                // （FloatRectangleRasterisingInfo），边缘覆盖会差 1（与竖纹瓦片同一个处理，见 prepareStripes）。
                {
                    juce::Path whole;
                    whole.addRectangle (layerArea (Layer::back).toFloat().expanded (1.0f));
                    g.reduceClipRegion (whole);
                }

                // 面板渐变铺满整个矩形：圆角由贴图时的面板裁剪给出（与原来 fillPath (panel) 同一路径、同一覆盖）。
                g.setGradientFill (juce::ColourGradient (juce::Colours::white, 0.0f, 0.0f, palette::spectrumFloor, 0.0f, h, false));
                g.fillAll();

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

                // ---- 修坯：低切以下打斜线（修坯此刻实际在处理时才画）----
                // 斜线正好 45°：边表按变换后坐标的浮点舍入选每行采样 1 次还是 2 次，直接画时随面板在窗口里的位置差 ≤ 5 LSB；
                // 画在这张图里与位置无关（见文件头）。
                if (clayActive && clayLowCutHz > 0.0f)
                {
                    juce::Graphics::ScopedSaveState hatchState (g);
                    const auto right = xOf (clayLowCutHz);
                    g.reduceClipRegion (juce::Rectangle<int> (0, 0, (int) std::ceil (right), height));
                    g.setColour (palette::c800.withAlpha (0.16f));

                    for (float x = -h; x < right; x += 5.0f * std::sqrt (2.0f))
                        g.drawLine (x, h, x + h, 0.0f, 1.2f);
                }

                break;
            }

            case Layer::frequencyText:
            {
                g.setFont (monoFont (10.5f));
                g.setColour (palette::ink2);

                for (const auto& [frequency, label] : { std::pair<float, const char*> { 100.0f, "100Hz" }, { 1000.0f, "1kHz" }, { 10000.0f, "10kHz" } })
                    g.drawText (label, frequencyLabelBounds (frequency), juce::Justification::centredLeft, false);

                break;
            }

            case Layer::dbLabels:
            {
                // 白色圆角底框（80 %）+ 字。T-036 起底框也画进缓存（原来每次直接画，为与 T-032 之前逐像素一致）：频谱一秒重画
                // 60 / 120 次，少 6 次圆角矩形；半透明边先画进透明图再贴，个别边缘像素差 1。
                g.setColour (juce::Colours::white.withAlpha (0.8f));

                for (const auto& chip : dbChips)
                    g.fillRoundedRectangle (chip, 3.0f);

                const auto dbFont = monoFont (10.0f);
                g.setFont (dbFont);
                g.setColour (palette::ink2);

                for (float db = -12.0f; db >= -72.0f; db -= 12.0f)
                {
                    const auto label = dbLabelFor (db, dbFont);
                    g.drawText (label.text, label.chip, juce::Justification::centred, false);
                }

                break;
            }

            case Layer::readout:
                // 影子、白底、描边、文字合成一张（T-036）：文字（shownReadout）变了才重画，频谱每帧重画时只贴图。
                g.setColour (palette::shadowTint.withAlpha (0.07f));
                g.fillRoundedRectangle (readoutShadowBounds (readoutBox()), 8.0f);
                g.setColour (juce::Colours::white);
                g.fillRoundedRectangle (readoutBox(), 7.0f);
                g.setColour (palette::controlBorder);
                g.drawRoundedRectangle (readoutBox().reduced (0.5f), 6.5f, 1.0f);
                g.setFont (monoFont (11.5f, Weight::semibold));
                g.setColour (palette::ink);
                g.drawText (shownReadout, readoutBox().reduced (10.0f, 0.0f), juce::Justification::centredLeft, false);
                break;

            case Layer::count:
                break;
        }
    }

    void SpectrumView::prepareLayers (float pixelScale)
    {
        for (size_t index = 0; index < layers.size(); ++index)
        {
            const auto which = (Layer) index;
            auto& cached = layers[index];

            // 显卡设备丢了（Windows 的 Direct2D 图关了备份，见下）：内容没了，当作作废重画。
            if (cached.valid && cached.image.isValid())
                if (auto data = cached.image.getPixelData())
                    if (auto* backup = data->getBackupExtensions())
                        if (backup->needsBackup() && ! backup->canBackup())
                            cached.valid = false;

            if (cached.valid && juce::approximatelyEqual (cached.pixelScale, pixelScale))
                continue;

            // 图覆盖 area 在设备像素上的外接矩形，左上角取整到设备像素：逻辑坐标 q 画在图的 q × 像素比 − origin 处，
            // 与组件自己的设备像素网格同相位（组件原点在整数像素上时，贴回去与直接画逐像素相同）。
            const auto area = layerArea (which);
            const auto x0 = (int) std::floor ((float) area.getX() * pixelScale);
            const auto y0 = (int) std::floor ((float) area.getY() * pixelScale);
            const auto x1 = (int) std::ceil ((float) area.getRight() * pixelScale);
            const auto y1 = (int) std::ceil ((float) area.getBottom() * pixelScale);

            cached.area = area;
            cached.origin = { x0, y0 };
            // 底层不透明，用 RGB 图：JUCE 9 的软件渲染器往 ARGB 上混色按 ÷255 四舍五入，往 RGB 上按 × (256 − α) >> 8，
            // 半透明的网格 / 斜线画进 ARGB 底层再贴到 RGB 的窗口后备图（Linux 的窗口、截图工具都是 RGB）会差 1；画进 RGB 底层
            // 与直接画同一套算式。RGB 贴 RGB 满覆盖的行是整行拷贝，也最快。其余几张有透明背景，只能是 ARGB（它们的元素
            // 是往全透明上画一次，贴回去与直接画相同）。
            // 尺寸与格式没变就复用原图、清成透明再画（读数框随文字变，T-036 起约 0.27 s 一次）；内容随时能重画，关掉备份——
            // Windows 的 Direct2D 图开着备份时，每往图里画一次都要从显存回读一次（同 AlignedComponentCache）。
            const auto format = which == Layer::back ? juce::Image::RGB : juce::Image::ARGB;
            const auto w = juce::jmax (1, x1 - x0), h = juce::jmax (1, y1 - y0);

            if (cached.image.isValid() && cached.image.getWidth() == w && cached.image.getHeight() == h && cached.image.getFormat() == format)
            {
                cached.image.clear (cached.image.getBounds());
            }
            else
            {
                cached.image = juce::Image (format, w, h, true);
                cached.image.setBackupEnabled (false);
            }

            cached.pixelScale = pixelScale;
            cached.valid = true;

            juce::Graphics g (cached.image);
            g.addTransform (juce::AffineTransform::scale (pixelScale).translated ((float) -x0, (float) -y0));
            paintLayer (which, g);
        }
    }

    void SpectrumView::drawLayer (juce::Graphics& g, Layer which)
    {
        const auto& cached = layer (which);

        if (! cached.image.isValid())
            return;

        // 变换只剩平移；低质量插值 = 软件渲染器取最近的整数像素（CoreGraphics / Direct2D 为最近邻）：组件原点落在小数像素上时
        // 整张图吸附到最近的整数像素（≤ 0.5 像素），不做双线性重采样，字与细线不糊。原点在整数像素上时就是逐像素贴。
        juce::Graphics::ScopedSaveState state (g);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.setColour (juce::Colours::black);
        g.drawImageTransformed (cached.image, juce::AffineTransform::translation ((float) cached.origin.x, (float) cached.origin.y)
                                                  .scaled (1.0f / cached.pixelScale));
    }

    void SpectrumView::prepareStripes (float pixelScale, juce::Colour colour)
    {
        if (stripeRow.isValid() && juce::approximatelyEqual (stripeRowScale, pixelScale) && stripeRowColour == colour)
            return;

        // 竖纹瓦片（T-032 R9-04 ①）：一行，宽 = 面板宽 × 像素比（向上取整再多 1 像素）。条纹用与原来逐条 fillRect (x, 0, 0.8, h)
        // 同样的逻辑坐标、同样的像素比与颜色栅格化（相位以组件原点为 0），每个设备像素上的覆盖与颜色都一样；纵向靠平铺。
        // OUT 内部（边缘覆盖满）贴出来与原来逐像素相同；OUT 的抗锯齿边上，瓦片像素（已按条纹覆盖率乘过、8 位舍入）再乘一次
        // 边缘覆盖率，原来是两个覆盖率先相乘再乘颜色，差 ≤ 2 LSB。逐像素相同的写法试过：裁到 OUT 后一次 fillRectList 516 条
        // （一张每行 1032 条边的边表）——×1 9.6 ms / ×4 63 ms，比瓦片（5.0 / 34 ms）慢近一倍，不取。
        // 整面宽的一行而不是 2 逻辑像素宽的小瓦片：任何像素比下条纹间距都正好 2 逻辑像素，不会因取整漂移。
        // 一行高：面板高 × 像素比不是整数时（×1.25 等）最底下那一行原来只被条纹部分覆盖，现在满覆盖（那一行被渐隐盖住，差 ≤ 1）。
        // 先把裁剪区换成边表（整行满覆盖）：原来的条纹是在「面板 ∩ OUT」的边表裁剪区里 fillRect 的，走 EdgeTable (Rectangle<float>)
        // 的抗锯齿；新图默认的矩形列表裁剪区走另一套浮点矩形抗锯齿（FloatRectangleRasterisingInfo），边缘覆盖会差 1。
        stripeRow = juce::Image (juce::Image::ARGB, (int) std::ceil ((float) width * pixelScale) + 1, 1, true);

        {
            juce::Graphics g (stripeRow);
            g.addTransform (juce::AffineTransform::scale (pixelScale));
            juce::Path wholeRow;
            wholeRow.addRectangle (0.0f, 0.0f, (float) width + 4.0f, (float) height);
            g.reduceClipRegion (wholeRow);
            g.setColour (colour);

            for (float x = 0.0f; x < (float) width; x += 2.0f)
                g.fillRect (x, 0.0f, 0.8f, (float) height);
        }

        // T-036：条纹不再单独填（与按响度上色合成一行图，prepareOutputRow），只要每个设备像素列的条纹不透明度——读一次存下，
        // 每帧合成时不再读图（Windows 的 Direct2D 图每次读都要从显存回读）。
        stripeAlpha.resize ((size_t) stripeRow.getWidth());

        {
            const juce::Image::BitmapData data (stripeRow, juce::Image::BitmapData::readOnly);

            for (int x = 0; x < stripeRow.getWidth(); ++x)
                stripeAlpha[(size_t) x] = data.getPixelColour (x, 0).getFloatAlpha();   // 白色，预乘后 r = g = b = a
        }

        stripeRowScale = pixelScale;
        stripeRowColour = colour;
    }

    //==============================================================================
    void SpectrumView::paint (juce::Graphics& g)
    {
        const auto pixelScale = juce::jmax (0.01f, g.getInternalContext().getPhysicalPixelScaleFactor());

        // 读数文字变了：读数框那张缓存图（影子 + 白底 + 描边 + 字）重画。
        if (readoutString != shownReadout)
        {
            shownReadout = readoutString;
            invalidate (Layer::readout);
        }

        prepareLayers (pixelScale);

        if (! curvePathsValid)
        {
            // 在这里重建（换了斜率、曲线静止时）：这一帧画出的区域并进「旧区域」，下一次只重画新旧包围盒时不漏掉它（评审 R-02）。
            buildCurvePaths();
            curveArea = curveArea.getUnion (curveRegion());
        }

        const auto h = (float) height;
        const auto outputAlpha = outputToggle.isOn() ? 1.0f : 0.12f;
        const auto showInput = inputToggle.isOn() && inputReady;

        if (showInput && ! inputDashesValid)
        {
            if (inputLine.isEmpty())
                buildLine (input, inputLine);

            const float dashes[] = { 3.0f, 2.0f };
            inputDashes.clear();
            juce::PathStrokeType (1.1f).createDashedStroke (inputDashes, inputLine, dashes, 2);
            inputDashesValid = true;
        }

        // 哪些整块与这次要重画的区域相交（T-032 R9-04 ③）：在按面板圆角裁剪之前判断——之后裁剪区成了边表，只剩包围盒可比。
        const auto intersects = [&g] (juce::Rectangle<float> area) { return g.clipRegionIntersects (area.getSmallestIntegerContainer()); };
        // 描边超出路径包围盒的上限：尖角接头（JUCE 默认）最多伸出 半宽 × 斜接上限 4 = 2 × 线宽，再加 1 像素抗锯齿。
        const auto strokeArea = [] (const juce::Path& path, float thickness) { return path.getBounds().expanded (2.0f * thickness + 1.0f); };
        const auto drawOutputFill = intersects (outputFill.getBounds());
        const auto drawOutputLine = intersects (strokeArea (outputLine, 0.8f));
        const auto drawAverage = intersects (strokeArea (averageLine, 1.4f));
        const auto drawInput = showInput && intersects (inputDashes.getBounds().expanded (1.0f));
        const auto drawPeak = peakToggle.isOn() && intersects (strokeArea (peakLine, 1.0f));
        const auto drawCursor = hoverFrequency.has_value() && intersects (cursorBounds().toFloat());
        const auto drawFrequencyLabels = intersects (layer (Layer::frequencyText).area.toFloat());
        const auto drawDbLabels = intersects (layer (Layer::dbLabels).area.toFloat());
        const auto drawReadout = intersects (layer (Layer::readout).area.toFloat());

        {
            juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (panel);

            // ---- 底：面板渐变 + 网格（+ 低切以下的斜线），缓存图 ----
            drawLayer (g, Layer::back);

            // ---- 频谱：OUT 填充（按响度上色）+ 竖纹 + 渐隐 + 轮廓 + 平均线 ----
            if (drawOutputFill)
            {
                // 按响度上色 + 白色竖纹：一行图（prepareOutputRow）纵向平铺，一次填进 OUT 的形状。OUT 关着时整体乘 0.12。
                // 图的 1 像素 = 1 / 像素比 个逻辑单位：原点在整数像素上时变换只剩整数平移，逐像素拷贝；落在小数像素上时
                // 与缓存图一样吸附到最近的整数像素（≤ 0.5 像素的相位差），不做双线性重采样（条纹不变淡）。
                prepareOutputRow (pixelScale);
                {
                    juce::Graphics::ScopedSaveState rowState (g);
                    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
                    juce::FillType rowFill (outputRow, juce::AffineTransform::scale (1.0f / pixelScale));
                    rowFill.setOpacity (outputAlpha);
                    g.setFillType (rowFill);
                    g.fillPath (outputFill);
                }

                // 往下渐隐：同样直接填 OUT 的形状。y < 40 处渐隐全透明，先裁掉（曲线很响时少填十几 % 的像素）。
                juce::Graphics::ScopedSaveState fadeState (g);
                g.reduceClipRegion (juce::Rectangle<int> (0, 40, width, height - 40));
                g.setGradientFill (juce::ColourGradient (palette::spectrumFloor.withAlpha (0.0f), 0.0f, 40.0f,
                                                         palette::spectrumFloor.withAlpha (0.92f * outputAlpha), 0.0f, h, false));
                g.fillPath (outputFill);
            }

            if (drawOutputLine)
            {
                g.setColour (palette::c700.withAlpha (0.7f * outputAlpha));
                g.strokePath (outputLine, juce::PathStrokeType (0.8f));
            }

            if (drawAverage)
            {
                g.setColour (juce::Colours::white.withAlpha (0.9f * outputAlpha));
                // 斜角接头、平头：1.4 px 的线、相邻点 2 px，看不出与圆角的差别；软件渲染器每帧省约 0.2 ms（评审 R-07 实测）。
                g.strokePath (averageLine, juce::PathStrokeType (1.4f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
            }

            // ---- IN（虚线）与峰值 ----
            if (drawInput)
            {
                g.setColour (palette::c600.withAlpha (0.75f));
                g.fillPath (inputDashes);
            }

            if (drawPeak)
            {
                g.setColour (palette::c700.withAlpha (0.45f));
                g.strokePath (peakLine, juce::PathStrokeType (1.0f));
            }

            // ---- 修坯的频率标记 ----
            if (clayActive)
            {
                const auto marker = [&] (float frequency, const juce::String& label, float labelTextWidth)
                {
                    if (frequency <= 0.0f)
                        return;

                    // 低于 20 Hz 的低切（自动低切常在 16 Hz 附近）画不出竖线：只在左边缘（避开 dB 刻度）写标签。
                    const auto inRange = frequency >= SpectrumAnalyser::minFrequency;
                    const auto x = std::round (xOf (juce::jmax (frequency, SpectrumAnalyser::minFrequency))) + 0.5f;
                    const juce::Rectangle<float> chip (juce::jmax (x + 3.5f, 38.0f), 46.0f, labelTextWidth + 10.0f, 15.0f);

                    if (! intersects (chip.getUnion (juce::Rectangle<float> (x - 1.0f, 40.0f, 2.0f, h - 40.0f))))
                        return;

                    const float dashes[] = { 3.0f, 3.0f };

                    if (inRange)
                    {
                        g.setColour (palette::c800.withAlpha (0.45f));
                        g.drawDashedLine (juce::Line<float> (x, 40.0f, x, h), dashes, 2, 1.0f);
                    }

                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.fillRoundedRectangle (chip, 4.0f);
                    g.setFont (uiFont (10.0f));
                    g.setColour (palette::c800);
                    g.drawText (label, chip, juce::Justification::centred, false);
                };

                marker (clayLowCutHz, lowCutLabel, lowCutLabelWidth);
                marker (clayMonoHz, monoLabel, monoLabelWidth);
            }

            // ---- 悬停光标 ----
            if (drawCursor)
            {
                const auto x = xOf (*hoverFrequency);
                const float dashes[] = { 2.0f, 3.0f };
                g.setColour (palette::c900.withAlpha (0.45f));
                g.drawDashedLine (juce::Line<float> (x, 0.0f, x, h), dashes, 2, 1.0f);

                const auto db = curveDbAt (output, *hoverFrequency) + tiltAt (*hoverFrequency);
                const auto dot = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x, yOf (db) });
                g.setColour (juce::Colours::white);
                g.fillEllipse (dot);
                g.setColour (palette::c900);
                g.drawEllipse (dot, 1.6f);
            }

            // ---- 刻度：频率字、dB 底框 + 字各一张缓存图 ----
            if (drawFrequencyLabels)
                drawLayer (g, Layer::frequencyText);

            if (drawDbLabels)
                drawLayer (g, Layer::dbLabels);

            // ---- 读数框：影子、白底、描边、文字一张缓存图（文字变了才重画，见开头）----
            if (drawReadout)
                drawLayer (g, Layer::readout);
        }

        g.setColour (palette::panelBorder);
        g.strokePath (panel, juce::PathStrokeType (1.0f));
    }

    void SpectrumView::mouseMove (const juce::MouseEvent& e)
    {
        const auto u = juce::jlimit (0.0f, 1.0f, e.position.x / (float) width);
        setHoverFrequency (SpectrumAnalyser::minFrequency * std::pow (SpectrumAnalyser::maxFrequency / SpectrumAnalyser::minFrequency, u));
    }

    void SpectrumView::mouseExit (const juce::MouseEvent&)
    {
        setHoverFrequency (std::nullopt);
    }
}
