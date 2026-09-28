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

    void LoudnessMeter::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate  = spec.sampleRate;
        numChannels = (int) juce::jmax ((juce::uint32) 1, spec.numChannels);

        const auto pi = juce::MathConstants<double>::pi;

        // 第 1 级：高搁架（模拟头部的声学效应）。
        {
            constexpr double f0 = 1681.974450955533;
            constexpr double gainDb = 3.999843853973347;
            constexpr double q = 0.7071752369554196;

            const auto k  = std::tan (pi * f0 / sampleRate);
            const auto vh = std::pow (10.0, gainDb / 20.0);
            const auto vb = std::pow (vh, 0.4996667741545416);
            const auto a0 = 1.0 + k / q + k * k;

            shelf.b0 = (vh + vb * k / q + k * k) / a0;
            shelf.b1 = 2.0 * (k * k - vh) / a0;
            shelf.b2 = (vh - vb * k / q + k * k) / a0;
            shelf.a1 = 2.0 * (k * k - 1.0) / a0;
            shelf.a2 = (1.0 - k / q + k * k) / a0;
        }

        // 第 2 级：RLB 高通（约 38 Hz）。分子按标准表保持 1, −2, 1。
        {
            constexpr double f0 = 38.13547087602444;
            constexpr double q = 0.5003270373238773;

            const auto k  = std::tan (pi * f0 / sampleRate);
            const auto a0 = 1.0 + k / q + k * k;

            highpass.b0 = 1.0;
            highpass.b1 = -2.0;
            highpass.b2 = 1.0;
            highpass.a1 = 2.0 * (k * k - 1.0) / a0;
            highpass.a2 = (1.0 - k / q + k * k) / a0;
        }

        channelStates.assign ((size_t) numChannels, ChannelState {});
        truePeak.prepare (numChannels);
        subBlockLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));

        reset();
    }

    void LoudnessMeter::reset()
    {
        for (auto& state : channelStates)
            state = ChannelState {};

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
        histogramEnergy.fill (0.0);
        histogramCount.fill (0);

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

            for (auto& state : channelStates)
                state = ChannelState {};

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
        recentSubBlocks[(size_t) recentWriteIndex] = subBlockEnergy;
        recentWriteIndex = (recentWriteIndex + 1) % shortTermSubBlocks;
        recentCount = juce::jmin (recentCount + 1, shortTermSubBlocks);

        subBlockEnergy = 0.0;
        samplesInSubBlock = 0;

        // 短期：最近 3 s（没满 3 s 时用已有的部分，刚播放就有读数）。
        double shortTermEnergy = 0.0;
        for (int i = 0; i < recentCount; ++i)
            shortTermEnergy += recentSubBlocks[(size_t) i];

        shortTermLufs.store (energyToLufs (shortTermEnergy / (double) (recentCount * subBlockLength)));

        // 每 100 ms 形成一个 400 ms 门块（75% 重叠），记进直方图。
        if (recentCount < gatingSubBlocks)
            return;

        double gatingEnergy = 0.0;
        for (int back = 1; back <= gatingSubBlocks; ++back)
        {
            const auto index = (recentWriteIndex - back + shortTermSubBlocks) % shortTermSubBlocks;
            gatingEnergy += recentSubBlocks[(size_t) index];
        }

        const auto meanSquare = gatingEnergy / (double) (gatingSubBlocks * subBlockLength);
        const auto blockLufs = energyToLufs (meanSquare);

        if (blockLufs < histogramMinLufs)
            return;   // 绝对门：−70 LUFS 以下不计入

        const auto bin = juce::jlimit (0, histogramBins - 1,
                                       (int) std::floor ((blockLufs - histogramMinLufs) * (float) histogramBinsPerLu));
        histogramEnergy[(size_t) bin] += meanSquare;
        ++histogramCount[(size_t) bin];

        updateIntegrated();
    }

    void LoudnessMeter::updateIntegrated() noexcept
    {
        // 第一步：绝对门以上所有门块的平均 → 相对门 = 平均响度 − 10 LU。
        double energy = 0.0;
        juce::uint64 count = 0;

        for (int bin = 0; bin < histogramBins; ++bin)
        {
            energy += histogramEnergy[(size_t) bin];
            count += histogramCount[(size_t) bin];
        }

        if (count == 0)
        {
            integratedLufs.store (noReading);
            return;
        }

        const auto relativeGate = energyToLufs (energy / (double) count) - 10.0f;

        // 第二步：只取相对门以上的门块再平均（按 0.1 LU 直方图格中心判断，误差 < 0.05 LU）。
        double gatedEnergy = 0.0;
        juce::uint64 gatedCount = 0;

        for (int bin = 0; bin < histogramBins; ++bin)
        {
            const auto binCentre = histogramMinLufs + ((float) bin + 0.5f) / (float) histogramBinsPerLu;

            if (binCentre >= relativeGate)
            {
                gatedEnergy += histogramEnergy[(size_t) bin];
                gatedCount += histogramCount[(size_t) bin];
            }
        }

        integratedLufs.store (gatedCount > 0 ? energyToLufs (gatedEnergy / (double) gatedCount) : noReading);
    }
}
