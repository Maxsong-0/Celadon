#!/usr/bin/env bash
# T-074 性能对照（安静的机器上、绑 CPU 5、全机锁里）：ClayModule::process 每帧耗时，改前（main）与改后（统计关 / 开）交替跑 3 轮。
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
W=/home/maxsong/All-Project/Celadon/.claude/worktrees/complete-remaining-tasks-cdaccd
P=$OUT/progress13.txt; : > $P
BASEBUILD=$("$SPARK" $SRC/t074-base dir)
for round in 1 2 3; do
  "$SPARK" "$W" run taskset -c 5 "$BASEBUILD/celadon_tests" --only ClayBenchTmp > $OUT/bench-base-$round.txt 2>&1
  echo "base round $round exit=$? $(date +%T)" >> $P
  "$SPARK" "$W" run taskset -c 5 "$T/bench-new-tests" --only ClayBenchTmp > $OUT/bench-new-$round.txt 2>&1
  echo "new round $round exit=$? $(date +%T)" >> $P
done
echo done >> $P
