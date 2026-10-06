# T-048 阶段 2 · Spark 验证：合入草稿 #63（main `23e0185`）

> 时间：2026-10-06 07:22～07:34（本机 PDT）。代理在 max-spark（DGX aarch64）上跑。
> 开工 tip（PR 分支）`0d102c7`；Max 已将 #63 合入 main，验证对照 tip **`23e0185`**（与 `0d102c7` **树哈希相同** `7f2e855…`，内容一致）。
> 工作树：`.claude/worktrees/t048-phase2-integrate-spark`。未跑 `deploy.sh`、未 Mac / xcodebuild、未再合并。
> 证据：`docs/validation/2026-10-05-T048-phase2/P2-spark/`；完整日志在 `~/.cache/celadon-spark/logs/p2-integrate-*.log` 与 `p2-remote/`。

## 0. 结论

| 项 | 结果 |
|---|---|
| 1 Linux/Spark 编译 | **通过**：`celadon_tests`、`celadon_render`、`celadon_ui_snapshot`。5 条 `-Wunused-function`（`MusicAnalyzerTests` 4 + `SnapshotMain` 1），与 INTEGRATION-NOTES 一致，**无新增错误** |
| 2 全套 `celadon_tests`（Spark aarch64） | **136062 过 / 443 败**。其中 **438 = CI 同款已知阶段 2 陈旧**；**+5 = 本机 aarch64 专有**（Color NEON 4 + BalancePerformance 1）。**新增单元测试回归：0** |
| 3 `phase2_gate_selfcheck.py` | **12 / 12 通过** |
| 4 22 首 `gate_check.py` | **未跑**（合入后优先对照单元矩阵与自检；曲目资产仍在 `~/.cache/celadon-t048-22songs/`，可后续补） |
| 5 107 / 109 | **107 x86** 与 **109 MSVC（Wine）** 各 **136320 过 / 438 败**，与 PR CI 基线数字一致。本会话未再跑 107 `-march=x86-64-v3` / 109 原生 x86（PR CI 的 v3 已是同款 438；main 推送 CI 另计） |
| 6 渲染自检 | `--selftest` **全过**；`--selftest-house` **1 败**；`--selftest-live` **2 败**。属阶段 2 连续实时相对阶段 1「聆听→锁定」**逐位不变**口径的预期语义差，见 §4。不是上述 438 之外的新单元失败 |
| **给 Max** | #63 **已经在 main**。Spark / 107 / 109 未发现超出「House 锚点 + 聆听/锁定陈旧套件 + aarch64 已知 5 条」的新单元回归。后续应重写陈旧套件与相关自检，而不是把 438 当合入阻挡 |

## 1. 环境

- 机器：NVIDIA DGX Spark，`max-spark`，aarch64，Ubuntu 24.04，GCC 13，Release + Ninja
- 构建：`~/.cache/celadon-spark/bin/spark-test.sh` → `build/t048-phase2-integrate-spark-81eec6fb1d`
- 远端：`scripts/test-linux.sh` → 107（githubci）；`scripts/windows-wine.sh test` → 109（github-2 MSVC）
- 验证对象树：`23e0185` ≡ `0d102c7`（merge 父母：`aac62d1` + `0d102c7`）

## 2. 单元失败对照（核心）

### 2.1 总数

| 平台 | 通过 | 失败 | 说明 |
|---|---:|---:|---|
| PR CI 基线 x86（run 37390408873） | 136320 | **438** | Max 合入前已红 |
| Spark aarch64 | 136062 | **443** | 438 + 5 aarch64 |
| 107 x86 | 136320 | **438** | 与 CI 一致 |
| 109 MSVC | 136320 | **438** | 与 CI 一致 |

### 2.2 Spark 443 按类（`P2-spark/spark-fail-by-class.tsv`）

