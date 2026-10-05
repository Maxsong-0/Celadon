/*
  ==============================================================================

    MiddleLayer.h
    中层（主旋钮一带）的小部件：GLAZE 标题行与阶段标签、重新聆听 / 等响按钮、左下角的说明与操作提示、右下角的颜色图例。
    风格与 A / B 用 Segmented（Controls.h），主旋钮见 GlazeKnob，粒子见 ParticleField。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"

namespace celadon::ui
{
    // 「GLAZE 智能母带 [跟随]」：阶段标签跟状态行的那一条走（起步、保持、跟随、受限……），不再另说一套聆听 / 锁定。
    class GlazeTitle  : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        enum class Chip { manual, manualLimit, phase, coldExport, limited, starting, holding, following };

        GlazeTitle();

        void setLanguage (Language newLanguage);
        void setChip (Chip newChip);

        void paint (juce::Graphics&) override;

    private:
        Language language = Language::chinese;
        Chip chip = Chip::starting;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeTitle)
    };

    // 带图标的胶囊按钮：圆箭头（参考学习的学 / 取消 / 清除）、等响（两道横线）。GLAZE 没有「重新聆听」。
    class PillButton  : public ClickableComponent
    {
    public:
        enum class Icon { relearn, match };
        enum class Look { normal, off, attention };

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

    // 左下角两行小字：平时说明 GLAZE 在做什么（「联动 7 项参数 / 已聆听 15 秒」），悬停或拖动控件时换成操作提示。
    class CornerNote  : public juce::Component
    {
    public:
        CornerNote();

        void setLines (const juce::String& first, const juce::String& second, bool isHint, bool isWarning = false);
        const juce::String& getFirstLine() const noexcept   { return line1; }
        bool isShowingHint() const noexcept                 { return hint; }
        void paint (juce::Graphics&) override;

    private:
        juce::String line1, line2;
        bool hint = false;
        bool warning = false;   // 第二行用琥珀色：本次打开的旧工程提示
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
}
