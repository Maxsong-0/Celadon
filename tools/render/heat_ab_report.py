#!/usr/bin/env python3
"""T-096d：汇总多次 `celadon_render --replay-override` 运行的 heat_ab.csv，按阈值给每格「2a / ABX」结论、列出不可信的行。

只用标准库（门槛与统计脚本的约定，T-096 计划 §13 Q3）。工具只读、只报告：不改任何输入文件；--md / --csv / --rows 只写给定的新文件。

用法：
  heat_ab_report.py [--md 报告.md] [--csv 格子.csv] [--rows 逐行.csv] 路径 [路径 ...]
  heat_ab_report.py --selftest

路径可以是 heat_ab.csv 文件，也可以是目录（目录下递归找 heat_ab.csv）。每份 CSV 的「语料标签」默认取它所在目录的上一级目录名
（run_heat_ab.sh 的布局是 runs/<标签>/<曲目>/heat_ab.csv：给 runs、runs/<标签>、或其中单个文件，标签都是 <标签>）；CSV 就在给的目录
本身里（一次 celadon_render 的输出目录）时取这个目录名。别的布局写成「标签=路径」指定标签。

判定（T-096 计划 §13 Q8，Max 2026-10-11「全按推荐」）：一格 = 语料标签 × 交付 × GLAZE × 风格 × 聆听起点（曲风不分格：House 组每首用
各自的子类）。只用可信行判——trust = ok、两层参数核对都是 true、两项 K 加权读数都有：这一格里最坏那首的差信号 K 加权响度（整首）比
母带低 ≥ 50 LU，且这一格里最坏的 3 s 窗（各首最坏 3 s 窗里最响的那个，可以不是同一首）也低 ≥ 50 LU → 2a（直接实施，不做 ABX）；
否则 → ABX（ADR-034 决定 4：40 次分 3 个会话，通过线 26/40）。
所有格子都是 2a → 总结论「直接走 2a」；有一格 ABX → 「交 ABX」；有格子一行可信数据都没有 → 「数据不全」（先补跑）。
不可信（untrusted）、无自动 Heat（noop）、算不出（K 加权读数留空，原因在 na_reason）的行分别计数列出，不进判定。

退出码：0 = 报告出来了（不论结论）；1 = 输入有问题（文件读不了、缺列、数不是数、同一行出现两次）；2 = 命令行参数错。
"""

import argparse
import csv
import io
import math
import os
import sys

# ---- 阈值与列（改口径只改这里）----
MARGIN_LU = 50.0                    # 差信号 K 加权比母带至少低这么多才算「听不见」（Q8 起点值，Max 认作完成标准）
K_COLUMN = 'diff_k_re_master_lu'    # 整首（母带门控）
WINDOW_COLUMN = 'diff_k_worst3s_lu' # 最坏 3 s 窗
REQUIRED_COLUMNS = (
    'song', 'genre', 'delivery', 'glaze', 'style', 'listen_from_bar', 'override', 'trust', 'replay_check',
    'fed_params_equal', 'chain_params_equal', 'first_mismatch', 'heat_mean', 'heat_max', 'heat_zero_pct',
    K_COLUMN, WINDOW_COLUMN, 'worst3s_start_s', 'diff_rms_re_master_db', 'diff_above_17k8_re_master_db', 'na_reason',
)
DELIVERY_ORDER = {'standard': 0, 'streaming': 1, 'club': 2}
CSV_NAME = 'heat_ab.csv'

VERDICT_2A = '2a'
VERDICT_ABX = 'ABX'
VERDICT_EMPTY = '无可信数据'


class InputError(Exception):
    """输入有问题（退出码 1）。"""


# ---------------------------------------------------------------------------------------------------------------------
# 读

def parse_number(text, where):
    """空串 → None（素材原因算不出）；'-inf' / 'inf' 照收（差信号恰好为 0 时 celadon_render 写 -inf）；别的不是数 → InputError。"""
    text = (text or '').strip()
    if text == '':
        return None
    try:
        value = float(text)
    except ValueError:
        raise InputError(f'{where}：「{text}」不是数')
    if math.isnan(value):
        raise InputError(f'{where}：不认 NaN')
    return value


