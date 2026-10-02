# T-048 阶段 1 · 代理 A 报告：实时画像 LiveProfile

2026-10-02，代理 A（Opus 5.5 max）。工作树 `/Users/maxsong/All-Project/Celadon/.claude/worktrees/wf_a08da55a-d9a-1`，分支 `worktree-wf_a08da55a-d9a-1`（开工时快进合并到脚手架 `49caf27`），提交 `d063c7e`，服务器 107。

> 本报告本应写到 `docs/validation/2026-10-02-T048-phase1/REPORT-A.md`。子代理写报告文件被工具拦下，按任务书改放在这里，请主进程落盘。

## 0. 结论

- **完成**：`Source/dsp/LiveProfile.cpp` 按 `LiveProfile.h` 与 BRIEF 第 3 节写完（整份替换占位）。`tests/LiveProfileTests.cpp` 有 14 个用例（`LiveProfile`），覆盖任务书 a～g，另加 W_mem / M0 的单位、提交记录环形缓冲、坏段、主段模型切换与格式字段。
- **测试**（107，GCC `-Wall -Wextra -Wshadow -Wconversion`）：
  - 编译 0 警告；
  - `--only LiveProfile`：139 过、0 败；
  - `--only MusicAnalyzer,MusicAnalyzerHouse,SegmentLedger,LiveProfile`：3663 过、0 败，等于开工基线 3524 + 139。基线是脚手架 `49caf27` 上同三类测试 3524 过、0 败、0 警告。
- **等价性**（遗忘关掉，每段 m = 账本的 full ? 1 : 0）：
  - 除按设计不同的几项（第 4 节）外，频谱、立体声、起音、次声、真峰值都逐位相同；
  - I 与 PLR 差在 1e-4 以内，实测 4 位小数相同；
  - 6 项试算代价差 ≤ 0.01 dB，实测最大 0.0055 dB；
  - 混合损失数值逐位相同；S_ref 与段落账逐位相同。
- **耗时**：每并入一段中位 5 µs、P99 6 µs、P99.9 12 µs。冷启动 2 µs；最坏情形「每段都重规范」一段 6.3 µs。
- **状态 XML**：12～19 KB。
- **要拍板的有 9 件**（第 9 节），最要紧的两件：
  1. 出厂常数下，**换歌软遗忘会被 House 的长 breakdown 触发**，32 小节的 breakdown 能把 c 从约 1 打到约 0.05；
  2. LRA 按规格用全部段统计，**正好违背 T-038 阶段 3 改成「只用满编段」的理由**：合成曲上全部段 8.8 LU、只用满编段 0.2 LU，方案的 LRA 修正差约 0.9 dB。

## 1. 改动的文件

| 文件 | 改了什么 |
|---|---|
| `Source/dsp/LiveProfile.cpp` | 整份替换占位：实现全部接口 |
| `tests/LiveProfileTests.cpp` | 整份替换占位：`UnitTest ("LiveProfile")`，14 个用例 |
| `Source/dsp/LiveProfile.h` | 只加两行注释：prepare 不再分配；setConstants 拷贝之后会重算派生量。接口没动 |
| `Source/dsp/GlazeConstants.h` | `LiveConstants` 加一个字段 `peakTailSeconds = 0.5`（理由见 2.4「真峰值」与第 7 节）；没改任何默认值 |

## 2. 实现口径

### 2.1 三个遗忘域：全局缩放因子 + 周期性重规范

| 域 | 装什么 | 每段的遗忘因子 | 加进去的权重 |
|---|---|---|---|
| 主段域 | 4/4 型的 I（门块）、真峰值、频谱 / 立体声 / 低频起伏 / 通量、起音、次声、试算与混合损失、主段质量 M | exp(−(w ÷ W_mem + 无主段超出的秒数 ÷ noMainDecaySeconds)) | m |
| 参考域 | S_ref、lowRef、LRA 的直方图（全部段） | exp(−有效时长 ÷ referenceMemorySeconds) | 1（直方图权重 = 有效时长） |
| 全部段域 | 响度型 / 被动型的 I（门块）与它的有效时长 | exp(−有效时长 ÷ W_mem) | 1 |

