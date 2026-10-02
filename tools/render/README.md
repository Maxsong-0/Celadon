# celadon_render · 真歌离线对照工具

> v0.1.7.0 修坯 Clay 引入，四个子项目（修坯 / 推响不刺 / 目标频谱 / 发行安全）共用。
> 规格：设计规格-Clay-分析决策与验收 第 7 节；决策 ADR-009。源码 `tools/render/RenderMain.cpp`，服务器脚本 `scripts/render-linux.sh`。
> **默认只在测试服务器上编译运行**（ADR-002 修订 4；服务器故障时以 Max 当场指令为准，修订 5）。

## 用途

合成信号证明不了「好听」。把 3～5 首风格各异的真歌（至少一首低音很宽 / 有反相内容）按设置网格过整条链离线渲染，
所有成品对齐到同一整体响度供盲听，另出一张指标表，把听感结论和数字放在一起看。听感结论写进版本档。

默认网格：GLAZE 0 / 35 / 70 × 修坯 自动 / 全关 × 风格 自动 = 每首歌 6 组，另加每个 GLAZE > 0 一组「同决策重放 · 修坯关」，共 8 个成品。

- **修坯「自动」**= 高级抽屉的默认（低切 / 收单 / 加宽三项都「自动」，由 GLAZE 决策 + 实时保护给值）；
- **修坯「全关」**= 三项都选「关」（永远 0，GLAZE 多大都不处理）。这条链自己跑 GLAZE，是「用户关掉修坯」的真实效果；
- **同决策重放 · 修坯关**（`clay-replay-off`，ADR-016）= 照搬「自动」那条链逐块实际交给模块的参数、只把修坯换成「关」。两条链决策
  一模一样，差别只剩修坯本身：**修坯门槛和「修坯到底改了什么」的盲听都用它对比**（见下「同决策重放」）；
- 其余参数全是新实例默认（Limit Ceiling -1.0 dBTP、Lookahead 2 ms、Gain 0、Color / Balance 手动值 0 等），取自
  `Source/ParameterDefaults.h` 的 `factoryChainParams()`——插件参数表读的同一处真源，处理器 Debug 版构造时逐项核对两边一致
  （T-032 R10-01，2026-09-29；此前在 `makeFactoryParams` 里照参数表手写，0.1.9.0 的高级母带 8 项没跟上，成品是两项关着的链）。
  高级母带按新实例默认：推响不刺「自动」50 %、瞬态保留 50 %、目标频谱「自动」35 %（两项实际强度随 GLAZE 联动，GLAZE 0 时为 0），
  编码余量 0、导出抖动关。metrics.md 头部写明本次的高级母带设置。**T-032 之前出的成品与指标表不含这两项**，门槛要按新工具重跑。

## 用法

### 一键：服务器编译 + 渲染 + 拉回本机（推荐）

```bash
scripts/render-linux.sh <本地歌曲目录> [celadon_render 的额外参数…]

# 例
scripts/render-linux.sh ~/Music/Celadon对照
scripts/render-linux.sh ~/Music/Celadon对照 --glaze 0,50 --style auto,warm --match-lufs -14
```

脚本做的事（照 `scripts/test-linux.sh` 的做法）：

1. 把工作树（不含 `artifacts/`）与 `../JUCE/modules` rsync 到服务器；
2. 把歌曲目录第一层的 wav / wave / bwf / aif / aiff / aifc / flac rsync 到服务器的歌曲缓存（按大小 + 修改时间增量，同一目录再跑不重传）；
3. 远端 `cmake` + `cmake --build --target celadon_render`（Release，Ninja；`celadon_render` 是 EXCLUDE_FROM_ALL 目标，测试构建不编它）；
4. 远端先跑 `celadon_render --selftest`（几秒钟，见下「自检」），通不过就停；
5. 远端运行 `celadon_render --in <歌曲缓存> --out <本次输出> [额外参数]`（`nice -n 10`，单线程）；
6. 把输出目录拉回本机，再删掉服务器上的本次输出。

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `CELADON_TEST_HOST` | `maxsong@192.168.100.107` | 目标主机（与 test-linux.sh 相同；需在家里网络或 VPN 下） |
| `CELADON_RENDER_OUT` | `<仓库>/artifacts/render-<日期时间>/` | 本机结果目录 |
| `CELADON_RENDER_SKIP_SELFTEST` | 不设 | 设为 `1` 跳过渲染前的自检（只在明知自检某项因别的改动失败、仍要先听时用） |

