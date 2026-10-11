# 攻击整形窗形状 vs 增益调制谱（玩具模型，照 LimiterModule 的控制路：滑动最小 L+63 → 释放（瞬时下、30 ms 线性域指数回）→ 多级滑动平均）
import numpy as np
fs = 48000
def ma(x, n):
    if n <= 1: return x.copy()
    c = np.cumsum(np.concatenate([np.ones(n), x]))  # 初值 1（同 MovingAverage::clear）
    return (c[n:] - c[:-n]) / n
def chain(req, L, kernel):
    H = 63
    W = L + H
    # sliding min (causal, window W)
    from numpy.lib.stride_tricks import sliding_window_view
    pad = np.concatenate([np.ones(W-1), req])
    held = sliding_window_view(pad, W).min(axis=1)
    # release
    coef = np.exp(-1/(0.030*fs))
    env = np.empty_like(held); e = 1.0
    for i,t in enumerate(held):
        e = t if t <= e else t + (e - t)*coef
        env[i] = e
    if kernel == 'box1':
        g = ma(env, L)
    elif kernel == 'box2':   # 现行：L2 = floor(L/2), L1 = L - L2 + 1
        L2 = max(1, L//2); L1 = max(1, L - L2 + 1); g = ma(ma(env, L1), L2)
    elif kernel == 'box3':   # 三级：L1+L2+L3-2 = L
        a = (L + 2)//3; b = (L + 2 - a)//2; c = L + 2 - a - b; g = ma(ma(ma(env, a), b), c)
    elif kernel == 'hann':   # 升余弦核，长度 L，归一
        w = np.hanning(L + 2)[1:-1]; w /= w.sum()
        pad2 = np.concatenate([np.ones(L-1), env]); g = np.convolve(pad2, w, mode='valid')
    return g
N = fs  # 1 s
res = {}
for L in (48, 96):
    for depth_db in (3.0, 6.0):
        req = np.ones(N); req[N//4] = 10**(-depth_db/20)   # 单个过载样本（一个瞬态峰）
        # 也做一个 20 ms 过载（持续过载）
        req2 = np.ones(N); req2[N//4:N//4+960] = 10**(-depth_db/20)
        for name, r in (('spike', req), ('20ms', req2)):
            for k in ('box1','box2','box3','hann'):
                g = chain(r, L, k)
                m = g - 1.0
                # 增益调制谱：|FFT(g-1)|²，按偏离载波的频率 f 统计能量占比
                M = np.abs(np.fft.rfft(m * np.hanning(len(m))))**2
                f = np.fft.rfftfreq(len(m), 1/fs)
                tot = M.sum()
                def frac(lo): return 10*np.log10(M[f>lo].sum()/tot + 1e-30)
                res[(L, depth_db, name, k)] = (frac(200), frac(1000), frac(3000), frac(8000))
for key in sorted(res):
    L, d, name, k = key
    a,b,c,e = res[key]
    print(f"L={L:3d} depth={d:.0f}dB {name:5s} {k:5s}  >200Hz {a:7.1f}  >1k {b:7.1f}  >3k {c:7.1f}  >8k {e:7.1f} dB")
