#!/usr/bin/env python3
# 预热起点检查（「从第 1 小节播 vs 从 drop 播」）。T-038 时是 House 门槛 ①（tools/render/house_gate.py）；T-108（ADR-039）删掉曲风与交付、
# 撤掉 House 门禁后，只把这一条按通用档留下（Max 2026-11-13 Grill Q4：「起播第 33 小节检查改按通用档」）。
#
# 读 celadon_render 的 metrics.csv（要带 T-038 追加列：跑的时候给 --bpm 与 --listen-from-bar 1,<N>），按（歌, GLAZE, 修坯, 风格）分组，
# 每组拿 listen_from_bar = 1 的一行与每个 > 1 的行比：第一遍（预热）结束时的方案增益 plan_gain_db 相差 ≤ 0.5 dB。
#   - 行里有非空 confidence（T-096a 起 celadon_render 填成品末尾的 c）：两边都要 c ≥ 0.5，否则判失败（主段还没热）；
#   - confidence 列缺或空（T-096a 之前的旧表）：只比方案增益，结论里注明「没核对 c」。
#   阶段 1 的「两边都已锁定 + 方案增益加校准偏差」（评审 F2）随 ADR-030 没有了锁定与校准，这里不再实现。
# 重放行（clay = replay-off）、GLAZE 0 的组不判（GLAZE 0 不做方案）。
#
# 用法：listen_start_check.py <metrics.csv> [<metrics.csv> …]；多份表（按聆听起点分开跑的）一起给，按上面的键合并。
# 返回 0 = 没有失败（缺对照的组只列说明）；1 = 有失败；2 = 用法 / 输入错误。只用标准库。
import csv
import math
import sys
from collections import defaultdict

PLAN_GAIN_TOLERANCE_DB = 0.5
CONFIDENCE_READY = 0.5          # 同 gate_check.py
REQUIRED_COLUMNS = ('kind', 'song', 'glaze', 'clay', 'style', 'listen_from_bar', 'plan_gain_db')


def num(value, default=math.nan):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def confidence_of(row):
    """非空 confidence 才算数；缺列或空串返回 None。"""
    raw = (row.get('confidence') or '').strip()
    return None if raw == '' else num(raw)


def compare(row, partner):
    """一对（从第 1 小节、从第 N 小节）的结论：(verdict, note)。"""
    a, b = num(row.get('plan_gain_db')), num(partner.get('plan_gain_db'))
    bar = partner['listen_from_bar']
    if not (math.isfinite(a) and math.isfinite(b)):
        return 'FAIL', f"从第 {bar} 小节：有一边第一遍结束时没有方案（plan_gain_db 空）"
    row_c, partner_c = confidence_of(row), confidence_of(partner)
    delta = b - a
    if row_c is None and partner_c is None:
        ready, ready_text = True, '（confidence 列空，没核对 c）'
    else:
        ready = row_c is not None and row_c >= CONFIDENCE_READY and partner_c is not None and partner_c >= CONFIDENCE_READY
        ready_text = '' if ready else f"（主段还没热：c {row.get('confidence')} / {partner.get('confidence')}）"
    ok = ready and abs(delta) <= PLAN_GAIN_TOLERANCE_DB + 1e-9
    return ('PASS' if ok else 'FAIL'), f"方案增益从第 1 小节 {a:+.2f} dB、从第 {bar} 小节 {b:+.2f} dB，差 {delta:+.2f} dB{ready_text}"


def load(paths):
    rows = []
    for path in paths:
        with open(path, encoding='utf-8') as handle:
            reader = csv.DictReader(handle)
            missing = [c for c in REQUIRED_COLUMNS if c not in (reader.fieldnames or [])]
            if missing:
                raise ValueError(f"{path}：缺列 {', '.join(missing)}（celadon_render 要加 --bpm 与 --listen-from-bar 才有 T-038 追加列）")
            rows.extend(r for r in reader if r['kind'] == 'render')
    return rows


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__ or '用法：listen_start_check.py <metrics.csv> [<metrics.csv> …]', file=sys.stderr)
        return 2
    try:
        rows = load(argv[1:])
    except (OSError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 2

    groups = defaultdict(list)
    for r in rows:
        if r['clay'] == 'replay-off' or num(r['glaze'], 0.0) <= 0.0:
            continue
        groups[(r['song'], r['glaze'], r['clay'], r['style'])].append(r)

    failures, notes = 0, []
    for key in sorted(groups, key=lambda k: (k[0], num(k[1]), k[2], k[3])):
        song, glaze, clay, style = key
        members = sorted(groups[key], key=lambda r: num(r['listen_from_bar'], 1.0))
        heads = [r for r in members if r['listen_from_bar'] == '1']
        partners = [r for r in members if r['listen_from_bar'] != '1']
        label = f"{song} g{glaze} clay-{clay} {style}"
        if not heads or not partners:
            notes.append(f"{label}：缺{'从第 1 小节' if not heads else '从 drop'}起播的一组，没判")
            continue
        for partner in partners:
            verdict, note = compare(heads[0], partner)
            failures += verdict == 'FAIL'
            print(f"{verdict}  {label}：{note}")

    for note in notes:
        print(f"说明  {note}")
    print(f"\n{'有 ' + str(failures) + ' 项失败' if failures else '全部通过'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