`--in` / `--out` 由脚本给，不要再传。服务器上的布局：

| 远端路径 | 内容 |
|---|---|
| `~/.cache/celadon-render/<工作树名>-<哈希>/Celadon`、`JUCE` | 源码（与 test-linux.sh 的目录分开：各自 `rsync --delete` 互不删对方，渲染跑十几分钟也不挡测试） |
| `~/.cache/celadon-render-build/<工作树名>-<哈希>/` | 构建目录（第一次要编 JUCE 模块，之后增量） |
| `~/.cache/celadon-render/songs-<歌曲目录哈希>/` | 歌曲缓存（不自动删；清理：`ssh <主机> 'rm -rf ~/.cache/celadon-render/songs-*'`） |
| `~/.cache/celadon-render/<工作树名>-<哈希>/out-<运行号>/` | 本次输出（拉回成功后删除；拉回失败时保留，脚本会打印路径） |

### 直接调用（已在服务器上编好时）

```bash
celadon_render --in <歌曲目录> --out <输出目录> [--glaze 0,35,70] [--clay auto,off] [--style auto] [--block 512] [--match-lufs -16] [--metrics-only] [--trace]
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `--in` | 必填 | 歌曲目录：只读第一层的 wav / wave / bwf / aif / aiff / aifc / flac（按扩展名交给对应格式；.wave / .aifc 按内容认），跳过隐藏文件 |
| `--out` | 必填 | 输出目录（不存在就建；不能与 `--in` 相同） |
| `--glaze` | `0,35,70` | GLAZE 取值 0～100，逗号分隔 |
| `--clay` | `auto,off` | `auto` = 三项都「自动」，`off` = 三项都「关」；`lowcut` / `mono` / `width` = 只留这一项「自动」、另两项「关」（拆分诊断：是哪一项改变了成品） |
| `--style` | `auto` | GLAZE 风格 `auto` / `warm` / `punchy` / `clean`，可逗号分隔多个（也是网格的一维） |
| `--block` | `512` | 宿主块长（样本，1～65536），两遍都用它 |
| `--match-lufs` | `-16` | 成品按整体响度只乘增益对齐到这个值（-60～0 LUFS） |
| `--metrics-only` | 关 | 照常渲染并量指标，但不写成品 wav（大批歌做门槛验收时用：几十首 × 8 组的 float wav 有好几 GB） |
| `--trace` | 关 | 每组另写 `trace/<成品名>.csv`：第二遍逐块的限幅 GR、实际 Limit Gain / Tone / Glue / Heat、修坯方案与保护状态、分析器慢路 / 快路低频相关度、峰值守护动作的样本数与最小 g、守护混合损失的两份 K 加权能量（`blend_intended_energy` / `blend_output_energy`，T-043）、校准状态；另写 `trace/<成品名>__listen.csv`：第一遍逐块的校准状态（诊断用；列见下「逐块追踪的校准列」；一首 3 分钟的歌两份合计约 4 MB） |
| `--no-replay` | 关 | 不做同决策重放（默认每组 GLAZE > 0 的修坯「自动」之后多一组 `clay-replay-off` 与阳性对照，见下） |
| `--selftest` | — | 只跑自检，不读歌（其余选项忽略） |

进度、警告、错误都写 stderr。返回 0 = 全部成功；1 = 有文件读 / 写失败（其余照做，指标表照写）或自检失败；2 = 参数错误。
采样率按文件自己的（8 k～768 kHz）；单声道复制成双声道，多声道只取前两个；输入里的 NaN / Inf 样本按 0 处理（备注里写数量，
stderr 也报一次；插件是在处理器层把含非有限值的整块清零，这里逐样本清）。

## 渲染流程（每首歌 × 每组设置）

1. **第一遍（聆听）**：新链 → `setParams` → `prepare`（与 `PluginProcessor::prepareToPlay` 同一顺序），按离线宿主口径
   （`HostInfo.offline = true`、在播、位置从 0 起随块推进）把整首歌送进链，输出丢弃。GLAZE 在这一遍里学画像（15～30 s 有效音频）、
   走 3 s 方案过渡、校准（条件稳定 1 s + 3 s settle + 10 s 采样）——歌至少要一分钟左右，第一遍才走得到「已锁定」。
2. **存档**：`createGlazeStateXml()`，即宿主存工程时 GLAZE 那一节（已提交版本 + 执行快照 EXECUTION，含 Clay 四个实际值与保护状态）。
3. **第二遍（成品）**：另起一条新链 → `restoreGlazeStateXml()` → `setParams` → `prepare`（= 重开工程后离线导出），从头渲染整首歌，
   末尾补「链延迟 + 0.35 s 余振」个零把尾巴冲出来（余振与插件报给宿主的尾巴同一常量 CeladonChain::filterRingOutSeconds），丢掉开头「链延迟」个样本
   （48 kHz / Lookahead 2 ms 时 282 + Clay 的 1102（线性相位收单 574 + 峰值守护前瞻 528）= 1384 个，ADR-012 / ADR-014）。
   成品 = 原曲长度 + 0.35 s，与原曲、各组成品之间**逐样本对齐**（DAW 里并排放轨、solo 切换即可无缝对比）。
4. **量、对齐、写**：量对齐前成品的指标 → 只乘一个增益把整体响度对齐到 `--match-lufs` → 写 32-bit float wav。

为什么第二遍用「存档 → 新链读回」而不是同一条链跳回位置 0：新链的模块、表、分析器都是干净的（同一条链跳回 0，限幅器延迟线与各滤波器里
还留着歌尾，成品开头会先吐出一段歌尾）；而且这正是用户真实的「听一遍 → 存工程 → 重开 → 离线导出」流程，也是 rendercheck 验过的路径。
成品从第一个样本起就是学好之后的处理。第一遍没走到「已锁定」时（歌太短等），备注里会写明。

## 输出

```text
<输出目录>/
  <歌名>__g<GLAZE>_clay-<auto|off|replay-off>_<风格>.wav   成品，32-bit float，已按整体响度对齐，允许 > 0 dBFS
  metrics.md                                     指标表（人看），每做完一首歌重写一次
  metrics.csv                                    同一份数据（脚本 / 表格软件用）
