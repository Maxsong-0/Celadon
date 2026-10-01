# V8 · 链路与 JUCE 集成 反驳核验（T-032，2026-09-29）

> 核验代理 V8，对象：`R8-chain-juce.md` 的 4 条结论。子系统：`CeladonChain` / `PluginProcessor` / `Parameters` / `ParameterRanges` / `AtomicHelpers` / `.jucer`。
> 只读；本机没编译、没跑测试；**没有在 107 复跑数值实验**——四条结论的每个实测数字都能从代码逐行推出（见各条「我自己找到的证据」），复跑不会增加信息，107 又正被全套与其他代理占用。
> 写于 2026-09-29 22:4x PDT。

## 0. 一句话

四条**全部成立**（confirmed），没有一条是误读代码、已修或规格里已定的取舍；R8-02 与 R8-03 的优先级 / 口径要调：R8-02 需要外部先写坏参数才触发，降到 **P2**；R8-03 的「听原声」部分有规格与界面代码两重依据可直接做（**P1**），「宿主旁通」部分仍待 Max。修法四条都可用，各有一处要补的细节（第 2 节）。漏掉的同类问题只有两条小的（第 3 节）。

| 编号 | 判决 | 严重度 | 优先级 | 修法 | 备注 |
|---|---|---|---|---|---|
| R8-01 | confirmed | medium | P1 | 可用，补两处细节 | 四种退出方式都是块边界上的 6 dB 阶跃；只有 Listen 开着的一种有交接 |
| R8-02 | confirmed | medium | **P2**（研究写 P1） | 可用，钳范围要按 −4.5 下限 | 机理比研究写的更精确：`MovingAverage::push` 的 `jmin (1.0, NaN)` 返回 1.0，限幅器「静默失效」而不是输出 NaN |
| R8-03 | confirmed | medium | P1（听原声）/ 待 Max（宿主旁通） | 可用，恢复时要重攒门块 | 规格 118 行 + 界面代码 1035-1041 行两重依据 |
| R8-04 | confirmed | low | P2 | 可用，但建议去掉 3 s 超时 | 行为符合规格字面，属规格层取舍，改要 Max 点头 |

## 1. 逐条判决

### R8-01 预听退出时监听增益单样本阶跃 —— confirmed

**我自己找到的证据**（`Source/dsp/CeladonChain.cpp`）：

- `:143-145`：`releaseAllowed = ! dryPath && ! auditionDifference;`、`nextPreview = releaseAllowed && ! offline && previewRequested`。
- `:147-153`：进预听时交接（`releaseSafety.setCurrentPreviewGain (listenGain.getCurrentValue())`）；退出预听只有 `previewActive && ! nextPreview && releaseAllowed && ! offline && newParams.limit.listen` 这一种情形才 `listenGain.setCurrentAndTargetValue (releaseSafety.getCurrentPreviewGain())`。
- `:155`：`if (! releaseAllowed || (previewActive && ! nextPreview)) releaseSafety.reset();` → `ReleaseSafety.cpp:24-29` 把 `previewGain` 直接置 1.0。
- `:295`：`releaseSafety.process` 只在 `releaseAllowed && ! differenceInvolved() && ! nonFinite` 时调；`ReleaseSafety.cpp:61-69` 预听关掉后目标 1.0、且已经 `reset` 到 1.0，`:76-81` 走「精确旁通」提前返回——前一块乘的是 0.501，这一块什么都不乘，阶跃就落在块边界。
- `:383-392` `applyListenGain` 在 `processListen` 里对所有路径无条件执行（`:395-479`，含 `hostIsBypassed`），所以用 `listenGain` 承接斜坡在干路 / 差值路径上都能落地——研究的修法前提成立。

**不是已定取舍**：规格 [[设计规格-高级母带控制]] §6（`:76-79`）写「增益 50 ms 平滑」「宿主旁通、听原声、试听差值时也关」「与 Listen 交接当前增益，不叠加」，没写「关闭立即归一」；[[设计规格-Limit]] `:116` 只写「预听打开时 Listen 让位，交接时把当前监听增益传过去」。`docs/design/0.1.9-mastering-controls.md` 对退出方式一字未提。代码里唯一的意图注释是 `:154` 「Monitoring must be neutral from the first bypass/offline sample (no release tail into exports)」——只覆盖旁通 / 离线，对「Listen 关着关预听」没有任何依据，属实现遗漏。TASKS 待认领区明确写着「0.1.9.0 在真实 DAW 里走一遍……三种临时监听」还没做，Max 没有拍板过这里的行为。

