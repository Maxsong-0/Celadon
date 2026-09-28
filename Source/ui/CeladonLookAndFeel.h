/*
  ==============================================================================

    CeladonLookAndFeel.h
    「梅子青夜窑」外观（设计规格-界面 第 1、2 节；ADR-005）：颜色常量 + 旋钮 / 开关 / 按钮 / 气泡的画法。

    全部矢量自绘，任何缩放比例都清晰。颜色只在这里定义，改一处全局生效。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace celadon::ui
{
    namespace palette
    {
        inline const juce::Colour background  { 0xff16211d };   // 夜窑深青黑
        inline const juce::Colour panel       { 0xff1e2c27 };
        inline const juce::Colour panelStroke { 0xff2f443b };
        inline const juce::Colour knobBody    { 0xff22342d };
        inline const juce::Colour knobStroke  { 0xff3a5247 };
        inline const juce::Colour track       { 0xff33493f };
        inline const juce::Colour crackle     { 0xff2e443a };   // 画时乘 crackleOpacity
        inline const juce::Colour accent      { 0xff8fc7a8 };   // 梅子青：数值弧、打开的开关、电平表
        inline const juce::Colour text        { 0xffe6eee8 };   // 白瓷
        inline const juce::Colour textDim     { 0xff8fa89a };   // 灰青
        inline const juce::Colour reduction   { 0xffd9a46a };   // GR 表（琥珀）
        inline const juce::Colour warning     { 0xffe0645c };   // 只用于顶栏警告：NaN 屏蔽计数、Debug 构建提醒
        inline const juce::Colour tooltipFill { 0xffe9efe8 };
        inline const juce::Colour tooltipText { 0xff16211d };

        inline constexpr float crackleOpacity  = 0.55f;
        inline constexpr float comingSoonAlpha = 0.35f;
    }

    // Slider 的 properties 里设这个键为 true → 按 GLAZE 大旋钮样式画（开片纹旋钮面、6 px 弧）。
    inline constexpr const char* bigKnobProperty = "celadonBigKnob";

    // Slider 的 properties 里有这个键（double，0 ～ 1）→ 在弧上画 GLAZE 叠加后的实际值（淡弧 + 小圆点）；
    // 没有这个键就不画。由 Knob::setEffectiveProportion 维护。
    inline constexpr const char* effectiveProportionProperty = "celadonEffectiveProportion";

    // Button 的 properties 里设这个键为 true → 画成梅子青描边胶囊：描边与文字都用梅子青、不填底（GLAZE 的「重新聆听」，
    // 设计规格-GLAZE-界面与实现 第 1 节；与 ModulePanel「即将推出」徽章同一种描边胶囊）。悬停 / 按下的反馈与普通胶囊一致。
    // 只对 TextButton 生效（drawButtonBackground / drawButtonText）。
    inline constexpr const char* accentOutlineProperty = "celadonAccentOutline";

    // 小旋钮给实际值小圆点留的出血（px）：Knob::resized 把滑条区向上下标签各多伸这么多，
    // drawRotarySlider 在小旋钮四周留同样的白——两边共用这一个常数，旋钮大小与圆心才不会悄悄错开。
    // 数值由小圆点尺寸推出，CeladonLookAndFeel.cpp 里 static_assert 核对。
    inline constexpr int smallKnobMarkerBleed = 2;

    // 统一字体：系统字体，中文自动回退苹方；medium = 中粗（只用常规与中粗两种字重）。
    juce::Font celadonFont (float height, bool medium = false);

    class CeladonLookAndFeel  : public juce::LookAndFeel_V4
    {
    public:
        CeladonLookAndFeel();
        ~CeladonLookAndFeel() override;

        // 小旋钮：直径 34 px，270° 轨道弧 + 梅子青数值弧 + 白色指针。大旋钮见 bigKnobProperty。
        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                               juce::Slider&) override;

        // 胶囊开关：开 = 实心梅子青底 + 深色字；关 = 只有描边。按钮文字取 getButtonText()。
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        // TextButton（Reset 等）：同样的胶囊外观；设了 accentOutlineProperty 的画成梅子青描边胶囊（「重新聆听」）。
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
        void drawButtonText (juce::Graphics&, juce::TextButton&,
                             bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        // 提示气泡：白瓷底深色字梅子青边；文字按 '\n' 分三行，第 1 行中粗。宽度随文字，最宽 340 px。
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                               juce::Rectangle<int> parentArea) override;
        void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

        juce::Font getLabelFont (juce::Label&) override;

        // 右下角缩放把手：三道短斜线（灰青，悬停 / 拖动时换梅子青），替掉默认的白色斜线。
        void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    private:
        // ---- 以下由实现者（v0.2 计划 A4）按需要补充；公开接口不得改动 ----

        // GLAZE 大旋钮面上的开片纹：按「本体直径 × 物理像素比」缓存，只有其中一个变了才重生成
        // （窗口缩放、Retina 与非 Retina 屏之间拖动都会触发一次重生成，平时每帧只是贴图）。
        const juce::Image& getBigKnobCrackle (int diameter, float pixelScale);

        juce::Image bigKnobCrackle;
        int   bigKnobCrackleDiameter = 0;
        float bigKnobCrackleScale    = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CeladonLookAndFeel)
    };
}
