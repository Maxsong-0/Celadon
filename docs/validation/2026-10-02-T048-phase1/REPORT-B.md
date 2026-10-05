# T-048 阶段 1 · 代理 B 报告：分析器实时模式与 ClayTrials 抽取

> 2026-10-02 · 分支 `worktree-wf_a08da55a-d9a-2`（基于脚手架 `49caf27`）· 提交 `f55e185` · 服务器 109
> 逐位口径：实时模式关着（默认）时，插件的一切输出、画像、方案与 `49caf27` 逐位相同；实时模式开着时，编排器能读到的一切也与关着时逐位相同。

## 0. 结论

| 项 | 结果 |
|---|---|
| 先试候选抽成 ClayTrials | 完成；分析器在原位置按原顺序调用；原有分析器用例全过 |
| 分析器实时模式 `setLiveMode` / `setSegmentSink` | 完成；**另起一份实时段落账**，House 门控那一份完全不动（理由 2.1，待拍板 1） |
| SegmentLedger 定稿时交 sink | 完成（`deriveSegment` 之后、`commit` 之前；存没存进账本都交） |
| 编排器 / 链直通 `setLiveSegmentSink` | 完成；只加这一条 |
| 109 定向测试 | 最终 15 个类 7195 passed / 0 failed，编译 0 警告 |
| 49caf27 ↔ HEAD 渲染逐位对照 | 55 个输出文件除 `metrics.md` 的「生成时间」一行外逐字节相同 |
| 性能 | 抽取前后同机交替对照无可测差别；实时模式开着每块均值 55.6 ～ 57.2 µs（护栏 75 µs，未改） |

## 1. ClayTrials：接口与职责划分

文件 `Source/dsp/ClayTrials.{h,cpp}`（替换脚手架占位）；头注释写全了算法、调用顺序与逐位约定。

### 1.1 两个类型

- `ClayBiquad`（全内联）：修坯分析的双二阶——`lowpass` / `highpass` / `highShelf` 系数设计、转置直接 II 型 `run`、状态归零 `snap`。
  原 MusicAnalyzer 的 `makeLowpass / makeHighpass / makeHighShelf / runBiquad` 逐字搬来，分析器的真次声低通与候选共用。
- `ClayTrials`（不可拷贝）：先试后用的候选本身。

### 1.2 接口

| 成员 | 线程 | 作用 |
|---|---|---|
| `prepare (sampleRate, groupLength)` | 消息 | 块网格（整数个起音组 = 100 ms）、各候选系数与延迟；分配收单低通 / M·S 延迟环 / 两份守护；之后要求重新起跑 |
| `requestRestart()` / `isRestartPending()` | 音频 | 下一次 `process` 前从零状态重新起跑 |
| `process (L, R, stereo, length, position, receiver)` | 音频 | 一段（同组内连续 ≤ 128 样本）= 原 `runTrial` |
| `noteGroup (groupsSinceRestart, countsLearning, countsLive, receiver)` | 音频 | 组末原始一侧 = 原 `noteTrialGroup`，多一种口径 |
| `snapStates()` | 音频 | 组末 IIR 状态归零 = 原 `snapTrialStates` |
| `pendingBlockStart (ran)` | 音频 | 还没交齐的最早一块的起点（原 `houseCompleteBefore` 里那三行） |
| `BlockReceiver::trialBlockCompleted (Block)` | 音频 | 交齐的块按块号先后、每块一次交给调用方（无效的也交，带两个标志） |
| 静态 `insertTop / accumulate / percentileOfTop / costsOf` | 任意 | 纯函数：每路留最大 8 个、并块、线性插值 P99、代价与混合损失（10 s 有效门、−120 dB 封底） |

`Block` = 块号、原始块峰值、6 个峰值候选的块峰值、2 个混合损失候选的两份 K 加权能量，以及
`valid`（学习口径 = 原 `TrialRecord::valid`：过了起跑预热、每组都在学习区间内且在有效音频门上）与
`liveValid`（实时口径：过了起跑预热、每组都在有效音频门上，不看学习区间）。

