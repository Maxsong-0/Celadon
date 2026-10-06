#!/usr/bin/env python3
# celadon_render 门槛检查（ADR-016；取代 docs/validation/2026-09-26-clay/render/gate_check.py 那份 ADR-014 口径的跨链对比，
# 旧脚本留在原处作 T-012 的证据）。用法：gate_check.py <metrics.csv>；有失败返回 1，否则 0。
#
# 判定（失败）：
#   1. GLAZE 0：修坯「自动」与「全关」两组成品逐位相同（audio_hash；设计规格-Clay 第 4 节「逐位直通」）。
#   2. 同决策重放的阳性对照：每组 GLAZE > 0 的「自动」replay_check 必须是 ok（重放与自动链最大样本差 ≤ −180 dBFS；
#      mismatch = 重放走样，门槛对照不可信）；
#      n/a（有块的实际值重放链照搬不了，原因写在 metrics.md 该曲备注；ADR-015 起不再因自动收单出现）只列说明。
#   3. 修坯门槛（ADR-014 的阈值，ADR-016 的口径）：每首歌、每个 GLAZE > 0，「自动」对「重放全关」（同一份逐块参数、只差修坯）——
#        - 限幅器入口峰值（max(GR − 本块实际 Limit Gain)）增加 ≤ 0.3 dB；
#        - 整体响度下降 ≤ 0.1 LU；响度下降但入口峰值同降（差不超过 0.05 dB）= 切掉的是超低频能量、限幅器少压，列说明不算失败；
#        - 反相提示时长占比（phase_guard_pct，列名沿用旧名；收单灯琥珀那条，不含 House 档反相提示）≤ 5 %：看「自动」行
#          自己的值（重放全关是 GLAZE 0 的链，这一列恒为 0，不比差）。ADR-015 起反相只提示、不改声音，这条查的是提示误亮；
#          只判插件慢路 ρ 从未 < −0.2 的歌，有真反相段的歌豁免、列出。
#   4. 连续编排器（T-048 阶段 2）：行里有非空 confidence 时，有效音乐 ≥ 30 s、GLAZE > 0 的成品（不含重放行）
#      末尾必须 c ≥ 0.5（主段统计已热，不再停在「起步」）。没有这列、或格子是空的，仍按阶段 1「已锁定 + 校准有效」
#      （编排器还没把 c 写进指标表时，旧表的输出一字不差）。更短的列「时长不够」说明。
#   5. 真峰值：所有成品 ≤ Ceiling + 0.01 dB（渲染工具一律用出厂 Ceiling −1.0 dBTP）。
# 参考（只打印，不判定）：「自动」对「全关」的跨链差——两条链各自跑 GLAZE，约束反馈发生的时刻不同就会走到不同的方案，
# 差别里混着方案分岔（T-015：WTF g70 两条链反馈上限 1.145 / 0.274 dB），所以不再拿它判定。
#
# 配对：同一组「自动 / 全关 / 重放全关」按（歌, GLAZE, 风格）找；指标表有 T-038 追加列 genre / delivery / listen_from_bar 时
# 这三列也并入配对键（--genre / --delivery / --listen-from-bar 多值网格里，同一首歌同一 GLAZE 有几组「自动」，各自成组）。
# 某一维在表里取值不止一种时，分组标签与表格才写出它；只有一种取值（或没有这几列）时输出与只按（歌, GLAZE, 风格）配对时逐字节相同。
import csv
import math
import sys
from collections import defaultdict

CEILING_DBTP = -1.0
# 同决策重放阳性对照的容差（dBFS）：与 RenderMain.cpp 的 replayTolerance（1e-9 = −180 dBFS）同值，改一处要同步改另一处
REPLAY_TOLERANCE_DBFS = -180
MIN_LOCK_MUSIC_SECONDS = 30.0
CONFIDENCE_READY = 0.5


def phase2_confidence(row):
    """非空 confidence 才算阶段 2 的格子。缺列或空串返回 None，调用方退回阶段 1 的锁定判定。"""
    if 'confidence' not in row:
        return None
    raw = (row.get('confidence') or '').strip()
    if raw == '':
        return None
    return num(raw)

# T-038 网格的另三维（celadon_render 只在用了新选项时把它们追加在 metrics.csv 末尾）。渲染工具自己也按这三维配对
# （RenderMain.cpp 的 checkGlazeZeroPairs；重放全关行沿用「自动」行的组合），这里与它一致。
GRID_COLUMNS = ('genre', 'delivery', 'listen_from_bar')


