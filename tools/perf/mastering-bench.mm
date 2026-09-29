// Standalone macOS callback benchmark. Link this file against the actual Release
// Celadon shared-code library and the same JUCE definitions/frameworks as that build.
// No editor, UiSettings object, audio device, project state, or filesystem output.
//
//   mastering-bench [--repeat 3] [--rate 48000|96000|192000] [--block 32|64|512]
//
// CSV goes to stdout; configuration/validation go to stderr. Every repetition is
// reported, including its worst callback: no best-of-run selection. The baseline
// disables the reference bus; the learning case enables its real stereo input bus.
// Both use identical main audio, GLAZE 45, and the new default automatic settings.
// Warm-up and measurement round UP to complete host blocks (at least 1 s / 8 s).
// This is an unpaced throughput/tail-latency benchmark, not a DAW underrun test.

#include <JuceHeader.h>
#include "../../Source/PluginProcessor.h"
#include "../../Source/ParameterIds.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/qos.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr double warmSeconds = 1.0, measuredSeconds = 8.0;
    constexpr std::array<int, 3> supportedRates { 48000, 96000, 192000 };
    constexpr std::array<int, 3> supportedBlocks { 32, 64, 512 };
    double microsecondsPerTick = 0.0;

    struct Options
    {
        int repeats = 3, onlyRate = 0, onlyBlock = 0;
    };

    int positiveInteger (const char* text)
    {
        char* end = nullptr;
        const auto value = std::strtol (text, &end, 10);
        if (text == end || *end != '\0' || value < 1 || value > 1000000)
            throw std::runtime_error ("Expected a positive integer");
        return static_cast<int> (value);
    }

    Options parseOptions (int argc, char** argv)
    {
        Options result;
        for (int i = 1; i < argc; ++i)
        {
            const std::string flag (argv[i]);
            if (flag == "--help")
            {
                std::fprintf (stdout, "usage: %s [--repeat 3] [--rate 48000|96000|192000] [--block 32|64|512]\n", argv[0]);
                std::exit (0);
            }
            if (i + 1 >= argc)
                throw std::runtime_error ("Missing option value: " + flag);
            const auto value = positiveInteger (argv[++i]);
            if (flag == "--repeat") result.repeats = value;
            else if (flag == "--rate") result.onlyRate = value;
            else if (flag == "--block") result.onlyBlock = value;
            else throw std::runtime_error ("Unknown option: " + flag);
        }
        if (result.repeats > 20)
            throw std::runtime_error ("--repeat must be in 1..20");
        if (result.onlyRate != 0 && std::find (supportedRates.begin(), supportedRates.end(), result.onlyRate) == supportedRates.end())
            throw std::runtime_error ("--rate must be 48000, 96000, or 192000");
        if (result.onlyBlock != 0 && std::find (supportedBlocks.begin(), supportedBlocks.end(), result.onlyBlock) == supportedBlocks.end())
            throw std::runtime_error ("--block must be 32, 64, or 512");
        return result;
    }

    // A real playing timeline prevents transport-dependent GLAZE work from being
    // accidentally omitted. Only the benchmark thread changes this position.
    class PlayHead final : public juce::AudioPlayHead
    {
    public:
        explicit PlayHead (double rate) : sampleRate (rate) {}
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            info.setTimeInSamples (samplePosition);
            info.setTimeInSeconds (static_cast<double> (samplePosition) / sampleRate);
            info.setBpm (120.0);
            info.setPpqPosition (2.0 * static_cast<double> (samplePosition) / sampleRate);
            return info;
        }
        std::int64_t samplePosition = 0;
    private:
        double sampleRate;
    };

    juce::AudioBuffer<float> makeMusic (int rate, int samples, bool reference)
    {
        juce::AudioBuffer<float> audio (2, samples);
        std::uint32_t random = reference ? 0x9b057a31u : 0x61ad0045u;
        const auto noise = [&random]() noexcept
        {
            random = random * 1664525u + 1013904223u;
            return static_cast<double> (random >> 8) / 8388608.0 - 1.0;
        };
        const auto beatRate = reference ? 103.0 / 60.0 : 2.0;
        const auto transpose = reference ? 1.189207115 : 1.0;
        const auto twoPi = juce::MathConstants<double>::twoPi;
        double kickPhase = 0.0;
        std::array<double, 2> previousNoise {}, lowNoise {};
        auto* left = audio.getWritePointer (0);
        auto* right = audio.getWritePointer (1);
        for (int i = 0; i < samples; ++i)
        {
            const auto time = static_cast<double> (i) / static_cast<double> (rate);
            const auto beat = time * beatRate;
            const auto phase = beat - std::floor (beat);
            const auto eighth = beat * 2.0 - std::floor (beat * 2.0);
            const auto kickTime = phase / beatRate;
            kickPhase += (45.0 + 95.0 * std::exp (-kickTime * 32.0)) / static_cast<double> (rate);
            const auto kick = 0.48 * std::sin (twoPi * kickPhase) * std::exp (-kickTime * 10.0);
            const auto snareEnvelope = (static_cast<int> (beat) % 2 == 1) ? std::exp (-kickTime * 25.0) : 0.0;
            const double roots[] { 55.0, 43.6535, 65.4064, 48.9994 };
            const auto root = roots[(static_cast<int> (beat) / 4) % 4] * transpose;
            auto bass = 0.0;
            for (int harmonic = 1; harmonic <= 5; ++harmonic)
                bass += 0.10 / static_cast<double> (harmonic) * std::sin (twoPi * root * harmonic * time);
            const auto common = noise();
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto ownNoise = 0.72 * common + 0.28 * noise();
                lowNoise[static_cast<size_t> (channel)] += 0.055 * (ownNoise - lowNoise[static_cast<size_t> (channel)]);
                const auto hat = 0.09 * (ownNoise - previousNoise[static_cast<size_t> (channel)])
                               * std::exp (-eighth * 22.0);
                previousNoise[static_cast<size_t> (channel)] = ownNoise;
                auto chord = 0.0;
                for (const auto interval : { 4.0, 4.7568, 5.9932, 8.0 })
                    chord += 0.023 * std::sin (twoPi * root * interval * (channel == 0 ? 1.0 : 1.002) * time + 0.4 * channel);
                const auto body = kick + bass + chord + hat + 0.18 * ownNoise * snareEnvelope
                                + 0.09 * lowNoise[static_cast<size_t> (channel)] + 0.015 * ownNoise;
                (channel == 0 ? left : right)[i] = static_cast<float> (juce::jlimit (-0.95, 0.95, body * (reference ? 0.67 : 0.52)));
            }
        }
        return audio;
    }

    void setParameter (CeladonAudioProcessor& processor, const char* id, float value)
    {
        auto* parameter = processor.getParameters().getParameter (id);
        if (parameter == nullptr)
            throw std::runtime_error (std::string ("Missing Release parameter: ") + id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    const char* referenceStateName (celadon::ReferenceState state)
    {
        switch (state)
        {
            case celadon::ReferenceState::idle: return "idle";
            case celadon::ReferenceState::waitingForSidechain: return "waiting";
            case celadon::ReferenceState::learning: return "learning";
            case celadon::ReferenceState::ready: return "ready";
        }
        return "unknown";
    }

    struct Result
    {
        std::vector<std::uint64_t> ticks; // Entire callback population, never sampled or downselected.
        size_t peakBlock = 0;
        celadon::MasteringStatus peakMastering, finalMastering;
        celadon::glaze::Status peakGlaze;
        float outputTruePeakDb = -100.0f;
        double latencyMs = 0.0;
        int nonFiniteBlocks = 0, warmBlocks = 0;
    };

    Result runCase (int rate, int blockSize, bool learning,
                    const juce::AudioBuffer<float>& mainMusic, const juce::AudioBuffer<float>& referenceMusic)
    {
        PlayHead playHead (rate);
        CeladonAudioProcessor processor;
        auto layout = processor.getBusesLayout();
        if (layout.inputBuses.size() != 2 || layout.outputBuses.size() != 1)
            throw std::runtime_error ("Expected main + optional reference inputs and one main output");
        layout.inputBuses.set (0, juce::AudioChannelSet::stereo());
        layout.inputBuses.set (1, learning ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
        layout.outputBuses.set (0, juce::AudioChannelSet::stereo());
        if (! processor.setBusesLayout (layout))
            throw std::runtime_error ("Processor rejected benchmark bus layout");
        using namespace celadon::param;
        setParameter (processor, glazeAmount, 45.0f);
        setParameter (processor, glazeStyle, 0.0f);
        setParameter (processor, masterHarshMode, 0.0f);
        setParameter (processor, masterHarshAmount, 50.0f);
        setParameter (processor, masterTransient, 50.0f);
        setParameter (processor, masterTargetMode, 1.0f);
        setParameter (processor, masterTargetAmount, 35.0f);
        setParameter (processor, masterCodecMarginDb, 0.0f);
        setParameter (processor, masterDither, 0.0f);
        processor.setPlayHead (&playHead);
        processor.setNonRealtime (false);
        processor.setRateAndBufferSizeDetails (rate, blockSize);
        processor.prepareToPlay (rate, blockSize);

        Result result;
        result.warmBlocks = static_cast<int> (std::ceil (warmSeconds * rate / blockSize));
        result.ticks.resize (static_cast<size_t> (std::ceil (measuredSeconds * rate / blockSize)));
        juce::AudioBuffer<float> buffer (learning ? 4 : 2, blockSize);
        juce::MidiBuffer midi;
        const auto fill = [&]
        {
            const auto offset = static_cast<int> (playHead.samplePosition);
            for (int channel = 0; channel < 2; ++channel)
            {
                buffer.copyFrom (channel, 0, mainMusic, channel, offset, blockSize);
                if (learning)
                    buffer.copyFrom (channel + 2, 0, referenceMusic, channel, offset, blockSize);
            }
        };
        for (int block = 0; block < result.warmBlocks; ++block)
        {
            fill();
            processor.processBlock (buffer, midi);
            playHead.samplePosition += blockSize;
        }
        processor.getAndResetOutputTruePeakDb(); // Warm-up peaks are excluded.
        if (learning)
            processor.requestReferenceLearn();

        std::uint64_t maximumTicks = 0;
        for (size_t block = 0; block < result.ticks.size(); ++block)
        {
            fill();
            const auto start = mach_absolute_time();
            processor.processBlock (buffer, midi);
            const auto elapsed = mach_absolute_time() - start;
            result.ticks[block] = elapsed;
            // Peak-state reads, input copies, timeline updates, collection, and
            // all statistics are OUTSIDE the processBlock timing interval.
            if (elapsed > maximumTicks || block == 0)
            {
                maximumTicks = elapsed;
                result.peakBlock = block;
                result.peakMastering = processor.getMasteringStatus();
                result.peakGlaze = processor.getGlazeStatus();
            }
            playHead.samplePosition += blockSize;
        }
        result.finalMastering = processor.getMasteringStatus();
        result.outputTruePeakDb = processor.getAndResetOutputTruePeakDb();
        result.latencyMs = processor.getLatencyMilliseconds();
        result.nonFiniteBlocks = processor.getNonFiniteBlockCount();
        processor.releaseResources();
        processor.setPlayHead (nullptr);
        if (learning && (result.finalMastering.referenceState != celadon::ReferenceState::learning
                         || ! result.finalMastering.sidechainConnected || result.finalMastering.hasReference
                         || result.finalMastering.referenceSeconds < 7.0 || result.finalMastering.referenceSeconds >= 10.0))
            throw std::runtime_error ("Learning case did not remain actively learning throughout its eight-second measurement");
        if (! learning && (result.finalMastering.referenceState != celadon::ReferenceState::idle
                          || result.finalMastering.sidechainConnected || result.finalMastering.hasReference))
            throw std::runtime_error ("Baseline unexpectedly acquired a reference/sidechain");
        if (result.nonFiniteBlocks != 0)
            throw std::runtime_error ("Processor reported non-finite audio during benchmark");
        return result;
    }

    void report (const Result& result, int rate, int blockSize, bool learning, int repeat)
    {
        auto sorted = result.ticks;
        std::sort (sorted.begin(), sorted.end());
        const auto percentile = [&sorted] (double probability)
        {
            // Nearest-rank quantile: every callback contributes equally.
            const auto rank = static_cast<size_t> (std::ceil (probability * static_cast<double> (sorted.size())));
            return static_cast<double> (sorted[std::max (size_t { 1 }, rank) - 1]) * microsecondsPerTick;
        };
        const auto mean = std::accumulate (result.ticks.begin(), result.ticks.end(), 0.0)
                        / static_cast<double> (result.ticks.size()) * microsecondsPerTick;
        const auto p99 = percentile (0.99), p999 = percentile (0.999);
        const auto maximum = static_cast<double> (sorted.back()) * microsecondsPerTick;
        const auto deadline = 1.0e6 * blockSize / rate;
        const auto overBudget = std::count_if (result.ticks.begin(), result.ticks.end(), [deadline] (std::uint64_t ticks)
        {
            return static_cast<double> (ticks) * microsecondsPerTick > deadline;
        });
        std::printf ("%d,%d,%s,%d,%zu,%.6f,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%lld,%zu,%.6f,%s,%.6f,%d,%d,%d,%s,%.6f,%.3f,%.3f,%d\n",
                     rate, blockSize, learning ? "reference-learning" : "main-only", repeat, result.ticks.size(),
                     static_cast<double> (result.warmBlocks * blockSize) / rate,
                     static_cast<double> (result.ticks.size() * static_cast<size_t> (blockSize)) / rate,
                     deadline, mean, p99, p999, maximum,
                     100.0 * mean / deadline, 100.0 * p99 / deadline, 100.0 * p999 / deadline, 100.0 * maximum / deadline,
                     static_cast<long long> (overBudget), result.peakBlock,
                     static_cast<double> (result.peakBlock * static_cast<size_t> (blockSize)) / rate,
                     referenceStateName (result.peakMastering.referenceState), result.peakMastering.referenceSeconds,
                     static_cast<int> (result.peakGlaze.phase), result.peakGlaze.profileReady ? 1 : 0,
                     result.peakGlaze.planReady ? 1 : 0, referenceStateName (result.finalMastering.referenceState),
                     result.finalMastering.referenceSeconds, result.outputTruePeakDb, result.latencyMs, result.nonFiniteBlocks);
        std::fflush (stdout);
    }
}

int main (int argc, char** argv)
{
    try
    {
        const auto options = parseOptions (argc, argv);
        mach_timebase_info_data_t timebase {};
        if (mach_timebase_info (&timebase) != KERN_SUCCESS || timebase.denom == 0)
            throw std::runtime_error ("Unable to initialise mach_absolute_time timebase");
        microsecondsPerTick = static_cast<double> (timebase.numer) / static_cast<double> (timebase.denom) / 1000.0;
        const auto qosResult = pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
        if (qosResult != 0)
            throw std::runtime_error ("Unable to set USER_INTERACTIVE QoS: " + std::to_string (qosResult));
        // JUCE's Processor starts a timer; initialise its message infrastructure,
        // but never create an editor or instantiate/read/write UiSettings.
        juce::ScopedJuceInitialiser_GUI initialiseJuce;
        std::fprintf (stderr, "mastering-bench: Release library callback timing; QoS=USER_INTERACTIVE; repeats=%d; warm>=1s; measure>=8s; GLAZE=45; harsh=Auto/50; target=Auto/35; dither=Off\n", options.repeats);
        std::fprintf (stderr, "Peak GLAZE phase: 0=unlearned, 1=listening, 2=planTransition, 3=calibrating, 4=locked. Scenario order alternates by repeat. No realtime pacing or scheduling guarantee.\n");
        std::puts ("rate_hz,block_size,scenario,repeat,callbacks,warm_seconds,measured_seconds,deadline_us,mean_us,p99_us,p999_us,max_us,mean_deadline_pct,p99_deadline_pct,p999_deadline_pct,max_deadline_pct,over_budget_callbacks,max_block_index,max_block_start_seconds,max_reference_state,max_reference_seconds,max_glaze_phase,max_profile_ready,max_plan_ready,final_reference_state,final_reference_seconds,output_true_peak_db,latency_ms,nonfinite_blocks");
        for (const auto rate : supportedRates)
        {
            if (options.onlyRate != 0 && options.onlyRate != rate) continue;
            // Pre-generated once per rate, before either scenario and all timers.
            // Two spare maximum blocks cover both rounded-up interval lengths.
            const auto samples = static_cast<int> ((warmSeconds + measuredSeconds) * rate) + 2 * supportedBlocks.back();
            const auto mainMusic = makeMusic (rate, samples, false);
            const auto referenceMusic = makeMusic (rate, samples, true);
            if (std::memcmp (mainMusic.getReadPointer (0), referenceMusic.getReadPointer (0),
                             static_cast<size_t> (samples) * sizeof (float)) == 0)
                throw std::runtime_error ("Main and reference fixtures must differ");
            for (const auto blockSize : supportedBlocks)
            {
                if (options.onlyBlock != 0 && options.onlyBlock != blockSize) continue;
                for (int repeat = 1; repeat <= options.repeats; ++repeat)
                    for (int scenario = 0; scenario < 2; ++scenario)
                    {
                        const auto learning = ((scenario + repeat - 1) % 2) != 0;
                        const auto result = runCase (rate, blockSize, learning, mainMusic, referenceMusic);
                        report (result, rate, blockSize, learning, repeat);
                    }
            }
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf (stderr, "mastering-bench: %s\n", error.what());
        return 1;
    }
}