调用顺序（分析器 runOnsets / processGroup，每个样本仍是「起音 / 真次声 → 候选 → 组末结组」）：`prepare` → 需要时 `requestRestart` →
每段 `process` → 每组末 `noteGroup` 再 `snapStates`；候选不在跑时两者都不调。

### 1.3 职责划分

- ClayTrials 管：候选 DSP、块网格与延迟补偿、交齐记录、两种有效口径、P99 与代价算法（`costsOf`）。
- 分析器管：候选什么时候跑（学习中、或实时模式开着）；每组按哪种门算（`noteTrialGroup (inside && effective, effective)`）；
  交齐的块进哪里（学习累计、House 段、实时段，见 2.3）；House 画像按满编段合并（`mergeHouseTrial`）；置信度。
- 「累计 → 画像」：P99 / 代价公式挪进 ClayTrials（原来通用画像 `updateTrialProfile + percentileOfTop` 与 House 画像 `houseTrialProfile`
  里的 lambda 各写一份同样的公式，现在两处都调 `costsOf`，由分析器的 `applyTrialCosts` 写回画像字段）。学习累计的存放与 House 的整段
  合并留在分析器：它们是「哪些块进画像」的口径。

### 1.4 逐位不变的依据

- 运算顺序与舍入点（低切输出存 float、double 累计）与原代码逐字相同；只改了成员名与「交齐的块交给接收者」这一层（同样的先后）。
- 写回画像由「对已有 Feature 调 setFeature」改成整份赋值（confidence 先为 0）；两条路径紧接着都调 `finalizeConfidences` / `houseConfidences`
  重算 confidence，最终值相同（House 画像的 `lowCutBlendLossDb` 原来就从不定 confidence，仍是 0）。
- 证据：原有分析器用例（块长无关、学习区间、延迟对齐、削顶底鼓代价、混合损失）全过；第 6 节渲染对照逐位相同。

## 2. 分析器实时模式

### 2.1 为什么另起一份实时段落账

硬约束是「实时模式开着时 House 候选画像与 CeladonChain 输出也逐位不变」。House 那一份段落账的定稿时刻、事件、计数与账本内容编排器每块都读
（锁定后防漏网数「连续更响更满」的事件、`getCurrentReading` / `isCurrentSegmentFull` 判此刻是否主段落、`getSurgeCount` 判快过渡、
`getFinalizedCount` 判何时重试重开、重开时按账本重算画像）。单份账本要满足实时一侧的四个要求，每一条都会改动它们：

1. 不学习时也要等先试后用的块交齐再定稿（否则段尾一两块的 live* 记没记上取决于宿主块长）——定稿晚约 0.1 s + 最长延迟，事件、`getCurrentReading`
   的退路（开着的前一段与最近定稿的那一段判定规则不同）、门控结果都会晚几块变；
2. 通用档也记账——通用档的段进 House 账本（账本跨换曲风保留，换回 House 后重开时按它重算画像）；
3. 重新聆听 / clearHouseLedger / reset 前先就地定稿交出去——用 endEpoch 会多出事件、新响段计数、定稿计数，编排器的快过渡判断与重开重试会变；
4. 段首尾相接——endEpoch 不截段尾，跳播之后旧段的 end 盖过新段的 start。

所以 House 那一份完全不动（实时模式开着时连段里的 live* 四项都与关着时逐字节相同），另一份 `liveLedger`（同一个类、同一套常数）专给实时一侧。
代价：实时模式开着时多约 2 MB 账本；House 档里每路数据记两份（实测不到 1 µs / 块）；关着时 Impl 只多 SegmentLedger 的定长成员约 50 KB。

### 2.2 实时段落账的口径

- 分段、记账与 House 档逐项同口径：`LedgerFeed`（各路读到哪里、组网格原点是否已知、进行中的起音事件、找段缓存）一份喂法给两份账共用，
  归段函数（`noteSubBlock / noteMusicFrame / accumulateSegmentStereo / noteGroupInto / noteEventInto / noteLedgerTrial`）与
  `completeBefore` 同一份；区别只有：所有曲风都跑，且**一直等先试后用的块交齐**（House 那一份只在学习中等，照旧）。
  宿主走带只按 SegmentLedger 现有规则用（小节锚定、位置分格），没有新增按走带触发的逻辑。
