/*
  ==============================================================================

    LimiterTests.cpp
    设计规格-Limit「验收 · 自动测试」四条 + 分块无关性 + Lookahead 变动；
    v0.5：Clip 级空闲跳过与「永不跳过」的参照逐位相同。

  ==============================================================================
*/

#include "TestHelpers.h"

#include <cstring>

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

        double lastSecondRms (const Signal& signal, double sampleRate)
        {
            const auto count = (int) sampleRate;
            const auto first = (int) signal[0].size() - count;
            double energy = 0.0;
            for (int i = 0; i < count; ++i)
                energy += juce::square ((double) signal[0][(size_t) (first + i)]);
            return std::sqrt (energy / count);
        }
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
            testLowFrequencyReleaseReducesIntermodulation();
            testLowFrequencyReleasePreservesTransientsAndManual();
            testLowFrequencyReleaseRejectsUnstablePeriods();
            testLowFrequencyReleaseBlockResetAndAllocation();
            testShortOverloadDoesNotHoldTheSlowEnvelope();
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

        // 验收 1：各种信号推 +12 / +24 dB → 输出真峰值 ≤ Ceiling + 0.01 dB。
        void testTruePeakNeverExceedsCeiling()
        {
            beginTest ("true peak <= ceiling + 0.01 dB");

            float worstOwnDb = -100.0f, worstReferenceDb = -100.0f;
            juce::String worstOwnCase, worstReferenceCase;
            int runs = 0, referenceRuns = 0, referenceOver01 = 0, referenceOver03 = 0;

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

            logMessage ("  runs: " + juce::String (runs));
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
                        + juce::String (worstReferenceDb, 4) + " dB above ceiling [" + worstCase + "]");

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
                logMessage ("  " + caseName + ": skipped " + juce::String (100.0 * skippedShare, 1) + "% of samples");
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
