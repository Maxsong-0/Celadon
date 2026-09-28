/*
  ==============================================================================

    Knob.h
    一个旋钮 = 名字（上）+ 旋钮 + 数值（下）。统一手感（设计规格-界面 第 2 节）：
    上下拖动；双击回默认值；按住 ⌘ 拖动慢速精调；滚轮微调；拖动时数值实时变。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace celadon::ui
{
    class Knob  : public juce::Component
    {
    public:
        enum class Size { small, big };

        // 小旋钮建议占位（名字 18 + 旋钮约 44 + 数值 18）；大旋钮由调用方给更大的区域。
        static constexpr int labelHeight = 18;   // 名字 / 数值标签各占的高度（12 px 字）；「高级」抽屉按它裁出旋钮那一截
        static constexpr int smallWidth  = 56;
        static constexpr int smallHeight = 80;   // 名字 18 + 旋钮 44（本体 34 = 44 − 2 × (弧宽 3 + 间隙 2)）+ 数值 18；
                                                 // 滑条组件上下各多伸 2 px 叠进标签（48），给 GLAZE 实际值小圆点出血，旋钮本身位置不变

        // 绑定参数：数值文字、双击默认值取自参数；提示文案按参数 ID 从 HelpText 取。
        Knob (juce::AudioProcessorValueTreeState& state, const char* parameterId,
              const juce::String& displayName, Size size = Size::small);

        // 占位（功能未上线，如 Balance、GLAZE）：不绑定参数、不可拖动，显示固定的指针位置与数值文字，
        // 悬停仍按 helpId 显示提示。
        Knob (const char* helpId, const juce::String& displayName,
              const juce::String& placeholderValueText, double placeholderProportion, Size size = Size::small);

        ~Knob() override;

        void resized() override;

        juce::Slider& getSlider() noexcept   { return slider; }

        // GLAZE 叠加后的「实际值」（ADR-006）：proportion 是实际值在该参数 0 ～ 1 刻度上的位置；
        // show 为 false 时不画（GLAZE = 0）。编辑器定时器每 30 ms 调一次，只有变化时才重画。
        void setEffectiveProportion (double proportion, bool show);

    private:
        // ---- 以下由实现者（v0.2 计划 A5）按需要补充；公开接口不得改动 ----

        // 自己算拖动增量的 Slider：普通拖动 250 px 走满量程，按住 ⌘ 慢 8 倍。
        // 不用 JUCE 自带的 velocity 模式：它按鼠标速度非线性缩放（慢移 1 px 完全不动、快移只慢 2 倍）
        // 还会隐藏光标，做不到「确定地慢至少 4 倍」。mouseDown / mouseUp 仍交给基类，
        // 附件才能收到拖动开始 / 结束（beginChangeGesture / endChangeGesture）。
        class PrecisionSlider  : public juce::Slider
        {
        public:
            void mouseDown (const juce::MouseEvent&) override;
            void mouseDrag (const juce::MouseEvent&) override;
            void mouseUp (const juce::MouseEvent&) override;

        private:
            juce::Point<float> lastDragPosition;
            double dragProportion = 0.0;   // 未经步进吸附的连续位置，步进参数拖起来才不会「卡格」
            bool dragging = false;
        };

        PrecisionSlider slider;
        juce::Label nameLabel, valueLabel;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

        // GLAZE 实际值标记的显示状态（setEffectiveProportion）：记住上次写进 slider 属性的那份，
        // 只有显示状态变了、或比例比上次画的相差超过阈值才再写属性并重画——编辑器每 30 ms 调一次，不能每次都重画。
        bool   effectiveShown      = false;
        double effectiveProportion = 0.0;   // 显示时有效：上次画出来的实际值比例（0 ～ 1）

        void setUpCommon (const juce::String& displayName, const juce::String& tooltip, Size size);
        void updateValueText();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
    };
}
