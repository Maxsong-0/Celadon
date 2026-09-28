/*
  ==============================================================================

    ModuleCards.cpp

  ==============================================================================
*/

#include "ModuleCards.h"

#include "../ParameterIds.h"
#include "CeladonLookAndFeel.h"

#include <cmath>
#include <tuple>
#include <utility>

namespace celadon::ui
{
    namespace
    {
        constexpr int padX = 14, headerTop = 12, headerHeight = 22;
        constexpr float bypassedAlpha = 0.35f;

        juce::RangedAudioParameter& parameterFor (juce::AudioProcessorValueTreeState& state, const char* id)
        {
            auto* parameter = state.getParameter (id);
            jassert (parameter != nullptr);   // ParameterIds.h 与 Parameters.cpp 不一致
            return *parameter;
        }

        const juce::String& dash()
        {
            static const juce::String text = juce::String::charToString ((juce::juce_wchar) 0x2014);
            return text;
        }
    }

    //==============================================================================
    ModuleCard::ModuleCard (juce::AudioProcessorValueTreeState& s, const char* bypassParameterId, InteractionHost& h,
                            juce::String numberText, juce::String titleText)
        : state (s), host (h), power (parameterFor (s, bypassParameterId)),
          number (std::move (numberText)), title (std::move (titleText))
    {
        // 不在这里 setSize：resized() 会调子类的 layoutContent，基类构造时子类还不存在。尺寸由编辑器摆放时给。
        power.setComponentID (bypassParameterId);
        addAndMakeVisible (power);
        power.onChange = [this] { updateBypassLook(); repaint(); };
    }

    void ModuleCard::addContent (juce::Component& component)
    {
        content.add (&component);
        addAndMakeVisible (component);
    }

    void ModuleCard::updateBypassLook()
    {
        const auto alpha = power.isModuleOn() ? 1.0f : bypassedAlpha;

        for (auto* component : content)
            component->setAlpha (alpha);
    }

    void ModuleCard::setLanguage (Language newLanguage)
    {
        language = newLanguage;
        repaint();
    }

    void ModuleCard::resized()
    {
        power.setTopLeftPosition (width - padX - 22, headerTop);
        layoutContent();
        updateBypassLook();
    }

    juce::String ModuleCard::getTooltip()
    {
        return getMouseXYRelative().y < headerTop + headerHeight ? getHeaderTooltip() : juce::String();
    }

    void ModuleCard::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (palette::panel);
        g.fillRoundedRectangle (bounds, 14.0f);
        g.setColour (palette::panelBorder);
        g.drawRoundedRectangle (bounds.reduced (0.5f), 13.5f, 1.0f);

        // 卡片头：序号 · 名字 · 说明 ……（右）备注
        const juce::Rectangle<float> header ((float) padX, (float) headerTop, (float) (width - 2 * padX), (float) headerHeight);
        auto x = header.getX();

        const auto numberFont = monoFont (11.0f);
        g.setFont (numberFont);
        g.setColour (palette::c600);
        g.drawText (number, juce::Rectangle<float> (x, header.getY(), 30.0f, header.getHeight()), juce::Justification::centredLeft, false);
        x += textWidth (numberFont, number) + 7.0f;

        const auto titleFont = withLetterSpacing (uiFont (13.0f, Weight::semibold), 2.0f);
        g.setFont (titleFont);
        g.setColour (palette::ink);
        g.drawText (title, juce::Rectangle<float> (x, header.getY(), 120.0f, header.getHeight()), juce::Justification::centredLeft, false);
        x += textWidth (titleFont, title) + 7.0f;

        g.setFont (uiFont (11.5f));
        g.setColour (palette::ink3);
        g.drawText (getDescription(), juce::Rectangle<float> (x, header.getY(), (float) power.getX() - x - 6.0f, header.getHeight()),
                    juce::Justification::centredLeft, true);

