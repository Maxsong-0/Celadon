# T-096c2 交付检查单（`codec_tp_check.py master-check`）

状态：代码改完，未验证（本机和 DGX 上都还没跑过；验收命令见文末，由主会话在 DGX 跑）。

## 这是什么

Max 从 DAW 导出（或 `celadon_render` 离线渲出）成品之后，一条命令逐首出一张检查单：该判失败的判失败，其余只报。
它**直接量成品 wav，不渲染**，量具全部是 c1（`tools/render/codec_tp_check.py` 里 `measure_master` 等，见
`docs/validation/2026-10-11-T096c-measure-lib/README.md`）的同一份实现，这里没有重写任何真峰值 / 响度 / PSR / LRA 的算法。
工具只报告，不改任何输入文件，不自动回退增益；失败以后重导出、降 Ceiling 还是回混音，由 Max 决定。

T-085 的默认命令（不带子命令的 `codec_tp_check.py --render ...`）一行没动，输出与改前逐字节相同（对账方法见文末）。

## 用法

在仓库根目录、DGX 的 venv 里跑（python 路径 `~/.cache/celadon-spark/venv/bin/python`，需要 numpy / scipy / soundfile / pyloudnorm 和 ffmpeg）：

```bash
# 流媒体版：PCM 真峰值上限固定 -2.0 dBTP，目标响度默认 -10 LUFS
python tools/render/codec_tp_check.py master-check --in /path/to/streaming_masters \
  --delivery streaming --out docs/validation/<日期>-T0xx-delivery-check/streaming

# 俱乐部版：上限 = --ceiling（默认 -1.0），目标默认 -7 LUFS；可以给多个目录或文件
python tools/render/codec_tp_check.py master-check --in /path/to/club_masters --delivery club --ceiling -1.0 \
  --out docs/validation/<日期>-T0xx-delivery-check/club

# 带段落标注（主段改取标注里的 drop 段）、带 Apple AAC 参考列、只测两个编码预设
python tools/render/codec_tp_check.py master-check --in masters/ --delivery streaming --target-lufs -10 \
  --annotations annotations/ --apple-aac-dir apple_aac/ --codecs aac-256-pns-off,opus-160 --out out/

# 先预演：只打印输入、标注匹配、预设可用性和将写的文件，不量、不编码、不写
python tools/render/codec_tp_check.py master-check --in masters/ --delivery streaming --out out/ --dry-run
```

| 参数 | 说明 |
|---|---|
| `--in` | 成品 wav 文件或目录，可给多个；目录只取第一层的 `*.wav`，跳过 `.` 开头的（macOS 的 `._` 文件）。文件名不许重复（Apple 参考按文件名匹配） |
| `--delivery` | `streaming` 或 `club`，必填 |
| `--target-lufs` | 目标响度；不给取档基准（流媒体 -10、俱乐部 -7）。风格偏移（暖 / 冲击 / 通透）要自己算好填进来 |
| `--ceiling` | 只用于俱乐部档，默认 -1.0；流媒体档给了会报错退出（上限固定 -2.0） |
| `--annotations` | 段落标注 JSON 文件或目录，格式与解析同 `house_gate.load_annotations`（按歌名 = 文件名去扩展名对上；也认 `celadon_render` 的 `<歌名>__g…` 命名） |
| `--apple-aac-dir` | `afconvert_roundtrip.sh` 解回的 wav 目录，按同名文件匹配；给了就多一列 Apple AAC 参考 |
| `--codecs` | 逗号分隔的预设名：`mp3-320`、`aac-256-pns-off`、`aac-256-default`、`opus-160`；`all`（默认）= 全部，`none` = 不做编码回测 |
| `--out` | 输出目录，必填，不存在会建 |
| `--ffmpeg` | 默认 `/usr/bin/ffmpeg` |
| `--work` | 编码回测的临时目录，默认系统临时目录；中间文件每个预设测完当场删 |
| `--dry-run` | 预演，不写任何文件 |

### 退出码

