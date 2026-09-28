/*
  ==============================================================================

    LoudnessMeter.h
    ITU-R BS.1770-4 响度表 + 真峰值。

      - K 加权：高搁架预滤波 + RLB 高通，系数按采样率由模拟原型换算（48 kHz 时与标准表一致）；
      - 短期响度：最近 3 s；
      - 整体响度：400 ms 门块、75% 重叠，−70 LUFS 绝对门 + −10 LU 相对门（0.1 LU 直方图，内存固定）；
      - 真峰值：与限幅器同一个 TruePeakDetector（按块算，与逐样本逐位相同）；
      - 单声道按「左右相同的双声道」计量（设计规格-总体），能量计两次，读数与立体声一致；
      - 可以只算真峰值（setLoudnessEnabled (false)）：IN 表只显示真峰值，省掉 K 加权与 LUFS 统计；
      - v0.6：完成的 100 ms 子块（K 加权均方 + 子块真峰值 + 样本时间戳）放进固定容量的环形缓冲，给 GLAZE 的
        分析器与校准器在音频线程上读（设计规格-GLAZE-分析器第 2 节）。只读信号、只多存一份统计，表的读数逐位不变。

    音频线程只调 process() 与 reset()；界面线程只读原子变量，不持锁、不阻塞音频线程。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include "TruePeak.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace celadon
{
    class LoudnessMeter
    {
    public:
        // 「没有读数」时返回的值（静音 / 还没攒够数据 / 响度计量关着）。
        static constexpr float noReading = -100.0f;

        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        // 音频线程：只读 buffer，不改它。块长不受 prepare 时的块大小限制。
        void process (const juce::AudioBuffer<float>& buffer);

        // 开 / 关响度计量（默认开）。关掉时只算真峰值：跳过 K 加权与短期 / 整体统计，两个 LUFS 读数恒为 noReading。
        // 任何线程都可以调；音频线程在下一次 process() 开头切换，切换时响度统计从头开始（重新打开后从零累计）。
        void setLoudnessEnabled (bool shouldMeasureLoudness) noexcept   { loudnessEnabled.store (shouldMeasureLoudness); }
        bool isLoudnessEnabled() const noexcept                         { return loudnessEnabled.load(); }

        // ---- 以下由界面线程调用 ----
        float getShortTermLufs() const noexcept    { return isLoudnessEnabled() ? shortTermLufs.load (std::memory_order_relaxed) : noReading; }
        float getIntegratedLufs() const noexcept   { return isLoudnessEnabled() ? integratedLufs.load (std::memory_order_relaxed) : noReading; }

        // 自上次读取以来的最大真峰值（dBTP），读完清零。
        float getAndResetTruePeakDb() noexcept;

        // 请求清空整体响度与短期窗口；下一次 process() 开头在音频线程里执行。
        void requestReset() noexcept               { resetRequested.store (true); }

        // ---- 以下由音频线程（调用 process 的线程）调用：v0.6 GLAZE 的分析器与校准器读子块（设计规格-GLAZE-分析器第 2 节）----

        // 一个完成的 100 ms 子块。时间戳 = 自上次 reset() 以来处理过的样本数（requestReset 不清它），链里各表同一时间轴。
        //
        // 子块网格：从 reset()（或重新打开响度计量）起每 getSubBlockLength() 个样本一个，与表自己的读数网格互不牵连——
        // 界面的 requestReset 只清表的读数（短期窗口、整体直方图），不打断子块序列，所以分析器的 M / S 窗口与校准器
        // 两块表的配对不会因为有人点了 Reset 而断开。响度计量关着时不产生子块；重新打开时网格从那一刻重新起
        // （K 加权滤波器状态也是那一刻清的），相邻两个子块的 endSample 之差不等于子块长度 = 中间有空档。
        struct SubBlock
        {
            double meanSquare = 0.0;        // K 加权均方（各声道之和，与 LUFS 同口径：LUFS = −0.691 + 10·log10）
            float truePeakLinear = 0.0f;    // 本子块内的真峰值最大值（线性）。按子块边界切段交给表的 8× 检测器取段最大；
                                            // 检测器本身有 TruePeakDetector::latencySamples（32）个样本的延迟，描述的是早 32 个样本的那一段
            std::int64_t endSample = 0;     // 处理完本子块最后一个样本时的累计样本数：本子块覆盖 [endSample − 子块长度, endSample)
        };

        static constexpr int subBlockRingCapacity = 128;   // ≥ 12.8 s，远大于任何宿主块

        // 读 cursor 之后新完成的子块，最多 maxCount 个，并把 cursor 推进到已读位置。读者落后超过容量时跳到最旧可用的子块，
        // 返回值取负（-读出的个数）表示中间丢过。cursor 从 0 起（cursor 是子块序号，不是样本数）。响度计量关着时不产生子块。
        // cursor 比已完成的子块数还大（表被 reset 过、读者没跟着清）时从头读，也按「丢过」报告。
        int readSubBlocksSince (std::int64_t& cursor, SubBlock* out, int maxCount) const noexcept;

        // 自上次 reset() 以来处理过的样本数。
        std::int64_t getProcessedSamples() const noexcept;

        // 上一次 process() 这一块的真峰值最大值（线性）；与界面读的 getAndResetTruePeakDb 互不影响。
        float getLastBlockTruePeakLinear() const noexcept;

        // 子块长度（样本）= round(0.1 × 采样率)；prepare 之前是 4800。（v0.6 A1 追加：分析器按它折算时长与窗口。）
        int getSubBlockLength() const noexcept   { return subBlockLength; }

    private:
        struct Biquad
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        struct ChannelState
        {
            double shelfS1 = 0.0, shelfS2 = 0.0;
            double highpassS1 = 0.0, highpassS2 = 0.0;
        };

        static constexpr int shortTermSubBlocks = 30;          // 30 × 100 ms = 3 s
        static constexpr int gatingSubBlocks = 4;              // 4 × 100 ms = 400 ms 门块
        static constexpr float histogramMinLufs = -70.0f;
        static constexpr int histogramBinsPerLu = 10;
        static constexpr int histogramBins = 80 * histogramBinsPerLu;   // −70 ～ +10 LUFS

        void clearMeasurement() noexcept;
        void finishSubBlock() noexcept;
        void updateIntegrated() noexcept;
        void measureLoudness (const float* const* channels, int channelsToUse, int startSample, int numSamples) noexcept;
        void restartSubBlockGrid() noexcept;
        void publishSubBlock() noexcept;

        static double kWeight (double x, const Biquad& f, double& s1, double& s2) noexcept
        {
            // 转置直接 II 型双二阶，double 精度（38 Hz 高通在 float 下误差偏大）。
            const auto y = f.b0 * x + s1;
            s1 = f.b1 * x - f.a1 * y + s2;
            s2 = f.b2 * x - f.a2 * y;
            return y;
        }

        double sampleRate = 0.0;
        int numChannels = 0;

        Biquad shelf, highpass;
        std::vector<ChannelState> channelStates;
        TruePeakDetector truePeak;

        int subBlockLength = 4800;         // 100 ms 的样本数
        int samplesInSubBlock = 0;
        double subBlockEnergy = 0.0;       // 当前 100 ms 内 Σ(各声道 K 加权样本²)

        std::array<double, (size_t) shortTermSubBlocks> recentSubBlocks {};
        int recentWriteIndex = 0;
        int recentCount = 0;

        std::array<double, (size_t) histogramBins> histogramEnergy {};
        std::array<juce::uint32, (size_t) histogramBins> histogramCount {};

        std::atomic<float> shortTermLufs { noReading };
        std::atomic<float> integratedLufs { noReading };
        std::atomic<float> truePeakLinear { 0.0f };
        std::atomic<bool> resetRequested { false };

        std::atomic<bool> loudnessEnabled { true };   // 目标状态（任何线程写）
        bool loudnessActive = true;                   // 音频线程上当前生效的状态

        // ---- v0.6 GLAZE 子块环形缓冲（音频线程写、音频线程读，不跨线程）----
        std::array<SubBlock, (size_t) subBlockRingCapacity> subBlockRing {};
        std::int64_t subBlocksWritten = 0;   // 自 reset 以来完成的子块数（= 下一个子块的序号）
        std::int64_t processedSamples = 0;   // 自 reset 以来 process() 处理过的样本数
        int ringSamplesInSubBlock = 0;       // 当前子块已累计的样本数（网格见 SubBlock 的注释）
        double ringEnergy = 0.0;             // 当前子块 Σ(各声道 K 加权样本²)：与 subBlockEnergy 同一个算式，只是网格不随 requestReset 重启
        float ringPeak = 0.0f;               // 当前子块的真峰值最大值（线性）
        float lastBlockPeak = 0.0f;          // 上一次 process() 这一块的真峰值最大值（线性）
    };
}
