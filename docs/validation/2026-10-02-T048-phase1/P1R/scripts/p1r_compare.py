#!/usr/bin/env python3
# T-048 阶段 1 · P1R 的逐位对照脚本（DGX Spark 本机；依赖 numpy / soundfile）。
#
#   live <工作目录> <标签 A> <标签 B> [--out 逐首.csv]
#       两个 render-live 标签逐首比：追踪（每发布一份画像一行）、__final.csv（去掉 tag 一行；constants_override 不同时也去掉）、
#       __judgements.csv、输入端逐段统计 segments/<曲>__input.csv。不同就报第一处不同的行（段）、那一行里不同的列、不同的行数、
#       I / c / m 的最大绝对差。
#   tree <输出 A> <输出 B> [--out 文件.csv]
#       两棵 celadon_render 输出树逐文件比（metrics.md 去掉「生成时间」一行再比）；metrics.csv 不同就逐行逐列列出差；wav 不同就报
#       最大样本差（dBFS）与第一个不同的样本。
#
# 只读两边的文件，不改任何东西；结果打印到 stdout，--out 另写 CSV。

import argparse
import csv
import math
import os
import sys

import numpy as np

KEY_TRACE_COLUMNS = ('main_level_lufs', 'confidence', 'mainness', 'low_share', 'loudness_lufs')


def read_rows(path):
    with open(path, encoding='utf-8', newline='') as f:
        return list(csv.reader(f))


def to_float(text):
    try:
        return float(text)
    except ValueError:
        return math.nan


def compare_tables(rows_a, rows_b, label_column=None):
    """两张 CSV（第一行是表头）逐行逐字比。返回 dict：same、rows_a、rows_b、diff_rows、first_row（数据行号，从 0 起）、first_label、
    first_columns（那一行里不同的列名，; 连起来）、max_abs（列名 → 全部可比行上的最大绝对差）。"""
    header_a, header_b = rows_a[0], rows_b[0]
    result = {'same': rows_a == rows_b, 'rows_a': len(rows_a) - 1, 'rows_b': len(rows_b) - 1, 'diff_rows': 0,
              'first_row': '', 'first_label': '', 'first_columns': '', 'max_abs': {}, 'header_same': header_a == header_b}
    if result['same']:
        return result
    columns = [c for c in header_a if c in header_b]
    index_a = {c: i for i, c in enumerate(header_a)}
    index_b = {c: i for i, c in enumerate(header_b)}
    for n, (ra, rb) in enumerate(zip(rows_a[1:], rows_b[1:])):
        differing = [c for c in columns if ra[index_a[c]] != rb[index_b[c]]]
        if not differing:
            continue
        result['diff_rows'] += 1
        for c in differing:
            delta = abs(to_float(ra[index_a[c]]) - to_float(rb[index_b[c]]))
            if math.isfinite(delta):
                result['max_abs'][c] = max(result['max_abs'].get(c, 0.0), delta)
            else:
                result['max_abs'].setdefault(c, math.nan)
        if result['first_row'] == '':
            result['first_row'] = n
            result['first_label'] = ra[index_a[label_column]] if label_column in index_a else ''
            result['first_columns'] = ';'.join(differing)
    if len(rows_a) != len(rows_b):
        result['diff_rows'] += abs(len(rows_a) - len(rows_b))
        if result['first_row'] == '':
            result['first_row'] = min(len(rows_a), len(rows_b)) - 1
            result['first_columns'] = 'rows'
    return result


def read_final(path, drop_keys, drop_constants=False):
    """__final.csv → {键: 值}，去掉 drop_keys；drop_constants 时另去掉 const.* 各行（两组常数本来就不同，只比结果）。"""
    with open(path, encoding='utf-8', newline='') as f:
        return {r['key']: r['value'] for r in csv.DictReader(f)
                if r['key'] not in drop_keys and not (drop_constants and r['key'].startswith('const.'))}


def load_manifest(work):
    with open(os.path.join(work, 'manifest.tsv'), encoding='utf-8') as f:
        return list(csv.DictReader(f, delimiter='\t'))


def trace_names(folder, track):
    names = sorted(n for n in os.listdir(folder) if n.startswith(track + '__') and n.endswith('.csv')
                   and not n.endswith('__final.csv') and not n.endswith('__judgements.csv'))
    return names


