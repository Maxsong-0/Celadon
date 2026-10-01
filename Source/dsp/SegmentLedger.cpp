/*
  ==============================================================================

    SegmentLedger.cpp
    T-038 House 档按小节记账（口径见 SegmentLedger.h）。

  ==============================================================================
*/

#include "SegmentLedger.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdlib>

namespace celadon::glaze
{
    namespace
    {
        // 向下取整的整数除法（宿主位置可以是负数：预备拍）。
        std::int64_t floorDivide (std::int64_t value, std::int64_t divisor) noexcept
        {
            auto quotient = value / divisor;

            if ((value % divisor != 0) && ((value < 0) != (divisor < 0)))
                --quotient;

            return quotient;
        }

        bool validDenominator (int denominator) noexcept
        {
            return denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8
                || denominator == 16 || denominator == 32 || denominator == 64;
        }

        double energyToLufs (double meanSquare) noexcept
        {
            return -0.691 + 10.0 * std::log10 (meanSquare);   // 调用方保证 meanSquare > 0（同分析器的 energyToLufs）
        }

        // 加权直方图的第 fraction 百分位：累计权重第一次达到 fraction × 总权重的那一格（相对 1e-9 的容差，免得等权时
        // 「恰好达到」被求和的舍入挡在下一格）。total ≤ 0 时返回 −1。
        template <size_t N>
        int weightedPercentileBin (const std::array<double, N>& weights, double total, double fraction) noexcept
        {
            if (! (total > 0.0))
                return -1;

            const auto target = fraction * total - 1.0e-9 * total;
            double cumulative = 0.0;

            for (size_t bin = 0; bin < N; ++bin)
            {
                cumulative += weights[bin];

                if (weights[bin] > 0.0 && cumulative >= target)
                    return (int) bin;
            }

            for (size_t bin = N; bin > 0; --bin)   // 舍入让最后也没达到：取有权重的最高一格
                if (weights[bin - 1] > 0.0)
                    return (int) (bin - 1);

            return -1;
        }
    }

    //==============================================================================
    void SegmentLedger::prepare (double newSampleRate, int newSubBlockLength)
    {
        sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
        positionTolerance = juce::jmax ((std::int64_t) 2, (std::int64_t) std::llround (positionToleranceSeconds * sampleRate));
        setSubBlockLength (newSubBlockLength);
        setConstants (constants);   // 2 秒段的样本数随采样率
        store.assign ((size_t) maxSegments, Segment {});
        reset (0);
    }

    void SegmentLedger::setSubBlockLength (int newSubBlockLength) noexcept
    {
        subBlockLength = juce::jmax (1, newSubBlockLength);
    }

    void SegmentLedger::setConstants (const HouseConstants& newConstants) noexcept
    {
        constants = newConstants;

        // 坏值按出厂值（HouseConstants 的默认值就是出厂值）；只防坏快照，合法范围内原样用。
        const HouseConstants factory;
        gateWidthLu = std::isfinite (constants.gateWidthLu) && constants.gateWidthLu >= 0.0f ? constants.gateWidthLu : factory.gateWidthLu;
        lowGateFactor = std::isfinite (constants.lowGateFactor) && constants.lowGateFactor >= 0.0f
                            ? juce::jmin (1.0f, constants.lowGateFactor) : factory.lowGateFactor;
        const auto seconds = std::isfinite (constants.fallbackSegmentSeconds)
                                 ? juce::jlimit (minimumBarSeconds, maximumBarSeconds, constants.fallbackSegmentSeconds)
                                 : factory.fallbackSegmentSeconds;
        fallbackSamples = juce::jmax ((std::int64_t) 1, (std::int64_t) std::llround (seconds * sampleRate));

        if (! summary.valid)
        {
            summary.gateWidthLu = gateWidthLu;   // 还没有门控结果：快照值跟着新常数（有结果时下次重算门控再换）
            summary.lowGateFactor = lowGateFactor;
        }
    }