**现有测试只覆盖 Listen 开着**：`tests/AdvancedMasteringTests.cpp:782-804` `testMonitoringHandover` 用 `params.limit.listen = true`。`:768-781`「offline transition removes preview from its first sample」证明离线那一支的立即归一是有意的，研究把它排除是对的。

**界面路径的补充**（不影响结论）：`Source/PluginEditor.cpp:724-731 / 743-750` 切原声、试听差值时编辑器先 `setReleasePreviewEnabled (false)` 再写 `compareOriginal` / `auditionDifference`，两个原子量分开写；音频线程可能在两块里分别看到，于是界面上的「预听中切原声」多半退化成「Listen 关着关预听」+ 下一块进干路——阶跃照样发生，只是走的分支不同。宿主旁通则不经过界面（`PluginProcessor.cpp:601-603` 直接传 `hostBypassed`），研究测的「预听中宿主旁通」分支是真实可达的。链级修法两条路都盖住。

**严重度 / 优先级**：medium / P1 维持。阶跃幅度 = 目标 − 整体响度，默认 −14 对一首 −8 LUFS 的母带就是 6 dB，目标调到 −24 时可到 16 dB；每次退出预听必现，正是工单点名的「旁通与 A/B 的爆音」。只改监听信号，母带、表、GLAZE 不动。

**修法**：可用（fix_ok = true），补两处：
1. `listenGain` 是 `Multiplicative` 平滑器，`setCurrentAndTargetValue` 传 0 会坏。现状 `previewGain` ≥ 10^(−144/20) > 0、`setCurrentPreviewGain` 钳到 [0, 1]（`ReleaseSafety.cpp:38-43`）所以实际不会是 0，但交接那一行应写成 `jmax (1.0e-6f, (float) gain)` 一类的守卫，别依赖上游巧合。
2. 宿主旁通那一支与 `:154` 注释的「第一个旁通样本就中性」相抵触。我判断可以接受：模块自己的旁通本来就是 20 ms 交叉淡化（`:168-176`，ChainTests「host bypass must return the unattenuated delayed input」只从 250 ms 之后比），干路上再多一条 20 ms 的监听增益斜坡与之同性质；「实时导出中途旁通、且恰好在预听」这种组合不值得保留一个爆音。但综合阶段要把这条注释一起改掉，并在 [[设计规格-高级母带控制]] §6 补一句「退出预听（含旁通 / 原声 / 差值）时监听增益 20 ms 交接到 Listen，离线立即归一」。
3. 建议的验证用例判据（相邻增益比跳变 < 0.01，沿用 `ChainTests.cpp:621-646`）合适；四个用例都要用「参照链同步做同样切换」的写法，否则干路的 20 ms 交叉淡化会被误算成阶跃。

**风险**：低。只在 `previewActive && ! nextPreview && ! offline` 那一块之后 ≤ 20 ms 的监听信号上变；预听从未打开时每一行都不执行，逐位不变（ADR-013 不涉及）。

### R8-02 Limit / Color 参数不拦非有限值 —— confirmed，优先级降到 P2

**我自己找到的证据**：

