"""T-044：压力反馈第一条触发量改成先试口径（100 ms 块峰值 P99 比）后，降档阶梯在各歌上的走法（模型，原曲输入，近似：降档立即生效、不做 3 s 斜坡）。
口径：W10 / W30 = 滑动 10 s / 30 s（窗满才判）；CUM = 自上次降档起累计（≥ 10 s 才判）。触发 = 统计量 > 0.2 dB 连续 1 s 且距上次降档 ≥ 4 s；降档后窗清空。"""
import sys, numpy as np, soundfile as sf
sys.path.insert(0, '.')
from guard_model import lowcut
S = '/home/maxsong/.cache/celadon-render/songs-9a355b99857c/'
alias = {"deep": "/home/maxsong/.cache/celadon-t044/in/deep-track_0155328.wav", "tech": "/home/maxsong/.cache/celadon-t044/in/tech-track_0046047.wav"}
cases = [("WTF", 10.58), ("will we be okay", 13.72), ("somggggg", 11.24), ("deep", 10.59), ("emotapes", 11.58), ("tech", 13.6),
         ("somggggg_mp3", 11.42), ("fule", 12.15), ("woqu", 16.25), ("woqu", 20.0)]
def bpk(s, fs):
    blk = int(round(0.1*fs)); n = len(s)//blk
    return np.abs(s[:n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
def p99(a): return np.percentile(a, 99)
for name, fc0 in cases:
    x, fs = sf.read(alias.get(name, S + name + ".wav"), dtype='float64', always_2d=True)
    px = bpk(x, fs)
    valid = px > px.max()*10**(-30/20)
    ladder = {1.0: fc0, 0.5: 10*(fc0/10)**0.5, 0.25: 10*(fc0/10)**0.25, 0.0: 0.0}
    py = {s: (bpk(lowcut(x, fs, f), fs) if f > 0 else px) for s, f in ladder.items()}
    out = []
    for mode in ["W10", "W30", "CUM"]:
        scale, since, above, wy, wx, steps = 1.0, 40, 0, [], [], []
        for t in range(len(px)):
            since += 1
            if not valid[t]:
                continue
            wy.append(py[scale][t]); wx.append(px[t])
            W = {"W10": 100, "W30": 300}.get(mode)
            if W: wy, wx = wy[-W:], wx[-W:]
            ready = len(wy) >= (W if W else 100)
            stat = 20*np.log10(p99(wy)/p99(wx)) if ready else 0.0
            above = above + 1 if (ready and stat > 0.2) else 0
            if above >= 10 and since >= 40 and scale > 0:
                scale = {1.0: 0.5, 0.5: 0.25, 0.25: 0.0}[scale]; since, above, wy, wx = 0, 0, [], []
                steps.append(f"{t/10:.0f}s")
        out.append(f"{mode}: scale {scale:.2f} steps [{', '.join(steps)}]")
    print(f"{name[:16]:16s} {fc0:5.2f} Hz | " + " | ".join(out), flush=True)
