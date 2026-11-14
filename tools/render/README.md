# celadon_render · 真歌离线对照工具

> v0.1.7.0 修坯 Clay 引入，四个子项目（修坯 / 推响不刺 / 目标频谱 / 发行安全）共用。
> 规格：设计规格-Clay-分析决策与验收 第 7 节；决策 ADR-009。源码 `tools/render/RenderMain.cpp`，服务器脚本 `scripts/render-linux.sh`。
> **默认只在测试服务器上编译运行**（ADR-002 修订 4；服务器故障时以 Max 当场指令为准，修订 5）。

## 用途

合成信号证明不了「好听」。把 3～5 首风格各异的真歌（至少一首低频有反相段，用来核对反相提示误亮与门槛里的豁免；最好再有一首低音很宽的，用来看低切与加宽）按设置网格过整条链离线渲染，
所有成品对齐到同一整体响度供盲听，另出一张指标表，把听感结论和数字放在一起看。听感结论写进版本档。

默认网格：GLAZE 0 / 35 / 70 × 修坯 自动 / 全关 × 风格 自动 = 每首歌 6 组，另加每个 GLAZE > 0 一组「同决策重放 · 修坯关」，共 8 个成品。

- **修坯「自动」**= 三态默认（低切 / 收单 / 加宽三项都「自动」：低切由 GLAZE 决策给值（含代价封顶与压力反馈缩放），加宽由决策 + 宽度保护给值；收单 ADR-015 起恒为 0，低频反相时只亮提示、不改声音）；
- **修坯「全关」**= 三项都选「关」（永远 0，GLAZE 多大都不处理）。这条链自己跑 GLAZE，是「用户关掉修坯」的真实效果；
- **同决策重放 · 修坯关**（`clay-replay-off`，ADR-016）= 照搬「自动」那条链逐块实际交给模块的参数、只把修坯换成「关」。两条链决策
  一模一样，差别只剩修坯本身：**修坯门槛和「修坯到底改了什么」的盲听都用它对比**（见下「同决策重放」）。T-044 起连 prepare 之前那次
  交给模块的参数也照搬（阶段 1 时读回存档后执行快照的修坯首块值在那一次就用掉；之前只记逐块、第一遍末尾正在降档的歌阳性对照会判 mismatch。
  T-048 阶段 2 起没有执行快照，编排器 prepare 之前不套用存档，那一次交的是手动参数；照搬的做法不变）；
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
4. 远端先跑 `celadon_render --selftest`，再跑 `--selftest-transport`（各几秒钟，见下「自检」与 T-038 的「自检（`--selftest-transport`）」），任一项通不过就停；
5. 远端运行 `celadon_render --in <歌曲缓存> --out <本次输出> [额外参数]`（`nice -n 10`，单线程）；
6. 把输出目录拉回本机，再删掉服务器上的本次输出。

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `CELADON_TEST_HOST` | `maxsong@192.168.100.107` | 目标主机（与 test-linux.sh 相同；需在家里网络或 VPN 下） |
| `CELADON_RENDER_OUT` | `<仓库>/artifacts/render-<日期时间>/` | 本机结果目录 |
| `CELADON_RENDER_SKIP_SELFTEST` | 不设 | 设为 `1` 跳过渲染前的两项自检（`--selftest`、`--selftest-transport`；只在明知自检某项因别的改动失败、仍要先听时用） |

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
               [--bpm 124 [--meter 4/4] [--listen-from-bar 1,33]] [--segments-csv]
               [--live-trace [--live-only] [--live-constants 键=值,…] [--live-tag 名字]]
               [--replay-override color.heat=0]
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `--in` | 必填 | 歌曲目录：只读第一层的 wav / wave / bwf / aif / aiff / aifc / flac（按扩展名交给对应格式；.wave / .aifc 按内容认），跳过隐藏文件 |
| `--out` | 必填 | 输出目录（不存在就建；不能与 `--in` 相同） |
| `--glaze` | `0,35,70` | GLAZE 取值 0～100，逗号分隔 |
| `--clay` | `auto,off` | `auto` = 三项都「自动」，`off` = 三项都「关」；`lowcut` / `mono` / `width` = 只留这一项「自动」、另两项「关」（拆分诊断：是哪一项改变了成品。ADR-015 起自动收单恒为 0，`mono` 这一组的成品与 `off` 逐位相同，只多记反相提示时长 `phase_guard_pct`） |
| `--style` | `auto` | GLAZE 风格 `auto` / `warm` / `punchy` / `clean`，可逗号分隔多个（也是网格的一维） |
| `--block` | `512` | 宿主块长（样本，1～65536），两遍都用它 |
| `--match-lufs` | `-16` | 成品按整体响度只乘增益对齐到这个值（-60～0 LUFS） |
| `--metrics-only` | 关 | 照常渲染并量指标，但不写成品 wav（大批歌做门槛验收时用：几十首 × 8 组的 float wav 有好几 GB） |
| `--trace` | 关 | 每组另写 `trace/<成品名>.csv`：第二遍逐块的限幅 GR、实际 Limit Gain / Tone / Glue / Heat、修坯方案值 / 宽度保护后的值 / 实际值与反相提示状态、分析器慢路 / 快路低频相关度、峰值守护动作的样本数与最小 g、守护混合损失的两份 K 加权能量（`blend_intended_energy` / `blend_output_energy`，T-043）、压力反馈此刻的缩放、第一触发量的代价与攒下的块数、平滑后的混合损失（`clay_pressure_scale` / `clay_peak_cost_db` / `clay_peak_cost_blocks` / `clay_blend_loss_db`，T-044；代价攒满 100 块才有）、校准列（T-048 阶段 2 起多为占位；列见下「逐块追踪的修坯列」「逐块追踪的校准列」）；另写 `trace/<成品名>__listen.csv`：第一遍（预热——旧称「聆听」的那一遍，见下「渲染流程」）逐块的 Limit Gain 与同一组校准列（诊断用；只有 `t_sec`、`limit_gain_db` 和校准列，见下「逐块追踪的校准列」；一首 3 分钟的歌两份合计约 4 MB）。T-096 起第二遍那份末尾再加六列（主段度、守门、限幅器渲染计数），见下「T-096」 |
| `--no-replay` | 关 | 不做同决策重放（默认每组 GLAZE > 0 的修坯「自动」之后多一组 `clay-replay-off` 与阳性对照，见下） |
| `--replay-override` | 不给 | T-096d：同决策重放时覆盖参数（`键=值`，逗号分隔；白名单只有 `color.heat`，0～100）。每组 GLAZE > 0 的修坯「自动」之后另加一行 `clay-replay-heat0`，与阳性对照逐样本相减，差信号写 `heat_ab.csv`（见下「T-096」）；不能与 `--no-replay`、`--live-only` 一起用。不给时一切输出与改动前相同 |
| `--bpm` | 不给 | 模拟宿主走带（20～999 BPM）：每块给速度、PPQ、上一小节起点与拍号（见下「T-038」）。不给时与改动前一样不给拍点 |
| `--meter` | `4/4` | 拍号（分子 1～32，分母 1 / 2 / 4 / 8 / 16 / 32）；要与 `--bpm` 一起用 |
| `--listen-from-bar` | `1` | 第一遍（预热；参数名沿用旧称 listen，见下「渲染流程」）从第几小节起播到歌尾（1 = 从头），可逗号分隔多个（网格的一维）；要 `--bpm`。第二遍（成品）照旧从头渲染 |
| `--segments-csv` | 关 | 逐段写统计到 `segments/`（有 `--bpm` 时一段 = 一小节，否则每 2 秒）：输入端每首一份、成品每组一份（见下「T-038」） |
| `--live-trace` | 关 | T-048：每一遍的链接上实时画像，每发布一份画像写一行追踪到 `live/<标签>/`（见下「T-048」）。阶段 1 时链的输出逐位不变；T-048 阶段 2 起这是外部探针，会把段从链自己的实时画像引走，存档会变、GLAZE > 0 时成品也会变 |
| `--live-only` | 关 | T-048：只跑第一遍（预热）出追踪，不出成品、不做重放——最快的追踪跑法；要 `--live-trace` |
| `--live-constants` | 不给 | T-048：覆盖实时画像常数 `LiveConstants` 的任意字段，`键=值` 逗号分隔（键表见下「T-048」）；不认识的键、不合法的值报错；要 `--live-trace` 与 `--live-tag` |
| `--live-tag` | `default` | T-048：追踪的输出分组 `live/<标签>/`（字母、数字、`.` `_` `-`，不以 `.` 开头，≤ 64 个字符）；扫参时一组常数一个标签 |
| `--selftest` | — | 只跑自检，不读歌（其余选项忽略） |
| `--selftest-transport` | — | 只跑走带选项的自检（见下「T-038」；T-108 前叫 `--selftest-house`），不读歌；`render-linux.sh` 渲染前在 `--selftest` 之后跑它 |
| `--selftest-live` | — | 只跑 T-048 实时画像追踪的自检（见下「T-048」），不读歌；`render-linux.sh` 不跑它，改了相关代码手动跑 |

T-108（ADR-039，2026-11-13）删掉曲风与交付：`--genre`、`--delivery`、`--live-model`、`--streaming-target-lufs`（T-085 的测量覆盖）与
`--selftest-house` 都删了（`--selftest-house` 留下的走带几项改名 `--selftest-transport`），给了报「不认识的选项」。

进度、警告、错误都写 stderr。返回 0 = 全部成功；1 = 有文件读 / 写失败（其余照做，指标表照写）或自检失败；2 = 参数错误。
采样率按文件自己的（8 k～768 kHz）；单声道复制成双声道，多声道只取前两个；输入里的 NaN / Inf 样本按 0 处理（备注里写数量，
stderr 也报一次；插件是在处理器层把含非有限值的整块清零，这里逐样本清）。

