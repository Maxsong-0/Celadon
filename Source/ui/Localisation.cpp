/*
  ==============================================================================

    Localisation.cpp
    界面固定文字的中英对照表。措辞以 V4 界面设计稿为准；数值与单位由调用方拼接（数字格式两种语言相同）。

    只依赖 juce_core：这个文件同时进插件与无头测试（celadon_tests）编译。

  ==============================================================================
*/

#include "Localisation.h"

#include <array>
#include <cmath>

namespace celadon
{
    const char* languageCode (Language language) noexcept
    {
        return language == Language::english ? "en" : "zh";
    }

    Language languageFromCode (juce::StringRef code) noexcept
    {
        return juce::String (code).trim().equalsIgnoreCase ("en") ? Language::english : Language::chinese;
    }

    Language defaultLanguageForSystem()
    {
        // 按系统的界面显示语言（「zh-Hans-CN」「zh-CN」「en-US」，只看前两个字母）。不用 getUserLanguage：Windows 上它是
        // 「区域格式」的语言——中文界面配英文格式时会误判成英文。显示语言取不到时再看它。
        auto system = juce::SystemStats::getDisplayLanguage().toLowerCase();

        if (system.isEmpty())
            system = juce::SystemStats::getUserLanguage().toLowerCase();

        return system.startsWith ("zh") ? Language::chinese : Language::english;
    }
}

