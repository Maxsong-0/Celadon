# T-098 清理批（T-114 第二波 L4）

2026-11-13。基点 `f4741f5`（集成分支，第一波四条线合入后），本批在 T-099 拆锁提交 `d9a2628` 之上。
只删死代码、改说明、把一个读错了来源的门槛输入接回真量；**不改任何声音**：GLAZE 0 与 GLAZE > 0 的成品都应与基点逐位相同
（`audio_hash` 全同），插件存档格式不变。本线不编译、不跑测试，验收命令交主会话在 108 / DGX 统一跑（第 5 节）。

## 1. 改了什么（按 T-098 条目）

| 条 | 做了什么 | 文件 |
|---|---|---|
| 2.1 | 删「重新聆听」整条空接口：`GlazeController::requestRelearn`、`CeladonChain::requestGlazeRelearn`、`CeladonAudioProcessor::requestGlazeRelearn`、编辑器的 `GlazeRelearnAction` / `setGlazeRelearnAction` 与 createEditor 里注入它的那一行；测试里调它的地方一起删（阶段 2 起本来就是空操作） | `Source/dsp/GlazeController.*`、`CeladonChain.h`、`Source/PluginProcessor.*`、`Source/PluginEditor.*`（只动这几行）、`tests/*` |
| 2.2 | 删探针 `TestProbe` 里阶段 2 起恒为初值的 49 个字段与 `getPhaseForTesting`（慢路 / 快路低频相关度两项改成现取，见第 2 节）；编排器原来借 `probe.transitionActive` 存「这一块参数变了」，改成自己的成员 `planningChanged`（行为不变）；删只读这些字段的测试断言与日志；`tests/GlazeControllerTests.cpp` 删掉 T-048 起就不在 `runTest` 里的阶段 1 用例（约 4400 行）与只给它们用的辅助函数 | `GlazeController.*`、`tests/GlazeControllerTests.cpp`、`GlazeTests.cpp`、`ClayGlazeTests.cpp`、`MusicAnalyzerTests.cpp` |
| 2.3 | `--trace` 删 22 个恒为初值的列；`slow_inverted_pct` 改读真量（第 2 节）；指标表删 `clay_peak_cost_db` / `clay_peak_cost_s` | `tools/render/RenderMain.cpp`、`README.md` |
| 2.4 | 删旧校准参考条件哈希（`Fnv1a`、`emitConditions` 含「Mast」段、`HashSink`、`WordRecorder`、候选码搜索、`setManualConditions`、`upgradeLegacyConditions`）与方案过渡的 `blendClayPlans`：插件、渲染工具、跑着的测试都不调。`CalibrationReference` 结构与 GlazeProfile 的读写留着（旧存档格式） | `GlazeController.*`、`GlazeTypes.h` |
| 2.5 | 注释与工具文字里指「第一遍 / 开头那一段」的「聆听」改「预热」；指阶段 1 学习 / 锁定状态机、旧按钮「重新聆听」的照旧（历史名字）；引用界面旧工程提示原文的注释改成不引原文 | `tools/render/*`、`Source/PluginProcessor.cpp`、`GlazeController.h`、`PluginEditor.h`（重新聆听那句）、测试注释 |
| 2.6 | `--help` 补上 `--help`、`--no-align` 对齐、`--trace` 的说明改成现行列、`--live-trace` 写明会改存档与成品、「聆听」改「预热」；README 选项表补 `--no-align` / `--help` | `RenderMain.cpp`、`tools/render/README.md` |
| 2.7 | `ColorModule.h` 补 Heat 残差 makeup 系数的量级（按标定公式复算：Heat 0 ≈ 1.00396、25 ≈ 1.00617、50 ≈ 1.00887、75 ≈ 1.01204、100 ≈ 1.01567） | `Source/dsp/ColorModule.h` |
| 2.9 | GCC 15 `-Wunused-function`：删 `MusicAnalyzerTests.cpp` 的 `referenceIntegrated`、`referenceLoudnessRange`、`meanSquaresOf`、`sameLedgerState`（连带只被它调的 `sameEvent`）；同类扫描（测试与渲染工具里 4 / 8 格缩进的自由函数）另找到并删了 `GlazeControllerTests.cpp` 的 `withClayNeeds`（2.2 删用例后成了孤儿） | 同左 |
| 2.10 | T-085 两份文档开头加指针句：`codec_tp_check.py` 已随 T-108 删除（`93af3e0`），通用量具在 `measure_lib.py` | `docs/validation/2026-10-08-T085-codec-tp/REPORT.md`、`RESULTS.md` |
| 2.4 / 2.8 | vault 规格与 ADR 的改动只写成草稿交主会话（本线不碰 vault） | — |

