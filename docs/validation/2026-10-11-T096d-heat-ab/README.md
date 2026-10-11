# T-096d：自动 Heat 底量 A/B 对照

T-093 研究 R6-03 的问题：GLAZE 自动给的那一小份 Heat 底量（风格常数 12 × GLAZE × κ，补偿要求负 Gain 时撤回）在成品里到底听不听得见？
听不见就直接走 2a；听得见（或量不准）就交 ABX。条件与判定口径是 T-096 计划 §13 的 Q4 / Q8 / Q12（Max 2026-10-11「全按推荐」）。

插件代码一行不改。全部在离线渲染工具 `celadon_render` 里做：

- 同一首歌、同一组「自动」（GLAZE > 0）渲染完之后，按 ADR-016 的同决策重放再跑两遍 GLAZE 0 的链：
  - **基准 replay-copy**：修坯照填自动链的实际值（就是已有的阳性对照那一遍，已证与自动链最大样本差 ≤ −180 dBFS）；
  - **覆盖 replay-heat0**：同一份逐块参数、同样修坯照填，**prepare 前那份与每一块**都再把 `color.heat` 改成 0（BUG-012 的教训：只改逐块会让开头 20 ms 的平滑走样）。
- 两遍逐块核对参数：交给链的、链实际交给模块的，都要「除 `color.heat` 外逐字段逐位相同，`color.heat` 逐位等于 0」。
- 覆盖 − 基准 = 这份 Heat 底量在成品里的贡献（差信号），在进程内逐样本相减、量完写 `heat_ab.csv`，不写差信号 wav。
- `color.heat = 0` 走 `ColorModule` 现有的空闲跳过，所以「覆盖成 0」就是「这一份 Heat 根本不存在」。

不给 `--replay-override` 时，网格、文件名、`metrics.csv` / `metrics.md` 都与改动前逐字节相同（用 `--check-unchanged` 验）。

## 跑法（DGX）

```bash
SPARK=~/.cache/celadon-spark/bin/spark-test.sh
SRC=~/.cache/celadon-spark/src/t096-d                        # 本分支的源码
$SPARK $SRC build celadon_render
$SPARK $SRC run bash $SRC/docs/validation/2026-10-11-T096d-heat-ab/run_heat_ab.sh \
  --render '$BUILD/celadon_render' --out ~/.cache/celadon-t096d/heat-ab
```

- `run` 走全机串行锁；脚本里每首歌一个进程，`--jobs` 个并行（默认 10）。`'$BUILD'` 由 `spark-test.sh run` 展开；万一原样传进来，脚本按环境变量 `BUILD` 再补展开一次。也可以先用 `$SPARK $SRC dir` 取到构建目录，直接写全路径。
- 先跑门槛自检：`celadon_render --selftest`、`--selftest-house`、`--selftest-live`，以及 `heat_ab_report.py --selftest`。任何一项没过就不渲染，退出码 1。`--skip-selftest` 可跳过。
- **预演**：`bash run_heat_ab.sh --dry-run --render x --out y`。只打印输入核对结果和要跑的每条命令，不建目录、不写文件，在 Mac 上也能跑。
- **验「不给新开关时逐字节不变」**：加 `--check-unchanged <main 3a7a367 构建的 celadon_render>`。两版不给 `--replay-override`、同参数各跑一遍，逐首比较：
  - `metrics.csv` 逐字节比；
  - `metrics.md` 去掉「生成时间」一行后逐字节比。

  用的网格：22 首 `--glaze 0,35,70,100 --style auto --clay auto,off`；House 9 首用各自子类 × 三档交付 × `--bpm`，走 T-038 追加列那条路。
- 输出目录要先在 vault TASKS ② 登记占用（建议 `~/.cache/celadon-t096d/`）。
- `--out` 里已经有 `runs/`、`unchanged/` 或 `jobs/` 时脚本拒跑，免得新旧结果混在一起。

### 跑哪些

| 组 | 曲目 | 曲风 | 交付 | GLAZE | 风格 / 修坯 | 每首几行对照 |
|---|---|---|---|---|---|---|
| songs22 | `~/.cache/celadon-t048-22songs/wav/` + `wav-missing2/`（应为 22 首） | 通用（默认） | 标准（默认） | 35 / 100 | auto / auto | 2 |
| house9 | `corpus.csv` 的 9 首（deep / tech / melodic 各 3），音频 `~/.cache/celadon-t048-cal/<类>/<曲>/<曲>.wav` | 各自子类 | 标准 / 流媒体 / 俱乐部 | 35 / 100 | auto / auto | 6 |

