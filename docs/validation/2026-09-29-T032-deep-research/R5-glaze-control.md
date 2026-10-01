# R5 · GLAZE 控制 / 校准 / 守护 / 画像 —— 深度审计（2026-09-29，T-032）

范围：`Source/dsp/{GlazeController,GlazeCalibrator,GlazeGuard,GlazeProfile}.{h,cpp}`、`GlazeTypes.h`、`ChainParams.h`，以及它们与 `CeladonChain` / `PluginProcessor` 的接线。
底：worktree `fable-deep-research-634a5e`（main `e6b445b`，v0.1.8.5）。只读审计，未改任何源码；本机没编译、没跑测试。
数值实验在 107（`~/.cache/celadon-t032/R5/`，python3 + numpy，g++ 15.2）上做，做完已删目录。

先读的档案：`设计规格-GLAZE.md`（含第 7 节 Codex 修订）、`-决策与校准`、`-时间线与保存`、`-实现修订`、`-分析器`、`-界面与实现`、ADR-008、ADR-018、BUG-003、BUG-004、T-027 REPORT。

## 0. 结论总表（按 优先级 × 置信度 排序）

| 编号 | 结论 | 严重度 | 优先级 | 类别 | 置信度 | 工作量 | 改后输出 |
|---|---|---|---|---|---|---|---|
| R5-01 | 校准轮因条件临时变化（GLAZE 拧到 0 做 A/B、切模块 Bypass ≥ 3 s）被放弃后，条件变回原样就**永远不再开轮**——「刚对同一条件试过」的门把「放弃」也当成了「失败」 | high | P0 | 状态机漏洞 | 0.85 | S | 变 |
| R5-02 | 保护层 / 上游监视的约束上限按「方案槽自己的重算计数」判断是否要更新，两个槽的计数每隔一次方案切换就撞上一次 → 换风格 / 提交 / 重学后守护沿用**上一份方案的上限**，直到用户碰任一旋钮 | medium | P0 | 算法正确性 | 0.85 | S | 变 |
| R5-03 | 「保护层卡住」用宿主样本时间计时，静音 / 停播也算 → 音乐停 6 s（或一段空拍）就把临时退让**永久**并进方案上限（只降不升） | medium | P1 | 与规格不一致 | 0.75 | S | 变 |
| R5-04 | 重开工程 / 宿主 reset 后方案被冻结成执行快照（含吸收进去的保护退让、过渡中途的混合值）；第一次碰旋钮 / 换风格时快照直接被丢掉、`snapTo` 到新方案 → 净增益**阶跃**最多 3 dB（退让量）＋过渡剩余量 | medium | P1 | 设计缺陷 | 0.65 | M | 变 |
| R5-05 | 校准器按「建议 = 实际 + e」单位增益修正，忽略限幅损失随增益的斜率（d net/dG = 1 − 0.08·E）→ E ≥ 4 dB 时两轮几乎必然 partial，向上系统性少修 0.3 ～ 0.8 dB | low | P2 | 校准收敛 | 0.6 | M | 变 |
| R5-06 | 热路径每块重复算纯函数：`blendConstants` ×3、`confidenceScale` ×4～5、`matchingLiveSlopes`（27 段回归）×2、`referenceMatches` ×6 | low | P2 | 优化 | 0.7 | S | 不变 |
| R5-07 | `publishState` 每块在自旋锁里整份拷贝 5312 B 的 `SaveState`，`publishStatus` 每块写 17 词 seqlock；100 ms 节流与脏标志的脚手架全在、却没接上 | low | P2 | 优化 | 0.7 | M | 音频不变；存档快照最多晚 100 ms |
| R5-08 | 读档与提交两条路径的运行态初始化不对称：`consumeRestore` 不清画像失配累计；`commitCandidate` 换了 `committed.constants` 却不重折算守护时长 / 上游监视窗 | low | P2 | 设计缺陷 | 0.7 | S | 变（仅这两种情形） |
| R5-09 | `Status::unlearnedOfflineWarning` 从未被置 true：界面 `Kind::unlearnedExported` 与规格「未学习，导出使用保守方案」都是死文案 | low | P2 | 与规格不一致 | 0.8 | S | 不变 |
| R5-10 | `GlazeController.cpp` 3330 行、`Impl` 一个结构体约 2100 行 / 约 130 个成员，七种职责混在一起；给出可分段落地的拆分方案 | low | P2 | 结构 | 0.8 | L | 不变 |

经实验**否定、不报**的两条见第 3 节：校准器子块配对的 ~28 ms 错位（误差 p95 0.066 dB，远小于 0.25 dB 容差）；校准器只用 −70 LUFS 绝对门（对分析器门会排除的素材偏置 ≤ 0.13 dB）。

---

## 1. 逐条详情

### R5-01 放弃的校准轮把参考条件留在「上次尝试」里，条件变回去就永远不再校准

