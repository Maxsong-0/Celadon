/*
  ==============================================================================

    HalfBandOversampler.cpp

  ==============================================================================
*/

#include "HalfBandOversampler.h"

#include <cstring>

// MSVC：VS2022（v143）的 /fp:precise 默认不融合乘加；更早的工具集（v142）配 /arch:AVX2 时 /fp:precise 下仍可能融合，
// 这里写死关掉（ADR-013：各路径逐位相同的前提）。clang-cl 不认这个 pragma，由 /clang:-ffp-contract=off 与源码守卫管。
#if defined (_MSC_VER) && ! defined (__clang__)
 #pragma fp_contract (off)
#endif

// SIMD 路径：ARM 用 NEON；x86 / x64 用 SSE2（x64 的基线指令集，一定有）；其他平台用标量回退。
// 三条路径做的是同一串 IEEE 单精度运算（先加对称对、再乘、再累加，各自舍入），结果逐位相同——前提是编译器不把乘和加
// 融合成 FMA（见下面的守卫）。CELADON_FORCE_SCALAR_SIMD 只给验证用：在 ARM / x86 上强制走标量回退，与 SIMD 路径逐位对比。
#if ! defined (CELADON_FORCE_SCALAR_SIMD) && defined (__ARM_NEON)
 #define CELADON_HALFBAND_NEON 1
 #include <arm_neon.h>
#elif ! defined (CELADON_FORCE_SCALAR_SIMD) && (defined (__SSE2__) || defined (_M_X64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 2))
 #define CELADON_HALFBAND_SSE2 1
 #include <emmintrin.h>
#endif

// 编译前提（ADR-013，BUG-002）：乘和加必须各自舍入。GCC 对 C++ 默认 -ffp-contract=fast，目标有 FMA（-mfma / -march=x86-64-v3 /
// native、aarch64）时会跨函数合并成融合乘加，而且各循环合并得不一样；构建须给 -ffp-contract=off 并定义 CELADON_FP_CONTRACT_OFF=1
// （CMakeLists.txt 已做；关了合并 __FP_FAST_FMAF 照样有，只能看这个宏）。clang 的 -ffp-contract=fast 编译期识别不出，同样不许用。
#if defined (__GNUC__) && ! defined (__clang__) && defined (__FP_FAST_FMAF) && ! defined (CELADON_FP_CONTRACT_OFF)
 #error "GCC 在有 FMA 的目标上默认融合乘加：加 -ffp-contract=off 并定义 CELADON_FP_CONTRACT_OFF=1（ADR-013）"
#endif
// clang-cl（Visual Studio 的「LLVM (clang-cl)」工具集）沿用 clang 默认的 -ffp-contract=on，并因此预定义 _M_FP_CONTRACT：
// 要加 /clang:-ffp-contract=off（CMakeLists.txt 已做）。MSVC 自己的 cl.exe 默认 /fp:precise，不合并、不定义它。
#if defined (_MSC_VER) && defined (__clang__) && defined (_M_FP_CONTRACT)
 #error "clang-cl 默认合并乘加：加 /clang:-ffp-contract=off（ADR-013）"
#elif defined (__FAST_MATH__) || defined (_M_FP_FAST) || defined (_M_FP_CONTRACT)
 #error "不许用 -ffast-math、/fp:fast、/fp:contract：会融合乘加，破坏逐位契约（ADR-013）"
#endif

// 向量封装与核函数一律强制内联：Debug（-O0 / -O1）构建里也不留函数调用，免得 Debug 版慢到爆音（v0.5 研究 P1 / P2）。
#if defined (_MSC_VER)
 #define CELADON_SIMD_INLINE __forceinline
#else
 #define CELADON_SIMD_INLINE inline __attribute__ ((always_inline))
#endif

namespace celadon
{
    namespace
    {
        // 缓冲尾部余量：卷积按 4 个输出一组补齐，最后一组可能多读 / 多写 3 个；留 16 个。
        constexpr int bufferPadding = 16;

        int roundUpToFour (int n) noexcept   { return (n + 3) & ~3; }

