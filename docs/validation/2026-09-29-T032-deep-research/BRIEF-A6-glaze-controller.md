# T-032 实现任务书 · A6-glaze-controller · GLAZE 控制 / 校准 / 守护

> 生成于 2026-09-29 23:0x，来源：研究代理 R*.md 与核验代理 V*.md（同目录）。逐条以「核验调整后的修法」为准。

## 你负责的文件（只许改这些）

Source/dsp/GlazeController.{h,cpp}、Source/dsp/GlazeGuard.{h,cpp}、Source/dsp/GlazeCalibrator.{h,cpp}；tests/GlazeControllerTests.cpp、tests/GlazeCalibrationTests.cpp；档案 01-框架/设计规格-GLAZE-决策与校准.md（末尾加「2026-09-29 修订（T-032）」小节）。**GlazePlanner / GlazeTypes / GlazeProfile / GlazeConstants 与 GlazeController.cpp 里 R6-02 的七处门（1588 / 1621 / 1633 / 1845 / 1922 / 2160 / 2576）归 A7，你不要碰这七处**。

## 主进程补充要求

- R5-01 只做研究修法①（记录挪到 finishRound、仅 !committed.calibration.valid 时记 roundReference），顺带堵住 requestPlanTransition 换风格往返那条（核验 M1）；用例三例（GLAZE 0 往返、Color Bypass 往返、校准中换风格往返）。
- R5-02 用全局单调计数 evaluationCounter；探针加 guardLimits。
- R5-03 与核验 M2 一起改：guardSteadySamples 只在 guardStats.musicActive 时累加（静音不加也不清）；约束反馈观察期同样只在 musicActive 时累加（或过渡结束处 monitor.clear()），用例复用 12c Rig 送 8 s 静音。
- R5-04 按核验的调整修法：只 carry 吸收量（Impl 加 absorbedGuardDb，adoptExecution 记下、非沿用时清 0；解除冻结两处保持 snapTo 并追加 guardCarryStartDb / Elapsed / Length，复用 lowerGainCap 的 carry 机制）；③ 不动。
- R5-06 只做同一阶段内（beginBlock 内 / endBlock 内）的去重：同输入的纯函数在同一次调用里只算一次并复用；**不做跨块缓存键**（核验指出键漏一项就破逐位）。加「逐块比去重前后逐位相同」思路的用例或用现有全套 + 摘要日志证明。
- R6-04：liveTame 乘 confidenceScale(committed.hasProfile ? &committed.profile : nullptr, snapshot)，2110 行条件加 committed.hasProfile；先核 GlazeControllerTests.cpp:2315-2380 的 +2.0 余量。
- 核验漏项 M3（refreshPlans 在沿用执行快照时把吸收过退让的执行方案并进正式版本）：不改，报告里列「待 Max」。

## 逐条结论

