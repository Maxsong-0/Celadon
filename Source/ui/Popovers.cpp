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
        constexpr float bulletIndent = 14.0f;   // 帮助面板：圆点占的宽度，正文从这里起排

        // 设置面板各行的纵坐标（卡片内）。T-111 重排：屏幕标签挪到预览框上方（4-10，原来画在框里会被窗口框穿过），
        // 预览框 140 → 150、说明 292 → 300，下面各行依次下移，再加「语言」一行（包 D）。
        constexpr float screenFrameY = 150.0f, sizeNoteY = 300.0f, firstDividerY = 348.0f, reduceMotionRowY = 356.0f,
                        secondDividerY = 398.0f, languageRowY = 410.0f, thirdDividerY = 440.0f, versionRowY = 448.0f;

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
        // 语言：与原来顶栏同一种外观（灰底槽、选中项白底），两项按字宽。
        languageSelector.setOptions ({ juce::String::fromUTF8 ("中"), "EN" });
        languageSelector.setEqualWidths (false);
        languageSelector.setTextStyle (TextSize::body, Weight::medium);
        languageSelector.onSelect = [this] (int index)
        {
            if (onLanguageChange != nullptr)
                onLanguageChange (index == 1 ? Language::english : Language::chinese);
        };
        addAndMakeVisible (languageSelector);

        setSize (cardWidth + 2 * shadowMargin, cardHeight + 2 * shadowMargin);
    }

    void SettingsPanel::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        languageSelector.setSelected (language == Language::english ? 1 : 0);
        repaint();
    }

    void SettingsPanel::resized()
    {
        const auto card = getCardBounds();
        const auto width = languageSelector.getNaturalWidth();
        languageSelector.setBounds ((int) (card.getRight() - padding) - width, (int) (card.getY() + languageRowY) - 5, width, 26);
    }

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
        return { card.getRight() - padding - 38.0f, card.getY() + reduceMotionRowY + 4.0f, 38.0f, 22.0f };
    }

    juce::Font SettingsPanel::reduceMotionNoteFont()   { return uiFont (TextSize::caption); }
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
        g.setFont (uiFont (TextSize::title, Weight::semibold));
        g.setColour (palette::ink);
        drawTextLine (g, tr (Text::settings, language), juce::Rectangle<float> (left, card.getY() + 12.0f, 200.0f, 24.0f), juce::Justification::centredLeft);
        paintCloseButton (g);

        // 界面大小：125%  1500 × 1025
        {
            const auto y = card.getY() + 42.0f;
            g.setFont (uiFont (TextSize::body, Weight::medium));
            drawTextLine (g, tr (Text::windowSize, language), juce::Rectangle<float> (left, y, 150.0f, 16.0f), juce::Justification::centredLeft);

            const auto pxFont = monoFont (TextSize::caption);
            const auto px = pixelText (zoom);
            const auto pxWidth = textWidth (pxFont, px);
            g.setFont (pxFont);
            g.setColour (palette::ink3);
            drawTextLine (g, px, juce::Rectangle<float> (right - pxWidth, y, pxWidth + 1.0f, 16.0f), juce::Justification::centredRight);
            g.setFont (monoFont (TextSize::value, Weight::semibold));
            g.setColour (palette::ink);
            drawTextLine (g, percentText (zoom), juce::Rectangle<float> (right - pxWidth - 8.0f - 60.0f, y, 60.0f, 16.0f), juce::Justification::centredRight);
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
            g.setFont (monoFont (TextSize::caption, Weight::medium));
            g.setColour (on ? juce::Colours::white : palette::ink);
            drawTextLine (g, percentText (presets[i]), area, juce::Justification::centred);
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

            // 滑块 14 px：与卡片上的滑条同一尺寸（T-111 / 4-25，原 12）。
            const auto thumb = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ bar.getX() + bar.getWidth() * proportion, bar.getCentreY() });
            g.setColour (juce::Colours::white);
            g.fillEllipse (thumb);
            g.setColour (palette::c700);
            g.drawEllipse (thumb.reduced (1.0f), 2.0f);

            g.setFont (monoFont (TextSize::micro));
            g.setColour (palette::ink3);
            drawTextLine (g, "70%", juce::Rectangle<float> (left, slider.getBottom(), 40.0f, 14.0f), juce::Justification::centredLeft);
            drawTextLine (g, "200%", juce::Rectangle<float> (right - 40.0f, slider.getBottom(), 40.0f, 14.0f), juce::Justification::centredRight);
        }

        // 屏幕预览：按屏幕比例缩进 256 × 144 的框，窗口按当前大小画进去；比屏幕大时变橙色并提醒。
        // 「1280 × 1024 屏幕」与「超出屏幕」写在框的上方（框外），窗口再大也压不到字。
        {
            const juce::Rectangle<float> frame (left + (innerWidth - 256.0f) * 0.5f, card.getY() + screenFrameY, 256.0f, 144.0f);
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

            g.setFont (uiFont (TextSize::caption));
            g.setColour (palette::ink3);
            drawTextLine (g, trFormat (Text::screenSize, language, { juce::String (screen.getWidth()), juce::String (screen.getHeight()) }),
                          juce::Rectangle<float> (frame.getX(), frame.getY() - 15.0f, 150.0f, 12.0f), juce::Justification::centredLeft);

            if (over)
            {
                g.setFont (uiFont (TextSize::caption, Weight::medium));
                g.setColour (palette::warningText);
                drawTextLine (g, tr (Text::tooBig, language), juce::Rectangle<float> (frame.getRight() - 104.0f, frame.getY() - 15.0f, 104.0f, 12.0f),
                              juce::Justification::centredRight);
            }
        }

        // 说明（可换行）
        {
            juce::AttributedString note;
            note.setWordWrap (juce::AttributedString::byWord);
            note.setLineSpacing (3.0f);
            note.append (tr (Text::sizeNote, language), uiFont (TextSize::caption), palette::ink2);
            juce::TextLayout layout;
            layout.createLayout (note, innerWidth);
            layout.draw (g, juce::Rectangle<float> (left, card.getY() + sizeNoteY, innerWidth, 40.0f));
        }

        g.setColour (palette::track);
        g.fillRect (left, card.getY() + firstDividerY, innerWidth, 1.0f);

        // 减少动效 + 开关
        {
            const auto y = card.getY() + reduceMotionRowY;
            g.setFont (uiFont (TextSize::body, Weight::medium));
            g.setColour (palette::ink);
            drawTextLine (g, tr (Text::reduceMotion, language), juce::Rectangle<float> (left, y, 220.0f, 16.0f), juce::Justification::centredLeft);
            g.setFont (reduceMotionNoteFont());
            g.setColour (palette::ink3);
            drawTextLine (g, tr (Text::reduceMotionNote, language), juce::Rectangle<float> (left, y + 16.0f, reduceMotionNoteWidth(), 14.0f),
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
        g.fillRect (left, card.getY() + secondDividerY, innerWidth, 1.0f);

        // 语言：名字在左，中 / EN 选择器（子组件，见 resized）在右
        g.setFont (uiFont (TextSize::body, Weight::medium));
        g.setColour (palette::ink);
        drawTextLine (g, tr (Text::language, language), juce::Rectangle<float> (left, card.getY() + languageRowY, 200.0f, 16.0f), juce::Justification::centredLeft);

        g.setColour (palette::track);
        g.fillRect (left, card.getY() + thirdDividerY, innerWidth, 1.0f);

        // 版本 · 延迟
        {
            const auto y = card.getY() + versionRowY;
            g.setFont (monoFont (TextSize::caption));
            g.setColour (palette::ink3);
            drawTextLine (g, tr (Text::version, language) + " " + juce::String (JucePlugin_VersionString),
                          juce::Rectangle<float> (left, y, 140.0f, 14.0f), juce::Justification::centredLeft);
            drawTextLine (g, tr (Text::latency, language) + " " + juce::String (latencyMs, 1) + " ms",
                          juce::Rectangle<float> (right - 140.0f, y, 140.0f, 14.0f), juce::Justification::centredRight);
        }
    }

    void SettingsPanel::mouseDown (const juce::MouseEvent& e)
    {
        // 关闭键在松手时关（mouseUp），与帮助、高级两块一致（T-111 / 4-24）。
        if (isOnCloseButton (e.position))
            return;

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

            // 按在滑块圆上：记下偏移、不跳；按在轨道别处：照旧跳到按下点。
            const auto slider = sliderBounds();
            const auto proportion = (float) juce::jlimit (0.0, 1.0, (zoom - minZoom) / (maxZoom - minZoom));
            const auto thumbX = slider.getX() + slider.getWidth() * proportion;
            const auto onThumb = std::abs (e.position.x - thumbX) <= 7.0f;
            sliderGrabOffset = onThumb ? e.position.x - thumbX : 0.0f;

            if (onZoomDragging != nullptr)
                onZoomDragging (true);

            if (! onThumb && onZoomChange != nullptr)
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
            onZoomChange (zoomAt (getLocalPoint (nullptr, e.getScreenPosition()).toFloat().x - sliderGrabOffset));
    }

    void SettingsPanel::mouseUp (const juce::MouseEvent& e)
    {
        if (draggingSlider)
        {
            draggingSlider = false;
            sliderGrabOffset = 0.0f;

            if (onZoomDragging != nullptr)
                onZoomDragging (false);

            return;
        }

        // 按下与松手都在关闭键上才关（按下后拖出去再松手不算）。
        if (isOnCloseButton (e.mouseDownPosition) && isOnCloseButton (e.position) && onClose != nullptr)
            onClose();
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
        // 操作手法六条一组，「颜色 / 母带与原声 / 试听差值」三条一组；条间距 6，两组之间再加 6。
        const Text lines[] = { Text::helpDrag, Text::helpSlider, Text::helpFine, Text::helpReset, Text::helpType, Text::helpWheel,
                               Text::helpColours, Text::helpAb, Text::helpAudition };
        constexpr size_t secondGroup = 6;
        constexpr float itemGap = 6.0f, groupGap = 6.0f;
        const auto bodyWidth = (float) cardWidth - 2.0f * padding - bulletIndent;

        items.clear();
        auto y = 0.0f;

        for (size_t i = 0; i < std::size (lines); ++i)
        {
            if (i > 0)
                y += itemGap + (i == secondGroup ? groupGap : 0.0f);

            juce::AttributedString text;
            text.setWordWrap (juce::AttributedString::byWord);
            text.setLineSpacing (3.0f);
            text.append (tr (lines[i], language), uiFont (TextSize::body), palette::ink2);

            Item item;
            item.layout.createLayout (text, bodyWidth);
            item.y = y;
            y += item.layout.getHeight();
            items.push_back (std::move (item));
        }

        cardHeight = (int) std::ceil (48.0f + y + padding);
        setSize (cardWidth + 2 * shadowMargin, cardHeight + 2 * shadowMargin);
    }

    void HelpPanel::paint (juce::Graphics& g)
    {
        PopoverCard::paint (g);

        const auto card = getCardBounds();
        g.setFont (uiFont (TextSize::title, Weight::semibold));
        g.setColour (palette::ink);
        drawTextLine (g, tr (Text::helpTitle, language), juce::Rectangle<float> (card.getX() + padding, card.getY() + 12.0f, 200.0f, 24.0f),
                      juce::Justification::centredLeft);
        paintCloseButton (g);

        const auto left = card.getX() + padding, top = card.getY() + 46.0f;
        const auto bodyWidth = card.getWidth() - 2.0f * padding - bulletIndent;
        const auto bulletFont = uiFont (TextSize::body);

        for (const auto& item : items)
        {
            // 圆点与正文第一行同高居中（第一行行高取字体高度）。
            g.setFont (bulletFont);
            g.setColour (palette::c600);
            drawTextLine (g, juce::String::fromUTF8 ("•"), juce::Rectangle<float> (left, top + item.y, bulletIndent, bulletFont.getHeight()),
                          juce::Justification::centredLeft);
            item.layout.draw (g, juce::Rectangle<float> (left + bulletIndent, top + item.y, bodyWidth, item.layout.getHeight()));
        }
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

        // 高级面板里的说明（可换行）：正文档，次要的说明用说明档（size）。
        void drawAdvancedNote (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                               juce::Colour colour = palette::ink2, TextSize size = TextSize::body)
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
        harshMode.setSelectedColours (0, { palette::violetSoft, palette::violetText });
        targetMode.setSelectedColours (1, { palette::violetSoft, palette::violetText });
        tabs.setTextStyle (TextSize::body, Weight::medium);
        harshMode.setTextStyle (TextSize::body, Weight::medium);
        targetMode.setTextStyle (TextSize::body, Weight::medium);
        dither.setTextStyle (TextSize::body, Weight::medium);

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
        g.setFont (uiFont (TextSize::title, Weight::semibold));
        drawTextLine (g, tr (Text::advanced, language), area (13.0f, 23.0f).withTrimmedRight (40.0f), juce::Justification::centredLeft);
        paintCloseButton (g);
        drawAdvancedNote (g, tr (Text::advancedSubtitle, language), area (38.0f, 20.0f), palette::ink3, TextSize::caption);

        if (page == 0)
        {
            drawAdvancedNote (g, tr (Text::harshNote, language), area (117.0f, 36.0f), palette::ink);
            g.setFont (monoFont (TextSize::value, Weight::medium));
            g.setColour (palette::c700);
            drawTextLine (g, trFormat (Text::harshReduction, language, { juce::String (juce::jmax (0.0f, finiteReading (status.harshReductionDb)), 1) }),
                          area (174.0f, 20.0f).withTrimmedLeft (316.0f), juce::Justification::centredRight);
            const auto note = harshMode.getSelected() == (int) celadon::HarshMode::automatic ? Text::harshAutoNote
                            : harshMode.getSelected() == (int) celadon::HarshMode::off ? Text::harshOffNote : Text::harshManualNote;
            g.setColour (palette::spectrumFloor);
            g.fillRoundedRectangle (area (325.0f, 62.0f), 8.0f);
            drawAdvancedNote (g, tr (note, language), area (339.0f, 40.0f).reduced (14.0f, 0.0f));
        }
        else if (page == 1)
        {
            drawAdvancedNote (g, tr (Text::targetNote, language), area (108.0f, 20.0f));
            g.setFont (uiFont (TextSize::caption, Weight::medium));
            g.setColour (palette::ink3);
            drawTextLine (g, tr (Text::targetCurveTitle, language), area (207.0f, 19.0f), juce::Justification::centredLeft);
            g.setColour (palette::c700);
            g.setFont (uiFont (TextSize::body, Weight::medium));
            drawTextLine (g, referenceStatusText(), area (308.0f, 22.0f), juce::Justification::centredLeft, true);
            drawAdvancedNote (g, tr (Text::referenceRouting, language), area (334.0f, 32.0f), palette::ink3, TextSize::caption);
        }
        else
        {
            drawAdvancedNote (g, tr (Text::codecNote, language), area (151.0f, 34.0f), palette::ink3, TextSize::caption);
            g.setColour (palette::divider);
            g.drawHorizontalLine ((int) (top + 190.0f), left, left + width);
            g.setColour (palette::ink);
            g.setFont (uiFont (TextSize::body, Weight::medium));
            drawTextLine (g, tr (Text::ditherLabel, language), area (202.0f, 23.0f).withWidth (240.0f), juce::Justification::centredLeft);
            drawAdvancedNote (g, tr (Text::ditherNote, language), area (240.0f, 35.0f), palette::ink3, TextSize::caption);
            g.setColour (palette::divider);
            g.drawHorizontalLine ((int) (top + 284.0f), left, left + width);
            drawAdvancedNote (g, tr (Text::previewNote, language), area (339.0f, 36.0f), palette::ink2, TextSize::caption);
            if (status.previewEnabled)
            {
                g.setColour (palette::c700);
                g.setFont (monoFont (TextSize::value));
                drawTextLine (g, trFormat (Text::previewAttenuation, language, { juce::String (std::abs (finiteReading (status.previewGainDb)), 1) }),
                              area (378.0f, 19.0f), juce::Justification::centredLeft);
            }
        }

        g.setColour (palette::panelBorder);
        g.drawHorizontalLine ((int) (top + 409.0f), left, left + width);
        drawAdvancedNote (g, tr (Text::advancedFooter, language), area (417.0f, 16.0f), palette::ink3, TextSize::caption);
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
        g.setFont (monoFont (TextSize::micro));
        g.setColour (palette::ink3);
        drawTextLine (g, "+6", bounds.withWidth (28.0f).withHeight (20.0f), juce::Justification::centredRight);
        drawTextLine (g, juce::String::charToString ((juce::juce_wchar) 0x2212) + "6", juce::Rectangle<float> (0.0f, (float) getHeight() - 20.0f, 28.0f, 20.0f),
                      juce::Justification::centredRight);
        drawTextLine (g, "dB", juce::Rectangle<float> ((float) getWidth() - 28.0f, 0.0f, 24.0f, 20.0f), juce::Justification::centredRight);
        g.setFont (uiFont (TextSize::micro));
        const auto labels = juce::Rectangle<float> (plot.getX(), bounds.getBottom() - 14.0f, plot.getWidth(), 12.0f);
        drawTextLine (g, tr (Text::lowBand, language), labels, juce::Justification::centredLeft);
        drawTextLine (g, tr (Text::midBand, language), labels, juce::Justification::centred);
        drawTextLine (g, tr (Text::highBand, language), labels, juce::Justification::centredRight);
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
        auto w = 9.0f + textWidth (monoFont (TextSize::value), text) + 9.0f;

        if (fine)
            w += 6.0f + textWidth (uiFont (TextSize::caption), fineLabel) + 10.0f;

        return (int) std::ceil (w);
    }

    void ValueBubble::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (palette::ink);
        g.fillRoundedRectangle (bounds, 7.0f);

        const auto font = monoFont (TextSize::value);
        g.setFont (font);
        g.setColour (juce::Colours::white);
        drawTextLine (g, text, juce::Rectangle<float> (9.0f, 0.0f, textWidth (font, text) + 2.0f, bounds.getHeight()), juce::Justification::centredLeft);

        if (fine)
        {
            const auto labelFont = uiFont (TextSize::caption);
            const juce::Rectangle<float> chip (9.0f + textWidth (font, text) + 6.0f, (bounds.getHeight() - 16.0f) * 0.5f,
                                               textWidth (labelFont, fineLabel) + 10.0f, 16.0f);
            g.setColour (palette::c400);
            g.fillRoundedRectangle (chip, 4.0f);
            g.setFont (labelFont);
            g.setColour (palette::c900);
            drawTextLine (g, fineLabel, chip, juce::Justification::centred);
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

        g.setFont (uiFont (TextSize::body));
        g.setColour (palette::c200);
        drawTextLine (g, title, juce::Rectangle<float> (0.0f, 22.0f, bounds.getWidth(), 16.0f), juce::Justification::centred);

        g.setFont (uiFont (TextSize::displayLarge, Weight::light));
        g.setColour (juce::Colours::white);
        drawTextLine (g, percentText (zoom), juce::Rectangle<float> (0.0f, 38.0f, bounds.getWidth(), 42.0f), juce::Justification::centred);

        g.setFont (monoFont (TextSize::value));
        g.setColour (palette::c100);
        drawTextLine (g, pixelText (zoom) + " px", juce::Rectangle<float> (0.0f, 82.0f, bounds.getWidth(), 16.0f), juce::Justification::centred);
    }
}
