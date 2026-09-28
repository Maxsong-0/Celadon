/*
  ==============================================================================

    Saturator.cpp

  ==============================================================================
*/

#include "Saturator.h"

// SIMD 路径：ARM 用 NEON；x86 / x64 用 SSE2；其他平台用标量回退。三条路径是同一串 IEEE 单精度运算
// （先乘后加、各自舍入，不用融合乘加），结果逐位相同——前提是编译器也不替我们合并（见下面的守卫）。
// CELADON_FORCE_SCALAR_SIMD 只给验证用。
#if ! defined (CELADON_FORCE_SCALAR_SIMD) && defined (__ARM_NEON)
 #define CELADON_SATURATOR_NEON 1
 #include <arm_neon.h>
#elif ! defined (CELADON_FORCE_SCALAR_SIMD) && (defined (__SSE2__) || defined (_M_X64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 2))
 #define CELADON_SATURATOR_SSE2 1
 #include <emmintrin.h>
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
        //==============================================================================
        // 4 路浮点向量的最小封装（只有这里分平台）。钳位要让 NaN 原样穿过：Heat 的输入若带 NaN，
        // 输出也必须是 NaN，处理器的 NaN 保护才看得见（std::tanh 就是这样）。
       #if CELADON_SATURATOR_NEON
        using Vec  = float32x4_t;
        using Mask = uint32x4_t;

        CELADON_SIMD_INLINE Vec vLoad (const float* p) noexcept              { return vld1q_f32 (p); }
        CELADON_SIMD_INLINE void vStore (float* p, Vec v) noexcept           { vst1q_f32 (p, v); }
        CELADON_SIMD_INLINE Vec vSplat (float x) noexcept                    { return vdupq_n_f32 (x); }
        CELADON_SIMD_INLINE Vec vAdd (Vec a, Vec b) noexcept                 { return vaddq_f32 (a, b); }
        CELADON_SIMD_INLINE Vec vSub (Vec a, Vec b) noexcept                 { return vsubq_f32 (a, b); }
        CELADON_SIMD_INLINE Vec vMul (Vec a, Vec b) noexcept                 { return vmulq_f32 (a, b); }
        CELADON_SIMD_INLINE Vec vDiv (Vec a, Vec b) noexcept                 { return vdivq_f32 (a, b); }
        CELADON_SIMD_INLINE Vec vAbs (Vec a) noexcept                        { return vabsq_f32 (a); }
        CELADON_SIMD_INLINE Mask vLess (Vec a, Vec b) noexcept               { return vcltq_f32 (a, b); }
        CELADON_SIMD_INLINE Vec vSelect (Mask m, Vec a, Vec b) noexcept      { return vbslq_f32 (m, a, b); }

        // FMIN / FMAX：任一操作数是 NaN 结果就是 NaN。
        CELADON_SIMD_INLINE Vec vClamp (Vec v, Vec low, Vec high) noexcept   { return vmaxq_f32 (vminq_f32 (v, high), low); }
       #elif CELADON_SATURATOR_SSE2
        using Vec  = __m128;
        using Mask = __m128;

        CELADON_SIMD_INLINE Vec vLoad (const float* p) noexcept              { return _mm_loadu_ps (p); }
        CELADON_SIMD_INLINE void vStore (float* p, Vec v) noexcept           { _mm_storeu_ps (p, v); }
        CELADON_SIMD_INLINE Vec vSplat (float x) noexcept                    { return _mm_set1_ps (x); }
        CELADON_SIMD_INLINE Vec vAdd (Vec a, Vec b) noexcept                 { return _mm_add_ps (a, b); }
        CELADON_SIMD_INLINE Vec vSub (Vec a, Vec b) noexcept                 { return _mm_sub_ps (a, b); }
        CELADON_SIMD_INLINE Vec vMul (Vec a, Vec b) noexcept                 { return _mm_mul_ps (a, b); }
        CELADON_SIMD_INLINE Vec vDiv (Vec a, Vec b) noexcept                 { return _mm_div_ps (a, b); }
        CELADON_SIMD_INLINE Vec vAbs (Vec a) noexcept                        { return _mm_andnot_ps (_mm_set1_ps (-0.0f), a); }
        CELADON_SIMD_INLINE Mask vLess (Vec a, Vec b) noexcept               { return _mm_cmplt_ps (a, b); }
        CELADON_SIMD_INLINE Vec vSelect (Mask m, Vec a, Vec b) noexcept      { return _mm_or_ps (_mm_and_ps (m, a), _mm_andnot_ps (m, b)); }

        // minps / maxps 在任一操作数是 NaN 时返回「第二个」操作数：把 v 放在第二位，NaN 才穿得过去。
        CELADON_SIMD_INLINE Vec vClamp (Vec v, Vec low, Vec high) noexcept   { return _mm_max_ps (low, _mm_min_ps (high, v)); }
       #else
        struct Vec  { float v[4]; };
        struct Mask { bool m[4]; };

        CELADON_SIMD_INLINE Vec vLoad (const float* p) noexcept              { return { { p[0], p[1], p[2], p[3] } }; }
        CELADON_SIMD_INLINE void vStore (float* p, Vec a) noexcept           { for (int k = 0; k < 4; ++k) p[k] = a.v[k]; }
        CELADON_SIMD_INLINE Vec vSplat (float x) noexcept                    { return { { x, x, x, x } }; }

        template <typename Op>
        CELADON_SIMD_INLINE Vec lanewise (Vec a, Vec b, Op op) noexcept
        {
            Vec r;
            for (int k = 0; k < 4; ++k) r.v[k] = op (a.v[k], b.v[k]);
            return r;
        }

        CELADON_SIMD_INLINE Vec vAdd (Vec a, Vec b) noexcept                 { return lanewise (a, b, [] (float x, float y) { return x + y; }); }
        CELADON_SIMD_INLINE Vec vSub (Vec a, Vec b) noexcept                 { return lanewise (a, b, [] (float x, float y) { return x - y; }); }
        CELADON_SIMD_INLINE Vec vMul (Vec a, Vec b) noexcept                 { return lanewise (a, b, [] (float x, float y) { return x * y; }); }
        CELADON_SIMD_INLINE Vec vDiv (Vec a, Vec b) noexcept                 { return lanewise (a, b, [] (float x, float y) { return x / y; }); }
        CELADON_SIMD_INLINE Vec vAbs (Vec a) noexcept                        { Vec r; for (int k = 0; k < 4; ++k) r.v[k] = std::abs (a.v[k]); return r; }

        CELADON_SIMD_INLINE Mask vLess (Vec a, Vec b) noexcept
        {
            Mask r;
            for (int k = 0; k < 4; ++k) r.m[k] = a.v[k] < b.v[k];
            return r;
        }

        CELADON_SIMD_INLINE Vec vSelect (Mask m, Vec a, Vec b) noexcept
        {
            Vec r;
            for (int k = 0; k < 4; ++k) r.v[k] = m.m[k] ? a.v[k] : b.v[k];
            return r;
        }

        // 比较结果为假（含 NaN）时取 v 本身：NaN 原样穿过。
        CELADON_SIMD_INLINE Vec vClamp (Vec a, Vec low, Vec high) noexcept
        {
            Vec r;
            for (int k = 0; k < 4; ++k)
            {
                const auto x = high.v[k] < a.v[k] ? high.v[k] : a.v[k];
                r.v[k] = x < low.v[k] ? low.v[k] : x;
            }
            return r;
        }
       #endif

        //==============================================================================
        // tanh 的有理式逼近（Eigen generic_fast_tanh_float：分子 13 次奇多项式、分母 6 次偶多项式）。
        // 钳位常数取 Eigen「不用 FMA」那组：按下面先乘后加的求值顺序，p/q 在 ±7.9053 处恰好到 ±1。
        // （若被编译器合并成 FMA，钳位处只到 ±0.999999762，±Inf 也就不再恰为 ±1——BUG-002 实测，所以文件头要求不合并。）
        constexpr float tanhClamp = 7.90531110763549805f;
        constexpr float tanhTiny  = 0.0004f;

        constexpr float alpha1  = 4.89352455891786e-03f;
        constexpr float alpha3  = 6.37261928875436e-04f;
        constexpr float alpha5  = 1.48572235717979e-05f;
        constexpr float alpha7  = 5.12229709037114e-08f;
        constexpr float alpha9  = -8.60467152213735e-11f;
        constexpr float alpha11 = 2.00018790482477e-13f;
        constexpr float alpha13 = -2.76076847742355e-16f;

        constexpr float beta0 = 4.89352518554385e-03f;
        constexpr float beta2 = 2.26843463243900e-03f;
        constexpr float beta4 = 1.18534705686654e-04f;
        constexpr float beta6 = 1.19825839466702e-06f;

        CELADON_SIMD_INLINE Vec tanh4 (Vec v) noexcept
        {
            const auto x  = vClamp (v, vSplat (-tanhClamp), vSplat (tanhClamp));
            const auto x2 = vMul (x, x);

            auto p = vAdd (vMul (x2, vSplat (alpha13)), vSplat (alpha11));
            p = vAdd (vMul (x2, p), vSplat (alpha9));
            p = vAdd (vMul (x2, p), vSplat (alpha7));
            p = vAdd (vMul (x2, p), vSplat (alpha5));
            p = vAdd (vMul (x2, p), vSplat (alpha3));
            p = vAdd (vMul (x2, p), vSplat (alpha1));
            p = vMul (x, p);

            auto q = vAdd (vMul (x2, vSplat (beta6)), vSplat (beta4));
            q = vAdd (vMul (x2, q), vSplat (beta2));
            q = vAdd (vMul (x2, q), vSplat (beta0));

            // 很小的 x 直接返回 x：tanh(x) = x − x³/3 + …，|x| < 4e-4 时修正项不到半个 ulp；也保住了 ±0 的符号。
            return vSelect (vLess (vAbs (v), vSplat (tanhTiny)), v, vDiv (p, q));
        }

        // 对 data[0 … numSamples) 逐 4 个套用 op；不足 4 个的尾巴先补成 4 个（补 0）再算，
        // 所以每个样本走的都是同一段 4 路代码，结果与位置、长度无关。
        template <typename Op>
        CELADON_SIMD_INLINE void applyInFours (float* data, int numSamples, Op&& op) noexcept
        {
            int i = 0;

            for (; i + 4 <= numSamples; i += 4)
                vStore (data + i, op (vLoad (data + i)));

            if (i < numSamples)
            {
                float tail[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const auto remaining = numSamples - i;

                for (int k = 0; k < remaining; ++k)
                    tail[k] = data[i + k];

                vStore (tail, op (vLoad (tail)));

                for (int k = 0; k < remaining; ++k)
                    data[i + k] = tail[k];
            }
        }
    }

    //==============================================================================
    void Saturator::setCurve (Curve newCurve) noexcept
    {
        curve = newCurve;
    }

    void Saturator::setDrive (float newDrive) noexcept
    {
        drive = juce::jmax (1.0f, newDrive);
        updateTube();
    }

    void Saturator::setThreshold (float newThreshold) noexcept
    {
        threshold = juce::jmax (1.0e-6f, newThreshold);
        updateSoftClip();
    }

    void Saturator::setAsymmetry (float newAsymmetry) noexcept
    {
        asymmetry = newAsymmetry;
        updateTube();
    }

    void Saturator::setDriveAndAsymmetry (float newDrive, float newAsymmetry) noexcept
    {
        drive = juce::jmax (1.0f, newDrive);
        asymmetry = newAsymmetry;
        updateTube();
    }

    void Saturator::updateSoftClip() noexcept
    {
        kneeStart = threshold * 0.7f;
        kneeWidth = threshold - kneeStart;
    }

    void Saturator::updateTube() noexcept
    {
        // 原点斜率：d/dx [tanh(d·x + b)] 在 x = 0 处 = d · (1 − tanh²(b))。
        // 除以它之后，小信号时 y ≈ x，Heat 转大不会先把整体音量抬高。
        tanhOfAsymmetry = std::tanh (asymmetry);
        const auto slopeAtZero = drive * (1.0f - tanhOfAsymmetry * tanhOfAsymmetry);
        inverseSlopeAtZero = 1.0f / slopeAtZero;

        // 整段版本用同一个 fastTanh 算 tanh(b)：块里 x = 0 时 fastTanh (d·0 + b) 与它逐位相同，y(0) 精确为 0。
        fastTanhOfAsymmetry = fastTanh (asymmetry);
        const auto fastSlopeAtZero = drive * (1.0f - fastTanhOfAsymmetry * fastTanhOfAsymmetry);
        fastInverseSlopeAtZero = 1.0f / fastSlopeAtZero;
    }

    void Saturator::processTubeBlock (float* data, int numSamples) const noexcept
    {
        const auto d        = vSplat (drive);
        const auto b        = vSplat (asymmetry);
        const auto offset   = vSplat (fastTanhOfAsymmetry);
        const auto inverse  = vSplat (fastInverseSlopeAtZero);

        applyInFours (data, numSamples, [&] (Vec x) noexcept
        {
            return vMul (vSub (tanh4 (vAdd (vMul (d, x), b)), offset), inverse);
        });
    }

    void Saturator::fastTanhBlock (float* data, int numSamples) noexcept
    {
        applyInFours (data, numSamples, [] (Vec x) noexcept { return tanh4 (x); });
    }

    float Saturator::fastTanh (float x) noexcept
    {
        float lanes[4];
        vStore (lanes, tanh4 (vSplat (x)));
        return lanes[0];
    }
}
