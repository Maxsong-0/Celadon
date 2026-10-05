# T-048 阶段 2 · 接口草图

基线：`main` `8bf09e4`（PR #54，阶段 1 收口）。本文件只定接线，不开编排器实现，不合并。

权威顺序：vault 计划 §2 与遗留 1～10、`设计规格-实时GLAZE` 及其子规格（摘录见本目录开工输入）、ADR-030。库内没有 ADR-030 全文，也没有另一份接口草图；阶段 1 的 `BRIEF.md` / `CLOSEOUT.md` 只说明「实时路径默认关、声音不变」。摘录与阶段 1 代码不一致时，下面标成 **【待 Max】**，不在草图里偷偷改口径。

阶段 2 要做的事（规格 §9）：删聆听 / 锁定 / 校准状态机，决策器继续当纯函数，每 1 s 吃实时画像，增量按速率限制与棘轮走向新目标，输出修正与守护族闭环，工程改存 `LIVE`。插件声音会变。曲风从 4 项扩到 13 项、延迟改收单，都不是本阶段（曲风规格 §4；分期表第 2 行没有延迟）。

## 1. 听写锁：删哪些，界面留哪些

「聆听 / 锁定」专指 GLAZE 的学习期与已提交画像。等响、听原声是另一条路，留着。

### 删（产品路径）

| 现在 | 阶段 2 |
|---|---|
| `Phase`：`unlearned` / `listening` / `planTransition` / `calibrating` / `locked` | 不再当状态机。界面只看第 7 节那七条，避免文案和枚举各说各的 |
| `requestRelearn`、`CeladonChain::requestGlazeRelearn`、编辑器 `setGlazeRelearnAction` | 删。没有「重新聆听」，也没有取消聆听 |
| `MusicAnalyzer::startLearning` / `stopLearning`、候选画像当方案源、先试后用只在学习区间跑 | 删学习。候选画像不再存在 |
| House 锁定三条、暂定方案、锁定后防漏网（`tryHouseReopen`，含 T-049 重开时沿用混合损失）、只在主段落才校准 | 随锁定删。`houseGate.valid` 不再当「锁定过」 |
| `GlazeCalibrator`、校准轮、3 s 方案过渡、约束反馈下调已提交方案、画像失配 | 删。误差改由 `G_trim` + 守护族吸收 |
| 状态行「聆听中 x / y s」「已锁定 / 待校准 / 校准中」「与聆听时的音乐差很多」「沿用旧版方案」、House「已听 n / 64 小节」、橙点、重新聆听按钮 | 删。`unlearnedOfflineWarning` 换成下面的导出冷启动提示 |
| 工程节点 `GLAZE_PROFILE` / `PLAN` / `CALIBRATION` / `EXECUTION` | 读到就丢 |

`GlazeStatusText::Kind` 里 `listening`、`calibrating`、`calibrationPaused`、`awaitingCalibration`、`locked`、`profileMismatch`、`houseListening`、`unlearned`、`unlearnedExported` 删除。`classify` 的 `listenOn` 随校准删掉（它只为了「试听不暂停校准」）。

### 留（界面与实时路径）

- **等响**与 **听原声**。`CeladonChain::hasListenDifference` / `getListenDifferenceDb`、文案 `listeningOriginal`（「正在听原声」）是 A/B，不是学习。按钮留在原处。
- 状态行第 1 条「受手动设置限制」、House **反相提示**（只亮灯，ADR-015 起不自动收单）、Clay 峰值守护 / 宽度灯、频谱跟随。
- 风格、曲风、交付、GLAZE 旋钮。GLAZE ≤ 0 时交给模块的参数仍逐位等于手动值，状态行只写「GLAZE 0 · 只用手动设置」。
- `ShortTermView`、实时立体声两路相关度（ClayGuard 还读）、Balance、各模块 DSP、响度表、T-045 预算数、交付三档、限幅跟拍、ClayGuard 与压力反馈。
- `GlazePlanner::makePlan` 仍是纯函数。入口改吃实时画像；`planClay` 的峰值代价与混合损失改读实时试算（`liveTrial*` / `liveBlend*`，学习删掉之后并成一组，见遗留 1、7）。代价斜坡 0.2～0.6 dB、混合损失封顶 0.09 dB 不变。
- 界面「Reset」只清两块响度表，不清 `LIVE`。

规格 §7 的七条，取第一条成立的。琥珀优先于梅子青，梅子青优先于灰青：

