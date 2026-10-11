#!/usr/bin/env python3
"""T-096c2 交付检查单自检：合成信号 + 已知答案，测 tools/render/codec_tp_check.py 的 master-check 子命令。

不读语料、不读外部文件、不调 ffmpeg（编码回测用注入的假函数；几条用例把合成信号写进临时目录再量，用完即删）。
用法：python tools/render/delivery_check_selfcheck.py（在哪个目录跑都行）
每条检查一行 ok / FAIL；全部通过退出码 0，任何一条 FAIL 或抛异常退出码 1。
依赖 numpy / scipy / soundfile / pyloudnorm，版本见 docs/validation/2026-10-08-T085-codec-tp/requirements.txt。
覆盖：已知 TP 超限 -> FAIL；已知削波 -> FAIL；DC / 首尾静音 / 低 I / 纯 M -> 只报且值正确；全部干净 -> PASS；
退出码 0 / 1 / 2；整列没测出来 -> FAIL；Apple 参考缺文件 -> 只写备注不判。
"""
import contextlib
import csv
import importlib
import io
import json
import math
from pathlib import Path
import sys
import tempfile

import numpy as np
import soundfile as sf

# 与 codec_tp_check.py 同目录，不依赖 cwd；用 importlib 导入，保证上面的 sys.path 设置先于导入生效
sys.path.insert(0, str(Path(__file__).resolve().parent))
meters = importlib.import_module('codec_tp_check')

SR = 48000
SECONDS = 12
CHECKS = []
TARGET_LUFS = -14.0      # 干净样本的整曲响度约 -14 LUFS（见 clean_master 的注释），自检里统一用它当目标
PROFILE = meters.build_profile('streaming', TARGET_LUFS)


def check(func):
    CHECKS.append(func)
    return func


def near(value, expected, tol):
    return abs(value - expected) <= tol   # NaN 比较为 False，算不通过


# ---------------------------------------------------------------- 合成信号

def tone(freq, seconds, amp, sr=SR, phase=0.0):
    t = np.arange(int(round(seconds * sr))) / sr
    return amp * np.sin(2 * np.pi * freq * t + phase)


def stereo(left, right=None):
    return np.column_stack((left, left if right is None else right))