- 全部带 `--metrics-only`，不写成品 wav。
- 22 首不足 22 首、9 首缺音频时，脚本默认停下报错。确实要先跑已有的，就加 `--allow-partial`；`run_info.txt` 会记下缺了什么。
- House 组的 `--bpm` 取 `~/.cache/celadon-t048-cal/manifest.tsv`。找不到时用仓库副本 `docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv`，原样传，同 `calibrate_house.py`。
- CPU 不量，归 (f) 线。

### 输出

```text
<out>/run_info.txt                     起止时间、主机、代码提交、两份 celadon_render 的 sha256、条件、逐首输入清单、失败清单
<out>/selftest-<n>.log                 门槛自检日志
<out>/in/<组>/<曲目>/                  只含这一首软链的输入目录（--in 指向它）
<out>/jobs/<编号>.sh                   每个 celadon_render 进程一份作业脚本（xargs 并行跑它们）
<out>/runs/<组>/<曲目>/heat_ab.csv     每首一份（下节）；同目录 metrics.csv / metrics.md 里多了 clay-replay-heat0 行，stderr.txt、exit_code
<out>/unchanged/{base,new}/<组>/<曲目>/  --check-unchanged 的两版输出
<out>/REPORT.md  cells.csv  rows.csv   heat_ab_report.py 的汇总（每格结论 / 每格一行 / 每行的分类）
```

退出码：

| 码 | 含义 |
|---|---|
| 0 | 全部跑完、没有失败 |
| 1 | 有失败：自检没过；某首退出码非 0 或没出 `heat_ab.csv`；汇总出错；两版输出不同 |
| 2 | 参数或输入不对 |

一首失败不会挡住别的歌。`celadon_render` 遇到这几种情况会计入失败、退出码非 0，但照样把这首量完、写完：

- 参数核对没过；
- 延迟或长度对不上；
- 阳性对照 mismatch。

## heat_ab.csv 各列

一行 = 一首歌 × 一组「自动」（GLAZE > 0）。dB / LU 都是「差信号相对母带」，越负越听不见。差信号恰好为 0 时写 `-inf`。算不出的项留空，原因写进 `na_reason`，绝不填 0。

| 列 | 含义 |
|---|---|
| `song` / `source_file` / `sample_rate` | 歌名、原文件名、采样率 |
| `glaze` / `style` / `genre` / `delivery` / `listen_from_bar` | 这一组「自动」的条件 |
| `override` | 覆盖原文（`color.heat=0`） |
| `variant_file` | 覆盖行的成品文件名（`--metrics-only` 时为空） |
| `trust` | `ok`：可信；`untrusted`：不可信，见下；`noop`：这一组每块的自动 Heat 都已是 0，覆盖什么都没改，差信号各列留空 |
| `replay_check` / `replay_max_diff_dbfs` | 这一组阳性对照（replay-copy 对自动链）的结论与最大样本差，同 `metrics.csv` |
| `fed_params_equal` | 交给两条重放链的参数（prepare 前 + 逐块）：除 `color.heat` 外逐字段逐位相同，`color.heat` 逐位等于覆盖值 |
| `chain_params_equal` | 两条重放链实际交给模块的参数（`getEffectiveParams`）：同上 |
| `first_mismatch` | 上两列不是 true 时的第一处（块号 + 字段名） |
| `blocks` / `overridden_blocks` | 自动链第二遍的块数；记录值与覆盖值不同的份数（含 prepare 前那份；0 就是 noop） |
| `heat_mean` / `heat_max` / `heat_zero_pct` | 自动链逐块实际 Heat 的均值 / 最大，以及为 0 的块占比（%）。这就是被移除的那份 Heat；渲染器的手动 Heat 出厂为 0，所以全是自动量 |
| `master_lufs` | 母带整体响度（插件自己的 `LoudnessMeter`） |
| `diff_max_abs_dbfs` | 差信号最大样本绝对值（dBFS） |
| `diff_rms_re_master_db` | 不加权 RMS：Σ差² ÷ Σ母带²，两声道合计 |
| `diff_k_re_master_lu` | **K 加权响度差（整首）**：在母带按 BS.1770-4 门控选中的 400 ms 门块上，差信号 K 加权能量 ÷ 母带 K 加权能量。差信号常在 −70 LUFS 以下，自己过不了绝对门，所以用母带的门块，在同一组时刻上比 |
| `diff_k_worst3s_lu` / `worst3s_start_s` | **最坏 3 s 窗**：30 个 100 ms 子块，同短期响度 S，逐子块滑动；只计母带这一窗 ≥ −70 LUFS 且不低于整体 − 20 LU 的窗（歌头歌尾的静音、淡出不算）。取差 ÷ 母带最大的那个，以及它的起点 |
| `diff_above_17k8_rms_dbfs` / `diff_above_17k8_re_master_db` | 差信号过 8 阶 Butterworth 17.8 kHz 高通后的 RMS（dBFS），以及它相对**母带全频**能量的值。MP3 解码的母带在 17.8 kHz 以上常常是空的，所以不拿同一段比。采样率 ≤ 35.6 kHz 时留空 |
| `band_<40…16000>hz_re_master_db` | 27 段 1/3 倍频程（Balance 的分段与分析，标称中心频率）：差信号段能量 ÷ 母带同一段能量。母带这一段没有能量时留空 |
| `na_reason` | 不可信的原因、算不出的项及原因 |

