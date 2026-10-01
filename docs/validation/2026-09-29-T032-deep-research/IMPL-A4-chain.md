# T-032 实现报告 · A4-chain · 链路 / 处理器 / 表 / 发行安全 / 参数真源 / 渲染工具

> 2026-10-01（UTC），实施代理第三次接续（前任依次：第一任额度中断，主进程保全为 `c258e3f`；第二任断网中断，保全为 `83b8ebe`；
> 第三任 107 断网，留下两个从未编译过的 WIP `e46e57c` / `610175c` 与本报告的初稿）。本轮的实施代理写完报告正文后中断，
> 编译警告、性能复跑、处理器 harness、渲染工具、试合并几处数字由主进程按它留下的运行日志补齐。
> 工作树 `/home/user/wt-a4`，分支 `worktree-wf_abb55518-7ae-4`，改前的底 = `e6b445b`（main，0.1.8.5 源码）。
> 任务书 `BRIEF-A4-chain.md`；研究 `R7-analysis.md` / `R8-chain-juce.md` / `R10-design.md`，核验 `V7` / `V8` / `V10`（`claude/fable-deep-research-634a5e` 同名目录）。
> **验证全部在云端容器上做，不是 107**：Ubuntu 24.04、GCC 13.3、glibc 2.39、4 核（与其他代理共用，同一时刻最多两个编译 / 测试），
> Release + Ninja + `-ffp-contract=off`（CMake 自带），用主进程给的 `/home/user/celadon-test` / `slot-run`（等价于 107 的 `scripts/test-linux.sh`）。
> 107、109 与 Mac 都连不上：没跑 MSVC、没跑 Mac / clang、`tools/perf/bench.mm` 只能读（Objective-C++）。

## 结论一览

| 条目 | 做了什么 | 改输出 | 用例 | 改前（e6b445b + 同一用例） | 改后 |
|---|---|---|---|---|---|
| R8-01 + R10-03 退出预听不阶跃 | 主进程合并修法：在线、链照常出母带、Listen 关 → 发行端自己 50 ms 斜坡回 1；宿主旁通 / 听原声 / 试听差值 / Listen 开 → 先把此刻的衰减交给 Listen 的 20 ms 平滑器（下限 1e-6）再归一；离线立即归一。另补「退出斜坡还没走完又切」的交接 | 只改退出预听后 ≤ 50 ms 的监听信号 | `CeladonChainMonitoring::testPreviewExitNeverSteps`（5 种退出 × 参照链同步切换） | 四种退出单样本跳 0.695（稳态预听 −10.32 dB） | 每样本最大变化 0.000290（= 50 ms 斜坡理论值）/ 0.001236 / 0.001236 / 0.001102；Listen 开（对照）0.000534 前后相同 |
| R8-03 听原声冻结 OUT 整体响度 | `LoudnessMeter::setIntegrationPaused`；被暂停碰过的子块不进门块，恢复后要 4 个干净子块；链 `setIntegrationPaused (compareOriginal \|\| (hostBypassed && pauseIntegrationOnHostBypass))` | 否（音频逐位不变；改读数与预听归一化的输入） | `testOriginalFreezesIntegrated`、Guards 的暂停 / 宿主旁通口径 | 听原声 6 s 整体响度被拉低 2.03 LU，回母带 6 s 后仍低 1.27 LU | 0.000 / +0.002 LU |
| R8-04 Reset 时保持衰减 | `ReleaseSafety` 记住最近一次有效读数算出的目标；「暂时没有读数」（≤ −90）且此前有过有效读数 → 保持；从未有读数 / `reset()` / 关预听 / 离线 → 归一；非有限读数 → 回 1 并忘掉（既有用例要求，见逐条）。不设超时 | 只改 Reset 后 ≤ 400 ms 的监听 | `testMeterResetKeepsPreview`、Guards `testReleaseSafetyHold` | Reset 后预听回到 0.000 dB（稳态 −10.32） | 2 s 内最高 −10.323 dB |
| 核验漏项：整链 reset 归一预听 | `ReleaseSafety::resetDitherSequence()`；`CeladonChain::reset()` / `restartWithFadeIn()` 只回种子 | 只改同规格 prepareToPlay / 中途重启后的监听 | `testResetKeepsPreviewGain`（两种）、`testDitherRestartsOnReset` | 之后 0.5 s 预听最高 −0.328 dB（先回 0 dB 再滑回） | −10.284 / −10.275 dB；抖动 reset == 新 prepare 逐位 |
| R7-04 零风险版 | `updateIntegrated` 两遍只扫到 `histogramTopBin`，第二遍从相对门所在格起 | 否（逐位不变） | `ChainDigest` meter-sweep | — | 摘要改前改后逐字相同 |
| R10-08 表对 NaN 自愈 | `finishSubBlock` / `publishSubBlock` 前判能量非有限 → 只清坏声道的 K 加权状态、丢本子块、断开门块连续性；直方图永不吃非有限值（顺带消掉 `(int) floor (+Inf)` 的 UB）；处理器守卫保留；渲染工具 stderr 报一次 | 否（有限输入逐位不变） | `testNonFiniteReachingMeters`（单表 / 整链，NaN 后推响 +6 dB） | 短期读数从此 −100、整体读数冻结；整链 IN 表短期 −100、GLAZE 聆听停在 2.90 s | 读数跟着走（单表 I −15.665 → −10.760、S = 单独量的 −9.644）；GLAZE 3.80 → 7.80 s |
| R10-01 默认值真源 + 实际参数无损回喂 | 新 `Source/ParameterDefaults.h`；参数表 / 渲染工具 / bench 都取它；Debug 处理器逐项核对；`MasteringSettings::harshResolved / targetResolved`；`effective.mastering` 回写实际值（模式改手动 / 显式形状，`codecMarginDb` 置 0）；渲染工具 expressible、`--selftest` 两条新对照、metrics.md 头部、README | 插件否；渲染工具 GLAZE > 0 成品会变（目的） | `testEffectiveParamsReplay`、`testFactoryDefaults`、`--selftest`、处理器 harness A | 回喂 GLAZE 0 链最大样本差 0.052 | 逐位相同（回喂原始设置仍是 0.052，对照有牙） |
| R10-02 存档格式号（前半） | 根元素写 `celadonStateFormat=1`、`celadonVersion`；读出传给 `addMissingParameterNodes` 作锚点，两个属性不进状态树；format 0 特判照旧 | 否 | 处理器 harness B（真处理器，Debug / Release）；bench `statecheck` 两条（Mac，只读核过） | 改前存档没有两个属性 | 往返逐字节相同；format 0 / 1 读出逐项相同；旧工程特判照旧 |
| R8-02 处理器第二道 + 链级 | `readParams` 每个原始值 `finiteOr (值, 新实例默认)`（Lookahead 交 `LookaheadHold`）；链里 GLAZE、高级母带百分比 / 预听目标、Limit Ceiling（扣余量之前）按默认算 | 有限值逐位不变 | Guards `testNonFiniteParams`（34 字段 × 3 坏值；Color 5 / Limit 4 字段在 T-032 集成后打开，`df48815`）；处理器 harness C | 链级 75 对里 18 对失败（Ceiling 5、GLAZE +Inf、高级母带 12） | 75 / 75；处理器：状态里写 "nan" / 原始值写 NaN·±Inf，输出与默认状态逐位相同 |
| R10-06 数值环境 | 删 `processChain` 里重复的 `ScopedNoDenormals`；渲染工具 `main` 开头一次 | 插件否；渲染工具只在 < 1.2e-38 量级 | 渲染改前 / 改后对照 | — | GLAZE 0 两组成品正常范围的样本逐位相同；只有末尾 0.5 s 静音尾巴里 30,334 个非规格化样本（≤ 1.175e-38）被清零 |

