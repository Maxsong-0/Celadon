#!/usr/bin/env python3
"""母带检查单（T-096c2 的通用部分）：直接量成品 wav（不渲染），逐首给 PASS / INFO / FAIL，多首时加一行 EP 汇总。

只报告，不改任何输入文件，不自动回退增益。量具全部来自 measure_lib.py，段落标注的读法来自 section_lib.py，这里不另写一份。
口径、列说明与判定表：docs/validation/2026-10-11-T096c-master-check/README.md；自检：master_check_selfcheck.py。

用法：
  master_check.py --in <wav 或目录> [...] --out <输出目录> [--ceiling -1.0] [--target-lufs -9] [--annotations <JSON 或目录> ...] [--dry-run]
退出码：0 = 没有 FAIL；1 = 至少一首 FAIL；2 = 基础设施错（参数 / 路径 / 标注读不了 / 输出写不了）。

ADR-039 删掉交付档之后这里只留与交付无关的检查：判失败只看真峰值是否超过你给的 Ceiling、原采样点削波、文件读不了或含非有限样本；
原来的流媒体 / 俱乐部上限与目标、编码再解码的真峰值、按交付档取的 PSR 下限都删了（要出流媒体版，把 --ceiling 设成 -2）。
"""
import argparse
import csv
import hashlib
from importlib import metadata as importlib_metadata
import importlib
import json
import math
from pathlib import Path
import platform
import sys
import traceback

import numpy as np
import soundfile as sf

# 同目录的两个库，不依赖 cwd；用 importlib 导入，保证上面的 sys.path 设置先于导入生效
sys.path.insert(0, str(Path(__file__).resolve().parent))
meters = importlib.import_module('measure_lib')
section_lib = importlib.import_module('section_lib')

# ---- 判定项与阈值：全部集中在这一块（README 的判定表与 master_check.md 的判定表都由这里取数）----
# 判定三档：PASS / INFO（只报、标黄）/ FAIL。FAIL 只有三项（Max 2026-10-11 Q9 的四项去掉编码器那一项）：
#   ① PCM 真峰值高于 Ceiling + 容差；② 原采样点削波数 > 0；③ 文件读不了或含非有限样本。其余一律只报（INFO），绝不判失败。
# 注释里写「暂定」的数字是 T-096 任务书没给、按调研报告取的起点值，待 Max 认。
DEFAULT_CEILING_DBTP = -1.0          # --ceiling 不给时的真峰值上限（插件出厂 Ceiling）
TP_LIMIT_TOLERANCE_DB = 0.01         # 真峰值超限的容差，同 gate_check.py 的 +0.01
TARGET_WINDOW_LU = 1.0               # 给了 --target-lufs 时：依据响度距目标超过它标黄
DC_WARN_DBFS = -60.0                 # 直流偏置高于它标黄；暂定
LEAD_SILENCE_WARN_S = 1.0            # 开头静音超过它标黄；暂定
TRAIL_SILENCE_WARN_S = 3.0           # 结尾静音超过它标黄；暂定
MONO_LF_LOSS_WARN_DB = -1.0          # 单声道低频损失低于它标黄（R1 G1 ②）
EXPECTED_SAMPLE_RATES = (44100, 48000)       # 采样率不在此列标黄；暂定
EXPECTED_SUBTYPES = ('PCM_16', 'PCM_24')     # 位深（libsndfile 子类型）不在此列标黄；暂定
EP_MAIN_S_RANGE_MAX_LU = 1.0         # EP 汇总：各曲主段 S 极差上限（R1 G7）
EP_SLOPE_RANGE_MAX_DB_PER_OCT = 0.3  # EP 汇总：各曲谱斜率极差上限（R1 G7）
EP_PSR_MEDIAN_RANGE_MAX_DB = 1.5     # EP 汇总：各曲主段 PSR 中位极差上限（R1 G7）
EP_ROW_NAME = 'EP 汇总'               # CSV / Markdown 里 EP 汇总行的 file 列
CSV_NAME, MD_NAME, ENVIRONMENT_NAME = 'master_check.csv', 'master_check.md', 'environment.json'

