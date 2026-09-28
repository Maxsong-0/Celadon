/*
  ==============================================================================

    DelayLine.h
    整数样本延迟线（Limit、Color 共用）。prepare 时分配，处理时零分配。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace celadon
{
    class DelayLine
    {
    public:
        // 分配能容纳 maxDelay 个样本延迟的缓冲（不在音频线程调用）。
        void prepare (int maxDelay)
        {
            buffer.assign ((size_t) juce::jmax (1, maxDelay + 1), 0.0f);
            writeIndex = 0;
            delay = juce::jlimit (0, maxDelay, delay);
        }

        // 调用方保证 newDelay ≤ prepare 时的 maxDelay；改延迟后通常要 clear()。
        void setDelay (int newDelay) noexcept   { delay = newDelay; }
        int getDelay() const noexcept           { return delay; }

        void clear() noexcept
        {
            fill (0.0f);
        }

        // 整条延迟线填成同一个值（例如重置时让「延迟后的参数值」立即等于当前值）。
        void fill (float value) noexcept
        {
            std::fill (buffer.begin(), buffer.end(), value);
            writeIndex = 0;
        }

        // 写入一个样本，返回 delay 个样本之前写入的那个。
        float pushPop (float x) noexcept
        {
            buffer[(size_t) writeIndex] = x;

            auto readIndex = writeIndex - delay;
            if (readIndex < 0)
                readIndex += (int) buffer.size();

            if (++writeIndex == (int) buffer.size())
                writeIndex = 0;

            return buffer[(size_t) readIndex];
        }

    private:
        std::vector<float> buffer;
        int writeIndex = 0;
        int delay = 0;
    };
}
