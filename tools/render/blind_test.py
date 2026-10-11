#!/usr/bin/env python3
"""T-096b R5-02 轻量盲测脚本（含 R1-05 离线归一化 A/B）：出盲测包、算答卷统计、自检。

规则出处：ADR-034 决定 4（盲测协议）、T-096 计划 §13 Q13 / Q14。流程与第一批清单见
docs/validation/2026-10-11-T096b-blind-test/README.md。

  build     把 A / B（可选原曲 O）两套同名渲染输出切成盲测包：固定种子切片 → 等响记录 → 随机改名 → 试次表 / 会话表 / 答卷模板；
            登记文件 registration.json（决策规则、种子、片段边界、等响容差、脚本 sha256）与答案钥匙 key.json 单独存放。--dry-run 只预演、不写任何文件。
  score     读答卷 + key.json + registration.json：2AFC 排除「无偏好」后做符号检验，ABX 做单侧二项检验；输出 result.md / result.json（不带时间戳，同输入逐字相同）。
  selfcheck 合成素材跑 build → 程序化 A/A 检查（同一文件两侧）→ 统计已知值（12/16 → p = 0.038、26/40 → p = 0.040）→ 重跑逐字相同。

边界（计划 §13 Q13）：出包、登记、统计、自检全在 DGX 上跑；Mac 只播放 package/ 里的片段、编辑 answers.csv，不跑脚本；
key.json 与 registration.json 不下发到 Mac，直到答卷交回。音频不进仓库（DGX ~/.cache/celadon-blind/<批次>/），
只有登记、试次表、答卷、统计结果、报告进 docs/validation/。

依赖：score 只用标准库（math.comb）；build / selfcheck 另需 numpy / scipy / soundfile / pyloudnorm
（版本见 docs/validation/2026-10-08-T085-codec-tp/requirements.txt）。响度 / 峰值的量具只有 codec_tp_check.measure 一份，这里 import、不另抄。
出错规则（Q12）：基础设施错（读不到文件、缺列、缺依赖）→ 退出码 1；参数不合法 → 2；素材原因算不出（没有 breakdown→drop、不够长）→ 空值 + na_reason，不填 0。
工具只报告、不改任何源文件，也不回退增益。
"""
import argparse
import contextlib
import csv
import hashlib
import io
import json
import math
import random
import re
import struct
import sys
import tempfile
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
sys.path.insert(0, str(SCRIPT_PATH.parent))   # 复用同目录的 codec_tp_check（量具）与 calibrate_house（标签读取）

# ---------------------------------------------------------------------------------------------------------------------
# 常量：协议数字和阈值全在这里（改协议只改这一处；登记文件会把生效的值写进去）

FORMAT_VERSION = 1
MODES = ('2afc', 'abx')
CONDITIONS = ('A', 'B', 'O')                    # A / B：两套渲染；O：原曲（R1-05 三方归一化）
MODE_DEFAULTS = {'2afc': {'trials': 18, 'sessions': 2}, 'abx': {'trials': 40, 'sessions': 3}}   # ADR-034 决定 4
FILE_SLOTS = {'2afc': ('1', '2'), 'abx': ('a', 'b', 'x')}      # 一个试次里的文件位
ANSWER_CHOICES = {'2afc': ('1', '2', 'none'), 'abx': ('a', 'b')}
MAX_TRIALS_PER_SESSION = 15                     # BS.1116：会话 10～15 次
MAX_SESSION_MINUTES = 30.0                      # BS.1116：会话 ≤ 20～30 min（只对估算值发警告）
REPLAY_FACTOR = 1.5                             # 估时：每个文件平均听 1.5 遍
TRIAL_OVERHEAD_S = {'2afc': 15.0, 'abx': 20.0}  # 估时：每试次的切换与作答秒数

DEFAULT_BARS, BARS_MIN, BARS_MAX = 12, 8, 16    # 片段长度（小节），ADR-034 决定 4：8～16 小节
DEFAULT_SEGMENTS_PER_SONG = 3                   # 每首 3～4 片段
BEATS_PER_BAR = 4
MIN_SIDE_BARS = 2                               # 窗口里 breakdown 与 drop 各至少这么多「核心」小节（不含交界的 transition）
MAX_GAP_BARS = 2                                # breakdown 与 drop 之间最多隔这么多个 transition 小节
MIN_SHIFT_BARS = 2                              # 同一边界上再取窗口时，起点至少错开这么多小节
SEGMENT_MIN_SECONDS = 4.0                       # codec_tp_check.measure 要求 ≥ 3 s，留余量
FADE_MS_DEFAULT, FADE_MS_MIN = 25.0, 20.0       # 淡入淡出 ≥ 20 ms

LOUDNESS_TOLERANCE_LU = 0.1                     # 登记的等响容差
SENSITIVE_LU = 0.3                              # 片段两侧响度差超过它 → 标「敏感性」
WARN_LU = 1.0                                   # 超过它 → 警告（都不拒绝出包）
MIN_PREFERENCE = 0.65                           # 2AFC 通过线：偏好候选 ≥ 65 %
ALPHA = 0.05                                    # 单侧显著性
DELIVERY_TOKENS = ('streaming', 'club')         # 渲染文件名里的交付档记号（默认档不带记号）
SUBCLASS_TOKENS = ('deep', 'tech', 'melodic')
COVERAGE_CELLS = tuple((sub, delivery) for sub in SUBCLASS_TOKENS for delivery in DELIVERY_TOKENS)

VERDICT_TEXT = {
    'pass': '通过登记的线（偏好部分；B 组不退步由完成报告另判）',
    'fail': '没过登记的线',
    'incomplete': '答卷不完整：不下结论（下面数字只是中途读数）',
    'aa-no-leak': 'A/A 包：与 50 % 无显著差异，无泄漏迹象',
    'aa-suspected-leak': 'A/A 包：显著偏离 50 %，疑似有泄漏线索或位置偏好（先查包再用）',
}


class ToolError(Exception):
    """基础设施错：文件读不到、列缺、依赖缺失、两侧对不上……退出码 1。"""


class UsageError(ToolError):
    """命令行参数不合法：退出码 2。"""


# ---------------------------------------------------------------------------------------------------------------------
# 小工具（读写文件的副作用都集中在这几个函数里）

def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def write_text(path, text):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def write_json(path, data):
    write_text(path, json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + '\n')


def write_table(path, header, rows, delimiter=','):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        writer = csv.writer(f, delimiter=delimiter, lineterminator='\n')
        writer.writerow(header)
        writer.writerows(rows)


def read_json(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError) as error:
        raise ToolError(f'读不了 {path}：{error}')


def read_rows(path, delimiter=','):
    """CSV / TSV → 行字典列表；'#' 开头的行是注释。utf-8-sig 兼容表格软件存出来的 BOM。"""
    try:
        with open(path, encoding='utf-8-sig', newline='') as f:
            return list(csv.DictReader((line for line in f if not line.startswith('#')), delimiter=delimiter))
    except OSError as error:
        raise ToolError(f'读不了 {path}：{error}')


def require_columns(rows, columns, path):
    if rows:
        missing = [c for c in columns if c not in rows[0]]
        if missing:
            raise ToolError(f'{path} 缺列：{missing}')


def finite_or_none(value, digits=4):
    return round(float(value), digits) if value is not None and math.isfinite(value) else None


def fmt_number(value, digits=4):
    return '' if value is None else f'{value:.{digits}f}'


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


# ---------------------------------------------------------------------------------------------------------------------
# 统计（纯函数、只用标准库）：X ~ Binomial(n, 0.5)

def _tail_ge(k, n):
    """P(X ≥ k) 的分子（分母 2**n）。整数运算，没有浮点误差。"""
    return sum(math.comb(n, i) for i in range(max(k, 0), n + 1))


def _tail_le(k, n):
    return sum(math.comb(n, i) for i in range(0, min(k, n) + 1)) if k >= 0 else 0


def binom_p_ge(k, n):
    """单侧精确二项检验 P(X ≥ k | n, 0.5)；n = 0 → None。12/16 → 0.0384，26/40 → 0.0403。"""
    return None if n <= 0 else _tail_ge(k, n) / 2 ** n


def binom_p_two_sided(k, n):
    """双侧（加倍法）；只当参考，不进登记的通过线。"""
    if n <= 0:
        return None
    return min(2 * min(_tail_ge(k, n), _tail_le(k, n)), 2 ** n) / 2 ** n


def min_wins(n, alpha=ALPHA, min_preference=0.0):
    """n 个有效试次里，单侧 p < alpha（且偏好 ≥ min_preference）所需的最少胜场；达不到 → None。"""
    if n <= 0:
        return None
    for k in range(n + 1):
        if k / n >= min_preference and _tail_ge(k, n) / 2 ** n < alpha:
            return k
    return None


# ---------------------------------------------------------------------------------------------------------------------
# 随机：每种用途一路独立的种子化随机流，全部只用 rng.random()（它的流在各 Python 版本间一致）

def derive_rng(seed, *purpose):
    return random.Random(':'.join([str(seed), *map(str, purpose)]))   # 字符串种子按 sha512 展开，跨版本稳定


def shuffled(items, rng):
    """Fisher-Yates。不用 random.shuffle：它的内部实现在 Python 版本间改过，会让同一种子出不同顺序。"""
    out = list(items)
    for i in range(len(out) - 1, 0, -1):
        j = int(rng.random() * (i + 1))
        out[i], out[j] = out[j], out[i]
    return out


def balanced_flags(count, extra_to_true, rng):
    """count 个布尔值，真假各半（奇数时多出的一个给 extra_to_true 指定的一方），顺序打乱。"""
    n_true = count // 2 + (1 if (count % 2 and extra_to_true) else 0)
    return shuffled([True] * n_true + [False] * (count - n_true), rng)


# ---------------------------------------------------------------------------------------------------------------------
# 片段：从 T-038 的逐小节标签里找 breakdown → drop 边界，切 8～16 小节的窗口

def load_tempo(path):
    """T-038 tempo-manifest.tsv → ({歌: BPM}, {歌: 子类})。"""
    rows = read_rows(path, '\t')
    require_columns(rows, ('track', 'bpm'), path)
    return {r['track']: float(r['bpm']) for r in rows}, {r['track']: r.get('class', '') for r in rows}


def load_bar_kinds(labels_path, override_path):
    """T-038 逐小节标签（可叠改标）→ {歌: {小节: 标签}}。复用 calibrate_house.load_labels，不另抄改标与交界规则。"""
    try:
        import calibrate_house
        labels = calibrate_house.load_labels(None, override_path, labels_path)
    except (ImportError, OSError, KeyError, ValueError) as error:
        raise ToolError(f'读不了段落标签 {labels_path}：{error}')
    kinds = {}
    for (track, bar), label in labels.items():
        kinds.setdefault(track, {})[bar] = label
    return kinds


