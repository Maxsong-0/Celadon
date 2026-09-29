// T-027 单次开销：makeReference 手动参数部分的两条路径，每次调用多少 ns（107，与测试同一套 GCC Release 选项）。
//   - 重算：glaze::setManualConditions（真函数，链接改后的 GlazeController.o；改前每次 makeReference 都走它）；
//   - 命中：缓存键比较（samePlanningManual 逐位比 + 约束反馈上限逐位比）+ 整份参考拷贝。samePlanningManual / sameClaySettings
//     在 GlazeController.cpp 的匿名命名空间里、链接不到，这里逐字照抄（同一套编译选项）。
// 两种手动设置：Clay 默认（最常见）与 Clay 三项都「手动」（哈希里项最多）。
#include "GlazeController.h"

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace celadon;

namespace
{
    bool sameBits (float a, float b) noexcept
    {
        return std::memcmp (&a, &b, sizeof (float)) == 0;
    }

    // ---- 照抄 GlazeController.cpp ----
    bool sameClaySettings (const ClaySettings& a, const ClaySettings& b) noexcept
    {
        return a.lowCutMode == b.lowCutMode && a.monoMode == b.monoMode && a.widthMode == b.widthMode
            && sameBits (a.lowCutHz, b.lowCutHz) && sameBits (a.monoHz, b.monoHz) && sameBits (a.widthDb, b.widthDb)
            && a.bypass == b.bypass;
    }

    bool samePlanningManual (const ChainParams& a, const ChainParams& b) noexcept
    {
        return sameClaySettings (a.claySettings, b.claySettings)
            && sameBits (a.balance.recover, b.balance.recover) && sameBits (a.balance.tame, b.balance.tame)
            && sameBits (a.balance.brighten, b.balance.brighten) && a.balance.speed == b.balance.speed
            && a.balance.bypass == b.balance.bypass
            && sameBits (a.color.toneDb, b.color.toneDb) && sameBits (a.color.glue, b.color.glue)
            && sameBits (a.color.heat, b.color.heat) && sameBits (a.color.mixPercent, b.color.mixPercent)
            && sameBits (a.color.outputDb, b.color.outputDb) && a.color.bypass == b.color.bypass
            && sameBits (a.limit.gainDb, b.limit.gainDb) && sameBits (a.limit.ceilingDb, b.limit.ceilingDb)
            && sameBits (a.limit.releaseMs, b.limit.releaseMs) && a.limit.autoRelease == b.limit.autoRelease
            && sameBits (a.limit.lookaheadMs, b.limit.lookaheadMs) && sameBits (a.limit.clip, b.limit.clip)
            && a.limit.bypass == b.limit.bypass;
    }
    // ---- 照抄完 ----

    struct Cache
    {
        glaze::CalibrationReference conditions;
        ChainParams input;
        float feedback = 0.0f;
        bool known = false;
    };

    __attribute__ ((noinline)) glaze::CalibrationReference recompute (const ChainParams& m, float feedback)
    {
        glaze::CalibrationReference r;
        glaze::setManualConditions (r, m, feedback);
        return r;
    }

    __attribute__ ((noinline)) glaze::CalibrationReference cached (Cache& c, const ChainParams& m, float feedback)
    {
        if (! c.known || ! samePlanningManual (m, c.input) || ! sameBits (feedback, c.feedback))
        {
            c.conditions = glaze::CalibrationReference {};
            glaze::setManualConditions (c.conditions, m, feedback);
            c.input = m;
            c.feedback = feedback;
            c.known = true;
        }

        return c.conditions;
    }

    ChainParams typical (bool clayManual)
    {
        ChainParams p;
        p.glazePercent = 60.0f;
        p.balance.recover = 12.5f;
        p.balance.tame = 8.0f;
        p.color.toneDb = 0.4f;
        p.color.glue = 22.0f;
        p.color.heat = 18.0f;
        p.limit.gainDb = 3.25f;
        p.limit.releaseMs = 137.9f;
        p.limit.autoRelease = false;

        if (clayManual)
        {
            p.claySettings.lowCutMode = ClayMode::manual;
            p.claySettings.monoMode = ClayMode::manual;
            p.claySettings.widthMode = ClayMode::manual;
            p.claySettings.lowCutHz = 27.3f;
            p.claySettings.monoHz = 131.7f;
            p.claySettings.widthDb = 0.8f;
        }

        return p;
    }
}

int main()
{
    constexpr int n = 20000000;
    volatile std::uint32_t sink = 0;

    for (const auto clayManual : { false, true })
    {
        const auto params = typical (clayManual);
        const auto feedback = 9.75f;
        Cache cache;

        for (int rep = 0; rep < 5; ++rep)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i)
                sink = sink + recompute (params, feedback).discreteParamsHash;

            const auto t1 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i)
                sink = sink + cached (cache, params, feedback).discreteParamsHash;

            const auto t2 = std::chrono::steady_clock::now();
            std::printf ("clay %s: recompute %.1f ns/call, cache hit %.1f ns/call\n", clayManual ? "manual " : "default",
                         std::chrono::duration<double, std::nano> (t1 - t0).count() / n,
                         std::chrono::duration<double, std::nano> (t2 - t1).count() / n);
        }
    }

    return (int) (sink & 0u);
}
