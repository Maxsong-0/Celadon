/*
  ==============================================================================

    Porcelain.cpp
    固定种子的开片网络与温润的青瓷釉面。开片是釉下细线，不参与表面法线；
    整体柔和渐变、极轻的云状釉色、边缘积釉与宽软反光，维持一整块烧制瓷面的连续质感。

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

        // 逻辑像素线宽随屏幕像素比一起渲染；细开片只作釉下肌理，不形成亮暗切面。
        constexpr float crackWidths[]    = { 0.48f, 0.34f, 0.24f };
        constexpr float crackOpacities[] = { 0.21f, 0.15f, 0.10f };

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

        // 三档釉色（g0 近圆心、g1 中段、g2 外缘）：从浅灰青向较深的豆青过渡，depth = GLAZE^0.75。
        // depth 0 / 1 正好是两端（Colour::interpolatedWith 在 ≤ 0 / ≥ 1 时原样返回端点色）。
        struct GlazePalette
        {
            Rgb g0, g1, g2;
        };

        GlazePalette paletteFor (float depth)
        {
            return { toRgb (mixHex (0xffeff3ed, 0xffbfd4c8, depth)),
                     toRgb (mixHex (0xffdce8e1, 0xff95b5a9, depth)),
                     toRgb (mixHex (0xffbcd1c7, 0xff71978d, depth)) };
        }

        // 图（正方形，边长 size 像素）覆盖逻辑区域 [−imageHalf, imageHalf]²：逻辑坐标 → 像素。
        juce::AffineTransform toImageTransform (int size)
        {
            return juce::AffineTransform::translation (imageHalf, imageHalf).scaled ((float) size / (2.0f * imageHalf));
        }

        // 底层的一个像素里与 GLAZE 无关的量：覆盖率、在哪一段釉色渐变里（内段 g0 → g1 / 外段 g1 → g2）与段内位置、云斑与微粒。
        // 分层渲染按像素比把它们算好存着，每次只按当次的三档釉色着色（shadeBase）——与参照逐像素同一套算式、同一个着色函数，
        // 底层与原算法逐位相同。
        struct BaseSample
        {
            float coverage = 0.0f;       // ≤ 0：圆外，透明
            float mix = 0.0f;            // 段内位置（参照的 gt / 0.68 或 (gt − 0.68) / 0.32）
            float variation = 0.0f;      // 云斑与微粒（亮度起伏约 2 %）
            bool outer = false;          // 外段（gt ≥ 0.68）
        };

        struct BaseSampler
        {
            explicit BaseSampler (int size)
                : pixelsPerUnit ((float) size / (2.0f * imageHalf)) {}

            BaseSample at (int x, int y) const noexcept
            {
                const auto ly = ((float) y + 0.5f) / pixelsPerUnit - imageHalf;
                const auto lx = ((float) x + 0.5f) / pixelsPerUnit - imageHalf;
                const auto radius = std::sqrt (lx * lx + ly * ly);
                const auto coverage = juce::jlimit (0.0f, 1.0f, (glazeRadius - radius) * pixelsPerUnit + 0.5f);

                if (coverage <= 0.0f)
                    return {};

                const auto distance = juce::Point<float> (lx, ly).getDistanceFrom (gradientCentre);
                const auto gt = juce::jlimit (0.0f, 1.0f, distance / gradientRadius);

                // 很轻的釉色云斑与微粒；亮度起伏仅约 2 %，在小尺寸下仍是一整块温润的瓷。
                const auto cloud = 0.72f * mottleNoise.at (lx * 0.026f, ly * 0.026f)
                                 + 0.28f * mottleNoise.at (lx * 0.058f + 17.0f, ly * 0.058f + 5.0f);
                const auto grain = grainNoise.at (lx * 0.82f, ly * 0.82f);
                const auto variation = cloud * 0.018f + grain * 0.0025f;

                BaseSample sample;
                sample.coverage = coverage;
                sample.outer = ! (gt < 0.68f);
                sample.mix = sample.outer ? (gt - 0.68f) / 0.32f : gt / 0.68f;
                sample.variation = variation;
                return sample;
            }

            const float pixelsPerUnit;
            const GradientNoise mottleNoise { 7u }, grainNoise { 3u };
            const juce::Point<float> gradientCentre { -22.0f, -30.0f };
            static constexpr float gradientRadius = glazeRadius * 1.45f;
        };

        // 按当次的三档釉色给一个底层像素着色：釉色按半径渐变 + 云斑与微粒，按覆盖率预乘。整面共享明暗，裂纹不把高光切成晶体状碎块。
        void shadeBase (juce::PixelARGB& pixel, const BaseSample& sample, const GlazePalette& palette) noexcept
        {
            if (sample.coverage <= 0.0f)
            {
                pixel.setARGB (0, 0, 0, 0);
                return;
            }

            auto colour = sample.outer ? lerp (palette.g1, palette.g2, sample.mix) : lerp (palette.g0, palette.g1, sample.mix);
            colour = { colour.r + sample.variation * 0.88f,
                       colour.g + sample.variation,
                       colour.b + sample.variation * 0.92f };

            const auto coverage = sample.coverage;
            const auto alpha = (juce::uint8) juce::roundToInt (coverage * 255.0f);
            const auto premultiply = [coverage] (float channel)
            {
                return (juce::uint8) juce::roundToInt (juce::jlimit (0.0f, 1.0f, channel) * coverage * 255.0f);
            };

            pixel.setARGB (alpha, premultiply (colour.r), premultiply (colour.g), premultiply (colour.b));
        }

        // 釉面圆的裁剪（裂纹与釉光层都画在里面）。
        juce::Path glazeClip()
        {
            juce::Path clip;
            clip.addEllipse (-glazeRadius + 0.5f, -glazeRadius + 0.5f, 2.0f * glazeRadius - 1.0f, 2.0f * glazeRadius - 1.0f);
            return clip;
        }

        juce::Path crackLine (const Crack& crack)
        {
            juce::Path line;
            line.startNewSubPath (crack.points.front());

            for (size_t k = 1; k < crack.points.size(); ++k)
                line.lineTo (crack.points[k]);

            return line;
        }

        juce::PathStrokeType crackStroke (const Crack& crack)
        {
            return { crack.width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded };
        }

        juce::Colour crackColour (const Crack& crack, float t)
        {
            return juce::Colour (0xff4b7166).withAlpha (crack.opacity * (0.8f + 0.2f * t));
        }

        // 釉光层（画在釉面圆的裁剪里、裂纹之上；与 GLAZE 无关）：边缘积釉、宽软的暖白釉光、右下方的环境反光。
        void drawSheen (juce::Graphics& g)
        {
            // 边缘积釉：外缘略深，过渡足够宽，不产生金属的锐利倒角。
            juce::ColourGradient rim (juce::Colour (0x003d665e), 0.0f, 0.0f,
                                     juce::Colour (0x293d665e), glazeRadius, 0.0f, true);
            rim.addColour (0.72, juce::Colour (0x003d665e));
            rim.addColour (0.92, juce::Colour (0x103d665e));
            g.setGradientFill (rim);
            g.fillEllipse (-glazeRadius, -glazeRadius, 2.0f * glazeRadius, 2.0f * glazeRadius);

            // 宽软的暖白釉光：一整片反射，不为每块开片生成独立高光。
            {
                juce::Graphics::ScopedSaveState glossState (g);
                g.addTransform (juce::AffineTransform::scale (1.0f, 29.0f / 59.0f)
                                    .rotated (juce::degreesToRadians (-24.0f))
                                    .translated (-26.0f, -43.0f));
                const auto pearl = juce::Colour (0xfffffdf5);
                juce::ColourGradient gloss (pearl.withAlpha (0.36f), 0.0f, 0.0f,
                                            pearl.withAlpha (0.0f), 59.0f, 0.0f, true);
                gloss.addColour (0.3, pearl.withAlpha (0.28f));
                g.setGradientFill (gloss);
                g.fillEllipse (-59.0f, -59.0f, 118.0f, 118.0f);
            }

            // 右下方的微弱环境反光使釉面有厚度，暗部仍保留青色。
            {
                juce::Graphics::ScopedSaveState bounceState (g);
                g.addTransform (juce::AffineTransform::scale (1.0f, 23.0f / 46.0f)
                                    .rotated (juce::degreesToRadians (-32.0f))
                                    .translated (31.0f, 48.0f));
                const auto bounceColour = juce::Colour (0xffe1f0e6);
                juce::ColourGradient bounce (bounceColour.withAlpha (0.18f), 0.0f, 0.0f,
                                             bounceColour.withAlpha (0.0f), 46.0f, 0.0f, true);
                g.setGradientFill (bounce);
                g.fillEllipse (-46.0f, -46.0f, 92.0f, 92.0f);
            }
        }

        // 外缘两圈与旋钮帽（裁剪之外、最上层；与 GLAZE 无关）。
        void drawRimAndCap (juce::Graphics& g)
        {
            // 外缘保留一圈细薄釉光；减淡外围深线，让主体不再像镶在金属圈里的宝石。
            g.setColour (juce::Colour (0xfffffdf5).withAlpha (0.62f));
            g.drawEllipse (-85.5f, -85.5f, 171.0f, 171.0f, 0.9f);
            g.setColour (juce::Colour (0x163d665e));
            g.drawEllipse (-87.0f, -87.0f, 174.0f, 174.0f, 0.8f);

            // 深青色旋钮帽保留读数与指针的对比度；柔影和细上缘使它像瓷面上的凹入控制区。
            juce::ColourGradient shadow (juce::Colour (6, 24, 29).withAlpha (0.25f), 0.0f, 2.0f,
                                         juce::Colour (6, 24, 29).withAlpha (0.0f), capRadius + 5.0f, 2.0f, true);
            shadow.addColour (capRadius / (capRadius + 5.0f), juce::Colour (6, 24, 29).withAlpha (0.2f));
            g.setGradientFill (shadow);
            g.fillEllipse (-(capRadius + 5.0f), -(capRadius + 5.0f) + 2.0f, 2.0f * (capRadius + 5.0f), 2.0f * (capRadius + 5.0f));

            const juce::Point<float> focus (-capRadius + 0.4f * 2.0f * capRadius, -capRadius + 0.32f * 2.0f * capRadius);
            juce::ColourGradient cap (palette::capTop, focus, palette::capBottom,
                                     focus.translated (0.8f * 2.0f * capRadius, 0.0f), true);
            g.setGradientFill (cap);
            g.fillEllipse (-capRadius, -capRadius, 2.0f * capRadius, 2.0f * capRadius);
            g.setColour (juce::Colour (0xffedf7ef).withAlpha (0.2f));
            g.drawEllipse (-capRadius, -capRadius, 2.0f * capRadius, 2.0f * capRadius, 0.75f);
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

        const auto& cracks = crackNetwork();
        const auto visible = juce::jmin ((int) cracks.size(), visibleCrackCount (t));

        // 从浅灰青向较深的豆青过渡；整面共享明暗，裂纹不再把高光切成晶体状碎块。
        juce::Image image (juce::Image::ARGB, size, size, true);
        {
            const auto palette = paletteFor (std::pow (t, 0.75f));
            const BaseSampler sampler (size);
            const juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    shadeBase (*reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (x, y)), sampler.at (x, y), palette);
        }

        {
            juce::Graphics g (image);
            g.addTransform (toImageTransform (size));

            {
                juce::Graphics::ScopedSaveState saved (g);
                g.reduceClipRegion (glazeClip());

                // 细开片被后续的釉光覆盖，看起来嵌在透明釉层内，而非刻在旋钮表面。
                for (int i = 0; i < visible; ++i)
                {
                    const auto& crack = cracks[(size_t) i];
                    g.setColour (crackColour (crack, t));
                    g.strokePath (crackLine (crack), crackStroke (crack));
                }

                drawSheen (g);
            }

            drawRimAndCap (g);
        }

        if (desaturated)
            desaturate (image, 0.3f);

        return image;
    }

    //==============================================================================
    void GlazeRenderer::buildLayers (int size)
    {
        // 底层里与 GLAZE 无关的逐像素量（三次梯度噪声、两次开方都在这里做完）。
        {
            const BaseSampler sampler (size);
            baseSamples.resize ((size_t) size * (size_t) size);

            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    const auto sample = sampler.at (x, y);
                    // 段别放在覆盖率的符号里（覆盖的像素覆盖率 > 0，取负与取绝对值都是精确的）：每像素 12 字节。
                    baseSamples[(size_t) y * (size_t) size + (size_t) x] = { sample.outer ? -sample.coverage : sample.coverage,
                                                                           sample.mix, sample.variation };
                }
        }

        // 覆盖层：与参照同一组绘制调用、同一叠放顺序（釉面圆裁剪里的积釉 / 釉光 / 反光，之后外环与帽子），画在透明图上。
        overlay = juce::Image (juce::Image::ARGB, size, size, true);
        {
            juce::Graphics g (overlay);
            g.addTransform (toImageTransform (size));

            {
                juce::Graphics::ScopedSaveState saved (g);
                g.reduceClipRegion (glazeClip());
                drawSheen (g);
            }

            drawRimAndCap (g);
        }

        layerSize = size;
        crackOutlines.clear();   // 轮廓精度跟着像素比：下次渲染时按新精度重建
        ++layerBuilds;
    }

    void GlazeRenderer::buildCrackOutlines (float accuracy)
    {
        // 与参照的 strokePath 同一条路：软件渲染器的 strokePath = 按此刻物理像素比 createStrokedPath，再 fillPath。
        const auto& cracks = crackNetwork();
        crackOutlines.clear();
        crackOutlines.reserve (cracks.size());

        for (const auto& crack : cracks)
        {
            juce::Path outline;
            crackStroke (crack).createStrokedPath (outline, crackLine (crack), {}, accuracy);
            crackOutlines.push_back (std::move (outline));
        }

        crackOutlineAccuracy = accuracy;
    }

    juce::Image GlazeRenderer::render (float glazeProportion, float pixelScale, bool desaturated)
    {
        const auto t = juce::jlimit (0.0f, 1.0f, std::isfinite (glazeProportion) ? glazeProportion : 0.0f);
        const auto scale = juce::jlimit (0.5f, 8.0f, std::isfinite (pixelScale) ? pixelScale : 1.0f);
        const auto size = juce::jmax (8, juce::roundToInt (2.0f * imageHalf * scale));

        if (size != layerSize)
            buildLayers (size);

        const auto& cracks = crackNetwork();
        const auto visible = juce::jmin ((int) cracks.size(), visibleCrackCount (t));

        // 底层：按当次的三档釉色逐像素着色——与参照同一个着色函数，逐位相同；与 GLAZE 无关的量都在建层时算好了。
        juce::Image image (juce::Image::ARGB, size, size, false);   // 下面每个像素都写
        {
            const auto palette = paletteFor (std::pow (t, 0.75f));
            const juce::Image::BitmapData out (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < size; ++y)
            {
                const auto* samples = baseSamples.data() + (size_t) y * (size_t) size;
                auto* o = out.getLinePointer (y);

                for (int x = 0; x < size; ++x, o += out.pixelStride)
                {
                    const auto& stored = samples[x];
                    BaseSample sample;
                    sample.coverage = std::abs (stored.signedCoverage);
                    sample.outer = stored.signedCoverage < 0.0f;
                    sample.mix = stored.mix;
                    sample.variation = stored.variation;
                    shadeBase (*reinterpret_cast<juce::PixelARGB*> (o), sample, palette);
                }
            }
        }

        {
            juce::Graphics g (image);

            {
                juce::Graphics::ScopedSaveState saved (g);
                g.addTransform (toImageTransform (size));
                g.reduceClipRegion (glazeClip());

                const auto accuracy = g.getInternalContext().getPhysicalPixelScaleFactor();

                if (crackOutlines.size() != cracks.size() || ! juce::exactlyEqual (accuracy, crackOutlineAccuracy))
                    buildCrackOutlines (accuracy);

                for (int i = 0; i < visible; ++i)
                {
                    g.setColour (crackColour (cracks[(size_t) i], t));
                    g.fillPath (crackOutlines[(size_t) i]);
                }
            }

            // 覆盖层与图同尺寸、像素对像素（此刻的颜色是 Graphics 初始的不透明黑：整张原样叠上）。
            g.drawImageAt (overlay, 0, 0);
        }

        if (desaturated)
            desaturate (image, 0.3f);

        return image;
    }
}
