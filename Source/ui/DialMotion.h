/*
  ==============================================================================

    DialMotion.h
    旋钮的「彗尾 + 弹簧读数 + 外圈手感」纯计算部分（T-100，ADR-037）：照 React Bits 的 CometDial（JS + CSS 版）移植，
    常数与公式逐条对应原版（注释里写原版的名字）；只依赖 juce_core，无头测试能编（DialMotionTests）。

      - 读数（Reading）：画出来的位置，追着参数值跑。点外圈、外部改值（宿主自动化、预设、撤销、输入、双击回默认）走弹簧，
        拖动时按 60 ms 时间常数跟着手，停稳时精确落在目标上（静止画面与改前逐像素相同靠这一条）；
      - 彗尾（Comet）：读数的速度平滑后换成 0 ～ 1 的强度，指示点身后（逆运动方向）画 12 段渐隐渐细的弧；
      - 外圈手感：按到的角度换成比例，拖进底部缺口时锁在进缺口前那一端，不会从最大绕回 0；
        松手前 80 ms 内还在动就按速度多滑一段（惯性），回弹系数在点按（0.2）与满速甩（0.1）之间插值。

    单位：位置用参数的 0 ～ 1 比例（原版除以量程后的口径），速度用「量程 / 秒」，时间用秒（甩动历史用毫秒，与原版一致）。
    角度约定同 Controls.h 的 knob：0 = 12 点方向、顺时针为正，270° 轨道从 −135° 到 +135°。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <optional>

namespace celadon::ui::dial
{
    //==============================================================================
    // 几何：270° 轨道（V4 设计稿；原版是 320°，这里保留面板的弧长）。
    inline constexpr float startAngle = -0.75f * juce::MathConstants<float>::pi;
    inline constexpr float endAngle   =  0.75f * juce::MathConstants<float>::pi;
    inline constexpr float sweep      = endAngle - startAngle;

    //==============================================================================
    // 手感常数（原版 CometDial 的默认值；调手感只改这里）。
    inline constexpr int    cometSegmentCount   = 12;      // K：彗尾分几段
    inline constexpr float  fullSpeed           = 4.0f;    // V_FULL：量程 / 秒，彗尾到满长满宽
    inline constexpr float  flickSpeed          = 2.0f;    // V_FLICK：松手速度到这里回弹系数取 flickBounce
    inline constexpr double followSeconds       = 0.06;    // TAU_FOLLOW：拖动时读数追手的时间常数
    inline constexpr double speedSmoothSeconds  = 0.05;    // TAU_V：彗尾用的速度平滑时间常数
    inline constexpr double deceleration        = 0.99;    // DECEL：惯性滑行的衰减（每毫秒）
    inline constexpr float  originalRadius      = 80.0f;   // R：原版旋钮的半径（DRAG_PX、DEAD 都是按它定的）
    inline constexpr float  dragThresholdPixels = 10.0f;   // DRAG_PX：外圈按下后移动超过它才算拖（之前是点按）；原版在 R 80 上，约 7°
    inline constexpr float  minimumDragPixels   = 4.0f;    // 按半径换算出来的门槛最少这么多：手点一下的抖动不算拖
    inline constexpr double staleReleaseMs      = 80.0;    // STALE_MS：最后一次移动距松手超过它就不算甩
    inline constexpr float  responsiveness      = 25.0f;   // speed（0 ～ 100）：弹簧多快到位
    inline constexpr float  tapBounce           = 0.2f;    // 点按 / 外部改值后的回弹
    inline constexpr float  flickBounce         = 0.1f;    // 满速甩之后的回弹
    inline constexpr float  momentum            = 1.0f;    // 甩出去能多滑多远（0 = 松手处停下）
    inline constexpr float  cometReach          = juce::MathConstants<float>::pi;   // cometReach 180°：满速时尾巴的角长
    inline constexpr float  deadZoneRatio       = 0.55f;   // DEAD 44 / R 80：中间多大一块不算外圈
    inline constexpr double maxFrameSeconds     = 0.05;    // 一帧最多推进这么久（卡顿后不一下子跳完）
    inline constexpr float  restingSpeed        = 0.002f;  // 平滑后的速度低于它（量程 / 秒）算停下

    // 弹簧：stiffness = 200 + speed × 7（speed 25 → 375），质量 1；阻尼 = 临界阻尼 ×（1 − bounce），bounce 最多 0.9。
    inline constexpr double springStiffness = 200.0 + (double) responsiveness / 100.0 * 700.0;
    double springDamping (float bounce) noexcept;

    // 弹簧停稳的判据（原版 motion 的 restDelta 0.01 / restSpeed 0.1 在 0 ～ 100 量程上，换成比例）。
    inline constexpr double settleDistance = 1.0e-4;
    inline constexpr double settleSpeed    = 1.0e-3;

    //==============================================================================
    float angleFor (float proportion) noexcept;          // 比例 → 角度（先钳到 0 ～ 1）

    // 外圈按下后移动多少像素才算拖：按原版的角度（R 80 上 10 px）换算到这颗旋钮的轨道半径，最少 minimumDragPixels。
    // 固定 10 px 在半径 23 的小旋钮上约 25°（轨道的 9 %），慢慢拖时前四分之一圈没反应。
    float dragThreshold (float trackRadius) noexcept;
    float wrappedAngleDelta (float from, float to) noexcept;   // to − from 折到 (−π, π]

    // 拖进底部缺口时锁哪一端：none = 还没落过（按下就在缺口里：取近的一端，并锁在这一端）。
    enum class Side { none, low, high };

    // 相对圆心的偏移（屏幕坐标，y 向下）→ 比例 0 ～ 1（原版 angleAt）。在弧上时顺带记下落在前半还是后半（side），
    // 之后拖进缺口就锁在那一端；还没记过（按下就在缺口里）时取缺口里近的一端并记下，之后在缺口里拖过正下方也不翻到另一端。
    float proportionAtOffset (float dx, float dy, Side& side) noexcept;

    //==============================================================================
    // 读数：画出来的位置。jump = 直接落下；launch = 从当前位置按弹簧去 target（velocity：量程 / 秒）；
    // follow = 拖动时按 followSeconds 追手（同时记下速度，松手后接弹簧用）；advance = 弹簧推进 seconds 秒。
    class Reading
    {
    public:
        void jump (float proportion) noexcept;
        void launch (float newTarget, float bounce, float initialVelocity) noexcept;
        void launch (float newTarget, float bounce) noexcept     { launch (newTarget, bounce, velocity); }
        void follow (float newTarget, double seconds) noexcept;
        void advance (double seconds) noexcept;

        float getPosition() const noexcept      { return position; }
        float getVelocity() const noexcept      { return velocity; }
        float getTarget() const noexcept        { return target; }
        bool isAnimating() const noexcept       { return animating; }

    private:
        float position = 0.0f, velocity = 0.0f, target = 0.0f;
        double damping = 0.0;
        bool animating = false;
    };

    //==============================================================================
    // 彗尾：读数的速度（量程 / 秒）按 speedSmoothSeconds 平滑；强度 = |速度| / fullSpeed（钳到 1）。
    // restart = 动画从静止重新开始（原版 wake 把上一帧位置清空：这一帧不算速度）。
    class Comet
    {
    public:
        void restart() noexcept                 { previous.reset(); }
        void clear() noexcept                   { previous.reset(); smoothed = 0.0f; }
        void update (float readingPosition, double seconds) noexcept;

        float getSpeed() const noexcept         { return smoothed; }
        float getStrength() const noexcept;
        int getDirection() const noexcept       { return smoothed < 0.0f ? -1 : 1; }
        bool isResting() const noexcept         { return std::abs (smoothed) <= restingSpeed; }

    private:
        std::optional<float> previous;
        float smoothed = 0.0f;
    };

    // 彗尾的一段：从 fromAngle 到 toAngle（fromAngle < toAngle），weight = 1 − j / K（头部 1，越往后越小）。
    // 画的时候：线宽 = 轨道宽 + 额外宽 × weight × 强度，不透明度 = weight × 强度。
    struct CometSegment
    {
        float fromAngle = 0.0f, toAngle = 0.0f, weight = 0.0f;
    };

    using CometSegments = std::array<CometSegment, (size_t) cometSegmentCount>;

    // 指示点在 headAngle、强度 strength、方向 direction（+1 = 往大走）：尾巴拖在身后，钳在轨道两端之间。返回画几段。
    int cometSegments (float headAngle, float strength, int direction, CometSegments& out, float reach = cometReach) noexcept;

    //==============================================================================
    // 甩动：拖动时记最近 4 个（时间毫秒，位置）；松手时算速度（量程 / 秒）。
    class FlickTracker
    {
    public:
        void reset() noexcept                   { count = 0; }
        void add (double timeMs, float proportion) noexcept;

        // 最后一次移动距 releaseTimeMs 超过 staleReleaseMs，或不到两个点：0（停住再松手不甩）。
        float releaseVelocity (double releaseTimeMs) const noexcept;

    private:
        struct Sample { double timeMs; float proportion; };
        std::array<Sample, 4> samples {};
        int count = 0;
    };

    // 惯性落点：at + 速度 × (DECEL / (1 − DECEL)) / 1000 × momentum（原版公式；没钳到 0 ～ 1）。
    float momentumTarget (float at, float velocity) noexcept;

    // 松手后的回弹：点按 0.2 → 满速（flickSpeed）甩 0.1 线性插值。
    float releaseBounce (float velocity) noexcept;
}
