/*
  ==============================================================================

    DialMotion.cpp

  ==============================================================================
*/

#include "DialMotion.h"

#include <algorithm>

namespace celadon::ui::dial
{
    namespace
    {
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        // 原版按「度」判 0.01：短于这个角长的彗尾段不画。
        constexpr float minimumSegmentAngle = 0.01f * juce::MathConstants<float>::pi / 180.0f;

        // 弹簧推进的最大步长：显式积分步子小，回弹的形状才与解析解一致（60 帧一帧拆成 10 步）。
        constexpr double maxSpringStep = 1.0 / 600.0;
    }

    double springDamping (float bounce) noexcept
    {
        return 2.0 * std::sqrt (springStiffness) * (1.0 - (double) juce::jlimit (0.0f, 0.9f, bounce));
    }

    float angleFor (float proportion) noexcept
    {
        return startAngle + juce::jlimit (0.0f, 1.0f, proportion) * sweep;
    }

    float dragThreshold (float trackRadius) noexcept
    {
        return juce::jmax (minimumDragPixels, dragThresholdPixels * trackRadius / originalRadius);
    }

    float wrappedAngleDelta (float from, float to) noexcept
    {
        auto delta = std::remainder (to - from, twoPi);   // [−π, π]

        if (delta <= -juce::MathConstants<float>::pi)
            delta += twoPi;

        return delta;
    }

    float proportionAtOffset (float dx, float dy, Side& side) noexcept
    {
        // atan2 (dx, −dy)：0 = 12 点、顺时针为正（屏幕 y 向下）。rel = 从轨道起点顺时针量过去的角度，0 ～ 2π。
        const auto angle = std::atan2 (dx, -dy);
        const auto rel = std::fmod (angle - startAngle + 2.0f * twoPi, twoPi);
        const auto gap = twoPi - sweep;

        if (rel > sweep)
        {
            if (side == Side::high)
                return 1.0f;

            if (side == Side::low)
                return 0.0f;

            side = rel < sweep + 0.5f * gap ? Side::high : Side::low;
            return side == Side::high ? 1.0f : 0.0f;
        }

        side = rel > 0.5f * sweep ? Side::high : Side::low;
        return rel / sweep;
    }

    //==============================================================================
    void Reading::jump (float proportion) noexcept
    {
        position = target = proportion;
        velocity = 0.0f;
        animating = false;
    }

    void Reading::launch (float newTarget, float bounce, float initialVelocity) noexcept
    {
        target = newTarget;
        velocity = std::isfinite (initialVelocity) ? initialVelocity : 0.0f;
        damping = springDamping (bounce);
        animating = true;
    }

    void Reading::follow (float newTarget, double seconds) noexcept
    {
        animating = false;
        target = newTarget;

        if (! (seconds > 0.0))
            return;

        const auto before = position;
        position += (newTarget - position) * (float) (1.0 - std::exp (-seconds / followSeconds));

        // 追到这么近就落在目标上：按住不动时位置不再每帧挪一点点（主旋钮才不会每帧重新合成）。
        if (std::abs ((double) newTarget - (double) position) < settleDistance)
            position = newTarget;

        velocity = (float) ((double) (position - before) / seconds);
    }

    void Reading::advance (double seconds) noexcept
    {
        if (! animating || ! (seconds > 0.0))
            return;

        // 以目标为原点：x'' = −k x − c x'（质量 1），半隐式欧拉。
        const auto steps = juce::jmax (1, (int) std::ceil (seconds / maxSpringStep));
        const auto h = seconds / (double) steps;
        auto x = (double) position - (double) target;
        auto v = (double) velocity;

        for (int i = 0; i < steps; ++i)
        {
            v += (-springStiffness * x - damping * v) * h;
            x += v * h;
        }

        // 停稳时精确落在目标上：静止画面与没有弹簧时逐像素相同。
        if (std::abs (x) < settleDistance && std::abs (v) < settleSpeed)
        {
            position = target;
            velocity = 0.0f;
            animating = false;
            return;
        }

        position = (float) ((double) target + x);
        velocity = (float) v;
    }

    //==============================================================================
    void Comet::update (float readingPosition, double seconds) noexcept
    {
        const auto raw = previous.has_value() && seconds > 0.0 ? (double) (readingPosition - *previous) / seconds : 0.0;
        previous = readingPosition;
        smoothed += (float) ((raw - (double) smoothed) * (1.0 - std::exp (-juce::jmax (0.0, seconds) / speedSmoothSeconds)));
    }

    float Comet::getStrength() const noexcept
    {
        return juce::jlimit (0.0f, 1.0f, std::abs (smoothed) / fullSpeed);
    }

    int cometSegments (float headAngle, float strength, int direction, CometSegments& out, float reach) noexcept
    {
        const auto segment = reach * juce::jlimit (0.0f, 1.0f, strength) / (float) cometSegmentCount;
        int count = 0;

        if (segment < minimumSegmentAngle)
            return 0;

        for (int j = 0; j < cometSegmentCount; ++j)
        {
            // 尾巴拖在运动方向的反面：往大走（顺时针）时在指示点之前的角度上。
            auto from = direction > 0 ? headAngle - (float) (j + 1) * segment : headAngle + (float) j * segment;
            auto to   = direction > 0 ? headAngle - (float) j * segment       : headAngle + (float) (j + 1) * segment;
            from = juce::jlimit (startAngle, endAngle, from);
            to   = juce::jlimit (startAngle, endAngle, to);

            if (to - from < minimumSegmentAngle)
                continue;

            out[(size_t) count++] = { from, to, 1.0f - (float) j / (float) cometSegmentCount };
        }

        return count;
    }

    //==============================================================================
    void FlickTracker::add (double timeMs, float proportion) noexcept
    {
        if (count == (int) samples.size())
        {
            std::move (samples.begin() + 1, samples.end(), samples.begin());
            --count;
        }

        samples[(size_t) count++] = { timeMs, proportion };
    }

    float FlickTracker::releaseVelocity (double releaseTimeMs) const noexcept
    {
        if (count < 2)
            return 0.0f;

        const auto& first = samples[0];
        const auto& last = samples[(size_t) count - 1];

        if (releaseTimeMs - last.timeMs > staleReleaseMs)
            return 0.0f;

        return (float) ((double) (last.proportion - first.proportion) / juce::jmax (1.0, last.timeMs - first.timeMs) * 1000.0);
    }

    float momentumTarget (float at, float velocity) noexcept
    {
        return at + (float) ((double) velocity / 1000.0 * (deceleration / (1.0 - deceleration))) * momentum;
    }

    float releaseBounce (float velocity) noexcept
    {
        return tapBounce + (flickBounce - tapBounce) * juce::jlimit (0.0f, 1.0f, std::abs (velocity) / flickSpeed);
    }
}
