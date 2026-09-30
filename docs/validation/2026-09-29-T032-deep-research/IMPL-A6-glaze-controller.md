# T-032 实施报告 · A6-glaze-controller（GLAZE 编排器：校准 / 保护层 / 约束反馈）

> 2026-09-30，Opus 5.5 · max 接续前任（Fable 5.1，09-29 23:2x 撞额度，半成品由主进程做成 WIP `c72e073`）。
> 工作树 `.claude/worktrees/wf_abb55518-7ae-5`，分支 `worktree-wf_abb55518-7ae-5`，底 = `c72e073~1`（main `e6b445b`）。
> 任务书 `BRIEF-A6-glaze-controller.md`；研究 / 核验全文 `R5-glaze-control.md`、`V5-glaze-control.md`、`R6-glaze-planner.md`、`V6-glaze-planner.md`（主进程工作树同目录）。
> 只在 107 上编译与测试（本机 Mac 未跑任何编译 / 测试），构建一律 `-j 3`。

## 0. 一览

| 条目 | 结论 | 改输出 | 新用例（`tests/GlazeControllerTests.cpp`） | 修前（B3 实测） | 修后 |
|---|---|---|---|---|---|
| R5-01 + 核验 M1 | 做（修法①） | 是：只在放弃 / 丢轮后条件回原样时 | `testAbandonedRoundIsNotAFailedAttempt` | GLAZE 0 / Limit Bypass / 换风格三例都停在门掩码 512，锁定但永不校准 | 三例各再开 1 轮、锁定且有效 |
| R5-02 | 做（全局单调计数） | 是：换风格 / 提交 / 重学之后 | `testGuardLimitsFollowEveryPlanChange` | 10 次核对 4 次沿用上一份方案的上限 | 0 / 10 |
| R5-03 + 核验 M2 | 做 | 是：静音 / 停播 / 旁通场景 | `testFrozenGuardTimeIsNotHostTime` | 静音、宿主旁通中吸收；Color 旁通中开轮→预算受限吸收、切回再吸收；静音 8 s 观察期走了 8 s | 冻结中 0 反馈、计数与观察期都不走；音乐回来照常吸收 |
| R5-04 | 做（只 carry 吸收量）＋ carry 淡完前不开校准轮 | 是：重开 / reset 后第一次改条件 | `testReleasingAFrozenExecutionCarriesTheAbsorbedRetreat` | 解除冻结一块跳 0.465 dB（GLAZE）/ 0.510 dB（换风格） | 首块只剩 GLAZE 映射本身；淡出每块 ≤ 0.011 / 0.016 dB |
| R5-08 | 做 ＋ 读档清「建议重新聆听」提示（同根） | 是：播放中读档、旧快照重学 | `testRestoreAndCommitInitialiseRuntimeAlike` | 读档 0.49 s 就按旧累计触发；提示残留；重学后窗 60 格 / 卡住 11 s | 累计重来、1.48 s 才触发；提示清掉；30 格 / 6 s |
| R6-04 | 做 | 是：κ < 1、未学习 | `testLiveTameNeedsAProfileAndScalesWithConfidence` | 未学习多给 36.3 Tame；κ 0.675 也到 22.5 | 未学习 0；κ 0.675 封顶 15.187 |
| R5-06 | 做（同一次调用内去重，不做跨块缓存） | 否（逐位不变） | `testT032ControllerDigests`（跨构建摘要对照） | — | 四段摘要去重前后逐位相同 |
| 核验 M3 | 不改（主进程要求） | — | — | — | 列「待 Max」 |

**107 全套：133151 过 / 0 败，编译警告 0**（基线：前任 09-29 23:07 在 107 跑的改前全套 132981 过 / 0 败 / 0 警告；多出的 170 项是 7 个新用例的断言）。最终树复跑见第 3 节。

## 1. 逐条

