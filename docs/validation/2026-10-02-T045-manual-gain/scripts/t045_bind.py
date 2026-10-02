import csv, glob, os, re, collections, math
B = os.path.expanduser("~/.cache/celadon-t045-diag/cmp")
def f(x):
    try: return float(x)
    except: return math.nan
caps = {'warm': (1.0, 2.5, 6.0), 'punchy': (0.8, 2.0, 5.0), 'clean': (1.5, 3.5, 8.0)}
runs = collections.defaultdict(dict)
for d in glob.glob(B + "/new-*-auto-*-g*/"):
    m = re.match(r"new-(track_\d+)-auto-(\w+)-g(\d+)$", os.path.basename(os.path.normpath(d)))
    r = [x for x in csv.DictReader(open(d + "metrics.csv")) if x['kind'] == 'render']
    if m and r: runs[m.group(1)][int(m.group(3))] = r[0]
bind = collections.Counter(); lines = []
for t, g in sorted(runs.items()):
    first = next((x for x in sorted(g) if g[x]['plan_level_withdrawn'] == '1'), None)
    if first is None: continue
    r = g[first]
    style = max(('warm', 'punchy', 'clean'), key=lambda s: f(r['weight_' + s]))
    mc, pc, kc = caps[style]
    mean, p95, peak = f(r['limit_gr_mean_db']), f(r['diag_limit_gr_p95_db']), f(r['limit_gr_max_db'])
    over = [n for n, v, c in (('均值', mean, mc), ('P95', p95, pc), ('峰值', peak, kc)) if v > c]
    bind[' + '.join(over) or '实测都没超（模型判超）'] += 1
    lines.append(f"{t} {style} 起点 {first} dB：实测均值 {mean:.2f}/{mc}  P95 {p95:.2f}/{pc}  峰值 {peak:.2f}/{kc}")
print("新预算下撤回那一档，实测超出的是：", dict(bind))
print("\n".join(lines[:12]))
