#!/usr/bin/env python3
"""T-074：真歌 trace（临时树多了 clay_peak_guard_active / clay_peak_guard_kept 两列）→ 每首歌每个产品：灯亮的块占比、保留比分布、亮灭切换次数、守护动作的块占比。"""
import csv, glob, os, statistics, sys

out = sys.argv[1]
print(f"{'song':14s} {'product':28s} {'blocks':>7s} {'lowcut>0':>9s} {'guard acted':>12s} {'lit %':>7s} {'kept min':>9s} {'kept p50':>9s} {'kept max':>9s} {'switches':>9s} {'first lit s':>12s}")
for path in sorted(glob.glob(f"{out}/*/trace/*.csv")):
    if path.endswith("__listen.csv"):
        continue
    song = path.split("/")[-3]
    product = os.path.basename(path)[:-4].split("__", 1)[1]
    rows = lit = low = guard = 0
    kept = []
    last = None
    switches = 0
    first = None
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows += 1
            try:
                a = int(r["clay_peak_guard_active"])
                k = float(r["clay_peak_guard_kept"])
                t = float(r["t_sec"])
                if float(r["clay_lowcut_hz"]) > 0:
                    low += 1
                if int(float(r["peak_guard_samples"])) > 0:
                    guard += 1
            except Exception:
                continue
            lit += a
            kept.append(k)
            if last is not None and a != last:
                switches += 1
            if a and first is None:
                first = t
            last = a
    if not kept:
        continue
    kept_sorted = sorted(kept)
    print(f"{song:14s} {product:28s} {rows:7d} {low:9d} {guard:12d} {100.0 * lit / rows:7.1f} {kept_sorted[0]:9.3f} {statistics.median(kept):9.3f} {kept_sorted[-1]:9.3f} {switches:9d} {('%.1f' % first) if first is not None else '-':>12s}")
