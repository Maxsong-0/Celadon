/*
  ==============================================================================

    GlazeController.h
    GLAZE 编排器。T-048 阶段 2 起是连续调度（设计规格-实时GLAZE、ADR-030「取消聆听与锁定」）：设计规格-GLAZE-时间线与保存
    里的状态机、候选 / 已提交版本都已删掉，三个就绪标志只剩 Status 里不再推进的旧字段。每块用实时画像与实时读数算出交给各模块
    的实际参数（v0.1.7.0 起四个：Balance / Clay / Color / Limit），并处理跳播 / 回绕 / 宿主 reset / 离线导出与工程存读档。
    实现口径与数值见 GlazeController.cpp 开头（速率、死区、G_trim 的常数在其中 Impl 开头）。由 CeladonChain 持有，
    链里每块按下面的顺序调用：

        CeladonChain::setHostInfo   → controller.setHostInfo (info);                 // 识别跳播 / 循环回绕 / 离线
        CeladonChain::setParams     → p = controller.beginBlock (manual, dryPath);  // dryPath = 宿主旁通或听原声。再把 p 交给四个模块
                                                                                      //  （与 v0.5 同一时机，getLatencySamples 照样立即反映 Lookahead）
        CeladonChain::process       → controller.analyzeSpectrum (balance);        // 每个宿主块开头一次：上一块 Balance 新发布的帧（见该函数注释）
                                      GLAZE > 0 时宿主块在预算环 10 ms 时隙的边界切片（getGuardSamplesUntilBoundary；宿主在播、
                                      给了位置或拍点与速度时，后几片先 setHostInfo 到片起点，走完再补到块尾；试听差值时另按
                                      差值缓冲长度切，与 GLAZE 无关），每片：
                                      inputMeter.process (io);
                                      controller.analyzeInput (io, inputMeter);     // 入口特征（模块处理前）
                                      balance / clay / color / limiter 处理 io；输出非有限值 → 清零 + 重启淡入（其中调 hostReset）；
                                      outputMeter.process (io);
                                      controller.endBlock (balance, inputMeter, outputMeter, stats, latency, true, &io);
                                      各片都用本块 beginBlock 给的同一份参数；endBlock 新算出的量到下一块 setParams 才交给模块。

    实际参数 = clamp(手动值 + 已施加的自动增量)（applyIncrements）；Tone / Tame / Heat / Glue 另有实时跟随，Limit Gain 另加 G_trim
    与三个守门的最小退让（见下面「实现口径」）。GLAZE ≤ 0（含 NaN）时已施加增量、G_trim、实时跟随量与守门退让当块落到 0
    （snapAutomatic），applyIncrements 对 0 增量原样返回手动值，其余附加项都只在 GLAZE > 0 时做：GLAZE ≤ 0 时实际参数逐位等于
    手动值（不经任何额外运算；ADR-035 决定 2；Clay 按下面的三态合成）。

    v0.1.7.0 修坯 Clay（设计规格-Clay、ADR-009；A6 接线）：Clay 不走「手动值 + 自动增量」，而是三态（自动 / 关 / 手动）：
      自动值 = 目标方案的 plan.clay 经压力反馈缩放（scaleClayPlan，ADR-014）→ ClayGuard 实时保护（updateClay，每块 beginBlock 调；
      编排器不对它做速率限制，也没有方案过渡混合）。GLAZE ≤ 0、还没读到实时画像、画像格式低于 clayProfileFormatVersion（实时画像
      发布时恒填这一版，这条实际碰不到），或还没有目标方案（prepare / 读档之后的第一块：目标方案到这一块的 endBlock 才重算）时，
      交给 ClayGuard 的是中性方案。
      实际参数 effective.clay = resolveClayParams(用户三态设置, 保护后的自动值, GLAZE)。GLAZE ≤ 0 时也照算（「手动」照样
      生效；全「自动」时是中性，Clay 逐位直通）——原有 7 个参数在 GLAZE ≤ 0 时逐位等于手动值的保证不变。
      保护后的自动值（liveClay）、ClayGuard 状态与压力反馈缩放随工程存进 LIVE 的 CLAY 节点；读档后第一块 beginBlock 就按上面的
      中性方案重算 liveClay，读回的 ClayGuard 状态与缩放接着用。
      ADR-012：Status::clayCostLimitedMask = 上面经压力反馈缩放后的自动值（探针的 clayPlanned）里因「试过会抬高峰值」被减量 /
      否决的「自动」态项——决策器因代价置的位，加上压力反馈缩小了的项（scaleClayPlan 置位）；GLAZE > 0 才报。

    线程：
      - 音频线程：setHostInfo / beginBlock / analyzeSpectrum / analyzeInput / endBlock / handleDiscontinuity / hostReset。0 分配、0 锁。
      - 消息线程：prepare / createStateXml / restoreStateXml / takeStateRestoredBeforePrepare / setLiveSegmentSink / syncLiveModel；
        requestRelearn 已是空操作。
      - 任何线程：getStatus（无锁快照，endBlock 每块发布一份；legacySessionDiscarded 另读一个原子标志）。
      编排器不再在两线程之间交换「已提交版本」（旧版音频线程 try-lock 发布、消息线程持锁读写的那一套随状态机删掉了）：
      createStateXml / restoreStateXml 直接读写音频线程每块在改的量，编排器内部不加锁；其中 LiveProfile 要求自己的存读档不与
      音频线程接口同时调用（LiveProfile.h 文件头第三节）。
      restoreStateXml 之后、第一块音频之前调 createStateXml，返回的是刚恢复（或冷启动）的内容（不是旧值；例外：还没 prepare 时
      冷启动不清 LiveProfile，LIVE_PROFILE 照旧是它当时的内容，见 .cpp coldStart）。

    ---- 实现口径（T-048 阶段 2；GlazeController.cpp 开头只是概述，细节以本节为准——例如 hostReset 除滤波器与时隙外还清分析器
    短窗口与 Clay 的两个状态，见下「宿主 reset」一段）----

    没有聆听、候选 / 已提交画像、锁定、方案过渡、校准轮、约束反馈、画像失配，也没有「重新聆听」：每块按实时画像与实时读数连续调度。

    画像：链持有的 LiveProfile（attachLiveProfile）。prepare 把它绑成分析器的段接收者并打开实时模式；endBlock 每块读它已发布的那一份。
    置信度 c 取自 LiveProfile 的诊断（Status::confidence）；决策器与下面 Tone / Tame 的实时跟随按 confidenceScale 打折（κ = 底强度
    + (1 − 底强度) × min(画像 I 置信度, 真峰值置信度, 代表性) ÷ fullConfidence 后钳到 1；出厂底强度 0.35、fullConfidence 0.8，
    GlazePlanner.cpp confidenceScale）。
    主段度 m：endBlock 每块用实时段落账正在开着的那一段的暂判读数算（LiveProfile::mainnessFor；静段为 0，其余读不到的情况为 1），
    交给出口电平跟踪器（OutputLevelTracker：m ≥ 0.5 的段才进出口主段电平），也决定增益棘轮。

    目标方案：GLAZE > 0 时 makePlan 只在三种时候算——还没有目标方案；本块 beginBlock 发现 GLAZE / 风格 / 曲风 / 交付或影响方案的
    手动值变了；编排器自己的时间线（已处理样本数，随工程保存）跨过 1 s 网格。不提交、不过渡，maxGainDb 恒为不限。每次重算时
    「自动」风格的权重朝 resolveAutomaticWeights 走一步（有实时画像时；步长 = weightPerSecond × 本块时长），混合后的风格常数
    交给预算环作上限，其中手动单独已超预算的项（Plan::manualBudgetMask）置 0 = 不设约束。
    已施加增量：GLAZE > 0 时 endBlock 每块按速率限制朝目标方案的增量走——Recover / Tame / Glue / Heat / Clip 上 2、下 10（每秒，
    0 ～ 100 刻度），Tone 上 0.2、下 1 dB/s，Limit Gain 上 0.2、下 1 dB/s 并带回差死区（误差进 0.1 dB 停住、超过 0.3 dB 再走）。
    增益棘轮：此刻 m < 0.5 时 Limit Gain 增量只降不升（ADR-030 决定 4）。
    实时跟随：Tone 按实时与画像的频谱斜率之差偏移（死区外，≤ 0.15 dB/s）；Heat / Glue 按实时密度与变亮程度收回自动加上去的部分；
    Tame 增量取已施加量与按实时「持续突出频段」证据换算的量中较大的一个。以上都只在 GLAZE > 0 时做；Tone / Heat / Glue 还要
    Color 没旁通、Mix > 0，它们的跟随量只在出口是母带、有有效音乐时推进（Impl::updateFollowers）；Tame 这一项还要 Balance
    没旁通（beginBlock）。

    Limit Gain 附加项（applyLimitExtras，beginBlock 末尾）：
      - G_trim：输出侧慢修正，取代校准器。endBlock 按「方案目标响度 − 出口主段电平」积分，≤ 0.05 dB/s，钳在 ±3 dB；施加时负的
        G_trim 最多撤掉自动加上去的那部分。宿主旁通 / 听原声、Color 或 Limit 旁通、电平类自动全部撤回、预算环正在退让、
        还没有目标方案或出口主段电平时不积分。
      - 三个守门各出一份 ≤ 0 的退让：预算环 GlazeGuard、落差守门 ContrastGuard、PSR 守门 PsrGuard；取最小的一份，只退 G_trim 之后
        高出手动 Gain 的那部分。宿主旁通 / 听原声、Color 或 Limit 旁通时守门冻结、退让保持（预算环丢掉未完成的时隙）。
      - 电平类自动全部撤回（Plan::automaticLevelWithdrawn）时守门不跑、G_trim 与退让都不加（Limit Gain 就是 applyIncrements 的结果）。
      - 加 G_trim、再加退让，两步各钳在 0 ～ 24 dB。

    状态（getStatus）：phase 恒为 unlearned、calibrationValid 恒为 false，requiredSeconds 与 houseTargetSegments /
    houseRequiredFullSegments / houseTargetSeconds 填 0，其余聆听 / 校准 / House 锁定的旧字段不写、停在结构默认值（GlazeTypes.h
    Status；.cpp publishStatus）。界面状态行（GlazeStatusText readingFromStatus）读 confidence、currentMainness、coldExport
    （离线且 c < 0.5）、guardCause / guardEngaged / protectionDb（三个守门里正在起作用、退得最深的那一扇）、limitedByManual，
    以及 houseActive / housePhaseHint / housePhaseMonoLossDb、effectiveTargetLufs、resolvedStyle。planLimited（预算环退到下限
    仍超）照常发布，但状态行只认 guardEngaged、不读它（tests/GlazeStatusTextTests.cpp「planLimited does not stand in for
    guardEngaged」）；legacySessionDiscarded 由编辑器读，点亮一次「旧的聆听结果已丢弃」提示。

    工程：只存 LIVE（GLAZE_STATE formatVersion 2，LIVE 节点外层 zlib + base64）——实时画像、已施加增量（含 Limit Gain 的回差
    死区状态）、G_trim、风格权重、实时跟随量、时间线、三个守门、出口主段电平、Clay 自动值 / ClayGuard 状态 / 压力反馈缩放。
    读到更高版本或坏档 → 冷启动；旧格式（formatVersion 1，或带 GLAZE_PROFILE / PLAN / CALIBRATION / EXECUTION）→ 整份丢弃、
    冷启动，Status::legacySessionDiscarded 让界面提示一次。读档后目标方案在第一块 GLAZE > 0 的 endBlock 重算。

    宿主 reset / 跳播 / 回绕：分析器在此刻断开短窗口，实时段落账开着的段截到此刻、就地定稿交给段接收者（插件里是
    LiveProfile；MusicAnalyzer handleDiscontinuity）；hostReset 另让预算环丢掉未完成的时隙、出口电平跟踪器清 K 加权滤波器、
    Clay 压力反馈的累计与守护回收指示清零。LIVE 里的统计不清（ADR-030 决定 6），已施加增量、G_trim、目标方案与守门的退让也都不动。

    离线导出：与实时同一条路径，分析器把离线当作在播；c < 0.5 时 Status::coldExport 提示。先播一遍再导出才从头到位，重复导出
    不保证逐位相同（ADR-030 决定 6）。

    曲风：每块 analyzer.setGenre 跟着此刻的曲风；曲风与交付都进 makePlan 与预算环上限。主段模型（mainSectionModelFor）由
    syncLiveModel 在消息线程交给 LiveProfile；插件里只有 prepareToPlay 调到它（CeladonChain::prepare 与 syncGlazeLiveModel），
    播放中换曲风，主段模型要到下一次 prepareToPlay 才切。House 子类在 GLAZE > 0 且自动 Clay 可用时另算反相提示
    （ClayGuard::updateHouseHint：资格 = 此刻是满编段，条件须连续成立 phaseHintSegments 小节，不按小节分段时按
    phaseHintFallbackSeconds 秒）。但阶段 2 起段落账不再算整本账的门控（SegmentLedger.cpp「读数」节：summary.valid 保持 false），
    isCurrentSegmentFull 恒为 false，这条提示实际不会亮（端到端用例 testHousePhaseHintEndToEnd 现不在 runTest 里）；按规格「保留」
    修代码还是规格追认取消，待 Max 定（T-094 复查发现）。T-038 的 House 锁定条件、暂定方案与锁定后防漏网都已删掉（ADR-030 取代
    设计规格-曲风-House学习方式的锁定条件）。

  ==============================================================================
*/

