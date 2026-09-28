/*
  ==============================================================================

    ClayIndicator.cpp
    修坯三灯指示：三盏灯的画法与动态气泡。灯的颜色类别与气泡文字的判定在 HelpText（clayLampFor / clayIndicatorTooltip），
    这里只管画与「什么时候重画 / 重拼」。读数（ClayReadout）含 ADR-012 的三个代价标志：它们变了气泡照样按「读数变了」重拼，
    灯的颜色类别不看它们（不重画）。

  ==============================================================================
*/

#include "ClayIndicator.h"

#include "CeladonLookAndFeel.h"

namespace celadon::ui
{
    namespace
    {
        juce::Colour colourFor (help::ClayLamp lamp) noexcept
        {
            switch (lamp)
            {
                case help::ClayLamp::on:    return palette::accent;      // 梅子青：该级在处理
                case help::ClayLamp::guard: return palette::reduction;   // 琥珀（与 GR 表同色）：该级的实时保护在介入
                case help::ClayLamp::off:   break;
            }

            // 暗：灰青按「即将推出」同一个 35% 画——看得出灯在那里，又一眼分得出没亮。
            return palette::textDim.withAlpha (palette::comingSoonAlpha);
        }
    }

    //==============================================================================
    juce::String ClayLiveTooltip::get()
    {
        const auto now = juce::Time::getMillisecondCounter();

        // 第一次问、或读数变了且距上次换字已满一秒：按最新读数重拼。无符号减法：计数器约 49 天回绕一次，间隔照样算对。
        if (! hasText || (latest != shown && now - lastChangeMs >= minChangeIntervalMs))
        {
            shown = latest;
            text = help::clayIndicatorTooltip (shown);
            lastChangeMs = now;
            hasText = true;
        }

        return text;
    }

    //==============================================================================
    ClayIndicator::ClayIndicator()
    {
        // 透明：箭头（画布静态层）从下面透出来。整块接鼠标：停在灯上或箭头上都出修坯的气泡。
        setOpaque (false);
        setInterceptsMouseClicks (true, false);
        setComponentID ("clayIndicator");   // 离屏核对程序与以后的界面测试按 ID 找它
    }

    ClayIndicator::~ClayIndicator() = default;

    //==============================================================================
    void ClayIndicator::setReadout (const help::ClayReadout& readout)
    {
        liveTooltip.setReadout (readout);

        auto newLamps = lamps;

        for (int i = 0; i < help::numClayStages; ++i)
            newLamps[static_cast<std::size_t> (i)] = help::clayLampFor (readout, static_cast<help::ClayStage> (i));

        // 30 ms 一次：绝大多数时候颜色没变，直接返回、不重画。变了也只标本组件这一小块（灯的颜色不影响别处）。
        if (newLamps == lamps)
            return;

        lamps = newLamps;
        repaint();
    }

    help::ClayLamp ClayIndicator::getLamp (help::ClayStage stage) const noexcept
    {
        const auto index = static_cast<int> (stage);

        return juce::isPositiveAndBelow (index, help::numClayStages) ? lamps[static_cast<std::size_t> (index)]
                                                                      : help::ClayLamp::off;
    }

    juce::String ClayIndicator::getTooltip()
    {
        return liveTooltip.get();
    }

    //==============================================================================
    juce::Rectangle<float> ClayIndicator::lampBounds (int index) const noexcept
    {
        const juce::Point<float> centre (static_cast<float> (getWidth()) * 0.5f,
                                         firstLampCentreY + static_cast<float> (index) * lampPitch);

        return juce::Rectangle<float> (lampDiameter, lampDiameter).withCentre (centre);
    }

    void ClayIndicator::paint (juce::Graphics& g)
    {
        for (int i = 0; i < help::numClayStages; ++i)
        {
            g.setColour (colourFor (lamps[static_cast<std::size_t> (i)]));
            g.fillEllipse (lampBounds (i));
        }
    }
}
