#!/usr/bin/env python3
# T-050 回归：tools/render/gate_check.py 把 T-038 追加列 genre / delivery / listen_from_bar 并入配对键之后——
#   ① 仓库里现有的指标表（docs/validation 下表头以 kind,song 开头的 CSV）改前改后输出逐字节相同、返回码相同；
#   ② 现有小表 trial-3songs-metrics.csv 补上 T-038 追加列、每列只有一种取值（general / standard / 1）：改后输出与改前对原表逐字节相同；
#   ③ 曲风两值（general + deep，按渲染工具的网格顺序交错）：改后各自成组、标签与表格带曲风；改前后一组覆盖前一组（general 的组整组丢了）；
#   ④ 聆听起点三值（1 / 9 / 33）+ 交付两值：标签写 from-b<N>、按小节号数值排（9 在 33 前）。
# 用法：regress_gate_check.py --old <改前 gate_check.py> --new <改后 gate_check.py> --repo <仓库根> --out <输出目录>
#   ③ ④ 造的表与两版输出都写进 --out；全部符合预期返回 0，否则 1。只用标准库。改前的脚本：git show 9c18ff9:tools/render/gate_check.py。
# 本目录另有（2026-10-04 DGX Spark 本机）：
#   regress-output.txt                 本脚本的完整输出
#   real-genre-metrics.csv             真跑的指标表：make_synthetic_house.py 的 synthetic-house-124.wav，
#                                      celadon_render --glaze 0,35 --clay auto,off --genre general,deep --bpm 124 --metrics-only
#   real-genre.old.txt / .new.txt      改前 / 改后对它的门槛输出：改前 general 那组被 deep 覆盖、表里只剩一行（ref:dLUFS +0.000 是 deep 的），
#                                      改后两组各一行（general 的 ref:dLUFS +0.297）
import argparse
import csv
import io
import pathlib
import re
import subprocess
import sys

BASE_TABLE = 'docs/validation/2026-09-28-T022-replay-gate/trial-3songs-metrics.csv'


def run(script, table):
    """跑一版 gate_check，返回 (返回码, stdout 字节, stderr 字节)。"""
    p = subprocess.run([sys.executable, str(script), str(table)], capture_output=True)
    return p.returncode, p.stdout, p.stderr


def metrics_tables(repo, skip_dirs):
    """docs/validation 下的指标表；本目录（与 --out）里是本任务造的 / 新跑的多值表，不算「现有」。"""
    for path in sorted((repo / 'docs/validation').rglob('*.csv')):
        if any(d in path.resolve().parents for d in skip_dirs):
            continue
        with open(path, encoding='utf-8') as f:
            if f.readline().startswith('kind,song'):
                yield path


def extended_header(repo):
    """从 RenderMain.cpp 的 csvExtendedHeader() 抽出 T-038 追加列名（与渲染工具同一份，不在这里手抄）。"""
    source = (repo / 'tools/render/RenderMain.cpp').read_text(encoding='utf-8')
    body = re.search(r'const char\* csvExtendedHeader\(\) noexcept\s*\{(.*?)\}', source, re.S).group(1)
    return ''.join(re.findall(r'"([^"]*)"', body)).split(',')


def read_table(path):
    with open(path, encoding='utf-8', newline='') as f:
        reader = csv.DictReader(f)
        return reader.fieldnames, list(reader)


def write_table(path, fieldnames, rows):
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=fieldnames, lineterminator='\n')
    writer.writeheader()
    writer.writerows(rows)
    path.write_text(buffer.getvalue(), encoding='utf-8')


def extend(row, extra, genre='general', delivery='standard', from_bar='1'):
    """补上 T-038 追加列：成品行填曲风 / 交付 / 聆听起点，其余追加列与输入行都空（渲染工具的写法）。"""
    out = dict(row)
    for column in extra:
        out[column] = ''
    if row['kind'] == 'render':
        out.update(genre=genre, delivery=delivery, listen_from_bar=from_bar)
    return out


