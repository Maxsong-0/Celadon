#!/usr/bin/env python3
"""T-085 offline codec round trips; never changes plugin parameters or defaults.

Dependencies: numpy, scipy, soundfile, pyloudnorm. Audio stays in --work;
CSV/JSON evidence goes in --out. See the T-085 REPORT.md for definitions.
"""
import argparse
import csv
import hashlib
import json
from importlib import metadata as importlib_metadata
import math
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import traceback

import numpy as np
import pyloudnorm as pyln
import soundfile as sf
from scipy.signal import chirp, firwin, sosfilt, butter
from scipy.signal import welch
from scipy.fft import rfft, irfft, next_fast_len

CODECS = (('aac', 256, 'm4a'), ('aac', 128, 'm4a'),
          ('libopus', 160, 'opus'), ('libopus', 96, 'opus'),
          ('libmp3lame', 320, 'mp3'), ('libmp3lame', 128, 'mp3'))

# ---- 量具常数：口径只在这里改一处（README 与 measure_selfcheck.py 都按这里取值）----
HOP_S = 0.1                      # 响度 / PSR 序列的步长（s）；measure() 一直用的 100 ms 网格
SHORT_TERM_WINDOW_S = 3.0        # BS.1770 / EBU Tech 3341 短期响度窗（s）
PSR_WINDOW_HOPS = int(round(SHORT_TERM_WINDOW_S / HOP_S))   # PSR 的 3 s 尾随窗 = 30 步（峰值按步分箱，窗必须是整数步）
PSR_GATE_LUFS = -70.0            # S 不高于它的窗不进 PSR（静音段没有意义）
PSR_LOW_PERCENTILE = 10          # psr_percentiles 的 P10 / P90
PSR_HIGH_PERCENTILE = 90
LRA_ABSOLUTE_GATE_LUFS = -70.0   # EBU Tech 3342 绝对门
LRA_RELATIVE_GATE_LU = -20.0     # EBU Tech 3342 相对门：绝对门之后短期值（能量平均）再低 20 LU
LRA_LOW_PERCENTILE = 10          # LRA = 门后短期分布的 P95 - P10
LRA_HIGH_PERCENTILE = 95
LOW_BAND_CUTOFF_HZ = 125.0       # 低频相关度的低通截止，同 RenderMain.cpp lowCorrelationCutoffHz
LOW_BAND_ORDER = 4               # 4 阶 Butterworth（单向滤波），同 RenderMain.cpp 的 Lowpass4
SILENT_MEAN_SQUARE = 1.0e-14     # 低通后均方不高于它按「没有信号」算，同 RenderMain.cpp silentMeanSquare
SILENCE_THRESHOLD_DBFS = -60.0   # 首尾静音：所有声道的样本绝对值都不超过它
SIDE_MID_SUSPECT_DB = -60.0      # 整曲 S/M 低于它 -> 疑似带着临时监听（单声道求和）导出
SUSPECT_LOUDNESS_GAP_LU = 3.0    # I 比目标低达到它，并且 ……
SUSPECT_TP_MARGIN_DB = 3.0       # …… 真峰值比 Ceiling 低达到它（「远低于」的暂定取值，待 Max 认）-> 疑似带着监听衰减导出

# ---- measure_master() 的字段表：(name, unit, 含义) ----
# (a) 的 quality_metrics.py 与 (c2) 的交付检查单都按这里的 name 取值，不各自另起名字。
# unit == 'text' 的是字符串列，其余是数值；数值算不出时留 NaN（写 CSV 时写空值）并在 na_reason 写原因，绝不填 0。
# 和 measure() 同名的字段（lufs_i、tp8_dbtp、psr_median_db 等）就是 measure() 的值，不是另算的一份。
MASTER_FIELDS = (
    ('file', 'text', '文件名（不含目录）'),
    ('subtype', 'text', 'libsndfile 子类型（即位深）：PCM_16 / PCM_24 / FLOAT 等'),
    ('sample_rate', 'Hz', '采样率'),
    ('channels', 'count', '声道数；响度类量具只支持 2（立体声）'),
    ('duration_s', 's', '时长'),
    ('nonfinite_sample_count', 'count', '非有限样本（NaN / Inf）个数；大于 0 时下面的测量项全部留空'),
    ('lufs_i', 'LUFS', '整曲积分响度（BS.1770 K 加权，-70 LUFS 绝对门 + -10 LU 相对门）'),
    ('lra_lu', 'LU', '响度范围（EBU Tech 3342：3 s 短期值，-70 绝对门 + -20 LU 相对门，P95 - P10）'),
    ('sample_peak_dbfs', 'dBFS', '采样峰值（所有声道）'),
    ('tp4_dbtp', 'dBTP', '4 倍过采样真峰值'),
    ('tp8_dbtp', 'dBTP', '8 倍过采样真峰值（主口径）'),
    ('sample_clip_count', 'count', '样本绝对值大于 1.0 的个数（只对浮点文件有意义；整数 PCM 读出来恒为 0）'),
    ('intersample_clip4_count', 'count', '4 倍过采样插值点绝对值大于 1.0 的个数'),
    ('intersample_clip8_count', 'count', '8 倍过采样插值点绝对值大于 1.0 的个数'),
    ('crest_db', 'dB', '峰值因数：采样峰值 - 全频 RMS'),
    ('plr_db', 'dB', 'PLR = tp8_dbtp - lufs_i'),
    ('psr_p10_db', 'dB', '3 s 尾随 PSR（8 倍真峰值 - 同窗短期响度 S）序列的 P10；只统计 S 高于 -70 LUFS 的窗'),
    ('psr_median_db', 'dB', '同上序列的中位数（T-085 沿用的口径）'),
    ('psr_p90_db', 'dB', '同上序列的 P90'),
    ('dc_offset_dbfs', 'dBFS', '直流偏置：各声道均值绝对值的较大者；零偏置读 -300'),
    ('lead_silence_s', 's', '开头静音时长（所有声道的样本都不超过 -60 dBFS）'),
    ('trail_silence_s', 's', '结尾静音时长（判定同上）'),
    ('low_corr_lt125hz', 'ratio', '125 Hz 以下左右相关度 rho = 2·sum(LR) / (sum(L^2) + sum(R^2))，-1 ~ +1'),
    ('mono_lf_loss_db', 'dB', '单声道求和的低频功率损失 10·log10((1 + rho) / 2)，不大于 0；rho = -1 时为 -inf'),
    ('side_mid_db', 'dB', '整曲侧中比 S/M = 10·log10(sum(S^2) / sum(M^2))，M = (L+R)/2，S = (L-R)/2；纯中间信号为 -inf'),
    ('monitor_suspicion', 'text', '「疑似带着临时监听导出」的原因；空 = 无嫌疑'),
    ('na_reason', 'text', '有字段因素材原因算不出时的原因；空 = 该算的都算出了'),
)
MASTER_FIELD_NAMES = tuple(name for name, _unit, _meaning in MASTER_FIELDS)

# ---- 交付检查单（master-check 子命令，T-096c2）：判定项与阈值，全部集中在这一块 ----
# 判定三档：PASS / INFO（只报、标黄）/ FAIL。FAIL 只有四项（Max 2026-10-11 Q9）：
#   ① PCM 真峰值高于档上限 + 容差；② 原采样点削波数 > 0；③ 文件读不了或含非有限样本；④ 必需编码器缺失导致整列没测出来。
# 其余一律只报（INFO），绝不判失败。注释里写「暂定」的数字是任务书没给、我按调研报告取的起点值，待 Max 认。
STREAMING_TP_LIMIT_DBTP = -2.0       # 流媒体档 PCM 真峰值上限（固定值；ADR-034 决定 5）
CLUB_DEFAULT_CEILING_DBTP = -1.0     # 俱乐部档上限 = 用户的 Ceiling；--ceiling 不给时取它
TP_LIMIT_TOLERANCE_DB = 0.01         # 真峰值超限的容差，同 gate_check.py / house_gate.py 的 +0.01
DELIVERY_TARGET_LUFS = {'streaming': -10.0, 'club': -7.0}   # --target-lufs 不给时的默认（档基准；风格偏移由使用者自己填）
TARGET_WINDOW_LU = 1.0               # 目标核对：依据响度距目标超过它标黄（同 house_gate.py 的 TARGET_WINDOW_LU）
CODEC_TP_WARN_DBTP = 0.0             # 编码再解码后真峰值高于它标黄（解码器输出会在满刻度处削波）；暂定
PSR_P10_FLOOR_DB = {'streaming': 7.0, 'club': 6.0}       # 主段 PSR P10 下限：R1 G3 ① / PsrGuard.cpp:7-16 的数；暂定
PSR_MEDIAN_FLOOR_DB = {'streaming': 8.0, 'club': 6.5}    # 主段 PSR 中位下限：R1 G10；暂定
DC_WARN_DBFS = -60.0                 # 直流偏置高于它标黄；暂定
LEAD_SILENCE_WARN_S = 1.0            # 开头静音超过它标黄；暂定
TRAIL_SILENCE_WARN_S = 3.0           # 结尾静音超过它标黄；暂定
MONO_LF_LOSS_WARN_DB = -1.0          # 单声道低频损失低于它标黄（R1 G1 ②）
EXPECTED_SAMPLE_RATES = (44100, 48000)       # 采样率不在此列标黄；暂定
EXPECTED_SUBTYPES = ('PCM_16', 'PCM_24')     # 位深（libsndfile 子类型）不在此列标黄；暂定
EP_MAIN_S_RANGE_MAX_LU = 1.0         # EP 汇总：各曲主段 S 极差上限（R1 G7）
EP_SLOPE_RANGE_MAX_DB_PER_OCT = 0.3  # EP 汇总：各曲谱斜率极差上限（R1 G7）
EP_PSR_MEDIAN_RANGE_MAX_DB = 1.5     # EP 汇总：各曲主段 PSR 中位极差上限（R1 G7）
MAIN_LOUDEST_PERCENTILE = 95         # 没有 drop 标注时，主段 = 短期响度不低于「P95 - 下面这个数」的 3 s 窗（R1-01 口径）
MAIN_LOUDEST_BELOW_LU = 3.0
SLOPE_BAND_COUNT = 27                # 谱斜率：27 个 1/3 倍频程段，中心 1000·2^((b-14)/3) Hz（39.4 Hz ～ 16 kHz，同 BalanceModule）
SLOPE_NPERSEG = 16384                # 谱斜率：Welch 段长（48 kHz 下 2.9 Hz 一格，最低的 40 Hz 段有 3 格）
SLOPE_BAND_GATE_DB = 60.0            # 谱斜率：比最响的段低过它的段不进回归（同 LiveProfile 的 -60 dB 门，这里按相对电平取）
SLOPE_MIN_BANDS = 6                  # 谱斜率：进回归的段少于它就读 NaN（同 LiveProfile 的 minimumSlopeBands）
INTEGER_PCM_BITS = {'PCM_16': 16, 'PCM_24': 24, 'PCM_32': 32}   # 整数 PCM 的削波口径：样本贴满刻度码值（浮点文件看 |x| > 1.0）

# 编码回测预设（--codecs 的取值）。命令形状与 main() 里 T-085 的内联流程相同：
#   ffmpeg -i MASTER -map 0:a:0 -c:a ENCODER -b:a Nk [extra] OUT；再 -c:a pcm_f32le 浮点解码，不削波。
CODEC_PRESETS = {
    'mp3-320': {'label': 'MP3 320', 'encoder': 'libmp3lame', 'bitrate_kbps': 320, 'extension': 'mp3', 'extra_args': ()},
    'aac-256-pns-off': {'label': 'AAC 256（PNS 关）', 'encoder': 'aac', 'bitrate_kbps': 256, 'extension': 'm4a',
                        'extra_args': ('-aac_pns', '0')},
    'aac-256-default': {'label': 'AAC 256（ffmpeg 默认）', 'encoder': 'aac', 'bitrate_kbps': 256, 'extension': 'm4a',
                        'extra_args': ()},
    'opus-160': {'label': 'Opus 160', 'encoder': 'libopus', 'bitrate_kbps': 160, 'extension': 'opus',
                 'extra_args': ('-application', 'audio', '-vbr', 'on')},
}
APPLE_AAC_KEY = 'apple-aac-256'       # Apple AAC 参考列：由 Mac 上 afconvert 编解码后拷回的 wav 量出，不是 ffmpeg 预设
CODEC_COLUMN_ORDER = tuple(CODEC_PRESETS) + (APPLE_AAC_KEY,)
CODEC_LABELS = {**{key: preset['label'] for key, preset in CODEC_PRESETS.items()}, APPLE_AAC_KEY: 'Apple AAC 256（参考）'}
EP_ROW_NAME = 'EP 汇总'               # CSV / Markdown 里 EP 汇总行的 file 列
CSV_NAME, MD_NAME, ENVIRONMENT_NAME = 'delivery_check.csv', 'delivery_check.md', 'environment.json'