1. 琥珀「受手动设置限制」
2. 琥珀 House 反相提示（现状文案）
3. 琥珀「导出时主段统计还没热，第一个 drop 可能偏轻：先播放一遍再导出」（`c < 0.5` 且宿主离线；取代 `unlearnedOfflineWarning`）
4. 琥珀「受限 · 预算 / 交付峰值 / 保对比 · 已退 1.5 dB」（守护族退到底，或仍在退）
5. 梅子青「起步 · 还没听到主段落（偏保守）」（`c < 0.5`）
6. 灰青「保持 · 本段较轻」（此刻主段度 `m < 0.5`，增益被棘轮按住）
7. 灰青「跟随 · 自动→温润 · 目标 −9.6 LUFS」（House 档前缀「House ·」）

旧工程读档后另提示一次「旧的聆听结果已丢弃，声音会变」。这一次不写入 `LIVE`，只活在本次打开的界面里。**【待 Max Q1】**

## 2. LiveProfile 怎样连续喂编排器

阶段 1 已经把实时模式做成可开关的旁路：插件不调 `setLiveSegmentSink`，出声不读 `LiveProfile`。阶段 2 把这条旁路改成插件唯一的画像来源。`LiveProfile` 的发布语义（回看重判、`advanceTo`、分片、坏段上界）不重写。

数据路：

```
入口样本 / IN 表 / Balance 帧
        │  MusicAnalyzer 实时段落账（所有曲风，不再另挂 House 门控账）
        ▼
LiveProfile::segmentFinalized   （只入队）
LiveProfile::advanceTo          （分片重算，到点发布）
        │  已发布的 MusicProfile + LiveDiagnostics（c、S_ref、I）
        │  每 1 s 分析器时间取一次
        ▼
makePlan(PlanInputs)            （纯函数，约 1 µs，不分配）
        │  目标增量
        ▼
速率限制 + 棘轮 → 自动增量
        │
OUT 表 / 出口短时能量 → G_trim（≤ 0.05 dB/s，取代校准器）
模块统计 → min(预算环, 落差守门, PSR 守门) → 加在 Limit Gain 上
```

接线约定（按阶段 1 公开接口，不新造发布规则）：

- **谁持有**。`CeladonChain` 持有一份 `LiveProfile`，`prepare` 时与 `MusicAnalyzer` 用同一 `ProcessSpec::sampleRate`。分析器 `setLiveMode(true)` + `setSegmentSink(&liveProfile)` 在插件路径上常开。渲染工具今天自己 `new LiveProfile` 再 `setLiveSegmentSink`；阶段 2 改为读链里这一份的 `readCommits` / `getDiagnostics`，避免两份画像。**【待 Max Q2】**
- **采样率**。规格主段 §5：实时入口按 `LiveProfile` 自己的采样率折算段长上界。两边采样率不一致时，整小节会被当成坏段。`prepare` 会冷启动，所以换采样率的顺序是：音频已停 → 消息线程先 `createStateXml` 拿到内存里的 `LIVE` → 两边 `prepare` → 立刻 `restoreStateXml`。宿主 reset / 停播 / 跳播 / 回绕 **不** 调 `LiveProfile::reset()`。
- **1 s 网格**。用分析器时间轴上的绝对样本（与 `advanceTo` 同一条轴），`round(sampleRate)` 样本一格，和宿主块长无关。到格才读 `getProfile()`，不在每次 `livePublished` 上调决策器（一小节可能发布一次，124 BPM 约 0.5 s）。读到的是已经发布的那一份；发布延迟仍是 `publishDelaySeconds`（出厂 0.5 s）。
- **两种变化分开走**（计划表里的原话，摘录没有定义）。本草图的读法：画像只在 1 s 格上改决策器输入；GLAZE、手动参数、风格、曲风、交付变了，就在本块开头用 **当前已发布画像** 立刻重算目标（`makePlan` 不分配）。施加侧共用一套速率限制，不恢复 3 s 过渡。**【待 Max Q3】**
- **冷启动**。没有任何统计时 `c = 0`，`makePlan` 走现有的低置信度保守方案（空画像指针今天就是这条）。`c` 随主段质量上升。`breakdown` / `build` 由棘轮按住，不靠「重新聆听」。
- **此刻这一段的 m**。棘轮与状态行第 6 条用 `LiveProfile::mainnessFor`，输入取实时段落账正在开着的那一段的暂判读数（`SegmentLedger::getCurrentReading`）。没有参考时 `mainnessFor` 以这份读数自己为参考。这是音频线程上的 const 调用。
- **换曲风**。`mainSectionModelFor(genre)` 变了才需要 `LiveProfile::setConstants`。该函数会同步重算整窗（零点几毫秒），并且规定不与 `segmentFinalized` / `advanceTo` 同时调。48 kHz / 32 一块只有 667 µs，同步重算放不进最慢块门槛。**【待 Max Q4】**
- **软遗忘**。不在编排器里再写一套。阶段 1 的 `LiveConstants` 已是 6 LU / 20 s / ×0.2，并且多了 Max 选过的「轻的那一支要电平稳」（`newProgramRangeLu = 2`）。规格 §6 那一句更短，没有这道起伏门。**【待 Max Q5】**
- **试算**。低切 20 Hz、加宽、两个混合损失守护（10 / 20 Hz）一直跑。四个收单候选删掉。`MusicProfile::monoPeakCostDb[4]` 本阶段留着，停算之后保持 `valid = false`（格式 2，不填 `houseGate`）。**【待 Max Q6】**

