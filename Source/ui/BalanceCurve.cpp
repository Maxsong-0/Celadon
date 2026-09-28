/*
  ==============================================================================

    BalanceCurve.cpp
    Balance 面板的 27 段修正曲线。读数由编辑器定时器每 30 ms 推进来（setBandGains），
    这里只管画：中线虚线、穿过 27 个点的平滑曲线、提升（梅子青）与压下（琥珀）各填到中线。
    背景透明，面板底色由 ModulePanel 画。

  ==============================================================================
*/

#include "BalanceCurve.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"
#include "HelpText.h"

#include <cmath>
#include <iterator>

namespace celadon::ui
{
    namespace
    {
        constexpr int   numBands         = celadon::BalanceModule::numBands;
        constexpr float rangeDb          = 8.0f;     // 纵轴 ±8 dB，包含增强后的 Tame 压制范围
        constexpr float sideMargin       = 4.0f;     // 两端各留 4 px，最低 / 最高段的点不贴边
        constexpr float curveThickness   = 1.5f;
        constexpr float fillAlpha        = 0.35f;
        constexpr float visibleGainDb    = 0.01f;    // |G| 小于这个视作 0（全为 0 时只画中线）

        // 曲线上的点移动不到半个物理像素就不重画：抗锯齿边缘的变化肉眼看不出来。
        constexpr float minimumVisibleShiftPixels = 0.5f;
        constexpr float midlineDashes[]  = { 4.0f, 4.0f };   // 与 ModulePanel 的占位虚线同样式

        // 曲线线宽的一半：绘图区上下各留这么多，±8 dB 顶格时描边仍完整落在组件里。
        constexpr float verticalMargin   = curveThickness * 0.5f;

        // 横轴对数刻度的两端（40 Hz、16 kHz）。每次现算而不是放静态变量：getBandCentreHz 在另一个
        // 编译单元里，静态初始化顺序没有保证；三次 log10 一帧 27 次，开销可以忽略。
        float logMinHz() noexcept   { return std::log10 (celadon::BalanceModule::getBandCentreHz (0)); }
        float logMaxHz() noexcept   { return std::log10 (celadon::BalanceModule::getBandCentreHz (numBands - 1)); }
    }

    //==============================================================================
    BalanceCurve::BalanceCurve()
    {
        setTooltip (celadon::help::tooltipFor (celadon::uiid::balanceCurve));

        // 只为悬停提示接收鼠标事件，没有可点的东西；子组件也没有。
        setInterceptsMouseClicks (true, false);
        setOpaque (false);
    }

    BalanceCurve::~BalanceCurve() = default;

    //==============================================================================
    void BalanceCurve::setBandGains (const celadon::BalanceModule::BandGains& gainsDb)
    {
        // 非有限值（NaN / inf）按 0 处理：NaN 的比较全为假，不清掉会让曲线卡在上一帧或画出坏路径。
        celadon::BalanceModule::BandGains cleaned {};

        for (size_t band = 0; band < (size_t) numBands; ++band)
        {
            const auto raw = gainsDb[band];
            cleaned[band] = std::isfinite (raw) ? raw : 0.0f;
        }

        // 画面上看得出的变化才重画：「有没有可见修正」翻转（曲线出现 / 消失），或者任一段的点移动 ≥ 半个物理像素。
        // 超出 ±8 dB 的点钉在边上，按钉住后的位置比。v0.4 按 0.01 dB（约 0.04 px）就重画，Balance 工作时几乎每帧都画，
        // 还把 macOS 合成的外接矩形拉到窗口下半部（研究 P5 R5）。
        auto changed = anyVisibleGain (cleaned) != anyVisibleGain (gains);

        if (! changed)
        {
            const auto physicalPixelsPerDb = pixelsPerDb() * physicalPixelsPerLogical;

            for (size_t band = 0; band < (size_t) numBands && ! changed; ++band)
            {
                const auto from = juce::jlimit (-rangeDb, rangeDb, gains[band]);
                const auto to   = juce::jlimit (-rangeDb, rangeDb, cleaned[band]);
                changed = std::abs (to - from) * physicalPixelsPerDb >= minimumVisibleShiftPixels;
            }
        }

        if (! changed)
            return;

        gains = cleaned;
        repaint();
    }