# master_check.csv 的字段表：(name, unit, 含义)。unit == 'text' 是字符串列，其余是数值，没算出 / 没测留空，绝不填 0。
_HEAD_FIELDS = (
    ('file', 'text', '文件名（不含目录）；EP 汇总行写「EP 汇总」'),
    ('verdict', 'text', '判定：PASS / INFO / FAIL；有 FAIL 项即 FAIL，否则有只报项或算不出的项即 INFO，否则 PASS'),
    ('fail_reasons', 'text', '判失败的原因，多条用 "; " 连接；空 = 没有失败项'),
    ('info_flags', 'text', '只报不判（标黄）的项，多条用 "; " 连接；空 = 没有'),
    ('target_lufs', 'LUFS', '目标响度（--target-lufs）；没给留空，目标核对与「监听衰减」那条嫌疑都不做'),
    ('tp_limit_dbtp', 'dBTP', 'PCM 真峰值上限 = --ceiling（默认 -1.0）'),
)
_EXTRA_FIELDS = (
    ('clipped_sample_count', 'count', '原采样点削波数（判失败用）：浮点文件 = 样本绝对值大于 1.0 的个数；整数 PCM = 样本贴满刻度码值的个数'),
    ('tp_margin_db', 'dB', '真峰值余量 = tp_limit_dbtp - tp8_dbtp（负 = 超限）'),
    ('target_basis', 'text', '目标核对的依据：drop = 标注的 drop 段短期响度；whole = 整曲 I；空 = 没给目标'),
    ('target_check_lufs', 'LUFS', '目标核对用的响度值（依据见 target_basis）'),
    ('target_delta_lu', 'LU', 'target_check_lufs - target_lufs'),
    ('main_basis', 'text', '主段的定义：drop = 标注里的 drop 段；loudest = 短期响度不低于 P95 - 3 LU 的 3 s 窗；空 = 没有主段'),
    ('main_seconds', 's', '主段窗合并后的总时长'),
    ('main_s_lufs', 'LUFS', '主段 S：主段内各 3 s 窗短期响度（能量平均）'),
    ('main_psr_p10_db', 'dB', '主段内 PSR 序列的 P10（只报数）'),
    ('main_psr_median_db', 'dB', '主段内 PSR 序列的中位数（只报数；EP 极差用）'),
    ('spectral_slope_db_per_oct', 'dB/oct', '主段谱斜率：27 个 1/3 倍频程段电平对 log2 频率的最小二乘斜率，粉红噪声 = 0'),
)
_EP_FIELDS = (
    ('ep_main_s_range_lu', 'LU', '仅 EP 汇总行：各曲主段 S 的极差（最大 - 最小）'),
    ('ep_slope_range_db_per_oct', 'dB/oct', '仅 EP 汇总行：各曲谱斜率的极差'),
    ('ep_psr_median_range_db', 'dB', '仅 EP 汇总行：各曲主段 PSR 中位的极差'),
)
_TAIL_FIELDS = (
    next(field for field in meters.MASTER_FIELDS if field[0] == 'na_reason'),
    ('notes', 'text', '不影响判定的备注（主段改用最响窗口的原因等）'),
)
# 列顺序：判定头 -> measure_master 的字段（去掉 file 和 na_reason）-> 本工具新增 -> EP 极差 -> na_reason / notes
CHECK_FIELDS = (_HEAD_FIELDS
                + tuple(field for field in meters.MASTER_FIELDS if field[0] not in ('file', 'na_reason'))
                + _EXTRA_FIELDS + _EP_FIELDS + _TAIL_FIELDS)
CHECK_FIELD_NAMES = tuple(name for name, _unit, _meaning in CHECK_FIELDS)


class InfrastructureError(Exception):
    """基础设施错（参数 / 路径 / 标注读不了 / 输出写不了）：退出码 2。素材本身的问题写进行里，不走这里。"""


def _short_error(error):
    return f'{type(error).__name__}: {error}'


def _num(value):
    """任何东西 -> float；转不了读 NaN。NaN 参与比较一律为 False，所以算不出的项自动不触发。"""
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


# ---------------------------------------------------------------- 一个成品文件 -> 一行（量，不判）

