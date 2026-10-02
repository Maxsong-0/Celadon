/*
  ==============================================================================

    ClayTrialsTests.cpp
    T-048 阶段 1：修坯 Clay 的先试后用候选（Source/dsp/ClayTrials.h，从 MusicAnalyzer「五」节原样抽出）的单元测试。

    直接驱动 ClayTrials：一个小的「组网格」模拟（Driver）按分析器 runOnsets / processGroup 同样的规矩喂——一段 = 同一个组里连续的样本
    （段长由测试给：整组、单样本、随机），组满就 noteGroup（两种有效口径由测试按组给），再 snapStates。覆盖：块网格与各写者的延迟
    （44.1 / 48 / 96 / 192 kHz）、交齐的块按块号先后每块一次、还没交齐的最早一块的起点、起跑预热、两种有效口径各自独立、
    纯单声道的延迟补偿逐位精确、单声道路径与左右相同的立体声逐位相同、任意切段逐位相同、重新起跑与之前的历史无关（实时模式开着时
    学习期的代价与关着时逐位相同就靠这一条）、坏样本、P99 与「存全部再排序」逐位相同、代价的有效门与封底、音频线程零分配。
    候选处理在真实素材上的数值（削顶底鼓的低切代价、混合损失等）照旧由 MusicAnalyzerClay 的用例经分析器核对。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/ClayTrials.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace celadon::test
{
    namespace
    {
        using glaze::ClayTrials;

        bool sameDouble (double a, double b)   { return std::memcmp (&a, &b, sizeof (double)) == 0; }

        bool sameBlock (const ClayTrials::Block& a, const ClayTrials::Block& b)
        {
            auto same = a.index == b.index && sameDouble (a.original, b.original) && a.valid == b.valid && a.liveValid == b.liveValid;

            for (size_t c = 0; c < a.candidates.size(); ++c)
                same = same && sameDouble (a.candidates[c], b.candidates[c]);

            for (size_t c = 0; c < a.blendIntended.size(); ++c)
                same = same && sameDouble (a.blendIntended[c], b.blendIntended[c]) && sameDouble (a.blendOutput[c], b.blendOutput[c]);

            return same;
        }

        bool sameBlocks (const std::vector<ClayTrials::Block>& a, const std::vector<ClayTrials::Block>& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
                if (! sameBlock (a[i], b[i]))
                    return false;

            return true;
        }

        // 分析器的组长：round(fs / 3000)，封顶 128（MusicAnalyzer.cpp「三」节 onsetGroupSeconds / maximumGroupLength）。
        int groupLengthFor (double sampleRate)
        {
            return juce::jlimit (1, ClayTrials::maxSegmentSamples, juce::roundToInt (sampleRate / 3000.0));
        }

        // 组网格的模拟（见文件头）。交齐的块存进预先留好容量的 vector（音频线程上不分配；装满后只计数）。
        struct Driver final : ClayTrials::BlockReceiver
        {
            explicit Driver (double rate = 48000.0) : sampleRate (rate), groupLength (groupLengthFor (rate))
            {
                trials.prepare (sampleRate, groupLength);
                blocks.reserve (2048);
            }

            void trialBlockCompleted (const ClayTrials::Block& block) noexcept override
            {
                if (blocks.size() < blocks.capacity())
                    blocks.push_back (block);
                else
                    ++overflow;
            }

            // 送 left / right 的 [0, numSamples)。segmentLength(组内剩余) 给每段的长度（≥ 1，超过组内剩余的按剩余）；counts(组号) 给每组
            // 结束时两种口径算不算（默认都算）。
            void feed (const float* left, const float* right, bool stereo, int numSamples,
                       const std::function<int (int)>& segmentLength = {},
                       const std::function<std::pair<bool, bool> (std::int64_t)>& counts = {})
            {
                for (int done = 0; done < numSamples;)
                {
                    const auto room = groupLength - groupFill;
                    auto length = juce::jmin (numSamples - done, room);

                    if (segmentLength)
                        length = juce::jlimit (1, length, segmentLength (room));

                    trials.process (left + done, right + done, stereo, length, groups * (std::int64_t) groupLength + groupFill, *this);
                    groupFill += length;
                    done += length;

                    if (groupFill == groupLength)
                    {
                        ++groups;
                        groupFill = 0;
                        const auto c = counts ? counts (groups) : std::pair<bool, bool> { true, true };
                        trials.noteGroup (groups, c.first, c.second, *this);
                        trials.snapStates();
                    }
                }
            }

            void feedStereo (const Signal& signal, int from, int numSamples, const std::function<int (int)>& segmentLength = {},
                             const std::function<std::pair<bool, bool> (std::int64_t)>& counts = {})
            {
                feed (signal[0].data() + from, signal[1].data() + from, true, numSamples, segmentLength, counts);
            }

            std::int64_t ran() const noexcept   { return groups * (std::int64_t) groupLength + groupFill; }

            double sampleRate;
            int groupLength;
            ClayTrials trials;
            std::int64_t groups = 0;
            int groupFill = 0;
            std::vector<ClayTrials::Block> blocks;
            int overflow = 0;
        };

        // 两声道独立的均匀噪声（−20 dBFS 左右）+ 50 Hz 底鼓式正弦脉冲（两声道相同），让每个候选都有事可做。
        Signal makeMaterial (double sampleRate, double seconds, juce::int64 seed)
        {
            const auto numSamples = (int) std::lround (seconds * sampleRate);
            auto signal = makeNoise (2, numSamples, 0.17f, seed);
            const auto period = (int) std::lround (0.5 * sampleRate);
            const auto twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto t = (double) (i % period) / sampleRate;
                const auto kick = (float) (0.5 * std::exp (-t / 0.12) * std::sin (twoPi * 50.0 * t));

                for (auto& channel : signal)
                    channel[(size_t) i] = juce::jlimit (-0.6f, 0.6f, channel[(size_t) i] + kick);
            }

            return signal;
        }

        // P99 的参照：存下全部值、升序排好，位置 0.99·(n − 1) 两侧插值（与 ClayTrials 同一插值式；排序与 nth_element 选出的是同一对值）。
        double referencePercentile (std::vector<double> values)
        {
            std::sort (values.begin(), values.end());
            const auto n = (int) values.size();
            const auto position = ClayTrials::percentile * (double) (n - 1);
            const auto lower = (int) std::floor (position);
            const auto below = values[(size_t) lower];

            if (lower + 1 >= n)
                return below;

            const auto above = values[(size_t) lower + 1];
            return below + (above - below) * (position - (double) lower);
        }
    }

    class ClayTrialsTests  : public juce::UnitTest
    {
    public:
        ClayTrialsTests() : juce::UnitTest ("ClayTrials", "Celadon") {}

        void runTest() override
        {
            testGridAndDelays();
            testBlocksOnceInOrder();
            testWarmUp();
            testValidityFlagsIndependent();
            testPureMonoIsExact();
            testMonoPathEqualsIdenticalStereo();
            testSegmentationIndependence();
            testRestartForgetsHistory();
            testBadSamples();
            testPercentileMatchesFullSort();
            testCosts();
            testNoAllocation();
        }

    private:
        // 依据：ClayTrials.h 第三节「块 = 整数个组，44.1 / 48 / 96 / 192 kHz 下正好 100 ms」、第二节「收单 D_c = 2(L_c − 1)、低切 / 加宽 0」、
        // 「最长的延迟短于一块」（两个记录槽轮用的前提）。
        void testGridAndDelays()
        {
            beginTest ("block grid of whole groups (exactly 100 ms at 44.1 / 48 / 96 / 192 kHz) and the writers' delays");

            for (const auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                Driver d (rate);
                const auto& t = d.trials;
                const auto tag = juce::String (rate, 0) + " Hz";
                expectEquals (t.getBlockLength(), t.getBlockGroups() * d.groupLength, "whole groups: " + tag);
                expectEquals (t.getBlockLength(), (int) std::lround (0.1 * rate), "100 ms: " + tag);
                expectEquals (t.getDelay (ClayTrials::lowCutSlot), 0, "low cut has no delay: " + tag);
                expectEquals (t.getDelay (ClayTrials::widthSlot), 0, "width has no delay: " + tag);

                auto maximum = t.getGuardDelay();

                for (int c = 0; c < ClayTrials::numMono; ++c)
                {
                    const auto length = LinearPhaseLowpass::lengthFor (rate, (double) glaze::clayMonoCandidateHz[(size_t) c]);
                    expectEquals (t.getDelay (ClayTrials::firstMonoSlot + c), 2 * (length - 1), "mono D_c = 2 (L_c - 1): " + tag);
                    maximum = juce::jmax (maximum, t.getDelay (ClayTrials::firstMonoSlot + c));
                }

                expectEquals (t.getMaximumDelay(), maximum, "the longest writer: " + tag);
                expect (t.getMaximumDelay() < t.getBlockLength(), "every delay is shorter than a block: " + tag);
                expectEquals ((int) t.getWarmUpSamples(), (int) std::ceil (0.25 * rate), "250 ms warm-up: " + tag);
            }
        }

        // 依据：ClayTrials.h 第三节「交齐的块交给 BlockReceiver（按块号先后、每块一次）」、pendingBlockStart 的定义。3 s 素材：块号从 0 起
        // 逐一递增、不重不漏；块号 < pendingBlockStart ÷ 块长 的块全都交了，之后的一块都没交。
        void testBlocksOnceInOrder()
        {
            beginTest ("completed blocks arrive in block order, each exactly once, up to the first pending block");

            const auto input = makeMaterial (48000.0, 3.0, 11);
            Driver d;
            auto consistent = true;

            for (int from = 0; from < (int) input[0].size(); from += 4096)
            {
                d.feedStereo (input, from, juce::jmin (4096, (int) input[0].size() - from));
                const auto pending = d.trials.pendingBlockStart (d.ran()) / d.trials.getBlockLength();
                consistent = consistent && (std::int64_t) d.blocks.size() == pending;
            }

            auto ordered = true;

            for (size_t i = 0; i < d.blocks.size(); ++i)
                ordered = ordered && d.blocks[i].index == (std::int64_t) i;

            expect (consistent, "after every chunk: delivered blocks == pendingBlockStart / block length");
            expect (ordered, "block indices 0, 1, 2, ... without gaps or repeats");
            expectEquals ((int) d.blocks.size(), (int) ((3 * 48000 - d.trials.getMaximumDelay()) / 4800), "29 blocks of 3 s are complete");
            expectEquals (d.overflow, 0);
        }

        // 依据：ClayTrials.h 第三节「候选从零状态起跑后 250 ms 内开始的块不算」。48 kHz：第 0 ～ 2 块（起点 0 / 4800 / 9600 < 12000）无效，
        // 第 3 块起有效（每组都算时）。
        void testWarmUp()
        {
            beginTest ("blocks starting within 250 ms of the start are invalid under both definitions");

            const auto input = makeMaterial (48000.0, 2.0, 12);
            Driver d;
            d.feedStereo (input, 0, (int) input[0].size());

            auto rule = d.blocks.size() > 5;

            for (const auto& block : d.blocks)
            {
                const auto warm = block.index * 4800 >= 12000;
                rule = rule && block.valid == warm && block.liveValid == warm;
            }

            expect (rule, "valid == liveValid == (block start >= 12000 samples)");
        }

        // 依据：ClayTrials.h 第五节两种口径。学习口径在第 5、6 块里有一组不算，实时口径在第 8 块里有一组不算：各自只影响自己的标志。
        void testValidityFlagsIndependent()
        {
            beginTest ("the learning and live validity flags are tracked independently");

            const auto input = makeMaterial (48000.0, 2.0, 13);
            Driver d;
            const auto groupsPerBlock = (std::int64_t) d.trials.getBlockGroups();
            d.feedStereo (input, 0, (int) input[0].size(), {}, [groupsPerBlock] (std::int64_t group)
            {
                const auto block = (group - 1) / groupsPerBlock;   // 这一组所在的块（组号从 1 起）
                const auto offset = (group - 1) % groupsPerBlock;
                const auto learning = ! ((block == 5 || block == 6) && offset == 17);
                const auto live = ! (block == 8 && offset == 250);
                return std::pair<bool, bool> { learning, live };
            });

            auto rule = d.blocks.size() > 10;

            for (const auto& block : d.blocks)
            {
                const auto warm = block.index >= 3;
                rule = rule && block.valid == (warm && block.index != 5 && block.index != 6)
                            && block.liveValid == (warm && block.index != 8);
            }

            expect (rule, "blocks 5 / 6 lose only the learning flag, block 8 only the live flag");
        }

        // 依据：ClayTrials.h 第三节「纯单声道素材（S = 0）收单后逐位就是延迟了的原始」、第二节加宽「|M| + |S''|」。左右相同时：
        // 四个收单候选与加宽候选的块峰值逐位等于原始的块峰值（延迟补偿的块网格上同一段音频）；低切确实改了（阳性对照）。
        void testPureMonoIsExact()
        {
            beginTest ("pure mono: the delay-compensated mono candidates and the width candidate equal the original bit for bit");

            const auto material = makeMaterial (48000.0, 3.0, 14);
            const Signal mono { material[0], material[0] };
            Driver d;
            d.feedStereo (mono, 0, (int) mono[0].size());

            auto exact = d.blocks.size() > 20, lowCutDiffers = false;

            for (const auto& block : d.blocks)
            {
                if (! block.valid)
                    continue;

                for (int c = ClayTrials::firstMonoSlot; c < ClayTrials::firstMonoSlot + ClayTrials::numMono; ++c)
                    exact = exact && sameDouble (block.candidates[(size_t) c], block.original);

                exact = exact && sameDouble (block.candidates[(size_t) ClayTrials::widthSlot], block.original);
                lowCutDiffers = lowCutDiffers || ! sameDouble (block.candidates[(size_t) ClayTrials::lowCutSlot], block.original);
            }

            expect (exact, "mono and width candidates == original in every valid block");
            expect (lowCutDiffers, "the low-cut candidate does change the peaks (positive control)");
        }

        // 依据：ClayTrials::process「单声道时传同一个声道、stereo = false」（分析器：单声道输入 L、R 取同一个声道）。单声道路径与左右
        // 逐位相同的立体声路径交出的块逐位相同（单声道 = 左右相同的双声道，与分析器 midSampleOf 的口径一致）。
        void testMonoPathEqualsIdenticalStereo()
        {
            beginTest ("the mono path equals the stereo path with identical channels, bit for bit");

            const auto material = makeMaterial (48000.0, 3.0, 15);
            const Signal mono { material[0], material[0] };
            Driver stereo, single;
            stereo.feedStereo (mono, 0, (int) mono[0].size());
            single.feed (material[0].data(), material[0].data(), false, (int) material[0].size());

            expect (stereo.blocks.size() > 20 && sameBlocks (stereo.blocks, single.blocks), "identical block sequences");
        }

        // 依据：ClayTrials.h「逐位约定：结果只取决于送进来的样本序列、组网格位置与 noteGroup 的判定，与宿主怎么切块无关」。
        // 整组一段、单样本一段、随机段长（两个种子）：交出的块逐位相同。
        void testSegmentationIndependence()
        {
            beginTest ("any segmentation inside the groups gives bit-identical blocks");

            const auto input = makeMaterial (48000.0, 4.0, 16);
            Driver reference;
            reference.feedStereo (input, 0, (int) input[0].size());
            expect (reference.blocks.size() > 30);

            Driver single;
            single.feedStereo (input, 0, (int) input[0].size(), [] (int) { return 1; });
            expect (sameBlocks (reference.blocks, single.blocks), "one sample per segment");

            for (const auto seed : { 3, 77 })
            {
                juce::Random random (seed);
                Driver randomCuts;
                randomCuts.feedStereo (input, 0, (int) input[0].size(), [&random] (int room) { return 1 + random.nextInt (room); });
                expect (sameBlocks (reference.blocks, randomCuts.blocks), "random segments, seed " + juce::String (seed));
            }
        }

        // 依据：ClayTrials.h 第四节「requestRestart：下一次 process 之前从零状态重新起跑」。两份各送 2 s 不同的素材（其中一份削得很狠）
        // 再要求重新起跑、送同一段 5 s：重新起跑之后交出的块逐位相同——候选的滤波器、延迟环、守护、块记账都不带历史。分析器的实时模式
        // 靠这一条：开始学习时候选照旧重新起跑，学习期的代价与实时模式关着时（候选在开始学习前根本没跑）逐位相同。
        void testRestartForgetsHistory()
        {
            beginTest ("a restart forgets everything before it: the blocks afterwards do not depend on the history");

            const auto lead = makeMaterial (48000.0, 2.0, 17);
            auto otherLead = makeMaterial (48000.0, 2.0, 18);
            scale (otherLead, 3.0f);
            const auto tail = makeMaterial (48000.0, 5.0, 19);

            Driver a, b;
            a.feedStereo (lead, 0, (int) lead[0].size());
            b.feedStereo (otherLead, 0, (int) otherLead[0].size());
            const auto beforeA = a.blocks.size(), beforeB = b.blocks.size();
            a.trials.requestRestart();
            b.trials.requestRestart();
            a.feedStereo (tail, 0, (int) tail[0].size());
            b.feedStereo (tail, 0, (int) tail[0].size());

            const std::vector<ClayTrials::Block> afterA (a.blocks.begin() + (std::ptrdiff_t) beforeA, a.blocks.end());
            const std::vector<ClayTrials::Block> afterB (b.blocks.begin() + (std::ptrdiff_t) beforeB, b.blocks.end());
            expect (afterA.size() > 40 && sameBlocks (afterA, afterB), "identical blocks after the restart");

            // 重新起跑那一刻还没交齐的块作废：重新起跑之后交出的第一块是重启点所在的那一块（96000 / 4800 = 第 20 块），且无效（预热）。
            expect (! afterA.empty() && afterA.front().index == 20 && ! afterA.front().valid && ! afterA.front().liveValid,
                    "the first block after the restart is the one containing the restart point, invalid (warm-up)");

            auto warmRule = true;

            for (const auto& block : afterA)
                warmRule = warmRule && block.valid == (block.index * 4800 >= 96000 + 12000);

            expect (warmRule, "valid again once the block starts 250 ms after the restart");
        }

        // 依据：ClayTrials::process「坏样本（NaN / Inf）：只跑它之前的样本，候选从下一段起从零重来」。1 s 处放一个 NaN：交出的块全是有限值，
        // 坏样本所在的块与之后 250 ms 内开始的块都无效，再往后恢复有效。
        void testBadSamples()
        {
            beginTest ("a NaN sample restarts the candidates; every block stays finite and the blocks around it are invalid");

            auto input = makeMaterial (48000.0, 3.0, 20);
            input[1][48000 + 123] = std::numeric_limits<float>::quiet_NaN();
            Driver d;
            d.feedStereo (input, 0, (int) input[0].size());

            auto finite = d.blocks.size() > 20, rule = true;

            for (const auto& block : d.blocks)
            {
                finite = finite && std::isfinite (block.original);

                for (const auto value : block.candidates)
                    finite = finite && std::isfinite (value);

                for (size_t c = 0; c < block.blendIntended.size(); ++c)
                    finite = finite && std::isfinite (block.blendIntended[c]) && std::isfinite (block.blendOutput[c]);

                const auto start = block.index * 4800;
                const auto expectedValid = start >= 12000 && (start + 4800 <= 48000 + 123 || start >= 48000 + 128 + 12000);

                if (start + 4800 <= 48000 + 123 || start >= 48000 + 128 + 12000 || (start <= 48000 + 123 && 48000 + 123 < start + 4800))
                    rule = rule && block.valid == expectedValid;
            }

            expect (finite, "no NaN reaches any block");
            expect (rule, "the block with the NaN and the 250 ms after the restart are invalid, the rest valid");
        }

        // 依据：ClayTrials.h 第六节「n ≤ 512 时每一路只留最大的 8 个就能逐位得到与存下全部块峰值再排序相同的结果」。n = 1 ～ 512 的随机值
        // （含重复值与 0）逐个 insertTop：每个 n 的 percentileOfTop 与参照逐位相同。
        void testPercentileMatchesFullSort()
        {
            beginTest ("P99 from the top 8 equals P99 from all values, bit for bit, for every n up to 512");

            juce::Random random (21);
            ClayTrials::Top top {};
            std::vector<double> all;
            auto same = true;

            for (int n = 1; n <= ClayTrials::capacity; ++n)
            {
                const auto value = random.nextInt (5) == 0 ? (double) random.nextInt (4) * 0.25 : random.nextDouble();
                ClayTrials::insertTop (top, n - 1, value);
                all.push_back (value);
                same = same && sameDouble (ClayTrials::percentileOfTop (top, n), referencePercentile (all));
            }

            expect (same, "every n from 1 to 512");
        }

        // 依据：ClayTrials.h 第六节 costsOf。48 kHz：块时长 = n / 10 s。99 块（9.9 s）一律「不知道」；100 块（10 s）有效、观测充分度 1；
        // 候选 P99 为 0 时按原始的 −120 dB 封底；混合损失要两份能量都 > 0；有效时长都是块时长。
        void testCosts()
        {
            beginTest ("costs: valid from 10 s of blocks, the -120 dB floor, the blend loss needs both energies");

            ClayTrials::TopSeries top {};
            ClayTrials::BlendSums intended {}, output {};
            ClayTrials::Block block;
            block.original = 0.5;
            block.candidates.fill (0.25);
            block.candidates[(size_t) ClayTrials::widthSlot] = 0.0;   // 加宽候选全是 0：按 −120 dB 封底
            block.blendIntended = { 2.0, 0.0 };
            block.blendOutput = { 1.0, 3.0 };

            for (int n = 0; n < 100; ++n)
                ClayTrials::accumulate (top, intended, output, n, block);

            const auto short99 = ClayTrials::costsOf (top, intended, output, 99, 4800, 48000.0);
            expect (! short99.originalP99Db.valid && ! short99.lowCutPeakCostDb.valid && ! short99.widthPeakCostDb.valid
                        && ! short99.blendLossDb[0].valid && ! short99.blendLossDb[1].valid,
                    "9.9 s of blocks: everything unknown");
            expectWithinAbsoluteError (short99.measured, 0.99f, 1.0e-6f);

            const auto full = ClayTrials::costsOf (top, intended, output, 100, 4800, 48000.0);
            expect (full.originalP99Db.valid && full.lowCutPeakCostDb.valid && full.widthPeakCostDb.valid, "10 s: valid");
            expectEquals (full.measured, 1.0f);
            expectWithinAbsoluteError (full.originalP99Db.value, (float) (20.0 * std::log10 (0.5)), 1.0e-6f);
            expectWithinAbsoluteError (full.lowCutPeakCostDb.value, (float) (20.0 * std::log10 (0.5)), 1.0e-6f, "0.25 vs 0.5: -6.02 dB");

            for (const auto& mono : full.monoPeakCostDb)
                expect (mono.valid && std::abs (mono.value - (float) (20.0 * std::log10 (0.5))) < 1.0e-6f);

            expectWithinAbsoluteError (full.widthPeakCostDb.value, -120.0f, 1.0e-4f, "zero candidate: floored at -120 dB");
            expect (full.blendLossDb[0].valid, "blend loss with both energies");
            expectWithinAbsoluteError (full.blendLossDb[0].value, (float) (10.0 * std::log10 (2.0)), 1.0e-6f);
            expect (! full.blendLossDb[1].valid, "no intended energy: unknown");
            expectEquals (full.lowCutPeakCostDb.effectiveSeconds, 10.0, "effective seconds = block time");
            expectEquals (full.lowCutPeakCostDb.confidence, 0.0f, "confidence is left to the caller");
        }

        // 依据：ClayTrials.h「process / noteGroup / snapStates / requestRestart 不分配」；TestMain.cpp 的 operator new 计数。随机切段、
        // 途中重新起跑一次、一个 NaN（坏样本路径），交出的块存进预先留好容量的 vector。
        void testNoAllocation()
        {
            beginTest ("the audio-thread API does not allocate (segments, groups, restarts, bad samples)");

            auto input = makeMaterial (48000.0, 3.0, 22);
            input[0][100000] = std::numeric_limits<float>::infinity();
            Driver d;
            juce::Random random (5);
            const std::function<int (int)> cuts = [&random] (int room) { return 1 + random.nextInt (room); };   // 在计数区外构造
            const auto before = allocationCountOnThisThread();

            d.feedStereo (input, 0, 60000, cuts);
            d.trials.requestRestart();
            d.feedStereo (input, 60000, (int) input[0].size() - 60000, cuts);

            const auto allocations = (int) (allocationCountOnThisThread() - before);
            expectEquals (allocations, 0, "no operator new");
            expect (d.blocks.size() > 20 && d.overflow == 0, "the run delivered blocks");
        }
    };

    static ClayTrialsTests clayTrialsTests;
}
