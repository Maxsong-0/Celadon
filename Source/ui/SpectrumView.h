/*
  ==============================================================================

    SpectrumView.h
    上半部分正中的频谱仪面板（V4 设计稿 1032 × 238）与右上角的响度小窗。

      - 20 Hz ～ 20 kHz 对数横轴，−84 ～ −3 dB 纵轴；OUT 是插件最终输出（听原声时就是原声），填充按响度上色（T-036，见下）、
        白色细竖纹、往下渐隐；白线 = 平均；深青细线 = 峰值保持；虚线 = IN（链入口）。三者都可开关；
      - 显示斜率 0 / 3 / 4.5 dB/oct（绕 1 kHz 转，默认 4.5：音乐的频谱大致按这个斜率往下走，转平后各段高低一眼可比）；
      - CLAY 的低切与收单频率画成竖虚线与标签，低切以下打斜线（修坯此刻实际在处理时才画）；
      - 悬停读数：「−18.80dB | 882.32Hz | A5 + 4 音分」；鼠标不在面板上时读的是此刻最响的那个峰。

    绘制开销（T-032 R9-04 / R9-10，2026-09-30 ～ 10-01；其中竖纹单独填、dB 底框与读数文字每次照画、OUT 填充用轮廓的拷贝三处
    已被 T-036 改掉，见下节）：
      - 竖纹是一张按像素比生成的一行瓦片（条纹在设备像素上的覆盖与原来逐条 fillRect 相同），纵向平铺后一次 fillPath 填进 OUT；
        渐隐同样直接 fillPath，不再裁剪、不再 516 次 fillRect。代价：瓦片里条纹已按 8 位预乘存好，OUT 抗锯齿边上的
        条纹像素再乘一次边缘覆盖率（原来是两个覆盖率先相乘、只舍入一次），那些像素差 ≤ 2 LSB（< ×1.5）/ ≤ 1 LSB（≥ ×1.5）；
      - 面板渐变 + 网格（+ 修坯低切以下的斜线）、频率与 dB 刻度的字、读数框的影子 / 白底 / 描边按像素比缓存成图，整数像素对齐贴上；
        像素比、修坯低切、语言变了才重生成；曲线、dB 底框、修坯标记、光标与读数文字每次照画。底层是不透明的 RGB 图
        （窗口后备图是 RGB 时与直接画同一套混色算式）；其余每张图里的元素互不重叠、都往全透明上画一次：贴回去与直接画相同。
        组件原点在整数设备像素上时与改前逐像素相同，例外是上面的竖纹边、圆角上个别抗锯齿像素与修坯斜线：JUCE 的边表对
        正好 45° 的边按变换后坐标的浮点舍入选每行采样 1 次还是 2 次，改前直接画的斜线随面板在窗口里的位置变（同一面板放在
        (0, 0) 与 (84, 60) 差 ≤ 5 LSB），画进缓存后与位置无关（见 T-032 A10 实现报告）；
        落在小数像素上时（界面缩放 70 % 等）缓存图整张吸附到最近的整数像素（≤ 0.5 像素）；
      - 每点的横坐标、频率与斜率在构造 / 换斜率时算好；几条曲线的路径（OUT 只建一次，填充用它的拷贝闭合）在数据或斜率变了才重建；
      - 悬停只重画新旧光标列与读数框（读数文字变了才重画），paint 按裁剪区跳过不相交的整块
        （macOS 只拿脏区的包围盒，省得少；Linux 的软件渲染器与 Windows 的 Direct2D 按矩形列表裁剪，省得多）。

    T-036（2026-10-01，Max「更顺滑、响应更快，可以模仿 MiniMeters」）：
      - 编辑器按屏幕刷新推（垂直同步，60 / 120 帧，封顶 120），setSpectrum 带上距上一帧的秒数。读数（此刻最响的峰、悬停处）的 dB
        约 0.27 s 换一次，鼠标移动时立即换；文字拼好缓存（readoutString），变了才重画读数框；
      - OUT 填充按响度上色（像 MiniMeters）：每一列的颜色由它 ±2 像素里曲线顶的高度定——安静 = 灰青、半透明，越响越亮越饱和，
        最响是亮薄荷青；白色竖纹与往下渐隐照旧。颜色与竖纹合成一行图（prepareOutputRow），纵向平铺一次填进 OUT：原来横向渐变 +
        竖纹瓦片两遍填充，现在一遍。图例 OUT 色块同一条色带；
      - 曲线在屏上变了（按加斜率、钳位后的纵坐标比，0.1 像素）才重建、重画，只重画「新旧曲线的包围盒 ∪ 悬停光标」，安静段落
        脏区小；在 paint 里重建的那一帧（换斜率）画出的区域也并进「旧区域」。图例开关、斜率、响度小窗用对齐缓存
        （setBufferedToAlignedImage）——频谱一秒重画 60 / 120 次时它们从缓存贴、不重排文字，小数像素比下字也不糊；dB 刻度的白底框
        并进刻度那张缓存图，读数框的影子 / 白底 / 描边 / 文字并成一张。缓存图尺寸不变就复用、关掉备份（Direct2D 不回读）。
        半透明元素先画进透明图再贴，个别边缘像素与直接画差 1；
      - 曲线每个逻辑像素一个点（SpectrumAnalyser::numPoints = 1033）；OUT 填充与轮廓各自直接建路径（不拷贝）；平均线本身已沿
        频率平滑，路径隔一个点取一个、斜角接头；峰值线每两个点取较高的那个（尖峰不丢、线段少一半）；渐隐裁到 y ≥ 40 再填。

    T-058（2026-10-04，峰值线关着时不再每帧重建、重画）：
      - 只比看得见的曲线：PEAK 关着时峰值线不参与「屏上变没变」的判断（没有 IN 的曲线时 IN 也不比）。原来峰值保持往下落的
        那几秒（停播 1.5 s 后按 12 dB/s 落过整块面板，约 6 ～ 7 s；起音溅出的保持值也一样）每帧判「变了」、重建四条路径、
        重画曲线区，屏上什么都没变；
      - 计算器此刻的值每帧拷一份（latest*，只拷不比），任何一次重建（setSpectrum 判变了、换斜率或打开 PEAK 后在 paint 里）都先
        换成它再建：画出来的总是此刻的曲线。原来「画出来的那组值」既是比较基准、又是重建用的数据，只在看得出变化时刷新——
        曲线静止时换斜率，paint 里按它重建，压在底边下的旧值被斜率抬出来画一帧（T-051 证据里露出来的旧峰值保持数据就是这个）；
        只做「PEAK 关时不比」而不这样改的话，PEAK 关着停播后 OUT 与平均线停在约 0.3 s 时的样子，换斜率时同样露出来（截图工具
        S10b 抓得到）。读数（换斜率、悬停、定时刷新）也用此刻的值；
      - 打开 PEAK 的那一刻在 paint 里按此刻的值整组重建（setPeakShown），画出的区域并进旧区域（评审 R-02 ①）。截图工具 --only check
        的 S9（残影、PEAK 关时 0 次重建）与 S10（打开 PEAK、换斜率那一刻画的是此刻的数据）守这几条。

    T-073（2026-10-05，Max 拍板「按你的建议来」，T-070 评审 SV-2）：curveRegion 只在 PEAK 开着时才把峰值线算进脏区。原来
      R-02 ② 让它不论开关都算（开关切换那一帧是整面重画，之后的「旧区域」要盖住可能画过的地方）；T-058 起打开 PEAK 时的重建与 ①
      已经盖住了，② 成了第二道保险，代价是 PEAK 关着放音乐时看不见的峰值线照样进脏区（峰值保持常比 OUT 顶高几 dB）、脏区设备像素
      多 11 ～ 12 %。T-070 的红绿矩阵证明现有检查守得住打开 / 关闭 PEAK 时的重画，所以收紧；--only check 另有一条守它（PEAK 关时
      区域不含峰值线、开着时含）。buildEnvelope 仍无条件建峰值线路径：只省 517 个点（一次重建要走 3 千多个点），不值得多一条
      「峰值线路径可能是旧的」的不变量。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "SpectrumAnalyser.h"

namespace celadon::ui
{
    //==============================================================================
    // 折线的虚线描边（T-051，评审 R-07）：与 juce::PathStrokeType::createDashedStroke（单位变换、精度 1、偏移 0）同一算法、
    // 同一组浮点运算，dest 逐字节相同；区别只在切出的中线段写进调用方给的 centre（clear() 保留容量）。JUCE 那个每次在函数里
    // 新建一条 Path 放中线段、边长边重新分配——频谱仪的 IN 虚线播放中每帧重建一次，实测每次 18 次堆分配；centre、dest 用成员
    // 路径后只剩描边（createStrokedPath）里 JUCE 自己的 2 次（展平迭代器的栈、线段表；Graphics::strokePath 每次也有）。
    // line 只能由 startNewSubPath / lineTo 组成（频谱仪 buildLine 建的折线）；截图工具 --only check 拿它与 JUCE 的逐字节比。
    void createDashedStrokeReusing (const juce::PathStrokeType& stroke, const juce::Path& line,
                                    const float* dashLengths, int numDashLengths, juce::Path& centre, juce::Path& dest);

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

        // 每帧调（计算器 update() 之后），elapsedSeconds = 距上一帧的秒数：从计算器拷出几条曲线，看得见的曲线有看得出的变化
        // 才重建、重画（只重画新旧曲线的包围盒；PEAK 关着时不看峰值线，T-058）。顺带把 IN 虚线开没开告诉计算器——关着时它跳过
        // IN 那路的加窗 + FFT（T-032 R7-05）；刚打开时计算器下一次 update 才有 IN 的曲线，这之前不画 IN。
        void setSpectrum (SpectrumAnalyser& analyser, double elapsedSeconds);

        // CLAY 此刻的实际频率（0 = 没在处理）；active = 修坯没被旁通。
        void setClayMarkers (float lowCutHz, float monoHz, bool active);

        LoudnessHud& getLoudnessHud() noexcept   { return hud; }

        // 只读，给截图工具的检查用（T-070）：路径重建过多少次（buildCurvePaths 每次 +1）——「PEAK 关、只剩峰值在落时不重建」
        // 要数真的重建，不能拿脏区当代理；此刻读数框里的文字——悬停时读数必须是光标处此刻的值，工具按计算器的值独立算一份对。
        std::uint32_t getCurveBuildCount() const noexcept   { return curveBuilds; }
        const juce::String& getReadoutText() const noexcept   { return readoutString; }

        // 此刻几条曲线（含描边外伸）占的区域，增量重画按它标脏（T-073：PEAK 关着时不含峰值线）。
        juce::Rectangle<float> getCurveRegion() const   { return curveRegion(); }

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
            dbLabels,           // dB 刻度：白色底框 + 字（T-036 起底框也进缓存）
            readout,            // 读数框：影子 + 白底 + 描边 + 文字（文字变了才重画，T-036）
            count
        };

        float tiltAt (float frequency) const noexcept;
        void updateTilt();
        void setPeakShown (bool shouldShow);
        void updateMarkerLabels();
        void layoutControls();
        juce::String readoutTextFor (float frequency, float db) const;
        void refreshReadout();
        void updatePeakReadout (const std::vector<float>& values);
        static float curveDbAt (const std::vector<float>& values, float frequency) noexcept;
        void setHoverFrequency (std::optional<float> frequency);
        juce::Rectangle<int> cursorBounds() const;
        juce::Rectangle<float> readoutBox() const noexcept;

        void buildLine (const std::vector<float>& values, juce::Path& path, int step = 1) const;
        void buildEnvelope (const std::vector<float>& values, juce::Path& path) const;
        void buildCurvePaths();
        void buildColumnLevels();
        void prepareOutputRow (float pixelScale);
        juce::Rectangle<float> curveRegion() const;
        void repaintCurves();
        CachedLayer& layer (Layer which) noexcept                     { return layers[(size_t) which]; }
        void invalidate (Layer which) noexcept                        { layer (which).valid = false; }
        juce::Rectangle<int> layerArea (Layer which) const;
        void paintLayer (Layer which, juce::Graphics&) const;
        void prepareLayers (float pixelScale);
        void prepareStripes (float pixelScale, juce::Colour colour);
        void drawLayer (juce::Graphics&, Layer which);

        Language language = Language::chinese;
        // 曲线的值都是未加斜率的 dB（SpectrumAnalyser 的 numPoints 个点）。画出来的那组：路径按它建，也是 setSpectrum 判「屏上
        // 变没变」的基准，只在重建时更新；latest* = 计算器此刻的值，每帧拷（只拷不比），重建时先换进画出来的那组（T-058）。
        std::vector<float> output, input, average, peak;
        std::vector<float> latestOutput, latestInput, latestAverage, latestPeak;
        std::array<float, SpectrumAnalyser::numPoints> pointX {}, pointFrequency {}, tilt {};   // 每点的横坐标、频率、斜率（dB）
        std::optional<float> hoverFrequency;
        float peakFrequency = 1000.0f, peakDb = SpectrumAnalyser::floorDb;
        double secondsSinceReadout = 1.0e9;                         // 距上次刷新读数（最响的峰 / 悬停处的 dB）的秒数
        float hoverDb = SpectrumAnalyser::floorDb;                  // 悬停处 OUT 的值（未加斜率），与读数同节奏刷新
        juce::String readoutString;                                 // 此刻该显示的读数文字（refreshReadout 拼好）
        bool inputReady = false;         // 计算器此刻有 IN 的曲线（IN 开着、且打开以来算过至少一帧）
        juce::String shownReadout;       // 读数框缓存图里的文字：与此刻的读数不同就重画那张图

        float clayLowCutHz = 0.0f, clayMonoHz = 0.0f;
        bool clayActive = false;
        juce::String lowCutLabel, monoLabel;
        float lowCutLabelWidth = 0.0f, monoLabelWidth = 0.0f;
        std::array<juce::Rectangle<float>, 6> dbChips;               // −12 … −72 dB 刻度的白色底框（构造时量好）

        juce::Path panel;                                            // 圆角面板（裁剪与描边）
        juce::Path outputLine, outputFill, averageLine, peakLine;   // 加了斜率的曲线；outputFill = outputLine 闭合到底边
        juce::Path inputLine, inputDashes;                           // IN 的折线与虚线（虚线已展开成填充路径，画的时候才展开）
        juce::Path inputDashCentre;                                  // 展开虚线时切出的中线段（只为复用容量，见 createDashedStrokeReusing）
        std::array<float, SpectrumAnalyser::numPoints> columnLevel {};   // 每个逻辑像素 OUT 填充的颜色位置（0 = 底边、1 = 顶边）
        juce::Image outputRow;                                       // OUT 填充的一行：按响度上色 + 白色竖纹（曲线变了才重建）
        float outputRowScale = 0.0f;
        bool outputRowValid = false;
        juce::Rectangle<float> curveArea;                            // 上一次重画时曲线占的区域（下一帧与新区域合起来标脏）
        bool curvePathsValid = false, inputDashesValid = false;
        std::uint32_t curveBuilds = 0;                               // 路径重建次数（只读访问器 getCurveBuildCount，给截图工具的检查用）

        std::array<CachedLayer, (size_t) Layer::count> layers;
        juce::Image stripeRow;                                       // 竖纹瓦片：一行，宽 = 面板宽 × 像素比（T-036 起只用来量覆盖率）
        std::vector<float> stripeAlpha;                              // ……每个设备像素列的条纹不透明度（合成 outputRow 用）
        float stripeRowScale = 0.0f;
        juce::Colour stripeRowColour;

        // PEAK 一律经 setPeakShown 开关（打开那一刻要按此刻的值重建峰值线，T-058），别直接 peakToggle.setOn。
        LegendToggle outputToggle { LegendToggle::Swatch::gradient },
                     inputToggle { LegendToggle::Swatch::input },
                     peakToggle { LegendToggle::Swatch::peak };
        SlopeControl slopeControl;
        LoudnessHud hud;
        int readoutWidth = 250;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
    };
}
