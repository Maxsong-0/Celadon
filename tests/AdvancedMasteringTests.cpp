/*
    Signal-level acceptance for optional mastering features. Helpers allocate
    outside measured audio calls; the zero-new checks use TestMain's counter
    (which intentionally does not claim to intercept direct malloc calls).
*/
#include "TestHelpers.h"

#include "../Source/dsp/ReferenceSpectrum.h"
#include "../Source/dsp/ReleaseSafety.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace celadon::test
{
    namespace
    {
        constexpr double masteringTestRate = 48000.0;
        constexpr int referenceLength = 480000;

        bool identicalSignals (const Signal& a, const Signal& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t ch = 0; ch < a.size(); ++ch)
                if (a[ch].size() != b[ch].size()
                    || std::memcmp (a[ch].data(), b[ch].data(), a[ch].size() * sizeof (float)) != 0)
                    return false;
            return true;
        }

        Signal renderRelease (ReleaseSafety& module, Signal signal, float lufs, int blockSize)
        {
            const auto length = (int) signal[0].size();
            for (int offset = 0; offset < length; offset += blockSize)
            {
                const auto count = std::min (blockSize, length - offset);
                std::array<float*, 2> pointers { signal[0].data() + offset, nullptr };
                if (signal.size() == 2)
                    pointers[1] = signal[1].data() + offset;
                juce::AudioBuffer<float> view (pointers.data(), (int) signal.size(), count);
                module.process (view, lufs);
            }
            return signal;
        }

        Signal referenceAudio (bool antiPhase = true, float gain = 1.0f)
        {
            auto signal = makeSine (2, masteringTestRate, 1000.0, 0.4f, 10.0);
            for (int i = 0; i < referenceLength; ++i)
            {
                const auto high = (float) (0.2 * std::sin (juce::MathConstants<double>::twoPi
                                                         * 4000.0 * i / masteringTestRate));
                const auto value = (signal[0][(size_t) i] + high) * gain;
                signal[0][(size_t) i] = value;
                signal[1][(size_t) i] = antiPhase ? -value : value;
            }
            return signal;
        }

        void feedReference (ReferenceSpectrum& module, Signal& signal, int offset, int length,
                            int blockSize, bool connected = true)
        {
            const auto end = offset + length;
            for (int position = offset; position < end; position += blockSize)
            {
                std::array<float*, 2> pointers { signal[0].data() + position, nullptr };
                if (signal.size() == 2)
                    pointers[1] = signal[1].data() + position;
                juce::AudioBuffer<float> view (pointers.data(), (int) signal.size(),
                                               std::min (blockSize, end - position));
                module.process (view, connected);
            }
        }

        void flushReference (ReferenceSpectrum& reference, int blockSize = 31, bool connected = true)
        {
            // At most 250 ms of processing time, never additional reference signal.
            auto zeros = makeSilence (2, 12000);
            feedReference (reference, zeros, 0, 12000, blockSize, connected);
        }

        bool sameCurve (const ReferenceSpectrum::Snapshot& a, const ReferenceSpectrum::Snapshot& b,
                        float tolerance = 0.0f)
        {
            if (a.hasReference != b.hasReference)
                return false;
            for (size_t band = 0; band < a.relativeDb.size(); ++band)
                if (! std::isfinite (a.relativeDb[band]) || ! std::isfinite (b.relativeDb[band])
                    || std::abs (a.relativeDb[band] - b.relativeDb[band]) > tolerance)
                    return false;
            return true;
        }
    }

    class AdvancedMasteringTests : public juce::UnitTest
    {
    public:
        AdvancedMasteringTests() : juce::UnitTest ("AdvancedMastering", "Celadon") {}

        void runTest() override
        {
            testPreview();
            testDither();
            const auto committed = testReferenceSignal();
            testReferenceWaiting();
            testReferenceScheduling();
            if (committed != nullptr && committed->getNumChildElements() == ReferenceSpectrum::numBands)
                testReferenceState (*committed);
            testAudioAllocations();
        }

    private:
        void testPreview()
        {
            beginTest ("preview applies a real -6 dB gain after 50 ms, equally to both channels");
            ReleaseSafety module;
            module.prepare (masteringTestRate);
            module.setParams (true, -14.0f, 0);
            auto input = makeSine (2, masteringTestRate, 1000.0, 0.4f, 0.2, 0.3);
            const auto output = renderRelease (module, input, -8.0f, 127);
            double beforePower = 0.0, afterPower = 0.0, largestError = 0.0;
            const auto expectedGain = std::pow (10.0, -6.0 / 20.0);
            for (size_t i = 2400; i < input[0].size(); ++i)
            {
                beforePower += (double) input[0][i] * input[0][i];
                afterPower += (double) output[0][i] * output[0][i];
                largestError = std::max (largestError, std::abs ((double) output[0][i] - input[0][i] * expectedGain));
            }
            expectWithinAbsoluteError (10.0 * std::log10 (afterPower / beforePower), -6.0, 0.001);
            expectWithinAbsoluteError (module.getPreviewGainDb(), -6.0f, 0.0001f);
            expect (largestError < 1.0e-7, "the samples, not just the UI gain, are attenuated");
            expect (std::memcmp (output[0].data(), output[1].data(), output[0].size() * sizeof (float)) == 0);
            expect (std::abs (output[0][0]) < std::abs (input[0][0])
                    && std::abs (output[0][0]) > std::abs (input[0][0]) * 0.99f,
                    "preview begins a ramp rather than jumping to its final gain");

            beginTest ("quiet or unknown loudness never boosts; silence stays silent");
            for (const auto reading : { -24.0f, -90.0f, -100.0f,
                                       std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::infinity() })
            {
                module.reset();
                const auto unchanged = renderRelease (module, input, reading, 511);
                expect (identicalSignals (unchanged, input));
                expectEquals (module.getPreviewGainDb(), 0.0f);
            }
            auto silence = makeSilence (1, 4800);
            expect (identicalSignals (renderRelease (module, silence, -8.0f, 31), silence));

            beginTest ("invalid metering recovers unity and disabled preview becomes bit-identical");
            const auto recovered = renderRelease (module, input,
                std::numeric_limits<float>::quiet_NaN(), 127);
            expectEquals (module.getPreviewGainDb(), 0.0f);
            expect (std::memcmp (recovered[0].data() + 2400, input[0].data() + 2400,
                                (input[0].size() - 2400) * sizeof (float)) == 0);
            renderRelease (module, input, -8.0f, 127);
            module.setParams (false, -14.0f, 0);
            renderRelease (module, input, -8.0f, 127);
            input[0][0] = -0.0f;
            input[0][1] = std::numeric_limits<float>::quiet_NaN();
            input[1][0] = std::numeric_limits<float>::infinity();
            expect (identicalSignals (renderRelease (module, input, -8.0f, 127), input),
                    "settled bypass preserves all input bits, including signed zero");
        }

        void testDither()
        {
            for (const auto bits : { 16, 24 })
            {
                beginTest ("TPDF " + juce::String (bits) + " bit: PCM grid, unbiased noise, independent channels");
                constexpr int count = 131072;
                const auto scale = std::ldexp (1.0, bits - 1);
                ReleaseSafety module;
                module.prepare (masteringTestRate);
                module.setParams (false, -14.0f, bits);
                expectEquals (module.getDitherBits(), bits);
                auto silence = makeSilence (2, count);
                const auto noise = renderRelease (module, silence, -90.0f, count);
                std::array<double, 2> sums {}, powers {};
                double cross = 0.0;
                bool onGrid = true, bounded = true, hasPositive = false, hasNegative = false;
                for (int i = 0; i < count; ++i)
                {
                    const auto left = (double) noise[0][(size_t) i] * scale;
                    const auto right = (double) noise[1][(size_t) i] * scale;
                    cross += left * right;
                    hasPositive = hasPositive || left > 0.0;
                    hasNegative = hasNegative || left < 0.0;
                    for (size_t ch = 0; ch < 2; ++ch)
                    {
                        const auto value = (double) noise[ch][(size_t) i] * scale;
                        onGrid = onGrid && std::isfinite (value) && value == std::round (value);
                        bounded = bounded && std::abs (value) <= 1.0;
                        sums[ch] += value;
                        powers[ch] += value * value;
                    }
                }
                expect (onGrid && bounded && hasPositive && hasNegative);
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    expect (std::abs (sums[ch] / count) < 0.01, "no truncation-induced DC bias");
                    expectWithinAbsoluteError (std::sqrt (powers[ch] / count), 0.5, 0.015);
                }
                const auto covariance = cross - sums[0] * sums[1] / count;
                const auto correlation = covariance / std::sqrt (
                    (powers[0] - sums[0] * sums[0] / count) * (powers[1] - sums[1] * sums[1] / count));
                expect (std::abs (correlation) < 0.02, "left and right dither are independent");

                beginTest ("TPDF " + juce::String (bits) + " bit: reset reproducibility and block-size invariance");
                module.reset();
                expect (identicalSignals (noise, renderRelease (module, silence, -90.0f, count)));
                for (const auto block : { 1, 127, 512, 8192 })
                {
                    module.reset();
                    expect (identicalSignals (noise, renderRelease (module, silence, -90.0f, block)),
                            "block size " + juce::String (block));
                }

                beginTest ("TPDF preserves sub-LSB mean and saturates malformed/full-scale inputs at " + juce::String (bits));
                for (auto& sample : silence[0]) sample = (float) (0.25 / scale);
                for (auto& sample : silence[1]) sample = (float) (-0.25 / scale);
                module.reset();
                const auto fractional = renderRelease (module, silence, -90.0f, 127);
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    double mean = 0.0;
                    for (const auto sample : fractional[ch]) mean += (double) sample * scale;
                    expectWithinAbsoluteError (mean / count, ch == 0 ? 0.25 : -0.25, 0.015);
                }
                Signal extremes {{ -2.0f, 2.0f, -1.0f, 1.0f,
                    std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }};
                const auto saturated = renderRelease (module, extremes, -90.0f, 3);
                bool safe = true;
                for (const auto sample : saturated[0])
                    safe = safe && std::isfinite (sample) && sample >= -1.0f && sample <= 1.0 - 1.0 / scale
                         && (double) sample * scale == std::round ((double) sample * scale);
                expect (safe);
                expectEquals (saturated[0][0], -1.0f);
                expectEquals (saturated[0][1], (float) (1.0 - 1.0 / scale));
            }
        }

        std::unique_ptr<juce::XmlElement> testReferenceSignal()
        {
            beginTest ("reference requires ten actual seconds of sidechain; anti-phase stereo does not cancel");
            auto audio = referenceAudio();
            ReferenceSpectrum reference;
            reference.prepare ({ masteringTestRate, 512, 2 });
            reference.requestLearn();
            feedReference (reference, audio, 0, referenceLength - 1, 31);
            expect (! reference.getSnapshot().hasReference, "9.99998 seconds is not a finished reference");
            feedReference (reference, audio, referenceLength - 1, 1, 1);
            flushReference (reference, 31, false); // A disconnected bus must still drain an already-captured terminal frame.
            const auto learned = reference.getSnapshot();
            expect (learned.hasReference && learned.state == ReferenceSpectrum::State::ready);
            expectWithinAbsoluteError (learned.seconds, 10.0, 1.0e-9);
            expectWithinAbsoluteError (learned.relativeDb[14], (float) (10.0 * std::log10 (0.8)), 0.08f);
            expectWithinAbsoluteError (learned.relativeDb[20], (float) (10.0 * std::log10 (0.2)), 0.08f);
            expect (learned.relativeDb[4] < -50.0f, "the spectrum follows real frequencies, not a flat placeholder");

            beginTest ("incremental FFT preserves identical spectra at 1, 31 and 4093 sample callbacks");
            for (const auto block : { 1, 4093 })
            {
                ReferenceSpectrum identical;
                identical.prepare ({ masteringTestRate, 4096, 2 });
                identical.requestLearn();
                feedReference (identical, audio, 0, referenceLength, block);
                flushReference (identical, block);
                expect (sameCurve (learned, identical.getSnapshot(), 0.0f));
            }
            beginTest ("cancel or clear a terminal FFT cannot resurrect its pending reference");
            for (const auto cancel : { false, true })
            {
                ReferenceSpectrum pending;
                pending.prepare ({ masteringTestRate, 512, 2 });
                pending.requestLearn();
                feedReference (pending, audio, 0, referenceLength, 31);
                expect (! pending.getSnapshot().hasReference);
                if (cancel) pending.cancelLearn(); else pending.clear();
                flushReference (pending);
                expect (! pending.getSnapshot().hasReference);
                expect (pending.getSnapshot().state == ReferenceSpectrum::State::idle);
            }
            beginTest ("reference spectrum is invariant to channel polarity, gain and host block size");
            for (const auto gain : { 1.0f, 0.03125f })
            {
                auto comparisonAudio = referenceAudio (false, gain);
                ReferenceSpectrum comparison;
                comparison.prepare ({ masteringTestRate, 4096, 2 });
                comparison.requestLearn();
                feedReference (comparison, comparisonAudio, 0, referenceLength, 4093);
                flushReference (comparison, 4093);
                expect (comparison.getSnapshot().hasReference);
                expect (sameCurve (learned, comparison.getSnapshot(), 0.01f),
                        "relative spectrum survives a 30 dB input-level change and L/R polarity change");
            }
            return reference.createStateXml();
        }

        void testReferenceWaiting()
        {
            beginTest ("silence and disconnected sidechain cannot complete or advance reference learning");
            auto silence = makeSilence (2, referenceLength);
            auto audio = referenceAudio();
            ReferenceSpectrum reference;
            reference.prepare ({ masteringTestRate, 512, 2 });
            reference.requestLearn();
            feedReference (reference, silence, 0, referenceLength, 512);
            expect (! reference.getSnapshot().hasReference);
            expectEquals (reference.getSnapshot().seconds, 0.0);
            feedReference (reference, audio, 0, referenceLength, 512, false);
            expect (reference.getSnapshot().state == ReferenceSpectrum::State::waitingForSidechain);
            expect (! reference.getSnapshot().hasReference && ! reference.getSnapshot().sidechainConnected);
            expectEquals (reference.getSnapshot().seconds, 0.0);
            feedReference (reference, audio, 0, 144000, 512);
            const auto progress = reference.getSnapshot().seconds;
            expect (progress > 2.7 && progress <= 3.0);
            feedReference (reference, audio, 0, referenceLength, 512, false);
            feedReference (reference, silence, 0, referenceLength, 512);
            const auto drainedProgress = reference.getSnapshot().seconds;
            expect (drainedProgress >= progress && drainedProgress <= 3.0);
            feedReference (reference, silence, 0, referenceLength, 31);
            expectEquals (reference.getSnapshot().seconds, drainedProgress);
            expect (! reference.getSnapshot().hasReference, "elapsed wall/audio time cannot replace valid sidechain evidence");
            feedReference (reference, audio, 0, 384000, 511);
            flushReference (reference);
            expect (reference.getSnapshot().hasReference, "learning resumes when valid sidechain returns");
        }

        void testReferenceScheduling()
        {
            beginTest ("small terminal frames survive disconnected drain and a preceding invalid frame at 32 kHz");
            for (const auto invalidPredecessor : { false, true })
            {
                constexpr double rate = 32000.0;
                constexpr int target = 320000, frame = 8192;
                const auto length = target + (invalidPredecessor ? frame : 0);
                auto audio = makeSine (2, rate, 1000.0, 0.4f, (double) length / rate);
                if (invalidPredecessor)
                    for (auto& channel : audio)
                        std::fill (channel.begin() + 38 * frame, channel.begin() + 39 * frame, 0.4f); // DC passes energy gating but fails spectral evidence.
                ReferenceSpectrum learner;
                learner.prepare ({ rate, 4096, 2 });
                learner.requestLearn();
                feedReference (learner, audio, 0, length - 1, 31);
                expect (! learner.getSnapshot().hasReference);
                feedReference (learner, audio, length - 1, 1, 1);
                auto empty = makeSilence (2, 12000);
                feedReference (learner, empty, 0, 12000, 31, false);
                expect (learner.getSnapshot().hasReference, "complete 512-sample terminal prefix must not be discarded");
                expectWithinAbsoluteError (learner.getSnapshot().relativeDb[14], 0.0f, 0.10f);
                expectEquals (learner.getSnapshot().seconds, 10.0);
            }

            beginTest ("an invalid terminal prefix preserves already-captured valid suffix samples");
            {
                constexpr double rate = 32000.0;
                constexpr int target = 320000, frame = 8192;
                auto audio = makeSine (2, rate, 1000.0, 0.4f, (double) (target + frame) / rate);
                for (auto& channel : audio)
                {
                    std::fill (channel.begin() + 39 * frame, channel.begin() + target, 0.0f);
                    for (int i = target; i < target + frame; ++i)
                        channel[(size_t) i] = (float) (0.4 * std::sin (juce::MathConstants<double>::twoPi * 4000.0 * i / rate));
                }
                ReferenceSpectrum learner;
                learner.prepare ({ rate, 32, 2 }); learner.requestLearn();
                feedReference (learner, audio, 0, target + frame, 31);
                auto empty = makeSilence (2, 12000);
                feedReference (learner, empty, 0, 12000, 31, false);
                expect (learner.getSnapshot().hasReference);
                // 39 complete 1 kHz windows followed by one COMPLETE 4 kHz
                // window. Dropping its early suffix would lower this proportion.
                expectWithinAbsoluteError (learner.getSnapshot().relativeDb[20], (float) (10.0 * std::log10 (1.0 / 40.0)), 0.08f);
            }

            beginTest ("incremental reference shape and completion are block-size independent at 96 and 192 kHz");
            for (const auto rate : { 96000.0, 192000.0 })
            {
                const auto length = (int) (rate * 10.0);
                auto audio = makeSine (2, rate, 1000.0, 0.4f, 10.0);
                auto empty = makeSilence (2, (int) (rate * 0.4));
                ReferenceSpectrum small, large;
                small.prepare ({ rate, 32, 2 }); large.prepare ({ rate, 4096, 2 });
                small.requestLearn(); large.requestLearn();
                feedReference (small, audio, 0, length - 1, 32);
                expect (! small.getSnapshot().hasReference);
                feedReference (small, audio, length - 1, 1, 1);
                feedReference (large, audio, 0, length, 4093);
                feedReference (small, empty, 0, (int) empty[0].size(), 32, false);
                feedReference (large, empty, 0, (int) empty[0].size(), 4093, false);
                expect (small.getSnapshot().hasReference && large.getSnapshot().hasReference);
                expect (sameCurve (small.getSnapshot(), large.getSnapshot(), 0.0f));
                expectWithinAbsoluteError (small.getSnapshot().relativeDb[14], 0.0f, 0.10f);
            }

            beginTest ("a pending Learn survives repeated prepare before the first audio block");
            ReferenceSpectrum pending;
            pending.requestLearn();
            pending.prepare ({ masteringTestRate, 512, 2 });
            pending.prepare ({ masteringTestRate, 512, 2 });
            auto audio = referenceAudio();
            feedReference (pending, audio, 0, 48000, 31);
            expect (pending.getSnapshot().state == ReferenceSpectrum::State::learning);
            expect (pending.getSnapshot().seconds > 0.5);
        }

        void testReferenceState (const juce::XmlElement& committed)
        {
            beginTest ("XML save/restore publishes immediately and reprepare preserves the committed curve");
            ReferenceSpectrum reference;
            reference.restoreStateXml (&committed);
            const auto baseline = reference.getSnapshot();
            expect (baseline.hasReference && baseline.state == ReferenceSpectrum::State::ready);
            const auto pendingSave = reference.createStateXml();
            ReferenceSpectrum roundTrip;
            roundTrip.restoreStateXml (pendingSave.get());
            expect (sameCurve (baseline, roundTrip.getSnapshot()));
            reference.prepare ({ 96000.0, 1024, 2 });
            expect (sameCurve (baseline, reference.getSnapshot()));
            auto audio = referenceAudio();
            reference.requestLearn();
            feedReference (reference, audio, 0, 48000, 511);
            expect (reference.getSnapshot().state == ReferenceSpectrum::State::learning);
            reference.prepare ({ masteringTestRate, 512, 2 });
            expect (sameCurve (baseline, reference.getSnapshot()));
            expect (reference.getSnapshot().state == ReferenceSpectrum::State::ready,
                    "a partial replacement is abandoned on reprepare");

            beginTest ("cancel preserves the previous reference; clear is not swallowed by later UI commands");
            reference.requestLearn();
            feedReference (reference, audio, 0, 48000, 511);
            reference.cancelLearn();
            expect (sameCurve (baseline, reference.getSnapshot()));
            expect (reference.getSnapshot().state == ReferenceSpectrum::State::ready);
            const auto cancelledSave = reference.createStateXml();
            roundTrip.restoreStateXml (cancelledSave.get());
            expect (sameCurve (baseline, roundTrip.getSnapshot()), "cancel can be saved before the next audio callback");
            feedReference (reference, audio, 0, 512, 512);
            expect (sameCurve (baseline, reference.getSnapshot()));
            reference.clear();
            reference.requestLearn();
            reference.cancelLearn();
            expect (! reference.getSnapshot().hasReference);
            expectEquals (reference.createStateXml()->getNumChildElements(), 0);
            feedReference (reference, audio, 0, 512, 512);
            expect (! reference.getSnapshot().hasReference);
            expect (reference.getSnapshot().state == ReferenceSpectrum::State::idle);

            beginTest ("reprepare discards uncommitted progress without inventing a reference");
            reference.requestLearn();
            feedReference (reference, audio, 0, 48000, 511);
            expect (reference.getSnapshot().seconds > 0.5);
            reference.prepare ({ masteringTestRate, 512, 2 });
            expect (! reference.getSnapshot().hasReference);
            expectEquals (reference.getSnapshot().seconds, 0.0);

            beginTest ("invalid/missing XML cannot inject or resurrect a reference curve");
            const auto reject = [&] (const juce::XmlElement* xml)
            {
                reference.restoreStateXml (&committed);
                expect (reference.getSnapshot().hasReference);
                reference.restoreStateXml (xml);
                expect (! reference.getSnapshot().hasReference);
                feedReference (reference, audio, 0, 512, 512);
                expect (! reference.getSnapshot().hasReference);
                expect (reference.getSnapshot().state == ReferenceSpectrum::State::idle);
            };
            reject (nullptr);
            for (int invalid = 0; invalid < 11; ++invalid)
            {
                juce::XmlElement bad (committed);
                auto* first = bad.getChildElement (0);
                switch (invalid)
                {
                    case 0: bad.setTagName ("WRONG"); break;
                    case 1: bad.setAttribute ("format", 99); break;
                    case 2: bad.setAttribute ("seconds", 9.999); break;
                    case 3: bad.removeChildElement (first, true); break;
                    case 4: bad.getChildElement (1)->setAttribute ("index", 0); break;
                    case 5: first->setAttribute ("index", 0.5); break;
                    case 6: first->setAttribute ("relativeDb", "nan"); break;
                    case 7: first->setAttribute ("relativeDb", "-3junk"); break;
                    case 8: first->setAttribute ("relativeDb", -121.0); break;
                    case 9:
                    case 10:
                        for (auto* band : bad.getChildIterator())
                            band->setAttribute ("relativeDb", invalid == 9 ? -120.0 : 0.0);
                        break;
                    default: break;
                }
                reject (&bad);
            }
        }

        void testAudioAllocations()
        {
            beginTest ("reference FFT, learning completion, commands and release processing make zero operator-new calls");
            ReferenceSpectrum reference;
            ReleaseSafety release;
            reference.prepare ({ masteringTestRate, 512, 2 });
            release.prepare (masteringTestRate);
            release.setParams (true, -14.0f, 24);
            reference.requestLearn();
            juce::AudioBuffer<float> buffer (2, 512);
            std::uint64_t allocations = 0;
            ReferenceSpectrum::Snapshot last;
            for (int block = 0; block < 938; ++block)
            {
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const auto sample = (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi
                        * 1000.0 * (block * 512 + i) / masteringTestRate));
                    buffer.setSample (0, i, sample);
                    buffer.setSample (1, i, -sample);
                }
                const auto before = allocationCountOnThisThread();
                reference.process (buffer, true);
                release.process (buffer, -8.0f);
                last = reference.getSnapshot();
                allocations += allocationCountOnThisThread() - before;
            }
            for (int offset = 0; offset < 12000; offset += 512)
            {
                buffer.clear();
                juce::AudioBuffer<float> tail (buffer.getArrayOfWritePointers(), 2, std::min (512, 12000 - offset));
                const auto countBefore = allocationCountOnThisThread();
                reference.process (tail, true);
                release.process (tail, -8.0f);
                last = reference.getSnapshot();
                allocations += allocationCountOnThisThread() - countBefore;
            }
            expect (last.hasReference, "the allocation window includes the final commit of a real learned spectrum");
            const auto before = allocationCountOnThisThread();
            reference.clear();
            reference.requestLearn();
            reference.process (buffer, false);
            reference.cancelLearn();
            reference.process (buffer, true);
            release.setParams (false, -14.0f, 16);
            release.reset();
            release.process (buffer, -8.0f);
            allocations += allocationCountOnThisThread() - before;
            expectEquals ((juce::int64) allocations, (juce::int64) 0);
        }
    };

    static AdvancedMasteringTests advancedMasteringTests;
}

