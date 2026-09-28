/*
  ==============================================================================

    MeterBar.h
    电平表：竖条（IN / OUT 真峰值、Limit GR）或标题行里的横向小表（Glue GR）。
    下落 20 dB/s；竖条下方写峰值保持数字，点数字清零。悬停显示提示（setTooltip 由调用方设）。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace celadon::ui
{
    class MeterBar  : public juce::Component,
                      public juce::SettableTooltipClient
    {
    public:
        enum class Style { vertical, horizontalMini };

        // growsFromTop = true：GR 表（琥珀色，从上往下长 / 横向从右往左读都可，由实现者按规格定）。
        MeterBar (const juce::String& title, float minDb, float maxDb, bool growsFromTop,
                  Style style = Style::vertical);
        ~MeterBar() override;

        // 界面定时器每 30 ms 调一次，传这段时间的最新读数（dB）。内部负责下落与峰值保持；
        // 只有画面上看得出变化时才重画（见 shownDb）。「没有读数」请传 minDb 或更低（例如 LoudnessMeter::noReading）。
        void pushReading (float db);

        // 峰值保持清零（点数字、或总览区 Reset 时调用）。
        void resetHold();

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        // ---- 以下由实现者（v0.2 计划 A5）按需要补充；公开接口不得改动 ----
        juce::String title;
        float minDb, maxDb;
        bool fromTop;
        Style style;

        float displayDb;       // 带下落的显示值
        float holdDb;          // 复位以来的最大读数
        bool hasHold = false;  // 复位以来收到过有效读数才显示数字，否则写「-inf」（GR 表的 0.0 也算有效）

        // 画面上正显示的那一份（v0.5 研究 P5 R5）：paint 只读这几项。新读数只有在屏幕上看得出来时才换进来并重画——
        // 表面边缘或峰值刻线移动 ≥ 半个物理像素，或者数字（一位小数）变了。比的是「上次画出来的」，
        // 慢慢漂移攒够半个像素也会画，不会卡住。v0.4 按 0.01 dB（竖表约 0.03 px）就重画，几乎每帧都画。
        float shownDb;
        float shownHoldDb;
        bool shownHasHold = false;
        juce::String shownText;   // 竖表：峰值保持数字；横向小表：当前值「x.x dB」

        // 上次 paint 时每个逻辑像素对应的物理像素（界面缩放 × Retina × 宿主缩放）。第一次画之前按 3（150 % × Retina）算，
        // 宁可多画一次。
        float physicalPixelsPerLogical = 3.0f;

        float proportionOf (float db) const noexcept;
        float barLength() const noexcept;                       // 表条在读数方向上的长度（逻辑像素）
        bool movesVisibly (float fromDb, float toDb) const noexcept;
        juce::String holdText() const;
        juce::String valueText (float db) const;
        juce::String shownTextFor() const;                      // 按当前读数应显示的数字
        void updateShownState (bool force);
        void paintVertical (juce::Graphics&);
        void paintHorizontalMini (juce::Graphics&);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterBar)
    };
}