## 渲染流程（每首歌 × 每组设置）

1. **第一遍（预热）**：新链 → `setParams` → `prepare`（与 `PluginProcessor::prepareToPlay` 同一顺序），按离线宿主口径
   （`HostInfo.offline = true`、在播、位置从 0 起随块推进）把整首歌送进链，输出丢弃。这一遍以前叫「聆听」：命令行参数、文件名、列名与
   工具打印的文字仍沿用旧称（`--listen-from-bar`、`trace/<成品名>__listen.csv`、`listen_phase` 等）。T-048 阶段 2（ADR-030）起 GLAZE
   没有聆听、锁定与校准：这一遍里编排器按链上的实时画像连续调度（每 1 s 重算方案，自动增量按速率限制推向目标，Limit Gain 另加输出侧
   慢修正 G_trim 与预算环 / 落差 / PSR 三个守门里最深的那份退让），让主段统计、自动增量、G_trim 与守门状态随整首歌热起来——即 ADR-030 决定 6 的
   「先播一遍再导出，导出从头就到位」。（阶段 1 时 GLAZE 在这一遍里学画像（15～30 s 有效音频）、走 3 s 方案过渡、校准（条件稳定 1 s +
   3 s settle + 10 s 采样），歌至少要一分钟左右，第一遍才走得到「已锁定」。）
2. **存档**：`createGlazeStateXml()`，即宿主存工程时 GLAZE 那一节。T-048 阶段 2 起只存 LIVE（GLAZE_STATE formatVersion 2）：实时画像
   （主段统计）、已施加的自动增量与 G_trim、风格权重、三个守门与出口电平的状态、修坯的自动值（ClayGuard 之后、三态合成之前）、
   压力缩放与 ClayGuard（反相提示 / 宽度保护）的状态。阶段 1 存的是已提交版本 + 执行快照 EXECUTION，这两样已随 ADR-030 取消
   （旧存档读进来按冷启动处理）。
3. **第二遍（成品）**：另起一条新链 → `restoreGlazeStateXml()` → `setParams` → `prepare`（= 重开工程后离线导出），从头渲染整首歌，
   末尾补「链延迟 + 0.35 s 余振」个零把尾巴冲出来（余振与插件报给宿主的尾巴同一常量 CeladonChain::filterRingOutSeconds），丢掉开头「链延迟」个样本
   （48 kHz / Lookahead 2 ms 时 282 + Clay 的 1102（线性相位收单 574 + 峰值守护前瞻 528）= 1384 个，ADR-012 / ADR-014）。
   成品 = 原曲长度 + 0.35 s，与原曲、各组成品之间**逐样本对齐**（DAW 里并排放轨、solo 切换即可无缝对比）。
   读回的 LIVE 在 `prepare` 冷启动之后套上（CeladonChain::prepare），所以成品第一块起就接着用读回的已施加增量、G_trim、Heat / Glue 跟随量、预算环退让与落差 / PSR 退让（后两项 T-097 起）；自动 Clay、Tone / Tame 实时跟随要到第一块 endBlock 之后（第二块起）才接上；GLAZE 在这一遍里继续实时跟随
   （链上的实时画像接着收这一遍的段，方案每 1 s 重算）。
4. **量、对齐、写**：量对齐前成品的指标 → 只乘一个增益把整体响度对齐到 `--match-lufs` → 写 32-bit float wav。

为什么第二遍用「存档 → 新链读回」而不是同一条链跳回位置 0：新链的模块、表、分析器都是干净的（同一条链跳回 0，限幅器延迟线与各滤波器里
还留着歌尾，成品开头会先吐出一段歌尾）；而且这正是用户真实的「听一遍 → 存工程 → 重开 → 离线导出」流程，也是 rendercheck 验过的路径
（阶段 1 时验的；`tools/perf/bench.mm` 的 rendercheck 在 T-114 改成核对阶段 2 的 LIVE 存档，原来核对的执行快照 EXECUTION 已随 ADR-030 取消）。
成品从第一块起就接着第一遍末的 LIVE 处理（自动 Clay、Tone / Tame 实时跟随与落差 / PSR 退让晚一块；阶段 1 的说法是「学好之后的处理」）。阶段 1 时第一遍没走到「已锁定」（歌太短等）会在备注里写明；
T-048 阶段 2 起状态里的阶段恒为 unlearned，这条备注（「第一遍结束时 GLAZE 还没学完……」）对 GLAZE > 0 的每组都会出现——工具还没改，不代表第一遍出了问题。

## 输出

```text
<输出目录>/
  <歌名>__g<GLAZE>_clay-<auto|off|replay-off>_<风格>.wav   成品，32-bit float，已按整体响度对齐，允许 > 0 dBFS
  <歌名>__g<GLAZE>_clay-replay-heat0_<风格>.wav   只在给了 --replay-override 时：覆盖 Heat 的同决策重放（T-096d）
  heat_ab.csv                                    同上：每组覆盖重放一行差信号指标（T-096d，见下「T-096」）
  metrics.md                                     指标表（人看），每做完一首歌重写一次
  metrics.csv                                    同一份数据（脚本 / 表格软件用）
  segments/<歌名>__input.csv、segments/<成品名>.csv   --segments-csv 的逐段统计（T-038，见下）
  live/<标签>/<歌名>[__from-b<N>].csv、__final.csv、__judgements.csv、<歌名>__passes.csv   --live-trace 的实时画像追踪（T-048，见下）
```

例：`Song A__g35_clay-auto_auto.wav`。同名不同扩展名的两首（`a.wav`、`a.flac`）歌名后加扩展名区分（`a-wav`、`a-flac`）。
预热起点不是第 1 小节时接在后面：`_from-b<小节>`，如 `Song A__g35_clay-auto_auto_from-b33.wav`（默认组合的文件名与改动前一字不差；
T-108 之前曲风 / 交付不是默认值时还会接 `_<曲风>`、`_<交付>`）。

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
| 反相提示 / 宽度保护（格末） | `clay_cost_limited` | 成品末尾哪几项「自动」修坯因代价被减量 / 否决（`lowcut` / `width`，两项都有写 `lowcut+width`，空 = 没有；ADR-015 起自动不收单，不再出现 `mono`）。三处会置位：先试后用试出会抬高峰值（ADR-012）、低切按守护混合损失封顶（ADR-027）、压力反馈降过档（ADR-014 / ADR-028）。metrics.md 没有单独一列：有值时接在「反相提示 / 宽度保护」那一格末尾（「，代价限：lowcut」），空时不写 |
| 低切 Hz / 收单 Hz / 收单量 / 加宽 dB | `clay_lowcut_hz` / `clay_mono_hz` / `clay_mono_amount` / `clay_width_db` | 成品末尾 `getGlazeStatus()` 的 Clay 四个实际值（上一块交给 ClayModule 的值）；低切 0 =「关」；收单量 0 时收单 Hz 无意义（本工具不用手动收单，ADR-015 起收单 Hz 恒为 80、收单量恒为 0） |
| 反相提示 / 宽度保护 | `phase_guard_pct` / `width_guard_pct` | 第二遍里反相提示亮着（收单灯琥珀；ADR-015 起只提示、不改声音；只在收单为「自动」的组计：`auto`、`mono` 组，其余组与重放行恒为 0）/ 宽度保护介入的时长占比（只在加宽为「自动」的组计）；`phase_guard_pct` 沿用旧列名（改名会断旧指标表） |
| GLAZE 状态 | `glaze_phase` / `calibration_valid` / `resolved_style` | 成品末尾的阶段、校准是否有效、「自动」风格此刻偏向哪种。阶段 1 的阶段取值是 unlearned / listening / planTransition / calibrating / locked；T-048 阶段 2（ADR-030）起没有聆听 / 锁定 / 校准状态机，`glaze_phase` 恒为 unlearned、`calibration_valid` 恒为 0（成品行在 metrics.md 那一格恒以「未学习 · 校准无效」开头；同决策重放行例外：glaze_phase 写 replay，calibration_valid、resolved_style 与 listen_phase 为空，metrics.md 那一格写「同决策重放（…）」），列只为不改表格式而留；`resolved_style` 照常有意义 |
| — | `listen_phase` | 第一遍（预热）结束时的阶段（T-048 阶段 2 起恒为 unlearned；同决策重放行为空） |
| — | `audio_hash` | 对齐前成品（输入行是立体声化的输入）的 FNV-1a 哈希：同一首歌同一组设置跨版本哈希相同 = 逐位不变 |
| 成品文件 | `file` | 成品文件名；写失败为空 |
| — | `trial_lowcut_cost_db` / `trial_mono60_cost_db` … `trial_mono160_cost_db` / `trial_width_cost_db` / `trial_seconds` | 成品末尾画像里「先试后用」量到的代价（dB）与试用的有效时长（诊断）。阶段 1 是已提交画像，试用只看聆听的前 15 ～ 30 s；T-048 阶段 2 起是编排器最近读到的那份实时画像：代价是窗口里按主段权重（含遗忘）累计的实时试算，`trial_seconds` 是同一权重下的有效试算时长。`trial_mono*` 四列 ADR-015 起决策不用，T-048 阶段 2 起停算、恒为空 |
| — | `profile_sub_share_db` / `profile_low_corr` | 同一份画像的真次声占比与低频相关度（阶段 1 是聆听窗口里的值，阶段 2 是实时画像窗口里的加权值；可能与整首的 `infra_share_lt16hz_db` / `low_corr_lt125hz` 不同） |
| — | `peak_guard_pct` / `peak_guard_min_gain` | 修坯峰值守护（ADR-014）第二遍里 g < 1 的样本占比与全曲最小 g |
| — | `slow_inverted_pct` | 分析器慢路低频相关度有效的块里 ρ < −0.2 的占比：歌里有没有真反相段（门槛「反相提示时长占比 ≤ 5 %」只管这一列为 0 的歌）（T-048 阶段 2 起编排器探针不抄此字段，本列恒为 0 / 空，上面的说明是阶段 1 的口径；恢复抄写还是标成死列待 Max 定）（于是 gate_check.py 的反相豁免实际失效） |
| — | `limit_input_peak_db` | 限幅器入口峰值：max(GR − 本块实际 Limit Gain)，只计 GR > 0 的块。扣掉两条链各自施加的 Limit Gain 之差（阶段 1 是校准出的增益差，阶段 2 是自动增量、G_trim 与守门退让的差），修坯门槛比的是它（ADR-014） |
| — | `clay_pressure_scale` / `clay_pressure_steps` | 成品末尾修坯压力反馈的缩放（1 = 不限）与降档次数（ADR-014） |
| — | `clay_peak_cost_db` / `clay_peak_cost_s` | T-044（ADR-028）：成品末尾压力反馈第一触发量的代价——自上次清零（降档 / 新画像）起按有效音乐攒下的 100 ms 块峰值，20·log10(P99 ①② 处理后 / P99 干路)，与「先试后用」的 `trial_lowcut_cost_db` 同一口径（不经守护）；攒满 10 s 才有（不满时为空），持续 1 s 高于 0.2 dB 就降一档。`clay_peak_cost_s` = 攒下的时长（T-048 阶段 2 起编排器探针不抄此字段，本列恒为 0 / 空，上面的说明是阶段 1 的口径；恢复抄写还是标成死列待 Max 定） |
| — | `replay_check` / `replay_max_diff_dbfs` | 只在 GLAZE > 0 的「自动」行：同决策重放的阳性对照 ok / mismatch / n/a 与重放对自动链的最大样本差（`-inf` = 逐位相同；允许 ≤ −180 dBFS） |
| 有效音乐（表头） | `music_seconds` | 这段音频 K 加权 100 ms 子块在 −70 LUFS 绝对门以上的总时长（原与校准器同一门限，校准器 T-048 阶段 2 已删、口径照旧；歌尾静音不算）。门槛看原始输入行的值 |
| — | `trial_blend10_db` / `trial_blend20_db` | T-043（ADR-027）：成品末尾画像（阶段 1 是已提交画像，阶段 2 是实时画像）里「先试后用」量到的峰值守护混合损失（10 / 20 Hz 低切候选，K 加权「本该 / 实际」，dB）。决策器按两点幂律把低切频率封顶在预测 0.09 dB 处 |
| — | `clay_blend_loss_db` | T-043：第二遍链里修坯峰值守护的混合损失（全曲两份 K 加权能量之比，dB；低切 / 收单都没作用过时为空）。正 = 守护把低切拉回干路时与干路相位不同、互相抵消，成品比修坯本该给的更轻；与同决策重放的响度差几乎一一对应（woqu：0.215 dB ↔ −0.199 LU） |

