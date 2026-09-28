/*
  ==============================================================================

    BalanceCurve.h
    Balance 面板的 27 段修正曲线（设计规格-界面 第 3 节「BALANCE：顶部 27 段修正曲线」）。
    横轴按对数频率排 27 个段中心（40 Hz ～ 16 kHz），纵轴 ±8 dB；中线虚线；
    向上（提升）用梅子青、向下（压下）用琥珀色填充到中线；悬停显示 HelpText 的 ui_balance_curve。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "../dsp/BalanceModule.h"

namespace celadon::ui
{
    class BalanceCurve  : public juce::Component,
                          public juce::SettableTooltipClient
    {
    public:
        BalanceCurve();
        ~BalanceCurve() override;

        // 界面定时器每 30 ms 调一次，传 27 段当前修正量（dB）。画面上看不出变化时不重画：
        // 每段的点移动都不到半个物理像素，而且「有没有可见修正」没有翻转（v0.5 R5）。
        void setBandGains (const celadon::BalanceModule::BandGains& gainsDb);

        void paint (juce::Graphics&) override;

    private:
        // ---- 以下由实现者（v0.3 计划 B4）按需要补充；公开接口不得改动 ----
        // 画面上正显示的 27 段修正量：paint 只读它，只有要重画时才整份换新（与「上次画出来的」比，慢慢漂移攒够也会画）。
        celadon::BalanceModule::BandGains gains {};

        // 上次 paint 时每个逻辑像素对应的物理像素；第一次画之前按 3（150 % × Retina）算，宁可多画一次。
        float physicalPixelsPerLogical = 3.0f;

        // 任一段的修正量肉眼可见（|G| ≥ 0.01 dB）才画曲线与填充；全为 0 时 paint 只画中线。
        static bool anyVisibleGain (const celadon::BalanceModule::BandGains& bandGains) noexcept;
        bool hasVisibleGains() const noexcept   { return anyVisibleGain (gains); }

        // 纵轴每 dB 多少逻辑像素（与 paint 里绘图区同一算法）。
        float pixelsPerDb() const noexcept;

        // 第 band 段在绘图区里的点：横轴 log10(中心频率) 在 [plot.x, plot.right] 内均匀分布，
        // 纵轴 ±8 dB 映射到 [plot.bottom, plot.y]（上正下负，超出 ±8 dB 的值钉在边上）。
        juce::Point<float> pointForBand (int band, juce::Rectangle<float> plot) const noexcept;

        // 穿过 27 个点的平滑开放路径（单调三次 Hermite 转三次贝塞尔：相邻两点之间不过冲，曲线不会越出绘图区）。
        juce::Path buildCurvePath (juce::Rectangle<float> plot) const;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BalanceCurve)
    };
}