def blank_row():
    """所有列都在：文字列空串，数值列 NaN（写 CSV 时是空格子，绝不填 0）。"""
    return {name: ('' if unit == 'text' else math.nan) for name, unit, _meaning in CHECK_FIELDS}


def build_profile(target_lufs=None, ceiling=None):
    """检查参数：真峰值上限 = ceiling（默认 -1.0）；target_lufs 不给就是 None（不做目标核对）。"""
    limit = DEFAULT_CEILING_DBTP if ceiling is None else float(ceiling)
    target = None if target_lufs is None else float(target_lufs)
    return {'target_lufs': target, 'tp_limit_dbtp': limit}


def check_master_file(path, profile, annotation=None):
    """量一个成品文件。返回 {'row': 行, 'read_error': 读不了的原因或空串}；不判定（判定在 judge_master）。
    文件读不了不抛异常：记 read_error，由判定给 FAIL，调用者接着查下一个文件。"""
    path = Path(path)
    row = blank_row()
    target = profile['target_lufs']
    row.update(file=path.name, target_lufs=math.nan if target is None else target, tp_limit_dbtp=profile['tp_limit_dbtp'])
    result = {'row': row, 'read_error': ''}
    try:
        measured = meters.measure_master(path, target, profile['tp_limit_dbtp'])
        audio, rate = sf.read(path, always_2d=True, dtype='float64')
    except Exception as error:   # 文件读不了：记原因，继续检查下一个文件
        result['read_error'] = _short_error(error)
        return result
    row.update(measured)
    if row['nonfinite_sample_count']:
        return result   # measure_master 已把测量项留空并写了 na_reason
    reasons = [row['na_reason']] if row['na_reason'] else []
    notes = []
    row['clipped_sample_count'] = meters.clipped_sample_count(audio, row['subtype'])
    row['tp_margin_db'] = profile['tp_limit_dbtp'] - row['tp8_dbtp']

    main_audio, basis = None, ''
    if audio.shape[1] == 2 and len(audio) >= 3 * rate:
        short, peaks_db = meters.windowed_short_term_and_peak_db(audio, rate)
        hop = meters._hop_samples(rate)
        window = meters.PSR_WINDOW_HOPS * hop
        spans = None
        if annotation is not None:
            try:
                spans = section_lib.drop_spans_seconds(annotation)
            except ValueError as error:
                notes.append(f'{error}；主段改按最响窗口')
        mask, basis, main_note = meters.select_main_windows(short, hop / rate, window / rate, spans)
        if main_note:
            (notes if basis else reasons).append(main_note)
        row['main_basis'] = basis
        if basis:
            row['main_s_lufs'], row['main_psr_p10_db'], row['main_psr_median_db'] = meters.main_section_stats(short, peaks_db, mask)
            sample_spans = meters.merged_sample_spans(mask, hop, window, len(audio))
            row['main_seconds'] = sum(end - start for start, end in sample_spans) / rate
            main_audio = np.concatenate([audio[start:end] for start, end in sample_spans])
    if main_audio is not None:
        row['spectral_slope_db_per_oct'] = meters.spectral_slope_db_per_oct(main_audio, rate)
        if math.isnan(row['spectral_slope_db_per_oct']):
            reasons.append(f'谱斜率：主段里高于最响段 -{meters.SLOPE_BAND_GATE_DB:g} dB 的 1/3 倍频程段不足 {meters.SLOPE_MIN_BANDS} 个')
    if target is not None:
        # 目标核对：有 drop 标注就用 drop 段的 S（目标指的是 drop 响度），否则退回整曲 I
        if basis == 'drop':
            row['target_basis'], row['target_check_lufs'] = 'drop', row['main_s_lufs']
        else:
            row['target_basis'], row['target_check_lufs'] = 'whole', row['lufs_i']
        row['target_delta_lu'] = row['target_check_lufs'] - target
    row['na_reason'] = '; '.join(reasons)
    row['notes'] = '; '.join(notes)
    return result


# ---------------------------------------------------------------- 判定（纯函数）

