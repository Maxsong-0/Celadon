/*
  ==============================================================================

    Phase2Contract.h
    T-048 阶段 2 的测试契约（docs/validation/2026-10-05-T048-phase2/INTERFACE-SKETCH.md，Max 对 Q1–Q12 全部「可以」）。

    编排器（代理 A）、分析器（代理 B）、状态行（代理 C）还没合进这条分支。本头文件把草图里测试要钉住的纯规则写成可执行规格：
    七条状态行、速率与棘轮、G_trim、三守门取最小、旧工程读档、1 s 网格、曲风模型切换折进后续分片。
    生产类型已经有的部分（makePlan、HouseConstants 死字段、LiveConstants 软遗忘、House 反相提示的现成文案）由
    Phase2OrchestratorTests 直接调生产代码，不在这里再写一遍。

    桩（不可避免，合入后删）：
      - 状态行 Kind / classify 的生产签名仍是阶段 1。七条文案以 phase2::classify 为准；C 合入后让
        glazestatus::classify 与它逐字相同，再把 GlazeStatusTextTests 的入口换过去。
      - LIVE 外层 zlib + base64 由编排器实现。这里只核对未压缩的树形、formatVersion、constantsVersion、
        旧节点丢弃。不把一份 Base64 冒充 zlib。
      - OutputLevelTracker 的 K 加权滤波器在编排器里。这里只钉「宿主 reset 不清、冷启动才 clear、m′ < 0.5 不计能量」。

    不碰：HouseConstants 的 lock / reopen 死字段（Q12）、已退役的 clayMono* 名字（T-063，不得复用）。

  ==============================================================================
*/

#pragma once

