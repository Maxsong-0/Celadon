# R6-02 前置重渲（T-096e）：T-085 的 13 首在含 T-089 的 main 上被多压多少

只渲染、只出报告，不改任何源码（`Source/**`、`tools/render/RenderMain.cpp`、`tools/render/codec_tp_check.py` 都没动）。
工具是 `tools/render/r602_rerender.py`，四个子命令：`prepare`、`render`、`report`、`selftest`。

本目录的 `README.md` 是用法；跑完之后这里会多出 `input_check.csv`、`metrics_*.csv`、`environment.json`、`side_by_side.csv`、`REPORT.md`。

## 要回答的问题

ADR-033 的决定 4：实现「交付档允许自动增益为负」之前，先在含 T-089（1a0e26a）的 main 上重渲 T-085 的 13 首，
看热混音（入口比偏好响度 L_t 更响的歌）在流媒体 / 俱乐部档还被多压多少。判据逐格给，不跨曲平均：

- 热格 = `I − L_t > 0.5 LU`（I = `profile_lufs`；L_t 脚本离线推）。
- 每个热格列：`limit_gr_mean_db`、`GR 均值 − 同档 GLAZE 0 的 GR 均值`（推过头的部分）、`出口 LUFS − L_t`。
- 「问题还在」= 至少一首真歌在流媒体 −10 或 −14 组、GLAZE > 0 的某个热格里 GR 均值 ≥ 0.3 dB，且高于同档 GLAZE 0 的 GR 均值。
  阈值常量集中在脚本顶部（`HOT_GAP_LU`、`PROBLEM_GR_MEAN_DB`、`ABOVE_FLOOR_MARGIN_DB`、`PROBLEM_TARGETS`），属于完成标准，由 Max 认。
- `limit_gr_mean_db` 是「块峰值」的平均，比逐样本平均偏高；ADR-033 与 T-085 用的是同一列，口径一致，但读数偏高要写进结论。

## 矩阵

13 首（9 首 Jamendo + 4 首合成压力曲，来源见 `docs/validation/2026-10-08-T085-codec-tp/corpus.csv`、`input_manifest.csv`）
× 12 格 × 2 个二进制：

| 交付档 | 目标（LUFS） | GLAZE |
|---|---|---|
| 流媒体 | −10、−12、−14（`--streaming-target-lufs`，三项一起平移） | 0、35、100 |
| 俱乐部 | −7（出厂，没有覆盖选项，不传 `--streaming-target-lufs`） | 0、35、100 |

固定参数与 T-085 一致（`tools/render/codec_tp_check.py:226-228`）：`--style auto --clay auto --genre general --no-align --no-replay`，另加 `--metrics-only`。
GLAZE 0 是「不推时的下限」（流媒体的 −2 dBTP Ceiling 封顶只看交付档、不看 GLAZE），GLAZE 35 是出厂默认，GLAZE 100 对齐 T-085。

两个二进制：

| 标签 | 提交 | 含义 |
|---|---|---|
| `main` | 3a7a367 | 现行 main，含 T-089 |
| `pre-t089` | b594874 | T-089 之前、含 f51dd5a；用来把 T-089 的作用与「f51dd5a + x86 到 aarch64 + ffmpeg 版本」这些混杂拆开 |

## 全流程（全部在 DGX 上跑；Mac 不渲、不测、不编）

```bash
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
PY=$HOME/.cache/celadon-spark/venv/bin/python
W=$HOME/.cache/celadon-t096e                  # 工作目录：WAV、下载的 mp3、各格原始产物；不入库
OUT=$W/report                                 # 报告产物（csv / json / md）；最后拷回仓库本目录入库
TOOL=$SRC/t096e-tools/tools/render/r602_rerender.py
```

### 0. 取三份源码（在 Mac 的仓库根目录只打包，不编译）

