# T-097 开工基线（DGX max-spark）

> 用途：最后统一跑全量时，只把**新增**失败算到 T-097（全局规则「开工基线只算新增」）。
> 机器：DGX（max-spark，aarch64，20 核），`~/.cache/celadon-spark/bin/spark-test.sh <源码目录> test`（全机锁）。
> **现行基线是 main `4be946b`**（2026-10-23 分支并入 main 之后同机重跑，下一节）；第二节起的 `3a7a367` 是 2026-10-11 的开工基线，留作历史，不再拿来比。

## main `4be946b`（2026-10-23，现行）

源码 `git archive 4be946b` 解到 `~/.cache/celadon-spark/src/t097-main2/`；日志在 DGX `~/.cache/celadon-spark/logs/t097/`。

| 项 | 命令（全机锁内） | 结果 |
|---|---|---|
| 全量单元测试（22:3x PDT） | `spark-test.sh t097-main2 test` | `==== celadon_tests: 134719 passed, 5 failed ====`；5 条与下面 `3a7a367` 那 5 条相同（4 条 ColorModule NEON 融合乘加、1 条 BalancePerformance 计时，aarch64 已知）；`main2-full.log` |
| 20 首通用 / 标准 | 同下面 `3a7a367` 的 `batch.sh … --glaze 0,35,70,100 --style auto --clay auto,off`，输出 `~/.cache/celadon-t097/r22-main2/` | 20 首全部成功；`metrics.csv` 240 行，sha256 `6e6d8436…457a`，**与 `3a7a367` 逐字相同**；`gate_check.py` FAILS 93，输出 sha256 `466f4f26…e6c8`（也与 `3a7a367` 相同） |
| House 合成曲 | `make_synthetic_house.py` 两首（house-124、tech-124）→ `celadon_render --genre deep --delivery standard,streaming,club --bpm 124 --listen-from-bar 1,33 --segments-csv --metrics-only --no-replay` → `house_gate.py --annotations`，输出 `~/.cache/celadon-t097/house-main2/` | 12 行 15 项失败（`metrics.csv` sha256 `24f668a1…9f49e`，`house_gate.txt` sha256 `77015b87…3e3a`）；逐行见 REPORT 第 4 节。`house_gate.py` 对两首都提示「画像没有 House 门控结果（house_gate_valid ≠ 1）」，T-097 那份同样有，两边同条件比 |

## main `3a7a367`（2026-10-11 开工基线，历史）

源码：`git archive 3a7a367` 解到 `~/.cache/celadon-spark/src/t097-main/`。

### 全量单元测试

- 时间：2026-10-11 14:3x（PDT）；用时 3 分 39 秒（real）。
- 结果：`==== celadon_tests: 134603 passed, 5 failed ====`，620 组用例。
- 5 条失败（都是 aarch64 上已知的固定失败，与 GLAZE 守护族无关）：

| 用例组 / 用例 | 失败断言 |
|---|---|
| ColorModule / stereo with L = R is bit-identical to mono (packed stereo biquads == scalar biquads) | left vs mono, block 512：Expected 0, Actual 1 |
| 同上 | right vs mono, block 512：Expected 0, Actual 1 |
| 同上 | left vs mono, block 17：Expected 0, Actual 1 |
| 同上 | right vs mono, block 17：Expected 0, Actual 1 |
| BalancePerformance / balance on comb material (20+ bands working): per-block best-of-5, mean and worst block | stereo balance costs 1.73 x mono (limit 1.40) |

### 22 首 gate_check + house_gate

#### 通用 / 标准（20 首，2026-10-11 14:4x PDT）

- 22 首里有 2 首还没进歌单（T-096 补到 `wav-missing2/`，到了再补跑这 2 首）；现有 20 首 = `~/.cache/celadon-t048-22songs/manifest.tsv`（sha256 核对通过）。
- 命令（全机锁内）：`spark-test.sh ~/.cache/celadon-spark/src/t097-main run bash -c "bash ~/.cache/celadon-t048-22songs/batch.sh $BUILD/celadon_render ~/.cache/celadon-t097/r22-main --glaze 0,35,70,100 --style auto --clay auto,off"`
- `celadon_render` sha256 `279bc9bf9c70784fbfaac5dd31b0dcd50b70cc06e10507337ae37b09f2a3e736`；20 首全部成功，`metrics.csv` 240 行（input 20、render 220），sha256 `6e6d8436ad3f156e0d3dcb0e94ce5c106696b4b79ecaec8700225b5e992e457a`。
- `gate_check.py metrics.csv`：退出码 1，**FAILS: 93**（输出存 `~/.cache/celadon-t097/r22-main/gate_check.txt`，sha256 `466f4f26…e6c8`）：

| 失败类 | 条数 | 说明 |
|---|---|---|
| 有效音乐 ≥ 30 s，成品末尾却是 unlearned / 校准无效 | 72 | 指标表的 confidence 列是空的，gate_check 退回阶段 1 的「已锁定」判定；阶段 2 下所有格子都是 unlearned。是工具口径问题，与守门无关 |
| 反相提示时长占比 > 5 %，而插件慢路 ρ 从未 < −0.2（误触发） | 21 | 7 首 × 3 档 GLAZE（WTF、Will We Be OKay (Extended Mix)、emmmayiyu、l_kskdhjf、untitled、wildtom、will we be okay） |

- 修坯门槛（入口峰值、响度下降）、GLAZE 0 逐位、重放阳性对照、真峰值：0 失败。
- 改后同参数重跑，逐条比对：只把**新增**失败算到 T-097。

#### House

10-11 没在 `3a7a367` 上跑；2026-10-23 在 `4be946b` 上跑了，见上面第一节。
