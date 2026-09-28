/*
  ==============================================================================

    AdvancedDrawer.cpp
    「高级」抽屉：声像（修坯）列的三行控件与 Bypass、后三列「即将推出」、右上角 ✕、120 ms 淡入淡出。

    布局（抽屉自己的坐标，868 × 232，与模块面板同一套内边距 14）：

      y  14 ～  30   标题行：各列标题；右上角 ✕
      y  38 ～ 190   声像列三行，每行 48 高、行距 4：名字 36 | 6 | 三态胶囊 3 × 40 + 2 × 4 | 10 | 旋钮 48 | 4 | 数值
      y 196 ～ 218   Bypass 修坯（88 × 22）；旧工程时右边一行琥珀小字「点「重新聆听」后启用自动修坯」
      x  14 ～ 314   声像列（300 宽）；其后三列各 166 宽，列间 14、正中一条 1 px 分隔线

  ==============================================================================
*/

#include "AdvancedDrawer.h"

#include "CapsuleSelector.h"
#include "CeladonLookAndFeel.h"
#include "Knob.h"
#include "../ParameterIds.h"
#include "../dsp/ChainParams.h"

#include <cmath>

namespace celadon::ui
{
    namespace
    {
        // ---- 外框（与 ModulePanel 同一套：圆角 10、内边距 14、标题 13 px 中粗）----
        constexpr float cornerRadius    = 10.0f;
        constexpr int   padding         = 14;
        constexpr int   titleHeight     = 16;
        constexpr float titleFontHeight = 13.0f;
        constexpr int   titleGap        = 8;       // 标题行与下面内容之间

        // ---- 四列：声像列 300 宽，后三列均分剩下的；列间 14，正中一条 1 px 分隔线 ----
        constexpr int   clayColumnWidth = 300;
        constexpr int   columnGap       = 14;

        // ---- Knob 的纵向布局：名字 + 旋钮 44 + 数值 = Knob::smallHeight，标签高 Knob::labelHeight（Knob.h 公开常量，Knob 布局改了
        // 这里跟着变），滑条区向上下标签各多伸 smallKnobMarkerBleed 给实际值小圆点出血（Knob::resized）----
        constexpr int   knobLabelHeight = Knob::labelHeight;
        constexpr int   knobSliderSpan  = Knob::smallHeight - 2 * knobLabelHeight + 2 * smallKnobMarkerBleed;
        static_assert (knobSliderSpan == 48, "Knob 的纵向布局变了：同步改这里（抽屉每行只露出旋钮的滑条区）");

        // ---- 声像列的行 ----
        constexpr int   rowHeight        = knobSliderSpan;   // 一行 = 露出来的那截旋钮（本体 34 + 弧 + 小圆点出血）
        constexpr int   rowGap           = 4;
        constexpr int   nameWidth        = 36;     // 「低切」「收单」「加宽」：两个 12 px 汉字
        constexpr int   nameGap          = 6;
        constexpr int   numModes         = 3;      // 自动 / 关 / 手动
        constexpr int   modeCapsuleWidth = 40;     // 两个 12 px 汉字 + 两边各 8
        constexpr int   modesWidth       = numModes * modeCapsuleWidth + (numModes - 1) * CapsuleSelector::capsuleGap;   // 128
        constexpr int   modesGap         = 10;     // 三态胶囊组与旋钮之间：比胶囊间距大，看得出是两样东西
        constexpr int   valueGap         = 4;      // 旋钮与数值之间
        constexpr float labelFontHeight  = 12.0f;  // 名字 / 数值：与旋钮的名字 / 数值同为 12 px

        constexpr int   bypassGap        = 6;      // 最后一行与 Bypass 开关之间
        constexpr int   bypassWidth      = 88;     // 「Bypass 修坯」比模块面板的「Bypass」长：胶囊加宽到 88，高 22 同开关
        constexpr int   bypassHeight     = 22;
        constexpr int   relearnGap       = 12;     // Bypass 开关与旧工程提示之间
        constexpr float hintFontHeight   = 11.0f;  // 旧工程提示：与胶囊组小标题同为 11 px