- 断开：只在不连续点（跳播、回绕、宿主 reset、表的时间轴断开、子块长变了）。开着的段**截到此刻**（`now`：数据只送到这里），就地定稿交出去，
  下一块重新锚定。截断：SegmentLedger 没有「截断再定稿」的接口（头文件不归本线），`liveEndEpoch` 经 `openSegmentAt (now − 1)` 拿到
  正在播的那一段改 `end`，紧接着 `endEpoch` 定稿（cross_file_requests 第 1 条）。
- 不断开：重新聆听、停止学习、clearHouseLedger、换曲风都不碰它（段不丢、不截；跨换曲风的段按定稿时的曲风交出）。
- 清空前先交：`reset()`、`prepare()`（按旧采样率折算）、`setLiveMode (false)` 都先就地定稿交给接收者再清。
- 关着时：实时段落账不分配、不跑，接收者一次都不调（即使已 `setSegmentSink`）。

### 2.3 先试后用在实时模式下

- 候选一直跑（`trialsRun() = learning || liveMode`）。打开实时模式时若不在学习，候选从零状态起跑；关掉时若不在学习，候选从此不跑。
- **开始学习时候选照旧从零状态重新起跑**：学习区间里的候选状态、块网格、交齐记录与关着时逐位相同（ClayTrials 用例「重新起跑与历史无关」验了这一条），
  学习期代价因此不变。代价：实时一侧每次开始学习丢掉重启时还没交齐的 1 ～ 2 块，再有 250 ms 预热空档（待拍板 3）。
- 交齐的块怎么分（`trialBlockCompleted`）：
  - `learningBlock = learning && block.valid`（交齐时还在学习）→ 学习累计（≤ 512 块）+ House 段的 trial* 与 live*（关着时即「学习期间同时记、数值相同」）；
  - 实时段：`block.liveValid` → live*；`learningBlock` → trial*；
  - 关着时候选只在学习中跑，「交齐时还在学习」恒成立 = 原来「有效就记」；开着时停止学习后才交齐的块（原来根本交不齐）不进学习累计，画像照旧在停止时冻结。

### 2.4 公开接口

```cpp
void setLiveMode (bool enabled);                    // 消息线程、处理之前；开 = 分配实时段落账，关 = 先交出开着的段
void setSegmentSink (SegmentSink* sink) noexcept;   // 实时段落账定稿的段交给它；只存指针
```

两者在 prepare / reset 之后保持；`getSegmentLedger()` 仍是 House 门控那一份。

## 3. SegmentLedger.cpp

`finalizeOldest` 里 `deriveSegment` 之后、`commit` 之前：`if (sink != nullptr) sink->segmentFinalized (segment);`。存不存进账本都交
（没有有效音频、账本满、重放不如旧值的段同样交），endEpoch 就地定稿的段同样交；账本状态一个不改，公开接口未动。

## 4. 编排器 / 链直通

- `GlazeController::setLiveSegmentSink (SegmentSink*)`：非空 = 先交接收者再开实时模式；空 = 先关（开着的段交给原接收者）再撤接收者。
  设定在分析器里，同规格 prepare（走 handleDiscontinuity）、换规格 prepare（走 analyzer.prepare）、宿主 reset、读档之后都保持。头文件只前置声明 `SegmentSink`。
- `CeladonChain::setLiveSegmentSink`：头文件内联转交（`CeladonChain.cpp` 没改）。编排器其余行为一律没动。

## 5. 测试

命令（109）：`CELADON_TEST_HOST=maxsong@192.168.100.109 scripts/test-linux.sh bash Celadon/agent-b-check.sh <类名,…>`
（临时脚本：`cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release`、`cmake --build "$BUILD" --target celadon_tests`、
统计 `warning:`、`"$BUILD/celadon_tests" --only …`；用完已删）。

