# T-032 实现报告 · A4-chain · 链路 / 处理器 / 表 / 发行安全 / 参数真源 / 渲染工具

> 2026-09-30，Opus 5.5 · max 第二次接续（前任依次是 Fable 5.1——额度中断，主进程保全为 `c258e3f`；Opus 5.5——断网中断，主进程保全为 `83b8ebe`）。
> 工作树 `.claude/worktrees/wf_abb55518-7ae-4`，分支 `worktree-wf_abb55518-7ae-4`，改前的底 = `e6b445b`（main，0.1.8.5 源码）。
> 任务书：`BRIEF-A4-chain.md`（研究 `R7-analysis.md` / `R8-chain-juce.md` / `R10-design.md`，核验 `V7` / `V8` / `V10`，都在 `fable-deep-research-634a5e` 工作树同名目录）。
> 验证全部在 107（GCC，Release，`-ffp-contract=off`，`-j 4`）上用本工作树的 `scripts/test-linux.sh` 跑；本机 Mac 未编译、未测试；109 连不上，未验证 MSVC / Mac。

【验证状态：见文末「本轮验证记录」】

## 结论一览

| 条目 | 做了什么 | 改输出 | 新测试 | 状态 |
|---|---|---|---|---|
| R8-01 + R10-03 退出预听不阶跃 | 按主进程合并修法：在线、链照常出母带、Listen 关 → 发行端自己 50 ms 斜坡回 1；宿主旁通 / 听原声 / 试听差值 / Listen 开 → 先把此刻衰减交给 Listen 的 20 ms 平滑器（下限 1e-6）再归一；离线立即归一。另补：退出斜坡还没走完时再切干路 / 开 Listen / 重开预听，同样交接不阶跃 | 只改退出预听后 ≤ 50 ms 的监听信号 | 5 种退出 × 参照链同步切换，相邻增益比跳变 < 0.01；Listen 关时步长 ≤ 50 ms 线性斜坡理论值；宿主旁通第一块预听读数 0 dB | 完成 |
| R8-03 听原声冻结 OUT 整体响度 | `LoudnessMeter::setIntegrationPaused`：暂停时 K 加权、子块环、样本计数、短期、真峰值照常，门块不进直方图；被暂停碰过的子块不进任何门块，恢复后要 4 个干净子块。链：`outputMeter.setIntegrationPaused (compareOriginal \|\| (hostBypassed && pauseIntegrationOnHostBypass))` | 否（音频逐位不变，只改读数与预听归一化的输入） | 母带 6 s → 原声 6 s → 母带 6 s；三种暂停对齐方式 + 突发；宿主旁通口径用例 | 完成；**宿主旁通一半待 Max**（开关已接，现为 false） |
| R8-04 预听中点表 Reset 保持衰减 | `ReleaseSafety` 记住最近一次有效读数算出的目标；读数 ≤ −90 LUFS 且此前有过有效读数 → 保持；从未有读数 / `reset()` / 关预听 / 离线 → 归一；非有限读数 → 回 1 并忘掉记忆。不设超时 | 只改 Reset 后 ≤ 400 ms 的监听信号 | 链级：Reset 后 2 s 内预听最高 ≤ 稳态 + 0.5 dB，从未有读数仍 0 dB；单元：七种情形 | 完成；**规格 §6 口径待 Max** |
| 核验漏项：整链 reset 归一预听 | `ReleaseSafety::reset` 拆出 `resetDitherSequence()`；`CeladonChain::reset()` / `restartWithFadeIn()` 只回种子，预听增益保留 | 只改同规格 prepareToPlay / 中途重启后的监听 | 两种重启后 0.5 s 内预听不回 0 dB；抖动开着时 reset == 新 prepare 逐位 | 完成 |
| R7-04 零风险版 | `updateIntegrated` 两遍扫描只到 `histogramTopBin`，第二遍从相对门所在格起（与原判断同一表达式校正下界） | 否（逐位不变） | `ChainDigest` 的 meter-sweep 摘要（60 s、相对门来回移动、中途 Reset）与改前逐位相同 | 完成 |
| R10-08 表对 NaN 自愈 | `finishSubBlock` 开头子块能量非有限 → 只清坏了的声道滤波器状态、丢本子块、断开门块连续性；`publishSubBlock` 前同判 `ringEnergy`；直方图永不吃非有限值（顺带消掉 floor(±Inf) 转 int 的 UB）。处理器守卫保留；渲染工具 stderr 报一次 | 否（有限输入逐位不变） | 单表：NaN 后短期 / 整体恢复；整链：NaN 块后 IN 表 1 s 内有读数、GLAZE 聆听继续 | 完成 |
| R10-01 默认值真源 + 实际参数无损回喂 | 新 `Source/ParameterDefaults.h`（`factoryChainParams()`）；参数表、渲染工具、bench `chainParamsFor` 都取它；处理器 Debug 构造逐项核对；`MasteringSettings` 加 `harshResolved / targetResolved`；链回写 `effective.mastering`（模式改手动 / 解析出的显式形状，`codecMarginDb` 置 0）；渲染工具 expressible 判定、`--selftest` 阳性对照与「有牙」对照、metrics.md 头部写高级母带设置、README 改指真源 | 插件否；渲染工具成品会变（目的） | 链级：GLAZE 35 + 两项自动 + 余量 0.5 dB 的实际参数喂回 GLAZE 0 链逐位相同，回喂原始设置明显不同；真源默认值逐项核对；`--selftest` 两条新对照 | 完成 |
| R10-02 存档格式号（前半） | 根元素写 `celadonStateFormat=1` 与 `celadonVersion`；读时取出（缺 = 0）传给 `addMissingParameterNodes` 作锚点，两个属性不进参数状态树；format 0 特判照旧 | 否 | 107 处理器级 harness（Debug 构建真处理器）；bench `statecheck` 加两条（Mac 专用，未跑） | 完成 |
| R8-02 处理器第二道 + 链级 | `readParams` 每个原始值套 `finiteOr (值, 新实例默认)`（Lookahead 交 `LookaheadHold`）；链里 GLAZE、高级母带百分比 / 预听目标、**Limit Ceiling（扣编码余量之前）** 按默认值算 | 有限值逐位不变 | `ChainTests` 34 个浮点字段 × NaN / ±Inf：有限、不静音、真峰值 ≤ Ceiling + 0.01 dB、与回退值逐位相同（Color 5 个、Limit 4 个字段等 A2 / A3 合并后跑） | 完成（9 个字段待合并后由主进程跑） |
| R10-06 数值环境 | 删 `processChain` 里重复的 `ScopedNoDenormals`；渲染工具 `main` 开头一次（单线程，覆盖聆听 / 成品 / 重放三遍与自检） | 插件否；渲染工具只在 < 1.2e-38 量级 | 渲染前后对照（GLAZE 0 成品逐位相同） | 完成 |

