/*
  ==============================================================================

    PluginEditor.cpp
    v0.1.8.0 界面改版：画布的布局（坐标全部照 V4 设计稿，1200 × 820 逻辑像素）、静态背景、控件之间的交互服务
    （数值气泡、直接输入框、操作提示、浮层），以及每 30 ms 一次把处理器的读数送到各个部件。

  ==============================================================================
*/

#include "PluginEditor.h"

#include <array>
#include <cmath>
#include <limits>

namespace
{
    using namespace celadon;
    using namespace celadon::ui;

    constexpr double minZoom = UiSettings::minZoom;
    constexpr double maxZoom = UiSettings::maxZoom;
    constexpr int refreshHz = 30;
    constexpr double maxSpectrumFps = 120.0;            // 频谱仪跟屏幕刷新、但最多这么多帧（T-036；高于它的屏按整数分频：144 Hz → 72、240 Hz → 120）
    constexpr double spectrumVBlankTimeoutMs = 100.0;   // 垂直同步这么久没来就由 30 Hz 定时器推频谱仪（T-036）
    constexpr double spectrumMinStepMs = 15.0;          // 定时器推的两帧至少隔这么久（刚被别处推过就跳过）
    constexpr double maxSpectrumStepSeconds = 0.25;     // 一帧最多按这么长算（窗口刚露出来、卡顿之后不一次跳到底）
    constexpr double staticLayerSettleMs = 150.0;   // 缩放窗口时等像素比稳定下来再重画静态背景
    constexpr float maxStaticLayerScale = 2.0f;
    constexpr double resizeHudHoldMs = 700.0;
    constexpr double nonFiniteWarningMs = 10000.0;  // 屏蔽了异常音频块：之后 10 s 内顶部提示
    constexpr double soundHangMs = 500.0;           // 电平掉到静音以下多久才算「停止播放」

    // 受 GLAZE 影响的 7 个参数（= glaze::AutoIncrements 的 7 个字段）：卡片按它们画紫色的自动加量。
    static_assert (sizeof (glaze::AutoIncrements) == 7 * sizeof (float), "glaze::AutoIncrements 的项数变了：同步改各卡片的 setEffective");

    const juce::String& minusSign()
    {
        static const juce::String sign = juce::String::charToString ((juce::juce_wchar) 0x2212);
        return sign;
    }

    juce::String signedOneDecimal (float value)
    {
        const auto magnitude = juce::String (std::abs (value), 1);

        if (value <= -0.05f)
            return minusSign() + magnitude;

        return (value >= 0.05f ? "+" : "") + magnitude;
    }

    // 按「 · 」整体拆成几段（StringArray::fromTokens 会把多字节分隔符当成字符集合）。
    juce::StringArray splitSegments (const juce::String& text)
    {
        juce::StringArray parts;
        const auto separator = juce::String::fromUTF8 (" · ");
        auto start = 0;

        for (;;)
        {
            const auto at = text.indexOf (start, separator);
            parts.add (at < 0 ? text.substring (start) : text.substring (start, at));

            if (at < 0)
                break;

            start = at + separator.length();
        }

        return parts;
    }

    // 右下角缩放把手的按下 / 松开：画布据此在拖动期间显示大小提示。
    struct CornerDragListener  : public juce::MouseListener
    {
        std::function<void (bool)> onDrag;
        void mouseDown (const juce::MouseEvent&) override   { if (onDrag != nullptr) onDrag (true); }
        void mouseUp (const juce::MouseEvent&) override     { if (onDrag != nullptr) onDrag (false); }
    };

    float rawValue (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        if (auto* value = state.getRawParameterValue (id))
            return value->load();

        jassertfalse;
        return 0.0f;
    }

    // ---- 静态背景的面板阴影（T-032 R9-08）----
    // 设计稿 0 1px 2px rgba(13,40,48,.05), 0 8px 24px rgba(13,40,48,.06)：每块面板两层 DropShadow。
    // DropShadow::drawForPath 每次都在逻辑分辨率里重画路径、做盒模糊，再按上下文比例贴（juce_DropShadowEffect.cpp）——
    // 模糊出来的图与像素比无关：按同一算法生成一次，之后每次重画静态背景只贴图，结果与每次现算逐位相同。
    struct ShadowMask
    {
        juce::Image image;              // 单通道，逻辑分辨率
        juce::Point<int> position;      // 左上角（逻辑坐标）
        juce::Colour colour;
    };

    ShadowMask makeShadowMask (const juce::DropShadow& shadow, const juce::Path& path, const juce::ImageType& imageType)
    {
        // 与 drawForPath 相同的外接区域：路径外接整数矩形 + 偏移，四周各扩 radius + 1。drawForPath 还会与「裁剪 + radius + 1」
        // 求交：静态背景的裁剪是整张画布，七块面板的阴影都在里面，求交不改变区域。
        const auto area = (path.getBounds().getSmallestIntegerContainer() + shadow.offset).expanded (shadow.radius + 1);
        juce::Image image (juce::Image::SingleChannel, area.getWidth(), area.getHeight(), true, imageType);

        {
            juce::Graphics maskGraphics (image);
            maskGraphics.setColour (juce::Colours::white);
            maskGraphics.fillPath (path, juce::AffineTransform::translation ((float) (shadow.offset.x - area.getX()),
                                                                            (float) (shadow.offset.y - area.getY())));
        }

        image.getPixelData()->applySingleChannelBoxBlurEffect (shadow.radius);
        return { image, area.getPosition(), shadow.colour };
    }

    std::vector<ShadowMask> makePanelShadows (const juce::ImageType& imageType)
    {
        std::vector<ShadowMask> masks;

        const auto add = [&masks, &imageType] (juce::Rectangle<float> area, float cornerRadius)
        {
            juce::Path shape;
            shape.addRoundedRectangle (area, cornerRadius);
            masks.push_back (makeShadowMask (juce::DropShadow (palette::shadowTint.withAlpha (0.07f), 20, { 0, 8 }), shape, imageType));
            masks.push_back (makeShadowMask (juce::DropShadow (palette::shadowTint.withAlpha (0.06f), 3, { 0, 1 }), shape, imageType));
        };

        add ({ 16.0f, 60.0f, 60.0f, 238.0f }, 12.0f);
        add ({ 1124.0f, 60.0f, 60.0f, 238.0f }, 12.0f);
        add ({ 84.0f, 60.0f, 1032.0f, 238.0f }, 14.0f);

        for (const auto x : { 16.0f, 314.0f, 612.0f, 910.0f })
            add ({ x, 560.0f, (float) ModuleCard::width, (float) ModuleCard::height }, 14.0f);

        return masks;
    }

    // ---- 静态背景的细颗粒 ----
    // 每个物理像素一点点随机明暗（设计稿 3.5 % 的灰噪声，线性同余序列逐像素取一档）。查找表 lut[档][通道值] 用与原来逐像素
    // 现算同一条浮点算式生成，结果逐位相同；逐像素只剩三次查表（原来每像素三次浮点取整、钳位）。
    const std::array<std::array<juce::uint8, 256>, 256>& grainTable()
    {
        static const auto table = []
        {
            std::array<std::array<juce::uint8, 256>, 256> result {};

            for (int level = 0; level < 256; ++level)
            {
                const auto noise = ((float) level / 255.0f - 0.5f) * 0.035f * 255.0f * 0.5f;

                for (int channel = 0; channel < 256; ++channel)
                    result[(size_t) level][(size_t) channel] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt ((float) channel + noise));
            }

            return result;
        }();

        return table;
    }
}