    void SegmentLedger::reset (std::int64_t newAcceptFrom) noexcept
    {
        regime = Regime {};
        anchored = false;
        newestIndex = 0;
        acceptFrom = newAcceptFrom;
        ++epoch;

        poolUsed.fill (false);
        openHead = openCount = 0;

        numStored = 0;
        storeFull = false;
        lastStoredSlot = -1;

        summary = HouseGateSummary {};
        summary.gateWidthLu = gateWidthLu;
        summary.lowGateFactor = lowGateFactor;
        lowReferenceValid = false;
    }

    //==============================================================================
    // 一、分段
    //==============================================================================
    SegmentLedger::Regime SegmentLedger::regimeFor (const HostInfo& host) const noexcept
    {
        Regime r;
        const auto playing = ! host.hasPlayState || host.isPlaying;

        if (playing && host.hasTempo && std::isfinite (host.bpm) && host.bpm > 0.0 && host.hasPpq && std::isfinite (host.ppqPosition))
        {
            auto numerator = 4, denominator = 4;

            if (host.hasTimeSignature && host.timeSignatureNumerator >= 1 && host.timeSignatureNumerator <= 64
                && validDenominator (host.timeSignatureDenominator))
            {
                numerator = host.timeSignatureNumerator;
                denominator = host.timeSignatureDenominator;
            }

            const auto quarters = (double) numerator * 4.0 / (double) denominator;
            const auto samplesPerQuarter = sampleRate * 60.0 / host.bpm;
            const auto samplesPerBar = quarters * samplesPerQuarter;

            if (std::isfinite (samplesPerBar) && samplesPerBar >= minimumBarSeconds * sampleRate
                && samplesPerBar <= maximumBarSeconds * sampleRate)
            {
                r.mode = SegmentMode::bars;
                r.positioned = true;
                r.bpm = host.bpm;
                r.samplesPerQuarter = samplesPerQuarter;
                r.quartersPerBar = quarters;
                r.samplesPerSegment = samplesPerBar;
                r.numerator = numerator;
                r.denominator = denominator;
                return r;
            }
        }

        // 拿不到拍点：每 fallbackSegmentSeconds 秒一段；在播且有位置时按宿主位置分格（可去重）。
        r.mode = SegmentMode::seconds;
        r.positioned = playing && host.hasPosition;
        r.segmentSamples = fallbackSamples;
        r.samplesPerSegment = (double) fallbackSamples;
        return r;
    }

    bool SegmentLedger::consistent (const Regime& wanted, std::int64_t blockStart, const HostInfo& host) const noexcept
    {
        if (wanted.mode != regime.mode || wanted.positioned != regime.positioned)
            return false;

        if (wanted.mode == SegmentMode::bars)
        {
            if (wanted.numerator != regime.numerator || wanted.denominator != regime.denominator
                || std::abs (wanted.bpm - regime.bpm) > tempoTolerance * regime.bpm)
                return false;

            const auto predicted = regime.anchorPpq + (double) (blockStart - regime.anchorTime) / regime.samplesPerQuarter;
            return std::abs (host.ppqPosition - predicted) * regime.samplesPerQuarter <= (double) positionTolerance;
        }

        if (wanted.segmentSamples != regime.segmentSamples)
            return false;

        if (wanted.positioned)
        {
            const auto predicted = regime.anchorPosition + (blockStart - regime.anchorTime);
            return std::llabs (host.positionSamples - predicted) <= positionTolerance;
        }

        return true;
    }

    std::int64_t SegmentLedger::boundary (std::int64_t index) const noexcept
    {
        if (regime.mode == SegmentMode::bars)
            return regime.originSample + (std::int64_t) std::llround ((double) index * regime.samplesPerSegment);

        return regime.originSample + index * regime.segmentSamples;
    }

    std::int64_t SegmentLedger::keyOf (std::int64_t index) const noexcept
    {
        if (regime.mode == SegmentMode::bars)
            return (std::int64_t) std::llround ((regime.originPpq + (double) index * regime.quartersPerBar) * keyScale);

        return regime.positioned ? regime.originCell + index : 0;
    }