#pragma once

#include "BalanceModule.h"
#include "ChainParams.h"
#include "GlazeConstants.h"
#include "GlazeProfile.h"
#include "GlazeTypes.h"
#include "LoudnessMeter.h"

#include <juce_dsp/juce_dsp.h>

#include <initializer_list>
#include <memory>

namespace celadon::glaze
{
    class SegmentSink;   // T-048：段定稿的接收者（SegmentLedger.h）
    class LiveProfile;   // 链持有的实时画像。编排器只存指针，不另分配一份

    class GlazeController
    {
    public:
        GlazeController();
        ~GlazeController();

        // 消息线程。分配全部固定容量状态。按新规格 prepare 分析器、三个守门与出口电平跟踪器，ClayGuard、压力反馈监视器的累计与
        // 守护回收监视清零；时间线归 0、目标方案作废（第一块 GLAZE > 0 的 endBlock 重算），已施加增量、G_trim、实时跟随量与守门退让
        // 落到 0（snapAutomatic）。不碰 LiveProfile（链另外 prepare 它）、自动风格权重与压力反馈缩放；卸掉 setLiveSegmentSink 挂的
        // 外部接收者，有 LiveProfile 时把分析器重新绑回它。换采样率 / 块长时链先 createStateXml、prepare 之后立刻读回
        // （CeladonChain::prepare），这些控制量照存档恢复。
        void prepare (const juce::dsp::ProcessSpec& spec);