全套（改后 `a16d571`）：**133388 过 / 2 败**，两条都是性能预算（`ColorModule` 的「Color GLAZE 100 1.406 %（限 1.2 %）」、`BalancePerformance` 的「@96000 mean 11.486 %（限 10 %）」），
同一台容器上未改的 main（`1e6b0f5`）全套同样是这两条失败（1.296 %、11.353 %；132979 过 / 2 败）——按主进程口径属环境，见「验证」第 3 条。
后一条的预算 main 已由 Max 定为 15 %（PR #8），合并后不再报败；合进 main 的集成全套见 `REPORT.md`。
功能与逐位用例全部通过；新增 409 个断言（132981 + 409 = 133390）。编译警告：干净重编 `celadon_tests`（55 个编译单元）只有既有两条——
`ColorModule.cpp:404` 经 stl 的 `-Wstringop-overflow`、`ChainTests.cpp:168`（原 162，本线在前面加了几行）的 `-Wrange-loop-construct`；
`celadon_ui_snapshot`（处理器 + 编辑器）改前、改后各干净重编一次（66 个编译单元），都只有 `ColorModule.cpp:404` 一条。本线新增 0 条。

## 验证

### 1. 命令

```bash
# 改后：定向 / 全套（celadon-test = configure + 编 celadon_tests + 运行，构建目录 /home/user/build/wt-a4）
/home/user/celadon-test /home/user/wt-a4 --only CeladonChain,CeladonChainClay,CeladonChainMonitoring,CeladonChainGuards,ChainDigest,LegacyFixture,AdvancedMastering,AdvancedMasteringChain,LoudnessMeter,GlazeCalibration,GlazeChain
/home/user/celadon-test /home/user/wt-a4
# 改前：git worktree add --detach /home/user/a4-base e6b445b；把本线的 tests/ChainTests.cpp（CeladonChainMonitoring 整段 + Guards 移植版，见第 4 条）
# 与 tests/LegacyFixtureTests.cpp（ChainDigest）放进去，源码不动
/home/user/celadon-test /home/user/a4-base --only CeladonChainMonitoring,CeladonChainGuards,ChainDigest
# 渲染工具（在同一构建目录里另编 EXCLUDE_FROM_ALL 目标）
/home/user/slot-run cmake --build /home/user/build/wt-a4 --target celadon_render && /home/user/build/wt-a4/celadon_render --selftest
# 处理器 harness（临时程序，不进仓库：复用工作树自己的 celadon_ui_snapshot 目标、把 SnapshotMain.cpp 换成 harness.cpp；改前 / 改后 × Debug / Release）
cmake -S /home/user/scratch/a4/harness -B <构建目录> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCELADON_SRC=/home/user/wt-a4 && cmake --build <构建目录> --target a4_harness a4_harness_neg
```

### 2. 用例抓得住缺陷：同一份用例放回改前源码

`CeladonChainMonitoring` 整段只用 `e6b445b` 就有的公开接口，原样放回改前源码。`CeladonChainGuards` 用到本线新接口，改前编不过，
放回去的是**移植版**：去掉 `ParameterDefaults.h`、`testHostBypassIntegrationPolicy` / `testPauseExcludesTouchedSubBlocks` / `testFactoryDefaults`
三个只测新接口的用例、`holdsLastValidTargetForTesting()` / `resetDitherSequence()` 的几行、非有限参数里 `mastering.harshResolved / targetResolved` 两个新字段，
其余逐字不动。

| 用例 | 改前（`e6b445b`） | 改后 |
|---|---|---|
| Listen 关着关预听 | 增益比单样本跳 0.695316（稳态预听 −10.32 dB）；「不快于 50 ms 斜坡」同样失败（0.695 > 0.000290） | 每样本 ≤ 0.000290 |
| 预听中切原声 / 宿主旁通 / 试听差值 | 三种都单样本跳 0.695316 | 0.001236 / 0.001236 / 0.001102；宿主旁通第一块预听读数 0 dB |
| Listen 开着关预听（对照） | 0.000534（过） | 0.000534 |
| 听原声 6 s 冻结整体响度 | 整体响度被拉低 2.0298 LU，回母带 6 s 后仍低 1.2691 LU | 0.000 / +0.002 LU；短期 −15.665 = 单独量原声 |
| 预听中点表 Reset | Reset 后 2 s 内预听最高 0.000 dB（稳态 −10.323） | −10.323 dB |
| 同规格 prepareToPlay / restartWithFadeIn | 之后 0.5 s 预听最高 −0.328 / −0.328 dB | −10.284 / −10.275 dB |
| reset 仍把抖动序列回到种子（回归） | 过 | 过 |
| 单表 NaN 后推响 +6 dB | 短期 −100.000（「无读数」）、整体冻结在 −15.665 | 短期 −9.644（单独量 −9.644）、整体 −10.760 |
| 整链一块 NaN 输入、之后 +6 dB | IN 表短期 −100、整体冻结、GLAZE 聆听停在 2.90 s | IN 短期 −9.699、整体 −12.702 → −8.018、聆听 3.80 → 7.80 s；输出全部有限、NaN 到达那一块清零重启 |
| 实际参数回喂 GLAZE 0 链 | 最大样本差 0.052231（两项「自动」被算成 0、编码余量再扣一次） | 0（逐位）；回喂原始设置仍是 0.052231 |
| ReleaseSafety 读数暂时没有时保持 | 失败：0 dB（应 −6 dB） | −6.000 dB；坏读数回 1 并忘掉、reset / 关预听归一都过 |
| 非有限参数（链级，75 对） | 18 对失败：`limit.ceilingDb` NaN 静音、+Inf 真峰值 −0.350 dBTP 超 Ceiling −1.300、三种都不逐位（5 条）；`glazePercent` +Inf（1）；`mastering.harshAmount / transientPreserve / targetAmount / previewTargetLufs` × 3（12） | 75 / 75 |

