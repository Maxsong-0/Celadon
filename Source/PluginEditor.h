/*
  ==============================================================================

    PluginEditor.h
    界面组装（设计规格-界面 第 3 节；ADR-005）：「梅子青夜窑」布局、等比缩放、30 ms 刷新表。

    结构：
      - 全部子组件放在一块 900 × 560 的画布（Canvas）上，按基准坐标布局一次、永不改动；
      - 整块画布用 AffineTransform 等比缩放（80% ～ 150%），拖右下角改大小；
        比例存进 APVTS 状态树的 uiScale 属性（不是参数、不进自动化），重开工程保持；
      - 定时器每 30 ms 从处理器的原子变量读表与 Balance 的 27 段修正量，不持锁、不阻塞音频线程；
      - GLAZE 总旋钮绑定参数 glaze_amount；v0.6（ADR-008，设计规格-GLAZE-界面与实现 第 1 节）在它左边加一列
        「风格」四选一胶囊（参数 glaze_style）+「重新聆听」按钮，旋钮下方原来的短说明换成状态行；
      - GLAZE 的三样运行时数据经注入点取（见下面 setGlaze…Source）：状态快照（状态行、按钮文字）、「重新聆听」动作、
        7 个受 GLAZE 影响的旋钮（Recover、Tame、Tone、Glue、Heat、Gain、Clip）此刻交给模块的实际值（淡弧 + 小圆点）。
        编辑器只认这三个函数，不关心背后的实现（v0.6 起是处理器转交的 GLAZE 编排器）；
      - 顶栏右侧版本号取自 JucePlugin_VersionString（Projucer 按 .jucer 的版本生成），不手写。
      - v0.1.7.0 修坯 Clay（设计规格-Clay 第 5 节；ADR-009）：定稿布局不动，只加三样——
          · BALANCE → COLOR 箭头上的三灯指示（ClayIndicator）：低切 / 收单 / 加宽此刻是否在处理、保护是否介入，悬停气泡第 2 行是实时值；
          · 顶栏版本号左边的「高级」胶囊（72 × 22，样式同开关）：点开 / 收起高级抽屉；开合不存、不是参数，每次打开编辑器默认收起；
          · 高级抽屉（AdvancedDrawer）：盖在下半区三块面板上方（同位置同大小），120 ms 淡入淡出；淡入到位后把被盖住的三块面板与
            指示灯藏起来（不再白画），开始收起时先亮回来。
        Clay 的实际值与保护状态走状态源（glaze::Status 的 clay* 字段），与状态行同一次取的快照；三态与旁通从 APVTS 的原子指针读。

    v0.5 界面性能（研究 P5 R2 ～ R5）：macOS 把一帧里所有脏矩形合成一个外接矩形整块重画，表一动，外接矩形里的
    背景、GLAZE 大旋钮都跟着重画。所以：画布的静态层（底色 + 开片纹 + 固定文字 + 箭头）缓存成一张不透明图、按屏幕
    像素 1:1 贴（R3）；编辑器不再在画布底下重复填底（R2）；GLAZE 大旋钮缓存成图（R4）；表、曲线、读数只在画面上
    真的会变（半个屏幕像素 / 显示的文字）时才重画（R5，在各组件里）。外观与 v0.4 一致；Debug 构建顶栏中间多一行红字。
    v0.6 新加的风格列与状态行照同一套办法：内容变了才重画自己，平时被外接矩形扫到只贴像素对齐的缓存图。

    只依赖 ui/ 下各组件的公开接口与 dsp/ 的数据类型（ChainParams、glaze::Status）；GLAZE 的运行时数据全部经处理器注入。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>

#include "PluginProcessor.h"
#include "dsp/ChainParams.h"
#include "dsp/GlazeTypes.h"
#include "ui/AdvancedDrawer.h"
#include "ui/BalanceCurve.h"
#include "ui/CeladonLookAndFeel.h"
#include "ui/ClayIndicator.h"
#include "ui/GlazeStatusLine.h"
#include "ui/GlazeStyleColumn.h"
#include "ui/HelpText.h"
#include "ui/Knob.h"
#include "ui/LoudnessReadout.h"
#include "ui/MeterBar.h"
#include "ui/ModulePanel.h"

//==============================================================================
class CeladonAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                     private juce::Timer,
                                     private juce::ValueTree::Listener,
                                     private juce::AsyncUpdater
{
public:
    explicit CeladonAudioProcessorEditor (CeladonAudioProcessor&);
    ~CeladonAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    // 基准尺寸与缩放范围（设计规格-界面 第 3 节：默认 900 × 560，等比 80% ～ 150%）。
    static constexpr int baseWidth  = 900;
    static constexpr int baseHeight = 560;
    static constexpr float minScale = 0.8f;
    static constexpr float maxScale = 1.5f;

    //==============================================================================
    // GLAZE 的运行时数据：三个注入点（v0.6，设计规格-GLAZE-界面与实现 第 1 节）。都只在消息线程调用，每次调用都要快
    // （状态源与实际值源每 30 ms 各调一次），且不能等音频线程——实现方从原子变量 / 无锁快照读。
    //
    //   状态源        → 状态行（只显示最要紧的一个）与「重新聆听」按钮的文字（relearnInProgress）；
    //   重新聆听动作  → 点按钮时调：未在重学 = 开始聆听，正在重学 = 取消（旧方案不受影响）；
    //   实际值源      → 本块交给三个模块的实际参数。编辑器只读受 GLAZE 影响的 7 项（balance.recover / tame、
    //                   color.toneDb / glue / heat、limit.gainDb / clip，Limit Gain 含校准偏差与保护偏移），其余字段不看。
    //
    // 处理器在 createEditor 里注入（v0.6 阶段 2：编排器的无锁状态快照、「重新聆听」原子标志、音频线程每块写的实际值）。
    // 注入之前（与传空函数时）：状态按「未学习」、按钮不做事、不画实际值标记。换源后立刻按新源刷新一次，不等下一次定时器。
    using GlazeStatusSource          = std::function<celadon::glaze::Status()>;
    using GlazeRelearnAction         = std::function<void()>;
    using GlazeEffectiveParamsSource = std::function<celadon::ChainParams()>;

    void setGlazeStatusSource (GlazeStatusSource newSource);
    void setGlazeRelearnAction (GlazeRelearnAction newAction);
    void setGlazeEffectiveParamsSource (GlazeEffectiveParamsSource newSource);

private:
    //==============================================================================
    // 画布：承载全部子组件的 900 × 560 内容层。自己只画底色、开片纹、顶栏文字、
    // GLAZE 标题与模块间的小箭头；子组件由编辑器摆放。
    class Canvas final : public juce::Component
    {
    public:
        Canvas();
        ~Canvas() override;

        void paint (juce::Graphics&) override;

        // 顶栏中间（操作提示 / NaN 警告 / Debug 构建提醒）与右边（版本 · 延迟）的文字。变了才重画，且只重画顶栏。
        void setTopBarTexts (const juce::String& centreText, bool centreIsWarning, const juce::String& rightText);

        // 静态层与它的物理像素比（还没画过时图无效）。GLAZE 大旋钮的缓存图拿它当底：旋钮下面只有静态层（v0.5 R4）。
        const juce::Image& getStaticLayer() const noexcept   { return staticLayer; }
        float getStaticLayerScale() const noexcept          { return staticLayerScale; }

    private:
        // 静态层（v0.5 R3）：底色 + 开片纹 + 顶栏标题 + GLAZE 标题 + 模块间箭头，按这次绘制的物理像素比渲染成一张
        // 不透明图。物理像素比 = 画布到屏幕像素的缩放（界面缩放 × Retina 等），变了（拖大小、换屏幕）才重生成；
        // paint 里按 1:1 贴回（图的每个像素正好落在一个屏幕像素上，不重采样），再画顶栏中、右两段会变的文字。
        void refreshStaticLayerIfNeeded (float pixelScale);
        static void paintStaticText (juce::Graphics&);

        juce::String centreText, rightText;
        bool centreIsWarning = false;

        juce::Image staticLayer;
        float staticLayerScale = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Canvas)
    };

    //==============================================================================
    void timerCallback() override;

    // 宿主在界面打开时换掉整棵状态树（读工程 / 换预设）：把存的缩放比例重新应用到窗口。
    // 回调可能来自任意线程，所以只投递一个异步更新，真正改窗口在消息线程做。
    void valueTreeRedirected (juce::ValueTree&) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void handleAsyncUpdate() override;

    void buildOverview();
    void buildModules();
    void layoutCanvas();
    void updateTopBar();
    void resetMeters();

    // buildModules 里每加一个受 GLAZE 影响的旋钮就登记一次：按参数 ID 取「从实际参数里取这一项」的函数与参数对象
    // （换算比例用）。找不到都是 ID / 7 项表 / 参数表脱节，Debug 版断住。
    void rememberGlazedKnob (celadon::ui::Knob& knob, const char* parameterId);

    // 定时器每 30 ms：GLAZE > 0 时从实际值源取本块的实际参数，7 项各换成该参数 0 ～ 1 刻度上的比例喂给旋钮；
    // GLAZE 为 0 时不画标记（规格第 1 节）。
    void updateEffectiveValues();

    // 定时器每 30 ms：从状态源取快照，更新状态行与「重新聆听」按钮（内容没变时两者都不重画）；
    // 同一份快照再交给修坯（updateClay），不另取一次。
    void updateGlazeStatus();

    // 修坯（v0.1.7.0）：快照里的 Clay 实际值 + 参数里的三态与旁通 → 量化读数（help::clayReadoutFor），喂三灯指示与抽屉。
    // 两者都只在显示内容变了时重画。
    void updateClay (const celadon::glaze::Status& status);

    // 「高级」胶囊 / 抽屉右上角 ✕：开合抽屉（胶囊的亮灭跟着抽屉走）。
    void setAdvancedDrawerOpen (bool shouldBeOpen);

    // 抽屉淡入到位 / 开始收起时由抽屉回调：藏起 / 亮回被它盖住的三块模块面板与修坯指示灯。
    void setModulesCovered (bool covered);

    // 「高级」胶囊的位置：版本号左边，按「最宽的版本 · 延迟文字」算一次，延迟数字变了胶囊也不挪。
    juce::Rectangle<int> advancedButtonBounds() const;

    // 点「重新聆听」：调注入的动作，然后立刻刷新一次状态（不等下一次定时器）。
    void relearnClicked();

    float readStoredScale() const;
    void applyStoredScale();
    void storeScale (double scale);

    //==============================================================================
    // 声明顺序 = 构造顺序，析构相反：LookAndFeel 最先声明（最后析构），所有用它的组件都先于它销毁；
    // 画布先于挂在它上面的子组件与气泡声明。
    celadon::ui::CeladonLookAndFeel lookAndFeel;
    CeladonAudioProcessor& processor;

    const juce::Identifier uiScaleId { celadon::uiid::uiScaleProperty };
    bool writingScale = false;      // 自己写 uiScale 属性时为 true，免得回调再触发一轮

    Canvas canvas;

    // ---- 总览区 ----
    celadon::ui::MeterBar inputBar     { "IN",  -60.0f, 3.0f,  false };
    celadon::ui::MeterBar outputBar    { "OUT", -60.0f, 3.0f,  false };
    celadon::ui::MeterBar reductionBar { "GR",  0.0f,   24.0f, true };

    // GLAZE 总旋钮（v0.4 接通）：绑定参数 glaze_amount，可拖、可自动化；提示由 Knob 按参数 ID 自动取。
    // 名字由画布画在旋钮下方（「GLAZE 施釉」），所以这里的显示名留空。
    // processor 在上面先声明（成员按声明顺序构造），这里才能拿它的 APVTS。
    celadon::ui::Knob glazeKnob { processor.getParameters(), celadon::param::glazeAmount, juce::String(),
                                  celadon::ui::Knob::Size::big };

    // GLAZE 左边一列（v0.6）：风格四选一（绑定 glaze_style）+「重新聆听」；旋钮下方的状态行（v0.6，取代 v0.4 的短说明）。
    celadon::ui::GlazeStyleColumn glazeStyleColumn { processor.getParameters() };
    celadon::ui::GlazeStatusLine glazeStatusLine;

    celadon::ui::LoudnessReadout readout;

    // ---- 模块区 ----
    // Glue GR 小表是 COLOR 面板的标题附件，面板不拥有它：先于面板声明，保证面板先销毁、不会引用到已死的表。
    celadon::ui::MeterBar glueBar { "GR", 0.0f, 12.0f, true, celadon::ui::MeterBar::Style::horizontalMini };

    // 27 段修正曲线（v0.3）挂在 BALANCE 面板的曲线带里，面板不拥有它：同样先于面板声明，面板先销毁。
    // 定时器每 30 ms 从处理器读 27 个原子变量喂给它。
    celadon::ui::BalanceCurve balanceCurve;

    celadon::ui::ModulePanel balancePanel { "BALANCE" };
    celadon::ui::ModulePanel colorPanel   { "COLOR" };
    celadon::ui::ModulePanel limitPanel   { "LIMIT" };

    // ---- 修坯（v0.1.7.0）----
    // 三灯指示：BALANCE → COLOR 之间的箭头上方（画布子组件，在面板之后加入）。
    celadon::ui::ClayIndicator clayIndicator;

    // 高级抽屉：盖在三块面板上方（画布子组件，最后加入、在它们之上）。绑定 7 个 clay_* 参数，所以放在 processor 之后声明。
    // 回调里会碰面板与指示灯：抽屉先于它们析构（声明在后），析构时不会回调。
    celadon::ui::AdvancedDrawer advancedDrawer { processor.getParameters() };

    // 顶栏「高级」胶囊（开关样式 72 × 22）：亮 = 抽屉开着。不是参数、不存。
    juce::ToggleButton advancedButton;

    // 提示气泡：挂在画布上——跟着界面一起缩放，又永远画在插件窗口之内；悬停 0.5 s 出现。
    // LookAndFeel 沿父组件链继承自编辑器，就是上面那个 CeladonLookAndFeel。
    juce::TooltipWindow tooltipWindow { &canvas, 500 };

    // ---- 受 GLAZE 影响的旋钮（7 项：Recover、Tame、Tone、Glue、Heat、Gain、Clip = glaze::AutoIncrements 的 7 个字段）----
    // 旋钮本身由各面板拥有，这里只记指针：定时器在析构函数里先停，之后才按声明的相反顺序销毁面板，指针不会悬空。
    using EffectiveValueGetter = float (*) (const celadon::ChainParams&) noexcept;

    struct GlazedKnob
    {
        celadon::ui::Knob* knob = nullptr;                    // 面板拥有
        EffectiveValueGetter effectiveValue = nullptr;       // 从「交给模块的实际参数」里取这一项
        juce::RangedAudioParameter* parameter = nullptr;     // 实际值 → 0 ～ 1 比例：按参数自己的刻度换算
    };

    // 项数 = 自动增量的字段数：以后 AutoIncrements 加项而界面忘了登记，PluginEditor.cpp 的 static_assert 与构造末尾的
    // jassert 会立刻指出。
    static constexpr std::size_t numGlazedKnobs = 7;
    std::array<GlazedKnob, numGlazedKnobs> glazedKnobs {};
    std::size_t numGlazedKnobsRegistered = 0;

    std::atomic<float>* glazeRawValue = nullptr;        // glaze_amount 的原子指针（0 ～ 100 %）
    std::atomic<float>* glazeStyleRawValue = nullptr;   // glaze_style：选项下标的 float（状态行写「自动→温润」要知道选的哪种）
    std::atomic<float>* listenRawValue = nullptr;       // limit_listen：状态行区分「校准暂停」的原因（Listen / 旁通）

    // 修坯（v0.1.7.0）：三态（选项下标的 float，= celadon::ClayMode）与旁通。三灯指示据此判断保护是否算数（保护只管「自动」态）、
    // 旁通时全暗。顺序 = help::ClayStage（低切 / 收单 / 加宽）。
    std::array<std::atomic<float>*, static_cast<std::size_t> (celadon::help::numClayStages)> clayModeRawValues {};
    std::atomic<float>* clayBypassRawValue = nullptr;

    // ---- GLAZE 数据源（见 setGlaze…Source）----
    GlazeStatusSource glazeStatusSource;
    GlazeRelearnAction glazeRelearnAction;
    GlazeEffectiveParamsSource glazeEffectiveParamsSource;

    // ---- 表的显示状态（只在消息线程读写）----
    float outputPeakHoldDb   = celadon::LoudnessMeter::noReading;   // 复位以来 OUT 真峰值最大值
    float reductionDisplayDb = 0.0f;                                // 响度区 GR 数字（带下落）

    // 顶栏上次据以拼字符串的两个数：都没变就不再每 30 ms 拼一遍字符串（只在消息线程读写）。
    int topBarBlockedCount = -1;
    double topBarLatencyMs = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CeladonAudioProcessorEditor)
};
