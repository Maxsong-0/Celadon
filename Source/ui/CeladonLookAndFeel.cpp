/*
  ==============================================================================

    CeladonLookAndFeel.cpp
    「梅子青夜窑」外观的画法。几何尺寸以设计规格-界面 第 2 节为准：
    小旋钮本体 34 px、GLAZE 大旋钮约 116 px、胶囊高 22、气泡最宽 340。

  ==============================================================================
*/

#include "CeladonLookAndFeel.h"

#include "CrackleTexture.h"

#include <cmath>
#include <vector>

namespace celadon::ui
{
    namespace
    {
        // ---- 旋钮 ----
        constexpr float smallArcWidth         = 3.0f;    // 小旋钮弧线宽（规格：3 px 圆头）
        constexpr float bigArcWidth           = 6.0f;    // GLAZE 大旋钮弧线宽
        constexpr float maxArcWidthRatio      = 0.14f;   // 区域很小时弧线按比例收，别让弧线把旋钮吃掉
        constexpr float arcToBodyGap          = 2.0f;    // 弧线内缘到本体的空隙：44 px 外径 → 本体 34 px
        constexpr float pointerInnerRatio     = 0.32f;   // 指针从本体半径的 32% 画到 84%
        constexpr float pointerOuterRatio     = 0.84f;
        constexpr float smallPointerWidth     = 2.0f;
        constexpr float bigPointerWidth       = 3.0f;
        constexpr float minValueArcProportion = 0.001f;  // 正好在起点时不画数值弧（圆头会留下一个点）
        constexpr int   bigKnobCrackleDensity = 10;      // 旋钮面上约 10 条裂纹（CrackleTexture.h）
        constexpr juce::uint32 bigKnobCrackleSeed = 0x43454c41;   // 'CELA'：固定种子，每次打开一样

        // ---- GLAZE 实际值标记（ADR-006；设计规格-界面 第 2 节），只画在小旋钮上 ----
        constexpr float effectiveDotDiameter      = 5.0f;     // 实际值处的白瓷实心小圆点
        constexpr float effectiveDotOutline       = 1.0f;     // 小圆点外围一圈旋钮底色描边：白点压在梅子青弧上也分得开
        constexpr float effectiveArcAlpha         = 0.4f;     // 当前值 → 实际值的淡弧：梅子青 40%
        constexpr float minEffectiveArcProportion = 0.0005f;  // 实际值与当前值相差不到这个比例时只画小圆点：圆头淡弧会留下一个点

        // 小旋钮外缘到滑条区边缘的留白：小圆点连描边直径 7，放在 3 px 宽的弧的半径上会超出弧的外缘 (7 − 3) / 2 = 2 px，
        // 而弧的外缘贴着滑条区的边，不留白的话实际值在正上方附近时小圆点会被组件边界裁平。
        // Knob::resized 把小旋钮的滑条区向名字 / 数值区各多伸同样的 2 px（smallKnobMarkerBleed）：48 px 滑条区 → 留白后外径 44 → 本体 34，
        // 大小与圆心都与留白前一样。大旋钮不画标记，不留白。
        constexpr float smallKnobInset = (effectiveDotDiameter + 2.0f * effectiveDotOutline - smallArcWidth) * 0.5f;
        static_assert (juce::exactlyEqual (smallKnobInset, (float) smallKnobMarkerBleed),
                       "小圆点尺寸变了：同步改 CeladonLookAndFeel.h 的 smallKnobMarkerBleed（Knob::resized 也用它）");

        // 属性键只建一次：Identifier 从 const char* 构造要进全局字符串池（加锁 + 查找 + 临时 String），每次画都建没必要。
        // 函数内静态，首次使用时构造，不依赖静态初始化顺序。
        const juce::Identifier& bigKnobKey()
        {
            static const juce::Identifier key (bigKnobProperty);
            return key;
        }

        const juce::Identifier& effectiveProportionKey()
        {
            static const juce::Identifier key (effectiveProportionProperty);
            return key;
        }

        const juce::Identifier& accentOutlineKey()
        {
            static const juce::Identifier key (accentOutlineProperty);
            return key;
        }

