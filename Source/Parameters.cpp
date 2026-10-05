/*
  ==============================================================================

    Parameters.cpp
    参数表：显示文字；范围取自 ParameterRanges.h、默认值取自 ParameterDefaults.h（都与无头测试、离线工具共用；T-032 R10-01 起
    默认值一处真源，处理器 Debug 版构造时逐项核对参数表与真源一致）。范围在这里钳制，DSP 模块内不再信任外部值。

  ==============================================================================
*/

#include "Parameters.h"
#include "ParameterDefaults.h"
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
        // 选项名用英文（宿主自动化列表里显示，与 Balance Speed、Glaze Style 一致）；CLAY 卡片上的三态胶囊显示中文「自动 / 关 / 手动」
        //（胶囊按下标绑定，只核对选项个数）。处理器 Debug 版构造时核对选项数与 ClayMode 一致。
        std::unique_ptr<juce::RangedAudioParameter> makeClayMode (const char* id, const juce::String& name, int defaultIndex)
        {
            return std::make_unique<juce::AudioParameterChoice> (makeId (id, kParamVersionClay), name,
                                                                 juce::StringArray { "Auto", "Off", "Manual" }, defaultIndex);
        }
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // ---- Limit（设计规格-Limit「参数」表）----
        layout.add (makeFloat (limitGain, "Limit Gain",
                               ranges::limitGain(), defaults::limitGainDb, decibelAttributes()));

        layout.add (makeFloat (limitCeiling, "Limit Ceiling",
                               ranges::limitCeiling(), defaults::limitCeilingDb,
                               decibelAttributes().withLabel ("dBTP")));

        layout.add (makeFloat (limitRelease, "Limit Release",
                               ranges::limitRelease(), defaults::limitReleaseMs,
                               juce::AudioParameterFloatAttributes()
                                   .withLabel ("ms")
                                   .withStringFromValueFunction ([] (float value, int)
                                                                 {
                                                                     return juce::String (juce::roundToInt (value));
                                                                 })));

        layout.add (makeBool (limitReleaseAuto, "Limit Auto Release", defaults::limitReleaseAuto));

        // 步进 0.5 ms（ParameterRanges.h）：每换一档都要重置限幅器并重新上报延迟。
        layout.add (makeFloat (limitLookahead, "Limit Lookahead",
                               ranges::limitLookahead(), defaults::limitLookaheadMs,
                               juce::AudioParameterFloatAttributes()
                                   .withLabel ("ms")
                                   .withStringFromValueFunction ([] (float value, int)
                                                                 {
                                                                     return juce::String (value, 1);
                                                                 })));

        layout.add (makeFloat (limitClip, "Limit Clip",
                               ranges::limitClip(), defaults::limitClip, integerAttributes()));

        layout.add (makeBool (limitListen, "Limit Listen", defaults::limitListen));
        layout.add (makeBool (limitBypass, "Limit Bypass", defaults::limitBypass));

        // ---- Color（设计规格-Color「参数」表；v0.2 引入，版本号 2）----
        layout.add (makeFloat (colorTone, "Color Tone",
                               ranges::colorTone(), defaults::colorToneDb, decibelAttributes(), kParamVersionColor));

        layout.add (makeFloat (colorGlue, "Color Glue",
                               ranges::colorGlue(), defaults::colorGlue, integerAttributes(), kParamVersionColor));

        layout.add (makeFloat (colorHeat, "Color Heat",
                               ranges::colorHeat(), defaults::colorHeat, integerAttributes(), kParamVersionColor));

        layout.add (makeFloat (colorMix, "Color Mix",
                               ranges::colorMix(), defaults::colorMix, integerAttributes ("%"), kParamVersionColor));

        layout.add (makeFloat (colorOutput, "Color Output",
                               ranges::colorOutput(), defaults::colorOutputDb, decibelAttributes(), kParamVersionColor));

        layout.add (makeBool (colorBypass, "Color Bypass", defaults::colorBypass, kParamVersionColor));

        // ---- Balance（设计规格-Balance「参数」表；v0.3 引入，版本号 3）----
        layout.add (makeFloat (balanceRecover, "Balance Recover",
                               ranges::balanceRecover(), defaults::balanceRecover, integerAttributes(), kParamVersionBalance));

        layout.add (makeFloat (balanceTame, "Balance Tame",
                               ranges::balanceTame(), defaults::balanceTame, integerAttributes(), kParamVersionBalance));

        // −100 ～ +100：带符号整数显示（「+20」「-20」），0 显示「0」。
        layout.add (makeFloat (balanceBrighten, "Balance Brighten",
                               ranges::balanceBrighten(), defaults::balanceBrighten,
                               juce::AudioParameterFloatAttributes()
                                   .withStringFromValueFunction ([] (float value, int)
                                                                 {
                                                                     const auto rounded = juce::roundToInt (value);
                                                                     return (rounded > 0 ? "+" : "") + juce::String (rounded);
                                                                 }),
                               kParamVersionBalance));

        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (balanceSpeed, kParamVersionBalance),
                                                                  "Balance Speed",
                                                                  juce::StringArray { "Slow", "Mid", "Fast" }, defaults::balanceSpeedIndex));

        layout.add (makeBool (balanceBypass, "Balance Bypass", defaults::balanceBypass, kParamVersionBalance));

        // 新实例从温和的自动处理开始；0 仍是严格手动。旧工程缺参的回退值在处理器中独立保留为 0。
        layout.add (makeFloat (glazeAmount, "Glaze",
                               ranges::glazeAmount(), defaults::glazeAmount, integerAttributes ("%"), kParamVersionGlaze));

        // ---- GLAZE 风格（ADR-008；v0.6 引入，版本号 6）：自动 / 温润 / 冲击 / 通透，默认自动 ----
        // 选项名用英文（宿主自动化列表里显示，与 Balance Speed 一致）；界面胶囊显示中文。顺序 = glaze::Style 的下标。
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (glazeStyle, kParamVersionGlazeStyle),
                                                                  "Glaze Style",
                                                                  juce::StringArray { "Auto", "Warm", "Punchy", "Clean" }, defaults::glazeStyleIndex));

        // ---- Clay 修坯（设计规格-Clay 第 6 节、ADR-009；v0.1.7.0 引入，ParameterID 版本 kParamVersionClay = 7）----
        // 全部在 CLAY 卡片上（v0.1.7.0 时在「高级」抽屉里）、都可自动化，追加在参数表末尾（已发布参数的下标不动）。顺序与 ParameterIds.h 的 allParameterIds 一致。
        // 每项「三态 + 手动值」：三态默认「自动」——GLAZE 为 0 时自动值为 0（不处理）；「手动」严格按旋钮值，GLAZE 0 也生效；
        // 「关」永远不处理。三态合成在 GLAZE 编排器里（glaze::resolveClayParams），这里只定范围、默认值与显示。
        // 旧工程缺这些参数时回到默认（addMissingParameterNodes 补只带 id 的节点）：三态「自动」而旧画像没有立体声 / 超低频特征，
        // 决策器给的自动值为 0，打开旧工程声音不变（设计规格-Clay 第 7 节）。
        // 手动值的范围必须与 ClayModule 的钳位常数一致（minLowCutHz … maxWidthDb），处理器 Debug 版构造时逐项核对。
        layout.add (makeClayMode (clayLowCutMode, "Clay Low Cut Mode", defaults::clayLowCutModeIndex));

        // 低切（L/R 4 阶 Butterworth 高通）：10 ～ 60 Hz 对数刻度，默认 25 Hz；整数显示「25 Hz」。
        layout.add (makeFloat (clayLowCutHz, "Clay Low Cut",
                               ranges::clayLowCut(), defaults::clayLowCutHz, integerAttributes ("Hz"), kParamVersionClay));

        layout.add (makeClayMode (clayMonoMode, "Clay Mono Mode", defaults::clayMonoModeIndex));

        // 低频收单（只对 S 的 2 阶高通）的分频点：40 ～ 300 Hz 对数刻度，默认 120 Hz；整数显示「120 Hz」。
        layout.add (makeFloat (clayMonoHz, "Clay Mono",
                               ranges::clayMono(), defaults::clayMonoHz, integerAttributes ("Hz"), kParamVersionClay));

        layout.add (makeClayMode (clayWidthMode, "Clay Width Mode", defaults::clayWidthModeIndex));

        // 加宽（只对 S 的 2 kHz 高架）：−3 ～ +3 dB 线性，默认 0；步长与显示同 Color Tone（0.01 dB 步进、「+0.8 dB」一位小数）。
        // 负值 = 收窄，只能手动（自动只给 0 ～ +1.5 dB）。
        layout.add (makeFloat (clayWidthDb, "Clay Width",
                               ranges::clayWidth(), defaults::clayWidthDb, decibelAttributes(), kParamVersionClay));

        layout.add (makeBool (clayBypass, "Clay Bypass", defaults::clayBypass, kParamVersionClay));

        // ---- 高级母带控制（设计规格-高级母带控制 第 2 节、ADR-020；0.1.9.0 引入，版本号 8）----
        // 新实例：推响不刺、目标频谱都「自动」；旧工程缺这几个参数时四项特判成关（PluginProcessor::addMissingParameterNodes）。
        // 选项顺序 = celadon::HarshMode / TargetMode / DitherMode 的枚举顺序；默认下标见 ParameterDefaults.h。
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (masterHarshMode, kParamVersionMastering),
                    "Harshness Mode", juce::StringArray { "Auto", "Off", "Manual" }, defaults::masterHarshModeIndex));
        layout.add (makeFloat (masterHarshAmount, "Harshness Protection", { 0.0f, 100.0f, 0.1f }, defaults::masterHarshAmount,
                               integerAttributes ("%"), kParamVersionMastering));
        layout.add (makeFloat (masterTransient, "Transient Preserve", { 0.0f, 100.0f, 0.1f }, defaults::masterTransient,
                               integerAttributes ("%"), kParamVersionMastering));
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (masterTargetMode, kParamVersionMastering),
                    "Spectrum Target", juce::StringArray { "Off", "Auto", "Warm", "Neutral", "Bright", "Reference" }, defaults::masterTargetModeIndex));
        layout.add (makeFloat (masterTargetAmount, "Spectrum Target Amount", { 0.0f, 100.0f, 0.1f }, defaults::masterTargetAmount,
                               integerAttributes ("%"), kParamVersionMastering));
        layout.add (makeFloat (masterCodecMarginDb, "Codec Headroom Reserve", { 0.0f, 1.5f, 0.1f }, defaults::masterCodecMarginDb,
                               decibelAttributes(), kParamVersionMastering));
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (masterDither, kParamVersionMastering),
                    "Export Dither", juce::StringArray { "Off", "16 bit", "24 bit" }, defaults::masterDitherIndex));
        layout.add (makeFloat (masterPreviewLufs, "Normalisation Preview Target", { -24.0f, -8.0f, 0.1f }, defaults::masterPreviewLufs,
                    decibelAttributes().withLabel ("LUFS"), kParamVersionMastering));

        // ---- 曲风与交付（T-038，设计规格-曲风与交付 第 5 节、ADR-026；版本 9）----
        // 一首歌定一次，不进宿主自动化。APVTS 按选项下标存状态：以后只许在末尾追加选项（禁止重排、插入、删除），旧工程才不会错位。
        // 选项顺序 = glaze::Genre / glaze::Delivery 的下标；旧工程缺这两个参数时按 0（通用 / 标准）读（addMissingParameterNodes）。
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (glazeGenre, kParamVersionGenre), "Genre",
                    juce::StringArray { "General", "Deep House", "Tech House", "Melodic-Progressive" }, defaults::glazeGenreIndex,
                    juce::AudioParameterChoiceAttributes().withAutomatable (false)));
        layout.add (std::make_unique<juce::AudioParameterChoice> (makeId (glazeDelivery, kParamVersionGenre), "Delivery",
                    juce::StringArray { "Standard", "Streaming", "Club" }, defaults::glazeDeliveryIndex,
                    juce::AudioParameterChoiceAttributes().withAutomatable (false)));
        return layout;
    }
}