### R5-01 · 校准轮因条件临时变化被放弃后，条件变回原样就永远不再开轮（「上次尝试」门把放弃当成失败）
- 严重度 / 优先级：high / P0 → 核验 high / P0；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:1575`
- 证据：beginRound 1540-1541：lastAttemptReference = currentReference; haveLastAttempt = true。updateCalibration 1575-1581 放弃分支：calibrator.reset(); roundActive = false; committedPhase = locked（不清 haveLastAttempt）。开轮门 1620-1623：&& !(haveLastAttempt && referenceMatches(lastAttemptReference, currentReference))。haveLastAttempt 只在 1357 / 2763 / 2944（reset 沿用执行量、读档、prepare）被清。GLAZE ≤ 0 时 2440 referenceKnown = false → 轮中 1572 matches = false → 3 s 后放弃；Balance/Color/Limit Bypass 在 emitConditions 289/295/300 的哈希里。
- 机理：门 512 的本意是 begin() 目标非有限立即结束为无效时不每块重开；但放弃分支共用同一份 lastAttemptReference。校准中把 GLAZE 拧到 0 做 A/B ≥ 3 s 再拧回、或切模块 Bypass 对比 ≥ 3 s 再切回，currentReference 与 lastAttemptReference 逐位相同 → 门永远为真 → calibrationApplicable 为 false、phase locked，状态行写「已锁定 · 待校准（条件已变，稳定后自动校准）」（GlazeStatusText.cpp:140, 271）却永远不校准，偏差保持 0，直到用户改别的条件。
- 研究修法：① 把「记下上次尝试」从 beginRound 挪到 finishRound，只在 !committed.calibration.valid 时记：if (!committed.calibration.valid) { lastAttemptReference = roundReference; haveLastAttempt = true; }（保住 begin() 非有限目标时的防循环）；② 放弃分支（1575-1581）加 haveLastAttempt = false。
- 核验理由：代码逐行核实：beginRound 1540-1541 记「上次尝试」，放弃分支 1575-1581 不清 haveLastAttempt，开轮门 1623 按 referenceMatches 挡，haveLastAttempt 只在 1357/2763/2944 清；GLAZE 0 时 2440 referenceKnown=false 而 updateCalibration 在 endBlock 2606 无条件跑，3 s 后真会放弃；拧回后参考逐位相同、只剩 512 位挡门，状态行 GlazeStatusText 140/271 承诺「稳定后自动校准」却永不发生。512 门的本意只覆盖 begin() 目标非有限那一种失败（finish() 其余路径都 valid=true）。另发现研究漏了第二条同根因路径：requestPlanTransition 1061-1065 丢轮也不清，校准中换风格再切回（2357 上限回不限）同样永远不开。
- **核验调整后的修法（以此为准）**：只做研究的修法①：把记录挪到 finishRound、仅 !committed.calibration.valid 时记 roundReference（同时堵住换风格往返那条）；②单独在放弃分支清 haveLastAttempt 堵不住换风格路径，不能单独用（可留作保险）。用例除研究的 GLAZE 0 / Color Bypass 两例外，再加一例「校准中切风格 1 跑满过渡再切回 0 → calibrationBegins 加 1、最终 locked 且 calibrationValid」。
- 风险：输出只在「放弃后条件回到原样」场景改变（原永不校准→校准），GLAZE 0 逐位契约不涉及；用例 931、12d、4301 都不走放弃分支。不会形成开→放弃→开紧循环（放弃需 3 s 不一致、重开需 1 s 精确稳定）。107 全套重跑确认。
- 验证：新用例：Rig 学完 → GLAZE 60 开轮进入 calibrating → GLAZE 0 跑 4 s（> calibrationAbandonSeconds）→ 拧回 60 跑 3 s：断言 probe.calibrationBegins 加 1、最终 phase == locked && status.calibrationValid；第二例用 Color Bypass on 4 s / off。修前两例停在 calibrationGateMask & 512。现有用例 931 只覆盖「先校准完再回 0」。

### R5-02 · 守护 / 上游监视的约束上限按「方案槽自己的重算计数」去重，两槽计数每隔一次方案切换撞一次 → 换风格 / 提交后守护沿用旧方案的上限
- 严重度 / 优先级：medium / P0 → 核验 medium / P0；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:1681`
- 证据：PlanSlot 718：std::uint32_t evaluations（每槽各自计数）；loadSlot 1007-1011 不清它；startTransition 1023 toIndex = 1 - toIndex 两槽轮换；evaluate 1104 ++slot.evaluations；updateGuardLimits 1681-1688：if (guardLimitsKnown && guardLimitsEvaluation == slot.evaluations) return; … guard.setLimits(caps, …); monitor.setCaps(caps); guardLimitsEvaluation = slot.evaluations。guardLimitsKnown 只在 2773 / 2924 / 3127 清。对照 syncClayConstants 2247 用全局递增的 source.generation。
- 机理：第一块槽 0 重算 1 次 → 守护记 1；每次方案切换换到另一槽并重算 1 次：槽 1 计数 0→1，1 == 1 跳过；再切回槽 0 2 ≠ 1 更新（记 2）；再切槽 1 2 == 2 跳过……两槽计数始终相差 ≤ 1，每隔一次切换守护与上游监视都沿用上一份方案的 blendConstants 上限，直到用户碰任一进方案的旋钮。caps 只随权重变，所以换风格 / 重学提交受害最大：温润→通透后一半几率仍按温润 limit 1.0/2.5/4.0 dB（通透 1.5/3.5/6.0，GlazeConstants.cpp:61-63,105-107）过早退让最多 3 dB；反向则按通透的 Glue 上限过早触发 monitor.isOver() 多降 1 dB 方案。
- 研究修法：把去重键改成方案源代 + 重算次数：Impl 加 std::uint32_t guardLimitsGeneration，判 guardLimitsKnown && guardLimitsGeneration == slot.source.generation && guardLimitsEvaluation == slot.evaluations，更新时两者都记；或把 evaluations 改成全局单调计数（Impl::evaluationCounter，evaluate 里 slot.evaluations = ++evaluationCounter）。
- 核验理由：PlanSlot::evaluations 718 每槽各自、loadSlot 1007 不清、startTransition 1023 翻槽、evaluate 1104 自增、updateGuardLimits 1681 只比单一 guardLimitsEvaluation；evaluations 全工程仅这 4 处引用。逐块推演复核：首块槽 0=1 记 1，首个候选到槽 1=1 跳过，再回槽 0=2 更新……不碰旋钮时隔次沿用旧 caps，首次聆听（权重随候选变）、提交后、换风格后各一半几率受害。GlazeGuard::reset（105-113）不清 limitsSet，所以不会升级成守护整个关掉；lowerGainCap 翻槽但权重不变、caps 相同，无害。
- **核验调整后的修法（以此为准）**：采纳，推荐全局单调计数：Impl 加 evaluationCounter，evaluate 里 slot.evaluations = ++evaluationCounter（一行，去重键语义直接）；探针加 guardLimits 按研究的用例验证。
- 风险：守护介入且经历首次聆听/提交/换风格的场景音频会变（属修正）；2885、3755 换风格用例是零分配计数，逻辑不受影响；12a～12d 单风格反馈翻槽 caps 不变。全套 132981 项必须在 107 重跑逐条看变化断言。
- 验证：TestProbe 加 guardLimits（守护生效的 StyleConstants）；用例：学完锁定（自动）→ 切温润跑满过渡 → 断言 guardLimits == blendConstants(committed.constants, weightsFor(warm)) → 再切通透再比；修前第二次切换必不等。行为断言：温润→通透后 ×5 热素材，通透 limitGrPeakCapDb 6.0 下守护不该退让，修前按 4.0 会退。

