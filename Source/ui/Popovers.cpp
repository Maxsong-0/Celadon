/*
  ==============================================================================

    Popovers.cpp

  ==============================================================================
*/

#include "Popovers.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"
#include "../ParameterIds.h"

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

    juce::Font SettingsPanel::reduceMotionNoteFont()   { return uiFont (11.0f); }
    float SettingsPanel::reduceMotionNoteWidth()       { return (float) cardWidth - 2.0f * padding - 50.0f; }   // 内容宽减去开关那 50

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
            g.setFont (reduceMotionNoteFont());
            g.setColour (palette::ink3);
            g.drawText (tr (Text::reduceMotionNote, language), juce::Rectangle<float> (left, y + 16.0f, reduceMotionNoteWidth(), 14.0f),
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
        const Text lines[] = { Text::helpDrag, Text::helpFine, Text::helpReset, Text::helpType, Text::helpWheel, Text::helpColours, Text::helpAb,
                               Text::helpAudition };

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
    namespace
    {
        juce::RangedAudioParameter& advancedParameter (juce::AudioProcessorValueTreeState& state, const char* id)
        {
            auto* parameter = state.getParameter (id);
            jassert (parameter != nullptr);
            return *parameter;
        }

        float finiteReading (float value) noexcept
        {
            return std::isfinite (value) ? value : 0.0f;
        }

        void drawAdvancedNote (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                               juce::Colour colour = palette::ink2, float size = 12.5f)
        {
            juce::AttributedString attributed;
            attributed.setWordWrap (juce::AttributedString::byWord);
            attributed.setLineSpacing (3.0f);
            attributed.append (text, uiFont (size), colour);
            juce::TextLayout layout;
            layout.createLayout (attributed, area.getWidth());
            layout.draw (g, area);
        }
    }

    AdvancedPanel::AdvancedPanel (juce::AudioProcessorValueTreeState& state, InteractionHost& host)
        : harshAmount (advancedParameter (state, param::masterHarshAmount), host, false),
          transientPreserve (advancedParameter (state, param::masterTransient), host, false),
          targetAmount (advancedParameter (state, param::masterTargetAmount), host, false),
          codecMargin (advancedParameter (state, param::masterCodecMarginDb), host, false),
          previewLufs (advancedParameter (state, param::masterPreviewLufs), host, false)
    {
        previewLufs.setValueWidth (86);
        tabs.setComponentID (uiid::masterTabs);
        harshMode.setComponentID (param::masterHarshMode);
        harshAmount.setComponentID (param::masterHarshAmount);
        transientPreserve.setComponentID (param::masterTransient);
        targetMode.setComponentID (param::masterTargetMode);
        targetAmount.setComponentID (param::masterTargetAmount);
        codecMargin.setComponentID (param::masterCodecMarginDb);
        dither.setComponentID (param::masterDither);
        previewLufs.setComponentID (param::masterPreviewLufs);
        targetCurve.setComponentID (uiid::masterCurve);
        learn.setComponentID (uiid::masterLearn);
        cancel.setComponentID (uiid::masterCancel);
        clear.setComponentID (uiid::masterClear);
        preview.setComponentID (uiid::masterPreview);

        juce::Component* controls[] = { &tabs, &harshMode, &harshAmount, &transientPreserve, &targetMode, &targetAmount,
                                       &codecMargin, &dither, &previewLufs, &targetCurve, &learn, &cancel, &clear, &preview };
        for (auto* control : controls)
            addAndMakeVisible (*control);

        harshMode.setOptions ({ "", "", "" });
        targetMode.setOptions ({ "", "", "", "", "", "" });
        dither.setOptions ({ "", "", "" });
        harshMode.setSelectedColours (0, { palette::violetSoft, palette::violet });
        targetMode.setSelectedColours (1, { palette::violetSoft, palette::violet });
        tabs.setFont (uiFont (13.0f, Weight::medium));
        harshMode.setFont (uiFont (12.5f, Weight::medium));
        targetMode.setFont (uiFont (12.5f, Weight::medium));
        dither.setFont (uiFont (12.5f, Weight::medium));

        harshAttachment = std::make_unique<SegmentedAttachment> (harshMode, advancedParameter (state, param::masterHarshMode));
        targetAttachment = std::make_unique<SegmentedAttachment> (targetMode, advancedParameter (state, param::masterTargetMode));
        ditherAttachment = std::make_unique<SegmentedAttachment> (dither, advancedParameter (state, param::masterDither));
        harshAttachment->onChange = [this] (int) { refreshControls(); repaint(); };
        targetAttachment->onChange = [this] (int) { refreshControls(); repaint(); };
        tabs.onSelect = [this] (int index) { page = index; refreshControls(); repaint(); };
        learn.onClick = [this] { if (onLearnReference != nullptr) onLearnReference(); };
        cancel.onClick = [this] { if (onCancelReference != nullptr) onCancelReference(); };
        clear.onClick = [this] { if (onClearReference != nullptr) onClearReference(); };
        preview.onClick = [this] { if (onPreviewChange != nullptr) onPreviewChange (! status.previewEnabled); };

        setLanguage (host.getLanguage());
        setSize (cardWidth + 2 * shadowMargin, cardHeight + 2 * shadowMargin);
    }

    void AdvancedPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        tabs.setOptions ({ tr (Text::subprojectLoudNotHarsh, language), tr (Text::subprojectTargetSpectrum, language),
                           tr (Text::subprojectReleaseSafety, language) });
        harshMode.setOptions ({ tr (Text::modeAuto, language), tr (Text::modeOff, language), tr (Text::modeManual, language) });
        targetMode.setOptions ({ tr (Text::modeOff, language), tr (Text::modeAuto, language), tr (Text::targetWarm, language),
                                tr (Text::targetNeutral, language), tr (Text::targetBright, language), tr (Text::targetReference, language) });
        dither.setOptions ({ tr (Text::modeOff, language), "16-bit", "24-bit" });
        harshAmount.setLabel (tr (Text::harshStrength, language));
        transientPreserve.setLabel (tr (Text::transientPreserve, language));
        targetAmount.setLabel (tr (Text::targetStrength, language));
        codecMargin.setLabel (tr (Text::codecMargin, language));
        previewLufs.setLabel (tr (Text::previewLevel, language));
        learn.setText (tr (Text::referenceLearn, language));
        cancel.setText (tr (Text::referenceCancel, language));
        clear.setText (tr (Text::referenceClear, language));
        targetCurve.setLanguage (language);

        juce::Component* controls[] = { &tabs, &harshMode, &harshAmount, &transientPreserve, &targetMode, &targetAmount,
                                       &codecMargin, &dither, &previewLufs, &targetCurve, &learn, &cancel, &clear, &preview };
        for (auto* control : controls)
            if (auto* tooltip = dynamic_cast<juce::SettableTooltipClient*> (control))
                tooltip->setTooltip (help::tooltipFor (control->getComponentID(), language));

        refreshControls();
        repaint();
    }

    void AdvancedPanel::setStatus (const celadon::MasteringStatus& newStatus)
    {
        // 面板多数时候是关着的：只记下，不拼读数、不量曲线；打开时 visibilityChanged 按最新状态一次刷新（T-032 R9-12）。
        if (! isVisible())
        {
            status = newStatus;
            return;
        }

        const auto oldReferenceText = referenceStatusText();
        const bool readingsChanged = juce::String (finiteReading (status.harshReductionDb), 1) != juce::String (finiteReading (newStatus.harshReductionDb), 1)
                                  || juce::String (finiteReading (status.previewGainDb), 1) != juce::String (finiteReading (newStatus.previewGainDb), 1);
        const bool controlsChanged = status.hasReference != newStatus.hasReference || status.referenceState != newStatus.referenceState
                                  || status.previewEnabled != newStatus.previewEnabled || status.sidechainConnected != newStatus.sidechainConnected;
        status = newStatus;
        targetCurve.setValues (status.targetCorrectionDb);
        if (controlsChanged)
            refreshControls();
        if (readingsChanged || controlsChanged || oldReferenceText != referenceStatusText())
            repaint();
    }

    void AdvancedPanel::visibilityChanged()
    {
        if (! isVisible())
            return;

        // 关着时攒下的状态（学习进度、预听、参考谱修正曲线……）：控件、曲线与读数一次刷新到最新。
        targetCurve.setValues (status.targetCorrectionDb);
        refreshControls();
        repaint();
    }

    void AdvancedPanel::refreshControls()
    {
        for (auto* control : std::initializer_list<juce::Component*> { &harshMode, &harshAmount, &transientPreserve })
            control->setVisible (page == 0);
        for (auto* control : std::initializer_list<juce::Component*> { &targetMode, &targetAmount, &targetCurve, &learn, &cancel, &clear })
            control->setVisible (page == 1);
        for (auto* control : std::initializer_list<juce::Component*> { &codecMargin, &dither, &previewLufs, &preview })
            control->setVisible (page == 2);

        const auto harshActive = harshMode.getSelected() != (int) celadon::HarshMode::off;
        harshAmount.setEnabled (harshActive);
        transientPreserve.setEnabled (harshActive);
        harshAmount.setAlpha (harshActive ? 1.0f : 0.4f);
        transientPreserve.setAlpha (harshActive ? 1.0f : 0.4f);
        const auto targetActive = targetMode.getSelected() != (int) celadon::TargetMode::off;
        targetAmount.setEnabled (targetActive);
        targetAmount.setAlpha (targetActive ? 1.0f : 0.4f);

        const auto learning = status.referenceState == celadon::ReferenceState::learning
                           || status.referenceState == celadon::ReferenceState::waitingForSidechain;
        learn.setEnabled (! learning);
        learn.setAlpha (learning ? 0.4f : 1.0f);
        cancel.setEnabled (learning);
        cancel.setAlpha (learning ? 1.0f : 0.4f);
        clear.setEnabled (status.hasReference && ! learning);
        clear.setAlpha (clear.isEnabled() ? 1.0f : 0.4f);
        preview.setText (tr (status.previewEnabled ? Text::previewStop : Text::previewStart, language));
        preview.setLook (status.previewEnabled ? PillButton::Look::normal : PillButton::Look::off);
    }

    juce::String AdvancedPanel::referenceStatusText() const
    {
        if (status.referenceState == celadon::ReferenceState::waitingForSidechain)
            return tr (Text::referenceWaiting, language);
        if (status.referenceState == celadon::ReferenceState::learning)
        {
            const auto seconds = std::isfinite (status.referenceSeconds) ? juce::jmax (0.0, status.referenceSeconds) : 0.0;
            return trFormat (Text::referenceLearning, language, { juce::String (seconds, 1) });
        }
        return tr (status.hasReference ? Text::referenceReady : Text::referenceIdle, language);
    }

    void AdvancedPanel::resized()
    {
        const auto card = getCardBounds().toNearestInt();
        const auto x = card.getX() + 22, y = card.getY(), width = card.getWidth() - 44;
        tabs.setBounds (x, y + 62, width, 34);
        harshMode.setBounds (x, y + 166, 300, 34);
        harshAmount.setBounds (x, y + 226, width, 30);
        transientPreserve.setBounds (x, y + 270, width, 30);
        targetMode.setBounds (x, y + 130, width, 32);
        targetAmount.setBounds (x, y + 173, width, 28);
        targetCurve.setBounds (x, y + 228, width, 72);
        learn.setBounds (x, y + 368, 168, 30);
        cancel.setBounds (x + 180, y + 368, 154, 30);
        clear.setBounds (x + 346, y + 368, 154, 30);
        codecMargin.setBounds (x, y + 112, width, 30);
        dither.setBounds (x + 250, y + 199, width - 250, 30);
        previewLufs.setBounds (x, y + 296, width - 232, 30);
        preview.setBounds (x + width - 220, y + 295, 220, 32);
    }

    void AdvancedPanel::paint (juce::Graphics& g)
    {
        PopoverCard::paint (g);
        const auto card = getCardBounds();
        const auto left = card.getX() + 22.0f, top = card.getY(), width = card.getWidth() - 44.0f;
        const auto area = [=] (float y, float height) { return juce::Rectangle<float> (left, top + y, width, height); };
        g.setColour (palette::ink);
        g.setFont (uiFont (15.0f, Weight::semibold));
        g.drawText (tr (Text::advanced, language), area (13.0f, 23.0f), juce::Justification::centredLeft, false);
        paintCloseButton (g);
        drawAdvancedNote (g, tr (Text::advancedSubtitle, language), area (38.0f, 20.0f), palette::ink3, 11.5f);

        if (page == 0)
        {
            drawAdvancedNote (g, tr (Text::harshNote, language), area (117.0f, 36.0f), palette::ink, 14.0f);
            g.setFont (monoFont (12.0f, Weight::medium));
            g.setColour (palette::c700);
            g.drawText (trFormat (Text::harshReduction, language, { juce::String (juce::jmax (0.0f, finiteReading (status.harshReductionDb)), 1) }),
                        area (174.0f, 20.0f).withTrimmedLeft (326.0f), juce::Justification::centredRight, false);
            const auto note = harshMode.getSelected() == (int) celadon::HarshMode::automatic ? Text::harshAutoNote
                            : harshMode.getSelected() == (int) celadon::HarshMode::off ? Text::harshOffNote : Text::harshManualNote;
            g.setColour (palette::spectrumFloor);
            g.fillRoundedRectangle (area (325.0f, 62.0f), 8.0f);
            drawAdvancedNote (g, tr (note, language), area (339.0f, 40.0f).reduced (14.0f, 0.0f));
        }
        else if (page == 1)
        {
            drawAdvancedNote (g, tr (Text::targetNote, language), area (108.0f, 20.0f));
            g.setFont (uiFont (11.5f, Weight::medium));
            g.setColour (palette::ink3);
            g.drawText (tr (Text::targetCurveTitle, language), area (207.0f, 19.0f), juce::Justification::centredLeft, false);
            g.setColour (palette::c700);
            g.setFont (uiFont (12.5f, Weight::medium));
            g.drawText (referenceStatusText(), area (308.0f, 22.0f), juce::Justification::centredLeft, true);
            drawAdvancedNote (g, tr (Text::referenceRouting, language), area (334.0f, 32.0f), palette::ink3, 11.5f);
        }
        else
        {
            drawAdvancedNote (g, tr (Text::codecNote, language), area (151.0f, 34.0f), palette::ink3, 12.0f);
            g.setColour (palette::divider);
            g.drawHorizontalLine ((int) (top + 190.0f), left, left + width);
            g.setColour (palette::ink);
            g.setFont (uiFont (12.5f, Weight::medium));
            g.drawText (tr (Text::ditherLabel, language), area (202.0f, 23.0f).withWidth (240.0f), juce::Justification::centredLeft, false);
            drawAdvancedNote (g, tr (Text::ditherNote, language), area (240.0f, 35.0f), palette::ink3, 12.0f);
            g.setColour (palette::divider);
            g.drawHorizontalLine ((int) (top + 284.0f), left, left + width);
            drawAdvancedNote (g, tr (Text::previewNote, language), area (339.0f, 36.0f), palette::ink2, 12.0f);
            if (status.previewEnabled)
            {
                g.setColour (palette::c700);
                g.setFont (monoFont (11.5f));
                g.drawText (trFormat (Text::previewAttenuation, language, { juce::String (std::abs (finiteReading (status.previewGainDb)), 1) }),
                            area (378.0f, 19.0f), juce::Justification::centredLeft, false);
            }
        }

        g.setColour (palette::panelBorder);
        g.drawHorizontalLine ((int) (top + 409.0f), left, left + width);
        drawAdvancedNote (g, tr (Text::advancedFooter, language), area (417.0f, 16.0f), palette::ink3, 11.0f);
    }

    void AdvancedPanel::TargetCurve::setValues (const std::array<float, 27>& newValues)
    {
        bool changed = false;
        for (size_t i = 0; i < values.size(); ++i)
        {
            const auto value = std::round (finiteReading (newValues[i]) * 50.0f) / 50.0f;
            changed = changed || value != values[i];
            values[i] = value;
        }
        if (changed)
            repaint();
    }

    void AdvancedPanel::TargetCurve::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (palette::spectrumFloor);
        g.fillRoundedRectangle (bounds, 7.0f);
        const auto plot = bounds.reduced (32.0f, 10.0f).withTrimmedBottom (8.0f);
        const auto centre = plot.getCentreY();
        g.setColour (palette::divider);
        g.drawHorizontalLine (juce::roundToInt (centre), plot.getX(), plot.getRight());
        for (int division = 0; division <= 4; ++division)
        {
            const auto x = plot.getX() + plot.getWidth() * (float) division / 4.0f;
            g.setColour (palette::track);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        }
        juce::Path line;
        for (size_t i = 0; i < values.size(); ++i)
        {
            const auto x = plot.getX() + plot.getWidth() * (float) i / (float) (values.size() - 1);
            const auto y = centre - juce::jlimit (-6.0f, 6.0f, values[i]) * plot.getHeight() / 12.0f;
            if (i == 0) line.startNewSubPath (x, y);
            else line.lineTo (x, y);
        }
        g.setColour (palette::c600);
        g.strokePath (line, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setFont (monoFont (9.5f));
        g.setColour (palette::ink3);
        g.drawText ("+6", bounds.withWidth (28.0f).withHeight (20.0f), juce::Justification::centredRight, false);
        g.drawText ("-6", juce::Rectangle<float> (0.0f, (float) getHeight() - 20.0f, 28.0f, 20.0f), juce::Justification::centredRight, false);
        g.drawText ("dB", juce::Rectangle<float> ((float) getWidth() - 28.0f, 0.0f, 24.0f, 20.0f), juce::Justification::centredRight, false);
        g.setFont (uiFont (9.5f));
        const auto labels = juce::Rectangle<float> (plot.getX(), bounds.getBottom() - 14.0f, plot.getWidth(), 12.0f);
        g.drawText (tr (Text::lowBand, language), labels, juce::Justification::centredLeft, false);
        g.drawText (tr (Text::midBand, language), labels, juce::Justification::centred, false);
        g.drawText (tr (Text::highBand, language), labels, juce::Justification::centredRight, false);
    }

    void AdvancedPanel::mouseUp (const juce::MouseEvent& e)
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
