#!/usr/bin/env python3
"""T-113 听感批的逐 drop 报告：celadon_render --trace 的成品追踪 → 每首歌每个 drop 的预算环退让次数、Limit Gain 总变差，基线对候选并排。

设计与用法总说明见 docs/validation/2026-11-13-T113-listening/README.md。三个入口：

  compare   --a <基线输出目录> [--b <候选输出目录>] (--annotations <标注 JSON 或目录> … | --labels <labels-auto.tsv>
            [--labels-override <改标.tsv>] --tempo <tempo-manifest.tsv>) --out <报告目录> [--a-name current] [--b-name c105-106]
            → <报告目录>/drop_report.md 与 drops.csv（不带时间戳，同输入逐字相同）。
            输出目录 = 一次或多次 celadon_render --out（递归找 trace/<成品名>.csv，跳过第一遍的 __listen.csv）；A、B 按成品文件名配对。
  windows   (--annotations … | --labels … --tempo …) --out segments.json [--tracks a,b] [--before-bars 4] [--after-bars 8] [--per-song 3]
            → blind_test.py build --segments 用的 {"歌名": [[起点秒, 终点秒], ...]}：每首从第一个 drop 起取「drop 前 4 小节 + 后 8 小节」，
            第一个 drop 必在（冷启动只发生在它上面），不够再取后面的 drop。只读标注，不看任何渲染结果（盲听登记前就能定）。
  --selftest 合成追踪与标注跑一遍两个入口，核对已知答案与「重跑逐字相同」，再核对坏输入报错。

量什么（成品追踪的列，RenderMain.cpp 的 --trace 表头）：
  - 退让次数 retreats：guard_db（预算环自己的 g_保护）连续下降的一段算一次，起点落在这个 drop 里才算这个 drop 的；
    两段之间停不到 MERGE_GAP_S 的并成一次（退让按 10 ms 一格走，块边界会把一段切碎）。
  - 退让量 retreat_db：drop 里 guard_db 下降的总量（dB，正数）。
  - 增益总变差 gain_tv_db：drop 里相邻两块 limit_gain_db（模块实际拿到的 Limit Gain）之差的绝对值之和——增益来回走得越多越大。
  - 前段增益 early_gain_db：drop 头 EARLY_BARS 小节 limit_gain_db 的平均（1-06 冷启动第一个 drop「推得不够」看它）。
  - 进 drop 时的 guard_db、drop 里最低的 guard_db、限幅 GR 平均 gr_mean_db、主段度 ≥ 0.5 的块占比 main_share。
  - 整首：非主段放退让 non_main_release_db = 主段度 < 0.5 的块里 guard_db 上升的总量（1-05 主段门开着时应为 0）。
段落从哪来：合成曲用 make_synthetic_house.py 写的标注 JSON；Jamendo 用 T-038 的逐小节标签 + 速度表（wav 已裁到小节线，第 N 小节从
(N − 1) × 小节秒数起）。标签在段落交界两侧各标 1 个 transition：drop 的起点取交界那条线（transition 段的中间），终点同理。

出错规则（同 quality_metrics.py 的 Q12）：基础设施错（读不到、缺列、A / B 对不上、同名追踪重复）→ 退出码 1、不写输出；
参数不合法 → 2；素材原因算不出（没有标注、没有 drop）→ 那首歌写 na_reason，不填 0。只用标准库；只读输入，只写 --out。
"""
import argparse
import csv
import io
import json
import math
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import section_lib  # noqa: E402  同目录的段落读法（标注 JSON、逐小节标签），不另抄

