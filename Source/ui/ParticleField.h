/*
  ==============================================================================

    ParticleField.h
    中层（频谱仪与模块卡片之间、主旋钮两侧）的粒子流（V4 设计稿）：

      - 左边 14 条流线从画面边缘汇向旋钮左侧的边缘，右边 14 条从旋钮右侧流向画面边缘——声音从左边流进 GLAZE、从右边流出；
      - 每边最多 100 个粒子沿流线移动（约 15 % 是模糊的大颗粒），数量 = 100 × GLAZE^0.8：GLAZE 越大越密；
      - 速度与亮度跟着输出电平（最近一帧的 RMS）走，停止播放时平滑减速到停；听原声（B）时变慢、变暗；
      - 旋钮外一圈圈扩散的波纹与柔光，强度也跟 GLAZE；
      - 「减少动效」：粒子与波纹都关掉，只留静止的流线与柔光。

    流线与粒子的参数（位置、时长、颜色、缓动）与设计稿用同一个线性同余种子生成，画出来一样。
    动画由显示器的垂直同步驱动（VBlankAttachment），约 30 帧 / s（与界面刷新率一致），没有东西在动时不重画。每帧取离 1/30 s
    最近的整数个 VBlank（即半个 VBlank 的容差，isFrameDue）：60 / 90 / 120 / 240 Hz 屏正好 30 帧（隔 2 / 3 / 4 / 8 个 VBlank），
    144 Hz 28.8 帧，72 / 100 Hz 36 / 33 帧；75 / 165 Hz 上 1/30 s 不是整数个 VBlank，在 2 / 3（5 / 6）个之间交替，平均约 29.3 / 29.7 帧。
    不用严格的「距上一帧 ≥ 1/30 s」：60 Hz 屏两个 VBlank 恰好 1/30 s，显示器周期略短于 1/60 s 或时间戳抖一点，
    该画的那一帧就拖到第 3 个 VBlank（20 帧 / s、忽快忽慢）。

    流线与柔光是两张按物理像素缓存的图（T-032 R9-05 / M-3）：按上下文的实际像素比生成（不封顶）、按整数设备像素贴
    （最近邻），软件渲染器与 CoreGraphics 都走整块拷贝；像素比变了（缩放窗口）才重新生成。每帧真正画的只有波纹与粒子。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"

#include <array>
#include <vector>

namespace celadon::ui
{
    class ParticleField  : public juce::Component
    {
    public:
        ParticleField();
        ~ParticleField() override;

        // 画布上的位置：这一条带（主旋钮所在的中层）。粒子的坐标按 1200 × 820 画布算。
        static constexpr int top = 300;
        static constexpr int bandHeight = 260;

        // 目标帧率（规格「界面刷新 30 Hz」）：按离 1 / maxFrameRate 最近的整数个 VBlank 画（isFrameDue）。
        static constexpr double maxFrameRate = 30.0;

        // 节流规则（纯函数，截图工具的检查直接测）：距上一帧 sinceLastFrame 秒、这次 VBlank 距上次 vblankInterval 秒时该不该画。
        // 容差半个 VBlank（最多半帧）：60 Hz 屏每 2 个 VBlank、90 Hz 每 3 个、120 Hz 每 4 个、144 Hz 每 5 个画一次。
        static bool isFrameDue (double sinceLastFrame, double vblankInterval) noexcept;

        void setGlaze (float proportion);
        void setAudio (float rmsDb, bool playing);
        void setComparing (bool listeningToOriginal)       { comparing = listeningToOriginal; }
        void setReduceMotion (bool shouldReduceMotion);

        // 离屏截图用：不靠垂直同步，直接把动画时钟往前推这么多秒。
        void advance (double seconds);

        void paint (juce::Graphics&) override;

        // 缓存图重新生成的次数（截图工具的检查与基准读它：像素比不变时应当一直不变）。
        struct CacheStats { int lineRenders = 0, glowRenders = 0; };
        CacheStats getCacheStats() const noexcept           { return cacheStats; }

    private:
        struct Streamline
        {
            std::vector<juce::Point<float>> points;     // 沿贝塞尔曲线等参采样的折线
            std::vector<float> lengths;                 // 累计弧长
        };

        struct Particle
        {
            int line = 0;
            bool right = false;
            bool soft = false;
            float radius = 1.0f;
            float opacity = 1.0f;
            float duration = 4.0f;
            float offset = 0.0f;                        // SMIL 的 begin（负数：动画早就开始了）
            juce::Colour colour;
        };

        void build();
        void onVBlank (double timestampSeconds);
        void step (double seconds);
        void renderLines (float pixelScale);
        void renderGlow (float pixelScale);
        juce::Point<float> positionOn (const Streamline& line, float fraction) const noexcept;

        std::array<std::vector<Streamline>, 2> lines;   // [0] 左（汇入），[1] 右（流出）
        std::array<std::vector<Particle>, 2> particles;
        juce::Path leftCurves, rightCurves;

        AlignedImage lineImage;                         // 28 条流线（全不透明度，只覆盖流线的外接矩形）
        AlignedImage glowImage;                         // 旋钮后面的柔光（全不透明度，柔光圆与这条带的交集）
        CacheStats cacheStats;

        float glaze = 0.35f;
        float energy = 0.0f;                            // 0 ～ 1：电平折算的「音乐有多满」
        bool playing = false, comparing = false, reduceMotion = false;

        double clock = 0.0;                             // 动画时间（秒）：速度随音乐变
        float speed = 0.0f, brightness = 0.3f;
        double lastVBlank = -1.0, lastRepaint = -1.0;
        bool animating = false;

        juce::VBlankAttachment vblank;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParticleField)
    };
}