        // ---- 音频线程 ----

        void setHostInfo (const HostInfo& info) noexcept;

        // 宿主 reset / 播放中途重置（链的 reset 与 restartWithFadeIn 调）：分析器在此刻断开短窗口（同跳播：开着的实时段截到此刻
        // 定稿），预算环丢掉未完成的时隙，出口电平跟踪器只清 K 加权滤波器，Clay 压力反馈的累计与守护回收指示清零。LIVE 里的统计
        // （实时画像、出口主段电平的段累计）不清（ADR-030 决定 6），已施加增量、G_trim、目标方案与三个守门的退让也都不动。
        void hostReset() noexcept;
        int getGuardSamplesUntilBoundary() const noexcept;

        // 不连续点（setHostInfo 自己能识别跳播与回绕；这里留给调用方直接报，目前链与测试都不调它——链遇到宿主 reset 直接调
        // hostReset）。hostReset 这一种等同 hostReset()；跳播 / 回绕：分析器断开短窗口，并把开着的实时段截到此刻、定稿交给 LiveProfile
        // （同文件头「宿主 reset / 跳播 / 回绕」一段；MusicAnalyzer::handleDiscontinuity → beginDiscontinuity → liveEndEpoch）；已施加增量、
        // G_trim、目标方案与守门退让不动，也不计入探针的 transportJumps / loopWraps。
        void handleDiscontinuity (Discontinuity kind) noexcept;

