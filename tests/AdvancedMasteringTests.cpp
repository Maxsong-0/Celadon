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
    }

    class AdvancedMasteringChainTests : public juce::UnitTest
    {
    public:
        AdvancedMasteringChainTests() : juce::UnitTest ("AdvancedMasteringChain", "Celadon") {}
        void runTest() override
        {
            testNeutral();
            testHarshness();
            testTargets();
            testReleaseRouting();
            testMonitoringHandover();
        }
    private:
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