`clay-replay-off` 行是 GLAZE 0 的链，GLAZE 状态列没有意义：`glaze_phase` 写 `replay`，`calibration_valid` / `resolved_style` / `listen_phase` 空。

### 逐块追踪的修坯列（`--trace`）

只在第二遍那份（`trace/<成品名>.csv`）里，一行 = 一块：

| 列 | 含义 |
|---|---|
| `planned_lowcut_hz` / `planned_mono_amount` / `planned_mono_hz` / `planned_width_db` | 决策器给「自动」态的值（含压力反馈缩放，ClayGuard 之前；阶段 1 还含方案过渡混合，T-048 阶段 2 起没有方案过渡，直接取此刻目标方案的值） |
| `live_mono_amount` / `live_width_db` | ClayGuard 之后、三态合成之前的自动值：宽度保护只会把加宽往小里改，收单原样透传 |
| `guard_phase_floor` / `guard_phase_hold_s` / `guard_phase_active` | 反相提示的强度（0～1，含攻击 / 释放）、条件消失后剩下的保持秒数、是否亮着（= 强度 > 0）。列名沿用旧名，ADR-015 起只提示、不改声音；触发条件与时间常数见 `Source/dsp/ClayGuard.h` 文件头。这是 ClayGuard 自己的状态、不看三态：收单不在「自动」的组（`off` / `lowcut` / `width`）也可能是 1，收单灯与 `phase_guard_pct` 只算收单「自动」的组 |
| `guard_seconds` | 这一块交给 ClayGuard 推进的有效音乐秒数（= 上一块的有效音乐时长）。为 0 时：静音、宿主旁通、停播，以及读回存档后的第一块——反相提示与宽度保护的状态停在原处；GLAZE 0 的组、或方案源不支持自动 Clay（旧画像 / 没有画像）每块都是 0，且 ClayGuard 不是停住而是回到中性（`guard_*` 与 `planned_*` / `live_*` 全是中性值，哪怕 `rho_slow` 明明 < −0.2）（T-048 阶段 2 起编排器探针不抄此字段，本列恒为 0 / 空，上面的说明是阶段 1 的口径；恢复抄写还是标成死列待 Max 定） |
| `rho_slow` / `rho_fast` | 分析器慢路 / 快路低频相关度（反相提示的判据；无效时空）。这是本块处理后的读数，ClayGuard 下一块才用它（T-048 阶段 2 起编排器探针不抄此字段，本列恒为 0 / 空，上面的说明是阶段 1 的口径；恢复抄写还是标成死列待 Max 定） |
| `clay_lowcut_hz` / `clay_mono_amount` / `clay_mono_hz` / `clay_width_db` | 三态合成后交给 ClayModule 的实际值（同指标表的 Clay 四列） |

ADR-015 起「自动」不收单：本工具各组的 `planned_mono_amount`、`live_mono_amount`、`clay_mono_amount` 恒为 0，`planned_mono_hz` 恒为 80。

低切不经 ClayGuard 改动，所以没有 `live_lowcut_hz` 列：`planned_lowcut_hz` 就是交给三态合成的自动低切。`guard_phase_hold_s` 在反相条件成立期间保持为整段保持期（出厂 2 s），条件消失后才开始倒数。
峰值守护与压力反馈的列（`peak_guard_samples`、`peak_guard_min_gain`、`blend_intended_energy`、`blend_output_energy`、`clay_pressure_scale`、`clay_peak_cost_db`、`clay_peak_cost_blocks`、`clay_blend_loss_db`）不在这一节，口径见上面 `--trace` 一行与指标表里的同名列。

### 逐块追踪的校准列（`--trace`）

两份追踪末尾都有同一组校准列（第一遍那份只有 `t_sec`、`limit_gain_db` 加这组列）。阶段 1 时 T-015 / BUG-003「整首停在校准中」就是靠它们查出来的：
先看 `cal_begins` / `cal_finishes` / `feedback_steps` 怎么涨，再看卡住的那段 `cal_interrupt_mask` / `cal_gate_mask` 是哪一位。

**ADR-030 / T-048 阶段 2 起这组列是占位**：没有校准器、方案过渡与约束反馈，编排器不再写探针里的这些字段，列保留只为不改 CSV 格式（列名与次序照旧）。
还有读数的只剩 `cal_target_db`、`applied_bias_db` / `executed_bias_db`、`guard_db`（见下表「阶段 2」一栏）；第一遍那份另有 `limit_gain_db`（本块实际交给限幅器的 Limit Gain）。
原定探针删掉之后把这组列换成 confidence、g_trim_db、protection_db、guard_cause；那要改列名、改 CSV 格式，须另立任务（先问 Max）。读阶段 1 的旧追踪时看「阶段 1 的含义」一栏。

| 列 | 阶段 1 的含义 | 阶段 2（现行）写的值 |
|---|---|---|
| `glaze_phase` | 阶段：0 未学习、1 聆听中、2 方案过渡、3 校准中、4 已锁定 | 恒为 0 |
| `cal_round_active` / `cal_state` | 校准轮进行中；校准器状态 0 空闲、1 等待（settle / 屏障）、2 采集、3 结束 | 恒为 0 / 0 |
| `cal_interrupt_mask` | 轮中本块作废当前子窗的原因（位或）：1 宿主旁通 / 暂停、2 GLAZE ≤ 0、4 方案过渡中、8 条件与本轮参考不一致、16 受手动设置限制、32 保护层介入。偏差变了由校准器自己当作中断（看 `executed_bias_db`） | 恒为 0 |
| `cal_gate_mask` | 不在轮中时没开新一轮的原因（位或）：1 没有已提交画像、2 GLAZE ≤ 0、4 旁通 / 暂停、8 方案过渡中、16 受手动限制、32 保护层在退让或恢复、64 参考条件未知、128 已提交的校准适用（不需要）、256 用户条件稳定不足 1 s、512 刚对同一条件试过 | 恒为 0 |
| `cal_rounds_done` / `cal_subwindows` / `cal_subwindow_s` | 本次校准已完成的轮数、本轮已完成的子窗数（每轮 4 个）、当前子窗已采到的有效时长（静音不推进） | 恒为 0 / 0 / 0.00 |
| `cal_guard_s` | 本次校准里保护层介入期间累计的有效音乐；到校准窗长（出厂 10 s）即结束为「预算受限」 | 恒为 0.00 |
| `cal_last_error_db` / `cal_suggested_db` / `cal_target_db` | 最近一轮的修正量 e（目标净提升 − 实测净提升）、校准器此刻的建议偏差、方案的目标净提升 | 前两列恒为 0.000；`cal_target_db` 照常是此刻目标方案的约束后净提升（`effectiveGainDb`） |
| `cal_applicable` | 已提交的校准对此刻适用（= 状态行的 calibrationValid） | 恒为 0 |
| `applied_bias_db` / `executed_bias_db` / `guard_db` | 平滑中的校准偏差、上一块实际执行的偏差、保护层自己的退让 | 前两列都是 G_trim（输出侧慢修正，取代校准偏差；两列同值）；`guard_db` 是预算环此刻的退让（落差 / PSR 守门的退让不在这一列） |
| `plan_transition` / `plan_transitions` | 方案过渡中；方案过渡累计次数 | 恒为 0 / 0 |
| `cal_begins` / `cal_finishes` / `feedback_steps` / `feedback_max_db` | 校准轮开始 / 结束累计次数；约束反馈累计次数与当前上限（不限时写 99） | 恒为 0 / 0 / 0 / 99.000 |