```bash
git archive 3a7a367 | ssh max-spark 'D=$HOME/.cache/celadon-spark/src/t096e-main; mkdir -p "$D"; tar -x -C "$D"'
git archive b594874 | ssh max-spark 'D=$HOME/.cache/celadon-spark/src/t096e-pre; mkdir -p "$D"; tar -x -C "$D"'
git archive claude/t096e-rerender | ssh max-spark 'D=$HOME/.cache/celadon-spark/src/t096e-tools; mkdir -p "$D"; tar -x -C "$D"'
```

`t096e-tools` 只用来跑脚本（脚本、`codec_tp_check.py`、T-085 的 CSV 都在里面）；两个二进制分别从 `t096e-main`、`t096e-pre` 编。
Mac 解析不到 `max-spark.local` 时用局域网地址，见 vault `04-协调/TASKS.md`。DGX 全机锁上有别的会话在占时先排队。

### 1. 编两个 `celadon_render`

```bash
"$SPARK" "$SRC/t096e-main" build celadon_render
"$SPARK" "$SRC/t096e-pre"  build celadon_render
BIN_MAIN="$("$SPARK" "$SRC/t096e-main" dir)/celadon_render"
BIN_PRE="$("$SPARK" "$SRC/t096e-pre" dir)/celadon_render"
```

### 2. 先过自检与预演（只用标准库，不碰盘）

```bash
python3 "$TOOL" selftest                                    # 期望：11 行 ok，末行「selftest：11 ok，0 失败」，退出码 0
python3 "$TOOL" prepare --dry-run --work "$W"               # 期望：stdout 正好 13 行（9 首 Jamendo + 4 首合成）
python3 "$TOOL" render --dry-run --render "$BIN_MAIN" --label main --work "$W" --out "$OUT"   # 期望：stdout 12 条命令
```

### 3. 重建输入（下载 9 首 Jamendo 约 82 MB + 合成 4 首）

```bash
mkdir -p "$OUT"
"$PY" "$TOOL" prepare --work "$W" --out "$OUT"
```

`prepare` 直接调 `codec_tp_check.download_jamendo`（T-085 的下载解码配方，不复制）和 `codec_tp_check.synthetic`（种子 85），
再对每个文件核对：sha256 是否与 `input_manifest.csv` 一致、帧数是否一致、Jamendo 原件 sha256 是否与 `corpus.csv` 的 `download_sha256` 一致。
sha256 不一致的文件用 `codec_tp_check.measure` 量 LUFS 与 8× 真峰值，对 T-085 的 input 行求偏差，
超过 ±0.05 LU / ±0.05 dB 的标「输入不同，不并排旧数」、只留新数，**不中止**。结果在 `input_check.csv`。
全部 13 首都不同时会在 stderr 打警告，这时旧数整体不能并排，先问 Max。

重建的 WAV 可能与 T-085 的不逐位相同：T-085 在 x86、ffmpeg 7.1.5 上解码，这里是 aarch64、ffmpeg 6.1.1。
`prepare` 阶段的偏差是用 `codec_tp_check.measure`（pyloudnorm + 8× 真峰值）量的初筛；`report` 会再用渲染器自己的 input 行
（与 T-085 同一把量具）重判，最终「并排与否」以 `report` 为准（`REPORT.md` 第 5 节、`side_by_side.csv` 的 `t085_status`）。

### 4. 渲两个二进制（走全机锁）

```bash
"$SPARK" "$SRC/t096e-main" run "$PY" "$TOOL" render --render "$BIN_MAIN" --label main     --commit 3a7a367 --work "$W" --out "$OUT"
"$SPARK" "$SRC/t096e-pre"  run "$PY" "$TOOL" render --render "$BIN_PRE"  --label pre-t089 --commit b594874 --work "$W" --out "$OUT"
```