`untrusted` 的来源（分号分隔写在 `na_reason`）：

- 阳性对照不是 `ok`，即 mismatch，或修坯 / 目标频谱「重放表达不了」的 n/a；
- 两层参数核对任一没过；
- 覆盖重放与基准的延迟或长度不同。

后两种同时计入 `celadon_render` 的失败。不可信的行照样量完，数字留着，方便查原因，但不进判定。

## 结论怎么读

```bash
python3 tools/render/heat_ab_report.py --md REPORT.md --csv cells.csv --rows rows.csv <out>/runs   # 只用标准库
```

- **一格** = 语料（songs22 / house9）× 交付 × GLAZE × 风格 × 聆听起点。曲风不分格：House 组每首用的是各自的子类。
- **进判定的行**要同时满足：`trust = ok`；两列参数核对都是 true；`diff_k_re_master_lu` 与 `diff_k_worst3s_lu` 都有数。
- **一格判 2a**：这一格里两项都比母带低 ≥ 50 LU（`MARGIN_LU`）。
  - 整首：取最坏那首的 `diff_k_re_master_lu`；
  - 3 s 窗：取各首最坏 3 s 窗里最响的那个，不一定是同一首。
- **一格判 ABX**：上面两项有一项不到 50 LU。
- **无可信数据**：这一格一行可进判定的都没有。
- **总结论**：
  - 所有格都是 2a：「直接走 2a」；
  - 有一格 ABX：「交 ABX」，按 ADR-034 决定 4 做 40 次、分 3 个会话，通过线 26/40；
  - 否则有无可信数据的格：「数据不全」，先补跑。
- 报告另列：
  - 不可信 / 无自动 Heat / 算不出的行数，以及逐行原因；
  - 参数核对不全是 true 的行。验收要求这两列全是 true。

50 LU 的来历：T-096 计划 §13 Q8 定的起点值，没有另做听感标定，属于完成标准。要改由 Max 定，只改 `heat_ab_report.py` 顶上的 `MARGIN_LU`。

## 注意

- **带开关的输出目录不要拿去跑 `gate_check.py` / `house_gate.py`。** 它们不认识 `clay-replay-heat0` 行：前者当普通行在第 4 步按锁定状态判，后者只排除 `replay-off`。门槛照旧用不带开关的那次输出。
- House 组的音频是 Jamendo 45 首里裁到第一个小节线的 44.1 kHz 24 bit wav（T-038 标定语料），不是成品母带。22 首是 Max 的门槛曲。两组的 17.8 kHz 以上读数含义不同：Jamendo 原件是 MP3 解码。
- 内存：每个进程额外留着基准那一遍的整段成品、覆盖那一遍、差信号与三份逐块参数。一首 8 分钟的歌约多 1 GB，`--jobs 10` 在 DGX（121 GB）上没问题。
- 差信号的四项读数（K 加权整首、最坏 3 s 窗、27 段、17.8 kHz 以上）都在 `celadon_render --selftest` 里用合成信号核对过已知答案：
  - 恒定比例 −60 dB 时各项都是 −60 ± 0.01；
  - 20 kHz 正弦的高通读数与 |H|² 公式差 ≤ 0.05 dB；
  - 最坏窗定位与静音门；
  - 门控与 `LoudnessMeter` 差 ≤ 0.1 LU；
  - 差为 0 时写 `-inf`；
  - 2 s / 32 kHz 的歌留空并写原因。

  整条链部分还核对了四件事：
  - GLAZE 35 时自动 Heat > 0，覆盖成 0 差信号非 0；
  - 覆盖值 = 记录值时与 replay-copy 逐位相同；
  - prepare 前那份与每一块都被覆盖，且这一项有牙；
  - GLAZE 0 没有重放行。