        //==============================================================================
        // 4 路浮点向量的最小封装：只有这里分平台，下面的卷积代码三条路径共用一份。
       #if CELADON_HALFBAND_NEON
        using Vec = float32x4_t;

        CELADON_SIMD_INLINE Vec vLoad (const float* p) noexcept          { return vld1q_f32 (p); }
        CELADON_SIMD_INLINE void vStore (float* p, Vec v) noexcept       { vst1q_f32 (p, v); }
        CELADON_SIMD_INLINE Vec vSplat (float x) noexcept                { return vdupq_n_f32 (x); }
        CELADON_SIMD_INLINE Vec vAdd (Vec a, Vec b) noexcept             { return vaddq_f32 (a, b); }
        CELADON_SIMD_INLINE Vec vMul (Vec a, Vec b) noexcept             { return vmulq_f32 (a, b); }

        // p[0..7] = e0 o0 e1 o1 e2 o2 e3 o3
        CELADON_SIMD_INLINE void vStoreInterleaved (float* p, Vec even, Vec odd) noexcept
        {
            float32x4x2_t pair;
            pair.val[0] = even;
            pair.val[1] = odd;
            vst2q_f32 (p, pair);
        }

        // even = p0 p2 p4 p6，odd = p1 p3 p5 p7
        CELADON_SIMD_INLINE void vLoadDeinterleaved (const float* p, Vec& even, Vec& odd) noexcept
        {
            const auto pair = vld2q_f32 (p);
            even = pair.val[0];
            odd  = pair.val[1];
        }
       #elif CELADON_HALFBAND_SSE2
        using Vec = __m128;

        CELADON_SIMD_INLINE Vec vLoad (const float* p) noexcept          { return _mm_loadu_ps (p); }
        CELADON_SIMD_INLINE void vStore (float* p, Vec v) noexcept       { _mm_storeu_ps (p, v); }
        CELADON_SIMD_INLINE Vec vSplat (float x) noexcept                { return _mm_set1_ps (x); }
        CELADON_SIMD_INLINE Vec vAdd (Vec a, Vec b) noexcept             { return _mm_add_ps (a, b); }
        CELADON_SIMD_INLINE Vec vMul (Vec a, Vec b) noexcept             { return _mm_mul_ps (a, b); }

        CELADON_SIMD_INLINE void vStoreInterleaved (float* p, Vec even, Vec odd) noexcept
        {
            _mm_storeu_ps (p,     _mm_unpacklo_ps (even, odd));   // e0 o0 e1 o1
            _mm_storeu_ps (p + 4, _mm_unpackhi_ps (even, odd));   // e2 o2 e3 o3
        }

        CELADON_SIMD_INLINE void vLoadDeinterleaved (const float* p, Vec& even, Vec& odd) noexcept
        {
            const auto a = _mm_loadu_ps (p);
            const auto b = _mm_loadu_ps (p + 4);
            even = _mm_shuffle_ps (a, b, _MM_SHUFFLE (2, 0, 2, 0));
            odd  = _mm_shuffle_ps (a, b, _MM_SHUFFLE (3, 1, 3, 1));
        }
       #else
        // 标量回退：每条「向量运算」是 4 次独立的单精度运算，写在各自的函数里——clang / MSVC 默认只在同一个表达式内合并，
        // 不会把它们合并成乘加。GCC 的 -ffp-contract=fast 在有 FMA 的目标上会跨函数合并，而且与 SIMD 路径、主循环与尾循环
        // 合并得不一样，三条路径与块长无关都会失效（BUG-002，109 实测）——所以文件头的守卫要求 GCC 关掉合并。
        struct Vec { float v[4]; };

        CELADON_SIMD_INLINE Vec vLoad (const float* p) noexcept          { return { { p[0], p[1], p[2], p[3] } }; }
        CELADON_SIMD_INLINE void vStore (float* p, Vec a) noexcept       { for (int k = 0; k < 4; ++k) p[k] = a.v[k]; }
        CELADON_SIMD_INLINE Vec vSplat (float x) noexcept                { return { { x, x, x, x } }; }

        CELADON_SIMD_INLINE Vec vAdd (Vec a, Vec b) noexcept
        {
            Vec r;
            for (int k = 0; k < 4; ++k) r.v[k] = a.v[k] + b.v[k];
            return r;
        }