#include "../Source/dsp/GlazeTypes.h"
#include "../Source/ui/HelpText.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace celadon::phase2
{
    // Q8：GuardCause 单列 PSR。文案用下面的中文名。
    enum class GuardCause : std::uint8_t { none = 0, budget, deliveryPeak, contrast, psr };

    struct GuardOutput
    {
        float reliefDb = 0.0f;    // ≤ 0
        GuardCause cause = GuardCause::none;
        bool engaged = false;
    };

    // 草图第 4 节的 Status：没有 phase / 校准 / 聆听进度。界面七条只读这些。
    struct Status
    {
        float effectiveTargetLufs = 0.0f;
        bool limitedByManual = false;
        glaze::Style resolvedStyle = glaze::Style::clean;
        bool houseActive = false;
        bool housePhaseHint = false;
        float housePhaseMonoLossDb = 0.0f;
        float confidence = 0.0f;          // c
        float currentMainness = 1.0f;     // 正在播的这一段的 m
        bool coldExport = false;          // 离线且 c < 0.5
        GuardCause guardCause = GuardCause::none;
        float protectionDb = 0.0f;        // ≤ 0
        bool guardEngaged = false;
    };

    enum class Kind : int
    {
        manualLimit = 1,     // 1 琥珀 受手动设置限制
        housePhaseHint,      // 2 琥珀 House 反相提示（现状文案）
        coldExport,          // 3 琥珀 导出冷启动
        guardRelief,         // 4 琥珀 受限
        starting,            // 5 梅子青 起步
        holding,             // 6 灰青 保持
        following,           // 7 灰青 跟随
        glazeOff             // GLAZE ≤ 0：只用手动设置
    };

    enum class Tone : int { dim = 0, accent, amber };

    struct View
    {
        Kind kind = Kind::glazeOff;
        glaze::Style chosenStyle = glaze::Style::automatic;
        glaze::Style resolvedStyle = glaze::Style::automatic;
        bool hasTarget = false;
        int targetTenthsLufs = 0;
        GuardCause guardCause = GuardCause::none;
        int yieldedTenthsDb = 0;
        int houseLossTenthsDb = 0;
        bool housePrefix = false;

        // 只比会写进这一行的字段。House 反相提示不带风格，跟随才带。
        bool operator== (const View& other) const noexcept
        {
            if (kind != other.kind)
                return false;

            switch (kind)
            {
                case Kind::housePhaseHint:
                    return houseLossTenthsDb == other.houseLossTenthsDb;

                case Kind::guardRelief:
                    return guardCause == other.guardCause && yieldedTenthsDb == other.yieldedTenthsDb;

                case Kind::following:
                    return chosenStyle == other.chosenStyle && resolvedStyle == other.resolvedStyle
                        && hasTarget == other.hasTarget && targetTenthsLufs == other.targetTenthsLufs
                        && housePrefix == other.housePrefix;

                case Kind::glazeOff:
                case Kind::manualLimit:
                case Kind::coldExport:
                case Kind::starting:
                case Kind::holding:
                    return true;
            }

            return false;
        }

        bool operator!= (const View& other) const noexcept   { return ! operator== (other); }
    };

    // Q1：只活在本次打开的界面里，不进 LIVE。
    inline const char* legacySessionNoticeZh = "旧的聆听结果已丢弃，声音会变";
    inline const char* legacySessionNoticeEn = "The old listen result was discarded; the sound will change.";

    inline bool glazeIsOff (float glazePercent) noexcept
    {
        return ! std::isfinite (glazePercent) || glazePercent <= 0.0f;
    }

    inline View classify (const Status& status, float glazePercent, glaze::Style chosenStyle) noexcept
    {
        View view;
        view.chosenStyle = chosenStyle;
        view.resolvedStyle = status.resolvedStyle;

        if (glazeIsOff (glazePercent))
        {
            view.kind = Kind::glazeOff;
            return view;
        }

        if (status.limitedByManual)
        {
            view.kind = Kind::manualLimit;
            return view;
        }

        if (status.houseActive && status.housePhaseHint)
        {
            view.kind = Kind::housePhaseHint;
            view.houseLossTenthsDb = celadon::help::houseLossTenthsFor (status.housePhaseMonoLossDb);
            return view;
        }

        const auto confidence = std::isfinite (status.confidence) ? status.confidence : 0.0f;
        const auto mainness = std::isfinite (status.currentMainness) ? status.currentMainness : 1.0f;

        if (status.coldExport)
        {
            view.kind = Kind::coldExport;
            return view;
        }

        if (status.guardEngaged)
        {
            view.kind = Kind::guardRelief;
            view.guardCause = status.guardCause;
            if (std::isfinite (status.protectionDb) && status.protectionDb < 0.0f)
                view.yieldedTenthsDb = juce::roundToInt (juce::jlimit (0.0, 99.9, (double) -status.protectionDb) * 10.0);
            return view;
        }

        if (confidence < 0.5f)
        {
            view.kind = Kind::starting;
            return view;
        }

        if (mainness < 0.5f)
        {
            view.kind = Kind::holding;
            return view;
        }

        view.kind = Kind::following;
        view.housePrefix = status.houseActive;
        view.resolvedStyle = chosenStyle == glaze::Style::automatic ? status.resolvedStyle : glaze::Style::automatic;

        if (std::isfinite (status.effectiveTargetLufs))
        {
            view.hasTarget = true;
            view.targetTenthsLufs = juce::roundToInt (juce::jlimit (-99.9, 0.0, (double) status.effectiveTargetLufs) * 10.0);
        }

        return view;
    }

    inline Tone toneFor (Kind kind) noexcept
    {
        switch (kind)
        {
            case Kind::manualLimit:
            case Kind::housePhaseHint:
            case Kind::coldExport:
            case Kind::guardRelief:
                return Tone::amber;

            case Kind::starting:
                return Tone::accent;

            case Kind::holding:
            case Kind::following:
            case Kind::glazeOff:
                return Tone::dim;
        }

        return Tone::dim;
    }

    inline juce::String styleName (glaze::Style style, Language language)
    {
        const auto chinese = language != Language::english;

        switch (style)
        {
            case glaze::Style::warm:    return chinese ? utf8 ("温润") : juce::String ("Warm");
            case glaze::Style::punchy:  return chinese ? utf8 ("冲击") : juce::String ("Punchy");
            case glaze::Style::clean:   return chinese ? utf8 ("通透") : juce::String ("Clean");
            case glaze::Style::automatic:
            default:                    return chinese ? utf8 ("自动") : juce::String ("Auto");
        }
    }

    inline juce::String stylePhrase (const View& view, Language language)
    {
        if (view.chosenStyle != glaze::Style::automatic)
            return styleName (view.chosenStyle, language);

        if (view.resolvedStyle == glaze::Style::automatic)
            return styleName (glaze::Style::automatic, language);

        return styleName (glaze::Style::automatic, language) + utf8 ("→") + styleName (view.resolvedStyle, language);
    }

    inline juce::String formatTenths (int tenths)
    {
        const auto negative = tenths < 0;
        const auto magnitude = std::abs (tenths);
        auto text = juce::String (magnitude / 10) + "." + juce::String (magnitude % 10);

        if (negative)
            text = juce::String::fromUTF8 ("\xe2\x88\x92") + text;

        return text;
    }

    inline juce::String guardCauseName (GuardCause cause, Language language)
    {
        const auto chinese = language != Language::english;

        switch (cause)
        {
            case GuardCause::budget:        return chinese ? utf8 ("预算") : juce::String ("budget");
            case GuardCause::deliveryPeak:  return chinese ? utf8 ("交付峰值") : juce::String ("delivery peak");
            case GuardCause::contrast:      return chinese ? utf8 ("保对比") : juce::String ("contrast");
            case GuardCause::psr:           return chinese ? utf8 ("PSR") : juce::String ("PSR");
            case GuardCause::none:          break;
        }

        return {};
    }

    inline juce::String textFor (const View& view, Language language = Language::chinese)
    {
        const auto dot = utf8 (" \xc2\xb7 ");
        const auto chinese = language != Language::english;

        switch (view.kind)
        {
            case Kind::glazeOff:
                return chinese ? utf8 ("GLAZE 0 · 只用手动设置")
                               : juce::String ("GLAZE 0") + dot + "manual settings only";

            case Kind::manualLimit:
                return chinese ? utf8 ("受手动设置限制") : juce::String ("Limited by manual settings");

            case Kind::housePhaseHint:
                return celadon::help::housePhaseHintText (view.houseLossTenthsDb, language);

            case Kind::coldExport:
                return chinese ? utf8 ("导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出")
                               : juce::String ("Export started before the main-section stats warmed up; the first drop may be light. Play it through once, then export.");

            case Kind::guardRelief:
            {
                auto text = chinese ? utf8 ("受限") : juce::String ("Limited");
                const auto cause = guardCauseName (view.guardCause, language);

                if (cause.isNotEmpty())
                    text << dot << cause;

                if (view.yieldedTenthsDb > 0)
                    text << dot << (chinese ? utf8 ("已退 ") : juce::String ("backed off "))
                         << formatTenths (view.yieldedTenthsDb) << " dB";

                return text;
            }

            case Kind::starting:
                return chinese ? utf8 ("起步 · 还没听到主段落（偏保守）")
                               : juce::String ("Starting") + dot + "main section not heard yet (staying conservative)";

            case Kind::holding:
                return chinese ? utf8 ("保持 · 本段较轻")
                               : juce::String ("Holding") + dot + "this section is quieter";

            case Kind::following:
            {
                auto text = view.housePrefix ? juce::String ("House") + dot : juce::String();
                text << (chinese ? utf8 ("跟随") : juce::String ("Following")) << dot << stylePhrase (view, language);

                if (view.hasTarget)
                    text << dot << (chinese ? utf8 ("目标 ") : juce::String ("target "))
                         << formatTenths (view.targetTenthsLufs) << " LUFS";

                return text;
            }
        }

        return {};
    }

    inline juce::StringArray segmentsFor (const View& view, Language language = Language::chinese)
    {
        juce::StringArray segments;
        const auto text = textFor (view, language);
        const auto separator = utf8 (" \xc2\xb7 ");
        int start = 0;

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

    // --------------------------------------------------------------------------
    // Q8 出厂速率。死区：|误差| ≤ 0.1 dB 进入并停住，|误差| ≥ 0.3 dB 离开并继续走，中间保持上一步。
    inline constexpr float gainUpDbPerSecond = 0.2f;
    inline constexpr float gainDownDbPerSecond = 1.0f;
    inline constexpr float deadbandExitDb = 0.3f;
    inline constexpr float deadbandEnterDb = 0.1f;
    inline constexpr float trimMaxDbPerSecond = 0.05f;
    inline constexpr float trimClampDb = 3.0f;

    struct GainServo
    {
        float appliedDb = 0.0f;
        bool inDeadband = false;

        // Q7：mainness < 0.5 时净增益只许下降。Glue / Heat / Clip / Tone 不走这个函数。
        void stepGain (float targetDb, float dtSeconds, float mainness) noexcept
        {
            const auto goal = mainness < 0.5f ? std::min (targetDb, appliedDb) : targetDb;
            stepToward (goal, dtSeconds, gainUpDbPerSecond, gainDownDbPerSecond);
        }

        void stepToward (float targetDb, float dtSeconds, float upDbPerSecond, float downDbPerSecond) noexcept
        {
            if (! (dtSeconds > 0.0f) || ! std::isfinite (targetDb))
                return;

            const auto error = targetDb - appliedDb;

            if (inDeadband)
            {
                if (std::abs (error) >= deadbandExitDb)
                    inDeadband = false;
            }
            else if (std::abs (error) <= deadbandEnterDb)
            {
                inDeadband = true;
            }

            if (inDeadband)
                return;

            const auto rate = error >= 0.0f ? upDbPerSecond : downDbPerSecond;
            const auto step = juce::jlimit (-rate * dtSeconds, rate * dtSeconds, error);
            appliedDb += step;
        }
    };

    struct TrimIntegrator
    {
        float trimDb = 0.0f;

        // errorDb = 目标 − 出口主段电平。预算环正在退让时冻结（抗饱和）。
        void step (float errorDb, float dtSeconds, bool budgetRetreating) noexcept
        {
            if (budgetRetreating || ! (dtSeconds > 0.0f) || ! std::isfinite (errorDb))
                return;

            const auto maxStep = trimMaxDbPerSecond * dtSeconds;
            trimDb = juce::jlimit (-trimClampDb, trimClampDb, trimDb + juce::jlimit (-maxStep, maxStep, errorDb));
        }
    };

    // 状态行用的那一对：只看 engaged 的门。更负的退让胜出；平局留下先到的原因（预算、对比、PSR）；退让为 0 则原因 none。
    // 没在起作用的更深退让（例如正在回升）不改这一行。音频上三扇取最小是另一条路径。
    inline GuardOutput minGuard (std::initializer_list<GuardOutput> guards) noexcept
    {
        GuardOutput best;

        for (const auto& guard : guards)
        {
            if (! guard.engaged)
                continue;

            const auto relief = std::isfinite (guard.reliefDb) ? std::min (guard.reliefDb, 0.0f) : 0.0f;

            if (! best.engaged || relief < best.reliefDb)
            {
                best.reliefDb = relief;
                best.cause = relief < 0.0f ? guard.cause : GuardCause::none;
            }

            best.engaged = true;
        }

        return best;
    }

    // Q9：宿主 reset 不清。m′ < 0.5 的样本不进主段能量。能量是均方的占位，不是 K 加权。
    struct OutputLevelTracker
    {
        void prepare (double sampleRateIn) noexcept
        {
            sampleRate = sampleRateIn > 0.0 ? sampleRateIn : 48000.0;
        }

        void clear() noexcept
        {
            energy = 0.0;
            samples = 0;
            mainness = 0.0f;
        }

        void hostReset() noexcept {}

        void noteMainness (float m) noexcept
        {
            mainness = std::isfinite (m) ? m : 0.0f;
        }

        void processOutput (const float* samplesIn, int numSamples) noexcept
        {
            if (samplesIn == nullptr || numSamples <= 0 || mainness < 0.5f)
                return;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto x = samplesIn[i];

                if (std::isfinite (x))
                    energy += (double) x * (double) x;
            }

            samples += numSamples;
        }

        bool hasLevel() const noexcept   { return samples > 0; }

        float mainLevelLufs() const noexcept
        {
            if (samples <= 0 || ! (sampleRate > 0.0))
                return -100.0f;

            const auto meanSquare = energy / (double) samples;
            return meanSquare > 0.0 ? (float) (-0.691 + 10.0 * std::log10 (meanSquare)) : -100.0f;
        }

        double sampleRate = 48000.0;
        double energy = 0.0;
        int samples = 0;
        float mainness = 0.0f;
    };

    // Q3 / 草图第 2 节：画像只在 1 s 格上改决策器输入，与宿主块长无关。
    inline std::int64_t samplesPerGrid (double sampleRate) noexcept
    {
        return std::max<std::int64_t> (1, std::llround (sampleRate));
    }

    inline bool crossedOneSecondGrid (std::int64_t previousSample, std::int64_t sample, double sampleRate) noexcept
    {
        const auto grid = samplesPerGrid (sampleRate);
        return sample / grid != previousSample / grid;
    }

    // Q4：音频线程只记下新模型，已发布画像继续用旧的，切换折进后续 advanceTo 分片。
    struct ModelSwitch
    {
        int published = 0;
        int pending = 0;

        void request (int model) noexcept
        {
            pending = model;
        }

        void advanceSlice() noexcept
        {
            published = pending;
        }

        bool waiting() const noexcept   { return published != pending; }
    };

    // --------------------------------------------------------------------------
    // 第 5 节工程树。旧节点读到就丢。GLAZE_CONSTANTS 不参与 makePlan。
    inline bool isDiscardedListenTag (juce::StringRef tag) noexcept
    {
        return tag == juce::StringRef ("GLAZE_PROFILE")
            || tag == juce::StringRef ("PLAN")
            || tag == juce::StringRef ("CALIBRATION")
            || tag == juce::StringRef ("EXECUTION");
    }

    struct OpenResult
    {
        bool coldStart = true;
        bool sessionNotice = false;     // Q1：不写入下面的树
        int constantsVersion = 0;       // Q11：诊断用，不参与计算
        bool planUsesFactoryConstants = true;
    };

    inline OpenResult interpretState (const juce::XmlElement& xml)
    {
        OpenResult result;
        auto droppedListen = false;

        for (auto* child = xml.getFirstChildElement(); child != nullptr; child = child->getNextElement())
            droppedListen = droppedListen || isDiscardedListenTag (child->getTagName());

        const auto version = xml.getIntAttribute ("formatVersion", 0);
        const auto* live = xml.getChildByName ("LIVE");

        if (! xml.hasTagName ("GLAZE_STATE") || version != 2 || live == nullptr)
        {
            result.coldStart = true;
            result.sessionNotice = version < 2 || droppedListen;
            return result;
        }

        result.coldStart = false;
        result.sessionNotice = false;
        result.constantsVersion = live->getIntAttribute ("constantsVersion", 0);
        result.planUsesFactoryConstants = true;
        return result;
    }

    inline std::unique_ptr<juce::XmlElement> saveLiveTree (int constantsVersion)
    {
        auto state = std::make_unique<juce::XmlElement> ("GLAZE_STATE");
        state->setAttribute ("formatVersion", 2);
        auto* live = state->createNewChildElement ("LIVE");
        live->setAttribute ("formatVersion", 1);
        live->setAttribute ("compressed", 1);
        live->setAttribute ("constantsVersion", constantsVersion);
        live->createNewChildElement ("LIVE_PROFILE");
        live->createNewChildElement ("CONTROL");
        live->createNewChildElement ("BUDGET_GUARD");
        live->createNewChildElement ("CONTRAST_GUARD");
        live->createNewChildElement ("PSR_GUARD");
        live->createNewChildElement ("OUTPUT_LEVEL");
        live->createNewChildElement ("CLAY");
        return state;
    }

    inline bool treeWritesLegacyNotice (const juce::XmlElement& xml)
    {
        return xml.toString().contains (juce::String::fromUTF8 (legacySessionNoticeZh))
            || xml.getChildByName ("LEGACY_NOTICE") != nullptr
            || xml.getChildByName ("GLAZE_CONSTANTS") != nullptr
            || xml.getChildByName ("GLAZE_PROFILE") != nullptr
            || xml.getChildByName ("CALIBRATION") != nullptr;
    }
}