**严重度 high · P0 · 置信度 0.85 · 工作量 S · 改后输出：变（校准会重新开轮）**

证据（`Source/dsp/GlazeController.cpp`）：

```cpp
// 1533-1545  beginRound：开轮时记下「上次尝试」
lastAttemptReference = currentReference;
haveLastAttempt = true;

// 1570-1581  updateCalibration：轮中条件变了 3 s → 放弃本轮
if (roundMismatchSamples >= abandonSamples)
{
    calibrator.reset();
    roundActive = false;
    committedPhase = Phase::locked;     // ← haveLastAttempt 没有清
}

// 1620-1626  开新一轮的门：
&& ! (haveLastAttempt && GlazeCalibrator::referenceMatches (lastAttemptReference, currentReference)))
```

`haveLastAttempt` 只在 `onHostReset` 沿用执行量那一支（1357）、`consumeRestore`（2763）、`prepare`（2944）被清；`finishRound` 与放弃分支都不清。

机理：门 512 的本意是「`begin()` 因目标非有限立即结束为无效时，不要每块反复重开」（注释：「试过而失败的不反复重试，等条件变了再说」）。但放弃分支也走同一份 `lastAttemptReference`。两种最常见的用户动作正好命中：

1. **校准中把 GLAZE 拧到 0 做 A/B、3 s 后拧回原值**：`!active` → `referenceKnown = false`（2440）→ 轮中 `matches = false`（1572）→ 3 s 后放弃；拧回来后 `currentReference` 与 `lastAttemptReference` 逐位相同 → 门 512 永远为真。
2. **校准中切 Balance / Color / Limit 的 Bypass 对比 ≥ 3 s 再切回**：Bypass 在 `emitConditions` 的哈希里（289 / 295 / 300），同样放弃后条件回到原样。

之后 `calibrationApplicable()` 为 false、`committedPhase = locked` → 状态行写「已锁定 · 待校准（条件已变，稳定后自动校准）」（`GlazeStatusText.cpp:140, 271`），但**永远不会自动校准**，校准偏差保持 0（纯前馈），直到用户改了别的什么条件。`paused`（宿主旁通 / 听原声）不受影响：它只作废子窗、不改参考（1588-1589）。

修法（两处，都在 `GlazeController.cpp`）：

1. 只有「试过且失败」才记下上次尝试：把 1540-1541 两行从 `beginRound` 挪到 `finishRound`，写成
   `if (! committed.calibration.valid) { lastAttemptReference = roundReference; haveLastAttempt = true; }`（`begin()` 目标非有限 → 立即 finished、`valid = false`，正是门 512 要防的循环）。
2. 放弃分支（1575-1581）显式 `haveLastAttempt = false;`（保险；挪走第 1 步后它本来就不会再为真）。

验证（`tests/GlazeControllerTests.cpp` 新用例）：Rig 学完 → GLAZE 60 开轮进入 `calibrating` → 拧 GLAZE 0 跑 4 s（> `calibrationAbandonSeconds` 3 s）→ 拧回 60 → 跑 3 s：断言 `probe.calibrationBegins` 加 1、最终 `phase == locked && status.calibrationValid`；同样用 Color Bypass on/off 4 s 做第二例。修前两例都停在 `calibrationGateMask & 512`。现有用例 931（「GLAZE 0 … turning it up calibrates; back to 0」）只覆盖「先校准完再回 0」，抓不到。

---

### R5-02 守护 / 上游监视的上限按「方案槽自己的计数」去重，两槽计数撞上就不更新

**严重度 medium · P0 · 置信度 0.85 · 工作量 S · 改后输出：变（守护按正确上限判定）**

证据（`GlazeController.cpp`）：

```cpp
// 710-719  每个方案槽自己的重算计数
struct PlanSlot { … std::uint32_t evaluations = 0; /* 每重算一次 + 1（保护层上限等据此知道方案变了） */ };

// 1007-1011 loadSlot：换方案源时不清 evaluations
slot.source = source; slot.valid = false;

// 1021-1028 startTransition：toIndex 翻转，两槽轮换
toIndex = 1 - toIndex; loadSlot (toSlot(), source);

// 1104  evaluate：++slot.evaluations;

// 1677-1689 updateGuardLimits：只比「当前 to 槽的计数」与上次记的数
if (guardLimitsKnown && guardLimitsEvaluation == slot.evaluations) return;
const auto caps = blendConstants (slot.source.constants, slot.plan.weights);
guard.setLimits (caps, slot.source.constants);
monitor.setCaps (caps);
guardLimitsEvaluation = slot.evaluations;
```

`guardLimitsKnown` 只在 `consumeRestore`（2773）、`prepare`（2924）、`setConstantsForTesting`（3127）清零，方案切换不清。

