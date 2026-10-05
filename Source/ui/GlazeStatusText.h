/*
  ==============================================================================

    GlazeStatusText.h
    GLAZE 状态行（T-048 阶段 2，接口草图第 1 节）。

    纯函数，只依赖 juce_core 与 dsp/GlazeTypes.h：插件与无头测试都能编（与 HelpText 一样）。
    编辑器定时器每 30 ms 调一次 classify，得到一个量化到显示精度的 View；View 没变就不重拼字符串、不重画。

    没有聆听 / 锁定 / 校准状态机。GLAZE > 0 时只取下面第一条成立的（琥珀优先于梅子青，梅子青优先于灰青）：

      1. 受手动设置限制                                                              琥珀
      2. House 反相提示（现状文案：俱乐部单声道会丢低频…）                            琥珀
      3. 导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出                  琥珀   coldExport（c < 0.5 且宿主离线）
      4. 受限 · 预算 / 交付峰值 / 保对比 / PSR · 已退 1.5 dB                           琥珀   正在退或退到底的那一扇；原因属于写出的退让
      5. 起步 · 还没听到主段落（偏保守）                                              梅子青  c < 0.5
      6. 保持 · 本段较轻                                                              灰青   此刻主段度 m < 0.5，增益被棘轮按住
      7. 跟随 · 自动→温润 · 目标 −9.6 LUFS                                            灰青   House 档前缀「House ·」

    GLAZE ≤ 0 时状态行只写「GLAZE 0 · 只用手动设置」，上面七条都不显示。
    旧工程的「旧的聆听结果已丢弃，声音会变」不在这七条里：它只活在本次打开的界面（编辑器 noteDiscardedLegacyGlaze），不进 Reading。

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
    // 与编排器 GuardCause 同一顺序（接口草图第 4 节；Q8 默认单列 PSR）。界面自己留一份，好在字段还没进 Status 时也能编。
    enum class GuardCause : std::uint8_t
    {
        none = 0,
        budget,
        deliveryPeak,
        contrast,
        psr
    };

    enum class Kind : int
    {
        manualOnly = 0,     // GLAZE 0 · 只用手动设置
        limitedByManual,    // 受手动设置限制
        housePhaseHint,     // 俱乐部单声道会丢低频（约 1.6 dB），请回混音修
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
        bool houseActive = false;
        bool housePhaseHint = false;
        float housePhaseMonoLossDb = 0.0f;
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
        bool housePrefix = false;                               // 第 7 条、House 档：「House ·」
        glaze::Style chosenStyle = glaze::Style::automatic;     // 第 7 条：参数 glaze_style
        glaze::Style resolvedStyle = glaze::Style::automatic;   // 第 7 条且选「自动」：此刻偏向哪种
        bool hasTarget = false;
        int targetTenthsLufs = 0;                               // effectiveTarget × 10，四舍五入
        GuardCause guardCause = GuardCause::none;
        int yieldedTenthsDb = 0;                                // |protectionDb| × 10，四舍五入
        int houseLossTenthsDb = 0;                              // 反相提示：损失 × 10（正数；0 = 不写数）
        int styleSegment = -1;                                  // 第 7 条里风格那一段的下标（胶囊上色）；没有则 −1

        bool operator== (const View& other) const noexcept;
        bool operator!= (const View& other) const noexcept   { return ! operator== (other); }
    };

    // 从编排器快照填 Reading。阶段 2 新增的 confidence / currentMainness / coldExport / guardCause / guardEngaged
    // 已经在 Status 上就读它们；还没有时：c = 0、m = 1、不冷导出、原因 none，guardEngaged 先看旧的 planLimited
    //（字段落地后不再读它）。其余用草图留着的灯：limitedByManual、House 反相、protectionDb、目标、resolvedStyle。
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
}
