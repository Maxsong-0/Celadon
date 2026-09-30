/*
  ==============================================================================

    LimiterTests.cpp
    设计规格-Limit「验收 · 自动测试」四条 + 分块无关性 + Lookahead 变动；
    v0.5：Clip 级空闲跳过与「永不跳过」的参照逐位相同。
    v0.1.8.0：试听差值（process 的 difference 输出）——不改输出、没衰减时恰为 0、与输出拼回加了 Gain 的输入、旁通与淡入时归零、分块无关。

  ==============================================================================
*/

#include "TestHelpers.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace celadon::test
{
    namespace
    {
        constexpr int numChannels = 2;
        constexpr int maxBlock = 512;

        LimiterModule::Params makeParams (float gainDb, float ceilingDb, float lookaheadMs, bool autoRelease, float clip)
        {
            LimiterModule::Params p;
            p.gainDb = gainDb;
            p.ceilingDb = ceilingDb;
            p.lookaheadMs = lookaheadMs;
            p.autoRelease = autoRelease;
            p.releaseMs = 100.0f;
            p.clip = clip;
            return p;
        }

        void prepareLimiter (LimiterModule& limiter, double sampleRate, const LimiterModule::Params& p)
        {
            limiter.setParams (p);
            limiter.prepare ({ sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels });
        }

        struct NamedSignal
        {
            const char* name;
            Signal signal;
        };

        std::vector<NamedSignal> makePeakTestSignals (double sampleRate)
        {
            std::vector<NamedSignal> signals;
            signals.push_back ({ "1 kHz sine 0 dBFS",   makeSine (numChannels, sampleRate, 1000.0, 1.0f, 0.5) });
            signals.push_back ({ "100 Hz square",       makeSquare (numChannels, sampleRate, 100.0, 1.0f, 0.5) });
            signals.push_back ({ "unit impulse",        makeImpulse (numChannels, (int) (0.3 * sampleRate), (int) (0.1 * sampleRate), 1.0f) });
            signals.push_back ({ "20 ms burst",         makeBurst (numChannels, sampleRate, 1.0f, 0.3) });

            // 采样点间峰值：fs/4 正弦、相位 45°，采样点全是 ±0.707，真峰值是 1.0（+3 dB）。
            signals.push_back ({ "fs/4 sine at 45 deg", makeSine (numChannels, sampleRate, sampleRate / 4.0, 1.0f, 0.5,
                                                                  juce::MathConstants<double>::pi / 4.0) });
            return signals;
        }

        Signal makeLowHighSignal (double sampleRate, double lowHz, double seconds)
        {
            auto signal = makeSilence (numChannels, (int) (sampleRate * seconds));
            for (int i = 0; i < (int) signal[0].size(); ++i)
            {
                const auto t = i / sampleRate;
                const auto x = (float) (0.62 * std::sin (juce::MathConstants<double>::twoPi * lowHz * t)
                                     + 0.12 * std::sin (juce::MathConstants<double>::twoPi * 6000.0 * t));
                signal[0][(size_t) i] = x;
                signal[1][(size_t) i] = -x; // pure Side must not disappear from the low-frequency observer
            }
            return signal;
        }

        double lastSecondAmplitude (const Signal& signal, double sampleRate, double frequency)
        {
            const auto count = (int) sampleRate;
            const auto first = (int) signal[0].size() - count;
            double real = 0.0, imaginary = 0.0;
            for (int i = 0; i < count; ++i)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
                real += signal[0][(size_t) (first + i)] * std::cos (phase);
                imaginary += signal[0][(size_t) (first + i)] * std::sin (phase);
            }
            return 2.0 * std::sqrt (real * real + imaginary * imaginary) / count;
        }

        // runLimiter 的试听差值版本：同一趟处理同时取回输出与差值。
        struct OutputAndDifference
        {
            Signal output, difference;
        };

        OutputAndDifference runLimiterWithDifference (LimiterModule& limiter, const Signal& input, int blockSize)
        {
            const auto numSamples = (int) input.front().size();
            OutputAndDifference result { input, input };

            juce::AudioBuffer<float> block (numChannels, blockSize), difference (numChannels, blockSize);

            for (int start = 0; start < numSamples; start += blockSize)
            {
                const auto length = juce::jmin (blockSize, numSamples - start);
                block.setSize (numChannels, length, false, false, true);
                difference.setSize (numChannels, length, false, false, true);
                difference.clear();

                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                limiter.process (block, &difference);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + length, result.output[(size_t) ch].begin() + start);
                    std::copy (difference.getReadPointer (ch), difference.getReadPointer (ch) + length,
                               result.difference[(size_t) ch].begin() + start);
                }
            }

            return result;
        }

        // 响的音乐样材料：低频 + 中频 + 噪声，峰值约 0 dBFS（+9 dB 推进去会被结实地限幅）。
        Signal makeListenLikeMaterial (double sampleRate)
        {
            auto input = makeNoise (numChannels, (int) (0.8 * sampleRate), 0.25f, 29);
            for (const auto& component : { std::pair<double, float> { 55.0, 0.45f }, { 880.0, 0.25f } })
            {
                const auto tone = makeSine (numChannels, sampleRate, component.first, component.second, 0.8);
                for (int ch = 0; ch < numChannels; ++ch)
                    for (size_t i = 0; i < input[(size_t) ch].size(); ++i)
                        input[(size_t) ch][i] += tone[(size_t) ch][i];
            }
            return input;
        }

        float peakOf (const Signal& signal)
        {
            float peak = 0.0f;
            for (const auto& channel : signal)
                for (auto sample : channel)
                    peak = juce::jmax (peak, std::abs (sample));
            return peak;
        }

        double lastSecondRms (const Signal& signal, double sampleRate)
        {
            const auto count = (int) sampleRate;
            const auto first = (int) signal[0].size() - count;
            double energy = 0.0;
            for (int i = 0; i < count; ++i)
                energy += juce::square ((double) signal[0][(size_t) (first + i)]);
            return std::sqrt (energy / count);
        }

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        // 输出的逐位指纹（FNV-1a 64 位，按每个样本的 32 位位模式、声道依次喂入）：只打进日志，不断言。
        // 用途：换核 / 换算法但声称「输出逐位不变」时，改动前后各跑一次、对照日志里的指纹（T-032：SSE2 预广播系数表、
        // van Herk 滑动最小值、ceilingDelay 重置填法三项都靠它取证）。同一平台、同一编译选项下指纹才可比（NEON 与 SSE2 本来就不同）。
        struct Fingerprint
        {
            std::uint64_t hash = 14695981039346656037ull;

            void add (const float* samples, int count) noexcept
            {
                for (int i = 0; i < count; ++i)
                {
                    std::uint32_t bits = 0;
                    std::memcpy (&bits, samples + i, sizeof (bits));

                    for (int shift = 0; shift < 32; shift += 8)
                    {
                        hash ^= (std::uint64_t) ((bits >> shift) & 0xffu);
                        hash *= 1099511628211ull;
                    }
                }
            }

            void add (const Signal& signal) noexcept
            {
                for (const auto& channel : signal)
                    add (channel.data(), (int) channel.size());
            }

            void add (const juce::AudioBuffer<float>& buffer) noexcept
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    add (buffer.getReadPointer (ch), buffer.getNumSamples());
            }

            juce::String toString() const
            {
                return juce::String::toHexString ((juce::int64) hash).paddedLeft ('0', 16);
            }
        };
    }

    class LimiterTests  : public juce::UnitTest
    {
    public:
        LimiterTests() : juce::UnitTest ("LimiterModule", "Celadon") {}

        void runTest() override
        {
            testTruePeakNeverExceedsCeiling();
            testIndependentTruePeakAcceptance();
            testNullWhenNothingToDo();
            testBypassIsBitExact();
            testMeasuredLatencyMatchesReported();
            testNoNonFiniteOutput();
            testBlockSizeIndependence();
            testLookaheadChange();
            testGainReductionMeter();
            testGainReturnsExactlyToUnity();
            testRestartWithFadeIn();
            testGainIsSmoothed();
            testClipIdleSkipIsBitExact();
            testClipIdleSkipWithFewerChannelsIsSafe();
            testDifferenceIsZeroWithoutReduction();
            testDifferenceLeavesOutputUntouched();
            testDifferenceReconstructsGainedInput();
            testDifferenceFollowsBypassAndFadeIn();
            testLowFrequencyReleaseReducesIntermodulation();
            testLowFrequencyReleasePreservesTransientsAndManual();
            testLowFrequencyReleaseRejectsUnstablePeriods();
            testLowFrequencyReleaseBlockResetAndAllocation();
            testShortOverloadDoesNotHoldTheSlowEnvelope();
            testSlidingMinimumMatchesReferences();
            testHardClipCeilingDelayFollowsResets();
            testParamsGuardNonFiniteAndOutOfRange();
        }

    private:
        void testShortOverloadDoesNotHoldTheSlowEnvelope()
        {
            beginTest ("a short overload releases with the fast envelope; a sustained overload still holds");

            constexpr double sampleRate = 48000.0;
            constexpr float quiet = 0.08f;
            constexpr float hot = 0.95f;
            const auto params = makeParams (8.0f, -1.0f, 2.0f, true, 0.0f);

            const auto render = [&] (double hotSeconds)
            {
                constexpr double lead = 0.35;
                constexpr double tail = 0.45;
                const auto total = lead + hotSeconds + tail;
                auto signal = makeSine (numChannels, sampleRate, 3000.0, quiet, total);
                const auto hotStart = (int) (lead * sampleRate);
                const auto hotEnd = hotStart + (int) (hotSeconds * sampleRate);

                for (int i = hotStart; i < hotEnd; ++i)
                    for (auto& channel : signal)
                        channel[(size_t) i] = hot * (float) std::sin (juce::MathConstants<double>::twoPi * 3000.0 * i / sampleRate);

                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, params);
                const auto output = runLimiter (limiter, signal, 128);
                const auto latency = limiter.getLatencySamples();
                const auto measureAt = hotEnd + latency + (int) (0.045 * sampleRate);
                const auto count = (int) (0.025 * sampleRate);
                double energy = 0.0;

                for (int i = 0; i < count; ++i)
                    energy += juce::square ((double) output[0][(size_t) (measureAt + i)]);

                return std::sqrt (energy / count);
            };

            const auto afterShort = render (0.012);
            const auto afterLong = render (0.280);
            logMessage ("  RMS 45 ms after short overload " + juce::String (toDb (afterShort), 2)
                        + " dB, after sustained overload " + juce::String (toDb (afterLong), 2) + " dB");

            expect (std::isfinite (afterShort) && std::isfinite (afterLong));
            expect (afterShort > 1.0e-4, "the tail after a short overload must still be present");
            expect (toDb (afterShort / juce::jmax (1.0e-12, afterLong)) >= 1.2f,
                    "a sustained overload must keep ducking after the short overload has let go");
        }

        // 试听差值 ①：限幅器什么都没削（输入 + Gain 仍低于内部阈值、Clip 0）时差值逐位为 0——包括 Gain 在 20 ms 平滑里变动：
        // 「加了 Gain 的输入」由干路 × 同样延迟过的逐样本 Gain 重建，与当初送进 Clip 级的样本逐位相同（LimiterModule.h 文件头）。
        void testDifferenceIsZeroWithoutReduction()
        {
            beginTest ("difference is exactly zero when nothing is limited, even while Gain is ramping");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (0.0f, -1.0f, lookaheadMs, true, 0.0f));

                // 真峰值约 −14 dBTP：+6 dB 之后仍低于 −1.35 dBTP 的内部阈值。
                auto input = makeNoise (numChannels, (int) (0.6 * sampleRate), 0.05f, 42);
                for (auto frequency : { 60.0, 1000.0, 9000.0 })
                {
                    const auto tone = makeSine (numChannels, sampleRate, frequency, 0.03f, 0.6);
                    for (int ch = 0; ch < numChannels; ++ch)
                        for (size_t i = 0; i < input[(size_t) ch].size(); ++i)
                            input[(size_t) ch][i] += tone[(size_t) ch][i];
                }

                expect (truePeakOf (input) * juce::Decibels::decibelsToGain (6.0f) < juce::Decibels::decibelsToGain (-1.35f),
                        "test signal + 6 dB must stay below the guarded threshold");

                // 前 0.2 s Gain 0 dB，之后 +6 dB（20 ms 平滑正好落在中间），再回 +2 dB。
                const auto third = (int) input.front().size() / 3;
                juce::AudioBuffer<float> block (numChannels, maxBlock), removed (numChannels, maxBlock);
                float worst = 0.0f;

                for (int start = 0; start < (int) input.front().size(); start += maxBlock)
                {
                    if (start >= third)
                        limiter.setParams (makeParams (start >= 2 * third ? 2.0f : 6.0f, -1.0f, lookaheadMs, true, 0.0f));

                    const auto length = juce::jmin (maxBlock, (int) input.front().size() - start);
                    block.setSize (numChannels, length, false, false, true);
                    removed.setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                    limiter.process (block, &removed);

                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int i = 0; i < length; ++i)
                            worst = juce::jmax (worst, std::abs (removed.getSample (ch, i)));
                }

                expectEquals (worst, 0.0f, "difference must be exactly zero @" + juce::String (sampleRate) + " la=" + juce::String (lookaheadMs));
                expectEquals (limiter.getAndResetGainReductionDb(), 0.0f, "the fixture must not trigger any reduction");
            }
        }

        // 试听差值 ②：传不传差值缓冲，输出、GR 表与每块统计都逐位相同（重限幅 + Clip，含一次 Lookahead 变动与中途重启）。
        void testDifferenceLeavesOutputUntouched()
        {
            beginTest ("passing a difference buffer never changes output, meter or block stats");

            constexpr double sampleRate = 48000.0;
            const auto input = makeNoise (numChannels, 48000, 1.0f, 17);

            for (auto clip : { 0.0f, 40.0f })
            {
                auto params = makeParams (12.0f, -1.0f, 3.0f, true, clip);
                LimiterModule plain, auditioned;
                prepareLimiter (plain, sampleRate, params);
                prepareLimiter (auditioned, sampleRate, params);

                juce::AudioBuffer<float> a (numChannels, maxBlock), b (numChannels, maxBlock), removed (numChannels, maxBlock);
                bool sameOutput = true, sameStats = true, sameMeter = true;

                for (int start = 0, index = 0; start < (int) input.front().size(); start += 300, ++index)
                {
                    if (index == 40)
                    {
                        params.lookaheadMs = 6.0f;
                        plain.setParams (params);
                        auditioned.setParams (params);
                    }

                    if (index == 90)
                    {
                        plain.restartWithFadeIn (100);
                        auditioned.restartWithFadeIn (100);
                    }

                    const auto length = juce::jmin (300, (int) input.front().size() - start);
                    for (auto* buffer : { &a, &b, &removed })
                        buffer->setSize (numChannels, length, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        a.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);
                        b.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);
                    }

                    plain.process (a);
                    auditioned.process (b, &removed);

                    for (int ch = 0; ch < numChannels; ++ch)
                        sameOutput = sameOutput && std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) length) == 0;

                    const auto& sa = plain.getBlockStats();
                    const auto& sb = auditioned.getBlockStats();
                    sameStats = sameStats && sa.grMeanDb == sb.grMeanDb && sa.grMaxDb == sb.grMaxDb
                                          && sa.clipActiveFraction == sb.clipActiveFraction && sa.clipDepthMaxDb == sb.clipDepthMaxDb;
                    sameMeter = sameMeter && plain.getAndResetGainReductionDb() == auditioned.getAndResetGainReductionDb();
                }

                expect (sameOutput, "output must be bit-identical, clip " + juce::String (clip));
                expect (sameStats, "block stats must be identical, clip " + juce::String (clip));
                expect (sameMeter, "GR meter must be identical, clip " + juce::String (clip));
            }
        }

        // 试听差值 ③：Gain 稳定、Clip 0 时「输出 + Gain × 差值」= 加了 Gain 的延迟输入（浮点舍入内）；
        // 纯增益衰减的差值 = 输入 ×（1 − 衰减），逐样本不超过输入、也不超过 Ceiling；Clip 开着时照样有限、不超过 Ceiling；分块无关。
        void testDifferenceReconstructsGainedInput()
        {
            beginTest ("output + gain * difference == gained delayed input; difference never louder than input or ceiling");

            constexpr double sampleRate = 48000.0;
            const auto gainDb = 9.0f;
            const auto gain = juce::Decibels::decibelsToGain (gainDb);
            const auto ceiling = juce::Decibels::decibelsToGain (-1.0f);

            auto input = makeListenLikeMaterial (sampleRate);
            const auto length = (int) input.front().size();

            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (gainDb, -1.0f, 2.0f, true, 0.0f));
                const auto latency = limiter.getLatencySamples();
                const auto result = runLimiterWithDifference (limiter, input, maxBlock);

                float reconstruction = 0.0f, louderThanInput = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = latency; i < length; ++i)
                    {
                        const auto x = input[(size_t) ch][(size_t) (i - latency)];
                        const auto y = result.output[(size_t) ch][(size_t) i];
                        const auto d = result.difference[(size_t) ch][(size_t) i];
                        reconstruction = juce::jmax (reconstruction, std::abs (y + gain * d - gain * x));
                        louderThanInput = juce::jmax (louderThanInput, std::abs (d) - std::abs (x));
                    }

                logMessage ("  reconstruction error " + juce::String (toDb (reconstruction), 1) + " dBFS, difference peak "
                            + juce::String (toDb (peakOf (result.difference)), 1) + " dBFS");
                expect (reconstruction < 2.0e-6f, "output + gain * difference must rebuild the gained input: " + juce::String (reconstruction));
                expect (louderThanInput <= 1.0e-6f, "pure gain reduction never makes the difference louder than the input");
                expect (peakOf (result.difference) > 0.05f, "the fixture must actually be limited");
                expect (peakOf (result.difference) <= ceiling, "difference must stay within the ceiling");
            }

            Signal reference;
            for (auto blockSize : { 512, 1, 17, 256, 2048 })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (gainDb, -1.0f, 2.0f, true, 60.0f));
                const auto result = runLimiterWithDifference (limiter, input, blockSize);
                expect (allFinite (result.difference), "difference must stay finite with Clip");
                expect (peakOf (result.difference) <= ceiling, "difference with Clip must stay within the ceiling");

                if (reference.empty())
                    reference = result.difference;
                else
                    expectEquals (maxAbsDifference (reference, result.difference, 0, length), 0.0f, "block size " + juce::String (blockSize));
            }
        }

        // 试听差值 ④：旁通（从头就旁通）时差值逐位为 0；重启淡入的静音段里差值也是 0，之后跟着淡入回来。
        void testDifferenceFollowsBypassAndFadeIn()
        {
            beginTest ("difference is silent while bypassed and during the fade-in hold");

            constexpr double sampleRate = 48000.0;
            const auto input = makeNoise (numChannels, 24000, 1.0f, 23);

            {
                auto params = makeParams (12.0f, -1.0f, 2.0f, true, 50.0f);
                params.bypass = true;
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, params);
                const auto result = runLimiterWithDifference (limiter, input, maxBlock);
                expectEquals (peakOf (result.difference), 0.0f, "a bypassed limiter removes nothing");
            }

            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (12.0f, -1.0f, 2.0f, true, 0.0f));
                runLimiterWithDifference (limiter, input, maxBlock);

                constexpr int extraHold = 150;
                limiter.restartWithFadeIn (extraHold);
                const auto hold = limiter.getLatencySamples() + extraHold;
                const auto result = runLimiterWithDifference (limiter, input, maxBlock);

                float duringHold = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < hold; ++i)
                        duringHold = juce::jmax (duringHold, std::abs (result.difference[(size_t) ch][(size_t) i]));

                expectEquals (duringHold, 0.0f, "difference must be silent while the output is held");
                expect (peakOf (result.difference) > 0.05f, "difference comes back after the fade-in");
                expect (allFinite (result.difference));
            }
        }

        void testLowFrequencyReleaseReducesIntermodulation()
        {
            beginTest ("LF-aware Auto release reduces bass/treble IMD against the original double envelope at identical settings");
            constexpr double sampleRate = 48000.0;
            const auto params = makeParams (9.0f, -1.0f, 2.0f, true, 0.0f);
            for (const auto lowHz : { 40.0, 55.0, 80.0 })
            {
                const auto input = makeLowHighSignal (sampleRate, lowHz, 3.0);
                LimiterModule current, baseline;
                baseline.setLowFrequencyReleaseEnabledForTesting (false);
                prepareLimiter (current, sampleRate, params);
                prepareLimiter (baseline, sampleRate, params);
                auto output = runLimiter (current, input, maxBlock);
                auto original = runLimiter (baseline, input, maxBlock);
                const auto measureImd = [&] (const Signal& signal)
                {
                    double energy = 0.0;
                    for (const auto offset : { -2.0, -1.0, 1.0, 2.0 })
                        energy += juce::square (lastSecondAmplitude (signal, sampleRate, 6000.0 + offset * lowHz));
                    return std::sqrt (energy) / juce::jmax (1.0e-12, lastSecondAmplitude (signal, sampleRate, 6000.0));
                };
                const auto before = measureImd (original);
                const auto after = measureImd (output);
                const auto improvement = toDb (before / juce::jmax (1.0e-12, after));
                const auto levelChange = toDb (lastSecondRms (output, sampleRate) / lastSecondRms (original, sampleRate));
                const auto tag = juce::String (lowHz, 0) + " Hz";
                logMessage ("  " + tag + " IMD " + juce::String (100.0 * before, 4) + "% -> " + juce::String (100.0 * after, 4)
                            + "%, improvement " + juce::String (improvement, 3) + " dB, RMS change " + juce::String (levelChange, 3)
                            + " dB, fast release " + juce::String (current.getAutoFastReleaseMsForTesting(), 2) + " ms");
                expect (before > 1.0e-5, tag + ": baseline must contain measurable modulation");
                expect (improvement >= 2.0f, tag + ": expected at least 2 dB less carrier-normalized IMD");
                expect (std::abs (levelChange) <= 0.75f, tag + ": do not hide distortion by making the whole output much quieter");
                expect (current.getAutoFastReleaseMsForTesting() > 35.0f, tag + ": sustained pure-Side bass must activate the observer");
                expect (allFinite (output));

                // The three-second spectral window ends inside the delayed
                // waveform. Zero-padding that OUTPUT invents an abrupt cut
                // which the limiter never received. Send silence through the
                // INPUT, as in the existing TP matrix, and retain the real tail.
                const auto croppedCurrentTp = toDb (truePeakOf (output));
                const auto croppedBaselineTp = toDb (truePeakOf (original));
                const auto noPaddingCurrentTp = toDb (steadyTruePeakRange (output).second);
                const auto noPaddingBaselineTp = toDb (steadyTruePeakRange (original).second);
                const auto silence = makeSilence (numChannels, current.getLatencySamples() + 128);
                const auto currentTail = runLimiter (current, silence, maxBlock);
                const auto baselineTail = runLimiter (baseline, silence, maxBlock);
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    output[(size_t) ch].insert (output[(size_t) ch].end(), currentTail[(size_t) ch].begin(), currentTail[(size_t) ch].end());
                    original[(size_t) ch].insert (original[(size_t) ch].end(), baselineTail[(size_t) ch].begin(), baselineTail[(size_t) ch].end());
                }
                const auto currentTp = toDb (truePeakOf (output));
                const auto baselineTp = toDb (truePeakOf (original));
                logMessage ("  " + tag + " TP legacy/current: cropped " + juce::String (croppedBaselineTp, 4)
                            + "/" + juce::String (croppedCurrentTp, 4) + ", unpadded interior " + juce::String (noPaddingBaselineTp, 4)
                            + "/" + juce::String (noPaddingCurrentTp, 4) + ", complete flushed output " + juce::String (baselineTp, 4)
                            + "/" + juce::String (currentTp, 4) + " dBTP");
                expect (baselineTp <= params.ceilingDb + 0.01f, tag + ": original double envelope must meet the unchanged TP criterion");
                expect (currentTp <= params.ceilingDb + 0.01f, tag + ": LF-aware release must meet the unchanged TP criterion");
                expectEquals (current.getLatencySamples(), baseline.getLatencySamples(), "observer adds no latency");
            }
        }

        void testLowFrequencyReleasePreservesTransientsAndManual()
        {
            beginTest ("isolated decaying kicks, treble-only material and manual Release retain their original response");
            constexpr double sampleRate = 48000.0;
            auto kicks = makeSine (numChannels, sampleRate, 1000.0, 0.03f, 3.0);
            for (int i = 0; i < (int) kicks[0].size(); ++i)
            {
                const auto phase = std::fmod (i / sampleRate, 0.5);
                const auto kick = phase < 0.15 ? (float) (0.9 * std::sin (juce::MathConstants<double>::twoPi * 60.0 * phase)
                                                                   * std::exp (-phase / 0.04)) : 0.0f;
                for (auto& channel : kicks) channel[(size_t) i] += kick;
            }
            for (int mode = 0; mode < 3; ++mode)
            {
                auto params = makeParams (9.0f, -1.0f, 2.0f, mode != 2, 0.0f);
                params.releaseMs = 83.0f;
                const auto input = mode == 0 ? kicks : mode == 1 ? makeSine (numChannels, sampleRate, 1000.0, 0.7f, 2.0)
                                                                : makeLowHighSignal (sampleRate, 55.0, 2.0);
                LimiterModule current, baseline;
                baseline.setLowFrequencyReleaseEnabledForTesting (false);
                prepareLimiter (current, sampleRate, params);
                prepareLimiter (baseline, sampleRate, params);
                const auto a = runLimiter (current, input, 333);
                const auto b = runLimiter (baseline, input, 333);
                const auto difference = maxAbsDifference (a, b, 0, (int) input[0].size());
                expectEquals (difference, 0.0f, "original transient/manual response, mode=" + juce::String (mode));
                expectEquals (current.getAutoFastReleaseMsForTesting(), 30.0f, "non-sustained/non-LF/manual case must not extend release");
            }
        }

        void testLowFrequencyReleaseRejectsUnstablePeriods()
        {
            beginTest ("steady low-frequency noise and rapid continuous pitch changes cannot masquerade as stable bass periods");
            constexpr double sampleRate = 48000.0;
            constexpr int length = 4 * (int) sampleRate;
            auto noise = makeSilence (numChannels, length);
            auto changingPitch = noise;
            juce::Random random (0x6c6f77);
            const auto lowAlpha = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 100.0 / sampleRate);
            const auto dcAlpha = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * 20.0 / sampleRate);
            double low1 = 0.0, low2 = 0.0, dc = 0.0, noiseEnergy = 0.0;
            // One second of preroll makes the noise stationary at the input,
            // rather than relying on an onset or silence gate to reject it.
            for (int i = -(int) sampleRate; i < length; ++i)
            {
                low1 += lowAlpha * (2.0 * random.nextDouble() - 1.0 - low1);
                low2 += lowAlpha * (low1 - low2);
                dc += dcAlpha * (low2 - dc);
                if (i >= 0)
                {
                    const auto x = (float) (low2 - dc);
                    noise[0][(size_t) i] = x;
                    noise[1][(size_t) i] = -x;
                    noiseEnergy += (double) x * x;
                }
            }
            scale (noise, (float) (0.38 / std::sqrt (noiseEnergy / length)));
            double phase = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const auto t = i / sampleRate;
                const auto sweep = std::fmod (t, 0.16) / 0.08;
                const auto frequency = 40.0 + 70.0 * (sweep <= 1.0 ? sweep : 2.0 - sweep);
                phase += juce::MathConstants<double>::twoPi * frequency / sampleRate;
                const auto x = (float) (0.62 * std::sin (phase)
                                     + 0.12 * std::sin (juce::MathConstants<double>::twoPi * 6000.0 * t));
                changingPitch[0][(size_t) i] = x;
                changingPitch[1][(size_t) i] = -x;
            }
            const auto params = makeParams (9.0f, -1.0f, 2.0f, true, 0.0f);
            for (int mode = 0; mode < 2; ++mode)
            {
                const auto& input = mode == 0 ? noise : changingPitch;
                const auto tag = mode == 0 ? juce::String ("stationary LF noise") : juce::String ("40-110 Hz rapid sweep");
                LimiterModule current, baseline;
                baseline.setLowFrequencyReleaseEnabledForTesting (false);
                prepareLimiter (current, sampleRate, params);
                prepareLimiter (baseline, sampleRate, params);
                juce::AudioBuffer<float> a (numChannels, 333), b (numChannels, 333);
                float maximumRelease = 30.0f, difference = 0.0f;
                for (int start = 0; start < length; start += 333)
                {
                    const auto count = juce::jmin (333, length - start);
                    a.setSize (numChannels, count, false, false, true);
                    b.setSize (numChannels, count, false, false, true);
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        a.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);
                        b.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);
                    }
                    current.process (a);
                    baseline.process (b);
                    maximumRelease = juce::jmax (maximumRelease, current.getAutoFastReleaseMsForTesting());
                    for (int ch = 0; ch < numChannels; ++ch)
                        for (int i = 0; i < count; ++i)
                            difference = juce::jmax (difference, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
                }
                logMessage ("  " + tag + ": maximum fast release " + juce::String (maximumRelease, 4)
                            + " ms, output difference " + juce::String (difference, 8));
                expectEquals (maximumRelease, 30.0f, tag + ": stable energy alone must not lengthen release");
                expectEquals (difference, 0.0f, tag + ": retain the original double-envelope response");
            }
        }

        void testLowFrequencyReleaseBlockResetAndAllocation()
        {
            beginTest ("LF-aware release stays bit-exact across block sizes, resets cleanly and allocates nothing while active");
            constexpr double sampleRate = 48000.0;
            const auto params = makeParams (9.0f, -1.0f, 2.0f, true, 0.0f);
            const auto input = makeLowHighSignal (sampleRate, 55.0, 2.0);
            Signal reference;
            for (const auto blockSize : { 512, 17, 333, 2048 })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, params);
                const auto output = runLimiter (limiter, input, blockSize);
                expect (limiter.getAutoFastReleaseMsForTesting() > 35.0f, "case must exercise sustained LF control");
                if (reference.empty()) reference = output;
                else expectEquals (maxAbsDifference (reference, output, 0, (int) input[0].size()), 0.0f, "block=" + juce::String (blockSize));
            }
            LimiterModule used, fresh;
            prepareLimiter (used, sampleRate, params);
            prepareLimiter (fresh, sampleRate, params);
            runLimiter (used, input, maxBlock);
            used.reset();
            expectEquals (used.getAutoFastReleaseMsForTesting(), 30.0f, "reset clears period and sustain history");
            const auto a = runLimiter (used, input, maxBlock);
            const auto b = runLimiter (fresh, input, maxBlock);
            expectEquals (maxAbsDifference (a, b, 0, (int) input[0].size()), 0.0f, "reset == fresh prepare");
            {
                Fingerprint fingerprint;
                fingerprint.add (reference);
                fingerprint.add (a);
                logMessage ("  output fingerprint (block sizes + reset) " + fingerprint.toString());
            }
            juce::AudioBuffer<float> block (numChannels, maxBlock);
            std::uint64_t allocations = 0;
            for (int start = 0; start + maxBlock <= (int) input[0].size(); start += maxBlock)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    block.copyFrom (ch, 0, input[(size_t) ch].data() + start, maxBlock);
                const auto before = allocationCountOnThisThread();
                used.setParams (params);
                used.process (block);
                allocations += allocationCountOnThisThread() - before;
            }
            expectEquals ((juce::int64) allocations, (juce::int64) 0, "active low-frequency observer must not allocate");
        }

        // T-032 R1-02：SlidingMinimum 由单调队列（每样本 2 ～ 3 次整数取模 + 两个数据相关的 while）换成 van Herk / Gil-Werman
        // 块算法，声称逐位相同。两把尺子逐样本对照：暴力 std::min_element（上次清空以来推入的最近 window 个样本）与旧实现逐行照抄的参照。
        // 窗长在 1 … maxLookahead + 63（48 kHz 下 543）间随机切换。奇数轮只调 setWindow、不另调 clear：改窗必须自己清空
        //（块内位置可能已超过新窗长，不清就会越界写）；偶数轮照 resetLimiterState 原来的写法再 clear 一次，结果应一样。
        // 输入两种：增益状（70 % 恰为 1.0，其余量化到 1/64 网格造大量相等值，偶尔 0）与纯随机 [0, 1)。
        // 另测块中途缩窗（位置 300 时改成窗长 7）与音频线程用法不分配。
        void testSlidingMinimumMatchesReferences()
        {
            beginTest ("sliding minimum (van Herk) is bit-exact with brute force and with the legacy monotonic queue");

            // v0.1.8.5 及之前的实现，逐行照抄（LimiterModule.cpp @ e6b445b）作参照。
            struct LegacyMonotonicQueue
            {
                void prepare (int maxWindow)
                {
                    values.assign ((size_t) (maxWindow + 1), 1.0f);
                    indices.assign ((size_t) (maxWindow + 1), 0);
                    clear();
                }

                void setWindow (int newWindow) noexcept   { window = newWindow; }
                void clear() noexcept                     { head = 0; count = 0; sampleIndex = 0; }

                float push (float value) noexcept
                {
                    const auto capacity = (int) values.size();

                    while (count > 0)
                    {
                        const auto back = (head + count - 1) % capacity;

                        if (values[(size_t) back] < value)
                            break;

                        --count;
                    }

                    const auto slot = (head + count) % capacity;
                    values[(size_t) slot] = value;
                    indices[(size_t) slot] = sampleIndex;
                    ++count;

                    while (indices[(size_t) head] <= sampleIndex - window)
                    {
                        head = (head + 1) % capacity;
                        --count;
                    }

                    ++sampleIndex;
                    return values[(size_t) head];
                }

                std::vector<float> values;
                std::vector<std::int64_t> indices;
                int head = 0, count = 0, window = 1;
                std::int64_t sampleIndex = 0;
            };

            constexpr int maxWindow = 480 + LimiterModule::holdSamples;   // 10 ms @ 48 kHz + 保持段 = 543，与 prepare 里的上限一致
            constexpr int samplesPerTrial = 4000;
            constexpr int trials = 60;

            LimiterModule::SlidingMinimum vanHerk;
            LegacyMonotonicQueue legacy;
            vanHerk.prepare (maxWindow);
            legacy.prepare (maxWindow);

            std::vector<float> history;
            history.reserve ((size_t) samplesPerTrial);
            juce::Random random (0x5157);

            // 暴力参照：history 里最近 window 个（不足 window 个时是全部）的最小值。
            const auto bruteForceMin = [&history] (int window)
            {
                return *std::min_element (history.end() - juce::jmin (window, (int) history.size()), history.end());
            };

            for (int kind = 0; kind < 2; ++kind)
            {
                const auto kindName = kind == 0 ? juce::String ("gain-like input") : juce::String ("random input");
                int bruteMismatches = 0, legacyMismatches = 0, checked = 0;

                for (int trial = 0; trial < trials; ++trial)
                {
                    // 前三轮固定在边界：1、2、最大窗；其余随机。
                    const auto window = trial == 0 ? 1 : trial == 1 ? 2 : trial == 2 ? maxWindow : 1 + random.nextInt (maxWindow);
                    vanHerk.setWindow (window);   // 改窗同时清空

                    if (trial % 2 == 0)
                        vanHerk.clear();

                    legacy.setWindow (window);
                    legacy.clear();
                    history.clear();

                    for (int n = 0; n < samplesPerTrial; ++n)
                    {
                        float value = 0.0f;

                        if (kind == 0)
                        {
                            const auto roll = random.nextInt (100);
                            value = roll < 70 ? 1.0f
                                  : roll < 72 ? 0.0f
                                              : (float) (1 + random.nextInt (64)) / 64.0f;
                        }
                        else
                        {
                            value = random.nextFloat();
                        }

                        history.push_back (value);
                        const auto expected = bruteForceMin (window);
                        const auto got = vanHerk.push (value);
                        const auto legacyGot = legacy.push (value);

                        bruteMismatches  += sameBits (got, expected)  ? 0 : 1;
                        legacyMismatches += sameBits (got, legacyGot) ? 0 : 1;
                        ++checked;
                    }
                }

                logMessage ("  " + kindName + ": " + juce::String (checked) + " samples over " + juce::String (trials) + " window lengths");
                expectEquals (bruteMismatches, 0, kindName + ": van Herk differs from brute-force min");
                expectEquals (legacyMismatches, 0, kindName + ": van Herk differs from the legacy monotonic queue");
            }

            // 块中途缩窗：窗长 500 推 300 个（块内位置 300），直接改成 7——位置已超过新窗长；改窗不清空就会一路越界写、结果也不对。
            {
                vanHerk.setWindow (500);

                for (int n = 0; n < 300; ++n)
                    juce::ignoreUnused (vanHerk.push (random.nextFloat()));

                vanHerk.setWindow (7);
                history.clear();
                int mismatches = 0;

                for (int n = 0; n < 2000; ++n)
                {
                    const auto value = random.nextFloat();
                    history.push_back (value);
                    mismatches += sameBits (vanHerk.push (value), bruteForceMin (7)) ? 0 : 1;
                }

                expectEquals (mismatches, 0, "shrinking the window mid-block must restart cleanly");
            }

            // 音频线程用法：push / setWindow / clear 都不分配（计数见 TestMain.cpp）。
            vanHerk.setWindow (maxWindow);
            const auto before = allocationCountOnThisThread();

            for (int n = 0; n < 3 * maxWindow; ++n)
                juce::ignoreUnused (vanHerk.push (random.nextFloat()));

            vanHerk.setWindow (7);
            vanHerk.clear();
            const auto allocations = allocationCountOnThisThread() - before;
            expectEquals ((int) allocations, 0, "sliding minimum must not allocate on the audio thread");
        }

        // T-032 R1-03：输出硬钳位的 Ceiling 延迟线在重置时填成当前 Ceiling（与 Gain 延迟线同构），不再清成 0。
        // 从输出上看不出来：重置同时清空了音频延迟线，头 coreDelay 个样本 delayed = 0，钳到 ±0 与 ±Ceiling 都是 +0
        //（输出逐位不变由其余用例兜底），但以后若「换前瞻不清音频延迟线」，清零就会把这 coreDelay 个样本静默钳成 0。
        // 所以直接查延迟线接下来要交出的值（测试钩子在副本上推）：prepare、restartWithFadeIn、reset 之后全是当前 Ceiling
        //（reset 时平滑器先被重置、再落到新目标，要两段式填）；换前瞻的重置发生在下一块开头、Ceiling 还没开始滑，
        // 那一刻的值填满整条线，之后才是逐样本滑动的新值。清零的旧写法这里拿到的全是 0。
        void testHardClipCeilingDelayFollowsResets()
        {
            beginTest ("hard-clip ceiling delay is refilled with the current ceiling on every reset, never with zeros");

            constexpr double sampleRate = 48000.0;
            const auto gainOf = [] (float db) { return juce::Decibels::decibelsToGain (db); };   // 与 reset() 同一个式子

            // pending 的前 count 个值都与 expected 逐位相同。
            const auto checkPending = [this] (const std::vector<float>& pending, int count, float expected, const juce::String& what)
            {
                int mismatches = 0;

                for (int i = 0; i < count; ++i)
                    mismatches += sameBits (pending[(size_t) i], expected) ? 0 : 1;

                expect (count > 0 && count <= (int) pending.size() && expected > 0.0f, what + ": fixture must be meaningful");
                expectEquals (mismatches, 0, what);
            };

            LimiterModule limiter;
            auto params = makeParams (6.0f, -1.0f, 2.0f, true, 0.0f);
            prepareLimiter (limiter, sampleRate, params);

            const auto coreDelay = (int) limiter.getPendingHardClipCeilingsForTesting().size();
            expect (coreDelay > 0, "the core delay must not be empty");
            checkPending (limiter.getPendingHardClipCeilingsForTesting(), coreDelay, gainOf (-1.0f), "after prepare");

            // 跑一段（Ceiling 不变，平滑器停在目标上），再中途重启。
            juce::ignoreUnused (runLimiter (limiter, makeNoise (numChannels, 4800, 1.0f, 73), maxBlock));
            limiter.restartWithFadeIn (100);
            checkPending (limiter.getPendingHardClipCeilingsForTesting(), coreDelay, gainOf (-1.0f), "after restartWithFadeIn");

            // reset() 先按平滑器当前值（旧的 −1）重置状态，再把平滑器落到新目标 −2.5：延迟线必须跟着第二次填成 −2.5。
            params.ceilingDb = -2.5f;
            limiter.setParams (params);
            limiter.reset();
            checkPending (limiter.getPendingHardClipCeilingsForTesting(), coreDelay, gainOf (-2.5f), "after reset with a new ceiling");

            // 换前瞻、同时把 Ceiling 改回 −1：下一块开头重置时平滑器还停在 −2.5（从这一块的第一个样本才开始滑），
            // 整条线填 −2.5；送 1 个样本后，前 coreDelay′ − 1 个仍是 −2.5，最新的一个是滑向 −1 的第一步。
            params.ceilingDb = -1.0f;
            params.lookaheadMs = 5.0f;
            limiter.setParams (params);
            juce::AudioBuffer<float> one (numChannels, 1);
            one.clear();
            limiter.process (one);

            const auto pending = limiter.getPendingHardClipCeilingsForTesting();
            const auto newCoreDelay = (int) pending.size();
            expect (newCoreDelay > coreDelay, "the lookahead change must lengthen the core delay");
            checkPending (pending, newCoreDelay - 1, gainOf (-2.5f), "after a lookahead change while the ceiling starts to ramp");
            expect (pending.back() > gainOf (-2.5f) && pending.back() < gainOf (-1.0f), "the newest value is the first step of the ceiling ramp");
        }

        // T-032 R8-02（Limit 部分）：setParams 不信任外部值。APVTS 与链只钳有限值，"nan" 会从工程文件原样读回：
        // 释放时间 NaN → 释放系数 NaN → 包络永远 NaN → MovingAverage::push 里 jmin (1.0, NaN) 返回 1.0 → 限幅器静默失效、
        // 只剩硬钳位在削（不报非有限）；Gain / Ceiling NaN → decibelsToGain 返回 0 → 永久静音；前瞻 NaN → 被悄悄当成 1 个样本。
        // 每个 float 字段分别喂 NaN / +Inf / −Inf，两种时机：prepare 时就是坏值（工程读回），与处理中途从正常值变成坏值
        //（自动化 / 预设写坏）。0 dBFS 噪声推 +24 dB：输出有限、坏值生效之后的尾段不静音、真峰值 ≤ Ceiling + 0.01 dB，
        // 且与「该字段在同一时刻取默认值」的限幅器逐位相同、上报延迟相同。基准参数的每个字段都不取默认值，换成默认值才看得出变化。
        // 越界的有限值钳到边界（与直接给边界值的限幅器逐位相同；Ceiling 下限 −4.5：链扣掉编码余量后会交 −4.5，钳到 −3 会让
        // 编码余量失效）；范围内的值原样保留（−4.5 真的限在 −4.5）。
        void testParamsGuardNonFiniteAndOutOfRange()
        {
            beginTest ("setParams: non-finite fields fall back to defaults, out-of-range finite fields are clamped, in-range fields untouched");

            constexpr double sampleRate = 48000.0;
            constexpr int blockSize = 333;
            constexpr int switchAt = 12 * blockSize;                         // 中途换参数的块边界（约 83 ms 处）
            const auto input = makeNoise (numChannels, 24000, 1.0f, 61);   // 0 dBFS 噪声
            const auto length = (int) input.front().size();
            constexpr int tailLength = 4000;                                 // 尾段：换参数之后早已走完延迟与淡入

            using Params = LimiterModule::Params;
            using Member = float Params::*;
            const Params defaults;

            struct Run
            {
                Signal output;
                int latency = 0;
            };

            // prepare 时拿 first，从第 switchAt 个样本起拿 second（setParams 在块与块之间调，与处理器一样）。
            const auto runSchedule = [&] (const Params& first, const Params& second)
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, first);
                Run run { input, 0 };
                juce::AudioBuffer<float> block (numChannels, blockSize);

                for (int start = 0; start < length; start += blockSize)
                {
                    if (start == switchAt)
                        limiter.setParams (second);

                    const auto count = juce::jmin (blockSize, length - start);
                    block.setSize (numChannels, count, false, false, true);

                    for (int ch = 0; ch < numChannels; ++ch)
                        block.copyFrom (ch, 0, input[(size_t) ch].data() + start, count);

                    limiter.process (block);

                    for (int ch = 0; ch < numChannels; ++ch)
                        std::copy (block.getReadPointer (ch), block.getReadPointer (ch) + count, run.output[(size_t) ch].begin() + start);
                }

                run.latency = limiter.getLatencySamples();
                return run;
            };

            const auto tailPeak = [&] (const Signal& signal)
            {
                float peak = 0.0f;

                for (const auto& channel : signal)
                    for (int i = length - tailLength; i < length; ++i)
                        peak = juce::jmax (peak, std::abs (channel[(size_t) i]));

                return peak;
            };

            struct Field { const char* name; Member member; bool manualRelease; };
            const Field fields[] = { { "gainDb",      &Params::gainDb,      false },
                                     { "ceilingDb",   &Params::ceilingDb,   false },
                                     { "releaseMs",   &Params::releaseMs,   true  },   // 手动释放才用到释放系数
                                     { "lookaheadMs", &Params::lookaheadMs, false },
                                     { "clip",        &Params::clip,        false } };

            const float nonFinite[] = { std::numeric_limits<float>::quiet_NaN(),
                                        std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity() };
            const char* const nonFiniteNames[] = { "NaN", "+Inf", "-Inf" };
            int cases = 0;

            for (const auto& field : fields)
            {
                // 基准：每个字段都不是默认值（Gain 24、Ceiling −2、Release 250、Lookahead 5、Clip 30）。
                auto base = makeParams (24.0f, -2.0f, 5.0f, ! field.manualRelease, 30.0f);
                base.releaseMs = 250.0f;

                for (int v = 0; v < 3; ++v)
                {
                    auto poisoned = base;
                    auto defaulted = base;
                    poisoned.*(field.member) = nonFinite[v];
                    defaulted.*(field.member) = defaults.*(field.member);
                    const auto ceilingDb = juce::jmax (base.ceilingDb, defaulted.ceilingDb);

                    for (int midway = 0; midway < 2; ++midway)
                    {
                        // midway = 0：prepare 时就是坏值，中途换回正常值；1：先正常，中途变坏。参照在同样的时刻拿「该字段 = 默认」。
                        const auto a = midway == 0 ? runSchedule (poisoned, base) : runSchedule (base, poisoned);
                        const auto b = midway == 0 ? runSchedule (defaulted, base) : runSchedule (base, defaulted);
                        const auto caseName = juce::String (field.name) + " = " + nonFiniteNames[v]
                                            + (midway == 0 ? " at prepare" : " mid-stream");

                        expectEquals (a.latency, b.latency, caseName + ": latency must match the defaulted parameters");
                        expect (allFinite (a.output), caseName + ": output must stay finite");
                        expect (tailPeak (a.output) > 0.1f, caseName + ": output must not fall silent");
                        expect (toDb (truePeakOf (a.output)) <= ceilingDb + 0.01f, caseName + ": true peak must stay under the ceiling");
                        expectEquals (maxAbsDifference (a.output, b.output, 0, length), 0.0f,
                                      caseName + ": must be bit-identical to the defaulted parameters");
                        ++cases;
                    }
                }
            }

            logMessage ("  " + juce::String (cases) + " non-finite cases");

            // 越界的有限值钳到边界：与直接给边界值的限幅器逐位相同（前瞻由 lookaheadSamplesFor 钳，效果一样）。
            struct Clamp { const char* name; Member member; float wild; float clamped; };
            const Clamp clamps[] = { { "gainDb 30 -> 24",        &Params::gainDb,      30.0f,   24.0f },
                                     { "gainDb -6 -> 0",         &Params::gainDb,      -6.0f,   0.0f },
                                     { "ceilingDb -9 -> -4.5",   &Params::ceilingDb,   -9.0f,   -4.5f },
                                     { "ceilingDb +2 -> 0",      &Params::ceilingDb,   2.0f,    0.0f },
                                     { "releaseMs 1 -> 10",      &Params::releaseMs,   1.0f,    10.0f },
                                     { "releaseMs 5000 -> 1000", &Params::releaseMs,   5000.0f, 1000.0f },
                                     { "clip 150 -> 100",        &Params::clip,        150.0f,  100.0f },
                                     { "clip -20 -> 0",          &Params::clip,        -20.0f,  0.0f },
                                     { "lookaheadMs 20 -> 10",   &Params::lookaheadMs, 20.0f,   10.0f },
                                     { "lookaheadMs 0.2 -> 1",   &Params::lookaheadMs, 0.2f,    1.0f } };

            for (const auto& clamp : clamps)
            {
                const auto manual = clamp.member == &Params::releaseMs;
                auto wild = makeParams (12.0f, -1.0f, 2.0f, ! manual, 30.0f);
                auto clamped = wild;
                wild.*(clamp.member) = clamp.wild;
                clamped.*(clamp.member) = clamp.clamped;

                const auto a = runSchedule (wild, wild);
                const auto b = runSchedule (clamped, clamped);
                expectEquals (a.latency, b.latency, juce::String (clamp.name) + ": latency must match the clamped value");
                expect (allFinite (a.output), juce::String (clamp.name) + ": output must stay finite");
                expectEquals (maxAbsDifference (a.output, b.output, 0, length), 0.0f,
                              juce::String (clamp.name) + ": must be bit-identical to the clamped value");
            }

            // 范围内的值原样保留：Ceiling −4.5（编码余量 1.5 dB 时链交下来的值）真的限在 −4.5 附近，而不是被改回 −3 或默认 −1。
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (12.0f, -4.5f, 2.0f, true, 0.0f));
                auto padded = input;
                appendSilence (padded, limiter.getLatencySamples() + 64);
                const auto output = runLimiter (limiter, padded, blockSize);
                const auto peakDb = toDb (truePeakOf (output));
                logMessage ("  ceiling -4.5 dBTP delivers " + juce::String (peakDb, 3) + " dBTP");
                expect (peakDb <= -4.5f + 0.01f, "ceiling -4.5 must be honoured");
                expect (peakDb > -4.5f - 1.0f, "ceiling -4.5 must not be clamped to -3 or defaulted to -1 (peak far below)");
            }
        }

        // 验收 1：各种信号推 +12 / +24 dB → 输出真峰值 ≤ Ceiling + 0.01 dB。
        void testTruePeakNeverExceedsCeiling()
        {
            beginTest ("true peak <= ceiling + 0.01 dB");

            float worstOwnDb = -100.0f, worstReferenceDb = -100.0f;
            juce::String worstOwnCase, worstReferenceCase;
            int runs = 0, referenceRuns = 0, referenceOver01 = 0, referenceOver03 = 0;
            Fingerprint outputs;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                const auto signals = makePeakTestSignals (sampleRate);

                for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
                 for (auto autoRelease : { true, false })
                  for (auto clip : { 0.0f, 50.0f, 100.0f })
                   for (auto gainDb : { 12.0f, 24.0f })
                    for (const auto& named : signals)
                    {
                        constexpr float ceilingDb = -1.0f;
                        LimiterModule limiter;
                        prepareLimiter (limiter, sampleRate, makeParams (gainDb, ceilingDb, lookaheadMs, autoRelease, clip));

                        auto input = named.signal;
                        appendSilence (input, limiter.getLatencySamples() + 64);
                        const auto output = runLimiter (limiter, input, maxBlock);
                        outputs.add (output);

                        const auto caseName = juce::String (named.name) + " @" + juce::String (sampleRate)
                                            + " la=" + juce::String (lookaheadMs) + " auto=" + juce::String ((int) autoRelease)
                                            + " clip=" + juce::String (clip) + " gain=" + juce::String (gainDb);

                        expect (allFinite (output), "non-finite output: " + caseName);

                        const auto ownDb = toDb (truePeakOf (output)) - ceilingDb;
                        expect (ownDb <= 0.01f, "own meter overshoot " + juce::String (ownDb, 4) + " dB: " + caseName);

                        if (ownDb > worstOwnDb)
                        {
                            worstOwnDb = ownDb;
                            worstOwnCase = caseName;
                        }

                        // 独立参考测量（只在 48 kHz、Lookahead 2 ms 做，太慢）：只记录，不作为硬门槛。
                        if (sampleRate == 48000.0 && lookaheadMs == 2.0f)
                        {
                            const auto ceilingLinear = juce::Decibels::decibelsToGain ((double) ceilingDb);
                            const auto referenceDb = toDb (referenceTruePeakOf (output, 0.25 * ceilingLinear)) - ceilingDb;

                            ++referenceRuns;
                            referenceOver01 += referenceDb > 0.1f ? 1 : 0;
                            referenceOver03 += referenceDb > 0.3f ? 1 : 0;

                            if (referenceDb > worstReferenceDb)
                            {
                                worstReferenceDb = referenceDb;
                                worstReferenceCase = caseName;
                            }
                        }

                        ++runs;
                    }
            }

            logMessage ("  runs: " + juce::String (runs) + ", output fingerprint " + outputs.toString());
            logMessage ("  worst overshoot (own meter):       " + juce::String (worstOwnDb, 4) + " dB  [" + worstOwnCase + "]");
            // 验收口径是插件自己的表（ADR-004）；16× 参考只作记录，量「自己的表」相对理想重建差多少。
            logMessage ("  16x reference (for information only, not a pass criterion):");
            logMessage ("    worst " + juce::String (worstReferenceDb, 4) + " dB  [" + worstReferenceCase + "]");
            logMessage ("    cases > +0.1 dB: " + juce::String (referenceOver01) + " / " + juce::String (referenceRuns)
                        + ",  > +0.3 dB: " + juce::String (referenceOver03) + " / " + juce::String (referenceRuns));
        }

        // 独立重建护栏：插件检测器是 8× / 64 抽头；referenceTruePeakOf 是另写的 double 精度
        // 16× / 128 抽头 Kaiser sinc。原始实现的宽带噪声最坏超限 +0.3039 dB，需求增益加入 0.35 dB
        // 经验余量后，此网格按 +0.01 dB 数值容差验收；不代表任意波形的数学保证。自身检测器门槛也保留。
        // 用短片段限制离线参考的成本；LF + HF 多音保持 120 ms，覆盖多次 55 Hz 低频周期。
        void testIndependentTruePeakAcceptance()
        {
            beginTest ("independent 16x reconstruction <= ceiling + 0.01 dB across rates, gain and clip");

            constexpr float ceilingDb = -1.0f;
            constexpr float reconstructionToleranceDb = 0.01f;
            const auto ceiling = juce::Decibels::decibelsToGain ((double) ceilingDb);
            float worstReferenceDb = -100.0f;
            juce::String worstCase;
            int runs = 0;
            Fingerprint outputs;

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                const auto shortLength = (int) (0.04 * sampleRate);
                std::vector<NamedSignal> signals;
                signals.push_back ({ "impulse", makeImpulse (numChannels, shortLength, shortLength / 2, 0.9f) });
                signals.push_back ({ "fs/4 inter-sample tone", makeSine (numChannels, sampleRate, sampleRate / 4.0,
                                                                          0.9f, 0.04, juce::MathConstants<double>::pi / 4.0) });
                signals.push_back ({ "deterministic noise", makeNoise (numChannels, shortLength, 0.8f, 0x1eaf) });

                auto mixed = makeSilence (numChannels, (int) (0.12 * sampleRate));
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < (int) mixed[(size_t) ch].size(); ++i)
                    {
                        const auto t = (double) i / sampleRate;
                        const auto phase = ch * 0.37;
                        mixed[(size_t) ch][(size_t) i] = (float)
                            (0.62 * std::sin (juce::MathConstants<double>::twoPi * 55.0 * t + phase)
                           + 0.18 * std::sin (juce::MathConstants<double>::twoPi * 6000.0 * t + phase));
                    }
                signals.push_back ({ "55 Hz + sustained 6 kHz", std::move (mixed) });

                for (auto gainDb : { 6.0f, 18.0f })
                 for (auto clip : { 0.0f, 50.0f })
                  for (const auto& named : signals)
                  {
                      LimiterModule limiter;
                      prepareLimiter (limiter, sampleRate, makeParams (gainDb, ceilingDb, 2.0f, true, clip));
                      auto input = named.signal;
                      appendSilence (input, limiter.getLatencySamples() + 128);
                      const auto output = runLimiter (limiter, input, maxBlock);
                      outputs.add (output);
                      const auto caseName = juce::String (named.name) + " @" + juce::String (sampleRate)
                                          + " gain=" + juce::String (gainDb) + " clip=" + juce::String (clip);

                      expect (allFinite (output), "non-finite output: " + caseName);
                      const auto ownDb = toDb (truePeakOf (output)) - ceilingDb;
                      expect (ownDb <= 0.01f, "own meter overshoot " + juce::String (ownDb, 4) + " dB: " + caseName);

                      // 阈值为 0：短网格的参考检查每个插值位置，不使用跳过低电平附近插值的加速。
                      const auto referenceDb = toDb (referenceTruePeakOf (output, 0.0)) - ceilingDb;
                      expect (referenceDb <= reconstructionToleranceDb,
                              "independent reconstruction overshoot " + juce::String (referenceDb, 4) + " dB: " + caseName);

                      if (referenceDb > worstReferenceDb)
                      {
                          worstReferenceDb = referenceDb;
                          worstCase = caseName;
                      }
                      ++runs;
                  }
            }

            logMessage ("  independent reference: " + juce::String (runs) + " cases; worst "
                        + juce::String (worstReferenceDb, 4) + " dB above ceiling [" + worstCase + "]; output fingerprint " + outputs.toString());

            // 负控：采样点没超过 Ceiling 的 fs/4 正弦，重建峰值高出约 0.5 dB，必须能被同一门槛拒绝。
            const auto control = makeSine (numChannels, 48000.0, 12000.0,
                                            (float) (ceiling * juce::Decibels::decibelsToGain (0.5)),
                                            0.01, juce::MathConstants<double>::pi / 4.0);
            expect (toDb (referenceTruePeakOf (control, 0.0)) - ceilingDb > reconstructionToleranceDb,
                    "independent reference must reject an inter-sample overshoot beyond the tolerance");
        }

        // 验收 2：Gain = 0、Clip = 0、输入真峰值 < Ceiling − 0.35 dB 的内部阈值 → 延迟对齐后差值 < −120 dBFS。
        void testNullWhenNothingToDo()
        {
            beginTest ("null when gain = 0, clip = 0, input below the guarded threshold");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
              for (auto autoRelease : { true, false })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (0.0f, -1.0f, lookaheadMs, autoRelease, 0.0f));

                // 多个频率叠加 + 噪声，真峰值约 −4 dBTP，低于 −1 dBTP 的 Ceiling。
                auto input = makeNoise (numChannels, (int) (0.5 * sampleRate), 0.15f, 42);
                for (auto frequency : { 60.0, 1000.0, 9000.0, 18000.0 })
                {
                    const auto tone = makeSine (numChannels, sampleRate, frequency, 0.1f, 0.5);
                    for (int ch = 0; ch < numChannels; ++ch)
                        for (size_t i = 0; i < input[(size_t) ch].size(); ++i)
                            input[(size_t) ch][i] += tone[(size_t) ch][i];
                }

                expect (truePeakOf (input) < juce::Decibels::decibelsToGain (-1.35f), "test signal must stay below the guarded threshold");

                const auto length = (int) input.front().size();
                const auto latency = limiter.getLatencySamples();
                appendSilence (input, latency);
                const auto output = runLimiter (limiter, input, maxBlock);

                const auto diff = maxAbsDifference (input, output, latency, length);
                expect (diff < 1.0e-6f, "null residual " + juce::String (toDb (diff), 1) + " dBFS @" + juce::String (sampleRate)
                                          + " la=" + juce::String (lookaheadMs));
            }
        }

        // 旁通：逐位等于延迟后的输入（哪怕输入远超 Ceiling）。
        void testBypassIsBitExact()
        {
            beginTest ("bypass is bit-exact delayed input");

            constexpr double sampleRate = 48000.0;
            auto params = makeParams (12.0f, -1.0f, 2.0f, true, 50.0f);
            params.bypass = true;

            LimiterModule limiter;
            prepareLimiter (limiter, sampleRate, params);

            auto input = makeNoise (numChannels, 24000, 1.0f, 7);
            const auto length = (int) input.front().size();
            const auto latency = limiter.getLatencySamples();
            appendSilence (input, latency);
            const auto output = runLimiter (limiter, input, maxBlock);

            expectEquals (maxAbsDifference (input, output, latency, length), 0.0f);
        }

        // 验收 3：脉冲实测延迟 = 上报延迟（Clip 关 / 开两种路径都测）。
        void testMeasuredLatencyMatchesReported()
        {
            beginTest ("impulse latency == reported latency");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0 })
             for (auto lookaheadMs : { 1.0f, 2.0f, 10.0f })
              for (auto clip : { 0.0f, 50.0f })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (0.0f, -1.0f, lookaheadMs, true, clip));

                const auto latency = limiter.getLatencySamples();
                constexpr int position = 100;
                auto input = makeImpulse (numChannels, position + latency + 256, position, 0.5f);
                const auto output = runLimiter (limiter, input, maxBlock);

                int peakIndex = 0;
                for (int i = 0; i < (int) output[0].size(); ++i)
                    if (std::abs (output[0][(size_t) i]) > std::abs (output[0][(size_t) peakIndex]))
                        peakIndex = i;

                expectEquals (peakIndex - position, latency,
                              "@" + juce::String (sampleRate) + " la=" + juce::String (lookaheadMs) + " clip=" + juce::String (clip));
            }
        }

        // 验收 4：任意输入不出 NaN / Inf，且采样点不超 Ceiling。
        void testNoNonFiniteOutput()
        {
            beginTest ("no NaN / Inf for extreme input");

            constexpr double sampleRate = 48000.0;

            for (auto clip : { 0.0f, 100.0f })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (24.0f, -3.0f, 1.0f, true, clip));

                auto input = makeNoise (numChannels, 48000, 1000.0f, 99);   // +60 dBFS 噪声再推 +24 dB
                input[0][1000] = 1.0e20f;
                input[1][2000] = -1.0e20f;

                const auto output = runLimiter (limiter, input, 333);
                expect (allFinite (output));

                float samplePeak = 0.0f;
                for (const auto& channel : output)
                    for (auto sample : channel)
                        samplePeak = juce::jmax (samplePeak, std::abs (sample));

                expect (samplePeak <= juce::Decibels::decibelsToGain (-3.0f) + 1.0e-6f);
            }
        }

        // 不同块大小（含超过 prepare 块大小的块）结果完全一致。
        void testBlockSizeIndependence()
        {
            beginTest ("output independent of block size");

            constexpr double sampleRate = 48000.0;
            const auto params = makeParams (12.0f, -1.0f, 3.0f, true, 40.0f);
            const auto input = makeNoise (numChannels, 48000, 0.8f, 3);

            Signal reference;

            for (auto blockSize : { 512, 1, 17, 256, 2048 })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, params);
                auto output = runLimiter (limiter, input, blockSize);

                if (reference.empty())
                    reference = output;
                else
                    expectEquals (maxAbsDifference (reference, output, 0, (int) input.front().size()), 0.0f,
                                  "block size " + juce::String (blockSize));
            }

            Fingerprint fingerprint;
            fingerprint.add (reference);
            logMessage ("  output fingerprint " + fingerprint.toString());
        }

        // Lookahead 变动：上报延迟立即更新，之后照样不超 Ceiling、不出 NaN。
        void testLookaheadChange()
        {
            beginTest ("lookahead change updates latency and stays safe");

            constexpr double sampleRate = 48000.0;
            auto params = makeParams (18.0f, -1.0f, 2.0f, true, 0.0f);

            LimiterModule limiter;
            prepareLimiter (limiter, sampleRate, params);
            const auto latencyBefore = limiter.getLatencySamples();

            auto first = runLimiter (limiter, makeSine (numChannels, sampleRate, 440.0, 1.0f, 0.3), maxBlock);

            params.lookaheadMs = 7.0f;
            limiter.setParams (params);
            const auto latencyAfter = limiter.getLatencySamples();
            expectEquals (latencyAfter - latencyBefore, juce::roundToInt (0.005 * sampleRate));

            auto second = runLimiter (limiter, makeSquare (numChannels, sampleRate, 100.0, 1.0f, 0.5), maxBlock);
            expect (allFinite (first) && allFinite (second));
            expect (toDb (truePeakOf (second)) <= -1.0f + 0.01f);

            Fingerprint fingerprint;
            fingerprint.add (first);
            fingerprint.add (second);
            logMessage ("  output fingerprint (before + after the change) " + fingerprint.toString());
        }

        void testGainReductionMeter()
        {
            beginTest ("gain reduction meter reports limiting");

            LimiterModule limiter;
            prepareLimiter (limiter, 48000.0, makeParams (12.0f, -1.0f, 2.0f, true, 0.0f));
            runLimiter (limiter, makeSine (numChannels, 48000.0, 1000.0, 1.0f, 0.3), maxBlock);

            // 0 dBFS 正弦推 +12 dB、Ceiling −1，加上 0.35 dB 的重建余量 → 约 13.35 dB 衰减。
            expectWithinAbsoluteError (limiter.getAndResetGainReductionDb(), 13.35f, 0.5f);
            expectEquals (limiter.getAndResetGainReductionDb(), 0.0f);
        }

        // 复核 V-05 回归：限过一次幅之后，增益必须能精确回到 1.0（null 重新是零）。
        void testGainReturnsExactlyToUnity()
        {
            beginTest ("gain returns exactly to 1.0 after limiting");

            constexpr double sampleRate = 48000.0;

            for (auto autoRelease : { false, true })
            {
                LimiterModule limiter;
                prepareLimiter (limiter, sampleRate, makeParams (0.0f, -1.0f, 2.0f, autoRelease, 0.0f));

                // 先 0.2 s 的 0 dBFS 方波（真峰值约 +2 dBTP，一定会限幅），再 10 s 低于 Ceiling 的信号。
                auto input = makeSquare (numChannels, sampleRate, 100.0, 1.0f, 0.2);
                const auto quiet = makeNoise (numChannels, (int) (10.0 * sampleRate), 0.2f, 21);
                for (int ch = 0; ch < numChannels; ++ch)
                    input[(size_t) ch].insert (input[(size_t) ch].end(), quiet[(size_t) ch].begin(), quiet[(size_t) ch].end());

                const auto length = (int) input.front().size();
                const auto latency = limiter.getLatencySamples();
                appendSilence (input, latency);
                const auto output = runLimiter (limiter, input, maxBlock);

                // 最后 1 s 与延迟对齐后的输入逐位相等。
                const auto lastSecond = (int) sampleRate;
                float diff = 0.0f;
                for (size_t ch = 0; ch < input.size(); ++ch)
                    for (int i = length - lastSecond; i < length; ++i)
                        diff = juce::jmax (diff, std::abs (input[ch][(size_t) i] - output[ch][(size_t) (i + latency)]));

                expectEquals (diff, 0.0f, autoRelease ? "auto release" : "manual release");

                Fingerprint fingerprint;
                fingerprint.add (output);
                logMessage (juce::String ("  output fingerprint (") + (autoRelease ? "auto" : "manual") + ") " + fingerprint.toString());
            }
        }

        // 播放中途重置（宿主 Reset、NaN 恢复）：先静音一个延迟长度，再 10 ms 淡入，之后回到逐位直通。
        void testRestartWithFadeIn()
        {
            beginTest ("restartWithFadeIn: silence, then fade, then bit-exact again");

            constexpr double sampleRate = 48000.0;
            LimiterModule limiter;
            prepareLimiter (limiter, sampleRate, makeParams (0.0f, -1.0f, 2.0f, true, 0.0f));

            const auto warmUp = makeSine (numChannels, sampleRate, 440.0, 0.5f, 0.2);
            runLimiter (limiter, warmUp, maxBlock);

            limiter.restartWithFadeIn();
            const auto latency = limiter.getLatencySamples();
            const auto input = makeSine (numChannels, sampleRate, 440.0, 0.5f, 0.5);
            const auto output = runLimiter (limiter, input, maxBlock);

            float silentPart = 0.0f;
            for (const auto& channel : output)
                for (int i = 0; i < latency; ++i)
                    silentPart = juce::jmax (silentPart, std::abs (channel[(size_t) i]));

            expectEquals (silentPart, 0.0f, "first latency samples must be silent");

            // 淡入 10 ms 之后：输出 = 延迟后的输入（逐位）。
            const auto settled = latency + (int) (0.02 * sampleRate);
            float diff = 0.0f;
            for (size_t ch = 0; ch < output.size(); ++ch)
                for (int i = settled; i < (int) output[ch].size(); ++i)
                    diff = juce::jmax (diff, std::abs (output[ch][(size_t) i] - input[ch][(size_t) (i - latency)]));

            expectEquals (diff, 0.0f, "after the fade the limiter is transparent again");

            Fingerprint fingerprint;
            fingerprint.add (output);
            logMessage ("  output fingerprint " + fingerprint.toString());
        }

        // Gain 的 20 ms 平滑：−60 dBFS 直流（加 +24 dB 也远不到 Ceiling −1，限幅器不动作）、Gain 0 → 24 dB 在块边界一步切换。
        // 逐样本增益（输出 / 输入）在切换生效前逐位为 1；之后单调不减、在 20 ms + 一块内到达 24 dB（15.849 倍，误差 1e-4）、
        // 此后逐位不变。依据：设计规格-总体「增益类参数经 SmoothedValue（20 ms）平滑，避免拉链噪声」；
        // 2026-09-25 评审 v0.4 R1-02：此前没有任何测试覆盖——把平滑换成立即跳变，全套测试仍全绿。
        void testGainIsSmoothed()
        {
            beginTest ("gain changes are smoothed over 20 ms: monotonic ramp, then exactly steady");

            constexpr double sampleRate = 48000.0;
            constexpr float dc = 0.001f;
            constexpr int blockSize = 64;
            const auto targetGain = juce::Decibels::decibelsToGain (24.0f);

            LimiterModule limiter;
            prepareLimiter (limiter, sampleRate, makeParams (0.0f, -1.0f, 2.0f, true, 0.0f));

            const auto switchAt = (int) (0.2 * sampleRate) / blockSize * blockSize;   // 块边界
            const auto total    = (int) (0.5 * sampleRate);
            const auto input    = Signal ((size_t) numChannels, std::vector<float> ((size_t) total, dc));

            Signal first, second;
            for (const auto& channel : input)
            {
                first.emplace_back (channel.begin(), channel.begin() + switchAt);
                second.emplace_back (channel.begin() + switchAt, channel.end());
            }

            auto output = runLimiter (limiter, first, blockSize);
            limiter.setParams (makeParams (24.0f, -1.0f, 2.0f, true, 0.0f));
            const auto tail = runLimiter (limiter, second, blockSize);
            for (size_t ch = 0; ch < output.size(); ++ch)
                output[ch].insert (output[ch].end(), tail[ch].begin(), tail[ch].end());

            const auto latency = limiter.getLatencySamples();
            const auto& left = output[0];

            // 延迟填满之后、切换生效之前：逐位等于输入。
            bool unityBefore = true;
            for (int i = latency; i < switchAt; ++i)
                unityBefore = unityBefore && left[(size_t) i] == dc;
            expect (unityBefore, "gain must be exactly 1 before the switch");

            // 切换之后：比值单调不减；记下第一次到达目标的位置。
            bool monotonic = true;
            int reachedAt = -1;
            for (int i = switchAt + 1; i < total; ++i)
            {
                monotonic = monotonic && left[(size_t) i] >= left[(size_t) (i - 1)];
                if (reachedAt < 0 && std::abs (left[(size_t) i] / dc - targetGain) < 1.0e-4f * targetGain)
                    reachedAt = i;
            }

            expect (monotonic, "gain ramp must be monotonic");
            expect (reachedAt > switchAt, "target gain must be reached");

            // 真的有斜坡：相邻样本的增益比最多只走一小步。平滑是乘法斜坡（dB 线性）：20 ms 从 1 到 15.85，每样本乘
            // 15.85^(1/960) ≈ 1.0029，最大一步在顶端约 0.046；平滑被拿掉时会一步跳 14.85（不论增益加在延迟线前还是后）。
            // 门槛 0.2：比真实斜坡大 4 倍，比无平滑小 70 倍。
            float largestStep = 0.0f;
            for (int i = switchAt + 1; i < total; ++i)
                largestStep = juce::jmax (largestStep, (left[(size_t) i] - left[(size_t) (i - 1)]) / dc);
            logMessage ("  largest per-sample gain step " + juce::String (largestStep, 4));
            expect (largestStep < 0.2f, "gain must not jump: largest per-sample step " + juce::String (largestStep, 4));

            const auto rampMs = 1000.0 * (reachedAt - switchAt) / sampleRate;
            logMessage ("  ramp reached +24 dB after " + juce::String (rampMs, 2) + " ms (latency " + juce::String (latency) + " samples)");
            expect (reachedAt - switchAt <= (int) (0.02 * sampleRate) + latency + blockSize,
                    "ramp must finish within 20 ms (+ latency + one block), took " + juce::String (rampMs, 2) + " ms");

            // 到达之后逐位不变。
            bool steadyAfter = true;
            for (int i = reachedAt + latency + blockSize; i < total; ++i)
                steadyAfter = steadyAfter && left[(size_t) i] == left[(size_t) (total - 1)];
            expect (steadyAfter, "gain must stay exactly constant after the ramp");
        }

        // v0.5 Clip 级空闲跳过（研究 P4 R2）：Clip = 0 且淡出走完后不跑 4× 过采样，重开前用最近的输入预滚过采样器。
        // 参照 = 同样参数、关掉跳过（setClipIdleSkipEnabledForTesting (false)，过采样器永远照常跑）的限幅器。
        // 覆盖：Clip 反复开关——淡出途中又打开（从未空闲）、空闲短于 / 等于 / 刚超过预滚长度 256、长时间空闲；随机块长
        // （含大于 prepare 块长的块）；空闲时改 Gain 与 Ceiling（削波阈值不动，跳过应继续）；Listen / Bypass 切换；
        // 中途 restartWithFadeIn。每个输出样本与每块的 GR 读数都要逐位相同，且跳过真的发生过。
        // 逐位相同的前提是每块声道数 = prepare 时的声道数（处理器一直如此）；少声道的块只要求安全，见下一条。
        void testClipIdleSkipIsBitExact()
        {
            beginTest ("clip idle skip is bit-exact with the never-skipping reference");

            for (auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
             for (auto blockLimit : { 64, 512 })
            {
                auto params = makeParams (12.0f, -1.0f, 2.0f, true, 0.0f);
                LimiterModule skipping, reference;
                reference.setClipIdleSkipEnabledForTesting (false);

                for (auto* limiter : { &skipping, &reference })
                {
                    limiter->setParams (params);
                    limiter->prepare ({ sampleRate, (juce::uint32) blockLimit, (juce::uint32) numChannels });
                }

                // 热信号：噪声 + 低频正弦，峰值约 0 dBFS，推 +12 dB 后削波与限幅都在动。
                constexpr double seconds = 3.0;
                auto input = makeNoise (numChannels, (int) (seconds * sampleRate), 0.5f, 31);
                const auto tone = makeSine (numChannels, sampleRate, 220.0, 0.5f, seconds);
                for (size_t ch = 0; ch < input.size(); ++ch)
                    for (size_t i = 0; i < input[ch].size(); ++i)
                        input[ch][i] += tone[ch][i];

                // Clip 时间表：交替「关 fade + offset 个样本」与「开若干样本」。fade = 20 ms 淡出；空闲从淡出走完算起，
                // 再过 256 个样本才开始跳过，所以 offset 取在 0 / 256 两个边界的两侧。表走完后一直关（长时间空闲）。
                const auto fade = (int) (0.02 * sampleRate);
                const int idleOffsets[] = { -400, -1, 0, 100, 255, 256, 257, 300, 1000, 20000 };
                const float clipValues[] = { 30.0f, 50.0f, 80.0f, 100.0f };
                std::vector<std::pair<int, float>> script;   // { 段落结束位置, 该段的 Clip }
                int scriptEnd = 0;

                for (int k = 0; k < 30; ++k)
                {
                    scriptEnd += fade + idleOffsets[k % 10];
                    script.push_back ({ scriptEnd, 0.0f });
                    scriptEnd += 300 + 700 * (k % 3);
                    script.push_back ({ scriptEnd, clipValues[k % 4] });
                }

                auto clipAt = [&script] (int position)
                {
                    for (const auto& segment : script)
                        if (position < segment.first)
                            return segment.second;

                    return 0.0f;
                };

                const auto total = (int) input.front().size();
                juce::Random random (41 + (int) sampleRate + blockLimit);
                int mismatches = 0, grMismatches = 0;
                bool restarted = false, retuned = false, bypassOn = false, bypassOff = false, listened = false;
                Fingerprint outputs;

                for (int start = 0; start < total;)
                {
                    const auto length = juce::jmin (total - start, 1 + random.nextInt (random.nextInt (8) == 0 ? 3000 : 700));

                    params.clip = clipAt (start);

                    if (! retuned && start > total / 2)
                    {
                        params.gainDb = 15.0f;       // 空闲时改 Gain / Ceiling：削波阈值不变，跳过继续
                        params.ceilingDb = -2.0f;
                        retuned = true;
                    }

                    if (! bypassOn && start > (int) (0.7 * total))   { params.bypass = true;  bypassOn = true; }
                    if (! bypassOff && start > (int) (0.75 * total)) { params.bypass = false; bypassOff = true; }
                    if (! listened && start > (int) (0.8 * total))   { params.listen = true;  listened = true; }

                    skipping.setParams (params);
                    reference.setParams (params);

                    if (! restarted && start > (int) (0.4 * total))
                    {
                        skipping.restartWithFadeIn (17);
                        reference.restartWithFadeIn (17);
                        restarted = true;
                    }

                    juce::AudioBuffer<float> a (numChannels, length), b (numChannels, length);

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        a.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);
                        b.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);
                    }

                    skipping.process (a);
                    reference.process (b);
                    outputs.add (a);

                    for (int ch = 0; ch < numChannels; ++ch)
                        mismatches += std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) length) == 0 ? 0 : 1;

                    const auto grSkipping = skipping.getAndResetGainReductionDb();
                    const auto grReference = reference.getAndResetGainReductionDb();
                    grMismatches += std::memcmp (&grSkipping, &grReference, sizeof (float)) == 0 ? 0 : 1;

                    start += length;
                }

                const auto caseName = "@" + juce::String (sampleRate) + " maxBlock " + juce::String (blockLimit);
                expectEquals (mismatches, 0, "blocks with differing output " + caseName);
                expectEquals (grMismatches, 0, "blocks with differing GR reading " + caseName);

                // 不能是空测试：跳过的样本要占相当比例（长空闲段 + 表走完后一直空闲），参照一个都不跳。
                const auto skipped = skipping.getClipIdleSkippedSamples();
                const auto skippedShare = (double) skipped / (double) total;
                logMessage ("  " + caseName + ": skipped " + juce::String (100.0 * skippedShare, 1) + "% of samples, output fingerprint "
                            + outputs.toString());
                expect (skippedShare > 0.3, "too few skipped samples (" + juce::String (skipped) + ") " + caseName);
                expectEquals (reference.getClipIdleSkippedSamples(), (juce::int64) 0);
            }
        }

        // 宿主给的块声道数少于 prepare 时（插件本身不会这样调）：跳过与重开照样安全——输出有限、采样点不超 Ceiling，
        // 之后回到全声道也照常。不要求与参照逐位相同：JUCE 的多级过采样器在少声道的块里，内层级会用上一块残留的数据
        // 推进缺席声道的状态，这份残留在跳过 / 不跳过两种情况下本来就不同，也没有意义。
        void testClipIdleSkipWithFewerChannelsIsSafe()
        {
            beginTest ("clip idle skip stays safe when a block has fewer channels than prepared");

            constexpr double sampleRate = 48000.0;
            auto params = makeParams (18.0f, -1.0f, 2.0f, true, 0.0f);
            LimiterModule limiter;
            prepareLimiter (limiter, sampleRate, params);

            const auto input = makeNoise (numChannels, (int) (2.0 * sampleRate), 1.0f, 57);
            const auto total = (int) input.front().size();
            juce::Random random (58);
            bool finite = true;
            float samplePeak = 0.0f;

            for (int start = 0, block = 0; start < total; ++block)
            {
                const auto length = juce::jmin (total - start, 1 + random.nextInt (900));
                params.clip = (block / 20) % 2 == 0 ? 0.0f : 60.0f;   // 约每 20 块开关一次：有跳过，也有重开
                limiter.setParams (params);

                const auto channelsThisBlock = block % 7 == 3 ? 1 : numChannels;
                juce::AudioBuffer<float> buffer (channelsThisBlock, length);

                for (int ch = 0; ch < channelsThisBlock; ++ch)
                    buffer.copyFrom (ch, 0, input[(size_t) ch].data() + start, length);

                limiter.process (buffer);

                for (int ch = 0; ch < channelsThisBlock; ++ch)
                    for (int i = 0; i < length; ++i)
                    {
                        const auto y = buffer.getSample (ch, i);
                        finite = finite && std::isfinite (y);
                        samplePeak = juce::jmax (samplePeak, std::abs (y));
                    }

                start += length;
            }

            expect (finite);
            expect (samplePeak <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-6f);
            expect (limiter.getClipIdleSkippedSamples() > 0);
        }
    };

    static LimiterTests limiterTests;
}