        // ---- 右上角 ✕ ----
        constexpr int   closeButtonSize  = 22;     // 点击区（与胶囊同高）
        constexpr float closeGlyphSize   = 9.0f;   // 叉本身的边长
        constexpr float closeGlyphStroke = 1.5f;

        // ---- 「即将推出」徽章：与 ModulePanel 蒙层上的徽章同一种画法 ----
        constexpr float comingSoonFontHeight = 14.0f;
        constexpr float badgeHeight          = 24.0f;
        constexpr float badgePadding         = 28.0f;   // 徽章宽 = 文字宽 + 28

        // 三态胶囊上的字：顺序 = 参数 clay_*_mode 的选项顺序（= celadon::ClayMode 的下标）。
        constexpr const char* modeLabels[numModes] = { "自动", "关", "手动" };
        static_assert (static_cast<int> (ClayMode::automatic) == 0 && static_cast<int> (ClayMode::off) == 1
                        && static_cast<int> (ClayMode::manual) == 2,
                       "ClayMode 的下标变了：同步改三态胶囊的字与顺序");

        //==============================================================================
        // 只露出 Knob 旋钮那一截的窗口。
        //
        // 抽屉每项一行，放不下「名字在上、数值在下」的 80 px 高旋钮（三行就 240，抽屉只有 232）。把整个 Knob（手感、双击回默认、
        // ⌘ 精调、滚轮、提示、实际值标记全在里面）放进 48 px 高的窗口：Knob 整块上移 16，名字与数值两个标签落到窗口外、被父组件
        // 裁掉（绘制与鼠标都裁），露出来的正好是 48 px 的滑条区（本体 34 + 弧 + 上下各 2 px 小圆点出血）。窗口上下各 2 px 仍归标签
        // （标签画在滑条之上、空文字不画东西），与模块面板里的旋钮一样只出提示——手感逐项相同。数值改由行里右边的标签显示。
        // 不复制 Knob 的任何实现：旋钮与模块面板里的是同一个类、同一种画法。
        class KnobWindow final : public juce::Component
        {
        public:
            KnobWindow (juce::AudioProcessorValueTreeState& state, const char* parameterId)
                : knob (state, parameterId, juce::String())
            {
                addAndMakeVisible (knob);
            }

            Knob& getKnob() noexcept   { return knob; }

            void resized() override
            {
                // Knob::resized 把滑条区放在 y = 18 − 2 = 16 起的 48 px：整块上移 16，滑条区正好落在窗口的 0 ～ 48。
                knob.setBounds (0, smallKnobMarkerBleed - knobLabelHeight, getWidth(), Knob::smallHeight);
            }

        private:
            Knob knob;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobWindow)
        };