### R5-01（含核验 M1）「上次尝试」只记结束为无效的轮
- **改法**（核验调整后的修法①）：`beginRound` 不再记「上次尝试」；`finishRound` 里仅 `! committed.calibration.valid` 时 `lastAttemptReference = roundReference; haveLastAttempt = true;`。修法②（放弃分支清标志）未加：①已同时堵住「放弃」与「换方案丢轮」两条路，②单独堵不住换风格、叠加也无必要。
- **位置**：`Source/dsp/GlazeController.cpp` `beginRound`（1574）、`finishRound`（1586–1601）；`GlazeController.h` 校准段与 TestProbe 门掩码 512 位注释。
- **语义复核**：`GlazeCalibrator::begin` 只在目标非有限时直接 `finished / valid = false`，`finish()` 其余路径都 `valid = true`，防「每块重开」的作用保留。
- **用例**：Rig 学完、GLAZE 60 自动风格进入第一轮校准后绕行，再回原条件，`learnAndCalibrate(30)`：
  - GLAZE 0 4 s / Limit Bypass 4 s / 换温润跑满过渡再切回：修后「begins 2（绕行中 0）、finishes 1、门掩码 128、锁定 / 有效」；B3 修前三例都是「begins 1、finishes 0、门掩码 512、锁定 / 不有效」。
  - **Color Bypass 往返改为对照例**：任务书要求的「Color Bypass 往返」核过**不复现**缺陷。Color 旁通在参考条件里但不挡开轮：第 3 s 放弃的那一块，旁通条件已稳定 3 s，当即按旁通条件开了新一轮（修前也因此把「上次尝试」覆盖成旁通条件），切回后照常再校准——修前修后都是 begins 3、锁定 / 有效（修前结束时门掩码 640 = 512 + 128，只是诊断位不同）。真会卡住的旁通是 **Limit Bypass**：它让方案 `limitedByManual`，放弃后开不了新一轮，补为缺陷例。前任用例在 Color 一例断言「绕行中不开新一轮」，修前修后都不成立。
- **风险**：只在「放弃 / 丢轮后条件回到原样」改输出（原永不校准 → 校准）；GLAZE 0 契约不涉及。

### R5-02 保护层 / 上游监视上限的去重键改为全局单调计数
- **改法**：`Impl::evaluationCounter`（838）；`evaluate` 里 `slot.evaluations = ++evaluationCounter`（1133）；`updateGuardLimits`（1734）照旧按 `slot.evaluations` 去重，键语义变成「目标方案的这一次重算」。32 位计数即使连续自动化、过渡中两槽都重算（每 5 ms 最多两次），也要约 124 天才回绕，回绕后还得恰好撞上同一个值才会漏一次。
- **探针**：`guardLimitsSet / guardLimits`（`GlazeGuard.h` 加两个只读访问器 `hasLimits / getLimits`）。
- **用例**：学完（自动）→ 依次换风格 1 3 2 1 0 3 2 0（各跑满过渡）→ 重新聆听提交；每次核对 `guardLimits` 与 `blendConstants(已提交常数, 目标方案实际权重)` 全部 20 个字段逐位相同，另断言温润与通透的上限确实不同（区分度）。修后 0 / 10 沿用；B3 修前 4 / 10（例：换温润后保护层仍是自动风格的 5.60 / 1.40 dB，应为 4.00 / 1.00；换冲击后仍是通透的 6.00 / 1.50，应为 3.00 / 0.80）。
- **未做**研究建议的「行为式」断言（温润→通透后 ×5 热素材不该退让）：依赖素材的 GR 恰好落在两风格上限之间，脆；探针逐字段比直接覆盖根因。