        CELADON_SIMD_INLINE Vec vMul (Vec a, Vec b) noexcept
        {
            Vec r;
            for (int k = 0; k < 4; ++k) r.v[k] = a.v[k] * b.v[k];
            return r;
        }

        CELADON_SIMD_INLINE void vStoreInterleaved (float* p, Vec even, Vec odd) noexcept
        {
            for (int k = 0; k < 4; ++k)
            {
                p[2 * k]     = even.v[k];
                p[2 * k + 1] = odd.v[k];
            }
        }

        CELADON_SIMD_INLINE void vLoadDeinterleaved (const float* p, Vec& even, Vec& odd) noexcept
        {
            for (int k = 0; k < 4; ++k)
            {
                even.v[k] = p[2 * k];
                odd.v[k]  = p[2 * k + 1];
            }
        }
       #endif

        //==============================================================================
        // 对称抽头对卷积（半带 FIR 去掉零系数与中心抽头后的部分），一次算 4 × numVectors 个相邻输出：
        //   acc[i] = Σ_{m = 0 … numPairs−1} c[m] · (h[i + m] + h[i + span − m])，m 升序，先加、再乘、再累加。
        // h 指向第一个输出对应的「最旧样本」，span = 2n + 1（最旧与最新样本的距离）。
        template <int numVectors>
        CELADON_SIMD_INLINE void accumulatePairs (const float* h, int span, const float* c, int numPairs, Vec (&acc)[numVectors]) noexcept
        {
            for (int k = 0; k < numVectors; ++k)
                acc[k] = vSplat (0.0f);

            const float* newest = h + span;

            for (int m = 0; m < numPairs; ++m)
            {
                const auto cm = vSplat (c[m]);

                for (int k = 0; k < numVectors; ++k)
                    acc[k] = vAdd (acc[k], vMul (vAdd (vLoad (h + m + 4 * k), vLoad (newest - m + 4 * k)), cm));
            }
        }

        // 上采样一级的核：history = [历史 2n+1 | 本块输入 × 2]，输出 2 × numInput 个（偶、奇交错）。
        //   偶数输出 out[2i]   = 对称抽头对卷积（JUCE：Σ (buf[k] + buf[N−k−1])·fir[k]）
        //   奇数输出 out[2i+1] = history[n + 1 + i] × 0.5（JUCE：buf[Ndiv2 + 1]·fir[Ndiv2]，即 2·x[i − n]·0.5）
        // numInput 按 4 补齐：多算的输出落在 out 的余量里，没人读。
        void upsampleKernel (const float* history, const float* c, int n, float* out, int numInput) noexcept
        {
            const int span = 2 * n + 1;
            const int numPairs = n + 1;
            const float* odd = history + n + 1;
            const auto half = vSplat (0.5f);
            const int count = roundUpToFour (numInput);
            int i = 0;

            for (; i + 16 <= count; i += 16)
            {
                Vec acc[4];
                accumulatePairs<4> (history + i, span, c, numPairs, acc);

                for (int k = 0; k < 4; ++k)
                    vStoreInterleaved (out + 2 * (i + 4 * k), acc[k], vMul (vLoad (odd + i + 4 * k), half));
            }

            for (; i < count; i += 4)
            {
                Vec acc[1];
                accumulatePairs<1> (history + i, span, c, numPairs, acc);
                vStoreInterleaved (out + 2 * i, acc[0], vMul (vLoad (odd + i), half));
            }
        }