        // 每块处理前调：manual = 用户在各模块旋钮上的值（含 glazePercent、glazeStyle、glazeGenre、glazeDelivery）；GLAZE / 风格 /
        // 曲风 / 交付或影响方案的手动值相对上一块变了，就记下来让本块 endBlock 重算目标方案（GLAZE > 0 时）。
        // hostBypassed = 出口此刻不是母带（链传 dryPath = 宿主旁通或听原声）：endBlock 里守门与 G_trim 冻结、出口电平跟踪器不吃样本、
        // 实时跟随不走、ClayGuard 不推进时间。Listen（监听增益）在所有测点之后，不影响编排器。
        // 返回本块交给四个模块（Balance、Clay、Color、Limit）的实际参数（引用在下一次 beginBlock 之前有效）。时间只在 endBlock 里按 stats.numSamples 推进。
        const ChainParams& beginBlock (const ChainParams& manual, bool hostBypassed) noexcept;

        // 模块处理前：input = 链入口原始输入；inputMeter 已处理过这一块。
        void analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept;

        // 模块处理后：算此刻的主段度 m、出口主段电平吃样本、读实时画像、推进时间线（需要时重算目标方案）、三个守门、速率限制与棘轮、
        // G_trim、实时跟随、Clay 压力反馈，最后发布状态快照（顺序与条件见 .cpp Impl::endBlock）。stats = 本块模块统计。
        // balance、chainLatencySamples、spectrumDeferred 三个参数 T-048 阶段 2 起不再使用（签名留着，链照传）：频谱帧只在
        // analyzeSpectrum 里交给分析器，endBlock 不再代交——不经链、直接驱动编排器的调用方要自己调 analyzeSpectrum。
        // output：出口母带（Listen 之前）。空指针时输出电平跟踪器本块不吃样本（不经链的测试）。
        void endBlock (const BalanceModule& balance, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                       const ModuleStats& stats, int chainLatencySamples, bool spectrumDeferred = false,
                       const juce::AudioBuffer<float>* output = nullptr) noexcept;

        // v0.6 A4 追加。块开头、IN 表处理本块之前调：把上一块 Balance 新发布的帧交给分析器（频谱证据、斜率、占比）。
        // 为什么不在上一块的 endBlock 里做：Balance 每 hop 最后一片分析与帧的发布在同一块，分析器的逐帧统计若也挤进这一块，
        // 就叠在整条链的结构性最坏块上（192 kHz / 块 32 实测 +0.76 µs，一半预算）。挪到下一块开头，分析器看到的调用顺序与参数
        // 完全不变（processInput(k) → processSpectrum(k) → processInput(k+1)，两边的累计样本数也与原位置相同），只是画像晚一块
        // 用上这一块的帧。音频线程；0 分配。
        void analyzeSpectrum (const BalanceModule& balance) noexcept;

        // ---- 消息线程 ----

        // T-048 阶段 2（ADR-030）起是空操作：没有「重新聆听」，不清实时画像、不改任何状态。签名留着，因为转发链
        // CeladonChain::requestGlazeRelearn ← CeladonAudioProcessor::requestGlazeRelearn 与测试还在调；处理器仍把它交给编辑器的
        // setGlazeRelearnAction，但编辑器收下就丢掉（PluginEditor.cpp），界面上已没有入口。
        void requestRelearn() noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;

        // xml 为空、formatVersion 高于本版本、或 LIVE 读坏 → 冷启动；formatVersion 2 → 恢复 LIVE 里的实时画像与各控制量
        // （目标方案在第一块 GLAZE > 0 的 endBlock 重算）；旧格式（formatVersion 1，或带 GLAZE_PROFILE / PLAN / CALIBRATION /
        // EXECUTION）→ 整份丢弃、冷启动，并置 Status::legacySessionDiscarded；formatVersion 缺失或 ≤ 0、又没有这些旧节点的，
        // 同样冷启动、不置这个标志。不序列化滤波器 / 检测器的音频历史。
        // 第一次 prepare 之前调时另留一份原件给 takeStateRestoredBeforePrepare。
        void restoreStateXml (const juce::XmlElement* xml);

        // 第一次 prepare 之前读进的原始存档（宿主打开工程：setStateInformation 先于 prepareToPlay）。
        // 链在第一次 prepare 冷启动之后把它读回。已经 prepare 过再读档不进这里（换采样率时链自己先 createStateXml）。
        // 取出后清空。没有读过档、或读的是空指针，返回空。
        std::unique_ptr<juce::XmlElement> takeStateRestoredBeforePrepare();