def cmd_live(args):
    records = []
    base_a = os.path.join(args.work, 'live', args.tag_a)
    base_b = os.path.join(args.work, 'live', args.tag_b)
    for r in load_manifest(args.work):
        track = r['track']
        names_a = trace_names(os.path.join(base_a, 'traces'), track)
        names_b = trace_names(os.path.join(base_b, 'traces'), track)
        record = {'class': r['class'], 'track': track}
        if not names_a or names_a != names_b:
            record['status'] = f'缺追踪或名字不同：{names_a} / {names_b}'
            records.append(record)
            continue
        stem = names_a[0][:-4]
        trace = compare_tables(read_rows(os.path.join(base_a, 'traces', names_a[0])), read_rows(os.path.join(base_b, 'traces', names_a[0])), 'bar')
        drop = {'tag', 'constants_override'} if args.ignore_constants else {'tag'}
        final_a = read_final(os.path.join(base_a, 'traces', stem + '__final.csv'), drop, args.ignore_constants)
        final_b = read_final(os.path.join(base_b, 'traces', stem + '__final.csv'), drop, args.ignore_constants)
        final_keys = sorted(k for k in set(final_a) | set(final_b) if final_a.get(k) != final_b.get(k))
        judgement_a = os.path.join(base_a, 'traces', stem + '__judgements.csv')
        judgement_b = os.path.join(base_b, 'traces', stem + '__judgements.csv')
        if os.path.isfile(judgement_a) and os.path.isfile(judgement_b):
            judgements = compare_tables(read_rows(judgement_a), read_rows(judgement_b), 'bar')
        else:
            judgements = {'same': os.path.isfile(judgement_a) == os.path.isfile(judgement_b), 'diff_rows': '', 'first_label': '', 'first_columns': '缺文件'}
        seg_a = os.path.join(base_a, 'segments', track + '__input.csv')
        seg_b = os.path.join(base_b, 'segments', track + '__input.csv')
        with open(seg_a, 'rb') as fa, open(seg_b, 'rb') as fb:
            segments_same = fa.read() == fb.read()
        same = trace['same'] and not final_keys and judgements['same'] and segments_same
        record.update({
            'status': '相同' if same else '不同', 'trace_same': int(trace['same']), 'trace_rows': f"{trace['rows_a']}/{trace['rows_b']}",
            'trace_diff_rows': trace['diff_rows'], 'trace_first_bar': trace['first_label'], 'trace_first_columns': trace['first_columns'],
            **{f'max_abs_{c}': (f"{trace['max_abs'][c]:.6g}" if c in trace['max_abs'] else '') for c in KEY_TRACE_COLUMNS},
            'final_diff_keys': ';'.join(final_keys), 'judgements_same': int(bool(judgements['same'])),
            'judgements_diff_rows': judgements['diff_rows'], 'judgements_first_bar': judgements['first_label'],
            'judgements_first_columns': judgements['first_columns'], 'segments_same': int(segments_same)})
        records.append(record)
    same = sum(1 for x in records if x.get('status') == '相同')
    print(f'live {args.tag_a} vs {args.tag_b}（{args.work}）：{same} / {len(records)} 首相同')
    for x in records:
        if x.get('status') != '相同':
            print(f"  {x['class']} {x['track']}：{x.get('status')} 追踪不同 {x.get('trace_diff_rows')} 行，第一处在第 {x.get('trace_first_bar')} 小节："
                  f"{x.get('trace_first_columns')}；final 不同的键 {x.get('final_diff_keys')}；judgements 不同 {x.get('judgements_diff_rows')} 行；"
                  f"segments 相同 {x.get('segments_same')}；I 最大差 {x.get('max_abs_main_level_lufs')}、c {x.get('max_abs_confidence')}、m {x.get('max_abs_mainness')}")
    write_csv(args.out, records)
    return 0 if same == len(records) else 1


def metrics_md_text(path):
    with open(path, encoding='utf-8') as f:
        return [line for line in f if not line.startswith('- 生成时间')]


