/*
  ==============================================================================

    PluginEditor.cpp

  ==============================================================================
*/

#include "PluginEditor.h"

#include "ParameterIds.h"
#include "ui/CrackleTexture.h"
#include "ui/GlazeStatusText.h"

#include <functional>
#include <iterator>
#include <string_view>

namespace
{
    namespace palette = celadon::ui::palette;

    constexpr int refreshHz = 33;                                          // 约 30 ms 刷新一次表
    constexpr float reductionFallDbPerTick = 20.0f / (float) refreshHz;    // 响度区 GR 数字下落 20 dB/s，与表同速

    // 开片纹：固定种子（每次打开都一样，设计规格-界面 第 1 节），背景约 40 条。
    constexpr juce::uint32 crackleSeed = 0x43454c41;   // "CELA"
    constexpr int crackleDensity = 40;

    //==============================================================================
    // 基准布局（900 × 560，设计规格-界面 第 3 节）。全是画布坐标，缩放由画布的变换统一处理，这里永远不变。
    namespace layout
    {
        constexpr int margin = 24;

        // 顶栏 0 ～ 52：左标题，中操作提示 / NaN 警告，右版本与延迟
        const juce::Rectangle<int> topBar     { 0, 0, 900, 52 };
        const juce::Rectangle<int> topLeft    { margin, 0, 260, 52 };
        const juce::Rectangle<int> topCentre  { 250, 0, 400, 52 };
        const juce::Rectangle<int> topRight   { 640, 0, 900 - margin - 640, 52 };

        // 总览区 52 ～ 280：左三条竖表，中 GLAZE 总旋钮，右响度读数
        constexpr int meterWidth = 48;
        constexpr int meterGap   = 8;
        const juce::Rectangle<int> meterInArea  { margin,                              60, meterWidth, 212 };
        const juce::Rectangle<int> meterOutArea { margin + (meterWidth + meterGap),     60, meterWidth, 212 };
        const juce::Rectangle<int> meterGrArea  { margin + 2 * (meterWidth + meterGap), 60, meterWidth, 212 };

        const juce::Rectangle<int> glazeKnobArea  { 360, 61, 180, 168 };   // 滑条区 132 → 本体 116（规格），圆心与原来一致
        const juce::Rectangle<int> glazeTitleArea { 330, 234, 240, 18 };

        // GLAZE 左边一列（v0.6）：风格四选一 +「重新聆听」，76 × 132，与大旋钮的滑条区（y 79 ～ 211）上下对齐；
        // 横向居中在表组右缘（x 184）与旋钮本体左缘（x 384）正中（x 284），胶囊离两边各约 64 px。
        const juce::Rectangle<int> glazeStyleArea { 246, 79, celadon::ui::GlazeStyleColumn::preferredWidth,
                                                    celadon::ui::GlazeStyleColumn::preferredHeight };

        // 状态行（v0.6，取代 v0.4 的短说明、同一行位置）：以旋钮为中心，宽 300——最长的一条（约 250 px）放得下，
        // 右缘正好到响度读数的左缘（x 600）为止、不越过去。
        const juce::Rectangle<int> glazeStatusArea { 300, 252, 300, 18 };

        const juce::Rectangle<int> readoutArea { 600, 60, 900 - margin - 600, 212 };

        // 模块区 290 ～ 522：三块 280 × 232（x = 16 / 310 / 604），之间 14 px 放小箭头
        constexpr int panelY      = 290;
        constexpr int panelWidth  = 280;
        constexpr int panelHeight = 232;
        constexpr int panelX[]    = { 16, 310, 604 };

        // ---- 修坯（v0.1.7.0，设计规格-Clay 第 5 节；定稿布局不动，只加下面三样）----
        // 模块间小箭头的 y：290 + 232 / 2 = 406（paintStaticText 画箭头用同一个值）。
        constexpr int moduleArrowY = panelY + panelHeight / 2;

        // 三灯指示：BALANCE 右缘（296）与 COLOR 左缘（310）之间 14 px 的空当，竖向盖住三盏灯与下面的箭头——灯心在箭头上方
        // 27 / 18 / 9 px（ClayIndicator 的几何：灯心 9、18、27，箭头落在组件内 y = 36），鼠标停在箭头上也出修坯的气泡。
        constexpr int clayIndicatorArrowOffset = 36;
        const juce::Rectangle<int> clayIndicatorArea { panelX[0] + panelWidth, moduleArrowY - clayIndicatorArrowOffset,
                                                       panelX[1] - (panelX[0] + panelWidth),
                                                       celadon::ui::ClayIndicator::preferredHeight };

        // 高级抽屉：盖住下半区三块面板，同位置同大小（x 16 ～ 884，y 290 ～ 522）。
        const juce::Rectangle<int> advancedDrawerArea { panelX[0], panelY, panelX[2] + panelWidth - panelX[0], panelHeight };

        // 顶栏「高级」胶囊：样式同开关 72 × 22，在顶栏里垂直居中；右缘离「版本 · 延迟」文字（按最宽的算）12 px。
        constexpr int advancedWidth  = 72;
        constexpr int advancedHeight = 22;
        constexpr int advancedGap    = 12;
    }

    // 顶栏右边「v0.1.7.0 · 延迟 5.9 ms」。updateTopBar 与「高级」胶囊的定位用同一个格式，改文字时两处自动一致。
    juce::String topBarRightText (double latencyMs)
    {
        return "v" + juce::String (JucePlugin_VersionString)
                 + celadon::utf8 (" · 延迟 ") + juce::String (latencyMs, 1) + " ms";
    }

    //==============================================================================
    // 「主文字 + 次文字」并排：左边中粗（可带字距）、右边常规灰青，如顶栏「CELADON 青瓷」、「GLAZE 施釉」。
    void drawTitlePair (juce::Graphics& g, juce::Rectangle<int> area,
                        const juce::String& primary,   const juce::Font& primaryFont,   juce::Colour primaryColour,
                        const juce::String& secondary, const juce::Font& secondaryFont, juce::Colour secondaryColour,
                        bool centred)
    {
        constexpr int gap = 8;
        const auto primaryWidth   = juce::GlyphArrangement::getStringWidthInt (primaryFont, primary);
        const auto secondaryWidth = juce::GlyphArrangement::getStringWidthInt (secondaryFont, secondary);
        const auto totalWidth     = primaryWidth + gap + secondaryWidth;

        auto x = centred ? area.getCentreX() - totalWidth / 2 : area.getX();

        g.setFont (primaryFont);
        g.setColour (primaryColour);
        g.drawText (primary, x, area.getY(), primaryWidth + 2, area.getHeight(), juce::Justification::centredLeft, false);

        x += primaryWidth + gap;
        g.setFont (secondaryFont);
        g.setColour (secondaryColour);
        g.drawText (secondary, x, area.getY(), secondaryWidth + 2, area.getHeight(), juce::Justification::centredLeft, false);
    }

