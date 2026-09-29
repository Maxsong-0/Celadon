/*
  ==============================================================================

    SpectrumView.h
    上半部分正中的频谱仪面板（V4 设计稿 1032 × 260）与右上角的响度小窗。

      - 20 Hz ～ 20 kHz 对数横轴，−84 ～ −3 dB 纵轴；OUT 是插件最终输出（听原声时就是原声），填充用单色青的横向渐变、
        62 % 不透明、白色细竖纹、往下渐隐；白线 = 平均；深青细线 = 峰值保持；虚线 = IN（链入口）。三者都可开关；
      - 显示斜率 0 / 3 / 4.5 dB/oct（绕 1 kHz 转，默认 4.5：音乐的频谱大致按这个斜率往下走，转平后各段高低一眼可比）；
      - CLAY 的低切与收单频率画成竖虚线与标签，低切以下打斜线（修坯此刻实际在处理时才画）；
      - 悬停读数：「−18.80dB | 882.32Hz | A5 + 4 音分」；鼠标不在面板上时读的是此刻最响的那个峰。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "SpectrumAnalyser.h"

namespace celadon::ui
{
    //==============================================================================
    // 响度小窗（214 × 88）：整体响度（大字）、短期、真峰值（复位以来的最大值）、峰响比、流媒体归一量、目标响度标记。
    class LoudnessHud  : public juce::Component,
                         public juce::TooltipClient
    {
    public:
        LoudnessHud();

        void setLanguage (Language newLanguage);

        struct Values
        {
            float integratedLufs = -100.0f;     // LoudnessMeter::noReading（−100）= 没有读数
            float shortTermLufs = -100.0f;
            float truePeakMaxDb = -100.0f;      // 复位以来 OUT 的最大真峰值
            float ceilingDb = -1.0f;
            bool hasTarget = false;             // GLAZE 此刻追的目标响度（锁定且 GLAZE > 0）
            float targetLufs = 0.0f;
        };

        void setValues (const Values& newValues);

        std::function<void()> onReset;

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        juce::String getTooltip() override;

    private:
        juce::Rectangle<float> resetIconBounds() const;
        bool hasReading (float lufs) const noexcept   { return lufs > -99.0f; }

        Language language = Language::chinese;
        Values values;
        juce::String shownKey;           // 上次重画时各项显示文字拼起来：没变就不重画
        bool resetHovered = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessHud)
    };

    //==============================================================================
    class SpectrumView  : public juce::Component,
                          public juce::SettableTooltipClient
    {
    public:
        SpectrumView();

        static constexpr int width = 1032;
        static constexpr int height = 260;
        static constexpr float topDb = -3.0f;
        static constexpr float bottomDb = -84.0f;

        void setLanguage (Language newLanguage);

        // 每帧调：从计算器拷出几条曲线，重画。
        void setSpectrum (const SpectrumAnalyser& analyser);

        // CLAY 此刻的实际频率（0 = 没在处理）；active = 修坯没被旁通。
        void setClayMarkers (float lowCutHz, float monoHz, bool active);

        LoudnessHud& getLoudnessHud() noexcept   { return hud; }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        static float xOf (float frequency) noexcept;
        static float yOf (float db) noexcept;

    private:
        // 图例开关（OUT / IN / PEAK）：色块 + 名字；关掉时整颗变淡。
        class LegendToggle  : public ClickableComponent
        {
        public:
            enum class Swatch { gradient, input, peak };

            explicit LegendToggle (Swatch swatch);

            void setText (const juce::String& newText);
            void setOn (bool shouldBeOn)             { on = shouldBeOn; repaint(); }
            bool isOn() const noexcept               { return on; }
            int getNaturalWidth() const;

            void paint (juce::Graphics&) override;

        private:
            Swatch swatch;
            juce::String text;
            bool on = true;
        };

        // 斜率选择：「斜率」+ 0 / 3 / 4.5 +「dB/oct」，一个描边的小容器。
        class SlopeControl  : public juce::Component
        {
        public:
            SlopeControl();

            void setLabel (const juce::String& newLabel);
            float getSlope() const noexcept;
            int getNaturalWidth() const;

            std::function<void()> onChange;

            void paint (juce::Graphics&) override;
            void resized() override;

        private:
            Segmented choices { Segmented::Look::compact };
            juce::String label;
        };

        float tiltAt (float frequency) const noexcept;
        void layoutControls();
        juce::String readoutText() const;
        void updatePeakReadout();

        Language language = Language::chinese;
        std::vector<float> output, input, average, peak;   // 未加斜率的 dB（SpectrumAnalyser 的 numPoints 个点）
        std::optional<float> hoverFrequency;
        float peakFrequency = 1000.0f, peakDb = SpectrumAnalyser::floorDb;
        int framesSincePeakReadout = 1000;

        float clayLowCutHz = 0.0f, clayMonoHz = 0.0f;
        bool clayActive = false;

        LegendToggle outputToggle { LegendToggle::Swatch::gradient },
                     inputToggle { LegendToggle::Swatch::input },
                     peakToggle { LegendToggle::Swatch::peak };
        SlopeControl slopeControl;
        LoudnessHud hud;
        int readoutWidth = 250;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
    };
}
