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

        // 解析后的实际强度（0 ～ 1，交给 Balance 的 h、t）——实际参数的无损回喂通道（T-032 R10-01，2026-09-29）。
        // 链按「模式 × GLAZE」算 h、t（CeladonChain::setParams；unit(amount) × glaze 不能从 0 ～ 100 的 amount 逐位复现）。
        // getEffectiveParams().mastering 回写的是「交给模块的实际值」：这两项填算好的那个 float，harshMode 改成手动（关仍是关）、
        // targetMode 改成解析出的显式形状（自动按风格落到温润 / 中性 / 明亮；「参考」还没学好 → 关），codecMarginDb 置 0
        //（余量已扣进 limit.ceilingDb，回喂不再扣第二次）。再把它喂给链（离线工具的同决策重放，ADR-016）时，模式不是「关」且
        // 这两项 ≥ 0 就直接用、不再经乘法，逐位同值。插件从不设它们（readParams 不填），保持 −1 = 未设，链照旧算，逐位不变。
        float harshResolved = -1.0f, targetResolved = -1.0f;

        // T-038：解析后的目标频谱斜率（同上，实际参数的无损回喂通道）。House 档「目标频谱 = 自动」按曲风取斜率（Melodic 的 −1.25
        // 不是任何一种显式形状），回写时置 targetSlopeResolvedSet、填算好的斜率；回喂时直接用。插件从不设，链照旧按形状算，逐位不变。
        bool targetSlopeResolvedSet = false;
        float targetSlopeResolvedDbPerOct = 0.0f;
    };

    struct MasteringStatus
    {
        ReferenceState referenceState = ReferenceState::idle;
        double referenceSeconds = 0.0;
        bool hasReference = false, sidechainConnected = false, previewEnabled = false;
        float harshReductionDb = 0.0f, previewGainDb = 0.0f, codecMarginDb = 0.0f;
        // T-078：界面判断「超出上限」用的 Ceiling，由链在 setParams 里发布（CeladonChain）。
        // 限幅器在干活时 = 扣过编码余量、做过 −4.5 ～ 0 钳位、流媒体档再封顶之后交给限幅器的值
        // （与 getEffectiveParams().limit.ceilingDb 相同）。LIMIT 旁通或干路（宿主旁通 / 听原声）时 = 用户 Ceiling
        // （不扣余量、不封顶），与 codecMarginDb 在旁通时发布 0、界面因此显示用户 Ceiling 的口径一致。
        // 默认 −1.0 是参数默认值；第一次 setParams 之前界面读到的就是它。
        float effectiveCeilingDb = -1.0f;
        std::array<float, 27> targetCorrectionDb {};
        DitherMode dither = DitherMode::off;
    };
}