def clean_master(seconds=SECONDS, sr=SR, seed=96, mono=False):
    """一个「什么都对」的 48 kHz 双声道成品，数值都能手算：
    中间声道 m = 960 Hz 脉冲（每 0.5 s 一个 62.5 ms、占空比 1/8、幅度 0.5、整数个周期）+ 100 Hz 连续音（幅度 0.02，整数个周期）；
    L = 1.1·m + 噪声，R = 0.9·m + 噪声（噪声标准差 0.004，固定种子，给谱斜率一点宽带内容）。
    手算：3 s 窗恰含 6 个脉冲，S 约 -15.0 LUFS；整曲 I 只有含脉冲的 400 ms 块过相对门，约 -14.05 LUFS；
    采样峰值约 0.59（-4.6 dBFS），真峰值不超过 -4 dBTP，PSR 约 10.5 dB；S/M 约 -19.8 dB；
    100 Hz 同相，低频相关度约 0.98（单声道损失约 -0.04 dB）；零均值，首尾都没有静音。
    mono=True 时 L = R（含噪声也相同），侧信号恰为 0。"""
    n = int(seconds * sr)
    t = np.arange(n) / sr
    gate = (np.arange(n) % (sr // 2)) < (sr // 16)
    mid = 0.5 * np.sin(2 * np.pi * 960 * t) * gate + 0.02 * np.sin(2 * np.pi * 100 * t)
    rng = np.random.default_rng(seed)
    noise = 0.004 * rng.standard_normal((n, 2))
    if mono:
        return stereo(mid + noise[:, 0])
    return np.column_stack((1.1 * mid + noise[:, 0], 0.9 * mid + noise[:, 1]))


def quarter_rate_sine(true_peak_dbtp, seconds=SECONDS):
    """fs/4 正弦、相位 45 度：采样点恒为 ±0.7071·A，真峰值 = A（落在两个采样点正中），所以采样峰值比真峰值低 3.01 dB。"""
    return stereo(tone(SR / 4, seconds, 10 ** (true_peak_dbtp / 20), phase=math.pi / 4))


@contextlib.contextmanager
def temp_dir():
    with tempfile.TemporaryDirectory() as folder:
        yield Path(folder)


def write_wav(folder, name, data, sr=SR, subtype='PCM_24'):
    path = Path(folder) / name
    sf.write(str(path), data, sr, subtype=subtype)
    return path


def evaluate(path, profile=PROFILE, annotation=None, codec_keys=(), roundtrip=None, apple_dir=None, missing=()):
    """量 + 判，就是 master-check 对一个文件做的事（不含写文件）。返回 check_master_file 的结果字典，行里已有判定列。"""
    result = meters.check_master_file(path, profile, annotation, codec_keys, roundtrip, apple_dir)
    meters.apply_judgement(result['row'], profile, result['read_error'], missing, result['codec_failures'])
    return result


_cache = {}


def evaluated(kind):
    """几条用例共用的整条链路结果，缓存避免重复做 8 倍过采样。"""
    if kind not in _cache:
        clean = clean_master()
        builders = {
            'clean': lambda: clean,
            'dc': lambda: clean + 0.01,
            'silence': lambda: np.vstack((np.zeros((2 * SR, 2)), clean, np.zeros((4 * SR, 2)))),
            'quiet': lambda: clean * 10 ** (-10 / 20),
            'pure_mid': lambda: clean_master(mono=True),
        }
        with temp_dir() as folder:
            _cache[kind] = evaluate(write_wav(folder, f'{kind}.wav', builders[kind]()))
    return _cache[kind]


def clean_row(**overrides):
    """手工拼的一行「全部干净」的测量结果，给判定函数的纯逻辑用例用。"""
    row = meters.blank_delivery_row()
    row.update(nonfinite_sample_count=0, tp8_dbtp=-5.0, clipped_sample_count=0, target_basis='whole', target_check_lufs=-14.0,
               target_delta_lu=0.0, main_psr_p10_db=9.0, main_psr_median_db=10.0, dc_offset_dbfs=-100.0, lead_silence_s=0.0,
               trail_silence_s=0.5, sample_rate=48000, subtype='PCM_24', mono_lf_loss_db=-0.1, monitor_suspicion='', na_reason='')
    row.update(overrides)
    return row


def judged(row, profile=PROFILE, **kwargs):
    verdict = meters.apply_judgement(row, profile, **kwargs)
    return verdict, row['fail_reasons'], row['info_flags']


def run_cli(*argv):
    """跑 master-check，返回 (退出码, 标准输出 + 标准错误)；argparse 的参数错（SystemExit）也折成退出码。"""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = meters.master_check_main([str(x) for x in argv])
        except SystemExit as exit_:
            code = exit_.code
    return code, out.getvalue() + err.getvalue()


# ---------------------------------------------------------------- 字段表 / 预设 / 参数

@check
def field_table_is_consistent():
    names = meters.DELIVERY_FIELD_NAMES
    blank = meters.blank_delivery_row()
    numeric_blank = all(math.isnan(blank[name]) for name, unit, _ in meters.DELIVERY_FIELDS if unit != 'text')
    ok = (len(set(names)) == len(names) and names[0] == 'file' and names[-1] == 'notes' and list(blank) == list(names)
          and set(meters.MASTER_FIELD_NAMES) <= set(names) and numeric_blank)
    return ok, f'{len(names)} 列，无重复，首列 file、末列 notes，空行的数值列全是 NaN'


@check
def profile_rules():
    streaming, club = meters.build_profile('streaming'), meters.build_profile('club')
    custom = meters.build_profile('club', -6.5, -2.0)
    try:
        meters.build_profile('streaming', None, -3.0)
        refused = False
    except meters.InfrastructureError:
        refused = True
    ok = (streaming['target_lufs'] == -10.0 and streaming['tp_limit_dbtp'] == -2.0 and club['target_lufs'] == -7.0
          and club['tp_limit_dbtp'] == -1.0 and custom['target_lufs'] == -6.5 and custom['tp_limit_dbtp'] == -2.0 and refused)
    return ok, f'流媒体 {streaming}；俱乐部 {club}；自定义 {custom}；流媒体 + --ceiling 被拒 = {refused}'


@check
def codec_list_parsing():
    names = list(meters.CODEC_PRESETS)
    try:
        meters.parse_codec_list('mp3-320,nonsense')
        refused = False
    except meters.InfrastructureError:
        refused = True
    ok = (meters.parse_codec_list('all') == names and meters.parse_codec_list('none') == [] and meters.parse_codec_list('') == []
          and meters.parse_codec_list('opus-160,mp3-320,opus-160') == ['opus-160', 'mp3-320'] and refused
          and names == ['mp3-320', 'aac-256-pns-off', 'aac-256-default', 'opus-160'])
    return ok, f'预设 {names}；all / none / 去重 / 未知预设被拒'


@check
def encode_commands_have_the_expected_shape():
    mp3, pns_off, default, opus = (meters.encode_command('ffmpeg', 'in.wav', key, 'out.x')
                                   for key in ('mp3-320', 'aac-256-pns-off', 'aac-256-default', 'opus-160'))
    decode = meters.decode_command('ffmpeg', 'out.x', 'dec.wav')
    ok = (mp3[mp3.index('-c:a') + 1] == 'libmp3lame' and '320k' in mp3
          and pns_off[pns_off.index('-c:a') + 1] == 'aac' and default[default.index('-c:a') + 1] == 'aac'
          and '-aac_pns' not in default and pns_off[pns_off.index('-aac_pns') + 1] == '0' and '256k' in pns_off and '256k' in default
          and opus[opus.index('-c:a') + 1] == 'libopus' and opus[opus.index('-application') + 1] == 'audio'
          and opus[opus.index('-vbr') + 1] == 'on' and '160k' in opus
          and all(c[-1] == 'out.x' and '-map' in c for c in (mp3, pns_off, default, opus))
          and decode[-2:] == ['pcm_f32le', 'dec.wav'])
    return ok, f'PNS 关: {" ".join(pns_off)}'


@check
def ffmpeg_encoder_list_parsing():
    sample = (' Encoders:\n V..... = Video\n A..... = Audio\n S..... = Subtitle\n ------\n'
              ' V....D a64multi             Multicolor charset for Commodore 64\n'
              ' A....D aac                  AAC (Advanced Audio Coding)\n'
              ' A....D libmp3lame           libmp3lame MP3 (MPEG audio layer 3) (codec mp3)\n'
              ' A....D libopus              libopus Opus (codec opus)\n')
    names = meters.parse_ffmpeg_encoders(sample)
    return names == {'a64multi', 'aac', 'libmp3lame', 'libopus'}, f'解析出 {sorted(names)}（图例里的 A..... = Audio 不算）'


# ---------------------------------------------------------------- 主段 / 标注 / 削波口径 / 谱斜率（纯函数）

@check
def drop_annotation_converts_bars_to_seconds():
    note = {'bpm': 120, 'meter': '4/4', 'sections': [{'kind': 'intro', 'start_bar': 1, 'end_bar': 8},
                                                     {'kind': 'drop', 'start_bar': 9, 'end_bar': 16},
                                                     {'kind': 'drop', 'start_bar': 25, 'end_bar': 32}]}
    spans = meters.drop_spans_seconds(note)
    triple = meters.drop_spans_seconds({'bpm': 90, 'meter': '3/4', 'sections': [{'kind': 'drop', 'start_bar': 1, 'end_bar': 2}]})
    none = meters.drop_spans_seconds({'bpm': 120, 'sections': [{'kind': 'intro', 'start_bar': 1, 'end_bar': 2}]})
    try:
        meters.drop_spans_seconds({'sections': []})
        raised = False
    except ValueError:
        raised = True
    ok = spans == [(16.0, 32.0), (48.0, 64.0)] and near(triple[0][1], 4.0, 1e-9) and none == [] and raised
    return ok, (f'120 BPM 4/4，每小节 2 s：drop 第 9～16、25～32 小节 -> {spans}（期望 [(16, 32), (48, 64)]）；'
                f'3/4 90 BPM 两小节 {triple}；缺 bpm 抛 ValueError = {raised}')


@check
def annotation_lookup_by_name_or_render_prefix():
    table = {'song': {'song': 'song'}, 'song-long': {'song': 'song-long'}}
    ok = (meters.find_annotation(table, 'song') is table['song'] and meters.find_annotation(table, 'song__g100_clay-auto') is table['song']
          and meters.find_annotation(table, 'song-long__g35') is table['song-long'] and meters.find_annotation(table, 'other') is None)
    return ok, '整名相等 / 「歌名__g…」前缀 / 取最长前缀 / 找不到 None'


@check
def main_windows_follow_annotation():
    short = np.full(100, -12.0)   # 100 个窗，起点 0, 0.1 ... 9.9 s，每窗 3 s
    mask, basis, note = meters.select_main_windows(short, 0.1, 3.0, [(2.0, 8.0)])
    chosen = np.flatnonzero(mask)   # 起点 2.0 ～ 5.0 s 的窗完整落在 [2, 8] 里：第 20 ～ 50 个，共 31 个
    ok = basis == 'drop' and len(chosen) == 31 and (int(chosen[0]), int(chosen[-1])) == (20, 50) and note == ''
    return ok, f'依据 {basis}，窗数 {len(chosen)}（期望 31），范围 {chosen[:1]}～{chosen[-1:]}（期望 20～50）'


@check
def main_windows_fall_back_to_loudest():
    short = np.concatenate((np.full(80, -20.0), np.full(20, -10.0)))
    loudest, basis, note = meters.select_main_windows(short, 0.1, 3.0, None)               # 没标注：P95 = -10，取不低于 -13 的窗
    no_drop, basis2, note2 = meters.select_main_windows(short, 0.1, 3.0, [])               # 标注里没有 drop 段：回退并写备注
    too_short, basis3, note3 = meters.select_main_windows(short, 0.1, 3.0, [(0.0, 2.0)])   # drop 段放不下一个 3 s 窗
    with_silence = np.concatenate((np.full(50, -300.0), np.full(10, -10.0)))
    quiet_mask, basis4, _ = meters.select_main_windows(with_silence, 0.1, 3.0, None)       # 静音窗永远不进主段
    silent_mask, basis5, note5 = meters.select_main_windows(np.full(10, -300.0), 0.1, 3.0, None)
    ok = (basis == 'loudest' and int(loudest.sum()) == 20 and note == ''
          and basis2 == 'loudest' and int(no_drop.sum()) == 20 and 'drop' in note2
          and basis3 == 'loudest' and '放不下' in note3 and int(too_short.sum()) == 20
          and basis4 == 'loudest' and int(quiet_mask.sum()) == 10 and not quiet_mask[:50].any()
          and basis5 == '' and not silent_mask.any() and note5 != '')
    return ok, f'无标注 {int(loudest.sum())} 窗；无 drop 段 {basis2}/{note2!r}；放不下 {note3!r}；全静音依据 {basis5!r}'


@check
def sample_spans_merge_overlapping_windows():
    mask = np.zeros(10, dtype=bool)
    mask[[1, 2, 3, 7]] = True
    spans = meters.merged_sample_spans(mask, 10, 30, 100)   # 窗 1 -> [10,40]，窗 2 -> [20,50]，窗 3 -> [30,60]；窗 7 -> [70,100]
    return spans == [(10, 60), (70, 100)], f'区间 {spans}（期望 [(10, 60), (70, 100)]）'


@check
def main_stats_are_energy_mean_and_psr_over_chosen_windows():
    short = np.array([-10.0, -10.0, -20.0, -4.0])
    peaks = np.array([0.0, -2.0, 0.0, 0.0])
    mask = np.array([True, True, False, False])
    main_s, p10, median = meters.main_section_stats(short, peaks, mask)
    empty = meters.main_section_stats(short, peaks, np.zeros(4, dtype=bool))
    # 选中的两窗：PSR = 峰值 - S = [10, 8]；P10 = 8.2（线性插值），中位 = 9
    ok = near(main_s, -10.0, 1e-9) and near(p10, 8.2, 1e-9) and near(median, 9.0, 1e-9) and all(math.isnan(v) for v in empty)
    return ok, f'主段 S {main_s:.4f}（期望 -10）、PSR P10 {p10:.4f}（期望 8.2）、中位 {median:.4f}（期望 9）；没有窗全 NaN = {all(math.isnan(v) for v in empty)}'


@check
def clipped_count_depends_on_file_type():
    top24, top16 = 1.0 - 2.0 ** -23, 1.0 - 2.0 ** -15
    pcm24 = meters.clipped_sample_count(np.array([[0.5, top24], [-1.0, 0.99]]), 'PCM_24')       # 贴满刻度的 2 个
    pcm16 = meters.clipped_sample_count(np.array([[top16, 0.99], [0.5, -0.25]]), 'PCM_16')      # 1 个
    float_ = meters.clipped_sample_count(np.array([[1.2, 1.0], [-1.5, 0.3]]), 'FLOAT')         # 正好 1.0 不算：2 个
    return (pcm24, pcm16, float_) == (2, 1, 2), f'PCM_24 {pcm24}（期望 2）、PCM_16 {pcm16}（期望 1）、FLOAT {float_}（期望 2）'


@check
def spectral_slope_known_answers():
    n = 20 * SR
    white = np.random.default_rng(7).standard_normal((n, 2))
    spectrum = np.fft.rfft(white, axis=0)
    frequency = np.fft.rfftfreq(n, 1 / SR)
    pink_spectrum = spectrum.copy()
    pink_spectrum[1:] /= np.sqrt(frequency[1:])[:, None]               # 幅度 ∝ 1/√f，功率谱 ∝ 1/f = 粉红噪声
    pink = np.fft.irfft(pink_spectrum, n=n, axis=0)
    steeper = np.fft.irfft(pink_spectrum / np.sqrt(np.maximum(frequency, 1.0))[:, None], n=n, axis=0)   # 再多衰减 1/f（功率）
    flat_slope = meters.spectral_slope_db_per_oct(white, SR)           # 白噪声：1/3 倍频程段的总功率每倍频程 +3 dB
    pink_slope = meters.spectral_slope_db_per_oct(pink, SR)            # 粉红噪声：0
    steeper_slope = meters.spectral_slope_db_per_oct(steeper, SR)      # 功率谱 ∝ 1/f²：段总功率每倍频程 -3 dB
    single = meters.spectral_slope_db_per_oct(stereo(tone(1000, 4, 0.5)), SR)    # 单一正弦：进回归的段不足 6 个 -> NaN
    short = meters.spectral_slope_db_per_oct(stereo(tone(1000, 0.1, 0.5)), SR)   # 短于一个 Welch 段 -> NaN
    ok = (near(flat_slope, 3.0, 0.3) and near(pink_slope, 0.0, 0.3) and near(steeper_slope, -3.0, 0.3)
          and math.isnan(single) and math.isnan(short))
    return ok, f'白噪声 {flat_slope:.3f}（期望 +3）、粉红 {pink_slope:.3f}（期望 0）、再衰减 {steeper_slope:.3f}（期望 -3）；单音 {single}、过短 {short}（期望 NaN）'


# ---------------------------------------------------------------- 判定（手工拼的行，纯逻辑）

@check
def judge_clean_row_passes():
    verdict, fails, infos = judged(clean_row())
    return verdict == 'PASS' and fails == '' and infos == '', f'{verdict} / {fails!r} / {infos!r}'


@check
def judge_true_peak_boundary():
    limit = meters.STREAMING_TP_LIMIT_DBTP + meters.TP_LIMIT_TOLERANCE_DB
    at_limit = judged(clean_row(tp8_dbtp=limit))
    over = judged(clean_row(tp8_dbtp=limit + 0.011))
    club_default = judged(clean_row(tp8_dbtp=-1.5), meters.build_profile('club'))             # 俱乐部 -1.0：-1.5 过
    club_over = judged(clean_row(tp8_dbtp=-0.5), meters.build_profile('club'))                # -0.5 > -0.99：失败
    club_ceiling = judged(clean_row(tp8_dbtp=-0.5), meters.build_profile('club', None, -0.4))   # Ceiling -0.4：-0.5 过
    ok = (at_limit[0] == 'PASS' and over[0] == 'FAIL' and '真峰值' in over[1]
          and club_default[0] == 'PASS' and club_over[0] == 'FAIL' and club_ceiling[0] != 'FAIL')
    return ok, f'流媒体上限 +0.01 内 {at_limit[0]}、超 0.011 {over[0]}；俱乐部 -1.5 {club_default[0]}、-0.5 {club_over[0]}、Ceiling -0.4 时 {club_ceiling[0]}'


@check
def judge_other_fail_items():
    clipped = judged(clean_row(clipped_sample_count=3))
    unreadable = judged(meters.blank_delivery_row(), read_error='LibsndfileError: bad header')
    nonfinite = judged(clean_row(nonfinite_sample_count=2))
    missing = judged(clean_row(), missing_codecs=['mp3-320'])
    ok = (clipped[0] == 'FAIL' and '削波 3' in clipped[1] and unreadable[0] == 'FAIL' and '读不了' in unreadable[1]
          and nonfinite[0] == 'FAIL' and '非有限' in nonfinite[1] and missing[0] == 'FAIL' and 'MP3 320' in missing[1])
    return ok, f'削波 {clipped[:2]}；读不了 {unreadable[:2]}；非有限 {nonfinite[:2]}；缺编码器 {missing[:2]}'


@check
def judge_report_only_items_never_fail():
    aac_tp, aac_over = meters.codec_column_names('aac-256-default')
    apple_tp, apple_over = meters.codec_column_names(meters.APPLE_AAC_KEY)
    cases = {
        'codec TP': clean_row(**{aac_tp: 0.4, aac_over: 2.4}),
        'Apple TP': clean_row(**{apple_tp: 0.2, apple_over: 1.0}),
        'target': clean_row(target_delta_lu=-1.6, target_check_lufs=-15.6),
        'PSR P10': clean_row(main_psr_p10_db=5.0),
        'PSR median': clean_row(main_psr_median_db=7.0),
        'DC': clean_row(dc_offset_dbfs=-40.0),
        'lead silence': clean_row(lead_silence_s=2.5),
        'trail silence': clean_row(trail_silence_s=6.0),
        'sample rate': clean_row(sample_rate=96000),
        'bit depth': clean_row(subtype='FLOAT'),
        'mono loss': clean_row(mono_lf_loss_db=-math.inf),
        'monitor': clean_row(monitor_suspicion='S/M -80.0 dB 低于 -60 dB'),
        'na_reason': clean_row(na_reason='LRA：没有足够的短期响度值'),
    }
    results = {name: judged(row) for name, row in cases.items()}
    flagged = all(v[0] == 'INFO' and v[1] == '' for v in results.values())
    keywords = {'codec TP': 'AAC 256（ffmpeg 默认）', 'Apple TP': 'Apple AAC', 'target': '距目标', 'PSR P10': 'PSR P10', 'PSR median': 'PSR 中位',
                'DC': '直流', 'lead silence': '开头静音', 'trail silence': '结尾静音', 'sample rate': '采样率', 'bit depth': '位深',
                'mono loss': '单声道低频', 'monitor': '临时监听'}
    named = all(word in results[name][2] for name, word in keywords.items())
    return flagged and named, f'全部只报不判：{flagged}；提示文字都带关键词：{named}'


@check
def judge_unmeasured_values_do_not_trigger():
    # 没给 Apple 参考、算不出的项（NaN）既不判失败也不标黄；PSR 下限按交付档取不同的数
    verdict = judged(clean_row())[0]
    club_psr = judged(clean_row(main_psr_p10_db=6.2, main_psr_median_db=6.8), meters.build_profile('club'))    # 俱乐部下限 6 / 6.5：过
    streaming_psr = judged(clean_row(main_psr_p10_db=6.2, main_psr_median_db=6.8))                              # 流媒体下限 7 / 8：标黄
    ok = verdict == 'PASS' and club_psr[0] == 'PASS' and streaming_psr[0] == 'INFO'
    return ok, f'无 Apple 列 {verdict}；PSR 6.2 / 6.8：俱乐部 {club_psr[0]}、流媒体 {streaming_psr[0]}'


@check
def ep_row_reports_ranges_only():
    def song(main_s, slope, psr):
        return clean_row(main_s_lufs=main_s, spectral_slope_db_per_oct=slope, main_psr_median_db=psr)
    tight = meters.build_ep_row([song(-9.8, -2.2, 9.0), song(-10.2, -2.3, 9.5)], PROFILE)       # 极差 0.4 LU / 0.1 / 0.5：都在上限内
    loose = meters.build_ep_row([song(-9.0, -2.0, 8.0), song(-11.0, -2.5, 10.0)], PROFILE)      # 极差 2.0 LU / 0.5 / 2.0：三项都超
    single = meters.build_ep_row([song(-10.0, -2.0, 9.0), clean_row()], PROFILE)                # 只有 1 首有值：算不出
    ok = (tight['verdict'] == 'PASS' and near(tight['ep_main_s_range_lu'], 0.4, 1e-9) and near(tight['ep_slope_range_db_per_oct'], 0.1, 1e-9)
          and near(tight['ep_psr_median_range_db'], 0.5, 1e-9)
          and loose['verdict'] == 'INFO' and loose['info_flags'].count('超过') == 3 and loose['file'] == meters.EP_ROW_NAME
          and single['verdict'] == 'INFO' and math.isnan(single['ep_main_s_range_lu']) and '至少 2 首' in single['na_reason'])
    return ok, f'紧 {tight["verdict"]} / 松 {loose["verdict"]}（{loose["info_flags"]}）/ 单首 {single["verdict"]}（{single["na_reason"]}）'


@check
def column_missing_only_when_all_eligible_files_failed():
    fine = {'row': clean_row(), 'read_error': '', 'codec_failures': {}}
    failed_one = {'row': clean_row(), 'read_error': '', 'codec_failures': {'opus-160': 'ffmpeg 失败：x'}}
    unreadable = {'row': meters.blank_delivery_row(), 'read_error': 'bad', 'codec_failures': {}}
    keys = ['mp3-320', 'opus-160']
    partial = meters.missing_codec_columns([fine, failed_one], keys, keys)        # 一个成功一个失败：不算整列没测出来
    whole = meters.missing_codec_columns([failed_one], keys, keys)                # 能测的文件全失败：整列没测出来
    no_encoder = meters.missing_codec_columns([fine], keys, ['mp3-320'])          # ffmpeg 没有 libopus
    nothing_eligible = meters.missing_codec_columns([unreadable], keys, keys)     # 一个能测的都没有：不下结论
    verdict, fails, infos = judged(clean_row(), missing_codecs=whole, codec_failures={'opus-160': 'x'})
    partial_verdict, _, partial_infos = judged(clean_row(), codec_failures={'opus-160': 'x'})
    ok = (partial == [] and whole == ['opus-160'] and no_encoder == ['opus-160'] and nothing_eligible == []
          and verdict == 'FAIL' and '整列' in fails and infos == '' and partial_verdict == 'INFO' and 'Opus' in partial_infos)
    return ok, f'部分失败 {partial}、全失败 {whole}、缺编码器 {no_encoder}、无可测文件 {nothing_eligible}；FAIL 原因 {fails!r}'


# ---------------------------------------------------------------- 整条链路（合成音频写进临时 WAV，不调 ffmpeg）

@check
def clean_master_passes():
    row = evaluated('clean')['row']
    ok = (row['verdict'] == 'PASS' and row['fail_reasons'] == '' and row['info_flags'] == '' and row['na_reason'] == ''
          and near(row['lufs_i'], -14.0, 0.8) and row['tp8_dbtp'] < -4.0 and row['clipped_sample_count'] == 0
          and row['main_basis'] == 'loudest' and row['target_basis'] == 'whole' and math.isfinite(row['spectral_slope_db_per_oct'])
          and near(row['main_psr_median_db'], 10.0, 1.5) and near(row['side_mid_db'], -20.0, 1.0) and row['lead_silence_s'] < 0.05)
    return ok, (f"{row['verdict']}；I {row['lufs_i']:.2f}（期望约 -14）、TP {row['tp8_dbtp']:.2f}、PSR 中位 {row['main_psr_median_db']:.2f}（约 10）、"
                f"S/M {row['side_mid_db']:.2f}；失败={row['fail_reasons']!r} 提示={row['info_flags']!r} 算不出={row['na_reason']!r}")


@check
def true_peak_over_streaming_limit_fails_even_when_samples_look_safe():
    with temp_dir() as folder:
        row = evaluate(write_wav(folder, 'hot.wav', quarter_rate_sine(-1.0)))['row']
    sample_peak = row['sample_peak_dbfs']
    ok = (row['verdict'] == 'FAIL' and '真峰值' in row['fail_reasons'] and near(row['tp8_dbtp'], -1.0, 0.1)
          and near(sample_peak, -1.0 - 3.01, 0.1) and row['clipped_sample_count'] == 0 and near(row['tp_margin_db'], -1.0, 0.1))
    return ok, f"{row['verdict']}；TP {row['tp8_dbtp']:.3f}（期望 -1.0）、采样峰值 {sample_peak:.3f}（期望 -4.01）、余量 {row['tp_margin_db']:.3f}；{row['fail_reasons']!r}"


@check
def club_uses_its_own_ceiling():
    with temp_dir() as folder:
        near_ceiling = write_wav(folder, 'p05.wav', quarter_rate_sine(-0.5))
        minus_one = write_wav(folder, 'p10.wav', quarter_rate_sine(-1.0))
        club_default = evaluate(near_ceiling, meters.build_profile('club'))['row']                    # -0.5 > -1.0 + 0.01：失败
        club_roomy = evaluate(minus_one, meters.build_profile('club', None, -0.5))['row']             # Ceiling -0.5：-1.0 在内
        streaming_same = evaluate(minus_one, meters.build_profile('streaming'))['row']                # 同一文件按流媒体 -2.0：失败
    ok = club_default['verdict'] == 'FAIL' and club_roomy['fail_reasons'] == '' and streaming_same['verdict'] == 'FAIL'
    return ok, f"俱乐部默认 {club_default['verdict']}；Ceiling -0.5 的失败原因 {club_roomy['fail_reasons']!r}；同文件流媒体 {streaming_same['verdict']}"


@check
def clipping_fails_for_float_and_integer_files():
    sine = stereo(tone(1000, 4, 1.2))
    saturated = np.clip(stereo(tone(1000, 4, 1.5)), -1.0, 1.0 - 2.0 ** -23)
    with temp_dir() as folder:
        as_float = evaluate(write_wav(folder, 'float.wav', sine, subtype='FLOAT'))['row']
        as_int = evaluate(write_wav(folder, 'int24.wav', saturated, subtype='PCM_24'))['row']
        quiet = evaluate(write_wav(folder, 'ok.wav', stereo(tone(1000, 4, 0.3)), subtype='PCM_24'))['row']
    ok = (as_float['verdict'] == 'FAIL' and as_float['clipped_sample_count'] > 0 and '削波' in as_float['fail_reasons']
          and as_int['verdict'] == 'FAIL' and as_int['clipped_sample_count'] > 0 and '削波' in as_int['fail_reasons']
          and quiet['clipped_sample_count'] == 0 and '削波' not in quiet['fail_reasons'])
    return ok, f"浮点 {as_float['clipped_sample_count']} 个、24 bit 贴满刻度 {as_int['clipped_sample_count']} 个、干净 {quiet['clipped_sample_count']} 个"


@check
def dc_offset_is_reported_not_failed():
    row = evaluated('dc')['row']
    ok = row['verdict'] == 'INFO' and row['fail_reasons'] == '' and '直流' in row['info_flags'] and near(row['dc_offset_dbfs'], -40.0, 0.1)
    return ok, f"{row['verdict']}；DC {row['dc_offset_dbfs']:.3f} dBFS（期望 -40）；提示 {row['info_flags']!r}"


@check
def leading_and_trailing_silence_are_reported_not_failed():
    row = evaluated('silence')['row']
    ok = (row['verdict'] == 'INFO' and row['fail_reasons'] == '' and '开头静音' in row['info_flags'] and '结尾静音' in row['info_flags']
          and near(row['lead_silence_s'], 2.0, 0.01) and near(row['trail_silence_s'], 4.0, 0.01))
    return ok, f"{row['verdict']}；开头 {row['lead_silence_s']:.4f} s（期望 2.0）、结尾 {row['trail_silence_s']:.4f} s（期望 4.0）"


@check
def low_loudness_is_reported_with_monitor_suspicion():
    row = evaluated('quiet')['row']
    ok = (row['verdict'] == 'INFO' and row['fail_reasons'] == '' and near(row['target_delta_lu'], -10.0, 1.0)
          and '距目标' in row['info_flags'] and '临时监听' in row['info_flags'] and row['monitor_suspicion'] != '')
    return ok, f"{row['verdict']}；距目标 {row['target_delta_lu']:.2f} LU（期望约 -10）；监听嫌疑 {row['monitor_suspicion']!r}"


@check
def pure_mid_is_reported_as_monitor_suspicion():
    row = evaluated('pure_mid')['row']
    ok = (row['verdict'] == 'INFO' and row['fail_reasons'] == '' and row['side_mid_db'] == -math.inf
          and 'S/M' in row['monitor_suspicion'] and '临时监听' in row['info_flags'] and near(row['mono_lf_loss_db'], 0.0, 1e-6))
    return ok, f"{row['verdict']}；S/M {row['side_mid_db']}；单声道低频损失 {row['mono_lf_loss_db']:.6f}；{row['monitor_suspicion']!r}"


@check
def unreadable_and_nonfinite_files_fail_without_stopping():
    data = clean_master()
    data[1000, 0] = math.nan
    with temp_dir() as folder:
        broken = folder / 'broken.wav'
        broken.write_bytes(b'this is not a wav file')
        unreadable = evaluate(broken)
        nonfinite = evaluate(write_wav(folder, 'nan.wav', data, subtype='FLOAT'))
        good = evaluate(write_wav(folder, 'good.wav', clean_master()))
    ok = (unreadable['row']['verdict'] == 'FAIL' and unreadable['read_error'] != '' and '读不了' in unreadable['row']['fail_reasons']
          and nonfinite['row']['verdict'] == 'FAIL' and '非有限' in nonfinite['row']['fail_reasons']
          and math.isnan(nonfinite['row']['lufs_i']) and good['row']['verdict'] == 'PASS')
    return ok, f"读不了 {unreadable['row']['fail_reasons']!r}；NaN 文件 {nonfinite['row']['fail_reasons']!r}；其后的文件 {good['row']['verdict']}"


@check
def mono_file_is_reported_not_zero():
    with temp_dir() as folder:
        row = evaluate(write_wav(folder, 'mono.wav', tone(1000, 5, 0.3), subtype='PCM_24'))['row']
    ok = (row['channels'] == 1 and math.isnan(row['tp8_dbtp']) and row['na_reason'] != '' and row['fail_reasons'] == ''
          and row['verdict'] == 'INFO' and '真峰值未测' in row['info_flags'])
    return ok, f"{row['verdict']}；TP {row['tp8_dbtp']}；na_reason {row['na_reason']!r}；提示 {row['info_flags']!r}"


@check
def annotation_drives_main_section_and_target_check():
    note = {'song': 'annotated', 'bpm': 120, 'meter': '4/4',
            'sections': [{'name': 'intro', 'kind': 'intro', 'start_bar': 1, 'end_bar': 1},
                         {'name': 'drop', 'kind': 'drop', 'start_bar': 2, 'end_bar': 5}]}   # 每小节 2 s：drop = 2 ～ 10 s
    profile = meters.build_profile('streaming', -15.0)   # drop 段的 S 约 -15.0（整曲 I 约 -14.05），目标按它取
    with temp_dir() as folder:
        path = write_wav(folder, 'annotated.wav', clean_master())
        (folder / 'annotated.json').write_text(json.dumps(note), encoding='utf-8')
        table = meters.load_annotation_table([folder])
        row = evaluate(path, profile, annotation=meters.find_annotation(table, path.stem))['row']
    ok = (row['main_basis'] == 'drop' and near(row['main_seconds'], 8.0, 0.01) and row['target_basis'] == 'drop'
          and near(row['target_check_lufs'], row['main_s_lufs'], 1e-9) and near(row['main_s_lufs'], -15.0, 0.8) and row['verdict'] == 'PASS')
    return ok, (f"依据 {row['main_basis']}，主段 {row['main_seconds']:.3f} s（期望 8.0），主段 S {row['main_s_lufs']:.2f}（期望约 -15），"
                f"目标核对依据 {row['target_basis']}，判定 {row['verdict']}，提示 {row['info_flags']!r}")


@check
def codec_columns_with_injected_roundtrip_and_apple_reference():
    def fake_roundtrip(source, key):
        if key == 'opus-160':
            raise RuntimeError('boom')
        return {'mp3-320': -2.5, 'aac-256-pns-off': 0.5}[key]     # 只有 AAC 解码后超过满刻度

    keys = ['mp3-320', 'aac-256-pns-off', 'opus-160']
    with temp_dir() as folder:
        masters, apple = folder / 'masters', folder / 'apple'
        masters.mkdir()
        apple.mkdir()
        with_apple = write_wav(masters, 'a.wav', clean_master())
        without_apple = write_wav(masters, 'b.wav', clean_master(seed=97))
        write_wav(apple, 'a.wav', stereo(tone(1000, 4, 1.2)), subtype='FLOAT')      # Apple 解回的 wav：真峰值约 +1.6 dBTP
        first = meters.check_master_file(with_apple, PROFILE, None, keys, fake_roundtrip, apple)
        second = meters.check_master_file(without_apple, PROFILE, None, keys, fake_roundtrip, apple)
    missing = meters.missing_codec_columns([first, second], keys, keys)
    for result in (first, second):
        meters.apply_judgement(result['row'], PROFILE, result['read_error'], missing, result['codec_failures'])
    a, b = first['row'], second['row']
    mp3_col, aac_col, opus_col = (meters.codec_column_names(k) for k in keys)
    apple_col = meters.codec_column_names(meters.APPLE_AAC_KEY)
    ok = (near(a[aac_col[0]], 0.5, 1e-9) and near(a[aac_col[1]], 0.5 - a['tp8_dbtp'], 1e-9) and near(a[mp3_col[0]], -2.5, 1e-9)
          and math.isnan(a[opus_col[0]]) and 'opus-160' in first['codec_failures'] and missing == ['opus-160']
          and a['verdict'] == 'FAIL' and '整列' in a['fail_reasons']
          and 'AAC 256（PNS 关）' in a['info_flags'] and 'MP3 320' not in a['info_flags'] and 'Apple AAC' in a['info_flags']
          and a[apple_col[0]] > 1.0
          and math.isnan(b[apple_col[0]]) and '未测' in b['notes'] and 'Apple' in b['notes'] and 'Apple' not in b['fail_reasons'])
    return ok, (f"AAC 解码后 TP {a[aac_col[0]]:.2f}、Apple {a[apple_col[0]]:.2f}；整列没测出来 {missing}；"
                f"A 的失败 {a['fail_reasons']!r}；B 缺 Apple 的备注 {b['notes']!r}")


# ---------------------------------------------------------------- 命令行：退出码与输出文件

@check
def cli_exit_codes_and_output_files():
    with temp_dir() as folder:
        good_dir, mixed_dir = folder / 'good', folder / 'mixed'
        good_dir.mkdir()
        mixed_dir.mkdir()
        write_wav(good_dir, 'good.wav', clean_master())
        write_wav(mixed_dir, 'good.wav', clean_master())
        write_wav(mixed_dir, 'hot.wav', quarter_rate_sine(-1.0))
        write_wav(mixed_dir, '._hidden.wav', clean_master())               # macOS 的 ._ 资源叉文件应被跳过
        common = ['--delivery', 'streaming', '--target-lufs', '-14', '--codecs', 'none']
        code_ok, _ = run_cli('--in', good_dir, *common, '--out', folder / 'out_ok')
        code_fail, _ = run_cli('--in', mixed_dir, *common, '--out', folder / 'out_fail')
        out = folder / 'out_fail'
        files_ok = all((folder / 'out_ok' / name).is_file() for name in ('delivery_check.csv', 'delivery_check.md', 'environment.json'))
        with (out / 'delivery_check.csv').open(newline='', encoding='utf-8') as stream:
            reader = csv.DictReader(stream)
            rows = list(reader)
            header = reader.fieldnames
        markdown = (out / 'delivery_check.md').read_text(encoding='utf-8')
        environment = json.loads((out / 'environment.json').read_text(encoding='utf-8'))
    by_name = {r['file']: r for r in rows}
    ok = (code_ok == 0 and code_fail == 1 and files_ok and header == list(meters.DELIVERY_FIELD_NAMES)
          and sorted(by_name) == sorted(['good.wav', 'hot.wav', meters.EP_ROW_NAME]) and by_name['hot.wav']['verdict'] == 'FAIL'
          and by_name['good.wav']['verdict'] == 'PASS' and by_name[meters.EP_ROW_NAME]['verdict'] == 'INFO'
          and float(by_name[meters.EP_ROW_NAME]['ep_main_s_range_lu']) > 1.0
          and '# 交付检查单' in markdown and 'EP 汇总' in markdown and '**FAIL**' in markdown and 'hot.wav' in markdown and '判定表' in markdown
          and environment['ffmpeg_version'] is None and len(environment['script_sha256']) == 64 and environment['verdict_counts']['FAIL'] == 1
          and environment['packages']['numpy'] and '._hidden.wav' not in markdown)
    return ok, f'全好 -> {code_ok}（期望 0），有 FAIL -> {code_fail}（期望 1）；CSV 行 {sorted(by_name)}'


@check
def cli_infrastructure_errors_exit_2():
    with temp_dir() as folder:
        good = folder / 'good'
        good.mkdir()
        write_wav(good, 'good.wav', clean_master(seconds=4))
        empty_dir = folder / 'empty'
        empty_dir.mkdir()
        base = ['--delivery', 'streaming', '--codecs', 'none', '--out', folder / 'out']
        codes = {
            'missing_input': run_cli('--in', folder / 'nope', *base)[0],
            'no_wav': run_cli('--in', empty_dir, *base)[0],
            'ceiling_on_streaming': run_cli('--in', good, *base, '--ceiling', '-3')[0],
            'missing_annotation': run_cli('--in', good, *base, '--annotations', folder / 'nope.json')[0],
            'unknown_codec': run_cli('--in', good, '--delivery', 'streaming', '--codecs', 'nonsense', '--out', folder / 'out')[0],
            'no_ffmpeg': run_cli('--in', good, '--delivery', 'streaming', '--ffmpeg', folder / 'no-such-ffmpeg', '--out', folder / 'out')[0],
            'missing_apple_dir': run_cli('--in', good, *base, '--apple-aac-dir', folder / 'nope')[0],
            'missing_args': run_cli('--in', good)[0],
        }
    return all(code == 2 for code in codes.values()), f'退出码 {codes}（期望全是 2）'


@check
def cli_dry_run_writes_nothing():
    with temp_dir() as folder:
        write_wav(folder, 'good.wav', clean_master(seconds=4))
        out = folder / 'not_created'
        code, text = run_cli('--in', folder, '--delivery', 'club', '--codecs', 'none', '--out', out, '--dry-run')
        created = out.exists()
    ok = code == 0 and not created and '[dry-run]' in text and 'good.wav' in text and 'delivery_check.csv' in text
    return ok, f'退出码 {code}，输出目录被创建 = {created}（期望 False）'


# ---------------------------------------------------------------- 输出格式

@check
def csv_cells_and_markdown_rendering():
    cells = [meters._csv_cell(v) for v in (math.nan, math.inf, -math.inf, 3, 1.23456, 'abc')]
    good = clean_row(file='good.wav', delivery='streaming', target_lufs=-14.0, tp_limit_dbtp=-2.0, lufs_i=-14.0, main_basis='drop',
                     main_s_lufs=-15.0, tp8_dbtp=-4.6, side_mid_db=-math.inf)
    bad = clean_row(file='bad|pipe.wav', delivery='streaming', target_lufs=-14.0, tp_limit_dbtp=-2.0, tp8_dbtp=-1.0, lufs_i=-9.0)
    for row in (good, bad):
        meters.apply_judgement(row, PROFILE, missing_codecs=['mp3-320'])
    ep = meters.build_ep_row([good, bad], PROFILE)
    with temp_dir() as folder:
        meters.write_delivery_csv(folder / 'x.csv', [good, bad, ep])
        with (folder / 'x.csv').open(newline='', encoding='utf-8') as stream:
            rows = list(csv.DictReader(stream))
    text = meters.render_markdown(rows, ['mp3-320', 'aac-256-pns-off'], True)
    ok = (cells == ['', 'inf', '-inf', '3', '1.2346', 'abc'] and rows[0]['side_mid_db'] == '-inf' and rows[1]['fail_reasons'] != ''
          and '未测' in text and 'bad\\|pipe.wav' in text and '**FAIL**' in text and 'EP 汇总（极差）' in text
          and 'Apple AAC 256（参考）' in text and '## 判定详情' in text and '## 判定表' in text and text.count('| 级别 | 条件 |') == 1)
    return ok, f'CSV 格子 {cells}；Markdown {len(text)} 字符'


@check
def readme_lists_every_field():
    readme = Path(__file__).resolve().parents[2] / 'docs' / 'validation' / '2026-10-11-T096c-delivery-check' / 'README.md'
    if not readme.exists():
        return True, f'{readme.name} 不在原位，跳过（字段表以 codec_tp_check.py 为准）'
    text = readme.read_text(encoding='utf-8')
    missing = [name for name in meters.DELIVERY_FIELD_NAMES if f'`{name}`' not in text]
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
