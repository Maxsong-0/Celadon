/*
  ==============================================================================

    ParameterDefaults.h
    全部参数的「新实例默认值」——一处真源（T-032 R10-01，2026-09-29）。

    以前出厂默认写在两处：参数表（Parameters.cpp）与离线对照工具（tools/render/RenderMain.cpp 的 makeFactoryParams「照参数表手写一份」）。
    0.1.9.0 加了高级母带 8 个参数、参数表默认「自动」（ADR-020），渲染工具那份没跟上：22 首门槛与试听包渲染的是两项关着的链，
    与用户新实例听到的不同。现在参数表、渲染工具、性能基准（tools/perf/bench.mm 的链级参数）都从这里取默认值；
    处理器 Debug 版构造时逐项核对「参数表的 getDefaultValue() 反归一 == factoryChainParams() 的对应字段」，以后再漂就在启动时断住。

    与 ParameterRanges.h 同一做法：标量常量不依赖任何 JUCE 模块；factoryChainParams() 要用 ChainParams（含各模块的 Params），
    所以本头文件包含 dsp/ChainParams.h（只依赖 juce_dsp，无头工具与测试都能编）。
    三态 / 选项类参数的默认按选项下标写（AudioParameterChoice 的默认就是下标），并在 factoryChainParams() 里转成对应枚举——
    枚举值与选项顺序一一对应（Parameters.cpp 的 StringArray 顺序 = 枚举定义顺序；Debug 核对也按下标比）。

    改默认值：只改这里；旧工程缺参数时的回退值另在 PluginProcessor::addMissingParameterNodes（与新实例默认有意不同，ADR-020）。

  ==============================================================================
*/

#pragma once

#include "dsp/ChainParams.h"

namespace celadon::param::defaults
{
    // ---- Limit（设计规格-Limit「参数」表）----
    inline constexpr float limitGainDb      = 0.0f;     // limit_gain          0 dB
    inline constexpr float limitCeilingDb   = -1.0f;    // limit_ceiling       −1.0 dBTP
    inline constexpr float limitReleaseMs   = 100.0f;   // limit_release       100 ms（Auto 开着时不起作用）
    inline constexpr bool  limitReleaseAuto = true;     // limit_release_auto  开
    inline constexpr float limitLookaheadMs = 2.0f;     // limit_lookahead     2 ms（决定链延迟）
    inline constexpr float limitClip        = 0.0f;     // limit_clip          0
    inline constexpr bool  limitListen      = false;    // limit_listen        关
    inline constexpr bool  limitBypass      = false;    // limit_bypass        关

    // ---- Color（设计规格-Color「参数」表）----
    inline constexpr float colorToneDb   = 0.0f;        // color_tone    0 dB
    inline constexpr float colorGlue     = 0.0f;        // color_glue    0
    inline constexpr float colorHeat     = 0.0f;        // color_heat    0
    inline constexpr float colorMix      = 100.0f;      // color_mix     100 %
    inline constexpr float colorOutputDb = 0.0f;        // color_output  0 dB
    inline constexpr bool  colorBypass   = false;       // color_bypass  关

    // ---- Balance（设计规格-Balance「参数」表）----
    inline constexpr float balanceRecover    = 0.0f;    // balance_recover   0
    inline constexpr float balanceTame       = 0.0f;    // balance_tame      0
    inline constexpr float balanceBrighten   = 0.0f;    // balance_brighten  0
    inline constexpr int   balanceSpeedIndex = 1;       // balance_speed     下标 1 = Mid（Slow / Mid / Fast）
    inline constexpr bool  balanceBypass     = false;   // balance_bypass    关

    // ---- GLAZE（v0.4 / v0.6）：新实例从温和的自动处理开始；旧工程缺参回 0（addMissingParameterNodes）----
    inline constexpr float glazeAmount     = 35.0f;     // glaze_amount  35 %
    inline constexpr int   glazeStyleIndex = 0;         // glaze_style   下标 0 = Auto（Auto / Warm / Punchy / Clean）

