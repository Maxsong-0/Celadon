#!/usr/bin/env python3
# T-038 House 门槛检查（设计规格-曲风与交付 第 7 节第 4 条；学习方式见设计规格-曲风-House学习方式）。
# 读 celadon_render 的输出（metrics.csv 末尾的 T-038 追加列 + --segments-csv 的 segments/）与段落标注，逐首、逐组设置判五条：
#   ① 从第 1 小节播与从 drop 播，锁定后的方案增益相差 ≤ 0.5 dB（同一组设置要有 listen_from_bar = 1 与 > 1 两行：
#      celadon_render 加 --listen-from-bar 1,<drop 起始小节>；比第一遍结束时已提交方案的 plan_gain_db，两行都得「已锁定」）；
#   ② drop 段限幅 GR 不超风格预算：drop 各小节的块 GR（块内最大，比插件保护层按 10 ms 时隙的均值偏严）——均值 ≤ limitGrMeanCapDb、
#      高于 limitGrP95CapDb 的块 ≤ 5 %（即 P95 ≤ 上限）、最大 ≤ limitGrPeakCapDb；预算取成品末尾保护层生效的上限（budget_* 列）；
#   ③ breakdown → drop 的响度落差：处理后比处理前最多少 1 LU（段落响度 = 段内各小节有效子块的 K 加权能量平均，输入 / 成品各一份）；
#   ④ 真峰值不超过交付档上限：成品真峰值 ≤ 交给限幅器的有效 Ceiling + 0.01 dB；流媒体档另查有效 Ceiling ≤ −2.0 dBTP；
#   ⑤ drop 段响度落在目标 ±1 LU（只有流媒体 / 俱乐部）：目标 = clamp(基准 + 风格偏移 − LRA 修正, 档内范围)（规格第 4.1 节），
#      GLAZE < 100 时按决策器同一关系折算：目标_g = drop 输入响度 + GLAZE × (目标 − drop 输入响度)；每个 drop 段分别判。
# 另打印「门控对照」：用输入端逐小节的段响度与低频占比按规格第 2.3 节离线门控一遍（整首都听过），前奏被挡住、drop 通过的比例——
# 门控阈值标定（规格第 6 节）用；不判定。
#
# 用法：house_gate.py <输出目录> [<输出目录> …] --annotations <标注目录或 .json 文件> [--gate-width 3] [--low-factor 0.6]
#   多次运行（例如按歌、按聆听起点分开跑）的输出目录一起给，按 (歌, GLAZE, 修坯, 风格, 曲风, 交付, 聆听起点) 合并。
#   标注 JSON 见 tools/render/make_synthetic_house.py：{"song", "bpm", "sections": [{"name", "kind", "start_bar", "end_bar"}]}，
#   kind ∈ intro / drop / breakdown / 其他（小节从 1 起、两端都含）；按 song（= metrics.csv 的 song 列，文件名去扩展名）对上。
# 返回 0 = 没有失败（「未判」只列说明）；1 = 有失败；2 = 用法 / 输入错误。只用标准库。
import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict

# 规格常数（设计规格-曲风与交付 第 4.1 节；源码出厂值在 GlazeConstants.h DeliveryConstants 与 lraReferenceLu / lraSlopeDbPerLu）。
# 这里照规格独立写一份：门槛判的是「实现对不对规格」，不拿实现自己的数。规格改了要同步改这里。
DELIVERY = {
    # 档: (基准目标 LUFS, 范围下限, 范围上限, 真峰值上限 dBTP 或 None)
    'streaming': (-10.0, -12.0, -8.0, -2.0),
    'club': (-7.0, -9.0, -5.0, None),
}
STYLE_PREFERRED = {'warm': -8.5, 'punchy': -9.0, 'clean': -10.0}   # 风格混合前各风格的 preferredLufs
STYLE_OFFSET_REFERENCE = -9.0                                      # 风格偏移 = 混合后的 preferredLufs − 冲击的 −9
LRA_REFERENCE_LU = 6.0                                             # LRA 超过 6 LU 的部分每 LU 降 1/3 dB（在钳位之前减）
LRA_SLOPE_DB_PER_LU = 1.0 / 3.0

