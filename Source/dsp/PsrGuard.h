/*
  ==============================================================================

    PsrGuard.h
    T-048 阶段 2 PSR 守门（草图第 4 节，Q8：GuardCause 单列 psr）；T-097 按规格改成闭环
    （设计规格-实时GLAZE-主段与增益 第 4 节第 4 条）。音频线程、定长、不分配、不加锁。

    测点：出口（OUT 表，监听增益之前的母带）的 3 s PSR。编排器每读到 OUT 表的一个 100 ms 子块就调一次 process，
    守门自己的 3 s 窗（ShortTermWindow）跟着子块推进，所以同一串子块不论宿主怎么切块，退让轨迹都逐位相同。
    T-097 之前读的是链入口的 PSR：退增益改变不了被测量（开环），输入一密就一路退到底，输入松而出口被压扁时又看不见。

    每个子块一步（步长 = 子块时长 stepSeconds）：
      - 3 s PSR < 门槛（该档下限 − 0.5 dB）持续满 3 s → 以 0.5 dB/s 退，退到 max(floorDb, 界) 为止
        （界 = −自动加上去的全部，编排器每个子块用 setRetreatBoundDb 交进来；编排器给的 floorDb 只是 −24 的安全下限，Q15 A）；
      - 回程（规格没写，T-097 Max 2026-10-10 定，起点值）：PSR ≥ 门槛 + 0.5 dB 持续满 5 s → 以 0.15 dB/s 回到 0；
        门槛与门槛 + 0.5 dB 之间两边计时都清零、退让保持——闭环之后没有这道回差会在门槛附近来回摆；
      - 读数无效（窗不满 3 s、近乎静音）或非有效音乐：计时与退让都保持；
      - 本曲风关着 PSR 守门（GlazeConstants guardSwitchesFor(…).psr，出厂四个曲风都关）：有效音乐时计时清零、退让按回程速率放回 0
        （T-097 Max 定：切到关着的曲风不跳变）。
    下限按交付（floorFor）。曲风表里的下限（设计规格-实时GLAZE-曲风 第 2 节）与这张表的统一另议（ADR-034 第 6 条）。

    clear() 只用于冷启动；snapRelief() 给 GLAZE ≤ 0；clearWindow() 给冻结（旁通、听原声）期间（出口不是方案在听的信号），
    以及跳播、循环回绕、宿主 reset（同分析器的窗口一起断开）：3 s 窗断开重攒，计时与退让不动。
    存档：reliefDb / floorDb / belowSeconds / engaged 同 T-048，前两项按 0.1.8.10 的不变量写（floorDb 恒 −3、reliefDb 写
    max(−3, 退让)，见 ProfileMath.h legacyGuardSavedReliefDb：0.1.8.10 读档直接拿 floorDb 当底、从不另设，再存又原样写回）；
    floorDb 只校验、不采纳（下限是编排器 setFloorDb 的配置，读档中途失败冷启动时也不会停在 −3 上）。T-097 追加两个可选属性：
    aboveSeconds（旧档缺按 0 读）、reliefDbExact（真实退让，缺了按 reliefDb 读）；两项退让任一 > 0 当损坏拒收。3 s 窗不存
    （设计规格-实时GLAZE-主段与增益 第 5 节：窗口状态不存），读档后 3 s 内读数无效、守门保持不动。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"
#include "LoudnessMeter.h"
#include "ProfileMath.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace celadon::glaze
{
    class PsrGuard
    {
    public:
        // stepSeconds = OUT 表一个子块的时长（round(0.1 × 采样率) ÷ 采样率）。冷启动。
        void prepare (double stepSeconds) noexcept;
        void clear() noexcept;
        void clearWindow() noexcept;
        void setFloorDb (float floorDb) noexcept;
        void snapRelief() noexcept;

        // T-097 Q15 A（Max 2026-10-11）：再往下退的界 = −自动加上去的全部（规格总钳位 g_守护 ≥ −max(0, 增量.gain + G_trim)），
        // 编排器每个子块交进来。退让只在比界浅时往下退、退到界为止；已比界更深（自动部分变小）时保持、不往回弹。
        // 默认不设界（−∞）；floorDb 只剩绝对安全下限（编排器设成 guardSafetyFloorDb，不再是 −3 dB）。
        void setRetreatBoundDb (float boundDb) noexcept   { retreatBoundDb = std::isfinite (boundDb) ? juce::jmin (0.0f, boundDb) : unboundedDb; }

        // 一个出口子块。musicActive = 这一块是有效音乐；enabled = 此刻的曲风开着 PSR 守门。
        GuardOutput process (const LoudnessMeter::SubBlock& output, bool musicActive, bool enabled, Delivery delivery) noexcept;

        // 此刻的退让（上一步之后；没推进的块里保持）。
        GuardOutput current() const noexcept;

        // 上一步的出口 3 s PSR（诊断与测试）；窗不满或近乎静音时 false。
        bool getLastPsrDb (float& psrDb) const noexcept;
        float getFloorDb() const noexcept   { return floorDb; }

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

        // 出厂下限（dB）。「低于下限 − 0.5」才开始计 3 s。
        static float floorFor (Delivery delivery) noexcept;
        static float tripFor (Delivery delivery) noexcept   { return floorFor (delivery) - marginDb; }

        static constexpr float marginDb = 0.5f;
        static constexpr float retreatDbPerSecond = 0.5f;
        static constexpr double sustainSeconds = 3.0;
        static constexpr float releaseHysteresisDb = 0.5f;     // T-097：回程要 PSR ≥ 门槛 + 本项
        static constexpr double releaseAfterSeconds = 5.0;     // T-097：并持续这么久（同预算环「安全 5 s 后回」）
        static constexpr float releaseDbPerSecond = 0.15f;

    private:
        // 计时用秒累加，比较时留半步余量：0.1 累加 30 次不一定正好是 3.0。
        bool reached (double seconds, double threshold) const noexcept   { return seconds >= threshold - 0.5 * stepSeconds; }

        ShortTermWindow window;
        double stepSeconds = 0.1;
        float floorDb = legacyGuardFloorDb;   // 没设过下限时的默认值（编排器 prepare / 读档时设成 guardSafetyFloorDb）
        static constexpr float unboundedDb = -1.0e30f;
        float retreatBoundDb = unboundedDb;
        float reliefDb = 0.0f;
        double belowSeconds = 0.0;
        double aboveSeconds = 0.0;
        bool engaged = false;
        bool lastPsrValid = false;
        float lastPsrDb = 0.0f;
    };
}
