/*
  ==============================================================================

    GlazeGuard.cpp
    GLAZE 保护层（设计规格-GLAZE-决策与校准 第 7 节；04-协调/2026-09-25-v0.6-GLAZE实现计划 A3）。

    本文件里的内部常数与理由：
      - 时隙 10 ms：比常见宿主块（≤ 512 @ 48k ≈ 10.7 ms）细、3 s 窗只需 300 格。判定与退让 / 恢复都在时隙结束时做，
        轨迹与块长无关（大块摊到多格、小块并进一格）；返回给编排器的 g_保护最多晚一个时隙（≤ 10 ms）更新。
      - p95 近似：各时隙的均值（时隙内按样本平均的块均值）按格数算 95 百分位，判定写成「超过上限的格数 > 5 %」。
        块内 / 时隙内的起伏被平均掉，所以估计值偏低，偏差不超过同一时隙（块长 > 10 ms 时为同一块）内 GR 的（最大 − 均值）：
        Glue（攻击 20 ms、释放 ≥ 50 ms）在 10 ms 内的变化一般在零点几 dB；限幅器的快速尖峰另由「峰值」约束管。
        CeladonChain 通过 getSamplesUntilSlotBoundary 切片，实际模块统计不跨时隙；因此宿主大块不再进一步摊宽短峰。
        直接调用本类且提供跨时隙均值的外部调用方仍只有近似统计，不能据此宣称音频分块不变。
      - 峰值类（Limit 峰值 GR、Clip 深度）按最近 100 ms 的最大值判「此刻超出」：单个尖峰只贡献 100 ms 的超出时间，
        3 s 内累计满 1 s（≈ 3 次 / 秒以上持续超限）才退让；若用 3 s 窗最大值，一个尖峰就会让它连续超出 3 s、退让 2 dB。
      - 回差：dB 类上限留 0.5 dB（上限很小时不超过上限的一半），占比类留上限的 20 %。进入回差区即停止恢复、
        安全计时清零；再次满 5 s 安全音乐才继续恢复——所以恢复会停在略低于上限的地方，不在上限附近来回拉锯。
      - 代表性：统计窗里 g_保护与当前值相差 > 0.5 dB 的旧时隙移出（只从最旧一端移）。1 dB/s 退让时统计只看最近约 0.5 s，
        判定滞后约 0.25 s（多退约 0.25 dB），而不是 3 s 窗带来的 1.5 dB 以上；恢复（0.3 dB/s）时看最近约 1.7 s。
        「3 s 内累计超出 ≥ 1 s」的超出计时仍按完整的 3 s 历史算，不受移出影响。
      - 各统计先清洗：NaN / 负数按 0，dB 值封顶 1000，占比钳到 [0, 1]，累计量不会被坏值永久污染；
        累计的加减每 1024 个时隙整窗重算一次，清掉舍入累积（结果只取决于时隙序列，仍与块长无关）。

  ==============================================================================
*/