每个二进制 12 格，默认 6 格并行（`--jobs`）。每格的 `metrics.csv` 逐字节复制成
`metrics_<标签>_<交付档>_<目标>_g<GLAZE>.csv`（例：`metrics_main_streaming_-14_g100.csv`、`metrics_pre-t089_club_-7_g0.csv`），
`environment.json` 记提交号、二进制 sha256、每格命令与退出码、输入 WAV 的 sha256、机器信息。
任何一格失败（退出码非 0、行数不对、交付档 / GLAZE 列对不上文件名）就报错、退出码 1，不复制那一格的 metrics。
中断后加 `--resume` 续跑（只跳过已有且完整的格，且二进制 sha256 必须与上次相同）。

### 5. 出报告

```bash
python3 "$TOOL" report --new "$OUT"          # --old 默认就是仓库里 T-085 的三份 render_target_*.csv
```

产出 `side_by_side.csv`（每首 × 档 × GLAZE × 标签一行）与 `REPORT.md`；stdout 逐行打印 L_t 自检（每个可查的格子一行，
`偏差=` 是 `plan_requested_target_lufs` 与预期的差，≥ 0.1 dB 标「偏大」），stderr 打各标签的机器判定。
把 `$OUT` 里的 csv / json / md 拷回仓库本目录入库（WAV、mp3、`stderr.txt` 不入库）。

## 产物

| 文件 | 内容 |
|---|---|
| `input_check.csv` | 每个输入：sha256 / 帧数 / Jamendo 原件 sha256 是否与 T-085 一致，LUFS 与真峰值偏差，能否并排旧数 |
| `metrics_<标签>_<档>_<目标>_g<GLAZE>.csv` | 每格 celadon_render 的 metrics.csv 原样 |
| `environment.json` | `labels.<标签>`：提交号、二进制 sha256、命令、输入 sha256；`prepare`：ffmpeg / 包版本 / 机器 |
| `side_by_side.csv` | `profile_lufs`（I）、`lt_lufs`（L_t，离线推导）、`i_minus_lt`、出口 `out_lufs`、`out_minus_lt`、`limit_gr_mean_db`、`gr_glaze0_mean_db`、`gr_minus_glaze0_db`、`implied_push_db`、`lt_selfcheck_dev_db`、`verdict`、T-085 旧值（或「输入不同，不并排旧数」）、`na_reason` |
| `REPORT.md` | 第 1 节「结论」留给主会话填（重跑 `report` 会保留）；第 2 节机器判定、3 口径、4 环境、5 输入核对、6 热格逐格表、7 三方对照（T-085 / pre-t089 / main）、8 自检 |

## 出错规则（Max 2026-10-10 Q12）

- 基础设施错（文件读不到、渲染失败、列缺）：stderr 报错、退出码 1。
- 素材原因算不出（没有画像、没有同档 GLAZE 0 行）：对应列写空值，原因写进 `na_reason`，绝不填 0。
- 工具只报告：不改渲染器、不改任何输入文件、不自动回退增益。

## 注意事项

- L_t 不在 metrics.csv 里，脚本照 `Source/dsp/GlazePlanner.cpp:1648-1663` 推导，常数抄自 `GlazeConstants.h:174-181, 204-205, 336` 与
  `GlazeConstants.cpp:71, 94, 117`，集中在脚本顶部一处。改了这些常数，脚本要同步改。自检：`plan_requested_target_lufs` 应等于
  `I + 手动链净增益 + GLAZE/100 × (L_t − I)`（手动链净增益取同档 GLAZE 0 行的 `plan_requested_gain_db`），
  不受约束的格子偏差应 < 0.1 dB；结果在 `REPORT.md` 第 8 节。
- 旧数（T-085）混了三个变量：f51dd5a、T-089、x86 对 aarch64。`pre-t089` 这一组同机同输入，用来拆开它们。
- 结果钉在 main 3a7a367（含 T-089、不含守护族最小修）。守护族修复合入后，用同一组命令重跑一遍，作为 R6-02 实现前的最终基线
  （它会让很响的输入少推响，直接改 GR 数）。
- 本目录的数不能替代 22 首门禁与 45 首回放（TASKS 记 T-089 合并时尚未跑）。
