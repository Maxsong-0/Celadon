# T-096c2 母带检查单（`tools/render/master_check.py`）

状态：2026-10-11 写成（T-096 第 1 步 c2，当时叫「交付检查单」，是 `codec_tp_check.py master-check` 子命令）；
2026-11-13 由 T-114 移植进 0.1.8.11，按 ADR-039（删掉曲风与交付）只留通用部分、改成独立脚本 `master_check.py`。
**代码改完·未验证**（验收命令见文末，由主会话跑：先 108 testbox，其次 DGX）。

## 这是什么

Max 从 DAW 导出（或 `celadon_render` 离线渲出）成品之后，一条命令逐首出一张检查单：该判失败的判失败，其余只报。
它**直接量成品 wav，不渲染**，量具全部来自 `tools/render/measure_lib.py`（见 `docs/validation/2026-10-11-T096c-measure-lib/README.md`），
段落标注的读法来自 `tools/render/section_lib.py`，这里没有重写任何真峰值 / 响度 / PSR / LRA 的算法。
工具只报告，不改任何输入文件，不自动回退增益；失败以后重导出、降 Ceiling 还是回混音，由 Max 决定。

ADR-039 裁掉的部分（不再有）：`--delivery streaming / club` 两档与各自的上限 / 目标、四个 ffmpeg 编码预设的编码再解码真峰值、
Apple AAC 参考列（连 Mac 上的 `afconvert` 脚本）、按交付档取的 PSR 下限。要出流媒体版，把 `--ceiling` 设成 -2 即可。

## 用法

在仓库根目录跑（依赖见 `tools/render/requirements.txt`：numpy / scipy / soundfile / pyloudnorm；不需要 ffmpeg）：

```bash
# 默认：真峰值上限 = 插件出厂 Ceiling -1.0 dBTP，不做目标核对；可以给多个目录或文件
python tools/render/master_check.py --in /path/to/masters --out docs/validation/<日期>-T0xx-master-check

# 流媒体版：上限 -2.0、核对目标 -10 LUFS；带段落标注（主段改取标注里的 drop 段）
python tools/render/master_check.py --in masters/ --ceiling -2 --target-lufs -10 --annotations annotations/ --out out/

# 先预演：只打印输入、标注匹配和将写的文件，不量、不写
python tools/render/master_check.py --in masters/ --out out/ --dry-run
```

| 参数 | 说明 |
|---|---|
| `--in` | 成品 wav 文件或目录，可给多个；目录只取第一层的 `*.wav`，跳过 `.` 开头的（macOS 的 `._` 文件）。文件名不许重复（CSV 按文件名区分各行） |
| `--ceiling` | PCM 真峰值上限（dBTP），默认 -1.0 |
| `--target-lufs` | 目标响度；给了才做目标核对与「监听衰减」那条嫌疑，不给就只报数 |
| `--annotations` | 段落标注 JSON 文件或目录，格式与解析见 `section_lib.load_annotations`（按歌名 = 文件名去扩展名对上；也认 `celadon_render` 的 `<歌名>__g…` 命名） |
| `--out` | 输出目录，必填，不存在会建 |
| `--dry-run` | 预演，不写任何文件 |

### 退出码

| 码 | 含义 |
|---|---|
| 0 | 没有 FAIL（可以有 INFO） |
| 1 | 至少一首 FAIL |
| 2 | 基础设施错：参数错、输入 / 标注不存在或读不了、没有 wav、输入重名、输出写不了、意料之外的异常 |

单个文件读不了 **不是** 基础设施错：那一首记 FAIL（"文件读不了"），其余文件照常检查。

### 输出（`--out` 目录）

