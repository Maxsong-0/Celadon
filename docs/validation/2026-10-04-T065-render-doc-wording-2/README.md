# T-065 · 渲染工具第二批说明改准（2026-10-04，DGX Spark 本机）

Max 2026-10-04 19:4x 同意把 T-059 PR 里留作待拍板的几类并进来。**只改文字与注释**，判定逻辑、`metrics.csv`、列名、C++ 标识符都不变。措辞取 T-059 核实 workflow 的 `wording-proposals.json`（第 6 项 5 条 + `pending_for_max` 的两类），`apply_wording.py` 逐条断言原文恰好出现一次才套用。

| 类 | 内容 |
|---|---|
| 代价限来源写全 | `clay_cost_limited` 有三处会置位：先试后用试出会抬高峰值（ADR-012）、低切按守护混合损失封顶（ADR-027）、压力反馈降过档（ADR-014 / ADR-028）。README:138、`RenderMain.cpp` costLimitedText 注释、metrics.md 口径同步 |
| ADR-016「逐位」口径 | 同决策重放阳性对照的判据是最大样本差 ≤ −180 dBFS、不要求逐位（`RenderMain.cpp:1675`、README:218 早已这么写）；文件头 / `--selftest` 注释 / 重放注释 / `--help` / metrics.md 句子改准，`--help` 多一行说明判据 |
| 第 6 项 `--trace` | README:14（修坯「自动」不再说成由实时保护给三项值：收单 ADR-015 起恒为 0）、README:78、新增「逐块追踪的修坯列」一节（`planned_*` / `live_*` / `guard_phase_*` / `guard_seconds` / `rho_*` / `clay_*`；不写时间常数，指向 `ClayGuard.h` 文件头）、`RenderMain.cpp` 的 `--trace` 注释与 `--help` |

## 验证

- `celadon_render` 改前（main `5620af1` 的整合树，`final-all`）/ 改后各编一次，**0 警告**。
- 合成曲（T-056 的 24 s 反相曲）`--glaze 0,35,70 --clay auto,off,mono --metrics-only`（`render_compare.sh`）：
  - `metrics.csv` **逐字节相同**（4132 字节，`render-after-metrics.csv`）；
  - `metrics.md` 只差生成时间与两处口径句子（第 52 行代价限、第 55 行重放判据，`metrics-md.diff`）；
  - `--help` 只差预期的几行（`help.diff`）：`--trace` 两行换措辞、`--no-replay` 去掉「逐位」并多一行判据；
  - `--selftest` 全过。
- 新增「逐块追踪的修坯列」里的列名与 `RenderMain.cpp` 追踪表头逐一对过（`planned_lowcut_hz` … `clay_width_db` 全在）。
- `--help` 各行显示宽度 ≤ 124 列（改前最宽）。

## 没做

- ADR-015 后常值写明（README:139 / 140 / 145、`RenderMain.cpp:2512`）：Max 这次没选，仍低优先、未做。
- n/a 判定口径与 `--clay mono` 去留：属判定 / 行为变更，另开任务。
