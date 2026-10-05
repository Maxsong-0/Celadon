#!/usr/bin/env bash
# T-074 验证第三阶段·运行：性能对照（改前 / 改后各一个二进制，绑 CPU 5，全机锁里）→ 截图 --deterministic 出图（改前 vs 改后逐字节比）
#   → 真歌渲染（改前 vs 改后：成品 wav 逐文件 sha256、metrics.csv、--trace 的逐块表逐字节比）
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
W=/home/maxsong/All-Project/Celadon/.claude/worktrees/complete-remaining-tasks-cdaccd
P=$OUT/progress6.txt; : > $P

SPARK_JOBS=${SPARK_JOBS:-12} "$SPARK" "$W" build celadon_ui_snapshot celadon_render > $OUT/new.build2.txt 2>&1
echo "new build (no-op expected) exit=$? $(date +%T)" >> $P
BASEBUILD=$("$SPARK" $SRC/t074-base dir)
NEWBUILD=$("$SPARK" "$W" dir)

# ---- 1. 性能对照（ClayModule::process 每帧耗时）----
"$SPARK" "$W" run taskset -c 5 "$BASEBUILD/celadon_tests" --only ClayBenchTmp > $OUT/bench-base.txt 2>&1
echo "bench base exit=$? $(date +%T)" >> $P
"$SPARK" "$W" run taskset -c 5 "$T/bench-new-tests" --only ClayBenchTmp > $OUT/bench-new.txt 2>&1
echo "bench new exit=$? $(date +%T)" >> $P
"$SPARK" "$W" run taskset -c 5 "$BASEBUILD/celadon_tests" --only ClayBenchTmp > $OUT/bench-base2.txt 2>&1
"$SPARK" "$W" run taskset -c 5 "$T/bench-new-tests" --only ClayBenchTmp > $OUT/bench-new2.txt 2>&1
echo "bench second round $(date +%T)" >> $P

# ---- 2. 截图 --deterministic 出图：改前 vs 改后 ----
rm -rf $OUT/det-base $OUT/det-new $OUT/home-det-base $OUT/home-det-new
mkdir -p $OUT/det-base $OUT/det-new $OUT/home-det-base/.config $OUT/home-det-new/.config
"$SPARK" "$W" run bash -c "HOME=$OUT/home-det-base XDG_CONFIG_HOME=$OUT/home-det-base/.config xvfb-run -a -s '-screen 0 1280x1024x24' $BASEBUILD/celadon_ui_snapshot --out $OUT/det-base --deterministic > $OUT/det-base.log 2>&1"
echo "det base exit=$? pngs=$(ls $OUT/det-base/*.png 2>/dev/null | wc -l) $(date +%T)" >> $P
"$SPARK" "$W" run bash -c "HOME=$OUT/home-det-new XDG_CONFIG_HOME=$OUT/home-det-new/.config xvfb-run -a -s '-screen 0 1280x1024x24' $NEWBUILD/celadon_ui_snapshot --out $OUT/det-new --deterministic > $OUT/det-new.log 2>&1"
echo "det new exit=$? pngs=$(ls $OUT/det-new/*.png 2>/dev/null | wc -l) $(date +%T)" >> $P
same=0; diff=0; list=""
for f in $OUT/det-base/*.png; do
  b=$(basename "$f")
  if cmp -s "$f" "$OUT/det-new/$b"; then same=$((same+1)); else diff=$((diff+1)); list="$list $b"; fi
done
echo "det compare: base vs new same=$same different=$diff${list:+:$list}" >> $P
only_new=""
for f in $OUT/det-new/*.png; do b=$(basename "$f"); [ -f "$OUT/det-base/$b" ] || only_new="$only_new $b"; done
echo "det only in new:$only_new" >> $P

# ---- 3. 真歌渲染 ----
rm -rf $OUT/songs $OUT/render-base $OUT/render-new
mkdir -p $OUT/songs/deep $OUT/songs/tech $OUT/songs/melodic
ln ~/.cache/celadon-t048-cal/deep/track_0155328/track_0155328.wav $OUT/songs/deep/
ln ~/.cache/celadon-t048-cal/tech/track_0003518/track_0003518.wav $OUT/songs/tech/
ln ~/.cache/celadon-t048-cal/melodic/track_0241941/track_0241941.wav $OUT/songs/melodic/
echo "render start $(date +%T)" >> $P
"$SPARK" "$W" run bash -c "
  i=0
  for tree in base new; do
    if [ \$tree = base ]; then bin=$BASEBUILD/celadon_render; else bin=$NEWBUILD/celadon_render; fi
    for song in deep tech melodic; do
      mkdir -p $OUT/render-\$tree/\$song
      cpu=\$((6 + i % 10)); i=\$((i + 1))
      taskset -c \$cpu nice -n 5 \$bin --in $OUT/songs/\$song --out $OUT/render-\$tree/\$song --glaze 35,70 --clay auto,lowcut --no-replay --trace > $OUT/render-\$tree-\$song.log 2>&1 &
    done
  done
  wait
"
echo "render done exit=$? $(date +%T)" >> $P

cmp_dirs() {   # cmp_dirs <song>：成品 wav / metrics.csv / trace 逐文件比
  local song=$1 same=0 diff=0 missing=0 list=""
  for f in $(cd $OUT/render-base/$song && find . -type f \( -name '*.wav' -o -name '*.csv' \) | sort); do
    if [ ! -f "$OUT/render-new/$song/$f" ]; then missing=$((missing+1)); list="$list $f(missing)"; continue; fi
    if cmp -s "$OUT/render-base/$song/$f" "$OUT/render-new/$song/$f"; then same=$((same+1)); else diff=$((diff+1)); list="$list $f"; fi
  done
  local extra=$(cd $OUT/render-new/$song && find . -type f \( -name '*.wav' -o -name '*.csv' \) | wc -l)
  echo "render $song: files base=$(cd $OUT/render-base/$song && find . -type f \( -name '*.wav' -o -name '*.csv' \) | wc -l) new=$extra identical=$same different=$diff missing=$missing${list:+: $list}"
}
for song in deep tech melodic; do cmp_dirs $song >> $P; done
( cd $OUT/render-new && find . -name '*.wav' | sort | xargs sha256sum | cut -c1-16,65- > $OUT/render-new.sha256 )
( cd $OUT/render-base && find . -name '*.wav' | sort | xargs sha256sum | cut -c1-16,65- > $OUT/render-base.sha256 )
echo "render wav sha256 lines: base=$(wc -l < $OUT/render-base.sha256) new=$(wc -l < $OUT/render-new.sha256) $(date +%T)" >> $P
echo done >> $P