#include "../Source/dsp/CeladonChain.h"

namespace celadon::test
{
    namespace
    {
        Signal masteringMultitone (double seconds)
        {
            auto signal = makeSilence (2, (int) (seconds * masteringTestRate));
            for (int band = 3; band <= 25; ++band)
            {
                const auto hz = 1000.0 * std::pow (2.0, (double) (band - 14) / 3.0);
                for (size_t i = 0; i < signal[0].size(); ++i)
                {
                    const auto value = (float) (0.015 * std::sin (juce::MathConstants<double>::twoPi * hz
                                                 * (double) i / masteringTestRate + (double) band * 0.71));
                    signal[0][i] += value;
                    signal[1][i] += value;
                }
            }
            return signal;
        }

        double toneAmplitude (const Signal& signal, double hz, int start, int length)
        {
            double re = 0.0, im = 0.0;
            for (int i = start; i < start + length; ++i)
            {
                const auto phase = juce::MathConstants<double>::twoPi * hz * i / masteringTestRate;
                re += signal[0][(size_t) i] * std::cos (phase);
                im += signal[0][(size_t) i] * std::sin (phase);
            }
            return 2.0 * std::hypot (re, im) / length;
        }

        Signal renderBalanceMastering (BalanceModule& balance, Signal audio, int blockSize = 257)
        {
            for (int offset = 0; offset < (int) audio[0].size(); offset += blockSize)
            {
                float* ptrs[] { audio[0].data() + offset, audio[1].data() + offset };
                juce::AudioBuffer<float> block (ptrs, 2, std::min (blockSize, (int) audio[0].size() - offset));
                balance.process (block);
            }
            return audio;
        }