- **缩放因子**：每个域一个缩放因子 g，实值 = 存储值 × g。遗忘只乘 g，存储值不逐格乘，所以不会一路衰减成次正规数。新加的量按 m ÷ g 放大后加进存储值。
- **重规范**：g 低于 1e-30 时，把存储值一次乘回实值、g 回到 1；实值绝对值低于 1e-250 的归零，这样之后再乘 g ≥ 1e-30 也不会成为次正规数。只在并入一段时判断，所以重规范发生在哪一段只取决于交来的段序列。
- **为什么等价性能逐位**：W_mem 与参考遗忘都取 +∞ 时 g 恒为 1，存储值就是 Σ m × 段的量。m = 1 时与 House 按段求和逐位相同：顺序相同，× 1 和 ÷ 1 都是精确的。
- **W_mem 与 M0**：按小节分段时 W_mem = memoryBars × 段的标称长度，否则 = memorySeconds。M0 用同样的换算，取最近一段的分段方式。用例 `testMemoryUnits` 两种分段方式都核对了。
- **试算直方图只扫有数的范围**：每一路「有数的格」的范围 [low, top] 一直在跟踪：并入时扩大，重规范后收缩，读档后整张重扫。冷启动与重规范只扫这个范围。整张 0.58 MB 扫一遍约 50 µs；改成只扫范围后，冷启动从 66 µs 降到 2 µs，每段都重规范时一段从 48 µs 降到 6.3 µs。

### 2.2 主段度 m、参考、冷启动

- **m 的公式**：ramp_w(x) = clamp(0.5 + x ÷ w, 0, 1)。
  - 4/4 型：m = ramp(L_seg − (S_ref − ΔG)) × ramp(lowShare ÷ lowRef − g_low)；g_low ≤ 0 时不乘低频因子。
  - 响度型：只有响度因子。
  - 被动型：有效音频一律 m = 1。
  - 段响度读不出（没有有效音频）→ m = 0。
  - 4/4 型低频占比读不出 → 低频因子 = 0，与 SegmentLedger 门控一致：读不出的那道门就过不了。
- **参考**：
  - S_ref：参考域段响度直方图（0.1 LU 一格，按有效时长加权）的 referencePercentile 分位，取格中心。
  - lowRef：响度因子 ≥ 0.5 的段的 lowShare 直方图（0.001 一格）的同一分位。
  - 分位的取法照抄 `SegmentLedger.cpp weightedPercentileBin`。等价性用例里 S_ref 与段落账逐位相同（全部段都已听时，两者是同一口径）。
- **因果**：m 用「这一段并入之前」的参考来算，提交记录里的 `referenceLufs` 记的也是它。`segmentFinalized` 与 `mainnessFor` 共用同一个函数，用例逐位核对了两者的 m 与 w。
- **换模型**：`setConstants` 拷贝常数之后会立即按新常数重算 S_ref / lowRef 与画像。原因是分位、I 取哪个域、M0 都随模型变；不重算的话，换模型后的第一段会按旧分位的 S_ref 来算 m，与头注释「之后的段按新模型算 m」不符。重算不分配，约几 µs。
- **冷启动的取法**：以这一段自己为参考，即 S_ref = L_seg、lowRef = lowShare。于是响度因子 = ramp(ΔG)、低频因子 = ramp(1 − g_low)，出厂常数下两者都是 1，第一段算主段。两道参考各自冷启动：S_ref 已有、lowRef 还没有时，只有低频因子按自己算。理由：
  1. 此刻它是唯一的证据。规格第 5 节已经接受「冷启动时 DJ 前奏会被当成主段」：c 从 0 起爬，决策器按 c 收紧；真主段一来 S_ref 就上移，前奏掉出门外，前奏的累计按进账遗忘淡出。
  2. 不取 m = 0：那样第一段白白丢掉，c 晚一段才起爬；对只有一种段落的素材（整段都是 drop 的片段）也没有好处。
  3. 用公式本身，不另设一个数：常数改了也自洽，例如 ΔG = 0 时第一段 m = 0.5（有用例）。

### 2.3 换歌与长时间无主段