### R5-03 · 「保护层卡住」按宿主样本时间计，静音 / 停播也算 → 音乐停 6 s 就把临时退让永久并进方案上限（只降不升、随工程保存）
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:2588`
- 证据：endBlock 2586-2589：guardSteadySamples = sameBits(guardDb, lastGuardDb) ? guardSteadySamples + n : 0（n = 本块全部样本，不看 musicActive）；GlazeGuard.cpp:181-182 !stats.musicActive 时整块不计、protection 不变 → 静音期间恒「没变」；updateFeedback 1937-1942：guard.isEngaged() && guardSteadySamples >= guardStuckSamples && calibrationApplicable() && executedGuardDb < -0.01 → applyFeedback(0)；guardStuckSamples 943 = guardRecoverAfterSeconds + 1 = 6 s；校准门 1604 guardSettled 同用它。规格决策与校准 §7「静音、停播不计入恢复时间」。
- 机理：守护恢复只数音乐时隙（GlazeGuard.cpp:302-311 safeSlots），「卡住」本意是音乐时间里超过恢复等待仍不动。改用宿主时间后，g_保护 = −1 dB 时停播 6 s 或歌里 6 s 空拍就判「卡住」→ 上限 = 方案净提升 − 1 dB 永久生效；音乐一回来本可 5 s 后恢复到 0。同时 guardSettled 在停顿中打开校准轮，恢复播放后在守护介入下采集 → 预算受限 → 再吸收一次。
- 研究修法：只在 guardStats.musicActive 为真的块累加：if (guardStats.musicActive) guardSteadySamples = sameBits(guardDb, lastGuardDb) ? guardSteadySamples + n : 0;（静音 / 停播不加也不清）。samplesSinceFeedback / samplesSinceMismatch 保持宿主时间即可。
- 核验理由：endBlock 2576-2593 分支只看 active && !limitedByManual，2588 按整块 n 累加不看 musicActive；GlazeGuard::process 181-182 !musicActive 整块早退、protection 不动 → 静音/停播/paused/Color 或 Limit Bypass（2581 四个条件）期间恒「没变」；updateFeedback 1937-1942 与校准门 1604 都用它，guardStuckSamples = 6 s（943）；updateFeedback 本身也无 musicActive 门，停播中照样 lowerGainCap。规格决策与校准 §7「静音、停播不计入恢复时间」，实现修订 §3 的「卡住」以「它自己的恢复等待 + 1 s」为门槛，而恢复等待按 GlazeGuard 302-311 只走音乐时隙，宿主时间计数去比音乐时间门槛不自洽。Limit Bypass 6 s 也会把冻结的 −1 dB 永久吸收（computeEffective 2173 照读 protection）。研究关于校准门在停顿中放开→预算受限的第二段推演偏弱（guardSamples 只累计门限以上入口子块，不必然用完预算），主结论不依赖它。
- **核验调整后的修法（以此为准）**：采纳：if (guardStats.musicActive) guardSteadySamples = sameBits(...) ? +n : 0;（静音不加也不清，lastGuardDb 照旧每块更新），四种冻结情形一并覆盖。可与漏项 M2（反馈观察期同样按宿主时间）一起改。
- 风险：只在有停顿/旁通场景改变输出；12c/12d 连续合成素材 view.musicActive 恒真不受影响；含静音间隙的用例需重跑核对。
- 验证：复用 12c testStuckGuardAbsorbed 的 Rig：×5 热素材让守护退到约 −0.9 dB 后 6 s 内送 8 s 静音（sourceGain = 0），再送回热素材：修前静音结束时 probe.feedbackFromStuckGuard 已加 1、feedbackMaxGainDb 已降；修后静音期间不吸收。现有 12c 用连续音乐抓不到。

### R5-04 · 执行快照解除冻结时没有连续性：重开 / 宿主 reset 后第一次碰旋钮或换风格，吸收进快照的保护退让与过渡中途值被直接丢掉，净增益阶跃最多 3 dB
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 M；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:2331`
- 证据：desiredSource 969-979：executionHeld 时 useSavedPlan = true、savedPlan = execution.plan（冻结）；adoptExecution 1398-1407：absorbed = max(-increments.limitGainDb, protectionDb) 并进 plan.increments.limitGainDb / effectiveGainDb，protectionDb 清 0；beginBlock 2326-2343：任一参考条件不一致（GLAZE 差 0.1、任一手动值、风格）→ executionHeld = false … snapTo(desiredSource())；captureExecution 2498-2510 存的是过渡混合后的增量与目标；onHostReset 1347-1360 沿用执行量也 snapTo。对照 lowerGainCap 1813-1822 吸收退让时用 guardCarry 保持净增益连续。
- 机理：快照方案 = 保存 / reset 那一刻听到的（含 absorbed ≤ 3 dB 的退让、过渡走到一半的混合值）。丢弃时新方案按已提交画像重算，不含 absorbed（守护在 consumeRestore / onHostReset 已 reset 到 0）也不含过渡剩余量，Limit Gain 一步跳回，只有模块 20 ms 平滑；heldBiasDb 那一半按 1 dB/s 滑、没问题。热素材上守护常年非零，保存时非零 → 重开播放一致 → 拧一下 GLAZE 或换风格 → 限幅器瞬间多吃 |absorbed| dB，几秒后守护再退回。换风格路径过渡起点是重算出的旧风格方案而非快照方案，同样阶跃。
- 研究修法：① Impl 加 absorbedGuardDb，adoptExecution 记下 absorbed（非沿用时清 0）；② 丢快照的两处（2331-2342 与换风格）改为 guardCarryStartDb = absorbedGuardDb; guardCarryElapsed = 0; guardCarryLength = planTransitionSamples; requestSource(desiredSource(), planTransitionSamples)：from 槽保留冻结方案、to 槽按新条件每块重算、增量 3 s 线性过渡、退让随 carry 淡出（与 lowerGainCap 同一套）；③ onHostReset 沿用执行量时若 transitionActive 不 snapTo 截断（保留 pendingSource / 剩余长度，或只沿用 heldBias 与守护、不冻结方案）。
- 核验理由：机理逐行核实：adoptExecution 1398-1407 把退让并进冻结方案、protectionDb 清 0；desiredSource 969-979 useSavedPlan 冻结；beginBlock 2326-2343 参考一变 snapTo 到按已提交画像重算的新方案（不含 absorbed）；consumeRestore 2764 / onHostReset 1369 守护已 reset 到 0；换风格路径先 snapTo 到旧风格重算方案（currentStyle 未变）再 requestPlanTransition，过渡起点不是快照方案；captureExecution 2500-2510 存混合值。对照 lowerGainCap 1816-1822 有 guardCarry，这里没有。阶跃属实、上限 guardMinDb 3 dB。但研究修法②要在解除冻结时开 3 s 方案过渡，违反时间线与保存 §4「拧 GLAZE…不重做方案过渡」（Max 09-25 设计确认）；③ onHostReset 过渡中不截断是设计取舍且研究自己置信度低。
- **核验调整后的修法（以此为准）**：只 carry 吸收量、不改路径语义：Impl 加 absorbedGuardDb（adoptExecution 记 absorbed，非沿用时清 0）；解除冻结两处（2331-2342 与换风格支）保持 snapTo，追加 guardCarryStartDb = absorbedGuardDb; guardCarryElapsed = 0; guardCarryLength = planTransitionSamples;（复用 lowerGainCap 1816-1818 与 computeEffective 2173 的 carry 机制）。净增益在吸收量上连续、GLAZE 旋钮仍即时；过渡中途保存的混合值差仍是阶跃（少见，接受）；③ 不动。验证按研究的 12c 变体（GLAZE 60→61 逐块 limit.gainDb 变化 ≤ 1 dB/s×块长）再加换风格一例。
- 风险：改动集中在两处解除冻结与 adoptExecution，复用现有 carry 路径；重开/reset 后第一次拧旋钮或换风格的输出会变（属修正），其余场景不变。需核对 12b 那类读档→守护用例的断言。
- 验证：12c 的 Rig 让守护退到 −0.9 dB 后 createStateXml → 新链 restoreStateXml → 播 2 s → GLAZE 60→61：逐块记录 effective.limit.gainDb，修前一块内跳 ≈ 0.9 dB，修后每块变化 ≤ 1 dB/s × 块长；宿主 reset 例同理。

