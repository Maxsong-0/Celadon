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

    T-038（2026-10-01，曲风与交付）：夹具本身仍是通用 / 标准，历史 fromXml 不写 genre / delivery。
    T-048 阶段 2（Q1）：这份 v1 聆听结果在读档时整份丢弃，冷启动，界面提示一次，提示不写入 LIVE。
    下面「打开工程前 20 块逐位相同」是阶段 1 编排器还在采用旧方案时的行为；阶段 2 的读档契约以 phase2::interpretState 为准。

  ==============================================================================
*/

#include "Phase2Contract.h"
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

        // T-038：树里任何一个节点带这个属性（递归）。曲风 / 交付只在参考条件（CALIBRATION、EXECUTION 的 REFERENCE）里存，
        // 但查整棵树：别的节点误写了也算。
        bool hasAttributeAnywhere (const juce::XmlElement& element, const char* attribute)
        {
            if (element.hasAttribute (attribute))
                return true;

            for (const auto* child : element.getChildIterator())
                if (hasAttributeAnywhere (*child, attribute))
                    return true;

            return false;
        }

        struct FixtureRun
        {
            glaze::Status status;
            std::unique_ptr<juce::XmlElement> saved;   // 跑完之后再存出去的 GLAZE 状态（createGlazeStateXml）
        };

        // 新链读回 v0.1.6.2 存档、喂夹具素材 blocks 块（顺序同下面的逐位核对：setParams → prepare → restore；每块 setHostInfo →
        // setParams → process），返回最后的状态与再存出去的 GLAZE 状态。链约 70 KB，放堆上。
        FixtureRun runFixture (const juce::XmlElement& fixture, const CeladonChain::Params& p, int blocks)
        {
            const double fs = 48000.0;
            const int blockSize = 512;
            const auto loop = makeFixtureLoop (fs, 8.0);
            auto chain = std::make_unique<CeladonChain>();
            chain->setParams (p, false);
            chain->prepare ({ fs, (juce::uint32) blockSize, 2 });
            chain->restoreGlazeStateXml (&fixture);

            glaze::HostInfo host;
            host.hasPlayState = host.isPlaying = host.hasPosition = true;
            juce::AudioBuffer<float> buffer (2, blockSize);

            for (int b = 0; b < blocks; ++b)
            {
                const auto position = (std::int64_t) b * blockSize;

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        buffer.setSample (ch, i, loop.getSample (ch, (int) ((position + i) % loop.getNumSamples())));

                host.positionSamples = position;
                chain->setHostInfo (host);
                chain->setParams (p, false);
                chain->process (buffer);
            }

            FixtureRun run;
            run.status = chain->getGlazeStatus();
            run.saved = chain->createGlazeStateXml();
            return run;
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

            beginTest ("phase 2 opens the v0.1.6.2 fixture as a cold start and does not write the session notice into LIVE");
            {
                const auto opened = phase2::interpretState (*xml);
                expect (opened.coldStart, "format 1 has no LIVE tree");
                expect (opened.sessionNotice, "the discarded listen result is announced once");
                expect (opened.planUsesFactoryConstants);
                expect (phase2::isDiscardedListenTag ("GLAZE_PROFILE"));
                expect (phase2::isDiscardedListenTag ("PLAN"));
                expect (phase2::isDiscardedListenTag ("CALIBRATION"));
                expect (phase2::isDiscardedListenTag ("EXECUTION"));
                expect (! phase2::isDiscardedListenTag ("LIVE"));
                expect (! phase2::isDiscardedListenTag ("GLAZE_CONSTANTS"), "constants are ignored, not a listen node");

                const auto saved = phase2::saveLiveTree (glaze::currentAlgorithmVersion);
                expect (saved != nullptr && ! phase2::treeWritesLegacyNotice (*saved));
                expect (! saved->toString().contains (juce::String::fromUTF8 (phase2::legacySessionNoticeZh)));
            }

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

            beginTest ("opening the v0.1.6.2 project cold-starts: the saved plan is not replayed, Clay stays neutral");
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

                expectEquals (mismatchBlocks, 20, "the saved listen plan is dropped; none of the 20 blocks replays v0.1.6.2");
                expectEquals (clayActiveBlocks, 0, "cold start keeps Clay neutral");

                const auto status = chain.getGlazeStatus();
                expect (status.legacySessionDiscarded, "the discarded listen result is announced for this open");
                expect (! status.calibrationValid, "the saved calibration is not applied");
                expect (status.phase == glaze::Phase::unlearned);
                expect (! status.clayNeedsRelearn, "there is no relearn hint");
            }

            // T-038（设计规格-曲风与交付 第 5、6 节；ADR-026）：旧存档没有曲风 / 交付——读成通用 / 标准；再存出去也不写这两个属性
            // （参考条件只在不是默认值时写，旧工程重存逐字不变）。阳性对照：同一份已提交版本把参考条件改成 Deep House / 俱乐部，
            // 存出来就有这两个属性、读回来是同样的值——证明上面的「没有」不是因为属性名查错了。
            beginTest ("the v0.1.6.2 fixture reads as General / Standard and saves without genre / delivery attributes");
            {
                glaze::CommittedState state;
                expect (glaze::fromXml (*xml, state), "committed state reads");
                expect (state.calibration.reference.genre == glaze::Genre::general, "calibration reference genre reads as General");
                expect (state.calibration.reference.delivery == glaze::Delivery::standard, "calibration reference delivery reads as Standard");

                const auto* executionXml = xml->getChildByName (glaze::executionTag);
                glaze::ExecutionState execution;
                expect (executionXml != nullptr && glaze::fromXml (*executionXml, execution), "execution snapshot reads");
                expect (execution.reference.genre == glaze::Genre::general, "execution reference genre reads as General");
                expect (execution.reference.delivery == glaze::Delivery::standard, "execution reference delivery reads as Standard");

                const auto resavedState = glaze::toXml (state);
                const auto resavedExecution = glaze::toXml (execution);
                expect (resavedState != nullptr && ! hasAttributeAnywhere (*resavedState, "genre")
                            && ! hasAttributeAnywhere (*resavedState, "delivery"),
                        "re-saving the committed state writes no genre / delivery attribute");
                expect (resavedExecution != nullptr && ! hasAttributeAnywhere (*resavedExecution, "genre")
                            && ! hasAttributeAnywhere (*resavedExecution, "delivery"),
                        "re-saving the execution snapshot writes no genre / delivery attribute");

                // 阳性对照：非默认的参考条件真的写成 genre / delivery 两个属性（十进制下标），读回来不变。
                auto house = state;
                house.calibration.reference.genre = glaze::Genre::deepHouse;
                house.calibration.reference.delivery = glaze::Delivery::club;
                const auto houseXml = glaze::toXml (house);
                const auto* calibrationXml = houseXml != nullptr ? houseXml->getChildByName ("CALIBRATION") : nullptr;
                expect (calibrationXml != nullptr && calibrationXml->getStringAttribute ("genre") == "1"
                            && calibrationXml->getStringAttribute ("delivery") == "2",
                        "a Deep House / Club reference does write genre=\"1\" delivery=\"2\" (the attribute names checked above are right)");

                glaze::CommittedState houseBack;
                expect (houseXml != nullptr && glaze::fromXml (*houseXml, houseBack)
                            && houseBack.calibration.reference.genre == glaze::Genre::deepHouse
                            && houseBack.calibration.reference.delivery == glaze::Delivery::club,
                        "the Deep House / Club reference reads back unchanged");
            }

            // 整条链：读回旧存档、按通用 / 标准（旧工程读进来的参数）喂 20 块，再存出去的整棵 GLAZE 状态里没有 genre / delivery；
            // 存档里的校准照样适用、不是 House 档。换成 Deep House 或俱乐部（参数变了、存档没变）：参考条件对不上，存档里的校准不再适用
            // （规格第 4.3 节「换交付或曲风 → 方案重算、重新校准」）——同时证明读进来的参考条件确实按通用 / 标准参与了比较。
            beginTest ("opening the v0.1.6.2 project re-saves without genre / delivery and does not keep the old calibration");
            {
                const auto general = runFixture (*xml, fixtureParams(), 20);
                expect (general.saved != nullptr && ! hasAttributeAnywhere (*general.saved, "genre")
                            && ! hasAttributeAnywhere (*general.saved, "delivery"),
                        "the re-saved GLAZE state of the old project has no genre / delivery attribute");
                expect (general.status.legacySessionDiscarded, "General / Standard: the listen result is discarded");
                expect (! general.status.calibrationValid, "General / Standard: the saved calibration is not applied");
                expect (! general.status.houseActive, "General: no House status");

                auto deepParams = fixtureParams();
                deepParams.glazeGenre = (int) glaze::Genre::deepHouse;
                expect (! runFixture (*xml, deepParams, 20).status.calibrationValid,
                        "Deep House: the General reference no longer matches, the saved calibration does not apply");

                auto clubParams = fixtureParams();
                clubParams.glazeDelivery = (int) glaze::Delivery::club;
                expect (! runFixture (*xml, clubParams, 20).status.calibrationValid,
                        "Club: the Standard reference no longer matches, the saved calibration does not apply");
            }
        }
    };

    static LegacyFixtureTests legacyFixtureTests;
}


