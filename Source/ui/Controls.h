/*
  ==============================================================================

    Controls.h
    v0.1.8.0 界面改版的通用控件：旋钮、行滑条、可拖数值、分段选择、电源键（旁通）、耳机键（试听差值）、小胶囊开关、只读的衰减条。

    统一手感（V4 设计稿第 7 条）：
      - 拖动调节（旋钮上下拖、滑条左右拖；旋钮左右拖也算）；按住 ⌘ / Ctrl 慢 8 倍精调；
      - 双击回默认值；按住 Alt / Option 单击直接输入数值（回车确认、Esc 取消、点别处也算确认）；滚轮微调；
      - 拖动时在控件上方出数值气泡（精调时带「精调」小标签）；悬停 / 拖动时中层左下角换成操作提示。
    数值气泡、输入框、操作提示由编辑器画布实现（InteractionHost），控件只负责手感与自己的样子。

    参数一律经 juce::ParameterAttachment 连接：拖动 = 一次手势（begin / 若干 set / end），宿主自动化与撤销都按手势记；
    宿主或自动化改了参数，界面在消息线程收到回调重画。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Localisation.h"

#include <cmath>
#include <functional>
#include <optional>

namespace celadon::ui
{
    //==============================================================================
    // 编辑器画布提供给控件的服务。
    class InteractionHost
    {
    public:
        virtual ~InteractionHost() = default;

        virtual Language getLanguage() const noexcept = 0;

        // 数值气泡：anchor 是气泡底边中点（source 的本地坐标）。同一时刻只有一个；hide 只收起 source 自己的那个。
        virtual void showValueBubble (juce::Component& source, juce::Point<float> anchor, const juce::String& text, bool fine) = 0;
        virtual void hideValueBubble (juce::Component& source) = 0;

        // 直接输入数值：在 area（source 的本地坐标）上放一个小输入框，确认时回调 onCommit（取消不回调）。
        virtual void beginTextEntry (juce::Component& source, juce::Rectangle<int> area, const juce::String& initialText,
                                     std::function<void (const juce::String&)> onCommit) = 0;

        // 悬停 / 拖动某个可调控件（nullptr = 离开）：中层左下角换成操作提示。
        virtual void setControlHint (juce::Component* control) = 0;
    };

    //==============================================================================
    // 旋钮几何：270° 轨道从 7 点半（−135°）顺时针到 4 点半（+135°），正上方是正中；角度 0 = 12 点方向、顺时针为正
    //（与 juce::Path::addCentredArc 同一约定）。
    namespace knob
    {
        inline constexpr float startAngle = -0.75f * juce::MathConstants<float>::pi;
        inline constexpr float endAngle   =  0.75f * juce::MathConstants<float>::pi;

        inline float angleFor (float proportion) noexcept
        {
            return startAngle + juce::jlimit (0.0f, 1.0f, proportion) * (endAngle - startAngle);
        }

        // 圆上一点（角度约定同上）。
        inline juce::Point<float> pointAt (juce::Point<float> centre, float radius, float angle) noexcept
        {
            return { centre.x + radius * std::sin (angle), centre.y - radius * std::cos (angle) };
        }
    }

    // 画一段按单色青渐变分段上色的数值弧（设计稿 segArc）：从 from 到 to（0 ～ 1 的比例），每满量程 segmentsPerRange 段；
    // bipolar = 从正中往两边长，颜色按离中点的距离加深。两端补圆头。
    // saturation < 1 时整段去色（听原声时的主旋钮）。
    void drawValueArc (juce::Graphics& g, juce::Point<float> centre, float radius, float width,
                       float from, float to, int segmentsPerRange, bool bipolar, float saturation = 1.0f);

    // 深色旋钮帽：径向渐变（左上亮）+ 下方一圈柔和的影子。
    void drawKnobCap (juce::Graphics& g, juce::Point<float> centre, float radius);

    //==============================================================================
    // 按物理像素缓存的图，贴到整数设备像素上（T-032 R9-01 / 05 / 07、M-3）。
    //
    // 贴图若带小数平移或比例 ≠ 1，软件渲染器逐像素双线性重采样（juce_RenderingHelpers.h renderImage），CoreGraphics 也插值——
    // 又慢又把图糊掉半个像素。这里把坐标系换回设备像素（addTransform (scale (1 / pixelScale))）、最近邻、drawImageAt 整数偏移：
    // 比例精确为 1，软件渲染器走整块拷贝，CoreGraphics 不插值。上下文自身的小数平移（非整数缩放下组件原点不在整像素上）
    // 由最近邻吸附到最近的整像素，位置误差 ≤ 0.5 px、不糊。
    // 上下文此刻的像素比与图不一致时（缩放窗口后、新图还没按新像素比生成的那几帧）照常按比例变换贴图。
    //
    // image 的像素 (0, 0) 对应「本地逻辑坐标 × pixelScale」下的整数偏移 deviceOrigin；opacity 是整张图的不透明度。
    void drawCachedImage (juce::Graphics& g, const juce::Image& image, float pixelScale, juce::Point<int> deviceOrigin, float opacity = 1.0f);

    // 此刻上下文的物理像素比（非有限或 ≤ 0 时按 1）：缓存图按它生成。
    float physicalPixelScale (juce::Graphics& g) noexcept;

    // 一张按物理像素生成、整数对齐贴的缓存图：覆盖本地逻辑区域 area 在 pixelScale 下的整像素范围。
    struct AlignedImage
    {
        juce::Image image;
        float pixelScale = 0.0f;              // 生成时的物理像素比（0 = 还没有）
        juce::Point<int> deviceOrigin;        // 像素 (0, 0) 在「本地坐标 × pixelScale」下的整数偏移

        // 与此刻上下文的像素比完全一致才算有效（按实际像素比生成，贴图才是整块拷贝）。
        bool isValidFor (float scale) const noexcept   { return image.isValid() && juce::exactlyEqual (pixelScale, scale); }

        // 重新生成：透明 ARGB 图，paintContent 以本地逻辑坐标作画（已加好缩放与平移）。
        void render (juce::Rectangle<float> area, float scale, const std::function<void (juce::Graphics&)>& paintContent);
        void draw (juce::Graphics& g, float opacity = 1.0f) const   { drawCachedImage (g, image, pixelScale, deviceOrigin, opacity); }
    };

    // 组件缓存成按物理像素的图（R9-01：粒子带里的文字小部件）。失效规则与 setBufferedToImage 相同——组件自己 repaint 时
    // 整块或局部作废、下次 paint 重画；不同的只有贴图：按上下文实际像素比生成、drawCachedImage 整数对齐（JUCE 自带的
    // StandardCachedComponentImage 用 drawImageTransformed 小数平移贴图，每次双线性重采样、文字糊半个像素）。
    // 局部作废后的重画按作废区裁：每块乘像素比、向外取整到设备像素——小数像素比下也只重画碰到作废区的那些设备像素，
    // 组件 paint 里按 clipRegionIntersects 跳过的绘制照样跳过（T-041；JUCE 那套「排除有效区」在小数像素比下退化成整块重画）。
    class AlignedComponentCache  : public juce::CachedComponentImage
    {
    public:
        explicit AlignedComponentCache (juce::Component& ownerToUse) noexcept   : owner (ownerToUse) {}

        void paint (juce::Graphics&) override;
        bool invalidateAll() override                                   { validArea.clear(); return true; }
        bool invalidate (const juce::Rectangle<int>& area) override     { validArea.subtract (area); return true; }
        void releaseResources() override                                { image = {}; validArea.clear(); }

    private:
        juce::Component& owner;
        juce::Image image;
        float pixelScale = 0.0f;
        juce::RectangleList<int> validArea;

        JUCE_DECLARE_NON_COPYABLE (AlignedComponentCache)
    };

    // component.setCachedComponentImage (new AlignedComponentCache (component))。
    void setBufferedToAlignedImage (juce::Component& component);

    //==============================================================================
    // 参数值的显示文字：参数自己的格式（「+3.0」「25」），ASCII 连字符换成 U+2212 减号；withUnit 时接单位（「+3.0 dB」）。
    juce::String formatParameterValue (const juce::RangedAudioParameter& parameter, float value, bool withUnit);

    // 把用户输入的文字解析成参数值：认 U+2212、忽略单位与空格；解析不出返回空。结果按参数范围钳制并吸附到步进。
    std::optional<float> parseParameterValue (const juce::RangedAudioParameter& parameter, const juce::String& text);

    //==============================================================================
    // 连着一个参数、可拖动的控件的公共部分：手势、精调、双击回默认、Alt 单击输入、滚轮、悬停提示、数值气泡。
    class ParameterControl  : public juce::Component,
                              public juce::SettableTooltipClient
    {
    public:
        ParameterControl (juce::RangedAudioParameter& parameter, InteractionHost& host);
        ~ParameterControl() override;

        juce::RangedAudioParameter& getParameter() noexcept        { return parameter; }
        float getValue() const noexcept                            { return value; }
        float getProportion() const noexcept;
        juce::String getValueText (bool withUnit) const            { return formatParameterValue (parameter, value, withUnit); }

        // 参数变了（拖动、输入、宿主自动化）之后调用，在重画之后。
        std::function<void()> onValueChange;

        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        // 精调：拖动与滚轮慢 8 倍（设计规格要求至少 4 倍）。
        static constexpr double fineFactor = 0.125;

    protected:
        // 子类实现：数值气泡的锚点（本地坐标）与直接输入框的位置。
        virtual juce::Point<float> getBubbleAnchor() const = 0;
        virtual juce::Rectangle<int> getTextEntryArea() const = 0;

        // 能不能调（CLAY 的手动值只在「手动」时可调）。
        virtual bool isAdjustable() const   { return isEnabled(); }

        // 拖动：beginDrag 在按下时调（手势懒启动：真正动了才 beginGesture，双击回默认不会嵌在手势里）；
        // dragTo 以连续比例设值（内部按参数步进吸附）；endDrag 在松开时调。
        void beginDrag (bool fineAtStart);
        void dragTo (double proportion, bool fineNow);
        void endDrag();
        bool isDragging() const noexcept                   { return dragActive; }
        double getDragProportion() const noexcept          { return dragProportion; }

        // 一次完整的改动（滚轮、双击、输入）。
        void setValueAsCompleteGesture (float newValue);

        // Alt / Option 单击：打开输入框。子类在 mouseDown 里先调它，返回 true 就不要再当成拖动。
        bool handleTextEntryClick (const juce::MouseEvent& e);

        // 参数回调之后给子类的机会（默认重画）。
        virtual void parameterValueChanged()               { repaint(); }

        InteractionHost& host;

    private:
        void updateBubble();
        void setValueFromAttachment (float newValue);

        juce::RangedAudioParameter& parameter;
        std::unique_ptr<juce::ParameterAttachment> attachment;
        float value = 0.0f;
        float defaultValue = 0.0f;

        bool dragActive = false;       // 按下后还没松开
        bool gestureActive = false;    // 已经向宿主 beginGesture
        bool fine = false;
        double dragProportion = 0.0;   // 未经步进吸附的连续位置，步进参数拖起来才不会「卡格」

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterControl)
    };

    //==============================================================================
    // 模块卡片里的小旋钮（72 × 100）：名字（上）+ 54 px 旋钮 + 数值（下，GLAZE 的自动加量紫色接在后面）。
    class ParamKnob  : public ParameterControl
    {
    public:
        struct Options
        {
            bool bipolar = false;        // 从正中往两边长（Tone、Brighten、Output……）
            int autoDecimals = 0;        // 自动加量显示几位小数（整数量 0，dB 量 1）
        };

        ParamKnob (juce::RangedAudioParameter& parameter, InteractionHost& host, Options options);

        void setLabel (const juce::String& newName);

        // GLAZE 叠加后的实际值（参数单位）；空 = 不显示（GLAZE 0）。只在看得出变化时重画。
        void setEffectiveValue (std::optional<float> effective);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        static constexpr int width = 72;
        static constexpr int height = 100;

    protected:
        juce::Point<float> getBubbleAnchor() const override;
        juce::Rectangle<int> getTextEntryArea() const override;

    private:
        juce::String autoAmountText() const;

        Options options;
        juce::String name;
        std::optional<float> effectiveValue;
        float effectiveBaseValue = 0.0f;          // 上次 setEffectiveValue 时的用户值（加量文字只由它与 effectiveValue 决定）
        juce::String shownAutoText;               // 上次重画时的自动加量文字与实际值比例（-1 = 不显示）
        float shownEffectiveProportion = -1.0f;
        juce::Point<float> lastDragPosition;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamKnob)
    };

    //==============================================================================
    // 卡片下半部分的行滑条（246 × 24）：名字 70 px + 轨道 + 数值 60 px（+ 可选的小胶囊，如释放的 AUTO）。
    class ParamSlider  : public ParameterControl
    {
    public:
        ParamSlider (juce::RangedAudioParameter& parameter, InteractionHost& host, bool bipolar);

        void setLabel (const juce::String& newName);

        // 右边留给小胶囊的宽度（0 = 没有）；胶囊本身是另一个组件，由卡片摆放。
        void setTrailingSpace (int pixels);
        void setValueWidth (int pixels);

        // 变暗（例如 AUTO 释放打开时，手动的 Release 不起作用）：照样能拖。
        void setDimmed (bool shouldBeDimmed);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        static constexpr int nameWidth = 70;
        static constexpr int valueWidth = 60;
        static constexpr int gap = 8;

    protected:
        juce::Point<float> getBubbleAnchor() const override;
        juce::Rectangle<int> getTextEntryArea() const override;

    private:
        juce::Rectangle<float> getTrackArea() const;
        double proportionAtX (float x) const;

        bool bipolar = false;
        bool dimmed = false;
        int trailingSpace = 0;
        int valueAreaWidth = valueWidth;
        juce::String name;
        float lastDragX = 0.0f;
        bool fineDrag = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSlider)
    };

    //==============================================================================
    // 可拖的数值文字（CLAY 每行右边的手动值）：只在 setAdjustable (true) 时能调，否则只是显示文字（由所有者设）。
    class DragValue  : public ParameterControl
    {
    public:
        DragValue (juce::RangedAudioParameter& parameter, InteractionHost& host);

        // 不可调时显示的文字与颜色（此刻的实际值、「未启用」……）；可调时显示参数值（深色）。
        void setDisplay (const juce::String& text, juce::Colour colour);
        void setAdjustable (bool shouldBeAdjustable);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    protected:
        juce::Point<float> getBubbleAnchor() const override;
        juce::Rectangle<int> getTextEntryArea() const override;
        bool isAdjustable() const override             { return adjustable; }

    private:
        juce::String displayText;
        juce::Colour displayColour;
        bool adjustable = false;
        bool hovering = false;
        juce::Point<float> lastDragPosition;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragValue)
    };

    //==============================================================================
    // 分段选择器（风格、三态、速度、语言、斜率、A / B……）。外观按 Style 选，选中项的颜色可按下标单独指定（「自动」是紫色）。
    class Segmented  : public juce::Component,
                       public juce::SettableTooltipClient
    {
    public:
        enum class Look
        {
            groove,     // 浅灰底槽，选中项白底 + 阴影（CLAY 三态、Balance 速度）
            bordered,   // 白底描边容器，选中项染色（GLAZE 风格、A / B）
            language,   // 顶栏语言：灰底槽，选中项白底
            compact     // 频谱仪里的斜率：无容器，选中项浅青底
        };

        struct Colours
        {
            juce::Colour background, text;
        };

        explicit Segmented (Look look);

        void setOptions (const juce::StringArray& labels);
        void setSelected (int index, bool notify = false);
        int getSelected() const noexcept                         { return selected; }

        // 选中某一项时的底色与字色（默认按 Look）。
        void setSelectedColours (int index, Colours colours);

        // 每项等宽（true，默认）或按文字宽度（false，两边各留 padding）。
        void setEqualWidths (bool shouldBeEqual)                 { equalWidths = shouldBeEqual; repaint(); }
        void setFont (const juce::Font& newFont)                 { font = newFont; repaint(); }
        void setPrefix (int index, const juce::String& prefix);  // A / B 的「A」「B」：等宽粗体接在名字前

        // 容器里最左边的一格灰字说明（T-038：GLAZE 两侧的「曲风」「交付」），不可点、后面一道细竖线，各项排在它右边。
        // 空串 = 没有（默认；其余选择器的样子与点按逐像素不变）。
        void setCaption (const juce::String& newCaption);

        // 按文字算出来的自然宽度（setEqualWidths (false) 时有用；有说明格时含说明格）。
        int getNaturalWidth() const;

        std::function<void (int)> onSelect;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> segmentBounds (int index) const;
        int segmentAt (juce::Point<float> position) const;
        float itemWidth (int index) const;
        float captionWidth() const;   // 说明格的宽度（含右边细竖线的空隙）；没有说明时 0

        Look look;
        juce::String caption;
        juce::StringArray labels, prefixes;
        juce::Array<std::optional<Colours>> selectedColours;
        juce::Font font;
        int selected = 0;
        int hovered = -1;
        bool equalWidths = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Segmented)
    };

    // 把 Segmented 连到一个选项参数（AudioParameterChoice / Bool）：下标 = 参数值。
    class SegmentedAttachment
    {
    public:
        SegmentedAttachment (Segmented& control, juce::RangedAudioParameter& parameter);
        ~SegmentedAttachment();

        std::function<void (int)> onChange;   // 参数变了（含宿主）之后

    private:
        Segmented& control;
        juce::ParameterAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE (SegmentedAttachment)
    };

    //==============================================================================
    // 卡片右上角的电源键（旁通）：亮 = 模块在工作（青底白标），暗 = 已旁通。连 *_bypass 参数（取反）。
    class PowerButton  : public juce::Component,
                         public juce::SettableTooltipClient
    {
    public:
        explicit PowerButton (juce::RangedAudioParameter& bypassParameter);

        bool isModuleOn() const noexcept   { return on; }
        std::function<void()> onChange;

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
        void mouseExit (const juce::MouseEvent&) override    { repaint(); }

    private:
        juce::ParameterAttachment attachment;
        bool on = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerButton)
    };

    //==============================================================================
    // 小胶囊开关（释放的 AUTO）：开 = 紫底紫字（自动），关 = 白底灰字。连一个布尔参数。
    class ChipToggle  : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        explicit ChipToggle (juce::RangedAudioParameter& parameter);

        void setText (const juce::String& newText)   { text = newText; repaint(); }
        bool isOn() const noexcept                    { return on; }
        int getNaturalWidth() const;

        std::function<void()> onChange;

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::ParameterAttachment attachment;
        juce::String text;
        bool on = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChipToggle)
    };

    //==============================================================================
    // 只读的衰减行（粘合衰减、限幅衰减）：名字 + 条 + 「1.4 dB」。warning 时换橙色（方案受限）。
    class ReductionRow  : public juce::Component,
                          public juce::SettableTooltipClient
    {
    public:
        explicit ReductionRow (float fullScaleDb);

        void setLabel (const juce::String& newName);

        // 界面定时器每帧调：内部有与电平表同样的下落（20 dB/s）；只有看得出变化时才重画。
        void pushReading (float reductionDb, bool warning);
        void paint (juce::Graphics&) override;

    private:
        float fullScale;
        float shownDb = 0.0f;
        bool shownWarning = false;
        juce::String name, shownText;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReductionRow)
    };

    //==============================================================================
    // 按住的小按钮 / 图标按钮的通用底：悬停、按下、点击回调。
    class ClickableComponent  : public juce::Component,
                                public juce::SettableTooltipClient
    {
    public:
        std::function<void()> onClick;

        bool isHovering() const noexcept   { return hovering; }
        bool isPressed() const noexcept    { return pressed; }

        void mouseEnter (const juce::MouseEvent&) override   { hovering = true;  repaint(); }
        void mouseExit (const juce::MouseEvent&) override    { hovering = false; repaint(); }
        void mouseDown (const juce::MouseEvent&) override    { pressed = true;   repaint(); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            pressed = false;
            repaint();

            if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
                onClick();
        }

    private:
        bool hovering = false, pressed = false;
    };

    //==============================================================================
    // 卡片头的耳机键（LIMIT 的「试听差值」，22 × 22，挨在电源键左边）：关 = 白底灰图标（设计稿），开 = 深色底白图标——
    // 与 A / B 选中 B 同一种颜色：「此刻听到的不是母带」。不连参数（试听差值只是界面状态）；禁用时（模块旁通）35 % 淡、不接点击。
    class HeadphoneButton  : public ClickableComponent
    {
    public:
        HeadphoneButton();

        void setOn (bool shouldBeOn);
        bool isOn() const noexcept   { return on; }

        void paint (juce::Graphics&) override;
        void enablementChanged() override;

    private:
        bool on = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeadphoneButton)
    };
}
