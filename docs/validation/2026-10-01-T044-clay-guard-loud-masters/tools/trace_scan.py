import csv, glob, sys
for f in sorted(glob.glob(sys.argv[1] + "/*/trace/*clay-auto*.csv")):
    if "__listen" in f:
        continue
    print("==", f.split("/")[-3])
    prev = None
    for r in csv.DictReader(open(f)):
        t = float(r["t_sec"])
        scale = r["clay_pressure_scale"]
        on_grid = abs(t - round(t / 5.0) * 5.0) < 0.006
        if scale != prev or on_grid:
            print(f"  t {t:7.2f} scale {scale} cost {r['clay_peak_cost_db'] or '-':>7} blocks {r['clay_peak_cost_blocks']:>5} "
                  f"lowcut {r['clay_lowcut_hz']} blend {r['clay_blend_loss_db']}")
        prev = scale
