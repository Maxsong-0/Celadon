#!/usr/bin/env python3
# T-048 阶段 1 · P1R：实时画像常数扫描（计划 1.5 标定的准备；只用 --live-constants，不改 GlazeConstants 的默认值）。
#
#   p1r_scan.py --configs 组.tsv --render <celadon_render> --calibrate <calibrate_house.py> --out <汇总目录>
#               [--work <Jamendo 工作目录>] [--syn <合成曲工作目录>] [--labels 改标.tsv --auto-labels 自动标签.tsv] [-j 8] [--only 名字,…]
#
# 组.tsv：name<TAB>genre<TAB>constants（genre = auto / general；constants 原样交给 --live-constants，空 = 出厂）。每组的标签 = p1r-s-<name>
# （出厂那一组也重跑一遍，确定性另由 6b 核对）。一组 = render-live（-j 并行首数，nice 10）→ analyze-live（只给这一个标签）→ 汇总一行。
# 已有 <工作目录>/live/<标签>/tracks.csv 的组跳过渲染与分析（--redo 重来）。汇总写 <汇总目录>/scan.csv 与 scan.md。

import argparse
import csv
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p1r_live_extra as extra  # noqa: E402

PY = sys.executable


def run(command, log):
    with open(log, 'a', encoding='utf-8') as f:
        f.write(f"\n$ {' '.join(command)}\n")
        f.flush()
        return subprocess.call(command, stdout=f, stderr=subprocess.STDOUT)


def ensure_tag(args, work, labels, name, genre, constants, log):
    tag = 'p1r-s-' + name
    base = os.path.join(work, 'live', tag)
    if os.path.isfile(os.path.join(base, 'tracks.csv')) and not args.redo:
        return tag, 0
    command = ['nice', '-n', '10', PY, args.calibrate, 'render-live', work, args.render, '-j', str(args.jobs), '--tag', tag]
    if genre != 'auto':
        command += ['--genre', genre]
    if constants:
        command += ['--live-constants', constants]
    code = run(command, log)
    if code != 0:
        return tag, code
    command = ['nice', '-n', '10', PY, args.calibrate, 'analyze-live', work, '--tags', tag] + labels
    return tag, run(command, log)