- 入口不清洗：`Source/PluginProcessor.cpp:352-368` `readParams` 对 Color / Limit 各字段直接 `load()`；`:783-818` `setStateInformation` 只摘 GLAZE / 参考节点、`replaceState`，不看值是否有限；`CeladonChain.cpp:105 / 112` 原样拷贝。对照已守卫的：`BalanceModule.cpp:447-468`、`ClayModule.cpp:375-386`、`CeladonChain.cpp:114 / 131`（`unit` / 编码余量）、`PluginProcessor.cpp:441 / 458`（LookaheadHold）——守卫确实只做了一半。
- 模块不拦：`LimiterModule.cpp:338-370` 无 `isfinite`；`ColorModule.cpp:606-614` 五个 `jlimit`，而 `juce_MathsFunctions.h:551-560` 的 `jlimit` 对 NaN 两个比较都为假、原样返回；`ColorModule.cpp:607` 注释写着「不信任外部值」——意图与实现不符。
- JUCE 9.0.2 各层放行 NaN：`juce_core/text/juce_CharacterFunctions.h:254-260` 把 "nan" 读成 `quiet_NaN`；`juce_core/maths/juce_NormalisableRange.h:259-268` `clampTo0To1` = `jlimit`；`juce_audio_processors_headless/utilities/juce_RangedAudioParameter.cpp:48-58` `convertTo0to1 / convertFrom0to1` 只有 `snapToLegalValue` + `jlimit`；`juce_AudioProcessorValueTreeState.cpp:101-105` `setDenormalisedValue` 只 `approximatelyEqual` 判等后 `setNormalisedValue`→`:171-177` `setValueNotifyingHost`→`juce_AudioProcessorParameter.cpp:59-63` 不钳→`juce_AudioParameterFloat.cpp:98` `value = convertFrom0to1 (NaN)` = NaN。研究引的行号与我看到的一致。
- **机理比研究写的更精确**（研究说「包络比较全假→前瞻增益级失效」，方向对、细节不全）：`LimiterModule.cpp:72-75` `timeConstantCoeff (fs, NaN)` = NaN；`:85-91` `followWithRelease` 第一次「释放」时 `settle (target + (current − target) · NaN)` → `:79-82` `settle` 对 NaN 两个比较都假、返回 NaN，`:993` `envelopeManual` 从此永远是 NaN；`:154-170` `MovingAverage::push` 的 `sum` 变 NaN 后 **`:169` `juce::jmin (1.0, NaN)`**（`b < a ? b : a`，`NaN < 1.0` 为假）**返回 1.0**——增益恒为 1、限幅器静默失效，只剩 `:872` `jlimit (−ceiling, ceiling, ·)` 硬钳位在削。这正是研究实测「0 个非有限块、峰值恰为 Ceiling、RMS 0.7727」的来源：不是 NaN 输出被链拦下，而是 NaN 被 `jmin` 吃掉成了「不限幅」。`gainDb / ceilingDb / outputDb = NaN`：`Decibels::decibelsToGain` 是 `decibels > minusInfinityDb ? pow : 0`，NaN 比较为假 → 0 → 静音。`toneDb = NaN` → `updateToneCoefficients (NaN)` → 输出 NaN → `CeladonChain.cpp:255-261` 清零 + `restartWithFadeIn` → 每块重来。研究的三种表现都能从代码推出。
- 不改声音：`GlazeController.cpp:2182` 已把实际 `limit.gainDb` 钳到 0 ～ 24、`:2145` `color.toneDb` 钳到 ±3；链交给限幅器的 Ceiling 下限是 −4.5（`CeladonChain.cpp:131`，[[设计规格-Limit]] `:115`）；`lookaheadSamplesFor` (`:382-386`) 已钳 1 ～ 10。测试 / 工具里的 Limit / Color 取值我 grep 过全部在范围内（唯一越界的 `releaseMs = 1.0e7f` 在 `GlazeControllerTests.cpp:4116`，喂的是纯函数 `upgradeLegacyConditions`，不经过 `LimiterModule::setParams`）。校准参考条件读的是链的 `manual`（`GlazeController.cpp:3251`），模块内钳制不碰它。
- 现有测试没覆盖：`tests/` 里的 `quiet_NaN` 全是音频样本或 GLAZE / ReleaseSafety 读数，没有一个把 NaN 喂给 `LimiterModule::Params` / `ColorModule::Params`。

**严重度 / 优先级**：触发条件是上游先写坏——宿主自动化传的是 0 ～ 1 的归一值、界面滑块出不了 NaN，实际只剩「手改 / 损坏的工程文件、外部工具生成的预设」这一条路；一旦进来后果确实重（静音或硬削波、且随工程保存）、又不报警（释放时间 NaN 那种连非有限计数都不涨）。所以严重度 medium 可以保留，**优先级降为 P2**：这是把已经做了一半的「模块不信任外部值」补齐，S 工作量、对有限输入逐位恒等，综合阶段顺手做即可，但不该排在 R8-01 / R8-03 前面。

