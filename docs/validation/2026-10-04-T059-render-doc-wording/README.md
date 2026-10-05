# T-059 · 渲染工具里 ADR-015 后不准的说明（2026-10-04，DGX Spark 本机）

ADR-015 起修坯「自动」不再收单，但 `tools/render` 里还有几处说明停在旧口径（T-056 只改了 `phase_guard_pct` 的叫法）。**只改说明文字**：`metrics.csv`、判定逻辑、列名、C++ 标识符都不变。

## 改了什么（核实 workflow `wf_8b34caaf-a79` 的 11 条「确定要改」，脚本 `apply_wording.py` 逐条断言 `current` 恰好出现一次才套用）

| 文件 | 内容 |
|---|---|
| `gate_check.py` 第 9 / 149 行 | `replay_check = n/a` 不再写成「自动收单比例在 0、1 之间」：n/a 有两种原因（收单比例落在 (0, 1)、目标频谱用了「参考」形状），gate_check 只读 CSV 分不出，改指向 metrics.md 该曲备注；ADR-015 起前一种不会出现，工具的目标频谱固定是出厂「自动」，后一种也走不到 |
| `README.md` 219-220 | 补「原因写进备注」、两种原因眼下都走不到 |
| `README.md` 374 盲听指引 | 删掉被 ADR-015 推翻的两条旧听点（低频更紧、单声道回放更稳），只写事实、**不立新听点** |
| `README.md` 138 | metrics.md 没有「代价限」列：改成「反相提示 / 宽度保护」格末的「，代价限：…」，ADR-015 起不再出现 `mono` |
| `README.md` 73、`RenderMain.cpp` 148（注释）、484（`--help`） | `--clay mono` 一组的成品与 off 逐位相同，只多记反相提示时长（同根漏网） |
| `RenderMain.cpp` 2500、2537 | 写进 metrics.md 的「0.3 s 余振」实为 0.35 s（`CeladonChain::filterRingOutSeconds`）：写死 0.35 |
| `RenderMain.cpp` 2513 | 口径一节补一句「代价限」的含义 |

## 验证

- **gate_check**（`compare_gate_check.py`，输出 `compare-output.txt`）：改前（main `c4a804c`）/ 改后对 docs/validation 下 **37 份**现有指标表（返回码 30 个 0、7 个 1）、自造的 n/a 分支表（24 行 n/a，两版都给 24 条说明、返回码 0 → 0）、自造的「误触发」失败分支表（2 条、返回 1）各跑一次，
  把旧文字字面换成新文字后 stdout 逐字节相同、返回码与 stderr 相同；并核对改前输出本来不含新文字。**0 处不一致**。
- **渲染**（`render_compare.sh`，输出 `render-compare-output.txt`）：改前（main）/ 改后两版 `celadon_render`（各编一次，0 警告）渲染 T-056 的 24 s 合成曲（8 ～ 16 s 右声道低频反相）
  `--glaze 0,35,70 --clay auto,off,mono --metrics-only`：
  - `metrics.csv` **逐字节相同**（4132 字节，`render-after-metrics.csv`）；
  - `metrics.md` 只差生成时间与改的三行（两处 0.35 s、口径一节追加的一句，`metrics-md.diff`）；stderr 除用时（7.7 / 7.6 s）外相同；
  - `--help` 只多一行（484 之后那行说明）；
  - 核实结论在这首曲上复现：`mono` 组与 `off` 组 `audio_hash` 相同（GLAZE 0 / 35 / 70），`mono` 组 `phase_guard_pct` 与 `auto` 组相同（GLAZE 35 / 70 都是 48.43）。
- 与 T-048 分支（`claude/t048-realtime-glaze` @ `38ee761`）核实 workflow 已用 `git merge-tree` 双向模拟 0 冲突（三份文件的改动与 T-048 不重叠）。

## 没做（待 Max，清单与建议见 `wording-proposals.json` 的 `pending_for_max`）

- 「第 6 项」`--trace` 的 `guard_phase_*` 说明与 README:14「实时保护给值」（Max 原话「先问」，5 处措辞已备好）。
- 0.35 改成由 `ringOutSeconds` 生成（根除常数与文字漂移，但改的是拼字符串的表达式，不算「只改文字」）。
- 代价位来源写全（先试后用 / 守护混合损失封顶 / 压力反馈降档三处会置位，T-043 / T-044 留下的另一根因）、ADR-016「逐位」口径四处、ADR-015 后常值写明三处（低优先）。
- n/a 的判定口径（出现「参考」原因时门槛混进目标频谱差别；以后出现 n/a 判失败还是只列说明）、`--clay mono` 的去留：属行为 / 判定变更，另开任务。
- README:9 的选歌要求「至少一首低音很宽 / 有反相内容」留不留。
