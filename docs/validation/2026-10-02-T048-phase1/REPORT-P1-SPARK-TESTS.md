# T-048 阶段 1 · P1T 报告：在本机（DGX Spark）上的编译、定向测试、两次没编过的提交、新用例反向验证、自检与耗时

> 时间：2026-10-04 00:00～00:37（本机 PDT）。代理 P1T（Opus 5.5 max），夜间任务。
> 工作树 `.claude/worktrees/wf_b646ec6c-573-1`，分支 `claude/t048-p1-tests`（从 `claude/t048-realtime-glaze` 的 `9e8575a` 起），tip `27ad72a`。
> 机器：NVIDIA DGX Spark，aarch64，Ubuntu 24.04，GCC 13.3，20 核 / 121 GB；Release + Ninja，统一用 `~/.cache/celadon-spark/bin/spark-test.sh`。编译并行 8；测试与计时在全机锁 `locks/run.lock` 里串行。107 / 109 连不上，没用。
> 本机已知基线（主进程测的）：
> - main `9c18ff9` 全套 148335 过 / 5 败；`9e8575a` 全套 148803 过 / 5 败；
> - 5 条都是 aarch64 专有：ColorModule「stereo with L = R is bit-identical to mono」4 条（NEON 快路径写死了融合乘加）、BalancePerformance 计时 1 条。
>
> 证据文件在 `docs/validation/2026-10-02-T048-phase1/P1T-spark/`（第 9 节有清单）。本报告由主进程落盘：子代理写报告文件被工具拦下了。

## 0. 结论

| 项 | 结果 |
|---|---|
| 1 编译与警告 | `9e8575a` 两个目标编过，5 条警告，与 main `9c18ff9` 在本机编两个目标的 5 条逐条同类。**T-048 新增 0 条**。<br>基线日志里「多出来」的那条 `-Wstringop-overflow`：`ColorModule.cpp` 给 `celadon_render` 再编一遍时，同一处又报了一次；main 的「4 条」只编了 `celadon_tests`。<br>这条本身是 GCC 13 在 aarch64 上的误报，没改。最终提交 `27ad72a` 全新构建也是 5 条 |
| 2 定向测试（10 类） | HEAD `9e8575a`：**6991 过、0 败**（195 个用例）。<br>开工基线 `a2daf2b`：6806 过、0 败（179 个用例）。<br>没有新增失败，不需要修 |
| 3 两次没编过的提交 | **`1200248` 原样**：两个目标都编过；LiveProfile + MusicAnalyzerLive + SegmentLedger **454 过、0 败**；三份自检全过，已删的「收尾补块」一项确实不在了。<br>**`9e8575a`**：编过，定向全过。<br>⑥「约 1 分钟」的用例只卡两头：换了之后 2～18 s 挡住，65 s 之后全放开。实测三道门一直放开的时刻：帧 44.5 s、有效子块 52.3 s、有效起音 40.6 s（之前响 60 s）；之前响 300 s 时是 50.3 / 58.1 / 48.4 s。<br>没有 ⑥（`1200248`）时要 2.8～8.3 分钟（之前响 60 s）或 5.4～11.5 分钟（之前响 300 s）。<br>⑥ 的用例放在 `1200248` 上，「65 s 之后全放开」两个块长都失败，19 / 19 小节没放开 |
| 4 新用例在改前代码上（反向验证） | **B2**：8 个新用例换进 `a2daf2b`，编得过，**8 个全失败（41 条断言）**；原有 20 个全过。<br>**A2**：HEAD 的 `LiveProfileTests.cpp` 在 `a2daf2b` 上编不过（128 个错误，缺的接口见 4.2）。按旧接口最小改写 r1～r3、d3（合成段逐字相同）后，在旧算法上 **15 条断言败 10 条**。旧算法给出的正是报告里说的坏值：<br>- r1：drop 12 小节 m 全是 0，lowRef 0.4495；<br>- r2：I 停在 −34.08；<br>- r3：要 32 个 drop 小节才进 ±1 LU；<br>- d3：起伏 2.1 LU 与淡出都在第 11 小节触发 |
| 5 三份自检 | 17 / 13 / 12 过（另 1 条信息），0 失败。<br>键表 37 个；12 种坏写法都报错；块长 512 / 4096 / 480 的追踪逐字节相同 |
| 6 耗时（本机，负载 7～10） | 分析器每块：HEAD 与 `a2daf2b` 交替各跑 4 轮，**均值差 ≤ 0.6 %，最坏块差 ≤ 1 µs**。<br>LiveProfile 每片 **P99.9**：5 次里 4/4 型 10.85～13.14 µs、响度型 12.10～14.86 µs，**都 ≤ 15 µs**；每一遍都在发布时刻之前做完。<br>这是 aarch64 的数，不能代替 107 |
| 7 全套（最终代码提交 `1431d47`） | **148803 过 / 5 败**，666 个用例，逐用例结果与主进程的 `9e8575a` 基线完全相同。5 条败的都是已知的 aarch64 专有项。<br>main 的 615 个用例全在、结果相同。**新增失败 0** |