def headline(work, tag):
    """一组的汇总数（与 analyze-live 的汇总同口径，另加 P1R 的 M2c、M4 拆分、c 下沉）。"""
    tracks, bars = extra.load_tag(work, tag)
    rows = [t for t in tracks.values() if t['excluded'] == '0']
    scored = [t for t in rows if extra.scored(t)]
    result = extra.tag_report(work, tag, None, with_reject=True)
    with open(os.path.join(work, 'live', tag, 'breakdowns.csv'), encoding='utf-8') as f:
        breakdowns = [extra.num(b['drift']) for b in csv.DictReader(f)
                      if tracks.get(b['track'], {}).get('excluded') == '0' and math.isfinite(extra.num(b['drift']))]
    m1 = extra.summary([abs(extra.num(t['m1_final_minus_drop'])) for t in scored])
    m3 = extra.summary(breakdowns)
    m4 = [extra.num(t['m4_delta4']) for t in scored]
    settle = [extra.num(t['m4_settle_bars']) for t in scored]
    c08 = [extra.num(t['c08_active_s']) for t in rows]
    split = result['m4split']
    dips = [x for x in result['dip'] if 'c_min_first_drop' in x]
    fell = [x for x in dips if math.isfinite(x['c_drop_over_pre']) and x['c_drop_over_pre'] > extra.DIP_MIN]

    def share(kind, final=False):
        a = sum(int(t[f'{kind}_final_main_bars' if final else f'{kind}_main_bars'] or 0) for t in rows)
        b = sum(int(t[f'{kind}_final_bars' if final else f'{kind}_heard_bars'] or 0) for t in rows)
        return 100.0 * a / b if b else math.nan

    h = {'tag': tag, 'scored': len(scored),
         'm1_med': m1[0], 'm1_p90': m1[1], 'm1_max': m1[2],
         'm2_med': result['heads']['m2'][0], 'm2_p90': result['heads']['m2'][1],
         'm2b_med': result['heads']['m2b'][0], 'm2b_p90': result['heads']['m2b'][1],
         'm2c_med': result['heads']['m2c'][0], 'm2c_p90': result['heads']['m2c'][1],
         'm2_pooled_p90': result['pooled']['m2'][1],
         'm3_p90': m3[1], 'm3_max': m3[2], 'm3_pass': sum(1 for x in breakdowns if x <= 0.5), 'm3_total': len(breakdowns),
         'm4_pass': sum(1 for x in m4 if math.isfinite(x) and abs(x) <= 1.0),
         'm4_med': extra.summary([abs(x) for x in m4])[0], 'm4_p90': extra.summary([abs(x) for x in m4])[1],
         'm4_settle4': sum(1 for x in settle if math.isfinite(x) and x <= 4),
         'm4_track_fail': sum(1 for x in split if abs(x['tracking']) > 1.0),
         'm4_track_med': extra.summary([abs(x['tracking']) for x in split])[0],
         'm4_track_p90': extra.summary([abs(x['tracking']) for x in split])[1],
         'c08_med': extra.summary(c08)[0], 'c08_p90': extra.summary(c08)[1], 'c08_never': sum(1 for x in c08 if not math.isfinite(x)),
         'min_c_after_drop_med': extra.summary([extra.num(t['min_c_after_first_drop']) for t in scored])[0],
         'dip_tracks': len(fell), 'dip_kappa_ratio_min': min((x['kappa_ratio_min_over_pre'] for x in fell), default=math.nan),
         'dip_kappa_ratio_med': extra.summary([x['kappa_ratio_min_over_pre'] for x in fell])[0],
         'intro_first': share('intro'), 'intro_final': share('intro', True), 'drop_first': share('drop'), 'drop_final': share('drop', True),
         'breakdown_first': share('breakdown'), 'outro_first': share('outro'),
         'new_program_tracks': sum(1 for t in rows if int(t['new_program_count'] or 0) > 0),
         'new_program_bars': ' '.join(f"{t['track'][-7:]}@{t['new_program_bars']}" for t in rows if t['new_program_bars']),
         'decay_tracks': sum(1 for t in rows if int(t['time_decay_episodes'] or 0) > 0)}
    h['spec_1'] = int(h['m2_med'] <= 0.3 and h['m2_p90'] <= 1.0)
    h['spec_2'] = int(h['m3_pass'] == h['m3_total'])
    h['spec_3'] = int(h['m4_pass'] == h['scored'])
    return h


MD_COLUMNS = [
    ('组', lambda h: h['name']), ('常数', lambda h: h['constants'] or '出厂'),
    ('M1 中位 / P90', lambda h: f"{extra.fmt(h['m1_med'])} / {extra.fmt(h['m1_p90'])}"),
    ('M2（≥16）', lambda h: f"{extra.fmt(h['m2_med'])} / {extra.fmt(h['m2_p90'])}{'' if h['spec_1'] else ' ✗'}"),
    ('M2b', lambda h: f"{extra.fmt(h['m2b_med'])} / {extra.fmt(h['m2b_p90'])}"),
    ('M3 P90 / 最大（达标段）', lambda h: f"{extra.fmt(h['m3_p90'])} / {extra.fmt(h['m3_max'])}（{h['m3_pass']} / {h['m3_total']}）"),
    ('M4 达标首 / \\|δ4\\| 中位 / P90', lambda h: f"{h['m4_pass']} / {extra.fmt(h['m4_med'])} / {extra.fmt(h['m4_p90'])}"),
    ('n ≤ 4', lambda h: f"{h['m4_settle4']}"),
    ('跟踪误差 > 1 的首', lambda h: f"{h['m4_track_fail']}"),
    ('c 到 0.8 中位 / P90 s', lambda h: f"{extra.fmt(h['c08_med'], 1)} / {extra.fmt(h['c08_p90'], 1)}{'（' + str(h['c08_never']) + ' 首到不了）' if h['c08_never'] else ''}"),
    ('c 掉了的首 / κ 比最小', lambda h: f"{h['dip_tracks']} / {extra.fmt(h['dip_kappa_ratio_min'])}"),
    ('intro / drop / bd m≥0.5 首判 %', lambda h: f"{extra.fmt(h['intro_first'], 0)} / {extra.fmt(h['drop_first'], 0)} / {extra.fmt(h['breakdown_first'], 0)}"),
    ('换歌 / 衰减 首', lambda h: f"{h['new_program_tracks']} / {h['decay_tracks']}"),
    ('合成两首 M4 达标 / M2 中位', lambda h: f"{h.get('syn_m4_pass', '—')} / {extra.fmt(h.get('syn_m2_med', math.nan))}"),
]