namespace celadon::ui
{
    namespace
    {
        // 一行 = 键 + 中文 + English。{0}、{1} 是 trFormat 的占位符。
        constexpr TextEntry table[] =
        {
            // ---- 顶栏 ----
            { Text::brandSubtitle,      "母带处理",                 "MASTERING" },
            { Text::latency,            "延迟",                     "LAT" },
            { Text::settings,           "设置",                     "Settings" },
            { Text::help,               "帮助",                     "Help" },
            { Text::close,              "关闭",                     "Close" },
            { Text::version,            "版本",                     "Version" },
            { Text::advanced,           "高级",                     "Advanced" },

            // ---- 电平表 ----
            { Text::meterIn,            "输入",                     "IN" },
            { Text::meterOut,           "输出",                     "OUT" },
            { Text::meterGr,            "衰减",                     "GR" },

            // ---- 频谱仪 ----
            { Text::showOutput,         "输出",                     "OUT" },
            { Text::showInput,          "输入",                     "IN" },
            { Text::showPeak,           "峰值",                     "PEAK" },
            { Text::slope,              "斜率",                     "Slope" },
            { Text::cents,              "音分",                     "Cents" },
            { Text::markLowCut,         "低切",                     "Low cut" },
            { Text::markMono,           "收单",                     "Mono" },

            // ---- 响度 ----
            { Text::loudness,           "响度",                     "LOUDNESS" },
            { Text::truePeakShort,      "真峰值",                   "TP" },
            { Text::shortTermShort,     "短期",                     "S" },
            { Text::peakToLoudness,     "峰响比",                   "PLR" },
            { Text::normalisation,      "归一",                     "Norm" },
            { Text::streamingReference, "流媒体 \xe2\x88\x92" "14", "Stream \xe2\x88\x92" "14" },
            { Text::resetMeters,        "重新计量",                 "Reset meters" },

            // ---- GLAZE 中层 ----
            { Text::glazeSubtitle,      "智能母带",                 "Smart mastering" },
            { Text::relearn,            "重新聆听",                 "Relearn" },
            { Text::relearnCancel,      "取消聆听",                 "Cancel" },
            { Text::processed,          "处理后",                   "Processed" },
            { Text::original,           "原声",                     "Original" },
            { Text::match,              "等响",                     "Match" },
            { Text::matchWithDifference, "等响 {0} dB",             "Match {0} dB" },
            { Text::matchOff,           "等响 关",                  "Match off" },
            { Text::listeningOriginal,  "正在听原声",               "Listening to original" },
            { Text::matchedBy,          "已等响 {0} dB",            "level-matched {0} dB" },
            { Text::notMatched,         "未等响（响度差 {0} dB）",  "not matched ({0} dB apart)" },
            { Text::differenceUnknown,  "播放几秒后测出响度差",     "play a few seconds to measure" },
            { Text::drivesParameters,   "联动 7 项参数",            "Drives 7 parameters" },
            { Text::glazeManual,        "GLAZE 为 0：全部手动",     "GLAZE at 0: fully manual" },
            { Text::cornerUnlearned,    "播放后自动开始聆听",       "Starts listening on playback" },
            { Text::cornerListening,    "正在聆听 {0} / {1} 秒",    "Listening {0} / {1} s" },
            { Text::cornerListened,     "已聆听 {0} 秒",            "Listened {0} s" },
            { Text::hintDrag,           "拖动调节 · \xe2\x8c\x98/Ctrl 精调", "Drag \xc2\xb7 \xe2\x8c\x98/Ctrl for fine" },
            { Text::hintClick,          "双击复位 · Alt 点击输入",  "Double-click reset \xc2\xb7 Alt-click to type" },
            { Text::fine,               "精调",                     "Fine" },
            { Text::legendManual,       "手动设置",                 "Manual" },
            { Text::legendAuto,         "GLAZE 自动调整",           "GLAZE auto" },
            { Text::legendAttention,    "需要注意",                 "Attention" },
            { Text::chipLocked,         "已锁定",                   "Locked" },
            { Text::chipListening,      "聆听中",                   "Listening" },
            { Text::chipCalibrating,    "校准中",                   "Calibrating" },
            { Text::chipUnlearned,      "未学习",                   "Not learned" },
            { Text::chipPaused,         "已暂停",                   "Paused" },
            { Text::chipLimited,        "受限",                     "Limited" },

            // ---- 模块卡片 ----
            { Text::describeClay,       "声像",                     "Stereo" },
            { Text::describeBalance,    "动态均衡",                 "Dynamic EQ" },
            { Text::describeColor,      "音色",                     "Tone" },
            { Text::describeLimit,      "限幅",                     "Limiter" },
            { Text::bypassModule,       "旁通",                     "Bypass" },
            { Text::bypassedModule,     "已旁通",                   "Bypassed" },
            { Text::clayLowCut,         "低切",                     "Low cut" },
            { Text::clayMono,           "收单",                     "Mono" },
            { Text::clayWidth,          "加宽",                     "Width" },
            { Text::modeAuto,           "自动",                     "Auto" },
            { Text::modeOff,            "关",                       "Off" },
            { Text::modeManual,         "手动",                     "Manual" },
            { Text::stripStereo,        "立体声",                   "STEREO" },
            { Text::clayNotActive,      "未启用",                   "idle" },
            { Text::clayBypassed,       "已旁通",                   "bypassed" },
            { Text::recover,            "回补",                     "Recover" },
            { Text::tame,               "抑制",                     "Tame" },
            { Text::brighten,           "提亮",                     "Brighten" },
            { Text::speed,              "速度",                     "SPEED" },
            { Text::speedSlow,          "慢",                       "Slow" },
            { Text::speedMid,           "中",                       "Mid" },
            { Text::speedFast,          "快",                       "Fast" },
            { Text::bands27,            "27 段",                    "27 bands" },
            { Text::tone,               "音调",                     "Tone" },
            { Text::glue,               "粘合",                     "Glue" },
            { Text::heat,               "热度",                     "Heat" },
            { Text::mix,                "干湿",                     "MIX" },
            { Text::output,             "输出",                     "OUTPUT" },
            { Text::glueReduction,      "粘合衰减",                 "GLUE GR" },
            { Text::gain,               "增益",                     "Gain" },
            { Text::ceiling,            "上限",                     "Ceiling" },
            { Text::clip,               "削波",                     "Clip" },
            { Text::release,            "释放",                     "RELEASE" },
            { Text::lookahead,          "预读",                     "LOOKAHEAD" },
            { Text::limitReduction,     "限幅衰减",                 "LIMIT GR" },
            { Text::autoChip,           "自动",                     "AUTO" },
            { Text::auditioningDifference,  "正在试听差值",         "Listening to difference" },
            { Text::auditionDifferenceNote, "只听被削掉的部分",     "only what is removed" },

            // ---- 高级面板 ----
            { Text::advancedClay,       "修坯 CLAY",                "CLAY" },
            { Text::advancedClayNote,   "低切、收单、加宽各自可选「自动 / 关 / 手动」，\n三态与手动值都在下方 CLAY 卡片上。",
                                        "Auto / Off / Manual and the manual values for low cut, mono and width are on the CLAY card below." },
            { Text::comingSoon,         "即将推出",                 "Coming soon" },
            { Text::subprojectLoudNotHarsh,   "推响不刺",           "Loud, not harsh" },
            { Text::subprojectTargetSpectrum, "目标频谱",           "Target spectrum" },
            { Text::subprojectReleaseSafety,  "发行安全",           "Release safety" },

            // ---- 设置面板 ----
            { Text::windowSize,         "界面大小",                 "Window size" },
            { Text::screenSize,         "{0} × {1} 屏幕",           "{0} \xc3\x97 {1} screen" },
            { Text::tooBig,             "超出屏幕",                 "Larger than screen" },
            { Text::sizeNote,           "也可以拖动窗口右下角自由缩放，比例保持不变；下次打开时沿用这个大小。",
                                        "You can also drag the bottom-right corner; the aspect ratio is kept and the size is remembered." },
            { Text::reduceMotion,       "减少动效",                 "Reduce motion" },
            { Text::reduceMotionNote,   "关闭粒子、波纹和裂纹生长动画", "Turns off particles, ripples and crack growth" },
            { Text::language,           "语言",                     "Language" },

            // ---- 帮助面板 ----
            { Text::helpTitle,          "操作提示",                 "Quick tips" },
            { Text::helpDrag,           "上下拖动旋钮、左右拖动滑条调节数值",
                                        "Drag knobs up and down, sliders left and right" },
            { Text::helpFine,           "按住 \xe2\x8c\x98 / Ctrl 拖动或滚动：精调（慢 8 倍）",
                                        "Hold \xe2\x8c\x98 / Ctrl while dragging or scrolling for fine steps (8\xc3\x97 slower)" },
            { Text::helpReset,          "双击：回到默认值",         "Double-click: back to the default" },
            { Text::helpType,           "按住 Alt / Option 单击：直接输入数值，回车确认、Esc 取消",
                                        "Alt / Option-click: type a value, Return to confirm, Esc to cancel" },
            { Text::helpWheel,          "滚轮：微调",               "Mouse wheel: small steps" },
            { Text::helpColours,        "青色 = 你的设置；紫色 = GLAZE 此刻的自动加量；橙色 = 需要注意",
                                        "Cyan = your setting; violet = GLAZE's current automatic amount; orange = needs attention" },
            { Text::helpAb,             "A / B：在处理后与原声之间切换；打开「等响」后两边按实测响度对齐，只衰减、不抬高",
                                        "A / B switches between processed and original; with Match on both play at the measured loudness, attenuation only" },
            { Text::helpAudition,       "LIMIT 卡片的耳机：试听差值，只听限幅削掉的部分；再点一次回到正常输出",
                                        "Headphones on the LIMIT card: listen to the difference, only what the limiter removes; click again to go back" },

            // ---- 警告 ----
            { Text::warnTruePeak,       "真峰值 {0} dBTP · 超出上限 {1}", "True peak {0} dBTP \xc2\xb7 above ceiling {1}" },
            { Text::warnNonFinite,      "已屏蔽 {0} 个异常音频块（NaN/Inf）", "Blocked {0} invalid audio blocks (NaN/Inf)" },
            { Text::debugBuild,         "Debug 构建：CPU 远高于 Release，只供调试",
                                        "Debug build: far higher CPU than Release, for debugging only" },
        };