def judge_master(row, profile, read_error=''):
    """一首的判定。返回 (fail 原因列表, info 提示列表)。FAIL 只有三项，其余一律是 info；算不出的项（NaN）不触发。"""
    fails, infos = [], []
    limit = profile['tp_limit_dbtp']
    # ---- FAIL：三项 ----
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
    # ---- INFO：只报不判 ----
    if not read_error and nonfinite == 0 and math.isnan(tp):
        infos.append('PCM 真峰值未测（见 na_reason）')
    delta = _num(row.get('target_delta_lu'))
    if profile['target_lufs'] is not None and abs(delta) > TARGET_WINDOW_LU:
        what = 'drop 段 S' if row.get('target_basis') == 'drop' else '整曲 I'
        infos.append(f"{what} {_num(row.get('target_check_lufs')):.1f} LUFS 距目标 {profile['target_lufs']:.1f} 为 {delta:+.1f} LU"
                     f'（超过 ±{TARGET_WINDOW_LU:g}）')
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
    return fails, infos


def apply_judgement(row, profile, read_error=''):
    """把判定写进行的 fail_reasons / info_flags / verdict 三列并返回 verdict。
    FAIL = 有失败项；INFO = 无失败项但有只报项、或有项目因素材原因算不出（na_reason 非空）；PASS = 其余。"""
    fails, infos = judge_master(row, profile, read_error)
    row['fail_reasons'] = '; '.join(fails)
    row['info_flags'] = '; '.join(infos)
    row['verdict'] = 'FAIL' if fails else ('INFO' if infos or row['na_reason'] else 'PASS')
    return row['verdict']


def build_ep_row(song_rows, profile):
    """EP 汇总行：各曲主段 S、谱斜率、主段 PSR 中位的极差（R1 G7）。超过上限只报（INFO）；不够 2 首有值的写 na_reason。"""
    row = blank_row()
    target = profile['target_lufs']
    row.update(file=EP_ROW_NAME, target_lufs=math.nan if target is None else target, tp_limit_dbtp=profile['tp_limit_dbtp'],
               notes=f'{len(song_rows)} 首参与')

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


def write_check_csv(path, rows):
    with Path(path).open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(CHECK_FIELD_NAMES)
        for row in rows:
            writer.writerow([_csv_cell(row.get(name, '')) for name in CHECK_FIELD_NAMES])