//==============================================================================
class CeladonAudioProcessorEditor::Canvas  : public juce::Component,
                                             public InteractionHost,
                                             private juce::Timer
{
public:
    Canvas (CeladonAudioProcessorEditor& editorToUse, CeladonAudioProcessor& processorToUse)
        : editor (editorToUse),
          glazeKnob (*processorToUse.getParameters().getParameter (param::glazeAmount), *this),
          clayCard (processorToUse.getParameters(), *this),
          balanceCard (processorToUse.getParameters(), *this),
          colorCard (processorToUse.getParameters(), *this),
          limitCard (processorToUse.getParameters(), *this),
          advancedPanel (processorToUse.getParameters(), *this)
    {
        setOpaque (true);
        auto& state = processorToUse.getParameters();

        // ---- 上层 ----
        addAndMakeVisible (topBar);
        topBar.setBounds (0, 0, baseWidth, TopBar::height);
        addAndMakeVisible (inputMeter);
        inputMeter.setTopLeftPosition (16, 60);
        inputMeter.setComponentID ("input_meter");
        addAndMakeVisible (outputMeter);
        outputMeter.setTopLeftPosition (1124, 60);
        outputMeter.setComponentID ("output_meter");
        addAndMakeVisible (spectrum);
        spectrum.setTopLeftPosition (84, 60);

        // ---- 中层 ----
        addAndMakeVisible (particles);
        particles.setBounds (0, ParticleField::top, baseWidth, ParticleField::bandHeight);
        addAndMakeVisible (glazeTitle);
        // T-038：标题收窄到曲风 / 交付两个选择之间（x 470 ～ 730，Max 2026-10-01 选「放在 GLAZE 标题行」）：三者的区域不重叠，
        // 点选择时不会被标题吃掉。标题内容按宽度居中（中心仍是 600），英文最长一版（GLAZE · smart mastering · 校准中）约 242 px，放得下。
        glazeTitle.setBounds (470, 305, 260, 20);
        addAndMakeVisible (glazeKnob);
        // Keep the knob's interaction coordinates while giving more canvas space to the cards.
        glazeKnob.setTopLeftPosition (0, 0);
        glazeKnob.setTransform (juce::AffineTransform::scale (200.0f / (float) GlazeKnob::size).translated (500.0f, 322.0f));

        styleSelector.setOptions ({ "", "", "", "" });
        styleSelector.setFont (uiFont (12.0f, Weight::medium));
        styleSelector.setSelectedColours (0, { palette::violetSoft, palette::violet });
        addAndMakeVisible (styleSelector);
        styleAttachment = std::make_unique<SegmentedAttachment> (styleSelector, *state.getParameter (param::glazeStyle));

        // T-038 曲风与交付：与风格同一种选择器（带灰字说明格），分放 GLAZE 主旋钮左右——声音从左边流进 GLAZE、从右边流出，
        // 曲风说的是进来的音乐，交付说的是出去的母带（位置待 Max 看截图定，设计规格-曲风与交付 第 5 节）。选项文字在 setLanguage 里填。
        genreSelector.setOptions ({ "", "", "", "" });
        deliverySelector.setOptions ({ "", "", "" });

        for (auto* selector : { &genreSelector, &deliverySelector })
        {
            selector->setFont (uiFont (12.0f, Weight::medium));
            selector->setEqualWidths (false);
            addAndMakeVisible (*selector);
        }

        genreAttachment = std::make_unique<SegmentedAttachment> (genreSelector, *state.getParameter (param::glazeGenre));
        deliveryAttachment = std::make_unique<SegmentedAttachment> (deliverySelector, *state.getParameter (param::glazeDelivery));

        addAndMakeVisible (relearnButton);

        abSelector.setOptions ({ "", "" });
        abSelector.setEqualWidths (false);
        abSelector.setFont (uiFont (11.5f, Weight::medium));
        abSelector.setPrefix (0, "A");
        abSelector.setPrefix (1, "B");
        abSelector.setSelectedColours (0, { palette::c100, palette::c800 });
        abSelector.setSelectedColours (1, { palette::ink, juce::Colours::white });
        addAndMakeVisible (abSelector);

        addAndMakeVisible (monitoringButton);
        monitoringButton.setBounds (24, 452, 290, 34);
        monitoringButton.setLook (PillButton::Look::attention);
        monitoringButton.onClick = [this]
        {
            editor.setListeningToOriginal (false);
            editor.setAuditioningDifference (false);
            editor.setReleasePreviewEnabled (false);
        };

        addAndMakeVisible (matchButton);
        matchAttachment = std::make_unique<juce::ParameterAttachment> (*state.getParameter (param::limitListen),
                                                                       [this] (float value) { matchOn = value >= 0.5f; });
        matchAttachment->sendInitialUpdate();
        matchButton.onClick = [this] { matchAttachment->setValueAsCompleteGesture (matchOn ? 0.0f : 1.0f); };

        addAndMakeVisible (cornerNote);
        cornerNote.setBounds (24, 494, 330, 46);
        addAndMakeVisible (legend);

        // ---- 下层：按处理顺序 ----
        const std::pair<ModuleCard*, int> cards[] = { { &balanceCard, 16 }, { &clayCard, 314 }, { &colorCard, 612 }, { &limitCard, 910 } };

        for (const auto& [card, x] : cards)
        {
            addAndMakeVisible (*card);
            card->setBounds (x, 560, ModuleCard::width, ModuleCard::height);
        }

        // ---- 浮在最上面的 ----
        addChildComponent (dismissLayer);
        dismissLayer.setBounds (getLocalBounds().withSize (baseWidth, baseHeight));
        dismissLayer.onDismiss = [this] { editor.showSettings (false); editor.showHelp (false); editor.showAdvanced (false); };

        addChildComponent (settingsPanel);
        settingsPanel.setTopLeftPosition (856 - PopoverCard::shadowMargin, 58 - PopoverCard::shadowMargin);
        settingsPanel.onClose = [this] { editor.showSettings (false); };

        addChildComponent (helpPanel);
        helpPanel.onClose = [this] { editor.showHelp (false); };

        addChildComponent (advancedPanel);
        advancedPanel.onClose = [this] { editor.showAdvanced (false); };

        addChildComponent (bubble);

        valueEntry.setFont (monoFont (12.0f));
        valueEntry.setJustification (juce::Justification::centred);
        valueEntry.setSelectAllWhenFocused (true);
        valueEntry.setIndents (4, 2);
        valueEntry.onReturnKey = [this] { finishTextEntry (true); };
        valueEntry.onEscapeKey = [this] { finishTextEntry (false); };
        valueEntry.onFocusLost = [this] { finishTextEntry (true); };
        addChildComponent (valueEntry);

        addChildComponent (resizeHud);
        resizeHud.setTopLeftPosition (480, 350);

        // 气泡是圆角的：TooltipWindow 默认不透明，圆角外的四角会露出一块底色。设成透明，让画布画到四角下面。
        tooltipWindow.setOpaque (false);

        // 组件 ID：截图工具按它找控件、模拟操作（卡片里的旋钮 / 滑条 / 三态 / 电源键按参数 ID 命名）。
        glazeKnob.setComponentID (param::glazeAmount);
        styleSelector.setComponentID (param::glazeStyle);
        genreSelector.setComponentID (param::glazeGenre);
        deliverySelector.setComponentID (param::glazeDelivery);
        abSelector.setComponentID ("ab");
        matchButton.setComponentID ("match");
        relearnButton.setComponentID ("relearn");
        cornerNote.setComponentID ("corner_note");
        glazeTitle.setComponentID ("glaze_title");
        legend.setComponentID ("legend");
        monitoringButton.setComponentID ("monitoring");
        valueEntry.setComponentID ("value_entry");
        bubble.setComponentID ("value_bubble");
        topBar.getLanguageSelector().setComponentID ("language");
        topBar.getAdvancedButton().setComponentID (uiid::advanced);
        advancedPanel.setComponentID ("advanced_panel");

        // 粒子带（画布 y 300 ～ 560）里的文字小部件（T-032 R9-01）：粒子最多 30 帧 / s 地重画这条带，macOS 只拿脏矩形的
        // 包围盒，带里的组件每帧都被拖进来重画、重新排版文字。各自缓存成按物理像素的图、整数对齐贴；它们的状态一改就
        // repaint，缓存随之作废重画。主旋钮自管整颗缓存（GlazeKnob），粒子的柔光与流线也是缓存图（ParticleField）。
        for (auto* widget : std::initializer_list<juce::Component*> { &glazeTitle, &styleSelector, &relearnButton, &abSelector,
                                                                      &matchButton, &monitoringButton, &cornerNote, &legend,
                                                                      &genreSelector, &deliverySelector })
            setBufferedToAlignedImage (*widget);

        startTimerHz (10);
    }

    ~Canvas() override
    {
        // 之后析构的成员（卡片里的旋钮等）若正在拖动，会回来收气泡：气泡与输入框在它们之前就析构了，这里先关掉这些服务。
        destroying = true;
        stopTimer();
    }

    //==============================================================================
    Language getLanguage() const noexcept override   { return language; }

    void showValueBubble (juce::Component& source, juce::Point<float> anchor, const juce::String& text, bool fine) override
    {
        if (destroying)
            return;

        bubbleOwner = &source;
        bubble.show (text, fine, tr (Text::fine, language));

        const auto point = getLocalPoint (&source, anchor);
        const auto w = bubble.getPreferredWidth();
        auto bounds = juce::Rectangle<int> (juce::roundToInt (point.x) - w / 2, juce::roundToInt (point.y) - ValueBubble::height, w, ValueBubble::height);
        bubble.setBounds (bounds.constrainedWithin (getLocalBounds().reduced (4)));

        if (! bubble.isVisible())
        {
            bubble.setVisible (true);
            bubble.toFront (false);
        }
    }

    void hideValueBubble (juce::Component& source) override
    {
        if (! destroying && bubbleOwner == &source)
        {
            bubble.setVisible (false);
            bubbleOwner = nullptr;
        }
    }

    void beginTextEntry (juce::Component& source, juce::Rectangle<int> area, const juce::String& initialText,
                         std::function<void (const juce::String&)> onCommit) override
    {
        if (destroying)
            return;

        finishTextEntry (true);
        entryCommit = std::move (onCommit);

        auto bounds = getLocalArea (&source, area);
        bounds = bounds.withSizeKeepingCentre (juce::jmax (64, bounds.getWidth()), juce::jmax (22, bounds.getHeight()));
        valueEntry.setBounds (bounds.constrainedWithin (getLocalBounds()));
        valueEntry.setText (initialText, false);
        valueEntry.setVisible (true);
        valueEntry.toFront (true);
        valueEntry.grabKeyboardFocus();
        valueEntry.selectAll();
    }

    void setControlHint (juce::Component* control) override
    {
        if (destroying)
            return;

        hintActive = control != nullptr;
        editor.timerCallback();   // 立刻换左下角的文字，不等下一帧
    }

    bool isHintActive() const noexcept   { return hintActive; }
    double getStaticLayerRenderMs() const noexcept   { return lastStaticLayerMs; }

    // Windows：Alt 单击打开输入框后松开 Alt，这个 WM_SYSKEYUP 没人要的话 JUCE 会转给宿主窗口，系统随即激活宿主的菜单栏，
    // 接下来敲的数字进了菜单（juce_Windowing_windows.cpp 的 WM_SYSKEYUP）。输入框开着时把按键抬起吃掉。
    bool keyStateChanged (bool isKeyDown) override
    {
        return ! isKeyDown && valueEntry.isVisible();
    }

    //==============================================================================
    void setLanguage (Language newLanguage)
    {
        language = newLanguage;
        topBar.setLanguage (language);
        inputMeter.setLanguage (language);
        outputMeter.setLanguage (language);
        spectrum.setLanguage (language);
        glazeTitle.setLanguage (language);
        glazeTitle.setTooltip (help::tooltipFor (uiid::glazeStatus, language));
        glazeKnob.setTooltip (help::tooltipFor (param::glazeAmount, language));

        styleSelector.setOptions ({ glazestatus::styleName (glaze::Style::automatic, language), glazestatus::styleName (glaze::Style::warm, language),
                                    glazestatus::styleName (glaze::Style::punchy, language), glazestatus::styleName (glaze::Style::clean, language) });
        styleSelector.setTooltip (help::tooltipFor (param::glazeStyle, language));
        genreSelector.setCaption (tr (Text::genreCaption, language));
        genreSelector.setOptions ({ tr (Text::genreGeneral, language), tr (Text::genreDeep, language),
                                    tr (Text::genreTech, language), tr (Text::genreMelodic, language) });
        genreSelector.setTooltip (help::tooltipFor (param::glazeGenre, language));
        deliverySelector.setCaption (tr (Text::deliveryCaption, language));
        deliverySelector.setOptions ({ tr (Text::deliveryStandard, language), tr (Text::deliveryStreaming, language),
                                       tr (Text::deliveryClub, language) });
        deliverySelector.setTooltip (help::tooltipFor (param::glazeDelivery, language));
        layoutGlazeChoices();
        relearnButton.setTooltip (help::tooltipFor (uiid::glazeRelearn, language));
        abSelector.setOptions ({ tr (Text::processed, language), tr (Text::original, language) });
        matchButton.setTooltip (help::tooltipFor (param::limitListen, language));
        legend.setLanguage (language);

        for (auto* card : std::initializer_list<ModuleCard*> { &clayCard, &balanceCard, &colorCard, &limitCard })
            card->setLanguage (language);

        settingsPanel.setLanguage (language);
        helpPanel.setLanguage (language);
        helpPanel.setTopLeftPosition (1180 - HelpPanel::cardWidth - PopoverCard::shadowMargin, 58 - PopoverCard::shadowMargin);
        advancedPanel.setLanguage (language);
        advancedPanel.setTopLeftPosition (1180 - AdvancedPanel::cardWidth - PopoverCard::shadowMargin, 58 - PopoverCard::shadowMargin);
        layoutBottomRow();
        repaint();
    }

    // 风格 · 重新聆听 | A / B · 等响：一排、整体居中在 x = 600（设计稿 200 ～ 1000，间隔 8）。宽度都没变时什么都不做。
    // 量宽度要排版文字：只在自然宽度可能变了时调——语言切换（setLanguage）、重新聆听 / 等响的文字或小橙点变了（编辑器定时器
    // 看 PillButton::setText / setDot 的返回值），不每帧调（T-032 R9-12）。
    void layoutBottomRow()
    {
        constexpr int y = 524, h = 30, gap = 8, dividerWidth = 1 + 2 * 6;
        const auto styleWidth = 232;
        const auto relearnWidth = relearnButton.getNaturalWidth();
        const auto abWidth = abSelector.getNaturalWidth();
        const auto matchWidth = matchButton.getNaturalWidth();
        const auto legendWidth = legend.getNaturalWidth();

        if (relearnWidth == relearnButton.getWidth() && abWidth == abSelector.getWidth() && matchWidth == matchButton.getWidth()
             && legendWidth == legend.getWidth())
            return;

        const auto total = styleWidth + gap + relearnWidth + gap + dividerWidth + gap + abWidth + gap + matchWidth;
        auto x = 600 - total / 2;

        styleSelector.setBounds (x, y, styleWidth, h);
        x += styleWidth + gap;
        relearnButton.setBounds (x, y, relearnWidth, h);
        x += relearnWidth + gap;
        dividerX = x + 6;
        x += dividerWidth + gap;
        abSelector.setBounds (x, y, abWidth, h);
        x += abWidth + gap;
        matchButton.setBounds (x, y, matchWidth, h);

        legend.setBounds (baseWidth - 24 - legendWidth, 504, legendWidth, 18);
        repaint (0, y - 2, baseWidth, h + 4);
    }

    // T-038 曲风 / 交付：放在 GLAZE 标题行左右（Max 2026-10-01 看两种截图后选定；原默认的「主旋钮左右」作废）。按文字宽度
    // （自然宽度随语言变，只在 setLanguage 里量），y 302 ～ 328，与标题（y 305 ～ 325）同一行：曲风右对齐到标题左沿 x = 470，
    // 交付左对齐到标题右沿 x = 730。频谱区下沿（y 298）在它们上面，主旋钮的数值弧在它们下面，都不重叠。
    void layoutGlazeChoices()
    {
        constexpr int top = 302, h = 26, genreRight = 470, deliveryLeft = 730;
        const auto genreWidth = genreSelector.getNaturalWidth();
        const auto deliveryWidth = deliverySelector.getNaturalWidth();

        genreSelector.setBounds (genreRight - genreWidth, top, genreWidth, h);
        deliverySelector.setBounds (deliveryLeft, top, deliveryWidth, h);
    }

    void noteZoomChanged (double zoom, bool fromUser)
    {
        if (! fromUser)
            return;

        resizeHud.setValues (tr (Text::windowSize, language), zoom);
        resizeHud.setVisible (true);
        resizeHud.toFront (false);
        hideHudAtMs = juce::Time::getMillisecondCounterHiRes() + resizeHudHoldMs;
    }

    void setDraggingSize (bool dragging)
    {
        draggingSize = dragging;

        if (! dragging)
            hideHudAtMs = juce::Time::getMillisecondCounterHiRes() + resizeHudHoldMs;
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        // 背景只有柔和的渐变、点阵与颗粒：最多按 2 倍像素生成（200 % × Retina 时整张图按 4 倍会到 60 MB），再拉伸贴上。
        const auto pixelScale = juce::jlimit (0.5f, maxStaticLayerScale, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (! juce::approximatelyEqual (pixelScale, wantedScale))
        {
            wantedScale = pixelScale;
            scaleChangedMs = juce::Time::getMillisecondCounterHiRes();
        }

        // 第一次（或还没有）直接生成；之后缩放变了先拉伸旧图，等像素比稳定再重画（见 timerCallback）。
        if (! staticLayer.isValid())
            renderStaticLayer (pixelScale);

        g.setColour (juce::Colours::black);
        g.drawImageTransformed (staticLayer, juce::AffineTransform::scale ((float) baseWidth / (float) staticLayer.getWidth(),
                                                                           (float) baseHeight / (float) staticLayer.getHeight()), false);

        // 风格 / 重新聆听 与 A / B 之间的细分隔线
        g.setColour (palette::divider);
        g.fillRect ((float) dividerX, 530.0f, 1.0f, 18.0f);
    }

    //==============================================================================
    CeladonAudioProcessorEditor& editor;
    Language language = Language::chinese;

    TopBar topBar;
    LevelMeterPanel inputMeter { LevelMeterPanel::Kind::input }, outputMeter { LevelMeterPanel::Kind::outputAndReduction };
    SpectrumView spectrum;
    ParticleField particles;
    GlazeTitle glazeTitle;
    GlazeKnob glazeKnob;
    Segmented styleSelector { Segmented::Look::bordered };
    std::unique_ptr<SegmentedAttachment> styleAttachment;
    Segmented genreSelector { Segmented::Look::bordered }, deliverySelector { Segmented::Look::bordered };   // T-038
    std::unique_ptr<SegmentedAttachment> genreAttachment, deliveryAttachment;
    PillButton relearnButton { PillButton::Icon::relearn };
    Segmented abSelector { Segmented::Look::bordered };
    PillButton matchButton { PillButton::Icon::match };
    PillButton monitoringButton { PillButton::Icon::match };
    std::unique_ptr<juce::ParameterAttachment> matchAttachment;
    bool matchOn = false;
    CornerNote cornerNote;
    ColourLegend legend;
    ClayCard clayCard;
    BalanceCard balanceCard;
    ColorCard colorCard;
    LimitCard limitCard;

    // 浮层打开时铺满画布、接住别处的点击来关闭浮层（点击本身不再往下传）。
    struct DismissLayer  : public juce::Component
    {
        std::function<void()> onDismiss;
        void mouseDown (const juce::MouseEvent&) override   { if (onDismiss != nullptr) onDismiss(); }
    };

    DismissLayer dismissLayer;
    SettingsPanel settingsPanel;
    HelpPanel helpPanel;
    AdvancedPanel advancedPanel;
    ValueBubble bubble;
    juce::TextEditor valueEntry;
    ResizeHud resizeHud;
    juce::TooltipWindow tooltipWindow { this, 600 };

private:
    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (staticLayer.isValid() && ! juce::approximatelyEqual (staticLayerScale, wantedScale) && now - scaleChangedMs >= staticLayerSettleMs)
        {
            renderStaticLayer (wantedScale);
            repaint();
        }

        if (resizeHud.isVisible() && ! draggingSize && now >= hideHudAtMs)
            resizeHud.setVisible (false);
    }

    void finishTextEntry (bool commit)
    {
        if (! valueEntry.isVisible())
            return;

        const auto text = valueEntry.getText();
        valueEntry.setVisible (false);
        auto callback = std::move (entryCommit);
        entryCommit = nullptr;

        if (commit && callback != nullptr)
            callback (text);
    }

    // 静态背景：底色、三团柔光、点阵、细颗粒、面板阴影、卡片之间的箭头。按物理像素生成一次，之后每帧只是贴图。
    // 分三段：Graphics 画底 → BitmapData 加颗粒 → 再开一个 Graphics 画阴影与箭头。同一张图同一时刻只能有一个 Graphics 或
    // BitmapData（Windows 上默认的图像是 Direct2D 的，两个同时开着时颗粒会被丢掉）。
    // 打开编辑器与每次缩放停下都在消息线程同步生成一次（T-032 R9-08）：面板阴影的模糊图与像素比无关，只在第一次生成；
    // 颗粒逐像素查表。两处都与原来每次现算逐位相同。耗时记在 lastStaticLayerMs（截图工具的基准读它；Debug 版打日志）。
    void renderStaticLayer (float pixelScale)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto w = juce::jmax (1, juce::roundToInt ((float) baseWidth * pixelScale));
        const auto h = juce::jmax (1, juce::roundToInt ((float) baseHeight * pixelScale));
        staticLayer = juce::Image (juce::Image::RGB, w, h, false);
        staticLayerScale = pixelScale;
        const auto toLayer = juce::AffineTransform::scale ((float) w / (float) baseWidth, (float) h / (float) baseHeight);

        {
            juce::Graphics g (staticLayer);
            g.addTransform (toLayer);
            g.fillAll (palette::background);

            // 旋钮后面的大片白光（半径 470，72 % 处淡完）+ 左右两团很淡的青色。
            {
                const juce::Point<float> centre (600.0f, 422.0f);
                juce::ColourGradient white (juce::Colours::white.withAlpha (0.95f), centre, juce::Colours::white.withAlpha (0.0f), centre.translated (470.0f * 0.72f, 0.0f), true);
                g.setGradientFill (white);
                g.fillEllipse (juce::Rectangle<float> (940.0f, 940.0f).withCentre (centre));

                for (const auto x : { 216.0f, 984.0f })
                {
                    juce::Graphics::ScopedSaveState saved (g);
                    const juce::Point<float> blob (x, 422.0f);
                    g.addTransform (juce::AffineTransform::scale (1.0f, 197.0f / 408.0f, blob.x, blob.y));
                    juce::ColourGradient cyan (juce::Colour (23, 153, 171).withAlpha (0.09f), blob, juce::Colour (23, 153, 171).withAlpha (0.0f),
                                               blob.translated (408.0f * 0.7f, 0.0f), true);
                    g.setGradientFill (cyan);
                    g.fillEllipse (juce::Rectangle<float> (816.0f, 816.0f).withCentre (blob));
                }
            }

            // 点阵：每 14 px 一个半径 0.8 的小点（#0A4A57 8 %）。
            g.setColour (palette::c800.withAlpha (0.08f));

            for (float y = 1.0f; y < (float) baseHeight; y += 14.0f)
                for (float x = 1.0f; x < (float) baseWidth; x += 14.0f)
                    g.fillEllipse (x - 0.8f, y - 0.8f, 1.6f, 1.6f);
        }

        // 细颗粒：每个物理像素一点点随机明暗（设计稿 3.5 % 的灰噪声）；同一条线性同余序列逐像素取一档，查表加到三个通道上。
        {
            const juce::Image::BitmapData data (staticLayer, juce::Image::BitmapData::readWrite);
            const auto& lut = grainTable();
            std::uint32_t seed = 0x6772616eu;

            for (int y = 0; y < data.height; ++y)
            {
                auto* line = data.getLinePointer (y);

                for (int x = 0; x < data.width; ++x)
                {
                    seed = seed * 1664525u + 1013904223u;
                    const auto& adjust = lut[(size_t) (seed >> 24)];
                    auto* pixel = reinterpret_cast<juce::PixelRGB*> (line + (size_t) x * (size_t) data.pixelStride);
                    pixel->setARGB (255, adjust[pixel->getRed()], adjust[pixel->getGreen()], adjust[pixel->getBlue()]);
                }
            }
        }

        {
            juce::Graphics g (staticLayer);
            g.addTransform (toLayer);

            // 面板阴影：第一次用到时按这张图的临时图类型生成（与 DropShadow::drawForPath 用的同一种），之后只贴。
            if (panelShadows.empty())
                panelShadows = makePanelShadows (*g.getInternalContext().getPreferredImageTypeForTemporaryImages());

            for (const auto& shadow : panelShadows)
            {
                g.setColour (shadow.colour);
                g.drawImageAt (shadow.image, shadow.position.x, shadow.position.y, true);
            }

            // 卡片之间的小箭头：处理顺序
            for (const auto x : { 292.0f, 590.0f, 888.0f })
            {
                const juce::Rectangle<float> circle (x, 674.0f, 20.0f, 20.0f);
                g.setColour (palette::panel);
                g.fillEllipse (circle);
                g.setColour (palette::segmentBorder);
                g.drawEllipse (circle.reduced (0.5f), 1.0f);

                juce::Path arrow;
                arrow.startNewSubPath (circle.getX() + 8.5f, circle.getY() + 7.0f);
                arrow.lineTo (circle.getX() + 11.8f, circle.getY() + 10.0f);
                arrow.lineTo (circle.getX() + 8.5f, circle.getY() + 13.0f);
                g.setColour (palette::c600);
                g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

        lastStaticLayerMs = juce::Time::getMillisecondCounterHiRes() - started;
        DBG ("Celadon static layer " << w << " x " << h << ": " << juce::String (lastStaticLayerMs, 1) << " ms");
    }

    juce::Image staticLayer;
    float staticLayerScale = 0.0f, wantedScale = 0.0f;
    double scaleChangedMs = 0.0;
    std::vector<ShadowMask> panelShadows;        // 七块面板各两层阴影的模糊图（逻辑分辨率，生成一次）
    double lastStaticLayerMs = 0.0;
    double hideHudAtMs = 0.0;
    bool draggingSize = false;
    bool hintActive = false;
    bool destroying = false;
    int dividerX = 600;
    juce::Component* bubbleOwner = nullptr;
    std::function<void (const juce::String&)> entryCommit;
};

//==============================================================================
CeladonAudioProcessorEditor::CeladonAudioProcessorEditor (CeladonAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    canvas = std::make_unique<Canvas> (*this, processor);
    addAndMakeVisible (*canvas);
    canvas->setBounds (0, 0, baseWidth, baseHeight);

    // 画布上的按钮
    canvas->topBar.onLanguageChange = [this] (celadon::Language language) { settings->setLanguage (language); };
    canvas->topBar.onSettings = [this] { showSettings (! canvas->settingsPanel.isVisible()); };
    canvas->topBar.onHelp = [this] { showHelp (! canvas->helpPanel.isVisible()); };
    canvas->topBar.onAdvanced = [this] { showAdvanced (! canvas->advancedPanel.isVisible()); };
    canvas->limitCard.onAuditionClicked = [this] { setAuditioningDifference (! processor.isAuditioningDifference()); };
    canvas->advancedPanel.onLearnReference = [this] { processor.requestReferenceLearn(); };
    canvas->advancedPanel.onCancelReference = [this] { processor.cancelReferenceLearn(); };
    canvas->advancedPanel.onClearReference = [this] { processor.clearReference(); };
    canvas->advancedPanel.onPreviewChange = [this] (bool enabled) { setReleasePreviewEnabled (enabled); };
    canvas->relearnButton.onClick = [this] { if (glazeRelearnAction != nullptr) glazeRelearnAction(); };
    canvas->abSelector.onSelect = [this] (int index) { setListeningToOriginal (index == 1); };
    canvas->spectrum.getLoudnessHud().onReset = [this] { resetMeters(); };
    canvas->inputMeter.onResetClicked = [this] { resetMeters(); };
    canvas->outputMeter.onResetClicked = [this] { resetMeters(); };
    canvas->settingsPanel.onZoomChange = [this] (double zoom) { setZoom (zoom); };
    canvas->settingsPanel.onZoomDragging = [this] (bool dragging) { canvas->setDraggingSize (dragging); };
    canvas->settingsPanel.onReduceMotionChange = [this] (bool reduce) { settings->setReduceMotion (reduce); };

    // 等比缩放：右下角把手，宽高比锁死 1200 : 820，70 % ～ 200 %。
    if (auto* sizeConstrainer = getConstrainer())
    {
        sizeConstrainer->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
        sizeConstrainer->setSizeLimits (juce::roundToInt (baseWidth * minZoom), juce::roundToInt (baseHeight * minZoom),
                                        juce::roundToInt (baseWidth * maxZoom), juce::roundToInt (baseHeight * maxZoom));
    }

    setResizable (true, true);

    if (resizableCorner != nullptr)
    {
        auto listener = std::make_unique<CornerDragListener>();
        listener->onDrag = [safeCanvas = juce::Component::SafePointer<Canvas> (canvas.get())] (bool dragging)
        {
            if (safeCanvas != nullptr)
                safeCanvas->setDraggingSize (dragging);
        };
        resizableCorner->addMouseListener (listener.get(), false);
        cornerListener = std::move (listener);
    }

    if (auto* display = currentDisplay())
        canvas->settingsPanel.setScreenSize (display->logicalBounds.toNearestInt());

    settings->addChangeListener (this);
    processor.getParameters().state.addListener (this);

    // 频谱仪：扔掉编辑器关着时积压在取样口里的旧片段，再开始接收。
    auto& tap = processor.getAnalyzerTap();
    tap.setActive (false);
    tap.discard (celadon::AnalyzerTap::Stream::input);
    tap.discard (celadon::AnalyzerTap::Stream::output);
    tap.setActive (true);
    pullBuffer.resize ((size_t) celadon::AnalyzerTap::capacity);

    // 频谱仪跟屏幕刷新走（T-036）：频谱仪所在的显示器每次垂直同步推一帧（60 Hz 屏 60 帧、120 Hz 屏 120 帧，封顶 maxSpectrumFps）。
    spectrumVBlank = juce::VBlankAttachment (&canvas->spectrum, [this] (double timestampSeconds) { spectrumFrame (timestampSeconds); });

    applyLanguage();
    applyReduceMotion();
    setListeningToOriginal (processor.isComparingOriginal());
    setAuditioningDifference (processor.isAuditioningDifference());
    applyStoredZoom();

    lastTimerMs = juce::Time::getMillisecondCounterHiRes();
    timerCallback();
    startTimerHz (refreshHz);
}

CeladonAudioProcessorEditor::~CeladonAudioProcessorEditor()
{
    stopTimer();
    cancelPendingUpdate();
    spectrumVBlank = {};   // 先于画布（频谱仪组件）释放

    // 窗口关了就回到 A（处理后）、关掉试听差值：不能让听不到母带的状态在看不见的时候一直开着。
    processor.setCompareOriginal (false);
    processor.setAuditionDifference (false);
    processor.setReleasePreviewEnabled (false);
    processor.getAnalyzerTap().setActive (false);

    if (resizableCorner != nullptr && cornerListener != nullptr)
        resizableCorner->removeMouseListener (cornerListener.get());

    settings->removeChangeListener (this);
    processor.getParameters().state.removeListener (this);

    canvas.reset();
    setLookAndFeel (nullptr);
}

//==============================================================================
void CeladonAudioProcessorEditor::setGlazeStatusSource (GlazeStatusSource source)
{
    glazeStatusSource = std::move (source);
    timerCallback();
}

void CeladonAudioProcessorEditor::setGlazeRelearnAction (GlazeRelearnAction action)
{
    glazeRelearnAction = std::move (action);
}

void CeladonAudioProcessorEditor::setGlazeEffectiveParamsSource (GlazeEffectiveParamsSource source)
{
    glazeEffectiveParamsSource = std::move (source);
    timerCallback();
}

//==============================================================================
void CeladonAudioProcessorEditor::applyLanguage()
{
    canvas->setLanguage (settings->getLanguage());
    glazeTooltipValid = false;   // 上一行把 GLAZE 状态的两处气泡写回了通用版：House 档暂定期间要随即重设（T-038）
    timerCallback();
}

void CeladonAudioProcessorEditor::applyReduceMotion()
{
    const auto reduce = settings->getReduceMotion();
    canvas->particles.setReduceMotion (reduce);
    canvas->glazeKnob.setReduceMotion (reduce);
    canvas->topBar.setReduceMotion (reduce);
    canvas->settingsPanel.setReduceMotion (reduce);
}

void CeladonAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // 用户设置变了（本窗口或同一进程里别的 Celadon 窗口改的）：语言与动效立刻跟上。
    applyLanguage();
    applyReduceMotion();
}

