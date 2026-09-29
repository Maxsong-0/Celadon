/*
  ==============================================================================

    PluginProcessor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/HelpText.h"
#include "dsp/GlazePlanner.h"
#include "dsp/GlazeProfile.h"

#include <cmath>
#include <iterator>

namespace
{
    bool containsNonFinite (const juce::AudioBuffer<float>& buffer) noexcept
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer (ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (data[i]))
                    return true;
        }

        return false;
    }

    // clay_*_mode 的原始值（选项下标的 float）→ ClayMode。取整故意用 juce::roundToInt（同 Balance Speed 的说明：
    // AudioParameterChoice::getIndex 用的就是它，这里读到的下标与参数对象、界面附件看到的一致），再钳到 自动 ～ 手动 防越界——
    // 枚举值与选项顺序一一对应（ChainParams.h：automatic = 0, off = 1, manual = 2；Parameters.cpp 的选项顺序同此）。
    celadon::ClayMode clayModeFromRawValue (float rawValue) noexcept
    {
        constexpr auto first = static_cast<int> (celadon::ClayMode::automatic);
        constexpr auto last  = static_cast<int> (celadon::ClayMode::manual);
        return static_cast<celadon::ClayMode> (juce::jlimit (first, last, juce::roundToInt (rawValue)));
    }
}

//==============================================================================
CeladonAudioProcessor::CeladonAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                       .withInput  ("Reference", juce::AudioChannelSet::stereo(), false)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#else
     :
#endif
       parameters (*this, nullptr, "CeladonParameters", celadon::param::createParameterLayout())
{
    using namespace celadon::param;

    limitParams.gain        = parameters.getRawParameterValue (limitGain);
    limitParams.ceiling     = parameters.getRawParameterValue (limitCeiling);
    limitParams.release     = parameters.getRawParameterValue (limitRelease);
    limitParams.releaseAuto = parameters.getRawParameterValue (limitReleaseAuto);
    limitParams.lookahead   = parameters.getRawParameterValue (limitLookahead);
    limitParams.clip        = parameters.getRawParameterValue (limitClip);
    limitParams.listen      = parameters.getRawParameterValue (limitListen);
    limitParams.bypass      = parameters.getRawParameterValue (limitBypass);

    colorParams.tone        = parameters.getRawParameterValue (colorTone);
    colorParams.glue        = parameters.getRawParameterValue (colorGlue);
    colorParams.heat        = parameters.getRawParameterValue (colorHeat);
    colorParams.mix         = parameters.getRawParameterValue (colorMix);
    colorParams.output      = parameters.getRawParameterValue (colorOutput);
    colorParams.bypass      = parameters.getRawParameterValue (colorBypass);

    // balance_speed 是 AudioParameterChoice：APVTS 给的原始值是选项下标（0 Slow / 1 Mid / 2 Fast）的 float，
    // 在 readParams 里取整（与 AudioParameterChoice::getIndex 同一取整）再钳到 0..2 转成 BalanceModule::Speed。
    balanceParams.recover   = parameters.getRawParameterValue (balanceRecover);
    balanceParams.tame      = parameters.getRawParameterValue (balanceTame);
    balanceParams.brighten  = parameters.getRawParameterValue (balanceBrighten);
    balanceParams.speed     = parameters.getRawParameterValue (balanceSpeed);
    balanceParams.bypass    = parameters.getRawParameterValue (balanceBypass);

    glazeAmount             = parameters.getRawParameterValue (celadon::param::glazeAmount);
    glazeStyle              = parameters.getRawParameterValue (celadon::param::glazeStyle);

    // Clay 修坯（v0.1.7.0）：三个 *_mode 是 AudioParameterChoice，原始值是选项下标（0 自动 / 1 关 / 2 手动）的 float，
    // 在 readParams 里按 clayModeFromRawValue 取整、钳到 0..2 转成 celadon::ClayMode。
    clayParams.lowCutMode   = parameters.getRawParameterValue (clayLowCutMode);
    clayParams.lowCutHz     = parameters.getRawParameterValue (clayLowCutHz);
    clayParams.monoMode     = parameters.getRawParameterValue (clayMonoMode);
    clayParams.monoHz       = parameters.getRawParameterValue (clayMonoHz);
    clayParams.widthMode    = parameters.getRawParameterValue (clayWidthMode);
    clayParams.widthDb      = parameters.getRawParameterValue (clayWidthDb);
    clayParams.bypass       = parameters.getRawParameterValue (clayBypass);

    masteringParams.harshMode = parameters.getRawParameterValue (masterHarshMode);
    masteringParams.harshAmount = parameters.getRawParameterValue (masterHarshAmount);
    masteringParams.transient = parameters.getRawParameterValue (masterTransient);
    masteringParams.targetMode = parameters.getRawParameterValue (masterTargetMode);
    masteringParams.targetAmount = parameters.getRawParameterValue (masterTargetAmount);
    masteringParams.codecMarginDb = parameters.getRawParameterValue (masterCodecMarginDb);
    masteringParams.dither = parameters.getRawParameterValue (masterDither);
    masteringParams.previewLufs = parameters.getRawParameterValue (masterPreviewLufs);

    // 任何一个 ID 拼错都会拿到空指针——开发时立刻在这里断住，而不是等到音频线程崩溃。
    jassert (limitParams.gain != nullptr && limitParams.ceiling != nullptr && limitParams.release != nullptr
             && limitParams.releaseAuto != nullptr && limitParams.lookahead != nullptr && limitParams.clip != nullptr
             && limitParams.listen != nullptr && limitParams.bypass != nullptr);
    jassert (colorParams.tone != nullptr && colorParams.glue != nullptr && colorParams.heat != nullptr
             && colorParams.mix != nullptr && colorParams.output != nullptr && colorParams.bypass != nullptr);
    jassert (balanceParams.recover != nullptr && balanceParams.tame != nullptr && balanceParams.brighten != nullptr
             && balanceParams.speed != nullptr && balanceParams.bypass != nullptr);
    jassert (glazeAmount != nullptr && glazeStyle != nullptr);
    jassert (clayParams.lowCutMode != nullptr && clayParams.lowCutHz != nullptr && clayParams.monoMode != nullptr
             && clayParams.monoHz != nullptr && clayParams.widthMode != nullptr && clayParams.widthDb != nullptr
             && clayParams.bypass != nullptr);

   #if JUCE_DEBUG
    // 每个真正注册的参数都必须有三行提示文案，且手工维护的 allParameterIds 与参数表一致（v0.1.7.0 起 28 个：
    // Limit 8 + Color 6 + Balance 5 + GLAZE 2 + Clay 7）：HelpTextTests（无头测试）只能对照 allParameterIds，这里在 Debug 版
    // 补上对真正参数表的检查。
    // 注意：本类的 getParameters() 返回 APVTS，会遮住基类同名函数，这里要显式调基类版本。
    for (auto* parameter : juce::AudioProcessor::getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            jassert (celadon::help::find (withId->paramID) != nullptr);

    jassert ((size_t) juce::AudioProcessor::getParameters().size() == std::size (celadon::param::allParameterIds));

    // GLAZE 决策器不依赖 APVTS：自动增量叠加手动值后按参数范围钳制（applyIncrements，范围写在 GlazePlanner.cpp）。
    // 逐项核对它钳到的上下限与真正的参数表一致：手动值在下限、增量极大 → 结果必须正好是上限；反之亦然。
    // 以后改了 Parameters.cpp 的范围却忘了改决策器，Debug 版一启动就断在这里（取代 v0.4 对 GlazeMacro 偏移表的核对）。
    {
        using Field = float& (*) (celadon::ChainParams&);
        using Increment = float& (*) (celadon::glaze::AutoIncrements&);
        struct Glazed { const char* id; Field field; Increment increment; };

        const Glazed glazed[] =
        {
            { celadon::param::balanceRecover, [] (celadon::ChainParams& p) -> float& { return p.balance.recover; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.balanceRecover; } },
            { celadon::param::balanceTame,    [] (celadon::ChainParams& p) -> float& { return p.balance.tame; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.balanceTame; } },
            { celadon::param::colorTone,      [] (celadon::ChainParams& p) -> float& { return p.color.toneDb; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.colorToneDb; } },
            { celadon::param::colorGlue,      [] (celadon::ChainParams& p) -> float& { return p.color.glue; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.colorGlue; } },
            { celadon::param::colorHeat,      [] (celadon::ChainParams& p) -> float& { return p.color.heat; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.colorHeat; } },
            { celadon::param::limitGain,      [] (celadon::ChainParams& p) -> float& { return p.limit.gainDb; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.limitGainDb; } },
            { celadon::param::limitClip,      [] (celadon::ChainParams& p) -> float& { return p.limit.clip; },
                                              [] (celadon::glaze::AutoIncrements& i) -> float& { return i.limitClip; } },
        };

        static_assert (sizeof (celadon::glaze::AutoIncrements) == std::size (glazed) * sizeof (float),
                       "glaze::AutoIncrements 的项数变了：同步改这里的范围核对");

        for (const auto& item : glazed)
        {
            auto* ranged = parameters.getParameter (item.id);
            jassert (ranged != nullptr);

            if (ranged == nullptr)
                continue;

            const auto& range = ranged->getNormalisableRange();

            celadon::ChainParams atStart, atEnd;
            celadon::glaze::AutoIncrements up, down;
            item.field (atStart) = range.start;
            item.increment (up) = 1.0e6f;
            item.field (atEnd) = range.end;
            item.increment (down) = -1.0e6f;

            auto raised = celadon::glaze::applyIncrements (atStart, up);
            auto lowered = celadon::glaze::applyIncrements (atEnd, down);
            jassert (juce::approximatelyEqual (item.field (raised), range.end));
            jassert (juce::approximatelyEqual (item.field (lowered), range.start));
        }
    }

    // GLAZE 旋钮自己的范围：决策器按 0 ～ 100 % 解释（> 100 按 100、≤ 0 不起作用）。
    if (auto* glazeParameter = parameters.getParameter (celadon::param::glazeAmount))
    {
        const auto& range = glazeParameter->getNormalisableRange();
        jassert (juce::approximatelyEqual (range.start, 0.0f) && juce::approximatelyEqual (range.end, 100.0f));
    }

    // Clay 修坯（v0.1.7.0）：三个手动值的参数范围必须与 ClayModule 自己的钳位常数一致——模块不信任外部值、按自己的常数钳，
    // 两边不一致时旋钮能拧到模块会悄悄钳掉的值（旋钮动了声音不动），或模块允许的值旋钮拧不到。以后改了任一边，Debug 版一启动就断在这里。
    // 三个三态参数的选项必须正好是 ClayMode 的三项（自动 / 关 / 手动），默认「自动」：readParams 按下标转枚举。
    {
        struct ClayRange { const char* id; float start; float end; };

        const ClayRange clayRanges[] =
        {
            { celadon::param::clayLowCutHz, celadon::ClayModule::minLowCutHz, celadon::ClayModule::maxLowCutHz },
            { celadon::param::clayMonoHz,   celadon::ClayModule::minMonoHz,   celadon::ClayModule::maxMonoHz },
            { celadon::param::clayWidthDb,  celadon::ClayModule::minWidthDb,  celadon::ClayModule::maxWidthDb },
        };

        for (const auto& item : clayRanges)
        {
            auto* ranged = parameters.getParameter (item.id);
            jassert (ranged != nullptr);

            if (ranged == nullptr)
                continue;

            const auto& range = ranged->getNormalisableRange();
            jassert (juce::approximatelyEqual (range.start, item.start) && juce::approximatelyEqual (range.end, item.end));
        }

        constexpr auto numClayModes = static_cast<int> (celadon::ClayMode::manual) + 1;

        for (const char* id : { celadon::param::clayLowCutMode, celadon::param::clayMonoMode, celadon::param::clayWidthMode })
        {
            auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameters.getParameter (id));
            jassert (choice != nullptr);

            if (choice == nullptr)
                continue;

            jassert (choice->choices.size() == numClayModes);

            // getDefaultValue 在 AudioParameterChoice 里是 private 的覆盖（JUCE 9.0.2）：经基类调（虚函数照样走到它）。
            const auto& asParameter = static_cast<const juce::RangedAudioParameter&> (*choice);
            jassert (clayModeFromRawValue (choice->convertFrom0to1 (asParameter.getDefaultValue())) == celadon::ClayMode::automatic);
        }
    }
   #endif

    startTimerHz (30);
}

CeladonAudioProcessor::~CeladonAudioProcessor()
{
    stopTimer();
}

//==============================================================================
const juce::String CeladonAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool CeladonAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool CeladonAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool CeladonAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double CeladonAudioProcessor::getTailLengthSeconds() const
{
    // 输入一静音，输出还要再响「链延迟 + 滤波器余振」这么久（v0.5 R10）。v0.4 报 0：JUCE 据此给 VST3 报 kNoTail、
    // 给 AU 报 0 s，按静音暂停插件处理的宿主（Cubase 的「无信号时暂停 VST3 处理」、FL 的 smart disable 等）可能把延迟线里
    // 最后几毫秒截掉，恢复时再吐出陈旧的这一小段。宿主可能在任何线程问：只读两个原子量；还没 prepare 时只报余振部分。
    const auto sampleRate = currentSampleRate.load (std::memory_order_relaxed);
    const auto latencySeconds = sampleRate > 0.0 ? (double) pendingLatency.load (std::memory_order_relaxed) / sampleRate
                                                 : 0.0;
    return latencySeconds + filterRingOutSeconds;
}

int CeladonAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int CeladonAudioProcessor::getCurrentProgram()
{
    return 0;
}

void CeladonAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String CeladonAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void CeladonAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
celadon::CeladonChain::Params CeladonAudioProcessor::readParams() const noexcept
{
    celadon::CeladonChain::Params p;

    // 按信号顺序读：Balance → Clay → Color → Limit。布尔参数在 APVTS 里是 0 / 1 的 float，按 0.5 判。

    // Balance（设计规格-Balance「参数」表）。Speed 是三选一：原始值是选项下标的 float（0 Slow / 1 Mid / 2 Fast），
    // 取整后钳到 0..2 再转枚举——枚举值与选项顺序一一对应（BalanceModule::Speed 定义 slow = 0, mid = 1, fast = 2）。
    // 取整故意用 juce::roundToInt：AudioParameterChoice::getIndex() 用的就是它，这里读到的下标与参数对象、
    // 界面附件看到的完全一致（它对恰好 .5 按奇偶取整，但 APVTS 的原始值已按步长 1 吸附成整数，不会出现 .5）。
    // 钳制是防越界的保险，不信任外部值。
    {
        const auto speedIndex = juce::jlimit (0, 2, juce::roundToInt (balanceParams.speed->load()));

        p.balance.recover  = balanceParams.recover->load();
        p.balance.tame     = balanceParams.tame->load();
        p.balance.brighten = balanceParams.brighten->load();
        p.balance.speed    = static_cast<celadon::BalanceModule::Speed> (speedIndex);
        p.balance.bypass   = balanceParams.bypass->load() >= 0.5f;
    }

    // Clay 修坯（v0.1.7.0，设计规格-Clay 第 5、6 节；ADR-009 三态）。这里只读用户在「高级」抽屉里的设置：三个三态按下标转枚举
    //（clayModeFromRawValue：取整同 Balance Speed、钳到 自动 ～ 手动），三个手动值与 Bypass 原样交出（范围已由参数钳过）。
    // 交给 ClayModule 的实际参数 p.clay 不在这里填：链里的 GLAZE 编排器每块按三态合成——自动 = 决策器方案 + 实时保护
    //（GLAZE 0 时为中性）、关 = 中性、手动 = 这里的手动值（GLAZE 0 也生效）。
    p.claySettings.lowCutMode = clayModeFromRawValue (clayParams.lowCutMode->load());
    p.claySettings.lowCutHz   = clayParams.lowCutHz->load();
    p.claySettings.monoMode   = clayModeFromRawValue (clayParams.monoMode->load());
    p.claySettings.monoHz     = clayParams.monoHz->load();
    p.claySettings.widthMode  = clayModeFromRawValue (clayParams.widthMode->load());
    p.claySettings.widthDb    = clayParams.widthDb->load();
    p.claySettings.bypass     = clayParams.bypass->load() >= 0.5f;

    // Color（设计规格-Color「参数」表）。
    p.color.toneDb     = colorParams.tone->load();
    p.color.glue       = colorParams.glue->load();
    p.color.heat       = colorParams.heat->load();
    p.color.mixPercent = colorParams.mix->load();
    p.color.outputDb   = colorParams.output->load();
    p.color.bypass     = colorParams.bypass->load() >= 0.5f;

    // Limit（设计规格-Limit「参数」表）。
    p.limit.gainDb      = limitParams.gain->load();
    p.limit.ceilingDb   = limitParams.ceiling->load();
    p.limit.releaseMs   = limitParams.release->load();
    p.limit.autoRelease = limitParams.releaseAuto->load() >= 0.5f;
    p.limit.lookaheadMs = limitParams.lookahead->load();
    p.limit.clip        = limitParams.clip->load();
    p.limit.listen      = limitParams.listen->load() >= 0.5f;
    p.limit.bypass      = limitParams.bypass->load() >= 0.5f;

    // GLAZE（ADR-008）：这里只读原始值；自动增量由链里的 GLAZE 编排器在 CeladonChain::setParams 里算。
    p.glazePercent = glazeAmount->load();

    // GLAZE 风格（v0.6，ADR-008）：四选一，原始值是选项下标的 float，取整方式同 Balance Speed，钳到 0..3（= glaze::Style）。
    p.glazeStyle = juce::jlimit (0, 3, juce::roundToInt (glazeStyle->load()));

    const auto choice = [] (float value, int maximum) noexcept { return std::isfinite (value) ? juce::jlimit (0, maximum, juce::roundToInt (value)) : 0; };
    p.mastering.harshMode = static_cast<celadon::HarshMode> (choice (masteringParams.harshMode->load(), 2));
    p.mastering.harshAmount = masteringParams.harshAmount->load();
    p.mastering.transientPreserve = masteringParams.transient->load();
    p.mastering.targetMode = static_cast<celadon::TargetMode> (choice (masteringParams.targetMode->load(), 5));
    p.mastering.targetAmount = masteringParams.targetAmount->load();
    p.mastering.codecMarginDb = masteringParams.codecMarginDb->load();
    p.mastering.dither = static_cast<celadon::DitherMode> (choice (masteringParams.dither->load(), 2));
    p.mastering.previewTargetLufs = masteringParams.previewLufs->load();
    return p;
}

void CeladonAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::dsp::ProcessSpec spec { sampleRate,
                                        (juce::uint32) juce::jmax (1, samplesPerBlock),
                                        (juce::uint32) juce::jmax (1, getTotalNumOutputChannels()) };

    // 界面把延迟换算成毫秒要用采样率；AudioProcessor::getSampleRate() 不是原子的，自己存一份。
    currentSampleRate.store (sampleRate, std::memory_order_relaxed);

    // 规格（采样率 / 块大小 / 声道数）没变就不整体重建（v0.5 研究 P5 R8）。宿主每次重启都会调 prepareToPlay：
    // 插入、VST3 收到延迟变化后 setActive(false / true)、FL 的 smart disable 恢复……整体重建要 1.5 ～ 3.7 ms
    // （Heat 补偿表标定、Balance 的 FFT / 窗 / 段权重），FL 下这段时间音频线程被挡住。同规格时 FFT、过采样滤波器、
    // 补偿表都与上次一模一样，只需把状态清回刚 prepare 完的样子——各模块的 prepare 本来就是「按规格分配 + reset()」，
    // 所以 setParams + reset 与重新 prepare 之后的输出逐位相同（见 IMPL-I3 的等价性实测）。
    const auto fullRebuild = ! preparedSpec.has_value() || *preparedSpec != spec;

    // Lookahead：整体重建时直接采用当前参数值；同规格重启时保留正在用的值（见 LookaheadHold）。
    auto params = readParams();
    lookaheadHold.prepare (params.limit.lookaheadMs, juce::roundToInt (lookaheadSettleSeconds * sampleRate), fullRebuild);
    params.limit.lookaheadMs = lookaheadHold.appliedMs;

    // 先把当前参数交给链再 prepare / reset：reset 会让各模块的平滑器直接落到这份参数，不从默认值滑过来。
    // 宿主旁通此刻未知，按不旁通准备；真旁通时第一块会交叉淡化过去。界面的实际值标记也先按这份参数给（不等第一块音频）。
    chain.setParams (params, false);
    publishEffectiveForUi (params);

    if (fullRebuild)
    {
        chain.prepare (spec);   // 含两块表与 GLAZE 编排器（v0.6 挪进链里）；编排器保留已提交的画像与方案
        preparedSpec = spec;
    }
    else
    {
        // 两块表不动（chain.reset 不清表）：它们只观察信号，宿主重启不该把整体响度与峰值读数清掉（v0.4 每次重启都清，
        // 拖一次 Lookahead 引起的 VST3 重启就会把 Integrated LUFS 归零）。要清由用户点 Reset。
        chain.reset();
    }

    // prepareToPlay 不在音频线程，可以直接上报；同时写 pendingLatency，免得定时器又报一次旧值。
    hostResetPending.store (false);
    lastBlockHostBypassed = false;
    blocksSinceHostBypassChange = hostBypassResetGraceBlocks;
    pendingLatency.store (chain.getLatencySamples(), std::memory_order_relaxed);
    setLatencySamples (chain.getLatencySamples());
}

//==============================================================================
void CeladonAudioProcessor::LookaheadHold::prepare (float requestedMs, int newSettleSamples, bool fullRebuild) noexcept
{
    settleSamples = juce::jmax (0, newSettleSamples);

    if (! initialised || fullRebuild)
    {
        // 参数值非有限（只有宿主写坏参数才会出现）时沿用正在用的值；第一次就坏则用参数默认值。
        const auto start = std::isfinite (requestedMs) ? requestedMs
                                                       : (initialised ? appliedMs : celadon::LimiterModule::Params {}.lookaheadMs);
        appliedMs = candidateMs = start;
        stableSamples = samplesSinceCommit = settleSamples;   // 视为早已停稳、早已采用
        commitOnFirstBlock = true;
        initialised = true;
        return;
    }

    stableSamples      = juce::jmin (stableSamples, settleSamples);
    samplesSinceCommit = juce::jmin (samplesSinceCommit, settleSamples);
    commitOnFirstBlock = true;   // 链刚 reset，第一块换前瞻听不到（见头文件；仍受 samplesSinceCommit 约束）
}

float CeladonAudioProcessor::LookaheadHold::next (float requestedMs, int numSamples, bool chainRestarting) noexcept
{
    // 非有限值当作「没变」：链永远拿不到 NaN 前瞻。APVTS 的原始值已按 0.5 ms 步进吸附，可以直接比相等。
    const auto target = std::isfinite (requestedMs) ? requestedMs : candidateMs;

    if (! juce::exactlyEqual (target, candidateMs))
    {
        candidateMs = target;
        stableSamples = 0;
    }

    if (! juce::exactlyEqual (appliedMs, candidateMs))
    {
        const auto settled = stableSamples >= settleSamples;
        const auto early   = (chainRestarting || commitOnFirstBlock) && samplesSinceCommit >= settleSamples;

        if (settled || early)
        {
            appliedMs = candidateMs;
            samplesSinceCommit = 0;
        }
    }

    if (numSamples > 0)
        commitOnFirstBlock = false;

    // 两个计数到门槛就封顶：不会溢出，门槛以上的值也没有意义。
    const auto advance = [this, numSamples] (int count) noexcept
    {
        return (int) juce::jmin ((juce::int64) settleSamples, (juce::int64) count + (juce::int64) juce::jmax (0, numSamples));
    };

    stableSamples      = advance (stableSamples);
    samplesSinceCommit = advance (samplesSinceCommit);
    return appliedMs;
}

void CeladonAudioProcessor::timerCallback()
{
    // pendingLatency 是链此刻真正的延迟；Lookahead 在处理器层稳住以后，它只在「停稳后换前瞻」那一刻变，
    // 所以拖一次旋钮这里只报一次（v0.4 是每档一次）。
    const auto latency = pendingLatency.load (std::memory_order_relaxed);

    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

void CeladonAudioProcessor::reset()
{
    hostResetPending.store (true);
}

void CeladonAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool CeladonAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // 只做 1 进 1 出与 2 进 2 出（设计规格-总体「错误处理与安全」）。
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    if (layouts.inputBuses.size() > 1)
    {
        const auto sidechain = layouts.inputBuses[1];
        if (! sidechain.isDisabled() && sidechain != juce::AudioChannelSet::mono()
                                    && sidechain != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
  #endif
}
#endif

void CeladonAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    processBuses (buffer, false);
}

void CeladonAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    processBuses (buffer, true);
}

void CeladonAudioProcessor::processBuses (juce::AudioBuffer<float>& buffer, bool hostBypassed)
{
    juce::ScopedNoDenormals noDenormals;
    // Read the actual optional input bus before touching the main output. Never learn the processed master.
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        auto sidechain = getBusBuffer (buffer, true, 1);
        chain.processReference (sidechain, true);
    }
    else
    {
        juce::AudioBuffer<float> disconnected (buffer.getArrayOfWritePointers(), 0, buffer.getNumSamples());
        chain.processReference (disconnected, false);
    }
    auto main = getBusBuffer (buffer, false, 0);
    processChain (main, hostBypassed);
}

void CeladonAudioProcessor::processChain (juce::AudioBuffer<float>& buffer, bool hostBypassed)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels  = getMainBusNumInputChannels();
    const auto totalNumOutputChannels = getMainBusNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // 宿主 Reset：清空整条链并淡入。例外：JUCE 9.0.2 的 AU 包装器在切换宿主旁通时也会调 Reset
    // （juce_audio_plugin_client_AU_1.mm 的 kAudioUnitProperty_BypassEffect 分支：代码是在「打开」旁通时调，
    // 旁边注释却写 off——两个方向都按「切换时可能来 Reset」处理）。本插件旁通期间整条链照常在跑、状态是新的，
    // 这时重置只会在切换瞬间把正在播的信号硬切成一段静音（咔哒 + 断音），所以旁通状态变化后 2 个块内的 Reset 忽略；
    // 多给 1 个块是防 Reset 与旁通状态分别在两个块里才被看到。
    if (hostBypassed != lastBlockHostBypassed)
        blocksSinceHostBypassChange = 0;
    else if (blocksSinceHostBypassChange < hostBypassResetGraceBlocks)
        ++blocksSinceHostBypassChange;

    const auto restartForHostReset = hostResetPending.exchange (false)
                                      && blocksSinceHostBypassChange >= hostBypassResetGraceBlocks;

    lastBlockHostBypassed = hostBypassed;

    // 输入有 NaN / Inf：本块置零、计数，照常往下走——时间线不断，延迟线里的尾巴自然输出完。
    if (containsNonFinite (buffer))
    {
        buffer.clear();
        nonFiniteBlocks.fetch_add (1, std::memory_order_relaxed);
    }

    // 频谱仪的 IN：链入口原始信号（NaN 保护之后）。编辑器没开时只读一个原子量就返回。
    analyzerTap.push (celadon::AnalyzerTap::Stream::input, buffer, buffer.getNumSamples());

    // ---- 信号链：IN 表 → Balance（v0.3）→ Clay（v0.1.7.0）→ Color → Limit → OUT 表，由 CeladonChain 按顺序调（GLAZE 编排器在链里）----
    // 宿主时间线先交给链：编排器据此识别跳播 / 循环回绕；离线导出沿用相同的样本时间学习与校准。
    chain.setHostInfo (readHostInfo());

    // 宿主旁通走同一条链：链内把每个模块都置为旁通，输出是延迟补偿过的干信号，切换有 20 ms 交叉淡化。
    // Lookahead 交给链的是处理器层稳住后的值（LookaheadHold）：参数在动时保持旧前瞻，停稳 0.3 s 才换。
    auto params = readParams();
    params.limit.lookaheadMs = lookaheadHold.next (params.limit.lookaheadMs, buffer.getNumSamples(), restartForHostReset);

    // 界面的 A / B 对比听 B（原声）时，链走与宿主旁通同一条干路；试听差值时只换送出去的信号（v0.1.8.0，见 CeladonChain::setParams）。
    chain.setParams (params, hostBypassed, compareOriginal.load (std::memory_order_relaxed),
                     auditionDifference.load (std::memory_order_relaxed));

    // 本块交给模块的实际参数里受 GLAZE 影响的 7 项给界面（淡弧与小圆点；Limit Gain 含校准偏差与保护偏移）。
    publishEffectiveForUi (params);

    // 宿主 Reset 放在交参数之后：这一块若同时换了前瞻，整链重启的静音段按新延迟算，限幅器也不会在重启之后
    // 再为换前瞻单独重置一次。参数没变时与 v0.4 的「先重启、后交参数」逐位相同（交同样的参数不改任何状态）。
    if (restartForHostReset)
        chain.restartWithFadeIn();

    pendingLatency.store (chain.getLatencySamples(), std::memory_order_relaxed);   // 由定时器在消息线程上报

    // 输出有 NaN / Inf（内部溢出）时链自己置零、清空整条链并淡入恢复（v0.6 挪进链里），这里只计数。
    if (chain.process (buffer))
        nonFiniteBlocks.fetch_add (1, std::memory_order_relaxed);

    // 频谱仪的 OUT：插件最终输出（听原声时就是原声，Listen 打开时含监听增益，试听差值时是差值）——频谱仪显示的就是此刻听到的。
    analyzerTap.push (celadon::AnalyzerTap::Stream::output, buffer, buffer.getNumSamples());
}

celadon::glaze::HostInfo CeladonAudioProcessor::readHostInfo() const noexcept
{
    celadon::glaze::HostInfo info;
    info.offline = isNonRealtime();

    // 不叫 playHead：AudioProcessor 有同名成员，MSVC /W4 报 C4458（局部变量遮住成员）。
    auto* hostPlayHead = getPlayHead();

    if (hostPlayHead == nullptr)
        return info;

    const auto position = hostPlayHead->getPosition();

    if (! position.hasValue())
        return info;

    info.hasPlayState = true;
    info.isPlaying = position->getIsPlaying();

    if (const auto samples = position->getTimeInSamples())
    {
        info.hasPosition = true;
        info.positionSamples = *samples;
    }

    // 循环：宿主给的是 PPQ。按当前速度与「此刻的 PPQ ↔ 样本」对应折成样本（假定循环内速度不变——只用来区分回绕与跳播，
    // 编排器在循环打开、位置往回跳时认作回绕，端点的精度不影响判断）。
    info.hasLoop = true;
    info.looping = position->getIsLooping();

    const auto loop = position->getLoopPoints();
    const auto bpm = position->getBpm();
    const auto ppq = position->getPpqPosition();
    const auto sampleRate = getSampleRate();

    if (loop.hasValue() && bpm.hasValue() && ppq.hasValue() && info.hasPosition && *bpm > 0.0 && sampleRate > 0.0)
    {
        const auto samplesPerQuarter = 60.0 / *bpm * sampleRate;
        const auto toSamples = [&] (double ppqValue)
        {
            const auto value = (double) info.positionSamples + (ppqValue - *ppq) * samplesPerQuarter;
            return std::isfinite (value) ? (std::int64_t) std::llround (juce::jlimit (-1.0e15, 1.0e15, value)) : (std::int64_t) 0;
        };

        info.loopStartSamples = toSamples (loop->ppqStart);
        info.loopEndSamples = toSamples (loop->ppqEnd);
    }

    return info;
}

//==============================================================================
float CeladonAudioProcessor::getAndResetLimitGainReductionDb() noexcept
{
    return chain.getAndResetLimitGainReductionDb();
}

float CeladonAudioProcessor::getAndResetGlueGainReductionDb() noexcept
{
    return chain.getAndResetGlueGainReductionDb();
}

void CeladonAudioProcessor::getBalanceBandGainsDb (celadon::BalanceModule::BandGains& out) const noexcept
{
    // 界面 30 ms 定时器调：链里每段一个原子变量，读不持锁。
    chain.getBalanceBandGainsDb (out);
}

void CeladonAudioProcessor::publishEffectiveForUi (const celadon::ChainParams& manual) noexcept
{
    // 差量 = 本块实际值 − 本块手动值（实际值已在参数范围内钳过）。GLAZE 0 时实际值就是手动值，差量逐位为 0。
    const auto& effective = chain.getEffectiveParams();
    glazeDeltaForUi.recover.store (effective.balance.recover - manual.balance.recover, std::memory_order_relaxed);
    glazeDeltaForUi.tame.store    (effective.balance.tame    - manual.balance.tame,    std::memory_order_relaxed);
    glazeDeltaForUi.toneDb.store  (effective.color.toneDb    - manual.color.toneDb,    std::memory_order_relaxed);
    glazeDeltaForUi.glue.store    (effective.color.glue      - manual.color.glue,      std::memory_order_relaxed);
    glazeDeltaForUi.heat.store    (effective.color.heat      - manual.color.heat,      std::memory_order_relaxed);
    glazeDeltaForUi.gainDb.store  (effective.limit.gainDb    - manual.limit.gainDb,    std::memory_order_relaxed);
    glazeDeltaForUi.clip.store    (effective.limit.clip      - manual.limit.clip,      std::memory_order_relaxed);
}

celadon::ChainParams CeladonAudioProcessor::getGlazeEffectiveParamsForUi() const noexcept
{
    // 界面线程（30 ms 定时器）调：此刻的手动值（原子指针，与音频线程 readParams 同一来源）+ 最近一块的差量，
    // 按决策器同一个钳位（applyIncrements：各参数自己的范围）得到实际值。音频停着时拧手动旋钮，标记跟着指针走，
    // 与指针的距离就是最近一块的自动增量；还没处理过音频时差量为 0，标记落在指针上。
    celadon::glaze::AutoIncrements delta;
    delta.balanceRecover = glazeDeltaForUi.recover.load (std::memory_order_relaxed);
    delta.balanceTame    = glazeDeltaForUi.tame.load (std::memory_order_relaxed);
    delta.colorToneDb    = glazeDeltaForUi.toneDb.load (std::memory_order_relaxed);
    delta.colorGlue      = glazeDeltaForUi.glue.load (std::memory_order_relaxed);
    delta.colorHeat      = glazeDeltaForUi.heat.load (std::memory_order_relaxed);
    delta.limitGainDb    = glazeDeltaForUi.gainDb.load (std::memory_order_relaxed);
    delta.limitClip      = glazeDeltaForUi.clip.load (std::memory_order_relaxed);
    return celadon::glaze::applyIncrements (readParams(), delta);
}

double CeladonAudioProcessor::getLatencyMilliseconds() const noexcept
{
    // 用 pendingLatency 而不是 getLatencySamples()：Lookahead 一变它就更新，不必等定时器上报给宿主。
    const auto sampleRate = currentSampleRate.load (std::memory_order_relaxed);

    if (sampleRate <= 0.0)
        return 0.0;   // 还没 prepare

    return 1000.0 * (double) pendingLatency.load (std::memory_order_relaxed) / sampleRate;
}

//==============================================================================
bool CeladonAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* CeladonAudioProcessor::createEditor()
{
    auto* editor = new CeladonAudioProcessorEditor (*this);

    // GLAZE 的三个数据源（v0.6，设计规格-GLAZE-界面与实现第 1 节）：编辑器每 30 ms 在消息线程调，全部无锁——
    // 状态是编排器的无锁快照，「重新聆听」只置原子标志，实际值是音频线程每块写的 7 个原子量。
    // 宿主保证编辑器先于处理器销毁，捕获 this 安全。
    editor->setGlazeStatusSource ([this] { return getGlazeStatus(); });
    editor->setGlazeRelearnAction ([this] { requestGlazeRelearn(); });
    editor->setGlazeEffectiveParamsSource ([this] { return getGlazeEffectiveParamsForUi(); });
    return editor;
}

//==============================================================================
void CeladonAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // DAW 存工程时调用：把整棵状态树存成 XML。根节点的属性（如界面缩放 celadon::uiid::uiScaleProperty）
    // 会写成根元素的 XML 属性，与参数一起随工程保存。
    // GLAZE 的已提交版本与独立执行快照附在导出的 XML 末尾，不挂进 parameters.state：APVTS 的状态树里
    // 只许有参数节点（addMissingParameterNodes 在 Debug 版核对这一点），也不该让宿主的撤销 / 预设机制去碰它。
    const auto state = parameters.copyState();

    if (const auto xml = state.createXml())
    {
        if (auto glaze = chain.createGlazeStateXml())
            xml->addChildElement (glaze.release());

        if (auto reference = chain.createReferenceStateXml())
            xml->addChildElement (reference.release());

        copyXmlToBinary (*xml, destData);
    }
}

void CeladonAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // DAW 打开工程时调用：只接受本插件自己的状态格式。先把 GLAZE 节点摘出来（旧工程没有这个节点 = 未学习），剩下的整棵树
    // 连同根属性一起换回来，最后把 GLAZE 节点交给编排器。缺席的参数（旧版本存的工程）由 addMissingParameterNodes 补齐：
    // GLAZE 回 0，其余（含 v0.1.7.0 的 clay_*）回默认。Clay 的画像特征、方案值与保护状态都在 GLAZE 节点里，由编排器读（旧格式按中性）。
    // 顺序有讲究：参数先换、GLAZE 后交。音频线程若在两步之间跑了一块，它看到的是新参数配旧版本（比如风格变了会开始一次过渡），
    // 下一块接到存档时整体直接落到位、覆盖掉这一块的中间状态；反过来的顺序则会让存档按旧参数落位、再被新参数当成「换风格」。
    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (parameters.state.getType()))
        {
            // 只认第一个 GLAZE 节点；多出来的（坏数据）也一并摘掉，免得进了参数树。
            std::unique_ptr<juce::XmlElement> glaze;

            while (auto* node = xml->getChildByName (celadon::glaze::stateTag))
            {
                xml->removeChildElement (node, false);

                if (glaze == nullptr)
                    glaze.reset (node);
                else
                    delete node;
            }

            std::unique_ptr<juce::XmlElement> reference;
            while (auto* node = xml->getChildByName ("REFERENCE_SPECTRUM"))
            {
                xml->removeChildElement (node, false);
                if (reference == nullptr) reference.reset (node); else delete node;
            }
            chain.setReleasePreviewEnabled (false);
            auto newState = juce::ValueTree::fromXml (*xml);
            addMissingParameterNodes (newState);
            parameters.replaceState (newState);

            chain.restoreGlazeStateXml (glaze.get());
            chain.restoreReferenceStateXml (reference.get());
        }
    }
}

void CeladonAudioProcessor::addMissingParameterNodes (juce::ValueTree& newState) const
{
    // 旧版本存的状态里没有后来才加的参数（v0.3 之前没有 Balance、v0.4 之前没有 GLAZE、v0.1.7.0 之前没有 Clay）。APVTS 换树时对缺席的参数
    // 只补一个空节点、保留实例当前值（juce_AudioProcessorValueTreeState.cpp updateParameterConnectionsToChildTrees），
    // 于是对「已经在用的实例」读旧预设 / 旧片段（Ableton Hot-Swap、FL 预设）时，GLAZE 会沿用读之前的值，而不是
    // 回到旧版的 0（2026-09-25 评审 v0.4 R2-01）。新实例的 GLAZE 默认已改为 35，因此缺参恢复必须显式写 0；
    // 其余缺席参数补只带 id 的节点，由 setNewState 使用参数默认值（JUCE 9.0.2 setNewState：tree.getProperty (value, 默认值)）。
    // Clay 的 7 个参数（v0.1.7.0）正是走「只带 id → 默认值」这一支，有意不特判：三态默认「自动」、Bypass 关，
    // 旧画像（格式 1）没有立体声 / 超低频特征，决策器给的自动值为 0 → 旧工程打开声音不变，点「重新聆听」后才启用（设计规格-Clay 第 7 节）；
    // 已经在用的实例读旧预设时也回到「自动」，不沿用读之前抽屉里的「手动 / 关」（与其余参数回默认一致）。
    // GLAZE 的特判只认 glaze_amount 这一个 ID，不会碰到 clay_*。
    // 节点类型与属性名是 APVTS 的状态格式（每个参数一个 PARAM 子节点，属性 id / value），与 copyState 存出来的一致。
    static const juce::Identifier parameterNodeType { "PARAM" }, parameterIdProperty { "id" };

   #if JUCE_DEBUG
    // 格式核对：当前状态树的参数节点必须就是这个类型、带这个属性（JUCE 以后改了格式，这里先断住）。
    for (const auto& child : parameters.state)
        jassert (child.hasType (parameterNodeType) && child.hasProperty (parameterIdProperty));
   #endif

    // 本类的 getParameters() 返回 APVTS，会遮住基类同名函数，这里要显式调基类版本。
    for (auto* parameter : juce::AudioProcessor::getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            if (! newState.getChildWithProperty (parameterIdProperty, withId->paramID).isValid())
            {
                juce::ValueTree node (parameterNodeType, { { parameterIdProperty, withId->paramID } });
                if (withId->paramID == celadon::param::glazeAmount)
                    node.setProperty ("value", 0.0f, nullptr);
                if (withId->paramID == celadon::param::masterHarshMode)
                    node.setProperty ("value", 1.0f, nullptr); // OFF: old sessions must not acquire automatic protection.
                if (withId->paramID == celadon::param::masterTargetMode
                    || withId->paramID == celadon::param::masterCodecMarginDb
                    || withId->paramID == celadon::param::masterDither)
                    node.setProperty ("value", 0.0f, nullptr);
                newState.appendChild (node, nullptr);
            }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CeladonAudioProcessor();
}
