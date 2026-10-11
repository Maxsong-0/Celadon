#!/usr/bin/env python3
"""R6-02 前置（T-096e）：T-085 的 13 首在含 T-089 的 main 上重渲，量热混音（入口比偏好响度更响的歌）被多压多少。

只渲染、只出报告，不改任何源码；工具只报告，不改渲染器、不回退增益。

子命令（全流程命令见 docs/validation/2026-10-11-T096e-r602-rerender/README.md）：
  prepare   按 corpus.csv 重下 9 首 Jamendo 并解码、生成 4 首合成，核对与 T-085 输入是不是同一份 → input_check.csv
  render    用一个 celadon_render 二进制跑整套矩阵，每格一份 metrics.csv 原样入库，另写 environment.json
  report    新旧并排：side_by_side.csv + REPORT.md（表格与逐格判定由本脚本生成，结论段留给人填）
  selftest  用仓库里 T-085 的三份旧 CSV 校对 L_t 推导与并排逻辑（只用标准库），退出 0 / 1

依赖：report、selftest、两个 --dry-run 只用标准库；prepare 真跑才要 numpy、scipy、soundfile、pyloudnorm、ffmpeg、curl，
它们经 codec_tp_check 懒加载。量具（响度、真峰值）与输入配方（下载解码、合成）只在 codec_tp_check.py 定义一次，这里 import、不复制。

出错规则（Max 2026-10-10 Q12）：基础设施错（文件读不到、渲染失败、列缺）→ 报错、退出码 1；素材原因算不出（没有画像、没有
GLAZE 0 下限）→ 写空值并在 na_reason 列写原因，绝不填 0。
"""
import argparse
import concurrent.futures
import csv
import hashlib
import json
import math
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ============================================================================================================
# 常量：矩阵、判据、L_t 的镜像常数，都集中在这一处（改判据只改这里）
# ============================================================================================================

REPO_ROOT = Path(__file__).resolve().parents[2]
T085_DIR = REPO_ROOT / 'docs' / 'validation' / '2026-10-08-T085-codec-tp'
DEFAULT_REPORT_DIR = REPO_ROOT / 'docs' / 'validation' / '2026-10-11-T096e-r602-rerender'

# ---- 渲染矩阵（计划 §13 Q7 / Q12；命令行与 T-085 一致，见 codec_tp_check.py:226-228）----
STREAMING_TARGETS = (-10.0, -12.0, -14.0)   # 流媒体目标（--streaming-target-lufs）
CLUB_TARGET = -7.0                          # 俱乐部用出厂值，没有覆盖选项；只用来命名和算 L_t
GLAZE_LEVELS = (0.0, 35.0, 100.0)           # 0 = 不推时的下限，35 = 出厂默认，100 = 与 T-085 对齐
RENDER_GENRE = 'general'
RENDER_FIXED_FLAGS = ('--style', 'auto', '--clay', 'auto')
RENDER_TAIL_FLAGS = ('--no-align', '--no-replay', '--metrics-only')
DELIVERY_ORDER = {'streaming': 0, 'club': 1}

# 每个二进制的标签；值 = 这个二进制含不含 T-089（1a0e26a，请求再封到「目标 − 输入」）。't085' 只给读旧 CSV 用。
LABEL_HAS_T089 = {'main': True, 'pre-t089': False, 't085': False}
PRIMARY_LABEL = 'main'
OLD_LABEL = 't085'
T085_GLAZE = 100.0                          # T-085 只渲了 GLAZE 100 的流媒体档，没有俱乐部、没有别的 GLAZE
SYNTHETIC_PREFIX = 'stress_'                # 合成压力曲的文件名前缀；其余是真歌
EXPECTED_SYNTHETIC_COUNT = 4

# ---- 「问题还在」的判据（完成标准，阈值由 Max 认；计划 §13 Q7）----
HOT_GAP_LU = 0.5                            # I − L_t 超过它的格子叫热格：入口已经比偏好响度更响
PROBLEM_GR_MEAN_DB = 0.3                    # limit_gr_mean_db 达到它算「压得多」（ADR-033 的起点值；该列是块峰值平均、偏高）
ABOVE_FLOOR_MARGIN_DB = 0.0                 # GR 均值要比同档 GLAZE 0 的 GR 均值高出多少才算「高于下限」（0 = 严格大于）
PROBLEM_TARGETS = (-10.0, -14.0)            # 「问题还在」只看流媒体这两档的真歌

# ---- 输入核对（计划 §13 Q12）----
INPUT_TOL_LUFS = 0.05                       # 重建输入与 T-085 input 行的 LUFS 偏差超过它 → 输入不同
INPUT_TOL_TP_DB = 0.05                      # 真峰值同理

# ---- L_t 自检 ----
SELFCHECK_TOL_DB = 0.1                      # 不受约束的曲，plan_requested_target_lufs 与预期偏差应小于它
SELFCHECK_MIN_PUSH_DB = 0.5                 # 含 T-089 的二进制：L_t − I 超过它才算「请求等于缺口」，不被封顶改写
PROFILE_SPREAD_TOL_DB = 0.01                # 同一首歌的 profile_lufs 在各格之间的差应小于它（画像与交付档、GLAZE 无关）

# ---- L_t 的镜像常数：Source/dsp/GlazePlanner.cpp:1648-1663 的公式用到的全部常数 ----
# 流媒体 / 俱乐部：(目标, 下限, 上限) LUFS，Source/dsp/GlazeConstants.h:174-181
STREAMING_FACTORY = (-10.0, -12.0, -8.0)
CLUB_FACTORY = (-7.0, -9.0, -5.0)
STYLE_OFFSET_REFERENCE_LUFS = -9.0          # GlazeConstants.h:180（风格偏移 = 混合偏好 − 本项）
STYLE_PREFERRED_LUFS = {'warm': -8.5, 'punchy': -9.0, 'clean': -10.0}   # GlazeConstants.cpp:71, 94, 117
LRA_REFERENCE_LU = 6.0                      # GlazeConstants.h:204
LRA_SLOPE_DB_PER_LU = 1.0 / 3.0             # GlazeConstants.h:205
MIN_FULL_DRIVE_DB = 3.0                     # GlazeConstants.h:336（最小推响量；T-089 之前的请求 = max(它, L_t − I)）

# ---- 判定文字（side_by_side.csv 与 REPORT.md 共用）----
V_COLD = '非热格'
V_FLOOR = 'GLAZE 0（下限）'
V_UNDER = '低于阈值'
V_OVER = '高于下限（问题在）'
V_INHERENT = '输入自带（落在下限）'
V_NA = '算不出'
T085_SIDE = '并排'
T085_DIFF = '输入不同，不并排旧数'
T085_UNCHECKED = '输入未核对，不并排旧数'
T085_NO_CELL = 'T-085 无此格'
T085_NO_SONG = 'T-085 无此曲'

# ---- 文件名 ----
LABEL_PATTERN = r'[a-z0-9]+(?:-[a-z0-9]+)*'
METRICS_NAME = re.compile(r'^metrics_(?P<label>' + LABEL_PATTERN + r')_(?P<delivery>streaming|club)_'
                          r'(?P<target>-?\d+(?:\.\d+)?)_g(?P<glaze>\d+(?:\.\d+)?)\.csv$')
OLD_NAME = re.compile(r'^render_target_(?P<target>-?\d+(?:\.\d+)?)\.csv$')

# metrics.csv 里本脚本要读的列（缺列 = 基础设施错）
REQUIRED_COLUMNS = ('kind', 'song', 'glaze', 'lufs', 'true_peak_dbtp', 'limit_gr_mean_db', 'limit_gr_max_db', 'audio_hash',
                    'genre', 'delivery', 'plan_gain_db', 'plan_requested_gain_db', 'plan_requested_target_lufs',
                    'weight_warm', 'weight_punchy', 'weight_clean', 'profile_lufs', 'profile_lra_lu')

INPUT_CHECK_FIELDS = ('file', 'kind', 'frames', 'frames_t085', 'frames_match', 'sha256', 'sha256_t085', 'sha_match',
                      'mp3_sha256', 'mp3_sha256_corpus', 'mp3_sha_match', 'lufs_pyln', 'tp8_pyln', 'lufs_t085', 'tp_t085',
                      'd_lufs', 'd_tp', 'comparable', 'reason')

SIDE_FIELDS = ('label', 'song', 'kind', 'delivery', 'target_lufs', 'glaze',
               'profile_lufs', 'profile_source', 'lra_lu', 'weight_warm', 'weight_punchy', 'weight_clean',
               'lt_lufs', 'i_minus_lt', 'hot', 'out_lufs', 'out_minus_lt',
               'limit_gr_mean_db', 'limit_gr_max_db', 'gr_glaze0_mean_db', 'gr_minus_glaze0_db',
               'plan_gain_db', 'plan_requested_gain_db', 'plan_requested_target_lufs', 'manual_net_db', 'implied_push_db',
               'lt_selfcheck_dev_db', 'verdict',
               't085_status', 't085_profile_lufs', 't085_out_lufs', 't085_gr_mean_db', 'gr_minus_t085_db', 'na_reason')
FOUR_DIGIT_FIELDS = ('weight_warm', 'weight_punchy', 'weight_clean')
GENERAL_NUMBER_FIELDS = ('target_lufs', 'glaze')

CONCLUSION_BEGIN = '<!-- CONCLUSION:BEGIN -->'
CONCLUSION_END = '<!-- CONCLUSION:END -->'
CONCLUSION_PLACEHOLDER = '（待主会话填写：按「机器判定」一节的数据下结论，并写明是否进入 ADR-033 的实现、还是降级 / 搁置。）'


class InfraError(Exception):
    """基础设施错（文件读不到、渲染失败、列缺）：main 打印后以退出码 1 结束。"""


# ============================================================================================================
# 通用小工具（文件、数字、格式）
# ============================================================================================================

def to_float(text) -> Optional[float]:
    """CSV 单元格转浮点；空、NaN、破折号都当「没有」返回 None，绝不当 0。"""
    if text is None:
        return None
    text = str(text).strip()
    if text in ('', '—', '-') or text.lower() in ('nan', 'inf', '-inf'):
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


def fmt(value, digits: int = 3) -> str:
    """数字转文本；None → 空串；避免出现 -0.000。"""
    if value is None:
        return ''
    if abs(value) < 0.5 * 10 ** (-digits):
        value = 0.0
    return f'{value:.{digits}f}'


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    try:
        with Path(path).open(newline='', encoding='utf-8-sig') as stream:
            return list(csv.DictReader(stream))
    except OSError as exc:
        raise InfraError(f'读不了 {path}：{exc}')


def write_csv(path: Path, rows: List[Dict[str, str]], fieldnames) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(fieldnames), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def read_json(path: Path) -> dict:
    path = Path(path)
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise InfraError(f'读不了 {path}：{exc}')


