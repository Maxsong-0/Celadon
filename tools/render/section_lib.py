#!/usr/bin/env python3
"""段落工具（T-096 量尺用）：段落标注 JSON、Jamendo 逐小节标签、celadon_render --segments-csv 的逐段统计。

只用标准库，纯读取与换算，不写文件。quality_metrics.py、blind_test.py、master_check.py 共用这一份，不各自另抄。

来历：这些函数原来在 T-038 的 house_gate.py（段落响度、标注读取）与 calibrate_house.py（逐小节标签）里；
ADR-039 删掉曲风时那两个脚本整个删了，与曲风 / 交付无关的读法与口径原样搬到这里（算术没动）。
格式：
  - 段落标注 JSON（make_synthetic_house.py 写的那种）：{"song", "bpm", ["meter"], "sections": [{"name", "kind", "start_bar", "end_bar"}]}，
    kind ∈ intro / drop / breakdown / 其他；小节从 1 起、两端都含；按 song（= 文件名去扩展名）对上。
  - 逐小节标签 TSV：class<TAB>track<TAB>bar<TAB>label；改标 TSV：track<TAB>first_bar<TAB>last_bar<TAB>label（闭区间，# 开头是注释）。
  - 逐段统计 CSV（--segments-csv）：segment, lufs, valid_s, mode, …；按小节分时 mode = bars、段号 = 小节号。
"""
import csv
import json
import math
import os

SECTION_KINDS = ('intro', 'drop', 'breakdown', 'outro')   # 四类段落；transition（交界 ±1 小节）与 exclude（人工不计）不进任何统计


def num(value, default=math.nan):
    """任何东西 → float；转不了读 default。"""
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def lufs_to_energy(lufs):
    return 10.0 ** ((lufs + 0.691) / 10.0)


def energy_to_lufs(energy):
    return -0.691 + 10.0 * math.log10(energy) if energy > 0.0 else math.nan


# ---------------------------------------------------------------------------------------------------------------------
# 段落标注 JSON

def load_annotations(paths):
    """标注 JSON 文件或目录（目录取第一层的 *.json）→ {歌名: 标注}。缺 song / sections 的 JSON 跳过；
    段缺 kind 时按 name 去掉尾部数字与空格、转小写补上（「Drop 2」→ drop）。"""
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


def drop_spans_seconds(annotation):
    """段落标注 -> [(开始秒, 结束秒)]，只取 kind == 'drop' 的段。小节从 1 起、两端都含；第 1 小节从 0 s 起
    （标注按「素材裁到小节线」做的）。每小节秒数 = 每小节拍数 × 60 / bpm，拍数取 meter 的分子（缺省 4/4）。
    标注缺 bpm、sections 等换算不了时抛 ValueError。没有 drop 段返回空列表。"""
    try:
        bpm = float(annotation['bpm'])
        beats = int(str(annotation.get('meter', '4/4')).split('/')[0])
        bar_s = beats * 60.0 / bpm
        return [((int(s['start_bar']) - 1) * bar_s, int(s['end_bar']) * bar_s)
                for s in annotation['sections'] if s.get('kind') == 'drop']
    except (KeyError, TypeError, ValueError, ZeroDivisionError) as error:
        raise ValueError(f'标注无法换算成秒（需要 bpm 与 sections）：{type(error).__name__} {error}') from error


def find_annotation(annotations, stem):
    """按文件名（不含扩展名）找标注：先整名相等，再认 celadon_render 的「<歌名>__g…」命名，取最长的歌名前缀。没有返回 None。"""
    if stem in annotations:
        return annotations[stem]
    prefixes = [song for song in annotations if stem.startswith(song + '__')]
    return annotations[max(prefixes, key=len)] if prefixes else None


# ---------------------------------------------------------------------------------------------------------------------
# Jamendo 逐小节标签（T-038）

def load_labels(auto_path, override_path=None):
    """(曲, 小节) → 段落标签。先读自动标签 auto_path，再叠改标 override_path（可不给）。
    只改已有的小节；改完重标交界：相邻两小节分属四类里不同的两类，两小节都改 transition
    （与自动标签同一规则；transition / exclude 不参与比较，不会连锁扩开）。"""
    labels = {}
    with open(auto_path, encoding='utf-8') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            labels[(r['track'], int(r['bar']))] = r['label']
    if override_path:
        changed = set()
        with open(override_path, encoding='utf-8') as f:
            for r in csv.DictReader((line for line in f if not line.startswith('#')), delimiter='\t'):
                for bar in range(int(r['first_bar']), int(r['last_bar']) + 1):
                    if (r['track'], bar) in labels:
                        labels[(r['track'], bar)] = r['label']
                        changed.add(r['track'])
        for track in changed:
            bars = sorted(b for t, b in labels if t == track)
            original = {b: labels[(track, b)] for b in bars}
            for prev, cur in zip(bars, bars[1:]):
                if original[prev] in SECTION_KINDS and original[cur] in SECTION_KINDS and original[prev] != original[cur]:
                    labels[(track, prev)] = labels[(track, cur)] = 'transition'
    return labels


# ---------------------------------------------------------------------------------------------------------------------
# celadon_render --segments-csv 的逐段统计

def load_segments(directory, relative):
    """segments/<名>.csv → {段号: 行}（段号从 1 起，按小节分时 = 小节号）。读不到返回 None。"""
    if not relative:
        return None
    path = os.path.join(directory, relative)
    if not os.path.isfile(path):
        return None
    with open(path, encoding='utf-8') as f:
        return {int(r['segment']): r for r in csv.DictReader(f)}


def segments_are_bars(segments):
    return bool(segments) and all(r.get('mode') == 'bars' for r in segments.values())


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
