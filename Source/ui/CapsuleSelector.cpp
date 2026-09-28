/*
  ==============================================================================

    CapsuleSelector.cpp
    竖排单选胶囊：小标题 + N 个同一 radio 组的胶囊，可绑定 AudioParameterChoice。
    （v0.3 ～ v0.5 这段逻辑写在 ModulePanel 里，v0.6 拆出来给 GLAZE 的风格共用；布局算术与原来逐项相同，
    Balance 面板画出来逐像素不变。）

  ==============================================================================
*/

#include "CapsuleSelector.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"

namespace celadon::ui
{
    namespace
    {
        constexpr float captionFontHeight = 11.0f;
        constexpr int   radioGroup        = 0x43454c;   // 'CEL'：胶囊都是本组件的子组件，radio 组只在兄弟之间生效，任意非零值即可
    }

    //==============================================================================
    CapsuleSelector::CapsuleSelector()
    {
        caption.setFont (celadonFont (captionFontHeight));
        caption.setColour (juce::Label::textColourId, palette::textDim);
        caption.setJustificationType (juce::Justification::centred);
        caption.setBorderSize (juce::BorderSize<int> (0));
        caption.setInterceptsMouseClicks (true, false);   // 要接住鼠标才会出提示
        addAndMakeVisible (caption);
    }

    CapsuleSelector::~CapsuleSelector()
    {
        // 附件先走（它的回调会碰胶囊），再按声明的相反顺序销毁胶囊与小标题。
        attachment.reset();
    }

    //==============================================================================
    void CapsuleSelector::rebuild (const juce::String& captionText, const juce::StringArray& options,
                                   const juce::String& tooltip)
    {
        // 先拆旧附件再拆旧胶囊：附件的回调会碰胶囊。
        attachment.reset();
        parameter = nullptr;
        capsules.clear();

        caption.setText (captionText, juce::dontSendNotification);
        caption.setTooltip (tooltip);

        for (int i = 0; i < options.size(); ++i)
        {
            auto* capsule = capsules.add (new juce::ToggleButton (options[i]));
            capsule->setRadioGroupId (radioGroup, juce::dontSendNotification);
            capsule->setClickingTogglesState (true);
            capsule->setTooltip (tooltip);
            addAndMakeVisible (*capsule);
        }

        resized();
    }

    void CapsuleSelector::bind (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                                const juce::String& captionText, const juce::StringArray& options)
    {
        auto* choice = state.getParameter (parameterId);
        jassert (choice != nullptr);   // 参数表里没有这个 ID：检查 Parameters.cpp 与 ParameterIds.h

       #if JUCE_DEBUG
        // 胶囊数必须等于参数的选项数，否则胶囊下标与参数值对不上（下标按归一化值等距映射）。
        if (auto* asChoice = dynamic_cast<juce::AudioParameterChoice*> (choice))
            jassert (asChoice->choices.size() == options.size());
       #endif

        rebuild (captionText, options, help::tooltipFor (parameterId));
        parameter = choice;

        for (int i = 0; i < capsules.size(); ++i)
            capsules[i]->onClick = [this, i] { capsuleClicked (i); };

        // 附件：参数变了（宿主自动化、读工程、别的界面）就在消息线程回调点亮对应胶囊；
        // 建好后立刻同步一次，胶囊一出现就是参数的当前值。撤销管理器与 APVTS 的其他附件共用同一个。
        if (parameter != nullptr)
        {
            attachment = std::make_unique<juce::ParameterAttachment> (*parameter,
                                                                     [this] (float value) { showValue (value); },
                                                                     state.undoManager);
            attachment->sendInitialUpdate();
        }
    }

    void CapsuleSelector::showPlaceholder (const char* helpId, const juce::String& captionText,
                                           const juce::StringArray& options, int selectedIndex)
    {
        rebuild (captionText, options, help::tooltipFor (helpId));

        for (int i = 0; i < capsules.size(); ++i)
        {
            capsules[i]->setToggleState (i == selectedIndex, juce::dontSendNotification);
            capsules[i]->setEnabled (false);   // 占位：不可点；TooltipWindow 不看 isEnabled，悬停仍有提示
        }
    }