def load_explicit_segments(path):
    """--segments JSON：{"歌名": [[起点秒, 终点秒], ...]}（Max 自己给 BPM / 区间的歌，如 22 首）。"""
    data = read_json(path)
    if not isinstance(data, dict):
        raise ToolError(f'{path}：顶层要写成 {{歌名: [[起点秒, 终点秒], ...]}}')
    segments = {}
    for song, spans in data.items():
        try:
            parsed = [(float(start), float(end)) for start, end in spans]
        except (TypeError, ValueError):
            raise ToolError(f'{path}：{song} 的区间要写成 [[起点秒, 终点秒], ...]')
        for start, end in parsed:
            if start < 0 or end - start < SEGMENT_MIN_SECONDS:
                raise ToolError(f'{path}：{song} 区间 [{start}, {end}] 不合法（起点 ≥ 0 且长度 ≥ {SEGMENT_MIN_SECONDS} s）')
        segments[song] = parsed
    return segments


def find_boundaries(kinds):
    """一首歌的逐小节标签 → [(b0, b1, d0, d1)]：breakdown 段 [b0, b1]，隔 0～MAX_GAP_BARS 个 transition，再是 drop 段 [d0, d1]（小节号，闭区间）。
    T-038 在段落交界前后各标 1 个 transition，所以真实边界两边通常隔着 2 个 transition 小节。"""
    boundaries = []
    for d0 in sorted(kinds):
        if kinds[d0] != 'drop' or kinds.get(d0 - 1) == 'drop':
            continue                                        # 只看 drop 段的第一小节
        b1 = d0 - 1
        while kinds.get(b1) == 'transition':
            b1 -= 1
        if kinds.get(b1) != 'breakdown' or d0 - 1 - b1 > MAX_GAP_BARS:
            continue
        b0 = b1
        while kinds.get(b0 - 1) == 'breakdown':
            b0 -= 1
        d1 = d0
        while kinds.get(d1 + 1) == 'drop':
            d1 += 1
        boundaries.append((b0, b1, d0, d1))
    return boundaries


def start_range(boundary, bars):
    """窗口 [s, s + bars - 1] 要整个落在 breakdown 段起点到 drop 段终点之间，且两侧各含 ≥ MIN_SIDE_BARS 个核心小节
    → 可取的起点范围 (lo, hi)；放不下 → None。"""
    b0, b1, d0, d1 = boundary
    lo = max(b0, d0 + MIN_SIDE_BARS - bars)
    hi = min(b1 - MIN_SIDE_BARS + 1, d1 - bars + 1)
    return (lo, hi) if lo <= hi else None