`PlanInputs` 阶段 2 的用法：`profile` 指向已发布画像；`lockedWeights` 不再表示「锁定那一次」。自动风格权重改成慢状态，放在 `LIVE` 里，作为 `lockedWeights` 的输入（决策器函数签名可以不改）。置信度缩放继续用 `confidenceScale`，读画像里的 `representativeness`（实时画像把它设成 `c`）。

## 3. 线程

总约束不变：音频线程 0 分配、0 锁；任意切块逐位相同；不用 fast-math、不依赖 FMA。

| 调用 | 线程 | 阶段 2 |
|---|---|---|
| `segmentFinalized` | 音频 | 只入队，O(1)。保持 |
| `advanceTo` | 音频 | 每网格点最多一片，片预算 `sliceBudgetNanoseconds`（8000 ns 的估计值）。保持。编排器不在这里做决策 |
| `mainnessFor` / `getProfile` / `getDiagnostics` | 音频 | 1 s 格与棘轮读已发布副本。返回的 `MusicProfile` 引用在下一次发布 / `reset` / `setConstants` 之前有效，本块内用完 |
| `makePlan` / 速率限制 / 棘轮 / `G_trim` / 三个守门 | 音频 | 新增。只用定长状态，不分配、不加锁。`makePlan` 现约 1 µs |
| `GlazeGuard::process` 等 | 音频 | 保持。三个守门的退让取最小（更负的那个），加在 Limit Gain 上，与今天的 `g_保护` 同一落点 |
| `getStatus` | 任意 | 保持无锁快照。每块 `endBlock` 发布 |
| `prepare` | 消息，且不与 `processBlock` 重叠 | 分析器与 `LiveProfile` 同一采样率。`prepare` 冷启动之后立刻读回内存里的 `LIVE` |
| `createStateXml` / `restoreStateXml` | 消息 | 含 zlib。禁止在音频线程做 |
| `setConstants` | 见 Q4 | 今天写明是消息线程。换曲风若改到音频线程，必须先改这句契约 |
| 换歌、分片、发布 | 音频，`LiveProfile` 内部 | 编排器不插第二套 |

从音频线程挪走的，是今天靠 try-lock 在两线程之间交换的「已提交版本」：提交、过渡排队、校准参考条件、约束反馈上限。这些结构随状态机删除。消息线程只剩 `prepare` 与存档。界面读 `getStatus`，不再向音频线程发「重新聆听」原子标志。

`PublicationListener` 仍是音频线程回调（0 分配）。插件不设监听者；渲染工具若要逐份快照，继续用它，但对象必须是链内那一份。

## 4. 状态机 → 连续调度

今天对外 `Phase` 在「正在聆听」与已提交阶段之间切换；重新聆听时旧方案照常，候选就绪后 3 s 过渡。阶段 2 没有候选，也没有已提交方案。

每一块音频线程只做这些事：