        void prepareMasteringChain (CeladonChain& chain, const ChainParams& params)
        {
            chain.setParams (params, false);
            chain.prepare ({ masteringTestRate, 512, 2 });
            chain.setParams (params, false);
        }

        Signal renderMasteringChain (CeladonChain& chain, const ChainParams& params, Signal audio,
                                      bool bypass = false, bool original = false, bool difference = false)
        {
            constexpr std::array<int, 5> sizes { 37, 512, 63, 1024, 127 };
            int blockIndex = 0;
            for (int offset = 0; offset < (int) audio[0].size();)
            {
                const auto count = std::min (sizes[(size_t) (blockIndex++ % 5)], (int) audio[0].size() - offset);
                float* ptrs[] { audio[0].data() + offset, audio[1].data() + offset };
                juce::AudioBuffer<float> block (ptrs, 2, count);
                chain.setParams (params, bypass, original, difference);
                chain.process (block);
                offset += count;
            }
            return audio;
        }

        //==============================================================================
        // T-032（2026-09-29，R2-01 / R2-08 验收与逐位证据）用到的工具。
        // 粉噪：Paul Kellet 7 极点近似，与 BalanceTests.cpp 同一组系数——那边在另一个编译单元的匿名命名空间里，这里只能再写一份。
        Signal masteringPinkNoise (int channels, int numSamples, float rmsDbfs, juce::int64 seed)
        {
            constexpr int warmUp = 8192;
            auto signal = makeSilence (channels, numSamples);
            const auto targetRms = (double) juce::Decibels::decibelsToGain (rmsDbfs);

            for (int ch = 0; ch < channels; ++ch)
            {
                juce::Random random (seed + (juce::int64) ch * 7919);
                double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
                auto& channel = signal[(size_t) ch];
                double sumOfSquares = 0.0;

                for (int i = -warmUp; i < numSamples; ++i)
                {
                    const auto white = (double) random.nextFloat() * 2.0 - 1.0;
                    b0 = 0.99886 * b0 + white * 0.0555179;
                    b1 = 0.99332 * b1 + white * 0.0750759;
                    b2 = 0.96900 * b2 + white * 0.1538520;
                    b3 = 0.86650 * b3 + white * 0.3104856;
                    b4 = 0.55000 * b4 + white * 0.5329522;
                    b5 = -0.7616 * b5 - white * 0.0168980;
                    const auto pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
                    b6 = white * 0.115926;

                    if (i >= 0)
                    {
                        channel[(size_t) i] = (float) pink;
                        sumOfSquares += pink * pink;
                    }
                }

                const auto gain = (float) (targetRms / std::sqrt (sumOfSquares / (double) numSamples));

                for (auto& sample : channel)
                    sample *= gain;
            }

            return signal;
        }