| 码 | 含义 |
|---|---|
| 0 | 没有 FAIL（可以有 INFO） |
| 1 | 至少一首 FAIL |
| 2 | 基础设施错：参数错、输入 / 标注 / Apple 目录不存在、没有 wav、ffmpeg 起不来、输出写不了、意料之外的异常 |

单个文件读不了 **不是** 基础设施错：那一首记 FAIL（"文件读不了"），其余文件照常检查。

### 输出（`--out` 目录）

- `delivery_check.csv`：**真源**，全部 58 列（下表），每首一行，多首时末尾再加一行 `EP 汇总`。数值算不出 / 没测一律留空，绝不填 0；`±inf` 写 `inf` / `-inf`。
- `delivery_check.md`：由 CSV 读回来生成，可以直接给厂牌：逐首核对表（含 EP 汇总行）、判定详情、文件与其他项、判定表、说明。编码列没有数的格子写「未测」。
- `environment.json`：ffmpeg 完整版本文本（没做编码回测时为 `null`）、包版本、`codec_tp_check.py` 的 sha256、命令行、输入文件的大小与 sha256、阈值快照、判定计数。同一台机器、同一条命令、同样的输入再跑，三个文件字节相同（里面不记时间）。

## 判定

只有三档：PASS / INFO / FAIL。**判失败只有四项**（Max 2026-10-11「全按推荐」认的计划 §13 Q9），其余一律只报（INFO，标黄）。
判定项与阈值集中在 `codec_tp_check.py` 顶部那一张常量表，下表的数字取自它；写进 `delivery_check.md` 的判定表由同一组常量生成，不会脱节。

| 级别 | 条件 | 常量 |
|---|---|---|
| FAIL | PCM 真峰值（8 倍过采样，`tp8_dbtp`）高于档上限 + 0.01 dB：流媒体 -2.0 dBTP；俱乐部取 `--ceiling`（默认 -1.0） | `STREAMING_TP_LIMIT_DBTP`、`CLUB_DEFAULT_CEILING_DBTP`、`TP_LIMIT_TOLERANCE_DB` |
| FAIL | 原采样点削波数 `clipped_sample_count` 大于 0（浮点文件：样本绝对值大于 1.0；整数 PCM：样本贴满刻度码值，单个就算） | `INTEGER_PCM_BITS` |
| FAIL | 文件读不了，或含非有限样本（NaN / Inf） | |
| FAIL | 必需编码器缺失、或某个编码预设整列都没测出来（ffmpeg 里没有该编码器；或每个能测的文件编码都失败）。"必需" = 本次 `--codecs` 里要的预设 | |
| INFO | 编码再浮点解码后的真峰值高于 0 dBTP（四个 ffmpeg 预设 + Apple 参考列各自判） | `CODEC_TP_WARN_DBTP`（暂定） |
| INFO | 目标核对：依据响度距目标超过 ±1 LU。有 drop 标注用 drop 段的 S，否则用整曲 I（`target_basis`） | `TARGET_WINDOW_LU` |
| INFO | 主段 PSR P10 低于下限（流媒体 7 / 俱乐部 6 dB），或主段 PSR 中位低于下限（流媒体 8 / 俱乐部 6.5 dB） | `PSR_P10_FLOOR_DB`、`PSR_MEDIAN_FLOOR_DB`（暂定） |
| INFO | 直流偏置高于 -60 dBFS | `DC_WARN_DBFS`（暂定） |
| INFO | 开头静音超过 1 s，或结尾静音超过 3 s（静音 = 样本都不超过 -60 dBFS） | `LEAD_SILENCE_WARN_S`、`TRAIL_SILENCE_WARN_S`（暂定） |
| INFO | 采样率不是 44.1 / 48 kHz，或位深不是 PCM_16 / PCM_24（`celadon_render` 的 32-bit float 成品会被标位深） | `EXPECTED_SAMPLE_RATES`、`EXPECTED_SUBTYPES`（暂定） |
| INFO | 单声道低频损失低于 -1 dB | `MONO_LF_LOSS_WARN_DB` |
| INFO | 疑似带着临时监听导出（整曲 S/M 低于 -60 dB；或 I 比目标低 3 LU 以上且真峰值比上限低 3 dB 以上） | c1 的 `SIDE_MID_SUSPECT_DB` 等 |
| INFO | 有项目因素材原因算不出（`na_reason` 非空）；PCM 真峰值没测出来 | |
| INFO | EP 汇总：各曲主段 S 极差超过 1 LU、谱斜率极差超过 0.3 dB/oct、主段 PSR 中位极差超过 1.5 dB（R1 G7） | `EP_*_MAX_*` |

