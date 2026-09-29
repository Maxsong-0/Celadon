/*
    A sidechain-only, level-independent reference spectrum. The owner must pass
    the actual sidechain bus, never a main-input/output or display-analyser tap.
*/
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace celadon
{
    class ReferenceSpectrum
    {
    public:
        static constexpr int numBands = 27;
        static constexpr double learningSeconds = 10.0;
        enum class State { idle, waitingForSidechain, learning, ready };

        struct Snapshot
        {
            State state = State::idle;
            double seconds = 0.0;
            bool hasReference = false, sidechainConnected = false;
            // 10 log10 (one-third-octave power / total stereo power), [-120, 0].
            // Silence/unsupported bands stay at the floor; callers must gate
            // correction on evidence instead of boosting an absent band.
            std::array<float, numBands> relativeDb {};
        };

        // Like other DSP prepare methods, call only while process is stopped.
        // Preserves a committed reference, but does not resume partial learning.
        // A new Learn request not yet consumed by process survives preparation.
        void prepare (const juce::dsp::ProcessSpec&);
        void process (const juce::AudioBuffer<float>& sidechain, bool connected) noexcept;
        void requestLearn() noexcept;
        void cancelLearn() noexcept;
        void clear() noexcept;
        Snapshot getSnapshot() const noexcept;
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        // Non-audio thread only; parsing may allocate. Concurrent state readers,
        // UI commands and process are safe, including save before the next block.
        void restoreStateXml (const juce::XmlElement*);

    private:
        struct Payload
        {
            Snapshot snapshot {};
            std::uint64_t referenceTicket = 0, controlTicket = 0;
        };

        // Single-writer publications. ALL payload words are atomic; an ordinary
        // seqlock around a non-atomic struct would still be a C++ data race.
        class Publication
        {
        public:
            Publication() noexcept;
            void write (const Payload&) noexcept;
            bool read (Payload&) const noexcept;
        private:
            static constexpr size_t wordCount = (sizeof (Payload) + 3) / 4;
            std::atomic<std::uint64_t> sequence { 0 };
            std::array<std::atomic<std::uint32_t>, wordCount> words;
        };

        static void advanceTicket (std::atomic<std::uint64_t>&, std::uint64_t) noexcept;
        void applyCommands() noexcept;
        void resetLearning() noexcept;
        void publish() noexcept;
        bool readSnapshot (Snapshot&) const noexcept;
        void startAnalysisIfReady() noexcept;
        void advanceAnalysis (int workUnits) noexcept;
        void finishAnalysis (bool valid) noexcept;
        void finishLearning() noexcept;

        // UI controls are independent monotonic tickets: collapsing learn/cancel
        // must never swallow an earlier clear or restore of the committed curve.
        std::atomic<std::uint64_t> nextTicket { 0 }, learnTicket { 0 },
                                   cancelTicket { 0 }, clearTicket { 0 };
        Publication published, restored;
        // Serializes only non-audio XML writers. The audio thread never locks it.
        std::mutex restoreMutex;
        Payload current {};
        std::uint64_t lastAudioControlTicket = 0;
        bool learning = false;
        double sampleRate = 48000.0;
        std::int64_t validSamples = 0, targetSamples = 480000;
        int fftSize = 0, frameFill = 0;
        std::array<std::vector<float>, 2> input, analysisInput, transform;
        // Two capture banks let a completed frame be transformed while collecting
        // the next one. One work unit is a constant-sized operation; 16 units per
        // input sample complete the largest supported FFT before its successor.
        enum class AnalysisStage { window, fft, power, bands, accumulate };
        bool analysisPending = false, captureInterrupted = false, requireFullFinalFrame = false;
        AnalysisStage analysisStage = AnalysisStage::window;
        int analysisLength = 0, analysisSuffix = 0, analysisIndex = 0, fftWidth = 2, fftGroup = 0, fftJ = 0;
        int analysisBand = 0, analysisBandBin = 0;
        double analysisEnergy = 0.0, analysisWindowEnergy = 0.0, analysisTotal = 0.0, analysisInBand = 0.0;
        std::array<double, numBands> analysisBands {};
        std::vector<float> window;
        std::vector<int> bitReversal;
        std::vector<float> twiddleReal, twiddleImag;
        std::vector<double> power;
        struct Band { int first = 1, count = 0, offset = 0; };
        std::array<Band, numBands> bands {};
        std::vector<double> weights;
        std::array<double, numBands> accumulatedBands {};
        double accumulatedTotal = 0.0;
    };
}
