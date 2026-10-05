#!/usr/bin/env bash
# zh-glaze90 的逐字节差异是不是运行间的非确定性：同一个二进制各跑 3 次 --deterministic --only zh-glaze90，两两比较
set -u
OUT=$1; FINAL=$2; BASE=$3
mkdir -p "$OUT"
for tag in base final; do
  if [ $tag = base ]; then BIN=$BASE; else BIN=$FINAL; fi
  for i in 1 2 3; do
    home="$OUT/home-$tag-$i"; mkdir -p "$home/.config" "$OUT/$tag-$i"
    HOME="$home" XDG_CONFIG_HOME="$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "$BIN/celadon_ui_snapshot" --out "$OUT/$tag-$i" --deterministic --only zh-glaze90 > /dev/null 2>&1
  done
done
for tag in base final; do
  for pair in "1 2" "1 3" "2 3"; do
    set -- $pair
    cmp -s "$OUT/$tag-$1/zh-glaze90.png" "$OUT/$tag-$2/zh-glaze90.png" && echo "$tag run$1 == run$2 逐字节相同" || echo "$tag run$1 != run$2 不同"
  done
done
for i in 1 2 3; do cmp -s "$OUT/base-$i/zh-glaze90.png" "$OUT/final-$i/zh-glaze90.png" && echo "base-$i == final-$i" || echo "base-$i != final-$i"; done
echo "完成 $(date +%T)"
