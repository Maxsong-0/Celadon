# V5 · GLAZE 控制 / 校准 / 守护 / 画像 —— 反驳核验（2026-09-29，T-032）

核验对象：`R5-glaze-control.md`（研究代理）。底：worktree `fable-deep-research-634a5e`（main `e6b445b`，v0.1.8.5）。
方法：只读；每条结论都回到 `Source/dsp/GlazeController.cpp`（3330 行）、`GlazeCalibrator.cpp`、`GlazeGuard.cpp`、`GlazePlanner.cpp` 逐行核对（下文行号均为本工作树），再对照规格
`设计规格-GLAZE.md` §7（Codex 修订，「与子文档冲突时以本节为准」）、`-决策与校准` §6～7、`-时间线与保存` §4～7、`-实现修订` §3～5、ADR-013、BUG-003、
`tests/GlazeControllerTests.cpp`（用例 931、1636、12c / 12d、4344）。本机没编译、没跑测试；R5-05 的数值只手工复核代数（见该条），没上 107。
研究报告里 107 实验的原始数据（sizeof、子块配对误差、绝对门偏置）**未复核**，只核了它们支撑的结论是否成立。

## 0. 判决总表

| 编号 | 判决 | 严重度（研究 → 核验） | 优先级（研究 → 核验） | 修法 | 一句话 |
|---|---|---|---|---|---|
| R5-01 | **confirmed** | high → high | P0 → P0 | 部分采纳：**必须用①**，②单独不够 | 放弃分支不清「上次尝试」属实；另有**换风格再切回**同一死路（研究漏了），只有把记录挪到 finishRound 才两条都堵住 |
| R5-02 | **confirmed** | medium → medium | P0 → P0 | 采纳（全局单调计数最直接） | 两槽各自计数、单一「上次记的数」，逐块推演与研究一致；`GlazeGuard::reset` 不清 `limitsSet`，所以不会升级成「守护整个关掉」 |
| R5-03 | **confirmed** | medium → medium | P1 → P1 | 采纳 | 守护自己的钟只走有效音乐（规格 §7），「卡住」却按宿主样本数；旁通 Color / Limit 同样命中 |
| R5-04 | **confirmed**（机理） | medium → medium | P1 → P1 | **不采纳研究的②③，改为「只 carry 吸收量」** | 阶跃属实；但研究要在第一次拧 GLAZE 时开 3 s 方案过渡，**违反时间线 §4「拧 GLAZE 不重做方案过渡」** |
| R5-05 | **confirmed**（改进而非缺陷） | low → low | P2 → P2 | 有条件采纳：先过真歌门槛，Max 拍板 | 前提成立（方案净目标已含限幅损失预测、传递斜率 < 1）、代数复核无误；但保守 partial 是规格 §6 定的取舍，改步长要过 calib-loop |
| R5-06 | **confirmed** | low → low | P2 → P2 | 采纳，但工作量不止 S | 重复调用点与次数属实；缓存键要跨 beginBlock / endBlock 且 `advanceTransition` 会换槽，键错就破逐位 |
| R5-07 | **confirmed**（事实） | low → low | P2 → P2 | **①不采纳、②采纳** | 脚手架没接线属实；但节流 `publishState` 会把 V2-05「重建第一块沿用刚听到的偏差」从精确变成 ≤ 0.1 dB，用例 1636 断言 1e-5 是这条契约的守卫，不该为 0.1～0.3 pp 让路 |
| R5-08 | **confirmed** | low → low | P2 → P2 | 采纳 | 两处不对称属实；`commitCandidate` 那半在今天的出厂常数下实际影响 ≈ 0（各版本守护时长没变过），属潜伏 |
| R5-09 | **confirmed** | low → low | P2 → P2 | 采纳，但**二选一由 Max 定**，今晚不擅改规格条款 | 字段永不为真属实；权威规格 GLAZE.md §7.3 已改「离线照常学习」，三份子文档仍是 09-25 的旧文 |
| R5-10 | **confirmed**（事实） | low → low | P2 → P2 | 采纳但**今晚不做** | 项目没有代码文件长度硬约束（300 行规则只管档案）；今晚 R5-01～08 全在这一个文件里改，先拆必撞车 |