1. `beginBlock`：若旋钮 / 风格 / 曲风 / 交付相对上一块有变，用已发布画像重算目标。GLAZE ≤ 0：目标增量全 0，速率限制器与 `G_trim`、守护退让在本块直接落到 0，交给模块的参数逐位等于手动值。
2. 分析（实时账 → `LiveProfile`）。这一步在 `analyzeInput` / `analyzeSpectrum`，与今天的调用顺序相同。
3. 若分析器时间跨过 1 s 格：用新发布的画像重算目标。同一块里若第 1 步已经因旋钮重算过，以 **本块最新的画像 + 本块旋钮** 再算一次，仍只这一次 `makePlan`。
4. 速率限制把已施加增量推向目标。棘轮：此刻 `m < 0.5` 时，净增益只许下降、不许上升。草图把棘轮限在增益上，不含 Glue / Heat / Clip / Tone。**【待 Max Q7】**
5. `G_trim` 按出口主段电平与目标的差积分。预算环正在退让时冻结积分（抗饱和）。
6. 三个守门各出一份 ≤ 0 dB 的退让，取最小，加到 Limit Gain。真峰值仍由限幅器与 Ceiling 硬保证。
7. 写 `Status` 快照。

速率数字摘录没写。T-047 汇总的起点（尚未在本规格摘录里复述）：增益上行 0.2 dB/s、下行 1 dB/s，死区进 0.3 / 出 0.1 dB；`G_trim` ≤ 0.05 dB/s，钳 ±3 dB。守护族起点：预算环退 0.3～6 dB/s、回 0.15～0.3 dB/s；落差 `(P90−P25)` 输入减输出超过 1.2 LU（俱乐部 1.5）时以 ≤ 0.1 dB/s 把退让收回；3 s PSR 低于下限 −0.5 且持续 3 s 时以 0.5 dB/s 退。**【待 Max Q8】**

`G_trim` 要的「输出侧主段电平」不用第二份 `LiveProfile`（回看窗口约 1.7 MB，试算直方图约 0.58 MB）。草图用一个定长的 `OutputLevelTracker`：门用输入侧已发布的 `m′`，能量用出口 K 加权、按同一段累计。**【待 Max Q9】**

### 编排器公开接口（拟）

删：`requestRelearn`。`setLiveSegmentSink` 从插件路径退休，渲染改为读内部画像（Q2）；测试若仍要注入，留在测试探针上，不进 `CeladonChain` 的产品接口。

留在音频线程：`setHostInfo`、`beginBlock`、`analyzeInput`、`analyzeSpectrum`、`endBlock`、`handleDiscontinuity`、`hostReset`。`hostReset` 清的是滤波器与未完成统计窗，不清 `LIVE` 里的累计量。

留在消息线程：`prepare`、`createStateXml`、`restoreStateXml`。

`beginBlock` 的返回值仍是本块交给四个模块的 `ChainParams`。Limit Gain 上的附加项从「校准偏差 + g_保护」改成「`G_trim` + 三者取小」。

### `Status` 字段

删除学习 / 锁定专用项：`phase`、`profileReady`、`planReady`、`calibrationValid`、`listenedSeconds`、`requiredSeconds`、`calibrationPaused`、`relearnInProgress`、`unlearnedOfflineWarning`、`savedPlanFromOlderAlgorithm`、`profileMismatch`、`waitingForPlayback`、`clayNeedsRelearn`，以及 House 锁定进度（`houseHeardSegments`、`houseTargetSegments`、`houseFullSegments`、`houseRequiredFullSegments`、`houseHeardSeconds`、`houseTargetSeconds`、`houseMainSectionHeard`、`houseLockPending`、`houseLedgerFull`、`houseOfflineListening`）。

保留给灯和旋钮说明：`effectiveTargetLufs`、`limitedByManual`、`resolvedStyle`、Clay 实际值与三盏灯、`clayCostLimitedMask`、`houseActive`、`housePhaseHint`、`housePhaseMonoLossDb`。`protectionDb` 改成三者取小之后的退让。

新增（界面七条够用即可，不要再平行做一套 Phase）：

| 字段 | 用途 |
|---|---|
| `float confidence` | `c`。第 3、5 条 |
| `float currentMainness` | 正在播的这一段的 `m`。第 6 条 |
| `bool coldExport` | 离线且 `c < 0.5`。第 3 条 |
| `GuardCause guardCause` | `none` / `budget` / `deliveryPeak` / `contrast`。第 4 条写原因；PSR 并进 `budget` 或单独一项，等 Q8 的文案 |
| `float protectionDb` | 已退的 dB（≤ 0）。第 4 条 |
| `bool guardEngaged` | 正在退，或已经退到底仍超。第 4 条 |