- **换歌软遗忘**：
  - 段响度与「这一段并入之前」画像的 I 相差超过 newProgramLu 的段，有效音频累计满 newProgramSeconds，就把三个域的 g 一起乘 newProgramKeep，然后计数清零。
  - 软遗忘在这一段并入**之前**做，这样新素材按原权重进来。
  - 中间夹一段不超的段，计数清零重来。段响度或 I 读不出的段既不计数也不清零（没有证据）。
  - 「超过」是严格大于。用例：差 5.9 LU 永不触发；差 7 LU 时第 11 段（累计 20.9 s）触发。
- **长时间无主段**：
  - 自上一次 m ≥ mainThreshold 起，有效音频超过 noMainSeconds 的那一部分，按 exp(−超出秒数 ÷ noMainDecaySeconds) 乘在主段域上，再与按进账的遗忘相乘。
  - 只算越过门槛之后的秒数，所以与段怎么切无关。用例核对了：60 s 之前逐位不动；越过之后按超出部分衰减；来一个主段就停。
  - 规格写的是「**改按**有效音频时长衰减」，我实现成两个乘性因子叠加：无主段时额外遗忘，按进账的遗忘照旧。出厂常数下两种写法几乎一样（m < 0.5 时按进账那一项 ≤ 0.5 × 有效时长 ÷ 93 s，时间衰减是有效时长 ÷ 120 s），但不逐位相同，列进待拍板。

### 2.4 逐个特征与 House 画像的对应

House 那一族函数在 `MusicAnalyzer.cpp`：computeHouseProfile 3323、houseLoudnessProfile 3459、houseSpectrumProfile 3541、houseStereoGroup 3718、houseOnsetProfile 3745、houseTrialProfile 3786、houseSubProfile 3844、houseConfidences 3858。实时画像一个函数对一个（`liveLoudnessProfile` 等），公式逐行照抄，只把满编段的整数计数换成加权的实数。权重全为 1 时退回 House 的整数公式：例如分位的 index = ⌊(n − 1) × 分位 + 0.5⌋、中位数 middle = ⌊(E − 1) ÷ 2⌋，用 double 算，n、E 是整数时与 House 的 uint64 结果相同。

| 画像字段 | House 画像 | 实时画像 | 差异的理由 |
|---|---|---|---|
| integratedLufs | 满编段门块，逐块过 −10 LU 相对门 | 4/4 型：主段域门块直方图（0.1 LU）。相对门用逐块累加的总和（与 House 求和顺序相同），「在不在门上」按格中心判（同通用画像）。响度型 / 被动型：全部段域的同一积分 | 门块个数不设上限，只能存直方图；判门误差只出在相对门那一格里的门块（≤ 0.05 LU） |
| truePeakDbtp | 满编段 truePeak 的最大值 | 主段加权的「段最大值」直方图（0.1 dB 一格，每格另记落进来的最大线性值）。取其上主段质量 ≥ peakTailSeconds 的最高一格里记下的最大值；总质量不到这个数时（冷启动）取最高一格 | 最大值本身不会遗忘，一个孤立的热峰值会永远定住真峰值；按质量设门后旧峰值随遗忘退出，m 很小的段也不能单独定峰值。遗忘关掉、每个满编段的 w ≥ 0.5 s 时与 House 逐位相同 |
| plrDb | TP − I | 同 | — |
| loudnessRangeLu | 满编段（T-038 阶段 3 起） | 全部段（参考域，按有效音频遗忘，τ = 180 s），−20 LU 相对门，P95 − P10；confidence 按 rangeSeconds ÷ 60 s 打折 | BRIEF 第 3 节明文要求全部段。与 House 差别很大，见待拍板 ② |
| 频谱（27 段电平、Tame / Recover、斜率、低 / 高频占比、低频起伏、bandConfidence） | 满编段的和 | 主段加权的和 | — |
| 立体声（逐段与两组） | 同上 | 同上 | — |
| 起音密度 / 强度、次声占比 | 同上 | 同上（事件个数是加权的） | — |
| 试算代价 6 项 | 满编段合并「每路最大的 8 个」，P99 逐位 | 见 2.5 | 块数不设 512 上限（靠遗忘），只能存直方图 |
| 混合损失 2 项 | 满编段两份 K 加权能量之和 | 主段加权的和 | — |
| confidence | min(代表性, 观测充分度) × 一致性 | min(c, 观测充分度) × 一致性；混合损失也给（照通用画像 `finalizeConfidences`） | `houseConfidences` 漏给混合损失（第 8 节） |
| effectiveSeconds / representativeness | 全部段有效时长 / 不重复素材与锁定进度 | M / c = 1 − exp(−M ÷ M0) | 头注释规定 |
| formatVersion / houseGate | 3 / 门控结果 | 2 / 不填 | 见 2.6 |