void CeladonAudioProcessorEditor::setListeningToOriginal (bool listenToOriginal)
{
    if (listenToOriginal)
    {
        setReleasePreviewEnabled (false);
        setAuditioningDifference (false);
    }

    const auto changed = processor.isComparingOriginal() != listenToOriginal;
    processor.setCompareOriginal (listenToOriginal);
    canvas->abSelector.setSelected (listenToOriginal ? 1 : 0);
    canvas->glazeKnob.setComparing (listenToOriginal);
    canvas->particles.setComparing (listenToOriginal);

    if (changed)
        restartOutputSpectrum();
}

void CeladonAudioProcessorEditor::setAuditioningDifference (bool shouldAudition)
{
    // LIMIT 旁通时没有东西可听（链也不会生效）：不打开。打开时先回到 A——差值是处理后那一路削掉的部分。
    const auto audition = shouldAudition && rawValue (processor.getParameters(), param::limitBypass) < 0.5f;

    if (audition)
        setReleasePreviewEnabled (false);

    if (audition && processor.isComparingOriginal())
        setListeningToOriginal (false);

    const auto changed = processor.isAuditioningDifference() != audition;
    processor.setAuditionDifference (audition);
    canvas->limitCard.setAuditioning (audition);

    if (changed)
        restartOutputSpectrum();
}

