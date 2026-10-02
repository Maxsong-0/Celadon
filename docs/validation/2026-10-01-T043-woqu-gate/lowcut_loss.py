#!/usr/bin/env python3
# T-043：量一首歌的低频能量分布，以及 Clay 低切（4 阶 Butterworth 高通，两级 RBJ，Q = 0.5412 / 1.3066）在若干截止频率下
# 让 BS.1770 整体响度下降多少（只滤波、不做别的处理；K 计权 + 400 ms 块 + 两级门，与 LoudnessMeter 同一口径）。
# 用法：lowcut_loss.py <wav> [fc ...]
import sys
import numpy as np
import scipy.io.wavfile as wf
from scipy import signal


def k_weight(fs):
    # BS.1770-4 的两级（按 fs 重新推导系数的通用写法）
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = np.tan(np.pi * f0 / fs); Vh = 10 ** (G / 20); Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = np.tan(np.pi * f0 / fs)
    a0 = 1 + K / Q + K * K
    b2 = [1, -2, 1]
    a2 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    return (b1, a1), (b2, a2)


def lufs(x, fs):
    (b1, a1), (b2, a2) = k_weight(fs)
    y = signal.lfilter(b2, a2, signal.lfilter(b1, a1, x, axis=0), axis=0)
    blk, hop = int(0.4 * fs), int(0.1 * fs)
    ms = np.array([np.sum(np.mean(y[i:i + blk] ** 2, axis=0)) for i in range(0, len(y) - blk + 1, hop)])
    l = -0.691 + 10 * np.log10(ms + 1e-30)
    g = ms[l > -70]
    rel = -0.691 + 10 * np.log10(np.mean(g)) - 10
    g2 = ms[(l > -70) & (l > rel)]
    return -0.691 + 10 * np.log10(np.mean(g2))


def rbj_hp(fc, Q, fs):
    w = 2 * np.pi * fc / fs; al = np.sin(w) / (2 * Q); c = np.cos(w)
    b = np.array([(1 + c) / 2, -(1 + c), (1 + c) / 2]); a = np.array([1 + al, -2 * c, 1 - al])
    return b / a[0], a / a[0]


def lowcut(x, fc, fs):
    for Q in (0.54119610014619698, 1.3065629648763766):
        b, a = rbj_hp(fc, Q, fs)
        x = signal.lfilter(b, a, x, axis=0)
    return x


def main():
    path = sys.argv[1]
    fcs = [float(v) for v in sys.argv[2:]] or [16.25, 20.0, 25.0]
    fs, x = wf.read(path)
    if x.dtype.kind == 'i':
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max + 1)
    x = x.astype(np.float64)
    if x.ndim == 1:
        x = np.stack([x, x], axis=1)
    x = x[:, :2]
    print(f"{path}: fs {fs}, {len(x) / fs:.2f} s, sample peak {20 * np.log10(np.max(np.abs(x))):.2f} dBFS")
    base = lufs(x, fs)
    print(f"原曲整体响度 {base:.3f} LUFS")

    # 低频能量分布：中声道功率谱（Welch，1 s 窗 → 1 Hz 分辨率），按段报占全频功率的比例
    m = x.mean(axis=1)
    f, p = signal.welch(m, fs, nperseg=fs, noverlap=fs // 2)
    tot = p.sum()
    edges = [0, 10, 16, 20, 25, 30, 35, 40, 50, 60, 80, 120, 250]
    print("中声道功率占比（dB，相对全频）：")
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = (f >= lo) & (f < hi)
        print(f"  {lo:>3}–{hi:<3} Hz {10 * np.log10(p[sel].sum() / tot + 1e-30):7.2f}")
    sel = (f >= 15) & (f < 120)
    pk = f[sel][np.argmax(p[sel])]
    print(f"  15～120 Hz 最强的频率 {pk:.0f} Hz")

    for fc in fcs:
        y = lowcut(x, fc, fs)
        print(f"低切 {fc:6.2f} Hz：整体响度 {lufs(y, fs) - base:+.3f} LU，"
              f"样本峰值 {20 * np.log10(np.max(np.abs(y)) / np.max(np.abs(x))):+.3f} dB")


if __name__ == '__main__':
    main()