## 验证

### 命令

```bash
# 定向
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 4 && "$BUILD/celadon_tests" --only CeladonChain,CeladonChainClay,CeladonChainMonitoring,CeladonChainGuards,ChainDigest,LegacyFixture,AdvancedMastering,AdvancedMasteringChain,LoudnessMeter,GlazeCalibration,GlazeChain'
# 全套（先 clean 再全量编译，数警告）
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target clean && cmake --build "$BUILD" --target celadon_tests -j 4 2>&1 | tee "$BUILD.full-build.log" && "$BUILD/celadon_tests"'
# 渲染工具自检
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_render -j 4 && "$BUILD/celadon_render" --selftest'
```

【数字待填】

### 用例抓得住缺陷：同一份用例放回改前源码

前任（Opus）在 107 上把 `CeladonChainMonitoring` 原样放回 `e6b445b` 源码（这一类只用改前就有的公开接口）：**29 过、14 失败**，每个缺陷都被抓住，数字如下（改后同一用例全部通过，改后数字见上）。

| 用例 | 改前（e6b445b） |
|---|---|
| Listen 关着关预听 | 增益比单样本跳 0.695（稳态预听 −10.32 dB）；另一条「不快于 50 ms 斜坡」同样失败 |
| 预听中切原声 / 宿主旁通 / 试听差值 | 三种都单样本跳 0.695 |
| 听原声 6 s | 整体响度被拉低 2.03 LU；回到母带 6 s 后仍低 1.27 LU |
| 预听中点表 Reset | Reset 后预听增益回到 0.000 dB（稳态 −10.32 dB） |
| 同规格 prepareToPlay / restartWithFadeIn | 之后 0.5 s 预听读数最高 −0.33 dB（先回 0 dB 再 50 ms 滑回） |
| 单表喂一个 NaN | 短期响度此后一直 −100（无读数），整体响度冻结 |
| 整链一块 NaN 输入 | IN 表短期一直 −100；GLAZE 聆听时长停在 2.90 s |
| 实际参数回喂 GLAZE 0 链 | 最大样本差 0.052（回喂原始设置同为 0.052：两项「自动」被算成 0、编码余量再扣一次） |

