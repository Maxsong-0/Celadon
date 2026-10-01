/*
  ==============================================================================

    PluginProcessor.h
    总管：持有参数（APVTS）、按顺序调模块、上报延迟、NaN 保护、表。

    信号链（设计规格-总体，顺序固定）：输入 → Balance（v0.3）→ Clay（v0.1.7.0，ADR-009）→ Color → Limit → 输出，
    由 dsp/CeladonChain 负责；这里只读参数、做 NaN 保护、喂表、上报延迟。

    v0.1.7.0（修坯 Clay，设计规格-Clay 第 5 ～ 7 节）：多读 7 个 clay_* 参数（三态 + 手动值 + Bypass）交给链的 claySettings；
    三态合成、自动值与实时保护都在链里的 GLAZE 编排器，这里不算。界面要的 Clay 实际值与保护状态走 getGlazeStatus()，
    不另加原子量。旧工程缺 clay_* 参数时回到默认「自动」（旧画像没有立体声特征，自动值为 0，声音不变）。

    v0.6（GLAZE，ADR-008）：两块表与 GLAZE 编排器挪进链里（dsp/CeladonChain）；这里每块从 AudioPlayHead 读播放位置、
    是否在播、循环与离线渲染标志交给链，工程存取时把 GLAZE 的已提交版本附在导出的 XML 上，给界面注入 GLAZE 的三个数据源。

    v0.5（性能优化，研究 P5）另外三件宿主集成的事也在这里：
      - 同规格的 prepareToPlay 只清状态、不整体重建（R8）：宿主每次重启都会调它，重建要 1.5 ～ 3.7 ms；
      - Lookahead 在处理器层稳住（R9 / R9b）：参数在动时链继续用旧前瞻，停稳 0.3 s 才换并上报延迟，
        拖一次旋钮宿主最多重启一两次，不再每档静音一次；
      - 报真实的尾巴长度（R10）：链延迟 + 滤波器余振。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <optional>

#include "AnalyzerTap.h"
#include "Parameters.h"
#include "dsp/CeladonChain.h"

//==============================================================================
class CeladonAudioProcessor  : public juce::AudioProcessor,
                               private juce::Timer
{
public:
    //==============================================================================
    CeladonAudioProcessor();
    ~CeladonAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // 宿主旁通（VST3 的 Bypass 键、AU 的 BypassEffect）：走同一条链，强制旁通全部模块，
    // 输出是延迟补偿过的干信号，切换有 20 ms 交叉淡化。
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // 宿主要求清空状态（VST3 setProcessing(false)、AU Reset）。可能不在音频线程调用，
    // 所以这里只立旗，真正的重置在下一个音频块开头做。
    void reset() override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    // 尾巴 = 当前链延迟 + 滤波器余振 filterRingOutSeconds（v0.5 R10；v0.1.7.0 起 0.35 s）。任何线程可调：只读两个原子量。
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // 界面用（界面线程调用，内部全是原子变量，无锁）。两块表在链里（v0.6），这里转交。
    juce::AudioProcessorValueTreeState& getParameters() noexcept   { return parameters; }
    float getAndResetInputTruePeakDb() noexcept                    { return chain.getAndResetInputTruePeakDb(); }
    float getAndResetOutputTruePeakDb() noexcept                   { return chain.getAndResetOutputTruePeakDb(); }
    float getOutputShortTermLufs() const noexcept                  { return chain.getOutputShortTermLufs(); }
    float getOutputIntegratedLufs() const noexcept                 { return chain.getOutputIntegratedLufs(); }
    void resetMeters() noexcept                                    { chain.requestMeterReset(); }
    float getAndResetLimitGainReductionDb() noexcept;
    float getAndResetGlueGainReductionDb() noexcept;

    // Balance 各段当前修正量（dB），界面 27 段曲线用；无锁。
    void getBalanceBandGainsDb (celadon::BalanceModule::BandGains& out) const noexcept;
    int getNonFiniteBlockCount() const noexcept                    { return nonFiniteBlocks.load (std::memory_order_relaxed); }

    // 当前整条链的延迟（毫秒），顶栏显示用；还没 prepare 时返回 0。
    double getLatencyMilliseconds() const noexcept;

    // ---- GLAZE（v0.6）：给界面的三个数据源（createEditor 里注入编辑器）。都在消息线程调、无锁、不等音频线程 ----
    // 状态快照（状态行与「重新聆听」按钮；v0.1.7.0 起还带 Clay 四个实际值与两项保护状态，修坯指示灯与抽屉的小圆点读它）；
    // 「重新聆听」动作（只置原子标志）；
    // 本块交给模块的实际参数：只有受 GLAZE 叠加的 7 项是实际值（limit.gainDb 含校准偏差与保护偏移），其余字段是此刻的手动值
    // （clay 字段不是 Clay 的实际值——那在状态快照里）。
    celadon::glaze::Status getGlazeStatus() const noexcept         { return chain.getGlazeStatus(); }
    void requestGlazeRelearn() noexcept                            { chain.requestGlazeRelearn(); }
    celadon::ChainParams getGlazeEffectiveParamsForUi() const noexcept;

    // ---- v0.1.8.0 界面改版：A / B 对比、等响读数、频谱仪（都在消息线程调、无锁）----
    // A / B 对比：true = 正在听 B（原声）。链按宿主旁通同一条干路出声（延迟补偿、20 ms 交叉淡化、校准暂停），
    // Listen 打开时两边等响（见 CeladonChain::setParams）。只是界面状态，不是参数：不进自动化、不随工程保存；
    // 编辑器关闭时切回 A，免得窗口关了还在听原声。
    void setCompareOriginal (bool shouldListenToOriginal) noexcept
    {
        if (shouldListenToOriginal) chain.setReleasePreviewEnabled (false);
        compareOriginal.store (shouldListenToOriginal, std::memory_order_relaxed);
    }
    bool isComparingOriginal() const noexcept                      { return compareOriginal.load (std::memory_order_relaxed); }

    // 试听差值（LIMIT 卡片的耳机）：true = 送出去的是限幅器削掉的部分（按推响前的电平），表、响度、GLAZE 照常按母带算；
    // 听原声、宿主旁通或 LIMIT 旁通时不生效（见 CeladonChain::setParams）。与 A / B 一样只是界面状态、不存，编辑器关闭时关掉。
    void setAuditionDifference (bool shouldAudition) noexcept
    {
        if (shouldAudition) chain.setReleasePreviewEnabled (false);
        auditionDifference.store (shouldAudition, std::memory_order_relaxed);
    }
    bool isAuditioningDifference() const noexcept                  { return auditionDifference.load (std::memory_order_relaxed); }

    void setReleasePreviewEnabled (bool enabled) noexcept
    {
        if (enabled)
        {
            compareOriginal.store (false, std::memory_order_relaxed);
            auditionDifference.store (false, std::memory_order_relaxed);
        }
        chain.setReleasePreviewEnabled (enabled);
    }
    bool isReleasePreviewEnabled() const noexcept { return chain.isReleasePreviewEnabled(); }
    void requestReferenceLearn() noexcept { chain.requestReferenceLearn(); }
    void cancelReferenceLearn() noexcept { chain.cancelReferenceLearn(); }
    void clearReference() noexcept { chain.clearReference(); }
    celadon::MasteringStatus getMasteringStatus() const noexcept { return chain.getMasteringStatus(); }

    // Listen 测到的「处理结果 − 原声」响度差（dB），见 CeladonChain::getListenDifferenceDb。
    bool hasListenDifference() const noexcept                      { return chain.hasListenDifference(); }
    float getListenDifferenceDb() const noexcept                   { return chain.getListenDifferenceDb(); }

    // 频谱仪的取样口：IN = 链入口（NaN 保护之后），OUT = 插件最终输出（含 Listen 的监听增益、A / B 的原声、试听差值）。
    celadon::AnalyzerTap& getAnalyzerTap() noexcept                { return analyzerTap; }

    // 当前采样率（频谱仪换算频率用）；还没 prepare 时返回 0。
    double getSampleRateForUi() const noexcept                     { return currentSampleRate.load (std::memory_order_relaxed); }

    // Lookahead 参数停稳多久才真正换前瞻（v0.5 R9b）。拖旋钮时相邻两档之间通常只隔几十毫秒，
    // 0.3 s 足以把一次拖动认成「一次改动」，松手后又不至于让人觉得延迟显示迟迟不跟。
    static constexpr double lookaheadSettleSeconds = 0.3;

    // 尾巴里「滤波器余振」那一段（v0.5 R10）：Balance 27 个峰值滤波器、Tone 搁架、5 Hz 隔直衰减到 −60 dB 约 0.25 s。
    // v0.1.7.0 加了 Clay：低切最低 10 Hz 的 4 阶 Butterworth 高通，最慢一对极点的衰减率 = sin(π/8) × 2π × 10 ≈ 24 /s，
    // 衰减 60 dB 约 ln(1000) / 24 ≈ 0.29 s，成了链里最慢的一级（收单高通 ≥ 40 Hz、加宽高架都快得多）；原来的 0.3 s
    // 只剩约 10 ms 余量，改为 0.35 s。只影响向宿主报的尾巴长度，不影响音频。
    static constexpr double filterRingOutSeconds = celadon::CeladonChain::filterRingOutSeconds;   // 一处真源（审核 V3-02）

private:
    //==============================================================================
    // 把全部参数（Balance + Clay 设置 + Color + Limit + GLAZE + 高级母带）读成一份快照，每块调一次；只读原子指针，不查字符串。
    // Clay 只读用户设置（claySettings）；交给 ClayModule 的实际参数（clay）由链里的编排器按三态合成，这里保持默认。
    // 非有限的原始值换成新实例默认值（T-032 R8-02，Lookahead 除外——交给 LookaheadHold）。
    celadon::CeladonChain::Params readParams() const noexcept;

    // 读入的状态树里缺哪个参数（旧版本存的状态），就补一个只带 id 的节点，让它回到默认值（少数参数特判成旧版的值）。
    // stateFormat = 存档根元素的 celadonStateFormat（没有 = 0，≤ 0.1.8.5；T-032 R10-02 起写 1），按存档区分回退规则的锚点。
    void addMissingParameterNodes (juce::ValueTree& newState, int stateFormat) const;
    void processChain (juce::AudioBuffer<float>& buffer, bool hostBypassed);
    void processBuses (juce::AudioBuffer<float>& buffer, bool hostBypassed);

    // 宿主时间线：播放位置、是否在播、循环（PPQ 按当前速度折成样本）、离线渲染。拿不到的项 has* = false。音频线程调。
    celadon::glaze::HostInfo readHostInfo() const noexcept;

    // 把链刚交给模块的实际参数与本块手动值之差（受 GLAZE 影响的 7 项）写进原子量（音频线程每块、prepareToPlay 各写一次）。
    void publishEffectiveForUi (const celadon::ChainParams& manual) noexcept;

    // 延迟上报放在消息线程：setLatencySamples 会加锁、向消息队列投递，不能在音频线程调。
    void timerCallback() override;

    //==============================================================================
    // Lookahead 在处理器层稳住（v0.5 研究 P5 R9 / R9b）。
    //
    // 问题：Lookahead 决定延迟。v0.4 每换一档，限幅器就整体重置一次（静音一个延迟长度再 10 ms 淡入），
    // 定时器再向宿主报一次新延迟——VST3 宿主每次都 setActive(false / true) 重启处理，FL 里重启期间音频线程被挡住。
    // 拖一次旋钮（1 → 10 ms 共 18 档）宿主重启 18 ～ 34 次，四分之一的输出是静音。
    //
    // 做法：参数在动时，交给链的前瞻保持旧值；同一个值连续保持 lookaheadSettleSeconds（按处理过的样本数算，
    // 离线导出也确定）才换成新值。链的延迟只在这一刻变，定时器下一次回调把它报给宿主——报给宿主的永远是链此刻
    // 真正的延迟，拖动途中不报「还没生效的延迟」。
    //
    // 两种情况不等停稳、直接采用（「提前采用」）：这一块链本来就要整体重启（宿主 Reset：反正要静音淡入），
    // 以及每次 prepareToPlay 之后的第一块——整体重建时宿主可能在 prepare 之后、第一块之前才读入工程状态；
    // 同规格重进时链刚被 reset，恢复播放的第一块本来就从空的延迟线起步，换前瞻的静音落在起点、听不到
    // （v0.5 验收 V2-2：VST3 宿主挂起时 setProcessing(false) 调的 reset 会被随后的 prepareToPlay 清掉，
    // 不在这里提前采用的话，挂起期间改的值要等恢复 0.3 s 后才换，静音与淡入落在音乐中间）。提前采用每 settleSamples
    // 最多一次：防止「采用 → 报延迟 → 宿主重启 → 又提前采用」在拖动途中连成一串。
    //
    // 只在音频线程与 prepareToPlay 里读写（宿主保证两者不并发），不需要原子量。
    struct LookaheadHold
    {
        // prepareToPlay 时调。fullRebuild = 这次是整体重建（第一次 prepare，或采样率 / 块长 / 声道数变了）：
        // 直接采用当前参数值、视为早已停稳，并允许第一块再提前采用一次。同规格的 prepare（宿主重启）先保留正在用的值，
        // 第一块照样允许提前采用，但要受「距上次采用 ≥ settleSamples」约束：拖动途中刚采用过、宿主因此重启时，
        // 不会把半路的值再提交出去、又引起一次重启。
        void prepare (float requestedMs, int newSettleSamples, bool fullRebuild) noexcept;

        // 每块调一次，返回这一块交给链的前瞻（毫秒）。chainRestarting = 这一块链本来就要整体重启（宿主 Reset）。
        float next (float requestedMs, int numSamples, bool chainRestarting) noexcept;

        float appliedMs = 0.0f;         // 交给链的前瞻
        float candidateMs = 0.0f;       // 最近看到的参数值
        int stableSamples = 0;          // 候选值连续不变的样本数（到 settleSamples 就不再往上加）
        int samplesSinceCommit = 0;     // 上次采用新值以来的样本数（同上封顶）
        int settleSamples = 0;          // 停稳门槛（样本）= lookaheadSettleSeconds × 采样率
        bool initialised = false;
        bool commitOnFirstBlock = false;
    };

    //==============================================================================
    juce::AudioProcessorValueTreeState parameters;

    // 音频线程读参数用的原子指针（构造时取一次，之后不再按字符串查找）。
    struct LimitParameterPointers
    {
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* ceiling = nullptr;
        std::atomic<float>* release = nullptr;
        std::atomic<float>* releaseAuto = nullptr;
        std::atomic<float>* lookahead = nullptr;
        std::atomic<float>* clip = nullptr;
        std::atomic<float>* listen = nullptr;
        std::atomic<float>* bypass = nullptr;
    };

    struct ColorParameterPointers
    {
        std::atomic<float>* tone = nullptr;
        std::atomic<float>* glue = nullptr;
        std::atomic<float>* heat = nullptr;
        std::atomic<float>* mix = nullptr;
        std::atomic<float>* output = nullptr;
        std::atomic<float>* bypass = nullptr;
    };

    struct BalanceParameterPointers
    {
        std::atomic<float>* recover = nullptr;
        std::atomic<float>* tame = nullptr;
        std::atomic<float>* brighten = nullptr;
        std::atomic<float>* speed = nullptr;     // AudioParameterChoice：原始值是选项下标 0 / 1 / 2（Slow / Mid / Fast）的 float
        std::atomic<float>* bypass = nullptr;
    };

    // Clay 修坯（v0.1.7.0，设计规格-Clay 第 6 节）：三个 *Mode 是 AudioParameterChoice，原始值是选项下标 0 / 1 / 2
    //（自动 / 关 / 手动 = celadon::ClayMode）的 float；三个手动值是实际单位（Hz / Hz / dB）；bypass 是 0 / 1 的 float。
    struct ClayParameterPointers
    {
        std::atomic<float>* lowCutMode = nullptr;
        std::atomic<float>* lowCutHz = nullptr;
        std::atomic<float>* monoMode = nullptr;
        std::atomic<float>* monoHz = nullptr;
        std::atomic<float>* widthMode = nullptr;
        std::atomic<float>* widthDb = nullptr;
        std::atomic<float>* bypass = nullptr;
    };

    struct MasteringParameterPointers
    {
        std::atomic<float>* harshMode = nullptr;
        std::atomic<float>* harshAmount = nullptr;
        std::atomic<float>* transient = nullptr;
        std::atomic<float>* targetMode = nullptr;
        std::atomic<float>* targetAmount = nullptr;
        std::atomic<float>* codecMarginDb = nullptr;
        std::atomic<float>* dither = nullptr;
        std::atomic<float>* previewLufs = nullptr;
    } masteringParams;

    LimitParameterPointers limitParams;
    ColorParameterPointers colorParams;
    BalanceParameterPointers balanceParams;
    ClayParameterPointers clayParams;
    std::atomic<float>* glazeAmount = nullptr;   // GLAZE 总旋钮 0 ～ 100 %（ADR-006）
    std::atomic<float>* glazeStyle = nullptr;    // GLAZE 风格：选项下标 0 ～ 3（自动 / 温润 / 冲击 / 通透，v0.6）
    std::atomic<float>* glazeGenre = nullptr;    // T-038 曲风：选项下标 0 ～ 3（通用 / Deep / Tech / Melodic-Progressive）
    std::atomic<float>* glazeDelivery = nullptr; // T-038 交付：选项下标 0 ～ 2（标准 / 流媒体 / 俱乐部）

    celadon::CeladonChain chain;   // 含两块表与 GLAZE 编排器（v0.6）

    // 受 GLAZE 影响的 7 项在本块的「实际值 − 手动值」（音频线程每块写，界面线程读；顺序 = glaze::AutoIncrements 的字段；
    // Limit Gain 含校准偏差与保护偏移）。存差量而不是实际值：界面取值时按此刻的手动值叠加，音频停着时拧手动旋钮，
    // 标记也跟着指针走（v0.6 验收 V2-07：存实际值时标记停在旧值，看起来像负的自动增量）。
    struct GlazeDeltaForUi
    {
        std::atomic<float> recover { 0.0f }, tame { 0.0f }, toneDb { 0.0f }, glue { 0.0f }, heat { 0.0f },
                           gainDb { 0.0f }, clip { 0.0f };
    };

    GlazeDeltaForUi glazeDeltaForUi;

    // 上次整体重建（chain / 两块表 prepare）用的规格；同规格再来 prepareToPlay 只清状态（R8）。只在 prepareToPlay 里读写。
    std::optional<juce::dsp::ProcessSpec> preparedSpec;

    LookaheadHold lookaheadHold;

    std::atomic<int> nonFiniteBlocks { 0 };
    std::atomic<int> pendingLatency { 0 };        // 音频线程写（= 链此刻的延迟），消息线程读后上报；尾巴长度也用它
    std::atomic<double> currentSampleRate { 0.0 }; // prepareToPlay 写，界面线程读（把延迟换算成毫秒）；0 = 还没 prepare
    std::atomic<bool> hostResetPending { false };
    std::atomic<bool> compareOriginal { false };  // 界面写（A / B 对比听 B），音频线程每块读
    std::atomic<bool> auditionDifference { false }; // 界面写（LIMIT 试听差值），音频线程每块读
    bool lastBlockHostBypassed = false;           // 只在音频线程读写

    celadon::AnalyzerTap analyzerTap;             // 频谱仪取样口：音频线程推、界面线程取

    // 宿主旁通切换后这么多个块内收到的 Reset 视为「切换附带的 Reset」而忽略（见 processChain）。只在音频线程读写。
    static constexpr int hostBypassResetGraceBlocks = 2;
    int blocksSinceHostBypassChange = hostBypassResetGraceBlocks;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CeladonAudioProcessor)
};