合计改前 **295 过 / 35 败**（Monitoring 16 + Guards 移植版 19），改后同三个类 **409 过 / 0 败**。
「Listen 开时关预听」「reset 仍回种子」两条是对照 / 回归项，改前改后都过。

另核前任的说法「`colorLimitGuardsMerged` 打开后 Color / Limit 字段在本线源码上崩」：本线临时改成 `true` 跑 `--only CeladonChainGuards`，
`color.toneDb` 三种坏值（NaN 还静音）、`color.glue = +Inf` 与回退值不逐位，跑到 `color.heat` 时进程段错误（exit 139）——属实，开关改回 `false`。

### 3. 全套与环境性失败

| 树 | 全套 | 失败 |
|---|---|---|
| 未改的 main `1e6b0f5`（主进程在本容器的基线 `/home/user/logs/main.log`） | 132979 过 / 2 败 | Color GLAZE 100 1.296 %（限 1.2 %）；@96000 mean 11.353 %（限 10 %） |
| 改后 `a16d571` | 133388 过 / 2 败 | Color GLAZE 100 1.406 %；@96000 mean 11.486 % |

两条都是同一个性能用例在 main 上也失败、幅度相近（容器 4 核与其他代理共用）。本线没碰 `ColorModule` / `BalanceModule`；链里只多几个分支与两遍扫描变短。
单独复跑这两个类（同一台机器先后各一遍，`--only ColorModule,BalancePerformance`）：改前 `e6b445b` Color GLAZE 100 1.260 %、@96000 mean 11.419 %；
改后 `a16d571` 1.264 %、11.384 %——两边都是 684 过 / 2 败、同两条，改前改后一致，属环境。

### 4. 逐位证据

**`ChainDigest`（`tests/LegacyFixtureTests.cpp`，七个场景，FNV-1a 64；同一份测试在改前 / 改后各跑一次）**：

| 场景 | 覆盖 | 音频 | 读数 | OUT 整体响度 |
|---|---|---|---|---|
| manual-busy | GLAZE 0，四模块都在做事，Listen、手动推响不刺 / 暖色目标、余量 0.5、16 bit | `14c23c589c9a10c9` = | `d87075eda3fe55f0` = | `d0aa3ecfe8ce7485` = |
| glaze-auto-defaults | GLAZE 35 + 两项自动 + 余量 0.3（聆听 → 过渡），R10-01 回写路径 | `4bf945a6d7dc2323` = | `974827b3b1dbb965` = | `d1326040cb49cef7` = |
| preview-steady | 稳态预听（交接进入、读数保持路径） | `43a5f132baa849c7` = | `bf763e26c0e96dcf` = | `a0732c36fe289a14` = |
| preview-listen-exit | Listen 开时退出 / 再进 / 离线关掉，24 bit | `b2d1cc0dc4a2a456` = | `57ecae1cb8d0befb` = | `664dc2db328108e0` = |
| monitor-toggles | 没开过预听，宿主旁通 / 听原声 / 试听差值来回切 | `9ca2cb0c4afbe443` = | `9b79feb9688b700b` = | 改前 `327282438585aaf8` → 改后 `8ca48394b50fd780`（R8-03 有意：听原声那 0.5 s 不积分，末值 −11.898 → −11.615 LUFS） |
| reset-paths | restartWithFadeIn / setParams + reset / 界面 Reset，24 bit | `05977d4ff3ca916a` = | `5ed9b4410750dd20` = | `f0abe14883a5c495` = |
| meter-sweep | 单表 60 s，电平每 0.7 s 跳、块长轮换、中途 Reset（R7-04 两遍扫描的上下界） | — | `6521e096b6bf16ae` = | `d5066b8a43585ee8` = |

「=」= 改前改后逐字相同。前任写进表里、注明「107 实测」的那一版：音频与整体响度摘要与本容器逐字相同，读数摘要七个都不同
（读数里有 float 的 dB 换算，多半是 libm 的 `log10f` 版本不同）；`baselineDigests` 已换成本容器的改前值。

**处理器 harness**（真 `CeladonAudioProcessor`；48 kHz / 256；同一份 harness 在改前 / 改后两棵树上编）：

临时程序（不进仓库，`/home/user/scratch/a4/harness/`）在改前 `e6b445b`、改后 `a16d571` 两棵树上各按 Debug / Release 编一份，另编一份「故意改错一个默认值」的阴性对照：

| 段 | 内容 | 改后（Debug / Release） | 改前（Debug / Release） |
|---|---|---|---|
| A | Debug 构造里的默认值真源核对（`PluginProcessor.cpp` 的 JUCE_DEBUG 段），看 stderr 有没有它的断言 | 通过：新实例 GLAZE 35；构造、B、D 三段 stderr 里没有任何断言 | 通过（改前没有这道核对） |
| B | 存档格式号（R10-02） | 全过：根元素 `celadonStateFormat=1`、`celadonVersion=0.1.8.5`；两个属性不进状态树；存 → 读 → 存逐字节相同（13584 字节）；format 0（去掉两个属性）读出逐项与 format 1 相同、再存逐字节相同；format 0 的旧工程缺参特判照旧 | 两个属性那两条不过（改前不写；13536 字节），其余同 |
| C | 处理器第二道非有限守卫（R8-02）：状态里浮点参数写 nan / inf / −inf，或直接把原始值存成 NaN / ±Inf | 全过：`"nan"` 进状态后 22 个浮点参数的原始值确实非有限，输出与默认状态逐位相同（`484e74e318bb11cd`）；`"inf"` / `"-inf"` 被 APVTS 钳到量程端点，不需要守卫；原始值直接存 NaN / ±Inf 三种，输出都与默认状态逐位相同 | 进程段错误（exit 139）——正是 R8-02 的缺陷 |
| D | 有限参数逐位：4 组设置渲染 8 s 的输出摘要 | `978dd266c9ec5c41` / `a5ed08bb62f1c228` / `06377fb22ef1e153` / `4cd9bd0d7101f22c` | 与改后逐字相同（Debug 与 Release 也相同） |

