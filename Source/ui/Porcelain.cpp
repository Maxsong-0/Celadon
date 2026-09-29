/*
  ==============================================================================

    Porcelain.cpp
    开片网络的生成（与 V4 设计稿的生成器一步一步对应：同一个线性同余种子、同样的随机数消耗顺序）与瓷面的像素渲染
    （设计稿里的 SVG 滤镜链：luminanceToAlpha → 两档高斯模糊各半 → feDiffuseLighting 正片叠底 0.85 →
    feSpecularLighting 滤色 → 斑驳噪声正片叠底 0.45，在这里逐像素算）。

  ==============================================================================
*/

#include "Porcelain.h"

#include "CeladonLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace celadon::ui::porcelain
{
    namespace
    {
        using Vec = std::array<double, 2>;
        using Polygon = std::vector<Vec>;

        // 设计稿的随机数：sd = (imul (sd, 1664525) + 1013904223) >>> 0; rnd = sd / 2^32。
        class Lcg
        {
        public:
            explicit Lcg (std::uint32_t seed) noexcept : state (seed) {}

            double next() noexcept
            {
                state = state * 1664525u + 1013904223u;
                return (double) state / 4294967296.0;
            }

        private:
            std::uint32_t state;
        };

        double polygonArea (const Polygon& p) noexcept
        {
            double a = 0.0;

            for (size_t i = 0; i < p.size(); ++i)
            {
                const auto& u = p[i];
                const auto& v = p[(i + 1) % p.size()];
                a += u[0] * v[1] - v[0] * u[1];
            }

            return std::abs (a) * 0.5;
        }

        Vec centroid (const Polygon& p) noexcept
        {
            Vec c { 0.0, 0.0 };

            for (const auto& v : p)
            {
                c[0] += v[0];
                c[1] += v[1];
            }

            return { c[0] / (double) p.size(), c[1] / (double) p.size() };
        }

        // 主轴方向（二阶矩）：新裂线垂直于它，把狭长的小块横着切开，块才会越切越接近等大的多边形。
        double principalAxis (const Polygon& p, const Vec& c) noexcept
        {
            double sxx = 0.0, syy = 0.0, sxy = 0.0;

            for (const auto& v : p)
            {
                const auto dx = v[0] - c[0], dy = v[1] - c[1];
                sxx += dx * dx;
                syy += dy * dy;
                sxy += dx * dy;
            }

            return 0.5 * std::atan2 (2.0 * sxy, sxx - syy);
        }

        // 中点位移分形：把 a → b 的直线抖成自然的裂纹，递归 depth 层，每层幅度 × 0.52。结果按顺序追加到 acc。
        void fractal (const Vec& a, const Vec& b, double amp, int depth, Lcg& rng, Polygon& acc)
        {
            if (depth == 0)
                return;

            const auto dx = b[0] - a[0], dy = b[1] - a[1];
            auto length = std::hypot (dx, dy);

            if (length == 0.0)
                length = 1.0;

            const auto offset = (rng.next() - 0.5) * 2.0 * amp;
            const Vec m { (a[0] + b[0]) * 0.5 - dy / length * offset, (a[1] + b[1]) * 0.5 + dx / length * offset };

            fractal (a, m, amp * 0.52, depth - 1, rng, acc);
            acc.push_back (m);
            fractal (m, b, amp * 0.52, depth - 1, rng, acc);
        }

        struct Level
        {
            int count;          // 这一级要切几刀
            double bias;        // 选块的权重 = 面积^bias（越大越偏向切大块）
            double jitter;      // 切点离形心的随机偏移（× √面积）
            double spread;      // 切线方向的随机偏转（rad）
            double amplitude;   // 分形抖动幅度（× 切线长度）
            int depth;          // 分形层数
            double minLength;   // 切线短于这个就放弃（逻辑像素）
        };

        constexpr Level levels[] =
        {
            { 12,  1.7,  0.25, 0.5, 0.045, 5, 14.0 },
            { 120, 1.4,  0.3,  0.9, 0.055, 4, 6.0 },
            { 260, 1.15, 0.34, 1.2, 0.06,  3, 3.0 },
        };

        constexpr float crackWidths[]    = { 0.85f, 0.62f, 0.48f };
        constexpr float crackOpacities[] = { 0.42f, 0.32f, 0.24f };
        constexpr float reliefWidths[]   = { 2.0f, 1.5f, 1.05f };   // 高度图里的沟宽

        struct Cell
        {
            Polygon polygon;
            double area;
        };

        std::vector<Crack> generate()
        {
            Lcg rng (0x51a7c0deu);

            Polygon rim;

            for (int i = 0; i < 160; ++i)
            {
                const auto angle = (double) i / 160.0 * juce::MathConstants<double>::twoPi;
                rim.push_back ({ (double) glazeRadius * std::cos (angle), (double) glazeRadius * std::sin (angle) });
            }

            std::vector<Cell> cells { { rim, polygonArea (rim) } };
            std::vector<Crack> cracks;

            const auto splitOnce = [&] (int levelIndex)
            {
                const auto& level = levels[levelIndex];

                // 按「面积^bias」加权随机选一块。
                std::vector<double> weights;
                weights.reserve (cells.size());
                double total = 0.0;

                for (const auto& cell : cells)
                {
                    weights.push_back (std::pow (cell.area, level.bias));
                    total += weights.back();
                }

                auto r = rng.next() * total;
                size_t chosen = 0;

                for (; chosen + 1 < cells.size(); ++chosen)
                {
                    r -= weights[chosen];

                    if (r <= 0.0)
                        break;
                }

                const auto polygon = cells[chosen].polygon;   // 拷贝：下面要替换 cells[chosen]
                const auto area = cells[chosen].area;
                const auto c0 = centroid (polygon);
                const Vec c { c0[0] + (rng.next() - 0.5) * std::sqrt (area) * level.jitter,
                              c0[1] + (rng.next() - 0.5) * std::sqrt (area) * level.jitter };
                const auto theta = principalAxis (polygon, c0) + juce::MathConstants<double>::halfPi + (rng.next() - 0.5) * level.spread;
                const auto dx = std::cos (theta), dy = std::sin (theta);
                const auto side = [&] (const Vec& p) { return (p[0] - c[0]) * dy - (p[1] - c[1]) * dx; };

                Polygon left, right;
                std::vector<Vec> hits;
                int hitLeft[2] = { -1, -1 }, hitRight[2] = { -1, -1 };

                for (size_t i = 0; i < polygon.size(); ++i)
                {
                    const auto& p = polygon[i];
                    const auto& q = polygon[(i + 1) % polygon.size()];
                    const auto sp = side (p), sq = side (q);

                    (sp >= 0.0 ? left : right).push_back (p);

                    if ((sp >= 0.0) != (sq >= 0.0))
                    {
                        const auto u = sp / (sp - sq);
                        const Vec x { p[0] + (q[0] - p[0]) * u, p[1] + (q[1] - p[1]) * u };

                        if (hits.size() < 2)
                        {
                            hitLeft[hits.size()] = (int) left.size();
                            hitRight[hits.size()] = (int) right.size();
                        }

                        left.push_back (x);
                        right.push_back (x);
                        hits.push_back (x);
                    }
                }

                if (hits.size() != 2 || left.size() < 3 || right.size() < 3)
                    return;

                const auto h0 = hits[0], h1 = hits[1];
                const auto length = std::hypot (h1[0] - h0[0], h1[1] - h0[1]);

                if (length < level.minLength)
                    return;

                Polygon mids;
                const auto nx = -(h1[1] - h0[1]) / length, ny = (h1[0] - h0[0]) / length;
                const auto bend = (rng.next() - 0.5) * length * 0.12;
                fractal (h0, h1, length * level.amplitude, level.depth, rng, mids);

                // 整条裂线再整体弯一点（中间最弯）：真瓷器的开片很少是直的。
                for (size_t i = 0; i < mids.size(); ++i)
                {
                    const auto u = (double) (i + 1) / (double) (mids.size() + 1);
                    const auto b = bend * 4.0 * u * (1.0 - u);
                    mids[i] = { mids[i][0] + nx * b, mids[i][1] + ny * b };
                }

                // 弯折后的裂线同时成为两个新块的边（沿着各自多边形的顶点顺序插进去）。
                const auto bendIn = [&mids] (Polygon& shape, int i0, int i1)
                {
                    const auto n = (int) shape.size();

                    if ((i0 + 1) % n == i1)
                        shape.insert (shape.begin() + i0 + 1, mids.begin(), mids.end());
                    else if ((i1 + 1) % n == i0)
                        shape.insert (shape.begin() + i1 + 1, mids.rbegin(), mids.rend());
                };

                bendIn (left, hitLeft[0], hitLeft[1]);
                bendIn (right, hitRight[0], hitRight[1]);

                cells[chosen] = { left, polygonArea (left) };
                cells.insert (cells.begin() + (std::ptrdiff_t) chosen + 1, Cell { right, polygonArea (right) });

                Polygon points;
                points.push_back (h0);
                points.insert (points.end(), mids.begin(), mids.end());
                points.push_back (h1);

                if (rng.next() >= 0.5)
                    std::reverse (points.begin(), points.end());

                Crack crack;
                crack.level = levelIndex;

                for (const auto& p : points)
                    crack.points.push_back ({ (float) p[0], (float) p[1] });

                cracks.push_back (std::move (crack));
            };

            for (int levelIndex = 0; levelIndex < (int) std::size (levels); ++levelIndex)
            {
                const auto goal = cracks.size() + (size_t) levels[levelIndex].count;
                int attempts = 0;

                while (cracks.size() < goal && attempts++ < levels[levelIndex].count * 12)
                    splitOnce (levelIndex);
            }

            // 线宽与不透明度（每条再乘 0.8 ～ 1 的随机数，取两位小数，与设计稿一致）。
            for (auto& crack : cracks)
            {
                crack.width = crackWidths[crack.level];
                crack.opacity = (float) (std::round ((double) crackOpacities[crack.level] * (0.8 + 0.2 * rng.next()) * 100.0) / 100.0);
            }

            return cracks;
        }

        //==============================================================================
        // 平滑的值噪声（斑驳用）：固定排列表的梯度噪声，输出约 −1 ～ 1。
        class GradientNoise
        {
        public:
            explicit GradientNoise (std::uint32_t seed)
            {
                Lcg rng (seed);

                for (int i = 0; i < 256; ++i)
                    permutation[(size_t) i] = (std::uint8_t) i;

                for (int i = 255; i > 0; --i)
                    std::swap (permutation[(size_t) i], permutation[(size_t) (rng.next() * (double) (i + 1)) % 256]);
            }

            float at (float x, float y) const noexcept
            {
                const auto xi = (int) std::floor (x), yi = (int) std::floor (y);
                const auto xf = x - (float) xi, yf = y - (float) yi;
                const auto u = fade (xf), v = fade (yf);

                const auto g00 = grad (hash (xi, yi), xf, yf);
                const auto g10 = grad (hash (xi + 1, yi), xf - 1.0f, yf);
                const auto g01 = grad (hash (xi, yi + 1), xf, yf - 1.0f);
                const auto g11 = grad (hash (xi + 1, yi + 1), xf - 1.0f, yf - 1.0f);

                const auto a = g00 + u * (g10 - g00);
                const auto b = g01 + u * (g11 - g01);
                return (a + v * (b - a)) * 1.4f;
            }

        private:
            static float fade (float t) noexcept   { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

            int hash (int x, int y) const noexcept
            {
                return permutation[(size_t) ((permutation[(size_t) (x & 255)] + y) & 255)];
            }

            static float grad (int h, float x, float y) noexcept
            {
                switch (h & 7)
                {
                    case 0:  return  x + y;
                    case 1:  return -x + y;
                    case 2:  return  x - y;
                    case 3:  return -x - y;
                    case 4:  return  x;
                    case 5:  return -x;
                    case 6:  return  y;
                    default: return -y;
                }
            }

            std::array<std::uint8_t, 256> permutation {};
        };

        // 三次盒式模糊近似高斯（σ 以像素计）：对 w × h 的单通道浮点图原地模糊。
        void boxBlur (std::vector<float>& data, int w, int h, float sigma)
        {
            if (sigma < 0.3f || w <= 0 || h <= 0)
                return;

            // 三个盒子的宽度（Kovesi 的算法）。
            const auto ideal = std::sqrt (12.0f * sigma * sigma / 3.0f + 1.0f);
            auto lower = (int) std::floor (ideal);

            if (lower % 2 == 0)
                --lower;

            const auto upper = lower + 2;
            const auto m = juce::roundToInt ((12.0f * sigma * sigma - 3.0f * (float) (lower * lower) - 12.0f * (float) lower - 9.0f)
                                             / (-4.0f * (float) lower - 4.0f));

            std::vector<float> scratch (data.size());

            const auto pass = [&] (std::vector<float>& from, std::vector<float>& to, int radius, bool horizontal)
            {
                const auto lines = horizontal ? h : w;
                const auto length = horizontal ? w : h;
                const auto stride = horizontal ? 1 : w;
                const auto scale = 1.0f / (float) (2 * radius + 1);

                for (int line = 0; line < lines; ++line)
                {
                    const auto base = horizontal ? line * w : line;
                    const auto sampleAt = [&] (int i) { return from[(size_t) (base + juce::jlimit (0, length - 1, i) * stride)]; };

                    float sum = 0.0f;

                    for (int i = -radius; i <= radius; ++i)
                        sum += sampleAt (i);

                    for (int i = 0; i < length; ++i)
                    {
                        to[(size_t) (base + i * stride)] = sum * scale;
                        sum += sampleAt (i + radius + 1) - sampleAt (i - radius);
                    }
                }
            };

            for (int box = 0; box < 3; ++box)
            {
                const auto size = box < m ? lower : upper;
                const auto radius = juce::jmax (0, (size - 1) / 2);

                if (radius == 0)
                    continue;

                pass (data, scratch, radius, true);
                pass (scratch, data, radius, false);
            }
        }

        juce::Colour mixHex (juce::uint32 from, juce::uint32 to, float amount)
        {
            return juce::Colour (from).interpolatedWith (juce::Colour (to), juce::jlimit (0.0f, 1.0f, amount));
        }

        struct Rgb { float r, g, b; };

        Rgb toRgb (juce::Colour c) noexcept
        {
            return { c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue() };
        }

        Rgb lerp (Rgb a, Rgb b, float t) noexcept
        {
            return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
        }
    }

    //==============================================================================
    const std::vector<Crack>& crackNetwork()
    {
        static const std::vector<Crack> network = generate();
        return network;
    }

    int visibleCrackCount (float glazeProportion) noexcept
    {
        const auto t = juce::jlimit (0.0f, 1.0f, std::isfinite (glazeProportion) ? glazeProportion : 0.0f);
        return juce::roundToInt ((float) crackNetwork().size() * std::pow (t, 0.9f));
    }

    void desaturate (juce::Image& image, float saturation)
    {
        const auto s = juce::jlimit (0.0f, 1.0f, saturation);
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);

        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x)
            {
                auto* pixel = reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (x, y));
                const auto r = (float) pixel->getRed(), g = (float) pixel->getGreen(), b = (float) pixel->getBlue();

                // CSS / SVG saturate() 的矩阵（在预乘的值上做与在原值上做结果相同：矩阵是线性的）。
                const auto nr = (0.213f + 0.787f * s) * r + (0.715f - 0.715f * s) * g + (0.072f - 0.072f * s) * b;
                const auto ng = (0.213f - 0.213f * s) * r + (0.715f + 0.285f * s) * g + (0.072f - 0.072f * s) * b;
                const auto nb = (0.213f - 0.213f * s) * r + (0.715f - 0.715f * s) * g + (0.072f + 0.928f * s) * b;
                const auto a = (float) pixel->getAlpha();

                pixel->setARGB (pixel->getAlpha(),
                                (juce::uint8) juce::jlimit (0.0f, a, nr),
                                (juce::uint8) juce::jlimit (0.0f, a, ng),
                                (juce::uint8) juce::jlimit (0.0f, a, nb));
            }
    }

    juce::Image renderGlaze (float glazeProportion, float pixelScale, bool desaturated)
    {
        const auto t = juce::jlimit (0.0f, 1.0f, std::isfinite (glazeProportion) ? glazeProportion : 0.0f);
        const auto scale = juce::jlimit (0.5f, 8.0f, std::isfinite (pixelScale) ? pixelScale : 1.0f);
        const auto size = juce::jmax (8, juce::roundToInt (2.0f * imageHalf * scale));
        const auto pixelsPerUnit = (float) size / (2.0f * imageHalf);

        const auto& cracks = crackNetwork();
        const auto visible = juce::jmin ((int) cracks.size(), visibleCrackCount (t));

        // 逻辑坐标（圆心为原点）→ 图的像素坐标。
        const auto toImage = juce::AffineTransform::translation (imageHalf, imageHalf).scaled (pixelsPerUnit);

        // ① 高度图：白色圆盘，裂纹处刻成黑色的沟（三级沟宽 2 / 1.5 / 1.05）。
        std::vector<float> height ((size_t) (size * size), 0.0f);
        {
            juce::Image map (juce::Image::RGB, size, size, true);
            juce::Graphics g (map);
            g.addTransform (toImage);
            g.setColour (juce::Colours::white);
            g.fillEllipse (-glazeRadius, -glazeRadius, 2.0f * glazeRadius, 2.0f * glazeRadius);
            g.setColour (juce::Colours::black);

            for (int level = 0; level < 3; ++level)
            {
                juce::Path grooves;

                for (int i = 0; i < visible; ++i)
                {
                    if (cracks[(size_t) i].level != level)
                        continue;

                    const auto& points = cracks[(size_t) i].points;
                    grooves.startNewSubPath (points.front());

                    for (size_t k = 1; k < points.size(); ++k)
                        grooves.lineTo (points[k]);
                }

                g.strokePath (grooves, juce::PathStrokeType (reliefWidths[level], juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            const juce::Image::BitmapData data (map, juce::Image::BitmapData::readOnly);

            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    height[(size_t) (y * size + x)] = (float) data.getPixelColour (x, y).getRed() / 255.0f;
        }

        // ② 两档模糊（σ 1.1 与 3.6 个逻辑像素）各取一半：细处保留沟的锐利，粗处给出柔和的鼓起。
        auto fineBlur = height;
        boxBlur (fineBlur, size, size, 1.1f * pixelsPerUnit);
        auto wideBlur = std::move (height);
        boxBlur (wideBlur, size, size, 3.6f * pixelsPerUnit);

        std::vector<float> surface ((size_t) (size * size));

        for (size_t i = 0; i < surface.size(); ++i)
            surface[i] = 0.5f * (fineBlur[i] + wideBlur[i]);

        // ③ 逐像素：釉色（径向渐变）× 斑驳 × 漫反射，再滤色叠高光。
        const auto g0 = toRgb (mixHex (0xfff2f9f9, 0xff5fb0b4, std::pow (t, 0.7f)));
        const auto g1 = toRgb (mixHex (0xffe1f0f1, 0xff3e9196, std::pow (t, 0.7f)));
        const auto g2 = toRgb (mixHex (0xffc6e1e3, 0xff1f5f66, std::pow (t, 0.7f)));
        const auto diffuseLight = toRgb (juce::Colour (0xffe3f4f4));
        const auto highlightOpacity = 0.6f * (0.7f + 0.3f * t);

        // 光源：方位 225°（左上）、仰角 55°（漫反射）/ 36°（高光）；SVG 的方位按 x 轴顺时针（y 向下）。
        const auto azimuth = juce::degreesToRadians (225.0f);
        const auto elevation = juce::degreesToRadians (55.0f), specularElevation = juce::degreesToRadians (36.0f);
        const float light[3] = { std::cos (azimuth) * std::cos (elevation), std::sin (azimuth) * std::cos (elevation), std::sin (elevation) };
        float halfway[3] = { std::cos (azimuth) * std::cos (specularElevation), std::sin (azimuth) * std::cos (specularElevation),
                             std::sin (specularElevation) + 1.0f };
        {
            const auto n = std::sqrt (halfway[0] * halfway[0] + halfway[1] * halfway[1] + halfway[2] * halfway[2]);

            for (auto& component : halfway)
                component /= n;
        }

        // 法线：SVG 的 Sobel 核 × surfaceScale（2）× ¼（下面的式子里），导数按「每逻辑像素」求（× pixelsPerUnit）：
        // 浏览器的滤镜按 CSS 像素算（设计稿里的样子），任何缩放、任何屏幕上浮雕都一样深。
        const auto normalScale = 2.0f * pixelsPerUnit;

        const GradientNoise mottleNoise (7u), grainNoise (3u);

        // 径向渐变：中心 (0.42, 0.38) × 172 − 86、半径 0.7 × 172（objectBoundingBox）。
        const juce::Point<float> gradientCentre (-glazeRadius + 0.42f * 2.0f * glazeRadius, -glazeRadius + 0.38f * 2.0f * glazeRadius);
        const auto gradientRadius = 0.7f * 2.0f * glazeRadius;

        juce::Image image (juce::Image::ARGB, size, size, true);
        {
            const juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
            const auto at = [&] (int x, int y) { return surface[(size_t) (juce::jlimit (0, size - 1, y) * size + juce::jlimit (0, size - 1, x))]; };

            for (int y = 0; y < size; ++y)
            {
                const auto ly = ((float) y + 0.5f) / pixelsPerUnit - imageHalf;

                for (int x = 0; x < size; ++x)
                {
                    const auto lx = ((float) x + 0.5f) / pixelsPerUnit - imageHalf;
                    const auto r = std::sqrt (lx * lx + ly * ly);
                    const auto coverage = juce::jlimit (0.0f, 1.0f, (glazeRadius - r) * pixelsPerUnit + 0.5f);

                    auto* pixel = reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (x, y));

                    if (coverage <= 0.0f)
                    {
                        pixel->setARGB (0, 0, 0, 0);
                        continue;
                    }

                    // 釉色
                    const auto gt = juce::jlimit (0.0f, 1.0f, juce::Point<float> (lx, ly).getDistanceFrom (gradientCentre) / gradientRadius);
                    auto colour = gt < 0.65f ? lerp (g0, g1, gt / 0.65f) : lerp (g1, g2, (gt - 0.65f) / 0.35f);

                    // 斑驳：大尺度的云状（频率 0.03）+ 细颗粒（频率 0.9），暗青色正片叠底，整体 45 %。
                    {
                        // SVG 的 fractalNoise 大多落在 0.5 ± 0.15 附近；这里的梯度噪声幅度约 ±1，按同样的离散程度缩小。
                        const auto n1 = 0.5f + 0.2f * (0.62f * mottleNoise.at (lx * 0.03f, ly * 0.03f)
                                                       + 0.26f * mottleNoise.at (lx * 0.06f + 17.0f, ly * 0.06f)
                                                       + 0.12f * mottleNoise.at (lx * 0.12f, ly * 0.12f + 5.0f));
                        const auto n2 = 0.5f + 0.12f * grainNoise.at (lx * 0.9f, ly * 0.9f);
                        const auto a1 = juce::jlimit (0.0f, 1.0f, 1.05f - 1.6f * n1);
                        const auto a2 = juce::jlimit (0.0f, 1.0f, 0.8f - 1.4f * n2);

                        // 两层叠在一起（上层细颗粒 over 下层云斑），再以 0.45 正片叠底。
                        const auto alpha = a2 + a1 * (1.0f - a2);

                        if (alpha > 0.0f)
                        {
                            const Rgb over { (0.08f * a2 + 0.1f * a1 * (1.0f - a2)) / alpha,
                                             (0.2f * a2 + 0.25f * a1 * (1.0f - a2)) / alpha,
                                             (0.2f * a2 + 0.26f * a1 * (1.0f - a2)) / alpha };
                            const auto k = alpha * 0.3f;
                            colour = { colour.r * (1.0f - k * (1.0f - over.r)),
                                       colour.g * (1.0f - k * (1.0f - over.g)),
                                       colour.b * (1.0f - k * (1.0f - over.b)) };
                        }
                    }

                    // 浮雕：Sobel 求梯度 → 法线
                    const auto nx = -normalScale * 0.25f * ((at (x + 1, y - 1) + 2.0f * at (x + 1, y) + at (x + 1, y + 1))
                                                          - (at (x - 1, y - 1) + 2.0f * at (x - 1, y) + at (x - 1, y + 1)));
                    const auto ny = -normalScale * 0.25f * ((at (x - 1, y + 1) + 2.0f * at (x, y + 1) + at (x + 1, y + 1))
                                                          - (at (x - 1, y - 1) + 2.0f * at (x, y - 1) + at (x + 1, y - 1)));
                    const auto norm = std::sqrt (nx * nx + ny * ny + 1.0f);
                    const float normal[3] = { nx / norm, ny / norm, 1.0f / norm };

                    // 漫反射（kd 1.22，浅青白光）正片叠底 0.85
                    const auto lambert = juce::jmax (0.0f, 1.22f * (normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2]));
                    colour = { colour.r * (0.15f + 0.85f * juce::jmin (1.0f, lambert * diffuseLight.r)),
                               colour.g * (0.15f + 0.85f * juce::jmin (1.0f, lambert * diffuseLight.g)),
                               colour.b * (0.15f + 0.85f * juce::jmin (1.0f, lambert * diffuseLight.b)) };

                    // 高光（ks 0.9，指数 14）滤色：结果 = 底 + 不透明度 × spec² × (1 − 底)（见文件头）
                    const auto nDotH = juce::jmax (0.0f, normal[0] * halfway[0] + normal[1] * halfway[1] + normal[2] * halfway[2]);
                    const auto specular = juce::jmin (1.0f, 0.9f * std::pow (nDotH, 14.0f));
                    const auto lift = highlightOpacity * specular * specular;
                    colour = { colour.r + lift * (1.0f - colour.r), colour.g + lift * (1.0f - colour.g), colour.b + lift * (1.0f - colour.b) };

                    const auto alpha = (juce::uint8) juce::roundToInt (coverage * 255.0f);
                    const auto premultiply = [coverage] (float channel)
                    {
                        return (juce::uint8) juce::roundToInt (juce::jlimit (0.0f, 1.0f, channel) * coverage * 255.0f);
                    };

                    pixel->setARGB (alpha, premultiply (colour.r), premultiply (colour.g), premultiply (colour.b));
                }
            }
        }

        // ④ 矢量部分：深色裂线、边缘暗角、左上柔光、两道细边、旋钮帽。
        {
            juce::Graphics g (image);
            g.addTransform (toImage);

            {
                juce::Graphics::ScopedSaveState saved (g);
                juce::Path clip;
                clip.addEllipse (-85.5f, -85.5f, 171.0f, 171.0f);
                g.reduceClipRegion (clip);

                for (int i = 0; i < visible; ++i)
                {
                    const auto& crack = cracks[(size_t) i];
                    juce::Path line;
                    line.startNewSubPath (crack.points.front());

                    for (size_t k = 1; k < crack.points.size(); ++k)
                        line.lineTo (crack.points[k]);

                    g.setColour (juce::Colour (0xff244f50).withAlpha (crack.opacity));
                    g.strokePath (line, juce::PathStrokeType (crack.width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }

                // 边缘暗角：半径 80 % 以内透明，到边上 #06313A 30 %。
                juce::ColourGradient rim (juce::Colour (0x0006313a), 0.0f, 0.0f, juce::Colour (0x4d06313a), glazeRadius, 0.0f, true);
                rim.addColour (0.8, juce::Colour (0x0006313a));
                g.setGradientFill (rim);
                g.fillEllipse (-glazeRadius, -glazeRadius, 2.0f * glazeRadius, 2.0f * glazeRadius);

                // 左上的柔光：以 (−30, −42) 为心、46 × 26、转 −28° 的椭圆，白 72 % → 0。
                {
                    juce::Graphics::ScopedSaveState glossState (g);
                    g.addTransform (juce::AffineTransform::scale (1.0f, 26.0f / 46.0f)
                                        .rotated (juce::degreesToRadians (-28.0f))
                                        .translated (-30.0f, -42.0f));
                    juce::ColourGradient gloss (juce::Colours::white.withAlpha (0.72f), 0.0f, 0.0f,
                                                juce::Colours::white.withAlpha (0.0f), 46.0f, 0.0f, true);
                    g.setGradientFill (gloss);
                    g.fillEllipse (-46.0f, -46.0f, 92.0f, 92.0f);
                }
            }

            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawEllipse (-85.5f, -85.5f, 171.0f, 171.0f, 1.0f);
            g.setColour (juce::Colour (0x2906313a));   // #06313A 16 %
            g.drawEllipse (-87.5f, -87.5f, 175.0f, 175.0f, 1.0f);

            // 旋钮帽（半径 36）：下方柔影 + 左上亮的深色径向渐变。
            {
                juce::ColourGradient shadow (juce::Colour (6, 24, 29).withAlpha (0.35f), 0.0f, 2.0f,
                                             juce::Colour (6, 24, 29).withAlpha (0.0f), capRadius + 5.0f, 2.0f, true);
                shadow.addColour (capRadius / (capRadius + 5.0f), juce::Colour (6, 24, 29).withAlpha (0.3f));
                g.setGradientFill (shadow);
                g.fillEllipse (-(capRadius + 5.0f), -(capRadius + 5.0f) + 2.0f, 2.0f * (capRadius + 5.0f), 2.0f * (capRadius + 5.0f));

                const juce::Point<float> focus (-capRadius + 0.4f * 2.0f * capRadius, -capRadius + 0.32f * 2.0f * capRadius);
                juce::ColourGradient cap (palette::capTop, focus, palette::capBottom, focus.translated (0.8f * 2.0f * capRadius, 0.0f), true);
                g.setGradientFill (cap);
                g.fillEllipse (-capRadius, -capRadius, 2.0f * capRadius, 2.0f * capRadius);
            }
        }

        if (desaturated)
            desaturate (image, 0.3f);

        return image;
    }
}