### R5-03（含核验 M2）保护层的时间轴
- **改法**：`endBlock` 取分析器视图后按保护层口径算 `guardMusicActive = view.musicActive && ! paused && ! manual.color.bypass && ! manual.limit.bypass`（2675，与原 `guardStats.musicActive` 同一式子，保护层直接用它）；`guardSteadySamples` 只在它为真的块累加（2730；冻结块不加也不清，`lastGuardDb` 照旧每块更新）；M2：`updateFeedback (n, guardMusicActive)` 里 `samplesSinceFeedback` 只在它为真时累加（1972–1975）——上游监视窗也只在这时加时隙，观察期与窗同一时间轴。
- **用例**：12b 同一个 Rig（冲击、GLAZE 100、×5 热素材、画像失配关掉）退到 −0.31 dB 后冻结 8 s：
  - 静音：修后「没变」计数 0.00 s、0 反馈；音乐回来后照常按音乐时间吸收（stuck 1、上限 3.45 dB）。B3 修前：静音中就吸收（上限 3.75 dB）。
  - 宿主旁通：修后同上；B3 修前旁通中吸收（上限 4.17 dB）。
  - Color 旁通：旁通期间校准不适用、卡住那条吸收不了，但修前计数照走，旁通中仍出了 1 步反馈，切回后又吸收 1 次（共 2 步，上限 3.15 dB）。旁通中那一步的来源日志没单独记，按排除法：上游监视冻结时不加时隙、保护层冻结不会新进「方案受限」、画像失配关着，只剩「计数走满 6 s → `guardSettled` 按卡住放行 → 旁通条件下开轮 → 保护层介入耗尽预算 → 预算受限吸收」——即研究第二段推演（核验判「偏弱」）在这里成立。修后 0 反馈。
  - Limit 旁通**不在**用例里：常规模式下 Limit 旁通让方案 `limitedByManual`，保护层整块跳过并 reset，不是冻结。核验说的「Limit Bypass 6 s 也会吸收」只在照用旧存档方案（`savedPlanMode`，方案里的 `limitedByManual` 不随旁通重算）时成立，本修法同样覆盖。
  - M2：吸收后 1 s 再静音 8 s，修后观察期 1.01 → 1.44 s（0.43 s 是分析器音乐门关上的延迟）；B3 修前 3.09 → 11.09 s。音乐回来观察期接着走。
- 冻结结束后的头 0.5 s 不许吸收（抓 Color 旁通切回那一块）；吸收后先跑 1 s（吸收型反馈的 0.5 s 方案过渡）再断言锁定且有效。

### R5-04 解除执行快照冻结时 carry 吸收量
- **改法**（核验调整：只 carry、不改路径语义）：`Impl::absorbedGuardDb`（901）；`adoptExecution` 记下并进冻结方案的退让（1443），不沿用时清 0；`releaseAbsorbedGuard()`（2029）：`guardCarryStartDb = absorbed + 剩余 carry`、`guardCarryElapsed = 0`、`guardCarryLength = planTransitionSamples`，与 `lowerGainCap` 同一套 carry（`computeEffective` 里 guardDb = 守护 + carry）；解除冻结处（`beginBlock`，2475）在 `snapTo` 之后调——换风格也先经这里解除冻结，一个调用点覆盖「两处」。GLAZE ≤ 0 不 carry（GLAZE 0 逐位契约；`! active` 直接清 0）。`commitCandidate` / `lowerGainCap` 也会解除冻结，但那两处开方案过渡、冻结方案留在过渡起点槽里随增量混合淡出，只清 `absorbedGuardDb`、不 carry。③（`onHostReset` 过渡中不截断）按核验不动。
- **补：carry 淡完之前不开新的校准轮**：`guardSettled` 加 `&& guardCarryLength <= 0`（1658；门掩码 32 位注释同步）。理由：解除冻结的淡出不伴随方案过渡，用户条件稳定 1 s 就能开轮；出厂常数下 settle 3 s 恰好盖住 3 s 淡出，但那是常数巧合、不是构造保证（决策第 6 节「保护层改了实际增益：子窗作废」）。约束反馈的 carry 与方案过渡同长、比过渡早一块结束，过渡本来就挡着，这一条对它不起作用（全套里既有用例的日志逐行不变印证）。
- **用例**：供体锁定后推热、保护层退让 > 0.5 dB（尚无反馈）时存档，absorbed = −0.51 dB；读回的链换回安静素材沿用 2 s 后改条件：
  - 重开 + GLAZE 100→99：修后首块净增益变化 −0.04484 dB，正好是 GLAZE 映射本身；淡出期间最大每块 0.01088 dB（界 0.03221）；一个过渡长度后 carry = 0。B3 修前首块跳 0.46516 dB。
  - 重开 + 冲击→温润：修后首块 0.00000 dB、淡出最大 0.01635 dB（界 0.03768）；B3 修前 0.51000 dB。
  - 宿主 reset 沿用执行量 + GLAZE 100→99（核验要求的「宿主 reset 例同理」，前任没写）：与重开逐位同数。
  - 开轮门：过渡放长到 3 s，解除冻结 1.5 s 后 carry −0.2525 dB、门掩码只剩 32（稳定、不适用）、没开轮；carry 淡完才开轮，开轮那一块 carry = 0。B2（去掉这一条）1 s 左右就开轮，开轮时 carry 还有 −0.2507 dB。