        // [start, end) 内乘 gain，两端 rampSamples 线性斜坡：电平阶跃 / 鼓点用，斜坡只为不让硬切的宽带咔哒混进来。
        void applyGainSegment (Signal& signal, int start, int end, float gain, int rampSamples)
        {
            start = std::max (0, start);
            end = std::min ((int) signal[0].size(), end);

            for (int i = start; i < end; ++i)
            {
                const auto in = (float) (i - start + 1) / (float) rampSamples;
                const auto out = (float) (end - i) / (float) rampSamples;
                const auto g = 1.0f + (gain - 1.0f) * std::min (1.0f, std::min (in, out));

                for (auto& channel : signal)
                    channel[(size_t) i] *= g;
            }
        }

        struct HarshRun
        {
            Signal output;
            std::vector<float> harshDb;                     // 每块读一次 getHarshReductionDb()
            std::vector<BalanceModule::BandGains> gains;    // 同时读的 27 段 G
        };

        // 按 512 一块送进 Balance，每块处理完读一次刺耳读数与 27 段 G。48 kHz 下块 = hop = 512：每块正好一笔（拆片时最后一片在
        // hop 后第 1/8 hop 处发布，落在同一块内），读数序列就是每 hop 的序列；其他采样率只作摘要用。
        // beforeBlock (块序号) 在每块处理之前调（换参数用）；块边界与 hop 边界重合，所以第 k 块起设的参数正好从第 k 块的 hop 起生效。
        template <typename BeforeBlock>
        HarshRun renderHarshPerBlockWith (BalanceModule& module, Signal audio, BeforeBlock&& beforeBlock)
        {
            constexpr int blockSize = 512;
            HarshRun run;
            int blockIndex = 0;

            for (int offset = 0; offset < (int) audio[0].size(); offset += blockSize, ++blockIndex)
            {
                beforeBlock (blockIndex);
                float* ptrs[] { audio[0].data() + offset, audio[1].data() + offset };
                juce::AudioBuffer<float> block (ptrs, 2, std::min (blockSize, (int) audio[0].size() - offset));
                module.process (block);
                run.harshDb.push_back (module.getHarshReductionDb());
                BalanceModule::BandGains gains {};
                module.getBandGainsDb (gains);
                run.gains.push_back (gains);
            }

            run.output = std::move (audio);
            return run;
        }

        HarshRun renderHarshPerBlock (BalanceModule& module, Signal audio)
        {
            return renderHarshPerBlockWith (module, std::move (audio), [] (int) {});
        }

        // FNV-1a 64 位摘要（同 BalanceTests.cpp「logged digests」）：只用来比同一台机器、同一套编译选项下改前 / 改后的两次构建。
        juce::uint64 fnv1a (const void* data, size_t size, juce::uint64 value)
        {
            const auto* bytes = static_cast<const unsigned char*> (data);

            for (size_t i = 0; i < size; ++i)
            {
                value ^= (juce::uint64) bytes[i];
                value *= 1099511628211ull;
            }

            return value;
        }

        constexpr juce::uint64 fnv1aBasis = 14695981039346656037ull;

        juce::uint64 digestOf (const Signal& signal)
        {
            auto value = fnv1aBasis;
            for (const auto& channel : signal)
                value = fnv1a (channel.data(), channel.size() * sizeof (float), value);
            return value;
        }

        juce::uint64 digestOf (const std::vector<BalanceModule::BandGains>& gains)
        {
            auto value = fnv1aBasis;
            for (const auto& g : gains)
                value = fnv1a (g.data(), sizeof (g), value);
            return value;
        }

        juce::uint64 digestOf (const std::vector<float>& values)
        {
            return fnv1a (values.data(), values.size() * sizeof (float), fnv1aBasis);
        }

        juce::String hex (juce::uint64 value)
        {
            return "0x" + juce::String::toHexString ((juce::int64) value);
        }
    }

