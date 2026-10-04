# T-056：phase_guard_pct 改叫「反相提示时长占比」（2026-10-04，DGX Spark 本机）

ADR-015 起修坯的反相保护只提示、不改声音，`phase_guard_pct` 只是收单灯琥珀亮着的时长占比。T-056 只改说明文字：
`tools/render/gate_check.py`（门槛第 3 条注释、打印表头 `guard%` → `hint%`、豁免说明与失败文字）、
`tools/render/README.md`（第 140、148、229 行）、`tools/render/RenderMain.cpp`（4 处注释 + metrics.md 的口径说明与表头）。
判定逻辑、CSV 列名 `phase_guard_pct`、C++ 标识符都不变；宽度保护仍真改声音，「宽度保护介入」照旧。

| 文件 | 内容 |
|---|---|
| `compare_gate_check.py` | 回归脚本：改前（`git show 218789a:tools/render/gate_check.py`）/ 改后对 docs/validation 下 35 份现有指标表、自造的失败分支表、`--extra` 给的表各跑一次，旧文字字面替换成新文字后 stdout 逐字节相同、返回码与 stderr 相同 |
| `compare-output.txt` | 上面脚本的输出：35 份现有表返回码 29 个 0、6 个 1；失败分支 2 条「误触发」、返回 1；0 处不一致 |
| `fail-branch-metrics.csv`、`fail-branch.{old,new}.txt` | 现有表走不到「误触发」失败分支：由 trial-3songs-metrics.csv 把 phase_guard_pct > 5 的「自动」行 slow_inverted_pct 改成 0 造出，及两版输出 |
| `make_synth_phase.py` | 24 s 合成曲（8 ～ 16 s 右声道低频反相），固定随机种子 |
| `render-after-metrics.csv` | 改前 / 改后两版 celadon_render（各编一次，0 警告）`--glaze 0,35,70 --clay auto,off --metrics-only` 渲染这首合成曲的 metrics.csv：两版逐字节相同（GLAZE 35 / 70「自动」行 phase_guard_pct 48.43） |
| `render-metrics-md.diff` | 两版 metrics.md 的差别：只有生成时间与改的两行（表头、口径说明）；stderr 日志除输出目录路径外相同（用时都是 5.6 s） |

与 T-048 分支（`claude/t048-realtime-glaze` @ `38ee761`）`git merge-tree` 模拟合并 0 冲突。本机是 aarch64；本任务不涉及数值，
不需要回 107 / 109 补跑。
