# 质量基线 · 2026-11-14（T-114 M4 第 1 步）

ADR-034 决定 2 的两行：同一网格（`tools/render/run_quality_baseline.sh`），同一指标脚本（`449340a` 的 `quality_metrics.py`，B9 第二项是回差拐点
`gain_reversals_per_min`），只换渲染器。DGX（aarch64）渲染，全机锁里跑。

| 目录 | 渲染器源码 | `celadon_render` sha256 | 声音 |
|---|---|---|---|
| `current/` | `3f43ea0` | `c400a7afa60f1e44e2c7c9da9af1b232a0fec94ab72fb22a1d222ad565c69ba2` | 0.1.8.11 听感批之前（T-113 的基点） |
| `base-3a7a367/` | `1452a91`（= `3a7a367` + T-096 工具，插件源码只多 `LimiterModule` 只读计数与 `getLimiterForTesting`） | `b49094616bd9a3cf0be3d586ef63d8013c5ac309f985f8d4fdfc4c31efde05d4` | 与已发布的 0.1.8.10 相同（`e0d509c` → `3a7a367` 之间没有 DSP 提交） |

- 网格：22 首门槛曲（DGX 上 20 首）× GLAZE 0 / 35 / 70，风格自动、修坯自动、`--trace`；Jamendo 45 首 × GLAZE 35 / 100，另加 `--segments-csv --bpm`。
- 两行都是先渲染（2026-11-14 08:23～08:52 UTC）、再用新口径不重渲重算指标（09:08～09:36 UTC）。`run.tsv` 是渲染那一次的记录：`commit` 原记 unknown
  （脚本在 `git archive` 出来、没有 `.git` 的源码副本里跑），入库时补上提交号；末行 `metrics_commit` 是重算用的指标脚本。DGX 路径换成了 `~`。
- 以后的候选对 `current/` 判：`python3 tools/render/quality_gate.py docs/validation/2026-10-11-T096a-metrics/baseline/2026-11-14-T114/current/quality-22.csv <新>/quality-22.csv`（45 首同理）。
  旧口径的表（有 `gain_sign_changes_per_min` 列）进门槛会报缺列。
- 结果与分析（旧口径 B9b 的量具问题、新口径下基线 → 现行还剩什么）：`docs/validation/2026-11-13-T114-release-0.1.8.11/README.md` 第 8 节。
- 渲染产物（成品、trace、segments）不入库，在 DGX `~/.cache/celadon-t114/quality/{current,base3a7}/`。