        // T-048 阶段 1 留下的探针（插件不调；测试与渲染工具 --live-trace 用）：非空 = 分析器实时模式开、定稿的段改交给 sink；
        // nullptr = 关实时模式（开着的段先交给原来的 sink）。prepare 之后、处理之前调（prepare 之前调不起作用：prepare 会把它卸掉）。
        // 阶段 2 起调它（非空或 nullptr）都会让链上的 LiveProfile 收不到段：编排器读到的实时画像不再更新，输出与不调时不再逐位相同
        // （bindLive）。宿主 reset、读档不动这项设定；下一次 prepare 先让分析器把开着的段交给它，再卸掉、把分析器绑回 LiveProfile。
        // pendingFrames（关的时候）= 帧还没交给分析器的 Balance：链在下一块开头才交帧（analyzeSpectrum），关的那一刻最后一块的帧还在
        // 它那里，分析器先把这些帧只交给实时段落账再就地定稿（别的一律不碰，下一块开头照常交）。CeladonChain 交它自己的 Balance；
        // 不经链、每块处理完就调 analyzeSpectrum 交了帧的调用方给 nullptr。
        void setLiveSegmentSink (SegmentSink* sink, const BalanceModule* pendingFrames = nullptr);

        // 链在构造时交来它持有的 LiveProfile。prepare 把它绑成分析器的段接收者并打开实时模式。
        void attachLiveProfile (LiveProfile* profile) noexcept;

        // 音频线程只记下曲风。syncLiveModel 在消息线程、音频已停时调 LiveProfile::setConstants。
        void notePendingModel (Genre genre) noexcept;
        void syncLiveModel() noexcept;
        bool hasPrepared() const noexcept;

        // ---- 任何线程 ----

        // 无锁快照（endBlock 每块发布）；legacySessionDiscarded 另从原子标志读。phase 恒为 unlearned、calibrationValid 恒为 false；
        // 界面读哪些字段见文件头「状态」。
        Status getStatus() const noexcept;

        // ---- 只给测试用 ----

        // 替换出厂常数（测试用）：编排器读 activeConstants() 的地方——决策器、预算环上限与时长、ClayGuard、实时跟随、House 反相
        // 提示——都改用它；LiveProfile 的 LiveConstants 不在这里（见 syncLiveModel）。在 prepare 之前调。
        void setConstantsForTesting (const Constants& constants);
        // T-048 阶段 2 起没有「已提交版本」：按此刻拼一份——当前常数、「自动」风格权重（放在 lockedWeights）、目标方案、
        // 此刻跟随的实时画像（读到过才 hasProfile，profileId 按它算）；calibration 恒为默认值。渲染工具与测试读它。
        CommittedState getCommittedStateForTesting() const;
        // 恒为 Phase::unlearned（没有状态机了）。渲染工具 --trace 的 glaze_phase 列与测试还在读。
        Phase getPhaseForTesting() const noexcept;

        // ---- v0.6 A4 追加（只给测试用；都在音频线程读，测试里与处理同一线程）----

        // 内部计数与此刻的中间量。T-048 阶段 2（ADR-030）起编排器只写其中一部分：probeForTesting 按此刻的值填 increments、
        // targetPlan、G_trim 与预算环几项、Clay 几项、offline、实时 Heat / Glue 减量、guardLimits*（feedbackMaxGainDb 恒填 1.0e9），
        // 另外累加 clayPressureSteps、hostResets、transportJumps、loopWraps，并借用 transitionActive 作重算标志
        // （GlazeController.cpp Impl::probeForTesting 与各处 probe.）。
        // 标「停用」的字段不再推进、恒为初值。留着是因为仍在跑的测试断言它们保持初值（如 GlazeControllerTests testContinuousPhase、
        // GlazeTests 的「calibration stays retired」），tests/ 里不再调用的旧用例与渲染工具 --trace 也还引用它们。
        struct TestProbe
        {
            int learningStarts = 0;          // 停用（恒 0）。原为分析器 startLearning 的次数（首次聆听、重新聆听、首次聆听重来）
            int commits = 0;                 // 停用（恒 0）。原为候选画像提交为已提交版本的次数
            int planTransitions = 0;         // 停用（恒 0）。原为 3 s 方案过渡开始的次数（提交、换风格、约束反馈、旧算法方案改按新算法）
            int learningUpdates = 0;         // 停用（恒 0）。原为首次聆听中低置信度方案按候选画像更新的次数
            int calibrationBegins = 0;       // 停用（恒 0，下一行 calibrationFinishes 同）。原为校准器 begin 的次数（GlazeCalibrator 已删）
            int calibrationFinishes = 0;
            int feedbackSteps = 0;           // 停用（恒 0）。原为约束反馈真正下调了上限的次数
            int clayPressureSteps = 0;       // 修坯压力反馈把自动 Clay 缩放降了一档的次数（ADR-014）
            int feedbackFromBudget = 0;      // 停用（恒 0，下三行同：都是约束反馈的触发来源）。原为：校准结束为「预算受限」
            int feedbackFromGuardLimit = 0;  // 原为：保护层进入「方案受限」
            int feedbackFromUpstream = 0;    // 原为：Glue / Heat 在 3 s 窗内累计超出 ≥ 1 s
            int feedbackFromStuckGuard = 0;  // 原为：锁定且校准适用时保护层卡在同一退让值上
            int mismatchSteps = 0;           // 停用（恒 0）。原为画像失配按「这一段当画像」重算的方案给净增益上限的次数（v0.6 验收 F-01）
            int hostResets = 0, transportJumps = 0, loopWraps = 0;   // 仍写：hostReset 每次 +1；后两项只数 setHostInfo 自己识别出的跳播 / 回绕
            // 仍写，但已不是「方案过渡中」：beginBlock 发现 GLAZE / 风格 / 曲风 / 交付或影响方案的手动值变了就置 true，同一块 endBlock
            // 读过就清回 false（GLAZE > 0 时据此重算目标方案）——endBlock 之后读到的恒为 false（--trace 的 plan_transition 列因此恒为 0）。
            bool transitionActive = false;
            float transitionProgress = 1.0f; // 停用（恒 1：没有方案过渡）
            bool learning = false;           // 停用（恒 false，下一行 relearning 同）。原为分析器正在累计候选
            bool relearning = false;
            int calibratorState = 0;         // 停用（恒 0）。原为 GlazeCalibrator::State 的整数值（校准器已删）
            float appliedBiasDb = 0.0f;      // 此刻的 G_trim（输出侧慢修正，取代校准偏差；未经 applyLimitExtras 按自动部分钳制）
            float executedBiasDb = 0.0f;     // 阶段 2 起与 appliedBiasDb 相同，都是 G_trim（没有交给校准器的回馈量了）
            float executedGuardDb = 0.0f;    // 预算环（GlazeGuard）最近交出的退让（budgetOut）；不含落差 / PSR 守门，也不是取小、钳过后的执行量
            float guardDb = 0.0f;            // 预算环（GlazeGuard）自己的 g_保护
            float feedbackMaxGainDb = 0.0f;  // 没有约束反馈了：probeForTesting 恒填 1.0e9（= 不限）
            bool savedPlanMode = false;      // 停用（恒 false）。原为照用保存的旧算法方案
            bool executionSnapshotActive = false; // 停用（恒 false）。原为同算法执行快照生效中（阶段 2 没有执行快照）
            bool offline = false;
            float liveHeatReduction = 0.0f, liveGlueReduction = 0.0f;
            AutoIncrements increments;       // 已施加的自动增量（速率限制 / 棘轮之后）；beginBlock 另加的 Tone / Tame / Heat / Glue 实时跟随不在内
            Plan targetPlan;                 // 目标方案（最近一次 makePlan；已施加增量按速率朝它走）