## 1. 编译与警告

命令：`spark-test.sh <源码> build`。默认编 `celadon_tests` 和 `celadon_render` 两个目标，各用全新构建目录。

| 版本 | 目标 | 警告 | 明细 |
|---|---|---|---|
| main `9c18ff9`（`git archive` 出来的副本） | 两个 | 5 | `-Wstringop-overflow` ×2：`ColorModule.cpp:423` 内联进 `stl_algobase.h:437`，celadon_tests（`[17/83]`）与 celadon_render（`[65/83]`）各报一次。<br>`-Wrange-loop-construct` ×3：`ChainTests.cpp:168`、`MusicAnalyzerTests.cpp:3509`、`SegmentLedgerTests.cpp:535` |
| 开工基线 `a2daf2b` | 两个 | 5 | 同上（全新构建 31 s） |
| HEAD `9e8575a` | 两个 | 5 | 同上，只是 SegmentLedgerTests 那条在 536 行（B2 在前面加了一行）。全新构建 27.9 s（-j8） |
| `1200248`（`git archive`） | 两个 | 5 | 同上 |
| 最终提交 `27ad72a`（`git archive`，全新构建） | 两个 | 5 | 同上（45 s，负载约 10） |

**多出来的那条 stringop 警告查到了**：

- `base-t048-build.log` 第 53 行和第 297 行是同一位置，都是 `ColorModule::heatCompensationTable` 里的 `shaped = sine;`。一次是编 `celadon_tests` 目标时报的，一次是编 `celadon_render` 目标时报的。
- 主进程 main 的构建目录里只有 `celadon_tests`（`base-main-build.log` 要编的 `celadon_ui_snapshot` 是不存在的目标），所以只数到 4 条。
- `git diff 9c18ff9 9e8575a -- Source/dsp/ColorModule.cpp Source/dsp/ColorModule.h` 为空。CMakeLists.txt 只多了 `LiveProfile.cpp`、`ClayTrials.cpp` 和两份测试。
- 结论：不是 T-048 的代码引起的。

**这条警告本身**：

- `sine` 与 `shaped` 都是 `heatCalibrationPoints` 个 float（4096 字节）的 `std::vector`，拷贝赋值走不到重新分配那一支。
- GCC 13 在 aarch64 -O3 下把那一支的目的地算成了大小 0，属于已知的一类误报（主进程说 x86 上是 0 条）。
- 不是真问题，也不归本线，没改（写进 out_of_scope）。

## 2. 定向测试：HEAD 对开工基线

命令：`spark-test.sh <源码> test --only LiveProfile,ClayTrials,MusicAnalyzer,MusicAnalyzerClay,MusicAnalyzerHouse,MusicAnalyzerLive,SegmentLedger,ClayGlaze,GlazeController,GlazeControllerHouse`。

- 10 个类名在两边都对得上。
- 开工基线的源码是 `git archive a2daf2b` 解出来的，放在主进程指定的 `~/.cache/celadon-spark/src/a2daf2b/`。

