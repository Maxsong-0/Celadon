/*
  ==============================================================================

    ClayIndicator.h
    修坯三灯指示（设计规格-Clay 第 5 节；ADR-009 第 5 条）：BALANCE → COLOR 两块面板之间的小箭头上方，竖排三盏直径 5 px 的灯，
    自上而下 = 修坯的三级「低切 / 收单 / 加宽」（= ClayModule 的处理顺序）。

      - 颜色：该级实际值非 0 → 梅子青；该级的实时保护正在介入（反相保护对「单」、宽度保护对「宽」）→ 琥珀（与 GR 表同色：
        「在压 / 保护中」同一种语义）；否则 35% 灰青。判定在 HelpText 的 clayLampFor（无头文件，有自动测试）。
      - 悬停气泡沿用三行格式，第 2 行是此刻的实际值（clayIndicatorTooltip），旧工程缺立体声特征时第 3 行提示「重新聆听」；
        某项因「本曲试过会抬高峰值」被减量 / 否决时（ADR-012 先试后用）第 3 行写「本曲试过：…会抬高峰值，已自动减量 / 跳过」
        （优先级低于「重新聆听」提示）。灯的颜色不因代价变：减量了灯照样亮、否决了（实际值 0）灯就暗，与「在不在处理」一致。
      - 重画：编辑器每 30 ms 喂一份量化过的读数（ClayReadout），三盏灯的颜色类别真的变了才重画自己这 14 × 42 的一小块
        （界面主线程已接近预算，设计规格-界面 第 6 节）；气泡文字不在定时器里拼，悬停时才按需拼（ClayLiveTooltip）。

    组件本身透明：画布静态层上的箭头从下面透出来；组件盖住灯与箭头，鼠标停在箭头上也出修坯的气泡。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <array>
#include <cstddef>

#include "HelpText.h"

namespace celadon::ui
{
    //==============================================================================
    // 会变的气泡文字（修坯三灯指示与抽屉「声像（修坯）」标题共用）。
    //
    // 为什么要「最多每秒换一次字」：JUCE 的 TooltipWindow 每 123 ms 问一次鼠标下那个组件的 getTooltip()，文字一变就把「悬停
    // 0.5 s 才出现」重新计时（juce_TooltipWindow.cpp：tipChanged → lastCompChangeTime = now）。实际值在方案过渡（约 3 s）、
    // 保护攻击 / 释放（0.1 ～ 2 s）时连续在变，每 30 ms 换一次字，气泡就要等数值停稳才出得来。这里文字两次更换至少隔
    // minChangeIntervalMs：换字后至少稳 1 s，TooltipWindow 最迟第 5 次询问（≈ 615 ms）就能让气泡出来；气泡出来之后
    // 读数照样每秒刷新一次。鼠标离开期间没人问，什么都不做——下次悬停第一问就给最新的读数。
    class ClayLiveTooltip
    {
    public:
        // 编辑器定时器每 30 ms：只记下最新读数（几个整数），不拼字。
        void setReadout (const help::ClayReadout& newReadout) noexcept   { latest = newReadout; }

        // TooltipClient::getTooltip 里调（消息线程）：读数变了且距上次换字满 minChangeIntervalMs 才重拼，否则返回上次的字。
        juce::String get();

        static constexpr juce::uint32 minChangeIntervalMs = 1000;

    private:
        help::ClayReadout latest, shown;
        juce::String text;
        juce::uint32 lastChangeMs = 0;
        bool hasText = false;
    };

    //==============================================================================
    class ClayIndicator  : public juce::Component,
                           public juce::TooltipClient
    {
    public:
        ClayIndicator();
        ~ClayIndicator() override;

        // 编辑器定时器每 30 ms 调（消息线程）。三盏灯的颜色类别变了才重画；读数交给气泡按需取用。
        void setReadout (const help::ClayReadout& readout);

        // 某一盏灯此刻画成什么颜色类别（离屏核对与以后的界面测试用）。
        help::ClayLamp getLamp (help::ClayStage stage) const noexcept;

        juce::String getTooltip() override;

        void paint (juce::Graphics&) override;

        // 几何（逻辑像素，随画布一起缩放）：灯心在组件水平正中，第一盏离上缘 firstLampCentreY，往下每 lampPitch 一盏。
        // 编辑器把组件放在两块面板之间 14 px 的空当里，箭头落在组件内 y = 36（见 PluginEditor.cpp 的布局）。
        static constexpr float lampDiameter     = 5.0f;   // 规格：直径 5 px
        static constexpr float lampPitch        = 9.0f;   // 相邻灯心距：5 px 灯 + 4 px 空
        static constexpr float firstLampCentreY = 9.0f;
        static constexpr int   preferredHeight  = 42;     // 三盏灯（最下一盏到 29.5）+ 下面的箭头（36 ± 3：箭头头部宽 6）+ 3 px 余量

    private:
        std::array<help::ClayLamp, static_cast<std::size_t> (help::numClayStages)> lamps {};   // 值初始化 = 全暗（ClayLamp::off = 0）
        ClayLiveTooltip liveTooltip;

        juce::Rectangle<float> lampBounds (int index) const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClayIndicator)
    };
}