PLAN_GAIN_TOLERANCE_DB = 0.5       # ①
P95_OVER_FRACTION = 0.05           # ②：高于 P95 上限的块不超过 5 %
BUDGET_EPSILON_DB = 0.01           # ② ④ 的比较余量（同 gate_check.py 的真峰值容差）
BREAKDOWN_DROP_LOSS_LU = 1.0       # ③
TARGET_WINDOW_LU = 1.0             # ⑤

EXTENDED_COLUMNS = ('genre', 'delivery', 'listen_from_bar', 'plan_gain_db', 'effective_ceiling_dbtp', 'segments_file')


def num(value, default=math.nan):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def fmt(value, digits=2, signed=False):
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return '—'
    return f'{value:+.{digits}f}' if signed else f'{value:.{digits}f}'


def lufs_to_energy(lufs):
    return 10.0 ** ((lufs + 0.691) / 10.0)


def energy_to_lufs(energy):
    return -0.691 + 10.0 * math.log10(energy) if energy > 0.0 else math.nan


def weighted_percentile(pairs, fraction):
    """pairs = [(值, 权重)]：按权重的百分位（第一个累计权重 ≥ fraction × 总权重的值）。空 → nan。"""
    pairs = sorted((v, w) for v, w in pairs if math.isfinite(v) and w > 0.0)
    total = sum(w for _, w in pairs)
    if total <= 0.0:
        return math.nan
    running = 0.0
    for value, weight in pairs:
        running += weight
        if running >= fraction * total - 1e-12:
            return value
    return pairs[-1][0]


# ---------------------------------------------------------------------------------------------------------------------
# 读输入

def load_annotations(paths):
    annotations = {}
    for path in paths:
        files = [os.path.join(path, f) for f in sorted(os.listdir(path)) if f.endswith('.json')] if os.path.isdir(path) else [path]
        for file in files:
            with open(file, encoding='utf-8') as f:
                data = json.load(f)
            if 'song' not in data or 'sections' not in data:
                continue
            for s in data['sections']:
                s.setdefault('kind', s.get('name', '').rstrip('0123456789 ').lower())
                s['start_bar'], s['end_bar'] = int(s['start_bar']), int(s['end_bar'])
            data['_file'] = file
            annotations[data['song']] = data
    return annotations


def load_segments(directory, relative):
    """segments/<名>.csv → {段号: 行}（段号从 1 起，按小节分时 = 小节号）。读不到返回 None。"""
    if not relative:
        return None
    path = os.path.join(directory, relative)
    if not os.path.isfile(path):
        return None
    with open(path, encoding='utf-8') as f:
        return {int(r['segment']): r for r in csv.DictReader(f)}


def load_outputs(directories):
    """各输出目录的 metrics.csv 合并：输入行按歌（只留第一份），成品行按组合键。"""
    inputs, renders = {}, []
    for directory in directories:
        path = os.path.join(directory, 'metrics.csv')
        if not os.path.isfile(path):
            raise SystemExit(f'house_gate：{directory} 里没有 metrics.csv')
        with open(path, encoding='utf-8') as f:
            rows = list(csv.DictReader(f))
        if rows and any(column not in rows[0] for column in EXTENDED_COLUMNS):
            raise SystemExit(f'house_gate：{path} 没有 T-038 追加列——要用 --genre / --delivery / --bpm / --segments-csv 这类新选项跑出的输出')
        for r in rows:
            r['_dir'] = directory
            if r['kind'] == 'input':
                inputs.setdefault(r['song'], r)
            elif r['kind'] == 'render' and r['clay'] != 'replay-off':
                renders.append(r)
    return inputs, renders


# ---------------------------------------------------------------------------------------------------------------------
# 段落统计

def bars_of(section):
    return range(section['start_bar'], section['end_bar'] + 1)


def section_loudness(segments, section):
    """段落响度：段内各小节有效子块的 K 加权能量平均（按 valid_s 加权）。没有有效子块 → nan。"""
    energy = seconds = 0.0
    for bar in bars_of(section):
        r = segments.get(bar)
        if r is None:
            continue
        lufs, valid = num(r['lufs']), num(r['valid_s'], 0.0)
        if math.isfinite(lufs) and valid > 0.0:
            energy += lufs_to_energy(lufs) * valid
            seconds += valid
    return energy_to_lufs(energy / seconds) if seconds > 0.0 else math.nan