    class AdvancedMasteringChainTests : public juce::UnitTest
    {
    public:
        AdvancedMasteringChainTests() : juce::UnitTest ("AdvancedMasteringChain", "Celadon") {}
        void runTest() override
        {
            testNeutral();
            testHarshness();
            testHarshIgnoresLevelStep();          // T-032 R2-01
            testHarshIgnoresDrumPulses();         // T-032 R2-01
            testHarshCymbalBurstObservation();    // T-032 核验漏项（只记录）
            testHarshOnsetRecoveryIsSmoothed();   // T-032 R2-08
            testHarshSwitchOnHasNoStaleOnset();   // T-032 R2-01 / R2-08（陈旧的起音状态）
            testTargets();
            testDigests();                        // T-032 逐位证据（只记录）
            testReleaseRouting();
            testMonitoringHandover();
            testGenreAndDelivery();               // T-038
        }
    private:
        //==============================================================================
        // T-038（设计规格-曲风与交付 第 3.1、4.1 节；设计规格-曲风-House学习方式 第 8 节）：链路上的三处小行为——
        // 流媒体档封顶有效 Ceiling、House 档「目标频谱 = 自动」按曲风取斜率并能无损回喂、House 档限幅跟拍的慢段释放上限。
        // 通用 + 标准一律不动（逐位与 main 相同由 22 首回归与 LegacyFixtureTests 把关，这里只核对实际参数）。
        void testGenreAndDelivery()
        {
            beginTest ("T-038: streaming caps the effective ceiling at -2 dBTP; standard and club leave it; a lower user ceiling wins");
            {
                for (int delivery = 0; delivery < glaze::numDeliveries; ++delivery)
                {
                    ChainParams params;
                    params.limit.ceilingDb = -1.0f;
                    params.glazeDelivery = delivery;
                    CeladonChain chain;
                    prepareMasteringChain (chain, params);
                    expectEquals (chain.getEffectiveParams().limit.ceilingDb, delivery == (int) glaze::Delivery::streaming ? -2.0f : -1.0f);
                }

                ChainParams lower;
                lower.limit.ceilingDb = -3.0f;
                lower.glazeDelivery = (int) glaze::Delivery::streaming;
                CeladonChain chain;
                prepareMasteringChain (chain, lower);
                expectEquals (chain.getEffectiveParams().limit.ceilingDb, -3.0f);

                ChainParams margin;   // 编码余量先扣，再与 -2 取小
                margin.limit.ceilingDb = -0.5f;
                margin.mastering.codecMarginDb = 1.0f;
                margin.glazeDelivery = (int) glaze::Delivery::streaming;
                CeladonChain withMargin;
                prepareMasteringChain (withMargin, margin);
                expectEquals (withMargin.getEffectiveParams().limit.ceilingDb, -2.0f);
            }

            beginTest ("T-038: House automatic spectrum target uses the genre slope and feeds back losslessly at GLAZE 0");
            {
                // 通用：风格「自动」→ 中性 -0.75（与 main 相同）；Deep -1.5、Tech -0.75、Melodic-Progressive -1.25。
                constexpr float expected[glaze::numGenres] { -0.75f, -1.5f, -0.75f, -1.25f };

                for (int genre = 0; genre < glaze::numGenres; ++genre)
                {
                    ChainParams params;
                    params.mastering.targetMode = TargetMode::automatic;
                    params.glazeGenre = genre;
                    CeladonChain chain;
                    prepareMasteringChain (chain, params);
                    const auto effective = chain.getEffectiveParams();
                    expectEquals (effective.balance.targetSlopeDbPerOct, expected[genre]);
                    expect (effective.mastering.targetSlopeResolvedSet == (genre != (int) glaze::Genre::general));
                    expect (effective.mastering.targetMode != TargetMode::automatic);

                    CeladonChain replay;   // 实际参数原样再喂（同决策重放的用法）：斜率逐位相同
                    prepareMasteringChain (replay, effective);
                    expectEquals (replay.getEffectiveParams().balance.targetSlopeDbPerOct, expected[genre]);
                }
            }

            beginTest ("T-038: House beat release cap = one beat / 3, only with auto release, a playing host and 100..140 BPM");
            {
                const auto capFor = [this] (int genre, bool autoRelease, bool hasTempo, double bpm, bool playing)
                {
                    ChainParams params;
                    params.glazeGenre = genre;
                    params.limit.autoRelease = autoRelease;
                    CeladonChain chain;
                    prepareMasteringChain (chain, params);

                    glaze::HostInfo info;
                    info.hasPlayState = true;
                    info.isPlaying = playing;
                    info.hasTempo = hasTempo;
                    info.bpm = bpm;
                    chain.setHostInfo (info);
                    chain.setParams (params, false);
                    return chain.getEffectiveParams().limit.beatReleaseCapMs;
                };

                const auto deep = (int) glaze::Genre::deepHouse;
                expectEquals (capFor (deep, true, true, 124.0, true), (float) (60000.0 / 124.0 / 3.0));
                expectEquals (capFor ((int) glaze::Genre::techHouse, true, true, 128.0, true), (float) (60000.0 / 128.0 / 3.0));
                expectEquals (capFor ((int) glaze::Genre::general, true, true, 124.0, true), 0.0f);
                expectEquals (capFor (deep, false, true, 124.0, true), 0.0f);    // 手动 Release
                expectEquals (capFor (deep, true, true, 90.0, true), 0.0f);      // BPM 不在 100 ～ 140
                expectEquals (capFor (deep, true, true, 150.0, true), 0.0f);
                expectEquals (capFor (deep, true, false, 0.0, true), 0.0f);      // 宿主没给速度
                expectEquals (capFor (deep, true, true, 124.0, false), 0.0f);    // 没在播放
            }
        }

        //==============================================================================
        // T-032 R2-01：宽带电平一降不许被当成「刺耳」。粉噪 −20 dBFS（本身平滑：每个 1/3 倍频程功率相同，2 ～ 8 kHz 各段与两邻段
        // 均值之差的期望约 0 dB，在 1.5 dB 死区之内），6 s 处整体 −10 dB；harshAmount = 1、起音保留默认 0.5。改前本段用 Welch
        // + 攻击 80 / 释放 400 ms 的 Lfast、邻段用 60 ms 快电平：电平一降本段挂在释放曲线上、超出量凭空出现，R2 / V2 复算峰值 2.99 dB
        // （> 1 dB 持续 0.70 s）；改后三者同一 60 ms 动态，共同变化相消。断言阶跃后 2 s 内 getHarshReductionDb() 峰值 < 0.3 dB
        // （粉噪 60 ms 估计的统计抖动偶尔越过 1.5 dB 死区，留 0.3 dB），阶跃前 2 s 同样 < 0.3 dB（对照：稳态本来就不该有）。
        void testHarshIgnoresLevelStep()
        {
            beginTest ("harshness ignores a broadband -10 dB level step (own band and neighbours share the 60 ms tracker)");
            constexpr double seconds = 10.0, stepAt = 6.0;
            auto signal = masteringPinkNoise (2, (int) (seconds * masteringTestRate), -20.0f, 20260929);
            applyGainSegment (signal, (int) (stepAt * masteringTestRate), (int) signal[0].size(),
                              juce::Decibels::decibelsToGain (-10.0f), 48);
            BalanceModule module;
            BalanceModule::Params params;
            params.harshAmount = 1.0f;
            module.setParams (params);
            module.prepare ({ masteringTestRate, 512, 2 });
            const auto run = renderHarshPerBlock (module, signal);
            float beforePeak = 0.0f, afterPeak = 0.0f;
            double overOneDbSeconds = 0.0;
            for (size_t k = 0; k < run.harshDb.size(); ++k)
            {
                const auto t = (double) (k + 1) * 512.0 / masteringTestRate;
                if (t >= stepAt - 2.0 && t < stepAt) beforePeak = std::max (beforePeak, run.harshDb[k]);
                if (t >= stepAt && t < stepAt + 2.0)
                {
                    afterPeak = std::max (afterPeak, run.harshDb[k]);
                    if (run.harshDb[k] > 1.0f) overOneDbSeconds += 512.0 / masteringTestRate;
                }
            }
            logMessage ("  harsh reduction peak: 2 s before the -10 dB step " + juce::String (beforePeak, 3)
                        + " dB, 2 s after " + juce::String (afterPeak, 3) + " dB (> 1 dB for "
                        + juce::String (overOneDbSeconds, 3) + " s; R2 measured 2.99 dB / 0.70 s before the fix)");
            expect (allFinite (run.output));
            expect (beforePeak < 0.3f, "steady pink noise is not harsh: " + juce::String (beforePeak, 3));
            expect (afterPeak < 0.3f, "a broadband level drop must not read as harshness: " + juce::String (afterPeak, 3));
        }

        // T-032 R2-01：鼓点式脉冲——250 ms 周期、前 50 ms −12 dBFS、其余 −30 dBFS 的粉噪（全段同步变化，1 ms 斜坡），8 s。改前每个
        // 间隙都把 2 ～ 8 kHz 压暗（R2 复算：h = 1 时均值 1.84 dB、100 % 时间 > 1 dB）。断言 3 s 起 2 ～ 8 kHz 各段 G 的均值 > −0.3 dB、
        // 刺耳读数均值 < 0.3 dB。对照：素材真的在跳（击打段与间隙的 RMS 相差 18 dB ± 1）。
        void testHarshIgnoresDrumPulses()
        {
            beginTest ("harshness ignores drum-like broadband pulses (50 ms at -12 dBFS every 250 ms over -30 dBFS)");
            constexpr double seconds = 8.0;
            constexpr int period = 12000, hitLength = 2400;
            auto signal = masteringPinkNoise (2, (int) (seconds * masteringTestRate), -30.0f, 20260930);
            for (int start = 0; start < (int) signal[0].size(); start += period)
                applyGainSegment (signal, start, start + hitLength, juce::Decibels::decibelsToGain (18.0f), 48);

            double hitPower = 0.0, gapPower = 0.0;
            int hitCount = 0, gapCount = 0;
            for (int i = (int) (3.0 * masteringTestRate); i < (int) signal[0].size(); ++i)
            {
                const auto inHit = (i % period) >= 200 && (i % period) < hitLength - 200;
                const auto inGap = (i % period) >= hitLength + 200 && (i % period) < period - 200;
                const auto p = (double) signal[0][(size_t) i] * signal[0][(size_t) i];
                if (inHit) { hitPower += p; ++hitCount; }
                if (inGap) { gapPower += p; ++gapCount; }
            }
            const auto contrastDb = 10.0 * std::log10 ((hitPower / hitCount) / (gapPower / gapCount));
            expect (std::abs (contrastDb - 18.0) < 1.0, "positive control: the pulses really are 18 dB above the gaps: " + juce::String (contrastDb, 2));

            BalanceModule module;
            BalanceModule::Params params;
            params.harshAmount = 1.0f;
            module.setParams (params);
            module.prepare ({ masteringTestRate, 512, 2 });
            const auto run = renderHarshPerBlock (module, signal);
            const auto firstHop = (size_t) (3.0 * masteringTestRate / 512.0);
            double harshSum = 0.0;
            float harshPeak = 0.0f;
            std::array<double, 27> gainSum {};
            int hops = 0;
            for (size_t k = firstHop; k < run.harshDb.size(); ++k)
            {
                harshSum += run.harshDb[k];
                harshPeak = std::max (harshPeak, run.harshDb[k]);
                for (size_t b = 0; b < 27; ++b) gainSum[b] += run.gains[k][b];
                ++hops;
            }
            float lowestMeanGain = 0.0f;
            for (int b = 0; b < 27; ++b)
            {
                const auto hz = BalanceModule::getBandCentreHz (b);
                if (hz >= 1900.0f && hz <= 8200.0f)
                    lowestMeanGain = std::min (lowestMeanGain, (float) (gainSum[(size_t) b] / hops));
            }
            logMessage ("  drum pulses from 3 s: harsh reduction mean " + juce::String (harshSum / hops, 3) + " dB, peak "
                        + juce::String (harshPeak, 3) + " dB; lowest 2-8 kHz band G mean " + juce::String (lowestMeanGain, 3)
                        + " dB (R2 measured mean 1.84 dB before the fix)");
            expect (allFinite (run.output));
            expect (harshSum / hops < 0.3, "drum gaps must not be read as harshness: mean " + juce::String (harshSum / hops, 3));
            expect (lowestMeanGain > -0.3f, "2-8 kHz bands are not darkened by the pulses: " + juce::String (lowestMeanGain, 3));
        }

