#!/usr/bin/env bash
# main 工具与 T-070 工具各跑一次 --only check，量墙钟（同一台机、锁里串行；第一遍预热不计，取第二遍）
set -u
FINAL=$1; BASE=$2; OUT=$3
mkdir -p $OUT
for tag in base final; do
  if [ $tag = base ]; then BIN=$BASE; else BIN=$FINAL; fi
  for round in 1 2; do
    home=$OUT/home-$tag-$round; mkdir -p $home/.config
    s=$(date +%s.%N)
    HOME=$home XDG_CONFIG_HOME=$home/.config xvfb-run -a -s "-screen 0 1280x1024x24" "$BIN/celadon_ui_snapshot" --only check > $OUT/check-$tag-$round.txt 2>&1
    e=$(date +%s.%N)
    echo "$tag round$round exit=$? wall=$(python3 -c "print(round($e-$s,1))")s PASS=$(grep -c '^PASS' $OUT/check-$tag-$round.txt) FAIL=$(grep -c '^FAIL' $OUT/check-$tag-$round.txt)"
  done
done
echo done