def read_csv_text(text, label, source):
    """一份 heat_ab.csv 的内容 → 行（dict，多带 _label / _file 两键）。缺列 → InputError。"""
    reader = csv.DictReader(io.StringIO(text))
    missing = [c for c in REQUIRED_COLUMNS if c not in (reader.fieldnames or [])]
    if missing:
        raise InputError(f'{source} 缺列：{"、".join(missing)}（不是 celadon_render --replay-override 写的 heat_ab.csv？）')
    rows = []
    for number, row in enumerate(reader, start=2):
        row['_label'] = label
        row['_file'] = source
        row['_line'] = number
        for column in (K_COLUMN, WINDOW_COLUMN, 'worst3s_start_s', 'diff_rms_re_master_db', 'diff_above_17k8_re_master_db',
                       'heat_mean', 'heat_max', 'heat_zero_pct', 'glaze'):
            row['_' + column] = parse_number(row.get(column), f'{source} 第 {number} 行 {column}')
        rows.append(row)
    return rows


def default_label(root, directory):
    """heat_ab.csv 在 directory 里、递归起点是 root 时的默认语料标签：所在目录的上一级目录名；CSV 就在 root 里时取 root 的名字
    （给 runs/<标签> 时，各曲目子目录的上一级就是它自己）。"""
    return os.path.basename(root if directory == root else os.path.dirname(directory))


def find_csvs(spec):
    """一个命令行路径 → [(标签, 文件路径)]。"""
    label, path = None, spec
    if '=' in spec and not os.path.exists(spec):
        label, path = spec.split('=', 1)
    if os.path.isfile(path):
        # 单个文件：runs/<标签>/<曲目>/heat_ab.csv → <标签>（所在目录的上一级）。
        return [(label or os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(path)))), path)]
    if not os.path.isdir(path):
        raise InputError(f'读不到 {path}（不是文件也不是目录）')
    found = []
    root = os.path.abspath(path)
    for directory, subdirs, files in os.walk(root):
        subdirs.sort()
        if CSV_NAME in files:
            found.append((label or default_label(root, directory), os.path.join(directory, CSV_NAME)))
    if not found:
        raise InputError(f'{path} 下没有 {CSV_NAME}')
    return sorted(found)


def load(specs):
    rows = []
    for spec in specs:
        for label, path in find_csvs(spec):
            try:
                with open(path, encoding='utf-8', newline='') as f:
                    text = f.read()
            except OSError as error:
                raise InputError(f'读不了 {path}：{error}')
            rows.extend(read_csv_text(text, label, path))
    check_duplicates(rows)
    return rows


def row_key(row):
    return (row['_label'], row['song'], row['genre'], row['delivery'], row['glaze'], row['style'], row['listen_from_bar'])


def check_duplicates(rows):
    seen = {}
    for row in rows:
        key = row_key(row)
        if key in seen:
            raise InputError(f'同一行出现两次：{" · ".join(key)}（{seen[key]} 与 {row["_file"]} 第 {row["_line"]} 行）——'
                             f'同一组条件跑了两遍？只给其中一次的目录')
        seen[key] = f'{row["_file"]} 第 {row["_line"]} 行'


# ---------------------------------------------------------------------------------------------------------------------
# 判

def classify(row):
    """一行属于哪一类：judged（进判定）/ untrusted / noop / na，以及原因。"""
    asserted = row['fed_params_equal'] == 'true' and row['chain_params_equal'] == 'true'
    if row['trust'] == 'untrusted' or (row['trust'] == 'ok' and not asserted):
        reason = row['na_reason'] or ('参数核对没过：' + row['first_mismatch'] if not asserted else '')
        return 'untrusted', reason
    if row['trust'] == 'noop':
        return 'noop', row['na_reason']
    if row['trust'] != 'ok':
        raise InputError(f'{row["_file"]} 第 {row["_line"]} 行：trust 只认 ok / untrusted / noop，不认「{row["trust"]}」')
    if row['_' + K_COLUMN] is None or row['_' + WINDOW_COLUMN] is None:
        return 'na', row['na_reason'] or 'K 加权读数留空'
    return 'judged', ''


def passes(value):
    return value <= -MARGIN_LU


def cell_key(row):
    return (row['_label'], row['delivery'], row['glaze'], row['style'], row['listen_from_bar'])


def cell_sort_key(key):
    label, delivery, glaze, style, listen = key
    return (label, DELIVERY_ORDER.get(delivery, 99), delivery, float(glaze), style, int(listen or 1))


