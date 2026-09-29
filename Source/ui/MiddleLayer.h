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
    // 「GLAZE 智能母带 [🔒 已锁定]」：阶段标签随状态变（聆听中、校准中、未学习、受限……）。
    class GlazeTitle  : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        enum class Chip { locked, listening, calibrating, unlearned, paused, limited, off };

        GlazeTitle();

        void setLanguage (Language newLanguage);
        void setChip (Chip newChip);

        void paint (juce::Graphics&) override;

    private:
        Language language = Language::chinese;
        Chip chip = Chip::unlearned;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeTitle)
    };

    // 带图标的胶囊按钮：重新聆听（圆箭头）、等响（两道横线）。
    class PillButton  : public ClickableComponent
    {
    public:
        enum class Icon { relearn, match };
        enum class Look { normal, off, attention };

        explicit PillButton (Icon icon);

        void setText (const juce::String& newText);
        void setLook (Look newLook);
        void setDot (bool shouldShowDot);   // 右边的小橙点（建议重新聆听）
        int getNaturalWidth() const;

        void paint (juce::Graphics&) override;

    private:
        Icon icon;
        Look look = Look::normal;
        juce::String text;
        bool dot = false;
    };

    // 左下角两行小字：平时说明 GLAZE 在做什么（「联动 7 项参数 / 已聆听 15 秒」），悬停或拖动控件时换成操作提示。
    class CornerNote  : public juce::Component
    {
    public:
        CornerNote();

        void setLines (const juce::String& first, const juce::String& second, bool isHint);
        const juce::String& getFirstLine() const noexcept   { return line1; }
        bool isShowingHint() const noexcept                 { return hint; }
        void paint (juce::Graphics&) override;

    private:
        juce::String line1, line2;
        bool hint = false;
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