`houseSegmentMode` 若状态行不再写「按秒 / 按小节」，就删。反相提示的持续时长仍按小节（没拍点退回秒），那是 ClayGuard 的内部量，不必回灌 `Status`。

### 守门类（新，音频线程，定长）

三个类同一形状，便于取最小：

```cpp
enum class GuardCause : std::uint8_t { none, budget, deliveryPeak, contrast, psr };

struct GuardOutput
{
    float reliefDb = 0.0f;    // ≤ 0
    GuardCause cause = GuardCause::none;
    bool engaged = false;     // 正在退，或退到底仍超
};

class ContrastGuard   // 落差守门。输入、输出各一份短期响度直方图
{
public:
    void prepare (double sampleRate) noexcept;
    void clear() noexcept;                 // 只用于冷启动。宿主 reset 不调
    GuardOutput process (float inputShortTermLufs, float outputShortTermLufs,
                         bool musicActive, int numSamples, bool clubDelivery) noexcept;
    // 消息线程，不与 process 同时：
    std::unique_ptr<juce::XmlElement> createStateXml() const;
    bool restoreStateXml (const juce::XmlElement&);
};

class PsrGuard;       // 同形状。process 改读 3 s PSR 与该档下限
```

`GlazeGuard` 保留类名，补 `createStateXml` / `restoreStateXml`，`reset()` 改名或分成「冷启动清空」与「宿主 reset 不清」。今天的 `reset()` 会把 `g_保护` 清零，阶段 2 不能在 `hostReset` 上调它。速率是否改成 T-047 的变速率，等 Q8；接口先按「每块 `process` 返回 `GuardOutput`」留出。

`OutputLevelTracker` 同样：`prepare` / `clear` 只在冷启动，`noteMainness` 在音频线程吃本段 `m′`，`processOutput` 吃出口样本，`mainLevelLufs()` 给 `G_trim`。状态进 `LIVE`。

## 5. 工程与读档

处理器仍把 GLAZE 挂在 APVTS 导出的 XML **外面**（`getStateInformation` 末尾），不进参数树。参考频谱节点不动。

新树（名字用英文）：

```xml
<GLAZE_STATE formatVersion="2">
  <LIVE formatVersion="1" compressed="1">
    <!-- base64(zlib(下面这棵未压缩子树))；读回逐位相同 -->
    <LIVE_PROFILE .../>          <!-- 阶段 1 已有，格式版本 2，LiveProfile 自己的节点 -->
    <CONTROL/>                   <!-- 速率限制器当前增量、G_trim、自动风格权重 -->
    <BUDGET_GUARD/>
    <CONTRAST_GUARD/>
    <PSR_GUARD/>
    <OUTPUT_LEVEL/>
    <CLAY liveClay="..." guard="..." pressureScale="..."/>
  </LIVE>
</GLAZE_STATE>
```

- `GLAZE_STATE` 的 `formatVersion` 从 1 升到 2：本插件只认带 `LIVE` 的 v2。更高版本整份按冷启动。
- 旧工程（v1，或只有 `GLAZE_PROFILE` / `PLAN` / `CALIBRATION` / `EXECUTION`）：这些子节点读到就丢，`LIVE` 缺省 = 冷启动，界面提示一次（Q1）。不尝试把旧画像迁进实时窗口（锁定方案无法变成因果统计）。
- `LIVE_PROFILE` 仍由 `LiveProfile::createStateXml` 生成（窗口、未发布段、事件时钟）。编排器不把这份格式拆开。压缩包一层在外面，这样阶段 1 的逐位读回测试仍对着未压缩节点做。
- 体积：阶段 1 实测未压缩时 40 段约 119 KB，窗口满 512 段 **1.52 MB**。计划遗留 5 指定 zlib + base64，无损。降精度或只存权重大的段会破坏读回逐位，本阶段不做。**【待 Max Q10】**
- 往返：读回后第一块起，控制量（已施加增量、`G_trim`、三守门、Clay 自动值、压力缩放、已发布画像）逐位等于保存时。滤波器、检测器、未写进 `LIVE` 的窗从零开始。
- 采样率改变：统计是 LUFS / dB，不清。跨采样率换算阶段 1 已能做；阶段 2 只保证 `prepare` 之后读回（遗留 4）。
- **`GLAZE_CONSTANTS`**：ADR-030 写「常数版本快照作废」。规格 §6 的 `LIVE` 清单里没有它。草图：不再写入；读到忽略；`makePlan` 只用本二进制的出厂 `Constants`。`LiveConstants` 本来就不进标定快照。可选在 `LIVE` 里记一个不参与计算的 `constantsVersion` 整数，只供诊断。**【待 Max Q11】**
- 重复导出不保证逐位相同（每次导出都在更新统计）。离线与实时同一条 `process` 路径，不另开两遍渲染。

