/*
  ==============================================================================

    ModulePanel.cpp
    模块面板：标题行（可带附件）、旋钮行（最多 5 格，右侧可放三选一胶囊）、底部开关行，
    以及 Balance 曲线带（27 段修正曲线组件 / 占位虚线）与「即将推出」蒙层。

  ==============================================================================
*/

#include "ModulePanel.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"

namespace celadon::ui
{
    namespace
    {
        constexpr float cornerRadius      = 10.0f;
        constexpr int   padding           = 14;    // 面板内边距
        constexpr int   knobRowInset      = 4;     // 旋钮行左右只留 4：5 格每格约 54 px，「Lookahead」几乎不用横向压缩
        constexpr int   titleHeight       = 16;
        constexpr float titleFontHeight   = 13.0f;
        constexpr float titleKerning      = 2.0f / titleFontHeight;   // 字距 2 px（extraKerning 按字高比例算）
        constexpr int   accessoryWidth    = 110;
        constexpr int   accessoryHeight   = 14;
        constexpr int   rowGap            = 6;     // 标题行 / 开关行与中间内容的间距
        constexpr int   curveBandHeight   = 44;    // Balance 曲线占位带
        constexpr int   maxKnobColumns    = 5;
        constexpr int   segmentedColumns  = 2;     // Speed 胶囊占旋钮行右侧两格（胶囊本身的尺寸见 CapsuleSelector）
        constexpr int   toggleWidth       = 72;
        constexpr int   toggleHeight      = 22;
        constexpr int   toggleGap         = 12;
        constexpr float comingSoonFont    = 14.0f;
    }

    //==============================================================================
    ModulePanel::ModulePanel (const juce::String& titleText)
        : title (titleText)
    {
        // 三选一胶囊用了（addSegmented / addPlaceholderSegmented）才显示。
        addChildComponent (segmented);
    }

    ModulePanel::~ModulePanel()
    {
        // 附件先走（它们监听着按钮 / 参数），再销毁按钮与旋钮；三选一胶囊的附件由 CapsuleSelector 自己先拆。
        // 面板不拥有 accessory 与 curve，父组件析构只会把它们摘下来。
        toggleAttachments.clear();
    }

    //==============================================================================
    Knob& ModulePanel::addKnob (std::unique_ptr<Knob> knob)
    {
        jassert (knob != nullptr);
        auto* added = knobs.add (knob.release());
        addAndMakeVisible (*added);
        resized();
        return *added;
    }

    juce::ToggleButton& ModulePanel::addToggleButton (const juce::String& label, const juce::String& tooltip)
    {
        auto* button = toggles.add (new juce::ToggleButton (label));
        button->setTooltip (tooltip);
        addAndMakeVisible (*button);
        resized();
        return *button;
    }