    //==============================================================================
    void CapsuleSelector::capsuleClicked (int index)
    {
        // radio 组里点亮一个胶囊时，被关掉的那些也各来一次 onClick；只有「被点亮」的那次才写参数。
        // 点已经亮着的胶囊：参数值没变，附件里 callIfParameterValueChanged 会直接跳过，不产生手势。
        if (attachment == nullptr || parameter == nullptr
             || ! juce::isPositiveAndBelow (index, capsules.size())
             || ! capsules[index]->getToggleState())
            return;

        // 下标 → 归一化值：与 JUCE 的 ComboBoxParameterAttachment 同一套等距映射（AudioParameterChoice 正好是 0 ～ N−1 的整数）。
        const auto count      = capsules.size();
        const auto normalised = count > 1 ? (float) index / (float) (count - 1) : 0.0f;

        attachment->setValueAsCompleteGesture (parameter->convertFrom0to1 (normalised));

        // 点击时胶囊已被 ToggleButton 自己点亮，参数回调（showValue）看到的是「没变」，所以这里单独通知一次。
        if (onSelectionChanged != nullptr)
            onSelectionChanged();
    }

    void CapsuleSelector::showValue (float denormalisedValue)
    {
        if (parameter == nullptr || capsules.isEmpty())
            return;

        const auto normalised = parameter->convertTo0to1 (denormalisedValue);
        const auto index      = juce::jlimit (0, capsules.size() - 1,
                                              juce::roundToInt (normalised * (float) (capsules.size() - 1)));

        // 不发通知：radio 组把其余胶囊关掉、都不触发 onClick，参数不会被再写一遍。已经亮着时什么都不做（也不重画）。
        const auto changed = ! capsules[index]->getToggleState();
        capsules[index]->setToggleState (true, juce::dontSendNotification);

        if (changed && onSelectionChanged != nullptr)
            onSelectionChanged();
    }

    void CapsuleSelector::setLayout (Layout newLayout)
    {
        layout = newLayout;
        caption.setVisible (layout == Layout::verticalWithCaption);
        resized();
    }

    //==============================================================================
    int CapsuleSelector::getContentHeight() const noexcept
    {
        const auto count = capsules.size();

        if (layout == Layout::horizontal)
            return capsuleHeight;

        return captionHeight + count * capsuleHeight + juce::jmax (0, count - 1) * capsuleGap;
    }

    int CapsuleSelector::getSelectedIndex() const noexcept
    {
        for (int i = 0; i < capsules.size(); ++i)
            if (capsules[i]->getToggleState())
                return i;

        return -1;
    }

    void CapsuleSelector::resized()
    {
        auto area = getLocalBounds();

        if (layout == Layout::horizontal)
        {
            // 横排：不显示小标题；胶囊等宽、间距 capsuleGap、垂直居中（高 capsuleHeight）。
            const auto count = capsules.size();

            if (count == 0)
                return;

            const auto capsuleWidth = (area.getWidth() - (count - 1) * capsuleGap) / count;

            for (auto* capsule : capsules)
            {
                capsule->setBounds (area.removeFromLeft (capsuleWidth).withSizeKeepingCentre (capsuleWidth, capsuleHeight));
                area.removeFromLeft (capsuleGap);
            }

            return;
        }

        // 竖排：小标题占满宽度在上；胶囊宽 min (72, 宽 − 4)，逐个竖排、水平居中。
        caption.setBounds (area.removeFromTop (captionHeight));

        const auto capsuleWidth = juce::jmin (maxCapsuleWidth, area.getWidth() - sideMargin);

        for (auto* capsule : capsules)
        {
            capsule->setBounds (area.removeFromTop (capsuleHeight).withSizeKeepingCentre (capsuleWidth, capsuleHeight));
            area.removeFromTop (capsuleGap);
        }
    }
}
