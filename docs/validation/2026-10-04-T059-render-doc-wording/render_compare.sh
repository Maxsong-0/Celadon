#!/usr/bin/env bash
# T-059 渲染对照（在 spark-test.sh run 的全机锁里跑）：改前 / 改后两版 celadon_render 渲染同一首合成曲，
# --glaze 0,35,70 --clay auto,off,mono --metrics-only：metrics.csv 应逐字节相同，metrics.md 只差生成时间与改的三处文字，
# mono 行与 off 行 audio_hash 相同、mono 行 phase_guard_pct 与 auto 行相同。用法：render_compare.sh <输出目录> <改前构建目录> <改后构建目录> <合成曲目录>
# 合成曲：docs/validation/2026-10-04-T056-phase-hint-wording/make_synth_phase.py <目录>/synth-phase.wav（Spark 上用 ~/.cache/celadon-spark/venv/bin/python）。
# 在 Spark 上放进全机锁：spark-test.sh <源码目录> run bash render_compare.sh …
set -u
OUT=$1; BEFORE=$2; AFTER=$3; SONG=$4
mkdir -p "$OUT/before" "$OUT/after"
"$BEFORE/celadon_render" --in "$SONG" --out "$OUT/before" --glaze 0,35,70 --clay auto,off,mono --metrics-only > "$OUT/before.stderr" 2>&1; echo "before 退出码 $?"
"$AFTER/celadon_render"  --in "$SONG" --out "$OUT/after"  --glaze 0,35,70 --clay auto,off,mono --metrics-only > "$OUT/after.stderr" 2>&1; echo "after 退出码 $?"
echo "== metrics.csv 逐字节比较"
cmp "$OUT/before/metrics.csv" "$OUT/after/metrics.csv" && echo "metrics.csv 逐字节相同 ($(wc -c < "$OUT/after/metrics.csv") 字节)"
echo "== metrics.md 差别"
diff "$OUT/before/metrics.md" "$OUT/after/metrics.md" > "$OUT/metrics-md.diff"; echo "diff 退出码 $?（1 = 有差别，预期）"; cat "$OUT/metrics-md.diff"
echo "== mono 行与 off 行 audio_hash、与 auto 行 phase_guard_pct"
python3 - "$OUT/after/metrics.csv" <<'PY'
import csv, sys
rows = [r for r in csv.DictReader(open(sys.argv[1], encoding="utf-8")) if r["kind"] == "render"]
by = {(r["glaze"], r["clay"]): r for r in rows}
for g in ("0", "35", "70"):
    for c in ("auto", "off", "mono"):
        r = by.get((g, c))
        if r:
            print(f"glaze {g:>2} clay {c:<5} audio_hash {r['audio_hash']}  phase_guard_pct {r['phase_guard_pct']:>6}  width_guard_pct {r['width_guard_pct']:>6}  clay_cost_limited {r['clay_cost_limited'] or '-'}")
ok = all(by[(g, "mono")]["audio_hash"] == by[(g, "off")]["audio_hash"] for g in ("0", "35", "70") if (g, "mono") in by)
ok2 = all(by[(g, "mono")]["phase_guard_pct"] == by[(g, "auto")]["phase_guard_pct"] for g in ("35", "70") if (g, "mono") in by)
print("mono 行 audio_hash 与 off 行相同：", ok, "；mono 行 phase_guard_pct 与 auto 行相同（GLAZE 35 / 70）：", ok2)
PY
echo "stderr 差别（输出目录路径除外）："; diff <(sed "s#$OUT/before#OUT#g" "$OUT/before.stderr") <(sed "s#$OUT/after#OUT#g" "$OUT/after.stderr") | head -10
echo "全部完成 $(date '+%F %T')"
