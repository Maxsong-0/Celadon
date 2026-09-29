#include "ReferenceSpectrum.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace celadon
{
    namespace
    {
        constexpr double silencePower = 1.0e-8; // -80 dBFS stereo mean-square
        constexpr float relativeFloorDb = -120.0f;
        constexpr int snapshotReadAttempts = 8;

        bool readNumber (const juce::XmlElement& xml, const char* attribute, double& result)
        {
            if (! xml.hasAttribute (attribute))
                return false;
            const auto text = xml.getStringAttribute (attribute).trim();
            const auto* start = text.toRawUTF8();
            char* end = nullptr;
            errno = 0;
            result = std::strtod (start, &end);
            return start != end && *end == '\0' && errno != ERANGE && std::isfinite (result);
        }
    }

    ReferenceSpectrum::Publication::Publication() noexcept
    {
        static_assert (std::is_trivially_copyable_v<Payload>);
        static_assert (std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
        for (auto& word : words)
            word.store (0);
    }

    void ReferenceSpectrum::Publication::write (const Payload& payload) noexcept
    {
        std::array<std::uint32_t, wordCount> raw {};
        std::memcpy (raw.data(), &payload, sizeof (payload));
        // Sequential consistency also orders the per-word reads against the
        // sequence markers; the reader never relies on racing ordinary memory.
        const auto before = sequence.load();
        sequence.store (before + 1);
        for (size_t i = 0; i < wordCount; ++i)
            words[i].store (raw[i]);
        sequence.store (before + 2);
    }

    bool ReferenceSpectrum::Publication::read (Payload& result) const noexcept
    {
        std::array<std::uint32_t, wordCount> raw {};
        for (int attempt = 0; attempt < snapshotReadAttempts; ++attempt)
        {
            const auto before = sequence.load();
            if ((before & 1u) != 0)
                continue;
            for (size_t i = 0; i < wordCount; ++i)
                raw[i] = words[i].load();
            if (sequence.load() == before)
            {
                std::memcpy (static_cast<void*> (&result), raw.data(), sizeof (result));
                return true;
            }
        }
        // No unbounded spin on audio and no mixed-generation curve is exposed.
        return false;
    }

    void ReferenceSpectrum::advanceTicket (std::atomic<std::uint64_t>& destination,
                                            std::uint64_t ticket) noexcept
    {
        auto existing = destination.load();
        while (existing < ticket && ! destination.compare_exchange_weak (existing, ticket)) {}
    }

    void ReferenceSpectrum::requestLearn() noexcept
    {
        advanceTicket (learnTicket, nextTicket.fetch_add (1) + 1);
    }

    void ReferenceSpectrum::cancelLearn() noexcept
    {
        advanceTicket (cancelTicket, nextTicket.fetch_add (1) + 1);
    }

    void ReferenceSpectrum::clear() noexcept
    {
        advanceTicket (clearTicket, nextTicket.fetch_add (1) + 1);
    }

    void ReferenceSpectrum::resetLearning() noexcept
    {
        frameFill = 0;
        analysisPending = false;
        captureInterrupted = false;
        requireFullFinalFrame = false;
        validSamples = 0;
        accumulatedBands.fill (0.0);
        accumulatedTotal = 0.0;
    }

    void ReferenceSpectrum::applyCommands() noexcept
    {
        Payload restore {};
        const auto gotRestore = restored.read (restore);
        const auto clearAt = clearTicket.load();
        const auto learnAt = learnTicket.load();
        const auto cancelAt = cancelTicket.load();
        const auto restoreAt = gotRestore ? restore.referenceTicket : std::uint64_t { 0 };

        if (restoreAt > current.referenceTicket && restoreAt > clearAt)
        {
            current.snapshot.hasReference = restore.snapshot.hasReference;
            current.snapshot.relativeDb = restore.snapshot.relativeDb;
            current.referenceTicket = restoreAt;
        }
        if (clearAt > current.referenceTicket)
        {
            current.snapshot.hasReference = false;
            current.snapshot.relativeDb.fill (0.0f);
            current.referenceTicket = clearAt;
        }

        const auto newest = std::max ({ learnAt, cancelAt, clearAt, restoreAt });
        if (newest > current.controlTicket)
        {
            resetLearning();
            learning = newest == learnAt;
            current.controlTicket = newest;
        }
    }

    void ReferenceSpectrum::publish() noexcept
    {
        auto& snapshot = current.snapshot;
        snapshot.state = learning ? (snapshot.sidechainConnected ? State::learning : State::waitingForSidechain)
                                  : (snapshot.hasReference ? State::ready : State::idle);
        snapshot.seconds = learning ? (double) validSamples / sampleRate
                                    : (snapshot.hasReference ? learningSeconds : 0.0);
        published.write (current);
    }

    void ReferenceSpectrum::prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = std::isfinite (spec.sampleRate) && spec.sampleRate >= 8000.0
                        && spec.sampleRate <= 768000.0 ? spec.sampleRate : 48000.0;
        int order = 13;
        while (order < 17 && (double) (1 << order) < sampleRate / 6.0)
            ++order;
        fftSize = 1 << order;
        targetSamples = (std::int64_t) std::ceil (sampleRate * learningSeconds);
        window.resize ((size_t) fftSize);
        bitReversal.resize ((size_t) fftSize);
        twiddleReal.resize ((size_t) fftSize / 2);
        twiddleImag.resize ((size_t) fftSize / 2);
        power.resize ((size_t) fftSize / 2 + 1);
        for (auto& channel : input)
            channel.assign ((size_t) fftSize * 2, 0.0f);
        for (auto& channel : analysisInput)
            channel.assign ((size_t) fftSize * 2, 0.0f);
        for (auto& channel : transform)
            channel.assign ((size_t) fftSize * 2, 0.0f);
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi
                                                              * (double) i / (double) fftSize));
            int reversed = 0, bits = i;
            for (int bit = 0; bit < order; ++bit)
            {
                reversed = (reversed << 1) | (bits & 1);
                bits >>= 1;
            }
            bitReversal[(size_t) i] = reversed;
            if (i < fftSize / 2)
            {
                const auto angle = -juce::MathConstants<double>::twoPi * (double) i / (double) fftSize;
                twiddleReal[(size_t) i] = (float) std::cos (angle);
                twiddleImag[(size_t) i] = (float) std::sin (angle);
            }
        }

        weights.clear();
        const auto binHz = sampleRate / (double) fftSize;
        const auto halfBand = std::pow (2.0, 1.0 / 6.0);
        for (int b = 0; b < numBands; ++b)
        {
            // Same exact third-octave edges as BalanceModule (whose public
            // centre getter returns the conventional rounded display labels).
            const auto centre = 1000.0 * std::pow (2.0, (double) (b - 14) / 3.0);
            const auto low = centre / halfBand, high = centre * halfBand;
            auto& band = bands[(size_t) b];
            band.first = std::max (1, (int) std::floor (low / binHz - 0.5) + 1);
            const auto last = std::min (fftSize / 2, (int) std::ceil (high / binHz + 0.5) - 1);
            band.count = std::max (0, last - band.first + 1);
            band.offset = (int) weights.size();
            for (int k = band.first; k <= last; ++k)
                weights.push_back (std::max (0.0, std::min (high, ((double) k + 0.5) * binHz)
                                                   - std::max (low, ((double) k - 0.5) * binHz)) / binHz);
        }
        // End an already-running learning session, but retain a new Learn that
        // has not reached process yet (e.g. the host prepares just after a click).
        applyCommands();
        learning = learning && current.controlTicket > lastAudioControlTicket;
        resetLearning();
        current.snapshot.sidechainConnected = false;
        publish();
    }

    void ReferenceSpectrum::startAnalysisIfReady() noexcept
    {
        if (analysisPending || ! learning) return;
        const auto needed = requireFullFinalFrame ? fftSize : (int) std::min ((std::int64_t) fftSize, targetSamples - validSamples);
        if (needed <= 0) return;
        if (frameFill < needed)
        {
            // A disconnected interval must not splice two separated pieces into
            // one FFT window. A complete terminal prefix, however, is retained.
            if (captureInterrupted) { frameFill = 0; captureInterrupted = false; }
            return;
        }
        input.swap (analysisInput); // vector swaps only: no allocation, copies or locks.
        analysisLength = needed;
        analysisSuffix = frameFill - needed;
        frameFill = analysisSuffix; // Reserve the suffix; window-stage work copies it incrementally below.
        captureInterrupted = false;
        analysisPending = true;
        analysisStage = AnalysisStage::window;
        analysisIndex = 0;
        analysisEnergy = analysisWindowEnergy = analysisTotal = analysisInBand = 0.0;
        analysisBands.fill (0.0);
    }

    void ReferenceSpectrum::finishAnalysis (bool valid) noexcept
    {
        if (valid) validSamples += analysisLength;
        else if (analysisLength < fftSize)
            // A rejected tiny terminal prefix must not repeatedly schedule a
            // whole FFT faster than it can run. Preserve its captured suffix and
            // resume full windows; this may add at most one valid analysis window
            // beyond the ten-second minimum, without dropping reference samples.
            requireFullFinalFrame = true;
        analysisPending = false;
        if (validSamples >= targetSamples) finishLearning();
    }

    void ReferenceSpectrum::advanceAnalysis (int workUnits) noexcept
    {
        // Work is distributed by AUDIO SAMPLES, not callback boundaries. Numeric
        // operation order and committed shape therefore do not depend on block size.
        while (analysisPending && workUnits-- > 0)
        {
            switch (analysisStage)
            {
                case AnalysisStage::window:
                {
                    const auto i = analysisIndex++;
                    if (i < analysisSuffix)
                        for (int ch = 0; ch < 2; ++ch)
                            input[(size_t) ch][(size_t) i] = analysisInput[(size_t) ch][(size_t) (analysisLength + i)];
                    if (i < fftSize)
                    {
                        const auto destination = (size_t) (2 * bitReversal[(size_t) i]);
                        const auto w = (double) window[(size_t) i];
                        if (i < analysisLength) analysisWindowEnergy += w * w;
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            const auto value = i < analysisLength ? analysisInput[(size_t) ch][(size_t) i] : 0.0f;
                            analysisEnergy += (double) value * value;
                            transform[(size_t) ch][destination] = value * window[(size_t) i];
                            transform[(size_t) ch][destination + 1] = 0.0f;
                        }
                    }
                    if (analysisIndex == std::max (fftSize, analysisSuffix))
                    {
                        if (analysisEnergy / (2.0 * analysisLength) < silencePower || analysisWindowEnergy <= 0.0)
                            finishAnalysis (false);
                        else
                        {
                            analysisStage = AnalysisStage::fft;
                            fftWidth = 2; fftGroup = fftJ = 0;
                        }
                    }
                    break;
                }
                case AnalysisStage::fft:
                {
                    const auto half = fftWidth / 2;
                    const auto a = (size_t) (2 * (fftGroup + fftJ));
                    const auto b = (size_t) (2 * (fftGroup + fftJ + half));
                    const auto twiddle = (size_t) (fftJ * (fftSize / fftWidth));
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto& buffer = transform[(size_t) ch];
                        const auto re = buffer[b] * twiddleReal[twiddle] - buffer[b + 1] * twiddleImag[twiddle];
                        const auto im = buffer[b] * twiddleImag[twiddle] + buffer[b + 1] * twiddleReal[twiddle];
                        const auto ar = buffer[a], ai = buffer[a + 1];
                        buffer[a] = ar + re; buffer[a + 1] = ai + im;
                        buffer[b] = ar - re; buffer[b + 1] = ai - im;
                    }
                    if (++fftJ == half)
                    {
                        fftJ = 0;
                        fftGroup += fftWidth;
                        if (fftGroup == fftSize)
                        {
                            fftGroup = 0;
                            fftWidth *= 2;
                            if (fftWidth > fftSize)
                            {
                                analysisStage = AnalysisStage::power;
                                analysisIndex = 1;
                            }
                        }
                    }
                    break;
                }
                case AnalysisStage::power:
                {
                    const auto k = analysisIndex++;
                    double value = 0.0;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const auto& buffer = transform[(size_t) ch];
                        const auto re = (double) buffer[(size_t) (2 * k)], im = (double) buffer[(size_t) (2 * k + 1)];
                        value += (re * re + im * im) * (k == fftSize / 2 ? 0.5 : 1.0);
                    }
                    power[(size_t) k] = value;
                    analysisTotal += value;
                    if (analysisIndex > fftSize / 2)
                    {
                        if (! std::isfinite (analysisTotal) || analysisTotal <= 0.0) finishAnalysis (false);
                        else { analysisStage = AnalysisStage::bands; analysisBand = analysisBandBin = 0; }
                    }
                    break;
                }
                case AnalysisStage::bands:
                {
                    const auto& band = bands[(size_t) analysisBand];
                    if (analysisBandBin < band.count)
                    {
                        analysisBands[(size_t) analysisBand] += power[(size_t) (band.first + analysisBandBin)]
                                                               * weights[(size_t) (band.offset + analysisBandBin)];
                        ++analysisBandBin;
                    }
                    else
                    {
                        analysisInBand += analysisBands[(size_t) analysisBand];
                        analysisBandBin = 0;
                        if (++analysisBand == numBands)
                        {
                            if (analysisInBand / ((double) fftSize * analysisWindowEnergy) < silencePower
                                || analysisInBand / analysisTotal < 1.0e-6) finishAnalysis (false);
                            else { analysisStage = AnalysisStage::accumulate; analysisIndex = 0; }
                        }
                    }
                    break;
                }
                case AnalysisStage::accumulate:
                {
                    const auto weight = (double) analysisLength / analysisWindowEnergy;
                    if (analysisIndex < numBands)
                    {
                        const auto b = (size_t) analysisIndex++;
                        accumulatedBands[b] += analysisBands[b] * weight;
                    }
                    else
                    {
                        accumulatedTotal += analysisTotal * weight;
                        finishAnalysis (true);
                    }
                    break;
                }
            }
        }
    }

    void ReferenceSpectrum::finishLearning() noexcept
    {
        if (accumulatedTotal > 0.0 && std::isfinite (accumulatedTotal))
        {
            for (size_t b = 0; b < accumulatedBands.size(); ++b)
                current.snapshot.relativeDb[b] = (float) juce::jlimit ((double) relativeFloorDb, 0.0,
                    10.0 * std::log10 (std::max (1.0e-12, accumulatedBands[b] / accumulatedTotal)));
            current.snapshot.hasReference = true;
            current.referenceTicket = current.controlTicket;
        }
        learning = false;
    }

    void ReferenceSpectrum::process (const juce::AudioBuffer<float>& sidechain, bool connected) noexcept
    {
        applyCommands();
        lastAudioControlTicket = current.controlTicket;
        current.snapshot.sidechainConnected = connected && sidechain.getNumChannels() > 0;
        if (! learning || fftSize == 0) { publish(); return; }
        if (! current.snapshot.sidechainConnected) captureInterrupted = true;
        const auto* left = current.snapshot.sidechainConnected ? sidechain.getReadPointer (0) : nullptr;
        const auto* right = current.snapshot.sidechainConnected ? sidechain.getReadPointer (std::min (1, sidechain.getNumChannels() - 1)) : nullptr;
        for (int i = 0; i < sidechain.getNumSamples() && learning; ++i)
        {
            startAnalysisIfReady();
            // Two-window capacity also preserves oversampled suffixes when a
            // terminal short prefix is rejected. Transfers run in the same fixed
            // work budget; future samples append after the reserved suffix.
            if (left != nullptr && ! captureInterrupted && frameFill < 2 * fftSize)
            {
                const auto sanitize = [] (float value) noexcept
                {
                    return std::isfinite (value) && std::abs (value) <= 1.0e6f ? value : 0.0f;
                };
                input[0][(size_t) frameFill] = sanitize (left[i]);
                input[1][(size_t) frameFill] = sanitize (right[i]);
                ++frameFill;
            }
            startAnalysisIfReady();
            advanceAnalysis (16);
            startAnalysisIfReady();
        }
        publish();
    }

    ReferenceSpectrum::Snapshot ReferenceSpectrum::getSnapshot() const noexcept
    {
        Snapshot snapshot {};
        readSnapshot (snapshot);
        return snapshot;
    }

    bool ReferenceSpectrum::readSnapshot (Snapshot& snapshot) const noexcept
    {
        Payload result {}, restore {};
        if (! published.read (result))
            return false; // A coherent safe fallback, never a torn committed curve.
        // A concurrent XML writer may still be publishing the next reference.
        // Keep the coherent committed curve from published until that restore
        // is readable; a transient empty snapshot would turn target EQ off.
        const auto restoreAt = restored.read (restore) ? restore.referenceTicket : std::uint64_t { 0 };
        const auto clearAt = clearTicket.load(), learnAt = learnTicket.load(), cancelAt = cancelTicket.load();
        if (restoreAt > result.referenceTicket && restoreAt > clearAt)
        {
            result.snapshot.hasReference = restore.snapshot.hasReference;
            result.snapshot.relativeDb = restore.snapshot.relativeDb;
            result.referenceTicket = restoreAt;
        }
        if (clearAt > result.referenceTicket)
        {
            result.snapshot.hasReference = false;
            result.snapshot.relativeDb.fill (0.0f);
        }
        const auto newest = std::max ({ learnAt, cancelAt, clearAt, restoreAt });
        if (newest > result.controlTicket)
        {
            const bool pendingLearn = newest == learnAt;
            result.snapshot.state = pendingLearn
                ? (result.snapshot.sidechainConnected ? State::learning : State::waitingForSidechain)
                : (result.snapshot.hasReference ? State::ready : State::idle);
            result.snapshot.seconds = pendingLearn ? 0.0 : (result.snapshot.hasReference ? learningSeconds : 0.0);
        }
        snapshot = result.snapshot;
        return true;
    }

    std::unique_ptr<juce::XmlElement> ReferenceSpectrum::createStateXml() const
    {
        // Saving already allocates XML and is a non-audio operation. Unlike the
        // bounded realtime getter, retry contention rather than persist its
        // deliberately conservative no-reference fallback into the project.
        Snapshot snapshot {};
        while (! readSnapshot (snapshot))
            std::this_thread::yield();
        auto xml = std::make_unique<juce::XmlElement> ("REFERENCE_SPECTRUM");
        xml->setAttribute ("format", 1);
        xml->setAttribute ("seconds", snapshot.hasReference ? learningSeconds : 0.0);
        if (snapshot.hasReference)
            for (int b = 0; b < numBands; ++b)
            {
                auto* band = xml->createNewChildElement ("BAND");
                band->setAttribute ("index", b);
                band->setAttribute ("relativeDb", (double) snapshot.relativeDb[(size_t) b]);
            }
        return xml;
    }

    void ReferenceSpectrum::restoreStateXml (const juce::XmlElement* xml)
    {
        Payload payload {};
        double seconds = 0.0;
        bool valid = xml != nullptr && xml->hasTagName ("REFERENCE_SPECTRUM")
            && xml->getStringAttribute ("format") == "1" && readNumber (*xml, "seconds", seconds)
            && seconds == learningSeconds && xml->getNumChildElements() == numBands;
        std::array<bool, numBands> seen {};
        double relativePower = 0.0;
        if (valid)
            for (const auto* band : xml->getChildIterator())
            {
                double index = 0.0, db = 0.0;
                if (! band->hasTagName ("BAND") || ! readNumber (*band, "index", index)
                    || index < 0.0 || index >= numBands || index != std::floor (index)
                    || ! readNumber (*band, "relativeDb", db) || db < relativeFloorDb || db > 0.0
                    || seen[(size_t) index] || band->getNumChildElements() != 0)
                {
                    valid = false;
                    break;
                }
                seen[(size_t) index] = true;
                payload.snapshot.relativeDb[(size_t) index] = (float) db;
                relativePower += std::pow (10.0, db / 10.0);
            }
        // The non-overlapping bands cannot contain more than total RMS power.
        // Allow float/XML rounding, but reject an all-floor invented reference.
        valid = valid && relativePower >= 1.0e-6 && relativePower <= 1.0001;
        if (valid)
        {
            payload.snapshot.hasReference = true;
            payload.snapshot.state = State::ready;
            payload.snapshot.seconds = learningSeconds;
        }
        else
            payload = {};
        const std::lock_guard<std::mutex> lock (restoreMutex);
        payload.referenceTicket = payload.controlTicket = nextTicket.fetch_add (1) + 1;
        restored.write (payload);
    }
}
