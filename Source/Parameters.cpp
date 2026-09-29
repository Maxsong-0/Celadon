/*
  ==============================================================================

    Parameters.cpp
    参数表：默认值、显示文字，范围取自 ParameterRanges.h（与无头测试共用）。范围在这里钳制，DSP 模块内不再信任外部值。

  ==============================================================================
*/

#include "Parameters.h"
#include "ParameterRanges.h"

namespace celadon::param
{
    namespace
    {
        juce::ParameterID makeId (const char* id, int version)
        {
            return { id, version };
        }

        // 显示为「+3.0 dB」这类带符号的一位小数。
        juce::AudioParameterFloatAttributes decibelAttributes()
        {
            return juce::AudioParameterFloatAttributes()
                .withLabel ("dB")
                .withStringFromValueFunction ([] (float value, int)
                                              {
                                                  // 先按显示精度（0.1 dB）取整并抹掉负零：参数吸附到步进时 clang 会把
                                                  // start + interval × n 合成 FMA，0 dB 实际存成 −6.7e-8，直接格式化会显示「−0.0」
                                                  // （2026-09-24 夜间评审 R2-2）。
                                                  const auto shown = std::round (value * 10.0f) / 10.0f;
                                                  const auto safe  = shown == 0.0f ? 0.0f : shown;
                                                  return (safe > 0.0f ? "+" : "") + juce::String (safe, 1);
                                              });
        }

        // 整数显示（0 ～ 100 这类量旋钮）。
        juce::AudioParameterFloatAttributes integerAttributes (const juce::String& label = {})
        {
            return juce::AudioParameterFloatAttributes()
                .withLabel (label)
                .withStringFromValueFunction ([] (float value, int)
                                              {
                                                  return juce::String (juce::roundToInt (value));
                                              });
        }

        std::unique_ptr<juce::RangedAudioParameter> makeFloat (const char* id,
                                                               const juce::String& name,
                                                               juce::NormalisableRange<float> range,
                                                               float defaultValue,
                                                               juce::AudioParameterFloatAttributes attributes,
                                                               int version = kParamVersion)
        {
            return std::make_unique<juce::AudioParameterFloat> (makeId (id, version), name, range, defaultValue, attributes);
        }

        std::unique_ptr<juce::RangedAudioParameter> makeBool (const char* id,
                                                              const juce::String& name,
                                                              bool defaultValue,
                                                              int version = kParamVersion)
        {
            return std::make_unique<juce::AudioParameterBool> (makeId (id, version), name, defaultValue);
        }

        // Clay 的三态（ADR-009）：选项顺序 = celadon::ClayMode 的下标（0 自动 / 1 关 / 2 手动），默认 0 = 自动。
        // 选项名用英文（宿主自动化列表里显示，与 Balance Speed、Glaze Style 一致）；抽屉里的三态胶囊显示中文「自动 / 关 / 手动」
        //（胶囊按下标绑定，只核对选项个数）。处理器 Debug 版构造时核对选项数与 ClayMode 一致。
        std::unique_ptr<juce::RangedAudioParameter> makeClayMode (const char* id, const juce::String& name)
        {
            return std::make_unique<juce::AudioParameterChoice> (makeId (id, kParamVersionClay), name,
                                                                 juce::StringArray { "Auto", "Off", "Manual" }, 0);
        }
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // ---- Limit（设计规格-Limit「参数」表）----
        layout.add (makeFloat (limitGain, "Limit Gain",
                               ranges::limitGain(), 0.0f, decibelAttributes()));

        layout.add (makeFloat (limitCeiling, "Limit Ceiling",
                               ranges::limitCeiling(), -1.0f,
                               decibelAttributes().withLabel ("dBTP")));

        layout.add (makeFloat (limitRelease, "Limit Release",
                               ranges::limitRelease(), 100.0f,
                               juce::AudioParameterFloatAttributes()
                                   .withLabel ("ms")
                                   .withStringFromValueFunction ([] (float value, int)
                                                                 {
                                                                     return juce::String (juce::roundToInt (value));
                                                                 })));

