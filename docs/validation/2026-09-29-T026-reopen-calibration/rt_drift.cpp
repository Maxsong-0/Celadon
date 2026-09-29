// 穷举 [s, e] 内每个 float 值，量 Celadon logRange 的归一化往返（to -> from，含 JUCE 的 clamp）k 次后的漂移（ulp）。
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
static inline float clamp01 (float p) { return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p); }
static inline float snapClamp (float v, float s, float e) { return (v <= s || e <= s) ? s : (v >= e ? e : v); }
static inline float toN (float s, float e, float v) { return clamp01 (std::log (snapClamp (v, s, e) / s) / std::log (e / s)); }
static inline float fromN (float s, float e, float p) { p = clamp01 (p); return snapClamp (s * std::pow (e / s, p), s, e); }
static inline float rt (float s, float e, float v) { return fromN (s, e, toN (s, e, v)); }
static inline int64_t ord (float f) { int32_t i; std::memcpy (&i, &f, 4); return i; }
static inline int64_t ulpd (float a, float b) { return std::llabs (ord (a) - ord (b)); }
int main()
{
    struct R { const char* name; float s, e; } ranges[] = { { "release", 10.0f, 1000.0f }, { "lowcut", 10.0f, 60.0f }, { "mono", 40.0f, 300.0f } };
    for (auto r : ranges)
    {
        int64_t maxD[9] = {}, maxFromRt1[9] = {};
        int64_t count = 0, flip1 = 0, flipAny = 0, notFixed1 = 0;
        double maxRel = 0;
        for (float v = r.s; v <= r.e; v = std::nextafter (v, 2.0f * r.e))
        {
            ++count;
            float x = v, x1 = 0;
            bool flipped = false;
            for (int k = 1; k <= 8; ++k)
            {
                x = rt (r.s, r.e, x);
                if (k == 1) x1 = x;
                maxD[k] = std::max (maxD[k], ulpd (x, v));
                maxFromRt1[k] = std::max (maxFromRt1[k], ulpd (x, x1));
                maxRel = std::max (maxRel, std::abs ((double) x - v) / v);
                if (std::llround ((double) x * 1e4) != std::llround ((double) v * 1e4)) { flipped = true; if (k == 1) ++flip1; }
            }
            if (flipped) ++flipAny;
            if (rt (r.s, r.e, x1) != x1) ++notFixed1;
        }
        std::printf ("%s [%g, %g]: floats=%lld flip@k1=%.3f%% flipAny(k<=8)=%.3f%% RT1-not-fixed=%.3f%% maxRel=%.3g\n", r.name, r.s, r.e,
                     (long long) count, 100.0 * flip1 / count, 100.0 * flipAny / count, 100.0 * notFixed1 / count, maxRel);
        std::printf ("  max ulp from v   k=1..8:"); for (int k = 1; k <= 8; ++k) std::printf (" %lld", (long long) maxD[k]); std::printf ("\n");
        std::printf ("  max ulp from RT1 k=1..8:"); for (int k = 1; k <= 8; ++k) std::printf (" %lld", (long long) maxFromRt1[k]); std::printf ("\n");
    }
}
