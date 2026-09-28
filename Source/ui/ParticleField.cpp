/*
  ==============================================================================

    ParticleField.cpp

  ==============================================================================
*/

#include "ParticleField.h"

#include <algorithm>
#include <cmath>

namespace celadon::ui
{
    namespace
    {
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

        constexpr float centreX = 600.0f, centreY = 456.0f, rimRadius = 108.0f;
        constexpr int linesPerSide = 22, particlesPerSide = 200, samplesPerLine = 48;
        constexpr double ringDuration = 3.9;
        constexpr double ringBegins[] = { 0.0, -1.3, -2.6 };
        constexpr double maxFrameRate = 45.0;

        // 两边各自的缓动（SMIL calcMode="spline" 的 keySplines）：左边进来时先慢后快（被吸进去），右边出去时先快后慢。
        constexpr float easeLeft[]  = { 0.45f, 0.0f, 0.85f, 0.7f };
        constexpr float easeRight[] = { 0.15f, 0.3f, 0.55f, 1.0f };

        constexpr juce::uint32 particleColours[] = { 0xff33b3c4, 0xff1799ab, 0xff1799ab, 0xff127f90, 0xff0e6473, 0xff0a4a57 };

        // CSS / SMIL 的三次贝塞尔缓动：已知时间比例 x，解出曲线上的 y。
        float cubicBezier (const float* c, float x) noexcept
        {
            const auto bezier = [] (float a, float b, float s) { return 3.0f * a * s * (1.0f - s) * (1.0f - s) + 3.0f * b * s * s * (1.0f - s) + s * s * s; };
            float low = 0.0f, high = 1.0f, s = x;

            for (int i = 0; i < 24; ++i)
            {
                const auto value = bezier (c[0], c[2], s);

                if (std::abs (value - x) < 1.0e-4f)
                    break;

                if (value < x)
                    low = s;
                else
                    high = s;

                s = 0.5f * (low + high);
            }

            return bezier (c[1], c[3], s);
        }

        float follow (float current, float target, double dt, float up, float down) noexcept
        {
            return current + (target - current) * (float) juce::jmin (1.0, dt * (double) (target > current ? up : down));
        }

        juce::Point<float> cubicPoint (const juce::Point<float>* p, float t) noexcept
        {
            const auto u = 1.0f - t;
            return p[0] * (u * u * u) + p[1] * (3.0f * u * u * t) + p[2] * (3.0f * u * t * t) + p[3] * (t * t * t);
        }
    }

    ParticleField::ParticleField()
        : vblank (this, [this] (double timestamp) { onVBlank (timestamp); })
    {
        setInterceptsMouseClicks (false, false);
        setOpaque (false);
        build();
    }

    ParticleField::~ParticleField() = default;

