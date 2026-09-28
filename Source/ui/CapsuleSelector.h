/*
  ==============================================================================

    CapsuleSelector.h
    竖排单选胶囊（设计规格-界面 第 2 节「Speed」；设计规格-GLAZE-界面与实现 第 1 节「风格」）：上方一行 11 px
    灰青小标题，下面 N 个竖排胶囊，选中实心梅子青、其余只描边（胶囊由 CeladonLookAndFeel::drawToggleButton 画）。

    Balance 面板的 Speed 与 GLAZE 旋钮左边的「风格」共用这一个实现（v0.6 从 ModulePanel 里拆出来）：两处的样子、
    尺寸与手感出自同一段代码，规格写的「样式同 Balance 的 Speed」不会随某一处单独修改而走样。

      - bind：绑定 AudioParameterChoice。点胶囊按一次完整手势写参数（宿主记一步自动化 / 撤销）；参数变了
        （宿主自动化、读工程、别的界面）在消息线程点亮对应胶囊。胶囊数必须等于参数的选项数。
      - showPlaceholder：不绑参数、不可点，selectedIndex 那个亮着，悬停仍有提示（v0.2 的 Speed 占位）。

    尺寸：小标题高 14，胶囊高 18、竖向间距 4，胶囊宽 = min (72, 组件宽 − 4) 且水平居中（组件宽 ≥ 76 时胶囊就是满宽 72）。
    组件的高度由调用方按 getContentHeight() 给（布局照旧把整组垂直居中）。

    v0.1.7.0 加横排（Layout::horizontal，「高级」抽屉的三态 [自动 | 关 | 手动]）：不显示小标题，胶囊等宽横排、间距 4、
    垂直居中，高度 = capsuleHeight。胶囊、绑定、双向同步与竖排是同一段代码，只有 resized 的排法不同。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace celadon::ui
{
    class CapsuleSelector  : public juce::Component
    {
    public:
        CapsuleSelector();
        ~CapsuleSelector() override;

        // 绑定参数：options 的顺序 = 参数的选项顺序（胶囊上的字可以与参数的选项名不同，如风格胶囊写中文）。
        // 小标题与每个胶囊悬停都按参数 ID 出提示。可重复调用：先拆旧的附件与胶囊再建新的。
        void bind (juce::AudioProcessorValueTreeState& state, const char* parameterId,
                   const juce::String& captionText, const juce::StringArray& options);

        // 占位：不绑参数、不可点，selectedIndex 亮着；悬停按 helpId 出提示。
        void showPlaceholder (const char* helpId, const juce::String& captionText,
                              const juce::StringArray& options, int selectedIndex);

        // 小标题 + 全部胶囊 + 间距的总高度（逻辑像素）。
        int getContentHeight() const noexcept;

        // 此刻亮着的胶囊下标；一个都没亮（还没绑定）时返回 −1。
        int getSelectedIndex() const noexcept;

        // 排法（默认竖排带小标题）。横排不显示小标题；在 bind / showPlaceholder 之前或之后调都行。
        enum class Layout { verticalWithCaption, horizontal };
        void setLayout (Layout newLayout);

        // 亮着的胶囊变了（点胶囊、宿主自动化、读工程）就在消息线程调；给需要跟着刷新的调用方（抽屉的行）用。
        std::function<void()> onSelectionChanged;

        static constexpr int captionHeight   = 14;
        static constexpr int capsuleHeight   = 18;
        static constexpr int capsuleGap      = 4;
        static constexpr int maxCapsuleWidth = 72;   // = 开关胶囊宽（设计规格-界面 第 2 节）
        static constexpr int sideMargin      = 4;    // 胶囊宽 = min (72, 组件宽 − 4)：两边至少各留 2 px，描边不贴边

        void resized() override;

    private:
        Layout layout = Layout::verticalWithCaption;
        juce::Label caption;
        juce::OwnedArray<juce::ToggleButton> capsules;

        // 绑定的参数（属于处理器的参数表，这里不拥有）与附件。附件必须在胶囊之后声明：析构先解除监听，胶囊才销毁。
        juce::RangedAudioParameter* parameter = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attachment;

        // 拆掉旧附件与旧胶囊，按 options 建新胶囊（同一 radio 组）；小标题与胶囊都挂 tooltip。
        void rebuild (const juce::String& captionText, const juce::StringArray& options, const juce::String& tooltip);

        // 双向同步：点了第 index 个胶囊 / 参数变成 denormalisedValue（选项下标）。
        void capsuleClicked (int index);
        void showValue (float denormalisedValue);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CapsuleSelector)
    };
}