机理（逐块推演）：第一块 `snapTo` → 槽 0 重算一次（`evaluations = 1`）→ 守护记 1。任何一次方案切换（首次聆听每秒的候选更新、提交、换风格、约束反馈）都 `startTransition` 到另一槽并重算一次：槽 1 的计数从 0 变 1 → `1 == 1` → **跳过**；下一次切换回槽 0 → 2 ≠ 1 → 更新（记 2）；再切到槽 1 → 2 == 2 → 跳过……两槽的计数总是相差 ≤ 1，所以**每隔一次方案切换守护就沿用旧方案的上限**，直到用户碰任一进方案的旋钮（重算 → 计数 +1 → 才更新）。同样的 `syncClayConstants`（2243-2253）用的是全局递增的 `source.generation`，没有这个问题。

后果：`caps` 只随权重变，所以约束反馈（权重不变）不受影响；**换风格与重学提交**受影响最大——例如从「温润」切到「通透」，守护有一半机会仍按温润的 `limitGrMeanCapDb 1.0 / p95 2.5 / peak 4.0` 判定（通透是 1.5 / 3.5 / 6.0，`GlazeConstants.cpp:61-63, 105-107`），比方案该有的更早退让、最多少 3 dB；反过来切到温润时按通透的 Glue 上限（1.5 / 2.5 vs 2.5 / 4.0）过早触发上游反馈 `monitor.isOver()` → 多降 1 dB 方案。整首歌都不碰旋钮的情形（试听风格）正好是最常见的。

修法：把去重键改成「方案源代 + 该源下的重算次数」。最省事：`Impl` 加 `std::uint32_t guardLimitsGeneration`，`updateGuardLimits` 判 `guardLimitsKnown && guardLimitsGeneration == slot.source.generation && guardLimitsEvaluation == slot.evaluations`，更新时两者都记。或者把 `evaluations` 换成全局单调的 `evaluationCounter`（`Impl` 成员，`evaluate` 里 `slot.evaluations = ++evaluationCounter`），一行改动、语义直接。

验证：新用例——学完锁定（自动风格），风格切「温润」跑满过渡后，读 `guard` 生效的上限（现有 `TestProbe` 没暴露，加一项 `guardLimits`），应等于 `blendConstants(committed.constants, weightsFor(warm))`；再切「通透」再比。修前第二次切换必然不等。也可用行为断言：温润 → 通透后用 ×5 的热素材，通透的 `limitGrPeakCapDb 6.0` 下守护不该退让，修前按温润的 4.0 会退。

---

### R5-03 「保护层卡住」按宿主样本时间计，静音 / 停播也算在内 → 临时退让被永久吸收

**严重度 medium · P1 · 置信度 0.75 · 工作量 S · 改后输出：变（只在有停顿的情形）**

证据（`GlazeController.cpp`）：

```cpp
// 2586-2589 endBlock（active && !limitedByManual 分支）
const auto guardDb = guard.getProtectionDb();
guardSteadySamples = sameBits (guardDb, lastGuardDb) ? guardSteadySamples + n : 0;   // n = 本块全部样本

// 1937-1942 updateFeedback：锁定后保护层「卡住」→ 吸收进方案（上限只降不升，随工程保存）
if (guard.isEngaged() && guardSteadySamples >= guardStuckSamples && calibrationApplicable()
    && executedGuardDb < -feedbackMinimumChangeDb)
    applyFeedback (0.0f);

// 1604  校准开轮的门也用它
const auto guardSettled = ! guard.isEngaged() || guardSteadySamples >= guardStuckSamples;
```

`GlazeGuard::process`（`GlazeGuard.cpp:181-182`）在 `!stats.musicActive` 时整块不计、`protection` 不动——所以静音 / 停播期间 `guardDb == lastGuardDb` 恒成立、`guardSteadySamples` 照加；`guardStuckSamples = guardRecoverAfterSeconds + 1 = 6 s`（943）。

机理：守护的恢复规则是「连续 ≥ 5 s **有效、安全的音乐**才恢复」（规格决策与校准 §7；`GlazeGuard.cpp:302-311` 用 `safeSlots` 只数音乐时隙）。「卡住」的判据本意是「在音乐时间里超过恢复等待仍没动 = 停在回差区、不会自己回来」。改用宿主样本时间后：g_保护 = −1 dB 时用户停播 6 s（或歌里有一段 6 s 以上的空拍 / 淡出淡入），就被判「卡住」→ `applyFeedback(0)` → 上限 = 方案净提升 − 1 dB，**随工程保存、只降不升**；而按规格音乐一回来 5 s 后守护本可恢复到 0。同一计时还让校准门 `guardSettled` 在停顿中打开：恢复播放后新一轮在守护介入下采集 → 10 s 预算用完 → 「预算受限」→ 又一次吸收。规格「静音、停播不计入恢复时间」的精神被绕过了。

修法：把 `guardSteadySamples` 挪到守护自己的时间轴——只在 `guardStats.musicActive` 为真的块累加：

