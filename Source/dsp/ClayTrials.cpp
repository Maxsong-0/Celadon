/*
  ==============================================================================

    ClayTrials.cpp
    修坯 Clay 的先试后用（口径见 ClayTrials.h）。T-048 阶段 1 从 MusicAnalyzer「五」节逐字搬来：restart = 原 restartTrial、
    process = 原 runTrial、noteGroup = 原 noteTrialGroup、snapStates = 原 snapTrialStates、recordFor / countWrite = 原 trialRecordFor /
    countTrialWrite（交齐的块改为交给 BlockReceiver；新增实时口径 liveValid），costsOf / percentileOfTop = 原 updateTrialProfile /
    percentileOfTop（原来分析器里通用画像与 House 画像各写一份，现在共用这一份）。

  ==============================================================================
*/

#include "ClayTrials.h"

#include <algorithm>

namespace celadon::glaze
{
    //==============================================================================
    // 配置（消息线程）
    //==============================================================================
    void ClayTrials::prepare (double newSampleRate, int groupLength)
    {
        sampleRate = newSampleRate;
        const auto groupSeconds = (double) groupLength / sampleRate;

        // 块 = 整数个组（与起音的组网格对齐：44.1 / 48 / 96 / 192 kHz 下正好 100 ms）。
        blockGroups = juce::jmax (1, juce::roundToInt (blockSeconds / groupSeconds));
        blockLength = blockGroups * groupLength;
        warmUpSamples = (std::int64_t) std::ceil (warmUpSeconds * sampleRate);

        for (size_t stage = 0; stage < cutCoefficients.size(); ++stage)
            cutCoefficients[stage] = ClayBiquad::highpass (sampleRate, lowCutHz, cutQ[stage]);

        shelfCoefficients = ClayBiquad::highShelf (sampleRate, widthShelfHz, widthDb);
        delays.fill (0);

        // 阶段 2（Q6）：四个收单候选停算。槽位还在（写者个数与画像 monoPeakCostDb[4] 的布局不变），
        // 延迟记 0，不再按分频点分配线性相位低通。块峰值保持 0，costsOf 把这四路标成不知道。
        for (size_t c = 0; c < monoLength.size(); ++c)
        {
            monoLength[c] = 1;
            delays[(size_t) firstMonoSlot + c] = 0;
        }

        // 混合损失候选：低切系数与守护前瞻 G（约 11 ms，同样短于一块）。
        for (size_t c = 0; c < blendCut.size(); ++c)
            for (size_t stage = 0; stage < cutQ.size(); ++stage)
                blendCut[c][stage] = ClayBiquad::highpass (sampleRate, (double) clayBlendCandidateHz[c], cutQ[stage]);

        guardDelay = ClayModule::PeakGuard::windowFor (sampleRate) + ClayModule::PeakGuard::attackFor (sampleRate);
        jassert (guardDelay < blockLength);

        // 一块的记录要等全部写者交齐（含混合损失候选，它们比原始晚 G）——最长的那个延迟决定块何时交齐。
        maxDelay = guardDelay;

        for (const auto delay : delays)
            maxDelay = juce::jmax (maxDelay, delay);

        // 随采样率定长的部分：收单候选的线性相位低通（按各自的 L_c）与 M / S 延迟环（2 的幂长度，大于最长的 D_c）。
        auto longestDelay = 0;

        for (size_t c = 0; c < monoLowpass.size(); ++c)
        {
            monoLowpass[c].prepare (monoLength[c]);
            longestDelay = juce::jmax (longestDelay, delays[(size_t) firstMonoSlot + c]);
        }

        const auto historyLength = juce::nextPowerOfTwo (longestDelay + 1);
        midHistory.assign ((size_t) historyLength, 0.0);
        sideHistory.assign ((size_t) historyLength, 0.0);
        historyMask = historyLength - 1;
        historyWrite = 0;

        // 混合损失候选：守护按立体声准备（单声道输入时两路喂同一个声道）。
        for (auto& guard : guards)
            guard.prepare (sampleRate, 2);

        restartPending = true;
    }

