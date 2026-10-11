# 玩具模型：底鼓触发的宽带限幅对踩镲的「压低」（泵）与底鼓本身的失真；对照「低频预限」（只压 150 Hz 以下的一段，宽带级兜底）
import numpy as np, sys
sys.path.insert(0, '.')
from dsp_np import fft_filter
from numpy.lib.stride_tricks import sliding_window_view
fs = 48000; rng = np.random.default_rng(7)
T = 8.0; n = int(fs*T); t = np.arange(n)/fs; beat = 60/124
kick = np.zeros(n)
for k in range(int(T/beat)):
    s = int(k*beat*fs); m = min(n - s, int(0.4*fs)); tt = np.arange(m)/fs
    f = 45 + 60*np.exp(-tt/0.03); ph = 2*np.pi*np.cumsum(f)/fs
    kick[s:s+m] += np.sin(ph)*np.exp(-tt/0.11)
hats = fft_filter(rng.standard_normal(n), fs, lo=6000)
hats *= 0.12/np.sqrt(np.mean(hats**2))
x = kick + hats
L = 96; H = 63
def limiter_gain(det, ceil, rel_ms):
    req = np.where(det > ceil, ceil/np.maximum(det, 1e-12), 1.0)
    W = L + H; pad = np.concatenate([np.ones(W-1), req]); held = sliding_window_view(pad, W).min(axis=1)
    c = np.exp(-1/(rel_ms*1e-3*fs)); env = np.empty(n); e = 1.0
    for i, v in enumerate(held):
        e = v if v <= e else v + (e - v)*c; env[i] = e
    def ma(z, k):
        cs = np.cumsum(np.concatenate([np.ones(k), z])); return (cs[k:] - cs[:-k])/k
    L2 = L//2; L1 = L - L2 + 1
    return ma(ma(env, L1), L2)
D = L + H - 1
def delay(z): return np.concatenate([np.zeros(D), z[:-D]])
ceil = 10**(-1/20)
gain_in = 10**(6/20)/np.max(np.abs(x))   # 底鼓峰超 Ceiling 约 6 dB
xs = x*gain_in
def metrics(y, name, extra=''):
    ref_h = delay(hats*gain_in); hf_out = fft_filter(y, fs, lo=6000)
    # 踩镲被压掉的量：输出高频能量 / 输入高频能量（dB）；kick 后 0~60 ms 与 150~300 ms 的高频增益差 = 泵感深度
    loss = 10*np.log10(np.mean(hf_out**2)/np.mean(ref_h**2))
    on = []; off = []
    for k in range(1, int(T/beat)-1):
        s = int(k*beat*fs) + D
        a = slice(s, s+int(0.06*fs)); b = slice(s+int(0.15*fs), s+int(0.30*fs))
        on.append(np.mean(hf_out[a]**2)/np.mean(ref_h[a]**2)); off.append(np.mean(hf_out[b]**2)/np.mean(ref_h[b]**2))
    pump = 10*np.log10(np.mean(off)/np.mean(on))
    dist = fft_filter(y, fs, lo=200, hi=4000); kref = delay(kick*gain_in)
    d = 10*np.log10(np.mean(dist**2)/np.mean(kref**2) + 1e-30)
    lk = 10*np.log10(np.mean(fft_filter(y, fs, lo=60)**2))
    print(f"{name:34s} 踩镲整体 {loss:+6.2f} dB  泵深 {pump:5.2f} dB  底鼓失真(200-4k) {d:6.1f} dB  响度近似 {lk:6.2f} {extra}")
for rel in (30, 80):
    g = limiter_gain(np.abs(xs), ceil*0.9605, rel); y = np.clip(delay(xs)*g, -ceil, ceil)
    metrics(y, f"宽带 释放 {rel} ms")
# 低频预限：LF = 150 Hz 以下（玩具里用 FFT 理想分频），只给 LF 一段增益；宽带级兜底
lf = fft_filter(xs, fs, hi=150); hf = xs - lf
for rel in (30, 80, 150):
    # 需求：峰值超出时只缩 LF，使 |hf + a·lf| ≤ c'（逐样本求 a，下限 a≥0.25 即最多 −12 dB）
    cp = ceil*0.9605; a = np.ones(n)
    over = np.abs(xs) > cp
    num = np.sign(lf)*cp - hf
    with np.errstate(divide='ignore', invalid='ignore'):
        sol = np.where(np.abs(lf) > 1e-9, num/lf, 1.0)
    a[over] = np.clip(sol[over], 0.25, 1.0)
    gL = limiter_gain(np.where(a < 1, cp/np.maximum(a, 1e-9), 0.0) + 0.0, cp, rel)  # 用 req=a 的同一控制路
    # 直接把 a 当需求增益走同一控制路
    W = L + H; pad = np.concatenate([np.ones(W-1), a]); held = sliding_window_view(pad, W).min(axis=1)
    c = np.exp(-1/(rel*1e-3*fs)); env = np.empty(n); e = 1.0
    for i, v in enumerate(held):
        e = v if v <= e else v + (e - v)*c; env[i] = e
    cs = lambda z, k: (np.cumsum(np.concatenate([np.ones(k), z]))[k:] - np.cumsum(np.concatenate([np.ones(k), z]))[:-k])/k
    gL = cs(cs(env, L - L//2 + 1), L//2)
    pre = delay(hf) + delay(lf)*gL
    # 宽带兜底级（同一延迟结构，玩具里不再加延迟对齐，只看能量）
    g2 = limiter_gain(np.abs(pre), cp, 30)
    y = np.clip(delay(pre)*g2, -ceil, ceil)
    # 对齐：y 比 pre 晚 D，metrics 里参考延迟一次 D；这里再补一次
    y = y[D:]; y = np.concatenate([y, np.zeros(D)])
    metrics(y, f"低频预限 释放 {rel} ms + 宽带兜底", f"(兜底级最大 GR {-20*np.log10(g2.min()):.2f} dB)")