# ======== 口径常数：只在这里改 ========
MAINNESS_MAIN = 0.5            # 主段：current_mainness ≥ 它（同插件「算主段」的线、同 quality_metrics.py 的 ⑤）
GUARD_STEP_DB = 0.0005         # guard_db 写 3 位小数：变动超过半个末位才算动了（恢复 0.3 dB/s ≈ 每块 0.0035 dB，看得见）
MERGE_GAP_S = 0.1              # 两段下降之间停不到 0.1 s 并成一次退让
EARLY_BARS = 8                 # drop 头几小节算「前段」
BEATS_PER_BAR_DEFAULT = 4      # 速度表没有拍号：按 4/4（同 blind_test.py 的 BEATS_PER_BAR）
WINDOW_BEFORE_BARS = 4         # windows：drop 前取几小节
WINDOW_AFTER_BARS = 8          # windows：drop 后取几小节（共 12 小节，落在 ADR-034 的 8～16 小节里）
WINDOWS_PER_SONG = 3           # windows：每首几个窗口（6 首 × 3 = 18 = 2AFC 默认试次）
REQUIRED_COLUMNS = ('t_sec', 'limit_gain_db', 'guard_db', 'current_mainness', 'limit_gr_db')
DROP_FIELDS = ('retreats', 'retreat_db', 'gain_tv_db', 'early_gain_db', 'entry_guard_db', 'min_guard_db', 'gr_mean_db', 'main_share')
SONG_FIELDS = ('retreats_total', 'retreats_in_drops', 'non_main_release_db')
CSV_COLUMNS = ('side', 'name', 'song', 'product', 'drop', 'first', 'start_s', 'end_s') + DROP_FIELDS + SONG_FIELDS + ('na_reason',)


class ToolError(Exception):
    """基础设施错 → 退出码 1。"""


class UsageError(Exception):
    """参数不合法 → 退出码 2。"""


# ---------------------------------------------------------------------------------------------------------------------
# 段落：标注 JSON / 逐小节标签 → 每首歌的 drop 区间（秒）与小节秒数

def drops_from_annotation(annotation):
    """标注 JSON → (小节秒数, [(起点秒, 终点秒)])。换算同 section_lib.drop_spans_seconds。"""
    spans = section_lib.drop_spans_seconds(annotation)
    beats = int(str(annotation.get('meter', '4/4')).split('/')[0])
    return beats * 60.0 / float(annotation['bpm']), spans


def drops_from_bar_labels(bar_labels, bpm):
    """一首歌的 {小节: 标签} → (小节秒数, [(起点秒, 终点秒)])。drop 段 = 连续的 drop 小节；前后紧挨的 transition 小节各让一半
    给 drop（交界在 transition 段的中间：标签规则是交界两侧各标 1 个，所以通常是往外扩 1 小节）。"""
    bar_s = BEATS_PER_BAR_DEFAULT * 60.0 / bpm
    spans = []
    bars = sorted(bar_labels)
    for d0 in bars:
        if bar_labels[d0] != 'drop' or bar_labels.get(d0 - 1) == 'drop':
            continue
        d1 = d0
        while bar_labels.get(d1 + 1) == 'drop':
            d1 += 1
        before = 0
        while bar_labels.get(d0 - 1 - before) == 'transition':
            before += 1
        after = 0
        while bar_labels.get(d1 + 1 + after) == 'transition':
            after += 1
        first, last = d0 - before // 2, d1 + after // 2
        spans.append(((first - 1) * bar_s, last * bar_s))
    return bar_s, spans


def load_sections(args):
    """--annotations 或 --labels + --tempo → {歌名: (小节秒数, [drop 区间])}。"""
    try:
        if args.annotations:
            table = {}
            for song, annotation in section_lib.load_annotations(args.annotations).items():
                table[song] = drops_from_annotation(annotation)
            return table
        labels = section_lib.load_labels(args.labels, args.labels_override)
        with open(args.tempo, encoding='utf-8') as f:
            tempo = {r['track']: float(r['bpm']) for r in csv.DictReader(f, delimiter='\t')}
    except (OSError, KeyError, ValueError, json.JSONDecodeError) as error:
        raise ToolError(f'读不了段落标注 / 标签 / 速度表：{type(error).__name__} {error}')
    per_song = {}
    for (track, bar), label in labels.items():
        per_song.setdefault(track, {})[bar] = label
    return {track: drops_from_bar_labels(bars, tempo[track]) for track, bars in per_song.items() if track in tempo}


def song_of(stem):
    """成品名 <歌名>__g35_… → 歌名（同 blind_test.song_key）。"""
    return stem.split('__')[0]


# ---------------------------------------------------------------------------------------------------------------------
# 追踪：读 → 逐 drop 量

