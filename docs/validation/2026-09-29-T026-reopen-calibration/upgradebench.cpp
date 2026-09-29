// upgradeLegacyConditions 最坏情形（Release 990 ms、低切 58 Hz 与收单 290 Hz 都「手动」、旧参考对不上任何候选、不早退）的耗时。
#include "Celadon/Source/dsp/GlazeController.h"
#include <chrono>
#include <cstdio>
using namespace celadon;
int main()
{
    ChainParams m;
    m.limit.releaseMs = 990.0f;
    m.claySettings.lowCutMode = m.claySettings.monoMode = ClayMode::manual;
    m.claySettings.lowCutHz = 58.0f;
    m.claySettings.monoHz = 290.0f;
    const int n = 2000;
    double worst = 0.0, total = 0.0;
    int upgraded = 0;
    for (int i = 0; i < n; ++i)
    {
        glaze::CalibrationReference a, b;
        a.manualParamsHash = 0x12345678u + (std::uint32_t) i;   // 对不上
        b.manualParamsHash = 0x9abcdef0u + (std::uint32_t) i;
        const auto t0 = std::chrono::steady_clock::now();
        upgraded += glaze::upgradeLegacyConditions ({ &a, &b }, m, 1.0e9f);
        const auto us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
        worst = us > worst ? us : worst;
        total += us;
    }
    std::printf ("worst-case upgrade: mean %.1f us, max %.1f us over %d calls (upgraded %d)\n", total / n, worst, n, upgraded);
}
