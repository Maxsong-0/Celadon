/*
  ==============================================================================

    GlazeCalibrator.cpp
    GLAZE 校准器（设计规格-GLAZE-决策与校准 第 6 节；04-协调/2026-09-25-v0.6-GLAZE实现计划 A3）。

    本文件里的内部常数与理由：
      - 每轮 4 个子窗（出厂窗长 10 s → 每个 2.5 s = 25 个 100 ms 配对）：一次中断最多丢 2.5 s 的数据；
        子窗又长到足够让「同一段音乐的出口 − 入口」稳定（两侧同时测、同一批配对，音乐本身的起伏大部分抵消）。
      - 收敛容差 0.25 dB：本轮 |e| ≤ 0.25 dB 且子窗不「方向相反」→ 已施加的偏差被这一轮测量直接验证，结束为有效。
      - 方向相反：有子窗 e ≥ +0.5 dB、同时有子窗 e ≤ −0.5 dB（链的净提升随音乐起伏超过 ±0.5 dB，单一偏差对不齐）。
        ±0.25 dB 以内的正负相间是正常的测量散布，不算。
      - 最后一轮仍未收敛 → partial，偏差取「已施加 + min(0, e)」：只允许往下修（更轻），不在没有验证的情况下再往上推。
      - 偏差变化判据 1e-4 dB；执行器饱和判据 0.01 dB（设置稳定后实际执行值仍与建议差这么多，说明被钳住了）。
      - 保护层预算 = calibrationWindowSeconds 的有效音乐（保护层介入期间收到的过门限入口子块累计）：
        一次 1 dB 的退让连同 5 s 等待与恢复约 9 s，单次介入不会结束校准，反复或长时间介入才结束为 budgetLimited。
      - 屏障多留一个子块：配对允许出口子块与平移后的入口相差半个子块，多留一个子块保证配到的出口也完全在中断之后。
      - 绝对门限 −70 LUFS 按子块均方比较（LUFS = −0.691 + 10·log10(均方)，与 LoudnessMeter 同口径），不取对数。
    全部状态固定容量（Impl 在构造时分配，之后 prepare / begin / process 都不分配）。

  ==============================================================================
*/

#include "GlazeCalibrator.h"

