#!/usr/bin/env python3
# T-048 阶段 1 · P1R：在 calibrate_house.py analyze-live 的结果（live/<标签>/tracks.csv、bars.csv）之上另算三样东西，给 Max 的两件事出数据：
#
#   (b) M2 的三种口径并排：M2 = 计入的小节 j ≥ 16（规格第 7 节第 1 条原文）；M2b = j ≥ d + 4（第一个 drop 的 4 小节都已并入）；
#       M2c = j ≥ d（从第一个 drop 起）。每种都是「逐首中位 → 各首的中位 / P90 / 最大」（同 analyze-live），另给全部小节合在一起的分布。
#       d = 第一个标为 drop 的小节（T-038 的交界小节不算，真正的第一个 drop 小节多半是 d − 1）。
#   (a) 第一个 drop 时 c 的下沉：c_pre = 第 d − 2 小节（标签上最后一个前奏小节，交界）那一份画像的 c——drop 开始时手里的 c；
#       c_min = 从第 d − 1 小节（真正的第一个 drop 小节）到第一段 drop 结束这一段里各小节画像 c 的最低值，及它落在 drop 的第几小节；
#       回到 0.8 = 第 d − 1 小节起第一次「c 跌破 0.8 之后又 ≥ 0.8」的小节（从 drop 开始数第几小节、按小节长折算的秒）；
#       另给 c 低时决策器的强度 κ = 0.35 + 0.65 × min(1, c ÷ 0.8)（GlazePlanner.cpp confidenceScale：unlearnedStrength 0.35、
#       fullConfidence 0.8；实时画像的 I / 真峰值置信度 = min(c, 观测充分度)、代表性 = c，所以这里是 κ 的上界）。
#   并排逐首表：两个标签的 M1 / M2 中位 / M3 / M4 δ4 与 n / drop 开始时 I − D / 前奏与 drop 的 m ≥ 0.5（首判、终判）/ 换歌 / 时间衰减 /
#       c 到 0.8 / drop 之后 c 的最低值。
#
#   p1r_live_extra.py <工作目录> --tags 标签1,标签2,… --out 汇总.md [--csv-dir 目录] [--pair 新:旧]
#
# 只读 analyze-live 写好的 CSV；先跑 analyze-live。口径与 calibrate_house.py 的 live_track_metrics 一致（计入的小节、held(j) = I_{j−1}）。

import argparse
import csv
import math
import os
import statistics
import sys

import numpy as np

LIVE_START_BAR = 16
DROP_OFFSET_BARS = 4
M2_LIMITS = (0.3, 1.0)
CONFIDENCE_FULL = 0.8
UNLEARNED_STRENGTH = 0.35