漏项（第 2 节）：M1 R5-01 的第二条触发路径（换风格往返，已并入 R5-01）；M2 约束反馈的「过渡 + 3 s 观察」按宿主时间计，静音把观察期走完后按过渡期的旧窗口再降 1 dB；
M3（存疑）沿用执行快照期间 `committed.plan` 被快照方案覆盖。

---

## 1. 逐条判决

### R5-01 放弃的校准轮把参考条件留在「上次尝试」里 —— confirmed · high · P0

**代码证据（都与研究一致）**

- `beginRound` 1533-1545：`lastAttemptReference = currentReference; haveLastAttempt = true;`（1540-1541）。
- 放弃分支 `updateCalibration` 1575-1581：`calibrator.reset(); roundActive = false; committedPhase = Phase::locked;`，不碰 `haveLastAttempt`。
- 开轮门 1620-1623：`&& ! (haveLastAttempt && GlazeCalibrator::referenceMatches (lastAttemptReference, currentReference))`。
- `haveLastAttempt = false` 只在 1357（`onHostReset` 沿用执行量）、2763（`consumeRestore`）、2944（`prepare`）。
- GLAZE ≤ 0：2440 `referenceKnown = false` → 轮中 1572 `matches = false` → `roundMismatchSamples` 按宿主样本累加 → 3 s 放弃。`updateCalibration` 在 `endBlock` 2606 无条件调用，GLAZE 0 时照跑，所以放弃真的会发生。
- 拧回后：2424-2426 `! referenceKnown` → `referenceStableSamples = 0`，1 s 后 256 位放开；`calibrationApplicable()` 若本来就为假（第一次校准、或条件刚变过还没校准成），则唯一挡门的就是 512 位，`currentReference` 与 `lastAttemptReference` 逐位相同（同画像号、风格、GLAZE、手动值、上限、采样率）→ 永远不开。
- 状态行：`GlazeStatusText.cpp:140` `awaiting = locked && ! calibrationValid` → 271 「已锁定 · 待校准（条件已变，稳定后自动校准）」——文案承诺「自动校准」，实际不会。
- Bypass 在哈希里：`emitConditions` 289 / 295 / 300 三个 `flag (bypass)`，切 Bypass ≥ 3 s 再切回同样命中。
- 512 门的本意：`GlazeCalibrator::begin` 395-399 目标非有限 → 立即 `finished` 且 `valid = false` → 下一块 `finishRound` → `calibrationApplicable()` 假 → 若无此门每块重开。`finish()` 158-166 其余所有结束路径都 `valid = true`，所以「试过而失败」在代码里**只有**这一种情形，研究的解读成立。

**研究漏掉的第二条触发路径（同一根因）**：`requestPlanTransition` 1061-1065 在轮中丢弃校准（`calibrator.reset(); roundActive = false;`）也不清 `haveLastAttempt`。校准中切风格（2354-2368 → `requestPlanTransition`），
再切回原风格：`feedbackMaxGainDb` 在 2357 被重置为不限，若原本就是不限，则切回后的 `currentReference` 与 `lastAttemptReference` 逐位相同 → 同样永远不开。其它丢轮路径（提交换画像号、
降上限型反馈改 `feedbackMaxGainDb`、画像失配、旧方案换新算法时 GLAZE 已变）都会改参考条件，不受影响。

**修法判定**：研究给的 ① 把两行挪到 `finishRound`、只在 `! committed.calibration.valid` 时记 `roundReference` —— 正确，且同时堵住上面第二条路径（丢轮不经 finishRound，永远不会再记）。
② 单独在放弃分支清 `haveLastAttempt` —— 只堵 GLAZE 0 / Bypass 那条，**堵不住换风格往返**，不能单独用。建议：只做 ①（② 可留作保险，无害）。
逐位契约：只在「放弃后条件回到原样」的场景改变输出（原来永不校准 → 现在校准），GLAZE 0 逐位契约不涉及；现有用例 931 只覆盖「校准完再回 0」，12d 两例都不走放弃分支，
4301 慢扫用例靠 1 s 精确稳定计时挡开轮，与此无关。不会形成开 → 放弃 → 开的紧循环：放弃要 3 s 不一致，重开要 1 s 精确稳定。