```cpp
if (guardStats.musicActive)
    guardSteadySamples = sameBits (guardDb, lastGuardDb) ? guardSteadySamples + n : 0;
// 静音 / 停播：不加也不清
```

（`samplesSinceFeedback` 的冷却与 `samplesSinceMismatch` 保持宿主时间即可，它们只延后动作、不改变方案。）

验证：新用例（可复用 12c `testStuckGuardAbsorbed` 的 Rig）：×5 热素材让守护退到约 −0.9 dB 后，**在 6 s 内**送 8 s 静音（`rig.sourceGain = 0`，分析器 `musicActive` 为 false），再送回同一热素材：修前静音结束时 `probe.feedbackFromStuckGuard` 已加 1、`feedbackMaxGainDb` 已降；修后静音期间不吸收，恢复播放后才按音乐时间判定。现有 12c 用连续音乐，抓不到。

---

### R5-04 执行快照的「冻结方案」被丢弃时没有连续性：第一次碰旋钮 / 换风格净增益阶跃

**严重度 medium · P1 · 置信度 0.65 · 工作量 M · 改后输出：变（阶跃改为过渡）**

证据（`GlazeController.cpp`）：

```cpp
// 964-979  desiredSource：沿用执行快照时方案源 = 快照本身，useSavedPlan = true（冻结，不随 GLAZE / 手动值重算）
if (executionHeld) { … s.useSavedPlan = true; s.savedPlan = execution.plan; }

// 1398-1407 adoptExecution：把已执行的保护退让并进方案增量 / 目标（absorbed ≤ 0），其余进 heldBiasDb
const auto absorbed = juce::jmax (-execution.plan.increments.limitGainDb, execution.protectionDb);
execution.plan.increments.limitGainDb += absorbed; … heldBiasDb += execution.protectionDb - absorbed;
execution.protectionDb = 0.0f;

// 2326-2343 beginBlock：任一参考条件变了（GLAZE 差 0.1、任一手动旋钮、风格）→ 丢快照，直接落到新方案源
if (! GlazeCalibrator::referenceMatches (execution.reference, reference))
{ executionHeld = false; … snapTo (desiredSource()); }

// 2487-2510 captureExecution：快照存的是过渡**混合后**的增量与目标
state.plan.increments = increments; … state.plan.effectiveGainDb = mix (from.effectiveGainDb, …);

// 1347-1360 onHostReset：宿主 reset 也走同一套沿用（snapTo，正在进行的过渡被截断在中途）
```

机理：快照里的方案 = 保存那一刻听到的（含吸收的退让 `absorbed`、过渡走到一半的混合值）。沿用期间 `useSavedPlan` 让方案不随任何输入变（这是有意的），但**丢弃**时走 `snapTo`：新方案源按已提交画像重算，不含 `absorbed`（守护在 `consumeRestore` / `onHostReset` 里已 `reset()` 到 0），也不含过渡剩余量，于是交给限幅器的 Limit Gain 一步跳回「未退让、过渡终点」的值——只有模块自己 20 ms 的平滑。`absorbed` 最多 3 dB（`guardMinDb`）。对比 `lowerGainCap`（1813-1822）吸收退让时特意用 `guardCarry` 让净增益连续，这里是同一类量、没有同样的处理。`heldBiasDb` 那一半没有问题：`heldBiasKnown` 一清，`appliedBiasDb` 按 1 dB/s 滑向新目标（1658-1672）。

触发场景：热素材上守护常年在 −0.5 ～ −2 dB 之间动（BUG-003 的真歌门槛记录）；保存时正好非零 → 重开播放（听到的与保存时一致）→ 拧一下 GLAZE 或换风格 → 限幅器瞬间多吃 |absorbed| dB，几秒后守护再退回。宿主 reset（Logic / FL 某些操作、AU 旁通切换）沿用执行量的路径同理，且过渡中途 reset 会把方案永久留在混合值上直到条件变。

修法（`GlazeController.cpp`）：

1. `Impl` 加 `float absorbedGuardDb = 0`，`adoptExecution` 里记下 `absorbed`（非沿用时清 0）。
2. 丢快照的两处（2331-2342 与换风格路径）不再 `snapTo`，而是 `guardCarryStartDb = absorbedGuardDb; guardCarryElapsed = 0; guardCarryLength = planTransitionSamples;` 后 `requestSource (desiredSource(), planTransitionSamples)`：from 槽保留冻结的快照方案，to 槽按新条件重算，增量线性过渡 3 s，退让随 carry 淡出——与 `lowerGainCap` 同一套连续性。（换风格那一支本来就要过渡，只是起点应是快照方案而不是重算出来的旧风格方案。）
3. `onHostReset` 沿用执行量时若 `transitionActive`，不要 `snapTo` 截断：把 `pendingSource` / 剩余长度保留，或干脆只沿用 `heldBias` 与守护、不冻结方案（过渡本身不依赖检测器历史，reset 不需要冻结它）。

