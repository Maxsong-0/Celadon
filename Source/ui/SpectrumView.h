/*
  ==============================================================================

    SpectrumView.h
    上半部分正中的频谱仪面板（V4 设计稿 1032 × 238）与右上角的响度小窗。

      - 20 Hz ～ 20 kHz 对数横轴，−84 ～ −3 dB 纵轴；OUT 是插件最终输出（听原声时就是原声），填充用单色青的横向渐变、
        62 % 不透明、白色细竖纹、往下渐隐；白线 = 平均；深青细线 = 峰值保持；虚线 = IN（链入口）。三者都可开关；
      - 显示斜率 0 / 3 / 4.5 dB/oct（绕 1 kHz 转，默认 4.5：音乐的频谱大致按这个斜率往下走，转平后各段高低一眼可比）；
      - CLAY 的低切与收单频率画成竖虚线与标签，低切以下打斜线（修坯此刻实际在处理时才画）；
      - 悬停读数：「−18.80dB | 882.32Hz | A5 + 4 音分」；鼠标不在面板上时读的是此刻最响的那个峰。

    绘制开销（T-032 R9-04 / R9-10，2026-09-30 ～ 10-01）：
      - 竖纹是一张按像素比生成的一行瓦片（条纹在设备像素上的覆盖与原来逐条 fillRect 相同），纵向平铺后一次 fillPath 填进 OUT；
        渐隐同样直接 fillPath，不再裁剪、不再 516 次 fillRect。代价：瓦片里条纹已按 8 位预乘存好，OUT 抗锯齿边上的
        条纹像素再乘一次边缘覆盖率（原来是两个覆盖率先相乘、只舍入一次），那些像素差 ≤ 2 LSB（×1）/ ≤ 1 LSB（×2 以上）；
      - 面板渐变 + 网格（+ 修坯低切以下的斜线）、频率与 dB 刻度的字、读数框的影子 / 白底 / 描边按像素比缓存成图，整数像素对齐贴上；
        像素比、修坯低切、语言变了才重生成；曲线、dB 底框、修坯标记、光标与读数文字每次照画。底层是不透明的 RGB 图
        （窗口后备图是 RGB 时与直接画同一套混色算式）；其余每张图里的元素互不重叠、都往全透明上画一次：贴回去与直接画相同。
        组件原点在整数设备像素上时与改前逐像素相同（除上面的竖纹边与个别抗锯齿像素，见 T-032 A10 实现报告）；
        落在小数像素上时（界面缩放 70 % 等）缓存图整张吸附到最近的整数像素（≤ 0.5 像素）；
      - 每点的横坐标、频率与斜率在构造 / 换斜率时算好；几条曲线的路径（OUT 只建一次，填充用它的拷贝闭合）在数据或斜率变了才重建；
      - 悬停只重画新旧光标列与读数框（读数文字变了才重画），paint 按裁剪区跳过不相交的整块
        （macOS 只拿脏区的包围盒，省得少；Linux / Windows 的软件渲染器按矩形列表裁剪，省得多）。

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
        static constexpr int height = 238;
        static constexpr float topDb = -3.0f;
        static constexpr float bottomDb = -84.0f;

        void setLanguage (Language newLanguage);

        // 每帧调（计算器 update() 之后）：从计算器拷出几条曲线，有看得出的变化才重画。顺带把 IN 虚线开没开告诉计算器——
        // 关着时它跳过 IN 那路的加窗 + FFT（T-032 R7-05）；刚打开时计算器下一次 update 才有 IN 的曲线，这之前不画 IN。
        void setSpectrum (SpectrumAnalyser& analyser);

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

        // 按物理像素比缓存的一块静态画面。area = 内容所在的逻辑区域；origin = 图左上角的设备像素位置（相对组件原点，
        // = ⌊area 左上 × 像素比⌋）：图里每个像素与组件的设备像素网格对齐，贴图时变换只剩整数平移。
        struct CachedLayer
        {
            juce::Rectangle<int> area;
            juce::Point<int> origin;
            juce::Image image;
            float pixelScale = 0.0f;
            bool valid = false;
        };

        // 各张缓存图（按叠放顺序）。一张图里的元素互不重叠，见文件头。
        enum class Layer
        {
            back,               // 面板渐变 + 网格 + 修坯低切以下的斜线（不透明的 RGB 图，贴时按面板圆角裁剪）
            frequencyText,      // 100Hz / 1kHz / 10kHz
            dbText,             // dB 刻度的字（白色底框每次直接画，见 paint）
            readoutShadow,      // 读数框的影子
            readoutBox,         // 读数框的白底
            readoutBorder,      // 读数框的描边
            count
        };

        float tiltAt (float frequency) const noexcept;
        void updateTilt();
        void updateMarkerLabels();
        void layoutControls();
        juce::String readoutText() const;
        void updatePeakReadout();
        void setHoverFrequency (std::optional<float> frequency);
        juce::Rectangle<int> cursorBounds() const;
        juce::Rectangle<float> readoutBox() const noexcept;

        void buildLine (const std::vector<float>& values, juce::Path& path) const;
        void buildCurvePaths();
        CachedLayer& layer (Layer which) noexcept                     { return layers[(size_t) which]; }
        void invalidate (Layer which) noexcept                        { layer (which).valid = false; }
        juce::Rectangle<int> layerArea (Layer which) const;
        void paintLayer (Layer which, juce::Graphics&) const;
        void prepareLayers (float pixelScale);
        void prepareStripes (float pixelScale, juce::Colour colour);
        void drawLayer (juce::Graphics&, Layer which);

        Language language = Language::chinese;
        std::vector<float> output, input, average, peak;   // 未加斜率的 dB（SpectrumAnalyser 的 numPoints 个点）
        std::array<float, SpectrumAnalyser::numPoints> pointX {}, pointFrequency {}, tilt {};   // 每点的横坐标、频率、斜率（dB）
        std::optional<float> hoverFrequency;
        float peakFrequency = 1000.0f, peakDb = SpectrumAnalyser::floorDb;
        int framesSincePeakReadout = 1000;
        bool inputReady = false;         // 计算器此刻有 IN 的曲线（IN 开着、且打开以来算过至少一帧）
        juce::String shownReadout;       // 上次画出来的读数文字：悬停时文字变了才重画读数框

        float clayLowCutHz = 0.0f, clayMonoHz = 0.0f;
        bool clayActive = false;
        juce::String lowCutLabel, monoLabel;
        float lowCutLabelWidth = 0.0f, monoLabelWidth = 0.0f;
        std::array<juce::Rectangle<float>, 6> dbChips;               // −12 … −72 dB 刻度的白色底框（构造时量好）

        juce::Path panel;                                            // 圆角面板（裁剪与描边）
        juce::Path outputLine, outputFill, averageLine, peakLine;   // 加了斜率的曲线；outputFill = outputLine 闭合到底边
        juce::Path inputDashes;                                      // IN 的虚线（已展开成填充路径）
        bool curvePathsValid = false, inputDashesValid = false;

        std::array<CachedLayer, (size_t) Layer::count> layers;
        juce::Image stripeRow;                                       // 竖纹瓦片：一行，宽 = 面板宽 × 像素比
        float stripeRowScale = 0.0f;
        juce::Colour stripeRowColour;

        LegendToggle outputToggle { LegendToggle::Swatch::gradient },
                     inputToggle { LegendToggle::Swatch::input },
                     peakToggle { LegendToggle::Swatch::peak };
        SlopeControl slopeControl;
        LoudnessHud hud;
        int readoutWidth = 250;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
    };
}