        layout.add (makeBool (limitReleaseAuto, "Limit Auto Release", true));

        // 步进 0.5 ms（ParameterRanges.h）：每换一档都要重置限幅器并重新上报延迟。
        layout.add (makeFloat (limitLookahead, "Limit Lookahead",
                               ranges::limitLookahead(), 2.0f,
                               juce::AudioParameterFloatAttributes()
                                   .withLabel ("ms")
                                   .withStringFromValueFunction ([] (float value, int)
                                                                 {
                                                                     return juce::String (value, 1);
                                                                 })));

        layout.add (makeFloat (limitClip, "Limit Clip",
                               ranges::limitClip(), 0.0f, integerAttributes()));

        layout.add (makeBool (limitListen, "Limit Listen", false));
        layout.add (makeBool (limitBypass, "Limit Bypass", false));

        // ---- Color（设计规格-Color「参数」表；v0.2 引入，版本号 2）----
        layout.add (makeFloat (colorTone, "Color Tone",
                               ranges::colorTone(), 0.0f, decibelAttributes(), kParamVersionColor));

        layout.add (makeFloat (colorGlue, "Color Glue",
                               ranges::colorGlue(), 0.0f, integerAttributes(), kParamVersionColor));

        layout.add (makeFloat (colorHeat, "Color Heat",
                               ranges::colorHeat(), 0.0f, integerAttributes(), kParamVersionColor));

        layout.add (makeFloat (colorMix, "Color Mix",
                               ranges::colorMix(), 100.0f, integerAttributes ("%"), kParamVersionColor));

        layout.add (makeFloat (colorOutput, "Color Output",
                               ranges::colorOutput(), 0.0f, decibelAttributes(), kParamVersionColor));

        layout.add (makeBool (colorBypass, "Color Bypass", false, kParamVersionColor));

        // ---- Balance（设计规格-Balance「参数」表；v0.3 引入，版本号 3）----
        layout.add (makeFloat (balanceRecover, "Balance Recover",
                               ranges::balanceRecover(), 0.0f, integerAttributes(), kParamVersionBalance));

        layout.add (makeFloat (balanceTame, "Balance Tame",
                               ranges::balanceTame(), 0.0f, integerAttributes(), kParamVersionBalance));

