/*
  ==============================================================================

    LoudnessReadout.cpp
    整体响度大数字 +「Integrated LUFS」，下面短期响度 / 真峰值最大值 / GR 三行，旁边 Reset。
    每行盖一块透明悬停区，各自出对应的提示气泡。

  ==============================================================================
*/

#include "LoudnessReadout.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"
#include "../ParameterIds.h"
#include "../dsp/LoudnessMeter.h"

namespace celadon::ui
{
    namespace
    {
        constexpr float bigFontHeight     = 40.0f;
        constexpr int   bigNumberHeight   = 46;
        constexpr float captionFontHeight = 12.0f;
        constexpr int   captionHeight     = 16;
        constexpr int   rowHeight         = 18;
        constexpr int   rowGap            = 2;
        constexpr int   unitWidth         = 36;   // 每行最右的单位列（LUFS / dBTP / dB）
        constexpr int   resetWidth        = 72;
        constexpr int   resetHeight       = 22;
        constexpr int   resetGap          = 12;

        // 只为悬停提示存在的透明组件：不画、不抢子组件事件；点击穿不过去也无妨（这几行没有点击功能）。
        class HoverRegion final : public juce::Component,
                                  public juce::SettableTooltipClient
        {
        public:
            explicit HoverRegion (const juce::String& tooltip)
            {
                setTooltip (tooltip);
                setInterceptsMouseClicks (true, false);
                setPaintingIsUnclipped (true);
            }

            void paint (juce::Graphics&) override {}
        };

        juce::String lufsText (float lufs)
        {
            return lufs <= LoudnessMeter::noReading ? juce::String ("-inf") : juce::String (lufs, 1);
        }
    }

    //==============================================================================
    LoudnessReadout::LoudnessReadout()
        : resetButton ("Reset"),
          integratedText (lufsText (LoudnessMeter::noReading)),
          shortTermText (lufsText (LoudnessMeter::noReading)),
          truePeakText (lufsText (LoudnessMeter::noReading)),
          reductionText (juce::String (0.0f, 1))
    {
        integratedRegion = std::make_unique<HoverRegion> (help::tooltipFor (uiid::integrated));
        shortTermRegion  = std::make_unique<HoverRegion> (help::tooltipFor (uiid::shortTerm));
        truePeakRegion   = std::make_unique<HoverRegion> (help::tooltipFor (uiid::meterOut));
        reductionRegion  = std::make_unique<HoverRegion> (help::tooltipFor (uiid::meterGr));

        for (auto* region : { integratedRegion.get(), shortTermRegion.get(), truePeakRegion.get(), reductionRegion.get() })
            addAndMakeVisible (*region);

        resetButton.setTooltip (help::tooltipFor (uiid::resetMeters));
        resetButton.onClick = [this]
        {
            if (onReset != nullptr)
                onReset();
        };
        addAndMakeVisible (resetButton);
    }

    LoudnessReadout::~LoudnessReadout()
    {
        resetButton.onClick = nullptr;
    }

    //==============================================================================
    void LoudnessReadout::setValues (float integratedLufs, float shortTermLufs, float truePeakMaxDb, float reductionDb)
    {
        // 30 ms 一次。v0.4 只要任一值变了 0.005 以上就整块重画——文字可能根本没变，却把右上角整块拉进 macOS 合成的
        // 外接矩形；现在按「显示出来的文字」比，变了才重画，而且只重画那一行（研究 P5 R5）。画出来的像素与 v0.4 相同。
        updateText (integratedText, lufsText (integratedLufs), bigNumberArea);
        updateText (shortTermText,  lufsText (shortTermLufs),  shortTermRow);
        updateText (truePeakText,   lufsText (truePeakMaxDb),  truePeakRow);
        updateText (reductionText,  juce::String (reductionDb, 1), reductionRow);
    }

    void LoudnessReadout::updateText (juce::String& shown, const juce::String& newText, juce::Rectangle<int> area)
    {
        if (shown == newText)
            return;

        shown = newText;
        repaint (area);
    }

    //==============================================================================
    void LoudnessReadout::paintRow (juce::Graphics& g, juce::Rectangle<int> row, const juce::String& name,
                                    const juce::String& value, const juce::String& unit) const
    {
        g.setFont (celadonFont (captionFontHeight));

        // 单位靠左贴着数字（灰青），数字右对齐（白瓷），名字靠左（灰青）。
        auto unitArea = row.removeFromRight (unitWidth);
        g.setColour (palette::textDim);
        g.drawText (unit, unitArea.withTrimmedLeft (4), juce::Justification::centredLeft, false);

        g.setColour (palette::text);
        g.setFont (celadonFont (captionFontHeight, true));
        g.drawText (value, row, juce::Justification::centredRight, false);

        g.setColour (palette::textDim);
        g.setFont (celadonFont (captionFontHeight));
        g.drawText (name, row, juce::Justification::centredLeft, false);
    }

    void LoudnessReadout::paint (juce::Graphics& g)
    {
        // 整体响度：40 px 中粗大数字，下面「Integrated LUFS」。
        g.setColour (palette::text);
        g.setFont (celadonFont (bigFontHeight, true));
        g.drawText (integratedText, bigNumberArea, juce::Justification::centredLeft, false);

        g.setColour (palette::textDim);
        g.setFont (celadonFont (captionFontHeight));
        g.drawText ("Integrated LUFS", captionArea, juce::Justification::centredLeft, false);

        paintRow (g, shortTermRow, "Short-term", shortTermText, "LUFS");
        paintRow (g, truePeakRow,  "True peak",  truePeakText,  "dBTP");
        paintRow (g, reductionRow, "GR",         reductionText, "dB");
    }

    void LoudnessReadout::resized()
    {
        auto bounds = getLocalBounds();

        bigNumberArea = bounds.removeFromTop (bigNumberHeight);
        captionArea   = bounds.removeFromTop (captionHeight);
        integratedRegion->setBounds (bigNumberArea.getUnion (captionArea));

        bounds.removeFromTop (rowGap * 2);

        // 三行在左，Reset 在右下（与最后一行底对齐）。
        auto rowsArea = bounds;
        auto resetColumn = rowsArea.removeFromRight (resetWidth);
        rowsArea.removeFromRight (resetGap);

        shortTermRow = rowsArea.removeFromTop (rowHeight);
        rowsArea.removeFromTop (rowGap);
        truePeakRow  = rowsArea.removeFromTop (rowHeight);
        rowsArea.removeFromTop (rowGap);
        reductionRow = rowsArea.removeFromTop (rowHeight);

        shortTermRegion->setBounds (shortTermRow);
        truePeakRegion->setBounds (truePeakRow);
        reductionRegion->setBounds (reductionRow);

        resetButton.setBounds (resetColumn.withY (reductionRow.getBottom() - resetHeight)
                                          .withHeight (resetHeight));
    }
}
