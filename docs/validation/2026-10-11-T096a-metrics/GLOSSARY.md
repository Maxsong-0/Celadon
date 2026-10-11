# T-096a 指标口径表（GLOSSARY）

状态：口径定稿草案，待 Max 过目（计划 §13 Q6：「先出表过目再写码」）。实现见同目录 `README.md` 与 `tools/render/quality_metrics.py`。

ADR-034 决定 6 要求「低频冲击、泵感、PSR 下限、起音保留各只留一个定义」，Q6 又加了「主段」「落差损失」两项。下面六项每项只留一个定义，
写明公式、出处（文件:行）和被放弃的其他口径。之后是 ADR-034 B 组 10 项到 `quality.csv` 字段、单位、`quality_gate.py` 阈值常量的逐项映射。

出处里的简写：

- **vault** = `Max's Secret Place/项目/Celadon/`（只读）。
- **研究目录** = `docs/validation/2026-10-10-T093-optimization-research/`，在分支 `claude/plugin-optimization-research-5f0550`（`84d3e14`）上，main 里还没有。
- 代码行号按本分支的基点（main `3a7a367` + T-096c `a0b9846`）。

## 0. 共同约定

- **对齐**：成品 wav = 链输出 × 10^(`align_gain_db` / 20)，长度 = 原曲 + 0.35 s 余振，已去掉链延迟，与原曲逐样本对齐
  （`tools/render/README.md:107-109`）。还原：y = 成品 × 10^(−`align_gain_db` / 20)，截到原曲长度。源文件按渲染器同样的规则立体声化
  （单声道复制成两声道、多声道取前两个）。`align_gain_db` 为空（`--no-align`）时增益按 0。
- **方向**：所有差值一律「出口 − 入口」；损失类（落差损失）写成正数 = 变差。
- **主段**：见 ⑤。带「主段」字样的量只在主段上统计。
- **算不出**：素材原因（没有主段、不够 3 s、没有标注、没有高频……）写空值，原因写进 `na_reason`（多条用「; 」连接），绝不填 0。
  基础设施错（文件读不到、列缺、行重复、长度对不上）整次运行报错，退出码 1，不写 `quality.csv`。
- **量具**：真峰值、响度、短期响度 S、PSR、LRA、低频相关度、S/M 只用 `tools/render/codec_tp_check.py` 的函数（T-096c 量具库），
  `quality_metrics.py` 不再写第二份。

## 1. 六项统一口径

### ① PSR 下限：`psr_floor_spec_db`（判定用）+ `psr_floor_code_db`（只报）

- **定义**：主段出口 PSR 中位的参照下限，按交付档取**规格值**：标准 8.5、流媒体 8.0、俱乐部 6.5 dB。
  代码现值 8.0 / 7.0 / 6.0 另列一列，等守护族按规格落地后对齐（两列相同时代码列就可以删）。
- **PSR 本身**：`codec_tp_check.psr_series_db`——每 100 ms 一个点，3 s 尾随 8× 真峰值 − 同窗短期响度 S，S 不高于 −70 LUFS 的窗不算。
  主段中位 = 窗终点落在主段里的点的中位数（`psr_main_med_in_db` / `psr_main_med_out_db`）。
- **公式**：B3 判定 `psr_main_med_out_db ≥ psr_floor_spec_db − 0.5`。
- **出处**：vault `01-框架/设计规格-实时GLAZE-曲风.md:58-62`（PSR 守门下限表；House 三类属原型 A，A 行写「现有四项默认关；Techno 8.5 / 8 / 6.5」，
  取同原型 Techno 的数）；vault `03-记忆/ADR-034-算法改动的验收门.md:20`（决定 6：规格 8.5 / 8 / 6.5 对代码 8 / 7 / 6）；
  `Source/dsp/PsrGuard.cpp:7-16`（代码 8 / 7 / 6）；研究目录 `J3-archive-consistency.md:273`（规格与代码没对齐）。
