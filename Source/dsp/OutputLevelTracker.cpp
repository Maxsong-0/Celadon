#include "OutputLevelTracker.h"

#include "GlazeConstants.h"

#include <cmath>
#include <cstring>

namespace celadon::glaze
{
    namespace
    {
        float energyToLufs (double energy, std::int64_t samples) noexcept
        {
            if (samples <= 0 || ! (energy > 0.0) || ! std::isfinite (energy))
                return -100.0f;

            const auto meanSquare = energy / (double) samples;
            if (! (meanSquare > 0.0))
                return -100.0f;

            return -0.691f + (float) (10.0 * std::log10 (meanSquare));
        }
    }

    void OutputLevelTracker::prepare (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0 && std::isfinite (newSampleRate))
            sampleRate = newSampleRate;

        LoudnessMeter::designKWeighting (sampleRate, shelf, highpass);
        maxOpenSamples = std::max<std::int64_t> (1, (std::int64_t) std::llround (sampleRate * 8.0));
        clear();
    }

    void OutputLevelTracker::clear() noexcept
    {
        resetFilters();
        open = {};
        closed.fill (Segment {});
        closedCount = 0;
        closedWrite = 0;
        haveOpenMainness = false;
        openWasMain = false;
    }

    void OutputLevelTracker::resetFilters() noexcept
    {
        shelfS1.fill (0.0);
        shelfS2.fill (0.0);
        highpassS1.fill (0.0);
        highpassS2.fill (0.0);
    }

    void OutputLevelTracker::pushClosed (const Segment& segment) noexcept
    {
        if (segment.samples <= 0 || ! (segment.mainness >= mainnessGate))
            return;

        closed[(std::size_t) closedWrite] = segment;
        closedWrite = (closedWrite + 1) % closedCapacity;
        closedCount = juce::jmin (closedCapacity, closedCount + 1);
    }

    void OutputLevelTracker::noteMainness (float mainness) noexcept
    {
        if (! std::isfinite (mainness))
            mainness = 0.0f;

        const auto main = mainness >= mainnessGate;

        if (! haveOpenMainness)
        {
            haveOpenMainness = true;
            openWasMain = main;
            open.mainness = mainness;
            return;
        }

        if (main != openWasMain)
        {
            if (openWasMain)
                pushClosed (open);

            open = {};
            openWasMain = main;
        }

        open.mainness = mainness;
    }

    void OutputLevelTracker::closeSegment() noexcept
    {
        if (openWasMain)
            pushClosed (open);

        open = {};
        haveOpenMainness = false;
        openWasMain = false;
    }

    void OutputLevelTracker::resetChannel (int channel) noexcept
    {
        shelfS1[(std::size_t) channel] = shelfS2[(std::size_t) channel] = 0.0;
        highpassS1[(std::size_t) channel] = highpassS2[(std::size_t) channel] = 0.0;
    }

    void OutputLevelTracker::processOutput (const juce::AudioBuffer<float>& output) noexcept
    {
        if (! haveOpenMainness)
            return;

        const auto channels = juce::jmin (2, output.getNumChannels());
        const auto numSamples = output.getNumSamples();

        if (channels <= 0 || numSamples <= 0)
            return;

        const auto weight = channels == 1 ? 2.0 : 1.0;

        for (int i = 0; i < numSamples; ++i)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto x = (double) output.getReadPointer (channel)[i];

                if (! std::isfinite (x))
                {
                    resetChannel (channel);
                    continue;
                }

                auto y = LoudnessMeter::kWeight (x, shelf, shelfS1[(std::size_t) channel], shelfS2[(std::size_t) channel]);
                y = LoudnessMeter::kWeight (y, highpass, highpassS1[(std::size_t) channel], highpassS2[(std::size_t) channel]);

                if (! std::isfinite (y))
                {
                    resetChannel (channel);
                    continue;
                }

                open.energy += weight * y * y;
            }

            ++open.samples;

            if (open.samples >= maxOpenSamples)
            {
                const auto mainness = open.mainness;
                const auto wasMain = openWasMain;
                if (wasMain)
                    pushClosed (open);

                open = {};
                open.mainness = mainness;
                openWasMain = wasMain;
            }
        }
    }

    bool OutputLevelTracker::accumulate (double& energy, std::int64_t& samples) const noexcept
    {
        energy = 0.0;
        samples = 0;

        const auto stored = juce::jmin (closedCount, closedCapacity);

        for (int age = 0; age < stored; ++age)
        {
            const auto index = (closedWrite - 1 - age + closedCapacity * 2) % closedCapacity;
            const auto& segment = closed[(std::size_t) index];

            if (segment.samples > 0 && segment.mainness >= mainnessGate)
            {
                energy += segment.energy;
                samples += segment.samples;
            }
        }

        if (openWasMain && open.samples > 0 && open.mainness >= mainnessGate)
        {
            energy += open.energy;
            samples += open.samples;
        }

        return samples > 0 && std::isfinite (energy);
    }

    bool OutputLevelTracker::mainLevelValid() const noexcept
    {
        double energy = 0.0;
        std::int64_t samples = 0;
        return accumulate (energy, samples);
    }

    float OutputLevelTracker::mainLevelLufs() const noexcept
    {
        double energy = 0.0;
        std::int64_t samples = 0;

        if (! accumulate (energy, samples))
            return -100.0f;

        return energyToLufs (energy, samples);
    }

    std::unique_ptr<juce::XmlElement> OutputLevelTracker::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("OUTPUT_LEVEL");
        xmlio::Writer writer (*xml);
        writer.field ("openEnergy", open.energy);
        writer.field ("openMainness", open.mainness);
        writer.field ("closedCount", closedCount);
        writer.field ("closedWrite", closedWrite);
        writer.field ("haveOpenMainness", haveOpenMainness);
        writer.field ("openWasMain", openWasMain);
        xml->setAttribute ("openSamples", juce::String (open.samples));

        juce::MemoryBlock packed ((size_t) closedCapacity * sizeof (Segment));
        std::memcpy (packed.getData(), closed.data(), packed.getSize());
        xml->setAttribute ("closed", juce::Base64::toBase64 (packed.getData(), packed.getSize()));
        return xml;
    }

    bool OutputLevelTracker::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "OUTPUT_LEVEL")
            return false;

        Segment restoredOpen;
        auto count = closedCount;
        auto write = closedWrite;
        auto have = haveOpenMainness;
        auto wasMain = openWasMain;
        xmlio::Reader reader (xml);
        reader.field ("openEnergy", restoredOpen.energy);
        reader.field ("openMainness", restoredOpen.mainness);
        reader.field ("closedCount", count);
        reader.field ("closedWrite", write);
        reader.field ("haveOpenMainness", have);
        reader.field ("openWasMain", wasMain);
        restoredOpen.samples = xml.getStringAttribute ("openSamples").getLargeIntValue();

        juce::MemoryOutputStream packed;
        if (! reader.ok || count < 0 || count > closedCapacity || write < 0 || write >= closedCapacity
            || ! juce::Base64::convertFromBase64 (packed, xml.getStringAttribute ("closed"))
            || packed.getDataSize() != (size_t) closedCapacity * sizeof (Segment))
            return false;

        std::memcpy (closed.data(), packed.getData(), packed.getDataSize());
        open = restoredOpen;
        closedCount = count;
        closedWrite = write;
        haveOpenMainness = have;
        openWasMain = wasMain;
        return true;
    }
}
