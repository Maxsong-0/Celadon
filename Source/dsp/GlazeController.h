/*
  ==============================================================================

    GlazeController.h
    GLAZE 编排器（设计规格-GLAZE-时间线与保存）：状态机与三个就绪标志、候选 / 已提交版本、跳播 / 回绕 / reset /
    离线导出，每块产出交给各模块的实际参数（v0.1.7.0 起四个：Balance / Clay / Color / Limit）。由 CeladonChain 持有，
    链里每块按下面的顺序调用：

        CeladonChain::setHostInfo   → controller.setHostInfo (info);                 // 识别跳播 / 循环回绕 / 离线
        CeladonChain::setParams     → p = controller.beginBlock (manual, paused);   // 再把 p 交给四个模块（与 v0.5 同一时机，
                                                                                      //  getLatencySamples 照样立即反映 Lookahead）
        CeladonChain::process       → controller.analyzeSpectrum (balance);        // 上一块 Balance 新发布的帧（见该函数注释）
                                      inputMeter.process (io);
                                      controller.analyzeInput (io, inputMeter);     // 入口特征（模块处理前）
                                      balance / clay / color / limiter 处理 io；输出非有限值 → 清零 + 重启淡入；
                                      outputMeter.process (io);
                                      controller.endBlock (balance, inputMeter, outputMeter, stats, latency);

    实际参数 = clamp(手动值 + 自动增量)；Limit Gain 另加 校准偏差 + g_保护。GLAZE ≤ 0 时实际参数逐位等于手动值
    （自动增量、校准偏差、保护偏移全为 0，且不经任何额外运算），链输出与 v0.5 逐位相同。

    v0.1.7.0 修坯 Clay（设计规格-Clay、ADR-009；A6 接线）：Clay 不走「手动值 + 自动增量」，而是三态（自动 / 关 / 手动）：
      自动值 = 方案的 plan.clay（方案过渡中与自动增量同一进度混合：频率对数域、比例与 dB 线性）→ ClayGuard 实时保护；
      实际参数 effective.clay = resolveClayParams(用户三态设置, 保护后的自动值, GLAZE)。GLAZE ≤ 0 时也照算（「手动」照样
      生效；全「自动」时是中性，Clay 逐位直通）——原有 7 个参数在 GLAZE ≤ 0 时逐位等于手动值的保证不变。
      旧画像（格式 1）、旧算法方案、没有画像时自动 Clay 一律中性、保护不工作：旧工程重新聆听之前声音逐位不变。
      Clay 的三态设置并入校准参考条件的 manualParamsHash（默认设置时哈希与 v0.1.6.2 逐位相同，旧存档的校准照旧适用）；
      执行快照存 liveClay 与 ClayGuard 状态，恢复后第一块直接用快照值（方案源支持自动 Clay 时；否则中性，审核 V2-03）。
      ADR-012：Status::clayCostLimitedMask = 此刻生效方案里因「试过会抬高峰值」被减量 / 否决的「自动」态项（GLAZE > 0 才报）。

    线程：
      - 音频线程：setHostInfo / beginBlock / analyzeInput / endBlock / handleDiscontinuity / hostReset。0 分配、0 锁。
      - 消息线程：prepare / requestRelearn / createStateXml / restoreStateXml。
      - 任何线程：getStatus（无锁快照）。
      已提交版本在两线程之间交换：音频线程只做 try-lock 发布，拿不到下块再发，从不等待；消息线程读写时短暂持锁。
      restoreStateXml 之后、第一块音频之前调 createStateXml，返回的是刚恢复的内容（不是旧值）。

    ---- 实现口径（v0.6 A4；数值与理由见 GlazeController.cpp 开头）----

    状态：对外的 Phase = 正在聆听（首次聆听或重新聆听的候选在建）时为 listening，否则为已提交版本自己的阶段
    （unlearned → planTransition → calibrating → locked）。聆听与已提交版本分开记：重新聆听期间旧的已提交版本照常生效
    （方案、校准、保护层都照走），候选可用后整体替换并做 3 s 方案过渡。

    方案：由「方案源」（画像、锁定的风格权重、风格、标定快照、约束反馈上限）在本块的 GLAZE 与手动值下调决策器得到。
    方案切换（新画像提交、换风格、约束反馈降上限）= 从旧方案源到新方案源的线性过渡（按已处理样本，出厂 3 s），两端都随
    GLAZE / 手动值即时重算；过渡中再来的切换排队、接在当前过渡之后。GLAZE 连续控制（含宿主自动化）只让方案按确定映射
    重算，不重学、不做过渡、不重启校准，变化由各模块自己的 20 ms 平滑接住。首次聆听时方案源是候选画像，每 1 s 更新一次、
    每次 1 s 过渡（「随学习逐步放开、秒级平滑」）。GLAZE ≤ 0 时一切过渡直接落到终点、校准偏差与保护偏移归零。

    校准：参考条件 = 画像号、风格、GLAZE、影响声音的手动参数与约束反馈上限（哈希）、Ceiling、Lookahead、采样率。
    三个对数刻度的手动值（Limit Release、Clay 低切 / 收单分频点）不进精确哈希、另存值按相对容差比：它们不吸附，重开工程的
    归一化往返会漂几个 ulp（BUG-004，ADR-018；setManualConditions）。
    已提交的校准只在条件与参考一致时施加（「适用」）；不适用就只用前馈（偏差按 1 dB/s 回到 0），条件连续 1 s 不变且没有
    过渡、不在 Listen / 旁通 / 离线时才开新一轮。轮中条件变了：本块起当前子窗作废、偏差保持；变回来就接着采（已完成的
    子窗保留），3 s 没变回来就放弃本轮、以新条件为底重来。「刚对同一条件试过（结束为无效）就不反复重开」只记真正结束为无效的
    那一轮（finishRound）；放弃的轮与换方案丢掉的轮都不算「试过」，条件变回原样照样能再开（T-032 R5-01：修前校准中把 GLAZE
    拧到 0 或切 Limit Bypass 做 A/B ≥ 3 s、或换风格再切回，之后永远不再校准）。保护层在退让 / 恢复、或编排器接着施加的
    退让（约束反馈吸收、解除执行快照冻结时的淡出）还没淡完时也不开（T-032 R5-04）。

    约束反馈（PHASE2-NOTES 设计决定 1）：Glue 均值 / p95、Heat 在 3 s 窗内累计超出 ≥ 1 s，或保护层新进入「方案受限」，
    或校准结束为「预算受限」→ 以当前实际净提升为基础下调约束反馈上限（每次 1 dB；预算受限时取当时净提升），下限是手动链
    自己的净增益，重算方案并走 3 s 过渡；两次之间至少隔一个过渡 + 3 s 观察（按有效音乐时间计，静音 / 停播 / 旁通不走，T-032）。
    「锁定后保护层卡住」同样按保护层自己的时间轴判定。上限只降不升（收敛、不振荡），随已提交
    版本保存；重新聆听提交与换风格时回到不限。上限在参考条件里：降 1 dB 的反馈（方案受限、上游超限）让已提交的校准失效、
    过渡后重测；预算受限与锁定后保护层卡住这两种「吸收」（不额外减、净增益连续）把适用的校准改绑到新上限、继续适用，
    不重测（BUG-003：修前每次吸收都重开一轮，又推回保护层，整首歌停在「校准中」）。

    手动设置（T-032 R6-02 收尾，A11）：保护层、上游监视、校准偏差与校准轮、约束反馈、画像失配只在电平类自动全部撤回
    （Plan::automaticLevelWithdrawn：手动已让限幅器超约束、Limit 旁通、上限低于手动链净增益）时停。手动只让 Heat / Glue / Clip
    某一项超出风格预算时照常工作，但保护层与上游监视只核对自动部分：那一项（Plan::manualBudgetMask）不设约束，其余照风格上限；
    这组项变了就清空两个窗口。界面「受手动设置限制」仍看 limitedByManual（任一模块被撤回）。

    T-038 House 档（设计规格-曲风-House学习方式 第 3、4、7、9 节；只在曲风为 House 子类时走，通用档逐位同改前；数值与理由见 .cpp 开头）：
      - 分析器每块跟着此刻的曲风（MusicAnalyzer::setGenre），House 档的候选画像由段落账的满编段门控算出；
      - 锁定：House 档提交候选要三条都满足——不重复的已听小节 ≥ lockSegments（2 秒段：已听有效音频 ≥ fallbackLockSeconds）、满编段
        ≥ requiredFullSegments 小节（2 秒段 ≥ fallbackFullSeconds 秒）、现有的置信度条件（listeningComplete）；
      - 暂定方案（首次聆听）：每次段落重新门控后按当前的满编段重算，一般 3 s 过渡、新响段进来 fastTransitionSeconds 快过渡（正在走的
        过渡按进度连续地加快走完）；暂定期不开校准轮（没有已提交画像，本来就不校准）；
      - 锁定后防漏网：连续 reopenSegments 段「段响度 ≥ 画像 I + reopenAboveProfileLu、低频占比 ≥ 画像 lowShare × lowGateFactor」→ 按段落账
        门控重算画像（更响才换），快过渡换过去、重新校准两轮；每份画像最多重开 maxReopens 次；段落账不够锁定条件（例如刚读档）时
        旧画像照常、等够了再换；现有 10 LU 画像失配保护照旧；
      - 换曲风：通用 → House 时正在聆听就从此刻重新开始这一轮；已提交画像不是 House 锁定过的（格式 1 / 2、通用档学的）就开一轮
        聆听、旧画像照常生效（不清空）；House → 通用时为 House 开的那一轮取消。「重新聆听」清空段落账；读取工程后段落账从空开始；
      - 状态：Status 的 house* 各项；反相提示每块交 ClayGuard::updateHouseHint（资格 = 此刻这一段是满编段，时长 = phaseHintSegments 小节）。

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
    class GlazeController
    {
    public:
        GlazeController();
        ~GlazeController();

        // 消息线程。分配全部固定容量状态。采样率改变：保留画像与风格权重，核对频带定义，
        // 校准标「待验证」（calibrationValid = false），稳定后自动再校准，不重听。
        void prepare (const juce::dsp::ProcessSpec& spec);

        // ---- 音频线程 ----

        void setHostInfo (const HostInfo& info) noexcept;

        // 宿主 reset / 播放中途重置：清运行状态（分析窗口、校准当前子窗、保护偏移归 0、平滑器到目标），
        // 保留已提交的画像与方案；正在聆听时已累计的有效时长保留。
        void hostReset() noexcept;
        int getGuardSamplesUntilBoundary() const noexcept;

        // 不连续点（setHostInfo 自己能识别跳播与回绕；这里给链与测试直接调用）。
        void handleDiscontinuity (Discontinuity kind) noexcept;

        // 每块处理前调：manual = 用户在各模块旋钮上的值（含 glazePercent、glazeStyle）；
        // hostBypassed = 宿主旁通（校准暂停、当前子窗作废）。Listen 位于测点之后，不暂停校准或频谱跟随。
        // 返回本块交给四个模块（Balance、Clay、Color、Limit）的实际参数（引用在下一次 beginBlock 之前有效）。时间只在 endBlock 里按 stats.numSamples 推进。
        const ChainParams& beginBlock (const ChainParams& manual, bool hostBypassed) noexcept;

        // 模块处理前：input = 链入口原始输入；inputMeter 已处理过这一块。
        void analyzeInput (const juce::AudioBuffer<float>& input, const LoudnessMeter& inputMeter) noexcept;

        // 模块处理后：频谱证据、校准子窗、保护层、状态机推进、状态快照发布。
        // stats = 本块模块统计；chainLatencySamples = 本块整条链的延迟（校准对齐输入输出用）。
        // 这一块若没调 analyzeSpectrum（不经链、直接驱动编排器的调用方），频谱证据在这里处理。
        // 链只在宿主块开头交频谱帧；其内部统计切片传 spectrumDeferred=true，避免切片边界影响证据交付。
        void endBlock (const BalanceModule& balance, const LoudnessMeter& inputMeter, const LoudnessMeter& outputMeter,
                       const ModuleStats& stats, int chainLatencySamples, bool spectrumDeferred = false) noexcept;

        // v0.6 A4 追加。块开头、IN 表处理本块之前调：把上一块 Balance 新发布的帧交给分析器（频谱证据、斜率、占比）。
        // 为什么不在上一块的 endBlock 里做：Balance 每 hop 最后一片分析与帧的发布在同一块，分析器的逐帧统计若也挤进这一块，
        // 就叠在整条链的结构性最坏块上（192 kHz / 块 32 实测 +0.76 µs，一半预算）。挪到下一块开头，分析器看到的调用顺序与参数
        // 完全不变（processInput(k) → processSpectrum(k) → processInput(k+1)，两边的累计样本数也与原位置相同），只是画像晚一块
        // 用上这一块的帧。音频线程；0 分配。
        void analyzeSpectrum (const BalanceModule& balance) noexcept;

        // ---- 消息线程 ----

        // 「重新聆听」按钮：未在重学 → 开始建候选版本；正在重学 → 取消（旧的已提交版本不受影响）。只置原子标志。
        // （首次自动聆听期间点它 = 从 0 重新开始这一轮聆听，不是取消：PHASE2-NOTES 设计决定 2。）
        void requestRelearn() noexcept;

        std::unique_ptr<juce::XmlElement> createStateXml() const;

        // xml 为空或正式状态读取失败 → 未学习；有效 EXECUTION 独立恢复当前执行量。
        // 临时画像仍未正式学习完成，重新积累证据期间保持已听到的执行方案；不序列化滤波器/检测器音频历史。
        void restoreStateXml (const juce::XmlElement* xml);

        // ---- 任何线程 ----

        Status getStatus() const noexcept;

        // ---- 只给测试用 ----

        // 替换出厂常数（测试用短时长常数，免得每个用例都要跑满 15 s）。在 prepare 之前调。
        void setConstantsForTesting (const Constants& constants);
        CommittedState getCommittedStateForTesting() const;
        Phase getPhaseForTesting() const noexcept;

        // ---- v0.6 A4 追加（只给测试用；都在音频线程读，测试里与处理同一线程）----

        // 内部计数与此刻的中间量：状态转移、过渡、校准、约束反馈是否发生过、发生了几次。
        struct TestProbe
        {
            int learningStarts = 0;          // 分析器 startLearning 的次数（首次聆听、重新聆听、首次聆听重来）
            int commits = 0;                 // 候选画像提交为已提交版本的次数
            int planTransitions = 0;         // 方案过渡（3 s 那种：提交、换风格、约束反馈、旧算法方案改按新算法）开始的次数
            int learningUpdates = 0;         // 首次聆听中低置信度方案按候选画像更新的次数
            int calibrationBegins = 0;       // 校准器 begin 的次数
            int calibrationFinishes = 0;
            int feedbackSteps = 0;           // 约束反馈真正下调了上限的次数
            int clayPressureSteps = 0;       // 修坯压力反馈把自动 Clay 缩放降了一档的次数（ADR-014）
            int feedbackFromBudget = 0;      // 触发来源：校准结束为「预算受限」
            int feedbackFromGuardLimit = 0;  // 触发来源：保护层进入「方案受限」
            int feedbackFromUpstream = 0;    // 触发来源：Glue / Heat 在 3 s 窗内累计超出 ≥ 1 s
            int feedbackFromStuckGuard = 0;  // 触发来源：锁定且校准适用时保护层卡在同一退让值上（不会再开校准轮来吸收它）
            int mismatchSteps = 0;           // 画像失配：按「这一段当画像」重算的方案给净增益上限的次数（v0.6 验收 F-01）
            int hostResets = 0, transportJumps = 0, loopWraps = 0;
            bool transitionActive = false;
            float transitionProgress = 1.0f; // 0 ～ 1
            bool learning = false;           // 分析器正在累计候选
            bool relearning = false;
            int calibratorState = 0;         // GlazeCalibrator::State 的整数值
            float appliedBiasDb = 0.0f;      // 此刻施加的校准偏差（平滑后、未按手动值钳制）
            float executedBiasDb = 0.0f;     // 上一块实际执行的校准偏差（交给校准器的回馈量）
            float executedGuardDb = 0.0f;    // 上一块实际执行的保护偏移
            float guardDb = 0.0f;            // 保护层自己的 g_保护
            float feedbackMaxGainDb = 0.0f;  // 约束反馈上限（不限时是一个很大的数）
            bool savedPlanMode = false;      // 照用保存的旧算法方案
            bool executionSnapshotActive = false; // 同算法执行快照（不等同正式画像/有效校准）
            bool offline = false;
            float liveHeatReduction = 0.0f, liveGlueReduction = 0.0f;
            AutoIncrements increments;       // 上一块交给模块的自动增量（过渡混合后）
            Plan targetPlan;                 // 目标方案（过渡终点）

            // ---- v0.1.7.0 修坯 Clay（A6 追加）----
            ClayPlan clayPlanned;            // 上一块方案（含过渡混合与压力反馈缩放）给的「自动」态值（保护之前；方案源不支持 Clay 时为中性）
            ClayPlan liveClay;               // 上一块经 ClayGuard 之后、三态合成之前的自动值（执行快照存的就是它）
            ClayGuardState clayGuard;        // ClayGuard 此刻的状态
            float clayGuardSeconds = 0.0f;   // 上一块交给 ClayGuard 推进的有效音频秒数（静音 / 宿主旁通 / 停播块 = 0）
            bool clayAutomaticSupported = false; // 上一块的方案源支持自动 Clay（画像格式 ≥ 2、当前算法方案）
            bool clayRestoredFirstBlock = false; // 上一块直接用了执行快照里的 Clay 值（保存重开 / 宿主 reset 后第一块）
            std::uint32_t manualParamsHash = 0;  // 按上一块手动参数算的校准参考条件哈希（含 Clay 三态设置；旧口径，见 setManualConditions）
            int legacyConditionsUpgrades = 0;    // 读取工程后旧存档参考升成新口径的份数（BUG-004，upgradeLegacyConditions）
            Feature liveLowCorrelation;          // 分析器实时视图的低频相关度慢路 / 快路（读取此刻的读数：处理完这一块之后，ClayGuard 下一块才用它；诊断追踪用）
            Feature liveLowCorrelationFast;
            float clayPressureScale = 1.0f;      // 修坯压力反馈给自动 Clay 的缩放（ADR-014；随工程保存）
            double clayPeakCostDb = 0.0;         // 压力反馈第一触发量此刻的代价（dB，与先试同口径，ADR-028；有效块未满 10 s 为 0）
            int clayPeakCostBlocks = 0;          // 自上次清零起攒下的有效峰值块数（每块 0.1 s）
            double clayBlendLossSmoothedDb = 0.0;   // 守护混合损失的平滑值（dB，T-043：ClayPressureMonitor::blendLossDb）
            double clayGuardKeepChange = 0.0;       // 守护回收指示（T-074，ClayGuardKeepMonitor）：平滑后的 Σd² 与 Σ(g·d)²、
            double clayGuardKeepKept = 0.0;         // 灭着时保留比连续低于亮灯线的秒数（诊断）
            double clayGuardKeepBelowSeconds = 0.0;

            // ---- T-015（BUG-003）校准诊断：渲染工具 --trace 的校准列用（tools/render/README.md）----
            bool roundActive = false;                 // 校准轮进行中（阶段「校准中」）
            bool calibrationApplicable = false;       // 已提交的校准对此刻适用（= Status::calibrationValid）
            int calibrationRoundsDone = 0;            // 本次 begin 以来已完成的轮数（GlazeCalibrator::Progress::round）
            int calibrationSubWindows = 0;            // 本轮已完成、条件一致的子窗数（每轮 4 个）
            double calibrationSubWindowSeconds = 0.0; // 当前子窗已采到的有效配对时长（静音不推进）
            double calibrationGuardSeconds = 0.0;     // 本次 begin 以来保护层介入期间累计的有效音乐（到窗长即「预算受限」）
            float calibrationLastErrorDb = 0.0f;      // 最近一轮的修正量 e = 目标净提升 − 实测净提升
            float calibrationSuggestedDb = 0.0f;      // 校准器此刻的建议偏差（相对本轮基线）
            // 轮中本块作废当前子窗的原因（位或，上一块）：1 宿主旁通 / 暂停、2 GLAZE ≤ 0、4 方案过渡中、8 条件与本轮参考不一致、
            // 16 电平类自动全部撤回（Plan::automaticLevelWithdrawn；T-032 R6-02 收尾前是「受手动设置限制」）、32 保护层介入
            // （GlazeGuard::isEngaged）、64 House 档此刻不在主段落（T-038 评审 F1：House 门控画像只代表满编段，校准只在主段落采样）。
            // 偏差变了（换工作点）由校准器自己当作中断，看执行偏差列。
            int calibrationInterruptMask = 0;
            // 不在轮中时没开新一轮的原因（位或，上一块）：1 没有已提交画像、2 GLAZE ≤ 0、4 宿主旁通 / 暂停、8 方案过渡中、
            // 16 电平类自动全部撤回（Plan::automaticLevelWithdrawn；R6-02 收尾前是「受手动设置限制」）、32 保护层在退让或恢复（未稳定
            // 也未卡住），或编排器接着施加的退让（carry）还没淡完、64 参考条件未知、
            // 128 已提交的校准适用（不需要）、256 用户条件稳定不足 1 s、512 刚对同一条件试过而结束为无效（T-032 R5-01 起放弃的轮、
            // 换方案丢掉的轮不算）、1024 House 档此刻不在主段落或还读不到（T-038 评审 F1）。0 = 本块开了新一轮或正在轮中。
            int calibrationGateMask = 0;

            // ---- T-032（R5-02 / R5-03 / R5-04 / R5-08）：保护层与约束反馈的运行量（上一块 endBlock 之后的值）----
            bool guardLimitsSet = false;              // 保护层已按某份方案设过上限（GlazeGuard::setLimits）
            // 保护层此刻生效的约束上限 = blendConstants(目标方案源的常数, 目标方案的权重)，其中目标方案 manualBudgetMask 标出的项（手动单独
            // 已超预算）置 0 = 不设约束（T-032 A11；上游监视用同一份，heatDriveCap 也在里面）。掩码为 0 时与混合常数逐位相同。
            StyleConstants guardLimits;
            std::int64_t guardSteadySamples = 0;      // g_保护 连续没变的有效音乐样本数（静音 / 停播 / 旁通冻结时不推进也不清）
            std::int64_t guardStuckSamples = 0;       // 「卡住」门槛：activeConstants().guardRecoverAfterSeconds + 1 s 折算的样本数
            std::int64_t samplesSinceFeedback = 0;    // 上一次约束反馈以来的有效音乐样本数（「过渡 + 3 s 观察」按它计）
            int monitorWindowSlots = 0;               // 上游监视窗的时隙数（activeConstants().guardWindowSeconds / 100 ms）
            int monitorTriggerSlots = 0;              // 上游监视的触发时隙数（guardTriggerSeconds / 100 ms）
            float absorbedGuardDb = 0.0f;             // 沿用执行快照时并进冻结方案的保护退让（≤ 0）；解除冻结时改由 carry 淡出
            float guardCarryDb = 0.0f;                // 此刻由编排器接着施加的退让（lowerGainCap 吸收 / 解除冻结的 carry 剩余量）
            std::int64_t mismatchSamples = 0;         // 画像失配此刻累计的有效音乐样本（M 在回差线以上；满持续时长才可能触发）

            // ---- T-038 House 档（上一块 endBlock 之后的值）----
            int houseListenStarts = 0;                // 为 House 锁定条件开的聆听（通用 → House、旧画像在 House 档）
            int houseUpdates = 0;                     // 首次聆听中暂定方案按新的门控结果更新的次数
            int houseFastTransitions = 0;             // 其中新响段触发的快过渡次数
            int houseReopens = 0;                     // 锁定后防漏网重开（按段落账门控重算并提交）的次数
            int houseReopenRun = 0;                   // 锁定后连续「更响更满」的段数
            bool houseReopenArmed = false;            // 已满足重开条件，等段落账够锁定条件
            std::int64_t transitionLengthSamples = 0; // 正在进行的方案过渡的长度（样本；没在过渡为 0）
        };

        TestProbe getProbeForTesting() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlazeController)
    };

    // v0.1.7.0 修坯 Clay（A6）：方案过渡中 Clay 自动值的混合（编排器每块用；纯函数、不分配，公开给测试核对口径）。
    // t = 过渡进度 0 ～ 1（与自动增量同一个 t；t ≤ 0 或 NaN 给 from、t ≥ 1 给 to，都是逐位原值）。
    //   - 频率在对数域混合（线性混合在低频端听感不均匀）；monoAmount、widthDb 线性；
    //   - lowCutHz = 0（关）的一端按 lowCutFloorHz 参与对数混合（两端都关 = 0）：从关到开是 floor → 目标，
    //     10 Hz 的 4 阶高通在 20 Hz 只衰减 0.02 dB，起步无感；lowCutFloorHz 不是正的有限值时用 ClayModule 的最低低切频率；
    //   - monoAmount = 0 的一端 monoHz 没有意义（GlazeTypes.h ClayPlan），按另一端的分频点参与：收单从无到有时分频点不扫动；
    //   - 对数混合的结果钳在两端之间（舍入不越过端点：端点在合法范围内，结果也就在）；
    //   - costLimitedMask（ADR-012，只给界面解释）过渡中（0 < t < 1）取目标方案 to 的；两端照上面的规则逐位原样（含各自的 mask）。
    ClayPlan blendClayPlans (const ClayPlan& from, const ClayPlan& to, float t, float lowCutFloorHz) noexcept;

    // BUG-004（ADR-018）：校准参考条件里手动参数这一部分。纯函数、不分配（编排器每次算参考条件都用；公开给测试核对口径）。
    // 填 reference 的 conditionsFormat（新口径）、manualParamsHash（旧口径哈希，逐位同 v0.1.8.0）、discreteParamsHash 与
    // 三个对数刻度值（limitReleaseMs；clayLowCutHz / clayMonoHz 只在 Clay 非默认设置且该项「手动」时取旋钮值，否则 0），
    // 其余项不动。feedbackMaxGainDb = 约束反馈上限（它也在哈希里）。
    void setManualConditions (CalibrationReference& reference, const ChainParams& manual, float feedbackMaxGainDb) noexcept;

    // 旧存档读回来的参考（conditionsFormat = legacyConditionsFormat，只有旧口径哈希）按此刻的手动参数升格：三个对数刻度值
    // 各自前后 16 个 float 内可能取到的量化码逐组代进旧口径哈希，有一组等于参考的 manualParamsHash = 同一条件（只差往返漂移），
    // 把它的手动参数部分换成 setManualConditions 算出的新口径；一组都不等 = 条件真的不同，原样保留（此后照旧只按旧口径哈希比，
    // 与修前相同）。已是新口径的、空指针跳过。候选组合超过 1024（越界的坏值；范围内最多 693）就不升格。返回升格的份数。
    // 纯函数、不分配；音频线程上只在读取工程后的第一块调一次（另在 prepare 沿用已听到的执行快照前对那份副本调）。
    int upgradeLegacyConditions (std::initializer_list<CalibrationReference*> references, const ChainParams& manual,
                                 float feedbackMaxGainDb) noexcept;
}
