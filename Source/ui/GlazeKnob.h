/*
  ==============================================================================

    GlazeKnob.h
    GLAZE 主旋钮（V4 设计稿中层正中，224 × 224）：瓷面（开片随 GLAZE 变密、釉色变深）+ 外圈 270° 分段青色数值弧 +
    白色把手 + 深色旋钮帽里的百分数。不画刻度尺。

      - 瓷面由 porcelain::GlazeRenderer 分层生成、按物理像素缓存；GLAZE 变了在定时器里重新生成（拖动时最多约 16 次 / s），
        新旧两张交叉淡化约 0.26 s（「减少动效」时直接换）——新长出来的裂纹是淡入的；
      - 听原声（A / B 的 B）时整颗旋钮变淡、去色（饱和度 30 %），表示此刻听到的不是 GLAZE 的结果；
      - 手感与所有旋钮一致：上下拖（250 px 满量程）、⌘ / Ctrl 精调、双击回 35 %、Alt 单击在帽子上直接输入、滚轮。

    整颗旋钮（影子、轨道、数值弧、瓷面、指针、把手、数字）合成一张按物理像素的缓存图（T-032 R9-01 / 07 / 14）：
    旋钮在粒子带里，粒子每帧重画都会把它拖进来（macOS 只拿脏矩形的包围盒），GLAZE 不动时每次 paint 只是整数对齐贴一次图。
    缓存的键 = 值 × 听原声 × 像素比 × 瓷面版本 × 交叉淡化进度，任何一项变了下次 paint 重新合成（paint 里逐项比，
    不靠各处记得作废）。听原声时缓存图按 55 % 贴，与原来整颗旋钮画进 55 % 的透明图层等价，只是不再每次 paint 都开层。
    JUCE 的 setBufferedToImage 贴图带小数平移、每次双线性重采样，这里不用它。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "Porcelain.h"

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

        // 截图工具的检查与基准读它：paint 次数、整颗旋钮缓存图重新合成的次数、瓷面生成次数与分层重建次数、上次生成瓷面的毫秒数。
        struct PaintStats
        {
            int paints = 0, faceRenders = 0, porcelainRenders = 0, porcelainLayerBuilds = 0;
            double lastPorcelainMs = 0.0;
        };

        PaintStats getPaintStats() const noexcept;

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
            int serial = 0;                // 第几次生成（缓存键用；0 = 没有）
        };

        // 整颗旋钮缓存图的键：画出来的样子只由这几项决定。
        struct FaceKey
        {
            float value = 0.0f;
            bool comparing = false;
            int currentSerial = 0, previousSerial = 0;
            float fade = 1.0f;

            bool operator== (const FaceKey& other) const noexcept;
        };

        void timerCallback() override;
        bool needsRender() const noexcept;
        void render (bool crossfade);
        bool isCrossfading() const noexcept                 { return fade < 1.0f && previous.image.isValid(); }
        FaceKey currentFaceKey() const noexcept;
        void paintFace (juce::Graphics&, float pixelScale) const;
        void drawGlaze (juce::Graphics&, const Rendered&, float opacity, float pixelScale) const;

        porcelain::GlazeRenderer renderer;
        Rendered current, previous;
        int renderSerial = 0;
        float fade = 1.0f;                 // 0 → 1：previous 淡出、current 淡入
        double fadeStartMs = 0.0;
        double lastRenderMs = 0.0, lastRenderCostMs = 0.0;
        float wantedPixelScale = 0.0f;
        double pixelScaleChangedMs = 0.0;
        bool comparing = false;
        bool reduceMotion = false;
        juce::Point<float> lastDragPosition;

        AlignedImage face;                 // 整颗旋钮（覆盖 224 × 224，按此刻物理像素比）
        FaceKey faceKey;
        PaintStats stats;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeKnob)
    };
}