        // T-032 核验漏项（V2 第 3 节第 4 条）：镲片式 150 ms 亮瞬态。粉噪 −20 dBFS 底 + 从 2 s 起每 0.5 s 一次 150 ms 的 3.5 ～ 9 kHz
        // 带限白噪突发（约 −14 dBFS，把 4 ～ 8 kHz 各段抬约 14 dB；2 ms 斜坡）。R2-01 修后判据用 60 ms 快电平，对 60 ～ 200 ms 的亮
        // 瞬态比原来（Welch + 80 ms 攻击）反应快；55 ms 起音保持只盖前 55 ms、p = 0.5 只放一半。这里只把一次突发的逐 hop 读数与
        // 尾巴记进日志，不设门槛——要不要像自动 Tame 那样加持续时间门槛是另一个设计决定（待 Max）。
        void testHarshCymbalBurstObservation()
        {
            beginTest ("cymbal-like 150 ms bright bursts: log the per-hop harsh reduction and its tail (observation only)");
            constexpr double seconds = 8.0, firstBurst = 2.0, burstPeriod = 0.5, burstLength = 0.15;
            const auto length = (int) (seconds * masteringTestRate);
            auto signal = masteringPinkNoise (2, length, -20.0f, 20260931);
            auto bright = makeNoise (2, length, 1.0f, 20260932);
            for (auto& channel : bright)
            {
                juce::dsp::IIR::Filter<float> highA, highB, low;
                highA.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (masteringTestRate, 3500.0f);
                highB.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (masteringTestRate, 3500.0f);
                low.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (masteringTestRate, 9000.0f);
                for (auto& sample : channel)
                    sample = low.processSample (highB.processSample (highA.processSample (sample)));
            }
            double brightPower = 0.0;
            for (auto sample : bright[0]) brightPower += (double) sample * sample;
            const auto brightGain = (float) (juce::Decibels::decibelsToGain (-14.0) / std::sqrt (brightPower / length));
            const auto ramp = (int) (0.002 * masteringTestRate);
            for (double start = firstBurst; start + burstLength < seconds - 0.5; start += burstPeriod)
            {
                const auto first = (int) (start * masteringTestRate), last = first + (int) (burstLength * masteringTestRate);
                for (int i = first; i < last; ++i)
                {
                    const auto envelope = std::min (1.0f, std::min ((float) (i - first + 1) / ramp, (float) (last - i) / ramp));
                    for (size_t ch = 0; ch < 2; ++ch)
                        signal[ch][(size_t) i] += bright[ch][(size_t) i] * brightGain * envelope;
                }
            }

            BalanceModule module;
            BalanceModule::Params params;
            params.harshAmount = 1.0f;
            module.setParams (params);
            module.prepare ({ masteringTestRate, 512, 2 });
            const auto run = renderHarshPerBlock (module, signal);
            expect (allFinite (run.output));

            // 取 4.0 s 的那一次突发：从突发起点前一 hop 到起点后 300 ms，逐 hop 列出；再给峰值、55 / 110 / 150 ms 处与尾巴。
            const auto hopSeconds = 512.0 / masteringTestRate;
            const auto burstHop = (size_t) std::lround (4.0 / hopSeconds);
            juce::String trace;
            float peakInBurst = 0.0f, peakInTail = 0.0f;
            const auto readingAt = [&] (double offsetSeconds)
            {
                return run.harshDb[std::min (run.harshDb.size() - 1, burstHop + (size_t) std::lround (offsetSeconds / hopSeconds))];
            };
            for (size_t k = burstHop - 1; k <= burstHop + 28 && k < run.harshDb.size(); ++k)
            {
                trace += (k == burstHop - 1 ? "" : " ") + juce::String (run.harshDb[k], 2);
                const auto offset = ((double) k - (double) burstHop) * hopSeconds;
                if (offset >= 0.0 && offset < burstLength) peakInBurst = std::max (peakInBurst, run.harshDb[k]);
                if (offset >= burstLength && offset < burstLength + 0.15) peakInTail = std::max (peakInTail, run.harshDb[k]);
            }
            logMessage ("  cymbal burst at 4.0 s, per-hop harsh reduction (dB) from one hop before: " + trace);
            logMessage ("  peak during the 150 ms burst " + juce::String (peakInBurst, 2) + " dB; at +55 ms "
                        + juce::String (readingAt (0.055), 2) + ", +110 ms " + juce::String (readingAt (0.110), 2) + ", +150 ms "
                        + juce::String (readingAt (0.150), 2) + " dB; peak in the 150 ms after the burst " + juce::String (peakInTail, 2) + " dB");
        }

        // T-032 R2-08：起音保留的恢复侧不再硬切。素材同「persistent 3.15 kHz peak」（刺耳包络封顶 E = 3 dB）+ 从 3 s 起每 0.5 s 一次
        // 62.5 ms 的 ×3 宽带突发（起音判据 P > 1.7·P_prev 触发 55 ms 保持）；harshAmount = 1、起音保留默认 p = 0.5。保持窗口里已施加
        // 的削减仍立刻乘 1 − p（起音侧不变），窗口结束后乘数以 20 ms 一阶爬回 1。
        // 门槛的来历（每 hop 10.67 ms，c = e^(−10.67/20) = 0.587，攻击 a = e^(−10.67/25) = 0.653）：保持期间包络的目标也只有 (1 − p)·E，
        // 按 200 ms 释放往下走，保持结束时包络 e0 在 0.5E ～ 0.86E 之间（保持 6 hop 为 0.86E，越长越低）。结束后第一 hop 读数的升幅 =
        // e1·(1 − p·c) − e0·(1 − p)，e1 = E − (E − e0)·a：e0 = 0.86E 时 0.64 dB，e0 → 0.5E 时趋近上界 0.68 dB；改前乘数硬跳回 1，
        // 升幅 = e1 − e0·(1 − p)，同样范围内为 1.44 ～ 1.27 dB（R2 计最大 1.5 dB）。任务书写的「≤ 0.6 dB」在 10 ～ 20 ms 平滑下做
        // 不到——单是乘数这一项就有 p·E·(1 − c) = 0.62 dB——所以断言 2 s 起相邻 hop 读数的正向台阶 ≤ 0.75 dB（上界 0.68 加余量，
        // 仍远低于改前的 1.27）。对照：突发期间读数确实压到稳态的 0.7 倍以下（起音侧仍立刻闪避）、稳态读数 ≥ 2.5 dB（包络确实在
        // 封顶附近，台阶不是因为没东西可跳）。
        void testHarshOnsetRecoveryIsSmoothed()
        {
            beginTest ("transient-preserve: the reduction returns after an onset hold without a hard step (<= 0.75 dB per hop)");
            auto signal = makeNoise (2, 8 * 48000, 0.07f, 937);
            for (size_t i = 0; i < signal[0].size(); ++i)
                for (auto& ch : signal)
                    ch[i] += (float) (0.12 * std::sin (juce::MathConstants<double>::twoPi * 3150.0 * (double) i / masteringTestRate));
            for (int burst = 0; burst < 10; ++burst)
            {
                const auto start = 3 * 48000 + burst * 24000;
                for (int i = start; i < start + 3000; ++i)
                    for (auto& ch : signal) ch[(size_t) i] *= 3.0f;
            }
            BalanceModule module;
            BalanceModule::Params params;
            params.harshAmount = 1.0f;
            module.setParams (params);
            module.prepare ({ masteringTestRate, 512, 2 });
            const auto run = renderHarshPerBlock (module, signal);
            const auto hopOf = [] (double s) { return (size_t) (s * masteringTestRate / 512.0); };
            float steady = 0.0f, dip = 1000.0f, largestRise = 0.0f;
            size_t riseAt = 0;
            for (size_t k = hopOf (2.0); k < run.harshDb.size(); ++k)
            {
                if (k < hopOf (3.0)) steady = std::max (steady, run.harshDb[k]);
                else dip = std::min (dip, run.harshDb[k]);
                const auto rise = run.harshDb[k] - run.harshDb[k - 1];
                if (rise > largestRise) { largestRise = rise; riseAt = k; }
            }
            logMessage ("  steady reduction " + juce::String (steady, 3) + " dB, deepest duck during bursts " + juce::String (dip, 3)
                        + " dB, largest per-hop rise " + juce::String (largestRise, 3) + " dB at " + juce::String ((double) riseAt * 512.0 / masteringTestRate, 3)
                        + " s (hard release before the fix: about 1.4 dB)");
            expect (allFinite (run.output));
            expect (steady >= 2.5f, "positive control: the persistent peak keeps the envelope near its 3 dB cap: " + juce::String (steady, 3));
            expect (dip <= 0.7f * steady, "positive control: onsets still duck the applied reduction immediately: " + juce::String (dip, 3));
            expect (largestRise <= 0.75f, "the reduction must climb back smoothly after an onset hold: " + juce::String (largestRise, 3));
        }