| 类 | 断言失败 | 归类 |
|---|---:|---|
| GlazeController | 294 | **已知陈旧**：仍等聆听 / 锁定 / 校准状态机 |
| GlazeControllerHouse | 58 | **已知陈旧**（同上） |
| ClayGlaze | 29 | **已知陈旧**（同上） |
| GlazeChain | 27 | **已知陈旧**（同上） |
| LiveProfile | 30 | **已知**：House 等价锚点 `testHouseEquivalence`（门控账退役，段数 0）；单用例多断言 |
| ColorModule | 4 | **已知 aarch64**：L=R 与 mono 逐位（NEON FMA） |
| BalancePerformance | 1 | **已知 aarch64**：立体声 / 单声道耗时比 |
| **其它类** | **0** | **无新回归** |

CI / 107 / 109 的 438 = 上表去掉 ColorModule 4 + BalancePerformance 1。桶一致：

- 陈旧聆听/锁定：294+58+29+27 = **408**
- House 锚点：**30**
- 合计非 aarch64：**438**

与 INTEGRATION-NOTES / PR 正文「LiveProfile ~30；GlazeController / House / ClayGlaze / GlazeChain 未当绿」一致。

### 2.3 仍绿、且阶段 2 相关的切片（抽样）

全套日志里下列类**没有**进入失败表（与合入说明中的切片一致）：`Phase2Orchestrator`、`GlazeStatusText`、`GlazeCalibration`、`MusicAnalyzerLive`、`HelpText*`、`Localisation`、`AdvancedMastering*`、`SegmentLedger`、`CeladonChain*` 等。T-078 字段（`publishedEffectiveCeiling` 等）未在本跑中表现为新红。

## 3. 门脚本

`tools/render/phase2_gate_selfcheck.py`（venv Python）：**12 项全过**（阶段 1 锁表兼容 + 连续编排 `confidence` 路径）。日志：`P2-spark/p2-integrate-gate-selfcheck.log`。

## 4. 渲染自检（诚实记录：与阶段 1 逐位口径不同）

命令：`celadon_render --selftest` / `--selftest-house` / `--selftest-live`。

| 套件 | 结果 | 说明 |
|---|---|---|
| `--selftest` | 全过 | 指标、哈希、GLAZE 0 直通、重放对照 |
| `--selftest-house` | **1 败** | 「通用档给拍点逐位不变：给了 --bpm 成品却变了」；慢段释放上限仍为 0（期望满足）。连续编排在通用档也会吃宿主拍点/PPQ，阶段 1「通用 + bpm 成品逐位不变」不再成立 |
| `--selftest-live` | **2 败** | 「与 GLAZE、风格无关…接了追踪存档却变了」；「追踪不改链的输出：接了追踪成品却变了」。信息行：第二遍追踪仅歌尾段与第一遍不同。阶段 2 连续 LIVE 路径下，接追踪探针会引走段（INTEGRATION-NOTES：外部 `setLiveSegmentSink`），**不能再要求与未接探针逐位相同** |

这些是阶段 2 产品语义相对阶段 1 自检断言的漂移，应改自检 / 合同，而不是当作「合入引入的隐蔽单元回归」。音频线程与块长对齐：单元侧 `MusicAnalyzerLive` 块长 7/32/480/512/4096 仍绿。

## 5. 未做 / 留给后续

- 22 首门槛批 + `gate_check.py`（资产在 Spark：`~/.cache/celadon-t048-22songs/`，网格可参考阶段 1 `--glaze 0,35,70 --style auto --clay auto,off --metrics-only`；阶段 2 应看 `confidence` 列）
- 107 `-march=x86-64-v3`、109 再跑一份原生 x86（本会话已有 107 默认 + 109 MSVC；CI v3 已覆盖同款 438）
- 重写 `GlazeController*` / `ClayGlaze` / `GlazeChain` 与 `LiveProfile` House 锚点
- 按阶段 2 改 `--selftest-house` / `--selftest-live` 的逐位断言
- Mac、部署（本任务禁止）

## 6. 建议

1. **合入**：已完成（main `23e0185`）。Spark 验证**支持**「红 CI 可合」的判断：红的是已知合同陈旧，不是新逻辑炸穿。
2. **不要**把 438/443 清零当作发版门槛，除非先重写测试。
3. **下一步优先**：陈旧套件按 `Phase2Contract` 重写；自检去掉「探针不改声 / 通用+bpm 逐位」；可选补 22 首 `gate_check`（认 `c ≥ 0.5`）。
4. 本代理**未部署**。

