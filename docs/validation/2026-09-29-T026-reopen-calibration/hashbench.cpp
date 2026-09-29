// 旧 conditionsHash（一份 FNV）与新 conditionsHashes（同一趟两份）每次调用的耗时。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
struct Fnv { std::uint32_t h = 2166136261u; void add (std::uint64_t v) noexcept { for (int i = 0; i < 8; ++i) { h ^= (std::uint32_t) ((v >> (8 * i)) & 0xffu); h *= 16777619u; } } };
static inline std::uint64_t q (float x) { return std::isfinite (x) ? (std::uint64_t) std::llround (std::clamp ((double) x * 1e4, -1e12, 1e12)) : 0x7ff8dead7ff8deadULL; }
struct P { float v[14]; bool b[5]; int speed; };
__attribute__((noinline)) std::uint32_t oldHash (const P& p) { Fnv h; for (int i = 0; i < 12; ++i) h.add (q (p.v[i])); h.add ((std::uint64_t) p.speed); for (int i = 0; i < 5; ++i) h.add (p.b[i] ? 1u : 0u); return h.h; }
__attribute__((noinline)) std::uint64_t newHashes (const P& p) { Fnv a, d; auto both = [&] (std::uint64_t w) { a.add (w); d.add (w); };
  for (int i = 0; i < 12; ++i) { if (i == 11) a.add (q (p.v[i])); else both (q (p.v[i])); } both ((std::uint64_t) p.speed); for (int i = 0; i < 5; ++i) both (p.b[i] ? 1u : 0u); return ((std::uint64_t) a.h << 32) | d.h; }
int main() {
  P p {}; for (int i = 0; i < 14; ++i) p.v[i] = 1.2345f * (float) (i + 1); p.speed = 1;
  const int n = 20000000; volatile std::uint64_t sink = 0;
  for (int rep = 0; rep < 3; ++rep) {
    auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < n; ++i) { p.v[0] += 1e-3f; sink = sink + oldHash (p); }
    auto t1 = std::chrono::steady_clock::now(); for (int i = 0; i < n; ++i) { p.v[0] += 1e-3f; sink = sink + newHashes (p); }
    auto t2 = std::chrono::steady_clock::now();
    std::printf ("old %.1f ns/call, new %.1f ns/call\n", std::chrono::duration<double, std::nano> (t1 - t0).count() / n, std::chrono::duration<double, std::nano> (t2 - t1).count() / n);
  }
}
