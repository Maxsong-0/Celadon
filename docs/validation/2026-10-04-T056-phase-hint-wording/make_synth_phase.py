#!/usr/bin/env python3
# T-056 渲染比对用的合成曲：24 s、44.1 kHz、24 bit 立体声。底鼓（每 0.5 s 一下）+ 55 Hz 贝斯 + 左右独立的宽高频噪声；
# 8 ～ 16 s 右声道低频整体反相，让修坯的反相提示亮起（phase_guard_pct > 0），慢路也真看到反相段（slow_inverted_pct > 0）。
# 用法：make_synth_phase.py <输出.wav>（要 numpy、soundfile；Spark 上用 ~/.cache/celadon-spark/venv/bin/python）。随机种子固定，结果可复现。
import sys

import numpy as np
import soundfile as sf

FS = 44100
SECONDS = 24
INVERTED_FROM, INVERTED_TO = 8.0, 16.0


def synth():
    n = SECONDS * FS
    t = np.arange(n) / FS
    rng = np.random.default_rng(56)
    kick = np.zeros(n)
    for beat in np.arange(0, SECONDS, 0.5):
        i = int(beat * FS)
        m = min(n - i, int(0.25 * FS))
        tt = np.arange(m) / FS
        kick[i:i + m] += np.sin(2 * np.pi * (50 + 80 * np.exp(-tt * 30)) * tt) * np.exp(-tt * 8)
    low = 0.5 * kick + 0.4 * np.sin(2 * np.pi * 55 * t)
    sign = np.where((t >= INVERTED_FROM) & (t < INVERTED_TO), -1.0, 1.0)
    left = low + 0.05 * rng.standard_normal(n)
    right = sign * low + 0.05 * rng.standard_normal(n)
    return (np.stack([left, right], 1) * 0.3).astype(np.float32)


if __name__ == '__main__':
    sf.write(sys.argv[1], synth(), FS, subtype='PCM_24')
