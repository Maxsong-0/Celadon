/*
  ==============================================================================

    GlazeStatusText.h
    GLAZE 状态行（T-048 阶段 2，接口草图第 1 节）。

    纯函数，只依赖 juce_core 与 dsp/GlazeTypes.h：插件与无头测试都能编（与 HelpText 一样）。
    编辑器定时器每 30 ms 调一次 classify，得到一个量化到显示精度的 View；View 没变就不重拼字符串、不重画。

    没有聆听 / 锁定 / 校准状态机。GLAZE > 0 时只取下面第一条成立的（琥珀优先于梅子青，梅子青优先于灰青）：

      1. 受手动设置限制                                                              琥珀
      2. 导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出                  琥珀   coldExport（c < 0.5 且宿主离线）
      3. 受限 · 预算 / 限幅 · 已退 1.5 dB                                             琥珀   预算守护正在退或退到底；原因属于写出的退让
      4. 起步 · 还没听到主段落（偏保守）                                              梅子青  c < 0.5
      5. 保持 · 本段较轻                                                              灰青   此刻主段度 m < 0.5，增益被棘轮按住
      6. 跟随 · 自动→温润 · 目标 −9.6 LUFS                                            灰青

    T-108（ADR-039）删了 T-038 的「House 反相提示」一条与「House ·」前缀，T-097 两扇守护的原因「保对比 / PSR」，
    「交付峰值」改叫「限幅」（交付删了；意思不变：限幅 GR 是超限来源）。
    GLAZE ≤ 0 时状态行只写「GLAZE 0 · 只用手动设置」，上面六条都不显示。
    颜色怎么分给各处见文件末尾的 attentionOwner（T-111 橙色规则）。
    旧工程的两条提示（「旧的聆听结果已丢弃，声音会变」「曲风与交付已取消…」）不在这六条里：只活在本次打开的界面
   （编辑器计时器每帧从处理器取，见 PluginEditor.h setGenreDeliveryResetSource），不进 Reading。

    等响、听原声（「正在听原声」）是 A / B，不是学习，不在这里判定。

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "../dsp/GlazeTypes.h"
#include "Localisation.h"

#include <limits>

namespace celadon::ui::glazestatus
{
    // 与编排器 GuardCause 同一顺序（接口草图第 4 节）。界面自己留一份，好在字段还没进 Status 时也能编。
    enum class GuardCause : std::uint8_t
    {
        none = 0,
        budget,
        limiterPeak     // 原 deliveryPeak（T-108 改名）
    };

    enum class Kind : int
    {
        manualOnly = 0,     // GLAZE 0 · 只用手动设置
        limitedByManual,    // 受手动设置限制
        coldExport,         // 导出时主段统计还没热…
        guardLimited,       // 受限 · 预算 · 已退 1.5 dB
        starting,           // 起步 · 还没听到主段落（偏保守）
        holding,            // 保持 · 本段较轻
        following           // 跟随 · 自动→温润 · 目标 −9.6 LUFS
    };

    // 颜色类别：灰青（稳态）/ 梅子青（起步，进行中）/ 琥珀（要用户动手）。
    // 编辑器：琥珀 → 状态胶囊的警告样式；梅子青（起步）→ 圆点呼吸；灰青 = 胶囊的常规样式。不再有聆听用的橙点。
    enum class Tone : int { dim = 0, accent, amber };

    // 状态行要的那几项。编排器 Status 里还没有的字段，readingFromStatus 用缺省（见该函数）。
    // effectiveTargetLufs 非有限 = 不写目标。
    struct Reading
    {
        bool limitedByManual = false;
        float confidence = 0.0f;             // c。缺统计时是 0
        float currentMainness = 1.0f;        // 正在播的这一段的 m。没有读数时不当成「较轻」
        bool coldExport = false;             // 离线且 c < 0.5
        GuardCause guardCause = GuardCause::none;
        float protectionDb = 0.0f;           // 已退的 dB，≤ 0
        bool guardEngaged = false;           // 正在退，或已经退到底仍超
        float effectiveTargetLufs = std::numeric_limits<float>::quiet_NaN();
        glaze::Style resolvedStyle = glaze::Style::automatic;
    };

    // 状态行此刻要显示的内容：只含决定文字与颜色的量，已量化到显示精度（0.1 dB / 0.1 LU）。
    // 与这一种情形无关的字段一律清成默认值——两次相等就说明画面上一个像素都不会变。
    struct View
    {
        Kind kind = Kind::manualOnly;
        glaze::Style chosenStyle = glaze::Style::automatic;     // 第 6 条：参数 glaze_style
        glaze::Style resolvedStyle = glaze::Style::automatic;   // 第 6 条且选「自动」：此刻偏向哪种
        bool hasTarget = false;
        int targetTenthsLufs = 0;                               // effectiveTarget × 10，四舍五入
        GuardCause guardCause = GuardCause::none;
        int yieldedTenthsDb = 0;                                // |protectionDb| × 10，四舍五入
        int styleSegment = -1;                                  // 第 6 条里风格那一段的下标（胶囊上色）；没有则 −1

        bool operator== (const View& other) const noexcept;
        bool operator!= (const View& other) const noexcept   { return ! operator== (other); }
    };

    // 从编排器快照填 Reading。阶段 2 新增的 confidence / currentMainness / coldExport / guardCause / guardEngaged
    // 已经在 Status 上就读它们；还没有时：c = 0、m = 1、不冷导出、原因 none，guardEngaged 先看旧的 planLimited
    //（字段落地后不再读它）。其余用草图留着的灯：limitedByManual、protectionDb、目标、resolvedStyle。
    Reading readingFromStatus (const glaze::Status& status) noexcept;

    // glazePercent = GLAZE 参数值（0 ～ 100，NaN / 负数按 0）；chosenStyle = 参数 glaze_style。
    View classify (const Reading& reading, float glazePercent, glaze::Style chosenStyle) noexcept;

    // 文字（数字保留一位小数，负号用 U+2212「−」）。不带语言参数 = 中文。
    // 各段之间一律用「 · 」连接（导出那一条用冒号，整句一段）。界面顶部的状态胶囊按它拆开上色（见 segmentsFor）。
    juce::String textFor (const View& view);
    juce::String textFor (const View& view, Language language);

    juce::StringArray segmentsFor (const View& view, Language language);

    Tone toneFor (Kind kind) noexcept;

    // 风格名：自动 / 温润 / 冲击 / 通透；英文 Auto / Warm / Punchy / Clean。
    juce::String styleName (glaze::Style style);
    juce::String styleName (glaze::Style style, Language language);

    // 参数 glaze_style 的原始值（选项下标的 float）→ 风格：取整方式同 AudioParameterChoice::getIndex，钳到 0 ～ 3，非有限值按「自动」。
    glaze::Style styleFromParameterValue (float rawValue) noexcept;

    // ---- 橙色归谁（T-111 / 4-06，规格 V4「橙色规则」）----
    // 同一件事可以在源头和顶部状态胶囊两处同时亮橙；不同的事不同时亮。编辑器每帧把此刻成立的几件事交给 attentionOwner，
    // 只有返回的那一件的各个源头用橙，其余源头退回中性色（OUT 表不报超限、GR 不染橙、标题标签与左侧板状态句用中性色、
    // CLAY 守护灯按「在处理」画、旧工程提示用灰字）。优先级与顶部胶囊一致：
    //   异常音频块（NaN / Inf）> 真峰值超限 > GLAZE 琥珀（受手动设置限制、导出时还没热、受限）> CLAY 守护灯 > 旧工程提示。
    // 图例「橙 = 需要注意」的色块与右侧板历史里的守护刻度不算（一个是钥匙，一个是过去的记录），不经过这里。
    enum class Attention : int { none = 0, nonFinite, truePeak, glaze, clayGuard, legacyNotice };

    struct AttentionInputs
    {
        bool nonFinite = false;       // 最近 10 s 屏蔽过异常音频块
        bool truePeakOver = false;    // 最近 3 s 真峰值超过上限（听原声时不算）
        bool glazeAmber = false;      // GLAZE > 0 且状态行是琥珀（toneFor == amber）
        bool clayGuard = false;       // CLAY 三行里有一盏守护灯亮着
        bool legacyNotice = false;    // 本次打开的旧工程提示还在左下角
    };

    Attention attentionOwner (const AttentionInputs& inputs) noexcept;
}
