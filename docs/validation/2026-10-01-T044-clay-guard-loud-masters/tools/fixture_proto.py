"""T-044：端到端回归夹具原型。48 kHz、20 Hz 低切（测试画像通透档的方案值）。量：新口径 P99 代价、旧压力格（2 s 平滑、> 0.2 连续 1 s 的时刻）、混合损失（K 加权、2 s）。"""
import sys, numpy as np
sys.path.insert(0, '.')
from guard_model import lowcut, guard
from scipy.signal import lfilter
from scipy.ndimage import maximum_filter1d, uniform_filter1d
fs = 48000; T = 24.0; n = int(T*fs); t = np.arange(n)/fs
def kweight(x):
    b1, a1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585]
    b2, a2 = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
    return lfilter(b2, a2, lfilter(b1, a1, x, axis=0), axis=0)
def limiter(s, c, look=0.005, rel=0.05):
    pk = maximum_filter1d(np.abs(s), 2*int(look*fs)+1)
    g = np.minimum(1.0, c/np.maximum(pk, 1e-12))
    # 释放：一阶上升
    out = np.empty_like(g); r = np.exp(-1/(rel*fs)); cur = 1.0
    for i in range(len(g)):
        cur = g[i] if g[i] < cur else 1 - (1-cur)*r
        out[i] = cur
    return s*out
def stats(name, mono, fc=20.0):
    x = np.stack([mono, mono], axis=1)
    y = lowcut(x, fs, fc); d = y - x
    g, press, B, req = guard(x, y, fs)
    out = x + g[:, None]*d
    blk = int(0.1*fs); nb = n//blk
    px = np.abs(x[:nb*blk]).max(axis=1).reshape(nb, blk).max(axis=1); py = np.abs(y[:nb*blk]).max(axis=1).reshape(nb, blk).max(axis=1)
    p99 = 20*np.log10(np.percentile(py[:100], 99)/np.percentile(px[:100], 99))
    p99all = 20*np.log10(np.percentile(py, 99)/np.percentile(px, 99))
    # 旧压力：10 ms 格平均 dB，按 2 s 一阶平滑（每格一步），> 0.2 连续 1 s
    a = 1 - np.exp(-0.01/2.0); s = 0; above = 0; first = None
    for k, v in enumerate(press):
        s += (v - s)*a; above = above + 0.01 if s > 0.2 else 0
        if above >= 1.0 and first is None: first = (k+1)*0.01
    kw_i, kw_o = kweight(y).astype(float)**2, kweight(out).astype(float)**2
    ei = uniform_filter1d(kw_i.sum(axis=1), int(2*fs)); eo = uniform_filter1d(kw_o.sum(axis=1), int(2*fs))
    blend = 10*np.log10(ei[int(2*fs):]/eo[int(2*fs):])
    print(f"{name}: peak {20*np.log10(np.abs(x).max()):.2f} dBFS | new P99 cost first10s {p99:+.3f} whole {p99all:+.3f} dB | old pressure mean {press.mean():.3f} max2s {uniform_filter1d(press,200).max():.3f} first-step {first} | blend 2s median {np.median(blend):.3f} max {blend.max():.3f} dB | g<1 {100*(g<1).mean():.1f}% mean g {g.mean():.3f}")
tw = 2*np.pi
m = 0.45*np.sin(tw*50*t) + 0.2*np.sin(tw*100*t + 0.7) + 0.08*np.sin(tw*150*t + 1.9)
rng = np.random.default_rng(7)
noise = rng.standard_normal(n)*0.02
for dAmp in [0.08, 0.12, 0.18]:
    dd = dAmp*np.sin(tw*2.5*t)
    stats(f"A limited  d={dAmp}", limiter(m + dd + noise, 0.6))
    stats(f"B raw      d={dAmp}", m + dd + noise)
