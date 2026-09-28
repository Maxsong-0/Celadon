/*
  ==============================================================================

    Porcelain.h
    GLAZE 主旋钮的瓷面（v0.1.8.0 界面改版，V4 设计稿「主旋钮：瓷器开片」）。

      - 开片网络：固定种子，把旋钮面（半径 86 的圆）反复用略带弯折、分形抖动的裂线切成多边形，三级由粗到细
        （几条贯穿的长裂 → 中等网格 → 细碎网格），与青瓷开片的层次一致。GLAZE 越大，显示的裂纹越多
        （前 total × t^0.9 条），釉色越深（t^0.7 混色）。与设计稿的生成器逐步对应，同一种子画出同一张网。
      - 釉面：径向渐变的釉色 + 斑驳（两层噪声的正片叠底）+ 浮雕光照（裂纹作高度图：模糊后求法线，
        左上方来光的漫反射正片叠底 + 高光滤色，裂纹两侧一边亮一边暗，像釉面上微微凸起的小块）+ 深色裂线 +
        边缘暗角 + 左上的柔光 + 两道细边。
      - 深色旋钮帽（半径 36）也在这张图里；数值弧、指针、把手、数字是动态的，由 GlazeKnob 每次重画。

    渲染只在 GLAZE 或像素比变了时发生（几毫秒到十几毫秒），结果是一张按物理像素生成的图，平时每帧只是贴图。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <vector>

namespace celadon::ui::porcelain
{
    // 旋钮面的逻辑尺寸（与设计稿一致：224 × 224 的旋钮区，圆心在正中）。
    inline constexpr float glazeRadius = 86.0f;
    inline constexpr float capRadius   = 36.0f;
    inline constexpr float imageHalf   = 90.0f;    // 瓷面图覆盖 [−90, 90]²（留出外圈细边与抗锯齿）

    struct Crack
    {
        std::vector<juce::Point<float>> points;   // 圆心为原点的逻辑坐标
        int level = 0;                            // 0 长裂 / 1 中网格 / 2 细网格
        float width = 0.0f;                       // 裂线宽（逻辑像素）
        float opacity = 0.0f;
    };

    // 全部裂纹（按生成顺序：先长裂、后细网格）。第一次调用时生成，之后只读。
    const std::vector<Crack>& crackNetwork();

    // GLAZE 比例（0 ～ 1）下显示前几条裂纹。
    int visibleCrackCount (float glazeProportion) noexcept;

    // 渲染瓷面：返回 ARGB 图，覆盖逻辑区域 [−imageHalf, imageHalf]²，每逻辑像素 pixelScale 个像素。
    // desaturated = 听原声（A / B 的 B）时的去色版本（饱和度 30 %）。
    juce::Image renderGlaze (float glazeProportion, float pixelScale, bool desaturated);

    // 把一张图去色到 saturation（1 = 原样，0 = 灰）：CSS saturate() 同一组系数。
    void desaturate (juce::Image& image, float saturation);
}