//==============================================================================
// T-032（2026-09-29，A4-chain）：整链逐位摘要——链路 / 表 / 发行安全改动「逐位不变」的对照工具。
//
// 七个固定场景（六个整链、一个单独的 LoudnessMeter）各跑一段固定素材，把输出样本、每块的读数（两块表、GR、预听增益与状态、
// 交给模块的实际参数、Listen 差值、延迟……）与 OUT 整体响度按 IEEE 位喂进 FNV-1a 64，得到三个摘要（音频 / 读数 / 整体响度），
// 写进日志（grep「digest」）。本段只用 e6b445b（0.1.8.5）就有的公开接口：同一份测试原样放回改动前的源码也能编，同一台机器、
// 同一套编译选项各跑一次，两边日志的摘要逐字相同 = 这些路径逐位不变（做法同 BalanceTests::testGainAndOutputUnchanged）。
// T-032 A4 的改前 / 改后对照（2026-10-01，云端容器：Ubuntu 24.04、GCC 13.3、glibc 2.39，x86-64，-ffp-contract=off，Release）见
// docs/validation/2026-09-29-T032-deep-research/IMPL-A4-chain.md；下面 baselineDigests 是那次的改前值，日志逐项注明是否与它相同——
// 只作提示、不判失败：摘要取决于编译器、libm 与架构（读数摘要里有 float 的 dB 换算，换一版 libm 的 log10f 就会变；音频与整体响度
// 摘要只经 double 运算），别的分支合进来有意改声时它本来就该变。判失败的只有「输出全部有限」与各场景的阳性对照（场景真的走到了
// 它要覆盖的路径）。monitor-toggles 的整体响度摘要按 R8-03 有意改变（听原声期间 OUT 表暂停积分），它的音频与读数摘要仍与改前相同。
// preview-listen-exit 的音频摘要按 T-095 有意改变。
#include <cstddef>
#include <cstdio>
#include <functional>