| 类 | HEAD `9e8575a` 用例数 | `a2daf2b` 用例数 | 差别 |
|---|---|---|---|
| LiveProfile | 22 | 15 | A2 整份重写：旧用例换成新语义，另加 r1～r3、d3、调用方式、窗口外、常数表等 |
| MusicAnalyzerLive | 16 | 8 | B2 加 7 个（5a ×2、5b ×2、5c、5d、5f），`9e8575a` 加 1 个（⑥） |
| SegmentLedger | 13 | 12 | B2 加 1 个（锚定取整） |
| ClayTrials / MusicAnalyzer / MusicAnalyzerClay / MusicAnalyzerHouse / ClayGlaze / GlazeController / GlazeControllerHouse | 12 / 26 / 15 / 6 / 15 / 57 / 13 | 同左 | 用例名逐字相同 |
| **合计** | **195 个用例，6991 过、0 败**（130 s，开始时负载 4.5） | **179 个用例，6806 过、0 败**（107 s，负载 7.5） | 共有的 168 个用例两边都过。HEAD 独有 27 个、基线独有 11 个（改名或改写），也都过 |

逐用例对照见 `P1T-spark/targeted-tests.tsv`；完整日志是 `targeted-head-9e8575a.log` 和 `targeted-base-a2daf2b.log`。

## 3. 两次没编过的提交

### 3.1 `1200248`：advanceTo 只由分析器报；工具去掉补 0 样本块与自检那一项

- 原样（`git archive 1200248`）编两个目标：编过，警告同第 1 节。
- `--only LiveProfile,MusicAnalyzerLive,SegmentLedger`：**454 过、0 败**，用例数 22 + 15 + 13。
- 三份自检：17 / 13 / 12 过，0 失败。
- `--selftest-live` 里没有「收尾补块（对照）」了。`tools/render/RenderMain.cpp` 里 grep 不到 `flushOnFinish`、`unflushed`、`收尾补块`。
- 块长那一项的摘要 `c423806b6b033d96`，与 `9e8575a` 相同：⑥ 不改自检合成曲的追踪，因为那里的 breakdown 只轻约 10 LU。
- 测试台不再自己报 advanceTo 之后，整链块长用例照过：`f. analyzer live mode -> … every publication is bit-identical for host blocks 4096 / 512 / 480 / 32 / 7`。
  - 发布时刻也在逐位比较之内；
  - 所以各块长之间一致，就说明由分析器驱动时，没有段晚于自己的发布时刻才到。

### 3.2 `9e8575a`：⑥ 持续偏轻就加快遗忘

- 编过；定向 10 类全过（第 2 节）。

**「约 1 分钟」在用例里断言了什么**：

用例是 `MusicAnalyzerLive` 的「live gates: a program 28 dB quieter …」。素材是合成 House，60 s 之后整段压低 28 dB，曲长约 162.6 s。三条断言：

1. 换了之后 [2 s, 18 s] 里的段，一个有效子块都没有（至少 6 段）；
2. 换了之后 65 s 起的整小节，帧、有效子块、有效起音样本都 ≥ 响小节最小值的 90 %（至少 15 段）；
3. 块长 512 与 4096 交来的段逐字段相同。

18～65 s 之间什么时候放开没有断言，「约 1 分钟」只以 65 s 为上界出现。

**实测**：在用例里加了一行只记录、不断言的日志（提交 `1431d47`），跑 `--only MusicAnalyzerLive`：

| 块长 | 帧门一直放开 | 有效子块一直放开 | 有效起音一直放开 |
|---|---|---|---|
| 512 | 换了之后 44.5 s | 52.3 s | 40.6 s |
| 4096 | 44.5 s | 52.3 s | 40.6 s |

与 `MusicAnalyzer.cpp` ⑥ 注释里的估算一致（帧门约 40～45 s、子块门约 50～60 s）。

**反向验证**：

- 做法：把 `9e8575a` 的 `tests/MusicAnalyzerTests.cpp` 换进 `1200248`（没有 ⑥），编完跑 `--only MusicAnalyzerLive`。
- 结果：131 过、2 败。败的正是「65 s after the switch all three gates are open (19 of 19 bars pass less than 90 % of a loud bar)」，块长 512 与 4096 各一条。
- 「头 20 s 挡住」和「512 对 4096 逐字段相同」照过；其余 15 个用例全过。
- 日志：`P1T-spark/reverse-quiet-gates-test-on-1200248.log`。

