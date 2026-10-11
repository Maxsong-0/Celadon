#!/usr/bin/env python3
"""T-096c 量具库自检：合成信号 + 已知答案，只测 tools/render/codec_tp_check.py 里的量具函数。

不读语料、不读外部文件（几条用例把合成信号写进临时目录再量，用完即删）。
用法：python tools/render/measure_selfcheck.py（在哪个目录跑都行）
每条检查一行 ok / FAIL；全部通过退出码 0，任何一条 FAIL 或抛异常退出码 1。
依赖 numpy / scipy / soundfile / pyloudnorm，版本见 docs/validation/2026-10-08-T085-codec-tp/requirements.txt。
"""
import contextlib
import importlib
import math
from pathlib import Path
import sys
import tempfile

import numpy as np
import pyloudnorm as pyln
import soundfile as sf

# 与 codec_tp_check.py 同目录，不依赖 cwd；用 importlib 导入，保证上面的 sys.path 设置先于导入生效
sys.path.insert(0, str(Path(__file__).resolve().parent))
meters = importlib.import_module('codec_tp_check')

SR = 48000
CHECKS = []

# measure() 一直以来的返回键及顺序（T-085 的 masters.csv / codec_roundtrips.csv 列头由它决定，续测还按它读）
MEASURE_KEYS = ['duration_s', 'sample_rate', 'lufs_i', 'sample_peak_dbfs', 'sample_clip_count', 'crest_db',
                'tp4_dbtp', 'intersample_clip4_count', 'tp8_dbtp', 'intersample_clip8_count',
                'psr_median_db', 'plr_db']


def check(func):
    CHECKS.append(func)
    return func


def tone(freq, seconds, amp, sr=SR, phase=0.0):
    t = np.arange(int(round(seconds * sr))) / sr
    return amp * np.sin(2 * np.pi * freq * t + phase)


def stereo(left, right=None):
    return np.column_stack((left, left if right is None else right))


def burst_train(seconds, amp, sr=SR):
    """每 0.5 s 一个 0.125 s 的 1 kHz 正弦脉冲（整数个周期，占空比 1/4），其余静音。"""
    period, on = sr // 2, sr // 8
    one = np.zeros(period)
    one[:on] = amp * np.sin(2 * np.pi * 1000 * np.arange(on) / sr)
    return np.tile(one, int(round(seconds * 2)))


def near(value, expected, tol):
    return abs(value - expected) <= tol   # NaN 比较为 False，算不通过


_psr_cache = {}


def psr_median(kind, gain=1.0):
    """20 s 双声道正弦 / 脉冲串（振幅 0.5 × gain）的 PSR 中位数；几条用例共用，缓存避免重复做 8 倍过采样。"""
    key = (kind, gain)
    if key not in _psr_cache:
        base = tone(1000, 20, 0.5) if kind == 'sine' else burst_train(20, 0.5)
        _psr_cache[key] = float(np.median(meters.psr_series_db(stereo(base * gain), SR)))
    return _psr_cache[key]


@contextlib.contextmanager
def temp_wav(data, sr, subtype='PCM_24'):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'selfcheck.wav'
        sf.write(str(path), data, sr, subtype=subtype)
        yield path


def sample_master():
    """10 s、44.1 kHz：中间 0.3 的 1 kHz 正弦 + 侧 0.03 的同频余弦（S/M 约 -20 dB），L = M + S，R = M - S。"""
    t = np.arange(10 * 44100) / 44100
    mid = 0.3 * np.sin(2 * np.pi * 1000 * t)
    side = 0.03 * np.cos(2 * np.pi * 1000 * t)
    return np.column_stack((mid + side, mid - side))


# ---------------------------------------------------------------- 短期响度 / LRA

@check
def grid_constants():
    ok = meters.PSR_WINDOW_HOPS == 30 and meters._hop_samples(44100) == 4410 and meters._hop_samples(48000) == 4800
    return ok, f'PSR_WINDOW_HOPS={meters.PSR_WINDOW_HOPS}，hop(44.1k)={meters._hop_samples(44100)}，hop(48k)={meters._hop_samples(48000)}'