    // time 所在的段在当前锚点下的序号（time 不早于第 0 段的起点）。整数边界按 round 推，先估再按边界修正一两步。
    std::int64_t SegmentLedger::indexAt (std::int64_t time) const noexcept
    {
        if (regime.mode == SegmentMode::bars)
        {
            auto index = juce::jmax ((std::int64_t) 0, (std::int64_t) std::floor ((double) (time - regime.originSample) / regime.samplesPerSegment));

            while (boundary (index + 1) <= time)
                ++index;

            while (index > 0 && boundary (index) > time)
                --index;

            return index;
        }

        return juce::jmax ((std::int64_t) 0, (time - regime.originSample) / regime.segmentSamples);
    }

    void SegmentLedger::anchor (const Regime& wanted, std::int64_t blockStart, const HostInfo& host) noexcept
    {
        regime = wanted;
        regime.anchorTime = blockStart;

        if (regime.mode == SegmentMode::bars)
        {
            // 小节起点：宿主给了 ppqLastBarStart（且不晚于此刻）就用它，跨过的整小节补上（编排器把一个宿主块切成几片时，后几片的 PPQ
            // 已按片的偏移推过，小节起点还是块起点那一小节的）；否则按 PPQ ÷ 每小节拍数推（假定从 0 起拍号没变）。
            const auto ppq = host.ppqPosition;
            const auto quarters = regime.quartersPerBar;
            auto barStart = std::floor (ppq / quarters + 1.0e-9) * quarters;

            if (host.hasBarStart && std::isfinite (host.ppqLastBarStart))
            {
                const auto since = ppq - host.ppqLastBarStart;

                if (since >= -1.0e-6 && since < 1024.0 * quarters)
                    barStart = host.ppqLastBarStart + std::floor (since / quarters + 1.0e-9) * quarters;
            }

            const auto offset = juce::jlimit (0.0, regime.samplesPerSegment - 1.0, (ppq - barStart) * regime.samplesPerQuarter);
            regime.anchorPpq = ppq;
            regime.originPpq = barStart;
            regime.originSample = blockStart - (std::int64_t) std::llround (offset);
        }
        else if (regime.positioned)
        {
            const auto cell = floorDivide (host.positionSamples, regime.segmentSamples);
            regime.anchorPosition = host.positionSamples;
            regime.originCell = cell;
            regime.originSample = blockStart - (host.positionSamples - cell * regime.segmentSamples);
        }
        else
        {
            regime.originSample = blockStart;
        }

        anchored = true;

        const auto index = indexAt (blockStart);
        const auto key = keyOf (index);

        if (openCount > 0)
        {
            auto& open = newest();

            if (open.start <= blockStart && blockStart < open.end && regime.positioned && open.positioned
                && open.mode == regime.mode && open.key == key)
            {
                // 同一段（例如小节中途换了速度）：接着记，段尾与标称长度按新锚点。
                open.end = boundary (index + 1);
                open.nominalLength = boundary (index + 1) - boundary (index);
                newestIndex = index;
                return;
            }

            if (open.end > blockStart)
                open.end = blockStart;   // 截断：此刻起的数据归新段
        }

        openSegment (index, blockStart);
    }

    void SegmentLedger::openSegment (std::int64_t index, std::int64_t start) noexcept
    {
        if (openCount == maxOpenSegments)
            finalizeOldest();   // 只在一块跨过 8 段以上的病态块长时发生

        auto slot = 0;

        while (poolUsed[(size_t) slot])
            ++slot;

        auto& segment = pool[(size_t) slot];
        segment = Segment {};
        segment.mode = regime.mode;
        segment.positioned = regime.positioned;
        segment.key = keyOf (index);
        segment.start = start;
        segment.end = boundary (index + 1);
        segment.nominalLength = boundary (index + 1) - boundary (index);
        segment.epoch = epoch;

        poolUsed[(size_t) slot] = true;
        order[(size_t) ((openHead + openCount) % maxOpenSegments)] = slot;
        ++openCount;
        newestIndex = index;
    }