    //==============================================================================
    // 候选从零状态重新起跑。position = 下一个要跑的样本在组网格上的序号。只清零、不分配。
    //==============================================================================
    void ClayTrials::restart (std::int64_t position) noexcept
    {
        restartPending = false;

        for (auto& channelStates : cutState)
            channelStates.fill (ClayBiquad::State {});

        shelfState = ClayBiquad::State {};

        for (size_t c = 0; c < monoLowpass.size(); ++c)
            monoLowpass[c].reset (monoLength[c]);

        std::fill (midHistory.begin(), midHistory.end(), 0.0);
        std::fill (sideHistory.begin(), sideHistory.end(), 0.0);
        historyWrite = 0;
        runSamples = 0;

        // 原始的当前块在候选起跑之前就开始了：不算。各候选的当前块按自己的延迟对到同一个网格上——候选此刻的输出对应原始的
        // 第 position − D_c 个样本；早于重启点的那 D_c 个输出（延迟环里的 0）并进第 0 块，而第 0 块落在重启后 250 ms 的预热里。
        originalMax = 0.0;
        blockWarm = false;
        blockValid = false;
        blockLiveValid = false;
        const auto blockSamples = (std::int64_t) blockLength;

        for (size_t c = 0; c < (size_t) numCandidates; ++c)
        {
            const auto source = position - (std::int64_t) delays[c];
            candidateMax[c] = 0.0;
            candidateBlock[c] = source >= 0 ? source / blockSamples : 0;
            candidateLeft[c] = (int) (source >= 0 ? blockSamples - source % blockSamples : blockSamples - source);
        }

        // 混合损失候选：从零状态重新起跑，块网格同收单候选（按自己的延迟 G 对齐）；守护的 64 样本网格从这里起算。
        for (auto& guard : guards)
            guard.reset();

        for (auto& candidate : blendCutState)
            for (auto& channelStates : candidate)
                channelStates.fill (ClayBiquad::State {});

        guardGridLeft = 0;

        blendIntendedBlock.fill (0.0);
        blendOutputBlock.fill (0.0);

        {
            const auto source = position - (std::int64_t) guardDelay;
            blendBlock = source >= 0 ? source / blockSamples : 0;
            blendLeft = (int) (source >= 0 ? blockSamples - source % blockSamples : blockSamples - source);
        }

        for (auto& record : records)
            record = Record {};
    }