- **已知遗留**：过渡中途保存的混合值差仍是阶跃（核验接受）；解除冻结时若恰有进行中的校准轮、3 s 内条件又回到本轮参考，本轮会接着采集（见第 9 节）。

### R5-08 读档 / 提交的运行态初始化对称
- **改法**：`consumeRestore` 末尾（`updateTiming` 之后，冷却按本版本折算）`clearMismatch(); samplesSinceMismatch = mismatchCooldownSamples;`；**同根一并修**：`profileMismatch = false`（「建议重新聆听」提示说的是旧画像；`commitCandidate` 换画像时本来就清，读档漏了；失配给的上限随工程保存、照常读回）。`commitCandidate` 在 `requestPlanTransition` 之前 `updateTiming(); monitor.configure (sampleRate, activeConstants());`（只有 `guardStuckSamples` 随快照变，其余折算来自 live 常数不变；监视窗清空对新方案合理）。
- **探针**：`monitorWindowSlots / monitorTriggerSlots / guardStuckSamples`，新增 `mismatchSamples`（失配累计量，用来断言用例前提成立）。
- **用例 (a)**：门槛 12 / 回差 6 LU；读回低 10 LU 的画像播 3 s（累计 2.52 s、不触发；前提断言累计已超持续时长）→ 读回低 15 LU：修后累计从读档那一块重来，1.48 s 才触发；B3 修前 0.49 s 就触发。**修前并不是「第一块」触发**：读档时分析器短期窗重来，约 0.4 s 后 M 恢复有效才按旧累计触发——研究 / 核验的「第一块」说法不准，用例按实测写「累计已从读档块重来」「不早于持续时长」两条断言。再读回原画像：修后提示清掉，B3 修前残留。
- **用例 (b)**：旧快照（守护窗 6 s、恢复等待 10 s）读回后 60 格 / 11 s；重新聆听提交后修后回到 30 格 / 触发 10 格 / 6 s，B3 修前停在 60 格 / 528000 样本。

### R6-04 实时 Tame 许可乘 κ、学后才放
- **改法**：`computeEffective` 实时 Tame 分支加 `committed.hasProfile`，许可 = `tameCap × GLAZE × κ × 证据`，κ = `confidenceScale(&committed.profile, 目标方案源常数)`（与 Tone 同一份，2249–2256）。
- **`committed.hasProfile` 放在内层条件**，不是核验字面的「加进 2110 行（外层）条件」：外层条件还罩着 `restoredTamePending` 恢复分支，放外层会让「首次聆听中存档、重开沿用执行量」的第一块丢掉快照里的 liveTame（首块阶跃），违背核验同条的「恢复路径（2126–2130）不变」。放内层后恢复分支逐字不变；未学习的沿用期间只用快照份额、不放新许可，条件一变（解除冻结时清 `restoredTamePending`）或提交之后按新规则。
- **既有用例的 +2.0 余量**（任务书要先核的 `locked profile: equal-loudness spectral change increases live Tame`）：该用例画像 κ = 0.512，修后基线 6.150、最高 11.531，余量 3.381（修前最高约 22.5 = 上限），照过；加了一行日志记录余量。
- **新用例**：(a) 未学习（首次聆听 15 s）GLAZE 100 通透 + 2 kHz 强共振：修后超出方案增量的量 0.00000；B3 修前 36.29（顶到通透上限 45）。(b) κ 0.675 / 1 两份画像同一证据：修后最高 15.187 / 22.500，各自正好顶在「上限 × GLAZE × κ」；B3 修前两者都是 22.5。
- κ = 1 时逐位同修前：第 4 节 κ = 1 段 B0 = B1。