注意（阶段 1 的口径，读旧追踪时用）：音乐不足约 15 s 的歌学不完画像；有画像之后一轮校准约 13 s（1 s 条件稳定 + settle + 10 s 采样），偏差 > 0.25 dB 还要第二轮（再约 13 s）。
所以音乐不到 30 s 左右的歌，成品末尾停在「校准中」多半是时长不够，不是故障——先看原曲有效音乐多长（歌尾长静音不算）。
阶段 2 没有学习期与校准轮，「时长够不够」改看置信度 c（门槛：有效音乐 ≥ 30 s 的歌成品末尾 c ≥ 0.5，见下「同决策重放（ADR-016）与门槛检查」）；c 不在 `--trace` 的追踪里，指标表的 `confidence` 列（T-096a 起填成品末尾的 c，见下「T-038」一节开头）有它。`--live-trace` 追踪里的 `confidence` 是追踪器自建那份实时画像的 c，不是链上那一份的。

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
| `--trace` 不改成品（T-096a） | 同一组设置带 `--trace` 再跑第二遍 | 成品与不追踪时逐位相同；表头以追加的六列收尾、每行字段数与表头相同 |
| 限幅器渲染计数（T-096a） | 白噪声进限幅器，Gain 0 / +18 dB，计数开 / 关各一遍 | 开 / 关输出与每块 GR / Clip 统计逐位相同；关着时三项都是 0；Gain 0 的差值能量恰为 0，+18 dB 时 0 < 差值 < 入口 |
| 重放覆盖（T-096d） | 合成信号上自动 Heat > 0 的一组 | 非法写法都报错；文件名 / 标签；参数核对覆盖 ChainParams 全部字段且抓得住单个字段的改动；覆盖值 = 记录值时与 replay-copy 逐位相同；差信号各指标的已知答案；prepare 前那份与每一块都被覆盖（BUG-012：在参数层判，见下「T-096」） |

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
以上是阶段 1 的情形；T-048 阶段 2 起没有约束反馈与校准偏差，但 G_trim、预算环与落差守门看的是修坯之后的量（出口响度、模块统计），
修坯开关一改，两条链照样会分岔。跨链比较量到的多半是方案分岔，不是修坯本身。所以工具在每组 GLAZE > 0 的「自动」之后：

1. 第二遍渲染时逐块记下链实际交给模块的参数（`getEffectiveParams()`：自动增量、G_trim（阶段 1 是校准偏差）、守门退让都已算进去；T-032 起
   `mastering` 也是实际值——推响不刺 / 目标频谱的强度在 `harshResolved` / `targetResolved`、编码余量已扣进 Ceiling，原样回喂逐位复现）；
2. **阳性对照**：另起一条 GLAZE 0 的链（编排器不做决策，实际参数逐位等于喂进去的值），逐块喂这些参数，修坯三项用「手动」照填
   自动链那一块的实际值——输出应复现自动链（最大样本差 ≤ −180 dBFS）。不要求逐位：GLAZE > 0 的链按预算环 10 ms 时隙与 OUT 表 100 ms 子块中较近的边界切片处理（T-097 起），
   GLAZE 0 整块处理，歌尾余振衰减到 −600 dBFS 量级时会差一点（2026-09-28 实测 l_kskdhjf、ooaa 差 1e-31）。自动收单比例在
   0、1 之间的块「手动」态表达不了（手动收单固定是 1），目标频谱用了「参考」形状的块重放链没有侧链参考，都记 `n/a`，原因写进
   metrics.md 该曲备注。ADR-015 起自动不收单，前一种已不再出现；本工具的目标频谱固定是出厂「自动」（不会落到「参考」），后一种眼下也走不到；
3. **重放全关**：同样的逐块参数、修坯三项都「关」，出一组 `clay-replay-off`（写成品、量指标）。

门槛检查：`python3 tools/render/gate_check.py <metrics.csv>`（多首歌分开跑时先把各自的 metrics.csv 合并成一份）。判定（失败返回 1）：

| 项 | 规则 |
|---|---|
| GLAZE 0 逐位直通 | 修坯「自动」与「全关」`audio_hash` 相同 |
| 阳性对照 | GLAZE > 0 的「自动」`replay_check` = ok（n/a 只列说明） |
| 修坯门槛（阈值同 ADR-014） | 「自动」对「重放全关」：限幅入口峰值增加 ≤ 0.3 dB；响度下降 ≤ 0.1 LU（入口峰值同降的列说明）；反相提示时长占比 ≤ 5 %（「自动」行自己的收单灯琥珀时长；ADR-015 起只提示、不改声音，这条查提示误亮；有真反相段的歌豁免） |
| 热起 / 锁定 | 行里 `confidence` 非空：有效音乐 ≥ 30 s 的歌，GLAZE > 0 的成品末尾 c ≥ 0.5。格子空着或没有这列：仍要求「已锁定 + 校准有效」（阶段 1 表的输出一字不差）。更短的列「时长不够」说明。T-096a 起渲染工具填 `confidence`，按 c 判；T-096a 之前的表 `confidence` 空、`glaze_phase` 恒为 unlearned，这一条会对有效音乐 ≥ 30 s、GLAZE > 0 的格子判失败 |
| 真峰值 | 所有成品 ≤ −1.0 + 0.01 dBTP |

「自动」对「全关」的跨链差只打印作参考（`ref:` 列），不判定。旧的跨链门槛脚本 `docs/validation/2026-09-26-clay/render/gate_check.py`
留作 T-012 的证据，不再维护。`gate_check.py` 按（歌, GLAZE, 风格）配对；指标表末尾有 T-038 追加列时，`listen_from_bar` 也并入配对键——`--listen-from-bar`
多值网格里同一首歌同一 GLAZE 的几组「自动」各自成组判。它取值不止一种时，失败 / 说明的分组标签（如 `Song A g35 from-b33`）与表格（接在 `g` 列后）
才写出它；没有这列、或只有一种取值时，输出与只按（歌, GLAZE, 风格）配对时逐字节相同。T-108 之前 `genre` / `delivery` 两列也并入配对键。

## T-038：模拟走带、预热起点与逐段统计

T-038 加了曲风、交付、模拟走带与逐段统计四样；T-108（ADR-039，取代 ADR-026）删掉曲风与交付，留下的走带几项与改动前同口径。
**都不给走带选项时与改动前逐位相同**：HostInfo、网格、文件名、metrics.md 头部与 metrics.csv 一字不差。T-048 阶段 2 起连续编排器也读 PPQ，
给拍点的成品可以与不给时不同（见下「自检（`--selftest-transport`）」）。只要给了任一走带选项，metrics.csv 末尾就追加下面的列、metrics.md 头部多一行。
T-048 阶段 2 另外在每张表末尾固定加上 `confidence,current_mainness,cold_export,guard_cause,protection_db,status_line`
（接在 `clay_peak_cost_s` 之后、T-038 列之前），因此默认 metrics.csv 不再与阶段 1 的表逐字节相同。T-096a 起前四列填成品末尾 Status 的
c、此刻主段度、冷导出（离线且 c < 0.5）与守门原因（`none` / `budget` / `limiterPeak`），重放行留空；`status_line` 仍写空；`protection_db` 写状态行
显示的那扇守门的退让（GLAZE 0 或没有守门在起作用时为 0）。两遍渲染和成品 wav 走的已是阶段 2 的连续编排器；`--live-trace` 仍自建一份 LiveProfile
接在外部探针上（改成读链内那一份的 `readCommits` / `getDiagnostics` 要另改工具，见下「T-048」）。
有 `confidence` 时 `gate_check.py` / `listen_start_check.py` 按 c ≥ 0.5 判；T-096a 之前的旧表 `confidence` 空，`gate_check.py` 退回阶段 1 的判定
（`glaze_phase` = locked 且 `calibration_valid` = 1），而阶段 2 的 `glaze_phase` / `listen_phase` 恒为 unlearned、`calibration_valid` 恒为 0，
所以拿旧表跑门槛，有效音乐 ≥ 30 s、GLAZE > 0 的格子会判失败；`listen_start_check.py` 遇到空 `confidence` 只比方案增益、注明没核对 c。

### 模拟宿主走带（`--bpm` / `--meter`）

离线导出从工程开头起播的口径：恒定速度，样本 0 = PPQ 0。每块按块起点给（同 `PluginProcessor::readHostInfo` 的各项）：
`hasTempo` / `bpm`；`hasPpq`，`ppqPosition = 位置样本 × BPM ÷ (60 × 采样率)`（四分音符计）；`hasBarStart`，
`ppqLastBarStart = floor(PPQ ÷ 每小节拍数) × 每小节拍数`（每小节拍数 = 分子 × 4 ÷ 分母，6/8 = 3）；`hasTimeSignature` 与分子 / 分母；
在播（`isPlaying`）、离线、位置照旧。PPQ 只由样本位置决定，任何块长下同一位置逐位相同。三遍（预热、成品、重放）用同一条时间线。
`--listen-from-bar N`：第一遍从第 N 小节的起点样本 `round((N − 1) × 每小节样本数)` 起播到歌尾，宿主位置照实际样本给（「从 drop 播」，见下「预热起点检查」）。

### 逐段统计（`--segments-csv`）