def summarize(rows):
    """按格汇总：每格的计数、最坏 K 加权与最坏 3 s 窗（及哪一首）、结论。"""
    cells = {}
    for row in rows:
        kind, reason = classify(row)
        row['_kind'], row['_reason'] = kind, reason
        cell = cells.setdefault(cell_key(row), {'rows': [], 'judged': [], 'untrusted': 0, 'noop': 0, 'na': 0})
        cell['rows'].append(row)
        if kind == 'judged':
            cell['judged'].append(row)
        else:
            cell[kind] += 1
    for cell in cells.values():
        judged = cell['judged']
        if judged:
            worst_k = max(judged, key=lambda r: (r['_' + K_COLUMN], r['song']))   # 打平按曲名取：与读入顺序无关
            worst_window = max(judged, key=lambda r: (r['_' + WINDOW_COLUMN], r['song']))
            cell['worst_k'] = (worst_k['_' + K_COLUMN], worst_k['song'])
            cell['worst_window'] = (worst_window['_' + WINDOW_COLUMN], worst_window['song'], worst_window['_worst3s_start_s'])
            cell['verdict'] = VERDICT_2A if passes(cell['worst_k'][0]) and passes(cell['worst_window'][0]) else VERDICT_ABX
        else:
            cell['worst_k'] = cell['worst_window'] = None
            cell['verdict'] = VERDICT_EMPTY
        heats = [r['_heat_max'] for r in cell['rows'] if r['_heat_max'] is not None]
        cell['heat_max'] = max(heats) if heats else None
    return cells


def overall(cells):
    verdicts = [cell['verdict'] for cell in cells.values()]
    if not verdicts:
        return '没有数据'
    if VERDICT_ABX in verdicts:
        return '交 ABX（有格子不过线；按 ADR-034 决定 4 做 40 次 ABX）'
    if VERDICT_EMPTY in verdicts:
        return '数据不全（有格子一行可信数据都没有，先补跑再下结论）'
    return '直接走 2a（所有格子最坏首、最坏 3 s 窗都比母带低 ≥ %g LU）' % MARGIN_LU


# ---------------------------------------------------------------------------------------------------------------------
# 写

def assertion_failures(rows):
    """两列参数核对不全是 true 的行（「除覆盖的键外逐位相同」的断言没过）。"""
    return [r for r in rows if not (r['fed_params_equal'] == 'true' and r['chain_params_equal'] == 'true')]


def assertion_summary(rows):
    failures = assertion_failures(rows)
    if not failures:
        return f'{len(rows)} 行全是 true'
    return f'**有 {len(failures)} 行不是 true**（{"；".join(r["song"] + " · " + r["delivery"] + " · g" + r["glaze"] for r in failures[:5])}' \
           f'{" 等" if len(failures) > 5 else ""}；celadon_render 已把它们计入失败、退出码非 0）'


def lu(value):
    return '—' if value is None else ('-inf' if value == -math.inf else f'{value:.1f}')


