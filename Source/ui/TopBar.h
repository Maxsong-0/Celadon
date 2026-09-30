/*
  ==============================================================================

    TopBar.h
    顶栏（V4 设计稿 1200 × 52）：左边品牌，正中的状态胶囊，右边延迟 · 中 / EN · 设置 · 帮助 · 高级。

    状态胶囊只写此刻最要紧的一件事（优先级：需要注意 > 正在听原声 / 试听差值 > GLAZE 状态）：
      - 需要注意（橙色，带三角）：方案受限 / 受手动设置限制 / 与聆听时的音乐差很多 / 真峰值超过上限 / 屏蔽了异常音频块 /
        Debug 构建；
      - 听原声（B）：「B · 正在听原声 · 已等响 4.8 dB」；试听差值：「LIMIT · 正在试听差值 · 只听被削掉的部分」；
      - 其余：GLAZE 状态行（「● 已锁定 · 自动→温润 · 目标 −9.6 LUFS」），第二段风格在选「自动」时是紫色。
    胶囊按内容定宽、居中在 x = 600，但不压到左边的品牌与右边的延迟读数；实在放不下时最后一段加省略号。

    编辑器每 33 ms 送一次内容与延迟（T-032 R9-12 / 13）：内容、数值都没变就什么都不做；各段文字的宽度与位置只在内容或
    胶囊宽度变了时量一次。聆听 / 校准中圆点呼吸，每帧只重画圆点那一小块；胶囊与顶栏的 paint 跳过与脏区不相交的文字。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "GlazeStatusText.h"

#include <limits>
#include <vector>

namespace celadon::ui
{
    class StatusPill  : public juce::Component,
                        public juce::SettableTooltipClient,
                        private juce::Timer
    {
    public:
        StatusPill();
        ~StatusPill() override;

        struct Content
        {
            bool warning = false;
            juce::StringArray segments;          // 「 · 」隔开的几段
            int accentSegment = -1;              // 哪一段上强调色（紫 = GLAZE 自动 / 深色 = B）
            juce::Colour accentColour;
            juce::Colour dotColour;
            bool pulse = false;                  // 圆点呼吸（聆听中、校准中）

            bool operator== (const Content& other) const;
        };

        // 内容变了返回 true（编辑器据此才重新摆放胶囊）。
        bool setContent (const Content& newContent);
        void setReduceMotion (bool shouldReduceMotion);

        // 按内容算出的宽度（最少 350，最多 560）：编辑器据此居中摆放。内容变了时量一次。
        int getPreferredWidth() const noexcept                  { return preferredWidth; }

        // 呼吸时每帧重画的那一小块（圆点与光晕，本地坐标）。
        juce::Rectangle<int> getPulseArea() const noexcept      { return pulseArea; }

        // paint 里真正排版、画出的文字段数累计（截图工具的检查读它：只重画圆点那一帧应当一段都不画）。
        int getTextDraws() const noexcept                       { return textDraws; }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        struct TextItem
        {
            juce::String text;
            juce::Font font;
            juce::Colour colour;
            juce::Rectangle<float> area;
            bool ellipsis = false;
        };

        void timerCallback() override;
        void measure();          // 内容变了：各段宽度、内容总宽、首选宽度
        void layoutContent();    // 内容或宽度变了：图标 / 圆点与各段文字的位置

        Content content;
        bool reduceMotion = false;
        float pulsePhase = 0.0f;

        std::vector<float> segmentWidths;
        float separatorWidth = 0.0f, contentWidth = 0.0f;
        int preferredWidth = 350;
        std::vector<TextItem> items;
        float markerX = 0.0f;                // 三角图标左边 / 圆点左边
        juce::Rectangle<int> pulseArea;
        int textDraws = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StatusPill)
    };

    // 顶栏右边的圆形图标按钮（设置、帮助）。
    class IconButton  : public ClickableComponent
    {
    public:
        enum class Icon { settings, help };

        explicit IconButton (Icon icon);

        void setActive (bool shouldBeActive)   { active = shouldBeActive; repaint(); }
        void paint (juce::Graphics&) override;

    private:
        Icon icon;
        bool active = false;
    };

    // 顶栏最右的「高级」（设计稿：滑杆图标 + 文字的描边按钮）；面板打开时换浅青底。
    class AdvancedButton  : public ClickableComponent
    {
    public:
        AdvancedButton();

        void setText (const juce::String& newText);
        void setActive (bool shouldBeActive)   { active = shouldBeActive; repaint(); }
        int getNaturalWidth() const;

        void paint (juce::Graphics&) override;

    private:
        juce::String text;
        bool active = false;
    };

    class TopBar  : public juce::Component
    {
    public:
        TopBar();

        static constexpr int height = 52;

        void setLanguage (Language newLanguage);
        void setLatency (double milliseconds);
        void setStatus (const StatusPill::Content& content);
        void setReduceMotion (bool shouldReduceMotion)     { pill.setReduceMotion (shouldReduceMotion); }
        void setSettingsOpen (bool open)                    { settingsButton.setActive (open); }
        void setHelpOpen (bool open)                        { helpButton.setActive (open); }
        void setAdvancedOpen (bool open)                    { advancedButton.setActive (open); }

        std::function<void (Language)> onLanguageChange;
        std::function<void()> onSettings, onHelp, onAdvanced;

        Segmented& getLanguageSelector() noexcept           { return languageSelector; }
        StatusPill& getStatusPill() noexcept                { return pill; }
        AdvancedButton& getAdvancedButton() noexcept        { return advancedButton; }
        const juce::String& getLatencyText() const noexcept { return latencyText; }

        // paint 里真正排版、画出的文字（品牌、副标题、延迟）累计次数（截图工具的检查读它）。
        int getTextDraws() const noexcept                   { return textDraws; }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void layoutPill();
        void updateLatencyText();
        float brandRight() const;
        float latencyLeft() const;

        Language language = Language::chinese;
        double latencyMs = std::numeric_limits<double>::quiet_NaN();   // 还没收到过
        juce::String latencyText, subtitleText;
        float brandWidth = 0.0f, subtitleWidth = 0.0f, latencyWidth = 0.0f;
        int textDraws = 0;
        StatusPill pill;
        Segmented languageSelector { Segmented::Look::language };
        IconButton settingsButton { IconButton::Icon::settings }, helpButton { IconButton::Icon::help };
        AdvancedButton advancedButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
    };
}
