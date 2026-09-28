/*
  ==============================================================================

    Knob.cpp
    名字（上）+ 旋钮 + 数值（下）。旋钮的画法在 CeladonLookAndFeel，这里只管手感与文字。

  ==============================================================================
*/

#include "Knob.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        constexpr float labelFontHeight    = 12.0f;
        constexpr int   fullDragPixels     = 250;    // 普通拖动：走满量程要拖的像素（JUCE 默认值，34 px 旋钮手感合适）
        constexpr double fineDragFactor    = 8.0;    // 按住 ⌘：慢 8 倍（规格要求至少 4 倍）

        // 小旋钮的滑条区向名字 / 数值区各多伸的像素：实际值小圆点连描边直径 7，放在 3 px 宽的弧上会超出弧的外缘 2 px，
        // 而弧的外缘正好贴着 44 px 滑条区的边——实际值在正上方附近时小圆点会被组件边界裁平。
        // CeladonLookAndFeel::drawRotarySlider 在小旋钮四周留同样的 2 px（两边共用 CeladonLookAndFeel.h 的 smallKnobMarkerBleed），
        // 旋钮本身仍是 44 → 本体 34、圆心不动；多出来的只是小圆点的出血。两个标签在滑条之后加入、画在上层、背景透明，
        // 12 px 文字离标签边至少 3 px，落不进这 2 px；鼠标在这 2 px 里仍归标签（与以前一样只出提示），手感不变。

        // 实际值比例比上次画的相差不超过这个数就不重画（0.0005 × 270° ≈ 0.14°，弧上不到 0.05 px，肉眼看不出）。
        constexpr double effectiveRepaintThreshold = 0.0005;

        // 270° 轨道：从 7 点钟（1.25π）顺时针到 5 点钟（2.75π），正上方是正中。
        constexpr float rotaryStartAngle = juce::MathConstants<float>::pi * 1.25f;
        constexpr float rotaryEndAngle   = juce::MathConstants<float>::pi * 2.75f;

        // 属性键只建一次：Identifier 从 const char* 构造要进全局字符串池（加锁 + 查找 + 临时 String），
        // 每 30 ms × 7 个旋钮反复建没必要。函数内静态，首次使用时构造，不依赖静态初始化顺序。
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
    }

    //==============================================================================
    void Knob::PrecisionSlider::mouseDown (const juce::MouseEvent& e)
    {
        // 基类负责：alt 单击回默认值、开始拖动手势（附件 → beginChangeGesture）。
        juce::Slider::mouseDown (e);

        // 与基类同一条件下才算开始拖动：alt 单击是「回默认」不是拖动；禁用时什么都不做。
        dragging = isEnabled()
                    && getMaximum() > getMinimum()
                    && e.mods.withoutMouseButtons() != juce::ModifierKeys (juce::ModifierKeys::altModifier);

        lastDragPosition = e.position;
        dragProportion = valueToProportionOfLength (getValue());
    }

    void Knob::PrecisionSlider::mouseDrag (const juce::MouseEvent& e)
    {
        // 故意不调基类：基类按「相对按下点的总位移」算值，拖到一半按下 / 松开 ⌘ 会跳；
        // 这里逐事件累加增量，修饰键随时切换都连续。
        if (! dragging || ! isEnabled())
            return;

        // 上下拖为主，左右也算（RotaryHorizontalVerticalDrag 的约定：右 / 上为增）。
        const auto delta = (double) ((e.position.x - lastDragPosition.x) + (lastDragPosition.y - e.position.y));
        lastDragPosition = e.position;

        const auto pixelsForFullRange = (double) fullDragPixels * (e.mods.isCommandDown() ? fineDragFactor : 1.0);

        // 每次都钳在 [0, 1]：拖过头再往回拖时立刻响应，不会先「空转」一段。
        dragProportion = juce::jlimit (0.0, 1.0, dragProportion + delta / pixelsForFullRange);

        // setValue 内部会按参数步进吸附（snapToLegalValue），并同步通知附件。
        setValue (proportionOfLengthToValue (dragProportion), juce::sendNotificationSync);
    }

    void Knob::PrecisionSlider::mouseUp (const juce::MouseEvent& e)
    {
        dragging = false;
        juce::Slider::mouseUp (e);   // 结束拖动手势（附件 → endChangeGesture）
    }

    //==============================================================================
    Knob::Knob (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                const juce::String& displayName, Size size)
    {
        setUpCommon (displayName, help::tooltipFor (parameterId), size);

        // 附件负责双向同步（拖旋钮 → 参数，DAW 自动化 → 旋钮），并把参数的范围、文字函数、
        // 双击默认值装进 slider。这里再按参数显式设一次双击默认值，不依赖附件的实现细节。
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterId, slider);

        if (auto* parameter = state.getParameter (parameterId))
            slider.setDoubleClickReturnValue (true, (double) parameter->convertFrom0to1 (parameter->getDefaultValue()));

        slider.onValueChange = [this] { updateValueText(); };
        updateValueText();
    }

    Knob::Knob (const char* helpId, const juce::String& displayName,
                const juce::String& placeholderValueText, double placeholderProportion, Size size)
    {
        setUpCommon (displayName, help::tooltipFor (helpId), size);

        // 占位：不绑参数，指针停在固定位置，数值写固定文字；整体禁用后不可拖、不可双击、不吃滚轮，
        // 但仍是 TooltipClient，悬停照样出提示。
        slider.setRange (0.0, 1.0);
        slider.setValue (juce::jlimit (0.0, 1.0, placeholderProportion), juce::dontSendNotification);
        valueLabel.setText (placeholderValueText, juce::dontSendNotification);

        setEnabled (false);
    }

    Knob::~Knob()
    {
        // 附件先于 slider 析构（成员按声明的相反顺序销毁），这里显式写出以免以后调整声明顺序时踩坑。
        slider.onValueChange = nullptr;
        attachment.reset();
    }

    void Knob::setUpCommon (const juce::String& displayName, const juce::String& tooltip, Size size)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (rotaryStartAngle, rotaryEndAngle, true);
        slider.setMouseDragSensitivity (fullDragPixels);
        slider.setScrollWheelEnabled (true);
        slider.setPopupMenuEnabled (false);

        // 关掉「按修饰键切换 velocity 模式」：⌘ 的精调由 PrecisionSlider 自己做，
        // 基类在 mouseDown 里那次内部 mouseDrag 也就永远走绝对模式、不会隐藏光标。
        slider.setVelocityModeParameters (1.0, 1, 0.0, false);

        if (size == Size::big)
            slider.getProperties().set (bigKnobKey(), true);

        slider.setTooltip (tooltip);
        addAndMakeVisible (slider);

        // 名字在上（text 色）、数值在下（textDim 色），都是 12 px，居中；列窄时允许略微压缩字宽。
        for (auto* label : { &nameLabel, &valueLabel })
        {
            label->setFont (celadonFont (labelFontHeight));
            label->setJustificationType (juce::Justification::centred);
            label->setBorderSize (juce::BorderSize<int> (0));
            label->setMinimumHorizontalScale (0.75f);
            label->setInterceptsMouseClicks (true, false);   // 要接住鼠标才会出提示
            label->setTooltip (tooltip);
            addAndMakeVisible (*label);
        }

        nameLabel.setColour (juce::Label::textColourId, palette::text);
        valueLabel.setColour (juce::Label::textColourId, palette::textDim);
        nameLabel.setText (displayName, juce::dontSendNotification);
    }

    void Knob::updateValueText()
    {
        // 附件把参数的 stringFromValue 装进了 slider，这里拿到的就是参数文字（如「+3.0」「100」）。
        valueLabel.setText (slider.getTextFromValue (slider.getValue()), juce::dontSendNotification);
    }

    void Knob::setEffectiveProportion (double proportion, bool show)
    {
        // 改的是组件属性并触发重画：只能在消息线程（或持有 MessageManager 锁时）调。
        JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED

        // NaN / Inf 说明调用方算错了：Debug 版断住；Release 版当作「不显示」——不知道实际值在哪就别画，
        // 也不能让 NaN 进下面的比较（比较全为 false 会让状态卡死）。
        jassert (! show || std::isfinite (proportion));
        const bool shouldShow = show && std::isfinite (proportion);

        if (! shouldShow)
        {
            // 从「显示」变成「不显示」才动手；本来就没显示时什么都不做（GLAZE = 0 时每 30 ms 都会走到这里）。
            if (! effectiveShown)
                return;

            effectiveShown = false;
            slider.getProperties().remove (effectiveProportionKey());
            slider.repaint();
            return;
        }

        const auto clamped = juce::jlimit (0.0, 1.0, proportion);

        // 与上次画出来的比：逐次小步累积也会在偏离超过阈值时重画，误差永远不超过 0.0005。
        if (effectiveShown && std::abs (clamped - effectiveProportion) <= effectiveRepaintThreshold)
            return;

        effectiveShown      = true;
        effectiveProportion = clamped;
        slider.getProperties().set (effectiveProportionKey(), clamped);
        slider.repaint();
    }

    void Knob::resized()
    {
        auto area = getLocalBounds();
        nameLabel.setBounds (area.removeFromTop (labelHeight));
        valueLabel.setBounds (area.removeFromBottom (labelHeight));

        // 小旋钮：滑条区向上下两个标签各多伸 smallKnobMarkerBleed（见常量处说明），LookAndFeel 四周留同样的白，旋钮大小与圆心不变。
        // 大旋钮不画实际值标记，滑条区照旧（132 → 本体 116）。看的是 slider 属性而不是记下来的 Size：
        // LookAndFeel 也按这个属性决定画法，两边永远一致。
        const bool big = static_cast<bool> (slider.getProperties()[bigKnobKey()]);
        slider.setBounds (big ? area : area.expanded (0, smallKnobMarkerBleed));
    }
}