void CeladonAudioProcessorEditor::setReleasePreviewEnabled (bool enabled)
{
    if (enabled)
    {
        setListeningToOriginal (false);
        setAuditioningDifference (false);
    }

    const auto changed = processor.isReleasePreviewEnabled() != enabled;
    processor.setReleasePreviewEnabled (enabled);
    if (changed)
        restartOutputSpectrum();
}

void CeladonAudioProcessorEditor::spectrumFrame (double timestampSeconds)
{
    // 垂直同步的一帧：两次回调连着时按帧时间戳之差算时长（帧与帧等距，动画最匀）；中间由定时器推过（垂直同步停过）就按墙钟。
    // 刷新率高于 maxSpectrumFps 的屏：离上一帧不到一帧时长就跳过这一次（时间戳不更新，时长累到下一帧），按整数分频、仍然等距。
    const auto now = juce::Time::getMillisecondCounterHiRes();
    lastVBlankMs = now;

    if (lastAdvanceWasVBlank && timestampSeconds > lastVBlankSeconds && timestampSeconds - lastVBlankSeconds < 0.9 / maxSpectrumFps)
        return;

    auto seconds = (now - lastSpectrumAdvanceMs) / 1000.0;

    if (lastAdvanceWasVBlank && timestampSeconds > lastVBlankSeconds)
        seconds = timestampSeconds - lastVBlankSeconds;

    lastVBlankSeconds = timestampSeconds;
    lastAdvanceWasVBlank = true;
    advanceSpectrum (seconds, now);
}