LRA 只报数，没有阈值（R1 G10 的"出口 LRA ≥ 入口 LRA - 1"需要入口，成品直测没有入口）。
Apple AAC 缺文件（没给目录、或目录里没有同名文件）只写 `notes`，**不判**，也不让整行变 INFO。

### 主段、目标核对与谱斜率

- **主段**（`main_basis`）：有段落标注且有窗完整落在某个 drop 段里 -> `drop`；否则 -> `loudest`：短期响度 S 不低于 P95 - 3 LU 的 3 s 窗（R1-01 口径）。S 不高于 -70 LUFS 的窗永远不进主段。
  标注按"素材裁到小节线"约定：第 1 小节从 0 s 起，每小节秒数 = 每小节拍数 × 60 / bpm（拍数取 `meter` 的分子，缺省 4/4）。
- **主段 S**（`main_s_lufs`）：选中窗的短期响度做能量平均（同 `house_gate.py` 的"K 加权能量平均"）。**主段 PSR**（`main_psr_p10_db`、`main_psr_median_db`）：选中窗的 PSR_k = 窗内 8 倍真峰值 - S_k 的 P10 与中位数。
- **目标核对**：有 drop 标注时核对的是 drop 段 S（规格第 4 节：目标是 drop 响度，不是整曲积分；T-085 实测整曲 I 中位与目标相差 0.7 ～ 2 LU（目标 -10 时 -10.69，目标 -14 时 -12.01），按整曲判会误报）；没有标注只能退回整曲 I，此时偏离多半只是曲子本身起伏大。
- **谱斜率**（`spectral_slope_db_per_oct`）：主段音频（选中窗合并）的 Welch 长时平均功率谱（两声道功率平均），累加成 27 个 1/3 倍频程段（中心 1000·2^((b-14)/3) Hz，同 `BalanceModule`），段总功率（dB）对 log2 频率做最小二乘直线，粉红噪声 = 0（口径同 `LiveProfile` 的 `spectralSlopeDbPerOct`）。
  比最响的段低过 60 dB 的段不进回归，进回归的段少于 6 个读空并写 `na_reason`。受母带低通影响（MP3 来源的素材高频段会很低）。

## 列说明（`delivery_check.csv`）

文字列没有就是空串；数值列没算出 / 没测就是空格子。前 7 列是判定头，`measure_master` 的字段口径见 c1 的 README。

