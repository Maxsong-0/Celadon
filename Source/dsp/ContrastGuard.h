/*
  ==============================================================================

    ContrastGuard.h
    落差守门（T-048 阶段 2，T-047 起点；Max 2026-10-05 接受 Q8）。

    输入、输出各一份短期响度的定长直方图。(P90 − P25) 输入减输出超过 1.2 LU
    （俱乐部 1.5）时，退让以 ≤ 0.1 dB/s 往更负走，把多出来的压缩收回。
    回到门槛以下则以同样的速率回到 0。音频线程，不分配、不加锁。
    clear() 只用于冷启动。宿主 reset 不调。

  ==============================================================================
*/

#pragma once

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
        void snapRelief() noexcept;

        GuardOutput process (float inputShortTermLufs, float outputShortTermLufs,
                             bool musicActive, int numSamples, bool clubDelivery) noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

    private:
        static constexpr int histogramCapacity = 64;   // 约 6.4 s，每 100 ms 一点
        static constexpr double sampleStrideSeconds = 0.1;

        double sampleRate = 48000.0;
        int strideSamples = 4800;
        int samplesUntilSample = 0;
        float reliefDb = 0.0f;
        bool retreating = false;
        int writeIndex = 0;
        int count = 0;
        std::array<float, (size_t) histogramCapacity> inputLufs {};
        std::array<float, (size_t) histogramCapacity> outputLufs {};
    };
}