    //==============================================================================
    // 组件左上角在「这次绘制的物理像素」坐标里的位置（可以带小数）。顶层组件的原点就是屏幕像素的原点（插件窗口、
    // 离屏渲染都如此）；中间各层的位置与变换（画布的等比缩放）由 getLocalPoint 换算，physicalScale 是本组件
    // 1 个逻辑像素对应的物理像素。
    juce::Point<float> physicalOriginOf (const juce::Component& component, float physicalScale)
    {
        const auto* top = component.getTopLevelComponent();
        const auto origin = top->getLocalPoint (&component, juce::Point<float>());
        const auto unitX  = top->getLocalPoint (&component, juce::Point<float> (1.0f, 0.0f));
        const auto componentToTop = origin.getDistanceFrom (unitX);   // 本组件 1 个逻辑像素 = 顶层的多少逻辑像素

        return origin * (componentToTop > 0.0f ? physicalScale / componentToTop : physicalScale);
    }

    //==============================================================================
    // GLAZE 大旋钮的图片缓存（v0.5 R4）。机制与 JUCE 的 setBufferedToImage 相同：组件（或它的子组件）要求重画时
    // JUCE 调 invalidate，下一次绘制重新渲染；没人要求重画时，被 macOS 合成的外接矩形扫到只是贴一张图。
    // 不直接用 setBufferedToImage，因为要与 v0.4 画出来一样：
    //   - JUCE 自带的缓存图按「组件尺寸 × 缩放」取整、贴回时再拉伸到组件范围，界面缩放不是整数倍时（80 %、113 % × Retina……）
    //     旋钮被重采样，细弧与开片纹发虚、错开零点几个像素（实测 0.8 × 1 最大差 94 / 255）。这里让图的像素格与屏幕像素格
    //     对齐：按组件在屏幕上的小数偏移渲染进图，贴回时正好是整数平移，不重采样。
    //   - 透明底的缓存图再叠到背景上，半透明边缘的舍入与直接画不同（整数倍缩放下差 2 ～ 5 / 255）。旋钮下面只有画布的
    //     静态层（底色 + 开片纹），所以先把静态层对应的那块逐像素拷进图当底、再画旋钮，得到一张不透明图，每一步都与
    //     直接画在屏幕上相同（实测整数倍缩放下差 ≤ 1 / 255）。静态层还没生成、或像素比对不上时退回透明底（只差舍入）。
    class PixelAlignedImageCache final : public juce::CachedComponentImage
    {
    public:
        // 底图：画布静态层与它的物理像素比；画布位于顶层原点，静态层的像素 (x, y) 就是屏幕像素 (x, y)。
        struct Backdrop
        {
            const juce::Image* image = nullptr;
            float physicalScale = 0.0f;
        };

        PixelAlignedImageCache (juce::Component& componentToCache, std::function<Backdrop()> backdropSource)
            : owner (componentToCache), getBackdrop (std::move (backdropSource)) {}

        void paint (juce::Graphics& g) override
        {
            const auto scale  = juce::jmax (0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());
            const auto origin = physicalOriginOf (owner, scale);
            const juce::Point<int> whole ((int) std::floor (origin.x), (int) std::floor (origin.y));
            const juce::Point<float> fraction (origin.x - (float) whole.x, origin.y - (float) whole.y);

            const auto width  = juce::jmax (1, (int) std::ceil (fraction.x + (float) owner.getWidth()  * scale - 1.0e-3f));
            const auto height = juce::jmax (1, (int) std::ceil (fraction.y + (float) owner.getHeight() * scale - 1.0e-3f));

            const auto backdrop = getBackdrop != nullptr ? getBackdrop() : Backdrop {};
            const auto opaque = backdrop.image != nullptr && backdrop.image->isValid()
                                 && std::abs (backdrop.physicalScale - scale) < 1.0e-3f
                                 && whole.x >= 0 && whole.y >= 0
                                 && whole.x + width  <= backdrop.image->getWidth()
                                 && whole.y + height <= backdrop.image->getHeight();

            // 缩放、小数偏移或底图可用与否变了（拖大小、换屏幕），图要按新的像素格重建。
            if (! image.isValid() || image.getWidth() != width || image.getHeight() != height || opaque != imageIsOpaque
                 || std::abs (scale - imageScale) > 1.0e-4f || fraction.getDistanceFrom (imageFraction) > 1.0e-3f)
            {
                image = juce::Image (opaque ? juce::Image::RGB : juce::Image::ARGB, width, height, ! opaque);
                imageScale = scale;
                imageFraction = fraction;
                imageIsOpaque = opaque;
                valid = false;
            }

            if (! valid)
            {
                // 整张重画（不按作废的小块补画）：小块的边在屏幕上可能落在半个像素上，补画会留缝；旋钮只有 GLAZE 变了、
                // 悬停 / 拖动时才作废，整张重画与 v0.4 每次直接画开销相当。
                juce::Graphics imageGraphics (image);

                if (opaque)
                    imageGraphics.drawImageAt (*backdrop.image, -whole.x, -whole.y);   // 逐像素拷贝，不缩放
                else
                    image.clear (image.getBounds());

                imageGraphics.addTransform (juce::AffineTransform::scale (scale).translated (fraction.x, fraction.y));
                owner.paintEntireComponent (imageGraphics, true);
                valid = true;
            }

            // 贴回：先减去小数偏移、再按 1 / 缩放换回本组件的逻辑坐标；与当前变换合起来是整数平移。
            g.setColour (juce::Colours::black.withAlpha (owner.getAlpha()));
            g.drawImageTransformed (image, juce::AffineTransform::translation (-fraction.x, -fraction.y).scaled (1.0f / scale), false);
        }

        bool invalidateAll() override                               { valid = false; return true; }
        bool invalidate (const juce::Rectangle<int>&) override      { valid = false; return true; }
        void releaseResources() override                            { image = {}; valid = false; }