- 改后两份都是 `0 failure(s)`、exit 0。
- 阴性对照（改后源码，一个默认值故意改错，Debug）：第 A 段「新实例 GLAZE 35」不过，`PluginProcessor.cpp:307` 的核对断言在构造、B、C、D 各段一共触发 17 次——说明这道核对真能抓到默认值走样。
- stderr 里另有的两类 JUCE 断言都只出现在 C 段（故意写非有限值的那一段），与本线无关：`juce_NormalisableRange.h:265` 改前改后都是 110 条（往参数里写 NaN / Inf 时 JUCE 自己的检查）；`juce_Messaging_linux.cpp:87` 只在改后出现（111 条），因为改前在 C 段开头就段错误、走不到后面，而这个临时程序没有跑消息循环。

**渲染工具**（`celadon_render`，改前 / 改后各编一份，同一首 26 s 合成歌，GLAZE 0 / 35 × 修坯 自动 / 全关）：

- `--selftest`：改后 17 条全过（改前 15 条），多出的两条是本线加的——「重放对照覆盖高级母带：1911 块推响不刺与目标频谱强度都 > 0（出厂默认 GLAZE 35 × 两项自动）」「重放对照抓得住未解析的高级母带：回喂原始设置时最大样本差 −44.6 dBFS（期望 > −120 dBFS）」。合进 main 后（集成树）同样 17 条全过。
- 成品逐样本比（`cmpwav.py`）：GLAZE 0 两组（修坯 自动 / 全关）正常范围的样本逐位相同，差异只在末尾 0.5 s 静音尾巴里的 30,334 个非规格化样本（≤ 1.175e-38，R10-06 的 FTZ 改到 `main` 开头一次）；GLAZE 35 三组按设计改变（R10-01：渲染工具的出厂参数改取真源，GLAZE 35 + 两项自动，最大样本差 0.11）。
- 两边都报「1 处失败」，而且是同一处：GLAZE 35、修坯「自动」那一组的同决策重放阳性对照不通过（重放与自动链成品最大差 −37.6 dBFS，允许 ≤ −180 dBFS，ADR-016）。改前 `e6b445b`、改后、合进 main 后的集成树三处数值完全相同——是本线之前就有的问题，不是本线引入的，列「待 Max」第 5 条。

### 5. 试合并（主进程参考，未提交）

临时工作树 `/home/user/a4-merge` = `claude/fable-deep-research-634a5e`（`3ec4648`，已含 A2 / A3）+ 本线改动，不提交：

| 步 | 改动 | 跑的 | 结果 |
|---|---|---|---|
| 1 | `tests/ChainTests.cpp` 的 `constexpr bool colorLimitGuardsMerged = false;` 改成 `true`（Color 5 个、Limit 4 个字段同样判定） | 全套 | 134266 过 / 1 败，败的只有当时 10 % 的计时预算（@96000 mean 10.643 %） |
| 2 | 只把 `CeladonChain.h` 的 `pauseIntegrationOnHostBypass` 改成 `true`（宿主旁通也冻结 OUT 整体响度） | `--only GlazeChain,CeladonChainGuards` | `GlazeChain` 的「OUT integrated」逐位对照失败（mode 2 宿主旁通的块，各块长 / 声道数都失败）——与「待 Max」第 1 条的预期一致 |
| 3 | 在 2 的基础上，`GlazeTests.cpp` 参照 OUT 表在宿主旁通的块同样 `setIntegrationPaused (hostBypassed)`（跨文件需求 1 的那一行） | `--only GlazeChain,CeladonChainGuards,CeladonChainMonitoring,ChainDigest` | 640 过 / 0 败 |

- 第 1 步就是集成时要做的事：已在 T-032 集成分支提交 `df48815`（「A2 / A3 已与 A4 合在一起，打开 CeladonChainGuards 里 Color / Limit 的 9 个守卫字段」），随 PR #10 合进 main；集成全套见 `REPORT.md`。
- 第 2、3 步只是为 Max 拍板准备的验证：开关仍是 `false`，`GlazeTests.cpp` 没动。

## 逐条说明

### R8-01 + R10-03 · 退出归一化预听不再单样本阶跃

- 位置：`Source/dsp/CeladonChain.cpp:180-212`（`leaving` / `previewGainNow` / `previewGainPending` / `rampsItself`），头注 `CeladonChain.h:38-49`。
- 逐情形：在线 + 链照常出母带（`releaseAllowed`）+ Listen 关 → 不 reset，`ReleaseSafety::process` 在 `previewEnabled = false` 下按 50 ms 线性斜坡回 1，
  走完精确落 1、转回逐位旁通；离线 → 立即 `reset()`（导出第一个样本起不带预听、不带尾巴，既有用例 `offline transition removes preview from its first sample` 照过）；
  其余在线情形（宿主旁通 / 听原声 / 试听差值时 `releaseSafety.process` 不再被调、斜坡走不动；Listen 开时 Listen 要接手）→
  `listenGain.setCurrentAndTargetValue (jmax (1e-6f, listenGain × previewGain))` 再 `reset()`，随后 `updateListenTarget()` 让 Listen 的 20 ms 乘法平滑器走到目标
  （Listen 关或干路 = 1）。预听期间 Listen 增益恒为 1（进入预听时交接成 1、`listenRequested` 为假），所以「× listenGain」在刚退出那一块恰好等于任务书的
  「交接 previewGain」、逐位同值（`ChainDigest` preview-listen-exit 改前改后相同即是这一支）。
- 比任务书多做的一处：`previewGainPending`（预听增益还没回到 1）——Listen 关着退出、50 ms 斜坡还在走时又切原声 / 宿主旁通 / 试听差值 / 开 Listen，
  同样把此刻的剩余衰减交给 Listen，不在块边界跳；重新打开预听时把 `Listen × 剩余衰减` 交给预听（预听增益为 1 时与改前逐位相同）。
- 宿主旁通：监听增益 20 ms 交接（与模块自身旁通的 20 ms 交叉淡化同量级），预听读数从第一个旁通块起是 0 dB（R10-03 核验保留的原意，用例断言）。
- 预听从未打开时：`previewGainNow == 1.0` 精确成立（`reset` 与斜坡末尾都精确落 1），新分支一个都不进，逐位不变（`ChainDigest` monitor-toggles 的音频 / 读数）。
- 试听差值结束后的 20 ms 淡出里不跑 `releaseSafety.process`（核验漏项 2，cosmetic，按任务书只记一句）：这 ≤ 20 ms 不加抖动；差值试听本身是临时监听，不进导出。

