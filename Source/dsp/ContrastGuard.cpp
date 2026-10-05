#include "ContrastGuard.h"

#include <algorithm>
#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        float percentile (const float* sorted, int count, float p) noexcept
        {
            const auto pos = p * (float) (count - 1);
            const auto i0 = (int) pos;
            const auto i1 = juce::jmin (count - 1, i0 + 1);
            const auto fraction = pos - (float) i0;
            return sorted[i0] + (sorted[i1] - sorted[i0]) * fraction;
        }
    }

    void ContrastGuard::prepare (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0 && std::isfinite (newSampleRate))
            sampleRate = newSampleRate;

        pushLength = juce::jmax (1, (int) std::lround (sampleRate * 0.1));
        clear();
    }

    void ContrastGuard::clear() noexcept
    {
        reliefDb = 0.0f;
        engaged = false;
        ringCount = 0;
        ringWrite = 0;
        samplesUntilPush = pushLength;
        inputRing.fill (0.0f);
        outputRing.fill (0.0f);
    }

    void ContrastGuard::setFloorDb (float newFloorDb) noexcept
    {
        floorDb = std::isfinite (newFloorDb) ? juce::jmin (0.0f, newFloorDb) : -3.0f;
        reliefDb = juce::jmax (floorDb, reliefDb);
    }

    void ContrastGuard::snapRelief() noexcept
    {
        reliefDb = 0.0f;
        engaged = false;
    }

    void ContrastGuard::push (float inputLufs, float outputLufs) noexcept
    {
        if (! std::isfinite (inputLufs) || ! std::isfinite (outputLufs))
            return;

        inputRing[(std::size_t) ringWrite] = inputLufs;
        outputRing[(std::size_t) ringWrite] = outputLufs;
        ringWrite = (ringWrite + 1) % ringCapacity;
        ringCount = juce::jmin (ringCapacity, ringCount + 1);
    }

    bool ContrastGuard::spreads (float& inputSpread, float& outputSpread) const noexcept
    {
        if (ringCount < 8)
            return false;

        std::array<float, (std::size_t) ringCapacity> inputSorted {}, outputSorted {};

        for (int i = 0; i < ringCount; ++i)
        {
            const auto index = (ringWrite - ringCount + i + ringCapacity * 2) % ringCapacity;
            inputSorted[(std::size_t) i] = inputRing[(std::size_t) index];
            outputSorted[(std::size_t) i] = outputRing[(std::size_t) index];
        }

        std::sort (inputSorted.begin(), inputSorted.begin() + ringCount);
        std::sort (outputSorted.begin(), outputSorted.begin() + ringCount);

        inputSpread = percentile (inputSorted.data(), ringCount, 0.90f) - percentile (inputSorted.data(), ringCount, 0.25f);
        outputSpread = percentile (outputSorted.data(), ringCount, 0.90f) - percentile (outputSorted.data(), ringCount, 0.25f);
        return std::isfinite (inputSpread) && std::isfinite (outputSpread);
    }

    GuardOutput ContrastGuard::process (float inputShortTermLufs, float outputShortTermLufs,
                                         bool musicActive, int numSamples, bool clubDelivery) noexcept
    {
        GuardOutput out;
        out.cause = GuardCause::contrast;

        if (musicActive && numSamples > 0
            && std::isfinite (inputShortTermLufs) && std::isfinite (outputShortTermLufs)
            && inputShortTermLufs > -70.0f && outputShortTermLufs > -70.0f)
        {
            samplesUntilPush -= numSamples;
            auto pushes = 0;

            while (samplesUntilPush <= 0 && pushes < 64)
            {
                push (inputShortTermLufs, outputShortTermLufs);
                samplesUntilPush += pushLength;
                ++pushes;
            }

            if (samplesUntilPush <= 0)
                samplesUntilPush = pushLength;

            float inputSpread = 0.0f, outputSpread = 0.0f;

            if (spreads (inputSpread, outputSpread))
            {
                const auto excess = inputSpread - outputSpread;
                const auto threshold = clubDelivery ? clubSpreadLu : standardSpreadLu;
                const auto dt = (float) ((double) numSamples / sampleRate);
                const auto step = retreatDbPerSecond * dt;
                const auto before = reliefDb;

                if (excess > threshold)
                    reliefDb = juce::jmax (floorDb, reliefDb - step);
                else
                    reliefDb = juce::jmin (0.0f, reliefDb + step);

                const auto atFloor = reliefDb <= floorDb;
                engaged = reliefDb < before || (atFloor && excess > threshold);
                out.engaged = engaged;
                out.reliefDb = reliefDb;
                out.cause = reliefDb < 0.0f ? GuardCause::contrast : GuardCause::none;
                return out;
            }
        }

        engaged = false;
        out.engaged = false;
        out.reliefDb = reliefDb;
        out.cause = reliefDb < 0.0f ? GuardCause::contrast : GuardCause::none;
        return out;
    }

    std::unique_ptr<juce::XmlElement> ContrastGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("CONTRAST_GUARD");
        xmlio::Writer writer (*xml);
        writer.field ("reliefDb", reliefDb);
        writer.field ("floorDb", floorDb);
        writer.field ("engaged", engaged);
        writer.field ("ringCount", ringCount);
        writer.field ("ringWrite", ringWrite);
        writer.field ("samplesUntilPush", samplesUntilPush);
        writer.field ("inputRing", inputRing);
        writer.field ("outputRing", outputRing);
        return xml;
    }

    bool ContrastGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "CONTRAST_GUARD")
            return false;

        auto restored = *this;
        xmlio::Reader reader (xml);
        reader.field ("reliefDb", restored.reliefDb);
        reader.field ("floorDb", restored.floorDb);
        reader.field ("engaged", restored.engaged);
        reader.field ("ringCount", restored.ringCount);
        reader.field ("ringWrite", restored.ringWrite);
        reader.field ("samplesUntilPush", restored.samplesUntilPush);
        reader.field ("inputRing", restored.inputRing);
        reader.field ("outputRing", restored.outputRing);

        if (! reader.ok || restored.ringCount < 0 || restored.ringCount > ringCapacity
            || restored.ringWrite < 0 || restored.ringWrite >= ringCapacity)
            return false;

        reliefDb = restored.reliefDb;
        floorDb = restored.floorDb;
        engaged = restored.engaged;
        ringCount = restored.ringCount;
        ringWrite = restored.ringWrite;
        samplesUntilPush = restored.samplesUntilPush;
        inputRing = restored.inputRing;
        outputRing = restored.outputRing;
        return true;
    }
}
