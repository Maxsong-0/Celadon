/*
  ==============================================================================

    GlazeStatusLine.cpp

  ==============================================================================
*/

#include "GlazeStatusLine.h"

#include "CeladonLookAndFeel.h"
#include "HelpText.h"
#include "../ParameterIds.h"

namespace celadon::ui
{
    namespace
    {
        constexpr float fontHeight = 12.0f;   // 规格：状态行 12 px

        juce::Colour colourFor (glazestatus::Tone tone) noexcept
        {
            switch (tone)
            {
                case glazestatus::Tone::accent: return palette::accent;      // 梅子青：正在进行（聆听、校准）
                case glazestatus::Tone::amber:  return palette::reduction;   // 琥珀（与 GR 表同色）：方案受约束 / 手动设置限制
                case glazestatus::Tone::dim:    break;
            }

            return palette::textDim;                                         // 灰青：稳态说明
        }
    }

    //==============================================================================
    GlazeStatusLine::GlazeStatusLine()
    {
        setTooltip (help::tooltipFor (uiid::glazeStatus));
        setInterceptsMouseClicks (true, false);   // 要接住鼠标才会出提示（范围由 hitTest 收窄到文字）
    }

    GlazeStatusLine::~GlazeStatusLine() = default;

    //==============================================================================
    void GlazeStatusLine::setView (const glazestatus::View& newView)
    {
        // 30 ms 一次：绝大多数时候状态没变，这里直接返回，不拼字符串、不重画。
        if (hasView && newView == view)
            return;

        view = newView;
        hasView = true;

        auto newText = glazestatus::textFor (view);
        const auto newTone = glazestatus::toneFor (view.kind);

        // View 变了但画出来一样（例如已锁定、目标暂不可显示时 GLAZE 在 0 与 > 0 之间切换）：不重画。
        if (newText == text && newTone == tone)
            return;

        text = std::move (newText);
        tone = newTone;
        textWidth = juce::GlyphArrangement::getStringWidthInt (celadonFont (fontHeight), text);
        repaint();
    }

    //==============================================================================
    void GlazeStatusLine::paint (juce::Graphics& g)
    {
        g.setFont (celadonFont (fontHeight));
        g.setColour (colourFor (tone));

        // 文字都在布局给的宽度之内（离屏核对量过最长的一条）；万一将来改长了，宁可省略号也不越出自己的范围压到别的控件。
        g.drawText (text, getLocalBounds(), juce::Justification::centred, true);
    }

    bool GlazeStatusLine::hitTest (int x, int y)
    {
        // 文字居中：左右各留 (宽 − 文字宽) / 2。再各放宽 4 px，鼠标停在字边上也算。
        juce::ignoreUnused (y);
        const auto margin = juce::jmax (0, (getWidth() - textWidth) / 2 - 4);
        return text.isNotEmpty() && x >= margin && x < getWidth() - margin;
    }
}