    void ParticleField::build()
    {
        Lcg rng (0x50415254u);

        for (int i = 0; i < linesPerSide; ++i)
        {
            const auto u = ((double) i + 0.5) / (double) linesPerSide - 0.5;

            // 左：从画面左缘（x 20 ～ 60）汇向旋钮左侧的边缘。
            const auto x0 = 20.0 + rng.next() * 40.0;
            const auto y0 = (double) centreY + u * 212.0 + (rng.next() - 0.5) * 10.0;
            const auto theta = juce::MathConstants<double>::pi - u * 1.25;
            const auto ex = (double) centreX + (double) rimRadius * std::cos (theta);
            const auto ey = (double) centreY + (double) rimRadius * std::sin (theta);

            const juce::Point<float> left[] = { { (float) x0, (float) y0 },
                                                { (float) (x0 + (ex - x0) * 0.42), (float) y0 },
                                                { (float) (x0 + (ex - x0) * 0.8), (float) (y0 + (ey - y0) * 0.9) },
                                                { (float) ex, (float) ey } };

            // 右：从旋钮右侧的边缘流向画面右缘（x 1140 ～ 1180）。
            const auto sx = (double) centreX + (double) rimRadius * std::cos (u * 1.25);
            const auto sy = (double) centreY + (double) rimRadius * std::sin (u * 1.25);
            const auto x1 = 1180.0 - rng.next() * 40.0;
            const auto y1 = (double) centreY + u * 212.0 + (rng.next() - 0.5) * 10.0;

            const juce::Point<float> right[] = { { (float) sx, (float) sy },
                                                 { (float) (sx + (x1 - sx) * 0.2), (float) (sy + (y1 - sy) * 0.1) },
                                                 { (float) (sx + (x1 - sx) * 0.58), (float) y1 },
                                                 { (float) x1, (float) y1 } };

            for (int side = 0; side < 2; ++side)
            {
                const auto* control = side == 0 ? left : right;
                auto& curves = side == 0 ? leftCurves : rightCurves;
                curves.startNewSubPath (control[0]);
                curves.cubicTo (control[1], control[2], control[3]);

                Streamline line;
                float total = 0.0f;

                for (int k = 0; k <= samplesPerLine; ++k)
                {
                    const auto point = cubicPoint (control, (float) k / (float) samplesPerLine);

                    if (! line.points.empty())
                        total += point.getDistanceFrom (line.points.back());

                    line.points.push_back (point);
                    line.lengths.push_back (total);
                }

                lines[(size_t) side].push_back (std::move (line));
            }
        }

        // 粒子：每边 200 个（与设计稿的 mk() 同样的随机数消耗顺序）。
        for (int side = 0; side < 2; ++side)
        {
            for (int k = 0; k < particlesPerSide; ++k)
            {
                Particle p;
                p.right = side == 1;
                p.soft = rng.next() < 0.15;
                p.duration = (float) (3.6 + rng.next() * 2.8);
                p.opacity = (float) (p.soft ? 0.16 + rng.next() * 0.2 : 0.45 + rng.next() * 0.5);
                p.line = juce::jlimit (0, linesPerSide - 1, (int) std::floor (rng.next() * (double) linesPerSide));
                p.radius = (float) (p.soft ? 2.4 + rng.next() * 2.4 : 0.7 + rng.next() * 1.6);
                p.colour = juce::Colour (particleColours[juce::jlimit (0, 5, (int) std::floor (rng.next() * 6.0))]);
                p.offset = (float) (-rng.next() * (double) p.duration);
                particles[(size_t) side].push_back (p);
            }
        }
    }

    void ParticleField::setGlaze (float proportion)
    {
        proportion = juce::jlimit (0.0f, 1.0f, std::isfinite (proportion) ? proportion : 0.0f);

        if (std::abs (proportion - glaze) < 1.0e-4f)
            return;

        glaze = proportion;
        repaint();
    }

    void ParticleField::setAudio (float rmsDb, bool isPlaying)
    {
        // 电平 → 0 ～ 1：−36 dBFS 起算、−10 dBFS 满（母带常见的 RMS 在 −14 ～ −8）。升得快、落得慢一点。
        const auto target = isPlaying && std::isfinite (rmsDb) ? juce::jlimit (0.0f, 1.0f, (rmsDb + 36.0f) / 26.0f) : 0.0f;
        energy += (target - energy) * (target > energy ? 0.6f : 0.25f);
        playing = isPlaying;
    }

    void ParticleField::setReduceMotion (bool shouldReduceMotion)
    {
        if (reduceMotion == shouldReduceMotion)
            return;

        reduceMotion = shouldReduceMotion;
        repaint();
    }