**修法**：可用（fix_ok = true），三点要注意：
1. `LimiterModule::setParams` 的 Ceiling 钳位下限必须是 **−4.5**（不是头文件注释的 −3），否则编码余量 1.5 dB 时链交下来的 −4.5 会被改掉、破坏 `AdvancedMasteringTests`「codec reserve actually lowers delivered peak level」——研究正文写对了，落地时别照头文件抄。
2. `lookaheadMs` 的钳制是冗余的（`lookaheadSamplesFor` 已钳），加 `isfinite` 回默认即可，别改它的取整。
3. `readParams` 那层 `finiteOr` 只当第二道；真正的守卫放模块（研究已说明，`PluginProcessor` 不在 `celadon_tests` 里）。

**风险**：极低。`std::isfinite ? jlimit : 默认` 对有限且在范围内的值是恒等，ADR-013 无关；GLAZE 校准哈希输入不变。

### R8-03 听原声 / 宿主旁通期间 OUT 表继续积分 —— confirmed（听原声 P1；宿主旁通待 Max）

**我自己找到的证据**：

- `CeladonChain.cpp:109` `dryPath = hostBypassed || compareOriginal`；`:173-176` 四个模块 `bypass || dryPath`——出口是延迟后的原声；`:263` `outputMeter.process (io)` 不看 `dryPath`；`LoudnessMeter.cpp:271-309` `finishSubBlock` 无条件把 400 ms 门块记进直方图（`:306-309`）；`:312-348` `updateIntegrated` 按 BS.1770 相对门（均值 − 10 LU）取块——原声比母带轻 ≤ 10 LU 时全部计入。研究的数字能直接算出来：母带 −14 与原声 −20 各 6 s，能量平均 = (10^−1.4 + 10^−2.0) / 2 → −16.03 LUFS，与实测逐位对得上；回到母带后直方图里的原声块永远留着，只能被稀释、不会消失（−15.27 → −14.71）。
- 预听归一化读的正是它：`:295` `releaseSafety.process (io, outputMeter.getIntegratedLufs())`；`ReleaseSafety.cpp:62-68` 衰减 = 目标 − 整体响度 → 偏差 1 ～ 2 dB 与研究一致。
- GLAZE 与 Listen 自己已经避开了：`GlazeController.cpp:2310` `paused = hostBypassed`（校准暂停）、`CeladonChain.cpp:432` `processListen` 在 `hostIsBypassed` 时 `continue`——**只有 OUT 表的整体积分没避**，与它们不一致。
- 规格与界面两重依据：[[设计规格-总体]] `:118`「原声、差值、预听三种临时监听互斥，只改送给宿主的监听信号，**表与 GLAZE 照常按母带算**」；`CeladonChain.h:121-123`「OUT 的短期 / 整体响度：Listen 监听补偿之前的母带输出」；`Source/PluginEditor.cpp:1033-1041` 听原声时把真峰值最大值排除在响度小窗之外（「听原声时 OUT 是原声：不进响度小窗的最大值」），`:1043` 整体响度却照读——界面自己已经认定小窗该只看母带，整体响度是漏网的一项。[[设计规格-界面-V4融合版]] `:37`「OUT 填充（听原声时就是原声）」说的是频谱仪，不与此冲突（频谱仪显示此刻听到的，小窗显示母带）。

**不是已定取舍**：翻了 [[ADR-020]]、`docs/design/0.1.9-mastering-controls.md`、09-28 / 09-29 日志，没有任何地方决定过「A/B 期间整体响度计原声」。Reset 是可用的兜底（[[设计规格-界面]] `:101`），但用户 A/B 一次就得记得点一次 Reset，不是设计。

**严重度 / 优先级**：medium 维持。**听原声部分 P1**——A/B 是母带流程里最常做的动作之一，40 px 的整体响度大数字与预听归一化一起被拉偏 1 ～ 2 LU、且不自愈；规格已经把口径定死，不需要新拍板。**宿主旁通部分**规格没写、我同意研究「建议冻结」（宿主旁通就是宿主那边的 A/B，机理一模一样），但要 Max 点头，实施时可先按同一开关做、在规格补一句时标「待 Max 确认」。

**修法**：可用（fix_ok = true），补一处：暂停只跳过直方图那一步、短期环照推的话，**恢复积分后的前 3 个门块**（`:288-303` 每个门块由最近 4 个子块拼成）会混进 1 ～ 3 个原声子块（≤ 300 ms 的混合）。建议恢复时把「门块攒够 4 个子块」的计数重新从 0 起（复用 `recentCount` 的逻辑或加一个 `subBlocksSinceResume`），代价只是恢复后 400 ms 不进直方图。其余（子块环、`processedSamples`、真峰值、短期、GLAZE 配对）研究的分析正确：`resetListenMeasurement` 与校准器读的是子块环，与直方图无关。文件归属：`LoudnessMeter.{h,cpp}` 是 R7 的文件，综合阶段要协调。