    //==============================================================================
    bool BalanceCurve::anyVisibleGain (const celadon::BalanceModule::BandGains& bandGains) noexcept
    {
        for (size_t band = 0; band < (size_t) numBands; ++band)
            if (std::abs (bandGains[band]) >= visibleGainDb)
                return true;

        return false;
    }

    float BalanceCurve::pixelsPerDb() const noexcept
    {
        // 与 paint 里的绘图区同一算法：中线压在整像素行上，上下各留半个线宽，±8 dB 顶到绘图区上下边。
        const auto bounds = getLocalBounds().toFloat();
        const auto midY = std::floor (bounds.getCentreY()) + 0.5f;
        const auto halfHeight = juce::jmax (1.0f, juce::jmin (midY - bounds.getY(), bounds.getBottom() - midY) - verticalMargin);
        return halfHeight / rangeDb;
    }

    juce::Point<float> BalanceCurve::pointForBand (int band, juce::Rectangle<float> plot) const noexcept
    {
        const auto logHz = std::log10 (celadon::BalanceModule::getBandCentreHz (band));
        const auto xProportion = (logHz - logMinHz()) / (logMaxHz() - logMinHz());
        const auto x = plot.getX() + plot.getWidth() * juce::jlimit (0.0f, 1.0f, xProportion);

        // 上正下负：+8 dB 到绘图区顶、−8 dB 到底、0 dB 在中线。
        const auto db = juce::jlimit (-rangeDb, rangeDb, gains[(size_t) band]);
        const auto y = plot.getCentreY() - (db / rangeDb) * (plot.getHeight() * 0.5f);

        return { x, y };
    }

    juce::Path BalanceCurve::buildCurvePath (juce::Rectangle<float> plot) const
    {
        std::array<juce::Point<float>, (size_t) numBands> points;

        for (int band = 0; band < numBands; ++band)
            points[(size_t) band] = pointForBand (band, plot);

        // 单调三次 Hermite（Fritsch–Carlson）：相邻两点之间曲线不会越出这两点的数值范围。
        // 不用 Catmull-Rom：它在单独一段 +6 dB 时会让两侧 G = 0 的段被画出向下的凹陷（实测 1.5 px），
        // 琥珀填充会出现在根本没压下的频段上。
        std::array<float, (size_t) numBands> slopes {};
        std::array<float, (size_t) numBands - 1> secants {};

        for (size_t i = 0; i + 1 < points.size(); ++i)
        {
            const auto dx = points[i + 1].x - points[i].x;
            secants[i] = dx > 0.0f ? (points[i + 1].y - points[i].y) / dx : 0.0f;
        }

        slopes.front() = secants.front();
        slopes.back()  = secants.back();

        for (size_t i = 1; i + 1 < points.size(); ++i)
        {
            const auto left = secants[i - 1], right = secants[i];

            // 两侧割线异号或有一侧为平 → 这是局部极值 / 平台，切线取 0，不会有过冲；
            // 同号 → 取两者的平均。
            slopes[i] = (left * right <= 0.0f) ? 0.0f : (left + right) * 0.5f;
        }

        // Fritsch–Carlson 限幅：α² + β² ≤ 9 才保证单调，超出就把两端切线等比缩回圆内。
        for (size_t i = 0; i + 1 < points.size(); ++i)
        {
            const auto secant = secants[i];

            if (secant == 0.0f)
            {
                slopes[i] = slopes[i + 1] = 0.0f;
                continue;
            }

            const auto alpha = slopes[i] / secant;
            const auto beta  = slopes[i + 1] / secant;
            const auto radius2 = alpha * alpha + beta * beta;

            if (radius2 > 9.0f)
            {
                const auto tau = 3.0f / std::sqrt (radius2);
                slopes[i]     = tau * alpha * secant;
                slopes[i + 1] = tau * beta  * secant;
            }
        }

        // Hermite → 三次贝塞尔：控制点 = 端点 ± (Δx / 3) · (1, 切线)。限幅后 α、β ≤ 3，
        // 控制点纵坐标必落在两端点之间，曲线不会出绘图区；clampY 只是再保一道。
        const auto clampY = [plot] (juce::Point<float> p) noexcept
        {
            return juce::Point<float> (p.x, juce::jlimit (plot.getY(), plot.getBottom(), p.y));
        };

        juce::Path path;
        path.preallocateSpace (3 + 7 * (numBands - 1));
        path.startNewSubPath (points.front());

        for (size_t i = 0; i + 1 < points.size(); ++i)
        {
            const auto p1 = points[i];
            const auto p2 = points[i + 1];
            const auto third = (p2.x - p1.x) / 3.0f;

            const auto c1 = clampY ({ p1.x + third, p1.y + third * slopes[i] });
            const auto c2 = clampY ({ p2.x - third, p2.y - third * slopes[i + 1] });

            path.cubicTo (c1, c2, p2);
        }

        return path;
    }

