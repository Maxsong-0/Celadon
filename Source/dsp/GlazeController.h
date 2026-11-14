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
                                      GLAZE > 0 时宿主块在预算环 10 ms 时隙边界与 OUT 表 100 ms 子块边界中较近的一个处切片
                                      （getSamplesUntilSliceBoundary，T-097；子块边界 T-108 删守门后照旧切：每片一次 endBlock，少切一刀通用档的声音就变；宿主在播、给了位置或拍点与速度时，后几片先
                                      setHostInfo 到片起点，走完再补到块尾；试听差值时另按差值缓冲长度切，与 GLAZE 无关），每片：
                                      inputMeter.process (io);
                                      controller.analyzeInput (io, inputMeter);     // 入口特征（模块处理前）
                                      balance / clay / color / limiter 处理 io；输出非有限值 → 清零 + 重启淡入（其中调 hostReset）；
                                      outputMeter.process (io);
                                      controller.endBlock (balance, inputMeter, outputMeter, stats, latency, true, &io);
                                      各片都用本块 beginBlock 给的同一份参数；endBlock 新算出的量到下一块 setParams 才交给模块。

    实际参数 = clamp(手动值 + 已施加的自动增量)（applyIncrements）；Tone / Tame / Heat / Glue 另有实时跟随，Limit Gain 另加 G_trim
    与预算环的退让（见下面「实现口径」）。GLAZE ≤ 0（含 NaN）时已施加增量、G_trim、实时跟随量与守门退让当块落到 0
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
      - 消息线程：prepare / createStateXml / restoreStateXml / captureState / stageRestore / commitRestore / keepStateOnLoad /
        takeStateRestoredBeforePrepare / setLiveSegmentSink / syncLiveConstants（encodeState 任何线程）。
      - 任何线程：getStatus（无锁快照，endBlock 每块发布一份；legacySessionDiscarded 另读一个原子标志）。
      编排器不再在两线程之间交换「已提交版本」（旧版音频线程 try-lock 发布、消息线程持锁读写的那一套随状态机删掉了）：
      存读档直接读写音频线程每块在改的量，编排器内部不加锁；其中 LiveProfile 要求自己的存读档不与音频线程接口同时调用
      （LiveProfile.h 文件头第三节）。插件里由处理器保证（T-099，ADR-040）：存读档拆成两步（见下「存读档拆两步」），碰运行状态的
      那一步（captureState / commitRestore / keepStateOnLoad）在宿主的回调锁里调、音频线程那时停在锁上，只做拷贝与交换；
      编码、压缩、解压、解析、校验与重算画像（encodeState / stageRestore）在锁外。createStateXml / restoreStateXml 两步连着做，
      给不跑音频的调用方（链的 prepare、测试、渲染工具）。
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

    目标方案：GLAZE > 0 时 makePlan 只在三种时候算——还没有目标方案；本块 beginBlock 发现 GLAZE / 风格或影响方案的
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
      - G_trim：输出侧慢修正，取代校准器。endBlock 按「方案净增益 − 实测净增益」积分（T-106 起；改前是「方案目标响度 − 出口主段
        电平」），≤ 0.05 dB/s，钳在 ±3 dB；施加时负的 G_trim 最多撤掉自动加上去的那部分。宿主旁通 / 听原声、Color 或 Limit 旁通、
        电平类自动全部撤回、还没有目标方案或读数时不积分；T-097 起（抗积分饱和，ADR-036 决定 3）另有两类冻结：预算环的
        退让量 < 0（在退、停住、回程中都算）或这一块刚改过退让量；1 s 内改过参数（风格 / GLAZE / 规划相关手动值
        与上一块不同，插件刚开的第一块不算；按处理过的时长计，存进 CONTROL 的 secondsSinceParamChange）。
        T-106（ADR-038）换误差口径，读数都来自 OutputLevelTracker::evidenceAt：
          · 实测净增益 = 出口主段电平 − 入口主段电平，两侧是跟踪器里的同一批段、同样的权重（入口样本由 analyzeInput 交给
            跟踪器，和同一片的出口样本记进同一段）。方案净增益 = Plan::effectiveGainDb。改前拿出口去比「画像的输入响度 I + 方案
            净增益」，I 是画像自己那套窗口算的（全部有效音频的门控积分），和出口历史不是同一批段。
          · 出口那一侧不用实录值，而是折算到「手动 + 这一个宿主块执行的自动增量 + G_trim、不含守门退让」的 Limit Gain 上的值
            （每段按当块执行的 Limit Gain 折回 0 dB 存着），所以 G_trim 走 δ、读数当块就变 δ。折算只在录制增益附近成立：录制
            增益离参考增益 ≤ 1 dB 的段全算，≥ 3 dB 的不算，中间线性；加权后不足 3 s 不积分。
          · 只收方案到位时录的片，没到位的也不积分：自动增量还在往目标方案走的时候，实测净增益里缺的是还没施加的那部分
            方案，不是链的偏差（Recover / Tame / Glue / Heat / Clip 差 ≤ 1 个单位、Tone 差 ≤ 0.1 dB、Limit Gain 进了死区才算到位）。
            到没到位、到位的是净增益多少的方案、执行的 Limit Gain 自动增量是多少，在 beginBlock 定一次（那一刻的已施加增量
            就是这一个宿主块执行的设置），这一块切出来的各片沿用；误差里的方案净增益、折算用的自动增量都用这一份——这一片
            刚重算的方案、刚被速率限制推了一步的增量，下一个宿主块才执行。
          · 出口历史里的段每块按实时画像此刻的参考回看重判（OutputLevelTracker::rejudge，每段记着当时输入侧的段响度与低频
            占比），只有此刻仍算主段的才计入读数：从头播时先被暂判成主段的前奏，drop 来了以后和输入侧一样不再算数。
          · 只在这一片的出口交给了跟踪器、且跟踪器正开着的段是主段时才积；积分状态不再往用不上的一侧多走（负的最多到撤光
            自动加上去的部分，正的不把 Limit Gain 顶过 24 dB；已经在外面的不往回拽）。
          · 已知近似：每片记的 Limit Gain 是交给限幅器的目标值（限幅器另有 20 ms 平滑与前瞻延迟），入口比出口早一个链延迟，
            都不对齐；到位的片上差 ≤ 0.002 dB。
        旧存档（OUTPUT_LEVEL 没有 T-106 的字段）读入时出口电平历史清空、G_trim 归 0 重新收敛。
      - 守门：预算环 GlazeGuard 出一份 ≤ 0 的退让，只退 G_trim 之后高出手动 Gain 的那部分；按 10 ms 时隙走、−3 dB 封底。
        宿主旁通 / 听原声、Color 或 Limit 旁通时冻结、退让保持（丢掉未完成的时隙）。T-097 另有落差守门 ContrastGuard 与 PSR 守门
        PsrGuard（按 OUT 表 100 ms 子块走），只在 House 开或出厂全关；T-108 删掉曲风后通用档永远不跑，两个守门连代码一起删掉
        （ADR-039，Q5 = A）。
      - 电平类自动全部撤回（Plan::automaticLevelWithdrawn）时守门不跑、G_trim 与退让都不加（Limit Gain 就是 applyIncrements 的结果）。
      - 加 G_trim、再加退让，两步各钳在 0 ～ 24 dB。

    状态（getStatus）：phase 恒为 unlearned、calibrationValid 恒为 false，requiredSeconds 填 0，其余聆听 / 校准的旧字段不写、停在
    结构默认值（GlazeTypes.h Status；.cpp publishStatus）。界面状态行（GlazeStatusText readingFromStatus）读 confidence、
    currentMainness、coldExport（离线且 c < 0.5）、guardCause / guardEngaged / protectionDb（预算环正在起作用时）、limitedByManual，
    以及 effectiveTargetLufs、resolvedStyle。planLimited（预算环退到下限
    仍超）照常发布，但状态行只认 guardEngaged、不读它（tests/GlazeStatusTextTests.cpp「planLimited does not stand in for
    guardEngaged」）；legacySessionDiscarded 由编辑器读，点亮一次旧工程提示（文字在 Source/ui）。

    工程：只存 LIVE（GLAZE_STATE formatVersion 2，LIVE 节点外层 zlib + base64）——实时画像、已施加增量（含 Limit Gain 的回差
    死区状态）、G_trim、风格权重、实时跟随量、时间线、预算环、出口主段电平、Clay 自动值 / ClayGuard 状态 / 压力反馈缩放（另给
    0.1.8.10 写两份空的 CONTRAST_GUARD / PSR_GUARD，读档时不看）。
    读到更高版本或坏档 → 冷启动；旧格式（formatVersion 1，或带 GLAZE_PROFILE / PLAN / CALIBRATION / EXECUTION）→ 整份丢弃、
    冷启动，Status::legacySessionDiscarded 让界面提示一次。读档后目标方案在第一块 GLAZE > 0 的 endBlock 重算。

    宿主 reset / 跳播 / 回绕：分析器在此刻断开短窗口，实时段落账开着的段截到此刻、就地定稿交给段接收者（插件里是
    LiveProfile；MusicAnalyzer handleDiscontinuity）；hostReset 另让预算环丢掉未完成的时隙、出口电平跟踪器清 K 加权滤波器、
    Clay 压力反馈的累计与守护回收指示清零。LIVE 里的统计不清（ADR-030 决定 6），已施加增量、G_trim、目标方案与守门的退让、
    计时也都不动。

    离线导出：与实时同一条路径，分析器把离线当作在播；c < 0.5 时 Status::coldExport 提示。先播一遍再导出才从头到位，重复导出
    不保证逐位相同（ADR-030 决定 6）。

    T-108（ADR-039）删掉曲风与交付：没有 House 档的主段模型、音色覆盖、限幅跟拍、守门开关与反相提示，也没有流媒体 / 俱乐部的
    目标响度与 −2 dBTP 封顶。主段模型只剩响度型（LiveConstants::loudness）；syncLiveConstants 在消息线程把出厂 LiveConstants
    交给 LiveProfile（CeladonChain::prepare 调；插件的 prepareToPlay 每次另经 CeladonChain::syncGlazeLiveConstants 再调一次，同规格重启也算）。

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

        // 消息线程。分配全部固定容量状态。按新规格 prepare 分析器、预算环与出口电平跟踪器，ClayGuard、压力反馈监视器的累计与
        // 守护回收监视清零；时间线归 0、目标方案作废（第一块 GLAZE > 0 的 endBlock 重算），已施加增量、G_trim、实时跟随量与守门退让
        // 落到 0（snapAutomatic）。不碰 LiveProfile（链另外 prepare 它）、自动风格权重与压力反馈缩放；卸掉 setLiveSegmentSink 挂的
        // 外部接收者，有 LiveProfile 时把分析器重新绑回它。换采样率 / 块长时链先 createStateXml、prepare 之后立刻读回
        // （CeladonChain::prepare），这些控制量照存档恢复。
        void prepare (const juce::dsp::ProcessSpec& spec);

        // ---- 音频线程 ----

        void setHostInfo (const HostInfo& info) noexcept;

        // 宿主 reset / 播放中途重置（链的 reset 与 restartWithFadeIn 调）：分析器在此刻断开短窗口（同跳播：开着的实时段截到此刻
        // 定稿），预算环丢掉未完成的时隙，出口电平跟踪器只清 K 加权滤波器，Clay 压力反馈的累计与守护回收指示清零。LIVE 里的统计
        // （实时画像、出口主段电平的段累计）不清（ADR-030 决定 6），已施加增量、G_trim、目标方案与预算环的退让也都不动。
        void hostReset() noexcept;
        int getSamplesUntilSliceBoundary() const noexcept;

        // 不连续点（setHostInfo 自己能识别跳播与回绕；这里留给调用方直接报，目前链与测试都不调它——链遇到宿主 reset 直接调
        // hostReset）。hostReset 这一种等同 hostReset()；跳播 / 回绕：分析器断开短窗口，并把开着的实时段截到此刻、定稿交给 LiveProfile
        // （同文件头「宿主 reset / 跳播 / 回绕」一段；MusicAnalyzer::handleDiscontinuity → beginDiscontinuity → liveEndEpoch）；已施加增量、
        // G_trim、目标方案与守门退让不动，也不计入探针的 transportJumps / loopWraps。
        void handleDiscontinuity (Discontinuity kind) noexcept;

        // 每块处理前调：manual = 用户在各模块旋钮上的值（含 glazePercent、glazeStyle）；GLAZE / 风格或影响方案的手动值相对上一块变了，就记下来让本块 endBlock 重算目标方案（GLAZE > 0 时）。
        // hostBypassed = 出口此刻不是母带（链传 dryPath = 宿主旁通或听原声）：endBlock 里守门与 G_trim 冻结、出口电平跟踪器不吃样本、
        // 实时跟随不走、ClayGuard 不推进时间。Listen（监听增益）在所有测点之后，不影响编排器。
        // 返回本块交给四个模块（Balance、Clay、Color、Limit）的实际参数（引用在下一次 beginBlock 之前有效）。时间只在 endBlock 里按 stats.numSamples 推进。
        const ChainParams& beginBlock (const ChainParams& manual, bool hostBypassed) noexcept;

        // 模块处理前：input = 链入口原始输入；inputMeter 已处理过这一块。
        void analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept;

        // 模块处理后：算此刻的主段度 m、出口主段电平吃样本、读实时画像、推进时间线（需要时重算目标方案）、预算环、速率限制与棘轮、
        // G_trim、实时跟随、Clay 压力反馈，最后发布状态快照（顺序与条件见 .cpp Impl::endBlock）。stats = 本块模块统计。
        // balance、chainLatencySamples、spectrumDeferred 三个参数 T-048 阶段 2 起不再使用（签名留着，链照传）：频谱帧只在
        // analyzeSpectrum 里交给分析器，endBlock 不再代交——不经链、直接驱动编排器的调用方要自己调 analyzeSpectrum。
        // inputMeter 原给 T-097 的落差守门读入口子块（T-108 随守门删掉），留着签名、不再读；outputMeter 只用来推算 100 ms 子块的
        // 切片边界（getSamplesUntilSliceBoundary）。
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

        // createStateXml() = captureState + encodeState（下面「存读档拆两步」）。
        std::unique_ptr<juce::XmlElement> createStateXml() const;

        // xml 为空、formatVersion 高于本版本、或 LIVE 读坏 → 冷启动；formatVersion 2 → 恢复 LIVE 里的实时画像与各控制量
        // （目标方案在第一块 GLAZE > 0 的 endBlock 重算）；旧格式（formatVersion 1，或带 GLAZE_PROFILE / PLAN / CALIBRATION /
        // EXECUTION）→ 整份丢弃、冷启动，并置 Status::legacySessionDiscarded；formatVersion 缺失或 ≤ 0、又没有这些旧节点的，
        // 同样冷启动、不置这个标志。不序列化滤波器 / 检测器的音频历史。
        // 第一次 prepare 之前调时另留一份原件给 takeStateRestoredBeforePrepare。
        // restoreStateXml() = stageRestore + commitRestore（下面「存读档拆两步」）。
        void restoreStateXml (const juce::XmlElement* xml);

        // 运行中的实例读档、不读 GLAZE 存档时调（T-099，ADR-040；处理器在回调锁里调，同 commitRestore）：实时画像、已施加增量与
        // 各守护状态都接着用、不冷启动，只清掉上一次读档点亮的 Status::legacySessionDiscarded——那条提示说的是那次读档冷启动了，
        // 这次没有；不清的话，读进干净的工程之后提示还一直亮着。
        void keepStateOnLoad() noexcept;

        // ---- 存读档拆两步（T-099，ADR-040）----
        // 插件在宿主的回调锁里存读档（音频线程那时停在锁上）。锁里只做拷贝与交换，编码、压缩、解压、解析、校验、重算画像都在锁外：
        //   存档 = captureState（锁里：把编码要读的量拷进事先分配好的快照，只拷不分配）
        //          + encodeState（锁外、任何线程：拼 XML、LIVE 外层 zlib + base64）；
        //   读档 = stageRestore（锁外、消息线程：解压、解析、校验，实时画像在另一份工作区里读回并重算，结果放进暂存）
        //          + commitRestore（锁里：换入暂存；坏档 / 旧格式 / 空存档时冷启动，规则同 restoreStateXml）。
        // createStateXml / restoreStateXml 就是这两对的组合：存出来的字节、读回来的状态与拆开调逐位相同。

        // 存档快照（值类型）。构造时分配全部容量（含实时画像的整个回看窗口，约 1.7 MB），在拿锁之前建好；可以反复用。
        class StateSnapshot
        {
        public:
            StateSnapshot();
            ~StateSnapshot();

            struct Data;   // 不透明（.cpp 里定义）

        private:
            std::unique_ptr<Data> data;
            friend class GlazeController;

            JUCE_DECLARE_NON_COPYABLE (StateSnapshot)
        };

        // 不与音频线程接口同时调（插件里在回调锁里）：只拷不分配、不调宿主、不拿别的锁。
        void captureState (StateSnapshot& snapshot) const noexcept;
        // 任何线程，不碰任何实例：把快照编码成 GLAZE_STATE 节点。
        static std::unique_ptr<juce::XmlElement> encodeState (const StateSnapshot& snapshot);

        // 读档暂存。构造只分配外壳；stageRestore 按需要再分配（实时画像的工作区约 2.3 MB）——都在锁外。commitRestore 之后里面是
        // 换下来的旧画像工作区与旧的「prepare 之前读进的原件」，随暂存一起在锁外释放。一份暂存只 commit 一次。
        class StagedRestore
        {
        public:
            StagedRestore();
            ~StagedRestore();

            struct Data;   // 不透明（.cpp 里定义）

        private:
            std::unique_ptr<Data> data;
            friend class GlazeController;

            JUCE_DECLARE_NON_COPYABLE (StagedRestore)
        };

        // 消息线程，可与音频线程接口同时调：只读消息线程才改的配置（是否已 prepare、采样率、实时画像的常数），不碰运行状态。
        void stageRestore (const juce::XmlElement* xml, StagedRestore& staged) const;
        // 不与音频线程接口同时调（插件里在回调锁里）：定长拷贝与指针交换，不分配、不释放、不调宿主、不拿别的锁。
        void commitRestore (StagedRestore& staged) noexcept;

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

        // 消息线程、音频已停时：把出厂 LiveConstants 交给 LiveProfile（LiveProfile::setConstants 同步重算已发布的那一份）。
        // 交的是 setLiveConstantsForTesting 设过的那一份，没设过就是出厂值（插件从不设）。
        // CeladonChain::prepare 每次都调；插件的 prepareToPlay 不论整体重建还是同规格只 reset，最后都经 CeladonChain::syncGlazeLiveConstants
        // 再调一次（T-108 之前叫 syncLiveModel，另按曲风选主段模型；那时两处也都调）。
        void syncLiveConstants() noexcept;
        bool hasPrepared() const noexcept;

        // T-113 听感批（1-05 / 1-07）此刻生效的常数：没设过就是出厂值（ListeningConstants 的默认值）。链的 setParams（音频线程）读它算
        // 推响不刺 / 目标频谱「自动」的强度；返回的引用只在下一次 setListeningConstantsForTesting 之前有效。
        const ListeningConstants& getListeningConstants() const noexcept;

        // ---- 任何线程 ----

        // 无锁快照（endBlock 每块发布）；legacySessionDiscarded 另从原子标志读。phase 恒为 unlearned、calibrationValid 恒为 false；
        // 界面读哪些字段见文件头「状态」。
        Status getStatus() const noexcept;

        // ---- 只给测试用 ----

        // 替换出厂常数（测试用）：编排器读 activeConstants() 的地方——决策器、预算环上限与时长、ClayGuard、实时跟随——都改用它；
        // LiveProfile 的 LiveConstants 不在这里（见 syncLiveConstants）。在 prepare 之前调。
        void setConstantsForTesting (const Constants& constants);
        // T-113 候选（插件不调；渲染工具 --candidate 与测试用）：换掉听感批的出厂常数。消息线程、音频已停时调（渲染工具在新链 prepare
        // 之前调）。ListeningConstants 由编排器与链读（预算环主段门、音色强度映射）；LiveConstants 立刻交给链上的 LiveProfile
        //（若已接上），之后每次 syncLiveConstants 也交这一份——链的 prepare 读档之前画像就已是这份常数，读档时还没发布的段按它进账。
        void setListeningConstantsForTesting (const ListeningConstants& constants) noexcept;
        void setLiveConstantsForTesting (const LiveConstants& constants) noexcept;
        // T-048 阶段 2 起没有「已提交版本」：按此刻拼一份——当前常数、「自动」风格权重（放在 lockedWeights）、目标方案、
        // 此刻跟随的实时画像（读到过才 hasProfile，profileId 按它算）；calibration 恒为默认值。渲染工具与测试读它。
        CommittedState getCommittedStateForTesting() const;
        // ---- v0.6 A4 追加（只给测试用；都在音频线程读，测试里与处理同一线程）----

        // 内部计数与此刻的中间量：probeForTesting 按此刻的值填 increments、targetPlan、G_trim 与预算环几项、Clay 几项、
        // 实时视图的低频相关度、offline、实时 Heat / Glue 减量、guardLimits*、G_trim 冻结与读数；编排器另外累加 clayPressureSteps、hostResets、transportJumps、loopWraps
        // （GlazeController.cpp Impl::probeForTesting 与各处 probe.）。T-098 删掉了 T-048 阶段 2（ADR-030）起不再推进、恒为初值的
        // 字段（聆听 / 提交 / 方案过渡 / 校准 / 约束反馈 / 画像失配 / 执行快照的计数与读数、旧参考条件哈希、Clay 的几项旧诊断）。
        struct TestProbe
        {
            int clayPressureSteps = 0;       // 修坯压力反馈把自动 Clay 缩放降了一档的次数（ADR-014）
            int hostResets = 0, transportJumps = 0, loopWraps = 0;   // hostReset 每次 +1；后两项只数 setHostInfo 自己识别出的跳播 / 回绕
            float appliedBiasDb = 0.0f;      // 此刻的 G_trim（输出侧慢修正；未经 applyLimitExtras 按自动部分钳制）
            float executedBiasDb = 0.0f;     // 与 appliedBiasDb 相同，都是 G_trim（渲染工具 --trace 的 executed_bias_db 列照旧读它）
            float executedGuardDb = 0.0f;    // 预算环（GlazeGuard）最近交出的退让（budgetOut）；不是钳过后的执行量
            float guardDb = 0.0f;            // 预算环（GlazeGuard）自己的 g_保护
            bool offline = false;
            float liveHeatReduction = 0.0f, liveGlueReduction = 0.0f;
            AutoIncrements increments;       // 已施加的自动增量（速率限制 / 棘轮之后）；beginBlock 另加的 Tone / Tame / Heat / Glue 实时跟随不在内
            Plan targetPlan;                 // 目标方案（最近一次 makePlan；已施加增量按速率朝它走）

            // ---- v0.1.7.0 修坯 Clay（A6 追加）----
            // clayPlanned = 最近一次 beginBlock 里目标方案的 plan.clay 经压力反馈缩放与范围收拾后的值（还没有目标方案时为中性）；
            // clayAutomaticSupported 为 false 时它照算，但交给 ClayGuard 的是中性方案。
            ClayPlan clayPlanned;            // 「自动」态值，ClayGuard 之前（口径见上两行）
            ClayPlan liveClay;               // 经 ClayGuard 之后、三态合成之前的自动值（随工程存进 LIVE 的 CLAY 节点）
            ClayGuardState clayGuard;        // ClayGuard 此刻的状态
            bool clayAutomaticSupported = false; // GLAZE > 0、已读到实时画像且画像格式 ≥ clayProfileFormatVersion（不满足时自动 Clay 走中性方案）
            float clayPressureScale = 1.0f;      // 修坯压力反馈给自动 Clay 的缩放（ADR-014；随工程保存）
            // 分析器实时视图（MusicAnalyzer ShortTermView）此刻的 40 ～ 125 Hz 低频相关度：慢路 300 ms / 快路 120 ms。ClayGuard 的反相
            // 提示在下一块 beginBlock 读的就是这两项（两次处理之间视图不变）；读探针时现取。T-098 之前这两项不再填、恒无效，渲染工具
            // --trace 的 rho_slow / rho_fast 与 slow_inverted_pct（gate_check.py 的反相豁免）因此恒空 / 恒 0。
            Feature liveLowCorrelation;
            Feature liveLowCorrelationFast;

            // ---- 预算环的约束上限（上一块 endBlock 之后的值）----
            bool guardLimitsSet = false;              // 预算环已设过上限（GlazeGuard::hasLimits；第一次重算目标方案起为 true）
            // 预算环此刻生效的约束上限 = blendConstants(当前常数, 目标方案的权重)，其中目标方案 manualBudgetMask 标出的项
            // （手动单独已超预算）置 0 = 不设约束（T-032 A11；heatDriveCap 也在里面）。掩码为 0 时与混合常数逐位相同。
            StyleConstants guardLimits;

            // ---- T-097 G_trim 冻结（上一块 endBlock 之后的值）。T-097 的落差 / PSR 守门读数随两个守门一起删掉（T-108）----
            bool trimFrozen = false;                  // 上一块 G_trim 因预算环有退让量 / 刚改过退让、或 1 s 内改过参数而没积分
            double secondsSinceParamChange = 0.0;     // 距上次改参数的秒数（封顶 1 s）

            // ---- T-106 G_trim 的读数（读探针时现算）----
            float trimLevelLufs = -100.0f;            // 出口主段电平折算到「手动 + 这一块执行的自动增量 + G_trim、不含守门退让」的 Limit Gain，按录制增益的远近加权（没有证据 = −100）
            float trimInputLufs = -100.0f;            // 同一批段、同样权重的入口主段电平；上一项减它 = 实测净增益，G_trim 的误差 = 方案净增益 − 实测净增益
            double trimEvidenceSeconds = 0.0;         // 上一项加权后的主段时长（不足 3 s 时 G_trim 不积分）
            double trimMainSeconds = 0.0;             // 出口历史里此刻计入电平的主段时长（回看重判之后、不加权，含正在开着的段）
            bool trimMainNow = false;                 // 正在开着的段此刻暂判是主段（G_trim 只在这时积分）
            bool trimPlanInPlace = false;             // 最近一次 beginBlock 记下的：这一个宿主块执行的设置是「方案到位」的（各片沿用）
            float trimPlanGainDb = 0.0f;              //   到位的那份方案的净增益（G_trim 的误差 = 它 − 实测净增益）；没到位时是 NaN
            bool haveTargetPlan = false;              // 此刻有目标方案（读档、prepare 之后到第一次重算之前没有）
            bool gainInDeadband = false;              // 此刻 Limit Gain 的自动增量停在回差死区里（「到位」的条件之一）
        };

        TestProbe getProbeForTesting() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeController)
    };
}
