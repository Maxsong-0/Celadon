#!/usr/bin/env bash
# T-043 回路（照 T-042 的 t042-render.sh）：按提交编 celadon_render、只渲染指定的门槛歌、本机跑 gate_check。
#   build <提交|wt>          git archive 该提交（wt = 本工作树当前文件）同步到 109、编 celadon_render、自检，
#                            二进制存成 ~/.cache/celadon-t043/bin/celadon_render.<标签>
#   run <标签> <本地输出目录> <歌名,歌名…> [celadon_render 额外参数…]
#                            每首歌一个进程并行（--metrics-only），拉回本机，合并 metrics.csv，跑 gate_check
set -euo pipefail
WT=/Users/maxsong/All-Project/Celadon/.claude/worktrees/serene-goodall-e48f64
SCR=/private/tmp/claude-501/-Users-maxsong-All-Project-Celadon--claude-worktrees-serene-goodall-e48f64/d79b7637-9e96-45f2-9a6b-5a98da11ce09/scratchpad
JUCE_DIR=/Users/maxsong/All-Project/JUCE
HOST=${CELADON_TEST_HOST:-maxsong@192.168.100.109}
SONGS=.cache/celadon-render/songs-9a355b99857c
BASE=.cache/celadon-t043
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
source "$WT/scripts/paths.sh"

cmd="$1"; shift
case "$cmd" in
build)
  C="$1"
  if [ "$C" = wt ]; then
    LABEL="wt"; SRC="$WT"
  elif [ "${C#dir:}" != "$C" ]; then
    LABEL="${C#dir:}"; SRC="$SCR/src-$LABEL"   # 事先在 scratchpad 准备好的变体源码
  else
    LABEL="$(git -C "$WT" rev-parse --short=7 "$C")"; SRC="$SCR/src-$LABEL"
    rm -rf "$SRC"; mkdir -p "$SRC"
    git -C "$WT" archive "$LABEL" | tar -x -C "$SRC"
  fi
  "${SSH[@]}" "$HOST" "mkdir -p '$BASE/src-$LABEL/Celadon' '$BASE/JUCE' '$BASE/bin' && ln -sfn ../JUCE '$BASE/src-$LABEL/JUCE'"
  rsync -rlpD --checksum --delete --delete-excluded "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" \
    -e "${SSH[*]}" "$SRC"/ "$HOST:$BASE/src-$LABEL/Celadon/"
  rsync -a --delete --delete-excluded --include '/LICENSE.md' --include '/modules/***' --exclude '*' \
    -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$BASE/JUCE/"
  "${SSH[@]}" "$HOST" "BASE=$(printf '%q' "$BASE") LABEL=$(printf '%q' "$LABEL") bash -euo pipefail -s" <<'EOS'
cd "$HOME/$BASE/src-$LABEL"
BUILD="$HOME/$BASE/build-$LABEL"
mkdir -p "$BUILD"
nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
nice -n 10 cmake --build "$BUILD" --target celadon_render 2>&1 | grep -i -E 'warning|error|Linking' | tail -20 || true
nice -n 10 "$BUILD/celadon_render" --selftest 2>&1 | tail -3 || echo "（自检未全过，诊断变体照样保留二进制）"
cp "$BUILD/celadon_render" "$HOME/$BASE/bin/celadon_render.$LABEL"
echo "built → ~/$BASE/bin/celadon_render.$LABEL"
EOS
  ;;
run)
  LABEL="$1"; OUT_LOCAL="$2"; LIST="$3"; shift 3
  ARGS=""
  if [ "$#" -gt 0 ]; then ARGS="$(printf '%q ' "$@")"; fi
  RUN="out-$LABEL-$(date +%H%M%S)-$$"
  "${SSH[@]}" "$HOST" "BASE=$(printf '%q' "$BASE") SONGS=$(printf '%q' "$SONGS") LABEL=$(printf '%q' "$LABEL") LIST=$(printf '%q' "$LIST") RUN=$(printf '%q' "$RUN") ARGS=$(printf '%q' "$ARGS") bash -euo pipefail -s" <<'EOS'
BIN="$HOME/$BASE/bin/celadon_render.$LABEL"
OUT="$HOME/$BASE/$RUN"
mkdir -p "$OUT"
eval "set -- $ARGS"
pids=()
IFS=',' read -r -a names <<< "$LIST"
for s in "${names[@]}"; do
  f="$HOME/$SONGS/$s.wav"
  one="$OUT/in-$s"; mkdir -p "$one"; ln -sf "$f" "$one/"
  nice -n 10 "$BIN" --in "$one" --out "$OUT/$s" --metrics-only "$@" 2> "$OUT/$s.stderr.log" &
  pids+=($!)
done
st=0
for p in "${pids[@]}"; do wait "$p" || st=1; done
rm -rf "$OUT"/in-*
exit $st
EOS
  mkdir -p "$OUT_LOCAL"
  rsync -rt -e "${SSH[*]}" "$HOST:$BASE/$RUN/" "$OUT_LOCAL/"
  "${SSH[@]}" "$HOST" "rm -rf $(printf '%q' "$BASE/$RUN")"
  # 合并各歌 metrics.csv（表头只留一份）
  first=1
  : > "$OUT_LOCAL/all.csv"
  for m in "$OUT_LOCAL"/*/metrics.csv; do
    if [ $first = 1 ]; then cat "$m" >> "$OUT_LOCAL/all.csv"; first=0; else tail -n +2 "$m" >> "$OUT_LOCAL/all.csv"; fi
  done
  python3 "$WT/tools/render/gate_check.py" "$OUT_LOCAL/all.csv" > "$OUT_LOCAL/check.txt" || true
  cat "$OUT_LOCAL/check.txt"
  ;;
*) echo "usage: build <commit|wt> | run <label> <local-out> <songs> [args]" >&2; exit 2 ;;
esac
