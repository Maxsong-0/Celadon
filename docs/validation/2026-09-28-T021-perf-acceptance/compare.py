# 对比基线（v0.1.6.2）与当前的 GRID 行：每轮各自相减，两轮都列出来看一致性。
import sys, collections
def load(path):
    rows = {}
    for line in open(path, errors="replace"):
        if not line.startswith("GRID,"): continue
        f = line.strip().split(",")
        kv = dict(x.split("=", 1) for x in f[5:] if "=" in x)
        rows[(f[2], float(f[3]), int(f[4]))] = {k: float(kv[k]) for k in ("avg%", "worst%", "structWorst%", "structWorst64k%")}
    return rows
S = sys.argv[1]
b = [load(f"{S}/base-grid-{r}.log") for r in (1, 2)]
c = [load(f"{S}/cur-grid-{r}.log") for r in (1, 2)]
keys = sorted(set(b[0]) & set(c[0]), key=lambda k: (k[0], k[1], k[2]))
print(f"{'scenario':10s} {'sr':>6s} {'bs':>4s} | {'avg base':>9s} {'avg cur':>8s} {'Δavg r1/r2':>13s} | {'sw base':>7s} {'sw cur':>7s} {'Δsw r1/r2':>13s} | {'sw64k b':>7s} {'sw64k c':>7s} {'Δsw64k r1/r2':>14s}")
for k in keys:
    if not all(k in d for d in b + c): continue
    avg = [c[r][k]["avg%"] - b[r][k]["avg%"] for r in (0, 1)]
    sw  = [c[r][k]["structWorst%"] - b[r][k]["structWorst%"] for r in (0, 1)]
    swl = [c[r][k]["structWorst64k%"] - b[r][k]["structWorst64k%"] for r in (0, 1)]
    print(f"{k[0]:10s} {k[1]:6.0f} {k[2]:4d} | {b[0][k]['avg%']:9.3f} {c[0][k]['avg%']:8.3f} {avg[0]:+6.3f}/{avg[1]:+6.3f} | "
          f"{b[0][k]['structWorst%']:7.2f} {c[0][k]['structWorst%']:7.2f} {sw[0]:+6.2f}/{sw[1]:+6.2f} | "
          f"{b[0][k]['structWorst64k%']:7.2f} {c[0][k]['structWorst64k%']:7.2f} {swl[0]:+6.2f}/{swl[1]:+6.2f}")