    private:
        juce::Component& owner;
        std::function<Backdrop()> getBackdrop;
        juce::Image image;
        float imageScale = 0.0f;
        juce::Point<float> imageFraction;
        bool imageIsOpaque = false;
        bool valid = false;

        JUCE_DECLARE_NON_COPYABLE (PixelAlignedImageCache)
    };

    //==============================================================================
    // 受 GLAZE 影响的 7 个参数（设计规格-GLAZE-决策与校准 第 5、8 节；= glaze::AutoIncrements 的 7 个字段）：
    // 参数 ID + 从「交给模块的实际参数」里取该项的函数。顺序 = 信号顺序（Balance → Color → Limit）。
    struct GlazedParameter
    {
        const char* parameterId;
        float (*effectiveValue) (const celadon::ChainParams&) noexcept;
    };

    constexpr GlazedParameter glazedParameters[] =
    {
        { celadon::param::balanceRecover, [] (const celadon::ChainParams& p) noexcept { return p.balance.recover; } },
        { celadon::param::balanceTame,    [] (const celadon::ChainParams& p) noexcept { return p.balance.tame; } },
        { celadon::param::colorTone,      [] (const celadon::ChainParams& p) noexcept { return p.color.toneDb; } },
        { celadon::param::colorGlue,      [] (const celadon::ChainParams& p) noexcept { return p.color.glue; } },
        { celadon::param::colorHeat,      [] (const celadon::ChainParams& p) noexcept { return p.color.heat; } },
        { celadon::param::limitGain,      [] (const celadon::ChainParams& p) noexcept { return p.limit.gainDb; } },
        { celadon::param::limitClip,      [] (const celadon::ChainParams& p) noexcept { return p.limit.clip; } },
    };

    // AutoIncrements 是 7 个 float：以后给它加一项（多一个受 GLAZE 影响的参数）而这里忘了加，编译就停在这里。
    static_assert (sizeof (celadon::glaze::AutoIncrements) == std::size (glazedParameters) * sizeof (float),
                   "glaze::AutoIncrements 的项数变了：同步改 glazedParameters 与 PluginEditor.h 的 numGlazedKnobs");

    const GlazedParameter* findGlazedParameter (const char* parameterId) noexcept
    {
        if (parameterId == nullptr)
            return nullptr;

        for (const auto& item : glazedParameters)
            if (std::string_view (item.parameterId) == parameterId)
                return &item;

        return nullptr;
    }
}

//==============================================================================
CeladonAudioProcessorEditor::Canvas::Canvas() = default;
CeladonAudioProcessorEditor::Canvas::~Canvas() = default;

void CeladonAudioProcessorEditor::Canvas::setTopBarTexts (const juce::String& newCentreText, bool newCentreIsWarning,
                                                          const juce::String& newRightText)
{
    if (centreText == newCentreText && centreIsWarning == newCentreIsWarning && rightText == newRightText)
        return;

    centreText      = newCentreText;
    centreIsWarning = newCentreIsWarning;
    rightText       = newRightText;
    repaint (layout::topBar);
}

void CeladonAudioProcessorEditor::Canvas::paintStaticText (juce::Graphics& g)
{
    // ---- 顶栏左：标题 ----
    drawTitlePair (g, layout::topLeft,
                   "CELADON", celadon::ui::celadonFont (22.0f, true).withExtraKerningFactor (6.0f / 22.0f), palette::text,
                   celadon::utf8 ("青瓷"), celadon::ui::celadonFont (13.0f), palette::textDim,
                   false);

    // ---- GLAZE 标题：旋钮下方「GLAZE 施釉」（设计规格-界面 第 2 节）。下一行在 v0.4 是 12 px 灰青短说明「拧大：更密、更响」，
    // v0.6 换成会变的状态行（GlazeStatusLine 组件，不在静态层里）----
    drawTitlePair (g, layout::glazeTitleArea,
                   "GLAZE", celadon::ui::celadonFont (13.0f, true).withExtraKerningFactor (2.0f / 13.0f), palette::text,
                   celadon::utf8 ("施釉"), celadon::ui::celadonFont (13.0f), palette::textDim,
                   true);

    // ---- 模块间的小箭头：信号顺序 BALANCE → COLOR → LIMIT ----
    g.setColour (palette::textDim);
    const auto arrowY = (float) layout::panelY + (float) layout::panelHeight * 0.5f;

    for (int i = 0; i < 2; ++i)
    {
        const auto gapStart = (float) (layout::panelX[i] + layout::panelWidth);
        const auto gapEnd   = (float) layout::panelX[i + 1];
        g.drawArrow ({ gapStart + 2.0f, arrowY, gapEnd - 2.0f, arrowY }, 1.2f, 6.0f, 5.0f);
    }
}

void CeladonAudioProcessorEditor::Canvas::refreshStaticLayerIfNeeded (float pixelScale)
{
    if (staticLayer.isValid() && std::abs (pixelScale - staticLayerScale) < 1.0e-3f)
        return;

    staticLayerScale = pixelScale;

    // 图盖住画布在屏幕上的整个范围（向上取整到整像素，比例不是整数时最后一列 / 行也盖得到）。
    const auto width  = juce::jmax (1, (int) std::ceil ((float) baseWidth  * pixelScale - 1.0e-3f));
    const auto height = juce::jmax (1, (int) std::ceil ((float) baseHeight * pixelScale - 1.0e-3f));

    // 不透明 RGB：贴回去时是整块拷贝，不做逐像素混合（v0.4 每帧把透明的开片纹图和底色、文字在外接矩形里重新混合一遍，
    // 是一帧里最贵的一项）。内存与 v0.4 缓存的整幅开片纹图同样大，那张图现在只在这里用一次、不再留着。
    staticLayer = juce::Image (juce::Image::RGB, width, height, false);

    juce::Graphics g (staticLayer);
    g.addTransform (juce::AffineTransform::scale (pixelScale));   // 与屏幕上同一个「逻辑 → 物理像素」映射
    g.fillAll (palette::background);

    // 开片纹：与 v0.4 同一张图、同一种画法（固定种子，按这个像素比生成，拉到画布的逻辑范围）。
    const auto crackle = celadon::ui::renderCrackleImage (baseWidth, baseHeight, pixelScale, crackleSeed, crackleDensity);

    if (crackle.isValid())
        g.drawImage (crackle, juce::Rectangle<float> ((float) baseWidth, (float) baseHeight));

    paintStaticText (g);
}

