/*
  ==============================================================================

    ContrastGuard.h
    T-048 阶段 2 落差守门（草图第 4 节，Q8 / Q9）。音频线程、定长、不分配、不加锁。

    输入、输出各一份短期响度的滑动窗。(P90 − P25) 的输入减输出超过 1.2 LU（俱乐部 1.5）时，
    以 ≤ 0.1 dB/s 把 Limit 退让往更负的方向收回；否则以同样的速率回到 0。退让钳在 floorDb（≤ 0）。

    clear() 只用于冷启动。宿主 reset 不清累计窗，只可另清滤波器类状态（这里没有）。

  ==============================================================================
*/

#pragma once

#include "GlazeConstants.h"
#include "GlazeTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <memory>

namespace celadon::glaze
{
    class ContrastGuard
    {
    public:
        void prepare (double sampleRate) noexcept;
        void clear() noexcept;
        void setFloorDb (float floorDb) noexcept;
        void snapRelief() noexcept;

        GuardOutput process (float inputShortTermLufs, float outputShortTermLufs,
                             bool musicActive, int numSamples, bool clubDelivery) noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

    private:
        static constexpr int ringCapacity = 128;
        static constexpr float retreatDbPerSecond = 0.1f;
        static constexpr float standardSpreadLu = 1.2f;
        static constexpr float clubSpreadLu = 1.5f;

        void push (float inputLufs, float outputLufs) noexcept;
        bool spreads (float& inputSpread, float& outputSpread) const noexcept;

        double sampleRate = 48000.0;
        int pushLength = 4800;
        int samplesUntilPush = 4800;
        float floorDb = -3.0f;
        float reliefDb = 0.0f;
        bool engaged = false;

        std::array<float, (std::size_t) ringCapacity> inputRing {}, outputRing {};
        int ringCount = 0;
        int ringWrite = 0;
    };
}