### R8-03 · 听原声期间 OUT 表整体响度暂停积分

- 位置：`LoudnessMeter.h` `setIntegrationPaused` 与头注；`LoudnessMeter.cpp:230-232`（整块在暂停中就给正在累计的子块打「碰过」）、`:326-341`
  （碰过的子块不进门块、连续计数清零，恢复后 `cleanSubBlocksInRow` 攒够 4 个才成门块——核验要求的「恢复时重攒」）；链 `CeladonChain.cpp:244-247`。
- 暂停时 K 加权、子块环（`processedSamples`、GLAZE / Listen 配对）、短期、真峰值照常（`GlazeCalibration` 类全过）；从未暂停、输入有限时 `cleanSubBlocksInRow` 恒为 4，
  与改前同一判断（meter-sweep 摘要相同）。
- 偏保守的一处（头注里写了）：暂停中那一块恰好在子块边界上结束时，紧接着的子块（没有暂停中的样本）也按碰过算，恢复后多等 100 ms；暂停中的样本一个都漏不进
  （Guards `testPauseExcludesTouchedSubBlocks`：暂停短于一个子块且在中间 / 跨很多子块两头不齐 / 两头对齐，三种都与只听稳定音的表差 < 0.01 LU）。
- 试听差值不暂停：表在差值换上输出之前读的是母带（`processSlice` 顺序），本来就对。
- **宿主旁通一半：开关已接、现为关**——`CeladonChain.h:85` `static constexpr bool pauseIntegrationOnHostBypass = false;`。没按任务书直接 `setIntegrationPaused (dryPath)`：
  `tests/GlazeTests.cpp` 的 `testGlazeZeroEqualsReferenceChain`（不归本线）逐位比「链的 OUT 表 == 一块普通的表」，mode 2 里宿主旁通每 90 块来回切，接上就失败、
  而那个文件我不能改。Max 确认冻结后改成 `true`，并在该用例 `referenceOut.process (r);` 前加一行 `referenceOut.setIntegrationPaused (hostBypassed);`（跨文件需求 1；
  两步都在试合并树上验过，见「验证」第 5 条）。`CeladonChainGuards::testHostBypassIntegrationPolicy` 按常量两种取值各有断言，翻转时自动换判据。

### R8-04 · 预听中点表 Reset 保持衰减

- 位置：`ReleaseSafety.cpp:68-100`（`process` 三分支）、`:38-50`（关预听忘记记忆）、`:24-36`（`reset` / `resetDitherSequence`）；头注 `ReleaseSafety.h`。
- 按核验去掉 3 s 超时：读数只经清表变成「无读数」；prepare、离线、干路都走 `reset()` = 从未有读数 → 1。
- **与任务书字面的一处区分（有意）**：规格 §6 的「读数未知」含「≤ −90 LUFS」与「非有限」。实现只对前者保持；非有限读数回 1 并忘掉记忆。原因是既有用例
  `tests/AdvancedMasteringTests.cpp:155-158`「invalid metering recovers unity and disabled preview becomes bit-identical」（A1 的文件）在刚有过 −8 LUFS 有效读数之后
  喂 NaN、断言预听回 0 dB——照字面「非有限也保持」会让它失败，而本线不能删改别的线的断言。R10-08 之后链里的表不会再给出非有限读数，这一支只是防御，
  两种取舍都不比改前危险（预听只衰减）。写进「待 Max」第 2 条。

### 核验漏项 · 整链 reset 不再归一预听

- 位置：`ReleaseSafety::resetDitherSequence()`；`CeladonChain.cpp:59-73`（`reset`）、`:75-99`（`restartWithFadeIn`）。
- 任务书写「chain.reset() 只清前者并把 previewActive 置 false 让下一块重新交接」。实现保留增益、也保留 `previewActive`：增益保留时「重新交接」只是把
  `1 × 当前预听增益` 交回预听（预听期间 Listen 恒为 1）；下一块若预听已关，`leaving` 与 `previewGainPending` 走同一组分支。两种写法唯一的差别：置 false
  会让下一块的 `setCurrentPreviewGain` 打断正在走的 50 ms 平滑、从当前值重新起一段 50 ms——保留 `previewActive` 少这一下，也少一个状态改动。
- 抖动：`reset()` / `restartWithFadeIn()` 仍把 TPDF 序列回到固定种子（导出可复现）：`testDitherRestartsOnReset` 断言「跑过再 reset == 新 prepare」逐位相同。

### R7-04 · 两遍扫描只扫有数的格

- 位置：`LoudnessMeter.cpp:361`（`histogramTopBin`）、`:366-415`（`updateIntegrated`）。
- 第一遍 `[0, topBin]`：更高的格子能量 0.0、计数 0，`x + 0.0 == x`（能量非负，没有 −0），求和顺序与每步舍入不变。第二遍先按算式估起点，再用与原判断
  同一个表达式 `binCentre >= relativeGate` 向两边校正到真正下界（格中心随格号单调不减：`(float) bin + 0.5f` 精确，除以 10、加常数都保序），纳入的格子与扫满
  800 格一模一样；直方图空时 `topBin = −1`，两个循环都不进。读数逐位不变（meter-sweep：60 s、相对门来回移动、中途 Reset）。
- 两遍合计从 1600 次格访问降到（topBin + 1）+（topBin − 相对门所在格 + 1）：典型母带（最高格约 −4 LUFS、相对门约 −20 LUFS）约 820 次。没做计时（容器噪声大），
  不作性能结论。
- 不做 `setReadoutEnabled`（核验建议不做）。

### R10-08 · 表对 NaN 自愈

- 位置：`LoudnessMeter.cpp:95-102`（`healFilterStates`：只清状态已坏的声道）、`:184-187`（`publishSubBlock` 前判 `ringEnergy`）、`:298-310`（`finishSubBlock` 开头判 `subBlockEnergy`）。
- 两套子块网格（读数网格可被界面 Reset 重起、子块环不断）先后发现同一个 NaN 时，后一次不会把已经恢复的状态再清一遍。子块环里那个坏子块按既有约定仍以非有限
  `meanSquare` 发布（分析器、Listen、校准器、渲染工具本来就跳过它）。