def write_json(path: Path, data: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def host_info() -> Dict[str, str]:
    return {'machine': platform.machine(), 'platform': platform.platform(), 'python': platform.python_version()}


def song_of(file_name: str) -> str:
    """input_manifest 的文件名 → metrics.csv 的 song 列（去掉扩展名）。"""
    return Path(file_name).stem


def is_synthetic(song: str) -> bool:
    return song.startswith(SYNTHETIC_PREFIX)


def target_text(target: float) -> str:
    return f'{target:g}'


def cell_name(delivery: str, target: float, glaze: float) -> str:
    return f'{delivery}_{target_text(target)}_g{glaze:g}'


def metrics_file_name(label: str, delivery: str, target: float, glaze: float) -> str:
    return f'metrics_{label}_{cell_name(delivery, target, glaze)}.csv'


def parse_metrics_name(name: str) -> Optional[Tuple[str, str, float, float]]:
    match = METRICS_NAME.match(name)
    if not match:
        return None
    return match['label'], match['delivery'], float(match['target']), float(match['glaze'])


def update_environment(out_dir: Path, section: str, key: Optional[str], payload: dict) -> None:
    """environment.json 里一个小节的读-改-写：key 为空时整节替换，否则替换 section[key]。prepare 与 render 共用同一份文件。"""
    path = Path(out_dir) / 'environment.json'
    data = read_json(path)
    data.setdefault('schema', 1)
    if key is None:
        data[section] = payload
    else:
        data.setdefault(section, {})[key] = payload
    write_json(path, data)


# ============================================================================================================
# L_t（偏好响度）的离线推导：L_t 不在 metrics.csv 里，照 GlazePlanner.cpp:1648-1663 用 CSV 里已有的列推
# ============================================================================================================

def sanitise_weights(warm, punchy, clean) -> Tuple[float, float, float]:
    """照 GlazePlanner.cpp:120-135：非有限值与负数按 0，再归一；全为 0 → 通透。"""
    values = [w if (w is not None and math.isfinite(w) and w > 0.0) else 0.0 for w in (warm, punchy, clean)]
    total = sum(values)
    if not total > 0.0:
        return 0.0, 0.0, 1.0
    return values[0] / total, values[1] / total, values[2] / total


def delivery_range(delivery: str, target: float) -> Tuple[float, float, float]:
    """(目标, 下限, 上限)。--streaming-target-lufs 把流媒体三项一起平移（RenderMain.cpp:112-129）；俱乐部没有覆盖。"""
    if delivery == 'streaming':
        shift = target - STREAMING_FACTORY[0]
        return target, STREAMING_FACTORY[1] + shift, STREAMING_FACTORY[2] + shift
    if delivery == 'club':
        if target != CLUB_FACTORY[0]:
            raise InfraError(f'俱乐部档没有目标覆盖选项，目标只能是 {CLUB_FACTORY[0]:g}，收到 {target:g}')
        return CLUB_FACTORY
    raise InfraError(f'不认识的交付档：{delivery}')


def derive_preferred_lufs(delivery: str, target: float, weights, lra: Optional[float]) -> float:
    """L_t = clamp(档目标 + (风格混合偏好 − 参考) − LRA 修正, 档下限, 档上限)；weights = (暖, 冲击, 通透)。LRA 缺失按参考值（修正 0）。"""
    w_warm, w_punchy, w_clean = sanitise_weights(*weights)
    style_preferred = (w_warm * STYLE_PREFERRED_LUFS['warm'] + w_punchy * STYLE_PREFERRED_LUFS['punchy']
                       + w_clean * STYLE_PREFERRED_LUFS['clean'])
    style_offset = style_preferred - STYLE_OFFSET_REFERENCE_LUFS
    lra_value = LRA_REFERENCE_LU if lra is None else lra
    lra_correction = LRA_SLOPE_DB_PER_LU * max(0.0, lra_value - LRA_REFERENCE_LU)
    base, low, high = delivery_range(delivery, target)
    return min(max(base + style_offset - lra_correction, low), high)


# ============================================================================================================
# 渲染矩阵与命令（纯函数，render 与 selftest 共用）
# ============================================================================================================

def matrix_cells() -> List[Tuple[str, float, float]]:
    """(交付档, 目标, GLAZE)：流媒体 3 个目标 × 3 个 GLAZE，加俱乐部 × 3 个 GLAZE，共 12 格。"""
    cells = [('streaming', target, glaze) for target in STREAMING_TARGETS for glaze in GLAZE_LEVELS]
    cells += [('club', CLUB_TARGET, glaze) for glaze in GLAZE_LEVELS]
    return cells


def render_command(binary: Path, input_dir: Path, output_dir: Path, delivery: str, target: float, glaze: float) -> List[str]:
    """与 T-085 的命令（codec_tp_check.py:226-228）同参数；多一个 --metrics-only，俱乐部不传 --streaming-target-lufs。"""
    command = [str(binary), '--in', str(input_dir), '--out', str(output_dir), '--glaze', f'{glaze:g}',
               *RENDER_FIXED_FLAGS, '--genre', RENDER_GENRE, '--delivery', delivery]
    if delivery == 'streaming':
        command += ['--streaming-target-lufs', f'{target:g}']
    command += list(RENDER_TAIL_FLAGS)
    return command


# ============================================================================================================
# 读 CSV → 格（Cell）
# ============================================================================================================

@dataclass
class Cell:
    """metrics.csv 里一首歌在一格（交付档 × 目标 × GLAZE）的 render 行，加上它的画像列。None = 这一项没有读数。"""
    label: str
    song: str
    delivery: str
    target: float
    glaze: float
    out_lufs: Optional[float] = None
    out_tp: Optional[float] = None
    gr_mean: Optional[float] = None
    gr_max: Optional[float] = None
    plan_gain: Optional[float] = None
    plan_req_gain: Optional[float] = None
    plan_req_target: Optional[float] = None
    profile_lufs: Optional[float] = None
    lra: Optional[float] = None
    w_warm: Optional[float] = None
    w_punchy: Optional[float] = None
    w_clean: Optional[float] = None
    profile_source: str = '同格'


@dataclass
class InputInfo:
    """metrics.csv 里 input 行的读数（渲染器自己的量具）。"""
    lufs: Optional[float]
    tp: Optional[float]
    audio_hash: str


def profile_complete(cell: Cell) -> bool:
    return None not in (cell.profile_lufs, cell.w_warm, cell.w_punchy, cell.w_clean)


def read_metrics_csv(path: Path) -> List[Dict[str, str]]:
    rows = read_csv_rows(path)
    if not rows:
        raise InfraError(f'{path} 是空的')
    missing = [name for name in REQUIRED_COLUMNS if name not in rows[0]]
    if missing:
        raise InfraError(f'{path} 缺列：{", ".join(missing)}')
    return rows


def parse_run_rows(rows: List[Dict[str, str]], label: str, delivery: str, target: float, glaze: float,
                   source: str) -> Tuple[Dict[str, Cell], Dict[str, InputInfo]]:
    """一份 metrics.csv（一格）→ 每首歌的 Cell 与 input 行读数；文件名里的交付档 / GLAZE 与行里的列对不上就报错。"""
    cells: Dict[str, Cell] = {}
    inputs: Dict[str, InputInfo] = {}
    for row in rows:
        song = row['song']
        if row['kind'] == 'input':
            inputs[song] = InputInfo(to_float(row['lufs']), to_float(row['true_peak_dbtp']), row['audio_hash'])
        elif row['kind'] == 'render':
            if song in cells:
                raise InfraError(f'{source}：{song} 有不止一个 render 行')
            row_glaze = to_float(row['glaze'])
            if row_glaze is None or abs(row_glaze - glaze) > 1e-9:
                raise InfraError(f'{source}：{song} 的 glaze 列是 {row["glaze"]!r}，与文件名的 {glaze:g} 不符')
            if row['delivery'] != delivery or row['genre'] != RENDER_GENRE:
                raise InfraError(f'{source}：{song} 的 delivery / genre 是 {row["delivery"]!r} / {row["genre"]!r}，'
                                 f'应为 {delivery} / {RENDER_GENRE}')
            cells[song] = Cell(label=label, song=song, delivery=delivery, target=target, glaze=glaze,
                               out_lufs=to_float(row['lufs']), out_tp=to_float(row['true_peak_dbtp']),
                               gr_mean=to_float(row['limit_gr_mean_db']), gr_max=to_float(row['limit_gr_max_db']),
                               plan_gain=to_float(row['plan_gain_db']), plan_req_gain=to_float(row['plan_requested_gain_db']),
                               plan_req_target=to_float(row['plan_requested_target_lufs']),
                               profile_lufs=to_float(row['profile_lufs']), lra=to_float(row['profile_lra_lu']),
                               w_warm=to_float(row['weight_warm']), w_punchy=to_float(row['weight_punchy']),
                               w_clean=to_float(row['weight_clean']))
    return cells, inputs


def donor_sort_key(cell: Cell):
    """借画像时的优先顺序：GLAZE 高的先，再流媒体先于俱乐部，再目标从 −10 往下。"""
    return -cell.glaze, DELIVERY_ORDER[cell.delivery], -cell.target


def fill_profiles(cells: List[Cell]) -> None:
    """画像（I、风格权重、LRA）与交付档、GLAZE 无关：某一格的画像列是空的（比如 GLAZE 0 没有方案列）时，借同标签同一首歌别的格的。"""
    groups: Dict[Tuple[str, str], List[Cell]] = {}
    for cell in cells:
        groups.setdefault((cell.label, cell.song), []).append(cell)
    for group in groups.values():
        donors = sorted((c for c in group if profile_complete(c)), key=donor_sort_key)
        for cell in group:
            if profile_complete(cell):
                continue
            if not donors:
                cell.profile_source = '无'
                continue
            donor = donors[0]
            cell.profile_lufs, cell.lra = donor.profile_lufs, donor.lra
            cell.w_warm, cell.w_punchy, cell.w_clean = donor.w_warm, donor.w_punchy, donor.w_clean
            cell.profile_source = f'借自 {donor.delivery} {donor.target:g} g{donor.glaze:g}'


def profile_spread_issues(cells: List[Cell]) -> List[str]:
    """同一首歌自己有画像的格之间，profile_lufs 不该有差（画像只由输入决定）；差超过容差就列出来。"""
    groups: Dict[Tuple[str, str], List[float]] = {}
    for cell in cells:
        if cell.profile_source == '同格' and cell.profile_lufs is not None:
            groups.setdefault((cell.label, cell.song), []).append(cell.profile_lufs)
    issues = []
    for (label, song), values in sorted(groups.items()):
        spread = max(values) - min(values)
        if spread > PROFILE_SPREAD_TOL_DB:
            issues.append(f'{label} {song}：profile_lufs 在 {len(values)} 格里相差 {spread:.3f} dB（容差 {PROFILE_SPREAD_TOL_DB}）')
    return issues


def expand_old_paths(paths) -> List[Path]:
    """--old 可以给目录（取里面的 render_target_*.csv）或具体文件。"""
    result: List[Path] = []
    for path in paths:
        path = Path(path)
        if path.is_dir():
            result.extend(sorted(p for p in path.iterdir() if OLD_NAME.match(p.name)))
        else:
            result.append(path)
    if not result:
        raise InfraError('--old 里没有 render_target_*.csv')
    return result


def load_t085(paths: List[Path]) -> Tuple[Dict[Tuple[str, float], Cell], Dict[str, InputInfo]]:
    """T-085 的 render_target_<目标>.csv → {(歌, 目标): Cell}（都是流媒体 GLAZE 100）与每首歌的 input 行读数。"""
    cells: Dict[Tuple[str, float], Cell] = {}
    inputs: Dict[str, InputInfo] = {}
    for path in paths:
        match = OLD_NAME.match(Path(path).name)
        if not match:
            raise InfraError(f'{Path(path).name} 不是 render_target_<目标>.csv')
        target = float(match['target'])
        run_cells, run_inputs = parse_run_rows(read_metrics_csv(path), OLD_LABEL, 'streaming', target, T085_GLAZE, Path(path).name)
        for song, cell in run_cells.items():
            cells[(song, target)] = cell
        for song, info in run_inputs.items():
            inputs.setdefault(song, info)
    fill_profiles(list(cells.values()))
    return cells, inputs


def discover_runs(new_dir: Path) -> List[Tuple[Path, str, str, float, float]]:
    """新目录里的 metrics_<标签>_<交付档>_<目标>_g<GLAZE>.csv；认不得的标签只提醒、不读。"""
    runs = []
    for path in sorted(Path(new_dir).glob('metrics_*.csv')):
        parsed = parse_metrics_name(path.name)
        if parsed is None:
            continue
        label, delivery, target, glaze = parsed
        if label not in LABEL_HAS_T089 or label == OLD_LABEL:
            print(f'提醒：{path.name} 的标签 {label!r} 不在 {sorted(k for k in LABEL_HAS_T089 if k != OLD_LABEL)} 里，已跳过', file=sys.stderr)
            continue
        runs.append((path, label, delivery, target, glaze))
    return runs


def load_new_cells(runs) -> Tuple[Dict[tuple, Cell], Dict[str, Dict[str, InputInfo]]]:
    """新的 metrics 文件 → {(标签, 歌, 交付档, 目标, GLAZE): Cell} 与 {标签: {歌: input 读数}}；同标签里同一首歌的输入变过就报错。"""
    cells: Dict[tuple, Cell] = {}
    inputs: Dict[str, Dict[str, InputInfo]] = {}
    for path, label, delivery, target, glaze in runs:
        run_cells, run_inputs = parse_run_rows(read_metrics_csv(path), label, delivery, target, glaze, path.name)
        for song, cell in run_cells.items():
            cells[(label, song, delivery, target, glaze)] = cell
        label_inputs = inputs.setdefault(label, {})
        for song, info in run_inputs.items():
            previous = label_inputs.setdefault(song, info)
            if previous.audio_hash != info.audio_hash:
                raise InfraError(f'{path.name}：{label} 的 {song} 输入 audio_hash 与同标签其它格不同，各格渲染用的输入不是同一份')
    fill_profiles(list(cells.values()))
    return cells, inputs


def label_input_mismatches(inputs: Dict[str, Dict[str, InputInfo]]) -> List[str]:
    """两个二进制渲的输入应当是同一份（audio_hash 相同）；不同的歌列出来。"""
    seen: Dict[str, Tuple[str, str]] = {}
    problems = []
    for label in sorted(inputs, key=lambda x: (x != PRIMARY_LABEL, x)):
        for song, info in inputs[label].items():
            if song in seen and seen[song][1] != info.audio_hash:
                problems.append(f'{song}：{seen[song][0]} 与 {label} 的输入 audio_hash 不同')
            seen.setdefault(song, (label, info.audio_hash))
    return problems


# ============================================================================================================
# 输入核对（prepare 与 report 共用判据）
# ============================================================================================================

@dataclass
class InputVerdict:
    comparable: bool
    reason: str
    d_lufs: Optional[float] = None
    d_tp: Optional[float] = None
    hash_match: Optional[bool] = None
    source: str = ''


def decide_input(identical: Optional[bool], d_lufs: Optional[float], d_tp: Optional[float]) -> Tuple[bool, str]:
    """逐位相同 → 并排旧数；否则 LUFS 与真峰值的偏差都在容差内才并排，其余标「输入不同」。"""
    if identical:
        return True, '逐位相同'
    if d_lufs is None or d_tp is None:
        return False, '输入不同，不并排旧数：没有可比的响度 / 真峰值读数'
    if abs(d_lufs) <= INPUT_TOL_LUFS and abs(d_tp) <= INPUT_TOL_TP_DB:
        return True, f'不逐位相同，但 LUFS 差 {d_lufs:+.3f}、真峰值差 {d_tp:+.3f} dB，都在容差内'
    return False, (f'输入不同，不并排旧数：LUFS 差 {d_lufs:+.3f}（容差 {INPUT_TOL_LUFS}）、'
                   f'真峰值差 {d_tp:+.3f} dB（容差 {INPUT_TOL_TP_DB}）')


def judge_inputs(old_inputs: Dict[str, InputInfo], new_inputs: Dict[str, InputInfo],
                 prepared_rows: List[Dict[str, str]]) -> Dict[str, InputVerdict]:
    """report 里的最终判定：优先用渲染器自己的 input 行（与 T-085 同一把量具）；没有就退回 prepare 的初筛；都没有就不并排。"""
    prepared = {song_of(row['file']): row for row in prepared_rows}
    verdicts: Dict[str, InputVerdict] = {}
    for song, old in old_inputs.items():
        new = new_inputs.get(song)
        if new is not None and None not in (new.lufs, new.tp, old.lufs, old.tp):
            d_lufs, d_tp = new.lufs - old.lufs, new.tp - old.tp
            hash_match = bool(new.audio_hash) and new.audio_hash == old.audio_hash
            comparable, reason = decide_input(hash_match, d_lufs, d_tp)
            verdicts[song] = InputVerdict(comparable, reason, d_lufs, d_tp, hash_match, '渲染器输入行')
        elif song in prepared:
            row = prepared[song]
            verdicts[song] = InputVerdict(row.get('comparable') == 'yes', row.get('reason', ''), to_float(row.get('d_lufs')),
                                          to_float(row.get('d_tp')), row.get('sha_match') == 'yes', 'prepare 初筛')
        else:
            verdicts[song] = InputVerdict(False, '没有输入核对数据', source='无')
    return verdicts


# ============================================================================================================
# 逐格判定与并排行
# ============================================================================================================

def judge_cell(gap: Optional[float], glaze: float, gr: Optional[float], gr0: Optional[float]) -> str:
    """热格（I − L_t 超过 HOT_GAP_LU）才判：GR 不到阈值 → 低于阈值；到了再看比同档 GLAZE 0 高不高。"""
    if gap is None or gr is None:
        return V_NA
    if gap <= HOT_GAP_LU:
        return V_COLD
    if glaze <= 0:
        return V_FLOOR
    if gr < PROBLEM_GR_MEAN_DB:
        return V_UNDER
    if gr0 is None:
        return V_NA
    return V_OVER if gr - gr0 > ABOVE_FLOOR_MARGIN_DB else V_INHERENT


def selfcheck_deviation(cell: Cell, lt: Optional[float], manual: Optional[float], has_t089: bool) -> Optional[float]:
    """plan_requested_target_lufs 与预期值的偏差（dB）。预期 = I + 手动链净增益 + GLAZE/100 × (L_t − I)，
    见 GlazePlanner.cpp:1875-1876。只对「请求恰好等于缺口」的格子有意义：含 T-089 的二进制要缺口 > SELFCHECK_MIN_PUSH_DB，
    T-089 之前要缺口 ≥ 最小推响量（否则请求被 3 dB 底量或档上限改写）；GLAZE 0 没有 GLAZE 部分，返回 None。"""
    if lt is None or cell.profile_lufs is None or cell.plan_req_target is None or cell.glaze <= 0:
        return None
    push = lt - cell.profile_lufs
    applies = push > SELFCHECK_MIN_PUSH_DB if has_t089 else push >= MIN_FULL_DRIVE_DB
    if not applies:
        return None
    expected = cell.profile_lufs + (manual if manual is not None else 0.0) + cell.glaze / 100.0 * push
    return cell.plan_req_target - expected


def build_row(cell: Cell, cells: Dict[tuple, Cell], t085_cells: Dict[Tuple[str, float], Cell], t085_songs: set,
              verdicts: Dict[str, InputVerdict]) -> dict:
    """一格一首歌 → side_by_side 的一行（数值保持浮点，写 CSV 时才格式化）。"""
    has_t089 = LABEL_HAS_T089[cell.label]
    reasons: List[str] = []
    row = {key: None for key in SIDE_FIELDS}
    row.update(label=cell.label, song=cell.song, kind='synthetic' if is_synthetic(cell.song) else 'real',
               delivery=cell.delivery, target_lufs=cell.target, glaze=cell.glaze,
               profile_lufs=cell.profile_lufs, profile_source=cell.profile_source, lra_lu=cell.lra,
               weight_warm=cell.w_warm, weight_punchy=cell.w_punchy, weight_clean=cell.w_clean,
               out_lufs=cell.out_lufs, limit_gr_mean_db=cell.gr_mean, limit_gr_max_db=cell.gr_max,
               plan_gain_db=cell.plan_gain, plan_requested_gain_db=cell.plan_req_gain,
               plan_requested_target_lufs=cell.plan_req_target)

    lt = None
    if profile_complete(cell):
        lt = derive_preferred_lufs(cell.delivery, cell.target, (cell.w_warm, cell.w_punchy, cell.w_clean), cell.lra)
    else:
        reasons.append('没有画像：profile_lufs / 风格权重为空，同曲其它格也借不到')
    gap = cell.profile_lufs - lt if lt is not None else None
    row['lt_lufs'] = lt
    row['i_minus_lt'] = gap
    row['hot'] = None if gap is None else gap > HOT_GAP_LU
    if lt is not None and cell.out_lufs is not None:
        row['out_minus_lt'] = cell.out_lufs - lt
    if cell.gr_mean is None:
        reasons.append('limit_gr_mean_db 为空')

    floor = cells.get((cell.label, cell.song, cell.delivery, cell.target, 0.0))
    gr0 = floor.gr_mean if floor is not None else None
    manual = floor.plan_req_gain if floor is not None else None
    row['gr_glaze0_mean_db'] = gr0
    row['manual_net_db'] = manual
    if cell.gr_mean is not None and gr0 is not None:
        row['gr_minus_glaze0_db'] = cell.gr_mean - gr0
    if cell.glaze > 0 and manual is not None and cell.plan_req_gain is not None:
        row['implied_push_db'] = (cell.plan_req_gain - manual) / (cell.glaze / 100.0)
    row['lt_selfcheck_dev_db'] = selfcheck_deviation(cell, lt, manual, has_t089)

    row['verdict'] = judge_cell(gap, cell.glaze, cell.gr_mean, gr0)
    if row['verdict'] == V_NA and gap is not None and cell.gr_mean is not None:
        reasons.append('没有同档 GLAZE 0 的行，算不出下限')

    row['t085_status'] = T085_NO_CELL
    if cell.delivery == 'streaming' and cell.glaze == T085_GLAZE:
        old = t085_cells.get((cell.song, cell.target))
        if cell.song not in t085_songs:
            row['t085_status'] = T085_NO_SONG
        elif old is None:
            row['t085_status'] = T085_NO_CELL
        elif cell.song not in verdicts:
            row['t085_status'] = T085_UNCHECKED
        elif not verdicts[cell.song].comparable:
            row['t085_status'] = T085_DIFF
        else:
            row['t085_status'] = T085_SIDE
            row['t085_profile_lufs'] = old.profile_lufs
            row['t085_out_lufs'] = old.out_lufs
            row['t085_gr_mean_db'] = old.gr_mean
            if cell.gr_mean is not None and old.gr_mean is not None:
                row['gr_minus_t085_db'] = cell.gr_mean - old.gr_mean
    row['na_reason'] = '；'.join(reasons)
    return row


def row_sort_key(key: tuple):
    label, song, delivery, target, glaze = key
    return (0 if label == PRIMARY_LABEL else 1, label, song, DELIVERY_ORDER[delivery], -target, glaze)


def build_rows(cells: Dict[tuple, Cell], t085_cells: Dict[Tuple[str, float], Cell],
               verdicts: Dict[str, InputVerdict]) -> List[dict]:
    t085_songs = {song for song, _ in t085_cells}
    return [build_row(cells[key], cells, t085_cells, t085_songs, verdicts) for key in sorted(cells, key=row_sort_key)]


def format_side_row(row: dict) -> Dict[str, str]:
    out = {}
    for key in SIDE_FIELDS:
        value = row.get(key)
        if value is None:
            out[key] = ''
        elif key in GENERAL_NUMBER_FIELDS:
            out[key] = f'{value:g}'
        elif key in FOUR_DIGIT_FIELDS:
            out[key] = fmt(value, 4)
        elif key == 'hot':
            out[key] = '1' if value else '0'
        elif isinstance(value, float):
            out[key] = fmt(value)
        else:
            out[key] = str(value)
    return out


@dataclass
class ProblemResult:
    hits: List[dict]        # 热格 + GR ≥ 阈值 + 高于 GLAZE 0 下限：问题在
    inherent: List[dict]    # 热格 + GR ≥ 阈值，但没高过 GLAZE 0 下限：输入自带
    loose: List[dict]       # 不限热格，GR ≥ 阈值且高于下限（参考，推响本身也会产生 GR）
    unjudged: List[dict]    # 热格但算不出（没有 GLAZE 0 行等）


def evaluate_problem(rows: List[dict], label: str) -> ProblemResult:
    """「问题还在」= 真歌 × 流媒体 PROBLEM_TARGETS × GLAZE > 0 里至少有一格的判定是「高于下限（问题在）」。"""
    result = ProblemResult([], [], [], [])
    for r in rows:
        if r['label'] != label or r['kind'] != 'real' or r['delivery'] != 'streaming':
            continue
        if r['target_lufs'] not in PROBLEM_TARGETS or r['glaze'] <= 0:
            continue
        gr, gr0 = r['limit_gr_mean_db'], r['gr_glaze0_mean_db']
        if gr is not None and gr0 is not None and gr >= PROBLEM_GR_MEAN_DB and gr - gr0 > ABOVE_FLOOR_MARGIN_DB:
            result.loose.append(r)
        if r['verdict'] == V_OVER:
            result.hits.append(r)
        elif r['verdict'] == V_INHERENT:
            result.inherent.append(r)
        elif r['verdict'] == V_NA and r['hot']:
            result.unjudged.append(r)
    return result


# ============================================================================================================
# REPORT.md 生成（表格与逐格判定；第 1 节结论保留人填的内容）
# ============================================================================================================

def md_cell(value, digits: int = 2) -> str:
    return '—' if value is None else fmt(value, digits)


def md_table(headers: List[str], body: List[List[str]]) -> List[str]:
    lines = ['| ' + ' | '.join(headers) + ' |', '|' + '|'.join(['---'] * len(headers)) + '|']
    lines += ['| ' + ' | '.join(str(c) for c in line) + ' |' for line in body]
    return lines


def extract_conclusion(old_text: Optional[str]) -> Optional[str]:
    """上一版 REPORT.md 里人填的结论；没有标记或还是占位就返回 None（重跑 report 不许把它冲掉）。"""
    if old_text is None:
        return None
    begin = old_text.find(CONCLUSION_BEGIN)
    end = old_text.find(CONCLUSION_END)
    if begin < 0 or end < 0 or end < begin:
        return None
    body = old_text[begin + len(CONCLUSION_BEGIN):end].strip('\n')
    if not body.strip() or body.strip() == CONCLUSION_PLACEHOLDER:
        return None
    return body


def hot_table_line(r: dict) -> List[str]:
    old = md_cell(r['t085_gr_mean_db'], 3) if r['t085_status'] == T085_SIDE else r['t085_status']
    return [r['song'], f"{r['delivery']} {r['target_lufs']:g}", f"{r['glaze']:g}", md_cell(r['profile_lufs']), md_cell(r['lt_lufs']),
            md_cell(r['i_minus_lt']), md_cell(r['out_lufs']), md_cell(r['out_minus_lt']), md_cell(r['limit_gr_mean_db'], 3),
            md_cell(r['gr_glaze0_mean_db'], 3), md_cell(r['gr_minus_glaze0_db'], 3), r['verdict'], old]


HOT_TABLE_HEADERS = ['曲', '档', 'GLAZE', 'I', 'L_t', 'I−L_t', '出口 LUFS', '出口−L_t', 'GR 均值', 'GLAZE 0 的 GR', 'GR−GLAZE 0', '判定', 'T-085 的 GR']


def section_thresholds() -> List[str]:
    return ['## 2. 机器判定', '',
            '判据常量集中在 `tools/render/r602_rerender.py` 顶部，改阈值只改那一处：', '',
            *md_table(['常量', '值', '含义'], [
                ['HOT_GAP_LU', f'{HOT_GAP_LU:g} LU', 'I − L_t 超过它的格子叫「热格」（入口已经比偏好响度更响）'],
                ['PROBLEM_GR_MEAN_DB', f'{PROBLEM_GR_MEAN_DB:g} dB', 'limit_gr_mean_db 达到它算「压得多」（ADR-033 的起点值）'],
                ['ABOVE_FLOOR_MARGIN_DB', f'{ABOVE_FLOOR_MARGIN_DB:g} dB', 'GR 均值比同档 GLAZE 0 的 GR 均值多出这么多才算「高于下限」（严格大于）'],
                ['PROBLEM_TARGETS', ' / '.join(f'{t:g}' for t in PROBLEM_TARGETS), '「问题还在」只看真歌 × 流媒体这两档 × GLAZE > 0']]),
            '',
            '「问题还在」= 上述范围里至少一格同时满足：热格、GR 均值 ≥ 阈值、GR 均值高于同档 GLAZE 0 的 GR 均值。'
            '落在 GLAZE 0 下限的格子是输入自带的峰值超限，不是 GLAZE 推出来的。', '']


def section_machine_verdict(rows: List[dict], labels: List[str]) -> List[str]:
    lines = section_thresholds()
    for label in labels:
        result = evaluate_problem(rows, label)
        lines += [f'### {label}', '',
                  f'- 机器判定：**{"问题还在" if result.hits else "未命中（没有格子同时满足三条）"}**'
                  f'（命中 {len(result.hits)} 格）',
                  f'- 热格里 GR 均值 ≥ {PROBLEM_GR_MEAN_DB:g} dB、但没高过 GLAZE 0 下限（输入自带）：{len(result.inherent)} 格',
                  f'- 热格里算不出（没有同档 GLAZE 0 行等）：{len(result.unjudged)} 格',
                  f'- 参考：不限热格、只看 GR ≥ 阈值且高于下限：{len(result.loose)} 格'
                  '（推响本身也会产生 GR，所以这个数不能当「推过头」）', '']
        if result.hits:
            lines += [*md_table(HOT_TABLE_HEADERS, [hot_table_line(r) for r in result.hits]), '']
    return lines


def section_notes() -> List[str]:
    return ['## 3. 口径与限制', '',
            '- **GR 均值偏高**：`limit_gr_mean_db` 是第二遍每个宿主块读一次「块内最大压缩量」再按块长加权平均，是「块峰值」的平均，'
            '比逐样本平均偏高，静音段计 0（tools/render/README.md 第 143 行）。ADR-033 与 T-085 用的是同一列，口径一致；'
            f'{PROBLEM_GR_MEAN_DB:g} dB 阈值放在这个偏高的口径上，读数更容易越线。',
            '- **GLAZE 0 下限**：流媒体档的有效 Ceiling 封在 −2 dBTP，只看交付档、不看 GLAZE（Source/dsp/CeladonChain.cpp:268-275），'
            '所以同档 GLAZE 0 的 GR 均值 = 不推时输入自带的压缩量；「GR − GLAZE 0」才是 GLAZE 额外压出来的部分。',
            '- **L_t 是离线推的**：L_t 不在 metrics.csv 里，脚本照 Source/dsp/GlazePlanner.cpp:1648-1663 用 profile_lufs、weight_*、profile_lra_lu '
            '与档位常数推（常数集中在脚本顶部，抄自 GlazeConstants.h:174-181, 204-205, 336 与 GlazeConstants.cpp:71, 94, 117）。'
            '`--streaming-target-lufs` 把流媒体的目标、下限、上限一起平移。',
            '- **出口 LUFS**：`--no-align` 下未对齐的整段 LUFS。',
            '- **三个变量混在旧数里**：T-085 的底版（5a21425）早于 f51dd5a（先读档再第一次 prepare 不再清 LIVE）与 T-089（1a0e26a），'
            '又是在 x86 机上渲的；本次在 DGX（aarch64）上渲。所以另渲 b594874（T-089 之前、含 f51dd5a）：'
            '「pre-t089 对 T-085」= f51dd5a 与机器 / ffmpeg 差异，「main 对 pre-t089」= T-089。',
            '- **逐格不是逐曲**：判定按格（曲 × 档 × GLAZE）给；跨曲平均会把「推过头」冲淡，所以不用。', '']


def section_environment(env: dict) -> List[str]:
    lines = ['## 4. 环境', '']
    body = []
    for label, item in sorted((env.get('labels') or {}).items(), key=lambda kv: (kv[0] != PRIMARY_LABEL, kv[0])):
        host = item.get('host') or {}
        body.append([label, item.get('commit', ''), f"`{(item.get('binary_sha256') or '')[:16]}…`", item.get('status', ''),
                     len(item.get('runs') or []), host.get('machine', ''), item.get('finished_utc', '')])
    if body:
        lines += md_table(['标签', '提交号', '二进制 sha256（前 16 位）', '状态', '格数', '机器', '完成时间（UTC）'], body)
    else:
        lines.append('（没有 environment.json 的 labels 小节）')
    prepare = env.get('prepare') or {}
    if prepare:
        packages = ', '.join(f'{k} {v}' for k, v in (prepare.get('packages') or {}).items())
        lines += ['', f"- 输入重建：{prepare.get('ffmpeg', '')}；{packages}；{(prepare.get('host') or {}).get('machine', '')}"]
    lines += ['- T-085 旧数的环境见 docs/validation/2026-10-08-T085-codec-tp/environment.json（x86_64，ffmpeg 7.1.5）。', '']
    return lines


def section_inputs(verdicts: Dict[str, InputVerdict], prepared_rows: List[Dict[str, str]], mismatches: List[str]) -> List[str]:
    prepared = {song_of(row['file']): row for row in prepared_rows}
    lines = ['## 5. 输入核对', '',
             f'重建的输入与 T-085 的 input 行比：逐位相同（sha256 / 渲染器 audio_hash）算同一份；否则 LUFS 与真峰值偏差都在 '
             f'±{INPUT_TOL_LUFS:g} 内才并排旧数，其余标「{T085_DIFF}」、只留新数。最终判定优先用渲染器自己的 input 行（与 T-085 同一把量具），'
             'prepare 阶段的 input_check.csv 是初筛。', '']
    body = []
    for song in sorted(verdicts):
        v = verdicts[song]
        prep = prepared.get(song)
        body.append([song, (prep or {}).get('sha_match', '—') or '—', '是' if v.hash_match else ('否' if v.hash_match is not None else '—'),
                     md_cell(v.d_lufs, 3), md_cell(v.d_tp, 3), '是' if v.comparable else '否', f'{v.source}：{v.reason}'])
    if body:
        lines += md_table(['曲', 'prepare：sha256 一致', 'audio_hash 一致', 'ΔLUFS', 'ΔTP', '并排旧数', '依据'], body)
        different = [s for s, v in verdicts.items() if not v.comparable]
        if len(different) == len(verdicts):
            lines += ['', f'**注意：{len(verdicts)} 首全部与 T-085 输入不同，旧数整体不能并排，先问 Max。**']
    else:
        lines.append('（没有可核对的输入）')
    if mismatches:
        lines += ['', '**两个二进制渲的输入不是同一份：**', *[f'- {m}' for m in mismatches]]
    return lines + ['']


def section_hot_cells(rows: List[dict], labels: List[str]) -> List[str]:
    lines = ['## 6. 热格逐格表', '',
             f'热格 = I − L_t > {HOT_GAP_LU:g} LU 的格子；只列真歌、GLAZE > 0（GLAZE 0 是下限，在「GLAZE 0 的 GR」一列里）。'
             '合成曲的热格与全部格子见 side_by_side.csv。', '']
    for label in labels:
        subset = [r for r in rows if r['label'] == label and r['kind'] == 'real' and r['glaze'] > 0]
        lines += [f'### {label}', '']
        groups: Dict[tuple, dict] = {}
        for r in subset:
            key = (DELIVERY_ORDER[r['delivery']], -r['target_lufs'], r['glaze'])
            g = groups.setdefault(key, {'name': f"{r['delivery']} {r['target_lufs']:g}", 'glaze': r['glaze'],
                                        'hot': 0, V_OVER: 0, V_INHERENT: 0, V_UNDER: 0, V_NA: 0})
            if r['hot']:
                g['hot'] += 1
                if r['verdict'] in g:
                    g[r['verdict']] += 1
        lines += md_table(['档', 'GLAZE', '热格数（真歌）', V_OVER, V_INHERENT, V_UNDER, V_NA],
                          [[g['name'], f"{g['glaze']:g}", g['hot'], g[V_OVER], g[V_INHERENT], g[V_UNDER], g[V_NA]]
                           for _, g in sorted(groups.items())])
        hot_rows = [r for r in subset if r['hot']]
        lines += ['']
        if hot_rows:
            lines += [*md_table(HOT_TABLE_HEADERS, [hot_table_line(r) for r in hot_rows]), '']
        else:
            lines += ['（这个标签没有热格。）', '']
    return lines


def section_three_way(rows: List[dict]) -> List[str]:
    """流媒体 GLAZE 100：T-085 旧值 / pre-t089 / main 的 GR 均值并排，拆开 T-089 与其它变量。"""
    index = {(r['label'], r['song'], r['delivery'], r['target_lufs'], r['glaze']): r for r in rows}
    songs = sorted({r['song'] for r in rows})
    lines = ['## 7. 三方对照：T-085 旧值 / T-089 之前 / main', '',
             '流媒体、GLAZE 100 的 GR 均值（dB）。「T-085」列在输入不同时只写状态。', '']
    body = []
    for target in STREAMING_TARGETS:
        for song in songs:
            main = index.get((PRIMARY_LABEL, song, 'streaming', target, T085_GLAZE))
            pre = index.get(('pre-t089', song, 'streaming', target, T085_GLAZE))
            if main is None and pre is None:
                continue
            base = main or pre
            old = md_cell(base['t085_gr_mean_db'], 3) if base['t085_status'] == T085_SIDE else base['t085_status']
            delta = None
            if main is not None and pre is not None and main['limit_gr_mean_db'] is not None and pre['limit_gr_mean_db'] is not None:
                delta = main['limit_gr_mean_db'] - pre['limit_gr_mean_db']
            body.append([song, f'{target:g}', md_cell(base['i_minus_lt']), old,
                         md_cell(pre['limit_gr_mean_db'], 3) if pre else '—',
                         md_cell(main['limit_gr_mean_db'], 3) if main else '—', md_cell(delta, 3)])
    if body:
        lines += md_table(['曲', '目标', 'I−L_t', 'T-085 GR', 'pre-t089 GR', 'main GR', 'main−pre-t089'], body)
    else:
        lines.append('（没有流媒体 GLAZE 100 的格子。）')
    return lines + ['']


def section_selfcheck(rows: List[dict], spread: List[str]) -> List[str]:
    checked = [r for r in rows if r['lt_selfcheck_dev_db'] is not None]
    large = [r for r in checked if abs(r['lt_selfcheck_dev_db']) >= SELFCHECK_TOL_DB]
    lines = ['## 8. 自检', '',
             'L_t 推导自检：`plan_requested_target_lufs` 应等于 I + 手动链净增益 + GLAZE/100 × (L_t − I)（手动链净增益取同档 GLAZE 0 行的 '
             'plan_requested_gain_db，没有就按 0）。只对「请求恰好等于缺口」的格子查（含 T-089 的二进制：缺口 > '
             f'{SELFCHECK_MIN_PUSH_DB:g} dB；T-089 之前：缺口 ≥ {MIN_FULL_DRIVE_DB:g} dB）。',
             f'- 查了 {len(checked)} 格，偏差 ≥ {SELFCHECK_TOL_DB:g} dB 的 {len(large)} 格。']
    if large:
        lines += ['', *md_table(['标签', '曲', '档', 'GLAZE', 'I', 'L_t', '偏差'],
                                [[r['label'], r['song'], f"{r['delivery']} {r['target_lufs']:g}", f"{r['glaze']:g}",
                                  md_cell(r['profile_lufs']), md_cell(r['lt_lufs']), md_cell(r['lt_selfcheck_dev_db'], 3)]
                                 for r in large])]
    lines += ['', f'- 画像一致性：同一首歌 profile_lufs 在各格相差超过 {PROFILE_SPREAD_TOL_DB:g} dB 的：'
                  + ('无' if not spread else f'{len(spread)} 首')]
    lines += [f'  - {s}' for s in spread]
    return lines + ['']


def selfcheck_lines(rows: List[dict]) -> List[str]:
    """L_t 自检逐行打印：每个可查的格子一行，偏差达到容差的标「偏大」（不可查的格子——受约束、GLAZE 0——没有这一行）。"""
    lines = []
    for r in rows:
        dev = r['lt_selfcheck_dev_db']
        if dev is None:
            continue
        flag = '偏大' if abs(dev) >= SELFCHECK_TOL_DB else 'ok'
        lines.append(f"[L_t 自检] {r['label']} {r['song']} {r['delivery']} {r['target_lufs']:g} g{r['glaze']:g}  "
                     f"L_t={fmt(r['lt_lufs'])} plan_requested_target_lufs={fmt(r['plan_requested_target_lufs'])} 偏差={fmt(dev)} {flag}")
    return lines


def build_report_md(rows: List[dict], verdicts: Dict[str, InputVerdict], prepared_rows: List[Dict[str, str]], env: dict,
                    mismatches: List[str], spread: List[str], conclusion: Optional[str]) -> str:
    labels = sorted({r['label'] for r in rows}, key=lambda x: (x != PRIMARY_LABEL, x))
    lines = ['# R6-02 前置重渲：热混音被多压多少', '',
             '本文件的表格与逐格判定由 `tools/render/r602_rerender.py report` 生成；重跑会整份重写，只有第 1 节「结论」里人填的内容会保留。'
             '全部格子的数在 side_by_side.csv，各次渲染的原始 metrics 在 metrics_*.csv。', '',
             '## 1. 结论', '', CONCLUSION_BEGIN, conclusion if conclusion else CONCLUSION_PLACEHOLDER, CONCLUSION_END, '']
    lines += section_machine_verdict(rows, labels)
    lines += section_notes()
    lines += section_environment(env)
    lines += section_inputs(verdicts, prepared_rows, mismatches)
    lines += section_hot_cells(rows, labels)
    lines += section_three_way(rows)
    lines += section_selfcheck(rows, spread)
    return '\n'.join(lines).rstrip('\n') + '\n'


# ============================================================================================================
# 子命令 prepare
# ============================================================================================================

@dataclass
class InputPlan:
    file: str
    kind: str                     # 'jamendo' 或 'synthetic'
    category: str = ''
    track_id: str = ''
    url: str = ''
    mp3_bytes: str = ''
    mp3_sha256: str = ''


def plan_inputs(corpus_rows: List[Dict[str, str]], manifest_rows: List[Dict[str, str]]) -> List[InputPlan]:
    """corpus.csv 的 9 首 Jamendo + input_manifest.csv 里其余的合成曲；对不上就报错。文件名配方同 codec_tp_check.download_jamendo。"""
    plans: List[InputPlan] = []
    manifest_files = {row['file'] for row in manifest_rows}
    for row in corpus_rows:
        track = str(int(row['track_id'].split('_')[1]))
        file = f"{row['category']}_{track}.wav"
        if file not in manifest_files:
            raise InfraError(f'corpus.csv 的 {file} 不在 input_manifest.csv 里')
        plans.append(InputPlan(file, 'jamendo', row['category'], row['track_id'], row['url'], row['bytes'], row['download_sha256']))
    jamendo_files = {p.file for p in plans}
    synthetic = sorted(f for f in manifest_files if f not in jamendo_files)
    if len(synthetic) != EXPECTED_SYNTHETIC_COUNT or not all(is_synthetic(song_of(f)) for f in synthetic):
        raise InfraError(f'input_manifest.csv 里除 Jamendo 外应有 {EXPECTED_SYNTHETIC_COUNT} 首 {SYNTHETIC_PREFIX}* 合成曲，实际是 {synthetic}')
    plans += [InputPlan(f, 'synthetic') for f in synthetic]
    return plans


def describe_plan(index: int, total: int, plan: InputPlan, input_dir: Path) -> str:
    """dry-run 的一行说明（一条输入一行）。"""
    prefix = f'[{index:02d}/{total}] {plan.file}'
    if plan.kind == 'jamendo':
        return (f'{prefix}  curl 下载 {plan.url}（{plan.mp3_bytes} 字节，期望 sha256 {plan.mp3_sha256[:12]}…）'
                f' → ffmpeg 解成 f32 / 48 kHz / 立体声（codec_tp_check.download_jamendo 的配方）→ {input_dir / plan.file}')
    return f'{prefix}  合成 codec_tp_check.synthetic()（种子 85，40 s，48 kHz；4 首一次生成）→ {input_dir / plan.file}'


def import_codec_tp_check():
    """懒加载：codec_tp_check 在模块顶部就 import numpy / scipy / pyloudnorm，只有 prepare 真跑才需要它们。"""
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)
    try:
        import codec_tp_check
    except ImportError as exc:
        raise InfraError(f'导不进 codec_tp_check（prepare 真跑需要 numpy / scipy / soundfile / pyloudnorm）：{exc}')
    return codec_tp_check


