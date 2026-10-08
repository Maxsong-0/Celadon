# 0.1.8.9 阶段 2 测试基线

基线提交：`5a21425`（发行 0.1.8.9，T-048 阶段 2）。本目录只改测试、渲染自检和这篇说明。插件 DSP 没有改。

合同在 `tests/Phase2Contract.h`（`celadon::phase2`）。生产实现是 `Source/dsp/GlazeController.cpp` 的 `Impl`：`Phase` 停在 `unlearned`，`calibrationValid` 恒为 false，`requestRelearn()` 是空操作。连续行为仍在：约 1 秒一格重算方案，增益上行 0.2 dB/s、下行 1.0 dB/s，死区进入 `|误差| < 0.1 dB`、离开 `|误差| > 0.3 dB`（生产常量名与合同草图对调，行为与草图一致）。

改前数字取自同树的 `docs/validation/2026-10-05-T048-phase2/REPORT-P2-INTEGRATE-SPARK.md`，没有再跑那一版会在聆听锁定上超时的全套。

## 前后计数

| 套件 | 改前 | 改后 |
|---|---|---|
| x86 `celadon_tests`（本机 Release，同一套 CI 会编的目标） | 136320 过 / 438 败 | 134733 过 / 0 败 |
| Spark aarch64 `celadon_tests` | 136062 过 / 443 败 | 本机是 x86，未重跑。下面 5 条 aarch64 专有失败仍应在 Spark 上出现 |
| `celadon_render --selftest-house` | 1 败 | 通过（本机 Release x86，2026-10-08） |
| `celadon_render --selftest-live` | 2 败 | 通过（本机 Release x86，2026-10-08） |

改前一共执行 136320 + 438 = 136758 条断言。改后是 134733 条，少 2025 条。其中 438 条是原来的失败。其余约 1587 条原先能通过，但写在已经不再调用的聆听 / 锁定方法里，依赖的状态不存在了，所以不再执行。下面列出的新用例补上了连续编排器、真峰值、滑移和零分配，不是 438 条一对一留下。

本机全套命令：`/tmp/celadon-build/celadon_tests`（Release，2026-10-08，日志末行 `134733 passed, 0 failed`）。CI 的 x86-64 与 x86-64-v3 矩阵还要在自托管 runner 上再跑一遍；本机没有 `-march=x86-64-v3` 的第二遍。

改前 438 的分桶：

| 类 | 失败断言 | 归类 |
|---|---:|---|
| GlazeController | 294 | (a) 聆听 / 锁定 / 校准 |
| GlazeControllerHouse | 58 | (a) 同上，另有 House 门控 |
| ClayGlaze | 29 | (a) 为主，少量 (b) 时序与低切锚点 |
| GlazeChain | 27 | (a) |
| LiveProfile | 30 | (b) `testHouseEquivalence`，门控账已退役 |
| ColorModule | 4 | aarch64 专有，本 PR 不改 |
| BalancePerformance | 1 | aarch64 专有，本 PR 不改 |

非 aarch64 合计 408 + 30 = 438。

本机改写后的类（Release，GCC 13，`cmake -S /workspace -B /tmp/celadon-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=/JUCE`）：

| 类 | 结果 |
|---|---|
| LiveProfile | 356 过 / 0 败 |
| GlazeController + GlazeControllerHouse | 96 过 / 0 败 |
| ClayGlaze | 129 过 / 0 败 |
| GlazeChain | 176 过 / 0 败 |

仓库里没有这些失败对应的音频 golden 文件。锚点是运行时比较和自检哈希。下面的「重生成」是改比较目标并记下测量，不写入旧的 T-032 摘要。

## (a) 陈旧的聆听 / 锁定断言

阶段 2 删掉了聆听 → 锁定 → 校准。这些断言改成核对连续编排器，没有放宽真峰值、GLAZE 0 逐位直通、同一次运行内的逐位确定、音频线程零分配。

`GlazeControllerTests::runTest` 现在只跑：

