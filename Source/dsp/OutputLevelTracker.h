/*
  ==============================================================================

    OutputLevelTracker.h
    T-048 阶段 2 输出侧主段电平（Q9）。不用第二份 LiveProfile。

    门是输入侧已发布的 m′（noteMainness）。能量是出口 K 加权、按同一段累计。
    m′ ≥ 0.5 的段才进入主段电平。G_trim 读 mainLevelLufs()。

    clear() 只用于冷启动。宿主 reset 只清 K 加权滤波器（resetFilters），不清段累计。
    滤波器状态不进 LIVE（草图第 5 节：滤波器从零开始）；段累计进 LIVE。

  ==============================================================================
*/

#pragma once

#include "GlazeTypes.h"
#include "LoudnessMeter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <memory>

namespace celadon::glaze
{
    class OutputLevelTracker
    {
    public:
        void prepare (double sampleRate) noexcept;
        void clear() noexcept;
        void resetFilters() noexcept;

        // 本段暂判的 m′。跨过 0.5，或调用方 closeSegment()，就把上一段收进主段账。
        void noteMainness (float mainness) noexcept;
        void closeSegment() noexcept;

        void processOutput (const juce::AudioBuffer<float>& output) noexcept;

        bool mainLevelValid() const noexcept;
        float mainLevelLufs() const noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

    private:
        struct Segment
        {
            double energy = 0.0;
            std::int64_t samples = 0;
            float mainness = 0.0f;
        };

        void pushClosed (const Segment& segment) noexcept;
        void resetChannel (int channel) noexcept;
        bool accumulate (double& energy, std::int64_t& samples) const noexcept;

        static constexpr int closedCapacity = 32;
        static constexpr float mainnessGate = 0.5f;

        double sampleRate = 48000.0;
        std::int64_t maxOpenSamples = 384000;
        LoudnessMeter::Biquad shelf {}, highpass {};
        std::array<double, 2> shelfS1 {}, shelfS2 {}, highpassS1 {}, highpassS2 {};
        Segment open;
        std::array<Segment, (std::size_t) closedCapacity> closed {};
        int closedCount = 0;
        int closedWrite = 0;
        bool haveOpenMainness = false;
        bool openWasMain = false;
    };
}
