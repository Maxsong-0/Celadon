#!/usr/bin/env python3
"""T-096a 质量不退步门槛：拿新的 quality.csv 对基线 quality.csv 逐行判（只用标准库）。

用法：quality_gate.py <基线 quality.csv> <新 quality.csv>
      quality_gate.py --selftest
退出码（同 gate_check.py）：0 = 没有失败；1 = 有失败；2 = 用法或输入错（文件读不到、缺列、配对键重复）。

口径与阈值出处：docs/validation/2026-10-11-T096a-metrics/GLOSSARY.md 第 2 节。配对键 = (song, kind, glaze, clay, style, genre, delivery)。
- A 组绝对：true_peak_dbtp − ceiling_dbtp ≤ 0.01；hard_clip_samples == 0。越线就失败，不看基线。
- B 组不退步：没越线 = 通过；越线但基线同一行也越线、且新值不比基线差 = 已知（K）；否则失败。
  基线有值、新值空 = 失败（算不出不能当通过）。两边都空 = N/A。新表缺基线里的行 = 失败；新表多出的行只判绝对的几条。
- 「同组 P90」按基线表里同 (glaze, clay, style, genre, delivery) 的行算（线性插值分位，同 numpy 默认）。
本脚本只读两份表、只往标准输出写，不改任何文件。
"""
import csv
import math
import sys
from collections import defaultdict

# ======== 阈值常量：只在这里改（与 GLOSSARY.md 第 2 节同名同值）========
TP_EPSILON_DB = 0.01               # A：真峰值 ≤ Ceiling + 0.01（同 gate_check.py / house_gate.py 的容差）
DROP_TARGET_WINDOW_LU = 1.0        # B1：|drop 目标偏差| ≤ 1 LU
DROP_LOSS_LU = 1.2                 # B2：落差损失 ≤ 1.2 LU（标准 / 流媒体）
DROP_LOSS_CLUB_LU = 1.5            # B2：俱乐部档 ≤ 1.5 LU
LRA_LOSS_ALLOWED_LU = 1.0          # B2：LRA 出口 ≥ 入口 − 1 LU
PSR_FLOOR_MARGIN_DB = 0.5          # B3：主段 PSR 中位 ≥ 规格下限 − 0.5 dB
BASELINE_PERCENTILE = 90           # B4 / B9：同组基线的 P90
LF_CREST_ALLOWED_DROP_DB = 0.3     # B5：低频峰值因数变化 ≥ 同行基线 − 0.3 dB
LIMIT_DIFF_ALLOWED_RISE_DB = 1.0   # B6：限幅差值能量 ≤ 同行基线 + 1 dB
LOW_CORR_ALLOWED_DROP = 0.05       # B7：低频相关度变化 ≥ −0.05
SIDE_MID_MAX_RISE_DB = 0.5         # B7：逐段 S/M 变化 P95 ≤ 0.5 dB
COMPARE_EPSILON = 1e-9             # 比较余量：CSV 是定点小数，只防浮点尾数

PAIR_KEY = ('song', 'kind', 'glaze', 'clay', 'style', 'genre', 'delivery')
GROUP_KEY = ('glaze', 'clay', 'style', 'genre', 'delivery')

PASS, FAIL, KNOWN, NA = 'PASS', 'FAIL', 'KNOWN', 'N/A'
SYMBOL = {PASS: '.', FAIL: 'F', KNOWN: 'K', NA: '-'}

