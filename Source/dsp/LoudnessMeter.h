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
      - T-032（2026-09-29，A4）：
          · setIntegrationPaused（R8-03）：整体响度的积分可以暂停——链在听原声期间暂停 OUT 表，整体响度只积分母带（宿主旁通接在
            同一个开关上、口径待 Max 确认，见 CeladonChain.h 的 pauseIntegrationOnHostBypass）。
            暂停时 K 加权、子块环、processedSamples、短期、真峰值照常，只是门块不进直方图、整体读数不动。按子块排除：
            只要有一个样本是在暂停中处理的，这个 100 ms 子块就算「被暂停碰过」（含暂停开始 / 结束那一块里跨边界的半截子块），
            门块要 4 个连续的、没被碰过的子块才成立——恢复后前 3 个门块不会混进干路的子块。偏保守的一处：暂停中处理的那一块恰好
            在子块边界上结束时，紧接着的那个子块（一个暂停中的样本也没有）也按碰过算，恢复后多等 100 ms，暂停中的样本照样一个都漏不进；
          · 非有限值自愈（R10-08）：一个 NaN 进了 K 加权状态会永久留在里面（转置 II 型状态与 NaN 相乘还是 NaN），此后每个子块
            都是 NaN——改前短期响度从此一直是「无读数」、整体响度冻结在出事前的值（NaN 能量经 energyToLufs 成了 noReading，被绝对门
            挡在直方图外；+Inf 能量则让 floor 转 int 成了未定义行为），GLAZE 分析与 Listen 停摆（改前改后对照实测见 IMPL-A4-chain.md）。
            现在子块能量非有限时只清掉状态已坏的声道、丢掉这个子块（也断开门块的连续性），短期窗口与直方图永不吃非有限值；
            子块环那边同样自愈（该子块按现有约定仍以非有限 meanSquare 发布，读者本来就跳过它）；
          · updateIntegrated 两遍扫描只扫到直方图里有数的最高一格（R7-04）：多扫的格子都是 0.0 / 0，读数逐位不变。两遍合计从
            1600 次格访问降到（最高格 + 1）+（最高格 − 相对门所在格 + 1），典型母带（最高格约 −4 LUFS、相对门约 −20 LUFS）约 820 次。
          有限输入、从未暂停时，表的读数、子块环与改动前逐位相同。

    音频线程只调 process()、reset() 与 setIntegrationPaused()；界面线程只读原子变量，不持锁、不阻塞音频线程。

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

        // 暂停 / 恢复整体响度的积分（音频线程，与 process 串行；链在每块的 setParams 里调，按整块生效）。暂停期间：K 加权滤波器、
        // 短期窗口、子块环与样本计数、真峰值都照常推进，只是 400 ms 门块不进直方图、整体读数停在暂停前的值；任何一个样本在暂停中
        // 处理过的子块都不进门块（暂停中的那一块恰好在子块边界结束时，下一个子块也按碰过算——偏保守），恢复后要攒够 4 个完全在
        // 暂停之外的子块才再成门块。默认不暂停；从未暂停过时读数逐位同 T-032 之前。
        void setIntegrationPaused (bool shouldPause) noexcept            { integrationPaused = shouldPause; }
        bool isIntegrationPaused() const noexcept                        { return integrationPaused; }

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

        // K 加权的两级系数（T-043：修坯峰值守护的混合损失按与本表同一口径的 K 加权量，ClayModule 借用）。prepare 用的就是它，
        // 表的读数逐位不变。采样率须 > 0。
        struct Biquad
        {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };

        static void designKWeighting (double fs, Biquad& shelfOut, Biquad& highpassOut) noexcept;

        static double kWeight (double x, const Biquad& f, double& s1, double& s2) noexcept
        {
            // 转置直接 II 型双二阶，double 精度（38 Hz 高通在 float 下误差偏大）。
            const auto y = f.b0 * x + s1;
            s1 = f.b1 * x - f.a1 * y + s2;
            s2 = f.b2 * x - f.a2 * y;
            return y;
        }

    private:
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
        void clearFilterStates() noexcept;
        void healFilterStates() noexcept;
        void finishSubBlock() noexcept;
        void updateIntegrated() noexcept;
        void measureLoudness (const float* const* channels, int channelsToUse, int startSample, int numSamples) noexcept;
        void restartSubBlockGrid() noexcept;
        void publishSubBlock() noexcept;

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
        int histogramTopBin = -1;          // 直方图里有数的最高一格（−1 = 空）：两遍扫描只扫到它（T-032 R7-04）

        // 整体响度积分的暂停（T-032 R8-03）：暂停时门块不进直方图。pauseTouchedSubBlock = 正在累计的这个子块里有样本是在暂停中
        // 处理的；cleanSubBlocksInRow = 最近连续几个子块完全在暂停之外、也没因非有限值被丢掉（封顶 gatingSubBlocks）——门块要 4 个。
        bool integrationPaused = false;
        bool pauseTouchedSubBlock = false;
        int cleanSubBlocksInRow = gatingSubBlocks;   // 从未暂停过 = 早已攒够

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