def markdown(cells, rows):
    counts = {kind: sum(1 for r in rows if r['_kind'] == kind) for kind in ('judged', 'untrusted', 'noop', 'na')}
    overrides = sorted({r['override'] for r in rows})
    out = ['# Heat 底量 A/B 对照汇总（T-096d）', '']
    out.append(f'- 覆盖：{"、".join(overrides)}；{len(rows)} 行，来自 {len({r["_file"] for r in rows})} 份 heat_ab.csv')
    out.append(f'- 阈值：差信号 K 加权比母带低 ≥ {MARGIN_LU:g} LU（每格最坏那首），且最坏 3 s 窗也低 ≥ {MARGIN_LU:g} LU → 2a；否则 ABX')
    out.append(f'- 行：进判定 {counts["judged"]}、不可信 {counts["untrusted"]}、无自动 Heat {counts["noop"]}、算不出 {counts["na"]}')
    out.append(f'- 参数核对（fed_params_equal / chain_params_equal）：{assertion_summary(rows)}')
    out.append(f'- **总结论：{overall(cells)}**')
    out.append('')
    out.append('| 语料 | 交付 | GLAZE | 风格 | 起播小节 | 行 | 判定 | 不可信 | 无自动 Heat | 算不出 | 最坏 K 加权 LU（哪首） | 最坏 3 s 窗 LU（哪首 @ 起点 s） | 最大 Heat | 结论 |')
    out.append('|---|---|---:|---|---:|---:|---:|---:|---:|---:|---|---|---:|---|')
    for key in sorted(cells, key=cell_sort_key):
        cell = cells[key]
        label, delivery, glaze, style, listen = key
        worst_k = f'{lu(cell["worst_k"][0])}（{cell["worst_k"][1]}）' if cell['worst_k'] else '—'
        if cell['worst_window']:
            start = cell['worst_window'][2]
            worst_window = f'{lu(cell["worst_window"][0])}（{cell["worst_window"][1]} @ {"—" if start is None else f"{start:.1f}"}）'
        else:
            worst_window = '—'
        heat = '—' if cell['heat_max'] is None else f'{cell["heat_max"]:.2f}'
        out.append(f'| {label} | {delivery} | {glaze} | {style} | {listen} | {len(cell["rows"])} | {len(cell["judged"])} | {cell["untrusted"]} '
                   f'| {cell["noop"]} | {cell["na"]} | {worst_k} | {worst_window} | {heat} | {cell["verdict"]} |')
    out.append('')
    excluded = [r for r in rows if r['_kind'] != 'judged']
    if excluded:
        out.append('## 不进判定的行')
        out.append('')
        out.append('| 语料 | 曲目 | 交付 | GLAZE | 类别 | 阳性对照 | 原因 |')
        out.append('|---|---|---|---:|---|---|---|')
        names = {'untrusted': '不可信', 'noop': '无自动 Heat', 'na': '算不出'}
        for r in sorted(excluded, key=lambda r: (cell_sort_key(cell_key(r)), r['song'])):
            reason = (r['_reason'] or '').replace('|', '\\|')
            out.append(f'| {r["_label"]} | {r["song"]} | {r["delivery"]} | {r["glaze"]} | {names[r["_kind"]]} | {r["replay_check"]} | {reason} |')
        out.append('')
    out.append('## 逐首（进判定的行）')
    out.append('')
    out.append('| 语料 | 曲目 | 曲风 | 交付 | GLAZE | K 加权 LU | 最坏 3 s 窗 LU | 起点 s | RMS dB | 17.8 kHz 以上 dB | Heat 均值 / 最大 / 为 0 % |')
    out.append('|---|---|---|---|---:|---:|---:|---:|---:|---:|---|')
    for r in sorted((r for r in rows if r['_kind'] == 'judged'), key=lambda r: (cell_sort_key(cell_key(r)), r['song'])):
        start = r['_worst3s_start_s']
        out.append(f'| {r["_label"]} | {r["song"]} | {r["genre"]} | {r["delivery"]} | {r["glaze"]} | {lu(r["_" + K_COLUMN])} '
                   f'| {lu(r["_" + WINDOW_COLUMN])} | {"—" if start is None else f"{start:.1f}"} | {lu(r["_diff_rms_re_master_db"])} '
                   f'| {lu(r["_diff_above_17k8_re_master_db"])} | {r["heat_mean"]} / {r["heat_max"]} / {r["heat_zero_pct"]} |')
    out.append('')
    return '\n'.join(out)


def cells_csv(cells):
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator='\n')
    writer.writerow(['label', 'delivery', 'glaze', 'style', 'listen_from_bar', 'rows', 'judged', 'untrusted', 'noop', 'na',
                     'worst_k_lu', 'worst_k_song', 'worst_window_lu', 'worst_window_song', 'worst_window_start_s', 'heat_max', 'verdict'])
    for key in sorted(cells, key=cell_sort_key):
        cell = cells[key]
        k = cell['worst_k'] or (None, '')
        w = cell['worst_window'] or (None, '', None)
        writer.writerow(list(key) + [len(cell['rows']), len(cell['judged']), cell['untrusted'], cell['noop'], cell['na'],
                                     '' if k[0] is None else repr_lu(k[0]), k[1], '' if w[0] is None else repr_lu(w[0]), w[1],
                                     '' if w[2] is None else f'{w[2]:.1f}', '' if cell['heat_max'] is None else f'{cell["heat_max"]:.3f}',
                                     cell['verdict']])
    return buffer.getvalue()


def rows_csv(rows):
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator='\n')
    writer.writerow(['label', 'song', 'genre', 'delivery', 'glaze', 'style', 'listen_from_bar', 'kind', 'reason', K_COLUMN, WINDOW_COLUMN, 'file'])
    for r in sorted(rows, key=lambda r: (cell_sort_key(cell_key(r)), r['song'])):
        writer.writerow([r['_label'], r['song'], r['genre'], r['delivery'], r['glaze'], r['style'], r['listen_from_bar'], r['_kind'], r['_reason'],
                         r[K_COLUMN], r[WINDOW_COLUMN], r['_file']])
    return buffer.getvalue()


def repr_lu(value):
    return '-inf' if value == -math.inf else f'{value:.3f}'