def codec_column_names(key):
    """某个编码列在 CSV 里的两个列名：(解码后 8 倍真峰值, 相对成品的过冲)。"""
    slug = key.replace('-', '_')
    return f'tp8_{slug}_dbtp', f'overshoot8_{slug}_db'


# delivery_check.csv 的字段表：(name, unit, 含义)。unit == 'text' 是字符串列，其余是数值，没算出 / 没测留空，绝不填 0。
_DELIVERY_HEAD_FIELDS = (
    ('file', 'text', '文件名（不含目录）；EP 汇总行写「EP 汇总」'),
    ('verdict', 'text', '判定：PASS / INFO / FAIL；有 FAIL 项即 FAIL，否则有只报项或算不出的项即 INFO，否则 PASS'),
    ('fail_reasons', 'text', '判失败的原因，多条用 "; " 连接；空 = 没有失败项'),
    ('info_flags', 'text', '只报不判（标黄）的项，多条用 "; " 连接；空 = 没有'),
    ('delivery', 'text', '交付档：streaming / club'),
    ('target_lufs', 'LUFS', '目标响度（--target-lufs，或档基准）'),
    ('tp_limit_dbtp', 'dBTP', 'PCM 真峰值上限（流媒体固定 -2.0；俱乐部 = --ceiling，默认 -1.0）'),
)
_DELIVERY_EXTRA_FIELDS = (
    ('clipped_sample_count', 'count', '原采样点削波数（判失败用）：浮点文件 = 样本绝对值大于 1.0 的个数；整数 PCM = 样本贴满刻度码值的个数'),
    ('tp_margin_db', 'dB', '真峰值余量 = tp_limit_dbtp - tp8_dbtp（负 = 超限）'),
    ('target_basis', 'text', '目标核对的依据：drop = 标注的 drop 段短期响度；whole = 整曲 I'),
    ('target_check_lufs', 'LUFS', '目标核对用的响度值（依据见 target_basis）'),
    ('target_delta_lu', 'LU', 'target_check_lufs - target_lufs'),
    ('main_basis', 'text', '主段的定义：drop = 标注里的 drop 段；loudest = 短期响度不低于 P95 - 3 LU 的 3 s 窗；空 = 没有主段'),
    ('main_seconds', 's', '主段窗合并后的总时长'),
    ('main_s_lufs', 'LUFS', '主段 S：主段内各 3 s 窗短期响度（能量平均）'),
    ('main_psr_p10_db', 'dB', '主段内 PSR 序列的 P10（PSR 下限核对用）'),
    ('main_psr_median_db', 'dB', '主段内 PSR 序列的中位数（PSR 下限核对与 EP 极差用）'),
    ('spectral_slope_db_per_oct', 'dB/oct', '主段谱斜率：27 个 1/3 倍频程段电平对 log2 频率的最小二乘斜率，粉红噪声 = 0'),
)
_DELIVERY_CODEC_FIELDS = tuple(
    field for key in CODEC_COLUMN_ORDER for field in (
        (codec_column_names(key)[0], 'dBTP', f'{CODEC_LABELS[key]}：编码再浮点解码后的 8 倍真峰值；未测留空'),
        (codec_column_names(key)[1], 'dB', f'{CODEC_LABELS[key]}：解码后真峰值 - 成品 tp8_dbtp；未测留空'),
    ))
_DELIVERY_EP_FIELDS = (
    ('ep_main_s_range_lu', 'LU', '仅 EP 汇总行：各曲主段 S 的极差（最大 - 最小）'),
    ('ep_slope_range_db_per_oct', 'dB/oct', '仅 EP 汇总行：各曲谱斜率的极差'),
    ('ep_psr_median_range_db', 'dB', '仅 EP 汇总行：各曲主段 PSR 中位的极差'),
)
_DELIVERY_TAIL_FIELDS = (
    next(field for field in MASTER_FIELDS if field[0] == 'na_reason'),
    ('notes', 'text', '不影响判定的备注（主段改用最响窗口的原因、Apple AAC 缺文件、编码回测失败的原因等）'),
)
# 列顺序：判定头 -> measure_master 的字段（去掉 file 和 na_reason）-> 本命令新增 -> 各编码列 -> EP 极差 -> na_reason / notes
DELIVERY_FIELDS = (_DELIVERY_HEAD_FIELDS
                   + tuple(field for field in MASTER_FIELDS if field[0] not in ('file', 'na_reason'))
                   + _DELIVERY_EXTRA_FIELDS + _DELIVERY_CODEC_FIELDS + _DELIVERY_EP_FIELDS + _DELIVERY_TAIL_FIELDS)
DELIVERY_FIELD_NAMES = tuple(name for name, _unit, _meaning in DELIVERY_FIELDS)


def run(command, log=None):
    result = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, check=True)
    if log is not None:
        Path(log).write_text(result.stdout, encoding='utf-8')
    return result.stdout


def db(value):
    return 20 * math.log10(max(float(value), 1e-15))


def write_csv(path, rows):
    with Path(path).open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def download_jamendo(args):
    """Repository T-038 manifest, first N IDs per category, full tracks, no redistribution."""
    manifest = Path(__file__).resolve().parents[2] / 'docs/validation/2026-10-01-T038-reference/manifest.tsv'
    with manifest.open() as stream:
        rows = list(csv.DictReader(stream, delimiter='\t'))
    records = []
    args.input.mkdir(parents=True, exist_ok=True)
    download = args.work / 'downloads'
    download.mkdir(parents=True, exist_ok=True)
    for category in ('deep', 'tech', 'melodic'):
        for row in [r for r in rows if r['category'] == category][:args.jamendo_per_class]:
            track = str(int(row['track_id'].split('_')[1]))
            url = f'https://prod-1.storage.jamendo.com/download/track/{track}/mp32/'
            mp3 = download / (track + '.mp3')
            run(['curl', '-sSL', '--fail', '--retry', '2', '--max-time', '120', '-o', mp3, url])
            wav = args.input / f'{category}_{track}.wav'
            run([args.ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', mp3,
                 '-ar', '48000', '-ac', '2', '-c:a', 'pcm_f32le', wav])
            records.append({**row, 'url': url, 'download_sha256': hashlib.sha256(mp3.read_bytes()).hexdigest()})
    write_csv(args.out / 'corpus.csv', records)


def synthetic(folder, seconds=40, rate=48000):
    """Fixed seed; four supplementary stress cases, not substitutes for music."""
    folder.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(85)
    t = np.arange(seconds * rate) / rate
    beat = t % .5
    kick = np.sin(2 * np.pi * (55 * beat + 35 * .025 * (1 - np.exp(-beat / .025)))) * np.exp(-beat * 22)
    noise = rng.standard_normal((len(t), 2))
    hats = sosfilt(butter(4, 7500, 'highpass', fs=rate, output='sos'), noise, axis=0)
    env = np.exp(-(t % .125) * 65)
    bass = np.sin(2 * np.pi * 55 * t)
    cases = {
        'stress_hot_mix': np.tanh(2.2 * (.55 * noise + kick[:, None] + .4 * bass[:, None])),
        'stress_dense_hats': .8 * hats * env[:, None] + .35 * kick[:, None],
        'stress_side_bass': np.column_stack((bass, -bass)) * .8 + .12 * noise,
        'stress_sweep': np.column_stack((chirp(t, 20, seconds, 22000, method='logarithmic'),
                                        chirp(t, 22000, seconds, 20, method='logarithmic'))) * .9,
    }
    for name, audio in cases.items():
        audio *= .97 / np.max(np.abs(audio))
        # Brief fades avoid endpoint discontinuities; preserve the full programme.
        fade = np.minimum(1, np.minimum(t / .02, (seconds - t) / .02))
        audio *= fade[:, None]
        desired = -8.0 if name == 'stress_hot_mix' else -18.0
        loudness = pyln.Meter(rate, filter_class='DeMan').integrated_loudness(audio)
        audio *= min(1.0, 10 ** ((desired - loudness) / 20))
        sf.write(folder / (name + '.wav'), audio, rate, subtype='FLOAT')


# ======== 量具库 ========
# 下面的函数是全仓库唯一的真峰值 / 响度 / PSR / LRA / 低频相关度等定义，别处只许 import，不许再写一份。
# measure() 只是把原来的函数体拆成几块小函数再按原顺序拼回去（算术语句、键顺序都没动），
# 所以它的返回值和 main() 的默认命令行输出应当与 T-085 逐位相同；改这一段后要拿旧输入对账（见 README）。

def _as_audio_2d(x):
    """输入统一成 float64 的 (帧, 声道) 数组；一维按单声道，绝不改调用者的数据。"""
    audio = np.asarray(x, dtype=np.float64)
    if audio.ndim == 1:
        audio = audio[:, None]
    if audio.ndim != 2:
        raise ValueError('audio must be 1-D or (frames, channels)')
    return audio


def _hop_samples(rate, hop_s=HOP_S):
    """步长换成样本数（整数）。HOP_S = 0.1 时，对 10 的整倍数采样率就是 rate // 10。"""
    return max(1, int(round(rate * hop_s)))


def _k_weight(audio, meter):
    """BS.1770 K 加权后的副本。用的是 pyloudnorm 私有的 meter._filters，所以包版本钉在 requirements.txt。"""
    weighted = audio.copy()
    for filt in meter._filters.values():
        for channel in range(audio.shape[1]):
            weighted[:, channel] = filt.apply_filter(weighted[:, channel])
    return weighted


def _short_term_series(weighted, window, hop):
    """K 加权音频上的短期响度序列（LUFS）：窗终点 = window, window + hop, ... 不超过总帧数；各声道权重 1。"""
    energy = np.sum(weighted ** 2, axis=1)
    sums = np.concatenate(([0.0], np.cumsum(energy)))
    return np.array([-0.691 + 10 * math.log10(max((sums[end] - sums[end - window]) / window, 1e-30))
                     for end in range(window, len(weighted) + 1, hop)], dtype=np.float64)


def _psr_series(weighted, hop_peaks, hop):
    """PSR 序列（dB）：每个步长一个，3 s 尾随 8 倍真峰值 - 同窗 K 加权短期响度 S；S 不高于门的窗不算。"""
    short = _short_term_series(weighted, PSR_WINDOW_HOPS * hop, hop)
    psr = []
    for k, level in enumerate(short):
        end_index = PSR_WINDOW_HOPS + k   # = 窗终点 // hop
        if level > PSR_GATE_LUFS:
            psr.append(db(hop_peaks[end_index - PSR_WINDOW_HOPS:end_index].max()) - level)
    return np.array(psr, dtype=np.float64)


def _psr_summary(psr):
    """PSR 序列的 (P10, 中位, P90)；序列为空三个都是 NaN。"""
    if len(psr) == 0:
        return math.nan, math.nan, math.nan
    low, high = np.percentile(psr, [PSR_LOW_PERCENTILE, PSR_HIGH_PERCENTILE])
    return float(low), float(np.median(psr)), float(high)


def sample_clip_count(x):
    """样本绝对值大于 1.0（满刻度）的个数。整数 PCM 读成浮点后最大就是 1.0，所以只对浮点文件有意义。"""
    return int(np.count_nonzero(np.abs(x) > 1))


