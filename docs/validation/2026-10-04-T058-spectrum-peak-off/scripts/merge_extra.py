#!/usr/bin/env python3
# T-058：把改前基准（spectrum-peak-bench 的逐秒行，干净二进制）与交叉核对（count_probe.py 的逐秒行：只因峰值线重建的帧数）按段对上，
# 估「PEAK 关时多出来的」重建 / 重画开销：
#   多出来的帧 K = 这一段里第一处判变化的是峰值线的帧（PEAK 关时屏上不变，见交叉核对）；
#   多出来的重画 ≈ K × 这一段曲线帧的每帧重画 CPU——先扣掉混在曲线帧里的读数框 / 响度小窗文字重画（探针数的「with text」帧数 ×
#     同一组合里只有文字重画的帧的每帧重画 CPU）：那部分改后照样要画，不算多出来的；
#   多出来的 advance ≈ K × （这一段曲线帧的每帧 advance CPU − 同一组合全静那几秒的每帧 advance CPU）——含重建路径与标脏，
#     也含重画后缓存变冷让下一帧 advance 变慢的那一点（×2 时更明显）。停止场景的基线取自最后全静的静音段，放正弦那几秒的 advance
#     本来就略贵一点（编辑器其余读数在变），那几段估出的 advance 部分偏大；它比重画小一个数量级，不影响结论；
#   占比 = 多出来的 / 这一段（或整组合）advance + 重画的 CPU。
# 用法：merge_extra.py <基准输出> <count_probe.py 的输出>
import re
import sys

bench_path, probe_path = sys.argv[1], sys.argv[2]
num = r'([0-9.]+|-)'
bench_re = re.compile(r'^  (\S.*?)\s+\| fr\s+(\d+) dirty\s+(\d+) curves\s+(\d+) \| dev px\s+(\d+) \| advance wall\s+' + num + r' cpu\s+' + num
                      + r' \(cpu/fr curve ' + num + r' other ' + num + r'\) \| paint wall\s+' + num + r' cpu\s+' + num
                      + r' \(cpu/fr curve ' + num + r' other ' + num + r'\) \| cpu\s+' + num)
probe_re = re.compile(r'^  (\S.*?)\s+fr\s+(\d+) \| rebuilds\s+(\d+) \((.*?)\) \| curves\s+(\d+) \(with text (\d+)\).*?peak-only\s+(\d+):')


def f(x):
    return None if x == '-' else float(x)


def read_bench(path):
    cases, cur = {}, None
    for row in open(path, encoding='utf-8'):
        row = row.rstrip('\n')
        if row.startswith('---- '):
            cur = cases.setdefault(row[5:], [])
            continue
        if row.startswith('==== summary'):
            cur = None
            continue
        m = bench_re.match(row)
        if m and cur is not None:
            label = m.group(1)
            if label.startswith('~') or label.startswith('='):
                continue
            cur.append(dict(label=label, frames=int(m.group(2)), dirty=int(m.group(3)), curves=int(m.group(4)), px=int(m.group(5)),
                            adv_cpu=f(m.group(7)), adv_curve=f(m.group(8)), adv_other=f(m.group(9)),
                            paint_cpu=f(m.group(11)), paint_curve=f(m.group(12)), cpu=f(m.group(14))))
    return cases


def read_probe(path):
    cases, cur = {}, None
    for row in open(path, encoding='utf-8'):
        row = row.rstrip('\n')
        if row.startswith('---- '):
            cur = cases.setdefault(row[5:], [])
            continue
        if row.startswith('===='):
            cur = None
            continue
        m = probe_re.match(row)
        if m and cur is not None:
            label = m.group(1)
            if label.startswith('='):
                continue
            cur.append(dict(label=label, frames=int(m.group(2)), rebuilds=int(m.group(3)), firsts=m.group(4), curves=int(m.group(5)),
                            curves_with_text=int(m.group(6)), peak_only=int(m.group(7))))
    return cases


bench = read_bench(bench_path)
probe = read_probe(probe_path)

print('case | extra frames (peak-only rebuilds, PEAK off) of all rebuilds | extra paint cpu ms | extra advance cpu ms | extra total ms | '
      'case cpu ms | share of case | fall window (segments where every frame is a peak-only rebuild): seconds, extra ms/s, share of that cpu')
for title, segs in bench.items():
    if 'PEAK off' not in title:
        continue
    psegs = probe[title]
    assert [s['label'] for s in segs] == [p['label'] for p in psegs], title
    # 全静那几秒：没有任何标脏的段，取它们 advance 的每帧 CPU 作「什么都没变」的基线
    quiet = [s for s in segs if s['dirty'] == 0 and s['frames'] > 1]
    base = sum(s['adv_cpu'] for s in quiet) / sum(s['frames'] for s in quiet)
    # 只有文字重画的帧（脏、但不是曲线帧）的每帧重画 CPU：整组合平均
    text_frames = sum(s['dirty'] - s['curves'] for s in segs)
    text_paint = sum(s['paint_cpu'] - (s['paint_curve'] or 0.0) * s['curves'] for s in segs)
    text_cost = text_paint / text_frames if text_frames > 0 else 0.0
    extra_paint = extra_adv = 0.0
    k_total = sum(p['peak_only'] for p in psegs)
    rebuilds = sum(p['rebuilds'] for p in psegs)
    fall_secs, fall_extra, fall_cpu = 0, 0.0, 0.0
    for s, p in zip(segs, psegs):
        k = p['peak_only']
        if k == 0:
            continue
        curve_only_paint = (s['paint_curve'] * s['curves'] - p['curves_with_text'] * text_cost) / s['curves']
        seg_paint = k * curve_only_paint
        seg_adv = k * (s['adv_curve'] - base)
        extra_paint += seg_paint
        extra_adv += seg_adv
        if k == s['frames']:
            fall_secs += 1
            fall_extra += seg_paint + seg_adv
            fall_cpu += s['cpu']
    case_cpu = sum(s['cpu'] for s in segs)
    total = extra_paint + extra_adv
    print(f'{title} | {k_total} of {rebuilds} | {extra_paint:.1f} | {extra_adv:.1f} | {total:.1f} | {case_cpu:.1f} | {100 * total / case_cpu:.0f} % | '
          f'{fall_secs} s, {fall_extra / max(1, fall_secs):.1f} ms/s, {100 * fall_extra / max(1e-9, fall_cpu):.0f} % '
          f'(static advance baseline {base * 1000:.1f} us/fr; text-only repaint {text_cost:.3f} ms/fr)')
