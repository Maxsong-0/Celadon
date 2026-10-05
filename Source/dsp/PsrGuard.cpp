#include "PsrGuard.h"

#include "GlazeConstants.h"

#include <cmath>

namespace celadon::glaze
{
    namespace
    {
        constexpr float psrFloorDb = 8.0f;
        constexpr float psrFloorClubDb = 6.5f;
        constexpr float psrMarginDb = 0.5f;
        constexpr double psrHoldSeconds = 3.0;
        constexpr float psrRetreatDbPerSecond = 0.5f;
        constexpr float psrReleaseDbPerSecond = 0.15f;
        constexpr float psrFloorReliefDb = -3.0f;
        constexpr const char* tag = "PSR_GUARD";

        float floorFor (Delivery delivery) noexcept
        {
            return delivery == Delivery::club ? psrFloorClubDb : psrFloorDb;
        }
    }

    void PsrGuard::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
        clear();
    }

    void PsrGuard::snapRelief() noexcept
    {
        reliefDb = 0.0f;
        retreating = false;
        belowSeconds = 0.0;
    }

    void PsrGuard::clear() noexcept
    {
        reliefDb = 0.0f;
        belowSeconds = 0.0;
        retreating = false;
    }

    GuardOutput PsrGuard::process (float psrDb, bool psrValid, bool musicActive, int numSamples, Delivery delivery) noexcept
    {
        const auto dt = (double) juce::jmax (0, numSamples) / sampleRate;
        const auto tooLow = musicActive && psrValid && std::isfinite (psrDb) && psrDb < floorFor (delivery) - psrMarginDb;

        if (tooLow)
            belowSeconds = juce::jmin (psrHoldSeconds, belowSeconds + dt);
        else
            belowSeconds = 0.0;

        const auto over = belowSeconds >= psrHoldSeconds && tooLow;
        const auto stepRetreat = psrRetreatDbPerSecond * (float) dt;
        const auto stepRelease = psrReleaseDbPerSecond * (float) dt;

        if (over)
            reliefDb = juce::jmax (psrFloorReliefDb, reliefDb - stepRetreat);
        else if (reliefDb < 0.0f)
            reliefDb = juce::jmin (0.0f, reliefDb + stepRelease);

        retreating = over && reliefDb > psrFloorReliefDb + 0.0001f;
        const auto atFloor = over && ! (reliefDb > psrFloorReliefDb + 0.0001f);

        GuardOutput out;
        out.reliefDb = reliefDb;
        out.engaged = retreating || atFloor;
        out.cause = out.engaged || reliefDb < 0.0f ? GuardCause::psr : GuardCause::none;
        return out;
    }

    std::unique_ptr<juce::XmlElement> PsrGuard::createStateXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> (tag);
        xmlio::Writer writer (*xml);
        writer.field ("reliefDb", reliefDb);
        writer.field ("belowSeconds", belowSeconds);
        writer.field ("retreating", retreating);
        return xml;
    }

    bool PsrGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (! xml.hasTagName (tag))
            return false;

        xmlio::Reader reader (xml);
        float relief = 0.0f;
        double below = 0.0;
        bool moving = false;
        reader.field ("reliefDb", relief);
        reader.field ("belowSeconds", below);
        reader.field ("retreating", moving);

        if (! reader.ok || ! std::isfinite (relief) || ! std::isfinite (below) || below < 0.0)
            return false;

        reliefDb = relief;
        belowSeconds = below;
        retreating = moving;
        return true;
    }
}