- `testContinuousPhase`：阶段停在 unlearned，提交 / 校准 / 反馈计数为 0，`requestRelearn` 不提交、也不清置信度
- `testGlazeZeroSnapAndSlew`：GLAZE 0 与手动逐位相同，Clay 中性；拧上之后按 0.2 / 1.0 dB/s 滑移
- `testStyleChangeDoesNotCalibrate`：风格切换立刻重算，不开校准
- `testTransportAndHostReset`：停着的光标移动不是跳播；播放中的跳播计 `transportJumps`；循环回绕计 `loopWraps`；宿主 reset 增加 `hostResets`，不清置信度
- `testLiveRoundTrip`：存档是 `GLAZE_STATE` formatVersion 2，外层没有 PLAN / CALIBRATION / GLAZE_PROFILE / EXECUTION；读回后重放逐位相同
- `testLegacyListenStateColdStarts`：v1 聆听树冷启动并置 `legacySessionDiscarded`
- `testOfflineColdExport`：离线且置信度 < 0.5 时 `coldExport`，到 0.5 以后清除
- `testSameSpecReprepareKeepsLive`：同规格再次 prepare 保留 LIVE
- `testDeterministicReplay`：两次运行逐位相同，真峰值不超 Ceiling
- `testRatchetBlocksGainRise`：mainness < 0.5 时增益不升
- `testPrepareOrderLogsFirstPrepare`：见类别 (c)，只断言先 prepare 再读档
- `testManualComesFirst`：手动增益是地板；保护层仍会退让。刺激改成 punchy、手动增益 3、再把素材增益放到 8 持续 8 秒，因为 0.2 dB/s 的上行滑移在旧的 6 秒 / 素材增益 3 上到不了紧预算。本机最深执行保护 −3.00 dB，真峰值 −1.35 dBTP（Ceiling −1）
- `testNonFiniteOutputAndInputMeter`：非有限输出仍报告、清缓冲、`hostResets + 1`、随后恢复；阶段改为 unlearned
- `testAudioThreadDoesNotAllocate`：60 秒路径分配数为 0。探针改为跳播 / 回绕 / reset 各至少 1，提交与校准保持 0
- `testClayNeutralAtGlazeZero`

`GlazeControllerHouseTests::runTest` 现在只跑：

- `testHouseStaysUnlocked`：不锁定、不提交、`houseLockPending` 保持 false，`houseGate` 无效
- `testBeatReleaseCap`：House + 有效拍点封顶 `60000 / bpm / beatReleaseDivisor`；通用档或没有拍点时为 0
- `testTempoDeterminism`：House 124 BPM 重复走带逐位相同；通用档给不给拍点在这条 2 秒夹具上逐位相同（自检的噪声夹具不必相同，见下）
- `testDeliveryChangeDoesNotCalibrate`
- `testHouseColdExportAndTruePeak`
- `testHouseAudioThreadAllocation`：8 秒换交付 + 跳播，分配数为 0

下面这些私有方法还在文件里，**没有调用**。它们断言的锁定、重开、校准窗口、T-032 摘要已经不存在。留着是为了编译期仍能看见旧夹具；不要把它们算进在跑的套件。

GlazeController（49 个）：`testPhasesAndFlags`，`testGlazeZeroLearnsButLeavesManualUntouched`，`testFirstListenButtonRestarts`，`testRelearnCandidateAndCancel`，`testDiscontinuitiesClearTheirOwnState`，`testInternalSliceTimeline`，`testOfflineDoesNotLearnOrCalibrate`，`testExecutionSnapshots`，`testSaveAndRestore`，`testSampleRateChange`，`testSameSpecReprepareKeepsHeardExecution`，`testGlazeAutomationPath`，`testStyleChangeIsAPlanTransition`，`testOlderAlgorithmPlanIsKept`，`testAlgorithmSixProjectIsCurrent`，`testOldHeatModelIsMigrated`，`testListenDoesNotPauseCalibration`，`testLiveColorAfterProfileLocks`，`testLiveToneAfterProfileLocks`，`testLiveTameAfterProfileLocks`，`testConstraintFeedbackConverges`，`testStuckGuardIsAbsorbed`，`testAbsorbingFeedbackKeepsCalibration`，`testProfileMismatch`，`testIrregularHostTimeline`，`testClayThreeStates`，`testClayProfileFormatCompatibility`，`testClayOldProfileStaysNeutral`，`testClayTransitionBlend`，`testClayPressureFeedback`，`testClayPressurePeakCost`，`testClayPeakGuardIndicatorThroughController`，`testClayGuardThroughController`，`testClaySnapshotRoundTrip`，`testClaySettingsJoinCalibrationReference`，`testClayAudioThreadDoesNotAllocate`，`testClayCostMaskReachesStatus`，`testClayRestoreNeedsSupport`，`testReferenceSurvivesParameterRoundTrip`，`testCalibrationSurvivesReopenDrift`，`testSlowLogKnobSweepIsNotStable`，`testManualConditionsCacheIsBitExact`，`testAbandonedRoundIsNotAFailedAttempt`，`testGuardLimitsFollowEveryPlanChange`，`testFrozenGuardTimeIsNotHostTime`，`testReleasingAFrozenExecutionCarriesTheAbsorbedRetreat`，`testRestoreAndCommitInitialiseRuntimeAlike`，`testLiveTameNeedsAProfileAndScalesWithConfidence`，`testT032ControllerDigests`。