| 批次（--only） | 结果 |
|---|---|
| ClayTrials, SegmentLedger | 155 passed, 0 failed |
| MusicAnalyzerLive | 48 passed, 0 failed |
| MusicAnalyzer, MusicAnalyzerClay, MusicAnalyzerHouse, ClayGlaze | 3768 passed, 0 failed |
| GlazeController | 2599 passed, 0 failed |
| 最终一遍（最后一次改动后）：上面 8 个 + GlazeControllerHouse, GlazeChain, ChainDigest, LegacyFixture, CeladonChain, CeladonChainClay, AnalyzerTap | 7195 passed, 0 failed（211 个用例） |
| 编译 celadon_tests / celadon_render（GCC，-Wall -Wextra -Wshadow -Wconversion） | 0 警告 |

### 5.1 ClayTrials（`tests/ClayTrialsTests.cpp`，类名 `ClayTrials`，12 个用例）

块网格与延迟（44.1 / 48 / 96 / 192 kHz 正好 100 ms、D_c = 2(L_c − 1)、最长延迟短于一块）；交齐的块按块号先后每块一次、与 `pendingBlockStart` 一致；
起跑预热；两种口径各自独立；纯单声道时收单 / 加宽候选逐位等于原始（延迟补偿）；单声道路径 = 左右相同的立体声；任意切段逐位相同；
**重新起跑与历史无关**（两份不同历史，重启后交出的块逐位相同）；坏样本；P99 与「存全部再排序」逐位相同（n = 1 ～ 512）；代价的 10 s 门、
−120 dB 封底、混合损失有效条件；零分配。

### 5.2 分析器实时模式（`tests/MusicAnalyzerTests.cpp` 末尾新类 `MusicAnalyzerLive`，8 个用例；原有用例与工具一个没改，只在文件末尾追加）

- a 关 vs 开（分析器，**每一块之后**比较编排器能读到的一切：候选画像全部字段与门控结果、辅助量、短期视图、House 段落账全部读数 / 每段全部字段 /
  事件 / 计数、画像版本号）：三个脚本各 65 s——House 档学习 / 停 / 立即重算 / 重新聆听 / 停 / 清空；通用档学习 / 停 / 重新聆听；混合（通用 → House →
  通用 → 另一子类、跳播与回绕）。实时段各 33 / 33 / 36 段；关着时 sink 0 次。
- a 关 vs 开（CeladonChain，69 s；通用档 GLAZE 100、Deep House GLAZE 70）：首次聆听 → 提交 → 过渡 → 校准，30 s 重新聆听，50 s 跳播；
  每一块输出逐位相同，编排器计数相同（两遍都 2 次开始学习、2 次提交、1 次跳播，实时段 36 段）。
- b 首尾相接（48 s：学习、换 Deep House、停、跳播 +5 s、重新聆听、回绕到 10 s、换回通用、清空 House 账、reset）：第一段从 0 起、最后一段止于 48 s、
  相邻两段首尾相接；不完整的段只在跳播 / 回绕 / 结尾两侧；跳播与回绕处 epoch + 1；按小节 28 段、按位置 2 秒格 25 段。
- c 块长 7 / 32 / 480 / 512 / 4096：段序列逐字段相同（事件放在 430080 样本 = 五个块长最小公倍数的整数倍上；含学习、换曲风、跳播、重新聆听、reset）。
- d 不学习时每段 liveTrialBlocks = 起点落在段里的有效块数、trialBlocks = 0（9 段）；学习区间里 live* == trial*（9 段）；停止后 live* 照记（4 段）；
  实时模式关、House 学习时账本每段 live* == trial*。
- e 实时模式开着时处理、换曲风、三种不连续点（含表与 Balance 的 reset）、重算、清空、分析器 reset 全程零分配。
- 直通：sink 在 reset、同规格 prepare、读档之后照样收段（4 → 8 → 12 → 16），`setLiveSegmentSink (nullptr)` 交出开着的一段（17）后不再收。
- f 性能（第 7 节）。

## 6. 49caf27 ↔ HEAD 渲染逐位对照（必须项）

方法（109，临时脚本 `agent-b-render.sh` 经 `scripts/test-linux.sh` 跑，用完已删）：