Segment 不带分析器的子块长与 Balance 的 hop。这两个由段还原：逐个累加的时长 ÷ 个数 × 采样率再取整，舍入误差远小于半个样本。这样频谱与 LRA 的时长就和 House 是同一个公式（个数 × 长度 ÷ 采样率），等价性才能逐位。试算块长按分析器 `configure` 的公式由采样率算出。

### 2.5 试算 P99 的方法与精度

- **直方图**：每一路（原始 + 6 个候选）一张加权直方图，−80 … +24 dBFS、0.01 dB 一格，共 10400 格。每段交来的「每路最大的 8 个块峰值」（不足 8 块就是全部块）各按权重 m 计入；这一段的全部块数（`liveTrialBlocks`）计入总块数 W，没交数值的块都在这 8 个之下。
- **P99**：把 `houseTrialProfile` 用的 numpy 线性插值推广到加权：
  1. 条目 = 有权重的格，值取格中心的线性幅度，按值升序排在「质量坐标」上，条目 k 的起点 S_k = 它下面的质量。
  2. 每个条目先占一段长 max(0, u − 1) 的平台（值不变），再用长 min(u, 1) 的一段线性过渡到上一格的值。
  3. 目标位置 = 0.99 × (W − min(u_顶, 1))，从顶往下扫，几格就找到。
  - 等权（u = 1）时平台长 0、过渡长 1、目标 = 0.99 × (n − 1)，就是 numpy 的结果。整数权重时与「拆成 u 个等权条目」相同，所以几个块落进同一格不改变结果，误差只来自格中心与真值之差。
- **精度**：每个 P99 的误差 ≤ 半格（0.005 dB），代价（两个 P99 之差）≤ 0.01 dB。决策器代价斜坡的两端是 0.2 / 0.6 dB，误差小一个量级以上。等价性实测最大 0.0055 dB。
- **局限**：P99 的尾巴里如果某一段占了超过 8 块，它第 9 块以下的块没有记下数值，P99 会略偏低。
  - House 有 512 块的上限，尾巴不超过 7 块，所以没有这个问题。
  - 实时画像稳态时 W ≈ 930 块，尾巴约 9.3 块。这种情况只在一段明显更响的新段落刚开始、尾巴全落在头一两小节里时出现，过一小节就好。

### 2.6 houseGate / formatVersion / representativeness / confidence 的填法

依据代码现状。

- **决策器 `GlazePlanner.cpp` 不读 houseGate，也不读 formatVersion**（全文件 0 处）。它读的是：
  - representativeness：`confidenceScale` 1519～1530 里，κ 取 I、真峰值、代表性三者里最没把握的那个；`makePlan` 封顶方案的「拉满」清单（1991～2009）也有它。
  - 各特征的 confidence：`trustOf` 1118，用于 I、TP、LRA、斜率、lowShare、lowBandRange、起音两项、次声、高频侧中比；bandConfidence 经 `evidenceScore` / `meanBandConfidence` 使用。
  - 代价与混合损失只看 valid：`usableCost`、`capLowCutForBlend` 1208。
