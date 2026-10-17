/*
  ==============================================================================

    DialMotionTests.cpp
    旋钮「彗尾 + 弹簧读数 + 外圈手感」纯计算部分（T-100，Source/ui/DialMotion）的自动测试：
    角度换算与缺口锁边、弹簧回弹与停稳落点、拖动跟随、彗尾强度与几何、甩动速度与惯性落点。
    数值都按原版 CometDial 的公式手算对照。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/ui/DialMotion.h"

#include <cmath>

namespace celadon::test
{
    namespace
    {
        using namespace celadon::ui;

        constexpr float pi = juce::MathConstants<float>::pi;

        // 角度 angle（0 = 12 点、顺时针）方向上、离圆心 radius 的偏移（屏幕坐标，y 向下）。
        float offsetX (float angle, float radius = 50.0f)   { return radius * std::sin (angle); }
        float offsetY (float angle, float radius = 50.0f)   { return -radius * std::cos (angle); }

        float proportionAtAngle (float angle, dial::Side& side)
        {
            return dial::proportionAtOffset (offsetX (angle), offsetY (angle), side);
        }

        // 按 60 帧推进弹簧直到停稳（最多 seconds 秒）；返回推进过程中位置的最大值。
        float runSpring (dial::Reading& reading, double seconds, double frame = 1.0 / 60.0)
        {
            auto peak = reading.getPosition();

            for (double t = 0.0; t < seconds && reading.isAnimating(); t += frame)
            {
                reading.advance (frame);
                peak = juce::jmax (peak, reading.getPosition());
            }

            return peak;
        }
    }

    class DialMotionTests  : public juce::UnitTest
    {
    public:
        DialMotionTests() : juce::UnitTest ("DialMotion", "Celadon") {}

        void runTest() override
        {
            beginTest ("angle -> proportion on the 270 degree track (12 o'clock is the middle)");
            {
                auto side = dial::Side::none;
                expectWithinAbsoluteError (proportionAtAngle (0.0f, side), 0.5f, 1.0e-5f);
                expect (side != dial::Side::none, "a press on the track picks a side");

                side = dial::Side::none;
                expectWithinAbsoluteError (proportionAtAngle (dial::startAngle + 1.0e-4f, side), 0.0f, 1.0e-4f);
                side = dial::Side::none;
                expectWithinAbsoluteError (proportionAtAngle (dial::endAngle - 1.0e-4f, side), 1.0f, 1.0e-4f);
                expect (side == dial::Side::high);

                side = dial::Side::none;
                expectWithinAbsoluteError (proportionAtAngle (0.5f * pi, side), 5.0f / 6.0f, 1.0e-5f, "3 o'clock = 225 of 270 degrees");

                // 角度换回比例，比例换回角度：一致。
                for (const auto p : { 0.05f, 0.25f, 0.5f, 0.77f, 0.99f })
                {
                    side = dial::Side::none;
                    expectWithinAbsoluteError (proportionAtAngle (dial::angleFor (p), side), p, 1.0e-5f);
                }

                expectEquals (dial::angleFor (-0.3f), dial::startAngle, "angleFor clamps below 0");
                expectEquals (dial::angleFor (1.7f), dial::endAngle, "angleFor clamps above 1");
            }

            beginTest ("the bottom gap: nearest end when pressed there, locked to the side you came from while dragging");
            {
                // 缺口正下方（π）偏左一点更近起点（7 点半），偏右一点更近终点（4 点半）。
                auto side = dial::Side::none;
                expectEquals (proportionAtAngle (pi + 0.1f, side), 0.0f, "just left of the bottom -> start");
                expect (side == dial::Side::low, "a press in the gap locks the nearer end");
                expectEquals (proportionAtAngle (pi - 0.1f, side), 0.0f, "then dragging past the bottom within the gap stays at the start");

                side = dial::Side::none;
                expectEquals (proportionAtAngle (pi - 0.1f, side), 1.0f, "just right of the bottom -> end");
                expect (side == dial::Side::high);
                expectEquals (proportionAtAngle (pi + 0.1f, side), 1.0f, "and stays at the end across the bottom");

                // 从缺口拖回轨道上：值跟着角度走，锁的一端也换成所在的那半边。
                expectWithinAbsoluteError (proportionAtAngle (dial::angleFor (0.1f), side), 0.1f, 1.0e-5f);
                expect (side == dial::Side::low);

                // 从 0.9 拖进缺口、一直拖到偏左（离起点更近）：仍锁在最大，不绕回 0。
                side = dial::Side::none;
                proportionAtAngle (dial::angleFor (0.9f), side);
                expect (side == dial::Side::high);
                expectEquals (proportionAtAngle (pi + 0.15f, side), 1.0f, "dragged past the end stays at the maximum");

                side = dial::Side::none;
                proportionAtAngle (dial::angleFor (0.1f), side);
                expect (side == dial::Side::low);
                expectEquals (proportionAtAngle (pi - 0.15f, side), 0.0f, "dragged past the start stays at the minimum");
            }

            beginTest ("drag threshold keeps the original's angle (10 px at radius 80), at least 4 px");
            {
                expectWithinAbsoluteError (dial::dragThreshold (dial::originalRadius), 10.0f, 1.0e-5f, "the original dial: 10 px");
                expectWithinAbsoluteError (dial::dragThreshold (102.0f), 12.75f, 1.0e-4f, "GLAZE ring (radius 102)");
                expectEquals (dial::dragThreshold (23.0f), dial::minimumDragPixels, "small knob (radius 23): 2.9 px -> floored at 4 px");
                expect (dial::dragThreshold (23.0f) / 23.0f < 0.2f, "the small knob no longer waits a quarter turn (10 px was about 25 degrees)");
            }

            beginTest ("wrapped angle delta takes the short way round");
            {
                expectWithinAbsoluteError (dial::wrappedAngleDelta (3.0f, -3.0f), 2.0f * pi - 6.0f, 1.0e-5f);
                expectWithinAbsoluteError (dial::wrappedAngleDelta (-3.0f, 3.0f), 6.0f - 2.0f * pi, 1.0e-5f);
                expectWithinAbsoluteError (dial::wrappedAngleDelta (0.2f, 0.5f), 0.3f, 1.0e-6f);
                expect (dial::wrappedAngleDelta (0.0f, pi) > 0.0f && dial::wrappedAngleDelta (0.0f, -pi) > 0.0f, "half a turn is +pi");
            }

            beginTest ("spring: stiffness 375, damping = critical x (1 - bounce), bounce capped at 0.9");
            {
                expectWithinAbsoluteError (dial::springStiffness, 375.0, 1.0e-9);
                expectWithinAbsoluteError (dial::springDamping (0.0f), 2.0 * std::sqrt (375.0), 1.0e-9);
                expectWithinAbsoluteError (dial::springDamping (0.2f), 0.8 * 2.0 * std::sqrt (375.0), 1.0e-6);
                expectWithinAbsoluteError (dial::springDamping (0.95f), 0.1 * 2.0 * std::sqrt (375.0), 1.0e-6);
            }

            beginTest ("tap spring rings past the target a little, then lands exactly on it");
            {
                dial::Reading reading;
                reading.jump (0.2f);
                reading.launch (0.8f, dial::tapBounce, 0.0f);
                expect (reading.isAnimating());

                const auto peak = runSpring (reading, 3.0);
                expect (! reading.isAnimating(), "settles within 3 s");
                expectEquals (reading.getPosition(), 0.8f, "lands exactly on the target");
                expectEquals (reading.getVelocity(), 0.0f);

                // 阻尼比 0.8：过冲 exp (−ζπ / √(1 − ζ²)) ≈ 1.5 %（0.6 的距离 ≈ 0.009）。
                const auto overshoot = (peak - 0.8f) / 0.6f;
                expect (overshoot > 0.01f && overshoot < 0.02f, "overshoot " + juce::String (overshoot, 4));
            }

            beginTest ("no bounce: no overshoot; settle time is about half a second");
            {
                dial::Reading reading;
                reading.jump (0.0f);
                reading.launch (1.0f, 0.0f, 0.0f);
                const auto peak = runSpring (reading, 3.0);
                expect (peak <= 1.0f + 1.0e-6f, "critically damped does not overshoot (" + juce::String (peak, 7) + ")");

                dial::Reading timed;
                timed.jump (0.0f);
                timed.launch (1.0f, dial::tapBounce, 0.0f);
                double elapsed = 0.0;

                while (timed.isAnimating() && elapsed < 3.0)
                {
                    timed.advance (1.0 / 60.0);
                    elapsed += 1.0 / 60.0;
                }

                expect (elapsed > 0.3 && elapsed < 1.0, "full-range tap settles in " + juce::String (elapsed, 3) + " s");
            }

            beginTest ("spring is frame-rate independent");
            {
                dial::Reading at60, at120;
                at60.jump (0.0f);
                at120.jump (0.0f);
                at60.launch (1.0f, dial::tapBounce, 0.0f);
                at120.launch (1.0f, dial::tapBounce, 0.0f);

                for (int i = 0; i < 12; ++i)
                    at60.advance (1.0 / 60.0);

                for (int i = 0; i < 24; ++i)
                    at120.advance (1.0 / 120.0);

                expectWithinAbsoluteError (at60.getPosition(), at120.getPosition(), 1.0e-3f);
            }

            beginTest ("launch keeps the reading's own velocity unless one is given");
            {
                dial::Reading reading;
                reading.jump (0.0f);
                reading.follow (1.0f, 0.06);
                expectWithinAbsoluteError (reading.getPosition(), 1.0f - std::exp (-1.0f), 1.0e-5f, "one time constant = 63 %");
                expectWithinAbsoluteError (reading.getVelocity(), (1.0f - std::exp (-1.0f)) / 0.06f, 1.0e-3f);
                expect (! reading.isAnimating(), "following is not the spring");

                const auto velocity = reading.getVelocity();
                reading.launch (0.5f, dial::tapBounce);
                expectEquals (reading.getVelocity(), velocity);
                reading.launch (0.5f, dial::tapBounce, -2.0f);
                expectEquals (reading.getVelocity(), -2.0f);
                reading.launch (0.5f, dial::tapBounce, std::numeric_limits<float>::quiet_NaN());
                expectEquals (reading.getVelocity(), 0.0f, "a non-finite velocity starts from rest");
                reading.follow (0.5f, 0.0);
                expect (! reading.isAnimating(), "follow stops the spring even with no time passing");
            }

            beginTest ("comet strength: smoothed speed / 4 ranges per second");
            {
                dial::Comet comet;
                expectEquals (comet.getStrength(), 0.0f);
                expect (comet.isResting());

                // 恒速 2 量程 / 秒走 0.5 s：平滑速度追到 ≈ 2，强度 ≈ 0.5。
                float position = 0.0f;
                comet.update (position, 1.0 / 60.0);
                expectEquals (comet.getStrength(), 0.0f, "the first frame after a restart has no speed");

                for (int i = 0; i < 30; ++i)
                {
                    position += 2.0f / 60.0f;
                    comet.update (position, 1.0 / 60.0);
                }

                expectWithinAbsoluteError (comet.getStrength(), 0.5f, 0.01f);
                expectEquals (comet.getDirection(), 1);

                // 反过来走得很快：方向变负、强度钳到 1。
                for (int i = 0; i < 30; ++i)
                {
                    position -= 10.0f / 60.0f;
                    comet.update (position, 1.0 / 60.0);
                }

                expectEquals (comet.getStrength(), 1.0f);
                expectEquals (comet.getDirection(), -1);

                // 停下：平滑速度按 50 ms 时间常数落，0.5 s 后低于「停下」阈值。
                for (int i = 0; i < 30; ++i)
                    comet.update (position, 1.0 / 60.0);

                expect (comet.isResting(), "speed after stopping " + juce::String (comet.getSpeed(), 6));

                comet.clear();
                expectEquals (comet.getSpeed(), 0.0f);
                comet.update (position + 0.5f, 1.0 / 60.0);
                expectEquals (comet.getSpeed(), 0.0f, "after clear the next frame is a fresh start, not a jump");
            }

            beginTest ("comet segments trail behind the head, fade and stay on the track");
            {
                dial::CometSegments segments;

                expectEquals (dial::cometSegments (dial::angleFor (0.5f), 0.0f, 1, segments), 0, "no strength, no tail");

                const auto head = dial::angleFor (0.8f);
                const auto count = dial::cometSegments (head, 1.0f, 1, segments);
                expectEquals (count, dial::cometSegmentCount);
                expectWithinAbsoluteError (segments[0].toAngle, head, 1.0e-6f, "moving up: the first segment ends at the head");
                expectWithinAbsoluteError (segments[(size_t) count - 1].fromAngle, head - dial::cometReach, 1.0e-5f, "full strength = 180 degrees");

                for (int j = 0; j < count; ++j)
                {
                    const auto& s = segments[(size_t) j];
                    expect (s.fromAngle < s.toAngle);
                    expectWithinAbsoluteError (s.weight, 1.0f - (float) j / 12.0f, 1.0e-6f);
                }

                // 往小走：尾巴在指示点之后（角度更大）；半强度 = 90°。
                const auto low = dial::angleFor (0.3f);
                const auto down = dial::cometSegments (low, 0.5f, -1, segments);
                expectEquals (down, dial::cometSegmentCount);
                expectWithinAbsoluteError (segments[0].fromAngle, low, 1.0e-6f, "moving down: the tail starts at the head");
                expectWithinAbsoluteError (segments[(size_t) down - 1].toAngle, low + 0.5f * dial::cometReach, 1.0e-5f);

                // 靠近终点往小走：尾巴撞到终点被截短（0.8 + 半强度：只剩 8 段）。
                const auto nearEnd = dial::cometSegments (head, 0.5f, -1, segments);
                expectEquals (nearEnd, 8);
                expectEquals (segments[(size_t) nearEnd - 1].toAngle, dial::endAngle, "clamped at the end of the track");

                // 靠近起点往大走：尾巴撞到起点被截短，不越出轨道。
                const auto clipped = dial::cometSegments (dial::angleFor (0.05f), 1.0f, 1, segments);
                expect (clipped > 0 && clipped < dial::cometSegmentCount, "clipped tail (" + juce::String (clipped) + " segments)");

                for (int j = 0; j < clipped; ++j)
                    expect (segments[(size_t) j].fromAngle >= dial::startAngle);
            }

            beginTest ("flick velocity from the last four moves; a pause before release is not a flick");
            {
                dial::FlickTracker flick;
                expectEquals (flick.releaseVelocity (0.0), 0.0f, "nothing recorded");
                flick.add (0.0, 0.5f);
                expectEquals (flick.releaseVelocity (1.0), 0.0f, "one sample is not enough");

                flick.reset();

                for (int i = 0; i < 6; ++i)
                    flick.add (10.0 * i, 0.1f + 0.02f * (float) i);   // 2 量程 / 秒

                // 只留最后 4 个：(20 ms, 0.14) … (50 ms, 0.20)。
                expectWithinAbsoluteError (flick.releaseVelocity (55.0), 2.0f, 1.0e-4f);
                expectWithinAbsoluteError (flick.releaseVelocity (50.0 + 80.0), 2.0f, 1.0e-4f, "80 ms is still a flick");
                expectEquals (flick.releaseVelocity (50.0 + 80.5), 0.0f, "more than 80 ms after the last move: no flick");

                // 同一毫秒里的几次移动：时间差按 1 ms 算（原版 Math.max (1, …)）。
                flick.reset();
                flick.add (100.0, 0.30f);
                flick.add (100.0, 0.31f);
                expectWithinAbsoluteError (flick.releaseVelocity (100.0), 10.0f, 1.0e-3f);
            }

            beginTest ("momentum target and release bounce follow the original formulas");
            {
                expectWithinAbsoluteError (dial::momentumTarget (0.5f, 2.0f), 0.5f + 2.0f * 0.099f, 1.0e-5f);
                expectWithinAbsoluteError (dial::momentumTarget (0.5f, -1.0f), 0.5f - 0.099f, 1.0e-5f);
                expectEquals (dial::momentumTarget (0.3f, 0.0f), 0.3f);

                expectWithinAbsoluteError (dial::releaseBounce (0.0f), 0.2f, 1.0e-6f);
                expectWithinAbsoluteError (dial::releaseBounce (1.0f), 0.15f, 1.0e-6f);
                expectWithinAbsoluteError (dial::releaseBounce (-2.0f), 0.1f, 1.0e-6f);
                expectWithinAbsoluteError (dial::releaseBounce (9.0f), 0.1f, 1.0e-6f);
            }
        }
    };

    static DialMotionTests dialMotionTests;
}
