#!/usr/bin/env python3
# T-038 第 5 步标定（设计规格-曲风与交付第 3.2 节、设计规格-曲风-House学习方式第 6 节）。参考曲是 Jamendo 45 首
# （docs/validation/2026-10-01-T038-reference/，Max 2026-10-01 定：标定不用他自己的曲子）。依赖 numpy / scipy / soundfile / matplotlib。
#
#   prep    <mp3 根目录> <工作目录>                mp3 → wav；估速度与拍相位，起点裁到估出的第一个小节线；写 manifest.tsv
#   render  <工作目录> <celadon_render> [-j 12]    逐首跑 House 档（各自的子类与速度），出画像谱斜率、门控结果与逐小节统计
#   label   <工作目录> <图目录>                     用与门控无关的三个读数（底鼓起伏、低音底、中高频密度）给每小节标段落，出图供人工核对
#   analyze <工作目录> [--labels 改标.tsv]          Tone 参考斜率中位数；ΔG × 低频门系数扫描；64 / 8 小节与锁定后重开核对
#
# 段落标签：intro（第一段 drop 之前）、drop（满编：底鼓、低音、密度都在）、breakdown（两段 drop 之间）、outro（最后一段 drop 之后）；
# 段落交界前后各 1 小节标 transition，不进统计（估出的小节线可能差几拍）。自动标签按 8 小节一组取多数，再由人看图改（--labels）。
# 门控口径同 SegmentLedger：已听（有效音频 ≥ 半小节）的小节里，段响度的第 95 百分位（按有效时长加权）= S_ref；过响度门的小节里
# 低频占比的第 95 百分位 = 低频参考；满编 = 段响度 ≥ S_ref − ΔG 且低频占比 ≥ 低频参考 × 系数。

import argparse
import concurrent.futures
import csv
import math
import os
import statistics
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from house_gate import fmt, num, weighted_percentile  # noqa: E402

CLASSES = ('deep', 'tech', 'melodic')
# GlazeConstants.cpp genre 表的出厂 Tone 参考斜率（2026-10-01 标定后三类都 −2.25；标定时比的是起点值 −3.5 / −2.5 / −3.0，见报告）
FACTORY_SLOPE = {'deep': -2.25, 'tech': -2.25, 'melodic': -2.25}
FACTORY_WIDTH_LU, FACTORY_LOW_FACTOR = 3.0, 0.6
LOCK_SEGMENTS, REQUIRED_FULL = 64, 8
REOPEN_ABOVE_LU, REOPEN_SEGMENTS = 3.0, 4
N_FFT, HOP = 2048, 512


# ---------------------------------------------------------------------------------------------------------------------
# 频带能量（prep 与 label 共用）

def band_edges(low=30.0, high=16000.0, count=24):
    return np.geomspace(low, high, count + 1)