    juce::ToggleButton& ModulePanel::addToggle (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                                                const juce::String& label)
    {
        auto& button = addToggleButton (label, help::tooltipFor (parameterId));
        toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, parameterId, button));
        return button;
    }

    juce::ToggleButton& ModulePanel::addPlaceholderToggle (const char* helpId, const juce::String& label, bool on)
    {
        auto& button = addToggleButton (label, help::tooltipFor (helpId));
        button.setToggleState (on, juce::dontSendNotification);
        button.setEnabled (false);   // 不可点；TooltipWindow 不看 isEnabled，悬停仍有提示
        return button;
    }

    void ModulePanel::addPlaceholderSegmented (const char* helpId, const juce::String& caption,
                                               const juce::StringArray& options, int selectedIndex)
    {
        // 占位不绑参数：之前若绑过（addSegmented），CapsuleSelector 会先解除附件再拆胶囊。
        segmented.showPlaceholder (helpId, caption, options, selectedIndex);
        segmented.setVisible (true);
        hasSegmented = true;
        resized();
    }

    void ModulePanel::addSegmented (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                                    const juce::String& caption, const juce::StringArray& options)
    {
        // 绑定、提示、双向同步都在 CapsuleSelector 里（与 GLAZE 的风格胶囊同一份实现）。
        segmented.bind (state, parameterId, caption, options);
        segmented.setVisible (true);
        hasSegmented = true;
        resized();
    }

    void ModulePanel::setCurveComponent (juce::Component* newCurve)
    {
        if (curve == newCurve)
            return;

        if (curve != nullptr)
            removeChildComponent (curve);

        curve = newCurve;

        if (curve != nullptr)
            addAndMakeVisible (*curve);

        resized();
        repaint();
    }

    void ModulePanel::setTitleAccessory (juce::Component* newAccessory)
    {
        if (accessory == newAccessory)
            return;

        if (accessory != nullptr)
            removeChildComponent (accessory);

        accessory = newAccessory;

        if (accessory != nullptr)
            addAndMakeVisible (*accessory);

        resized();
    }

    void ModulePanel::setShowsBalanceCurvePlaceholder (bool shouldShow)
    {
        if (showsCurvePlaceholder == shouldShow)
            return;

        showsCurvePlaceholder = shouldShow;
        resized();
        repaint();
    }

    void ModulePanel::setComingSoon (bool shouldBeComingSoon)
    {
        if (comingSoon == shouldBeComingSoon)
            return;

        comingSoon = shouldBeComingSoon;

        // 禁用面板本身：Component::isEnabled 会沿父链查，所有子控件（旋钮、开关、附件）一并失效，
        // 且不改子控件自己的 enabled 标志——占位旋钮本来就禁用，取消蒙层时也不会被误开。
        // 提示气泡不受影响：TooltipWindow::getTipFor 不检查 isEnabled。
        setEnabled (! comingSoon);
        repaint();
    }

    //==============================================================================
    void ModulePanel::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (palette::panel);
        g.fillRoundedRectangle (bounds, cornerRadius);
        g.setColour (palette::panelStroke);
        g.drawRoundedRectangle (bounds, cornerRadius, 1.0f);

        g.setColour (palette::text);
        g.setFont (celadonFont (titleFontHeight, true).withExtraKerningFactor (titleKerning));
        g.drawText (title, titleArea, juce::Justification::centredLeft, false);

        // Balance 曲线占位：旋钮行上方一条 track 色虚线。设了曲线组件（v0.3 的 27 段修正曲线）就由它自己画，不再画虚线。
        if (showsCurvePlaceholder && curve == nullptr && ! curveArea.isEmpty())
        {
            const float dashes[] = { 4.0f, 4.0f };
            const auto y = (float) curveArea.getCentreY() + 0.5f;

            g.setColour (palette::track);
            g.drawDashedLine (juce::Line<float> ((float) curveArea.getX(), y, (float) curveArea.getRight(), y),
                              dashes, 2, 1.0f);
        }
    }

    void ModulePanel::paintOverChildren (juce::Graphics& g)
    {
        if (! comingSoon)
            return;

        // 用底色按 (1 − comingSoonAlpha) 盖一层：整块只剩 35% 亮度；圆角与面板一致，不露方角。
        g.setColour (palette::background.withAlpha (1.0f - palette::comingSoonAlpha));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);

        // 「即将推出」做成描边胶囊徽章，放在曲线带（没有曲线带时放标题行下方），不压在旋钮上
        // ——2026-09-24 夜间截图验收发现：写在面板正中会盖住 Brighten 旋钮、看不清。
        const auto label = utf8 ("即将推出");
        const auto font = celadonFont (comingSoonFont, true);
        const auto badgeWidth = (float) juce::GlyphArrangement::getStringWidthInt (font, label) + 28.0f;
        const auto badgeHeight = 24.0f;
        const auto centreY = curveArea.isEmpty() ? (float) titleArea.getBottom() + 22.0f
                                                 : (float) curveArea.getCentreY();

        const auto badge = juce::Rectangle<float> (badgeWidth, badgeHeight)
                               .withCentre ({ (float) getLocalBounds().getCentreX(), centreY });

        g.setColour (palette::panel);
        g.fillRoundedRectangle (badge, badgeHeight * 0.5f);
        g.setColour (palette::accent);
        g.drawRoundedRectangle (badge.reduced (0.5f), badgeHeight * 0.5f, 1.0f);
        g.setFont (font);
        g.drawText (label, badge, juce::Justification::centred, false);
    }

    void ModulePanel::resized()
    {
        auto bounds = getLocalBounds().reduced (padding);

        // 标题行：标题靠左，附件（Glue GR 小表）靠右。
        titleArea = bounds.removeFromTop (titleHeight);

        if (accessory != nullptr)
        {
            auto accessoryArea = titleArea.removeFromRight (accessoryWidth);
            accessory->setBounds (accessoryArea.withSizeKeepingCentre (accessoryWidth, accessoryHeight));
            titleArea.removeFromRight (rowGap);
        }

        // 底部开关行：胶囊 72 × 22，从左到右间距 12。
        auto toggleRow = bounds.removeFromBottom (toggleHeight);

        for (auto* button : toggles)
        {
            button->setBounds (toggleRow.removeFromLeft (toggleWidth));
            toggleRow.removeFromLeft (toggleGap);
        }

        bounds.removeFromTop (rowGap);
        bounds.removeFromBottom (rowGap);

        // Balance 曲线带在旋钮行上方：放曲线组件（v0.3）或画占位虚线；两者都没有时不占高度。
        curveArea = hasCurveBand() ? bounds.removeFromTop (curveBandHeight) : juce::Rectangle<int>();

        if (curve != nullptr)
            curve->setBounds (curveArea);

        // 旋钮行：在剩余高度里垂直居中，左右只留 knobRowInset，等分 5 格。
        const juce::Rectangle<int> knobRow (knobRowInset,
                                            bounds.getY() + (bounds.getHeight() - Knob::smallHeight) / 2,
                                            getWidth() - 2 * knobRowInset,
                                            Knob::smallHeight);

        auto columnBounds = [knobRow] (int index)
        {
            const auto left  = knobRow.getX() + knobRow.getWidth() * index / maxKnobColumns;
            const auto right = knobRow.getX() + knobRow.getWidth() * (index + 1) / maxKnobColumns;
            return juce::Rectangle<int> (left, knobRow.getY(), right - left, knobRow.getHeight());
        };

        for (int i = 0; i < knobs.size(); ++i)
            knobs[i]->setBounds (columnBounds (i));

        // 三选一胶囊：右侧两格合并，整组（说明文字在上、胶囊竖排）垂直居中；组内怎么排由 CapsuleSelector 负责。
        if (hasSegmented)
        {
            auto area = columnBounds (maxKnobColumns - segmentedColumns)
                            .getUnion (columnBounds (maxKnobColumns - 1));

            segmented.setBounds (area.withSizeKeepingCentre (area.getWidth(),
                                                              juce::jmin (area.getHeight(), segmented.getContentHeight())));
        }
    }
}
