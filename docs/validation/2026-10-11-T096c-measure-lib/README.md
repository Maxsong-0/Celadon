# T-096c 统一量具库（`tools/render/codec_tp_check.py`）

状态：代码改完，未验证（本机和 DGX 上都还没跑过；验收命令见文末，由主会话在 DGX 跑）。

## 这是什么

T-096 第 1 步里，(a) 指标管线（`quality_metrics.py`）和 (c2) 交付检查单都要量真峰值、响度、PSR、LRA。
ADR-034 决定 6 要求每个量只留一个定义，Max 在 Q3 定的位置是 `tools/render/codec_tp_check.py`。
本任务（c1）在这个文件里补齐量具函数，别的脚本只许 `import`，不许再写一份。

不变的部分：`measure(path)` 的签名、返回的键与顺序、数值，以及 `main()` 的默认命令行输出（`masters.csv`、
`codec_roundtrips.csv`、`environment.json` 的列与内容）都不动。做法是把 `measure()` 原来的函数体拆成几块小函数
（K 加权、短期响度序列、PSR 序列、真峰值扫描）再按原顺序拼回去；算术语句没改，只是搬了位置。
`main()` 一行没动。

## 用法

```python
import sys
sys.path.insert(0, 'tools/render')          # 或者在 tools/render 目录里运行
import codec_tp_check as meters

row = meters.measure_master('master.wav', target_lufs=-10.0, ceiling_dbtp=-2.0)   # 一个成品一行，字段见下表
lra = meters.loudness_range_lu(audio, sr)    # audio: float 数组 (帧, 2)，sr: 采样率
p10, p50, p90 = meters.psr_percentiles(audio, sr)
```

自检（在 DGX 的 venv 里；在哪个目录跑都行）：

```
~/.cache/celadon-spark/venv/bin/python tools/render/measure_selfcheck.py
```

## 纯函数（输入 float 数组，不读文件）

`x` 是 `(帧, 声道)` 数组，一维按单声道；`sr` 是采样率。所有函数都不改输入。

| 函数 | 口径与返回 |
|---|---|
| `short_term_loudness_series(x, sr, hop_s=0.1)` | BS.1770 K 加权（沿用 `measure()` 的 pyloudnorm DeMan 滤波器）3 s 窗，每 `hop_s` 一个值（LUFS，未过任何门）；不足 3 s 返回空数组 |
| `loudness_range_lu(x, sr)` | EBU Tech 3342：短期值先过 -70 LUFS 绝对门，再过「能量平均 - 20 LU」相对门，取 P95 - P10（`np.percentile` 线性插值）；没有足够的值返回 NaN |
| `psr_series_db(x, sr)` | 每 100 ms 一个：3 s 尾随 8 倍真峰值 - 同窗 S；S 不高于 -70 LUFS 的窗不算。就是 `measure()` 里 `psr_median_db` 用的序列 |
| `psr_percentiles(x, sr)` | `(P10, 中位, P90)`，序列为空时三个都是 NaN |
| `dc_offset_db(x)` | 各声道均值绝对值的较大者，dBFS；零偏置读 -300（`db()` 的下限） |
| `leading_trailing_silence_s(x, sr, thresh_dbfs=-60)` | `(开头秒数, 结尾秒数)`；静音 = 所有声道样本绝对值都不超过门（逐样本，不是 RMS）；整段静音时两个值都是总时长 |
| `sample_clip_count(x)` | 样本绝对值大于 1.0 的个数；`measure()` 也用它 |
| `low_band_correlation(x, sr, fc=125)` | rho = 2ΣLR / (ΣL² + ΣR²)，4 阶 Butterworth 低通、单向滤波、整段累计；口径同 `RenderMain.cpp` 的 `measure()`（:1226-1441）。低通后均方不高于 1e-14 返回 NaN |
| `mono_lf_loss_db(rho)` | 10·log10((1 + rho) / 2)；rho = -1 为 -inf；不像 `ClayGuard.cpp:113` 那样把 rho 钳到 -0.99（那是界面数字封顶用的） |
| `side_mid_ratio_db(x)` | 整曲 10·log10(ΣS² / ΣM²)，M = (L+R)/2，S = (L-R)/2；纯中间为 -inf，纯侧为 +inf，全静音 NaN |
| `monitor_export_suspicion(stats, target_lufs, ceiling_dbtp)` | 返回原因字符串，没有嫌疑返回空串。规则 1：S/M 低于 -60 dB；规则 2：I 比目标低至少 3 LU 且 tp8 比 Ceiling 低至少 3 dB（目标与 Ceiling 都要给）。stats 用 `side_mid_db`、`lufs_i`、`tp8_dbtp` 三个字段名 |
| `true_peak_scan(audio, rate)` | 独立 4 倍 / 8 倍真峰值（原 `measure()` 里的那段，64 抽头、Kaiser β=12），返回 `(字段, 按 100 ms 分箱的 8 倍峰值)`；一般不直接用 |

