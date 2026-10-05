# T-048 阶段 1 收口 · DGX Spark 验证报告（⑤⑥⑦）

> 时间：2026-10-05 14:34～14:49（本机 PDT）。执行器在 max-spark（aarch64）上跑。
> 工作树 `.claude/worktrees/t048-phase1-closeout-c841`，分支 `cursor/t048-phase1-closeout-c841`，尖端当时 `5c6c79b`（合入 main `75a97ba` + 阶段 1 收口）。
> 构建：`~/.cache/celadon-spark/bin/spark-test.sh`（Release + Ninja，`-j8`）。Python：`~/.cache/celadon-spark/venv/bin/python`。
> 全机锁：`~/.cache/celadon-spark/locks/run.lock`（渲染 / 测试串行）。
> 小证据：`docs/validation/2026-10-02-T048-phase1/P1C-spark/`；原始产物在 `~/.cache/celadon-t048-cal/closeout/` 与 `~/.cache/celadon-t048-cal/live/closeout-*`、`~/.cache/celadon-t048-22songs/out-closeout-*`。

## 0. 结论

| 项 | 结果 |
|---|---|
| ⑤ 因果重放（4/4 型，新口径跟踪误差） | M2 **达标**（0.23 / 0.79）；M3 **达标**（31 / 31）；M4 跟踪误差 \|I_{d+3} − D4\| **28 / 38** 首 ≤ 1 LU（中位 / P90 / 最大 = 0.40 / 1.76 / 2.80）。旧口径 \|I_{d+3} − I_end\| 仍 21 / 38（与 P1R 一致）。M0=6 后 live-trace 摘要已变（见 §1）。 |
| ⑤ 响度型对照（general） | 按设计不达标：M2 0.54 / 1.41；M4 跟踪 15 / 38。 |
| ⑥ 默认网格 vs main `75a97ba`（live 关） | **20 / 20 首** `metrics.csv` 逐字节相同；合并表 sha256 `cc8352a28e8d6618` 相同。插件出声不变。 |
| ⑥ 从第 N 小节起播（listen-from-bar 1,33）重定基线 | 20 首跑完。对 main：14 首相同、**6 首不同**（B2 半样本锚定留在共用 `SegmentLedger::anchor`，预期）。新基线 = 本分支 `out-closeout-H`（合并表 sha256 `77626bbcb226c943`）。Ansun / KINOSEI 原件仍缺 → 20 首。 |
| ⑦ 全套 0 新增失败 / 0 新增警告 | Spark：**149099 过 / 5 败**；5 败与同机基线同类（ColorModule L=R×4、BalancePerformance×1）。相对 P1T 基线 `148803/5`：**新增失败 0**。独立源码副本全新构建 closeout / main 的 `warning:` 行均为 **0**（新增警告 0）。 |
| 107 / 109 | **可达且四套全绿**（见 §8）：107 x86 / 107 x86-v3 / 109 x86 / 109 MSVC 均为 **149357 / 0**，EXIT 0。 |

## 1. 二进制与 M0=6（live-trace 摘要会变）

| 二进制 | 源码 | 构建目录 | celadon_render sha256 |
|---|---|---|---|
| closeout `5c6c79b` | 本工作树 | `~/.cache/celadon-spark/build/t048-phase1-closeout-c841-f00161ca04` | `e2882f9606bffb7147…` |
| main `75a97ba` | `celadon-t076-rebuild` | `…/celadon-t076-rebuild-b647a8d186` | `60b84952c9cbc96f91…` |

- `LiveModelConstants::fourOnFloor.confidenceBars`：**6**（本分支 `__final.csv` 写明 `const.fourOnFloor.confidenceBars,6`）。P1R 出厂追踪同键为 **8**。
- 因此 live-trace / `__final` / 分析摘要与 P1R 的 `p1r-default` **不再逐字节相同**（抽样三条轨迹 sha256 前 16 位均不同）：

| 轨迹 | closeout-default | p1r-default（M0=8） |
|---|---|---|
| `track_0268597__tech.csv` | `f58c3da442b746cd…` | `552d3a95aca14428…` |
| `track_1062232__melodic.csv` | `d47576e6ba6c009b…` | `822283ae7ac16d53…` |
| `track_0155328__deep.csv` | `ea7e7ea16824fdb1…` | `05aeffec1a31abe6…` |

