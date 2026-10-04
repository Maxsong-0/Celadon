#!/usr/bin/env python3
# T-048 阶段 1 · P1R：几份 celadon_render 对同一批歌各跑同一组网格（声音不变核对、6a 对照用），之后用 p1r_compare.py tree 逐首比。
#
#   p1r_render_batch.py --out <根目录> --bin 名字=路径 [--bin …] --work <工作目录> [--work …] [--tracks 曲1,曲2] [-j 8]
#                       [--bpm] [--genre-mode sub|deep] [--glaze 0,35,70] [--from-bar 33] [--full]
#
# 工作目录 = calibrate_house.py 的布局（manifest.tsv、<类>/<曲>/<曲>.wav）。每首 × 每份二进制一个任务，输出 <根目录>/<名字>/<曲>/。
# 曲风：通用 + 该曲的子类（--genre-mode sub，manifest 的 class）或一律 deep（--genre-mode deep）。默认 --metrics-only（比 metrics.csv 的
# audio_hash 与全部指标）；--full 写成品 wav 并带 --trace --segments-csv（逐字节比成品）。已有 done 标记的任务跳过（中途断了可接着跑）。
# 每个任务 nice -n 10；-j 是同时跑的任务数。

import argparse
import concurrent.futures
import csv
import os
import subprocess
import sys
import time


def load_rows(work, tracks):
    with open(os.path.join(work, 'manifest.tsv'), encoding='utf-8') as f:
        rows = [dict(r, work=work) for r in csv.DictReader(f, delimiter='\t')]
    return [r for r in rows if not tracks or r['track'] in tracks]


def command_for(args, binary, r, out):
    genre = 'general,' + (r['class'] if args.genre_mode == 'sub' else 'deep')
    command = ['nice', '-n', '10', binary, '--in', os.path.join(r['work'], r['class'], r['track']), '--out', out,
               '--glaze', args.glaze, '--genre', genre]
    if args.bpm or args.from_bar:
        command += ['--bpm', r['bpm']]
    if args.from_bar:
        command += ['--listen-from-bar', args.from_bar]
    command += ['--trace', '--segments-csv'] if args.full else ['--metrics-only']
    return command


def run_one(args, name, binary, r):
    out = os.path.join(args.out, name, r['track'])
    done = os.path.join(args.out, name, '.done-' + r['track'])
    if os.path.isfile(done):
        return name, r['track'], 0, 0.0, '跳过（已完成）'
    os.makedirs(out, exist_ok=True)
    command = command_for(args, binary, r, out)
    start = time.time()
    with open(os.path.join(args.out, name, r['track'] + '.log'), 'w', encoding='utf-8') as log:
        log.write(' '.join(command) + '\n')
        log.flush()
        code = subprocess.call(command, stdout=log, stderr=subprocess.STDOUT)
    if code == 0:
        open(done, 'w').close()
    return name, r['track'], code, time.time() - start, ''


def main(argv):
    parser = argparse.ArgumentParser(description='P1R 批量渲染')
    parser.add_argument('--out', required=True)
    parser.add_argument('--bin', action='append', required=True, help='名字=celadon_render 路径')
    parser.add_argument('--work', action='append', required=True)
    parser.add_argument('--tracks', help='逗号分隔；默认 manifest 全部')
    parser.add_argument('-j', '--jobs', type=int, default=8)
    parser.add_argument('--bpm', action='store_true', help='给 --bpm（manifest 的速度）')
    parser.add_argument('--genre-mode', choices=('sub', 'deep'), default='sub')
    parser.add_argument('--glaze', default='0,35,70')
    parser.add_argument('--from-bar', help='--listen-from-bar（隐含 --bpm）')
    parser.add_argument('--full', action='store_true', help='写成品 wav，带 --trace --segments-csv')
    args = parser.parse_args(argv)
    tracks = set(args.tracks.split(',')) if args.tracks else set()
    rows = [r for work in args.work for r in load_rows(work, tracks)]
    rows.sort(key=lambda r: -float(r['duration_s']))   # 长的先跑，并行收尾更齐
    bins = [b.split('=', 1) for b in args.bin]
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'batch.txt'), 'a', encoding='utf-8') as f:
        f.write(f"{time.strftime('%F %T')} {' '.join(argv)}\n")
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, args, name, binary, r) for r in rows for name, binary in bins]
        for future in concurrent.futures.as_completed(futures):
            name, track, code, seconds, note = future.result()
            failed += code != 0
            print(f"{time.strftime('%T')} {name} {track} 退出码 {code} {seconds:.0f} s {note}", flush=True)
    print(f'完成：{len(futures) - failed} / {len(futures)} 个任务成功', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
