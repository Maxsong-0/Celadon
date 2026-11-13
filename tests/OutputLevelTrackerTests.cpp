/*
  ==============================================================================

    OutputLevelTrackerTests.cpp
    T-106（ADR-038）出口电平跟踪器（Source/dsp/OutputLevelTracker.h）的自动测试。它是 G_trim 的传感器：G_trim 的误差
    = 方案净增益 − 实测净增益，实测净增益由它的 evidenceAt() 给。

    不经链，直接喂两路样本（入口是白噪声，出口 = 入口 × 一个倍数），每条断言都能按算式手算。覆盖：
      1. 读数：实测净增益 = 出口主段电平 − 入口主段电平（同一批段）；参考增益动多少 dB，出口读数当下就动多少 dB。
      2. 证据加权：每段按录制增益离参考增益的远近计权（≤ 1 dB 全算、≥ 3 dB 不算、中间线性）；两侧的能量和、时长用同一个权重。
      3. 入口配对：入口没配齐的段不当证据；一片的入口能量不会算到别的片上（没有出口、样本数对不上、明说作废、已经用过）；
         哪一侧有非有限样本的片不配对。
      4. 回看重判：只有此刻仍算主段的段进读数；没有读数（或读数不能用）的段保持当时的判定；可逆；判定函数收到的是段内平均；
         正开着的段不归它管。
      5. 存读档：存 → 读 → 再存逐属性相同、读数逐位相同；0.1.8.10 就有的八个属性原样还在、整块数组的字节数不变。
      6. 旧存档（没有 T-106 的七个属性）读成空历史并打上标记；新格式里属性不全、三个样本数不是严格的十进制、段本身或每段另记的
         量有一项不合规矩的整节拒收，拒收时状态不动；正好在界上的收下。
      7. 主段门：只有 m′ ≥ 0.5 的段进账；主段 → 非主段 → 主段，中间那一截两侧都不留痕迹，新开的段从零记起。
      8. 没有出口能量的段（数字静音）不当证据，也不占证据时长。
    跟踪器接进编排器之后的行为（G_trim 环路、读 0.1.8.10 存的整份 LIVE）见 GlazeControllerTests.cpp 的 GlazeControllerTrimLoop
    与 GlazeControllerGuardFamily 第 5 条。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/GlazeConstants.h"
#include "../Source/dsp/OutputLevelTracker.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace celadon::test
{
    namespace
    {
        constexpr int numChannels = 2;

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        class OutputLevelTrackerTests  : public juce::UnitTest
        {
        public:
            OutputLevelTrackerTests() : juce::UnitTest ("OutputLevelTracker", "Celadon") {}

            void runTest() override
            {
                testNetGainFollowsTheReference();
                testEvidenceWeighting();
                testInputPairing();
                testRejudge();
                testStateRoundTrip();
                testLegacyAndBrokenState();
                testMainnessFlips();
                testSegmentsWithoutOutputEnergyAreNotEvidence();
            }

        private:
            using Tracker = glaze::OutputLevelTracker;
            using Evidence = Tracker::Evidence;

            static constexpr double rate = 48000.0;
            static constexpr int block = 480;                  // 10 ms：100 块 = 1 s，下面的秒数都是整数个块
            static constexpr int blocksPerSecond = 100;
            static constexpr float fullDb = 1.0f;              // 同 GlazeController.cpp trimEvidenceFullDb
            static constexpr float zeroDb = 3.0f;              // 同 trimEvidenceZeroDb
            static constexpr double secondsTolerance = 1.0e-6;
            static constexpr float levelToleranceDb = 2.0e-3f;

            // T-106 的七个属性与 0.1.8.10 就有的八个（OutputLevelTracker.cpp createStateXml）。
            static constexpr const char* extraAttributes[] = { "openUnityEnergy", "openInputEnergy", "openInputLufsSum", "openLowShareSum",
                                                               "openInputSamples", "openPairedSamples", "closedExtra" };
            static constexpr const char* legacyAttributes[] = { "openEnergy", "openMainness", "closedCount", "closedWrite", "haveOpenMainness",
                                                                "openWasMain", "openSamples", "closed" };
            // 存档里两个整块数组的字节数：32 段 ×（Segment 24 字节 = double + int64 + float 补齐到 8；Extra 48 字节 = 4 个 double + 2 个 int64）。
            // 0.1.8.10 按 768 字节读 closed；这两个数变了就是存档格式变了。
            static constexpr size_t closedBytes = 32 * 24;
            static constexpr size_t closedExtraBytes = 32 * 48;
            // 读档时一段样本数的上界（OutputLevelTracker.cpp stateMaximumSegmentSamples）：一段最长 8 s × 采样率上界 768 kHz。
            static constexpr std::int64_t maximumSegmentSamples = 8 * 768000;

            struct Feed
            {
                float limitGainDb = 0.0f;      // 记账用的 Limit Gain（processOutput 的第二个参数）
                float outputGain = 1.0f;       // 出口 = 入口 × 这个倍数
                float mainness = 1.0f;
                bool measured = true;          // 这一块带输入侧读数
                float inputLufs = -20.0f;
                float lowShare = 0.3f;
                bool withInput = true;         // 这一片交入口样本
                float inputScale = 1.0f;       // 入口噪声幅度的倍数
            };

            // 喂 blocks 块。入口是白噪声（幅度 0.1 × inputScale；每块一个种子，从 seed 起逐块加 1），出口 = 入口 × outputGain。
            // 调用顺序同链里的一片：processInput（analyzeInput）→ noteMainness → processOutput（endBlock）。
            static void feed (Tracker& tracker, int blocks, juce::int64 seed, const Feed& f)
            {
                juce::AudioBuffer<float> input (numChannels, block), output (numChannels, block);

                for (int b = 0; b < blocks; ++b)
                {
                    const auto noise = makeNoise (numChannels, block, 0.1f * f.inputScale, seed + b);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        input.copyFrom (ch, 0, noise[(size_t) ch].data(), block);
                        output.copyFrom (ch, 0, noise[(size_t) ch].data(), block);
                    }

                    output.applyGain (f.outputGain);

                    if (f.withInput)
                        tracker.processInput (input);

                    tracker.noteMainness (f.mainness, f.measured, f.inputLufs, f.lowShare);
                    tracker.processOutput (output, f.limitGainDb);
                }
            }

            // 一条「线性的链」：出口 = 入口 × 10^(gainDb / 20)，记账的 Limit Gain 也是 gainDb——折回 0 dB 的出口能量就等于入口能量，
            // 按参考增益 r 读出来的净增益正好是 r。
            static Feed linearChain (float gainDb)
            {
                Feed f;
                f.limitGainDb = gainDb;
                f.outputGain = juce::Decibels::decibelsToGain (gainDb);
                return f;
            }

            static bool sameEvidence (const Evidence& a, const Evidence& b) noexcept
            {
                return sameBits (a.levelLufs, b.levelLufs) && sameBits (a.inputLufs, b.inputLufs) && a.seconds == b.seconds;
            }

            static bool noEvidence (const Evidence& e) noexcept
            {
                return ! e.valid() && e.seconds == 0.0 && e.levelLufs == -100.0f && e.inputLufs == -100.0f;
            }

            static std::int64_t samplesFor (int seconds) noexcept   { return (std::int64_t) seconds * 48000; }

            //==============================================================================
            // 1. 依据：OutputLevelTracker.h「读数 = 折回 0 dB 的主段电平 + g」「实测净增益 = 出口主段电平 − 入口主段电平，同一批段」。
            //    出口 = 入口 × 2（+6.0206 dB），记账的 Limit Gain 4 dB，6 s 都是主段。
            //    断言：按 4 dB 读，净增益 6.0206 dB、证据 6 s；参考增益换成 4.5 dB，出口电平正好高 0.5 dB、入口电平逐位不变——
            //    参考增益一动读数当下跟着动（G_trim 走 δ、读数就变 δ：它看得到自己的动作，T-106 的出发点）。
            void testNetGainFollowsTheReference()
            {
                beginTest ("evidenceAt: net gain = output - input over the same segments; the output level follows the reference gain dB for dB");

                Tracker tracker;
                tracker.prepare (rate);
                Feed f;
                f.limitGainDb = 4.0f;
                f.outputGain = 2.0f;
                feed (tracker, 6 * blocksPerSecond, 100, f);

                const auto at4 = tracker.evidenceAt (4.0f, fullDb, zeroDb);
                const auto at45 = tracker.evidenceAt (4.5f, fullDb, zeroDb);
                const auto expectedNetDb = (float) (20.0 * std::log10 (2.0));
                logMessage ("  at 4 dB: output " + juce::String (at4.levelLufs, 4) + " / input " + juce::String (at4.inputLufs, 4) + " LUFS, net "
                            + juce::String (at4.netGainDb(), 4) + " dB over " + juce::String (at4.seconds, 3) + " s; at 4.5 dB: output "
                            + juce::String (at45.levelLufs, 4) + " LUFS");
                expect (at4.valid() && at45.valid(), "six seconds of paired main material are evidence");
                expect (at4.inputLufs > -40.0f && at4.inputLufs < -10.0f, "sanity: white noise at 0.1 reads somewhere near -22 LUFS: " + juce::String (at4.inputLufs, 2));
                expectWithinAbsoluteError (at4.seconds, 6.0, secondsTolerance, "evidence = the six seconds fed");
                expectWithinAbsoluteError (at4.netGainDb(), expectedNetDb, levelToleranceDb, "net gain = 20 log10 (2) = 6.0206 dB");
                expectWithinAbsoluteError (at45.levelLufs - at4.levelLufs, 0.5f, 1.0e-4f, "raising the reference gain by 0.5 dB raises the output level by 0.5 dB");
                expect (sameBits (at45.inputLufs, at4.inputLufs), "the input level does not depend on the reference gain");
                expectWithinAbsoluteError (at45.seconds, 6.0, secondsTolerance, "0.5 dB away from the recorded gain still counts in full");
                expect (tracker.openSegmentIsMain(), "the open segment is a main section");
                expect (tracker.mainSamples() == samplesFor (6), "six seconds of main samples");
            }

            // 2. 依据：OutputLevelTracker.h evidenceAt「每段按录它时的 Limit Gain 离 limitGainDb 多远加权：差 ≤ fullWithinDb 全算，
            //    ≥ zeroBeyondDb 不算，中间线性」「两个电平用同一批段、同样的权重」；录制增益 = 10·log10(实录能量 ÷ 折算能量)。
            //    线性的链，两段：A 4 s 录在 2 dB、入口幅度 ×1，关段，B 6 s 录在 6 dB、入口幅度 ×0.5（轻 6.02 dB）。全算 1 dB、不算 3 dB：
            //      参考 2 → A 差 0（全算）、B 差 4（不算）→ 证据 4 s，净增益 2，入口电平就是 A 的（记作 a）；
            //      参考 6 → A 不算、B 全算 → 6 s，净增益 6，入口电平就是 B 的（记作 b）；
            //      参考 4 → 两段都差 2 → 权重 (3 − 2) ÷ (3 − 1) = 0.5 → 0.5 × 4 + 0.5 × 6 = 5 s，净增益 4；
            //      参考 3.5 → A 差 1.5（0.75）、B 差 2.5（0.25）→ 3 + 1.5 = 4.5 s，净增益 3.5；
            //      参考 10 → 都不算 → 没有证据。参考不是有限值 → 没有证据。
            //    净增益等于参考增益：线性的链上，哪一段在什么增益下录的都不影响读数（历史跟着折算）。
            //    两段的入口电平故意不一样：权重 wA / wB 下的入口电平应是
            //      10·log10 ((wA·4·10^(a/10) + wB·6·10^(b/10)) ÷ (wA·4 + wB·6))，
            //    出口电平 = 它 + 参考增益。能量和与时长用的不是同一个权重的话，净增益（两侧相减）和证据时长都照样对得上，
            //    只有电平本身会偏——所以这里直接核入口电平。
            void testEvidenceWeighting()
            {
                beginTest ("evidenceAt weights each segment by how far its recorded gain is from the reference (full within 1 dB, none beyond 3 dB)");

                Tracker tracker;
                tracker.prepare (rate);
                auto quieter = linearChain (6.0f);
                quieter.inputScale = 0.5f;
                feed (tracker, 4 * blocksPerSecond, 200, linearChain (2.0f));
                tracker.closeSegment();
                feed (tracker, 6 * blocksPerSecond, 1200, quieter);

                const auto onlyA = tracker.evidenceAt (2.0f, fullDb, zeroDb);
                const auto onlyB = tracker.evidenceAt (6.0f, fullDb, zeroDb);
                const auto a = (double) onlyA.inputLufs, b = (double) onlyB.inputLufs;
                logMessage ("  input level of segment A " + juce::String (a, 4) + " LUFS, of segment B " + juce::String (b, 4) + " LUFS");
                expect (onlyA.valid() && onlyB.valid(), "fixture: each segment alone is evidence at its own recorded gain");
                expect (a - b > 5.5 && a - b < 6.5, "fixture: segment B's input is about 6 dB quieter than segment A's: " + juce::String (a - b, 3));

                struct Case { float reference; double weightA, weightB, seconds; };

                for (const auto& c : { Case { 2.0f, 1.0, 0.0, 4.0 }, Case { 6.0f, 0.0, 1.0, 6.0 }, Case { 4.0f, 0.5, 0.5, 5.0 },
                                       Case { 3.5f, 0.75, 0.25, 4.5 } })
                {
                    const auto e = tracker.evidenceAt (c.reference, fullDb, zeroDb);
                    const auto tag = "reference " + juce::String (c.reference, 1) + " dB: ";
                    const auto weightedSeconds = c.weightA * 4.0 + c.weightB * 6.0;
                    const auto expectedInput = 10.0 * std::log10 ((c.weightA * 4.0 * std::pow (10.0, 0.1 * a) + c.weightB * 6.0 * std::pow (10.0, 0.1 * b))
                                                                  / weightedSeconds);
                    logMessage ("  " + tag + juce::String (e.seconds, 4) + " s, net " + juce::String (e.netGainDb(), 4) + " dB, input "
                                + juce::String (e.inputLufs, 4) + " LUFS (expected " + juce::String (expectedInput, 4) + ")");
                    expect (e.valid(), tag + "has evidence");
                    expectWithinAbsoluteError (weightedSeconds, c.seconds, secondsTolerance, tag + "fixture: the weights add up to the seconds in the comment");
                    expectWithinAbsoluteError (e.seconds, c.seconds, secondsTolerance, tag + "weighted evidence seconds");
                    expectWithinAbsoluteError (e.netGainDb(), c.reference, levelToleranceDb, tag + "a linear chain reads a net gain equal to the reference");
                    expectWithinAbsoluteError ((double) e.inputLufs, expectedInput, (double) levelToleranceDb,
                                               tag + "the input level is the energy mean under the same weights as the seconds");
                }

                expect (noEvidence (tracker.evidenceAt (10.0f, fullDb, zeroDb)), "reference 10 dB: every segment is more than 3 dB away - no evidence");
                expect (noEvidence (tracker.evidenceAt (std::numeric_limits<float>::quiet_NaN(), fullDb, zeroDb)), "a non-finite reference gives no evidence");
                expect (noEvidence (tracker.evidenceAt (4.0f, fullDb, fullDb)), "full = zero = 1 dB: 2 dB away is past the edge - no evidence (and no division by zero)");
                expectWithinAbsoluteError (tracker.evidenceAt (4.0f, 2.0f, zeroDb).seconds, 10.0, secondsTolerance, "full within 2 dB: both segments count in full");
                expect (tracker.mainSamples() == samplesFor (10), "weighting does not change what counts as main: ten seconds");
            }

            // 3. 依据：OutputLevelTracker.h「待入账的入口能量…样本数和这一片对不上就不入账，所在的段入口没配齐、不进 evidenceAt()」
            //    「这一片没有 processOutput 就作废」（discardInput；下一次 processInput 也先清）。
            void testInputPairing()
            {
                beginTest ("evidenceAt only counts segments whose input was paired sample for sample; one slice's input never reaches another slice");

                // (a) 整段没交入口：主段样本照计，但不当证据。关段后喂 4 s 配齐的，只有这一段当证据。
                {
                    Tracker tracker;
                    tracker.prepare (rate);
                    Feed unpaired;
                    unpaired.withInput = false;
                    feed (tracker, 4 * blocksPerSecond, 300, unpaired);
                    expect (tracker.mainSamples() == samplesFor (4), "(a) the unpaired segment still counts as main samples");
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(a) a segment without input energy is not evidence");

                    tracker.closeSegment();
                    feed (tracker, 4 * blocksPerSecond, 700, Feed {});
                    const auto e = tracker.evidenceAt (0.0f, fullDb, zeroDb);
                    expect (e.valid(), "(a) the next, paired segment is evidence");
                    expectWithinAbsoluteError (e.seconds, 4.0, secondsTolerance, "(a) only the paired four seconds count");
                    // 出口那一路滤波器带着前一段的状态、入口那一路从零起（前一段没交入口），开头几毫秒两路略有出入：容差放到 0.01 dB。
                    expectWithinAbsoluteError (e.netGainDb(), 0.0f, 0.01f, "(a) output = input at 0 dB: net gain 0");
                    expect (tracker.mainSamples() == samplesFor (8), "(a) eight seconds of main samples in all");
                }

                // (b) 中间有一片入口样本数和出口对不上：这一段整段不当证据（两侧不是同一批样本）；关段后的下一段不受影响。
                {
                    Tracker tracker;
                    tracker.prepare (rate);
                    feed (tracker, 2 * blocksPerSecond, 1100, Feed {});
                    expectWithinAbsoluteError (tracker.evidenceAt (0.0f, fullDb, zeroDb).seconds, 2.0, secondsTolerance, "(b) two paired seconds so far");

                    const auto noise = makeNoise (numChannels, block, 0.1f, 1300);
                    juce::AudioBuffer<float> halfInput (numChannels, block / 2), output (numChannels, block);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        halfInput.copyFrom (ch, 0, noise[(size_t) ch].data(), block / 2);
                        output.copyFrom (ch, 0, noise[(size_t) ch].data(), block);
                    }

                    tracker.processInput (halfInput);
                    tracker.noteMainness (1.0f, true, -20.0f, 0.3f);
                    tracker.processOutput (output, 0.0f);
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(b) one mismatched slice takes its whole segment out of the evidence");

                    feed (tracker, 1 * blocksPerSecond, 1400, Feed {});
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(b) ... and more paired slices in the same segment do not bring it back");

                    tracker.closeSegment();
                    feed (tracker, 3 * blocksPerSecond, 1600, Feed {});
                    expectWithinAbsoluteError (tracker.evidenceAt (0.0f, fullDb, zeroDb).seconds, 3.0, secondsTolerance, "(b) the next segment is evidence again");
                }

                // (c) 一片交了入口、没有出口（冻结），下一片照常：上一片的入口能量不会算到下一片头上。
                //     响 18 dB 的一片（幅度 ×8，能量 ×64）漏进 1 s（100 片）里的话，入口电平会高 10·log10(1 + 64 ÷ 100) ≈ 2.1 dB。
                //     入口那一路滤波器是连续跑的，响的那一片留下的尾巴只影响开头几毫秒：容差 0.1 dB。出口一路两边喂的一样，逐位相同。
                {
                    Tracker stale, clean;
                    stale.prepare (rate);
                    clean.prepare (rate);
                    const auto loud = makeNoise (numChannels, block, 0.8f, 1900);
                    juce::AudioBuffer<float> loudInput (numChannels, block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        loudInput.copyFrom (ch, 0, loud[(size_t) ch].data(), block);

                    stale.processInput (loudInput);   // 这一片没有 processOutput
                    feed (stale, 1 * blocksPerSecond, 2000, Feed {});
                    feed (clean, 1 * blocksPerSecond, 2000, Feed {});
                    const auto a = stale.evidenceAt (0.0f, fullDb, zeroDb);
                    const auto b = clean.evidenceAt (0.0f, fullDb, zeroDb);
                    logMessage ("  (c) input level " + juce::String (a.inputLufs, 4) + " LUFS after a dropped loud slice vs " + juce::String (b.inputLufs, 4)
                                + " LUFS without it");
                    expect (a.valid() && b.valid() && a.seconds == b.seconds, "(c) both have the same one second of evidence");
                    expect (std::abs (a.inputLufs - b.inputLufs) < 0.1f, "(c) the dropped slice's input energy is not booked to the next slice");
                    expect (sameBits (a.levelLufs, b.levelLufs), "(c) the output side saw the same samples: bit-identical");
                }

                // (d) 一片交了入口后明说作废（discardInput：这一片不会有出口），下一片只有出口、没交入口：那一片不配对，
                //     所在的段不当证据——作废的入口能量配不到别的片上。不作废的话两片样本数相同，它就会被当成下一片的入口。
                {
                    Tracker tracker;
                    tracker.prepare (rate);
                    const auto noise = makeNoise (numChannels, block, 0.1f, 2300);
                    juce::AudioBuffer<float> buffer (numChannels, block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        buffer.copyFrom (ch, 0, noise[(size_t) ch].data(), block);

                    tracker.processInput (buffer);
                    tracker.discardInput();
                    tracker.noteMainness (1.0f, true, -20.0f, 0.3f);
                    tracker.processOutput (buffer, 0.0f);
                    expect (tracker.mainSamples() == (std::int64_t) block, "(d) the output slice is booked");
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(d) ... but it has no input paired to it: no evidence");
                }

                // (e) 一片的入口只用一次：正常喂 1 s 之后，不交入口再来一片出口——上一片的入口已经随上一片入账了，这一片没有入口
                //     可配，所在的段不当证据。（入账之后不清的话，样本数相同的下一片会把同一份入口能量再记一遍。）
                {
                    Tracker tracker;
                    tracker.prepare (rate);
                    feed (tracker, 1 * blocksPerSecond, 2600, Feed {});
                    expectWithinAbsoluteError (tracker.evidenceAt (0.0f, fullDb, zeroDb).seconds, 1.0, secondsTolerance, "(e) one paired second so far");

                    const auto noise = makeNoise (numChannels, block, 0.1f, 2800);
                    juce::AudioBuffer<float> buffer (numChannels, block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        buffer.copyFrom (ch, 0, noise[(size_t) ch].data(), block);

                    tracker.noteMainness (1.0f, true, -20.0f, 0.3f);
                    tracker.processOutput (buffer, 0.0f);
                    expect (tracker.mainSamples() == samplesFor (1) + (std::int64_t) block, "(e) the extra output slice is booked");
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(e) ... but it does not reuse the previous slice's input: no evidence");
                }

                // (f) (g) 依据：OutputLevelTracker.h processInput「这一片里有非有限的样本：不记待入账，这一片不配对」、processOutput
                //     「非有限的出口样本照旧计入段样本数、不计能量，也不算配上了入口」。正常喂 1 s 之后来一片，入口（f）或出口（g）
                //     有一个样本是 NaN / +∞：这一片照样记进主段样本，但所在的段不再当证据——那个样本的能量没量到，两侧不是同一批。
                //     对照：同样的一片不带坏样本，这一段还是证据（1.01 s）。
                {
                    const auto notANumber = std::numeric_limits<float>::quiet_NaN();
                    const auto infinity = std::numeric_limits<float>::infinity();

                    struct BadSample { bool inInput; float value; const char* what; };

                    for (const auto& bad : { BadSample { true, 0.0f, "(control) a clean slice" },
                                             BadSample { true, notANumber, "(f) a NaN input sample" }, BadSample { true, infinity, "(f) an infinite input sample" },
                                             BadSample { false, notANumber, "(g) a NaN output sample" }, BadSample { false, infinity, "(g) an infinite output sample" } })
                    {
                        Tracker tracker;
                        tracker.prepare (rate);
                        feed (tracker, 1 * blocksPerSecond, 3100, Feed {});

                        const auto noise = makeNoise (numChannels, block, 0.1f, 3300);
                        juce::AudioBuffer<float> input (numChannels, block), output (numChannels, block);

                        for (int ch = 0; ch < numChannels; ++ch)
                        {
                            input.copyFrom (ch, 0, noise[(size_t) ch].data(), block);
                            output.copyFrom (ch, 0, noise[(size_t) ch].data(), block);
                        }

                        const auto clean = std::isfinite (bad.value);

                        if (! clean)
                            (bad.inInput ? input : output).setSample (1, block / 2, bad.value);

                        tracker.processInput (input);
                        tracker.noteMainness (1.0f, true, -20.0f, 0.3f);
                        tracker.processOutput (output, 0.0f);
                        const auto e = tracker.evidenceAt (0.0f, fullDb, zeroDb);
                        const auto what = juce::String (bad.what);
                        expect (tracker.mainSamples() == samplesFor (1) + (std::int64_t) block, what + ": the slice is booked as main samples");

                        if (clean)
                            expectWithinAbsoluteError (e.seconds, 1.01, secondsTolerance, what + ": the segment is evidence, 1.01 s");
                        else
                            expect (noEvidence (e), what + " leaves the slice unpaired: its segment is no longer evidence");
                    }
                }
            }

            // 4. 依据：OutputLevelTracker.h「rejudge() 按此刻的参考把每一段重判一遍，只有此刻仍算主段的段才计入电平」「没有读数的段
            //    保持当时的判定」「judge 收到的是段内平均的输入侧段响度、低频占比」；noteMainness「静段、读不出…这一块不计入平均」
            //    （.cpp：measured 为假、读数不是有限值、低频占比为负的块都不计）。
            //    五段各 2 s，当时都判主段：
            //      ① 读数 −30 LUFS / 低频占比 0.5（轻的前奏）   ② −20 / 0.3   ③ 没有读数（measured 为假）
            //      ④ 前一半 −30 / 0.2、后一半 −10 / 0.4（段内平均 −20 / 0.3）
            //      ⑤ 说是有读数、但读数不能用：前一半段响度是 NaN，后一半低频占比为负——按没有读数记。
            //    重判「段响度 ≥ −25 才算主段」：① 出局，其余留下（③ ⑤ 没有读数、不重判）→ 8 s；judge 只被问到 ① ② ④ 三段。
            //    重判成「都算」→ 回到 10 s（可逆）。重判成「都不算」→ 只剩 ③ ⑤。judge 返回非有限值 → 按不算。
            void testRejudge()
            {
                beginTest ("rejudge: only segments that still count as main under the current reference are read; segments without readings keep their judgement");

                Tracker tracker;
                tracker.prepare (rate);
                Feed quiet, normal, unmeasured, firstHalf, secondHalf, badLoudness, badLowShare;
                quiet.inputLufs = -30.0f;
                quiet.lowShare = 0.5f;
                normal.inputLufs = -20.0f;
                unmeasured.measured = false;
                firstHalf.inputLufs = -30.0f;
                firstHalf.lowShare = 0.2f;
                secondHalf.inputLufs = -10.0f;
                secondHalf.lowShare = 0.4f;
                badLoudness.inputLufs = std::numeric_limits<float>::quiet_NaN();
                badLowShare.lowShare = -1.0f;

                feed (tracker, 2 * blocksPerSecond, 3000, quiet);
                tracker.closeSegment();
                feed (tracker, 2 * blocksPerSecond, 3300, normal);
                tracker.closeSegment();
                feed (tracker, 2 * blocksPerSecond, 3600, unmeasured);
                tracker.closeSegment();
                feed (tracker, 1 * blocksPerSecond, 3900, firstHalf);
                feed (tracker, 1 * blocksPerSecond, 4100, secondHalf);
                tracker.closeSegment();
                feed (tracker, 1 * blocksPerSecond, 4400, badLoudness);
                feed (tracker, 1 * blocksPerSecond, 4600, badLowShare);
                tracker.closeSegment();

                const auto secondsAt0 = [&] { return tracker.evidenceAt (0.0f, fullDb, zeroDb).seconds; };
                expect (tracker.mainSamples() == samplesFor (10), "before any rejudge all five segments count (their judgement at the time)");
                expectWithinAbsoluteError (secondsAt0(), 10.0, secondsTolerance, "... ten seconds of evidence");

                std::vector<std::pair<float, float>> seen;
                tracker.rejudge ([&] (float lufs, float low) noexcept
                {
                    seen.emplace_back (lufs, low);
                    return lufs >= -25.0f ? 1.0f : 0.0f;
                });

                expect (tracker.mainSamples() == samplesFor (8), "the quiet intro is judged out: eight seconds left");
                expectWithinAbsoluteError (secondsAt0(), 8.0, secondsTolerance, "... and the evidence follows");
                expect (seen.size() == 3, "the judge is asked about the three segments that carry usable readings, not the two without: "
                                          + juce::String ((int) seen.size()));

                // ① 的读数是 −30 / 0.5；② 与 ④ 的段内平均相同（−20 LUFS、低频占比 0.3：④ 是 (−30 + −10) ÷ 2 与 (0.2 + 0.4) ÷ 2）。
                int quietSeen = 0, at20 = 0;

                for (const auto& [lufs, low] : seen)
                {
                    quietSeen += std::abs (lufs + 30.0f) < 1.0e-4f && std::abs (low - 0.5f) < 1.0e-5f ? 1 : 0;
                    at20 += std::abs (lufs + 20.0f) < 1.0e-4f && std::abs (low - 0.3f) < 1.0e-5f ? 1 : 0;
                }

                expect (quietSeen == 1 && at20 == 2,
                        "the judge receives each segment's average reading (-30 / 0.5 once; -20 / 0.3 twice, one of them the mean of two halves)");

                const auto acceptAll = [] (float, float) noexcept { return 1.0f; };
                tracker.rejudge (acceptAll);
                expect (tracker.mainSamples() == samplesFor (10), "rejudging is reversible: with a reference that accepts everything all ten seconds are back");

                tracker.rejudge ([] (float, float) noexcept { return 0.0f; });
                expect (tracker.mainSamples() == samplesFor (4), "a reference that rejects everything leaves only the two segments without readings");
                expectWithinAbsoluteError (secondsAt0(), 4.0, secondsTolerance, "... and the evidence is those four seconds");

                // 非有限的判定按「不算」：先全收回来（10 s），再给 NaN，又只剩没有读数的两段。
                tracker.rejudge (acceptAll);
                expect (tracker.mainSamples() == samplesFor (10), "fixture: all ten seconds are back before the non-finite judgement");
                tracker.rejudge ([] (float, float) noexcept { return std::numeric_limits<float>::quiet_NaN(); });
                expect (tracker.mainSamples() == samplesFor (4), "a non-finite judgement counts as not main");

                // 正开着的段不归 rejudge 管（OutputLevelTracker.h「正开着的那一段不在重判之列：它每块都按此刻的参考现判」）：
                // 再喂 1 s 开着的主段，读数是 −30（按上面的参考会被判出局）。重判只问已收的三段；开着的这 1 s 一直按交来的 m′ 算。
                feed (tracker, 1 * blocksPerSecond, 4800, quiet);
                seen.clear();
                tracker.rejudge ([&] (float lufs, float low) noexcept
                {
                    seen.emplace_back (lufs, low);
                    return lufs >= -25.0f ? 1.0f : 0.0f;
                });
                expect (seen.size() == 3, "the open segment is not put to the judge (still the three closed segments with readings): "
                                          + juce::String ((int) seen.size()));
                expect (tracker.openSegmentIsMain() && tracker.mainSamples() == samplesFor (8 + 1),
                        "the open segment keeps the mainness it was fed: eight closed seconds plus its one");
                tracker.rejudge ([] (float, float) noexcept { return 0.0f; });
                expect (tracker.mainSamples() == samplesFor (4 + 1), "... also under a reference that rejects every closed segment with readings");
            }

            // 一份有内容的状态：三段已收（其中一段读数偏轻）＋一段开着，录制增益各不相同。
            static void fillForState (Tracker& tracker)
            {
                Feed quiet = linearChain (1.0f);
                quiet.inputLufs = -30.0f;
                quiet.inputScale = 0.3f;
                feed (tracker, 2 * blocksPerSecond, 5000, quiet);
                tracker.closeSegment();
                feed (tracker, 3 * blocksPerSecond, 5300, linearChain (2.0f));
                tracker.closeSegment();
                feed (tracker, 9 * blocksPerSecond, 5700, linearChain (2.5f));   // 满 8 s 自动切一段，余 1 s 开着
            }

            static bool sameReadings (const Tracker& a, const Tracker& b)
            {
                auto same = a.mainSamples() == b.mainSamples() && a.openSegmentIsMain() == b.openSegmentIsMain();

                for (const auto reference : { 0.0f, 1.0f, 2.0f, 2.5f, 4.0f })
                    same = same && sameEvidence (a.evidenceAt (reference, fullDb, zeroDb), b.evidenceAt (reference, fullDb, zeroDb));

                return same;
            }

            // 5. 依据：OutputLevelTracker.h「段累计进 LIVE」「T-106 的量另写七个属性，上面那些原样不动，0.1.8.10 照旧读得了这一节」
            //    「读回后各段先按当时的判定，下一次 rejudge() 起按此刻的参考」。
            //    断言：存 → 读进另一个跟踪器 → 再存，两份 XML 逐属性相同；读数（五个参考增益下的证据、主段样本数）逐位相同；
            //    十五个属性都在，两个整块数组的字节数是 768 / 1536；供体重判掉一段之后存的档，读回先按当时的判定（那一段还在），
            //    做同样的重判后与供体一致。读回后接着喂，两边仍逐位一致（滤波器状态不存：两边都先清滤波器）。
            void testStateRoundTrip()
            {
                beginTest ("state: create -> restore -> create is attribute-identical; readings are bit-identical; the eight 0.1.8.10 attributes are still written");

                Tracker donor;
                donor.prepare (rate);
                fillForState (donor);
                const auto judge = [] (float lufs, float) noexcept { return lufs >= -25.0f ? 1.0f : 0.0f; };
                const auto beforeRejudge = donor.mainSamples();
                expect (beforeRejudge == samplesFor (14), "fixture: fourteen seconds of main samples in four segments");

                const auto saved = donor.createStateXml();
                expect (saved != nullptr && saved->getTagName() == "OUTPUT_LEVEL", "the node is OUTPUT_LEVEL");

                if (saved == nullptr)
                    return;

                auto allPresent = saved->getNumAttributes() == 15;

                for (const auto* attribute : legacyAttributes)
                    allPresent = allPresent && saved->hasAttribute (attribute);

                for (const auto* attribute : extraAttributes)
                    allPresent = allPresent && saved->hasAttribute (attribute);

                expect (allPresent, "the node has the eight 0.1.8.10 attributes and the seven T-106 attributes, nothing else");

                juce::MemoryOutputStream closedData, extraData;
                const auto closedDecoded = juce::Base64::convertFromBase64 (closedData, saved->getStringAttribute ("closed"));
                const auto extraDecoded = juce::Base64::convertFromBase64 (extraData, saved->getStringAttribute ("closedExtra"));
                expect (closedDecoded && closedData.getDataSize() == closedBytes,
                        "closed is still 32 segments x 24 bytes (what 0.1.8.10 reads): " + juce::String ((int) closedData.getDataSize()));
                expect (extraDecoded && extraData.getDataSize() == closedExtraBytes,
                        "closedExtra is 32 x 48 bytes: " + juce::String ((int) extraData.getDataSize()));

                Tracker restored;
                restored.prepare (rate);
                expect (restored.restoreStateXml (*saved), "a state just written restores");
                expect (! restored.restoredLegacyState(), "... and is not taken for a legacy state");
                const auto again = restored.createStateXml();
                expect (again != nullptr && again->isEquivalentTo (saved.get(), false), "create -> restore -> create is attribute-identical");
                expect (sameReadings (donor, restored), "readings are bit-identical after the restore");

                // 重判不进存档：供体重判掉偏轻的那一段再存，读回的先按当时的判定。
                donor.rejudge (judge);
                expect (donor.mainSamples() == samplesFor (12), "fixture: the rejudge takes the quiet two seconds out");
                const auto savedAfterRejudge = donor.createStateXml();
                expect (savedAfterRejudge != nullptr && savedAfterRejudge->isEquivalentTo (saved.get(), false), "a rejudge does not change what is saved");

                Tracker second;
                second.prepare (rate);
                expect (savedAfterRejudge != nullptr && second.restoreStateXml (*savedAfterRejudge), "restores");
                expect (second.mainSamples() == beforeRejudge, "after the restore every stored segment counts again (its judgement at the time)");
                second.rejudge (judge);
                expect (sameReadings (donor, second), "... and the same rejudge brings it in line with the donor");

                // 接着喂：两边滤波器都从零起（读档不带滤波器状态；供体这边手动清），之后逐位一致。
                donor.resetFilters();
                feed (donor, 2 * blocksPerSecond, 6600, linearChain (2.5f));
                feed (second, 2 * blocksPerSecond, 6600, linearChain (2.5f));
                donor.rejudge (judge);
                second.rejudge (judge);
                expect (sameReadings (donor, second), "feeding both the same material afterwards keeps them bit-identical");
                const auto donorAfter = donor.createStateXml();
                const auto secondAfter = second.createStateXml();
                expect (donorAfter != nullptr && secondAfter != nullptr && donorAfter->isEquivalentTo (secondAfter.get(), false),
                        "... and so are their saved states");
            }

            // 6. 依据：OutputLevelTracker.h「没有 T-106 字段的旧存档：旧字段照常校验，然后段累计清空、restoredLegacyState() 置 true」
            //    「T-106 字段在但不全或坏了：整节拒收，同别的字段」。拒收时状态一点不动。
            void testLegacyAndBrokenState()
            {
                beginTest ("state: a node without the T-106 attributes restores as an empty history (legacy); a partial or broken set is rejected and changes nothing");

                Tracker donor;
                donor.prepare (rate);
                fillForState (donor);
                const auto saved = donor.createStateXml();
                expect (saved != nullptr);

                if (saved == nullptr)
                    return;

                // 目标先装着别的内容：拒收之后要原样还在，接受旧档之后要清空。
                const auto makeTarget = [&] (Tracker& target)
                {
                    target.prepare (rate);
                    feed (target, 5 * blocksPerSecond, 7000, linearChain (3.0f));
                };

                // ---- 旧形状：七个属性都没有 ----
                {
                    juce::XmlElement legacy (*saved);

                    for (const auto* attribute : extraAttributes)
                        legacy.removeAttribute (attribute);

                    Tracker target;
                    makeTarget (target);
                    expect (target.mainSamples() == samplesFor (5) && ! target.restoredLegacyState(), "fixture: the target holds five seconds of its own");
                    expect (target.restoreStateXml (legacy), "legacy node: accepted");
                    expect (target.restoredLegacyState(), "legacy node: flagged as legacy (the orchestrator resets G_trim on it)");
                    expect (target.mainSamples() == 0 && ! target.openSegmentIsMain() && noEvidence (target.evidenceAt (0.0f, fullDb, zeroDb)),
                            "legacy node: the history is empty (neither the saved one nor what the target held)");

                    feed (target, 4 * blocksPerSecond, 7600, Feed {});
                    expectWithinAbsoluteError (target.evidenceAt (0.0f, fullDb, zeroDb).seconds, 4.0, secondsTolerance, "legacy node: new material builds evidence from scratch");

                    const auto upgraded = target.createStateXml();
                    Tracker next;
                    next.prepare (rate);
                    expect (upgraded != nullptr && upgraded->getNumAttributes() == 15 && next.restoreStateXml (*upgraded) && ! next.restoredLegacyState(),
                            "legacy node: saved again it is in the new shape and no longer legacy");

                    // 旧字段坏了的旧档照旧拒收（closedCount 写成 99：环只有 32 段；整数字段是十进制）。
                    juce::XmlElement brokenLegacy (legacy);
                    brokenLegacy.setAttribute ("closedCount", "99");
                    Tracker keep;
                    makeTarget (keep);
                    const auto before = keep.createStateXml();
                    expect (! keep.restoreStateXml (brokenLegacy), "legacy node with a broken old field: rejected");
                    const auto after = keep.createStateXml();
                    expect (before != nullptr && after != nullptr && after->isEquivalentTo (before.get(), false) && ! keep.restoredLegacyState(),
                            "... and the target is untouched");
                }

                // ---- 拒收：每一种都核「返回 false、目标的状态逐属性不变」----
                const auto expectRejected = [&] (const juce::XmlElement& node, const juce::String& what)
                {
                    Tracker target;
                    makeTarget (target);
                    const auto before = target.createStateXml();
                    const auto accepted = target.restoreStateXml (node);
                    const auto after = target.createStateXml();
                    expect (! accepted, what + ": rejected");
                    expect (before != nullptr && after != nullptr && after->isEquivalentTo (before.get(), false) && ! target.restoredLegacyState()
                                && target.mainSamples() == samplesFor (5),
                            what + ": the target is untouched");
                };

                for (const auto* attribute : extraAttributes)
                {
                    juce::XmlElement partial (*saved);
                    partial.removeAttribute (attribute);
                    expectRejected (partial, "T-106 attributes present but " + juce::String (attribute) + " missing");
                }

                {
                    // 内容原样、只有标签不对。
                    juce::XmlElement wrongTag (*saved);
                    wrongTag.setTagName ("OUTPUT_LEVELS");
                    expectRejected (wrongTag, "wrong tag");
                }

                {
                    juce::XmlElement truncated (*saved);
                    truncated.setAttribute ("closedExtra", juce::Base64::toBase64 ("short", (size_t) 5));
                    expectRejected (truncated, "closedExtra of the wrong size");
                }

                // ---- 每段另记的六个量逐项验（OutputLevelTracker.cpp validExtra）----
                // 开着的那一段（1 s = 48000 个样本）的量是六个属性；已收的段在 closedExtra 整块里，每段 48 字节：四个 double（折回 0 dB 的
                // 出口能量、入口能量、段响度读数之和、低频占比读数之和），再两个 int64（有读数的样本数、配上入口的样本数）。
                // 这里改第 0 段（fillForState 里最先收的那一段，2 s = 96000 个样本），它从字节 0 起。
                // 规矩：两个能量与低频占比之和不能为负；四个 double 都要是有限值（段响度之和可以为负，LUFS 本来就是负的）；
                // 两个样本数在 0 ～ 该段样本数之间。合法的一侧由上一条的往返覆盖：存下来的状态里这些量都有值（两个样本数正好等于
                // 段样本数），没用到的 29 段全是 0。
                juce::MemoryOutputStream decodedExtra;
                const auto extraDecoded = juce::Base64::convertFromBase64 (decodedExtra, saved->getStringAttribute ("closedExtra"));
                expect (extraDecoded && decodedExtra.getDataSize() == closedExtraBytes, "fixture: closedExtra decodes to 32 x 48 bytes");

                if (! extraDecoded || decodedExtra.getDataSize() != closedExtraBytes)
                    return;

                const auto withClosedExtra = [&] (size_t offset, const void* value, size_t size)
                {
                    juce::MemoryBlock bytes (decodedExtra.getData(), decodedExtra.getDataSize());
                    std::memcpy (static_cast<char*> (bytes.getData()) + offset, value, size);
                    juce::XmlElement node (*saved);
                    node.setAttribute ("closedExtra", juce::Base64::toBase64 (bytes.getData(), bytes.getSize()));
                    return node;
                };

                const auto notANumber = std::numeric_limits<double>::quiet_NaN();
                const auto infinity = std::numeric_limits<double>::infinity();

                struct BadDouble
                {
                    const char* attribute;   // 开着的段：属性名
                    size_t closedOffset;     // 已收的第 0 段：这个量在 closedExtra 里的字节位置
                    double value;
                    const char* what;
                };

                for (const auto& bad : { BadDouble { "openUnityEnergy", 0, -1.0, "negative" }, BadDouble { "openUnityEnergy", 0, notANumber, "NaN" },
                                         BadDouble { "openUnityEnergy", 0, infinity, "infinite" },
                                         BadDouble { "openInputEnergy", 8, -1.0, "negative" }, BadDouble { "openInputEnergy", 8, notANumber, "NaN" },
                                         BadDouble { "openInputEnergy", 8, infinity, "infinite" },
                                         BadDouble { "openInputLufsSum", 16, notANumber, "NaN" }, BadDouble { "openInputLufsSum", 16, -infinity, "infinite" },
                                         BadDouble { "openLowShareSum", 24, -1.0, "negative" }, BadDouble { "openLowShareSum", 24, notANumber, "NaN" },
                                         BadDouble { "openLowShareSum", 24, infinity, "infinite" } })
                {
                    juce::XmlElement openNode (*saved);
                    glaze::xmlio::Writer (openNode).field (bad.attribute, bad.value);
                    expectRejected (openNode, juce::String (bad.what) + " " + bad.attribute);
                    expectRejected (withClosedExtra (bad.closedOffset, &bad.value, sizeof (bad.value)),
                                    juce::String (bad.what) + " double at byte " + juce::String ((int) bad.closedOffset) + " of closedExtra");
                }

                struct BadCount
                {
                    const char* attribute;
                    size_t closedOffset;
                    std::int64_t openValue;     // 开着的段有 48000 个样本
                    std::int64_t closedValue;   // 第 0 段有 96000 个样本
                    const char* what;
                };

                for (const auto& bad : { BadCount { "openInputSamples", 32, -1, -1, "negative" },
                                         BadCount { "openInputSamples", 32, 48001, 96001, "more than the segment's samples:" },
                                         BadCount { "openPairedSamples", 40, -1, -1, "negative" },
                                         BadCount { "openPairedSamples", 40, 48001, 96001, "more than the segment's samples:" } })
                {
                    juce::XmlElement openNode (*saved);
                    openNode.setAttribute (bad.attribute, juce::String (bad.openValue));
                    expectRejected (openNode, juce::String (bad.what) + " " + bad.attribute);
                    expectRejected (withClosedExtra (bad.closedOffset, &bad.closedValue, sizeof (bad.closedValue)),
                                    juce::String (bad.what) + " count at byte " + juce::String ((int) bad.closedOffset) + " of closedExtra");
                }

                // ---- 新格式里段本身也验（OutputLevelTracker.cpp validSegment）、三个样本数严格按十进制读（parseCount）----
                // closed 整块里每段 24 字节：double 能量（+0）、int64 样本数（+8）、float 主段度（+16）。第 0 段 = fillForState 最先收的
                // 那一段（96000 个样本）；第 5 段没用到、全是 0，它另记的量也都是 0——样本数写成负的，别的检查（另记的样本数不超过
                // 段样本数）拦不住，只有段本身这一道拦。
                // 规矩：能量有限且非负；样本数在 0 ～ 8 s × 768 kHz（6144000）；主段度有限。正好在界上的收下。
                juce::MemoryOutputStream decodedClosed;
                const auto closedDecoded = juce::Base64::convertFromBase64 (decodedClosed, saved->getStringAttribute ("closed"));
                expect (closedDecoded && decodedClosed.getDataSize() == closedBytes, "fixture: closed decodes to 32 x 24 bytes");

                if (! closedDecoded || decodedClosed.getDataSize() != closedBytes)
                    return;

                const auto withClosed = [&] (size_t offset, const void* value, size_t size)
                {
                    juce::MemoryBlock bytes (decodedClosed.getData(), decodedClosed.getDataSize());
                    std::memcpy (static_cast<char*> (bytes.getData()) + offset, value, size);
                    juce::XmlElement node (*saved);
                    node.setAttribute ("closed", juce::Base64::toBase64 (bytes.getData(), bytes.getSize()));
                    return node;
                };

                const auto expectAccepted = [&] (const juce::XmlElement& node, const juce::String& what)
                {
                    Tracker target;
                    makeTarget (target);
                    expect (target.restoreStateXml (node) && ! target.restoredLegacyState(), what + ": accepted");
                };

                constexpr size_t segmentBytes = 24, energyAt = 0, samplesAt = 8, mainnessAt = 16;
                constexpr size_t unusedSegment = 5;

                {
                    juce::XmlElement truncated (*saved);
                    truncated.setAttribute ("closed", juce::Base64::toBase64 ("short", (size_t) 5));
                    expectRejected (truncated, "closed of the wrong size");
                }

                struct BadEnergy   { double value; const char* what; };
                struct BadMainness { float value; const char* what; };

                for (const auto& bad : { BadEnergy { -1.0, "negative" }, BadEnergy { notANumber, "NaN" }, BadEnergy { infinity, "infinite" } })
                {
                    expectRejected (withClosed (energyAt, &bad.value, sizeof (bad.value)), juce::String (bad.what) + " energy in closed segment 0");
                    juce::XmlElement openNode (*saved);
                    glaze::xmlio::Writer (openNode).field ("openEnergy", bad.value);
                    expectRejected (openNode, juce::String (bad.what) + " openEnergy");
                }

                for (const auto& bad : { BadMainness { std::numeric_limits<float>::quiet_NaN(), "NaN" },
                                         BadMainness { std::numeric_limits<float>::infinity(), "infinite" } })
                {
                    expectRejected (withClosed (mainnessAt, &bad.value, sizeof (bad.value)), juce::String (bad.what) + " mainness in closed segment 0");
                    juce::XmlElement openNode (*saved);
                    glaze::xmlio::Writer (openNode).field ("openMainness", bad.value);
                    expectRejected (openNode, juce::String (bad.what) + " openMainness");
                }

                {
                    const std::int64_t negative = -1, atLimit = maximumSegmentSamples, pastLimit = maximumSegmentSamples + 1;
                    expectRejected (withClosed (unusedSegment * segmentBytes + samplesAt, &negative, sizeof (negative)),
                                    "negative sample count in an otherwise empty closed segment");
                    expectRejected (withClosed (samplesAt, &pastLimit, sizeof (pastLimit)), "closed segment 0 with more samples than 8 s at 768 kHz");
                    expectAccepted (withClosed (samplesAt, &atLimit, sizeof (atLimit)), "closed segment 0 with exactly 8 s at 768 kHz of samples");

                    juce::XmlElement openPast (*saved), openAtLimit (*saved);
                    openPast.setAttribute ("openSamples", juce::String (pastLimit));
                    openAtLimit.setAttribute ("openSamples", juce::String (atLimit));
                    expectRejected (openPast, "openSamples past 8 s at 768 kHz");
                    expectAccepted (openAtLimit, "openSamples of exactly 8 s at 768 kHz");
                }

                // 三个样本数：空、不是数字、数字后面带别的、前面带空格或正负号、小数——都拒收。juce 的 getLargeIntValue 会把
                // 「garbage」读成 0、把「48000x」读成 48000（都能混过后面的范围检查），所以这里不能用它。
                for (const auto* attribute : { "openSamples", "openInputSamples", "openPairedSamples" })
                    for (const auto* text : { "", "garbage", "48000x", " 48000", "+48000", "-0", "48000.0" })
                    {
                        juce::XmlElement node (*saved);
                        node.setAttribute (attribute, text);
                        expectRejected (node, juce::String (attribute) + " written as \"" + text + "\"");
                    }
            }

            // 7. 依据：OutputLevelTracker.h「m′ ≥ 0.5 的段才进入主段电平」「跨过 0.5，或调用方 closeSegment()，就把上一段收进主段账」
            //    （Q9 的门；T-106 每段另记的量跟着段走）。
            //    主段 2 s（线性的链，2 dB）→ 非主段 3 s（m′ = 0.2；出口故意放大 10 倍，漏进读数就差得远）→ 主段 2 s（2 dB）。
            //    断言：主段样本 4 s、证据 4 s、净增益 2 dB——非主段那 3 s 两侧都没进账；非主段期间 openSegmentIsMain() 为假；
            //    回到主段后新开的段从零记起（上一截另记的量带过来的话，它「配上入口的样本数」和段样本数对不上、不当证据）。
            //    门槛正好在 0.5：m′ = 0.5 算主段，0.49 不算。
            void testMainnessFlips()
            {
                beginTest ("only main segments (m' >= 0.5) are booked; a non-main stretch in between leaves no trace on either side");

                Tracker tracker;
                tracker.prepare (rate);
                feed (tracker, 2 * blocksPerSecond, 8000, linearChain (2.0f));
                expect (tracker.openSegmentIsMain(), "the open segment is main while m' = 1");

                Feed nonMain;
                nonMain.mainness = 0.2f;
                nonMain.outputGain = 10.0f;
                feed (tracker, 3 * blocksPerSecond, 8300, nonMain);
                expect (! tracker.openSegmentIsMain(), "... and not main while m' = 0.2");
                expect (tracker.mainSamples() == samplesFor (2), "the non-main stretch adds no main samples");

                feed (tracker, 2 * blocksPerSecond, 8700, linearChain (2.0f));
                const auto e = tracker.evidenceAt (2.0f, fullDb, zeroDb);
                logMessage ("  two main segments around a non-main stretch: " + juce::String (e.seconds, 4) + " s of evidence, net "
                            + juce::String (e.netGainDb(), 4) + " dB");
                expect (tracker.openSegmentIsMain() && tracker.mainSamples() == samplesFor (4),
                        "back in a main section: four seconds of main samples in two segments");
                expectWithinAbsoluteError (e.seconds, 4.0, secondsTolerance, "both main segments are evidence (the new one started from zero)");
                // 出口那一路滤波器带着 ×10 那一截的尾巴进了新段的头几毫秒：容差 0.01 dB。
                expectWithinAbsoluteError (e.netGainDb(), 2.0f, 0.01f, "the net gain is that of the main segments alone (the x10 stretch is not in it)");

                Tracker edge;
                edge.prepare (rate);
                Feed atGate, belowGate;
                atGate.mainness = 0.5f;
                belowGate.mainness = 0.49f;
                feed (edge, 1 * blocksPerSecond, 9000, atGate);
                expect (edge.openSegmentIsMain() && edge.mainSamples() == samplesFor (1), "m' = 0.5 counts as main");
                feed (edge, 1 * blocksPerSecond, 9200, belowGate);
                expect (! edge.openSegmentIsMain() && edge.mainSamples() == samplesFor (1), "m' = 0.49 does not (and the closed main second stays)");
            }

            // 8. 依据：OutputLevelTracker.h evidenceAt「没有出口能量的段（数字静音）量不出录制增益、折算不了：不收，也不占时长」。
            //    (a) 入口、出口都是数字静音的 4 s 主段（段落账读不出时 m′ 按 1 交来：宿主停着喂静音就是这样），关段，再来 2 s 正常的：
            //        主段样本 6 s，证据只有那 2 s——静音凑不了「至少 3 s 证据」。只有静音时没有证据。
            //    (b) 入口正常、出口是数字静音的 4 s，关段，再来 2 s 线性的链（0 dB）：证据 2 s、净增益 0 dB。
            //        把静音那一段也算进去的话，入口能量多出三分之二、出口一点没有，净增益会读成 10·log10 (2 ÷ 6) = −4.77 dB。
            void testSegmentsWithoutOutputEnergyAreNotEvidence()
            {
                beginTest ("a segment with no output energy (digital silence) is not evidence and adds no evidence seconds");

                {
                    Tracker tracker;
                    tracker.prepare (rate);
                    Feed silence;
                    silence.inputScale = 0.0f;
                    feed (tracker, 4 * blocksPerSecond, 9500, silence);
                    expect (tracker.openSegmentIsMain() && tracker.mainSamples() == samplesFor (4), "(a) fixture: four silent seconds are booked as a main segment");
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(a) silence alone is no evidence");

                    tracker.closeSegment();
                    feed (tracker, 2 * blocksPerSecond, 9900, Feed {});
                    const auto e = tracker.evidenceAt (0.0f, fullDb, zeroDb);
                    expect (tracker.mainSamples() == samplesFor (6), "(a) six seconds of main samples in all");
                    expect (e.valid(), "(a) the two seconds of material are evidence");
                    expectWithinAbsoluteError (e.seconds, 2.0, secondsTolerance, "(a) the silent segment adds no evidence seconds");
                    expectWithinAbsoluteError (e.netGainDb(), 0.0f, levelToleranceDb, "(a) output = input at 0 dB: net gain 0");
                }

                {
                    Tracker tracker;
                    tracker.prepare (rate);
                    Feed mutedOutput;
                    mutedOutput.outputGain = 0.0f;
                    feed (tracker, 4 * blocksPerSecond, 10300, mutedOutput);
                    expect (tracker.mainSamples() == samplesFor (4), "(b) fixture: four seconds with a silent output are booked as a main segment");
                    expect (noEvidence (tracker.evidenceAt (0.0f, fullDb, zeroDb)), "(b) a segment without output energy is no evidence, whatever its input");

                    tracker.closeSegment();
                    feed (tracker, 2 * blocksPerSecond, 10700, Feed {});
                    const auto e = tracker.evidenceAt (0.0f, fullDb, zeroDb);
                    logMessage ("  (b) after a muted segment: " + juce::String (e.seconds, 4) + " s of evidence, net " + juce::String (e.netGainDb(), 4) + " dB");
                    expect (e.valid(), "(b) the two seconds that follow are evidence");
                    expectWithinAbsoluteError (e.seconds, 2.0, secondsTolerance, "(b) only those two seconds count");
                    // 入口那一路滤波器连续跑过了前 4 s、出口那一路吃的是静音，新段开头几毫秒两路略有出入：容差 0.01 dB。
                    expectWithinAbsoluteError (e.netGainDb(), 0.0f, 0.01f, "(b) the muted segment's input energy is not in the reading (it would read -4.77 dB)");
                }
            }
        };

        static OutputLevelTrackerTests outputLevelTrackerTests;
    }
}
