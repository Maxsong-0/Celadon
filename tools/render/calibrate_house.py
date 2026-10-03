#!/usr/bin/env python3
# T-038 第 5 步标定（设计规格-曲风与交付第 3.2 节、设计规格-曲风-House学习方式第 6 节）。参考曲是 Jamendo 45 首
# （docs/validation/2026-10-01-T038-reference/，Max 2026-10-01 定：标定不用他自己的曲子）。依赖 numpy / scipy / soundfile / matplotlib。
#
#   prep    <mp3 根目录> <工作目录>                mp3 → wav；估速度与拍相位，起点裁到估出的第一个小节线；写 manifest.tsv
#   render  <工作目录> <celadon_render> [-j 12]    逐首跑 House 档（各自的子类与速度），出画像谱斜率、门控结果与逐小节统计
#   label   <工作目录> <图目录>                     用与门控无关的三个读数（底鼓起伏、低音底、中高频密度）给每小节标段落，出图供人工核对
#   analyze <工作目录> [--labels 改标.tsv]          Tone 参考斜率中位数；ΔG × 低频门系数扫描；64 / 8 小节与锁定后重开核对
#
# T-048 实时画像的因果重放（设计规格-实时GLAZE-主段与增益 第 7 节；渲染工具 celadon_render --live-trace，见 README「T-048 实时画像追踪」）：
#   prep-synthetic <工作目录>                      合成 House 两首（make_synthetic_house.py）摆成与上面同样的布局（<类>/<曲>/<曲>.wav、manifest.tsv），
#                                                 段落边界取自标注 JSON 写成 labels-auto.tsv（边界是精确的：没有交界小节）
#   render-live <工作目录> <celadon_render> [-j 12] [--tag default] [--genre auto|general|deep|tech|melodic] [--live-constants …] [--live-model …]
#                                                 逐首按各自的子类（auto）与速度跑最快的追踪（--live-only、GLAZE 0），输出 <工作目录>/live/<标签>/
#   analyze-live <工作目录> [--labels 改标.tsv] [--auto-labels 自动标签.tsv] [--tags t1,t2] [--out 汇总.md] [--figures 图目录]
#                                                 逐首 + 汇总的因果重放指标（M1 ～ M4 与另给的几项，定义见 live_track_metrics），写 live/<标签>/ 下的
#                                                 analysis.md、tracks.csv、bars.csv、breakdowns.csv
#
# 段落标签：intro（第一段 drop 之前）、drop（满编：底鼓、低音、密度都在）、breakdown（两段 drop 之间）、outro（最后一段 drop 之后）；
# 段落交界前后各 1 小节标 transition，不进统计（估出的小节线可能差几拍）。自动标签按 8 小节一组取多数，再由人看图改（--labels）。
# 门控口径同 SegmentLedger：已听（有效音频 ≥ 半小节）的小节里，段响度的第 95 百分位（按有效时长加权）= S_ref；过响度门的小节里
# 低频占比的第 95 百分位 = 低频参考；满编 = 段响度 ≥ S_ref − ΔG 且低频占比 ≥ 低频参考 × 系数。

import argparse
import concurrent.futures
import csv
import datetime
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from house_gate import fmt, num, weighted_percentile  # noqa: E402

CLASSES = ('deep', 'tech', 'melodic')
SECTION_KINDS = ('intro', 'drop', 'breakdown', 'outro')   # 四类段落；transition（交界 ±1 小节）与 exclude（人工不计）不进任何统计
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


def read_segments(path):
    """celadon_render --segments-csv 的输入端逐段统计（segments/<曲>__input.csv）：每小节一行。"""
    with open(path, encoding='utf-8') as f:
        return [s for s in csv.DictReader(f)]


def load_render(work, r):
    out = os.path.join(work, r['class'], r['track'], 'out')
    with open(os.path.join(out, 'metrics.csv'), encoding='utf-8') as f:
        metrics = list(csv.DictReader(f))
    render = next(m for m in metrics if m['kind'] == 'render')
    segments = read_segments(os.path.join(out, 'segments', r['track'] + '__input.csv'))
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

def load_labels(work, overrides, auto_path=None):
    """(曲, 小节) → 段落标签。自动标签默认读 <工作目录>/labels-auto.tsv（auto_path 可另给，例如仓库里 T-038 那一份），再叠改标。"""
    labels = {}
    with open(auto_path or os.path.join(work, 'labels-auto.tsv'), encoding='utf-8') as f:
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
        for track in changed:
            bars = sorted(b for t, b in labels if t == track)
            original = {b: labels[(track, b)] for b in bars}
            for prev, cur in zip(bars, bars[1:]):
                if original[prev] in SECTION_KINDS and original[cur] in SECTION_KINDS and original[prev] != original[cur]:
                    labels[(track, prev)] = labels[(track, cur)] = 'transition'
    return labels


def bar_kinds(labels, track, bars):
    """每小节的段落类别：标签里没有的小节当 exclude（同 T-038 analyze）。bars 见 track_bars。"""
    return {b[0]: labels.get((track, b[0]), 'exclude') for b in bars}


def drop_loudness(bars, kinds):
    """人工标为 drop、已听的小节的段响度能量平均（LUFS）：T-038 analyze「画像代表性」（2b）与逐首表的 drop 响度口径，
    实时画像的因果重放（analyze-live）按同一口径比。没有这样的小节 → nan。"""
    return energy_mean_lufs([b[1] for b in bars if kinds[b[0]] == 'drop' and b[4]])


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
        kinds = bar_kinds(labels, r['track'], bars)
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
                counts = {k: [0, 0] for k in SECTION_KINDS}
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
            drop_lufs = drop_loudness(bars, kinds)
            plain.append(abs(energy_mean_lufs([b[1] for b in bars if b[4]]) - drop_lufs))
        for w in widths:
            cells = []
            for fac in factors:
                errors, counts = [], []
                for r, bars, kinds in candidates:
                    full, _, _ = gate(bars, w, fac)
                    drop_lufs = drop_loudness(bars, kinds)
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
        drop_lufs = drop_loudness(bars, kinds)
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