- **所以决策器在 House 档与通用档都能直接吃实时画像**：c 低时按现有规则向保守默认收拢（`unlearnedStrength` 0.35 起）。出厂 fullConfidence 0.8 对应 c ≥ 0.8，即 M ≥ 1.61 × M0；4/4 型约 13 个主段小节（124 BPM 约 25 s）后 κ 到 1。
- **编排器 `GlazeController.cpp`**：
  - 用 formatVersion ≥ 2 判「方案源带 Clay 特征」（`sourceSupportsClay` 2775、`clayNeedsRelearn` 3341）。实时画像带着这些特征，所以填 **2**（`clayProfileFormatVersion`），Clay 照常可用。
  - 用 formatVersion ≥ 3 且 houseGate.valid 判「House 档锁定过的画像」，涉及 `houseProfileLocked` 1477、`houseLockSatisfied` 1497、重开 1652 / 1679、`houseCalibrationOutsideMain` 1992。这是 T-038「学习 → 锁定 → 校准」的机制；实时画像没有锁定，它的段数也不去重，填进去会让这几条逻辑按不可比的数走。所以 **houseGate 不填**（valid = false），S_ref / lowRef 从 `getDiagnostics` 读。
- **不需要改决策器**。阶段 2 如果把实时画像提交成编排器的 `committed.profile`，编排器里上面那几条 House 逻辑要按实时模式绕开或改判据（已写进 cross_file_requests）。

### 2.7 状态 XML

- **节点与内容**：节点名 `LIVE_PROFILE`，`formatVersion="1"`。存这些：
  - 三个域的全部存储值与缩放因子；
  - 不遗忘的计数与时钟：并入段数、有效音频总时长、无主段计时、换歌计时与次数、最近一段的分段方式与标称长度；
  - 由段还原的子块长与 hop，以及采样率；
  - 每格记下的最大真峰值。
- **不存**：提交记录的环形缓冲；派生量（S_ref / lowRef、画像）读回后重算。所以在常数与主段模型相同的前提下（常数不是状态），读回后 getProfile / getDiagnostics 与保存时逐位相同。
- **编码**：double / float 写成 IEEE 位模式的十六进制（16 / 8 位），数组里连续的 +0.0 写成「z个数」（−0.0 的位模式不是 0，照常写出）。字段表只列一次（`visitMain / visitReference / visitAll`），冷启动、重规范、存、读四处共用，增减字段不会漏项或错位。
- **读的时候严格**：出现下列任何一种情况，都冷启动并返回 false，之前的累计也清掉：
  - 标签不对、格式版本不认识、缺属性；
  - 十六进制位数不对、有非法字符、数组长度不对；
  - 非有限值、该非负的是负数、缩放因子不在 (0, 1]；
  - 计数不是十进制、分段方式越界。
  - 以上都有用例，另加「试算直方图读到一半失败」一种。每种坏数据之后再交同样的段，结果都与全新实例逐位相同，证明读到一半的数据没有残留。
- **跨采样率读档**：与保存时采样率不同时，按样本计的量（起音有效样本数、子块长、hop）换算成新采样率下的同一段时长；其余量本来就按秒或按个数存。有用例。

## 3. 测试（`tests/LiveProfileTests.cpp`，`UnitTest ("LiveProfile")`）

