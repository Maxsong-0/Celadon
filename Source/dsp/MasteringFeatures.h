#pragma once
#include <array>

namespace celadon
{
    enum class HarshMode : int { automatic = 0, off = 1, manual = 2 };
    enum class TargetMode : int { off = 0, automatic = 1, warm = 2, neutral = 3, bright = 4, reference = 5 };
    enum class DitherMode : int { off = 0, bits16 = 1, bits24 = 2 };
    enum class ReferenceState : int { idle = 0, waitingForSidechain = 1, learning = 2, ready = 3 };

    // Neutral defaults preserve old callers. New-instance defaults live in ParameterDefaults.h (Parameters.cpp reads them).
    struct MasteringSettings
    {
        HarshMode harshMode = HarshMode::off;
        float harshAmount = 50.0f, transientPreserve = 50.0f;
        TargetMode targetMode = TargetMode::off;
        float targetAmount = 35.0f, codecMarginDb = 0.0f;
        DitherMode dither = DitherMode::off;
        float previewTargetLufs = -14.0f;

        // 解析后的实际强度——实际参数的无损回喂通道（T-032 R10-01，2026-09-29）。
        // 链按「模式 × GLAZE」算出交给 Balance 的 h、t（CeladonChain::setParams；unit(amount) × glaze 不能从 0 ～ 100 的 amount 逐位
        // 复现），getEffectiveParams().mastering 回写时把算好的那个 float 填进这里；再喂给链（离线工具的同决策重放，ADR-016）时
        // ≥ 0 就直接用、不再经乘法，逐位同值。插件从不设它们（readParams 不填），保持 −1 = 未设，链照旧算，逐位不变。
        float harshResolved = -1.0f, targetResolved = -1.0f;
        TargetMode targetResolvedMode = TargetMode::off;   // targetResolved ≥ 0 时的目标形状（自动已按风格解析；「参考」且没学好 → off）
    };

    struct MasteringStatus
    {
        ReferenceState referenceState = ReferenceState::idle;
        double referenceSeconds = 0.0;
        bool hasReference = false, sidechainConnected = false, previewEnabled = false;
        float harshReductionDb = 0.0f, previewGainDb = 0.0f, codecMarginDb = 0.0f;
        std::array<float, 27> targetCorrectionDb {};
        DitherMode dither = DitherMode::off;
    };
}
