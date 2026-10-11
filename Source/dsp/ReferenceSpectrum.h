/*
    只从侧链学、与电平无关的参考频谱。持有者必须传入真正的侧链总线，
    绝不能拿主输入 / 主输出或界面显示用分析器的抽头来顶替。
*/
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace celadon
{
    class ReferenceSpectrum
    {
    public:
        static constexpr int numBands = 27;
        static constexpr double learningSeconds = 10.0;
        enum class State { idle, waitingForSidechain, learning, ready };

        struct Snapshot
        {
            State state = State::idle;
            double seconds = 0.0;
            bool hasReference = false, sidechainConnected = false;
            // 10 log10（1/3 倍频程段功率 / 立体声总功率），取值 [-120, 0] dB。
            // 静音段与不支持的段停在下限；调用方必须按证据门控修正，
            // 不能去提升一个本来就不存在的段。
            std::array<float, numBands> relativeDb {};
        };

        // 与其他 DSP 模块的 prepare 一样，只许在 process 停下时调用。
        // 保留已学参考曲线（代码里叫 committed），但学到一半的进度不续接。
        // 还没被 process 取走的新「学习参考」请求，prepare 之后依然有效。
        void prepare (const juce::dsp::ProcessSpec&);
        void process (const juce::AudioBuffer<float>& sidechain, bool connected) noexcept;
        void requestLearn() noexcept;
        void cancelLearn() noexcept;
        void clear() noexcept;
        Snapshot getSnapshot() const noexcept;
        std::unique_ptr<juce::XmlElement> createStateXml() const;
        // 只许在非音频线程调用：解析可能分配内存。与之并发的状态读取、界面命令和
        // process 都是安全的，包括恢复之后、下一块到来之前就保存。
        void restoreStateXml (const juce::XmlElement*);

    private:
        struct Payload
        {
            Snapshot snapshot {};
            std::uint64_t referenceTicket = 0, controlTicket = 0;
        };

        // 单写者发布。Payload 的全部 32 位字一律是原子量：若只用普通 seqlock 包住一个
        // 非原子结构体，在 C++ 里照样是数据竞争。
        class Publication
        {
        public:
            Publication() noexcept;
            void write (const Payload&) noexcept;
            bool read (Payload&) const noexcept;
        private:
            static constexpr size_t wordCount = (sizeof (Payload) + 3) / 4;
            std::atomic<std::uint64_t> sequence { 0 };
            std::array<std::atomic<std::uint32_t>, wordCount> words;
        };

        static void advanceTicket (std::atomic<std::uint64_t>&, std::uint64_t) noexcept;
        void applyCommands() noexcept;
        void resetLearning() noexcept;
        void publish() noexcept;
        bool readSnapshot (Snapshot&) const noexcept;
        void startAnalysisIfReady() noexcept;
        void advanceAnalysis (int workUnits) noexcept;
        void finishAnalysis (bool valid) noexcept;
        void finishLearning() noexcept;

        // 界面的各个控制各用一张独立、单调递增的票号（ticket）：学习 / 取消合并成只看最新一次时，
        // 绝不能吞掉更早发出的、对已学参考曲线的清除或恢复。
        std::atomic<std::uint64_t> nextTicket { 0 }, learnTicket { 0 },
                                   cancelTicket { 0 }, clearTicket { 0 };
        Publication published, restored;
        // 只串行化非音频线程上的 XML 写者（restoreStateXml）。音频线程从不锁它。
        std::mutex restoreMutex;
        Payload current {};
        std::uint64_t lastAudioControlTicket = 0;
        bool learning = false;
        double sampleRate = 48000.0;
        std::int64_t validSamples = 0, targetSamples = 480000;
        int fftSize = 0, frameFill = 0;
        std::array<std::vector<float>, 2> input, analysisInput, transform;
        // 两组采集缓冲：一帧采齐后，可以一边变换这一帧、一边采下一帧。
        // 一个工作单元是一次固定大小的操作；每个输入样本推进 16 个单元，
        // 保证所支持的最大 FFT（2^17 点）也能在下一帧采齐之前算完。
        enum class AnalysisStage { window, fft, power, bands, accumulate };
        bool analysisPending = false, captureInterrupted = false, requireFullFinalFrame = false;
        AnalysisStage analysisStage = AnalysisStage::window;
        int analysisLength = 0, analysisSuffix = 0, analysisIndex = 0, fftWidth = 2, fftGroup = 0, fftJ = 0;
        int analysisBand = 0, analysisBandBin = 0;
        double analysisEnergy = 0.0, analysisWindowEnergy = 0.0, analysisTotal = 0.0, analysisInBand = 0.0;
        std::array<double, numBands> analysisBands {};
        std::vector<float> window;
        std::vector<int> bitReversal;
        std::vector<float> twiddleReal, twiddleImag;
        std::vector<double> power;
        struct Band { int first = 1, count = 0, offset = 0; };
        std::array<Band, numBands> bands {};
        std::vector<double> weights;
        std::array<double, numBands> accumulatedBands {};
        double accumulatedTotal = 0.0;
    };
}
