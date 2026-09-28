/*
  ==============================================================================

    TruePeak.h
    8× 多相插值真峰值检测（ADR-004）。

    限幅器的峰值检测和表的真峰值用的是同一个类，所以「限幅器压的峰」
    和「表上显示的峰」是同一个算法算出来的。

    原理：把每个输入样本之间插出 7 个点（8 倍过采样），取 8 个点里绝对值最大的。
    插值滤波器是 Kaiser 窗 sinc（每相 64 抽头），截止在原采样率的奈奎斯特频率；
    第 0 相恰好等于原样本本身，所以采样点峰值永远不会被低估。

    为什么不是 BS.1770 的 4×：实测 4× / 24 抽头在 0.4 fs 以上明显低估（Clip 削出来的
    高频尖峰读低 0.4 ～ 1.3 dB）；8× / 64 抽头把这类误差压到约 0.1 dB，代价是延迟 32 样本。

    延迟：每次送进一个样本，返回的是 latencySamples 个样本之前那一段的峰值。

    两种用法（v0.5）：
      - 实时路径（限幅器、两块表）用 processBlock / processBlockMax：一次处理一段样本，
        向量化的核一次算 12 个相邻输出 × 7 个相位，比逐样本快约 20 倍；
      - processSample 是逐样本的参照实现（直接型，7 相 × 64 抽头逐项累加），测试与离线测量用。
    两者算的是同一个滤波器、同一种舍入（每个输出、每个相位都按抽头顺序逐项乘加）、同一种取大（同一条指令），
    结果逐位相同——非规格化数按 IEEE 算或冲零（FTZ / DAZ）都是——可以在同一个检测器上交替调用（DspPartsTests 有逐位对照）。
    前提是编译器不替我们合并乘加（GCC 要关 -ffp-contract，ADR-013）。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

namespace celadon
{
    class TruePeakDetector
    {
    public:
        static constexpr int oversamplingFactor = 8;
        static constexpr int tapsPerPhase = 64;
        static constexpr int latencySamples = tapsPerPhase / 2;

        TruePeakDetector();

        // 分配每个声道的历史缓冲（不在音频线程里调用）。
        void prepare (int numChannels);
        void reset() noexcept;

        // 按块检测：peaks[i] = 送进 input[i] 之后的峰值，与连续调用 numSamples 次 processSample 逐位相同。
        // 任意长度（内部按固定长度切段）；不分配内存；peaks 可以就是 input（原地）。
        void processBlock (int channel, const float* input, float* peaks, int numSamples) noexcept;

        // 按块取最大：返回 max(0, 这段每个样本的峰值)，与逐样本 jmax 折叠逐位相同（NaN 峰值不计入）。
        float processBlockMax (int channel, const float* input, int numSamples) noexcept;

        // 逐样本参照实现：送入一个样本，返回 [n − latency, n − latency + 1) 这一段里 8 个插值点的最大绝对值。
        float processSample (int channel, float x) noexcept;

        // 本平台编进来的是哪种按块核（"NEON" / "SSE2" / "scalar"），测试的性能护栏与日志用。
        static const char* getBlockKernelName() noexcept;

    private:
        struct ChannelState
        {
            // 双写环形缓冲：同一个样本写两份，读的时候永远是一段连续内存，不用取模。
            std::array<float, (size_t) (2 * tapsPerPhase)> history {};
            int writeIndex = 0;
        };

        static constexpr int historyLength = tapsPerPhase - 1;    // 算一个输出要用到的「之前的」样本数
        static constexpr int groupSize = 12;                      // 向量核一次算的输出个数
        static constexpr int chunkLength = 20 * groupSize;        // 按块处理时每段的样本数

        // 把一段（≤ chunkLength）输入接在历史后面算峰值，结果放进 peakScratch[0 … n)，并把历史推进 n 个样本。
        void processChunk (ChannelState& state, const float* input, int numSamples) noexcept;

        // reversedPhases[q][k] 与 window[k] 直接点乘；q 是插值点在两个样本之间的位置（q / 8）。参照实现用。
        std::array<std::array<float, (size_t) tapsPerPhase>, (size_t) oversamplingFactor> reversedPhases {};

        // 同一批系数按抽头重排给向量核用：tapsByTap[8k + p − 1] = reversedPhases[p][k]（p = 1 … 7），每个 k 的第 8 格补 0。
        std::array<float, (size_t) (tapsPerPhase * 8)> tapsByTap {};

        std::vector<ChannelState> channels;

        // 按块处理的工作区（固定大小，不随块长分配）：历史 + 本段输入 + 凑满最后一组的补零；每段的峰值。
        std::array<float, (size_t) (historyLength + chunkLength + groupSize)> line {};
        std::array<float, (size_t) (chunkLength + groupSize)> peakScratch {};
    };
}