### 3.3 没有 ⑥ 时要多久（提交说明里「原来 5～13 分钟」）

**做法**：只在源码副本里加一个诊断用例，不进仓库，源码见 `P1T-spark/diag-quiet-gates-long.cpp.txt`。

- 素材：合成 House 624 小节，约 1207 s；前 60 s 或 300 s 原样，之后整段压低 28 dB。
- 设置：块长 4096、通用档。
- HEAD（`1431d47`）与 `1200248` 各跑一遍。

| 情形 | 帧门一直放开 | 有效子块一直放开 | 有效起音一直放开 |
|---|---|---|---|
| HEAD（有 ⑥），之前响 60 s | 44.5 s | 52.3 s | 40.6 s |
| HEAD（有 ⑥），之前响 300 s | 50.3 s | 58.1 s | 48.4 s |
| `1200248`（没有 ⑥），之前响 60 s | 241.9 s（4.0 分钟） | 495.5 s（8.3 分钟） | 168.4 s（2.8 分钟） |
| `1200248`（没有 ⑥），之前响 300 s | 418.1 s（7.0 分钟） | 692.9 s（11.5 分钟） | 323.2 s（5.4 分钟） |

- 有 ⑥ 时，两种情形都在 1 分钟内（最慢 58.1 s）。
- 没有 ⑥ 时，在之前响 300 s 的情形下是 5.4～11.5 分钟，与 B2 的估算同量级（参考吃满时帧门约 5.5、子块门约 13 分钟）。
- 两个版本上，起音门的「第一次放开」都在换了之后的第一个小节（0.0 s；31 小节在 124 BPM 下正好是 60.000 s）。慢包络与底鼓让这一小节过了门，之后又挡住。这是用例注释里说的「起音门在底鼓上可能漏过几帧」，不是问题。
- 结果文件：`P1T-spark/diag-quiet-gates-long-results.txt`。

## 4. 新用例在改前代码上的结果（反向验证）

### 4.1 B2 的 8 个新用例

**做法**：

1. 复制一份 `a2daf2b` 源码。
2. 换进 t048 工作树根目录 B2 留下的 `b2-oldtests/MusicAnalyzerTests.cpp` 和 `SegmentLedgerTests.cpp`。核对过：这两份就是 B2 收尾 `449e72a` 的测试，只有一行不同——`setLiveMode (false, &rig->balance)` 改成了 `setLiveMode (false)`，因为改前没有两参数版。
3. 只编 `celadon_tests`：4 条警告，与同一目标的基线相同。
4. 跑 `--only MusicAnalyzerLive,SegmentLedger`：**204 过、41 败**。

| 新用例 | 在 `a2daf2b` 上 | 旧代码上看到的 |
|---|---|---|
| MusicAnalyzerLive · B2 5a 分析器一级（学习停在真实块边界，块长 7 / 32 / 480 / 512 / 4096） | 败 6 / 共 14 | 通用档：块长 7 / 32 在第 15 段、4096 在第 21 段与 512 不同；Deep House 一样 |
| MusicAnalyzerLive · B2 5a 链一级 | 败 8 / 共 20 | 各块长在第 3 / 15 / 21 / 23 段与 512 不同 |
| MusicAnalyzerLive · B2 5b 分析器一级（学习中 vs 读档后不学习） | 败 4 / 共 9 | 第 0 段就不同；不学习时，实时相对门挡不住压低的那一段 |
| MusicAnalyzerLive · B2 5b 链一级（第一遍 vs 读档后第二遍） | 败 2 / 共 8 | 第 0 段就不同 |
| MusicAnalyzerLive · B2 5c advanceTo | 败 3 / 共 8 | 849 块里 advanceTo 报了 0 次，最后报到 0 |
| MusicAnalyzerLive · B2 5d 关实时模式补帧 | 败 2 / 共 4 | 最后一段的帧数修前修后都是 77（没补交） |
| MusicAnalyzerLive · B2 5f 从第 N 小节起播 | 败 4 / 共 15 | 从第 33 / 3 小节起播时，第一段不是那一小节 |
| SegmentLedger · 起点离小节线不到半个样本 | 败 12 / 共 45 | 第一段是 [0, 1) 的碎段（例：起点 2972903，得到 [0, 1)、key 1984） |
| 原有用例（MusicAnalyzerLive 8 个、SegmentLedger 12 个） | **20 个全过** | — |

