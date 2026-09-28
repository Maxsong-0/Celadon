#!/usr/bin/env python3
# 修坯门槛（设计规格-Clay-根治修订 第 4 节第 1 条，ADR-014 校正口径）：每首歌、每个 GLAZE > 0，修坯「自动」对「全关」——
#   限幅器入口峰值（max(GR − 本块实际 Limit Gain)，扣掉两条链各自校准出的增益差）增加 ≤ 0.3 dB、整体响度下降 ≤ 0.1 LU；
#   （响度下降时入口峰值同等下降 = 切掉的是超低频能量、限幅器少压，列入说明不算失败）；
#   反相保护介入时间 ≤ 5 %（插件慢路 ρ 从未 < −0.2 的歌；有真反相段的歌豁免、列出）。GR 最大照列（参考：含增益差）。
# GLAZE 0：自动 / 全关逐位相同（audio_hash）。用法：gate_check.py <metrics.csv>
import csv, sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1], encoding='utf-8')))
by = defaultdict(dict)
inputs = {}
for r in rows:
    if r['kind'] == 'input':
        inputs[r['song']] = r
        continue
    by[(r['song'], r['glaze'], r['style'])][r['clay']] = r

fails, notes = [], []
print(f"{'song':32} {'g':>3} {'dInPk':>7} {'dGRmax':>7} {'dLUFS':>7} {'guard%':>7} {'inv%':>6} {'pg%':>6} {'scale':>5} {'lc':>6} {'mono':>6} {'w':>6} {'cost':>12}")
for (song, g, style), pair in sorted(by.items(), key=lambda kv: (kv[0][0], float(kv[0][1]))):
    if 'auto' not in pair or 'off' not in pair:
        continue
    a, o = pair['auto'], pair['off']
    if float(g) == 0.0:
        if a['audio_hash'] != o['audio_hash']:
            fails.append(f"{song} g0: auto/off not bit-identical")
        continue
    dgr = float(a['limit_gr_max_db']) - float(o['limit_gr_max_db'])
    ai, oi = a.get('limit_input_peak_db') or '', o.get('limit_input_peak_db') or ''
    dinp = (float(ai) - float(oi)) if ai and oi else (0.0 if not ai and not oi else float('nan'))
    dl = float(a['lufs']) - float(o['lufs'])
    guard = float(a['phase_guard_pct'] or 0)
    inv = float(a.get('slow_inverted_pct') or 0)
    pg = a.get('peak_guard_pct', '')
    pgm = a.get('peak_guard_min_gain', '')
    sc = a.get('clay_pressure_scale', '')
    print(f"{song[:32]:32} {g:>3} {dinp:+7.3f} {dgr:+7.3f} {dl:+7.3f} {guard:7.2f} {inv:6.2f} {pg:>6} {sc:>5} {a['clay_lowcut_hz']:>6} {a['clay_mono_amount']:>6} {a['clay_width_db']:>6} {a.get('clay_cost_limited',''):>12}")
    if not (dinp <= 0.3):
        fails.append(f"{song} g{g}: limiter input peak +{dinp:.3f} dB > 0.3 (GR max {dgr:+.3f})")
    # 响度下降 > 0.1 LU 且入口峰值没有跟着同等下降（ΔinPeak > ΔLUFS + 0.05）才算伤害：峰值与响度一起降 = 切掉的是超低频能量、
    # 限幅器反而少压（PLR 升高），不是被多压；两条链各自校准，响度本来就有 ±0.25 dB 的收敛容差（GlazeCalibrator）。
    if dl < -0.1:
        if dinp == dinp and dinp <= dl + 0.05:
            notes.append(f"{song} g{g}: loudness {dl:+.3f} LU with limiter input peak {dinp:+.3f} dB (dropped together: removed low energy, less limiting)")
        else:
            fails.append(f"{song} g{g}: loudness {dl:+.3f} LU < -0.1 and the limiter input peak did not drop with it ({dinp:+.3f} dB)")
    if guard > 5.0:
        if inv > 0.0:
            notes.append(f"{song} g{g}: phase guard {guard:.1f} % — exempt: plugin slow ρ < −0.2 for {inv:.2f} % of valid blocks")
        else:
            fails.append(f"{song} g{g}: phase guard {guard:.1f} % > 5 % with no slow ρ < −0.2 at all")

print()
print("FAILS:", len(fails))
for f in fails: print("  -", f)
print("NOTES (exempt / explained):", len(notes))
for n in notes: print("  -", n)