- **放弃**：代码值 8 / 7 / 6 作判定（Q6 定按规格，代码值只并列）；R1 G3 的「P10 ≥ 8 / 7 / 6」（研究目录 `R1-mastering-engineer.md:24`：
  用 P10、取代码值）；R1 G10 的 8 / 6.5（只给两档）；R5 的触发线 7.5 / 6.5 / 5.5（= 代码值 − 0.5，再减 B3 的 0.5 就扣了两次）；
  T-047 REPORT-C 的「流媒体 F = 8」（只有一档）。
- **待认**：通用档（响度型）规格没给 PSR 下限，这里同样按交付档取上面这组数——只作尺子，不是插件守门的阈值。

### ② 泵感：`pump_db`（信号法，主读数）

- **定义**：链给 2～8 kHz 施加的慢速（0.5～8 Hz）电平起伏有多大。
- **公式**：
  1. 入口 x、出口 y 各过 4 阶 Butterworth 2～8 kHz 带通（单向）；
  2. 每 10 ms 一帧，求两声道平均的均方 P_in[k]、P_out[k]；
  3. r[k] = 10·log10 P_out[k] − 10·log10 P_in[k]（dB）= 这一帧链给高频带的增益；P_in[k] ≤ 1e-12（约 −120 dBFS）的帧无效：
     先填成有效帧的中位数再滤波，且不进统计；
  4. r 减去有效帧中位数，过 2 阶 Butterworth 0.5～8 Hz 带通（双向、零相位）；
  5. `pump_db` = 主段有效帧上的 RMS（dB）。
- **性质**（`--selftest` 核对）：纯增益 → r 为常数 → 0；入口 = 出口 → 0；电平按 ±A dB、2 Hz 正弦摆动 → A / √2。
- **增益法（排查子表，不判定）**：`p_slow_db` = trace `limit_gr_db`（块内最大限幅 GR）取负当 20·log10 g 的块率近似，减中位、
  同一个 0.5～8 Hz 带通、主段块上的 RMS。`d_fast_db` 见第 3 节。
- **出处**：研究目录 `R1-mastering-engineer.md:29`（G8 HMD：2～8 kHz 10 ms 包络的出口 / 入口比）、`R5-control-evaluation.md:124-126`
  （U 组）、`R2-limit.md:24`（P_slow：0.5～8 Hz 起伏度区的 RMS）；ADR-034:20（HMD / P_slow 统一命名）；Q6「信号法为主、P_slow 排查」。
- **放弃**：R1 原式「减 1 s 滑动中位去趋势后主段 P95 − P5」（去趋势与带通重复；P95 − P5 对 ±A 正弦读 2A，与 P_slow 的 RMS 不同尺度，
  放不进一张表）；先分别算入口、出口的调制深度再相减（入口自带踩镲的 2～8 Hz 起伏，深度相减会让「压平了踩镲」与「加了泵」互相抵消；
  逐帧比值才只剩链施加的部分）；R5 主读数「`limit_gr_db` 2～10 Hz 带通」（只看限幅器，看不到 Glue / Clip 造成的泵 → 降为排查子表 P_slow，
  频带统一成 0.5～8 Hz）；R5 辅读数七带 MTF（量多、没有阈值，留作以后的排查工具，这次不做）。
- **名字**：主读数只叫 `pump_db`；HMD、P_slow 不再作主读数的名字。

### ③ 低频冲击：`lf_crest_db`（低频峰值因数 LCF）

- **公式**：M = (L + R) / 2 → 4 阶 Butterworth 120 Hz 低通（单向）→ 3 s 窗、100 ms 步长（与 PSR 同一网格）：
  crest = 20·log10(窗内 |M_LP| 的采样峰值 / 窗内 RMS)；窗内均方 ≤ 1e-14 的窗不算；取窗终点落在主段里的窗的中位数，
  入口、出口各一份（`lf_crest_in_db` / `lf_crest_out_db`），`lf_crest_db` = 出口 − 入口。
- **出处**：研究目录 `R1-mastering-engineer.md:23`（G2 LCF：M 声道 4 阶 120 Hz 低通、3 s 窗峰 / RMS、主段中位、出口 − 入口）；
  ADR-034:15（「低频峰值因数」）；研究目录 `J3-archive-consistency.md:272`（三套口径）。
