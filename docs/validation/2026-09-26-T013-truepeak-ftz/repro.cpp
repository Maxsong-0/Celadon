// BUG-001 复现：x86 上 FTZ / DAZ 对「取大」的三种写法的影响。
// 独立程序，不依赖 JUCE；在 x86_64 Linux 服务器上用 g++ -O2（与 CMake Release 同级）与 -O0 各编一次。
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

static std::uint32_t bitsOf (float x) { std::uint32_t u; std::memcpy (&u, &x, 4); return u; }

// 1. juce::jmax 的写法（参照实现 directFormPeak 用的）。
template <typename T> __attribute__ ((noinline)) T jmaxLike (T a, T b) { return a < b ? b : a; }

// 2. 现在 SSE2 核的 keepLarger：cmpltps + and / andnot / or 混合。
__attribute__ ((noinline)) __m128 blendKeepLarger (__m128 peak, __m128 candidate)
{
    const auto larger = _mm_cmplt_ps (peak, candidate);
    return _mm_or_ps (_mm_and_ps (larger, candidate), _mm_andnot_ps (larger, peak));
}

// 3. 显式 maxps / maxss（候选在前、峰值在后 = jmax (peak, candidate) 的 IEEE 语义）。
__attribute__ ((noinline)) __m128 maxpsKeepLarger (__m128 peak, __m128 candidate) { return _mm_max_ps (candidate, peak); }
__attribute__ ((noinline)) float maxssKeepLarger (float peak, float candidate)
{
    return _mm_cvtss_f32 (_mm_max_ss (_mm_set_ss (candidate), _mm_set_ss (peak)));
}

int main()
{
    const float denormal = 1.0e-40f;                       // 非规格化数
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const unsigned int modes[] = { 0u, 0x8000u, 0x0040u, 0x8040u };
    const char* names[] = { "IEEE", "FTZ only", "DAZ only", "FTZ+DAZ (juce::ScopedNoDenormals)" };
    const unsigned int saved = _mm_getcsr();

    struct Case { float peak, candidate; const char* what; };
    const Case cases[] = {
        { denormal, 0.0f,     "peak=denormal, cand=0      " },
        { 0.0f,     denormal, "peak=0,        cand=denormal" },
        { denormal, 1.0f,     "peak=denormal, cand=1      " },
        { 2.0e-40f, denormal, "peak=2e-40,    cand=1e-40  " },
        { denormal, nan,      "peak=denormal, cand=NaN    " },
        { nan,      denormal, "peak=NaN,      cand=denormal" },
    };

    for (int m = 0; m < 4; ++m)
    {
        _mm_setcsr ((saved & ~0x8040u) | modes[m]);
        std::printf ("== MXCSR mode: %s\n", names[m]);

        for (const auto& c : cases)
        {
            const auto a = jmaxLike (c.peak, c.candidate);
            const auto b = _mm_cvtss_f32 (blendKeepLarger (_mm_set1_ps (c.peak), _mm_set1_ps (c.candidate)));
            const auto p = _mm_cvtss_f32 (maxpsKeepLarger (_mm_set1_ps (c.peak), _mm_set1_ps (c.candidate)));
            const auto s = maxssKeepLarger (c.peak, c.candidate);
            std::printf ("  %s  jmax=%08x  blend=%08x  maxps=%08x  maxss=%08x%s\n", c.what,
                         bitsOf (a), bitsOf (b), bitsOf (p), bitsOf (s),
                         (bitsOf (a) == bitsOf (b) && bitsOf (p) == bitsOf (s) && bitsOf (a) == bitsOf (p)) ? "" : "   <-- differ");
        }
    }

    _mm_setcsr (saved);
    return 0;
}
