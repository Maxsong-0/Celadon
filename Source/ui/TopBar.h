/*
  ==============================================================================

    TopBar.h
    顶栏（V4 设计稿 1200 × 52）：左边品牌，正中的状态胶囊，右边延迟 · 中 / EN · 设置 · 帮助。

    状态胶囊只写此刻最要紧的一件事（优先级：需要注意 > 正在听原声 > GLAZE 状态）：
      - 需要注意（橙色，带三角）：方案受限 / 受手动设置限制 / 与聆听时的音乐差很多 / 真峰值超过上限 / 屏蔽了异常音频块 /
        Debug 构建；
      - 听原声（B）：「B · 正在听原声 · 已等响 4.8 dB」；
      - 其余：GLAZE 状态行（「● 已锁定 · 自动→温润 · 目标 −9.6 LUFS」），第二段风格在选「自动」时是紫色。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Controls.h"
#include "GlazeStatusText.h"

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

        void setContent (const Content& newContent);
        void setReduceMotion (bool shouldReduceMotion);

        // 按内容算出的宽度（最少 350，最多 560）：编辑器据此居中摆放。
        int getPreferredWidth() const;

        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;

        Content content;
        bool reduceMotion = false;
        float pulsePhase = 0.0f;

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

        std::function<void (Language)> onLanguageChange;
        std::function<void()> onSettings, onHelp;

        Segmented& getLanguageSelector() noexcept           { return languageSelector; }
        StatusPill& getStatusPill() noexcept                { return pill; }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        Language language = Language::chinese;
        juce::String latencyText;
        StatusPill pill;
        Segmented languageSelector { Segmented::Look::language };
        IconButton settingsButton { IconButton::Icon::settings }, helpButton { IconButton::Icon::help };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
    };
}
