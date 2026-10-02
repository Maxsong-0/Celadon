import csv, glob, os, re, statistics, collections, math
base = os.path.expanduser("~/.cache/celadon-t045-diag/out")
data = collections.defaultdict(dict)
for d in glob.glob(base + "/*/"):
    m = re.match(r"(track_\d+)-g(\d+)-(\w+)$", os.path.basename(os.path.normpath(d)))
    if not m:
        continue
    track, gain, genre = m.group(1), int(m.group(2)), m.group(3)
    rows = [r for r in csv.DictReader(open(d + "metrics.csv")) if r["kind"] == "render"]
    if rows:
        data[(track, "general" if genre == "general" else "house")][gain] = rows[0]
def f(x):
    try: return float(x)
    except: return math.nan
summary = collections.defaultdict(list)
detail = []
for (track, mode), g in sorted(data.items()):
    gains = sorted(g)
    w = g[0]
    style = max(("warm", "punchy", "clean"), key=lambda s: f(w["weight_" + s]))
    first = next((x for x in gains if g[x]["plan_level_withdrawn"] == "1"), None)
    summary[(mode, style)].append(first if first is not None else 99)
    summary[(mode, "all")].append(first if first is not None else 99)
    if first is None:
        detail.append(f"{mode} {track} {style}: 8 dB 也不撤回")
        continue
    r = g[first]
    idx = gains.index(first)
    prev = g[gains[idx - 1]] if idx > 0 else None
    detail.append(f"{mode} {track} {style}: 起点 {first} dB；实测 GR 均值 {f(r['limit_gr_mean_db']):.2f} / 最大 {f(r['limit_gr_max_db']):.2f}"
                  f"（预算 {f(r['budget_limit_gr_mean_db']):.2f} / {f(r['budget_limit_gr_peak_db']):.2f}）；"
                  f"前一档 GR 均值 {f(prev['limit_gr_mean_db']) if prev else math.nan:.2f}、校准 {prev['listen_calibration_valid'] if prev else '-'}；"
                  f"输入 {f(w['profile_lufs']):.1f} LUFS、GLAZE 自动增益 {f(w['plan_gain_db']):.2f} dB")
for key in sorted(summary):
    v = summary[key]
    print(key, "撤回起点分布", dict(sorted(collections.Counter(v).items())), "中位", statistics.median(v), "首数", len(v))
print()
print("\n".join(detail))
