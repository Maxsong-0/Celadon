#include "PsrGuard.h"

#include <cmath>

namespace celadon::glaze
{
    float PsrGuard::floorFor (Delivery delivery) noexcept
    {
        switch (delivery)
        {
            case Delivery::club:      return 6.0f;
            case Delivery::streaming: return 7.0f;
            case Delivery::standard:  return 8.0f;
            default:                  return 8.0f;
        }
    }

    void PsrGuard::prepare (double newSampleRate) noexcept
    {
        if (newSampleRate > 0.0 && std::isfinite (newSampleRate))
            sampleRate = newSampleRate;

        clear();
    }

    void PsrGuard::clear() noexcept
    {
        reliefDb = 0.0f;
        belowSeconds = 0.0;
        engaged = false;
    }

    void PsrGuard::setFloorDb (float newFloorDb) noexcept
    {
        floorDb = std::isfinite (newFloorDb) ? juce::jmin (0.0f, newFloorDb) : -3.0f;
        reliefDb = juce::jmax (floorDb, reliefDb);
    }

    void PsrGuard::snapRelief() noexcept
    {
        reliefDb = 0.0f;
        engaged = false;
        belowSeconds = 0.0;
    }

    GuardOutput PsrGuard::process (float psrDb, bool psrValid, bool musicActive, int numSamples, Delivery delivery) noexcept
    {
        const auto dt = musicActive && numSamples > 0 ? (double) numSamples / sampleRate : 0.0;
        const auto trip = floorFor (delivery) - marginDb;
        const auto below = psrValid && std::isfinite (psrDb) && psrDb < trip;

        if (musicActive)
            belowSeconds = below ? belowSeconds + dt : 0.0;

        const auto before = reliefDb;
        const auto sustained = below && belowSeconds >= sustainSeconds;

        if (sustained)
            reliefDb = juce::jmax (floorDb, reliefDb - retreatDbPerSecond * (float) dt);
        else if (! below)
            reliefDb = juce::jmin (0.0f, reliefDb + releaseDbPerSecond * (float) dt);

        const auto atFloor = reliefDb <= floorDb;
        engaged = reliefDb < before || (atFloor && sustained);

        GuardOutput out;
        out.reliefDb = reliefDb;
        out.cause = reliefDb < 0.0f ? GuardCause::psr : GuardCause::none;
        out.engaged = engaged;
        return out;
    }

    std::unique_ptr<juce::XmlElement> PsrGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("PSR_GUARD");
        xmlio::Writer writer (*xml);
        writer.field ("reliefDb", reliefDb);
        writer.field ("floorDb", floorDb);
        writer.field ("belowSeconds", belowSeconds);
        writer.field ("engaged", engaged);
        return xml;
    }

    bool PsrGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "PSR_GUARD")
            return false;

        auto relief = reliefDb;
        auto floor = floorDb;
        auto below = belowSeconds;
        auto wasEngaged = engaged;
        xmlio::Reader reader (xml);
        reader.field ("reliefDb", relief);
        reader.field ("floorDb", floor);
        reader.field ("belowSeconds", below);
        reader.field ("engaged", wasEngaged);

        if (! reader.ok || ! std::isfinite (relief) || ! std::isfinite (below))
            return false;

        reliefDb = relief;
        floorDb = floor;
        belowSeconds = below;
        engaged = wasEngaged;
        return true;
    }
}
