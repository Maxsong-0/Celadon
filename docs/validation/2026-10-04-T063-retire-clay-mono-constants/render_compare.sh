#!/usr/bin/env bash
# T-063 渲染对照：改前（main）/ 改后两版 celadon_render 渲染同一批歌，metrics.csv（含每行的 audio_hash）应逐字节相同——
# 声音与方案逐位不变（7 个常数运行期无读者）。用法（在 Spark 全机锁里跑）：
#   spark-test.sh <源码目录> run bash render_compare.sh <输出目录> <改前构建目录> <改后构建目录> [每个曲风取几首，默认 3]
# 曲库：~/.cache/celadon-t048-cal/{deep,tech,melodic}/<track>/<track>.wav（Jamendo 45 首，T-038 标定用；清单与 BPM 在 manifest.tsv）。
# 通用组：--glaze 0,35,70 --clay auto,off --metrics-only；House 组：deep 曲风 + 俱乐部交付 + 模拟走带（--bpm 取清单、4/4），GLAZE 35、修坯自动。
set -u
OUT=$1; BEFORE=$2; AFTER=$3; PER=${4:-3}
CAL="$HOME/.cache/celadon-t048-cal"
mkdir -p "$OUT"
LIST="$OUT/tracks.txt"; : > "$LIST"
for g in deep tech melodic; do
  ls "$CAL/$g" | sort | head -n "$PER" | while read -r t; do printf '%s\t%s\n' "$g" "$t" >> "$LIST"; done
done
echo "曲目 $(wc -l < "$LIST") 首"

one() {   # one <改前|改后> <构建目录> <曲风目录> <曲目> <组>
  local tag=$1 bin=$2/celadon_render genre=$3 track=$4 kind=$5
  local in="$OUT/in/$track"; mkdir -p "$in"; [ -e "$in/$track.wav" ] || ln -s "$CAL/$genre/$track/$track.wav" "$in/$track.wav"
  local o="$OUT/$tag/$kind/$track"; mkdir -p "$o"
  if [ "$kind" = general ]; then
    "$bin" --in "$in" --out "$o" --glaze 0,35,70 --clay auto,off --metrics-only > "$o/stderr.txt" 2>&1
  else
    local bpm; bpm=$(awk -F'\t' -v t="$track" '$2==t {printf "%d", $3+0.5}' "$CAL/manifest.tsv")
    "$bin" --in "$in" --out "$o" --glaze 35 --clay auto --genre deep --delivery club --bpm "${bpm:-124}" --meter 4/4 --metrics-only > "$o/stderr.txt" 2>&1
  fi
  echo "$tag $kind $track 退出码 $?"
}
export -f one
export OUT CAL

# 通用组：每首一个进程，改前 / 改后各跑一遍（两版并行，最多 16 个进程）
{ while IFS=$'\t' read -r g t; do printf '改前\t%s\t%s\t%s\tgeneral\n' "$BEFORE" "$g" "$t"; printf '改后\t%s\t%s\t%s\tgeneral\n' "$AFTER" "$g" "$t"; done < "$LIST"; } \
  | xargs -P 16 -L 1 bash -c 'one "$0" "$1" "$2" "$3" "$4"' 
# House 组：只取 deep 曲目
{ grep -P '^deep\t' "$LIST" | while IFS=$'\t' read -r g t; do printf '改前\t%s\t%s\t%s\thouse\n' "$BEFORE" "$g" "$t"; printf '改后\t%s\t%s\t%s\thouse\n' "$AFTER" "$g" "$t"; done; } \
  | xargs -P 16 -L 1 bash -c 'one "$0" "$1" "$2" "$3" "$4"'

echo "== metrics.csv 逐字节比较"
same=0; diff=0
for kind in general house; do
  while IFS=$'\t' read -r g t; do
    a="$OUT/改前/$kind/$t/metrics.csv"; b="$OUT/改后/$kind/$t/metrics.csv"
    [ -f "$a" ] && [ -f "$b" ] || continue
    if cmp -s "$a" "$b"; then same=$((same+1)); else diff=$((diff+1)); echo "不同：$kind $t"; fi
  done < "$LIST"
done
rows=$(cat "$OUT"/改后/general/*/metrics.csv "$OUT"/改后/house/*/metrics.csv 2>/dev/null | grep -c '^render,')
echo "相同 $same 份，不同 $diff 份；改后共 $rows 个 render 行（audio_hash 全在里面）"
echo "全部完成 $(date '+%F %T')"