**验证建议补一例**：Rig 学完 → GLAZE 60 进 `calibrating` → 切风格 1 跑满过渡 → 切回 0 → 跑 4 s：断言 `calibrationBegins` 加 1、最终 `locked && calibrationValid`；修前停在 `calibrationGateMask & 512`。研究的两例（GLAZE 0 / Color Bypass）照加。

**严重度**：维持 high / P0。校准中做 A/B（GLAZE 0 或 Bypass）≥ 3 s、或试听另一种风格再切回，都是最常见的用户动作；后果是状态行永久撒谎 + 校准偏差（修正范围 3 dB × GLAZE × κ）永远不施加。工作量 S。

### R5-02 守护 / 上游监视上限按「槽自己的重算计数」去重 —— confirmed · medium · P0

**代码证据**：`PlanSlot::evaluations` 718 每槽各自；`loadSlot` 1007-1011 不清它；`startTransition` 1023 `toIndex = 1 - toIndex`；`evaluate` 1104 `++slot.evaluations`；
`updateGuardLimits` 1677-1689 只比 `guardLimitsEvaluation == toSlot().evaluations`；`guardLimitsKnown = false` 只在 2773 / 2924 / 3127。`evaluations` 全工程只有这 4 处引用（grep），没有别的消费者。

**逐块推演（复核）**：第一块 `! styleKnown` → `snapTo` 槽 0 → `refreshPlans` 评估 → 槽 0 = 1 → 守护记 1。首次聆听第一个候选 2397 `requestSource` → `startTransition` → 槽 1 评估 = 1 → `1 == 1` 跳过；
1 s 后下一次更新回槽 0 = 2 → 更新；再到槽 1 = 2 → 跳过……不碰旋钮时两槽计数永远差 ≤ 1，每隔一次方案切换守护与 `monitor` 沿用上一份方案的 `caps`。
`caps = blendConstants (constants, plan.weights)`：首次聆听时权重未锁、随候选画像变；提交后锁定；换风格 `weightsFor` 变。所以受害场景 = 首次聆听全程（隔次）、提交后（一半几率）、换风格后（一半几率），直到用户碰任一进方案的旋钮。
`lowerGainCap` → `requestPlanTransition` 也翻槽，但权重不变、caps 相同，无害；`guard.reset()`（`GlazeGuard.cpp:105-113`）**不清 `limitsSet`**，所以跳过更新时守护仍按旧 caps 工作，不会变成「无限制 = 关掉」——研究没说错，这里只是排除更坏的可能。

**修法判定**：两种都对。全局单调计数（`Impl::evaluationCounter`，`evaluate` 里 `slot.evaluations = ++evaluationCounter`）改动最小、语义直接，推荐。改后输出在首次聆听 / 提交 / 换风格且守护介入的场景会变；
用例 2885、3755 里的换风格只是零分配计数用例，逻辑不受影响；12a～12d 都是单风格 + 反馈翻槽（caps 不变），不该变。全套 132981 项必须在 107 重跑确认哪些断言变了。

**严重度**：medium 合理（守护按错的 GR 上限判定，最多多退 3 dB 或过早触发上游反馈多降 1 dB；试听风格整首不碰旋钮正是常见情形）。P0 只因为是一行改动。

### R5-03 「保护层卡住」按宿主样本时间计 —— confirmed · medium · P1

**代码证据**：`endBlock` 2576-2593 分支只看 `active && ! limitedByManual`，不看 musicActive；2588 `guardSteadySamples = sameBits (guardDb, lastGuardDb) ? guardSteadySamples + n : 0`，n = 整块。
`GlazeGuard::process` 181-182 `! stats.musicActive → return protectionDb`（整块不计、`protection` 不动）→ 静音 / 停播 / `paused` / Color 或 Limit Bypass（2581 `guardStats.musicActive` 的四个条件）期间「没变」恒成立。
`updateFeedback` 1937-1942 用它判「卡住」→ `applyFeedback (0)`；`guardStuckSamples = guardRecoverAfterSeconds + 1 = 6 s`（943）；校准门 1604 同用。
`updateFeedback` 本身也没有 musicActive 门：`active`、有画像、不在过渡、不受手动限制、冷却到期（宿主时间）就判，停播中照样执行 `lowerGainCap`。

