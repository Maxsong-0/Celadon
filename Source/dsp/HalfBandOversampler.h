/*
  ==============================================================================

    HalfBandOversampler.h
    块式半带 FIR 过采样器：与 juce::dsp::Oversampling 的 filterHalfBandFIREquiripple 级
    同系数（同一个 FilterDesign 函数生成）、同线性相位、同整数延迟，输出只差舍入（与 JUCE 差 < −120 dBFS），
    但按块处理、SIMD 并行。Color 的 Heat 用它（三级 8×）；Limit 的 Clip 级（两级 4×，两级参数与 Heat 前两级相同）
    集成时也换用它——接口仿 juce::dsp::Oversampling，调用处只换类型与 addOversamplingStage 的写法。

    为什么自己写（v0.5 研究 P3 / P4）：JUCE 的实现每进一个样本就把整段状态缓冲平移一次，卷积是一条
    串行累加链，受延迟限制——Heat 的 8× 过采样占了 Color 模块 3/4 的 CPU，而且 Heat = 0 时也在跑。

    每级、每声道的做法：
      上采样：线性缓冲 [历史 2n+1 | 本块输入 × 2]；偶数输出 = 对称抽头对卷积，奇数输出 = 中心抽头 0.5 × 延迟 n 的输入。
      下采样：偶相、奇相各一段线性缓冲；输出 = 偶相对称抽头对卷积 + 中心抽头 0.5 × 延迟 n+1 的奇相样本。
      （与 JUCE 逐项同一公式；滤波器长 4n + 3，除中心外奇数位系数为 0。）
      每块只搬一次历史（memmove），不再逐样本平移。
    并行方向是「输出样本」：一次算 16 个（尾部 4 个）相邻输出，每个输出都按抽头对 m = 0 … n 的顺序
    「先加对称对、再乘系数、再累加」，三步各自舍入（不用融合乘加；编译器也不许替我们合并，GCC 要关 -ffp-contract，ADR-013）。所以：
      - 每个输出的运算与块长、分组方式无关 → 输出与块长无关，逐位相同；
      - ARM（NEON）、x86（SSE2）与其他平台的标量回退算出的是同一串 IEEE 运算 → 三条路径逐位相同；
      - x86 上不依赖 FMA 指令（没开 FMA 时 std::fma 是软件实现，极慢）。

    只依赖 juce_dsp（Source/dsp/ 的硬规则）。initProcessing 分配，处理路径零分配、零锁。

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace celadon
{
    class HalfBandOversampler
    {
    public:
        explicit HalfBandOversampler (size_t numChannels);

        // 追加一级 2× 半带。参数与 juce::dsp::Oversampling::addOversamplingStage (filterHalfBandFIREquiripple, …)
        // 完全相同（归一化过渡带宽、阻带衰减 dB，上 / 下采样各一组），系数由同一个
        // juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod 生成。
        void addOversamplingStage (float normalisedTransitionWidthUp, float stopbandAmplitudedBUp,
                                   float normalisedTransitionWidthDown, float stopbandAmplitudedBDown);

        void clearOversamplingStages();

        // 分配处理缓冲并清空状态（不在音频线程调用）。之后每次处理的块长不得超过这个值。
        void initProcessing (size_t maximumNumberOfSamplesBeforeOversampling);

        // 清空全部滤波器历史（音频线程可调：不分配、不加锁）。
        void reset() noexcept;

        // 上采样：返回过采样后的块（内部缓冲，声道数 = 输入块声道数，长度 = 输入长度 × 倍率），调用方可原地改写，
        // 然后用 processSamplesDown 下采样回来。与 juce::dsp::Oversampling 的用法相同。
        juce::dsp::AudioBlock<float> processSamplesUp (const juce::dsp::AudioBlock<const float>& inputBlock) noexcept;

        // 下采样：把最近一次 processSamplesUp 返回的块（已被调用方处理过）滤波抽取后写进 outputBlock。
        void processSamplesDown (juce::dsp::AudioBlock<float>& outputBlock) noexcept;

        // 总延迟（1× 样本，未补偿），算法与 juce::dsp::Oversampling::getLatencyInSamples 相同（float 运算顺序也相同）。
        // 这里不做 JUCE 的 Thiran 小数延迟补偿：调用方要让各级参数凑出整数延迟（Heat 64、Clip 60），与干路逐样本对齐。
        float getLatencyInSamples() const noexcept;

        size_t getOversamplingFactor() const noexcept;
        size_t getNumStages() const noexcept;

        // 记忆长度（1× 样本）：任意时刻的内部状态只取决于最近这么多个输入样本（中间的波形整形须无记忆、
        // 且在这段输入上参数不变）。所以「空闲时跳过、重开时预热」可以做到与一直在跑逐位相同：
        // reset() 后把最近 ≥ 这么多个输入样本按同样的整形重跑一遍即可（Color 的 Heat、Limit 的 Clip 级都这么做）。
        // 取值 = ⌈Σ 各级（上采样长 + 下采样长）/ 2^(级号+1)⌉ + 级数，偏保守；ColorTests 有逐位验证。
        int getMemoryInSamples() const noexcept;

        // 上采样链 / 下采样链的直流增益（各级 FIR 系数和之积，理想值 1，等纹波设计差 1e-4 量级）。
        // Color 的 Heat 空闲时用只含第 1 级的过采样器近似整条链，用它把其余各级的直流增益补齐。
        double getUpsamplingDcGain() const noexcept;
        double getDownsamplingDcGain() const noexcept;

    private:
        struct Filter
        {
            std::vector<float> pairCoefficients;   // pairCoefficients[m] = h[2m]，m = 0 … n（对称对的系数）
            int n = 0;                             // 滤波器长 = 4n + 3，中心抽头 h[2n + 1] = 0.5
            int length = 0;
            double dcGain = 1.0;                   // Σ h = 2 Σ pairCoefficients + 0.5

            int span() const noexcept   { return 2 * n + 1; }
        };

        struct Stage
        {
            Filter up, down;
            float latency = 0.0f;   // 本级高采样率下的延迟 = (上采样阶数 + 下采样阶数) × 0.5（同 JUCE）

            // 每声道一组缓冲，都指向 storage 里的片段（initProcessing 时分好）。
            std::vector<float*> upHistory;     // [2n+1 个历史 | 本块输入 × 2 | 余量]
            std::vector<float*> downEven;      // [2n+1 个历史 | 本块偶相 | 余量]
            std::vector<float*> downOdd;       // [n+1 个历史 | 本块奇相 | 余量]
            std::vector<float*> output;        // 本级上采样输出（= 本级下采样输入 / 下一级下采样的输出），长 2 × 输入长
        };

        static Filter designFilter (float normalisedTransitionWidth, float stopbandAmplitudedB);

        size_t numChannels = 0;
        std::vector<Stage> stages;
        size_t maxInputSamples = 0;
        bool isReady = false;

        std::vector<float> storage;                 // 全部缓冲的一整块内存
        std::vector<float*> passThrough;            // 0 级时（倍率 1）的拷贝缓冲，每声道一段
        std::vector<float*> outputChannelPointers;  // processSamplesUp 返回块的声道指针
        size_t lastUpsampledInputSamples = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HalfBandOversampler)
    };
}