逐小节（或每 2 秒）导出段响度、低频占比与限幅 GR，看前奏 / breakdown / drop 各段的读数（T-038 时给 House 门控标定与 House 门槛用，
T-108 删了那两样，选项作通用诊断留下）。只读音频，不依赖编排器的 SegmentLedger，口径在工具里独立算：

- **分段**：有 `--bpm` 时一段 = 一小节，第 k 段起点 = `round(k × 每小节样本数)`（锚点 0、恒定速度，同规格 2.1 节）；没给时每
  `SegmentConstants::fallbackSegmentSeconds`（出厂 2 s；T-108 前叫 `HouseConstants::fallbackSegmentSeconds`）一段，**按样本墙钟**分（插件分 2 秒段时按有效音频样本推进，这里不同）。
- **段响度 `lufs`**：段内过了「有效音频」门的 100 ms 子块的 K 加权能量平均，换算成 LUFS（规格 2.3 节；子块来自插件自己的
  `LoudnessMeter`，与 IN 表、分析器同一测点同一算法；子块按起点归段）。门 = 以本子块结尾的 400 ms 门块（前 3 个子块用已有的）≥ −70 LUFS，
  且不低于参考 I − 20 LU——判据同 `MusicAnalyzer` 的 `effective`，**参考 I 换成整段音频的 BS.1770 整体响度**（插件阶段 1 用学习区间的 I；
  T-048 阶段 2 起唯一的段落账是实时段落账，参考它自己按门块遗忘的「实时 I」，`MusicAnalyzer.cpp` 第「七」节）。
  紧接响段的静音段，前 3 个子块的门块还带着响段，按这个口径算有效，读数是 K 加权滤波器的余振（比前一段低 20 LU 以上）。
- **短期响度 `short_term_lufs`**（参考列）：EBU 短期响度 S（3 s 窗、不门限，同 S 表 / 分析器的 `shortTermLufs`）在本段最后一个子块结尾处的读数；
  124 BPM 一小节约 1.94 s，窗会带进上一小节的约 1 s。**按段比较用 `lufs`（段响度），不是它。**
- **低频占比 `low_share`**：与画像 `lowShare` 同一口径——一个独立的 `BalanceModule`（参数全 0，只用它的分析；处理的是拷贝）逐帧给 27 段
  1/3 倍频程功率（两声道功率平均，段边界 35.6 Hz～17.8 kHz 连续铺满、bin 按重叠宽度计权），低频 = **100 Hz 段及以下（段 0～4，上边界
  112 Hz）的功率 ÷ 全部 27 段的功率**，未加权线性功率，过帧门的帧上分别累加再相除；帧按分析窗起点（`endSample − 窗长`）归段，窗还没装满的帧不用。
  帧门 = 总功率 ≥ −70 dBFS（`frameAbsoluteGatePower`），且不低于全曲过绝对门各帧平均 20 dB（插件阶段 1 按学习区间的均值；T-048 阶段 2 起
  实时段落账按自己的「实时帧均值」，按进来的帧遗忘，同上）。
  三个门限常数照抄 `MusicAnalyzer.cpp`（那边在匿名命名空间里），改那边要同步改 `RenderMain.cpp` 的 `lowShareLastBand` 等。
- **限幅 GR**（只有成品）：第二遍（或重放）逐块读 `getAndResetLimitGainReductionDb()`（块内最大），块按成品时间轴的起点归段
  （开头延迟段归第 1 段，原曲之后的余振不计）。`gr_over_p95_cap_blocks` = 块 GR 高于这组风格预算 `limitGrP95CapDb` 的块数
  （成品末尾保护层生效的上限；重放行用同组「自动」的）。块内最大比保护层按 10 ms 时隙的均值偏严。
- 成品那份按对齐前、截到原曲长度量（与输入同一条时间轴、同一张段表）。

| 列 | 含义 |
|---|---|
| `segment` / `mode` | 段号（从 1 起；按小节分时 = 小节号）/ `bars` 或 `seconds` |
| `start_sample` / `end_sample` / `start_s` / `end_s` | 段的起止（`[start, end)`，最后一段截到音频末尾） |
| `lufs` / `valid_s` | 段响度（没有有效子块时空）/ 有效子块时长 |
| `short_term_lufs` | 段末的短期响度 S（参考；歌开头不满 3 s 时空） |
| `low_share` / `frames` | 低频占比（没有过门帧时空）/ 过门帧数 |
| `gr_mean_db` / `gr_max_db` / `gr_blocks` / `gr_over_p95_cap_blocks` | 只有成品：按块长加权的块 GR 均值 / 最大 / 块数 / 高于 P95 预算的块数 |

### T-038 追加列（metrics.csv 末尾）

方案、画像各列取**第一遍（预热）结束时编排器的状态**：阶段 1 是「已提交的版本」；T-048 阶段 2 起没有提交与锁定，取的是此刻的目标方案、
自动风格权重与链上的实时画像（校准两列恒为 0）。有效 Ceiling、风格预算取成品那一遍。T-108 删了 `genre`、`delivery`、`house_gate_valid`、
`house_segment_mode`、`house_heard_segments`、`house_full_segments`、`house_heard_s`、`house_full_s`、`house_reopens`、`house_lock_pending`、
`beat_release_cap_ms` 共 11 列（33 → 22 列）。
重放行（GLAZE 0 的链）不填方案各列；输入行只填 `bpm` / `meter` / `segments_file`。

| 列 | 含义 |
|---|---|
| `bpm` / `meter` / `listen_from_bar` / `listen_start_s` | 这一组的模拟走带、第一遍起播小节与秒数 |
| `listen_has_profile` / `plan_gain_db` / `plan_requested_gain_db` | 第一遍末有没有画像（阶段 1 是已提交画像；阶段 2 是编排器读过链上的实时画像就为 1，不代表已热起）；方案的约束后净提升（`effectiveGainDb`）/ 想要的净提升 |
| `plan_requested_target_lufs` / `plan_effective_target_lufs` | 方案想要 / 约束后的目标响度 |
| `listen_calibration_valid` / `calibration_bias_db` | 已提交的校准有效 / 校准偏差（阶段 1）；T-048 阶段 2 起没有校准，恒为 0 / 0.000 |
| `weight_warm` / `weight_punchy` / `weight_clean` | 方案的风格权重（阶段 1 是「自动」风格锁定的那份；阶段 2 不再锁定，是第一遍末那份方案的） |
| `profile_lufs` / `profile_lra_lu` / `profile_low_share` | 画像 I、LRA、低频占比 |
| `profile_slope_db_per_oct` | 画像谱斜率（dB / 倍频程，Balance 段内总功率口径，粉噪 = 0；T-048 阶段 2 起是实时画像的，按主段权重（含遗忘）累计） |
| `effective_ceiling_dbtp` | 成品最后一块交给限幅器的有效 Ceiling（编码余量已算进去） |
| `budget_limit_gr_mean_db` / `budget_limit_gr_p95_db` / `budget_limit_gr_peak_db` | 成品末尾保护层生效的约束上限 = 风格预算（GLAZE 0 等没设时空） |
| `segments_file` | 这一行的逐段统计（相对 `--out`） |

### 合成测试曲与预热起点检查

```bash
python3 tools/render/make_synthetic_house.py <目录>           # 要 numpy（109 上：~/.venvs/audio/bin/python）
celadon_render --in <目录> --out <输出> --glaze 100 --clay auto --bpm 124 --listen-from-bar 1,33 --segments-csv --metrics-only --no-replay
python3 tools/render/listen_start_check.py <输出>/metrics.csv   # 只用标准库；有失败返回 1
```

`make_synthetic_house.py` 出两首 124 BPM / 48 kHz / 24 bit 立体声：`synthetic-house-124`（32 小节纯鼓前奏 → 32 小节 drop（kick + hat + clap +
沙锤 + offbeat bass + pad）→ 16 小节 breakdown（只有 pad）→ 32 小节 drop）与 `synthetic-tech-124`（同样段落，前奏就有滚动 bass），各带段落标注
`<歌名>.json`（小节从 1 起、两端都含，`kind` ∈ intro / drop / breakdown）。T-038 时它们是 House 门槛的阳性对照；T-108（Max 2026-11-13 Grill Q4）
撤掉 House 门禁后作普通素材留下，「从第 1 小节播 vs 从第 33 小节（drop）播」改按通用档查。

`listen_start_check.py`（原 `house_gate.py` 的门槛 ①）：按（歌, GLAZE, 修坯, 风格）分组，`listen_from_bar = 1` 与每个 `> 1` 的行比第一遍末的
`plan_gain_db`，差 ≤ 0.5 dB；行里有 `confidence` 时两边都要 c ≥ 0.5，空着时只比增益、注明没核对 c。GLAZE 0 与重放行不判；缺对照的组只列说明。
`house_gate.py` 的另四条（drop 段 GR 预算、breakdown → drop 落差、交付档真峰值、流媒体 / 俱乐部目标响度）与 `calibrate_house.py` 随 T-108 删掉。

### 自检（`--selftest-transport`）

