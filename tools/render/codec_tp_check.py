#!/usr/bin/env python3
"""T-085 offline codec round trips; never changes plugin parameters or defaults.

Dependencies: numpy, scipy, soundfile, pyloudnorm. Audio stays in --work;
CSV/JSON evidence goes in --out. See the T-085 REPORT.md for definitions.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

import numpy as np
import pyloudnorm as pyln
import soundfile as sf
from scipy.signal import chirp, firwin, sosfilt, butter
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


def main():
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
    main()
