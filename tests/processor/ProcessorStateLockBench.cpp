/*
  ==============================================================================

    ProcessorStateLockBench.cpp
    存读档持回调锁的时间（T-099，ADR-040）：按最坏的画像大小量。第一版锁里整份编码（播放中存档让音频线程等约 138 ms），
    T-114 第二波拆成「锁里只拷快照 / 换入暂存，编码与解档在锁外」，期望等锁最长 < 一块的时长（64 样本 = 1.33 ms）、不掉块。

    最坏画像 = 实时画像的回看窗口满了（LiveProfile::windowCapacity = 512 段，120 BPM 一段一小节 2 s，约 17 分钟）：
    先放 1084 s 合成音乐把窗口填满（再多放 60 s，让最老的段开始被挤出），这时 LIVE 存档最大。

    量四样（都打印成「T099-BENCH …」一行，结论写进 docs/validation/2026-11-13-T099-state-lock）：
      1. 单线程存档一次的耗时与存档大小（整份二进制、GLAZE 节点、解开后的 LIVE）；
      2. 播放中存档：音频线程按真实时间节拍（48 kHz / 64 样本一块 = 1.33 ms）送块，主线程每 100 ms 存一次档，
         记音频线程等锁的最长时间、单块（等锁 + 处理）最长时间、误了截止时刻的块数；先空跑同样长的时间作对照；
      3. 播放中第一次读档（换入暂存那条路）：实例只送过静音，读最坏画像的存档，同样记等锁时间；
      4. 运行中的实例读档（只判断、不解档）：同样记等锁时间。
    输出格式（各行「T099-BENCH …」）拆锁前后不变，主会话照旧 grep。

    只在 --bench 时跑（类别 CeladonProcessorBench）：约 1 分钟，要独占机器数才准，不进 CI、不在 TSan 下跑。

  ==============================================================================
*/

#include "ProcessorHarness.h"

#include <algorithm>
#include <vector>

namespace celadon::processortest
{
    namespace
    {
        constexpr int pacedBlockSize = 64;
        constexpr int callsPerPhase = 10;
        constexpr auto gapBetweenCalls = std::chrono::milliseconds (100);

        double millisecondsSince (std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
        }

        juce::String statsOf (std::vector<double> values)
        {
            if (values.empty())
                return "-";

            std::sort (values.begin(), values.end());
            return "min " + juce::String (values.front(), 3) + " / median " + juce::String (values[values.size() / 2], 3)
                   + " / max " + juce::String (values.back(), 3) + " ms";
        }

        juce::String audioReport (const AudioThread& audio)
        {
            return "audio lock wait max " + juce::String (audio.maxLockWaitMs(), 3) + " ms, block total max "
                   + juce::String (audio.maxBlockTotalMs(), 3) + " ms, missed deadlines " + juce::String (audio.missedDeadlines())
                   + " (block " + juce::String (pacedBlockSize) + " samples = " + juce::String (1000.0 * pacedBlockSize / sampleRate, 3) + " ms)";
        }
    }

    class ProcessorStateLockBench final : public juce::UnitTest
    {
    public:
        ProcessorStateLockBench() : juce::UnitTest ("ProcessorStateLockBench", "CeladonProcessorBench") {}

        void runTest() override
        {
            beginTest ("worst-case live profile: look-back window full");

            const auto fillSeconds = glaze::LiveProfile::windowCapacity * 2.0 + 60.0;
            juce::MemoryBlock worst;

            {
                Rig filler;
                filler.setParameter (param::glazeAmount, 100.0f);
                filler.prepare();
                const auto start = std::chrono::steady_clock::now();
                filler.playMusic (fillSeconds);
                logMessage ("T099-BENCH fill: " + juce::String (fillSeconds, 0) + " s of audio in " + juce::String (millisecondsSince (start) / 1000.0, 1) + " s");

                std::vector<double> saveMs;

                for (int i = 0; i < callsPerPhase; ++i)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    worst = filler.save();
                    saveMs.push_back (millisecondsSince (t0));
                }

                logMessage ("T099-BENCH save (single thread, whole getStateInformation): " + statsOf (saveMs));
            }