「Listen 开时关预听」与「reset 仍把抖动序列回到种子」是对照 / 回归项，改前改后都过。改前 `color.heat = NaN` 让测试进程段错误（`ColorModule::heatCompensationFor` 里 `jlimit` 放过 NaN、`(int) NaN` 越界查表）——这是 A2 线的缺陷，所以 `CeladonChainGuards` 里 Color / Limit 的 9 个字段（Color 5 个；Limit 的 gain / release / lookahead / clip）在合并前整段跳过（连跑都不跑），不是只记日志。

### 逐位证据

【待填：ChainDigest 七个场景改前 / 改后对照、渲染工具 GLAZE 0 对照、处理器 harness】

## 逐条说明

### R8-01 + R10-03 · 退出归一化预听不再单样本阶跃

- 位置：`Source/dsp/CeladonChain.cpp:180-212`（`leaving` / `previewGainPending` / `rampsItself`），头文件说明 `CeladonChain.h:38-49`。
- 逐情形：在线 + 链照常出母带（`releaseAllowed`）+ Listen 关 → 不 reset，`ReleaseSafety::process` 在 `previewEnabled = false` 下按 50 ms 线性斜坡回 1，走完精确落 1、转回逐位旁通；离线 → 立即 `reset()`（导出第一个样本起不带预听、不带尾巴，既有用例要求）；其余在线情形（宿主旁通 / 听原声 / 试听差值时 `releaseSafety.process` 不再被调，斜坡走不动；Listen 开时 Listen 要接手）→ `listenGain.setCurrentAndTargetValue (jmax (1e-6f, listenGain × previewGain))` 再 `reset()`，随后 `updateListenTarget()` 让 Listen 的 20 ms 乘法平滑器走到目标（Listen 关或干路 = 1）。预听期间 Listen 增益恒为 1（进入预听时交接成 1、`listenRequested` 为假），所以「× listenGain」在刚退出那一块恰好等于任务书的「交接 previewGain」，逐位同值。
- 比任务书多做的一处：`previewGainPending`（预听增益还没回到 1）——Listen 关着退出后 50 ms 斜坡还在走时，用户又切原声 / 宿主旁通 / 试听差值 / 开 Listen，同样把「此刻的剩余衰减」交给 Listen 平滑，不在块边界跳；重新打开预听时把 `Listen × 剩余衰减` 交给预听（预听增益为 1 时与改前逐位相同）。
- 宿主旁通：监听增益 20 ms 交接（与模块自身旁通的 20 ms 交叉淡化同量级），预听读数从第一个旁通块起是 0 dB（保住 `:154` 注释的原意，用例断言）。
- 预听从未打开时每个分支都不执行：`previewGainNow == 1.0` 精确成立（`reset` / 斜坡末尾都精确落 1），逐位不变；`ChainDigest` 的 monitor-toggles（没开过预听、三种监听来回切、24 bit 抖动）音频与读数摘要与改前相同。
- 用例：`ChainTests.cpp` `CeladonChainMonitoring::testPreviewExitNeverSteps`（任务书指定写在 ChainTests，AdvancedMasteringTests 归 A1）。

### R8-03 · 听原声期间 OUT 表整体响度暂停积分

