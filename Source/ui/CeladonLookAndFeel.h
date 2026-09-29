/*
  ==============================================================================

    CeladonLookAndFeel.h
    v0.1.8.0 界面改版的外观：浅色底 + 单色青渐变 + 紫色（GLAZE 自动）+ 橙色（需要注意），颜色与字体只在这里定义。

    配色规则（V4 设计稿）：
      - 青色只有一种色相，从 c100（最浅）到 c900（最深）；数值越大颜色越深（cyanAt）；
      - 紫色只表示「GLAZE 此刻的自动加量 / 自动态」，别的地方不用；
      - 橙色只表示「需要注意」（真峰值超限、方案受限、NaN……），界面上同一时刻最多出现在一处要紧的地方。

    全部矢量自绘，任何缩放比例都清晰。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace celadon::ui
{
    namespace palette
    {
        // ---- 底与面板 ----
        inline const juce::Colour background     { 0xffedf2f4 };
        inline const juce::Colour outside        { 0xffc7d1d5 };   // 画布之外（宿主给了不合宽高比的窗口时露出的边）
        inline const juce::Colour panel          { 0xffffffff };
        inline const juce::Colour panelBorder    { 0xffdce6e9 };
        inline const juce::Colour controlBorder  { 0xffd0dde2 };
        inline const juce::Colour segmentBorder  { 0xffd8e3e7 };
        inline const juce::Colour groove         { 0xffeef3f5 };   // 分段选择器的底槽
        inline const juce::Colour languageGroove { 0xffe3ebee };
        inline const juce::Colour track          { 0xffe6eef1 };   // 电平表与滑条的轨道
        inline const juce::Colour knobTrack      { 0xffdce6e9 };   // 旋钮的 270° 轨道
        inline const juce::Colour spectrumFloor  { 0xfff3f8f9 };
        inline const juce::Colour divider        { 0xffcfdce1 };
        inline const juce::Colour shadowTint     { 0xff0d2830 };   // 面板阴影的色调（rgba(13,40,48,…)）

        // ---- 文字 ----
        inline const juce::Colour ink       { 0xff0d1b21 };
        inline const juce::Colour ink2      { 0xff4b5e66 };
        inline const juce::Colour ink3      { 0xff637780 };
        inline const juce::Colour ink4      { 0xff8a9ca3 };
        inline const juce::Colour separator { 0xff9aabb2 };
        inline const juce::Colour lampIdle  { 0xffb8c7cd };

        // ---- 单色青（c100 最浅 → c900 最深）----
        inline const juce::Colour c900 { 0xff06313a };
        inline const juce::Colour c800 { 0xff0a4a57 };
        inline const juce::Colour c700 { 0xff0e6473 };
        inline const juce::Colour c600 { 0xff127f90 };
        inline const juce::Colour c500 { 0xff1799ab };
        inline const juce::Colour c400 { 0xff33b3c4 };
        inline const juce::Colour c300 { 0xff67cad7 };
        inline const juce::Colour c200 { 0xffa3e0e8 };
        inline const juce::Colour c100 { 0xffd5f1f5 };
        inline const juce::Colour relearnBorder { 0xff9fd7e0 };

        // ---- 紫色 = GLAZE 自动 ----
        inline const juce::Colour violet       { 0xff6a4dff };
        inline const juce::Colour violetSoft   { 0xffeeeaff };
        inline const juce::Colour violetBorder { 0xffb9acff };

        // ---- 橙色 = 需要注意 ----
        inline const juce::Colour warning       { 0xffe8590c };
        inline const juce::Colour warningText   { 0xffc2410c };
        inline const juce::Colour warningSoft   { 0xfffff4ec };
        inline const juce::Colour warningBorder { 0xfff5b48a };

        // ---- 旋钮帽（深色）----
        inline const juce::Colour capTop    { 0xff1f4049 };
        inline const juce::Colour capBottom { 0xff061519 };
    }

    // 单色青渐变上的一点：x ∈ [0, 1]，0 = c200（浅）→ 0.35 c400 → 0.7 c600 → 1 = c800（深）。数值弧、滑条填充按它上色：
    // 值越大颜色越深。
    juce::Colour cyanAt (float x) noexcept;

    // ---- 字体 ----
    // 界面字体族：优先「中文字体自带的拉丁字形」——苹方（macOS）/ 微软雅黑 UI（Windows）/ Noto Sans CJK（Linux），中英文同一族，
    // 同一行里中文不会比英文大一号（v0.2 夜间截图验收发现的问题）。等宽字体给数字：Menlo / Consolas / DejaVu Sans Mono。
    enum class Weight : int { light = 0, regular, medium, semibold };

    juce::Font uiFont (float height, Weight weight = Weight::regular);
    juce::Font monoFont (float height, Weight weight = Weight::regular);

    // 旧接口：常规 / 中粗两种字重。
    juce::Font celadonFont (float height, bool medium = false);

    // 文字宽度（逻辑像素）。
    float textWidth (const juce::Font& font, const juce::String& text);

    // 字距：CSS 的 letter-spacing（像素）换成 JUCE 的字距系数（相对字高）。
    juce::Font withLetterSpacing (const juce::Font& font, float pixels);

    //==============================================================================
    class CeladonLookAndFeel  : public juce::LookAndFeel_V4
    {
    public:
        CeladonLookAndFeel();
        ~CeladonLookAndFeel() override;

        // 提示气泡：白底、浅灰描边、8 px 圆角；文字按 '\n' 分行，第 1 行中粗。宽度随文字，最宽 360 px。
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                               juce::Rectangle<int> parentArea) override;
        void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

        juce::Font getLabelFont (juce::Label&) override;
        juce::Font getPopupMenuFont() override;

        // 右下角缩放把手：三道短斜线（灰，悬停 / 拖动时换青色）。
        void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

        // 直接输入数值的小文本框：白底、青色描边。
        void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
        void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CeladonLookAndFeel)
    };
}
