#!/usr/bin/env python3
"""T-096a 质量指标管线（ADR-034 B 组最小集）：celadon_render 的输出 + 源文件 → 每首 × 每组设置一行 quality.csv。

口径表：docs/validation/2026-10-11-T096a-metrics/GLOSSARY.md（每个字段只有一个定义，公式与出处写在那里）。

用法：
  quality_metrics.py --renders <输出目录> [<输出目录> …] --sources <源文件目录> [<源文件目录> …] --out quality.csv
                     [--labels-auto labels-auto.tsv [--labels-override labels-override.tsv]] [--jobs 8]
  quality_metrics.py --selftest

输出目录 = 一次 celadon_render 的 --out（成品 wav + metrics.csv + --trace 的 trace/<成品名>.csv）；要用 T-096a 之后的渲染器跑 --trace。
源文件按 metrics.csv 的 source_file 列在 --sources 目录里找（按给的顺序，先找到先用）。

出错（Max 的 Q12）：基础设施错（文件读不到、列缺、行重复、长度对不上）→ 打印原因、不写 --out、退出码 1；
素材原因算不出（没有主段、不够 3 s、没有标注……）→ 空值 + na_reason，绝不填 0。只读输入，只写 --out。
依赖 numpy / scipy / soundfile / pyloudnorm（版本见 docs/validation/2026-10-08-T085-codec-tp/requirements.txt）。
真峰值 / 响度 / PSR / LRA / 低频相关度 / S/M 一律用 codec_tp_check（T-096c 量具库），这里不另写一份。
"""
import argparse
import collections
import concurrent.futures
import contextlib
import csv
import importlib
import math
import os
from pathlib import Path
import sys
import tempfile

import numpy as np
import soundfile as sf
from scipy.fft import rfft
from scipy.signal import butter, get_window, sosfilt, sosfiltfilt

# 同目录的三个脚本，不依赖 cwd；用 importlib 导入，保证上面的 sys.path 设置先于导入生效（同 measure_selfcheck.py）
sys.path.insert(0, str(Path(__file__).resolve().parent))
meters = importlib.import_module('codec_tp_check')            # 量具库（T-096c）
house_gate = importlib.import_module('house_gate')            # 落差 / drop 目标的段落口径（③ / ⑤）
calibrate_house = importlib.import_module('calibrate_house')  # Jamendo 逐小节标签的读法（load_labels、SECTION_KINDS）

# ======== 口径常数：只在这里改（GLOSSARY.md 写的就是这些数）========
MAINNESS_MAIN = 0.5                   # ⑤ 主段：逐块 current_mainness ≥ 它（同插件棘轮与「长时间无主段」的 0.5）
PUMP_BAND_HZ = (2000.0, 8000.0)       # ② 泵感信号法的高频带
PUMP_BAND_ORDER = 4                   # 高频带 Butterworth 带通阶数（单向滤波）
ENVELOPE_FRAME_S = 0.010              # ② 包络帧 10 ms
ENVELOPE_FLOOR_POWER = 1.0e-12        # ② 入口帧均方不高于它（约 −120 dBFS）的帧无效：没有高频时比值没有意义
MODULATION_BAND_HZ = (0.5, 8.0)       # ② 起伏度区（泵感、P_slow 共用）
MODULATION_ORDER = 2                  # 0.5～8 Hz Butterworth 带通阶数（双向、零相位）
GAIN_FAST_CUTOFF_HZ = 20.0            # D_fast：增益里 20 Hz 以上的调制
GAIN_FAST_ORDER = 2                   # D_fast 的 20 Hz 低通阶数（双向）
LF_CREST_CUTOFF_HZ = 120.0            # ③ 低频峰值因数的低通截止
LF_CREST_ORDER = meters.LOW_BAND_ORDER   # ③ 4 阶 Butterworth（单向），同低频相关度的滤波器形式
ATTACK_FRAME_S = 0.005                # ④ 起音检测帧 5 ms
ONSET_REFERENCE_S = 0.050             # ④ 起音判据：比前 50 ms 的能量平均高
ONSET_RISE_DB = 6.0                   # ④ …… 至少 6 dB
ONSET_FLOOR_DBFS = -60.0              # ④ …… 且本帧不低于 −60 dBFS
ONSET_MIN_GAP_S = 0.100               # ④ 两个起音至少隔 100 ms
ATTACK_PEAK_S = 0.005                 # ④ 起音后 5 ms 内的峰值
ATTACK_RMS_S = 0.050                  # ④ 起音后 50 ms 的 RMS
ATTACK_MIN_ONSETS = 20                # ④ 主段起音少于它就不报
SPREAD_HIGH_PERCENTILE = 90           # ⑥ 无标注口径 P90 − P25
SPREAD_LOW_PERCENTILE = 25
SPREAD_MIN_SECONDS = 60.0             # ⑥ 门后两份 S 都要满 60 s
SPREAD_MIN_INPUT_LU = 3.0             # ⑥ 入口 P90 − P25 不足 3 LU = 没有对比可保
SIDE_MID_SEGMENT_S = 2.0              # B7 逐段 S/M 的段长
SIDE_MID_PERCENTILE = 95              # B7 取各段「出口 − 入口」的 P95
SEGMENT_MAIN_FRACTION = 0.5           # B7 一段里主段样本至少占一半才算
SPECTRUM_FFT_S = 0.17                 # B8 FFT 点数 = 不小于 0.17 s 的 2 的幂（48 k 下 8192，40 Hz 段分得开）
SPECTRUM_ANCHOR_HZ = (200.0, 1200.0)  # B8 锚区：中心落在这里的各段 Δ 的均值
SPECTRUM_PERCENTILE = 95              # B8 取 |净改动| 的 P95
BAND_COUNT = 27                       # B8 27 段 1/3 倍频程：中心 1000·2^((b − 14) / 3)，同 BalanceModule.cpp
BAND_REFERENCE_INDEX = 14
BAND_REFERENCE_HZ = 1000.0
PSR_FLOOR_SPEC_DB = {'standard': 8.5, 'streaming': 8.0, 'club': 6.5}   # ① 规格值（设计规格-实时GLAZE-曲风.md:60，原型 A）
PSR_FLOOR_CODE_DB = {'standard': 8.0, 'streaming': 7.0, 'club': 6.0}   # ① 代码现值（PsrGuard.cpp:7-16），只报
FACTORY_CEILING_DBTP = -1.0           # 指标表没有 effective_ceiling_dbtp 时的出厂 Ceiling（同 gate_check.py）
DEFAULT_GENRE, DEFAULT_DELIVERY = 'general', 'standard'   # 没用 --genre / --delivery 时渲染器的默认值
T_SEC_DECIMALS = 4                    # trace 的 t_sec 只留 4 位小数（RenderMain.cpp renderPass）：块起点按整表拟合还原

# metrics.csv 至少要有的列；trace 至少要有的列（后六列是 T-096a 追加的）。
METRICS_REQUIRED = ('kind', 'song', 'source_file', 'sample_rate', 'glaze', 'clay', 'style', 'align_gain_db',
                    'true_peak_dbtp', 'file')
TRACE_REQUIRED = ('t_sec', 'limit_gr_db', 'limit_gain_db', 'current_mainness', 'protection_db', 'guard_cause',
                  'hard_clip_samples', 'limit_diff_energy', 'limit_in_energy')

# quality.csv 的列：(名字, 格式)。格式 = 小数位数，或 'text' / 'int'。含义见 GLOSSARY.md。
QUALITY_FIELDS = (
    ('song', 'text'), ('kind', 'text'), ('glaze', 'text'), ('clay', 'text'), ('style', 'text'),
    ('genre', 'text'), ('delivery', 'text'),
    ('file', 'text'), ('source_file', 'text'), ('sample_rate', 'int'), ('duration_s', 3), ('align_gain_db', 3),
    ('main_s', 1), ('main_pct', 1), ('main_in_drop_pct', 1), ('drop_in_main_pct', 1),
    ('true_peak_dbtp', 3), ('ceiling_dbtp', 3), ('hard_clip_samples', 'int'),
    ('drop_target_dev_lu', 3), ('main_level_err_lu', 3),
    ('drop_loss_lu', 3), ('drop_loss_method', 'text'), ('lra_in_lu', 3), ('lra_out_lu', 3), ('lra_delta_lu', 3),
    ('psr_main_med_in_db', 3), ('psr_main_med_out_db', 3), ('psr_floor_spec_db', 1), ('psr_floor_code_db', 1),
    ('pump_db', 4), ('p_slow_db', 4), ('d_fast_db', 2),
    ('lf_crest_in_db', 3), ('lf_crest_out_db', 3), ('lf_crest_db', 3),
    ('attack_keep_db', 3), ('attack_onsets', 'int'),
    ('limit_diff_db', 3),
    ('low_corr_in', 4), ('low_corr_out', 4), ('low_corr_delta', 4), ('side_mid_delta_p95_db', 3),
    ('net_spectrum_p95_db', 3), ('net_spectrum_bands_db', 'text'),
    ('gain_tv_db_per_min', 3), ('gain_sign_changes_per_min', 3), ('guard_engaged_pct', 2), ('guard_cause_hist', 'text'),
    ('na_reason', 'text'),
)
QUALITY_FIELD_NAMES = tuple(name for name, _ in QUALITY_FIELDS)
PAIR_KEY = ('song', 'kind', 'glaze', 'clay', 'style', 'genre', 'delivery')


class InfraError(Exception):
    """基础设施错：文件读不到、列缺、行重复、长度对不上。整次运行退出码 1。"""