# 每条规则：(代号, 中文名, 读的字段, 方向, 阈值来源, 参数)。
#   方向：max = 值 ≤ 阈值；min = 值 ≥ 阈值；abs_max = |值| ≤ 阈值。
#   阈值来源：absolute = 只看本行（参数是 行 → 阈值 的函数）；row = 同行基线 + 参数；group = 同组基线 P90；
#            hard = A 组绝对（参数是 行 → 阈值，越线即失败、不看基线）。
RULES = (
    ('A1', '真峰值', 'tp_over_ceiling', 'max', 'hard', lambda row: TP_EPSILON_DB),
    ('A2', '硬钳位', 'hard_clip_samples', 'max', 'hard', lambda row: 0.0),
    ('B1', 'drop 目标', 'drop_target_dev_lu', 'abs_max', 'absolute', lambda row: DROP_TARGET_WINDOW_LU),
    ('B2a', '落差', 'drop_loss_lu', 'max', 'absolute',
     lambda row: DROP_LOSS_CLUB_LU if row.get('delivery') == 'club' else DROP_LOSS_LU),
    ('B2b', 'LRA', 'lra_delta_lu', 'min', 'absolute', lambda row: -LRA_LOSS_ALLOWED_LU),
    ('B3', 'PSR', 'psr_main_med_out_db', 'min', 'absolute', lambda row: num(row.get('psr_floor_spec_db')) - PSR_FLOOR_MARGIN_DB),
    ('B4', '泵感', 'pump_db', 'max', 'group', None),
    ('B5', '低频峰值因数', 'lf_crest_db', 'min', 'row', -LF_CREST_ALLOWED_DROP_DB),
    ('B6', '限幅差值', 'limit_diff_db', 'max', 'row', LIMIT_DIFF_ALLOWED_RISE_DB),
    ('B7a', '低频相关', 'low_corr_delta', 'min', 'absolute', lambda row: -LOW_CORR_ALLOWED_DROP),
    ('B7b', 'S/M', 'side_mid_delta_p95_db', 'max', 'absolute', lambda row: SIDE_MID_MAX_RISE_DB),
    ('B9a', '增益总变差', 'gain_tv_db_per_min', 'max', 'group', None),
    ('B9b', '增益变号', 'gain_sign_changes_per_min', 'max', 'group', None),
)
# 表里必须有的列（tp_over_ceiling 由 true_peak_dbtp − ceiling_dbtp 现算）。
REQUIRED_COLUMNS = PAIR_KEY + ('true_peak_dbtp', 'ceiling_dbtp', 'psr_floor_spec_db') + tuple(
    rule[2] for rule in RULES if rule[2] != 'tp_over_ceiling')


class InputError(Exception):
    """用法或输入错：退出码 2。"""


def num(value, default=math.nan):
    """CSV 格子 → float（认 inf / -inf）；空串、None、读不成数 → default。"""
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def value_of(row, field):
    if row is None:
        return math.nan
    if field == 'tp_over_ceiling':
        return num(row.get('true_peak_dbtp')) - num(row.get('ceiling_dbtp'))
    return num(row.get(field))


def percentile(values, percent):
    """线性插值分位（同 numpy.percentile 默认）：位置 = (n − 1) × p / 100。空 → NaN。"""
    ordered = sorted(v for v in values if not math.isnan(v))
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * percent / 100.0
    low = math.floor(position)
    high = min(low + 1, len(ordered) - 1)
    if ordered[low] == ordered[high]:   # 也兜住两端都是 ±inf 的情形（inf − inf 是 NaN）
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def crosses(value, limit, direction):
    if direction == 'max':
        return value > limit + COMPARE_EPSILON
    if direction == 'min':
        return value < limit - COMPARE_EPSILON
    return abs(value) > limit + COMPARE_EPSILON


def not_worse(new, base, direction):
    if direction == 'max':
        return new <= base + COMPARE_EPSILON
    if direction == 'min':
        return new >= base - COMPARE_EPSILON
    return abs(new) <= abs(base) + COMPARE_EPSILON


def describe(value, limit, direction):
    sign = {'max': '≤', 'min': '≥', 'abs_max': '|·| ≤'}[direction]
    return f'{value:.3f}（要 {sign} {limit:.3f}）'


def judge(rule, new_row, base_row, group_limits):
    """一条规则、一行 → (结论, 说明)。纯函数：不读文件。"""
    code, _name, field, direction, source, parameter = rule
    new, base = value_of(new_row, field), value_of(base_row, field)
    if math.isnan(new):
        if not math.isnan(base):
            return FAIL, f'基线有值（{base:.3f}）、新值空'
        return NA, '两边都没有读数'
    if source in ('hard', 'absolute'):
        limit = parameter(new_row)
        base_limit = parameter(base_row) if base_row is not None else math.nan
    elif source == 'row':
        limit = base_limit = base + parameter
    else:
        limit = base_limit = group_limits.get((code, tuple(new_row.get(k, '') for k in GROUP_KEY)), math.nan)
    if math.isnan(limit):
        return NA, '没有阈值（基线这一行或这一组没有读数）'
    if not crosses(new, limit, direction):
        return PASS, describe(new, limit, direction)
    if source != 'hard' and not math.isnan(base) and not math.isnan(base_limit) \
            and crosses(base, base_limit, direction) and not_worse(new, base, direction):
        return KNOWN, f'{describe(new, limit, direction)}；基线也越线（{base:.3f}），没有更差'
    return FAIL, describe(new, limit, direction) + (f'；基线 {base:.3f}' if not math.isnan(base) else '；基线没有读数')


def group_percentiles(baseline_rows):
    """同组基线 P90：{(规则代号, 组键): 阈值}。"""
    values = defaultdict(list)
    for row in baseline_rows:
        group = tuple(row.get(k, '') for k in GROUP_KEY)
        for code, _name, field, _direction, source, _parameter in RULES:
            if source == 'group':
                values[(code, group)].append(value_of(row, field))
    return {key: percentile(vals, BASELINE_PERCENTILE) for key, vals in values.items()}