        // ---- 胶囊开关 / 按钮 ----
        constexpr float capsuleHeight     = 22.0f;
        constexpr float capsuleTextHeight = 12.0f;
        constexpr float hoverBrighten     = 0.12f;       // 悬停「略提亮」的幅度
        constexpr float pressedBrighten   = 0.25f;       // 按下再亮一点
        constexpr float pressedFillAlpha  = 0.35f;       // 关状态按下时的淡填充
        constexpr float accentPressedFillAlpha = 0.18f;  // 梅子青描边胶囊按下时的淡填充（梅子青比描边色亮得多，淡一半才不像「开」）

        // ---- 提示气泡 ----
        constexpr float tooltipPadding     = 12.0f;
        constexpr float tooltipMaxWidth    = 340.0f;
        constexpr float tooltipCorner      = 8.0f;
        constexpr float tooltipTitleHeight = 13.0f;
        constexpr float tooltipBodyHeight  = 12.0f;
        constexpr float tooltipLineGap     = 4.0f;       // 行与行之间的额外间距（在字号本身之外）
        constexpr int   tooltipOffsetX     = 16;         // 相对鼠标热点的偏移：避开箭头光标
        constexpr int   tooltipOffsetY     = 20;
        constexpr int   tooltipFlipMargin  = 8;          // 翻到鼠标上方时与热点的距离

        // ---- Label ----
        constexpr float labelFontHeight        = 12.0f;
        constexpr float defaultLabelFontHeight = 15.0f;  // juce::Label 构造时的默认字号（juce_Label.h）

        //==============================================================================
        // 系统字体的中粗样式名。macOS 的系统字体（SF）按样式名取字重，名字必须与 CoreText 列出的一致：
        // JUCE 在 CoreTextTypeface::findBestAvailableStyle 里查不到就退回列表第 0 项，那可能是 Ultralight，
        // 所以先看列表再选。只查一次 CoreText，之后用缓存。一个候选都没有就退回常规——宁可不粗，也不能换字体族。
        // 界面字体族：优先「中文字体自带的拉丁字形」——苹方（macOS）/ 微软雅黑 UI（Windows）。
        // 原因（2026-09-24 夜间截图验收发现）：主字体用 SF、中文靠回退时，JUCE 按 SF 的字高缩放回退字形，
        // 同一行里中文比英文大一号、还显得更粗，气泡第 1 行与正文也对不上。中英文同一字体族就没有这个问题。
        juce::String findInterfaceTypefaceName()
        {
            const auto installed = juce::Font::findAllTypefaceNames();

            for (const auto* candidate : { "PingFang SC", "Microsoft YaHei UI" })
                if (installed.contains (candidate))
                    return candidate;

            return juce::Font::getSystemUIFontName();
        }

        const juce::String& interfaceTypefaceName()
        {
            static const juce::String name = findInterfaceTypefaceName();
            return name;
        }

        juce::String findMediumStyleName()
        {
            const auto styles = juce::Font::findAllTypefaceStyles (interfaceTypefaceName());

            for (const auto* candidate : { "Medium", "Semibold", "Demibold", "Bold" })
                if (styles.contains (candidate))
                    return candidate;

            return "Regular";
        }

        const juce::String& mediumStyleName()
        {
            static const juce::String name = findMediumStyleName();
            return name;
        }

        //==============================================================================
        // 胶囊：高 22、全圆角；组件比 22 高时垂直居中。
        juce::Rectangle<float> capsuleBounds (const juce::Component& component)
        {
            const auto bounds = component.getLocalBounds().toFloat();
            return bounds.withSizeKeepingCentre (bounds.getWidth(), juce::jmin (capsuleHeight, bounds.getHeight()));
        }