| 用例 | 内容 |
|---|---|
| a `testHouseEquivalence` | 分析器 House 档：deepHouse，宿主给恒定 124 BPM 的拍点，块 512，16 小节前奏 + 16 小节 drop，学习中。把存下的 32 段各拷一份、trial / blend 拷进 live*，按 `commitWithMainness (full ? 1 : 0)` 交给遗忘全关的 LiveProfile，逐项与 `getCandidateProfile()` 比。lockSegments = 1、M0 取极小，让两边的代表性都是 1，置信度就能逐位比。前提都断言了：满编段 16 段、试算 310 块、门边那一格没有门块 |
| b `testBreakdownLeavesMainUntouched` | 出厂常数：24 小节 drop 之后接 8 小节 breakdown（m = 0），除 LRA 外画像、M、c 都逐位不变；再来 16 小节更响、低频更多的 drop，I 与 lowShare 向它移动 |
| c `testMainnessFormula` | 冷启动（含 ΔG = 0 时为 0.5）、斜坡两端与中点、两个因子相乘、读不出的读数、响度型 / 被动型；`mainnessFor` 与 `segmentFinalized` 的 m 与 w 逐位相同 |
| `testMemoryUnits` | W_mem、M0 按小节 / 按 2 秒段的换算；M = w1·exp(−w2/W_mem) + w2；c = 1 − exp(−M/M0)；representativeness = c，effectiveSeconds = M |
| d `testNoMainDecay` / `testNewProgram` | 时间衰减的起点、按超出部分衰减、主段一来就停；换歌第 11 段（20.9 s）触发、第 10 段（19 s）不触发、夹一段只差 2 LU 的清零重计、差 5.9 LU 永不触发；触发时 M 正好乘 newProgramKeep |
| `testModelsAndFormat` | 4/4 型的 I 是主段的；换成响度型后 I 立即变成全部段的 BS.1770 积分，与测试里按定义逐块算的参照差在 1e-4 以内；格式 2、不填 houseGate；commitWithMainness 钳位（> 1、< 0、NaN） |
| e `testStateRoundTrip` / `testBadState` | 三种状态（出厂常数 40 段且缩放因子 ≠ 1、每段都重规范、冷启动）往返逐位相同，并各经 XML 文本往返一次；恢复出来的副本再交 10 段仍与原件逐位相同；跨采样率读档；15 种坏数据 |
| f `testDeterminismAndReset` / `testNoAllocation` | 同一段序列：全新实例与「交到一半 reset、再从头交、中间穿插读数」的实例逐位相同；提交序号跨 reset 单调。音频线程接口（并入、重规范、换歌、时间衰减、读数、提交记录、reset）不经 operator new 分配 |
| `testCommitRing` | 容量 64；读者落后时跳到能读到的最早一条、返回负的条数；cursor 超前时钳回；记录的各字段 |
| `testBadSegment` | 段里有 NaN、计数越出定长数组 → 只记一条 m = 0 的提交，不碰任何累计 |
| g `testCommitTiming` | 2000 段的均值、最大、中位、P99、P99.9；另量「每段都重规范」与 reset；护栏只防百倍退化 |

107 跑法：`scripts/test-linux.sh bash Celadon/agent-a-check.sh <类名>`。临时脚本用完已删，没提交。

## 4. 等价性：容差与实测差

| 字段 | 容差 | 容差的来源 | 实测（House / 实时） |
|---|---|---|---|
| integratedLufs | 1e-4 LU | 实时画像判门按格中心、门上能量按格求和。先断言没有门块离相对门不到 0.1 LU（实测最近的门块离门 7.25 LU），这时门上门块的集合与 House 相同，只剩求和顺序（double 末位）与 float 舍入的差 | −12.3665 / −12.3665 |
| plrDb | 1e-4 dB | 同上（PLR = TP − I） | 通过 |
| truePeakDbtp | 逐位 | 每格记最大值 | 逐位 |
| LRA | 与全部段的参照逐位 | 参照是测试里按 House 同一公式、在全部 32 段上算的（House 本身只用满编段） | 全部段 8.80 LU（House 只用满编段：0.20 LU） |
| 频谱、立体声、起音、次声（11 项特征 + 7 个 27 段数组） | 逐位 | 同一批和、同一顺序、同一公式 | 逐位 |
| 低切代价 | 0.01 dB | 两个 P99 各 ≤ 半格 0.005 dB | 0.8508 / 0.8527（差 0.0019） |
| 加宽代价 | 0.01 dB | 同上 | 0.0025 / 0.0009（差 0.0016） |
| 收单代价 60 / 80 / 120 / 160 Hz | 0.01 dB | 同上 | 差 0.0002 / 0.0050 / 0.0020 / 0.0055 |
| 混合损失 10 / 20 Hz | 数值逐位 | 同一批块的两份能量和 | 逐位；confidence：House 恒为 0，实时 = 观测充分度（按设计） |
| effectiveSeconds | 与 House 的 houseGate.fullSeconds 逐位 | M = 满编段有效时长之和 | 逐位 |
| S_ref | 与段落账逐位 | 同 `weightedPercentileBin` 口径 | −12.25 / −12.25 |

## 5. 并入耗时

条件：107，Release，`nice -n 10`，与其他测试在同一个进程里跑。

| 情形 | 均值 | 中位 | P99 | P99.9 | 最大 |
|---|---|---|---|---|---|
| 2000 段（前奏 / drop / breakdown 交替，出厂常数） | 4.8～5.0 µs | 5 µs | 6 µs | 11～12 µs | 三轮分别为 18 / 84 / 374 µs，各是一个孤立值 |
| 每段都重规范（W_mem = 0.02 s） | 6.3 µs | — | — | — | 10～13 µs |
| reset（冷启动） | 2 µs | | | | |

