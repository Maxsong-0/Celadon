/*
  ==============================================================================

    GlazeStatusText.cpp
    状态行的判定与文字。优先级表见头文件；这里的每个分支按表的顺序排，改顺序时两边一起改。

    只依赖 juce_core：这个文件可以同时进插件与无头测试编译。

  ==============================================================================
*/

#include "GlazeStatusText.h"

#include "HelpText.h"

#include <cmath>
#include <cstdlib>

namespace celadon::ui::glazestatus
{
    namespace
    {
        using glaze::Phase;

        constexpr int maxShownSeconds = 999;   // 秒数显示上限：只防荒唐值撑爆整数与文字，正常 15 ～ 30 s 远在其下
        constexpr int maxShownTenths  = 9999;  // 0.1 精度的数字显示上限（±999.9）

        // 一位小数；负号用 U+2212 减号（规格写「−9.6 LUFS」，不是 ASCII 连字符，这里写成转义免得被编辑器悄悄换掉）；0 写「0.0」。
        juce::String formatTenths (int tenths)
        {
            const auto magnitude = std::abs (tenths);
            const auto text = juce::String (magnitude / 10) + "." + juce::String (magnitude % 10);
            return tenths < 0 ? utf8 ("\xe2\x88\x92") + text : text;
        }

        // 有限值 → 0.1 精度的整数。先钳再取整：荒唐的大数不会在 roundToInt 里溢出。
        int toTenths (float value) noexcept
        {
            const auto limit = (double) maxShownTenths / 10.0;
            return juce::roundToInt (juce::jlimit (-limit, limit, (double) value) * 10.0);
        }

        // 已锁定里的风格：选「自动」写「自动→温润」（此刻偏向哪种），选定某种就只写它。
        juce::String styleText (const View& view, Language language)
        {
            if (view.chosenStyle != glaze::Style::automatic)
                return styleName (view.chosenStyle, language);

            if (view.resolvedStyle == glaze::Style::automatic)   // 编排器还没给出偏向（不该发生）：只写「自动」
                return styleName (glaze::Style::automatic, language);

            return styleName (glaze::Style::automatic, language) + utf8 ("→") + styleName (view.resolvedStyle, language);
        }

        // 英文版（v0.1.8.0）：与中文逐条对应，段的划分相同（「 · 」连接）。
        juce::String englishTextFor (const View& view)
        {
            const auto dot = utf8 (" \xc2\xb7 ");

            switch (view.kind)
            {
                case Kind::unlearned:
                    return view.glazeActive
                         ? "Not learned" + dot + "listens on playback (export uses a safe plan)"
                         : "Not learned" + dot + "listens on playback";

                case Kind::unlearnedExported:
                    return "Not learned, export used a safe plan";

                case Kind::listening:
                    return view.requiredSeconds > 0
                         ? "Listening " + juce::String (view.listenedSeconds) + " / " + juce::String (view.requiredSeconds) + " s"
                         : "Listening " + juce::String (view.listenedSeconds) + " s";

                case Kind::calibrating:
                    return "Calibrating";

                case Kind::calibrationPaused:
                    return view.pausedByTransport ? juce::String ("Calibration paused (stopped)")
                                                  : juce::String ("Calibration paused (bypassed)");

                case Kind::awaitingCalibration:
                    return "Locked" + dot + "awaiting calibration (conditions changed)";

                case Kind::locked:
                {
                    auto text = "Locked" + dot + (view.olderPlan ? juce::String ("legacy plan") : styleText (view, Language::english));

                    if (view.hasTarget)
                        text << dot << "Target " << formatTenths (view.targetTenthsLufs) << " LUFS";

                    return text;
                }

                case Kind::planLimited:
                    return view.yieldedTenthsDb > 0
                         ? "Plan limited" + dot + "backed off " + formatTenths (view.yieldedTenthsDb) + " dB"
                         : juce::String ("Plan limited");

                case Kind::limitedByManual:
                    return "Limited by manual settings";

                case Kind::profileMismatch:
                    return "Music differs from what was learned" + dot + "relearn suggested";
            }

            return {};
        }
    }

    //==============================================================================
    bool View::operator== (const View& other) const noexcept
    {
        return kind == other.kind
            && glazeActive == other.glazeActive
            && listenedSeconds == other.listenedSeconds
            && requiredSeconds == other.requiredSeconds
            && chosenStyle == other.chosenStyle
            && resolvedStyle == other.resolvedStyle
            && hasTarget == other.hasTarget
            && targetTenthsLufs == other.targetTenthsLufs
            && yieldedTenthsDb == other.yieldedTenthsDb
            && pausedByTransport == other.pausedByTransport
            && olderPlan == other.olderPlan;
    }