### R5-06 同一次调用内去重（不做跨块缓存）
- **做了**：`computeEffective` 的 Tame / Tone 共用一次 `blendConstants` 与一次 κ（按需惰性算，门没过的块一次也不算，2218–2238）；`biasTarget` 的 `correctionLimitDb` 与 `calibrationApplicable` 各一次（1685 起）；`finishRound` 的 `correctionLimitDb` 一次；`updateLiveTone / updateLiveColor` 共用一次 27 段斜率回归（`LiveSlopes`，2076；`endBlock` 2755 起），两次调用之间分析器视图与已提交画像不变。都是栈上局部，零分配、无锁（既有「音频线程零分配」用例照过）。
- **没做**：`updateCalibration` 门里的 `calibrationApplicable()` / `referenceMatches(lastAttempt)` 各两次——那两行（改前 1615 / 1621）是 A7 R6-02 的门，任务书明令不碰；`updateFeedback` 卡住分支与状态快照里的 `calibrationApplicable()`：与前面之间有 `finishRound / applyFeedback` 改状态，不是同输入；`updateLiveTone` 与 `correctionLimitDb` 的 κ：常数来源不同（目标方案源 vs 已提交快照，沿用执行快照 / 首次聆听时不是同一份）；跨块缓存键（主进程明令不做）。
- **逐位证据**：第 4 节（B0 = B3、B1 = B2，四段全等）。

## 2. 前任改动的审查与改正

前任源码改动逐条对照任务书复核过；测试写了 6 个用例但从未编译运行过。改正如下：

1. **`updateCalibration` 开轮门整段重排**（改成早退 + 两个局部变量）：压在 A7 R6-02 要改的 1615 / 1621 两行上，1621 本身就是任务书「不要碰」的七处门之一，合并必冲突。恢复成改前原文；这里的去重不做（第 1 节 R5-06）。
2. **`guardMusicActive` 插在 A7 要改的 2574–2575 注释与 2576 的 `if` 之间**：合并必冲突。挪到 `endBlock` 取分析器视图之后（离 A7 的行很远），保护层与观察期共用。
3. **R5-01 用例的 Color Bypass 一例**断言「绕行中不开新一轮」，修前修后都不成立（第 1 节 R5-01）。改为 Limit Bypass 作缺陷例，Color Bypass 保留为对照例并改正断言。
4. **R5-03 用例只测了静音**；M2 那条断言放在「从没反馈过」的状态（`samplesSinceFeedback` 起点 `max / 4`，冷却早就过期），检查的量没有意义，而且修前静音中的吸收会把它清零、断言照样通过。补宿主旁通、Color 旁通两例，M2 改成在一次吸收之后（冷却从 0 起算）再静音检查。
5. **R5-04 只有保存重开一种入口**：补宿主 reset 沿用执行量一例；新增「carry 淡完前不开轮」及其用例。
6. **R5-08**：补「建议重新聆听」提示同根一并清；用例 (a) 没核前提（读档前是否真的累计满持续时长）→ 加探针 `mismatchSamples` 断言前提，9 LU 改 10 LU（离回差线远一点），并按实测改正「修前第一块触发」的说法。
7. **R5-02 用例**只比 10 个字段、按 `weightsFor` 推期望：改为与目标方案实际权重的 `blendConstants` 全 20 字段逐位比，加区分度断言与重新聆听提交一例。
8. **-Wshadow**：用例里的局部变量 `name` 遮蔽 `juce::UnitTest::name`，首次编译 6 条警告；全部改名 `tag`，之后 0 警告。
9. **用例时序**：吸收后立刻断言「锁定」撞上吸收型反馈的 0.5 s 方案过渡（首轮定向 2 败），改为吸收后先跑 1 s。
10. **保留**：R5-01 `finishRound` 记录、R5-02 全局计数、R5-03 / M2 的 musicActive 门、R5-04 `absorbedGuardDb` + `releaseAbsorbedGuard`、R5-08 两处、R6-04（κ 与 `hasProfile` 放内层条件）、R5-06 其余去重、探针与 `GlazeGuard` 两个只读访问器——逐项复核语义与逐位等价后保留。另补：`profileMismatch` 成员注释、`.cpp` 文件头 T-032 段、头文件校准段 / 约束反馈段 / 门掩码 32、512 位注释。