- 最大值是孤立的调度抖动：同一轮的 P99.9 只有 12 µs。
- 改成「试算直方图只扫有数的范围」之前，reset 要 66 µs，每段都重规范时一段 48 µs。
- 护栏：均值 ≤ 500 µs（约 100 倍余量），最大 ≤ 20 ms（含调度抖动），依据写在用例注释里。
- 一段约 2 s（一小节）才并入一次，对音频线程的平均负担约 2.5 µs/s。

## 6. 状态大小

| 情形 | XML 文本 |
|---|---|
| 出厂常数，40 段 | 14.9 KB |
| 每段都重规范，40 段 | 12.0 KB |
| 出厂常数，2000 段（合成素材，3 种段落） | 19.3 KB |

- **构成**：大头是试算直方图（7 路）与 S_ref、lowRef、LRA、I 四张响度类直方图，空格靠「z个数」压掉。
- **会继续增长**：有数的格只在重规范时才可能被清掉，而出厂常数下重规范要约 69 个 W_mem（数小时的主段素材）才发生一次。所以真歌、长时间、素材多变时会更大：估计试算直方图每路最多几百到两千格有数，最坏到几十～两百 KB。
- 规格估的是整个 LIVE 节点 < 10 KB，单是画像这一块就已经超出，列进待拍板。

## 7. 留给主进程标定的常数

- **`LiveConstants` 里已有的 ③（BRIEF 已列）**：
  - 斜坡宽 1.5 LU / 0.15、W_mem（48 小节 / 90 s）；
  - 响度型的 ΔG 6、斜坡 3、P90、M0 20 s；
  - referenceMemorySeconds 180；
  - noMainSeconds 60 / noMainDecaySeconds 120；
  - newProgramLu 6 / newProgramSeconds 20 / newProgramKeep 0.2。最后这三个与 House 长 breakdown 冲突（第 9 节 ①），标定时请一并看。
- **新加的 `peakTailSeconds = 0.5 s`（③）**：
  - 越小越接近 House 的「最大值」，也越保守（旧峰值留得越久）：一个满主段小节（w ≈ 1.94 s）的峰值约 ln(w ÷ θ) 个 W_mem 之后才退出读数，θ = 0.5 时约 1.4 个，θ = 0.1 时约 3 个。
  - 越大越像高分位，m 小的段越难单独定峰值。
  - 上限是不超过最短的满编段 w，否则遗忘关掉时也不再等于 House。
- **实现常数**（不进 LiveConstants，理由写在代码注释里）：
  - 试算格宽 0.01 dB：决定代价误差 ≤ 0.01 dB，与内存、状态大小成反比；
  - 真峰值格宽 0.1 dB；
  - 重规范门槛 1e-30、归零门槛 1e-250。

## 8. 范围外发现（未改，交主进程决定）

- **House 画像漏给混合损失的置信度**：`MusicAnalyzer.cpp houseConfidences`（3858～3914）没给 `lowCutBlendLossDb` 置信度，House 画像里这两项的 confidence 恒为 0；通用画像 `finalizeConfidences`（2997～2998）是给的。
  - 决策器只看 valid，所以现在不影响声音。
  - 要修就补一行 `for (auto& loss : p.lowCutBlendLossDb) finalize (loss, m.trial);`。注意：如果 `computeProfileId` 把 confidence 也并进画像号，House 画像号会跟着变，要一起核对。
  - 实时画像已照通用画像的口径给了。

## 9. cross_file_requests 与 pending_user_decisions

**cross_file_requests**