def interleave(rows, variants, extra):
    """按渲染工具的网格顺序展开：每首歌内 GLAZE 外、修坯中、网格新维度在内；「重放全关」紧跟它的「自动」行（RenderMain processSong）。
    variants：[(维度取值 dict, 改行函数)]，同一组（歌, GLAZE, 修坯）依次出每个变体。"""
    out, i = [], 0
    while i < len(rows):
        r = rows[i]
        if r['kind'] == 'input':
            out.append(extend(r, extra))
            i += 1
            continue
        group = [r]
        if r['clay'] == 'auto' and i + 1 < len(rows) and rows[i + 1]['clay'] == 'replay-off':
            group.append(rows[i + 1])
        for values, tweak in variants:
            for g in group:
                out.append(tweak(extend(g, extra, **values)))
        i += len(group)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--old', required=True)
    parser.add_argument('--new', required=True)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    repo, out = pathlib.Path(args.repo), pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    problems = []

    # ① 现有指标表
    tables = list(metrics_tables(repo, {out.resolve(), pathlib.Path(__file__).resolve().parent}))
    same = 0
    for table in tables:
        old, new = run(args.old, table), run(args.new, table)
        if old == new:
            same += 1
        else:
            problems.append(f'① {table.relative_to(repo)}：改前改后输出不同（返回码 {old[0]} / {new[0]}）')
        print(f"① {'相同' if old == new else '不同'}  rc={new[0]}  {table.relative_to(repo)}")
    print(f'① 现有指标表 {len(tables)} 份，逐字节相同 {same} 份\n')

    extra = extended_header(repo)
    fieldnames, rows = read_table(repo / BASE_TABLE)
    ext_fields = fieldnames + extra
    print(f'T-038 追加列 {len(extra)} 个（取自 RenderMain.cpp csvExtendedHeader）：{",".join(extra[:5])},…')

    # ② 追加列都只有一种取值
    single = out / 'ext-single.csv'
    write_table(single, ext_fields, [extend(r, extra) for r in rows])
    base_old = run(args.old, repo / BASE_TABLE)
    single_new = run(args.new, single)
    ok2 = base_old == single_new
    if not ok2:
        problems.append('② 单值追加列：改后输出与改前对原表的输出不同')
    print(f"② 单值追加列：改后对 ext-single.csv 与改前对原表 {'逐字节相同' if ok2 else '不同'}（rc {single_new[0]} / {base_old[0]}）\n")

    # ③ 曲风两值：general 原样；deep 一份同样的行，另在两边各埋一个只属于自己那组的问题——
    #    general 的 WTF g0「全关」改了哈希（GLAZE 0 逐位直通应失败），deep 的 WTF g70「自动」响度低 0.5 LU（修坯门槛应失败）。
    def general_tweak(r):
        if r['kind'] == 'render' and r['song'] == 'WTF' and r['glaze'] == '0' and r['clay'] == 'off':
            r = dict(r, audio_hash='0000000000000000')
        return r

    def deep_tweak(r):
        if r['kind'] == 'render' and r['song'] == 'WTF' and r['glaze'] == '70' and r['clay'] == 'auto':
            r = dict(r, lufs=f"{float(r['lufs']) - 0.5:.3f}")
        return r

    genre_table = out / 'ext-genre.csv'
    write_table(genre_table, ext_fields,
                interleave(rows, [({'genre': 'general'}, general_tweak), ({'genre': 'deep'}, deep_tweak)], extra))
    g_old, g_new = run(args.old, genre_table), run(args.new, genre_table)
    (out / 'ext-genre.old.txt').write_bytes(g_old[1] + g_old[2])
    (out / 'ext-genre.new.txt').write_bytes(g_new[1] + g_new[2])
    new_text, old_text = g_new[1].decode('utf-8'), g_old[1].decode('utf-8')
    expect_new = [
        'WTF g0 general: GLAZE 0 修坯「自动」与「全关」不逐位相同',
        'WTF g70 deep: 修坯让响度下降 -0.500 LU',
        'ooaa g70 general clay-auto: calibrating',
        'ooaa g70 deep clay-auto: calibrating',
    ]
    for text in expect_new:
        if text not in new_text:
            problems.append(f'③ 改后输出里缺「{text}」')
    if 'FAILS: 2' not in new_text or g_new[0] != 1:
        problems.append(f'③ 改后应正好 2 条失败、返回 1（返回 {g_new[0]}）')
    header = new_text.splitlines()[0]
    if 'genre' not in header or 'delivery' in header or 'listen_from_bar' in header:
        problems.append(f'③ 改后表头应只多 genre 一列：{header}')
    # 改前：deep 的行把 general 的覆盖掉——general 那条 GLAZE 0 失败漏了，标签也分不出曲风。
    if 'GLAZE 0 修坯「自动」与「全关」不逐位相同' in old_text:
        problems.append('③ 改前居然抓到了 general 的 GLAZE 0 失败（对照没有牙）')
    print('③ 曲风两值：改后 rc={}，改前 rc={}；改后输出：'.format(g_new[0], g_old[0]))
    print('   ' + new_text.replace('\n', '\n   '))
    print('   改前输出（对照：general 的组被 deep 覆盖）：')
    print('   ' + old_text.replace('\n', '\n   '))

    # ④ 聆听起点三值 + 交付两值：标签与数值排序。
    variants = [({'from_bar': '33'}, lambda r: r), ({'from_bar': '9'}, lambda r: r), ({'from_bar': '1'}, lambda r: r),
                ({'from_bar': '1', 'delivery': 'club'}, lambda r: r)]
    bars_table = out / 'ext-bars.csv'
    write_table(bars_table, ext_fields, interleave(rows, variants, extra))
    b_new = run(args.new, bars_table)
    (out / 'ext-bars.new.txt').write_bytes(b_new[1] + b_new[2])
    bars_text = b_new[1].decode('utf-8')
    wtf_rows = [line.split() for line in bars_text.splitlines()[1:] if line.startswith('WTF ')]
    order = [(cells[2], cells[3]) for cells in wtf_rows]   # 表格 g 列之后：delivery、listen_from_bar
    expected_order = [('club', '1'), ('standard', '1'), ('standard', '9'), ('standard', '33')]
    if order != expected_order:
        problems.append(f'④ WTF 各组顺序 {order}，应为 {expected_order}（交付按字符串、聆听起点按数值）')
    for text in ('ooaa g70 standard from-b9 clay-auto: calibrating', 'ooaa g70 club from-b1 clay-off: calibrating'):
        if text not in bars_text:
            problems.append(f'④ 改后输出里缺「{text}」')
    print(f'④ 聆听起点 1 / 9 / 33 + 交付 standard / club：WTF 各组顺序 {order}')
    print('   ' + bars_text.replace('\n', '\n   '))

    print('结论：' + ('全部符合预期' if not problems else '有问题'))
    for p in problems:
        print('  - ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
