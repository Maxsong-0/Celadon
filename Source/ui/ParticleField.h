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
    动画由显示器的垂直同步驱动（VBlankAttachment），最多 30 帧 / s（与界面刷新率一致：60 Hz 屏隔帧、120 Hz 屏每四帧重画一次），
    没有东西在动时不重画。

    流线与柔光是两张按物理像素缓存的图（T-032 R9-05 / M-3）：按上下文的实际像素比生成、按整数设备像素贴（最近邻），
    软件渲染器与 CoreGraphics 都走整块拷贝；每帧真正画的只有波纹与粒子。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <array>
#include <vector>

namespace celadon::ui
{
    class ParticleField  : public juce::Component
    {
    public:
        ParticleField();
        ~ParticleField() override;

        // 画布上的位置：这一条带（主旋钮所在的中层）。粒子的坐标按 1100 × 820 画布算。
        static constexpr int top = 300;
        static constexpr int bandHeight = 260;

        void setGlaze (float proportion);
        void setAudio (float rmsDb, bool playing);
        void setComparing (bool listeningToOriginal)       { comparing = listeningToOriginal; }
        void setReduceMotion (bool shouldReduceMotion);

        // 离屏截图用：不靠垂直同步，直接把动画时钟往前推这么多秒。
        void advance (double seconds);

        void paint (juce::Graphics&) override;

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
        juce::Point<float> positionOn (const Streamline& line, float fraction) const noexcept;

        std::array<std::vector<Streamline>, 2> lines;   // [0] 左（汇入），[1] 右（流出）
        std::array<std::vector<Particle>, 2> particles;
        juce::Path leftCurves, rightCurves;

        juce::Image lineImage;                          // 28 条流线的缓存（按物理像素）
        float lineImageScale = 0.0f;

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