namespace celadon::test
{
    namespace
    {
        struct Fnv1a64
        {
            std::uint64_t h = 14695981039346656037ull;   // FNV-1a 64 位初值（offset basis）

            void addBytes (const void* data, std::size_t size) noexcept
            {
                const auto* bytes = static_cast<const unsigned char*> (data);

                for (std::size_t i = 0; i < size; ++i)
                {
                    h ^= bytes[i];
                    h *= 1099511628211ull;                   // FNV 64 位素数
                }
            }

            template <typename T>
            void add (const T& value) noexcept   { addBytes (&value, sizeof (value)); }
        };

        juce::String hex64 (std::uint64_t v)
        {
            char text[32] = {};
            std::snprintf (text, sizeof (text), "%016llx", static_cast<unsigned long long> (v));
            return juce::String (text);
        }

        constexpr double digestRate = 48000.0;
        constexpr int digestBlock = 512;

        // t 落在「when 起的那一块」里（块起点按样本换算成秒）：一次性动作只做一次。
        bool atBlock (double t, double when) noexcept   { return t >= when && t < when + (double) digestBlock / digestRate; }

        // 一块的监听状态。每块的顺序照处理器：预听开关（界面线程写的原子量）→ setHostInfo → setParams → process。
        struct MonitorState
        {
            bool hostBypassed = false, compareOriginal = false, auditionDifference = false, offline = false;
            int preview = -1;   // −1 = 不动；0 / 1 = 在这一块开头关 / 开归一化预听
        };