1. `MusicAnalyzer.cpp`（代理 B / 主进程）：第 8 节，`houseConfidences` 漏给混合损失置信度。
2. 共享口径常数：LiveProfile.cpp 抄了 MusicAnalyzer.cpp 匿名命名空间里约 50 个画像口径常数与 8 个小函数（energyToLufs、histogramBinOf、firstBinAtOrAbove、binCentreLufs、fraction、setFeature、sideToMidDbOf、correlationOf），都注明了「同 MusicAnalyzer.cpp」。能对上 SegmentLedger.h 公开常数的已用 static_assert 钉住；其余改动时两边要一起改。建议搬进一个共享头（例如 `Source/dsp/ProfileMath.h`），两边共用一份。
3. 阶段 2（`GlazeController.cpp`）：如果把实时画像提交成 `committed.profile`，判据是「formatVersion ≥ 3 且 houseGate.valid」的 House 逻辑（锁定 1477 / 1497、重开 1652 / 1679、只在主段落校准 1992）要按实时模式绕开或改判据。实时画像是格式 2、不填 houseGate，这几条逻辑会把它当成「没锁定的画像」。
4. 代理 B 在 `SegmentLedger.cpp` 接上 `sink->segmentFinalized` 之后，建议加一条集成用例：分析器实时模式 → 账本 → sink → LiveProfile，不同宿主块长下画像逐位相同。我这条分支上账本还不调 sink，测不了；用例 f 只证明了「段序列相同 ⟹ 结果逐位相同」。

**pending_user_decisions**

1. **换歌软遗忘被 House 长 breakdown 触发**：出厂常数下，比 I 低 6 LU 以上、持续 20 s（124 BPM 约 10.3 小节）的 breakdown 每 20 s 触发一次，全部质量 × 0.2。M ≈ W_mem 时 c 依次掉到约 0.70 → 0.21 → 0.05（32 小节的 breakdown 触发 3 次），κ 降到接近底强度；drop 回来后还要约 13 小节 c 才回到 0.8。这与规格「breakdown 里什么都不动」冲突。可选：
   - 4/4 型只数低频因子 ≥ 0.5 的段（新歌的 drop 有低音，breakdown 没有）；
   - 只数比 I 更响的段；
   - 交给代理 C 的因果重放标定 newProgramLu / newProgramSeconds。
   我按规格原样实现，没加判据。
2. **LRA 用全部段还是主段**：我按 BRIEF / 规格用了全部段。但 T-038 阶段 3 正是因为全部段的 LRA 随起播位置变（合成 Tech 曲方案增益差 1.4 dB），才改成只用满编段。合成曲上全部段 8.8 LU、只用满编段 0.2 LU，L_pref 的 LRA 修正低约 0.9 dB。可选：改成主段加权的 LRA；或照规格写的「有效 < 60 s 用曲风先验」，但那要决策器 / 编排器来做，LiveProfile 拿不到曲风先验。
3. **真峰值口径**：规格第 1 节的表写「100 ms 块真峰值的主段加权 P99」，BRIEF 写「同 House」（满编段最大值），而 Segment 只带段最大值。我实现成「加权最大值 + 尾部质量门」，新加常数 peakTailSeconds 0.5 s（③）。要按规格做块的 P99，Segment 需要加逐块真峰值（脚手架接口 / 代理 B）。
4. **状态大小**：单是画像这一块就 12～19 KB，长时间、素材多变时估计几十～两百 KB；规格估整个 LIVE 节点 < 10 KB。可选：zlib + base64 压缩；试算格放宽到 0.02～0.05 dB（代价误差相应 ≤ 0.02～0.05 dB）；更勤的重规范加相对归零。
5. **冷启动取法**：以这一段自己为参考，出厂常数下第一段 m = 1。请确认。
6. **houseGate / formatVersion 的填法**：不填 / 填 2（理由见 2.6）。请确认；另一种做法是 4/4 型把 S_ref、lowRef 等填进 houseGate、用格式 3。
7. **无主段时间衰减**：实现成「与按进账的遗忘叠加」，而不是规格字面的「改按」。出厂常数下几乎一样，但不逐位相同。请确认。
8. **prepare 会冷启动**：这是头文件的约定，但规格第 5 节说「采样率改变不清」。所以阶段 2 换采样率时，要在 prepare 之后读档恢复（跨采样率读档会换算，已有用例）。请确认阶段 2 的做法。
9. **等价性容差是否认可**：I / PLR 1e-4（前提是门边那一格没有门块，已断言），试算代价 0.01 dB，其余逐位。另外我定了：段响度或 I 读不出的段，在换歌计数里既不计数也不清零。