- 位置：`LoudnessMeter.h` `setIntegrationPaused` 与头注；`LoudnessMeter.cpp:226-233`（整块在暂停中就给当前子块打「碰过」标记）、`:326-341`（碰过的子块不进门块、连续计数清零，恢复后 `cleanSubBlocksInRow` 攒够 4 个才成门块）；链 `CeladonChain.cpp:244-247`。
- 口径拆开：听原声部分（规格已定）直接做；宿主旁通部分接在同一个开关上，但 `CeladonChain::pauseIntegrationOnHostBypass` 现为 `false`（宿主旁通照旧积分），**待 Max 确认**。没按任务书「dryPath」直接接上的原因：`GlazeTests::testGlazeZeroEqualsReferenceChain`（不归本线）逐位比「链的 OUT 表 == 一块普通的表」，mode 2 里宿主旁通每 90 块来回切——接上后该用例必然失败，而改它不在本线文件范围。Max 确认冻结后：把常量改成 `true`，并把该用例参照 OUT 表在宿主旁通的块同样 `setIntegrationPaused (true)`（见跨文件需求 1）。`CeladonChainGuards::testHostBypassIntegrationPolicy` 按常量两种取值各有断言，翻转时自动换判据。
- 偏保守的一处（已写进头注）：暂停中处理的那一块恰好在读数子块边界上结束时，紧接着的子块（一个暂停中的样本也没有）也按碰过算，恢复后多等 100 ms；暂停中的样本一个都漏不进。要做到精确得在逐样本循环里判断，不值得。
- 试听差值不暂停：表在差值换上输出之前读的是母带（`processSlice` 顺序），本来就对。
- 短期响度照常（显示此刻听到的原声，与界面「OUT 是原声」口径一致）；子块环、样本计数照常，校准器 / Listen 配对不经直方图。

### R8-04 · 预听中点表 Reset 保持衰减

- 位置：`ReleaseSafety.cpp:68-100`（`process` 的三分支）、`:38-50`（关预听忘记记忆）、`:24-36`（`reset` / `resetDitherSequence`）；头注 `ReleaseSafety.h`。
- 按核验去掉 3 s 超时：读数只经清表变成「无读数」；prepare 与离线都走 `reset()` = 从未有读数 → 1。
- 与任务书字面的一处区分：规格 §6 的「读数未知」包括「≤ −90 LUFS」与「非有限」两种。实现只对前者（表的「无读数」）保持；非有限读数当坏读数——回 1 并忘掉记忆（之后再来「无读数」也不跳回旧衰减）。R10-08 之后链里的表不会再给出非有限读数，这一支只是防御；两种取舍都不比现状危险（预听只衰减），写进「待 Max」第 2 条一并定。

### 核验漏项 · 整链 reset 不再归一预听

- 位置：`ReleaseSafety::resetDitherSequence()`；`CeladonChain.cpp:59-73`、`:75-99`。
- 任务书写「chain.reset() 只清抖动序列并把 previewActive 置 false 让下一块重新交接」。实现保留增益、也保留 `previewActive`：增益保留时「重新交接」是恒等操作——预听期间 Listen 增益恒为 1，下一块进入分支会把 `1 × 当前预听增益` 交回预听、Listen 仍为 1；若下一块预听已关，`leaving` 与 `previewGainPending` 走的是同一组分支。两种写法对所有后续块逐位等价，少一个状态改动。
- 抖动：`reset()` 仍把 TPDF 序列回到固定种子（导出可复现），用例 `testDitherRestartsOnReset` 断言「跑过再 reset == 新 prepare」逐位相同。
- 试听差值 20 ms 淡出期间不跑 `releaseSafety.process`（cosmetic，任务书要求只记一句）：淡出的这 20 ms 没有抖动，差值试听本身就是临时监听，不进导出。

### R7-04 · 两遍扫描只扫有数的格

- 位置：`LoudnessMeter.cpp:361`（`histogramTopBin`）、`:366-415`（`updateIntegrated`）。
- 第一遍 `[0, topBin]`：更高的格子能量 0.0、计数 0，`x + 0.0 == x`（能量非负、没有 −0），求和顺序与每步舍入不变。第二遍从算式估的起点出发，用与原判断同一个表达式 `binCentre >= relativeGate` 向两边校正到真正下界（格中心随格号单调不减：`(float) bin + 0.5f` 精确，除以 10 与加常数都保序），纳入的格子与扫满 800 格一模一样。
- 不做 `setReadoutEnabled`（核验建议不做）。

### R10-08 · 表对 NaN 自愈

