#!/usr/bin/env python3
# T-042：22 首门槛歌改前改后对比（反相提示时长、慢路反相占比、宽度保护、音频哈希）。
import csv, glob, os, sys

def load(root):
    rows = {}
    for f in sorted(glob.glob(os.path.join(root, '*', 'metrics.csv'))):
        for r in csv.DictReader(open(f, encoding='utf-8')):
            key = (r['song'], r['kind'], r['glaze'], r['clay'], r['style'])
            rows[key] = r
    return rows

before, after = load(sys.argv[1]), load(sys.argv[2])
keys = sorted(set(before) | set(after))
missing = [k for k in keys if k not in before or k not in after]
hash_diff = [k for k in keys if k in before and k in after and before[k]['audio_hash'] != after[k]['audio_hash']]
print(f'rows before {len(before)}, after {len(after)}, missing {len(missing)}, audio hash differs {len(hash_diff)}')
for k in missing[:10]:
    print('  missing', k)
for k in hash_diff[:20]:
    print('  hash differs', k)

print()
print('| 歌 | GLAZE | 反相提示 % 改前 → 改后 | 慢路 ρ<−0.2 % 改前 → 改后 | 宽度保护 % 改前 → 改后 |')
print('|---|---|---|---|---|')
changed = 0
for k in keys:
    song, kind, glaze, clay, style = k
    if kind != 'render' or clay != 'auto' or glaze in ('0', '0.0') or k not in before or k not in after:
        continue
    b, a = before[k], after[k]
    pb, pa = b['phase_guard_pct'], a['phase_guard_pct']
    sb, sa = b['slow_inverted_pct'], a['slow_inverted_pct']
    wb, wa = b['width_guard_pct'], a['width_guard_pct']
    mark = '' if (pb, sb, wb) == (pa, sa, wa) else ' ←'
    changed += mark != ''
    print(f'| {song} | {glaze} | {pb} → {pa} | {sb} → {sa} | {wb} → {wa} |{mark}')
print(f'\nrows with any change: {changed}')
