/*
  ==============================================================================

    ProcessorConcurrencyTests.cpp
    存读档与音频线程同时进行（T-099，ADR-040）。音频线程按宿主包装层的样子每块持回调锁连续 processBlock（ProcessorHarness.h
    AudioThread），主线程同时存档 / 读档。

    这几条用例的主要用处是在 ThreadSanitizer 下跑（cmake -DCELADON_TSAN=ON，见 CMakeLists.txt 与
    docs/validation/2026-11-13-T099-state-lock）：处理器在存读档时不拿回调锁的话，TSan 报编排器 / 实时画像上的数据竞争；
    拿了就不报。不开 TSan 时它们照样核对能看得见的后果：

      - 播放中存档：每一份存档里的实时画像都读得回来（存到一半被音频线程改了的话，头里的段数与段对不上，读回整份冷启动）；
      - 播放中读档、实例还没听到音乐：每次都落位（参数换掉，GLAZE 存档读回），音频线程照常跑；
      - 播放中读档、实例已在运行：参数照换，实时画像保留（读完画像没被换成预设里的空画像，段数只增不减）。

    节拍：块长 256（比默认的 512 多交错一倍）、尽快送块；读档 / 存档之间等音频线程再处理几块，保证两边真的交错。

  ==============================================================================
*/

#include "ProcessorHarness.h"

namespace celadon::processortest
{
    namespace
    {
        constexpr int concurrentBlockSize = 256;
        constexpr int iterations = 24;
        constexpr std::uint64_t blocksBetweenCalls = 3;
    }

    class ProcessorConcurrencyTests final : public juce::UnitTest
    {
    public:
        ProcessorConcurrencyTests() : juce::UnitTest ("ProcessorConcurrency", "CeladonProcessor") {}

        void runTest() override
        {
            const auto projectA = makeProjectA (concurrentBlockSize);
            const auto presetD = makePresetD (concurrentBlockSize);
            const auto summaryA = summarise (projectA);

            beginTest ("rig precondition: project A carries a live profile");
            expect (summaryA.window > 0, describe (summaryA));

            testSaveWhilePlaying();
            testLoadWhilePlayingBeforeMusic (projectA, presetD, summaryA);
            testLoadWhilePlayingWithLiveState (projectA, presetD);
        }

    private:
        void testSaveWhilePlaying()
        {
            beginTest ("save while playing: every saved live profile restores");

            Rig rig (concurrentBlockSize);
            applyProjectA (rig);
            rig.prepare();
            rig.playMusic (20.0);   // 先在本线程攒出画像，并发阶段每份存档都该带着它

            juce::Array<juce::MemoryBlock> saved;

            {
                AudioThread audio (rig, true);

                for (int i = 0; i < iterations; ++i)
                {
                    expect (audio.waitForBlocks (blocksBetweenCalls), "audio thread stalled");
                    saved.add (rig.save());
                }
            }

            glaze::LiveProfile checker;
            int broken = 0, empty = 0;

            for (const auto& block : saved)
            {
                const auto summary = summarise (block, &checker);
                broken += summary.restorable ? 0 : 1;
                empty += summary.window > 0 ? 0 : 1;
            }

            expectEquals (broken, 0, "saved states whose live profile does not restore (of " + juce::String (saved.size()) + ")");
            expectEquals (empty, 0, "saved states with an empty live profile");
        }

        void testLoadWhilePlayingBeforeMusic (const juce::MemoryBlock& projectA, const juce::MemoryBlock& presetD,
                                              const GlazeSummary& summaryA)
        {
            beginTest ("load while playing, no music heard yet: every load restores GLAZE and swaps parameters");

            // 再造一份「参数不同、画像也空」的预设，与 D 来回换：两份都没有画像，实例一直不算「运行中」，每次读档都走落位那条路。
            const auto presetE = []
            {
                Rig donor (concurrentBlockSize);
                applyProjectA (donor);
                donor.prepare();
                return donor.save();
            }();

            Rig rig (concurrentBlockSize);
            rig.prepare();

            {
                AudioThread audio (rig, false);

                for (int i = 0; i < iterations; ++i)
                {
                    expect (audio.waitForBlocks (blocksBetweenCalls), "audio thread stalled");
                    const auto& blob = (i % 2 == 0) ? presetD : presetE;
                    rig.load (blob);
                    expect (parameterMismatches (*rig.processor, blob).isEmpty(),
                            "load " + juce::String (i) + " did not swap parameters: " + parameterMismatches (*rig.processor, blob).joinIntoString (", "));
                }

                // 最后读带画像的工程 A：同样在播放中落位（实例只送过静音，不算运行中）。
                expect (audio.waitForBlocks (blocksBetweenCalls));
                rig.load (projectA);
                expect (audio.waitForBlocks (blocksBetweenCalls));
            }

            // 落位之后音频线程又送了几块静音：静音不会让段变少，只会多出空段。
            const auto after = summarise (rig.save());
            expectGreaterOrEqual (after.window + after.pending, summaryA.window + summaryA.pending,
                                  "project A's live profile was not restored (" + describe (after) + ")");
        }

        void testLoadWhilePlayingWithLiveState (const juce::MemoryBlock& projectA, const juce::MemoryBlock& presetD)
        {
            beginTest ("load while playing into a running instance: parameters swap, live profile kept");

            Rig rig (concurrentBlockSize);
            rig.setParameter (param::glazeAmount, 80.0f);
            rig.prepare();
            rig.playMusic (20.0);

            const auto before = summarise (rig.save());
            expect (before.window > 0, "rig precondition: live profile present (" + describe (before) + ")");

            {
                AudioThread audio (rig, true);

                for (int i = 0; i < iterations; ++i)
                {
                    expect (audio.waitForBlocks (blocksBetweenCalls), "audio thread stalled");
                    const auto& blob = (i % 2 == 0) ? presetD : projectA;
                    rig.load (blob);
                    expect (parameterMismatches (*rig.processor, blob).isEmpty(),
                            "load " + juce::String (i) + " did not swap parameters: " + parameterMismatches (*rig.processor, blob).joinIntoString (", "));
                }

                expect (audio.waitForBlocks (blocksBetweenCalls));
            }

            // 预设 D 的画像是空的：读进来的话段数会归零；保留的话只会随着继续播放变多（窗口未满）。
            const auto after = summarise (rig.save());
            expectGreaterOrEqual (after.window + after.pending, before.window + before.pending,
                                  "live profile replaced by a load (before " + describe (before) + ", after " + describe (after) + ")");
            expectGreaterThan (rig.processor->getGlazeStatus().confidence, 0.0f);
        }
    };

    static ProcessorConcurrencyTests processorConcurrencyTests;
}
