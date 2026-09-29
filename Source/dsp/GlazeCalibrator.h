/*
  ==============================================================================

    GlazeCalibrator.h
    GLAZE 校准器（设计规格-GLAZE-决策与校准 第 6 节）：有结束条件，不做整首歌的持续闭环。

    每轮：方案过渡结束 → 等检测器覆盖稳定方案（settle + 链延迟）→ 固定方案下分子窗采集 →
          算一次修正 e = 目标净提升 − (L_out − L_in) → 交给编排器平滑施加。
    输入输出响度用两块表的 100 ms 子块能量（带样本时间戳），按链延迟对齐，块选择口径两侧一致。

      - 任何中断（手动改相关旋钮、跳播、Listen / 旁通、保护层改了实际增益）→ 当前子窗作废，已完成且条件一致的子窗保留；
      - 第二轮主要验证第一轮；偏差仍大或子窗方向相反 → 结束为 partial（保守、未完全校准）；
      - 保护层持续触发 → 结束为 budgetLimited；
      - 防积分饱和按方向：到上限只禁止继续往上，仍允许往下；回馈用实际执行的控制量。

    实现约定（v0.6 A3；数值与理由见 GlazeCalibrator.cpp 开头）：
      - 时间全按两块表的子块时间戳（样本数）算，与宿主块长无关。
      - 中断的「屏障」：发生中断的那次 process 送来的入口子块全部不用，之后送来的第一个入口子块（可能跨着中断点）也不用；
        再往后、开始时刻 ≥ 屏障 + settle + 链延迟 + 一个子块的入口子块才参与采集。所以编排器必须：
          · 每块把两块表本块新完成的全部子块一次交齐（按时间顺序；LoudnessMeter::readSubBlocksSince 读到的全部）；
          · 中断发生在哪一块，就在那一块调 process 之前调 invalidateCurrentWindow（或在那一块的 process 里 guardActive = true）。
      - 配对：入口子块按链延迟平移后，与结束时刻最接近（相差 ≤ 半个子块）的出口子块配成一对；
        块选择：两侧都在绝对门限（−70 LUFS）以上的配对才计入，两侧用的是同一批配对。静音不推进采集。
      - 一轮 = calibrationWindowSeconds 的有效配对，分 4 个子窗；本轮所有子窗必须在同一个实际执行的偏差下测得。
      - 实际执行的偏差（appliedBiasDb）只要变了（> 1e-4 dB），就当作一次中断（换了工作点）；已完成但偏差不同的子窗不再混用。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"
#include "LoudnessMeter.h"

#include <juce_dsp/juce_dsp.h>

namespace celadon::glaze
{
    class GlazeCalibrator
    {
    public:
        enum class State : int { idle = 0, settling = 1, collecting = 2, finished = 3 };

        GlazeCalibrator();
        ~GlazeCalibrator();

        void prepare (double sampleRate, int maxBlockSize);
        void reset() noexcept;                         // 回到 idle，丢弃全部子窗

        // 开始一轮校准：target = 本方案的目标净提升（dB，已含 GLAZE 与约束）；
        // correctionLimitDb = 本轮允许的修正范围（已按 GLAZE 与置信度缩放）；reference = 参考条件。
        // 目标净提升指整条链「出口响度 − 入口响度」的期望值（校准器测的就是整条链，含手动部分）。
        // 从纯前馈（建议偏差 0）起步；target 不是有限值时直接结束为无效（valid = false、偏差 0）。
        void begin (float targetNetGainDb, float correctionLimitDb, const CalibrationReference& reference,
                    int chainLatencySamples) noexcept;

        // 当前子窗作废（中断）；已完成的子窗保留。在中断发生的那一块、调 process 之前调。
        void invalidateCurrentWindow() noexcept;

        // 每块调用：input / output = 两块表本块新完成的 100 ms 子块（编排器从表的子块环形缓冲读出后转交，
        // 校准器不直接碰表，单独可测）。入口子块按链延迟平移后与出口子块按时间戳配对，块选择口径两侧一致；
        // appliedBiasDb = 本块实际执行的校准偏差（钳位后，回馈用它）；
        // guardActive = 保护层不在参考状态（本块 g_保护 ≠ 0 或刚改过；传 GlazeGuard::isEngaged()）：当前子窗作废，
        //               持续到累计 calibrationWindowSeconds 的有效音乐就结束为 budgetLimited。
        void process (const LoudnessMeter::SubBlock* input, int numInput,
                      const LoudnessMeter::SubBlock* output, int numOutput,
                      float appliedBiasDb, bool guardActive, int numSamples) noexcept;

        State getState() const noexcept;

        // 最新建议的偏差（编排器负责平滑施加）；finished 后固定。valid 只在结束时置位（校准中的建议是临时的）。
        const Calibration& getResult() const noexcept;

        // 参考条件是否与给定条件一致（不一致即失效）。浮点项容许 1e-4（GLAZE %、dB、ms）的表示误差，采样率相对 1e-9。
        // 手动参数：两份都是新口径时比离散哈希，三个对数刻度值按相对 1e-4 比；有一份是旧存档的口径就比旧口径哈希（BUG-004）。
        static bool referenceMatches (const CalibrationReference& a, const CalibrationReference& b) noexcept;

        // ---- v0.6 A3 追加 ----

        // 时长常数（calibrationSettleSeconds / calibrationWindowSeconds / calibrationRounds）。默认出厂常数；
        // 下一次 begin 起生效（编排器用 setConstantsForTesting 换了常数时一并转交）。
        void setConstants (const Constants& constants) noexcept;

        // 进度（界面 / 测试用；音频线程读）。
        struct Progress
        {
            int round = 0;                      // 已完成的轮数
            int completedSubWindows = 0;        // 本轮已完成、条件一致的子窗数
            int subWindowsPerRound = 4;
            double currentSubWindowSeconds = 0.0;   // 当前子窗已采到的有效配对时长
            float lastRoundErrorDb = 0.0f;      // 最近一轮的修正量 e
            bool blockedUp = false;             // 执行器向上饱和（实际执行 < 建议）：不再往上建议
            bool blockedDown = false;
            double guardSeconds = 0.0;          // 保护层介入期间累计的有效音乐时长（预算）
        };

        Progress getProgress() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeCalibrator)
    };
}