def find_traces(directory):
    """输出目录（可含多次渲染的子目录）里所有成品追踪 → {成品名: 路径}。同名两份 → 基础设施错（配对会乱）。"""
    root = Path(directory)
    if not root.is_dir():
        raise ToolError(f'{directory} 不是目录')
    found = {}
    for path in sorted(root.rglob('*.csv')):
        if path.parent.name != 'trace' or path.stem.endswith('__listen'):
            continue
        if path.stem in found:
            raise ToolError(f'成品追踪重名：{found[path.stem]} 与 {path}（一个目录里只放一个候选的渲染）')
        found[path.stem] = path
    if not found:
        raise ToolError(f'{directory} 里没有 trace/<成品名>.csv（渲染时要给 --trace）')
    return found


def read_trace(path):
    """成品追踪 → 每列一个 float 列表（只取 REQUIRED_COLUMNS）。"""
    try:
        with open(path, encoding='utf-8') as f:
            reader = csv.DictReader(f)
            missing = [c for c in REQUIRED_COLUMNS if c not in (reader.fieldnames or [])]
            if missing:
                raise ToolError(f'{path} 缺列 {"、".join(missing)}（要 T-096 之后的渲染器跑 --trace）')
            columns = {c: [] for c in REQUIRED_COLUMNS}
            for row in reader:
                for c in REQUIRED_COLUMNS:
                    columns[c].append(section_lib.num(row[c]))
    except OSError as error:
        raise ToolError(f'读不了 {path}：{error}')
    if not columns['t_sec']:
        raise ToolError(f'{path} 没有数据行')
    return columns


def retreat_onsets(times, guard):
    """guard_db 连续下降的段 → 每段起点的块下标（相邻块都在降的算同一段；停不到 MERGE_GAP_S 又接着降的两段也并成一段）。"""
    onsets, last_fall = [], None
    for i in range(1, len(guard)):
        if guard[i] < guard[i - 1] - GUARD_STEP_DB:
            continuing = last_fall is not None and (last_fall == i - 1 or times[i] - times[last_fall] <= MERGE_GAP_S)
            if not continuing:
                onsets.append(i)
            last_fall = i
    return onsets


def mean(values):
    finite = [v for v in values if math.isfinite(v)]
    return sum(finite) / len(finite) if finite else math.nan


def measure_drop(trace, onsets, start_s, end_s, bar_s):
    """一个 drop 的各项（DROP_FIELDS）；drop 落在追踪之外 → None。"""
    times = trace['t_sec']
    index = [i for i, t in enumerate(times) if start_s <= t < end_s]
    if not index:
        return None
    guard, gain = trace['guard_db'], trace['limit_gain_db']
    falls = [guard[i - 1] - guard[i] for i in index if i > 0 and guard[i] < guard[i - 1] - GUARD_STEP_DB]
    pairs = [(i - 1, i) for i in index if i - 1 >= index[0]]
    early_end = start_s + EARLY_BARS * bar_s
    # dB 量用 math.fsum：没有元素时也是浮点 0.0（内置 sum 给整数 0，CSV 会写成「0」而不是「0.000」）。
    return {
        'retreats': sum(1 for o in onsets if start_s <= times[o] < end_s),
        'retreat_db': math.fsum(falls),
        'gain_tv_db': math.fsum(abs(gain[b] - gain[a]) for a, b in pairs),
        'early_gain_db': mean([gain[i] for i in index if times[i] < early_end]),
        'entry_guard_db': guard[index[0]],
        'min_guard_db': min(guard[i] for i in index),
        'gr_mean_db': mean([trace['limit_gr_db'][i] for i in index]),
        'main_share': sum(1 for i in index if trace['current_mainness'][i] >= MAINNESS_MAIN) / len(index),
    }


def measure_song(trace, drops, bar_s):
    """一份成品追踪 → (整首的 SONG_FIELDS, [每个 drop 的 DROP_FIELDS 或 None])。"""
    times, guard, mainness = trace['t_sec'], trace['guard_db'], trace['current_mainness']
    onsets = retreat_onsets(times, guard)
    per_drop = [measure_drop(trace, onsets, start, end, bar_s) for start, end in drops]
    song = {
        'retreats_total': len(onsets),
        'retreats_in_drops': sum(d['retreats'] for d in per_drop if d is not None),
        'non_main_release_db': math.fsum(guard[i] - guard[i - 1] for i in range(1, len(guard))   # fsum：同 measure_drop
                                   if mainness[i] < MAINNESS_MAIN and guard[i] > guard[i - 1] + GUARD_STEP_DB),
    }
    return song, per_drop