- 自检：`--selftest` / `--selftest-house` / `--selftest-live` 全部通过（含键表 37、live-constants 拒收 12 种坏写法）。

## 2. ⑤ 因果重放（新口径）

命令（工作目录 `~/.cache/celadon-t048-cal`，标签已拷到该目录）：

```text
calibrate_house.py render-live <cal> <closeout celadon_render> -j 8 --tag closeout-default
calibrate_house.py analyze-live <cal> --auto-labels <abs>/labels-auto.tsv --labels <abs>/labels-override.tsv --tags closeout-default
```

另跑 `--genre general` → 标签 `closeout-general`（响度型对照）。

### 2.1 4/4 型（`closeout-default`，45 首；可算 38）

| 指标 | 中位 / P90 / 最大 | 门槛 | 结论 |
|---|---|---|---|
| M1 \|I_end − D\| | 0.15 / 0.48 / 0.95 | 对照离线 | 只对照 |
| M2 第 16 小节起逐首中位 | **0.23 / 0.79** / 2.47 | ≤ 0.3 / ≤ 1.0 | **达标** |
| M3 breakdown 漂移 | 0.02 / 0.19 / 0.48（31 / 31） | 每段 ≤ 0.5 | **达标** |
| **M4 跟踪误差 \|I_{d+3} − D4\|** | **0.40 / 1.76 / 2.80（28 / 38）** | ≤ 1.0 | 28 / 38（规格第 5 节已知限制约 10 首） |
| M4 旧口径 \|I_{d+3} − I_end\| | 0.88 / 2.50 / 8.65（21 / 38） | — | 与 P1R 一致 |
| M4 歌自己变了 \|D4 − I_end\| | 0.75 / 2.01 / 9.20 | — | 对照 |
| c 到 0.8 的有效音频 s | **31.4 / 58.8 / 95.9** | — | 相对 P1R（M0=8）的 40.5 / 70.2 / 103.3 更快 |
| 第一个 drop 之后 c 最低 | 0.69 / 0.96 / 1.00 | — | 相对 P1R 0.59 / 0.92 / 0.99 略抬 |

终判门控占比（首判 / 终判 m≥0.5）：intro 78/37、drop 87/86、breakdown 20/20、outro 28/28 %——与 P1R 同口径数字一致（M0 不改 m）。

证据：`P1C-spark/analysis-default.md`、`combined-default.md`；原始 `~/.cache/celadon-t048-cal/live/closeout-default/`。

### 2.2 响度型对照（`closeout-general`）

| 指标 | 中位 / P90 / 最大 | 备注 |
|---|---|---|
| M2 | 0.54 / 1.41 / 3.27 | 按设计不达标 |
| M3 | 0.54 / 1.54 / 2.07（13 / 31） | 按设计 |
| M4 跟踪 \|I_{d+3} − D4\| | 1.50 / 3.06 / 4.15（15 / 38） | 对照 |

证据：`P1C-spark/analysis-general.md`。

## 3. ⑥ 门槛曲 20 首（Ansun / KINOSEI 缺）

歌单：`~/.cache/celadon-t048-22songs/`（manifest 20 首；批跑脚本 `batch.sh`）。网格与先前 s22 一致。

### 3.1 默认网格 G（`--glaze 0,35,70 --style auto --clay auto,off`，无 listen-from-bar）

| 对照 | 结果 |
|---|---|
| closeout vs main `75a97ba` | **20 / 20** 首 `metrics.csv` 逐字节相同；合并表 **180 行**，sha256 **相同** `cc8352a28e8d661889b3151a1a6fee64fe8cacfd8cd02c3dc6207ac89f53d1d3` |
| 状态 | 两边 20 首退出码全 0 |

→ **插件出声（live 默认关）与 main 逐位不变。**

### 3.2 从第 N 小节起播 H（`--genre deep --bpm 124 --listen-from-bar 1,33` + 同上 GLAZE/修坯）— **重定基线**