def band_power(x, sr, edges):
    """单声道 x → (帧数, 频带数) 的功率，帧移 HOP。分块做，内存只占一块。"""
    window = np.hanning(N_FFT).astype(np.float32)
    freqs = np.fft.rfftfreq(N_FFT, 1.0 / sr)
    index = np.digitize(freqs, edges) - 1
    valid = (index >= 0) & (index < len(edges) - 1)
    frames = max(0, 1 + (len(x) - N_FFT) // HOP)
    out = np.zeros((frames, len(edges) - 1), dtype=np.float64)
    view = np.lib.stride_tricks.sliding_window_view(x, N_FFT)[::HOP]
    for start in range(0, frames, 4096):
        chunk = view[start:start + 4096] * window
        power = np.abs(np.fft.rfft(chunk, axis=1)) ** 2
        for b in range(len(edges) - 1):
            out[start:start + len(chunk), b] = power[:, valid & (index == b)].sum(axis=1)
    return out


def onset_envelope(power, edges):
    level = 10.0 * np.log10(power + 1e-10)
    flux = np.maximum(0.0, np.diff(level, axis=0, prepend=level[:1]))
    low = edges[1:] <= 160.0
    env = flux.sum(axis=1) / max(1, flux.shape[1]) + flux[:, low].sum(axis=1) / max(1, low.sum())
    kernel = np.ones(43) / 43.0                                        # 约 0.5 s 的局部均值，减掉后半波整流
    return np.maximum(0.0, env - np.convolve(env, kernel, mode='same'))


def fold_score(env, frame_rate, bpm, bins=48):
    period = 60.0 * frame_rate / bpm
    phase = (np.arange(len(env)) % period) / period
    hist = np.bincount((phase * bins).astype(int) % bins, weights=env, minlength=bins)
    best = int(hist.argmax())
    return hist[best] / (hist.mean() + 1e-12), (best + 0.5) / bins * period


def estimate_tempo(env, frame_rate, low=100.0, high=140.0):
    coarse = np.arange(low, high, 0.05)
    scores = [fold_score(env, frame_rate, b)[0] for b in coarse]
    centre = coarse[int(np.argmax(scores))]
    fine = np.arange(centre - 0.06, centre + 0.06, 0.002)
    scored = [(fold_score(env, frame_rate, b), b) for b in fine]
    (score, phase_frames), bpm = max(scored, key=lambda item: item[0][0])
    return float(bpm), float(phase_frames / frame_rate), float(score)


def choose_downbeat(power, edges, frame_rate, bpm, phase_s):
    """四个候选小节起点里，选让相邻小节能量跳变最大的那个（段落在小节线上换，错位的网格会把跳变摊到两小节）。"""
    beat_frames = 60.0 * frame_rate / bpm
    low = edges[1:] <= 160.0
    level = power.sum(axis=1)
    low_level = power[:, low].sum(axis=1)
    best, best_offset = -1.0, 0
    for offset in range(4):
        start = phase_s * frame_rate + offset * beat_frames
        bars = []
        while start + 4 * beat_frames <= len(level):
            a, b = int(start), int(start + 4 * beat_frames)
            bars.append((math.log10(level[a:b].mean() + 1e-10), math.log10(low_level[a:b].mean() + 1e-10)))
            start += 4 * beat_frames
        if len(bars) < 4:
            continue
        arr = np.array(bars)
        score = float((np.diff(arr, axis=0) ** 2).sum())
        if score > best:
            best, best_offset = score, offset
    return phase_s + best_offset * 60.0 / bpm


def cmd_prep(args):
    import soundfile as sf
    rows = []
    for cls in CLASSES:
        source = os.path.join(args.mp3_root, cls)
        for name in sorted(f for f in os.listdir(source) if f.endswith('.mp3')):
            track = name[:-4]
            audio, sr = sf.read(os.path.join(source, name), dtype='float32', always_2d=True)
            mono = audio.mean(axis=1)
            edges = band_edges()
            power = band_power(mono, sr, edges)
            frame_rate = sr / HOP
            env = onset_envelope(power, edges)
            bpm, phase_s, score = estimate_tempo(env, frame_rate)
            start_s = choose_downbeat(power, edges, frame_rate, bpm, phase_s)
            start = int(round(start_s * sr))
            target = os.path.join(args.work, cls, track)
            os.makedirs(target, exist_ok=True)
            sf.write(os.path.join(target, track + '.wav'), audio[start:], sr, subtype='PCM_24')
            rows.append([cls, track, f'{bpm:.3f}', f'{start_s:.4f}', f'{score:.2f}', str(sr), f'{(len(audio) - start) / sr:.2f}'])
            print(cls, track, f'{bpm:.3f} BPM', f'起点 {start_s:.3f} s', f'折叠峰 {score:.2f}', flush=True)
    with open(os.path.join(args.work, 'manifest.tsv'), 'w', encoding='utf-8') as f:
        f.write('class\ttrack\tbpm\ttrim_s\tfold_peak\tsample_rate\tduration_s\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')


def load_manifest(work):
    with open(os.path.join(work, 'manifest.tsv'), encoding='utf-8') as f:
        return list(csv.DictReader(f, delimiter='\t'))


# ---------------------------------------------------------------------------------------------------------------------
# render

def cmd_render(args):
    rows = load_manifest(args.work)

    def one(r):
        folder = os.path.join(args.work, r['class'], r['track'])
        out = os.path.join(folder, 'out')
        command = [args.render, '--in', folder, '--out', out, '--glaze', '100', '--style', 'auto', '--clay', 'auto',
                   '--genre', r['class'], '--delivery', 'standard', '--bpm', r['bpm'], '--segments-csv', '--metrics-only',
                   '--no-replay']
        with open(os.path.join(folder, 'render.log'), 'w', encoding='utf-8') as log:
            code = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT)
        return r['class'], r['track'], code

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for cls, track, code in pool.map(one, rows):
            print(cls, track, '完成' if code == 0 else f'失败（退出码 {code}）', flush=True)