def section_reduction(segments, section):
    """drop 段的块 GR：(均值, 高于 P95 上限的块占比, 最大, 块数)。块按块长近似等长，均值按块数加权。"""
    weighted = blocks = over = 0.0
    peak = -math.inf
    for bar in bars_of(section):
        r = segments.get(bar)
        if r is None or not r.get('gr_blocks'):
            continue
        count = num(r['gr_blocks'], 0.0)
        if count <= 0.0:
            continue
        weighted += num(r['gr_mean_db'], 0.0) * count
        blocks += count
        over += num(r['gr_over_p95_cap_blocks'], 0.0)
        peak = max(peak, num(r['gr_max_db'], -math.inf))
    if blocks <= 0.0:
        return math.nan, math.nan, math.nan, 0
    return weighted / blocks, over / blocks, peak, int(blocks)


def segments_are_bars(segments):
    return bool(segments) and all(r.get('mode') == 'bars' for r in segments.values())


# ---------------------------------------------------------------------------------------------------------------------
# 五条门槛（每条返回 (结论, 说明)；结论 ∈ PASS / FAIL / N/A）

def gate_plan_gain(row, partners):
    if row['listen_from_bar'] != '1':
        return 'N/A', '这一行本身是从 drop 起播的一组（结论记在从第 1 小节播的那一行）'
    if not partners:
        return 'N/A', '缺从 drop 起播的一组：celadon_render 加 --listen-from-bar 1,<drop 起始小节> 再跑'
    verdicts, notes = [], []
    for partner in partners:
        locked = row.get('listen_phase') == 'locked' and partner.get('listen_phase') == 'locked'
        a, b = num(row['plan_gain_db']), num(partner['plan_gain_db'])
        if not (math.isfinite(a) and math.isfinite(b)):
            verdicts.append('FAIL')
            notes.append(f"从第 {partner['listen_from_bar']} 小节：有一边第一遍结束时还没有已提交方案")
            continue
        delta = b - a
        ok = locked and abs(delta) <= PLAN_GAIN_TOLERANCE_DB + 1e-9
        verdicts.append('PASS' if ok else 'FAIL')
        lock_text = '' if locked else f"（没都锁定：{row.get('listen_phase')} / {partner.get('listen_phase')}）"
        notes.append(f"从第 1 小节 {fmt(a)} dB、从第 {partner['listen_from_bar']} 小节 {fmt(b)} dB，差 {fmt(delta, 2, True)} dB{lock_text}")
    return ('FAIL' if 'FAIL' in verdicts else 'PASS'), '；'.join(notes)


def gate_budget(row, out_segments, drops):
    caps = (num(row.get('budget_limit_gr_mean_db')), num(row.get('budget_limit_gr_p95_db')), num(row.get('budget_limit_gr_peak_db')))
    if not all(math.isfinite(c) for c in caps):
        return 'N/A', '没有风格预算（成品末尾保护层没设上限：GLAZE 0 等）'
    if out_segments is None:
        return 'N/A', '缺成品的逐段统计（--segments-csv）'
    verdicts, notes = [], []
    for section in drops:
        mean, over, peak, blocks = section_reduction(out_segments, section)
        if blocks == 0:
            verdicts.append('N/A')
            notes.append(f"{section['name']}：没有块")
            continue
        ok = mean <= caps[0] + BUDGET_EPSILON_DB and over <= P95_OVER_FRACTION and peak <= caps[2] + BUDGET_EPSILON_DB
        verdicts.append('PASS' if ok else 'FAIL')
        notes.append(f"{section['name']}：均值 {fmt(mean)} / {fmt(caps[0])}、高于 P95 上限 {fmt(caps[1])} 的块 {fmt(100.0 * over, 1)} %"
                     f" / 5 %、最大 {fmt(peak)} / {fmt(caps[2])} dB")
    if 'FAIL' in verdicts:
        return 'FAIL', '；'.join(notes)
    return ('PASS' if 'PASS' in verdicts else 'N/A'), '；'.join(notes)