def measure_side(directory, sections):
    """一侧（一个输出目录）→ {成品名: (歌名, 整首, [每 drop], 区间, na_reason)}。"""
    result = {}
    for stem, path in find_traces(directory).items():
        song = song_of(stem)
        if song not in sections:
            result[stem] = (song, None, [], [], '没有这首歌的段落标注 / 标签')
            continue
        bar_s, drops = sections[song]
        if not drops:
            result[stem] = (song, None, [], [], '段落里没有 drop')
            continue
        whole, per_drop = measure_song(read_trace(path), drops, bar_s)
        result[stem] = (song, whole, per_drop, drops, '')
    return result


# ---------------------------------------------------------------------------------------------------------------------
# 输出

def fmt(value, digits=2):
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return ''
    if isinstance(value, int):
        return str(value)
    text = f'{value:.{digits}f}'
    return '0.' + '0' * digits if text.strip('-0.') == '' else text   # 负零写成 0


def csv_rows(side, name, measured):
    rows = []
    for stem in sorted(measured):
        song, whole, per_drop, drops, reason = measured[stem]
        base = {'side': side, 'name': name, 'song': song, 'product': stem}
        if reason:
            rows.append(dict(base, na_reason=reason))
            continue
        for k, ((start, end), values) in enumerate(zip(drops, per_drop), start=1):
            row = dict(base, drop=k, first=int(k == 1), start_s=fmt(start, 3), end_s=fmt(end, 3))
            row.update({f: fmt(whole[f], 3) for f in SONG_FIELDS})
            if values is None:
                row['na_reason'] = 'drop 在追踪之外（歌比标注短？）'
            else:
                row.update({f: fmt(values[f], 3) for f in DROP_FIELDS})
            rows.append(row)
    return rows


def render_csv(rows):
    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=CSV_COLUMNS, lineterminator='\n')
    writer.writeheader()
    for row in rows:
        writer.writerow({c: row.get(c, '') for c in CSV_COLUMNS})
    return out.getvalue()


def totals(measured, first_only):
    """所有算得出的 drop 加总：退让次数、退让量、增益总变差；前段增益取平均；非主段放退让按整首加总。"""
    count = retreats = 0
    retreat_db = tv = 0.0
    early = []
    release = 0.0
    for stem in sorted(measured):
        _, whole, per_drop, _, reason = measured[stem]
        if reason:
            continue
        release += whole['non_main_release_db']
        for k, values in enumerate(per_drop, start=1):
            if values is None or (first_only and k != 1):
                continue
            count += 1
            retreats += values['retreats']
            retreat_db += values['retreat_db']
            tv += values['gain_tv_db']
            early.append(values['early_gain_db'])
    return {'drops': count, 'retreats': retreats, 'retreat_db': retreat_db, 'gain_tv_db': tv,
            'early_gain_db': mean(early), 'non_main_release_db': release}