void CeladonAudioProcessorEditor::advanceSpectrum (double elapsedSeconds, double nowMs)
{
    // 频谱仪推一帧：取样口里新到的样本 → 计算器（按时长走动态）→ 视图（有变化才重画变了的那一块）。
    if (canvas == nullptr)
        return;

    lastSpectrumAdvanceMs = nowMs;
    const auto seconds = std::isfinite (elapsedSeconds) ? juce::jlimit (0.0, maxSpectrumStepSeconds, elapsedSeconds) : 0.0;

    analyser.setSampleRate (processor.getSampleRateForUi());
    auto& tap = processor.getAnalyzerTap();

    for (const auto stream : { celadon::AnalyzerTap::Stream::input, celadon::AnalyzerTap::Stream::output })
    {
        const auto count = tap.pull (stream, pullBuffer.data(), (int) pullBuffer.size());
        analyser.push (stream == celadon::AnalyzerTap::Stream::input ? SpectrumAnalyser::Stream::input : SpectrumAnalyser::Stream::output,
                       pullBuffer.data(), count);
    }

    // 窗口最小化 / 被宿主藏起来（有窗口但看不见）：样本照收（历史与电平不断档，取样口不积压），不做 FFT、不建路径、不重画；
    // 再显示时下一帧直接按最新的窗口分析（时长钳到 0.25 s，曲线一两帧就到位）。没有窗口的离屏编辑器（截图工具）照常算。
    // 被别的窗口挡住时 JUCE 拿不到，照常推（macOS 的遮挡状态待办）。
    if (auto* peer = canvas->spectrum.getPeer(); peer != nullptr && (peer->isMinimised() || ! canvas->spectrum.isShowing()))
        return;

    analyser.update (seconds);
    canvas->spectrum.setSpectrum (analyser, seconds);
}