def num(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return math.nan
    return value


def summary(values):
    values = [v for v in values if math.isfinite(v)]
    if not values:
        return math.nan, math.nan, math.nan, 0
    return statistics.median(values), float(np.percentile(values, 90)), max(values), len(values)


def fmt(value, digits=2, signed=False):
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return '—'
    return f'{value:+.{digits}f}' if signed else f'{value:.{digits}f}'


def triple(stats, digits=2):
    return f'{fmt(stats[0], digits)} / {fmt(stats[1], digits)} / {fmt(stats[2], digits)}'


def kappa(c):
    return UNLEARNED_STRENGTH + (1.0 - UNLEARNED_STRENGTH) * min(1.0, max(0.0, c) / CONFIDENCE_FULL) if math.isfinite(c) else math.nan


def read_csv(path):
    with open(path, encoding='utf-8') as f:
        return list(csv.DictReader(f))


def load_tag(work, tag):
    base = os.path.join(work, 'live', tag)
    tracks = {r['track']: r for r in read_csv(os.path.join(base, 'tracks.csv'))}
    bars = {}
    for b in read_csv(os.path.join(base, 'bars.csv')):
        bars.setdefault(b['track'], []).append(b)
    return tracks, bars


def scored(track_row):
    return track_row['excluded'] == '0' and math.isfinite(num(track_row['drop_lufs']))


def m2_variants(track_row, bar_rows):
    """一首歌三种口径的 |held − D| 列表（计入的小节）。"""
    d = int(track_row['first_drop_bar'])
    errors = {'m2': [], 'm2b': [], 'm2c': []}
    for b in bar_rows:
        if b['counted'] != '1':
            continue
        e = num(b['abs_error_held'])
        if not math.isfinite(e):
            continue
        j = int(b['bar'])
        if j >= LIVE_START_BAR:
            errors['m2'].append(e)
        if j >= d + DROP_OFFSET_BARS:
            errors['m2b'].append(e)
        if j >= d:
            errors['m2c'].append(e)
    return errors


def first_drop_run_end(bar_rows, d):
    """第一段 drop（从 d 起连续标为 drop 的小节）的最后一小节。"""
    labels = {int(b['bar']): b['label'] for b in bar_rows}
    end = d
    while labels.get(end + 1) == 'drop':
        end += 1
    return end


def c_dip(track_row, bar_rows):
    d = int(track_row['first_drop_bar'])
    bar_seconds = 240.0 / float(track_row['bpm'])
    c_by_bar = {int(b['bar']): num(b['confidence']) for b in bar_rows}
    start = d - 1                                   # 真正的第一个 drop 小节（交界）
    end = first_drop_run_end(bar_rows, d)
    pre = c_by_bar.get(d - 2, math.nan)             # 最后一个前奏小节那一份的 c = drop 开始时手里的 c
    window = [(j, c_by_bar[j]) for j in range(start, end + 1) if j in c_by_bar and math.isfinite(c_by_bar[j])]
    record = {'class': track_row['class'], 'track': track_row['track'], 'bpm': track_row['bpm'], 'first_drop_bar_d': d,
              'drop_start_bar': start, 'first_drop_run_end': end, 'c_pre': pre, 'kappa_pre': kappa(pre)}
    if not window or d - 2 < 1:
        record.update({'note': '第一个 drop 在开头（没有前奏）' if d - 2 < 1 else '没有 drop 小节的 c'})
        return record
    j_min, c_min = min(window, key=lambda x: (x[1], x[0]))
    record.update({'c_min_first_drop': c_min, 'c_min_bar': j_min, 'c_min_drop_bar_index': j_min - start + 1,
                   'kappa_min': kappa(c_min), 'kappa_ratio_min_over_pre': kappa(c_min) / kappa(pre) if math.isfinite(pre) else math.nan,
                   'c_drop_over_pre': pre - c_min if math.isfinite(pre) else math.nan})
    # 回到 0.8：从 drop 开始往后第一个 c < 0.8 的小节之后，第一次 c ≥ 0.8 的小节（不限于第一段 drop）
    later = sorted((j, c) for j, c in c_by_bar.items() if j >= start and math.isfinite(c))
    below = next((j for j, c in later if c < CONFIDENCE_FULL), None)
    if below is None:
        record.update({'below_08': 0, 'recover_bar': '', 'recover_drop_bars': 0, 'recover_s': 0.0, 'note': 'drop 之后 c 一直 ≥ 0.8'})
    else:
        back = next((j for j, c in later if j > below and c >= CONFIDENCE_FULL), None)
        record['below_08'] = 1
        record['first_below_bar'] = below
        if back is None:
            record.update({'recover_bar': '', 'recover_drop_bars': math.nan, 'recover_s': math.nan, 'note': '到歌尾都没回到 0.8'})
        else:
            record.update({'recover_bar': back, 'recover_drop_bars': back - start + 1, 'recover_s': (back - below) * bar_seconds,
                           'recover_from_drop_s': (back - start + 1) * bar_seconds})
        if math.isfinite(pre) and pre < CONFIDENCE_FULL:
            record['note'] = 'drop 开始时 c 还没到 0.8'
    return record


def energy_mean(values):
    values = [v for v in values if math.isfinite(v)]
    return -0.691 + 10.0 * math.log10(sum(10.0 ** ((v + 0.691) / 10.0) for v in values) / len(values)) if values else math.nan


def m4_split(track_row, bar_rows):
    """M4 拆两半：δ4 = I_{d+3} − I_end = (I_{d+3} − D4) + (D4 − I_end)。D4 = 第 d … d + 3 小节（标为 drop、已听）的输入段响度能量平均
    （第一个 drop 头 4 个计入小节的 drop 响度，同 D 的口径）。前一半是实时画像在 d + 3 时跟踪第一个 drop 的误差（算法能改的），
    后一半是歌自己后来变了多少（最终值由后面的 drop 决定；任何因果的估计在 d + 3 都看不到）——|D4 − I_end| > 1 的歌，
    I 在 d + 3 准确跟住第一个 drop 也过不了第 3 条。"""
    d = int(track_row['first_drop_bar'])
    by_bar = {int(b['bar']): b for b in bar_rows}
    drop4 = [num(by_bar[j]['segment_lufs']) for j in range(d, d + DROP_OFFSET_BARS)
             if j in by_bar and by_bar[j]['label'] == 'drop' and by_bar[j]['heard'] == '1']
    d4 = energy_mean(drop4)
    i_d3 = num(by_bar[d + DROP_OFFSET_BARS - 1]['i_after']) if d + DROP_OFFSET_BARS - 1 in by_bar else math.nan
    i_end = num(track_row['final_i_lufs'])
    # 跟上第一个 drop 要几小节：从第 d − 1 小节（真正的第一个 drop 小节）起，第一个「手里的 I」离 D4 ≤ 1 LU 的小节（0 = drop 开始时就在手里）
    catch = next((j - (d - 1) for j in range(d - 1, d + 64) if j in by_bar and math.isfinite(num(by_bar[j]['i_held']))
                  and abs(num(by_bar[j]['i_held']) - d4) <= 1.0), math.nan) if math.isfinite(d4) and d > 1 else math.nan
    return {'class': track_row['class'], 'track': track_row['track'], 'first_drop_bar_d': d, 'drop4_bars': len(drop4),
            'drop4_lufs': d4, 'drop_lufs': num(track_row['drop_lufs']), 'i_d3': i_d3, 'i_end': i_end,
            'delta4': i_d3 - i_end, 'tracking': i_d3 - d4, 'evolution': d4 - i_end,
            'pass': int(abs(i_d3 - i_end) <= 1.0), 'oracle_pass': int(abs(d4 - i_end) <= 1.0), 'catch_bars': catch}


def m4_markdown(results):
    out = ['## M4 拆两半：δ4 = (I_{d+3} − D4)「跟踪误差」+ (D4 − I_end)「歌自己变了」（D4 = 第 d … d+3 小节 drop 的输入段响度能量平均）', '',
           '| 标签 | 第 3 条达标 | 「跟住第一个 drop 就够」的首数（\\|D4 − I_end\\| ≤ 1） | 跟踪误差 \\|I_{d+3} − D4\\| 中位 / P90 / 最大 | '
           '歌自己变了 \\|D4 − I_end\\| 中位 / P90 / 最大 | 不达标里：跟踪误差 > 1 的 / 只因歌自己变了 |', '|---|---|---|---|---|---|']
    for r in results:
        split = r['m4split']
        fails = [x for x in split if not x['pass']]
        tracking_fail = [x for x in fails if abs(x['tracking']) > 1.0]
        evolution_only = [x for x in fails if abs(x['tracking']) <= 1.0]
        out.append(f"| {r['tag']} | {sum(x['pass'] for x in split)} / {len(split)} | {sum(x['oracle_pass'] for x in split)} / {len(split)} | "
                   f"{triple(summary([abs(x['tracking']) for x in split]))} | {triple(summary([abs(x['evolution']) for x in split]))} | "
                   f"{len(tracking_fail)} / {len(evolution_only)} |")
    out.append('')
    out.append('跟上第一个 drop 要几小节（从真正的第一个 drop 小节 d − 1 起，第一个「手里的 I」离 D4 ≤ 1 LU 的小节；0 = drop 开始时就在手里；有前奏的歌）：')
    out.append('')
    out.append('| 标签 | 中位 / P90 / 最大（小节） | ≤ 4 小节的首数 | 64 小节内跟不上的首数 |')
    out.append('|---|---|---|---|')
    for r in results:
        catches = [x['catch_bars'] for x in r['m4split'] if x['first_drop_bar_d'] > 1]
        finite = [float(c) for c in catches if math.isfinite(c)]
        out.append(f"| {r['tag']} | {triple(summary(finite), 0)} | {sum(1 for c in finite if c <= 4)} / {len(catches)} | "
                   f"{sum(1 for c in catches if not math.isfinite(c))} |")
    out.append('')
    for r in results[:1]:
        fails = sorted((x for x in r['m4split'] if not x['pass']), key=lambda x: x['delta4'])
        out.append(f"**{r['tag']}** 不达标的 {len(fails)} 首：")
        out.append('')
        out.append('| 子类 | 曲目 | d | δ4 | 跟踪误差 I_{d+3} − D4 | 歌自己变了 D4 − I_end | D4 | D（全部 drop） | I_end |')
        out.append('|---|---|---|---|---|---|---|---|---|')
        for x in fails:
            out.append(f"| {x['class']} | {x['track']} | {x['first_drop_bar_d']} | {fmt(x['delta4'], 2, True)} | {fmt(x['tracking'], 2, True)} | "
                       f"{fmt(x['evolution'], 2, True)} | {fmt(x['drop4_lufs'])} | {fmt(x['drop_lufs'])} | {fmt(x['i_end'])} |")
        out.append('')
    return out


def drop_reject(work, tag, track_row, bar_rows):
    """计入的 drop 小节里首判 m < 0.5 的，按哪道门挡的分：只响度门（低频因子 = 1）、只低频门（响度因子 = 1）、两道都（其余）。
    因子取追踪里那一小节（最后一段）发布那一份画像的 loudness_factor / low_factor（= 首判 m 的两个因子）。另给终判 m < 0.5 的小节数。"""
    folder = os.path.join(work, 'live', tag, 'traces')
    names = sorted(n for n in os.listdir(folder) if n.startswith(track_row['track'] + '__') and n.endswith('.csv')
                   and not n.endswith('__final.csv') and not n.endswith('__judgements.csv'))
    by_bar = {}
    with open(os.path.join(folder, names[0]), encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if row['bar']:
                by_bar[int(row['bar'])] = row
    counted = [b for b in bar_rows if b['counted'] == '1' and b['label'] == 'drop']
    record = {'class': track_row['class'], 'track': track_row['track'], 'drop_counted': len(counted),
              'drop_rejected': 0, 'by_loudness': 0, 'by_low': 0, 'by_both': 0, 'drop_rejected_final': 0}
    for b in counted:
        j = int(b['bar'])
        if math.isfinite(num(b['mainness_final'])) and num(b['mainness_final']) < 0.5:
            record['drop_rejected_final'] += 1
        if num(b['mainness']) >= 0.5 or j not in by_bar:
            continue
        record['drop_rejected'] += 1
        loud, low = num(by_bar[j]['loudness_factor']), num(by_bar[j]['low_factor'])
        record['by_loudness' if low >= 0.999 else 'by_low' if loud >= 0.999 else 'by_both'] += 1
    return record


DIP_EVENT = 0.2      # 一份画像的 c 比前 4 份里的最高低这么多 = 一次「c 下沉事件」
DIP_LOOKBACK = 4


def c_events(work, tag, track_row):
    """整首歌里 c 的下沉事件（不只第一个 drop）：第 k 份画像的 c 比前 DIP_LOOKBACK 份里的最高低 DIP_EVENT 以上，且上一份还没低这么多
    （一次连续下沉记一次，记最先跌过线的那一份）。原因按这一份与上一份比：S_ref 升 ≥ 0.5 LU、lowRef 变 ≥ 10 %、换歌、无主段时间衰减。"""
    folder = os.path.join(work, 'live', tag, 'traces')
    names = sorted(n for n in os.listdir(folder) if n.startswith(track_row['track'] + '__') and n.endswith('.csv')
                   and not n.endswith('__final.csv') and not n.endswith('__judgements.csv'))
    with open(os.path.join(folder, names[0]), encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    events, inside = [], False
    for k in range(1, len(rows)):
        c = num(rows[k]['confidence'])
        peak = max((num(r['confidence']) for r in rows[max(0, k - DIP_LOOKBACK):k]), default=math.nan)
        falling = math.isfinite(c) and math.isfinite(peak) and peak - c > DIP_EVENT
        if falling and not inside:
            prev = rows[k - 1]
            causes = []
            if num(rows[k]['reference_lufs']) - num(prev['reference_lufs']) >= 0.5:
                causes.append(f"S_ref +{num(rows[k]['reference_lufs']) - num(prev['reference_lufs']):.1f}")
            low_now, low_prev = num(rows[k]['low_reference']), num(prev['low_reference'])
            if math.isfinite(low_now) and math.isfinite(low_prev) and low_prev > 0 and abs(low_now / low_prev - 1.0) >= 0.1:
                causes.append(f'lowRef {low_prev:.2f}→{low_now:.2f}')
            if rows[k]['new_program'] == '1':
                causes.append('换歌')
            if rows[k]['time_decay'] == '1':
                causes.append('无主段衰减')
            events.append({'class': track_row['class'], 'track': track_row['track'], 'bar': rows[k]['bar'], 'c_before': peak, 'c_after': c,
                           'label_first_drop_bar': track_row['first_drop_bar'], 'cause': '；'.join(causes) or '其它（参考小幅变化累积）'})
        inside = falling
    return events


def events_markdown(results):
    out = [f'## 整首歌里 c 的下沉事件（某一份画像的 c 比前 {DIP_LOOKBACK} 份里的最高低 {DIP_EVENT} 以上；一次连续下沉记一次）', '']
    for r in results:
        events = r.get('events', [])
        if not events:
            out.append(f"**{r['tag']}**：没有。")
            out.append('')
            continue
        tracks = sorted({e['track'] for e in events})
        by_cause = {}
        for e in events:
            key = 'S_ref 升' if 'S_ref' in e['cause'] else 'lowRef 变' if 'lowRef' in e['cause'] else e['cause']
            by_cause[key] = by_cause.get(key, 0) + 1
        out.append(f"**{r['tag']}**：{len(events)} 次、{len(tracks)} 首；原因（同一次有几样时按 S_ref → lowRef 的先后归一类）："
                   + '、'.join(f'{k} {v} 次' for k, v in sorted(by_cause.items(), key=lambda x: -x[1])) + '。')
        out.append('')
        out.append('| 子类 | 曲目 | 小节 | 首个 drop（标签 d） | c 之前 → 之后 | 原因 |')
        out.append('|---|---|---|---|---|---|')
        for e in sorted(events, key=lambda e: (e['track'], int(e['bar'] or 0)))[:40]:
            out.append(f"| {e['class']} | {e['track']} | {e['bar']} | {e['label_first_drop_bar']} | {fmt(e['c_before'])} → {fmt(e['c_after'])} | {e['cause']} |")
        out.append('')
    return out


def write_csv(path, records):
    if not records:
        return
    keys = []
    for r in records:
        keys.extend(k for k in r if k not in keys)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for r in records:
            writer.writerow({k: (fmt(v, 4) if isinstance(v, float) else v) for k, v in r.items()})


def tag_report(work, tag, csv_dir, with_reject=True):
    tracks, bars = load_tag(work, tag)
    rows = [t for t in tracks.values() if scored(t)]
    m2_records, dip_records, split_records = [], [], []
    pooled = {'m2': [], 'm2b': [], 'm2c': []}
    for t in rows:
        errors = m2_variants(t, bars[t['track']])
        record = {'class': t['class'], 'track': t['track'], 'first_drop_bar_d': int(t['first_drop_bar'])}
        for key, values in errors.items():
            stats = summary(values)
            record[f'{key}_median'], record[f'{key}_p90'], record[f'{key}_bars'] = stats[0], stats[1], stats[3]
            pooled[key].extend(values)
        m2_records.append(record)
        dip_records.append(c_dip(t, bars[t['track']]))
        split_records.append(m4_split(t, bars[t['track']]))
    reject_records = [drop_reject(work, tag, t, bars[t['track']]) for t in rows] if with_reject else []
    event_records = [e for t in tracks.values() if t['excluded'] == '0' for e in c_events(work, tag, t)] if with_reject else []
    if csv_dir:
        write_csv(os.path.join(csv_dir, f'{tag}-m2-variants.csv'), m2_records)
        write_csv(os.path.join(csv_dir, f'{tag}-c-dip.csv'), dip_records)
        write_csv(os.path.join(csv_dir, f'{tag}-m4-split.csv'), split_records)
        write_csv(os.path.join(csv_dir, f'{tag}-drop-reject.csv'), reject_records)
        write_csv(os.path.join(csv_dir, f'{tag}-c-events.csv'), event_records)
    heads = {key: summary([r[f'{key}_median'] for r in m2_records]) for key in pooled}
    pooled_stats = {key: summary(values) for key, values in pooled.items()}
    has_final = any(math.isfinite(num(b['mainness_final'])) for track_bars in bars.values() for b in track_bars)
    return {'tag': tag, 'tracks': tracks, 'rows': rows, 'm2': m2_records, 'dip': dip_records, 'm4split': split_records,
            'reject': reject_records, 'events': event_records, 'heads': heads, 'pooled': pooled_stats, 'has_final': has_final}


def reject_markdown(results):
    out = ['## drop 小节被挡（计入的 drop 小节里首判 m < 0.5；只响度门 = 低频因子 1、只低频门 = 响度因子 1）', '',
           '| 标签 | 计入的 drop 小节 | 首判被挡 | 只响度门 / 只低频门 / 两道都 | 终判被挡 | tech 0268597 首判被挡 / 终判被挡 |', '|---|---|---|---|---|---|']
    for r in results:
        if not r['reject']:
            continue
        total = {k: sum(x[k] for x in r['reject']) for k in ('drop_counted', 'drop_rejected', 'by_loudness', 'by_low', 'by_both', 'drop_rejected_final')}
        t = next((x for x in r['reject'] if x['track'] == 'track_0268597'), None)
        final_cell = str(total['drop_rejected_final']) if r['has_final'] else '—（旧追踪没有终判）'
        t_cell = '—'
        if t:
            t_cell = f"{t['drop_rejected']} / {t['drop_rejected_final'] if r['has_final'] else '—'}（共 {t['drop_counted']}）"
        out.append(f"| {r['tag']} | {total['drop_counted']} | {total['drop_rejected']} | {total['by_loudness']} / {total['by_low']} / {total['by_both']} | "
                   f"{final_cell} | {t_cell} |")
    out.append('')
    return out


def passes(stats):
    return math.isfinite(stats[0]) and stats[0] <= M2_LIMITS[0] and stats[1] <= M2_LIMITS[1]


def m2_markdown(results):
    out = ['## M2 三种口径（逐首中位 → 各首的中位 / P90 / 最大；门槛 中位 ≤ 0.3、P90 ≤ 1.0 LU）', '',
           '| 口径 | ' + ' | '.join(r['tag'] for r in results) + ' |', '|---|' + '---|' * len(results)]
    names = {'m2': 'M2：j ≥ 16（规格原文）', 'm2b': 'M2b：j ≥ d + 4', 'm2c': 'M2c：j ≥ d（从第一个 drop 起）'}
    for key, name in names.items():
        out.append(f'| {name} | ' + ' | '.join(f"{triple(r['heads'][key])}（{'达标' if passes(r['heads'][key]) else '不达标'}）" for r in results) + ' |')
        out.append(f'| 同上，全部小节合在一起 | ' + ' | '.join(triple(r['pooled'][key]) for r in results) + ' |')
    out.append('')
    for r in results:
        worst = sorted(r['m2'], key=lambda x: -x['m2_median'] if math.isfinite(x['m2_median']) else 0)[:8]
        out.append(f"**{r['tag']}**：M2 逐首中位最大的 8 首（d = 第一个 drop 小节；d > 16 = 第 16 小节还在前奏里）——")
        out.append('')
        out.append('| 子类 | 曲目 | d | M2（≥16） | M2b（≥d+4） | M2c（≥d） |')
        out.append('|---|---|---|---|---|---|')
        for x in worst:
            out.append(f"| {x['class']} | {x['track']} | {x['first_drop_bar_d']} | {fmt(x['m2_median'])} | {fmt(x['m2b_median'])} | {fmt(x['m2c_median'])} |")
        long_intro = [x for x in r['m2'] if x['first_drop_bar_d'] > LIVE_START_BAR]
        out.append('')
        out.append(f"d > 16 的 {len(long_intro)} 首：M2 逐首中位 {triple(summary([x['m2_median'] for x in long_intro]))}，"
                   f"M2b {triple(summary([x['m2b_median'] for x in long_intro]))}；d ≤ 16 的 {len(r['m2']) - len(long_intro)} 首："
                   f"M2 {triple(summary([x['m2_median'] for x in r['m2'] if x['first_drop_bar_d'] <= LIVE_START_BAR]))}。")
        out.append('')
    return out


DIP_MIN = 0.05   # c_pre − c_min 超过它才算「掉了」


def dip_markdown(results):
    out = ['## 第一个 drop 时 c 的下沉', '',
           f'c_pre = drop 开始时手里的 c（第 d − 2 小节那一份）；c_min = 第一段 drop 里（第 d − 1 小节起到第一段 drop 结束）c 的最低；「掉了」= c_pre − c_min > {DIP_MIN}；'
           '到 0.8 = 第 d − 1 小节起第一次 c < 0.8 之后第一次 ≥ 0.8 的小节（drop 开始时 c 已 < 0.8 的歌就是「drop 之后多久到 0.8」）；'
           'κ = 0.35 + 0.65 × min(1, c ÷ 0.8)（κ 的上界，见文件头），κ_min ÷ κ_pre ≈ 自动增益与各模块处理量被收到原来的几成。', '']
    for r in results:
        intro = [x for x in r['dip'] if 'c_min_first_drop' in x]
        fell = [x for x in intro if math.isfinite(x['c_drop_over_pre']) and x['c_drop_over_pre'] > DIP_MIN]
        fell_from_full = [x for x in fell if x['c_pre'] >= CONFIDENCE_FULL]
        reach = [x for x in intro if isinstance(x.get('recover_drop_bars'), int) and x.get('below_08') == 1]
        out.append(f"**{r['tag']}**：可算 {len(r['dip'])} 首，有前奏（d ≥ 3）{len(intro)} 首；第一段 drop 里 c 掉了的 {len(fell)} 首"
                   f"（其中 drop 开始时 c ≥ 0.8 的 {len(fell_from_full)} 首）；drop 开始之后 c 有一段 < 0.8 的 {sum(1 for x in intro if x.get('below_08') == 1)} 首。")
        out.append('')
        out.append('| 量 | 中位 / P90 / 最大 |')
        out.append('|---|---|')
        out.append(f"| drop 开始时 c（有前奏的 {len(intro)} 首） | {triple(summary([x['c_pre'] for x in intro]))} |")
        out.append(f"| 下沉量 c_pre − c_min（{len(intro)} 首） | {triple(summary([x['c_drop_over_pre'] for x in intro]))} |")
        out.append(f"| 掉了的 {len(fell)} 首：c_pre | {triple(summary([x['c_pre'] for x in fell]))} |")
        out.append(f"| 掉了的 {len(fell)} 首：c_min（中位 / P10 / 最小） | {fmt(summary([x['c_min_first_drop'] for x in fell])[0])} / "
                   f"{fmt(float(np.percentile([x['c_min_first_drop'] for x in fell], 10)) if fell else math.nan)} / "
                   f"{fmt(min([x['c_min_first_drop'] for x in fell], default=math.nan))} |")
        out.append(f"| 掉了的 {len(fell)} 首：最低落在 drop 第几小节 | {triple(summary([float(x['c_min_drop_bar_index']) for x in fell]), 0)} |")
        out.append(f"| 掉了的 {len(fell)} 首：κ_min ÷ κ_pre（中位 / P10 / 最小） | {fmt(summary([x['kappa_ratio_min_over_pre'] for x in fell])[0])} / "
                   f"{fmt(float(np.percentile([x['kappa_ratio_min_over_pre'] for x in fell], 10)) if fell else math.nan)} / "
                   f"{fmt(min([x['kappa_ratio_min_over_pre'] for x in fell], default=math.nan))} |")
        out.append(f"| 到 0.8：drop 第几小节（{len(reach)} 首） | {triple(summary([float(x['recover_drop_bars']) for x in reach]), 0)} |")
        out.append(f"| 到 0.8：从 drop 开始的秒数（同上） | {triple(summary([x['recover_from_drop_s'] for x in reach]), 1)} |")
        out.append('')
        if fell:
            out.append('| 子类 | 曲目 | d − 1 | c_pre | c_min | 最低在 drop 第几小节 | 到 0.8（drop 第几小节 / 从 drop 开始几秒） | κ_min ÷ κ_pre |')
            out.append('|---|---|---|---|---|---|---|---|')
            for x in sorted(fell, key=lambda x: -x['c_drop_over_pre']):
                recover = (f"{x['recover_drop_bars']} / {fmt(x['recover_from_drop_s'], 1)} s" if isinstance(x.get('recover_drop_bars'), int)
                           and x.get('below_08') == 1 else x.get('note', ''))
                out.append(f"| {x['class']} | {x['track']} | {x['drop_start_bar']} | {fmt(x['c_pre'])} | {fmt(x['c_min_first_drop'])} | "
                           f"{x['c_min_drop_bar_index']} | {recover} | {fmt(x['kappa_ratio_min_over_pre'])} |")
            out.append('')
    return out


def side_by_side(results, pair):
    new, old = pair.split(':')
    a = next(r for r in results if r['tag'] == new)
    b = next(r for r in results if r['tag'] == old)
    out = [f'## 逐首并排：{new}（新）/ {old}（旧）', '',
           '| 子类 | 曲目 | d | D | M1 | M2 中位 | M3 最大 | M4 δ4 | n | drop 开始时 I − D | 前奏 m≥0.5 首判（终判） | drop m≥0.5 首判（终判） | 换歌 | 时间衰减（次） | c≥0.8（s） | drop 后 c 最低 |',
           '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for track in sorted(a['tracks'], key=lambda t: (('deep', 'tech', 'melodic').index(a['tracks'][t]['class'])
                                                     if a['tracks'][t]['class'] in ('deep', 'tech', 'melodic') else 9, t)):
        x, y = a['tracks'][track], b['tracks'].get(track)
        if x['excluded'] == '1':
            out.append(f"| {x['class']} | {track} | 整首 exclude |" + ' |' * 13)
            continue
        if y is None:
            continue

        def pair_cell(key, digits=2, signed=False):
            return f"{fmt(num(x[key]), digits, signed)} / {fmt(num(y[key]), digits, signed)}"

        def share(row, kind, final=False):
            if final:
                return f"{row.get(f'{kind}_final_main_bars', '')}" if row.get(f'{kind}_final_bars') else '—'
            return f"{row[f'{kind}_main_bars']} / {row[f'{kind}_heard_bars']}"

        intro = f"{share(x, 'intro')}（{share(x, 'intro', True)}）/ {share(y, 'intro')}"
        drop = f"{share(x, 'drop')}（{share(x, 'drop', True)}）/ {share(y, 'drop')}"
        np_cell = f"{x['new_program_count']}{'（' + x['new_program_bars'] + '）' if x['new_program_bars'] else ''} / " \
                  f"{y['new_program_count']}{'（' + y['new_program_bars'] + '）' if y['new_program_bars'] else ''}"
        out.append(f"| {x['class']} | {track} | {x['first_drop_bar']} | {fmt(num(x['drop_lufs']))} | {pair_cell('m1_final_minus_drop', signed=True)} | "
                   f"{pair_cell('m2_median')} | {pair_cell('m3_max_drift')} | {pair_cell('m4_delta4', signed=True)} | "
                   f"{fmt(num(x['m4_settle_bars']), 0)} / {fmt(num(y['m4_settle_bars']), 0)} | {pair_cell('held_at_first_drop_minus_drop', signed=True)} | "
                   f"{intro} | {drop} | {np_cell} | {x['time_decay_episodes']} / {y['time_decay_episodes']} | "
                   f"{fmt(num(x['c08_active_s']), 1)} / {fmt(num(y['c08_active_s']), 1)} | "
                   f"{fmt(num(x['min_c_after_first_drop']))} / {fmt(num(y.get('min_c_after_first_drop', '')))} |")
    out.append('')
    return out


def main(argv):
    parser = argparse.ArgumentParser(description='P1R：M2 口径、c 下沉、逐首并排')
    parser.add_argument('work')
    parser.add_argument('--tags', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--csv-dir')
    parser.add_argument('--pair', action='append', default=[], help='新:旧，出逐首并排表')
    args = parser.parse_args(argv)
    if args.csv_dir:
        os.makedirs(args.csv_dir, exist_ok=True)
    results = [tag_report(args.work, tag, args.csv_dir) for tag in args.tags.split(',')]
    out = [f'# P1R：M2 口径、第一个 drop 时 c 的下沉、逐首并排（{args.work}）', '']
    out += m2_markdown(results)
    out += m4_markdown(results)
    out += dip_markdown(results)
    out += reject_markdown(results)
    out += events_markdown(results)
    for pair in args.pair:
        out += side_by_side(results, pair)
    text = '\n'.join(out) + '\n'
    with open(args.out, 'w', encoding='utf-8') as f:
        f.write(text)
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
