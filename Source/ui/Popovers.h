/*
  ==============================================================================

    Popovers.h
    浮在画布上的几样东西：设置面板（界面大小、减少动效）、帮助面板（操作提示）、「高级」面板（子项目）、拖动时的数值气泡、
    缩放窗口时的大小提示。

    设置面板（V4 设计稿）：界面大小 70 % ～ 200 %（五个常用档位 + 滑条 + 按屏幕比例画的预览，超出屏幕时提醒），
    也可以拖窗口右下角自由缩放（宽高比不变）；「减少动效」关掉粒子、波纹与裂纹淡入，旋钮也不拖彗尾、不回弹（T-100）。两项都记在用户设置里。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "MiddleLayer.h"
#include "../dsp/MasteringFeatures.h"

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

        // 「减少动效」下面那行说明：单行画到开关左边，放不下会被截成省略号。界面缩放是整体变换，各档位逻辑宽度相同；
        // 截图工具的检查按这两个量中英两句（T-102）。
        static juce::Font reduceMotionNoteFont();
        static float reduceMotionNoteWidth();

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
    // 高级设置逐页展开；参数使用同一 APVTS，学习与预听由编辑器转交处理器。
    class AdvancedPanel  : public PopoverCard
    {
    public:
        AdvancedPanel (juce::AudioProcessorValueTreeState&, InteractionHost&);

        static constexpr int cardWidth = 680;

        void setLanguage (Language newLanguage);

        // 编辑器每帧都送（T-032 R9-12）：面板关着时只记下，打开时（visibilityChanged）按最新状态刷新一次。
        void setStatus (const celadon::MasteringStatus& newStatus);
        int getCardHeight() const noexcept   { return cardHeight; }

        std::function<void()> onLearnReference, onCancelReference, onClearReference;
        std::function<void (bool)> onPreviewChange;

        void paint (juce::Graphics&) override;
        void resized() override;
        void visibilityChanged() override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        class TargetCurve : public juce::Component, public juce::SettableTooltipClient
        {
        public:
            void setValues (const std::array<float, 27>& newValues);
            void setLanguage (Language newLanguage) { language = newLanguage; repaint(); }
            void paint (juce::Graphics&) override;

        private:
            std::array<float, 27> values {};
            Language language = Language::chinese;
        };

        void refreshControls();
        juce::String referenceStatusText() const;

        Language language = Language::chinese;
        celadon::MasteringStatus status;
        static constexpr int cardHeight = 440;
        int page = 0;
        Segmented tabs { Segmented::Look::groove };
        Segmented harshMode { Segmented::Look::bordered }, targetMode { Segmented::Look::bordered }, dither { Segmented::Look::groove };
        ParamSlider harshAmount, transientPreserve, targetAmount, codecMargin, previewLufs;
        TargetCurve targetCurve;
        PillButton learn { PillButton::Icon::relearn }, cancel { PillButton::Icon::relearn }, clear { PillButton::Icon::relearn };
        PillButton preview { PillButton::Icon::match };
        std::unique_ptr<SegmentedAttachment> harshAttachment, targetAttachment, ditherAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
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
