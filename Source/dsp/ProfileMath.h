/*
  ==============================================================================

    ProfileMath.h
    画像口径的共享常数与小函数（T-048 阶段 1 遗留 2，阶段 2 代理 B 抽出）。

    原先 MusicAnalyzer.cpp 与 LiveProfile.cpp 的匿名命名空间各有一份：约 50 个口径常数，以及
    energyToLufs、histogramBinOf、firstBinAtOrAbove、binCentreLufs、fraction、setFeature、
    sideToMidDbOf、correlationOf（外加两边都有的 invalidate）。分析器改读这一份。

    LiveProfile.cpp 里的抄本先留着（发布语义不在本代理的主改范围）。集成时改为 include 本头，
    删掉那边的同名常数与函数。有限输入上 firstBinAtOrAbove 与抄本逐位相同；抄本把 NaN 当最低格
    （`!(position > 0)`），本头与原来的分析器一样（`position <= 0`）。集成时若要保留 NaN 兜底，
    用 `!(position > 0.0)` 那一式，有限值不变。

    四个收单候选本阶段停算（Q6）：MusicProfile::monoPeakCostDb 的布局不动，valid 保持 false。
    LiveProfile 发布处要跟着把这四路标成不知道，不要把停算后的 0 峰值写成一个有效的 −120 dB。

  ==============================================================================
*/

#pragma once

#include "GlazeTypes.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <limits>

namespace celadon::glaze
{
    //==============================================================================
    // 响度直方图与相对门（EBU R128 / Tech 3342；与 SegmentLedger 的响度直方图同格）。
    inline constexpr double integratedRelativeGateLu = -10.0;
    inline constexpr double lraRelativeGateLu        = -20.0;
    inline constexpr double lraLowFraction  = 0.10;
    inline constexpr double lraHighFraction = 0.95;
    inline constexpr int minimumLraValues = 10;

    inline constexpr double histogramMinLufs = -70.0;
    inline constexpr int histogramBinsPerLu = 10;
    inline constexpr int histogramBins = 80 * histogramBinsPerLu;

    //==============================================================================
    // 各特征「观测充分」与「给得出数值」的时长（秒）。置信度 = min(观测充分度, 代表性)。
    inline constexpr double loudnessFullSeconds = 10.0;
    inline constexpr double lraFullSeconds      = 60.0;
    inline constexpr double spectrumFullSeconds = 10.0;
    inline constexpr double onsetFullSeconds    = 10.0;
    inline constexpr double minimumSpectrumSeconds = 1.0;
    inline constexpr double minimumOnsetSeconds    = 1.0;
    inline constexpr int minimumOnsetEvents = 3;

    //==============================================================================
    // 频谱证据、占比、滚降。
    inline constexpr double bandGateDb = -60.0;
    inline constexpr double recoverPersistenceStart = 0.5;
    inline constexpr double recoverPersistenceFull  = 0.8;
    inline constexpr double rollOffStepDb = 3.0;
    inline constexpr int lowShareLastBand   = 4;
    inline constexpr int highShareFirstBand = 22;
    inline constexpr int minimumSlopeBands  = 6;
    inline constexpr double noDataDb = -150.0;

    // 低频逐帧电平直方图（−120 … +20 dB，0.5 dB 一格）。lowRangeMinDb 只在分析器里用，不在这里。
    inline constexpr double lowRangeBinDb = 0.5;
    inline constexpr int lowRangeBins = 280;
    inline constexpr double lowRangeLowFraction  = 0.10;
    inline constexpr double lowRangeHighFraction = 0.90;

    //==============================================================================
    // 起音强度直方图，以及组网格（实时段落账的块网格与它对齐）。
    inline constexpr double onsetGroupSeconds = 1.0 / 3000.0;
    inline constexpr int maximumGroupLength = 128;
    inline constexpr double onsetStrengthCapDb = 30.0;
    inline constexpr double strengthBinDb = 0.25;
    inline constexpr int strengthBins = (int) (onsetStrengthCapDb / strengthBinDb) + 1;
    inline constexpr double minimumOnsetAgreement = 0.5;