代价：第一次拧 GLAZE 时增量 3 s 内线性到位（to 端每块照旧按当前 GLAZE 重算，旋钮不失灵），相当于把「解除冻结」当成一次方案切换，符合时间线 §4「方案切换 = 约 3 s 整体过渡」。

验证：现有 12c 的 Rig 让守护退到 −0.9 dB 后 `createStateXml` → 新链 `restoreStateXml` → 播 2 s（沿用中）→ GLAZE 60 → 61：逐块记录 `effective.limit.gainDb`，修前一块内跳 ≈ 0.9 dB，修后每块变化 ≤ 1 dB/s × 块长；宿主 reset 例同理。

---

### R5-05 校准器的修正步长忽略限幅损失的斜率，E ≥ 4 dB 时两轮几乎必然 partial

**严重度 low · P2 · 置信度 0.6 · 工作量 M · 改后输出：变**

证据（`GlazeCalibrator.cpp`）：

```cpp
// 329-333 completeRound：下一轮建议 = 实际执行 + e（单位增益）
auto proposed = clampToLimit (applied + error);
// 168-177 finishConservative：最后一轮仍未收敛 → applied + min(0, e)（向上不再推）
```

机理：Limit Gain 到出口响度的传递不是 1：整链净提升 net(G) = G − loss(E0 + G)，按标定快照自己的模型 loss = 0.04·E²（`GlazeConstants.h:203-204`【实测】），d net / dG = 1 − 0.08·E。E = 6 dB 时斜率 0.52：施加 e 只换来 0.52e 的净提升，残差 0.48e；两轮、容差 0.25 dB 下 e ≥ 0.6 dB 就结束为 partial，且 `finishConservative` 向上只保留 `applied`，等于系统性少修 (1 − s)·e。第 3 节实验 2：E0 = 4 / 6 / 8 dB、前馈误差 0.8 dB 时单位步残差 0.28 / 0.41 / 0.54 dB（全部 partial）；按斜率缩放后 0.06 / 0.09 / 0.20 dB（valid）。

修法：`begin()` 多传一个本方案的 `estimatedExcessDb`（`Plan::estimatedExcessDb`，编排器手边就有）与 `limitLossCoeff / Exponent`（来自 `committed.constants`），`completeRound` 里 `slope = clamp(1 − coeff·expo·max(0, E0 + applied)^(expo−1), 0.35, 1)`，`proposed = clampToLimit (applied + error / slope)`；`blockedUp / blockedDown` 与 `clampToLimit` 照旧兜底。斜率下限 0.35 防止模型外推时步子过大。

反驳点与回答：「向上少修是有意保守、守护会来打架」——是，但少修的量不受控（随 E 变），且向下（e < 0，太响）同样少修（实验里 −0.8 dB → 残差 −0.36 partial）；斜率缩放两个方向都更准，守护的约束仍由 `guardActive` 预算与 `blockedUp` 管。

验证：`GlazeCalibrationTests` 的「converges to a known chain deviation」用的是线性偏差链；加一例用 `net(G) = G − 0.04·max(0, E0+G)²` 的模型链（E0 = 6，δ = 0.8）：修前 partial、残差 ≈ 0.41，修后 valid。

---

### R5-06 热路径每块重复算同一批纯函数

**严重度 low · P2 · 置信度 0.7 · 工作量 S · 改后输出：不变（同输入同输出，逐位）**

证据（`GlazeController.cpp`，GLAZE > 0 的每块）：

| 纯函数 | 每块次数 | 位置 |
|---|---|---|
| `blendConstants (constants, weights)`（20 项 ×3 乘加，独热时早退） | 3 | 2075（updateLiveTone）、2117（computeEffective Tame）、2137（computeEffective Tone） |
| `confidenceScale (&committed.profile, c)` | 4～5 | 2076、2138、1516（`correctionLimitDb`，被 `biasTarget` 1649 / 1653 各调一次，`updateBias`→`biasTarget` 再来一遍） |
| `matchingLiveSlopes`（27 段加权回归） | 2 | 2016（updateLiveColor）、2081（updateLiveTone） |
| `calibrationApplicable()` → `referenceMatches`（~15 项比较） | 最多 6 | 1614/1616（诊断掩码）、1621（门）、1645 / 1647（biasTarget）、1937（updateFeedback）、2642（状态） |
| `weightsFor` + `dominantStyle` | 1 | 2665 |

每项几十 ns，合计约 0.3 ～ 0.5 µs / 块。T-027 用 296 ns / 块换到了 192 k / 32 下 0.18 个百分点（`docs/validation/2026-09-29-T027-reference-cache/REPORT.md` 第 4 节），这里量级相同或更大；48 k / 512 下可忽略。