def verdict_rule_lines(tp_limit):
    """判定表（级别, 条件）：写进 Markdown 的「判定表」一节；数字都取自顶部常量表，不会和代码脱节。"""
    return (
        ('FAIL', f'PCM 真峰值（8 倍过采样）高于 {tp_limit:+.1f} dBTP + {TP_LIMIT_TOLERANCE_DB:g} dB'),
        ('FAIL', '原采样点削波数大于 0（浮点文件：样本绝对值大于 1.0；整数 PCM：样本贴满刻度码值）'),
        ('FAIL', '文件读不了，或含非有限样本（NaN / Inf）'),
        ('INFO', f'给了目标时：目标核对（有 drop 标注用 drop 段 S，否则用整曲 I）距目标超过 ±{TARGET_WINDOW_LU:g} LU'),
        ('INFO', f'直流偏置高于 {DC_WARN_DBFS:g} dBFS'),
        ('INFO', f'开头静音超过 {LEAD_SILENCE_WARN_S:g} s，或结尾静音超过 {TRAIL_SILENCE_WARN_S:g} s'
                 f'（静音 = 样本都不超过 {meters.SILENCE_THRESHOLD_DBFS:g} dBFS）'),
        ('INFO', '采样率不是 44.1 / 48 kHz，或位深不是 ' + ' / '.join(EXPECTED_SUBTYPES)),
        ('INFO', f'单声道低频损失低于 {MONO_LF_LOSS_WARN_DB:g} dB'),
        ('INFO', f'疑似带着临时监听导出（整曲 S/M 低于 {meters.SIDE_MID_SUSPECT_DB:g} dB；或给了目标时 I 比目标低 '
                 f'{meters.SUSPECT_LOUDNESS_GAP_LU:g} LU 以上且真峰值比上限低 {meters.SUSPECT_TP_MARGIN_DB:g} dB 以上）'),
        ('INFO', '有项目因素材原因算不出（见 na_reason 列）；LRA、PSR、谱斜率只报数，没有阈值'),
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


def render_markdown(rows):
    """master_check.csv 读回来的行（字符串） -> Markdown 文本。"""
    songs = [r for r in rows if r['file'] != EP_ROW_NAME]
    ep = next((r for r in rows if r['file'] == EP_ROW_NAME), None)
    first = songs[0] if songs else rows[0]
    counts = {level: sum(1 for r in songs if r['verdict'] == level) for level in ('FAIL', 'INFO', 'PASS')}
    target_text = f"目标 {_cell(first['target_lufs'])} LUFS" if first['target_lufs'] else '没给目标（不做目标核对）'
    out = ['# 母带检查单', '',
           f"- {target_text}；PCM 真峰值上限 {_cell(first['tp_limit_dbtp'], 1, True)} dBTP（容差 +{TP_LIMIT_TOLERANCE_DB:g} dB）",
           f"- 共 {len(songs)} 首：FAIL {counts['FAIL']} 首、INFO {counts['INFO']} 首、PASS {counts['PASS']} 首"
           '（FAIL = 必须处理；INFO = 只报、标黄，由你决定；PASS = 全部干净）',
           f'- 本表由 {CSV_NAME} 生成，CSV 是真源（列更全）。工具只报告，不改任何文件，不自动回退增益。', '']

    header = ['文件', '判定', '整曲 I', '主段 S', 'LRA', '谱斜率', 'PSR P10', 'PSR 中位', '真峰值']
    body = []
    for r in songs:
        basis = {'drop': ' drop', 'loudest': ' 最响'}.get(r['main_basis'], '')
        verdict = f"**{r['verdict']}**" if r['verdict'] == 'FAIL' else r['verdict']
        body.append([_escape(r['file']), verdict, _cell(r['lufs_i']), _cell(r['main_s_lufs']) + (basis if r['main_s_lufs'] else ''),
                     _cell(r['lra_lu']), _cell(r['spectral_slope_db_per_oct'], 2, True), _cell(r['main_psr_p10_db']),
                     _cell(r['main_psr_median_db']), _cell(r['tp8_dbtp'], 2, True)])
    if ep is not None:
        body.append(['**EP 汇总（极差）**', ep['verdict'], '—', _cell(ep['ep_main_s_range_lu'], 2) + ' LU' if ep['ep_main_s_range_lu'] else '—',
                     '—', _cell(ep['ep_slope_range_db_per_oct'], 2) + ' dB/oct' if ep['ep_slope_range_db_per_oct'] else '—',
                     '—', _cell(ep['ep_psr_median_range_db'], 2) + ' dB' if ep['ep_psr_median_range_db'] else '—', '—'])
    out += ['## 逐首核对', '', '单位：I / S 为 LUFS；LRA 为 LU；谱斜率 dB/oct；PSR dB；真峰值 dBTP。'
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
        target_cell = '—' if r['target_delta_lu'] == '' else (
            f"{'drop 段 S' if r['target_basis'] == 'drop' else '整曲 I'} {_cell(r['target_check_lufs'])}（{_cell(r['target_delta_lu'], 1, True)} LU）")
        body_b.append([_escape(r['file']), _cell(r['sample_rate'], 0), r['subtype'] or '—', _cell(r['channels'], 0),
                       _cell(r['duration_s']), _cell(r['dc_offset_dbfs']), _cell(r['lead_silence_s'], 2), _cell(r['trail_silence_s'], 2),
                       _cell(r['mono_lf_loss_db']), _cell(r['side_mid_db']), _cell(r['clipped_sample_count'], 0), target_cell])
    out += ['## 文件与其他项', ''] + _table(header_b, body_b) + ['']

    out += ['## 判定表', '', '判失败只有三项，其余一律只报。', ''] + _table(
        ['级别', '条件'], [[level, text] for level, text in verdict_rule_lines(_num(first['tp_limit_dbtp']))]) + ['']
    out += ['## 说明', '',
            '- 主段：有段落标注 JSON 时取标注里的 drop 段（第 1 小节从 0 s 起）；没有标注或没有 drop 段时取短期响度不低于 P95 - 3 LU 的 3 s 窗。',
            '- 谱斜率：主段的 27 个 1/3 倍频程段电平对 log2 频率的直线斜率，粉红噪声为 0；受母带低通影响。',
            '- 若交付采样率不同于工程采样率，抖动请放在宿主的最后一步；本检查单不检查抖动。', '']
    return '\n'.join(out)


# ---------------------------------------------------------------- 命令行（有副作用的部分）

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
        raise InfrastructureError(f"输入里有重名文件（CSV 按文件名区分各行，必须唯一）：{', '.join(duplicated)}")
    return files


def load_annotation_table(paths):
    """--annotations -> {歌名: 标注}（section_lib.load_annotations）。读不了属于基础设施错。"""
    if not paths:
        return {}
    for path in paths:
        if not Path(path).exists():
            raise InfrastructureError(f'--annotations 不存在：{path}')
    try:
        return section_lib.load_annotations([str(p) for p in paths])
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise InfrastructureError(f'标注读不了：{type(error).__name__} {error}') from error


def sha256_of(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def threshold_snapshot():
    """顶部常量表与 measure_lib 口径常数的当前取值，写进 environment.json，事后能知道这次是按什么线判的。"""
    local = ('DEFAULT_CEILING_DBTP', 'TP_LIMIT_TOLERANCE_DB', 'TARGET_WINDOW_LU', 'DC_WARN_DBFS', 'LEAD_SILENCE_WARN_S',
             'TRAIL_SILENCE_WARN_S', 'MONO_LF_LOSS_WARN_DB', 'EXPECTED_SAMPLE_RATES', 'EXPECTED_SUBTYPES', 'EP_MAIN_S_RANGE_MAX_LU',
             'EP_SLOPE_RANGE_MAX_DB_PER_OCT', 'EP_PSR_MEDIAN_RANGE_MAX_DB')
    shared = ('MAIN_LOUDEST_PERCENTILE', 'MAIN_LOUDEST_BELOW_LU', 'SLOPE_NPERSEG', 'SLOPE_BAND_GATE_DB', 'SLOPE_MIN_BANDS',
              'SILENCE_THRESHOLD_DBFS', 'SIDE_MID_SUSPECT_DB', 'SUSPECT_LOUDNESS_GAP_LU', 'SUSPECT_TP_MARGIN_DB')
    return {**{name: globals()[name] for name in local}, **{name: getattr(meters, name) for name in shared}}


def build_parser():
    parser = argparse.ArgumentParser(
        prog='master_check.py',
        description='母带检查单：直接量成品 wav（不渲染），逐首出判定，多首时出 EP 汇总行。只报告，不改任何输入文件，不自动回退增益。',
        epilog=f'退出码：0 = 没有 FAIL；1 = 至少一首 FAIL；2 = 基础设施错（参数 / 路径 / 标注读不了 / 输出写不了）。'
               f'输出：--out 目录下 {CSV_NAME}（真源）、{MD_NAME}（由 CSV 生成）、{ENVIRONMENT_NAME}。')
    parser.add_argument('--in', dest='input', type=Path, nargs='+', required=True,
                        help='成品 wav 文件或目录（目录只取第一层的 *.wav），可给多个')
    parser.add_argument('--ceiling', type=float, default=None,
                        help=f'PCM 真峰值上限（dBTP），默认 {DEFAULT_CEILING_DBTP}；出流媒体版就写 -2')
    parser.add_argument('--target-lufs', type=float, default=None,
                        help='目标响度；给了才做目标核对与「监听衰减」那条嫌疑，不给就只报数')
    parser.add_argument('--annotations', type=Path, nargs='+', default=[],
                        help='段落标注 JSON 文件或目录（按歌名 = 文件名去扩展名对上，也认 celadon_render 的「歌名__g…」）；有 drop 段就用它定主段')
    parser.add_argument('--out', type=Path, required=True, help='输出目录（不存在会建）')
    parser.add_argument('--dry-run', action='store_true', help='只打印将要做什么（输入、标注匹配、输出文件），不量、不写文件')
    return parser


def print_dry_run(args, profile, files, annotations):
    print('[dry-run] 不量、不写任何文件')
    target = profile['target_lufs']
    print(f"[dry-run] 目标 {'不核对' if target is None else f'{target:+.1f} LUFS'}；PCM 真峰值上限 {profile['tp_limit_dbtp']:+.1f} dBTP")
    print(f'[dry-run] 输入 {len(files)} 个文件：')
    for path in files:
        annotation = section_lib.find_annotation(annotations, path.stem)
        print(f"[dry-run]   {path}  标注 {annotation['song'] if annotation else '无（主段按最响窗口）'}")
    for name in (CSV_NAME, MD_NAME, ENVIRONMENT_NAME):
        print(f'[dry-run] 将写 {args.out / name}')
    return 0


def environment_record(args, argv, profile, files, rows):
    """environment.json 的内容：包版本、脚本 sha256、命令，外加输入哈希、阈值快照、判定计数。"""
    return {
        'tool': 'master_check.py',
        'command': ['master_check.py'] + [str(x) for x in argv],
        'script_sha256': sha256_of(__file__),
        'measure_lib_sha256': sha256_of(meters.__file__),
        'python': sys.version,
        'platform': platform.platform(),
        'packages': {**{name: __import__(name).__version__ for name in ('numpy', 'scipy', 'soundfile')},
                     'pyloudnorm': importlib_metadata.version('pyloudnorm')},
        'profile': profile,
        'thresholds': threshold_snapshot(),
        'annotations': [str(p) for p in args.annotations],
        'inputs': [{'file': p.name, 'bytes': p.stat().st_size, 'sha256': sha256_of(p)} for p in files],
        'verdict_counts': {level: sum(1 for r in rows if r['file'] != EP_ROW_NAME and r['verdict'] == level)
                           for level in ('FAIL', 'INFO', 'PASS')},
    }


def run_master_check(args, argv):
    profile = build_profile(args.target_lufs, args.ceiling)
    files = collect_inputs(args.input)
    annotations = load_annotation_table(args.annotations)
    if args.dry_run:
        return print_dry_run(args, profile, files, annotations)
    try:
        args.out.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise InfrastructureError(f'目录建不了：{error}') from error

    rows = []
    for index, path in enumerate(files, 1):
        print(f'[{index}/{len(files)}] {path.name}', flush=True)
        result = check_master_file(path, profile, section_lib.find_annotation(annotations, path.stem))
        apply_judgement(result['row'], profile, result['read_error'])
        rows.append(result['row'])
    if len(rows) >= 2:
        rows.append(build_ep_row(list(rows), profile))
    songs = [r for r in rows if r['file'] != EP_ROW_NAME]

    try:
        write_check_csv(args.out / CSV_NAME, rows)
        with (args.out / CSV_NAME).open(newline='', encoding='utf-8') as stream:
            csv_rows = list(csv.DictReader(stream))
        (args.out / MD_NAME).write_text(render_markdown(csv_rows), encoding='utf-8')
        record = environment_record(args, argv, profile, files, rows)
        (args.out / ENVIRONMENT_NAME).write_text(json.dumps(record, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    except OSError as error:
        raise InfrastructureError(f'输出写不了：{error}') from error

    for row in songs:
        detail = '; '.join(part for part in (row['fail_reasons'], row['info_flags'], row['na_reason']) if part)
        print(f"{row['verdict']:4} {row['file']}" + (f'  {detail}' if detail else ''))
    failed = sum(1 for r in songs if r['verdict'] == 'FAIL')
    print(f"共 {len(songs)} 首：FAIL {failed}、INFO {sum(1 for r in songs if r['verdict'] == 'INFO')}、"
          f"PASS {sum(1 for r in songs if r['verdict'] == 'PASS')}；输出 {args.out}")
    return 1 if failed else 0


def main(argv):
    """返回退出码：0 无 FAIL / 1 有 FAIL / 2 基础设施错（argparse 的参数错也是 2）。"""
    args = build_parser().parse_args(argv)
    try:
        return run_master_check(args, argv)
    except InfrastructureError as error:
        print(f'错误（基础设施）：{error}', file=sys.stderr)
        return 2
    except Exception:   # 意料之外的异常也按基础设施错处理，并留下完整堆栈便于排查
        traceback.print_exc()
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