- **放弃**：R3-04 的 MD / CF_LF（研究目录 `R3-balance-clay.md:91-95`：40～120 Hz 带通、10 ms 包络、每小节 P95 − P10，要小节网格，22 首没有）；
  R5 T 组 40～130 Hz「5 ms 峰 / 50 ms RMS」（那是起音口径，归 ④）。阈值不用 R1 G2 的「≥ −1.5」和 R1-08 的「3 dB」，按 ADR-034 取「≥ 基线 − 0.3」。

### ④ 起音保留：`attack_keep_db`（只报不判）

- **公式**：起音只在入口上找——入口 M 每 5 ms 一帧求均方（dB）；某帧比前 50 ms（10 帧）的能量平均高 ≥ 6 dB、本身 ≥ −60 dBFS、
  离上一个起音 ≥ 100 ms，就是起音，锚点 n0 = 该帧起点。入口、出口在同一个 n0 上各算 crest = 20·log10(n0 起 5 ms 内 |M| 的峰值 / n0 起 50 ms 的 RMS)；
  `attack_keep_db` = 主段内起音上 (crest_out − crest_in) 的中位数；主段起音少于 20 个写空。`attack_onsets` = 参与的起音个数。
- **出处**：研究目录 `R5-control-evaluation.md:127`（T 组：以输入起音为锚，5 ms 峰值 / 50 ms RMS，输出 / 输入比的中位）；Q6「起音保留 T 组只报」。
- **放弃**：R5 原式分两带（40～130 Hz、2～8 kHz）各一个读数——一项两个数，按决定 6 先只留全频 M 一个，需要分带时再加排查列；
  R1 G3 ②「起音处 10 ms 峰值 − 其后 100 ms RMS、≥ −2 dB」（研究目录 `R1-mastering-engineer.md:24`：窗长不同、还带判定）。
- **待认**：起音判据的 6 dB / 50 ms / −60 dBFS / 100 ms / 20 个是本表定的起点值（只报不判，影响小）。

### ⑤ 主段

- **定义**：trace 逐块 `current_mainness`（= 插件 `Status.currentMainness`，分析器给正在播的这一段的主段度 m）≥ 0.5 的块。
  一块覆盖的输入样本 = [块在链里的起点, 起点 + 块长)；trace 的 `t_sec` 是成品时间轴（= 链里起点 − 链延迟），所以起点 = `t_sec` × 采样率 + 链延迟，
  链延迟 = −首行 `t_sec` × 采样率。`t_sec` 只留 4 位小数（48 k 下每行差最多 2.4 个样本），所以实现里块长与链延迟用整张表拟合
  （块长 = 首末行跨度 / 行数，链延迟 = 各行「序号 × 块长 − `t_sec` × 采样率」的平均），拟合不上 = 基础设施错。
  第一段读数出来之前插件给 m = 1.0，这一小段照插件原样算主段。
- **Jamendo 交叉核对**（只报）：有逐小节标签（`labels-auto.tsv` 叠 `labels-override.tsv`）且指标表有 `bpm` 列时，
  `main_in_drop_pct` = 主段时长里落在 drop 小节的占比，`drop_in_main_pct` = drop 小节时长里被判成主段的占比（10 ms 帧；只算标签表里有的小节；小节 = 4 拍，缓存已裁到第一个小节线）。
- **出处**：vault `01-框架/设计规格-实时GLAZE-主段与增益.md:35`（第 2 节主段度 m）、`:53`（m ≥ 0.5 的提交）、`:75`（棘轮：主段度 < 0.5 时只降不升）；
  `Source/dsp/GlazeController.cpp:1226-1234`（currentMainness 的三种取值）；`Source/dsp/GlazeTypes.h:432-433`（Status 字段）；Q6「主段 m ≥ 0.5」。