**规格对照**：`-决策与校准` §7「静音、停播不计入恢复时间」；`-实现修订` §3「保护层在同一退让值上卡住超过它自己的恢复等待 + 1 s」——「它自己的恢复等待」按规格只走有效音乐（`GlazeGuard.cpp:302-311` `safeSlots` 只在时隙完成时加），
拿宿主时间的计数去比音乐时间的门槛，推断「超过恢复等待仍没动 = 卡在回差区」不成立。研究判「与规格不一致」成立（规格没有一个字写「卡住按音乐时间」，但按恢复等待的口径推是唯一自洽的读法）。

**后果复核**：g_保护 = −1 dB 时停播 6 s → 上限 = 方案净提升 − 1 dB，随工程保存、只降不升（换风格 / 提交才回到不限）；音乐回来本可 5 s 后恢复到 0。另一种：Limit Bypass 开着 6 s → 守护冻结在 −1、`executedGuardDb` 仍 −1（`computeEffective` 2173 照读 `guard.getProtectionDb()`）→ 吸收 → 关掉 Bypass 后方案永久低 1 dB。
研究关于「`guardSettled` 在停顿中放开校准门 → 恢复播放后预算受限」的第二段推演偏弱：守护 engaged 时 `calibrator.process (…, guardActive = true)` 每块中断，但 `guardSamples` 只累计门限以上的入口子块，恢复播放后若素材已不热、守护 5 s 后开始恢复，不一定用完 10 s 预算。主结论不依赖这一段。

**修法判定**：`if (guardStats.musicActive) guardSteadySamples = …;`（静音不加也不清，`lastGuardDb` 照旧每块更新）正确，四种冻结情形一并覆盖。`samplesSinceFeedback` / `samplesSinceMismatch` 研究说保持宿主时间即可——见漏项 M2，前者其实也有同类问题，但可以分开处理。
改后输出只在有停顿 / 旁通的场景变；12c / 12d 用连续素材，`view.musicActive`（`MusicAnalyzer.cpp:1027-1029`：绝对门 + I − 20 LU 相对门）在合成素材上恒真，不受影响。全套重跑。

**严重度**：medium / P1 合理。

### R5-04 执行快照解除冻结没有连续性 —— confirmed（机理）· medium · P1 · 修法须改

**代码证据（与研究一致）**：`adoptExecution` 1398-1407 把 `absorbed = max (−increments.limitGainDb, protectionDb)` 并进 `plan.increments.limitGainDb / effectiveGainDb / effectiveTargetLufs`，`protectionDb = 0`；`desiredSource` 969-979 冻结（`useSavedPlan = true`）；
`beginBlock` 2326-2343 参考条件一变 → `executionHeld = false … snapTo (desiredSource())`，新方案源按已提交画像用 `makePlan` 重算，不含 `absorbed`；`consumeRestore` 2764 / `onHostReset` 1369 都 `guard.reset()`，守护从 0 起步；模块侧只有自己的平滑。
换风格路径：2330 `reference.style = style` 不匹配 → 先 `snapTo` 到**旧风格重算方案**（`currentStyle` 还是旧的）→ 2354 再 `requestPlanTransition` 到新风格：过渡起点是重算出的旧风格方案而非冻结的快照方案，起点处同样阶跃。
`captureExecution` 2500-2510 存过渡混合值，所以过渡中途保存 / reset 的快照也带混合值。对照 `lowerGainCap` 1816-1822 用 `guardCarry` 保净增益连续，这里没有。机理属实。

**研究修法的问题**：② 把解除冻结改成 `requestSource (…, planTransitionSamples)` 3 s 过渡——时间线与保存 §4 明写「拧 GLAZE：…**不重学、不重做方案过渡、不重启校准**」（Max 2026-09-25 设计确认）。研究自己也承认「相当于把解除冻结当成一次方案切换」，这是在改 Max 定的路径语义，不能在核验里放行。
③ `onHostReset` 过渡中不 `snapTo`——是设计取舍（reset 后「听到的与 reset 前一致」，冻结在混合值上直到条件变），研究置信度也低，今晚不动。