    //==============================================================================
    // 真次声与立体声组（画像格式 2）。
    inline constexpr double subShareFloor = 1.0e-15;
    inline constexpr double minimumSubSeconds = 1.0;
    inline constexpr double subFullSeconds = 10.0;
    inline constexpr int lowStereoFirstBand = 0;
    inline constexpr int lowStereoLastBand = 5;
    inline constexpr int highStereoFirstBand = 17;
    inline constexpr int highStereoLastBand = 26;
    inline constexpr double stereoRatioFloor = 1.0e-12;
    inline constexpr double minimumStereoSeconds = 1.0;
    inline constexpr double stereoFullSeconds = 10.0;

    //==============================================================================
    inline double energyToLufs (double meanSquare) noexcept
    {
        return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare)
                                : -std::numeric_limits<double>::infinity();
    }

    inline int histogramBinOf (double lufs) noexcept
    {
        return juce::jlimit (0, histogramBins - 1,
                             (int) std::floor ((lufs - histogramMinLufs) * (double) histogramBinsPerLu));
    }

    // 格中心 ≥ gate 的第一格（可能等于 histogramBins：没有一格在门之上）。
    inline int firstBinAtOrAbove (double gate) noexcept
    {
        const auto position = (gate - histogramMinLufs) * (double) histogramBinsPerLu - 0.5;

        if (position <= 0.0)
            return 0;

        if (position >= (double) histogramBins)
            return histogramBins;

        return (int) std::ceil (position);
    }

    inline double binCentreLufs (int bin) noexcept
    {
        return histogramMinLufs + ((double) bin + 0.5) / (double) histogramBinsPerLu;
    }

    inline float fraction (double numerator, double fullScale) noexcept
    {
        return fullScale > 0.0 ? (float) juce::jlimit (0.0, 1.0, numerator / fullScale) : 0.0f;
    }

    inline void invalidate (Feature& feature) noexcept
    {
        feature = Feature {};
    }

    inline void setFeature (Feature& feature, double value, double effectiveSeconds) noexcept
    {
        feature.value = (float) value;
        feature.valid = true;
        feature.effectiveSeconds = effectiveSeconds;
    }

    // S/M：P_M、P_S 各按 P 的 1e-12 封底。power = ΣP > 0，cross = ΣX。
    inline double sideToMidDbOf (double power, double cross) noexcept
    {
        const auto floorPower = stereoRatioFloor * power;
        const auto mid = juce::jmax (floorPower, 0.5 * (power + cross));
        const auto side = juce::jmax (floorPower, 0.5 * (power - cross));
        return 10.0 * std::log10 (side / mid);
    }

    // ρ = ΣX / ΣP，钳在 [−1, 1]。
    inline double correlationOf (double power, double cross) noexcept
    {
        return juce::jlimit (-1.0, 1.0, cross / power);
    }

    //==============================================================================
    // 3 s 短期窗（T-097）：最近 30 个 100 ms 子块（LoudnessMeter::SubBlock）的 K 加权均方与真峰值。
    // 口径同 MusicAnalyzer 的短期视图：S = 30 个子块均方的平均（EBU Tech 3341，不门限、不平滑）；
    // PSR₃ₛ = 3 s 内真峰值最大值 − S；S 在绝对门 −70 LUFS 之下（近乎静音）时两者都无效。
    // 守护族（落差守门、PSR 守门）在入口 / 出口各持一份，按子块推进——同一串子块不论宿主怎么切块，读数都逐位相同。
    // 定长、不分配、不加锁。
    class ShortTermWindow
    {
    public:
        static constexpr int subBlocks = 30;
        static constexpr double absoluteGateLufs = -70.0;

        void clear() noexcept
        {
            count = 0;
            write = 0;
        }

        // 非有限或为负的子块（链里出过 NaN 的那一块）断开窗口：之前的子块不再与之后的拼成 3 s。
        void push (double meanSquare, float truePeakLinear) noexcept
        {
            if (! std::isfinite (meanSquare) || meanSquare < 0.0 || ! std::isfinite (truePeakLinear))
            {
                clear();
                return;
            }

            energy[(size_t) write] = meanSquare;
            peak[(size_t) write] = truePeakLinear;
            write = (write + 1) % subBlocks;
            count = juce::jmin (subBlocks, count + 1);
        }

        bool isFull() const noexcept   { return count >= subBlocks; }

        // S（LUFS）。窗不满 3 s，或低于绝对门：false，lufs 不动。
        bool shortTermLufs (double& lufs) const noexcept
        {
            if (count < subBlocks)
                return false;

            double sum = 0.0;

            for (int back = 1; back <= subBlocks; ++back)   // 从新到旧，求和顺序固定（同分析器的 windowSum）
                sum += energy[(size_t) ((write - back + subBlocks) % subBlocks)];

            const auto loudness = energyToLufs (sum / (double) subBlocks);

            if (! (loudness >= absoluteGateLufs))
                return false;

            lufs = loudness;
            return true;
        }

        // PSR₃ₛ（dB）。S 无效，或 3 s 内真峰值为 0：false，psrDb 不动。
        bool psrDb (double& psr) const noexcept
        {
            double loudness = 0.0;

            if (! shortTermLufs (loudness))
                return false;

            float maxPeak = 0.0f;

            for (const auto value : peak)
                maxPeak = juce::jmax (maxPeak, value);

            if (! (maxPeak > 0.0f))
                return false;

            psr = 20.0 * std::log10 ((double) maxPeak) - loudness;
            return true;
        }

    private:
        std::array<double, (size_t) subBlocks> energy {};
        std::array<float, (size_t) subBlocks> peak {};
        int count = 0;
        int write = 0;
    };

    //==============================================================================
    // 子块守门（落差守门、PSR 守门）共用的口径（T-097）。两个守门各写一份时，改一处就得记得改另一处（复查第 2 轮 R2-F14）。

    // T-097 之前（0.1.8.10）两个守门的底，也是守门对象没设过下限时的默认值。
    inline constexpr float legacyGuardFloorDb = -3.0f;

    // 存档里写给 0.1.8.10 的退让（reliefDb 属性）。0.1.8.10 读档直接拿 floorDb 当底、从不另设，退让也恒 ≥ 底；在它那里再存，
    // 两项原样写回。所以 floorDb 恒写 −3、reliefDb 写 max(−3, 退让)：降级打开时退让最多 3 dB（比 −3 深的在载入那一刻变浅，
    // 播放中不跳），底也一直是 −3。比 −3 深的真实退让另写 T-097 的可选属性 reliefDbExact，新代码读档优先读它（复查第 2 轮 R2-F1）。
    inline float legacyGuardSavedReliefDb (float reliefDb) noexcept
    {
        return juce::jmax (legacyGuardFloorDb, reliefDb);
    }

    // 子块守门这一步之后的 engaged：本步在退，或已退到底（bottomDb = floorDb 与界中较浅的那个）且本步读数仍超门槛
    // （hasReading 时的 stillOver）。到界停住、或比界深而保持时也算在起作用：状态行「已经退到底仍超」靠它（同预算环
    // planLimited && lastOver，看此刻超没超、不等持续）。底为 0（界为 0 = 没有自动部分，applyLimitExtras 一点也退不了）时不算，
    // 哪怕早先退下的量还保持着。
    // 本步没有读数（非有效音乐，或断窗后 3 s 窗还在重攒、近乎静音）时保持上一步的：跳播、循环回绕、冻结结束、读档之后约 3 s，
    // 状态行不该从「受限」掉回「跟随」再跳回来（退让一直在执行）。底回到 0、或这个守门此刻关着（本曲风不开）时清掉：关着的守门
    // 不做判定，不会「到底仍超」（复查第 2 轮 R2-F3：停播时换到关着的曲风，状态行不该一直显示受限）。
    inline bool guardEngagedAfterStep (bool hasReading, bool enabled, bool wasEngaged, float reliefBeforeDb, float reliefDb,
                                       bool stillOver, float bottomDb) noexcept
    {
        if (! hasReading)
            return wasEngaged && enabled && bottomDb < 0.0f;

        return reliefDb < reliefBeforeDb || (stillOver && bottomDb < 0.0f && reliefDb <= bottomDb);
    }
}