## 3. 测试命令与数字

远端都用本工作树的 `scripts/test-linux.sh`（按工作树路径分远端目录）：

```
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests" --only GlazeController,GlazeCalibration,GlazeChain,GlazeBehaviourReport,ClayGlaze'
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests"'
```

（任务书建议的 `--only …,Glaze` 类名不存在：`GlazeTests.cpp` 的类名是 `GlazeChain`，已按实际类名跑。）

| 运行 | 源码 | 结果 | 编译警告 |
|---|---|---|---|
| 改前基线（前任 09-29 23:07） | `c72e073~1` | 全套 132981 过 / 0 败 | 0（`grep -c "warning:"` = 7，全是测试名里的 "phase warning:"） |
| 定向（首轮） | 接续后 | 编译 6 条 -Wshadow（`name`）→ 已改 | 6 → 0 |
| 定向（次轮） | 接续后 | 3582 过 / 2 败（用例时序，已改） | 0 |
| 全套 | `29e8b6e` 同源码 | **133151 过 / 0 败** | 0（raw 7，同基线） |
| B3 缺陷还原（`--only GlazeController`） | 修复全撤、探针与去重保留 | 2539 过 / 48 败——全落在 7 个新用例的缺陷断言上 | — |
| B2 去重与开轮门还原（同上） | 只撤 R5-06 与 carry 开轮门 | 2584 过 / 3 败——只有「carry 淡出期间不开轮」3 条 | — |
| B0 改前源码 + 只加摘要用例（同上） | `c72e073~1` 源码与用例 + `testT032ControllerDigests` | 2425 过 / 0 败 | — |
| 全套（最终树） | `a8a2e4d`（源码与用例即最终提交的内容，之后只加本报告） | **133151 过 / 0 败** | 0（raw 7，同基线） |

最终树全套是在 B0 换过头文件之后跑的，`GlazeController.cpp`、`GlazeGuard.cpp` 与各 Glaze 测试文件都重新编译过；四段摘要与 B1 相同，既有用例日志除性能计时外与上一次全套逐行相同。

**既有用例输出逐行比**：全套日志去掉构建行与性能计时后与改前基线逐行相同（含 `GlazeBehaviourReport` 全部风格表、12a / 12b / 12d 的反馈统计、整链生命周期与路径统计）；差异只有性能计时抖动与新用例自己的行。

## 4. 逐位证据（跨构建摘要）

`testT032ControllerDigests` 逐块把交给模块的实际参数（Balance / Color / Limit 9 项）、执行的校准偏差 / 保护偏移、实时 Heat / Glue 回收量与输出样本按位并进 64 位 FNV-1a。四段场景覆盖去重的每条路径：κ = 1 锁定画像换段播放（音色跟随、实时 Tame、手动 Tone +2 dB 让校准偏差非 0）、κ = 0.675 同上、密度跟随（稀疏素材上学、GLAZE 60 温润换密集白噪声）、GLAZE 0 边学边播（另断言每块实际参数逐位等于手动值）。用例自己还断言每条路径确实走到、同一场景跑两遍摘要相同。

| 段 | B0 改前 | B3 缺陷还原（含去重） | B1 修后 | B2 修后撤去重与开轮门 |
|---|---|---|---|---|
| κ = 1 锁定 | `0x3d0cd9f549a606cf` | `0x3d0cd9f549a606cf` | `0x3d0cd9f549a606cf` | `0x3d0cd9f549a606cf` |
| κ = 0.675 锁定 | `0x0b33b6428de75274` | `0x0b33b6428de75274` | `0x954886a32c3307b6` | `0x954886a32c3307b6` |
| 密度跟随 | `0xeef4e509c391e272` | `0xeef4e509c391e272` | `0xeef4e509c391e272` | `0xeef4e509c391e272` |
| GLAZE 0 | `0xaa8f458c4d75b314` | `0xaa8f458c4d75b314` | `0xaa8f458c4d75b314` | `0xaa8f458c4d75b314` |