def true_peak_scan(audio, rate):
    """独立的 4 倍 / 8 倍真峰值扫描（audio 是 float64 的 (帧, 声道) 数组）。
    返回 (字段, hop_peaks)：字段 = tp4_dbtp / intersample_clip4_count / tp8_dbtp / intersample_clip8_count，
    键的顺序就是 measure() 一直以来的列顺序；hop_peaks = 8 倍真峰值按 100 ms 分箱的各声道最大值（PSR 用）。"""
    result = {}
    # Independent linear-phase interpolation: BS.1770 Annex 2 oversampling method.
    # 64 taps per phase, Kaiser beta=12, unity DC; not JUCE's detector nor the
    # illustrative 12-tap/phase ITU coefficients. 4x and 8x reported separately.
    hop = _hop_samples(rate)
    bins = math.ceil(len(audio) / hop)
    peaks8 = np.zeros(bins)
    for factor in (4, 8):
        kernel = firwin(64 * factor + 1, 1 / factor, window=('kaiser', 12.0))
        peak, count = 0.0, 0
        for start in range(0, len(audio), hop * 100):
            stop = min(start + hop * 100, len(audio))
            lo, hi = max(0, start - 64), min(len(audio), stop + 64)
            # Equivalent to resample_poly's centered FIR, but reuse the input
            # FFT across phases and never allocate a full 8x waveform.
            chunk = audio[lo:hi]
            size = next_fast_len(len(chunk) + 64)
            spectrum = rfft(chunk, n=size, axis=0)
            for phase in range(factor):
                taps = kernel[phase::factor] * factor
                filtered = irfft(spectrum * rfft(taps, n=size)[:, None], n=size, axis=0)
                core = np.abs(filtered[32 + start - lo:32 + stop - lo])
                peak = max(peak, float(np.max(core)))
                if phase != 0:
                    count += int(np.count_nonzero(core > 1))
                if factor == 8:
                    mono = core.max(axis=1)
                    for offset in range(0, stop - start, hop):
                        index = (start + offset) // hop
                        peaks8[index] = max(peaks8[index], mono[offset:offset + hop].max())
        result[f'tp{factor}_dbtp'] = db(peak)
        result[f'intersample_clip{factor}_count'] = count
    return result, peaks8


def _measure_core(audio, rate):
    """measure() 的计算本体（audio 已确认是有限的双声道、至少 3 s）。返回 (结果字典, PSR 序列)。"""
    meter = pyln.Meter(rate, filter_class='DeMan')  # BS.1770 K weighting, 400 ms, 75% overlap, -70 / -10 LU gates.
    loudness = float(meter.integrated_loudness(audio))
    sample_peak = np.max(np.abs(audio))
    result = {'duration_s': len(audio) / rate, 'sample_rate': rate, 'lufs_i': loudness,
              'sample_peak_dbfs': db(sample_peak),
              'sample_clip_count': sample_clip_count(audio),
              'crest_db': db(sample_peak) - db(np.sqrt(np.mean(audio ** 2)))}
    scan, hop_peaks = true_peak_scan(audio, rate)
    result.update(scan)
    # PSR: synchronized trailing 3 s TP minus ungated 3 s K-weighted loudness,
    # every 100 ms; exclude S < -70 LUFS. Whole-track PLR is a scalar.
    psr = _psr_series(_k_weight(audio, meter), hop_peaks, _hop_samples(rate))
    result['psr_median_db'] = float(np.median(psr)) if len(psr) else math.nan
    result['plr_db'] = result['tp8_dbtp'] - loudness
    return result, psr


def measure(path):
    audio, rate = sf.read(path, always_2d=True, dtype='float64')
    if audio.shape[1] != 2 or not np.all(np.isfinite(audio)) or len(audio) < 3 * rate:
        raise ValueError(f'{path}: requires finite stereo audio >= 3 seconds')
    return _measure_core(audio, rate)[0]


def short_term_loudness_series(x, sr, hop_s=HOP_S):
    """BS.1770 K 加权的 3 s 短期响度序列（LUFS，未过任何门）。
    第 i 个值是以 3 s + i·hop_s 为终点的那个 3 s 窗；不足 3 s 返回空数组。x 是 (帧, 声道) 数组，一维按单声道。"""
    audio = _as_audio_2d(x)
    weighted = _k_weight(audio, pyln.Meter(sr, filter_class='DeMan'))
    return _short_term_series(weighted, int(round(sr * SHORT_TERM_WINDOW_S)), _hop_samples(sr, hop_s))


def loudness_range_lu(x, sr):
    """响度范围 LRA（LU），EBU Tech 3342：短期值先过 -70 LUFS 绝对门，再过「能量平均 - 20 LU」相对门，取 P95 - P10。
    分位数用 np.percentile 线性插值（不做 0.1 LU 直方图量化）。没有足够的短期值（不足 3 s、全静音）返回 NaN。"""
    short = short_term_loudness_series(x, sr)
    short = short[short > LRA_ABSOLUTE_GATE_LUFS]
    if len(short) == 0:
        return math.nan
    relative_gate = 10 * math.log10(np.mean(10 ** (short / 10))) + LRA_RELATIVE_GATE_LU
    short = short[short > relative_gate]
    if len(short) == 0:
        return math.nan
    low, high = np.percentile(short, [LRA_LOW_PERCENTILE, LRA_HIGH_PERCENTILE])
    return float(high - low)


def psr_series_db(x, sr):
    """PSR 序列（dB）：每 100 ms 一个，3 s 尾随 8 倍真峰值 - 同窗 K 加权短期响度 S；S 不高于 -70 LUFS 的窗不算。
    这就是 measure() 里 psr_median_db 的那个序列（measure() 与本函数共用同一段实现）。"""
    audio = _as_audio_2d(x)
    _, hop_peaks = true_peak_scan(audio, sr)
    weighted = _k_weight(audio, pyln.Meter(sr, filter_class='DeMan'))
    return _psr_series(weighted, hop_peaks, _hop_samples(sr))


def psr_percentiles(x, sr):
    """返回 (P10, 中位, P90) 三个 PSR 值（dB）；序列为空时三个都是 NaN。"""
    return _psr_summary(psr_series_db(x, sr))


def dc_offset_db(x):
    """直流偏置（dBFS）：各声道均值绝对值的较大者；零偏置读 -300（db() 的下限）。空数组返回 NaN。"""
    audio = _as_audio_2d(x)
    if len(audio) == 0:
        return math.nan
    return db(np.max(np.abs(np.mean(audio, axis=0))))


def leading_trailing_silence_s(x, sr, thresh_dbfs=SILENCE_THRESHOLD_DBFS):
    """(开头静音秒数, 结尾静音秒数)：静音 = 所有声道的样本绝对值都不超过 thresh_dbfs（逐样本，不是 RMS）。
    整段都是静音时两个值都等于总时长。"""
    audio = _as_audio_2d(x)
    loud = np.flatnonzero(np.max(np.abs(audio), axis=1) > 10 ** (thresh_dbfs / 20))
    if len(loud) == 0:
        return len(audio) / sr, len(audio) / sr
    return float(loud[0]) / sr, float(len(audio) - 1 - loud[-1]) / sr


def low_band_correlation(x, sr, fc=LOW_BAND_CUTOFF_HZ):
    """fc 以下左右相关度 rho = 2·sum(LR) / (sum(L^2) + sum(R^2))，-1 ~ +1，x 必须是双声道。
    口径同 RenderMain.cpp 的 measure()（4 阶 Butterworth 低通、单向滤波、整段累计）与 ClayGuard 的低频相关度：
    它看的是侧声道能量占多少，左右电平不等时与皮尔逊相关系数不同。低通后几乎无能量（均方不高于 SILENT_MEAN_SQUARE）返回 NaN。"""
    audio = _as_audio_2d(x)
    if audio.shape[1] != 2:
        raise ValueError('low_band_correlation requires stereo audio')
    if len(audio) == 0:
        return math.nan
    low = sosfilt(butter(LOW_BAND_ORDER, fc, 'lowpass', fs=sr, output='sos'), audio, axis=0)
    power = float(np.sum(low[:, 0] ** 2) + np.sum(low[:, 1] ** 2))
    if power / len(audio) <= SILENT_MEAN_SQUARE:
        return math.nan
    return 2 * float(np.sum(low[:, 0] * low[:, 1])) / power


def mono_lf_loss_db(rho):
    """左右等功率时，单声道求和的低频功率损失 10·log10((1 + rho) / 2)（dB，不大于 0）；rho = -1 时为 -inf。
    与 ClayGuard.cpp 的 houseMonoLossDb 同一公式，区别：那边把 rho 钳到 [-0.99, 1] 只为界面数字封顶（约 -23 dB），这里不钳。"""
    if math.isnan(rho):
        return math.nan
    ratio = (1.0 + min(float(rho), 1.0)) / 2.0
    return 10 * math.log10(ratio) if ratio > 0 else -math.inf


def side_mid_ratio_db(x):
    """整曲侧中比 S/M（dB）= 10·log10(sum(S^2) / sum(M^2))，M = (L + R) / 2，S = (L - R) / 2，全频段，x 必须是双声道。
    纯中间信号（S 全 0）为 -inf，纯侧信号为 +inf，两者都为 0（全静音）返回 NaN。"""
    audio = _as_audio_2d(x)
    if audio.shape[1] != 2:
        raise ValueError('side_mid_ratio_db requires stereo audio')
    mid_energy = float(np.sum((0.5 * (audio[:, 0] + audio[:, 1])) ** 2))
    side_energy = float(np.sum((0.5 * (audio[:, 0] - audio[:, 1])) ** 2))
    if mid_energy == 0 and side_energy == 0:
        return math.nan
    if side_energy == 0:
        return -math.inf
    if mid_energy == 0:
        return math.inf
    return 10 * math.log10(side_energy / mid_energy)


def monitor_export_suspicion(stats, target_lufs, ceiling_dbtp):
    """「疑似带着临时监听导出」的原因字符串，没有嫌疑返回空串。stats 用 measure_master 的字段名：
    side_mid_db、lufs_i、tp8_dbtp。两条规则（任一成立即报，都成立则用 '; ' 连起来）：
    1) 整曲 S/M 低于 SIDE_MID_SUSPECT_DB（几乎只有中间信号，像是带着单声道求和导出）；
    2) 整曲 I 比目标低至少 SUSPECT_LOUDNESS_GAP_LU，且 8 倍真峰值比 Ceiling 低至少 SUSPECT_TP_MARGIN_DB
       （像是带着监听衰减导出；需要 target_lufs 与 ceiling_dbtp 都给）。字段是 NaN 的规则不触发。"""
    reasons = []
    side_mid = stats.get('side_mid_db', math.nan)
    if side_mid < SIDE_MID_SUSPECT_DB:
        reasons.append(f'S/M {side_mid:.1f} dB 低于 {SIDE_MID_SUSPECT_DB:.0f} dB：几乎只有中间信号')
    if target_lufs is not None and ceiling_dbtp is not None:
        gap = target_lufs - stats.get('lufs_i', math.nan)
        margin = ceiling_dbtp - stats.get('tp8_dbtp', math.nan)
        if gap >= SUSPECT_LOUDNESS_GAP_LU and margin >= SUSPECT_TP_MARGIN_DB:
            reasons.append(f'I 比目标低 {gap:.1f} LU，且真峰值比 Ceiling 低 {margin:.1f} dB')
    return '; '.join(reasons)


def file_info(path):
    """文件头信息（不读音频）：subtype（位深）、sample_rate、channels、duration_s。读不了文件时抛 soundfile 的异常。"""
    info = sf.info(path)
    return {'subtype': info.subtype, 'sample_rate': info.samplerate, 'channels': info.channels,
            'duration_s': info.frames / info.samplerate}


