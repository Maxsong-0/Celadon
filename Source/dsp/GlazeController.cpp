/*
  ==============================================================================

    GlazeController.cpp
    T-048 阶段 2 连续编排。聆听 / 锁定 / 校准不在这里。

    本块 beginBlock 交出的是上一块 endBlock 已经靠过去的增量。endBlock 在分析之后
    最多 makePlan 一次（旋钮变了，或走过 1 秒格子），再把增量靠向新目标。下一块才出声。

  ==============================================================================
*/

#include "GlazeController.h"

#include "ClayGuard.h"
#include "ClayModule.h"
#include "ContrastGuard.h"
#include "GlazeConstants.h"
#include "GlazeGuard.h"
#include "GlazePlanner.h"
#include "GlazeProfile.h"
#include "MusicAnalyzer.h"
#include "OutputLevelTracker.h"
#include "PsrGuard.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace celadon::glaze
{
    namespace
    {
        constexpr float slewUpPerSecond = 0.2f;
        constexpr float slewDownPerSecond = 1.0f;
        constexpr float deadbandExit = 0.3f;
        constexpr float deadbandEnter = 0.1f;
        constexpr float trimRateDbPerSecond = 0.05f;
        constexpr float trimClampDb = 3.0f;
        constexpr float deliveryRetreatPerSecond = 0.5f;
        constexpr float deliveryReleasePerSecond = 0.15f;
        constexpr float deliveryFloorDb = -3.0f;
        constexpr float weightSlewPerSecond = 0.2f;
        constexpr float clayScalePerSecond = 1.0f;
        constexpr int liveWrapperFormat = 1;
        constexpr int axisCount = 7;

        bool isFinite (float x) noexcept
        {
            return std::isfinite (x);
        }

        bool sameBits (float a, float b) noexcept
        {
            return std::memcmp (&a, &b, sizeof (float)) == 0;
        }

        bool sameBool (bool a, bool b) noexcept
        {
            return a == b;
        }

        Style styleFromIndex (int index) noexcept
        {
            return static_cast<Style> (juce::jlimit ((int) Style::automatic, (int) Style::clean, index));
        }

        Style dominantStyle (const StyleWeights& weights) noexcept
        {
            auto best = Style::clean;
            auto bestWeight = weights.clean;

            if (weights.warm > bestWeight)
            {
                best = Style::warm;
                bestWeight = weights.warm;
            }

            if (weights.punchy > bestWeight)
                best = Style::punchy;

            return best;
        }

        ClayPlan sanitizeClay (const ClayPlan& plan) noexcept
        {
            const ClayPlan neutral {};
            ClayPlan out;
            out.lowCutHz = isFinite (plan.lowCutHz) && plan.lowCutHz > 0.0f
                               ? juce::jlimit (ClayModule::minLowCutHz, ClayModule::maxLowCutHz, plan.lowCutHz) : 0.0f;
            out.monoHz = isFinite (plan.monoHz) ? juce::jlimit (ClayModule::minMonoHz, ClayModule::maxMonoHz, plan.monoHz) : neutral.monoHz;
            out.monoAmount = isFinite (plan.monoAmount) ? juce::jlimit (0.0f, 1.0f, plan.monoAmount) : 0.0f;
            out.widthDb = isFinite (plan.widthDb) ? juce::jlimit (ClayModule::minWidthDb, ClayModule::maxWidthDb, plan.widthDb) : 0.0f;
            out.costLimitedMask = static_cast<std::uint8_t> (plan.costLimitedMask
                                                              & (clayCostLimitedLowCut | clayCostLimitedMono | clayCostLimitedWidth));
            return out;
        }

        std::uint8_t reportedCostMask (std::uint8_t planned, const ClaySettings& settings) noexcept
        {
            unsigned int mask = 0;

            if (settings.lowCutMode == ClayMode::automatic)
                mask |= (unsigned int) (planned & clayCostLimitedLowCut);

            if (settings.monoMode == ClayMode::automatic)
                mask |= (unsigned int) (planned & clayCostLimitedMono);

            if (settings.widthMode == ClayMode::automatic)
                mask |= (unsigned int) (planned & clayCostLimitedWidth);

            return (std::uint8_t) mask;
        }

        bool planningKnobsEqual (const ChainParams& a, const ChainParams& b) noexcept
        {
            if (! sameBits (a.glazePercent, b.glazePercent) || a.glazeStyle != b.glazeStyle || a.glazeGenre != b.glazeGenre
                || a.glazeDelivery != b.glazeDelivery)
                return false;

            const auto& ab = a.balance;
            const auto& bb = b.balance;

            if (! sameBits (ab.recover, bb.recover) || ! sameBits (ab.tame, bb.tame) || ! sameBits (ab.brighten, bb.brighten)
                || ab.speed != bb.speed || ! sameBool (ab.bypass, bb.bypass) || ! sameBits (ab.harshAmount, bb.harshAmount)
                || ! sameBits (ab.transientPreserve, bb.transientPreserve) || ! sameBits (ab.targetAmount, bb.targetAmount)
                || ! sameBits (ab.targetSlopeDbPerOct, bb.targetSlopeDbPerOct) || ! sameBool (ab.hasTargetReference, bb.hasTargetReference))
                return false;

            const auto& ac = a.color;
            const auto& bc = b.color;

            if (! sameBits (ac.toneDb, bc.toneDb) || ! sameBits (ac.glue, bc.glue) || ! sameBits (ac.heat, bc.heat)
                || ! sameBits (ac.mixPercent, bc.mixPercent) || ! sameBits (ac.outputDb, bc.outputDb) || ! sameBool (ac.bypass, bc.bypass))
                return false;

            const auto& al = a.limit;
            const auto& bl = b.limit;

            if (! sameBits (al.gainDb, bl.gainDb) || ! sameBits (al.ceilingDb, bl.ceilingDb) || ! sameBits (al.releaseMs, bl.releaseMs)
                || ! sameBool (al.autoRelease, bl.autoRelease) || ! sameBits (al.lookaheadMs, bl.lookaheadMs)
                || ! sameBits (al.clip, bl.clip) || ! sameBool (al.bypass, bl.bypass))
                return false;

            const auto& ay = a.claySettings;
            const auto& by = b.claySettings;
            return ay.lowCutMode == by.lowCutMode && ay.monoMode == by.monoMode && ay.widthMode == by.widthMode
                   && sameBits (ay.lowCutHz, by.lowCutHz) && sameBits (ay.monoHz, by.monoHz) && sameBits (ay.widthDb, by.widthDb)
                   && sameBool (ay.bypass, by.bypass);
        }

        StyleConstants releaseManualBudget (StyleConstants caps, std::uint8_t released) noexcept
        {
            if ((released & manualBudgetHeat) != 0)
                caps.heatDriveCap = 0.0f;

            if ((released & manualBudgetGlue) != 0)
                caps.glueGrMeanCapDb = caps.glueGrP95CapDb = 0.0f;

            if ((released & manualBudgetClip) != 0)
                caps.clipDepthCapDb = caps.clipActivityCap = 0.0f;

            return caps;
        }
    }

    ClayPlan blendClayPlans (const ClayPlan& from, const ClayPlan& to, float t, float lowCutFloorHz) noexcept
    {
        if (! (t > 0.0f))
            return from;

        if (! (t < 1.0f))
            return to;

        const auto floorHz = isFinite (lowCutFloorHz) && lowCutFloorHz > 0.0f ? lowCutFloorHz : ClayModule::minLowCutHz;
        const auto linearMix = [t] (float a, float b) noexcept { return a + (b - a) * t; };
        const auto logMix = [t] (float a, float b) noexcept
        {
            if (! (a > 0.0f && b > 0.0f && isFinite (a) && isFinite (b)))
                return a + (b - a) * t;

            const auto mixed = (float) std::exp (std::log ((double) a) + (std::log ((double) b) - std::log ((double) a)) * (double) t);
            return juce::jlimit (juce::jmin (a, b), juce::jmax (a, b), mixed);
        };

        ClayPlan out;

        if (from.lowCutHz > 0.0f || to.lowCutHz > 0.0f)
            out.lowCutHz = logMix (from.lowCutHz > 0.0f ? from.lowCutHz : floorHz, to.lowCutHz > 0.0f ? to.lowCutHz : floorHz);

        auto fromMonoHz = from.monoHz;
        auto toMonoHz = to.monoHz;

        if (! (from.monoAmount > 0.0f) && to.monoAmount > 0.0f)
            fromMonoHz = to.monoHz;
        else if (from.monoAmount > 0.0f && ! (to.monoAmount > 0.0f))
            toMonoHz = from.monoHz;

        out.monoHz = logMix (fromMonoHz, toMonoHz);
        out.monoAmount = linearMix (from.monoAmount, to.monoAmount);
        out.widthDb = linearMix (from.widthDb, to.widthDb);
        out.costLimitedMask = to.costLimitedMask;
        return out;
    }

    //==============================================================================
    struct GlazeController::Impl
    {
        struct Axis
        {
            float value = 0.0f;
            bool moving = false;
        };

        static constexpr int statusWords = (int) ((sizeof (Status) + 3u) / 4u);

        MusicAnalyzer analyzer;
        LiveProfile liveProfile;
        GlazeGuard guard;
        ContrastGuard contrast;
        PsrGuard psr;
        OutputLevelTracker outputLevel;
        ClayGuard clayGuard;
        ClayPressureMonitor pressure;
        ClayGuardKeepMonitor clayKeep;

        bool prepared = false;
        bool analyzerReady = false;
        double sampleRate = 48000.0;
        double analyzedSampleRate = 0.0;
        int analyzedChannels = 0;
        std::int64_t positionTolerance = 48;

        bool hasOverride = false;
        Constants overrideConstants {};

        ChainParams manual {};
        ChainParams effective {};
        bool haveManual = false;
        bool paused = false;
        bool active = false;
        Style style = Style::automatic;
        Genre genre = Genre::general;
        Delivery delivery = Delivery::standard;
        MainSectionModel pendingModel = MainSectionModel::loudness;
        MainSectionModel appliedModel = MainSectionModel::loudness;

        HostInfo host {};
        bool hostFresh = false;
        bool offline = false;
        bool expectKnown = false;
        bool lastPlaying = false;
        std::int64_t expectedPosition = 0;
        int hostInputOffset = 0;

        bool blockMusic = false;
        float currentMainness = 1.0f;
        bool needsPlan = true;
        std::int64_t processedSamples = 0;
        std::int64_t plannedGridIndex = -1;

        Plan plan {};
        StyleWeights weights {};
        bool weightsReady = false;
        Axis axes[(size_t) axisCount] {};
        float gTrim = 0.0f;
        float deliveryRelief = 0.0f;
        GuardOutput budgetOut {};
        GuardOutput deliveryOut {};
        GuardOutput contrastOut {};
        GuardOutput psrOut {};
        float protectionDb = 0.0f;
        GuardCause guardCause = GuardCause::none;
        bool guardEngaged = false;
        std::uint8_t guardReleased = 0;

        float liveToneOffsetDb = 0.0f;
        float liveHeatReduction = 0.0f;
        float liveGlueReduction = 0.0f;
        ClayPlan liveClay {};
        float clayScale = 1.0f;
        float clayScaleTarget = 1.0f;
        double claySeconds = 0.0;
        bool clayFromRestore = false;
        bool levelWithdrawn = false;

        std::unique_ptr<juce::XmlElement> stashed;
        bool restorePending = false;
        std::atomic<bool> legacyDiscarded { false };

        std::atomic<std::uint32_t> statusSeq { 0 };
        std::atomic<std::uint32_t> statusWord[(size_t) statusWords];

        const Constants& constantsInUse() const noexcept
        {
            return hasOverride ? overrideConstants : factoryConstants();
        }

        const Constants* planConstants() const noexcept
        {
            return hasOverride ? &overrideConstants : nullptr;
        }

        bool hostPlaying() const noexcept
        {
            if (offline)
                return true;

            return ! host.hasPlayState || host.isPlaying;
        }

        void resetControl() noexcept
        {
            plan = Plan {};
            weights = StyleWeights {};
            weightsReady = false;
            needsPlan = true;
            plannedGridIndex = -1;
            processedSamples = 0;
            gTrim = 0.0f;
            deliveryRelief = 0.0f;
            budgetOut = {};
            deliveryOut = {};
            contrastOut = {};
            psrOut = {};
            protectionDb = 0.0f;
            guardCause = GuardCause::none;
            guardEngaged = false;
            guardReleased = 0;
            liveToneOffsetDb = 0.0f;
            liveHeatReduction = 0.0f;
            liveGlueReduction = 0.0f;
            liveClay = {};
            clayScale = 1.0f;
            clayScaleTarget = 1.0f;
            claySeconds = 0.0;
            clayFromRestore = false;
            levelWithdrawn = false;
            appliedModel = MainSectionModel::loudness;
            currentMainness = 1.0f;
            blockMusic = false;

            for (auto& axis : axes)
                axis = {};
        }

        void snapRelief() noexcept
        {
            for (auto& axis : axes)
            {
                axis.value = 0.0f;
                axis.moving = false;
            }

            gTrim = 0.0f;
            deliveryRelief = 0.0f;
            deliveryOut = {};
            budgetOut = {};
            contrastOut = {};
            psrOut = {};
            protectionDb = 0.0f;
            guardCause = GuardCause::none;
            guardEngaged = false;
            guard.releaseOutput();
            contrast.snapRelief();
            psr.snapRelief();
        }

        AutoIncrements appliedIncrements() const noexcept
        {
            AutoIncrements inc;
            inc.balanceRecover = axes[0].value;
            inc.balanceTame = axes[1].value;
            inc.colorToneDb = axes[2].value;
            inc.colorGlue = axes[3].value;
            inc.colorHeat = axes[4].value;
            inc.limitGainDb = axes[5].value;
            inc.limitClip = axes[6].value;
            return inc;
        }

        void slewToward (const AutoIncrements& target, double dt, bool ratchetGain) noexcept
        {
            const float targets[axisCount] = { target.balanceRecover, target.balanceTame, target.colorToneDb, target.colorGlue,
                                               target.colorHeat, target.limitGainDb, target.limitClip };

            for (int i = 0; i < axisCount; ++i)
            {
                auto& axis = axes[(size_t) i];
                const auto goal = isFinite (targets[i]) ? targets[i] : 0.0f;
                const auto error = goal - axis.value;
                const auto magnitude = std::abs (error);

                if (magnitude < deadbandEnter)
                    axis.moving = false;
                else if (magnitude > deadbandExit)
                    axis.moving = true;

                if (! axis.moving)
                    continue;

                if (ratchetGain && i == 5 && error > 0.0f)
                    continue;

                const auto rate = error > 0.0f ? slewUpPerSecond : slewDownPerSecond;
                const auto step = rate * (float) dt;

                if (magnitude <= step)
                    axis.value = goal;
                else
                    axis.value += std::copysign (step, error);
            }
        }

        float readMainness() noexcept
        {
            float lufs = 0.0f;
            float lowShare = 0.0f;
            const auto reading = analyzer.getSegmentLedger().getCurrentReading (lufs, lowShare);

            if (reading == SegmentLedger::Reading::quiet)
                return 0.0f;

            if (reading == SegmentLedger::Reading::none)
                return 1.0f;

            const auto mainness = liveProfile.mainnessFor (isFinite (lufs), lufs, isFinite (lowShare), lowShare);
            return isFinite (mainness) ? juce::jlimit (0.0f, 1.0f, mainness) : 1.0f;
        }

        const MusicProfile* publishedProfile() noexcept
        {
            const auto diagnostics = liveProfile.getDiagnostics();

            if (diagnostics.commits == 0 || ! (diagnostics.confidence > 0.0f))
                return nullptr;

            return &liveProfile.getProfile();
        }

        void followWeights (bool move, double dt) noexcept
        {
            const auto* profile = publishedProfile();

            if (profile == nullptr)
                return;

            const auto target = resolveAutomaticWeights (*profile, constantsInUse());

            if (! weightsReady)
            {
                weights = target;
                weightsReady = true;
                return;
            }

            if (! move || ! (dt > 0.0))
                return;

            const auto step = weightSlewPerSecond * (float) dt;
            auto approach = [step] (float& value, float goal) noexcept
            {
                value += juce::jlimit (-step, step, goal - value);
            };

            approach (weights.warm, target.warm);
            approach (weights.punchy, target.punchy);
            approach (weights.clean, target.clean);

            const auto sum = weights.warm + weights.punchy + weights.clean;

            if (sum > 0.0f)
            {
                weights.warm /= sum;
                weights.punchy /= sum;
                weights.clean /= sum;
            }
        }

        void syncGuardLimits() noexcept
        {
            const auto released = (std::uint8_t) (plan.manualBudgetMask & manualBudgetAll);
            auto caps = releaseManualBudget (blendConstants (constantsInUse(), plan.weights, genre, delivery), released);
            guard.setLimits (caps, constantsInUse());

            if (released != guardReleased)
            {
                guard.clearStatistics();
                guardReleased = released;
            }
        }

        void makePlanNow() noexcept
        {
            PlanInputs inputs;
            inputs.profile = publishedProfile();
            inputs.style = style;
            inputs.genre = genre;
            inputs.delivery = delivery;
            inputs.glazePercent = manual.glazePercent;
            inputs.manual = manual;
            inputs.constants = planConstants();
            inputs.lockedWeights = weightsReady ? &weights : nullptr;
            plan = makePlan (inputs);
            levelWithdrawn = plan.automaticLevelWithdrawn;
            syncGuardLimits();
        }

        void updateDelivery (bool music, float truePeak, float ceiling, double dt) noexcept
        {
            if (! music)
            {
                deliveryOut.reliefDb = deliveryRelief;
                deliveryOut.engaged = false;
                deliveryOut.cause = deliveryRelief < 0.0f ? GuardCause::deliveryPeak : GuardCause::none;
                return;
            }

            const auto over = isFinite (truePeak) && isFinite (ceiling) && truePeak > ceiling;

            if (over)
                deliveryRelief = juce::jmax (deliveryFloorDb, deliveryRelief - deliveryRetreatPerSecond * (float) dt);
            else if (deliveryRelief < 0.0f)
                deliveryRelief = juce::jmin (0.0f, deliveryRelief + deliveryReleasePerSecond * (float) dt);

            const auto atFloor = over && ! (deliveryRelief > deliveryFloorDb + 0.0001f);
            deliveryOut.reliefDb = deliveryRelief;
            deliveryOut.engaged = (over && ! atFloor) || atFloor;
            deliveryOut.cause = deliveryOut.engaged || deliveryRelief < 0.0f ? GuardCause::deliveryPeak : GuardCause::none;
        }

        void updateTrim (bool freeze, double dt) noexcept
        {
            if (freeze || ! outputLevel.hasMainLevel() || ! (dt > 0.0))
                return;

            const auto error = plan.effectiveTargetLufs - outputLevel.mainLevelLufs();

            if (! isFinite (error))
                return;

            const auto step = trimRateDbPerSecond * (float) dt;
            gTrim = juce::jlimit (-trimClampDb, trimClampDb, gTrim + juce::jlimit (-step, step, error));
        }

        void rememberMinRelief() noexcept
        {
            auto chosen = budgetOut;

            if (deliveryOut.reliefDb < chosen.reliefDb)
                chosen = deliveryOut;

            if (contrastOut.reliefDb < chosen.reliefDb)
                chosen = contrastOut;

            if (psrOut.reliefDb < chosen.reliefDb)
                chosen = psrOut;

            protectionDb = juce::jmin (0.0f, chosen.reliefDb);
            guardCause = chosen.cause;
            guardEngaged = chosen.engaged;
        }

        float clayFloorHz() const noexcept
        {
            const auto hz = constantsInUse().clayLowCutFloorHz;

            if (! (isFinite (hz) && hz > 0.0f))
                return ClayModule::minLowCutHz;

            return juce::jlimit (ClayModule::minLowCutHz, ClayModule::maxLowCutHz, hz);
        }

        bool claySupported() noexcept
        {
            if (liveProfile.getDiagnostics().commits == 0)
                return false;

            return liveProfile.getProfile().formatVersion >= clayProfileFormatVersion
                   && plan.algorithmVersion == currentAlgorithmVersion;
        }

        bool slopesMatch (const MusicProfile& profile, float& referenceSlope, float& currentSlope) const noexcept
        {
            const auto& view = analyzer.getShortTermView();
            const auto& learned = profile.spectralSlopeDbPerOct;
            const auto& current = view.liveSpectralSlopeDbPerOct;

            if (! learned.valid || ! isFinite (learned.value) || learned.confidence < 0.5f
                || ! current.valid || ! isFinite (current.value) || current.confidence < 0.6f)
                return false;

            double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, trusted = 0.0;

            for (int band = 0; band < numBands; ++band)
            {
                const auto w = (double) view.liveSlopeWeights[(size_t) band];
                const auto y = profile.bandLongTermDb[(size_t) band];
                const auto confidence = profile.bandConfidence[(size_t) band];

                if (! isFinite (y) || (w > 0.0 && (! isFinite (confidence) || ! (confidence > 0.0f))))
                    return false;

                trusted += w * (double) juce::jlimit (0.0f, 1.0f, confidence);
                const auto x = (double) band / 3.0;
                sw += w;
                sx += w * x;
                sy += w * y;
                sxx += w * x * x;
                sxy += w * x * y;
            }

            const auto denominator = sw * sxx - sx * sx;

            if (! (denominator > 1.0e-9) || trusted < 0.5 * sw)
                return false;

            referenceSlope = (float) ((sw * sxy - sx * sy) / denominator);
            currentSlope = current.value;
            return true;
        }

        void updateLiveColour (double dt) noexcept
        {
            const auto* profile = publishedProfile();
            const auto& view = analyzer.getShortTermView();

            if (profile == nullptr || paused || manual.color.bypass || ! (manual.color.mixPercent > 0.0f) || ! view.musicActive
                || ! view.momentaryLufs.valid || ! isFinite (view.momentaryLufs.value) || view.momentaryLufs.value < -50.0f
                || ! profile->integratedLufs.valid || profile->integratedLufs.confidence < 0.5f || profile->representativeness < 0.5f
                || view.momentaryLufs.value < profile->integratedLufs.value - 15.0f || ! view.psr3sDb.valid || ! isFinite (view.psr3sDb.value)
                || ! profile->plrDb.valid || ! isFinite (profile->plrDb.value) || profile->plrDb.confidence < 0.5f)
                return;

            float referenceSlope = 0.0f, currentSlope = 0.0f;

            if (! slopesMatch (*profile, referenceSlope, currentSlope))
                return;

            const auto& constants = constantsInUse();

            if (! (isFinite (constants.liveColorDensityRangeDb) && constants.liveColorDensityRangeDb > 0.0f
                   && isFinite (constants.liveColorBrightRangeDbPerOct) && constants.liveColorBrightRangeDbPerOct > 0.0f
                   && isFinite (constants.liveColorDensityDeadbandDb) && isFinite (constants.liveColorBrightDeadbandDbPerOct)))
                return;

            const auto density = juce::jlimit (0.0f, 1.0f, juce::jmax (profile->plrDb.value - view.psr3sDb.value - constants.liveColorDensityDeadbandDb,
                                                                        8.0f - view.psr3sDb.value)
                                                             / constants.liveColorDensityRangeDb);
            const auto brightness = juce::jlimit (0.0f, 1.0f, (currentSlope - juce::jmin (referenceSlope, 0.0f) - constants.liveColorBrightDeadbandDbPerOct)
                                                                 / constants.liveColorBrightRangeDbPerOct);

            auto approach = [dt, &constants] (float& reduction, float risk, float maximum) noexcept
            {
                if (! isFinite (maximum))
                    return;

                maximum = juce::jlimit (0.0f, 1.0f, maximum);
                const auto goal = maximum * risk;

                if (goal != 0.0f && std::abs (goal - reduction) < 0.025f)
                    return;

                const auto speed = goal > reduction ? constants.liveColorAttackPerSecond : constants.liveColorReleasePerSecond;

                if (! (isFinite (speed) && speed > 0.0f))
                    return;

                const auto step = juce::jmin (1.0f, speed) * (float) dt;
                reduction = juce::jlimit (0.0f, maximum, reduction + juce::jlimit (-step, step, goal - reduction));
            };

            approach (liveHeatReduction, juce::jmax (density, brightness), constants.liveHeatMaxReduction);
            approach (liveGlueReduction, juce::jmax (density, 0.5f * brightness), constants.liveGlueMaxReduction);
        }

        void updateLiveTone (double dt) noexcept
        {
            const auto* profile = publishedProfile();
            const auto& view = analyzer.getShortTermView();

            if (profile == nullptr || paused || manual.color.bypass || ! (manual.color.mixPercent > 0.0f) || ! view.musicActive)
                return;

            const auto& learned = profile->spectralSlopeDbPerOct;
            const auto& current = view.liveSpectralSlopeDbPerOct;

            if (! current.valid || ! isFinite (current.value) || current.confidence < 0.6f || ! learned.valid || ! isFinite (learned.value)
                || learned.confidence < 0.5f || ! view.momentaryLufs.valid || view.momentaryLufs.value < -50.0f
                || (profile->integratedLufs.valid && view.momentaryLufs.value < profile->integratedLufs.value - 15.0f))
                return;

            const auto& constants = constantsInUse();
            const auto caps = blendConstants (constants, plan.weights);
            const auto strength = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f) * confidenceScale (profile, constants);

            if (! (strength > 0.0f) || ! isFinite (caps.toneMaxDb) || ! (caps.toneMaxDb > 0.0f) || ! isFinite (constants.toneDbPerSlopeDb))
                return;

            float referenceSlope = 0.0f, currentSlope = 0.0f;

            if (! slopesMatch (*profile, referenceSlope, currentSlope))
                return;

            const auto difference = referenceSlope - currentSlope;
            const auto outside = std::copysign (juce::jmax (0.0f, std::abs (difference) - automaticToneSlopeDeadbandDbPerOct), difference);
            const auto base = strength > 0.0f ? plan.increments.colorToneDb / strength : 0.0f;
            const auto brightCap = toneBrightCapDb (caps, constants, genre);
            const auto goal = juce::jlimit (-caps.toneMaxDb, brightCap, base + outside * juce::jmax (0.0f, constants.toneDbPerSlopeDb)) - base;
            const auto step = 0.15f * (float) dt;
            liveToneOffsetDb += juce::jlimit (-step, step, goal - liveToneOffsetDb);
        }

        void applySpectrumFollow() noexcept
        {
            const auto* profile = publishedProfile();
            const auto& constants = constantsInUse();

            if (profile != nullptr && ! manual.color.bypass && manual.color.mixPercent > 0.0f)
            {
                if (isFinite (manual.color.heat))
                    effective.color.heat -= juce::jmax (0.0f, effective.color.heat - manual.color.heat) * liveHeatReduction;

                if (isFinite (manual.color.glue))
                    effective.color.glue -= juce::jmax (0.0f, effective.color.glue - manual.color.glue) * liveGlueReduction;
            }

            if (! manual.balance.bypass && profile != nullptr)
            {
                const auto& evidence = analyzer.getShortTermView().liveTameEvidenceDb;

                if (evidence.valid && isFinite (evidence.value) && constants.tameEvidenceFullDb > 0.0f)
                {
                    const auto caps = blendConstants (constants, plan.weights);
                    const auto glaze = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f);
                    const auto tameCap = isFinite (caps.tameCap) ? juce::jlimit (0.0f, 100.0f, caps.tameCap) : 0.0f;
                    const auto kappa = confidenceScale (profile, constants);
                    const auto liveTame = tameCap * glaze * kappa * juce::jlimit (0.0f, 1.0f, evidence.value / constants.tameEvidenceFullDb);
                    const auto increments = appliedIncrements();
                    effective.balance.tame = juce::jlimit (0.0f, 100.0f, manual.balance.tame + juce::jmax (increments.balanceTame, liveTame));
                    effective.balance.automaticTame = juce::jmax (0.0f, effective.balance.tame - manual.balance.tame);
                }
            }

            if (profile != nullptr && ! manual.color.bypass && manual.color.mixPercent > 0.0f)
            {
                const auto caps = blendConstants (constants, plan.weights);
                const auto strength = juce::jlimit (0.0f, 1.0f, manual.glazePercent * 0.01f) * confidenceScale (profile, constants);

                if (isFinite (caps.toneMaxDb) && caps.toneMaxDb > 0.0f && strength > 0.0f && isFinite (manual.color.toneDb))
                {
                    const auto bright = toneBrightCapDb (caps, constants, genre);
                    const auto offset = std::round (liveToneOffsetDb * 100.0f) * 0.01f;
                    const auto increments = appliedIncrements();
                    const auto tone = juce::jlimit (-caps.toneMaxDb * strength, bright * strength, increments.colorToneDb + offset * strength);
                    effective.color.toneDb = juce::jlimit (-3.0f, 3.0f, manual.color.toneDb + tone);
                }
            }
        }

        void applyLimitExtras() noexcept
        {
            const auto manualGain = manual.limit.gainDb;
            const auto after = effective.limit.gainDb;

            if (! isFinite (manualGain) || ! isFinite (after))
                return;

            const auto trim = levelWithdrawn ? juce::jmin (0.0f, gTrim) : gTrim;
            effective.limit.gainDb = juce::jlimit (manualGain, 24.0f, after + trim + protectionDb);
        }

        void updateClay() noexcept
        {
            const auto seconds = claySeconds;
            claySeconds = 0.0;
            const auto& view = analyzer.getShortTermView();

            if (! active)
            {
                liveClay = {};
                clayGuard.update (liveClay, view, 0.0, false);
                clayGuard.updateHouseHint (view, 0.0, false, 0.0);
            }
            else if (clayFromRestore)
            {
                clayFromRestore = false;
                clayGuard.setConstants (constantsInUse());
            }
            else if (claySupported())
            {
                clayGuard.setConstants (constantsInUse());
                const auto planned = scaleClayPlan (sanitizeClay (plan.clay), clayScale, clayFloorHz());
                liveClay = sanitizeClay (clayGuard.update (planned, view, seconds, true));
            }
            else
            {
                clayGuard.reset();
                liveClay = {};
            }

            effective.clay = resolveClayParams (manual.claySettings, liveClay, manual.glazePercent);

            if (active && isHouse (genre) && claySupported())
            {
                const auto& ledger = analyzer.getSegmentLedger();
                const auto& house = constantsInUse().house;
                const auto barSeconds = ledger.getMode() == SegmentMode::bars ? ledger.getCurrentSegmentSeconds() : 0.0;
                const auto required = barSeconds > 0.0 ? (double) juce::jmax (1, house.phaseHintSegments) * barSeconds
                                                       : house.phaseHintFallbackSeconds;
                clayGuard.updateHouseHint (view, seconds, ledger.isCurrentSegmentFull(), required);
            }
            else if (active)
            {
                clayGuard.updateHouseHint (view, 0.0, false, 0.0);
            }
        }

        void publish() noexcept
        {
            Status status;
            const auto diagnostics = liveProfile.getDiagnostics();
            status.effectiveTargetLufs = plan.effectiveTargetLufs;
            status.protectionDb = protectionDb;
            status.limitedByManual = plan.limitedByManual;
            status.resolvedStyle = dominantStyle (plan.weights);
            status.confidence = diagnostics.confidence;
            status.currentMainness = currentMainness;
            status.coldExport = offline && diagnostics.confidence < 0.5f;
            status.guardCause = guardCause;
            status.guardEngaged = guardEngaged;
            status.legacyStateDiscarded = legacyDiscarded.load (std::memory_order_relaxed);
            status.clayLowCutHz = effective.clay.lowCutHz;
            status.clayMonoHz = effective.clay.monoHz;
            status.clayMonoAmount = effective.clay.monoAmount;
            status.clayWidthDb = effective.clay.widthDb;
            status.clayPhaseGuardActive = clayGuard.getState().phaseActive;
            status.clayWidthGuardActive = clayGuard.getState().widthActive;
            status.clayPeakGuardActive = clayKeep.active();
            status.clayPeakGuardKept = clayKeep.keptFraction();
            status.clayCostLimitedMask = active ? reportedCostMask (liveClay.costLimitedMask, manual.claySettings) : 0;
            status.houseActive = isHouse (genre);
            status.housePhaseHint = clayGuard.getHouseHint().active;
            status.housePhaseMonoLossDb = clayGuard.getHouseHint().monoLossDb;

            std::uint32_t raw[(size_t) statusWords] {};
            std::memcpy (raw, &status, sizeof (Status));
            const auto seq = statusSeq.load (std::memory_order_relaxed);
            statusSeq.store (seq + 1u, std::memory_order_release);

            for (int i = 0; i < statusWords; ++i)
                statusWord[(size_t) i].store (raw[(size_t) i], std::memory_order_relaxed);

            statusSeq.store (seq + 2u, std::memory_order_release);
        }

        void onDiscontinuity (Discontinuity kind) noexcept
        {
            if (! analyzerReady)
                return;

            analyzer.handleDiscontinuity (kind);
        }

        void noteTransport() noexcept
        {
            if (! hostFresh || ! host.hasPosition || ! expectKnown)
                return;

            const auto playingNow = hostPlaying();
            const auto difference = host.positionSamples - expectedPosition;
            const auto magnitude = difference < 0 ? -difference : difference;

            if (magnitude <= positionTolerance || ! playingNow)
                return;

            if (lastPlaying && host.hasLoop && host.looping && difference < 0)
                onDiscontinuity (Discontinuity::loopWrap);
            else
                onDiscontinuity (Discontinuity::transportJump);
        }

        void rememberHost (int numSamples) noexcept
        {
            if (hostFresh && host.hasPosition)
            {
                const auto playingNow = hostPlaying();
                expectedPosition = host.positionSamples + (playingNow ? (std::int64_t) numSamples : 0);
                lastPlaying = playingNow;
                expectKnown = true;
            }
            else if (! hostFresh && expectKnown && host.hasPosition)
            {
                if (hostPlaying())
                    expectedPosition += (std::int64_t) numSamples;
            }
            else
            {
                expectKnown = false;
            }

            hostFresh = false;
        }

        bool looksLegacy (const juce::XmlElement& xml) const noexcept
        {
            const auto version = xml.getIntAttribute ("formatVersion", 0);

            if (version > 0 && version <= 1)
                return true;

            return xml.getChildByName (profileTag) != nullptr || xml.getChildByName ("PLAN") != nullptr
                   || xml.getChildByName ("CALIBRATION") != nullptr || xml.getChildByName (executionTag) != nullptr
                   || xml.getChildByName ("GLAZE_CONTROLLER") != nullptr;
        }

        std::unique_ptr<juce::XmlElement> createLiveBody() const
        {
            auto body = std::make_unique<juce::XmlElement> ("LIVE_BODY");

            if (auto profile = liveProfile.createStateXml())
                body->addChildElement (profile.release());

            auto* control = body->createNewChildElement ("CONTROL");
            xmlio::Writer writer (*control);
            const char* names[axisCount] = { "recover", "tame", "tone", "glue", "heat", "gain", "clip" };
            const char* moving[axisCount] = { "recoverMoving", "tameMoving", "toneMoving", "glueMoving", "heatMoving", "gainMoving", "clipMoving" };

            for (int i = 0; i < axisCount; ++i)
            {
                writer.field (names[i], axes[(size_t) i].value);
                writer.field (moving[i], axes[(size_t) i].moving);
            }

            writer.field ("gTrim", gTrim);
            writer.field ("warm", weights.warm);
            writer.field ("punchy", weights.punchy);
            writer.field ("clean", weights.clean);
            writer.field ("weightsReady", weightsReady);
            writer.field ("deliveryRelief", deliveryRelief);
            const auto model = (int) appliedModel;
            writer.field ("sectionModel", model);
            writer.field ("toneOffset", liveToneOffsetDb);
            writer.field ("heatReduction", liveHeatReduction);
            writer.field ("glueReduction", liveGlueReduction);
            writer.field ("levelWithdrawn", levelWithdrawn);
            writer.field ("guardReleased", guardReleased);
            const auto lo = (std::uint32_t) ((std::uint64_t) processedSamples & 0xffffffffu);
            const auto hi = (std::uint32_t) ((std::uint64_t) processedSamples >> 32);
            writer.field ("samplesLo", lo);
            writer.field ("samplesHi", hi);

            if (auto budget = guard.createStateXml())
                body->addChildElement (budget.release());

            if (auto contrastXml = contrast.createStateXml())
                body->addChildElement (contrastXml.release());

            if (auto psrXml = psr.createStateXml())
                body->addChildElement (psrXml.release());

            if (auto level = outputLevel.createStateXml())
                body->addChildElement (level.release());

            auto* clay = body->createNewChildElement ("CLAY");
            xmlio::Writer clayWriter (*clay);
            clayWriter.field ("lowCutHz", liveClay.lowCutHz);
            clayWriter.field ("monoHz", liveClay.monoHz);
            clayWriter.field ("monoAmount", liveClay.monoAmount);
            clayWriter.field ("widthDb", liveClay.widthDb);
            clayWriter.field ("costMask", liveClay.costLimitedMask);
            const auto guardState = clayGuard.getState();
            clayWriter.field ("phaseFloor", guardState.phaseFloor);
            clayWriter.field ("phaseHold", guardState.phaseHoldSeconds);
            clayWriter.field ("widthCap", guardState.widthCapDb);
            clayWriter.field ("phaseActive", guardState.phaseActive);
            clayWriter.field ("widthActive", guardState.widthActive);
            clayWriter.field ("scale", clayScale);
            clayWriter.field ("scaleTarget", clayScaleTarget);
            return body;
        }

        bool readControl (const juce::XmlElement& control) noexcept
        {
            xmlio::Reader reader (control);
            const char* names[axisCount] = { "recover", "tame", "tone", "glue", "heat", "gain", "clip" };
            const char* moving[axisCount] = { "recoverMoving", "tameMoving", "toneMoving", "glueMoving", "heatMoving", "gainMoving", "clipMoving" };
            int model = (int) MainSectionModel::loudness;
            std::uint32_t lo = 0, hi = 0;

            for (int i = 0; i < axisCount; ++i)
            {
                reader.field (names[i], axes[(size_t) i].value);
                reader.field (moving[i], axes[(size_t) i].moving);
            }

            reader.field ("gTrim", gTrim);
            reader.field ("warm", weights.warm);
            reader.field ("punchy", weights.punchy);
            reader.field ("clean", weights.clean);
            reader.field ("weightsReady", weightsReady);
            reader.field ("deliveryRelief", deliveryRelief);
            reader.field ("sectionModel", model);
            reader.field ("toneOffset", liveToneOffsetDb);
            reader.field ("heatReduction", liveHeatReduction);
            reader.field ("glueReduction", liveGlueReduction);
            reader.field ("levelWithdrawn", levelWithdrawn);
            reader.field ("guardReleased", guardReleased);
            reader.field ("samplesLo", lo);
            reader.field ("samplesHi", hi);

            if (! reader.ok)
                return false;

            if (model < 0 || model > (int) MainSectionModel::passive)
                model = (int) MainSectionModel::loudness;

            appliedModel = (MainSectionModel) model;
            processedSamples = (std::int64_t) ((std::uint64_t) lo | ((std::uint64_t) hi << 32));
            const auto grid = juce::jmax (std::int64_t { 1 }, (std::int64_t) std::llround (sampleRate));
            plannedGridIndex = processedSamples / grid;
            needsPlan = true;
            gTrim = juce::jlimit (-trimClampDb, trimClampDb, gTrim);
            deliveryRelief = juce::jmin (0.0f, deliveryRelief);
            return true;
        }

        bool readClay (const juce::XmlElement& clay) noexcept
        {
            xmlio::Reader reader (clay);
            ClayPlan restored;
            ClayGuardState state;
            reader.field ("lowCutHz", restored.lowCutHz);
            reader.field ("monoHz", restored.monoHz);
            reader.field ("monoAmount", restored.monoAmount);
            reader.field ("widthDb", restored.widthDb);
            reader.field ("costMask", restored.costLimitedMask);
            reader.field ("phaseFloor", state.phaseFloor);
            reader.field ("phaseHold", state.phaseHoldSeconds);
            reader.field ("widthCap", state.widthCapDb);
            reader.field ("phaseActive", state.phaseActive);
            reader.field ("widthActive", state.widthActive);
            reader.field ("scale", clayScale);
            reader.field ("scaleTarget", clayScaleTarget);

            if (! reader.ok)
                return false;

            liveClay = sanitizeClay (restored);
            clayGuard.setState (state);
            clayScale = juce::jlimit (0.0f, 1.0f, isFinite (clayScale) ? clayScale : 0.0f);
            clayScaleTarget = juce::jlimit (0.0f, 1.0f, isFinite (clayScaleTarget) ? clayScaleTarget : clayScale);
            clayFromRestore = true;
            return true;
        }

        bool restoreBody (const juce::XmlElement& body) noexcept
        {
            const auto* profile = body.getChildByName ("LIVE_PROFILE");
            const auto* control = body.getChildByName ("CONTROL");
            const auto* budget = body.getChildByName ("BUDGET_GUARD");
            const auto* contrastXml = body.getChildByName ("CONTRAST_GUARD");
            const auto* psrXml = body.getChildByName ("PSR_GUARD");
            const auto* level = body.getChildByName ("OUTPUT_LEVEL");
            const auto* clay = body.getChildByName ("CLAY");

            if (profile == nullptr || control == nullptr || budget == nullptr || contrastXml == nullptr || psrXml == nullptr
                || level == nullptr || clay == nullptr)
                return false;

            if (! liveProfile.restoreStateXml (*profile) || ! readControl (*control) || ! guard.restoreStateXml (*budget)
                || ! contrast.restoreStateXml (*contrastXml) || ! psr.restoreStateXml (*psrXml) || ! outputLevel.restoreStateXml (*level)
                || ! readClay (*clay))
                return false;

            return true;
        }

        bool restoreCompressed (const juce::XmlElement& live) noexcept
        {
            if (live.getIntAttribute ("formatVersion", 0) != liveWrapperFormat || live.getIntAttribute ("compressed", 0) != 1)
                return false;

            juce::MemoryOutputStream decoded;

            if (! juce::Base64::convertFromBase64 (decoded, live.getAllSubText().trim()))
                return false;

            juce::MemoryInputStream input (decoded.getData(), decoded.getDataSize(), false);
            juce::GZIPDecompressorInputStream gzip (&input, false, juce::GZIPDecompressorInputStream::zlibFormat);
            juce::MemoryOutputStream plain;
            plain.writeFromInputStream (gzip, -1);
            const auto parsed = juce::XmlDocument::parse (plain.toString());
            return parsed != nullptr && parsed->hasTagName ("LIVE_BODY") && restoreBody (*parsed);
        }

        void coldRuntime() noexcept
        {
            if (prepared)
                liveProfile.reset();

            guard.clear();
            contrast.clear();
            psr.clear();
            outputLevel.clear();
            clayGuard.reset();
            pressure.reset();
            clayKeep.reset();
            resetControl();
        }

        void syncSectionModel() noexcept
        {
            if (! prepared || pendingModel == appliedModel)
                return;

            liveProfile.setConstants (LiveConstants {}, pendingModel);
            appliedModel = pendingModel;
        }

        bool applyOuter (const juce::XmlElement& xml) noexcept
        {
            if (! xml.hasTagName (stateTag))
            {
                coldRuntime();

                if (xml.hasTagName (profileTag))
                    legacyDiscarded.store (true, std::memory_order_relaxed);

                return false;
            }

            const auto version = xml.getIntAttribute ("formatVersion", 0);

            if (version > liveArchiveFormatVersion)
            {
                coldRuntime();
                return false;
            }

            if (version == liveArchiveFormatVersion)
            {
                if (const auto* live = xml.getChildByName ("LIVE"))
                {
                    if (restoreCompressed (*live))
                    {
                        syncSectionModel();
                        return true;
                    }
                }
            }

            if (looksLegacy (xml))
                legacyDiscarded.store (true, std::memory_order_relaxed);

            coldRuntime();
            return false;
        }
    };

    static_assert (std::is_trivially_copyable<Status>::value, "状态快照要能按字拷贝");

    //==============================================================================
    GlazeController::GlazeController() : impl (std::make_unique<Impl>()) {}
    GlazeController::~GlazeController() = default;

    void GlazeController::prepare (const juce::dsp::ProcessSpec& spec)
    {
        auto& s = *impl;
        std::unique_ptr<juce::XmlElement> carry;

        if (s.restorePending)
        {
            carry = std::move (s.stashed);
            s.restorePending = false;
        }
        else if (s.prepared)
        {
            carry = createStateXml();
        }

        if (spec.sampleRate > 0.0 && std::isfinite (spec.sampleRate))
            s.sampleRate = spec.sampleRate;

        const auto channels = (int) spec.numChannels;
        const auto same = s.analyzerReady && s.analyzedChannels == channels && s.analyzedSampleRate == s.sampleRate;

        if (! same)
        {
            s.analyzer.prepare (spec);
            s.analyzerReady = true;
            s.analyzedChannels = channels;
            s.analyzedSampleRate = s.sampleRate;
        }

        s.analyzer.setHouseConstants (s.constantsInUse().house);
        s.positionTolerance = juce::jmax (std::int64_t { 2 }, (std::int64_t) std::llround (0.05 * s.sampleRate));
        s.liveProfile.prepare (s.sampleRate);
        s.guard.prepare (s.sampleRate);
        s.contrast.prepare (s.sampleRate);
        s.psr.prepare (s.sampleRate);
        s.outputLevel.prepare (s.sampleRate);
        s.clayGuard.reset();
        s.pressure.reset();
        s.clayKeep.reset();
        s.resetControl();
        s.expectKnown = false;
        s.prepared = true;

        if (carry != nullptr)
            s.applyOuter (*carry);

        s.syncSectionModel();

        if (same)
            s.analyzer.handleDiscontinuity (Discontinuity::hostReset);

        if (! s.analyzer.isLiveMode())
        {
            s.analyzer.setSegmentSink (&s.liveProfile);
            s.analyzer.setLiveMode (true);
        }

        s.publish();
    }

    void GlazeController::setHostInfo (const HostInfo& info) noexcept
    {
        impl->host = info;
        impl->hostFresh = true;
        impl->hostInputOffset = 0;
        impl->offline = info.offline;
        impl->noteTransport();
    }

    void GlazeController::hostReset() noexcept
    {
        impl->onDiscontinuity (Discontinuity::hostReset);
        impl->hostInputOffset = 0;
        impl->expectKnown = false;
    }

    int GlazeController::getGuardSamplesUntilBoundary() const noexcept
    {
        return impl->guard.getSamplesUntilSlotBoundary();
    }

    void GlazeController::handleDiscontinuity (Discontinuity kind) noexcept
    {
        impl->onDiscontinuity (kind);
    }

    const ChainParams& GlazeController::beginBlock (const ChainParams& newManual, bool hostBypassed) noexcept
    {
        auto& s = *impl;
        const auto knobsChanged = s.haveManual && ! planningKnobsEqual (s.manual, newManual);
        s.manual = newManual;
        s.haveManual = true;
        s.paused = hostBypassed;
        s.style = styleFromIndex (newManual.glazeStyle);
        s.genre = genreFromIndex (newManual.glazeGenre);
        s.delivery = deliveryFromIndex (newManual.glazeDelivery);
        s.pendingModel = mainSectionModelFor (s.genre);
        s.active = newManual.glazePercent > 0.0f;

        if (knobsChanged)
            s.needsPlan = true;

        if (! s.prepared)
        {
            s.effective = newManual;
            s.effective.clay = resolveClayParams (newManual.claySettings, ClayPlan {}, newManual.glazePercent);
            return s.effective;
        }

        s.analyzer.setGenre (s.genre);

        if (! s.active)
        {
            s.snapRelief();
            s.effective = newManual;
            s.updateClay();
            s.publish();
            return s.effective;
        }

        s.effective = applyIncrements (newManual, s.appliedIncrements());
        s.applySpectrumFollow();
        s.applyLimitExtras();
        s.updateClay();
        s.publish();
        return s.effective;
    }

    void GlazeController::analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept
    {
        if (! impl->prepared)
            return;

        auto analysisHost = impl->host;

        if (impl->offline)
            analysisHost.isPlaying = true;

        if (analysisHost.hasPosition && impl->hostPlaying())
            analysisHost.positionSamples += impl->hostInputOffset;

        if (impl->analyzer.isLiveMode() && impl->hostInputOffset > 0 && impl->hostPlaying() && analysisHost.hasPpq && analysisHost.hasTempo
            && std::isfinite (analysisHost.bpm) && analysisHost.bpm > 0.0)
            analysisHost.ppqPosition += (double) impl->hostInputOffset * analysisHost.bpm / (60.0 * impl->sampleRate);

        impl->analyzer.setHostInfo (analysisHost);
        impl->analyzer.processInput (input, inputMeter);
        impl->hostInputOffset += input.getNumSamples();
        impl->blockMusic = impl->analyzer.getShortTermView().musicActive;
    }

    void GlazeController::analyzeSpectrum (const BalanceModule& balance) noexcept
    {
        if (! impl->prepared)
            return;

        impl->analyzer.processSpectrum (balance);
    }

    void GlazeController::noteOutput (const juce::AudioBuffer<float>& rendered) noexcept
    {
        if (! impl->prepared)
            return;

        impl->currentMainness = impl->readMainness();
        impl->outputLevel.noteMainness (impl->currentMainness);
        impl->outputLevel.processOutput (rendered);
    }

    void GlazeController::endBlock (const BalanceModule& balance, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                                    const ModuleStats& stats, int chainLatencySamples, bool spectrumDeferred) noexcept
    {
        (void) chainLatencySamples;
        auto& s = *impl;

        if (! spectrumDeferred && s.prepared)
            s.analyzer.processSpectrum (balance);

        const auto numSamples = juce::jmax (0, stats.numSamples);
        s.rememberHost (numSamples);

        if (! s.prepared)
            return;

        const auto dt = s.sampleRate > 0.0 ? (double) numSamples / s.sampleRate : 0.0;
        const auto music = s.blockMusic && s.hostPlaying();
        const auto running = s.active && ! s.paused;
        const auto grid = juce::jmax (std::int64_t { 1 }, (std::int64_t) std::llround (s.sampleRate));

        if (s.processedSamples / grid != s.plannedGridIndex)
            s.needsPlan = true;

        if (running)
        {
            s.followWeights (music, dt);

            if (s.needsPlan)
            {
                s.makePlanNow();
                s.needsPlan = false;
                s.plannedGridIndex = s.processedSamples / grid;
            }

            if (music)
                s.slewToward (s.plan.increments, dt, s.currentMainness < 0.5f);

            auto local = stats;
            local.musicActive = music;
            s.syncGuardLimits();
            s.budgetOut = s.guard.process (local);
            s.contrastOut = s.contrast.process (inputMeter.getShortTermLufs(), outputMeter.getShortTermLufs(), music, numSamples,
                                                s.delivery == Delivery::club);
            const auto& psrReading = s.analyzer.getShortTermView().psr3sDb;
            s.psrOut = s.psr.process (psrReading.value, psrReading.valid, music, numSamples, s.delivery);
            s.updateDelivery (music, stats.outputTruePeakDbtp, s.manual.limit.ceilingDb, dt);
            s.rememberMinRelief();
            s.updateTrim (! music || s.guard.isRetreating(), dt);
            s.updateLiveTone (dt);
            s.updateLiveColour (dt);
        }
        else
        {
            s.guard.discardPartialSlot();
        }

        const auto clayMusic = music && ! s.paused;
        s.claySeconds = clayMusic ? dt : 0.0;

        ClayPressureMonitor::PeakBlocks peaks;
        peaks.pairs = stats.clayPeakBlocks;
        peaks.count = stats.clayPeakBlockCount;
        peaks.dropped = stats.clayPeakBlocksDropped;

        if (s.pressure.update (peaks, clayMusic, clayMusic ? dt : 0.0, stats.clayBlendIntendedEnergy, stats.clayBlendOutputEnergy,
                               clayMusic ? stats.clayBlendSeconds : 0.0))
            s.clayScaleTarget = ClayPressureMonitor::nextScale (s.clayScaleTarget);

        if (clayMusic)
        {
            const auto step = clayScalePerSecond * (float) dt;
            s.clayScale = juce::jlimit (0.0f, 1.0f, s.clayScale + juce::jlimit (-step, step, s.clayScaleTarget - s.clayScale));
        }

        s.clayKeep.update (stats.clayGuardChangeEnergy, stats.clayGuardKeptEnergy, dt, s.sampleRate);
        s.processedSamples += (std::int64_t) numSamples;
        s.publish();
    }

    std::unique_ptr<juce::XmlElement> GlazeController::createStateXml() const
    {
        if (impl->restorePending && impl->stashed != nullptr)
            return std::make_unique<juce::XmlElement> (*impl->stashed);

        auto xml = std::make_unique<juce::XmlElement> (stateTag);
        xml->setAttribute ("formatVersion", liveArchiveFormatVersion);
        auto* live = xml->createNewChildElement ("LIVE");
        live->setAttribute ("formatVersion", liveWrapperFormat);
        live->setAttribute ("compressed", 1);
        live->setAttribute ("constantsVersion", factoryConstants().version);

        const auto text = impl->createLiveBody()->toString();
        juce::MemoryOutputStream compressed;

        {
            juce::GZIPCompressorOutputStream gzip (compressed, -1, 0);
            gzip.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
            gzip.flush();
        }

        live->addTextElement (juce::Base64::toBase64 (compressed.getData(), compressed.getDataSize()));
        return xml;
    }

    void GlazeController::restoreStateXml (const juce::XmlElement* xml)
    {
        if (! impl->prepared)
        {
            impl->stashed = xml != nullptr ? std::make_unique<juce::XmlElement> (*xml) : nullptr;
            impl->restorePending = true;
            return;
        }

        if (xml == nullptr)
        {
            impl->coldRuntime();
            impl->publish();
            return;
        }

        impl->applyOuter (*xml);
        impl->publish();
    }

    Status GlazeController::getStatus() const noexcept
    {
        Status status {};

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto before = impl->statusSeq.load (std::memory_order_acquire);

            if ((before & 1u) != 0u)
                continue;

            std::uint32_t raw[(size_t) Impl::statusWords];

            for (int i = 0; i < Impl::statusWords; ++i)
                raw[(size_t) i] = impl->statusWord[(size_t) i].load (std::memory_order_relaxed);

            const auto after = impl->statusSeq.load (std::memory_order_acquire);

            if (before == after)
            {
                std::memcpy (&status, raw, sizeof (Status));
                break;
            }
        }

        if (impl->legacyDiscarded.load (std::memory_order_relaxed))
            status.legacyStateDiscarded = true;

        return status;
    }

    const LiveProfile& GlazeController::getLiveProfile() const noexcept
    {
        return impl->liveProfile;
    }

    void GlazeController::setConstantsForTesting (const Constants& constants)
    {
        impl->overrideConstants = constants;
        impl->hasOverride = true;

        if (impl->analyzerReady)
            impl->analyzer.setHouseConstants (constants.house);
    }
}