- **放弃**：R1 的「`--segments-csv` 段响度 ≥ 本曲 P95 − 3 LU」（研究目录 `R1-mastering-engineer.md:95`；插件不按它决策）；只用人工标签（22 首没有标签）；
  按 m 加权（得不到二值掩码，「主段中位」没法定义）。

### ⑥ 落差损失：`drop_loss_lu`（正 = 处理后 breakdown → drop 的对比变小）

- **有标注**（`drop_loss_method = sections`）：要逐小节标签，且渲染给了 `--bpm` 与 `--segments-csv`（逐段统计按小节分）。
  对每个「breakdown 之后接的第一个 drop」：loss = (D_in − B_in) − (D_out − B_out)，D / B = `house_gate.section_loudness`
  （段内各小节有效子块的 K 加权能量平均，按 `valid_s` 加权）；报各对里最大的 loss。
- **无标注**（`drop_loss_method = p90_p25`）：入口、出口各取整曲 3 s 短期响度 S（`codec_tp_check.short_term_loudness_series`，100 ms 一个），
  过 −70 LUFS 绝对门与「能量平均 − 20 LU」相对门（与 LRA 同一组门，`codec_tp_check.LRA_*`）；loss = (P90 − P25)_入口 − (P90 − P25)_出口。
  任一边门后不足 60 s，或入口 P90 − P25 < 3 LU（没有对比可保），写空。
- **公式**：B2 判定 `drop_loss_lu ≤ 1.2`（俱乐部档 1.5）。
- **出处**：`tools/render/house_gate.py:7`（③）、`:146-158`（section_loudness）、`:260-286`（gate_breakdown 的配对）；
  vault `01-框架/设计规格-实时GLAZE-主段与增益.md:81-82`（loss = (P90 − P25)_输入 − (P90 − P25)_输出、两份都满 60 s、输入 ≥ 3 LU 才判）；Q6。
- **放弃**：规格在线版的「按有效音频遗忘 τ 180 s」直方图（离线整首就是稳态，遗忘只关乎守门的时效）；报告 B 的「drop 与 breakdown 平均 GR 之差」
  （那是原因的代理，不是结果）；只看 LRA（看整首分布，不专看 breakdown → drop；LRA 另作 B2 第二条）。

## 2. ADR-034 B 组 10 项 → 字段 / 单位 / 判定常量

编号按研究目录 `REPORT.md:291-301`（§6 B 组）；ADR-034:15 是同一份清单。判定常量都在 `tools/render/quality_gate.py` 顶部。
「同组」= 基线表里 (glaze, clay, style, genre, delivery) 相同、歌不同的行；「同行」= 基线表里配对键相同的那一行。