### R5-08 · 读档与提交两条路径的运行态初始化不对称：consumeRestore 不清画像失配累计；commitCandidate 换了 committed.constants 却不重折算守护「卡住」门槛与上游监视窗
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:2734`
- 证据：consumeRestore 2734-2782 清了 calibrator/guard/monitor/carry/过渡，无 clearMismatch()；onHostReset 1381、onTransportJump 1330、prepare 2912 都清。updateMismatch 1857-1874 用累计的 mismatchSamples/mismatchEnergy 与新画像 I 比较。commitCandidate 1219-1250 设 committed.constants = constants，但 guardStuckSamples（943 取 activeConstants().guardRecoverAfterSeconds）与 monitor.configure(sampleRate, activeConstants())（2780/2927）只在读档 / prepare 折算。
- 机理：播放中读档（Hot-Swap、预设切换）时上一份画像下累计的能量可能在第一块就给新方案降上限；旧工程（快照版本 1～6）读回后点重新聆听提交新画像，上游监视窗与卡住门槛仍按旧快照时长。
- 研究修法：consumeRestore 加 clearMismatch(); samplesSinceMismatch = mismatchCooldownSamples;；commitCandidate 末尾加 updateTiming(); monitor.configure(sampleRate, activeConstants());（几个乘法，音频线程可调）。
- 核验理由：consumeRestore 2734-2782 清 calibrator/guard/monitor/carry/过渡/samplesSinceFeedback 但无 clearMismatch()，onHostReset 1381、onTransportJump 1330、onLoopWrap 1339、prepare 2912 都清，属实；累计的是最近真实音乐（M 一低于回差线就清），读档后立即触发只是跳过 sustain 要求，影响低。commitCandidate 1227 设 committed.constants 而 guardStuckSamples（943）与 monitor.configure 只在 2779-2780/2927/3126-3129 折算，属实；但 updateGuardLimits 1685 setLimits 会让守护自身时长跟方案源走（GlazeGuard::setLimits 159-173 timingChanged→updateTiming），只剩编排器侧两项停旧值，且出厂守护时长常数版本 1～7 没改过，今天实际影响≈0，潜伏项。
- **核验调整后的修法（以此为准）**：采纳研究修法：consumeRestore 加 clearMismatch(); samplesSinceMismatch = mismatchCooldownSamples;；commitCandidate 末尾 updateTiming(); monitor.configure(sampleRate, activeConstants());（内含 clear，提交时清监视窗合理；几个乘法 + 固定数组清零，音频线程可调）。
- 风险：极低；仅播放中读档与旧快照重学两种情形改变行为。
- 验证：播放中 restoreStateXml 一份 I 低 12 LU 的画像，第一块 probe.mismatchSteps 不加 1；setConstantsForTesting 给不同 guardWindowSeconds 的旧快照读回再重学，monitor 窗长回到出厂值（探针暴露 monitor 窗长）。

### R6-04 · 实时 Tame 许可不乘置信度 κ、不乘段可信度，且未学习 / 首次聆听中也生效，与方案里 GLAZE × κ 的口径不一致、与规格 7.2「学习结束后」不符
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:2120`
- 证据：GlazeController.cpp:2110 `if (active && ! savedPlanMode && ! manual.balance.bypass && toSlot().valid)`；:2120 `liveTame = tameCap * glaze * jlimit (0, 1, evidence / tameEvidenceFullDb)`；对照 GlazePlanner.cpp:1527 `spectralStrength = glaze * kappa`、:1565 `inc.balanceTame = max(底量, 证据 × bandTrust) × spectralStrength`。未学习时 toSlot().valid 也为真（保守方案照样评估），κ = 0.35：实验 F 未学习 GLAZE 100 方案 Tame 增量 8.4，而实时路径同状态上限 45（通透 tameCap）× 证据。
- 机理：同一物理量两条路用不同强度口径：方案按置信度收紧，实时许可不收紧；未学习/首次聆听中就按满置信度放许可，规格 7.2 写的是学习结束后再用短期证据加强。
- 研究修法：liveTame 乘 confidenceScale(committed.hasProfile ? &committed.profile : nullptr, snapshot)（与 updateLiveTone 同一写法），并把 committed.hasProfile（或至少 learning != firstListen）列入 2110 行条件；执行快照里的 liveTame 是实际值，恢复路径不变。
- 核验理由：GlazeController.cpp:2110 条件 active && !savedPlanMode && !balance.bypass && toSlot().valid；2120–2121 liveTame = tameCap × glaze × clamp(evidence/tameEvidenceFullDb)，无 κ；toSlot().valid 在任何 evaluate 后为真（1080–1100），含 profile == nullptr 的未学习保守方案；对照同文件 updateLiveTone（2049–2076）门在 committed.hasProfile 且乘 confidenceScale(&committed.profile, c)；方案侧 GlazePlanner.cpp:1527、1565 Tame 增量乘 glaze × κ；实时证据 liveTameEvidenceDb（MusicAnalyzer.cpp:1899–1909）过 settleFrames 即有效、置信恒 1、与是否已提交无关；规格 7.2「学习结束后继续用 Balance 帧算短期突出证据」。三条路只有实时 Tame 既不看学没学过也不乘 κ，口径不一致属实。
- **核验调整后的修法（以此为准）**：liveTame 乘 confidenceScale(committed.hasProfile ? &committed.profile : nullptr, snapshot)，并把 committed.hasProfile 加进 2110 行条件；restoredTamePending 恢复路径（2126–2130）不变。实现前在 107 核 GlazeControllerTests.cpp:2315–2380「locked profile: equal-loudness spectral change increases live Tame」：fastConstants 下只学 4 s 后断言 highest > baseline + 2.0，若该画像 κ < 1 余量会缩；上界断言 ≤ 0.5 × clean.tameCap 不受影响。
- 风险：未学习/低置信时实时 Tame 变小（保守方向）；既有 live Tame 用例的 +2 余量可能需要重看；Tame 是收敛型处理，风险低。
- 验证：GlazeControllerTests：未学习 + GLAZE 100 + 强 Tame 证据素材，effective.balance.automaticTame ≤ tameCap × 0.35 + 1e-3；学习完成后（κ = 1）与现行为一致。