**调整后的修法（只碰吸收量，不碰路径语义）**：`Impl` 加 `absorbedGuardDb`（`adoptExecution` 记 `absorbed`，非沿用时清 0）；解除冻结的两处（2331-2342、换风格那一支）保持 `snapTo`，但加
`guardCarryStartDb = absorbedGuardDb; guardCarryElapsed = 0; guardCarryLength = planTransitionSamples;`（与 `lowerGainCap` 1816-1818 同一套；`computeEffective` 2173 `guardDb = guard.getProtectionDb() + guardCarryDb()` 自动生效）。
效果：新方案（不含吸收量）+ carry（吸收量 3 s 线性淡出）= 净增益在吸收量这一项上连续，GLAZE 旋钮仍即时；过渡中途保存那一小部分混合值差仍是阶跃（少见，接受）。`heldBiasDb` 那半已按 1 dB/s 滑，不用动。
验证按研究的 12c 变体即可（GLAZE 60 → 61 逐块 `effective.limit.gainDb` 变化 ≤ 1 dB/s × 块长），换风格再加一例。

**严重度**：medium / P1 维持（热素材守护常年非零；阶跃最多 3 dB、通常 0.5～2 dB）。工作量 M → 调整后约 S+。

### R5-05 校准器单位增益修正忽略限幅损失斜率 —— confirmed（收敛改进，非缺陷）· low · P2 · 有条件采纳

**前提复核**：`GlazePlanner.cpp:538` `e.netDb = color.loudnessDb + gainDb − limitLossDb (…)`、`1571` `plan.effectiveGainDb = manualNetDb + strength × fullPlan.gainDb`——方案的 `effectiveGainDb` 是**净**目标、已含 0.04·E² 的损失预测（`GlazeConstants.h` `limitLossCoeff / Exponent`【实测】）；
校准器 `begin (toSlot().plan.effectiveGainDb, …)` 以它为 `target`，`errorFor` 88 量实际净提升 → e 是纯模型残差；再往 Limit Gain 加 e，净提升只变 s·e，s = 1 − 0.08·E。前提成立。
`completeRound` 330 `proposed = clampToLimit (applied + error)`、`finishConservative` 168-177、`calibrationRounds = 2`（`GlazeConstants.h:105`）、容差 0.25（`GlazeCalibrator.cpp:34`）——与研究一致。
**代数手工复核**（E0 = 6，δ = 0.8）：第 1 轮 e = 0.8 → 施加 0.8；第 2 轮净变化 = 0.8 − 0.04·(6.8² − 6²) = 0.39 → 残差 0.41 > 0.25 → `round >= rounds` → `finishConservative` → 偏差 0.8、partial。斜率步：0.8 / 0.52 = 1.54 → 净变化 1.54 − 0.04·(7.54² − 36) = 0.706 → 残差 0.094 → valid。与研究表格逐格一致。

**为什么不是 P1**：规格 `-决策与校准` §6「第二轮主要验证第一轮；仍有明显偏差…就接受保守、未完全校准的结果」——两轮 + 保守 partial 是设计取舍，少修不是「错」；真歌上 |e| 的分布报告里没有（BUG-003 记到过 +1.47 的 e，但那是保护层参与的病态轮）。
斜率靠模型外推：Clip 分担超出时损失按 `clipLossFloor` 缩，真实斜率可能比模型高（步子过大 → 第 2 轮 e < 0 → `finishConservative` 往下修、不验证，可收回但仍是 partial）。研究把斜率钳到 [0.35, 1] 是对的，但 0.35 这个下限没依据。

**修法判定**：方向正确，可做；但**改校准结果 = 改 22 首真歌门槛的锁定率与响度**，必须过 `calib-loop.sh` 并由 Max 拍板，不属于今晚「无争议落地」那一档。要做的话 `begin()` 多传 `estimatedExcessDb` 与 `limitLossCoeff / Exponent`，`completeRound` 与 `finishConservative` 都按 e / s。

### R5-06 热路径每块重复算纯函数 —— confirmed · low · P2

**代码证据（逐处核对）**：`blendConstants` 2075 / 2117 / 2137；`confidenceScale` 2076 / 2138 / 1516（`correctionLimitDb` 被 `biasTarget` 1649 与 1653 各调一次，`updateBias` 2610 → `biasTarget`，`beginBlock` 2477 首块也调）；
`matchingLiveSlopes`（27 段加权回归 1972-1989）2016 / 2081；`calibrationApplicable()`（→ `referenceMatches` 514-549 约 15 项比较）1616 / 1621 / 1645 / 1647 / 1937 / 2642。属实。
`blendConstants` 独热时早退（`GlazePlanner.cpp:1290-1293`）只拷 20 个浮点；自动风格权重混合时 20 × 3 乘加。`matchingLiveSlopes` 两次是大头。合计 0.3～0.6 µs / 块，192k/32（每块 22.7 µs，T-027 §4）约 0.2～0.3 个百分点，48k/512 可忽略——量级与研究一致。