1. 本机 `git archive --format=tar 49caf27 -- . ':(exclude)docs' ':(exclude)site' ':(exclude)installer'` 打包，随工作树同步到 109，解到独立目录；
   `cmake -S <基线> -B <基线构建> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=<同步来的 JUCE>`，`--target celadon_render`；
   HEAD 在测试构建目录里同样 `--target celadon_render`（同一编译器、同一选项，0 警告）。
2. 素材：`tools/render/make_synthetic_house.py` 的 `synthetic-house-124.wav` 与 `synthetic-tech-124.wav`（各 112 小节、217.27 s）；
   通用素材：numpy 生成的 75 s、96 BPM 半拍鼓 + 持续低音 + 和声垫 + 宽立体声噪声（前 12 s 很轻、中段满、末段 breakdown），24 bit。
3. 两份可执行文件各跑：
   `celadon_render --in songs-house --out … --glaze 70,100 --clay auto --genre general,deep --bpm 124 --metrics-only --trace --segments-csv`；
   `celadon_render --in songs-general --out … --glaze 70,100 --clay auto --genre general,deep --metrics-only --trace --segments-csv`
   （同决策重放默认开：每个 GLAZE > 0 的自动行另有一行 `clay-replay-off`）。实时模式关（默认）。渲染共用时 603 s。
4. `diff -rq base head` 两棵输出树。

结果：两边各 55 个文件（metrics.csv / metrics.md、34 + 17 份逐块追踪与逐段统计），**只有两份 metrics.md 的「生成时间」一行不同**；
metrics.csv 逐字节相同（音频哈希、试算代价 lowcut / mono×4 / width、trial_seconds、混合损失 blend10 / 20、次声占比、低频相关度、阶段等全部列）。
音频哈希（两边相同）：

| 素材 | g70 通用 | g70 Deep | g100 通用 | g100 Deep |
|---|---|---|---|---|
| synthetic-house-124 | 2b3a92a85d719178 | ece495354a57dd0d | 6fb585c29b2c5be7 | cc1742d69778ad8e |
| synthetic-tech-124 | bc361be0f8ea4127 | e8af91e4ffe94aff | b38bc47e4f4fd6fd | 57dcb4fb8773c1e3 |
| general-music-96 | faf2e8ed6f8209c8 | 3ee66c2c0beac374 | bde5067dddd9f727 | 7b4e27a7f0d963f7 |

（通用素材的 Deep 两行停在「聆听中」：不带 --bpm 时按 2 秒段，House 锁定要 124 s 已听，75 s 的素材锁不了——两边一样，照样逐位相同。）

## 7. 性能

同机交替对照（109，48 kHz 立体声、块 512，每块 4 遍取最短再求均值；base = 49caf27、head = f55e185，交替各三遍）：

| 量法 | 49caf27 | HEAD |
|---|---|---|
| 通用护栏，学习中（候选在跑） | 56.70 / 56.27 / 56.55 µs | 56.09 / 56.37 / 56.41 µs |
| 通用护栏，不学习 | 4.96 / 4.95 / 4.97 µs | 4.98 / 5.01 / 4.99 µs |
| House 护栏，学习中（House 曲） | 59.56 / 59.12 / 59.76 µs | 59.00 / 58.84 / 58.52 µs |
| 满账本一次重算 | 65 ～ 66 µs | 65 ～ 66 µs |
| 实时模式关，学习中（MusicAnalyzerLive，同一素材 + 拍点） | — | 56.40 / 56.59 / 56.35 µs |
| 实时模式关，不学习 | — | 4.95 / 4.96 / 4.96 µs |
| 实时模式开，通用，不学习（候选一直跑 + 实时账） | — | 55.58 / 55.93 / 55.67 µs（最坏块 71 ～ 72 µs） |
| 实时模式开，House，学习中（候选 + 两份账 + 门控画像） | — | 56.91 / 57.18 / 57.06 µs（最坏块 73 ～ 74 µs） |

- ClayTrials 抽取无可测代价（差别在抖动之内）。
- 实时模式开着的工作量上限就是「学习中」：候选一直跑占大头，实时账本身不到 1 µs / 块；两种都低于 `learningGuardMicroseconds`（75 µs，未改；
  新用例对实时模式另加了同一门槛的断言）。最坏块（不设护栏）实时 House 学习中到 73 ～ 77 µs。