void CeladonAudioProcessorEditor::Canvas::paint (juce::Graphics& g)
{
    // 物理像素比取自这次绘制本身（画布缩放 × Retina × 宿主缩放全在里面），与静态层按同一个比例生成。
    refreshStaticLayerIfNeeded (juce::jmax (0.25f, g.getInternalContext().getPhysicalPixelScaleFactor()));

    // 按 1:1 贴回：当前变换 × (1 / 像素比) = 单位变换，图的每个像素正好落在一个屏幕像素上，不重采样；
    // 画出来与直接画底色、开片纹、文字逐像素相同。图是不透明的，贴完就盖满了整块画布。
    g.setColour (juce::Colours::black);   // 贴图按当前不透明度合成：显式设成完全不透明
    g.drawImageTransformed (staticLayer, juce::AffineTransform::scale (1.0f / staticLayerScale), false);

    // ---- 顶栏中、右两段会变的文字（与静态文字不重叠，后画不影响结果）----
    g.setFont (celadon::ui::celadonFont (12.0f));
    g.setColour (centreIsWarning ? palette::warning : palette::textDim);
    g.drawText (centreText, layout::topCentre, juce::Justification::centred, false);

    g.setColour (palette::textDim);
    g.drawText (rightText, layout::topRight, juce::Justification::centredRight, false);
}

//==============================================================================
CeladonAudioProcessorEditor::CeladonAudioProcessorEditor (CeladonAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    // 画布固定 900 × 560，先把所有子组件摆好，再决定窗口大小。
    addAndMakeVisible (canvas);
    canvas.setBounds (0, 0, baseWidth, baseHeight);

    buildOverview();
    buildModules();
    layoutCanvas();

    // 等比缩放：右下角把手，宽高比锁死 900 : 560，80% ～ 150%。
    if (auto* sizeConstrainer = getConstrainer())
    {
        sizeConstrainer->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
        sizeConstrainer->setSizeLimits (juce::roundToInt ((float) baseWidth  * minScale),
                                        juce::roundToInt ((float) baseHeight * minScale),
                                        juce::roundToInt ((float) baseWidth  * maxScale),
                                        juce::roundToInt ((float) baseHeight * maxScale));
    }

    setResizable (true, true);

    // 气泡是圆角的：TooltipWindow 默认不透明，圆角外的四角会露出一块底色。设成透明，
    // 让父组件（画布上的面板、背景）画到四角下面，气泡落在哪里都干净。
    tooltipWindow.setOpaque (false);

    // 上次存的比例（没有就 100%）→ setSize → resized() 里应用变换并写回属性。
    processor.getParameters().state.addListener (this);
    applyStoredScale();

    // GLAZE 数据源由处理器在 createEditor 里经 setGlaze…Source 注入编排器的真数据（v0.6 阶段 2）；注入之前三个源都是空的：
    // 状态按「未学习」、按钮不做事、不画实际值标记。

    updateTopBar();
    updateEffectiveValues();   // 实际值标记也在第一帧就画出来，不等第一次定时器回调（评审 v0.4 R2-02）
    updateGlazeStatus();       // 状态行与按钮文字同理
    startTimerHz (refreshHz);
}

CeladonAudioProcessorEditor::~CeladonAudioProcessorEditor()
{
    stopTimer();
    cancelPendingUpdate();
    processor.getParameters().state.removeListener (this);

    // 先解除 LookAndFeel，再按声明的相反顺序析构子组件；lookAndFeel 最后销毁，没人再引用它。
    setLookAndFeel (nullptr);
}

//==============================================================================
void CeladonAudioProcessorEditor::buildOverview()
{
    using namespace celadon;

    inputBar.setTooltip     (help::tooltipFor (uiid::meterIn));
    outputBar.setTooltip    (help::tooltipFor (uiid::meterOut));
    reductionBar.setTooltip (help::tooltipFor (uiid::meterGr));

    // GLAZE（v0.4 接通，ADR-006）：旋钮已在声明处绑定 glaze_amount，正常亮度、可拖；提示由 Knob 按参数 ID 取。
    // bigKnobProperty 让 LookAndFeel 按大旋钮样式（开片纹旋钮面、6 px 弧）画——Knob 按 Size::big 已设过，
    // 这里再显式设一次：大旋钮样式是编辑器的布局决定，不依赖 Knob 的实现细节。
    glazeKnob.getSlider().getProperties().set (ui::bigKnobProperty, true);

    // 大旋钮缓存成图（v0.5 R4）：它只在 GLAZE 变了、或鼠标悬停 / 拖动时才需要重画，却因为旁边的表、读数一动，
    // macOS 合成的外接矩形就把它扫进去，每帧把开片纹按圆裁剪 + 6 px 弧重画一遍。缓存后被扫到只是贴图；
    // 滑条要求重画（值变了、悬停、拖动）时缓存作废、重画，显示永远是新的。缓存图与屏幕像素对齐，见 PixelAlignedImageCache。
    // 只缓存滑条（旋钮本体：开片纹面 + 弧 + 指针，贵的都在这里）；下面的数值文字照旧直接画——缩放不是整数倍时，
    // 字形在缓存图里与直接画的亚像素取舍会差一点（实测 1.13 × Retina 数值字形最大差 68 / 255），直接画就与 v0.4 完全一样。
    // 画布先于旋钮声明（先构造、后析构），缓存对象随滑条一起删，引用画布是安全的。
    auto& glazeSlider = glazeKnob.getSlider();
    glazeSlider.setCachedComponentImage (new PixelAlignedImageCache (glazeSlider, [this]
    {
        return PixelAlignedImageCache::Backdrop { &canvas.getStaticLayer(), canvas.getStaticLayerScale() };
    }));   // 组件接管缓存对象并负责删除

    // 定时器要读 GLAZE、风格与 Listen 的参数值（实际值标记与状态行）：构造时取一次原子指针，之后不再按字符串查找。
    auto& state = processor.getParameters();
    glazeRawValue      = state.getRawParameterValue (param::glazeAmount);
    glazeStyleRawValue = state.getRawParameterValue (param::glazeStyle);
    listenRawValue     = state.getRawParameterValue (param::limitListen);
    jassert (glazeRawValue != nullptr && glazeStyleRawValue != nullptr && listenRawValue != nullptr);   // ID 与参数表脱节

    // GLAZE 左边的风格列与下方的状态行（v0.6）。两者都照大旋钮的办法缓存成与屏幕像素对齐的图（底是画布静态层）：
    // 表、读数一动，macOS 合成的外接矩形就会扫过它们；缓存后被扫到只是贴图，只有自己的内容变了（选中项、悬停、
    // 按钮文字、状态文字）才作废重画。新控件没有 v0.4 的样子要对齐，文字也一起缓存（全新实例同样经缓存画，两者逐像素相同）。
    glazeStyleColumn.onRelearnClicked = [this] { relearnClicked(); };

    // 组件 ID：离屏核对程序与以后的界面测试按 ID 找到它们（风格胶囊组与「重新聆听」按钮的 ID 在 GlazeStyleColumn 里设）。
    glazeStyleColumn.setComponentID ("glazeStyleColumn");
    glazeStatusLine.setComponentID ("glazeStatus");

    for (auto* cached : { static_cast<juce::Component*> (&glazeStyleColumn), static_cast<juce::Component*> (&glazeStatusLine) })
    {
        cached->setCachedComponentImage (new PixelAlignedImageCache (*cached, [this]
        {
            return PixelAlignedImageCache::Backdrop { &canvas.getStaticLayer(), canvas.getStaticLayerScale() };
        }));   // 组件接管缓存对象并负责删除；画布先于它们声明（先构造、后析构），引用画布是安全的
    }

    readout.onReset = [this] { resetMeters(); };

    // 顶栏「高级」胶囊（v0.1.7.0，设计规格-Clay 第 5 节）：开关样式（LookAndFeel::drawToggleButton），亮 = 抽屉开着。
    // 界面动作、不是参数：不进自动化、不存，每次打开编辑器默认收起。
    advancedButton.setButtonText (utf8 ("高级"));
    advancedButton.setClickingTogglesState (true);
    advancedButton.setToggleState (false, juce::dontSendNotification);
    advancedButton.setTooltip (help::tooltipFor (uiid::advanced));
    advancedButton.setComponentID ("advanced");
    advancedButton.onClick = [this] { setAdvancedDrawerOpen (advancedButton.getToggleState()); };

    canvas.addAndMakeVisible (inputBar);
    canvas.addAndMakeVisible (outputBar);
    canvas.addAndMakeVisible (reductionBar);
    canvas.addAndMakeVisible (glazeKnob);
    canvas.addAndMakeVisible (glazeStyleColumn);
    canvas.addAndMakeVisible (glazeStatusLine);
    canvas.addAndMakeVisible (readout);
    canvas.addAndMakeVisible (advancedButton);
}

