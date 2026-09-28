/*
  ==============================================================================

    Popovers.h
    浮在画布上的几样东西：设置面板（界面大小、减少动效）、帮助面板（操作提示）、拖动时的数值气泡、缩放窗口时的大小提示。

    设置面板（V4 设计稿）：界面大小 70 % ～ 200 %（五个常用档位 + 滑条 + 按屏幕比例画的预览，超出屏幕时提醒），
    也可以拖窗口右下角自由缩放（宽高比不变）；「减少动效」关掉粒子、波纹与裂纹淡入。两项都记在用户设置里。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"

namespace celadon::ui
{
    //==============================================================================
    // 带大阴影的浮层卡片：组件比卡片大一圈（留给阴影），只有卡片本身接鼠标。
    class PopoverCard  : public juce::Component
    {
    public:
        static constexpr int shadowMargin = 28;

        juce::Rectangle<float> getCardBounds() const;
        bool hitTest (int x, int y) override;
        void paint (juce::Graphics&) override;

        std::function<void()> onClose;

    protected:
        juce::Rectangle<float> closeButtonBounds() const;
        void paintCloseButton (juce::Graphics&) const;
        bool isOnCloseButton (juce::Point<float> position) const;
    };

    //==============================================================================
    class SettingsPanel  : public PopoverCard
    {
    public:
        SettingsPanel();

        static constexpr int cardWidth = 324;
        static constexpr int cardHeight = 426;

        void setLanguage (Language newLanguage);
        void setZoom (double newZoom);
        void setReduceMotion (bool shouldReduceMotion);
        void setLatency (double milliseconds);
        void setScreenSize (juce::Rectangle<int> screenArea);

        std::function<void (double)> onZoomChange;
        std::function<void (bool)> onZoomDragging;      // 拖滑条的开始 / 结束（编辑器据此显示大小提示）
        std::function<void (bool)> onReduceMotionChange;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        static constexpr double presets[] = { 0.8, 1.0, 1.25, 1.5, 2.0 };

    private:
        juce::Rectangle<float> presetBounds (int index) const;
        juce::Rectangle<float> sliderBounds() const;
        juce::Rectangle<float> switchBounds() const;
        double zoomAt (float x) const;

        Language language = Language::chinese;
        double zoom = 1.0;
        bool reduceMotion = false;
        double latencyMs = 0.0;
        juce::Rectangle<int> screen { 0, 0, 2560, 1440 };
        bool draggingSlider = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
    };

    //==============================================================================
    class HelpPanel  : public PopoverCard
    {
    public:
        HelpPanel();

        static constexpr int cardWidth = 340;

        void setLanguage (Language newLanguage);
        int getCardHeight() const noexcept   { return cardHeight; }

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        void layoutText();

        Language language = Language::chinese;
        juce::TextLayout layout;
        int cardHeight = 300;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpPanel)
    };

    //==============================================================================
    // 拖动时控件上方的深色数值气泡（精调时带「精调」小标签）。
    class ValueBubble  : public juce::Component
    {
    public:
        ValueBubble();

        void show (const juce::String& text, bool fine, const juce::String& fineLabel);
        int getPreferredWidth() const;

        void paint (juce::Graphics&) override;

        static constexpr int height = 24;

    private:
        juce::String text, fineLabel;
        bool fine = false;
    };

    //==============================================================================
    // 缩放窗口时画布正中的大小提示（「界面大小 125 % 1500 × 1025 px」）。
    class ResizeHud  : public juce::Component
    {
    public:
        ResizeHud();

        void setValues (const juce::String& title, double zoom);
        void paint (juce::Graphics&) override;

    private:
        juce::String title;
        double zoom = 1.0;
    };
}
