#!/usr/bin/env python3
# T-043：两份逐样本对齐的成品（auto / replay-off）对比——撤销响度对齐增益后，按频段比电平、按 400 ms 块比 K 计权响度。
# 用法：diff_ab.py <a.wav> <a 的对齐增益 dB> <b.wav> <b 的对齐增益 dB>
import sys
import numpy as np
import scipy.io.wavfile as wf
from scipy import signal
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from lowcut_loss import k_weight, lufs


def load(path, align_db):
    fs, x = wf.read(path)
    return fs, x.astype(np.float64) / 10 ** (float(align_db) / 20)


def main():
    fs, a = load(sys.argv[1], sys.argv[2])
    _, b = load(sys.argv[3], sys.argv[4])
    print(f"整体响度 a {lufs(a, fs):.3f}  b {lufs(b, fs):.3f}  差 {lufs(a, fs) - lufs(b, fs):+.3f} LU")
    d = a - b
    print(f"差值信号峰值 {20 * np.log10(np.max(np.abs(d)) + 1e-30):.1f} dBFS，RMS {10 * np.log10(np.mean(d ** 2) + 1e-30):.1f} dBFS")

    # 按频段比电平（两声道功率和，Welch 1 Hz 分辨率）
    def psd(x):
        f, p = signal.welch(x, fs, nperseg=fs, noverlap=fs // 2, axis=0)
        return f, p.sum(axis=1)
    f, pa = psd(a); _, pb = psd(b); _, pd = psd(d)
    edges = [0, 16, 25, 40, 60, 80, 120, 250, 500, 1000, 2000, 4000, 8000, 16000, 24000]
    print("频段      a−b 电平差 (dB)   差值信号相对 b (dB)")
    for lo, hi in zip(edges[:-1], edges[1:]):
        s = (f >= lo) & (f < hi)
        print(f"  {lo:>5}–{hi:<5} Hz  {10 * np.log10(pa[s].sum() / pb[s].sum()):+8.3f}   {10 * np.log10(pd[s].sum() / pb[s].sum() + 1e-30):8.1f}")

    # 按 400 ms 块（100 ms 步）比 K 计权均方
    (b1, a1), (b2, a2) = k_weight(fs)
    kw = lambda x: signal.lfilter(b2, a2, signal.lfilter(b1, a1, x, axis=0), axis=0)
    ka, kb = kw(a), kw(b)
    blk, hop = int(0.4 * fs), int(0.1 * fs)
    rows = []
    for i in range(0, len(a) - blk + 1, hop):
        ma = np.sum(np.mean(ka[i:i + blk] ** 2, axis=0)); mb = np.sum(np.mean(kb[i:i + blk] ** 2, axis=0))
        rows.append((i / fs, -0.691 + 10 * np.log10(mb + 1e-30), 10 * np.log10((ma + 1e-30) / (mb + 1e-30))))
    rows = np.array(rows)
    print("400 ms 块：b 的响度 / a−b（每 1 s 取一块）")
    for t, lb, dl in rows[::10]:
        print(f"  {t:6.1f} s  {lb:7.2f} LUFS  {dl:+.3f} dB")


if __name__ == '__main__':
    main()