- `master_check.csv`：**真源**，全部 47 列（下表），每首一行，多首时末尾再加一行 `EP 汇总`。数值算不出 / 没测一律留空，绝不填 0；`±inf` 写 `inf` / `-inf`。
- `master_check.md`：由 CSV 读回来生成，可以直接给厂牌：逐首核对表（含 EP 汇总行）、判定详情、文件与其他项、判定表、说明。
- `environment.json`：包版本、`master_check.py` 与 `measure_lib.py` 的 sha256、命令行、输入文件的大小与 sha256、阈值快照、判定计数。
  同一台机器、同一条命令、同样的输入再跑，三个文件字节相同（里面不记时间）。

## 判定

只有三档：PASS / INFO / FAIL。**判失败只有三项**（Max 2026-10-11「全按推荐」认的计划 §13 Q9 原有四项，第四项「必需编码器缺失」随编码回测删了），
其余一律只报（INFO，标黄）。判定项与阈值集中在 `master_check.py` 顶部那一张常量表，下表的数字取自它；写进 `master_check.md` 的判定表
由同一组常量生成，不会脱节。

| 级别 | 条件 | 常量 |
|---|---|---|
| FAIL | PCM 真峰值（8 倍过采样，`tp8_dbtp`）高于 `--ceiling`（默认 -1.0）+ 0.01 dB | `DEFAULT_CEILING_DBTP`、`TP_LIMIT_TOLERANCE_DB` |
| FAIL | 原采样点削波数 `clipped_sample_count` 大于 0（浮点文件：样本绝对值大于 1.0；整数 PCM：样本贴满刻度码值，单个就算） | `measure_lib.INTEGER_PCM_BITS` |
| FAIL | 文件读不了，或含非有限样本（NaN / Inf） | |
| INFO | 给了目标时：依据响度距目标超过 ±1 LU。有 drop 标注用 drop 段的 S，否则用整曲 I（`target_basis`） | `TARGET_WINDOW_LU` |
| INFO | 直流偏置高于 -60 dBFS | `DC_WARN_DBFS`（暂定） |
| INFO | 开头静音超过 1 s，或结尾静音超过 3 s（静音 = 样本都不超过 -60 dBFS） | `LEAD_SILENCE_WARN_S`、`TRAIL_SILENCE_WARN_S`（暂定） |
| INFO | 采样率不是 44.1 / 48 kHz，或位深不是 PCM_16 / PCM_24（`celadon_render` 的 32-bit float 成品会被标位深） | `EXPECTED_SAMPLE_RATES`、`EXPECTED_SUBTYPES`（暂定） |
| INFO | 单声道低频损失低于 -1 dB | `MONO_LF_LOSS_WARN_DB` |
| INFO | 疑似带着临时监听导出（整曲 S/M 低于 -60 dB；或给了目标时 I 比目标低 3 LU 以上且真峰值比上限低 3 dB 以上） | `measure_lib` 的 `SIDE_MID_SUSPECT_DB`、`SUSPECT_*` |
| INFO | 有项目因素材原因算不出（`na_reason` 非空）；PCM 真峰值没测出来 | |
| INFO | EP 汇总：各曲主段 S 极差超过 1 LU、谱斜率极差超过 0.3 dB/oct、主段 PSR 中位极差超过 1.5 dB（R1 G7） | `EP_*_MAX_*` |

LRA 与 PSR 只报数，没有阈值：LRA 的「出口 ≥ 入口 - 1」需要入口，成品直测没有入口；PSR 下限原来按交付档取，ADR-039 删了交付，
插件里也没有了 PSR 守门（要不要加一条通用的 PSR 只报线，列在待 Max 拍板里）。

### 主段、目标核对与谱斜率

- **主段**（`main_basis`）：有段落标注且有窗完整落在某个 drop 段里 -> `drop`；否则 -> `loudest`：短期响度 S 不低于 P95 - 3 LU 的 3 s 窗（R1-01 口径）。
  S 不高于 -70 LUFS 的窗永远不进主段。标注按「素材裁到小节线」约定：第 1 小节从 0 s 起，每小节秒数 = 每小节拍数 × 60 / bpm（拍数取 `meter` 的分子，缺省 4/4）。