日志：`P1T-spark/reverse-b2-tests-on-a2daf2b.log`。

### 4.2 A2 的 r1～r3、d3

**HEAD 的 `LiveProfileTests.cpp` 在 `a2daf2b` 上编不过**：换进去编 `celadon_tests`，出 128 个错误。缺的接口：

- `LiveProfile`：
  - `flush`、`getNextPublicationTime`；
  - `setPublicationListener` / `PublicationListener::livePublished`；
  - `readJudgements` / `LiveSegmentJudgement`、`setFrozenReferencesForTesting`；
  - 常量 `windowCapacity` / `sliceGridSeconds` / `noPublication`。
- 常数表：`LiveConstantDomain`、`visitLiveConstantPairs`、`sanitiseLiveConstants`；`LiveConstants::newProgramRangeLu`、`publishDelaySeconds`。
- `LiveCommitRecord`：`publishTime`、`admissionMainness` / `admissionLoudnessFactor` / `admissionLowFactor`、`newProgramVote`、`newProgramRangeValid` / `newProgramRangeLu`。
- `LiveDiagnostics`：`windowSegments`、`droppedSegments`、`oldestMainShare`、`oldestReferenceShare`、`publishTime`、`pendingSegments`、`lastPassSlices`、`catchUps`。

**最小改写**：源码在 `P1T-spark/LiveProfileReverse-for-a2daf2b.cpp.txt`，只在 `a2daf2b` 副本里编，不是仓库里的测试。

- 合成段 `Spec` / `makeSegment` 在 `a2daf2b` 与 HEAD 逐字相同（核对过），直接用。
- 接口对应：
  - `feed` = `segmentFinalized`：旧代码当场并入，没有发布时刻；
  - 「发布时的 m′」 → 提交记录的 `mainness`：旧代码只有进账那一刻的 m，用并入之前的参考算；
  - `judgementsOf`（按窗口重判的 m′ 与权重） → 提交记录：旧代码不重判，段的 m 与权重定在进账那一刻；
  - d3：旧 `LiveConstants` 没有 `newProgramRangeLu`，也就是改前不看起伏，四种情形都按旧规则跑。
- 断言照抄 HEAD。
- 结果：编出 0 警告；`--only LiveProfileReverse,LiveProfile` 得 159 过、10 败；`a2daf2b` 原有的 15 个 LiveProfile 用例全过。

| 用例 | 断言（照抄 HEAD） | 旧算法（`a2daf2b`）实测 | HEAD `9e8575a` 实测 |
|---|---|---|---|
| r1 低音重的前奏 | 4 条**全败** | drop 12 小节进账时 m 全是 0.000（12 小节里 0 个是主段）；<br>前奏 m 全是 1.000，权重合计 30.40 s；<br>lowRef 0.4495；I −12.038 | drop 第 1～4 小节发布时 m′ 都是 1.000；<br>lowRef 0.2005；I −8.007 |
| r2 冷启动落在淡入上 | 3 条败 2 条 | 淡入之后 6 个镲小节 I 一直是 −34.08；<br>镲 m 全是 0.000；<br>淡入那一段 m 1.000（定在进账时）；lowRef 0.1505 | 第 1 个镲小节 I 就到 −20.06，镲 m′ 1.000 |
| r3 前奏出主段 | 4 条败 2 条（「4 个 drop 小节内进 1 LU」「前奏权重为 0」） | 前奏 I −11.90（c 0.946）；<br>drop 第 1～4 小节 I −11.65 / −11.43 / −11.23 / −11.05；16 小节后 −9.74；<br>**32 个 drop 小节才进 −8.03 ± 1 LU**；前奏权重 60.80 s | drop 第 1～4 小节 I −11.65 / −8.07 / −8.05 / −8.04（c 0.379）；<br>16 小节后 −8.03（c 0.816） |
| d3 电平稳 | 4 条败 2 条（起伏 2.1 LU、淡出都应不触发） | 起伏 1.9 / 2.1 LU、两种淡出，**全在第 11 小节触发** | 1.9 LU 第 11 小节触发；2.1 LU 不触发；淡出不触发；∞ 时淡出第 11 小节触发 |