# ======== 小工具 ========

def hop_samples(sr):
    """100 ms 步长的样本数；与 codec_tp_check 的写法相同（max(1, round(sr × HOP_S))）。"""
    return max(1, int(round(sr * meters.HOP_S)))


def to_float(text):
    """CSV 格子 → float；空串、None 或读不成数 → NaN。"""
    try:
        return float(text)
    except (TypeError, ValueError):
        return math.nan


def stereo_like_renderer(audio):
    """照渲染器读歌的规则立体声化：单声道复制成双声道，多声道取前两个（RenderMain.cpp 读歌处的注释）。"""
    if audio.shape[1] == 1:
        return np.repeat(audio, 2, axis=1)
    return audio[:, :2]


def mid_of(audio):
    return 0.5 * (audio[:, 0] + audio[:, 1])


def energy_mean_db(levels_db):
    """一组 dB（或 LUFS）读数的能量平均；空 → NaN。"""
    levels_db = np.asarray(levels_db, dtype=np.float64)
    if len(levels_db) == 0:
        return math.nan
    return float(10.0 * np.log10(np.mean(10.0 ** (levels_db / 10.0))))


def points_in_mask(ends, sample_mask):
    """窗终点（样本，开区间）落在主段里的点：看窗的最后一个样本。"""
    if len(ends) == 0:
        return np.zeros(0, dtype=bool)
    index = np.clip(np.asarray(ends, dtype=np.int64) - 1, 0, len(sample_mask) - 1)
    return sample_mask[index]


def format_value(value, kind):
    """写 CSV：NaN / None → 空；±inf → inf / -inf；小数按位数，-0.000 写成 0.000（基线 diff 才干净）。"""
    if kind == 'text':
        return '' if value is None else str(value)
    if value is None:
        return ''
    if kind == 'int':
        return str(int(value))
    value = float(value)
    if math.isnan(value):
        return ''
    if math.isinf(value):
        return 'inf' if value > 0 else '-inf'
    text = f'{value:.{kind}f}'
    return text[1:] if text.startswith('-') and float(text) == 0.0 else text


# ======== 主段与 trace ========

class Trace:
    """--trace 逐块表里本管线要用的列（numpy 数组）。starts / ends 是成品时间轴的样本号（= 链里位置 − 链延迟）。

    渲染器每块等长（最后一块可能不满），t_sec 只留 4 位小数（48 k 下 ±2.4 个样本），所以块长与链延迟用整张表拟合：
    块长 = 首末行跨度 / 行数，链延迟 = 各行「序号 × 块长 − t_sec × 采样率」的平均；拟合后与每行的差超过舍入范围 = 基础设施错。"""

    def __init__(self, rows, sr):
        self.sr = sr
        t_sec = np.array([to_float(r['t_sec']) for r in rows])
        if len(t_sec) < 2 or not np.all(np.isfinite(t_sec)):
            raise InfraError('trace 少于两行或 t_sec 读不成数')
        positions = t_sec * sr
        index = np.arange(len(t_sec), dtype=np.int64)
        self.block = int(round((positions[-1] - positions[0]) / (len(t_sec) - 1)))
        if self.block <= 0:
            raise InfraError('trace 的 t_sec 不是递增的')
        self.latency = int(round(float(np.mean(index * self.block - positions))))
        self.starts = index * self.block - self.latency
        tolerance = 0.5 * 10.0 ** -T_SEC_DECIMALS * sr + 1.0
        if np.max(np.abs(self.starts - positions)) > tolerance:
            raise InfraError(f'trace 的块不等长（块长 {self.block}、链延迟 {self.latency} 拟合不上 t_sec）')
        if self.latency < 0:
            raise InfraError('trace 首行 t_sec > 0：不是从链里第 0 个样本起的逐块表')
        self.ends = self.starts + self.block
        self.gr_db = np.array([to_float(r['limit_gr_db']) for r in rows])
        self.gain_db = np.array([to_float(r['limit_gain_db']) for r in rows])
        self.mainness = np.array([to_float(r['current_mainness']) for r in rows])
        self.causes = [r['guard_cause'] or '' for r in rows]
        self.hard_clips = [r['hard_clip_samples'] for r in rows]
        self.diff_energy = np.array([to_float(r['limit_diff_energy']) for r in rows])
        self.in_energy = np.array([to_float(r['limit_in_energy']) for r in rows])

    def main_blocks(self):
        return np.isfinite(self.mainness) & (self.mainness >= MAINNESS_MAIN)

    def blocks_in_song(self, n_input):
        """成品时间轴上与原曲 [0, n_input) 有交集的块（去掉开头延迟段与余振）。"""
        return (self.starts < n_input) & (self.ends > 0)


def main_mask_from_trace(trace, n_input):
    """⑤ 主段的样本掩码（输入时间轴）：current_mainness ≥ 0.5 的块覆盖的输入样本 = [链里起点, 链里起点 + 块长)。
    链里起点 = 成品起点 + 链延迟：主段度是分析器读到链入口这一块时给的，对的是这块输入，不是这块输出。"""
    mask = np.zeros(n_input, dtype=bool)
    for start, end, main in zip(trace.starts + trace.latency, trace.ends + trace.latency, trace.main_blocks()):
        if main:
            mask[max(0, start):min(n_input, end)] = True
    return mask


def block_energy(audio, starts, ends):
    """每块（成品时间轴的 [起点, 终点)，截到音频长度）的两声道能量和。"""
    cumulative = np.concatenate(([0.0], np.cumsum(np.sum(audio ** 2, axis=1))))
    lo = np.clip(starts, 0, len(audio))
    hi = np.clip(ends, 0, len(audio))
    return cumulative[hi] - cumulative[lo]


# ======== 每个信号自己的序列（与主段无关，输入只算一次）========

def psr_points(audio, sr):
    """PSR 点（dB）与它们的窗终点（样本），外加整条短期响度序列 S 与窗终点。
    PSR 值来自 codec_tp_check.psr_series_db（只含 S 过 −70 LUFS 门的窗）；窗终点用同一份 S（short_term_loudness_series，同窗长同步长）
    过同一道门对回去——这是为了能按主段挑点，不另算 PSR。"""
    hop = hop_samples(sr)
    window = int(round(sr * meters.SHORT_TERM_WINDOW_S))
    if window != meters.PSR_WINDOW_HOPS * hop:
        raise InfraError(f'采样率 {sr}：3 s 窗与 30 个 100 ms 步对不齐，PSR 点没法对回时间')
    short = meters.short_term_loudness_series(audio, sr)
    short_ends = window + hop * np.arange(len(short), dtype=np.int64)
    if len(short) == 0:
        return np.zeros(0, dtype=np.int64), np.zeros(0), short_ends, short
    psr = meters.psr_series_db(audio, sr)
    kept = np.flatnonzero(short > meters.PSR_GATE_LUFS)
    if len(kept) != len(psr):
        raise InfraError('PSR 序列与短期响度序列的点数对不上')
    return short_ends[kept], psr, short_ends, short


def lf_crest_points(audio, sr):
    """③ 低频峰值因数序列：M 过 4 阶 120 Hz 低通，3 s 窗、100 ms 步长（窗终点与 PSR 同一网格），20·log10(峰 / RMS)；静音窗不算。"""
    hop = hop_samples(sr)
    hops_per_window = meters.PSR_WINDOW_HOPS
    low = sosfilt(butter(LF_CREST_ORDER, LF_CREST_CUTOFF_HZ, 'lowpass', fs=sr, output='sos'), mid_of(audio))
    n_hops = len(low) // hop
    if n_hops < hops_per_window:
        return np.zeros(0, dtype=np.int64), np.zeros(0)
    parts = low[:n_hops * hop].reshape(n_hops, hop)
    peak = np.lib.stride_tricks.sliding_window_view(np.max(np.abs(parts), axis=1), hops_per_window).max(axis=1)
    energy = np.concatenate(([0.0], np.cumsum(np.sum(parts ** 2, axis=1))))
    mean_square = (energy[hops_per_window:] - energy[:-hops_per_window]) / (hops_per_window * hop)
    ends = (np.arange(len(peak), dtype=np.int64) + hops_per_window) * hop
    valid = (mean_square > meters.SILENT_MEAN_SQUARE) & (peak > 0.0)
    crest = 20.0 * np.log10(peak[valid] / np.sqrt(mean_square[valid]))
    return ends[valid], crest


def band_frame_power(audio, sr):
    """② 2～8 kHz 每 10 ms 一帧的两声道平均均方；带的上沿不低于奈奎斯特时返回 None。"""
    if PUMP_BAND_HZ[1] >= sr / 2.0:
        return None
    filtered = sosfilt(butter(PUMP_BAND_ORDER, PUMP_BAND_HZ, 'bandpass', fs=sr, output='sos'), audio, axis=0)
    frame = max(1, int(round(sr * ENVELOPE_FRAME_S)))
    n = len(filtered) // frame
    return np.mean((filtered[:n * frame] ** 2).reshape(n, frame, audio.shape[1]), axis=(1, 2))


def spectrum_fft_size(sr):
    return 1 << int(math.ceil(math.log2(sr * SPECTRUM_FFT_S)))


