/*
  ==============================================================================

    LinearPhaseLowpass.cpp

  ==============================================================================
*/

#include "LinearPhaseLowpass.h"

#include <cmath>

// MSVC：VS2022（v143）的 /fp:precise 默认不融合乘加；更早的工具集（v142）配 /arch:AVX2 时 /fp:precise 下仍可能融合，
// 这里写死关掉（ADR-013：各路径逐位相同的前提）。clang-cl 不认这个 pragma，由 /clang:-ffp-contract=off 与源码守卫管。
#if defined (_MSC_VER) && ! defined (__clang__)
 #pragma fp_contract (off)
#endif

namespace celadon
{
    namespace
    {
        // 输入钳位：±1e4（约 +80 dBFS）以外的样本钳到边界，非有限值当 0（理由见头文件私有部分）。取这么大是为了让任何
        // 真实音频（包括宿主内部远超 0 dBFS 的浮点信号）都原样通过；滑动和的量级因此最多 1e4 · L，舍入误差仍在 1e-12 · L 以内。
        constexpr double inputLimit = 1.0e4;

        // lengthFor 的上限只防 lround 溢出（截止频率趋近 0 时）；实际用到的长度最多几千。
        constexpr double maxComputedLength = 1.0e7;
    }

    int LinearPhaseLowpass::lengthFor (double sampleRate, double cutoffHz) noexcept
    {
        // 非正 / 非有限的采样率或截止频率没有意义：给最短的 2（调用方本来就不该传；Clay 与分析器都先钳过）。
        if (! (sampleRate > 0.0) || ! (cutoffHz > 0.0) || ! std::isfinite (sampleRate) || ! std::isfinite (cutoffHz))
            return 2;

        // L = round(0.24 · fs / fc)：4 个长度 L 的矩形窗级联，低通 −3 dB 点约在 fc（设计规格-Clay-根治修订 第 2 节）。
        const auto exactLength = lengthScale * sampleRate / cutoffHz;
        return juce::jmax (2, static_cast<int> (std::lround (juce::jmin (exactLength, maxComputedLength))));
    }

    void LinearPhaseLowpass::prepare (int newMaxLength)
    {
        maxLength = juce::jmax (2, newMaxLength);
        history.assign (static_cast<std::size_t> (numStages) * static_cast<std::size_t> (maxLength), 0.0);
        reset (maxLength);
    }

    void LinearPhaseLowpass::reset (int newLength) noexcept
    {
        length = juce::jlimit (2, maxLength, newLength);
        inverseLength = 1.0 / static_cast<double> (length);

        // 不清历史环（T-020）：reset 之后的头 length 个样本写入的格正好是从 0 起还没写过的那些，process 在这期间把
        // 「length 个样本之前的输入」直接当 0，与先把格清零再读出来的算术逐位相同。这样 reset 是 O(1)——Clay 切换分频点时
        // 在一块里清 4 × L 个 double（192 kHz / 40 Hz 为 37 KB）的尖峰没有了。
        sums.fill (0.0);
        writeIndex = 0;
        written = 0;
        samplesUntilResync = resyncIntervalSamples;
    }

    double LinearPhaseLowpass::process (double input) noexcept
    {
        // 没 prepare 过就没有历史环：输出 0（当作「此前输入全是 0」），不访问空的 vector。
        if (history.empty())
            return 0.0;

        // 输入清洗：|x| ≤ 1e4 原样；更大的钳到 ±1e4；NaN 两个比较都不成立 → 0。
        auto x = std::abs (input) <= inputLimit ? input
                                                : (input > inputLimit ? inputLimit : (input < -inputLimit ? -inputLimit : 0.0));

        // 各级：取出 length 个样本之前的输入、写入本级输入，滑动和加上两者之差，本级输出 = 和 / L（乘预先算好的 1 / L）。
        // 相减与相加分成两步写（和 + (新 − 旧)），对取负精确对称：输入整体取负时每一级的和与输出都恰好取负（Clay 的左右互换对称）。
        // reset 之后的头 length 个样本：本样本要写的格还没写过（写位置从 0 起，与已写个数相同），「length 个样本之前的输入」是 0。
        auto* stageSlot = history.data() + writeIndex;
        const auto slotWritten = written == length;

        for (int stage = 0; stage < numStages; ++stage)
        {
            const auto oldest = slotWritten ? *stageSlot : 0.0;
            *stageSlot = x;

            auto& sum = sums[static_cast<std::size_t> (stage)];
            sum += x - oldest;
            x = sum * inverseLength;

            stageSlot += maxLength;
        }

        writeIndex = writeIndex + 1 == length ? 0 : writeIndex + 1;

        if (! slotWritten)
            ++written;

        // 固定网格（从 reset 起每 resyncIntervalSamples 个样本，与调用方怎么切块无关）：按历史环重新求和。
        if (--samplesUntilResync == 0)
        {
            resync();
            samplesUntilResync = resyncIntervalSamples;
        }

        return x;
    }

    void LinearPhaseLowpass::resync() noexcept
    {
        // 各级的和换成历史环里 length 个值按下标顺序（0 → length − 1）直接相加：递推里累积的舍入误差清零，换成一次 length 项求和的
        // 误差（与递推无关、有界）。顺序固定，结果只由历史环的内容决定——同一输入序列任意切块逐位相同
        // （前提：编译器不合并乘加，ADR-013）。
        // 静音时它还负责「回到精确的 0」：递推的和在信号离开后常留下 1e-16 量级的残差（加减不严格抵消），重算后第 1 级的和精确为 0，
        // 残差逐级传下去，最多 numStages 个网格周期后整个状态精确归零。
        // 只加写过的格（reset 之后还没满 length 个样本时，没写过的格代表 0；正常用法里重算网格远在填满之后，written == length）。
        for (int stage = 0; stage < numStages; ++stage)
        {
            const auto* stageHistory = history.data() + static_cast<std::size_t> (stage) * static_cast<std::size_t> (maxLength);
            double total = 0.0;

            for (int i = 0; i < written; ++i)
                total += stageHistory[i];

            sums[static_cast<std::size_t> (stage)] = total;
        }
    }
}
