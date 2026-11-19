/*
  ==============================================================================

    SpectrumDeck.cpp

  ==============================================================================
*/

#include "SpectrumDeck.h"

#include "CeladonLookAndFeel.h"

namespace celadon::ui
{
    SpectrumDeck::SpectrumDeck()
    {
        // 组件 ID 与 V4 编辑器里那三样相同：截图工具按 ID 找电平表（V5 换版面之后编辑器里只剩这一套）。
        inputMeter.setComponentID ("input_meter");
        outputMeter.setComponentID ("output_meter");

        addAndMakeVisible (inputMeter);
        addAndMakeVisible (spectrum);
        addAndMakeVisible (outputMeter);

        setSize (width, tallHeight);
    }

    void SpectrumDeck::setLanguage (Language newLanguage)
    {
        spectrum.setLanguage (newLanguage);
        inputMeter.setLanguage (newLanguage);
        outputMeter.setLanguage (newLanguage);
    }

    void SpectrumDeck::resized()
    {
        // 规格 V5 2.3（画布坐标，卡片在 x 16）：输入表 x 30 ～ 74，图 x 84 起 1032 宽，输出 + 衰减 x 1116 ～ 1172。
        const auto innerHeight = juce::jmax (1, getHeight() - verticalPadding);

        inputMeter.setBounds (14, graphY, LevelMeterPanel::deckInputWidth, innerHeight);
        spectrum.setBounds (graphX, graphY, SpectrumView::width, innerHeight);
        outputMeter.setBounds (getWidth() - 12 - LevelMeterPanel::deckOutputWidth, graphY, LevelMeterPanel::deckOutputWidth, innerHeight);
    }

    void SpectrumDeck::paint (juce::Graphics& g)
    {
        // 白底圆角 16；描边内缩半像素，整条 1 px 线都在卡片里（同 V4 的面板）。
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (palette::panel);
        g.fillRoundedRectangle (bounds, 16.0f);
        g.setColour (palette::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 15.5f, 1.0f);
    }
}
