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

    void PsrGuard::prepare (double newStepSeconds) noexcept
    {
        if (newStepSeconds > 0.0 && std::isfinite (newStepSeconds))
            stepSeconds = newStepSeconds;

        clear();
    }

    void PsrGuard::clear() noexcept
    {
        reliefDb = 0.0f;
        belowSeconds = 0.0;
        aboveSeconds = 0.0;
        engaged = false;
        clearWindow();
    }

    void PsrGuard::clearWindow() noexcept
    {
        window.clear();
        lastPsrValid = false;
        lastPsrDb = 0.0f;
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
        aboveSeconds = 0.0;
    }

    GuardOutput PsrGuard::process (const LoudnessMeter::SubBlock& output, bool musicActive, bool enabled, Delivery delivery) noexcept
    {
        window.push (output.meanSquare, output.truePeakLinear);

        double psr = 0.0;
        lastPsrValid = window.psrDb (psr);
        lastPsrDb = lastPsrValid ? (float) psr : 0.0f;

        const auto before = reliefDb;
        const auto dt = (float) stepSeconds;
        auto sustainedBelow = false;

        if (musicActive && ! enabled)
        {
            belowSeconds = 0.0;
            aboveSeconds = 0.0;
            reliefDb = juce::jmin (0.0f, reliefDb + releaseDbPerSecond * dt);
        }
        else if (musicActive && lastPsrValid)
        {
            const auto trip = (double) tripFor (delivery);

            if (psr < trip)
            {
                belowSeconds += stepSeconds;
                aboveSeconds = 0.0;
                sustainedBelow = reached (belowSeconds, sustainSeconds);

                if (sustainedBelow)
                    reliefDb = juce::jmax (floorDb, reliefDb - retreatDbPerSecond * dt);
            }
            else if (psr >= trip + (double) releaseHysteresisDb)
            {
                aboveSeconds += stepSeconds;
                belowSeconds = 0.0;

                if (reached (aboveSeconds, releaseAfterSeconds))
                    reliefDb = juce::jmin (0.0f, reliefDb + releaseDbPerSecond * dt);
            }
            else
            {
                belowSeconds = 0.0;
                aboveSeconds = 0.0;
            }
        }
        // 其余（非有效音乐，或读数无效）：计时与退让都保持。

        engaged = reliefDb < before || (reliefDb <= floorDb && sustainedBelow);
        return current();
    }

    GuardOutput PsrGuard::current() const noexcept
    {
        GuardOutput out;
        out.reliefDb = reliefDb;
        out.cause = reliefDb < 0.0f ? GuardCause::psr : GuardCause::none;
        out.engaged = engaged;
        return out;
    }

    bool PsrGuard::getLastPsrDb (float& psrDb) const noexcept
    {
        if (! lastPsrValid)
            return false;

        psrDb = lastPsrDb;
        return true;
    }

    std::unique_ptr<juce::XmlElement> PsrGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("PSR_GUARD");
        xmlio::Writer writer (*xml);
        writer.field ("reliefDb", reliefDb);
        writer.field ("floorDb", floorDb);
        writer.field ("belowSeconds", belowSeconds);
        writer.field ("engaged", engaged);
        writer.optionalField ("aboveSeconds", aboveSeconds, 0.0);
        return xml;
    }

    bool PsrGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "PSR_GUARD")
            return false;

        auto relief = reliefDb;
        auto floor = floorDb;
        auto below = belowSeconds;
        auto above = aboveSeconds;
        auto wasEngaged = engaged;
        xmlio::Reader reader (xml);
        reader.field ("reliefDb", relief);
        reader.field ("floorDb", floor);
        reader.field ("belowSeconds", below);
        reader.field ("engaged", wasEngaged);
        reader.optionalField ("aboveSeconds", above, 0.0);   // T-097 之前的存档没有：按 0 读

        if (! reader.ok || ! std::isfinite (relief) || ! std::isfinite (floor) || ! std::isfinite (below) || ! std::isfinite (above)
            || below < 0.0 || above < 0.0)
            return false;

        reliefDb = relief;
        floorDb = floor;
        belowSeconds = below;
        aboveSeconds = above;
        engaged = wasEngaged;
        clearWindow();
        return true;
    }
}