GlazeControllerHouse（13 个）：`testLockConditions`，`testFastTransitionAtTheDrop`，`testReopenAfterLock`，`testQuieterSectionsNeverReopen`，`testGenreSwitches`，`testDeliveryChangeRecalibrates`，`testGeneralBitExactWithTempo`，`testStatusFields`，`testHousePhaseHintEndToEnd`，`testCalibrationOnlyInMainSection`，`testOfflineExportWhileListeningIsFlagged`，`testHouseAudioThreadDoesNotAllocate`，`testReopenAfterReloadCarriesBlendLoss`。

ClayGlaze：

- `learnAndCalibrate` 改成等到置信度 ≥ 0.5 且已跟随画像的 `integratedLufs` 有效，不再等锁定。
- `testGlazeZeroIsBitExact` 仍要求 GLAZE 0 逐位直通和手动 30 Hz 的阳性对照。阶段改为 unlearned。
- `testLegacyProject` 改成 v1 XML 冷启动。`requestRelearn` 不提交。GLAZE 0 的手动 30 Hz 仍必须改变输出（本机最大差 0.518）。
- `testExecutionSnapshot` 改成 formatVersion 2，外层没有 EXECUTION / CALIBRATION。读回的 Clay 值与重放仍逐位核对。
- 四个单声道峰值代价不再发布（`LiveProfile.cpp` 的 `finalizeProfile` 与 `ClayTrials::costsOf`，阶段 2 Q6）。`allCostsValid` 改成低切和加宽必须有效，四个 `monoPeakCostDb` 必须无效。这是在核对已删除的行为，不是把失败改成通过。
- 手动 Heat / Glue 超预算：稳态 30 秒里执行保护为 0、反馈步数为 0、上限保持 1e9。响 8 倍的 3 秒之后，方案目标落到 0.00 dB，实际增益 0.10 dB，仍贴着目标（死区 0.3 dB）。出厂 `noMainSeconds` 是 60 秒，35 秒观察窗本来就不会把这次冲击忘掉。旧断言「自动增益保持 > 0.5 dB」是锁定方案的口径。
- 手动 Glue 从 100 收到 20：被胶水压住的峰值回来，保护层退让。本机 4 秒内最深 −0.52 dB。断言改为退让不超过 1 dB，且反馈步数与上限不变。

GlazeChain：

- `learnState` 在置信度 ≥ 0.5 且 LIVE 积分响度有效时停下。
- `RestoredChain` 先 `prepare` 再读档（见类别 c）。
- 响度阶梯：上行 0.2 dB/s。10 秒渲染里 GLAZE 50/75/100 的目标都还没走到，积分响度贴在一起（75 相对 50 为 0.00 LU，100 还低 0.01 LU）。小档仍用 10 秒，只要求不比 GLAZE 0 更安静（容差 0.02 LU）。25/50/75/100 改为同一条 40 秒素材。本机：−24.52 / −20.62（+3.90）/ −17.95（+2.67）/ −16.12（+1.84）/ −15.62（+0.50）LUFS。50 及以上真峰值 −1.35 dBTP，仍要求 ≤ Ceiling + 0.01。每档仍必须比上一档响 0.1 LU 以上。
- 有效参数逐位、最深保护 < 0、零分配、真峰值生命周期都保留。

渲染自检（`tools/render/RenderMain.cpp`）：

- `--selftest-house`「通用档给拍点逐位不变」改为：两边释放上限都是 0；同一拍点再走一遍必须逐位相同；真峰值 ≤ −1 + 0.01。本机两次哈希不同（连续编排器吃 PPQ），重复走带哈希相同，真峰值 −11.983 / −12.016 dBTP。House 124 BPM 上限 161.290 ms 的检查没动。
- `--selftest-live`：GLAZE 70 温润与 GLAZE 0 的追踪仍须逐字节相同。外接 `setLiveSegmentSink` 会把段从链的 LiveProfile 引走，所以接上追踪之后的 LIVE 存档和成品都必须与不追踪不同。两次不追踪彼此逐位相同，两次追踪彼此逐位相同，两边真峰值 −1.350 dBTP。

## (b) 锚点漂移与重生成步骤

机器：本 VM，x86_64，GCC 13.3，Release，JUCE `/JUCE` 9.0.2。日期 2026-10-08。没有新的二进制 golden。数字都是下面命令的日志。

```
cmake -S /workspace -B /tmp/celadon-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=/JUCE
cmake --build /tmp/celadon-build --target celadon_tests celadon_render -j2
/tmp/celadon-build/celadon_tests --only LiveProfile
/tmp/celadon-build/celadon_tests --only GlazeController,GlazeControllerHouse
/tmp/celadon-build/celadon_tests --only ClayGlaze
/tmp/celadon-build/celadon_tests --only GlazeChain
/tmp/celadon-build/celadon_render --selftest-house
/tmp/celadon-build/celadon_render --selftest-live
```