文件接口：

| 函数 | 说明 |
|---|---|
| `file_info(path)` | 只读文件头：`subtype`、`sample_rate`、`channels`、`duration_s` |
| `measure(path)` | 老接口，T-085 在用，不变 |
| `measure_master(path, target_lufs=None, ceiling_dbtp=None)` | 一个成品一行，字段见下表，键顺序同表 |

## `measure_master()` 字段表

真源是 `codec_tp_check.py` 顶部的 `MASTER_FIELDS`，下表由它抄出；`measure_selfcheck.py` 会核对本 README 里每个字段名都在。
数值算不出时是 NaN（写 CSV 请写空值），原因写进 `na_reason`，绝不填 0。

| 字段 | 单位 | 含义 |
|---|---|---|
| `file` | text | 文件名（不含目录） |
| `subtype` | text | libsndfile 子类型（即位深）：PCM_16 / PCM_24 / FLOAT 等 |
| `sample_rate` | Hz | 采样率 |
| `channels` | count | 声道数；响度类量具只支持 2 |
| `duration_s` | s | 时长 |
| `nonfinite_sample_count` | count | 非有限样本（NaN / Inf）个数；大于 0 时下面的测量项全部留空 |
| `lufs_i` | LUFS | 整曲积分响度（-70 绝对门 + -10 LU 相对门） |
| `lra_lu` | LU | 响度范围（EBU Tech 3342） |
| `sample_peak_dbfs` | dBFS | 采样峰值 |
| `tp4_dbtp` | dBTP | 4 倍过采样真峰值 |
| `tp8_dbtp` | dBTP | 8 倍过采样真峰值（主口径） |
| `sample_clip_count` | count | 样本绝对值大于 1.0 的个数（只对浮点文件有意义；整数 PCM 恒为 0） |
| `intersample_clip4_count` | count | 4 倍插值点绝对值大于 1.0 的个数 |
| `intersample_clip8_count` | count | 8 倍插值点绝对值大于 1.0 的个数 |
| `crest_db` | dB | 采样峰值 - 全频 RMS |
| `plr_db` | dB | `tp8_dbtp` - `lufs_i` |
| `psr_p10_db` | dB | PSR 序列 P10 |
| `psr_median_db` | dB | PSR 序列中位数（T-085 口径） |
| `psr_p90_db` | dB | PSR 序列 P90 |
| `dc_offset_dbfs` | dBFS | 直流偏置 |
| `lead_silence_s` | s | 开头静音（-60 dBFS 门） |
| `trail_silence_s` | s | 结尾静音 |
| `low_corr_lt125hz` | ratio | 125 Hz 以下左右相关度（与 `metrics.csv` 同名同口径） |
| `mono_lf_loss_db` | dB | 单声道求和低频损失 |
| `side_mid_db` | dB | 整曲 S/M |
| `monitor_suspicion` | text | 疑似带着临时监听导出的原因；空 = 无嫌疑 |
| `na_reason` | text | 有字段因素材原因算不出时的原因；空 = 该算的都算出了 |

与 `measure()` 同名的字段（`lufs_i`、`tp8_dbtp`、`psr_median_db` 等）就是 `measure()` 的值，同一份实现。

