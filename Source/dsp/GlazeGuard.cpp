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
        clear();
    }

    void GlazeGuard::clear() noexcept
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
    void GlazeGuard::releaseOutput() noexcept
    {
        protection = 0.0;
        protectionDb = 0.0f;
        retreating = recovering = changedThisBlock = false;
        planLimited = false;
    }

    GuardOutput GlazeGuard::process (const ModuleStats& stats) noexcept
    {
        retreating = recovering = changedThisBlock = false;

        // 静音、停播（有效音乐门限以下）整块不计：窗口、计时、g_保护全都停住。
        if (! limitsSet || ! stats.musicActive || stats.numSamples <= 0)
            return budgetOutput();

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
        return budgetOutput();
    }

    GuardOutput GlazeGuard::budgetOutput() const noexcept
    {
        GuardOutput out;
        out.reliefDb = protectionDb;
        const auto atFloor = planLimited;
        out.engaged = retreating || atFloor;
        out.cause = out.engaged || protectionDb < 0.0f ? GuardCause::budget : GuardCause::none;
        return out;
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

        slots[(size_t) indexFromNewest (0)].over = verdict.over;
        overSlotsInHistory += verdict.over ? 1 : 0;

        if (verdict.over)
        {
            safeSlots = 0;

            // 本时隙开始时已满足「3 s 内累计超出 ≥ 1 s」且此刻仍超出：按 1 dB/s 退让这一格。
            if (overBefore >= triggerSlots)
            {
                protection = juce::jmax ((double) minimumDb, protection - retreatStepDb);

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
            // 本时隙开始时已有连续 ≥ 5 s 安全音乐：按 0.3 dB/s 恢复这一格（只回到 0，不去找更高响度）。
            if (safeSlots >= recoverSlots && protection < 0.0)
            {
                protection = juce::jmin (0.0, protection + recoverStepDb);

                if (protection > (double) minimumDb)
                    planLimited = false;
            }

            safeSlots = juce::jmin (safeSlots + 1, INT_MAX / 2);
        }
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

        auto check = [&verdict] (double value, float cap, float nearLine)
        {
            if (value > (double) cap)       verdict.over = true;
            if (value > (double) nearLine)  verdict.near = true;
        };

        if (statsCount > 0)
        {
            const auto count = (double) statsCount;

            if (enforced (limits.glueGrMeanCapDb))
                check (sumGlue / count, limits.glueGrMeanCapDb, nearLineDb (limits.glueGrMeanCapDb));

            if (enforced (limits.limitGrMeanCapDb))
                check (sumLimit / count, limits.limitGrMeanCapDb, nearLineDb (limits.limitGrMeanCapDb));

            if (enforced (limits.clipActivityCap))
                check (sumClip / count, limits.clipActivityCap, nearLineFraction (limits.clipActivityCap));

            // p95 > 上限 ⟺ 超过上限的时隙 > 5 %（回差线同理）。
            const auto allowed = p95ExceedFraction * count;

            if (enforced (limits.glueGrP95CapDb))
            {
                verdict.over = verdict.over || (double) glueAboveCap > allowed;
                verdict.near = verdict.near || (double) glueAboveNear > allowed;
            }

            if (enforced (limits.limitGrP95CapDb))
            {
                verdict.over = verdict.over || (double) limitAboveCap > allowed;
                verdict.near = verdict.near || (double) limitAboveNear > allowed;
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
                check (limitPeak, limits.limitGrPeakCapDb, nearLineDb (limits.limitGrPeakCapDb));

            if (enforced (limits.clipDepthCapDb))
                check (clipDepth, limits.clipDepthCapDb, nearLineDb (limits.clipDepthCapDb));
        }

        return verdict;
    }

    //==============================================================================
    namespace
    {
        constexpr const char* budgetTag = "BUDGET_GUARD";

        void writeU32 (juce::MemoryOutputStream& out, std::uint32_t value)
        {
            const unsigned char bytes[4] = {
                (unsigned char) (value & 0xffu),
                (unsigned char) ((value >> 8) & 0xffu),
                (unsigned char) ((value >> 16) & 0xffu),
                (unsigned char) ((value >> 24) & 0xffu)
            };
            out.write (bytes, 4);
        }

        void writeF32 (juce::MemoryOutputStream& out, float value)
        {
            std::uint32_t bits = 0;
            std::memcpy (&bits, &value, sizeof (bits));
            writeU32 (out, bits);
        }

        bool readU32 (const juce::MemoryBlock& block, int& cursor, std::uint32_t& value)
        {
            if (cursor < 0 || cursor + 4 > (int) block.getSize())
                return false;

            const auto* bytes = static_cast<const unsigned char*> (block.getData());
            value = (std::uint32_t) bytes[cursor]
                  | ((std::uint32_t) bytes[cursor + 1] << 8)
                  | ((std::uint32_t) bytes[cursor + 2] << 16)
                  | ((std::uint32_t) bytes[cursor + 3] << 24);
            cursor += 4;
            return true;
        }

        bool readF32 (const juce::MemoryBlock& block, int& cursor, float& value)
        {
            std::uint32_t bits = 0;

            if (! readU32 (block, cursor, bits))
                return false;

            std::memcpy (&value, &bits, sizeof (value));
            return true;
        }
    }

    std::unique_ptr<juce::XmlElement> GlazeGuard::createStateXml() const
    {
        juce::MemoryOutputStream raw;
        writeF32 (raw, (float) protection);
        writeF32 (raw, protectionDb);
        writeU32 (raw, (std::uint32_t) writeIndex);
        writeU32 (raw, (std::uint32_t) historyCount);
        writeU32 (raw, (std::uint32_t) statsCount);
        writeU32 (raw, (std::uint32_t) safeSlots);
        writeU32 (raw, (std::uint32_t) partSamples);
        writeF32 (raw, (float) partGlueSum);
        writeF32 (raw, (float) partLimitSum);
        writeF32 (raw, (float) partClipSum);
        writeF32 (raw, partLimitPeak);
        writeF32 (raw, partClipDepth);

        for (const auto& slot : slots)
        {
            writeF32 (raw, slot.glueMeanDb);
            writeF32 (raw, slot.limitMeanDb);
            writeF32 (raw, slot.clipFraction);
            writeF32 (raw, slot.limitPeakDb);
            writeF32 (raw, slot.clipDepthDb);
            writeF32 (raw, slot.protectionDb);
            writeU32 (raw, slot.over ? 1u : 0u);
        }

        auto xml = std::make_unique<juce::XmlElement> (budgetTag);
        xmlio::Writer writer (*xml);
        const auto protectionBits = protection;
        writer.field ("protection", protectionBits);
        writer.field ("retreating", retreating);
        writer.field ("recovering", recovering);
        writer.field ("planLimited", planLimited);
        writer.field ("limitsSet", limitsSet);
        xml->setAttribute ("slots", juce::Base64::toBase64 (raw.getData(), raw.getDataSize()));
        return xml;
    }

    bool GlazeGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (! xml.hasTagName (budgetTag))
            return false;

        juce::MemoryOutputStream decoded;

        if (! juce::Base64::convertFromBase64 (decoded, xml.getStringAttribute ("slots")))
            return false;

        juce::MemoryBlock block (decoded.getData(), decoded.getDataSize());
        auto cursor = 0;
        float protectionFloat = 0.0f;
        float savedProtectionDb = 0.0f;
        std::uint32_t savedWrite = 0, savedHistory = 0, savedStats = 0, savedSafe = 0, savedPartSamples = 0;
        float glue = 0.0f, limit = 0.0f, clip = 0.0f, peak = 0.0f, depth = 0.0f;

        if (! readF32 (block, cursor, protectionFloat) || ! readF32 (block, cursor, savedProtectionDb)
            || ! readU32 (block, cursor, savedWrite) || ! readU32 (block, cursor, savedHistory)
            || ! readU32 (block, cursor, savedStats) || ! readU32 (block, cursor, savedSafe)
            || ! readU32 (block, cursor, savedPartSamples)
            || ! readF32 (block, cursor, glue) || ! readF32 (block, cursor, limit)
            || ! readF32 (block, cursor, clip) || ! readF32 (block, cursor, peak)
            || ! readF32 (block, cursor, depth))
            return false;

        if ((int) savedWrite < 0 || (int) savedWrite >= slotCapacity
            || (int) savedHistory < 0 || (int) savedHistory > slotCapacity
            || (int) savedStats < 0 || (int) savedStats > (int) savedHistory
            || (int) savedPartSamples < 0)
            return false;

        std::array<Slot, (std::size_t) slotCapacity> loaded {};

        for (auto& slot : loaded)
        {
            std::uint32_t over = 0;

            if (! readF32 (block, cursor, slot.glueMeanDb) || ! readF32 (block, cursor, slot.limitMeanDb)
                || ! readF32 (block, cursor, slot.clipFraction) || ! readF32 (block, cursor, slot.limitPeakDb)
                || ! readF32 (block, cursor, slot.clipDepthDb) || ! readF32 (block, cursor, slot.protectionDb)
                || ! readU32 (block, cursor, over))
                return false;

            slot.over = over != 0;
        }

        xmlio::Reader reader (xml);
        double savedProtection = 0.0;
        bool savedRetreating = false, savedRecovering = false, savedLimited = false, savedLimits = false;
        reader.field ("protection", savedProtection);
        reader.field ("retreating", savedRetreating);
        reader.field ("recovering", savedRecovering);
        reader.field ("planLimited", savedLimited);
        reader.field ("limitsSet", savedLimits);

        if (! reader.ok || ! std::isfinite (savedProtection))
            return false;

        protection = savedProtection;
        protectionDb = (float) protection;
        retreating = savedRetreating;
        recovering = savedRecovering;
        planLimited = savedLimited;
        limitsSet = savedLimits;
        writeIndex = (int) savedWrite;
        historyCount = (int) savedHistory;
        statsCount = (int) savedStats;
        safeSlots = (int) savedSafe;
        partSamples = (int) savedPartSamples;
        partGlueSum = glue;
        partLimitSum = limit;
        partClipSum = clip;
        partLimitPeak = peak;
        partClipDepth = depth;
        slots = loaded;
        recomputeSums();
        overSlotsInHistory = 0;

        for (int age = 0; age < historyCount; ++age)
            if (slots[(size_t) indexFromNewest (age)].over)
                ++overSlotsInHistory;

        return true;
    }

    float GlazeGuard::getProtectionDb() const noexcept   { return protectionDb; }
    bool GlazeGuard::isRetreating() const noexcept       { return retreating; }
    bool GlazeGuard::isRecovering() const noexcept       { return recovering; }
    bool GlazeGuard::isPlanLimited() const noexcept      { return planLimited; }
    bool GlazeGuard::isEngaged() const noexcept          { return protectionDb != 0.0f || changedThisBlock; }
}