def render_markdown(a_name, a, b_name, b):
    lines = ['# 逐 drop 报告（T-113 听感批）', '',
             f'- A（基线）：{a_name}' + (f'；B（候选）：{b_name}' if b is not None else '（只有一侧）'),
             f'- 退让 = guard_db 连续下降的一段（停不到 {MERGE_GAP_S:g} s 并成一次）；增益总变差 = Σ|Δ limit_gain_db|；'
             f'前段增益 = drop 头 {EARLY_BARS} 小节 limit_gain_db 的平均；非主段放退让 = 主段度 < {MAINNESS_MAIN:g} 时 guard_db 上升的总量',
             '- 口径与用法：tools/render/drop_report.py 文件头；设计：docs/validation/2026-11-13-T113-listening/README.md', '']
    sides = [(a_name, a)] + ([(b_name, b)] if b is not None else [])
    lines += ['## 合计', '', '| 范围 | 侧 | drop 数 | 退让次数 | 退让量 dB | 增益总变差 dB | 前段增益 dB | 非主段放退让 dB |', '|---|---|---|---|---|---|---|---|']
    for label, first_only in (('全部 drop', False), ('第一个 drop', True)):
        for name, measured in sides:
            t = totals(measured, first_only)
            lines.append(f"| {label} | {name} | {t['drops']} | {t['retreats']} | {fmt(t['retreat_db'])} | {fmt(t['gain_tv_db'])} | "
                         f"{fmt(t['early_gain_db'])} | {fmt(t['non_main_release_db']) if not first_only else ''} |")
    lines += ['', '## 逐首逐 drop', '']
    header = '| 成品 | drop | 区间 s | 主段占比 | 退让次数 | 退让量 dB | 增益总变差 dB | 前段增益 dB | 进 drop guard dB |'
    if b is None:
        lines += [header, '|---|---|---|---|---|---|---|---|---|']
    else:
        lines += ['每格写「A → B」。', '', header, '|---|---|---|---|---|---|---|---|---|']
    notes = []
    for stem in sorted(a):
        song, whole, per_drop, drops, reason = a[stem]
        other = b.get(stem) if b is not None else None
        if reason:
            notes.append(f'- {stem}：{reason}')
            continue
        for k, ((start, end), values) in enumerate(zip(drops, per_drop), start=1):
            if values is None:
                notes.append(f'- {stem} drop {k}：在追踪之外')
                continue
            theirs = other[2][k - 1] if other is not None and len(other[2]) >= k else None

            def pair(field, digits=2):
                mine = fmt(values[field], digits)
                return mine if theirs is None else f'{mine} → {fmt(theirs[field], digits)}'
            lines.append(f"| {stem} | {k} | {fmt(start, 1)}–{fmt(end, 1)} | {pair('main_share')} | {pair('retreats')} | "
                         f"{pair('retreat_db')} | {pair('gain_tv_db')} | {pair('early_gain_db')} | {pair('entry_guard_db')} |")
    if notes:
        lines += ['', '## 算不出的', ''] + notes
    return '\n'.join(lines) + '\n'


def run_compare(args):
    if args.b is not None and Path(args.a).resolve() == Path(args.b).resolve():
        raise UsageError('--a 与 --b 是同一个目录')
    sections = load_sections(args)
    a = measure_side(args.a, sections)
    b = measure_side(args.b, sections) if args.b is not None else None
    if b is not None and sorted(a) != sorted(b):
        only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
        raise ToolError(f'A、B 的成品对不上：只在 A {only_a[:5]}，只在 B {only_b[:5]}（两侧要用同一份网格渲染）')
    rows = csv_rows('A', args.a_name, a) + (csv_rows('B', args.b_name, b) if b is not None else [])
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'drop_report.md').write_text(render_markdown(args.a_name, a, args.b_name, b), encoding='utf-8')
    (out / 'drops.csv').write_text(render_csv(rows), encoding='utf-8')
    print(f'drop_report：{len(a)} 份成品追踪 → {out / "drop_report.md"}、{out / "drops.csv"}')


# ---------------------------------------------------------------------------------------------------------------------
# windows：盲听片段（只看段落，不看渲染结果）

def windows_for(bar_s, drops, before_bars, after_bars, per_song):
    """drop 区间 → 至多 per_song 个 [drop 前 before 小节, drop 后 after 小节] 窗口（秒），按顺序取、互不重叠；第一个 drop 必在。"""
    picked = []
    for start, _ in drops:
        lo, hi = start - before_bars * bar_s, start + after_bars * bar_s
        if lo < 0.0 or (picked and lo < picked[-1][1]):
            continue
        picked.append((round(lo, 6), round(hi, 6)))
        if len(picked) == per_song:
            break
    return picked


