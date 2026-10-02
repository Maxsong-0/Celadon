"""T-044：累计 P99 口径 + 降档阶梯，按渲染工具的两遍模拟（第一遍 15 s 聆听后开始监测，第二遍从头重攒，缩放沿用），比较最少块数。
近似：降档立即生效（不做 3 s 斜坡），冷却 4 s 内的块不攒；原曲输入（插件里 Clay 前还有 Balance）。"""
import sys, numpy as np, soundfile as sf
sys.path.insert(0, '.')
from guard_model import lowcut
S = '/home/maxsong/.cache/celadon-render/songs-9a355b99857c/'
alias = {"deep": "in/deep-track_0155328.wav", "tech": "in/tech-track_0046047.wav"}
cases = [("WTF", 10.58), ("will we be okay", 13.72), ("somggggg", 11.24), ("deep", 10.59), ("emotapes", 11.58), ("tech", 13.6),
         ("somggggg_mp3", 11.42), ("fule", 12.15), ("woqu", 11.60)]
def bpk(s, fs):
    blk = int(round(0.1*fs)); n = len(s)//blk
    return np.abs(s[:n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
def p99(a): return np.percentile(a, 99)
def run(px, py, valid, start, scale, nmin):
    wy, wx, since, above, steps = [], [], 40, 0, []
    for t in range(start, len(px)):
        since += 1
        if not valid[t] or since <= 40 or scale == 0.0:
            continue
        wy.append(py[scale][t]); wx.append(px[t])
        n = len(wy)
        stat = 20*np.log10(p99(wy)/p99(wx)) if n >= 100 else None
        if nmin == 0:   # 两档：≥ 0.6 dB 满 100 块、> 0.2 dB 满 512 块
            hit = stat is not None and (stat >= 0.6 or (stat > 0.2 and n >= 512))
        else:
            hit = stat is not None and n >= nmin and stat > 0.2
        above = above + 1 if hit else 0
        if above >= 10:
            scale = {1.0: 0.5, 0.5: 0.25, 0.25: 0.0}[scale]; since, above, wy, wx = 0, 0, [], []
            steps.append(f"{t/10:.0f}")
    return scale, steps
for name, fc0 in cases:
    x, fs = sf.read(alias.get(name, S + name + ".wav"), dtype='float64', always_2d=True)
    px = bpk(x, fs); valid = px > px.max()*10**(-30/20)
    py = {s: (bpk(lowcut(x, fs, 10*(fc0/10)**s), fs) if s > 0 else px) for s in (1.0, 0.5, 0.25, 0.0)}
    out = []
    for nmin in (100, 512, 0):
        s1, st1 = run(px, py, valid, 150, 1.0, nmin)          # 第一遍：15 s 聆听后开始
        s2, st2 = run(px, py, valid, 0, s1, nmin)             # 第二遍：从头重攒
        out.append(f"N{nmin}: {s1:.2f}/{s2:.2f} [{','.join(st1)}|{','.join(st2)}]")
    print(f"{name[:16]:16s} {fc0:5.2f} {len(px)/10:5.0f}s | " + " | ".join(out), flush=True)