def write_new(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


# ---------------------------------------------------------------------------------------------------------------------
# 自检（合成行，已知答案；不读写文件）

def synthetic_csv(rows):
    header = list(REQUIRED_COLUMNS)
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=header, lineterminator='\n')
    writer.writeheader()
    for values in rows:
        row = {c: '' for c in header}
        row.update({'genre': 'general', 'delivery': 'standard', 'glaze': '35', 'style': 'auto', 'listen_from_bar': '1',
                    'override': 'color.heat=0', 'trust': 'ok', 'replay_check': 'ok', 'fed_params_equal': 'true',
                    'chain_params_equal': 'true', 'heat_mean': '4.2', 'heat_max': '4.2', 'heat_zero_pct': '0.00'})
        row.update(values)
        writer.writerow(row)
    return buffer.getvalue()


def selftest():
    failed = []

    def check(ok, name, detail=''):
        print(('通过  ' if ok else '失败  ') + name + ('：' + detail if detail else ''))
        if not ok:
            failed.append(name)

    # 一格全过（含 -inf：差信号恰好为 0）→ 2a；另一格最坏 3 s 窗 −48 → ABX（整首 −60 也不行：两项都要过）；
    # 不可信行就算数很差也不进判定；noop、算不出各自计数；只有不可信行的格 → 无可信数据。
    text = synthetic_csv([
        {'song': 'a', K_COLUMN: '-62.0', WINDOW_COLUMN: '-55.5', 'worst3s_start_s': '12.0'},
        {'song': 'b', K_COLUMN: '-inf', WINDOW_COLUMN: '-inf', 'worst3s_start_s': '0.0'},
        {'song': 'c', K_COLUMN: '-10.0', WINDOW_COLUMN: '-5.0', 'trust': 'untrusted', 'replay_check': 'mismatch', 'na_reason': '不可信：阳性对照 mismatch'},
        {'song': 'd', 'trust': 'noop', 'na_reason': '无自动 Heat'},
        {'song': 'e', K_COLUMN: '-70.0', 'na_reason': '不够 3 s：没有 3 s 窗'},
        {'song': 'a', 'glaze': '100', K_COLUMN: '-60.0', WINDOW_COLUMN: '-48.0', 'worst3s_start_s': '30.0'},
        {'song': 'b', 'glaze': '100', K_COLUMN: '-75.0', WINDOW_COLUMN: '-66.0', 'worst3s_start_s': '1.0'},
        {'song': 'a', 'delivery': 'club', K_COLUMN: '-80.0', WINDOW_COLUMN: '-70.0', 'fed_params_equal': 'false', 'first_mismatch': '第 3 块 limit.gainDb'},
    ])
    rows = read_csv_text(text, 'syn', '<自检>')
    cells = summarize(rows)
    g35 = cells[('syn', 'standard', '35', 'auto', '1')]
    g100 = cells[('syn', 'standard', '100', 'auto', '1')]
    club = cells[('syn', 'club', '35', 'auto', '1')]
    check(g35['verdict'] == VERDICT_2A and g35['worst_k'] == (-62.0, 'a') and g35['worst_window'][:2] == (-55.5, 'a')
          and len(g35['judged']) == 2 and g35['untrusted'] == 1 and g35['noop'] == 1 and g35['na'] == 1,
          '全过的格 → 2a（不可信 / noop / 算不出各计 1，不进判定）', f'{g35["verdict"]}，最坏 {g35["worst_k"]} / {g35["worst_window"]}')
    check(g100['verdict'] == VERDICT_ABX and g100['worst_window'][:2] == (-48.0, 'a'),
          '最坏 3 s 窗 −48 → ABX（整首 −60 也不放过）', f'{g100["verdict"]}，最坏窗 {g100["worst_window"]}')
    check(club['verdict'] == VERDICT_EMPTY and club['untrusted'] == 1,
          '参数核对为 false 的行即使 trust = ok 也按不可信算', f'{club["verdict"]}，不可信 {club["untrusted"]}')
    check([r['song'] + r['delivery'] for r in assertion_failures(rows)] == ['aclub'] and '有 1 行不是 true' in assertion_summary(rows)
          and assertion_summary(rows[:2]) == '2 行全是 true', '参数核对不全是 true 的行单独报', assertion_summary(rows))
    check(overall(cells).startswith('交 ABX'), '有一格 ABX → 总结论交 ABX', overall(cells))
    check(overall({k: v for k, v in cells.items() if k[2] == '35' and k[1] == 'standard'}).startswith('直接走 2a'),
          '只剩全过的格 → 直接走 2a')
    check(passes(-50.0) and not passes(-49.999), '阈值边界：正好低 50 LU 算过')

    # 输入有问题的几种：缺列、数不是数、同一行两次、trust 不认识。
    def raises(function):
        try:
            function()
        except InputError:
            return True
        return False

    check(raises(lambda: read_csv_text('song,trust\nx,ok\n', 'syn', '<缺列>')), '缺列 → 输入错误')
    check(raises(lambda: read_csv_text(synthetic_csv([{'song': 'x', K_COLUMN: 'abc'}]), 'syn', '<坏数>')), '数不是数 → 输入错误')
    check(raises(lambda: check_duplicates(read_csv_text(synthetic_csv([{'song': 'x'}, {'song': 'x'}]), 'syn', '<重复>'))), '同一行两次 → 输入错误')
    check(raises(lambda: summarize(read_csv_text(synthetic_csv([{'song': 'x', 'trust': 'maybe'}]), 'syn', '<坏 trust>'))), 'trust 不认识 → 输入错误')

    # 报告与输入行的顺序无关（全部按键排序；最坏值打平时按曲名取，不按读入先后），-inf 照写。
    def report_of(csv_text):
        fresh = read_csv_text(csv_text, 'syn', '<自检>')
        fresh_cells = summarize(fresh)
        return markdown(fresh_cells, fresh), cells_csv(fresh_cells)

    data_lines = text.splitlines(keepends=True)
    reversed_text = data_lines[0] + ''.join(reversed(data_lines[1:]))
    tie_text = synthetic_csv([{'song': 'y', K_COLUMN: '-inf', WINDOW_COLUMN: '-inf'}, {'song': 'x', K_COLUMN: '-inf', WINDOW_COLUMN: '-inf'}])
    tie_lines = tie_text.splitlines(keepends=True)
    tie_cells = summarize(read_csv_text(tie_lines[0] + tie_lines[2] + tie_lines[1], 'syn', '<打平>'))
    tie = tie_cells[('syn', 'standard', '35', 'auto', '1')]
    forward = report_of(text)
    check(forward == report_of(reversed_text) and '-inf' in forward[0] and tie['worst_k'] == (-math.inf, 'y')
          and summarize(read_csv_text(tie_text, 'syn', '<打平>'))[('syn', 'standard', '35', 'auto', '1')]['worst_k'] == (-math.inf, 'y'),
          '报告与输入行顺序无关、-inf 照写、打平按曲名取')

    # 语料标签的默认取法（纯路径运算，不碰文件）。
    labels = (default_label('/r/runs', '/r/runs/songs22/t1'), default_label('/r/runs/house9', '/r/runs/house9/t1'),
              default_label('/r/out', '/r/out'))
    check(labels == ('songs22', 'house9', 'out'), '语料标签：所在目录的上一级；CSV 就在给的目录里时取它', ' / '.join(labels))

    print('自检全部通过' if not failed else f'自检有 {len(failed)} 项失败')
    return 0 if not failed else 1