    void ParticleField::step (double seconds)
    {
        const auto dt = juce::jlimit (0.0, 0.1, seconds);
        const auto targetSpeed = playing && ! reduceMotion ? (0.35f + 0.95f * energy) * (comparing ? 0.3f : 1.0f) : 0.0f;
        speed = follow (speed, targetSpeed, dt, 3.0f, 1.3f);
        clock += dt * (double) speed;

        const auto targetBrightness = playing ? (0.45f + 0.55f * energy) * (comparing ? 0.55f : 1.0f) : 0.3f;
        brightness = follow (brightness, targetBrightness, dt, 2.5f, 2.5f);

        // 要重画：粒子与波纹在动（有粒子、没关动效、速度不为 0），或者整体亮度还在变（流线的深浅跟它）。
        const auto particlesMoving = speed > 1.0e-3f && ! reduceMotion && glaze > 0.0f;
        animating = particlesMoving || std::abs (targetBrightness - brightness) > 2.0e-3f;
    }

    void ParticleField::advance (double seconds)
    {
        // 截图用：按 30 帧 / s 的步长走，与实际播放时的平滑一致。
        for (double t = 0.0; t < seconds; t += 1.0 / 30.0)
            step (1.0 / 30.0);

        repaint();
    }

    void ParticleField::onVBlank (double timestampSeconds)
    {
        const auto dt = lastVBlank < 0.0 ? 0.0 : timestampSeconds - lastVBlank;
        lastVBlank = timestampSeconds;
        step (dt);

        if (animating && (lastRepaint < 0.0 || timestampSeconds - lastRepaint >= 1.0 / maxFrameRate))
        {
            lastRepaint = timestampSeconds;
            repaint();
        }
    }

    juce::Point<float> ParticleField::positionOn (const Streamline& line, float fraction) const noexcept
    {
        const auto total = line.lengths.back();
        const auto distance = juce::jlimit (0.0f, 1.0f, fraction) * total;
        const auto it = std::lower_bound (line.lengths.begin(), line.lengths.end(), distance);
        const auto index = juce::jlimit ((std::ptrdiff_t) 1, (std::ptrdiff_t) line.lengths.size() - 1, it - line.lengths.begin());
        const auto from = line.lengths[(size_t) index - 1], to = line.lengths[(size_t) index];
        const auto t = to > from ? (distance - from) / (to - from) : 0.0f;
        return line.points[(size_t) index - 1] + (line.points[(size_t) index] - line.points[(size_t) index - 1]) * t;
    }

    void ParticleField::renderLines (float pixelScale)
    {
        const auto w = juce::jmax (1, juce::roundToInt (1200.0f * pixelScale));
        const auto h = juce::jmax (1, juce::roundToInt ((float) bandHeight * pixelScale));
        lineImage = juce::Image (juce::Image::ARGB, w, h, true);
        lineImageScale = pixelScale;

        juce::Graphics g (lineImage);
        g.addTransform (juce::AffineTransform::translation (0.0f, (float) -top).scaled (pixelScale));

        juce::ColourGradient leftGradient (juce::Colour (0x0033b3c4), 20.0f, 0.0f, juce::Colour (0xff0e6473), 492.0f, 0.0f, false);
        leftGradient.addColour (0.6, juce::Colour (0x8c1799ab));
        g.setGradientFill (leftGradient);
        g.strokePath (leftCurves, juce::PathStrokeType (0.9f));

        juce::ColourGradient rightGradient (juce::Colour (0xff0e6473), 708.0f, 0.0f, juce::Colour (0x0033b3c4), 1180.0f, 0.0f, false);
        rightGradient.addColour (0.4, juce::Colour (0x8c1799ab));
        g.setGradientFill (rightGradient);
        g.strokePath (rightCurves, juce::PathStrokeType (0.9f));
    }