    //==============================================================================
    void BalanceCurve::paint (juce::Graphics& g)
    {
        // 记下这次的物理像素比，下一次判断「看不看得出变化」用它。
        physicalPixelsPerLogical = juce::jmax (0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());

        const auto bounds = getLocalBounds().toFloat();

        if (bounds.isEmpty())
            return;

        // 中线：track 色 1 px 虚线，压在整像素行上（+0.5 让 1 px 线不糊成 2 px）。
        const auto midY = std::floor (bounds.getCentreY()) + 0.5f;

        g.setColour (palette::track);
        g.drawDashedLine (juce::Line<float> (bounds.getX(), midY, bounds.getRight(), midY),
                          midlineDashes, (int) std::size (midlineDashes), 1.0f);

        if (! hasVisibleGains())
            return;

        // 绘图区：左右各留 4 px，上下各留半个线宽；纵向以中线为中心对称，0 dB 正好落在中线上。
        const auto halfHeight = juce::jmax (1.0f, juce::jmin (midY - bounds.getY(), bounds.getBottom() - midY) - verticalMargin);
        const auto plot = juce::Rectangle<float> (bounds.getX() + sideMargin, midY - halfHeight,
                                                  juce::jmax (1.0f, bounds.getWidth() - 2.0f * sideMargin),
                                                  2.0f * halfHeight);

        const auto curve = buildCurvePath (plot);

        // 填充：曲线 + 沿中线折返的闭合区域。非零环绕规则下，曲线在中线上方与下方围出的每一块都各算一次，
        // 所以同一条闭合路径分别用上半 / 下半剪裁各填一次，就得到「提升填梅子青、压下填琥珀」。
        juce::Path fill (curve);
        fill.lineTo (curve.getCurrentPosition().x, midY);
        fill.lineTo (plot.getX(), midY);
        fill.closeSubPath();

        // 上下两半用浮点矩形路径剪裁（不是整像素矩形）：中线在 y = 22.5 这种半像素位置时，
        // 整数剪裁会把中线所在那一行的半个像素丢给错误的一侧，路径剪裁则按抗锯齿精确到中线。
        juce::Path upperHalf, lowerHalf;
        upperHalf.addRectangle (bounds.withBottom (midY));
        lowerHalf.addRectangle (bounds.withTop (midY));

        {
            juce::Graphics::ScopedSaveState state (g);

            if (g.reduceClipRegion (upperHalf))
            {
                g.setColour (palette::accent.withAlpha (fillAlpha));
                g.fillPath (fill);
            }
        }

        {
            juce::Graphics::ScopedSaveState state (g);

            if (g.reduceClipRegion (lowerHalf))
            {
                g.setColour (palette::reduction.withAlpha (fillAlpha));
                g.fillPath (fill);
            }
        }

        // 曲线本体：灰青 1.5 px，圆角接头与端头。
        g.setColour (palette::textDim);
        g.strokePath (curve, juce::PathStrokeType (curveThickness, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }
}