CLOSEOUT：B2 半样本锚定留在共用 `SegmentLedger::anchor`，故相对 main 允许不同；本分支产物立为新基线。

| 对照 | 结果 |
|---|---|
| closeout vs main | 20 首都跑通；**14 首** metrics 相同、**6 首**不同 |
| 不同的歌 | WTF、Will We Be OKay (Extended Mix)、emmmayiyu、l_kskdhjf、wildtom、will we be okay |
| 差异形态 | 多数行是 `audio_hash` 变；部分行另有 house_heard/full、trial_*、lufs 等（Deep + from-bar 触达锚定取整） |
| **新基线** | `~/.cache/celadon-t048-22songs/out-closeout-H/`；合并表 sha256 `77626bbcb226c9434ffb4dba678c542f2199e547670bae29f80ba4eca3ef3a0b`（340 行） |

证据：`P1C-spark/compare-G.txt`、`compare-H.txt`、`compare-H-detail.txt`、`metrics-sha256.txt`。

## 4. ⑦ 全套与警告

### 4.1 Spark aarch64 全套

`spark-test.sh <工作树> run` 内直接跑 `celadon_tests`（已持 run.lock）。

| 版本 | 结果 |
|---|---|
| closeout `5c6c79b` | **149099 过 / 5 败** |
| 同机 P1T 基线（`base-t048-tests.log`，9e8575a 系） | 148803 过 / 5 败 |
| 同机 main 旧基线（`base-main-tests.log`） | 148335 过 / 5 败 |

5 败（与基线同类，aarch64 专有）：

1. ColorModule「stereo with L = R is bit-identical to mono」4 条断言；
2. BalancePerformance「stereo balance costs 1.74 x mono (limit 1.40)」1 条。

**新增失败 0。** 通过数增加来自本分支多出来的 Live / 收口用例，不是新坏了旧用例。

### 4.2 警告

对 `git archive` 出的独立源码树全新 `spark-test.sh … build celadon_tests celadon_render`：

| 树 | `warning:` 行数 |
|---|---|
| closeout-warn-5c6c79b | **0** |
| main-warn-75a97ba | **0** |

→ **相对 main 新增警告 0。**（日志里仍有 GCC `note: parameter passing …`，与先前报告一样不计入警告。）

## 5. 107 / 109

本机 SSH `maxsong@192.168.100.107`（githubci，x86_64）与 `…109`（github-2，x86_64）**可达**。从 Spark 工作树调 `scripts/test-linux.sh` / `scripts/windows-wine.sh`。

| 主机 | 套件 | 结果（落盘时） |
|---|---|---|
| 107 | 默认 x86 全套 | **149357 过 / 0 败**，EXIT 0；日志 `P1C-spark/107-x86-full.log` |
| 109 | MSVC（Wine）全套 | **149357 过 / 0 败**，EXIT 0；日志 `P1C-spark/109-msvc-full.log` |
| 109 | x86 全套（`CELADON_TEST_HOST=109`） | **149357 / 0**，EXIT 0；`P1C-spark/109-x86-full.log` |
| 107 | `-march=x86-64-v3` 全套 | **149357 / 0**，EXIT 0；`P1C-spark/107-x86-v3-full.log` |

x86 分片耗时：107 全套已含 `MusicAnalyzerLive` 性能护栏用例；专门的 LiveProfile 分片 P99.9 对照需在 107 空闲后再跑定向（本收口批未另开计时会话）。Spark 上 P1T 已有 aarch64 分片数，**不能代替 107**。

## 6. 阶段 1 能否关上

| 门槛 | 状态 |
|---|---|
| ①～④ 代码收口 | 已在分支（CLOSEOUT.md） |
| ⑤ 因果重放 + 新跟踪口径报告 | **本报告**；4/4 型 M2/M3 达标；跟踪误差 28/38（已知限制） |
| ⑥ 对 main 出声不变 + from-bar 重定基线 | **完成**（20 首） |
| ⑦ Spark 0 新增失败 / 0 新增警告 | **完成** |
| 107/109 全套 | **四套全绿**（§8） |

## 7. 复现要点