        //==============================================================================
        // 透明的悬停区：只为让「声像（修坯）」标题出与三灯指示同一个动态气泡。不画东西。
        class LiveTooltipRegion final : public juce::Component,
                                        public juce::TooltipClient
        {
        public:
            explicit LiveTooltipRegion (ClayLiveTooltip& source)
                : tooltipSource (source)
            {
                setInterceptsMouseClicks (true, false);   // 要接住鼠标才会出提示
            }

            juce::String getTooltip() override   { return tooltipSource.get(); }

        private:
            ClayLiveTooltip& tooltipSource;   // 抽屉拥有，先于本组件构造、后于本组件析构

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveTooltipRegion)
        };

        //==============================================================================
        // 右上角 ✕：矢量画的叉（任何缩放都清晰），平时灰青、悬停白瓷、按下梅子青；不画胶囊底，免得像一个开关。
        class CloseButton final : public juce::Button
        {
        public:
            CloseButton()
                : juce::Button ("close")
            {
                setTitle (utf8 ("收起高级抽屉"));   // 读屏用的名字（画面上只有一个叉）
            }

            void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
            {
                const auto glyph = getLocalBounds().toFloat().withSizeKeepingCentre (closeGlyphSize, closeGlyphSize);

                juce::Path cross;
                cross.startNewSubPath (glyph.getTopLeft());
                cross.lineTo (glyph.getBottomRight());
                cross.startNewSubPath (glyph.getBottomLeft());
                cross.lineTo (glyph.getTopRight());

                g.setColour (shouldDrawButtonAsDown ? palette::accent
                                                    : (shouldDrawButtonAsHighlighted ? palette::text : palette::textDim));
                g.strokePath (cross, juce::PathStrokeType (closeGlyphStroke, juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
            }

        private:
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CloseButton)
        };

        //==============================================================================
        // 「即将推出」描边胶囊徽章：与 ModulePanel::paintOverChildren 的徽章同样的字号、高度、留白与配色，画在 centre 处。
        void drawComingSoonBadge (juce::Graphics& g, const juce::String& label, juce::Point<float> centre)
        {
            const auto font  = celadonFont (comingSoonFontHeight, true);
            const auto width = static_cast<float> (juce::GlyphArrangement::getStringWidthInt (font, label)) + badgePadding;
            const auto badge = juce::Rectangle<float> (width, badgeHeight).withCentre (centre);

            g.setColour (palette::panel);
            g.fillRoundedRectangle (badge, badgeHeight * 0.5f);
            g.setColour (palette::accent);
            g.drawRoundedRectangle (badge.reduced (0.5f), badgeHeight * 0.5f, 1.0f);
            g.setFont (font);
            g.drawText (label, badge, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    // 声像列的一行：名字 + 三态胶囊 + 小旋钮（只露出旋钮那一截）+ 数值（带单位）。
    class AdvancedDrawer::ClayRow final : public juce::Component,
                                          private juce::Slider::Listener
    {
    public:
        ClayRow (juce::AudioProcessorValueTreeState& state, const char* modeParameterId,
                 const char* valueParameterId, const juce::String& displayName)
            : knobWindow (state, valueParameterId),
              valueParameter (state.getParameter (valueParameterId))
        {
            jassert (valueParameter != nullptr);   // 参数表里没有这个 ID：检查 Parameters.cpp 与 ParameterIds.h

            // 名字与数值：与旋钮的名字 / 数值同为 12 px（名字白瓷色、数值灰青）。名字悬停出旋钮参数的提示——这一级是做什么的。
            const auto tooltip = help::tooltipFor (valueParameterId);

            for (auto* rowLabel : { &nameLabel, &valueLabel })
            {
                rowLabel->setFont (celadonFont (labelFontHeight));
                rowLabel->setJustificationType (juce::Justification::centredLeft);
                rowLabel->setBorderSize (juce::BorderSize<int> (0));
                rowLabel->setMinimumHorizontalScale (0.75f);
                rowLabel->setInterceptsMouseClicks (true, false);   // 要接住鼠标才会出提示
                rowLabel->setTooltip (tooltip);
                addAndMakeVisible (*rowLabel);
            }

            nameLabel.setText (displayName, juce::dontSendNotification);
            nameLabel.setColour (juce::Label::textColourId, palette::text);
            valueLabel.setColour (juce::Label::textColourId, palette::textDim);

            // 三态胶囊：与 Balance 的 Speed、GLAZE 的风格同一个 CapsuleSelector（同一段绑定与画法），横排、不带小标题。
            juce::StringArray modeNames;

            for (const auto* label : modeLabels)
                modeNames.add (utf8 (label));

            modes.setLayout (CapsuleSelector::Layout::horizontal);
            modes.bind (state, modeParameterId, {}, modeNames);
            addAndMakeVisible (modes);
            addAndMakeVisible (knobWindow);

            // 三态变了（点胶囊、宿主自动化、读工程）立刻调亮度与标记；拖旋钮时数值跟着变（Slider 监听，不占 Knob 自己的 onValueChange）。
            modes.onSelectionChanged = [this] { refreshMode(); };
            knobWindow.getKnob().getSlider().addListener (this);

            updateValueText();
            refreshMode();
        }

        ~ClayRow() override
        {
            // 先解除对旋钮的监听、断开三态回调，再按声明的相反顺序销毁子组件。
            knobWindow.getKnob().getSlider().removeListener (this);
            modes.onSelectionChanged = nullptr;
        }

        // 此刻交给 ClayModule 的该级实际值（value，未量化）与该级此刻是否在处理（stageActive：与三灯指示同一个判定，旁通时为 false）。
        // 标记只在「自动」态画。
        void setActual (float value, bool stageActive)
        {
            actualValue  = value;
            actualActive = stageActive;
            applyMarker();
        }

        void resized() override
        {
            // 名字 36 | 6 | 三态胶囊 128 | 10 | 旋钮 48 | 4 | 数值（其余）。行高 = 露出来的那截旋钮（48）。
            auto area = getLocalBounds();

            nameLabel.setBounds (area.removeFromLeft (nameWidth));
            area.removeFromLeft (nameGap);
            modes.setBounds (area.removeFromLeft (modesWidth));
            area.removeFromLeft (modesGap);
            knobWindow.setBounds (area.removeFromLeft (knobSliderSpan).withSizeKeepingCentre (knobSliderSpan, knobSliderSpan));
            area.removeFromLeft (valueGap);
            valueLabel.setBounds (area);
        }

    private:
        juce::Label nameLabel;
        CapsuleSelector modes;
        KnobWindow knobWindow;
        juce::Label valueLabel;

        juce::RangedAudioParameter* valueParameter = nullptr;   // 实际值 → 0 ～ 1 比例按它自己的刻度换算；数值的单位也取它的
        float actualValue = 0.0f;
        bool actualActive = false;

        // 此刻选中的三态；还没绑上参数（一个都没亮）时按参数默认值「自动」。
        ClayMode currentMode() const noexcept
        {
            const auto index = modes.getSelectedIndex();
            return juce::isPositiveAndBelow (index, numModes) ? static_cast<ClayMode> (index) : ClayMode::automatic;
        }

        void refreshMode()
        {
            // 「关」：旋钮与数值 35% 暗（规格第 5 节；与「即将推出」同一个 35%）。「自动」「手动」正常亮度。
            // setAlpha 值没变时 JUCE 什么都不做，三态没变的 30 ms 不会重画。
            const auto rowAlpha = currentMode() == ClayMode::off ? palette::comingSoonAlpha : 1.0f;
            knobWindow.setAlpha (rowAlpha);
            valueLabel.setAlpha (rowAlpha);

            applyMarker();
        }

        void applyMarker()
        {
            auto& knob = knobWindow.getKnob();

            // 只在「自动」态、且该级此刻在处理时画（规格第 5 节：「自动」态旋钮上用小圆点显示此刻自动给的值）。
            if (! actualActive || currentMode() != ClayMode::automatic || valueParameter == nullptr
                 || ! std::isfinite (actualValue))
            {
                knob.setEffectiveProportion (0.0, false);   // 本来就没显示时 Knob 里什么都不做
                return;
            }

            // 实际值换成该参数自己 0 ～ 1 刻度上的位置（与数值弧同一映射）：用范围本身换算、先钳到范围——与编辑器给 7 个受 GLAZE
            // 影响的旋钮画标记同一种算法（参数的 convertTo0to1 会先吸附到步进，小圆点要标的是真正在用的值）。
            const auto& range = valueParameter->getNormalisableRange();
            const auto proportion = static_cast<double> (range.convertTo0to1 (juce::jlimit (range.start, range.end, actualValue)));
            const bool finite = std::isfinite (proportion);
            knob.setEffectiveProportion (finite ? proportion : 0.0, finite);
        }

        void updateValueText()
        {
            // 与 Knob 自己的数值标签同一段文字（附件装进 slider 的参数文字，如「25」「+0.8」），再补上参数的单位：抽屉里数值
            // 在旋钮右边单独成列，带单位更好读。参数文字本身已带单位时不重复。
            auto& slider = knobWindow.getKnob().getSlider();
            auto shown = slider.getTextFromValue (slider.getValue());

            if (valueParameter != nullptr)
            {
                const auto unit = valueParameter->getLabel();

                if (unit.isNotEmpty() && ! shown.endsWithIgnoreCase (unit))
                    shown << " " << unit;
            }

            valueLabel.setText (shown, juce::dontSendNotification);
        }

        void sliderValueChanged (juce::Slider*) override
        {
            updateValueText();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClayRow)
    };

    //==============================================================================
    AdvancedDrawer::AdvancedDrawer (juce::AudioProcessorValueTreeState& state)
        : clayTitle (utf8 ("声像（修坯）")),
          relearnHint (help::clayRelearnHint()),
          comingSoonText (utf8 ("即将推出"))
    {
        // 后三列：子项目 2 ～ 4（设计规格-Clay 第 1 节），列名照规格第 5 节。
        placeholderTitles.add (utf8 ("推响不刺"));
        placeholderTitles.add (utf8 ("目标频谱"));
        placeholderTitles.add (utf8 ("发行"));
        jassert (placeholderTitles.size() == numPlaceholders);

        // 三行 = 修坯的三级（与三灯指示自上而下同一顺序）。
        rows[0] = std::make_unique<ClayRow> (state, param::clayLowCutMode, param::clayLowCutHz, utf8 ("低切"));
        rows[1] = std::make_unique<ClayRow> (state, param::clayMonoMode,   param::clayMonoHz,   utf8 ("收单"));
        rows[2] = std::make_unique<ClayRow> (state, param::clayWidthMode,  param::clayWidthDb,  utf8 ("加宽"));

        for (auto& row : rows)
            addAndMakeVisible (*row);

        bypassButton.setButtonText (utf8 ("Bypass 修坯"));
        bypassButton.setTooltip (help::tooltipFor (param::clayBypass));
        bypassButton.setComponentID ("clayBypass");
        addAndMakeVisible (bypassButton);
        bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, param::clayBypass,
                                                                                                  bypassButton);

        auto newCloseButton = std::make_unique<CloseButton>();
        newCloseButton->setComponentID ("advancedClose");
        newCloseButton->onClick = [this]
        {
            if (onCloseClicked != nullptr)
                onCloseClicked();
        };
        addAndMakeVisible (*newCloseButton);
        closeButton = std::move (newCloseButton);

        clayTitleRegion = std::make_unique<LiveTooltipRegion> (liveTooltip);
        addAndMakeVisible (*clayTitleRegion);

        // 圆角外露出画布（底色 + 开片纹）：不能声明不透明。默认收起：不可见、完全透明（第一次打开从 0 淡入）。
        setOpaque (false);
        setComponentID ("advancedDrawer");
        setAlpha (0.0f);
        setVisible (false);
    }

    AdvancedDrawer::~AdvancedDrawer()
    {
        stopTimer();

        // 附件先走（它监听着按钮与参数），其余按声明的相反顺序销毁（标题悬停区先于它引用的 liveTooltip）。
        closeButton->onClick = nullptr;
        bypassAttachment.reset();
    }

    //==============================================================================
    void AdvancedDrawer::setOpen (bool shouldBeOpen)
    {
        if (open == shouldBeOpen)
            return;

        open = shouldBeOpen;

        // 从此刻的不透明度起淡：半路反向时不跳；整程 120 ms 按要走的比例缩短（走一半就 60 ms）。
        fadeFromAlpha  = isVisible() ? getAlpha() : 0.0f;
        fadeToAlpha    = open ? 1.0f : 0.0f;
        fadeStartMs    = juce::Time::getMillisecondCounter();
        fadeDurationMs = static_cast<juce::uint32> (juce::roundToInt (static_cast<float> (fadeMilliseconds)
                                                                      * std::abs (fadeToAlpha - fadeFromAlpha)));

        if (open)
        {
            setAlpha (fadeFromAlpha);
            setVisible (true);
            updateMarkers();   // 收起期间不更新标记：打开的第一帧就按最新的快照画
        }
        else
        {
            // 先让下面的面板可见：淡出时从抽屉底下透出来（面板被藏起来的话，淡出期间露出的是空底）。
            setCovering (false);
        }

        startTimerHz (fadeTimerHz);
    }

    void AdvancedDrawer::timerCallback()
    {
        // 按真实经过的时间算 alpha：计时器抖动、某一帧晚到都不改变总时长。无符号减法：计数器回绕也算对。
        const auto elapsed = juce::Time::getMillisecondCounter() - fadeStartMs;

        if (elapsed >= fadeDurationMs)
        {
            stopTimer();
            setAlpha (fadeToAlpha);

            if (open)
                setCovering (true);   // 完全盖住：编辑器把下面的面板与修坯指示灯藏起来
            else
                setVisible (false);

            return;
        }

        const auto progress = static_cast<float> (elapsed) / static_cast<float> (fadeDurationMs);
        setAlpha (fadeFromAlpha + (fadeToAlpha - fadeFromAlpha) * progress);
    }

    void AdvancedDrawer::setCovering (bool shouldCover)
    {
        if (covering == shouldCover)
            return;

        covering = shouldCover;

        if (onCoverageChanged != nullptr)
            onCoverageChanged (covering);
    }

    //==============================================================================
    void AdvancedDrawer::setClayStatus (const glaze::Status& newStatus, const help::ClayReadout& newReadout)
    {
        status = newStatus;
        liveTooltip.setReadout (newReadout);

        // 旧工程提示只在出现 / 消失时重画那一小块。
        if (newReadout.needsRelearn != readout.needsRelearn && ! relearnArea.isEmpty())
            repaint (relearnArea);

        readout = newReadout;

        // 收起期间（不可见）不必更新标记：setOpen 打开时先按最新的快照补一次。
        if (isVisible())
            updateMarkers();
    }

    void AdvancedDrawer::updateMarkers()
    {
        // 「该级此刻在处理」与三灯指示同一个判定（量化读数；旁通时全为 0）；标记的位置用快照里未量化的实际值。
        rows[0]->setActual (status.clayLowCutHz, readout.lowCutHz > 0);
        rows[1]->setActual (status.clayMonoHz,   readout.monoPercent > 0);
        rows[2]->setActual (status.clayWidthDb,  readout.widthTenthsDb != 0);
    }

    //==============================================================================
    void AdvancedDrawer::paint (juce::Graphics& g)
    {
        // 深色面板底：与模块面板同一种圆角框（盖在它们上面，看上去是同一块面板换了内容）。
        const auto frame = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (palette::panel);
        g.fillRoundedRectangle (frame, cornerRadius);
        g.setColour (palette::panelStroke);
        g.drawRoundedRectangle (frame, cornerRadius, 1.0f);

        // 列间分隔线：描边色 1 px（颜色沿用上面的描边色）。
        for (const auto x : separatorX)
            g.fillRect (x, separatorTop, 1, separatorBottom - separatorTop);

        const auto titleFont = celadonFont (titleFontHeight, true);

        // 声像列标题。
        if (g.clipRegionIntersects (clayTitleArea))
        {
            g.setFont (titleFont);
            g.setColour (palette::text);
            g.drawText (clayTitle, clayTitleArea, juce::Justification::centredLeft, false);
        }

        // 后三列：标题按 35% 画（ADR-005「即将推出」），列中间一枚与模块面板蒙层上同样的描边徽章。
        for (std::size_t i = 0; i < placeholderAreas.size(); ++i)
        {
            const auto& column = placeholderAreas[i];

            if (column.isEmpty() || ! g.clipRegionIntersects (column))
                continue;

            g.setFont (titleFont);
            g.setColour (palette::text.withAlpha (palette::comingSoonAlpha));
            g.drawText (placeholderTitles[static_cast<int> (i)], column.withHeight (titleHeight),
                        juce::Justification::centredLeft, false);

            drawComingSoonBadge (g, comingSoonText, column.withTrimmedTop (titleHeight + titleGap).toFloat().getCentre());
        }

        // 旧工程：琥珀小字「点「重新聆听」后启用自动修坯」（与三灯指示气泡的第 3 行同一句；琥珀 = 要你动手，同状态行）。
        if (readout.needsRelearn && g.clipRegionIntersects (relearnArea))
        {
            g.setFont (celadonFont (hintFontHeight));
            g.setColour (palette::reduction);
            g.drawText (relearnHint, relearnArea, juce::Justification::centredLeft, true);
        }
    }

    void AdvancedDrawer::resized()
    {
        auto inner = getLocalBounds().reduced (padding);

        // 右上角 ✕：点击区 22 × 22，与标题行垂直居中、右缘贴内边距。
        closeButton->setBounds (juce::Rectangle<int> (closeButtonSize, closeButtonSize)
                                    .withCentre ({ inner.getRight() - closeButtonSize / 2, inner.getY() + titleHeight / 2 }));

        // 四列：声像列在左，后三列均分剩下的宽度；列间 14，分隔线在列间正中。
        auto clayColumn = inner.removeFromLeft (clayColumnWidth);
        const auto placeholderWidth = (inner.getWidth() - numPlaceholders * columnGap) / numPlaceholders;

        separatorTop    = inner.getY();
        separatorBottom = inner.getBottom();

        for (std::size_t i = 0; i < placeholderAreas.size(); ++i)
        {
            separatorX[i] = inner.getX() + columnGap / 2;
            inner.removeFromLeft (columnGap);
            placeholderAreas[i] = inner.removeFromLeft (placeholderWidth);
        }

        // 声像列：标题行 → 三行（行距 4）→ Bypass 开关（右边同一行放旧工程提示）。
        clayTitleArea = clayColumn.removeFromTop (titleHeight);

        // 标题悬停区只盖住文字（再放宽 4 px），标题行右边的空白处不冒气泡。
        const auto titleWidth = juce::GlyphArrangement::getStringWidthInt (celadonFont (titleFontHeight, true), clayTitle);
        clayTitleRegion->setBounds (clayTitleArea.withWidth (juce::jmin (clayTitleArea.getWidth(), titleWidth + 4)));

        clayColumn.removeFromTop (titleGap);

        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            if (i > 0)
                clayColumn.removeFromTop (rowGap);

            rows[i]->setBounds (clayColumn.removeFromTop (rowHeight));
        }

        clayColumn.removeFromTop (bypassGap);
        auto bypassRow = clayColumn.removeFromTop (bypassHeight);
        bypassButton.setBounds (bypassRow.removeFromLeft (bypassWidth));
        bypassRow.removeFromLeft (relearnGap);
        relearnArea = bypassRow;

        // 抽屉高 232（= 模块面板高）正好放下：14 + 16 + 8 + 3 × 48 + 2 × 4 + 6 + 22 + 14。改了尺寸常数而放不下时 Debug 版断在这里。
        jassert (getHeight() <= 0 || bypassButton.getBottom() <= getHeight() - padding);
    }
}
