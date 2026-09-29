/*
  ==============================================================================

    GlazeKnob.h
    GLAZE 主旋钮（V4 设计稿中层正中，224 × 224）：瓷面（开片随 GLAZE 变密、釉色变深）+ 外圈 270° 分段青色数值弧 +
    白色把手 + 深色旋钮帽里的百分数。不画刻度尺。

      - 瓷面由 porcelain::renderGlaze 生成、按物理像素缓存；GLAZE 变了在定时器里重新生成（拖动时最多约 14 次 / s），
        新旧两张交叉淡化约 0.26 s（「减少动效」时直接换）——新长出来的裂纹是淡入的；
      - 听原声（A / B 的 B）时整颗旋钮变淡、去色（饱和度 30 %），表示此刻听到的不是 GLAZE 的结果；
      - 手感与所有旋钮一致：上下拖（250 px 满量程）、⌘ / Ctrl 精调、双击回 35 %、Alt 单击在帽子上直接输入、滚轮。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"

namespace celadon::ui
{
    class GlazeKnob  : public ParameterControl,
                       private juce::Timer
    {
    public:
        GlazeKnob (juce::RangedAudioParameter& glazeParameter, InteractionHost& host);
        ~GlazeKnob() override;

        static constexpr int size = 224;

        void setComparing (bool listeningToOriginal);
        void setReduceMotion (bool shouldReduceMotion)      { reduceMotion = shouldReduceMotion; }

        void paint (juce::Graphics&) override;
        bool hitTest (int x, int y) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    protected:
        juce::Point<float> getBubbleAnchor() const override;
        juce::Rectangle<int> getTextEntryArea() const override;
        void parameterValueChanged() override;

    private:
        struct Rendered
        {
            juce::Image image;
            float proportion = -1.0f;
            float pixelScale = 0.0f;
            bool desaturated = false;
            int cracks = -1;
        };

        void timerCallback() override;
        bool needsRender() const noexcept;
        void render (bool crossfade);
        void drawGlaze (juce::Graphics&, const Rendered&, float opacity) const;

        Rendered current, previous;
        float fade = 1.0f;                 // 0 → 1：previous 淡出、current 淡入
        double fadeStartMs = 0.0;
        double lastRenderMs = 0.0, lastRenderCostMs = 0.0;
        float wantedPixelScale = 0.0f;
        double pixelScaleChangedMs = 0.0;
        bool comparing = false;
        bool reduceMotion = false;
        juce::Point<float> lastDragPosition;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeKnob)
    };
}