    void ParticleField::paint (juce::Graphics& g)
    {
        // 流线缓存最多按 2.5 倍像素生成（很淡的细线，再高看不出区别，200 % × Retina 时图会很大）。
        const auto pixelScale = juce::jlimit (0.5f, 2.5f, g.getInternalContext().getPhysicalPixelScaleFactor());
        g.addTransform (juce::AffineTransform::translation (0.0f, (float) -top));   // 以下按画布坐标画

        // 柔光：旋钮后面的一团浅青（强度跟 GLAZE）。
        {
            const juce::Point<float> centre (centreX, centreY);
            juce::ColourGradient glow (juce::Colour (0x5733b3c4), centre, juce::Colour (0x0067cad7), centre.translated (200.0f, 0.0f), true);
            glow.addColour (0.55, juce::Colour (0x1f67cad7));
            glow.multiplyOpacity (0.2f + 0.8f * glaze);
            g.setGradientFill (glow);
            g.fillEllipse (juce::Rectangle<float> (400.0f, 400.0f).withCentre (centre));
        }

        // 流线（缓存图）
        if (! lineImage.isValid() || ! juce::approximatelyEqual (lineImageScale, pixelScale))
            renderLines (pixelScale);

        const auto lineOpacity = ((reduceMotion ? 0.04f : 0.08f) + 0.4f * glaze) * brightness;
        g.setOpacity (juce::jlimit (0.0f, 1.0f, lineOpacity));
        g.drawImageTransformed (lineImage, juce::AffineTransform::scale (1.0f / lineImageScale).translated (0.0f, (float) top), false);

        if (reduceMotion || glaze <= 0.0f)
            return;

        // 波纹：三圈从旋钮边缘（r 106）向外扩到 r 236，前 20 % 淡入、之后淡出。
        {
            const auto ringOpacity = 0.1f + 0.45f * glaze;

            for (const auto begin : ringBegins)
            {
                const auto fraction = (float) (std::fmod (clock - begin, ringDuration) / ringDuration);
                const auto radius = 106.0f + 130.0f * fraction;
                const auto alpha = (fraction < 0.2f ? fraction / 0.2f : 1.0f - (fraction - 0.2f) / 0.8f) * ringOpacity * brightness;

                if (alpha <= 0.002f)
                    continue;

                g.setColour (juce::Colour (0xff1799ab).withAlpha (alpha));
                g.drawEllipse (juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre ({ centreX, centreY }), 1.2f);
            }
        }

        // 粒子
        const auto count = juce::roundToInt ((float) particlesPerSide * std::pow (glaze, 0.8f));

        for (int side = 0; side < 2; ++side)
        {
            const auto* ease = side == 0 ? easeLeft : easeRight;

            for (int k = 0; k < juce::jmin (count, (int) particles[(size_t) side].size()); ++k)
            {
                const auto& p = particles[(size_t) side][(size_t) k];
                const auto fraction = (float) (std::fmod (clock - (double) p.offset, (double) p.duration) / (double) p.duration);
                const auto envelope = fraction < 0.15f ? fraction / 0.15f : (fraction < 0.8f ? 1.0f : 1.0f - (fraction - 0.8f) / 0.2f);
                const auto alpha = juce::jlimit (0.0f, 1.0f, p.opacity * envelope * brightness);

                if (alpha <= 0.004f)
                    continue;

                const auto position = positionOn (lines[(size_t) side][(size_t) p.line], cubicBezier (ease, fraction));

                if (p.soft)
                {
                    // 模糊的大颗粒（设计稿 feGaussianBlur 1.6）：径向渐变代替模糊。
                    const auto outer = p.radius + 3.2f;
                    juce::ColourGradient blob (p.colour.withAlpha (alpha), position, p.colour.withAlpha (0.0f), position.translated (outer, 0.0f), true);
                    blob.addColour ((double) (p.radius / outer) * 0.6, p.colour.withAlpha (alpha * 0.85f));
                    g.setGradientFill (blob);
                    g.fillEllipse (juce::Rectangle<float> (2.0f * outer, 2.0f * outer).withCentre (position));
                }
                else
                {
                    g.setColour (p.colour.withAlpha (alpha));
                    g.fillEllipse (juce::Rectangle<float> (2.0f * p.radius, 2.0f * p.radius).withCentre (position));
                }
            }
        }
    }
}