        // T-032 R2-01（顺带修的陈旧状态）与 R2-08：刺耳从关到开的第一 hop 就按全力起步，不被陈旧状态当成起音。素材同
        // 「persistent 3.15 kHz peak」（正弦比所在段的噪声高约 20 dB，段功率很稳，自己不会触发起音），起音保留默认 0.5，块 512 = hop：
        //   1. 前 281 块（约 3 s）h = 0，之后 h = 1。原来起音比较用的「上一帧功率」只在母带块运行时才更新，新实例里还是 reset 时的 0，
        //      打开那一 hop「本帧 > 1.7 × 上一帧」必然成立，白开一次 55 ms 起音保持：目标与已施加的削减都只剩一半，第一 hop 读数约
        //      3 × (1 − e^(−10.67/25)) × 0.5 × 0.5 ≈ 0.26 dB。修后与自动 Tame 共用每 hop 更新的上一帧功率，第一 hop ≈ 1.04 dB。
        //   2. 第 468 块起一次 ×3、3000 样本的宽带突发（真起音，第 470 块那一 hop 帧功率约升 3.8 倍），第 472 块把 h 关掉（此时起音
        //      保持还在走）；余尾约 2.5 s 归零、母带块停下，第 750 块（8.0 s）再打开。原来 h = 0 时保持计时冻结，再打开的头几 hop
        //      仍按起音处理（同样约 0.26 dB）；修后计时照常走完、闪避乘数也回到 1 才停块，再打开的第一 hop 与情形 1 一样约 1.04 dB。
        // 断言两次打开的第一 hop 读数都 > 0.7 dB。阳性对照：从没打开时读数精确为 0；突发真的开了起音保持（关掉之前两 hop 的读数
        // 压到稳态的 0.7 倍以下）；再打开之前余尾确实已归零、母带块已停（读数精确为 0）。
        void testHarshSwitchOnHasNoStaleOnset()
        {
            beginTest ("harshness switched on, or back on after its tail, starts at full strength (no stale onset hold)");
            constexpr int blockSize = 512, switchOnBlock = 281, burstBlock = 468, switchOffBlock = 472, switchBackBlock = 750;
            auto signal = makeNoise (2, 9 * 48000, 0.07f, 937);
            for (size_t i = 0; i < signal[0].size(); ++i)
                for (auto& ch : signal)
                    ch[i] += (float) (0.12 * std::sin (juce::MathConstants<double>::twoPi * 3150.0 * (double) i / masteringTestRate));
            for (int i = burstBlock * blockSize; i < burstBlock * blockSize + 3000; ++i)
                for (auto& ch : signal) ch[(size_t) i] *= 3.0f;

            BalanceModule::Params off, on;
            on.harshAmount = 1.0f;
            BalanceModule module;
            module.setParams (off);
            module.prepare ({ masteringTestRate, (juce::uint32) blockSize, 2 });
            const auto run = renderHarshPerBlockWith (module, signal, [&] (int blockIndex)
            {
                const auto enabled = (blockIndex >= switchOnBlock && blockIndex < switchOffBlock) || blockIndex >= switchBackBlock;
                module.setParams (enabled ? on : off);
            });

            float steady = 0.0f;
            for (int k = burstBlock - 12; k < burstBlock; ++k)
                steady = std::max (steady, run.harshDb[(size_t) k]);
            const auto ducked = std::min (run.harshDb[(size_t) switchOffBlock - 2], run.harshDb[(size_t) switchOffBlock - 1]);
            const auto firstOn = run.harshDb[(size_t) switchOnBlock];
            const auto firstBack = run.harshDb[(size_t) switchBackBlock];
            logMessage ("  first hop after switching on " + juce::String (firstOn, 3) + " dB, after switching back on "
                        + juce::String (firstBack, 3) + " dB (about 0.26 dB before the fix); steady " + juce::String (steady, 3)
                        + " dB, onset duck before switching off " + juce::String (ducked, 3) + " dB");
            expect (allFinite (run.output));
            expectEquals (run.harshDb[(size_t) switchOnBlock - 1], 0.0f, "positive control: nothing is published before the first switch-on");
            expect (ducked <= 0.7f * steady, "positive control: the burst opened an onset hold before harshness was switched off: "
                                                 + juce::String (ducked, 3) + " of " + juce::String (steady, 3));
            expectEquals (run.harshDb[(size_t) switchBackBlock - 1], 0.0f, "positive control: the tail has ended before harshness returns");
            expect (firstOn > 0.7f, "first hop after switching on must not be treated as an onset: " + juce::String (firstOn, 3));
            expect (firstBack > 0.7f, "first hop after switching back on must not inherit a frozen onset hold: " + juce::String (firstBack, 3));
        }

        // T-032 逐位证据：固定素材、固定参数下输出、每块 G 与每块刺耳读数的 FNV-1a 摘要写进日志（grep「T032 digest」），改前 / 改后在
        // 同一台机器、同一套编译选项下人工比对（同 BalanceTests「logged digests」的做法）。三组：
        //   target：目标频谱 t = 1（温暖）、刺耳 0——走目标块的测量（R2-06：均值改读已算好的 quickLevelDb、逐段 measured 保留原式、
        //           目标时间常数预算），这几处本身各采样率逐位不变；R2-07 只改 ≥ 88.2 kHz 的系数重算间隔，所以改后 44.1 / 48 kHz 的
        //           摘要与改前相同，96 / 192 kHz 随之改变（把 R2-07 单独退回的中间构建四个采样率都与改前相同，见 IMPL-A1 报告）；
        //   harsh：刺耳 h = 1、p = 0.5——R2-01 / R2-08 有意改变，摘要只作记录；
        //   both：两者同开。
        void testDigests()
        {
            beginTest ("T-032 digests: output, per-block G and harsh readings for target-only, harsh-only and both (compare by hand)");
            struct Case { const char* name; float harsh; float target; };
            const Case cases[] = { { "target", 0.0f, 1.0f }, { "harsh", 1.0f, 0.0f }, { "both", 1.0f, 1.0f } };
            for (auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                auto signal = masteringPinkNoise (2, (int) (3.0 * rate), -20.0f, 20260933);
                for (size_t i = 0; i < signal[0].size(); ++i)
                    for (auto& ch : signal)
                        ch[i] += (float) (0.05 * std::sin (juce::MathConstants<double>::twoPi * 3150.0 * (double) i / rate));
                for (const auto& c : cases)
                {
                    BalanceModule module;
                    BalanceModule::Params p;
                    p.harshAmount = c.harsh;
                    p.targetAmount = c.target;
                    p.targetSlopeDbPerOct = -1.5f;
                    module.setParams (p);
                    module.prepare ({ rate, 512, 2 });
                    const auto run = renderHarshPerBlock (module, signal);
                    logMessage ("  T032 digest " + juce::String (c.name) + " @" + juce::String (rate, 0) + " Hz: output " + hex (digestOf (run.output))
                                + ", G " + hex (digestOf (run.gains)) + ", harsh " + hex (digestOf (run.harshDb))
                                + " (" + juce::String ((int) run.gains.size()) + " readings)");
                    expect (allFinite (run.output), juce::String (c.name) + " @" + juce::String (rate, 0));
                }
            }
        }

        void testNeutral()
        {
            beginTest ("GLAZE zero with new automatic defaults is bit-identical to the established manual chain");
            ChainParams legacy;
            legacy.balance.recover = 20.0f;
            legacy.balance.tame = 25.0f;
            legacy.color.toneDb = 0.4f;
            legacy.limit.gainDb = 4.0f;
            auto automatic = legacy;
            automatic.mastering.harshMode = HarshMode::automatic;
            automatic.mastering.targetMode = TargetMode::automatic;
            CeladonChain a, b;
            prepareMasteringChain (a, legacy);
            prepareMasteringChain (b, automatic);
            const auto signal = masteringMultitone (2.0);
            const auto oldOutput = renderMasteringChain (a, legacy, signal);
            const auto newOutput = renderMasteringChain (b, automatic, signal);
            expect (identicalSignals (oldOutput, newOutput));
            expectEquals (a.getLatencySamples(), b.getLatencySamples());
            a.reset(); b.reset();
            expect (identicalSignals (renderMasteringChain (a, legacy, signal), renderMasteringChain (b, automatic, signal)));
            expectEquals (b.getMasteringStatus().harshReductionDb, 0.0f);
            for (const auto value : b.getMasteringStatus().targetCorrectionDb) expectEquals (value, 0.0f);
        }

