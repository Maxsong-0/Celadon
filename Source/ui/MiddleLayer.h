/*
  ==============================================================================

    MiddleLayer.h
    中层（主旋钮一带）的小部件：GLAZE 标题行与阶段标签、等响按钮、底排左边的操作提示、右下角的颜色图例，
    以及 T-107 重构包 A 的两块侧板（左「此刻在推什么」、右「交付」）。
    风格与母带 / 原声用 Segmented（Controls.h），主旋钮见 GlazeKnob，粒子见 ParticleField。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"

#include <array>

namespace celadon::ui
{
    // 「GLAZE 智能母带 [跟随]」：阶段标签跟状态行的那一条走（起步、保持、跟随、受限……）。
    class GlazeTitle  : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        enum class Chip { manual, manualLimit, coldExport, limited, starting, holding, following };

        GlazeTitle();

        void setLanguage (Language newLanguage);
        // attention：橙色此刻归 GLAZE（编辑器按 glazestatus::attentionOwner 给）。为 false 时琥珀三种标签退成中性灰底。
        void setChip (Chip newChip, bool attention = true);

        void paint (juce::Graphics&) override;

    private:
        Language language = Language::chinese;
        Chip chip = Chip::starting;
        bool chipAttention = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeTitle)
    };

    // 带图标的胶囊按钮：圆箭头（学习参考）、停止（取消学习）、叉（清除参考）、返回（退出监听）、等响（两道横线）。
    // 外观：normal 白底青边、off 灰（关着）、monitoring 深色（ink 底白字，退出听原声 / 试听差值 / 预听，与胶囊的深色圆点同一套；
    // 监听是一种模式，不是问题，所以不用橙，T-111 / 4-06）。
    class PillButton  : public ClickableComponent
    {
    public:
        enum class Icon { relearn, stop, cross, back, match };
        enum class Look { normal, off, monitoring };

        explicit PillButton (Icon icon);

        // setText 返回这次有没有改变（改自然宽度：编辑器只在变了时重排底排，T-032 R9-12）。
        bool setText (const juce::String& newText);
        void setLook (Look newLook);
        const juce::String& getText() const noexcept   { return text; }
        int getNaturalWidth() const;

        void paint (juce::Graphics&) override;

    private:
        Icon icon;
        Look look = Look::normal;
        juce::String text;
    };

    // 底排左边两行小字（T-107 起在画布 (24, 524, 330, 30)）：悬停或拖动控件时的操作提示、本次打开的旧工程警告；平时空着
    // （原来的静态说明「联动 7 项参数 / 跟着主段慢慢调整」删了：左侧板已经把 7 项列出来）。
    class CornerNote  : public juce::Component
    {
    public:
        CornerNote();

        void setLines (const juce::String& first, const juce::String& second, bool isHint, bool isWarning = false);
        const juce::String& getFirstLine() const noexcept   { return line1; }
        const juce::String& getSecondLine() const noexcept  { return line2; }
        bool isShowingHint() const noexcept                 { return hint; }
        void paint (juce::Graphics&) override;

    private:
        juce::String line1, line2;
        bool hint = false;
        bool warning = false;   // 用琥珀色：本次打开的旧工程提示（平时只有第二行有字）
    };

    // 右下角的颜色图例：手动设置（青色渐变）· GLAZE 自动调整（紫）· 需要注意（橙）。
    class ColourLegend  : public juce::Component
    {
    public:
        ColourLegend();

        void setLanguage (Language newLanguage);
        int getNaturalWidth() const;
        void paint (juce::Graphics&) override;

    private:
        Language language = Language::chinese;
    };

    //==============================================================================
    // T-107 重构包 A（T-105 线 ③ 第 6.2 节）：中层两侧的两块侧板。组件从标题行（画布 y 302）起：上面 26 px 是表头（画在标题行里、
    // 不带底，占满一行），往下 6 px 是空隙，再往下是白底面板（画布 y 334 ～ 516）。表头那一条只是标题行里的字，不接鼠标
    // （hitTest），和标题行别处一样点了没反应；能点的只有面板里的东西。两块都挂整数对齐缓存（粒子带每帧重画时只贴图）。
    namespace sidepanel
    {
        inline constexpr int top = 302;            // 组件上沿（画布 y）
        inline constexpr int headerHeight = 26;
        inline constexpr int boxTop = 32;          // 面板上沿（组件内 y；画布 y 334）
        inline constexpr int height = 214;         // 到画布 y 516
        inline constexpr int padX = 12;            // 内容区左右各留
    }

    // 左侧板「此刻在推什么」：四个模块各一行（出力条 + GLAZE 的自动加量），分隔线下是不截断的状态句与把握 / 主段 / 保护。
    class GlazeNowPanel  : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        // 一行模块。CLAY 没有出力条（它不在 GLAZE 的 7 项联动里），bar 填 −1，badge 写三项里哪几项在自动档。
        struct Row
        {
            float bar = -1.0f;              // 0 ～ 1：出力条长度；< 0 = 不画条
            juce::String badge;             // 不画条时，条的位置写的字（CLAY：自动 / 手动 / 自动 2 项）
            juce::String text;              // 文字列：加量或此刻的值；空 = 「—」
            bool automatic = false;         // 文字用紫色（GLAZE 自动加量）
            bool bypassed = false;          // 整行 0.35 透明度（与卡片一致）

            bool operator== (const Row& other) const noexcept;
            bool operator!= (const Row& other) const noexcept   { return ! operator== (other); }
        };

        struct Values
        {
            std::array<Row, 4> rows;        // BALANCE、CLAY、COLOR、LIMIT（处理顺序）
            juce::String statusLine;        // 状态胶囊那一句的全文（不截断，最多两行）
            bool statusWarning = false;     // 琥珀状态：状态句用 warningText
            juce::String details;           // 末行：把握 · 主段 · 保护（GLAZE 0 时空）

            bool operator== (const Values& other) const noexcept;
            bool operator!= (const Values& other) const noexcept   { return ! operator== (other); }
        };

        GlazeNowPanel();

        void setLanguage (Language newLanguage);
        void setValues (const Values& newValues);
        const Values& getValues() const noexcept   { return values; }   // 截图工具的检查读

        bool hitTest (int x, int y) override   { return x >= 0 && y >= sidepanel::boxTop; }
        void paint (juce::Graphics&) override;

    private:
        Language language = Language::chinese;
        Values values;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeNowPanel)
    };

    // 右侧板「交付」：整体响度大字、短期 / 真峰值 / 峰响比，−24 ～ −6 LUFS 的响度条（目标三角在条上、−14 虚线在条下），
    // 最近 60 秒的短期响度与 GLAZE 自动增益。表头写「目标 −17.4 LUFS」（右对齐）。
    // 历史由编辑器每 100 ms 推一个点（pushHistory），600 点；没有声音时编辑器不推，画面不滚动。
    class DeliveryPanel  : public juce::Component,
                           public juce::TooltipClient
    {
    public:
        DeliveryPanel();

        struct Values
        {
            float integratedLufs = -100.0f;     // LoudnessMeter::noReading（−100）= 没有读数
            float shortTermLufs = -100.0f;
            float truePeakMaxDb = -100.0f;      // 复位以来 OUT 的最大真峰值
            float ceilingDb = -1.0f;
            bool hasTarget = false;             // GLAZE > 0 且编排器给出了有限目标
            float targetLufs = 0.0f;
            bool truePeakAttention = true;      // 橙色没被别的事占着（T-111 橙色规则）：真峰值最大值超过上限时才可以用橙字
        };

        struct HistoryPoint
        {
            float shortTermLufs = -100.0f;
            float autoGainDb = 0.0f;            // GLAZE 推给 LIMIT 增益的自动加量（dB）
            bool guardEngaged = false;          // 守护此刻在退让
        };

        static constexpr int historyLength = 600;          // 60 s × 每 100 ms 一点
        static constexpr double historyStepSeconds = 0.1;

        void setLanguage (Language newLanguage);
        void setValues (const Values& newValues);
        void pushHistory (const HistoryPoint& point);
        void clearHistory();
        int getHistoryCount() const noexcept   { return historyCount; }

        std::function<void()> onReset;

        bool hitTest (int x, int y) override   { return x >= 0 && y >= sidepanel::boxTop; }
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        juce::String getTooltip() override;

    private:
        juce::Rectangle<float> contentArea() const;
        juce::Rectangle<float> resetIconBounds() const;
        static bool hasReading (float lufs) noexcept   { return lufs > -99.0f; }

        Language language = Language::chinese;
        Values values;
        juce::String shownKey;              // 上次重画时各项显示文字拼起来：没变就不重画
        std::array<HistoryPoint, historyLength> history {};
        int historyHead = 0, historyCount = 0;   // 环形缓冲：下一个写入位置、已有点数
        bool resetHovered = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeliveryPanel)
    };
}
