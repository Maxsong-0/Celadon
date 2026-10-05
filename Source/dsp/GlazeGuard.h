/*
  ==============================================================================

    GlazeGuard.h
    GLAZE 保护层（设计规格-GLAZE-决策与校准 第 7 节）：慢速退让，不是全部安全保证。

      G_实际 = G_参考 + g_保护，g_保护 ∈ [guardMinDb, 0]（出厂 −3 dB）。
      - 任一约束在 3 s 窗内持续超出 ≥ 1 s → 以 1 dB/s 退让；
      - 连续 ≥ 5 s 有效、安全的音乐 → 以 0.3 dB/s 恢复；恢复中再接近上限（留回差）就停止恢复；
      - 静音、停播不计入恢复时间；
      - 退到下限仍超约束 → isPlanLimited()（「方案受限」）；最终真峰值由限幅器与 Ceiling 硬保证，不归这里。
    速率全部按样本数折算（块长会变）。学习、过渡、校准期间同样生效。

    实现要点（v0.6 A3；数值与理由见 GlazeGuard.cpp 开头）：
      - 时间轴 = 有效音乐的样本数：stats.musicActive 为 false 的块（静音、停播）整块不计——窗口、超出计时、恢复计时、
        退让与恢复都停住，g_保护不变。
      - 按 10 ms 音乐时隙记账（固定容量环形缓冲，prepare 之后不分配）：大块摊到多个时隙、小块并进同一时隙，
        判定与退让 / 恢复都在时隙结束时做，所以同一段统计按任意块长切分，g_保护的轨迹都相同。
      - 各约束的量（上限来自 setLimits 的混合后常数；上限 ≤ 0 表示该项不设约束）：
          Glue 均值 / Limit 均值 / Clip 活动占比：3 s 窗内按样本平均；
          Glue p95 / Limit p95：3 s 窗内各时隙均值的 95 百分位（近似，误差见 .cpp）；
          Limit 峰值 / Clip 深度：最近 100 ms 的最大值（单个尖峰只算 100 ms「超出」，3 s 内累计满 1 s 才退让）。
      - 「持续超出」= 3 s 窗内被判为超出的时隙累计 ≥ 1 s；此后只要当前仍超出就按 1 dB/s 退让，一不超出就停。
      - 窗口里 g_保护与当前相差 > 0.5 dB 的旧时隙不再参与统计（它们描述的是已经不存在的工作点），
        退让 / 恢复时统计只看最近、有代表性的数据，不会因 3 s 窗的滞后多退。

    线程：全部在音频线程调用（编排器 endBlock 里）；无锁、不分配。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <memory>

namespace celadon::glaze
{
    class GlazeGuard
    {
    public:
        // Chain must end a processing slice here: a host-wide mean/max cannot
        // reconstruct the short peaks inside separate 10 ms statistics slots.
        int getSamplesUntilSlotBoundary() const noexcept { return slotLength - partSamples; }
        // A frozen gate ends the unfinished statistics fragment. Retain all
        // complete history and protection; do not let a 1-sample remainder
        // force thousands of DSP calls during silence/bypass.
        void discardPartialSlot() noexcept
        {
            partGlueSum = partLimitSum = partClipSum = 0.0;
            partLimitPeak = partClipDepth = 0.0f;
            partSamples = 0;
        }
        void prepare (double sampleRate) noexcept;
        void clear() noexcept;                         // 冷启动：g_保护回 0，窗口清空。宿主 reset 不调
        void reset() noexcept { clear(); }             // 与 clear 相同。编排器的 hostReset 不调它
        void releaseOutput() noexcept;                 // 只把已经给出的退让归零，统计窗留着（GLAZE ≤ 0 的这一块）

        // 本方案生效的约束（风格常数按权重混合后的上限）。可以每块调：上限没变时什么都不做；
        // 变了只重算窗口里的超限计数（历史保留，g_保护不变）。constants 里的保护层时长 / 速率 / 下限同时生效。
        // 调用之前保护层不做任何判定（g_保护保持 0）。
        void setLimits (const StyleConstants& blendedLimits, const Constants& constants) noexcept;

        // 每块调用：stats 是本块模块统计。退让在 reliefDb（≤ 0），原因是预算。
        GuardOutput process (const ModuleStats& stats) noexcept;

        // 消息线程，不与 process 同时。
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

        float getProtectionDb() const noexcept;
        bool isRetreating() const noexcept;   // 本块在退让（校准据此作废子窗）
        bool isPlanLimited() const noexcept;  // 已到下限仍超约束

        // ---- v0.6 A3 追加 ----

        bool isRecovering() const noexcept;   // 本块在恢复

        // 保护层不在参考状态：g_保护 ≠ 0，或本块刚改过它（退让或恢复）。这时测到的输出响度不是参考方案的，
        // 校准器不能采集——编排器把它作为 GlazeCalibrator::process 的 guardActive（只传 isRetreating 不够：
        // g_保护停在 −1 dB 时校准器会把这 1 dB 当成偏差补回去，与保护层互相顶）。
        bool isEngaged() const noexcept;

        // ---- T-032 R5-02 追加（只给编排器的测试探针读）----

        // 此刻生效的约束上限（最近一次 setLimits 交来的混合后常数）与「已经设过上限」标志。
        const StyleConstants& getLimits() const noexcept   { return limits; }
        bool hasLimits() const noexcept                    { return limitsSet; }

        // ---- T-032 R6-02 收尾（A11）----

        // 约束的口径变了（编排器因手动单独超预算把某几项放开或收回，GlazeController.cpp updateGuardLimits）：窗口里已完成的时隙是按
        // 另一套口径攒的——手动 Glue 从超预算拧回上限以内那一刻，旧时隙里手动造成的大 GR 按新上限一判就是「持续超出」，会白白退让
        // 自动 Gain。清掉已完成时隙的统计窗、3 s 超出计时与 p95 计数，按新口径从头判定。不动的：g_保护（已执行的退让照常按规则恢复，
        // 净增益不跳）、连续安全时隙数、「方案受限」、正在累计的那一格（时隙网格不变，链按它切片）。上限本身照旧（setLimits 管）。
        void clearStatistics() noexcept;

    private:
        // 10 ms 音乐时隙（环形缓冲的一格）。
        struct Slot
        {
            float glueMeanDb = 0.0f;     // 时隙内按样本平均
            float limitMeanDb = 0.0f;
            float clipFraction = 0.0f;
            float limitPeakDb = 0.0f;    // 时隙内最大
            float clipDepthDb = 0.0f;
            float protectionDb = 0.0f;   // 这一格判定之前、按时隙推进的 g_保护（代表性比较用，见 .cpp completeSlot）
            bool over = false;           // 该时隙结束时判为「超出」
        };

        // 当前所有约束的判定：over = 有一项超出上限；near = 有一项进入回差区（恢复要停）。
        struct Verdict
        {
            bool over = false;
            bool near = false;
        };

        static constexpr int slotCapacity = 1024;   // ≥ 窗口时隙数 + 1（出厂 3 s = 300 格；可容纳 10 s 的窗）

        GuardOutput budgetOutput() const noexcept;
        void updateTiming() noexcept;
        void clearWindows() noexcept;
        void completeSlot() noexcept;
        void addToStats (const Slot& slot) noexcept;
        void removeFromStats (const Slot& slot) noexcept;
        void recountNearOver() noexcept;
        void recomputeSums() noexcept;
        Verdict evaluate() const noexcept;
        int indexFromNewest (int age) const noexcept;   // age 0 = 最新的时隙

        double sampleRate = 48000.0;
        StyleConstants limits;
        Constants constants;
        bool limitsSet = false;

        float protectionDb = 0.0f;    // 返回给编排器的 g_保护（= protection 取 float）
        double protection = 0.0;      // 内部累计值（double：逐时隙累加不漂）
        float minimumDb = -3.0f;      // guardMinDb（钳到 ≤ 0）
        bool retreating = false;
        bool recovering = false;
        bool changedThisBlock = false;
        bool planLimited = false;

        // 时长折算（updateTiming）
        int slotLength = 480;         // 10 ms 的样本数
        int windowSlots = 300;        // 3 s
        int triggerSlots = 100;       // 1 s
        int recoverSlots = 500;       // 5 s
        int peakHoldSlots = 10;       // 100 ms
        double retreatStepDb = 0.01;  // 每时隙退让量
        double recoverStepDb = 0.003; // 每时隙恢复量

        // 正在累计的时隙
        double partGlueSum = 0.0, partLimitSum = 0.0, partClipSum = 0.0;
        float partLimitPeak = 0.0f, partClipDepth = 0.0f;
        int partSamples = 0;

        // 环形缓冲：最新 historyCount 格是 3 s 历史（超出计时用），其中最新 statsCount 格是统计窗（均值 / p95 / 占比用）。
        std::array<Slot, (std::size_t) slotCapacity> slots {};
        int writeIndex = 0;           // 下一个写入位置
        int historyCount = 0;
        int statsCount = 0;
        int overSlotsInHistory = 0;
        int slotsSinceRecompute = 0;

        // 统计窗的累计量
        double sumGlue = 0.0, sumLimit = 0.0, sumClip = 0.0;
        int glueAboveCap = 0, glueAboveNear = 0;     // 时隙均值超过 p95 上限 / 回差线的格数
        int limitAboveCap = 0, limitAboveNear = 0;

        int safeSlots = 0;            // 连续安全的音乐时隙数
    };
}
