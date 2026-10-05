#!/usr/bin/env bash
# T-074 最终候选（提交前）：当前工作树打成 tar → 全新目录 → 三个目标 0 警告编译 → 全套测试 → 截图 --only check → --deterministic 出图与改前逐字节比
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
P=$OUT/progress14.txt; : > $P
rm -rf $SRC/t074-final; mkdir -p $SRC/t074-final
tar -x -C $SRC/t074-final -f $T/new74b.tar
SPARK_JOBS=${SPARK_JOBS:-12} "$SPARK" $SRC/t074-final build celadon_tests celadon_render celadon_ui_snapshot > $OUT/final.build.txt 2>&1
echo "build exit=$? steps=$(grep -c '^\[' $OUT/final.build.txt) warnings=$(grep -c ' warning: ' $OUT/final.build.txt) errors=$(grep -c ' error: ' $OUT/final.build.txt) $(date +%T)" >> $P
BUILD=$("$SPARK" $SRC/t074-final dir)
"$SPARK" $SRC/t074-final test > $OUT/final.full.txt 2>&1
echo "tests exit=$? $(grep -a '^==== celadon_tests' $OUT/final.full.txt | tail -n 1) $(date +%T)" >> $P
grep -a '^!!! ' $OUT/final.full.txt | cut -c1-200 >> $OUT/final.full-failures.txt
rm -rf $OUT/final-cwd $OUT/final-home $OUT/final-det $OUT/final-home-det
mkdir -p $OUT/final-cwd $OUT/final-home/.config $OUT/final-det $OUT/final-home-det/.config
"$SPARK" $SRC/t074-final run bash -c "cd $OUT/final-cwd && HOME=$OUT/final-home XDG_CONFIG_HOME=$OUT/final-home/.config xvfb-run -a -s '-screen 0 1280x1024x24' $BUILD/celadon_ui_snapshot --only check > $OUT/final.check.txt 2>&1"
echo "check exit=$? PASS=$(grep -ac '^PASS' $OUT/final.check.txt) FAIL=$(grep -ac '^FAIL' $OUT/final.check.txt) $(date +%T)" >> $P
"$SPARK" $SRC/t074-final run bash -c "HOME=$OUT/final-home-det XDG_CONFIG_HOME=$OUT/final-home-det/.config xvfb-run -a -s '-screen 0 1280x1024x24' $BUILD/celadon_ui_snapshot --out $OUT/final-det --deterministic > $OUT/final.det.log 2>&1"
echo "det exit=$? pngs=$(ls $OUT/final-det/*.png 2>/dev/null | wc -l) $(date +%T)" >> $P
same=0; diff=0; list=""
for f in $OUT/det-base/*.png; do
  b=$(basename "$f")
  if cmp -s "$f" "$OUT/final-det/$b"; then same=$((same+1)); else diff=$((diff+1)); list="$list $b"; fi
done
echo "det compare: base vs final same=$same different=$diff${list:+:$list}" >> $P
new_same=0
for b in en-guard-recovering en-guard-tooltip zh-guard-recovering zh-guard-tooltip; do cmp -s $OUT/det-new/$b.png $OUT/final-det/$b.png && new_same=$((new_same+1)); done
echo "det: the 4 new pngs identical to the earlier run: $new_same of 4" >> $P
echo done >> $P
