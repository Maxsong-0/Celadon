/*
  ==============================================================================

    LegacyFixtureTests.cpp
    v0.1.6.2 真实存档夹具的回归测试（T-012 阶段 4，审核 V2-04）。

    夹具不是用现源码「伪造的旧存档」：它由 v0.1.6.2 的备份源码（Celadon-releases/source-baseline-2026-09-26-v0.1.6.2，
    只为在 Linux GCC 上编过把裸 size_t 改成 std::size_t）在测试服务器 107 上编出的二进制写出——出厂常数、48 kHz / 512、
    下面 makeFixtureLoop 的 8 s 循环素材、GLAZE 60、风格自动、手动 Recover 12.5 / Glue 20 / Limit Gain 2 / Ceiling −1，
    处理 150 s 后已锁定、校准有效，再 createGlazeStateXml。生成程序同时把「新实例读回这份存档后前 20 块交给模块的 7 个实际参数」
    按 IEEE 位记下来（下面 expectedBits）。现源码读同一份存档、喂同样的素材，这 7 个参数必须逐块逐位相同——即
    「v0.1.6.2 工程打开后，约束反馈触发之前逐位不变」（isCompatibleAlgorithm；审核 V2-01 的限定口径）。

  ==============================================================================
*/

#include "TestHelpers.h"

#include "../Source/dsp/CeladonChain.h"
#include "../Source/dsp/GlazeProfile.h"

#include <cmath>
#include <cstring>

