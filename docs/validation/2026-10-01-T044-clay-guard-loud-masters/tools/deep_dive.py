import sys, numpy as np, soundfile as sf
sys.path.insert(0, '.')
from guard_model import lowcut, guard
from scipy.signal import butter, sosfiltfilt
from scipy.ndimage import uniform_filter1d
def blockpk(s, fs, sec=0.1):
    blk = int(round(sec*fs)); n = len(s)//blk
    return np.abs(s[:n*blk]).max(axis=1).reshape(n, blk).max(axis=1)
path, fc = sys.argv[1], float(sys.argv[2])
x, fs = sf.read(path, dtype='float64', always_2d=True)
y = lowcut(x, fs, fc); d = y - x
print(f"== {path.split('/')[-1]} fc {fc}")
# (2) 触发事件的超界量分布：req<1 的样本上 |y|/B
g, press, B, req = guard(x, y, fs)
ov = 20*np.log10(np.abs(y).max(axis=1)[req < 1]/B[req < 1])
print(f"  trigger samples {np.sum(req<1)} ({100*np.mean(req<1):.2f}%), overshoot dB p50 {np.percentile(ov,50):.3f} p90 {np.percentile(ov,90):.3f} p99 {np.percentile(ov,99):.3f} max {ov.max():.3f}; frac <0.05 dB {np.mean(ov<0.05):.2f} <0.1 {np.mean(ov<0.1):.2f} <0.2 {np.mean(ov<0.2):.2f}")
# (1) 25-80 Hz 段 50 ms 电平：守护后 / 不守护 相对干路的 dB 波动
sos = butter(4, [25, 80], btype='band', fs=fs, output='sos')
def lvl(s):
    b = sosfiltfilt(sos, s.sum(axis=1)); e = uniform_filter1d(b*b, int(0.05*fs))
    return e[::int(0.01*fs)]
lx, ly = lvl(x), lvl(y)
mask = lx > lx.max()*1e-4
for hr in [0.0, 0.05, 0.1, 0.2]:
    g, press, B, req = guard(x, y, fs, hr)
    out = x + g[:, None]*d
    lo = lvl(out)
    dyo = 10*np.log10(lo[mask]/lx[mask]); dyy = 10*np.log10(ly[mask]/lx[mask])
    keep = 10*np.log10(((g[:, None]*d)**2).sum()/(d**2).sum())
    px, py, po = blockpk(x, fs), blockpk(y, fs), blockpk(out, fs)
    p99 = lambda a: np.percentile(a, 99)
    pmax = lambda a: np.max(20*np.log10(a/px))
    print(f"  hr {hr:4.2f}: g<1 {100*(g<1).mean():5.1f}% mean g {g.mean():.3f} retain {keep:+.2f} dB | 25-80Hz 50ms lvl vs dry: guarded mean {dyo.mean():+.3f} std {dyo.std():.3f}, fullcut mean {dyy.mean():+.3f} std {dyy.std():.3f} | "
          f"P99 cost {20*np.log10(p99(po)/p99(px)):+.3f} (unguarded {20*np.log10(p99(py)/p99(px)):+.3f}) blockmax rise {pmax(po):+.3f} (unguarded {pmax(py):+.3f}) song peak {20*np.log10(po.max()/px.max()):+.3f}")
# (4) 滑动 10 s 的 P99 代价（先试的统计量）vs 压力格 2 s 平滑
g, press, B, req = guard(x, y, fs)
px, py = blockpk(x, fs), blockpk(y, fs)
w = 100; costs = []
for s in range(0, len(px)-w, 10):
    costs.append(20*np.log10(np.percentile(py[s:s+w], 99)/np.percentile(px[s:s+w], 99)))
costs = np.array(costs)
ps = uniform_filter1d(press, 200)
print(f"  sliding-10s P99 cost: median {np.median(costs):+.3f} p90 {np.percentile(costs,90):+.3f} frac>0.2 {np.mean(costs>0.2):.2f}  | pressure 2s-mean: median {np.median(ps):.3f} p90 {np.percentile(ps,90):.3f} frac>0.2 {np.mean(ps>0.2):.3f}")
