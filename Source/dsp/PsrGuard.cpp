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
        floorDb = std::isfinite (newFloorDb) ? juce::jmin (0.0f, newFloorDb) : legacyGuardFloorDb;
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
        auto belowNow = false;   // 本步读数低于门槛：engaged 的「仍低」看它；持续满 3 s 只决定要不要再退

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
                belowNow = true;
                belowSeconds += stepSeconds;
                aboveSeconds = 0.0;

                if (reached (belowSeconds, sustainSeconds) && reliefDb > retreatBoundDb)   // Q15 A：已比界深就保持，不回弹
                    reliefDb = juce::jmax (juce::jmax (floorDb, retreatBoundDb), reliefDb - retreatDbPerSecond * dt);
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

        // engaged 的口径见 guardEngagedAfterStep（两个子块守门共用）。「仍低」看本步读数、不看 3 s 持续：持续计时被换交付档、
        // 或一步落进两线之间清零后，守门还在底上、读数也仍低，状态行不该空 3 s 显示「跟随」。
        engaged = guardEngagedAfterStep (musicActive && lastPsrValid, enabled, engaged, before, reliefDb, belowNow,
                                         juce::jmax (floorDb, retreatBoundDb));
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
        // reliefDb / floorDb 两项按 0.1.8.10 的不变量写（见 legacyGuardSavedReliefDb）；新代码读档读 reliefDbExact、不采纳 floorDb。
        const auto savedReliefDb = legacyGuardSavedReliefDb (reliefDb);
        writer.field ("reliefDb", savedReliefDb);
        writer.field ("floorDb", legacyGuardFloorDb);
        writer.field ("belowSeconds", belowSeconds);
        writer.field ("engaged", engaged);
        writer.optionalField ("aboveSeconds", aboveSeconds, 0.0);
        writer.optionalField ("reliefDbExact", reliefDb, savedReliefDb);
        return xml;
    }

    bool PsrGuard::restoreStateXml (const juce::XmlElement& xml)
    {
        if (xml.getTagName() != "PSR_GUARD")
            return false;

        auto legacyRelief = reliefDb;
        auto floor = floorDb;
        auto below = belowSeconds;
        auto above = aboveSeconds;
        auto wasEngaged = engaged;
        xmlio::Reader reader (xml);
        reader.field ("reliefDb", legacyRelief);
        reader.field ("floorDb", floor);
        reader.field ("belowSeconds", below);
        reader.field ("engaged", wasEngaged);
        reader.optionalField ("aboveSeconds", above, 0.0);   // T-097 之前的存档没有：按 0 读
        // 真实退让（T-097 起写）。0.1.8.10 写的节点、或在 0.1.8.10 里再存过的节点没有这一项：按 reliefDb 读。
        auto relief = legacyRelief;
        reader.optionalField ("reliefDbExact", relief, legacyRelief);

        // 退让 > 0 哪一版都写不出来（只往下退、回程夹在 0）：当损坏拒收，免得它挡住之后的退（退让停在正数上，往下退要先走回 0）。
        if (! reader.ok || ! std::isfinite (relief) || ! std::isfinite (legacyRelief) || relief > 0.0f || legacyRelief > 0.0f
            || ! std::isfinite (floor) || ! std::isfinite (below) || ! std::isfinite (above) || below < 0.0 || above < 0.0)
            return false;

        reliefDb = relief;   // floorDb 不动：存档里那份只写给 0.1.8.10（见 createStateXml），下限由编排器设
        belowSeconds = below;
        aboveSeconds = above;
        engaged = wasEngaged;
        clearWindow();
        return true;
    }
}