    void SegmentLedger::beginBlock (std::int64_t blockStart, int numSamples, const HostInfo& host) noexcept
    {
        if (numSamples <= 0 || store.empty())
            return;

        const auto wanted = regimeFor (host);

        if (! anchored || ! consistent (wanted, blockStart, host))
            anchor (wanted, blockStart, host);
        else if (openCount == 0)
            openSegment (indexAt (blockStart), blockStart);   // 上一段恰好在块边界上定稿：按原锚点接着开，不重新锚定（重新锚定会让边界随分块变）

        // 开着的段要盖住这一块的最后一个样本。
        const auto until = blockStart + (std::int64_t) numSamples;

        while (openCount > 0 && newest().end < until)
            openSegment (newestIndex + 1, newest().end);
    }

    void SegmentLedger::endEpoch (std::int64_t newAcceptFrom) noexcept
    {
        while (openCount > 0)
            finalizeOldest();

        anchored = false;
        acceptFrom = newAcceptFrom;
        ++epoch;
    }

    //==============================================================================
    // 二、记账
    //==============================================================================
    SegmentLedger::Segment* SegmentLedger::openSegmentAt (std::int64_t time, int& hint) noexcept
    {
        if (hint >= 0 && hint < maxOpenSegments && poolUsed[(size_t) hint])
        {
            auto& cached = pool[(size_t) hint];

            if (cached.start <= time && time < cached.end)
                return &cached;
        }

        for (int k = 0; k < openCount; ++k)
        {
            const auto slot = order[(size_t) ((openHead + k) % maxOpenSegments)];
            auto& segment = pool[(size_t) slot];

            if (segment.start <= time && time < segment.end)
            {
                hint = slot;
                return &segment;
            }
        }

        return nullptr;
    }

    //==============================================================================
    // 三、定稿
    //==============================================================================
    bool SegmentLedger::finalizeBefore (std::int64_t completeBefore) noexcept
    {
        auto any = false;

        while (openCount > 0 && pool[(size_t) order[(size_t) openHead]].end <= completeBefore)
        {
            finalizeOldest();
            any = true;
        }

        return any;
    }

    void SegmentLedger::deriveSegment (Segment& segment) const noexcept
    {
        segment.effectiveSeconds = (double) segment.effectiveSubBlocks * (double) subBlockLength / sampleRate;
        segment.heard = segment.effectiveSubBlocks > 0
                     && segment.effectiveSeconds >= heardFraction * (double) segment.nominalLength / sampleRate;

        const auto meanSquare = segment.effectiveSubBlocks > 0 ? segment.effectiveEnergy / (double) segment.effectiveSubBlocks : 0.0;
        segment.loudnessValid = std::isfinite (meanSquare) && meanSquare > 0.0;
        segment.loudnessLufs = segment.loudnessValid ? (float) energyToLufs (meanSquare) : 0.0f;

        double total = 0.0, low = 0.0;

        for (size_t b = 0; b < (size_t) numBands; ++b)
        {
            total += segment.powerSum[b];

            if (b <= (size_t) lowShareLastBand)
                low += segment.powerSum[b];
        }

        segment.lowShareValid = segment.frameSeconds >= minimumLowShareSeconds && std::isfinite (total) && total > 0.0;
        segment.lowShare = segment.lowShareValid ? (float) (low / total) : 0.0f;
        segment.full = false;
    }

    int SegmentLedger::commit (const Segment& segment) noexcept
    {
        // 没有一个有效音频子块的实例（静音、底噪、截断后只剩几个样本、整段落在不连续点的丢弃区里）：不进账本，也不覆盖旧值——
        // 它对门控与画像都没有贡献；宿主停着、插件照常收静音时，不按位置的 2 秒段也就不会把账本塞满。
        if (segment.effectiveSubBlocks <= 0)
            return -1;

        if (segment.positioned)
        {
            for (int i = 0; i < numStored; ++i)
            {
                auto& old = store[(size_t) i];

                if (old.positioned && old.mode == segment.mode && old.key == segment.key)
                {
                    // 同一位置再听一遍：新实例「已听」或有效音频不比旧的少就覆盖（不重复计入）；只听了一点就不毁掉完整的旧值。
                    if (segment.heard || segment.effectiveSeconds >= old.effectiveSeconds)
                        old = segment;

                    return i;
                }
            }
        }

        if (numStored >= maxSegments)
        {
            storeFull = true;
            return -1;
        }

        store[(size_t) numStored] = segment;
        return numStored++;
    }