        // −100 ～ +100：带符号整数显示（「+20」「-20」），0 显示「0」。
        layout.add (makeFloat (balanceBrighten, "Balance Brighten",
                               ranges::balanceBrighten(), 0.0f,
                               juce::AudioParameterFloatAttributes()
                                   .withStringFromValueFunction ([] (float value, int)
                                                                 {
                                                                     const auto rounded = juce::roundToInt (value);
                                                                     return (rounded > 0 ? "+" : "") + juce::String (rounded);
                                                                 }),
                               kParamVersionBalance));

        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (balanceSpeed, kParamVersionBalance),
                                                                  "Balance Speed",
                                                                  juce::StringArray { "Slow", "Mid", "Fast" }, 1));

        layout.add (makeBool (balanceBypass, "Balance Bypass", false, kParamVersionBalance));

        // 新实例从温和的自动处理开始；0 仍是严格手动。旧工程缺参的回退值在处理器中独立保留为 0。
        layout.add (makeFloat (glazeAmount, "Glaze",
                               ranges::glazeAmount(), 35.0f, integerAttributes ("%"), kParamVersionGlaze));

        // ---- GLAZE 风格（ADR-008；v0.6 引入，版本号 6）：自动 / 温润 / 冲击 / 通透，默认自动 ----
        // 选项名用英文（宿主自动化列表里显示，与 Balance Speed 一致）；界面胶囊显示中文。顺序 = glaze::Style 的下标。
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (glazeStyle, kParamVersionGlazeStyle),
                                                                  "Glaze Style",
                                                                  juce::StringArray { "Auto", "Warm", "Punchy", "Clean" }, 0));

        // ---- Clay 修坯（设计规格-Clay 第 6 节、ADR-009；v0.1.7.0 引入，ParameterID 版本 kParamVersionClay = 7）----
        // 全部在「高级」抽屉里、都可自动化，追加在参数表末尾（已发布参数的下标不动）。顺序与 ParameterIds.h 的 allParameterIds 一致。
        // 每项「三态 + 手动值」：三态默认「自动」——GLAZE 为 0 时自动值为 0（不处理）；「手动」严格按旋钮值，GLAZE 0 也生效；
        // 「关」永远不处理。三态合成在 GLAZE 编排器里（glaze::resolveClayParams），这里只定范围、默认值与显示。
        // 旧工程缺这些参数时回到默认（addMissingParameterNodes 补只带 id 的节点）：三态「自动」而旧画像没有立体声 / 超低频特征，
        // 决策器给的自动值为 0，打开旧工程声音不变（设计规格-Clay 第 7 节）。
        // 手动值的范围必须与 ClayModule 的钳位常数一致（minLowCutHz … maxWidthDb），处理器 Debug 版构造时逐项核对。
        layout.add (makeClayMode (clayLowCutMode, "Clay Low Cut Mode"));

        // 低切（L/R 4 阶 Butterworth 高通）：10 ～ 60 Hz 对数刻度，默认 25 Hz；整数显示「25 Hz」。
        layout.add (makeFloat (clayLowCutHz, "Clay Low Cut",
                               ranges::clayLowCut(), 25.0f, integerAttributes ("Hz"), kParamVersionClay));

        layout.add (makeClayMode (clayMonoMode, "Clay Mono Mode"));

        // 低频收单（只对 S 的 2 阶高通）的分频点：40 ～ 300 Hz 对数刻度，默认 120 Hz；整数显示「120 Hz」。
        layout.add (makeFloat (clayMonoHz, "Clay Mono",
                               ranges::clayMono(), 120.0f, integerAttributes ("Hz"), kParamVersionClay));

        layout.add (makeClayMode (clayWidthMode, "Clay Width Mode"));

        // 加宽（只对 S 的 2 kHz 高架）：−3 ～ +3 dB 线性，默认 0；步长与显示同 Color Tone（0.01 dB 步进、「+0.8 dB」一位小数）。
        // 负值 = 收窄，只能手动（自动只给 0 ～ +1.5 dB）。
        layout.add (makeFloat (clayWidthDb, "Clay Width",
                               ranges::clayWidth(), 0.0f, decibelAttributes(), kParamVersionClay));

        layout.add (makeBool (clayBypass, "Clay Bypass", false, kParamVersionClay));

        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (masterHarshMode, kParamVersionMastering),
                    "Harshness Mode", juce::StringArray { "Auto", "Off", "Manual" }, 0));
        layout.add (makeFloat (masterHarshAmount, "Harshness Protection", { 0.0f, 100.0f, 0.1f }, 50.0f, integerAttributes ("%"), kParamVersionMastering));
        layout.add (makeFloat (masterTransient, "Transient Preserve", { 0.0f, 100.0f, 0.1f }, 50.0f, integerAttributes ("%"), kParamVersionMastering));
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (masterTargetMode, kParamVersionMastering),
                    "Spectrum Target", juce::StringArray { "Off", "Auto", "Warm", "Neutral", "Bright", "Reference" }, 1));
        layout.add (makeFloat (masterTargetAmount, "Spectrum Target Amount", { 0.0f, 100.0f, 0.1f }, 35.0f, integerAttributes ("%"), kParamVersionMastering));
        layout.add (makeFloat (masterCodecMarginDb, "Codec Headroom Reserve", { 0.0f, 1.5f, 0.1f }, 0.0f, decibelAttributes(), kParamVersionMastering));
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (masterDither, kParamVersionMastering),
                    "Export Dither", juce::StringArray { "Off", "16 bit", "24 bit" }, 0));
        layout.add (makeFloat (masterPreviewLufs, "Normalisation Preview Target", { -24.0f, -8.0f, 0.1f }, -14.0f,
                    decibelAttributes().withLabel ("LUFS"), kParamVersionMastering));
        return layout;
    }
}