void CeladonAudioProcessorEditor::restartOutputSpectrum()
{
    // 送出去的信号换了来源（A / B、试听差值）：频谱仪 OUT 扔掉积压的旧样本、曲线重来，峰值线不带着前一种信号（SpectrumAnalyser::restartOutput）。
    processor.getAnalyzerTap().discard (celadon::AnalyzerTap::Stream::output);
    analyser.restartOutput();
}

void CeladonAudioProcessorEditor::updateDismissLayer()
{
    // 三个浮层同一时刻最多开一个；开着时铺一层透明的「点别处关闭」。
    const auto anyOpen = canvas->settingsPanel.isVisible() || canvas->helpPanel.isVisible() || canvas->advancedPanel.isVisible();
    canvas->dismissLayer.setVisible (anyOpen);
}

void CeladonAudioProcessorEditor::showSettings (bool shouldShow)
{
    if (shouldShow)
    {
        showHelp (false);
        showAdvanced (false);
    }

    canvas->settingsPanel.setZoom ((double) getWidth() / (double) baseWidth);

    // 屏幕预览按插件窗口此刻所在的那块屏幕算（多屏、不同缩放比例时主屏幕不一定是它）。
    if (shouldShow)
        if (auto* display = currentDisplay())
            canvas->settingsPanel.setScreenSize (display->logicalBounds.toNearestInt());

    canvas->settingsPanel.setVisible (shouldShow);
    canvas->topBar.setSettingsOpen (shouldShow);
    updateDismissLayer();

    if (shouldShow)
    {
        canvas->dismissLayer.toFront (false);
        canvas->settingsPanel.toFront (false);
    }
}

void CeladonAudioProcessorEditor::showHelp (bool shouldShow)
{
    if (shouldShow)
    {
        showSettings (false);
        showAdvanced (false);
    }

    canvas->helpPanel.setVisible (shouldShow);
    canvas->topBar.setHelpOpen (shouldShow);
    updateDismissLayer();

    if (shouldShow)
    {
        canvas->dismissLayer.toFront (false);
        canvas->helpPanel.toFront (false);
    }
}

void CeladonAudioProcessorEditor::showAdvanced (bool shouldShow)
{
    if (shouldShow)
    {
        showSettings (false);
        showHelp (false);
    }

    canvas->advancedPanel.setVisible (shouldShow);
    canvas->topBar.setAdvancedOpen (shouldShow);
    updateDismissLayer();

    if (shouldShow)
    {
        canvas->dismissLayer.toFront (false);
        canvas->advancedPanel.toFront (false);
    }
}

void CeladonAudioProcessorEditor::resetMeters()
{
    processor.resetMeters();
    truePeakMaxDb = -120.0f;
    canvas->inputMeter.resetHolds();
    canvas->outputMeter.resetHolds();
}

//==============================================================================
void CeladonAudioProcessorEditor::setZoom (double zoom)
{
    zoom = juce::jlimit (minZoom, maxZoom, std::isfinite (zoom) ? zoom : 1.0);
    canvas->noteZoomChanged (zoom, true);
    setSize (juce::roundToInt (baseWidth * zoom), juce::roundToInt (baseHeight * zoom));
}

double CeladonAudioProcessorEditor::readStoredZoom() const
{
    const auto& state = processor.getParameters().state;

    // 工程里存过就用工程的；没有（新实例、v0.1.7 的旧工程）用用户设置里的默认大小。
    const auto stored = state.hasProperty (uiZoomId) ? (double) state.getProperty (uiZoomId) : settings->getDefaultZoom();
    return std::isfinite (stored) ? juce::jlimit (minZoom, maxZoom, stored) : 1.0;
}

const juce::Displays::Display* CeladonAudioProcessorEditor::currentDisplay() const
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    // 已经在屏幕上：就是它所在的那块。还没显示（刚打开）：鼠标所在的那块——用户刚在宿主里点开插件，宿主窗口多半就在那儿。
    if (isShowing())
        if (auto* display = displays.getDisplayForRect (getScreenBounds()))
            return display;

    if (auto* display = displays.getDisplayForPoint (juce::Desktop::getMousePosition().toFloat()))
        return display;

    return displays.getPrimaryDisplay();
}

double CeladonAudioProcessorEditor::fitZoomToScreen (double zoom) const
{
    // 不超过所在屏幕的可用区域（去掉任务栏 / Dock / 菜单栏；再给宿主的插件窗口标题栏与边框留 40 × 80 px）：Windows 笔记本常见的
    // 1920 × 1080 @ 150 % 只有 1280 × 720 逻辑像素，100 % 的 1200 × 820 下半截卡片会跑到屏幕外。最小仍是 70 %。
    if (auto* display = currentDisplay())
    {
        const auto area = display->userBounds;
        const auto fit = juce::jmin (((double) area.getWidth() - 40.0) / (double) baseWidth, ((double) area.getHeight() - 80.0) / (double) baseHeight);

        if (fit > 0.0)
            return juce::jmin (zoom, juce::jmax (minZoom, fit));
    }

    return zoom;
}

void CeladonAudioProcessorEditor::applyStoredZoom()
{
    const auto stored = readStoredZoom();
    const auto zoom = fitZoomToScreen (stored);
    const auto width = juce::roundToInt (baseWidth * zoom);
    const auto height = juce::roundToInt (baseHeight * zoom);

    // 为放进屏幕而缩小的这一次不写回（resized 里跳过 storeZoom）：工程与默认大小仍记着用户选的，换到大屏幕上打开时照旧。
    // 用户拖角、设置里拖滑杆、宿主改窗口大小照常写回。
    const juce::ScopedValueSetter<bool> fitting (fittingZoomToScreen, zoom < stored);

    if (width != getWidth() || height != getHeight())
        setSize (width, height);
}

void CeladonAudioProcessorEditor::storeZoom (double zoom)
{
    const juce::ScopedValueSetter<bool> guard (writingZoom, true);
    processor.getParameters().state.setProperty (uiZoomId, zoom, nullptr);
    settings->setDefaultZoom (zoom);
}

void CeladonAudioProcessorEditor::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    // 画布保持 1200 × 820，整块按窗口等比缩放；宿主给了不合宽高比的尺寸时取较小的比例，多出来的边由底色填。
    const auto scale = juce::jmin ((float) getWidth() / (float) baseWidth, (float) getHeight() / (float) baseHeight);
    canvas->setTransform (juce::AffineTransform::scale (scale));

    const auto zoom = (double) getWidth() / (double) baseWidth;
    canvas->settingsPanel.setZoom (zoom);
    canvas->noteZoomChanged (zoom, (resizableCorner != nullptr && resizableCorner->isMouseButtonDown()) || canvas->resizeHud.isVisible());

    if (! fittingZoomToScreen)
        storeZoom (zoom);
}