T-099 的修前演示补丁 `docs/validation/2026-11-13-T099-state-lock/remove-state-lock.patch` 按本批改过行号的 `PluginProcessor.cpp` 重生成（仍是那三处锁）。

## 2. 反相豁免读错来源（2.3 的根因）

`gate_check.py` 的修坯门槛：反相提示时长占比 `phase_guard_pct` > 5 % 时，若 `slow_inverted_pct` > 0（歌里有真反相段）就豁免，否则判「误触发」。
`slow_inverted_pct` 由渲染工具逐块读探针的 `liveLowCorrelation`（分析器实时视图的慢路低频相关度）统计 ρ < −0.2 的块占比。
T-048 阶段 2 起编排器不再往探针里抄这一项，它恒无效 → `slow_inverted_pct` 恒为 0 → 豁免从不生效；
集成树 20 首网格上 7 首 × GLAZE 35 / 70 / 100 共 21 条「误触发」FAIL 就是这么来的。

修法：探针在 `probeForTesting` 里现取 `analyzer.getShortTermView()` 的 `liveLowCorrelation` / `liveLowCorrelationFast`——ClayGuard 的反相提示在下一块
`beginBlock`（`updateClay`）读的正是这份视图，两次处理之间它不变，所以渲染工具读到的与 ClayGuard 用的是同一个量。阈值从写死的 −0.2 改读
`factoryConstants().clayGuardPhaseCorrelation`（同值）。音频路径一行没动。

- **门槛判据没改**（仍是「> 5 % 且 slow_inverted_pct = 0 才算误触发」），只是输入从恒 0 变成真量。
- **21 条 FAIL 的预期**：歌里确有慢路 ρ < −0.2 的段的，`slow_inverted_pct` > 0，转为豁免（NOTE）；真没有反相段、提示却亮了 > 5 % 的，
  照旧 FAIL（那才是真误触发，要另查 ClayGuard）。具体几条转豁免要 DGX 重跑才知道，按第 5 节对比后填进本节。
- 连带：`--trace` 的 `rho_slow` / `rho_fast` 两列也变成真值（原先恒空）；`ClayGlazeTests` 第 11 条「pad 漏进来的低频不让慢路相关度有效」
  的断言原先是空转（读的就是这个恒无效字段），现在真的在核对（见第 4 节风险）。

## 3. 删掉 / 改接的列

| 表 | 删掉 | 改接 | 不动 |
|---|---|---|---|
| `trace/<成品名>.csv`（第二遍） | `guard_seconds`、`clay_peak_cost_db`、`clay_peak_cost_blocks`、`clay_blend_loss_db`，加下面 18 列 | `rho_slow` / `rho_fast`（恒空 → 真值） | 其余；T-096 追加的六列仍在最后 |
| 两份追踪共有 | `glaze_phase`、`cal_round_active`、`cal_state`、`cal_interrupt_mask`、`cal_gate_mask`、`cal_rounds_done`、`cal_subwindows`、`cal_subwindow_s`、`cal_guard_s`、`cal_last_error_db`、`cal_suggested_db`、`cal_applicable`、`plan_transition`、`plan_transitions`、`cal_begins`、`cal_finishes`、`feedback_steps`、`feedback_max_db` | — | `cal_target_db`、`applied_bias_db`、`executed_bias_db`、`guard_db` |
| `metrics.csv` | `clay_peak_cost_db`、`clay_peak_cost_s` | `slow_inverted_pct`（恒 0 → 真值） | 其余列与次序 |