| 项 | 期望（依据） |
|---|---|
| 模拟走带 120 BPM 4/4、6/8 | 第 96000 样本 PPQ 4 / 小节起点 4，前一样本小节起点 0，第 300000 样本 12.5 / 12；6/8 每小节 3 拍（双精度精确值，按 == 比） |
| 不给 `--bpm` | 四类拍点字段都没有，其余同改动前 |
| 分段网格 | 124 BPM 下 600 个段起点各归本段、前一样本归上一段；不给 `--bpm` 时每段 2 s |
| 逐段响度 / 短期响度 / 低频占比 | 120 BPM 五小节（静音 / 1 kHz / 1 kHz / 60 Hz + 1 kHz / 静音，−20 dBFS）：响段 −20.0 ± 0.1 LUFS、各 20 个有效子块；段末 S：第 2 小节 −20 + 10·log10(2/3) = −21.76、第 3 小节 −20.0（± 0.1），第 1 小节没有；只有 1 kHz 的段 < 0.005，60 Hz + 1 kHz 等幅约 0.497 ± 0.02（Hann 窗下 60 Hz 约 1.3 % 漏到 35.2 Hz 以下） |
| 给拍点 | GLAZE 35 两遍流程：两次给拍点的成品逐位相同、有效 Ceiling 都是 −1 dBTP、真峰值不超 Ceiling（+0.01）；给不给 `--bpm` 的成品哈希只打印「信息」（T-048 阶段 2 起连续编排器也读 PPQ，不要求相同） |
| 热信号的真峰值 | 白噪声 0.9：有效 Ceiling −1，成品真峰值在 −1.5 ～ −0.99 dBTP（限幅器动作、贴着 Ceiling）。T-108 前这里另比流媒体档 −2 dBTP 与 House 档限幅跟拍 |
| 第一遍从第 N 小节起播 | 第 3 小节 = 第 192000 样本，逐块追踪第一行 4.0000 s |
| 文件名、追加列个数 | 带 `_from-b33` 后缀的文件名与标签；列名与值个数相同（22） |

## T-048：实时画像追踪与因果重放

规格：设计规格-实时GLAZE-主段与增益（第 1～3 节实时画像、第 7 节因果重放验收）；T-048 阶段 1（`docs/validation/2026-10-02-T048-phase1/`，
BRIEF 第 3 节、REPORT-A / B / C；阶段 1 修订「回看重判」见 REPORT-A2）。**不给 `--live-trace` 时一切输出与改动前逐字节相同**；给了会多写
`live/` 下的追踪、metrics.md 头部一行与每首歌的备注。阶段 1 时成品 wav、metrics.csv、`--trace` / `--segments-csv` 的文件都与不给时逐字节相同（追踪不改链的输出，
验证记录见 REPORT-C）；**T-048 阶段 2 起不再如此**：链自己持有一份实时画像、GLAZE 只读它，而追踪用的 `CeladonChain::setLiveSegmentSink` 是外部探针，
接上之后段改交给追踪器、不再交给链的那一份，所以存档会变、GLAZE > 0 时成品也会变（`--selftest-live` 改成核对这一契约，见下「自检」；阶段 2 集成说明
INTEGRATION-NOTES.md，在 docs/validation/2026-10-05-T048-phase2/）。带 `--live-trace` 渲染的成品与指标不等于不带时的；metrics.md 头部那一行
仍写着「追踪不改链的输出」，是工具里还没改的旧文字。

实时画像现在是**回看重判**（`Source/dsp/LiveProfile.h` 文件头）：最近 512 段放在窗口里，每来一段按此刻的参考（S_ref、lowRef）把窗口里每一段的
主段度重算一遍、重新合并出画像（= 带遗忘的离线门控）；每一段的画像在「段尾 + 发布延迟（`publishDelaySeconds`，出厂 0.5 s）」这一刻**发布**，
重算按绝对样本网格分片做完。所以追踪是「每发布一份画像一行」，不再是「每并入一段一行」——行数一样（一段发布一份），内容是那一份画像。

### 做法

- 每一遍新建的链（第一遍预热、第二遍成品、两遍重放）`prepare` 之后调 `CeladonChain::setLiveSegmentSink`，接上工具自己的接收者：里面一个
  `LiveProfile`（`prepare (采样率)`、`setConstants (常数)`；T-108 起只有响度型一个主段模型，`--live-model` 删了）。分析器随之打开实时
  模式，实时段落账每定稿一段（有拍点一小节、否则 2 s）就交给接收者，接收者入队交给 `LiveProfile`。T-048 阶段 2 起这一接，段就不再交给链自己的
  那份实时画像（见本节开头）；改成直接读链内那一份要另改工具。
- 分析器在每块 `processInput` / `processSpectrum` 末尾把实时段落账**交齐的位置**报给接收者（`advanceTo`：end 不晚于它的段都已交出），接收者转给
  `LiveProfile`：在到期的网格点上做重算的分片、到了发布时刻就发布那一段的画像。追踪器自己不报时间——报「送进链的样本数」会走在交齐位置前面，
  段就可能晚于自己的发布时刻才到（`MusicAnalyzer.h`「时间轴与 advanceTo」）。
- 每发布一份，`LiveProfile` 回调接收者（`PublicationListener`）：接收者把这一份的画像（`MusicProfile` 整份）与诊断拷进 64 格的定长环形缓冲——只有
  定长拷贝，不分配、不加锁。主线程每处理完一块用 `readCommits` 读新提交、按序号配上快照，写成一行。一次 `advanceTo` 可能发布好几份（大块、追赶），
  每一份都有自己的快照，所以追踪与宿主块长无关。段里有、提交记录里没有的三项（epoch、已听、实时试算块数）接收者按段的到达序号另记（新实例里提交
  记录的序号就是到达序号）。
- 一遍结束（这一遍要读的状态、成品、指标都读完之后）：`setLiveSegmentSink (nullptr)`——链先把最后一块还在 Balance 里的帧**只交给实时段落账**
  （链在「下一块」开头才交上一块的帧，离线流没有下一块；不交的话歌尾一两段少了最后一块的帧、随块长变），分析器再把开着的段（歌尾不满一小节的
  那一截）就地定稿交出来——最后 `flush`：还没到发布时刻的段按序发布完（各行仍记各自的发布时刻），并读出窗口里每一段的**最终判断**
  （`readJudgements`，写 `__judgements.csv`）。T-048 阶段 1 修订 B2 之前这里要先补一个 0 样本的块，现在不用（`MusicAnalyzerLive` 用例 5d：两种做法交来的段逐字段相同）。
- **只写一份**：实时画像只看输入（与 GLAZE、风格、修坯无关），同一首歌 × 预热起点在一次运行里（一次运行只有一组常数，用标签分开）只写第一次跑到的那一遍**第一遍**；其余各遍照样追踪，只与写下的那份逐行比较（`__passes.csv`）。第二遍与重放从不当写下的
  那份：输入尾部多了延迟与余振的补零，歌尾一两段不同。REPORT-C 2.5 节「读档之后分析器不学习，第二遍从第一段起就不同」在 B2（实时段落账与学习
  脱钩）之后不再出现：`9e8575a` 上合成 House（当时的两个曲风；网格里起播 1 / 33 的各组）的第二遍与重放（都从第 1 小节起）与从第 1 小节起的第一遍前
  112 / 113 段逐行相同，melodic 1062232 前 118 / 120 段逐行相同，差的只是歌尾（`REPORT-P1-SPARK-REPLAY.md` 第 2 节）。

### 最快的跑法（标定、扫参用）

```bash
celadon_render --in <歌曲目录> --out <输出> --glaze 0 --bpm 124 --live-trace --live-only [--segments-csv] \
               [--live-constants loudness.gateWidthLu=5,newProgramLu=8 --live-tag dg5-np8]
```

- `--live-only`：每首歌 × 每个预热起点只跑一遍第一遍（取网格里第一个这样的组合的参数），不出成品、不做重放，指标表只有原始输入行；
  GLAZE 0 最快。一首 3～4 分钟的歌约 5 s（回看重判每段约 70 µs 的重算，一首几十毫秒，可以不计）。
- 与完整跑法（`--glaze 70,100 --style auto,warm --clay auto,off …`、带重放）写出的追踪与 `__final.csv` **逐字节相同**（同一块长；REPORT-C 2.4 节，
  `--selftest-live`「与 GLAZE、风格无关」）。`9e8575a` 上复核（DGX Spark 本机，`REPORT-P1-SPARK-REPLAY.md` 第 2 节）：合成 House（deep / 通用 ×
  起播 1 / 33）与 melodic 1062232（melodic / 通用）的追踪、`__final.csv`、`__judgements.csv` 逐字节相同；带不带 `--live-trace`，成品 wav、metrics.csv、
  `trace/`、`segments/` 逐字节相同（162 / 162、82 / 82 个文件）。这是阶段 1 的复核：T-048 阶段 2 起接追踪会改链的输出（见本节开头），
  这后半句不再成立；追踪本身仍与 GLAZE、风格无关（`--selftest-live`「与 GLAZE、风格无关」照旧要求）。
- 块长：自检合成曲在 512 / 4096 / 480 下逐字节相同（`--selftest-live`）。REPORT-C 2.4 节的真歌块长差别（帧门等三道相对门参考学习区间、学习在宿主
  块边界上停）B2 已去掉：实时段落账的三道门改参考它自己的长期参考（`MusicAnalyzer.cpp` 第「七」节）。复跑（`9e8575a`，DGX Spark 本机，
  `REPORT-P1-SPARK-REPLAY.md` 第 4 节）：Jamendo 45 首 × 4/4 型 / 响度型、合成两首 × 两型，块长 512 与 4096 的追踪、`__final.csv`、`__judgements.csv`、
  输入端逐段统计全部逐字节相同。比较不同块长时，`__final.csv` 的 `tag` 一行要去掉（标签不同）。
- 批量（Jamendo 45 首、合成曲）当年用 `calibrate_house.py render-live`；那个脚本随 T-108 删了（它按曲风跑 4/4 型 / 响度型两组），
  要批跑时逐首调上面的命令即可。

### 输出（`--out` 之下）

```text
live/<标签>/<歌名>[__from-b<N>].csv              追踪：每发布一份画像一行（列见下）
live/<标签>/<歌名>[__from-b<N>]__final.csv       汇总：这一遍的设定、常数全部 19 项的实际值、最终诊断与画像（键,值）
live/<标签>/<歌名>[__from-b<N>]__judgements.csv  歌尾窗口里每一段的最终判断（回看重判到歌尾的 m′ 与权重）
live/<标签>/<歌名>__passes.csv                          逐遍比较（有别的遍可比时才写；--live-only 不写）
```