def index_rows(rows, where):
    indexed = {}
    for row in rows:
        key = tuple(row.get(k, '') for k in PAIR_KEY)
        if key in indexed:
            raise InputError(f'{where}：配对键 {key} 重复')
        indexed[key] = row
    return indexed


def evaluate(baseline_rows, new_rows):
    """两份表 → (逐行结论 [(键, {代号: 结论})], 失败列表, 已知列表, 说明列表, N/A 计数)。纯函数。"""
    base, new = index_rows(baseline_rows, '基线'), index_rows(new_rows, '新表')
    limits = group_percentiles(baseline_rows)
    table, fails, known, notes = [], [], [], []
    na_count = defaultdict(int)
    for key in sorted(set(base) | set(new), key=lambda k: (k[0], num(k[2]), k[3:], k[1])):
        tag = ' / '.join(part for part in key if part)
        if key not in new:
            fails.append(f'{tag}：新表缺这一行（基线里有）')
            continue
        if key not in base:
            notes.append(f'{tag}：基线没有这一行，只判 A 组与绝对阈值')
        verdicts = {}
        for rule in RULES:
            verdict, detail = judge(rule, new[key], base.get(key), limits)
            verdicts[rule[0]] = verdict
            if verdict == FAIL:
                fails.append(f'{tag}：{rule[0]} {rule[1]} {detail}')
            elif verdict == KNOWN:
                known.append(f'{tag}：{rule[0]} {rule[1]} {detail}')
            elif verdict == NA:
                na_count[rule[0]] += 1
        table.append((key, verdicts))
    return table, fails, known, notes, na_count


def read_table(path):
    try:
        with open(path, encoding='utf-8', newline='') as stream:
            rows = list(csv.DictReader(stream))
    except OSError as error:
        raise InputError(f'读不了 {path}：{error}') from error
    if not rows:
        raise InputError(f'{path} 是空表')
    missing = [c for c in REQUIRED_COLUMNS if c not in rows[0]]
    if missing:
        raise InputError(f'{path} 缺列 {", ".join(missing)}（要 quality_metrics.py 写的表）')
    return rows


def print_report(table, fails, known, notes, na_count):
    codes = [rule[0] for rule in RULES]
    print('图例：. 通过  F 失败  K 已知（基线也越线且没有更差）  - N/A')
    print(f"{'song':30} {'glaze':>5} {'clay':>10} {'style':>7} {'genre':>8} {'delivery':>9}  " + ' '.join(f'{c:>4}' for c in codes))
    for key, verdicts in table:
        song, _kind, glaze, clay, style, genre, delivery = key
        print(f'{song[:30]:30} {glaze:>5} {clay[:10]:>10} {style[:7]:>7} {genre[:8]:>8} {delivery[:9]:>9}  '
              + ' '.join(f'{SYMBOL[verdicts[c]]:>4}' for c in codes))
    print(f'\nFAILS: {len(fails)}')
    for line in fails:
        print(f'  - {line}')
    print(f'KNOWN: {len(known)}')
    for line in known:
        print(f'  - {line}')
    print(f'NOTES: {len(notes)}')
    for line in notes:
        print(f'  - {line}')
    names = {rule[0]: rule[1] for rule in RULES}
    print('N/A：' + ('、'.join(f'{c} {names[c]} {na_count[c]} 行' for c in codes if na_count[c]) or '无'))
    # 分开报：A 组是绝对门（基线自己越线也算），B 组才是「退步」；缺行单列。
    hard = {rule[0] for rule in RULES if rule[4] == 'hard'}
    a_fails = sum(1 for _key, verdicts in table for c in hard if verdicts[c] == FAIL)
    b_fails = sum(1 for _key, verdicts in table for c in codes if c not in hard and verdicts[c] == FAIL)
    print(f'汇总：A 组越线 {a_fails}、B 组退步 {b_fails}、新表缺行 {len(fails) - a_fails - b_fails}')


def main(argv):
    if argv == ['--selftest']:
        return run_selftest()
    if len(argv) != 2:
        print('用法：quality_gate.py <基线 quality.csv> <新 quality.csv>（或 --selftest）', file=sys.stderr)
        return 2
    try:
        baseline_rows, new_rows = read_table(argv[0]), read_table(argv[1])
        table, fails, known, notes, na_count = evaluate(baseline_rows, new_rows)
    except InputError as error:
        print(f'quality_gate：{error}', file=sys.stderr)
        return 2
    print(f'quality_gate：基线 {len(baseline_rows)} 行、新表 {len(new_rows)} 行')
    print_report(table, fails, known, notes, na_count)
    return 1 if fails else 0