逐个核对过读列的脚本：`gate_check.py`（`glaze_phase`、`calibration_valid`、`phase_guard_pct`、`slow_inverted_pct`、`replay_check`、`music_seconds`、
`lufs`、`limit_input_peak_db` 等，都还在）、`listen_start_check.py`、`quality_gate.py`、`quality_metrics.py`（`TRACE_REQUIRED` 的 9 列都还在）、
`master_check.py`、`phase2_gate_selfcheck.py`（自己造表）——没有一个读删掉的列。`metrics.csv` 的 `glaze_phase` / `calibration_valid` / `listen_phase`
（Status 的阶段 1 字段）这次没删：`gate_check.py` 对旧表还在用它们，且它们来自 Status（界面线也在读），见第 4 节。

## 4. 没做 / 交出去的

- Status 里阶段 2 起恒为初值的字段（`phase`、`profileReady`、`planReady`、`calibrationValid`、`listenedSeconds`、`requiredSeconds`、
  `calibrationPaused`、`relearnInProgress`、`unlearnedOfflineWarning`、`savedPlanFromOlderAlgorithm`、`profileMismatch`、`waitingForPlayback`、
  `clayNeedsRelearn`）：`PluginEditor.cpp`、`HelpTextTests`、`tools/perf/*.mm`、`Phase2OrchestratorTests` 都在读，不在本线写域。
- 编排器里 Clay 压力反馈的代价、守护混合损失的平滑值、ClayGuard 每块推进的秒数并没有死，只是探针不再抄；按任务书删列，没有恢复抄写。
- `Source/ParameterRanges.h:13-14` 的注释还指向已删的 `setManualConditions`（不在本线写域）→ T-114 集成时主会话已改成现状。
- 渲染工具 `listenNote`：GLAZE > 0 的每一组都会在 metrics.md 备注里记「第一遍没走到已锁定」（阶段 2 的 Phase 恒为 unlearned），是噪声
  → T-114 集成时主会话删掉（同本批死列一类；`tools/render/README.md` 的说明一并改）。
- 历史文档（`docs/handoff/`、`docs/one-knob-mastering-review.md`、`docs/validation/` 下的旧记录）里的「聆听」与旧接口名照旧，是当时的记录。

## 5. 验收（主会话统一跑；本线没编译、没跑）

| 什么 | 在哪 | 期望 |
|---|---|---|
| `celadon_tests` 全量（x86-64-v3） | 108 testbox `run-ci.sh base` | 相对基点不新增失败；`ClayGlazeTests` 第 11 条若新败，是断言恢复了牙齿（第 2 节），先看慢路 ρ 的读数再定 |
| `celadon_processor_tests` release / tsan / tsan-before / bench / digest | 108 `run-processor-tests.sh <模式>` | 同 T-099 记录第 5 节；`digest` 与基点逐行相同 |
| `celadon_render --selftest` / `--selftest-transport` / `--selftest-live` | 108 或 DGX | 全过（`--trace 不改成品、末尾追加列` 那一项核对表头以六列收尾、字段数齐） |
| 20 首网格 `batch.sh … --glaze 0,35,70,100 --style auto --clay auto,off --trace` | DGX | `audio_hash` 与 `~/.cache/celadon-t114/base/songs/metrics.csv` 全同；`metrics.csv` 除删掉的两列、`slow_inverted_pct` 外逐格相同 |
| `gate_check.py` 重跑 | DGX，同一份 metrics.csv | 原 21 条反相「误触发」FAIL 里有真反相段的转 NOTE，其余照旧；不应出现新类别的 FAIL |
| T-096 Python 自检、ChainDigest 7 场景 | 108 / DGX | 与基点相同 |