- 最终一遍定向测试时 109 有别的负载，护栏读数偏高（通用 60.94 µs、House 63.47 µs），同一遍里 MusicAnalyzerLive 的四种量法仍是 56.40 / 4.98 / 55.74 / 57.04 µs；以交替对照为准。

## 8. cross_file_requests

1. `Source/dsp/SegmentLedger.h`：加「截到 dataEnd 再就地定稿」接口（如 `endEpochAt (dataEnd, acceptFrom)`），分析器 `liveEndEpoch` 改用它，去掉经 `openSegmentAt` 改 `end` 的写法。
2. `Source/dsp/SegmentLedger.h` 的 Segment 注释：「下面三项在候选真跑的每一块都记」只对实时段落账成立，House 门控那一份仍只记学习期间（与关着时一致），建议分开写。
3. `Source/dsp/GlazeController.cpp` `analyzeInput`：切片 PPQ 前推的条件 `isHouse (currentGenre)` 建议加上「或实时模式开着」（通用档 + 拍点 + 宿主块 > 50 ms 时实时段边界随块长变）；关着时逐位不变。
4. `Source/dsp/LiveProfile.cpp`（代理 A）：试算代价可复用 `ClayTrials::insertTop / accumulate / percentileOfTop / costsOf`；只读 `liveTrial*` / `liveBlend*`；
   交来的段含无有效音频的段、不连续点截短的段、跨换曲风的段；每次开始学习后实时试算有约 250 ms 空档。
5. `tools/render/*`（代理 C）：`--live-trace` 在每一遍新建的链 prepare 之后调 `CeladonChain::setLiveSegmentSink`；接收者 0 分配；注意第 3 条的大块长问题。
6. 本报告：子代理写报告文件被拦，请主进程落盘为 `docs/validation/2026-10-02-T048-phase1/REPORT-B.md` 并提交。

## 9. 待拍板

1. 实时一侧另起一份段落账、House 门控那一份不动（2.1）；阶段 2 是否合并 / 退役 House 那一份。
2. House 那一份在实时模式下 live* 仍只记学习期间（与关着时逐字节相同），实时的 live* 在实时段落账里——是否接受这种分工。
3. 开始学习时候选照旧从零重启，实时试算每次开始学习后有约 250 ms 空档、丢 1 ～ 2 块；消掉它要多跑一套候选（试算 CPU 约翻倍）。
4. 实时段落账在换曲风处不断开（跨切换点的段按定稿时的曲风交出）；若要切段，setGenre 时对实时账做一次截断定稿即可。
5. 通用档 + 实时模式 + 宿主块 > 50 ms 的切片 PPQ（cross_file_requests 3）是否允许改编排器那一行、由谁改。
6. 换采样率的 prepare 也先把旧会话开着的段交给接收者（按旧采样率折算）；是否改成丢弃。
7. 验收口径：「首尾相接」只在同一条时间轴上成立（表 reset / 链 prepare 重建后时间轴从 0 起）；跳播 / 回绕处截短的段与新段相接。

## 10. 范围外发现（未改）

- House 画像的 `lowCutBlendLossDb` 的 confidence 从来不定（`houseConfidences` 漏了这一项，一直是 0；通用画像是 min(代表性, 观测充分度)）。
  决策器只读 valid / value，不影响声音；存档里与通用画像口径不一致。要不要补，请定。
- MusicAnalyzer.cpp 文件头原写「Impl 约 40 KB」（T-038 加 House 账本后已不准），本线顺手改成「约 140 KB，其中两份段落账的定长成员各约 50 KB」（估算，未实测）。

## 11. 清理

工作树根目录的临时脚本（agent-b-check.sh / agent-b-render.sh / agent-b-perf.sh）与基线包 agent-b-baseline.tar 已删、未提交；109 上 `~/.cache/agent-b-t048`（323 MB：基线源码与构建、素材、渲染输出）已删；
同步目录里的临时脚本也已删。`scripts/test-linux.sh` 自己的每工作树缓存（~/.cache/celadon-tests、celadon-build 下本工作树那两个目录）照常保留，供复跑。
