#!/usr/bin/env python3
# T-059 回归：tools/render/gate_check.py 只改说明文字（ADR-015 起 replay_check = n/a 不再因自动收单出现，说明不再把
# 所有 n/a 都说成收单）之后——
#   ① docs/validation 下表头以 kind,song 开头的现有指标表：改前输出把 TEXT_CHANGES 里的旧文字换成新文字后，
#      与改后输出逐字节相同；返回码、stderr 都相同；
#   ② n/a 分支（现有表没有 replay_check = n/a 的行，走不到）：从 NA_SOURCE_TABLE 造一张表，把 GLAZE 35 的「自动」行
#      replay_check 改成 n/a（GLAZE 70 留 ok，两条分支同表都走到），两版都要给每个 n/a 行一条说明、返回码不变，同样按 ① 比；
#   ③ 「误触发」失败分支（T-056 造过的同一种表）：phase_guard_pct > 5 的「自动」行 slow_inverted_pct 改成 0，同样按 ① 比；
#   ④ --extra 给的表同 ① 比。
# 另外核对：改前输出里本来就不含新文字（否则字面替换会掩盖真差异）。
# 用法：compare_gate_check.py --old <改前 gate_check.py> --new <改后 gate_check.py> --repo <仓库根> --out <输出目录> [--extra 表 ...]
#   改前的脚本：git show origin/main:tools/render/gate_check.py（main c4a804c）。全部符合预期返回 0，否则 1。只用标准库。
import argparse
import csv
import io
import pathlib
import subprocess
import sys

# 本任务唯一允许的输出差别（旧 → 新）：gate_check 对 n/a 行打印的说明。
TEXT_CHANGES = (
    ('同决策重放的阳性对照不适用（自动收单比例在 0、1 之间），修坯门槛照常用重放全关判定',
     '同决策重放的阳性对照不适用（原因见 metrics.md 该曲备注），修坯门槛照常用重放全关判定'),
)
NA_SOURCE_TABLE = 'docs/validation/2026-10-01-T044-clay-guard-loud-masters/gate/base-all.csv'
FAIL_SOURCE_TABLE = 'docs/validation/2026-09-28-T022-replay-gate/trial-3songs-metrics.csv'


def run(script, table):
    """跑一版 gate_check，返回 (返回码, stdout 字节, stderr 字节)。"""
    p = subprocess.run([sys.executable, str(script), str(table)], capture_output=True)
    return p.returncode, p.stdout, p.stderr


def apply_text_changes(old_stdout):
    text = old_stdout.decode('utf-8')
    for old, new in TEXT_CHANGES:
        text = text.replace(old, new)
    return text.encode('utf-8')


def compare(old_result, new_result):
    """纯比较：返回不一致项列表（空 = 只差本任务改的文字）。"""
    old_rc, old_out, old_err = old_result
    new_rc, new_out, new_err = new_result
    problems = []
    if old_rc != new_rc:
        problems.append(f'返回码 {old_rc} → {new_rc}')
    if old_err != new_err:
        problems.append('stderr 不同')
    if any(new.encode('utf-8') in old_out for _, new in TEXT_CHANGES):
        problems.append('改前输出本来就含新文字，字面替换不可靠')
    if apply_text_changes(old_out) != new_out:
        problems.append('stdout 除改的文字外还有差别')
    return problems


def metrics_tables(repo, skip_dirs):
    """docs/validation 下的现有指标表；本目录与 --out 里是本任务造的 / 新跑的表，不算「现有」。"""
    for path in sorted((repo / 'docs/validation').rglob('*.csv')):
        if any(d in path.resolve().parents for d in skip_dirs):
            continue
        with open(path, encoding='utf-8') as f:
            if f.readline().startswith('kind,song'):
                yield path


