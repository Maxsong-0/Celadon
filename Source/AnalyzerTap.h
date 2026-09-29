/*
  ==============================================================================

    AnalyzerTap.h
    频谱仪的取样口（v0.1.8.0 界面改版）：音频线程把链入口（IN）与插件最终输出（OUT）的单声道和推进两个
    无锁单生产者 / 单消费者 FIFO，界面线程取走后自己做 FFT。

      - 只在编辑器开着时推（setActive）：没有界面时音频线程每块只多读一个原子量；
      - 单声道和 = (L + R) / 2（单声道输入就是它本身）。频谱仪看的是「音乐整体的频谱」，侧声道成分在和里会相消，
        与多数母带频谱仪的默认口径一致；
      - FIFO 满了（界面卡住、没来取）就丢掉新来的样本：音频线程绝不等待，界面下一次取到的只是晚一点的片段；
      - 不改 buffer、不分配内存：push 只拷贝。

    只依赖 juce_core 与 juce_audio_basics，无头测试也能编（tests/AnalyzerTapTests.cpp）。

  ==============================================================================
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace celadon
{
    class AnalyzerTap
    {
    public:
        enum class Stream : int { input = 0, output = 1 };

        // 每路容量（样本）：192 kHz 下约 170 ms，界面每 30 ms 来取一次，宿主块再大也装得下。
        static constexpr int capacity = 1 << 15;

        AnalyzerTap()
        {
            for (auto& stream : streams)
                stream.samples.fill (0.0f);
        }

        // 界面线程：编辑器打开时 true、关闭时 false。
        void setActive (bool shouldBeActive) noexcept     { active.store (shouldBeActive, std::memory_order_relaxed); }
        bool isActive() const noexcept                     { return active.load (std::memory_order_relaxed); }

        // 音频线程：把 buffer 前 numSamples 个样本的单声道和推进指定的一路。没开或 FIFO 满时丢弃（不等待）。
        void push (Stream which, const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            if (! isActive() || numSamples <= 0 || buffer.getNumChannels() <= 0)
                return;

            auto& stream = streams[(size_t) which];
            const auto count = juce::jmin (numSamples, buffer.getNumSamples(), stream.fifo.getFreeSpace());

            if (count <= 0)
                return;

            const auto* left  = buffer.getReadPointer (0);
            const auto* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr;

            // 先写满 count 个再一次性交出去：界面线程要么看到整段，要么一个都看不到。
            const auto scope = stream.fifo.write (count);
            auto sourceIndex = 0;

            const auto copyInto = [&] (int start, int length) noexcept
            {
                for (int i = 0; i < length; ++i, ++sourceIndex)
                {
                    const auto l = left[sourceIndex];
                    stream.samples[(size_t) (start + i)] = right != nullptr ? 0.5f * (l + right[sourceIndex]) : l;
                }
            };

            copyInto (scope.startIndex1, scope.blockSize1);
            copyInto (scope.startIndex2, scope.blockSize2);
        }

        // 界面线程：取走这一路最多 maxSamples 个样本（按到达顺序），返回取到的个数。
        int pull (Stream which, float* destination, int maxSamples) noexcept
        {
            auto& stream = streams[(size_t) which];
            const auto count = juce::jmin (maxSamples, stream.fifo.getNumReady());

            if (count <= 0 || destination == nullptr)
                return 0;

            const auto scope = stream.fifo.read (count);
            auto destinationIndex = 0;

            const auto copyFrom = [&] (int start, int length) noexcept
            {
                for (int i = 0; i < length; ++i)
                    destination[destinationIndex++] = stream.samples[(size_t) (start + i)];
            };

            copyFrom (scope.startIndex1, scope.blockSize1);
            copyFrom (scope.startIndex2, scope.blockSize2);
            return count;
        }

        // 界面线程：扔掉这一路积压的样本（编辑器刚打开时，里面可能是很久以前的片段）。
        void discard (Stream which) noexcept
        {
            auto& stream = streams[(size_t) which];
            const auto ready = stream.fifo.getNumReady();

            if (ready > 0)
                stream.fifo.read (ready);
        }

    private:
        struct Channel
        {
            juce::AbstractFifo fifo { capacity };
            std::array<float, (size_t) capacity> samples;
        };

        std::array<Channel, 2> streams;
        std::atomic<bool> active { false };

        JUCE_DECLARE_NON_COPYABLE (AnalyzerTap)
    };
}
