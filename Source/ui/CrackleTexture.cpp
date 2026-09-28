/*
  ==============================================================================

    CrackleTexture.cpp
    程序生成的开片纹：固定种子的 LCG 生成折线，渲染成透明底图片交给调用方缓存。

  ==============================================================================
*/

#include "CrackleTexture.h"

#include "CeladonLookAndFeel.h"

namespace celadon::ui
{
    namespace
    {
        constexpr int   minSegments      = 2;       // 每条裂纹 2 ～ 4 段
        constexpr int   maxSegments      = 4;
        constexpr float minSegmentLength = 20.0f;   // 段长 20 ～ 70 逻辑像素
        constexpr float maxSegmentLength = 70.0f;
        constexpr float minTurnRadians   = 0.35f;   // 相邻两段的转角约 20° ～ 75°：太小像直线，太大像锯齿
        constexpr float maxTurnRadians   = 1.30f;
        constexpr float minUsefulLength  = 3.0f;    // 被区域边界截得比这还短的段不要
        constexpr float strokeWidth      = 0.7f;    // 线宽，逻辑像素
        constexpr int   maxImagePixels   = 8192;    // 单边像素上限：离谱的尺寸只会把内存吃光

        // 固定种子的线性同余发生器（Numerical Recipes 的参数）。不用 juce::Random：它默认拿系统时间当种子，
        // 而开片纹要求同一个种子每次打开插件都画出同一张图。
        class Lcg
        {
        public:
            explicit Lcg (juce::uint32 seed) noexcept : state (seed) {}

            // [0, 1) 的均匀浮点数。只取高 24 位：LCG 的低位周期短、随机性差。
            float nextFloat() noexcept
            {
                state = state * 1664525u + 1013904223u;
                return (float) (state >> 8) * (1.0f / 16777216.0f);
            }

            // [0, count) 的整数。
            int nextInt (int count) noexcept
            {
                return juce::jmin (count - 1, (int) (nextFloat() * (float) count));
            }

            float nextBetween (float low, float high) noexcept
            {
                return low + nextFloat() * (high - low);
            }

        private:
            juce::uint32 state;
        };

        juce::Point<float> clampToArea (juce::Point<float> p, juce::Rectangle<float> area) noexcept
        {
            return { juce::jlimit (area.getX(), area.getRight(),  p.x),
                     juce::jlimit (area.getY(), area.getBottom(), p.y) };
        }
    }

    //==============================================================================
    juce::Path createCracklePath (juce::Rectangle<float> area, juce::uint32 seed, int density)
    {
        juce::Path path;

        if (area.isEmpty() || density <= 0)
            return path;

        Lcg rng (seed);

        for (int crack = 0; crack < density; ++crack)
        {
            // 起点均匀撒在区域里；第一段方向完全随机，之后每段在前一段方向上左转或右转一个随机角度，
            // 折出釉面冰裂那种「走一段拐一下」的形状。
            juce::Point<float> points[maxSegments + 1];
            int numPoints = 1;

            points[0] = { rng.nextBetween (area.getX(), area.getRight()),
                          rng.nextBetween (area.getY(), area.getBottom()) };

            auto angle = rng.nextBetween (0.0f, juce::MathConstants<float>::twoPi);
            const auto segments = minSegments + rng.nextInt (maxSegments - minSegments + 1);

            for (int segment = 0; segment < segments; ++segment)
            {
                if (segment > 0)
                {
                    const auto turn = rng.nextBetween (minTurnRadians, maxTurnRadians);
                    angle += rng.nextFloat() < 0.5f ? -turn : turn;
                }

                const auto from   = points[numPoints - 1];
                const auto length = rng.nextBetween (minSegmentLength, maxSegmentLength);
                const auto to     = clampToArea (from + juce::Point<float> (std::cos (angle), std::sin (angle)) * length, area);

                // 撞到边界被截得几乎没有：这条裂纹到此为止。
                if (to.getDistanceFrom (from) < minUsefulLength)
                    break;

                points[numPoints++] = to;
            }

            // 一个点成不了线：连第一段都被边界吃掉的裂纹直接丢掉，路径里不留孤零零的 moveTo。
            if (numPoints < 2)
                continue;

            path.startNewSubPath (points[0]);

            for (int i = 1; i < numPoints; ++i)
                path.lineTo (points[i]);
        }

        return path;
    }

    //==============================================================================
    juce::Image renderCrackleImage (int width, int height, float pixelScale, juce::uint32 seed, int density)
    {
        if (width <= 0 || height <= 0)
            return {};

        // 非法的像素比按 1 处理，别因为一个 0 把整张图丢了。
        const auto scale       = pixelScale > 0.0f ? pixelScale : 1.0f;
        const auto pixelWidth  = juce::jlimit (1, maxImagePixels, juce::roundToInt ((float) width  * scale));
        const auto pixelHeight = juce::jlimit (1, maxImagePixels, juce::roundToInt ((float) height * scale));

        // ARGB + 清成全透明：只有线条有颜色，贴到任何底色上都行。
        juce::Image image (juce::Image::ARGB, pixelWidth, pixelHeight, true);

        juce::Graphics g (image);

        // 逻辑坐标 → 物理像素。比例按实际得到的像素尺寸算（不是直接用 pixelScale），
        // 四舍五入后的图正好铺满，调用方把它拉到 width × height 的逻辑区域时不会差半个像素。
        g.addTransform (juce::AffineTransform::scale ((float) pixelWidth  / (float) width,
                                                      (float) pixelHeight / (float) height));

        g.setColour (palette::crackle.withAlpha (palette::crackleOpacity));
        g.strokePath (createCracklePath (juce::Rectangle<float> ((float) width, (float) height), seed, density),
                      juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        return image;
    }
}
