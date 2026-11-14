# T-096a 质量指标管线（R5-01）

状态：2026-10-11 写成（T-096 第 1 步 (a)），2026-11-13 由 T-114 移植进 0.1.8.11 并按 ADR-039（删掉曲风与交付）裁剪；
**代码改完·未验证**（验收命令由主会话在 108 testbox / DGX 跑，见文末）。

## 这是什么

ADR-034 决定 6 要一套离线尺子：同一批歌、同一组设置，改算法前后各量一遍，B 组不许退步、A 组两条绝对不许越线。
本线交四样东西：

| 文件 | 作用 |
|---|---|
| [GLOSSARY.md](GLOSSARY.md) | 指标口径表：六项统一口径（PSR 下限、泵感、低频冲击、起音保留、主段、落差损失）+ B 组各项的字段 / 单位 / 阈值常量。**每个字段只在那里定义一次** |
| `tools/render/quality_metrics.py` | 渲染输出 + 源文件 → `quality.csv`（每首 × 每组设置一行）；`--selftest` 合成信号已知答案自检 |
| `tools/render/quality_gate.py` | 新 `quality.csv` 对基线 `quality.csv` 逐行判；只用标准库；`--selftest` |
| `tools/render/run_quality_baseline.sh` | 在 DGX 上把 22 首 + 45 首渲一遍、出 `quality-22.csv` / `quality-45.csv` / `run.tsv`；`--dry-run` 只打印 |

量具（真峰值、响度、短期响度 S、PSR、LRA、低频相关度、S/M）全部 `import` 自 `tools/render/measure_lib.py`（T-096c），段落标注、
逐小节标签与逐段统计的读法 `import` 自 `tools/render/section_lib.py`，这里没有第二份。Python 依赖钉在 `tools/render/requirements.txt`。

## 渲染器与限幅器的改动（插件逐位不变）

- **`metrics.csv` 阶段 2 那四列**（Q1 批准的唯一例外）：`confidence` / `current_mainness` / `cold_export` / `guard_cause` 从成品第二遍结束时的
  `getGlazeStatus()` 填（重放行与其余 GLAZE 状态列一样留空；`protection_db` 原本就有值；`status_line` 仍空）。其余列、`metrics.md`、
  成品音频一字不变。`confidence` 有数以后 `gate_check.py` / `listen_start_check.py` 自动改看 c ≥ 0.5（两份脚本本身没改）。
- **`--trace` 逐块表末尾追加六列**（旧列名与顺序不动）：`current_mainness`、`protection_db`、`guard_cause`（这一块的 Status）、
  `hard_clip_samples`、`limit_diff_energy`、`limit_in_energy`（限幅器这一块的渲染计数，按宿主块，读完清零）。没有新增命令行选项。
- **`LimiterModule`**：`BlockStats` 末尾加三项、新增 `getAndResetRenderCountersForTesting()` 与 `setRenderCountersEnabledForTesting()`。
  - 三项计数（硬钳位改了值的样本数、差值能量、入口能量）都只在 `setRenderCountersEnabledForTesting (true)` 时数（只有 `celadon_render --trace`
    打开；插件从不打开）。关着时音频线程只多一次布尔判断，硬钳位那一行的比较也跳过；开着时读的是试听差值重建用的同一份干路与逐样本 Gain，
    不写任何缓冲、不分配、不加锁；样本照旧由原来那行 `jlimit` 写，输出逐位不变。
  - 为什么另给一份累计：链在 GLAZE 活跃时按切片调限幅器，`getBlockStats()` 只剩最后一片。
- **限幅器入口**：要 `CeladonChain::getLimiterForTesting()`。渲染器在编译期探测这个入口，没有时三列留空，`quality_metrics.py` 把
  `hard_clip_samples` / `limit_diff_db` 写空、`na_reason` 写「限幅器计数未接通」，`quality_gate.py` 记 N/A；入口加上以后渲染器不用再改。
- `celadon_render --selftest` 新增三条：带 `--trace` 的成品与不追踪时逐位相同且表头以六列收尾；限幅器开 / 不开计数输出与每块统计逐位相同、
  关着时三项都是 0；Gain 0 时差值能量恰为 0、重限幅时 0 < 差值 < 入口。

## 用法

```bash
PY=~/.cache/celadon-spark/venv/bin/python   # DGX 上的 venv；108 的沙盒里按 tools/render/requirements.txt 装

# 自检（合成信号，已知答案）
$PY tools/render/quality_metrics.py --selftest
python3 tools/render/quality_gate.py --selftest

# 一次渲染的输出 → quality.csv（渲染要带 --trace）
$PY tools/render/quality_metrics.py --renders <输出目录> [<输出目录> …] --sources <源文件目录> --out quality.csv \
    [--labels-auto docs/validation/2026-10-01-T038-calibration/labels-auto.tsv \
     --labels-override docs/validation/2026-10-01-T038-calibration/labels-override.tsv] [--jobs 8]

# 判门槛：0 = 没有失败，1 = 有失败，2 = 用法或输入错
python3 tools/render/quality_gate.py <基线 quality.csv> <新 quality.csv>

# 基线（DGX，经 ~/.cache/celadon-spark/bin/spark-test.sh 的 run 子命令拿全机锁；先 --dry-run 看命令）
tools/render/run_quality_baseline.sh --dry-run <celadon_render> <输出根> ~/.cache
tools/render/run_quality_baseline.sh <celadon_render> <输出根> ~/.cache
```