修法：`endBlock` 开头（`advanceTransition` 之后）算一次 `BlockCache { StyleConstants caps; float kappa; float strength; bool applicable; bool slopesOk; float refSlope, curSlope; }`，`caps` 按（`toSlot().source.generation`, `toSlot().evaluations`）缓存（与 R5-02 同一把键）；`kappa` 只随 `committed.profileId` / 常数变。各处改读缓存。全部是纯函数、输入相同，结果逐位相同（`blendConstants` 独热早退的逐位保证不受影响）。

验证：现有 T-027 用例的思路——逐块比缓存值与现算值逐位相同；全套逐位测试不变。

---

### R5-07 `publishState` 每块整份拷贝 5312 B、`publishStatus` 每块 seqlock，100 ms 节流脚手架未接线

**严重度 low · P2 · 置信度 0.7 · 工作量 M · 改后输出：音频不变；存档快照最多晚 100 ms（注释本来就以此为目标）**

证据：

```cpp
// GlazeController.cpp:100    constexpr double statePublishSeconds = 0.1;
// 925-926                     bool stateDirtyUrgent, stateDirtyLazy;  std::int64_t samplesSincePublish;
// 2701-2730 publishState：每块 tryEnter → published.committed = committed; published.execution = lastExecution; …
//           stateDirtyUrgent = stateDirtyLazy = false; samplesSincePublish = 0;   // 标志被写、被清，从未被读作条件
// 2696-2699 publishStatus：每块 statusSnapshot.write (makeStatusPayload());
```

sizeof（107，g++ 15.2，抽取同一份结构体定义）：`MusicProfile 1280`、`Constants 1168`、`CommittedState 2632`、`ExecutionState 2672`、**`SaveState 5312`**、`StatusPayload 68`（17 个 32 位原子写）。192 k / 32 下每秒 6000 块 → 每秒 32 MB 拷贝、约 0.2 ～ 0.3 个百分点（T-027 整链实测 22.7 µs / 块 = 13.6 %，5.3 KB memcpy ≈ 0.3 ～ 0.5 µs）；48 k / 512 下约 0.004 %。

修法（两步，都不碰音频）：

1. 接上已有脚手架：`publishState` 只在 `stateDirtyUrgent || samplesSincePublish >= statePublishSamples` 时发布（100 ms），`publishStatus` 同样 100 ms 一次（界面 30 ms 读一次，读到的最多是 100 ms 前的状态；`restored` / 提交 / 校准结束等已置 `stateDirtyUrgent`，立即发布）。
2. 进一步：`ExecutionState` 里 `profile`（1280 B）与 `constants`（1168 B）只随方案源变，按 `generation` 只在变化时拷贝，每块只拷约 200 B 的执行量。

注意：`prepare()` 同规格重建沿用 `published.execution`（2880-2899），节流后它最多晚 100 ms（偏差按 1 dB/s 最多差 0.1 dB）；用例 1636「resumes a completed correction from the last heard bias」与 T-027 用例 4344（`publishedChecks >= probeChecks / 2`）要按新节奏改断言。

---

### R5-08 读档与提交两条路径的运行态初始化不对称

**严重度 low · P2 · 置信度 0.7 · 工作量 S · 改后输出：仅这两种情形变**

证据：

- `consumeRestore`（2734-2782）清了 calibrator / guard / monitor / carry / 过渡，却没有 `clearMismatch()`；`onHostReset`（1381）、`onTransportJump`（1330）、`prepare`（2912）都清。读档前累计的 `mismatchSamples / mismatchEnergy` 会与读回的新画像比较（1857-1874），播放中读档（Ableton Hot-Swap、预设切换）时可能在第一块就以「上一份画像下累计的能量」给新方案降上限。
- `commitCandidate`（1219-1250）把 `committed.constants = constants`，但 `guardStuckSamples`（943，取 `activeConstants().guardRecoverAfterSeconds`）与 `monitor.configure (sampleRate, activeConstants())`（2780 / 2927）只在读档 / prepare 时折算；旧工程（Codex 第 1 ～ 5 轮存的快照版本 1 ～ 6）读回后点「重新聆听」提交新画像，上游监视窗与「卡住」门槛仍按旧快照的时长。

修法：`consumeRestore` 加 `clearMismatch(); samplesSinceMismatch = mismatchCooldownSamples;`；`commitCandidate` 末尾加 `updateTiming(); monitor.configure (sampleRate, activeConstants());`（都是几个乘法，音频线程可调）。

验证：单元级——播放中 `restoreStateXml` 一份 I 低 12 LU 的画像，第一块 `probe.mismatchSteps` 不该加 1；`setConstantsForTesting` 给不同 `guardWindowSeconds` 的旧快照读回再重学，`monitor` 窗长应回到出厂值（需在探针暴露 `monitor` 窗长）。

---

### R5-09 `unlearnedOfflineWarning` 从未置真：界面与规格的「未学习，导出使用保守方案」是死文案

**严重度 low · P2 · 置信度 0.8 · 工作量 S · 改后输出：不变**

