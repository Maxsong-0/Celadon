/*
  ==============================================================================

    TruePeak.cpp

    按块检测的向量核（v0.5 性能优化，研究 P4 R1）：
      - 一次算 12 个相邻输出 × 7 个插值相位；每个（输出, 相位）仍按抽头 k = 0 … 63 的顺序逐项乘加，
        所以舍入与逐样本的参照实现完全一样，结果逐位相同，也与样本落在哪一段、哪一组无关（分块无关）。
      - 「时间在向量的通道里」：通道 i 是第 i 个输出，每个抽头只做 3 次窗口加载、2 次系数加载、21 次乘加，
        没有水平求和；21 个独立累加器足够把乘加流水线喂满。
      - 逐项乘加的舍入方式按平台固定（下面的 multiplyAdd），参照实现与向量核用同一种：
          ARM64（NEON）：融合乘加（一次舍入）= 旧代码在 Xcode Release（-O3 + LTO）与 Debug 下编出的 fmadd 串行链，
                         所以装进 DAW 的版本读数与 v0.4 逐位相同；
          其他平台（x86 的 SSE2，将来的 Windows 版；或纯标量）：先乘后加、各自舍入。
        不依赖编译器怎么编（旧代码在 -O2 下会编成「先乘后加」，与 -O3 结果不同；新写法不再随编译选项变）——前提是编译器
        不替非 NEON 路径合并乘加：GCC 开 FMA 后会把 SSE2 核合并、参照实现却没合并（BUG-002），所以 GCC 要关 -ffp-contract（ADR-013）。
      - 8 个插值点取大（下面的 keepLarger）也按平台固定成同一种写法，冲零模式（FTZ / DAZ，宿主与
        juce::ScopedNoDenormals 常开）下两条路径才逐位相同（BUG-001）：
          x86（SSE2）：参照实现与向量核都用 max 指令（maxss / maxps）；
          ARM（NEON）与纯标量：比较 + 选择。

  ==============================================================================
*/

#include "TruePeak.h"

#include <algorithm>
#include <cmath>

#if (defined (__ARM_NEON) || defined (__ARM_NEON__)) && (defined (__aarch64__) || defined (__arm64__))
 #define CELADON_TRUE_PEAK_NEON 1
#elif defined (_M_ARM64) || defined (_M_ARM64EC)
 #define CELADON_TRUE_PEAK_NEON 1
#else
 #define CELADON_TRUE_PEAK_NEON 0
#endif

#if ! CELADON_TRUE_PEAK_NEON && (defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 2))
 #define CELADON_TRUE_PEAK_SSE2 1
#else
 #define CELADON_TRUE_PEAK_SSE2 0
#endif

#if CELADON_TRUE_PEAK_NEON
 #if defined (_MSC_VER) && ! defined (__clang__)
  #include <arm64_neon.h>
 #else
  #include <arm_neon.h>
 #endif
#elif CELADON_TRUE_PEAK_SSE2
 #include <emmintrin.h>
#endif

// 非 NEON 平台的逐项乘加必须「先乘后加、各自舍入」，参照实现与 SSE2 核才逐位相同：不许编译器把乘和加融合成 FMA。
// （clang 的 pragma 写在各函数体开头，但只在默认 -ffp-contract=on 下管用：-ffp-contract=fast 时 clang 不认它，109 实测参照实现
// 照样被合并。MSVC 默认 /fp:precise 本来就不融合，这里写死以防工程改成 /fp:contract。GCC 两种 pragma 都不认，靠下面的守卫。）
#if ! CELADON_TRUE_PEAK_NEON && defined (_MSC_VER) && ! defined (__clang__)
 #pragma fp_contract (off)
#endif

// 编译前提（ADR-013，BUG-002）：乘和加必须各自舍入。GCC 对 C++ 默认 -ffp-contract=fast，目标有 FMA（-mfma / -march=x86-64-v3 /
// native、aarch64）时会跨函数合并成融合乘加，而且各循环合并得不一样；构建须给 -ffp-contract=off 并定义 CELADON_FP_CONTRACT_OFF=1
// （CMakeLists.txt 已做；关了合并 __FP_FAST_FMAF 照样有，只能看这个宏）。clang 的 -ffp-contract=fast 编译期识别不出，同样不许用。
#if defined (__GNUC__) && ! defined (__clang__) && defined (__FP_FAST_FMAF) && ! defined (CELADON_FP_CONTRACT_OFF)
 #error "GCC 在有 FMA 的目标上默认融合乘加：加 -ffp-contract=off 并定义 CELADON_FP_CONTRACT_OFF=1（ADR-013）"
