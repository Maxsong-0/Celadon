# T-048 阶段 2 集成笔记

集成分支 `cursor/t048-phase2-integrate-7e50`，基线是 main 的 T-078（`aac62d1`，PR #55）。

只合入四份草稿，顺序 A → B → C → D：

- A #61 `cursor/t048-phase2-agent-a-fe57`：连续编排器、LIVE、删校准器
- B #60 `cursor/t048-phase2-b-analyzer-08ef`：退役聆听 / 候选画像 / House 重算，抽出 `ProfileMath.h`
- C #59 `cursor/t048-phase2-c-ui-7a14`：七条状态行，去掉重新聆听和橙点
- D #58 `cursor/t048-phase2-d-0f38`：`Phase2Contract` 与渲染门脚本

没有合入 #62（`cursor/t048-phase2-agent-a-498e`）和 #57 交接。两份都是中断代理留下的重复草稿，已经关闭。

权威是 `INTERFACE-SKETCH.md` 与已锁定的 Q1–Q12 默认。没有 Mac 构建，没有部署，没有跑 22 首 `gate_check.py`。

## 冲突怎么取的

- `CMakeLists.txt`：dsp 源文件用 A+B+C（`ContrastGuard.cpp`、`OutputLevelTracker.cpp`、`PsrGuard.cpp`，不再编 `GlazeCalibrator.cpp`）。D 只补两份测试：`tests/GlazeCalibrationTests.cpp`、`tests/Phase2OrchestratorTests.cpp`。整份改用 D 会把校准器编回来、丢掉三个守门。
- `tests/GlazeStatusTextTests.cpp`：用 C 的七条文案。`readingFromStatus` 改读真实字段（`confidence`、`currentMainness`、`guardCause`、`guardEngaged`）。`planLimited` 不再代替守门。
- `tests/MusicAnalyzerTests.cpp`：反相超低频那段注释用 B（ADR-015 起自动收单恒为中性）。
- `tests/Phase2Contract.h` 的 `minGuard`：跟 A 的 `publishStatus`。更负的退让胜出；平局留下先到的原因；`engaged` 取或，不改原因；退让为 0 则原因是 `none`。
- 死区名字和生产实现反着，行为对齐草图：`|误差| < 0.1 dB` 进入，`|误差| > 0.3 dB` 离开。增益上行 0.2 dB/s，下行 1 dB/s。棘轮只压净增益（`m < 0.5`）。

## 实现取舍

- T-078 留下：`publishedEffectiveCeiling`、`MasteringStatus::effectiveCeilingDb`、芯片「待校准 / Awaiting」。七条状态行不用这枚芯片。
- 守门速率是已经落地的 T-047，不是草图里当起点写的 1 / 0.3 dB/s。超限 0→3 dB 对应撤退 0.3→6 dB/s；深度 0→地板对应恢复 0.15→0.3 dB/s；地板 −3 dB。`isEngaged()` 是正在退，或退到底仍超。回程是 `isRecovering()`，不算 engaged。
- 外部 `setLiveSegmentSink` 是测试探针，会把段从链上的 `LiveProfile` 引走，所以开探针和关探针的链输出不是逐位相同。产品路径一直走链上的那一份画像（Q2）。`prepare` 时分析器先把开着的段交给探针，编排器再卸掉探针，所以探针计数会多 1。宿主 `reset` 不卸探针。`requestRelearn` 是空函数。`getPhaseForTesting()` 停在 `unlearned`。`calibrationValid` 保持 false。
- Guard 把宿主大块切成约 10 ms 时隙。`SegmentLedger::anchor` 要求后几片的 PPQ 已经按片内偏移推过，小节起点仍是块起点那一小节。A 原来只切片、不推位置：误差累积超过 0.05 s 容差后会按块起点的 PPQ 重新锚定，4096 样本的实时段就和 512 对不上（7 / 32 / 480 不切片，512 的 32 样本尾仍在容差内）。现已在 `CeladonChain::process` 里对偏移 > 0 的片推进 `positionSamples` 与 `ppqPosition`，直接交给编排器，不重走链上的离线 / 预听开关。多片走完后把宿主补到块尾，下一块若没再 `setHostInfo` 就从块尾接着走。频谱仍每宿主块读一次，在切片之前。