void CeladonAudioProcessorEditor::buildModules()
{
    using namespace celadon;
    using ui::Knob;

    auto& state = processor.getParameters();

    // 受 GLAZE 影响的旋钮（7 项，见 glazedParameters）：加完立刻登记，定时器据此画实际值标记。
    // addKnob 返回面板拥有的 Knob&，登记的就是这个对象。
    auto addGlazedKnob = [this, &state] (ui::ModulePanel& panel, const char* parameterId, const juce::String& displayName)
    {
        rememberGlazedKnob (panel.addKnob (std::make_unique<Knob> (state, parameterId, displayName)), parameterId);
    };

    // ---- BALANCE（v0.3 起可用）：三个旋钮、Speed 三选一、Bypass 都绑定参数；曲线带放 27 段修正曲线 ----
    // Speed 的选项顺序 = Parameters.cpp 里 AudioParameterChoice 的选项顺序（Slow / Mid / Fast）。
    addGlazedKnob (balancePanel, param::balanceRecover, "Recover");
    addGlazedKnob (balancePanel, param::balanceTame,    "Tame");
    balancePanel.addKnob (std::make_unique<Knob> (state, param::balanceBrighten, "Brighten"));
    balancePanel.addSegmented (state, param::balanceSpeed, "Speed", { "Slow", "Mid", "Fast" });
    balancePanel.addToggle (state, param::balanceBypass, "Bypass");

    // 曲线自己会按 ui_balance_curve 出提示；这里与各表一样再设一次，编辑器拥有的 SettableTooltipClient 都在这里统一挂文案。
    balanceCurve.setTooltip (help::tooltipFor (uiid::balanceCurve));
    balancePanel.setCurveComponent (&balanceCurve);

    // ---- COLOR ----
    addGlazedKnob (colorPanel, param::colorTone, "Tone");
    addGlazedKnob (colorPanel, param::colorGlue, "Glue");
    addGlazedKnob (colorPanel, param::colorHeat, "Heat");
    colorPanel.addKnob (std::make_unique<Knob> (state, param::colorMix,    "Mix"));
    colorPanel.addKnob (std::make_unique<Knob> (state, param::colorOutput, "Output"));
    colorPanel.addToggle (state, param::colorBypass, "Bypass");

    glueBar.setTooltip (help::tooltipFor (uiid::glueGr));
    colorPanel.setTitleAccessory (&glueBar);

    // ---- LIMIT ----
    addGlazedKnob (limitPanel, param::limitGain, "Gain");
    limitPanel.addKnob (std::make_unique<Knob> (state, param::limitCeiling,   "Ceiling"));
    limitPanel.addKnob (std::make_unique<Knob> (state, param::limitRelease,   "Release"));
    limitPanel.addKnob (std::make_unique<Knob> (state, param::limitLookahead, "Lookahead"));   // 名字标签允许横向压到 0.75，5 个一行放得下
    addGlazedKnob (limitPanel, param::limitClip, "Clip");
    limitPanel.addToggle (state, param::limitReleaseAuto, "Auto");
    limitPanel.addToggle (state, param::limitListen,      "Listen");
    limitPanel.addToggle (state, param::limitBypass,      "Bypass");

    // 7 项里的每一项都必须有对应旋钮（登记时已保证每项只登记一次）：以后加了项而这里忘了登记，
    // Debug 版一打开界面就断在这里，而不是某个旋钮悄悄少画一个小圆点。
    jassert (numGlazedKnobsRegistered == numGlazedKnobs);

    canvas.addAndMakeVisible (balancePanel);
    canvas.addAndMakeVisible (colorPanel);
    canvas.addAndMakeVisible (limitPanel);

    // ---- 修坯（v0.1.7.0）：三灯指示在两块面板之间的空当里；高级抽屉最后加入，在面板与指示灯之上 ----
    // 三态与旁通的原子指针：构造时取一次，定时器里不再按字符串查找（与 GLAZE、风格、Listen 同一种做法）。
    clayModeRawValues = { state.getRawParameterValue (param::clayLowCutMode),
                          state.getRawParameterValue (param::clayMonoMode),
                          state.getRawParameterValue (param::clayWidthMode) };
    clayBypassRawValue = state.getRawParameterValue (param::clayBypass);
    jassert (clayModeRawValues[0] != nullptr && clayModeRawValues[1] != nullptr && clayModeRawValues[2] != nullptr
              && clayBypassRawValue != nullptr);   // ID 与参数表脱节

    canvas.addAndMakeVisible (clayIndicator);

    // 抽屉的 ✕ 与「高级」胶囊走同一个开合函数；淡入到位 / 开始收起时藏起 / 亮回被盖住的面板。
    // 抽屉自己默认不可见（收起），这里用 addChildComponent 加入，不改它的可见性。
    advancedDrawer.onCloseClicked    = [this] { setAdvancedDrawerOpen (false); };
    advancedDrawer.onCoverageChanged = [this] (bool covered) { setModulesCovered (covered); };
    canvas.addChildComponent (advancedDrawer);
}