LiveProfile `testHouseEquivalence`：`MusicAnalyzer::startLearning` 已是空操作，候选画像与 `houseGate` 保持无效。新步骤是 `setGenre(deepHouse)`，`setLiveMode(true)` 之后再送 `makeIntroDrop(16, 16, 1.0)`、块 512、宿主 124 BPM，然后 `setLiveMode(false)`。本机存下 33 段、live trial 块 626。把这些段（不经已删除的 `withLiveTrials`）送进两个 `noForgetting()` 的 LiveProfile。两边画像与诊断逐位相同，`formatVersion == clayProfileFormatVersion`，`houseGate` 无效，代表度在 (0, 1]，置信度 > 0.5，换响度模型再换回来逐位相同。

Clay 反相提示时序。旧上界是「资格 + 分析滞后 + 4 块」。连续分析更慢，资格期下限没有放宽，收单量「从不改变」没有放宽。

| 量 | 本机测量 | 新上界 |
|---|---|---|
| 灯亮 | 362.7 ms | 资格 + 分析 + 斜坡 + 过零 + 4 块 + 0.20 s（日志上界 490.7 ms） |
| 满强度 | 565.3 ms | 灯亮上界 + attack（日志 590.7 ms） |
| 整段低频反相满强度 | 581.3 ms | 同上推导 + 0.08 s（日志 611.7 ms） |
| 灯灭 | 段落后 10.70 s（段长 2 s） | 段起点 + 14 s。尾音从 8 s 加到 16 s，否则慢相关度还没松开 |

块长。64 与 1024 各自的实时 / 离线仍然逐位相同。跨块长的 3 秒窗：跳过开头 3 秒滑移之后，最大窗差 0.02028 LU（末块加宽 0.894 / 0.874 dB），积分差 0.00726 LU。窗差上界从 0.02 放到 0.03 LU。积分响度仍要求 0.02 LU。

偏置低音低切。试用损失插值过零 12.351 Hz，实际低切 12.514 Hz。相对系数 1.0001 太紧，改为 `≤ 插值 + 0.25 Hz` 且 `< 20 Hz`。链内保护掺混损失 0.097 dB，仍要求 ≤ 0.12 dB。

## (c) 真实问题（没有改 DSP，也没有把它写成通过条件）

先读档、再做第一次 `prepare`，LIVE 会被清掉。`CeladonChain::prepare` 只在 `hasPrepared()` 时先把 LIVE 存下来。插件构造函数不 `prepare`。宿主常见顺序是 `setStateInformation` 然后 `prepareToPlay`，那一次会走到这条路径。

`testPrepareOrderLogsFirstPrepare` 的日志（本机）：

```
restore-before-first-prepare I valid 0 value 0.000; prepare-then-restore I -22.804
```

测试只断言先 prepare 再读档能逐位保留 I。先读档再第一次 prepare 只写日志，不把它当成合同。修这条要改 `CeladonChain::prepare` 或宿主读档顺序，超出本 PR。

## aarch64 专有，本 PR 未改

本机复现不了。不要为了它们放宽 x86 的逐位检查，也不要改 DSP。

| 类 | 失败 | 原因 |
|---|---:|---|
| ColorModule | 4 | L=R 与 mono 在 NEON FMA 上不逐位 |
| BalancePerformance | 1 | 立体声 / 单声道耗时比 |

Spark 上全套仍会比 x86 多这 5 条，除非另开 DSP 或计时容差的 PR。

## 本机跑不了、需要 107 / 109 / Spark 重打的基线

单元测试和两条自检都不读这些资产。阶段 2 的成品已经变了，旧的逐位或落差门槛不能直接当回归。

1. 22 首门槛：`tools/render/gate_check.py`。曲目缓存在 Spark 的 `~/.cache/celadon-t048-22songs/`（见 `REPORT-P2-INTEGRATE-SPARK.md`）。本机没有这批歌。阶段 2 应看 `confidence`，不要再等锁定。
2. 45 首 Jamendo 因果重放。阶段 1 的做法和标签在 `docs/validation/2026-10-02-T048-phase1/C-live/`（`general-analysis.md` 写明 45 首）。本机没有这批音频，也没有对应的批跑脚本输出。需要在 107 / 109 / Spark 上按阶段 2 的连续画像重跑，再决定落差门槛。

`tools/render/phase2_gate_selfcheck.py` 不在这次失败里。Spark 上一次是 12/12，它不是 22 首门槛，也不是 45 首重放。