        static_assert (std::size (table) == (size_t) Text::numTexts, "Localisation 表的行数与 Text 的键数不一致：两边一起改");

        // 按键查行：键的值就是下标时直接取，否则线性找（表的顺序与枚举顺序一致，正常总是直接命中）。
        const TextEntry* find (Text key) noexcept
        {
            const auto index = (size_t) key;

            if (index < std::size (table) && table[index].key == key)
                return &table[index];

            for (const auto& entry : table)
                if (entry.key == key)
                    return &entry;

            return nullptr;
        }
    }

    juce::String tr (Text key, Language language)
    {
        const auto* entry = find (key);

        if (entry == nullptr)
            return {};

        return utf8 (language == Language::english ? entry->english : entry->chinese);
    }

    juce::String trFormat (Text key, Language language, std::initializer_list<juce::String> arguments)
    {
        auto text = tr (key, language);
        int index = 0;

        for (const auto& argument : arguments)
            text = text.replace ("{" + juce::String (index++) + "}", argument);

        return text;
    }

    juce::String normaliseTypedNumber (const juce::String& text)
    {
        juce::String result;
        result.preallocateBytes (text.getNumBytesAsUTF8());

        for (auto p = text.getCharPointer(); ! p.isEmpty();)
        {
            auto c = p.getAndAdvance();

            if (c >= 0xff10 && c <= 0xff19)                                   c = (juce::juce_wchar) ('0' + (c - 0xff10));   // ０ ～ ９
            else if (c == 0x3002 || c == 0xff0e || c == 0xff61)                c = '.';   // 。 ． ｡
            else if (c == 0x2212 || c == 0xff0d || c == 0x2013 || c == 0x2014
                      || c == 0xfe63 || c == 0x2010 || c == 0x2011)           c = '-';   // − － – — ﹣ ‐ ‑
            else if (c == 0xff0b || c == 0xfe62)                               c = '+';   // ＋ ﹢
            else if (c == 0xff0c)                                              c = ',';   // ，

            result += juce::String::charToString (c);
        }

        return result;
    }

