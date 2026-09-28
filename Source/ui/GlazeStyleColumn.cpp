/*
  ==============================================================================

    GlazeStyleColumn.cpp

  ==============================================================================
*/

#include "GlazeStyleColumn.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"
#include "../ParameterIds.h"

namespace celadon::ui
{
    namespace
    {
        juce::String relearnText (bool inProgress)
        {
            return inProgress ? utf8 ("聆听中…") : utf8 ("重新聆听");
        }
    }

    //==============================================================================
    GlazeStyleColumn::GlazeStyleColumn (juce::AudioProcessorValueTreeState& state)
    {
        // 胶囊顺序 = 参数 glaze_style 的选项顺序（Auto / Warm / Punchy / Clean = glaze::Style 的下标）；胶囊上写中文。
        styleSelector.bind (state, param::glazeStyle, utf8 ("风格"),
                            { utf8 ("自动"), utf8 ("温润"), utf8 ("冲击"), utf8 ("通透") });
        styleSelector.setComponentID ("glazeStyle");
        addAndMakeVisible (styleSelector);

        // 「重新聆听」：普通按钮（不是开关、不绑参数），画成梅子青描边胶囊。
        relearnButton.setButtonText (relearnText (false));
        relearnButton.getProperties().set (accentOutlineProperty, true);
        relearnButton.setTooltip (help::tooltipFor (uiid::glazeRelearn));
        relearnButton.setComponentID ("glazeRelearn");
        relearnButton.onClick = [this]
        {
            if (onRelearnClicked != nullptr)
                onRelearnClicked();
        };
        addAndMakeVisible (relearnButton);
    }

    GlazeStyleColumn::~GlazeStyleColumn()
    {
        relearnButton.onClick = nullptr;
    }

    //==============================================================================
    void GlazeStyleColumn::setRelearnInProgress (bool inProgress)
    {
        if (relearnInProgress == inProgress)
            return;

        relearnInProgress = inProgress;
        relearnButton.setButtonText (relearnText (inProgress));   // TextButton 换字自己会重画（只重画按钮）
    }

    void GlazeStyleColumn::resized()
    {
        auto area = getLocalBounds();
        const auto relearnArea = area.removeFromBottom (relearnHeight);

        styleSelector.setBounds (area.removeFromTop (styleSelector.getContentHeight()));

        // 按钮与风格胶囊同宽（72）、同样水平居中：一列的左右边缘对齐。
        relearnButton.setBounds (relearnArea.withSizeKeepingCentre (juce::jmin (CapsuleSelector::maxCapsuleWidth,
                                                                                relearnArea.getWidth() - CapsuleSelector::sideMargin),
                                                                    relearnHeight));
    }
}