## 6. 阶段 1 遗留 1～10 落在哪

| # | 事项 | 落点 |
|---|---|---|
| 1 | 删学习后只留实时段落账，退役 House 门控那一份 | 代理 B。`trial*` / `blend*` 与 `live*` 并成一组 |
| 2 | `LiveProfile.cpp` 与 `MusicAnalyzer.cpp` 里约 50 个口径常数、8 个小函数抽到 `ProfileMath.h` | 代理 B 抽头文件并改分析器。`LiveProfile.cpp` 只改 include，写成跨文件请求，不放进 B 的主改范围（见第 7 节） |
| 3 | 删「格式 ≥ 3 且 `houseGate.valid`」的锁定 / 重开 / 主段落校准；T-049 重开沿用混合损失一起删 | 代理 A |
| 4 | `LiveProfile::prepare` 冷启动；换采样率要先准备再读档 | 代理 A 的 `prepare` 顺序（第 2、5 节） |
| 5 | 窗口满 1.52 MB，要压缩 | 代理 A 的 `LIVE` 外层。Q10 |
| 6 | 学习开始时实时试算约 250 ms 空档 | 删学习后自然消失。不另做 |
| 7 | 实时账的 `trial*` 仍只在学习期记 | 随第 1 条并档。并完之后决策器只读这一组 |
| 8 | 两处 ADR-015 过时注释：`MusicAnalyzer.cpp`「决策器选收单分频点…」、`MusicAnalyzerTests.cpp`「反相超低频由收单处理」 | 代理 B、D 顺手改注释。自动收单仍恒为中性 |
| 9 | 7 个 `clayMono*` 常数 | **已在 T-063 从 `main` 退役**（Max 当时选「现在退役」）。阶段 2 不再删，名字不得复用。代理清单不加 `GlazeConstants.{h,cpp}` |
| 10 | `GLAZE_CONSTANTS` 以后怎么存 | Q11。草图按「不再写入」 |

`HouseConstants` 里的 `lockSegments`、`requiredFullSegments`、`fastTransitionSeconds`、`reopen*` 阶段 2 没有读者。本阶段留成死字段，避免把 `GlazeConstants` 拉进代理清单、避免快照格式再变一次。**【待 Max Q12】**

## 7. 四个代理与上下文预算

计划原文：四代理 workflow + 1 个评审，Opus 5.5 · max，约 4 × 50 万 + 45 万 ≈ 245 万 token。评审在集成与验收之后，只有一次。本草图写完由主进程自查，不另派第五个实现代理。下面的「上下文」是开工时该装进窗口的量，不是 50 万额度本身。

| 代理 | 文件（计划表） | 上下文预算 | 不要装进窗口 |
|---|---|---|---|
| A 编排器与保存 | `GlazeController.{h,cpp}`、`GlazeProfile.{h,cpp}`、`GlazeTypes.h`、`GlazePlanner.{h,cpp}`、`GlazeGuard.{h,cpp}`、新 `ContrastGuard` / `PsrGuard` / `OutputLevelTracker`、`CeladonChain.{h,cpp}`、`PluginProcessor.{h,cpp}`、删 `GlazeCalibrator.{h,cpp}` | 50 万。先读本草图 + 上列 **头文件** + `LiveProfile.h` 公开接口（约 4～6 万）。`GlazeController.cpp` 按状态机节读，不要通读 4000 行再动手 | `MusicAnalyzer.cpp`、`LiveProfile.cpp`、整份 `tests/` |
| B 分析器 | `MusicAnalyzer.{h,cpp}`、`SegmentLedger.{h,cpp}`。本草图补：`ClayTrials.{h,cpp}`（四个收单候选在这里）、新建 `ProfileMath.h` | 50 万。头文件加 cpp 里「六、七」节与先试后用，约 8～12 万 | `GlazeController.cpp`、UI |
| C 界面 | `Source/ui/{GlazeStatusText,MiddleLayer,HelpText,Localisation}.*`、`PluginEditor.{h,cpp}`、`ParameterIds.h`、`tools/ui-snapshot/*` | 50 万额度留给文案往返和截图。源文件上下文约 2 万。只改 GLAZE 状态行与重新聆听按钮 | DSP 实现、`LiveProfile` |
| D 测试与工具 | `tests/*`、`tools/render/*`。重写依赖锁定的用例（计划点名的规模：GlazeController 251、MusicAnalyzer 78、GlazeCalibration 整份删、Glaze 41、GlazeStatusText 37、GlazePlanner 20、LegacyFixture 19、ClayGlaze 13、Color 10） | 50 万。按类名打开，不要一次读完所有测试 | 与锁定无关的模块测试 |