    //==============================================================================
    // 一段（同一个组里连续的 length 个样本）
    //==============================================================================
    void ClayTrials::process (const float* left, const float* right, bool stereo, int length, std::int64_t position,
                              BlockReceiver& receiver) noexcept
    {
        jassert (length <= maxSegmentSamples);

        if (restartPending)
            restart (position);

        // 坏样本（NaN / Inf）会把候选的滤波器状态弄坏：只跑它之前的样本，候选从下一段起从零重来。那一组的起音功率也非有限，
        // 分析器组末整体重启，跨越它的块本来就作废。
        auto count = 0;

        while (count < length && std::isfinite (left[count]) && std::isfinite (right[count]))
            ++count;

        if (count < length)
            restartPending = true;

        if (count == 0)
            return;

        // 1. M / S（单声道时 right 就是 left：M = 该声道、S = 0，与分析器 midSampleOf 同一口径）。
        for (int k = 0; k < count; ++k)
        {
            const auto l = (double) left[k];
            const auto r = (double) right[k];
            midScratch[(size_t) k] = 0.5 * (l + r);
            sideScratch[(size_t) k] = 0.5 * (l - r);
        }

        // 2. 收单候选停算（阶段 2）：不再跑线性相位低通。

        // 3. 逐样本：原始的块峰值累计；六个候选各自此刻的输出峰值先写暂存（第 4 步按各自的块边界累计）。
        const auto cutFirst = cutCoefficients[0];
        const auto cutSecond = cutCoefficients[1];
        const auto shelf = shelfCoefficients;
        auto leftFirst = cutState[0][0];
        auto leftSecond = cutState[0][1];
        auto rightFirst = cutState[1][0];
        auto rightSecond = cutState[1][1];
        auto shelfRun = shelfState;
        auto originalRun = originalMax;

        for (int k = 0; k < count; ++k)
        {
            const auto i = (size_t) k;
            const auto l = (double) left[k];
            const auto mid = midScratch[i];
            const auto side = sideScratch[i];

            // 原始：max(|L|, |R|)。0 低切：L、R 各两级高通，max(|L'|, |R'|)（单声道只跑一路：两路逐位相同）。
            // 低切输出另存一份 float（T-043：20 Hz 的混合损失候选直接用它，与 ClayModule 的低切同样以 float 交给守护）。
            auto inputPeak = std::abs (l);
            const auto cutLeft = ClayBiquad::run (ClayBiquad::run (l, cutFirst, leftFirst), cutSecond, leftSecond);
            auto cutPeak = std::abs (cutLeft);
            cutOut[0][i] = (float) cutLeft;
            cutOut[1][i] = (float) cutLeft;

            if (stereo)
            {
                const auto r = (double) right[k];
                inputPeak = juce::jmax (inputPeak, std::abs (r));
                const auto cutRight = ClayBiquad::run (ClayBiquad::run (r, cutFirst, rightFirst), cutSecond, rightSecond);
                cutPeak = juce::jmax (cutPeak, std::abs (cutRight));
                cutOut[1][i] = (float) cutRight;
            }

            originalRun = juce::jmax (originalRun, inputPeak);
            peakScratch[(size_t) lowCutSlot][i] = cutPeak;

            // 5 加宽：S'' = 高架(S)，峰值 |M| + |S''|。
            peakScratch[(size_t) widthSlot][i] = std::abs (mid) + std::abs (ClayBiquad::run (side, shelf, shelfRun));

            // 1 ～ 4 收单停算：块峰值记 0（不写延迟环、不跑低通）。写者仍要交齐，所以槽位留着。
            for (size_t c = 0; c < (size_t) numMono; ++c)
                peakScratch[(size_t) firstMonoSlot + c][i] = 0.0;
        }

        cutState[0][0] = leftFirst;
        cutState[0][1] = leftSecond;
        cutState[1][0] = rightFirst;
        cutState[1][1] = rightSecond;
        shelfState = shelfRun;
        originalMax = originalRun;
        runSamples = juce::jmin (runSamples + (std::int64_t) count, warmUpSamples);

        // 4. 各候选按自己（延迟补偿后）的块边界累计块峰值；交齐一块就写进这一块的记录。
        for (size_t c = 0; c < (size_t) numCandidates; ++c)
        {
            const auto& peaks = peakScratch[c];
            auto blockMax = candidateMax[c];
            auto remaining = candidateLeft[c];

            for (int k = 0; k < count;)
            {
                const auto take = juce::jmin (count - k, remaining);

                for (int e = k; e < k + take; ++e)
                    blockMax = juce::jmax (blockMax, peaks[(size_t) e]);

                k += take;
                remaining -= take;

                if (remaining == 0)
                {
                    auto& record = recordFor (candidateBlock[c]);
                    record.block.candidates[c] = blockMax;
                    countWrite (record, receiver);
                    ++candidateBlock[c];
                    blockMax = 0.0;
                    remaining = blockLength;
                }
            }

            candidateMax[c] = blockMax;
            candidateLeft[c] = remaining;
        }

        // 5. 混合损失候选（T-043）：按从起跑算的 64 样本网格切段（网格点上各守护先定这一格量不量，ClayModule::process 的同一做法），
        //    每段算各候选的低切输出（20 Hz 那个用上面存的 cutOut），连同原始干路送进各自的守护；守护的抽头写出逐输出样本的两份
        //    K 加权能量，按共同的块边界（两个延迟相同）累计，交齐一块就各自写进这一块的记录（第 8、9 个写者）。没量的输出样本
        //    （守护前瞻环里还没有低切过的样本）能量记 0。守护原地写回的输出用不着。
        for (int done = 0; done < count;)
        {
            if (guardGridLeft == 0)
            {
                for (auto& guard : guards)
                    guard.decideBlendMeasuring();

                guardGridLeft = ClayModule::PeakGuard::maxSegmentLength;
            }

            const auto chunk = juce::jmin (count - done, guardGridLeft);
            const float* const dry[] { left + done, right + done };

            for (size_t c = 0; c < (size_t) numBlend; ++c)
            {
                auto& io = blendIo[c];

                if ((int) c == sharedCutBlend)
                {
                    for (int k = 0; k < chunk; ++k)
                    {
                        io[0][(size_t) k] = cutOut[0][(size_t) (done + k)];
                        io[1][(size_t) k] = cutOut[1][(size_t) (done + k)];
                    }
                }
                else
                {
                    const auto first = blendCut[c][0];
                    const auto second = blendCut[c][1];
                    auto& states = blendCutState[c];

                    for (int k = 0; k < chunk; ++k)
                    {
                        io[0][(size_t) k] = (float) ClayBiquad::run (ClayBiquad::run ((double) dry[0][k], first, states[0][0]), second, states[0][1]);
                        io[1][(size_t) k] = stereo ? (float) ClayBiquad::run (ClayBiquad::run ((double) dry[1][k], first, states[1][0]), second, states[1][1])
                                                   : io[0][(size_t) k];
                    }
                }

                auto& guard = guards[c];
                guard.tapIntended = guardIntended[c].data() + done;
                guard.tapOutput = guardOutput[c].data() + done;
                float* const channels[] { io[0].data(), io[1].data() };
                guard.process (dry, channels, nullptr, 2, chunk);
            }

            done += chunk;
            guardGridLeft -= chunk;
        }

        auto intendedSum = blendIntendedBlock;
        auto outputSum = blendOutputBlock;
        auto remaining = blendLeft;

        for (int k = 0; k < count;)
        {
            const auto take = juce::jmin (count - k, remaining);

            for (size_t c = 0; c < (size_t) numBlend; ++c)
            {
                for (int e = k; e < k + take; ++e)
                {
                    intendedSum[c] += guardIntended[c][(size_t) e];
                    outputSum[c] += guardOutput[c][(size_t) e];
                }
            }

            k += take;
            remaining -= take;

            if (remaining == 0)
            {
                for (size_t c = 0; c < (size_t) numBlend; ++c)
                {
                    auto& record = recordFor (blendBlock);
                    record.block.blendIntended[c] = intendedSum[c];
                    record.block.blendOutput[c] = outputSum[c];
                    countWrite (record, receiver);
                }

                ++blendBlock;
                intendedSum.fill (0.0);
                outputSum.fill (0.0);
                remaining = blockLength;
            }
        }

        blendIntendedBlock = intendedSum;
        blendOutputBlock = outputSum;
        blendLeft = remaining;
    }