        // 下采样一级的核：even = [历史 2n+1 | 本块偶相]，odd = [历史 n+1 | 本块奇相]，输出 numOutput 个。
        //   out[i] = 对称抽头对卷积（偶相）+ odd[i] × 0.5（JUCE：先累加抽头对，再 out += buf2[pos]·fir[Ndiv2]）
        void downsampleKernel (const float* even, const float* odd, const float* c, int n, float* out, int numOutput) noexcept
        {
            const int span = 2 * n + 1;
            const int numPairs = n + 1;
            const auto half = vSplat (0.5f);
            const int count = roundUpToFour (numOutput);
            int i = 0;

            for (; i + 16 <= count; i += 16)
            {
                Vec acc[4];
                accumulatePairs<4> (even + i, span, c, numPairs, acc);

                for (int k = 0; k < 4; ++k)
                    vStore (out + i + 4 * k, vAdd (acc[k], vMul (vLoad (odd + i + 4 * k), half)));
            }

            for (; i < count; i += 4)
            {
                Vec acc[1];
                accumulatePairs<1> (even + i, span, c, numPairs, acc);
                vStore (out + i, vAdd (acc[0], vMul (vLoad (odd + i), half)));
            }
        }

        // in[0 … 2·count) 拆成偶相 even[0 … count) 与奇相 odd[0 … count)。纯搬运，逐位不变。
        void deinterleave (const float* in, float* even, float* odd, int count) noexcept
        {
            int i = 0;

            for (; i + 4 <= count; i += 4)
            {
                Vec e, o;
                vLoadDeinterleaved (in + 2 * i, e, o);
                vStore (even + i, e);
                vStore (odd + i, o);
            }

            for (; i < count; ++i)
            {
                even[i] = in[2 * i];
                odd[i]  = in[2 * i + 1];
            }
        }
    }

    //==============================================================================
    HalfBandOversampler::HalfBandOversampler (size_t numChannelsToUse)
        : numChannels (juce::jmax ((size_t) 1, numChannelsToUse))
    {
    }

    HalfBandOversampler::Filter HalfBandOversampler::designFilter (float normalisedTransitionWidth, float stopbandAmplitudedB)
    {
        // 与 JUCE 的 Oversampling2TimesEquirippleFIR 同一个设计函数、同样的 float 系数。
        const auto coefficients = juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod (normalisedTransitionWidth,
                                                                                                             stopbandAmplitudedB);
        const auto* h = coefficients->getRawCoefficients();

        Filter f;
        f.length = (int) coefficients->getFilterOrder() + 1;
        f.n = (f.length - 3) / 4;

        // 等纹波半带：长度 4n + 3、中心抽头精确为 0.5、除中心外奇数位系数为 0、偶数位关于中心对称。
        // 下面的核只用得到偶数位的一半（对称对）与中心抽头，前提不成立就不能用。
        jassert (f.length == 4 * f.n + 3);
        jassert (h[2 * f.n + 1] == 0.5f);

        f.pairCoefficients.resize ((size_t) f.n + 1);
        f.dcGain = 0.5;

        for (int m = 0; m <= f.n; ++m)
        {
            jassert (h[2 * m] == h[f.length - 1 - 2 * m]);
            f.pairCoefficients[(size_t) m] = h[2 * m];
            f.dcGain += 2.0 * (double) h[2 * m];
        }

        return f;
    }

    void HalfBandOversampler::addOversamplingStage (float normalisedTransitionWidthUp, float stopbandAmplitudedBUp,
                                                    float normalisedTransitionWidthDown, float stopbandAmplitudedBDown)
    {
        Stage stage;
        stage.up   = designFilter (normalisedTransitionWidthUp,   stopbandAmplitudedBUp);
        stage.down = designFilter (normalisedTransitionWidthDown, stopbandAmplitudedBDown);

        // JUCE：static_cast<SampleType> (orderUp + orderDown) * 0.5f，在本级的高采样率下计。
        stage.latency = static_cast<float> ((size_t) (stage.up.length - 1) + (size_t) (stage.down.length - 1)) * 0.5f;

        stages.push_back (std::move (stage));
        isReady = false;
    }

    void HalfBandOversampler::clearOversamplingStages()
    {
        stages.clear();
        isReady = false;
    }