# ---------------------------------------------------------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(description='汇总 heat_ab.csv，按阈值给每格 2a / ABX 结论（T-096d）')
    parser.add_argument('paths', nargs='*', help='heat_ab.csv 文件或目录（可写成 标签=路径）')
    parser.add_argument('--md', help='报告另存为这个 Markdown 文件（同时照样打到标准输出）')
    parser.add_argument('--csv', help='每格一行的汇总 CSV')
    parser.add_argument('--rows', help='每行的分类 CSV（judged / untrusted / noop / na 与原因）')
    parser.add_argument('--selftest', action='store_true', help='只跑自检（合成行，已知答案）')
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.paths:
        parser.print_usage(sys.stderr)
        print('heat_ab_report：要给至少一个 heat_ab.csv 或目录', file=sys.stderr)
        return 2

    try:
        rows = load(args.paths)
        cells = summarize(rows)
    except InputError as error:
        print(f'heat_ab_report：{error}', file=sys.stderr)
        return 1

    report = markdown(cells, rows)
    print(report)
    try:
        if args.md:
            write_new(args.md, report + '\n')
        if args.csv:
            write_new(args.csv, cells_csv(cells))
        if args.rows:
            write_new(args.rows, rows_csv(rows))
    except OSError as error:
        print(f'heat_ab_report：写不了输出：{error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