def measure_master(path, target_lufs=None, ceiling_dbtp=None):
    """量一个成品文件，返回 MASTER_FIELDS 里全部字段（键顺序同字段表）。
    基础设施错（文件读不了）直接抛异常，由调用者按退出码 1 处理；素材原因算不出的数值字段留 NaN，
    原因写进 na_reason，绝不填 0；含非有限样本时只报个数、其余测量项全部留空。
    target_lufs / ceiling_dbtp 只用于 monitor_suspicion，不给就不判第 2 条规则。"""
    info = file_info(path)
    audio, rate = sf.read(path, always_2d=True, dtype='float64')
    row = {name: ('' if unit == 'text' else math.nan) for name, unit, _meaning in MASTER_FIELDS}
    row.update(file=Path(path).name, subtype=info['subtype'], sample_rate=info['sample_rate'],
               channels=info['channels'], duration_s=info['duration_s'])
    row['nonfinite_sample_count'] = int(np.count_nonzero(~np.isfinite(audio)))
    if row['nonfinite_sample_count']:
        row['na_reason'] = '含非有限样本（NaN / Inf），测量项全部未测'
        return row
    if len(audio) == 0:
        row['na_reason'] = '空文件，测量项全部未测'
        return row
    reasons = []
    row['sample_clip_count'] = sample_clip_count(audio)
    row['dc_offset_dbfs'] = dc_offset_db(audio)
    row['lead_silence_s'], row['trail_silence_s'] = leading_trailing_silence_s(audio, rate)
    stereo = audio.shape[1] == 2
    if stereo:
        rho = low_band_correlation(audio, rate)
        row['low_corr_lt125hz'] = rho
        row['mono_lf_loss_db'] = mono_lf_loss_db(rho)
        row['side_mid_db'] = side_mid_ratio_db(audio)
        if math.isnan(rho):
            reasons.append('低频相关度：125 Hz 以下几乎没有能量')
        if math.isnan(row['side_mid_db']):
            reasons.append('S/M：全程没有信号')
    else:
        reasons.append(f"声道数 {info['channels']} 不是 2：响度 / 真峰值 / PSR / LRA / 低频相关度 / S/M 未测")
    if stereo and len(audio) >= 3 * rate:
        core, psr = _measure_core(audio, rate)
        row.update(core)
        row['psr_p10_db'], _, row['psr_p90_db'] = _psr_summary(psr)
        row['lra_lu'] = loudness_range_lu(audio, rate)
        if len(psr) == 0:
            reasons.append('PSR：没有 S 高于 -70 LUFS 的 3 s 窗')
        if math.isnan(row['lra_lu']):
            reasons.append('LRA：没有足够的短期响度值')
    elif stereo:
        reasons.append('时长不足 3 s：响度 / 真峰值 / PSR / LRA 未测')
    row['monitor_suspicion'] = monitor_export_suspicion(row, target_lufs, ceiling_dbtp)
    row['na_reason'] = '; '.join(reasons)
    return row


# ======== 交付检查单（T-096c2）：master-check 子命令，只量成品，不渲染，不改任何输入 ========
# 量具（真峰值 / 响度 / PSR / LRA / DC / 静音 / 低频相关 / 侧中比）全部沿用上面的定义；这一段只加
# 「怎么选主段、怎么判、怎么出表」和三个新量：逐窗对齐的 S 与峰值、谱斜率、削波口径。
# T-085 的默认命令（main() 里不带子命令的那一支）一行没动，它的输出与改前逐字节相同。
# 判定项与阈值在文件顶部那张常量表里；下面的函数不写死数字。

class InfrastructureError(Exception):
    """基础设施错（参数 / 路径 / ffmpeg 不可用 / 标注读不了 / 输出写不了）：退出码 2。素材本身的问题写进行里，不走这里。"""


# ---------------------------------------------------------------- 新量（纯函数，输入 float 数组）

def windowed_short_term_and_peak_db(x, sr):
    """逐窗对齐的 (短期响度 S, 同窗 8 倍真峰值 dBTP)，两个数组一样长；第 k 个窗从 k·hop 开始、长 3 s。
    就是 psr_series_db 内部的那两个序列（PSR_k = 峰值_k - S_k），区别是这里不丢掉 S 不高于 -70 LUFS 的窗，
    调用者才能按时间挑出主段。不足 3 s 返回两个空数组。"""
    audio = _as_audio_2d(x)
    _, hop_peaks = true_peak_scan(audio, sr)
    hop = _hop_samples(sr)
    weighted = _k_weight(audio, pyln.Meter(sr, filter_class='DeMan'))
    short = _short_term_series(weighted, PSR_WINDOW_HOPS * hop, hop)
    peaks = np.array([db(hop_peaks[k:k + PSR_WINDOW_HOPS].max()) for k in range(len(short))], dtype=np.float64)
    return short, peaks


def clipped_sample_count(x, subtype):
    """原采样点削波数（判失败用）。浮点文件：样本绝对值大于 1.0 的个数（同 sample_clip_count）；
    整数 PCM（16 / 24 / 32 bit）读成浮点后最大只有满刻度码值，所以数「贴满刻度码值」的样本，单个就算。"""
    audio = _as_audio_2d(x)
    bits = INTEGER_PCM_BITS.get(subtype)
    if bits is None:
        return sample_clip_count(audio)
    full_scale = 1.0 - 2.0 ** -(bits - 1)   # 正向最大码值 / 2^(bits-1)；负向最小码值读出来恰好是 -1.0
    return int(np.count_nonzero(np.abs(audio) >= full_scale - 1e-12))


def spectral_slope_db_per_oct(x, sr):
    """谱斜率（dB/oct）：Welch 长时平均功率谱（两声道功率平均），累加成 27 个 1/3 倍频程段总功率（dB），
    对 log2 频率（段号 / 3）做最小二乘直线，粉红噪声 = 0（口径同 LiveProfile 的 spectralSlopeDbPerOct）。
    比最响的段低过 SLOPE_BAND_GATE_DB 的段不进回归；进回归的段少于 SLOPE_MIN_BANDS 或音频短于一个 Welch 段，读 NaN。"""
    audio = _as_audio_2d(x)
    if len(audio) < SLOPE_NPERSEG:
        return math.nan
    frequencies, spectrum = welch(audio, fs=sr, window='hann', nperseg=SLOPE_NPERSEG, scaling='spectrum', axis=0)
    power = np.mean(spectrum, axis=1)
    octaves, levels = [], []
    for band in range(SLOPE_BAND_COUNT):
        centre = 1000.0 * 2.0 ** ((band - 14) / 3.0)
        low, high = centre / 2.0 ** (1.0 / 6.0), centre * 2.0 ** (1.0 / 6.0)
        if high > sr / 2.0:
            continue   # 采样率太低，这一段超过奈奎斯特
        inside = (frequencies >= low) & (frequencies < high)
        if not inside.any():
            continue
        levels.append(10 * math.log10(max(float(np.sum(power[inside])), 1e-30)))
        octaves.append(band / 3.0)
    if not levels:
        return math.nan
    levels, octaves = np.array(levels), np.array(octaves)
    keep = levels >= levels.max() - SLOPE_BAND_GATE_DB
    if np.count_nonzero(keep) < SLOPE_MIN_BANDS:
        return math.nan
    return float(np.polyfit(octaves[keep], levels[keep], 1)[0])


# ---------------------------------------------------------------- 主段

def drop_spans_seconds(annotation):
    """段落标注 -> [(开始秒, 结束秒)]，只取 kind == 'drop' 的段。小节从 1 起、两端都含；第 1 小节从 0 s 起
    （标注按「素材裁到小节线」做的）。每小节秒数 = 每小节拍数 × 60 / bpm，拍数取 meter 的分子（缺省 4/4）。
    标注缺 bpm、sections 等换算不了时抛 ValueError。没有 drop 段返回空列表。"""
    try:
        bpm = float(annotation['bpm'])
        beats = int(str(annotation.get('meter', '4/4')).split('/')[0])
        bar_s = beats * 60.0 / bpm
        return [((int(s['start_bar']) - 1) * bar_s, int(s['end_bar']) * bar_s)
                for s in annotation['sections'] if s.get('kind') == 'drop']
    except (KeyError, TypeError, ValueError, ZeroDivisionError) as error:
        raise ValueError(f'标注无法换算成秒（需要 bpm 与 sections）：{type(error).__name__} {error}') from error


def find_annotation(annotations, stem):
    """按文件名（不含扩展名）找标注：先整名相等，再认 celadon_render 的「<歌名>__g…」命名，取最长的歌名前缀。没有返回 None。"""
    if stem in annotations:
        return annotations[stem]
    prefixes = [song for song in annotations if stem.startswith(song + '__')]
    return annotations[max(prefixes, key=len)] if prefixes else None


def select_main_windows(short, hop_s, window_s, drop_spans=None):
    """挑主段窗。short = 各窗短期响度（第 k 窗从 k·hop_s 起、长 window_s）。返回 (布尔掩码, 依据, 备注)。
    drop_spans 是 [(开始秒, 结束秒)]：给了且有窗完整落在某个 drop 段内 -> 依据 'drop'；
    否则（没标注 / 标注没有 drop 段 / drop 段放不下一个窗）-> 依据 'loudest'：S 不低于 P95 - 3 LU 的窗。
    S 不高于 -70 LUFS 的窗永远不进主段。没有任何可用的窗：全 False、依据空串。"""
    short = np.asarray(short, dtype=np.float64)
    valid = short > PSR_GATE_LUFS
    if not valid.any():
        return np.zeros(len(short), dtype=bool), '', f'没有 S 高于 {PSR_GATE_LUFS:g} LUFS 的 3 s 窗，主段未定'
    note = ''
    if drop_spans is not None:
        starts = np.arange(len(short)) * hop_s
        inside = np.zeros(len(short), dtype=bool)
        for begin, end in drop_spans:
            inside |= (starts >= begin - 1e-9) & (starts + window_s <= end + 1e-9)
        mask = valid & inside
        if mask.any():
            return mask, 'drop', ''
        note = '标注里没有 drop 段' if not drop_spans else '标注的 drop 段里放不下一个 3 s 窗'
        note += '，主段改按最响窗口'
    reference = float(np.percentile(short[valid], MAIN_LOUDEST_PERCENTILE))
    return valid & (short >= reference - MAIN_LOUDEST_BELOW_LU), 'loudest', note


def merged_sample_spans(mask, hop, window, total):
    """被选中的窗合并成不重叠的样本区间 [(起, 止)]；hop / window 是样本数，total 是总样本数。"""
    spans = []
    for k in np.flatnonzero(mask):
        start, end = int(k) * hop, min(int(k) * hop + window, total)
        if spans and start <= spans[-1][1]:
            spans[-1][1] = max(spans[-1][1], end)
        else:
            spans.append([start, end])
    return [(start, end) for start, end in spans]


def main_section_stats(short, peaks_db, mask):
    """主段统计 (主段 S, PSR P10, PSR 中位)：S = 选中窗短期响度的能量平均；PSR_k = 窗峰值 - S_k。没有窗时全 NaN。"""
    if not np.any(mask):
        return math.nan, math.nan, math.nan
    chosen = np.asarray(short, dtype=np.float64)[mask]
    main_s = 10 * math.log10(float(np.mean(10 ** (chosen / 10))))
    p10, median, _ = _psr_summary(np.asarray(peaks_db, dtype=np.float64)[mask] - chosen)
    return main_s, p10, median


# ---------------------------------------------------------------- ffmpeg 编码回测（有副作用的部分）

def parse_ffmpeg_encoders(text):
    """解析 `ffmpeg -encoders` 的输出，返回编码器名集合。只认 ------ 分隔线之后的行（前面是图例，也有「A..... = Audio」）。"""
    names, started = set(), False
    for line in text.splitlines():
        if not started:
            started = line.strip().startswith('------')
            continue
        parts = line.split()
        if len(parts) >= 2 and len(parts[0]) == 6 and parts[0][0] in 'VAS':
            names.add(parts[1])
    return names


def probe_ffmpeg(ffmpeg):
    """(完整版本文本, 可用编码器集合)。ffmpeg 起不来属于基础设施错；某个编码器没编进去不算，那是判失败的第 4 项。"""
    try:
        version = run([ffmpeg, '-version'])
        encoders = parse_ffmpeg_encoders(run([ffmpeg, '-hide_banner', '-encoders']))
    except (OSError, subprocess.CalledProcessError) as error:
        raise InfrastructureError(f'ffmpeg 不可用（{ffmpeg}）：{error}') from error
    return version, encoders