追踪的列（空格 = 不知道，即 `valid = false`，不用 0 冒充；LUFS / dB / 秒 4 位小数，比例与因子 5 位）。「这一份画像」= 这一段发布的那一份：
窗口里全部段都按这一份的参考判过（之后每一份画像都会回看重判这一段，最终判断见 `__judgements.csv`）。

| 列 | 含义 |
|---|---|
| `sequence` | 第几份发布（从 0 起，= `LiveCommitRecord::sequence` = 段的到达序号） |
| `bar` / `bar_offset` | 有 `--bpm` 时：段的中点落在第几小节（歌曲的小节网格，同 `--segments-csv`；从第 N 小节起播时也按歌曲数）/ 段起点 − 该小节起点（样本，对齐时 0） |
| `start_sample` / `end_sample` / `start_s` / `end_s` | 段在分析器时间轴上的 `[start, end)`（这一遍送进链的第一个样本 = 0；从第 N 小节起播时加 `__final.csv` 的 `pass_start_sample` 才是歌曲里的位置） |
| `nominal_s` / `mode` | 段的标称长度 / `bars`、`seconds`、`none` |
| `epoch` / `heard` / `live_trial_blocks` | 段所属的连续时间段（不连续点 + 1）/ 已听（有效音频 ≥ 半段）/ 段里实时试算的有效块数（段里有、提交记录里没有的三项） |
| `effective_s` / `loudness_lufs` / `low_share` | 段的有效时长 / 段响度 / 低频占比 |
| `loudness_factor` / `low_factor` / `mainness` / `weight_s` | 这一份画像对这一段的判断：m′ 的两个因子 / 主段度 m′（段落占比的「首判」）/ 权重 w′ = m′ × 有效时长 |
| `reference_lufs` / `low_reference` | 这一份画像的 S_ref / lowRef（判窗口里全部段用的；改前是「并入之前」的） |
| `main_level_lufs` / `main_mass_s` / `confidence` | 这一份画像的 I（4/4 型 = 主段电平）/ 主段质量 M / 置信度 c |
| `time_decay` / `new_program` | 这一段进账时「长时间无主段」的时间衰减在生效 / 这一段进账时触发了换歌软遗忘 |
| `reference_after_lufs` / `low_reference_after` / `active_s` / `seconds_since_main` / `new_program_count` | 这一份画像的诊断（`getDiagnostics`）：S_ref、lowRef（与上面两列相同，留着兼容）、进账过的有效音频总时长（不遗忘）、最近一次进账 m ≥ mainThreshold 之后的有效音频、换歌累计次数 |
| `lra_lu` / `plr_db` / `true_peak_dbtp` / `slope_db_per_oct` / `profile_low_share` / `profile_high_share` / `representativeness` | 这一份画像的这几项 |
| `cost_lowcut_db` / `cost_mono60_db` … `cost_mono160_db` / `cost_width_db` / `blend10_db` / `blend20_db` | 这一份画像的实时试算代价与混合损失 |
| `publish_s` | 这一份画像的发布时刻（分析器时间轴，秒）= max(段尾 + 发布延迟, 上一份的发布时刻) |
| `admission_loudness_factor` / `admission_low_factor` / `admission_mainness` | 进账判断：按**上一份**画像的参考算的两个因子与 m（冷启动以这一段自己为参考）。长时间无主段只看 `admission_mainness`，换歌「轻」的那一支看 `admission_low_factor` |
| `np_vote` / `np_range_lu` | 换歌的票：0 不像（清零重计）、1 比 I 响 `newProgramLu` 以上、2 轻 `newProgramLu` 以上且低音满（只有 4/4 型投这一票，T-108 起不再出现；T-108 之前的追踪里有）、3 读不出（不计也不清零）/ 这一串已累计满 `newProgramSeconds` 时最近那一截的段响度起伏（「轻」的那一支要 ≤ `newProgramRangeLu` 才触发） |
| `window_segments` / `oldest_main_share` | 回看窗口里的段数 / 窗口最老一段在 M 里的份额（窗口满了以后下一段就丢掉它） |

`__final.csv`（`format` = 3；T-108 删了 2 里的 `genre`、`model`、`model_source`）的键：`song`、`tag`、`constants_override`（`--live-constants` 原文）、
`bpm`、`meter`、`listen_from_bar`、`pass_start_sample`、`commits`（行数）、`trace_digest`（全部行的位级摘要：写出的每个量的二进制位按 FNV-1a 累加，
固定小数位看不出的末位差它看得出）、`anomaly_overrun` / `anomaly_mismatch`（读提交落后环形容量 / 记录与快照配不上：工具自己的错，不该是 1）；
`diag.*`（finish 之后的 `getDiagnostics`，含 `window_segments`、`dropped_segments`、`oldest_main_share`、`oldest_reference_share`、`publish_s`；
工作量统计 `pendingSegments` / `lastPassSlices` / `catchUps` 随块长变，不写）；`profile.<特征>` 与 `profile.<特征>.confidence`（finish 之后的画像，I、LRA、PLR、真峰值、
斜率、低 / 高频占比、低频起伏、起音两项、次声、低 / 高频侧中比与相关度、6 项试算代价、2 项混合损失）与 `profile.effective_s` / `representativeness` /
`format_version`；`const.<键>`（常数全部 19 项的实际值，float 9 位、double 17 位有效数字）。取自网格里哪一组、宿主块长都**不写**（追踪与它们无关，
这样最快跑法与完整跑法的两份能逐字节比；取自哪一组写在 metrics.md 的备注与 `__passes.csv` 里）。

`__judgements.csv` 每段一行（窗口里从旧到新，最多 512 段：124 BPM 约 16.5 分钟，一首歌都在里面）：`sequence`、`bar`（同追踪）、`start_sample` / `end_sample` /
`start_s` / `end_s`、`mainness_final`（歌尾那一份画像的参考下这一段的 m′——段落占比的「终判」，对照 T-038 离线门控）、`weight_final_s`（这一段在歌尾
那一份画像里的权重：m′ × 有效时长 × 主段遗忘 × 全局因子）。

`__passes.csv` 每遍一行：`pass`（`listen` / `render` / `replay-copy` / `replay-off`）、`combo`、`start_bar`、`commits`、`digest`、`written`（就是写下的那份）、
`compared`、`matching_rows` / `reference_rows`（与写下的那份开头逐字相同的行数 / 写下的那份的行数）、`first_difference_sample`（第一处不同的段起点）、
`first_difference_columns`（那一行里不同的列，`;` 连起来；段数不同而前缀全同时写 `rows`）、`digest_differs`（各行逐字相同、摘要却不同）、`overrun`、`mismatch`。
其余第一遍（只差 GLAZE / 风格 / 修坯）应全部逐行相同——不同就是「实时画像只看输入」不成立，备注与 stderr 单独记一条「注意」。

### `--live-constants` 的键

键名 = `GlazeConstants.h` 里 `LiveConstants` 的字段名，主段模型的 9 个字段带 `loudness.` 前缀，共 9 + 10 = 19 个（T-108 前另有 `fourOnFloor.` / `passive.` 两个模型，共 37 个）。
键与取值范围用 `LiveProfile.h` 的公开表（`visitLiveConstants` / `isInLiveConstantDomain`）——`LiveProfile` 的清洗（`sanitiseLiveConstants`）用的是同一张表：
那边把坏值悄悄换成出厂值，这里直接报错（扫参时给错的值不能变成「其实跑的是出厂值」）。`inf` / `-inf` 只在表里允许的键上合法；同一个键给两次、
少了 `=`、不是数（含 NaN、溢出）都报错。

| 键 | 取值 | 出厂 |
|---|---|---|
| `loudness.gateWidthLu`（ΔG） | 有限的数 | 6 |
| `loudness.loudnessRampLu` | > 0 的有限数 | 3 |
| `loudness.lowGateFactor`（≤ 0 = 不用低频门） | 有限的数 | 0 |
| `loudness.lowRampWidth` | > 0 的有限数 | 0.15 |
| `loudness.referencePercentile` | (0, 1] | 0.90 |
| `loudness.memoryBars`（≤ 0 = 不按小节） | 数，可 ±inf | 0 |
| `loudness.memorySeconds` | ≥ 1 ms，可 inf | 90 |
| `loudness.confidenceBars`（≤ 0 = 用秒） | 数，可 ±inf | 0 |
| `loudness.confidenceSeconds` | ≥ 1 ms，可 inf | 20 |
| `referenceMemorySeconds` | ≥ 1 ms，可 inf | 180 |
| `mainThreshold` | [0, 1] | 0.5 |
| `noMainSeconds` / `noMainDecaySeconds` | ≥ 0 可 inf / ≥ 1 ms 可 inf | 60 / 120 |
| `newProgramLu` / `newProgramSeconds` / `newProgramKeep` | ≥ 0 可 inf / ≥ 1 ms 可 inf / [0, 1] | 6 / 20 / 0.2 |
| `newProgramRangeLu`（「轻」的那一支要电平稳；inf = 不要求） | ≥ 0，可 inf | 2 |
| `peakTailSeconds`（≤ 0 = 只取最大值） | 数，可 ±inf | 0.5 |
| `publishDelaySeconds`（发布延迟） | ≥ 0 的有限数 | 0.5 |

改了常数（`--live-constants`）必须给 `--live-tag`，免得与出厂常数的追踪混在 `live/default/` 里；这几个选项离开 `--live-trace` 都报错。

### 因果重放的批跑与分析（`calibrate_house.py`，已删）

T-048 阶段 1 的批跑与指标脚本 `calibrate_house.py`（`render-live` / `analyze-live` / `prep-synthetic`，M1 ～ M4 指标）随 T-108 删掉：它按曲风分
4/4 型与响度型两组跑，曲风删了之后没有这层分组。指标定义与当年的结果仍在 `docs/validation/2026-10-02-T048-phase1/`（REPORT-C 第 3 节，
`P1R/scripts/`）；脚本本身在 git 历史里（T-108 之前的提交）。

