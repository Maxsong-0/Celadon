# T-015 · BUG-003「整首停在校准中」诊断与修复证据（2026-09-27）

档案：`项目/Celadon/06-BUG/BUG-003-约束反馈吸收后校准重开导致整首停在校准中.md`、日志 `02-日志/2026-09-27.md` 第 6 节。
改前备份：`Celadon-releases/source-baseline-2026-09-27-T-015-before/`（含 SHA256SUMS）。

## 1. 复现回路

| 文件 | 用途 |
|---|---|
| `calib-loop.sh` | 同步工作树到 109、编 `celadon_render`（先跑 `--selftest`），指定歌曲每首一个进程并行跑两遍流程，拉回。`TRACE=1` 另出逐块追踪。**注意**：额外参数不能为空（`printf '%q '` 空参数会产出一个空字符串参数），默认网格要显式写 `--glaze 0,35,70 --clay auto,off` |
| `summ.py <输出目录>` | 每组两遍：开轮 / 结束 / 约束反馈时刻、末态 |
| `seg.py <追踪.csv>` | 逐块追踪压成状态段（阶段、轮、作废原因、不开轮原因、偏差、保护层、反馈） |
| `cols.py` | 诊断期间追踪用的是临时 `dbg_*` 列名，读入时统一成 README 里的定稿列名 |

歌曲在 109 的 `~/.cache/celadon-render/songs-9a355b99857c/`（Max 的 `~/Downloads/Test`，mp3 已转 wav）。

## 2. 修前（`before-subset-summary.txt`，11 首 × GLAZE 35 / 70 × 修坯全关，与门槛第 1 / 2 轮逐位一致）

注：这两份汇总里 `v` 是诊断列「已提交校准 valid」，不是「适用」；成品的权威状态看门槛指标表的 `calibration_valid`。

三类：
1. **循环**（本 BUG）：12223 g70、meme g35 / g70、somggggg g35 / g70、wildtom g35 聆听遍（193 s 开 4 轮）。
   `segments/*.before.txt`：预算受限 → 吸收反馈 → 过渡后从偏差 0 重开 → 又往上推 → 保护层又退让。
2. **有效音乐太短**：sh_t 16.4 s、emomax / fule / ooaa / woqu 约 15.5 ～ 16 s（原曲逐秒电平见日志；ooaa 后 30 s 是 −81 dBFS 静音，
   `segments/ooaa__g35_clay-off_auto.*.txt` 里第二轮「采集中」26 s 不动就是静音：配对全低于 −70 LUFS 门限）。
3. **untitled 7.8 s**：学不完画像。

## 3. 修复

`Source/dsp/GlazeController.cpp`：吸收型约束反馈（step = 0：预算受限、锁定后保护层卡住）把适用的已提交校准改绑到新上限、不开新一轮；
降 1 dB 的反馈与画像失配照旧失效重测。其余改动（探针诊断字段、追踪校准列、README）见档案 BUG-003。

## 4. 验证

- 回归用例 `GlazeControllerTests` 12d：修前 107 上 4 项失败，修后全过。
- 107 全套（GCC 15.2，Release -O3，增量构建 11 个目标文件 0 警告）：`full-107-summary.txt` —— **129314 过 / 0 败**。
- 修后子集 `after-subset-summary.txt`：循环类全部「已锁定 + 适用」，剩下的都是音乐太短与 untitled。
- 门槛全量（22 首 × GLAZE 0 / 35 / 70 × 修坯自动 / 全关，109，指标表 `artifacts/render-gate3-2026-09-27-T015/metrics.csv`）：
  - `gate2-vs-gate3.txt`：GLAZE 0 的 44 行与第 2 轮逐位相同；132 行成品 112 行逐位相同、20 行变了（响度变化 −0.22 ～ +0.19 LU）；
    GLAZE > 0 不是「已锁定 + 有效」的组 **27 → 17**，剩下 17 组全是音乐 ≤ 16 s（13 组）与 untitled（4 组）。
  - `gate3-check.txt`（ADR-014 修坯门槛）：3 项失败——WTF g70 限幅入口峰值 +0.747 dB；l_kskdhjf g70 +0.444 dB、响度 −0.100 LU。
    两首的「全关」与第 2 轮逐位相同，变的是「自动」。出峰块上 WTF 修坯中性（低切 0、收单 0、峰值守护 0 样本），
    两条链的约束反馈上限分别是 1.145 / 0.274 dB（Glue 16.8 / 4.0、Heat 7.6 / 1.8）：是 GLAZE 方案轨迹分岔，不是修坯抬峰。
    修前的循环每圈把上限往下压，两条链都被压到很低，差别被盖住（第 2 轮 0 失败）。门槛口径要不要改，待 Max 定。
- `old-vs-new-behaviour.txt`：同一份代码只把 `absorbsGuard` 关掉当「修前行为」对照，5 首长歌 × 4 组 × 两遍：
  开轮 65 → 29、反馈 41 → 34、方案过渡 61 → 54、处于「校准中」1525 → 695 s、处于过渡 483 → 462 s。
- `ab-compare.txt`：A/B 包重出为 `artifacts/ab-final-2026-09-27-T015/`，5 首里 WTF / l_kskdhjf / will we be okay 变了，KINOSEI / woqu 逐位不变。

## 5. 没做的

- 本机 Mac 没编译（纪律 ⑥）；装在系统里的 VST3 0.1.7.0 不含本修复，要进插件需 Xcode Release 重编并安装（已登记 TASKS「待重启」）。
- `-march=x86-64-v3` 全套没跑：改动只在编排器控制逻辑，不碰逐位契约相关的浮点核（ADR-013）。