    //==============================================================================
    View classify (const glaze::Status& status, float glazePercent, glaze::Style chosenStyle, bool listenOn) noexcept
    {
        juce::ignoreUnused (listenOn); // 全链 Listen 位于校准测点之后，不再暂停处理或校准。
        View view;

        // 「> 0」而不是「!= 0」：NaN 与负数都当作 0（与音频线程判断 GLAZE 是否起作用同一写法）。
        // 只写进「未学习」与「已锁定」两种 View（只有它们的文字随它变），别的情形保持默认，免得 GLAZE 在 0 与 > 0 之间
        // 切换时 View 白白变一次。
        const bool glazeActive = glazePercent > 0.0f;

        const bool listening = status.phase == Phase::listening || status.relearnInProgress;
        const bool settingUp = status.phase == Phase::planTransition || status.phase == Phase::calibrating;
        const bool awaiting  = status.phase == Phase::locked && ! status.calibrationValid;

        // 1、2：琥珀。只在 GLAZE > 0 时有意义（GLAZE 0 没有自动增量可撤、没有方案可受限）。
        if (glazeActive && status.limitedByManual)
        {
            view.kind = Kind::limitedByManual;
            return view;
        }

        if (glazeActive && status.planLimited)
        {
            view.kind = Kind::planLimited;
            view.yieldedTenthsDb = std::isfinite (status.protectionDb) ? std::abs (toTenths (status.protectionDb)) : 0;
            return view;
        }

        // 3：聆听中（首次聆听，或「重新聆听」的候选版本在建：旧方案照常生效，但进行中的是聆听）。
        if (listening)
        {
            view.kind = Kind::listening;
            // 要求秒数四舍五入；不是正的有限值（编排器没给）就只写已听秒数。先钳再取整，荒唐值不会溢出。
            view.requiredSeconds = std::isfinite (status.requiredSeconds) && status.requiredSeconds > 0.0f
                                       ? juce::jlimit (1, maxShownSeconds,
                                                       juce::roundToInt (juce::jmin ((double) status.requiredSeconds,
                                                                                     (double) maxShownSeconds)))
                                       : 0;

            // 已听秒数向下取整：数到「15 / 15」时正好听满。超过要求（置信度不够、继续听到上限）时停在要求值上，不写「17 / 15」。
            const auto ceiling = view.requiredSeconds > 0 ? view.requiredSeconds : maxShownSeconds;
            view.listenedSeconds = std::isfinite (status.listenedSeconds) && status.listenedSeconds > 0.0f
                                       ? (int) std::floor (juce::jmin ((double) status.listenedSeconds, (double) ceiling))
                                       : 0;
            return view;
        }

        // 3b：画像失配（琥珀，要用户动手：点「重新聆听」）。只在 GLAZE > 0 时有意义；聆听中已经在做这件事，排在它后面。
        if (glazeActive && status.profileMismatch)
        {
            view.kind = Kind::profileMismatch;
            return view;
        }

        // 4：未学习。GLAZE 0 时不写导出说明（导出就是手动处理的结果，没有「保守方案」）。
        if (status.phase == Phase::unlearned)
        {
            view.kind = glazeActive && status.unlearnedOfflineWarning ? Kind::unlearnedExported : Kind::unlearned;
            view.glazeActive = glazeActive && view.kind == Kind::unlearned;
            return view;
        }

        if (glazeActive)
        {
            // 5：校准暂停——只在此刻本该校准时（生成方案后的过渡、校准中、待校准）。原因：宿主旁通，
            //    或宿主停止播放（没有音乐，校准原地不动；不写「校准中」，免得看起来一直在校准却校准不完）。
            if (status.calibrationPaused && (settingUp || awaiting))
            {
                view.kind = Kind::calibrationPaused;
                return view;
            }

            if (status.waitingForPlayback && (settingUp || awaiting))
            {
                view.kind = Kind::calibrationPaused;
                view.pausedByTransport = true;
                return view;
            }

            // 6：校准中（方案过渡的约 3 s 也算：用户看来都是「方案在就位」）。
            if (settingUp)
            {
                view.kind = Kind::calibrating;
                return view;
            }

            // 7：待校准。
            if (awaiting)
            {
                view.kind = Kind::awaitingCalibration;
                return view;
            }
        }

        // 8：已锁定（GLAZE 0 时学完了就是这一行，只写风格、不写目标）。
        view.kind = Kind::locked;
        view.glazeActive = glazeActive;
        view.chosenStyle = chosenStyle;
        view.resolvedStyle = chosenStyle == glaze::Style::automatic ? status.resolvedStyle : glaze::Style::automatic;
        view.olderPlan = glazeActive && status.savedPlanFromOlderAlgorithm;

        if (view.olderPlan)
        {
            // 旧版方案不写风格（它是旧算法按当时的风格算的，此刻的「自动→X」对它没有意义）。
            view.chosenStyle = glaze::Style::automatic;
            view.resolvedStyle = glaze::Style::automatic;
        }

        if (glazeActive && std::isfinite (status.effectiveTargetLufs))
        {
            view.hasTarget = true;
            view.targetTenthsLufs = toTenths (status.effectiveTargetLufs);
        }

        return view;
    }