        struct DigestResult
        {
            std::uint64_t audio = 0, readings = 0, integrated = 0;
            bool finite = true;
            int blocks = 0;
            float minPreviewGainDb = 0.0f;    // 阳性对照：预听真的衰减过
            float lastIntegratedLufs = LoudnessMeter::noReading;
            int integratedChanges = 0;        // 阳性对照（meter-sweep）：整体响度读数真的来回变过
        };

        using Schedule = std::function<MonitorState (double)>;
        using Hook = std::function<void (CeladonChain&, double)>;

        // 跑一个整链场景：素材是 LegacyFixture 的 8 s 循环（48 kHz / 512）；schedule 按块起点（秒）给监听状态，hook 在块前做一次性动作。
        DigestResult runChainDigest (CeladonChain& chain, const CeladonChain::Params& params, double seconds,
                                     const Schedule& schedule, const Hook& hook)
        {
            const auto loop = makeFixtureLoop (digestRate, 8.0);
            const auto total = (int) std::lround (digestRate * seconds);

            glaze::HostInfo host;
            host.hasPlayState = host.isPlaying = host.hasPosition = true;
            juce::AudioBuffer<float> buffer (2, digestBlock);
            Fnv1a64 audio, readings, integrated;
            DigestResult result;
            auto previousIntegrated = LoudnessMeter::noReading;

            for (int position = 0; position < total; position += digestBlock)
            {
                const auto length = juce::jmin (digestBlock, total - position);
                buffer.setSize (2, length, false, false, true);

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < length; ++i)
                        buffer.setSample (ch, i, loop.getSample (ch, (int) (((std::int64_t) position + i) % loop.getNumSamples())));

                const auto t = (double) position / digestRate;

                if (hook)
                    hook (chain, t);

                const auto monitor = schedule ? schedule (t) : MonitorState {};

                if (monitor.preview >= 0)
                    chain.setReleasePreviewEnabled (monitor.preview > 0);

                host.offline = monitor.offline;
                host.positionSamples = position;
                chain.setHostInfo (host);
                chain.setParams (params, monitor.hostBypassed, monitor.compareOriginal, monitor.auditionDifference);
                chain.process (buffer);
                ++result.blocks;

                for (int ch = 0; ch < 2; ++ch)
                {
                    audio.addBytes (buffer.getReadPointer (ch), (std::size_t) length * sizeof (float));

                    for (int i = 0; i < length; ++i)
                        result.finite = result.finite && std::isfinite (buffer.getSample (ch, i));
                }

                const auto& e = chain.getEffectiveParams();
                const auto status = chain.getMasteringStatus();
                readings.add (chain.getOutputShortTermLufs());
                readings.add (chain.getAndResetOutputTruePeakDb());
                readings.add (chain.getAndResetInputTruePeakDb());
                readings.add (chain.getInputMeter().getShortTermLufs());
                readings.add (chain.getInputMeter().getIntegratedLufs());
                readings.add (chain.getAndResetLimitGainReductionDb());
                readings.add (chain.getAndResetGlueGainReductionDb());
                readings.add (status.previewGainDb);
                readings.add (status.harshReductionDb);
                readings.add (status.codecMarginDb);
                readings.add ((int) status.dither);
                readings.add (status.previewEnabled);
                readings.addBytes (status.targetCorrectionDb.data(), sizeof (status.targetCorrectionDb));
                readings.add (chain.hasListenDifference() ? chain.getListenDifferenceDb() : -1000.0f);
                readings.add (e.limit.gainDb);        readings.add (e.limit.clip);            readings.add (e.limit.ceilingDb);
                readings.add (e.balance.recover);     readings.add (e.balance.tame);
                readings.add (e.balance.automaticRecover);                                    readings.add (e.balance.automaticTame);
                readings.add (e.color.toneDb);        readings.add (e.color.glue);            readings.add (e.color.heat);
                readings.add (e.balance.harshAmount); readings.add (e.balance.transientPreserve);
                readings.add (e.balance.targetAmount);                                        readings.add (e.balance.targetSlopeDbPerOct);
                readings.add (e.clay.lowCutHz);       readings.add (e.clay.monoAmount);       readings.add (e.clay.monoHz);
                readings.add (e.clay.widthDb);
                readings.add (chain.getLatencySamples());

                const auto integratedLufs = chain.getOutputIntegratedLufs();
                integrated.add (integratedLufs);
                result.integratedChanges += integratedLufs != previousIntegrated ? 1 : 0;
                previousIntegrated = integratedLufs;
                result.minPreviewGainDb = juce::jmin (result.minPreviewGainDb, status.previewGainDb);
            }

