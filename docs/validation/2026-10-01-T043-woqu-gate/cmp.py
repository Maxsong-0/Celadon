#!/usr/bin/env python3
# T-043：几份 22 首指标表并排比——每首歌每个 GLAZE > 0：修坯自动对重放全关的 dLUFS / dInPk，及低切频率、守护介入、压力档。
# 用法：cmp.py 标签=all.csv [标签=all.csv …]
import csv
import math
import sys


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return math.nan


def pk(a, b):
    pa, pb = a.get('limit_input_peak_db') or '', b.get('limit_input_peak_db') or ''
    if not pa and not pb:
        return 0.0
    if not pa:
        return -math.inf
    if not pb:
        return math.inf
    return float(pa) - float(pb)


tables = []
for arg in sys.argv[1:]:
    label, path = arg.split('=', 1)
    by = {}
    for r in csv.DictReader(open(path, encoding='utf-8')):
        if r['kind'] == 'render':
            by.setdefault((r['song'], r['glaze']), {})[r['clay']] = r
    tables.append((label, by))

keys = sorted(set().union(*[set(t.keys()) for _, t in tables]), key=lambda k: (k[0].lower(), num(k[1])))
head = f"{'song':24} {'g':>3}"
for label, _ in tables:
    head += f" | {label[:10]:>10} {'lc Hz':>6} {'grd%':>5} {'stp':>3} {'dInPk':>7} {'dLUFS':>7} {'PLRΔ':>6}"
print(head)
for key in keys:
    if num(key[1]) == 0:
        continue
    line = f"{key[0][:24]:24} {key[1]:>3}"
    for label, t in tables:
        pair = t.get(key, {})
        a, rp = pair.get('auto'), pair.get('replay-off')
        if not a or not rp:
            line += f" | {'—':>10}" + ' ' * 41
            continue
        dl = num(a['lufs']) - num(rp['lufs'])
        di = pk(a, rp)
        verdict = 'FAIL' if (di > 0.3 or (dl < -0.1 and not di <= dl + 0.05)) else ('note' if dl < -0.1 else 'ok')
        line += (f" | {verdict:>10} {num(a['clay_lowcut_hz']):6.2f} {num(a['peak_guard_pct']):5.1f} {a.get('clay_pressure_steps', ''):>3}"
                 f" {di:+7.3f} {dl:+7.3f} {dl - di:+6.3f}")
    print(line)