`run_quality_baseline.sh` 的网格：22 首 `--glaze 0,35,70 --style auto --clay auto --trace`；45 首 `--glaze 35,100 --style auto --clay auto --trace`，
另加 `--bpm`（`tempo-manifest.tsv`）与 `--segments-csv`（逐小节统计：落差走段落口径、主段核对才算得出）。原来 22 首的流媒体 / 俱乐部那一遍、
45 首按子类给的 `--genre` 与三档 `--delivery` 随 ADR-039 去掉。重放行照默认保留（门槛脚本要它），`quality_metrics.py` 跳过。
输出根必须不存在或为空，脚本不删任何东西。

## `quality.csv` 字段

配对键 `song, kind, glaze, clay, style`（原来还有 `genre, delivery`，ADR-039 删了）。公式全在 GLOSSARY。

| 组 | 字段 |
|---|---|
| 身份 | `file`、`source_file`、`sample_rate`、`duration_s`、`align_gain_db` |
| 主段 ⑤ | `main_s`、`main_pct`；有标签与 `bpm` 时 `main_in_drop_pct`、`drop_in_main_pct` |
| A 组 | `true_peak_dbtp`（照抄 `metrics.csv`）、`ceiling_dbtp`、`hard_clip_samples` |
| B1 | `main_level_err_lu`（只报；drop 目标那一项随交付删了） |
| B2 | `drop_loss_lu` + `drop_loss_method`（`sections` / `p90_p25`）、`lra_in_lu` / `lra_out_lu` / `lra_delta_lu` |
| B3 | `psr_main_med_in_db` / `psr_main_med_out_db`、`psr_floor_spec_db`（8.5） |
| B4 | `pump_db`；排查子表 `p_slow_db`、`d_fast_db` |
| B5 / ④ | `lf_crest_in_db` / `lf_crest_out_db` / `lf_crest_db`；`attack_keep_db`、`attack_onsets`（只报） |
| B6 | `limit_diff_db` |
| B7 | `low_corr_in` / `low_corr_out` / `low_corr_delta`、`side_mid_delta_p95_db` |
| B8 | `net_spectrum_p95_db`、`net_spectrum_bands_db`（27 段，分号分隔，只报） |
| B9 | `gain_tv_db_per_min`、`gain_reversals_per_min`（回差 0.1 dB 的拐点；2026-11-14 前是逐块变号）；`guard_engaged_pct`、`guard_cause_hist`（只报） |
| | `na_reason`：空值的原因，多条用「; 」连接 |

写法：定点小数；算不出写空（绝不填 0）；`-inf` / `inf` 原样写（如纯增益时 `d_fast_db`）；`-0.000` 写成 `0.000`；按配对键排序。

## 出错约定（Max 的 Q12）

- **基础设施错** → 打印原因、**不写** `quality.csv`、退出码 1：`metrics.csv` / 成品 / trace / 源文件读不到，缺列，配对键重复，
  采样率或长度对不上，`file` 列为空（`--metrics-only`），trace 的块拟合不上。
- **素材原因** → 空值 + `na_reason`：没有主段、不足 3 s、没有高频、没有标注、门后不足 60 s……
- 两个脚本都只读输入，`quality_metrics.py` 只写 `--out`，`quality_gate.py` 只往标准输出写；不改文件、不回退增益。

## 基线入库

- 跑完把 `quality-22.csv`、`quality-45.csv`、`run.tsv` 复制进仓库 `docs/validation/2026-10-11-T096a-metrics/baseline/`（按需加日期子目录），
  音频（成品、trace、segments）留在 DGX 的输出根里，不进仓库。`run.tsv` 记提交号、`celadon_render` 的 sha256、网格参数与起止时间。
- 以后改算法：同一网格跑一遍新 `quality.csv`，`quality_gate.py baseline/quality-22.csv new/quality-22.csv`（45 首同理）。
  报告里「已知」（K）= 基线同一行就越线、这次没更差；「汇总」一行把 A 组越线、B 组退步、缺行分开数。

## 已知限制

- 限幅器三列要等链的入口（见上）；之前 B6 与 A2 全是 N/A。
- 22 首没有段落标签：落差走 `p90_p25`；不给 `--bpm` / `--segments-csv` 时指标表没有方案目标列，`main_level_err_lu` 空。
- `p_slow_db` / `d_fast_db` 是块率近似（48 k / 512 ≈ 94 Hz），逐样本版要等 R2-01 测试钩子。
- 两份能量在宿主旁通 / 对比原声时也照湿路算（渲染器从不走这两条路）。

## 验收命令

由主会话统一跑（先 108 testbox，其次 DGX `spark-test.sh`）：`celadon_render --selftest` / `--selftest-transport` 仍过；不带新选项时
`metrics.csv` 除 `confidence` / `current_mainness` / `cold_export` / `guard_cause` 四列外逐格相同、`audio_hash` 全同；
`quality_metrics.py --selftest` 全 ok；`quality_gate.py --selftest` 全 ok；跑完基线后 `quality_gate.py` 基线对基线 B 组 0 退步。