        if (const auto note = getNote(); note.isNotEmpty())
        {
            g.setFont (monoFont (10.5f));
            g.drawText (note, juce::Rectangle<float> ((float) power.getX() - 90.0f, header.getY(), 83.0f, header.getHeight()),
                        juce::Justification::centredRight, false);
        }
    }

    //==============================================================================
    ClayCard::ClayCard (juce::AudioProcessorValueTreeState& s, InteractionHost& h)
        : ModuleCard (s, param::clayBypass, h, "01", "CLAY")
    {
        const std::array<std::pair<const char*, const char*>, 3> ids { { { param::clayLowCutMode, param::clayLowCutHz },
                                                                         { param::clayMonoMode,   param::clayMonoHz },
                                                                         { param::clayWidthMode,  param::clayWidthDb } } };
        const help::ClayStage stages[] = { help::ClayStage::lowCut, help::ClayStage::mono, help::ClayStage::width };

        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto& row = rows[i];
            row.stage = stages[i];
            row.modeId = ids[i].first;
            row.valueId = ids[i].second;

            row.modes = std::make_unique<Segmented> (Segmented::Look::groove);
            row.modes->setComponentID (row.modeId);
            row.modes->setOptions ({ "", "", "" });
            row.modes->setSelectedColours (0, { palette::violetSoft, palette::violet });
            addContent (*row.modes);
            row.modesAttachment = std::make_unique<SegmentedAttachment> (*row.modes, parameterFor (state, row.modeId));
            row.modesAttachment->onChange = [this] (int) { refreshRows(); repaint(); };

            row.value = std::make_unique<DragValue> (parameterFor (state, row.valueId), host);
            row.value->setComponentID (row.valueId);
            addContent (*row.value);
        }

        power.onChange = [this] { updateBypassLook(); refreshRows(); repaint(); };
        refreshRows();
    }

    juce::String ClayCard::getDescription() const   { return tr (Text::describeClay, language); }

    juce::String ClayCard::getHeaderTooltip() const
    {
        return help::clayIndicatorTooltip (readout, language);
    }

    void ClayCard::setLanguage (Language newLanguage)
    {
        ModuleCard::setLanguage (newLanguage);
        power.setTooltip (help::tooltipFor (param::clayBypass, language));

        for (auto& row : rows)
        {
            row.modes->setOptions ({ tr (Text::modeAuto, language), tr (Text::modeOff, language), tr (Text::modeManual, language) });
            row.modes->setTooltip (help::tooltipFor (row.modeId, language));
            row.value->setTooltip (help::tooltipFor (row.valueId, language));
        }

        refreshRows();
    }

    int ClayCard::modeOf (const Row& row) const
    {
        return row.modes->getSelected();   // 0 自动 / 1 关 / 2 手动（= celadon::ClayMode）
    }

    void ClayCard::setReadout (const help::ClayReadout& newReadout)
    {
        if (newReadout == readout)
            return;

        readout = newReadout;
        refreshRows();
        repaint();
    }

    void ClayCard::refreshRows()
    {
        const auto bypassed = ! power.isModuleOn();

        for (auto& row : rows)
        {
            const auto mode = modeOf (row);
            row.value->setAdjustable (mode == 2 && ! bypassed);

            if (bypassed || mode == 1)
            {
                row.value->setDisplay (dash(), palette::ink3);
                continue;
            }

            const auto text = help::clayStageValue (readout, row.stage);
            const auto guard = help::clayLampFor (readout, row.stage) == help::ClayLamp::guard;

            if (text.isEmpty())
                row.value->setDisplay (tr (Text::clayNotActive, language), guard ? palette::warningText : palette::ink3);
            else
                row.value->setDisplay (text, guard ? palette::warningText : palette::violet);
        }
    }

    juce::Rectangle<float> ClayCard::rowBounds (int index) const
    {
        return { (float) padX, 42.0f + 34.0f * (float) index, 246.0f, 30.0f };
    }

    juce::Rectangle<float> ClayCard::stripBounds() const
    {
        return { (float) padX, 144.0f, 246.0f, 20.0f };
    }

    void ClayCard::layoutContent()
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto area = rowBounds ((int) i);
            rows[i].modes->setBounds (juce::Rectangle<int> ((int) area.getRight() - 132, (int) area.getY() + 3, 132, 24));
            rows[i].value->setBounds (juce::Rectangle<int> ((int) area.getX() + 15, (int) area.getY() + 15, 96, 14));
        }
    }

    juce::String ClayCard::getTooltip()
    {
        const auto position = getMouseXYRelative().toFloat();

        if (position.y < (float) (headerTop + headerHeight) || stripBounds().expanded (0.0f, 16.0f).contains (position))
            return getHeaderTooltip();

        for (size_t i = 0; i < rows.size(); ++i)
            if (rowBounds ((int) i).contains (position))
                return position.x < (float) padX + 12.0f ? getHeaderTooltip() : help::tooltipFor (rows[i].valueId, language);

        return {};
    }

    void ClayCard::paint (juce::Graphics& g)
    {
        ModuleCard::paint (g);

        const auto alpha = power.isModuleOn() ? 1.0f : bypassedAlpha;
        const Text names[] = { Text::clayLowCut, Text::clayMono, Text::clayWidth };

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto area = rowBounds ((int) i);
            const auto lamp = help::clayLampFor (readout, rows[i].stage);
            const auto mode = modeOf (rows[i]);

            // 灯：暗 = 灰圈；在处理 = 紫圈（自动）/ 青色实心（手动）；保护介入 = 橙色实心。
            juce::Colour fill = palette::panel, ring = palette::lampIdle;

            if (lamp == help::ClayLamp::guard)
                fill = ring = palette::warning;
            else if (lamp == help::ClayLamp::on)
                std::tie (fill, ring) = mode == 2 ? std::make_pair (palette::c600, palette::c600) : std::make_pair (palette::panel, palette::violet);

            const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ area.getX() + 3.5f, area.getCentreY() });
            g.setColour (fill.withMultipliedAlpha (alpha));
            g.fillEllipse (dot);
            g.setColour (ring.withMultipliedAlpha (alpha));
            g.drawEllipse (dot.reduced (0.75f), 1.5f);

            g.setFont (uiFont (11.5f));
            g.setColour (palette::ink.withMultipliedAlpha (alpha));
            g.drawText (tr (names[i], language), juce::Rectangle<float> (area.getX() + 15.0f, area.getY() + 1.0f, 96.0f, 14.0f),
                        juce::Justification::centredLeft, false);
        }

        // 频段小图：10 Hz ～ 1 kHz（对数），低切以下斜线、收单区浅青、其上立体声区更浅。
        {
            juce::Graphics::ScopedSaveState saved (g);
            g.setOpacity (alpha);

            const auto strip = stripBounds();
            const auto xOf = [&] (float frequency)
            {
                return strip.getX() + juce::jlimit (0.0f, 1.0f, std::log (frequency / 10.0f) / std::log (100.0f)) * strip.getWidth();
            };

            const auto bypassed = ! power.isModuleOn();
            const auto lowCut = ! bypassed && readout.lowCutHz > 0 ? xOf ((float) readout.lowCutHz) : strip.getX();
            const auto mono = ! bypassed && readout.monoPercent > 0 && readout.monoHz > 0 ? juce::jmax (lowCut, xOf ((float) readout.monoHz)) : lowCut;
            const auto bar = juce::Rectangle<float> (strip.getX(), strip.getY() + 4.0f, strip.getWidth(), 12.0f);

            g.setColour (palette::groove.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bar, 3.0f);

            if (lowCut > strip.getX() + 0.5f)
            {
                juce::Graphics::ScopedSaveState hatch (g);
                g.reduceClipRegion (bar.withRight (lowCut).toNearestInt());
                g.setColour (palette::c800.withAlpha (0.35f * alpha));

                for (float x = bar.getX() - bar.getHeight(); x < lowCut; x += 4.0f * std::sqrt (2.0f))
                    g.drawLine (x, bar.getBottom(), x + bar.getHeight(), bar.getY(), 1.2f);
            }

            if (mono > lowCut + 0.5f)
            {
                g.setColour (palette::c200.withMultipliedAlpha (alpha));
                g.fillRect (bar.withLeft (lowCut).withRight (mono));
            }

            {
                juce::Graphics::ScopedSaveState stereo (g);
                g.reduceClipRegion (bar.withLeft (mono).toNearestInt());
                g.setColour (palette::c100.withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (bar, 3.0f);
            }

            g.setColour (palette::c800.withMultipliedAlpha (alpha));

            if (lowCut > strip.getX() + 0.5f)
                g.fillRect (lowCut - 0.6f, strip.getY() + 1.0f, 1.2f, 18.0f);

            if (mono > lowCut + 0.5f)
                g.fillRect (mono - 0.6f, strip.getY() + 1.0f, 1.2f, 18.0f);

            // 标签：低切频率、收单频率、右边「立体声」（加宽时接加宽量）。
            g.setFont (monoFont (10.0f));
            g.setColour (palette::ink3.withMultipliedAlpha (alpha));
            const auto labelY = strip.getBottom();

            if (lowCut > strip.getX() + 0.5f)
                g.drawText (juce::String (readout.lowCutHz), juce::Rectangle<float> (lowCut - 6.0f, labelY, 30.0f, 14.0f),
                            juce::Justification::centredLeft, false);

            if (mono > lowCut + 0.5f)
                g.drawText (juce::String (readout.monoHz) + " Hz", juce::Rectangle<float> (mono - 16.0f, labelY, 60.0f, 14.0f),
                            juce::Justification::centredLeft, false);

            auto stereoLabel = tr (Text::stripStereo, language);

            if (! bypassed)
                if (const auto widthText = help::clayStageValue (readout, help::ClayStage::width); widthText.isNotEmpty())
                    stereoLabel << " " << widthText;

            g.drawText (stereoLabel, juce::Rectangle<float> (strip.getRight() - 130.0f, labelY, 130.0f, 14.0f), juce::Justification::centredRight, false);
        }
    }

    //==============================================================================
    ParamKnob& KnobCard::addKnob (const char* parameterId, ParamKnob::Options options)
    {
        auto* knob = knobs.add (new ParamKnob (parameterFor (state, parameterId), host, options));
        knob->setComponentID (parameterId);
        addContent (*knob);
        return *knob;
    }

    ParamSlider& KnobCard::addSlider (const char* parameterId, bool bipolar)
    {
        auto* slider = sliders.add (new ParamSlider (parameterFor (state, parameterId), host, bipolar));
        slider->setComponentID (parameterId);
        addContent (*slider);
        return *slider;
    }

    void KnobCard::layoutKnobs (int y)
    {
        // 三个 72 px 宽的旋钮在 246 px 里 space-around（设计稿）：两边各留 5 px、之间 10 px。
        for (int i = 0; i < knobs.size(); ++i)
            knobs[i]->setTopLeftPosition (padX + 5 + i * (ParamKnob::width + 10), y);
    }

    //==============================================================================
    BalanceCard::BalanceCard (juce::AudioProcessorValueTreeState& s, InteractionHost& h)
        : KnobCard (s, param::balanceBypass, h, "02", "BALANCE")
    {
        addKnob (param::balanceRecover, { false, 0 });
        addKnob (param::balanceTame, { false, 0 });
        addKnob (param::balanceBrighten, { true, 0 });

        speed.setOptions ({ "", "", "" });
        speed.setComponentID (param::balanceSpeed);
        addContent (speed);
        speedAttachment = std::make_unique<SegmentedAttachment> (speed, parameterFor (state, param::balanceSpeed));
        shownGains.fill (0.0f);
    }

    juce::String BalanceCard::getDescription() const   { return tr (Text::describeBalance, language); }
    juce::String BalanceCard::getNote() const          { return tr (Text::bands27, language); }
    juce::String BalanceCard::getHeaderTooltip() const { return help::tooltipFor (param::balanceBypass, language); }

    void BalanceCard::setLanguage (Language newLanguage)
    {
        ModuleCard::setLanguage (newLanguage);
        power.setTooltip (help::tooltipFor (param::balanceBypass, language));

        const Text names[] = { Text::recover, Text::tame, Text::brighten };
        const char* ids[] = { param::balanceRecover, param::balanceTame, param::balanceBrighten };

        for (int i = 0; i < knobs.size(); ++i)
        {
            knobs[i]->setLabel (tr (names[i], language));
            knobs[i]->setTooltip (help::tooltipFor (ids[i], language));
        }

        speed.setOptions ({ tr (Text::speedSlow, language), tr (Text::speedMid, language), tr (Text::speedFast, language) });
        speed.setTooltip (help::tooltipFor (param::balanceSpeed, language));
    }

    void BalanceCard::setEffective (const ChainParams& effective, bool glazeActive)
    {
        knobs[0]->setEffectiveValue (glazeActive ? std::optional<float> (effective.balance.recover) : std::nullopt);
        knobs[1]->setEffectiveValue (glazeActive ? std::optional<float> (effective.balance.tame) : std::nullopt);
        knobs[2]->setEffectiveValue (std::nullopt);
    }

    void BalanceCard::setBandGains (const BalanceModule::BandGains& gains)
    {
        // 柱高按 0.1 dB 量化：看得出的变化才重画。
        auto changed = false;

        for (size_t i = 0; i < gains.size(); ++i)
        {
            const auto value = std::isfinite (gains[i]) ? std::round (gains[i] * 10.0f) / 10.0f : 0.0f;

            if (! juce::approximatelyEqual (value, shownGains[i]))
            {
                shownGains[i] = value;
                changed = true;
            }
        }

        if (changed)
            repaint (graphBounds().toNearestInt().expanded (2));
    }

    void BalanceCard::layoutContent()
    {
        layoutKnobs (76);
        speed.setBounds (padX + ParamSlider::nameWidth + ParamSlider::gap, 168, 246 - ParamSlider::nameWidth - ParamSlider::gap, 22);
    }

    juce::String BalanceCard::getTooltip()
    {
        const auto position = getMouseXYRelative().toFloat();

        if (graphBounds().contains (position))
            return help::tooltipFor (uiid::balanceCurve, language);

        if (position.y >= 168.0f && position.x < (float) (padX + ParamSlider::nameWidth))
            return help::tooltipFor (param::balanceSpeed, language);

        return ModuleCard::getTooltip();
    }

    void BalanceCard::paint (juce::Graphics& g)
    {
        KnobCard::paint (g);

        const auto alpha = power.isModuleOn() ? 1.0f : bypassedAlpha;
        const auto graph = graphBounds();

        g.setColour (palette::spectrumFloor.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (graph, 6.0f);

        const float dashes[] = { 2.0f, 2.0f };
        g.setColour (juce::Colour (0xff0d3b45).withAlpha (0.18f * alpha));
        g.drawDashedLine (juce::Line<float> (graph.getX() + 4.0f, graph.getY() + 15.5f, graph.getRight() - 4.0f, graph.getY() + 15.5f), dashes, 2, 1.0f);

        // 27 段：提起向上、压下向下；越多颜色越深。
        for (size_t i = 0; i < shownGains.size(); ++i)
        {
            const auto gain = shownGains[i];
            const auto h = juce::jlimit (1.2f, 14.0f, std::abs (gain) * 4.6f);
            const auto x = graph.getX() + 6.0f + (float) i * 8.7f;
            const auto bar = gain >= 0.0f ? juce::Rectangle<float> (x, graph.getY() + 15.0f - h, 5.0f, h)
                                          : juce::Rectangle<float> (x, graph.getY() + 16.0f, 5.0f, h);
            const auto colour = gain >= 0.0f ? cyanAt (0.3f + std::abs (gain) / 3.0f) : cyanAt (0.72f + std::abs (gain) / 10.0f);
            g.setColour (colour.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bar, 1.2f);
        }

        g.setFont (monoFont (10.5f));
        g.setColour (palette::ink3.withMultipliedAlpha (alpha));
        g.drawText (tr (Text::speed, language), juce::Rectangle<float> ((float) padX, 168.0f, (float) ParamSlider::nameWidth, 22.0f),
                    juce::Justification::centredLeft, false);
    }

    //==============================================================================
    ColorCard::ColorCard (juce::AudioProcessorValueTreeState& s, InteractionHost& h)
        : KnobCard (s, param::colorBypass, h, "03", "COLOR")
    {
        addKnob (param::colorTone, { true, 1 });
        addKnob (param::colorGlue, { false, 0 });
        addKnob (param::colorHeat, { false, 0 });
        addSlider (param::colorMix, false);
        addSlider (param::colorOutput, true);
        addContent (glueReduction);
    }

    juce::String ColorCard::getDescription() const   { return tr (Text::describeColor, language); }
    juce::String ColorCard::getHeaderTooltip() const { return help::tooltipFor (param::colorBypass, language); }

    void ColorCard::setLanguage (Language newLanguage)
    {
        ModuleCard::setLanguage (newLanguage);
        power.setTooltip (help::tooltipFor (param::colorBypass, language));

        const Text names[] = { Text::tone, Text::glue, Text::heat };
        const char* ids[] = { param::colorTone, param::colorGlue, param::colorHeat };

        for (int i = 0; i < knobs.size(); ++i)
        {
            knobs[i]->setLabel (tr (names[i], language));
            knobs[i]->setTooltip (help::tooltipFor (ids[i], language));
        }

        sliders[0]->setLabel (tr (Text::mix, language));
        sliders[0]->setTooltip (help::tooltipFor (param::colorMix, language));
        sliders[1]->setLabel (tr (Text::output, language));
        sliders[1]->setTooltip (help::tooltipFor (param::colorOutput, language));
        glueReduction.setLabel (tr (Text::glueReduction, language));
        glueReduction.setTooltip (help::tooltipFor (uiid::glueGr, language));
    }

    void ColorCard::setEffective (const ChainParams& effective, bool glazeActive)
    {
        knobs[0]->setEffectiveValue (glazeActive ? std::optional<float> (effective.color.toneDb) : std::nullopt);
        knobs[1]->setEffectiveValue (glazeActive ? std::optional<float> (effective.color.glue) : std::nullopt);
        knobs[2]->setEffectiveValue (glazeActive ? std::optional<float> (effective.color.heat) : std::nullopt);
    }

    void ColorCard::setGlueReduction (float db)
    {
        glueReduction.pushReading (db, false);
    }

    void ColorCard::layoutContent()
    {
        layoutKnobs (40);
        sliders[0]->setBounds (padX, 128, 246, 18);
        sliders[1]->setBounds (padX, 150, 246, 18);
        glueReduction.setBounds (padX, 172, 246, 18);
    }

    //==============================================================================
    LimitCard::LimitCard (juce::AudioProcessorValueTreeState& s, InteractionHost& h)
        : KnobCard (s, param::limitBypass, h, "04", "LIMIT")
    {
        addKnob (param::limitGain, { false, 1 });
        addKnob (param::limitCeiling, { false, 1 });
        addKnob (param::limitClip, { false, 0 });
        addSlider (param::limitRelease, false);
        addSlider (param::limitLookahead, false);
        addContent (limitReduction);

        autoRelease = std::make_unique<ChipToggle> (parameterFor (state, param::limitReleaseAuto));
        autoRelease->setComponentID (param::limitReleaseAuto);
        autoRelease->onChange = [this] { sliders[0]->setDimmed (autoRelease->isOn()); };
        sliders[0]->setDimmed (autoRelease->isOn());
        addContent (*autoRelease);
    }

    juce::String LimitCard::getDescription() const   { return tr (Text::describeLimit, language); }
    juce::String LimitCard::getHeaderTooltip() const { return help::tooltipFor (param::limitBypass, language); }

    void LimitCard::setLanguage (Language newLanguage)
    {
        ModuleCard::setLanguage (newLanguage);
        power.setTooltip (help::tooltipFor (param::limitBypass, language));

        const Text names[] = { Text::gain, Text::ceiling, Text::clip };
        const char* ids[] = { param::limitGain, param::limitCeiling, param::limitClip };

        for (int i = 0; i < knobs.size(); ++i)
        {
            knobs[i]->setLabel (tr (names[i], language));
            knobs[i]->setTooltip (help::tooltipFor (ids[i], language));
        }

        sliders[0]->setLabel (tr (Text::release, language));
        sliders[0]->setTooltip (help::tooltipFor (param::limitRelease, language));
        sliders[1]->setLabel (tr (Text::lookahead, language));
        sliders[1]->setTooltip (help::tooltipFor (param::limitLookahead, language));
        autoRelease->setText (tr (Text::autoChip, language));
        autoRelease->setTooltip (help::tooltipFor (param::limitReleaseAuto, language));
        limitReduction.setLabel (tr (Text::limitReduction, language));
        limitReduction.setTooltip (help::tooltipFor (uiid::meterGr, language));
        layoutContent();
    }

    void LimitCard::setEffective (const ChainParams& effective, bool glazeActive)
    {
        knobs[0]->setEffectiveValue (glazeActive ? std::optional<float> (effective.limit.gainDb) : std::nullopt);
        knobs[1]->setEffectiveValue (std::nullopt);
        knobs[2]->setEffectiveValue (glazeActive ? std::optional<float> (effective.limit.clip) : std::nullopt);
    }

    void LimitCard::setLimitReduction (float db, bool planLimited)
    {
        limitReduction.pushReading (db, planLimited);
    }

    void LimitCard::layoutContent()
    {
        layoutKnobs (40);

        const auto chipWidth = autoRelease != nullptr ? autoRelease->getNaturalWidth() : 40;
        sliders[0]->setBounds (padX, 128, 246, 18);
        sliders[0]->setTrailingSpace (chipWidth + ParamSlider::gap);

        if (autoRelease != nullptr)
            autoRelease->setBounds (padX + 246 - chipWidth, 128, chipWidth, 18);

        sliders[1]->setBounds (padX, 150, 246, 18);
        limitReduction.setBounds (padX, 172, 246, 18);
    }
}