- 改前机理（本容器复现）：NaN 能量经 `energyToLufs` 成了 −100（`jmax (−100, NaN)` 取前者）→ 短期读数从此 −100、门块被绝对门挡掉 → 整体读数冻结；+Inf 能量的门块
  让 `(int) std::floor (+Inf)` 成了未定义行为。现在直方图永不吃非有限值。
- 处理器守卫保留（频谱仪 IN 取样口在链之前，核验①）；链照旧不管输入（`GlazeTests` 的 v0.5 参照链对照在 mode 2 往两边都塞 +Inf，固化了这一点，照过）。
- 渲染工具：读 wav 时本来就逐样本把 NaN / Inf 清成 0，现另在 stderr 报一次（`RenderMain.cpp:2470-2476`）。本容器实测：含 3 个 NaN / Inf 样本的 8 s 合成歌，stderr 报「注意：输入里有 3 个 NaN / Inf 样本，已按 0 处理（插件在处理器层把含非有限值的整块清零，这里逐样本清）」，渲染 0 处失败。

### R10-01 · 默认值一处真源 + 实际参数无损回喂

- 新文件 `Source/ParameterDefaults.h`（`factoryChainParams()` 在 `:79`）。与 `ParameterRanges.h` 的一处不同：要返回 `ChainParams`，所以包含 `dsp/ChainParams.h`
  （只依赖 juce_dsp；插件、无头测试、渲染工具、bench 都编得过），标量常量本身不依赖 JUCE。`Celadon.jucer` 照 `ParameterRanges.h` 加一条 file 条目（compile="0"，id 唯一，
  XML 解析通过；T-032 分支上 `ab36160` 的 CMake 核对只看 `Source/dsp`、`Source/ui` 的 .cpp，不涉及）。
- `Parameters.cpp` 全部默认值改读 `defaults::`；处理器 Debug 构造 `PluginProcessor.cpp:259-308` 用 `readParams()` 读参数表默认（原始值 = `convertFrom0to1 (getDefaultValue())`），
  与 `factoryChainParams()` 逐项核对（浮点 `approximatelyEqual`：对数刻度往返会漂几个 ulp；选项与开关按值），`static_assert` 项数 == `allParameterIds`（36）。
  harness A：改后 Debug 构造时 0 条断言；阴性对照（`Parameters.cpp` 副本把 GLAZE 默认写成 30）构造时恰好 1 条 `PluginProcessor.cpp:307`。
- `MasteringSettings::harshResolved / targetResolved`（−1 = 未设，`MasteringFeatures.h`）；链 `CeladonChain.cpp:127-144`：模式不是「关」且已设 → 直接用（`jmin (1, x)`），
  未设分支与改前同一个表达式。回写 `:163-172`：`harshMode` → 手动（关仍是关）、`targetMode` → 解析出的显式形状（「参考」没学好 → 关）、`codecMarginDb` → 0
  （余量已扣进 `limit.ceilingDb`，核验 (a)）。插件不设 resolved；处理器只从实际参数里读 Balance / Color / Limit 七项（`publishEffectiveForUi`），GLAZE 编排器不读 mastering，
  `GlazeTests` 的 `sameParams` 也不比 mastering——输出与界面逐位不变（全套、`ChainDigest`、harness D）。
- 研究稿的 `targetResolvedMode` 没另加字段：直接把 `targetMode` 改写成解析出的形状，效果相同、少一个字段。
- 渲染工具：`makeFactoryParams()` 返回真源；`masteringReplayable`（「参考」形状 → n/a）并进两处 expressible；`--selftest` 加「重放对照覆盖高级母带」（有块两项强度都 > 0）
  与「重放对照抓得住未解析的高级母带」（把 mastering 换回原始设置重放，差 > −120 dBFS）；metrics.md 头部写本次高级母带设置；README 三处改指真源。
- bench：`chainParamsFor` 起点改 `factoryChainParams()`（核验 (b)）。**注意**：modules 模式不经 `CeladonChain::setParams`，高级母带的「模式 × GLAZE → 强度」、编码余量、
  发行端都是链那一层算的，modules 模式里仍不生效（Balance 一级 harsh / target 强度为 0）；这次统一的只是 GLAZE 默认（`default` 场景 0 → 35）。前任注释写成
  「同一条链（高级母带两项自动……）」，已改正。要两级完全同参数得在 `setAll` 里照链解析，见「没做的」。

### R10-02 · 存档格式号（只做前半）

- `PluginProcessor.cpp:57-64`（常量）、`:851-852`（写）、`:896-902`（读、摘掉、传入）、`:911-916`（`addMissingParameterNodes (newState, stateFormat)`，现为锚点、`ignoreUnused`）。
- 两个属性读完就从 XML 上摘掉，不进参数状态树（存档时再写当前值；harness B 断言）。
- 已知边角（不处理）：降级到 0.1.8.5 打开新存档时，老版本会把两个属性当根属性留在状态树里、再存时原样写回——之后新版本读到的格式号是「新存档的」。现在 format 0 / 1
  读法相同，无影响；以后真按格式号分支时，降级往返不在兼容承诺内。
- bench `statecheck` 加两条（`bench.mm:1160-1188`，Mac 专用，本线只能读）：逐行核过 API（`getXmlFromBinary` / `XmlElement` 拷贝构造 / `removeAttribute (Identifier)` /
  `copyXmlToBinary` / `allParameterIds` 遍历），与 harness B 的两条同义。

### R8-02 · 非有限参数：处理器第二道 + 链级

- 处理器 `readParams`（`PluginProcessor.cpp:391-461`）：每个原始值套 `finiteOr (值, 新实例默认)`；Lookahead 例外——`LookaheadHold` 把非有限当「没变」、沿用正在用的前瞻
  （`:532-535`），换成默认 2 ms 反而会因坏值改延迟。
- harness C（真处理器）：工程文件里浮点参数写 `"nan"` → 22 个原始值确实是 NaN（JUCE 9.0.2 原样放行）→ 输出与默认状态**逐位相同**；直接往原始值写 NaN / +Inf / −Inf
  同样逐位相同。顺带核实：工程文件里写 `"inf"` / `"-inf"` 时 APVTS 的 `clampTo0To1` 把它钳到范围两端（原始值有限，例如 Limit Gain 24、GLAZE 100），所以经工程文件进来的
  非有限值只有 NaN。改前同一 harness 见「验证」第 4 条。
