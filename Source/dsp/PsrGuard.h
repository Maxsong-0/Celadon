/*
  ==============================================================================

    PsrGuard.h
    T-048 阶段 2 PSR 守门（草图第 4 节，Q8：GuardCause 单列 psr）。音频线程、定长、不分配、不加锁。

    3 s PSR 低于该档下限 − 0.5 dB，并且持续满 3 s，就以 0.5 dB/s 退让。条件消失后以 0.15 dB/s 回到 0
    （摘录没有回程速率，取预算环回程的慢端）。下限是本阶段的出厂起点，不进 GlazeConstants（Q12）。

    clear() 只用于冷启动。宿主 reset 不调。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
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
        void setFloorDb (float floorDb) noexcept;
        void snapRelief() noexcept;

        // psrValid = false 时不计时、不推动退让（读数保持）。
        GuardOutput process (float psrDb, bool psrValid, bool musicActive, int numSamples, Delivery delivery) noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

        // 出厂下限（dB）。「低于下限 − 0.5」才开始计 3 s。
        static float floorFor (Delivery delivery) noexcept;

    private:
        static constexpr float marginDb = 0.5f;
        static constexpr float retreatDbPerSecond = 0.5f;
        static constexpr float releaseDbPerSecond = 0.15f;
        static constexpr double sustainSeconds = 3.0;

        double sampleRate = 48000.0;
        float floorDb = -3.0f;
        float reliefDb = 0.0f;
        double belowSeconds = 0.0;
        bool engaged = false;
    };
}
