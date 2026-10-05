#!/usr/bin/env bash
# 完整 --deterministic（30 张）同一个二进制各跑 2 次，再和另一个二进制比：找出哪些图在「运行间」就不稳定
set -u
OUT=$1; FINAL=$2; BASE=$3
mkdir -p "$OUT"
for tag in base final; do
  if [ $tag = base ]; then BIN=$BASE; else BIN=$FINAL; fi
  for i in 1 2; do
    home="$OUT/home-$tag-$i"; mkdir -p "$home/.config" "$OUT/$tag-$i"
    HOME="$home" XDG_CONFIG_HOME="$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "$BIN/celadon_ui_snapshot" --out "$OUT/$tag-$i" --deterministic > /dev/null 2>&1
  done
done
cmpdir() {   # cmpdir <标签> <目录 A> <目录 B>
  local n=0 d=0 list=""
  for f in "$2"/*.png; do b=$(basename "$f"); if cmp -s "$f" "$3/$b"; then n=$((n+1)); else d=$((d+1)); list="$list $b"; fi; done
  echo "$1：相同 $n 张，不同 $d 张${list:+：$list}"
}
cmpdir "base-1 vs base-2（同一个 main 工具两次）" "$OUT/base-1" "$OUT/base-2"
cmpdir "final-1 vs final-2（同一个新工具两次）" "$OUT/final-1" "$OUT/final-2"
cmpdir "base-1 vs final-1" "$OUT/base-1" "$OUT/final-1"
cmpdir "base-2 vs final-2" "$OUT/base-2" "$OUT/final-2"
cmpdir "base-1 vs final-2" "$OUT/base-1" "$OUT/final-2"
echo "完成 $(date +%T)"
