# T-047 3c「只对残差做 ADAA」的半样本错位：输出峰值与混叠，对照全量 ADAA 与现行纯 K× 过采样（理想 FFT 上下采样，玩具）
import numpy as np, sys
sys.path.insert(0, '.')
from dsp_np import up, down, tp
fs = 48000
def f_soft(u, T, k=0.7):
    a = np.abs(u); ks = k*T; kw = T - ks
    return np.sign(u)*np.where(a <= ks, a, ks + kw*np.tanh((a - ks)/kw))
def F_soft(u, T, k=0.7):   # 原函数（偶函数），F(0)=0
    a = np.abs(u); ks = k*T; kw = T - ks
    inner = 0.5*a*a
    z = (a - ks)/kw
    outer = 0.5*ks*ks + ks*(a - ks) + kw*kw*np.log(np.cosh(np.minimum(z, 300)))  # z 大时 log cosh ≈ z − ln2
    outer = np.where(z > 300, 0.5*ks*ks + ks*(a - ks) + kw*kw*(z - np.log(2)), outer)
    return np.where(a <= ks, inner, outer)
def f_hard(u, T): return np.clip(u, -T, T)
def F_hard(u, T):
    a = np.abs(u); return np.where(a <= T, 0.5*a*a, T*a - 0.5*T*T)
def adaa1(xo, f, F, T):
    x0 = np.concatenate([[0.0], xo[:-1]]); d = xo - x0
    with np.errstate(divide='ignore', invalid='ignore'):
        y = (F(xo, T) - F(x0, T))/d
    small = np.abs(d) < 1e-7
    y[small] = f(0.5*(xo[small] + x0[small]), T)
    return y
def run(x, K, f, F, T, mode):
    xo = up(x, K)
    if mode == 'plain':  y = f(xo, T)
    elif mode == 'full': y = adaa1(xo, f, F, T)
    elif mode == 'resid':   # out = x + ADAA(r)，r = f − x，R = F − x²/2；干路不做 2 点平均
        y = xo + adaa1(xo, lambda u, T: f(u, T) - u, lambda u, T: F(u, T) - 0.5*u*u, T)
    elif mode == 'resid_avg':  # 干路也做 2 点平均（= 全量 ADAA 的线性部分）
        x0 = np.concatenate([[0.0], xo[:-1]])
        y = 0.5*(xo + x0) + adaa1(xo, lambda u, T: f(u, T) - u, lambda u, T: F(u, T) - 0.5*u*u, T)
    return down(y, K)
n = fs  # 1 s
t = np.arange(n)/fs
T = 0.5
for f0 in (1000.0, 3100.0):
    x = 1.0*np.sin(2*np.pi*f0*t)   # 超阈值 6 dB
    for name, f, F in (('hard', f_hard, F_hard), ('soft0.7', f_soft, F_soft)):
        for K in (1, 2, 4):
            out = []
            for mode in ('plain', 'full', 'resid', 'resid_avg'):
                y = run(x, K, f, F, T, mode)
                # 混叠：非谐波位置的能量 / 基波能量
                Y = np.abs(np.fft.rfft(y*np.hanning(n)))**2; fr = np.fft.rfftfreq(n, 1/fs)
                harm = np.zeros_like(fr, bool)
                for h in range(1, int(24000/f0)+1): harm |= np.abs(fr - h*f0) < 15
                fund = Y[np.abs(fr - f0) < 15].sum()
                alias = 10*np.log10(Y[~harm & (fr > 20) & (fr < 20000)].sum()/fund + 1e-30)
                peak = 20*np.log10(tp(y[n//4:3*n//4])/T)
                out.append(f"{mode}: 峰 {peak:+.2f} dB / 混叠 {alias:6.1f}")
            print(f"f0={f0:6.0f} {name:7s} K={K}  " + " | ".join(out))