            result.audio = audio.h;
            result.readings = readings.h;
            result.integrated = integrated.h;
            result.lastIntegratedLufs = previousIntegrated;
            return result;
        }

        // 单独一块 LoudnessMeter（R7-04 两遍扫描的上下界）：电平每 0.7 s 跳一次（−60 ～ 0 dBFS 的固定伪随机序列，约 12 % 的段是静音、
        // 落在绝对门以下），块长轮换 37 / 512 / 1000 / 64，30 s 处点一次 Reset：相对门在直方图里来回移动、跨过许多格。
        DigestResult runMeterSweepDigest()
        {
            LoudnessMeter meter;
            meter.prepare ({ digestRate, 1024, 2 });

            juce::Random random (0x7032);
            const auto total = (int) (60.0 * digestRate);
            const auto segment = (int) (0.7 * digestRate);
            constexpr int sizes[] = { 37, 512, 1000, 64 };
            juce::AudioBuffer<float> buffer (2, 1024);
            Fnv1a64 readings, integrated;
            DigestResult result;
            auto previousIntegrated = LoudnessMeter::noReading;
            auto gain = 0.0f;
            auto phase = 0.0;
            auto resetDone = false;
            const auto phaseStep = juce::MathConstants<double>::twoPi * 997.0 / digestRate;

            for (int position = 0, blockIndex = 0; position < total; ++blockIndex)
            {
                const auto length = juce::jmin (sizes[blockIndex % 4], total - position);
                buffer.setSize (2, length, false, false, true);

                for (int i = 0; i < length; ++i)
                {
                    if ((position + i) % segment == 0)
                        gain = random.nextFloat() < 0.12f ? 0.0f
                                                          : juce::Decibels::decibelsToGain (-60.0f + 60.0f * random.nextFloat());

                    phase += phaseStep;

                    if (phase > juce::MathConstants<double>::twoPi)
                        phase -= juce::MathConstants<double>::twoPi;

                    const auto tone = (float) std::sin (phase);
                    const auto noise = random.nextFloat() * 2.0f - 1.0f;
                    buffer.setSample (0, i, gain * (0.7f * tone + 0.3f * noise));
                    buffer.setSample (1, i, gain * (0.7f * tone - 0.3f * noise));
                }

                if (! resetDone && position >= (int) (30.0 * digestRate))
                {
                    meter.requestReset();
                    resetDone = true;
                }

                meter.process (buffer);
                position += length;
                ++result.blocks;

                readings.add (meter.getShortTermLufs());
                readings.add (meter.getAndResetTruePeakDb());
                const auto integratedLufs = meter.getIntegratedLufs();
                integrated.add (integratedLufs);
                result.integratedChanges += integratedLufs != previousIntegrated ? 1 : 0;
                previousIntegrated = integratedLufs;
            }

            result.readings = readings.h;
            result.integrated = integrated.h;
            result.lastIntegratedLufs = previousIntegrated;
            return result;
        }

        // T-032 A4 改动前（e6b445b 源码 + 本段测试）的摘要：{ 场景, 音频, 读数, 整体响度 }（2026-10-01 云端容器 GCC 13.3 / glibc 2.39 实测；
        // meter-sweep 没有音频）。前任注明「107 上实测」的那一版：音频与整体响度摘要与此逐字相同，读数摘要七个都不同（多半是 libm，见上）。
        struct BaselineDigest { const char* name; std::uint64_t audio, readings, integrated; };

