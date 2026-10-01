#!/usr/bin/env python3
# T-042 诊断：照 BalanceModule（Hann、2048 点 @ ≤48k、hop = N/4、1/3 倍频程 27 段、bin 权重）与
# MusicAnalyzer::updateLiveStereo（逐段 −60 dBFS 门、300 / 120 ms 功率域平滑、运行均值起步、帧门 −70 dBFS）
# 逐帧复算实时立体声视图，另记全频总功率的同口径平滑，存 npz 供门限分析。只做诊断，不进仓库。
import sys, os, wave
import numpy as np

def read_audio(path):
    import soundfile as sf  # 109 的 venv 有就用
    x, sr = sf.read(path, dtype='float64', always_2d=True)
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2], sr

def band_weights(sr, n):
    bin_hz = sr / n
    half = 2 ** (1 / 6)
    w = np.hanning(n + 1)[:-1]  # 0.5(1 − cos(2πn/N))，与 C++ 同
    w = 0.5 * (1 - np.cos(2 * np.pi * np.arange(n) / n))
    scale = 2.0 / (n * np.sum(w * w))
    W = np.zeros((27, n // 2 + 1))
    for b in range(27):
        c = 1000.0 * 2 ** ((b - 14) / 3)
        lo, hi = c / half, c * half
        first = max(1, int(np.floor(lo / bin_hz - 0.5)) + 1)
        last = min(n // 2, int(np.ceil(hi / bin_hz + 0.5)) - 1)
        for k in range(first, last + 1):
            cl, ch = (k - 0.5) * bin_hz, (k + 0.5) * bin_hz
            ov = max(0.0, min(hi, ch) - max(lo, cl))
            W[b, k] = scale * ov / bin_hz
    return w, W

def frames(x, sr):
    order = 11
    while (1 << (order - 11)) * 48000.0 < sr:
        order += 1
    n = 1 << order
    hop = n // 4
    win, W = band_weights(sr, n)
    ends = np.arange(n, x.shape[0] + 1, hop)
    P = np.zeros((len(ends), 27)); X = np.zeros((len(ends), 27))
    chunk = 2048
    for i0 in range(0, len(ends), chunk):
        e = ends[i0:i0 + chunk]
        idx = (e[:, None] - n) + np.arange(n)[None, :]
        L = np.fft.rfft(x[idx, 0] * win, axis=1)
        R = np.fft.rfft(x[idx, 1] * win, axis=1)
        pb = 0.5 * (np.abs(L) ** 2 + np.abs(R) ** 2)
        xb = np.real(L * np.conj(R))
        P[i0:i0 + chunk] = (pb @ W.T).astype(np.float32)
        X[i0:i0 + chunk] = np.clip((xb @ W.T).astype(np.float32), -P[i0:i0 + chunk], P[i0:i0 + chunk])
    return ends, P, X, n, hop

def live(ends, P, X, sr, hop):
    gate = 1e-6
    hs = hop / sr
    r300 = np.exp(-hs / 0.3); r120 = np.exp(-hs / 0.12)
    settle300 = max(1, int(np.ceil(0.3 / hs))); settle120 = max(1, int(np.ceil(0.12 / hs)))
    nF = len(ends)
    out = {k: np.full(nF, np.nan) for k in ('rho_slow', 'rho_fast', 'lowP', 'lowPf', 'tot', 'totf', 'hiM', 'hiS', 'midP')}
    music = np.zeros(nF, bool)
    lowX = lowPw = lowXf = lowPf = hiM = hiS = tot = totf = midP = 0.0
    k = 0
    for i in range(nF):
        p = P[i]; xx = X[i]
        total = p.sum()
        if total < 1e-7:
            continue
        music[i] = True
        g = p >= gate
        lx = xx[0:6][g[0:6]].sum(); lp = p[0:6][g[0:6]].sum()
        hm = (0.5 * (p[17:27] + xx[17:27]))[g[17:27]].sum(); hsd = (0.5 * (p[17:27] - xx[17:27]))[g[17:27]].sum()
        mp = p[6:12].sum()
        k += 1
        rs = min(r300, (k - 1) / k); rf = min(r120, (k - 1) / k)
        lowX = lx + (lowX - lx) * rs; lowPw = lp + (lowPw - lp) * rs
        lowXf = lx + (lowXf - lx) * rf; lowPf = lp + (lowPf - lp) * rf
        hiM = hm + (hiM - hm) * rs; hiS = hsd + (hiS - hsd) * rs
        tot = total + (tot - total) * rs; totf = total + (totf - total) * rf
        midP = mp + (midP - mp) * rs
        out['lowP'][i] = lowPw; out['lowPf'][i] = lowPf; out['tot'][i] = tot; out['totf'][i] = totf
        out['hiM'][i] = hiM; out['hiS'][i] = hiS; out['midP'][i] = midP
        if k >= settle300 and lowPw >= gate:
            out['rho_slow'][i] = np.clip(lowX / lowPw, -1, 1)
        if k >= settle120 and lowPf >= gate:
            out['rho_fast'][i] = np.clip(lowXf / lowPf, -1, 1)
    return out, music

def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    files = sorted(f for f in os.listdir(src) if f.lower().endswith(('.wav', '.flac', '.aif', '.aiff')) and not f.startswith('.'))
    for f in files:
        x, sr = read_audio(os.path.join(src, f))
        x = np.nan_to_num(x)
        ends, P, X, n, hop = frames(x, sr)
        out, music = live(ends, P, X, sr, hop)
        np.savez_compressed(os.path.join(dst, os.path.splitext(f)[0] + '.npz'), t=ends / sr, music=music, sr=sr, hop=hop, P=P[:, :12].astype(np.float32), X=X[:, :12].astype(np.float32), Ptot=P.sum(axis=1), **out)
        print(f, sr, len(ends), file=sys.stderr)

main()
