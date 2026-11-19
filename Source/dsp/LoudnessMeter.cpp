/*
  ==============================================================================

    LoudnessMeter.cpp

  ==============================================================================
*/

#include "LoudnessMeter.h"

#include "AtomicHelpers.h"

namespace celadon
{
    namespace
    {
        // BS.1770：响度 = −0.691 + 10·log10(Σ 各声道均方)。
        float energyToLufs (double meanSquare) noexcept
        {
            if (meanSquare <= 0.0)
                return LoudnessMeter::noReading;

            return juce::jmax (LoudnessMeter::noReading, (float) (-0.691 + 10.0 * std::log10 (meanSquare)));
        }
    }

    void LoudnessMeter::designKWeighting (double fs, Biquad& shelfOut, Biquad& highpassOut) noexcept
    {
        const auto pi = juce::MathConstants<double>::pi;

        // 第 1 级：高搁架（模拟头部的声学效应）。
        {
            constexpr double f0 = 1681.974450955533;
            constexpr double gainDb = 3.999843853973347;
            constexpr double q = 0.7071752369554196;

            const auto k  = std::tan (pi * f0 / fs);
            const auto vh = std::pow (10.0, gainDb / 20.0);
            const auto vb = std::pow (vh, 0.4996667741545416);
            const auto a0 = 1.0 + k / q + k * k;

            shelfOut.b0 = (vh + vb * k / q + k * k) / a0;
            shelfOut.b1 = 2.0 * (k * k - vh) / a0;
            shelfOut.b2 = (vh - vb * k / q + k * k) / a0;
            shelfOut.a1 = 2.0 * (k * k - 1.0) / a0;
            shelfOut.a2 = (1.0 - k / q + k * k) / a0;
        }

        // 第 2 级：RLB 高通（约 38 Hz）。分子按标准表保持 1, −2, 1。
        {
            constexpr double f0 = 38.13547087602444;
            constexpr double q = 0.5003270373238773;

            const auto k  = std::tan (pi * f0 / fs);
            const auto a0 = 1.0 + k / q + k * k;

            highpassOut.b0 = 1.0;
            highpassOut.b1 = -2.0;
            highpassOut.b2 = 1.0;
            highpassOut.a1 = 2.0 * (k * k - 1.0) / a0;
            highpassOut.a2 = (1.0 - k / q + k * k) / a0;
        }
    }

    void LoudnessMeter::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate  = spec.sampleRate;
        numChannels = (int) juce::jmax ((juce::uint32) 1, spec.numChannels);

        designKWeighting (sampleRate, shelf, highpass);