- **主段 S**（`main_s_lufs`）：选中窗的短期响度做能量平均（同 `section_lib.section_loudness` 的「K 加权能量平均」）。
  **主段 PSR**（`main_psr_p10_db`、`main_psr_median_db`）：选中窗的 PSR_k = 窗内 8 倍真峰值 - S_k 的 P10 与中位数。
- **目标核对**：有 drop 标注时核对的是 drop 段 S（目标是 drop 响度，不是整曲积分；T-085 实测整曲 I 中位与目标相差 0.7 ～ 2 LU，按整曲判会误报）；
  没有标注只能退回整曲 I，此时偏离多半只是曲子本身起伏大。
- **谱斜率**（`spectral_slope_db_per_oct`）：主段音频（选中窗合并）的 Welch 长时平均功率谱（两声道功率平均），累加成 27 个 1/3 倍频程段
  （中心 1000·2^((b-14)/3) Hz，同 `BalanceModule`），段总功率（dB）对 log2 频率做最小二乘直线，粉红噪声 = 0（口径同 `LiveProfile` 的 `spectralSlopeDbPerOct`）。
  比最响的段低过 60 dB 的段不进回归，进回归的段少于 6 个读空并写 `na_reason`。受母带低通影响（MP3 来源的素材高频段会很低）。

## 列说明（`master_check.csv`）

真源是 `master_check.py` 的 `CHECK_FIELDS`（判定头 -> `measure_lib.MASTER_FIELDS` 去掉 `file` / `na_reason` -> 本工具新增 -> EP 极差 -> `na_reason` / `notes`），
下表由它抄出；`master_check_selfcheck.py` 会核对本 README 里每个列名都在。文字列没有就是空串；数值列没算出 / 没测就是空格子。