**修法判定**：可做、逐位不变（同输入纯函数）。但「S」偏乐观：`computeEffective` 在 `beginBlock`，`updateLiveTone / Color` 在 `endBlock`，中间 `advanceTransition` 2605 可能 `startTransition (pendingSource)` 换掉 `toSlot()`；`refreshPlans` 在 `beginBlock` 又可能重算权重。
缓存键必须 = (`toIndex`, `toSlot().source.generation`, `toSlot().evaluations`（按 R5-02 改成全局计数后就够）)，并在 beginBlock 与 endBlock 各校验一次；`confidenceScale` 键 = `committed.profileId` + 常数代。键漏一项就破逐位。建议排在 R5-02 之后做，用 T-027 式「逐块比缓存与现算逐位相同」的用例守。

### R5-07 `publishState` 每块整份拷贝、100 ms 节流脚手架未接 —— confirmed（事实）· low · P2 · ① 不采纳、② 采纳

**代码证据**：`statePublishSeconds` 100、`statePublishSamples` 782 / 944 只被赋值；`stateDirtyUrgent / Lazy` 写于 1138 / 1249 / 1557 / 1755 / 1811 / 2364、清于 2728 / 2775-2776，**没有任何读作条件的地方**（grep 全部引用核对）；`publishState` 2701-2730 每块 `tryEnter` 后拷 `committed`、`lastExecution`、再 `makeReference`；`publishStatus` 2696-2699 每块 seqlock 写。属实。
文件头注释 20-21 自述「每块尝试发布」，2705 注释又说「保存不落后 100 ms」——两种意图并存，脚手架是遗留。sizeof 数字未复核，量级（约 5 KB）按结构体字段看合理。

**为什么 ① 不能直接上**：`prepare()` 2880-2899 同规格重建从 `published.execution` 冷启动，注释明说「prepareToPlay 会先 setParams，lastExecution 此时可能已是下一块尚未听到的量」——设计**依赖 published 精确等于最后听到的那一块**。
用例 1636 断言「第一块沿用刚听到的偏差」到 1e-5、且 `calibrationBegins` 不变，是 V2-05 契约的守卫；节流后偏差可差 0.1 dB、过渡中途可差 3 %，研究提议「按新节奏改断言」= 把契约从精确放宽到 0.1 dB。
块长重建是偶发事件、0.1 dB 听不出来，但用 0.1～0.3 个百分点（只在 192k/32）去换一条已验收契约的精度，应由 Max 决定，不在核验里放行。另外 `publishStatus` 一次 17 个原子写约 20 ns，不值得节流。

**可放行的部分**：② 按 `toSlot().source.generation` 只在方案源变时拷 `execution.profile`（1280 B）与 `constants`（1168 B），其余每块照拷——精确语义不变、省掉大头。`committed` 那份同理可按 `stateDirtyUrgent || stateDirtyLazy` 才拷（这两个标志正是为此设的；`refreshPlans` 改 `committed.plan` 时已置 `stateDirtyLazy`，`finishRound / commit / feedback / 换风格` 置 `stateDirtyUrgent`；还要补：`consumeRestore` 后第一块、`committed.calibration.reference` 在 `lowerGainCap` 1802 改绑时）。
若走这条路，必须逐项核对所有写 `committed` 的地方都置了脏标志，否则存档漏更新——比 ① 更需要一个「published.committed 与 committed 逐块逐位相同」的用例守着。

### R5-08 读档与提交两条路径的运行态初始化不对称 —— confirmed · low · P2

- `consumeRestore` 2734-2782：清了 calibrator / guard / monitor / carry / 过渡 / `samplesSinceFeedback`，**没有 `clearMismatch()`**；`onHostReset` 1381、`onTransportJump` 1330、`onLoopWrap` 1339、`prepare` 2912 都清。属实。
  影响复核：`updateMismatch` 1863-1875 用累计的 `mismatchSamples / mismatchEnergy` 与**新**画像的 I 比；累计的是最近一段真实音乐（M 一低于回差线就清），所以读档后立即触发的「这段比新画像响很多」判断本身不算错，错的只是跳过了 `sustain` 的持续要求。低。