证据：`GlazeController.cpp:911` 声明；1244、2393、2769 只写 false；2668 `s.unlearnedOfflineWarning = unlearnedOfflineWarning && …` 恒 false。界面 `GlazeStatusText.cpp:185` 的 `Kind::unlearnedExported` 分支永远不走。规格 `设计规格-GLAZE-时间线与保存.md` §7 与 `-界面与实现.md` §1 状态行表第一行仍写着这句提示；Codex 7.3 已把离线改成「同实时一样继续学习」，所以「导出使用保守方案」这句话本身也不再成立。

修法（二选一，建议前者）：① 删字段、删 `Kind::unlearnedExported` 与 HelpText 文案，规格两处改成「离线导出照常学习 / 校准」；② 若仍想提示，在 `beginBlock` 里 `unlearnedOfflineWarning = offline && ! committed.hasProfile && active` 并在状态行写「未学习 · 导出中边听边推」。

---

### R5-10 结构：3330 行单文件、约 2100 行 / 约 130 成员的 `Impl`

**严重度 low · P2 · 置信度 0.8 · 工作量 L · 改后输出：不变**

现状：`GlazeController.cpp` 里 `Impl`（749 ～ 2849）同时承担 7 种职责，成员约 130 个；文件头注释 60 行、`namespace {}` 里还塞着与编排无关的三块可独立单元。每次改一处都要重读整份文件，T-015 / T-026 / T-027 的改动都印证了这一点。可分段落地（每段单独编译、全套逐位不变）：

| 拆出 | 现在的位置 | 新文件 | 备注 |
|---|---|---|---|
| 参考条件哈希、两种口径、旧存档升格（`Fnv1a`、`emitConditions`、`HashSink`、`WordRecorder`、`candidateCodes`、`searchLegacyCodes`、`setManualConditions`、`upgradeLegacyConditions`、`samePlanningManual`） | 158 ～ 466、3242 ～ 3329 | `GlazeReference.{h,cpp}` | 纯函数，已有独立测试（T-026 / T-027） |
| 单写者 seqlock、`StatusPayload`、`statusForRestored`、`overlayPendingRelearn` | 514 ～ 577、2633 ～ 2699、2784 ～ 2848 | `GlazeStatusSnapshot.h` | 模板 + 两个静态函数 |
| `UpstreamMonitor` | 579 ～ 692 | `GlazeUpstreamMonitor.h` | 与 `GlazeGuard` 同型的时隙统计，可与其共用 10 ms 时隙实现 |
| 实时音色 / 密度跟随（`matchingLiveSlopes`、`updateLiveTone`、`updateLiveColor` 与 `liveTone* / liveHeat* / liveGlue*` 状态） | 1963 ～ 2091 | `GlazeLiveColor.{h,cpp}` | 输入：视图、画像、方案、常数；输出：三个偏移量 |
| 修坯 Clay 接线（`plannedClay`、`updateClay`、`syncClayConstants`、压力反馈缩放、`blendClayPlans`、`sanitizeClay*`） | 468 ～ 512、1723 ～ 1766、2186 ～ 2302、3189 ～ 3238 | `GlazeClayBridge.{h,cpp}` | 已有独立测试 |
| 画像失配（`updateMismatch` 与 `mismatch*` 状态） | 1827 ～ 1909 | `GlazeMismatch.h` | 小状态机 |

剩下的 `Impl` 只管方案源 / 过渡 / 状态机 / 校准编排 / 存档，约 900 行。顺序建议：先拆纯函数两块（零风险），再拆 Clay 与 live color。

---

## 2. 实验数据（107，2026-09-29 晚；目录已删）

### 实验 1（否定）：校准器子块配对的错位

`GlazeCalibrator::matchPairs`（202-228）把入口子块 [kL−L, kL) 配给「结束时刻最接近 kL + latency」的出口子块；两块表共用网格，latency（48 k 1384、96 k 2584、192 k 4984）都小于 L/2，配到的就是 [kL−L, kL)，比真正对应的出口早 δ = latency（26 ～ 29 ms）。
合成 240 s 类音乐（120 BPM 底鼓 / 军鼓、55 Hz 贝斯、粉噪 pad、每 8 s 随机 −6 ～ +3 dB 段落电平），纯延迟链（真误差 0），48 k：

| 窗 | 实现配对 |e| p50 / p95 / p99 / max（dB） | 按小数部分线性插值两个相邻出口子块 p50 / p95 / max |
|---|---|---|
| 2.5 s 子窗 | 0.019 / 0.066 / 0.103 / 0.192 | 0.010 / 0.056 / 0.349 |
| 10 s 整轮 | 0.004 / 0.020 / 0.032 / 0.065 | 0.003 / 0.025 / 0.086 |

91 轮里 0 轮被误判「方向相反」（±0.5 dB）；|子窗误差| > 0.25 dB 占 0 %。结论：错位误差远小于 0.25 dB 的收敛容差，插值反而把 max 拉大（能量在子块内非均匀）。**不报**。