- 位置：`LoudnessMeter.cpp:95-102`（`healFilterStates`：只清状态已坏的声道）、`:182-196`（`publishSubBlock` 前判 `ringEnergy`）、`:296-310`（`finishSubBlock` 开头判 `subBlockEnergy`）。
- 两套子块网格（读数网格可被界面 Reset 重起、子块环不断）先后发现同一个 NaN 时，后一次不会把已经恢复的状态再清一遍。子块环里那个坏子块按既有约定仍以非有限 `meanSquare` 发布（分析器、Listen、校准器、渲染工具本来就跳过它）。
- 处理器守卫保留（频谱仪取样口在链之前）；链照旧不管输入（`GlazeTests` 的 v0.5 参照链对照固化了这一点）。
- 渲染工具：读 wav 时已逐样本把 NaN / Inf 清成 0（原有），现另在 stderr 报一次（`RenderMain.cpp:2470-2476`）。

### R10-01 · 默认值一处真源 + 实际参数无损回喂

- 新文件 `Source/ParameterDefaults.h`：标量默认值 + `factoryChainParams()`；`Celadon.jucer` 照 `ParameterRanges.h` 加一条 file 条目（compile="0"）。
- `Parameters.cpp` 全部默认值改读 `defaults::`；处理器 Debug 构造 `PluginProcessor.cpp:259-308` 用 `readParams()` 读参数表默认，与 `factoryChainParams()` 逐项核对（浮点 `approximatelyEqual`，对数刻度往返会漂几个 ulp；选项与开关按值），`static_assert` 项数 == `allParameterIds`（36）。
- `MasteringSettings::harshResolved / targetResolved`（−1 = 未设）；链 `CeladonChain.cpp:127-144`：模式不是「关」且已设 → 直接用、不经乘法；未设分支与改前同一个表达式。回写 `:163-172`：`harshMode` → 手动（关仍是关）、`targetMode` → 解析出的显式形状（「参考」没学好 → 关）、`codecMarginDb` → 0（余量已扣进 `limit.ceilingDb`）。插件不设 resolved、也只从实际参数里读 Balance / Color / Limit 七项（`publishEffectiveForUi`），输出与界面逐位不变。
- 任务书研究稿的 `targetResolvedMode` 没另加字段：直接把 `targetMode` 改写成解析出的形状，效果相同、少一个字段。
- 渲染工具：`makeFactoryParams()` 返回真源；`masteringReplayable`（「参考」形状 → n/a）并进两处 expressible 判定；`--selftest` 加「重放对照覆盖高级母带」（有块两项强度都 > 0）与「重放对照抓得住未解析的高级母带」（把 mastering 换回原始设置重放，差 > −120 dBFS）；metrics.md 头部写本次高级母带设置；README 三处改指真源。
- bench：`chainParamsFor` 起点改 `factoryChainParams()`（modules 模式与整插件模式同一场景是同一条链）。**性能基准 modules 模式的数字会变**（场景没写 GLAZE 的现在是 35、高级母带两项自动），以后对比旧数要注意。

### R10-02 · 存档格式号（只做前半）

- `PluginProcessor.cpp:60-64`（常量）、`:851-852`（写）、`:896-902`（读、摘掉、传入）、`:911-916`（`addMissingParameterNodes (newState, stateFormat)`，现为锚点、`ignoreUnused`）。
- 两个属性读完就从 XML 上摘掉，不进参数状态树（存档时再写当前值）。
- bench `statecheck` 加两条（`bench.mm:1160-1188`，Mac 专用，本线未跑）；同样的检查在 107 上用临时 harness 跑了（见逐位证据）。

### R8-02 · 非有限参数：处理器第二道 + 链级

- 处理器 `readParams`（`PluginProcessor.cpp:391-461`）：每个原始值套 `finiteOr (值, 新实例默认)`；Lookahead 例外（`LookaheadHold` 把非有限当「没变」，不因坏值改延迟）。
- 链 `CeladonChain.cpp:110-115`（GLAZE → 0）、`:130-144`（高级母带百分比）、`:154-158`（**Limit Ceiling 在扣编码余量之前换默认**）、`:215`（预听目标）。Ceiling 这一处是前任最后一轮（09:32）加的，任务书没点名：链先用 Ceiling 做减法再交给编排器与限幅器，非有限值扣完余量还是非有限，A3 的模块守卫只能换成不含余量的默认值，与「该字段取默认值」的链差一个余量——根因在链，守卫就得在链。
- 链级用例 `CeladonChainGuards::testNonFiniteParams`：34 个浮点字段（含 `balance.*` 自动项、`clay.*` 输入、`mastering.*Resolved`）× NaN / +Inf / −Inf；Color 5 个与 Limit 4 个（gain / release / lookahead / clip）等 A2 / A3 合并后把 `colorLimitGuardsMerged` 改 `true` 再跑（跨文件需求 2）。

