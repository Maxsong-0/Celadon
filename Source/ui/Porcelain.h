/*
  ==============================================================================

    Porcelain.h
    GLAZE 主旋钮的青瓷釉面。

      - 开片网络：固定种子的分形裂线，三级由长到细，GLAZE 越大可见开片越多。
      - 材质：浅灰青到豆青的连续釉色、极轻云斑、低对比釉下细开片、边缘积釉与宽软反光。
        裂纹不参与浮雕法线，避免冰晶切面的亮暗碎块；整面始终像一整块温润的瓷。
      - 深青旋钮帽（半径 36）也在图里；数值弧、指针、把手与数字由 GlazeKnob 动态绘制。

    只在 GLAZE、原声比较或像素比改变时重新渲染，按物理像素生成并缓存，平时每帧只贴图。

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
