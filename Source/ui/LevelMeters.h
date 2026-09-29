/*
  ==============================================================================

    LevelMeters.h
    频谱仪两侧的电平表面板（V4 设计稿 60 × 260）：左边 IN（真峰值），右边 OUT（真峰值）+ GR（限幅衰减，从上往下长）。

      - 刻度 −60 ～ 0 dB（线性 dB）；表条下落 20 dB/s，峰值保持线停 1.2 s 后慢慢落；下面的数字是复位以来的最大值（点数字清零）；
      - OUT 超过 Ceiling 时顶上的小灯、保持线与数字变橙色；方案受限（GLAZE 的保护退到底）时 GR 条变橙色；
      - 只有画面上看得出变化（移动 ≥ 半个物理像素或数字变了）才重画。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Localisation.h"

namespace celadon::ui
{
    // 一根竖表条的显示状态（下落、保持），IN / OUT / GR 共用。
    class MeterBallistics
    {
    public:
        explicit MeterBallistics (bool isReduction) noexcept : reduction (isReduction) {}

        // 每帧（约 30 ms）一次读数：峰值表是 dBTP，衰减表是 dB（≥ 0）。
        void push (float reading, double elapsedSeconds) noexcept;
        void resetHold() noexcept;

        float getLevel() const noexcept     { return level; }
        float getHold() const noexcept      { return hold; }
        float getMaximum() const noexcept   { return maximum; }
        bool hasMaximum() const noexcept    { return maximumValid; }

    private:
        bool reduction;
        float level = -120.0f, hold = -120.0f, maximum = -120.0f;
        double holdAge = 0.0;
        bool maximumValid = false;
    };

    class LevelMeterPanel  : public juce::Component,
                             public juce::TooltipClient
    {
    public:
        enum class Kind { input, outputAndReduction };

        explicit LevelMeterPanel (Kind kind);

        void setLanguage (Language newLanguage);

        // 每帧调：峰值（dBTP）、衰减（dB，只有 OUT 面板用）、Ceiling（判断超限）、方案受限（GR 变橙）。
        void pushReadings (float peakDb, float reductionDb, float ceilingDb, bool planLimited, double elapsedSeconds);
        void resetHolds();

        // 点下面的数字清零时回调（编辑器顺便把响度小窗里的真峰值最大值也清掉）。
        std::function<void()> onResetClicked;

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        juce::String getTooltip() override;

        static constexpr int width = 60;
        static constexpr int height = 260;

    private:
        struct Shown
        {
            float level = -1.0f, hold = -1.0f, grLevel = -1.0f, grHold = -1.0f;
            juce::String peakText, grText;
            bool over = false, planLimited = false;

            bool operator== (const Shown& other) const noexcept;
        };

        Shown computeShown() const;

        Kind kind;
        Language language = Language::chinese;
        MeterBallistics peak { false }, reduction { true };
        float ceiling = -1.0f;
        bool planLimited = false;
        bool overRecently = false;
        double overAge = 10.0;
        Shown shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeterPanel)
    };
}