### 自检（`--selftest-live`）

合成 4/4 曲（House 风格素材，124 BPM、48 kHz：8 小节鼓前奏 → 12 小节 drop → 8 小节 breakdown → 8 小节 drop，最后一段 drop 再多放 0.4 小节就截断——歌尾不留静音），
GLAZE 0、块长 512、出厂常数，走真歌同一套第一遍（T-108 前这里按 Deep House 跑 4/4 型）：

| 项 | 期望 |
|---|---|
| `--live-constants` 解析 / 拒收坏值 | 改到 7 个字段（含 inf / -inf）、其余保持出厂值、键表 19 个键；13 种坏写法（含删掉的 `fourOnFloor.` 键）都报错 |
| 追踪选项的命令行约定 | 单独 `--live-only`、改常数不给标签、标签 `../x`、删掉的 `--live-model` / `--genre` 都报错；完整的一组解析对 |
| 追踪选项的命令行约定 | 单独 `--live-only`、改常数不给标签、标签 `../x` 都报错；完整的一组解析对 |
| 段首尾相接 | 第一段从 0 起、相邻两段首尾相接、最后一段止于音频末尾（finish 交出了歌尾、flush 发布完）；没有读提交落后、没有配不上 |
| 按小节对齐 | 36 小节 + 1 段尾巴，每段起止与工具的小节网格逐样本相同 |
| 从第 N 小节起播的小节号 | 从第 9 小节起播：小节号按歌曲数（第一段整段在第 9 小节、最后一段在第 37 小节），段起点离网格 ≤ 1 样本 |
| I 有效、段落门 | 每份画像 I 都有效；breakdown 里首判 m ≥ 0.5 的 0 小节、第二段 drop 里首判 m < 0.5 的 0 小节 |
| 块长 512 / 4096 / 480 | 追踪逐字节相同、位级摘要相同 |
| 与 GLAZE、风格无关 | GLAZE 70 温润与 GLAZE 0 的追踪逐字节相同 |
| 外接追踪改写 LIVE 存档 | 接了追踪，第一遍存下的 GLAZE 状态与不追踪不同（T-048 阶段 2 起探针把段从链的实时画像引走；阶段 1 要求两者逐字相同） |
| 追踪探针对输出的契约 | GLAZE 70 第二遍：两次不追踪逐位相同、两次追踪逐位相同、接上追踪后成品与不追踪不同（段被引走）、真峰值不超 Ceiling（+0.01）。阶段 1 这一项是「追踪不改链的输出」（接不接追踪成品哈希相同）。另打印「信息」：第二遍追踪与第一遍有多少段相同、第一处差在哪些列 |
| 常数穿到画像 | `loudness.lowGateFactor=0.6`（打开低频门）段数相同、摘要不同；`loudness.confidenceSeconds=300` 时最终 c 比出厂更低 |

## T-096：量尺（质量指标、盲测、母带检查单、Heat 底量 A/B、CPU 基线）

ADR-034 的验收门要的工具。ADR-039 删掉曲风与交付之后裁过一遍：流媒体 / 俱乐部的目标与上限、编码再解码的真峰值、R6-02 重渲都删了。
Python 依赖钉在 `tools/render/requirements.txt`（numpy / scipy / soundfile / pyloudnorm）；`quality_gate.py`、`heat_ab_report.py` 只用标准库。
按现行纪律都在 108（testbox）或 DGX 上跑，Mac 上不跑（见 `docs/BUILDING.md`）。

| 文件 | 做什么 | 口径与用法 |
|---|---|---|
| `measure_lib.py` | 全仓库唯一的真峰值 / 响度 / 短期响度 S / PSR / LRA / 低频相关度 / S/M / DC / 静音 / 谱斜率 / 主段挑选定义，别处只许 import（ADR-034 决定 6） | `docs/validation/2026-10-11-T096c-measure-lib/`；自检 `measure_selfcheck.py` |
| `section_lib.py` | 段落标注 JSON、Jamendo 逐小节标签（叠人工改标）、`--segments-csv` 逐段统计的读法与段落响度 | 原在 T-038 的 `house_gate.py` / `calibrate_house.py` 里，T-108 删脚本时搬到这里 |
| `quality_metrics.py` | 渲染输出（要带 `--trace`）+ 源文件 → 每首 × 每组设置一行 `quality.csv`（B 组最小集） | `docs/validation/2026-10-11-T096a-metrics/`（GLOSSARY 是每个字段的唯一定义）；`--selftest` |
| `quality_gate.py` | 新 `quality.csv` 对基线逐行判：A 组绝对、B 组不退步 | 同上；`--selftest`；退出码 0 / 1 / 2 |
| `run_quality_baseline.sh` | 22 首门槛曲 + 45 首 Jamendo 各渲一遍再出 `quality-22.csv` / `quality-45.csv` | 文件头；`--dry-run` 只打印命令 |
| `blind_test.py` | 盲测包（2AFC / ABX）：出包、登记、算答卷、自检 | `docs/validation/2026-10-11-T096b-blind-test/`；`selfcheck` 子命令 |
| `master_check.py` | 母带检查单：直接量成品 wav，FAIL 只有「真峰值超 `--ceiling`」「原采样点削波」「读不了 / 非有限样本」三项，其余只报；多首时出 EP 汇总行 | `docs/validation/2026-10-11-T096c-master-check/`；自检 `master_check_selfcheck.py` |
| `heat_ab_report.py` | 汇总 `--replay-override color.heat=0` 的 `heat_ab.csv`，每格给 2a / ABX 结论 | `docs/validation/2026-10-11-T096d-heat-ab/`（批跑 `run_heat_ab.sh`）；`--selftest` |
| `tools/perf/bench_portable.cpp`、`scripts/perf-dgx.sh`、`tools/perf/compare_perf.py` | 可移植 CPU 基准（处理器层 + 直接调 CeladonChain 的链层）与 DGX 上的同机不退步对比 | `docs/validation/2026-10-11-T096f-cpu-baseline/` |

**`--trace` 追加的六列**（只在第二遍那份，接在校准列之后）：`current_mainness`（本块主段度 m）、`protection_db`（状态行那扇守门的退让）、
`guard_cause`（`none` / `budget` / `limiterPeak`）、`hard_clip_samples` / `limit_diff_energy` / `limit_in_energy`（限幅器渲染计数：本块硬钳位样本数、
限幅前后差信号能量、限幅器入口能量）。限幅器计数只在渲染工具打开时才数（`LimiterModule::setRenderCountersEnabledForTesting`，插件从不调用；
关着时零开销，开着不分配内存、输出逐位不变），要链提供 `getLimiterForTesting()`，没有时三列留空、`quality_metrics.py` 在 `na_reason` 里写明。

**重放覆盖的自检怎么判有没有牙（BUG-012）**：Color 的参数平滑只有 20 ms，而 Clay 的 D + G 延迟（48 kHz 下 1102 样本，约 23 ms）里 Color 的输入全是零，
只覆盖 prepare 前那份参数在成品里听不出来。所以自检在参数层判：记录链实际收到的 prepare 前那份与每一块参数，覆盖的那一遍必须全是覆盖值，
只覆盖 prepare 前那份的对照必须在块里漏出记录值；成品差信号只作说明，不判定。

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
   低音很宽 / 有反相内容的那首：ADR-015 起「自动」不收单，反相段只亮收单灯（琥珀）、不改声音，与「重放全关」的差别和其余段落一样
   只来自低切与加宽，所以不再专门比低频是否更紧、单声道回放（DAW 里把总线切成单声道）是否更稳。

## 注意

- **默认不在本机跑**：本机 Mac 不编译、不渲染（ADR-002 修订 4；服务器故障时以 Max 当场指令为准）。测试服务器宕机时脚本连不上会直接报错退出。
- **结果体积**：每个成品是 32-bit float（48 kHz 立体声约 23 MB / 分钟），5 首 × 8 组约 4 GB。结果默认放在 `artifacts/` 下；
  `scripts/test-linux.sh` 与本脚本同步源码时都已排除 `artifacts/` 与 `docs/`，渲染结果不会过网。
- **单线程**：一次只跑一条链；DSP 代码没有按多条链并发验证过，服务器也是共用的。5 首 × 8 组在服务器上约十几分钟（每组两遍；重放组两遍单程：阳性对照 + 重放全关）。
- 工具只用 Source/dsp + juce_audio_formats，不依赖界面模块与 juce_audio_processors；参数不经过 APVTS，出厂默认值取
  `Source/ParameterDefaults.h`（插件参数表同一处真源，改默认值只改那里）。
- **数值环境**：整个进程在 FTZ / DAZ 下跑（`main` 开头一次 `juce::ScopedNoDenormals`，T-032 R10-06），与插件的音频线程一致；
  只影响 < 1.2e-38（约 −758 dBFS）的非规格化数。

## T-085：编码后真峰值与流媒体目标对比（已删）

T-085 的 `codec_tp_check.py`（按流媒体交付档、`--streaming-target-lufs` 改基准目标后做 AAC / Opus / MP3 编解码并测真峰值）随 T-108 删掉交付一起删了；
`--streaming-target-lufs` 也删了。`--no-align`（写未经整体等响的成品，默认仍按 `--match-lufs` 等响）作通用测量选项留下。
计量定义与当年的结果见 [T-085 报告](../../docs/validation/2026-10-08-T085-codec-tp/REPORT.md)；脚本在 git 历史里（T-108 之前的提交）。
它里面与编解码无关的量具（`measure()`、真峰值扫描、PSR、LRA 等）T-096 时已整理成库，现在在 `measure_lib.py`（见上「T-096」），算术没动。