namespace celadon::test
{
    namespace
    {
    // 与生成夹具的程序（v0.1.6.2 备份副本里的 tests/LegacyFixtureGen.cpp）逐字相同：同一编译器 → 同样的浮点序列。
    juce::AudioBuffer<float> makeFixtureLoop (double fs, double seconds)
    {
        const int n = (int) std::lround (fs * seconds);
        juce::AudioBuffer<float> out (2, n);
        juce::Random random (0x16a2);
        double pinkL = 0.0, pinkR = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / fs;
            const double beat = std::fmod (t, 0.5);                                  // 120 BPM 底鼓
            const double kick = std::exp (-beat * 18.0) * std::sin (2.0 * juce::MathConstants<double>::pi * (48.0 + 60.0 * std::exp (-beat * 40.0)) * beat);
            const double bass = 0.35 * std::sin (2.0 * juce::MathConstants<double>::pi * 55.0 * t) * (0.6 + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.5 * t));
            pinkL = 0.97 * pinkL + 0.03 * (random.nextDouble() * 2.0 - 1.0);
            pinkR = 0.97 * pinkR + 0.03 * (random.nextDouble() * 2.0 - 1.0);
            const double hat = std::fmod (t, 0.125) < 0.02 ? (random.nextDouble() * 2.0 - 1.0) * 0.15 : 0.0;
            const double chord = 0.12 * (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t) + std::sin (2.0 * juce::MathConstants<double>::pi * 277.18 * t)
                                         + std::sin (2.0 * juce::MathConstants<double>::pi * 329.63 * t));
            const double mid = 0.55 * kick + bass + chord + hat;
            out.setSample (0, i, (float) (0.5 * (mid + 1.5 * pinkL)));
            out.setSample (1, i, (float) (0.5 * (mid + 1.5 * pinkR)));
        }
        return out;
    }


        const char* const fixtureXml =
        R"CELADON(<GLAZE_STATE formatVersion="1" hasProfile="1" profileId="1a2a0ad9" lockedWarm="00000000" lockedPunchy="00000000" lockedClean="3f800000"><GLAZE_PROFILE formatVersion="1" integratedLufs_value="c1849661" integratedLufs_valid="1" integratedLufs_seconds="402e000000000000" integratedLufs_confidence="3f800000" loudnessRangeLu_value="3f000000" loudnessRangeLu_valid="1" loudnessRangeLu_seconds="4028333333333333" loudnessRangeLu_confidence="3e4e81b5" plrDb_value="415abd91" plrDb_valid="1" plrDb_seconds="402e000000000000" plrDb_confidence="3f800000" truePeakDbtp_value="c039bcc1" truePeakDbtp_valid="1" truePeakDbtp_seconds="402e000000000000" truePeakDbtp_confidence="3f800000" spectralSlopeDbPerOct_value="c012e171" spectralSlopeDbPerOct_valid="1" spectralSlopeDbPerOct_seconds="402de8ca11bfd44f" spectralSlopeDbPerOct_confidence="3f800000" lowShare_value="3f0a9d87" lowShare_valid="1" lowShare_seconds="402de8ca11bfd44f" lowShare_confidence="3f800000" highShare_value="3c4f3c8a" highShare_valid="1" highShare_seconds="402de8ca11bfd44f" highShare_confidence="3f800000" lowBandRangeDb_value="41300000" lowBandRangeDb_valid="1" lowBandRangeDb_seconds="402de8ca11bfd44f" lowBandRangeDb_confidence="3f800000" onsetDensityPerSecond_value="00000000" onsetDensityPerSecond_valid="1" onsetDensityPerSecond_seconds="402d83126e978d50" onsetDensityPerSecond_confidence="3f800000" onsetStrengthDb_value="00000000" onsetStrengthDb_valid="0" onsetStrengthDb_seconds="402d83126e978d50" onsetStrengthDb_confidence="00000000" bandLongTermDb="c1d7f6ccc1cfadcdc1d24735c1da5c10c20a9106c210f978c212c282c1e7fadbc1d845b3c1cec93ac20b5d2ec217bf01c21a7c6dc21d71f2c220ee9dc2243bfdc2275105c22a7319c22d5ad9c22f4670c2313dc3c2325c8cc232b1dfc2318f11c2300cf2c22d05d6c22a64cc" tameEvidenceDb="00000000000000003e7f80ae3fb29a300000000000000000000000003fd6f75a4054859d40a213e50000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" recoverEvidenceDb="0000000000000000000000000000000040086a5f404ada5d4072f105000000000000000000000000000000003ec60161000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" bandConfidence="3f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f800000" effectiveSeconds="402e000000000000" representativeness="3f800000"/><PLAN requestedTargetLufs="c117f2ae" effectiveTargetLufs="c11b904c" requestedGainDb="40e27427" effectiveGainDb="40db38ec" estimatedExcessDb="40a595c6" balanceRecover="4104feb4" balanceTame="41d7ffff" colorToneDb="00000000" colorGlue="422d9d67" colorHeat="413ff400" limitGainDb="3fa66bbc" limitClip="42723e31" weightWarm="00000000" weightPunchy="00000000" weightClean="3f800000" limitedByConstraints="1" limitedByManual="0" algorithmVersion="6" constantsVersion="6"/><CALIBRATION gainBiasDb="3f5d01d0" valid="1" partial="1" budgetLimited="0" profileId="1a2a0ad9" style="0" glazePercent="42700000" manualParamsHash="97ca7ac3" ceilingDb="bf800000" lookaheadMs="40000000" sampleRate="40e7700000000000"/><GLAZE_CONSTANTS formatVersion="1" version="6" algorithmVersion="6" warm_preferredLufs="c1080000" warm_heatDriveCap="428c0000" warm_glueGrMeanCapDb="40200000" warm_glueGrP95CapDb="40800000" warm_clipDepthCapDb="40400000" warm_clipActivityCap="3c23d70a" warm_limitGrMeanCapDb="3f800000" warm_limitGrP95CapDb="40200000" warm_limitGrPeakCapDb="40800000" warm_recoverCap="420c0000" warm_tameCap="42820000" warm_toneMaxDb="3f400000" warm_shareHeat="3dcccccd" warm_shareGlue="3e99999a" warm_shareClip="3dcccccd" warm_shareLimit="3f19999a" warm_heatPeakReductionCapDb="40000000" warm_toneReferenceSlopeDbPerOct="c0600000" punchy_preferredLufs="c1100000" punchy_heatDriveCap="42480000" punchy_glueGrMeanCapDb="3f800000" punchy_glueGrP95CapDb="40000000" punchy_clipDepthCapDb="40c00000" punchy_clipA)CELADON"
        R"CELADON(ctivityCap="3cf5c28f" punchy_limitGrMeanCapDb="3f4ccccd" punchy_limitGrP95CapDb="40000000" punchy_limitGrPeakCapDb="40400000" punchy_recoverCap="41f00000" punchy_tameCap="425c0000" punchy_toneMaxDb="3f400000" punchy_shareHeat="3d4ccccd" punchy_shareGlue="3dcccccd" punchy_shareClip="3eb33333" punchy_shareLimit="3f000000" punchy_heatPeakReductionCapDb="3fc00000" punchy_toneReferenceSlopeDbPerOct="c0200000" clean_preferredLufs="c1200000" clean_heatDriveCap="41a00000" clean_glueGrMeanCapDb="3fc00000" clean_glueGrP95CapDb="40200000" clean_clipDepthCapDb="3fc00000" clean_clipActivityCap="3b449ba6" clean_limitGrMeanCapDb="3fc00000" clean_limitGrP95CapDb="40600000" clean_limitGrPeakCapDb="40c00000" clean_recoverCap="41a00000" clean_tameCap="42340000" clean_toneMaxDb="3f400000" clean_shareHeat="3c23d70a" clean_shareGlue="3e19999a" clean_shareClip="3d23d70a" clean_shareLimit="3f4ccccd" clean_heatPeakReductionCapDb="3f800000" clean_toneReferenceSlopeDbPerOct="c0200000" lraReferenceLu="40c00000" lraSlopeDbPerLu="3eaaaaab" minPreferredLufs="c1600000" maxPreferredLufs="c0e00000" ceilingForEstimateDb="bf800000" fullConfidence="3f4ccccd" minListenSeconds="402e000000000000" maxListenSeconds="403e000000000000" lraStableSeconds="404e000000000000" calibrationSettleSeconds="4008000000000000" calibrationWindowSeconds="4024000000000000" calibrationMaxDb="40400000" calibrationRounds="2" guardMinDb="c0400000" guardAttackDbPerSecond="3f800000" guardReleaseDbPerSecond="3e99999a" guardTriggerSeconds="3ff0000000000000" guardWindowSeconds="4008000000000000" guardRecoverAfterSeconds="4014000000000000" planTransitionSeconds="4008000000000000" unlearnedInputLufs="c1600000" unlearnedPlrDb="41100000" unlearnedStrength="3eb33333" autoBaseWeight="3e19999a" autoTransientStrengthLowDb="40400000" autoTransientStrengthHighDb="41100000" autoTransientDensityLow="3f000000" autoTransientDensityHigh="40400000" autoDynamicLraLowLu="40a00000" autoDynamicLraHighLu="41400000" autoBrightRangeDbPerOct="3fc00000" autoWarmSustainWeight="3f333333" autoWarmBrightWeight="3e99999a" autoPunchyDynamicDamping="3f000000" glueThresholdAtFullDb="c1c00000" glueRatio="40000000" glueKneeDb="40c00000" glueMakeupFraction="3f000000" glueDetectorOffsetDb="bf000000" glueP95OffsetDb="3fc00000" glueLoudnessPerGrDb="3f866666" gluePeakPerGrDb="3f733333" glueLowRiskShareLow="3eb33333" glueLowRiskShareHigh="3f0ccccd" glueLowRiskRangeLowDb="40c00000" glueLowRiskRangeHighDb="41600000" glueLowRiskMaxCut="3f000000" mismatchThresholdLu="41200000" mismatchReleaseLu="41000000" mismatchSustainSeconds="3ff0000000000000" mismatchTransitionSeconds="3fe0000000000000" mismatchCooldownSeconds="4000000000000000" heatTableTruePeakDbtp="c1900000c1400000c0c000000000000040c00000" heatTableAmount="0000000040a00000412000004170000041a0000041f00000422000004270000042c80000" heatLoudnessDb="00000000b727c5acb727c5acb727c5acb7a7c5acb7fba882b827c5acb851b717b87ba88200000000b7a7c5acb827c5acb87ba882b8bcbe62b912ccf7b95c3372b9c1fc8fba54562e00000000b892ccf7b927c5acb988509cb9c1fc8fba252696ba766a55bae6afcdbb8a47ed00000000b9980b24ba252696ba85b185babf5d79bb247ed0bb7861a6bbe8fb01bc8b6d8700000000ba89a027bb141c82bb6e8d11bbaa10e0bc1057d1bc571f36bcc33722bd586ec1" heatPeakDb="0000000000000000000000008000000080000000b727c5acb7fba882b8a7c5acb9980b2400000000000000003727c5ac3727c5ac3727c5ac80000000b7a7c5acb8bcbe62b9d1b717000000003727c5ac37fba8823827c5ac3851b717387ba88238a7c5ac3851b717b9712c28000000003851b71738e6afcd3932420739712c2839b7803439f66a553a3a1f4b3a8ede5500000000391d4952399d495239ebedfa3a1fe8683a73cb3e3aa2877f3af12c283b339192" clipOffThresholdDb="41400000" clipKneeRatio="3f333333" clipActivityCoeff="3951b717" clipActivityExponent="40333333" limitLossCoeff="3d23d70a" limitLossExponent="40000000" clipLossFloor="3e99999a" limitMeanCoeff="3d0f5c29" limitP95KneeDb="40400000" clipRampMinFraction="3d4ccccd" clipMinimumDepthDb="3e800000" toneDbPerSlopeDb="3f000000" recoverEvidenceFullDb="3fc00000" tameEvidenceFullDb="3f800000" minimumFullDriveDb="404)CELADON"
        R"CELADON(00000" automaticRecoverFloor="41000000" automaticTameFloor="41c00000" automaticHeatFloor="41400000" liveHeatMaxReduction="3f000000" liveGlueMaxReduction="3eb33333" liveColorDensityDeadbandDb="3fc00000" liveColorDensityRangeDb="40800000" liveColorBrightDeadbandDbPerOct="3f19999a" liveColorBrightRangeDbPerOct="3fc00000" liveColorAttackPerSecond="3dcccccd" liveColorReleasePerSecond="3d23d70a"/><GLAZE_CONTROLLER formatVersion="1" feedbackMaxGainDb="4e6e6b28"/><EXECUTION formatVersion="1" valid="1" hasProfile="1" provisional="0" requiresCalibration="0" gainBiasDb="3f5d01d0" protectionDb="00000000" liveToneOffsetDb="00000000" liveTame="41d7ffff" liveHeatReduction="00000000" liveGlueReduction="00000000"><REFERENCE gainBiasDb="00000000" valid="0" partial="0" budgetLimited="0" profileId="1a2a0ad9" style="0" glazePercent="42700000" manualParamsHash="97ca7ac3" ceilingDb="bf800000" lookaheadMs="40000000" sampleRate="40e7700000000000"/><GLAZE_PROFILE formatVersion="1" integratedLufs_value="c1849661" integratedLufs_valid="1" integratedLufs_seconds="402e000000000000" integratedLufs_confidence="3f800000" loudnessRangeLu_value="3f000000" loudnessRangeLu_valid="1" loudnessRangeLu_seconds="4028333333333333" loudnessRangeLu_confidence="3e4e81b5" plrDb_value="415abd91" plrDb_valid="1" plrDb_seconds="402e000000000000" plrDb_confidence="3f800000" truePeakDbtp_value="c039bcc1" truePeakDbtp_valid="1" truePeakDbtp_seconds="402e000000000000" truePeakDbtp_confidence="3f800000" spectralSlopeDbPerOct_value="c012e171" spectralSlopeDbPerOct_valid="1" spectralSlopeDbPerOct_seconds="402de8ca11bfd44f" spectralSlopeDbPerOct_confidence="3f800000" lowShare_value="3f0a9d87" lowShare_valid="1" lowShare_seconds="402de8ca11bfd44f" lowShare_confidence="3f800000" highShare_value="3c4f3c8a" highShare_valid="1" highShare_seconds="402de8ca11bfd44f" highShare_confidence="3f800000" lowBandRangeDb_value="41300000" lowBandRangeDb_valid="1" lowBandRangeDb_seconds="402de8ca11bfd44f" lowBandRangeDb_confidence="3f800000" onsetDensityPerSecond_value="00000000" onsetDensityPerSecond_valid="1" onsetDensityPerSecond_seconds="402d83126e978d50" onsetDensityPerSecond_confidence="3f800000" onsetStrengthDb_value="00000000" onsetStrengthDb_valid="0" onsetStrengthDb_seconds="402d83126e978d50" onsetStrengthDb_confidence="00000000" bandLongTermDb="c1d7f6ccc1cfadcdc1d24735c1da5c10c20a9106c210f978c212c282c1e7fadbc1d845b3c1cec93ac20b5d2ec217bf01c21a7c6dc21d71f2c220ee9dc2243bfdc2275105c22a7319c22d5ad9c22f4670c2313dc3c2325c8cc232b1dfc2318f11c2300cf2c22d05d6c22a64cc" tameEvidenceDb="00000000000000003e7f80ae3fb29a300000000000000000000000003fd6f75a4054859d40a213e50000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" recoverEvidenceDb="0000000000000000000000000000000040086a5f404ada5d4072f105000000000000000000000000000000003ec60161000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" bandConfidence="3f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f8000003f800000" effectiveSeconds="402e000000000000" representativeness="3f800000"/><GLAZE_CONSTANTS formatVersion="1" version="6" algorithmVersion="6" warm_preferredLufs="c1080000" warm_heatDriveCap="428c0000" warm_glueGrMeanCapDb="40200000" warm_glueGrP95CapDb="40800000" warm_clipDepthCapDb="40400000" warm_clipActivityCap="3c23d70a" warm_limitGrMeanCapDb="3f800000" warm_limitGrP95CapDb="40200000" warm_limitGrPeakCapDb="40800000" warm_recoverCap="420c0000" warm_tameCap="42820000" warm_toneMaxDb="3f400000" warm_shareHeat="3dcccccd" warm_shareGlue="3e99999a" warm_shareClip="3dcccccd" warm_shareLimit="3f19999a" warm_heatPeakReductionCapDb="40000000" warm_toneReferenceSlopeDbPerOct="c0600000" punchy_preferredLufs="c1100000" punchy_heatDriveCap="42480000" punchy_glu)CELADON"
        R"CELADON(eGrMeanCapDb="3f800000" punchy_glueGrP95CapDb="40000000" punchy_clipDepthCapDb="40c00000" punchy_clipActivityCap="3cf5c28f" punchy_limitGrMeanCapDb="3f4ccccd" punchy_limitGrP95CapDb="40000000" punchy_limitGrPeakCapDb="40400000" punchy_recoverCap="41f00000" punchy_tameCap="425c0000" punchy_toneMaxDb="3f400000" punchy_shareHeat="3d4ccccd" punchy_shareGlue="3dcccccd" punchy_shareClip="3eb33333" punchy_shareLimit="3f000000" punchy_heatPeakReductionCapDb="3fc00000" punchy_toneReferenceSlopeDbPerOct="c0200000" clean_preferredLufs="c1200000" clean_heatDriveCap="41a00000" clean_glueGrMeanCapDb="3fc00000" clean_glueGrP95CapDb="40200000" clean_clipDepthCapDb="3fc00000" clean_clipActivityCap="3b449ba6" clean_limitGrMeanCapDb="3fc00000" clean_limitGrP95CapDb="40600000" clean_limitGrPeakCapDb="40c00000" clean_recoverCap="41a00000" clean_tameCap="42340000" clean_toneMaxDb="3f400000" clean_shareHeat="3c23d70a" clean_shareGlue="3e19999a" clean_shareClip="3d23d70a" clean_shareLimit="3f4ccccd" clean_heatPeakReductionCapDb="3f800000" clean_toneReferenceSlopeDbPerOct="c0200000" lraReferenceLu="40c00000" lraSlopeDbPerLu="3eaaaaab" minPreferredLufs="c1600000" maxPreferredLufs="c0e00000" ceilingForEstimateDb="bf800000" fullConfidence="3f4ccccd" minListenSeconds="402e000000000000" maxListenSeconds="403e000000000000" lraStableSeconds="404e000000000000" calibrationSettleSeconds="4008000000000000" calibrationWindowSeconds="4024000000000000" calibrationMaxDb="40400000" calibrationRounds="2" guardMinDb="c0400000" guardAttackDbPerSecond="3f800000" guardReleaseDbPerSecond="3e99999a" guardTriggerSeconds="3ff0000000000000" guardWindowSeconds="4008000000000000" guardRecoverAfterSeconds="4014000000000000" planTransitionSeconds="4008000000000000" unlearnedInputLufs="c1600000" unlearnedPlrDb="41100000" unlearnedStrength="3eb33333" autoBaseWeight="3e19999a" autoTransientStrengthLowDb="40400000" autoTransientStrengthHighDb="41100000" autoTransientDensityLow="3f000000" autoTransientDensityHigh="40400000" autoDynamicLraLowLu="40a00000" autoDynamicLraHighLu="41400000" autoBrightRangeDbPerOct="3fc00000" autoWarmSustainWeight="3f333333" autoWarmBrightWeight="3e99999a" autoPunchyDynamicDamping="3f000000" glueThresholdAtFullDb="c1c00000" glueRatio="40000000" glueKneeDb="40c00000" glueMakeupFraction="3f000000" glueDetectorOffsetDb="bf000000" glueP95OffsetDb="3fc00000" glueLoudnessPerGrDb="3f866666" gluePeakPerGrDb="3f733333" glueLowRiskShareLow="3eb33333" glueLowRiskShareHigh="3f0ccccd" glueLowRiskRangeLowDb="40c00000" glueLowRiskRangeHighDb="41600000" glueLowRiskMaxCut="3f000000" mismatchThresholdLu="41200000" mismatchReleaseLu="41000000" mismatchSustainSeconds="3ff0000000000000" mismatchTransitionSeconds="3fe0000000000000" mismatchCooldownSeconds="4000000000000000" heatTableTruePeakDbtp="c1900000c1400000c0c000000000000040c00000" heatTableAmount="0000000040a00000412000004170000041a0000041f00000422000004270000042c80000" heatLoudnessDb="00000000b727c5acb727c5acb727c5acb7a7c5acb7fba882b827c5acb851b717b87ba88200000000b7a7c5acb827c5acb87ba882b8bcbe62b912ccf7b95c3372b9c1fc8fba54562e00000000b892ccf7b927c5acb988509cb9c1fc8fba252696ba766a55bae6afcdbb8a47ed00000000b9980b24ba252696ba85b185babf5d79bb247ed0bb7861a6bbe8fb01bc8b6d8700000000ba89a027bb141c82bb6e8d11bbaa10e0bc1057d1bc571f36bcc33722bd586ec1" heatPeakDb="0000000000000000000000008000000080000000b727c5acb7fba882b8a7c5acb9980b2400000000000000003727c5ac3727c5ac3727c5ac80000000b7a7c5acb8bcbe62b9d1b717000000003727c5ac37fba8823827c5ac3851b717387ba88238a7c5ac3851b717b9712c28000000003851b71738e6afcd3932420739712c2839b7803439f66a553a3a1f4b3a8ede5500000000391d4952399d495239ebedfa3a1fe8683a73cb3e3aa2877f3af12c283b339192" clipOffThresholdDb="41400000" clipKneeRatio="3f333333" clipActivityCoeff="3951b717" clipActivityExponent="40333333" limitLossCoeff="3d23d70a" limitLossExponent="40000000" clipLossFloor="3e99999a" limitMeanCoeff="3d0f5c29" limitP95KneeDb="40400000" clipRampMinFraction="3d4ccccd" clipMinimumDepthDb="3e800000" toneDbPerSlo)CELADON"
        R"CELADON(peDb="3f000000" recoverEvidenceFullDb="3fc00000" tameEvidenceFullDb="3f800000" minimumFullDriveDb="40400000" automaticRecoverFloor="41000000" automaticTameFloor="41c00000" automaticHeatFloor="41400000" liveHeatMaxReduction="3f000000" liveGlueMaxReduction="3eb33333" liveColorDensityDeadbandDb="3fc00000" liveColorDensityRangeDb="40800000" liveColorBrightDeadbandDbPerOct="3f19999a" liveColorBrightRangeDbPerOct="3fc00000" liveColorAttackPerSecond="3dcccccd" liveColorReleasePerSecond="3d23d70a"/><PLAN requestedTargetLufs="c117f2ae" effectiveTargetLufs="c11b904c" requestedGainDb="40e27427" effectiveGainDb="40db38ec" estimatedExcessDb="40a595c6" balanceRecover="4104feb4" balanceTame="41d7ffff" colorToneDb="00000000" colorGlue="422d9d67" colorHeat="413ff400" limitGainDb="3fa66bbc" limitClip="42723e31" weightWarm="00000000" weightPunchy="00000000" weightClean="3f800000" limitedByConstraints="1" limitedByManual="0" algorithmVersion="6" constantsVersion="6"/></EXECUTION></GLAZE_STATE>)CELADON";

        // 生成程序记下的前 20 块实际参数（全部 20 块相同）：Limit Gain、Clip、Recover、Tame、Tone、Glue、Heat 的 IEEE 位。
        constexpr std::uint32_t expectedBits[7] = { 0x40853b29u, 0x42723e31u, 0x41a67f5au, 0x41d7ffffu, 0x00000000u, 0x427d9d67u, 0x413ff400u };

        std::uint32_t bitsOfFloat (float v) noexcept
        {
            std::uint32_t u = 0;
            std::memcpy (&u, &v, sizeof (u));
            return u;
        }

        CeladonChain::Params fixtureParams()
        {
            CeladonChain::Params p;   // claySettings 保持默认（三项「自动」）= 旧工程读进新版后的参数
            p.glazePercent = 60.0f;
            p.glazeStyle = 0;
            p.balance.recover = 12.5f;
            p.color.glue = 20.0f;
            p.limit.gainDb = 2.0f;
            p.limit.ceilingDb = -1.0f;
            return p;
        }
    }

    class LegacyFixtureTests : public juce::UnitTest
    {
    public:
        LegacyFixtureTests() : juce::UnitTest ("LegacyFixture", "Celadon") {}

        void runTest() override
        {
            // expect() 之前必须先有 beginTest()：这个类排第一个跑（--only LegacyFixture）时，JUCE 的结果表还是空的。
            beginTest ("the v0.1.6.2 fixture XML parses");
            const auto xml = juce::parseXML (juce::String::fromUTF8 (fixtureXml));
            expect (xml != nullptr, "fixture XML parses");

            if (xml == nullptr)
                return;

            beginTest ("the v0.1.6.2 fixture reads back with the same profile id (format 1 hash unchanged)");
            {
                glaze::CommittedState state;
                expect (glaze::fromXml (*xml, state), "committed state reads");
                expect (state.hasProfile);
                expectEquals (state.profile.formatVersion, 1);
                expectEquals (state.plan.algorithmVersion, 6);
                expect (state.profileId == 0x1a2a0ad9u, "stored profile id is the one v0.1.6.2 wrote");
                expect (glaze::computeProfileId (state.profile) == state.profileId, "recomputed id equals the stored one");
                expect (state.calibration.valid, "saved with a valid calibration");
            }

            beginTest ("opening the v0.1.6.2 project: the 7 effective params are bit-exact for the first 20 blocks, Clay neutral");
            {
                const double fs = 48000.0;
                const int blockSize = 512;
                const auto loop = makeFixtureLoop (fs, 8.0);
                const auto p = fixtureParams();

                CeladonChain chain;
                chain.setParams (p, false);
                chain.prepare ({ fs, (juce::uint32) blockSize, 2 });
                chain.restoreGlazeStateXml (xml.get());

                glaze::HostInfo host;
                host.hasPlayState = host.isPlaying = host.hasPosition = true;
                juce::AudioBuffer<float> buffer (2, blockSize);
                int mismatchBlocks = 0, clayActiveBlocks = 0;

                for (int b = 0; b < 20; ++b)
                {
                    const auto position = (std::int64_t) b * blockSize;

                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < blockSize; ++i)
                            buffer.setSample (ch, i, loop.getSample (ch, (int) ((position + i) % loop.getNumSamples())));

                    host.positionSamples = position;
                    chain.setHostInfo (host);
                    chain.setParams (p, false);
                    chain.process (buffer);

                    const auto& e = chain.getEffectiveParams();
                    const std::uint32_t got[7] = { bitsOfFloat (e.limit.gainDb), bitsOfFloat (e.limit.clip), bitsOfFloat (e.balance.recover),
                                                   bitsOfFloat (e.balance.tame), bitsOfFloat (e.color.toneDb), bitsOfFloat (e.color.glue),
                                                   bitsOfFloat (e.color.heat) };
                    mismatchBlocks += std::memcmp (got, expectedBits, sizeof (got)) == 0 ? 0 : 1;
                    clayActiveBlocks += (e.clay.lowCutHz != 0.0f || e.clay.monoAmount != 0.0f || e.clay.widthDb != 0.0f) ? 1 : 0;
                }

                expectEquals (mismatchBlocks, 0, "every block hands the modules exactly what v0.1.6.2 did");
                expectEquals (clayActiveBlocks, 0, "a format-1 profile gives neutral Clay");

                const auto status = chain.getGlazeStatus();
                expect (status.calibrationValid, "the saved calibration still applies");
                expect (! status.savedPlanFromOlderAlgorithm, "algorithm 6 is compatible, not an older saved plan");
                expect (status.clayNeedsRelearn, "Clay is automatic and GLAZE > 0: the relearn hint is shown");
            }
        }
    };

    static LegacyFixtureTests legacyFixtureTests;
}