## 这次跑过的测试

Linux，`/tmp/celadon-build` 的 Release `celadon_tests`。`tools/render/phase2_gate_selfcheck.py` 12 项通过。

- `MusicAnalyzerLive`：148 通过，0 失败。块长 7 / 32 / 480 / 512 / 4096 的实时段逐字段相同。prepare 先交出开着的段再卸探针。
- 回归切片：15722 通过，0 失败。类是 `Phase2Orchestrator`、`GlazeStatusText`、`GlazeCalibration`、`GlazePlanner`、`HelpText`、`HelpTextClay`、`HelpTextHouse`、`Localisation`、`AdvancedMastering`、`AdvancedMasteringChain`、`SegmentLedger`、`LegacyFixture`、`GlazeBehaviourReport`、`MusicAnalyzer`、`MusicAnalyzerClay`、`MusicAnalyzerHouse`、`CeladonChain`、`CeladonChainClay`、`CeladonChainMonitoring`、`CeladonChainGuards`。
- `CeladonChainMonitoring` 里 NaN 那一条：IN 表仍会恢复。`listenedSeconds` 停在 0，阶段 2 不再累计聆听时长。
- `LiveProfile`：除 House 等价锚点外，同一次运行里的用例都跑完。锚点 `testHouseEquivalence` 30 条失败（House 候选画像与门控账已退役，存下的段是 0）。空账时参照用 `energy / total` 与 `(gated - 1)`，会走出数组。现改成这条失败，不再把整个测试进程打崩（原先 SIGSEGV）。

没有跑、也不该在这次当成绿：

- `GlazeController`、`GlazeControllerHouse`、`ClayGlaze`、`GlazeChain`。驱动还在等聆听 / 锁定 / 校准状态机（`learnAndCalibrate` 一类等待可达 40 s）。D 只加了指向契约的注释。阶段 1 断言还在，需要按 `Phase2Contract` 重写，规模是整份测试类。
- `GlazeBehaviourReport` 不再要求提交一份聆听画像，因此也不再打出按风格的响度 / GR 表。输入响度仍会记。这一类本身是绿的。
- `MusicAnalyzerTests.cpp` 有四个未使用函数的警告（`referenceIntegrated`、`referenceLoudnessRange`、`meanSquaresOf`、`sameLedgerState`），不是错误。

## 留给父任务 / Spark / 107 / 109

- Q4：换曲风时 `LiveProfile::setConstants` 仍是消息线程上的同步重算。A 只记下 `pendingModel`，在 `prepare`（音频已停）时调用。没有把模型切换折进 `advanceTo` 分片，也没有改 `LiveProfile.h` 那句契约。不要在音频线程上半接。
- `ProfileMath.h` 已抽出，分析器在用。`LiveProfile.cpp` 仍留着一份抄本。有限输入上 `firstBinAtOrAbove` 逐位相同；抄本把 NaN 当最低格（`!(position > 0)`），头文件用 `position <= 0`。部分常数类型也不同（例如 `minimumLraValues`、`minimumOnsetEvents` 在抄本里是 `double`）。集成时若改 include，要先决定 NaN 兜底留哪一式。这次没改发布语义。
- Q12：`HouseConstants` 的锁定 / 重开字段留着。`constantsVersion` 只作诊断，不参与计算。
- `LiveProfile` 的 House 等价锚点要按退役后的实时账重写，或从全套里拿掉。现在它失败，但不再把进程打崩。
- 全套 `celadon_tests`、22 首 `gate_check.py`、合成曲落差、真编排器因果重放、Mac 构建、部署、Spark / 107 / 109，都不在这次。
