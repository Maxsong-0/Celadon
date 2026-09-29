/*
  ==============================================================================

    UiSettings.h
    跟着用户走、不跟着工程走的界面设置（v0.1.8.0）：语言、减少动效、新窗口的默认大小。

      - 存在用户目录的 Celadon.settings（macOS：~/Library/Application Support/Celadon/；Windows：%APPDATA%\Celadon\）；
      - 同一进程里所有插件实例共用一份（juce::SharedResourcePointer）：在一个窗口里切语言，别的窗口同时换；
      - 改动在消息线程发生，立刻写盘，并以 ChangeBroadcaster 通知各编辑器（异步，在下一次消息循环里）。

    窗口大小另外随工程存一份（APVTS 状态树的 uiZoom 属性）：打开旧工程时回到当时的大小；新实例用这里的默认值。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Localisation.h"

namespace celadon::ui
{
    class UiSettings  : public juce::ChangeBroadcaster
    {
    public:
        UiSettings();
        ~UiSettings() override;

        Language getLanguage() const noexcept               { return language; }
        void setLanguage (Language newLanguage);

        bool getReduceMotion() const noexcept               { return reduceMotion; }
        void setReduceMotion (bool shouldReduceMotion);

        // 新窗口的默认缩放（0.7 ～ 2.0，1 = 1200 × 820）。
        double getDefaultZoom() const noexcept              { return defaultZoom; }
        void setDefaultZoom (double zoom);

        static constexpr double minZoom = 0.7;
        static constexpr double maxZoom = 2.0;

    private:
        void save();

        std::unique_ptr<juce::PropertiesFile> file;
        Language language = Language::chinese;
        bool reduceMotion = false;
        double defaultZoom = 1.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UiSettings)
    };
}