@check
def short_term_series_flat_and_matches_integrated():
    x = stereo(tone(1000, 10, 0.5))
    series = meters.short_term_loudness_series(x, SR)
    integrated = pyln.Meter(SR, filter_class='DeMan').integrated_loudness(x)
    spread = float(np.ptp(series))
    ok = len(series) == 71 and spread < 0.02 and near(float(np.median(series)), integrated, 0.02)
    return ok, f'共 {len(series)} 个（期望 71），起伏 {spread:.4f} dB，中位 {np.median(series):.3f} vs 积分响度 {integrated:.3f} LUFS'


@check
def lra_two_levels_differ_by_10db():
    sr = 44100
    loud = tone(1000, 20, 0.5, sr)
    x = stereo(np.concatenate((loud, loud * 10 ** (-10 / 20))))
    lra = meters.loudness_range_lu(x, sr)
    return near(lra, 10.0, 0.5), f'LRA = {lra:.3f} LU，期望 10 ± 0.5'


@check
def lra_constant_level_is_zero():
    lra = meters.loudness_range_lu(stereo(tone(1000, 20, 0.5)), SR)
    return near(lra, 0.0, 0.05), f'LRA = {lra:.4f} LU，期望 0'


@check
def lra_relative_gate_drops_very_quiet_tail():
    sr = 44100
    loud = tone(1000, 60, 0.5, sr)
    x = stereo(np.concatenate((loud, tone(1000, 20, 0.005, sr))))   # 尾段比主体低 40 dB，应被 -20 LU 相对门挡掉
    lra = meters.loudness_range_lu(x, sr)
    return near(lra, 0.0, 0.5), f'LRA = {lra:.3f} LU，期望约 0（没有相对门会读到约 40）'


@check
def lra_too_short_is_nan():
    lra = meters.loudness_range_lu(stereo(tone(1000, 2, 0.5)), SR)
    return math.isnan(lra), f'2 s 的 LRA = {lra}，期望 NaN'


# ---------------------------------------------------------------- 真峰值 / PSR

@check
def true_peak_of_quarter_rate_sine():
    # fs/4 正弦、相位 45 度：采样点恒为 ±0.7071·A，真峰值 = A（落在两个采样点正中，4 倍和 8 倍网格都踩得到）。
    amp = 0.8
    x = stereo(tone(SR / 4, 5, amp, phase=math.pi / 4))
    fields, _ = meters.true_peak_scan(x, SR)
    expected = 20 * math.log10(amp)
    sample_peak = 20 * math.log10(float(np.max(np.abs(x))))
    ok = near(fields['tp4_dbtp'], expected, 0.1) and near(fields['tp8_dbtp'], expected, 0.1) and near(sample_peak, expected - 3.01, 0.05)
    return ok, f'TP4 {fields["tp4_dbtp"]:.3f} / TP8 {fields["tp8_dbtp"]:.3f}，期望 {expected:.3f} dBTP；采样峰值 {sample_peak:.3f}（期望 {expected - 3.01:.3f}）'


@check
def psr_of_stereo_sine_is_about_zero():
    # 双声道同幅正弦：S = 20·log10(A) + (K 加权在 1 kHz 的增益 - 0.691) ≈ 峰值电平，所以 PSR ≈ 0。
    p10, p50, p90 = meters.psr_percentiles(stereo(tone(1000, 10, 0.5)), SR)
    ok = near(p50, 0.0, 0.5) and (p90 - p10) < 0.05 and p10 <= p50 <= p90
    return ok, f'P10 {p10:.3f} / 中位 {p50:.3f} / P90 {p90:.3f} dB，期望中位约 0、三者几乎相等'


@check
def psr_of_quarter_duty_bursts_is_6db_above_sine():
    # 同一峰值、占空比 1/4：3 s 窗恰含 6 个周期，平均功率低 10·log10(4) = 6.02 dB，峰值不变，所以 PSR 高 6.02 dB。
    sine, burst = psr_median('sine'), psr_median('burst')
    delta = burst - sine
    ok = near(delta, 10 * math.log10(4), 0.15) and near(burst, 6.02, 0.6)
    return ok, f'脉冲串 {burst:.3f} - 正弦 {sine:.3f} = {delta:.3f} dB，期望 6.02 ± 0.15'


