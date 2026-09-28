/*
  ==============================================================================

    AdvancedDrawer.h
    「高级」抽屉（设计规格-Clay 第 5 节；ADR-009 第 4、5 条）：顶栏「高级」胶囊打开，盖在下半区三块模块面板上方（同位置同大小
    868 × 232），深色面板底，右上角 ✕ 收起。四个子项目共用，四列：

      声像（修坯）  三行「名字 + 三态胶囊 [自动 | 关 | 手动] + 34 px 小旋钮 + 数值」（低切 / 收单 / 加宽），最后一行「Bypass 修坯」开关；
      推响不刺 / 目标频谱 / 发行   子项目 2 ～ 4 还没做：按 ADR-005 灰显 35%、写「即将推出」。

    三态（ADR-009 第 4 条）：
      - 自动（默认）：实际值由决策器 + 实时保护给出，GLAZE 为 0 时为 0。旋钮可拖但不生效（它存的是「手动」要用的值），
        旋钮上用 v0.4 的实际值标记（淡弧 + 小圆点，Knob::setEffectiveProportion）显示此刻交给 ClayModule 的实际值；
        该级此刻没在处理（低切关、收单比例 0、加宽 0 dB）或修坯被旁通时不画标记——「没有小圆点 = 自动此刻不处理」，
        与三灯指示的「灯暗」同一个判定。
      - 关：永远不处理。旋钮与数值 35% 暗（仍可拖：改的是「手动」时要用的值）。
      - 手动：严格按旋钮值；正常亮度，不画标记（实际值就是指针所指）。

    开合：120 ms 淡入淡出（自己的计时器按真实时间推 alpha，见 setOpen）；开合状态不存、不是参数，每次打开编辑器默认收起。
    不用 ComponentAnimator：它淡出时换成组件快照（ProxyComponent）再按组件尺寸拉伸贴回，界面缩放不是整数倍时会重采样发虚
    （PluginEditor.cpp「PixelAlignedImageCache」一节说的同一个问题），而且没有「淡完了」的回调——这里淡入到位要通知编辑器
    藏起被盖住的面板。淡入到位后抽屉完全盖住下面三块面板，经 onCoverageChanged 让编辑器把它们藏起来（盖住期间不必再画）；
    开始收起时先让面板可见，淡出时从抽屉底下透出来。

    只依赖 ui/ 的公开接口（Knob、CapsuleSelector 的尺寸常数、HelpText、CeladonLookAndFeel、ClayIndicator 的动态气泡）与
    dsp/ 的数据类型（glaze::Status、ClayMode）；参数全部经 APVTS 绑定。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>

#include "../dsp/GlazeTypes.h"
#include "ClayIndicator.h"
#include "HelpText.h"

namespace celadon::ui
{
    class AdvancedDrawer  : public juce::Component,
                            private juce::Timer
    {
    public:
        explicit AdvancedDrawer (juce::AudioProcessorValueTreeState& state);
        ~AdvancedDrawer() override;

        // 打开 / 收起（消息线程）：从此刻的不透明度起淡入 / 淡出，整程 120 ms（半路反向按剩下的比例缩短，不跳）。
        // 收起完成后组件不可见；打开时第一帧就按最新的状态快照画实际值标记。
        void setOpen (bool shouldBeOpen);
        bool isOpen() const noexcept       { return open; }

        // 淡入到位、完全盖住下半区时为 true；开始收起时回到 false。
        bool isCovering() const noexcept   { return covering; }

        // 点右上角 ✕ 时调（编辑器据此弹起「高级」胶囊、收起抽屉）。
        std::function<void()> onCloseClicked;

        // isCovering() 变了时调，参数 = 新值：true = 下面三块面板已被完全盖住（编辑器把它们与修坯指示灯藏起来，不再白画）；
        // false = 马上要露出来（编辑器先让它们可见，淡出时透得出来）。
        std::function<void (bool)> onCoverageChanged;

        // 编辑器定时器每 30 ms：编排器的状态快照（clay* = 上一块交给 ClayModule 的实际值）与同一份量化读数（旁通、该级是否在处理）。
        // 抽屉可见时更新三个旋钮的实际值标记（变化超过阈值才重画，见 Knob::setEffectiveProportion）；读数交给「声像（修坯）」
        // 标题的动态气泡（悬停时才拼字）；旧工程提示（琥珀小字）只在出现 / 消失时重画那一小块。
        void setClayStatus (const glaze::Status& newStatus, const help::ClayReadout& newReadout);

        void paint (juce::Graphics&) override;
        void resized() override;

        static constexpr int fadeMilliseconds = 120;   // 规格：开合 120 ms 淡入淡出
        static constexpr int fadeTimerHz      = 60;    // 淡入淡出期间的刷新率（约 7 帧；按真实经过的时间算 alpha，不怕计时器抖动）

    private:
        class ClayRow;   // 一行：名字 + 三态胶囊 + 小旋钮 + 数值（定义在 AdvancedDrawer.cpp）

        void timerCallback() override;
        void setCovering (bool shouldCover);
        void updateMarkers();

        // 固定文字：构造时转一次 UTF-8，paint 里不再每次转。
        const juce::String clayTitle, relearnHint, comingSoonText;
        juce::StringArray placeholderTitles;

        std::array<std::unique_ptr<ClayRow>, static_cast<std::size_t> (help::numClayStages)> rows;   // 低切 / 收单 / 加宽
        juce::ToggleButton bypassButton;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;   // 在按钮之后声明：先于按钮析构
        std::unique_ptr<juce::Button> closeButton;          // 右上角 ✕（具体类型在 AdvancedDrawer.cpp）

        ClayLiveTooltip liveTooltip;                        // 下面的标题悬停区引用它：先声明（先构造、后析构）
        std::unique_ptr<juce::Component> clayTitleRegion;  // 「声像（修坯）」标题的透明悬停区：出与三灯指示同一个动态气泡
                                                            // （抽屉打开时指示灯被盖住，这里补一个看实时值的入口）

        glaze::Status status;          // 最近一次的状态快照（收起期间也记着：打开时第一帧就用它）
        help::ClayReadout readout;     // 同一次的量化读数

        bool open = false;
        bool covering = false;
        float fadeFromAlpha = 0.0f;
        float fadeToAlpha = 0.0f;
        juce::uint32 fadeStartMs = 0;
        juce::uint32 fadeDurationMs = 0;

        // resized 算好、paint 直接用的区域（抽屉自己的坐标）。
        static constexpr int numPlaceholders = 3;
        juce::Rectangle<int> clayTitleArea, relearnArea;
        std::array<juce::Rectangle<int>, static_cast<std::size_t> (numPlaceholders)> placeholderAreas;
        std::array<int, static_cast<std::size_t> (numPlaceholders)> separatorX {};
        int separatorTop = 0, separatorBottom = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedDrawer)
    };
}