- 日志：`P1T-spark/reverse-a2-r1r3d3-on-a2daf2b.log`。
- HEAD 上这些值与 REPORT-A2（107）一致。
- 旧算法的这些值，与 REPORT-C 第 5 节第 1～4 条在真歌上看到的病同向：
  - 低频参考被前奏定住；
  - 冷启动卡在淡入上；
  - 前奏只按进账遗忘，要几十小节才退；
  - 淡出误触发换歌。

## 5. 三份自检（`celadon_render`，`9e8575a`）

命令：`spark-test.sh . run nice -n 10 $BUILD/celadon_render --selftest`（另两份把参数换成 `--selftest-house`、`--selftest-live`）。

| 自检 | 结果 | 耗时 |
|---|---|---|
| `--selftest` | 17 过、0 失败 | 3 s |
| `--selftest-house` | 13 过、0 失败 | 3 s |
| `--selftest-live` | 12 过 + 1 条信息、0 失败 | 11 s |

`--selftest-live` 要点：

- **常数解析**：改到 7 个字段，键表 37 个（期望 37）；12 种坏写法都报错。
- **段与小节**：37 段首尾相接，止于 3381677 样本；按小节对齐，0 段不在小节线上。
- **从第 9 小节起播**：29 段，首末小节号 9 / 37，碎段 0 个。
- **段落门**：breakdown 里 m ≥ 0.5 的 0 小节，第二段 drop 里 m < 0.5 的 0 小节；最终 I −12.02 LUFS、c 0.877。
- **块长**：512 / 4096 / 480 的追踪逐字节相同（`c423806b6b033d96`）。
- **追踪不改输出**：GLAZE 70 第二遍接不接追踪，成品哈希相同（`3332befe28f639a4`）。
- **模型与常数**：通用档摘要 `a5194b2a49ae8cec`；`fourOnFloor.confidenceBars=64` 时最终 c 0.231。

没有失败，不需要给 P1R 提请求。日志：`P1T-spark/selftests-9e8575a.log`。

## 6. 耗时（本机 aarch64；只报数，不改门槛）

做法：HEAD（本工作树）与 `a2daf2b` 交替各跑 4 轮。

- 每轮 `spark-test.sh <源码> test --only MusicAnalyzerLive,LiveProfile`，在全机锁里跑，不 nice。
- 时间 2026-10-04 00:16:47～00:20:45 PDT，**负载 7.2～10.3**（同时有 P1R 的 8 路 `nice -n 10` 渲染在跑）。
- 逐轮原始数据：`P1T-spark/perf-rounds.tsv`。

### 6.1 分析器每 512 样本块（MusicAnalyzerLive 性能用例）

- 两边的用例代码逐字相同。
- 量法：4 遍取每块最短，再求均值与最坏。

| µs | 关·学习中 | 关·不学习 | 开·通用 | 开·House 学习中 |
|---|---|---|---|---|
| HEAD 4 轮（均值取平均 / 最坏取最大） | 40.09 / 46 | 3.29 / 5 | 39.50 / 45 | 40.44 / 48 |
| `a2daf2b` 4 轮 | 40.35 / 46 | 3.33 / 5 | 39.74 / 45 | 40.53 / 47 |
| HEAD − 基线 | −0.6 % / 0 | −1.2 % / 0 | −0.6 % / 0 | −0.2 % / +1 |

- 逐轮看，HEAD 与基线互有高低（例：开·通用第 1 轮 39.84 vs 39.89，第 4 轮 40.00 vs 39.79），差别在轮间起伏之内。
- 结论：B2 的三道门新参考和 ⑥，在这台机器上量不出代价。护栏 75 µs。