// ------------------------------------------------------------------------------
// T-097（规格「设计规格-实时GLAZE-主段与增益」第 4 节；Max 2026-10-10 Q11「有退让量就冻结」）：G_trim 抗积分饱和的新口径。
//
// 上面 TrimIntegrator::step 的口径是 T-048：只看一个布尔「预算环本块在退」。那会让落差 / PSR 守门退下去之后，
// G_trim 把这段差当偏差、按 0.05 dB/s 约 60 s 推满 +3 dB 抵消掉退让。新口径把「该不该积分」单独成一个纯函数：
// 任一条成立就不积分（G_trim 保持原值，不清零）——
//   1. 预算环本块在退（isRetreating）；
//   2. 预算 / 落差 / PSR 任一退让量 < 0（在退、停住、回程中都算，只要还没回到 0）；
//   3. 这一块刚改过某个退让量（回到 0 的那一步也算：before 与 after 不同）；
//   4. 距上次改参数不满 1 s（风格 / 曲风 / 交付 / GLAZE / 规划相关手动值，对应 CONTROL 的 secondsSinceParamChange）。
// 与 Source/dsp/GlazeController.cpp 的 guardHolding / recentlyChanged 逐条对应；旧函数不动，积分本身直接复用旧 step。
namespace celadon::phase2
{
    inline constexpr double trimParamChangeFreezeSeconds = 1.0;

