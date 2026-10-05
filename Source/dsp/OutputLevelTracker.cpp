#include "OutputLevelTracker.h"

#include "GlazeConstants.h"

#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        constexpr float mainGate = 0.5f;
        constexpr const char* tag = "OUTPUT_LEVEL";

        bool finiteMainness (float m) noexcept
        {
            return std::isfinite (m);
        }
    }

    void OutputLevelTracker::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
        LoudnessMeter::designKWeighting (sampleRate, shelf, highpass);
        clear();
    }

    void OutputLevelTracker::clear() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            shelfS1[ch] = shelfS2[ch] = 0.0;
            highpassS1[ch] = highpassS2[ch] = 0.0;
        }

        inMain = false;
        energy = 0.0;
        samples = 0;
        levelLufs = 0.0f;
        levelValid = false;
    }

    void OutputLevelTracker::closeSegment() noexcept
    {
        if (samples > 0 && energy > 0.0)
        {
            const auto mean = energy / (double) samples;
            levelLufs = (float) (-0.691 + 10.0 * std::log10 (mean));
            levelValid = std::isfinite (levelLufs);
        }

        energy = 0.0;
        samples = 0;
    }

    void OutputLevelTracker::noteMainness (float mainness) noexcept
    {
        const auto main = finiteMainness (mainness) && mainness >= mainGate;

        if (inMain && ! main)
            closeSegment();

        if (! inMain && main)
        {
            energy = 0.0;
            samples = 0;
        }

        inMain = main;
    }

    void OutputLevelTracker::processOutput (const juce::AudioBuffer<float>& output) noexcept
    {
        if (! inMain)
            return;

        const auto numSamples = output.getNumSamples();
        const auto numChannels = juce::jmin (2, output.getNumChannels());

        if (numSamples <= 0 || numChannels <= 0)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            auto sum = 0.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto x = (double) output.getSample (ch, i);
                const auto shelved = LoudnessMeter::kWeight (x, shelf, shelfS1[ch], shelfS2[ch]);
                const auto y = LoudnessMeter::kWeight (shelved, highpass, highpassS1[ch], highpassS2[ch]);
                sum += y * y;
            }

            if (numChannels == 1)
                sum += sum;

            if (std::isfinite (sum) && sum >= 0.0)
                energy += sum;
        }

        samples += (std::int64_t) numSamples;

        if (samples > 0 && energy > 0.0)
        {
            const auto mean = energy / (double) samples;
            const auto lufs = (float) (-0.691 + 10.0 * std::log10 (mean));

            if (std::isfinite (lufs))
            {
                levelLufs = lufs;
                levelValid = true;
            }
        }
    }

    float OutputLevelTracker::mainLevelLufs() const noexcept
    {
        return levelValid ? levelLufs : 0.0f;
    }

    std::unique_ptr<juce::XmlElement> OutputLevelTracker::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> (tag);
        xmlio::Writer writer (*xml);
        writer.field ("inMain", inMain);
        writer.field ("energy", energy);
        writer.field ("samples", (int) juce::jmin ((std::int64_t) 0x7fffffff, samples));
        writer.field ("levelLufs", levelLufs);
        writer.field ("levelValid", levelValid);
        writer.field ("shelfS1L", (float) shelfS1[0]);
        writer.field ("shelfS1R", (float) shelfS1[1]);
        writer.field ("shelfS2L", (float) shelfS2[0]);
        writer.field ("shelfS2R", (float) shelfS2[1]);
        writer.field ("highpassS1L", (float) highpassS1[0]);
        writer.field ("highpassS1R", (float) highpassS1[1]);
        writer.field ("highpassS2L", (float) highpassS2[0]);
        writer.field ("highpassS2R", (float) highpassS2[1]);
        return xml;
    }

    bool OutputLevelTracker::restoreStateXml (const juce::XmlElement& xml)
    {
        if (! xml.hasTagName (tag))
            return false;

        xmlio::Reader reader (xml);
        bool main = false;
        double savedEnergy = 0.0;
        int savedSamples = 0;
        float level = 0.0f;
        bool valid = false;
        float s1L = 0.0f, s1R = 0.0f, s2L = 0.0f, s2R = 0.0f;
        float h1L = 0.0f, h1R = 0.0f, h2L = 0.0f, h2R = 0.0f;
        reader.field ("inMain", main);
        reader.field ("energy", savedEnergy);
        reader.field ("samples", savedSamples);
        reader.field ("levelLufs", level);
        reader.field ("levelValid", valid);
        reader.field ("shelfS1L", s1L);
        reader.field ("shelfS1R", s1R);
        reader.field ("shelfS2L", s2L);
        reader.field ("shelfS2R", s2R);
        reader.field ("highpassS1L", h1L);
        reader.field ("highpassS1R", h1R);
        reader.field ("highpassS2L", h2L);
        reader.field ("highpassS2R", h2R);

        if (! reader.ok || ! std::isfinite (savedEnergy) || savedEnergy < 0.0 || savedSamples < 0
            || ! std::isfinite (level))
            return false;

        inMain = main;
        energy = savedEnergy;
        samples = savedSamples;
        levelLufs = level;
        levelValid = valid;
        shelfS1[0] = s1L;
        shelfS1[1] = s1R;
        shelfS2[0] = s2L;
        shelfS2[1] = s2R;
        highpassS1[0] = h1L;
        highpassS1[1] = h1R;
        highpassS2[0] = h2L;
        highpassS2[1] = h2R;
        return true;
    }
}