```bash
WT=…/t048-phase1-closeout-c841
~/.cache/celadon-spark/bin/spark-test.sh "$WT" build celadon_tests celadon_render
# ⑤
~/.cache/celadon-spark/bin/spark-test.sh "$WT" run bash -c '
  PY=~/.cache/celadon-spark/venv/bin/python; CAL=~/.cache/celadon-t048-cal; CB=$BUILD
  $PY $WT/tools/render/calibrate_house.py render-live "$CAL" "$CB/celadon_render" -j 8 --tag closeout-default
  $PY $WT/tools/render/calibrate_house.py analyze-live "$CAL" --auto-labels "$CAL/labels-auto.tsv" --labels "$CAL/labels-override.tsv" --tags closeout-default
'
# ⑥
bash ~/.cache/celadon-t048-22songs/batch.sh "$CB/celadon_render" ~/.cache/celadon-t048-22songs/out-closeout-G --glaze 0,35,70 --style auto --clay auto,off
# ⑦
~/.cache/celadon-spark/bin/spark-test.sh "$WT" test
# 远端
(cd "$WT" && scripts/test-linux.sh)
(cd "$WT" && CELADON_CXX_FLAGS=-march=x86-64-v3 scripts/test-linux.sh)
(cd "$WT" && scripts/windows-wine.sh test)
```


## 8. 107 / 109 终表（续跑收口，2026-10-05 14:51～14:58 PDT）

先前落盘时 109 x86 与 107 `-march=x86-64-v3` 仍在飞。续跑等到两边 `EXIT`：

| 主机 | 套件 | 结果 | EXIT | 日志 |
|---|---|---|---|---|
| 107 | 默认 x86 全套 | **149357 过 / 0 败** | 0 | `P1C-spark/107-x86-full.log` |
| 107 | `-march=x86-64-v3` 全套 | **149357 过 / 0 败** | 0 | `P1C-spark/107-x86-v3-full.log` |
| 109 | MSVC（Wine）全套 | **149357 过 / 0 败** | 0 | `P1C-spark/109-msvc-full.log` |
| 109 | x86 全套 | **149357 过 / 0 败** | 0 | `P1C-spark/109-x86-full.log` |
| Spark aarch64 | 全套（§4） | 149099 过 / 5 败（已知 aarch64；新增 0） | 1 | `~/.cache/celadon-t048-cal/closeout/logs/full-suite-closeout.log` |

四套 x86 / MSVC 数字一致（149357 / 0）。本机 test-linux 锁：109-x86 结束后交给 107-v3；两者均已释锁。

### LiveProfile 分片耗时（全套内嵌用例 g，未另开定向）

用例：`LiveProfile / g. per-enqueue and per-slice time…`（宽护栏，防 100× 回退；已通过）。摘自全套日志（`P1C-spark/liveprofile-shard-from-fullsuite.txt`）：

| 主机 | 4/4 slice P99.9 (µs) | loudness slice P99.9 (µs) | passes finished late |
|---|---|---|---|
| 107 x86 | 18.71 | 20.27 | 0 / 0 |
| 107 x86-v3 | 18.73 | 20.03 | 0 / 0 |
| 109 x86 | 18.16 | 20.32 | 0 / 0 |

对照：P1T / REPORT-A2 曾问「分片 P99.9 是否 ≤ 15 µs」——x86 上测得约 **18～20 µs**，高于该软目标，但用例护栏通过且「发布前做完」为 0 次迟到。未再开空机专项（避免占 107）。若把 ≤ 15 µs 当硬门槛，记为**可选后续**，不挡阶段 1 验证矩阵收口。

### 阶段 1 验证可否关上

| 项 | 状态 |
|---|---|
| ⑤⑥⑦ Spark | 已在本报告正文 |
| 107 / 109 四套全绿 | **是** |
| 分片 P99.9 ≤ 15 µs 软目标 | x86 实测 ~18–20 µs；护栏通过。可选后续，**不挡关** |
| 合并 PR #54 / deploy | **未做**（按指令） |

**结论：阶段 1 验证矩阵可以关上。** 剩余仅可选的分片软目标收紧（若产品仍坚持 ≤ 15 µs）；规格已知的 M4 跟踪 28/38 不是代码缺口。