void CeladonAudioProcessorEditor::rememberGlazedKnob (celadon::ui::Knob& knob, const char* parameterId)
{
    static_assert (std::size (glazedParameters) == numGlazedKnobs, "PluginEditor.h 的 numGlazedKnobs 与 7 项表脱节");

    const auto* entry = findGlazedParameter (parameterId);

    GlazedKnob item;
    item.knob           = &knob;
    item.effectiveValue = entry != nullptr ? entry->effectiveValue : nullptr;
    item.parameter      = processor.getParameters().getParameter (parameterId);

    // 7 项表、参数表（Parameters.cpp）与 ID（ParameterIds.h）任一脱节，Debug 版在这里断住；
    // Release 版跳过这一项（该旋钮不画小圆点），其余照常。
    jassert (item.effectiveValue != nullptr && item.parameter != nullptr);
    jassert (numGlazedKnobsRegistered < numGlazedKnobs);

    if (item.effectiveValue == nullptr || item.parameter == nullptr || numGlazedKnobsRegistered >= numGlazedKnobs)
        return;

   #if JUCE_DEBUG
    // 同一项不能登记两次：数组大小 = 项数，重复登记会把别的项挤掉。
    for (std::size_t i = 0; i < numGlazedKnobsRegistered; ++i)
        jassert (glazedKnobs[i].effectiveValue != item.effectiveValue);
   #endif

    glazedKnobs[numGlazedKnobsRegistered++] = item;
}

void CeladonAudioProcessorEditor::layoutCanvas()
{
    inputBar.setBounds         (layout::meterInArea);
    outputBar.setBounds        (layout::meterOutArea);
    reductionBar.setBounds     (layout::meterGrArea);
    glazeKnob.setBounds        (layout::glazeKnobArea);
    glazeStyleColumn.setBounds (layout::glazeStyleArea);
    glazeStatusLine.setBounds  (layout::glazeStatusArea);
    readout.setBounds          (layout::readoutArea);

    balancePanel.setBounds (layout::panelX[0], layout::panelY, layout::panelWidth, layout::panelHeight);
    colorPanel.setBounds   (layout::panelX[1], layout::panelY, layout::panelWidth, layout::panelHeight);
    limitPanel.setBounds   (layout::panelX[2], layout::panelY, layout::panelWidth, layout::panelHeight);

    // 修坯（v0.1.7.0）：都是画布坐标，随画布一起等比缩放（80% ～ 150%），与别的组件同一套办法。
    clayIndicator.setBounds  (layout::clayIndicatorArea);
    advancedDrawer.setBounds (layout::advancedDrawerArea);
    advancedButton.setBounds (advancedButtonBounds());
}

juce::Rectangle<int> CeladonAudioProcessorEditor::advancedButtonBounds() const
{
    // 按「最宽的版本 · 延迟文字」定位：延迟写成两位整数 + 一位小数（88.8 ms）量一次。延迟从 5.9 变到 10.5 ms 时胶囊不挪，
    // 右边的文字照旧右对齐、离胶囊近一点而已。字体与画布画这行字时相同（Canvas::paint：celadonFont 12 px）。
    const auto widestText = juce::GlyphArrangement::getStringWidthInt (celadon::ui::celadonFont (12.0f), topBarRightText (88.8));
    const auto rightEdge  = layout::topRight.getRight() - widestText - layout::advancedGap;

    // 不越过顶栏中间那段（操作提示 / 警告）的范围：版本号将来再长，胶囊宁可离文字近一点，也不压到中间的字。
    const auto x = juce::jmax (layout::topCentre.getRight(), rightEdge - layout::advancedWidth);

    return { x, (layout::topBar.getHeight() - layout::advancedHeight) / 2, layout::advancedWidth, layout::advancedHeight };
}

//==============================================================================
void CeladonAudioProcessorEditor::resetMeters()
{
    // 整体响度与短期窗口在音频线程的下一块开头清；峰值保持这里直接清。
    processor.resetMeters();

    for (auto* bar : { &inputBar, &outputBar, &reductionBar, &glueBar })
        bar->resetHold();

    outputPeakHoldDb   = celadon::LoudnessMeter::noReading;
    reductionDisplayDb = 0.0f;
}