# ---------------------------------------------------------------------------------------------------------------------
# T-048 实时画像的因果重放（设计规格-实时GLAZE-主段与增益 第 7 节；追踪的文件格式见 tools/render/README.md「T-048 实时画像追踪」）

LIVE_DIR = 'live'                      # <工作目录>/live/<标签>/：traces/、segments/、logs/、run.tsv 与分析结果
LIVE_START_BAR = 16                    # 规格第 7 节第 1 条：第 16 小节起
LIVE_DROP_OFFSET_BARS = 4              # 第 3 条：第一个 drop 后 4 小节；M2 的另一口径同此
LIVE_M2_LIMITS_LU = (0.3, 1.0)         # 第 1 条：汇总中位 ≤ 0.3、P90 ≤ 1.0 LU
LIVE_DRIFT_LIMIT_LU = 0.5              # 第 2 条：breakdown 期间主段电平漂移 ≤ 0.5 LU
LIVE_SETTLE_LIMIT_LU = 1.0             # 第 3 条：与最终值差 ≤ 1 LU
T038_OFFLINE_LU = (0.15, 0.60, 0.92)   # T-038 离线门控画像代表性 |满编小节能量平均 − drop|：中位 / P90 / 最大（REPORT.md 第 2 节）
T038_GATE_PERCENT = {'intro': ('挡', 60), 'drop': ('过', 85), 'breakdown': ('挡', 80)}   # T-038 出厂门控的逐小节挡住 / 通过率（REPORT.md 结论 2）
MAIN_THRESHOLD = 0.5                   # LiveConstants::mainThreshold 出厂值：m ≥ 0.5 = 主段
CONFIDENCE_FULL = 0.8                  # 决策器 fullConfidence 出厂值：c 到 0.8 时 κ 到 1（REPORT-A 2.6）
SYNTHETIC_CLASS = {'house': 'deep', 'tech': 'tech'}   # make_synthetic_house.py 的两种编排 → 子类（--genre）


def live_dir(work, tag):
    return os.path.join(work, LIVE_DIR, tag)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def cmd_prep_synthetic(args):
    """合成 House 两首 → <工作目录>/<类>/<曲>/<曲>.wav、manifest.tsv（速度精确：fold_peak 记 99）、labels-auto.tsv（标注 JSON 的段落，
    小节从 1 起、两端都含；边界精确，不标交界）。之后 render-live / analyze-live 照 Jamendo 的同一套跑。"""
    import make_synthetic_house as synthetic
    staging = os.path.join(args.work, '.synthetic')
    shutil.rmtree(staging, ignore_errors=True)
    if synthetic.main([staging]) != 0:
        raise SystemExit('prep-synthetic：make_synthetic_house.py 失败')
    manifest, labels = [], []
    for name in sorted(f for f in os.listdir(staging) if f.endswith('.json')):
        with open(os.path.join(staging, name), encoding='utf-8') as f:
            annotation = json.load(f)
        song = annotation['song']
        cls = SYNTHETIC_CLASS['tech' if song.startswith('synthetic-tech') else 'house']
        target = os.path.join(args.work, cls, song)
        os.makedirs(target, exist_ok=True)
        shutil.move(os.path.join(staging, annotation['file']), os.path.join(target, song + '.wav'))
        shutil.copy(os.path.join(staging, name), os.path.join(target, name))
        with wave.open(os.path.join(target, song + '.wav'), 'rb') as w:
            duration = w.getnframes() / w.getframerate()
        manifest.append([cls, song, f"{float(annotation['bpm']):.3f}", '0.0000', '99.00', str(annotation['sample_rate']), f'{duration:.2f}'])
        for s in annotation['sections']:
            labels.extend([cls, song, str(bar), s['kind']] for bar in range(int(s['start_bar']), int(s['end_bar']) + 1))
        print(cls, song, f"{annotation['bpm']} BPM", f'{duration:.2f} s', flush=True)
    shutil.rmtree(staging)
    with open(os.path.join(args.work, 'manifest.tsv'), 'w', encoding='utf-8') as f:
        f.write('class\ttrack\tbpm\ttrim_s\tfold_peak\tsample_rate\tduration_s\n')
        f.writelines('\t'.join(m) + '\n' for m in manifest)
    with open(os.path.join(args.work, 'labels-auto.tsv'), 'w', encoding='utf-8') as f:
        f.write('class\ttrack\tbar\tlabel\n')
        f.writelines('\t'.join(row) + '\n' for row in labels)


def live_genre(r, genre):
    """auto = 各首自己的子类（deep / tech / melodic，= celadon_render --genre 的名字）；其余照给的。"""
    return r['class'] if genre == 'auto' else genre


def render_live_one(args, out, r):
    """一首：celadon_render 最快的追踪跑法（--live-only、GLAZE 0，README「T-048」），另出输入端逐小节统计（--segments-csv，T-038 的 drop 响度
    口径要它）。成功后把追踪、汇总、逐段统计挪进 live/<标签>/ 的 traces/、segments/，删掉这一首的临时输出目录。返回退出码。"""
    genre = live_genre(r, args.genre)
    run = os.path.join(out, '.run', r['track'])
    command = [args.render, '--in', os.path.join(args.work, r['class'], r['track']), '--out', run, '--glaze', '0', '--clay', 'auto',
               '--style', 'auto', '--genre', genre, '--delivery', 'standard', '--bpm', r['bpm'], '--block', str(args.block),
               '--segments-csv', '--metrics-only', '--no-replay', '--live-trace', '--live-only', '--live-tag', args.tag]
    if args.live_constants:
        command += ['--live-constants', args.live_constants]
    if args.live_model:
        command += ['--live-model', args.live_model]
    with open(os.path.join(out, 'logs', r['track'] + '.log'), 'w', encoding='utf-8') as log:
        log.write(' '.join(command) + '\n')
        log.flush()
        code = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT)
    if code != 0:
        return code
    base = f"{r['track']}__{genre}"
    for name in (base + '.csv', base + '__final.csv'):
        shutil.move(os.path.join(run, LIVE_DIR, args.tag, name), os.path.join(out, 'traces', name))
    shutil.move(os.path.join(run, 'segments', r['track'] + '__input.csv'), os.path.join(out, 'segments', r['track'] + '__input.csv'))
    shutil.rmtree(run)
    return 0