def package_versions(names=('numpy', 'scipy', 'soundfile', 'pyloudnorm')) -> Dict[str, str]:
    from importlib import metadata
    versions = {}
    for name in names:
        try:
            versions[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            versions[name] = ''
    return versions


def ffmpeg_version_line(ffmpeg: str) -> str:
    try:
        output = subprocess.run([str(ffmpeg), '-version'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise InfraError(f'ffmpeg 跑不起来（{ffmpeg}）：{exc}')
    return output.splitlines()[0] if output else ''


def download_and_decode(codec, jamendo: List[InputPlan], work: Path, input_dir: Path, ffmpeg: str) -> Dict[str, str]:
    """直接调 codec_tp_check.download_jamendo（T-085 的下载解码配方，不复制）；返回新下的每个 mp3 的 sha256（按 track_id）。"""
    counts = Counter(p.category for p in jamendo)
    if len(set(counts.values())) != 1:
        raise InfraError(f'corpus.csv 每类曲数不一致：{dict(counts)}，download_jamendo 按「每类前 N 首」取')
    meta_dir = work / 'jamendo_meta'
    meta_dir.mkdir(parents=True, exist_ok=True)
    namespace = argparse.Namespace(work=work, input=input_dir, out=meta_dir, ffmpeg=ffmpeg,
                                   jamendo_per_class=next(iter(counts.values())))
    try:
        codec.download_jamendo(namespace)
    except subprocess.CalledProcessError as exc:
        raise InfraError(f'下载或解码失败：{shlex.join(str(x) for x in exc.cmd)}\n{exc.stdout}')
    except OSError as exc:
        raise InfraError(f'下载或解码跑不起来（curl / ffmpeg）：{exc}')
    fresh = {row['track_id']: row['download_sha256'] for row in read_csv_rows(meta_dir / 'corpus.csv')}
    expected = {p.track_id for p in jamendo}
    if set(fresh) != expected:
        raise InfraError(f'download_jamendo 取的曲目与 corpus.csv 不是同一批：多 {sorted(set(fresh) - expected)}，少 {sorted(expected - set(fresh))}')
    return fresh


def check_one_input(codec, plan: InputPlan, path: Path, manifest_row: Dict[str, str], old: Optional[InputInfo],
                    fresh_mp3: Dict[str, str]) -> Dict[str, str]:
    """一个输入文件对 T-085 的核对 → input_check.csv 的一行。逐位相同就不重测响度；不相同才用 codec_tp_check.measure 量。"""
    if not path.exists():
        raise InfraError(f'预期的输入没生成：{path}')
    sha = sha256_file(path)
    frames = codec.sf.info(path).frames
    sha_match = sha == manifest_row['sha256']
    lufs_new = tp_new = None
    if not sha_match:
        try:
            measured = codec.measure(path)
        except ValueError as exc:
            raise InfraError(str(exc))
        lufs_new, tp_new = measured['lufs_i'], measured['tp8_dbtp']
    d_lufs = lufs_new - old.lufs if (lufs_new is not None and old is not None and old.lufs is not None) else None
    d_tp = tp_new - old.tp if (tp_new is not None and old is not None and old.tp is not None) else None
    if old is None:
        comparable, reason = False, '输入不同，不并排旧数：T-085 的 render_target 里没有这首歌的 input 行'
    else:
        comparable, reason = decide_input(sha_match, d_lufs, d_tp)
    if sha_match:
        reason += '（sha256 相同，未重测响度）'
    row = {key: '' for key in INPUT_CHECK_FIELDS}
    row.update(file=plan.file, kind=plan.kind, frames=str(frames), frames_t085=manifest_row['frames'],
               frames_match='yes' if str(frames) == manifest_row['frames'] else 'no',
               sha256=sha, sha256_t085=manifest_row['sha256'], sha_match='yes' if sha_match else 'no',
               lufs_pyln=fmt(lufs_new), tp8_pyln=fmt(tp_new),
               lufs_t085=fmt(old.lufs) if old else '', tp_t085=fmt(old.tp) if old else '',
               d_lufs=fmt(d_lufs), d_tp=fmt(d_tp), comparable='yes' if comparable else 'no', reason=reason)
    if plan.kind == 'jamendo':
        mp3_match = fresh_mp3[plan.track_id] == plan.mp3_sha256
        row.update(mp3_sha256=fresh_mp3[plan.track_id], mp3_sha256_corpus=plan.mp3_sha256, mp3_sha_match='yes' if mp3_match else 'no')
        if not mp3_match:
            row['reason'] += '；Jamendo 原件字节已变（download_sha256 与 corpus.csv 不符）'
    return row


def cmd_prepare(args) -> int:
    corpus_rows = read_csv_rows(args.corpus)
    manifest_rows = read_csv_rows(args.manifest)
    plans = plan_inputs(corpus_rows, manifest_rows)
    work = args.work.resolve()
    input_dir = work / 'input'
    if args.dry_run:
        for index, plan in enumerate(plans, 1):
            print(describe_plan(index, len(plans), plan, input_dir))
        print(f'# dry-run：共 {len(plans)} 条输入（Jamendo {sum(p.kind == "jamendo" for p in plans)} + 合成 '
              f'{sum(p.kind == "synthetic" for p in plans)}），没有下载、没有写盘', file=sys.stderr)
        return 0

    _, t085_inputs = load_t085(expand_old_paths(args.old))
    manifest_by_file = {row['file']: row for row in manifest_rows}
    if shutil.which('curl') is None:
        raise InfraError('找不到 curl')
    ffmpeg_line = ffmpeg_version_line(args.ffmpeg)
    codec = import_codec_tp_check()
    jamendo = [p for p in plans if p.kind == 'jamendo']
    work.mkdir(parents=True, exist_ok=True)
    print(f'下载并解码 {len(jamendo)} 首 Jamendo（{ffmpeg_line}）…', file=sys.stderr, flush=True)
    fresh_mp3 = download_and_decode(codec, jamendo, work, input_dir, args.ffmpeg)
    print('生成 4 首合成曲（种子 85）…', file=sys.stderr, flush=True)
    codec.synthetic(input_dir)

    rows = []
    for plan in plans:
        print(f'核对 {plan.file}', file=sys.stderr, flush=True)
        rows.append(check_one_input(codec, plan, input_dir / plan.file, manifest_by_file[plan.file],
                                    t085_inputs.get(song_of(plan.file)), fresh_mp3))
    out_dir = args.out.resolve()
    write_csv(out_dir / 'input_check.csv', rows, INPUT_CHECK_FIELDS)
    update_environment(out_dir, 'prepare', None, {
        'finished_utc': utc_now_iso(), 'host': host_info(), 'ffmpeg': ffmpeg_line, 'packages': package_versions(),
        'work': str(work), 'inputs': {row['file']: row['sha256'] for row in rows}})

    same = sum(r['comparable'] == 'yes' for r in rows)
    print(f'input_check.csv 已写：{len(rows)} 首，可并排旧数 {same} 首，输入不同 {len(rows) - same} 首', file=sys.stderr)
    if same == 0:
        print('警告：全部输入都与 T-085 不同，旧数整体不能并排，停下来问 Max。', file=sys.stderr)
    return 0


# ============================================================================================================
# 子命令 render
# ============================================================================================================

@dataclass
class RenderJob:
    delivery: str
    target: float
    glaze: float
    name: str
    run_dir: Path
    command: List[str]
    metrics_copy: Path


def validate_metrics(path: Path, job: RenderJob, song_names: set) -> int:
    """一格的 metrics.csv 必须每首歌一个 input 行、一个 render 行，且交付档 / GLAZE / 曲风与这一格一致；返回行数。"""
    rows = read_metrics_csv(path)
    cells, inputs = parse_run_rows(rows, 'validate', job.delivery, job.target, job.glaze, Path(path).name)
    if set(cells) != song_names or set(inputs) != song_names:
        raise InfraError(f'{Path(path).name}：歌曲不齐，render 行缺 {sorted(song_names - set(cells))}，input 行缺 {sorted(song_names - set(inputs))}，'
                         f'多出 {sorted((set(cells) | set(inputs)) - song_names)}')
    return len(rows)


def run_render_job(job: RenderJob, song_names: set, resume: bool) -> dict:
    """跑一格（或复用已有且完整的结果）；任何失败都收进返回值，由主线程汇总后以退出码 1 结束。"""
    result = {'cell': job.name, 'command': job.command, 'metrics_file': job.metrics_copy.name, 'returncode': None,
              'seconds': 0.0, 'status': 'failed', 'rows': 0, 'metrics_sha256': '', 'error': ''}
    started = time.monotonic()
    if resume and job.metrics_copy.exists():
        try:
            result.update(status='skipped', rows=validate_metrics(job.metrics_copy, job, song_names),
                          metrics_sha256=sha256_file(job.metrics_copy))
            return result
        except InfraError:
            pass   # 不完整就重跑
    job.run_dir.mkdir(parents=True, exist_ok=True)
    produced = job.run_dir / 'metrics.csv'
    produced.unlink(missing_ok=True)   # 绝不把上一次的残留当成这一次的结果
    try:
        with (job.run_dir / 'stderr.txt').open('w', encoding='utf-8') as stream:
            completed = subprocess.run(job.command, stdout=stream, stderr=subprocess.STDOUT)
        result['returncode'] = completed.returncode
        if completed.returncode != 0:
            result['error'] = f'退出码 {completed.returncode}，见 {job.run_dir / "stderr.txt"}'
        else:
            result['rows'] = validate_metrics(produced, job, song_names)
            shutil.copyfile(produced, job.metrics_copy)   # 原样入库，逐字节复制
            result.update(status='ok', metrics_sha256=sha256_file(job.metrics_copy))
    except (OSError, InfraError) as exc:
        result['error'] = str(exc)
    result['seconds'] = round(time.monotonic() - started, 1)
    return result


def cmd_render(args) -> int:
    work = args.work.resolve()
    input_dir = (args.input or (work / 'input')).resolve()
    out_dir = args.out.resolve()
    binary = args.render.resolve()
    jobs = []
    for delivery, target, glaze in matrix_cells():
        name = cell_name(delivery, target, glaze)
        run_dir = work / 'render' / args.label / name
        jobs.append(RenderJob(delivery, target, glaze, name, run_dir,
                              render_command(binary, input_dir, run_dir, delivery, target, glaze),
                              out_dir / metrics_file_name(args.label, delivery, target, glaze)))
    if args.dry_run:
        for job in jobs:
            print(shlex.join(job.command))
        print(f'# dry-run：{len(jobs)} 条命令；每格的 metrics.csv 逐字节复制为 {out_dir}/metrics_{args.label}_<档>_<目标>_g<GLAZE>.csv', file=sys.stderr)
        return 0

    if not args.commit:
        raise InfraError('真跑必须给 --commit（这个二进制对应的提交号），environment.json 要记')
    if not (binary.is_file() and os.access(binary, os.X_OK)):
        raise InfraError(f'找不到可执行的 celadon_render：{binary}')
    wavs = sorted(input_dir.glob('*.wav'))
    if not wavs:
        raise InfraError(f'{input_dir} 里没有 wav（先跑 prepare）')
    song_names = {p.stem for p in wavs}
    binary_sha = sha256_file(binary)
    previous = (read_json(out_dir / 'environment.json').get('labels') or {}).get(args.label)
    if args.resume and previous and previous.get('binary_sha256') != binary_sha:
        raise InfraError(f'--resume：{args.label} 上一次用的二进制 sha256 与现在的不同，不能混用；去掉 --resume 重跑')
    record = {'commit': args.commit, 'binary': str(binary), 'binary_sha256': binary_sha, 'host': host_info(),
              'status': 'running', 'started_utc': utc_now_iso(), 'inputs': {p.name: sha256_file(p) for p in wavs},
              'runs': []}
    update_environment(out_dir, 'labels', args.label, record)

    print(f'{args.label}（{args.commit}）：{len(jobs)} 格 × {len(wavs)} 首，并行 {args.jobs}', file=sys.stderr, flush=True)
    by_name: Dict[str, dict] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(run_render_job, job, song_names, args.resume) for job in jobs]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            by_name[result['cell']] = result
            print(f"[{len(by_name)}/{len(jobs)}] {result['cell']} {result['status']}（{result['seconds']:.0f} s）"
                  + (f"：{result['error']}" if result['error'] else ''), file=sys.stderr, flush=True)
    results = [by_name[job.name] for job in jobs]
    failed = [r for r in results if r['status'] == 'failed']
    record.update(runs=results, status='failed' if failed else 'complete', finished_utc=utc_now_iso())
    update_environment(out_dir, 'labels', args.label, record)
    if failed:
        raise InfraError(f'{len(failed)} 格失败：{", ".join(r["cell"] for r in failed)}（没有复制它们的 metrics.csv；日志在 {work / "render" / args.label}）')
    print(f'{args.label} 完成：{len(results)} 格，metrics 已复制到 {out_dir}', file=sys.stderr)
    return 0


# ============================================================================================================
# 子命令 report
# ============================================================================================================

def cmd_report(args) -> int:
    new_dir = args.new.resolve()
    out_dir = (args.out or args.new).resolve()
    t085_cells, t085_inputs = load_t085(expand_old_paths(args.old))
    runs = discover_runs(new_dir)
    if not runs:
        raise InfraError(f'{new_dir} 里没有 metrics_<标签>_<档>_<目标>_g<GLAZE>.csv（先跑 render）')
    cells, label_inputs = load_new_cells(runs)

    merged_inputs: Dict[str, InputInfo] = {}
    for label in sorted(label_inputs, key=lambda x: (x != PRIMARY_LABEL, x)):
        for song, info in label_inputs[label].items():
            merged_inputs.setdefault(song, info)
    check_path = new_dir / 'input_check.csv'
    prepared_rows = read_csv_rows(check_path) if check_path.exists() else []
    verdicts = judge_inputs(t085_inputs, merged_inputs, prepared_rows)

    rows = build_rows(cells, t085_cells, verdicts)
    write_csv(out_dir / 'side_by_side.csv', [format_side_row(r) for r in rows], SIDE_FIELDS)

    report_path = out_dir / 'REPORT.md'
    previous = report_path.read_text(encoding='utf-8') if report_path.exists() else None
    markdown = build_report_md(rows, verdicts, prepared_rows, read_json(new_dir / 'environment.json'),
                               label_input_mismatches(label_inputs), profile_spread_issues(list(cells.values())),
                               extract_conclusion(previous))
    report_path.write_text(markdown, encoding='utf-8')

    labels = sorted({r['label'] for r in rows}, key=lambda x: (x != PRIMARY_LABEL, x))
    for line in selfcheck_lines(rows):
        print(line)
    print(f'side_by_side.csv {len(rows)} 行、REPORT.md 已写入 {out_dir}', file=sys.stderr)
    for label in labels:
        result = evaluate_problem(rows, label)
        print(f'{label}：机器判定 {"问题还在" if result.hits else "未命中"}（命中 {len(result.hits)} 格，输入自带 {len(result.inherent)} 格，'
              f'算不出 {len(result.unjudged)} 格）', file=sys.stderr)
    return 0


# ============================================================================================================
# 子命令 selftest
# ============================================================================================================

def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_close(actual, expected: float, tol: float, message: str) -> None:
    if actual is None or abs(actual - expected) > tol:
        raise AssertionError(f'{message}：得到 {actual}，期望 {expected} ± {tol}')


def make_cell(label='main', song='demo', delivery='streaming', target=-10.0, glaze=100.0, **fields) -> Cell:
    return Cell(label=label, song=song, delivery=delivery, target=target, glaze=glaze, **fields)


def selftest_matrix_and_commands() -> None:
    cells = matrix_cells()
    expect(len(cells) == 12, f'矩阵应有 12 格，实际 {len(cells)}')
    expect(cells[0] == ('streaming', -10.0, 0.0) and cells[-1] == ('club', -7.0, 100.0), '矩阵首尾格不对')
    expect(sum(c[0] == 'streaming' for c in cells) == 9 and sum(c[0] == 'club' for c in cells) == 3, '流媒体应 9 格、俱乐部 3 格')
    commands = [render_command(Path('/b/celadon_render'), Path('/in'), Path('/out'), *c) for c in cells]
    expect(commands[0] == ['/b/celadon_render', '--in', '/in', '--out', '/out', '--glaze', '0', '--style', 'auto', '--clay', 'auto',
                           '--genre', 'general', '--delivery', 'streaming', '--streaming-target-lufs', '-10',
                           '--no-align', '--no-replay', '--metrics-only'], f'第一条命令不对：{commands[0]}')
    for cell, command in zip(cells, commands):
        expect(('--streaming-target-lufs' in command) == (cell[0] == 'streaming'), '只有流媒体格带 --streaming-target-lufs')
        expect(all(flag in command for flag in RENDER_TAIL_FLAGS), '每条命令都要带 --no-align --no-replay --metrics-only')


def selftest_file_names() -> None:
    name = metrics_file_name('pre-t089', 'club', -7.0, 35.0)
    expect(name == 'metrics_pre-t089_club_-7_g35.csv', f'文件名不对：{name}')
    expect(parse_metrics_name(name) == ('pre-t089', 'club', -7.0, 35.0), '文件名解析回不来')
    expect(parse_metrics_name('metrics_main_streaming_-12_g100.csv') == ('main', 'streaming', -12.0, 100.0), '流媒体文件名解析不对')
    expect(parse_metrics_name('metrics.csv') is None and parse_metrics_name('metrics_main_other_-1_g0.csv') is None, '不合规的名字应解析不出')


def selftest_derive_known_answers() -> None:
    # GlazePlannerTests.cpp:3154-3158：冲击、LRA 5，风格偏移 0，流媒体目标就是 −10、俱乐部就是 −7
    expect_close(derive_preferred_lufs('streaming', -10.0, (0, 1, 0), 5.0), -10.0, 1e-9, '冲击 / LRA 5 / 流媒体 −10')
    expect_close(derive_preferred_lufs('club', -7.0, (0, 1, 0), 5.0), -7.0, 1e-9, '冲击 / LRA 5 / 俱乐部 −7')
    expect_close(derive_preferred_lufs('club', -7.0, (1, 0, 0), 5.0), -6.5, 1e-9, '温润的俱乐部偏好 −6.5（越过标准档上限，档内合法）')
    expect_close(derive_preferred_lufs('streaming', -10.0, (0, 0, 1), 20.0), -12.0, 1e-9, '通透 + LRA 20 钳到流媒体下限 −12')
    expect_close(derive_preferred_lufs('streaming', -14.0, (0, 0, 1), 10.7), -16.0, 1e-9, '−14 档下限 −16（三项一起平移）')
    expect_close(derive_preferred_lufs('club', -7.0, (0, 0, 1), 20.0), -9.0, 1e-9, '通透 + LRA 20 钳到俱乐部下限 −9')
    expect_close(derive_preferred_lufs('streaming', -10.0, (0, 1, 0), None), -10.0, 1e-9, 'LRA 缺失按参考值')
    expect(sanitise_weights(0, 0, 0) == (0.0, 0.0, 1.0), '全 0 权重应退回通透')
    expect(sanitise_weights(-1, float('nan'), 2) == (0.0, 0.0, 1.0), '负数与 NaN 按 0')
    expect_close(sum(sanitise_weights(0.1079, 0.1079, 0.7843)), 1.0, 1e-12, '权重归一')
    try:
        delivery_range('club', -8.0)
    except InfraError:
        pass
    else:
        raise AssertionError('俱乐部没有目标覆盖，−8 应报错')


def selftest_t085_known_answers() -> None:
    cells, inputs = load_t085(expand_old_paths([T085_DIR]))
    expect(len({song for song, _ in cells}) == 13 and len(inputs) == 13, f'T-085 应有 13 首，实际 {len({s for s, _ in cells})} / {len(inputs)}')
    expect(len(cells) == 39, f'T-085 应有 13 首 × 3 档 = 39 格，实际 {len(cells)}')
    melodic14 = cells[('melodic_241941', -14.0)]
    expect_close(melodic14.gr_mean, 1.680, 1e-9, 'melodic_241941 −14 组 GR 均值')
    expect_close(melodic14.profile_lufs, -8.088, 1e-9, 'melodic_241941 的 I')
    expect_close(cells[('melodic_241941', -10.0)].gr_mean, 1.916, 1e-9, 'melodic_241941 −10 组 GR 均值')
    expect_close(cells[('melodic_241941', -12.0)].gr_mean, 1.724, 1e-9, 'melodic_241941 −12 组 GR 均值')
    for target, gap in ((-10.0, 3.912), (-12.0, 5.912), (-14.0, 7.912)):   # 手算：通透、LRA 10.7，L_t 钳到各档下限
        cell = cells[('melodic_241941', target)]
        lt = derive_preferred_lufs('streaming', target, (cell.w_warm, cell.w_punchy, cell.w_clean), cell.lra)
        expect_close(cell.profile_lufs - lt, gap, 0.005, f'melodic_241941 {target:g} 组 I − L_t')
    hot_mix = cells[('stress_hot_mix', -10.0)]
    lt = derive_preferred_lufs('streaming', -10.0, (hot_mix.w_warm, hot_mix.w_punchy, hot_mix.w_clean), hot_mix.lra)
    expect_close(hot_mix.profile_lufs - lt, 3.0, 0.01, 'stress_hot_mix −10 组 I − L_t')
    expect_close(hot_mix.gr_mean, 0.0, 1e-9, 'stress_hot_mix 的 GR 均值是 0')

    deep = cells[('deep_155328', -10.0)]
    lt = derive_preferred_lufs('streaming', -10.0, (deep.w_warm, deep.w_punchy, deep.w_clean), deep.lra)
    expect_close(lt, -10.897, 0.002, 'deep_155328 −10 组 L_t')
    expect(abs(deep.plan_req_target - lt) < SELFCHECK_TOL_DB, f'deep_155328 −10 组 L_t 与 plan_requested_target_lufs 偏差 {deep.plan_req_target - lt:+.3f} 不小于 {SELFCHECK_TOL_DB}')
    dev = selfcheck_deviation(deep, lt, None, False)
    expect(dev is not None and abs(dev) < SELFCHECK_TOL_DB, f'自检函数对 deep_155328 −10 的偏差 {dev}')
    tech = cells[('tech_46047', -10.0)]
    lt = derive_preferred_lufs('streaming', -10.0, (tech.w_warm, tech.w_punchy, tech.w_clean), tech.lra)
    expect_close(lt, -11.572, 0.002, 'tech_46047 −10 组 L_t')
    expect(abs(tech.plan_req_target - lt) < SELFCHECK_TOL_DB, 'tech_46047 −10 组偏差应小于 0.1')


def selftest_judge_cell() -> None:
    expect(judge_cell(None, 100.0, 1.0, 0.1) == V_NA and judge_cell(2.0, 100.0, None, 0.1) == V_NA, '缺读数应「算不出」')
    expect(judge_cell(HOT_GAP_LU, 100.0, 1.0, 0.1) == V_COLD and judge_cell(-3.0, 100.0, 1.0, 0.1) == V_COLD, '缺口不超过热格阈值 → 非热格')
    expect(judge_cell(2.0, 0.0, 1.0, 1.0) == V_FLOOR, 'GLAZE 0 的热格是下限')
    expect(judge_cell(2.0, 100.0, PROBLEM_GR_MEAN_DB - 0.001, 0.0) == V_UNDER, '低于阈值')
    expect(judge_cell(2.0, 100.0, PROBLEM_GR_MEAN_DB, 0.1) == V_OVER, 'GR 恰好等于阈值也算到了，高于下限 → 问题在')
    expect(judge_cell(2.0, 100.0, 0.5, 0.5) == V_INHERENT and judge_cell(2.0, 100.0, 0.5, 0.6) == V_INHERENT, '不高于下限 → 输入自带')
    expect(judge_cell(2.0, 100.0, 0.5, None) == V_NA, '没有 GLAZE 0 行且 GR 到阈值 → 算不出')
    expect(judge_cell(2.0, 100.0, 0.1, None) == V_UNDER, '没有 GLAZE 0 行但 GR 不到阈值 → 低于阈值（不需要下限）')


def selftest_selfcheck_applicability() -> None:
    # 含 T-089：GLAZE 35、手动链净增益 −0.07、缺口 4 dB → 预期 I − 0.07 + 0.35 × 4
    cell = make_cell(glaze=35.0, profile_lufs=-14.0, plan_req_target=-14.0 - 0.07 + 0.35 * 4.0)
    expect_close(selfcheck_deviation(cell, -10.0, -0.07, True), 0.0, 1e-9, 'GLAZE 35 的预期展开')
    expect(selfcheck_deviation(make_cell(profile_lufs=-8.0, plan_req_target=-8.0), -11.0, None, True) is None, '热曲（L_t < I）不查')
    expect(selfcheck_deviation(make_cell(profile_lufs=-11.0, plan_req_target=-10.0), -10.0, None, False) is None, 'T-089 之前缺口 < 3 dB 被底量改写，不查')
    expect(selfcheck_deviation(make_cell(glaze=0.0, profile_lufs=-14.0, plan_req_target=-14.0), -10.0, None, True) is None, 'GLAZE 0 不查')


def selftest_fill_profiles() -> None:
    full = make_cell(glaze=100.0, profile_lufs=-9.0, lra=5.0, w_warm=0.1, w_punchy=0.2, w_clean=0.7)
    empty = make_cell(glaze=0.0)
    other_song = make_cell(song='lonely', glaze=0.0)
    fill_profiles([full, empty, other_song])
    expect(empty.profile_lufs == -9.0 and empty.w_clean == 0.7 and empty.lra == 5.0, '应借到同曲别的格的画像')
    expect(empty.profile_source.startswith('借自'), f'来源标注不对：{empty.profile_source}')
    expect(other_song.profile_lufs is None and other_song.profile_source == '无', '没有可借的格要标「无」且保持空')
    expect(full.profile_source == '同格', '自己有画像的格不动')
    expect(profile_spread_issues([full]) == [], '单格没有分歧')
    a, b = make_cell(profile_lufs=-9.0, w_warm=0, w_punchy=0, w_clean=1), make_cell(glaze=35.0, profile_lufs=-9.2, w_warm=0, w_punchy=0, w_clean=1)
    expect(len(profile_spread_issues([a, b])) == 1, '画像相差 0.2 dB 应被列出')


def selftest_inputs() -> None:
    old = {'a': InputInfo(-10.0, -1.0, 'h1'), 'b': InputInfo(-10.0, -1.0, 'h2'), 'c': InputInfo(-10.0, -1.0, 'h3'),
           'd': InputInfo(-10.0, -1.0, 'h4'), 'e': InputInfo(-10.0, -1.0, 'h5')}
    new = {'a': InputInfo(-10.2, -1.2, 'h1'),    # audio_hash 相同：偏差不看
           'b': InputInfo(-10.04, -1.04, 'x'),    # 都在容差内
           'c': InputInfo(-10.06, -1.0, 'x')}     # LUFS 超容差
    prepared = [{'file': 'd.wav', 'comparable': 'yes', 'reason': 'ok', 'd_lufs': '0.010', 'd_tp': '0.000', 'sha_match': 'no'}]
    verdicts = judge_inputs(old, new, prepared)
    expect(verdicts['a'].comparable and verdicts['a'].hash_match, 'audio_hash 相同应并排')
    expect(verdicts['b'].comparable, '偏差在容差内应并排')
    expect(not verdicts['c'].comparable and T085_DIFF in verdicts['c'].reason, 'LUFS 偏差 0.06 应标输入不同')
    expect(verdicts['d'].comparable and verdicts['d'].source == 'prepare 初筛', '渲染器没读数时用 prepare 初筛')
    expect(not verdicts['e'].comparable and verdicts['e'].source == '无', '什么核对数据都没有就不并排')
    expect(decide_input(False, None, None)[0] is False and decide_input(True, None, None)[0] is True, 'decide_input 边界')


def selftest_plan_inputs() -> None:
    plans = plan_inputs(read_csv_rows(T085_DIR / 'corpus.csv'), read_csv_rows(T085_DIR / 'input_manifest.csv'))
    expect(len(plans) == 13, f'应有 13 条输入，实际 {len(plans)}')
    expect(sum(p.kind == 'jamendo' for p in plans) == 9 and sum(p.kind == 'synthetic' for p in plans) == 4, '应为 9 首 Jamendo + 4 首合成')
    expect(plans[0].file == 'deep_155328.wav' and plans[8].file == 'melodic_647576.wav', 'Jamendo 文件名配方不对')
    expect(sorted(p.file for p in plans if p.kind == 'synthetic') == ['stress_dense_hats.wav', 'stress_hot_mix.wav', 'stress_side_bass.wav', 'stress_sweep.wav'], '合成曲名不对')
    lines = [describe_plan(i, len(plans), p, Path('/w/input')) for i, p in enumerate(plans, 1)]
    expect(all('\n' not in line for line in lines) and len(lines) == 13, 'dry-run 应一条输入一行')


def selftest_pipeline_smoke() -> None:
    """用 T-085 的旧格冒充 main，再造 GLAZE 0 的下限和 pre-t089，整条 build_rows → CSV → REPORT.md 跑一遍。"""
    t085_cells, t085_inputs = load_t085(expand_old_paths([T085_DIR]))
    cells = {}
    for (song, target), old in t085_cells.items():
        for label, gr_scale in ((PRIMARY_LABEL, 1.0), ('pre-t089', 1.1)):
            top = replace(old, label=label, gr_mean=old.gr_mean * gr_scale)
            floor = replace(old, label=label, glaze=0.0, gr_mean=0.1, plan_req_gain=-0.07, profile_lufs=None, w_warm=None,
                            w_punchy=None, w_clean=None, lra=None, profile_source='同格')
            cells[(label, song, 'streaming', target, 100.0)] = top
            cells[(label, song, 'streaming', target, 0.0)] = floor
    fill_profiles(list(cells.values()))
    verdicts = judge_inputs(t085_inputs, t085_inputs, [])   # 新输入 = 旧输入：audio_hash 相同
    rows = build_rows(cells, t085_cells, verdicts)
    expect(len(rows) == 2 * 13 * 3 * 2, f'行数应为 2 标签 × 13 首 × 3 档 × 2 GLAZE，实际 {len(rows)}')
    target_row = next(r for r in rows if r['label'] == PRIMARY_LABEL and r['song'] == 'melodic_241941' and r['target_lufs'] == -14.0 and r['glaze'] == 100.0)
    expect(target_row['verdict'] == V_OVER and target_row['hot'] is True, f"melodic_241941 −14 应判「高于下限」，实际 {target_row['verdict']}")
    expect_close(target_row['gr_minus_glaze0_db'], 1.58, 1e-9, 'GR − GLAZE 0')
    expect(target_row['t085_status'] == T085_SIDE and abs(target_row['gr_minus_t085_db']) < 1e-9, '输入相同时应并排旧数')
    floor_row = next(r for r in rows if r['label'] == PRIMARY_LABEL and r['song'] == 'melodic_241941' and r['target_lufs'] == -14.0 and r['glaze'] == 0.0)
    expect(floor_row['profile_source'].startswith('借自') and floor_row['verdict'] == V_FLOOR, 'GLAZE 0 的行应借到画像、判为下限')
    expect(floor_row['t085_status'] == T085_NO_CELL, 'GLAZE 0 在 T-085 里没有格')
    formatted = format_side_row(target_row)
    expect(list(formatted) == list(SIDE_FIELDS) and formatted['glaze'] == '100' and formatted['target_lufs'] == '-14' and formatted['hot'] == '1', 'CSV 行格式不对')
    result = evaluate_problem(rows, PRIMARY_LABEL)
    expect(result.hits and all(r['kind'] == 'real' and r['target_lufs'] in PROBLEM_TARGETS for r in result.hits), '命中格只能是真歌 × −10 / −14')
    check_lines = selfcheck_lines(rows)
    expect(check_lines and all('偏差=' in line for line in check_lines), 'L_t 自检应逐行打印可查的格子')
    markdown = build_report_md(rows, verdicts, [], {}, [], [], None)
    for needle in ('## 1. 结论', CONCLUSION_PLACEHOLDER, '## 2. 机器判定', '## 6. 热格逐格表', '## 7. 三方对照', '## 8. 自检', 'melodic_241941'):
        expect(needle in markdown, f'REPORT.md 缺 {needle!r}')


def selftest_conclusion_preserved() -> None:
    filled = f'头\n{CONCLUSION_BEGIN}\n真结论\n第二行\n{CONCLUSION_END}\n尾\n'
    expect(extract_conclusion(filled) == '真结论\n第二行', '应取出人填的结论')
    expect(extract_conclusion(f'{CONCLUSION_BEGIN}\n{CONCLUSION_PLACEHOLDER}\n{CONCLUSION_END}') is None, '占位文字不算结论')
    expect(extract_conclusion('没有标记') is None and extract_conclusion(None) is None, '没有标记 / 没有旧文件 → None')
    rows: List[dict] = []
    markdown = build_report_md(rows, {}, [], {}, [], [], '真结论\n第二行')
    expect('真结论\n第二行' in markdown and CONCLUSION_PLACEHOLDER not in markdown, '重新生成时应保留人填的结论')


SELFTESTS = (selftest_matrix_and_commands, selftest_file_names, selftest_derive_known_answers, selftest_t085_known_answers,
             selftest_judge_cell, selftest_selfcheck_applicability, selftest_fill_profiles, selftest_inputs,
             selftest_plan_inputs, selftest_pipeline_smoke, selftest_conclusion_preserved)


def cmd_selftest(args) -> int:
    failed = 0
    for test in SELFTESTS:
        name = test.__name__.replace('selftest_', '')
        try:
            test()
        except Exception as exc:   # 自检里任何异常都算这一项失败，继续跑后面的
            failed += 1
            print(f'FAIL {name}：{type(exc).__name__}: {exc}')
        else:
            print(f'ok   {name}')
    print(f'selftest：{len(SELFTESTS) - failed} ok，{failed} 失败')
    return 1 if failed else 0


# ============================================================================================================
# 命令行
# ============================================================================================================

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    prepare = sub.add_parser('prepare', help='重建 13 个输入并核对是否与 T-085 同一份')
    prepare.add_argument('--work', type=Path, required=True, help='DGX 工作目录（input/ 与 downloads/ 在里面，不入库）')
    prepare.add_argument('--corpus', type=Path, default=T085_DIR / 'corpus.csv')
    prepare.add_argument('--manifest', type=Path, default=T085_DIR / 'input_manifest.csv')
    prepare.add_argument('--old', type=Path, nargs='+', default=[T085_DIR], help='T-085 的 render_target_*.csv 或其所在目录')
    prepare.add_argument('--out', type=Path, default=DEFAULT_REPORT_DIR, help='input_check.csv 与 environment.json 写到哪')
    prepare.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or '/usr/bin/ffmpeg')
    prepare.add_argument('--dry-run', action='store_true', help='只打印 13 条输入的做法，不下载、不写盘')
    prepare.set_defaults(func=cmd_prepare)

    render = sub.add_parser('render', help='用一个二进制跑整套矩阵')
    render.add_argument('--render', type=Path, required=True, help='celadon_render 二进制')
    render.add_argument('--label', choices=('main', 'pre-t089'), required=True, help='main = 3a7a367（含 T-089）；pre-t089 = b594874')
    render.add_argument('--commit', default=None, help='这个二进制对应的提交号（写进 environment.json）；真跑必填，--dry-run 可省')
    render.add_argument('--work', type=Path, required=True)
    render.add_argument('--input', type=Path, default=None, help='输入 wav 目录，默认 <work>/input')
    render.add_argument('--out', type=Path, default=DEFAULT_REPORT_DIR, help='metrics_*.csv 与 environment.json 写到哪（入库）')
    render.add_argument('--jobs', type=int, default=6, help='同时跑几格')
    render.add_argument('--resume', action='store_true', help='跳过已有且完整的格（二进制 sha256 必须与上次相同）')
    render.add_argument('--dry-run', action='store_true', help='只打印 12 条渲染命令')
    render.set_defaults(func=cmd_render)

    report = sub.add_parser('report', help='新旧并排，出 side_by_side.csv 与 REPORT.md')
    report.add_argument('--old', type=Path, nargs='+', default=[T085_DIR], help='T-085 的 render_target_-10/-12/-14.csv 或其所在目录')
    report.add_argument('--new', type=Path, default=DEFAULT_REPORT_DIR, help='render 的 --out（metrics_*.csv、input_check.csv、environment.json 在里面）')
    report.add_argument('--out', type=Path, default=None, help='结果写到哪，默认同 --new')
    report.set_defaults(func=cmd_report)

    selftest = sub.add_parser('selftest', help='用 T-085 旧 CSV 校对推导与并排逻辑')
    selftest.set_defaults(func=cmd_selftest)
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except InfraError as exc:
        print(f'错误：{exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
