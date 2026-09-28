/*
  ==============================================================================

    ModulePanel.h
    一个模块面板（BALANCE / COLOR / LIMIT，默认 280 × 232）：标题行、旋钮行（最多 5 个）、底部开关行，
    可选的标题附件（Glue GR 小表）、Balance 曲线带（v0.3 起放 27 段修正曲线组件，之前画占位虚线）、
    三选一竖排胶囊（Speed，v0.3 起绑定参数；v0.6 起由 CapsuleSelector 实现，与 GLAZE 的风格胶囊共用）、「即将推出」蒙层。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "CapsuleSelector.h"
#include "Knob.h"

namespace celadon::ui
{
    class ModulePanel  : public juce::Component
    {
    public:
        explicit ModulePanel (const juce::String& title);
        ~ModulePanel() override;

        // 旋钮行：从左到右依次排列（面板拥有它）。返回引用方便调用方再设置。
        Knob& addKnob (std::unique_ptr<Knob> knob);

        // 底部开关行（胶囊 72 × 22，从左到右）。绑定参数；提示文案按参数 ID 取。
        juce::ToggleButton& addToggle (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                                       const juce::String& label);

        // 占位开关：不绑定参数、不可点，悬停按 helpId 显示提示。
        juce::ToggleButton& addPlaceholderToggle (const char* helpId, const juce::String& label, bool on);

        // 旋钮行右侧的三选一竖排胶囊（Balance 的 Speed；v0.2 是占位），caption 写在上方。
        void addPlaceholderSegmented (const char* helpId, const juce::String& caption,
                                      const juce::StringArray& options, int selectedIndex);

        // 旋钮行右侧的三选一竖排胶囊，绑定 AudioParameterChoice（Balance 的 Speed，v0.3 起）；caption 写在上方，
        // 每个胶囊悬停按参数 ID 显示提示。options 的顺序 = 参数的选项顺序。
        void addSegmented (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                           const juce::String& caption, const juce::StringArray& options);

        // 旋钮行上方曲线带里放一个组件（Balance 的 27 段修正曲线，v0.3 起）。面板不拥有它；
        // 设了之后曲线带显示这个组件，不再画占位虚线。
        void setCurveComponent (juce::Component* curve);

        // 标题行右侧的附件（Color 的 Glue GR 小表）。面板不拥有它，只负责摆放与显示。
        void setTitleAccessory (juce::Component* accessory);

        // 旋钮行上方画 Balance 的 27 段修正曲线位置（v0.2 画一条占位虚线）。
        void setShowsBalanceCurvePlaceholder (bool shouldShow);

        // 「即将推出」：整块 35% 亮度，中央写「即将推出」，全部子控件不可操作，但悬停仍有提示。
        void setComingSoon (bool shouldBeComingSoon);

        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;
        void resized() override;

    private:
        // ---- 以下由实现者（v0.2 计划 A5）按需要补充；公开接口不得改动 ----
        juce::String title;

        juce::OwnedArray<Knob> knobs;

        // 附件必须在按钮之后声明：析构按相反顺序，附件先解除监听，按钮才销毁。
        juce::OwnedArray<juce::ToggleButton> toggles;
        juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAttachments;

        // 三选一竖排胶囊（Speed）：说明文字 + 三个同一 radio 组的胶囊，绑定参数时自带附件（v0.6 起是独立组件
        // CapsuleSelector，与 GLAZE 的风格胶囊共用；v0.3 ～ v0.5 这段写在面板里）。用了才显示。
        CapsuleSelector segmented;
        bool hasSegmented = false;

        juce::Component* accessory = nullptr;   // 不拥有
        juce::Component* curve     = nullptr;   // 不拥有：曲线带里的 27 段修正曲线（setCurveComponent）
        bool showsCurvePlaceholder = false;
        bool comingSoon = false;

        // resized 算好、paint 直接用的区域。
        juce::Rectangle<int> titleArea, curveArea;

        juce::ToggleButton& addToggleButton (const juce::String& label, const juce::String& tooltip);

        // 曲线带是否要占位：设了曲线组件，或 v0.2 的占位虚线打开。
        bool hasCurveBand() const noexcept   { return curve != nullptr || showsCurvePlaceholder; }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModulePanel)
    };
}