**风险**：零音频改动，逐位不变；只改读数。

### R8-04 预听中点表 Reset → 预听增益回 0 dB 保持 400 ms 再落回 —— confirmed（规格层取舍，低 / P2）

**我自己找到的证据**：

- `ReleaseSafety.cpp:61-69`：`integratedLufs > −90` 才算衰减，否则 `targetGain = 1.0`，50 ms 平滑。
- `LoudnessMeter.cpp:98-110` `clearMeasurement` 把 `integratedLufs` 置 `noReading (−100)`，`:196` 界面 Reset 在下一块开头调它；`:288-289` `recentCount < gatingSubBlocks (4)` 前不进直方图 → 至少 4 × 100 ms 无整体读数。`CeladonChain.h:125-130` `requestMeterReset` 同时清两块表。
- 界面可达：`PluginEditor.cpp:608-610` 三个入口（响度小窗、IN 表、OUT 表的 Reset）都接 `resetMeters()`（`:853-859`），预听中没有禁用。
- 规格字面如此：[[设计规格-高级母带控制]] `:76`「读数未知（≤ −90 LUFS 或非有限）时回到 1」；`tests/AdvancedMasteringTests.cpp:142-150`「quiet or unknown loudness never boosts」把这条固化成测试（每次 `module.reset()` 后喂 −90 / −100 / NaN / Inf → 增益恒 1）。

研究把它定为「符合规格字面、属规格层取舍」是准确的；对「曾有读数、短暂丢失」的场景这条规则确实变成爆响源（+6 dB 持续 0.4 s）。**不是已定取舍**的意义在于：规格这句是给「从未有读数」写的安全回退，Reset 中途丢读数的情形没人讨论过。

**严重度 / 优先级**：low / P2 维持。触发要「预听中点 Reset」，一次 0.4 s；改要动规格 §6 一句，必须 Max 点头。

**修法**：可用（fix_ok = true），一处建议：**去掉「约 3 s 超时回 1」**。读数变未知只有三条路——`clearMeasurement`（Reset、`prepare`、开关响度计量）；`prepare` 与整链 `reset` 同时会 `releaseSafety.reset()`（`CeladonChain.cpp:65 / 87`、`ReleaseSafety.cpp:14-22`），仍是「从未有读数 → 1」；剩下就是界面 Reset。预听只衰减、永不放大，「保持上次衰减直到下一份有效读数」在任何时刻都不会比现状更危险；而 3 s 超时会在「静音时点 Reset、3 s 后音乐再进来」这种情形里把爆响原样搬到音乐起点。用例保留研究写的三条即可（Reset 后 2 s 内 ≤ 稳态 + 0.5 dB；从未有读数仍为 1；`reset()` / 关预听 / 离线立即归一），去掉「3 s 不回来回 1」。现有 `:142-150` 用例每轮都先 `module.reset()`，不会被这个改法打破。

**风险**：只在 Reset 后 ≤ 400 ms（有信号时）的监听信号上变；母带不变。

## 2. 修法总表（给综合阶段）

| 编号 | 改哪里 | 逐位契约 | 现有测试 | 需要 Max 的 |
|---|---|---|---|---|
| R8-01 | `CeladonChain::setParams:147-155` 两行 + `:154` 注释 + 规格 §6 一句 | 预听从未打开时逐位不变 | 全部保持绿；新增 4 个用例 | 无（未测 DAW 的实现遗漏） |
| R8-02 | `LimiterModule::setParams`（Ceiling 钳 −4.5 ～ 0）、`ColorModule::setParams`、`readParams` | 有限输入逐位不变 | 保持绿；新增 NaN / ±Inf 用例 | 无 |
| R8-03 | `LoudnessMeter` 加 `setIntegrationPaused`（恢复重攒 4 子块）+ `CeladonChain::setParams` 一行 + 规格一句 | 零音频改动 | 保持绿；新增 A/B 6 s 用例 | 宿主旁通是否同样冻结（建议冻结） |
| R8-04 | `ReleaseSafety` 记住上次有效目标（无超时）+ 规格 §6 一句 | 只动 Reset 后 ≤ 400 ms 监听 | `:142-150` 保持绿 | §6 口径 |