def write_outputs(out, rows):
    keys = []
    for r in rows:
        keys.extend(k for k in r if k not in keys)
    with open(os.path.join(out, 'scan.csv'), 'w', encoding='utf-8', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        for r in rows:
            writer.writerow({k: (extra.fmt(v, 4) if isinstance(v, float) else v) for k, v in r.items()})
    lines = ['# P1R 常数扫描（HEAD 的 celadon_render，--live-constants；Jamendo 38 首可算 + 合成两首）', '',
             '门槛：第 1 条 M2 中位 ≤ 0.3、P90 ≤ 1.0；第 2 条每段 breakdown ≤ 0.5；第 3 条每首 |δ4| ≤ 1（✗ = 第 1 条不达标）。', '',
             '| ' + ' | '.join(c for c, _ in MD_COLUMNS) + ' |', '|' + '---|' * len(MD_COLUMNS)]
    lines += ['| ' + ' | '.join(str(f(r)) for _, f in MD_COLUMNS) + ' |' for r in rows]
    with open(os.path.join(out, 'scan.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def main(argv):
    parser = argparse.ArgumentParser(description='P1R 常数扫描')
    parser.add_argument('--configs', required=True)
    parser.add_argument('--render', required=True)
    parser.add_argument('--calibrate', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--work', default=os.path.expanduser('~/.cache/celadon-t048-cal'))
    parser.add_argument('--syn', default=os.path.expanduser('~/.cache/celadon-t048-cal-syn'))
    parser.add_argument('--labels', default=os.path.expanduser('~/.cache/celadon-t048-cal/p1r/labels-override.tsv'))
    parser.add_argument('--auto-labels', default=os.path.expanduser('~/.cache/celadon-t048-cal/p1r/labels-auto.tsv'))
    parser.add_argument('-j', '--jobs', type=int, default=8)
    parser.add_argument('--only')
    parser.add_argument('--redo', action='store_true')
    args = parser.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    log = os.path.join(args.out, 'scan.log')
    with open(args.configs, encoding='utf-8') as f:
        configs = [r for r in csv.DictReader((line for line in f if not line.startswith('#')), delimiter='\t')]
    only = set(args.only.split(',')) if args.only else None
    rows = []
    for c in configs:
        name, genre, constants = c['name'], c['genre'], (c.get('constants') or '').strip()
        start = time.time()
        tag, code = ensure_tag(args, args.work, ['--labels', args.labels, '--auto-labels', args.auto_labels], name, genre, constants, log) \
            if only is None or name in only else ('p1r-s-' + name, 0)
        if code != 0:
            print(f'{name}：失败（退出码 {code}，见 {log}）', flush=True)
            continue
        if not os.path.isfile(os.path.join(args.work, 'live', tag, 'tracks.csv')):
            continue
        h = headline(args.work, tag)
        syn_tag, syn_code = ensure_tag(args, args.syn, ['--auto-labels', os.path.join(args.syn, 'labels-auto.tsv')], name, genre, constants, log) \
            if only is None or name in only else (tag, 0)
        if syn_code == 0 and os.path.isfile(os.path.join(args.syn, 'live', syn_tag, 'tracks.csv')):
            s = headline(args.syn, syn_tag)
            h.update({'syn_m4_pass': s['m4_pass'], 'syn_m2_med': s['m2_med'], 'syn_m3_max': s['m3_max'], 'syn_c08_med': s['c08_med']})
        h.update({'name': name, 'genre': genre, 'constants': constants})
        rows.append(h)
        write_outputs(args.out, rows)
        print(f"{time.strftime('%T')} {name}（{time.time() - start:.0f} s）：M2 {extra.fmt(h['m2_med'])} / {extra.fmt(h['m2_p90'])}，"
              f"M3 {h['m3_pass']} / {h['m3_total']}（最大 {extra.fmt(h['m3_max'])}），M4 {h['m4_pass']} / {h['scored']}（跟踪误差 > 1：{h['m4_track_fail']}），"
              f"c 到 0.8 中位 {extra.fmt(h['c08_med'], 1)} s，c 掉了 {h['dip_tracks']} 首", flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