### 6.2 LiveProfile（HEAD）：入队、分片、一遍同步重算（用例 g）

| 轮 | 入队 4/4 型：均值 / P99.9 / 最大（µs） | 分片 4/4 型：均值 / P99 / **P99.9** / 最大（µs，19390 片） | 分片 响度型：均值 / P99 / **P99.9** / 最大（µs，8274 片） | 同步一遍：1 / 64 / 256 / 512 段（µs） |
|---|---|---|---|---|
| 定向那一次（00:00，负载 4.5） | 0.59 / 2.13 / 3.3 | 4.20 / 11.55 / **13.14** / 27.9 | 4.02 / 13.17 / **14.86** / 25.9 | 2.3 / 8.1 / 25.2 / 48.0 |
| 第 1 轮 | 0.58 / 1.06 / 1.4 | 4.20 / 10.70 / **10.93** / 23.2 | 3.93 / 11.97 / **12.18** / 26.2 | 2.3 / 8.0 / 24.7 / 48.0 |
| 第 2 轮 | 0.61 / 2.56 / 3.8 | 4.14 / 10.75 / **10.94** / 14.3 | 3.92 / 12.02 / **12.24** / 15.6 | 2.3 / 8.2 / 24.8 / 48.9 |
| 第 3 轮 | 0.60 / 2.11 / 2.5 | 4.17 / 10.74 / **11.86** / 31.6 | 3.94 / 12.03 / **12.24** / 16.4 | 2.4 / 7.8 / 25.4 / 47.5 |
| 第 4 轮 | 0.57 / 1.31 / 2.5 | 4.15 / 10.69 / **10.85** / 14.2 | 3.96 / 11.95 / **12.10** / 14.2 | 2.3 / 8.2 / 24.9 / 48.7 |

- 每一遍都在发布时刻之前按网格做完：5 次都是「passes finished late: 0」。
- 一遍至多 11 片（4/4 型）或 18 片（响度型）；发布之前有 60 个网格点。
- 512 段一遍的每段开销（定向那一次，µs）：4/4 型非主段 0.0251、主段 0.1491；响度型非主段 0.0796、主段 0.1606。
- REPORT-A2 第 5 节问的「`b23c931` 按种类拆分估计之后，分片 P99.9 是否 ≤ 15 µs」：
  - **本机 5 次都 ≤ 15 µs**：4/4 型 10.85～13.14，响度型 12.10～14.86；
  - 对照 107 run3（统一估计）：4/4 型 18.1，响度型 37.4；
  - 本机不是 107（aarch64，负载 7～10），不能代替 107 的结论，107 恢复后还要再量。
- 2000 小节之后的状态 XML：1520378 字节（定向那一次）。

### 6.3 对照：`a2daf2b` 的 LiveProfile（每并入一段同步更新，旧版用例 g）

- 4 轮：每段均值 2.60～2.63 µs，中位 3，P99 3，P99.9 4～6，最大 5～34 µs。
- 每段都重规范时：均值 3.5～3.6 µs，最大 7～8 µs。
- 新做法把这件事拆成两步：入队（均值约 0.6 µs），加上每个 5 ms 网格点至多一片（均值约 4 µs）。两边不是同一个量，只并列放着。

## 7. 全套（最终代码提交 `1431d47`）

命令：`spark-test.sh <本工作树> test`，2026-10-04 00:21:44 开始，348 s，负载 10～13。

- **148803 过 / 5 败**，666 个用例。
- 逐用例结果与 `~/.cache/celadon-spark/logs/base-t048-tests.log`（主进程的 `9e8575a` 基线）**完全相同**。
- 5 条败的是：
  - ColorModule「stereo with L = R is bit-identical to mono (packed stereo biquads == scalar biquads)」4 条；
  - BalancePerformance「stereo balance costs 1.74 x mono (limit 1.40)」1 条。
  - 都是已知的 aarch64 专有项。
