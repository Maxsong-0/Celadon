#!/usr/bin/env python3
# T-056 回归：tools/render/gate_check.py 只改说明文字（phase_guard_pct 从「反相保护介入」改叫「反相提示时长占比」，
# 打印表头 guard% → hint%；ADR-015 起反相只提示、不改声音）之后——
#   ① docs/validation 下表头以 kind,song 开头的现有指标表：改前输出把 TEXT_CHANGES 里的旧文字换成新文字后，
#      与改后输出逐字节相同；返回码、stderr 都相同；
#   ② 失败分支（现有表走不到）：从 trial-3songs-metrics.csv 造一张表，把 phase_guard_pct > 5 的「自动」行的
#      slow_inverted_pct 改成 0，两版都要报「误触发」失败、返回 1，同样按 ① 比；
#   ③ --extra 给的表（如本目录 render-after-metrics.csv）同 ① 比。
# 另外核对：改前输出里本来就不含新文字（否则字面替换会掩盖真差异）。
# 用法：compare_gate_check.py --old <改前 gate_check.py> --new <改后 gate_check.py> --repo <仓库根> --out <输出目录> [--extra 表 ...]
#   改前的脚本：git show 218789a:tools/render/gate_check.py。全部符合预期返回 0，否则 1。只用标准库。
import argparse
import csv
import io
import pathlib
import subprocess
import sys

# 本任务唯一允许的输出差别（旧 → 新）。表头靠 {:>6} 右对齐，'hint%' 比 'guard%' 短一个字符，前面多补一个空格。
TEXT_CHANGES = (('反相保护介入', '反相提示时长占比'), ('guard%', ' hint%'))
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


def make_fail_branch_table(source, target):
    """把 phase_guard_pct > 5 的「自动」行的 slow_inverted_pct 改成 0：走 gate_check 的「误触发」失败分支。"""
    with open(source, encoding='utf-8', newline='') as f:
        reader = csv.DictReader(f)
        fieldnames, rows = reader.fieldnames, list(reader)
    touched = 0
    for row in rows:
        if row.get('clay') == 'auto' and float(row.get('phase_guard_pct') or 0) > 5.0:
            row['slow_inverted_pct'] = '0.00'
            touched += 1
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames, lineterminator='\n')
    writer.writeheader()
    writer.writerows(rows)
    target.write_text(buffer.getvalue(), encoding='utf-8')
    return touched


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
    rc_count = {}
    for table in existing:
        old, new = run(args.old, table), run(args.new, table)
        rc_count[new[0]] = rc_count.get(new[0], 0) + 1
        for p in compare(old, new):
            failures.append(f'① {table.relative_to(repo)}：{p}')
    print(f'① 现有指标表 {len(existing)} 份，返回码分布 {dict(sorted(rc_count.items()))}')

    fail_table = out / 'fail-branch-metrics.csv'
    touched = make_fail_branch_table(repo / FAIL_SOURCE_TABLE, fail_table)
    old, new = run(args.old, fail_table), run(args.new, fail_table)
    (out / 'fail-branch.old.txt').write_bytes(old[1])
    (out / 'fail-branch.new.txt').write_bytes(new[1])
    hits = new[1].decode('utf-8').count('误触发')
    print(f'② 失败分支表：改了 {touched} 行，改后返回 {new[0]}、「误触发」{hits} 条')
    if new[0] != 1 or hits != touched:
        failures.append(f'② 失败分支没走到（返回 {new[0]}，误触发 {hits} 条，应为 {touched}）')
    failures += [f'② {p}' for p in compare(old, new)]

    for extra in args.extra:
        old, new = run(args.old, extra), run(args.new, extra)
        print(f'③ {extra}：返回 {old[0]} → {new[0]}')
        failures += [f'③ {extra}：{p}' for p in compare(old, new)]

    print(f'\n不一致：{len(failures)}')
    for f in failures:
        print(f'  - {f}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
