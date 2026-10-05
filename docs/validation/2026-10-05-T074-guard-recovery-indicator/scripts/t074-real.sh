#!/usr/bin/env bash
# T-074：真歌上的保留比与指示实际亮不亮。临时树 t074-real = 改后源码 + 渲染工具 trace 多两列（clay_peak_guard_active / _kept，只在临时树里，不进仓库）；
# 守护真动作过的两首歌（track_0046047、track_0726856）× GLAZE 35 / 70 × 修坯 自动 / 只留低切，再加一首守护没动过的对照歌。
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
P=$OUT/progress12.txt; : > $P
rm -rf $SRC/t074-real; mkdir -p $SRC/t074-real
tar -x -C $SRC/t074-real -f $T/new74.tar
python3 - $SRC/t074-real/tools/render/RenderMain.cpp <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
a = '"clay_blend_loss_db,"\n                                 + std::string (calibrationTraceHeader()) + "\\n";'
assert text.count(a) == 1, "header anchor"
text = text.replace(a, '"clay_blend_loss_db,clay_peak_guard_active,clay_peak_guard_kept,"\n                                 + std::string (calibrationTraceHeader()) + "\\n";')
b = '+ "," + std::to_string (probe.clayPeakCostBlocks) + "," + fixed (probe.clayBlendLossSmoothedDb, 3) + ","\n'
assert text.count(b) == 1, "row anchor"
text = text.replace(b, b + '                              + (blockStatus.clayPeakGuardActive ? "1" : "0") + "," + fixed (blockStatus.clayPeakGuardKept, 4) + ","\n')
open(path, "w", encoding="utf-8").write(text)
print("patched")
PY
echo "patched $(date +%T)" >> $P
SPARK_JOBS=${SPARK_JOBS:-12} "$SPARK" $SRC/t074-real build celadon_render > $OUT/real.build.txt 2>&1
echo "build exit=$? warnings=$(grep -c ' warning: ' $OUT/real.build.txt) errors=$(grep -c ' error: ' $OUT/real.build.txt) $(date +%T)" >> $P
REAL=$("$SPARK" $SRC/t074-real dir)

rm -rf $OUT/real-out; mkdir -p $OUT/real-out
job() {
  local n=$1
  mkdir -p $OUT/real-out/$n
  nice -n 5 $REAL/celadon_render --in $OUT/songs2/$n --out $OUT/real-out/$n --glaze 35,70 --clay auto,lowcut --no-replay --metrics-only --trace > $OUT/real-out/$n.log 2>&1
}
export -f job
export OUT REAL
"$SPARK" $SRC/t074-real run bash -c "printf '%s\n' track_0046047 track_0726856 track_0155328 | xargs -P 3 -L 1 bash -c 'job \$0'"
echo "render exit=$? $(date +%T)" >> $P
echo done >> $P
