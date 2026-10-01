/*
  ==============================================================================

    GlazeKnob.cpp

  ==============================================================================
*/

#include "GlazeKnob.h"

#include "CeladonLookAndFeel.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr float centreXY          = (float) GlazeKnob::size * 0.5f;
        constexpr float ringRadius        = 102.0f;   // 数值弧
        constexpr float ringWidth         = 4.0f;
        constexpr float fullDragPixels    = 250.0f;   // 设计稿 knobH ('glaze', 250)
        constexpr double crossfadeMs      = 260.0;
        constexpr double minRenderGapMs   = 60.0;     // 拖动时两次生成瓷面至少隔这么久
        constexpr double scaleSettleMs    = 150.0;    // 缩放窗口时等像素比稳定下来再重新生成
        constexpr float comparingOpacity  = 0.55f;
        constexpr float comparingSaturation = 0.3f;

        double nowMs() noexcept   { return juce::Time::getMillisecondCounterHiRes(); }

        juce::Colour muted (juce::Colour colour, bool comparing)
        {
            return comparing ? colour.withSaturation (colour.getSaturation() * comparingSaturation) : colour;
        }
    }

    GlazeKnob::GlazeKnob (juce::RangedAudioParameter& glazeParameter, InteractionHost& h)
        : ParameterControl (glazeParameter, h)
    {
        setSize (size, size);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    GlazeKnob::~GlazeKnob()
    {
        stopTimer();
    }

    void GlazeKnob::setComparing (bool listeningToOriginal)
    {
        if (comparing == listeningToOriginal)
            return;

        comparing = listeningToOriginal;
        startTimerHz (60);
        repaint();
    }

    void GlazeKnob::parameterValueChanged()
    {
        repaint();

        if (! isTimerRunning())
            startTimerHz (60);
    }

    bool GlazeKnob::hitTest (int x, int y)
    {
        return juce::Point<float> ((float) x, (float) y).getDistanceFrom ({ centreXY, centreXY }) <= ringRadius + 8.0f;
    }

    juce::Point<float> GlazeKnob::getBubbleAnchor() const
    {
        // 设计稿：气泡中心在把手外 30 px（半径 132）处；InteractionHost 的锚点是气泡底边中点，往下挪半个气泡高。
        const auto point = knob::pointAt ({ centreXY, centreXY }, 132.0f, knob::angleFor (getProportion()));
        return point.translated (0.0f, 12.0f);
    }

    juce::Rectangle<int> GlazeKnob::getTextEntryArea() const
    {
        return { 80, 97, 64, 30 };
    }

    GlazeKnob::PaintStats GlazeKnob::getPaintStats() const noexcept
    {
        auto result = stats;
        result.porcelainLayerBuilds = renderer.getLayerBuilds();
        result.lastPorcelainMs = lastRenderCostMs;
        return result;
    }

    //==============================================================================
    bool GlazeKnob::needsRender() const noexcept
    {
        if (! current.image.isValid() || current.desaturated != comparing)
            return true;

        if (wantedPixelScale > 0.0f && std::abs (current.pixelScale - wantedPixelScale) > 1.0e-3f)
            return true;

        const auto proportion = getProportion();
        return porcelain::visibleCrackCount (proportion) != current.cracks || std::abs (proportion - current.proportion) > 0.004f;
    }

    void GlazeKnob::render (bool crossfade)
    {
        const auto started = nowMs();

        Rendered next;
        next.proportion  = getProportion();
        next.pixelScale  = wantedPixelScale > 0.0f ? wantedPixelScale : 1.0f;
        next.desaturated = comparing;
        next.cracks      = porcelain::visibleCrackCount (next.proportion);
        next.image       = renderer.render (next.proportion, next.pixelScale, next.desaturated);
        next.serial      = ++renderSerial;

        const auto finished = nowMs();

        if (crossfade && current.image.isValid() && ! reduceMotion)
        {
            previous = std::move (current);
            fade = 0.0f;
            fadeStartMs = finished;
        }
        else
        {
            previous = {};
            fade = 1.0f;
        }

        current = std::move (next);
        lastRenderMs = finished;
        lastRenderCostMs = finished - started;
        ++stats.porcelainRenders;
        DBG ("Celadon porcelain " << current.image.getWidth() << " px: " << juce::String (lastRenderCostMs, 2) << " ms");
        repaint();
    }

    void GlazeKnob::timerCallback()
    {
        const auto now = nowMs();

        if (fade < 1.0f)
        {
            fade = (float) juce::jlimit (0.0, 1.0, (now - fadeStartMs) / crossfadeMs);

            if (fade >= 1.0f)
                previous = {};

            repaint();
        }

        if (needsRender())
        {
            const auto scaleChanged = current.image.isValid() && std::abs (current.pixelScale - wantedPixelScale) > 1.0e-3f;
            const auto gap = juce::jmax (minRenderGapMs, 3.0 * lastRenderCostMs);

            if ((! scaleChanged || now - pixelScaleChangedMs >= scaleSettleMs) && now - lastRenderMs >= gap)
                render (! scaleChanged);

            return;
        }

        if (fade >= 1.0f)
            stopTimer();
    }

    //==============================================================================
    bool GlazeKnob::FaceKey::operator== (const FaceKey& other) const noexcept
    {
        return juce::exactlyEqual (value, other.value) && comparing == other.comparing && currentSerial == other.currentSerial
            && previousSerial == other.previousSerial && juce::exactlyEqual (fade, other.fade);
    }

    GlazeKnob::FaceKey GlazeKnob::currentFaceKey() const noexcept
    {
        FaceKey key;
        key.value = getValue();
        key.comparing = comparing;
        key.currentSerial = current.image.isValid() ? current.serial : 0;
        key.previousSerial = isCrossfading() ? previous.serial : 0;
        key.fade = isCrossfading() ? fade : 1.0f;
        return key;
    }

    void GlazeKnob::drawGlaze (juce::Graphics& g, const Rendered& rendered, float opacity, float pixelScale) const
    {
        if (! rendered.image.isValid() || opacity <= 0.0f)
            return;

        const auto imageSize = (float) rendered.image.getWidth();

        // 瓷面图按 rendered.pixelScale 生成（边长 round (180 × 像素比)），圆心对准旋钮中心。像素比与此刻一致：整数设备像素
        // 对齐贴（R9-07：最近邻、整块拷贝，圆心误差 ≤ 0.5 px）；不一致（缩放窗口后、新瓷面按新像素比生成前的约 150 ms）：
        // 按比例变换贴图，图覆盖逻辑区域 [22, 202]²。
        if (std::abs (rendered.pixelScale - pixelScale) <= 1.0e-4f * pixelScale)
        {
            const auto origin = juce::roundToInt (centreXY * pixelScale - imageSize * 0.5f);
            drawCachedImage (g, rendered.image, pixelScale, { origin, origin }, opacity);
            return;
        }

        const auto pixelsPerUnit = imageSize / (2.0f * porcelain::imageHalf);
        const auto transform = juce::AffineTransform::scale (1.0f / pixelsPerUnit)
                                   .translated (centreXY - porcelain::imageHalf, centreXY - porcelain::imageHalf);

        juce::Graphics::ScopedSaveState saved (g);
        g.setOpacity (opacity);
        g.drawImageTransformed (rendered.image, transform, false);
    }

    void GlazeKnob::paint (juce::Graphics& g)
    {
        const auto contextScale = physicalPixelScale (g);
        const auto pixelScale = juce::jmax (0.5f, contextScale);   // 瓷面最低按 0.5 倍生成

        if (! juce::approximatelyEqual (pixelScale, wantedPixelScale))
        {
            if (wantedPixelScale > 0.0f)
                pixelScaleChangedMs = nowMs();

            wantedPixelScale = pixelScale;
            startTimerHz (60);
        }

        // 第一次画还没有瓷面：当场生成一次（之后都在定时器里做）。
        if (! current.image.isValid())
            render (false);

        ++stats.paints;

        // 整颗旋钮的缓存图：键没变就直接贴（GLAZE 不动时的每一次 paint），变了按此刻的像素比重新合成一次。
        const auto key = currentFaceKey();

        if (! face.isValidFor (contextScale) || ! (key == faceKey))
        {
            face.render ({ 0.0f, 0.0f, (float) size, (float) size }, contextScale,
                         [this, contextScale] (juce::Graphics& fg) { paintFace (fg, contextScale); });
            faceKey = key;
            ++stats.faceRenders;
        }

        // 听原声：合成好的整颗旋钮按 55 % 贴，等价于原来把各元素画进 55 % 的透明图层（被瓷面盖住的影子、被把手盖住的轨道
        // 都不会透出来），但不再每次 paint 都开一层。
        face.draw (g, comparing ? comparingOpacity : 1.0f);
    }

    void GlazeKnob::paintFace (juce::Graphics& g, float pixelScale) const
    {
        const auto proportion = getProportion();
        const juce::Point<float> centre (centreXY, centreXY);
        const auto angle = knob::angleFor (proportion);

        // 影子：旋钮下方压扁的柔和椭圆（设计稿 cy 100、rx 84、ry 13、模糊 6、#0A3038 20 %）。
        {
            juce::Graphics::ScopedSaveState saved (g);
            const juce::Point<float> shadowCentre (centreXY, centreXY + 100.0f);
            constexpr float outer = 92.0f;
            g.addTransform (juce::AffineTransform::scale (1.0f, 21.0f / outer, shadowCentre.x, shadowCentre.y));
            juce::ColourGradient shadow (juce::Colour (0x290a3038), shadowCentre, juce::Colour (0x000a3038), shadowCentre.translated (outer, 0.0f), true);
            shadow.addColour (0.55, juce::Colour (0x1c0a3038));
            shadow.addColour (84.0 / outer, juce::Colour (0x080a3038));
            g.setGradientFill (shadow);
            g.fillEllipse (juce::Rectangle<float> (2.0f * outer, 2.0f * outer).withCentre (shadowCentre));
        }

        // 外圈：灰色 270° 轨道 + 分段青色数值弧（越往右越深）。
        {
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, knob::startAngle, knob::endAngle, true);
            g.setColour (palette::knobTrack);
            g.strokePath (track, juce::PathStrokeType (ringWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            drawValueArc (g, centre, ringRadius, ringWidth, 0.0f, proportion, 48, false, comparing ? comparingSaturation : 1.0f);
        }

        // 瓷面（交叉淡化中两张叠画）。
        if (isCrossfading())
        {
            drawGlaze (g, previous, 1.0f, pixelScale);
            drawGlaze (g, current, fade, pixelScale);
        }
        else
        {
            drawGlaze (g, current, 1.0f, pixelScale);
        }

        // 帽子边上的白色短指针（r 27 → 33：贴着帽子边，不压中间的数字）。
        {
            juce::Path needle;
            needle.startNewSubPath (knob::pointAt (centre, 27.0f, angle));
            needle.lineTo (knob::pointAt (centre, 33.0f, angle));
            g.setColour (juce::Colours::white);
            g.strokePath (needle, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // 把手：数值弧上的白色圆点 + 深青描边 + 柔影。
        {
            const auto handle = knob::pointAt (centre, ringRadius, angle);
            juce::ColourGradient shadow (juce::Colour (6, 49, 58).withAlpha (0.3f), handle.translated (0.0f, 1.0f),
                                         juce::Colour (6, 49, 58).withAlpha (0.0f), handle.translated (10.0f, 1.0f), true);
            g.setGradientFill (shadow);
            g.fillEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre (handle.translated (0.0f, 1.0f)));

            const auto dot = juce::Rectangle<float> (13.0f, 13.0f).withCentre (handle);
            g.setColour (juce::Colours::white);
            g.fillEllipse (dot);
            g.setColour (muted (palette::c800, comparing));
            g.drawEllipse (dot.reduced (1.0f), 2.0f);
        }

        // 帽子里的百分数：「45」细体 30 px 白色 +「%」11 px 浅青，基线对齐、整体居中。
        {
            const auto numberFont = uiFont (30.0f, Weight::light);
            const auto percentFont = uiFont (11.0f);
            const auto number = juce::String (juce::roundToInt (getValue()));
            const auto numberWidth = textWidth (withLetterSpacing (numberFont, -1.0f), number);
            const auto percentWidth = textWidth (percentFont, "%");
            const auto x = centreXY - (numberWidth + 1.0f + percentWidth) * 0.5f;
            const auto baseline = 94.0f + (34.0f - numberFont.getHeight()) * 0.5f + numberFont.getAscent();

            g.setColour (juce::Colours::white);
            g.setFont (withLetterSpacing (numberFont, -1.0f));
            g.drawSingleLineText (number, juce::roundToInt (x), juce::roundToInt (baseline));

            g.setColour (muted (palette::c200, comparing));
            g.setFont (percentFont);
            g.drawSingleLineText ("%", juce::roundToInt (x + numberWidth + 1.0f), juce::roundToInt (baseline));
        }
    }

    //==============================================================================
    void GlazeKnob::mouseDown (const juce::MouseEvent& e)
    {
        if (handleTextEntryClick (e) || ! e.mods.isLeftButtonDown() || ! isAdjustable())
            return;

        lastDragPosition = e.position;
        beginDrag (e.mods.isCommandDown());
    }

    void GlazeKnob::mouseDrag (const juce::MouseEvent& e)
    {
        if (! isDragging())
            return;

        const auto delta = (e.position.x - lastDragPosition.x) + (lastDragPosition.y - e.position.y);
        lastDragPosition = e.position;

        if (juce::exactlyEqual (delta, 0.0f))
            return;

        const auto fineNow = e.mods.isCommandDown();
        const auto pixels = (double) fullDragPixels / (fineNow ? fineFactor : 1.0);
        dragTo (getDragProportion() + (double) delta / pixels, fineNow);
    }

    void GlazeKnob::mouseUp (const juce::MouseEvent&)
    {
        endDrag();
    }
}