    struct GuardReliefStep
    {
        float beforeDb = 0.0f;    // 这一块开始前的退让量（≤ 0）
        float afterDb = 0.0f;     // 这一块结束后的退让量（≤ 0）
    };

    struct TrimFreezeInputs
    {
        bool budgetRetreatingThisBlock = false;
        GuardReliefStep budget;
        GuardReliefStep contrast;
        GuardReliefStep psr;
        double secondsSinceParamChange = trimParamChangeFreezeSeconds;   // 默认「很久以前改的」，不冻结
    };

    // 按位比较（对应生产的 sameBits）：+0 与 −0 算不同，NaN 算不同——都偏向「冻结」，是安全的一侧。
    inline bool reliefChanged (const GuardReliefStep& step) noexcept
    {
        return step.beforeDb != step.afterDb || std::signbit (step.beforeDb) != std::signbit (step.afterDb);
    }

    inline bool trimFrozenByGuards (const TrimFreezeInputs& inputs) noexcept
    {
        const auto holding = inputs.budgetRetreatingThisBlock
                          || inputs.budget.afterDb < 0.0f || inputs.contrast.afterDb < 0.0f || inputs.psr.afterDb < 0.0f
                          || reliefChanged (inputs.budget) || reliefChanged (inputs.contrast) || reliefChanged (inputs.psr);
        const auto recentlyChanged = inputs.secondsSinceParamChange < trimParamChangeFreezeSeconds;
        return holding || recentlyChanged;
    }

    // 复用 TrimIntegrator::step：它的第三个参数在旧口径里叫 budgetRetreating，这里传「按新口径是否冻结」。
    inline void stepTrimUnderGuards (TrimIntegrator& trim, float errorDb, float dtSeconds, const TrimFreezeInputs& inputs) noexcept
    {
        trim.step (errorDb, dtSeconds, trimFrozenByGuards (inputs));
    }
}