def rewrite_table(source, target, edit):
    """读 source，逐行交给 edit(row) -> 是否改了，写到 target；返回改了几行。"""
    with open(source, encoding='utf-8', newline='') as f:
        reader = csv.DictReader(f)
        fieldnames, rows = reader.fieldnames, list(reader)
    touched = sum(1 for row in rows if edit(row))
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames, lineterminator='\n')
    writer.writeheader()
    writer.writerows(rows)
    target.write_text(buffer.getvalue(), encoding='utf-8')
    return touched


def make_na(row):
    """GLAZE 35 的「自动」行改成阳性对照 n/a（RenderMain 走 n/a 分支时 replay_max_diff_dbfs 留空）。"""
    if row.get('kind') == 'render' and row.get('clay') == 'auto' and row.get('glaze') == '35':
        row['replay_check'] = 'n/a'
        row['replay_max_diff_dbfs'] = ''
        return True
    return False


def make_false_alarm(row):
    """phase_guard_pct > 5 的「自动」行 slow_inverted_pct 改成 0：走「误触发」失败分支。"""
    if row.get('clay') == 'auto' and float(row.get('phase_guard_pct') or 0) > 5.0:
        row['slow_inverted_pct'] = '0.00'
        return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old', required=True)
    ap.add_argument('--new', required=True)
    ap.add_argument('--repo', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--extra', nargs='*', default=[])
    args = ap.parse_args()
    repo, out = pathlib.Path(args.repo), pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    failures = []
    existing = list(metrics_tables(repo, {pathlib.Path(__file__).resolve().parent, out.resolve()}))
    rc_count, na_rows = {}, 0
    for table in existing:
        with open(table, encoding='utf-8') as f:
            na_rows += sum(1 for row in csv.DictReader(f) if row.get('replay_check') == 'n/a')
        old, new = run(args.old, table), run(args.new, table)
        rc_count[new[0]] = rc_count.get(new[0], 0) + 1
        for p in compare(old, new):
            failures.append(f'① {table.relative_to(repo)}：{p}')
    print(f'① 现有指标表 {len(existing)} 份，返回码分布 {dict(sorted(rc_count.items()))}，其中 replay_check = n/a 的行 {na_rows} 个')

    old_note, new_note = TEXT_CHANGES[0]
    na_table = out / 'na-branch-metrics.csv'
    touched = rewrite_table(repo / NA_SOURCE_TABLE, na_table, make_na)
    old, new = run(args.old, na_table), run(args.new, na_table)
    (out / 'na-branch.old.txt').write_bytes(old[1])
    (out / 'na-branch.new.txt').write_bytes(new[1])
    old_hits, new_hits = old[1].decode('utf-8').count(old_note), new[1].decode('utf-8').count(new_note)
    print(f'② n/a 分支表：改了 {touched} 行，返回 {old[0]} → {new[0]}，旧说明 {old_hits} 条 → 新说明 {new_hits} 条')
    if touched == 0 or old_hits != touched or new_hits != touched:
        failures.append(f'② n/a 分支没走全（改了 {touched} 行，旧说明 {old_hits} 条，新说明 {new_hits} 条）')
    failures += [f'② {p}' for p in compare(old, new)]

    fail_table = out / 'fail-branch-metrics.csv'
    touched = rewrite_table(repo / FAIL_SOURCE_TABLE, fail_table, make_false_alarm)
    old, new = run(args.old, fail_table), run(args.new, fail_table)
    hits = new[1].decode('utf-8').count('误触发')
    print(f'③ 误触发分支表：改了 {touched} 行，改后返回 {new[0]}、「误触发」{hits} 条')
    if new[0] != 1 or hits != touched:
        failures.append(f'③ 失败分支没走到（返回 {new[0]}，误触发 {hits} 条，应为 {touched}）')
    failures += [f'③ {p}' for p in compare(old, new)]

    for extra in args.extra:
        old, new = run(args.old, extra), run(args.new, extra)
        print(f'④ {extra}：返回 {old[0]} → {new[0]}')
        failures += [f'④ {extra}：{p}' for p in compare(old, new)]

    print(f'\n不一致：{len(failures)}')
    for f in failures:
        print(f'  - {f}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