        // 开 = 实心梅子青底；关 = 只有描边。悬停把填充 / 描边略提亮，按下再亮一点。
        void drawCapsule (juce::Graphics& g, juce::Rectangle<float> capsule, bool on, bool highlighted, bool down)
        {
            const auto radius = capsule.getHeight() * 0.5f;
            const auto lift   = down ? pressedBrighten : (highlighted ? hoverBrighten : 0.0f);

            if (on)
            {
                g.setColour (palette::accent.brighter (lift));
                g.fillRoundedRectangle (capsule, radius);
                return;
            }

            if (down)
            {
                // 关状态按下时给一点淡淡的填充，点击有回馈。
                g.setColour (palette::knobStroke.withAlpha (pressedFillAlpha));
                g.fillRoundedRectangle (capsule, radius);
            }

            // 描边本身很暗，提亮幅度加倍才看得出来。半像素内缩让 1 px 线落在整像素上。
            g.setColour (palette::knobStroke.brighter (lift * 2.0f));
            g.drawRoundedRectangle (capsule.reduced (0.5f), radius - 0.5f, 1.0f);
        }

        // 胶囊文字：12 px 居中；开 = 底色深字，关 = 白瓷色字。
        void drawCapsuleText (juce::Graphics& g, juce::Rectangle<float> capsule, const juce::String& text, bool on)
        {
            g.setFont (celadonFont (capsuleTextHeight));
            g.setColour (on ? palette::background : palette::text);
            g.drawText (text, capsule, juce::Justification::centred, true);
        }

        // 梅子青描边胶囊（accentOutlineProperty，「重新聆听」）：不填底、1 px 梅子青描边；悬停略提亮，按下再亮一点并给一层
        // 淡淡的梅子青填充，点击有回馈。它是动作按钮不是开关，没有「开」的样子——聆听中只换文字（规格第 1 节）。
        float accentLift (bool highlighted, bool down) noexcept
        {
            return down ? pressedBrighten : (highlighted ? hoverBrighten : 0.0f);
        }

        void drawAccentOutlineCapsule (juce::Graphics& g, juce::Rectangle<float> capsule, bool highlighted, bool down)
        {
            const auto radius = capsule.getHeight() * 0.5f;

            if (down)
            {
                g.setColour (palette::accent.withAlpha (accentPressedFillAlpha));
                g.fillRoundedRectangle (capsule, radius);
            }

            // 半像素内缩让 1 px 线落在整像素上（与普通胶囊同一种画法）。
            g.setColour (palette::accent.brighter (accentLift (highlighted, down)));
            g.drawRoundedRectangle (capsule.reduced (0.5f), radius - 0.5f, 1.0f);
        }

        void drawAccentOutlineCapsuleText (juce::Graphics& g, juce::Rectangle<float> capsule, const juce::String& text,
                                           bool highlighted, bool down)
        {
            g.setFont (celadonFont (capsuleTextHeight));
            g.setColour (palette::accent.brighter (accentLift (highlighted, down)));
            g.drawText (text, capsule, juce::Justification::centred, true);
        }

        //==============================================================================
        // 气泡文字：按 '\n' 分行，每行单独排版（第 1 行 13 px 中粗，其余 12 px 常规），行间加 tooltipLineGap。
        // 行内超过最大宽度就自动换行：JUCE 9 的 TextLayout 按 Unicode 换行规则（UAX #14）找断点，
        // 汉字之间是直接断点，所以中文逐字换行也正确，不需要 byChar。
        struct TooltipLayout
        {
            std::vector<juce::TextLayout> rows;
            std::vector<float> rowHeights;
            float width  = 0.0f;
            float height = 0.0f;
        };

        TooltipLayout layoutTooltip (const juce::String& text)
        {
            TooltipLayout result;
            const auto maxTextWidth = tooltipMaxWidth - tooltipPadding * 2.0f;
            const auto lines = juce::StringArray::fromLines (text);

            for (int i = 0; i < lines.size(); ++i)
            {
                const auto font = i == 0 ? celadonFont (tooltipTitleHeight, true)
                                         : celadonFont (tooltipBodyHeight);

                juce::AttributedString attributed;
                attributed.setJustification (juce::Justification::topLeft);
                attributed.setWordWrap (juce::AttributedString::byWord);
                attributed.append (lines[i], font, palette::tooltipText);

                juce::TextLayout layout;
                layout.createLayout (attributed, maxTextWidth);

                // 空行也占一行的高度，气泡不会因为某行没字而塌掉。
                const auto rowHeight = juce::jmax (layout.getHeight(), font.getHeight());

                result.width  = juce::jmax (result.width, layout.getWidth());
                result.height += rowHeight + (i > 0 ? tooltipLineGap : 0.0f);
                result.rowHeights.push_back (rowHeight);
                result.rows.push_back (std::move (layout));
            }

            return result;
        }
    }

