/*
  ==============================================================================

    OutputLevelTracker.h
    出口主段电平（T-048 阶段 2，Q9）。不用第二份 LiveProfile。

    门用输入侧已发布画像给出的 m′（noteMainness）。能量是出口 K 加权、
    按「m′ ≥ 0.5」的同一段累计。m′ 掉到 0.5 以下就封存这一段，下一次
    回到主段再开新的一段。mainLevelLufs() 给 G_trim。
    音频线程，不分配、不加锁。clear() 只用于冷启动。

  ==============================================================================
*/

#pragma once

#include "LoudnessMeter.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace celadon::glaze
{
    class OutputLevelTracker
    {
    public:
        void prepare (double sampleRate) noexcept;
        void clear() noexcept;

        void noteMainness (float mainness) noexcept;
        void processOutput (const juce::AudioBuffer<float>& output) noexcept;
        float mainLevelLufs() const noexcept;
        bool hasMainLevel() const noexcept { return levelValid; }

        std::unique_ptr<juce::XmlElement> createStateXml() const;
        bool restoreStateXml (const juce::XmlElement&);

    private:
        void closeSegment() noexcept;

        double sampleRate = 48000.0;
        LoudnessMeter::Biquad shelf {};
        LoudnessMeter::Biquad highpass {};
        double shelfS1[2] {};
        double shelfS2[2] {};
        double highpassS1[2] {};
        double highpassS2[2] {};
        bool inMain = false;
        double energy = 0.0;
        std::int64_t samples = 0;
        float levelLufs = 0.0f;
        bool levelValid = false;
    };
}
