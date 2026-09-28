/*
  ==============================================================================

    GlazeStatusLine.h
    GLAZE 大旋钮下方的状态行（设计规格-GLAZE-界面与实现 第 1 节）：12 px 一行字，居中，灰青 / 梅子青 / 琥珀三种颜色，
    只显示最要紧的一个状态（判定与文字见 GlazeStatusText.h）。悬停出 ui_glaze_status 的提示。

    重画（设计规格-GLAZE-界面与实现 第 4 节、设计规格-界面 第 6 节）：编辑器每 30 ms 喂一次 View，View 与上次相同就
    什么都不做；不同才重拼文字，文字或颜色真的变了才重画自己这一行——不牵动整窗。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "GlazeStatusText.h"

namespace celadon::ui
{
    class GlazeStatusLine  : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        GlazeStatusLine();
        ~GlazeStatusLine() override;

        void setView (const glazestatus::View& newView);

        // 画面上此刻的文字与颜色类别（离屏核对与测试用）。
        const juce::String& getText() const noexcept         { return text; }
        glazestatus::Tone getTone() const noexcept           { return tone; }

        void paint (juce::Graphics&) override;

        // 只有文字所在的那一段接鼠标（悬停出提示），两边的空白不接：状态行比文字宽得多，空白处不该冒出气泡。
        bool hitTest (int x, int y) override;

    private:
        glazestatus::View view;
        bool hasView = false;

        juce::String text;
        glazestatus::Tone tone = glazestatus::Tone::dim;
        int textWidth = 0;   // 文字宽度（逻辑像素，向上取整），hitTest 用

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeStatusLine)
    };
}