void CeladonAudioProcessorEditor::timerCallback()
{
    // 全部读的是处理器里的原子变量（读完清零的取「自上次读取以来的最大值」），不持锁。
    const auto inputPeakDb  = processor.getAndResetInputTruePeakDb();
    const auto outputPeakDb = processor.getAndResetOutputTruePeakDb();
    const auto limitGrDb    = processor.getAndResetLimitGainReductionDb();
    const auto glueGrDb     = processor.getAndResetGlueGainReductionDb();

    inputBar.pushReading     (inputPeakDb);
    outputBar.pushReading    (outputPeakDb);
    reductionBar.pushReading (limitGrDb);
    glueBar.pushReading      (glueGrDb);

    // 响度区：OUT 真峰值取复位以来的最大值；GR 数字带与表同速的下落，免得每 30 ms 跳一个数。
    outputPeakHoldDb   = juce::jmax (outputPeakHoldDb, outputPeakDb);
    reductionDisplayDb = juce::jmax (limitGrDb, reductionDisplayDb - reductionFallDbPerTick);

    readout.setValues (processor.getOutputIntegratedLufs(), processor.getOutputShortTermLufs(),
                       outputPeakHoldDb, reductionDisplayDb);

    // Balance 的 27 段修正量：27 个原子变量读成一份快照喂给曲线（变化小于 0.01 dB 时曲线自己不重画）。
    celadon::BalanceModule::BandGains balanceGainsDb {};
    processor.getBalanceBandGainsDb (balanceGainsDb);
    balanceCurve.setBandGains (balanceGainsDb);

    // GLAZE 叠加后的实际值：7 个受影响旋钮的淡弧 + 小圆点（旋钮只在比例或显示状态变了时重画）。
    updateEffectiveValues();

    // GLAZE 状态行与「重新聆听」按钮（内容没变时都不重画）。
    updateGlazeStatus();

    updateTopBar();
}

void CeladonAudioProcessorEditor::updateEffectiveValues()
{
    // GLAZE 从 APVTS 的原子指针读（与音频线程 readParams 同一来源，不查字符串、不持锁）。
    // GLAZE ≤ 0（或非法值）不画（规格第 1 节：GLAZE 为 0 时不显示标记），也不去问实际值源；> 0 时 7 个都画，
    // 哪怕实际值等于用户值或被钳在上限——圆点落在指针处，正好说明「没有自动增量 / 没余量了」。
    const auto glaze = glazeRawValue != nullptr ? glazeRawValue->load (std::memory_order_relaxed) : 0.0f;
    const bool show  = glaze > 0.0f && glazeEffectiveParamsSource != nullptr;   // 「> 0」：NaN 与负数都当作 0

    const auto effective = show ? glazeEffectiveParamsSource() : celadon::ChainParams {};

    for (std::size_t i = 0; i < numGlazedKnobsRegistered; ++i)
    {
        const auto& item = glazedKnobs[i];

        if (! show)
        {
            item.knob->setEffectiveProportion (0.0, false);   // 本来就没显示时什么都不做（Knob 里判断）
            continue;
        }

        // 本块交给模块的实际值，换成该参数自己 0 ～ 1 刻度上的位置（与旋钮的数值弧同一映射）。用范围本身换算而不是参数的
        // convertTo0to1：后者先吸附到步进，而音频线程用的实际值不吸附——小圆点要标的是真正在用的值。先钳到范围，保证落在 [0, 1]。
        const auto value = item.effectiveValue (effective);
        const auto& range = item.parameter->getNormalisableRange();
        const auto proportion = (double) range.convertTo0to1 (juce::jlimit (range.start, range.end, value));

        // 实际值非有限（参数被宿主写了 NaN、或数据源出错）时不画，也不把 NaN 塞进旋钮的属性。
        const bool finite = std::isfinite (proportion);
        item.knob->setEffectiveProportion (finite ? proportion : 0.0, finite);
    }
}

void CeladonAudioProcessorEditor::updateGlazeStatus()
{
    namespace glazestatus = celadon::ui::glazestatus;

    // 没有状态源（传了空函数）按「未学习」显示——默认构造的 Status 就是未学习。
    const auto status = glazeStatusSource != nullptr ? glazeStatusSource() : celadon::glaze::Status {};

    const auto glaze    = glazeRawValue != nullptr ? glazeRawValue->load (std::memory_order_relaxed) : 0.0f;
    const auto style    = glazestatus::styleFromParameterValue (glazeStyleRawValue != nullptr
                                                                   ? glazeStyleRawValue->load (std::memory_order_relaxed)
                                                                   : 0.0f);
    const auto listenOn = listenRawValue != nullptr && listenRawValue->load (std::memory_order_relaxed) >= 0.5f;

    // 两者内部都先比较：显示内容没变（绝大多数的 30 ms）就直接返回，不拼字符串、不重画。
    glazeStatusLine.setView (glazestatus::classify (status, glaze, style, listenOn));
    glazeStyleColumn.setRelearnInProgress (status.relearnInProgress);

    // 修坯（v0.1.7.0）：同一份快照（不另取一次）。
    updateClay (status);
}

void CeladonAudioProcessorEditor::updateClay (const celadon::glaze::Status& status)
{
    // 三态：选项下标的 float（AudioParameterChoice 的原始值，= celadon::ClayMode）。非有限值、或指针缺失（参数表脱节，
    // 构造时已断言）按参数默认值「自动」。
    const auto isAutomatic = [] (const std::atomic<float>* raw) noexcept
    {
        if (raw == nullptr)
            return true;

        const auto value = raw->load (std::memory_order_relaxed);
        return ! std::isfinite (value) || juce::roundToInt (value) == static_cast<int> (celadon::ClayMode::automatic);
    };

    const auto modeAt = [this] (celadon::help::ClayStage stage) noexcept
    {
        return clayModeRawValues[static_cast<std::size_t> (stage)];
    };

    celadon::help::ClayUserState user;
    user.lowCutAuto = isAutomatic (modeAt (celadon::help::ClayStage::lowCut));
    user.monoAuto   = isAutomatic (modeAt (celadon::help::ClayStage::mono));
    user.widthAuto  = isAutomatic (modeAt (celadon::help::ClayStage::width));
    user.bypassed   = clayBypassRawValue != nullptr && clayBypassRawValue->load (std::memory_order_relaxed) >= 0.5f;

    // 量化到显示精度的读数：灯只在颜色类别变了时重画，抽屉的标记变化超过阈值才重画，气泡文字悬停时才拼。
    const auto readout = celadon::help::clayReadoutFor (status, user);

    clayIndicator.setReadout (readout);
    advancedDrawer.setClayStatus (status, readout);
}

void CeladonAudioProcessorEditor::setAdvancedDrawerOpen (bool shouldBeOpen)
{
    // 胶囊的亮灭跟着抽屉：点 ✕ 收起时把胶囊弹起（不发通知，免得再回调一次）。
    advancedButton.setToggleState (shouldBeOpen, juce::dontSendNotification);
    advancedDrawer.setOpen (shouldBeOpen);
}