#endif
#if defined (__FAST_MATH__) || defined (_M_FP_FAST) || defined (_M_FP_CONTRACT)
 #error "不许用 -ffast-math、/fp:fast、/fp:contract：会融合乘加，破坏逐位契约（ADR-013）"
#endif

namespace celadon
{
    namespace
    {
        using PhaseTable = std::array<std::array<float, (size_t) TruePeakDetector::tapsPerPhase>,
                                      (size_t) TruePeakDetector::oversamplingFactor>;

        constexpr int numTaps       = TruePeakDetector::tapsPerPhase;
        constexpr int numPhases     = TruePeakDetector::oversamplingFactor;
        constexpr int tapStride     = 8;                                               // tapsByTap 里每个抽头占 8 格（7 相 + 1 格补 0）
        constexpr int phaseZeroTap  = numTaps - 1 - TruePeakDetector::latencySamples;  // 第 0 相 = 窗口里这个位置的原样本

        // 第一类零阶修正贝塞尔函数 I0（Kaiser 窗要用），级数展开到收敛。
        double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            const auto halfX = x * 0.5;

            for (int k = 1; k < 64; ++k)
            {
                const auto factor = halfX / (double) k;
                term *= factor * factor;
                sum += term;

                if (term < sum * 1.0e-17)
                    break;
            }

            return sum;
        }

        // Kaiser β：约 −90 dB 旁瓣。
        constexpr double kaiserBeta = 9.0;

        // 逐项乘加：sum + tap · x。舍入方式按平台固定，见文件头。
        inline float multiplyAdd (float sum, float tap, float x) noexcept
        {
           #if CELADON_TRUE_PEAK_NEON
            return std::fma (tap, x, sum);
           #else
            #if defined (__clang__)
             #pragma clang fp contract (off)
            #endif
            const auto product = tap * x;
            return sum + product;
           #endif
        }

        // 取大：candidate 比 peak 大就换成 candidate，否则（相等、任一边是 NaN）保留 peak——IEEE 语义与 juce::jmax (peak, candidate)
        // 相同。参照实现与向量核必须用同一条指令取大（向量核里对应的是下面各平台的 keepLarger 向量版）：
        //   x86（SSE2）：maxss（核里 maxps）。DAZ 开着时 max 指令先把非规格化的操作数换成 0，返回的也是换过的 0；
        //     「比较 + 按位选择」只在比较时当 0，返回的是原来的位（FTZ 单开两者相同）。旧写法两边都由编译器挑指令
        //     （2026-09-26 x86 服务器实测）：标量 jmax 在 GCC -O2 / -O3、clang -O2 下编成 maxss，GCC -O0 下编成比较 + 跳转；
        //     向量按位选择 GCC 照写，clang -O2 认成 maxps。于是 GCC Release 下参照实现是 maxss、SSE2 核是按位选择，
        //     DAZ 下非规格化样本的峰值一边是 0、一边是它自己（BUG-001）。所以两边都写死成 max 指令（intrinsic 在 -O0 下也是它）。
        //   ARM（NEON）与纯标量：比较 + 选择（NEON 核是 vclt + vbsl）。ARM 的 fmax / fmaxnm 的 NaN 规则与 jmax 不同，
        //     编译器只能编成比较 + 选择（fcmp + fcsel），FZ 下同样是比较时当 0、返回原来的位，与核一致。
        inline float keepLarger (float peak, float candidate) noexcept
        {
           #if CELADON_TRUE_PEAK_SSE2
            return _mm_cvtss_f32 (_mm_max_ss (_mm_set_ss (candidate), _mm_set_ss (peak)));   // 有 NaN 或相等时取第二个操作数 peak
           #else
            return peak < candidate ? candidate : peak;
           #endif
        }

