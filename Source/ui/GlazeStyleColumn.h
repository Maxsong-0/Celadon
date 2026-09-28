/*
  ==============================================================================

    GlazeStyleColumn.h
    GLAZE 大旋钮左边的一列（设计规格-GLAZE-界面与实现 第 1 节）：
      - 风格：小标题「风格」+ 四个竖排胶囊「自动 / 温润 / 冲击 / 通透」，绑定参数 glaze_style，样式同 Balance 的 Speed
        （同一个 CapsuleSelector）；宿主自动化、读工程时胶囊跟着变；
      - 重新聆听：风格下方一个梅子青描边胶囊。它是界面动作、不是参数（不进自动化）：点一下调 onRelearnClicked；
        聆听中（候选版本在建）文字换成「聆听中…」，再点一次同样调 onRelearnClicked（编排器据此取消，旧方案不受影响）。

    尺寸固定 76 × 132：宽 = 胶囊满宽 72 + 两边各 2（与 Balance 的 Speed 胶囊同宽，「重新聆听」也是 72 宽、与它们对齐）；
    高 = 小标题 14 + 4 × 18 胶囊 + 3 × 4 间距 = 98，空 12，再是 22 高的「重新聆听」——正好与 GLAZE 大旋钮的滑条区（132）
    同高，编辑器把它与旋钮上下对齐。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <functional>

#include "CapsuleSelector.h"

namespace celadon::ui
{
    class GlazeStyleColumn  : public juce::Component
    {
    public:
        explicit GlazeStyleColumn (juce::AudioProcessorValueTreeState& state);
        ~GlazeStyleColumn() override;

        // 点「重新聆听」时调（消息线程）。编辑器接到处理器的「重新聆听」请求上。
        std::function<void()> onRelearnClicked;

        // 候选版本是否在建：按钮文字「重新聆听」/「聆听中…」。只有变了才换字并重画按钮（编辑器每 30 ms 调一次）。
        void setRelearnInProgress (bool inProgress);
        bool isRelearnInProgress() const noexcept   { return relearnInProgress; }

        static constexpr int preferredWidth   = CapsuleSelector::maxCapsuleWidth + CapsuleSelector::sideMargin;   // 76
        static constexpr int relearnGap       = 12;   // 风格胶囊组与「重新聆听」之间：比胶囊间距大，看得出是两组
        static constexpr int relearnHeight    = 22;   // 按钮胶囊的标准高度（与 Reset、各开关相同）
        static constexpr int numStyles        = 4;
        static constexpr int preferredHeight  = CapsuleSelector::captionHeight
                                                 + numStyles * CapsuleSelector::capsuleHeight
                                                 + (numStyles - 1) * CapsuleSelector::capsuleGap
                                                 + relearnGap + relearnHeight;   // 132

        void resized() override;

    private:
        CapsuleSelector styleSelector;
        juce::TextButton relearnButton;
        bool relearnInProgress = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeStyleColumn)
    };
}
