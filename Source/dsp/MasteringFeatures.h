#pragma once
#include <array>

namespace celadon
{
    enum class HarshMode : int { automatic = 0, off = 1, manual = 2 };
    enum class TargetMode : int { off = 0, automatic = 1, warm = 2, neutral = 3, bright = 4, reference = 5 };
    enum class DitherMode : int { off = 0, bits16 = 1, bits24 = 2 };
    enum class ReferenceState : int { idle = 0, waitingForSidechain = 1, learning = 2, ready = 3 };

    // Neutral defaults preserve old callers. New-instance defaults live in Parameters.cpp.
    struct MasteringSettings
    {
        HarshMode harshMode = HarshMode::off;
        float harshAmount = 50.0f, transientPreserve = 50.0f;
        TargetMode targetMode = TargetMode::off;
        float targetAmount = 35.0f, codecMarginDb = 0.0f;
        DitherMode dither = DitherMode::off;
        float previewTargetLufs = -14.0f;
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
