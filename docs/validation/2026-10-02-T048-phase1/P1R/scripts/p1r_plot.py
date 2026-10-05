#!/usr/bin/env python3
# T-048 阶段 1 · P1R：挑几首歌画「回看重判（新）vs 提交时定 m（旧）」——上：输入段响度、D、两组手里的 I（held(j) = I_{j−1}）；
# 下：两组的 c 与新组的首判 m。竖线 = 第 d − 1 小节（真正的第一个 drop 小节）。读 analyze-live 写的 bars.csv / tracks.csv。
#
#   p1r_plot.py --out 图.png --new 工作目录:标签 --old 工作目录:标签 曲1 曲2 …（曲名可带工作目录前缀「syn/」表示合成曲目录）

import argparse
import csv
import math
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

for family in ('Noto Sans CJK SC', 'Noto Sans CJK JP', 'WenQuanYi Zen Hei', 'Droid Sans Fallback'):
    if any(family in f.name for f in matplotlib.font_manager.fontManager.ttflist):
        plt.rcParams['font.family'] = family
        break
plt.rcParams['axes.unicode_minus'] = False


def num(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return math.nan


def load(work, tag, track):
    base = os.path.join(work, 'live', tag)
    with open(os.path.join(base, 'tracks.csv'), encoding='utf-8') as f:
        row = next(r for r in csv.DictReader(f) if r['track'] == track)
    with open(os.path.join(base, 'bars.csv'), encoding='utf-8') as f:
        bars = [b for b in csv.DictReader(f) if b['track'] == track]
    return row, bars


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--new', required=True, help='Jamendo 工作目录:标签（合成曲用 --syn-dir 换目录，标签相同）')
    parser.add_argument('--old', required=True)
    parser.add_argument('--syn-dir', default=os.path.expanduser('~/.cache/celadon-t048-cal-syn'))
    parser.add_argument('tracks', nargs='+')
    args = parser.parse_args(argv)
    new_work, new_tag = args.new.split(':')
    old_work, old_tag = args.old.split(':')
    fig, axes = plt.subplots(len(args.tracks) * 2, 1, figsize=(11, 3.1 * len(args.tracks)),
                             gridspec_kw={'height_ratios': [2, 1] * len(args.tracks)})
    for k, name in enumerate(args.tracks):
        synthetic = name.startswith('syn/')
        track = name[4:] if synthetic else name
        nw = args.syn_dir if synthetic else new_work
        ow = args.syn_dir if synthetic else old_work
        new_row, new_bars = load(nw, new_tag, track)
        _, old_bars = load(ow, old_tag, track)
        top, bottom = axes[2 * k], axes[2 * k + 1]
        x = [int(b['bar']) for b in new_bars]
        top.step(x, [num(b['segment_lufs']) for b in new_bars], where='mid', color='0.7', lw=0.8, label='输入段响度')
        top.axhline(num(new_row['drop_lufs']), color='tab:red', ls='--', lw=0.8, label='D（drop 响度）')
        top.step(x, [num(b['i_held']) for b in new_bars], where='mid', color='tab:blue', lw=1.4, label=f'手里的 I（{new_tag}）')
        top.step([int(b['bar']) for b in old_bars], [num(b['i_held']) for b in old_bars], where='mid', color='tab:orange', lw=1.1,
                 label=f'手里的 I（{old_tag}）')
        d = int(new_row['first_drop_bar'])
        for ax in (top, bottom):
            ax.axvline(d - 1, color='k', lw=0.6, ls=':')
            ax.grid(alpha=0.25)
        finite = [v for b in new_bars for v in (num(b['segment_lufs']), num(b['i_held'])) if math.isfinite(v)]
        if finite:
            top.set_ylim(max(min(finite) - 1, num(new_row['drop_lufs']) - 14), max(finite) + 1)
        top.set_title(f"{new_row['class']} {track}　首个 drop 第 {d - 1} 小节（标签 d = {d}）　M4 δ4 {num(new_row['m4_delta4']):+.2f} LU", fontsize=9, loc='left')
        top.set_ylabel('LUFS', fontsize=8)
        top.legend(fontsize=7, loc='lower right', ncol=4)
        bottom.step(x, [num(b['confidence']) for b in new_bars], where='mid', color='tab:blue', lw=1.4, label=f'c（{new_tag}）')
        bottom.step([int(b['bar']) for b in old_bars], [num(b['confidence']) for b in old_bars], where='mid', color='tab:orange', lw=1.1,
                    label=f'c（{old_tag}）')
        bottom.step(x, [num(b['mainness']) for b in new_bars], where='mid', color='tab:green', lw=0.6, alpha=0.7, label=f'首判 m（{new_tag}）')
        bottom.axhline(0.8, color='0.4', lw=0.6, ls='--')
        bottom.set_ylim(-0.05, 1.05)
        bottom.set_ylabel('c / m', fontsize=8)
        bottom.legend(fontsize=7, loc='lower right', ncol=3)
        bottom.set_xlabel('小节', fontsize=8)
    fig.tight_layout()
    fig.savefig(args.out, dpi=72)
    print(args.out, os.path.getsize(args.out), '字节')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