跨文件请求（谁都不在自己的清单里直接改对方的文件）：

- B 抽出 `ProfileMath.h` 后，主进程集成时改 `LiveProfile.cpp` 的 include 与重复常数。B 不改发布语义。
- A 若要改 `setConstants` 的线程契约（Q4），主进程再改 `LiveProfile.h` 的那一句，不放进 A 的默认清单。
- D 的渲染自检跟着 A 的 `LIVE` 字段名走；字段名以本草图第 5 节为准，实现时不另起一套。

验收仍按计划：三套全套、22 首 `gate_check.py`、合成曲落差、真编排器因果重放、切块逐位、保存往返、48 kHz / 32 最慢块 ≤ 15 %、截图。本 kickoff 不跑这些，也不在 Mac 上构建。

## 8. 请 Max 拍板

下面每一条都挡住实现代理开工。建议默认写在题干里；不接受就改草图再开 workflow。

1. **Q1 旧工程提示。** 提示一次，不写入 `LIVE`，只在本次打开时显示。默认：是。
2. **Q2 画像只有一份。** 链持有 `LiveProfile`，`--live-trace` 读这一份，插件不再把外部 sink 当产品接口。默认：是。
3. **Q3 「两种变化」。** 画像每 1 s 改决策器输入；旋钮 / 风格 / 曲风 / 交付本块立刻重算目标；施加共用速率限制，不恢复 3 s 过渡。若「两种」是指增益上行与下行，请直接改口，并一起答 Q8。默认：按本条。
4. **Q4 换曲风与 `setConstants`。** 同步重算整窗放不进 48 kHz / 32 的最慢块。默认：音频线程只记下新的 `MainSectionModel`，已发布画像继续用旧模型，模型切换折进后续 `advanceTo` 分片（要改 `LiveProfile` 一句契约，集成时做）。备选：换曲风那一块允许超预算。
5. **Q5 软遗忘。** 沿用阶段 1 已落地的检测（6 LU / 20 s / ×0.2，轻的那一支再加起伏 ≤ 2 LU），编排器不另写。默认：沿用，不把规格 §6 那句较短的说法当成要删起伏门。
6. **Q6 画像字段。** 本阶段不改 `MusicProfile` 布局；四个收单代价停算，`valid = false`。默认：是。
7. **Q7 棘轮范围。** 只压净增益（`m < 0.5` 时只降不升）。Glue / Heat / Clip / Tone 仍走向新目标，但走 Q8 的速率。默认：只压增益。
8. **Q8 速率与守门数字。** 摘录没有 dB/s。默认把第 4 节列出的 T-047 起点写进出厂常数，并在因果重放里当验收起点，不在本阶段改算法结构。`GuardCause` 是否单列 PSR，默认单列。
9. **Q9 输出侧主段电平。** 不用第二份 `LiveProfile`，用定长 `OutputLevelTracker`（输入侧 `m′` 当门，出口能量按段累计）。默认：是。
10. **Q10 压缩。** `LIVE` 外层 zlib + base64，内部 `LIVE_PROFILE` 不降精度。默认：是。
11. **Q11 常数快照。** 不再写 `GLAZE_CONSTANTS`，读到忽略，方案只用本二进制出厂常数。`LIVE` 里可以记一个不参与计算的版本整数。默认：丢弃快照，版本整数要。
12. **Q12 死字段。** `HouseConstants` 的锁定 / 重开字段本阶段留着，不改 `GlazeConstants`。默认：留。

已由规格或已合入的提交定死、不再问：取消聆听与锁定；GLAZE ≤ 0 逐位等于手动；不按宿主走带判断换歌；自动收单保持中性；曲风 13 项与延迟不在本阶段；T-063 已删 7 个 `clayMono*`；坏段上界与 8 kHz～768 kHz 沿用阶段 1。