        constexpr BaselineDigest baselineDigests[] =
        {
            { "manual-busy",         0x14c23c589c9a10c9ull, 0xd87075eda3fe55f0ull, 0xd0aa3ecfe8ce7485ull },
            { "glaze-auto-defaults", 0x4bf945a6d7dc2323ull, 0x974827b3b1dbb965ull, 0xd1326040cb49cef7ull },
            { "preview-steady",      0x43a5f132baa849c7ull, 0xbf763e26c0e96dcfull, 0xa0732c36fe289a14ull },
            { "preview-listen-exit", 0xb2d1cc0dc4a2a456ull, 0x57ecae1cb8d0befbull, 0x664dc2db328108e0ull },
            { "monitor-toggles",     0x9ca2cb0c4afbe443ull, 0x9b79feb9688b700bull, 0x327282438585aaf8ull },
            { "reset-paths",         0x05977d4ff3ca916aull, 0x5ed9b4410750dd20ull, 0xf0abe14883a5c495ull },
            { "meter-sweep",         0x0000000000000000ull, 0x6521e096b6bf16aeull, 0xd5066b8a43585ee8ull },
        };
    }

    class ChainDigestTests : public juce::UnitTest
    {
    public:
        ChainDigestTests() : juce::UnitTest ("ChainDigest", "Celadon") {}

        void runTest() override
        {
            beginTest ("whole-chain bit digests (audio / per-block readings / OUT integrated), logged for before-after comparison");

            for (const auto& baseline : baselineDigests)
            {
                const auto scenario = juce::String (baseline.name);
                const auto result = runScenario (scenario);
                const auto compare = [&baseline] (std::uint64_t got, std::uint64_t base) -> juce::String
                {
                    if (baseline.audio == 0 && baseline.readings == 0 && baseline.integrated == 0)
                        return {};

                    return got == base ? " (= baseline)" : " (differs from baseline " + hex64 (base) + ")";
                };

                logMessage ("  digest " + scenario + ": audio " + hex64 (result.audio) + compare (result.audio, baseline.audio)
                            + ", readings " + hex64 (result.readings) + compare (result.readings, baseline.readings)
                            + ", integrated " + hex64 (result.integrated) + compare (result.integrated, baseline.integrated)
                            + " | " + juce::String (result.blocks) + " blocks, last integrated "
                            + juce::String (result.lastIntegratedLufs, 3) + " LUFS, min preview gain "
                            + juce::String (result.minPreviewGainDb, 3) + " dB, integrated changes " + juce::String (result.integratedChanges));

                expect (result.finite, scenario + ": every output sample is finite");
                expect (result.blocks > 0, scenario + ": the scenario ran");

                // 阳性对照：场景真的走到了要覆盖的路径。
                if (scenario.startsWith ("preview"))
                    expect (result.minPreviewGainDb < -1.0f, scenario + ": the normalisation preview really attenuated");

                if (scenario == "meter-sweep")
                    expect (result.integratedChanges > 50, scenario + ": the integrated reading moved around ("
                                                               + juce::String (result.integratedChanges) + " changes)");
                else
                    expect (result.lastIntegratedLufs > -70.0f, scenario + ": the OUT meter has an integrated reading");
            }
        }

    private:
        static void prepare (CeladonChain& chain, const CeladonChain::Params& p)
        {
            chain.setParams (p, false);
            chain.prepare ({ digestRate, (juce::uint32) digestBlock, 2 });
            chain.setParams (p, false);
        }

        static DigestResult runScenario (const juce::String& scenario)
        {
            if (scenario == "meter-sweep")
                return runMeterSweepDigest();

            // 链约 70 KB，放堆上（测试栈帧不宜再大）。
            auto chain = std::make_unique<CeladonChain>();
            CeladonChain::Params p;

            if (scenario == "manual-busy")
            {
                // GLAZE 0，四个模块都在做事，Listen 开，手动的推响不刺 / 目标频谱、编码余量、16 bit 抖动：正常路径全走一遍。
                p.balance.recover = 60.0f;  p.balance.tame = 60.0f;  p.balance.brighten = 20.0f;
                p.balance.speed = BalanceModule::Speed::fast;
                p.claySettings.lowCutMode = p.claySettings.monoMode = p.claySettings.widthMode = ClayMode::manual;
                p.claySettings.lowCutHz = 30.0f;  p.claySettings.monoHz = 150.0f;  p.claySettings.widthDb = 2.0f;
                p.color.toneDb = 1.5f;  p.color.glue = 40.0f;  p.color.heat = 40.0f;  p.color.mixPercent = 80.0f;  p.color.outputDb = 1.0f;
                p.limit.gainDb = 9.0f;  p.limit.ceilingDb = -1.5f;  p.limit.clip = 30.0f;  p.limit.lookaheadMs = 3.0f;  p.limit.listen = true;
                p.mastering.harshMode = HarshMode::manual;  p.mastering.harshAmount = 60.0f;  p.mastering.transientPreserve = 40.0f;
                p.mastering.targetMode = TargetMode::warm;  p.mastering.targetAmount = 50.0f;
                p.mastering.codecMarginDb = 0.5f;  p.mastering.dither = DitherMode::bits16;
                prepare (*chain, p);
                return runChainDigest (*chain, p, 4.0, {}, {});
            }

            if (scenario == "glaze-auto-defaults")
            {
                // 新实例默认：GLAZE 35、推响不刺自动 50、目标自动 35（聆听 → 方案过渡都在 10 s 里走），编码余量 0.3 dB。
                p.glazePercent = 35.0f;
                p.mastering.harshMode = HarshMode::automatic;   p.mastering.harshAmount = 50.0f;  p.mastering.transientPreserve = 50.0f;
                p.mastering.targetMode = TargetMode::automatic; p.mastering.targetAmount = 35.0f;
                p.mastering.codecMarginDb = 0.3f;
                prepare (*chain, p);
                return runChainDigest (*chain, p, 10.0, {}, {});
            }

            if (scenario == "preview-steady")
            {
                // 稳态归一化预听（从第一块起就开着，目标 −20 LUFS、母带 +6 dB）：进入交接与稳态衰减。
                p.color.outputDb = 6.0f;
                p.mastering.previewTargetLufs = -20.0f;
                prepare (*chain, p);
                chain->setReleasePreviewEnabled (true);
                return runChainDigest (*chain, p, 4.0, {}, {});
            }

            if (scenario == "preview-listen-exit")
            {
                // Listen 开着时退出预听（1.5 s，交接给 Listen）、再开（2.5 s）、离线把它关掉（3.2 ～ 3.6 s 离线）；24 bit 抖动。
                // 这几条退出路径改动前后走同一段代码（Listen 开 → 交接给 Listen 再 reset；离线 → 立即 reset）。
                p.color.outputDb = 6.0f;
                p.limit.listen = true;
                p.mastering.previewTargetLufs = -20.0f;
                p.mastering.dither = DitherMode::bits24;
                prepare (*chain, p);
                return runChainDigest (*chain, p, 4.5, [] (double t)
                {
                    MonitorState m;
                    m.preview = atBlock (t, 0.0) ? 1 : (atBlock (t, 1.5) ? 0 : (atBlock (t, 2.5) ? 1 : -1));
                    m.offline = t >= 3.2 && t < 3.6;
                    return m;
                }, {});
            }

            if (scenario == "monitor-toggles")
            {
                // 没开过预听：宿主旁通 / 听原声 / 试听差值来回切，Listen 开、24 bit 抖动（干路与差值上不该加抖动）。
                p.limit.gainDb = 6.0f;  p.limit.listen = true;
                p.mastering.dither = DitherMode::bits24;
                prepare (*chain, p);
                return runChainDigest (*chain, p, 4.0, [] (double t)
                {
                    MonitorState m;
                    m.hostBypassed = t >= 1.0 && t < 1.5;
                    m.compareOriginal = t >= 2.0 && t < 2.5;
                    m.auditionDifference = t >= 3.0 && t < 3.5;
                    return m;
                }, {});
            }

            if (scenario == "reset-paths")
            {
                // 播放中途 restartWithFadeIn（2 s）、setParams + reset（3 s，同规格 prepareToPlay 的路径）、界面 Reset 清表（3.5 s）。
                p.glazePercent = 20.0f;
                p.limit.gainDb = 4.0f;
                p.mastering.dither = DitherMode::bits24;
                prepare (*chain, p);
                return runChainDigest (*chain, p, 4.5, {}, [p] (CeladonChain& c, double t)
                {
                    if (atBlock (t, 2.0)) c.restartWithFadeIn();
                    if (atBlock (t, 3.0)) { c.setParams (p, false); c.reset(); }
                    if (atBlock (t, 3.5)) c.requestMeterReset();
                });
            }

            jassertfalse;
            return {};
        }
    };

    static ChainDigestTests chainDigestTests;
}