## 常数

全部集中在 `codec_tp_check.py` 顶部（`HOP_S`、`SHORT_TERM_WINDOW_S`、`PSR_*`、`LRA_*`、`LOW_BAND_*`、`SILENT_MEAN_SQUARE`、
`SILENCE_THRESHOLD_DBFS`、`SIDE_MID_SUSPECT_DB`、`SUSPECT_*`），改口径只改那一处。
其中 `SUSPECT_TP_MARGIN_DB = 3.0`（「TP 远低于 Ceiling」）是我暂定的取值，任务书只给了 -60 dB 与 3 LU，待 Max 认。

## 出错约定（Max 的 Q12）

- 基础设施错（文件读不了）：`measure_master` / `file_info` 直接抛 soundfile 的异常，调用者（c2）捕获后按退出码 1 处理。
- 素材原因算不出（不够 3 s、声道数不是 2、低频无能量、没有主段等）：数值字段 NaN + `na_reason`，不抛异常。
- 含非有限样本：不抛异常，`nonfinite_sample_count` 记个数、其余测量项全空，是否判失败由调用者定（c2 计划里它是失败项之一）。
- 量具只报告，不改任何文件，不自动回退增益。

## 如何确认 `measure()` 逐位不变

自检里的 `measure_keeps_its_keys_and_order` 只核对键序。逐位对账在 DGX 上做：把改前的文件（`git show 3a7a367:tools/render/codec_tp_check.py`）
和现在的文件各自 import，对同一个 wav 比 `repr(measure(p))`；再用 T-085 的一首旧输入跑 `main()` 默认命令，`masters.csv` 与
`codec_roundtrips.csv` 与改前输出 `diff` 为空。具体命令见主会话的验收清单。

## 已知限制

- `sample_clip_count` 的判据是「样本绝对值大于 1.0」，整数 PCM（16 / 24 bit）读成浮点后最大只有 1.0，所以对整数文件恒为 0。
  交付检查要想对 24 bit 成品判「原采样点削波」，需要另定口径（例如连续若干个样本贴满刻度），这属完成标准，本任务没加。
- LRA 的分位数用线性插值，没有做 EBU 参考实现的 0.1 LU 直方图量化，与 ffmpeg `ebur128` 的读数预计相差在 0.1 LU 量级（未实测）。
- 短期响度窗与 PSR 窗按 `round(采样率 × 0.1)` 取步长；对 10 的整倍数采样率（44.1k / 48k / 88.2k / 96k / 176.4k / 192k）与原来的 `rate // 10` 完全一致。
- `K` 加权用了 pyloudnorm 的私有属性 `Meter._filters`，所以 pyloudnorm 版本必须钉死（T-085 用的是 0.2.0）。
- 依赖版本见 `docs/validation/2026-10-08-T085-codec-tp/requirements.txt`（numpy 2.5.3、scipy 1.18.1、soundfile 0.14.0、pyloudnorm 0.2.0）；
  `tools/render/requirements.txt` 这次被写入钩子拦下（新增依赖属模块边界），待放行后补。

## 自检覆盖的已知答案

两段电平差 10 dB 的 LRA ≈ 10 LU、恒定电平 LRA ≈ 0、相对门挡掉低 40 dB 的尾段、不足 3 s 返回 NaN；
fs/4 正弦 45 度相位的真峰值比采样峰值高 3.01 dB；双声道正弦的 PSR ≈ 0、占空比 1/4 脉冲串的 PSR 比同峰值正弦高 6.02 dB、增益不变性；
纯 DC 偏置读数、首尾静音秒数与门电平、削波计数；同相 / 反相低频 rho = +1 / -1 与单声道损失 0 / -inf、高频不影响 rho、
rho 是能量比不是皮尔逊（0.8 的例子）、全静音 NaN；S/M 的 -20 dB 例子与纯中间 -inf；监听嫌疑两条规则及其边界；
`file_info`、`measure()` 键序、`measure_master` 键序与字段表一致且与 `measure()` 逐位相等、单声道 / 过短 / 含 NaN 文件的留空与 `na_reason`。