def encode_command(ffmpeg, source, key, encoded):
    """预设 -> ffmpeg 编码命令；形状与 main() 里 T-085 内联的那条相同（-map 0:a:0 -c:a ENC -b:a Nk [额外参数] OUT）。"""
    preset = CODEC_PRESETS[key]
    return ([ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', source, '-map', '0:a:0',
             '-c:a', preset['encoder'], '-b:a', f"{preset['bitrate_kbps']}k"] + list(preset['extra_args']) + [encoded])


def decode_command(ffmpeg, encoded, decoded):
    """浮点解码（不削波、不加增益），同 T-085。"""
    return [ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', encoded, '-c:a', 'pcm_f32le', decoded]


def decoded_true_peak_dbtp(path):
    """读一个（解码后的）wav，返回 8 倍真峰值 dBTP。空文件或含非有限样本抛 ValueError。"""
    audio, rate = sf.read(path, always_2d=True, dtype='float64')
    if len(audio) == 0 or not np.all(np.isfinite(audio)):
        raise ValueError('解码结果为空或含非有限样本')
    fields, _ = true_peak_scan(audio, rate)
    return fields['tp8_dbtp']


def roundtrip_true_peak(ffmpeg, source, key, work):
    """成品 -> 编码 -> 浮点解码 -> 8 倍真峰值。编码 / 解码失败抛 CalledProcessError。中间文件当场删。"""
    encoded = Path(work) / f"{Path(source).stem}__{key}.{CODEC_PRESETS[key]['extension']}"
    decoded = encoded.with_name(encoded.name + '.wav')
    try:
        run(encode_command(ffmpeg, source, key, encoded))
        run(decode_command(ffmpeg, encoded, decoded))
        return decoded_true_peak_dbtp(decoded)
    finally:
        encoded.unlink(missing_ok=True)
        decoded.unlink(missing_ok=True)


def _short_error(error):
    """异常 -> 一行短原因（ffmpeg 失败时取它输出的最后一段）。"""
    if isinstance(error, subprocess.CalledProcessError) and error.output:
        return 'ffmpeg 失败：' + ' '.join(str(error.output).split())[-200:]
    return f'{type(error).__name__}: {error}'


# ---------------------------------------------------------------- 一个成品文件 -> 一行（量，不判）

def blank_delivery_row():
    """所有列都在：文字列空串，数值列 NaN（写 CSV 时是空格子，绝不填 0）。"""
    return {name: ('' if unit == 'text' else math.nan) for name, unit, _meaning in DELIVERY_FIELDS}


def build_profile(delivery, target_lufs=None, ceiling=None):
    """交付档参数。流媒体的真峰值上限固定 -2.0，不接受 --ceiling；俱乐部上限 = --ceiling（默认 -1.0）。"""
    if delivery == 'streaming':
        if ceiling is not None:
            raise InfrastructureError('--ceiling 只用于俱乐部档；流媒体档的上限固定为 -2.0 dBTP')
        limit = STREAMING_TP_LIMIT_DBTP
    elif delivery == 'club':
        limit = CLUB_DEFAULT_CEILING_DBTP if ceiling is None else float(ceiling)
    else:
        raise InfrastructureError(f'未知交付档：{delivery}')
    target = DELIVERY_TARGET_LUFS[delivery] if target_lufs is None else float(target_lufs)
    return {'delivery': delivery, 'target_lufs': target, 'tp_limit_dbtp': limit}


def check_master_file(path, profile, annotation=None, codec_keys=(), roundtrip=None, apple_dir=None):
    """量一个成品文件。返回 {'row': 行, 'read_error': 读不了的原因或空串, 'codec_failures': {预设: 失败原因}}。
    不判定（判定在 judge_master）。文件读不了不抛异常：记 read_error，由判定给 FAIL。
    codec_keys 是要回测的预设（调用者已剔除 ffmpeg 里没有的）；roundtrip(源文件, 预设) 返回解码后 tp8，失败抛异常；
    apple_dir 给了就按同名找 afconvert 解回的 wav 量 Apple AAC 参考列，缺了只写备注，不判。"""
    path = Path(path)
    row = blank_delivery_row()
    row.update(file=path.name, delivery=profile['delivery'], target_lufs=profile['target_lufs'],
               tp_limit_dbtp=profile['tp_limit_dbtp'])
    result = {'row': row, 'read_error': '', 'codec_failures': {}}
    try:
        measured = measure_master(path, profile['target_lufs'], profile['tp_limit_dbtp'])
        audio, rate = sf.read(path, always_2d=True, dtype='float64')
    except Exception as error:   # 文件读不了：记原因，继续检查下一个文件
        result['read_error'] = _short_error(error)
        return result
    row.update(measured)
    if row['nonfinite_sample_count']:
        return result   # measure_master 已把测量项留空并写了 na_reason；含非有限样本的文件不再做回测
    reasons = [row['na_reason']] if row['na_reason'] else []
    notes = []
    row['clipped_sample_count'] = clipped_sample_count(audio, row['subtype'])
    row['tp_margin_db'] = profile['tp_limit_dbtp'] - row['tp8_dbtp']

    main_audio, basis = None, ''
    if audio.shape[1] == 2 and len(audio) >= 3 * rate:
        short, peaks_db = windowed_short_term_and_peak_db(audio, rate)
        hop = _hop_samples(rate)
        spans = None
        if annotation is not None:
            try:
                spans = drop_spans_seconds(annotation)
            except ValueError as error:
                notes.append(f'{error}；主段改按最响窗口')
        mask, basis, main_note = select_main_windows(short, hop / rate, PSR_WINDOW_HOPS * hop / rate, spans)
        if main_note:
            (notes if basis else reasons).append(main_note)
        row['main_basis'] = basis
        if basis:
            row['main_s_lufs'], row['main_psr_p10_db'], row['main_psr_median_db'] = main_section_stats(short, peaks_db, mask)
            sample_spans = merged_sample_spans(mask, hop, PSR_WINDOW_HOPS * hop, len(audio))
            row['main_seconds'] = sum(end - start for start, end in sample_spans) / rate
            main_audio = np.concatenate([audio[start:end] for start, end in sample_spans])
    if main_audio is not None:
        row['spectral_slope_db_per_oct'] = spectral_slope_db_per_oct(main_audio, rate)
        if math.isnan(row['spectral_slope_db_per_oct']):
            reasons.append(f'谱斜率：主段里高于最响段 -{SLOPE_BAND_GATE_DB:g} dB 的 1/3 倍频程段不足 {SLOPE_MIN_BANDS} 个')
    # 目标核对：有 drop 标注就用 drop 段的 S（规格第 4.2 节：目标是 drop 响度），否则退回整曲 I
    if basis == 'drop':
        row['target_basis'], row['target_check_lufs'] = 'drop', row['main_s_lufs']
    else:
        row['target_basis'], row['target_check_lufs'] = 'whole', row['lufs_i']
    row['target_delta_lu'] = row['target_check_lufs'] - profile['target_lufs']

    for key in codec_keys:
        try:
            tp = roundtrip(path, key)
        except Exception as error:   # 单个文件编码失败：记原因，该格留空
            result['codec_failures'][key] = _short_error(error)
            continue
        tp_column, over_column = codec_column_names(key)
        row[tp_column], row[over_column] = tp, tp - row['tp8_dbtp']
    for key, reason in result['codec_failures'].items():
        notes.append(f'{CODEC_LABELS[key]} 编码回测失败：{reason}')

    if apple_dir is not None:
        candidate = Path(apple_dir) / path.name
        if not candidate.is_file():
            notes.append(f'Apple AAC 未测：{Path(apple_dir).name}/ 里没有同名文件')
        else:
            try:
                tp = decoded_true_peak_dbtp(candidate)
            except Exception as error:
                notes.append(f'Apple AAC 未测：{_short_error(error)}')
            else:
                tp_column, over_column = codec_column_names(APPLE_AAC_KEY)
                row[tp_column], row[over_column] = tp, tp - row['tp8_dbtp']
    row['na_reason'] = '; '.join(reasons)
    row['notes'] = '; '.join(notes)
    return result


# ---------------------------------------------------------------- 判定（纯函数）

def _num(value):
    """任何东西 -> float；转不了读 NaN。NaN 参与比较一律为 False，所以算不出的项自动不触发。"""
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def judge_master(row, profile, read_error='', missing_codecs=(), codec_failures=None):
    """一首的判定。返回 (fail 原因列表, info 提示列表)。FAIL 只有四项，其余一律是 info；
    算不出的项（NaN）不触发。missing_codecs = 整列没测出来的预设（调用者算）；codec_failures = 单个文件的编码失败。"""
    fails, infos = [], []
    delivery, limit = profile['delivery'], profile['tp_limit_dbtp']
    # ---- FAIL：四项 ----
    if read_error:
        fails.append(f'文件读不了：{read_error}')
    nonfinite = _num(row.get('nonfinite_sample_count'))
    if nonfinite > 0:
        fails.append(f'含 {int(nonfinite)} 个非有限样本（NaN / Inf）')
    tp = _num(row.get('tp8_dbtp'))
    if tp > limit + TP_LIMIT_TOLERANCE_DB:
        fails.append(f'PCM 真峰值 {tp:+.2f} dBTP 高于上限 {limit:+.1f} dBTP（容差 +{TP_LIMIT_TOLERANCE_DB:g} dB）')
    clipped = _num(row.get('clipped_sample_count'))
    if clipped > 0:
        fails.append(f'原采样点削波 {int(clipped)} 个')
    for key in missing_codecs:
        fails.append(f'必需编码器缺失或整列没测出来：{CODEC_LABELS[key]}')
    # ---- INFO：只报不判 ----
    if not read_error and nonfinite == 0 and math.isnan(tp):
        infos.append('PCM 真峰值未测（见 na_reason）')
    for key in CODEC_COLUMN_ORDER:
        decoded_tp = _num(row.get(codec_column_names(key)[0]))
        if decoded_tp > CODEC_TP_WARN_DBTP:
            over = _num(row.get(codec_column_names(key)[1]))
            infos.append(f'{CODEC_LABELS[key]} 解码后真峰值 {decoded_tp:+.2f} dBTP（过冲 {over:+.2f} dB，高于 {CODEC_TP_WARN_DBTP:+g}）')
    delta = _num(row.get('target_delta_lu'))
    if abs(delta) > TARGET_WINDOW_LU:
        what = 'drop 段 S' if row.get('target_basis') == 'drop' else '整曲 I'
        infos.append(f"{what} {_num(row.get('target_check_lufs')):.1f} LUFS 距目标 {profile['target_lufs']:.1f} 为 {delta:+.1f} LU"
                     f'（超过 ±{TARGET_WINDOW_LU:g}）')
    p10, median = _num(row.get('main_psr_p10_db')), _num(row.get('main_psr_median_db'))
    if p10 < PSR_P10_FLOOR_DB[delivery]:
        infos.append(f'主段 PSR P10 {p10:.1f} dB 低于 {PSR_P10_FLOOR_DB[delivery]:g}')
    if median < PSR_MEDIAN_FLOOR_DB[delivery]:
        infos.append(f'主段 PSR 中位 {median:.1f} dB 低于 {PSR_MEDIAN_FLOOR_DB[delivery]:g}')
    dc = _num(row.get('dc_offset_dbfs'))
    if dc > DC_WARN_DBFS:
        infos.append(f'直流偏置 {dc:.1f} dBFS 高于 {DC_WARN_DBFS:g}')
    lead, trail = _num(row.get('lead_silence_s')), _num(row.get('trail_silence_s'))
    if lead > LEAD_SILENCE_WARN_S:
        infos.append(f'开头静音 {lead:.2f} s 超过 {LEAD_SILENCE_WARN_S:g}')
    if trail > TRAIL_SILENCE_WARN_S:
        infos.append(f'结尾静音 {trail:.2f} s 超过 {TRAIL_SILENCE_WARN_S:g}')
    rate = _num(row.get('sample_rate'))
    if math.isfinite(rate) and int(rate) not in EXPECTED_SAMPLE_RATES:
        infos.append(f'采样率 {int(rate)} Hz 不是 44.1 / 48 kHz')
    subtype = row.get('subtype') or ''
    if subtype and subtype not in EXPECTED_SUBTYPES:
        infos.append(f"位深 {subtype} 不是 {' / '.join(EXPECTED_SUBTYPES)}")
    loss = _num(row.get('mono_lf_loss_db'))
    if loss < MONO_LF_LOSS_WARN_DB:
        infos.append(f'单声道低频损失 {loss:.1f} dB 低于 {MONO_LF_LOSS_WARN_DB:g}')
    if row.get('monitor_suspicion'):
        infos.append(f"疑似带着临时监听导出：{row['monitor_suspicion']}")
    for key in (codec_failures or {}):
        if key not in missing_codecs:   # 整列都没测出来的已经在 FAIL 里写过，不重复
            infos.append(f'{CODEC_LABELS[key]} 编码回测失败（该首这一列未测）')
    return fails, infos


def apply_judgement(row, profile, read_error='', missing_codecs=(), codec_failures=None):
    """把判定写进行的 fail_reasons / info_flags / verdict 三列并返回 verdict。
    FAIL = 有失败项；INFO = 无失败项但有只报项、或有项目因素材原因算不出（na_reason 非空）；PASS = 其余。"""
    fails, infos = judge_master(row, profile, read_error, missing_codecs, codec_failures)
    row['fail_reasons'] = '; '.join(fails)
    row['info_flags'] = '; '.join(infos)
    row['verdict'] = 'FAIL' if fails else ('INFO' if infos or row['na_reason'] else 'PASS')
    return row['verdict']


def missing_codec_columns(results, requested_keys, available_keys):
    """整列没测出来的预设：ffmpeg 里没有这个编码器，或者每个能测的文件都编码失败。
    「能测的文件」= 读得了且不含非有限样本的文件；一个都没有就不下结论（那些文件本身已经 FAIL）。"""
    eligible = [r for r in results if not r['read_error'] and not _num(r['row']['nonfinite_sample_count']) > 0]
    missing = []
    for key in requested_keys:
        if key not in available_keys or (eligible and all(key in r['codec_failures'] for r in eligible)):
            missing.append(key)
    return missing


def build_ep_row(song_rows, profile):
    """EP 汇总行：各曲主段 S、谱斜率、主段 PSR 中位的极差（R1 G7）。超过上限只报（INFO）；不够 2 首有值的写 na_reason。"""
    row = blank_delivery_row()
    row.update(file=EP_ROW_NAME, delivery=profile['delivery'], target_lufs=profile['target_lufs'],
               tp_limit_dbtp=profile['tp_limit_dbtp'], notes=f'{len(song_rows)} 首参与')

    def spread(name):
        values = [v for v in (_num(r[name]) for r in song_rows) if math.isfinite(v)]
        return max(values) - min(values) if len(values) >= 2 else math.nan

    row['ep_main_s_range_lu'] = spread('main_s_lufs')
    row['ep_slope_range_db_per_oct'] = spread('spectral_slope_db_per_oct')
    row['ep_psr_median_range_db'] = spread('main_psr_median_db')
    infos, empty = [], []
    for name, label, unit, limit in (
            ('ep_main_s_range_lu', '各曲主段 S 极差', 'LU', EP_MAIN_S_RANGE_MAX_LU),
            ('ep_slope_range_db_per_oct', '各曲谱斜率极差', 'dB/oct', EP_SLOPE_RANGE_MAX_DB_PER_OCT),
            ('ep_psr_median_range_db', '各曲主段 PSR 中位极差', 'dB', EP_PSR_MEDIAN_RANGE_MAX_DB)):
        if math.isnan(row[name]):
            empty.append(label)
        elif row[name] > limit:
            infos.append(f'{label} {row[name]:.2f} {unit} 超过 {limit:g}')
    if empty:
        row['na_reason'] = '极差需要至少 2 首有值：' + '、'.join(empty)
    row['info_flags'] = '; '.join(infos)
    row['verdict'] = 'INFO' if infos or row['na_reason'] else 'PASS'
    return row


# ---------------------------------------------------------------- 输出：CSV（真源）与 Markdown（由 CSV 生成）

def _csv_cell(value):
    """CSV 格子：文字原样，整数写整数，NaN 写空，±inf 写 inf / -inf，其余浮点 4 位小数。"""
    if isinstance(value, str):
        return value
    if isinstance(value, (int, np.integer)):
        return str(int(value))
    number = float(value)
    if math.isnan(number):
        return ''
    if math.isinf(number):
        return 'inf' if number > 0 else '-inf'
    return f'{number:.4f}'


def write_delivery_csv(path, rows):
    with Path(path).open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(DELIVERY_FIELD_NAMES)
        for row in rows:
            writer.writerow([_csv_cell(row.get(name, '')) for name in DELIVERY_FIELD_NAMES])


def verdict_rule_lines(delivery, tp_limit):
    """判定表（级别, 条件）：写进 Markdown 的「判定表」一节；数字都取自顶部常量表，不会和代码脱节。"""
    return (
        ('FAIL', f'PCM 真峰值（8 倍过采样）高于 {tp_limit:+.1f} dBTP + {TP_LIMIT_TOLERANCE_DB:g} dB'),
        ('FAIL', '原采样点削波数大于 0（浮点文件：样本绝对值大于 1.0；整数 PCM：样本贴满刻度码值）'),
        ('FAIL', '文件读不了，或含非有限样本（NaN / Inf）'),
        ('FAIL', '必需编码器缺失、或某个编码预设整列都没测出来'),
        ('INFO', f'编码再浮点解码后的真峰值高于 {CODEC_TP_WARN_DBTP:+g} dBTP（含 Apple AAC 参考列；Apple 缺文件只标「未测」）'),
        ('INFO', f'目标核对（有 drop 标注用 drop 段 S，否则用整曲 I）距目标超过 ±{TARGET_WINDOW_LU:g} LU'),
        ('INFO', f'主段 PSR P10 低于 {PSR_P10_FLOOR_DB[delivery]:g} dB，或中位低于 {PSR_MEDIAN_FLOOR_DB[delivery]:g} dB'),
        ('INFO', f'直流偏置高于 {DC_WARN_DBFS:g} dBFS'),
        ('INFO', f'开头静音超过 {LEAD_SILENCE_WARN_S:g} s，或结尾静音超过 {TRAIL_SILENCE_WARN_S:g} s（静音 = 样本都不超过 {SILENCE_THRESHOLD_DBFS:g} dBFS）'),
        ('INFO', '采样率不是 44.1 / 48 kHz，或位深不是 ' + ' / '.join(EXPECTED_SUBTYPES)),
        ('INFO', f'单声道低频损失低于 {MONO_LF_LOSS_WARN_DB:g} dB'),
        ('INFO', f'疑似带着临时监听导出（整曲 S/M 低于 {SIDE_MID_SUSPECT_DB:g} dB；或 I 比目标低 {SUSPECT_LOUDNESS_GAP_LU:g} LU 以上且真峰值比上限低 {SUSPECT_TP_MARGIN_DB:g} dB 以上）'),
        ('INFO', '有项目因素材原因算不出（见 na_reason 列）；LRA 只报数，没有阈值'),
        ('INFO', f'EP 汇总：各曲主段 S 极差超过 {EP_MAIN_S_RANGE_MAX_LU:g} LU、谱斜率极差超过 {EP_SLOPE_RANGE_MAX_DB_PER_OCT:g} dB/oct、'
                 f'主段 PSR 中位极差超过 {EP_PSR_MEDIAN_RANGE_MAX_DB:g} dB'),
    )


def _cell(text, digits=1, signed=False, missing='—'):
    """CSV 里的字符串 -> 表格里的数字文字。空 = missing；inf 写 ±∞。"""
    if text is None or str(text).strip() == '':
        return missing
    try:
        value = float(text)
    except ValueError:
        return str(text)
    if math.isinf(value):
        return '+∞' if value > 0 else '-∞'
    return f'{value:+.{digits}f}' if signed else f'{value:.{digits}f}'


def _escape(text):
    return str(text).replace('|', '\\|')


def _table(header, body):
    lines = ['| ' + ' | '.join(header) + ' |', '|' + '|'.join('---' for _ in header) + '|']
    lines += ['| ' + ' | '.join(cells) + ' |' for cells in body]
    return lines


def render_markdown(rows, codec_keys, show_apple):
    """delivery_check.csv 读回来的行（字符串） -> Markdown 文本。codec_keys 是本次要显示的 ffmpeg 预设列；
    show_apple = 给了 --apple-aac-dir（没给就不占一列）。空格子在编码列写「未测」，在别的列写「—」。"""
    songs = [r for r in rows if r['file'] != EP_ROW_NAME]
    ep = next((r for r in rows if r['file'] == EP_ROW_NAME), None)
    first = songs[0] if songs else rows[0]
    shown = list(codec_keys) + ([APPLE_AAC_KEY] if show_apple else [])
    counts = {level: sum(1 for r in songs if r['verdict'] == level) for level in ('FAIL', 'INFO', 'PASS')}
    delivery_name = {'streaming': '流媒体', 'club': '俱乐部'}.get(first['delivery'], first['delivery'])
    out = ['# 交付检查单', '',
           f"- 交付档：{first['delivery']}（{delivery_name}）；目标 {_cell(first['target_lufs'])} LUFS；"
           f"PCM 真峰值上限 {_cell(first['tp_limit_dbtp'], 1, True)} dBTP（容差 +{TP_LIMIT_TOLERANCE_DB:g} dB）",
           f"- 共 {len(songs)} 首：FAIL {counts['FAIL']} 首、INFO {counts['INFO']} 首、PASS {counts['PASS']} 首"
           '（FAIL = 必须处理；INFO = 只报、标黄，由你决定；PASS = 全部干净）',
           '- 编码回测列：' + ('、'.join(CODEC_LABELS[k] for k in shown) if shown else '本次没有'),
           '- 本表由 delivery_check.csv 生成，CSV 是真源（列更全）。工具只报告，不改任何文件，不自动回退增益。', '']

    def codec_cell(row, key):
        tp, over = (row[c] for c in codec_column_names(key))
        return '未测' if tp == '' else f"{_cell(tp, 2, True)}（{_cell(over, 2, True)}）"

    header = ['文件', '判定', '整曲 I', '主段 S', 'LRA', '谱斜率', 'PSR P10', 'PSR 中位', '真峰值'] + [CODEC_LABELS[k] for k in shown]
    body = []
    for r in songs:
        basis = {'drop': ' drop', 'loudest': ' 最响'}.get(r['main_basis'], '')
        verdict = f"**{r['verdict']}**" if r['verdict'] == 'FAIL' else r['verdict']
        body.append([_escape(r['file']), verdict, _cell(r['lufs_i']), _cell(r['main_s_lufs']) + (basis if r['main_s_lufs'] else ''),
                     _cell(r['lra_lu']), _cell(r['spectral_slope_db_per_oct'], 2, True), _cell(r['main_psr_p10_db']),
                     _cell(r['main_psr_median_db']), _cell(r['tp8_dbtp'], 2, True)] + [codec_cell(r, k) for k in shown])
    if ep is not None:
        body.append(['**EP 汇总（极差）**', ep['verdict'], '—', _cell(ep['ep_main_s_range_lu'], 2) + ' LU' if ep['ep_main_s_range_lu'] else '—',
                     '—', _cell(ep['ep_slope_range_db_per_oct'], 2) + ' dB/oct' if ep['ep_slope_range_db_per_oct'] else '—',
                     '—', _cell(ep['ep_psr_median_range_db'], 2) + ' dB' if ep['ep_psr_median_range_db'] else '—', '—'] + ['—'] * len(shown))
    out += ['## 逐首核对', '', '单位：I / S 为 LUFS；LRA 为 LU；谱斜率 dB/oct；PSR dB；真峰值与编码列 dBTP（括号内为解码后相对成品的过冲 dB）。'
            '主段 S 后的「drop / 最响」是主段的依据；PSR 取主段。', ''] + _table(header, body) + ['']

    flagged = [r for r in rows if r['verdict'] != 'PASS']
    if flagged:
        out += ['## 判定详情', '']
        for r in flagged:
            parts = [f"{label}：{r[column]}" for label, column in
                     (('失败', 'fail_reasons'), ('提示', 'info_flags'), ('未算出', 'na_reason'), ('备注', 'notes')) if r[column]]
            out.append(f"- **{_escape(r['file'])}**（{r['verdict']}）" + (' — ' + '；'.join(parts) if parts else ''))
        out.append('')

    header_b = ['文件', '采样率', '位深', '声道', '时长 s', 'DC dBFS', '开头静音 s', '结尾静音 s', '单声道低频损失 dB', 'S/M dB', '削波数', '目标核对']
    body_b = []
    for r in songs:
        target_text = '—' if r['target_delta_lu'] == '' else (
            f"{'drop 段 S' if r['target_basis'] == 'drop' else '整曲 I'} {_cell(r['target_check_lufs'])}（{_cell(r['target_delta_lu'], 1, True)} LU）")
        body_b.append([_escape(r['file']), _cell(r['sample_rate'], 0), r['subtype'] or '—', _cell(r['channels'], 0),
                       _cell(r['duration_s']), _cell(r['dc_offset_dbfs']), _cell(r['lead_silence_s'], 2), _cell(r['trail_silence_s'], 2),
                       _cell(r['mono_lf_loss_db']), _cell(r['side_mid_db']), _cell(r['clipped_sample_count'], 0), target_text])
    out += ['## 文件与其他项', ''] + _table(header_b, body_b) + ['']

    out += ['## 判定表', '', '判失败只有四项，其余一律只报。', ''] + _table(
        ['级别', '条件'], [[level, text] for level, text in verdict_rule_lines(first['delivery'], _num(first['tp_limit_dbtp']))]) + ['']
    out += ['## 说明', '',
            '- 主段：有段落标注（house_gate 同款 JSON）时取标注里的 drop 段（第 1 小节从 0 s 起）；没有标注或没有 drop 段时取短期响度不低于 P95 - 3 LU 的 3 s 窗。',
            '- 谱斜率：主段的 27 个 1/3 倍频程段电平对 log2 频率的直线斜率，粉红噪声为 0；受母带低通影响。',
            '- 编码列：成品经 ffmpeg 编码再浮点解码（不削波）后的 8 倍真峰值；「未测」= 这一列没有数（编码器缺失、编码失败，或 Apple 参考没给）。',
            '- Apple AAC 参考列由 Mac 上 afconvert 编解码后拷回的 wav 量出，只作参考，缺了不判。',
            '- 若交付采样率不同于工程采样率，抖动请放在宿主的最后一步；本检查单不检查抖动。', '']
    return '\n'.join(out)


# ---------------------------------------------------------------- 命令行

def parse_codec_list(text):
    """--codecs 的值 -> 预设名列表（保持顺序、去重）。all = 四个 ffmpeg 预设；none = 不做编码回测。"""
    names = [name.strip() for name in text.split(',') if name.strip()]
    if not names or names == ['none']:
        return []
    if names == ['all']:
        return list(CODEC_PRESETS)
    unknown = [name for name in names if name not in CODEC_PRESETS]
    if unknown:
        raise InfrastructureError(f"--codecs 里有未知预设：{', '.join(unknown)}（可选 {', '.join(CODEC_PRESETS)}，或 all / none）")
    return list(dict.fromkeys(names))


def collect_inputs(paths):
    """--in 的文件或目录 -> 文件列表。目录只取第一层的 *.wav，跳过 . 开头的（macOS 的 ._ 资源叉）。文件名不许重复。"""
    files = []
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            files += sorted(p for p in path.iterdir() if p.is_file() and p.suffix.lower() == '.wav' and not p.name.startswith('.'))
        elif path.is_file():
            files.append(path)
        else:
            raise InfrastructureError(f'输入不存在：{path}')
    if not files:
        raise InfrastructureError('--in 里没有找到 wav 文件')
    names = [p.name for p in files]
    duplicated = sorted({name for name in names if names.count(name) > 1})
    if duplicated:
        raise InfrastructureError(f"输入里有重名文件（Apple 参考按文件名匹配，必须唯一）：{', '.join(duplicated)}")
    return files


def load_annotation_table(paths):
    """--annotations -> {歌名: 标注}，格式与解析同 house_gate.load_annotations。读不了属于基础设施错。"""
    if not paths:
        return {}
    for path in paths:
        if not Path(path).exists():
            raise InfrastructureError(f'--annotations 不存在：{path}')
    sys.path.insert(0, str(Path(__file__).resolve().parent))   # house_gate.py 与本文件同目录
    from house_gate import load_annotations
    try:
        return load_annotations([str(p) for p in paths])
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise InfrastructureError(f'标注读不了：{type(error).__name__} {error}') from error


def sha256_of(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def threshold_snapshot():
    """顶部常量表的当前取值，写进 environment.json，事后能知道这次是按什么线判的。"""
    names = ('STREAMING_TP_LIMIT_DBTP', 'CLUB_DEFAULT_CEILING_DBTP', 'TP_LIMIT_TOLERANCE_DB', 'DELIVERY_TARGET_LUFS', 'TARGET_WINDOW_LU',
             'CODEC_TP_WARN_DBTP', 'PSR_P10_FLOOR_DB', 'PSR_MEDIAN_FLOOR_DB', 'DC_WARN_DBFS', 'LEAD_SILENCE_WARN_S',
             'TRAIL_SILENCE_WARN_S', 'MONO_LF_LOSS_WARN_DB', 'EXPECTED_SAMPLE_RATES', 'EXPECTED_SUBTYPES', 'EP_MAIN_S_RANGE_MAX_LU',
             'EP_SLOPE_RANGE_MAX_DB_PER_OCT', 'EP_PSR_MEDIAN_RANGE_MAX_DB', 'MAIN_LOUDEST_PERCENTILE', 'MAIN_LOUDEST_BELOW_LU',
             'SLOPE_NPERSEG', 'SLOPE_BAND_GATE_DB', 'SLOPE_MIN_BANDS', 'SILENCE_THRESHOLD_DBFS', 'SIDE_MID_SUSPECT_DB',
             'SUSPECT_LOUDNESS_GAP_LU', 'SUSPECT_TP_MARGIN_DB')
    return {name: globals()[name] for name in names}


def build_master_check_parser():
    parser = argparse.ArgumentParser(
        prog='codec_tp_check.py master-check',
        description='交付检查单：直接量成品 wav（不渲染），逐首出判定、编码后真峰值回测，多首时出 EP 汇总行。'
                    '只报告，不改任何输入文件，不自动回退增益。',
        epilog='退出码：0 = 没有 FAIL；1 = 至少一首 FAIL；2 = 基础设施错（参数 / 路径 / ffmpeg 不可用 / 标注读不了 / 输出写不了）。'
               '输出：--out 目录下 delivery_check.csv（真源）、delivery_check.md（由 CSV 生成）、environment.json。')
    parser.add_argument('--in', dest='input', type=Path, nargs='+', required=True,
                        help='成品 wav 文件或目录（目录只取第一层的 *.wav），可给多个')
    parser.add_argument('--delivery', choices=('streaming', 'club'), required=True,
                        help='交付档；流媒体 PCM 真峰值上限固定 -2.0 dBTP，俱乐部取 --ceiling')
    parser.add_argument('--target-lufs', type=float, default=None,
                        help='目标响度；不给取档基准（流媒体 -10、俱乐部 -7），风格偏移自己填')
    parser.add_argument('--ceiling', type=float, default=None, help='俱乐部档的 Ceiling（dBTP），默认 -1.0；流媒体档不接受')
    parser.add_argument('--annotations', type=Path, nargs='+', default=[],
                        help='段落标注 JSON 文件或目录（格式同 house_gate.py，按歌名 = 文件名去扩展名对上）；有 drop 段就用它定主段')
    parser.add_argument('--apple-aac-dir', type=Path, default=None,
                        help='afconvert_roundtrip.sh 解回的 wav 目录，按同名文件匹配；给了就多一列 Apple AAC 参考')
    parser.add_argument('--codecs', default='all',
                        help=f"编码回测预设，逗号分隔：{', '.join(CODEC_PRESETS)}；all = 全部，none = 不做编码回测（默认 all）")
    parser.add_argument('--out', type=Path, required=True, help='输出目录（不存在会建）')
    parser.add_argument('--ffmpeg', default='/usr/bin/ffmpeg')
    parser.add_argument('--work', type=Path, default=None, help='编码回测的临时目录（默认系统临时目录；中间文件当场删）')
    parser.add_argument('--dry-run', action='store_true', help='只打印将要做什么（输入、标注匹配、预设、输出文件），不量、不编码、不写文件')
    return parser


def print_dry_run(args, profile, files, annotations, codec_keys, attempt_keys):
    print('[dry-run] 不量、不编码、不写任何文件')
    print(f"[dry-run] 交付档 {profile['delivery']}；目标 {profile['target_lufs']:+.1f} LUFS；PCM 真峰值上限 {profile['tp_limit_dbtp']:+.1f} dBTP")
    print(f'[dry-run] 输入 {len(files)} 个文件：')
    for path in files:
        annotation = find_annotation(annotations, path.stem)
        apple = ''
        if args.apple_aac_dir is not None:
            apple = '；Apple 参考 ' + ('有' if (args.apple_aac_dir / path.name).is_file() else '缺')
        print(f"[dry-run]   {path}  标注 {annotation['song'] if annotation else '无（主段按最响窗口）'}{apple}")
    for key in codec_keys:
        state = '可用' if key in attempt_keys else '缺编码器（该列整列未测，会判 FAIL）'
        print(f"[dry-run] 预设 {key}（{CODEC_PRESETS[key]['encoder']}）：{state}")
    if attempt_keys and files:
        encoded, decoded = Path('<work>') / f'{files[0].stem}__{attempt_keys[0]}', Path('<work>') / 'decoded.wav'
        print('[dry-run] 首个预设的命令示例：')
        print('[dry-run]   ' + ' '.join(str(x) for x in encode_command(args.ffmpeg, files[0], attempt_keys[0], encoded)))
        print('[dry-run]   ' + ' '.join(str(x) for x in decode_command(args.ffmpeg, encoded, decoded)))
    for name in (CSV_NAME, MD_NAME, ENVIRONMENT_NAME):
        print(f'[dry-run] 将写 {args.out / name}')
    return 0


def environment_record(args, argv, profile, files, codec_keys, encoders, ffmpeg_version, rows):
    """environment.json 的内容：ffmpeg 版本、包版本、脚本 sha256、命令，外加输入哈希、阈值快照、判定计数。"""
    return {
        'tool': 'codec_tp_check.py master-check',
        'command': [str(x) for x in ['codec_tp_check.py', 'master-check'] + list(argv)],
        'script_sha256': sha256_of(__file__),
        'python': sys.version,
        'platform': platform.platform(),
        'packages': {**{name: __import__(name).__version__ for name in ('numpy', 'scipy', 'soundfile')},
                     'pyloudnorm': importlib_metadata.version('pyloudnorm')},
        'ffmpeg': str(args.ffmpeg) if codec_keys else None,
        'ffmpeg_version': ffmpeg_version,
        'encoders_available': {key: CODEC_PRESETS[key]['encoder'] in encoders for key in codec_keys},
        'codecs_requested': codec_keys,
        'encode_options': 'ffmpeg -i MASTER -map 0:a:0 -c:a ENCODER -b:a Nk [extra]; Opus: -application audio -vbr on; '
                          'native AAC PNS off: -aac_pns 0',
        'decode_options': 'ffmpeg -i ENCODED -c:a pcm_f32le DECODED.wav (no gain or limiter)',
        'profile': profile,
        'thresholds': threshold_snapshot(),
        'annotations': [str(p) for p in args.annotations],
        'apple_aac_dir': str(args.apple_aac_dir) if args.apple_aac_dir is not None else None,
        'inputs': [{'file': p.name, 'bytes': p.stat().st_size, 'sha256': sha256_of(p)} for p in files],
        'verdict_counts': {level: sum(1 for r in rows if r['file'] != EP_ROW_NAME and r['verdict'] == level)
                           for level in ('FAIL', 'INFO', 'PASS')},
    }


def run_master_check(args, argv):
    profile = build_profile(args.delivery, args.target_lufs, args.ceiling)
    codec_keys = parse_codec_list(args.codecs)
    files = collect_inputs(args.input)
    annotations = load_annotation_table(args.annotations)
    if args.apple_aac_dir is not None and not args.apple_aac_dir.is_dir():
        raise InfrastructureError(f'--apple-aac-dir 不是目录：{args.apple_aac_dir}')
    ffmpeg_version, encoders = None, set()
    if codec_keys:
        ffmpeg_version, encoders = probe_ffmpeg(args.ffmpeg)
    attempt_keys = [key for key in codec_keys if CODEC_PRESETS[key]['encoder'] in encoders]
    if args.dry_run:
        return print_dry_run(args, profile, files, annotations, codec_keys, attempt_keys)
    try:
        args.out.mkdir(parents=True, exist_ok=True)
        if args.work is not None:
            args.work.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise InfrastructureError(f'目录建不了：{error}') from error

    results = []
    with tempfile.TemporaryDirectory(prefix='delivery_check_', dir=args.work) as work:
        def roundtrip(source, key):
            return roundtrip_true_peak(args.ffmpeg, source, key, work)

        for index, path in enumerate(files, 1):
            print(f'[{index}/{len(files)}] {path.name}', flush=True)
            results.append(check_master_file(path, profile, find_annotation(annotations, path.stem), attempt_keys, roundtrip,
                                             args.apple_aac_dir))

    missing = missing_codec_columns(results, codec_keys, attempt_keys)
    rows = []
    for result in results:
        apply_judgement(result['row'], profile, result['read_error'], missing, result['codec_failures'])
        rows.append(result['row'])
    if len(rows) >= 2:
        rows.append(build_ep_row(list(rows), profile))
    songs = [r for r in rows if r['file'] != EP_ROW_NAME]

    try:
        write_delivery_csv(args.out / CSV_NAME, rows)
        with (args.out / CSV_NAME).open(newline='', encoding='utf-8') as stream:
            csv_rows = list(csv.DictReader(stream))
        (args.out / MD_NAME).write_text(render_markdown(csv_rows, codec_keys, args.apple_aac_dir is not None), encoding='utf-8')
        record = environment_record(args, argv, profile, files, codec_keys, encoders, ffmpeg_version, rows)
        (args.out / ENVIRONMENT_NAME).write_text(json.dumps(record, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    except OSError as error:
        raise InfrastructureError(f'输出写不了：{error}') from error

    for row in songs:
        detail = '; '.join(part for part in (row['fail_reasons'], row['info_flags'], row['na_reason']) if part)
        print(f"{row['verdict']:4} {row['file']}" + (f'  {detail}' if detail else ''))
    if codec_keys and missing:
        print('整列没测出来：' + '、'.join(CODEC_LABELS[k] for k in missing))
    failed = sum(1 for r in songs if r['verdict'] == 'FAIL')
    print(f"共 {len(songs)} 首：FAIL {failed}、INFO {sum(1 for r in songs if r['verdict'] == 'INFO')}、"
          f"PASS {sum(1 for r in songs if r['verdict'] == 'PASS')}；输出 {args.out}")
    return 1 if failed else 0


def master_check_main(argv):
    """master-check 子命令入口。返回退出码：0 无 FAIL / 1 有 FAIL / 2 基础设施错（argparse 的参数错也是 2）。"""
    args = build_master_check_parser().parse_args(argv)
    try:
        return run_master_check(args, argv)
    except InfrastructureError as error:
        print(f'错误（基础设施）：{error}', file=sys.stderr)
        return 2
    except Exception:   # 意料之外的异常也按基础设施错处理，并留下完整堆栈便于排查
        traceback.print_exc()
        return 2


def main():
    # 子命令 master-check（交付检查单）单独走自己的解析器；不带子命令就是 T-085 的默认命令，下面一行没动。
    if sys.argv[1:2] == ['master-check']:
        return master_check_main(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--ffmpeg', default='/usr/bin/ffmpeg')
    parser.add_argument('--targets', default='-10,-12,-14')
    parser.add_argument('--jamendo-per-class', type=int, default=0, help='download first N tracks per T-038 category (0..15)')
    parser.add_argument('--synthetic', action='store_true', help='add fixed supplementary stress WAVs to input')
    parser.add_argument('--resume-measurements', action='store_true', help='resume complete CSV conditions after validating inputs/binary')
    parser.add_argument('--reuse-renders', action='store_true', help='reuse existing WAVs; only if settings/source unchanged')
    parser.add_argument('--aac-pns', choices=('default', 'on', 'off'), default='default',
                        help='native AAC PNS override; default leaves ffmpeg options unchanged')
    parser.add_argument('--codecs', choices=('all', 'aac'), default='all',
                        help='select AAC only for supplementary measurements')
    parser.add_argument('--aac-encoder', choices=('aac', 'libfdk_aac', 'aac_at'), default='aac')
    args = parser.parse_args()
    if args.aac_encoder != 'aac' and args.aac_pns != 'default':
        parser.error('--aac-pns only applies to the native aac encoder')
    selected_codecs = tuple((args.aac_encoder if enc == 'aac' else enc, rate, ext)
                            for enc, rate, ext in CODECS if args.codecs == 'all' or enc == 'aac')
    encoder_options = {'aac_pns': args.aac_pns, 'aac_encoder': args.aac_encoder}
    args.input = args.input.resolve()
    args.work = args.work.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    if not 0 <= args.jamendo_per_class <= 15:
        parser.error('--jamendo-per-class must be 0..15')
    if args.jamendo_per_class:
        download_jamendo(args)
    if args.synthetic:
        synthetic(args.input)
    sources = sorted(args.input.glob('*.wav'))
    if not sources:
        parser.error('--input has no WAV files')
    manifest = [{'file': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                 'frames': sf.info(p).frames, 'sample_rate': sf.info(p).samplerate} for p in sources]
    if args.resume_measurements and (args.out / 'input_manifest.csv').exists():
        with (args.out / 'input_manifest.csv').open() as stream:
            previous = list(csv.DictReader(stream))
        assert [{k: str(v) for k, v in r.items()} for r in manifest] == previous, 'input cache mismatch'
    write_csv(args.out / 'input_manifest.csv', manifest)
    metadata = {'ffmpeg_version': run([args.ffmpeg, '-version']), 'encoders': selected_codecs, 'encoder_options': encoder_options,
                'encode_options': 'ffmpeg -i MASTER -map 0:a:0 -c:a ENCODER -b:a Nk; Opus: -application audio -vbr on',
                'decode_options': 'ffmpeg -i ENCODED -c:a pcm_f32le DECODED.wav (no gain or limiter)',
                'render_sha256': hashlib.sha256(args.render.read_bytes()).hexdigest(),
                'command': sys.argv, 'render_commands': [],
                'packages': {name: __import__(name).__version__ for name in ('numpy', 'scipy', 'soundfile')},
                'pyloudnorm_version': __import__('importlib.metadata', fromlist=['version']).version('pyloudnorm')}
    metadata['reuse_renders'] = args.reuse_renders
    if args.aac_pns != 'default':
        metadata['encode_options'] += f'; native AAC: -aac_pns {0 if args.aac_pns == "off" else 1}'
    masters, codecs = [], []
    if args.resume_measurements and (args.out / 'environment.json').exists():
        previous = json.loads((args.out / 'environment.json').read_text())
        assert previous['render_sha256'] == metadata['render_sha256'], 'renderer cache mismatch'
        assert previous['ffmpeg_version'] == metadata['ffmpeg_version'], 'ffmpeg cache mismatch'
        assert previous['encoders'] == [list(c) for c in selected_codecs], 'codec cache mismatch'
        assert previous.get('encoder_options', {'aac_pns': 'default', 'aac_encoder': 'aac'}) == encoder_options, 'encoder options cache mismatch'
        metadata['render_commands'] = previous['render_commands']
        for name, records in (('masters.csv', masters), ('codec_roundtrips.csv', codecs)):
            if (args.out / name).exists():
                with (args.out / name).open() as stream:
                    for row in csv.DictReader(stream):
                        records.append({k: v if k in ('file', 'encoder') else float(v) for k, v in row.items()})
    (args.out / 'environment.json').write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + '\n')
    for target in (float(x) for x in args.targets.split(',')):
        output = args.work / f'render_{target:g}'
        command = [args.render.resolve(), '--in', args.input, '--out', output, '--glaze', '100',
                   '--style', 'auto', '--clay', 'auto', '--genre', 'general', '--delivery', 'streaming',
                   '--streaming-target-lufs', str(target), '--no-align', '--no-replay']
        metadata['render_commands'].append([str(x) for x in command])
        if not args.reuse_renders:
            run(command, args.work / f'render_{target:g}.log')
        files = sorted(output.glob('*.wav'))
        if len(files) != len(sources):
            raise ValueError(f'{output}: expected {len(sources)} masters, got {len(files)}')
        for path in files:
            existing = [r for r in codecs if r['file'] == path.name and r['target_lufs'] == target]
            if args.resume_measurements and len(existing) == len(selected_codecs):
                continue
            # Replace any incomplete condition checkpoint.
            masters = [r for r in masters if not (r['file'] == path.name and r['target_lufs'] == target)]
            codecs = [r for r in codecs if not (r['file'] == path.name and r['target_lufs'] == target)]
            print(f'{target:g}: {path.name}', flush=True)
            pre = measure(path)
            key = {'file': path.name, 'target_lufs': target}
            masters.append({**key, **pre})
            for encoder, bitrate, extension in selected_codecs:
                suffix = f'_pns_{args.aac_pns}' if encoder == 'aac' and args.aac_pns != 'default' else ''
                encoded = args.work / f'{path.stem}_{target:g}_{encoder}_{bitrate}{suffix}.{extension}'
                decoded = args.work / (encoded.name + '.wav')
                command = [args.ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', path,
                           '-map', '0:a:0', '-c:a', encoder, '-b:a', f'{bitrate}k']
                if encoder == 'aac' and args.aac_pns != 'default':
                    command += ['-aac_pns', '0' if args.aac_pns == 'off' else '1']
                if encoder == 'libopus':
                    command += ['-application', 'audio', '-vbr', 'on']
                run(command + [encoded])
                run([args.ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', encoded,
                     '-c:a', 'pcm_f32le', decoded])  # float prevents silently clipping decoded peaks.
                post = measure(decoded)
                row = {**key, 'encoder': encoder, 'bitrate_kbps': bitrate,
                       **{'pre_' + k: v for k, v in pre.items()},
                       **{'post_' + k: v for k, v in post.items()},
                       'overshoot4_db': post['tp4_dbtp'] - pre['tp4_dbtp'],
                       'overshoot8_db': post['tp8_dbtp'] - pre['tp8_dbtp']}
                # Requested platform gain-only models, attenuation only; no platform upload.
                for platform, level in (('spotify', -14), ('youtube', -14), ('apple', -16)):
                    gain = min(0, level - post['lufs_i'])
                    row[platform + '_attenuation_db'] = -gain
                    row[platform + '_playback_tp_dbtp'] = post['tp8_dbtp'] + gain
                codecs.append(row)
                decoded.unlink()  # Keep encoded artifacts and unaligned masters locally.
            write_csv(args.out / 'masters.csv', masters)
            write_csv(args.out / 'codec_roundtrips.csv', codecs)
    (args.out / 'environment.json').write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + '\n')


if __name__ == '__main__':
    sys.exit(main())   # 默认命令 main() 返回 None -> 退出码 0（同改前）；master-check 返回 0 / 1 / 2