void CeladonAudioProcessorEditor::setModulesCovered (bool covered)
{
    // 抽屉完全盖住下半区时，把被盖住的三块面板与修坯指示灯藏起来：画了也看不见，而且抽屉里的旋钮一重画，macOS 合成的
    // 外接矩形就会把下面三块面板（旋钮、曲线、胶囊）再白画一遍。藏起来期间定时器照常喂它们（Knob、曲线、表在不可见时
    // repaint 什么都不做），亮回来时直接就是最新的样子。
    for (auto* coveredComponent : { static_cast<juce::Component*> (&balancePanel), static_cast<juce::Component*> (&colorPanel),
                                    static_cast<juce::Component*> (&limitPanel), static_cast<juce::Component*> (&clayIndicator) })
        coveredComponent->setVisible (! covered);
}

void CeladonAudioProcessorEditor::relearnClicked()
{
    if (glazeRelearnAction != nullptr)
        glazeRelearnAction();

    // 处理器的状态快照若立刻反映了这次请求，按钮与状态行马上跟着变，不必等下一次定时器（最多 30 ms）。
    updateGlazeStatus();
}

//==============================================================================
void CeladonAudioProcessorEditor::setGlazeStatusSource (GlazeStatusSource newSource)
{
    glazeStatusSource = std::move (newSource);
    updateGlazeStatus();
}

void CeladonAudioProcessorEditor::setGlazeRelearnAction (GlazeRelearnAction newAction)
{
    glazeRelearnAction = std::move (newAction);
}

void CeladonAudioProcessorEditor::setGlazeEffectiveParamsSource (GlazeEffectiveParamsSource newSource)
{
    glazeEffectiveParamsSource = std::move (newSource);
    updateEffectiveValues();
}

void CeladonAudioProcessorEditor::updateTopBar()
{
    const auto blocked   = processor.getNonFiniteBlockCount();
    const auto latencyMs = processor.getLatencyMilliseconds();

    // 顶栏文字只由这两个数决定：都没变就不必每 30 ms 重拼一遍字符串。
    if (blocked == topBarBlockedCount && juce::exactlyEqual (latencyMs, topBarLatencyMs))
        return;

    topBarBlockedCount = blocked;
    topBarLatencyMs    = latencyMs;

    juce::String centre;
    auto centreIsWarning = true;

    if (blocked > 0)
    {
        centre = celadon::utf8 ("已屏蔽 ") + juce::String (blocked) + celadon::utf8 (" 个异常音频块（NaN/Inf）");
    }
    else
    {
       #if JUCE_DEBUG
        // Debug 构建的音频线程开销是 Release 的数倍（ADR-007）：万一装错进 DAW，一打开界面就看得见。
        // 与 NaN 警告同一个位置、同一种红；真出现 NaN 时让位给 NaN 警告——那是此刻正在发生的问题。
        centre = celadon::utf8 ("Debug 构建：CPU 远高于 Release，只供调试");
       #else
        centre = celadon::utf8 ("双击回默认 · ⌘ 拖动精调 · 滚轮微调");
        centreIsWarning = false;
       #endif
    }

    canvas.setTopBarTexts (centre, centreIsWarning, topBarRightText (latencyMs));
}

//==============================================================================
void CeladonAudioProcessorEditor::paint (juce::Graphics& g)
{
    // 画布（带缩放变换的子组件）会把它盖住的范围整块画满；JUCE 的不透明遮挡计算不看带变换的子组件，
    // 所以这里自己把画布完全盖住的整像素矩形挖掉，只填画布之外：宽高取整留下的不到 1 px 的缝、宿主给了
    // 不合宽高比的尺寸时多出来的边（v0.5 R2：v0.4 在画布底下重复填一整遍底色，约占一帧的两成）。
    // 挖掉的只是「整像素、完全在画布里」的部分，边上被画布盖住一半的像素照旧先填底色，结果与 v0.4 逐像素相同。
    const auto covered = juce::Rectangle<float> ((float) baseWidth, (float) baseHeight).transformedBy (canvas.getTransform());
    const auto inner = juce::Rectangle<int>::leftTopRightBottom ((int) std::ceil (covered.getX()),
                                                                 (int) std::ceil (covered.getY()),
                                                                 (int) std::floor (covered.getRight()),
                                                                 (int) std::floor (covered.getBottom()));
    if (! inner.isEmpty())
        g.excludeClipRegion (inner);

    g.fillAll (palette::background);
}

void CeladonAudioProcessorEditor::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    // 画布保持 900 × 560 不动，整块按窗口等比缩放。取宽、高两个方向里较小的比例：
    // 宿主给了不合宽高比的尺寸时整幅仍完全可见，多出来的边由底色填。
    const auto scale = juce::jmin ((float) getWidth()  / (float) baseWidth,
                                   (float) getHeight() / (float) baseHeight);
    canvas.setTransform (juce::AffineTransform::scale (scale));

    // 存的比例以宽度为准：round (900 × 比例) 恢复出来的宽度与现在完全一致，反复开关工程不会漂移一像素。
    storeScale ((double) getWidth() / (double) baseWidth);
}

//==============================================================================
float CeladonAudioProcessorEditor::readStoredScale() const
{
    const auto stored = (double) processor.getParameters().state.getProperty (uiScaleId, 1.0);

    if (! std::isfinite (stored))
        return 1.0f;

    return juce::jlimit (minScale, maxScale, (float) stored);
}

void CeladonAudioProcessorEditor::applyStoredScale()
{
    const auto scale  = readStoredScale();
    const auto width  = juce::roundToInt ((float) baseWidth  * scale);
    const auto height = juce::roundToInt ((float) baseHeight * scale);

    if (width != getWidth() || height != getHeight())
        setSize (width, height);
}

void CeladonAudioProcessorEditor::storeScale (double scale)
{
    // 只在消息线程：resized() 只会在消息线程被调。属性值没变时 ValueTree 不会发通知。
    const juce::ScopedValueSetter<bool> guard (writingScale, true);
    processor.getParameters().state.setProperty (uiScaleId, scale, nullptr);
}

void CeladonAudioProcessorEditor::valueTreeRedirected (juce::ValueTree&)
{
    triggerAsyncUpdate();
}

void CeladonAudioProcessorEditor::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // 根节点上的 uiScale 被别人改了（不是自己 resized() 写的）才需要跟：参数子节点的 value 变化在这里直接跳过。
    if (! writingScale && property == uiScaleId && tree == processor.getParameters().state)
        triggerAsyncUpdate();
}

void CeladonAudioProcessorEditor::handleAsyncUpdate()
{
    applyStoredScale();
}