def band_layout(sr, n_fft):
    """B8：27 段的中心频率、可用标记（上沿不超过奈奎斯特）与 bin 权重矩阵（段 × bin）。
    每个 bin 的功率看成铺满它的频率格 [f − Δ/2, f + Δ/2]，按与段 [下沿, 上沿) 的重叠比例分摊；直流 bin 不参与（同 BalanceModule.cpp）。"""
    bin_hz = sr / n_fft
    freqs = np.arange(n_fft // 2 + 1) * bin_hz
    centres = BAND_REFERENCE_HZ * 2.0 ** ((np.arange(BAND_COUNT) - BAND_REFERENCE_INDEX) / 3.0)
    lows, highs = centres / 2.0 ** (1.0 / 6.0), centres * 2.0 ** (1.0 / 6.0)
    left, right = freqs - bin_hz / 2.0, freqs + bin_hz / 2.0
    overlap = np.minimum(right[None, :], highs[:, None]) - np.maximum(left[None, :], lows[:, None])
    weights = np.clip(overlap, 0.0, None) / bin_hz
    weights[:, 0] = 0.0
    return centres, highs <= sr / 2.0, weights


def band_power_frames(audio, sr):
    """B8：Hann 窗、半帧步长的逐帧 27 段功率（两声道相加）与帧中心样本号。"""
    n_fft = spectrum_fft_size(sr)
    hop = n_fft // 2
    _, _, weights = band_layout(sr, n_fft)
    window = get_window('hann', n_fft)[:, None]
    starts = np.arange(0, len(audio) - n_fft + 1, hop, dtype=np.int64)
    powers = np.zeros((len(starts), BAND_COUNT))
    for i, start in enumerate(starts):
        spectrum = rfft(audio[start:start + n_fft] * window, axis=0)
        powers[i] = weights @ np.sum(np.abs(spectrum) ** 2, axis=1)
    return starts + n_fft // 2, powers


def onset_samples(mono, sr):
    """④ 入口起音：5 ms 一帧的均方（dB）比前 50 ms 的能量平均高 ≥ 6 dB、本身 ≥ −60 dBFS、离上一个起音 ≥ 100 ms；返回帧起点样本号。"""
    frame = max(1, int(round(sr * ATTACK_FRAME_S)))
    n = len(mono) // frame
    reference = int(round(ONSET_REFERENCE_S / ATTACK_FRAME_S))
    if n <= reference:
        return np.zeros(0, dtype=np.int64)
    energy = np.mean((mono[:n * frame] ** 2).reshape(n, frame), axis=1)
    cumulative = np.concatenate(([0.0], np.cumsum(energy)))
    before = (cumulative[reference:n] - cumulative[:n - reference]) / reference   # 第 k 帧（k ≥ reference）之前 reference 帧的平均
    level = 10.0 * np.log10(np.maximum(energy[reference:], 1e-30))
    rise = level - 10.0 * np.log10(np.maximum(before, 1e-30))
    candidates = np.flatnonzero((rise >= ONSET_RISE_DB) & (level >= ONSET_FLOOR_DBFS)) + reference
    gap = max(1, int(round(ONSET_MIN_GAP_S / ATTACK_FRAME_S)))
    onsets, last = [], -gap
    for k in candidates:
        if k - last >= gap:
            onsets.append(k)
            last = k
    return np.array(onsets, dtype=np.int64) * frame


class SignalAnalysis:
    """一个信号（入口或出口，(帧, 2)）与主段无关的全部序列；入口一首歌只算一次，各组设置共用。"""

    def __init__(self, audio, sr):
        self.audio = audio
        self.sr = sr
        self.mid = mid_of(audio)
        self.mid_energy = np.concatenate(([0.0], np.cumsum(self.mid ** 2)))
        self.psr_ends, self.psr, self.short_ends, self.short = psr_points(audio, sr)
        self.lf_ends, self.lf_crest = lf_crest_points(audio, sr)
        self.lra = meters.loudness_range_lu(audio, sr)
        self.hf_power = band_frame_power(audio, sr)
        self.band_centres_at, self.band_power = band_power_frames(audio, sr)
        self.low_corr = meters.low_band_correlation(audio, sr)
        self.onsets = onset_samples(self.mid, sr)


# ======== 两个信号之间的指标（纯函数：给 SignalAnalysis 与主段掩码，返回值与原因）========

def slow_modulation_rms_db(series_db, rate, valid, use):
    """series（dB）减去有效点中位数（无效点填 0 = 中位数）后过 0.5～8 Hz 带通（双向），返回 use 点上的 RMS（dB）。
    常数序列减中位数恰为 0，滤出来也恰为 0。use 全假、带边超出奈奎斯特、序列太短 → NaN。"""
    if not np.any(use) or MODULATION_BAND_HZ[1] >= rate / 2.0:
        return math.nan
    centred = np.where(valid, series_db - np.median(series_db[valid]), 0.0)
    sos = butter(MODULATION_ORDER, MODULATION_BAND_HZ, 'bandpass', fs=rate, output='sos')
    if len(centred) <= 3 * (2 * len(sos) + 1):   # sosfiltfilt 默认补边长度的下限
        return math.nan
    modulated = sosfiltfilt(sos, centred)
    return float(np.sqrt(np.mean(modulated[use] ** 2)))


def frames_in_mask(n_frames, frame, sample_mask):
    """每帧的中心样本是否在主段里。"""
    centres = np.minimum(np.arange(n_frames, dtype=np.int64) * frame + frame // 2, len(sample_mask) - 1)
    return sample_mask[centres]


def pump_db(inp, out, sample_mask):
    """② 泵感（信号法）：逐帧 2～8 kHz 电平比 r = 出口 − 入口（dB）的 0.5～8 Hz 起伏，主段有效帧上的 RMS。返回 (值, 原因)。"""
    if inp.hf_power is None or out.hf_power is None:
        return math.nan, '泵感：采样率太低，2～8 kHz 带超出奈奎斯特'
    n = min(len(inp.hf_power), len(out.hf_power))
    p_in, p_out = inp.hf_power[:n], out.hf_power[:n]
    valid = (p_in > ENVELOPE_FLOOR_POWER) & (p_out > 0.0)
    ratio = np.zeros(n)
    ratio[valid] = 10.0 * np.log10(p_out[valid]) - 10.0 * np.log10(p_in[valid])
    frame = max(1, int(round(inp.sr * ENVELOPE_FRAME_S)))
    use = valid & frames_in_mask(n, frame, sample_mask)
    if not np.any(use):
        return math.nan, '泵感：主段里没有 2～8 kHz 的有效帧'
    value = slow_modulation_rms_db(ratio, 1.0 / ENVELOPE_FRAME_S, valid, use)
    return value, ('' if math.isfinite(value) else '泵感：有效帧太少，带通滤不了')


def p_slow_db(gr_db, block_rate, use):
    """② 排查子表 P_slow：trace 块内最大限幅 GR 取负当 20·log10 g，0.5～8 Hz 起伏在 use 块上的 RMS。"""
    gain_db = -np.asarray(gr_db, dtype=np.float64)
    valid = np.isfinite(gain_db)
    return slow_modulation_rms_db(np.where(valid, gain_db, 0.0), block_rate, valid, use & valid)


def d_fast_db(gr_db, out_block_energy, block_rate, use):
    """排查子表 D_fast（块率近似）：10·log10(Σ E_y·(g_hp / g)² / Σ E_y)，g_hp = g′ − LP20(g′)，g′ = g − 中位(g)。
    常数增益时 g′ 恰为 0 → −∞。块率一半不高于 20 Hz 时 NaN。"""
    if GAIN_FAST_CUTOFF_HZ >= block_rate / 2.0 or not np.any(use):
        return math.nan
    gain = 10.0 ** (-np.asarray(gr_db, dtype=np.float64) / 20.0)
    valid = np.isfinite(gain)
    centred = np.where(valid, gain - np.median(gain[valid]), 0.0)
    sos = butter(GAIN_FAST_ORDER, GAIN_FAST_CUTOFF_HZ, 'lowpass', fs=block_rate, output='sos')
    if len(centred) <= 3 * (2 * len(sos) + 1):
        return math.nan
    fast = centred - sosfiltfilt(sos, centred)
    use = use & valid
    denominator = float(np.sum(out_block_energy[use]))
    if denominator <= 0.0:
        return math.nan
    numerator = float(np.sum(out_block_energy[use] * (fast[use] / gain[use]) ** 2))
    return -math.inf if numerator == 0.0 else 10.0 * math.log10(numerator / denominator)


def masked_median(ends, values, sample_mask):
    chosen = points_in_mask(ends, sample_mask)
    return float(np.median(values[chosen])) if np.any(chosen) else math.nan


def attack_crest_db(analysis, onsets, peak_len, rms_len):
    """④ 每个起音锚点上 20·log10(起点后 peak_len 个样本的峰值 / 起点后 rms_len 个样本的 RMS)；窗内静音为 NaN。"""
    peaks = np.max(np.abs(analysis.mid[onsets[:, None] + np.arange(peak_len)]), axis=1)
    rms = np.sqrt((analysis.mid_energy[onsets + rms_len] - analysis.mid_energy[onsets]) / rms_len)
    with np.errstate(divide='ignore', invalid='ignore'):
        return 20.0 * np.log10(peaks / rms)


def attack_keep_db(inp, out, sample_mask):
    """④ 起音保留：入口起音锚点上 crest 的 (出口 − 入口)，主段起音的中位。返回 (值, 起音数, 原因)。"""
    peak_len = max(1, int(round(inp.sr * ATTACK_PEAK_S)))
    rms_len = max(1, int(round(inp.sr * ATTACK_RMS_S)))
    onsets = inp.onsets[inp.onsets + rms_len <= min(len(inp.mid), len(out.mid))]
    onsets = onsets[sample_mask[onsets]] if len(onsets) else onsets
    if len(onsets) < ATTACK_MIN_ONSETS:
        return math.nan, len(onsets), f'起音保留：主段起音只有 {len(onsets)} 个（少于 {ATTACK_MIN_ONSETS}）'
    differences = attack_crest_db(out, onsets, peak_len, rms_len) - attack_crest_db(inp, onsets, peak_len, rms_len)
    differences = differences[np.isfinite(differences)]
    if len(differences) < ATTACK_MIN_ONSETS:
        return math.nan, len(differences), '起音保留：有效起音不足（窗内静音）'
    return float(np.median(differences)), len(differences), ''


def side_mid_delta_p95_db(inp, out, sample_mask):
    """B7 逐段 S/M：主段 2 s 段上 side_mid_ratio_db 的出口 − 入口，取 P95。两边都是纯中间（或纯侧）的段差值无意义，不算；
    一边 ±inf 的段照留（例如单声道输入被加了宽度 = +inf），所以 P95 取实际存在的值、不插值。返回 (值, 原因)。"""
    segment = max(1, int(round(inp.sr * SIDE_MID_SEGMENT_S)))
    n = min(len(inp.audio), len(out.audio)) // segment
    deltas = []
    for k in range(n):
        part = slice(k * segment, (k + 1) * segment)
        if np.mean(sample_mask[part]) < SEGMENT_MAIN_FRACTION:
            continue
        delta = meters.side_mid_ratio_db(out.audio[part]) - meters.side_mid_ratio_db(inp.audio[part])
        if not math.isnan(delta):
            deltas.append(delta)
    if not deltas:
        return math.nan, 'S/M：主段里没有可比的 2 s 段'
    return float(np.percentile(deltas, SIDE_MID_PERCENTILE, method='higher')), ''


def net_spectrum(inp, out, sample_mask):
    """B8 净频谱改动：主段帧的平均 27 段功率，Δ = 出口 − 入口（dB），减去 200 Hz～1.2 kHz 锚区的均值，取 |净改动| 的 P95。
    返回 (P95, 27 段净改动列表（不可用的段为 NaN）, 原因)。"""
    n = min(len(inp.band_power), len(out.band_power))
    if n == 0:
        return math.nan, [], '净频谱：太短，一帧 FFT 都不够'
    centres_at = inp.band_centres_at[:n]
    use = sample_mask[np.minimum(centres_at, len(sample_mask) - 1)]
    if not np.any(use):
        return math.nan, [], '净频谱：主段里没有完整的 FFT 帧'
    centres, usable, _ = band_layout(inp.sr, spectrum_fft_size(inp.sr))
    p_in = np.mean(inp.band_power[:n][use], axis=0)
    p_out = np.mean(out.band_power[:n][use], axis=0)
    valid = usable & (p_in > 0.0) & (p_out > 0.0)
    delta = np.full(BAND_COUNT, math.nan)
    delta[valid] = 10.0 * np.log10(p_out[valid]) - 10.0 * np.log10(p_in[valid])
    anchor = valid & (centres >= SPECTRUM_ANCHOR_HZ[0]) & (centres <= SPECTRUM_ANCHOR_HZ[1])
    if not np.any(anchor):
        return math.nan, [], '净频谱：锚区（200 Hz～1.2 kHz）没有能量'
    net = delta - np.mean(delta[anchor])
    return float(np.percentile(np.abs(net[valid]), SPECTRUM_PERCENTILE)), list(net), ''


def gated_short_term(short):
    """⑥ 无标注口径用的门后 S：−70 LUFS 绝对门，再「能量平均 − 20 LU」相对门——与 codec_tp_check.loudness_range_lu 同两道门同常数
    （--selftest 拿它的 P95 − P10 与 loudness_range_lu 对账）。"""
    short = short[short > meters.LRA_ABSOLUTE_GATE_LUFS]
    if len(short) == 0:
        return short
    relative = energy_mean_db(short) + meters.LRA_RELATIVE_GATE_LU
    return short[short > relative]


def spread_loss_lu(short_in, short_out):
    """⑥ 无标注口径：(P90 − P25)_入口 − (P90 − P25)_出口（门后整曲 S）。返回 (值, 原因)。"""
    gated_in, gated_out = gated_short_term(short_in), gated_short_term(short_out)
    need = int(round(SPREAD_MIN_SECONDS / meters.HOP_S))
    if len(gated_in) < need or len(gated_out) < need:
        return math.nan, f'落差损失（P90 − P25）：门后不足 {SPREAD_MIN_SECONDS:.0f} s'
    spread_in = float(np.subtract(*np.percentile(gated_in, [SPREAD_HIGH_PERCENTILE, SPREAD_LOW_PERCENTILE])))
    if spread_in < SPREAD_MIN_INPUT_LU:
        return math.nan, f'落差损失（P90 − P25）：入口只有 {spread_in:.2f} LU，没有对比可保'
    spread_out = float(np.subtract(*np.percentile(gated_out, [SPREAD_HIGH_PERCENTILE, SPREAD_LOW_PERCENTILE])))
    return spread_in - spread_out, ''


def compare_signals(inp, out, sample_mask):
    """入口 / 出口两份 SignalAnalysis + 主段掩码 → 只靠音频就能算的那些字段（dict）与 na 原因列表。"""
    values, reasons = {}, []

    def keep(reason):
        if reason:
            reasons.append(reason)

    values['lra_in_lu'], values['lra_out_lu'] = inp.lra, out.lra
    values['lra_delta_lu'] = out.lra - inp.lra
    if math.isnan(values['lra_delta_lu']):
        keep('LRA：没有足够的短期响度值')
    values['psr_main_med_in_db'] = masked_median(inp.psr_ends, inp.psr, sample_mask)
    values['psr_main_med_out_db'] = masked_median(out.psr_ends, out.psr, sample_mask)
    if math.isnan(values['psr_main_med_out_db']):
        keep('PSR：主段里没有过门的 3 s 窗')
    values['pump_db'], reason = pump_db(inp, out, sample_mask)
    keep(reason)
    values['lf_crest_in_db'] = masked_median(inp.lf_ends, inp.lf_crest, sample_mask)
    values['lf_crest_out_db'] = masked_median(out.lf_ends, out.lf_crest, sample_mask)
    values['lf_crest_db'] = values['lf_crest_out_db'] - values['lf_crest_in_db']
    if math.isnan(values['lf_crest_db']):
        keep('低频峰值因数：主段里没有 120 Hz 以下有能量的 3 s 窗')
    values['attack_keep_db'], values['attack_onsets'], reason = attack_keep_db(inp, out, sample_mask)
    keep(reason)
    values['low_corr_in'], values['low_corr_out'] = inp.low_corr, out.low_corr
    values['low_corr_delta'] = out.low_corr - inp.low_corr
    if math.isnan(values['low_corr_delta']):
        keep('低频相关度：125 Hz 以下几乎没有能量')
    values['side_mid_delta_p95_db'], reason = side_mid_delta_p95_db(inp, out, sample_mask)
    keep(reason)
    values['net_spectrum_p95_db'], bands, reason = net_spectrum(inp, out, sample_mask)
    values['net_spectrum_bands_db'] = ';'.join(format_value(v, 2) for v in bands)
    keep(reason)
    values['spread_loss_lu'], values['spread_reason'] = spread_loss_lu(inp.short, out.short)
    return values, reasons


# ======== 段落标注（Jamendo 逐小节标签）========

def sections_from_labels(labels, track):
    """逐小节标签 → 段落列表（house_gate 的段落格式）：相邻同类小节并成一段，transition / exclude 打断。"""
    sections, current, count = [], None, collections.Counter()
    for bar in sorted(b for t, b in labels if t == track):
        kind = labels[(track, bar)]
        if kind not in calibrate_house.SECTION_KINDS:
            current = None
            continue
        if current is not None and current['kind'] == kind and current['end_bar'] == bar - 1:
            current['end_bar'] = bar
            continue
        count[kind] += 1
        current = {'name': f'{kind}{count[kind]}', 'kind': kind, 'start_bar': bar, 'end_bar': bar}
        sections.append(current)
    return sections


def section_drop_loss_lu(in_segments, out_segments, sections):
    """⑥ 有标注口径：每个「breakdown 之后接的第一个 drop」的 (D_in − B_in) − (D_out − B_out)，取最大；配对与段落响度同 house_gate ③。"""
    pairs = []
    for i, section in enumerate(sections):
        if section['kind'] != 'breakdown':
            continue
        following = [s for s in sections[i + 1:] if s['kind'] == 'drop']
        if following:
            pairs.append((section, following[0]))
    if not pairs:
        return math.nan, '落差损失：标注里没有「breakdown 之后接 drop」'
    losses = []
    for breakdown, drop in pairs:
        before = house_gate.section_loudness(in_segments, drop) - house_gate.section_loudness(in_segments, breakdown)
        after = house_gate.section_loudness(out_segments, drop) - house_gate.section_loudness(out_segments, breakdown)
        if math.isfinite(before) and math.isfinite(after):
            losses.append(before - after)
    if not losses:
        return math.nan, '落差损失：breakdown / drop 有一段没有有效子块'
    return max(losses), ''


def drop_target_deviation_lu(row, in_segments, out_segments, drops):
    """B1 drop 目标偏差（同 house_gate ⑤ 的口径）：目标按 GLAZE 折算，方案受预算限制时改用方案实际目标；返回离目标最远的那段（带符号）。"""
    target, note = house_gate.delivery_target(row)
    if not math.isfinite(target):
        return math.nan, f'drop 目标：{note}'
    energy = seconds = 0.0
    for section in drops:   # drop 输入响度：所有 drop 小节合起来（同 house_gate.gate_target）
        for bar in house_gate.bars_of(section):
            segment = in_segments.get(bar)
            if segment is None:
                continue
            lufs, valid = house_gate.num(segment['lufs']), house_gate.num(segment['valid_s'], 0.0)
            if math.isfinite(lufs) and valid > 0.0:
                energy += house_gate.lufs_to_energy(lufs) * valid
                seconds += valid
    drop_in = house_gate.energy_to_lufs(energy / seconds) if seconds > 0.0 else math.nan
    if not math.isfinite(drop_in):
        return math.nan, 'drop 目标：drop 段输入没有有效子块'
    glaze = house_gate.num(row['glaze'], 0.0) / 100.0
    scaled = drop_in + glaze * (target - drop_in)
    effective = house_gate.num(row.get('plan_effective_target_lufs'))
    requested = house_gate.num(row.get('plan_requested_target_lufs'))
    if math.isfinite(effective) and math.isfinite(requested) and effective < requested - house_gate.TARGET_LIMITED_EPSILON_LU:
        scaled = effective
    deviations = [house_gate.section_loudness(out_segments, s) - scaled for s in drops]
    deviations = [d for d in deviations if math.isfinite(d)]
    if not deviations:
        return math.nan, 'drop 目标：drop 段成品没有有效子块'
    return max(deviations, key=abs), ''


def label_agreement(sample_mask, sr, bpm, labels, track):
    """⑤ 交叉核对（10 ms 帧，只算标签表里有的小节）：主段时长里落在 drop 小节的占比、drop 小节时长里被判成主段的占比（%）。
    小节 = 4 拍，缓存已裁到第一个小节线（第 1 小节从 0 s 起）。返回 (主段在 drop 里, drop 被判主段, 原因)。"""
    frame = max(1, int(round(sr * ENVELOPE_FRAME_S)))
    centres = np.arange(frame // 2, len(sample_mask), frame, dtype=np.int64)
    bars = (centres / (4.0 * 60.0 / bpm * sr)).astype(np.int64) + 1
    kinds = [labels.get((track, int(b))) for b in bars]
    known = np.array([k is not None for k in kinds], dtype=bool)
    drop = np.array([k == 'drop' for k in kinds], dtype=bool)
    main = sample_mask[centres]
    main_known = np.count_nonzero(main & known)
    if main_known == 0 or not np.any(drop):
        return math.nan, math.nan, '主段核对：标签里没有 drop 小节，或主段全落在标签表外'
    both = np.count_nonzero(main & drop)
    return 100.0 * both / main_known, 100.0 * both / np.count_nonzero(drop), ''


# ======== 控制质量（trace）========

def control_quality(gain_db, main_blocks, block_seconds):
    """B9：相邻两块都在主段的 ΔGain——总变差与变号次数，各除以主段分钟数。变号只数同一段连续主段之内（隔着非主段不算），ΔGain = 0 的对跳过。"""
    gain_db = np.asarray(gain_db, dtype=np.float64)
    minutes = np.count_nonzero(main_blocks) * block_seconds / 60.0
    if minutes <= 0.0:
        return math.nan, math.nan
    steps = np.diff(gain_db)
    paired = main_blocks[1:] & main_blocks[:-1] & np.isfinite(steps)
    run = np.cumsum(~paired)   # 同一段连续主段里的相邻对共用一个编号
    moving = paired & (steps != 0.0)
    signs, runs = np.sign(steps[moving]), run[moving]
    flips = np.count_nonzero((signs[1:] != signs[:-1]) & (runs[1:] == runs[:-1]))
    return float(np.sum(np.abs(steps[paired]))) / minutes, flips / minutes


def guard_stats(causes):
    """B9（只报）：块里 guard_cause ≠ none 的占比（%）与各原因的占比直方图（「原因:百分比」用分号连接）。"""
    total = len(causes)
    if total == 0:
        return math.nan, ''
    engaged = [c for c in causes if c and c != 'none']
    histogram = ';'.join(f'{cause}:{100.0 * n / total:.1f}' for cause, n in sorted(collections.Counter(engaged).items()))
    return 100.0 * len(engaged) / total, histogram


# ======== 读文件（基础设施错一律 InfraError）========

def read_csv(path):
    try:
        with open(path, encoding='utf-8', newline='') as stream:
            return list(csv.DictReader(stream))
    except OSError as error:
        raise InfraError(f'读不了 {path}：{error}') from error


def require_columns(rows, columns, where):
    if not rows:
        raise InfraError(f'{where} 是空表')
    missing = [c for c in columns if c not in rows[0]]
    if missing:
        raise InfraError(f'{where} 缺列 {", ".join(missing)}')


def read_audio(path):
    try:
        audio, sr = sf.read(str(path), dtype='float64', always_2d=True)
    except (OSError, RuntimeError) as error:   # soundfile 的读错是 RuntimeError 的子类
        raise InfraError(f'读不了音频 {path}：{error}') from error
    return audio, sr


def find_source(sources, name):
    for folder in sources:
        path = Path(folder) / name
        if path.is_file():
            return path
    raise InfraError(f'源文件 {name} 在 --sources 里找不到')


def recover_output(master, align_gain_db, n_input):
    """成品去掉对齐增益、截到原曲长度（成品 = 原曲 + 0.35 s 余振，已去掉链延迟，与原曲逐样本对齐）。"""
    if len(master) < n_input:
        raise InfraError(f'成品 {len(master)} 个样本比原曲 {n_input} 短：没对齐')
    gain = 10.0 ** (-(align_gain_db if math.isfinite(align_gain_db) else 0.0) / 20.0)
    return master[:n_input] * gain


def trace_path_of(directory, file_name):
    """trace/<成品名去掉 .wav>.csv（RenderMain.cpp 的 --trace 写法）。"""
    stem = file_name[:-4] if file_name.endswith('.wav') else file_name
    return Path(directory) / 'trace' / (stem + '.csv')


def load_render_dir(directory):
    """一个输出目录 → (输入行 {歌: 行}, 成品行列表)。重放行（clay-replay-off）是门槛的对照，不进质量表。"""
    rows = read_csv(Path(directory) / 'metrics.csv')
    require_columns(rows, METRICS_REQUIRED, f'{directory}/metrics.csv')
    inputs, renders = {}, []
    for row in rows:
        row['_dir'] = str(directory)
        if row['kind'] == 'input':
            inputs.setdefault(row['song'], row)
        elif row['kind'] == 'render' and row['clay'] != 'replay-off':
            renders.append(row)
    return inputs, renders


def pair_key_of(row):
    return (row['song'], row['kind'], row['glaze'], row['clay'], row['style'],
            row.get('genre') or DEFAULT_GENRE, row.get('delivery') or DEFAULT_DELIVERY)


# ======== 一行质量表 ========

def annotated_drop_metrics(row, input_row, sections):
    """有标注时的 B1 drop 目标与 ⑥ 段落口径落差；要渲染时给了 --bpm --segments-csv（逐段统计按小节分）。
    返回 (drop 目标偏差, 段落口径落差或 None, 原因列表)；缺逐段统计时落差返回 None，调用方改用 P90 − P25。"""
    in_segments = house_gate.load_segments(input_row['_dir'], input_row.get('segments_file')) if input_row else None
    out_segments = house_gate.load_segments(row['_dir'], row.get('segments_file'))
    if in_segments is None or out_segments is None or not house_gate.segments_are_bars(in_segments) \
            or not house_gate.segments_are_bars(out_segments):
        return math.nan, None, ['有标注但缺按小节分的逐段统计（渲染要给 --bpm --segments-csv）：drop 目标未测、落差改用 P90 − P25']
    reasons = []
    drops = [s for s in sections if s['kind'] == 'drop']
    deviation, reason = drop_target_deviation_lu(row, in_segments, out_segments, drops)
    if reason:
        reasons.append(reason)
    loss, reason = section_drop_loss_lu(in_segments, out_segments, sections)
    if reason:
        reasons.append(reason)
    return deviation, loss, reasons


def trace_metrics(trace, y, n_input):
    """trace 侧各项（成品时间轴；只看与原曲有交集的块）。返回 (字段 dict, 原因列表)。"""
    values, reasons = {}, []
    in_song = trace.blocks_in_song(n_input)
    main_blocks = trace.main_blocks() & in_song
    block_rate = trace.sr / trace.block
    values['p_slow_db'] = p_slow_db(trace.gr_db, block_rate, main_blocks)
    values['d_fast_db'] = d_fast_db(trace.gr_db, block_energy(y, trace.starts, trace.ends), block_rate, main_blocks)
    if all(value not in (None, '') for value in trace.hard_clips):
        values['hard_clip_samples'] = sum(int(value) for value in trace.hard_clips)   # A 组：全部块（含开头延迟段与余振）
    else:
        reasons.append('硬钳位 / 限幅差值：限幅器计数未接通（链还没有 getLimiterForTesting）')
    diff, inn = trace.diff_energy[main_blocks], trace.in_energy[main_blocks]
    if np.any(main_blocks) and np.all(np.isfinite(diff)) and np.all(np.isfinite(inn)):
        denominator, numerator = float(np.sum(inn)), float(np.sum(diff))
        if denominator > 0.0:
            values['limit_diff_db'] = -math.inf if numerator == 0.0 else 10.0 * math.log10(numerator / denominator)
        else:
            reasons.append('限幅差值：主段里限幅器入口没有能量')
    values['gain_tv_db_per_min'], values['gain_sign_changes_per_min'] = control_quality(trace.gain_db, main_blocks,
                                                                                        trace.block / trace.sr)
    values['guard_engaged_pct'], values['guard_cause_hist'] = guard_stats([c for c, keep in zip(trace.causes, in_song) if keep])
    return values, reasons


def quality_row(row, input_row, inp, x, sr, labels):
    """一行 metrics.csv（成品）→ 一行 quality.csv（dict）。x / inp 是这首歌的源文件与它的 SignalAnalysis。"""
    directory = Path(row['_dir'])
    if not row['file']:
        raise InfraError(f"{row['song']}：metrics.csv 的 file 列为空（--metrics-only 或写文件失败），没有成品可量")
    master, master_sr = read_audio(directory / row['file'])
    if master_sr != sr or int(to_float(row['sample_rate'])) != sr:
        raise InfraError(f"{row['file']}：采样率 {master_sr} 与源文件 {sr} 不同")
    if master.shape[1] != 2:
        raise InfraError(f"{row['file']}：成品不是双声道")
    align = to_float(row['align_gain_db'])
    y = recover_output(master, align, len(x))
    trace_path = trace_path_of(directory, row['file'])
    trace_rows = read_csv(trace_path)
    require_columns(trace_rows, TRACE_REQUIRED, str(trace_path))
    trace = Trace(trace_rows, sr)

    genre, delivery = row.get('genre') or DEFAULT_GENRE, row.get('delivery') or DEFAULT_DELIVERY
    result = {name: None for name in QUALITY_FIELD_NAMES}
    result.update(song=row['song'], kind=row['kind'], glaze=row['glaze'], clay=row['clay'], style=row['style'],
                  genre=genre, delivery=delivery, file=row['file'], source_file=row['source_file'], sample_rate=sr,
                  duration_s=len(x) / sr, align_gain_db=align)
    reasons = []

    # ⑤ 主段
    mask = main_mask_from_trace(trace, len(x))
    result['main_s'] = np.count_nonzero(mask) / sr
    result['main_pct'] = 100.0 * np.count_nonzero(mask) / len(x)
    if not np.any(mask):
        reasons.append('没有主段（current_mainness 从未 ≥ 0.5）：主段各项未测')

    # A 组真峰值（照抄 metrics.csv，同 gate_check.py）
    result['true_peak_dbtp'] = to_float(row['true_peak_dbtp'])
    ceiling = to_float(row.get('effective_ceiling_dbtp'))
    result['ceiling_dbtp'] = ceiling if math.isfinite(ceiling) else FACTORY_CEILING_DBTP

    # 音频侧各项
    out = SignalAnalysis(y, sr)
    values, audio_reasons = compare_signals(inp, out, mask)
    reasons.extend(audio_reasons)
    result.update({name: value for name, value in values.items() if name in result})
    result['psr_floor_spec_db'] = PSR_FLOOR_SPEC_DB.get(delivery, math.nan)
    result['psr_floor_code_db'] = PSR_FLOOR_CODE_DB.get(delivery, math.nan)

    # B1 热启动主段电平误差（只报）：主段过门 S 的能量平均 − 方案实际目标
    target = to_float(row.get('plan_effective_target_lufs'))
    chosen = points_in_mask(out.short_ends, mask) & (out.short > meters.PSR_GATE_LUFS)
    if math.isfinite(target) and np.any(chosen):
        result['main_level_err_lu'] = energy_mean_db(out.short[chosen]) - target
    elif not math.isfinite(target):
        reasons.append('主段电平误差：指标表没有方案目标（plan_effective_target_lufs）')

    # B1 drop 目标 + ⑥ 落差：有标注走段落口径，否则 P90 − P25
    sections = sections_from_labels(labels, row['song']) if labels else []
    section_loss = None
    if sections:
        result['drop_target_dev_lu'], section_loss, drop_reasons = annotated_drop_metrics(row, input_row, sections)
        reasons.extend(drop_reasons)
        bpm = to_float(row.get('bpm'))
        if math.isfinite(bpm) and bpm > 0.0:
            result['main_in_drop_pct'], result['drop_in_main_pct'], reason = label_agreement(mask, sr, bpm, labels, row['song'])
            if reason:
                reasons.append(reason)
        else:
            reasons.append('主段核对：指标表没有 bpm（渲染要给 --bpm）')
    else:
        reasons.append('drop 目标：没有段落标注')
    if section_loss is not None:
        result['drop_loss_lu'], result['drop_loss_method'] = section_loss, 'sections'
    else:
        result['drop_loss_lu'], result['drop_loss_method'] = values['spread_loss_lu'], 'p90_p25'
        if values['spread_reason']:
            reasons.append(values['spread_reason'])

    # trace 侧各项
    trace_values, trace_reasons = trace_metrics(trace, y, len(x))
    result.update(trace_values)
    reasons.extend(trace_reasons)

    result['na_reason'] = '; '.join(dict.fromkeys(r for r in reasons if r))   # 去重、保序
    return result


def process_source(task):
    """一首歌（一个源文件）的全部成品行；入口分析只做一次。返回 (质量行列表, 基础设施错列表)。"""
    source_path, items, labels = task
    try:
        audio, sr = read_audio(source_path)
        x = stereo_like_renderer(audio)
        inp = SignalAnalysis(x, sr)
        return [quality_row(row, input_row, inp, x, sr, labels) for row, input_row in items], []
    except InfraError as error:
        return [], [f'{source_path}: {error}']


def collect_tasks(render_dirs, sources, labels):
    """各输出目录的成品行按源文件分组（每组只带本组歌的标签）；同一配对键出现两次是基础设施错。"""
    grouped, seen = collections.defaultdict(list), {}
    for directory in render_dirs:
        inputs, renders = load_render_dir(directory)
        for row in renders:
            key = pair_key_of(row)
            if key in seen:
                raise InfraError(f'配对键 {key} 在 {seen[key]} 与 {directory} 里重复')
            seen[key] = directory
            grouped[str(find_source(sources, row['source_file']))].append((row, inputs.get(row['song'])))
    tasks = []
    for path, items in sorted(grouped.items()):
        songs = {row['song'] for row, _ in items}
        tasks.append((path, items, {key: kind for key, kind in labels.items() if key[0] in songs}))
    return tasks


def write_quality_csv(path, rows):
    kinds = dict(QUALITY_FIELDS)
    rows = sorted(rows, key=lambda r: (r['song'], to_float(r['glaze']), r['clay'], r['style'], r['genre'], r['delivery']))
    with open(path, 'w', encoding='utf-8', newline='') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(QUALITY_FIELD_NAMES)
        for row in rows:
            writer.writerow([format_value(row[name], kinds[name]) for name in QUALITY_FIELD_NAMES])


def load_labels(auto_path, override_path):
    """Jamendo 逐小节标签（calibrate_house.load_labels：自动标签叠人工改标，改过的交界前后各 1 小节标 transition）。"""
    if not auto_path:
        return {}
    try:
        return calibrate_house.load_labels(None, override_path, auto_path)
    except (OSError, KeyError, ValueError) as error:
        raise InfraError(f'读不了标签：{error}') from error


def run(args):
    try:
        labels = load_labels(args.labels_auto, args.labels_override)
        tasks = collect_tasks(args.renders, args.sources, labels)
    except InfraError as error:
        print(f'quality_metrics：{error}', file=sys.stderr)
        return 1
    if not tasks:
        print('quality_metrics：输出目录里没有成品行', file=sys.stderr)
        return 1
    if args.jobs <= 1:
        results = [process_source(task) for task in tasks]
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(process_source, tasks))
    rows, errors = [], []
    for task_rows, task_errors in results:
        rows.extend(task_rows)
        errors.extend(task_errors)
    if errors:
        for error in errors:
            print(f'quality_metrics：{error}', file=sys.stderr)
        print(f'quality_metrics：{len(errors)} 首有基础设施错，没写 {args.out}', file=sys.stderr)
        return 1
    write_quality_csv(args.out, rows)
    print(f'quality_metrics：{len(tasks)} 首、{len(rows)} 行 → {args.out}')
    return 0


# ======== 自检（--selftest）：合成信号 + 已知答案 ========

SELFTEST_SR = 48000
SELFTEST_CHECKS = []
NUMERIC_ZERO = 1e-9      # 纯增益时的「数值零」：dB 比值经过浮点缩放，差在 1e-12 量级，留三个数量级余量
FLOAT32_ZERO = 1e-4      # 端到端：成品是 32-bit float 文件，还原后与 0.7 × 入口差在 1e-7 相对量级

# compare_signals 里的差值项（纯增益 / 入口 = 出口时都该是 0）
DIFFERENCE_FIELDS = ('lra_delta_lu', 'pump_db', 'lf_crest_db', 'attack_keep_db', 'low_corr_delta',
                     'side_mid_delta_p95_db', 'net_spectrum_p95_db', 'spread_loss_lu')


def selftest(func):
    SELFTEST_CHECKS.append(func)
    return func


def synthetic_track(seconds=80.0, sr=SELFTEST_SR, seed=96):
    """固定种子的合成 House 片段：10 s 一段，drop 与 breakdown 交替（breakdown 底鼓 / 低音只剩 0.3 倍、铺底与踩镲也收小，约低 10 dB）；
    左右铺底不相关（有侧声道），峰值归一到 0.5。给「纯增益 / 入口 = 出口」用：每个差值项都要有读数。"""
    rng = np.random.default_rng(seed)
    t = np.arange(int(round(seconds * sr))) / sr
    beat = t % 0.5
    kick = np.sin(2 * np.pi * (50 * beat + 0.9 * (1 - np.exp(-beat / 0.03)))) * np.exp(-beat * 18)
    bass = 0.35 * np.sin(2 * np.pi * 55 * t)
    hat_env = np.exp(-(t % 0.125) * 70)
    hats = sosfilt(butter(4, 6000, 'highpass', fs=sr, output='sos'), rng.standard_normal(len(t))) * hat_env * 0.25
    pad = 0.08 * rng.standard_normal((len(t), 2))
    drop = (np.floor(t / 10.0) % 2 == 0).astype(np.float64)
    mono = (0.3 + 0.7 * drop) * (kick + bass) + hats * (0.4 + 0.6 * drop)
    audio = mono[:, None] + pad * (0.32 + 0.68 * drop)[:, None]
    return 0.5 * audio / np.max(np.abs(audio))


_track_cache = {}


def selftest_track():
    """80 s 合成曲与它的入口分析（几条自检共用，只算一次）。"""
    if 'x' not in _track_cache:
        _track_cache['x'] = synthetic_track()
        _track_cache['inp'] = SignalAnalysis(_track_cache['x'], SELFTEST_SR)
    return _track_cache['x'], _track_cache['inp']


@selftest
def pump_reads_known_modulation():
    """电平按 ±3 dB、2 Hz 正弦摆动的 4 kHz 正弦：pump_db = 3 / √2，误差 ≤ 0.1 dB（GLOSSARY ②）。"""
    sr, seconds, depth = SELFTEST_SR, 30.0, 3.0
    t = np.arange(int(seconds * sr)) / sr
    x = np.column_stack([0.25 * np.sin(2 * np.pi * 4000 * t)] * 2)
    y = x * (10.0 ** (depth * np.sin(2 * np.pi * 2.0 * t) / 20.0))[:, None]
    mask = (t >= 2.0) & (t < seconds - 2.0)   # 去掉两头滤波器的起止过渡
    value, _ = pump_db(SignalAnalysis(x, sr), SignalAnalysis(y, sr), mask)
    expected = depth / math.sqrt(2.0)
    return abs(value - expected) <= 0.1, f'pump_db {value:.4f}，期望 {expected:.4f} ± 0.1'


@selftest
def pure_gain_differences_are_zero():
    """出口 = 0.6 × 入口：每个差值项都有读数且 |值| ≤ 1e-9（数值零）。"""
    x, inp = selftest_track()
    values, _ = compare_signals(inp, SignalAnalysis(0.6 * x, SELFTEST_SR), np.ones(len(x), dtype=bool))
    bad = {k: values[k] for k in DIFFERENCE_FIELDS if not (math.isfinite(values[k]) and abs(values[k]) <= NUMERIC_ZERO)}
    if bad:
        return False, f'不为零或没读数：{bad}'
    return True, '全部 ≤ 1e-9：' + ', '.join(f'{k}={values[k]:.1e}' for k in DIFFERENCE_FIELDS)


@selftest
def identity_differences_are_exactly_zero():
    """出口 = 入口（逐样本相同的副本）：每个差值项恰为 0.0。"""
    x, inp = selftest_track()
    values, _ = compare_signals(inp, SignalAnalysis(x.copy(), SELFTEST_SR), np.ones(len(x), dtype=bool))
    bad = {k: values[k] for k in DIFFERENCE_FIELDS if values[k] != 0.0}
    return not bad, '全部恰为 0' if not bad else f'不为 0：{bad}'


@selftest
def gain_method_constant_and_sine():
    """增益法排查子表：GR 恒为 2 dB → P_slow = 0、D_fast = −∞；GR = 3 + 2·sin(2π·2 t) → P_slow = 2 / √2 ± 0.05。"""
    block_rate = SELFTEST_SR / 512.0
    t = np.arange(int(30.0 * block_rate)) / block_rate
    use = (t >= 2.0) & (t < 28.0)
    constant = np.full(len(t), 2.0)
    sine = 3.0 + 2.0 * np.sin(2 * np.pi * 2.0 * t)
    slow0, fast0 = p_slow_db(constant, block_rate, use), d_fast_db(constant, np.ones(len(t)), block_rate, use)
    slow1 = p_slow_db(sine, block_rate, use)
    expected = 2.0 / math.sqrt(2.0)
    ok = slow0 == 0.0 and fast0 == -math.inf and abs(slow1 - expected) <= 0.05
    return ok, f'恒定：P_slow {slow0}、D_fast {fast0}；正弦：P_slow {slow1:.4f}（期望 {expected:.4f}）'


def fake_trace_rows(n_blocks, block, sr, latency, mainness, gain=None, hard=None, diff=None, inn=None):
    """合成 --trace 行：t_sec = (位置 − 链延迟) / 采样率，与 RenderMain 一样只留 4 位小数。"""
    rows = []
    for k in range(n_blocks):
        rows.append({'t_sec': f'{(k * block - latency) / sr:.{T_SEC_DECIMALS}f}', 'limit_gr_db': '0.000',
                     'limit_gain_db': f'{(gain[k] if gain is not None else 0.0):.3f}',
                     'current_mainness': f'{mainness[k]:.3f}', 'protection_db': '0.000', 'guard_cause': 'none',
                     'hard_clip_samples': '' if hard is None else str(hard[k]),
                     'limit_diff_energy': '' if diff is None else f'{diff[k]:.9e}',
                     'limit_in_energy': '' if inn is None else f'{inn[k]:.9e}'})
    return rows


@selftest
def trace_fit_survives_rounding():
    """块起点拟合：块长 512、链延迟 1384（t_sec 舍入到 4 位后每行差最多 2.4 个样本），200 行 → 块长与链延迟原样还原。"""
    trace = Trace(fake_trace_rows(200, 512, SELFTEST_SR, 1384, [1.0] * 200), SELFTEST_SR)
    return trace.block == 512 and trace.latency == 1384, f'块长 {trace.block}、链延迟 {trace.latency}'


@selftest
def trace_main_mask_uses_chain_position():
    """主段掩码：块长 480、链延迟 96，第 2、3 块主段（0.9、0.5；0.49 不算）→ 输入样本 [960, 1920)（链里位置，不减延迟）。"""
    sr, block, latency = SELFTEST_SR, 480, 96
    mainness = [0.0, 0.2, 0.9, 0.5, 0.49] + [0.0] * 35
    trace = Trace(fake_trace_rows(len(mainness), block, sr, latency, mainness), sr)
    mask = main_mask_from_trace(trace, 8000)
    expected = np.zeros(8000, dtype=bool)
    expected[960:1920] = True
    ok = trace.latency == latency and trace.block == block and np.array_equal(mask, expected)
    span = f'[{np.flatnonzero(mask)[0]}, {np.flatnonzero(mask)[-1] + 1})' if mask.any() else '无'
    return ok, f'链延迟 {trace.latency}、块长 {trace.block}、主段样本 {span}'


@selftest
def control_quality_counts():
    """控制质量：ΔGain = +0.1, +0.1, −0.1, +0.1, 0, −0.1（7 块全在主段、合 1 分钟）→ 总变差 0.5 dB / 分、变号 3 次 / 分；
    第 4 块不是主段时只剩 +0.1, +0.1 | 0, −0.1 → 总变差 0.3 dB、变号 0（隔着非主段的那次翻转不算）。"""
    gain = np.array([0.0, 0.1, 0.2, 0.1, 0.2, 0.2, 0.1])
    block_seconds = 60.0 / len(gain)
    tv, flips = control_quality(gain, np.ones(len(gain), dtype=bool), block_seconds)
    split = np.array([True, True, True, False, True, True, True])
    tv2, flips2 = control_quality(gain, split, block_seconds)
    minutes2 = 6 * block_seconds / 60.0
    ok = abs(tv - 0.5) <= 1e-9 and abs(flips - 3.0) <= 1e-9 and abs(tv2 * minutes2 - 0.3) <= 1e-9 and flips2 == 0.0
    return ok, f'整段：TV {tv:.3f} dB、变号 {flips:.0f}；断开：TV {tv2 * minutes2:.3f} dB、变号 {flips2 * minutes2:.0f}'


@selftest
def guard_histogram():
    """守门：10 块里 budget 3 块、psr 1 块 → 介入 40 %，直方图 budget:30.0;psr:10.0。"""
    pct, histogram = guard_stats(['none'] * 6 + ['budget'] * 3 + ['psr'])
    return pct == 40.0 and histogram == 'budget:30.0;psr:10.0', f'{pct} %、{histogram}'


@selftest
def labels_become_sections():
    """标签 → 段落：transition / exclude 打断，同类相邻并段；主段核对：主段 = 第 6～8 小节（drop）→ 主段在 drop 里 100 %、drop 被判主段 60 %。"""
    kinds = ['intro'] * 4 + ['transition'] + ['drop'] * 3 + ['breakdown'] * 2 + ['exclude'] + ['drop'] * 2
    labels = {('t', i + 1): k for i, k in enumerate(kinds)}
    layout = [(s['kind'], s['start_bar'], s['end_bar']) for s in sections_from_labels(labels, 't')]
    expected = [('intro', 1, 4), ('drop', 6, 8), ('breakdown', 9, 10), ('drop', 12, 13)]
    sr, bpm = 1000, 120.0   # 一小节 2 s = 2000 样本
    mask = np.zeros(13 * 2000, dtype=bool)
    mask[5 * 2000:8 * 2000] = True
    inside, covered, _ = label_agreement(mask, sr, bpm, labels, 't')
    ok = layout == expected and abs(inside - 100.0) <= 1e-9 and abs(covered - 60.0) <= 1e-9
    return ok, f'段落 {layout}；主段在 drop 里 {inside:.1f} %、drop 被判主段 {covered:.1f} %'


@selftest
def spread_gate_matches_lra():
    """无标注落差用的门后 S 与 codec_tp_check.loudness_range_lu 同两道门：门后 P95 − P10 与 LRA 相差 ≤ 1e-9。"""
    _, inp = selftest_track()
    gated = gated_short_term(inp.short)
    mine = float(np.subtract(*np.percentile(gated, [meters.LRA_HIGH_PERCENTILE, meters.LRA_LOW_PERCENTILE])))
    return abs(mine - inp.lra) <= 1e-9, f'门后 P95 − P10 {mine:.6f}，loudness_range_lu {inp.lra:.6f}'


def fake_segments(levels):
    """按小节的逐段统计（house_gate 读的格式）：{小节: {lufs, valid_s, mode}}。"""
    return {bar: {'lufs': f'{lufs:.3f}', 'valid_s': '2.0', 'mode': 'bars'} for bar, lufs in levels.items()}


@selftest
def drop_numbers_agree_with_house_gate():
    """段落口径的落差损失与 drop 目标偏差，与 house_gate ③ / ⑤ 的判定一致（损失 1.0 / 1.5 LU 各判一次；偏差 0.6 / 1.6 LU 各判一次）。"""
    sections = [{'name': 'breakdown1', 'kind': 'breakdown', 'start_bar': 1, 'end_bar': 2},
                {'name': 'drop1', 'kind': 'drop', 'start_bar': 3, 'end_bar': 4}]
    inp = fake_segments({1: -16.0, 2: -16.0, 3: -10.0, 4: -10.0})
    results = []
    for wanted in (1.0, 1.5):
        out = fake_segments({1: -12.0, 2: -12.0, 3: -6.0 - wanted, 4: -6.0 - wanted})
        loss, _ = section_drop_loss_lu(inp, out, sections)
        verdict, _ = house_gate.gate_breakdown({'delivery': 'standard'}, inp, out, sections)
        results.append(abs(loss - wanted) <= 1e-9 and (verdict == 'PASS') == (loss <= house_gate.BREAKDOWN_DROP_LOSS_LU))
    row = {'delivery': 'streaming', 'glaze': '100', 'weight_warm': '0', 'weight_punchy': '1', 'weight_clean': '0',
           'profile_lra_lu': '5.0', 'plan_effective_target_lufs': '', 'plan_requested_target_lufs': ''}
    target, _ = house_gate.delivery_target(row)   # 冲击、LRA ≤ 6 → 流媒体基准 −10
    for offset in (0.6, 1.6):
        out = fake_segments({1: -12.0, 2: -12.0, 3: target + offset, 4: target + offset})
        deviation, _ = drop_target_deviation_lu(row, inp, out, [sections[1]])
        verdict, _ = house_gate.gate_target(row, inp, out, [sections[1]])
        results.append(abs(deviation - offset) <= 1e-9 and (verdict == 'PASS') == (abs(deviation) <= house_gate.TARGET_WINDOW_LU))
    return all(results), f'四种情形与 house_gate 一致：{results}'


@selftest
def csv_formatting():
    """写 CSV：NaN → 空、±inf → inf / -inf、-0.000 → 0.000、整数原样。"""
    cases = [(math.nan, 3, ''), (math.inf, 3, 'inf'), (-math.inf, 2, '-inf'), (-1e-12, 3, '0.000'),
             (-0.0004, 3, '0.000'), (-0.0006, 3, '-0.001'), (12, 'int', '12'), (None, 3, '')]
    bad = [(v, k, format_value(v, k)) for v, k, want in cases if format_value(v, k) != want]
    return not bad, '全部符合' if not bad else f'不符：{bad}'


@contextlib.contextmanager
def fake_render(x, sr, gain, align_gain_db, block=480, latency=1392):
    """临时输出目录（自检用完即删）：源文件、metrics.csv（input + 一行 render）、对齐过的成品（gain × 入口 + 0.35 s 余振）、trace。
    trace：前 5 s 不是主段；Gain 每块在 1.0 / 1.1 dB 间来回；每块限幅差值能量 1、入口能量 10；硬钳位 0。"""
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        (root / 'src').mkdir()
        (root / 'out' / 'trace').mkdir(parents=True)
        sf.write(str(root / 'src' / 'song.wav'), x, sr, subtype='FLOAT')
        name = 'song__g35_clay-auto_auto.wav'
        ring = int(math.ceil(0.35 * sr))
        master = np.vstack([gain * x, np.zeros((ring, 2))]) * 10.0 ** (align_gain_db / 20.0)
        sf.write(str(root / 'out' / name), master, sr, subtype='FLOAT')
        rows = [dict(zip(METRICS_REQUIRED, ['input', 'song', 'song.wav', str(sr), '', '', '', '', '-1.5', ''])),
                dict(zip(METRICS_REQUIRED, ['render', 'song', 'song.wav', str(sr), '35', 'auto', 'auto',
                                            f'{align_gain_db:.3f}', '-1.200', name]))]
        with open(root / 'out' / 'metrics.csv', 'w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(METRICS_REQUIRED), lineterminator='\n')
            writer.writeheader()
            writer.writerows(rows)
        n_blocks = (len(master) + latency) // block + 1
        mainness = [0.0 if k * block < 5 * sr else 1.0 for k in range(n_blocks)]
        gain_db = [1.0 + 0.1 * (k % 2) for k in range(n_blocks)]
        trace = fake_trace_rows(n_blocks, block, sr, latency, mainness, gain=gain_db, hard=[0] * n_blocks,
                                diff=[1.0] * n_blocks, inn=[10.0] * n_blocks)
        with open(trace_path_of(root / 'out', name), 'w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(trace[0]), lineterminator='\n')
            writer.writeheader()
            writer.writerows(trace)
        yield root


@selftest
def end_to_end_pure_gain_render():
    """端到端：临时输出目录（成品 = 0.7 × 入口、对齐 +4.5 dB、余振 0.35 s、合成 trace）→ 一行质量表：对齐还原后差值项 ≈ 0（≤ 1e-4）、
    主段 = 第 5 s 之后（20 s）、限幅差值 = −10 dB、每对相邻块变号一次、硬钳位 0、真峰值与 Ceiling 照抄 / 出厂 −1.0、无标注走 P90 − P25。"""
    sr = SELFTEST_SR
    x = synthetic_track(seconds=25.0)
    with fake_render(x, sr, 0.7, 4.5) as root:
        tasks = collect_tasks([root / 'out'], [root / 'src'], {})
        rows, errors = process_source(tasks[0])
    if errors or len(rows) != 1:
        return False, f'处理失败：{errors}'
    r = rows[0]
    checks = {
        'pump≈0': abs(r['pump_db']) <= FLOAT32_ZERO, 'lf_crest≈0': abs(r['lf_crest_db']) <= FLOAT32_ZERO,
        'lra≈0': abs(r['lra_delta_lu']) <= FLOAT32_ZERO, 'net≈0': abs(r['net_spectrum_p95_db']) <= FLOAT32_ZERO,
        'main_s=20': abs(r['main_s'] - 20.0) <= 1e-9, 'limit_diff=-10': abs(r['limit_diff_db'] + 10.0) <= 1e-9,
        'hard_clip=0': r['hard_clip_samples'] == 0, 'tp': r['true_peak_dbtp'] == -1.2 and r['ceiling_dbtp'] == -1.0,
        'flips': abs(r['gain_sign_changes_per_min'] * (480 / sr / 60.0) - 1.0) <= 0.01,
        'drop_method': r['drop_loss_method'] == 'p90_p25',
    }
    bad = [k for k, ok in checks.items() if not ok]
    detail = f"pump {r['pump_db']:.1e}、主段 {r['main_s']:.3f} s、na：{r['na_reason']}"
    return not bad, ('全部符合' if not bad else f'不符：{bad}') + f'（{detail}）'


def run_selftest():
    failed = 0
    for check in SELFTEST_CHECKS:
        try:
            ok, detail = check()
        except Exception as error:   # 自检要把任何异常都算失败并接着跑下一条
            ok, detail = False, f'抛异常 {type(error).__name__}：{error}'
        failed += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {check.__name__}：{detail}", flush=True)
    print(f'{len(SELFTEST_CHECKS) - failed} / {len(SELFTEST_CHECKS)} ok')
    return 0 if failed == 0 else 1


def main(argv):
    parser = argparse.ArgumentParser(description='T-096a 质量指标管线（口径见 docs/validation/2026-10-11-T096a-metrics/GLOSSARY.md）')
    parser.add_argument('--selftest', action='store_true', help='只跑合成信号自检')
    parser.add_argument('--renders', nargs='+', type=Path, help='celadon_render 的输出目录（含 metrics.csv、成品、trace/）')
    parser.add_argument('--sources', nargs='+', type=Path, help='源文件目录（按 metrics.csv 的 source_file 找）')
    parser.add_argument('--out', type=Path, help='写出的 quality.csv')
    parser.add_argument('--labels-auto', help='逐小节自动标签 labels-auto.tsv（Jamendo；有它才算段落口径与主段核对）')
    parser.add_argument('--labels-override', help='人工改标 labels-override.tsv')
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1), help='并行的歌数（默认 min(8, 核数)）')
    args = parser.parse_args(argv)
    if args.selftest:
        return run_selftest()
    if not args.renders or not args.sources or not args.out:
        parser.error('要给 --renders、--sources、--out（或 --selftest）')
    if args.labels_override and not args.labels_auto:
        parser.error('--labels-override 要与 --labels-auto 一起用')
    return run(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
