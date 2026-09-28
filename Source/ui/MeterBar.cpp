/*
  ==============================================================================

    MeterBar.cpp
    电平表。读数每 30 ms 由编辑器推进来；显示值按 20 dB/s 下落，峰值保持记复位以来的最大值。

  ==============================================================================
*/

#include "MeterBar.h"

#include "CeladonLookAndFeel.h"

namespace celadon::ui
{
    namespace
    {
        constexpr float fallDbPerSecond   = 20.0f;
        constexpr float refreshSeconds    = 0.03f;                            // 编辑器定时器周期
        constexpr float fallPerReading    = fallDbPerSecond * refreshSeconds;  // 每次 pushReading 最多落 0.6 dB
        constexpr float barWidth          = 10.0f;
        constexpr float barCornerRadius   = 3.0f;
        constexpr int   captionHeight     = 14;      // 竖表上方标题 / 下方数字各占的高度（11 px 字）
        constexpr float captionFontHeight = 11.0f;
        constexpr int   miniTextWidth     = 44;      // 横向小表右侧「x.x dB」的宽度
        constexpr int   miniBarHeight     = 14;
        constexpr int   barInset          = 2;       // 竖表：表条与上下文字之间各留的空
        constexpr int   miniGap           = 4;       // 横向小表：表条与右侧数字之间的空

        // 表面边缘 / 峰值刻线移动不到半个物理像素就不重画：抗锯齿边缘的变化肉眼看不出来。
        constexpr float minimumVisibleShiftPixels = 0.5f;
    }

    //==============================================================================
    MeterBar::MeterBar (const juce::String& titleText, float minimumDb, float maximumDb, bool growsFromTop,
                        Style barStyle)
        : title (titleText),
          minDb (minimumDb),
          maxDb (maximumDb),
          fromTop (growsFromTop),
          style (barStyle),
          displayDb (minimumDb),
          holdDb (minimumDb),
          shownDb (minimumDb),
          shownHoldDb (minimumDb)
    {
        jassert (maxDb > minDb);

        // 点数字（或表的任何地方）清零峰值保持，用手形光标提示可点。
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setInterceptsMouseClicks (true, false);

        shownText = shownTextFor();
    }

    MeterBar::~MeterBar() = default;

    //==============================================================================
    void MeterBar::pushReading (float db)
    {
        // 「没有读数」= minDb 或更低（LoudnessMeter::noReading）：只让显示值继续下落，不进峰值保持。
        // GR 表例外：它的 minDb 是 0 dB，0.0 是「此刻没在压」这个真实读数，峰值保持从 0.0 起记，
        // 否则 GR 表下面会一直写「-inf」直到第一次压缩。
        const auto hasReading = fromTop ? db >= minDb : db > minDb;
        const auto reading = hasReading ? juce::jmin (db, maxDb) : minDb;

        displayDb = juce::jmax (reading, displayDb - fallPerReading, minDb);

        if (hasReading && (! hasHold || reading > holdDb))
        {
            holdDb = reading;
            hasHold = true;
        }

        updateShownState (false);
    }

    void MeterBar::resetHold()
    {
        holdDb = minDb;
        hasHold = false;
        updateShownState (true);
    }

    void MeterBar::updateShownState (bool force)
    {
        const auto text = shownTextFor();

        // 横向小表不画峰值保持，峰值变了与画面无关。
        const auto holdVisible = style == Style::vertical;

        const auto changed = force
                          || text != shownText
                          || movesVisibly (shownDb, displayDb)
                          || (holdVisible && hasHold != shownHasHold)
                          || (holdVisible && hasHold && movesVisibly (shownHoldDb, holdDb));

        if (! changed)
            return;

        shownDb      = displayDb;
        shownHoldDb  = holdDb;
        shownHasHold = hasHold;
        shownText    = text;
        repaint();
    }

    void MeterBar::mouseDown (const juce::MouseEvent&)
    {
        resetHold();
    }

