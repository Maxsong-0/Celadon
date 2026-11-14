/*
  ==============================================================================

    TopBar.cpp

  ==============================================================================
*/

#include "TopBar.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        const juce::String& separator()
        {
            static const juce::String text = juce::String (" ") + juce::String::charToString ((juce::juce_wchar) 0x00b7) + " ";
            return text;
        }

        juce::Font pillFont (bool medium)   { return uiFont (TextSize::body, medium ? Weight::medium : Weight::regular); }
        juce::Font brandFont()              { return withLetterSpacing (uiFont (TextSize::title, Weight::semibold), 3.0f); }
        juce::Font subtitleFont()           { return withLetterSpacing (uiFont (TextSize::caption), 1.0f); }
        juce::Font advancedFont()           { return uiFont (TextSize::body); }

        constexpr float pillInset = 14.0f;   // 胶囊放不下全部内容时两端留的边

        // 设置键的齿轮轮廓：8 个梯形齿，齿顶半径 5.5、齿根半径 4，每齿顶部占 18°、根部占 27°。
        juce::Path gearOutline (juce::Point<float> centre)
        {
            constexpr int teeth = 8;
            constexpr float outer = 5.5f, inner = 4.0f;
            const auto step = juce::MathConstants<float>::twoPi / (float) teeth;
            const auto topHalf = juce::degreesToRadians (9.0f), rootHalf = juce::degreesToRadians (13.5f);
            const auto at = [centre] (float radius, float angle)
            {
                return juce::Point<float> (centre.x + radius * std::sin (angle), centre.y - radius * std::cos (angle));
            };

            juce::Path path;

            for (int i = 0; i < teeth; ++i)
            {
                const auto mid = step * (float) i;
                const auto rootStart = at (inner, mid - rootHalf);

                if (i == 0)
                    path.startNewSubPath (rootStart);
                else
                    path.lineTo (rootStart);

                path.lineTo (at (outer, mid - topHalf));
                path.lineTo (at (outer, mid + topHalf));
                path.lineTo (at (inner, mid + rootHalf));
            }

            path.closeSubPath();
            return path;
        }

        void drawWarningIcon (juce::Graphics& g, juce::Rectangle<float> area)
        {
            juce::Path triangle;
            triangle.addTriangle (area.getCentreX(), area.getY() + 1.0f, area.getRight() - 0.5f, area.getBottom() - 1.0f,
                                  area.getX() + 0.5f, area.getBottom() - 1.0f);
            g.setColour (palette::warning);
            g.fillPath (triangle);
            g.strokePath (triangle, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour (juce::Colours::white);
            g.drawLine (area.getCentreX(), area.getY() + 4.6f, area.getCentreX(), area.getY() + 7.4f, 1.4f);
            g.fillEllipse (juce::Rectangle<float> (1.6f, 1.6f).withCentre ({ area.getCentreX(), area.getY() + 9.1f }));
        }

        // 只在 area 与这次的脏区相交时才排版、画字（JUCE 只在光栅层裁剪，paint 里的整形照做）。area 左右各放宽 2 px：
        // 字形的墨迹可能越出排版框一点（负的左侧边距、字距），放宽后跳过的文字画了也全落在裁剪区外，结果逐位相同。
        // 胶囊呼吸时的脏区（圆点 + 光晕取整，右缘 ≤ 圆点左边 + 11）到第一段文字（圆点左边 + 14）放宽后的左缘还有 1 ～ 2 px。
        // 有缩放时脏区先向外取整到设备像素、clipRegionIntersects 再按包围盒换回用户坐标向外取整：1 倍及以上仍不相交；低于 1 倍
        // 一个设备像素比逻辑像素大，可能与第一段文字落进同一列设备像素——那时就得画那一段，跳过可能漏画墨迹（T-041）。
        bool needsDrawing (juce::Graphics& g, juce::Rectangle<float> area)
        {
            return g.clipRegionIntersects (area.expanded (2.0f, 0.0f).getSmallestIntegerContainer());
        }
    }

    //==============================================================================
    bool StatusPill::Content::operator== (const Content& other) const
    {
        return warning == other.warning && segments == other.segments && accentSegment == other.accentSegment
            && accentColour == other.accentColour && dotColour == other.dotColour && pulse == other.pulse;
    }

    StatusPill::StatusPill()
    {
        content.dotColour = palette::c500;
        measure();

        // 整颗胶囊缓存成按物理像素的图、整数对齐贴（R9-13）。聆听 / 校准中音乐在放，频谱每帧重画；macOS 只拿脏矩形的包围盒，
        // 频谱与圆点合起来的包围盒总把胶囊圈进去，只缩小 repaint 矩形的话每帧照样重排全部文字。缓存后：呼吸只作废圆点那一小块，
        // 缓存按那一块的裁剪重画（paint 跳过与脏区不相交的文字）；别处引起的重画只贴图。
        setBufferedToAlignedImage (*this);
    }

    StatusPill::~StatusPill()
    {
        stopTimer();
    }

    bool StatusPill::setContent (const Content& newContent)
    {
        if (newContent == content)
            return false;

        content = newContent;
        measure();
        layoutContent();

        if (content.pulse && ! reduceMotion)
            startTimerHz (30);
        else
            stopTimer();

        repaint();
        return true;
    }

    void StatusPill::setReduceMotion (bool shouldReduceMotion)
    {
        reduceMotion = shouldReduceMotion;

        if (content.pulse && ! reduceMotion)
            startTimerHz (30);
        else
            stopTimer();

        repaint();
    }

    void StatusPill::timerCallback()
    {
        advancePulse();
    }

    void StatusPill::advancePulse()
    {
        // 圆点呼吸：2.4 s 一个周期（设计稿 pulse 动画），每帧 1 / 30 s。只有圆点在变：只重画圆点那一小块。
        pulsePhase = std::fmod (pulsePhase + 1.0f / (30.0f * 2.4f), 1.0f);
        repaint (pulseArea);
    }

    void StatusPill::measure()
    {
        // 首选宽度与内容总宽沿用改版时的两种累加顺序（宽度取整、摆放居中各用各的，结果与逐帧重量时逐位相同）。
        separatorWidth = textWidth (pillFont (false), separator());
        segmentWidths.clear();

        auto total = content.warning ? 13.0f + 7.0f : 7.0f + 7.0f;
        contentWidth = total;

        for (int i = 0; i < content.segments.size(); ++i)
        {
            const auto w = textWidth (pillFont (content.warning || i == content.accentSegment), content.segments[i]);
            segmentWidths.push_back (w);

            total += w;

            if (i > 0)
                total += separatorWidth;

            contentWidth += w + (i > 0 ? separatorWidth : 0.0f);
        }

        preferredWidth = juce::jlimit (350, 560, (int) std::ceil (total + 40.0f));
    }

    void StatusPill::resized()
    {
        layoutContent();
    }

    void StatusPill::layoutContent()
    {
        items.clear();

        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto textHeight = bounds.getHeight() + 1.0f;

        // 放不下（顶栏两边的东西把胶囊限窄了）时从左边开始排，最后一段在右端加省略号。
        const auto right = bounds.getRight() - pillInset;
        auto x = contentWidth <= bounds.getWidth() - 2.0f * pillInset ? bounds.getCentreX() - contentWidth * 0.5f : bounds.getX() + pillInset;
        const auto centreY = bounds.getCentreY();

        markerX = x;
        x += content.warning ? 13.0f + 7.0f : 7.0f + 7.0f;

        // 圆点 7 × 7 + 3 px 光晕取整到逻辑像素。抗锯齿不出这个框：缓存（AlignedComponentCache）与窗口重画都把它向外取整到设备
        // 像素。不再多留 1 px（T-041）：多的那 1 px 在小数像素比下取整后与第一段文字的 2 px 字形余量碰上，每个呼吸帧都重排那段字。
        pulseArea = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ markerX + 3.5f, centreY }).expanded (3.0f)
                        .getSmallestIntegerContainer();

        for (int i = 0; i < content.segments.size() && x < right - 8.0f; ++i)
        {
            if (i > 0)
            {
                items.push_back ({ separator(), pillFont (false), content.warning ? palette::warningText : palette::separator,
                                   { x, 0.0f, separatorWidth + 1.0f, textHeight }, false });
                x += separatorWidth;
            }

            const auto accent = i == content.accentSegment;
            const auto w = segmentWidths[(size_t) i];
            items.push_back ({ content.segments[i], pillFont (content.warning || accent),
                               content.warning ? palette::warningText : (accent ? content.accentColour : palette::ink),
                               { x, 0.0f, juce::jmin (w + 2.0f, right - x), textHeight }, true });
            x += w;
        }
    }

    void StatusPill::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto radius = bounds.getHeight() * 0.5f;

        g.setColour (palette::shadowTint.withAlpha (0.05f));
        g.fillRoundedRectangle (bounds.translated (0.0f, 1.0f), radius);
        g.setColour (content.warning ? palette::warningSoft : palette::panel);
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (content.warning ? palette::warningBorder : palette::segmentBorder);
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        const auto centreY = bounds.getCentreY();

        if (content.warning)
        {
            drawWarningIcon (g, { markerX, centreY - 6.0f, 13.0f, 12.0f });
        }
        else
        {
            const auto alpha = content.pulse && ! reduceMotion ? 0.35f + 0.65f * (0.5f + 0.5f * std::cos (pulsePhase * juce::MathConstants<float>::twoPi)) : 1.0f;
            const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ markerX + 3.5f, centreY });
            g.setColour (content.dotColour.withAlpha (0.18f * alpha));
            g.fillEllipse (dot.expanded (3.0f));
            g.setColour (content.dotColour.withMultipliedAlpha (alpha));
            g.fillEllipse (dot);
        }

        for (const auto& item : items)
        {
            if (! needsDrawing (g, item.area))
                continue;

            ++textDraws;
            g.setFont (item.font);
            g.setColour (item.colour);
            drawTextLine (g, item.text, item.area, juce::Justification::centredLeft, item.ellipsis);
        }
    }

    //==============================================================================
    IconButton::IconButton (Icon i)
        : icon (i)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void IconButton::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (active ? palette::c100 : (isHovering() ? palette::groove : palette::panel));
        g.fillEllipse (bounds);
        g.setColour (active ? palette::relearnBorder : palette::controlBorder);
        g.drawEllipse (bounds, 1.0f);

        const auto centre = bounds.getCentre();

        if (icon == Icon::settings)
        {
            // 齿轮：8 齿实线轮廓（外径 11、齿根 8、中孔 4、线宽 1.4）。原来的虚线圆在 1 倍下像雪花（T-111 / 4-23）。
            g.setColour (palette::ink2);
            g.strokePath (gearOutline (centre), juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.drawEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (centre), 1.4f);
        }
        else
        {
            g.setFont (monoFont (TextSize::body));
            g.setColour (palette::ink2);
            drawTextLine (g, "?", bounds, juce::Justification::centred);
        }
    }

    //==============================================================================
    AdvancedButton::AdvancedButton()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void AdvancedButton::setText (const juce::String& newText)
    {
        if (text == newText)
            return;

        text = newText;
        repaint();
    }

    int AdvancedButton::getNaturalWidth() const
    {
        // 设计稿：左右各 12 px 内边距，12 px 图标，间隔 6。
        return (int) std::ceil (12.0f + 12.0f + 6.0f + textWidth (advancedFont(), text) + 12.0f);
    }

    void AdvancedButton::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (active ? palette::c100 : (isHovering() ? palette::groove : palette::panel));
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (active ? palette::relearnBorder : palette::controlBorder);
        g.drawRoundedRectangle (bounds, 8.0f, 1.0f);

        // 滑杆图标（设计稿 12 × 12）：两道横线，各带一个小圆钮。
        const auto icon = juce::Rectangle<float> (12.0f, 12.0f).withPosition (12.0f, bounds.getCentreY() - 6.0f);
        juce::Path lines;
        lines.startNewSubPath (icon.getX() + 1.0f, icon.getY() + 3.0f);  lines.lineTo (icon.getX() + 6.6f, icon.getY() + 3.0f);
        lines.startNewSubPath (icon.getX() + 9.4f, icon.getY() + 3.0f);  lines.lineTo (icon.getX() + 11.0f, icon.getY() + 3.0f);
        lines.startNewSubPath (icon.getX() + 1.0f, icon.getY() + 9.0f);  lines.lineTo (icon.getX() + 2.6f, icon.getY() + 9.0f);
        lines.startNewSubPath (icon.getX() + 5.4f, icon.getY() + 9.0f);  lines.lineTo (icon.getX() + 11.0f, icon.getY() + 9.0f);
        g.setColour (palette::ink2);
        g.strokePath (lines, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.drawEllipse (juce::Rectangle<float> (2.8f, 2.8f).withCentre ({ icon.getX() + 8.0f, icon.getY() + 3.0f }), 1.2f);
        g.drawEllipse (juce::Rectangle<float> (2.8f, 2.8f).withCentre ({ icon.getX() + 4.0f, icon.getY() + 9.0f }), 1.2f);

        g.setFont (advancedFont());
        g.setColour (palette::ink);
        drawTextLine (g, text, juce::Rectangle<float> (icon.getRight() + 6.0f, 0.0f, textWidth (advancedFont(), text) + 2.0f, (float) getHeight()),
                      juce::Justification::centredLeft);
    }

    //==============================================================================
    TopBar::TopBar()
    {
        addAndMakeVisible (pill);

        brandWidth = textWidth (brandFont(), "CELADON");
        subtitleText = tr (Text::brandSubtitle, language);
        subtitleWidth = textWidth (subtitleFont(), subtitleText);

        settingsButton.onClick = [this] { if (onSettings != nullptr) onSettings(); };
        helpButton.onClick = [this] { if (onHelp != nullptr) onHelp(); };
        advancedButton.onClick = [this] { if (onAdvanced != nullptr) onAdvanced(); };
        addAndMakeVisible (settingsButton);
        addAndMakeVisible (helpButton);
        addAndMakeVisible (advancedButton);
    }

    void TopBar::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        settingsButton.setTooltip (tr (Text::settings, language));
        helpButton.setTooltip (tr (Text::help, language));
        advancedButton.setText (tr (Text::advanced, language));
        advancedButton.setTooltip (help::tooltipFor (uiid::advanced, language));
        pill.setTooltip (help::tooltipFor (uiid::glazeStatus, language));

        subtitleText = tr (Text::brandSubtitle, language);
        subtitleWidth = textWidth (subtitleFont(), subtitleText);

        resized();   // 「高级」与副标题的宽度随语言变
        repaint();
    }

    void TopBar::setStatus (const StatusPill::Content& content)
    {
        // 内容没变时胶囊的首选宽度也不变；品牌与右边按钮的宽度变化在 setLanguage → resized 里重新摆放。
        if (pill.setContent (content))
            layoutPill();
    }

    float TopBar::brandRight() const
    {
        return 46.0f + brandWidth + 10.0f + subtitleWidth;
    }

    float TopBar::controlsLeft() const
    {
        return (float) settingsButton.getX();
    }

    void TopBar::layoutPill()
    {
        // 居中在 x = 600，两边各离品牌与设置键至少 16 px（按较窄的一侧对称收窄）。
        const auto room = 2.0f * juce::jmin (600.0f - (brandRight() + 16.0f), (controlsLeft() - 16.0f) - 600.0f);
        const auto w = juce::jmin (pill.getPreferredWidth(), juce::jmax (200, (int) std::floor (room)));

        if (w != pill.getWidth() || pill.getX() != 600 - w / 2)
            pill.setBounds (600 - w / 2, 12, w, 28);
    }

    void TopBar::resized()
    {
        const auto advancedWidth = advancedButton.getNaturalWidth();
        advancedButton.setBounds (1180 - advancedWidth, 12, advancedWidth, 28);
        helpButton.setBounds (advancedButton.getX() - 10 - 28, 12, 28, 28);
        settingsButton.setBounds (helpButton.getX() - 10 - 28, 12, 28, 28);
        layoutPill();
    }

    void TopBar::paint (juce::Graphics& g)
    {
        // 品牌：青色描边圆环（带淡光晕）+ CELADON + 副标题
        const juce::Point<float> ring (28.0f, 26.0f);
        g.setColour (palette::c500.withAlpha (0.14f));
        g.drawEllipse (juce::Rectangle<float> (22.0f, 22.0f).withCentre (ring), 3.0f);
        g.setColour (palette::c500);
        g.drawEllipse (juce::Rectangle<float> (13.5f, 13.5f).withCentre (ring), 2.5f);

        // 两段文字只在与脏区相交时画（状态胶囊呼吸时脏区只是胶囊里的圆点，这里什么都不用排版）。
        const juce::Rectangle<float> brandArea (46.0f, 0.0f, 140.0f, 52.0f);

        if (needsDrawing (g, brandArea))
        {
            ++textDraws;
            g.setFont (brandFont());
            g.setColour (palette::ink);
            drawTextLine (g, "CELADON", brandArea, juce::Justification::centredLeft);
        }

        // 副标题比品牌小两档，两者各自垂直居中时基线差约 1 px：下移 1 px 让两行字落在同一条基线上（T-111 / 4-25）。
        const juce::Rectangle<float> subtitleArea (46.0f + brandWidth + 10.0f, 1.0f, 160.0f, 52.0f);

        if (needsDrawing (g, subtitleArea))
        {
            ++textDraws;
            g.setFont (subtitleFont());
            g.setColour (palette::ink3);
            drawTextLine (g, subtitleText, subtitleArea, juce::Justification::centredLeft);
        }
    }
}