| 列 | 单位 | 含义 |
|---|---|---|
| `file` | text | 文件名；EP 汇总行写 `EP 汇总` |
| `verdict` | text | PASS / INFO / FAIL；有失败项即 FAIL，否则有只报项或算不出的项即 INFO，否则 PASS |
| `fail_reasons` | text | 判失败的原因，多条用 "; " 连接 |
| `info_flags` | text | 只报不判（标黄）的项，多条用 "; " 连接 |
| `delivery` | text | streaming / club |
| `target_lufs` | LUFS | 目标响度 |
| `tp_limit_dbtp` | dBTP | PCM 真峰值上限 |
| `subtype` | text | 位深（libsndfile 子类型） |
| `sample_rate` | Hz | 采样率 |
| `channels` | count | 声道数；响度类量具只支持 2 |
| `duration_s` | s | 时长 |
| `nonfinite_sample_count` | count | 非有限样本个数；大于 0 时测量项全部留空 |
| `lufs_i` | LUFS | 整曲积分响度 |
| `lra_lu` | LU | 响度范围（EBU Tech 3342） |
| `sample_peak_dbfs` | dBFS | 采样峰值 |
| `tp4_dbtp` | dBTP | 4 倍过采样真峰值 |
| `tp8_dbtp` | dBTP | 8 倍过采样真峰值（判失败用的主口径） |
| `sample_clip_count` | count | c1 口径：样本绝对值大于 1.0 的个数（整数 PCM 恒为 0，判失败不用它） |
| `intersample_clip4_count` | count | 4 倍插值点绝对值大于 1.0 的个数 |
| `intersample_clip8_count` | count | 8 倍插值点绝对值大于 1.0 的个数 |
| `crest_db` | dB | 采样峰值 - 全频 RMS |
| `plr_db` | dB | `tp8_dbtp` - `lufs_i` |
| `psr_p10_db` | dB | 整曲 PSR 序列 P10 |
| `psr_median_db` | dB | 整曲 PSR 序列中位数 |
| `psr_p90_db` | dB | 整曲 PSR 序列 P90 |
| `dc_offset_dbfs` | dBFS | 直流偏置 |
| `lead_silence_s` | s | 开头静音 |
| `trail_silence_s` | s | 结尾静音 |
| `low_corr_lt125hz` | ratio | 125 Hz 以下左右相关度 |
| `mono_lf_loss_db` | dB | 单声道求和低频损失 |
| `side_mid_db` | dB | 整曲 S/M |
| `monitor_suspicion` | text | 疑似带着临时监听导出的原因；空 = 无嫌疑 |
| `clipped_sample_count` | count | **判失败用**的原采样点削波数（口径见上表第二行） |
| `tp_margin_db` | dB | 真峰值余量 = `tp_limit_dbtp` - `tp8_dbtp`（负 = 超限） |
| `target_basis` | text | 目标核对的依据：`drop` 或 `whole` |
| `target_check_lufs` | LUFS | 目标核对用的响度值 |
| `target_delta_lu` | LU | `target_check_lufs` - `target_lufs` |
| `main_basis` | text | 主段的定义：`drop` / `loudest`；空 = 没有主段 |
| `main_seconds` | s | 主段窗合并后的总时长 |
| `main_s_lufs` | LUFS | 主段 S |
| `main_psr_p10_db` | dB | 主段 PSR P10 |
| `main_psr_median_db` | dB | 主段 PSR 中位数 |
| `spectral_slope_db_per_oct` | dB/oct | 主段谱斜率 |
| `tp8_mp3_320_dbtp` | dBTP | MP3 320（libmp3lame）编码再浮点解码后的 8 倍真峰值；未测留空 |
| `overshoot8_mp3_320_db` | dB | 解码后真峰值 - `tp8_dbtp`；未测留空 |
| `tp8_aac_256_pns_off_dbtp` | dBTP | 原生 AAC 256k、`-aac_pns 0`（**AAC 主列**） |
| `overshoot8_aac_256_pns_off_db` | dB | 同上的过冲 |
| `tp8_aac_256_default_dbtp` | dBTP | 原生 AAC 256k、ffmpeg 默认（PNS 开）作对照 |
| `overshoot8_aac_256_default_db` | dB | 同上的过冲 |
| `tp8_opus_160_dbtp` | dBTP | Opus 160（libopus，`-application audio -vbr on`） |
| `overshoot8_opus_160_db` | dB | 同上的过冲 |
| `tp8_apple_aac_256_dbtp` | dBTP | Apple AAC 256 参考列：afconvert 解回的 wav 的 8 倍真峰值；没给目录 / 缺文件留空 |
| `overshoot8_apple_aac_256_db` | dB | 同上的过冲 |
| `ep_main_s_range_lu` | LU | 仅 EP 汇总行：各曲主段 S 的极差 |
| `ep_slope_range_db_per_oct` | dB/oct | 仅 EP 汇总行：各曲谱斜率的极差 |
| `ep_psr_median_range_db` | dB | 仅 EP 汇总行：各曲主段 PSR 中位的极差 |
| `na_reason` | text | 有字段因素材原因算不出时的原因（含 c1 的原因 + 主段 / 谱斜率的原因） |
| `notes` | text | 不影响判定的备注：主段回退到最响窗口的原因、Apple 缺文件、单个文件编码回测失败的原因等 |