- 链：`CeladonChain.cpp:110-115`（GLAZE 非有限 → 0，ChainParams 的默认）、`:130-144`（高级母带百分比 → 结构默认）、`:154-158`（**Limit Ceiling 在扣编码余量之前换默认**）、
  `:215`（预听目标）。Ceiling 这一处是前任最后一轮加的，任务书没点名：链先用 Ceiling 做减法再交给编排器与限幅器，NaN 扣完余量还是 NaN，A3 的模块守卫只能换成
  不含余量的默认值，与「该字段取默认值」的链差一个余量——根因在链，守卫就得在链（改前 Ceiling NaN 静音、+Inf 真峰值 −0.350 dBTP 超 Ceiling）。
- 链级用例 `CeladonChainGuards::testNonFiniteParams`：34 个浮点字段（含 `balance.*` 自动项、`clay.*` 实际值、`claySettings.*` 手动值、`mastering.*Resolved`）× NaN / +Inf / −Inf，
  其余参数把链推满（Limit Gain +24 dB、Glue / Heat / Clip、手动推响不刺 / 暖色目标 / 余量 0.3、修坯三项手动、预听开），判有限、不静音、真峰值 ≤ Ceiling + 0.01 dB、
  与回退值逐位相同。Color 5 个与 Limit 4 个（gain / release / lookahead / clip）字段原先等 A2 / A3：**唯一的开关** `tests/ChainTests.cpp`
  `constexpr bool colorLimitGuardsMerged`，本线分支上是 `false`；T-032 集成时已改成 `true`（提交 `df48815`，随 PR #10 合进 main），
  试合并树与集成全套都验过（见「验证」第 5 条与 `REPORT.md`）。

### R10-06 · 数值环境

- 删 `PluginProcessor.cpp:648` 的重复声明（`processChain` 只被 `processBuses` 调，后者 `:630` 已设）。
- 渲染工具在 `main` 开头一次 `juce::ScopedNoDenormals`（`RenderMain.cpp:2389`），没按任务书在 `renderPass` / `replayPass` / 自检里各放一个：工具单线程（无 thread / 线程池），
  `main` 的作用域盖住聆听 / 成品 / 重放三遍、自检与指标测量；三处再各套一层就是本条要删的那种嵌套重复。效果与任务书相同。
- 按核验，不在 `ChainTests` / `TestMain` 单独加；ADR-016 的切块差异不作为验收条件。

## 前任改动里本轮改正 / 补上的

1. **两个 WIP 从未编译过**：`e46e57c`（链级 Ceiling 守卫）与 `610175c` 在本容器上第一次编、第一次跑；连同前面的 WIP 一起编译 0 新警告、用例全过，没有要改的编译问题。
2. **NaN 用例抓不住「整体读数冻结」**：原写法只比「与出事前相差很小」，改前冻结在出事前的整体读数（单表 −15.665、整链 IN −12.70）照样过。改成 NaN 之后推响 +6 dB，
   断言读数跟着走（单表整体 > 出事前 + 3 LU、短期 = 单独量那段；整链 IN 整体 > 出事前 + 2 LU），改前多抓到两条（14 → 16）。
3. **`ChainDigest` 的改前摘要**：表里的值注明「107 实测」，本容器上读数摘要七个都对不上（音频 / 整体响度对得上），换成本容器的改前实测值并写明环境。
4. **bench 注释与报告初稿里「modules 模式与整插件模式是同一条链」不对**（见 R10-01 bench 一条），注释已改。
5. **未核实的数字**：测试注释与初稿里的「107 实测」「29 过 14 败」等全部换成本容器实测；前任说的「color.heat = NaN 段错误」在本容器复现属实。
6. 初稿「整链 reset 保留 previewActive 与任务书写法逐位等价」不准确，改为写明唯一差别（会不会打断正在走的 50 ms 平滑）。
7. 初稿说 `设计规格-总体.md` 末尾小节「本线已写（136 行）」：档案在 Mac 上，本容器看不到，无法核实；要加的全文写在「需要同步到档案的内容」，由主进程照抄 / 比对。

## 没做的与原因

- R7-04 ② `setReadoutEnabled`、R10-02 后半「format ≥ 1 查表」：任务书明确不做。
- R10-06 在 `ChainTests` / `TestMain` 加 FTZ：任务书明确不改。
- R8-02 模块内守卫：A2（Color）/ A3（Limit）线；本线用例里那 9 个字段等合并后开关打开。
- 宿主旁通冻结 OUT 整体响度：待 Max（开关与 GlazeTests 那一行都备好）。
- bench：只能读、不能编（Objective-C++，Mac 专用）；`statecheck` 两条与 harness B 同义，已在真处理器上跑过。modules 模式的高级母带解析没补（任务书只要求 `chainParamsFor`
  指向真源；要补得在 `setAll` 里照 `CeladonChain::setParams` 再写一遍或把那段提成公用函数，需要 Mac 编译验证，建议另起任务）。
- 22 首门槛 / 试听包重跑：属主进程与 Max（R10-01 之后工具成品会变；核验建议先定 ADR-020 待复核第 1 条）。
- 预存的编译警告 `tests/ChainTests.cpp:168`（`-Wrange-loop-construct`，GCC 13.3 才报，main 上同样有、行号 162）没顺手改：不是本线引入，改它要动上面「一行不动」的既有用例。

## 跨文件需求

1. `tests/GlazeTests.cpp`（`testGlazeZeroEqualsReferenceChain`，A6 / A7 动过的文件）：Max 确认「宿主旁通也冻结 OUT 整体响度」后，与 `CeladonChain.h:85`
   `pauseIntegrationOnHostBypass = true` 同一提交，在 `referenceOut.process (r);` 之前加一行 `referenceOut.setIntegrationPaused (hostBypassed);`。
2. 主进程合并 A2 / A3 之后：`tests/ChainTests.cpp:2687` `colorLimitGuardsMerged` 改 `true`，跑 `--only CeladonChainGuards`（Color 5 字段、Limit 4 字段 × 3 个坏值）。
   **已做**：T-032 集成提交 `df48815`，集成全套（默认与 `-march=x86-64-v3`）都含这 9 个字段，见 `REPORT.md`。
3. `设计规格-高级母带控制.md` §6（A1 在改）：下面「需要同步到档案的内容」第 2 节三句。

## 待 Max