- `base-main-tests.log` 的 615 个用例全在，结果相同。
- **新增失败 0**。
- 编译警告：最终提交 `27ad72a` 全新构建两个目标 5 条，与 main 同机两个目标相同（第 1 节）。`27ad72a` 只加了文档，所以 `1431d47` 的全套结果对它同样成立。
- 日志：`P1T-spark/full-suite-1431d47.log`。

## 8. 没验的项

- **107 / 109 上的一切**（连不上）：
  - x86 的警告数（主进程说 x86 上是 0）；
  - x86 的计时，包括「107 上分片 P99.9 ≤ 15 µs」。
- **归 P1R 的项**：22 首门槛曲对 main 逐位相同（本机没有这些歌）、REPORT-C 那套因果重放指标（M2 / M3 / M4）、Jamendo 45 首的块长对照（B2 的 6a / 6b）。
- **没单独量的**：分析器加上 LiveProfile 一起接在链上时的每块耗时。分析器性能用例接的 sink 是 SegmentCollector，LiveProfile 的分片是另外量的。
- **本机计时的局限**：是在负载 7～10 下量的，只能看趋势和 HEAD / 基线的相对差，没有在空机上再量一组。

## 9. 改动、证据、复现、清理

**提交**：

- `1431d47`：只改 `tests/MusicAnalyzerTests.cpp`，让 ⑥ 的用例多打一行三道门实际放开的时刻，只记录、不断言。
- `27ad72a`：证据目录。
- `Source/**` 没有改：没有新增失败，也没有新增警告。

**证据**（`docs/validation/2026-10-02-T048-phase1/P1T-spark/`）：

| 文件 | 内容 |
|---|---|
| `targeted-tests.tsv`、`targeted-head-9e8575a.log`、`targeted-base-a2daf2b.log` | 第 2 节 |
| `full-suite-1431d47.log` | 第 7 节 |
| `reverse-b2-tests-on-a2daf2b.log` | 4.1 节 |
| `reverse-a2-r1r3d3-on-a2daf2b.log`、`LiveProfileReverse-for-a2daf2b.cpp.txt` | 4.2 节；把 `.cpp.txt` 插在 `a2daf2b` 的 `tests/LiveProfileTests.cpp` 末尾 `}` 之前就能编 |
| `reverse-quiet-gates-test-on-1200248.log` | 3.2 节 |
| `diag-quiet-gates-long.cpp.txt`、`diag-quiet-gates-long-results.txt` | 3.3 节 |
| `selftests-9e8575a.log` | 第 5 节 |
| `perf-rounds.tsv` | 第 6 节逐轮原始数据 |

**复现**（本机）：

- 基线源码在 `~/.cache/celadon-spark/src/a2daf2b/`（`git archive a2daf2b` 解出来的，本代理建的，留着）。
- B2 反向：复制一份基线，换进 `b2-oldtests/` 的两份测试，跑 `spark-test.sh <副本> test --only MusicAnalyzerLive,SegmentLedger`。
- A2 反向：复制一份基线，把 `LiveProfileReverse-for-a2daf2b.cpp.txt` 插进 `tests/LiveProfileTests.cpp` 末尾，跑 `--only LiveProfileReverse,LiveProfile`。
- ⑥ 反向：`git archive 1200248` 解开，换进 `9e8575a` 的 `tests/MusicAnalyzerTests.cpp`，跑 `--only MusicAnalyzerLive`。
- 长时诊断：把 `diag-quiet-gates-long.cpp.txt` 加进 `MusicAnalyzerLiveTests`，在 `runTest` 里 `testStartFromBarN()` 之后调用。

**清理**：

- 删掉了：
  - `~/.cache/celadon-spark/tmp/P1T/`；
  - 本代理那几份源码副本的构建目录：`build/src-main-23e4691fbd`、`rev-b2-*`、`rev-q6-*`、`rev-a2-*`、`diag-head-*`、`final-src-*`，连同 `.lock`。
- 留着：
  - 本工作树的构建目录 `build/wf_b646ec6c-573-1-dbb1e7bb58`；
  - 共享基线 `src/a2daf2b` 及其构建目录 `build/a2daf2b-5dc46c7e0e`。
- 没动：t048 工作树里的 `b2-oldtests/`、`b2-*.sh`（只读用过）；cal 素材目录。