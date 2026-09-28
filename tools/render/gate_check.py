#!/usr/bin/env python3
# celadon_render 门槛检查（ADR-016；取代 docs/validation/2026-09-26-clay/render/gate_check.py 那份 ADR-014 口径的跨链对比，
# 旧脚本留在原处作 T-012 的证据）。用法：gate_check.py <metrics.csv>；有失败返回 1，否则 0。
#
# 判定（失败）：
#   1. GLAZE 0：修坯「自动」与「全关」两组成品逐位相同（audio_hash；设计规格-Clay 第 4 节「逐位直通」）。
#   2. 同决策重放的阳性对照：每组 GLAZE > 0 的「自动」replay_check 必须是 ok（重放与自动链最大样本差 ≤ −180 dBFS；
#      mismatch = 重放走样，门槛对照不可信）；
#      n/a（自动收单比例在 0、1 之间，「手动」态表达不了）只列说明。
#   3. 修坯门槛（ADR-014 的阈值，ADR-016 的口径）：每首歌、每个 GLAZE > 0，「自动」对「重放全关」（同一份逐块参数、只差修坯）——
#        - 限幅器入口峰值（max(GR − 本块实际 Limit Gain)）增加 ≤ 0.3 dB；
#        - 整体响度下降 ≤ 0.1 LU；响度下降但入口峰值同降（差不超过 0.05 dB）= 切掉的是超低频能量、限幅器少压，列说明不算失败；
#        - 反相保护介入 ≤ 5 %（插件慢路 ρ 从未 < −0.2 的歌；有真反相段的歌豁免、列出）。
#   4. 锁定：有效音乐（原曲 music_seconds）≥ 30 s 的歌，GLAZE > 0 的成品（自动 / 全关 / 单项，不含重放行）末尾必须「已锁定 + 校准有效」；
#      更短的列「时长不够」说明（聆听要 15 s 有效音频，一轮校准约 13 s、误差 > 0.25 dB 还要第二轮；BUG-003「另外两类」）。
#   5. 真峰值：所有成品 ≤ Ceiling + 0.01 dB（渲染工具一律用出厂 Ceiling −1.0 dBTP）。
# 参考（只打印，不判定）：「自动」对「全关」的跨链差——两条链各自跑 GLAZE，约束反馈发生的时刻不同就会走到不同的方案，
# 差别里混着方案分岔（T-015：WTF g70 两条链反馈上限 1.145 / 0.274 dB），所以不再拿它判定。
import csv
import math
import sys
from collections import defaultdict

CEILING_DBTP = -1.0
MIN_LOCK_MUSIC_SECONDS = 30.0


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


def main(path):
    rows = list(csv.DictReader(open(path, encoding='utf-8')))
    inputs = {r['song']: r for r in rows if r['kind'] == 'input'}
    by = defaultdict(dict)
    for r in rows:
        if r['kind'] == 'render':
            by[(r['song'], r['glaze'], r['style'])][r['clay']] = r

    fails, notes = [], []
    has_replay = any('replay-off' in pair for pair in by.values())
    if not has_replay:
        notes.append('指标表里没有 clay-replay-off 行（旧工具，或用了 --no-replay）：修坯门槛无法按 ADR-016 口径判定')

    header = (f"{'song':30} {'g':>3} {'dInPk':>7} {'dLUFS':>7} {'ref:dInPk':>9} {'ref:dLUFS':>9} {'guard%':>6} {'inv%':>5} "
              f"{'music_s':>7} {'lock(auto/off)':>16} {'replay':>6}")
    print(header)

    for (song, g, style), pair in sorted(by.items(), key=lambda kv: (kv[0][0], num(kv[0][1]), kv[0][2])):
        glaze = num(g)
        tag = f"{song} g{g}" + ("" if style == 'auto' else f" {style}")
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

        # 4. 锁定（不含重放行：重放链是 GLAZE 0，没有 GLAZE 状态）
        locks = []
        for clay, r in sorted(pair.items()):
            if clay == 'replay-off':
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
                notes.append(f"{tag}: 同决策重放的阳性对照不适用（自动收单比例在 0、1 之间），修坯门槛照常用重放全关判定")
            elif check != 'ok':
                diff = a.get('replay_max_diff_dbfs') or '?'
                fails.append(f"{tag}: 同决策重放的阳性对照 {check or '缺失'}（应为 ok；最大样本差 {diff} dBFS，允许 ≤ −180），修坯门槛对照不可信")

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

        print(f"{song[:30]:30} {g:>3} {dinp:+7.3f} {dl:+7.3f} {ref_dinp:+9.3f} {ref_dl:+9.3f} {guard:6.2f} {inv:5.2f} "
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
                notes.append(f"{tag}: 反相保护介入 {guard:.1f} %——豁免：插件慢路 ρ < −0.2 的块占 {inv:.2f} %（有真反相段）")
            else:
                fails.append(f"{tag}: 反相保护介入 {guard:.1f} % > 5 %，而插件慢路 ρ 从未 < −0.2（误触发）")

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
