/*
  ==============================================================================

    ProcessorHarness.h
    处理器层测试（celadon_processor_tests，T-099）的公共台架：一台真的 CeladonAudioProcessor、走带中的宿主、合成音乐、
    按宿主包装层的样子在另一个线程里跑音频，以及读存档里 GLAZE 那一段的小工具。

    「宿主的样子」照 JUCE 9.0.2 的插件包装层：VST3（juce_audio_plugin_client_VST3.cpp processAudio）、AU（_AU_1.mm processBlock）、
    AAX、Standalone（AudioProcessorPlayer）处理每一块时都持着 getCallbackLock()。下面 AudioThread 每块照样拿这把锁，
    存读档在别的线程同时调——这正是 T-099 要守住的并发；不拿锁的话测试就测不到处理器在锁上的互斥。

    台架不建编辑器，不跑消息循环（处理器与 APVTS 的计时器起了也不会被派发），不依赖显示器。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "../../Source/PluginProcessor.h"
#include "../../Source/dsp/GlazeProfile.h"
#include "../../Source/dsp/LiveProfile.h"
#include "../TestHelpers.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

namespace celadon::processortest
{
    inline constexpr double sampleRate = 48000.0;
    inline constexpr int defaultBlockSize = 512;

    // LiveProfile 存档节点的标签（LiveProfile.cpp 的 liveStateTag；存档格式的一部分，改名等于改格式）。
    inline constexpr const char* liveProfileTag = "LIVE_PROFILE";

    //==============================================================================
    // 走带中的宿主：每块报「在播」、样本位置、120 BPM 4/4（GLAZE 的段落账按小节分段，一小节 2 s）。
    // 只在处理音频的那个线程上读写（getPosition 由 processBlock 调，advance 在 processBlock 之后调）。
    class PlayingHost final : public juce::AudioPlayHead
    {
    public:
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            const auto seconds = (double) position / sampleRate;
            const auto ppq = seconds * bpm / 60.0;
            info.setIsPlaying (true);
            info.setTimeInSamples (position);
            info.setTimeInSeconds (seconds);
            info.setBpm (bpm);
            info.setTimeSignature (TimeSignature {});   // 4/4
            info.setPpqPosition (ppq);
            info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            return info;
        }

        void advance (int numSamples) noexcept   { position += numSamples; }

    private:
        static constexpr double bpm = 120.0;
        std::int64_t position = 0;
    };

    //==============================================================================
    // 8 s 的合成音乐循环（120 BPM）：失谐 pad（TestHelpers 的和弦段落）+ 每拍一记 55 Hz 底鼓 + 衰减很快的噪声镲。
    // 只为让 GLAZE 的分析器认成「有效音频」、实时画像攒得起段；不追求像哪种曲风。第一次调用时生成，之后只读。
    inline const juce::AudioBuffer<float>& musicLoop()
    {
        static const juce::AudioBuffer<float> loop = []
        {
            constexpr double seconds = 8.0;
            const auto pad = test::makeDetunedPad (sampleRate, seconds, 2.0, 0.05);
            const auto numSamples = (int) pad.front().size();
            const auto noise = test::makeNoise (2, numSamples, 1.0f, 0x7099);
            const auto beatSamples = (int) (sampleRate * 0.5);
            juce::AudioBuffer<float> out (2, numSamples);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < numSamples; ++i)
                {
                    const auto sinceBeat = (double) (i % beatSamples) / sampleRate;
                    const auto kick = 0.5 * std::exp (-sinceBeat * 18.0) * std::sin (juce::MathConstants<double>::twoPi * 55.0 * sinceBeat);
                    const auto hat = 0.08 * std::exp (-sinceBeat * 60.0) * (double) noise[(size_t) ch][(size_t) i];
                    out.setSample (ch, i, (float) ((double) pad[(size_t) ch][(size_t) i] + kick + hat));
                }

            return out;
        }();

        return loop;
    }

    // 循环播放 musicLoop()；每个使用者一份读位置。
    class MusicSource
    {
    public:
        void render (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            const auto& loop = musicLoop();
            const auto length = loop.getNumSamples();

            for (int i = 0; i < numSamples; ++i)
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.setSample (ch, i, loop.getSample (ch % loop.getNumChannels(), cursor));

                cursor = (cursor + 1) % length;
            }
        }

    private:
        int cursor = 0;
    };

    //==============================================================================
    // 一台处理器 + 它的宿主时间线与缓冲。单线程用：直接 playMusic / playSilence；多线程用：交给 AudioThread 跑，
    // 期间主线程只许调处理器的公开接口（存读档、读参数），不许碰 host / buffer / music。
    struct Rig
    {
        explicit Rig (int blockSizeToUse = defaultBlockSize)
            : blockSize (blockSizeToUse), buffer (2, blockSizeToUse)
        {
            processor = std::make_unique<CeladonAudioProcessor>();
            processor->setPlayHead (&host);
        }

        ~Rig()
        {
            processor->setPlayHead (nullptr);
        }

        // 宿主的 prepare 顺序（setRateAndBufferSizeDetails 由包装层先调，再 prepareToPlay）：立体声进出，参考侧链关着（默认）。
        void prepare()
        {
            processor->setPlayConfigDetails (2, 2, sampleRate, blockSize);
            processor->prepareToPlay (sampleRate, blockSize);
        }

        void setParameter (const char* id, float value)
        {
            auto* parameter = processor->getParameters().getParameter (id);
            jassert (parameter != nullptr);
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        }

        float parameter (const char* id) const
        {
            return processor->getParameters().getRawParameterValue (id)->load();
        }

        // 处理一块：照包装层持着回调锁调 processBlock，再推进宿主时间线。
        void processOneBlock()
        {
            {
                const juce::ScopedLock callback (processor->getCallbackLock());
                processor->processBlock (buffer, midi);
            }

            host.advance (blockSize);
        }

        void playMusic (double seconds)
        {
            for (auto done = 0.0; done < seconds; done += (double) blockSize / sampleRate)
            {
                music.render (buffer, blockSize);
                processOneBlock();
            }
        }

        void playSilence (double seconds)
        {
            for (auto done = 0.0; done < seconds; done += (double) blockSize / sampleRate)
            {
                buffer.clear();
                processOneBlock();
            }
        }

        juce::MemoryBlock save()
        {
            juce::MemoryBlock block;
            processor->getStateInformation (block);
            return block;
        }

        void load (const juce::MemoryBlock& block)
        {
            processor->setStateInformation (block.getData(), (int) block.getSize());
        }

        const int blockSize;
        std::unique_ptr<CeladonAudioProcessor> processor;
        PlayingHost host;
        MusicSource music;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
    };

    //==============================================================================
    // 另一个线程里按宿主的样子连续处理音频（每块持回调锁）。paced = 按真实时间节拍送块（基准量音频线程等锁的时间用）；
    // 否则尽快送（并发用例要的是两个线程尽量多地交错）。music 可以在运行中切换（静音 ↔ 音乐）。
    class AudioThread
    {
    public:
        AudioThread (Rig& rigToDrive, bool startWithMusic, bool pacedToRealTime = false)
            : rig (rigToDrive), paced (pacedToRealTime)
        {
            music.store (startWithMusic);
            worker = std::thread ([this] { run(); });
        }

        ~AudioThread()   { stop(); }

        void stop()
        {
            stopRequested.store (true);

            if (worker.joinable())
                worker.join();
        }

        void setMusic (bool shouldPlayMusic) noexcept   { music.store (shouldPlayMusic); }
        std::uint64_t blocks() const noexcept           { return processed.load(); }

        // 等音频线程从此刻起再处理 count 块（超时 30 s 返回 false：音频线程卡住也不让测试挂死）。
        bool waitForBlocks (std::uint64_t count) const
        {
            const auto target = blocks() + count;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);

            while (blocks() < target)
            {
                if (std::chrono::steady_clock::now() > deadline)
                    return false;

                std::this_thread::yield();
            }

            return true;
        }

        // 停下之后才读（join 之后由主线程读，不另加同步）。
        double maxLockWaitMs() const noexcept      { return maxWaitMs; }
        double maxBlockTotalMs() const noexcept    { return maxTotalMs; }
        int missedDeadlines() const noexcept       { return missed; }

    private:
        void run()
        {
            using clock = std::chrono::steady_clock;
            const auto period = std::chrono::duration<double> ((double) rig.blockSize / sampleRate);
            auto nextDeadline = clock::now();

            while (! stopRequested.load())
            {
                if (music.load())
                    rig.music.render (rig.buffer, rig.blockSize);
                else
                    rig.buffer.clear();

                const auto start = clock::now();

                {
                    const juce::ScopedLock callback (rig.processor->getCallbackLock());
                    const auto locked = clock::now();
                    rig.processor->processBlock (rig.buffer, rig.midi);
                    maxWaitMs = juce::jmax (maxWaitMs, std::chrono::duration<double, std::milli> (locked - start).count());
                }

                const auto finished = clock::now();
                const auto totalMs = std::chrono::duration<double, std::milli> (finished - start).count();
                maxTotalMs = juce::jmax (maxTotalMs, totalMs);
                rig.host.advance (rig.blockSize);
                processed.fetch_add (1);

                if (paced)
                {
                    // 截止时刻 = 上一块的截止时刻 + 一块的时长：这一块从开始等锁到处理完超过一块的时长，就是真宿主里的一次掉块。
                    nextDeadline += std::chrono::duration_cast<clock::duration> (period);

                    if (finished > nextDeadline)
                    {
                        ++missed;
                        nextDeadline = finished;
                    }
                    else
                    {
                        std::this_thread::sleep_until (nextDeadline);
                    }
                }
            }
        }

        Rig& rig;
        const bool paced;
        std::atomic<bool> music { false }, stopRequested { false };
        std::atomic<std::uint64_t> processed { 0 };
        double maxWaitMs = 0.0, maxTotalMs = 0.0;
        int missed = 0;
        std::thread worker;
    };

    //==============================================================================
    // 存档里 GLAZE 那一段的摘要：GLAZE_STATE 节点原文（比较「整份没动」用），以及解开 LIVE 之后实时画像的段数。
    struct GlazeSummary
    {
        bool hasGlazeNode = false;
        juce::String glazeXml;        // GLAZE_STATE 节点的原文
        bool decoded = false;         // LIVE 解得开、里面有 LIVE_PROFILE
        int window = 0;               // 已发布那一份画像的回看窗口段数
        int pending = 0;              // 已交来、还没发布的段
        bool restorable = false;      // LIVE_PROFILE 交给一份新的 LiveProfile 读得回来（读不回 = 下次打开整份冷启动）
    };

    inline std::unique_ptr<juce::XmlElement> stateXml (const juce::MemoryBlock& block)
    {
        return juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    }

    inline juce::MemoryBlock toBlock (const juce::XmlElement& xml)
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (xml, block);
        return block;
    }

    // checker 非空时顺带核对 LIVE_PROFILE 读不读得回（LiveProfile 构造要分配约 2.3 MB，多份存档共用一个）。
    inline GlazeSummary summarise (const juce::MemoryBlock& block, glaze::LiveProfile* checker = nullptr)
    {
        GlazeSummary summary;
        const auto xml = stateXml (block);

        if (xml == nullptr)
            return summary;

        const auto* glazeNode = xml->getChildByName (glaze::stateTag);

        if (glazeNode == nullptr)
            return summary;

        summary.hasGlazeNode = true;
        summary.glazeXml = glazeNode->toString();

        const auto* liveNode = glazeNode->getChildByName (glaze::liveTag);
        const auto body = liveNode != nullptr ? glaze::decompressLiveBody (*liveNode) : nullptr;
        const auto* profile = body != nullptr ? body->getChildByName (liveProfileTag) : nullptr;

        if (profile == nullptr)
            return summary;

        summary.decoded = true;
        summary.window = profile->getIntAttribute ("window", -1);
        summary.pending = profile->getIntAttribute ("pending", -1);

        if (checker != nullptr)
        {
            checker->prepare (sampleRate);
            summary.restorable = checker->restoreStateXml (*profile);
        }

        return summary;
    }

    inline juce::String describe (const GlazeSummary& s)
    {
        return "window " + juce::String (s.window) + " + pending " + juce::String (s.pending) + (s.decoded ? "" : " (LIVE body not decodable)");
    }

    //==============================================================================
    // 两份互不相同的存档：「工程 A」放过 24 s 音乐、实时画像里有段；「预设 D」参数不同、没放过音频（实时画像是空的）。
    // Lookahead 两边相同：并发用例里来回换档时延迟不变，不牵扯延迟重报。
    inline void applyProjectA (Rig& rig)
    {
        rig.setParameter (param::glazeAmount, 62.0f);
        rig.setParameter (param::glazeStyle, 1.0f);
        rig.setParameter (param::colorTone, 1.5f);
        rig.setParameter (param::balanceRecover, 20.0f);
        rig.setParameter (param::limitLookahead, 3.5f);
    }

    inline void applyPresetD (Rig& rig)
    {
        rig.setParameter (param::glazeAmount, 20.0f);
        rig.setParameter (param::glazeStyle, 2.0f);
        rig.setParameter (param::colorTone, -1.0f);
        rig.setParameter (param::balanceRecover, 40.0f);
        rig.setParameter (param::limitLookahead, 3.5f);
    }

    inline juce::MemoryBlock makeProjectA (int blockSize = defaultBlockSize)
    {
        Rig rig (blockSize);
        applyProjectA (rig);
        rig.prepare();
        rig.playMusic (24.0);
        return rig.save();
    }

    inline juce::MemoryBlock makePresetD (int blockSize = defaultBlockSize)
    {
        Rig rig (blockSize);
        applyPresetD (rig);
        rig.prepare();
        return rig.save();
    }

    //==============================================================================
    // 存档里每个 PARAM 节点的值与处理器此刻的原始值逐个核对（APVTS 存的就是原始值）。返回对不上的 id（空 = 全对上）。
    inline juce::StringArray parameterMismatches (CeladonAudioProcessor& processor, const juce::MemoryBlock& block)
    {
        juce::StringArray mismatches;
        const auto xml = stateXml (block);

        if (xml == nullptr)
            return { "<state not decodable>" };

        for (const auto* node : xml->getChildWithTagNameIterator ("PARAM"))
        {
            const auto id = node->getStringAttribute ("id");
            const auto* raw = processor.getParameters().getRawParameterValue (id);

            if (raw == nullptr || std::abs ((double) raw->load() - node->getDoubleAttribute ("value")) > 1.0e-3)
                mismatches.add (id);
        }

        return mismatches;
    }
}