            // ---- v0.1.7.0 修坯 Clay（A6 追加）----
            // T-048 阶段 2 起仍写的只有 clayPlanned、liveClay、clayGuard、clayAutomaticSupported、clayPressureScale；标「停用」的恒为初值
            // （压力反馈与守护回收本身照常运行，只是读数不再抄进探针；界面的守护回收指示读 Status 的 clayPeakGuard*）。
            // 渲染工具 RenderMain.cpp 的 Clay 追踪列与指标还在读其中几项（clayGuardSeconds、liveLowCorrelation*、clayPeakCost*、
            // clayBlendLossSmoothedDb），所以那几列阶段 2 起恒为 0 / 无效。
            // clayPlanned = 最近一次 beginBlock 里目标方案的 plan.clay 经压力反馈缩放与范围收拾后的值（还没有目标方案时为中性）；
            // clayAutomaticSupported 为 false 时它照算，但交给 ClayGuard 的是中性方案。
            ClayPlan clayPlanned;            // 「自动」态值，ClayGuard 之前（口径见上两行）
            ClayPlan liveClay;               // 经 ClayGuard 之后、三态合成之前的自动值（随工程存进 LIVE 的 CLAY 节点）
            ClayGuardState clayGuard;        // ClayGuard 此刻的状态
            float clayGuardSeconds = 0.0f;   // 停用（恒 0）。原为上一块交给 ClayGuard 推进的有效音频秒数
            bool clayAutomaticSupported = false; // GLAZE > 0、已读到实时画像且画像格式 ≥ clayProfileFormatVersion（不满足时自动 Clay 走中性方案）
            bool clayRestoredFirstBlock = false; // 停用（恒 false）。原为上一块直接用了执行快照里的 Clay 值
            std::uint32_t manualParamsHash = 0;  // 停用（恒 0）。原为按上一块手动参数算的校准参考条件哈希（旧口径，见 setManualConditions）
            int legacyConditionsUpgrades = 0;    // 停用（恒 0）。原为读取工程后旧存档参考升成新口径的份数（BUG-004，upgradeLegacyConditions）
            Feature liveLowCorrelation;          // 停用（恒无效，下一行 liveLowCorrelationFast 同）。原为分析器实时视图的低频相关度慢路 / 快路（诊断追踪用）
            Feature liveLowCorrelationFast;
            float clayPressureScale = 1.0f;      // 修坯压力反馈给自动 Clay 的缩放（ADR-014；随工程保存）
            double clayPeakCostDb = 0.0;         // 停用（恒 0）。原为压力反馈第一触发量此刻的代价（dB，与先试同口径，ADR-028）
            int clayPeakCostBlocks = 0;          // 停用（恒 0）。原为自上次清零起攒下的有效峰值块数（每块 0.1 s）
            double clayBlendLossSmoothedDb = 0.0;   // 停用（恒 0）。原为守护混合损失的平滑值（dB，T-043：ClayPressureMonitor::blendLossDb）
            double clayGuardKeepChange = 0.0;       // 停用（恒 0，下两行同）。原为守护回收指示（T-074，ClayGuardKeepMonitor）：平滑后的 Σd² 与 Σ(g·d)²、
            double clayGuardKeepKept = 0.0;         // 灭着时保留比连续低于亮灯线的秒数（诊断）
            double clayGuardKeepBelowSeconds = 0.0;

