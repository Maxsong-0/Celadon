#!/usr/bin/env bash
# T-042 渲染回路（照 docs/validation/2026-09-27-T015-calibrating/calib-loop.sh）：
#   build <标签>                     同步本工作树到 109、编 celadon_render、自检，二进制存成 ~/.cache/celadon-t042/bin/celadon_render.<标签>
#   run <标签> <远端输入目录> <本地输出目录> [celadon_render 额外参数…]
#                                    用该标签的二进制，输入目录第一层每首歌一个进程并行（--metrics-only），拉回本机后删远端输出
set -euo pipefail
ROOT=/Users/maxsong/All-Project/Celadon/.claude/worktrees/wizardly-sanderson-9a859f
JUCE_DIR=/Users/maxsong/All-Project/JUCE
HOST=maxsong@192.168.100.109
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-render/$(basename "$ROOT")-$KEY"
source "$ROOT/scripts/paths.sh"

cmd="$1"; shift
case "$cmd" in
build)
  LABEL="$1"
  "${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE' .cache/celadon-t042/bin"
  rsync -rlpD --checksum --delete --delete-excluded "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" \
    -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"
  rsync -a --delete --delete-excluded --include '/LICENSE.md' --include '/modules/***' --exclude '*' \
    -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"
  "${SSH[@]}" "$HOST" "REMOTE=$(printf '%q' "$REMOTE") LABEL=$(printf '%q' "$LABEL") bash -euo pipefail -s" <<'EOS'
cd "$HOME/$REMOTE"
BUILD="$HOME/.cache/celadon-render-build/$(basename "$REMOTE")"
mkdir -p "$BUILD"
nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
nice -n 10 cmake --build "$BUILD" --target celadon_render 2>&1 | grep -i -E 'warning|error|Linking' | tail -20 || true
nice -n 10 "$BUILD/celadon_render" --selftest 2>&1 | tail -3
cp "$BUILD/celadon_render" "$HOME/.cache/celadon-t042/bin/celadon_render.$LABEL"
echo "built → ~/.cache/celadon-t042/bin/celadon_render.$LABEL"
EOS
  ;;
run)
  LABEL="$1"; IN="$2"; OUT_LOCAL="$3"; shift 3
  ARGS=""
  if [ "$#" -gt 0 ]; then ARGS="$(printf '%q ' "$@")"; fi
  RUN="out-$LABEL-$(date +%H%M%S)-$$"
  "${SSH[@]}" "$HOST" "LABEL=$(printf '%q' "$LABEL") IN=$(printf '%q' "$IN") RUN=$(printf '%q' "$RUN") ARGS=$(printf '%q' "$ARGS") bash -euo pipefail -s" <<'EOS'
BIN="$HOME/.cache/celadon-t042/bin/celadon_render.$LABEL"
OUT="$HOME/.cache/celadon-t042/$RUN"
mkdir -p "$OUT"
eval "set -- $ARGS"
pids=()
shopt -s nullglob
for f in "$IN"/*.wav "$IN"/*.flac "$IN"/*.aif "$IN"/*.aiff; do
  s="$(basename "$f")"; s="${s%.*}"
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
  rsync -rt -e "${SSH[*]}" "$HOST:.cache/celadon-t042/$RUN/" "$OUT_LOCAL/"
  "${SSH[@]}" "$HOST" "rm -rf .cache/celadon-t042/$(printf '%q' "$RUN")"
  echo "done → $OUT_LOCAL"
  ;;
*) echo "usage: build <label> | run <label> <remote-in> <local-out> [args]" >&2; exit 2 ;;
esac