        void testHarshness()
        {
            beginTest ("additional protection reduces a persistent 3.15 kHz peak in actual output");
            auto signal = makeNoise (2, 6 * 48000, 0.07f, 937);
            for (size_t i = 0; i < signal[0].size(); ++i)
                for (auto& ch : signal)
                    ch[i] += (float) (0.12 * std::sin (juce::MathConstants<double>::twoPi * 3150.0 * (double) i / masteringTestRate));
            BalanceModule module;
            BalanceModule::Params params;
            params.harshAmount = 1.0f;
            params.transientPreserve = 0.0f;
            module.setParams (params);
            module.prepare ({ masteringTestRate, 512, 2 });
            const auto output = renderBalanceMastering (module, signal);
            const auto ratio = toneAmplitude (output, 3150.0, 5 * 48000, 48000) / toneAmplitude (signal, 3150.0, 5 * 48000, 48000);
            expect (ratio < 0.96 && ratio > 0.45, "persistent harsh tone must be audibly but boundedly attenuated: " + juce::String (ratio));
            expect (module.getHarshReductionDb() > 0.3f && module.getHarshReductionDb() <= 3.01f);

            beginTest ("transient-preserve changes burst attack samples while retaining steady protection");
            auto bursts = signal;
            for (int burst = 0; burst < 8; ++burst)
            {
                const auto start = 3 * 48000 + burst * 12000;
                for (int i = start; i < start + 3000; ++i)
                    for (auto& ch : bursts) ch[(size_t) i] *= 3.0f;
            }
            module.reset();
            const auto unprotected = renderBalanceMastering (module, bursts);
            params.transientPreserve = 1.0f;
            BalanceModule preserved;
            preserved.setParams (params);
            preserved.prepare ({ masteringTestRate, 512, 2 });
            const auto protectedAudio = renderBalanceMastering (preserved, bursts);
            double preservedPower = 0.0, unprotectedPower = 0.0;
            for (int burst = 0; burst < 8; ++burst)
                for (int i = 3 * 48000 + burst * 12000; i < 3 * 48000 + burst * 12000 + 3000; ++i)
                {
                    preservedPower += std::pow ((double) protectedAudio[0][(size_t) i], 2.0);
                    unprotectedPower += std::pow ((double) unprotected[0][(size_t) i], 2.0);
                }
            expect (preservedPower > unprotectedPower * 1.002, "onset preservation must affect audio energy, not only a meter");
        }

        void testTargets()
        {
            beginTest ("warm and bright targets move the output spectrum in opposite directions");
            const auto input = masteringMultitone (8.0);
            BalanceModule warm, bright;
            BalanceModule::Params p;
            p.targetAmount = 1.0f;
            p.targetSlopeDbPerOct = -1.5f;
            warm.setParams (p); warm.prepare ({ masteringTestRate, 512, 2 });
            p.targetSlopeDbPerOct = 0.0f;
            bright.setParams (p); bright.prepare ({ masteringTestRate, 512, 2 });
            const auto warmAudio = renderBalanceMastering (warm, input);
            const auto brightAudio = renderBalanceMastering (bright, input);
            const auto lowRatio = toneAmplitude (warmAudio, 250.0, 6 * 48000, 96000) / toneAmplitude (brightAudio, 250.0, 6 * 48000, 96000);
            const auto highRatio = toneAmplitude (warmAudio, 4000.0, 6 * 48000, 96000) / toneAmplitude (brightAudio, 4000.0, 6 * 48000, 96000);
            expect (lowRatio > 1.04, "warm target raises the lower spectrum relative to bright: " + juce::String (lowRatio));
            expect (highRatio < 0.96, "warm target lowers the upper spectrum relative to bright: " + juce::String (highRatio));
            expectEquals (warm.getLatencySamples(), 0);

            beginTest ("target shape ignores a common +6 dB input level step");
            BalanceModule unchanged, stepped;
            p.targetSlopeDbPerOct = -0.75f;
            unchanged.setParams (p); stepped.setParams (p);
            unchanged.prepare ({ masteringTestRate, 512, 2 }); stepped.prepare ({ masteringTestRate, 512, 2 });
            auto louder = input;
            for (auto& ch : louder)
                for (size_t i = 4 * 48000; i < ch.size(); ++i) ch[i] *= 2.0f;
            renderBalanceMastering (unchanged, input); renderBalanceMastering (stepped, louder);
            BalanceModule::BandGains first {}, second {};
            unchanged.getTargetCorrectionDb (first); stepped.getTargetCorrectionDb (second);
            for (size_t b = 5; b < 24; ++b) expectWithinAbsoluteError (first[b], second[b], 0.05f);
        }

        void testReleaseRouting()
        {
            beginTest ("codec reserve actually lowers delivered peak level without adding latency");
            ChainParams p;
            p.limit.gainDb = 12.0f;
            CeladonChain normal, reserved;
            prepareMasteringChain (normal, p);
            auto safe = p; safe.mastering.codecMarginDb = 1.0f;
            prepareMasteringChain (reserved, safe);
            const auto input = makeSine (2, masteringTestRate, 1000.0, 0.6f, 2.0);
            const auto standard = renderMasteringChain (normal, p, input);
            const auto quieter = renderMasteringChain (reserved, safe, input);
            const auto change = 20.0 * std::log10 (toneAmplitude (quieter, 1000.0, 48000, 48000) / toneAmplitude (standard, 1000.0, 48000, 48000));
            expect (change < -0.8 && change > -1.2, juce::String (change));
            expectEquals (normal.getLatencySamples(), reserved.getLatencySamples());

            beginTest ("host bypass, original and difference audition never receive dither");
            p = {}; safe = p; safe.mastering.dither = DitherMode::bits16;
            for (int monitor = 0; monitor < 3; ++monitor)
            {
                CeladonChain plain, dithered;
                prepareMasteringChain (plain, p); prepareMasteringChain (dithered, safe);
                expect (identicalSignals (renderMasteringChain (plain, p, input, monitor == 0, monitor == 1, monitor == 2),
                                          renderMasteringChain (dithered, safe, input, monitor == 0, monitor == 1, monitor == 2)));
            }

            beginTest ("offline transition removes preview from its first sample and preserves the master meter");
            CeladonChain master, preview;
            prepareMasteringChain (master, p); prepareMasteringChain (preview, p);
            preview.setReleasePreviewEnabled (true);
            auto hot = makeSine (2, masteringTestRate, 1000.0, 0.6f, 3.0);
            renderMasteringChain (master, p, hot); renderMasteringChain (preview, p, hot);
            expect (preview.getMasteringStatus().previewGainDb < -1.0f);
            expectWithinAbsoluteError (preview.getOutputIntegratedLufs(), master.getOutputIntegratedLufs(), 0.0001f);
            glaze::HostInfo offline; offline.offline = true;
            preview.setHostInfo (offline); master.setHostInfo (offline);
            expect (identicalSignals (renderMasteringChain (master, p, input), renderMasteringChain (preview, p, input)));
            expect (! preview.isReleasePreviewEnabled());
        }

        void testMonitoringHandover()
        {
            beginTest ("Listen to preview hands over one gain without a loudness overshoot");
            ChainParams params;
            params.limit.gainDb = 12.0f;
            params.limit.listen = true;
            params.mastering.previewTargetLufs = -14.0f;
            CeladonChain chain;
            prepareMasteringChain (chain, params);
            const auto input = makeSine (2, masteringTestRate, 1000.0, 0.10f, 3.0, 0.3);
            const auto listened = renderMasteringChain (chain, params, input);
            const auto before = toneAmplitude (listened, 1000.0, 2 * 48000, 48000);
            chain.setReleasePreviewEnabled (true);
            const auto previewed = renderMasteringChain (chain, params, input);
            const auto after = toneAmplitude (previewed, 1000.0, 48000, 48000);
            double peak = 0.0;
            for (int i = 0; i < 2400; ++i) peak = std::max (peak, std::abs ((double) previewed[0][(size_t) i]));
            expect (peak <= std::max (before, after) * 1.03, "transition cannot exceed both steady endpoints");
            chain.setReleasePreviewEnabled (false);
            const auto back = renderMasteringChain (chain, params, input);
            peak = 0.0;
            for (int i = 0; i < 2400; ++i) peak = std::max (peak, std::abs ((double) back[0][(size_t) i]));
            expect (peak <= std::max (before, after) * 1.03);

            beginTest ("active advanced chain processing and state reads allocate no memory");
            params.mastering.harshMode = HarshMode::manual;
            params.mastering.targetMode = TargetMode::warm;
            params.mastering.dither = DitherMode::bits24;
            juce::AudioBuffer<float> block (2, 512);
            block.clear();
            const auto allocations = allocationCountOnThisThread();
            for (int i = 0; i < 100; ++i)
            {
                chain.setParams (params, false);
                chain.process (block);
                juce::ignoreUnused (chain.getMasteringStatus());
            }
            expectEquals ((juce::int64) (allocationCountOnThisThread() - allocations), (juce::int64) 0);
        }
    };
    static AdvancedMasteringChainTests advancedMasteringChainTests;
}