    //==============================================================================
    // 候选的 IIR 状态在长时间静音里同样会衰减进非规格化数：每组末尾归零（与真次声低通同一门槛、同一网格）。
    void ClayTrials::snapStates() noexcept
    {
        for (auto& channelStates : cutState)
            for (auto& stageState : channelStates)
                ClayBiquad::snap (stageState);

        ClayBiquad::snap (shelfState);

        for (auto& candidate : blendCutState)   // T-043 混合损失候选的低切（守护自己的状态由它的网格归零）
            for (auto& channelStates : candidate)
                for (auto& stageState : channelStates)
                    ClayBiquad::snap (stageState);
    }

    //==============================================================================
    // 原始一侧（组末）：本组按两种口径算不算；块的最后一组结组时交出这一块。等着重新起跑（还没跑、或刚出过坏样本）时不记。
    void ClayTrials::noteGroup (std::int64_t groupsSinceRestart, bool countsLearning, bool countsLive, BlockReceiver& receiver) noexcept
    {
        if (restartPending)
            return;

        blockValid = blockValid && countsLearning;
        blockLiveValid = blockLiveValid && countsLive;

        if (groupsSinceRestart % blockGroups != 0)
            return;

        auto& record = recordFor (groupsSinceRestart / blockGroups - 1);
        record.block.original = originalMax;
        record.block.valid = blockWarm && blockValid;
        record.block.liveValid = blockWarm && blockLiveValid;
        countWrite (record, receiver);

        // 下一块：开始时候选已跑满预热才算。
        originalMax = 0.0;
        blockValid = true;
        blockLiveValid = true;
        blockWarm = runSamples >= warmUpSamples;
    }

