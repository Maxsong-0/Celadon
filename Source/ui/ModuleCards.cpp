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
        constexpr int padX = 14, headerTop = 10, headerHeight = 32;
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

    void ModuleCard::setHeaderAccessory (juce::Component& accessory)
    {
        headerAccessory = &accessory;
        addAndMakeVisible (accessory);
    }

    float ModuleCard::headerRightEdge() const
    {
        return (float) (headerAccessory != nullptr ? headerAccessory->getX() : power.getX());
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
        power.setBounds (width - padX - 28, headerTop, 28, 28);

        if (headerAccessory != nullptr)
            headerAccessory->setBounds (power.getX() - 8 - 28, headerTop, 28, 28);

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

        // 名称与说明分两行，给 28 px 电源／耳机键留出独立空间，长英文说明不会压到按钮。
        const auto top = (float) headerTop;
        const auto numberFont = monoFont (12.0f);
        g.setFont (numberFont);
        g.setColour (palette::c600);
        g.drawText (number, juce::Rectangle<float> ((float) padX, top, 26.0f, 18.0f),
                    juce::Justification::centredLeft, false);

        const auto titleX = (float) padX + textWidth (numberFont, number) + 8.0f;
        const auto rightEdge = headerRightEdge() - 7.0f;
        g.setFont (withLetterSpacing (uiFont (13.5f, Weight::semibold), 1.5f));
        g.setColour (palette::ink);
        g.drawText (title, juce::Rectangle<float> (titleX, top, rightEdge - titleX, 18.0f),
                    juce::Justification::centredLeft, false);

        const auto note = getNote();
        const auto noteFont = monoFont (11.0f);
        const auto noteWidth = note.isEmpty() ? 0.0f : textWidth (noteFont, note) + 10.0f;
        const auto subtitleY = top + 18.0f;
        g.setFont (uiFont (11.5f));
        g.setColour (palette::ink3);
        g.drawText (getDescription(), juce::Rectangle<float> (titleX, subtitleY,
                                                            juce::jmax (0.0f, rightEdge - titleX - noteWidth), 14.0f),
                    juce::Justification::centredLeft, true);

        if (note.isNotEmpty())
        {
            g.setFont (noteFont);
            g.drawText (note, juce::Rectangle<float> (rightEdge - noteWidth, subtitleY, noteWidth, 14.0f),
                        juce::Justification::centredRight, false);
        }
    }

    //==============================================================================
    ClayCard::ClayCard (juce::AudioProcessorValueTreeState& s, InteractionHost& h)
        : ModuleCard (s, param::clayBypass, h, "02", "CLAY")
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
            row.modes->setFont (uiFont (11.5f, Weight::medium));
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
        return { (float) padX, 46.0f + 47.0f * (float) index, 246.0f, 42.0f };
    }

    juce::Rectangle<float> ClayCard::stripBounds() const
    {
        return { (float) padX, 204.0f, 246.0f, 20.0f };
    }

    juce::Rectangle<float> ClayCard::houseHintBounds() const
    {
        // 小图连同下面一行标签（204 ～ 238）的位置，往上借 12 px 放两行字；下边距与其余卡片的最后一行齐（248 − 10）。
        return { (float) padX, 192.0f, 246.0f, 46.0f };
    }

    void ClayCard::layoutContent()
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto area = rowBounds ((int) i);
            rows[i].modes->setBounds (juce::Rectangle<int> ((int) area.getRight() - 132, (int) area.getY() + 6, 132, 30));
            rows[i].value->setBounds (juce::Rectangle<int> ((int) area.getX() + 15, (int) area.getY() + 17, 96, 24));
        }
    }

    juce::String ClayCard::getTooltip()
    {
        const auto position = getMouseXYRelative().toFloat();

        if (position.y < (float) (headerTop + headerHeight) || stripBounds().expanded (0.0f, 16.0f).contains (position)
            || (readout.housePhaseHint && houseHintBounds().contains (position)))
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

            // 灯：暗 = 灰圈；在处理 = 紫圈（自动）/ 青色实心（手动）；宽度保护介入（加宽）/ 低频反相提示（收单）= 橙色实心。
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

            g.setFont (uiFont (12.5f));
            g.setColour (palette::ink.withMultipliedAlpha (alpha));
            g.drawText (tr (names[i], language), juce::Rectangle<float> (area.getX() + 15.0f, area.getY(), 96.0f, 17.0f),
                        juce::Justification::centredLeft, false);
        }

        // T-038 House 档反相提示（设计规格-曲风-House学习方式 第 9 节）：亮着时小图那一格换成两行琥珀提示——说的是混音本身
        //（俱乐部单声道回放会丢低频），修坯救不回来，所以不随旁通变淡；灭了就回到小图。
        if (readout.housePhaseHint)
        {
            const auto box = houseHintBounds();
            g.setColour (palette::warningSoft);
            g.fillRoundedRectangle (box, 8.0f);
            g.setColour (palette::warning.withAlpha (0.45f));
            g.drawRoundedRectangle (box.reduced (0.5f), 7.5f, 1.0f);

            const auto lines = help::housePhaseHintLines (readout.housePhaseLossTenthsDb, language);
            const auto text = box.reduced (10.0f, 5.0f);
            g.setColour (palette::warningText);
            g.setFont (uiFont (12.0f, Weight::medium));
            g.drawFittedText (lines[0], text.withHeight (text.getHeight() * 0.5f).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
            g.setFont (uiFont (12.0f));
            g.drawFittedText (lines[1], text.withTrimmedTop (text.getHeight() * 0.5f).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
            return;
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
            g.setFont (monoFont (11.0f));
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
        : KnobCard (s, param::balanceBypass, h, "01", "BALANCE")
    {
        addKnob (param::balanceRecover, { false, 0 });
        addKnob (param::balanceTame, { false, 0 });
        addKnob (param::balanceBrighten, { true, 0 });

        speed.setOptions ({ "", "", "" });
        speed.setFont (uiFont (12.0f, Weight::medium));
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
        layoutKnobs (90);
        speed.setBounds (padX + ParamSlider::nameWidth + ParamSlider::gap, 208, 246 - ParamSlider::nameWidth - ParamSlider::gap, 28);
    }

    juce::String BalanceCard::getTooltip()
    {
        const auto position = getMouseXYRelative().toFloat();

        if (graphBounds().contains (position))
            return help::tooltipFor (uiid::balanceCurve, language);

        if (position.y >= 208.0f && position.x < (float) (padX + ParamSlider::nameWidth))
            return help::tooltipFor (param::balanceSpeed, language);

        return ModuleCard::getTooltip();
    }

    namespace
    {
        // Balance 的 27 段是 1/3 倍频程，中心 40 Hz × 2^(i/3)（40 Hz ～ 约 16 kHz），图的横轴按这个对数刻度：两端各留半段。
        constexpr float balanceMinOctave = -0.5f / 3.0f;                                    // 相对 40 Hz 的倍频程
        constexpr float balanceMaxOctave = (float) (BalanceModule::numBands - 1) / 3.0f + 0.5f / 3.0f;

        float balancePositionOfHz (float hz) noexcept
        {
            return (std::log2 (hz / 40.0f) - balanceMinOctave) / (balanceMaxOctave - balanceMinOctave);
        }

        // 段增益沿对数频率的 Catmull-Rom 插值（段在横轴上等距），position ∈ [0, 1]。
        float balanceGainAt (const BalanceModule::BandGains& gains, float position) noexcept
        {
            const auto n = (int) gains.size();
            const auto t = juce::jlimit (0.0f, (float) (n - 1), (position * (balanceMaxOctave - balanceMinOctave) + balanceMinOctave) * 3.0f);
            const auto i = juce::jmin ((int) t, n - 2);
            const auto f = t - (float) i;
            const auto p0 = gains[(size_t) juce::jmax (0, i - 1)], p1 = gains[(size_t) i];
            const auto p2 = gains[(size_t) (i + 1)], p3 = gains[(size_t) juce::jmin (n - 1, i + 2)];
            return 0.5f * ((2.0f * p1) + (-p0 + p2) * f + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f * f
                           + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f * f * f);
        }
    }

    void BalanceCard::setBackdropSpectrum (const std::vector<float>& averageDb)
    {
        if (averageDb.size() != (size_t) SpectrumAnalyser::numPoints)
            return;

        auto changed = false;
        const auto top = 1.0f / (float) (curvePoints - 1);

        for (int i = 0; i < curvePoints; ++i)
        {
            const auto octave = balanceMinOctave + (float) i * top * (balanceMaxOctave - balanceMinOctave);
            const auto hz = 40.0f * std::exp2 (octave);
            const auto index = juce::jlimit (0, SpectrumAnalyser::numPoints - 1,
                                             juce::roundToInt (SpectrumAnalyser::positionOf (hz) * (float) (SpectrumAnalyser::numPoints - 1)));
            const auto db = averageDb[(size_t) index];
            // −84 ～ −12 dBFS 映到 0 ～ 1；按 1 dB 量化（1/72）。
            const auto level = std::isfinite (db) ? juce::jlimit (0.0f, 1.0f, std::round (db + 84.0f) / 72.0f) : 0.0f;

            if (! juce::approximatelyEqual (level, shownBackdrop[(size_t) i]))
            {
                shownBackdrop[(size_t) i] = level;
                changed = true;
            }
        }

        if (changed)
            repaint (graphBounds().toNearestInt().expanded (2));
    }

    void BalanceCard::paint (juce::Graphics& g)
    {
        KnobCard::paint (g);

        const auto alpha = power.isModuleOn() ? 1.0f : bypassedAlpha;
        const auto graph = graphBounds();

        g.setColour (palette::spectrumFloor.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (graph, 6.0f);

        const auto inner = graph.reduced (4.0f, 2.0f);
        const auto centreY = graph.getCentreY() + 0.5f;
        const auto xAt = [&] (float position) { return inner.getX() + position * inner.getWidth(); };

        g.saveState();
        g.reduceClipRegion (juce::Path ([&] { juce::Path p; p.addRoundedRectangle (graph, 6.0f); return p; }()));

        // 背景：OUT 平均频谱（淡），先画，曲线压在上面。
        {
            juce::Path backdrop;
            auto any = false;
            backdrop.startNewSubPath (xAt (0.0f), graph.getBottom());

            for (int i = 0; i < curvePoints; ++i)
            {
                const auto level = shownBackdrop[(size_t) i];
                any = any || level > 0.0f;
                backdrop.lineTo (xAt ((float) i / (float) (curvePoints - 1)), graph.getBottom() - level * graph.getHeight());
            }

            backdrop.lineTo (xAt (1.0f), graph.getBottom());
            backdrop.closeSubPath();

            if (any)
            {
                g.setColour (cyanAt (0.35f).withAlpha (0.16f * alpha));
                g.fillPath (backdrop);
            }
        }

        // 频率网格：100 Hz、1 kHz、10 kHz 实线，其余十进位刻度（50、200、500、2k、5k）更淡。
        for (const auto hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const auto major = hz == 100.0f || hz == 1000.0f || hz == 10000.0f;
            g.setColour (juce::Colour (0xff0d3b45).withAlpha ((major ? 0.07f : 0.03f) * alpha));
            g.fillRect (juce::Rectangle<float> (std::round (xAt (balancePositionOfHz (hz))), graph.getY(), 1.0f, graph.getHeight()));
        }

        g.setColour (juce::Colour (0xff0d3b45).withAlpha (0.14f * alpha));
        g.fillRect (juce::Rectangle<float> (inner.getX(), centreY - 0.5f, inner.getWidth(), 1.0f));

        // 修正曲线：27 段沿对数频率平滑插值、约每 2 px 一个点；提起在上、压下在下，5 px / dB，±16 px 软限。
        {
            juce::Path curve, fill;
            const auto yOf = [&] (float gain)
            {
                const auto px = 16.0f * std::tanh (gain * 5.0f / 16.0f);
                return centreY - px;
            };

            fill.startNewSubPath (xAt (0.0f), centreY);

            for (int i = 0; i < curvePoints; ++i)
            {
                const auto position = (float) i / (float) (curvePoints - 1);
                const auto point = juce::Point<float> (xAt (position), yOf (balanceGainAt (shownGains, position)));

                if (i == 0) curve.startNewSubPath (point); else curve.lineTo (point);
                fill.lineTo (point);
            }

            fill.lineTo (xAt (1.0f), centreY);
            fill.closeSubPath();

            g.setGradientFill (juce::ColourGradient::vertical (cyanAt (0.55f).withAlpha (0.30f * alpha), graph.getY(),
                                                                cyanAt (0.75f).withAlpha (0.30f * alpha), graph.getBottom()));
            g.fillPath (fill);
            g.setColour (cyanAt (0.7f).withMultipliedAlpha (alpha));
            g.strokePath (curve, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // 27 个段中心的小点：仍看得出是 27 段。
            g.setColour (cyanAt (0.8f).withMultipliedAlpha (0.55f * alpha));

            for (size_t b = 0; b < shownGains.size(); ++b)
            {
                const auto position = balancePositionOfHz (40.0f * std::exp2 ((float) b / 3.0f));
                g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre ({ xAt (position), yOf (shownGains[b]) }));
            }
        }

        g.restoreState();

        g.setFont (uiFont (12.0f));
        g.setColour (palette::ink3.withMultipliedAlpha (alpha));
        g.drawText (tr (Text::speed, language), juce::Rectangle<float> ((float) padX, 208.0f, (float) ParamSlider::nameWidth, 28.0f),
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
        layoutKnobs (52);
        sliders[0]->setBounds (padX, 162, 246, 24);
        sliders[1]->setBounds (padX, 190, 246, 24);
        glueReduction.setBounds (padX, 218, 246, 24);
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

        // 试听差值：LIMIT 旁通时没有东西可听（链也不生效，见 CeladonChain::setParams），耳机禁用。
        audition.setComponentID (uiid::limitAudition);
        audition.onClick = [this] { if (onAuditionClicked != nullptr) onAuditionClicked(); };
        audition.setEnabled (power.isModuleOn());
        setHeaderAccessory (audition);
        power.onChange = [this] { updateBypassLook(); audition.setEnabled (power.isModuleOn()); repaint(); };
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
        audition.setTooltip (help::tooltipFor (uiid::limitAudition, language));
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
        layoutKnobs (52);

        const auto chipWidth = autoRelease != nullptr ? autoRelease->getNaturalWidth() : 40;
        sliders[0]->setBounds (padX, 162, 246, 24);
        sliders[0]->setTrailingSpace (chipWidth + ParamSlider::gap);

        if (autoRelease != nullptr)
            autoRelease->setBounds (padX + 246 - chipWidth, 160, chipWidth, 28);

        sliders[1]->setBounds (padX, 190, 246, 24);
        limitReduction.setBounds (padX, 218, 246, 24);
    }
}
