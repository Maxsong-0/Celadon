/*
  ==============================================================================

    ModuleCards.h
    下半部分的四张模块卡片（274 × 248），按信号处理顺序从左到右：
    01 BALANCE 动态均衡 → 02 CLAY 声像 → 03 COLOR 音色 → 04 LIMIT 限幅。

      - 卡片头：序号、名字、一句话说明、（右边）备注与电源键（旁通；旁通时卡片内容变淡）；
      - 旋钮与滑条的手感见 Controls.h；紫色 = GLAZE 此刻的自动加量（弧 + 小圆点 + 数值后的「+13」）；
      - CLAY：三行 = 灯 + 名字 + 此刻的数值 + 自动 / 关 / 手动；「手动」时数值可以拖（改的是手动值）；
        下面一条 10 Hz ～ 1 kHz 的小图：低切以下斜线、收单的单声道区、其上的立体声区。
        T-038：House 档反相提示亮着时，小图那一格换成两行琥珀字「俱乐部单声道会丢低频（约 1.6 dB）/ 请回混音修」。
      - BALANCE：27 段修正量的小柱图 + 回补 / 抑制 / 提亮 + 速度；
      - COLOR：音调 / 粘合 / 热度 + 干湿、输出、粘合衰减；
      - LIMIT：增益 / 上限 / 削波 + 释放（带 AUTO）、预读、限幅衰减（方案受限时橙色）；卡片头电源键左边是耳机「试听差值」。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "../dsp/BalanceModule.h"
#include "../dsp/ChainParams.h"
#include "Controls.h"
#include "HelpText.h"

#include <memory>

namespace celadon::ui
{
    //==============================================================================
    class ModuleCard  : public juce::Component,
                        public juce::TooltipClient
    {
    public:
        ModuleCard (juce::AudioProcessorValueTreeState& state, const char* bypassParameterId, InteractionHost& host,
                    juce::String number, juce::String title);

        static constexpr int width = 274;
        static constexpr int height = 248;

        virtual void setLanguage (Language newLanguage);

        void paint (juce::Graphics&) override;
        void resized() override;
        juce::String getTooltip() override;

    protected:
        // 卡片头的说明文字与右边的备注（如「27 段」）。
        virtual juce::String getDescription() const = 0;
        virtual juce::String getNote() const             { return {}; }
        virtual juce::String getHeaderTooltip() const = 0;
        virtual void layoutContent() = 0;

        // 内容组件：旁通时一起变淡。
        void addContent (juce::Component& component);
        void updateBypassLook();

        // 卡片头电源键左边的一个 28 × 28 小键（LIMIT 的耳机）：说明文字与备注让出它的位置。
        void setHeaderAccessory (juce::Component& accessory);

        juce::AudioProcessorValueTreeState& state;
        InteractionHost& host;
        Language language = Language::chinese;
        PowerButton power;

    private:
        float headerRightEdge() const;

        juce::String number, title;
        juce::Array<juce::Component*> content;
        juce::Component* headerAccessory = nullptr;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleCard)
    };

    //==============================================================================
    class ClayCard  : public ModuleCard
    {
    public:
        ClayCard (juce::AudioProcessorValueTreeState& state, InteractionHost& host);

        void setLanguage (Language newLanguage) override;

        // 每帧调：编排器快照里的 Clay 实际值与保护状态（已按三态与旁通算好的读数）。
        void setReadout (const help::ClayReadout& readout);

        void paint (juce::Graphics&) override;
        juce::String getTooltip() override;

    protected:
        juce::String getDescription() const override;
        juce::String getHeaderTooltip() const override;
        void layoutContent() override;

    private:
        struct Row
        {
            help::ClayStage stage;
            const char* modeId;
            const char* valueId;
            std::unique_ptr<Segmented> modes;
            std::unique_ptr<SegmentedAttachment> modesAttachment;
            std::unique_ptr<DragValue> value;
        };

        void refreshRows();
        int modeOf (const Row& row) const;
        juce::Rectangle<float> rowBounds (int index) const;
        juce::Rectangle<float> stripBounds() const;
        juce::Rectangle<float> houseHintBounds() const;   // T-038 House 反相提示亮着时占小图那一格（两行琥珀字）

        std::array<Row, 3> rows;
        help::ClayReadout readout;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClayCard)
    };

    //==============================================================================
    // BALANCE / COLOR / LIMIT 共用：三个旋钮 + 下面几行。
    class KnobCard  : public ModuleCard
    {
    public:
        using ModuleCard::ModuleCard;

        // 每帧调：GLAZE 叠加后的实际值；glazeActive = GLAZE > 0（否则不画紫色）。
        virtual void setEffective (const ChainParams& effective, bool glazeActive) = 0;

    protected:
        ParamKnob& addKnob (const char* parameterId, ParamKnob::Options options);
        ParamSlider& addSlider (const char* parameterId, bool bipolar);
        void layoutKnobs (int y);

        juce::OwnedArray<ParamKnob> knobs;
        juce::OwnedArray<ParamSlider> sliders;
    };

    class BalanceCard  : public KnobCard
    {
    public:
        BalanceCard (juce::AudioProcessorValueTreeState& state, InteractionHost& host);

        void setLanguage (Language newLanguage) override;
        void setEffective (const ChainParams& effective, bool glazeActive) override;
        void setBandGains (const BalanceModule::BandGains& gains);

        void paint (juce::Graphics&) override;
        juce::String getTooltip() override;

    protected:
        juce::String getDescription() const override;
        juce::String getNote() const override;
        juce::String getHeaderTooltip() const override;
        void layoutContent() override;

    private:
        juce::Rectangle<float> graphBounds() const   { return { 14.0f, 44.0f, 246.0f, 36.0f }; }

        BalanceModule::BandGains shownGains {};
        Segmented speed { Segmented::Look::groove };
        std::unique_ptr<SegmentedAttachment> speedAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BalanceCard)
    };

    class ColorCard  : public KnobCard
    {
    public:
        ColorCard (juce::AudioProcessorValueTreeState& state, InteractionHost& host);

        void setLanguage (Language newLanguage) override;
        void setEffective (const ChainParams& effective, bool glazeActive) override;
        void setGlueReduction (float db);

    protected:
        juce::String getDescription() const override;
        juce::String getHeaderTooltip() const override;
        void layoutContent() override;

    private:
        ReductionRow glueReduction { 6.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColorCard)
    };

    class LimitCard  : public KnobCard
    {
    public:
        LimitCard (juce::AudioProcessorValueTreeState& state, InteractionHost& host);

        void setLanguage (Language newLanguage) override;
        void setEffective (const ChainParams& effective, bool glazeActive) override;
        void setLimitReduction (float db, bool planLimited);

        // 试听差值（界面状态，编辑器管）：耳机亮不亮；点耳机时调 onAuditionClicked。LIMIT 旁通时耳机禁用。
        void setAuditioning (bool auditioning)   { audition.setOn (auditioning); }
        bool isAuditioning() const noexcept      { return audition.isOn(); }
        std::function<void()> onAuditionClicked;

    protected:
        juce::String getDescription() const override;
        juce::String getHeaderTooltip() const override;
        void layoutContent() override;

    private:
        HeadphoneButton audition;
        std::unique_ptr<ChipToggle> autoRelease;
        ReductionRow limitReduction { 12.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimitCard)
    };
}