            // ---- T-015（BUG-003）校准诊断：渲染工具 --trace 的校准列用（tools/render/README.md）----
            // T-048 阶段 2（ADR-030）删掉校准器后整组停用、恒为初值。还在读的：渲染工具 --trace 的校准列占位（RenderMain.cpp
            // calibrationTraceColumns，十项全读）；仍在跑的测试只有 GlazeChainTests 的 testManualHeatOverBudget 与
            // testAutomaticLevelWithdrawn 断言 calibrationGateMask 第 16 位为 0，其余读者都在不再调用的旧用例里。各行「原为」之后
            // 是阶段 2 之前的含义，只供对照旧的 --trace CSV。
            bool roundActive = false;                 // 停用（恒 false）。原为校准轮进行中（阶段「校准中」）
            bool calibrationApplicable = false;       // 停用（恒 false）。原为已提交的校准对此刻适用（= Status::calibrationValid）
            int calibrationRoundsDone = 0;            // 停用（恒 0）。原为本次 begin 以来已完成的轮数（GlazeCalibrator::Progress::round）
            int calibrationSubWindows = 0;            // 停用（恒 0）。原为本轮已完成、条件一致的子窗数（每轮 4 个）
            double calibrationSubWindowSeconds = 0.0; // 停用（恒 0）。原为当前子窗已采到的有效配对时长（静音不推进）
            double calibrationGuardSeconds = 0.0;     // 停用（恒 0）。原为本次 begin 以来保护层介入期间累计的有效音乐（到窗长即「预算受限」）
            float calibrationLastErrorDb = 0.0f;      // 停用（恒 0）。原为最近一轮的修正量 e = 目标净提升 − 实测净提升
            float calibrationSuggestedDb = 0.0f;      // 停用（恒 0）。原为校准器此刻的建议偏差（相对本轮基线）
            // 停用（恒 0）。原为：
            // 轮中本块作废当前子窗的原因（位或，上一块）：1 宿主旁通 / 暂停、2 GLAZE ≤ 0、4 方案过渡中、8 条件与本轮参考不一致、
            // 16 电平类自动全部撤回（Plan::automaticLevelWithdrawn；T-032 R6-02 收尾前是「受手动设置限制」）、32 保护层介入
            // （GlazeGuard::isEngaged）、64 House 档此刻不在主段落（T-038 评审 F1：House 门控画像只代表满编段，校准只在主段落采样）。
            // 偏差变了（换工作点）由校准器自己当作中断，看执行偏差列。
            int calibrationInterruptMask = 0;
            // 停用（恒 0）。原为：
            // 不在轮中时没开新一轮的原因（位或，上一块）：1 没有已提交画像、2 GLAZE ≤ 0、4 宿主旁通 / 暂停、8 方案过渡中、
            // 16 电平类自动全部撤回（Plan::automaticLevelWithdrawn；R6-02 收尾前是「受手动设置限制」）、32 保护层在退让或恢复（未稳定
            // 也未卡住），或编排器接着施加的退让（carry）还没淡完、64 参考条件未知、
            // 128 已提交的校准适用（不需要）、256 用户条件稳定不足 1 s、512 刚对同一条件试过而结束为无效（T-032 R5-01 起放弃的轮、
            // 换方案丢掉的轮不算）、1024 House 档此刻不在主段落或还读不到（T-038 评审 F1）。0 = 本块开了新一轮或正在轮中。
            int calibrationGateMask = 0;

            // ---- T-032（R5-02 / R5-03 / R5-04 / R5-08）：保护层与约束反馈的运行量（上一块 endBlock 之后的值）----
            // T-048 阶段 2 起只有 guardLimitsSet、guardLimits 仍由 probeForTesting 从预算环（GlazeGuard）读出；其余各项随约束反馈、
            // 上游监视、执行快照与画像失配一起删掉，标「停用」、恒为初值。
            bool guardLimitsSet = false;              // 预算环已设过上限（GlazeGuard::hasLimits；第一次重算目标方案起为 true）
            // 预算环此刻生效的约束上限 = blendConstants(当前常数, 目标方案的权重, 曲风, 交付)，其中目标方案 manualBudgetMask 标出的项
            // （手动单独已超预算）置 0 = 不设约束（T-032 A11；heatDriveCap 也在里面）。掩码为 0 时与混合常数逐位相同。
            StyleConstants guardLimits;
            std::int64_t guardSteadySamples = 0;      // 停用（恒 0）。原为 g_保护 连续没变的有效音乐样本数
            std::int64_t guardStuckSamples = 0;       // 停用（恒 0）。原为「卡住」门槛折算的样本数
            std::int64_t samplesSinceFeedback = 0;    // 停用（恒 0）。原为上一次约束反馈以来的有效音乐样本数
            int monitorWindowSlots = 0;               // 停用（恒 0，下一行同）。原为上游监视窗的时隙数（guardWindowSeconds / 100 ms）
            int monitorTriggerSlots = 0;              // 原为上游监视的触发时隙数（guardTriggerSeconds / 100 ms）
            float absorbedGuardDb = 0.0f;             // 停用（恒 0）。原为沿用执行快照时并进冻结方案的保护退让（≤ 0）
            float guardCarryDb = 0.0f;                // 停用（恒 0）。原为编排器接着施加的退让（lowerGainCap 吸收 / 解除冻结的 carry 剩余量）
            std::int64_t mismatchSamples = 0;         // 停用（恒 0）。原为画像失配此刻累计的有效音乐样本

