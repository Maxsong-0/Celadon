#!/usr/bin/env python3
"""
首页「肆 · 试听」的片段准备（在 107 的 ~/venvs/celadon-site 里跑，由 site/scripts/make-listen.sh 调用）。

输入：原曲（wav / flac / aiff）+ celadon_render 渲染的整首成品（已按整体响度对齐，逐样本对齐原曲，比原曲长 0.35 s）
     + 渲染时乘的对齐增益 align_gain_db（metrics.csv），用来还原成品的真实电平。
做的事：
  1. 两版切同一段：在原曲里找最响的 N 秒（BS.1770 整合响度，1 s 步进滑窗）；
  2. 量这一段两版各自的真实响度（LUFS-I）与真峰值（4× 过采样），写进清单——网页关掉「等响」时用它们还原真实差距；
  3. 两版都对齐到同一个播放电平 L = min(-14, 两版在真峰值 ≤ -1 dBTP 下能到的最高响度)，只乘增益；
  4. 首尾各 20 ms 淡入淡出；AAC 256 kb/s（.m4a，faststart）；
  5. 400 点峰值包络（对齐后，0 ～ 1）。
输出：<out>/original.m4a、<out>/celadon.m4a、<out>/entry.json（与 site/src/content/listen.ts 的 ListenTrack 同形）。
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

import imageio_ffmpeg
import numpy as np
import pyloudnorm as pyln
import soundfile as sf
from scipy.signal import resample_poly

TARGET_LUFS = -14.0
CEILING_DBTP = -1.0
FADE_S = 0.02
PEAK_BINS = 400


def stereo(x: np.ndarray) -> np.ndarray:
    if x.ndim == 1:
        return np.stack([x, x], axis=1)
    if x.shape[1] == 1:
        return np.repeat(x, 2, axis=1)
    return x[:, :2]


def true_peak_db(x: np.ndarray) -> float:
    up = resample_poly(x, 4, 1, axis=0)
    peak = float(np.max(np.abs(up)))
    return 20 * np.log10(max(peak, 1e-12))


def loudest_window(meter: pyln.Meter, x: np.ndarray, sr: int, seconds: float) -> int:
    n = int(seconds * sr)
    if len(x) <= n:
        return 0
    best, best_start = -np.inf, 0
    for start in range(0, len(x) - n, sr):
        lufs = meter.integrated_loudness(x[start : start + n])
        if lufs > best:
            best, best_start = lufs, start
    return best_start


def encode(x: np.ndarray, sr: int, dest: Path) -> None:
    wav = dest.with_suffix('.wav')
    sf.write(wav, x.astype(np.float32), sr, subtype='FLOAT')
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-i', str(wav), '-c:a', 'aac', '-b:a', '256k', '-movflags', '+faststart', str(dest)], check=True)
    wav.unlink()


def peaks(x: np.ndarray) -> list:
    mono = np.max(np.abs(x), axis=1)
    bins = np.array_split(mono, PEAK_BINS)
    return [round(float(min(1.0, b.max())), 3) for b in bins]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--original', required=True)
    ap.add_argument('--rendered', required=True)
    ap.add_argument('--align-gain-db', type=float, required=True)
    ap.add_argument('--id', required=True)
    ap.add_argument('--title', required=True)
    ap.add_argument('--note', default='')
    ap.add_argument('--settings', required=True)
    ap.add_argument('--seconds', type=float, default=20.0)
    ap.add_argument('--start', type=float, default=None, help='手动指定片段起点（秒）；不给就找最响的一段')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    orig, sr = sf.read(a.original, always_2d=True, dtype='float64')
    rend, sr2 = sf.read(a.rendered, always_2d=True, dtype='float64')
    if sr != sr2:
        print(f'采样率不一致：原曲 {sr}，成品 {sr2}', file=sys.stderr)
        return 1
    orig = stereo(orig)
    rend = stereo(rend)[: len(orig)] * 10 ** (-a.align_gain_db / 20)  # 还原成品对齐前的真实电平

    meter = pyln.Meter(sr)
    start = int(a.start * sr) if a.start is not None else loudest_window(meter, orig, sr, a.seconds)
    n = min(int(a.seconds * sr), len(orig) - start)
    seg = {'original': orig[start : start + n], 'celadon': rend[start : start + n]}

    native = {k: (meter.integrated_loudness(v), true_peak_db(v)) for k, v in seg.items()}
    # 播放电平：-14 LUFS，但任一版对齐后真峰值不许超过 -1 dBTP
    level = min([TARGET_LUFS] + [CEILING_DBTP - (tp - lufs) for lufs, tp in native.values()])

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    fade = int(FADE_S * sr)
    ramp = np.linspace(0.0, 1.0, fade)[:, None]
    entry = {
        'id': a.id,
        'title': a.title,
        'note': a.note,
        'from': round(start / sr, 1),
        'seconds': round(n / sr, 2),
        'settings': a.settings,
        'matchLufs': round(level, 1),
    }
    for k, x in seg.items():
        lufs, tp = native[k]
        y = x * 10 ** ((level - lufs) / 20)
        y[:fade] *= ramp
        y[-fade:] *= ramp[::-1]
        encode(y, sr, out / f'{k}.m4a')
        entry[k] = {'src': f'audio/{a.id}/{k}.m4a', 'lufs': round(lufs, 1), 'truePeak': round(tp, 1), 'peaks': peaks(y)}
        print(f'{k}: 片段真实响度 {lufs:.1f} LUFS、真峰值 {tp:.1f} dBTP → 播放电平 {level:.1f} LUFS', file=sys.stderr)

    (out / 'entry.json').write_text(json.dumps(entry, ensure_ascii=False, indent=1), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