### R5-06 · 热路径每块重复算同一批纯函数：blendConstants ×3、confidenceScale ×4～5、matchingLiveSlopes ×2、referenceMatches ×6
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/dsp/GlazeController.cpp:2117`
- 证据：blendConstants：2075（updateLiveTone）、2117（computeEffective Tame）、2137（computeEffective Tone）；confidenceScale：2076、2138、1516（correctionLimitDb 被 biasTarget 1649/1653 各调一次，updateBias→biasTarget 再来一遍）；matchingLiveSlopes（27 段加权回归 1972-1989）：2016、2081；calibrationApplicable()→referenceMatches（~15 项比较 GlazeCalibrator.cpp:514-549）：1614/1616、1621、1645、1647、1937、2642。T-027 报告第 4 节：省 296 ns/块 = 192k/32 下 0.18 个百分点，这里量级相同或更大。
- 机理：都是同输入的纯函数，每块 0.3～0.5 µs 白算；48k/512 可忽略，192k/32 约 0.2～0.3 个百分点。
- 研究修法：endBlock 开头（advanceTransition 之后）算一次 BlockCache { StyleConstants caps; float kappa, strength; bool applicable; bool slopesOk; float refSlope, curSlope; }，caps 按 (toSlot().source.generation, toSlot().evaluations) 缓存（与 R5-02 同一把键），kappa 只随 committed.profileId / 常数变；各处改读缓存。
- 核验理由：逐处核对调用点：blendConstants 2075/2117/2137；confidenceScale 2076/2138/1516（correctionLimitDb 被 biasTarget 1649、1653 各调一次，updateBias 与首块 2477 再调）；matchingLiveSlopes 27 段回归 2016/2081；calibrationApplicable→referenceMatches 1616/1621/1645/1647/1937/2642。blendConstants 独热早退（GlazePlanner 1290-1293）只拷 20 浮点，自动风格混合权重时 60 次乘加；matchingLiveSlopes 两次是大头。量级 0.3～0.6 µs/块，192k/32 约 0.2～0.3 pp，与 T-027 §4 每块 22.7 µs 口径一致；48k/512 可忽略。
- **核验调整后的修法（以此为准）**：采纳但工作量不止 S：computeEffective 在 beginBlock、updateLiveTone/Color 在 endBlock，中间 advanceTransition 2605 可能 startTransition 换 toSlot()，refreshPlans 又可能重算权重；缓存键必须 = (toIndex, toSlot().source.generation, toSlot().evaluations（R5-02 改全局计数后即可）) 并在两处各校验一次，confidenceScale 键 = committed.profileId + 常数代。排在 R5-02 之后做，用 T-027 式「逐块比缓存与现算逐位相同」用例守。
- 风险：纯函数同输入逐位不变，风险全在键：漏一项就破逐位且难察觉；收益仅 192k/32 场景。
- 验证：仿 T-027 用例：逐块比缓存值与现算值逐位相同（blendConstants 独热早退保证不受影响）；全套逐位测试不变；192k/32 refbench 前后对比。
