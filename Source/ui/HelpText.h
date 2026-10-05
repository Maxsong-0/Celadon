/*
  ==============================================================================

    HelpText.h
    全部控件的提示文案（设计规格-界面 第 4 节，逐字一致）。

    只依赖 juce_core 与 dsp/GlazeTypes.h（纯数据类型，不含 JUCE）：插件和无头测试都能编。测试会检查 ParameterIds.h 里的
    每个参数、每个界面元素、每个 Balance 占位都有三行非空文案。

    v0.1.7.0 修坯（设计规格-Clay 第 5 节）：BALANCE→COLOR 箭头上的三灯指示（ClayIndicator）悬停气泡沿用三行格式，但第 2 行是
    此刻的实际值（「低切 25 Hz · 未收单 · 高频加宽 +0.8 dB · 低频反相：单声道播放会丢低音」）。这一行的文字、三盏灯的颜色类别，
    都由本文件末尾的纯函数从编排器的状态快照算出（ClayReadout）。放在这里而不是 ClayIndicator 里：HelpText 是界面里少数
    进无头测试编译的文件之一（CMakeLists 的 CELADON_HEADLESS_UI_SOURCES），判定与文字才能有自动测试——与 GlazeStatusText
    把状态行的判定与颜色类别放在无头文件里是同一种做法。
    ADR-012「先试后用」（设计规格-Clay-根治修订 3.5）：某项因「本曲试过会抬高峰值」被减量 / 否决时，气泡第 3 行写
    「本曲试过：低切会抬高峰值，已自动跳过」之类（clayCostLine）；优先级低于「点「重新聆听」后启用自动修坯」。

    v0.1.8.0 界面改版（中英双语）：每条文案另有英文三行（titleEn / effectEn / adviceEn），带 Language 参数的重载按语言取；
    不带语言参数的旧接口仍返回中文，逐字不变（测试按中文原文核对）。三灯指示改在 CLAY 卡片上逐行显示（每行一盏灯），
    判定与文字仍在这里。

    T-038 曲风与交付：两个参数的文案定稿；House 档反相提示的读数（ClayReadout::housePhaseHint）与文字、House 暂定方案期间
    GLAZE 状态的悬停气泡也在这里（文件末尾），同样进无头测试。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "../dsp/GlazeTypes.h"
#include "Localisation.h"   // celadon::utf8 与 celadon::Language

namespace celadon::help
{
    struct Entry
    {
        const char* id;       // 参数 ID（ParameterIds.h）或界面元素 ID（celadon::uiid）
        const char* title;    // 第 1 行：「名字 · 一句话作用」
        const char* effect;   // 第 2 行：效果
        const char* advice;   // 第 3 行：建议
        const char* titleEn;  // 同上三行的英文（v0.1.8.0）
        const char* effectEn;
        const char* adviceEn;
    };

    // 全部条目。
    const juce::Array<Entry>& allEntries();

    // 按 ID 查；找不到返回 nullptr。
    const Entry* find (juce::StringRef id);

    // 气泡文字：三行用 '\n' 连接（已转成正确的 Unicode）；找不到返回空串。不带语言参数 = 中文。
    juce::String tooltipFor (juce::StringRef id);
    juce::String tooltipFor (juce::StringRef id, Language language);

    //==============================================================================
    // 修坯三灯指示（v0.1.7.0，设计规格-Clay 第 5 节）：三盏灯自上而下 = 修坯的三级（低切 / 收单 / 加宽，= ClayModule 的处理顺序）。
    enum class ClayStage : int { lowCut = 0, mono = 1, width = 2 };
    inline constexpr int numClayStages = 3;

    // 一盏灯的颜色类别：暗（灰圈，没在处理）/ 亮（实际值非 0：「自动」紫圈、「手动」青色实心）/ 保护（橙色实心，该级的宽度保护正在介入或低频反相提示亮着）。
    // 界面按 palette::lampIdle / violet / c600 / warning 画（ModuleCards.cpp 的 paint）；橙色表示要留意：「保护中 / 提示中」。
    enum class ClayLamp : int { off = 0, on = 1, guard = 2 };

    // 界面从参数读到的用户设置里，只影响「显示什么」的那几项。
    struct ClayUserState
    {
        bool lowCutAuto = true;   // clay_lowcut_mode == 自动
        bool monoAuto   = true;   // clay_mono_mode == 自动：低频反相提示只在收单选「自动」时报（ADR-015；「手动」「关」= 用户接管）
        bool widthAuto  = true;   // clay_width_mode == 自动：宽度保护同理
        bool bypassed   = false;  // clay_bypass：旁通时什么都没在处理，三灯全暗
    };

    // 指示灯与气泡此刻要显示的内容：只含决定颜色与文字的量，已量化到显示精度（Hz 取整、收单比例取整百分比、加宽 0.1 dB）。
    // 两份相等 = 灯和气泡一个像素都不会变：编辑器每 30 ms 算一份，灯只在颜色类别变了时重画，气泡只在读数变了时重拼文字。
    // 实际值非 0 时显示值至少是 1 个单位（灯亮着，文字就不写「0 %」「+0.0 dB」）。
    struct ClayReadout
    {
        int lowCutHz = 0;            // 0 = 未低切；否则整数 Hz
        int monoHz = 0;              // 收单分频点（整数 Hz）；monoPercent = 0 时清成 0
        int monoPercent = 0;         // 收单比例 0 ～ 100 %
        int widthTenthsDb = 0;       // 高频加宽 × 10，带符号（负 = 收窄，只有手动给得出）
        bool phaseGuard = false;     // 低频反相提示中（只在收单选「自动」时；ADR-015 起不改声音）
        bool widthGuard = false;     // 宽度保护介入中（只算自动态的加宽）
        // T-074（设计 R4-03）：峰值守护正在深度回收低切 / 收单（Status::clayPeakGuardActive：保留比 Σ(g·d)² / Σd² < 75 % 持续 ≥ 1 s）。
        // 手动 / 自动都报（手动项恰恰没有别的反馈）；只在低切或收单此刻的实际值不为 0 时置，加宽不经守护、不受它影响。
        // peakGuardKeptPercent = 保留比取整到 5 %（0 ～ 100；气泡写「实际只剩约 N %」；取整是为了读数不在每帧小幅抖动时让气泡文字白白重拼）。
        bool peakGuard = false;
        int peakGuardKeptPercent = 100;
        bool needsRelearn = false;   // 有自动态的项、但已提交画像缺立体声 / 超低频特征（旧工程）：气泡第 3 行提示「重新聆听」
        bool bypassed = false;       // Bypass 修坯：以上各项的值与低频反相提示、宽度保护全清零，第 2 行写「修坯已旁通」
        // ADR-012「先试后用」：该项（自动态）因本曲试过会抬高峰值而被决策器减量 / 否决（Status::clayCostLimitedMask）。
        // 加宽在宽度保护介入时不算（第 2 行已写「宽度保护中」，此刻的实际值由保护定，不是代价的结果）；收单在低频反相提示亮着时
        // 不算（第 2 行已写「低频反相」，ADR-015 起决策器不再置收单的代价位，留着只为读旧快照时口径一致）；旁通时全清零。
        bool lowCutCostLimited = false;
        bool monoCostLimited = false;
        bool widthCostLimited = false;
        // T-038 House 档反相提示（Status::housePhaseHint；设计规格-曲风-House学习方式 第 9 节）：说的是混音本身，不按三态与旁通筛
        //（clayReadoutFor）。损失显示到 0.1 dB（正数，至少 1）；读不到损失时 0（提示照亮、不写数）。
        bool housePhaseHint = false;
        int housePhaseLossTenthsDb = 0;

        bool operator== (const ClayReadout& other) const noexcept;
        bool operator!= (const ClayReadout& other) const noexcept   { return ! operator== (other); }
    };

    // status = 编排器的状态快照（clay* 字段是上一块交给 ClayModule 的实际值，与低频反相提示、宽度保护的状态）；user = 三态与旁通。
    // 非有限的实际值按 0（不亮）处理。
    ClayReadout clayReadoutFor (const glaze::Status& status, const ClayUserState& user) noexcept;

    // 某一盏灯的颜色类别：宽度保护介入或低频反相提示 → 琥珀（压过「亮」：宽度保护把加宽收到 0 时灯仍是琥珀，说明「想加宽、被保护拦住」）；
    // 否则实际值非 0 → 亮（「自动」紫圈、「手动」青色实心，画法见 ModuleCards.cpp）；否则暗。旁通时 readout 里全是 0 → 全暗。
    ClayLamp clayLampFor (const ClayReadout& readout, ClayStage stage) noexcept;

    // 气泡第 2 行：各级此刻的实际值（为 0 时写「未低切」「未收单」「未加宽」），再接低频反相提示与宽度保护状态；旁通时写「修坯已旁通」。
    juce::String clayLiveLine (const ClayReadout& readout, Language language = Language::chinese);

    // 旧工程提示（气泡第 3 行与 CLAY 卡片的提示共用）：「点「重新聆听」后启用自动修坯」。
    juce::String clayRelearnHint (Language language = Language::chinese);

    // ADR-012 的代价说明（设计规格-Clay-根治修订 3.5）：「本曲试过：{低切 / 收单 / 加宽}会抬高峰值，已自动{减量 / 跳过}」（含低切时写
    // 「会抬高峰值或削弱低音」：T-043 / ADR-027，守护的混合损失把低切封顶），
    // 跳过 = 该项此刻实际值为 0（量化读数口径），否则是减量；多项按处理顺序用「、」连接，两种都有时写
    // 「……已自动跳过低切，减量收单」。没有被代价减量 / 否决的项时返回空串。不含句末「。」（与 clayRelearnHint 一样由气泡补）。
    juce::String clayCostLine (const ClayReadout& readout, Language language = Language::chinese);

    // 完整气泡：第 1 行 = 文案表 ui_clay_indicator 的第 1 行；第 2 行 = clayLiveLine；
    // 第 3 行按优先级：needsRelearn → clayRelearnHint + 「。」；否则 House 反相提示（T-038）→ 提示第 1 行 + 为什么要回混音；
    // 否则有代价说明 → clayCostLine + 「。」；否则文案表的第 3 行。
    juce::String clayIndicatorTooltip (const ClayReadout& readout, Language language = Language::chinese);

    // CLAY 卡片每行右边的数值（v0.1.8.0）：「25 Hz」「120 Hz」「120 Hz · 80%」「+0.8 dB」「−0.5 dB」；该级没在处理时返回空串
    //（由界面写「未启用」之类）。负号用 U+2212。
    juce::String clayStageValue (const ClayReadout& readout, ClayStage stage);

    //==============================================================================
    // T-038 House 档（设计规格-曲风-House学习方式 第 7、9 节）
    //
    // 反相提示：「俱乐部单声道会丢低频（约 1.6 dB），请回混音修」。lossTenthsDb = 损失 × 10（ClayReadout::housePhaseLossTenthsDb）；
    // ≤ 0 时不写括号里的数。两行版给 CLAY 卡片（逗号处断开：第 1 行说问题、第 2 行「请回混音修」），一行版给 GLAZE 状态行。
    juce::StringArray housePhaseHintLines (int lossTenthsDb, Language language);
    juce::String housePhaseHintText (int lossTenthsDb, Language language);

    // Status::housePhaseMonoLossDb（dB，≤ 0）→ 上面两个函数要的 lossTenthsDb：|x| × 10 四舍五入，至少 1、最多 999；非有限 → 0。
    // CLAY 卡片（clayReadoutFor）与 GLAZE 状态行（GlazeStatusText）共用，两处写的数一定相同。
    int houseLossTenthsFor (float lossDb) noexcept;

    // House 档暂定方案期间（锁定条件还没满足）GLAZE 状态的悬停气泡：第 1 行同 ui_glaze_status，第 2 行说 House 怎么学，
    // 第 3 行「从 drop 开始播放能学得更快。」（与左下角提示同一句，Localisation 表 cornerHouseTip）。
    juce::String houseListeningTooltip (Language language);
}