    std::optional<double> parseTypedNumber (const juce::String& text)
    {
        const auto cleaned = normaliseTypedNumber (text).retainCharacters ("0123456789.,+-");

        if (! cleaned.containsAnyOf ("0123456789") || cleaned.substring (1).containsAnyOf ("+-"))
            return std::nullopt;

        const auto sign = cleaned.startsWithChar ('-') || cleaned.startsWithChar ('+') ? cleaned.substring (0, 1) : juce::String();
        auto number = cleaned.substring (sign.length());

        if (number.containsChar (','))
        {
            if (number.containsChar ('.'))
                return std::nullopt;

            juce::StringArray groups;
            groups.addTokens (number, ",", {});

            auto thousands = groups[0].length() >= 1 && groups[0].length() <= 3 && ! groups[0].startsWithChar ('0');

            for (int i = 1; i < groups.size(); ++i)
                thousands = thousands && groups[i].length() == 3;

            if (thousands)
                number = groups.joinIntoString ({});
            else if (groups.size() == 2)
                number = groups[0] + "." + groups[1];
            else
                return std::nullopt;
        }
        else if (number.indexOfChar ('.') != number.lastIndexOfChar ('.'))
        {
            return std::nullopt;
        }

        const auto value = (sign + number).getDoubleValue();

        if (! std::isfinite (value))
            return std::nullopt;

        return value;
    }

    const TextEntry* textEntries (int& count) noexcept
    {
        count = (int) std::size (table);
        return table;
    }
}