- `commitCandidate` 1219-1250 设 `committed.constants = constants`，`guardStuckSamples`（943，取 `activeConstants().guardRecoverAfterSeconds`）与 `monitor.configure` 只在 2779-2780 / 2927 / 3126-3129 折算。属实。
  但 `updateGuardLimits` 1685 `guard.setLimits (caps, slot.source.constants)` 会让**守护自己**的时长跟着方案源走（`GlazeGuard::setLimits` 159-173 `timingChanged → updateTiming`），只剩编排器侧的 `guardStuckSamples` 与 `monitor` 窗停在旧值；而出厂 `guardRecoverAfterSeconds / guardWindowSeconds / guardTriggerSeconds` 从版本 1 到 7 没改过，今天实际影响 ≈ 0，是潜伏项。

修法（`consumeRestore` 加 `clearMismatch(); samplesSinceMismatch = mismatchCooldownSamples;`；`commitCandidate` 末尾 `updateTiming(); monitor.configure (sampleRate, activeConstants());`）正确、音频线程可调（几个乘法 + 固定数组清零）。注意 `monitor.configure` 内含 `clear()`，提交时清监视窗本来就合理。

### R5-09 `unlearnedOfflineWarning` 永不为真 —— confirmed · low · P2 · 二选一由 Max 定

**代码证据**：911 声明；1244 / 2393 / 2769 只写 false；2668 `s.unlearnedOfflineWarning = unlearnedOfflineWarning && …` 恒假（grep 全工程只有这几处 + 测试直接置位）。`GlazeStatusText.cpp:185` `Kind::unlearnedExported` 生产路径永不可达；`PluginEditor.cpp:1110 / 1132` 与 `HelpText` 还挂着它。属实。

**规格对照（研究说得对，且比它说的更明确）**：`设计规格-GLAZE.md` §7 头「本节与子文档冲突时，以本节为准」，§7.3「实时和离线用同一套样本时间的学习、校准与保护反馈：切到离线不停止学习…**这一条修订了 时间线与保存 的离线导出节和 实现修订 第 5 节 V2-03**」。
代码 2389-2391 / 1257 与 §7.3 一致；`-时间线与保存` §7 第 70 行「从没学过的实例被导出：用全程固定的保守方案…提示「未学习，导出使用保守方案」（Max 2026-09-25 按推荐定）」、`-实现修订` §4 状态行三处补充与 §5 V2-03、`-界面与实现` §1 表第一行，都是被 §7.3 推翻但没同步的旧文。

**修法判定**：① 删字段 / Kind / 文案 + 三份子文档改成「离线导出照常学习」——干净，但动的是 Max 09-25 拍板的条款（虽已被 §7.3 覆盖）；② 接上提示——提示内容得改（导出未学习实例：前 15～30 s 用保守方案、边听边推，导出结果不是最终效果），其实对用户更有用。
两者都可行；选哪个是产品决定，**今晚先问 Max**，代码侧的死字段清理随决定一起做。`Kind::unlearned` 在 GLAZE > 0 时的文案「（导出将用保守方案）」同样半过时，一并定。

### R5-10 单文件 3330 行 / Impl 约 2100 行 —— confirmed（事实）· low · P2 · 今晚不做

行数与职责划分属实（`Impl` 749-2849；`namespace {}` 里参考条件哈希 158-466 + 3242-3329、seqlock 514-568、`UpstreamMonitor` 579-692 都是可独立的单元）。拆分方案合理，纯函数两块零风险。
但：① 项目没有代码文件长度硬约束（全局 CLAUDE.md 的 ≤ 300 行只管档案文件）；② TASKS.md 第 86 行今晚「一个文件只归一个代理」，R5-01～08 全在 `GlazeController.cpp` 里改，先拆必然撞车；③ 新文件要同时进 `Celadon.jucer` 与 `CMakeLists.txt`（ADR-003 Projucer 为主、CMake 给 Linux 测试），Mac 侧还得 Projucer 重存。
研究自己也排在「下一轮」。同意：登记待认领，不算今晚的活。