        // 直接型参照：window[0] 最旧，window[numTaps − 1] 是刚送进来的样本。
        float directFormPeak (const float* window, const PhaseTable& reversedPhases) noexcept
        {
            // 第 0 相 = 原样本本身（延迟 latency 个样本）。
            auto peak = std::abs (window[phaseZeroTap]);

            for (size_t phase = 1; phase < (size_t) numPhases; ++phase)
            {
                const auto& taps = reversedPhases[phase];
                float sum = 0.0f;

                for (size_t k = 0; k < (size_t) numTaps; ++k)
                    sum = multiplyAdd (sum, taps[k], window[k]);

                peak = keepLarger (peak, std::abs (sum));
            }

            return peak;
        }

        //==============================================================================
        // 向量核：line[g·12 + i … g·12 + i + 63] 是第 g 组第 i 个输出的窗口，结果写 peaks[g·12 + i]。
       #if CELADON_TRUE_PEAK_NEON
        // 与 juce::jmax (peak, candidate) 相同：peak < candidate 时取 candidate，否则保留 peak（NaN 候选被忽略）。
        inline float32x4_t keepLarger (float32x4_t peak, float32x4_t candidate) noexcept
        {
            return vbslq_f32 (vcltq_f32 (peak, candidate), candidate, peak);
        }

        void computePeakGroups (const float* line, const float* tapsByTap, const PhaseTable&, float* peaks, int numGroups) noexcept
        {
            for (int group = 0; group < numGroups; ++group)
            {
                const float* window = line + group * 12;
                const auto zero = vdupq_n_f32 (0.0f);

                // 累加器 aPq：第 P 相、第 q 组 4 个输出（q = 0：输出 0–3，1：4–7，2：8–11）。
                // 用具名变量而不是数组：-O1（Debug 配置）下数组会被放进内存，慢 3 倍。
                auto a10 = zero, a11 = zero, a12 = zero, a20 = zero, a21 = zero, a22 = zero,
                     a30 = zero, a31 = zero, a32 = zero, a40 = zero, a41 = zero, a42 = zero,
                     a50 = zero, a51 = zero, a52 = zero, a60 = zero, a61 = zero, a62 = zero,
                     a70 = zero, a71 = zero, a72 = zero;

                for (int k = 0; k < numTaps; ++k)
                {
                    const auto x0 = vld1q_f32 (window + k);
                    const auto x1 = vld1q_f32 (window + k + 4);
                    const auto x2 = vld1q_f32 (window + k + 8);
                    const auto t0 = vld1q_f32 (tapsByTap + tapStride * k);       // 第 1 … 4 相的第 k 个抽头
                    const auto t1 = vld1q_f32 (tapsByTap + tapStride * k + 4);   // 第 5 … 7 相（第 4 格是 0，不用）

                    a10 = vfmaq_laneq_f32 (a10, x0, t0, 0);  a11 = vfmaq_laneq_f32 (a11, x1, t0, 0);  a12 = vfmaq_laneq_f32 (a12, x2, t0, 0);
                    a20 = vfmaq_laneq_f32 (a20, x0, t0, 1);  a21 = vfmaq_laneq_f32 (a21, x1, t0, 1);  a22 = vfmaq_laneq_f32 (a22, x2, t0, 1);
                    a30 = vfmaq_laneq_f32 (a30, x0, t0, 2);  a31 = vfmaq_laneq_f32 (a31, x1, t0, 2);  a32 = vfmaq_laneq_f32 (a32, x2, t0, 2);
                    a40 = vfmaq_laneq_f32 (a40, x0, t0, 3);  a41 = vfmaq_laneq_f32 (a41, x1, t0, 3);  a42 = vfmaq_laneq_f32 (a42, x2, t0, 3);
                    a50 = vfmaq_laneq_f32 (a50, x0, t1, 0);  a51 = vfmaq_laneq_f32 (a51, x1, t1, 0);  a52 = vfmaq_laneq_f32 (a52, x2, t1, 0);
                    a60 = vfmaq_laneq_f32 (a60, x0, t1, 1);  a61 = vfmaq_laneq_f32 (a61, x1, t1, 1);  a62 = vfmaq_laneq_f32 (a62, x2, t1, 1);
                    a70 = vfmaq_laneq_f32 (a70, x0, t1, 2);  a71 = vfmaq_laneq_f32 (a71, x1, t1, 2);  a72 = vfmaq_laneq_f32 (a72, x2, t1, 2);
                }

                // 第 0 相 = |原样本|，之后按第 1 … 7 相的顺序取较大者（与参照实现的比较顺序相同）。
                auto p0 = vabsq_f32 (vld1q_f32 (window + phaseZeroTap));
                auto p1 = vabsq_f32 (vld1q_f32 (window + phaseZeroTap + 4));
                auto p2 = vabsq_f32 (vld1q_f32 (window + phaseZeroTap + 8));

                p0 = keepLarger (p0, vabsq_f32 (a10));  p1 = keepLarger (p1, vabsq_f32 (a11));  p2 = keepLarger (p2, vabsq_f32 (a12));
                p0 = keepLarger (p0, vabsq_f32 (a20));  p1 = keepLarger (p1, vabsq_f32 (a21));  p2 = keepLarger (p2, vabsq_f32 (a22));
                p0 = keepLarger (p0, vabsq_f32 (a30));  p1 = keepLarger (p1, vabsq_f32 (a31));  p2 = keepLarger (p2, vabsq_f32 (a32));
                p0 = keepLarger (p0, vabsq_f32 (a40));  p1 = keepLarger (p1, vabsq_f32 (a41));  p2 = keepLarger (p2, vabsq_f32 (a42));
                p0 = keepLarger (p0, vabsq_f32 (a50));  p1 = keepLarger (p1, vabsq_f32 (a51));  p2 = keepLarger (p2, vabsq_f32 (a52));
                p0 = keepLarger (p0, vabsq_f32 (a60));  p1 = keepLarger (p1, vabsq_f32 (a61));  p2 = keepLarger (p2, vabsq_f32 (a62));
                p0 = keepLarger (p0, vabsq_f32 (a70));  p1 = keepLarger (p1, vabsq_f32 (a71));  p2 = keepLarger (p2, vabsq_f32 (a72));

                vst1q_f32 (peaks + group * 12,     p0);
                vst1q_f32 (peaks + group * 12 + 4, p1);
                vst1q_f32 (peaks + group * 12 + 8, p2);
            }
        }
       #elif CELADON_TRUE_PEAK_SSE2
        // 与参照实现的 keepLarger（maxss）同一种取大：maxps 在相等或任一边是 NaN 时取第二个操作数 peak。
        inline __m128 keepLarger (__m128 peak, __m128 candidate) noexcept
        {
            return _mm_max_ps (candidate, peak);
        }

