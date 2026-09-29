/*
  ==============================================================================

    Controls.cpp
    通用控件的手感与画法。尺寸、颜色照 V4 设计稿（1200 × 820 画布上的逻辑像素）。

  ==============================================================================
*/

#include "Controls.h"

#include "CeladonLookAndFeel.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr float knobFullDragPixels   = 180.0f;   // 小旋钮：拖满量程要拖的像素（设计稿 knobH (k, 180)）
        constexpr double wheelProportionStep = 0.03;     // 滚轮：JUCE 的滚轮量（Windows 一格约 0.47）× 这个数 ≈ 每格 1.4 %

        const juce::String& unicodeMinus()
        {
            static const juce::String sign = juce::String::charToString ((juce::juce_wchar) 0x2212);
            return sign;
        }

        // 实际值比例比上次画的相差不超过这个数就不重画（0.001 × 270° ≈ 0.27°，46 px 旋钮上不到 0.1 px）。
        constexpr float effectiveRepaintThreshold = 0.001f;
    }

    //==============================================================================
    void drawValueArc (juce::Graphics& g, juce::Point<float> centre, float radius, float width,
                       float from, float to, int segmentsPerRange, bool bipolar, float saturation)
    {
        const auto lo = juce::jlimit (0.0f, 1.0f, juce::jmin (from, to));
        const auto hi = juce::jlimit (0.0f, 1.0f, juce::jmax (from, to));

        if (hi - lo < 0.004f)
            return;

        const auto count = juce::jmax (1, (int) std::ceil ((hi - lo) * (float) segmentsPerRange));
        const juce::PathStrokeType stroke (width, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

        const auto colourAt = [bipolar, saturation] (float position)
        {
            const auto colour = cyanAt (bipolar ? 0.2f + 0.8f * std::abs (position - 0.5f) * 2.0f : 0.12f + 0.88f * position);
            return saturation < 1.0f ? colour.withSaturation (colour.getSaturation() * juce::jmax (0.0f, saturation)) : colour;
        };

        for (int i = 0; i < count; ++i)
        {
            const auto s0 = lo + (hi - lo) * (float) i / (float) count;
            const auto s1 = lo + (hi - lo) * (float) (i + 1) / (float) count;

            // 每段往后多画 0.03 rad 盖住相邻两段之间的缝（最后一段不越过终点）。
            juce::Path segment;
            segment.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                                   knob::angleFor (s0), juce::jmin (knob::angleFor (s1) + 0.03f, knob::angleFor (hi)), true);
            g.setColour (colourAt (0.5f * (s0 + s1)));
            g.strokePath (segment, stroke);
        }

        // 两端的圆头：与端点那一段同色。
        for (const auto end : { lo, hi })
        {
            const auto position = knob::pointAt (centre, radius, knob::angleFor (end));
            const auto within = end <= lo ? lo + 0.5f * (hi - lo) / (float) count : hi - 0.5f * (hi - lo) / (float) count;
            g.setColour (colourAt (within));
            g.fillEllipse (juce::Rectangle<float> (width, width).withCentre (position));
        }
    }

    void drawKnobCap (juce::Graphics& g, juce::Point<float> centre, float radius)
    {
        // 影子：往下偏一点的柔和暗圈（设计稿 drop-shadow (0 1px 2px rgba(6,24,29,.3))）。
        {
            const auto shadowCentre = centre.translated (0.0f, radius * 0.08f);
            const auto shadowRadius = radius + juce::jmax (1.5f, radius * 0.16f);
            juce::ColourGradient shadow (juce::Colour (6, 24, 29).withAlpha (0.32f), shadowCentre,
                                         juce::Colour (6, 24, 29).withAlpha (0.0f), shadowCentre.translated (shadowRadius, 0.0f), true);
            shadow.addColour (radius / shadowRadius * 0.92, juce::Colour (6, 24, 29).withAlpha (0.28f));
            g.setGradientFill (shadow);
            g.fillEllipse (juce::Rectangle<float> (shadowRadius * 2.0f, shadowRadius * 2.0f).withCentre (shadowCentre));
        }

        // 本体：径向渐变，亮点在左上（设计稿 cx 0.4、cy 0.32、r 0.8）。
        const auto focus = centre.translated (-0.2f * radius, -0.36f * radius);
        juce::ColourGradient body (palette::capTop, focus, palette::capBottom, focus.translated (1.6f * radius, 0.0f), true);
        g.setGradientFill (body);
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));
    }

    juce::String formatParameterValue (const juce::RangedAudioParameter& parameter, float value, bool withUnit)
    {
        auto text = parameter.getText (parameter.convertTo0to1 (value), 0).trim();

        if (text.startsWithChar ('-'))
            text = unicodeMinus() + text.substring (1);

        if (! withUnit)
            return text;

        const auto unit = parameter.getLabel().trim();

        if (unit.isEmpty())
            return text;

        return unit == "%" ? text + unit : text + " " + unit;
    }

    std::optional<float> parseParameterValue (const juce::RangedAudioParameter& parameter, const juce::String& text)
    {
        auto cleaned = text.replace (unicodeMinus(), "-").trim();

        // 只留数字、正负号与小数点（单位、空格、全角字符都去掉）。
        cleaned = cleaned.retainCharacters ("0123456789.+-");

        if (! cleaned.containsAnyOf ("0123456789"))
            return std::nullopt;

        const auto parsed = cleaned.getFloatValue();

        if (! std::isfinite (parsed))
            return std::nullopt;

        const auto& range = parameter.getNormalisableRange();
        return range.snapToLegalValue (juce::jlimit (range.start, range.end, parsed));
    }

    //==============================================================================
    ParameterControl::ParameterControl (juce::RangedAudioParameter& p, InteractionHost& h)
        : host (h), parameter (p)
    {
        defaultValue = parameter.convertFrom0to1 (parameter.getDefaultValue());
        attachment = std::make_unique<juce::ParameterAttachment> (parameter, [this] (float newValue) { setValueFromAttachment (newValue); });
        attachment->sendInitialUpdate();
    }

    ParameterControl::~ParameterControl()
    {
        // 拖到一半被销毁（宿主关窗）：把手势收尾，气泡收起。平时不在拖，不去碰 host。
        if (gestureActive)
            attachment->endGesture();

        if (dragActive)
            host.hideValueBubble (*this);

        attachment.reset();
    }

    float ParameterControl::getProportion() const noexcept
    {
        return parameter.convertTo0to1 (value);
    }

    void ParameterControl::setValueFromAttachment (float newValue)
    {
        if (juce::exactlyEqual (newValue, value))
            return;

        value = newValue;
        parameterValueChanged();
        updateBubble();

        if (onValueChange != nullptr)
            onValueChange();
    }

    void ParameterControl::beginDrag (bool fineAtStart)
    {
        dragActive = true;
        gestureActive = false;
        fine = fineAtStart;
        dragProportion = getProportion();
    }

    void ParameterControl::dragTo (double proportion, bool fineNow)
    {
        if (! dragActive)
            return;

        if (! gestureActive)
        {
            attachment->beginGesture();
            gestureActive = true;
        }

        fine = fineNow;
        dragProportion = juce::jlimit (0.0, 1.0, proportion);
        attachment->setValueAsPartOfGesture (parameter.convertFrom0to1 ((float) dragProportion));
        updateBubble();
    }

    void ParameterControl::endDrag()
    {
        if (gestureActive)
            attachment->endGesture();

        dragActive = false;
        gestureActive = false;
        fine = false;
        host.hideValueBubble (*this);

        if (! isMouseOver (true))
            host.setControlHint (nullptr);
    }

    void ParameterControl::setValueAsCompleteGesture (float newValue)
    {
        attachment->setValueAsCompleteGesture (newValue);
    }

    void ParameterControl::updateBubble()
    {
        if (dragActive && gestureActive)
            host.showValueBubble (*this, getBubbleAnchor(), getValueText (true), fine);
    }

    bool ParameterControl::handleTextEntryClick (const juce::MouseEvent& e)
    {
        if (! e.mods.isAltDown() || ! isAdjustable())
            return false;

        juce::Component::SafePointer<ParameterControl> safeThis (this);

        host.beginTextEntry (*this, getTextEntryArea(), formatParameterValue (parameter, value, false),
                             [safeThis] (const juce::String& text)
                             {
                                 if (safeThis == nullptr)
                                     return;

                                 if (const auto parsed = parseParameterValue (safeThis->parameter, text))
                                     safeThis->setValueAsCompleteGesture (*parsed);
                             });
        return true;
    }

    void ParameterControl::mouseEnter (const juce::MouseEvent&)
    {
        if (isAdjustable())
            host.setControlHint (this);
    }

    void ParameterControl::mouseExit (const juce::MouseEvent&)
    {
        if (! dragActive)
            host.setControlHint (nullptr);
    }

    void ParameterControl::mouseDoubleClick (const juce::MouseEvent& e)
    {
        if (! isAdjustable() || e.mods.isAltDown())
            return;

        // 第二次按下已经 beginDrag（懒启动，没真正拖就还没 beginGesture）：在手势里就按手势的一部分设，否则当一次完整改动。
        if (gestureActive)
        {
            dragProportion = parameter.convertTo0to1 (defaultValue);
            attachment->setValueAsPartOfGesture (defaultValue);
        }
        else
        {
            setValueAsCompleteGesture (defaultValue);
        }

        // 这次按下剩下的拖动不再改值：双击回默认之后手一抖不该又拖走。
        dragActive = false;
        host.hideValueBubble (*this);
    }

    void ParameterControl::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (! isAdjustable() || e.mods.isAnyMouseButtonDown())
            return;

        const auto amount = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY)
                              * (wheel.isReversed ? -1.0f : 1.0f);

        if (juce::exactlyEqual (amount, 0.0f))
            return;

        const auto step = (double) amount * wheelProportionStep * (e.mods.isCommandDown() ? fineFactor : 1.0);
        auto newValue = parameter.convertFrom0to1 ((float) juce::jlimit (0.0, 1.0, (double) getProportion() + step));

        // 步进参数至少走一格（与 JUCE Slider 相同），否则慢慢滚会一直原地不动。
        const auto interval = parameter.getNormalisableRange().interval;

        if (juce::exactlyEqual (newValue, value) && interval > 0.0f)
            newValue = parameter.getNormalisableRange().snapToLegalValue (value + (amount > 0.0f ? interval : -interval));

        if (! juce::exactlyEqual (newValue, value))
            setValueAsCompleteGesture (newValue);
    }

    //==============================================================================
    ParamKnob::ParamKnob (juce::RangedAudioParameter& p, InteractionHost& h, Options o)
        : ParameterControl (p, h), options (o)
    {
        setSize (width, height);
        setRepaintsOnMouseActivity (false);
    }

    void ParamKnob::setLabel (const juce::String& newName)
    {
        if (name == newName)
            return;

        name = newName;
        repaint();
    }

    void ParamKnob::setEffectiveValue (std::optional<float> effective)
    {
        if (effective.has_value() && ! std::isfinite (*effective))
            effective.reset();

        effectiveValue = effective;

        // 与上次画出来的比：比例挪动超过阈值、或加量文字变了才重画（编辑器每帧都来）。
        const auto text = autoAmountText();
        const auto proportion = effective.has_value() ? getParameter().convertTo0to1 (*effective) : -1.0f;

        if (text == shownAutoText && std::abs (proportion - shownEffectiveProportion) <= effectiveRepaintThreshold)
            return;

        shownAutoText = text;
        shownEffectiveProportion = proportion;
        repaint();
    }

    juce::String ParamKnob::autoAmountText() const
    {
        if (! effectiveValue.has_value())
            return {};

        const auto amount = *effectiveValue - getValue();
        const auto threshold = options.autoDecimals == 0 ? 0.5f : 0.05f;

        if (std::abs (amount) < threshold)
            return {};

        const auto magnitude = options.autoDecimals == 0 ? juce::String (juce::roundToInt (std::abs (amount)))
                                                         : juce::String (std::abs (amount), options.autoDecimals);
        return (amount > 0.0f ? juce::String ("+") : unicodeMinus()) + magnitude;
    }

    juce::Point<float> ParamKnob::getBubbleAnchor() const
    {
        return { (float) width * 0.5f, 18.0f };   // 旋钮上沿再往上约 4 px
    }

    juce::Rectangle<int> ParamKnob::getTextEntryArea() const
    {
        return { 4, 78, width - 8, 22 };
    }

    void ParamKnob::paint (juce::Graphics& g)
    {
        const auto proportion = getProportion();
        const juce::Point<float> centre ((float) width * 0.5f, 48.0f);
        constexpr float arcRadius = 23.0f, arcWidth = 4.0f;

        // 更大的名字与旋钮在小比例窗口下仍可辨认。
        g.setFont (uiFont (12.5f, Weight::medium));
        g.setColour (palette::ink2);
        g.drawText (name, juce::Rectangle<float> (0.0f, 0.0f, (float) width, 18.0f), juce::Justification::centred, false);

        // 轨道
        {
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, knob::startAngle, knob::endAngle, true);
            g.setColour (palette::knobTrack);
            g.strokePath (track, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // 数值弧（单色青，越大越深；双极从正中长）
        drawValueArc (g, centre, arcRadius, arcWidth, options.bipolar ? 0.5f : 0.0f, proportion, 16, options.bipolar);

        // GLAZE 的实际值：从用户值到实际值的紫色弧 + 紫色小圆点
        const auto hasEffective = effectiveValue.has_value() && autoAmountText().isNotEmpty();
        const auto effectiveProportion = hasEffective ? getParameter().convertTo0to1 (*effectiveValue) : proportion;

        if (hasEffective)
        {
            juce::Path bridge;
            bridge.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                  knob::angleFor (proportion), knob::angleFor (effectiveProportion), true);
            g.setColour (palette::violet);
            g.strokePath (bridge, juce::PathStrokeType (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        drawKnobCap (g, centre, 17.0f);

        if (hasEffective)
        {
            const auto dot = knob::pointAt (centre, arcRadius, knob::angleFor (effectiveProportion));
            g.setColour (palette::panel);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (dot));
            g.setColour (palette::violet);
            g.fillEllipse (juce::Rectangle<float> (5.6f, 5.6f).withCentre (dot));
        }

        // 白色指针：r 6 → r 14
        {
            const auto angle = knob::angleFor (proportion);
            juce::Path pointer;
            pointer.startNewSubPath (knob::pointAt (centre, 6.0f, angle));
            pointer.lineTo (knob::pointAt (centre, 14.0f, angle));
            g.setColour (juce::Colours::white);
            g.strokePath (pointer, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // 数值 + 紫色的自动加量（等宽字，居中）
        const auto valueFont = monoFont (13.0f, Weight::medium);
        const auto autoFont  = monoFont (12.0f);
        const auto valueText = getValueText (false);
        const auto autoText  = hasEffective ? autoAmountText() : juce::String();
        const auto valueWidth = textWidth (valueFont, valueText);
        const auto autoWidth  = autoText.isEmpty() ? 0.0f : textWidth (autoFont, autoText) + 4.0f;
        auto x = ((float) width - valueWidth - autoWidth) * 0.5f;
        const juce::Rectangle<float> row (0.0f, 78.0f, (float) width, 22.0f);

        g.setFont (valueFont);
        g.setColour (palette::ink);
        g.drawText (valueText, juce::Rectangle<float> (x, row.getY(), valueWidth + 2.0f, row.getHeight()), juce::Justification::centredLeft, false);

        if (autoText.isNotEmpty())
        {
            x += valueWidth + 4.0f;
            g.setFont (autoFont);
            g.setColour (palette::violet);
            g.drawText (autoText, juce::Rectangle<float> (x, row.getY() + 0.5f, autoWidth + 2.0f, row.getHeight()), juce::Justification::centredLeft, false);
        }
    }

    void ParamKnob::mouseDown (const juce::MouseEvent& e)
    {
        if (handleTextEntryClick (e) || ! e.mods.isLeftButtonDown() || ! isAdjustable())
            return;

        lastDragPosition = e.position;
        beginDrag (e.mods.isCommandDown());
    }

    void ParamKnob::mouseDrag (const juce::MouseEvent& e)
    {
        if (! isDragging())
            return;

        // 逐事件累加增量（不按「相对按下点的总位移」）：拖到一半按下 / 松开 ⌘ 不会跳。上下拖为主，左右也算（右 / 上为增）。
        const auto delta = (e.position.x - lastDragPosition.x) + (lastDragPosition.y - e.position.y);
        lastDragPosition = e.position;

        const auto fineNow = e.mods.isCommandDown();
        const auto pixels = (double) knobFullDragPixels / (fineNow ? fineFactor : 1.0);

        if (juce::exactlyEqual (delta, 0.0f))
            return;

        dragTo (getDragProportion() + (double) delta / pixels, fineNow);
    }

    void ParamKnob::mouseUp (const juce::MouseEvent&)
    {
        endDrag();
    }

    //==============================================================================
    ParamSlider::ParamSlider (juce::RangedAudioParameter& p, InteractionHost& h, bool isBipolar)
        : ParameterControl (p, h), bipolar (isBipolar)
    {
        setSize (246, 24);
    }

    void ParamSlider::setLabel (const juce::String& newName)
    {
        if (name == newName)
            return;

        name = newName;
        repaint();
    }

    void ParamSlider::setValueWidth (int pixels)
    {
        valueAreaWidth = juce::jmax (valueWidth, pixels);
        repaint();
    }

    void ParamSlider::setTrailingSpace (int pixels)
    {
        trailingSpace = juce::jmax (0, pixels);
        repaint();
    }

    void ParamSlider::setDimmed (bool shouldBeDimmed)
    {
        if (dimmed == shouldBeDimmed)
            return;

        dimmed = shouldBeDimmed;
        repaint();
    }

    juce::Rectangle<float> ParamSlider::getTrackArea() const
    {
        const auto left = (float) (nameWidth + gap);
        const auto right = (float) getWidth() - (float) (valueAreaWidth + gap) - (float) trailingSpace;
        return { left, 0.0f, juce::jmax (10.0f, right - left), (float) getHeight() };
    }

    double ParamSlider::proportionAtX (float x) const
    {
        const auto track = getTrackArea();
        return juce::jlimit (0.0, 1.0, (double) ((x - track.getX()) / track.getWidth()));
    }

    juce::Point<float> ParamSlider::getBubbleAnchor() const
    {
        const auto track = getTrackArea();
        return { track.getX() + track.getWidth() * getProportion(), -4.0f };
    }

    juce::Rectangle<int> ParamSlider::getTextEntryArea() const
    {
        const auto track = getTrackArea();
        return { (int) track.getRight() + gap, 0, valueAreaWidth, getHeight() };
    }

    void ParamSlider::paint (juce::Graphics& g)
    {
        const auto proportion = getProportion();
        const auto track = getTrackArea();
        const auto alpha = dimmed ? 0.45f : 1.0f;

        // 名字与数值分列，轨道垂直居中，24 px 高的整行均可操作。
        g.setFont (uiFont (12.0f));
        g.setColour (palette::ink3);
        g.drawText (name, juce::Rectangle<float> (0.0f, 0.0f, (float) nameWidth, (float) getHeight()), juce::Justification::centredLeft, true);

        // 轨道（4 px）+ 填充（浅青 → 按深度加深）
        const auto bar = juce::Rectangle<float> (track.getX(), track.getCentreY() - 2.0f, track.getWidth(), 4.0f);
        g.setColour (palette::track.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (bar, 2.0f);

        const auto depth = bipolar ? std::abs (proportion - 0.5f) * 2.0f : proportion;
        const auto fillFrom = bipolar ? juce::jmin (proportion, 0.5f) : 0.0f;
        const auto fillTo   = bipolar ? juce::jmax (proportion, 0.5f) : proportion;
        const auto fill = juce::Rectangle<float> (bar.getX() + bar.getWidth() * fillFrom, bar.getY(),
                                                  bar.getWidth() * (fillTo - fillFrom), bar.getHeight());

        if (fill.getWidth() > 0.5f)
        {
            juce::ColourGradient gradient (palette::c200.withMultipliedAlpha (alpha), fill.getX(), 0.0f,
                                           cyanAt (0.2f + 0.8f * depth).withMultipliedAlpha (alpha), fill.getRight(), 0.0f, false);

            if (bipolar && proportion < 0.5f)   // 往左长时深色在左端
                gradient = juce::ColourGradient (cyanAt (0.2f + 0.8f * depth).withMultipliedAlpha (alpha), fill.getX(), 0.0f,
                                                 palette::c200.withMultipliedAlpha (alpha), fill.getRight(), 0.0f, false);

            g.setGradientFill (gradient);
            g.fillRoundedRectangle (fill, 2.0f);
        }

        // 滑块：14 px 白圆 + 2 px 青色描边
        const auto thumb = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ bar.getX() + bar.getWidth() * proportion, bar.getCentreY() });
        g.setColour (juce::Colours::white.withMultipliedAlpha (alpha));
        g.fillEllipse (thumb);
        g.setColour (cyanAt (0.3f + 0.7f * depth).withMultipliedAlpha (alpha));
        g.drawEllipse (thumb.reduced (1.0f), 2.0f);

        // 数值（等宽 12 px，右对齐）
        g.setFont (monoFont (12.0f));
        g.setColour (palette::ink.withMultipliedAlpha (dimmed ? 0.6f : 1.0f));
        g.drawText (getValueText (true), juce::Rectangle<float> (track.getRight() + (float) gap, 0.0f, (float) valueAreaWidth, (float) getHeight()),
                    juce::Justification::centredRight, false);
    }

    void ParamSlider::mouseDown (const juce::MouseEvent& e)
    {
        if (handleTextEntryClick (e) || ! e.mods.isLeftButtonDown() || ! isAdjustable())
            return;

        fineDrag = e.mods.isCommandDown();
        lastDragX = e.position.x;
        beginDrag (fineDrag);

        // 点在轨道上：滑块直接跳过去（设计稿：绝对位置）；按住 ⌘ 按下时不跳，只做相对的精调。
        if (! fineDrag && getTrackArea().expanded (0.0f, 4.0f).contains (e.position))
            dragTo (proportionAtX (e.position.x), false);
    }

    void ParamSlider::mouseDrag (const juce::MouseEvent& e)
    {
        if (! isDragging())
            return;

        const auto fineNow = e.mods.isCommandDown();
        const auto dx = e.position.x - lastDragX;
        lastDragX = e.position.x;

        if (fineNow || fineDrag)
        {
            // 精调：相对拖动，慢 8 倍；松开 ⌘ 后继续相对拖（不跳到鼠标位置），直到这次按下结束。
            fineDrag = true;
            const auto width = (double) getTrackArea().getWidth();

            if (! juce::exactlyEqual (dx, 0.0f))
                dragTo (getDragProportion() + (double) dx / width * (fineNow ? fineFactor : 1.0), fineNow);

            return;
        }

        dragTo (proportionAtX (e.position.x), false);
    }

    void ParamSlider::mouseUp (const juce::MouseEvent&)
    {
        fineDrag = false;
        endDrag();
    }

    //==============================================================================
    DragValue::DragValue (juce::RangedAudioParameter& p, InteractionHost& h)
        : ParameterControl (p, h), displayColour (palette::ink)
    {
    }

    void DragValue::setDisplay (const juce::String& text, juce::Colour colour)
    {
        if (text == displayText && colour == displayColour)
            return;

        displayText = text;
        displayColour = colour;
        repaint();
    }

    void DragValue::setAdjustable (bool shouldBeAdjustable)
    {
        if (adjustable == shouldBeAdjustable)
            return;

        adjustable = shouldBeAdjustable;
        setMouseCursor (adjustable ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    juce::Point<float> DragValue::getBubbleAnchor() const
    {
        return { (float) getWidth() * 0.5f, -3.0f };
    }

    juce::Rectangle<int> DragValue::getTextEntryArea() const
    {
        return getLocalBounds().withWidth (juce::jmax (60, getWidth()));
    }

    void DragValue::paint (juce::Graphics& g)
    {
        const auto text = adjustable ? getValueText (true) : displayText;
        const auto colour = adjustable ? palette::ink : displayColour;
        const auto font = monoFont (12.5f, Weight::medium);

        g.setFont (font);
        g.setColour (colour);
        g.drawText (text, getLocalBounds().toFloat(), juce::Justification::centredLeft, false);

        // 可调时悬停给一条虚下划线，提示「这个数可以拖」。
        if (adjustable && (hovering || isDragging()))
        {
            const auto w = juce::jmin ((float) getWidth(), textWidth (font, text));
            const auto y = (float) getHeight() * 0.5f + font.getHeight() * 0.5f + 1.0f;
            const float dashes[] = { 2.0f, 2.0f };
            g.setColour (palette::c500);
            g.drawDashedLine (juce::Line<float> (0.0f, y, w, y), dashes, 2, 1.0f);
        }
    }

    void DragValue::mouseDown (const juce::MouseEvent& e)
    {
        if (handleTextEntryClick (e) || ! e.mods.isLeftButtonDown() || ! isAdjustable())
            return;

        lastDragPosition = e.position;
        beginDrag (e.mods.isCommandDown());
    }

    void DragValue::mouseDrag (const juce::MouseEvent& e)
    {
        if (! isDragging())
            return;

        hovering = true;
        const auto delta = (e.position.x - lastDragPosition.x) + (lastDragPosition.y - e.position.y);
        lastDragPosition = e.position;
        const auto fineNow = e.mods.isCommandDown();

        if (! juce::exactlyEqual (delta, 0.0f))
            dragTo (getDragProportion() + (double) delta / ((double) knobFullDragPixels / (fineNow ? fineFactor : 1.0)), fineNow);
    }

    void DragValue::mouseUp (const juce::MouseEvent& e)
    {
        endDrag();
        hovering = getLocalBounds().contains (e.getPosition());
        repaint();
    }

    //==============================================================================
    Segmented::Segmented (Look l)
        : look (l), font (uiFont (11.0f, Weight::medium))
    {
        setRepaintsOnMouseActivity (false);
    }

    void Segmented::setOptions (const juce::StringArray& newLabels)
    {
        labels = newLabels;

        while (prefixes.size() < labels.size())
            prefixes.add ({});

        while (selectedColours.size() < labels.size())
            selectedColours.add (std::nullopt);

        repaint();
    }

    void Segmented::setSelected (int index, bool notify)
    {
        index = juce::jlimit (0, juce::jmax (0, labels.size() - 1), index);

        if (index == selected)
            return;

        selected = index;
        repaint();

        if (notify && onSelect != nullptr)
            onSelect (selected);
    }

    void Segmented::setSelectedColours (int index, Colours colours)
    {
        while (selectedColours.size() <= index)
            selectedColours.add (std::nullopt);

        selectedColours.set (index, colours);
        repaint();
    }

    void Segmented::setPrefix (int index, const juce::String& prefix)
    {
        while (prefixes.size() <= index)
            prefixes.add ({});

        prefixes.set (index, prefix);
        repaint();
    }

    float Segmented::itemWidth (int index) const
    {
        const auto padding = look == Look::compact ? 6.0f : (look == Look::language ? 9.0f : 10.0f);
        auto w = textWidth (font, labels[index]) + 2.0f * padding;

        if (prefixes[index].isNotEmpty())
            w += textWidth (monoFont (font.getHeight(), Weight::semibold), prefixes[index]) + 5.0f;

        return std::ceil (w);
    }

    int Segmented::getNaturalWidth() const
    {
        const auto inset = look == Look::compact ? 0.0f : 2.0f;
        auto total = 2.0f * inset;

        for (int i = 0; i < labels.size(); ++i)
            total += itemWidth (i) + (i > 0 ? 2.0f : 0.0f);

        return (int) std::ceil (total);
    }

    juce::Rectangle<float> Segmented::segmentBounds (int index) const
    {
        const auto inset = look == Look::compact ? 0.0f : 2.0f;
        const auto area = getLocalBounds().toFloat().reduced (inset);
        const auto count = juce::jmax (1, labels.size());
        constexpr float gapBetween = 2.0f;

        if (equalWidths)
        {
            const auto w = (area.getWidth() - gapBetween * (float) (count - 1)) / (float) count;
            return { area.getX() + (float) index * (w + gapBetween), area.getY(), w, area.getHeight() };
        }

        auto x = area.getX();

        for (int i = 0; i < index; ++i)
            x += itemWidth (i) + gapBetween;

        return { x, area.getY(), itemWidth (index), area.getHeight() };
    }

    int Segmented::segmentAt (juce::Point<float> position) const
    {
        for (int i = 0; i < labels.size(); ++i)
            if (segmentBounds (i).expanded (1.0f, 2.0f).contains (position))
                return i;

        return -1;
    }

    void Segmented::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();

        // 容器
        switch (look)
        {
            case Look::groove:
                g.setColour (palette::groove);
                g.fillRoundedRectangle (bounds, 6.0f);
                break;

            case Look::bordered:
                g.setColour (palette::panel);
                g.fillRoundedRectangle (bounds, 8.0f);
                g.setColour (palette::segmentBorder);
                g.drawRoundedRectangle (bounds.reduced (0.5f), 7.5f, 1.0f);
                break;

            case Look::language:
                g.setColour (palette::languageGroove);
                g.fillRoundedRectangle (bounds, 7.0f);
                break;

            case Look::compact:
                break;
        }

        const auto corner = look == Look::bordered ? 6.0f : (look == Look::compact ? 4.0f : (look == Look::language ? 5.0f : 4.0f));

        for (int i = 0; i < labels.size(); ++i)
        {
            const auto segment = segmentBounds (i);
            const auto isSelected = i == selected;

            // 选中项的底色与字色：按下标指定的优先，其次按外观。
            Colours colours;

            if (isSelected)
            {
                if (selectedColours[i].has_value())
                    colours = *selectedColours[i];
                else if (look == Look::bordered)
                    colours = { palette::c100, palette::c800 };
                else if (look == Look::compact)
                    colours = { palette::c100, palette::c800 };
                else if (look == Look::language)
                    colours = { palette::panel, palette::ink };
                else
                    colours = { palette::panel, palette::c700 };

                // 白底的选中项带一层很淡的阴影（凸起感）。
                if (look == Look::groove || look == Look::language)
                {
                    g.setColour (palette::shadowTint.withAlpha (0.10f));
                    g.fillRoundedRectangle (segment.translated (0.0f, 0.8f), corner);
                }

                g.setColour (colours.background);
                g.fillRoundedRectangle (segment, corner);
            }
            else
            {
                colours.text = look == Look::bordered ? palette::ink2 : palette::ink3;

                if (i == hovered)
                {
                    g.setColour (palette::ink.withAlpha (0.045f));
                    g.fillRoundedRectangle (segment, corner);
                }
            }

            // 文字（可带等宽粗体前缀，如 A / B）
            const auto prefix = prefixes[i];
            const auto prefixFont = monoFont (font.getHeight(), Weight::semibold);
            const auto prefixWidth = prefix.isEmpty() ? 0.0f : textWidth (prefixFont, prefix) + 5.0f;
            const auto labelWidth = textWidth (font, labels[i]);
            auto x = segment.getCentreX() - (prefixWidth + labelWidth) * 0.5f;

            g.setColour (colours.text);

            if (prefix.isNotEmpty())
            {
                g.setFont (prefixFont);
                g.drawText (prefix, juce::Rectangle<float> (x, segment.getY(), prefixWidth, segment.getHeight()), juce::Justification::centredLeft, false);
                x += prefixWidth;
            }

            g.setFont (font);
            g.drawText (labels[i], juce::Rectangle<float> (x, segment.getY(), labelWidth + 2.0f, segment.getHeight()), juce::Justification::centredLeft, false);
        }
    }

    void Segmented::mouseDown (const juce::MouseEvent& e)
    {
        if (! isEnabled())
            return;

        const auto index = segmentAt (e.position);

        if (index >= 0 && index != selected)
        {
            selected = index;
            repaint();

            if (onSelect != nullptr)
                onSelect (selected);
        }
    }

    void Segmented::mouseMove (const juce::MouseEvent& e)
    {
        const auto index = segmentAt (e.position);

        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }

    void Segmented::mouseExit (const juce::MouseEvent&)
    {
        if (hovered != -1)
        {
            hovered = -1;
            repaint();
        }
    }

    //==============================================================================
    SegmentedAttachment::SegmentedAttachment (Segmented& c, juce::RangedAudioParameter& parameter)
        : control (c),
          attachment (parameter, [this] (float value)
                      {
                          control.setSelected (juce::roundToInt (value));

                          if (onChange != nullptr)
                              onChange (control.getSelected());
                      })
    {
        control.onSelect = [this] (int index) { attachment.setValueAsCompleteGesture ((float) index); };
        attachment.sendInitialUpdate();
    }

    SegmentedAttachment::~SegmentedAttachment()
    {
        control.onSelect = nullptr;
    }

    //==============================================================================
    PowerButton::PowerButton (juce::RangedAudioParameter& bypassParameter)
        : attachment (bypassParameter, [this] (float value)
                      {
                          const auto nowOn = value < 0.5f;

                          if (nowOn != on)
                          {
                              on = nowOn;
                              repaint();

                              if (onChange != nullptr)
                                  onChange();
                          }
                      })
    {
        setSize (28, 28);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        attachment.sendInitialUpdate();
    }

    void PowerButton::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto hover = isMouseOver();

        g.setColour (on ? (hover ? palette::c700 : palette::c600) : (hover ? palette::divider : palette::track));
        g.fillEllipse (bounds);

        // 电源符号：开口圆弧 + 竖线（设计稿 M3 2.4 A3.6 3.6 0 1 0 7 2.4 M5 1 v4，10 × 10 视图里 14 px 大）
        const auto scale = 14.0f / 10.0f;
        const auto origin = bounds.getCentre() - juce::Point<float> (7.0f, 7.0f);
        juce::Path icon;
        icon.addCentredArc (origin.x + 5.0f * scale, origin.y + 5.5f * scale, 3.6f * scale, 3.6f * scale, 0.0f,
                            juce::degreesToRadians (34.0f), juce::degreesToRadians (326.0f), true);
        icon.startNewSubPath (origin.x + 5.0f * scale, origin.y + 1.0f * scale);
        icon.lineTo (origin.x + 5.0f * scale, origin.y + 5.0f * scale);
        g.setColour (on ? juce::Colours::white : palette::ink4);
        g.strokePath (icon, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void PowerButton::mouseUp (const juce::MouseEvent& e)
    {
        if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()))
            attachment.setValueAsCompleteGesture (on ? 1.0f : 0.0f);   // 开 → 旁通
    }

    //==============================================================================
    HeadphoneButton::HeadphoneButton()
    {
        setSize (28, 28);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void HeadphoneButton::setOn (bool shouldBeOn)
    {
        if (on == shouldBeOn)
            return;

        on = shouldBeOn;
        repaint();
    }

    void HeadphoneButton::enablementChanged()
    {
        setAlpha (isEnabled() ? 1.0f : 0.35f);
        setMouseCursor (isEnabled() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void HeadphoneButton::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto hover = isHovering() && isEnabled();

        if (on)
        {
            g.setColour (hover ? palette::c900 : palette::ink);
            g.fillEllipse (bounds);
        }
        else
        {
            g.setColour (hover ? palette::groove : palette::panel);
            g.fillEllipse (bounds);
            g.setColour (palette::controlBorder);
            g.drawEllipse (bounds, 1.0f);
        }

        // 耳机（10 × 10 视图画成 14 px）：头梁 M1.5 6.5 V5 a3.5 3.5 0 0 1 7 0 V6.5，两只耳罩 2 × 3、圆角 0.8。
        constexpr float scale = 14.0f / 10.0f;
        const auto origin = getLocalBounds().toFloat().getCentre() - juce::Point<float> (7.0f, 7.0f);
        const auto at = [origin] (float x, float y) { return origin + juce::Point<float> (x * scale, y * scale); };

        juce::Path band;
        band.startNewSubPath (at (1.5f, 6.5f));
        band.lineTo (at (1.5f, 5.0f));
        band.addCentredArc (at (5.0f, 5.0f).x, at (5.0f, 5.0f).y, 3.5f * scale, 3.5f * scale, 0.0f,
                            -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, false);
        band.lineTo (at (8.5f, 6.5f));

        const auto foreground = on ? juce::Colours::white : palette::ink2;
        g.setColour (foreground);
        g.strokePath (band, juce::PathStrokeType (1.1f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        for (const auto x : { 1.0f, 7.0f })
            g.fillRoundedRectangle (juce::Rectangle<float> (at (x, 5.6f), at (x + 2.0f, 8.6f)), 0.8f * scale);
    }

    //==============================================================================
    ChipToggle::ChipToggle (juce::RangedAudioParameter& parameter)
        : attachment (parameter, [this] (float value)
                      {
                          const auto nowOn = value >= 0.5f;

                          if (nowOn != on)
                          {
                              on = nowOn;
                              repaint();

                              if (onChange != nullptr)
                                  onChange();
                          }
                      })
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        attachment.sendInitialUpdate();
    }

    int ChipToggle::getNaturalWidth() const
    {
        return (int) std::ceil (textWidth (withLetterSpacing (uiFont (11.5f, Weight::semibold), 0.3f), text) + 14.0f);
    }

    void ChipToggle::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (on ? palette::violetSoft : palette::panel);
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (on ? palette::violetBorder : palette::controlBorder);
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

        g.setFont (withLetterSpacing (uiFont (11.5f, Weight::semibold), 0.3f));
        g.setColour (on ? palette::violet : palette::ink3);
        g.drawText (text, getLocalBounds().toFloat(), juce::Justification::centred, false);
    }

    void ChipToggle::mouseUp (const juce::MouseEvent& e)
    {
        if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()))
            attachment.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
    }

    //==============================================================================
    ReductionRow::ReductionRow (float fullScaleDb)
        : fullScale (juce::jmax (0.1f, fullScaleDb))
    {
        setSize (246, 24);
        shownText = "0.0 dB";
    }

    void ReductionRow::setLabel (const juce::String& newName)
    {
        if (name == newName)
            return;

        name = newName;
        repaint();
    }

    void ReductionRow::pushReading (float reductionDb, bool warning)
    {
        // 表的下落：每帧最多落 0.66 dB（30 帧 / s × 0.66 ≈ 20 dB/s），升得立刻。
        const auto reading = std::isfinite (reductionDb) ? juce::jlimit (0.0f, 60.0f, reductionDb) : 0.0f;
        const auto next = juce::jmax (reading, shownDb - 0.66f, 0.0f);
        const auto text = juce::String (next, 1) + " dB";
        const auto movedPixels = std::abs (next - shownDb) / fullScale * (float) getWidth();

        if (text == shownText && warning == shownWarning && movedPixels < 0.25f)
            return;

        shownDb = next;
        shownWarning = warning;
        shownText = text;
        repaint();
    }

    void ReductionRow::paint (juce::Graphics& g)
    {
        const auto h = (float) getHeight();
        const auto left = (float) (ParamSlider::nameWidth + ParamSlider::gap);
        const auto right = (float) getWidth() - (float) (ParamSlider::valueWidth + ParamSlider::gap);

        g.setFont (uiFont (12.0f));
        g.setColour (palette::ink3);
        g.drawText (name, juce::Rectangle<float> (0.0f, 0.0f, (float) ParamSlider::nameWidth, h), juce::Justification::centredLeft, true);

        const auto bar = juce::Rectangle<float> (left, h * 0.5f - 2.0f, right - left, 4.0f);
        g.setColour (palette::track);
        g.fillRoundedRectangle (bar, 2.0f);

        const auto fill = bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, shownDb / fullScale));

        if (fill.getWidth() > 0.5f)
        {
            g.setGradientFill (shownWarning ? juce::ColourGradient (palette::warning, fill.getX(), 0.0f, palette::warningText, fill.getRight(), 0.0f, false)
                                            : juce::ColourGradient (palette::c600, fill.getX(), 0.0f, palette::c900, fill.getRight(), 0.0f, false));
            g.fillRoundedRectangle (fill, 2.0f);
        }

        g.setFont (monoFont (12.0f));
        g.setColour (shownWarning ? palette::warningText : palette::c800);
        g.drawText (shownText, juce::Rectangle<float> (right + (float) ParamSlider::gap, 0.0f, (float) ParamSlider::valueWidth, h),
                    juce::Justification::centredRight, false);
    }
}