    //==============================================================================
    float MeterBar::proportionOf (float db) const noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    }

    float MeterBar::barLength() const noexcept
    {
        // 与 paintVertical / paintHorizontalMini 里表条的实际尺寸同一算法。
        if (style == Style::vertical)
            return (float) juce::jmax (0, getHeight() - 2 * captionHeight - 2 * barInset);

        return (float) juce::jmax (0, getWidth() - miniTextWidth - miniGap);
    }

    bool MeterBar::movesVisibly (float fromDb, float toDb) const noexcept
    {
        const auto from = proportionOf (fromDb);
        const auto to   = proportionOf (toDb);

        // 到达或离开两端（表空 / 表满）一定算看得出：下落的最后一步常不到半个像素，只按阈值比会让画面永远停在
        // 离底一丝的地方，静止时留下一条细线（v0.5 验收 V2-1：OUT 表底部、GR 表顶部）。proportionOf 已钳到 [0, 1]。
        const auto endOf = [] (float proportion) noexcept { return proportion <= 0.0f ? -1 : (proportion >= 1.0f ? 1 : 0); };

        if (endOf (from) != endOf (to))
            return true;

        return std::abs (to - from) * barLength() * physicalPixelsPerLogical >= minimumVisibleShiftPixels;
    }

    juce::String MeterBar::shownTextFor() const
    {
        return style == Style::vertical ? holdText() : valueText (displayDb) + " dB";
    }

    juce::String MeterBar::valueText (float db) const
    {
        return juce::String (db, 1);
    }

    juce::String MeterBar::holdText() const
    {
        return hasHold ? valueText (holdDb) : juce::String ("-inf");
    }

    void MeterBar::paint (juce::Graphics& g)
    {
        // 记下这次的物理像素比，下一次判断「看不看得出变化」用它。
        physicalPixelsPerLogical = juce::jmax (0.25f, g.getInternalContext().getPhysicalPixelScaleFactor());

        if (style == Style::vertical)
            paintVertical (g);
        else
            paintHorizontalMini (g);
    }

    void MeterBar::paintVertical (juce::Graphics& g)
    {
        auto area = getLocalBounds();

        g.setFont (celadonFont (captionFontHeight));

        // 上：标题（textDim）。
        g.setColour (palette::textDim);
        g.drawText (title, area.removeFromTop (captionHeight), juce::Justification::centred, false);

        // 下：峰值保持数字（text 色，点它清零）。
        g.setColour (palette::text);
        g.drawText (shownText, area.removeFromBottom (captionHeight), juce::Justification::centred, false);

        // 中：10 px 宽竖条，track 底。
        area.removeFromTop (barInset);
        area.removeFromBottom (barInset);
        const auto bar = area.toFloat().withSizeKeepingCentre (barWidth, (float) area.getHeight());

        g.setColour (palette::track);
        g.fillRoundedRectangle (bar, barCornerRadius);

        const auto fillColour = fromTop ? palette::reduction : palette::accent;
        const auto fillHeight = bar.getHeight() * proportionOf (shownDb);

        if (fillHeight > 0.0f)
        {
            // GR 从上往下长（压得越深越长），峰值表从下往上长。
            const auto fill = fromTop ? bar.withHeight (fillHeight)
                                      : bar.withTop (bar.getBottom() - fillHeight);

            g.setColour (fillColour);
            g.fillRoundedRectangle (fill, barCornerRadius);
        }

        // 峰值保持刻线：一条 1 px 横线停在最大值处。
        if (shownHasHold)
        {
            const auto holdOffset = bar.getHeight() * proportionOf (shownHoldDb);
            const auto y = fromTop ? bar.getY() + holdOffset : bar.getBottom() - holdOffset;

            g.setColour (fillColour.withAlpha (0.9f));
            g.fillRect (bar.getX(), juce::jlimit (bar.getY(), bar.getBottom() - 1.0f, y - 0.5f), bar.getWidth(), 1.0f);
        }
    }

    void MeterBar::paintHorizontalMini (juce::Graphics& g)
    {
        auto area = getLocalBounds();

        // 右：当前压缩量「x.x dB」（这是标题行里的实时小表，看的是此刻的值，不是峰值保持）。
        g.setFont (celadonFont (captionFontHeight));
        g.setColour (palette::text);
        g.drawText (shownText, area.removeFromRight (miniTextWidth), juce::Justification::centredRight, false);

        area.removeFromRight (miniGap);

        // 左：横条，reduction 色从左往右表示压缩量。
        const auto bar = area.toFloat().withSizeKeepingCentre ((float) area.getWidth(),
                                                                (float) juce::jmin (miniBarHeight, area.getHeight()));

        g.setColour (palette::track);
        g.fillRoundedRectangle (bar, barCornerRadius);

        const auto fillWidth = bar.getWidth() * proportionOf (shownDb);

        if (fillWidth > 0.0f)
        {
            g.setColour (fromTop ? palette::reduction : palette::accent);
            g.fillRoundedRectangle (bar.withWidth (fillWidth), barCornerRadius);
        }
    }
}