def pick_windows(boundaries, bars, count, rng):
    """从各边界取 count 个窗口 → [(边界序号, 起点小节)]（按起点排序）；凑不够 → None。
    第一轮每个边界取一个居中的窗口（互不重叠）。T-038 的 45 首里多数歌只有 1 个边界，不够时第二轮在同一边界上把起点错开 ≥ MIN_SHIFT_BARS
    小节补：这些窗口会互相重叠、听的是同一段音乐的不同切法，登记里记下共享的边界，统计时当重复测量看。"""
    ranges = []
    for index, boundary in enumerate(boundaries):
        found = start_range(boundary, bars)
        if found:
            ranges.append((index, boundary[2], found[0], found[1]))
    picked = []
    for index, d0, lo, hi in shuffled(ranges, rng):
        start = min(max(d0 - bars // 2, lo), hi)
        if len(picked) < count and all(abs(start - s) >= bars for _, s in picked):
            picked.append((index, start))
    while len(picked) < count:
        best = None                                         # ((与同边界已取窗口的最小间距, -起点, -序号), 序号, 起点)；间距越大越好
        for index, d0, lo, hi in ranges:
            for start in range(lo, hi + 1):
                if any(i != index and abs(start - s) < bars for i, s in picked):
                    continue                                # 不跟别的边界的窗口重叠
                gap = min((abs(start - s) for i, s in picked if i == index), default=bars)
                if gap >= MIN_SHIFT_BARS and (best is None or (gap, -start, -index) > best[0]):
                    best = ((gap, -start, -index), index, start)
        if best is None:
            return None
        picked.append((best[1], best[2]))
    return sorted(picked, key=lambda item: (item[1], item[0]))


def bar_window_segments(picked, boundaries, bars, bpm):
    """窗口（小节）→ 片段（秒）。T-038 的 wav 已裁到第一个小节线，第 N 小节从 (N-1) × 小节秒数开始。"""
    bar_seconds = BEATS_PER_BAR * 60.0 / bpm
    return [{'start_bar': start, 'end_bar': start + bars - 1, 'boundary_bar': boundaries[index][2], 'boundary': index,
             'start_s': round((start - 1) * bar_seconds, 6), 'end_s': round((start + bars - 1) * bar_seconds, 6)}
            for index, start in picked]


def plan_song(song, probe, args, tempo, kinds, explicit):
    """一首歌的片段 → (segments, na_reason)；算不出就返回 ([], 原因)，由调用方记进 skipped（不填 0）。"""
    if song in explicit:
        segments = [{'start_bar': None, 'end_bar': None, 'boundary_bar': None, 'boundary': None,
                     'start_s': start, 'end_s': end} for start, end in explicit[song]]
    else:
        if song not in tempo or song not in kinds:
            return [], '速度表或段落标签里没有这首歌（也没给 --segments）'
        boundaries = find_boundaries(kinds[song])
        if not boundaries:
            return [], '段落标签里没有 breakdown→drop 边界'
        picked = pick_windows(boundaries, args.bars, args.segments_per_song, derive_rng(args.seed, 'windows', song))
        if picked is None:
            return [], (f'{len(boundaries)} 个边界放不下 {args.segments_per_song} 个 {args.bars} 小节窗口'
                        f'（两侧各 ≥ {MIN_SIDE_BARS} 个核心小节，同边界起点错开 ≥ {MIN_SHIFT_BARS} 小节）')
        segments = bar_window_segments(picked, boundaries, args.bars, tempo[song])
    for segment in segments:
        if segment['end_s'] - segment['start_s'] < SEGMENT_MIN_SECONDS:
            return [], f'片段不足 {SEGMENT_MIN_SECONDS} s'
        if segment['end_s'] > probe['seconds']:
            return [], f'片段终点 {segment["end_s"]:.2f} s 超出歌长 {probe["seconds"]:.2f} s'
    return segments, ''


def describe_song(song, file_name, classes):
    """子类（速度表的 class 列，缺则看文件名记号）与交付档（文件名记号，没有 = standard）。"""
    stem = Path(file_name).stem
    tokens = re.split(r'[_-]', stem.split('__', 1)[1]) if '__' in stem else []
    subclass = classes.get(song) or next((t for t in SUBCLASS_TOKENS if t in tokens), '')
    delivery = next((t for t in DELIVERY_TOKENS if t in tokens), 'standard')
    return subclass, delivery


def coverage_gaps(plans):
    """协议要求 Deep / Tech / Melodic × 流媒体 / 俱乐部都有歌；子类全未知（没速度表）时不判，返回 None。"""
    if not any(p['class'] for p in plans.values()):
        return None
    cells = {(p['class'], p['delivery']) for p in plans.values()}
    return [f'{sub}×{delivery}' for sub, delivery in COVERAGE_CELLS if (sub, delivery) not in cells]


# ---------------------------------------------------------------------------------------------------------------------
# 试次编排（纯逻辑）

def order_units(units, rng):
    """按歌轮流取片段（歌的顺序随机），让试次数在各歌之间尽量均匀。"""
    by_song = {}
    for unit in units:
        by_song.setdefault(unit['song'], []).append(unit)
    songs = shuffled(sorted(by_song), rng)
    longest = max(len(items) for items in by_song.values())
    return [by_song[song][k] for k in range(longest) for song in songs if k < len(by_song[song])]


def session_sizes(n_trials, n_sessions):
    base, extra = divmod(n_trials, n_sessions)
    return [base + (1 if i < extra else 0) for i in range(n_sessions)]


def plan_trials(units, mode, n_trials, n_sessions, rng):
    """→ 按会话顺序排好的试次 [{'id','session','unit','cand_first','x_is_a'}]。
    片段不够试次数时循环复用（同一片段再听一次，侧位另行平衡）；每个会话里候选在前 / 后各半、ABX 的 X 是 a / b 各半，
    奇数大小的会话轮流把多出的一个给「真」或「假」，所以整批的不平衡也不超过 1。"""
    ordered = order_units(units, rng)
    chosen = shuffled([ordered[i % len(ordered)] for i in range(n_trials)], rng)
    width = max(2, len(str(n_trials)))
    trials, cursor, odd_extra_true = [], 0, True
    for session, size in enumerate(session_sizes(n_trials, n_sessions), start=1):
        cand_first = balanced_flags(size, odd_extra_true, rng)
        x_is_a = balanced_flags(size, not odd_extra_true, rng)
        if size % 2:
            odd_extra_true = not odd_extra_true
        for i in range(size):
            cursor += 1
            trials.append({'id': f't{cursor:0{width}d}', 'session': session, 'unit': chosen[cursor - 1],
                           'cand_first': cand_first[i], 'x_is_a': x_is_a[i]})
    return trials


def trial_sides(trial, mode, base, cand):
    """文件位 → 条件（ABX 的 x 位复制 a 或 b 的内容），以及 ABX 的 x_is（2AFC 为 None）。"""
    first, second = (cand, base) if trial['cand_first'] else (base, cand)
    if mode == '2afc':
        return {'1': first, '2': second}, None
    x_is = 'a' if trial['x_is_a'] else 'b'
    sides = {'a': first, 'b': second}
    sides['x'] = sides[x_is]
    return sides, x_is


def session_minutes(trials, mode):
    """估时：每个文件平均听 REPLAY_FACTOR 遍 + 每试次固定的切换 / 作答秒数。只作警告依据，不是计时。"""
    seconds = sum(len(FILE_SLOTS[mode]) * (t['unit']['end_s'] - t['unit']['start_s']) * REPLAY_FACTOR + TRIAL_OVERHEAD_S[mode]
                  for t in trials)
    return seconds / 60.0


def loudness_flag(abs_diff):
    """片段两侧响度差的档位：ok ≤ 容差 < above_tolerance ≤ 0.3 < sensitive ≤ 1 < warn。"""
    if abs_diff > WARN_LU:
        return 'warn'
    if abs_diff > SENSITIVE_LU:
        return 'sensitive'
    if abs_diff > LOUDNESS_TOLERANCE_LU:
        return 'above_tolerance'
    return 'ok'


def loudness_record(measures, base, cand):
    """候选减基线的片段响度差（带符号，只进 key.json）与它的档位；响度不可算 → 空值 + na_reason，不填 0。"""
    lufs_base, lufs_cand = measures[base]['lufs'], measures[cand]['lufs']
    if lufs_base is None or lufs_cand is None:
        return {'loudness_diff_lu': None, 'loudness_diff_abs_lu': None, 'loudness_flag': 'na',
                'na_reason': '片段响度不可算（静音或低于 -70 LUFS 绝对门）'}
    diff = round(lufs_cand - lufs_base, 4)
    return {'loudness_diff_lu': diff, 'loudness_diff_abs_lu': abs(diff), 'loudness_flag': loudness_flag(abs(diff)), 'na_reason': ''}


# ---------------------------------------------------------------------------------------------------------------------
# 音频读写与量具（副作用在这里）

def load_ruler():
    """响度 / 峰值的唯一量具：codec_tp_check.measure（pyloudnorm 的 BS.1770 + 8 倍过采样真峰值）。"""
    try:
        import codec_tp_check
    except ImportError as error:
        raise ToolError(f'缺依赖（{error}）：需要 numpy / scipy / soundfile / pyloudnorm，版本见 docs/validation/2026-10-08-T085-codec-tp/requirements.txt')
    return codec_tp_check


def measure_file(path):
    try:
        return load_ruler().measure(Path(path))
    except (ValueError, RuntimeError) as error:
        raise ToolError(f'量不了 {path}：{error}')


def probe_song(song, sources):
    """各条件该歌的采样率与长度 → ({'rate','seconds'}, '')；对不上 → (None, na_reason)（素材原因，不退出）。"""
    import soundfile as sf
    try:
        infos = {cond: sf.info(str(files[song])) for cond, files in sources.items()}
    except RuntimeError as error:
        raise ToolError(f'读不了 {song} 的 wav：{error}')
    rates = {info.samplerate for info in infos.values()}
    if len(rates) != 1:
        return None, '各条件采样率不一致：' + '、'.join(f'{cond}={info.samplerate}' for cond, info in sorted(infos.items()))
    return {'rate': rates.pop(), 'seconds': min(info.frames / info.samplerate for info in infos.values())}, ''


def read_segment(path, start_s, end_s):
    """读 [start_s, end_s) → (float64 立体声 (帧, 2), 采样率)。单声道复制成两声道，多于两声道报错。"""
    import numpy as np
    import soundfile as sf
    try:
        info = sf.info(str(path))
        start, stop = round(start_s * info.samplerate), round(end_s * info.samplerate)
        if stop > info.frames:
            raise ToolError(f'{path}：片段终点超出文件长度')
        audio, rate = sf.read(str(path), start=start, stop=stop, dtype='float64', always_2d=True)
    except RuntimeError as error:
        raise ToolError(f'读不了 {path}：{error}')
    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)
    elif audio.shape[1] != 2:
        raise ToolError(f'{path}：{audio.shape[1]} 声道，只支持单声道 / 立体声')
    return audio, rate


def apply_fade(audio, rate, fade_ms):
    """两端各一段升余弦淡入 / 淡出（A / B 两侧用同一条，不引入差别）。"""
    import numpy as np
    n = min(int(round(rate * fade_ms / 1000.0)), len(audio) // 2)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    faded = audio.copy()
    faded[:n] *= ramp[:, None]
    faded[len(faded) - n:] *= ramp[::-1][:, None]
    return faded


def write_wav_float32(path, audio, rate):
    """自己写 32-bit float WAV（RIFF + fmt + fact + data）。不用 soundfile.write：libsndfile 的 float WAV 会带 PEAK 块和 time(NULL) 时间戳，
    同样的音频会写出不同的字节，A/A 的「字节相同」与「重跑逐字相同」都做不到。"""
    import numpy as np
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    data = np.ascontiguousarray(audio, dtype='<f4').tobytes()
    channels = audio.shape[1]
    block = 4 * channels
    fmt = struct.pack('<HHIIHHH', 3, channels, rate, rate * block, block, 32, 0)    # 3 = IEEE float；末项 cbSize = 0
    fact = struct.pack('<I', audio.shape[0])
    body = (b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'fact' + struct.pack('<I', len(fact)) + fact
            + b'data' + struct.pack('<I', len(data)) + data)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


# ---------------------------------------------------------------------------------------------------------------------
# build：参数 → 计划（make_plan，只读）→ 写包（execute_build）

CONDITION_FLAGS = {'A': '--a', 'B': '--b', 'O': '--original'}


def parse_pair(text):
    parts = text.split(':')
    if len(parts) != 2 or parts[0] == parts[1] or any(p not in CONDITIONS for p in parts):
        raise UsageError(f'--pair 要写成 基线:候选，如 A:B 或 O:A（条件取自 {"/".join(CONDITIONS)}）')
    return parts[0], parts[1]


def validate_build_args(args, base, cand, n_trials, n_sessions):
    if args.aa and args.pair != 'A:B':
        raise UsageError('--aa 固定比 A:B（B 侧读 A 的同一批文件），不要再给 --pair')
    if not BARS_MIN <= args.bars <= BARS_MAX:
        raise UsageError(f'--bars 要在 {BARS_MIN}～{BARS_MAX} 之间（ADR-034：8～16 小节）')
    if args.segments_per_song < 1:
        raise UsageError('--segments-per-song 至少 1')
    if args.fade_ms < FADE_MS_MIN:
        raise UsageError(f'--fade-ms 至少 {FADE_MS_MIN:g} ms')
    if n_trials < 1 or not 1 <= n_sessions <= n_trials:
        raise UsageError('--trials 至少 1，且 1 ≤ --sessions ≤ --trials')
    per_session = math.ceil(n_trials / n_sessions)
    if per_session > MAX_TRIALS_PER_SESSION:
        raise UsageError(f'每会话 {per_session} 次，超过上限 {MAX_TRIALS_PER_SESSION}（BS.1116 的 10～15 次）：加大 --sessions')
    if 'O' in (base, cand) and args.normalize_lufs is None:
        raise UsageError('原曲没有对齐过响度：--pair 含 O 时必须给 --normalize-lufs（R1-05：三方都归一化到同一目标）')
    if not (args.labels or args.segments):
        raise UsageError('片段来源二选一：--labels + --tempo（自动找 breakdown→drop），或 --segments JSON')
    if args.labels and not args.tempo:
        raise UsageError('--labels 要配 --tempo（小节要 BPM 才能换算成秒）')


def condition_inputs(args, base, cand):
    """条件 → (目录, glob, 自由说明)。--aa 时 B 与 A 读同一批文件。"""
    table = {'A': (args.a, args.glob_a, args.a_desc), 'B': (args.b, args.glob_b, args.b_desc),
             'O': (args.original, args.glob_o, args.o_desc)}
    return {'A': table['A'], 'B': table['A']} if args.aa else {cond: table[cond] for cond in (base, cand)}


def song_key(path):
    """渲染文件名 <歌名>__g35_… → 歌名；原曲 <歌名>.wav → 歌名。"""
    return Path(path).stem.split('__')[0]


def index_renders(directory, pattern, cond):
    """目录里按 pattern 找 wav → {歌名: 路径}；一首歌匹配到多个文件就报错，要用 --glob-* 收窄。"""
    root = Path(directory)
    if not root.is_dir():
        raise ToolError(f'条件 {cond}：目录不存在 {root}')
    found = {}
    for path in sorted(root.glob(pattern)):
        if not path.is_file() or path.suffix.lower() != '.wav':
            continue
        song = song_key(path)
        if song in found:
            raise ToolError(f'条件 {cond}：{song} 匹配到多个文件（{found[song].name}、{path.name}），请用 --glob-{cond.lower()} 收窄')
        found[song] = path
    if not found:
        raise ToolError(f'条件 {cond}：{root} 里按 {pattern} 没找到 wav')
    return found


def common_songs(sources, tracks_arg):
    songs = sorted(set.intersection(*(set(files) for files in sources.values())))
    if tracks_arg:
        wanted = [s for s in tracks_arg.split(',') if s]
        missing = [s for s in wanted if s not in songs]
        if missing:
            raise ToolError(f'--tracks 里的歌在各条件目录里不全：{missing}')
        songs = sorted(wanted)
    if not songs:
        raise ToolError('各条件目录里没有歌名相同的 wav')
    return songs


def make_plan(args):
    """读参数、目录、标签，排出片段与试次；只读不写。dry-run 与真出包共用这一份。"""
    base, cand = ('A', 'B') if args.aa else parse_pair(args.pair)
    n_trials = args.trials or MODE_DEFAULTS[args.mode]['trials']
    n_sessions = args.sessions or MODE_DEFAULTS[args.mode]['sessions']
    validate_build_args(args, base, cand, n_trials, n_sessions)
    inputs = condition_inputs(args, base, cand)
    sources = {}
    for cond, (directory, pattern, _) in inputs.items():
        if not directory:
            raise UsageError(f'条件 {cond} 没给目录（{CONDITION_FLAGS[cond]}）')
        sources[cond] = index_renders(directory, pattern, cond)
    songs = common_songs(sources, args.tracks)
    tempo, classes = load_tempo(args.tempo) if args.tempo else ({}, {})
    kinds = load_bar_kinds(args.labels, args.labels_override) if args.labels else {}
    explicit = load_explicit_segments(args.segments) if args.segments else {}
    plans, skipped = {}, []
    for song in songs:
        probe, reason = probe_song(song, sources)
        segments = []
        if probe is not None:
            segments, reason = plan_song(song, probe, args, tempo, kinds, explicit)
        if segments:
            subclass, delivery = describe_song(song, sources[cand][song].name, classes)
            plans[song] = {'class': subclass, 'delivery': delivery, 'bpm': tempo.get(song), 'segments': segments}
        else:
            skipped.append({'song': song, 'na_reason': reason})
    if not plans:
        raise ToolError('没有一首歌能出片段：' + '；'.join(f"{s['song']}：{s['na_reason']}" for s in skipped))
    units = [{'song': song, 'segment': i, **segment}
             for song, plan in sorted(plans.items()) for i, segment in enumerate(plan['segments'], start=1)]
    trials = plan_trials(units, args.mode, n_trials, n_sessions, derive_rng(args.seed, 'trials'))
    return {'batch': args.batch or Path(args.out).resolve().name, 'mode': args.mode, 'base': base, 'cand': cand,
            'n_trials': n_trials, 'n_sessions': n_sessions, 'sizes': session_sizes(n_trials, n_sessions),
            'inputs': inputs, 'sources': sources, 'plans': plans, 'skipped': skipped, 'trials': trials}


def print_plan(plan, args):
    sizes = ' + '.join(str(n) for n in plan['sizes'])
    print(f"批次 {plan['batch']} · 模式 {plan['mode']} · 基线 {plan['base']} 候选 {plan['cand']}{'（A/A）' if args.aa else ''} · 种子 {args.seed}")
    print(f"试次 {plan['n_trials']} · 会话 {plan['n_sessions']}（{sizes}）· 片段 {sum(len(p['segments']) for p in plan['plans'].values())} 个")
    for song, p in sorted(plan['plans'].items()):
        shared = len(p['segments']) - len({s['boundary'] for s in p['segments']})
        bpm = f"{p['bpm']:.3f}" if p['bpm'] else '—'
        extra = f'（{shared} 段与别段共用边界）' if shared and p['segments'][0]['boundary'] is not None else ''
        print(f"  {song}  [{p['class'] or '?'} / {p['delivery']}]  BPM {bpm}  {len(p['segments'])} 段{extra}")
        for i, s in enumerate(p['segments'], start=1):
            where = f"小节 {s['start_bar']}-{s['end_bar']}（边界 {s['boundary_bar']}）" if s['start_bar'] is not None else '显式秒数'
            print(f"      {i}. {where}  {s['start_s']:.2f}–{s['end_s']:.2f} s")
    for item in plan['skipped']:
        print(f"  跳过 {item['song']}：{item['na_reason']}")
    gaps = coverage_gaps(plan['plans'])
    if gaps:
        print(f"  覆盖不全（协议要求 Deep / Tech / Melodic × 流媒体 / 俱乐部，ADR-034 决定 4）：缺 {'、'.join(gaps)}")
    if len(plan['plans']) < 6:
        print(f"  只有 {len(plan['plans'])} 首歌（协议要求 ≥ 6 首）")
    for session in range(1, plan['n_sessions'] + 1):
        minutes = session_minutes([t for t in plan['trials'] if t['session'] == session], plan['mode'])
        note = f'  ← 超过 {MAX_SESSION_MINUTES:g} min，建议加 --sessions' if minutes > MAX_SESSION_MINUTES else ''
        print(f"  会话 {session}：{plan['sizes'][session - 1]} 次，估 {minutes:.1f} min{note}")


def decision_rule(mode, n_trials):
    """事先登记的决策规则。胜场表按「有效试次数」列出，读答卷前就能查线。"""
    if mode == '2afc':
        return {'type': '2afc-sign-test', 'direction': 'candidate', 'min_preference': MIN_PREFERENCE, 'alpha': ALPHA, 'one_sided': True,
                'none_answers': 'excluded-before-test',
                'min_wins_by_valid_trials': {str(n): min_wins(n, ALPHA, MIN_PREFERENCE) for n in range(1, n_trials + 1)}}
    return {'type': 'abx-binomial', 'alpha': ALPHA, 'one_sided': True, 'min_correct': min_wins(n_trials, ALPHA)}


def tool_versions():
    from importlib import metadata
    versions = {'python': '.'.join(str(x) for x in sys.version_info[:3])}
    for name in ('numpy', 'scipy', 'soundfile', 'pyloudnorm'):
        try:
            versions[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            versions[name] = None
    return versions


def build_registration(args, plan, source_info):
    """登记文件：出包时一次写定，之后不改。不含时间戳与输出目录，所以同输入重跑逐字相同。"""
    conditions = {cond: {'dir': str(Path(directory).resolve()), 'glob': pattern, 'desc': desc or ''}
                  for cond, (directory, pattern, desc) in plan['inputs'].items()}
    songs = {}
    for song, p in sorted(plan['plans'].items()):
        used = None if p['segments'][0]['boundary'] is None else len({s['boundary'] for s in p['segments']})
        songs[song] = {'class': p['class'], 'delivery': p['delivery'], 'bpm': p['bpm'], 'boundaries_used': used,
                       'segments': [{'index': i, **{k: s[k] for k in ('start_bar', 'end_bar', 'boundary_bar', 'start_s', 'end_s')}}
                                    for i, s in enumerate(p['segments'], start=1)],
                       'sources': source_info[song]}
    return {'format': FORMAT_VERSION, 'batch': plan['batch'], 'mode': plan['mode'], 'aa': bool(args.aa),
            'pair': {'baseline': plan['base'], 'candidate': plan['cand']}, 'seed': args.seed,
            'trials': plan['n_trials'], 'sessions': plan['n_sessions'], 'session_sizes': plan['sizes'],
            'bars': args.bars, 'segments_per_song': args.segments_per_song, 'fade_ms': args.fade_ms,
            'normalize_lufs': args.normalize_lufs, 'conditions': conditions,
            'loudness': {'tolerance_lu': LOUDNESS_TOLERANCE_LU, 'sensitive_lu': SENSITIVE_LU, 'warn_lu': WARN_LU,
                         'matching': '整曲匹配（celadon_render --match-lufs，或本脚本 --normalize-lufs 用 pyloudnorm 量整曲），不用短窗'},
            'decision_rule': decision_rule(plan['mode'], plan['n_trials']),
            'session_limits': {'max_trials': MAX_TRIALS_PER_SESSION, 'max_minutes': MAX_SESSION_MINUTES},
            'songs': songs, 'skipped_songs': plan['skipped'],
            'script': {'name': 'tools/render/blind_test.py', 'sha256': sha256_file(SCRIPT_PATH)},
            'versions': tool_versions()}


def render_trial(trial, sides, args, plan, gains, package):
    """写出一个试次的全部文件 → (files, sha256, measures)。同一条件的片段只读一次；ABX 的 x 位是 a 或 b 的字节复制。"""
    unit = trial['unit']
    cache, files, hashes, measures = {}, {}, {}, {}
    for slot in FILE_SLOTS[args.mode]:
        cond = sides[slot]
        if cond not in cache:
            audio, rate = read_segment(plan['sources'][cond][unit['song']], unit['start_s'], unit['end_s'])
            cache[cond] = (apply_fade(audio * gains[(unit['song'], cond)], rate, args.fade_ms), rate)
        audio, rate = cache[cond]
        rel = f"s{trial['session']}/{trial['id']}_{slot}.wav"
        write_wav_float32(package / rel, audio, rate)
        files[slot], hashes[slot] = rel, sha256_file(package / rel)
        if cond not in measures:
            m = measure_file(package / rel)                  # 量的就是发出去的这个文件
            measures[cond] = {'lufs': finite_or_none(m['lufs_i']), 'sample_peak_dbfs': finite_or_none(m['sample_peak_dbfs']),
                              'tp8_dbtp': finite_or_none(m['tp8_dbtp']), 'clip_samples': int(m['sample_clip_count'])}
    return files, hashes, measures


def trial_table(records, mode):
    """公开的试次表（下发到 Mac）：不含任何能看出 A / B 的列；响度差只给绝对值，带符号的留在 key.json。"""
    header = (['trial', 'session', 'song', 'segment', 'start_s', 'end_s', 'bars', 'loudness_diff_abs_lu', 'loudness_flag', 'na_reason']
              + [f'file_{slot}' for slot in FILE_SLOTS[mode]])
    rows = [[tid, r['session'], r['song'], r['segment'], f"{r['start_s']:.3f}", f"{r['end_s']:.3f}",
             '' if r['bars'] is None else r['bars'], fmt_number(r['loudness_diff_abs_lu']), r['loudness_flag'], r['na_reason']]
            + [r['files'][slot] for slot in FILE_SLOTS[mode]] for tid, r in sorted(records.items())]
    return header, rows


def listen_text(plan):
    n, k = plan['n_trials'], plan['n_sessions']
    lines = [f"Celadon 盲测包 {plan['batch']}", '']
    if plan['mode'] == '2afc':
        lines += [f'偏好盲测（2AFC）：共 {n} 次试次，分 {k} 个会话（文件夹 s1 … s{k}）；一个会话听完后，休息的时间不短于会话时长。',
                  '每次试次两个文件 tNN_1.wav 与 tNN_2.wav：同一首歌的同一段，响度已对齐。',
                  '两个都听完再选：哪个更好听，就在 answers.csv 的 answer 列填 1 或 2；真的听不出差别填 none。']
    else:
        lines += [f'区分盲测（ABX）：共 {n} 次试次，分 {k} 个会话（文件夹 s1 … s{k}）；一个会话听完后，休息的时间不短于会话时长。',
                  '每次试次三个文件：tNN_a.wav 与 tNN_b.wav 是两个版本，tNN_x.wav 和其中一个的声音完全相同。',
                  '听出 x 像 a 还是 b，在 answers.csv 的 answer 列填 a 或 b（必须选一个，猜也要填）。']
    lines += ['', '只填 answer 列（note 列随意）；整张填完再交回。',
              '不要看波形，不要比文件大小或校验和，不要按文件名猜；监听音量固定，别中途调。',
              '32-bit float 的片段可能超过 0 dBFS：监听先拉低 6 dB，别让 DA 的削波替你做判断。']
    return '\n'.join(lines) + '\n'


def execute_build(args, plan, out):
    package = out / 'package'
    base, cand, mode = plan['base'], plan['cand'], plan['mode']
    load_ruler()                                             # 缺依赖就在动手前报
    songs = sorted(plan['plans'])
    # 1) 源文件指纹与整曲响度（整曲匹配的依据；--normalize-lufs 时据此算增益。同一文件只量一次）
    digests, whole, gains, source_info = {}, {}, {}, {song: {} for song in songs}
    for song in songs:
        for cond, files in plan['sources'].items():
            path = files[song]
            if path not in whole:
                whole[path] = measure_file(path)['lufs_i']
                digests[path] = {'file': path.name, 'bytes': path.stat().st_size, 'sha256': sha256_file(path)}
            source_info[song][cond] = digests[path]
            if args.normalize_lufs is None:
                gains[(song, cond)] = 1.0
            elif math.isfinite(whole[path]):
                gains[(song, cond)] = 10.0 ** ((args.normalize_lufs - whole[path]) / 20.0)
            else:
                raise ToolError(f'{path}：整曲响度不可算（静音？），不能归一化')
    registration = build_registration(args, plan, source_info)
    write_json(out / 'registration.json', registration)
    registration_sha = sha256_file(out / 'registration.json')
    # 2) 逐试次切片、写文件、量响度
    records = {}
    for trial in plan['trials']:
        sides, x_is = trial_sides(trial, mode, base, cand)
        files, hashes, measures = render_trial(trial, sides, args, plan, gains, package)
        unit = trial['unit']
        records[trial['id']] = {
            'session': trial['session'], 'song': unit['song'], 'segment': unit['segment'],
            'start_s': unit['start_s'], 'end_s': unit['end_s'],
            'bars': None if unit['start_bar'] is None else unit['end_bar'] - unit['start_bar'] + 1,
            'sides': sides, 'x_is': x_is, 'files': files, 'sha256': hashes, 'measures': measures,
            **loudness_record(measures, base, cand)}
    # 3) 整曲层面的记录与警告
    song_info, warnings = {}, []
    for song in songs:
        lufs = {cond: whole[plan['sources'][cond][song]] for cond in plan['sources']}
        diff = lufs[cand] - lufs[base] if math.isfinite(lufs[cand]) and math.isfinite(lufs[base]) else None
        song_info[song] = {'whole_file_lufs': {c: finite_or_none(v) for c, v in sorted(lufs.items())},
                           'normalize_gain_db': {c: finite_or_none(20.0 * math.log10(gains[(song, c)])) for c in sorted(lufs)},
                           'whole_file_diff_lu': finite_or_none(diff)}
        if args.normalize_lufs is None and diff is not None and abs(diff) > LOUDNESS_TOLERANCE_LU:
            warnings.append(f'{song} 整曲响度差 {diff:+.2f} LU（对齐前）超过登记的容差 {LOUDNESS_TOLERANCE_LU} LU：渲染时 --match-lufs 没对准？')
    clipped = sorted(tid for tid, r in records.items() if any(m['clip_samples'] > 0 for m in r['measures'].values()))
    if clipped:
        warnings.append(f"{len(clipped)} 个试次有片段超过 0 dBFS（{', '.join(clipped)}）：32-bit float 照样能存，但监听要先拉低 6 dB")
    flags = [r['loudness_flag'] for r in records.values()]
    for flag, text in (('sensitive', f'片段响度差 > {SENSITIVE_LU} LU（统计里另列敏感性分析）'), ('warn', f'片段响度差 > {WARN_LU} LU（警告：等响没对上）')):
        if flag in flags:
            warnings.append(f"{flags.count(flag)} 个试次{text}")
    # 4) 公开文件与私密文件
    session_rows = []
    for session in range(1, plan['n_sessions'] + 1):
        ids = [t['id'] for t in plan['trials'] if t['session'] == session]
        minutes = session_minutes([t for t in plan['trials'] if t['session'] == session], mode)
        session_rows.append([session, ids[0], ids[-1], len(ids), f'{minutes:.1f}'])
        if minutes > MAX_SESSION_MINUTES:
            warnings.append(f'会话 {session} 估时 {minutes:.1f} min 超过 {MAX_SESSION_MINUTES:g} min：加 --sessions')
    write_table(package / 'trials.csv', *trial_table(records, mode))
    write_table(package / 'sessions.csv', ['session', 'first_trial', 'last_trial', 'trials', 'est_minutes'], session_rows)
    write_table(package / 'answers.csv', ['trial', 'session', 'answer', 'note'], [[tid, r['session'], '', ''] for tid, r in sorted(records.items())])
    write_text(package / 'LISTEN.txt', listen_text(plan))
    write_json(out / 'key.json', {'format': FORMAT_VERSION, 'batch': plan['batch'], 'mode': mode, 'aa': bool(args.aa),
                                  'pair': {'baseline': base, 'candidate': cand}, 'registration_sha256': registration_sha,
                                  'trials': records, 'songs': song_info, 'warnings': warnings})
    # 5) 包体检查与摘要
    checks = check_package(out)
    print_build_summary(out, records, warnings, checks, registration_sha)
    return 0 if all(ok for _, ok, _ in checks) else 1


def print_build_summary(out, records, warnings, checks, registration_sha):
    flags = [r['loudness_flag'] for r in records.values()]
    diffs = [r['loudness_diff_abs_lu'] for r in records.values() if r['loudness_diff_abs_lu'] is not None]
    print(f'\n出包完成：{out}')
    print(f"  试次 {len(records)} · 文件 {sum(len(r['files']) for r in records.values())} 个")
    print('  片段响度差档位：' + ' · '.join(f'{name} {flags.count(name)}' for name in ('ok', 'above_tolerance', 'sensitive', 'warn', 'na')) +
          (f'（最大 |Δ| {max(diffs):.4f} LU）' if diffs else ''))
    for text in warnings:
        print(f'  警告：{text}')
    print('  包体检查：')
    for name, ok, detail in checks:
        print(f"    {'ok  ' if ok else 'FAIL'} {name}{('：' + detail) if detail else ''}")
    print(f'  登记 sha256：{registration_sha}')
    print('  只把 package/ 拉到 Mac；key.json 与 registration.json 留在 DGX，答卷交回前不下发。')


def cmd_build(args):
    out = Path(args.out)
    if not args.dry_run and out.exists() and any(out.iterdir()):
        raise UsageError(f'输出目录已存在且非空：{out}（不覆盖已有批次）')
    plan = make_plan(args)
    print_plan(plan, args)
    if args.dry_run:
        print('--dry-run：没有写任何文件。')
        return 0
    return execute_build(args, plan, out)


# ---------------------------------------------------------------------------------------------------------------------
# 包体检查（每次出包自动跑、必须过；A/A 包另加「两侧逐字节相同」）

def check_package(batch_dir):
    """读盘上的成品包，逐项查有没有泄漏 A / B 的线索 → [(名称, 是否通过, 说明)]。只检查，不改文件。
    注意：真 A/B 包里两侧的响度 / 峰值本来就可以不同（那是版本差异，不是泄漏），所以这里只查元数据与位置；响度差由 loudness_flag 另报。"""
    import soundfile as sf
    batch = Path(batch_dir)
    package = batch / 'package'
    key, registration = read_json(batch / 'key.json'), read_json(batch / 'registration.json')
    mode, trials = key['mode'], key['trials']
    slots = FILE_SLOTS[mode]
    candidate = key['pair']['candidate']
    results = []

    def add(name, ok, detail=''):
        results.append((name, bool(ok), detail))

    expected = {rel for r in trials.values() for rel in r['files'].values()}
    actual = {p.relative_to(package).as_posix() for p in package.rglob('*.wav')}
    add('文件齐全、无多余', expected == actual, '' if expected == actual else f'缺 {sorted(expected - actual)[:3]}，多 {sorted(actual - expected)[:3]}')
    if expected != actual:
        return results

    width = max(2, len(str(len(trials))))
    pattern = re.compile(r'^s(\d+)/(t\d{2,})_([12abx])\.wav$')
    bad = [rel for tid, r in trials.items() for slot, rel in r['files'].items()
           if not ((m := pattern.match(rel)) and int(m.group(1)) == r['session'] and m.group(2) == tid and m.group(3) == slot and slot in slots)]
    ids_ok = sorted(trials) == [f't{i:0{width}d}' for i in range(1, len(trials) + 1)]
    add('命名规律（s<会话>/t<序号>_<位>.wav，序号连续，名字里没有条件信息）',
        ids_ok and not bad and all(set(r['files']) == set(slots) for r in trials.values()), f'异常 {bad[:3]}' if bad else '')

    uneven = []
    for tid, r in trials.items():
        shapes = set()
        for rel in r['files'].values():
            info = sf.info(str(package / rel))
            shapes.add((info.frames, info.samplerate, info.channels, info.subtype, (package / rel).stat().st_size))
        if len(shapes) != 1:
            uneven.append(tid)
    add('同一试次各文件的时长、采样率、声道、位深、字节数一致', not uneven, f'不一致的试次 {uneven[:5]}' if uneven else '')

    def spread(selector):
        """真 / 假个数之差：(整批, 各会话里最大的一个)。"""
        by_session = {}
        for r in trials.values():
            by_session.setdefault(r['session'], []).append(selector(r))
        flat = [v for values in by_session.values() for v in values]
        return abs(2 * sum(flat) - len(flat)), max(abs(2 * sum(v) - len(v)) for v in by_session.values())

    selectors = {f'候选在 {slots[0]} 位': lambda r: r['sides'][slots[0]] == candidate}
    if mode == 'abx':
        selectors['x 是 a'] = lambda r: r['x_is'] == 'a'
    for name, selector in selectors.items():
        overall, worst = spread(selector)
        add(f'位置均衡：{name}（整批差 ≤ 1，每会话差 ≤ 1）', overall <= 1 and worst <= 1, f'整批差 {overall}，会话最大差 {worst}')

    forbidden = {Path(src['file']).stem for meta in registration['songs'].values() for src in meta['sources'].values()} - set(registration['songs'])
    public_files = [(package / name).read_text(encoding='utf-8') for name in ('trials.csv', 'sessions.csv', 'answers.csv', 'LISTEN.txt')
                    if (package / name).exists()]
    public = '\n'.join(sorted(expected) + public_files)
    leaks = sorted(s for s in forbidden if s in public)
    add('文件名与公开表里没有源文件名', not leaks, f'泄漏 {leaks[:3]}' if leaks else '')
    header = read_rows(package / 'trials.csv')[0].keys() if (package / 'trials.csv').exists() else []
    add('公开试次表没有带符号的响度差列', 'loudness_diff_lu' not in header, '')

    disk = {rel: sha256_file(package / rel) for rel in expected}
    changed = sorted(rel for r in trials.values() for slot, rel in r['files'].items() if disk[rel] != r['sha256'][slot])
    add('盘上文件与 key.json 记的 sha256 一致', not changed, f'不一致 {changed[:3]}' if changed else '')
    add('key.json 对应这份 registration.json', key['registration_sha256'] == sha256_file(batch / 'registration.json'), '')

    if key['aa']:
        different = []
        for tid, r in trials.items():
            first, second = (r['files'][s] for s in slots[:2])               # 2AFC 的 1 / 2；ABX 的 a / b
            if disk[first] != disk[second] or r['measures'][key['pair']['baseline']] != r['measures'][candidate]:
                different.append(tid)
        add('A/A：两侧逐字节相同，时长 / 响度 / 峰值逐项相同', not different, f'不同的试次 {different[:5]}' if different else '')
    return results


# ---------------------------------------------------------------------------------------------------------------------
# score（只用标准库）

def read_answers(path, mode, trial_ids):
    """答卷 CSV → {试次号: 答案或 None}。答案不合法、试次号对不上、重复 → ToolError；空白 = 没答。"""
    rows = read_rows(path)
    require_columns(rows, ('trial', 'answer'), path)
    allowed = ANSWER_CHOICES[mode]
    answers, problems = {}, []
    for row in rows:
        tid = (row['trial'] or '').strip()
        if not tid:
            continue
        text = (row['answer'] or '').strip().lower()
        if text.endswith('.0'):
            text = text[:-2]                                  # 表格软件把 1 存成 1.0
        if tid not in trial_ids:
            problems.append(f'{tid}：不在试次表里')
        elif tid in answers:
            problems.append(f'{tid}：答卷里出现多次')
        elif text and text not in allowed:
            problems.append(f'{tid}：答案「{row["answer"]}」不合法（只能填 {"/".join(allowed)}）')
        else:
            answers[tid] = text or None
    if problems:
        raise ToolError(f'{path} 有问题：' + '；'.join(problems[:8]))
    return {tid: answers.get(tid) for tid in trial_ids}


def classify(mode, record, answer, candidate):
    """一个试次的结果：2AFC → candidate / baseline / none；ABX → correct / wrong。"""
    if mode == '2afc':
        if answer == 'none':
            return 'none'
        return 'candidate' if record['sides'][answer] == candidate else 'baseline'
    return 'correct' if answer == record['x_is'] else 'wrong'


def count_outcome(items, name):
    return sum(1 for _, _, outcome in items if outcome == name)


def tally(mode, items):
    """items = [(试次号, 记录, 结果)] → 统计块。2AFC 先排除「无偏好」再做符号检验。"""
    if mode == '2afc':
        wins, losses, none = count_outcome(items, 'candidate'), count_outcome(items, 'baseline'), count_outcome(items, 'none')
        valid = wins + losses
        return {'trials': len(items), 'candidate': wins, 'baseline': losses, 'none': none, 'valid': valid,
                'preference': ratio(wins, valid), 'p_one_sided': binom_p_ge(wins, valid), 'p_two_sided': binom_p_two_sided(wins, valid)}
    correct, total = count_outcome(items, 'correct'), len(items)
    return {'trials': total, 'correct': correct, 'wrong': total - correct, 'accuracy': ratio(correct, total),
            'p_one_sided': binom_p_ge(correct, total), 'p_two_sided': binom_p_two_sided(correct, total)}


def group_tally(mode, items, name_of):
    groups = {}
    for item in items:
        groups.setdefault(name_of(item), []).append(item)
    return {str(name): tally(mode, members) for name, members in sorted(groups.items())}


def rounded(value):
    """结果里的小数统一取 6 位：数字格式固定，同输入重跑逐字相同。"""
    if isinstance(value, float):
        return round(value, 6)
    if isinstance(value, dict):
        return {k: rounded(v) for k, v in value.items()}
    if isinstance(value, list):
        return [rounded(v) for v in value]
    return value


def decide(mode, rule, overall, complete, aa):
    """登记规则 → 结论。A/A 包不适用通过线，改判「有没有显著偏离 50 %」（双侧）。"""
    if not complete:
        return 'incomplete'
    if aa:
        p = overall['p_two_sided']
        return 'aa-suspected-leak' if p is not None and p < rule['alpha'] else 'aa-no-leak'
    if mode == '2afc':
        passed = overall['valid'] > 0 and overall['preference'] >= rule['min_preference'] and overall['p_one_sided'] < rule['alpha']
    else:
        passed = overall['trials'] > 0 and overall['p_one_sided'] < rule['alpha']
    return 'pass' if passed else 'fail'


def score_answers(key, registration, answers, answers_sha):
    """纯函数：钥匙 + 登记 + 答案 → 结果字典（不含时间戳）。"""
    mode, candidate, rule = key['mode'], key['pair']['candidate'], registration['decision_rule']
    ids = sorted(key['trials'])
    items = [(tid, key['trials'][tid], classify(mode, key['trials'][tid], answers[tid], candidate)) for tid in ids if answers[tid] is not None]
    missing = [tid for tid in ids if answers[tid] is None]
    overall = tally(mode, items)
    kept = [item for item in items if item[1]['loudness_flag'] not in ('sensitive', 'warn')]
    boundaries = [m['boundaries_used'] for m in registration['songs'].values() if m['boundaries_used'] is not None]
    required = rule['min_wins_by_valid_trials'].get(str(overall['valid'])) if mode == '2afc' else rule['min_correct']
    return rounded({
        'format': FORMAT_VERSION, 'batch': key['batch'], 'mode': mode, 'aa': key['aa'], 'pair': key['pair'],
        'registration_sha256': key['registration_sha256'], 'answers_sha256': answers_sha,
        'planned_trials': len(ids), 'answered_trials': len(items), 'missing_trials': missing,
        'overall': overall, 'sensitivity_excluding_flagged': tally(mode, kept),
        'by_song': group_tally(mode, items, lambda item: item[1]['song']),
        'by_session': group_tally(mode, items, lambda item: item[1]['session']),
        'rule': {k: v for k, v in rule.items() if k != 'min_wins_by_valid_trials'}, 'required_wins': required,
        'distinct_boundaries': sum(boundaries) if boundaries else None,
        'verdict': decide(mode, rule, overall, not missing, key['aa'])})


def pct(value):
    return '—' if value is None else f'{100 * value:.1f} %'


def pval(value):
    return '—' if value is None else f'{value:.4f}'


def table_header(mode, first):
    if mode == '2afc':
        return [f'| {first} | 偏好候选 | 偏好基线 | 无偏好 | 有效 | 偏好候选 % | 单侧 p | 双侧 p |', '|---|---:|---:|---:|---:|---:|---:|---:|']
    return [f'| {first} | 答对 | 答错 | 正确率 | 单侧 p | 双侧 p |', '|---|---:|---:|---:|---:|---:|']


def table_row(mode, label, block):
    if mode == '2afc':
        return (f"| {label} | {block['candidate']} | {block['baseline']} | {block['none']} | {block['valid']} | "
                f"{pct(block['preference'])} | {pval(block['p_one_sided'])} | {pval(block['p_two_sided'])} |")
    return (f"| {label} | {block['correct']} | {block['wrong']} | {pct(block['accuracy'])} | "
            f"{pval(block['p_one_sided'])} | {pval(block['p_two_sided'])} |")


def render_result_md(result):
    mode, pair, rule = result['mode'], result['pair'], result['rule']
    what = (f"2AFC 偏好：候选 {pair['candidate']} 对基线 {pair['baseline']}" if mode == '2afc'
            else f"ABX 区分：{pair['baseline']} 与 {pair['candidate']}")
    missing = result['missing_trials']
    lines = [f"# 盲测结果 · {result['batch']}", '',
             f"- 模式：{what}" + ('（A/A 自检包）' if result['aa'] else ''),
             f"- 试次：计划 {result['planned_trials']}、已答 {result['answered_trials']}、缺 {len(missing)}" + (f"（{', '.join(missing)}）" if missing else ''),
             f"- 登记 sha256：{result['registration_sha256']}",
             f"- 答卷 sha256：{result['answers_sha256']}", '',
             f"## 结论：{VERDICT_TEXT[result['verdict']]}", '',
             '## 全部试次', ''] + table_header(mode, '范围') + [table_row(mode, '全部', result['overall']), '',
             '## 敏感性分析：去掉片段响度差 > 0.3 LU 的试次（档位 sensitive / warn）', ''] + table_header(mode, '范围') + [
             table_row(mode, '去掉后', result['sensitivity_excluding_flagged']), '',
             '## 按歌', ''] + table_header(mode, '歌') + [table_row(mode, name, block) for name, block in result['by_song'].items()] + ['',
             '## 按会话', ''] + table_header(mode, '会话') + [table_row(mode, name, block) for name, block in result['by_session'].items()] + ['',
             '## 说明', '']
    if mode == '2afc':
        lines.append(f"- 登记的通过线：排除「无偏好」后，偏好候选 ≥ {100 * rule['min_preference']:.0f} % 且单侧精确二项检验 p < {rule['alpha']}；"
                     f"本批有效试次 {result['overall']['valid']} 次，需要候选胜 ≥ {result['required_wins'] if result['required_wins'] is not None else '（达不到）'} 场。")
    else:
        lines.append(f"- 登记的通过线：单侧精确二项检验 p < {rule['alpha']}；本批 {result['planned_trials']} 次需要答对 ≥ "
                     f"{result['required_wins'] if result['required_wins'] is not None else '（达不到）'} 次。")
    lines += ['- 双侧 p 仅供参考，不进通过线。',
              '- B 组不退步（ADR-034 决定 4 的第三个条件）不在本脚本判定，由完成报告对照 R5-01 的最小指标集给出。']
    if result['distinct_boundaries'] is not None:
        lines.append(f"- 片段取自 {result['distinct_boundaries']} 个不同的 breakdown→drop 边界；同一边界上错开的窗口是同一段音乐的重复测量，不是独立样本。")
    return '\n'.join(lines) + '\n'


def cmd_score(args):
    batch = Path(args.batch)
    key, registration = read_json(batch / 'key.json'), read_json(batch / 'registration.json')
    if key.get('registration_sha256') != sha256_file(batch / 'registration.json'):
        raise ToolError('key.json 记的登记 sha256 与 registration.json 对不上：这两个文件不是同一次出包的')
    answers = read_answers(args.answers, key['mode'], sorted(key['trials']))
    result = score_answers(key, registration, answers, sha256_file(args.answers))
    out = Path(args.out) if args.out else Path(args.answers).resolve().parent
    write_json(out / 'result.json', result)
    text = render_result_md(result)
    write_text(out / 'result.md', text)
    print(text)
    print(f'已写入 {out / "result.md"} 与 {out / "result.json"}')
    return 0


# ---------------------------------------------------------------------------------------------------------------------
# selfcheck：合成素材 + 已知答案。全 ok 退出 0。

SYN_RATE = 48000
SYN_BPM = 240.0                        # 一小节正好 1 s：片段短，自检快
SYN_LAYOUT = (('intro', 4), ('breakdown', 8), ('drop', 10), ('breakdown', 8), ('drop', 10), ('breakdown', 8), ('drop', 10), ('outro', 3))
SYN_SONGS = ('synth1', 'synth2')
SYN_GAIN_DB = 20.0 * math.log10(2.0)   # B 的幅度是 A 的两倍 → 响度高 6.0206 dB


class Report:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=''):
        self.rows.append((name, bool(ok)))
        print(f"{'ok  ' if ok else 'FAIL'} {name}{('  ' + detail) if detail else ''}", flush=True)

    def failures(self):
        return [name for name, ok in self.rows if not ok]


def guarded(report, name, function, *args):
    """自检里任何异常都记成一项失败，不让它中断后面的检查。"""
    try:
        function(report, *args)
    except Exception as error:
        report.check(f'{name}：运行出错', False, f'{type(error).__name__}: {error}')


def synthetic_bar_kinds():
    """合成歌的逐小节标签：raw 是原始段落，kinds 把交界前后各 1 小节改成 transition（与 T-038 自动标签同一规则）。"""
    raw = [kind for kind, count in SYN_LAYOUT for _ in range(count)]
    kinds = list(raw)
    for i in range(1, len(raw)):
        if raw[i - 1] != raw[i]:
            kinds[i - 1] = kinds[i] = 'transition'
    return raw, kinds


def synthetic_audio(song_index, raw):
    """一首 61 s 的立体声「歌」：每小节 1 s，段落决定电平，drop 里每拍一个底鼓。固定种子，峰值归一到 0.8。"""
    import numpy as np
    rng = np.random.default_rng(20261011 + song_index)
    level = {'intro': 0.05, 'breakdown': 0.12, 'drop': 0.25, 'outro': 0.06}
    t = np.arange(SYN_RATE) / SYN_RATE
    beat = t % 0.25
    kick = np.sin(2 * np.pi * 55 * beat) * np.exp(-beat * 18)
    pad = np.sin(2 * np.pi * (110 + 30 * song_index) * t) + 0.5 * np.sin(2 * np.pi * (165 + 30 * song_index) * t)
    bars = []
    for kind in raw:
        mono = level[kind] * pad + (0.35 * kick if kind == 'drop' else 0.0)
        bars.append(np.column_stack((mono + 0.01 * rng.standard_normal(SYN_RATE), 0.95 * mono + 0.01 * rng.standard_normal(SYN_RATE))))
    audio = np.concatenate(bars)
    return audio * (0.8 / np.max(np.abs(audio)))


def write_synthetic_corpus(root):
    """A = 原样，B = 幅度 ×2（+6.02 dB），O = 原曲；再写 T-038 格式的标签表与速度表。"""
    root = Path(root)
    raw, kinds = synthetic_bar_kinds()
    labels, tempo = [], []
    for index, song in enumerate(SYN_SONGS):
        audio = synthetic_audio(index, raw)
        write_wav_float32(root / 'orig' / f'{song}.wav', audio, SYN_RATE)
        write_wav_float32(root / 'render' / f'{song}__a.wav', audio, SYN_RATE)
        write_wav_float32(root / 'render' / f'{song}__b.wav', audio * 2.0, SYN_RATE)
        labels += [['deep', song, bar, label] for bar, label in enumerate(kinds, start=1)]
        tempo.append(['deep', song, f'{SYN_BPM:.3f}', '0', '9.00', SYN_RATE, f'{len(raw):.2f}'])
    write_table(root / 'labels.tsv', ['class', 'track', 'bar', 'label'], labels, '\t')
    write_table(root / 'tempo.tsv', ['class', 'track', 'bpm', 'trim_s', 'fold_peak', 'sample_rate', 'duration_s'], tempo, '\t')
    return {'render': root / 'render', 'orig': root / 'orig', 'labels': root / 'labels.tsv', 'tempo': root / 'tempo.tsv'}


def tree_digest(root):
    """目录里所有文件的（相对路径, sha256）合起来的指纹：比较两次出包是否逐字相同。"""
    digest = hashlib.sha256()
    for path in sorted(Path(root).rglob('*')):
        if path.is_file():
            digest.update(path.relative_to(root).as_posix().encode('utf-8') + b'\0' + sha256_file(path).encode('ascii') + b'\n')
    return digest.hexdigest()


def pick_answers(key, kind):
    """按钥匙造答案。kind = candidate：每次都选候选 / ABX 全答对；baseline：每次都选基线 / ABX 全答错。"""
    answers = {}
    for tid, record in key['trials'].items():
        if key['mode'] == '2afc':
            slot_of = {cond: slot for slot, cond in record['sides'].items()}
            answers[tid] = slot_of[key['pair'][kind]]
        else:
            answers[tid] = record['x_is'] if kind == 'candidate' else ('b' if record['x_is'] == 'a' else 'a')
    return answers


def score_case(report, parser, work, label, out, answers, verdict, extra=None):
    """写一份答卷 → 跑 score → 核对结论（和可选的额外条件）；返回结果目录。"""
    result_dir = work / f'score-{label}'
    key = read_json(out / 'key.json')
    path = result_dir / 'answers.csv'
    write_table(path, ['trial', 'session', 'answer', 'note'], [[tid, key['trials'][tid]['session'], answers.get(tid, ''), ''] for tid in sorted(key['trials'])])
    args = parser.parse_args(['score', '--batch', str(out), '--answers', str(path), '--out', str(result_dir)])
    with contextlib.redirect_stdout(io.StringIO()):
        code = args.func(args)
    result = read_json(result_dir / 'result.json')
    report.check(f'score：{label} → {verdict}', code == 0 and result['verdict'] == verdict and (extra is None or extra(result)), f"结论 {result['verdict']}")
    return result_dir


def selfcheck_stats(report):
    p = binom_p_ge(12, 16)
    report.check('统计：12/16 → 单侧 p = 0.038', _tail_ge(12, 16) == 2517 and round(p, 3) == 0.038, f'p = {p:.4f}（分子 2517 / 65536）')
    p = binom_p_ge(26, 40)
    report.check('统计：26/40 → 单侧 p = 0.040，25/40 过不了线', round(p, 3) == 0.040 and binom_p_ge(25, 40) > ALPHA, f'p = {p:.4f}，25/40 → {binom_p_ge(25, 40):.4f}')
    report.check('统计：11/16 → p = 0.105', round(binom_p_ge(11, 16), 3) == 0.105)
    report.check('统计：双侧 p（8/16 = 1，12/16 = 加倍）',
                 binom_p_two_sided(8, 16) == 1.0 and abs(binom_p_two_sided(12, 16) - 2 * 2517 / 65536) < 1e-12)
    lines = (min_wins(16, ALPHA), min_wins(40, ALPHA), min_wins(18, ALPHA, MIN_PREFERENCE), min_wins(20, ALPHA, MIN_PREFERENCE), min_wins(12, ALPHA))
    report.check('统计：最少胜场线（16 次 12、40 次 26、18 次 13、20 次 15、12 次 10）', lines == (12, 26, 13, 15, 10), str(lines))
    report.check('统计：没有有效试次 → None', binom_p_ge(0, 0) is None and min_wins(0) is None and ratio(0, 0) is None)


def selfcheck_windows(report):
    raw, kinds = synthetic_bar_kinds()
    boundaries = find_boundaries({bar: label for bar, label in enumerate(kinds, start=1)})
    report.check('片段：合成标签里找到 3 个 breakdown→drop 边界', boundaries == [(6, 11, 14, 21), (24, 29, 32, 39), (42, 47, 50, 57)], str(boundaries))
    report.check('片段：隔 3 个 transition 或隔 exclude 的不算边界',
                 find_boundaries({1: 'breakdown', 2: 'breakdown', 3: 'transition', 4: 'transition', 5: 'transition', 6: 'drop'}) == []
                 and find_boundaries({1: 'breakdown', 2: 'exclude', 3: 'drop'}) == [])
    report.check('片段：每边界取一个居中窗口，互不重叠', pick_windows(boundaries, 8, 3, derive_rng(1, 'windows')) == [(0, 10), (1, 28), (2, 46)])
    five = pick_windows(boundaries, 8, 5, derive_rng(1, 'windows'))
    report.check('片段：第一轮不够时同边界错开补窗口', five == [(0, 8), (0, 10), (1, 26), (1, 28), (2, 46)], str(five))
    report.check('片段：补到极限后凑不够 → None', pick_windows(boundaries, 8, 7, derive_rng(1, 'windows')) is None)
    near = pick_windows([(1, 6, 9, 30), (2, 7, 10, 40)], 8, 2, derive_rng(1, 'windows'))
    report.check('片段：两个边界的窗口会重叠时，改在同一边界上错开',
                 near is not None and len(near) == 2 and len({index for index, _ in near}) == 1, str(near))
    report.check('片段：breakdown 太短放不下窗口', start_range((1, 1, 4, 10), 8) is None)
    report.check('片段：同一种子同一结果', pick_windows(boundaries, 8, 3, derive_rng(5, 'x')) == pick_windows(boundaries, 8, 3, derive_rng(5, 'x')))


def selfcheck_builds(report, work):
    parser = make_parser()
    corpus = write_synthetic_corpus(work / 'corpus')
    render, orig = str(corpus['render']), str(corpus['orig'])
    common = ['--a', render, '--glob-a', '*__a.wav', '--labels', str(corpus['labels']), '--tempo', str(corpus['tempo']),
              '--bars', '8', '--segments-per-song', '3', '--trials', '12', '--sessions', '2', '--seed', '7', '--batch', 'selfcheck']
    with_b = ['--b', render, '--glob-b', '*__b.wav']

    def build(name, extra, dry=False):
        out = work / name
        args = parser.parse_args(['build', '--out', str(out)] + common + extra + (['--dry-run'] if dry else []))
        with contextlib.redirect_stdout(io.StringIO()):
            return out, args.func(args)

    def scenario(name, extra):
        """出一个包 → 报告退出码与包体检查每一项 → 返回 (目录, key)。"""
        try:
            out, code = build(name, extra)
            report.check(f'{name}：出包退出码 0', code == 0, f'退出码 {code}')
            for check_name, ok, detail in check_package(out):
                report.check(f'{name}：{check_name}', ok, detail)
            return out, read_json(out / 'key.json')
        except Exception as error:
            report.check(f'{name}：出包运行出错', False, f'{type(error).__name__}: {error}')
            return None, None

    def raises_usage(name, extra):
        try:
            build(name, extra)
        except UsageError:
            return True
        return False

    def known_diffs(key):
        return [r['loudness_diff_lu'] for r in key['trials'].values() if r['loudness_diff_lu'] is not None]

    aa_dir, aa_key = scenario('aa2afc', ['--aa'])
    abx_aa_dir, abx_aa_key = scenario('aaabx', ['--aa', '--mode', 'abx', '--sessions', '3'])
    gain_dir, gain_key = scenario('gain', with_b)
    abx_dir, abx_key = scenario('abxgain', with_b + ['--mode', 'abx', '--sessions', '3'])
    norm_dir, norm_key = scenario('normab', with_b + ['--normalize-lufs', '-14'])
    orig_dir, orig_key = scenario('normob', with_b + ['--original', orig, '--pair', 'O:B', '--normalize-lufs', '-14'])

    if aa_key is not None:
        registration = read_json(aa_dir / 'registration.json')
        windows = [(s['start_bar'], s['end_bar']) for s in registration['songs']['synth1']['segments']]
        report.check('登记：片段边界 = 每边界一个居中窗口（小节 10-17、28-35、46-53），3 个不同边界',
                     windows == [(10, 17), (28, 35), (46, 53)] and registration['songs']['synth1']['boundaries_used'] == 3, str(windows))
        report.check('登记：带决策规则、种子、容差、脚本 sha256',
                     registration['decision_rule']['min_wins_by_valid_trials']['12'] == 10 and registration['seed'] == 7
                     and registration['loudness']['tolerance_lu'] == LOUDNESS_TOLERANCE_LU and len(registration['script']['sha256']) == 64)
        songs = [r['song'] for r in aa_key['trials'].values()]
        pairs = [(r['song'], r['segment']) for r in aa_key['trials'].values()]
        report.check('编排：12 次试次 = 6 个片段各 2 次，两首歌各 6 次，会话 6 + 6',
                     sorted(songs.count(s) for s in set(songs)) == [6, 6] and sorted(pairs.count(p) for p in set(pairs)) == [2] * 6
                     and registration['session_sizes'] == [6, 6])
        rows = read_rows(aa_dir / 'package' / 'trials.csv')
        report.check('A/A：公开试次表里片段响度差全是 0、档位全 ok',
                     len(rows) == 12 and all(float(r['loudness_diff_abs_lu']) == 0.0 and r['loudness_flag'] == 'ok' for r in rows))
        again_dir, _ = scenario('aa2afc_again', ['--aa'])
        if again_dir is not None:
            report.check('重跑：同种子同输入 → 整个批次目录（登记、钥匙、表、全部 wav）逐字相同', tree_digest(aa_dir) == tree_digest(again_dir))
        seed_dir, seed_key = scenario('aa2afc_seed8', ['--aa', '--seed', '8'])
        if seed_key is not None:
            report.check('换种子 → 试次顺序与侧位不同', seed_key['trials'] != aa_key['trials'])
    if gain_key is not None:
        diffs = known_diffs(gain_key)
        report.check('等响记录：纯增益 ×2 → 每个片段响度差 +6.02 LU，档位 warn，照样出包',
                     len(diffs) == 12 and all(abs(d - SYN_GAIN_DB) < 0.002 for d in diffs) and all(r['loudness_flag'] == 'warn' for r in gain_key['trials'].values()),
                     f'差值范围 {min(diffs):.4f} ~ {max(diffs):.4f}' if diffs else '没有可算的差值')
        report.check('峰值报告：超过 0 dBFS 的试次有警告', any('0 dBFS' in w for w in gain_key['warnings']))
    for name, key in (('normab', norm_key), ('normob', orig_key)):
        if key is not None:
            diffs = known_diffs(key)
            worst = max((abs(d) for d in diffs), default=None)
            report.check(f'{name}：--normalize-lufs -14 后每个片段响度差 < 0.005 LU，档位全 ok',
                         len(diffs) == 12 and worst < 0.005 and all(r['loudness_flag'] == 'ok' for r in key['trials'].values()), f'最大 {worst} LU')
    if norm_key is not None:
        gains = norm_key['songs']['synth1']['normalize_gain_db']
        report.check('归一化：B 的增益比 A 低 6.02 dB（整曲各自对齐到 -14）', abs(gains['B'] - gains['A'] + SYN_GAIN_DB) < 0.01, str(gains))

    dry_out, dry_code = build('dry', with_b, dry=True)
    report.check('--dry-run：退出码 0，不写任何文件', dry_code == 0 and not dry_out.exists())
    report.check('参数检查：--pair 含 O 又不给 --normalize-lufs → 拒绝', raises_usage('usage1', ['--original', orig, '--pair', 'O:B'] + with_b))
    report.check('参数检查：--bars 5 → 拒绝', raises_usage('usage2', ['--aa', '--bars', '5']))
    report.check('参数检查：每会话 20 次 → 拒绝', raises_usage('usage3', ['--aa', '--trials', '40', '--sessions', '2']))
    report.check('参数检查：输出目录已有批次 → 不覆盖', raises_usage('aa2afc', ['--aa']))

    if gain_key is not None:
        cand, base = pick_answers(gain_key, 'candidate'), pick_answers(gain_key, 'baseline')
        ids = sorted(cand)
        first = score_case(report, parser, work, 'gain-candidate', gain_dir, cand, 'pass',
                           lambda r: r['overall']['preference'] == 1.0 and r['overall']['valid'] == 12
                           and r['sensitivity_excluding_flagged']['trials'] == 0 and r['sensitivity_excluding_flagged']['p_one_sided'] is None)
        again = score_case(report, parser, work, 'gain-candidate-again', gain_dir, cand, 'pass')
        report.check('score：同输入重跑 result.md / result.json 逐字相同',
                     all((first / name).read_bytes() == (again / name).read_bytes() for name in ('result.md', 'result.json')))
        score_case(report, parser, work, 'gain-baseline', gain_dir, base, 'fail', lambda r: r['overall']['preference'] == 0.0)
        score_case(report, parser, work, 'gain-always-1', gain_dir, {tid: '1' for tid in ids}, 'fail',
                   lambda r: abs(r['overall']['preference'] - 0.5) < 1e-9)
        score_case(report, parser, work, 'gain-8-of-12', gain_dir, {tid: (cand if i < 8 else base)[tid] for i, tid in enumerate(ids)}, 'fail',
                   lambda r: abs(r['overall']['preference'] - 8 / 12) < 1e-5 and abs(r['overall']['p_one_sided'] - 794 / 4096) < 1e-5)
        score_case(report, parser, work, 'gain-9-and-3-none', gain_dir, {tid: (cand[tid] if i < 9 else 'none') for i, tid in enumerate(ids)}, 'pass',
                   lambda r: r['overall']['valid'] == 9 and r['overall']['none'] == 3 and abs(r['overall']['p_one_sided'] - 1 / 512) < 1e-6)
        score_case(report, parser, work, 'gain-missing-one', gain_dir, {tid: cand[tid] for tid in ids[:-1]}, 'incomplete',
                   lambda r: r['missing_trials'] == [ids[-1]])
    if aa_key is not None:
        score_case(report, parser, work, 'aa-always-1', aa_dir, {tid: '1' for tid in aa_key['trials']}, 'aa-no-leak')
        score_case(report, parser, work, 'aa-simulated-leak', aa_dir, pick_answers(aa_key, 'candidate'), 'aa-suspected-leak')
    if abx_aa_key is not None:
        score_case(report, parser, work, 'abx-aa-always-a', abx_aa_dir, {tid: 'a' for tid in abx_aa_key['trials']}, 'aa-no-leak',
                   lambda r: r['overall']['accuracy'] == 0.5)
        score_case(report, parser, work, 'abx-aa-simulated-leak', abx_aa_dir, pick_answers(abx_aa_key, 'candidate'), 'aa-suspected-leak')
    if abx_key is not None:
        score_case(report, parser, work, 'abx-all-correct', abx_dir, pick_answers(abx_key, 'candidate'), 'pass',
                   lambda r: r['overall']['correct'] == 12 and r['required_wins'] == 10)
        score_case(report, parser, work, 'abx-all-wrong', abx_dir, pick_answers(abx_key, 'baseline'), 'fail')


def cmd_selfcheck(args):
    report = Report()
    guarded(report, '统计已知值', selfcheck_stats)
    guarded(report, '片段窗口', selfcheck_windows)
    if args.workdir:
        work = Path(args.workdir)
        if work.exists() and any(work.iterdir()):
            raise UsageError(f'--workdir 要是空目录或不存在：{work}')
        work.mkdir(parents=True, exist_ok=True)
        guarded(report, '合成素材出包与评分', selfcheck_builds, work)
    else:
        with tempfile.TemporaryDirectory(prefix='blind-selfcheck-') as tmp:
            guarded(report, '合成素材出包与评分', selfcheck_builds, Path(tmp))
    failures = report.failures()
    print(f'\nselfcheck：{len(report.rows)} 项，失败 {len(failures)}' + ('' if not failures else '：' + '；'.join(failures)))
    return 1 if failures else 0


# ---------------------------------------------------------------------------------------------------------------------
# 命令行

def make_parser():
    parser = argparse.ArgumentParser(
        description='Celadon 盲测脚本（T-096b）：build 出包 / score 统计 / selfcheck 自检。口径见文件头注释与 docs/validation/2026-10-11-T096b-blind-test/README.md。',
        allow_abbrev=False)
    sub = parser.add_subparsers(dest='command', required=True)

    build = sub.add_parser('build', allow_abbrev=False, help='出盲测包（--dry-run 只预演）')
    build.add_argument('--out', required=True, help='批次目录（DGX 上，如 ~/.cache/celadon-blind/<批次>/）；已存在且非空会拒绝')
    build.add_argument('--batch', help='批次名，会写进 LISTEN.txt 下发到 Mac：取中性名字，别写 A / B 的设置；默认取 --out 的目录名')
    build.add_argument('--a', help='条件 A（基线）的渲染输出目录')
    build.add_argument('--b', help='条件 B（候选）的渲染输出目录')
    build.add_argument('--original', help='条件 O（原曲）的目录；配 --pair O:A 或 O:B，并必须给 --normalize-lufs')
    build.add_argument('--glob-a', default='*.wav', help='A 目录里的文件匹配（默认 *.wav）；A 与 B 可以是同一目录、用不同 glob 区分')
    build.add_argument('--glob-b', default='*.wav', help='B 目录里的文件匹配（默认 *.wav）')
    build.add_argument('--glob-o', default='*.wav', help='原曲目录里的文件匹配；T-038 的布局用 */*/*.wav')
    build.add_argument('--a-desc', help='A 的自由说明（提交号、参数），只写进登记')
    build.add_argument('--b-desc', help='B 的自由说明，只写进登记')
    build.add_argument('--o-desc', help='O 的自由说明，只写进登记')
    build.add_argument('--pair', default='A:B', help='基线:候选，默认 A:B；R1-05 用 O:A 或 O:B（原曲对成品）。偏好百分比算的是候选')
    build.add_argument('--aa', action='store_true', help='A/A 自检包：B 侧读 A 的同一批文件（同一文件两侧）')
    build.add_argument('--normalize-lufs', type=float,
                       help='每个条件的整曲用 pyloudnorm 量响度、只乘一个增益对齐到这个值（R1-05 用 -14）；不给 = 信任渲染时的 --match-lufs')
    build.add_argument('--labels', help='T-038 逐小节标签 TSV（class / track / bar / label）')
    build.add_argument('--labels-override', help='T-038 改标 TSV（可选，叠在 --labels 上）')
    build.add_argument('--tempo', help='T-038 tempo-manifest.tsv（至少 track / bpm 两列）')
    build.add_argument('--segments', help='显式片段 JSON：{"歌名": [[起点秒, 终点秒], ...]}；写了的歌不再看标签')
    build.add_argument('--tracks', help='只用这些歌（逗号分隔的歌名）')
    build.add_argument('--bars', type=int, default=DEFAULT_BARS, help=f'片段长度（小节），{BARS_MIN}～{BARS_MAX}，默认 {DEFAULT_BARS}')
    build.add_argument('--segments-per-song', type=int, default=DEFAULT_SEGMENTS_PER_SONG, help='每首歌的片段数，默认 3')
    build.add_argument('--fade-ms', type=float, default=FADE_MS_DEFAULT, help=f'淡入淡出毫秒数，至少 {FADE_MS_MIN:g}，默认 {FADE_MS_DEFAULT:g}')
    build.add_argument('--mode', choices=MODES, default='2afc', help='2afc 偏好（默认）或 abx 区分')
    build.add_argument('--trials', type=int, help='试次数；默认 2AFC 18、ABX 40')
    build.add_argument('--sessions', type=int, help='会话数；默认 2AFC 2、ABX 3；每会话不超过 15 次')
    build.add_argument('--seed', type=int, required=True, help='随机种子（写进登记；同种子同输入出同一个包）')
    build.add_argument('--dry-run', action='store_true', help='只预演：打印片段与会话计划，不写任何文件、不量响度')
    build.set_defaults(func=cmd_build)

    score = sub.add_parser('score', allow_abbrev=False, help='读答卷出统计')
    score.add_argument('--batch', required=True, help='批次目录（里面有 key.json 与 registration.json）')
    score.add_argument('--answers', required=True, help='填好的 answers.csv')
    score.add_argument('--out', help='result.md / result.json 的输出目录；默认与答卷同目录')
    score.set_defaults(func=cmd_score)

    selfcheck = sub.add_parser('selfcheck', allow_abbrev=False, help='合成素材自检（A/A、统计已知值、重跑逐字相同）')
    selfcheck.add_argument('--workdir', help='自检的工作目录（空目录或不存在；用完不删，方便查）；默认用临时目录并在结束时删除')
    selfcheck.set_defaults(func=cmd_selfcheck)
    return parser


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except UsageError as error:
        parser.error(str(error))                   # 打印用法并以退出码 2 结束
    except ToolError as error:
        print(f'blind_test：{error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())

# @@PART@@