def gate_breakdown(in_segments, out_segments, sections):
    if out_segments is None or in_segments is None:
        return 'N/A', '缺输入或成品的逐段统计（--segments-csv）'
    pairs = []
    for i, section in enumerate(sections):
        if section['kind'] != 'breakdown':
            continue
        following = [s for s in sections[i + 1:] if s['kind'] == 'drop']
        if following:
            pairs.append((section, following[0]))
    if not pairs:
        return 'N/A', '标注里没有「breakdown 之后接 drop」'
    verdicts, notes = [], []
    for breakdown, drop in pairs:
        din = section_loudness(in_segments, drop) - section_loudness(in_segments, breakdown)
        dout = section_loudness(out_segments, drop) - section_loudness(out_segments, breakdown)
        if not (math.isfinite(din) and math.isfinite(dout)):
            verdicts.append('N/A')
            notes.append(f"{breakdown['name']} → {drop['name']}：有一段没有有效子块")
            continue
        ok = dout >= din - BREAKDOWN_DROP_LOSS_LU - 1e-9
        verdicts.append('PASS' if ok else 'FAIL')
        notes.append(f"{breakdown['name']} → {drop['name']}：处理前 {fmt(din, 2, True)} LU、处理后 {fmt(dout, 2, True)} LU（少 {fmt(din - dout)} LU）")
    if 'FAIL' in verdicts:
        return 'FAIL', '；'.join(notes)
    return ('PASS' if 'PASS' in verdicts else 'N/A'), '；'.join(notes)


def gate_true_peak(row):
    peak, ceiling = num(row.get('true_peak_dbtp')), num(row.get('effective_ceiling_dbtp'))
    if not math.isfinite(peak):
        return 'N/A', '成品没有真峰值读数'
    notes = []
    cap = ceiling if math.isfinite(ceiling) else -1.0
    if not math.isfinite(ceiling):
        notes.append('没有有效 Ceiling 列，按出厂 −1.0 dBTP')
    spec_cap = DELIVERY.get(row['delivery'], (None, None, None, None))[3]
    ok = True
    if spec_cap is not None and cap > spec_cap + 1e-6:
        ok = False
        notes.append(f'交给限幅器的有效 Ceiling {fmt(cap)} 高于{row["delivery"]}档上限 {fmt(spec_cap)} dBTP')
    if spec_cap is not None:
        cap = min(cap, spec_cap)
    ok = ok and peak <= cap + BUDGET_EPSILON_DB
    notes.insert(0, f'真峰值 {fmt(peak)} / 上限 {fmt(cap)} dBTP')
    return ('PASS' if ok else 'FAIL'), '；'.join(notes)


def delivery_target(row):
    """规格第 4.1 节的目标（GLAZE 100）：clamp(基准 + 风格偏移 − LRA 修正, 档内范围)。返回 (目标, 说明) 或 (nan, 原因)。"""
    spec = DELIVERY.get(row['delivery'])
    if spec is None:
        return math.nan, '标准档不判（规格只给了流媒体 / 俱乐部的目标）'
    weights = {k: num(row.get('weight_' + k)) for k in STYLE_PREFERRED}
    if not all(math.isfinite(w) for w in weights.values()) or sum(weights.values()) <= 0.0:
        return math.nan, '没有方案权重（第一遍结束时没有已提交方案）'
    total = sum(weights.values())
    preferred = sum(weights[k] * STYLE_PREFERRED[k] for k in STYLE_PREFERRED) / total
    offset = preferred - STYLE_OFFSET_REFERENCE
    lra = num(row.get('profile_lra_lu'))
    correction = LRA_SLOPE_DB_PER_LU * max(0.0, lra - LRA_REFERENCE_LU) if math.isfinite(lra) else 0.0
    target = min(max(spec[0] + offset - correction, spec[1]), spec[2])
    lra_text = f'LRA {fmt(lra, 1)} LU → 修正 {fmt(correction)}' if math.isfinite(lra) else 'LRA 无读数、不修正'
    return target, f'目标 {fmt(target)}（基准 {fmt(spec[0], 1)}、风格偏移 {fmt(offset, 2, True)}、{lra_text}）'