#include "GlazeGuard.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace celadon::glaze
{
    namespace
    {
        constexpr double slotSeconds          = 0.01;
        constexpr double peakHoldSeconds      = 0.1;
        constexpr double p95ExceedFraction    = 0.05;   // 超过上限的时隙占比 > 5 % ⟺ 95 百分位 > 上限
        constexpr float  marginDb             = 0.5f;
        constexpr float  marginMaxFraction    = 0.5f;   // dB 回差不超过上限的一半
        constexpr float  activityMarginRatio  = 0.2f;
        constexpr double relevanceDb          = 0.5;
        constexpr int    recomputeEverySlots  = 1024;
        constexpr double maxSeconds           = 1.0e6;  // 时长常数的防呆上限（换算成格数前先钳，免得溢出）

        float cleanDb (float value) noexcept
        {
            if (! (value > 0.0f))   // NaN、负数、0
                return 0.0f;

            return value < 1000.0f ? value : 1000.0f;
        }

        float cleanFraction (float value) noexcept
        {
            if (! (value > 0.0f))
                return 0.0f;

            return value < 1.0f ? value : 1.0f;
        }

        // 上限 ≤ 0 或非有限值：该项不设约束（未配置；也不因为手动处理触发退让）。
        bool enforced (float cap) noexcept
        {
            return std::isfinite (cap) && cap > 0.0f;
        }

        float nearLineDb (float cap) noexcept
        {
            return cap - juce::jmin (marginDb, cap * marginMaxFraction);
        }

        float nearLineFraction (float cap) noexcept
        {
            return cap * (1.0f - activityMarginRatio);
        }

        bool sameCaps (const StyleConstants& a, const StyleConstants& b) noexcept
        {
            return a.glueGrMeanCapDb == b.glueGrMeanCapDb && a.glueGrP95CapDb == b.glueGrP95CapDb
                && a.clipDepthCapDb == b.clipDepthCapDb && a.clipActivityCap == b.clipActivityCap
                && a.limitGrMeanCapDb == b.limitGrMeanCapDb && a.limitGrP95CapDb == b.limitGrP95CapDb
                && a.limitGrPeakCapDb == b.limitGrPeakCapDb;
        }

        bool sameTiming (const Constants& a, const Constants& b) noexcept
        {
            return a.guardMinDb == b.guardMinDb && a.guardAttackDbPerSecond == b.guardAttackDbPerSecond
                && a.guardReleaseDbPerSecond == b.guardReleaseDbPerSecond && a.guardTriggerSeconds == b.guardTriggerSeconds
                && a.guardWindowSeconds == b.guardWindowSeconds && a.guardRecoverAfterSeconds == b.guardRecoverAfterSeconds;
        }
    }

    //==============================================================================
    void GlazeGuard::prepare (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0 && std::isfinite (newSampleRate))
            sampleRate = newSampleRate;

        updateTiming();
        reset();
    }

    void GlazeGuard::reset() noexcept
    {
        protection = 0.0;
        protectionDb = 0.0f;
        retreating = recovering = changedThisBlock = false;
        planLimited = false;
        safeSlots = 0;
        clearWindows();
    }

    void GlazeGuard::clearWindows() noexcept
    {
        discardPartialSlot();
        clearStatistics();
    }

    void GlazeGuard::clearStatistics() noexcept
    {
        writeIndex = 0;
        historyCount = statsCount = 0;
        overSlotsInHistory = 0;
        slotsSinceRecompute = 0;

        sumGlue = sumLimit = sumClip = 0.0;
        glueAboveCap = glueAboveNear = limitAboveCap = limitAboveNear = 0;
    }

    void GlazeGuard::updateTiming() noexcept
    {
        slotLength = juce::jmax (1, (int) std::lround (sampleRate * slotSeconds));
        const auto slotDuration = (double) slotLength / sampleRate;

        auto toSlots = [slotDuration] (double seconds)
        {
            const auto clamped = juce::jlimit (0.0, maxSeconds, std::isfinite (seconds) ? seconds : 0.0);
            return (int) juce::jmin ((double) INT_MAX / 4.0, (double) std::llround (clamped / slotDuration));
        };

        auto rate = [] (float dbPerSecond) { return std::isfinite (dbPerSecond) ? juce::jmax (0.0, (double) dbPerSecond) : 0.0; };

        windowSlots   = juce::jlimit (1, slotCapacity - 1, toSlots (constants.guardWindowSeconds));
        triggerSlots  = juce::jlimit (0, windowSlots, toSlots (constants.guardTriggerSeconds));
        recoverSlots  = toSlots (constants.guardRecoverAfterSeconds);
        peakHoldSlots = juce::jlimit (1, windowSlots, toSlots (peakHoldSeconds));
        retreatStepDb = rate (constants.guardAttackDbPerSecond) * slotDuration;
        recoverStepDb = rate (constants.guardReleaseDbPerSecond) * slotDuration;
        minimumDb     = std::isfinite (constants.guardMinDb) ? juce::jmin (0.0f, constants.guardMinDb) : -3.0f;

        // 下限上移（例如 −3 → −2）时当前值跟着钳上来。
        if (protection < (double) minimumDb)
        {
            protection = (double) minimumDb;
            protectionDb = (float) protection;
        }
    }

    void GlazeGuard::setLimits (const StyleConstants& blendedLimits, const Constants& newConstants) noexcept
    {
        const auto capsChanged = ! limitsSet || ! sameCaps (limits, blendedLimits);
        const auto timingChanged = ! sameTiming (constants, newConstants);

        limits = blendedLimits;
        constants = newConstants;
        limitsSet = true;

        if (timingChanged)
            updateTiming();

        if (capsChanged)
            recountNearOver();
    }

    //==============================================================================
    void GlazeGuard::snapRelief() noexcept
    {
        protection = 0.0;
        protectionDb = 0.0f;
        retreating = recovering = changedThisBlock = false;
        planLimited = false;
        lastOver = false;
        lastCause = GuardCause::none;
        lastOvershootDb = 0.0f;
    }

    GuardOutput GlazeGuard::process (const ModuleStats& stats) noexcept
    {
        retreating = recovering = changedThisBlock = false;

        auto output = [this]() noexcept
        {
            GuardOutput out;
            out.reliefDb = protectionDb;
            out.cause = (protectionDb < 0.0f || retreating || (planLimited && lastOver)) ? lastCause : GuardCause::none;
            out.engaged = retreating || (planLimited && lastOver);
            return out;
        };

        // 静音、停播（有效音乐门限以下）整块不计：窗口、计时、g_保护全都停住。
        if (! limitsSet || ! stats.musicActive || stats.numSamples <= 0)
            return output();

        const auto before = protection;

        const auto glueMean   = cleanDb (stats.glueGrMeanDb);
        const auto limitMean  = cleanDb (stats.limitGrMeanDb);
        const auto clipFrac   = cleanFraction (stats.clipActiveFraction);
        const auto limitPeak  = cleanDb (stats.limitGrMaxDb);
        const auto clipDepth  = cleanDb (stats.clipDepthMaxDb);

        auto remaining = stats.numSamples;

        while (remaining > 0)
        {
            const auto take = juce::jmin (remaining, slotLength - partSamples);

            partGlueSum  += (double) glueMean * (double) take;
            partLimitSum += (double) limitMean * (double) take;
            partClipSum  += (double) clipFrac * (double) take;
            partLimitPeak = juce::jmax (partLimitPeak, limitPeak);
            partClipDepth = juce::jmax (partClipDepth, clipDepth);

            partSamples += take;
            remaining -= take;

            if (partSamples >= slotLength)
                completeSlot();
        }

        retreating = protection < before;
        recovering = protection > before;
        changedThisBlock = protection != before;
        protectionDb = (float) protection;
        return output();
    }

    void GlazeGuard::completeSlot() noexcept
    {
        const auto length = (double) slotLength;

        Slot slot;
        slot.glueMeanDb   = (float) (partGlueSum / length);
        slot.limitMeanDb  = (float) (partLimitSum / length);
        slot.clipFraction = (float) (partClipSum / length);
        slot.limitPeakDb  = partLimitPeak;
        slot.clipDepthDb  = partClipDepth;
        // 按时隙推进的 g_保护（本时隙判定之前的值）。实际施加的是块开头的值，两者最多差一块之内的变化（4096 样本 @ 48k
        // 退让时约 0.09 dB），远小于 0.5 dB 的代表性门槛；用它，统计窗与轨迹才只取决于时隙序列、与块长无关。
        slot.protectionDb = (float) protection;

        partGlueSum = partLimitSum = partClipSum = 0.0;
        partLimitPeak = partClipDepth = 0.0f;
        partSamples = 0;

        // 本时隙开始时、之前 3 s（windowSlots 格）里已累计的超出时隙——在移出最旧一格之前数。
        const auto overBefore = overSlotsInHistory;

        // 3 s 历史满了：最旧的一格离开（若还在统计窗里也一并移出）。
        while (historyCount >= windowSlots)
        {
            const auto& oldest = slots[(size_t) indexFromNewest (historyCount - 1)];

            if (statsCount == historyCount)
            {
                removeFromStats (oldest);
                --statsCount;
            }

            overSlotsInHistory -= oldest.over ? 1 : 0;
            --historyCount;
        }

        slots[(size_t) writeIndex] = slot;   // over 先记 false，判定后回填
        writeIndex = (writeIndex + 1) % slotCapacity;
        ++historyCount;
        ++statsCount;
        addToStats (slot);

        // 统计窗只留与当前 g_保护相差 ≤ 0.5 dB 的时隙（至少留最新一格）。
        while (statsCount > 1)
        {
            const auto& oldestStats = slots[(size_t) indexFromNewest (statsCount - 1)];

            if (std::abs ((double) oldestStats.protectionDb - protection) <= relevanceDb)
                break;

            removeFromStats (oldestStats);
            --statsCount;
        }

        if (++slotsSinceRecompute >= recomputeEverySlots)
        {
            recomputeSums();
            slotsSinceRecompute = 0;
        }

        const auto verdict = evaluate();
        lastOver = verdict.over;
        lastOvershootDb = juce::jmax (verdict.limitOvershootDb, verdict.otherOvershootDb);
        lastCause = (verdict.limitOver && verdict.limitOvershootDb >= verdict.otherOvershootDb)
                        ? GuardCause::deliveryPeak : GuardCause::budget;

        slots[(size_t) indexFromNewest (0)].over = verdict.over;
        overSlotsInHistory += verdict.over ? 1 : 0;

        const auto slotDuration = (double) slotLength / sampleRate;
        const auto span = minimumDb < 0.0f ? -minimumDb : 3.0f;
        const auto overshootT = juce::jlimit (0.0f, 1.0f, lastOvershootDb / 3.0f);
        const auto depthT = juce::jlimit (0.0f, 1.0f, (float) (-protection) / span);
        // T-047：超限 0→3 dB 映射退让 0.3→6 dB/s；退让深度 0→下限映射回程 0.15→0.3 dB/s。
        const auto retreatThisSlot = (0.3 + (double) overshootT * (6.0 - 0.3)) * slotDuration;
        const auto recoverThisSlot = (0.15 + (double) depthT * (0.3 - 0.15)) * slotDuration;

        if (verdict.over)
        {
            safeSlots = 0;

            // 本时隙开始时已满足「3 s 内累计超出 ≥ 1 s」且此刻仍超出：按上面的变速率退让这一格。
            if (overBefore >= triggerSlots)
            {
                protection = juce::jmax ((double) minimumDb, protection - retreatThisSlot);

                if (protection <= (double) minimumDb)
                    planLimited = true;   // 退到下限仍超约束
            }
        }
        else if (verdict.near)
        {
            safeSlots = 0;   // 接近上限（回差区内）：停止恢复，重新计安全时间
        }
        else
        {
            // 本时隙开始时已有连续 ≥ 5 s 安全音乐：按变速率恢复这一格（只回到 0，不去找更高响度）。
            if (safeSlots >= recoverSlots && protection < 0.0)
            {
                protection = juce::jmin (0.0, protection + recoverThisSlot);

                if (protection > (double) minimumDb)
                    planLimited = false;
            }

            safeSlots = juce::jmin (safeSlots + 1, INT_MAX / 2);
        }

        if (! (protectionDb < 0.0f) && ! verdict.over)
            lastCause = GuardCause::none;
    }

    //==============================================================================
    int GlazeGuard::indexFromNewest (int age) const noexcept
    {
        return (writeIndex - 1 - age + 2 * slotCapacity) % slotCapacity;
    }

    void GlazeGuard::addToStats (const Slot& slot) noexcept
    {
        sumGlue  += (double) slot.glueMeanDb;
        sumLimit += (double) slot.limitMeanDb;
        sumClip  += (double) slot.clipFraction;

        glueAboveCap   += slot.glueMeanDb  > limits.glueGrP95CapDb ? 1 : 0;
        glueAboveNear  += slot.glueMeanDb  > nearLineDb (limits.glueGrP95CapDb) ? 1 : 0;
        limitAboveCap  += slot.limitMeanDb > limits.limitGrP95CapDb ? 1 : 0;
        limitAboveNear += slot.limitMeanDb > nearLineDb (limits.limitGrP95CapDb) ? 1 : 0;
    }

    void GlazeGuard::removeFromStats (const Slot& slot) noexcept
    {
        sumGlue  -= (double) slot.glueMeanDb;
        sumLimit -= (double) slot.limitMeanDb;
        sumClip  -= (double) slot.clipFraction;

        glueAboveCap   -= slot.glueMeanDb  > limits.glueGrP95CapDb ? 1 : 0;
        glueAboveNear  -= slot.glueMeanDb  > nearLineDb (limits.glueGrP95CapDb) ? 1 : 0;
        limitAboveCap  -= slot.limitMeanDb > limits.limitGrP95CapDb ? 1 : 0;
        limitAboveNear -= slot.limitMeanDb > nearLineDb (limits.limitGrP95CapDb) ? 1 : 0;
    }

    void GlazeGuard::recountNearOver() noexcept
    {
        glueAboveCap = glueAboveNear = limitAboveCap = limitAboveNear = 0;

        for (int age = 0; age < statsCount; ++age)
        {
            const auto& slot = slots[(size_t) indexFromNewest (age)];
            glueAboveCap   += slot.glueMeanDb  > limits.glueGrP95CapDb ? 1 : 0;
            glueAboveNear  += slot.glueMeanDb  > nearLineDb (limits.glueGrP95CapDb) ? 1 : 0;
            limitAboveCap  += slot.limitMeanDb > limits.limitGrP95CapDb ? 1 : 0;
            limitAboveNear += slot.limitMeanDb > nearLineDb (limits.limitGrP95CapDb) ? 1 : 0;
        }
    }

    void GlazeGuard::recomputeSums() noexcept
    {
        sumGlue = sumLimit = sumClip = 0.0;

        for (int age = 0; age < statsCount; ++age)
        {
            const auto& slot = slots[(size_t) indexFromNewest (age)];
            sumGlue  += (double) slot.glueMeanDb;
            sumLimit += (double) slot.limitMeanDb;
            sumClip  += (double) slot.clipFraction;
        }

        recountNearOver();
    }

    GlazeGuard::Verdict GlazeGuard::evaluate() const noexcept
    {
        Verdict verdict;

        auto note = [&verdict] (float excess, bool limitConstraint)
        {
            if (! (excess > 0.0f))
                return;

            verdict.over = true;

            if (limitConstraint)
            {
                verdict.limitOver = true;
                verdict.limitOvershootDb = juce::jmax (verdict.limitOvershootDb, excess);
            }
            else
            {
                verdict.otherOvershootDb = juce::jmax (verdict.otherOvershootDb, excess);
            }
        };

        auto check = [&verdict, &note] (double value, float cap, float nearLine, bool limitConstraint, bool fraction)
        {
            if (value > (double) nearLine)  verdict.near = true;

            if (value > (double) cap)
            {
                // 占比类没有 dB：超出的份额按 10 倍折成 dB 当量，好落进 0～3 dB 的速率曲线。
                const auto excess = fraction ? (float) ((value - (double) cap) * 10.0)
                                             : (float) (value - (double) cap);
                note (excess, limitConstraint);
            }
        };

        if (statsCount > 0)
        {
            const auto count = (double) statsCount;

            if (enforced (limits.glueGrMeanCapDb))
                check (sumGlue / count, limits.glueGrMeanCapDb, nearLineDb (limits.glueGrMeanCapDb), false, false);

            if (enforced (limits.limitGrMeanCapDb))
                check (sumLimit / count, limits.limitGrMeanCapDb, nearLineDb (limits.limitGrMeanCapDb), true, false);

            if (enforced (limits.clipActivityCap))
                check (sumClip / count, limits.clipActivityCap, nearLineFraction (limits.clipActivityCap), false, true);

            // p95 > 上限 ⟺ 超过上限的时隙 > 5 %（回差线同理）。超出的格数占比折成 0～3 dB。
            const auto allowed = p95ExceedFraction * count;

            if (enforced (limits.glueGrP95CapDb))
            {
                verdict.near = verdict.near || (double) glueAboveNear > allowed;

                if ((double) glueAboveCap > allowed)
                    note ((float) ((((double) glueAboveCap - allowed) / count) * 3.0), false);
            }

            if (enforced (limits.limitGrP95CapDb))
            {
                verdict.near = verdict.near || (double) limitAboveNear > allowed;

                if ((double) limitAboveCap > allowed)
                    note ((float) ((((double) limitAboveCap - allowed) / count) * 3.0), true);
            }
        }

        // 峰值类：最近 100 ms（历史里最新的几格）的最大值。
        if (enforced (limits.limitGrPeakCapDb) || enforced (limits.clipDepthCapDb))
        {
            float limitPeak = 0.0f, clipDepth = 0.0f;
            const auto hold = juce::jmin (peakHoldSlots, historyCount);

            for (int age = 0; age < hold; ++age)
            {
                const auto& slot = slots[(size_t) indexFromNewest (age)];
                limitPeak = juce::jmax (limitPeak, slot.limitPeakDb);
                clipDepth = juce::jmax (clipDepth, slot.clipDepthDb);
            }

            if (enforced (limits.limitGrPeakCapDb))
                check ((double) limitPeak, limits.limitGrPeakCapDb, nearLineDb (limits.limitGrPeakCapDb), true, false);

            if (enforced (limits.clipDepthCapDb))
                check ((double) clipDepth, limits.clipDepthCapDb, nearLineDb (limits.clipDepthCapDb), false, false);
        }

        return verdict;
    }

    //==============================================================================
    float GlazeGuard::getProtectionDb() const noexcept   { return protectionDb; }
    bool GlazeGuard::isRetreating() const noexcept       { return retreating; }
    bool GlazeGuard::isRecovering() const noexcept       { return recovering; }
    bool GlazeGuard::isPlanLimited() const noexcept      { return planLimited; }
    bool GlazeGuard::isEngaged() const noexcept          { return retreating || (planLimited && lastOver); }

    namespace
    {
        struct StoredSlot
        {
            float glueMeanDb = 0.0f, limitMeanDb = 0.0f, clipFraction = 0.0f;
            float limitPeakDb = 0.0f, clipDepthDb = 0.0f, protectionDb = 0.0f;
            std::uint8_t over = 0;
            std::uint8_t pad[3] {};
        };

        static_assert (std::is_trivially_copyable_v<StoredSlot>, "guard slots pack by memcpy");
    }

    std::unique_ptr<juce::XmlElement> GlazeGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("BUDGET_GUARD");
        xmlio::Writer writer (*xml);
        writer.field ("protection", protection);
        writer.field ("protectionDb", protectionDb);
        writer.field ("minimumDb", minimumDb);
        writer.field ("planLimited", planLimited);
        writer.field ("lastOver", lastOver);
        writer.field ("lastOvershootDb", lastOvershootDb);
        writer.field ("safeSlots", safeSlots);
        writer.field ("writeIndex", writeIndex);
        writer.field ("historyCount", historyCount);
        writer.field ("statsCount", statsCount);
        writer.field ("overSlotsInHistory", overSlotsInHistory);
        writer.field ("partGlueSum", partGlueSum);
        writer.field ("partLimitSum", partLimitSum);
        writer.field ("partClipSum", partClipSum);
        writer.field ("partLimitPeak", partLimitPeak);
        writer.field ("partClipDepth", partClipDepth);
        writer.field ("partSamples", partSamples);
        writer.field ("sumGlue", sumGlue);
        writer.field ("sumLimit", sumLimit);
        writer.field ("sumClip", sumClip);
        writer.field ("glueAboveCap", glueAboveCap);
        writer.field ("glueAboveNear", glueAboveNear);
        writer.field ("limitAboveCap", limitAboveCap);
        writer.field ("limitAboveNear", limitAboveNear);
        const auto cause = (int) lastCause;
        writer.field ("lastCause", cause);

        std::array<StoredSlot, (std::size_t) slotCapacity> packed {};

        for (int i = 0; i < slotCapacity; ++i)
        {
            const auto& slot = slots[(std::size_t) i];
            auto& stored = packed[(std::size_t) i];
            stored.glueMeanDb = slot.glueMeanDb;
            stored.limitMeanDb = slot.limitMeanDb;
            stored.clipFraction = slot.clipFraction;
            stored.limitPeakDb = slot.limitPeakDb;
            stored.clipDepthDb = slot.clipDepthDb;
            stored.protectionDb = slot.protectionDb;
            stored.over = slot.over ? 1 : 0;
        }

        xml->setAttribute ("slots", juce::Base64::toBase64 (packed.data(), sizeof (packed)));
        return xml;
    }

    bool GlazeGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "BUDGET_GUARD")
            return false;

        auto copy = *this;
        auto cause = (int) copy.lastCause;
        xmlio::Reader reader (xml);
        reader.field ("protection", copy.protection);
        reader.field ("protectionDb", copy.protectionDb);
        reader.field ("minimumDb", copy.minimumDb);
        reader.field ("planLimited", copy.planLimited);
        reader.field ("lastOver", copy.lastOver);
        reader.field ("lastOvershootDb", copy.lastOvershootDb);
        reader.field ("safeSlots", copy.safeSlots);
        reader.field ("writeIndex", copy.writeIndex);
        reader.field ("historyCount", copy.historyCount);
        reader.field ("statsCount", copy.statsCount);
        reader.field ("overSlotsInHistory", copy.overSlotsInHistory);
        reader.field ("partGlueSum", copy.partGlueSum);
        reader.field ("partLimitSum", copy.partLimitSum);
        reader.field ("partClipSum", copy.partClipSum);
        reader.field ("partLimitPeak", copy.partLimitPeak);
        reader.field ("partClipDepth", copy.partClipDepth);
        reader.field ("partSamples", copy.partSamples);
        reader.field ("sumGlue", copy.sumGlue);
        reader.field ("sumLimit", copy.sumLimit);
        reader.field ("sumClip", copy.sumClip);
        reader.field ("glueAboveCap", copy.glueAboveCap);
        reader.field ("glueAboveNear", copy.glueAboveNear);
        reader.field ("limitAboveCap", copy.limitAboveCap);
        reader.field ("limitAboveNear", copy.limitAboveNear);
        reader.field ("lastCause", cause);

        juce::MemoryOutputStream packed;
        if (! reader.ok || cause < 0 || cause > (int) GuardCause::psr
            || copy.writeIndex < 0 || copy.writeIndex >= slotCapacity
            || copy.historyCount < 0 || copy.historyCount > slotCapacity
            || copy.statsCount < 0 || copy.statsCount > copy.historyCount
            || ! juce::Base64::convertFromBase64 (packed, xml.getStringAttribute ("slots"))
            || packed.getDataSize() != sizeof (std::array<StoredSlot, (std::size_t) slotCapacity>))
            return false;

        std::array<StoredSlot, (std::size_t) slotCapacity> stored {};
        std::memcpy (stored.data(), packed.getData(), packed.getDataSize());

        for (int i = 0; i < slotCapacity; ++i)
        {
            auto& slot = copy.slots[(std::size_t) i];
            const auto& item = stored[(std::size_t) i];
            slot.glueMeanDb = item.glueMeanDb;
            slot.limitMeanDb = item.limitMeanDb;
            slot.clipFraction = item.clipFraction;
            slot.limitPeakDb = item.limitPeakDb;
            slot.clipDepthDb = item.clipDepthDb;
            slot.protectionDb = item.protectionDb;
            slot.over = item.over != 0;
        }

        copy.lastCause = static_cast<GuardCause> (cause);
        copy.retreating = copy.recovering = copy.changedThisBlock = false;
        *this = copy;
        return true;
    }
}
