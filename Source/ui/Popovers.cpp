/*
  ==============================================================================

    Popovers.cpp

  ==============================================================================
*/

#include "Popovers.h"

#include "CeladonLookAndFeel.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr double minZoom = 0.7, maxZoom = 2.0;
        constexpr float padding = 16.0f;

        juce::String percentText (double zoom)
        {
            return juce::String (juce::roundToInt (zoom * 100.0)) + "%";
        }

        juce::String pixelText (double zoom)
        {
            return juce::String (juce::roundToInt (1200.0 * zoom)) + juce::String::fromUTF8 (" × ") + juce::String (juce::roundToInt (820.0 * zoom));
        }
    }

    //==============================================================================
    juce::Rectangle<float> PopoverCard::getCardBounds() const
    {
        return getLocalBounds().toFloat().reduced ((float) shadowMargin);
    }

    bool PopoverCard::hitTest (int x, int y)
    {
        return getCardBounds().contains ((float) x, (float) y);
    }

    void PopoverCard::paint (juce::Graphics& g)
    {
        const auto card = getCardBounds();

        // 阴影：往下偏 18 px 的一大片柔和暗影（设计稿 0 18px 50px rgba(13,40,48,.18)），用几层渐淡的圆角矩形近似。
        for (int i = 6; i >= 1; --i)
        {
            const auto spread = (float) i * 4.0f;
            g.setColour (palette::shadowTint.withAlpha (0.028f));
            g.fillRoundedRectangle (card.translated (0.0f, 10.0f).expanded (spread * 0.6f, spread * 0.45f), 14.0f + spread * 0.5f);
        }

        g.setColour (palette::panel);
        g.fillRoundedRectangle (card, 14.0f);
        g.setColour (palette::controlBorder);
        g.drawRoundedRectangle (card.reduced (0.5f), 13.5f, 1.0f);
    }

    juce::Rectangle<float> PopoverCard::closeButtonBounds() const
    {
        const auto card = getCardBounds();
        return { card.getRight() - padding - 24.0f, card.getY() + 12.0f, 24.0f, 24.0f };
    }

    void PopoverCard::paintCloseButton (juce::Graphics& g) const
    {
        const auto area = closeButtonBounds();
        g.setColour (palette::groove);
        g.fillEllipse (area);

        const auto centre = area.getCentre();
        g.setColour (palette::ink2);
        g.drawLine (centre.x - 3.0f, centre.y - 3.0f, centre.x + 3.0f, centre.y + 3.0f, 1.5f);
        g.drawLine (centre.x + 3.0f, centre.y - 3.0f, centre.x - 3.0f, centre.y + 3.0f, 1.5f);
    }

    bool PopoverCard::isOnCloseButton (juce::Point<float> position) const
    {
        return closeButtonBounds().expanded (3.0f).contains (position);
    }

    //==============================================================================
    SettingsPanel::SettingsPanel()
    {
        setSize (cardWidth + 2 * shadowMargin, cardHeight + 2 * shadowMargin);
    }

    void SettingsPanel::setLanguage (Language newLanguage)   { language = newLanguage; repaint(); }
    void SettingsPanel::setReduceMotion (bool shouldReduce)  { reduceMotion = shouldReduce; repaint(); }
    void SettingsPanel::setScreenSize (juce::Rectangle<int> area)   { if (! area.isEmpty()) screen = area; repaint(); }

    void SettingsPanel::setZoom (double newZoom)
    {
        if (std::abs (newZoom - zoom) < 1.0e-6)
            return;

        zoom = newZoom;
        repaint();
    }

    void SettingsPanel::setLatency (double milliseconds)
    {
        if (std::abs (milliseconds - latencyMs) < 0.05)
            return;

        latencyMs = milliseconds;
        repaint();
    }

    juce::Rectangle<float> SettingsPanel::presetBounds (int index) const
    {
        const auto card = getCardBounds();
        const auto w = (card.getWidth() - 2.0f * padding - 4.0f * 4.0f) / 5.0f;
        return { card.getX() + padding + (float) index * (w + 4.0f), card.getY() + 64.0f, w, 26.0f };
    }

    juce::Rectangle<float> SettingsPanel::sliderBounds() const
    {
        const auto card = getCardBounds();
        return { card.getX() + padding, card.getY() + 100.0f, card.getWidth() - 2.0f * padding, 18.0f };
    }

    juce::Rectangle<float> SettingsPanel::switchBounds() const
    {
        const auto card = getCardBounds();
        return { card.getRight() - padding - 38.0f, card.getY() + 352.0f, 38.0f, 22.0f };
    }

    double SettingsPanel::zoomAt (float x) const
    {
        const auto slider = sliderBounds();
        const auto proportion = juce::jlimit (0.0, 1.0, (double) ((x - slider.getX()) / slider.getWidth()));
        return std::round ((minZoom + proportion * (maxZoom - minZoom)) * 100.0) / 100.0;
    }

    void SettingsPanel::paint (juce::Graphics& g)
    {
        PopoverCard::paint (g);

        const auto card = getCardBounds();
        const auto left = card.getX() + padding, right = card.getRight() - padding;
        const auto innerWidth = right - left;

        // 标题 + 关闭
        g.setFont (uiFont (13.0f, Weight::semibold));
        g.setColour (palette::ink);
        g.drawText (tr (Text::settings, language), juce::Rectangle<float> (left, card.getY() + 12.0f, 200.0f, 24.0f), juce::Justification::centredLeft, false);
        paintCloseButton (g);

        // 界面大小：125%  1500 × 1025
        {
            const auto y = card.getY() + 42.0f;
            g.setFont (uiFont (12.0f, Weight::medium));
            g.drawText (tr (Text::windowSize, language), juce::Rectangle<float> (left, y, 150.0f, 16.0f), juce::Justification::centredLeft, false);

            const auto pxFont = monoFont (11.0f);
            const auto px = pixelText (zoom);
            const auto pxWidth = textWidth (pxFont, px);
            g.setFont (pxFont);
            g.setColour (palette::ink3);
            g.drawText (px, juce::Rectangle<float> (right - pxWidth, y, pxWidth + 1.0f, 16.0f), juce::Justification::centredRight, false);
            g.setFont (monoFont (12.0f, Weight::semibold));
            g.setColour (palette::ink);
            g.drawText (percentText (zoom), juce::Rectangle<float> (right - pxWidth - 8.0f - 60.0f, y, 60.0f, 16.0f), juce::Justification::centredRight, false);
        }

        // 五个档位
        for (int i = 0; i < (int) std::size (presets); ++i)
        {
            const auto area = presetBounds (i);
            const auto on = std::abs (zoom - presets[i]) < 1.0e-3;
            g.setColour (on ? palette::c700 : palette::panel);
            g.fillRoundedRectangle (area, 6.0f);
            g.setColour (on ? palette::c700 : palette::controlBorder);
            g.drawRoundedRectangle (area.reduced (0.5f), 5.5f, 1.0f);
            g.setFont (monoFont (11.0f, Weight::medium));
            g.setColour (on ? juce::Colours::white : palette::ink);
            g.drawText (percentText (presets[i]), area, juce::Justification::centred, false);
        }

        // 滑条 70 % ～ 200 %
        {
            const auto slider = sliderBounds();
            const auto bar = juce::Rectangle<float> (slider.getX(), slider.getY() + 7.0f, slider.getWidth(), 4.0f);
            const auto proportion = (float) juce::jlimit (0.0, 1.0, (zoom - minZoom) / (maxZoom - minZoom));
            g.setColour (palette::track);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setGradientFill (juce::ColourGradient (palette::c200, bar.getX(), 0.0f, palette::c700, bar.getRight(), 0.0f, false));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * proportion), 2.0f);

            const auto thumb = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ bar.getX() + bar.getWidth() * proportion, bar.getCentreY() });
            g.setColour (juce::Colours::white);
            g.fillEllipse (thumb);
            g.setColour (palette::c700);
            g.drawEllipse (thumb.reduced (1.0f), 2.0f);

            g.setFont (monoFont (10.0f));
            g.setColour (palette::ink3);
            g.drawText ("70%", juce::Rectangle<float> (left, slider.getBottom(), 40.0f, 14.0f), juce::Justification::centredLeft, false);
            g.drawText ("200%", juce::Rectangle<float> (right - 40.0f, slider.getBottom(), 40.0f, 14.0f), juce::Justification::centredRight, false);
        }

        // 屏幕预览：按屏幕比例缩进 256 × 144 的框，窗口按当前大小画进去；比屏幕大时变橙色并提醒。
        {
            const juce::Rectangle<float> frame (left + (innerWidth - 256.0f) * 0.5f, card.getY() + 140.0f, 256.0f, 144.0f);
            const auto scale = juce::jmin (frame.getWidth() / (float) screen.getWidth(), frame.getHeight() / (float) screen.getHeight());
            const auto screenRect = juce::Rectangle<float> ((float) screen.getWidth() * scale, (float) screen.getHeight() * scale).withCentre (frame.getCentre());
            const auto windowSize = juce::Point<float> (1200.0f * (float) zoom * scale, 820.0f * (float) zoom * scale);
            const auto over = 1200.0 * zoom > (double) screen.getWidth() || 820.0 * zoom > (double) screen.getHeight();

            g.setColour (palette::spectrumFloor);
            g.fillRoundedRectangle (screenRect, 4.0f);
            g.setColour (palette::divider);
            g.drawRoundedRectangle (screenRect.reduced (0.5f), 4.0f, 1.0f);

            {
                juce::Graphics::ScopedSaveState saved (g);
                g.reduceClipRegion (screenRect.toNearestInt());
                const auto window = juce::Rectangle<float> (windowSize.x, windowSize.y).withCentre (screenRect.getCentre());
                const auto colour = over ? palette::warning : palette::c700;
                g.setColour (palette::c100);
                g.fillRoundedRectangle (window, 2.0f);
                g.setColour (colour.withAlpha (0.35f));
                g.fillRect (window.withHeight (juce::jmin (window.getHeight(), 5.0f * (float) zoom)));
                g.setColour (colour);
                g.drawRoundedRectangle (window, 2.0f, 1.5f);
            }

            g.setFont (uiFont (10.0f));
            g.setColour (palette::ink3);
            g.drawText (trFormat (Text::screenSize, language, { juce::String (screen.getWidth()), juce::String (screen.getHeight()) }),
                        juce::Rectangle<float> (screenRect.getX() + 6.0f, screenRect.getY() + 5.0f, 160.0f, 12.0f), juce::Justification::centredLeft, false);

            if (over)
            {
                g.setFont (uiFont (10.0f, Weight::medium));
                g.setColour (palette::warningText);
                g.drawText (tr (Text::tooBig, language), juce::Rectangle<float> (screenRect.getRight() - 106.0f, screenRect.getY() + 5.0f, 100.0f, 12.0f),
                            juce::Justification::centredRight, false);
            }
        }

        // 说明（可换行）
        {
            juce::AttributedString note;
            note.setWordWrap (juce::AttributedString::byWord);
            note.setLineSpacing (3.0f);
            note.append (tr (Text::sizeNote, language), uiFont (11.0f), palette::ink2);
            juce::TextLayout layout;
            layout.createLayout (note, innerWidth);
            layout.draw (g, juce::Rectangle<float> (left, card.getY() + 292.0f, innerWidth, 40.0f));
        }

        g.setColour (palette::track);
        g.fillRect (left, card.getY() + 340.0f, innerWidth, 1.0f);

        // 减少动效 + 开关
        {
            const auto y = card.getY() + 348.0f;
            g.setFont (uiFont (12.0f, Weight::medium));
            g.setColour (palette::ink);
            g.drawText (tr (Text::reduceMotion, language), juce::Rectangle<float> (left, y, 220.0f, 16.0f), juce::Justification::centredLeft, false);
            g.setFont (uiFont (11.0f));
            g.setColour (palette::ink3);
            g.drawText (tr (Text::reduceMotionNote, language), juce::Rectangle<float> (left, y + 16.0f, innerWidth - 50.0f, 14.0f),
                        juce::Justification::centredLeft, true);

            const auto area = switchBounds();
            g.setColour (reduceMotion ? palette::c700 : palette::divider);
            g.fillRoundedRectangle (area, 11.0f);
            const auto knob = juce::Rectangle<float> (16.0f, 16.0f).withPosition (area.getX() + (reduceMotion ? 19.0f : 3.0f), area.getY() + 3.0f);
            g.setColour (palette::shadowTint.withAlpha (0.2f));
            g.fillEllipse (knob.translated (0.0f, 0.8f));
            g.setColour (juce::Colours::white);
            g.fillEllipse (knob);
        }

        g.setColour (palette::track);
        g.fillRect (left, card.getY() + 390.0f, innerWidth, 1.0f);

        // 版本 · 延迟
        {
            const auto y = card.getY() + 398.0f;
            g.setFont (monoFont (10.5f));
            g.setColour (palette::ink3);
            g.drawText (tr (Text::version, language) + " " + juce::String (JucePlugin_VersionString),
                        juce::Rectangle<float> (left, y, 160.0f, 14.0f), juce::Justification::centredLeft, false);
            g.drawText (tr (Text::latency, language) + " " + juce::String (latencyMs, 1) + " ms",
                        juce::Rectangle<float> (right - 160.0f, y, 160.0f, 14.0f), juce::Justification::centredRight, false);
        }
    }

    void SettingsPanel::mouseDown (const juce::MouseEvent& e)
    {
        if (isOnCloseButton (e.position))
        {
            if (onClose != nullptr)
                onClose();

            return;
        }

        for (int i = 0; i < (int) std::size (presets); ++i)
            if (presetBounds (i).contains (e.position))
            {
                if (onZoomChange != nullptr)
                    onZoomChange (presets[i]);

                return;
            }

        if (sliderBounds().expanded (0.0f, 4.0f).contains (e.position))
        {
            draggingSlider = true;

            if (onZoomDragging != nullptr)
                onZoomDragging (true);

            if (onZoomChange != nullptr)
                onZoomChange (zoomAt (e.position.x));

            return;
        }

        if (switchBounds().expanded (4.0f).contains (e.position))
        {
            if (onReduceMotionChange != nullptr)
                onReduceMotionChange (! reduceMotion);
        }
    }

    void SettingsPanel::mouseDrag (const juce::MouseEvent& e)
    {
        // 缩放会让面板自己跟着变大变小：按屏幕坐标换回面板坐标，拖起来才不会「越拖越远」。
        if (draggingSlider && onZoomChange != nullptr)
            onZoomChange (zoomAt (getLocalPoint (nullptr, e.getScreenPosition()).toFloat().x));
    }

    void SettingsPanel::mouseUp (const juce::MouseEvent&)
    {
        if (draggingSlider)
        {
            draggingSlider = false;

            if (onZoomDragging != nullptr)
                onZoomDragging (false);
        }
    }

    //==============================================================================
    HelpPanel::HelpPanel()
    {
        layoutText();
    }

    void HelpPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        layoutText();
        repaint();
    }

    void HelpPanel::layoutText()
    {
        juce::AttributedString text;
        text.setWordWrap (juce::AttributedString::byWord);
        text.setLineSpacing (5.0f);

        const auto bullet = juce::String::fromUTF8 ("•  ");
        const Text lines[] = { Text::helpDrag, Text::helpFine, Text::helpReset, Text::helpType, Text::helpWheel, Text::helpColours, Text::helpAb };

        for (size_t i = 0; i < std::size (lines); ++i)
        {
            if (i > 0)
                text.append ("\n", uiFont (11.5f), palette::ink2);

            // 操作手法与「颜色 / A-B」两组之间空半行。
            if (i == 5)
                text.append ("\n", uiFont (5.0f), palette::ink2);

            text.append (bullet, uiFont (11.5f), palette::c600);
            text.append (tr (lines[i], language), uiFont (11.5f), palette::ink2);
        }

        layout.createLayout (text, (float) cardWidth - 2.0f * padding);
        cardHeight = (int) std::ceil (48.0f + layout.getHeight() + padding);
        setSize (cardWidth + 2 * shadowMargin, cardHeight + 2 * shadowMargin);
    }

    void HelpPanel::paint (juce::Graphics& g)
    {
        PopoverCard::paint (g);

        const auto card = getCardBounds();
        g.setFont (uiFont (13.0f, Weight::semibold));
        g.setColour (palette::ink);
        g.drawText (tr (Text::helpTitle, language), juce::Rectangle<float> (card.getX() + padding, card.getY() + 12.0f, 200.0f, 24.0f),
                    juce::Justification::centredLeft, false);
        paintCloseButton (g);

        layout.draw (g, juce::Rectangle<float> (card.getX() + padding, card.getY() + 46.0f, card.getWidth() - 2.0f * padding, layout.getHeight()));
    }

    void HelpPanel::mouseUp (const juce::MouseEvent& e)
    {
        if (isOnCloseButton (e.position) && onClose != nullptr)
            onClose();
    }

    //==============================================================================
    ValueBubble::ValueBubble()
    {
        setInterceptsMouseClicks (false, false);
    }

    void ValueBubble::show (const juce::String& newText, bool isFine, const juce::String& newFineLabel)
    {
        if (newText == text && isFine == fine && newFineLabel == fineLabel)
            return;

        text = newText;
        fine = isFine;
        fineLabel = newFineLabel;
        repaint();
    }

    int ValueBubble::getPreferredWidth() const
    {
        auto w = 9.0f + textWidth (monoFont (12.0f), text) + 9.0f;

        if (fine)
            w += 6.0f + textWidth (uiFont (10.0f), fineLabel) + 10.0f;

        return (int) std::ceil (w);
    }

    void ValueBubble::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (palette::ink);
        g.fillRoundedRectangle (bounds, 7.0f);

        const auto font = monoFont (12.0f);
        g.setFont (font);
        g.setColour (juce::Colours::white);
        g.drawText (text, juce::Rectangle<float> (9.0f, 0.0f, textWidth (font, text) + 2.0f, bounds.getHeight()), juce::Justification::centredLeft, false);

        if (fine)
        {
            const auto labelFont = uiFont (10.0f);
            const juce::Rectangle<float> chip (9.0f + textWidth (font, text) + 6.0f, (bounds.getHeight() - 16.0f) * 0.5f,
                                               textWidth (labelFont, fineLabel) + 10.0f, 16.0f);
            g.setColour (palette::c400);
            g.fillRoundedRectangle (chip, 4.0f);
            g.setFont (labelFont);
            g.setColour (palette::c900);
            g.drawText (fineLabel, chip, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    ResizeHud::ResizeHud()
    {
        setInterceptsMouseClicks (false, false);
        setSize (240, 120);
    }

    void ResizeHud::setValues (const juce::String& newTitle, double newZoom)
    {
        title = newTitle;
        zoom = newZoom;
        repaint();
    }

    void ResizeHud::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (13, 27, 33).withAlpha (0.88f));
        g.fillRoundedRectangle (bounds, 16.0f);

        g.setFont (uiFont (12.0f));
        g.setColour (palette::c200);
        g.drawText (title, juce::Rectangle<float> (0.0f, 22.0f, bounds.getWidth(), 16.0f), juce::Justification::centred, false);

        g.setFont (uiFont (36.0f, Weight::light));
        g.setColour (juce::Colours::white);
        g.drawText (percentText (zoom), juce::Rectangle<float> (0.0f, 38.0f, bounds.getWidth(), 42.0f), juce::Justification::centred, false);

        g.setFont (monoFont (12.0f));
        g.setColour (palette::c100);
        g.drawText (pixelText (zoom) + " px", juce::Rectangle<float> (0.0f, 82.0f, bounds.getWidth(), 16.0f), juce::Justification::centred, false);
    }
}