def run_windows(args):
    if args.before_bars < 1 or args.after_bars < 1 or not 8 <= args.before_bars + args.after_bars <= 16:
        raise UsageError('--before-bars / --after-bars 各至少 1，合计 8～16 小节（ADR-034：片段 8～16 小节）')
    if args.per_song < 1:
        raise UsageError('--per-song 至少 1')
    sections = load_sections(args)
    tracks = [t for t in args.tracks.split(',') if t] if args.tracks else sorted(sections)
    result, skipped = {}, []
    for track in tracks:
        if track not in sections:
            skipped.append(f'{track}：没有段落')
            continue
        bar_s, drops = sections[track]
        picked = windows_for(bar_s, drops, args.before_bars, args.after_bars, args.per_song)
        if not drops or not picked or abs(picked[0][0] - (drops[0][0] - args.before_bars * bar_s)) > 1e-6:
            skipped.append(f'{track}：第一个 drop 前不够 {args.before_bars} 小节或没有 drop')
            continue
        if len(picked) < args.per_song:
            skipped.append(f'{track}：只放得下 {len(picked)} 个窗口（要 {args.per_song} 个；blind_test 照收，试次会少）')
        result[track] = [list(w) for w in picked]
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    print(f'windows：{len(result)} 首、{sum(len(v) for v in result.values())} 个窗口 → {args.out}')
    for line in skipped:
        print(f'  注意 {line}')


# ---------------------------------------------------------------------------------------------------------------------
# 自检

def write_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        writer = csv.writer(f, lineterminator='\n')
        writer.writerow(header)
        writer.writerows(rows)


def synthetic_trace(path, guard_at, gain_at, mainness_at):
    """0～28 s 每 0.25 s 一块的成品追踪（多带几列无关的列，验证按列名读）。"""
    rows = []
    for i in range(113):
        t = i * 0.25
        rows.append([f'{t:.4f}', '0.500', f'{gain_at(t):.3f}', '0.0000', f'{guard_at(t):.3f}', f'{mainness_at(t):.3f}', 'none'])
    write_csv(path, ['t_sec', 'limit_gr_db', 'limit_gain_db', 'applied_bias_db', 'guard_db', 'current_mainness', 'guard_cause'], rows)