        template <int lane>
        inline __m128 broadcast (__m128 v) noexcept
        {
            return _mm_shuffle_ps (v, v, _MM_SHUFFLE (lane, lane, lane, lane));
        }

        // x86-64 只有 16 个 XMM 寄存器：一次 4 个输出 × 7 相 = 7 个累加器；一组 12 个输出分三次算。
        void computePeakGroups (const float* line, const float* tapsByTap, const PhaseTable&, float* peaks, int numGroups) noexcept
        {
           #if defined (__clang__)
            #pragma clang fp contract (off)
           #endif
            const auto absMask = _mm_castsi128_ps (_mm_set1_epi32 (0x7fffffff));

            for (int quad = 0; quad < numGroups * 3; ++quad)
            {
                const float* window = line + quad * 4;
                auto a1 = _mm_setzero_ps(), a2 = a1, a3 = a1, a4 = a1, a5 = a1, a6 = a1, a7 = a1;

                for (int k = 0; k < numTaps; ++k)
                {
                    const auto x  = _mm_loadu_ps (window + k);
                    const auto t0 = _mm_loadu_ps (tapsByTap + tapStride * k);
                    const auto t1 = _mm_loadu_ps (tapsByTap + tapStride * k + 4);

                    a1 = _mm_add_ps (a1, _mm_mul_ps (x, broadcast<0> (t0)));
                    a2 = _mm_add_ps (a2, _mm_mul_ps (x, broadcast<1> (t0)));
                    a3 = _mm_add_ps (a3, _mm_mul_ps (x, broadcast<2> (t0)));
                    a4 = _mm_add_ps (a4, _mm_mul_ps (x, broadcast<3> (t0)));
                    a5 = _mm_add_ps (a5, _mm_mul_ps (x, broadcast<0> (t1)));
                    a6 = _mm_add_ps (a6, _mm_mul_ps (x, broadcast<1> (t1)));
                    a7 = _mm_add_ps (a7, _mm_mul_ps (x, broadcast<2> (t1)));
                }

                auto peak = _mm_and_ps (_mm_loadu_ps (window + phaseZeroTap), absMask);
                peak = keepLarger (peak, _mm_and_ps (a1, absMask));
                peak = keepLarger (peak, _mm_and_ps (a2, absMask));
                peak = keepLarger (peak, _mm_and_ps (a3, absMask));
                peak = keepLarger (peak, _mm_and_ps (a4, absMask));
                peak = keepLarger (peak, _mm_and_ps (a5, absMask));
                peak = keepLarger (peak, _mm_and_ps (a6, absMask));
                peak = keepLarger (peak, _mm_and_ps (a7, absMask));

                _mm_storeu_ps (peaks + quad * 4, peak);
            }
        }
       #else
        // 没有 SIMD 的平台：逐个输出走参照实现（同一个函数，逐位相同是必然的）。
        void computePeakGroups (const float* line, const float*, const PhaseTable& reversedPhases, float* peaks, int numGroups) noexcept
        {
            for (int i = 0; i < numGroups * 12; ++i)
                peaks[i] = directFormPeak (line + i, reversedPhases);
        }
       #endif
    }

    //==============================================================================
    TruePeakDetector::TruePeakDetector()
    {
        constexpr int numTapsTotal = oversamplingFactor * tapsPerPhase;   // 512 个抽头（8× 采样率下）
        constexpr int centre       = numTapsTotal / 2;                    // 256，对应原采样率下 latencySamples = 32

        static_assert (groupSize == 12, "向量核按 12 个输出一组写死");
        static_assert (chunkLength % groupSize == 0, "每段必须是整组");

        std::array<double, (size_t) numTapsTotal> h {};

        for (int k = 0; k < numTapsTotal; ++k)
        {
            const auto offset = k - centre;

            // sinc((k − centre) / 8)：截止在原采样率的奈奎斯特频率。
            // 偏移是 8 的整数倍时精确给 1 或 0，保证第 0 相就是原样本本身。
            double sinc = 0.0;

            if (offset % oversamplingFactor == 0)
                sinc = (offset == 0 ? 1.0 : 0.0);
            else
            {
                const auto x = juce::MathConstants<double>::pi * (double) offset / (double) oversamplingFactor;
                sinc = std::sin (x) / x;
            }

            // Kaiser 窗以 centre 为中心、半宽 centre（k = numTapsTotal 那一点落在 sinc 零点上，省掉不影响对称）。
            const auto ratio = (double) offset / (double) centre;
            const auto window = besselI0 (kaiserBeta * std::sqrt (juce::jmax (0.0, 1.0 - ratio * ratio)))
                              / besselI0 (kaiserBeta);

            h[(size_t) k] = sinc * window;
        }

        for (int phase = 0; phase < oversamplingFactor; ++phase)
        {
            // 每一相单独归一到直流增益 1：直流信号插出来的点不会偏高或偏低。
            double sum = 0.0;

            for (int j = 0; j < tapsPerPhase; ++j)
                sum += h[(size_t) (oversamplingFactor * j + phase)];

            for (int k = 0; k < tapsPerPhase; ++k)
            {
                const auto j = tapsPerPhase - 1 - k;
                reversedPhases[(size_t) phase][(size_t) k] = (float) (h[(size_t) (oversamplingFactor * j + phase)] / sum);
            }
        }

        // 同一批 float 系数按抽头重排给向量核（数值逐位不变，只是换了存放顺序）。
        for (int k = 0; k < tapsPerPhase; ++k)
        {
            for (int phase = 1; phase < oversamplingFactor; ++phase)
                tapsByTap[(size_t) (tapStride * k + phase - 1)] = reversedPhases[(size_t) phase][(size_t) k];

            tapsByTap[(size_t) (tapStride * k + tapStride - 1)] = 0.0f;
        }
    }

    const char* TruePeakDetector::getBlockKernelName() noexcept
    {
       #if CELADON_TRUE_PEAK_NEON
        return "NEON";
       #elif CELADON_TRUE_PEAK_SSE2
        return "SSE2";
       #else
        return "scalar";
       #endif
    }

    void TruePeakDetector::prepare (int numChannels)
    {
        channels.assign ((size_t) juce::jmax (1, numChannels), ChannelState {});
    }

    void TruePeakDetector::reset() noexcept
    {
        for (auto& state : channels)
        {
            state.history.fill (0.0f);
            state.writeIndex = 0;
        }
    }

    float TruePeakDetector::processSample (int channel, float x) noexcept
    {
        jassert (juce::isPositiveAndBelow (channel, (int) channels.size()));
        auto& state = channels[(size_t) channel];

        state.history[(size_t) state.writeIndex] = x;
        state.history[(size_t) (state.writeIndex + tapsPerPhase)] = x;
        state.writeIndex = (state.writeIndex + 1) % tapsPerPhase;

        // window[0] 是最旧的样本，window[tapsPerPhase − 1] 是刚写进来的样本。
        return directFormPeak (state.history.data() + state.writeIndex, reversedPhases);
    }

    void TruePeakDetector::processChunk (ChannelState& state, const float* input, int numSamples) noexcept
    {
        jassert (numSamples > 0 && numSamples <= chunkLength);
        auto* data = line.data();

        // 最近 63 个样本（旧 → 新）：环形缓冲里 writeIndex + 1 … writeIndex + 63 这一段（双写保证连续）。
        std::copy_n (state.history.data() + state.writeIndex + 1, historyLength, data);
        std::copy_n (input, numSamples, data + historyLength);

        // 最后一组不满 12 个时补零：多算的几个输出直接丢掉；补零只是让它们不碰到上一段留下的非有限值 / 非规格化数。
        const auto numGroups = (numSamples + groupSize - 1) / groupSize;
        std::fill (data + historyLength + numSamples, data + historyLength + numGroups * groupSize, 0.0f);

        computePeakGroups (data, tapsByTap.data(), reversedPhases, peakScratch.data(), numGroups);

        // 历史推进 numSamples 个样本，与逐个 processSample 写入的结果完全相同（环形缓冲只留得下最后 64 个写入）。
        for (int i = juce::jmax (0, numSamples - tapsPerPhase); i < numSamples; ++i)
        {
            const auto slot = (state.writeIndex + i) % tapsPerPhase;
            const auto x = data[historyLength + i];
            state.history[(size_t) slot] = x;
            state.history[(size_t) (slot + tapsPerPhase)] = x;
        }

        state.writeIndex = (state.writeIndex + numSamples) % tapsPerPhase;
    }

    void TruePeakDetector::processBlock (int channel, const float* input, float* peaks, int numSamples) noexcept
    {
        jassert (juce::isPositiveAndBelow (channel, (int) channels.size()));
        auto& state = channels[(size_t) channel];

        for (int done = 0; done < numSamples;)
        {
            const auto length = juce::jmin (chunkLength, numSamples - done);
            processChunk (state, input + done, length);   // 先把本段输入拷走，再写 peaks：原地调用也安全
            std::copy_n (peakScratch.data(), length, peaks + done);
            done += length;
        }
    }

    float TruePeakDetector::processBlockMax (int channel, const float* input, int numSamples) noexcept
    {
        jassert (juce::isPositiveAndBelow (channel, (int) channels.size()));
        auto& state = channels[(size_t) channel];
        float blockPeak = 0.0f;

        for (int done = 0; done < numSamples;)
        {
            const auto length = juce::jmin (chunkLength, numSamples - done);
            processChunk (state, input + done, length);

            for (int i = 0; i < length; ++i)
                blockPeak = juce::jmax (blockPeak, peakScratch[(size_t) i]);

            done += length;
        }

        return blockPeak;
    }
}
