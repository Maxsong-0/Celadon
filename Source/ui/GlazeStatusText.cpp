/*
  ==============================================================================

    GlazeStatusText.cpp
    状态行的判定与文字。优先级见头文件；这里的每个分支按那张表的顺序排，改顺序时两边一起改。

    只依赖 juce_core：这个文件可以同时进插件与无头测试编译。

  ==============================================================================
*/

#include "GlazeStatusText.h"

#include "HelpText.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace celadon::ui::glazestatus
{
    namespace
    {
        constexpr int maxShownTenths = 9999;   // 0.1 精度的数字显示上限（±999.9）
        constexpr float confidenceReady = 0.5f;
        constexpr float mainnessHold = 0.5f;

        template <typename T, typename = void>
        struct HasConfidence : std::false_type {};
        template <typename T>
        struct HasConfidence<T, std::void_t<decltype (std::declval<const T&>().confidence)>> : std::true_type {};

        template <typename T, typename = void>
        struct HasCurrentMainness : std::false_type {};
        template <typename T>
        struct HasCurrentMainness<T, std::void_t<decltype (std::declval<const T&>().currentMainness)>> : std::true_type {};

        template <typename T, typename = void>
        struct HasColdExport : std::false_type {};
        template <typename T>
        struct HasColdExport<T, std::void_t<decltype (std::declval<const T&>().coldExport)>> : std::true_type {};

        template <typename T, typename = void>
        struct HasGuardCause : std::false_type {};
        template <typename T>
        struct HasGuardCause<T, std::void_t<decltype (static_cast<std::uint8_t> (std::declval<const T&>().guardCause))>> : std::true_type {};

        template <typename T, typename = void>
        struct HasGuardEngaged : std::false_type {};
        template <typename T>
        struct HasGuardEngaged<T, std::void_t<decltype (std::declval<const T&>().guardEngaged)>> : std::true_type {};

        template <typename T, typename = void>
        struct HasPlanLimited : std::false_type {};
        template <typename T>
        struct HasPlanLimited<T, std::void_t<decltype (std::declval<const T&>().planLimited)>> : std::true_type {};

        // 一位小数；负号用 U+2212 减号（规格写「−9.6 LUFS」，不是 ASCII 连字符）；0 写「0.0」。
        juce::String formatTenths (int tenths)
        {
            const auto magnitude = std::abs (tenths);
            const auto text = juce::String (magnitude / 10) + "." + juce::String (magnitude % 10);
            return tenths < 0 ? utf8 ("\xe2\x88\x92") + text : text;
        }

        int toTenths (float value) noexcept
        {
            const auto limit = (double) maxShownTenths / 10.0;
            return juce::roundToInt (juce::jlimit (-limit, limit, (double) value) * 10.0);
        }

        // 选「自动」写「自动→温润」（此刻偏向哪种），选定某种就只写它。偏向还是「自动」时只写「自动」。
        juce::String styleText (const View& view, Language language)
        {
            if (view.chosenStyle != glaze::Style::automatic)
                return styleName (view.chosenStyle, language);

            if (view.resolvedStyle == glaze::Style::automatic)
                return styleName (glaze::Style::automatic, language);

            return styleName (glaze::Style::automatic, language) + utf8 ("→") + styleName (view.resolvedStyle, language);
        }

        juce::String causeName (GuardCause cause, Language language)
        {
            const auto english = language == Language::english;

            switch (cause)
            {
                case GuardCause::budget:       return english ? juce::String ("Budget") : utf8 ("预算");
                case GuardCause::deliveryPeak: return english ? juce::String ("Delivery peak") : utf8 ("交付峰值");
                case GuardCause::contrast:     return english ? juce::String ("Contrast") : utf8 ("保对比");
                case GuardCause::psr:          return juce::String ("PSR");
                case GuardCause::none:         break;
            }

            return {};
        }

        const juce::String& dot()
        {
            static const juce::String separator = utf8 (" \xc2\xb7 ");
            return separator;
        }

        juce::String guardText (const View& view, Language language)
        {
            const auto english = language == Language::english;
            auto text = english ? juce::String ("Limited") : utf8 ("受限");

            if (view.guardCause != GuardCause::none)
                text << dot() << causeName (view.guardCause, language);

            if (view.yieldedTenthsDb > 0)
                text << dot() << (english ? "backed off " : utf8 ("已退 ")) << formatTenths (view.yieldedTenthsDb) << " dB";

            return text;
        }

        juce::String followingText (const View& view, Language language)
        {
            const auto english = language == Language::english;
            juce::String text;

            if (view.housePrefix)
                text << "House" << dot();

            text << (english ? "Following" : utf8 ("跟随")) << dot() << styleText (view, language);

            if (view.hasTarget)
                text << dot() << (english ? "Target " : utf8 ("目标 ")) << formatTenths (view.targetTenthsLufs) << " LUFS";

            return text;
        }

        bool confidenceLow (float confidence) noexcept
        {
            return ! (std::isfinite (confidence) && confidence >= confidenceReady);
        }

        // 成员访问必须依赖模板参数：if constexpr 丢掉的分支仍要能通过语法检查，具体类型 glaze::Status 上没有的字段会直接报错。
        template <typename T>
        float confidenceOf (const T& status) noexcept
        {
            if constexpr (HasConfidence<T>::value)
                return static_cast<float> (status.confidence);
            else
                return 0.0f;
        }

        template <typename T>
        float currentMainnessOf (const T& status) noexcept
        {
            if constexpr (HasCurrentMainness<T>::value)
                return static_cast<float> (status.currentMainness);
            else
                return 1.0f;
        }

        template <typename T>
        bool coldExportOf (const T& status) noexcept
        {
            if constexpr (HasColdExport<T>::value)
                return status.coldExport;
            else
                return false;
        }

        template <typename T>
        GuardCause guardCauseOf (const T& status) noexcept
        {
            if constexpr (HasGuardCause<T>::value)
            {
                const auto raw = static_cast<std::uint8_t> (status.guardCause);
                return raw <= static_cast<std::uint8_t> (GuardCause::psr) ? static_cast<GuardCause> (raw) : GuardCause::none;
            }
            else
            {
                return GuardCause::none;
            }
        }

        template <typename T>
        bool guardEngagedOf (const T& status) noexcept
        {
            if constexpr (HasGuardEngaged<T>::value)
                return status.guardEngaged;
            else if constexpr (HasPlanLimited<T>::value)
                return status.planLimited;
            else
                return false;
        }
    }

    //==============================================================================
    bool View::operator== (const View& other) const noexcept
    {
        return kind == other.kind
            && housePrefix == other.housePrefix
            && chosenStyle == other.chosenStyle
            && resolvedStyle == other.resolvedStyle
            && hasTarget == other.hasTarget
            && targetTenthsLufs == other.targetTenthsLufs
            && guardCause == other.guardCause
            && yieldedTenthsDb == other.yieldedTenthsDb
            && houseLossTenthsDb == other.houseLossTenthsDb
            && styleSegment == other.styleSegment;
    }

    //==============================================================================
    Reading readingFromStatus (const glaze::Status& status) noexcept
    {
        Reading reading;
        reading.limitedByManual = status.limitedByManual;
        reading.houseActive = status.houseActive;
        reading.housePhaseHint = status.housePhaseHint;
        reading.housePhaseMonoLossDb = status.housePhaseMonoLossDb;
        reading.protectionDb = status.protectionDb;
        reading.effectiveTargetLufs = status.effectiveTargetLufs;
        reading.resolvedStyle = status.resolvedStyle;
        reading.confidence = confidenceOf (status);
        reading.currentMainness = currentMainnessOf (status);
        reading.coldExport = coldExportOf (status);
        reading.guardCause = guardCauseOf (status);
        reading.guardEngaged = guardEngagedOf (status);
        return reading;
    }

    //==============================================================================
    View classify (const Reading& reading, float glazePercent, glaze::Style chosenStyle) noexcept
    {
        View view;

        // 「> 0」而不是「!= 0」：NaN 与负数都当作 0（与音频线程判断 GLAZE 是否起作用同一写法）。
        if (! (glazePercent > 0.0f))
        {
            view.kind = Kind::manualOnly;
            return view;
        }

        // 1. 琥珀：手动值本身已超约束。
        if (reading.limitedByManual)
        {
            view.kind = Kind::limitedByManual;
            return view;
        }

        // 2. 琥珀：House 反相提示（只亮灯，不自动收单）。压过后面所有条，压不过第 1 条。
        if (reading.houseActive && reading.housePhaseHint)
        {
            view.kind = Kind::housePhaseHint;
            view.houseLossTenthsDb = help::houseLossTenthsFor (reading.housePhaseMonoLossDb);
            return view;
        }

        // 3. 琥珀：导出时主段统计还没热。coldExport 由编排器写成「离线且 c < 0.5」。
        if (reading.coldExport)
        {
            view.kind = Kind::coldExport;
            return view;
        }

        // 4. 琥珀：守护族正在退，或已经退到底仍超。
        if (reading.guardEngaged)
        {
            view.kind = Kind::guardLimited;
            view.guardCause = reading.guardCause;
            view.yieldedTenthsDb = std::isfinite (reading.protectionDb) ? std::abs (toTenths (reading.protectionDb)) : 0;
            return view;
        }

        // 5. 梅子青：还没听到足够的主段落，方案偏保守。非有限的 c 也走这里。
        if (confidenceLow (reading.confidence))
        {
            view.kind = Kind::starting;
            return view;
        }

        // 6. 灰青：这一段较轻，净增益被棘轮按住（只降不升）。
        if (std::isfinite (reading.currentMainness) && reading.currentMainness < mainnessHold)
        {
            view.kind = Kind::holding;
            return view;
        }

        // 7. 灰青：跟着已发布画像走。House 档在前面加「House ·」。
        view.kind = Kind::following;
        view.housePrefix = reading.houseActive;
        view.chosenStyle = chosenStyle;
        view.resolvedStyle = chosenStyle == glaze::Style::automatic ? reading.resolvedStyle : glaze::Style::automatic;
        view.styleSegment = (view.housePrefix ? 1 : 0) + 1;

        if (std::isfinite (reading.effectiveTargetLufs))
        {
            view.hasTarget = true;
            view.targetTenthsLufs = toTenths (reading.effectiveTargetLufs);
        }

        return view;
    }

    //==============================================================================
    juce::String textFor (const View& view)
    {
        switch (view.kind)
        {
            case Kind::manualOnly:
                return utf8 ("GLAZE 0 · 只用手动设置");

            case Kind::limitedByManual:
                return utf8 ("受手动设置限制");

            case Kind::housePhaseHint:
                return help::housePhaseHintText (view.houseLossTenthsDb, Language::chinese);

            case Kind::coldExport:
                return utf8 ("导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出");

            case Kind::guardLimited:
                return guardText (view, Language::chinese);

            case Kind::starting:
                return utf8 ("起步 · 还没听到主段落（偏保守）");

            case Kind::holding:
                return utf8 ("保持 · 本段较轻");

            case Kind::following:
                return followingText (view, Language::chinese);
        }

        return {};
    }

    namespace
    {
        juce::String englishTextFor (const View& view)
        {
            switch (view.kind)
            {
                case Kind::manualOnly:
                    return "GLAZE 0" + dot() + "manual settings only";

                case Kind::limitedByManual:
                    return "Limited by manual settings";

                case Kind::housePhaseHint:
                    return help::housePhaseHintText (view.houseLossTenthsDb, Language::english);

                case Kind::coldExport:
                    return "Main-section stats are still cold at export, so the first drop may come out quiet: play through once, then export";

                case Kind::guardLimited:
                    return guardText (view, Language::english);

                case Kind::starting:
                    return "Starting" + dot() + "main section not heard yet (staying conservative)";

                case Kind::holding:
                    return "Holding" + dot() + "this section is quieter";

                case Kind::following:
                    return followingText (view, Language::english);
            }

            return {};
        }
    }

    juce::String textFor (const View& view, Language language)
    {
        return language == Language::english ? englishTextFor (view) : textFor (view);
    }

    juce::StringArray segmentsFor (const View& view, Language language)
    {
        juce::StringArray segments;
        const auto text = textFor (view, language);
        int start = 0;

        // 按「 · 」拆（StringArray::addTokens 会把多字节分隔符当成字符集合，这里自己找）。
        for (;;)
        {
            const auto at = text.indexOf (start, dot());

            if (at < 0)
            {
                segments.add (text.substring (start));
                break;
            }

            segments.add (text.substring (start, at));
            start = at + dot().length();
        }

        segments.removeEmptyStrings();
        return segments;
    }

    Tone toneFor (Kind kind) noexcept
    {
        switch (kind)
        {
            case Kind::starting:
                return Tone::accent;

            case Kind::limitedByManual:
            case Kind::housePhaseHint:
            case Kind::coldExport:
            case Kind::guardLimited:
                return Tone::amber;

            case Kind::manualOnly:
            case Kind::holding:
            case Kind::following:
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