编码回测的命令形状与 T-085 `main()` 里内联的那条相同：`ffmpeg -i MASTER -map 0:a:0 -c:a ENC -b:a Nk [额外参数] OUT`，再 `-c:a pcm_f32le` 浮点解码（不削波、不加增益）。
EP 汇总行只在一次给了 2 首及以上时出；单首没有极差，不出这一行。

## Apple AAC 参考列的 Mac 流程

计划 §13 Q13 的窄例外：Mac 上**只**用系统 `afconvert` 编码再解码，不量测、不装 Python、不编译；量测仍在 DGX。这一列只作参考，缺了不判失败。

1. 把要参考的成品 wav 放到 Mac 上一个目录（例如 `~/Desktop/masters/`；可以是 DGX 渲好拷下来的，也可以是 DAW 导出的）。
2. 先预演，看清每条命令：
   ```bash
   tools/render/afconvert_roundtrip.sh --dry-run ~/Desktop/masters ~/Desktop/apple_aac
   ```
3. 正式跑（每个 wav 编成 AAC 256 kbps CBR，再解回 32-bit float wav，同名写进输出目录；默认跳过已有同名文件，`--force` 覆盖）：
   ```bash
   tools/render/afconvert_roundtrip.sh ~/Desktop/masters ~/Desktop/apple_aac
   ```
   它实际执行的是 `afconvert -f m4af -d aac -b 256000 -s 0 -q 127 IN.wav TMP.m4a` 和 `afconvert -f WAVE -d LEF32 TMP.m4a OUT.wav`（后一条与 `使用方式.md` 里 Max 已在用的写法同形）。
4. 把 `~/Desktop/apple_aac/` 拷到 DGX，量测时加 `--apple-aac-dir <拷过去的路径>`，文件名与成品一一对应。

注意：输入若是浮点文件且峰值超过 0 dBFS，afconvert 会在编码前处理超限，参考列就不代表"原样编码"；成品按交付要求本来不该超限，超限的文件会先被判 FAIL。

## 已知限制与待 Max 认的数

- **整数 PCM 的削波口径**是我暂定的：样本贴满刻度码值（16 / 24 / 32 bit）即算一个削波点，单个就判失败。c1 的 `sample_clip_count` 对整数文件恒为 0（见 c1 README "已知限制"），所以另加了 `clipped_sample_count`。
  一个干净的限幅成品在 Ceiling 之下不会碰满刻度码值；如果 Max 想要"连续若干个样本贴满"之类的更严口径，改 `clipped_sample_count()` 一处即可。
- **标「暂定」的阈值**（编码后真峰值标黄线 0 dBTP、PSR 下限 7/8 与 6/6.5、DC -60 dBFS、静音 1 s / 3 s、采样率与位深的期望值）是任务书没给、我按调研报告（R1 G1 / G3 / G10 / G7）取的起点值，只影响 INFO，不影响退出码。
- 主段的 `loudest` 回退依据是 S 的 P95 - 3 LU，对"整首都在同一响度"的曲子等于整首；它不是 drop，别拿它和 `--target-lufs` 当成 drop 核对。
- 标注的第 1 小节假定从 wav 的 0 s 开始；DAW 导出带前导静音时标注要自己平移，或接受主段回退。
- 谱斜率的 Welch 段长 16384（48 kHz 下 2.9 Hz 一格），40 Hz 一段只有约 3 格，低段读数有 ±1 dB 量级的离散抖动，对 27 段回归的斜率影响约 0.03 dB/oct。
- Opus 内部走 48 kHz；输入是 44.1 kHz 时 ffmpeg 会重采样，解码后真峰值是 48 kHz 网格上量的。
- DGX（aarch64）上 ffmpeg 6.1.1 的数字与 T-085（x86_64，ffmpeg 7.1.5）不做逐位对比，只比量级。
- `tools/render/requirements.txt` 没建（被模块边界拦着，c1 已报），依赖版本见 `docs/validation/2026-10-08-T085-codec-tp/requirements.txt`。
- 抖动提示：若交付采样率不同于工程采样率，抖动请放在宿主的最后一步。这句提示印在 `delivery_check.md` 的说明里；插件帮助文字（`HelpText.cpp`）按计划 Q15 等 T-095 合并后再改，不在本线。
- R2-02(a)（冲击 / 通透 / Clip 0 三组对比）不在本线：它依赖 (d) 的参数覆盖开关和 (a) 的等响二分。