def cmd_render_live(args):
    rows = load_manifest(args.work)
    out = live_dir(args.work, args.tag)
    for sub in ('traces', 'segments', 'logs', '.run'):   # 同一标签重跑：先清掉上一次的，免得两组常数的追踪混在一起
        shutil.rmtree(os.path.join(out, sub), ignore_errors=True)
        os.makedirs(os.path.join(out, sub))
    with open(os.path.join(out, 'run.tsv'), 'w', encoding='utf-8') as f:
        for key, value in (('date', datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')),
                           ('render', os.path.abspath(args.render)), ('render_sha256', sha256_of(args.render)),
                           ('genre', args.genre), ('live_constants', args.live_constants or ''), ('live_model', args.live_model or ''),
                           ('block', str(args.block)), ('tracks', str(len(rows)))):
            f.write(f'{key}\t{value}\n')
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [(r, pool.submit(render_live_one, args, out, r)) for r in rows]
        for r, future in futures:
            code = future.result()
            failed += code != 0
            print(r['class'], r['track'], live_genre(r, args.genre), '完成' if code == 0 else
                  f"失败（退出码 {code}，见 {os.path.join(out, 'logs', r['track'] + '.log')}，输出留在 {os.path.join(out, '.run', r['track'])}）",
                  flush=True)
    if not os.listdir(os.path.join(out, '.run')):
        os.rmdir(os.path.join(out, '.run'))
    print(f'render-live：{len(rows) - failed} / {len(rows)} 首完成，追踪在 {out}', flush=True)
    return 1 if failed else 0


def load_live_trace(base, track):
    """live/<标签>/traces/<曲>__<曲风>.csv（追踪，一次提交一行）与 __final.csv（键,值）。找不到 → (None, None)。"""
    folder = os.path.join(base, 'traces')
    names = sorted(n for n in os.listdir(folder) if n.startswith(track + '__') and n.endswith('.csv') and not n.endswith('__final.csv'))
    if not names:
        return None, None
    with open(os.path.join(folder, names[0]), encoding='utf-8') as f:
        trace = list(csv.DictReader(f))
    with open(os.path.join(folder, names[0][:-4] + '__final.csv'), encoding='utf-8') as f:
        final = {r['key']: r['value'] for r in csv.DictReader(f)}
    return trace, final


def live_summary(values):
    """中位 / P90 / 最大 / 个数（同 T-038 analyze：statistics.median、np.percentile 线性插值）。空 → nan。"""
    values = [v for v in values if math.isfinite(v)]
    if not values:
        return math.nan, math.nan, math.nan, 0
    return statistics.median(values), float(np.percentile(values, 90)), max(values), len(values)


def section_runs(kinds, kind):
    """连续标为 kind 的小节段 [[a, …, b], …]（按小节号；中间隔了别的标签就断开）。"""
    runs, run = [], []
    for bar in sorted(kinds):
        if kinds[bar] == kind and run and bar == run[-1] + 1:
            run.append(bar)
            continue
        if run:
            runs.append(run)
            run = []
        if kinds[bar] == kind:
            run = [bar]
    if run:
        runs.append(run)
    return runs


def settle_bars(held, counted, first_drop, reference, last_bar=None):
    """M4 的 n：最小的 n ≥ 1，使计入的小节 j ≥ first_drop + n（截到 last_bar）手里的 I（held(j) = I_{j−1}）都在 reference ± 1 LU 内——
    第一个 drop 的前 n 小节并入之后达到并一直保持。没有计入的小节或 reference 不知道 → nan。"""
    js = [j for j in counted if j > first_drop and (last_bar is None or j <= last_bar) and math.isfinite(held(j))]
    if not js or not math.isfinite(reference):
        return math.nan
    outside = [j for j in js if abs(held(j) - reference) > LIVE_SETTLE_LIMIT_LU]
    return 1 if not outside else outside[-1] - first_drop + 1


def live_track_metrics(r, bars, kinds, trace, final):
    """一首歌的因果重放指标。
    记号：I_k = 第 k 小节那一段并入之后的 I（追踪的 main_level_lufs；同一小节有几段时取最后一段）；第 j 小节播放时决策器手里的 I 记作
    held(j) = I_{j−1}（段要等它播完、先试后用的块交齐才定稿，约晚 0.1 s，所以第 j 小节里用的是上一小节并入之后的 I）；D = 人工标为 drop 的
    已听小节的段响度能量平均（drop_loudness，T-038 analyze 的同一口径；段响度来自同一次运行 --segments-csv 的输入端统计）；I_end = 最后一次
    提交（歌尾不满一小节的那一截）之后的 I；d = 第一个标为 drop 的小节（T-038 的交界小节不算，所以真正的第一个 drop 小节多半是 d − 1）；
    已听 = 有效音频 ≥ 半小节（T-038 口径）。计入的小节 = 已听、且标为四类段落之一（SECTION_KINDS）的小节：transition（交界 ±1）、exclude
    与没有标签的小节一律不计；整首 exclude 的歌不算任何指标；没有 drop 标签（D 不知道）的歌不算 M1 / M2 / M4。
      M1  I_end − D（汇总取 |·|）：与 T-038 离线门控的 |满编小节能量平均 − D|（0.15 / 0.60 / 0.92 LU）对照；
      M2  计入的小节 j ≥ 16 上 |held(j) − D| 的逐首中位（另给逐首 P90、最大）；汇总 = 各首中位的中位 / P90 / 最大，对规格第 7 节第 1 条
          「中位 ≤ 0.3、P90 ≤ 1.0 LU」；另给全部小节合在一起的分布（参考）；
      M2b 同上，从 j ≥ d + 4 起（第一个 drop 的 4 小节都已并入之后）；
      M3  每段 breakdown（连续标为 breakdown 的小节 a … b）：max_{k = a … b} |I_k − I_{a−1}|，即进入 breakdown 时手里的 I（held(a)）
          之后、到 breakdown 之后第一小节手里的 I（held(b + 1)）为止的最大漂移；第 2 条要 ≤ 0.5 LU；
      M4  δ4 = held(d + 4) − I_end = I_{d+3} − I_end（第一个 drop 的 4 小节并入之后与最终值之差；第 3 条要 |δ4| ≤ 1 LU）；n = settle_bars
          （达到并一直保持 ±1 LU 所需的 drop 小节数；n ≤ 4 ⟺ 第 3 条成立且一直保持到歌尾）；另给以「最后一个 drop 小节并入之后的 I」为
          终值、窗口截到那之后一小节的同一对（_alt：不受尾奏影响）；
      另  各类段落已听小节里 m ≥ 0.5 的占比（交界不计）；换歌软遗忘触发的次数与所在小节；无主段时间衰减生效的段数与次数（从不生效到
          生效记一次）；c 第一次 ≥ 0.8 时累计并入的有效音频（active_s）与小节、之后跌回 0.8 以下的次数；held(d) − D（第一个 drop 开始时
          手里的 I；前奏被当成主段、又比 drop 轻时为负）。"""
    track = r['track']
    excluded = bool(bars) and all(kinds[b[0]] == 'exclude' for b in bars)
    heard = {b[0]: b[4] for b in bars}
    counted = sorted(j for j in kinds if heard.get(j) and kinds[j] in SECTION_KINDS) if not excluded else []
    rows_by_bar = {int(row['bar']): row for row in trace if row['bar']}
    level = {bar: num(row['main_level_lufs']) for bar, row in rows_by_bar.items()}
    final_level = num(trace[-1]['main_level_lufs']) if trace else math.nan
    drop = drop_loudness(bars, kinds)
    drop_bars = [b[0] for b in bars if kinds[b[0]] == 'drop']
    first_drop = min(drop_bars) if drop_bars else None
    last_drop = max(drop_bars) if drop_bars else None

    def held(bar):
        return level.get(bar - 1, math.nan)

    def errors_from(start):
        return [abs(held(j) - drop) for j in counted if j >= start and math.isfinite(held(j))]

    usable = not excluded and math.isfinite(drop)
    m2 = errors_from(LIVE_START_BAR) if usable else []
    m2b = errors_from(first_drop + LIVE_DROP_OFFSET_BARS) if usable else []

    breakdowns = []
    for run in section_runs(kinds, 'breakdown') if not excluded else []:
        a, b = run[0], run[-1]
        before = [k for k in level if k < a]
        entry = level[max(before)] if before else math.nan
        inside = [level[k] for k in range(a, b + 1) if k in level and math.isfinite(level[k])]
        span = [rows_by_bar[k] for k in range(a, b + 1) if k in rows_by_bar]
        breakdowns.append({'class': r['class'], 'track': track, 'first_bar': a, 'last_bar': b, 'bars': b - a + 1,
                           'entry_i': entry, 'drift': max((abs(x - entry) for x in inside), default=math.nan),
                           'new_program': sum(row['new_program'] == '1' for row in span),
                           'time_decay': sum(row['time_decay'] == '1' for row in span)})

    main_counts = {kind: [0, 0] for kind in SECTION_KINDS}   # [m ≥ 0.5 的计入小节, 计入小节]
    for bar in counted:
        if bar in rows_by_bar:
            main_counts[kinds[bar]][1] += 1
            main_counts[kinds[bar]][0] += num(rows_by_bar[bar]['mainness']) >= MAIN_THRESHOLD

    new_program_bars = [row['bar'] for row in trace if row['new_program'] == '1']
    decay_flags = [row['time_decay'] == '1' for row in trace]
    decay_episodes = sum(1 for i, on in enumerate(decay_flags) if on and (i == 0 or not decay_flags[i - 1]))
    first_decay = next((row['bar'] for row in trace if row['time_decay'] == '1'), '')
    confidences = [num(row['confidence']) for row in trace]
    reach = next((i for i, c in enumerate(confidences) if c >= CONFIDENCE_FULL), None)
    dips = sum(1 for i in range(1, len(confidences)) if reach is not None and i > reach
               and confidences[i - 1] >= CONFIDENCE_FULL > confidences[i])
    misaligned = sum(1 for row in trace[:-1] if row['bar_offset'] not in ('', '0'))

    def m4(reference, window_end):
        delta = held(first_drop + LIVE_DROP_OFFSET_BARS) - reference
        return delta, settle_bars(held, counted, first_drop, reference, window_end)

    delta4, settle = m4(final_level, None) if usable else (math.nan, math.nan)
    alt_delta4, alt_settle = m4(level.get(last_drop, math.nan), last_drop + 1) if usable else (math.nan, math.nan)
    m2_stats, m2b_stats = live_summary(m2), live_summary(m2b)
    row = {
        'class': r['class'], 'track': track, 'bpm': r['bpm'], 'tempo_reliable': int(reliable_tempo(r)), 'excluded': int(excluded),
        'genre': final.get('genre', ''), 'model': final.get('model', ''), 'bars': len(bars), 'commits': len(trace),
        'misaligned_segments': misaligned, 'first_drop_bar': first_drop or '', 'last_drop_bar': last_drop or '',
        'drop_lufs': drop, 'final_i_lufs': final_level, 'm1_final_minus_drop': final_level - drop if usable else math.nan,
        'm2_median': m2_stats[0], 'm2_p90': m2_stats[1], 'm2_max': m2_stats[2], 'm2_bars': m2_stats[3],
        'm2b_median': m2b_stats[0], 'm2b_p90': m2b_stats[1], 'm2b_max': m2b_stats[2], 'm2b_bars': m2b_stats[3],
        'm3_breakdowns': len(breakdowns), 'm3_max_drift': max((x['drift'] for x in breakdowns if math.isfinite(x['drift'])), default=math.nan),
        'm3_over': sum(1 for x in breakdowns if math.isfinite(x['drift']) and x['drift'] > LIVE_DRIFT_LIMIT_LU),
        'm4_delta4': delta4, 'm4_settle_bars': settle, 'm4_alt_delta4': alt_delta4, 'm4_alt_settle_bars': alt_settle,
        'held_at_first_drop_minus_drop': held(first_drop) - drop if usable else math.nan,
        'new_program_count': len(new_program_bars), 'new_program_bars': ' '.join(new_program_bars),
        'time_decay_segments': sum(decay_flags), 'time_decay_episodes': decay_episodes, 'time_decay_first_bar': first_decay,
        'c08_active_s': num(trace[reach]['active_s']) if reach is not None else math.nan,
        'c08_bar': trace[reach]['bar'] if reach is not None else '', 'c08_dips': dips,
        'final_confidence': num(final.get('diag.confidence')), 'final_main_mass_s': num(final.get('diag.main_mass_s')),
        'final_lra_lu': num(final.get('profile.loudness_range_lu')), 'final_slope': num(final.get('profile.slope_db_per_oct')),
        'final_low_share': num(final.get('profile.low_share')),
        'anomalies': ' '.join(k for k in ('anomaly_overrun', 'anomaly_mismatch') if final.get(k) == '1'),
    }
    for kind in SECTION_KINDS:
        row[f'{kind}_main_bars'], row[f'{kind}_heard_bars'] = main_counts[kind]
    counted_set = set(counted)
    per_bar = [{'class': r['class'], 'track': track, 'bar': bar, 'label': kinds[bar], 'heard': int(bool(heard.get(bar))),
                'counted': int(bar in counted_set),
                'segment_lufs': next((b[1] for b in bars if b[0] == bar), math.nan), 'drop_lufs': drop, 'i_after': level.get(bar, math.nan),
                'i_held': held(bar), 'abs_error_held': abs(held(bar) - drop) if math.isfinite(drop) else math.nan,
                'mainness': num(rows_by_bar[bar]['mainness']) if bar in rows_by_bar else math.nan,
                'confidence': num(rows_by_bar[bar]['confidence']) if bar in rows_by_bar else math.nan,
                'time_decay': rows_by_bar[bar]['time_decay'] if bar in rows_by_bar else '',
                'new_program': rows_by_bar[bar]['new_program'] if bar in rows_by_bar else ''} for bar in sorted(kinds)]
    return {'row': row, 'bars': per_bar, 'breakdowns': breakdowns}


def analyze_live_tag(work, tag, rows, labels):
    """一个标签下 45 首（或合成两首）的指标；缺追踪的曲目记进 missing。"""
    base = live_dir(work, tag)
    tracks, missing = [], []
    for r in rows:
        trace, final = load_live_trace(base, r['track'])
        segments_path = os.path.join(base, 'segments', r['track'] + '__input.csv')
        if trace is None or not os.path.isfile(segments_path):
            missing.append(r['track'])
            continue
        bars = track_bars(read_segments(segments_path), float(r['bpm']))
        tracks.append(live_track_metrics(r, bars, bar_kinds(labels, r['track'], bars), trace, final))
    meta = {}
    if os.path.isfile(os.path.join(base, 'run.tsv')):
        with open(os.path.join(base, 'run.tsv'), encoding='utf-8') as f:
            meta = dict(line.rstrip('\n').split('\t', 1) for line in f if '\t' in line)
    return {'tag': tag, 'meta': meta, 'tracks': tracks, 'missing': missing}


def csv_value(value):
    if isinstance(value, float):
        return '' if not math.isfinite(value) else f'{value:.4f}'
    return str(value)


def write_live_csv(path, records):
    if not records:
        return
    with open(path, 'w', encoding='utf-8', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(list(records[0].keys()))
        writer.writerows([csv_value(v) for v in record.values()] for record in records)


def live_headline(result):
    """汇总的数（markdown 表与几个标签的对照共用）。逐首指标只取有 drop 标签、不是整首 exclude 的曲目。"""
    rows = [t['row'] for t in result['tracks'] if not t['row']['excluded']]
    scored = [x for x in rows if math.isfinite(x['drop_lufs'])]
    breakdowns = [b for t in result['tracks'] if not t['row']['excluded'] for b in t['breakdowns'] if math.isfinite(b['drift'])]
    pooled_m2 = [t2['abs_error_held'] for t in result['tracks'] if not t['row']['excluded'] and math.isfinite(t['row']['drop_lufs'])
                 for t2 in t['bars'] if t2['bar'] >= LIVE_START_BAR and t2['counted'] and math.isfinite(t2['abs_error_held'])]
    main = {kind: [sum(x[f'{kind}_main_bars'] for x in rows), sum(x[f'{kind}_heard_bars'] for x in rows)] for kind in SECTION_KINDS}
    return {
        'tracks': len(rows), 'scored': len(scored),
        'm1': live_summary([abs(x['m1_final_minus_drop']) for x in scored]),
        'm2': live_summary([x['m2_median'] for x in scored]),
        'm2_pooled': live_summary(pooled_m2),
        'm2b': live_summary([x['m2b_median'] for x in scored]),
        'm3': live_summary([b['drift'] for b in breakdowns]),
        'm3_pass': sum(1 for b in breakdowns if b['drift'] <= LIVE_DRIFT_LIMIT_LU), 'm3_total': len(breakdowns),
        'm4': live_summary([abs(x['m4_delta4']) for x in scored]),
        'm4_pass': sum(1 for x in scored if math.isfinite(x['m4_delta4']) and abs(x['m4_delta4']) <= LIVE_SETTLE_LIMIT_LU),
        'm4_settle': live_summary([float(x['m4_settle_bars']) for x in scored]),
        'm4_settle_pass': sum(1 for x in scored if math.isfinite(x['m4_settle_bars']) and x['m4_settle_bars'] <= LIVE_DROP_OFFSET_BARS),
        'm4_alt': live_summary([abs(x['m4_alt_delta4']) for x in scored]),
        'm4_alt_pass': sum(1 for x in scored if math.isfinite(x['m4_alt_delta4']) and abs(x['m4_alt_delta4']) <= LIVE_SETTLE_LIMIT_LU),
        'held': live_summary([x['held_at_first_drop_minus_drop'] for x in scored]),
        'c08': live_summary([x['c08_active_s'] for x in rows]),
        'c08_never': sum(1 for x in rows if not math.isfinite(x['c08_active_s'])),
        'main': main,
        'new_program_tracks': sum(1 for x in rows if x['new_program_count'] > 0),
        'new_program_total': sum(x['new_program_count'] for x in rows),
        'decay_tracks': sum(1 for x in rows if x['time_decay_episodes'] > 0),
        'misaligned': sum(x['misaligned_segments'] for x in rows),
        'anomalies': [x['track'] for x in rows if x['anomalies']],
    }


def stats_cells(stats, digits=2):
    median, p90, worst, n = stats
    return f'{fmt(median, digits)} | {fmt(p90, digits)} | {fmt(worst, digits)} | {n}'


def live_tag_markdown(result):
    """一个标签的分析（analysis.md）：汇总表（与规格第 7 节门槛对照）、段落的 m 分布（对照 T-038）、触发与置信度、逐首表、超限的 breakdown。"""
    h = live_headline(result)
    meta = result['meta']
    rows = [t['row'] for t in result['tracks']]
    m2_ok = h['m2'][0] <= LIVE_M2_LIMITS_LU[0] and h['m2'][1] <= LIVE_M2_LIMITS_LU[1]
    m2b_ok = h['m2b'][0] <= LIVE_M2_LIMITS_LU[0] and h['m2b'][1] <= LIVE_M2_LIMITS_LU[1]
    out = [f"# 因果重放：标签 {result['tag']}\n",
           f"- 曲风：{meta.get('genre', '?')}（auto = 各首的 House 子类）；主段模型：{', '.join(sorted({x['model'] for x in rows})) or '?'}；"
           f"常数覆盖：{meta.get('live_constants') or '无（出厂）'}；--live-model：{meta.get('live_model') or '无'}；块长 {meta.get('block', '?')}",
           f"- 渲染工具 {meta.get('render', '?')}（sha256 {meta.get('render_sha256', '?')[:12]}…），{meta.get('date', '?')}",
           f"- 曲目 {len(result['tracks'])} 首（整首 exclude {sum(1 for x in rows if x['excluded'])} 首不计）；有 drop 标签可算 M1 / M2 / M4 的 {h['scored']} 首"
           + (f"；缺追踪：{', '.join(result['missing'])}" if result['missing'] else '')
           + (f"；追踪异常：{', '.join(h['anomalies'])}" if h['anomalies'] else '')
           + (f"；不在小节线上的段 {h['misaligned']} 个（不该有）" if h['misaligned'] else ''),
           '', '## 汇总（与设计规格-实时GLAZE-主段与增益 第 7 节对照；定义见 calibrate_house.py live_track_metrics）', '',
           '| 指标 | 中位 | P90 | 最大 | 个数 | 门槛 | 结论 |', '|---|---|---|---|---|---|---|',
           f"| M1 \\|I_end − D\\|（LU，逐首） | {stats_cells(h['m1'])} | 对照 T-038 离线 {' / '.join(fmt(v) for v in T038_OFFLINE_LU)} | 只对照 |",
           f"| M2 第 16 小节起 \\|I − D\\| 的逐首中位（LU） | {stats_cells(h['m2'])} | 中位 ≤ {LIVE_M2_LIMITS_LU[0]}、P90 ≤ {LIVE_M2_LIMITS_LU[1]} | "
           f"{'达标' if m2_ok else '不达标'} |",
           f"| M2（全部小节合在一起，参考） | {stats_cells(h['m2_pooled'])} | — | — |",
           f"| M2b 第一个 drop + 4 小节起（逐首中位） | {stats_cells(h['m2b'])} | 同 M2 | {'达标' if m2b_ok else '不达标'} |",
           f"| M3 breakdown 内 I 的最大漂移（LU，逐段） | {stats_cells(h['m3'])} | 每段 ≤ {LIVE_DRIFT_LIMIT_LU} | "
           f"{h['m3_pass']} / {h['m3_total']} 段达标 |",
           f"| M4 第一个 drop 4 小节后 \\|I − I_end\\|（LU） | {stats_cells(h['m4'])} | ≤ {LIVE_SETTLE_LIMIT_LU} | {h['m4_pass']} / {h['scored']} 首达标 |",
           f"| M4 达到并保持 ±1 LU 所需的 drop 小节数 | {stats_cells(h['m4_settle'], 0)} | ≤ {LIVE_DROP_OFFSET_BARS} | "
           f"{h['m4_settle_pass']} / {h['scored']} 首 |",
           f"| M4（终值 = 最后一个 drop 小节之后的 I） | {stats_cells(h['m4_alt'])} | ≤ {LIVE_SETTLE_LIMIT_LU} | {h['m4_alt_pass']} / {h['scored']} 首 |",
           f"| 第一个 drop 开始时手里的 I − D（LU，带符号） | {stats_cells(h['held'])} | — | 负 = 前奏当了主段、比 drop 轻 |",
           f"| c 第一次到 {CONFIDENCE_FULL} 时的累计有效音频（s） | {stats_cells(h['c08'], 1)} | — | 到不了的 {h['c08_never']} 首 |",
           '', '## 各类段落的主段度（已听小节里 m ≥ 0.5 的占比；对照 T-038 出厂离线门控）', '',
           '| 段落 | m ≥ 0.5 | 已听小节 | 占比 | 换成 T-038 的说法 | T-038 出厂 |', '|---|---|---|---|---|---|']
    for kind in SECTION_KINDS:
        main, heard_bars = h['main'][kind]
        share = 100.0 * main / heard_bars if heard_bars else math.nan
        word, reference = T038_GATE_PERCENT.get(kind, ('', None))
        said = (f'{word} {fmt(share if word == "过" else 100.0 - share, 0)} %' if word else '—')
        out.append(f"| {kind} | {main} | {heard_bars} | {fmt(share, 0)} % | {said} | {f'{word} {reference} %' if reference else '—'} |")
    out += ['', '## 换歌软遗忘、无主段时间衰减', '',
            f"- 换歌软遗忘：{h['new_program_tracks']} 首触发过，共 {h['new_program_total']} 次；无主段时间衰减：{h['decay_tracks']} 首生效过（逐首见下表）。", '',
            '## 逐首', '',
            '| 子类 | 曲目 | 小节 | 首个 drop | D | I_end − D | M2 中位 / P90 | M2b 中位 | M3 最大（段数） | M4 δ4 / n | drop 开始时 I − D | '
            '前奏 m≥0.5 | drop m≥0.5 | 换歌（小节） | 时间衰减（次 / 段） | c≥0.8（s / 小节） |',
            '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for x in rows:
        if x['excluded']:
            out.append(f"| {x['class']} | {x['track']} | {x['bars']} | 整首 exclude | | | | | | | | | | | | |")
            continue
        out.append(f"| {x['class']} | {x['track']}{'' if x['tempo_reliable'] else '（速度低置信）'} | {x['bars']} | {x['first_drop_bar'] or '—'} | "
                   f"{fmt(x['drop_lufs'])} | {fmt(x['m1_final_minus_drop'], 2, True)} | {fmt(x['m2_median'])} / {fmt(x['m2_p90'])} | "
                   f"{fmt(x['m2b_median'])} | {fmt(x['m3_max_drift'])}（{x['m3_breakdowns']}） | {fmt(x['m4_delta4'], 2, True)} / "
                   f"{fmt(x['m4_settle_bars'], 0)} | {fmt(x['held_at_first_drop_minus_drop'], 2, True)} | "
                   f"{x['intro_main_bars']} / {x['intro_heard_bars']} | {x['drop_main_bars']} / {x['drop_heard_bars']} | "
                   f"{x['new_program_count']}{'（' + x['new_program_bars'] + '）' if x['new_program_bars'] else ''} | "
                   f"{x['time_decay_episodes']} / {x['time_decay_segments']} | {fmt(x['c08_active_s'], 1)} / {x['c08_bar'] or '—'} |")
    over = [b for t in result['tracks'] for b in t['breakdowns'] if math.isfinite(b['drift']) and b['drift'] > LIVE_DRIFT_LIMIT_LU]
    out += ['', f'## 漂移超过 {LIVE_DRIFT_LIMIT_LU} LU 的 breakdown', '']
    if over:
        out += ['| 子类 | 曲目 | 小节 | 长度 | 进入时 I | 最大漂移 | 其间换歌 / 时间衰减的段 |', '|---|---|---|---|---|---|---|']
        out += [f"| {b['class']} | {b['track']} | {b['first_bar']} ～ {b['last_bar']} | {b['bars']} | {fmt(b['entry_i'])} | {fmt(b['drift'])} | "
                f"{b['new_program']} / {b['time_decay']} |" for b in over]
    else:
        out.append('（没有）')
    return '\n'.join(out) + '\n'


def live_combined_markdown(results):
    """几个标签并排（--out）：每个标签一列，行 = 汇总的数。"""
    heads = [(r['tag'], live_headline(r)) for r in results]
    out = ['# 因果重放汇总（各标签并排；中位 / P90 / 最大，LU）', '', '| 指标 | ' + ' | '.join(tag for tag, _ in heads) + ' |',
           '|---|' + '---|' * len(heads)]

    def line(name, cell):
        out.append(f'| {name} | ' + ' | '.join(cell(h) for _, h in heads) + ' |')

    def triple(key, digits=2):
        return lambda h: ' / '.join(fmt(v, digits) for v in h[key][:3])

    line('可算的曲目（有 drop 标签）', lambda h: str(h['scored']))
    line('M1 \\|I_end − D\\|（对照 T-038 离线 0.15 / 0.60 / 0.92）', triple('m1'))
    line(f'M2 第 16 小节起逐首中位（门槛 中位 ≤ {LIVE_M2_LIMITS_LU[0]}、P90 ≤ {LIVE_M2_LIMITS_LU[1]}）', triple('m2'))
    line('M2 全部小节合在一起', triple('m2_pooled'))
    line('M2b 第一个 drop + 4 起', triple('m2b'))
    line(f'M3 breakdown 漂移（每段 ≤ {LIVE_DRIFT_LIMIT_LU}；达标段数）', lambda h: triple('m3')(h) + f"（{h['m3_pass']} / {h['m3_total']}）")
    line(f'M4 \\|I_{{d+3}} − I_end\\|（≤ {LIVE_SETTLE_LIMIT_LU}；达标首数）', lambda h: triple('m4')(h) + f"（{h['m4_pass']} / {h['scored']}）")
    line('M4 保持所需 drop 小节数（≤ 4 的首数）', lambda h: triple('m4_settle', 0)(h) + f"（{h['m4_settle_pass']} / {h['scored']}）")
    line('第一个 drop 开始时 I − D', triple('held'))
    line('c 到 0.8 的累计有效音频 s（到不了的首数）', lambda h: triple('c08', 1)(h) + f"（{h['c08_never']}）")
    for kind in SECTION_KINDS:
        line(f'{kind} 已听小节 m ≥ 0.5 占比', lambda h, k=kind: f"{fmt(100.0 * h['main'][k][0] / h['main'][k][1], 0) if h['main'][k][1] else '—'} %")
    line('换歌软遗忘：触发的首数 / 次数', lambda h: f"{h['new_program_tracks']} / {h['new_program_total']}")
    line('无主段时间衰减生效的首数', lambda h: str(h['decay_tracks']))
    return '\n'.join(out) + '\n'


def plot_live(rows, results, figures):
    """每页 3 首：上 = 输入逐小节段响度（灰）、D（虚线）、各标签第 j 小节播放时手里的 I（I_{j−1}）；下 = 各标签的 m 与 c。顶上一条是段落标签。"""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams['font.sans-serif'] = ['Noto Sans CJK SC', 'Noto Sans CJK JP', 'DejaVu Sans']
    plt.rcParams['axes.unicode_minus'] = False
    os.makedirs(figures, exist_ok=True)
    colours = {'intro': '#4e79a7', 'drop': '#e15759', 'breakdown': '#59a14f', 'outro': '#9c755f', 'transition': '#bab0ac', 'exclude': '#ffffff'}
    lines = ['#f28e2b', '#76b7b2', '#b07aa1', '#edc948']
    by_tag = {res['tag']: {t['row']['track']: t for t in res['tracks']} for res in results}
    shown = [r for r in rows if any(r['track'] in tracks for tracks in by_tag.values())]
    pages = [shown[i:i + 3] for i in range(0, len(shown), 3)]
    for page_index, page in enumerate(pages):
        fig, axes = plt.subplots(len(page) * 2, 1, figsize=(16, 4.4 * len(page)), gridspec_kw={'height_ratios': [1.6, 0.8] * len(page)})
        for i, r in enumerate(page):
            top, bottom = axes[2 * i], axes[2 * i + 1]
            first = next(tracks[r['track']] for tracks in by_tag.values() if r['track'] in tracks)
            bars = [b['bar'] for b in first['bars']]
            top.plot(bars, [b['segment_lufs'] for b in first['bars']], color='#999999', lw=0.8, drawstyle='steps-mid', label='段响度（输入）')
            top.axhline(first['row']['drop_lufs'], color='#e15759', lw=1, ls='--', label='D（drop 响度）')
            for j, (tag, tracks) in enumerate(by_tag.items()):
                if r['track'] not in tracks:
                    continue
                t = tracks[r['track']]
                colour = lines[j % len(lines)]
                top.plot(bars, [b['i_held'] for b in t['bars']], color=colour, lw=1.4, drawstyle='steps-post', label=f'I（{tag}）')
                bottom.plot(bars, [b['mainness'] for b in t['bars']], color=colour, lw=0.9, drawstyle='steps-mid', label=f'm（{tag}）')
                bottom.plot(bars, [b['confidence'] for b in t['bars']], color=colour, lw=0.9, ls=':', label=f'c（{tag}）')
            for b in first['bars']:
                top.axvspan(b['bar'] - 0.5, b['bar'] + 0.5, ymin=0.95, ymax=1.0, color=colours.get(b['label'], '#ffffff'))
            finite = [v for v in [b['segment_lufs'] for b in first['bars']] if math.isfinite(v)]
            if finite:
                top.set_ylim(max(min(finite), first['row']['drop_lufs'] - 14.0 if math.isfinite(first['row']['drop_lufs']) else min(finite)) - 1.0,
                             max(finite) + 2.0)
            top.set_xlim(0.5, len(bars) + 0.5)
            bottom.set_xlim(0.5, len(bars) + 0.5)
            bottom.set_ylim(-0.05, 1.05)
            row = first['row']
            top.set_title(f"{r['class']} {r['track']}  {float(r['bpm']):.2f} BPM{'' if reliable_tempo(r) else '【速度低置信】'}  首个 drop {row['first_drop_bar'] or '—'}  "
                          f"D {fmt(row['drop_lufs'])}  I_end − D {fmt(row['m1_final_minus_drop'], 2, True)}  M2 中位 {fmt(row['m2_median'])}", fontsize=9, loc='left')
            top.grid(axis='x', alpha=0.3)
            bottom.grid(axis='x', alpha=0.3)
            bottom.set_xticks(range(1, len(bars) + 1, 8))
            if i == 0:
                top.legend(fontsize=7, ncol=6, loc='lower left')
                bottom.legend(fontsize=7, ncol=6, loc='lower left')
        fig.tight_layout()
        fig.savefig(os.path.join(figures, f'live-{page_index + 1:02d}.png'), dpi=60)
        plt.close(fig)
    return len(pages)


def cmd_analyze_live(args):
    rows = load_manifest(args.work)
    labels = load_labels(args.work, args.labels, args.auto_labels)
    root = os.path.join(args.work, LIVE_DIR)
    tags = args.tags.split(',') if args.tags else sorted(t for t in os.listdir(root) if os.path.isdir(os.path.join(root, t, 'traces')))
    results = []
    for tag in tags:
        result = analyze_live_tag(args.work, tag, rows, labels)
        base = live_dir(args.work, tag)
        with open(os.path.join(base, 'analysis.md'), 'w', encoding='utf-8') as f:
            f.write(live_tag_markdown(result))
        write_live_csv(os.path.join(base, 'tracks.csv'), [t['row'] for t in result['tracks']])
        write_live_csv(os.path.join(base, 'bars.csv'), [b for t in result['tracks'] for b in t['bars']])
        write_live_csv(os.path.join(base, 'breakdowns.csv'), [b for t in result['tracks'] for b in t['breakdowns']])
        print(f'analyze-live：{tag} {len(result["tracks"])} 首 → {base}/analysis.md、tracks.csv、bars.csv、breakdowns.csv', flush=True)
        results.append(result)
    text = live_combined_markdown(results)
    print(text)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text)
    if args.figures:
        pages = plot_live(rows, results, args.figures)
        print(f'analyze-live：图 {pages} 页 → {args.figures}', flush=True)


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
    p = sub.add_parser('prep-synthetic', help='T-048：合成 House 两首摆成工作目录（manifest.tsv、labels-auto.tsv）')
    p.add_argument('work')
    p = sub.add_parser('render-live', help='T-048：逐首跑实时画像追踪（celadon_render --live-trace --live-only）')
    p.add_argument('work')
    p.add_argument('render')
    p.add_argument('-j', '--jobs', type=int, default=12)
    p.add_argument('--tag', default='default', help='输出分组 live/<标签>/（一组常数一个标签）')
    p.add_argument('--genre', default='auto', choices=('auto', 'general') + CLASSES, help='auto = 各首的 House 子类；general = 通用档（响度型）')
    p.add_argument('--live-constants', help='原样交给 celadon_render --live-constants')
    p.add_argument('--live-model', choices=('fourOnFloor', 'loudness', 'passive'), help='原样交给 celadon_render --live-model')
    p.add_argument('--block', type=int, default=512, help='宿主块长（默认同渲染工具；块长 512 / 4096 的对照见 T-048 REPORT-C 2.4 节）')
    p = sub.add_parser('analyze-live', help='T-048：因果重放指标（M1 ～ M4 等），写 live/<标签>/ 下的 analysis.md 与 CSV')
    p.add_argument('work')
    p.add_argument('--labels', help='改标（同 analyze）')
    p.add_argument('--auto-labels', help='自动标签 labels-auto.tsv（默认 <工作目录>/labels-auto.tsv）')
    p.add_argument('--tags', help='逗号分隔；默认 live/ 下全部有 traces/ 的标签')
    p.add_argument('--out', help='几个标签并排的汇总 markdown')
    p.add_argument('--figures', help='出图目录（每页 3 首：I、m、c 的轨迹）')
    args = parser.parse_args(argv)
    commands = {'prep': cmd_prep, 'render': cmd_render, 'label': cmd_label, 'analyze': cmd_analyze, 'prep-synthetic': cmd_prep_synthetic,
                'render-live': cmd_render_live, 'analyze-live': cmd_analyze_live}
    return commands[args.command](args) or 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