---

## 2. 漏项（研究没报、核验时发现）

**M1（已并入 R5-01）**：`requestPlanTransition` 1061-1065 丢轮不清 `haveLastAttempt` → 校准中换风格再切回同样永远不开轮。研究的修法 ① 顺带堵住，② 单独不堵。

**M2 · low · P2 · 与 R5-03 同族**：约束反馈的冷却 `samplesSinceFeedback`（1913，宿主时间；门槛 = 过渡 + 3 s，938）本意是「过渡后观察新方案 3 s」，但 `monitor` 只在有效音乐时加时隙（622-624）、`lowerGainCap` 1779 清窗后过渡期间照加（2591-2592，过渡中 `updateFeedback` 只是早退 1922-1924）。
反馈 → 3 s 过渡（窗内装满过渡期的混合数据）→ 静音 / 停播 3 s（窗不动，冷却在宿主时间里到期）→ `updateFeedback` 没有 musicActive 门（1922），`monitor.isOver()` 按过渡期的旧窗判 → 可能在**静音中**再降 1 dB，而设计要的是观察新方案 3 s。
守护那条（`planLimitedLatched`）要边沿、边沿要音乐，不受影响。修法：过渡结束时（`advanceTransition` 1160 处）`monitor.clear()`，或 `updateFeedback` 的上游判定加「窗内时隙数 ≥ `triggerSlots`」；或干脆让「观察 3 s」只在 `guardStats.musicActive` 时累加（与 R5-03 同一处改）。低频、低影响，可与 R5-03 一起做。

**M3 · low · 存疑，问 Max**：`refreshPlans` 1135-1139 「目标方案属于已提交版本时一并更新 `committed.plan`」的条件是 `committed.hasProfile && ! savedPlanMode && toSlot().source.hasLockedWeights`；沿用执行快照时 `desiredSource` 973 `hasLockedWeights = true`、977 `useSavedPlan = true`，
条件成立 → 读档后第一块 `committed.plan := execution.plan`（含 `adoptExecution` 并进去的保护退让，1402-1404），并置 `stateDirtyLazy` 随下次保存写进正式版本。文件头 21 行说「执行快照…未完成的候选与校准不混入正式版本」，这里方案混进去了。
后果：多次「保存时守护非零 → 重开 → 不碰旋钮再保存」正式方案单调下探；将来算法不兼容时 `savedPlanMode` 照用的是被吸收过的增量。另一面：重开后校准轮会在这份方案上重测、`committed.calibration` 也就对应它，自洽。是不是有意为之说不清，列为存疑；若不是，修法一行：条件加 `&& ! toSlot().source.useSavedPlan`。

**核过、不算问题的**：`GlazeGuard::reset` 不清 `limitsSet`（R5-02 不会升级）；`updateGuardLimits` 在 `advanceTransition` 之前用上一目标的 caps 最多晚一块；`manualNetGainDb` 1694 调 `desiredSource()` 白递增一次 `generationCounter`（无害，槽的 `source.generation` 不变）。

---

## 3. 建议落地顺序（供主进程综合）

1. **今晚必做（S，同一处改、一套用例）**：R5-01（修法 ①，补换风格往返一例）→ R5-02（全局计数）→ R5-03（+ 可顺手做 M2 的 `monitor.clear()`）→ R5-08。改完 107 全套一次跑完，逐条看变化的断言是否都在预期场景内（GLAZE 0 逐位契约与 12c / 12d 不该变）。
2. **今晚可做（S+）**：R5-04 按调整后的「只 carry 吸收量」做，加 12c 变体用例。
3. **要 Max 拍板再做**：R5-05（改校准结果，过 calib-loop 真歌门槛）、R5-07 ①（放宽 V2-05 契约）、R5-09（删提示还是接上提示）、M3（是否有意）。
4. **顺手 / 下一轮**：R5-06（排在 R5-02 后，键要跨 beginBlock / endBlock）、R5-07 ②（按 generation 拷方案源）、R5-10（登记待认领）。

ADR-013 提醒：以上没有一条碰浮点合并 / 编译选项；「逐位」这里指的是 132981 项测试的现有断言。R5-01 / 02 / 03 / 04 / 08 都会在各自场景改变输出，属预期；改前先跑一遍基线留日志，改后逐条对照失败项。