### 实验 2（支持 R5-05）：单位步 vs 斜率步

模型 net(G) = G − 0.04·max(0, E0+G)²（`GlazeConstants.h:203-204`），两轮、容差 0.25、修正范围 3 dB；δ = 模型比真实少算的损失（= 纯前馈误差）。

| E0 | δ | 单位步（现状） | 残差 | 斜率步 | 残差 | 斜率 |
|---|---|---|---|---|---|---|
| 2 | 0.8 | valid | 0.15 | valid | 0.04 | 0.84 |
| 4 | 0.8 | **partial** | 0.28 | valid | 0.06 | 0.68 |
| 4 | 1.2 | **partial** | 0.44 | valid | 0.12 | 0.68 |
| 6 | 0.8 | **partial** | 0.41 | valid | 0.09 | 0.52 |
| 6 | 1.2 | **partial** | 0.63 | valid | 0.21 | 0.52 |
| 6 | −0.8 | **partial** | −0.36 | valid | 0.09 | 0.52 |
| 8 | 0.8 | **partial** | 0.54 | valid | 0.20 | 0.36 |
| 8 | 1.2 | **partial** | 0.83 | partial | 0.48 | 0.36 |

### 实验 3（否定）：校准器只用 −70 LUFS 绝对门的偏置

能量池化下，响段占时长 p、静段低 d LU、响段限幅损失 loss，测得净提升相对「只看响段」的偏置（dB）：

| d（LU） | loss 1 / 2 / 3 dB（p = 0.5） | p = 0.7 |
|---|---|---|
| 6 | 0.22 / 0.48 / 0.79 | 0.11 / 0.24 / 0.40 |
| 10 | 0.10 / 0.22 / 0.38 | 0.05 / 0.10 / 0.17 |
| 15 | 0.03 / 0.08 / 0.13 | 0.02 / 0.03 / 0.06 |
| 20 | 0.01 / 0.03 / 0.04 | 0.00 / 0.01 / 0.02 |

分析器的「有效音乐」门是 I − 20 LU（`MusicAnalyzer.cpp:1027-1029`），它会排除的只有 d > 20 的段，偏置 ≤ 0.04 dB；d = 6 ～ 10 的主歌 / 副歌差是音乐本身，任何门都不该排除。**不报**。

### 实验 4（支持 R5-07）：结构体大小

g++ 15.2、x86-64：`MusicProfile 1280`、`Constants 1168`、`Plan 92`、`CalibrationReference 56`、`CommittedState 2632`、`ExecutionState 2672`、`SaveState 5312`、`PlanSource 2584`、`Status 56`、`StatusPayload 68`（17 词）。

## 3. 核对过、判定不成立或不报的项

- 子块配对错位（实验 1）、绝对门偏置（实验 3）：见上。
- seqlock 写者 `store(s+1, relaxed); fence(release); 数据 relaxed; store(s+2, release)`、读者 `load(acquire); 数据; fence(acquire); load(relaxed)`（522-560）：是标准的正确模式，release 栅栏阻止 s+1 之后的写重排到它前面。
- 参考条件容差：GLAZE（步 0.1）、Ceiling（0.1）、Lookahead（0.5）都吸附（`ParameterRanges.h:38-58`），`close()` 的 1e-4 绝对容差够用；三个对数刻度值已按 ADR-018 处理。
- `computeEffective` 的守护 / 偏差钳位（2163-2183）：实际 Limit Gain 永不低于手动值、守护只收自动份额，与规格决策 §8 / 实现修订 §3 一致。
- `lowerGainCap` 的 `guardCarry`（1813-1822）：净增益连续性推演成立（old + g·t + g·(1 − t) = old + g）。
- BUG-003 修复（`keepCalibration`，1792-1804）与 BUG-004 修复（升格只在读档后第一块）逻辑自洽。
- 采样率改变：分析器重建、候选作废、参考条件含采样率 → 待校准；执行快照与 heldBias 在第一块因采样率不一致被丢（2326-2343、2444-2448），与规格一致。
- 已登记待认领、不重报：修坯性能预算、55 Hz 单块尖峰、96k/32 最大值、NEON 差异、界面主线程 21 %、起音密度、编码余量、AU 版本号。

## 4. 建议的落地顺序（当晚）

1. R5-01（1 处挪两行 + 1 行）→ R5-02（一行改键）→ R5-03（一个 if）：三条都是 S，一起上，一套新增用例（3 个）覆盖。
2. R5-04：M，需要 `absorbedGuardDb` 与两处 `snapTo` 改过渡，单独一 PR。
3. R5-06 / R5-08 / R5-09：S，可顺手；R5-05、R5-07 各自单独评估（R5-05 改校准结果，要过一遍真歌门槛；R5-07 要改两处测试断言）。
4. R5-10 留给下一轮。
