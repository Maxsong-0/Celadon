/*
  ==============================================================================

    LoudnessReadout.h
    总览区右侧：整体响度大数字（40 px），下面短期响度 / 真峰值最大值 / GR 三行小字，旁边 Reset。
    各行悬停显示对应提示（HelpText：ui_integrated、ui_short_term、ui_meter_out、ui_meter_gr、ui_reset）。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <functional>

namespace celadon::ui
{
    class LoudnessReadout  : public juce::Component
    {
    public:
        LoudnessReadout();
        ~LoudnessReadout() override;

        // 界面定时器每 30 ms 调一次。LUFS 为 LoudnessMeter::noReading 时显示「-inf」。
        // 只有显示出来的文字（一位小数）变了才重画，而且只重画那一行（v0.5 R5）。
        void setValues (float integratedLufs, float shortTermLufs, float truePeakMaxDb, float reductionDb);

        // 点 Reset 时回调（编辑器在里面清整体响度与全部峰值保持）。
        std::function<void()> onReset;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        // ---- 以下由实现者（v0.2 计划 A5）按需要补充；公开接口不得改动 ----
        juce::TextButton resetButton;

        // 画面上正显示的四段文字：paint 只读它们，setValues 只在文字变了时换新并重画对应那一行。
        juce::String integratedText, shortTermText, truePeakText, reductionText;

        // 透明的悬停区：只为让每一行有自己的提示气泡，不画东西、不吃点击以外的事件。
        // 具体类型在 LoudnessReadout.cpp 里定义。
        std::unique_ptr<juce::Component> integratedRegion, shortTermRegion, truePeakRegion, reductionRegion;

        // resized 算好、paint 直接用的区域。
        juce::Rectangle<int> bigNumberArea, captionArea, shortTermRow, truePeakRow, reductionRow;

        void paintRow (juce::Graphics&, juce::Rectangle<int> row, const juce::String& name,
                       const juce::String& value, const juce::String& unit) const;

        // 文字变了才换新并重画 area（组件坐标）。
        void updateText (juce::String& shown, const juce::String& newText, juce::Rectangle<int> area);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessReadout)
    };
}
