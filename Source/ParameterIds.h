/*
  ==============================================================================

    ParameterIds.h
    全部参数 ID 常量。不依赖任何 JUCE 界面模块：处理器、界面、提示文案、无头测试共用。

    规则（见设计规格-总体）：
      - 参数 ID 一经发布就不改，DAW 工程里存的是 ID；
      - 命名「模块_参数」，全小写下划线；
      - 以后加参数只加不删、不改语义。
      - ParameterID 版本号：v0.6 及以前 = 引入它的插件次版本号；ADR-010（插件改用 .jucer 四段号，如 0.1.7.0）之后
        次版本号不再单调，改为「已发布参数的最大版本号 + 1」。宿主（尤其 AU）只要求新参数的版本号大于所有已发布参数。

  ==============================================================================
*/

#pragma once

namespace celadon::param
{
    // ParameterID 版本号：v0.1 引入的参数为 1，v0.2 为 2，v0.3 为 3，v0.4 为 4，v0.6 为 6（v0.5 没有新参数）。发布后不改。
    inline constexpr int kParamVersion           = 1;
    inline constexpr int kParamVersionColor      = 2;
    inline constexpr int kParamVersionBalance    = 3;
    inline constexpr int kParamVersionGlaze      = 4;
    inline constexpr int kParamVersionGlazeStyle = 6;
    inline constexpr int kParamVersionClay       = 7;   // v0.1.7.0 修坯 Clay（已发布最大值 6 + 1）

    // ---- Limit（v0.1）----
    inline constexpr const char* limitGain        = "limit_gain";
    inline constexpr const char* limitCeiling     = "limit_ceiling";
    inline constexpr const char* limitRelease     = "limit_release";
    inline constexpr const char* limitReleaseAuto = "limit_release_auto";
    inline constexpr const char* limitLookahead   = "limit_lookahead";
    inline constexpr const char* limitClip        = "limit_clip";
    inline constexpr const char* limitListen      = "limit_listen";
    inline constexpr const char* limitBypass      = "limit_bypass";

    // ---- Color（v0.2）----
    inline constexpr const char* colorTone   = "color_tone";
    inline constexpr const char* colorGlue   = "color_glue";
    inline constexpr const char* colorHeat   = "color_heat";
    inline constexpr const char* colorMix    = "color_mix";
    inline constexpr const char* colorOutput = "color_output";
    inline constexpr const char* colorBypass = "color_bypass";

    // ---- Balance（v0.3）----
    inline constexpr const char* balanceRecover  = "balance_recover";
    inline constexpr const char* balanceTame     = "balance_tame";
    inline constexpr const char* balanceBrighten = "balance_brighten";
    inline constexpr const char* balanceSpeed    = "balance_speed";     // 三选一：Slow / Mid / Fast
    inline constexpr const char* balanceBypass   = "balance_bypass";

    // ---- GLAZE 总旋钮（v0.4 引入；v0.6 起按音乐状态自动决策，见 dsp/GlazePlanner.h 与 ADR-008）----
    inline constexpr const char* glazeAmount = "glaze_amount";
    inline constexpr const char* glazeStyle  = "glaze_style";   // v0.6：四选一 自动 / 温润 / 冲击 / 通透（下标 = glaze::Style）

    // ---- Clay 修坯（v0.1.7.0，设计规格-Clay 第 6 节；v0.1.7 在「高级」抽屉里，v0.1.8.0 起在 CLAY 卡片上）----
    // *_mode 三选一：自动 / 关 / 手动（下标 = celadon::ClayMode）。默认自动。
    inline constexpr const char* clayLowCutMode = "clay_lowcut_mode";
    inline constexpr const char* clayLowCutHz   = "clay_lowcut_hz";    // 10 ～ 60 Hz，对数刻度，默认 25
    inline constexpr const char* clayMonoMode   = "clay_mono_mode";
    inline constexpr const char* clayMonoHz     = "clay_mono_hz";      // 40 ～ 300 Hz，对数刻度，默认 120
    inline constexpr const char* clayWidthMode  = "clay_width_mode";
    inline constexpr const char* clayWidthDb    = "clay_width_db";     // −3 ～ +3 dB，默认 0
    inline constexpr const char* clayBypass     = "clay_bypass";

    // 已发布的全部参数（测试与 Debug 版构造检查据此确认每个参数都有提示文案）。加参数时同步加在这里。
    inline constexpr const char* allParameterIds[] =
    {
        limitGain, limitCeiling, limitRelease, limitReleaseAuto, limitLookahead, limitClip, limitListen, limitBypass,
        colorTone, colorGlue, colorHeat, colorMix, colorOutput, colorBypass,
        balanceRecover, balanceTame, balanceBrighten, balanceSpeed, balanceBypass,
        glazeAmount, glazeStyle,
        clayLowCutMode, clayLowCutHz, clayMonoMode, clayMonoHz, clayWidthMode, clayWidthDb, clayBypass
    };
}

namespace celadon::uiid
{
    // 不是参数、但也要提示文案的界面元素。GLAZE 从 v0.4 起是正式参数（param::glazeAmount），不在这里。
    inline constexpr const char* meterIn      = "ui_meter_in";
    inline constexpr const char* meterOut     = "ui_meter_out";
    inline constexpr const char* meterGr      = "ui_meter_gr";
    inline constexpr const char* integrated   = "ui_integrated";
    inline constexpr const char* shortTerm    = "ui_short_term";
    inline constexpr const char* resetMeters  = "ui_reset";
    inline constexpr const char* glueGr       = "ui_glue_gr";
    inline constexpr const char* balanceCurve = "ui_balance_curve";   // Balance 面板的 27 段修正曲线
    inline constexpr const char* glazeRelearn = "ui_glaze_relearn";   // v0.6：「重新聆听」按钮（界面动作，不是参数）
    inline constexpr const char* glazeStatus  = "ui_glaze_status";    // v0.6：GLAZE 大旋钮下方的状态行
    inline constexpr const char* clayIndicator = "ui_clay_indicator"; // v0.1.7.0：修坯三灯指示（v0.1.8.0 起是 CLAY 卡片每行的灯；悬停第 2 行是实时值）
    // 顶栏「高级」（v0.1.7.0 起；不是参数）：v0.1.7.0 开合抽屉，v0.1.8.0 起打开「高级」面板——修坯的三态与手动值已在 CLAY 卡片上，
    // 面板写明这一点，并列出还在设计中的三个子项目（推响不刺 / 目标频谱 / 发行安全，ADR-005 只写「即将推出」）。
    inline constexpr const char* advanced      = "ui_advanced";
    inline constexpr const char* limitAudition = "ui_limit_audition"; // v0.1.8.0：LIMIT 卡片的耳机「试听差值」（界面状态，不是参数）

    // 界面缩放比例存在 APVTS 状态树的这个属性里（不是参数，不进自动化；随工程保存）。
    // uiScale 是 v0.1.7 及以前 900 × 560 画布的比例（不再读，留着只为说明旧工程里的这个属性）；
    // v0.1.8.0 起用 uiZoom：1200 × 820 画布的比例（0.7 ～ 2.0）。旧工程没有 uiZoom，按用户设置里的默认大小打开。
    inline constexpr const char* uiScaleProperty = "uiScale";
    inline constexpr const char* uiZoomProperty  = "uiZoom";

    inline constexpr const char* allUiIds[] =
    {
        meterIn, meterOut, meterGr, integrated, shortTerm, resetMeters, glueGr, balanceCurve, glazeRelearn, glazeStatus,
        clayIndicator, advanced, limitAudition
    };
}
