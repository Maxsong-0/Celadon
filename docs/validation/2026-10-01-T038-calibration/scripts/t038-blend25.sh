#!/usr/bin/env bash
# T-038 × T-043：House 档低切目标 25 Hz 高于混合损失的两个测量点（10 / 20 Hz），封顶靠外推。这里在临时副本里把候选改成 { 25, 20 }
# 编一个变体，45 首 Jamendo 按 House 档各渲一遍：变体的 trial_blend10_db = 实测 L(25)，与正式版 10 / 20 两点外推比。不进仓库。
set -euo pipefail
B=$HOME/.cache/celadon-t038-blend25
SRC=$HOME/.cache/celadon-tests/house-mastering-workflow-67aa29-48064f395b0a
CAL=$HOME/.cache/celadon-t038-cal
HEAD_RENDER=$HOME/.cache/celadon-build/house-mastering-workflow-67aa29-48064f395b0a/celadon_render
rm -rf "$B" && mkdir -p "$B"
cp -r "$SRC/Celadon" "$B/Celadon"
ln -s "$SRC/JUCE" "$B/JUCE"
sed -i 's/clayBlendCandidateHz { 10.0f, 20.0f }/clayBlendCandidateHz { 25.0f, 20.0f }/' "$B/Celadon/Source/dsp/GlazeTypes.h"
grep -q 'clayBlendCandidateHz { 25.0f, 20.0f }' "$B/Celadon/Source/dsp/GlazeTypes.h"
cmake -S "$B/Celadon" -B "$B/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$B/cmake.log" 2>&1
nice -n 5 cmake --build "$B/build" --target celadon_render > "$B/build.log" 2>&1
echo "variant built: $(tail -1 "$B/build.log")"
run() {
  local label=$1 bin=$2
  tail -n +2 "$CAL/manifest.tsv" | while IFS=$'\t' read -r cls track bpm _rest; do echo "$cls $track $bpm"; done \
    | xargs -P 12 -L 1 bash -c 'out="'"$B"'/out-'"$label"'/$1"; mkdir -p "$out"; nice -n 5 "'"$bin"'" --in "'"$CAL"'/$0/$1" --out "$out" --glaze 100 --style auto --clay auto --genre "$0" --delivery standard --bpm "$2" --metrics-only --no-replay > "$out.log" 2>&1 || echo "FAIL $1"'
}
date +%T; run head "$HEAD_RENDER"; date +%T; run v25 "$B/build/celadon_render"; date +%T
python3 - "$B" <<'PY'
import csv, glob, math, os, sys, statistics
B = sys.argv[1]
def rows(label):
    out = {}
    for p in glob.glob(f"{B}/out-{label}/*/metrics.csv"):
        track = os.path.basename(os.path.dirname(p))
        for r in csv.DictReader(open(p, encoding='utf-8')):
            if r['kind'] == 'render':
                out[track] = r
    return out
def num(v):
    try: return float(v)
    except: return math.nan
h, v = rows('head'), rows('v25')
limit = 0.09
diffs, mismatch20, decisions, lines = [], 0, [0, 0], []
for track in sorted(h):
    if track not in v: continue
    l10, l20 = num(h[track]['trial_blend10_db']), num(h[track]['trial_blend20_db'])
    l25, l20v = num(v[track]['trial_blend10_db']), num(v[track]['trial_blend20_db'])
    if h[track]['trial_blend20_db'] != v[track]['trial_blend20_db']: mismatch20 += 1
    if not all(map(math.isfinite, (l10, l20, l25))): 
        lines.append(f"{track}: 读数不全 L10={l10} L20={l20} L25={l25}"); continue
    x = math.log(25 / 10) / math.log(20 / 10)
    if l10 > 0 and l20 > 0:
        pred = math.exp(math.log(l10) + (math.log(l20) - math.log(l10)) * x)   # 幂律：L10 · 2.5^q，q = log2(L20 / L10)
    else:
        pred = l10 + (l20 - l10) * x                                         # 直线（同 capLowCutForBlend 的非幂律分支）
    d = l25 - pred
    diffs.append(abs(d))
    decisions[0 if (pred > limit) == (l25 > limit) else 1] += 1
    lines.append(f"{track}: L10 {l10:.4f}  L20 {l20:.4f}  外推 L25 {pred:.4f}  实测 L25 {l25:.4f}  差 {d:+.4f}")
print("\n".join(lines))
print(f"\n{len(diffs)} 首；|实测 − 外推| 中位 {statistics.median(diffs):.4f}、最大 {max(diffs):.4f} dB；≤ 0.01 dB 的 {sum(1 for d in diffs if d <= 0.01)} 首")
print(f"0.09 dB 门槛上封顶判断一致 {decisions[0]} 首、不一致 {decisions[1]} 首；变体 L20 与正式版不逐字相同 {mismatch20} 首")
PY