            // ---- T-038 House 档（上一块 endBlock 之后的值）----
            // T-048 阶段 2（ADR-030）删掉 House 锁定条件、暂定方案、锁定后防漏网与方案过渡，整组（含 transitionLengthSamples）停用、
            // 恒为初值。渲染工具不读；仍在跑的测试只有 GlazeControllerHouseTests 的 testHouseStaysUnlocked 断言 houseReopens 为 0，
            // 其余读者都在不再调用的旧用例里。各行「原为」之后是阶段 2 之前的含义。House 反相提示的计算路径还在（Status::housePhaseHint），
            // 但阶段 2 起满编段判定停用（SegmentLedger summary.valid 保持 false），实际不会亮——见文件头「曲风」段。
            int houseListenStarts = 0;                // 停用（恒 0）。原为：为 House 锁定条件开的聆听次数（通用 → House、旧画像在 House 档）
            int houseUpdates = 0;                     // 停用（恒 0）。原为首次聆听中暂定方案按新的门控结果更新的次数
            int houseFastTransitions = 0;             // 停用（恒 0）。原为：上一项里由新响段触发的快过渡次数
            int houseReopens = 0;                     // 停用（恒 0）。原为锁定后防漏网重开（按段落账门控重算并提交）的次数
            int houseReopenRun = 0;                   // 停用（恒 0）。原为锁定后连续「更响更满」的段数
            bool houseReopenArmed = false;            // 停用（恒 false）。原为已满足重开条件，等段落账够锁定条件
            std::int64_t transitionLengthSamples = 0; // 停用（恒 0）。原为正在进行的方案过渡的长度（样本；没在过渡为 0）
        };

        TestProbe getProbeForTesting() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeController)
    };

    // v0.1.7.0 修坯 Clay（A6）：方案过渡中 Clay 自动值的混合（纯函数、不分配）。T-048 阶段 2（ADR-030）起没有方案过渡，编排器
    // 不再调用它，插件与渲染工具也都不调；tests/GlazeControllerTests.cpp 里核对它的 testClayTransitionBlend、
    // testClayCostMaskReachesStatus 已不在 runTest 里、不再运行——函数只是还编着。下面是它原来的口径。
    // t = 过渡进度 0 ～ 1（t ≤ 0 或 NaN 给 from、t ≥ 1 给 to，都是逐位原值）。
    //   - 频率在对数域混合（线性混合在低频端听感不均匀）；monoAmount、widthDb 线性；
    //   - lowCutHz = 0（关）的一端按 lowCutFloorHz 参与对数混合（两端都关 = 0）：从关到开是 floor → 目标，
    //     10 Hz 的 4 阶高通在 20 Hz 只衰减 0.02 dB，起步无感；lowCutFloorHz 不是正的有限值时用 ClayModule 的最低低切频率；
    //   - monoAmount = 0 的一端 monoHz 没有意义（GlazeTypes.h ClayPlan），按另一端的分频点参与：收单从无到有时分频点不扫动；
    //   - 对数混合的结果钳在两端之间（舍入不越过端点：端点在合法范围内，结果也就在）；
    //   - costLimitedMask（ADR-012，只给界面解释）过渡中（0 < t < 1）取目标方案 to 的；两端照上面的规则逐位原样（含各自的 mask）。
    ClayPlan blendClayPlans (const ClayPlan& from, const ClayPlan& to, float t, float lowCutFloorHz) noexcept;

    // BUG-004（ADR-018）：校准参考条件里手动参数这一部分。纯函数、不分配。T-048 阶段 2（ADR-030）起没有校准，编排器不再算
    // 参考条件、不调用它；只剩下面的 upgradeLegacyConditions 在内部调（那个函数本身也没人调了）。tests/GlazeControllerTests.cpp
    // 里核对它的用例（经 referenceFor 辅助：testReferenceSurvivesParameterRoundTrip、testManualConditionsCacheIsBitExact）已不在
    // runTest 里、不再运行。
    // 填 reference 的 conditionsFormat（新口径）、manualParamsHash（旧口径哈希，逐位同 v0.1.8.0）、discreteParamsHash 与
    // 三个对数刻度值（limitReleaseMs；clayLowCutHz / clayMonoHz 只在 Clay 非默认设置且该项「手动」时取旋钮值，否则 0），
    // 其余项不动。feedbackMaxGainDb = 约束反馈上限（它也在哈希里；阶段 2 已没有约束反馈）。
    void setManualConditions (CalibrationReference& reference, const ChainParams& manual, float feedbackMaxGainDb) noexcept;

    // 旧存档读回来的参考（conditionsFormat = legacyConditionsFormat，只有旧口径哈希）按此刻的手动参数升格：三个对数刻度值
    // 各自前后 16 个 float 内可能取到的量化码逐组代进旧口径哈希，有一组等于参考的 manualParamsHash = 同一条件（只差往返漂移），
    // 把它的手动参数部分换成 setManualConditions 算出的新口径；一组都不等 = 条件真的不同，原样保留（此后照旧只按旧口径哈希比，
    // 与修前相同）。已是新口径的、空指针跳过。候选组合超过 1024（越界的坏值；范围内最多 693）就不升格。返回升格的份数。
    // 纯函数、不分配。T-048 阶段 2（ADR-030）起旧存档整份丢弃、冷启动（restoreStateXml），编排器不再调用它；核对它的
    // testReferenceSurvivesParameterRoundTrip（tests/GlazeControllerTests.cpp）已不在 runTest 里、不再运行。
    int upgradeLegacyConditions (std::initializer_list<CalibrationReference*> references, const ChainParams& manual,
                                 float feedbackMaxGainDb) noexcept;
}
