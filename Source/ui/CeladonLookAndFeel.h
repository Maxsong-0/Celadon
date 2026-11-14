/*
  ==============================================================================

    CeladonLookAndFeel.h
    v0.1.8.0 界面改版的外观：浅色底 + 单色青渐变 + 紫色（GLAZE 自动）+ 橙色（需要注意），颜色与字体只在这里定义。

    配色规则（V4 设计稿）：
      - 青色只有一种色相，从 c100（最浅）到 c900（最深）；数值越大颜色越深（cyanAt）；
      - 紫色只表示「GLAZE 此刻的自动加量 / 自动态」，别的地方不用；
      - 橙色只表示「需要注意」（真峰值超限、方案受限、NaN……）。T-111 起的写法（规格 V4 同一条）：同一件事可以在源头和顶部
        状态胶囊两处同时亮橙；不同的事不同时亮——同一时刻只有一件事拿到橙色，由编辑器按 glazestatus::attentionOwner 的优先级定
        （异常音频块 > 真峰值超限 > GLAZE 琥珀 > CLAY 守护灯 > 旧工程提示），别的源头退回中性色。两处例外不算「亮」：图例里
        「橙 = 需要注意」的色块是钥匙；右侧板 60 秒历史里的守护刻度是已经过去的记录。听原声、试听差值、归一化预听是监听模式，
        不是问题，一律用深色（ink），不用橙。

    全部矢量自绘，任何缩放比例都清晰。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <vector>

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
        inline const juce::Colour offFill        { 0xfff3f6f7 };   // 关着的胶囊按钮（等响 关、预听没开）的底色
        inline const juce::Colour languageGroove { 0xffe3ebee };
        inline const juce::Colour track          { 0xffe6eef1 };   // 电平表与滑条的轨道
        inline const juce::Colour knobTrack      { 0xffdce6e9 };   // 旋钮的 270° 轨道
        inline const juce::Colour spectrumFloor  { 0xfff3f8f9 };
        inline const juce::Colour divider        { 0xffcfdce1 };
        inline const juce::Colour shadowTint     { 0xff0d2830 };   // 面板阴影的色调（rgba(13,40,48,…)）

        // ---- 文字 ----
        // 小字（em < 14 px）对它的底色都要 ≥ 4.5 : 1（WCAG AA 小字）。ink3 原来 #637780：白底 4.69，画布 4.15、分段底槽 4.19、
        // 语言底槽 3.88 都不够（T-105 4-20）；T-112 换成 #566A73——白 5.67、画布 5.02、底槽 5.07、语言底槽 4.69、频谱底 5.29、
        // 侧板 5.56，用到它的底色全过（逐对见 docs/validation/2026-11-13-T112-type-contrast/）。ink4 只画图形（缩放把手、关着的电源）。
        inline const juce::Colour ink       { 0xff0d1b21 };
        inline const juce::Colour ink2      { 0xff4b5e66 };
        inline const juce::Colour ink3      { 0xff566a73 };
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
        // violet 只画弧线、圆点、出力条这些图形；紫色的字一律 violetText（T-112：#6A4DFF 在浅紫底上只有 4.33，#5438D9 白底 7.13、
        // 浅紫底 6.06）。
        inline const juce::Colour violet       { 0xff6a4dff };
        inline const juce::Colour violetText   { 0xff5438d9 };
        inline const juce::Colour violetSoft   { 0xffeeeaff };
        inline const juce::Colour violetBorder { 0xffb9acff };

        // ---- 橙色 = 需要注意 ----
        // warning 只画图形（对白 3.58，不够当字）；橙色的字一律 warningText（白 5.18、浅橙底 4.78、画布 4.59）。
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

    // 旁通的模块（卡片、左侧板的那一行）：照样能调、数值照样要读，所以只淡到 0.55（数值字对白底约 3.9 : 1；原来 0.35 只剩 2.2 : 1，
    // T-105 4-19）；「没在处理」改由数值弧、彗尾、GLAZE 加量与滑条填充去色来表达（饱和度 0 = 同明度的灰）。
    namespace bypassLook
    {
        inline constexpr float opacity    = 0.55f;
        inline constexpr float saturation = 0.0f;
    }

    // ---- 字体 ----
    // 界面字体族：优先「中文字体自带的拉丁字形」——苹方（macOS）/ 微软雅黑 UI（Windows）/ Noto Sans CJK（Linux），中英文同一族，
    // 同一行里中文不会比英文大一号（v0.2 夜间截图验收发现的问题）。等宽字体给数字：Menlo / Consolas / DejaVu Sans Mono。
    enum class Weight : int { light = 0, regular, medium, semibold };

    // ---- 字号（T-112 包 B）----
    // 全界面只有这几档，数是 100 % 缩放下的 em 像素（FontOptions::withPointHeight）：设计稿写多少 px，屏幕上 1 em 就是多少 px。
    // 原来传的是 JUCE 字高（升部 + 降部），同一个数在苹方上 em 只有它的 1 / 1.40、微软雅黑 UI 1 / 1.27、Menlo 1 / 1.164——
    // Mac 上正文只有 8.6 px，数字比旁边的字大两成；等宽串里回退出来的中文也按回退字体的升降部缩（T-105 4-01）。按 em 给之后
    // 苹方、微软雅黑 UI、Noto、DejaVu 同一档一样大，回退字体与主字体同 em（JUCE 换回退字体时保留点高）。
    // 主旋钮里的字按旋钮自己的 224 px 设计坐标给（画布上整颗缩到 200 px，与旋钮的其余尺寸一起缩）。
    enum class TextSize
    {
        micro,         //  9：刻度、单位、角标
        caption,       // 10：说明、图例、副标题、表头、小标签
        body,          // 11：控件名、分段文字、胶囊、气泡与浮层正文（Max 2026-11-12 定正文 em 11）
        value,         // 11：数值（配等宽字体）
        title,         // 13：卡片名、浮层标题、品牌（半粗）
        display,       // 22：整体响度
        displayLarge   // 32：主旋钮百分比、缩放提示
    };

    namespace typescale
    {
        inline constexpr float micro = 9.0f, caption = 10.0f, body = 11.0f, value = 11.0f, title = 13.0f,
                               display = 22.0f, displayLarge = 32.0f;
    }

    constexpr float emOf (TextSize size) noexcept
    {
        switch (size)
        {
            case TextSize::micro:        return typescale::micro;
            case TextSize::caption:      return typescale::caption;
            case TextSize::body:         return typescale::body;
            case TextSize::value:        return typescale::value;
            case TextSize::title:        return typescale::title;
            case TextSize::display:      return typescale::display;
            case TextSize::displayLarge: return typescale::displayLarge;
        }

        return typescale::body;
    }

    juce::Font uiFont (TextSize size, Weight weight = Weight::regular);
    juce::Font monoFont (TextSize size, Weight weight = Weight::regular);

    // 文字宽度（逻辑像素）。
    float textWidth (const juce::Font& font, const juce::String& text);

    // 字距：CSS 的 letter-spacing（像素）换成 JUCE 的字距系数（相对字高）。
    juce::Font withLetterSpacing (const juce::Font& font, float pixels);

    // ---- 单行文字（T-112）----
    // 画进固定框的单行字一律经它画：等同 g.drawText (text, area, justification, useEllipsis)，用 g 此刻的字体。JUCE 放不下时按框宽
    // 截掉末尾——不加省略号时是悄悄丢字（T-105 4-01 的风险）。截图工具打开 textfit 记录后每次都先量一次，放不下的记下来；
    // 平时不记录、不多量。
    void drawTextLine (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                       juce::Justification justification, bool useEllipsis = false);

    namespace textfit
    {
        struct Overflow
        {
            juce::String text;
            float textWidth = 0.0f, boxWidth = 0.0f;
        };

        // 只在消息线程用（paint 都在消息线程）。截图工具的检查打开、逐状态重画、取走记下的；插件里从不打开。
        void setRecording (bool shouldRecord) noexcept;
        bool isRecording() noexcept;
        std::vector<Overflow> takeOverflows();
    }

    //==============================================================================
    class CeladonLookAndFeel  : public juce::LookAndFeel_V4
    {
    public:
        CeladonLookAndFeel();
        ~CeladonLookAndFeel() override;

        // 提示气泡：白底、浅灰描边、8 px 圆角；文字按 '\n' 分行，第 1 行中粗（正文档），其余正文档常规。宽度随文字，最宽 360 px。
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