- **R5-06 逐位不变**：B0 = B3（改前行为上，去重前后四段全等）；B1 = B2（修后行为上，去重前后四段全等）。
- **GLAZE 0 契约**：四个构建同一个摘要；另有既有用例逐位断言（全套照过）。
- **R6-04 κ = 1 逐位同修前**：κ = 1 段 B0 = B1。κ = 0.675 段 B0 ≠ B1 且差异只来自 R6-04（B0 = B3、B1 = B2）。
- 旁通 / 默认抽屉等既有逐位契约：由既有用例断言（全套 0 败）。本次没有一条碰浮点合并或编译选项（ADR-013）。

## 5. 与 A7 的合并核对

A7（R6-02，工作树 `wf_abb55518-7ae-6`）在 `GlazeController.cpp` 只许改七处门、两个掩码第 16 位及其注释。用 `git merge-file`（底 = `c72e073~1`）把本工作树的 `GlazeController.cpp` 与 A7 工作树的当前文件三方试合并，两次都 **0 冲突**：09-30 约 08:00（A7 当时改了七处门与两个掩码，12 处 `automaticLevelWithdrawn` 全部并入）、08:24 用我方定稿文件再试（A7 的接续代理已改成只动 1845 一处门与 2159、2574–2575 两段注释）。我方离 A7 可能改动的行最近处：`guardSettled`（与 1599–1600 注释隔 3 行未改行）、`biasTarget` 去重（与 1633 隔 2 行）、`guardStats.musicActive`（与 2576 隔 4 行）。A7 定稿若再动这些行附近，主进程合并时需复核。

## 6. 没做的与原因

- `updateCalibration` 开轮门的 `calibrationApplicable()` / `referenceMatches(lastAttempt)` 去重：A7 的门行，按任务书不碰；A7 合入后可在其上单独补（纯去重、逐位不变）。
- 跨块缓存键（研究 R5-06 的 `BlockCache`）：主进程明令不做。其余不同输入的调用点见第 1 节 R5-06。
- R5-04 ③（`onHostReset` 沿用执行量时过渡中不截断）：核验判定不动。
- R5-02 行为式断言：见第 1 节，改用探针逐字段比。
- 核验 M3：主进程要求不改（第 9 节）。

## 7. 跨文件需求

- 无必需项。
- 提醒（主进程合并 A7 之后）：`GlazeController.h` TestProbe 两个掩码第 16 位的注释仍写「受手动设置限制」；A7 的 R6-02 让这一位改看 `automaticLevelWithdrawn`（电平类自动全部撤回），头文件归 A6、A7 改不了，合并后宜顺手改为「电平类自动全部撤回」。

## 8. 档案

- 已改（Edit 增量）：`01-框架/设计规格-GLAZE-决策与校准.md` 末尾加「2026-09-29 修订（T-032）」小节（全文 95 行）。
- 待主进程：`04-协调/TASKS.md`、当日日志、INDEX（本代理按任务书不碰）。

## 9. 待 Max

1. **核验 M3**：沿用执行快照期间 `refreshPlans` 把快照方案（含 `adoptExecution` 吸收的退让）写进 `committed.plan`，随下次保存进正式版本；是否有意未定，按主进程要求未改（若不是有意，修法一行：条件加 `&& ! toSlot().source.useSavedPlan`）。
2. **解除冻结时的进行中校准轮**：沿用执行快照期间可能在跑校准轮（执行快照带退让时要求重测）。解除冻结后若 3 s 内条件又回到本轮参考，本轮会接着采集，目标仍是冻结方案（含吸收的退让）的净提升，吸收量会以校准偏差的形式回来。建议解除冻结时与 `requestPlanTransition` 同一规则丢掉进行中的轮；这改变校准路径，不在本次核验调整后的修法里，未改。
3. **R6-04 的可闻变化**：κ < 1 的画像实时 Tame 许可按 κ 缩小（既有用例的画像 κ 0.512：共振段最高 Tame 约 22.5 → 11.5）；未学习 / 首次聆听中不再有实时许可。保守方向，待试听。
4. **研究 / 核验与代码不符的两处**（供复盘）：Color Bypass 往返不会卡住校准（Limit Bypass 会）；常规模式下 Limit Bypass 是 reset 保护层而不是冻结。R5-08 修前也不是「读档第一块」触发，而是分析器窗重来约 0.4 s 后。