            const auto summary = summarise (worst);
            expectEquals (summary.window, glaze::LiveProfile::windowCapacity, describe (summary));

            {
                const auto xml = stateXml (worst);
                const auto* glazeNode = xml != nullptr ? xml->getChildByName (glaze::stateTag) : nullptr;
                const auto* liveNode = glazeNode != nullptr ? glazeNode->getChildByName (glaze::liveTag) : nullptr;
                const auto body = liveNode != nullptr ? glaze::decompressLiveBody (*liveNode) : nullptr;

                logMessage ("T099-BENCH size: state blob " + juce::String ((juce::int64) worst.getSize()) + " bytes, GLAZE node "
                            + juce::String ((juce::int64) summary.glazeXml.getNumBytesAsUTF8()) + " bytes, LIVE body uncompressed "
                            + juce::String (body != nullptr ? (juce::int64) body->toString().getNumBytesAsUTF8() : (juce::int64) -1) + " bytes, "
                            + describe (summary));
            }

            // 一台按小块跑的实例，带着最坏画像（第一次读档：读回存档里的画像），之后几项都在它上面量。
            Rig paced (pacedBlockSize);
            paced.load (worst);
            paced.prepare();
            expectEquals (summarise (paced.save()).window, glaze::LiveProfile::windowCapacity);

            beginTest ("control: playing, no save / load");
            {
                AudioThread audio (paced, true, true);
                std::this_thread::sleep_for (gapBetweenCalls * callsPerPhase);
                audio.stop();
                logMessage ("T099-BENCH idle: " + audioReport (audio));
            }

            beginTest ("save while playing (worst-case profile)");
            {
                AudioThread audio (paced, true, true);
                std::vector<double> callMs;

                for (int i = 0; i < callsPerPhase; ++i)
                {
                    std::this_thread::sleep_for (gapBetweenCalls);
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto block = paced.save();
                    callMs.push_back (millisecondsSince (t0));
                    expect (block.getSize() > 0);
                }

                audio.stop();
                logMessage ("T099-BENCH save while playing: getStateInformation " + statsOf (callMs) + "; " + audioReport (audio));
            }

            beginTest ("load into a running instance (decide only, nothing restored)");
            {
                const auto presetD = makePresetD (pacedBlockSize);
                AudioThread audio (paced, true, true);
                std::vector<double> callMs;

                for (int i = 0; i < callsPerPhase; ++i)
                {
                    std::this_thread::sleep_for (gapBetweenCalls);
                    const auto t0 = std::chrono::steady_clock::now();
                    paced.load (presetD);
                    callMs.push_back (millisecondsSince (t0));
                }

                audio.stop();
                logMessage ("T099-BENCH load into running instance: setStateInformation " + statsOf (callMs) + "; " + audioReport (audio));
            }

            beginTest ("first load while playing (restore path, worst-case profile)");
            {
                std::vector<double> callMs, waitMs;
                int missed = 0;

                // 每次一台新实例：只送静音（不算运行中），读最坏画像的存档时走落位那条路。
                for (int i = 0; i < 5; ++i)
                {
                    Rig fresh (pacedBlockSize);
                    fresh.prepare();
                    AudioThread audio (fresh, false, true);
                    std::this_thread::sleep_for (gapBetweenCalls);
                    const auto t0 = std::chrono::steady_clock::now();
                    fresh.load (worst);
                    callMs.push_back (millisecondsSince (t0));
                    std::this_thread::sleep_for (gapBetweenCalls);
                    audio.stop();
                    waitMs.push_back (audio.maxLockWaitMs());
                    missed += audio.missedDeadlines();
                }

                logMessage ("T099-BENCH first load while playing: setStateInformation " + statsOf (callMs) + "; audio lock wait "
                            + statsOf (waitMs) + ", missed deadlines " + juce::String (missed) + " over 5 loads");
            }
        }
    };

    static ProcessorStateLockBench processorStateLockBench;
}