| # | 内容 | `quality.csv` 字段 | 单位 | `quality_gate.py` 判定（常量 = 值） |
|---|---|---|---|---|
| B1 | drop 目标 ±1 LU | `drop_target_dev_lu`（离目标最远的那个 drop 段，带符号） | LU | \|值\| ≤ `DROP_TARGET_WINDOW_LU` = 1.0；只有有标注 + 流媒体 / 俱乐部才有值 |
| B1 | 热启动主段电平误差 ≤ 0.3 LU | `main_level_err_lu` = 主段出口 S 能量平均 − `plan_effective_target_lufs` | LU | 只报（任务书门槛清单没列）；要指标表有方案目标列（用了 `--genre` 等新选项） |
| B2 | 落差损失 ≤ 1.2 / 1.5 LU | `drop_loss_lu`、`drop_loss_method` | LU | ≤ `DROP_LOSS_LU` = 1.2，俱乐部 `DROP_LOSS_CLUB_LU` = 1.5 |
| B2 | 出口 LRA ≥ 入口 − 1 LU | `lra_delta_lu`（`lra_in_lu`、`lra_out_lu`；`codec_tp_check.loudness_range_lu`，整曲） | LU | ≥ −`LRA_LOSS_ALLOWED_LU` = −1.0 |
| B3 | 主段出口 PSR 中位 ≥ 下限 − 0.5 | `psr_main_med_out_db` 对 `psr_floor_spec_db`（`psr_main_med_in_db`、`psr_floor_code_db` 只报） | dB | ≥ `psr_floor_spec_db` − `PSR_FLOOR_MARGIN_DB`（0.5） |
| B4 | 泵感 | `pump_db`（排查：`p_slow_db`、`d_fast_db`） | dB | ≤ 同组基线 `pump_db` 的 P90（`BASELINE_PERCENTILE` = 90） |
| B5 | 低频峰值因数 | `lf_crest_db`（`lf_crest_in_db`、`lf_crest_out_db`） | dB | ≥ 同行基线 − `LF_CREST_ALLOWED_DROP_DB`（0.3） |
| B6 | 限幅差值能量 | `limit_diff_db`（见第 3 节） | dB | ≤ 同行基线 + `LIMIT_DIFF_ALLOWED_RISE_DB`（1.0） |
| B7 | 低频相关度 ≥ 输入 − 0.05 | `low_corr_delta`（`low_corr_in`、`low_corr_out`；`codec_tp_check.low_band_correlation`，125 Hz，整曲） | 比值 | ≥ −`LOW_CORR_ALLOWED_DROP`（0.05） |
| B7 | 逐段 S/M 出口 − 入口 ≤ 0.5 dB | `side_mid_delta_p95_db` | dB | ≤ `SIDE_MID_MAX_RISE_DB`（0.5） |
| B8 | 净频谱改动 | `net_spectrum_p95_db`（`net_spectrum_bands_db` 写 27 段） | dB | 只报（没有目标时没有阈值；任务书门槛清单没列） |
| B9 | 控制质量 | `gain_tv_db_per_min`、`gain_sign_changes_per_min`（`guard_engaged_pct`、`guard_cause_hist` 只报） | dB/min、次/min | 两项各 ≤ 同组基线 P90（`BASELINE_PERCENTILE` = 90） |
| B10 | 流媒体档编码后 TP | 不进 `quality.csv` | dBTP | 只报，由 (c) 交付检查单（`codec_tp_check`）出 |

A 组（绝对门，`quality_gate.py` 同时判）：`true_peak_dbtp`（`metrics.csv` 原值：插件表量的对齐前成品 8× 真峰值，与 `gate_check.py` 同一列）
− `ceiling_dbtp`（`metrics.csv` 的 `effective_ceiling_dbtp`，没有这列时取出厂 −1.0）≤ `TP_EPSILON_DB`（0.01）；`hard_clip_samples` == 0。

**不退步怎么判**（每条 B 判定都一样）：新值没越线 → 通过；越线了，但基线同行也越线、且新值不比基线差 → 「已知」（不算退步，只列说明）；
否则 → 退步（FAIL）。基线有值、新值为空 → FAIL（指标消失）。所以基线对自己跑必然 0 退步；A 组是绝对门，基线自己超线也照样报 FAIL。

## 3. 其余字段的口径

- **`limit_diff_db`**（B6）= 10·log10(Σ主段块 `limit_diff_energy` / Σ主段块 `limit_in_energy`)。两列来自 `LimiterModule` 的只读计数（trace 末尾）：
  `limit_diff_energy` = Σ(加了 Gain 的入口 − 本级湿路输出)²（两声道合计；Clip、前瞻限幅、硬钳位一起算，与 LIMIT 卡片「试听差值」同一个差，
  只是按推响后的电平、不除以 Gain、不钳位），`limit_in_energy` = Σ(加了 Gain 的入口)²。
  分母为什么用入口：B6 原文是「/ 出口能量」；出口 = 入口 − 差值（向量），只有两份能量求不出出口能量；入口能量与差值是同一批样本、同一时刻，
  不受成品对齐、余振影响；做「≤ 基线 + 1 dB」的不退步比较时，两种分母都随 GR 单调变化。（待 Max 认，见返回的待拍板清单。）
