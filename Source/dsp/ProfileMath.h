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
}
