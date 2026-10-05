#!/usr/bin/env bash
# main 与 T-070 的 celadon_ui_snapshot 各跑一次 --only spectrum-peak-bench，对比 PEAK 关 / 开各组合的帧数、曲线帧、设备像素（时间列有几 % 起伏，不比）
set -u
FINAL=$1; BASE=$2; OUT=$3
mkdir -p $OUT
for tag in base final; do
  if [ $tag = base ]; then BIN=$BASE; else BIN=$FINAL; fi
  home=$OUT/home-$tag; mkdir -p $home/.config
  HOME=$home XDG_CONFIG_HOME=$home/.config xvfb-run -a -s "-screen 0 1280x1024x24" "$BIN/celadon_ui_snapshot" --only spectrum-peak-bench > $OUT/bench-$tag.txt 2>&1
done
echo done > $OUT/bench.done
