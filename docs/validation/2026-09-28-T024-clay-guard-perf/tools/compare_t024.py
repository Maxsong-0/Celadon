# T-024 Mac 验收的对比表（mac-acceptance.sh compare 调用）。
# 用法：python3 compare_t024.py <日志目录> [T-021 的 modules-after-T020.log]
# 日志目录里：base/t024/head-grid-{1,2}.log，t024/head-modules-{1,2}.log（缺的跳过）。
# 读法与 T-021 compare.py 相同：每轮各自相减，两轮都列；看平均与结构性最坏（structWorst / structWorst64k），不看单次最坏。
import os, sys

def load(path, tag):
    rows = {}
    if not os.path.exists(path):
        return None
    for line in open(path, errors="replace"):
        if not line.startswith(tag + ","):
            continue
        f = line.strip().split(",")
        kv = {}
        for x in f[5:]:
            if "=" in x:
                k, v = x.split("=", 1)
                try:
                    kv[k] = float(v.rstrip("%"))
                except ValueError:
                    pass
        rows[(f[2], float(f[3]), int(f[4]))] = kv
    return rows

S = sys.argv[1]
T021_MODULES = sys.argv[2] if len(sys.argv) > 2 else None
grid = {t: [load(f"{S}/{t}-grid-{r}.log", "GRID") for r in (1, 2)] for t in ("base", "t024", "head")}
mods = {t: [load(f"{S}/{t}-modules-{r}.log", "MODULES") for r in (1, 2)] for t in ("t024", "head")}

def have(runs):
    return all(r is not None for r in runs)

def grid_table(cur, title):
    b, c = grid["base"], grid[cur]
    if not (have(b) and have(c)):
        print(f"\n## {title}：缺日志，跳过")
        return
    print(f"\n## {title}（{cur} − base，每轮各自相减）")
    print(f"{'scenario':10s} {'sr':>6s} {'bs':>4s} | {'avg b':>7s} {'avg c':>7s} {'Δavg r1/r2':>15s} | "
          f"{'sw b':>6s} {'sw c':>6s} {'Δsw r1/r2':>13s} | {'sw64k b':>7s} {'sw64k c':>7s} {'Δsw64k r1/r2':>13s}")
    keys = sorted(set(b[0]) & set(c[0]))
    for k in keys:
        if not all(k in d for d in b + c):
            continue
        d = lambda m: [c[r][k][m] - b[r][k][m] for r in (0, 1)]
        avg, sw, sl = d("avg%"), d("structWorst%"), d("structWorst64k%")
        print(f"{k[0]:10s} {k[1]:6.0f} {k[2]:4d} | {b[0][k]['avg%']:7.3f} {c[0][k]['avg%']:7.3f} {avg[0]:+7.3f}/{avg[1]:+7.3f} | "
              f"{b[0][k]['structWorst%']:6.2f} {c[0][k]['structWorst%']:6.2f} {sw[0]:+6.2f}/{sw[1]:+6.2f} | "
              f"{b[0][k]['structWorst64k%']:7.2f} {c[0][k]['structWorst64k%']:7.2f} {sl[0]:+6.2f}/{sl[1]:+6.2f}")

def budget_summary(cur, title):
    b, c = grid["base"], grid[cur]
    if not (have(b) and have(c)):
        return
    print(f"\n## {title}：两个预算格（5 场景 × 2 轮）")
    for (sr, bs, metrics) in ((48000.0, 512, ("avg%",)), (192000.0, 32, ("structWorst%", "structWorst64k%"))):
        for m in metrics:
            vals = []
            for k in sorted(b[0]):
                if k[1] == sr and k[2] == bs and all(k in d for d in b + c):
                    vals += [(k[0], r + 1, c[r][k][m] - b[r][k][m]) for r in (0, 1)]
            if not vals:
                continue
            ds = [v[2] for v in vals]
            worst = max(vals, key=lambda v: v[2])
            print(f"  {sr/1000:.0f}k/{bs} {m:16s} Δ 最小 {min(ds):+.3f}  均值 {sum(ds)/len(ds):+.3f}  最大 {max(ds):+.3f}"
                  f"（{worst[0]} 第 {worst[1]} 轮）")

def modules_table():
    t, h = mods["t024"], mods["head"]
    ref = load(T021_MODULES, "MODULES") if T021_MODULES else None
    if not (have(t) or have(h)):
        print("\n## 修坯一级（modules）：缺日志，跳过")
        return
    print("\n## 修坯一级（modules 模式 clay 列；ref = T-021 的 modules-after-T020.log，即 T-024 之前）")
    print("注意：head 的 modules 模式起点是插件默认参数（T-032 R10-01），t024 与 ref 是结构体默认；场景里写了的参数两边相同，"
          "没写的可能不同（default 场景最明显：head 是 GLAZE 35，t024 / ref 是 0）。")
    print(f"{'scenario':10s} {'sr':>6s} {'bs':>4s} | {'ref avg':>7s} {'t024 avg r1/r2':>15s} {'head avg r1/r2':>15s} {'Δ h−t':>7s} | "
          f"{'ref sw64k':>9s} {'t024 sw64k r1/r2':>17s} {'head sw64k r1/r2':>17s} {'Δ h−t':>7s}")
    keys = sorted(set().union(*[set(d) for d in t + h if d is not None]))
    def pair(runs, k, m):
        v = [d[k][m] if (d is not None and k in d and m in d[k]) else float("nan") for d in runs]
        return v, f"{v[0]:7.3f}/{v[1]:7.3f}"
    for k in keys:
        if k[0] == "auto50xover":
            continue
        ta, tas = pair(t, k, "clay_avg%")
        ha, has = pair(h, k, "clay_avg%")
        tw, tws = pair(t, k, "clay_structWorst64k%")
        hw, hws = pair(h, k, "clay_structWorst64k%")
        ra = ref[k]["clay_avg%"] if ref and k in ref else float("nan")
        rw = ref[k].get("clay_structWorst64k%", float("nan")) if ref and k in ref else float("nan")
        da = (ha[0] + ha[1]) / 2 - (ta[0] + ta[1]) / 2
        dw = (hw[0] + hw[1]) / 2 - (tw[0] + tw[1]) / 2
        print(f"{k[0]:10s} {k[1]:6.0f} {k[2]:4d} | {ra:7.3f} {tas:>15s} {has:>15s} {da:+7.3f} | "
              f"{rw:9.3f} {tws:>17s} {hws:>17s} {dw:+7.3f}")

grid_table("t024", "A. 修坯的整插件增量：t024 对 v0.1.6.2（定预算看这张）")
budget_summary("t024", "A")
modules_table()
grid_table("head", "C. 今天的整插件总增量：head 对 v0.1.6.2（含非修坯改动，只作参考）")
budget_summary("head", "C")
for t in ("base", "t024", "head"):
    for r in (1, 2):
        p = f"{S}/{t}-grid-{r}.log"
        if os.path.exists(p):
            power = [l.strip() for l in open(p, errors="replace") if l.startswith(("POWER", "START", "END"))]
            print(f"{t}-grid-{r}: " + "  ".join(power))
