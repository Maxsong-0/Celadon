#!/usr/bin/env bash
# T-074 验证第二阶段·备树：改前（main 44f7730）与改后（带临时性能对照）两棵临时源码树，各自编译。
#   base  改前：celadon_tests（含临时性能对照，无 BENCH_KEEP）+ celadon_ui_snapshot + celadon_render
#   mut   改后副本：celadon_tests（含临时性能对照，BENCH_KEEP）+ celadon_ui_snapshot；之后红绿矩阵在这棵树上改坏、编译、跑、还原
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
P=$OUT/progress5.txt; : > $P
mkdir -p $OUT/mut $SRC

rm -rf $SRC/t074-base $SRC/t074-mut $SRC/t074-mut-pristine
mkdir -p $SRC/t074-base $SRC/t074-mut
tar -x -C $SRC/t074-base -f $T/base74.tar
tar -x -C $SRC/t074-mut -f $T/new74.tar
cp $T/scripts/ClayBenchTmp.cpp $SRC/t074-base/tests/ClayBenchTmp.cpp
{ printf '#define BENCH_KEEP 1\n'; cat $T/scripts/ClayBenchTmp.cpp; } > $SRC/t074-mut/tests/ClayBenchTmp.cpp
echo "trees extracted $(date +%T)" >> $P

SPARK_JOBS=${SPARK_JOBS:-12} "$SPARK" $SRC/t074-base build celadon_tests celadon_ui_snapshot celadon_render > $OUT/base.build.txt 2>&1
echo "base build exit=$? warnings=$(grep -c ' warning: ' $OUT/base.build.txt) errors=$(grep -c ' error: ' $OUT/base.build.txt) $(date +%T)" >> $P

SPARK_JOBS=${SPARK_JOBS:-12} "$SPARK" $SRC/t074-mut build celadon_tests celadon_ui_snapshot > $OUT/mut.build.txt 2>&1
echo "mut build exit=$? warnings=$(grep -c ' warning: ' $OUT/mut.build.txt) errors=$(grep -c ' error: ' $OUT/mut.build.txt) $(date +%T)" >> $P

# 改坏前的原样：矩阵每个改法跑完后从这里还原；改后的测试二进制另存一份给性能对照用。
mkdir -p $SRC/t074-mut-pristine/Source/dsp $SRC/t074-mut-pristine/Source/ui
for f in Source/dsp/ClayModule.cpp Source/dsp/ClayGuard.cpp Source/dsp/ClayGuard.h Source/dsp/CeladonChain.cpp Source/dsp/GlazeController.cpp Source/ui/HelpText.cpp; do
  cp $SRC/t074-mut/$f $SRC/t074-mut-pristine/$f
done
MUTBUILD=$("$SPARK" $SRC/t074-mut dir)
cp $MUTBUILD/celadon_tests $T/bench-new-tests
echo "pristine saved $(date +%T)" >> $P
echo done >> $P