def gate_target(row, in_segments, out_segments, drops):
    target, note = delivery_target(row)
    if not math.isfinite(target):
        return 'N/A', note
    if out_segments is None or in_segments is None:
        return 'N/A', '缺输入或成品的逐段统计（--segments-csv）'
    glaze = num(row['glaze'], 0.0) / 100.0
    energy = seconds = 0.0
    for section in drops:   # drop 输入响度：所有 drop 小节合起来（House 档的画像 I 就是满编段合起来的响度）
        for bar in bars_of(section):
            r = in_segments.get(bar)
            if r and math.isfinite(num(r['lufs'])) and num(r['valid_s'], 0.0) > 0.0:
                energy += lufs_to_energy(num(r['lufs'])) * num(r['valid_s'])
                seconds += num(r['valid_s'])
    drop_in = energy_to_lufs(energy / seconds) if seconds > 0.0 else math.nan
    if not math.isfinite(drop_in):
        return 'N/A', 'drop 段输入没有有效子块'
    scaled = drop_in + glaze * (target - drop_in)
    verdicts, notes = [], [f"{note}；GLAZE {fmt(100.0 * glaze, 0)} 折算 {fmt(scaled)}（drop 输入 {fmt(drop_in)}）；方案自己的目标 {fmt(num(row.get('plan_requested_target_lufs')))}"]
    for section in drops:
        out = section_loudness(out_segments, section)
        if not math.isfinite(out):
            verdicts.append('N/A')
            continue
        ok = abs(out - scaled) <= TARGET_WINDOW_LU + 1e-9
        verdicts.append('PASS' if ok else 'FAIL')
        notes.append(f"{section['name']} {fmt(out)}（差 {fmt(out - scaled, 2, True)} LU）")
    if 'FAIL' in verdicts:
        return 'FAIL', '；'.join(notes)
    return ('PASS' if 'PASS' in verdicts else 'N/A'), '；'.join(notes)


# ---------------------------------------------------------------------------------------------------------------------
# 门控对照（输入端，不判定）

def gate_overview(song, segments, sections, width, factor):
    bars = [(bar, num(r['lufs']), num(r['valid_s'], 0.0), num(r['low_share'])) for bar, r in sorted(segments.items())]
    reference = weighted_percentile([(lufs, valid) for _, lufs, valid, _ in bars], 0.95)
    passed_loudness = [(bar, lufs, valid, share) for bar, lufs, valid, share in bars if math.isfinite(lufs) and lufs >= reference - width]
    low_reference = weighted_percentile([(share, valid) for _, _, valid, share in passed_loudness], 0.95)
    full = {bar for bar, lufs, valid, share in passed_loudness if math.isfinite(share) and share >= low_reference * factor}
    lines = [f'  {song}：S_ref {fmt(reference)} LUFS（第 95 百分位，按有效时长加权），低频参考 {fmt(low_reference, 3)}；'
             f'ΔG {fmt(width, 1)} LU、低频门系数 {fmt(factor, 2)} 下满编 {len(full)} 小节']
    for section in sections:
        in_section = [b for b in bars_of(section) if b in segments]
        if not in_section:
            continue
        passing = sum(1 for b in in_section if b in full)
        loudness = section_loudness(segments, section)
        shares = [num(segments[b]['low_share']) for b in in_section if math.isfinite(num(segments[b]['low_share']))]
        share = sum(shares) / len(shares) if shares else math.nan
        lines.append(f"    {section['name']:<12} {section['kind']:<10} 第 {section['start_bar']}～{section['end_bar']} 小节：段落响度 {fmt(loudness)} LUFS、"
                     f"低频占比均值 {fmt(share, 3)}，过门 {passing} / {len(in_section)}")
    return lines