    ClayTrials::Record& ClayTrials::recordFor (std::int64_t index) noexcept
    {
        auto& record = records[(size_t) (index & 1)];

        if (record.block.index != index)
        {
            record = Record {};
            record.block.index = index;
        }

        return record;
    }

    // 原始、峰值候选与混合损失候选都写过这一块才算交齐：交给接收者，然后腾出槽位。
    void ClayTrials::countWrite (Record& record, BlockReceiver& receiver) noexcept
    {
        if (++record.writes < writers)
            return;

        receiver.trialBlockCompleted (record.block);
        record = Record {};
    }

    std::int64_t ClayTrials::pendingBlockStart (std::int64_t ran) const noexcept
    {
        const auto delivered = ran - (std::int64_t) maxDelay;
        const auto blocks = delivered > 0 ? delivered / (std::int64_t) blockLength : 0;
        return blocks * (std::int64_t) blockLength;
    }

    //==============================================================================
    // 累计与代价
    //==============================================================================
    void ClayTrials::insertTop (Top& top, int filled, double value) noexcept
    {
        auto slot = juce::jmin (filled, topCount - 1);

        if (filled >= topCount && ! (value > top[(size_t) slot]))
            return;

        for (; slot > 0 && top[(size_t) slot - 1] < value; --slot)
            top[(size_t) slot] = top[(size_t) slot - 1];

        top[(size_t) slot] = value;
    }

    void ClayTrials::accumulate (TopSeries& top, BlendSums& intended, BlendSums& output, int filled, const Block& block) noexcept
    {
        insertTop (top[0], filled, block.original);

        for (size_t c = 0; c < (size_t) numCandidates; ++c)
            insertTop (top[c + 1], filled, block.candidates[c]);

        for (size_t c = 0; c < (size_t) numBlend; ++c)
        {
            intended[c] += block.blendIntended[c];
            output[c] += block.blendOutput[c];
        }
    }

    double ClayTrials::percentileOfTop (const Top& top, int n) noexcept
    {
        const auto position = percentile * (double) (n - 1);
        const auto lower = (int) std::floor (position);
        const auto below = top[(size_t) (n - 1 - lower)];

        if (lower + 1 >= n)
            return below;

        const auto above = top[(size_t) (n - 2 - lower)];
        return below + (above - below) * (position - (double) lower);
    }

    ClayTrials::Costs ClayTrials::costsOf (const TopSeries& top, const BlendSums& intended, const BlendSums& output, int count,
                                           int blockSamples, double rate) noexcept
    {
        Costs costs;
        const auto seconds = (double) count * (double) blockSamples / rate;
        costs.measured = (float) juce::jlimit (0.0, 1.0, seconds / fullSeconds);

        const auto set = [seconds] (Feature& feature, double value) noexcept
        {
            feature.value = (float) value;
            feature.valid = true;
            feature.effectiveSeconds = seconds;
        };

        const auto original = count > 0 ? percentileOfTop (top[0], count) : 0.0;
        const auto enough = seconds >= minimumSeconds && original > 0.0;

        if (! enough)
            return costs;   // 全部「不知道」（混合损失也要与代价同时有效）

        set (costs.originalP99Db, 20.0 * std::log10 (original));

        for (int slot = 0; slot < numCandidates; ++slot)
        {
            // 四个收单槽停算：monoPeakCostDb 保持默认（valid = false），不把 0 峰值写成 −120 dB。
            if (slot >= firstMonoSlot && slot < widthSlot)
                continue;

            auto& cost = slot == lowCutSlot ? costs.lowCutPeakCostDb : costs.widthPeakCostDb;
            const auto candidate = percentileOfTop (top[(size_t) slot + 1], count);
            set (cost, 20.0 * std::log10 (juce::jmax (candidate, peakFloorRatio * original) / original));
        }

        // 混合损失（T-043，ADR-027）：同一批有效块里两份 K 加权能量之比，每个候选频率一个。两份都 > 0 才知道（整段静音时不知道）。
        for (size_t c = 0; c < (size_t) numBlend; ++c)
            if (intended[c] > 0.0 && output[c] > 0.0)
                set (costs.blendLossDb[c], 10.0 * std::log10 (intended[c] / output[c]));

        return costs;
    }
}
