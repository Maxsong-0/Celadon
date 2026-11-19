/*
  ==============================================================================

    SpectrumDeck.h
    V5「一釉」底部那张卡片（T-123，设计规格-界面-V5一釉 2.3）：左边输入表，中间频谱图，右边输出表与衰减表，都画在一张白底、
    圆角 16、1 px 描边的卡片上。宽 1168；高两档——模块都收起时 288（图高 266），有模块展开时 114（图高 92，频谱图进矮档）。

    卡片只做两件事：按自己的高度摆这三样，画自己的底与边。读数照旧由编辑器的计时器推给里面的三个组件（它们是公开成员）；
    阴影不归它（编辑器的画布画）；「放大频谱」键在频谱图里（SpectrumView::onEnlarge）。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "LevelMeters.h"
#include "SpectrumView.h"

namespace celadon::ui
{
    class SpectrumDeck  : public juce::Component
    {
    public:
        SpectrumDeck();

        static constexpr int width = 1168;
        static constexpr int tallHeight = 288;       // 模块都收起
        static constexpr int compactHeight = 114;    // 有模块展开
        static constexpr int verticalPadding = 22;   // 卡片高 − 图高：上 10、下 12

        // 图在卡片里的位置（卡片坐标）。电平表与图同上缘、同高。
        static constexpr int graphX = 68, graphY = 10;

        void setLanguage (Language newLanguage);

        void paint (juce::Graphics&) override;
        void resized() override;

        SpectrumView spectrum { SpectrumView::Style::deck };
        LevelMeterPanel inputMeter { LevelMeterPanel::Kind::input, LevelMeterPanel::Style::deck },
                        outputMeter { LevelMeterPanel::Kind::outputAndReduction, LevelMeterPanel::Style::deck };

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDeck)
    };
}