    void HalfBandOversampler::initProcessing (size_t maximumNumberOfSamplesBeforeOversampling)
    {
        maxInputSamples = juce::jmax ((size_t) 1, maximumNumberOfSamplesBeforeOversampling);

        // 先算总长度，一次分配，再把指针分给各级各声道。每段长度按 4 补齐再加余量。
        const auto maxIn = (int) maxInputSamples;
        size_t total = 0;

        auto region = [] (int length) { return (size_t) (roundUpToFour (length) + bufferPadding); };

        for (size_t s = 0; s < stages.size(); ++s)
        {
            const auto& stage = stages[s];
            const auto stageInput = maxIn << s;   // 本级上采样的输入长度 = 本级下采样的输出长度

            total += numChannels * (region (stage.up.span() + stageInput)
                                    + region (stage.down.span() + stageInput)
                                    + region (stage.down.n + 1 + stageInput)
                                    + region (2 * roundUpToFour (stageInput)));
        }

        if (stages.empty())
            total += numChannels * region (maxIn);

        storage.assign (total, 0.0f);

        float* next = storage.data();
        auto take = [&next, &region] (int length)
        {
            auto* p = next;
            next += region (length);
            return p;
        };

        for (size_t s = 0; s < stages.size(); ++s)
        {
            auto& stage = stages[s];
            const auto stageInput = maxIn << s;

            stage.upHistory.assign (numChannels, nullptr);
            stage.downEven.assign (numChannels, nullptr);
            stage.downOdd.assign (numChannels, nullptr);
            stage.output.assign (numChannels, nullptr);

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                stage.upHistory[ch] = take (stage.up.span() + stageInput);
                stage.downEven[ch]  = take (stage.down.span() + stageInput);
                stage.downOdd[ch]   = take (stage.down.n + 1 + stageInput);
                stage.output[ch]    = take (2 * roundUpToFour (stageInput));
            }
        }

        passThrough.assign (numChannels, nullptr);

        if (stages.empty())
            for (size_t ch = 0; ch < numChannels; ++ch)
                passThrough[ch] = take (maxIn);

        jassert (next == storage.data() + storage.size());