def num(value, default=math.nan):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def input_peak_delta(a, b):
    """限幅器入口峰值差 a − b（dB）。空 = 这组整首限幅器都没动作：两边都空记 0；只有 a 空说明 a 更低，记 −inf；只有 b 空记 +inf。"""
    pa, pb = a.get('limit_input_peak_db') or '', b.get('limit_input_peak_db') or ''
    if not pa and not pb:
        return 0.0
    if not pa:
        return -math.inf
    if not pb:
        return math.inf
    return float(pa) - float(pb)


def grid_value(row, column):
    """网格维度的取值（成品行总有；空串兜底，免得手拼的表缺格时 None 混进排序）。"""
    return row.get(column) or ''


def grid_sort_value(column, value):
    """配对键里网格维度的排序值：聆听起点按小节号的数值排（2 在 10 前），曲风 / 交付按字符串排（与 house_gate.py 相同）。"""
    return num(value, math.inf) if column == 'listen_from_bar' else value


def grid_label(column, value):
    """分组标签里网格维度的写法：聆听起点照成品文件名写 from-b<小节>，曲风 / 交付写选项名（RenderMain.cpp 的 comboSuffix）。"""
    return f"from-b{value}" if column == 'listen_from_bar' else value


def main(path):
    with open(path, encoding='utf-8') as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        columns = reader.fieldnames or []
    inputs = {r['song']: r for r in rows if r['kind'] == 'input'}
    renders = [r for r in rows if r['kind'] == 'render']

    # 配对键 =（歌, GLAZE, 风格）+ 表里有的网格列。分组标签与表格只写出取值不止一种的网格列（表格接在 g 列之后）：
    # 没有这几列、或每列只有一种取值时，输出与只按（歌, GLAZE, 风格）配对时逐字节相同。
    grid = [c for c in GRID_COLUMNS if c in columns]
    shown = [c for c in grid if len({grid_value(r, c) for r in renders}) > 1]
    widths = {c: max([len(c)] + [len(grid_value(r, c)) for r in renders]) for c in shown}

    by = defaultdict(dict)
    for r in renders:
        by[(r['song'], r['glaze'], r['style']) + tuple(grid_value(r, c) for c in grid)][r['clay']] = r

    fails, notes = [], []
    has_replay = any('replay-off' in pair for pair in by.values())
    if not has_replay:
        notes.append('指标表里没有 clay-replay-off 行（旧工具，或用了 --no-replay）：修坯门槛无法按 ADR-016 口径判定')

    header = (f"{'song':30} {'g':>3}" + ''.join(f" {c:>{widths[c]}}" for c in shown)
              + f" {'dInPk':>7} {'dLUFS':>7} {'ref:dInPk':>9} {'ref:dLUFS':>9} {'hint%':>6} {'inv%':>5} "
              f"{'music_s':>7} {'lock(auto/off)':>16} {'replay':>6}")
    print(header)

    def sort_key(item):
        key = item[0]
        return (key[0], num(key[1]), key[2]) + tuple(grid_sort_value(c, v) for c, v in zip(grid, key[3:]))

    for key, pair in sorted(by.items(), key=sort_key):
        song, g, style = key[:3]
        dims = dict(zip(grid, key[3:]))
        glaze = num(g)
        tag = f"{song} g{g}" + ("" if style == 'auto' else f" {style}") + ''.join(f" {grid_label(c, dims[c])}" for c in shown)
        music = num(inputs.get(song, {}).get('music_seconds'))

        # 5. 真峰值（每一行）
        for clay, r in pair.items():
            tp = num(r.get('true_peak_dbtp'))
            if not math.isnan(tp) and tp > CEILING_DBTP + 0.01:
                fails.append(f"{tag} clay-{clay}: 真峰值 {tp:.3f} dBTP > Ceiling {CEILING_DBTP} + 0.01")

        if glaze == 0.0:
            # 1. GLAZE 0 逐位直通
            if 'auto' in pair and 'off' in pair and pair['auto']['audio_hash'] != pair['off']['audio_hash']:
                fails.append(f"{tag}: GLAZE 0 修坯「自动」与「全关」不逐位相同（违反逐位直通）")
            continue

        a = pair.get('auto')

        # 4. 连续编排器，或阶段 1 的锁定（不含重放行：重放链是 GLAZE 0，没有 GLAZE 状态）
        locks = []
        for clay, r in sorted(pair.items()):
            if clay == 'replay-off':
                continue
            confidence = phase2_confidence(r)
            if confidence is not None:
                settled = confidence >= CONFIDENCE_READY
                locks.append('C' if settled else 'cold')
                if settled:
                    continue
                state = f"confidence {confidence:.3f} < {CONFIDENCE_READY:.1f}"
                if math.isnan(music):
                    notes.append(f"{tag} clay-{clay}: {state}——指标表没有有效音乐时长，热度无法按时长判定")
                elif music >= MIN_LOCK_MUSIC_SECONDS:
                    fails.append(f"{tag} clay-{clay}: 有效音乐 {music:.1f} s ≥ {MIN_LOCK_MUSIC_SECONDS:.0f} s，成品末尾 {state}")
                else:
                    notes.append(f"{tag} clay-{clay}: {state}——有效音乐 {music:.1f} s，时长不够热起（不算失败）")
                continue
            locked = r.get('glaze_phase') == 'locked' and r.get('calibration_valid') == '1'
            locks.append('L' if locked else r.get('glaze_phase', '?')[:5])
            if locked:
                continue
            state = f"{r.get('glaze_phase')} / 校准{'有效' if r.get('calibration_valid') == '1' else '无效'}"
            if math.isnan(music):
                notes.append(f"{tag} clay-{clay}: {state}——指标表没有有效音乐时长（旧工具），锁定无法按时长判定")
            elif music >= MIN_LOCK_MUSIC_SECONDS:
                fails.append(f"{tag} clay-{clay}: 有效音乐 {music:.1f} s ≥ {MIN_LOCK_MUSIC_SECONDS:.0f} s，成品末尾却是 {state}")
            else:
                notes.append(f"{tag} clay-{clay}: {state}——有效音乐 {music:.1f} s，时长不够锁定（不算失败）")

        if a is None:
            continue

        # 2. 阳性对照
        check = a.get('replay_check') or ''
        if has_replay:
            if check == 'n/a':
                notes.append(f"{tag}: 同决策重放的阳性对照不适用（原因见 metrics.md 该曲备注），修坯门槛照常用重放全关判定")
            elif check != 'ok':
                diff = a.get('replay_max_diff_dbfs') or '?'
                fails.append(f"{tag}: 同决策重放的阳性对照 {check or '缺失'}（应为 ok；最大样本差 {diff} dBFS，允许 ≤ −{abs(REPLAY_TOLERANCE_DBFS)}），修坯门槛对照不可信")

        # 参考：跨链
        o = pair.get('off')
        ref_dinp = input_peak_delta(a, o) if o else math.nan
        ref_dl = num(a['lufs']) - num(o['lufs']) if o else math.nan

        # 3. 修坯门槛（自动 vs 重放全关）
        rp = pair.get('replay-off')
        dinp = input_peak_delta(a, rp) if rp else math.nan
        dl = num(a['lufs']) - num(rp['lufs']) if rp else math.nan
        guard = num(a.get('phase_guard_pct'), 0.0)
        inv = num(a.get('slow_inverted_pct'), 0.0)

        print(f"{song[:30]:30} {g:>3}" + ''.join(f" {dims[c]:>{widths[c]}}" for c in shown)
              + f" {dinp:+7.3f} {dl:+7.3f} {ref_dinp:+9.3f} {ref_dl:+9.3f} {guard:6.2f} {inv:5.2f} "
              f"{music:7.1f} {'/'.join(locks):>16} {check:>6}")

        if rp is not None:
            if not (dinp <= 0.3):
                what = '自动链限幅器动作了、重放全关整首没动作' if dinp == math.inf else f"+{dinp:.3f} dB"
                fails.append(f"{tag}: 修坯让限幅器入口峰值增加 {what}（> 0.3 dB；对比同决策重放全关）")
            if dl < -0.1:
                if dinp <= dl + 0.05:
                    notes.append(f"{tag}: 响度 {dl:+.3f} LU、限幅器入口峰值 {dinp:+.3f} dB，一起下降（切掉的是超低频能量、限幅器少压）")
                else:
                    fails.append(f"{tag}: 修坯让响度下降 {dl:+.3f} LU（< −0.1），限幅器入口峰值没有跟着降（{dinp:+.3f} dB）")

        if guard > 5.0:
            if inv > 0.0:
                notes.append(f"{tag}: 反相提示时长占比 {guard:.1f} %——豁免：插件慢路 ρ < −0.2 的块占 {inv:.2f} %（有真反相段）")
            else:
                fails.append(f"{tag}: 反相提示时长占比 {guard:.1f} % > 5 %，而插件慢路 ρ 从未 < −0.2（误触发）")

    print(f"\nFAILS: {len(fails)}")
    for f in fails:
        print(f"  - {f}")
    print(f"NOTES: {len(notes)}")
    for n in notes:
        print(f"  - {n}")
    return 1 if fails else 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('用法：gate_check.py <metrics.csv>', file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
