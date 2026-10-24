/*
  ==============================================================================

    Dial.cpp

  ==============================================================================
*/

#include "Dial.h"

#include "CeladonLookAndFeel.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr int motionHz = 60;

        // 甩动按事件自带的时间算（截图工具的检查能造出确定的快甩与停顿后松手）。
        double eventTimeMs (const juce::MouseEvent& e)
        {
            return (double) e.eventTime.toMilliseconds();
        }

        // ---- 小旋钮的几何（设计稿：54 px 旋钮，轨道半径 23、宽 4，帽子半径 17）----
        const juce::Point<float> smallCentre { (float) ParamKnob::width * 0.5f, 48.0f };
        constexpr float smallArcRadius = 23.0f, smallArcWidth = 4.0f, smallCapRadius = 17.0f;
        constexpr float ringGrabMargin = 8.0f;          // 轨道外侧多大一圈还算外圈（主旋钮的 hitTest 也是 8 px）
        constexpr float smallCometExtraWidth = 5.0f;    // 小旋钮的彗尾头部多出的线宽（主旋钮 10；原版 12 在 6 px 粗的轨道上）

        // 实际值比例比上次画的相差不超过这个数就不重画（0.001 × 270° ≈ 0.27°，46 px 旋钮上不到 0.1 px）。
        constexpr float effectiveRepaintThreshold = 0.001f;
    }

    //==============================================================================
    DialControl::DialControl (juce::RangedAudioParameter& p, InteractionHost& h)
        : ParameterControl (p, h)
    {
        // 基类构造时已按参数的初始值回调过一次（那时还是基类的 parameterValueChanged）：读数从这里直接落下。
        reading.jump (getProportion());
    }

    DialControl::~DialControl()
    {
        ticker.stopTimer();
    }

    void DialControl::setReduceMotion (bool shouldReduceMotion)
    {
        if (reduceMotion == shouldReduceMotion)
            return;

        reduceMotion = shouldReduceMotion;

        if (reduceMotion)
            finishMotion();
    }

    void DialControl::finishMotion()
    {
        ticker.stopTimer();
        reading.jump (getProportion());
        comet.clear();
        dialMotionChanged();
    }

    bool DialControl::isReadingSettled() const noexcept
    {
        return ! reading.isAnimating() && juce::exactlyEqual (reading.getPosition(), getProportion());
    }

    float DialControl::getReadingValue() const
    {
        // 停稳时直接用参数值：比例换回参数单位有浮点误差（双极的 0 可能变成 −0.0），文字要与没有动画时逐字相同。
        if (isReadingSettled())
            return getValue();

        return getParameter().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, reading.getPosition()));
    }

    bool DialControl::isInRing (juce::Point<float> position) const
    {
        const auto geometry = getDialGeometry();
        const auto distance = position.getDistanceFrom (geometry.centre);
        return distance >= geometry.deadRadius && distance <= geometry.grabRadius;
    }

    float DialControl::pointerAngle (juce::Point<float> position) const
    {
        const auto offset = position - getDialGeometry().centre;
        return std::atan2 (offset.x, -offset.y);
    }

    float DialControl::ringProportion (juce::Point<float> position)
    {
        const auto offset = position - getDialGeometry().centre;
        return dial::proportionAtOffset (offset.x, offset.y, side);
    }

    //==============================================================================
    void DialControl::parameterValueChanged()
    {
        const auto target = getProportion();

        if (reduceMotion)
        {
            reading.jump (target);
            comet.clear();
            dialMotionChanged();
            return;
        }

        // 自己拖的时候读数追手（tick 里），不另起弹簧；宿主在这期间改的值也不理会（原版：抓着时不接受外部的值）。
        if (! isFollowing())
        {
            if (jumpNextChange)
            {
                // 滚轮一下跳过去：动画正在跑时也别把这一跳算成速度（不然下一帧彗尾猛地一闪）。
                reading.jump (target);
                comet.restart();
            }
            else
                reading.launch (target, dial::tapBounce);
        }

        startMotion();
        dialMotionChanged();
    }

    void DialControl::startMotion()
    {
        if (reduceMotion || ticker.isTimerRunning())
            return;

        // 从静止重新开始：这一帧不算速度（原版 wake 把上一帧位置清空），滚轮一下跳过去不会闪出彗尾。
        lastTickMs = juce::Time::getMillisecondCounterHiRes();
        comet.restart();
        ticker.startTimerHz (motionHz);
    }

    void DialControl::tick()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto seconds = juce::jlimit (0.0, dial::maxFrameSeconds, (now - lastTickMs) / 1000.0);
        lastTickMs = now;
        advanceMotion (seconds);
    }

    void DialControl::advanceMotion (double seconds)
    {
        if (reduceMotion)
        {
            finishMotion();
            return;
        }

        const auto positionBefore = reading.getPosition();
        const auto strengthBefore = comet.getStrength();

        if (isFollowing())
            reading.follow ((float) getDragProportion(), seconds);
        else
            reading.advance (seconds);

        comet.update (reading.getPosition(), seconds);

        // 停稳 = 读数到位（拖动中 = 追上了手；否则 = 弹簧停了）且彗尾的速度落到阈值下。按住不动也停：平滑后的速度是指数衰减、
        // 永远到不了 0，不停的话彗尾强度每帧都差一点点、每帧重画（主旋钮每帧重新合成）；再动时 mouseDrag 会重新起动画。
        const auto readingStill = isFollowing() ? juce::exactlyEqual (reading.getPosition(), reading.getTarget()) : ! reading.isAnimating();
        const auto stopping = readingStill && comet.isResting();

        if (stopping)
        {
            comet.clear();
            ticker.stopTimer();
        }

        // 读数、彗尾都没变的帧不重画；停下的那一帧通知一次（主旋钮据此把瓷面补到精确的读数上）。
        if (stopping || ! juce::exactlyEqual (reading.getPosition(), positionBefore) || ! juce::exactlyEqual (comet.getStrength(), strengthBefore))
            dialMotionChanged();
    }

    void DialControl::settleFromRelease (float bounce, float velocity)
    {
        if (reduceMotion)
            return;   // parameterValueChanged 已经让读数落下

        reading.launch (getProportion(), bounce, velocity);
        startMotion();
    }

    //==============================================================================
    void DialControl::mouseMove (const juce::MouseEvent& e)
    {
        if (! isDragging())
            setMouseCursor (isInRing (e.position) ? juce::MouseCursor::DraggingHandCursor : centreCursor);
    }

    void DialControl::mouseDown (const juce::MouseEvent& e)
    {
        if (handleTextEntryClick (e) || ! e.mods.isLeftButtonDown() || ! isAdjustable())
            return;

        const auto fineAtStart = e.mods.isCommandDown();
        downPosition = lastDragPosition = e.position;
        ringMoved = false;
        beginDrag (fineAtStart);

        if (! isInRing (e.position))
        {
            grab = Grab::centre;
            return;
        }

        grab = Grab::ring;
        ringRelative = fineAtStart;
        side = dial::Side::none;
        flick.reset();
        lastPointerAngle = pointerAngle (e.position);

        // 按住 ⌘ / Ctrl 按下：不跳，之后相对转动（精调要的是从当前值慢慢挪）。
        if (ringRelative)
            return;

        // 点外圈：参数直接到所按角度（手势在这里开始），读数在 parameterValueChanged 里从当前位置起弹簧。
        const auto at = ringProportion (e.position);
        flick.add (eventTimeMs (e), at);
        dragTo (at, false);
    }

    void DialControl::mouseDrag (const juce::MouseEvent& e)
    {
        if (! isDragging() || grab == Grab::none)
            return;

        const auto fineNow = e.mods.isCommandDown();

        if (grab == Grab::centre)
        {
            // 逐事件累加增量（不按相对按下点的总位移）：拖到一半按下 / 松开 ⌘ 不会跳。上下拖为主，左右也算（右 / 上为增）。
            const auto delta = (e.position.x - lastDragPosition.x) + (lastDragPosition.y - e.position.y);
            lastDragPosition = e.position;

            if (juce::exactlyEqual (delta, 0.0f))
                return;

            const auto pixels = (double) getDialGeometry().fullDragPixels / (fineNow ? fineFactor : 1.0);
            dragTo (getDragProportion() + (double) delta / pixels, fineNow);
            startMotion();
            return;
        }

        // 外圈：移动超过门槛才算拖（之前是点按，读数还在弹簧上）。
        if (! ringMoved)
        {
            if (e.position.getDistanceFrom (downPosition) < dial::dragThreshold (getDialGeometry().trackRadius))
                return;

            ringMoved = true;
        }

        const auto angle = pointerAngle (e.position);
        const auto turned = dial::wrappedAngleDelta (lastPointerAngle, angle);
        lastPointerAngle = angle;

        if (fineNow)
            ringRelative = true;

        // 相对转动：按这次转过的角度挪（精调时慢 8 倍），不看绝对角度；否则值 = 指针所在的角度。
        const auto at = ringRelative ? juce::jlimit (0.0, 1.0, getDragProportion() + (double) (turned / dial::sweep) * (fineNow ? fineFactor : 1.0))
                                     : (double) ringProportion (e.position);

        flick.add (eventTimeMs (e), (float) at);
        dragTo (at, fineNow);
        startMotion();
    }

    void DialControl::mouseUp (const juce::MouseEvent& e)
    {
        const auto released = grab;
        const auto moved = ringMoved;
        grab = Grab::none;   // 先放开：下面设落点时 parameterValueChanged 不再当成在追手
        ringMoved = false;

        if (released == Grab::ring && moved && isDragging())
        {
            // 松手前 80 ms 内还在动：按速度多滑一段，落点在同一个手势里设好再结束手势。「减少动效」时不多滑：
            // 没有动画交代值为什么跑到了松手处以外（与改前一样没有惯性）。
            const auto velocity = reduceMotion ? 0.0f : flick.releaseVelocity (eventTimeMs (e));
            const auto landing = juce::jlimit (0.0, 1.0, (double) dial::momentumTarget ((float) getDragProportion(), velocity));

            if (! juce::exactlyEqual (landing, getDragProportion()))
                dragTo (landing, false);

            endDrag();
            settleFromRelease (dial::releaseBounce (velocity), velocity);
            return;
        }

        const auto wasDragging = isDragging();
        endDrag();

        // 中间上下拖：读数还落后 60 ms，从当前位置用点按的回弹停稳。外圈点按的弹簧在按下时已经起了。
        if (released == Grab::centre && wasDragging)
            settleFromRelease (dial::tapBounce, reading.getVelocity());
    }

    void DialControl::mouseDoubleClick (const juce::MouseEvent& e)
    {
        // 双击回默认是一次「外部」改值：读数起弹簧，这次按下剩下的拖动不再改值（ParameterControl 里收掉 dragActive）。
        grab = Grab::none;
        ringMoved = false;
        ParameterControl::mouseDoubleClick (e);
    }

    void DialControl::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        const juce::ScopedValueSetter<bool> jumping (jumpNextChange, true);
        ParameterControl::mouseWheelMove (e, wheel);
    }

    //==============================================================================
    void DialControl::drawComet (juce::Graphics& g, juce::Point<float> centre, float radius, float thickness, float extraWidth,
                                 bool bipolar, float saturation) const
    {
        const auto strength = comet.getStrength();

        if (reduceMotion || ! (strength > 0.0f))
            return;

        dial::CometSegments segments;
        const auto count = dial::cometSegments (dial::angleFor (reading.getPosition()), strength, comet.getDirection(), segments);

        for (int j = 0; j < count; ++j)
        {
            const auto& segment = segments[(size_t) j];
            const auto amount = segment.weight * strength;   // 头部最宽最实，往后渐细渐隐
            const auto middle = (0.5f * (segment.fromAngle + segment.toAngle) - dial::startAngle) / dial::sweep;

            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, segment.fromAngle, segment.toAngle, true);
            g.setColour (valueArcColour (middle, bipolar, saturation).withMultipliedAlpha (amount));
            g.strokePath (arc, juce::PathStrokeType (thickness + extraWidth * amount, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    //==============================================================================
    ParamKnob::ParamKnob (juce::RangedAudioParameter& p, InteractionHost& h, Options o)
        : DialControl (p, h), options (o)
    {
        setSize (width, height);
        setRepaintsOnMouseActivity (false);
    }

    DialControl::Geometry ParamKnob::getDialGeometry() const
    {
        return { smallCentre, smallArcRadius, juce::jmax (smallCapRadius, dial::deadZoneRatio * smallArcRadius), smallArcRadius + ringGrabMargin,
                 knobFullDragPixels };
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

        // 编辑器每帧都来（T-032 R9-12）：加量文字与实际值比例只由实际值与用户值决定，两者都与上次相同就什么都不会变，
        // 不必再拼一次字符串来比。
        if (effective == effectiveValue && juce::exactlyEqual (getValue(), effectiveBaseValue))
            return;

        effectiveValue = effective;
        effectiveBaseValue = getValue();

        // 与上次画出来的比：比例挪动超过阈值、或加量文字变了才重画。
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
        // 画读数（停稳时就是参数值）；GLAZE 的自动加量文字仍按参数值算。
        const auto proportion = juce::jlimit (0.0f, 1.0f, getReadingProportion());
        const auto centre = smallCentre;

        // 更大的名字与旋钮在小比例窗口下仍可辨认。
        g.setFont (uiFont (12.5f, Weight::medium));
        g.setColour (palette::ink2);
        g.drawText (name, juce::Rectangle<float> (0.0f, 0.0f, (float) width, 18.0f), juce::Justification::centred, false);

        // 轨道
        {
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, smallArcRadius, smallArcRadius, 0.0f, knob::startAngle, knob::endAngle, true);
            g.setColour (palette::knobTrack);
            g.strokePath (track, juce::PathStrokeType (smallArcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // 数值弧（单色青，越大越深；双极从正中长）
        drawValueArc (g, centre, smallArcRadius, smallArcWidth, options.bipolar ? 0.5f : 0.0f, proportion, 16, options.bipolar);

        // 转动时的彗尾：压在数值弧上、紫色的自动加量之下。
        drawComet (g, centre, smallArcRadius, smallArcWidth, smallCometExtraWidth, options.bipolar);

        // GLAZE 的实际值：从用户值到实际值的紫色弧 + 紫色小圆点
        const auto hasEffective = effectiveValue.has_value() && autoAmountText().isNotEmpty();
        const auto effectiveProportion = hasEffective ? getParameter().convertTo0to1 (*effectiveValue) : proportion;

        if (hasEffective)
        {
            juce::Path bridge;
            bridge.addCentredArc (centre.x, centre.y, smallArcRadius, smallArcRadius, 0.0f,
                                  knob::angleFor (proportion), knob::angleFor (effectiveProportion), true);
            g.setColour (palette::violet);
            g.strokePath (bridge, juce::PathStrokeType (smallArcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        drawKnobCap (g, centre, smallCapRadius);

        if (hasEffective)
        {
            const auto dot = knob::pointAt (centre, smallArcRadius, knob::angleFor (effectiveProportion));
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

        // 数值（跟着读数走）+ 紫色的自动加量（等宽字，居中）
        const auto valueFont = monoFont (13.0f, Weight::medium);
        const auto autoFont  = monoFont (12.0f);
        const auto valueText = formatParameterValue (getParameter(), getReadingValue(), false);
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
}