        outputChannelPointers.assign (numChannels, nullptr);
        lastUpsampledInputSamples = 0;
        isReady = true;
        reset();
    }

    void HalfBandOversampler::reset() noexcept
    {
        if (! isReady)
            return;

        // 只有历史段是状态；本块区与各级输出缓冲每块先写后读，不必清。
        for (auto& stage : stages)
        {
            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                std::fill (stage.upHistory[ch], stage.upHistory[ch] + stage.up.span(), 0.0f);
                std::fill (stage.downEven[ch],  stage.downEven[ch]  + stage.down.span(), 0.0f);
                std::fill (stage.downOdd[ch],   stage.downOdd[ch]   + stage.down.n + 1, 0.0f);
            }
        }
    }

    juce::dsp::AudioBlock<float> HalfBandOversampler::processSamplesUp (const juce::dsp::AudioBlock<const float>& inputBlock) noexcept
    {
        jassert (isReady);
        jassert (inputBlock.getNumChannels() <= numChannels);
        jassert (inputBlock.getNumSamples() <= maxInputSamples);

        if (! isReady)
            return {};

        // 超过 initProcessing 的块长是调用方的错；截短处理，至少不写越界。
        const auto numInput = (int) juce::jmin (inputBlock.getNumSamples(), maxInputSamples);
        const auto channelsToUse = juce::jmin (inputBlock.getNumChannels(), numChannels);
        lastUpsampledInputSamples = (size_t) numInput;

        for (size_t ch = 0; ch < channelsToUse; ++ch)
        {
            const float* source = inputBlock.getChannelPointer (ch);

            if (stages.empty())
            {
                std::copy (source, source + numInput, passThrough[ch]);
                outputChannelPointers[ch] = passThrough[ch];
                continue;
            }

            int length = numInput;

            for (auto& stage : stages)
            {
                const auto span = stage.up.span();
                auto* history = stage.upHistory[ch];
                auto* out = stage.output[ch];

                // JUCE：buf[N − 1] = 2 · x（半带插值的增益 2 放在输入上）。乘 2 是精确运算。
                for (int i = 0; i < length; ++i)
                    history[span + i] = 2.0f * source[i];

                upsampleKernel (history, stage.up.pairCoefficients.data(), stage.up.n, out, length);

                // 留下最近 2n+1 个输入作下一块的历史。
                std::memmove (history, history + length, sizeof (float) * (size_t) span);

                source = out;
                length *= 2;
            }

            outputChannelPointers[ch] = stages.back().output[ch];
        }

        return juce::dsp::AudioBlock<float> (outputChannelPointers.data(), channelsToUse,
                                             (size_t) numInput * getOversamplingFactor());
    }

    void HalfBandOversampler::processSamplesDown (juce::dsp::AudioBlock<float>& outputBlock) noexcept
    {
        jassert (isReady);
        jassert (outputBlock.getNumChannels() <= numChannels);
        jassert (outputBlock.getNumSamples() == lastUpsampledInputSamples);

        if (! isReady)
            return;

        const auto numOutput = (int) juce::jmin (outputBlock.getNumSamples(), maxInputSamples);
        const auto channelsToUse = juce::jmin (outputBlock.getNumChannels(), numChannels);

        for (size_t ch = 0; ch < channelsToUse; ++ch)
        {
            auto* destination = outputBlock.getChannelPointer (ch);

            if (stages.empty())
            {
                std::copy (passThrough[ch], passThrough[ch] + numOutput, destination);
                continue;
            }

            // 从最高一级往下：第 s 级的输入是第 s 级上采样的输出缓冲（已被调用方处理），
            // 输出写进第 s − 1 级的输出缓冲（它此时已无用），第 0 级写回自己的缓冲再拷给调用方。
            for (auto s = (int) stages.size() - 1; s >= 0; --s)
            {
                auto& stage = stages[(size_t) s];
                const auto length = numOutput << s;
                const auto span = stage.down.span();
                auto* even = stage.downEven[ch];
                auto* odd = stage.downOdd[ch];
                auto* source = stage.output[ch];
                auto* out = s > 0 ? stages[(size_t) s - 1].output[ch] : source;

                deinterleave (source, even + span, odd + stage.down.n + 1, length);
                downsampleKernel (even, odd, stage.down.pairCoefficients.data(), stage.down.n, out, length);

                std::memmove (even, even + length, sizeof (float) * (size_t) span);
                std::memmove (odd,  odd  + length, sizeof (float) * (size_t) (stage.down.n + 1));
            }

            std::copy (stages.front().output[ch], stages.front().output[ch] + numOutput, destination);
        }
    }

    float HalfBandOversampler::getLatencyInSamples() const noexcept
    {
        // 与 juce::dsp::Oversampling::getUncompensatedLatency 同一算法、同样的 float 运算顺序。
        auto latency = 0.0f;
        size_t order = 1;

        for (const auto& stage : stages)
        {
            order *= 2;
            latency += stage.latency / static_cast<float> (order);
        }

        return latency;
    }

    size_t HalfBandOversampler::getOversamplingFactor() const noexcept
    {
        return (size_t) 1 << stages.size();
    }

    size_t HalfBandOversampler::getNumStages() const noexcept
    {
        return stages.size();
    }

    int HalfBandOversampler::getMemoryInSamples() const noexcept
    {
        // 第 s 级的上、下采样滤波器跨 2^(s+1) 倍率下的「长度」个样本 = 长度 / 2^(s+1) 个 1× 样本；
        // 各级串联，记忆相加。每级再加 1 抵消取整（偶 / 奇相对齐）的零头。
        double memory = 0.0;

        for (size_t s = 0; s < stages.size(); ++s)
            memory += (double) (stages[s].up.length + stages[s].down.length) / (double) ((size_t) 2 << s);

        return (int) std::ceil (memory) + (int) stages.size();
    }

    double HalfBandOversampler::getUpsamplingDcGain() const noexcept
    {
        // 上采样一级在直流处：偶数输出 = 2x·2Σc、奇数输出 = 2x·0.5，平均 = x·(2Σc + 0.5) = x·Σh。
        double gain = 1.0;

        for (const auto& stage : stages)
            gain *= stage.up.dcGain;

        return gain;
    }

    double HalfBandOversampler::getDownsamplingDcGain() const noexcept
    {
        // 下采样一级在直流处：输出 = v·2Σc + v·0.5 = v·Σh。
        double gain = 1.0;

        for (const auto& stage : stages)
            gain *= stage.down.dcGain;

        return gain;
    }
}