    //==============================================================================
    juce::String textFor (const View& view)
    {
        switch (view.kind)
        {
            case Kind::unlearned:
                return view.glazeActive
                     ? utf8 ("未学习 · 播放后自动聆听（导出将用保守方案）")
                     : utf8 ("未学习 · 播放后自动聆听");

            case Kind::unlearnedExported:
                return utf8 ("未学习，导出使用保守方案");

            case Kind::listening:
                return view.requiredSeconds > 0
                     ? utf8 ("聆听中 ") + juce::String (view.listenedSeconds) + " / " + juce::String (view.requiredSeconds) + " s"
                     : utf8 ("聆听中 ") + juce::String (view.listenedSeconds) + " s";

            case Kind::calibrating:
                return utf8 ("校准中");

            case Kind::calibrationPaused:
                return view.pausedByTransport ? utf8 ("校准暂停（未播放）")
                                              : utf8 ("校准暂停（旁通）");

            case Kind::awaitingCalibration:
                return utf8 ("已锁定 · 待校准（条件已变，稳定后自动校准）");

            case Kind::locked:
            {
                auto text = utf8 ("已锁定 · ") + (view.olderPlan ? utf8 ("沿用旧版方案") : styleText (view, Language::chinese));

                if (view.hasTarget)
                    text << utf8 (" · 目标 ") << formatTenths (view.targetTenthsLufs) << " LUFS";

                return text;
            }

            case Kind::planLimited:
                return view.yieldedTenthsDb > 0
                     ? utf8 ("方案受限 · 已退让 ") + formatTenths (view.yieldedTenthsDb) + " dB"
                     : utf8 ("方案受限");

            case Kind::limitedByManual:
                return utf8 ("受手动设置限制");

            case Kind::profileMismatch:
                return utf8 ("与聆听时的音乐差很多 · 建议重新聆听");
        }

        return {};
    }

    juce::String textFor (const View& view, Language language)
    {
        return language == Language::english ? englishTextFor (view) : textFor (view);
    }

    juce::StringArray segmentsFor (const View& view, Language language)
    {
        juce::StringArray segments;
        const auto text = textFor (view, language);
        const auto separator = utf8 (" \xc2\xb7 ");
        int start = 0;

        // 按「 · 」拆（StringArray::addTokens 会把多字节分隔符当成字符集合，这里自己找）。
        for (;;)
        {
            const auto at = text.indexOf (start, separator);

            if (at < 0)
            {
                segments.add (text.substring (start));
                break;
            }

            segments.add (text.substring (start, at));
            start = at + separator.length();
        }

        segments.removeEmptyStrings();
        return segments;
    }

    Tone toneFor (Kind kind) noexcept
    {
        switch (kind)
        {
            case Kind::listening:
            case Kind::calibrating:
                return Tone::accent;

            case Kind::planLimited:
            case Kind::limitedByManual:
            case Kind::profileMismatch:
                return Tone::amber;

            case Kind::unlearned:
            case Kind::unlearnedExported:
            case Kind::calibrationPaused:
            case Kind::awaitingCalibration:
            case Kind::locked:
                return Tone::dim;
        }

        return Tone::dim;
    }

    juce::String styleName (glaze::Style style)
    {
        return styleName (style, Language::chinese);
    }

    juce::String styleName (glaze::Style style, Language language)
    {
        const auto english = language == Language::english;

        switch (style)
        {
            case glaze::Style::automatic: return english ? juce::String ("Auto")   : utf8 ("自动");
            case glaze::Style::warm:      return english ? juce::String ("Warm")   : utf8 ("温润");
            case glaze::Style::punchy:    return english ? juce::String ("Punchy") : utf8 ("冲击");
            case glaze::Style::clean:     return english ? juce::String ("Clean")  : utf8 ("通透");
        }

        return english ? juce::String ("Auto") : utf8 ("自动");
    }

    glaze::Style styleFromParameterValue (float rawValue) noexcept
    {
        if (! std::isfinite (rawValue))
            return glaze::Style::automatic;

        return static_cast<glaze::Style> (juce::jlimit (0, 3, juce::roundToInt (rawValue)));
    }
}
