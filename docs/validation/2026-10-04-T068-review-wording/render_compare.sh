#!/usr/bin/env bash
# T-068 渲染对照：改前（main）/ 改后两版 celadon_render 渲染同一批歌——metrics.csv 逐字节相同（声音与指标不变），
# metrics.md 只差预期的口径句（去掉「生成时间」后）。csv 部分直接用 T-063 的脚本；这里再加 md 对照。
# 用法（在 Spark 全机锁里跑）：spark-test.sh <源码目录> run bash render_compare.sh <输出目录> <改前构建目录> <改后构建目录> [每个曲风取几首，默认 3]
set -u
OUT=$1; BEFORE=$2; AFTER=$3; PER=${4:-3}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash "$HERE/../2026-10-04-T063-retire-clay-mono-constants/render_compare.sh" "$OUT" "$BEFORE" "$AFTER" "$PER"
echo "== metrics.md 对照（去掉生成时间行内的时间）"
norm() { sed -E 's/生成时间：[^（]*/生成时间：X/' "$1"; }
files=0; same=0; changed=0
for kind in general house; do
  while IFS=$'\t' read -r g t; do
    a="$OUT/改前/$kind/$t/metrics.md"; b="$OUT/改后/$kind/$t/metrics.md"
    [ -f "$a" ] && [ -f "$b" ] || continue
    files=$((files+1))
    if diff <(norm "$a") <(norm "$b") >/dev/null; then same=$((same+1)); else changed=$((changed+1)); fi
  done < "$OUT/tracks.txt"
done
echo "metrics.md：$files 份，完全相同 $same，有差别 $changed"
first=$(ls "$OUT"/改前/general/*/metrics.md | head -1); t=$(basename "$(dirname "$first")")
echo "-- 以 $t 为例的差别（< 改前 / > 改后）："
diff <(norm "$OUT/改前/general/$t/metrics.md") <(norm "$OUT/改后/general/$t/metrics.md") | cut -c1-400
echo "全部完成 $(date '+%F %T')"