| 列 | 单位 | 含义 |
|---|---|---|
| `file` | text | 文件名（不含目录）；EP 汇总行写「EP 汇总」 |
| `verdict` | text | 判定：PASS / INFO / FAIL；有 FAIL 项即 FAIL，否则有只报项或算不出的项即 INFO，否则 PASS |
| `fail_reasons` | text | 判失败的原因，多条用 "; " 连接；空 = 没有失败项 |
| `info_flags` | text | 只报不判（标黄）的项，多条用 "; " 连接；空 = 没有 |
| `target_lufs` | LUFS | 目标响度（--target-lufs）；没给留空，目标核对与「监听衰减」那条嫌疑都不做 |
| `tp_limit_dbtp` | dBTP | PCM 真峰值上限 = --ceiling（默认 -1.0） |
| `subtype` | text | libsndfile 子类型（即位深）：PCM_16 / PCM_24 / FLOAT 等 |
| `sample_rate` | Hz | 采样率 |
| `channels` | count | 声道数；响度类量具只支持 2（立体声） |
| `duration_s` | s | 时长 |
| `nonfinite_sample_count` | count | 非有限样本（NaN / Inf）个数；大于 0 时下面的测量项全部留空 |
| `lufs_i` | LUFS | 整曲积分响度（BS.1770 K 加权，-70 LUFS 绝对门 + -10 LU 相对门） |
| `lra_lu` | LU | 响度范围（EBU Tech 3342：3 s 短期值，-70 绝对门 + -20 LU 相对门，P95 - P10） |
| `sample_peak_dbfs` | dBFS | 采样峰值（所有声道） |
| `tp4_dbtp` | dBTP | 4 倍过采样真峰值 |
| `tp8_dbtp` | dBTP | 8 倍过采样真峰值（主口径）（判失败用的主口径） |
| `sample_clip_count` | count | 样本绝对值大于 1.0 的个数（只对浮点文件有意义；整数 PCM 读出来恒为 0）（整数 PCM 恒为 0，判失败不用它，用 `clipped_sample_count`） |
| `intersample_clip4_count` | count | 4 倍过采样插值点绝对值大于 1.0 的个数 |
| `intersample_clip8_count` | count | 8 倍过采样插值点绝对值大于 1.0 的个数 |
| `crest_db` | dB | 峰值因数：采样峰值 - 全频 RMS |
| `plr_db` | dB | PLR = tp8_dbtp - lufs_i |
| `psr_p10_db` | dB | 3 s 尾随 PSR（8 倍真峰值 - 同窗短期响度 S）序列的 P10；只统计 S 高于 -70 LUFS 的窗 |
| `psr_median_db` | dB | 同上序列的中位数（T-085 沿用的口径） |
| `psr_p90_db` | dB | 同上序列的 P90 |
| `dc_offset_dbfs` | dBFS | 直流偏置：各声道均值绝对值的较大者；零偏置读 -300 |
| `lead_silence_s` | s | 开头静音时长（所有声道的样本都不超过 -60 dBFS） |
| `trail_silence_s` | s | 结尾静音时长（判定同上） |
| `low_corr_lt125hz` | ratio | 125 Hz 以下左右相关度 rho = 2·sum(LR) / (sum(L^2) + sum(R^2))，-1 ~ +1 |
| `mono_lf_loss_db` | dB | 单声道求和的低频功率损失 10·log10((1 + rho) / 2)，不大于 0；rho = -1 时为 -inf |
| `side_mid_db` | dB | 整曲侧中比 S/M = 10·log10(sum(S^2) / sum(M^2))，M = (L+R)/2，S = (L-R)/2；纯中间信号为 -inf |
| `monitor_suspicion` | text | 「疑似带着临时监听导出」的原因；空 = 无嫌疑 |
| `clipped_sample_count` | count | 原采样点削波数（判失败用）：浮点文件 = 样本绝对值大于 1.0 的个数；整数 PCM = 样本贴满刻度码值的个数 |
| `tp_margin_db` | dB | 真峰值余量 = tp_limit_dbtp - tp8_dbtp（负 = 超限） |
| `target_basis` | text | 目标核对的依据：drop = 标注的 drop 段短期响度；whole = 整曲 I；空 = 没给目标 |
| `target_check_lufs` | LUFS | 目标核对用的响度值（依据见 target_basis） |
| `target_delta_lu` | LU | target_check_lufs - target_lufs |
| `main_basis` | text | 主段的定义：drop = 标注里的 drop 段；loudest = 短期响度不低于 P95 - 3 LU 的 3 s 窗；空 = 没有主段 |
| `main_seconds` | s | 主段窗合并后的总时长 |
| `main_s_lufs` | LUFS | 主段 S：主段内各 3 s 窗短期响度（能量平均） |
| `main_psr_p10_db` | dB | 主段内 PSR 序列的 P10（只报数） |
| `main_psr_median_db` | dB | 主段内 PSR 序列的中位数（只报数；EP 极差用） |
| `spectral_slope_db_per_oct` | dB/oct | 主段谱斜率：27 个 1/3 倍频程段电平对 log2 频率的最小二乘斜率，粉红噪声 = 0 |
| `ep_main_s_range_lu` | LU | 仅 EP 汇总行：各曲主段 S 的极差（最大 - 最小） |
| `ep_slope_range_db_per_oct` | dB/oct | 仅 EP 汇总行：各曲谱斜率的极差 |
| `ep_psr_median_range_db` | dB | 仅 EP 汇总行：各曲主段 PSR 中位的极差 |
| `na_reason` | text | 有字段因素材原因算不出时的原因；空 = 该算的都算出了（含量具库的原因 + 主段 / 谱斜率的原因） |
| `notes` | text | 不影响判定的备注（主段改用最响窗口的原因等） |

EP 汇总行只在一次给了 2 首及以上时出；单首没有极差，不出这一行。

## 已知限制与待 Max 认的数

- **整数 PCM 的削波口径**是 T-096 暂定的：样本贴满刻度码值（16 / 24 / 32 bit）即算一个削波点，单个就判失败。`sample_clip_count` 对整数文件恒为 0，
  所以另加了 `clipped_sample_count`。一个干净的限幅成品在 Ceiling 之下不会碰满刻度码值；想要「连续若干个样本贴满」之类的更严口径，改
  `measure_lib.clipped_sample_count()` 一处即可。