## 自检

`tools/render/delivery_check_selfcheck.py`（numpy 合成样例，不读外部文件、不调 ffmpeg，编码回测用注入的假函数）：

- 已知 TP 超限 -> FAIL：fs/4 正弦 45 度相位，真峰值 -1.0 dBTP 而采样峰值只有 -4.0 dBFS；俱乐部按 Ceiling 判（-0.5 dBTP 失败、Ceiling -0.5 时 -1.0 通过）。
- 已知削波 -> FAIL：幅度 1.2 的 32-bit float 正弦；幅度 1.5 削到满刻度码值的 24 bit 文件；干净文件计数为 0。
- DC +0.01（-40 dBFS）、开头 2 s / 结尾 4 s 静音、整体降 10 dB（低 I + 监听嫌疑）、左右完全相同（纯 M，S/M = -inf）-> 只报（INFO）且值正确。
- 全部干净 -> PASS：脉冲 + 低频音 + 噪声的合成成品，I、S、PSR、S/M 都能手算（见 `clean_master` 的注释）。
- 判定边界（上限 + 0.01）、四个失败项、十三类只报项逐类触发、NaN 不触发；整列没测出来才 FAIL，部分文件失败只 INFO；Apple 缺文件只写备注。
- 主段选择（标注 / 最响回退 / 静音窗不进）、标注小节换算、区间合并、主段统计、削波口径、谱斜率（白噪声 +3、粉红 0、-3、单音 NaN）。
- 命令行：退出码 0 / 1 / 2（八种基础设施错）、三个输出文件及列头、`._` 文件被跳过、`--dry-run` 不写文件、README 列出每个字段。

退出码 0 = 全部 ok，1 = 有 FAIL。自检不过就不能说完成。

## 验收命令（在 DGX 上跑，由主会话统一执行）

```bash
PY=~/.cache/celadon-spark/venv/bin/python

# 1. 合成自检：期望每行 ok，最后一行 "N/N ok"，退出码 0
$PY tools/render/delivery_check_selfcheck.py

# 2. c1 的量具自检不受影响：期望全部 ok，退出码 0
$PY tools/render/measure_selfcheck.py

# 3. T-085 默认命令输出逐字节不变：用同一批旧输入、同一个渲染二进制，改前文件（git show 3a7a367:tools/render/codec_tp_check.py > /tmp/old.py）
#    与改后文件各跑一遍，对 masters.csv、codec_roundtrips.csv、environment.json（命令行一行除外）diff，期望为空
$PY /tmp/old.py --render build/celadon_render --input <旧输入> --work /tmp/w_old --out /tmp/o_old --targets -10
$PY tools/render/codec_tp_check.py --render build/celadon_render --input <旧输入> --work /tmp/w_new --out /tmp/o_new --targets -10
diff /tmp/o_old/masters.csv /tmp/o_new/masters.csv && diff /tmp/o_old/codec_roundtrips.csv /tmp/o_new/codec_roundtrips.csv

# 4. 对 DGX 上一首成品跑 master-check：期望出三个文件，退出码 0 或 1（视这首的真峰值 / 削波而定），不是 2
$PY tools/render/codec_tp_check.py master-check --in <一首成品.wav> --delivery streaming --out /tmp/dc_out
ls /tmp/dc_out   # delivery_check.csv  delivery_check.md  environment.json
```
