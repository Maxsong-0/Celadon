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
        floorDb = std::isfinite (newFloorDb) ? juce::jmin (0.0f, newFloorDb) : legacyFloorDb;
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

        // engaged = 本步在退，或已退到底（floorDb 与 Q15 A 的界中较浅的那个）且本步读数仍低于门槛。到界停住、或比界深而保持时也算在起作用：
        // 状态行「已经退到底仍超」靠它（同预算环 planLimited && lastOver，看此刻超没超、不等持续）。不看 3 s 持续：持续计时被换交付档、
        // 或一步落进两线之间清零后，守门还在底上、读数也仍低，状态行不该空 3 s 显示「跟随」。底为 0（界为 0 = 没有自动部分，
        // applyLimitExtras 一点也退不了）时不算，哪怕早先退下的量还保持着。
        // 本步没有读数（非有效音乐，或窗不满 3 s、近乎静音）时，计时与退让都保持，engaged 也保持上一步的：跳播、循环回绕、
        // 冻结结束、读档之后约 3 s 状态行不该从「受限」掉回「跟随」再跳回来（退让一直在执行）。界回到 0 时照样清掉。
        const auto bottom = juce::jmax (floorDb, retreatBoundDb);

        if (musicActive && (! enabled || lastPsrValid))
            engaged = reliefDb < before || (belowNow && bottom < 0.0f && reliefDb <= bottom);
        else
            engaged = engaged && bottom < 0.0f;
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
        // 写给 0.1.8.10：它读档直接拿这个数当底。写 −3（已退得更深时写当前退让，旧版读回不跳）；新代码读档只校验、不采纳。
        const auto savedFloorDb = juce::jmin (legacyFloorDb, reliefDb);
        writer.field ("floorDb", savedFloorDb);
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

        reliefDb = relief;   // floorDb 不动：存档里那份只写给 0.1.8.10（见 createStateXml），下限由编排器设
        belowSeconds = below;
        aboveSeconds = above;
        engaged = wasEngaged;
        clearWindow();
        return true;
    }
}