- **标「暂定」的阈值**（DC -60 dBFS、静音 1 s / 3 s、采样率与位深的期望值）是任务书没给、按调研报告（R1 G1 / G3 / G10 / G7）取的起点值，只影响 INFO，不影响退出码。
- 主段的 `loudest` 回退依据是 S 的 P95 - 3 LU，对「整首都在同一响度」的曲子等于整首；它不是 drop，别拿它和 `--target-lufs` 当成 drop 核对。
- 标注的第 1 小节假定从 wav 的 0 s 开始；DAW 导出带前导静音时标注要自己平移，或接受主段回退。
- 谱斜率的 Welch 段长 16384（48 kHz 下 2.9 Hz 一格），40 Hz 一段只有约 3 格，低段读数有 ±1 dB 量级的离散抖动，对 27 段回归的斜率影响约 0.03 dB/oct。
- 抖动提示：若交付采样率不同于工程采样率，抖动请放在宿主的最后一步。这句提示印在 `master_check.md` 的说明里。
- R2-02(a)（冲击 / 通透 / Clip 0 三组对比）不在本工具：它依赖 (d) 的参数覆盖开关和 (a) 的等响二分。

## 自检

`tools/render/master_check_selfcheck.py`（numpy 合成样例，不读外部文件）：

- 已知 TP 超限 -> FAIL：fs/4 正弦 45 度相位，真峰值 -1.0 dBTP 而采样峰值只有 -4.0 dBFS；`--ceiling` 决定上限（Ceiling -0.5 时 -1.0 通过）。
- 已知削波 -> FAIL：幅度 1.2 的 32-bit float 正弦；幅度 1.5 削到满刻度码值的 24 bit 文件；干净文件计数为 0。
- DC +0.01（-40 dBFS）、开头 2 s / 结尾 4 s 静音、整体降 10 dB（低 I + 监听嫌疑）、左右完全相同（纯 M，S/M = -inf）-> 只报（INFO）且值正确；不给目标就不做目标核对。
- 全部干净 -> PASS：脉冲 + 低频音 + 噪声的合成成品，I、S、PSR、S/M 都能手算（见 `clean_master` 的注释）。
- 判定边界（上限 + 0.01）、三个失败项、各类只报项逐类触发、PSR 与缺目标不触发、NaN 不触发；读不了 / 含 NaN 的文件判 FAIL 但不打断其余文件。
- 主段选择（标注 / 最响回退 / 静音窗不进）、标注小节换算与按名查找、区间合并、主段统计、削波口径、谱斜率（白噪声 +3、粉红 0、-3、单音 NaN）、EP 汇总只报极差。
- 命令行：退出码 0 / 1 / 2（各类基础设施错）、三个输出文件及列头、`._` 文件被跳过、`--dry-run` 不写文件、CSV 格子与 Markdown 渲染、README 列出每个字段。

退出码 0 = 全部 ok，1 = 有 FAIL。自检不过就不能说完成。

## 验收命令（由主会话统一执行：先 108 testbox，其次 DGX）

```bash
PY=~/.cache/celadon-spark/venv/bin/python     # DGX；108 的沙盒里先 pip install -r tools/render/requirements.txt

# 1. 合成自检：期望每行 ok，最后一行 "N/N ok"，退出码 0
$PY tools/render/master_check_selfcheck.py

# 2. 量具自检不受影响：期望全部 ok，退出码 0
$PY tools/render/measure_selfcheck.py

# 3. 对一首成品跑一遍：期望出三个文件，退出码 0 或 1（视这首的真峰值 / 削波而定），不是 2
$PY tools/render/master_check.py --in <一首成品.wav> --out /tmp/mc_out
ls /tmp/mc_out   # environment.json  master_check.csv  master_check.md
```