    //==============================================================================
    juce::Font celadonFont (float height, bool medium)
    {
        // 字体族见 interfaceTypefaceName()：苹方 / 微软雅黑 UI，都没有时退回系统 UI 字体（SF / Segoe UI）。
        // 不能用不带名字的 FontOptions (height)——那是「默认无衬线」占位符，macOS 上落到 Lucida Grande。
        // FontOptions 默认 fallbackEnabled = true（juce_FontOptions.h），缺字时仍会回退。
        return juce::Font (juce::FontOptions (interfaceTypefaceName(),
                                              medium ? mediumStyleName() : juce::String ("Regular"),
                                              juce::jmax (1.0f, height)));
    }

    //==============================================================================
    CeladonLookAndFeel::CeladonLookAndFeel()
        : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (palette::background,    // windowBackground
                                                                     palette::panel,         // widgetBackground
                                                                     palette::panel,         // menuBackground
                                                                     palette::panelStroke,   // outline
                                                                     palette::text,          // defaultText
                                                                     palette::accent,        // defaultFill
                                                                     palette::background,    // highlightedText
                                                                     palette::accent,        // highlightedFill
                                                                     palette::text))         // menuText
    {
        const auto transparent = juce::Colours::transparentBlack;

        // 窗口 / 面板
        setColour (juce::ResizableWindow::backgroundColourId, palette::background);
        setColour (juce::DocumentWindow::textColourId, palette::text);

        // Label（旋钮名字与数值）
        setColour (juce::Label::backgroundColourId, transparent);
        setColour (juce::Label::textColourId, palette::text);
        setColour (juce::Label::outlineColourId, transparent);
        setColour (juce::Label::textWhenEditingColourId, palette::text);
        setColour (juce::Label::backgroundWhenEditingColourId, palette::panel);
        setColour (juce::Label::outlineWhenEditingColourId, palette::accent);

        // Slider：旋钮本体由 drawRotarySlider 用 palette 直接画，这里管的是文本框与线性滑条的默认色。
        setColour (juce::Slider::backgroundColourId, transparent);
        setColour (juce::Slider::thumbColourId, palette::text);
        setColour (juce::Slider::trackColourId, palette::track);
        setColour (juce::Slider::rotarySliderFillColourId, palette::accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, palette::track);
        setColour (juce::Slider::textBoxTextColourId, palette::text);
        setColour (juce::Slider::textBoxBackgroundColourId, transparent);
        setColour (juce::Slider::textBoxHighlightColourId, palette::accent.withAlpha (0.35f));
        setColour (juce::Slider::textBoxOutlineColourId, transparent);

        // 文本编辑态（双击数值改数字时）
        setColour (juce::TextEditor::backgroundColourId, palette::panel);
        setColour (juce::TextEditor::textColourId, palette::text);
        setColour (juce::TextEditor::highlightColourId, palette::accent.withAlpha (0.35f));
        setColour (juce::TextEditor::highlightedTextColourId, palette::text);
        setColour (juce::TextEditor::outlineColourId, palette::knobStroke);
        setColour (juce::TextEditor::focusedOutlineColourId, palette::accent);
        setColour (juce::CaretComponent::caretColourId, palette::accent);

        // 开关与按钮：胶囊由 drawToggleButton / drawButtonBackground 画，这里是 JUCE 内部会读的默认值。
        setColour (juce::ToggleButton::textColourId, palette::text);
        setColour (juce::ToggleButton::tickColourId, palette::accent);
        setColour (juce::ToggleButton::tickDisabledColourId, palette::knobStroke);
        setColour (juce::TextButton::buttonColourId, palette::panel);
        setColour (juce::TextButton::buttonOnColourId, palette::accent);
        setColour (juce::TextButton::textColourOffId, palette::text);
        setColour (juce::TextButton::textColourOnId, palette::background);
        setColour (juce::ComboBox::outlineColourId, palette::knobStroke);

        // 提示气泡
        setColour (juce::TooltipWindow::backgroundColourId, palette::tooltipFill);
        setColour (juce::TooltipWindow::textColourId, palette::tooltipText);
        setColour (juce::TooltipWindow::outlineColourId, palette::accent);
        setColour (juce::BubbleComponent::backgroundColourId, palette::tooltipFill);
        setColour (juce::BubbleComponent::outlineColourId, palette::accent);

        // 弹出菜单
        setColour (juce::PopupMenu::backgroundColourId, palette::panel);
        setColour (juce::PopupMenu::textColourId, palette::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::accent);
        setColour (juce::PopupMenu::highlightedTextColourId, palette::background);

        // 「默认无衬线」占位符换成系统字体族：只对经过默认 LookAndFeel 解析、又没指定名字的字体生效
        // （JUCE 内部控件之类）；自己的文字一律走 celadonFont。CoreText 列不出常规与粗体就不换，免得字重丢失。
        if (const auto system = juce::Typeface::findSystemTypeface(); system != nullptr)
        {
            const auto styles = juce::Font::findAllTypefaceStyles (system->getName());

            if (styles.contains ("Regular") && styles.contains ("Bold"))
                setDefaultSansSerifTypefaceName (system->getName());
        }
    }

    CeladonLookAndFeel::~CeladonLookAndFeel() = default;

    //==============================================================================
    void CeladonLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                               juce::Slider& slider)
    {
        const auto& properties = slider.getProperties();
        const bool big = static_cast<bool> (properties[bigKnobKey()]);

        // 几何：整个旋钮（弧线外缘）撑满区域里的最大正方形，小旋钮再四周各留 smallKnobInset 给实际值小圆点出血；
        // 本体直径 = 外径 − 2 × (弧宽 + 空隙)。小旋钮 48 px 滑条区 → 外径 44 → 本体 34 px；
        // 大旋钮 132 px 区域 → 本体 116 px；区域别的尺寸时按同样规则缩放。
        const auto area  = juce::Rectangle<int> (x, y, width, height).toFloat();
        const auto outer = juce::jmin (area.getWidth(), area.getHeight()) - (big ? 0.0f : 2.0f * smallKnobInset);

        if (outer <= 0.0f)
            return;

        const auto centre     = area.getCentre();
        const auto arcWidth   = juce::jmin (big ? bigArcWidth : smallArcWidth, outer * maxArcWidthRatio);
        const auto arcRadius  = outer * 0.5f - arcWidth * 0.5f;
        const auto bodyRadius = juce::jmax (1.0f, outer * 0.5f - arcWidth - arcToBodyGap);
        const auto body       = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
        const auto toAngle    = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        // GLAZE 实际值（ADR-006）：属性存在才画，且只画在小旋钮上。值由 Knob::setEffectiveProportion 钳过，
        // 这里再钳一次、拒掉非有限值——画法不依赖写入方守规矩。
        bool  showEffective       = false;
        float effectiveProportion = 0.0f;

        if (! big)
        {
            if (const auto* stored = properties.getVarPointer (effectiveProportionKey()))
            {
                const auto storedProportion = static_cast<double> (*stored);

                if (std::isfinite (storedProportion))
                {
                    showEffective       = true;
                    effectiveProportion = (float) juce::jlimit (0.0, 1.0, storedProportion);
                }
            }
        }

        // 本体。禁用状态照常画：整块变暗由 ModulePanel 的「即将推出」蒙层负责。
        g.setColour (palette::knobBody);
        g.fillEllipse (body);

        if (big)
        {
            // GLAZE 旋钮面上的开片纹：裁成圆再贴缓存图。图按当前物理像素比生成（显示缩放 × 界面缩放），
            // 缩放后依然清晰；比例或直径没变时这里只是一次贴图。
            const auto pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();
            const auto& crackle   = getBigKnobCrackle (juce::roundToInt (bodyRadius * 2.0f), pixelScale);

            if (crackle.isValid())
            {
                juce::Graphics::ScopedSaveState saved (g);
                juce::Path clip;
                clip.addEllipse (body);

                if (g.reduceClipRegion (clip))
                    g.drawImage (crackle, body);
            }
        }

        g.setColour (palette::knobStroke);
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        // 轨道弧：270°（rotaryStartAngle → rotaryEndAngle），圆头。
        const juce::PathStrokeType arcStroke (arcWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (palette::track);
        g.strokePath (track, arcStroke);

        // 数值弧：从起点到当前值。
        if (sliderPosProportional > minValueArcProportion)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, toAngle, true);
            g.setColour (palette::accent);
            g.strokePath (value, arcStroke);
        }

        // 实际值标记：淡弧 → 小圆点，都在数值弧之上、指针之下（指针仍表示用户值）。
        // 旋钮整体变暗（Component::setAlpha 走透明图层、「即将推出」蒙层盖在子组件上）时标记跟着一起暗，这里不用另管。
        if (showEffective)
        {
            const auto effectiveAngle = rotaryStartAngle + effectiveProportion * (rotaryEndAngle - rotaryStartAngle);

            // ① 淡弧：从当前值到实际值，与数值弧同半径、同线宽、同圆头，梅子青 40%。
            // addCentredArc 两个方向都会走（实际值小于当前值时倒着画）。起点的圆头压在数值弧的圆头上，同色看不出；
            // 两者几乎重合时不画，否则圆头会在当前值处多出一个点。
            if (std::abs (effectiveProportion - sliderPosProportional) >= minEffectiveArcProportion)
            {
                juce::Path bridge;
                bridge.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, toAngle, effectiveAngle, true);
                g.setColour (palette::accent.withAlpha (effectiveArcAlpha));
                g.strokePath (bridge, arcStroke);
            }

            // ② 小圆点：圆心在数值弧的半径上；先画大一圈的旋钮底色圆、再画白瓷圆，外围正好剩 1 px 描边。
            // 区域很小、弧线被按比例收窄时小圆点同比例收，超出弧外缘的量始终不超过留白。
            const auto dotScale     = arcWidth / smallArcWidth;
            const auto dotDiameter  = effectiveDotDiameter * dotScale;
            const auto ringDiameter = dotDiameter + 2.0f * effectiveDotOutline * dotScale;
            const auto dotCentre    = centre.getPointOnCircumference (arcRadius, effectiveAngle);

            g.setColour (palette::knobBody);
            g.fillEllipse (juce::Rectangle<float> (ringDiameter, ringDiameter).withCentre (dotCentre));
            g.setColour (palette::text);
            g.fillEllipse (juce::Rectangle<float> (dotDiameter, dotDiameter).withCentre (dotCentre));
        }

        // 白色指针：本体半径的 32% → 84%，圆头。指针表示用户值，永远在最上层。
        juce::Path pointer;
        pointer.startNewSubPath (centre.getPointOnCircumference (bodyRadius * pointerInnerRatio, toAngle));
        pointer.lineTo (centre.getPointOnCircumference (bodyRadius * pointerOuterRatio, toAngle));
        g.setColour (palette::text);
        g.strokePath (pointer, juce::PathStrokeType (big ? bigPointerWidth : smallPointerWidth,
                                                     juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const juce::Image& CeladonLookAndFeel::getBigKnobCrackle (int diameter, float pixelScale)
    {
        if (bigKnobCrackle.isNull()
             || diameter != bigKnobCrackleDiameter
             || ! juce::approximatelyEqual (pixelScale, bigKnobCrackleScale))
        {
            bigKnobCrackle         = renderCrackleImage (diameter, diameter, pixelScale, bigKnobCrackleSeed, bigKnobCrackleDensity);
            bigKnobCrackleDiameter = diameter;
            bigKnobCrackleScale    = pixelScale;
        }

        return bigKnobCrackle;
    }

    //==============================================================================
    void CeladonLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
    {
        const auto capsule = capsuleBounds (button);
        const auto on      = button.getToggleState();

        drawCapsule (g, capsule, on, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
        drawCapsuleText (g, capsule, button.getButtonText(), on);
    }

    void CeladonLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
    {
        // 传进来的 backgroundColour 是按钮自己的 buttonColourId；胶囊只认 palette，不用它。
        if (static_cast<bool> (button.getProperties()[accentOutlineKey()]))
        {
            drawAccentOutlineCapsule (g, capsuleBounds (button), shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            return;
        }

        drawCapsule (g, capsuleBounds (button), button.getToggleState(),
                     shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }

    void CeladonLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                             bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
    {
        if (static_cast<bool> (button.getProperties()[accentOutlineKey()]))
        {
            drawAccentOutlineCapsuleText (g, capsuleBounds (button), button.getButtonText(),
                                          shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            return;
        }

        drawCapsuleText (g, capsuleBounds (button), button.getButtonText(), button.getToggleState());
    }

    //==============================================================================
    juce::Rectangle<int> CeladonLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                               juce::Rectangle<int> parentArea)
    {
        // screenPos 与 parentArea 在同一坐标系里：有父组件时是父组件坐标，否则是屏幕坐标（TooltipWindow 负责换算）。
        const auto layout = layoutTooltip (tipText);
        const auto w = (int) std::ceil (layout.width  + tooltipPadding * 2.0f);
        const auto h = (int) std::ceil (layout.height + tooltipPadding * 2.0f);

        // 首选鼠标右下方；下方放不下就翻到上方，免得光标压住文字；最后无论如何不超出父区域。
        const auto x = screenPos.x + tooltipOffsetX;
        auto y = screenPos.y + tooltipOffsetY;

        if (y + h > parentArea.getBottom())
            y = screenPos.y - h - tooltipFlipMargin;

        return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
    }

    void CeladonLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
    {
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);

        // 编辑器把 TooltipWindow 设成了透明（setOpaque (false)），圆角外的四角由父组件画，这里不填底色。
        g.setColour (palette::tooltipFill);
        g.fillRoundedRectangle (bounds, tooltipCorner);

        g.setColour (palette::accent);
        g.drawRoundedRectangle (bounds.reduced (0.5f), tooltipCorner, 1.0f);

        const auto layout = layoutTooltip (text);
        auto rowY = tooltipPadding;

        for (size_t i = 0; i < layout.rows.size(); ++i)
        {
            layout.rows[i].draw (g, juce::Rectangle<float> (tooltipPadding, rowY, layout.width, layout.rowHeights[i]));
            rowY += layout.rowHeights[i] + tooltipLineGap;
        }
    }

    //==============================================================================
    juce::Font CeladonLookAndFeel::getLabelFont (juce::Label& label)
    {
        // 没显式设过字体的 Label（还是 juce::Label 构造时的默认：15 px、默认无衬线占位名）统一成 12 px 系统字体；
        // 调用方 setFont 过别的（比如 40 px 的整体响度、带字距的标题）就原样尊重，否则 setFont 会被这里悄悄吃掉。
        const auto requested = label.getFont();

        const bool isJuceDefault = juce::approximatelyEqual (requested.getHeight(), defaultLabelFontHeight)
                                    && requested.getTypefaceName() == juce::Font::getDefaultSansSerifFontName();

        return isJuceDefault ? celadonFont (labelFontHeight) : requested;
    }

    //==============================================================================
    void CeladonLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
    {
        const auto size = (float) juce::jmin (w, h);
        g.setColour ((isMouseOver || isMouseDragging) ? palette::accent : palette::textDim);

        // 三道与右下角等距的斜线，越靠角越短。
        for (int i = 1; i <= 3; ++i)
        {
            const auto offset = size * 0.28f * (float) i;
            g.drawLine ((float) w - offset, (float) h - 1.0f, (float) w - 1.0f, (float) h - offset, 1.2f);
        }
    }
}