- **`d_fast_db`**（排查）= 10·log10(Σ主段块 E_y[b]·(g_hp[b] / g[b])² / Σ主段块 E_y[b])，g[b] = 10^(−`limit_gr_db`[b] / 20)，
  g_hp = g′ − LP20(g′)，g′ = g − 中位(g)，LP20 = 2 阶 Butterworth 20 Hz 低通（双向），E_y[b] = 成品这一块的能量。
  出处：研究目录 `R2-limit.md:23`。块率（48 k / 512 ≈ 94 Hz）只看得到 20 Hz 到块率一半的调制，是 R2 逐样本 D_fast 的粗近似；
  逐样本 g[n] 要等 R2-01 测试钩子（Q15 延后）。纯增益 → g′ 全 0 → −∞。
- **`side_mid_delta_p95_db`**（B7）：主段里每 2 s 一段（从 0 起切，主段样本 ≥ 一半的段才算），每段 `codec_tp_check.side_mid_ratio_db` 出口 − 入口，
  取 P95。出处：ADR-034:15、`REPORT.md:298`；研究目录 `R1-mastering-engineer.md:27`（G6 逐段 S/M）。放弃：R1 G6 的分带（2～16 kHz、40～125 Hz）
  与绝对值（ADR 写的是「出口 − 入口 ≤ 0.5」，变窄不算退步）。
- **`net_spectrum_p95_db`**（B8）：27 段 1/3 倍频程，中心 1000·2^((b − 14) / 3)、边界 × 2^(±1/6)，每个 FFT bin 的功率看成铺满它的频率格、
  按与段的重叠比例分摊（同 `Source/dsp/BalanceModule.cpp:52-60`、`:322-331`）。主段帧（Hann 窗、点数 = 不小于 0.17 s 的 2 的幂、半帧步长、
  帧中心落在主段）的平均功率谱 → Δ_b = 10·log10(P_out,b / P_in,b)；锚 = 中心在 200 Hz～1.2 kHz 的各段 Δ 的均值；net_b = Δ_b − 锚；
  取 |net_b| 的 P95（高于奈奎斯特、或任一边没能量的段不算）。「scipy 同口径滤波器 / 读 trace」二选一：选 FFT 按段积分（trace 里没有段功率；
  段边界与 bin 分摊规则跟 Balance 分析一样，离线用更长的窗把 40 Hz 段分得开）。出处：研究目录 `R5-control-evaluation.md:123`（S 组：主段 1/3 倍频程、
  200 Hz～1.2 kHz 锚区归一，没有目标时只报输出 − 输入）。
- **`gain_tv_db_per_min` / `gain_sign_changes_per_min`**（B9）：trace `limit_gain_db`（每块实际交给限幅器的 Limit Gain，含 G_trim 与守门退让）。
  「稳定段」= 主段块；只取前后两块都在主段的相邻对：总变差 = Σ|ΔGain|，变号 = 非零 ΔGain 序列里正负翻转的次数，都除以主段分钟数。
  出处：vault `01-框架/设计规格-实时GLAZE-主段与增益.md:136`（第 7 节第 5 条「不拉锯」）；研究目录 `R5-control-evaluation.md:134`（K 组）。
- **`guard_engaged_pct` / `guard_cause_hist`**（B9，只报）：原曲时长内的块里 trace `guard_cause` ≠ none 的占比，与各原因（budget / deliveryPeak /
  contrast / psr）各占多少（%）。`guard_cause` 读 `Status.guardCause`（`Source/dsp/GlazeController.cpp:909-934`：几扇门都在退时退得最多的那扇）。
- **`main_level_err_lu`**（B1 第二条，只报）：主段出口 S（窗终点落在主段、S > −70 LUFS）的能量平均 − `metrics.csv` 的 `plan_effective_target_lufs`。
  出处：vault `01-框架/设计规格-实时GLAZE-主段与增益.md:135`（热启动第二遍主段输出响度与方案目标差 ≤ 0.3 LU）。成品就是热启动的第二遍。
- **`hard_clip_samples`**（A 组）：trace 各块 `hard_clip_samples` 之和（含开头延迟段与余振）；`LimiterModule` 在硬钳位处真计数（`Source/dsp/LimiterModule.cpp`
  硬钳位那一行，只读、不改样本）。