    bool SegmentLedger::passesGate (bool heard, bool loudnessValid, float loudnessLufs, bool lowShareValid, float lowShare) const noexcept
    {
        return summary.valid && lowReferenceValid && heard && loudnessValid && lowShareValid
            && loudnessLufs >= summary.referenceLufs - gateWidthLu
            && lowShare >= summary.lowShareReference * lowGateFactor;
    }

    void SegmentLedger::finalizeOldest() noexcept
    {
        const auto slot = order[(size_t) openHead];
        auto& segment = pool[(size_t) slot];
        openHead = (openHead + 1) % maxOpenSegments;
        --openCount;

        deriveSegment (segment);

        // 新响段：比「这一段并入之前」的 S_ref 高 ΔG 以上、且过了那时的低频门（设计规格第 3.2 节）。
        const auto surge = summary.valid && lowReferenceValid && segment.heard && segment.loudnessValid && segment.lowShareValid
                        && segment.loudnessLufs >= summary.referenceLufs + gateWidthLu
                        && segment.lowShare >= summary.lowShareReference * lowGateFactor;

        const auto stored = commit (segment);
        poolUsed[(size_t) slot] = false;
        ++finalizedCount;

        if (stored >= 0)
        {
            lastStoredSlot = stored;
            lastStoredEpoch = segment.epoch;
            recomputeGate();
        }

        if (surge)
            ++surgeCount;

        Event event;
        event.sequence = eventTotal;
        event.epoch = segment.epoch;
        event.stored = stored >= 0;
        event.heard = segment.heard;
        event.full = stored >= 0 && store[(size_t) stored].full;
        event.surge = surge;
        event.loudnessValid = segment.loudnessValid;
        event.loudnessLufs = segment.loudnessLufs;
        event.lowShareValid = segment.lowShareValid;
        event.lowShare = segment.lowShare;
        events[(size_t) (eventTotal % (std::uint64_t) eventCapacity)] = event;
        ++eventTotal;
    }

    //==============================================================================
    // 四、门控
    //==============================================================================
    void SegmentLedger::recomputeGate() noexcept
    {
        auto& s = summary;
        s = HouseGateSummary {};
        s.mode = getMode();
        s.gateWidthLu = gateWidthLu;
        s.lowGateFactor = lowGateFactor;
        lowReferenceValid = false;

        // S_ref：已听各段段响度的第 95 百分位，按有效时长加权。
        loudnessWeights.fill (0.0);
        double loudnessTotal = 0.0;

        for (int i = 0; i < numStored; ++i)
        {
            auto& segment = store[(size_t) i];
            segment.full = false;

            if (! segment.heard)
                continue;

            ++s.heardSegments;
            s.heardSeconds += segment.effectiveSeconds;

            if (! segment.loudnessValid)
                continue;

            const auto bin = juce::jlimit (0, loudnessBins - 1,
                                           (int) std::floor (((double) segment.loudnessLufs - loudnessHistogramMinLufs) * (double) loudnessBinsPerLu));
            loudnessWeights[(size_t) bin] += segment.effectiveSeconds;
            loudnessTotal += segment.effectiveSeconds;
        }

        const auto loudnessBin = weightedPercentileBin (loudnessWeights, loudnessTotal, gatePercentile);

        if (loudnessBin < 0)
            return;   // 还没有已听的段：门控没有结果（valid = false）

        s.valid = true;
        s.referenceLufs = (float) (loudnessHistogramMinLufs + ((double) loudnessBin + 0.5) / (double) loudnessBinsPerLu);
        const auto loudnessGate = s.referenceLufs - gateWidthLu;

        // 低频参考：已过响度门各段 lowShare 的第 95 百分位（同样按有效时长加权）。
        lowShareWeights.fill (0.0);
        double lowTotal = 0.0;

        for (int i = 0; i < numStored; ++i)
        {
            const auto& segment = store[(size_t) i];

            if (! (segment.heard && segment.loudnessValid && segment.lowShareValid && segment.loudnessLufs >= loudnessGate))
                continue;

            const auto bin = juce::jlimit (0, lowShareBins - 1, (int) std::floor ((double) segment.lowShare * (double) lowShareBins));
            lowShareWeights[(size_t) bin] += segment.effectiveSeconds;
            lowTotal += segment.effectiveSeconds;
        }

        const auto lowBin = weightedPercentileBin (lowShareWeights, lowTotal, gatePercentile);

        if (lowBin < 0)
            return;   // 过了响度门的段都没有可信的低频占比：没有满编段

        lowReferenceValid = true;
        s.lowShareReference = (float) (((double) lowBin + 0.5) / (double) lowShareBins);

        for (int i = 0; i < numStored; ++i)
        {
            auto& segment = store[(size_t) i];
            segment.full = passesGate (segment.heard, segment.loudnessValid, segment.loudnessLufs, segment.lowShareValid, segment.lowShare);

            if (segment.full)
            {
                ++s.fullSegments;
                s.fullSeconds += segment.effectiveSeconds;
            }
        }
    }

