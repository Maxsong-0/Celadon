/*
  ==============================================================================

    ContrastGuard.h
    T-048 阶段 2 落差守门（草图第 4 节，Q8 / Q9）；T-097 按规格重写（设计规格-实时GLAZE-主段与增益 第 4 节第 4 条）。
    音频线程、定长、不分配、不加锁。

    测点：入口（IN 表）与出口（OUT 表，监听之前的母带）同一个 100 ms 子块。编排器每读到一对子块就调一次 process。
    每侧一份 3 s 窗算短期响度 S，再进一份 S 直方图（0.1 LU 一格，−70 ～ +10 LUFS，同 ProfileMath 的直方图格；
    按有效音频遗忘，τ 180 s）。落差损失 loss = (P90 − P25)_入口 − (P90 − P25)_出口：限幅把 drop 压得比 breakdown 多，
    出口的分布就比入口窄，loss 约等于两类段落平均 GR 之差（报告 B），减增益就能还回来。

    每对子块一步（步长 = 子块时长 stepSeconds；有效音乐、两侧 S 都有效才推进，否则计时与退让都保持）：
      - 判定前提：两份直方图都满 60 s（遗忘后的累计时长），且入口 P90 − P25 ≥ 3 LU（有对比可保）；
      - loss > 门槛（标准 / 流媒体 1.2 LU，俱乐部 1.5 LU，Max 2026-10-01 / 10-02 ①）持续满 10 s → 以 0.1 dB/s 退，退到 floorDb 为止；
      - loss < 门槛 − 0.3 LU 持续满 30 s → 以 0.05 dB/s 回到 0；两者之间两边计时都清零、退让保持（回差）；
      - 前提不成立按「低于门槛」计回程（T-097 默认：没有对比可保，留着退让只会卡住）；
      - 本曲风关着落差守门（GlazeConstants guardSwitchesFor(…).contrast：House 三类开、通用关）：直方图照攒，计时清零，
        退让按回程速率放回 0（T-097 Max 定：切到关着的曲风不跳变）。
    T-097 之前：128 笔环（12.8 s）、满 8 笔即判、退与回同速，每块两次 std::sort，读 OUT 表的 getShortTermLufs（界面点 Reset 会清）。

    clear() 只用于冷启动；snapRelief() 给 GLAZE ≤ 0；clearWindows() 给冻结（旁通、听原声）期间，以及跳播、循环回绕、宿主 reset：
    断开两侧 3 s 窗，直方图、计时与退让不动。
    存档（CONTRAST_GUARD）：reliefDb / floorDb / engaged 同 T-048；overSeconds / underSeconds / histogramSeconds 与两份直方图是 T-097 的。
    读到没有直方图的旧节点：退让照旧，计时与直方图从空开始（不冷启动整份 LIVE）。写出时另附旧版本要的 128 笔环字段（全 0），
    0.1.8.10 降级打开新工程也能读回、退让照旧。3 s 窗不存（规格第 5 节）。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"
#include "LoudnessMeter.h"
#include "ProfileMath.h"

#include <juce_core/juce_core.h>

#include <array>
#include <memory>

namespace celadon::glaze
{
    class ContrastGuard
    {
    public:
        // stepSeconds = 一个子块的时长（round(0.1 × 采样率) ÷ 采样率）。冷启动。
        void prepare (double stepSeconds) noexcept;
        void clear() noexcept;
        void clearWindows() noexcept;
        void setFloorDb (float floorDb) noexcept;
        void snapRelief() noexcept;

        // T-097 Q15 A（Max 2026-10-11）：再往下退的界 = −自动加上去的全部（规格总钳位 g_守护 ≥ −max(0, 增量.gain + G_trim)），
        // 编排器每个子块交进来。退让只在比界浅时往下退、退到界为止；已比界更深（自动部分变小）时保持、不往回弹。
        // 默认不设界（−∞）；floorDb 只剩绝对安全下限（编排器设成 guardSafetyFloorDb，不再是 −3 dB）。
        void setRetreatBoundDb (float boundDb) noexcept   { retreatBoundDb = std::isfinite (boundDb) ? juce::jmin (0.0f, boundDb) : unboundedDb; }

        // 同一时刻的一对子块（入口、出口）。musicActive = 这一块是有效音乐；enabled = 此刻的曲风开着落差守门。
        GuardOutput process (const LoudnessMeter::SubBlock& input, const LoudnessMeter::SubBlock& output,
                             bool musicActive, bool enabled, bool clubDelivery) noexcept;

        // 此刻的退让（上一步之后；没推进的块里保持）。
        GuardOutput current() const noexcept;

        // 上一步的判定量（诊断与测试）。judged = 判定前提成立；lossLu 只在 judged 时有意义。
        struct Reading
        {
            bool judged = false;
            float inputSpreadLu = 0.0f;
            float outputSpreadLu = 0.0f;
            float lossLu = 0.0f;
            double histogramSeconds = 0.0;
        };

        Reading getLastReading() const noexcept   { return lastReading; }

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

        static constexpr double forgetSeconds = 180.0;        // 直方图按有效音频遗忘的时间常数 ③
        static constexpr double fullSeconds = 60.0;           // 两份直方图都满这么久才判定 ③
        static constexpr float minimumInputSpreadLu = 3.0f;   // 入口 P90 − P25 至少这么大才判定（有对比可保）③
        static constexpr float standardLossLu = 1.2f;         // 门槛 ①（Max 2026-10-02，T-045 后由 1 改 1.2）
        static constexpr float clubLossLu = 1.5f;             // 俱乐部档门槛 ①（Max 2026-10-01）
        static constexpr float releaseMarginLu = 0.3f;        // 回程要 loss < 门槛 − 本项 ③
        static constexpr double triggerSeconds = 10.0;        // 超门槛持续这么久才退 ③
        static constexpr double releaseAfterSeconds = 30.0;   // 低于门槛 − 0.3 持续这么久才回 ③
        static constexpr float retreatDbPerSecond = 0.1f;
        static constexpr float releaseDbPerSecond = 0.05f;
        static constexpr double lowPercentile = 0.25;
        static constexpr double highPercentile = 0.90;

    private:
        using Histogram = std::array<float, (size_t) histogramBins>;

        static double percentileLufs (const Histogram& histogram, double total, double fraction) noexcept;
        static double spreadLu (const Histogram& histogram) noexcept;
        void addToHistograms (double inputLufs, double outputLufs) noexcept;
        Reading judge() const noexcept;
        bool reached (double seconds, double threshold) const noexcept   { return seconds >= threshold - 0.5 * stepSeconds; }

        ShortTermWindow inputWindow, outputWindow;
        Histogram inputHistogram {}, outputHistogram {};
        double stepSeconds = 0.1;
        double decay = 1.0;              // exp(−stepSeconds ÷ forgetSeconds)
        double histogramSeconds = 0.0;   // 遗忘后的累计有效时长（两份直方图同步进账）
        float floorDb = -3.0f;
        static constexpr float unboundedDb = -1.0e30f;
        float retreatBoundDb = unboundedDb;
        float reliefDb = 0.0f;
        double overSeconds = 0.0;
        double underSeconds = 0.0;
        bool engaged = false;
        Reading lastReading;
    };
}
