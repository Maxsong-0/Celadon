/*
  ==============================================================================

    UiSettings.h
    跟着用户走、不跟着工程走的界面设置（v0.1.8.0）：语言、减少动效、新窗口的默认大小。

      - 存在用户目录的 Celadon.settings（macOS：~/Library/Application Support/Celadon/；Windows：%APPDATA%\Celadon\）；
      - 同一进程里所有插件实例共用一份（juce::SharedResourcePointer）：在一个窗口里切语言，别的窗口同时换；
      - 改动在消息线程发生，立刻写盘，并以 ChangeBroadcaster 通知各编辑器（异步，在下一次消息循环里）。

    窗口大小另外随工程存一份（APVTS 状态树的 uiZoom 属性）：打开旧工程时回到当时的大小；新实例用这里的默认值。
    这里也没存过（新装）时用 firstRunZoom（125 %），编辑器再按屏幕可用区收小（T-112 / B4，Max 2026-11-12 定）。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Localisation.h"

#include <optional>

namespace celadon::ui
{
    // 新装（用户设置里从没存过默认大小）时的窗口大小。屏幕放不下时编辑器取屏幕放得下的最大值（两者取小，fitZoomToScreen）。
    // 用户已经存过的默认大小、工程里存过的大小都不动。
    inline constexpr double firstRunZoom = 1.25;

    // 打开窗口时用哪个大小（还没按屏幕收小）：工程里存过的 > 用户设置里存过的默认 > 新装默认 firstRunZoom。
    // 工程里的值非有限时按 1（与 T-112 之前相同）；结果钳到 UiSettings::minZoom ～ maxZoom。纯函数，截图工具的检查直接调。
    double initialZoom (std::optional<double> projectZoom, std::optional<double> storedDefaultZoom) noexcept;

    class UiSettings  : public juce::ChangeBroadcaster
    {
    public:
        UiSettings();
        ~UiSettings() override;

        Language getLanguage() const noexcept               { return language; }
        void setLanguage (Language newLanguage);

        bool getReduceMotion() const noexcept               { return reduceMotion; }
        void setReduceMotion (bool shouldReduceMotion);

        // 新窗口的默认缩放（0.7 ～ 2.0，1 = 1200 × 820）。没存过（新装）时 getStoredDefaultZoom 是空的，
        // getDefaultZoom 给 firstRunZoom。
        double getDefaultZoom() const noexcept              { return defaultZoom.value_or (firstRunZoom); }
        std::optional<double> getStoredDefaultZoom() const noexcept   { return defaultZoom; }
        void setDefaultZoom (double zoom);

        static constexpr double minZoom = 0.7;
        static constexpr double maxZoom = 2.0;

    private:
        void save();

        std::unique_ptr<juce::PropertiesFile> file;
        Language language = Language::chinese;
        bool reduceMotion = false;
        std::optional<double> defaultZoom;   // 设置文件里没有这一项 = 新装

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UiSettings)
    };
}