#include <array>
#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        constexpr int    subWindowsPerRound    = 4;
        constexpr float  convergedToleranceDb  = 0.25f;
        constexpr float  conflictDb            = 0.5f;
        constexpr float  appliedChangeDb       = 1.0e-4f;
        constexpr float  saturationDb          = 0.01f;
        constexpr double subBlockSeconds       = 0.1;      // 与 LoudnessMeter 的子块同长（round(fs · 0.1)）
        constexpr double absoluteGateLufs      = -70.0;
        constexpr int    fifoCapacity          = 2 * LoudnessMeter::subBlockRingCapacity;
        constexpr double maxSeconds            = 1.0e5;    // 时长常数的防呆上限

        double gateMeanSquare() noexcept
        {
            return std::pow (10.0, (absoluteGateLufs + 0.691) / 10.0);
        }

        bool usableEnergy (double meanSquare) noexcept
        {
            return std::isfinite (meanSquare) && meanSquare >= 0.0;
        }

        // 定长环形队列（子块），满了丢最旧的。
        struct SubBlockFifo
        {
            std::array<LoudnessMeter::SubBlock, (size_t) fifoCapacity> items {};
            int head = 0, count = 0;

            void clear() noexcept                              { head = count = 0; }
            bool empty() const noexcept                        { return count == 0; }
            const LoudnessMeter::SubBlock& front() const noexcept   { return items[(size_t) head]; }

            void pop() noexcept
            {
                head = (head + 1) % fifoCapacity;
                --count;
            }

            void push (const LoudnessMeter::SubBlock& item) noexcept
            {
                if (count == fifoCapacity)
                    pop();

                items[(size_t) ((head + count) % fifoCapacity)] = item;
                ++count;
            }
        };

        struct SubWindow
        {
            double energyIn = 0.0;
            double energyOut = 0.0;
            float appliedBiasDb = 0.0f;
        };

        float errorFor (float target, double energyIn, double energyOut) noexcept
        {
            return target - (float) (10.0 * std::log10 (energyOut / energyIn));
        }
    }

    struct GlazeCalibrator::Impl
    {
        Constants constants = factoryConstants();
        double sampleRate = 48000.0;
        int subBlockLength = 4800;
        double gate = gateMeanSquare();

        State state = State::idle;
        Calibration result;

        // 本次校准（begin）
        float target = 0.0f;
        float limit = 0.0f;
        std::int64_t latency = 0;
        std::int64_t settleSamples = 0;
        std::int64_t subWindowSamples = 0;
        std::int64_t guardBudgetSamples = 0;
        int rounds = 2;

        // 中断屏障
        bool eventPending = false;
        bool awaitingBarrier = false;
        bool barrierKnown = false;
        std::int64_t eligibleFromStart = 0;

        // 回馈
        bool haveApplied = false;
        float lastApplied = 0.0f;
        bool blockedUp = false, blockedDown = false;
        bool checkActuator = false;   // 下一次开始采集时核对「实际执行 ≈ 建议」

        // 配对与采集
        SubBlockFifo pendingInput, pendingOutput;
        double currentIn = 0.0, currentOut = 0.0;
        std::int64_t currentSamples = 0;
        std::array<SubWindow, (size_t) subWindowsPerRound> completed {};
        int completedCount = 0;
        int round = 0;
        float lastRoundError = 0.0f;
        std::int64_t guardSamples = 0;

        //==============================================================================
        void clearCollection() noexcept
        {
            pendingInput.clear();
            pendingOutput.clear();
            currentIn = currentOut = 0.0;
            currentSamples = 0;
        }

        // 中断：丢掉当前子窗与还没配上的子块（它们的出口可能在中断之后才出来），等下一个屏障。
        void interrupt() noexcept
        {
            clearCollection();
            awaitingBarrier = true;
            barrierKnown = false;

            if (state == State::collecting)
                state = State::settling;
        }

        float clampToLimit (float bias) const noexcept
        {
            return juce::jlimit (-limit, limit, bias);
        }

        void finish (float bias, bool partial, bool budgetLimited) noexcept
        {
            state = State::finished;
            result.gainBiasDb = std::isfinite (bias) ? clampToLimit (bias) : 0.0f;
            result.valid = true;
            result.partial = partial;
            result.budgetLimited = budgetLimited;
            clearCollection();
        }

        void finishConservative (float applied, float error) noexcept
        {
            // 只允许往下修；执行器向下饱和时往下也执行不了，保持已施加的值。
            auto bias = applied + juce::jmin (0.0f, error);

            if (blockedDown)
                bias = juce::jmax (bias, applied);

            finish (bias, true, false);
        }

        //==============================================================================
        void acceptInput (const LoudnessMeter::SubBlock& block) noexcept
        {
            if (awaitingBarrier)
            {
                // 中断之后送来的第一个入口子块：可能跨着中断点，不用；它的结束时刻就是屏障。
                awaitingBarrier = false;
                barrierKnown = true;
                eligibleFromStart = block.endSample + 1 + settleSamples + latency + (std::int64_t) subBlockLength;
                return;
            }

            if (! barrierKnown)
                return;

            const auto start = block.endSample - (std::int64_t) subBlockLength + 1;

            if (start < eligibleFromStart)
                return;

            pendingInput.push (block);
        }

        void matchPairs() noexcept
        {
            const auto halfSubBlock = (std::int64_t) subBlockLength / 2;

            while (! pendingInput.empty() && ! pendingOutput.empty() && state != State::finished)
            {
                const auto in = pendingInput.front();
                const auto out = pendingOutput.front();
                const auto wanted = in.endSample + latency;

                if (out.endSample < wanted - halfSubBlock)
                {
                    pendingOutput.pop();   // 比这个入口子块对应的时刻还早：不会再有入口子块配它
                    continue;
                }

                if (out.endSample > wanted + halfSubBlock)
                {
                    pendingInput.pop();    // 对应时刻的出口子块缺了（表跳过、读者落后）：这个入口子块作废
                    continue;
                }

                pendingInput.pop();
                pendingOutput.pop();
                addPair (in.meanSquare, out.meanSquare);
            }
        }

        void addPair (double energyIn, double energyOut) noexcept
        {
            if (! usableEnergy (energyIn) || ! usableEnergy (energyOut))
            {
                interrupt();   // 表读到非有限值：出口出过事（链会清零重启淡入），当作中断
                return;
            }

            // 块选择：两侧都在绝对门限以上才算，两侧同一批配对。
            if (energyIn < gate || energyOut < gate)
                return;

            if (state == State::settling)
                startCollecting();

            currentIn += energyIn;
            currentOut += energyOut;
            currentSamples += subBlockLength;

            if (currentSamples >= subWindowSamples)
                completeSubWindow();
        }

        void startCollecting() noexcept
        {
            state = State::collecting;

            if (! checkActuator)
                return;

            // 设置已稳定 settle 以上：实际执行值与建议仍差 > 0.01 dB → 该方向被钳住了。按实际执行值继续（回馈用执行量），
            // 之后不再往那个方向建议（防积分饱和），另一个方向照常允许。
            checkActuator = false;

            if (lastApplied < result.gainBiasDb - saturationDb)
            {
                blockedUp = true;
                result.gainBiasDb = lastApplied;
            }
            else if (lastApplied > result.gainBiasDb + saturationDb)
            {
                blockedDown = true;
                result.gainBiasDb = lastApplied;
            }
        }

        void completeSubWindow() noexcept
        {
            // 本轮所有子窗必须在同一个实际执行的偏差下测得；偏差变过（之前已作为中断处理），旧的子窗不再混用。
            if (completedCount > 0 && std::abs (completed[0].appliedBiasDb - lastApplied) > appliedChangeDb)
                completedCount = 0;

            completed[(size_t) completedCount] = { currentIn, currentOut, lastApplied };
            ++completedCount;

            currentIn = currentOut = 0.0;
            currentSamples = 0;

            if (completedCount >= subWindowsPerRound)
                completeRound();
        }

        void completeRound() noexcept
        {
            double energyIn = 0.0, energyOut = 0.0;
            float highest = -1.0e9f, lowest = 1.0e9f;

            for (int k = 0; k < completedCount; ++k)
            {
                const auto& window = completed[(size_t) k];
                energyIn += window.energyIn;
                energyOut += window.energyOut;

                const auto e = errorFor (target, window.energyIn, window.energyOut);
                highest = juce::jmax (highest, e);
                lowest = juce::jmin (lowest, e);
            }

            const auto applied = completed[0].appliedBiasDb;
            const auto error = errorFor (target, energyIn, energyOut);
            const auto conflict = highest >= conflictDb && lowest <= -conflictDb;

            completedCount = 0;
            ++round;
            lastRoundError = error;

            // 这一轮测到的就是已施加偏差下的净提升：够准就是验证通过。
            if (std::abs (error) <= convergedToleranceDb && ! conflict)
            {
                finish (applied, false, false);
                return;
            }

            if (round >= rounds)
            {
                finishConservative (applied, error);
                return;
            }

            // 下一轮的建议 = 实际执行值 + e（回馈用执行量），钳到修正范围；执行器饱和的方向不再推。
            auto proposed = clampToLimit (applied + error);

            if (blockedUp)    proposed = juce::jmin (proposed, applied);
            if (blockedDown)  proposed = juce::jmax (proposed, applied);

            // 到了上限 / 被钳住，建议已经动不了：再测一轮还是同样的结果，按保守结果结束。
            if (std::abs (proposed - applied) <= appliedChangeDb)
            {
                finishConservative (applied, error);
                return;
            }

            // 新建议：本块之后的配对还是旧偏差下的，一律不要（当作中断，重新等屏障与 settle）；编排器施加时偏差一变，
            // process 还会再当一次中断。执行器若被钳住、偏差一直不变，settle 之后开始采集时由 startCollecting 核对出来。
            result.gainBiasDb = proposed;
            checkActuator = true;
            interrupt();
            state = State::settling;
        }
    };

    //==============================================================================
    GlazeCalibrator::GlazeCalibrator() : impl (std::make_unique<Impl>()) {}
    GlazeCalibrator::~GlazeCalibrator() = default;

    void GlazeCalibrator::prepare (double sampleRate, int maxBlockSize)
    {
        juce::ignoreUnused (maxBlockSize);   // 子块由表交来，与宿主块长无关

        if (sampleRate > 0.0 && std::isfinite (sampleRate))
            impl->sampleRate = sampleRate;

        impl->subBlockLength = juce::jmax (1, juce::roundToInt (impl->sampleRate * subBlockSeconds));
        reset();
    }

    void GlazeCalibrator::reset() noexcept
    {
        auto& s = *impl;
        s.state = State::idle;
        s.result = Calibration {};
        s.eventPending = s.awaitingBarrier = s.barrierKnown = false;
        s.haveApplied = false;
        s.lastApplied = 0.0f;
        s.blockedUp = s.blockedDown = s.checkActuator = false;
        s.clearCollection();
        s.completedCount = 0;
        s.round = 0;
        s.lastRoundError = 0.0f;
        s.guardSamples = 0;
    }

    void GlazeCalibrator::setConstants (const Constants& constants) noexcept
    {
        impl->constants = constants;
    }

    void GlazeCalibrator::begin (float targetNetGainDb, float correctionLimitDb, const CalibrationReference& reference,
                                 int chainLatencySamples) noexcept
    {
        reset();

        auto& s = *impl;
        s.result.reference = reference;

        if (! std::isfinite (targetNetGainDb))
        {
            s.state = State::finished;   // 没有可用的目标：纯前馈，valid = false
            return;
        }

        auto toSamples = [&s] (double seconds)
        {
            const auto clamped = juce::jlimit (0.0, maxSeconds, std::isfinite (seconds) ? seconds : 0.0);
            return (std::int64_t) std::llround (clamped * s.sampleRate);
        };

        s.target = targetNetGainDb;
        s.limit = std::isfinite (correctionLimitDb) ? juce::jmax (0.0f, correctionLimitDb) : 0.0f;
        s.latency = juce::jmax (0, chainLatencySamples);
        s.settleSamples = toSamples (s.constants.calibrationSettleSeconds);
        s.subWindowSamples = juce::jmax ((std::int64_t) s.subBlockLength,
                                         toSamples (s.constants.calibrationWindowSeconds / (double) subWindowsPerRound));
        s.guardBudgetSamples = juce::jmax ((std::int64_t) s.subBlockLength, toSamples (s.constants.calibrationWindowSeconds));
        s.rounds = juce::jlimit (1, 16, s.constants.calibrationRounds);

        s.result.gainBiasDb = 0.0f;   // 从纯前馈起步
        s.state = State::settling;
        s.eventPending = true;        // begin 之前的一切都不算
    }

    void GlazeCalibrator::invalidateCurrentWindow() noexcept
    {
        auto& s = *impl;

        if (s.state == State::settling || s.state == State::collecting)
        {
            s.interrupt();
            s.eventPending = true;
        }
    }

    void GlazeCalibrator::process (const LoudnessMeter::SubBlock* input, int numInput,
                                   const LoudnessMeter::SubBlock* output, int numOutput,
                                   float appliedBiasDb, bool guardActive, int numSamples) noexcept
    {
        juce::ignoreUnused (numSamples);   // 时间全按子块时间戳推进
        auto& s = *impl;

        if (s.state != State::settling && s.state != State::collecting)
            return;

        // 这一块是否有中断：外部通知、保护层介入、实际执行的偏差变了（换了工作点）。
        auto interrupted = s.eventPending || guardActive;
        s.eventPending = false;

        if (! std::isfinite (appliedBiasDb))
        {
            interrupted = true;
        }
        else if (! s.haveApplied || std::abs (appliedBiasDb - s.lastApplied) > appliedChangeDb)
        {
            // 换了工作点：本轮已完成的子窗是另一个偏差下测的，不再混用。
            if (s.haveApplied)
            {
                interrupted = true;
                s.completedCount = 0;
            }

            s.haveApplied = true;
            s.lastApplied = appliedBiasDb;
        }

        if (interrupted)
            s.interrupt();

        for (int i = 0; i < numInput; ++i)
        {
            const auto& block = input[i];

            // 保护层预算：它介入期间收到的有效音乐。
            if (guardActive && usableEnergy (block.meanSquare) && block.meanSquare >= s.gate)
                s.guardSamples += s.subBlockLength;

            // 中断这一块送来的入口子块都与中断块重叠，一律不用。
            if (! interrupted)
                s.acceptInput (block);
        }

        // 中断之后送来的第一个入口子块定屏障（见 acceptInput）。
        if (interrupted)
            s.awaitingBarrier = true;

        for (int i = 0; i < numOutput; ++i)
            s.pendingOutput.push (output[i]);

        s.matchPairs();

        if (s.state != State::finished && s.guardSamples >= s.guardBudgetSamples)
        {
            // 保护层反复 / 长时间介入：不无限等待，结束为预算受限；正向的修正在预算受限时不保留。
            const auto applied = s.haveApplied ? s.lastApplied : 0.0f;
            s.finish (juce::jmin (applied, 0.0f), false, true);
        }
    }

    GlazeCalibrator::State GlazeCalibrator::getState() const noexcept    { return impl->state; }
    const Calibration& GlazeCalibrator::getResult() const noexcept      { return impl->result; }

    GlazeCalibrator::Progress GlazeCalibrator::getProgress() const noexcept
    {
        const auto& s = *impl;
        Progress p;
        p.round = s.round;
        p.completedSubWindows = s.completedCount;
        p.subWindowsPerRound = subWindowsPerRound;
        p.currentSubWindowSeconds = (double) s.currentSamples / s.sampleRate;
        p.lastRoundErrorDb = s.lastRoundError;
        p.blockedUp = s.blockedUp;
        p.blockedDown = s.blockedDown;
        p.guardSeconds = (double) s.guardSamples / s.sampleRate;
        return p;
    }

    bool GlazeCalibrator::referenceMatches (const CalibrationReference& a, const CalibrationReference& b) noexcept
    {
        // 浮点项只容许表示误差（旋钮归一化来回一趟的舍入），任何真实改动都会超过它；NaN 与任何值都不一致。
        auto close = [] (float x, float y) { return std::abs (x - y) <= 1.0e-4f; };

        // 对数刻度的三项（BUG-004，ADR-018）按相对差比：它们不吸附，重开工程的归一化往返会漂几个 ulp，反复存读最多约 1.2e-5
        //（ParameterRanges.h）；显示精度是整数 ms / Hz，最小一格也有 1e-3。1e-4 离两边都有约 8 倍余量。相对差 = 归一化位置的差
        //（对数刻度上处处等宽）。非有限值沿用旧口径哈希的记号：两边都非有限才算一致（NaN、±Inf 在哈希里是同一个记号）。
        auto closeRelative = [] (float x, float y)
        {
            if (! std::isfinite (x) || ! std::isfinite (y))
                return ! std::isfinite (x) && ! std::isfinite (y);

            return std::abs (x - y) <= 1.0e-4f * juce::jmax (std::abs (x), std::abs (y));
        };

        // 手动参数：两份都是新口径 → 离散哈希 + 三项容差；有一份是旧存档（读回后没能升格，见 upgradeLegacyConditions）→ 只能
        // 按旧口径哈希比（与修前相同）。新口径的参考同样带着旧口径哈希，所以混比也有意义。
        const auto continuous = a.conditionsFormat == continuousConditionsFormat && b.conditionsFormat == continuousConditionsFormat;
        const auto manualMatches = continuous
            ? a.discreteParamsHash == b.discreteParamsHash
                  && closeRelative (a.limitReleaseMs, b.limitReleaseMs)
                  && closeRelative (a.clayLowCutHz, b.clayLowCutHz)
                  && closeRelative (a.clayMonoHz, b.clayMonoHz)
            : a.manualParamsHash == b.manualParamsHash;

        const auto rateTolerance = 1.0e-9 * juce::jmax (std::abs (a.sampleRate), std::abs (b.sampleRate));

        return a.profileId == b.profileId
            && a.style == b.style
            && a.genre == b.genre            // T-038：换曲风或交付 → 方案重算、重新校准（设计规格-曲风与交付 第 4.3 节）
            && a.delivery == b.delivery
            && close (a.glazePercent, b.glazePercent)
            && manualMatches
            && close (a.ceilingDb, b.ceilingDb)
            && close (a.lookaheadMs, b.lookaheadMs)
            && std::abs (a.sampleRate - b.sampleRate) <= rateTolerance;
    }
}