```

例：`Song A__g35_clay-auto_auto.wav`。同名不同扩展名的两首（`a.wav`、`a.flac`）歌名后加扩展名区分（`a-wav`、`a-flac`）。

### 指标（metrics.md / metrics.csv）

每首歌一张表：第一行「原始输入」（立体声化后的原曲，同一套量法）作对照，之后每组设置一行。

| md 列 | csv 列 | 含义 |
|---|---|---|
| — | `kind` | `input` = 原始输入，`render` = 成品 |
| 组合 | `glaze` / `clay` / `style` | 这一组的设置 |
| LUFS（对齐前） | `lufs` | 插件自己的 BS.1770-4 表（`LoudnessMeter`，与界面 OUT 表同一算法）量整段对齐前成品 |
| 对齐增益 dB | `align_gain_db` | `--match-lufs` − 对齐前 LUFS；成品只乘这一个增益 |
| 真峰值 dBTP | `true_peak_dbtp` | 插件的 8× 真峰值检测器，对齐前；默认 Ceiling 下应 ≤ -1.0 dBTP（+0.01 dB 容差） |
| PLR dB | `plr_db` | 真峰值 − 整体响度（与对齐无关） |
| 限幅 GR 平均 / 最大 | `limit_gr_mean_db` / `limit_gr_max_db` | 第二遍每块读一次 `getAndResetLimitGainReductionDb()`（块内最大压缩量）：平均按块长加权（「块峰值」的平均，比逐样本平均偏高；静音段计 0），最大 = 全曲最大 |
| 低频相关度 < 125 Hz | `low_corr_lt125hz` | 左右各过 4 阶 Butterworth 125 Hz 低通后 ρ = 2ΣLR / (ΣL² + ΣR²) = (P_M − P_S) / (P_M + P_S)，与分析器 lowCorrelation 同一口径：1 单声道、0 不相关、−1 反相 |
| 真次声 < 16 Hz dB | `infra_share_lt16hz_db` | M = (L + R) / 2 过 8 阶 Butterworth 16 Hz 低通后的能量 ÷ M 全频能量，与分析器 subShareDb 同一定义（ADR-012；整段计，不设有效音频门；侧声道里的次声不计入） |
| 代价限 | `clay_cost_limited` | 先试后用（ADR-012）：成品末尾哪几项因「试过会抬高峰值」被减量 / 否决（lowcut / mono / width，空 = 没有） |
| 低切 Hz / 收单 Hz / 收单量 / 加宽 dB | `clay_lowcut_hz` / `clay_mono_hz` / `clay_mono_amount` / `clay_width_db` | 成品末尾 `getGlazeStatus()` 的 Clay 四个实际值（上一块交给 ClayModule 的值）；低切 0 =「关」；收单量 0 时收单 Hz 无意义 |
| 保护介入 反相 / 宽度 | `phase_guard_pct` / `width_guard_pct` | 第二遍里反相保护 / 宽度保护介入的时长占比 |
| GLAZE 状态 | `glaze_phase` / `calibration_valid` / `resolved_style` | 成品末尾的阶段（unlearned / listening / planTransition / calibrating / locked）、校准是否有效、「自动」风格此刻偏向哪种 |
| — | `listen_phase` | 第一遍（聆听）结束时的阶段 |
| — | `audio_hash` | 对齐前成品（输入行是立体声化的输入）的 FNV-1a 哈希：同一首歌同一组设置跨版本哈希相同 = 逐位不变 |
| 成品文件 | `file` | 成品文件名；写失败为空 |
| — | `trial_lowcut_cost_db` / `trial_mono60_cost_db` … `trial_mono160_cost_db` / `trial_width_cost_db` / `trial_seconds` | 成品末尾已提交画像里「先试后用」量到的代价（dB）与试用的有效时长（诊断：试用只看聆听的前 15 ～ 30 s） |
| — | `profile_sub_share_db` / `profile_low_corr` | 同一份画像的真次声占比与低频相关度（聆听窗口里的值，可能与整首的 `infra_share_lt16hz_db` / `low_corr_lt125hz` 不同） |
| — | `peak_guard_pct` / `peak_guard_min_gain` | 修坯峰值守护（ADR-014）第二遍里 g < 1 的样本占比与全曲最小 g |
| — | `slow_inverted_pct` | 分析器慢路低频相关度有效的块里 ρ < −0.2 的占比：歌里有没有真反相段（门槛「反相保护 ≤ 5 %」只管这一列为 0 的歌） |
| — | `limit_input_peak_db` | 限幅器入口峰值：max(GR − 本块实际 Limit Gain)，只计 GR > 0 的块。扣掉两条链各自校准出的增益差，修坯门槛比的是它（ADR-014） |
| — | `clay_pressure_scale` / `clay_pressure_steps` | 成品末尾修坯压力反馈的缩放（1 = 不限）与降档次数（ADR-014） |
| — | `clay_peak_cost_db` / `clay_peak_cost_s` | T-044（ADR-028）：成品末尾压力反馈第一触发量的代价——自上次清零（降档 / 新画像）起按有效音乐攒下的 100 ms 块峰值，20·log10(P99 ①② 处理后 / P99 干路)，与「先试后用」的 `trial_lowcut_cost_db` 同一口径（不经守护）；攒满 10 s 才有（不满时为空），持续 1 s 高于 0.2 dB 就降一档。`clay_peak_cost_s` = 攒下的时长 |
| — | `replay_check` / `replay_max_diff_dbfs` | 只在 GLAZE > 0 的「自动」行：同决策重放的阳性对照 ok / mismatch / n/a 与重放对自动链的最大样本差（`-inf` = 逐位相同；允许 ≤ −180 dBFS） |
| 有效音乐（表头） | `music_seconds` | 这段音频 K 加权 100 ms 子块在 −70 LUFS 绝对门以上的总时长（与校准器同一门限；歌尾静音不算）。门槛看原始输入行的值 |
| — | `trial_blend10_db` / `trial_blend20_db` | T-043（ADR-027）：成品末尾已提交画像里「先试后用」量到的峰值守护混合损失（10 / 20 Hz 低切候选，K 加权「本该 / 实际」，dB）。决策器按两点幂律把低切频率封顶在预测 0.09 dB 处 |
| — | `clay_blend_loss_db` | T-043：第二遍链里修坯峰值守护的混合损失（全曲两份 K 加权能量之比，dB；低切 / 收单都没作用过时为空）。正 = 守护把低切拉回干路时与干路相位不同、互相抵消，成品比修坯本该给的更轻；与同决策重放的响度差几乎一一对应（woqu：0.215 dB ↔ −0.199 LU） |

`clay-replay-off` 行是 GLAZE 0 的链，GLAZE 状态列没有意义：`glaze_phase` 写 `replay`，`calibration_valid` / `resolved_style` / `listen_phase` 空。

### 逐块追踪的校准列（`--trace`）

两份追踪末尾都有同一组校准列（第一遍那份只有 `t_sec`、`limit_gain_db` 加这组列）。T-015 / BUG-003「整首停在校准中」就是靠它们查出来的：
先看 `cal_begins` / `cal_finishes` / `feedback_steps` 怎么涨，再看卡住的那段 `cal_interrupt_mask` / `cal_gate_mask` 是哪一位。

| 列 | 含义 |
|---|---|
| `glaze_phase` | 阶段：0 未学习、1 聆听中、2 方案过渡、3 校准中、4 已锁定 |
| `cal_round_active` / `cal_state` | 校准轮进行中；校准器状态 0 空闲、1 等待（settle / 屏障）、2 采集、3 结束 |
| `cal_interrupt_mask` | 轮中本块作废当前子窗的原因（位或）：1 宿主旁通 / 暂停、2 GLAZE ≤ 0、4 方案过渡中、8 条件与本轮参考不一致、16 受手动设置限制、32 保护层介入。偏差变了由校准器自己当作中断（看 `executed_bias_db`） |
| `cal_gate_mask` | 不在轮中时没开新一轮的原因（位或）：1 没有已提交画像、2 GLAZE ≤ 0、4 旁通 / 暂停、8 方案过渡中、16 受手动限制、32 保护层在退让或恢复、64 参考条件未知、128 已提交的校准适用（不需要）、256 用户条件稳定不足 1 s、512 刚对同一条件试过 |
| `cal_rounds_done` / `cal_subwindows` / `cal_subwindow_s` | 本次校准已完成的轮数、本轮已完成的子窗数（每轮 4 个）、当前子窗已采到的有效时长（静音不推进） |
| `cal_guard_s` | 本次校准里保护层介入期间累计的有效音乐；到校准窗长（出厂 10 s）即结束为「预算受限」 |
| `cal_last_error_db` / `cal_suggested_db` / `cal_target_db` | 最近一轮的修正量 e（目标净提升 − 实测净提升）、校准器此刻的建议偏差、方案的目标净提升 |
| `cal_applicable` | 已提交的校准对此刻适用（= 状态行的 calibrationValid） |
| `applied_bias_db` / `executed_bias_db` / `guard_db` | 平滑中的校准偏差、上一块实际执行的偏差、保护层自己的退让 |
| `plan_transition` / `plan_transitions` | 方案过渡中；方案过渡累计次数 |
| `cal_begins` / `cal_finishes` / `feedback_steps` / `feedback_max_db` | 校准轮开始 / 结束累计次数；约束反馈累计次数与当前上限（不限时写 99） |

注意：音乐不足约 15 s 的歌学不完画像；有画像之后一轮校准约 13 s（1 s 条件稳定 + settle + 10 s 采样），偏差 > 0.25 dB 还要第二轮（再约 13 s）。
所以音乐不到 30 s 左右的歌，成品末尾停在「校准中」多半是时长不够，不是故障——先看原曲有效音乐多长（歌尾长静音不算）。

### 自检（`--selftest`）

这个工具不在 `celadon_tests` 里（单独的 CMake 目标），所以自带一组合成信号上的阳性对照，`render-linux.sh` 渲染真歌前先跑：

| 项 | 信号 | 期望（依据） |
|---|---|---|
| 低频相关度 | 60 Hz 正弦：左右相同 / 反相 / 右 = 0.5 × 左；两路不相关噪声 | 1 / −1 / 0.8（= 2g / (1 + g²)，皮尔逊会给 1）/ \|ρ\| < 0.1（约 2500 个独立样本，5 倍标准差） |
| 真次声占比 | 0.5·sin 10 Hz + 0.5·sin 1 kHz | 10·log10(0.5 / (1 + (10/16)^16)) ≈ −3.013 dB ± 0.05 |
| 整体响度 | 1 kHz、两声道 −20 dBFS 正弦 | −20.0 LUFS ± 0.1（BS.1770：0 dBFS 单声道 −3.01 LUFS） |
| 真峰值 | 1 kHz、幅度 0.5 | −6.02 dBTP ± 0.1 |
| 哈希 | 同一段噪声 / 改一个样本最低位 | 相同 / 不同 |
| 文件名 | g35 自动、g12.5 全关温润 | `Song A__g35_clay-auto_auto.wav`、`Song A__g12.5_clay-off_warm.wav` |
| 整条链 | 两路白噪声 −26 dBFS 峰值，GLAZE 0、出厂参数，走真歌同一套两遍流程 | 链延迟 = 282 + Clay 的 D + G（48 kHz / 2 ms 为 1384 样本）；成品长度 = 输入 + 0.35 s；成品对输入的互相关最大在滞后 0 且 > 0.9；修坯自动与全关逐位相同 |
| 同决策重放 | 上面的白噪声加两声道同相 8 Hz 正弦 0.1（让自动低切有东西可切），20 s，GLAZE 35 自动，两遍流程 | 修坯「手动」照填重放与自动链最大样本差 ≤ −180 dBFS（实测逐位相同；细节报 GLAZE 改过 Limit Gain 的块数、自动低切开着的块数、推响不刺与目标频谱都在起作用的块数）；重放全关的延迟与长度与自动链相同 |
| 重放覆盖高级母带（T-032） | 同上（出厂默认的推响不刺 / 目标频谱两项「自动」） | 有块的两项实际强度都 > 0（证明上一项覆盖了它们）；把逐块参数的 mastering 换回原始设置（T-032 之前的回写方式）再重放，与自动链的最大样本差 > −120 dBFS（对照有牙：GLAZE 0 的重放链把两项「自动」算成 0） |

### 自带的阳性对照：GLAZE 0 逐位核对

规格要求 GLAZE 0 时修坯「自动」与「全关」都与加 Clay 之前逐位相同（设计规格-Clay 第 4 节「逐位直通」）。网格里同时有
`g0 clay-auto` 与 `g0 clay-off` 时，工具比较两组对齐前成品的哈希，结果写进 metrics.md 每首歌的「备注」（也打印到 stderr）。
**「不同」就是实现有问题**，先查再听。跨版本的逐位对照：拿两次运行 metrics.csv 里 GLAZE 0 行的 `audio_hash` 比。

`audio_hash` 只在**同一台机器上的同一种构建**之间可比：成品取决于 libm（限幅软削波逐样本的 tanh、系数里的 exp / pow / sin …）、
FFT 后端（Mac 两个架构都是 vDSP，Windows / Linux 是 JUCE 自带的 FFTFallback）与架构（arm64 上 TruePeak / Balance 用融合乘加，
x86 是分开的乘和加）；Windows 的 UCRT 还按 CPU 是否支持 FMA3 换一套超越函数实现，两台 Windows 电脑之间都可能不同。
所以 Windows / Intel Mac 跑出来的哈希与 Linux、Apple Silicon 的表对不上是正常的——跨平台对照用 `gate_check.py` 的 dB / LU 门槛，
不要拿一个平台的哈希当另一个平台的金标准。

### 同决策重放（ADR-016）与门槛检查

两条各自跑 GLAZE 的链（修坯自动 / 全关）会因为约束反馈、保护层出手的时刻不同，走到不同的方案——反馈上限、Glue / Heat 份额、
校准偏差都可能不同（T-015 之后 WTF g70 两条链上限 1.145 / 0.274 dB，限幅入口峰值差 0.747 dB，而修坯在出峰那一刻是中性的）。
跨链比较量到的多半是方案分岔，不是修坯本身。所以工具在每组 GLAZE > 0 的「自动」之后：

1. 第二遍渲染时逐块记下链实际交给模块的参数（`getEffectiveParams()`：自动增量、校准偏差、保护退让都已算进去；T-032 起
   `mastering` 也是实际值——推响不刺 / 目标频谱的强度在 `harshResolved` / `targetResolved`、编码余量已扣进 Ceiling，原样回喂逐位复现）；
2. **阳性对照**：另起一条 GLAZE 0 的链（编排器不做决策，实际参数逐位等于喂进去的值），逐块喂这些参数，修坯三项用「手动」照填
   自动链那一块的实际值——输出应复现自动链（最大样本差 ≤ −180 dBFS）。不要求逐位：GLAZE > 0 的链按保护层 10 ms 时隙切片处理，
   GLAZE 0 整块处理，歌尾余振衰减到 −600 dBFS 量级时会差一点（2026-09-28 实测 l_kskdhjf、ooaa 差 1e-31）。自动收单比例在
   0、1 之间的块「手动」态表达不了（手动收单固定是 1），目标频谱用了「参考」形状的块重放链没有侧链参考，都记 `n/a`；
3. **重放全关**：同样的逐块参数、修坯三项都「关」，出一组 `clay-replay-off`（写成品、量指标）。

门槛检查：`python3 tools/render/gate_check.py <metrics.csv>`（多首歌分开跑时先把各自的 metrics.csv 合并成一份）。判定（失败返回 1）：

| 项 | 规则 |
|---|---|
| GLAZE 0 逐位直通 | 修坯「自动」与「全关」`audio_hash` 相同 |
| 阳性对照 | GLAZE > 0 的「自动」`replay_check` = ok（n/a 只列说明） |
| 修坯门槛（阈值同 ADR-014） | 「自动」对「重放全关」：限幅入口峰值增加 ≤ 0.3 dB；响度下降 ≤ 0.1 LU（入口峰值同降的列说明）；反相保护介入 ≤ 5 %（有真反相段的歌豁免） |
| 锁定 | 原曲有效音乐 ≥ 30 s 的歌，GLAZE > 0 的成品末尾「已锁定 + 校准有效」；更短的列「时长不够」说明 |
| 真峰值 | 所有成品 ≤ −1.0 + 0.01 dBTP |

「自动」对「全关」的跨链差只打印作参考（`ref:` 列），不判定。旧的跨链门槛脚本 `docs/validation/2026-09-26-clay/render/gate_check.py`
留作 T-012 的证据，不再维护。

## 盲听

成品已按整体响度对齐（响度差是最大的偏见来源），文件名却写着设置——听之前要把名字藏起来：

1. **按歌分组、随机改名，对照表另放**（在结果目录里执行；key 文件不要放进 blind 目录，听完再看）：

   ```bash
   cd artifacts/render-20260926-210000
   song="Song A"                      # 一次只盲听一首歌的各组，组间比较才有意义
   mkdir -p blind
   ls "$song"__g*.wav | awk 'BEGIN { srand() } { printf "%.9f\t%s\n", rand(), $0 }' | sort -n | cut -f2- |
     { n=0; while IFS= read -r f; do n=$((n + 1)); id=$(printf 'X%02d' "$n"); cp "$f" "blind/$id.wav";
       printf '%s\t%s\n' "$id" "$f" >> "blind-key-$song.tsv"; done; }
   ```

2. **找别人帮忙改名更好**：自己跑脚本时 `srand()` 按秒取种子，看不到 key 就行；由别人改名、甚至由别人播放（双盲）更可靠。
3. **DAW 里并排**：各组成品长度相同，与原曲从第 0 个样本起逐样本对齐（成品比原曲多 0.35 s 余振），放在同一工程的几条轨上
   从 0 对齐，用 solo 独奏切换，切换点无缝。
   32-bit float 成品可能超过 0 dBFS（对齐时抬高了较轻的成品）：把监听或 DAW 总线先拉低 6 dB，别让 DA 削波替你做判断。
4. **ABX**：想确认「真的听得出来」时，用 ABX 工具（如 foobar2000 的 ABX Comparator、Lacinato ABX）对两组做 10～16 次试验，
   对的次数显著多于一半才算听得出来。
5. **先听再看表**：metrics.md 里的数字（尤其 Clay 实际值、相关度）会带偏判断；把听感结论写下来之后再对照数字，
   结论与版本号一起写进版本档。
6. 每首至少对比：同一 GLAZE 下修坯「自动」vs「重放全关」（修坯本身的效果，两者 GLAZE 决策相同；「全关」那组还混着方案分岔，
   听的是「用户关掉修坯」的整体结果），以及 GLAZE 0 vs 35 vs 70（整体推力）。
   低音很宽 / 有反相内容的那首重点听低频是否更紧、单声道回放（DAW 里把总线切成单声道）是否更稳。

## 注意

- **默认不在本机跑**：本机 Mac 不编译、不渲染（ADR-002 修订 4；服务器故障时以 Max 当场指令为准）。测试服务器宕机时脚本连不上会直接报错退出。
- **结果体积**：每个成品是 32-bit float（48 kHz 立体声约 23 MB / 分钟），5 首 × 8 组约 4 GB。结果默认放在 `artifacts/` 下；
  `scripts/test-linux.sh` 与本脚本同步源码时都已排除 `artifacts/` 与 `docs/`，渲染结果不会过网。
- **单线程**：一次只跑一条链；DSP 代码没有按多条链并发验证过，服务器也是共用的。5 首 × 8 组在服务器上约十几分钟（每组两遍；重放组两遍单程：阳性对照 + 重放全关）。
- 工具只用 Source/dsp + juce_audio_formats，不依赖界面模块与 juce_audio_processors；参数不经过 APVTS，出厂默认值取
  `Source/ParameterDefaults.h`（插件参数表同一处真源，改默认值只改那里）。
- **数值环境**：整个进程在 FTZ / DAZ 下跑（`main` 开头一次 `juce::ScopedNoDenormals`，T-032 R10-06），与插件的音频线程一致；
  只影响 < 1.2e-38（约 −758 dBFS）的非规格化数。