void CeladonAudioProcessorEditor::paint (juce::Graphics& g)
{
    // 画布（带缩放变换的子组件）盖住的整像素矩形挖掉，只填画布之外：宽高取整留下的缝、宿主给了不合宽高比的尺寸时多出来的边。
    const auto covered = juce::Rectangle<float> ((float) baseWidth, (float) baseHeight).transformedBy (canvas->getTransform());
    const auto inner = juce::Rectangle<int>::leftTopRightBottom ((int) std::ceil (covered.getX()), (int) std::ceil (covered.getY()),
                                                                 (int) std::floor (covered.getRight()), (int) std::floor (covered.getBottom()));
    if (! inner.isEmpty())
        g.excludeClipRegion (inner);

    g.fillAll (palette::outside);
}

void CeladonAudioProcessorEditor::valueTreeRedirected (juce::ValueTree&)
{
    triggerAsyncUpdate();
}

void CeladonAudioProcessorEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // 根节点上的 uiZoom 被别人改了（宿主读工程）才跟：参数子节点的 value 变化在这里直接跳过。
    if (! writingZoom && property == uiZoomId && tree == processor.getParameters().state)
        triggerAsyncUpdate();
}

void CeladonAudioProcessorEditor::handleAsyncUpdate()
{
    applyStoredZoom();
}

//==============================================================================
void CeladonAudioProcessorEditor::advanceForSnapshot (double seconds)
{
    // 离屏没有垂直同步：频谱仪按给定的时长推一帧（不按墙钟，两次运行逐像素相同）。随后的 timerCallback 不再推：打上 snapshotDriven，
    // 定时器的看门狗整个关掉。原来靠「把最近一次垂直同步记成现在」让看门狗以为垂直同步还活着，但 timerCallback 里的 now 是之后才取的——
    // 这一帧（或 timerCallback 的前半段、或机器被占满时线程被切走）耗时 ≥ spectrumVBlankTimeoutMs，看门狗照样按墙钟再推一次，
    // 出图随机器快慢变（评审 R-06 想堵的就是这个；T-070 评审 F9 / T-072 查出没堵住）。
    snapshotDriven = true;
    lastAdvanceWasVBlank = false;
    advanceSpectrum (seconds, juce::Time::getMillisecondCounterHiRes());
    timerCallback();
    canvas->particles.advance (seconds);
}

double CeladonAudioProcessorEditor::getStaticLayerRenderMs() const noexcept
{
    return canvas != nullptr ? canvas->getStaticLayerRenderMs() : 0.0;
}