def load_render(work, r):
    out = os.path.join(work, r['class'], r['track'], 'out')
    with open(os.path.join(out, 'metrics.csv'), encoding='utf-8') as f:
        metrics = list(csv.DictReader(f))
    render = next(m for m in metrics if m['kind'] == 'render')
    path = os.path.join(out, 'segments', r['track'] + '__input.csv')
    with open(path, encoding='utf-8') as f:
        segments = [s for s in csv.DictReader(f)]
    return render, segments


# ---------------------------------------------------------------------------------------------------------------------
# label：与门控无关的三个读数

def bar_features(path, bpm):
    import soundfile as sf
    audio, sr = sf.read(path, dtype='float32', always_2d=True)
    mono = audio.mean(axis=1)
    edges = band_edges()
    power = band_power(mono, sr, edges)
    frame_rate = sr / HOP
    centres = np.sqrt(edges[:-1] * edges[1:])
    low = (centres >= 40.0) & (centres <= 130.0)
    mid = (centres >= 300.0) & (centres <= 8000.0)
    low_env = 10.0 * np.log10(power[:, low].sum(axis=1) + 1e-10)
    mid_env = 10.0 * np.log10(power[:, mid].sum(axis=1) + 1e-10)
    band_db = 10.0 * np.log10(power + 1e-10)
    beat = 60.0 * frame_rate / bpm
    bars = int(len(low_env) // (4 * beat))
    kick, floor, density, spectrum = [], [], [], []
    for k in range(bars):
        peaks, troughs = [], []
        for j in range(4):
            centre = (4 * k + j) * beat
            a, b = int(centre - 0.06 * frame_rate), int(centre + 0.06 * frame_rate) + 1
            c, d = int(centre + 0.35 * beat), int(centre + 0.65 * beat) + 1
            if a < 0 or d > len(low_env):
                continue
            peaks.append(low_env[a:b].max())
            troughs.append(low_env[c:d].mean())
        a, b = int(4 * k * beat), int(4 * (k + 1) * beat)
        kick.append(float(np.median(np.array(peaks) - np.array(troughs))) if peaks else math.nan)
        floor.append(float(np.median(troughs)) if troughs else math.nan)
        density.append(float(mid_env[a:b].mean()))
        spectrum.append(band_db[a:b].mean(axis=0))
    occupied = power.max(axis=0) > 0.0                                      # 低端几段窄于 FFT 分辨率、没有一根谱线：去掉
    return np.array(kick), np.array(floor), np.array(density), np.array(spectrum)[:, occupied], centres[occupied]


def section_boundaries(spectrum, min_length=8, window=4):
    """逐小节 24 段频谱（dB）→ 段落边界（小节下标，含 0 与末尾）。新颖度 = 前后各 window 小节均值谱的距离；
    取高于「中位数 + 1.5 × MAD」的局部峰，相邻边界至少隔 min_length 小节（House 的段落按 8 小节的倍数换）。"""
    bars = len(spectrum)
    v = spectrum - np.nanmean(spectrum, axis=0)
    novelty = np.zeros(bars)
    for k in range(window, bars - window + 1):
        novelty[k] = float(np.linalg.norm(v[k:k + window].mean(axis=0) - v[k - window:k].mean(axis=0)))
    median = float(np.median(novelty[window:bars - window + 1])) if bars > 2 * window else 0.0
    mad = float(np.median(np.abs(novelty[window:bars - window + 1] - median))) if bars > 2 * window else 0.0
    threshold = median + 1.5 * mad
    peaks = [k for k in range(window, bars - window + 1)
             if novelty[k] > threshold and novelty[k] >= novelty[max(0, k - 2):k + 3].max()]
    chosen = []
    for k in sorted(peaks, key=lambda k: -novelty[k]):
        if all(abs(k - c) >= min_length for c in chosen) and k >= min_length // 2 and bars - k >= min_length // 2:
            chosen.append(k)
    return [0] + sorted(chosen) + [bars]


def auto_labels(kick, floor, density, spectrum):
    """按段落分类：段内中位数——以低音底（拍间 40～130 Hz）最强、有底鼓的段为参照，低音底在参照 6 dB 以内、中高频密度在参照 4 dB 以内、底鼓起伏 ≥ 4 dB = drop
    （满编）；第一段 drop 之前 = intro，最后一段之后 = outro，其间 = breakdown；交界前后各 1 小节 = transition。不用段响度与低频占比
    （那是门控自己的两个读数，用了就成了自己验自己）。"""
    edges = section_boundaries(spectrum)
    sections = []
    for a, b in zip(edges[:-1], edges[1:]):
        sections.append((a, b, float(np.nanmedian(floor[a:b])), float(np.nanmedian(density[a:b])), float(np.nanmedian(kick[a:b]))))
    # 低音底最强、有底鼓、至少 16 小节的段 = 参照（满编的样子；主段落很少短于 16 小节，曲尾 8 小节的重低音尾巴不能当参照）
    long_enough = ([x for x in sections if x[1] - x[0] >= 16 and x[4] >= 4.0]
                   or [x for x in sections if x[1] - x[0] >= 8 and x[4] >= 4.0] or sections)
    reference = max(long_enough, key=lambda x: x[2])
    # 密度比参照段而不比「密度最高的段」：breakdown 常有更亮的铺底与上行音效，密度反而最高（deep 0155923 第 121～150 小节）
    full = [x[2] >= reference[2] - 6.0 and x[3] >= reference[3] - 4.0 and x[4] >= 4.0 for x in sections]
    drop_index = [i for i, f in enumerate(full) if f]
    labels = []
    for i, (a, b, *_rest) in enumerate(sections):
        if full[i]:
            kind = 'drop'
        elif not drop_index or i < drop_index[0]:
            kind = 'intro'
        elif i > drop_index[-1]:
            kind = 'outro'
        else:
            kind = 'breakdown'
        labels.extend([kind] * (b - a))
    final = list(labels)
    for k in range(1, len(labels)):
        if labels[k] != labels[k - 1]:
            final[k - 1] = final[k] = 'transition'
    return final, edges


def cmd_label(args):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams['font.sans-serif'] = ['Noto Sans CJK SC', 'Noto Sans CJK JP', 'DejaVu Sans']   # 109 装了 fonts-noto-cjk
    plt.rcParams['axes.unicode_minus'] = False
    rows = load_manifest(args.work)
    os.makedirs(args.figures, exist_ok=True)
    table = []
    paths = [(os.path.join(args.work, r['class'], r['track'], r['track'] + '.wav'), float(r['bpm'])) for r in rows]
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:   # 频谱最费时：逐首并行算好再画
        features = dict(zip((r['track'] for r in rows), pool.map(bar_features, *zip(*paths))))
    pages = [rows[i:i + 3] for i in range(0, len(rows), 3)]
    for page_index, page in enumerate(pages):
        fig, axes = plt.subplots(len(page) * 2, 1, figsize=(16, 4.2 * len(page)), gridspec_kw={'height_ratios': [1.3, 1] * len(page)})
        for i, r in enumerate(page):
            bpm = float(r['bpm'])
            kick, floor, density, spectrum, centres = features[r['track']]
            labels, edges = auto_labels(kick, floor, density, spectrum)
            render, segments = load_render(args.work, r)
            lufs = np.array([num(s['lufs']) for s in segments])
            share = np.array([num(s['low_share']) for s in segments])
            for k, label in enumerate(labels):
                table.append([r['class'], r['track'], str(k + 1), label])
            ax = axes[2 * i]
            ax.imshow(spectrum.T, aspect='auto', origin='lower', cmap='magma',
                      extent=(0.5, len(spectrum) + 0.5, 0, len(centres)), vmin=np.nanpercentile(spectrum, 5), vmax=np.nanpercentile(spectrum, 99.5))
            ticks = [int(np.argmin(np.abs(np.log(centres / f)))) for f in (50.0, 100.0, 330.0, 1100.0, 5000.0)]
            ax.set_yticks([t + 0.5 for t in ticks])
            ax.set_yticklabels(['50', '100', '330', '1.1k', '5k'])
            colours = {'intro': '#4e79a7', 'drop': '#e15759', 'breakdown': '#59a14f', 'outro': '#9c755f', 'transition': '#bab0ac'}
            for k, label in enumerate(labels):
                ax.axvspan(k + 0.5, k + 1.5, ymin=0.93, ymax=1.0, color=colours[label])
            for e in edges[1:-1]:
                ax.axvline(e + 0.5, color='white', lw=0.8, alpha=0.8)
            confidence = '' if reliable_tempo(r) else '  【速度低置信】'
            ax.set_title(f"{r['class']} {r['track']}  {bpm:.2f} BPM{confidence}  画像谱斜率 {render.get('profile_slope_db_per_oct') or '—'}  "
                         f"已听 {render.get('house_heard_segments') or '—'} 满编 {render.get('house_full_segments') or '—'}  "
                         f"锁定待定 {render.get('house_lock_pending')}  重开 {render.get('house_reopens')}", fontsize=9, loc='left')
            ax.set_xlim(0.5, max(len(spectrum), len(lufs)) + 0.5)
            ax2 = axes[2 * i + 1]
            bars = np.arange(1, len(lufs) + 1)
            ax2.plot(bars, lufs - np.nanmax(lufs), color='#e15759', lw=1, label='段响度 − 最大')
            ax2.plot(np.arange(1, len(kick) + 1), (kick - 12.0) / 2.0, color='#4e79a7', lw=0.8, label='底鼓起伏')
            ax2.plot(np.arange(1, len(floor) + 1), floor - np.nanpercentile(floor, 90), color='#59a14f', lw=0.8, label='低音底')
            ax2.plot(np.arange(1, len(density) + 1), density - np.nanpercentile(density, 90), color='#f28e2b', lw=0.8, label='中高频密度')
            ax2.plot(bars, (share - 1.0) * 20.0, color='#76b7b2', lw=0.8, label='低频占比（×20 − 20）')
            ax2.set_ylim(-25, 4)
            ax2.set_xlim(0.5, max(len(spectrum), len(lufs)) + 0.5)
            ax2.set_xticks(range(1, len(lufs) + 1, 8))
            ax2.grid(axis='x', alpha=0.3)
            if i == 0:
                ax2.legend(fontsize=7, ncol=5, loc='lower left')
        fig.tight_layout()
        fig.savefig(os.path.join(args.figures, f'page-{page_index + 1:02d}.png'), dpi=60)
        plt.close(fig)
        print('图', page_index + 1, '/', len(pages), flush=True)
    with open(os.path.join(args.work, 'labels-auto.tsv'), 'w', encoding='utf-8') as f:
        f.write('class\ttrack\tbar\tlabel\n')
        for t in table:
            f.write('\t'.join(t) + '\n')


# ---------------------------------------------------------------------------------------------------------------------
# analyze

def load_labels(work, overrides):
    labels = {}
    with open(os.path.join(work, 'labels-auto.tsv'), encoding='utf-8') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            labels[(r['track'], int(r['bar']))] = r['label']
    if overrides:
        # 改标：track<TAB>first_bar<TAB>last_bar<TAB>label（闭区间；label 可写 exclude）。只改已有的小节；改完重标交界：
        # 相邻两小节分属四类里不同的两类，两小节都改 transition（与自动标签同一规则；transition / exclude 不参与比较，不会连锁扩开）。
        changed = set()
        with open(overrides, encoding='utf-8') as f:
            for r in csv.DictReader((line for line in f if not line.startswith('#')), delimiter='\t'):
                for bar in range(int(r['first_bar']), int(r['last_bar']) + 1):
                    if (r['track'], bar) in labels:
                        labels[(r['track'], bar)] = r['label']
                        changed.add(r['track'])
        kinds = ('intro', 'drop', 'breakdown', 'outro')
        for track in changed:
            bars = sorted(b for t, b in labels if t == track)
            original = {b: labels[(track, b)] for b in bars}
            for prev, cur in zip(bars, bars[1:]):
                if original[prev] in kinds and original[cur] in kinds and original[prev] != original[cur]:
                    labels[(track, prev)] = labels[(track, cur)] = 'transition'
    return labels


def reliable_tempo(r):
    """速度估计可信：折叠峰 ≥ 5 且不贴 100 / 140 BPM 的搜索边界。"""
    return num(r['fold_peak']) >= 5.0 and 100.5 <= float(r['bpm']) <= 139.5


def gate(bars, width, factor):
    """bars = [(小节, 段响度, 有效秒, 低频占比, 已听)] → 满编小节集合（口径见文件头）。"""
    heard = [b for b in bars if b[4] and math.isfinite(b[1])]
    reference = weighted_percentile([(b[1], b[2]) for b in heard], 0.95)
    passed = [b for b in heard if b[1] >= reference - width]
    low_reference = weighted_percentile([(b[3], b[2]) for b in passed if math.isfinite(b[3])], 0.95)
    return {b[0] for b in passed if math.isfinite(b[3]) and b[3] >= low_reference * factor}, reference, low_reference


def track_bars(segments, bpm):
    bar_seconds = 4.0 * 60.0 / bpm
    return [(int(s['segment']), num(s['lufs']), num(s['valid_s'], 0.0), num(s['low_share']),
             num(s['valid_s'], 0.0) >= 0.5 * bar_seconds - 1e-6) for s in segments]


def energy_mean_lufs(values):
    values = [v for v in values if math.isfinite(v)]
    return -0.691 + 10.0 * math.log10(sum(10.0 ** ((v + 0.691) / 10.0) for v in values) / len(values)) if values else math.nan


def progressive_lock(bars, width, factor):
    """按播放顺序逐小节并入、每并入一小节重算门控（同段落账）：第一次「已听 ≥ 64 且满编 ≥ 8」的小节号与当时满编段的响度。"""
    seen = []
    for b in bars:
        seen.append(b)
        heard = sum(1 for x in seen if x[4])
        if heard < LOCK_SEGMENTS:
            continue
        full, _, _ = gate(seen, width, factor)
        if len(full) >= REQUIRED_FULL:
            return b[0], energy_mean_lufs([x[1] for x in seen if x[0] in full])
    return None, math.nan


def reopen_events(bars, lock_bar, profile_lufs, width, factor):
    """锁定之后：连续 4 个已听小节都比画像响 3 LU 以上且过低频门 = 一次重开的条件（低频门按到这一小节为止的门控）。"""
    events, run = 0, 0
    for i, b in enumerate(bars):
        if lock_bar is None or b[0] <= lock_bar or not b[4]:
            continue
        _, _, low_reference = gate(bars[:i + 1], width, factor)
        if math.isfinite(b[1]) and b[1] >= profile_lufs + REOPEN_ABOVE_LU and math.isfinite(b[3]) and b[3] >= low_reference * factor:
            run += 1
            if run == REOPEN_SEGMENTS:
                events += 1
                run = 0
        else:
            run = 0
    return events


def cmd_analyze(args):
    rows = load_manifest(args.work)
    labels = load_labels(args.work, args.labels)
    out = []
    per_track = []
    for r in rows:
        render, segments = load_render(args.work, r)
        bars = track_bars(segments, float(r['bpm']))
        kinds = {b[0]: labels.get((r['track'], b[0]), 'exclude') for b in bars}
        per_track.append((r, render, bars, kinds))

    # 1 Tone 参考斜率
    out.append('## 1 Tone 参考斜率（House 门控画像的 spectralSlopeDbPerOct，dB / 倍频程）\n')
    out.append('| 子类 | 首数 | 中位数 | 四分位 | 最小～最大 | 只算速度可信的（首数：中位数） | 出厂 | 建议（中位数取到 0.25） |')
    out.append('|---|---|---|---|---|---|---|---|')
    for cls in CLASSES:
        slopes = sorted(num(t[1].get('profile_slope_db_per_oct')) for t in per_track if t[0]['class'] == cls)
        slopes = [s for s in slopes if math.isfinite(s)]
        sure = [num(t[1].get('profile_slope_db_per_oct')) for t in per_track if t[0]['class'] == cls and reliable_tempo(t[0])]
        sure = [s for s in sure if math.isfinite(s)]
        if not slopes:
            continue
        q = np.percentile(slopes, [25, 75])
        median = statistics.median(slopes)
        out.append(f'| {cls} | {len(slopes)} | {fmt(median)} | {fmt(q[0])} ～ {fmt(q[1])} | {fmt(slopes[0])} ～ {fmt(slopes[-1])} | '
                   f'{len(sure)}：{fmt(statistics.median(sure)) if sure else "—"} | {fmt(FACTORY_SLOPE[cls])} | {fmt(round(median * 4) / 4)} |')

    # 1b Tone 的实际动作：GlazePlanner 的式子——差 = 参考 − 曲子斜率，超出死区 0.35 的部分 × 0.5 dB，钳在 −0.75（往暗）～ +0.4（House 往亮）；
    # 按 GLAZE 100、斜率读数全可信算满量（实际再 × GLAZE 与可信度）
    def tone_db(reference, slope):
        difference = reference - slope
        outside = math.copysign(max(0.0, abs(difference) - 0.35), difference)
        return min(0.4, max(-0.75, outside * 0.5))

    out.append('\n## 1b Tone 的实际动作（GLAZE 100 满量，dB；负 = 往暗）\n')
    out.append('| 子类 | 出厂参考：中位数 / 最暗～最亮 / 往暗的首数 | 改成本类中位数后：中位数 / 最暗～最亮 / 往暗的首数 |')
    out.append('|---|---|---|')
    for cls in CLASSES:
        slopes = [num(t[1].get('profile_slope_db_per_oct')) for t in per_track if t[0]['class'] == cls]
        slopes = [x for x in slopes if math.isfinite(x)]
        if not slopes:
            continue
        suggested = round(statistics.median(slopes) * 4) / 4
        cells = []
        for reference in (FACTORY_SLOPE[cls], suggested):
            tones = [tone_db(reference, x) for x in slopes]
            cells.append(f'{fmt(statistics.median(tones), 2, True)} / {fmt(min(tones), 2, True)} ～ {fmt(max(tones), 2, True)} / '
                         f'{sum(1 for x in tones if x < 0)} 首')
        out.append(f'| {cls} | {cells[0]} | {cells[1]}（参考 {fmt(suggested)}） |')

    # 2 门控扫描
    widths = [1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0]
    factors = [0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]
    out.append('\n## 2 门控扫描（整首离线一遍；前奏 / breakdown / 尾奏挡住率，drop 通过率；已听小节，交界 ±1 小节不计）\n')
    out.append('格子：前奏挡住 % / drop 通过 %（breakdown 挡住 %）。目标：前奏 ≥ 95 %、drop ≥ 95 %。\n')
    header = '| ΔG \\ 系数 | ' + ' | '.join(f'{f:.1f}' for f in factors) + ' |'
    for scope in ('全部', '只算速度可信的') + CLASSES:
        out.append(f'\n### {scope}\n')
        out.append(header)
        out.append('|---|' + '---|' * len(factors))
        for w in widths:
            cells = []
            for fac in factors:
                counts = {k: [0, 0] for k in ('intro', 'drop', 'breakdown', 'outro')}
                for r, render, bars, kinds in per_track:
                    if scope in CLASSES and r['class'] != scope:
                        continue
                    if scope == '只算速度可信的' and not reliable_tempo(r):
                        continue
                    full, _, _ = gate(bars, w, fac)
                    for b in bars:
                        kind = kinds[b[0]]
                        if kind in counts and b[4]:
                            counts[kind][1] += 1
                            counts[kind][0] += (b[0] in full) if kind == 'drop' else (b[0] not in full)
                pct = {k: 100.0 * v[0] / v[1] if v[1] else math.nan for k, v in counts.items()}
                mark = ' ✓' if pct['intro'] >= 95.0 and pct['drop'] >= 95.0 else ''
                cells.append(f"{fmt(pct['intro'], 0)} / {fmt(pct['drop'], 0)}（{fmt(pct['breakdown'], 0)}）{mark}")
            out.append(f'| {w:.1f} | ' + ' | '.join(cells) + ' |')

    # 2b 画像代表性：门控画像的响度（满编小节的能量平均）与人工标的 drop 响度之差——门控真正要保的是这个，逐小节挡住率只是手段
    out.append('\n## 2b 画像代表性（整首离线门控的满编小节能量平均 − 标为 drop 的小节能量平均，LU；有 drop 标签的曲目）\n')
    out.append('格子：|差| 的中位数 / 第 90 百分位 / 最大（满编小节数中位数）。参照：不门控（整首已听小节）。\n')
    for scope in ('全部', '只算速度可信的'):
        out.append(f'\n### {scope}\n')
        out.append(header)
        out.append('|---|' + '---|' * len(factors))
        candidates = [(r, bars, kinds) for r, render, bars, kinds in per_track
                      if any(kinds[b[0]] == 'drop' for b in bars) and (scope == '全部' or reliable_tempo(r))]
        plain = []
        for r, bars, kinds in candidates:
            drop_lufs = energy_mean_lufs([b[1] for b in bars if kinds[b[0]] == 'drop' and b[4]])
            plain.append(abs(energy_mean_lufs([b[1] for b in bars if b[4]]) - drop_lufs))
        for w in widths:
            cells = []
            for fac in factors:
                errors, counts = [], []
                for r, bars, kinds in candidates:
                    full, _, _ = gate(bars, w, fac)
                    drop_lufs = energy_mean_lufs([b[1] for b in bars if kinds[b[0]] == 'drop' and b[4]])
                    errors.append(abs(energy_mean_lufs([b[1] for b in bars if b[0] in full]) - drop_lufs))
                    counts.append(len(full))
                errors = [e for e in errors if math.isfinite(e)]
                cells.append(f'{fmt(statistics.median(errors))} / {fmt(float(np.percentile(errors, 90)))} / {fmt(max(errors))}'
                             f'（{int(statistics.median(counts))}）')
            out.append(f'| {w:.1f} | ' + ' | '.join(cells) + ' |')
        plain = [e for e in plain if math.isfinite(e)]
        out.append(f'\n不门控：{fmt(statistics.median(plain))} / {fmt(float(np.percentile(plain, 90)))} / {fmt(max(plain))}（{len(plain)} 首）')

    # 3 逐首：分不开的、锁定与重开
    out.append(f'\n## 3 逐首（出厂 ΔG {FACTORY_WIDTH_LU} LU、系数 {FACTORY_LOW_FACTOR}）\n')
    out.append('| 子类 | 曲目 | BPM | 小节 | 首段 drop | 前奏挡住 | drop 通过 | 渲染：锁定待定 / 已听 / 满编 / 重开 | 画像 I − drop I | 离线锁定小节 | 锁定时 − 整首 drop | 锁定后重开条件 |')
    out.append('|---|---|---|---|---|---|---|---|---|---|---|---|')
    inseparable = []
    for r, render, bars, kinds in per_track:
        full, reference, low_reference = gate(bars, FACTORY_WIDTH_LU, FACTORY_LOW_FACTOR)
        intro = [b for b in bars if kinds[b[0]] == 'intro' and b[4]]
        drop = [b for b in bars if kinds[b[0]] == 'drop' and b[4]]
        intro_blocked = sum(1 for b in intro if b[0] not in full)
        drop_passed = sum(1 for b in drop if b[0] in full)
        first_drop = min((b[0] for b in bars if kinds[b[0]] == 'drop'), default=None)
        drop_lufs = energy_mean_lufs([b[1] for b in drop])
        profile_delta = num(render.get('profile_lufs')) - drop_lufs
        lock_bar, lock_lufs = progressive_lock(bars, FACTORY_WIDTH_LU, FACTORY_LOW_FACTOR)
        reopen = reopen_events(bars, lock_bar, lock_lufs, FACTORY_WIDTH_LU, FACTORY_LOW_FACTOR)
        if intro and drop:
            intro_max = max(b[1] for b in intro)
            drop_min = min(b[1] for b in drop)
            if intro_max >= drop_min - 0.5:
                inseparable.append(f"{r['class']} {r['track']}：前奏最响小节 {fmt(intro_max)}、drop 最轻小节 {fmt(drop_min)} LUFS")
        out.append(f"| {r['class']} | {r['track']} | {fmt(float(r['bpm']), 2)}{'' if reliable_tempo(r) else '（低置信）'} | {len(bars)} | {first_drop or '—'} | "
                   f"{intro_blocked} / {len(intro)} | {drop_passed} / {len(drop)} | "
                   f"{render.get('house_lock_pending')} / {render.get('house_heard_segments')} / {render.get('house_full_segments')} / {render.get('house_reopens')} | "
                   f"{fmt(profile_delta, 2, True)} | {lock_bar or '—'} | {fmt(lock_lufs - drop_lufs, 2, True)} | {reopen} |")
    out.append('\n## 4 响度上分不开的曲目（前奏最响的小节不比 drop 最轻的小节轻 0.5 LU 以上）\n')
    out.extend(f'- {line}' for line in inseparable or ['（没有）'])
    text = '\n'.join(out) + '\n'
    print(text)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text)


def main(argv):
    parser = argparse.ArgumentParser(description='T-038 第 5 步标定（Jamendo 45 首）')
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prep')
    p.add_argument('mp3_root')
    p.add_argument('work')
    p = sub.add_parser('render')
    p.add_argument('work')
    p.add_argument('render')
    p.add_argument('-j', '--jobs', type=int, default=12)
    p = sub.add_parser('label')
    p.add_argument('work')
    p.add_argument('figures')
    p.add_argument('-j', '--jobs', type=int, default=12)
    p = sub.add_parser('analyze')
    p.add_argument('work')
    p.add_argument('--labels')
    p.add_argument('--out')
    args = parser.parse_args(argv)
    {'prep': cmd_prep, 'render': cmd_render, 'label': cmd_label, 'analyze': cmd_analyze}[args.command](args)


if __name__ == '__main__':
    main(sys.argv[1:])