## 3. 漏掉的同类问题（missed）

两条都比 R8-01 ～ R8-04 轻，列出来供综合阶段决定要不要一起做：

1. **同规格 `prepareToPlay` 的 `CeladonChain::reset()` 也把预听增益归一，而 `previewActive` 不变**（`CeladonChain.cpp:59-69` → `releaseSafety.reset()`；`:156` `previewActive` 只在 `setParams` 里改）。下一块起 `ReleaseSafety::process` 再从 1.0 用 50 ms 滑回目标；链此时刚清空，头 `getLatencySamples()`（48 kHz 为 1384 样本 ≈ 28.8 ms）个输出样本是延迟线里的静音，所以音频出来时斜坡已走完约 58 %：目标 −6 dB 时余下约 2.5 dB、≤ 21 ms 的过响。只在宿主每次开播都重新 `prepareToPlay` 的情形可闻，low。R8-04 的修法盖不住它（那是显式 `reset()`）；要修就让 `reset()` 只清抖动随机序列、保留 `previewGain`（`ReleaseSafety::reset` 拆成两半），或在 `CeladonChain::reset()` 之后把 `previewActive` 一并清掉、由下一块重新交接。
2. **试听差值结束后的 20 ms 淡出里 `releaseSafety.process` 不跑**（`CeladonChain.cpp:295` 的 `! differenceInvolved()` 在淡出期间仍为真）：`releaseAllowed` 已恢复、抖动位深已设回，但这 ≤ 20 ms 的输出没量化、没抖动。规格「差值试听不加抖动」把淡出算进试听也说得通，只是与「关掉抖动即逐位旁通」的边界不清，cosmetic；顺手把条件改成「淡出完成前照旧、淡出完成那一块起跑」即可，或者在规格里写明。

没有找到比研究四条更重的同类问题：`processSlice` 的顺序（OUT 表 → endBlock → Listen → 差值 → 预听 / 抖动）与规格 `:118` 一致；`setHostInfo` 的离线清预听、`processChain` 的旁通宽限、`LookaheadHold` 的非有限守卫、`AtomicHelpers::atomicMax` 对 NaN 的忽略（`NaN > current` 为假）都核过，没问题。

## 4. 核验方法与边界

- 读了 `CeladonChain.{h,cpp}` 全文、`ReleaseSafety.{h,cpp}` 全文、`LoudnessMeter.cpp` 的 reset / 子块 / 门块段、`LimiterModule.cpp` 的 `setParams` / 释放包络 / `MovingAverage` / 硬钳位、`ColorModule::setParams`、`BalanceModule / ClayModule::setParams`、`PluginProcessor.cpp` 的 `readParams` / `LookaheadHold` / `processChain` / `setStateInformation`、`PluginEditor.cpp` 的三种临时监听切换与表读数、`AtomicHelpers.h`、`ParameterRanges.h`、`GlazeController.cpp` 里与 Limit / Color 实际参数及 `paused` 有关的行；JUCE 9.0.2 的 `jlimit` / `NormalisableRange` / `RangedAudioParameter` / `AudioParameterFloat` / `APVTS ParameterAdapter` / `CharacterFunctions`。
- 档案：[[设计规格-总体]] 2026-09-28 修订段、[[设计规格-高级母带控制]] §6、[[设计规格-Limit]] 2026-09-28 修订、[[设计规格-界面-V4融合版]] 频谱仪 / A-B 段、[[设计规格-界面]] 表说明、[[ADR-013]]、[[ADR-020]]、`04-协调/TASKS.md` 与 T-032 计划、09-28 / 09-29 日志的相关行；仓库 `docs/design/0.1.9-mastering-controls.md`、`docs/validation/2026-09-28-mastering-controls/REPORT.md`。
- 测试：`tests/AdvancedMasteringTests.cpp` 的 ReleaseSafety / 交接 / 离线用例、`tests/ChainTests.cpp` Listen 斜坡用例；grep 了 `tests/` 与 `tools/` 里全部 Limit / Color 取值以确认钳制不改现有用例。
- 没做：没在 107 复跑实验（理由见文首）；没读 `AnalyzerTap.h`、`.jucer`、`CMakeLists.txt`（四条结论都不涉及）。
