import csv, glob, os, re, statistics, collections, math
B = os.path.expanduser("~/.cache/celadon-t045-diag/cmp")
caps = {'old': {'warm': (1.0, 2.5, 4.0), 'punchy': (0.8, 2.0, 3.0), 'clean': (1.5, 3.5, 6.0)},
        'new': {'warm': (1.0, 2.5, 6.0), 'punchy': (0.8, 2.0, 5.0), 'clean': (1.5, 3.5, 8.0)}, 'new3': {'warm': (2.0, 4.5, 7.0), 'punchy': (1.8, 4.0, 6.0), 'clean': (2.5, 5.5, 9.0)}}
def f(x):
    try: return float(x)
    except: return math.nan
rows = {}
for d in glob.glob(B + "/*/"):
    name = os.path.basename(os.path.normpath(d))
    m = re.match(r"(old|new3|new)-(track_\d+)-(\w+)-(\w+)-g(\d+)$", name)
    if not m: continue
    r = [x for x in csv.DictReader(open(d + "metrics.csv")) if x['kind'] == 'render']
    if r: rows[m.groups()] = r[0]
def q(v, p): v = sorted(v); return v[int(p * (len(v) - 1))] if v else math.nan
print("## 自动方案（GLAZE 100、无手动 Gain、通用档）新旧峰值预算对照")
print("| 风格 | 预算 | 保护层介入 >1% 的首数 | 介入占比中位 / 最大 % | 方案受限 >1% 首数 | 实测 GR 峰值中位 | P95 中位 / 超 P95+0.5 首数 | 均值中位 / 超均值+0.3 首数 | 成品 LUFS 中位 |")
print("|---|---|---|---|---|---|---|---|---|")
for style in ('warm', 'punchy', 'clean'):
    for label in ('old', 'new', 'new3'):
        sel = [r for (l, t, st, g, gain), r in rows.items() if l == label and st == style and g == 'general' and gain == '0']
        mean_c, p95_c, peak_c = caps[label][style]
        prot = [f(r['diag_protect_pct']) for r in sel]
        lim = [f(r['diag_plan_limited_pct']) for r in sel]
        peak = [f(r['limit_gr_max_db']) for r in sel]
        p95 = [f(r['diag_limit_gr_p95_db']) for r in sel]
        mean = [f(r['limit_gr_mean_db']) for r in sel]
        lufs = [f(r['lufs']) for r in sel]
        print(f"| {style} | {label} {mean_c:g}/{p95_c:g}/{peak_c:g} | {sum(1 for x in prot if x > 1)} / {len(sel)} | {statistics.median(prot):.1f} / {max(prot):.1f} | "
              f"{sum(1 for x in lim if x > 1)} | {statistics.median(peak):.2f} | {statistics.median(p95):.2f} / {sum(1 for x in p95 if x > p95_c + 0.5)} | "
              f"{statistics.median(mean):.2f} / {sum(1 for x in mean if x > mean_c + 0.3)} | {statistics.median(lufs):.2f} |")
# P95 形状：k = peak²/P95 − peak（模型 P95 = peak²/(peak + k)，出厂 k = 3）
ks = []
for (l, t, st, g, gain), r in rows.items():
    pk, p = f(r['limit_gr_max_db']), f(r['diag_limit_gr_p95_db'])
    if pk > 2.0 and p > 0.2:
        ks.append(pk * pk / p - pk)
print(f"\nP95 形状系数 k（峰值 > 2 dB 的 {len(ks)} 个渲染）：中位 {statistics.median(ks):.2f}、P10 {q(ks, 0.1):.2f}、P90 {q(ks, 0.9):.2f}（出厂 3.0；k 越小 P95 越大）")
for lab, grid in (('new', ('2', '4', '6', '8')), ('new3', ('2', '4', '6', '8', '10'))):
    print(f"\n## {lab} 预算下手动推 Gain 的撤回起点（House 子类、自动风格、GLAZE 100）")
    first = collections.Counter()
    for t in sorted({t for (l, t, st, g, gain) in rows if l == lab and st == 'auto'}):
        hit = None
        for gain in grid:
            r = [rr for (l, tt, st, g, gg), rr in rows.items() if l == lab and tt == t and st == 'auto' and gg == gain]
            if r and r[0].get('plan_level_withdrawn') == '1':
                hit = int(gain); break
        first[hit if hit is not None else 99] += 1
    print(f"撤回起点分布（dB；99 = 推到 {grid[-1]} dB 也不撤回）:", dict(sorted(first.items())))