1. **宿主旁通期间 OUT 整体响度是否冻结**（R8-03 另一半；研究、核验都建议冻结）。现为不冻结；开关、用例、GlazeTests 那一行都备好（跨文件需求 1）。
2. **规格 §6「读数未知时回到 1」改口径**（R8-04）：现实现 = 暂时没有读数（≤ −90，如界面 Reset 后约 400 ms）且此前有过有效读数 → 保持上次衰减；非有限读数 → 回 1
   （既有用例要求）。不设超时。
3. **门槛与试听包要按新工具重跑**（R10-01）：T-032 之前的成品是高级母带两项关着的链；GLAZE 0 成品不变。
4. **性能基准 modules 模式口径变了**（R10-01 b）：`default` 场景 GLAZE 0 → 35；高级母带强度在 modules 模式里仍不生效（与整插件模式在 Balance 一级还差这一块）。
5. **离线对照工具的同决策重放阳性对照在合成歌上不通过（本线之前就有）**：26 s 合成歌、GLAZE 35、修坯「自动」那一组，重放（修坯手动照填）与自动链成品
   最大差 −37.6 dBFS（ADR-016 要求 ≤ −180 dBFS），「重放全关」对照因此不可信。改前 `e6b445b`、本线改后、T-032 全部合进 main 后数值完全相同，
   与本线无关；要查是哪一项实际参数没被逐块照搬（待认领）。

## 需要同步到档案的内容

### 1. `01-框架/设计规格-总体.md` 末尾追加

```markdown
## 2026-09-29 修订（T-032）

T-032 深度研究 A4 线（链路 / 处理器 / 表 / 发行安全 / 参数真源 / 渲染工具），实施与验证见仓库 docs/validation/2026-09-29-T032-deep-research/IMPL-A4-chain.md。

1. **新实例默认值一处真源**：全部参数的新实例默认写在 Source/ParameterDefaults.h（factoryChainParams() 给整份链级参数）。参数表（Parameters.cpp）、
   离线对照工具（tools/render）、性能基准的链级参数（tools/perf/bench.mm）都从这里取；处理器 Debug 版构造时逐项核对参数表与真源一致。改默认值只改这一处；
   旧工程缺参的回退值仍在 PluginProcessor::addMissingParameterNodes（与新实例默认有意不同，ADR-020）。
2. **存档格式号**：工程存档的根元素带 celadonStateFormat（现为 1）与 celadonVersion（写它的插件版本）；没有这个属性的存档（≤ 0.1.8.5）按 0 读。读进来就摘掉，
   不进参数状态树。现在 0 与 1 的读法相同（缺参特判照旧）。以后改参数语义、改缺参回退规则、要按存档区分 DSP 兼容时，必须升号并在 addMissingParameterNodes 按号分支。
3. **实际参数的契约**：CeladonChain::getEffectiveParams() 的 mastering 也是交给模块的实际值——推响不刺 / 目标频谱的强度写在 harshResolved / targetResolved
   （模式改成手动 / 解析出的显式形状），编码余量为 0（已扣进 limit.ceilingDb）。整份原样当 GLAZE 0 的输入喂回链，逐位复现（离线工具的同决策重放，ADR-016）。插件从不设这两项。
4. **临时监听与表**（补 2026-09-28 修订「表与 GLAZE 照常按母带算」）：听原声期间 OUT 表的整体响度暂停积分，只积分母带；短期响度、真峰值照常（显示此刻听到的）。
   恢复后要攒够 4 个完全在暂停之外的 100 ms 子块才再成门块。宿主旁通期间是否同样冻结**待 Max 确认**（现为照旧积分）。预听归一化读的就是这个整体响度。
5. **退出归一化预听不阶跃**：在线、Listen 关时预听自己 50 ms 斜坡回 0 dB；宿主旁通 / 听原声 / 试听差值 / Listen 开时把此刻的衰减交给 Listen 的 20 ms 平滑；离线立即归一
   （导出不带尾巴）。同规格 prepareToPlay 与播放中途重启不改预听衰减，只把抖动序列回到固定种子。
6. **非有限值**：输入的 NaN / Inf 仍由处理器整块清零（频谱仪 IN 取样口在它之后），链自己不管输入；两块表对非有限值自愈（只清坏了的滤波器状态、丢掉那个 100 ms 子块，
   直方图与短期窗口永不吃非有限值）。参数的非有限原始值一律按新实例默认（处理器 readParams 第二道；链里 GLAZE、高级母带百分比 / 预听目标、Limit Ceiling 在扣编码余量之前
   按默认算；Lookahead 由 LookaheadHold 当作「没变」；各模块守卫自己的字段）。工程文件里能带进来的非有限值只有 NaN（±Inf 会被 APVTS 钳到范围两端）。
7. **数值环境**：插件音频线程在 processBuses 设一次 FTZ / DAZ；离线对照工具整个进程设一次（main 开头），两边同一数值环境（只影响 < 1.2e-38 的非规格化数）。
8. **表的计算**：整体响度两遍直方图扫描只扫到有数的最高一格，读数逐位不变。
```

### 2. `01-框架/设计规格-高级母带控制.md` §6（A1 的文件，主进程加）

- 预听交接：「退出预听时监听增益不阶跃：在线、Listen 关时预听自己 50 ms 斜坡回 0 dB；宿主旁通 / 听原声 / 试听差值 / Listen 开时把此刻的衰减交给 Listen 的 20 ms 平滑；离线立即归一（导出不带尾巴）。」
- 整体响度冻结：「听原声期间 OUT 表整体响度暂停积分，预听归一化读的是母带的整体响度（宿主旁通是否同样冻结待 Max 确认）。」
- 读数保持与 Reset：「读数暂时没有（≤ −90 LUFS，如界面 Reset 后约 400 ms）而此前有过有效读数时，预听保持上一次衰减；从未有读数、读数非有限、关预听、离线、prepare 时回到 1（待 Max 确认）。同规格 prepareToPlay 与播放中途重启不改预听衰减，只把抖动序列回到固定种子。」

### 3. `04-协调/TASKS.md`、当日日志（主进程）

- A4 行移已完成：R8-01 / R10-03 / R8-03（听原声）/ R8-04 / 核验漏项 / R7-04 / R10-08 / R10-01 / R10-02 前半 / R8-02（处理器与链级）/ R10-06；验证在云端容器（GCC 13.3），没在 107 / 109 / Mac。
- 待认领：① 宿主旁通冻结（待 Max，跨文件需求 1）；② 合并 A2 / A3 后打开 `colorLimitGuardsMerged`；③ bench modules 模式补高级母带解析（Mac）；④ 门槛 / 试听包按新工具重跑（待 Max 定 ADR-020 待复核第 1 条）。
