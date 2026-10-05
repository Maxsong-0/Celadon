/*
  ==============================================================================

    PsrGuard.h
    PSR 守门（T-048 阶段 2，T-047 起点；Q8 单独成一项原因）。

    3 s PSR 低于该档下限再低 0.5，并且持续 3 s，就以 0.5 dB/s 退让。
    回到安全区后以 0.15 dB/s 回到 0。下限：流媒体与标准 8，俱乐部 6.5。
    音频线程，不分配、不加锁。clear() 只用于冷启动。

  ==============================================================================
*/

#pragma once

#include "GlazeTypes.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace celadon::glaze
{
    class PsrGuard
    {
    public:
        void prepare (double sampleRate) noexcept;
        void clear() noexcept;
        void snapRelief() noexcept;

        GuardOutput process (float psrDb, bool psrValid, bool musicActive, int numSamples, Delivery delivery) noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

    private:
        double sampleRate = 48000.0;
        float reliefDb = 0.0f;
        double belowSeconds = 0.0;
        bool retreating = false;
    };
}
