/*
  ==============================================================================

    LinearPhaseLowpass.h
    线性相位低通：4 个长度 L 的滑动平均级联（ADR-012，设计规格-Clay-根治修订 第 2 节）。
    Clay 的线性相位收单（S' = S 延迟 D − a · LP(S) 再对齐延迟）与分析器「先试后用」的收单候选共用这一个实现。

    - 冲激响应 = 4 个长度 L 的矩形窗卷积（三次 B 样条形），长度 4(L − 1) + 1，关于中点对称 → 严格线性相位；
      群延迟 groupDelayFor(L) = 2(L − 1) 个样本。直流增益 1。
    - L = lengthFor(fs, fc) = max(2, round(0.24 · fs / fc))：低通 −3 dB 点约在 fc；互补高通（1 − LP）在 fc/2、fc、2fc、4fc 处
      约 −20.8 / −9.9 / −1.9 / 0 dB（与采样率无关），低频斜率 12 dB/oct。
    - 每级是 double 滑动和：sum += x[n] − x[n−L]，y[n] = sum · (1/L)（与 y[n] = y[n−1] + (x[n] − x[n−L]) / L 数学相同，取负精确对称），
      每级一个长度 L 的输入历史环；输入先清洗（非有限按 0、|x| > 1e4 钳位，永远不会卡死在 NaN）。为消除累积舍入误差，
      从 reset 起每 resyncIntervalSamples 个样本（固定网格，与块长无关）按历史精确重算一次各级滑动和——同一输入序列、
      任意切块，输出逐位相同。前提：编译器不合并乘加（ADR-013：GCC 全工程 -ffp-contract=off，任何构建不许 -ffast-math / -Ofast）。
    - 状态全零 = 「此前输入全是 0」。prepare 按最大长度分配；reset / process 不分配、不加锁。
      reset 是 O(1)（T-020）：不清历史环，之后的头 length 个样本把「length 个样本之前的输入」当 0，与清零后读出逐位相同——
      Clay 切换分频点时不必在一块里清 4 × L 个 double。
    只依赖 juce_core（数学与断言），可无头编译测试。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

namespace celadon
{
    class LinearPhaseLowpass
    {
    public:
        static constexpr int numStages = 4;
        static constexpr double lengthScale = 0.24;          // L = round(lengthScale · fs / fc)
        static constexpr double minCutoffHz = 40.0;          // 手动收单下限：决定最大长度与 Clay 的固定延迟
        static constexpr int resyncIntervalSamples = 65536;  // 滑动和精确重算的网格间隔

        static int lengthFor (double sampleRate, double cutoffHz) noexcept;
        static int maxLengthFor (double sampleRate) noexcept   { return lengthFor (sampleRate, minCutoffHz); }
        static int groupDelayFor (int length) noexcept         { return 2 * (length - 1); }

        LinearPhaseLowpass() = default;

        // 消息线程：按最大长度分配（lengthFor 的结果都不超过它）。之后 reset 到长度 maxLength。
        void prepare (int maxLength);

        // 清零全部状态并设长度（钳到 [2, prepare 的 maxLength]）。音频线程可调，不分配，O(1)（历史环按「还没写过 = 0」处理，见文件头）。
        void reset (int length) noexcept;

        int getLength() const noexcept   { return length; }

        // 一个样本进、一个样本出（输出相对输入延迟 groupDelayFor(getLength())）。
        double process (double input) noexcept;

    private:
        // ---- 实现（T-012 阶段 4，代理 P1；细节见 LinearPhaseLowpass.cpp 各函数的注释）----
        //  - 各级共用一个写位置：第 s 级的历史环占 history 的 [s · maxLength, s · maxLength + length)，只用前 length 格。
        //    本样本写入的那一格正好存着 length 个样本之前的输入（x[n − L]）——先取出、再写入，一次就是「进一个、出一个」。
        //  - 滑动和存「和」不存「平均」：sums[s] += x − x[n − L]，本级输出 = sums[s] · (1 / L)。每 resyncIntervalSamples 个样本
        //   （从 reset 起的固定网格）按历史环重新求和一次；静音足够久后状态会精确回到 0（见 resync）。
        //  - 输入先清洗（process）：非有限值当 0，超过 ±inputLimit（1e4 ≈ +80 dBFS）的钳到 ±inputLimit——一个 NaN / Inf 进了
        //    滑动和会一直留到下一个重算网格点（最长 65536 样本），一个 1e30 会让它在大数离开窗口后丢掉其间的小信号；这两种输入
        //    本来就不是音乐，钳住它们换来「状态永远有限、误差有界」。正常音频（|x| ≤ 1e4）不受影响。
        void resync() noexcept;

        std::vector<double> history;                   // numStages × maxLength（prepare 分配）
        std::array<double, numStages> sums {};         // 各级最近 length 个输入之和
        double inverseLength = 0.5;                    // 1 / length
        int writeIndex = 0;                            // 本样本写入的位置（0 ～ length − 1），各级共用
        int written = 0;                               // reset 以来写过的格数，封顶 length（没到 length 时写位置 = written，那一格按 0 读）
        int samplesUntilResync = resyncIntervalSamples;
        int length = 2;
        int maxLength = 2;
    };
}