def wav_difference(path_a, path_b):
    import soundfile
    a, sr_a = soundfile.read(path_a, dtype='float64', always_2d=True)
    b, sr_b = soundfile.read(path_b, dtype='float64', always_2d=True)
    if a.shape != b.shape or sr_a != sr_b:
        return f'形状不同 {a.shape} / {b.shape}'
    diff = np.abs(a - b)
    peak = float(diff.max())
    first = int(np.argmax(diff.max(axis=1) > 0)) if peak > 0 else -1
    level = 20.0 * math.log10(peak) if peak > 0 else -math.inf
    return f'最大样本差 {level:.1f} dBFS，第一个不同的样本 {first}（{first / sr_a:.3f} s）'


def metrics_csv_difference(path_a, path_b):
    rows_a, rows_b = read_rows(path_a), read_rows(path_b)
    if rows_a[0] != rows_b[0]:
        return ['表头不同']
    header = rows_a[0]
    key_columns = [c for c in ('kind', 'song', 'glaze', 'clay', 'style', 'genre', 'delivery', 'listen_from_bar') if c in header]
    lines = []
    for ra, rb in zip(rows_a[1:], rows_b[1:]):
        if ra == rb:
            continue
        key = ' '.join(ra[header.index(c)] for c in key_columns)
        cells = [f'{c}: {va} → {vb}' for c, va, vb in zip(header, ra, rb) if va != vb]
        lines.append(f'{key}｜' + '；'.join(cells))
    if len(rows_a) != len(rows_b):
        lines.append(f'行数不同 {len(rows_a)} / {len(rows_b)}')
    return lines


def cmd_tree(args):
    files_a = {os.path.relpath(os.path.join(d, n), args.a) for d, _, ns in os.walk(args.a) for n in ns}
    files_b = {os.path.relpath(os.path.join(d, n), args.b) for d, _, ns in os.walk(args.b) for n in ns}
    records = []
    for rel in sorted(files_a | files_b):
        if rel not in files_a or rel not in files_b:
            records.append({'file': rel, 'status': '只在 A' if rel in files_a else '只在 B', 'detail': ''})
            continue
        path_a, path_b = os.path.join(args.a, rel), os.path.join(args.b, rel)
        if os.path.basename(rel) == 'metrics.md':
            same = metrics_md_text(path_a) == metrics_md_text(path_b)
        else:
            with open(path_a, 'rb') as fa, open(path_b, 'rb') as fb:
                same = fa.read() == fb.read()
        detail = ''
        if not same:
            if rel.endswith('.wav'):
                detail = wav_difference(path_a, path_b)
            elif os.path.basename(rel) == 'metrics.csv':
                detail = ' ‖ '.join(metrics_csv_difference(path_a, path_b))
            elif rel.endswith('.csv'):
                diff = compare_tables(read_rows(path_a), read_rows(path_b), None)
                detail = (f"{diff['diff_rows']} 行不同，第一处数据行 {diff['first_row']}：{diff['first_columns']}；最大差 "
                          + '；'.join(f'{c} {v:.6g}' for c, v in sorted(diff['max_abs'].items())[:12]))
        records.append({'file': rel, 'status': '相同' if same else '不同', 'detail': detail})
    same = sum(1 for x in records if x['status'] == '相同')
    print(f'tree {args.a} vs {args.b}：{same} / {len(records)} 个文件相同（metrics.md 去掉生成时间）')
    for x in records:
        if x['status'] != '相同':
            print(f"  {x['status']} {x['file']} {x['detail']}")
    write_csv(args.out, records)
    return 0 if same == len(records) else 1


def write_csv(path, records):
    if not path or not records:
        return
    keys = []
    for r in records:
        keys.extend(k for k in r if k not in keys)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(records)


def main(argv):
    parser = argparse.ArgumentParser(description='T-048 P1R 逐位对照')
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('live')
    p.add_argument('work')
    p.add_argument('tag_a')
    p.add_argument('tag_b')
    p.add_argument('--out')
    p.add_argument('--ignore-constants', action='store_true',
                   help='__final.csv 的 constants_override 与 const.* 各行不比（两组常数不同、只看结果时）')
    p = sub.add_parser('tree')
    p.add_argument('a')
    p.add_argument('b')
    p.add_argument('--out')
    args = parser.parse_args(argv)
    return cmd_live(args) if args.command == 'live' else cmd_tree(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