        channelStates.assign ((size_t) numChannels, ChannelState {});
        fineHistogram.resize (histogramBins);
        truePeak.prepare (numChannels);
        subBlockLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));

        reset();
    }

    void LoudnessMeter::reset()
    {
        clearFilterStates();
        truePeak.reset();
        loudnessActive = loudnessEnabled.load();
        clearMeasurement();
        truePeakLinear.store (0.0f);

        // 子块序列与样本计数从零开始（链里各表与 Balance 一起 reset，时间戳在同一时间轴上）。
        subBlocksWritten = 0;
        processedSamples = 0;
        restartSubBlockGrid();
        lastBlockPeak = 0.0f;
    }

    void LoudnessMeter::clearFilterStates() noexcept
    {
        for (auto& state : channelStates)
            state = ChannelState {};
    }

    void LoudnessMeter::healFilterStates() noexcept
    {
        // 只清状态已经坏了的声道：另一个声道的滤波器照常连续（两套子块网格先后发现同一个 NaN 时，后一次不会把已经恢复的状态再清一遍）。
        for (auto& state : channelStates)
            if (! (std::isfinite (state.shelfS1) && std::isfinite (state.shelfS2)
                   && std::isfinite (state.highpassS1) && std::isfinite (state.highpassS2)))
                state = ChannelState {};
    }

    void LoudnessMeter::restartSubBlockGrid() noexcept
    {
        ringSamplesInSubBlock = 0;
        ringEnergy = 0.0;
        ringPeak = 0.0f;
    }

    void LoudnessMeter::clearMeasurement() noexcept
    {
        samplesInSubBlock = 0;
        subBlockEnergy = 0.0;
        recentSubBlocks.fill (0.0);
        recentWriteIndex = 0;
        recentCount = 0;
        aboveAbsoluteGate = {};
        histogram.fill ({});
        histogramTopBin = -1;

        // 读数网格从这里重新起：正在累计的子块作废（samplesInSubBlock = 0），它没被暂停碰过；暂停中清表时 process 会在处理样本之前
        // 重新标上。门块本来就要 recentCount 攒够 4 个新子块，连续计数回到「早已攒够」即可。
        pauseTouchedSubBlock = false;
        cleanSubBlocksInRow = gatingSubBlocks;

        shortTermLufs.store (noReading);
        integratedLufs.store (noReading);
    }

    float LoudnessMeter::getAndResetTruePeakDb() noexcept
    {
        const auto peak = truePeakLinear.exchange (0.0f);
        return juce::jmax (noReading, juce::Decibels::gainToDecibels (peak, noReading));
    }

    //==============================================================================
    // v0.6 GLAZE 子块环形缓冲（设计规格-GLAZE-分析器第 2 节）：读者与写者都在音频线程上（先 process，再读），不需要原子量。
    int LoudnessMeter::readSubBlocksSince (std::int64_t& cursor, SubBlock* out, int maxCount) const noexcept
    {
        if (out == nullptr || maxCount <= 0)
            return 0;

        auto from = cursor;
        bool dropped = false;

        // 游标比已完成的子块数还靠后：表被 reset 过而读者没跟着清。从头读，按「丢过」报告（读者据此断开窗口）。
        if (from > subBlocksWritten || from < 0)
        {
            from = 0;
            dropped = true;
        }

        // 落后超过容量：最旧的那些已被覆盖，跳到最旧可用的一个。
        const auto oldestAvailable = juce::jmax ((std::int64_t) 0, subBlocksWritten - (std::int64_t) subBlockRingCapacity);

        if (from < oldestAvailable)
        {
            from = oldestAvailable;
            dropped = true;
        }

        const auto count = (int) juce::jmin ((std::int64_t) maxCount, subBlocksWritten - from);

        for (int i = 0; i < count; ++i)
            out[i] = subBlockRing[(size_t) ((from + i) % subBlockRingCapacity)];

        cursor = from + count;
        return dropped ? -count : count;
    }

    std::int64_t LoudnessMeter::getProcessedSamples() const noexcept
    {
        return processedSamples;
    }

    float LoudnessMeter::getLastBlockTruePeakLinear() const noexcept
    {
        return lastBlockPeak;
    }

    void LoudnessMeter::publishSubBlock() noexcept
    {
        // 自愈（T-032 R10-08）：子块能量非有限 = K 加权状态里进了 NaN / Inf，不清掉就会永远留在里面、之后每个子块都坏。
        // 这个子块按既有约定照样发布（读者——分析器、Listen、校准器、渲染工具——本来就跳过非有限的 meanSquare），下一个子块起干净。
        if (! std::isfinite (ringEnergy))
            healFilterStates();

        auto& slot = subBlockRing[(size_t) (subBlocksWritten % subBlockRingCapacity)];
        slot.meanSquare     = ringEnergy / (double) subBlockLength;
        slot.truePeakLinear = ringPeak;
        slot.endSample      = processedSamples;
        ++subBlocksWritten;

        restartSubBlockGrid();
    }

    //==============================================================================
    void LoudnessMeter::process (const juce::AudioBuffer<float>& buffer)
    {
        if (sampleRate <= 0.0)
            return;

        // 响度计量开关变了：K 加权滤波器状态与统计都从头开始（关掉时两个 LUFS 读数立即是 noReading）。
        // 子块网格也从这一刻重新起：关着时不产生子块，打开后的第一个子块是完整的 100 ms（不混进关之前的半截）。
        const auto shouldMeasureLoudness = loudnessEnabled.load (std::memory_order_relaxed);

        if (shouldMeasureLoudness != loudnessActive)
        {
            loudnessActive = shouldMeasureLoudness;
            clearFilterStates();
            clearMeasurement();
            restartSubBlockGrid();
        }

        // 界面的 Reset 只清表自己的读数；子块序列（GLAZE 用）不受影响。
        if (resetRequested.exchange (false))
            clearMeasurement();

        const auto channelsToUse = juce::jmin (numChannels, buffer.getNumChannels());
        const auto* const* channels = buffer.getArrayOfReadPointers();
        const auto numSamples = buffer.getNumSamples();

        // 真峰值：每个声道交给检测器（按块与逐样本逐位相同、与怎么分段无关），取最大。
        // 各声道的检测器状态互不相干，先算哪个声道不影响结果；jmax 折叠与顺序、分段无关（峰值都 ≥ 0，NaN 峰值总被忽略）。
        float blockPeak = 0.0f;

        if (loudnessActive)
        {
            // 这一块整个在暂停中（链按块调 setIntegrationPaused）：正在累计的读数子块从现在起含暂停中的样本（R8-03）。
            if (integrationPaused)
                pauseTouchedSubBlock = true;

            // 按子块边界切段：每段先算 K 加权响度（逐样本，运算与整块一次算完相同），再把这段交给真峰值检测器取段最大，
            // 段最大同时折进子块峰值与整块峰值。表的读数（短期、整体、真峰值）与不切段逐位相同。
            for (int done = 0; done < numSamples;)
            {
                const auto length = juce::jmin (numSamples - done, subBlockLength - ringSamplesInSubBlock);

                measureLoudness (channels, channelsToUse, done, length);

                float segmentPeak = 0.0f;

                for (int ch = 0; ch < channelsToUse; ++ch)
                    segmentPeak = juce::jmax (segmentPeak, truePeak.processBlockMax (ch, channels[ch] + done, length));

                blockPeak = juce::jmax (blockPeak, segmentPeak);
                ringPeak  = juce::jmax (ringPeak, segmentPeak);

                done                  += length;
                processedSamples      += length;
                ringSamplesInSubBlock += length;

                if (ringSamplesInSubBlock == subBlockLength)
                    publishSubBlock();
            }
        }
        else
        {
            for (int ch = 0; ch < channelsToUse; ++ch)
                blockPeak = juce::jmax (blockPeak, truePeak.processBlockMax (ch, channels[ch], numSamples));

            processedSamples += numSamples;
        }

        lastBlockPeak = blockPeak;
        atomicMax (truePeakLinear, blockPeak);
    }

    void LoudnessMeter::measureLoudness (const float* const* channels, int channelsToUse, int startSample, int numSamples) noexcept
    {
        const auto weight = (channelsToUse == 1 ? 2.0 : 1.0);   // 单声道按双单声道计：能量计两次
        const auto endSample = startSample + numSamples;

        for (int i = startSample; i < endSample; ++i)
        {
            for (int ch = 0; ch < channelsToUse; ++ch)
            {
                const auto x = channels[ch][i];
                auto& state = channelStates[(size_t) ch];

                auto y = kWeight ((double) x, shelf, state.shelfS1, state.shelfS2);
                y = kWeight (y, highpass, state.highpassS1, state.highpassS2);

                // 两个累加器各是一个独立的表达式（与 v0.5 同一写法：编译器在每个表达式里各自决定乘加是否融合，
                // 表自己的 subBlockEnergy 的舍入与改动前逐位相同，改动前后的读数摘要逐位一致，见 IMPL-A1）。
                subBlockEnergy += weight * y * y;
                ringEnergy += weight * y * y;
            }

            if (++samplesInSubBlock == subBlockLength)
                finishSubBlock();
        }
    }

    void LoudnessMeter::finishSubBlock() noexcept
    {
        // 自愈（T-032 R10-08）：这 100 ms 里进了 NaN / Inf——K 加权状态已经坏了（转置 II 型状态与 NaN 相乘还是 NaN），清掉坏的声道，
        // 这个子块整个丢掉：短期窗口与直方图永不吃非有限值，下一个子块起读数照常。改前一个 NaN 就让短期响度从此一直是「无读数」
        //（energyToLufs 把 NaN 能量当 noReading）、整体响度冻结在出事前的值（NaN 门块被绝对门挡掉）；+Inf 能量的门块则让下面
        // floor 转 int 成了未定义行为。门块要 4 个时间上连续的子块，丢掉的这个把连续性断开。
        if (! std::isfinite (subBlockEnergy))
        {
            healFilterStates();
            subBlockEnergy = 0.0;
            samplesInSubBlock = 0;
            cleanSubBlocksInRow = 0;
            pauseTouchedSubBlock = integrationPaused;
            return;
        }

        recentSubBlocks[(size_t) recentWriteIndex] = subBlockEnergy;
        recentWriteIndex = (recentWriteIndex + 1) % shortTermSubBlocks;
        recentCount = juce::jmin (recentCount + 1, shortTermSubBlocks);

        subBlockEnergy = 0.0;
        samplesInSubBlock = 0;

        // 短期：最近 3 s（没满 3 s 时用已有的部分，刚播放就有读数）。暂停中照常——显示此刻听到的信号。
        double shortTermEnergy = 0.0;
        for (int i = 0; i < recentCount; ++i)
            shortTermEnergy += recentSubBlocks[(size_t) i];

        shortTermLufs.store (energyToLufs (shortTermEnergy / (double) (recentCount * subBlockLength)));

        // 整体响度的积分暂停（T-032 R8-03：链走干路时 OUT 表只积分母带）：这个子块有样本是在暂停中处理的，就不让它进任何门块，
        // 连续计数清零——恢复后要 4 个完全在暂停之外的子块才再成门块。下一个子块从此刻起，还在暂停就同样算被碰过。
        const auto touchedByPause = pauseTouchedSubBlock || integrationPaused;
        pauseTouchedSubBlock = integrationPaused;

        if (touchedByPause)
        {
            cleanSubBlocksInRow = 0;
            return;
        }

        cleanSubBlocksInRow = juce::jmin (cleanSubBlocksInRow + 1, gatingSubBlocks);

        // 每 100 ms 形成一个 400 ms 门块（75% 重叠），记进直方图。从未暂停、输入有限时 cleanSubBlocksInRow 恒为 4，与改动前同一判断。
        if (recentCount < gatingSubBlocks || cleanSubBlocksInRow < gatingSubBlocks)
            return;

        double gatingEnergy = 0.0;
        for (int back = 1; back <= gatingSubBlocks; ++back)
        {
            const auto index = (recentWriteIndex - back + shortTermSubBlocks) % shortTermSubBlocks;
            gatingEnergy += recentSubBlocks[(size_t) index];
        }

        const auto meanSquare = gatingEnergy / (double) (gatingSubBlocks * subBlockLength);
        if (meanSquare <= 0.0)
            return;
        const auto blockLufs = -0.691 + 10.0 * std::log10 (meanSquare);
        if (blockLufs <= histogramMinLufs)
            return;

        const auto fine = fineBinAt (blockLufs);
        const auto bin = fine / subdivisions;
        auto& coarse = histogram[(size_t) bin];
        auto& detail = fineHistogram[(size_t) bin];
        if (coarse.count == 0)
            detail.fill ({});   // reset 只清粗格，在本轮第一次用到时清这 16 个小格。
        coarse.add (meanSquare);
        detail[(size_t) (fine % subdivisions)].add (meanSquare);
        aboveAbsoluteGate.add (meanSquare);
        histogramTopBin = juce::jmax (histogramTopBin, bin);

        updateIntegrated();
    }

    int LoudnessMeter::fineBinAt (double lufs) noexcept
    {
        return (int) juce::jlimit (0.0, (double) (histogramBins * subdivisions - 1),
                                   (lufs - histogramMinLufs) * (histogramBinsPerLu * subdivisions));
    }

    void LoudnessMeter::updateIntegrated() noexcept
    {
        // −10 LU 在能量域就是乘 0.1。门限之外的整格无需近似；只有门穿过的小格可能混有两边的门块。
        const auto gateEnergy = 0.1 * (aboveAbsoluteGate.energy / (double) aboveAbsoluteGate.count);
        const auto fine = fineBinAt (-0.691 + 10.0 * std::log10 (gateEnergy));
        const auto bin = fine / subdivisions;
        GateTotals gated;
        if (histogram[(size_t) bin].count > 0)
        {
            const auto& detail = fineHistogram[(size_t) bin];
            const auto edge = fine % subdivisions;
            const auto& boundary = detail[(size_t) edge];
            if (boundary.energy > gateEnergy * (double) boundary.count)
                gated = boundary;
            for (int i = edge + 1; i < subdivisions; ++i)
                gated.add (detail[(size_t) i].energy, detail[(size_t) i].count);
        }
        for (int i = bin + 1; i <= histogramTopBin; ++i)
            gated.add (histogram[(size_t) i].energy, histogram[(size_t) i].count);
        integratedLufs.store (gated.count > 0 ? energyToLufs (gated.energy / (double) gated.count) : noReading);
    }
}
