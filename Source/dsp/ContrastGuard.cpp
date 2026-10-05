#include "ContrastGuard.h"

#include "GlazeConstants.h"

#include <algorithm>
#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        constexpr float contrastLimitLu = 1.2f;
        constexpr float contrastLimitClubLu = 1.5f;
        constexpr float contrastRateDbPerSecond = 0.1f;
        constexpr float contrastFloorDb = -3.0f;
        constexpr const char* tag = "CONTRAST_GUARD";

        bool finiteReading (float x) noexcept
        {
            return std::isfinite (x) && x > -70.0f;
        }

        float percentile (float* values, int n, float p) noexcept
        {
            if (n <= 0)
                return 0.0f;

            std::sort (values, values + n);
            const auto index = juce::jlimit (0, n - 1, (int) std::lround ((double) (n - 1) * (double) p));
            return values[index];
        }
    }

    void ContrastGuard::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
        strideSamples = juce::jmax (1, (int) std::llround (sampleStrideSeconds * sampleRate));
        clear();
    }

    void ContrastGuard::snapRelief() noexcept
    {
        reliefDb = 0.0f;
        retreating = false;
    }

    void ContrastGuard::clear() noexcept
    {
        reliefDb = 0.0f;
        retreating = false;
        writeIndex = 0;
        count = 0;
        samplesUntilSample = 0;
        inputLufs.fill (0.0f);
        outputLufs.fill (0.0f);
    }

    GuardOutput ContrastGuard::process (float inputShortTermLufs, float outputShortTermLufs,
                                        bool musicActive, int numSamples, bool clubDelivery) noexcept
    {
        const auto n = juce::jmax (0, numSamples);
        samplesUntilSample -= n;

        if (musicActive && finiteReading (inputShortTermLufs) && finiteReading (outputShortTermLufs)
            && samplesUntilSample <= 0)
        {
            inputLufs[(size_t) writeIndex] = inputShortTermLufs;
            outputLufs[(size_t) writeIndex] = outputShortTermLufs;
            writeIndex = (writeIndex + 1) % histogramCapacity;
            count = juce::jmin (histogramCapacity, count + 1);
            samplesUntilSample += strideSamples;
        }

        auto loss = 0.0f;
        auto known = false;

        if (count >= 8)
        {
            float inCopy[histogramCapacity];
            float outCopy[histogramCapacity];
            const auto start = (writeIndex - count + histogramCapacity) % histogramCapacity;

            for (int i = 0; i < count; ++i)
            {
                const auto index = (start + i) % histogramCapacity;
                inCopy[i] = inputLufs[(size_t) index];
                outCopy[i] = outputLufs[(size_t) index];
            }

            const auto inSpan = percentile (inCopy, count, 0.90f) - percentile (inCopy, count, 0.25f);
            const auto outSpan = percentile (outCopy, count, 0.90f) - percentile (outCopy, count, 0.25f);
            loss = inSpan - outSpan;
            known = std::isfinite (loss);
        }

        const auto limit = clubDelivery ? contrastLimitClubLu : contrastLimitLu;
        const auto over = known && loss > limit;
        const auto dt = (double) n / sampleRate;
        const auto step = contrastRateDbPerSecond * (float) dt;

        if (over)
            reliefDb = juce::jmax (contrastFloorDb, reliefDb - step);
        else if (reliefDb < 0.0f)
            reliefDb = juce::jmin (0.0f, reliefDb + step);

        retreating = over && reliefDb > contrastFloorDb + 0.0001f;
        const auto atFloor = over && ! (reliefDb > contrastFloorDb + 0.0001f);

        GuardOutput out;
        out.reliefDb = reliefDb;
        out.engaged = retreating || atFloor;
        out.cause = out.engaged || reliefDb < 0.0f ? GuardCause::contrast : GuardCause::none;
        return out;
    }

    std::unique_ptr<juce::XmlElement> ContrastGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> (tag);
        xmlio::Writer writer (*xml);
        writer.field ("reliefDb", reliefDb);
        writer.field ("retreating", retreating);
        writer.field ("writeIndex", writeIndex);
        writer.field ("count", count);
        writer.field ("samplesUntilSample", samplesUntilSample);
        writer.field ("inputLufs", inputLufs);
        writer.field ("outputLufs", outputLufs);
        return xml;
    }

    bool ContrastGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (! xml.hasTagName (tag))
            return false;

        ContrastGuard restored;
        restored.sampleRate = sampleRate;
        restored.strideSamples = strideSamples;
        xmlio::Reader reader (xml);
        reader.field ("reliefDb", restored.reliefDb);
        reader.field ("retreating", restored.retreating);
        reader.field ("writeIndex", restored.writeIndex);
        reader.field ("count", restored.count);
        reader.field ("samplesUntilSample", restored.samplesUntilSample);
        reader.field ("inputLufs", restored.inputLufs);
        reader.field ("outputLufs", restored.outputLufs);

        if (! reader.ok || restored.count < 0 || restored.count > histogramCapacity
            || restored.writeIndex < 0 || restored.writeIndex >= histogramCapacity
            || ! std::isfinite (restored.reliefDb))
            return false;

        *this = restored;
        return true;
    }
}