def selftest():
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))
        print(('ok    ' if ok else 'FAIL  ') + what)

    with tempfile.TemporaryDirectory() as work:
        root = Path(work)
        # 标注：120 BPM（小节 2 s）：intro 1–4（0–8 s）、drop 5–8（8–16 s）、breakdown 9–10（16–20 s）、drop 11–14（20–28 s）
        annotation = {'song': 'syn', 'bpm': 120, 'meter': '4/4', 'sections': [
            {'name': 'intro', 'kind': 'intro', 'start_bar': 1, 'end_bar': 4}, {'name': 'drop1', 'kind': 'drop', 'start_bar': 5, 'end_bar': 8},
            {'name': 'breakdown', 'kind': 'breakdown', 'start_bar': 9, 'end_bar': 10}, {'name': 'drop2', 'kind': 'drop', 'start_bar': 11, 'end_bar': 14}]}
        (root / 'ann').mkdir()
        (root / 'ann' / 'syn.json').write_text(json.dumps(annotation), encoding='utf-8')
        def ramp_down(t, start):   # start 起 1 s 内从 0 线性退到 −1
            return -min(max(t - start, 0.0), 1.0)

        def mainness_at(t):        # drop1、drop2 是主段，intro、breakdown 不是
            return 1.0 if (8.0 <= t < 16.0 or t >= 20.0) else 0.0

        def guard_a(t):            # A：drop1 里 9～10 s 退让到 −1；breakdown 里 2 s 放回 0（非主段放退让 1 dB）；drop2 里 21～22 s 又退：两次
            if t < 16.0:
                return ramp_down(t, 9.0)
            if t < 20.0:
                return -max(0.0, 1.0 - (t - 16.0) / 2.0)
            return ramp_down(t, 21.0)

        def guard_b(t):            # B（主段门）：drop1 同样退让，breakdown 里停在 −1，drop2 不再退：一次，非主段放退让 0
            return ramp_down(t, 9.0)

        def gain_a(t):             # 12 s 一步 +1 dB
            return 2.0 if t < 12.0 else 3.0

        def gain_b(t):
            return 3.0

        synthetic_trace(root / 'A' / 'syn' / 'trace' / 'syn__g35_clay-auto_auto.csv', guard_a, gain_a, mainness_at)
        write_csv(root / 'A' / 'syn' / 'trace' / 'syn__g35_clay-auto_auto__listen.csv', ['t_sec'], [['0']])   # 第一遍的要跳过
        synthetic_trace(root / 'B' / 'syn' / 'trace' / 'syn__g35_clay-auto_auto.csv', guard_b, gain_b, mainness_at)

        args = argparse.Namespace(a=str(root / 'A'), b=str(root / 'B'), a_name='current', b_name='c105', annotations=[str(root / 'ann')],
                                  labels=None, labels_override=None, tempo=None, out=str(root / 'out'))
        run_compare(args)
        with open(root / 'out' / 'drops.csv', encoding='utf-8') as f:
            rows = {(r['side'], r['drop']): r for r in csv.DictReader(f)}
        a1, a2, b1, b2 = rows[('A', '1')], rows[('A', '2')], rows[('B', '1')], rows[('B', '2')]
        check((a1['retreats'], a2['retreats'], b1['retreats'], b2['retreats']) == ('1', '1', '1', '0'), 'A 两个 drop 各退让一次、B 只在 drop1 退让一次')
        check(a1['retreat_db'] == '1.000' and a1['retreats_total'] == '2' and b1['retreats_total'] == '1', '退让量 1 dB、整首退让次数 2 对 1')
        check(abs(float(a1['non_main_release_db']) - 1.0) < 0.01 and b1['non_main_release_db'] == '0.000', '非主段放退让：A 约 1 dB、B 为 0')
        check(a1['gain_tv_db'] == '1.000' and b1['gain_tv_db'] == '0.000' and a2['gain_tv_db'] == '0.000', '增益总变差：A 的 drop1 里一步 +1 dB')
        check(a1['early_gain_db'] == '2.500' and a1['main_share'] == '1.000' and a1['start_s'] == '8.000' and a1['end_s'] == '16.000',
              'drop1 区间 8～16 s、前段增益（8 小节 = 整段）平均 2.5 dB、主段占比 1')
        check(a2['entry_guard_db'] == '0.000' and b2['entry_guard_db'] == '-1.000', '进 drop2 时 A 已放回 0、B 停在 −1')
        first = (root / 'out' / 'drop_report.md').read_text(encoding='utf-8') + (root / 'out' / 'drops.csv').read_text(encoding='utf-8')
        run_compare(args)
        again = (root / 'out' / 'drop_report.md').read_text(encoding='utf-8') + (root / 'out' / 'drops.csv').read_text(encoding='utf-8')
        check(first == again and '| 第一个 drop | c105 | 1 | 1 |' in first, '重跑逐字相同；合计表有第一个 drop 一行')

        # 逐小节标签：transition 两侧各 1 小节，交界取中间；速度 120（小节 2 s）
        labels = ['class\ttrack\tbar\tlabel']
        kinds = ['intro'] * 5 + ['transition', 'transition'] + ['drop'] * 6 + ['transition', 'transition'] + ['breakdown'] * 4 \
            + ['transition', 'transition'] + ['drop'] * 6 + ['outro'] * 2
        labels += [f'deep\ttr\t{bar}\t{kind}' for bar, kind in enumerate(kinds, start=1)]
        (root / 'labels.tsv').write_text('\n'.join(labels) + '\n', encoding='utf-8')
        (root / 'tempo.tsv').write_text('class\ttrack\tbpm\ndeep\ttr\t120\n', encoding='utf-8')
        wargs = argparse.Namespace(annotations=None, labels=str(root / 'labels.tsv'), labels_override=None, tempo=str(root / 'tempo.tsv'),
                                   out=str(root / 'seg.json'), tracks=None, before_bars=WINDOW_BEFORE_BARS, after_bars=WINDOW_AFTER_BARS,
                                   per_song=WINDOWS_PER_SONG)
        bar_s, drops = load_sections(wargs)['tr']
        # drop 小节 8～13，前面 transition 6～7 → 起点第 7 小节（12 s）；后面 transition 14～15 → 终点第 14 小节末（28 s）
        # 第二个 drop 小节 22～27，前面 transition 20～21 → 起点第 21 小节（40 s）；后面直接接 outro → 终点第 27 小节末（54 s）
        check(bar_s == 2.0 and drops[0] == (12.0, 28.0) and drops[1] == (40.0, 54.0), f'标签 → drop 区间取交界中线（{drops}）')
        run_windows(wargs)
        windows = json.loads((root / 'seg.json').read_text(encoding='utf-8'))
        check(windows == {'tr': [[4.0, 28.0], [32.0, 56.0]]}, f'windows：第一个 drop 前 4 后 8 小节，第二个不重叠（{windows}）')

        # 坏输入：缺列、A / B 对不上、没有追踪 → 基础设施错；--a 与 --b 同一目录 → 参数错
        write_csv(root / 'C' / 'trace' / 'syn__g35_clay-auto_auto.csv', ['t_sec', 'limit_gain_db'], [['0', '0']])
        write_csv(root / 'D' / 'trace' / 'other__g35_clay-auto_auto.csv', ['t_sec'], [['0']])
        (root / 'E').mkdir()
        outcomes = []
        for a_dir, b_dir, expected in (('C', None, ToolError), ('A', 'D', ToolError), ('E', None, ToolError), ('A', 'A', UsageError)):
            bad = argparse.Namespace(**dict(vars(args), a=str(root / a_dir), b=str(root / b_dir) if b_dir else None, out=str(root / 'bad')))
            try:
                run_compare(bad)
                outcomes.append(False)
            except expected:
                outcomes.append(True)
        check(all(outcomes) and not (root / 'bad').exists(), '缺列、A / B 对不上、没有追踪、两侧同目录都报错且不写输出')

    failed = sum(1 for ok, _ in checks if not ok)
    print(f'drop_report selftest：{len(checks)} 项，失败 {failed}')
    return 1 if failed else 0