@check
def psr_is_gain_invariant():
    base, quiet = psr_median('burst'), psr_median('burst', 0.25)
    return near(quiet, base, 0.01), f'增益 x0.25 前后 PSR 中位 {base:.4f} -> {quiet:.4f} dB，期望相同'


@check
def psr_too_short_is_nan():
    p10, p50, p90 = meters.psr_percentiles(stereo(tone(1000, 2, 0.5)), SR)
    return all(math.isnan(v) for v in (p10, p50, p90)), f'2 s 的 PSR 分位 = ({p10}, {p50}, {p90})，期望全 NaN'


# ---------------------------------------------------------------- DC / 静音 / 削波

@check
def dc_offset_reads_the_larger_channel():
    ac = tone(1000, 10, 0.3)
    x = stereo(0.01 + ac, -0.02 + ac)
    dc = meters.dc_offset_db(x)
    clean = meters.dc_offset_db(stereo(ac))
    expected = 20 * math.log10(0.02)
    return near(dc, expected, 0.01) and clean < -100, f'DC {dc:.3f} dBFS（期望 {expected:.3f}）；无偏置读 {clean:.1f} dBFS（期望 -100 以下）'


@check
def leading_trailing_silence_seconds():
    x = stereo(np.concatenate((np.zeros(SR), tone(1000, 3, 0.5), np.zeros(SR // 2))))
    lead, trail = meters.leading_trailing_silence_s(x, SR)
    ok = near(lead, 1.0, 0.001) and near(trail, 0.5, 0.001)
    return ok, f'开头 {lead:.5f} s（期望 1.0）、结尾 {trail:.5f} s（期望 0.5）'


@check
def silence_threshold_and_all_silent():
    quiet = stereo(tone(1000, 3, 10 ** (-70 / 20)))   # -70 dBFS 的音
    all_silent = meters.leading_trailing_silence_s(quiet, SR)                # 默认门 -60：整段都算静音
    heard = meters.leading_trailing_silence_s(quiet, SR, thresh_dbfs=-80)    # 门放到 -80：几乎没有静音
    ok = near(all_silent[0], 3.0, 1e-9) and near(all_silent[1], 3.0, 1e-9) and heard[0] < 0.001 and heard[1] < 0.001
    return ok, f'门 -60：{all_silent}（期望 (3, 3)）；门 -80：({heard[0]:.5f}, {heard[1]:.5f})（期望接近 0）'


@check
def sample_clip_count_counts_only_beyond_full_scale():
    x = np.array([[0.5, 1.0], [1.5, -2.0], [-1.0, 0.0]])
    count = meters.sample_clip_count(x)
    return count == 2, f'计数 {count}，期望 2（正好 ±1.0 不算）'


# ---------------------------------------------------------------- 低频相关 / 单声道损失 / 侧中比

@check
def low_corr_in_phase_and_anti_phase():
    lf = tone(60, 4, 0.5)
    same = meters.low_band_correlation(stereo(lf), SR)
    anti = meters.low_band_correlation(stereo(lf, -lf), SR)
    loss_same, loss_anti = meters.mono_lf_loss_db(same), meters.mono_lf_loss_db(anti)
    ok = near(same, 1.0, 1e-9) and near(anti, -1.0, 1e-9) and near(loss_same, 0.0, 1e-9) and loss_anti == -math.inf
    return ok, f'同相 rho={same:.9f} 损失 {loss_same:.6f} dB；反相 rho={anti:.9f} 损失 {loss_anti} dB'


@check
def low_corr_ignores_high_frequencies():
    lf, hf = tone(60, 4, 0.4), tone(5000, 4, 0.4)
    low_same = meters.low_band_correlation(stereo(lf + hf, lf - hf), SR)    # 低频同相、高频反相
    low_anti = meters.low_band_correlation(stereo(lf + hf, -lf + hf), SR)   # 低频反相、高频同相
    ok = low_same > 0.9999 and low_anti < -0.9999
    return ok, f'低频同相高频反相 rho={low_same:.7f}（期望接近 +1）；低频反相高频同相 rho={low_anti:.7f}（期望接近 -1）'


@check
def low_corr_is_energy_ratio_not_pearson():
    # L = 0.6·s、R = 0.3·s：皮尔逊 = 1，但 rho = 2·0.18 / (0.36 + 0.09) = 0.8（侧声道占能量的比例）。
    lf = tone(60, 4, 1.0)
    rho = meters.low_band_correlation(stereo(0.6 * lf, 0.3 * lf), SR)
    return near(rho, 0.8, 1e-6), f'rho = {rho:.8f}，期望 0.8'


@check
def low_corr_silent_is_nan():
    rho = meters.low_band_correlation(np.zeros((SR, 2)), SR)
    return math.isnan(rho), f'全静音 rho = {rho}，期望 NaN'


@check
def mono_lf_loss_formula():
    values = {0.0: 10 * math.log10(0.5), 0.5: 10 * math.log10(0.75), 1.0: 0.0}
    ok = all(near(meters.mono_lf_loss_db(rho), expected, 1e-9) for rho, expected in values.items())
    ok = ok and meters.mono_lf_loss_db(-1.0) == -math.inf and meters.mono_lf_loss_db(-1.0000001) == -math.inf
    ok = ok and meters.mono_lf_loss_db(1.0000001) == 0.0 and math.isnan(meters.mono_lf_loss_db(math.nan))
    return ok, 'rho = 0 / 0.5 / 1 / -1 / 越界 / NaN 的读数符合 10·log10((1+rho)/2)'


@check
def side_mid_ratio_known_answers():
    t = np.arange(4 * SR) / SR
    mid, side = np.sin(2 * np.pi * 1000 * t), 0.1 * np.cos(2 * np.pi * 1000 * t)
    known = meters.side_mid_ratio_db(np.column_stack((mid + side, mid - side)))   # S/M 能量比 0.01 -> -20 dB
    pure_mid = meters.side_mid_ratio_db(stereo(mid))
    pure_side = meters.side_mid_ratio_db(stereo(mid, -mid))
    silent = meters.side_mid_ratio_db(np.zeros((SR, 2)))
    ok = near(known, -20.0, 0.001) and pure_mid == -math.inf and pure_side == math.inf and math.isnan(silent)
    return ok, f'已知 {known:.4f} dB（期望 -20）；纯中间 {pure_mid}；纯侧 {pure_side}；静音 {silent}'


# ---------------------------------------------------------------- 临时监听嫌疑

@check
def monitor_suspicion_rules():
    rule = meters.monitor_export_suspicion
    only_side_mid = rule({'side_mid_db': -80.0, 'lufs_i': -10.0, 'tp8_dbtp': -2.0}, -10.0, -2.0)
    gain_cut = rule({'side_mid_db': -20.0, 'lufs_i': -17.0, 'tp8_dbtp': -8.0}, -14.0, -1.0)           # 低 3.0 LU（含边界）、TP 低 7 dB
    not_low_enough = rule({'side_mid_db': -20.0, 'lufs_i': -16.9, 'tp8_dbtp': -8.0}, -14.0, -1.0)     # 只低 2.9 LU
    tp_near_ceiling = rule({'side_mid_db': -20.0, 'lufs_i': -17.0, 'tp8_dbtp': -1.5}, -14.0, -1.0)    # TP 离 Ceiling 才 0.5 dB
    no_target = rule({'side_mid_db': -20.0, 'lufs_i': -17.0, 'tp8_dbtp': -8.0}, None, None)           # 没给目标，规则 2 不判
    pure_mid = rule({'side_mid_db': -math.inf, 'lufs_i': -10.0, 'tp8_dbtp': -2.0}, None, None)
    all_nan = rule({'side_mid_db': math.nan, 'lufs_i': math.nan, 'tp8_dbtp': math.nan}, -14.0, -1.0)
    ok = ('S/M' in only_side_mid and 'I 比目标' not in only_side_mid and 'I 比目标' in gain_cut and 'S/M' not in gain_cut
          and not_low_enough == '' and tp_near_ceiling == '' and no_target == '' and 'S/M' in pure_mid and all_nan == '')
    return ok, f'S/M: {only_side_mid!r}；降增益: {gain_cut!r}；2.9 LU: {not_low_enough!r}；TP 近 Ceiling: {tp_near_ceiling!r}'


# ---------------------------------------------------------------- 文件接口（写临时 WAV 再量）

@check
def file_info_reads_header():
    with temp_wav(sample_master(), 44100, 'PCM_24') as path:
        info = meters.file_info(path)
    ok = info['subtype'] == 'PCM_24' and info['sample_rate'] == 44100 and info['channels'] == 2 and near(info['duration_s'], 10.0, 1e-9)
    return ok, str(info)


@check
def measure_keeps_its_keys_and_order():
    with temp_wav(sample_master(), 44100, 'PCM_24') as path:
        result = meters.measure(path)
    finite = all(math.isfinite(v) for v in result.values())
    return list(result) == MEASURE_KEYS and finite, f'键 {list(result)}'


@check
def measure_master_matches_field_table_and_measure():
    with temp_wav(sample_master(), 44100, 'PCM_24') as path:
        plain = meters.measure(path)
        row = meters.measure_master(path)
        suspicious = meters.measure_master(path, target_lufs=-5.0, ceiling_dbtp=-2.0)
    same = all(row[key] == plain[key] for key in plain)   # 同一份实现，应当逐位相等
    ordered = list(row) == list(meters.MASTER_FIELD_NAMES) and set(MEASURE_KEYS) <= set(row)
    sane = (row['file'] == 'selfcheck.wav' and row['na_reason'] == '' and row['monitor_suspicion'] == ''
            and row['psr_p10_db'] <= row['psr_median_db'] <= row['psr_p90_db'] and -25 < row['side_mid_db'] < -15
            and row['nonfinite_sample_count'] == 0 and suspicious['monitor_suspicion'] != '')
    return same and ordered and sane, f'与 measure() 相等={same}，键序={ordered}，内容合理={sane}，na_reason={row["na_reason"]!r}'


@check
def measure_master_mono_file_is_reported_not_zero():
    with temp_wav(tone(1000, 5, 0.3, 44100), 44100, 'PCM_24') as path:
        row = meters.measure_master(path)
    ok = (row['channels'] == 1 and math.isnan(row['lufs_i']) and math.isnan(row['low_corr_lt125hz'])
          and row['na_reason'] != '' and row['sample_clip_count'] == 0)
    return ok, f'channels={row["channels"]}，lufs_i={row["lufs_i"]}，na_reason={row["na_reason"]!r}'


@check
def measure_master_short_file_leaves_blank():
    with temp_wav(sample_master()[:2 * 44100], 44100, 'PCM_24') as path:
        row = meters.measure_master(path)
    ok = math.isnan(row['lufs_i']) and math.isnan(row['lra_lu']) and math.isfinite(row['low_corr_lt125hz']) and '3 s' in row['na_reason']
    return ok, f'lufs_i={row["lufs_i"]}，low_corr={row["low_corr_lt125hz"]:.4f}，na_reason={row["na_reason"]!r}'


@check
def measure_master_reports_nonfinite_samples():
    data = sample_master()
    data[1000, 0] = math.nan
    with temp_wav(data, 44100, 'FLOAT') as path:
        row = meters.measure_master(path)
    ok = row['nonfinite_sample_count'] == 1 and math.isnan(row['lufs_i']) and 'NaN' in row['na_reason']
    return ok, f'非有限样本 {row["nonfinite_sample_count"]}，na_reason={row["na_reason"]!r}'


@check
def readme_lists_every_field():
    readme = Path(__file__).resolve().parents[2] / 'docs' / 'validation' / '2026-10-11-T096c-measure-lib' / 'README.md'
    if not readme.exists():
        return True, f'{readme.name} 不在原位，跳过（字段表以 codec_tp_check.py 为准）'
    text = readme.read_text(encoding='utf-8')
    missing = [name for name in meters.MASTER_FIELD_NAMES if f'`{name}`' not in text]
    return not missing, 'README 字段表齐全' if not missing else f'README 缺字段：{missing}'


def main():
    failures = 0
    for func in CHECKS:
        try:
            ok, detail = func()
        except Exception as error:   # 自检用例自己抛异常也算 FAIL，并写明原因
            ok, detail = False, f'抛出 {type(error).__name__}: {error}'
        print(f"{'ok  ' if ok else 'FAIL'} {func.__name__}: {detail}", flush=True)
        failures += 0 if ok else 1
    print(f'{len(CHECKS) - failures}/{len(CHECKS)} ok', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