    //==============================================================================
    // 五、读数
    //==============================================================================
    SegmentMode SegmentLedger::getMode() const noexcept
    {
        if (anchored)
            return regime.mode;

        if (openCount > 0)
            return newest().mode;

        return lastStoredSlot >= 0 ? store[(size_t) lastStoredSlot].mode : SegmentMode::none;
    }

    bool SegmentLedger::isCurrentSegmentFull() const noexcept
    {
        if (! summary.valid || ! lowReferenceValid)
            return false;

        if (openCount > 0)
        {
            const auto& open = newest();
            const auto effectiveSeconds = (double) open.effectiveSubBlocks * (double) subBlockLength / sampleRate;

            if (effectiveSeconds >= minimumProvisionalSeconds && open.frameSeconds >= minimumProvisionalSeconds && open.effectiveEnergy > 0.0)
            {
                double total = 0.0, low = 0.0;

                for (size_t b = 0; b < (size_t) numBands; ++b)
                {
                    total += open.powerSum[b];

                    if (b <= (size_t) lowShareLastBand)
                        low += open.powerSum[b];
                }

                if (! (total > 0.0) || ! std::isfinite (total))
                    return false;

                const auto loudness = (float) energyToLufs (open.effectiveEnergy / (double) open.effectiveSubBlocks);
                return passesGate (true, std::isfinite (loudness), loudness, true, (float) (low / total));
            }

            // 这一段刚开始：沿用同一段连续时间里上一段的门控结果（小节交界处不闪断）。
            if (lastStoredSlot >= 0 && lastStoredEpoch == open.epoch)
                return store[(size_t) lastStoredSlot].full;

            return false;
        }

        return lastStoredSlot >= 0 && lastStoredEpoch == epoch && store[(size_t) lastStoredSlot].full;
    }

    double SegmentLedger::getCurrentSegmentSeconds() const noexcept
    {
        return openCount > 0 ? (double) newest().nominalLength / sampleRate : 0.0;
    }

    int SegmentLedger::readEvents (std::uint64_t& cursor, Event* out, int maxCount) const noexcept
    {
        auto lost = false;

        if (cursor > eventTotal)
            cursor = eventTotal;

        if (eventTotal - cursor > (std::uint64_t) eventCapacity)
        {
            cursor = eventTotal - (std::uint64_t) eventCapacity;
            lost = true;
        }

        auto count = 0;

        while (cursor < eventTotal && count < maxCount)
        {
            out[count++] = events[(size_t) (cursor % (std::uint64_t) eventCapacity)];
            ++cursor;
        }

        return lost ? -count : count;
    }
}