    // ---- Clay 修坯（设计规格-Clay 第 6 节，ADR-009）----
    inline constexpr int   clayLowCutModeIndex = 0;     // clay_lowcut_mode  下标 0 = Auto（Auto / Off / Manual = celadon::ClayMode）
    inline constexpr float clayLowCutHz        = 25.0f; // clay_lowcut_hz    25 Hz
    inline constexpr int   clayMonoModeIndex   = 0;     // clay_mono_mode    Auto
    inline constexpr float clayMonoHz          = 120.0f;// clay_mono_hz      120 Hz
    inline constexpr int   clayWidthModeIndex  = 0;     // clay_width_mode   Auto
    inline constexpr float clayWidthDb         = 0.0f;  // clay_width_db     0 dB
    inline constexpr bool  clayBypass          = false; // clay_bypass       关

    // ---- 高级母带控制（设计规格-高级母带控制 第 2 节，ADR-020：新实例两项「自动」，旧工程读入时四项特判成关）----
    inline constexpr int   masterHarshModeIndex  = 0;      // master_harsh_mode      下标 0 = Auto（Auto / Off / Manual = celadon::HarshMode）
    inline constexpr float masterHarshAmount     = 50.0f;  // master_harsh_amount    50 %
    inline constexpr float masterTransient       = 50.0f;  // master_transient       50 %
    inline constexpr int   masterTargetModeIndex = 1;      // master_target_mode     下标 1 = Auto（Off / Auto / Warm / Neutral / Bright / Reference = celadon::TargetMode）
    inline constexpr float masterTargetAmount    = 35.0f;  // master_target_amount   35 %
    inline constexpr float masterCodecMarginDb   = 0.0f;   // master_codec_margin_db 0 dB
    inline constexpr int   masterDitherIndex     = 0;      // master_dither          下标 0 = Off（Off / 16 bit / 24 bit = celadon::DitherMode）
    inline constexpr float masterPreviewLufs     = -14.0f; // master_preview_lufs    −14 LUFS

    // 新实例的整份链级参数（处理器 readParams 在参数表默认下读到的就是它）。claySettings 只填用户设置；clay（交给 ClayModule 的
    // 实际值）作为输入时编排器忽略（ChainParams.h 文件头），保持结构默认。balance.automaticRecover / automaticTame 与
    // balance.harsh* / target* 不是参数（编排器与链填），同样保持结构默认。
    inline ChainParams factoryChainParams() noexcept
    {
        ChainParams p;

        p.limit.gainDb      = limitGainDb;
        p.limit.ceilingDb   = limitCeilingDb;
        p.limit.releaseMs   = limitReleaseMs;
        p.limit.autoRelease = limitReleaseAuto;
        p.limit.lookaheadMs = limitLookaheadMs;
        p.limit.clip        = limitClip;
        p.limit.listen      = limitListen;
        p.limit.bypass      = limitBypass;

        p.color.toneDb     = colorToneDb;
        p.color.glue       = colorGlue;
        p.color.heat       = colorHeat;
        p.color.mixPercent = colorMix;
        p.color.outputDb   = colorOutputDb;
        p.color.bypass     = colorBypass;

        p.balance.recover  = balanceRecover;
        p.balance.tame     = balanceTame;
        p.balance.brighten = balanceBrighten;
        p.balance.speed    = static_cast<BalanceModule::Speed> (balanceSpeedIndex);
        p.balance.bypass   = balanceBypass;

        p.glazePercent = glazeAmount;
        p.glazeStyle   = glazeStyleIndex;

        p.claySettings.lowCutMode = static_cast<ClayMode> (clayLowCutModeIndex);
        p.claySettings.lowCutHz   = clayLowCutHz;
        p.claySettings.monoMode   = static_cast<ClayMode> (clayMonoModeIndex);
        p.claySettings.monoHz     = clayMonoHz;
        p.claySettings.widthMode  = static_cast<ClayMode> (clayWidthModeIndex);
        p.claySettings.widthDb    = clayWidthDb;
        p.claySettings.bypass     = clayBypass;

        p.mastering.harshMode         = static_cast<HarshMode> (masterHarshModeIndex);
        p.mastering.harshAmount       = masterHarshAmount;
        p.mastering.transientPreserve = masterTransient;
        p.mastering.targetMode        = static_cast<TargetMode> (masterTargetModeIndex);
        p.mastering.targetAmount      = masterTargetAmount;
        p.mastering.codecMarginDb     = masterCodecMarginDb;
        p.mastering.dither            = static_cast<DitherMode> (masterDitherIndex);
        p.mastering.previewTargetLufs = masterPreviewLufs;

        return p;
    }
}