# ---------------------------------------------------------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(description='T-038 House 门槛检查（设计规格-曲风与交付 第 7 节第 4 条）')
    parser.add_argument('outputs', nargs='+', help='celadon_render 的输出目录（含 metrics.csv 与 segments/）')
    parser.add_argument('--annotations', nargs='+', required=True, help='段落标注：目录（读其中 *.json）或 .json 文件')
    parser.add_argument('--gate-width', type=float, default=3.0, help='门控对照的 ΔG（LU，出厂 3）')
    parser.add_argument('--low-factor', type=float, default=0.6, help='门控对照的低频门系数（出厂 0.6）')
    args = parser.parse_args(argv)

    annotations = load_annotations(args.annotations)
    inputs, renders = load_outputs(args.outputs)
    if not renders:
        print('house_gate：指标表里没有成品行', file=sys.stderr)
        return 2

    groups = defaultdict(list)
    for r in renders:
        groups[(r['song'], r['glaze'], r['clay'], r['style'], r['genre'], r['delivery'])].append(r)

    fails, notes, rows_out = 0, [], []
    no_house_gate = defaultdict(int)   # (歌, 曲风) → 画像没有 House 门控结果的行数
    names = ['①方案增益', '②GR 预算', '③落差', '④真峰值', '⑤目标响度']
    print('T-038 House 门槛（设计规格-曲风与交付 第 7 节第 4 条）')
    print(f"{'歌':<24} {'g':>4} {'曲风':<8} {'交付':<9} {'起播':>4}  " + '  '.join(f'{n:<8}' for n in names))

    for key in sorted(groups, key=lambda k: (k[0], num(k[1]), k[2], k[3], k[4], k[5])):
        song, glaze = key[0], key[1]
        annotation = annotations.get(song)
        input_row = inputs.get(song)
        in_segments = load_segments(input_row['_dir'], input_row.get('segments_file')) if input_row else None
        if annotation is None:
            notes.append(f'{song}：没有段落标注（--annotations 里找不到 song = {song} 的 .json），跳过')
            continue
        if in_segments is not None and not segments_are_bars(in_segments):
            notes.append(f'{song}：逐段统计不是按小节分的（跑渲染时要给 --bpm），段落标注对不上，跳过')
            continue
        sections = sorted(annotation['sections'], key=lambda s: s['start_bar'])
        drops = [s for s in sections if s['kind'] == 'drop']

        rows = sorted(groups[key], key=lambda r: int(num(r['listen_from_bar'], 1)))
        partners = [r for r in rows if r['listen_from_bar'] != '1']
        drop_starts = {s['start_bar'] for s in drops}
        for partner in partners:
            if int(num(partner['listen_from_bar'], 0)) not in drop_starts:
                notes.append(f"{song} g{glaze} {key[4]} {key[5]}：聆听起点第 {partner['listen_from_bar']} 小节不是标注里任何一个 drop 的起点")

        for row in rows:
            out_segments = load_segments(row['_dir'], row.get('segments_file'))
            if num(glaze, 0.0) <= 0.0:
                results = [('N/A', 'GLAZE 0 没有方案')] * 5
            else:
                results = [
                    gate_plan_gain(row, partners if row['listen_from_bar'] == '1' else []),
                    gate_budget(row, out_segments, drops),
                    gate_breakdown(in_segments, out_segments, sections),
                    gate_true_peak(row),
                    gate_target(row, in_segments, out_segments, drops),
                ]
            fails += sum(1 for verdict, _ in results if verdict == 'FAIL')
            print(f"{song[:24]:<24} {glaze:>4} {row['genre']:<8} {row['delivery']:<9} {row['listen_from_bar']:>4}  "
                  + '  '.join(f'{verdict:<8}' for verdict, _ in results))
            rows_out.append((row, results))

            if row['genre'] != 'general' and row.get('house_gate_valid') != '1':
                no_house_gate[(song, row['genre'])] += 1

    for (song, genre), count in sorted(no_house_gate.items()):
        notes.append(f'{song} · {genre}：{count} 行的画像没有 House 门控结果（house_gate_valid ≠ 1）——House 学习方式没有生效，'
                     '这些行的数字不代表 House 行为')

    print('\n明细：')
    for row, results in rows_out:
        weights = '/'.join(fmt(num(row.get('weight_' + k)), 2) for k in ('warm', 'punchy', 'clean'))
        print(f"  {row['song']} · g{row['glaze']} · {row['clay']} · {row['style']} · {row['genre']} · {row['delivery']} · 起播第 {row['listen_from_bar']} 小节"
              f"（第一遍末 {row.get('listen_phase')}，成品末 {row.get('glaze_phase')}，方案权重 温润 / 冲击 / 通透 {weights}）")
        for name, (verdict, detail) in zip(names, results):
            print(f'    {name} {verdict}：{detail}')

    print(f'\n门控对照（输入端，整首都听过时按规格第 2.3 节离线门控一遍；ΔG {fmt(args.gate_width, 1)} LU、系数 {fmt(args.low_factor, 2)}；不判定）：')
    for song in sorted(inputs):
        annotation = annotations.get(song)
        in_segments = load_segments(inputs[song]['_dir'], inputs[song].get('segments_file'))
        if annotation is None or in_segments is None or not segments_are_bars(in_segments):
            continue
        sections = sorted(annotation['sections'], key=lambda s: s['start_bar'])
        for line in gate_overview(song, in_segments, sections, args.gate_width, args.low_factor):
            print(line)

    if notes:
        print('\n说明：')
        for note in notes:
            print(f'  - {note}')

    print(f'\n{len(rows_out)} 行，{fails} 项失败')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