# ---------------------------------------------------------------------------------------------------------------------

def add_section_args(parser):
    parser.add_argument('--annotations', nargs='+', help='段落标注 JSON 或目录（make_synthetic_house.py 写的那种）')
    parser.add_argument('--labels', help='T-038 逐小节标签 labels-auto.tsv（Jamendo）')
    parser.add_argument('--labels-override', help='T-038 改标 labels-override.tsv（可选）')
    parser.add_argument('--tempo', help='T-038 tempo-manifest.tsv（配 --labels）')


def main(argv):
    if argv == ['--selftest']:
        return selftest()
    parser = argparse.ArgumentParser(description='T-113 逐 drop 报告（见文件头）')
    sub = parser.add_subparsers(dest='command', required=True)
    compare = sub.add_parser('compare', help='基线对候选：逐 drop 退让次数、增益总变差')
    compare.add_argument('--a', required=True, help='基线的 celadon_render 输出目录（递归找 trace/*.csv）')
    compare.add_argument('--b', help='候选的输出目录（不给 = 只报 A）')
    compare.add_argument('--a-name', default='current', help='报告里 A 的名字')
    compare.add_argument('--b-name', default='candidate', help='报告里 B 的名字')
    compare.add_argument('--out', required=True, help='报告目录（写 drop_report.md 与 drops.csv）')
    add_section_args(compare)
    windows = sub.add_parser('windows', help='盲听片段 JSON（blind_test.py build --segments）')
    windows.add_argument('--out', required=True, help='写出的 segments.json')
    windows.add_argument('--tracks', help='只要这些歌（逗号分隔）')
    windows.add_argument('--before-bars', type=int, default=WINDOW_BEFORE_BARS)
    windows.add_argument('--after-bars', type=int, default=WINDOW_AFTER_BARS)
    windows.add_argument('--per-song', type=int, default=WINDOWS_PER_SONG)
    add_section_args(windows)
    args = parser.parse_args(argv)
    try:
        if bool(args.annotations) == bool(args.labels) or (args.labels and not args.tempo):
            raise UsageError('段落来源二选一：--annotations，或 --labels + --tempo')
        (run_compare if args.command == 'compare' else run_windows)(args)
    except UsageError as error:
        print(f'drop_report：{error}', file=sys.stderr)
        return 2
    except ToolError as error:
        print(f'drop_report：{error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