# ======== 自检（--selftest）：内存里的合成行，已知结论 ========

def synthetic_row(song, **values):
    """一行全部通过的 quality 行（每个判定字段都有读数），values 覆盖个别字段。"""
    row = {'song': song, 'kind': 'render', 'glaze': '35', 'clay': 'auto', 'style': 'auto', 'genre': 'general',
           'delivery': 'standard', 'true_peak_dbtp': '-1.100', 'ceiling_dbtp': '-1.000', 'hard_clip_samples': '0',
           'drop_target_dev_lu': '0.200', 'drop_loss_lu': '0.500', 'lra_delta_lu': '-0.300', 'psr_main_med_out_db': '9.000',
           'psr_floor_spec_db': '8.5', 'pump_db': '0.1000', 'lf_crest_db': '-0.100', 'limit_diff_db': '-30.000',
           'low_corr_delta': '0.0000', 'side_mid_delta_p95_db': '0.100', 'gain_tv_db_per_min': '5.000',
           'gain_sign_changes_per_min': '10.000'}
    row.update({k: str(v) for k, v in values.items()})
    return row


def selftest_cases():
    """(说明, 基线行, 新行, 期望：失败条数, 已知条数)。"""
    # s0 … s9 与 over 同组（11 行：泵感 0.10 … 0.19，over 是 0.10）：P90 = 0.18；s9（0.19）在基线里就高于 P90。
    # club 自成一组（交付档不同）。
    baseline = [synthetic_row(f's{i}', pump_db=f'{0.10 + 0.01 * i:.4f}') for i in range(10)]
    baseline.append(synthetic_row('club', delivery='club', drop_loss_lu='1.400'))
    baseline.append(synthetic_row('over', side_mid_delta_p95_db='0.800'))   # 基线就越线（S/M）
    same = [dict(row) for row in baseline]

    def changed(song, **values):
        rows = []
        for row in baseline:
            row = dict(row)
            if row['song'] == song:
                row.update({k: str(v) for k, v in values.items()})
            rows.append(row)
        return rows

    return [
        ('基线对基线：0 失败（s9 泵感高于组 P90、over 的 S/M 越线都记已知）', baseline, same, 0, 2),
        ('真峰值超 Ceiling 0.02 dB：A1 失败', baseline, changed('s0', true_peak_dbtp='-0.980'), 1, 2),
        ('硬钳位 1 个样本：A2 失败', baseline, changed('s0', hard_clip_samples='1'), 1, 2),
        ('俱乐部落差 1.4 LU 在 1.5 以内：通过', baseline, same, 0, 2),
        ('标准档落差 1.3 LU：B2a 失败', baseline, changed('s1', drop_loss_lu='1.300'), 1, 2),
        ('基线有值、新值空（泵感）：B4 失败', baseline, changed('s2', pump_db=''), 1, 2),
        ('泵感升到 0.20（> 组 P90 0.18）：B4 失败', baseline, changed('s3', pump_db='0.2000'), 1, 2),
        ('over 的 S/M 从 0.8 变 0.9（越线且更差）：B7b 失败', baseline, changed('over', side_mid_delta_p95_db='0.900'), 1, 1),
        ('低频峰值因数比同行基线低 0.4 dB：B5 失败', baseline, changed('s4', lf_crest_db='-0.500'), 1, 2),
        ('限幅差值比同行基线高 0.9 dB：通过', baseline, changed('s5', limit_diff_db='-29.100'), 0, 2),
        ('新表少一行：失败', baseline, [row for row in same if row['song'] != 's6'], 1, 2),
    ]


def run_selftest():
    failed = 0
    cases = selftest_cases()
    for title, baseline, new, want_fails, want_known in cases:
        try:
            _table, fails, known, _notes, _na = evaluate(baseline, new)
            ok = len(fails) == want_fails and len(known) == want_known
            detail = f'失败 {len(fails)}（期望 {want_fails}）、已知 {len(known)}（期望 {want_known}）'
            if not ok:
                detail += '：' + ' | '.join(fails + known)
        except Exception as error:   # 自检要把任何异常都算失败并接着跑下一条
            ok, detail = False, f'抛异常 {type(error).__name__}：{error}'
        failed += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {title}：{detail}")
    rule_ok = abs(percentile([0.10 + 0.01 * i for i in range(10)], 90) - 0.181) <= 1e-12
    failed += 0 if rule_ok else 1
    print(f"{'ok  ' if rule_ok else 'FAIL'} 分位数：0.10 … 0.19 的 P90 = 0.181（线性插值）")
    total = len(cases) + 1
    print(f'{total - failed} / {total} ok')
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
