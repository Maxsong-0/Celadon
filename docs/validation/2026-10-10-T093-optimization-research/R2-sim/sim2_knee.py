import numpy as np, sys
sys.path.insert(0, '.')
from dsp_np import fft_filter, up, down, tp
fs = 48000; rng = np.random.default_rng(93)
T = 8.0; n = int(fs*T); t = np.arange(n)/fs
bpm = 124; beat = 60/bpm
x = np.zeros(n)
for k in range(int(T/beat)):
    s = int(k*beat*fs); m = min(n - s, int(0.4*fs)); tt = np.arange(m)/fs
    f = 45 + 60*np.exp(-tt/0.03); ph = 2*np.pi*np.cumsum(f)/fs
    x[s:s+m] += 0.9*np.sin(ph)*np.exp(-tt/0.11)
saw = 2*((t*55) % 1) - 1; bass = fft_filter(saw, fs, hi=300)
gate = ((t/beat) % 1 > 0.5).astype(float); x += 0.35*bass*gate
hn = fft_filter(rng.standard_normal(n), fs, lo=7000)
env = np.exp(-((t/(beat/4)) % 1)*(beat/4)/0.02); x += 0.15*hn*env
for f0 in (220, 261.6, 329.6): x += 0.05*np.sin(2*np.pi*f0*t)
x /= np.max(np.abs(x))
def softclip(u, T, k):
    a = np.abs(u); ks = k*T; kw = T - ks
    y = np.where(a <= ks, a, ks + kw*np.tanh((a - ks)/max(kw,1e-9)))
    return np.sign(u)*y
def proc(u, T, k):
    U = up(u, 4)
    yc = np.clip(U, -T, T) if k >= 0.999 else softclip(U, T, k)
    return down(yc, 4)
tp0 = tp(x)
def kw(u):
    v = fft_filter(u, fs, lo=60); return 10*np.log10(np.mean(v**2))
L0 = kw(x)
U0 = up(x, 4)
for target_db in (2.0, 4.0):
    goal = tp0*10**(-target_db/20)
    print(f"--- 目标：真峰值降 {target_db} dB（Clip 级单独承担）")
    for k in (0.5, 0.7, 0.85, 0.95, 1.0):
        lo, hi = 0.05, 1.5
        for _ in range(30):
            mid = 0.5*(lo+hi); y = proc(x, mid, k)
            if tp(y) > goal: hi = mid
            else: lo = mid
        y = proc(x, lo, k); r = y - x
        e_res = 10*np.log10(np.mean(r**2)/np.mean(x**2))
        e_hf = 10*np.log10(np.mean(fft_filter(r, fs, lo=8000)**2)/np.mean(x**2))
        e_mid = 10*np.log10(np.mean(fft_filter(r, fs, lo=1000, hi=8000)**2)/np.mean(x**2))
        touched = np.mean(np.abs(U0) > k*lo)
        print(f"knee={k:.2f} 阈值 {20*np.log10(lo/tp0):6.2f} dB  残差/信号 {e_res:6.1f}  残差1-8k {e_mid:6.1f}  残差>8k {e_hf:6.1f} dB  被整形 {100*touched:5.2f}%  K近似响度(同真峰值) {kw(y)-L0+target_db:+.2f} dB")
