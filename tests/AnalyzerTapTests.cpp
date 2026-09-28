/*
  ==============================================================================

    AnalyzerTapTests.cpp
    频谱仪取样口（Source/AnalyzerTap.h，v0.1.8.0）的自动测试：没开时不推、单声道和、按到达顺序取出、
    跨环形缓冲的边界不错位、满了丢新样本不阻塞、discard 清掉积压。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/AnalyzerTap.h"

#include <memory>
#include <vector>

namespace celadon::test
{
    class AnalyzerTapTests  : public juce::UnitTest
    {
    public:
        AnalyzerTapTests() : juce::UnitTest ("AnalyzerTap", "Celadon") {}

        void runTest() override
        {
            using Stream = AnalyzerTap::Stream;

            // 取样口本身有 2 × 32768 个 float，放堆上。
            auto tap = std::make_unique<AnalyzerTap>();
            std::vector<float> out ((size_t) AnalyzerTap::capacity);

            beginTest ("inactive tap ignores pushes");
            {
                juce::AudioBuffer<float> block (2, 64);
                block.clear();
                tap->push (Stream::input, block, 64);
                expectEquals (tap->pull (Stream::input, out.data(), (int) out.size()), 0);
            }

            beginTest ("stereo is summed to mono, mono passes through, streams stay separate");
            {
                tap->setActive (true);

                juce::AudioBuffer<float> stereo (2, 8);

                for (int i = 0; i < 8; ++i)
                {
                    stereo.setSample (0, i, (float) i);
                    stereo.setSample (1, i, (float) (10 * i));
                }

                tap->push (Stream::input, stereo, 8);

                juce::AudioBuffer<float> mono (1, 4);

                for (int i = 0; i < 4; ++i)
                    mono.setSample (0, i, -0.25f * (float) i);

                tap->push (Stream::output, mono, 4);

                expectEquals (tap->pull (Stream::input, out.data(), (int) out.size()), 8);

                for (int i = 0; i < 8; ++i)
                    expectEquals (out[(size_t) i], 0.5f * (float) (i + 10 * i));

                expectEquals (tap->pull (Stream::output, out.data(), (int) out.size()), 4);

                for (int i = 0; i < 4; ++i)
                    expectEquals (out[(size_t) i], -0.25f * (float) i);
            }

            beginTest ("samples come out in order across the ring boundary, in partial pulls");
            {
                juce::AudioBuffer<float> block (1, 1000);
                float next = 0.0f, expected = 0.0f;
                bool inOrder = true;

                // 推 100 次、每次 1000 个，每推 1 次取两次（先 300 再其余）：写指针绕环好几圈。
                for (int round = 0; round < 100; ++round)
                {
                    for (int i = 0; i < 1000; ++i)
                        block.setSample (0, i, next++);

                    tap->push (Stream::input, block, 1000);

                    for (const auto chunk : { 300, 1000 })
                    {
                        const auto got = tap->pull (Stream::input, out.data(), chunk);

                        for (int i = 0; i < got; ++i)
                            inOrder = inOrder && juce::exactlyEqual (out[(size_t) i], expected++);
                    }
                }

                expect (inOrder, "every sample arrives once and in order");
                expectEquals (expected, next);
            }

            beginTest ("a full FIFO drops new samples instead of blocking; discard empties it");
            {
                juce::AudioBuffer<float> block (1, 4096);
                block.clear();

                for (int i = 0; i < 20; ++i)
                    tap->push (Stream::output, block, 4096);

                const auto ready = tap->pull (Stream::output, out.data(), (int) out.size());
                expect (ready > 0 && ready < AnalyzerTap::capacity, "holds at most capacity - 1 samples: " + juce::String (ready));

                tap->push (Stream::output, block, 4096);
                tap->discard (Stream::output);
                expectEquals (tap->pull (Stream::output, out.data(), (int) out.size()), 0);

                tap->setActive (false);
                tap->push (Stream::output, block, 4096);
                expectEquals (tap->pull (Stream::output, out.data(), (int) out.size()), 0);
            }
        }
    };

    static AnalyzerTapTests analyzerTapTests;
}