void CeladonAudioProcessorEditor::timerCallback()
{
    if (canvas == nullptr)
        return;

    auto& state = processor.getParameters();
    auto& c = *canvas;
    const auto language = settings->getLanguage();
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto elapsed = juce::jlimit (0.0, 0.25, (now - lastTimerMs) / 1000.0);
    lastTimerMs = now;

    const auto glazePercent = rawValue (state, param::glazeAmount);
    const auto style = glazestatus::styleFromParameterValue (rawValue (state, param::glazeStyle));
    const auto listenOn = rawValue (state, param::limitListen) >= 0.5f;
    const auto comparing = processor.isComparingOriginal();
    const auto mastering = processor.getMasteringStatus();
    const auto ceiling = rawValue (state, param::limitCeiling) - mastering.codecMarginDb;
    c.advancedPanel.setStatus (mastering);
    const auto status = glazeStatusSource != nullptr ? glazeStatusSource() : glaze::Status {};

    // 试听差值：LIMIT 被旁通（界面、宿主自动化都算）就退出；耳机跟着处理器的状态。
    if (processor.isAuditioningDifference() && rawValue (state, param::limitBypass) >= 0.5f)
        setAuditioningDifference (false);

    const auto auditioning = processor.isAuditioningDifference();
    const auto monitoring = comparing || auditioning || mastering.previewEnabled;
    c.monitoringButton.setVisible (monitoring);
    if (monitoring)
    {
        c.monitoringButton.setText (tr (comparing ? Text::listeningOriginal : auditioning ? Text::auditioningDifference : Text::releasePreview, language));
        c.monitoringButton.setTooltip (tr (Text::returnToMaster, language));
    }

    if (c.limitCard.isAuditioning() != auditioning)
        c.limitCard.setAuditioning (auditioning);

    const auto view = glazestatus::classify (status, glazePercent, style, listenOn);
    const auto planLimited = view.kind == glazestatus::Kind::planLimited;

    if (c.abSelector.getSelected() != (comparing ? 1 : 0))
        setListeningToOriginal (comparing);

    // ---- 电平表与响度 ----
    const auto inputPeak = processor.getAndResetInputTruePeakDb();
    const auto outputPeak = processor.getAndResetOutputTruePeakDb();
    const auto limitGr = processor.getAndResetLimitGainReductionDb();
    const auto glueGr = processor.getAndResetGlueGainReductionDb();

    // 听原声时 OUT 是原声：它超过 Ceiling 不算问题（不亮橙灯、不进响度小窗的最大值）。
    c.inputMeter.pushReadings (inputPeak, 0.0f, std::numeric_limits<float>::infinity(), false, elapsed);
    c.outputMeter.pushReadings (outputPeak, limitGr, comparing ? std::numeric_limits<float>::infinity() : ceiling, planLimited, elapsed);
    c.colorCard.setGlueReduction (glueGr);
    c.limitCard.setLimitReduction (limitGr, planLimited);

    if (! comparing && std::isfinite (outputPeak))
        truePeakMaxDb = juce::jmax (truePeakMaxDb, outputPeak);

    LoudnessHud::Values loudness;
    loudness.integratedLufs = processor.getOutputIntegratedLufs();
    loudness.shortTermLufs = processor.getOutputShortTermLufs();
    loudness.truePeakMaxDb = truePeakMaxDb;
    loudness.ceilingDb = ceiling;

    // 目标标记照「已锁定」那一行：House 反相提示（T-038）压住状态行时照常显示（提示说的是混音，不改目标）。
    auto targetView = view;

    if (view.kind == glazestatus::Kind::housePhaseHint)
    {
        auto unhinted = status;
        unhinted.housePhaseHint = false;
        targetView = glazestatus::classify (unhinted, glazePercent, style, listenOn);
    }

    loudness.hasTarget = targetView.kind == glazestatus::Kind::locked && targetView.hasTarget;
    loudness.targetLufs = (float) targetView.targetTenthsLufs / 10.0f;
    c.spectrum.getLoudnessHud().setValues (loudness);

    // ---- 频谱仪：平时由垂直同步推（spectrumFrame，T-036）；垂直同步 100 ms 没来（离屏、宿主 / 系统不给）就由这里按 30 Hz 推；
    //      截图工具驱动的编辑器（snapshotDriven）每一帧都自己给时长，这里不补推 ----
    if (! snapshotDriven && now - lastVBlankMs >= spectrumVBlankTimeoutMs && now - lastSpectrumAdvanceMs >= spectrumMinStepMs)
    {
        lastAdvanceWasVBlank = false;
        advanceSpectrum ((now - lastSpectrumAdvanceMs) / 1000.0, now);
    }

    // ---- 粒子：数量跟 GLAZE，速度与亮度跟输出电平（上次取走以来、约 33 ms 里全部 OUT 样本的 RMS）----
    const auto rms = analyser.takeRecentRmsDb (SpectrumAnalyser::Stream::output);

    if (rms > -70.0f)
        lastSoundMs = now;

    const auto playing = now - lastSoundMs < soundHangMs && ! status.waitingForPlayback;
    c.particles.setGlaze (glazePercent / 100.0f);
    c.particles.setAudio (rms, playing);

    // ---- 卡片：GLAZE 的实际值（紫色）、修坯读数、Balance 的 27 段 ----
    {
        const auto glazeActive = glazePercent > 0.0f && glazeEffectiveParamsSource != nullptr;
        const auto effective = glazeEffectiveParamsSource != nullptr ? glazeEffectiveParamsSource() : ChainParams {};
        c.balanceCard.setEffective (effective, glazeActive);
        c.colorCard.setEffective (effective, glazeActive);
        c.limitCard.setEffective (effective, glazeActive);

        help::ClayUserState user;
        user.lowCutAuto = juce::roundToInt (rawValue (state, param::clayLowCutMode)) == 0;
        user.monoAuto   = juce::roundToInt (rawValue (state, param::clayMonoMode)) == 0;
        user.widthAuto  = juce::roundToInt (rawValue (state, param::clayWidthMode)) == 0;
        user.bypassed   = rawValue (state, param::clayBypass) >= 0.5f;
        const auto readout = help::clayReadoutFor (status, user);
        c.clayCard.setReadout (readout);
        c.spectrum.setClayMarkers ((float) readout.lowCutHz, readout.monoPercent > 0 ? (float) readout.monoHz : 0.0f, ! readout.bypassed);

        BalanceModule::BandGains gains {};
        processor.getBalanceBandGainsDb (gains);
        c.balanceCard.setBandGains (gains);
    }

    // ---- 中层：阶段标签、左下角、重新聆听、等响 ----
    {
        using Kind = glazestatus::Kind;
        using Chip = GlazeTitle::Chip;

        auto chip = Chip::locked;

        // House 反相提示（T-038）压住的是哪一行，阶段标签就照哪一行画：提示不改变「聆听中 / 已锁定」。
        switch (view.kind == Kind::housePhaseHint ? view.underlyingKind : view.kind)
        {
            case Kind::listening:
            case Kind::houseListening:      chip = Chip::listening;   break;
            case Kind::calibrating:         chip = Chip::calibrating; break;
            case Kind::calibrationPaused:   chip = Chip::paused;      break;
            case Kind::unlearned:
            case Kind::unlearnedExported:   chip = Chip::unlearned;   break;
            case Kind::planLimited:
            case Kind::limitedByManual:     chip = Chip::limited;     break;
            case Kind::awaitingCalibration:
            case Kind::locked:
            case Kind::profileMismatch:
            case Kind::housePhaseHint:      chip = Chip::locked;      break;
        }

        c.glazeTitle.setChip (chip);

        // T-038：House 档锁定条件还没满足（含还没开始播放）时，GLAZE 状态的悬停气泡换成 House 版、左下第二行写
        //「从 drop 开始播放能学得更快」（设计规格-曲风-House学习方式 第 7 节）。气泡只在 House 与否或语言变了时重设
        //（标题与顶栏状态胶囊两处；Canvas::setLanguage / TopBar::setLanguage 会写回通用版，applyLanguage 让这里随后重设）。
        const auto houseTip = glazestatus::houseLearningTipApplies (status);

        if (! glazeTooltipValid || houseTip != glazeTooltipHouse || language != glazeTooltipLanguage)
        {
            const auto tooltip = houseTip ? help::houseListeningTooltip (language) : help::tooltipFor (uiid::glazeStatus, language);
            c.glazeTitle.setTooltip (tooltip);
            c.topBar.getStatusPill().setTooltip (tooltip);
            glazeTooltipHouse = houseTip;
            glazeTooltipLanguage = language;
            glazeTooltipValid = true;
        }

        if (c.isHintActive())
        {
            c.cornerNote.setLines (tr (Text::hintDrag, language), tr (Text::hintClick, language), true);
        }
        else
        {
            juce::String second;

            if (! (glazePercent > 0.0f))
                second = tr (Text::glazeManual, language);
            else if (houseTip && (view.kind == Kind::houseListening || view.kind == Kind::housePhaseHint
                                  || view.kind == Kind::unlearned || view.kind == Kind::unlearnedExported))
                second = tr (Text::cornerHouseTip, language);
            else if (view.kind == Kind::listening)
                second = trFormat (Text::cornerListening, language, { juce::String (view.listenedSeconds), juce::String (view.requiredSeconds) });
            else if (view.kind == Kind::unlearned || view.kind == Kind::unlearnedExported)
                second = tr (Text::cornerUnlearned, language);
            else
                second = trFormat (Text::cornerListened, language, { juce::String (juce::roundToInt (status.requiredSeconds)) });

            c.cornerNote.setLines (tr (Text::drivesParameters, language), second, false);
        }

        // 底排按文字宽度摆放：重新聆听 / 等响的文字与小橙点都改自然宽度，只在它们变了时重排（语言切换由 Canvas::setLanguage 重排），
        // 不每帧量字（T-032 R9-12）。
        const auto mismatch = view.kind == Kind::profileMismatch;
        auto bottomRowChanged = c.relearnButton.setText (status.relearnInProgress ? tr (Text::relearnCancel, language) : tr (Text::relearn, language));
        c.relearnButton.setLook (mismatch ? PillButton::Look::attention : PillButton::Look::normal);
        bottomRowChanged = c.relearnButton.setDot (mismatch) || bottomRowChanged;

        const auto hasDifference = processor.hasListenDifference();
        const auto difference = processor.getListenDifferenceDb();   // 处理结果 − 原声
        bottomRowChanged = c.matchButton.setText (c.matchOn ? (hasDifference ? trFormat (Text::matchWithDifference, language, { juce::String (std::abs (difference), 1) })
                                                                             : tr (Text::match, language))
                                                            : tr (Text::matchOff, language))
                        || bottomRowChanged;
        c.matchButton.setLook (c.matchOn ? PillButton::Look::normal : PillButton::Look::off);

        if (bottomRowChanged)
            c.layoutBottomRow();

        // GLAZE 状态行的几段（两处用）：View 已量化到显示精度，与上次相同（且语言没变）就沿用上次拼好的。
        if (! glazeSegmentsValid || view != glazeSegmentsView || language != glazeSegmentsLanguage)
        {
            glazeSegments = glazestatus::segmentsFor (view, language);
            glazeSegmentsView = view;
            glazeSegmentsLanguage = language;
            glazeSegmentsValid = true;
        }

        // ---- 顶部状态胶囊：需要注意 > 听原声 / 试听差值 > GLAZE 状态 ----
        StatusPill::Content pill;
        const auto blocked = processor.getNonFiniteBlockCount();

        if (blocked != lastNonFiniteBlocks)
        {
            lastNonFiniteBlocks = blocked;
            nonFiniteWarningUntilMs = now + nonFiniteWarningMs;
        }

        // 真峰值超过 Ceiling：最近 3 s 内出现过才提示（听原声时不算）。
        if (! comparing && std::isfinite (outputPeak) && outputPeak > ceiling + 0.05f)
            lastOverMs = now;

        const auto truePeakOver = ! comparing && now - lastOverMs < 3000.0;

        if (blocked > 0 && now < nonFiniteWarningUntilMs)
        {
            pill.warning = true;
            pill.segments.add (trFormat (Text::warnNonFinite, language, { juce::String (blocked) }));
        }
        else if (truePeakOver)
        {
            pill.warning = true;
            pill.segments = splitSegments (trFormat (Text::warnTruePeak, language, { signedOneDecimal (juce::jmax (truePeakMaxDb, outputPeak)),
                                                                                       signedOneDecimal (ceiling) }));
        }
        else if (glazestatus::toneFor (view.kind) == glazestatus::Tone::amber && glazePercent > 0.0f)
        {
            pill.warning = true;
            pill.segments = glazeSegments;
        }
        else if (comparing)
        {
            pill.segments.add ("B");
            pill.segments.add (tr (Text::listeningOriginal, language));

            if (! hasDifference)
                pill.segments.add (tr (Text::differenceUnknown, language));
            else if (listenOn)
                pill.segments.add (trFormat (Text::matchedBy, language, { signedOneDecimal (difference) }));
            else
                pill.segments.add (trFormat (Text::notMatched, language, { juce::String (std::abs (difference), 1) }));

            pill.accentSegment = 1;
            pill.accentColour = palette::ink;
            pill.dotColour = palette::ink;
        }
        else if (auditioning)
        {
            // 与 B 同一种深色：此刻送出去的不是母带。表与响度仍是母带的（见 CeladonChain::setParams）。
            pill.segments.add ("LIMIT");
            pill.segments.add (tr (Text::auditioningDifference, language));
            pill.segments.add (tr (Text::auditionDifferenceNote, language));
            pill.accentSegment = 1;
            pill.accentColour = palette::ink;
            pill.dotColour = palette::ink;
        }
        else
        {
            pill.segments = glazeSegments;
            pill.accentSegment = pill.segments.size() > 1 ? 1 : -1;
            pill.accentColour = view.kind == Kind::locked && style == glaze::Style::automatic ? palette::violet : palette::c800;
            pill.dotColour = palette::c500;
            pill.pulse = view.kind == Kind::listening || view.kind == Kind::calibrating || view.kind == Kind::houseListening;
        }

       #if JUCE_DEBUG
        if (! pill.warning)
        {
            // Debug 构建的音频线程开销是 Release 的数倍（ADR-007）：万一装错进 DAW，一打开界面就看得见。
            pill = {};
            pill.warning = true;
            pill.segments.add (tr (Text::debugBuild, language));
        }
       #endif

        c.topBar.setStatus (pill);
    }

    // ---- 顶栏与设置面板里的延迟 ----
    const auto latency = processor.getLatencyMilliseconds();
    c.topBar.setLatency (latency);
    c.settingsPanel.setLatency (latency);
}