### R10-06 · 数值环境

- 删 `PluginProcessor.cpp:648` 的重复声明（`processChain` 只被 `processBuses` 调，后者已设）。
- 渲染工具 `main` 开头一次 `juce::ScopedNoDenormals`（`RenderMain.cpp:2389`）：工具单线程，一次覆盖聆听 / 成品 / 重放三遍与自检；再在 `renderPass` / `replayPass` / 自检里各套一层是嵌套重复——正是本条要删的那种。
- 按核验，不在 `ChainTests` 单独加；ADR-016 的切块差异不作为验收条件。

## 前任改动里本轮改正 / 补上的

【待填】

## 没做的与原因

- R7-04 ② `setReadoutEnabled`、R10-02 后半「format ≥ 1 查表」：任务书明确不做。
- R10-06 在 `ChainTests` / `TestMain` 加 FTZ：任务书明确不改。
- R8-02 模块内守卫：A2（Color）/ A3（Limit）线。
- 宿主旁通冻结 OUT 整体响度：待 Max（见上）。
- bench `statecheck`：Mac 专用，本线不在本机跑；同样的检查已在 107 的临时 harness 上跑过。
- 22 首门槛 / 试听包重跑：属主进程与 Max（R10-01 之后工具成品会变；核验建议先定 ADR-020 待复核第 1 条）。

## 跨文件需求

1. `tests/GlazeTests.cpp`（`testGlazeZeroEqualsReferenceChain`）：Max 确认「宿主旁通也冻结 OUT 整体响度」后，与 `CeladonChain::pauseIntegrationOnHostBypass = true` 同一提交：参照 OUT 表在 `hostBypassed` 的块前 `referenceOut.setIntegrationPaused (true)`、其余块 `false`。
2. 主进程合并 A2 / A3 之后：`tests/ChainTests.cpp:2665` `colorLimitGuardsMerged` 改 `true`，跑 `--only CeladonChainGuards`（Color 5 字段、Limit 4 字段 × 3 个坏值才判定）。
3. `设计规格-高级母带控制.md` §6（A1 在改）：下面「档案待补」三句。

## 档案待补（给主进程加进 设计规格-高级母带控制 §6）

- 预听交接：「退出预听时监听增益不阶跃：在线、Listen 关时预听自己 50 ms 斜坡回 0 dB；宿主旁通 / 听原声 / 试听差值 / Listen 开时把此刻的衰减交给 Listen 的 20 ms 平滑；离线立即归一（导出不带尾巴）。」
- 整体响度冻结：「听原声期间 OUT 表整体响度暂停积分，预听归一化读的是母带的整体响度（宿主旁通是否同样冻结待 Max 确认）。」
- 读数保持与 Reset：「读数暂时没有（≤ −90 LUFS，如界面 Reset 后约 400 ms）而此前有过有效读数时，预听保持上一次衰减；从未有读数、读数非有限、关预听、离线、prepare 时回到 1。同规格 prepareToPlay 与播放中途重启不改预听衰减，只把抖动序列回到固定种子。」

`设计规格-总体.md` 末尾「2026-09-29 修订（T-032）」小节本线已写（136 行）。

## 待 Max

1. **宿主旁通期间 OUT 整体响度是否冻结**（R8-03 另一半；建议冻结）。开关、用例、GlazeTests 要改的一行都已备好（跨文件需求 1）。
2. **规格 §6「读数未知时回到 1」改口径**（R8-04）：现实现 = 暂时没有读数且此前有过有效读数 → 保持上次衰减；非有限读数 → 回 1。不设超时。
3. **门槛与试听包要按新工具重跑**（R10-01）：T-032 之前的成品是高级母带两项关着的链；GLAZE 0 成品不变。
4. **性能基准 modules 模式口径变了**（R10-01 b）：场景没写 GLAZE 的现在跑新实例默认。

## 本轮验证记录

【待填】
