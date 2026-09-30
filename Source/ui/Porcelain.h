/*
  ==============================================================================

    Porcelain.h
    GLAZE 主旋钮的青瓷釉面。

      - 开片网络：固定种子的分形裂线，三级由长到细，GLAZE 越大可见开片越多。
      - 材质：浅灰青到豆青的连续釉色、极轻云斑、低对比釉下细开片、边缘积釉与宽软反光。
        裂纹不参与浮雕法线，避免冰晶切面的亮暗碎块；整面始终像一整块温润的瓷。
      - 深青旋钮帽（半径 36）也在图里；数值弧、指针、把手与数字由 GlazeKnob 动态绘制。

    只在 GLAZE、原声比较或像素比改变时重新渲染，按物理像素生成并缓存，平时每帧只贴图。

    分层渲染（T-032 R9-02，GlazeRenderer）：底层每个像素里贵的量（三次梯度噪声、两次开方、覆盖率、在哪段釉色渐变里）都与
    GLAZE 无关，按像素比算一次存着，每次只按当次的三档釉色逐像素着色（与原算法同一个着色函数，底层逐位相同；比「缓存 depth 0 / 1
    两张基底图再混合」多占一半内存，但不引入混合的两次 8 位量化）；裂纹的描边轮廓按同一精度缓存成 Path、每次 fillPath
    （软件渲染器的 strokePath 本来就是这样做的，裂纹逐位相同）；与 GLAZE 无关的积釉、釉光、反光、两圈外环、帽子缓存成
    一张覆盖层。叠放顺序与原算法（renderGlaze，保留作逐像素参照）相同：底层 → 裂纹 → 覆盖层 → 去色。与参照的差别只来自
    覆盖层先在透明图上合成、再整张叠上的 8 位取整，≤ 2 LSB（截图工具 --only check 逐像素核对）。

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

    // 渲染瓷面（参照实现：逐像素全量算）：返回 ARGB 图，覆盖逻辑区域 [−imageHalf, imageHalf]²，
    // 每逻辑像素约 pixelScale 个像素（边长 round (2 × imageHalf × pixelScale)）。
    // desaturated = 听原声（A / B 的 B）时的去色版本（饱和度 30 %）。
    // 界面用下面的 GlazeRenderer；这个函数是它逐像素对照的基准（截图工具的检查与基准）。
    juce::Image renderGlaze (float glazeProportion, float pixelScale, bool desaturated);

    // 分层渲染（见文件头）：结果与 renderGlaze 同尺寸、同叠放顺序，底层与裂纹逐位相同，覆盖层的取整让个别像素差 ≤ 2 LSB。
    // 各层按图的边长缓存，边长变了（像素比变了）才重建；只在消息线程用（与 GlazeKnob 同一线程）。
    class GlazeRenderer
    {
    public:
        juce::Image render (float glazeProportion, float pixelScale, bool desaturated);

        // 各层重建的次数（截图工具读它：像素比不变时应当一直不变）。
        int getLayerBuilds() const noexcept   { return layerBuilds; }

    private:
        // 底层一个像素里与 GLAZE 无关的量：段别（内段 g0 → g1 / 外段 g1 → g2）放在覆盖率的符号里，每像素 12 字节。
        struct StoredSample
        {
            float signedCoverage = 0.0f;            // 0 = 圆外（透明）；< 0 = 外段
            float mix = 0.0f;                       // 段内位置
            float variation = 0.0f;                 // 云斑与微粒（亮度起伏约 2 %）
        };

        void buildLayers (int size);
        void buildCrackOutlines (float accuracy);

        int layerSize = 0;
        std::vector<StoredSample> baseSamples;      // size × size，逐行
        juce::Image overlay;                        // 积釉、釉光、反光、外环、帽子（与 GLAZE 无关）
        std::vector<juce::Path> crackOutlines;      // 每条裂纹的描边轮廓（与参照 strokePath 同一精度）
        float crackOutlineAccuracy = 0.0f;
        int layerBuilds = 0;
    };

    // 把一张图去色到 saturation（1 = 原样，0 = 灰）：CSS saturate() 同一组系数。
    void desaturate (juce::Image& image, float saturation);
}
