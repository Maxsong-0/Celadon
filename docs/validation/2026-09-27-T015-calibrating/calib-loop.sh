#!/usr/bin/env bash
# T-015 复现回路：同步工作树到 109，编 celadon_render，只对指定歌曲跑两遍流程（--trace --metrics-only），拉回结果。
# 用法：calib-loop.sh <输出本地目录> "<歌名1>|<歌名2>|..." [celadon_render 额外参数…]
set -euo pipefail
OUT_LOCAL="$1"; shift
SONGS="$1"; shift
ROOT=/Users/maxsong/All-Project/Celadon
JUCE_DIR=/Users/maxsong/All-Project/JUCE
HOST=maxsong@192.168.100.109
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-render/$(basename "$ROOT")-$KEY"
SONGS_SRC=".cache/celadon-render/songs-9a355b99857c"
RUN="t015-$(date +%H%M%S)-$$"
REMOTE_IN="$REMOTE/in-$RUN"
REMOTE_OUT="$REMOTE/out-$RUN"

"${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE'"
rsync -rlpD --checksum --delete --delete-excluded \
  --exclude 'Builds/' --exclude 'JuceLibraryCode/' --exclude 'build/' --exclude 'artifacts/' --exclude 'docs/' \
  --exclude '.DS_Store' --exclude '.claude/' \
  -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"
rsync -a --delete --delete-excluded --include '/LICENSE.md' --include '/modules/***' --exclude '*' \
  -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"

ARGS="$(printf '%q ' "$@")"
"${SSH[@]}" "$HOST" "REMOTE=$(printf '%q' "$REMOTE") SONGS_SRC=$(printf '%q' "$SONGS_SRC") IN=$(printf '%q' "$REMOTE_IN") OUT=$(printf '%q' "$REMOTE_OUT") TRACE=$(printf "%q" "${TRACE:-}") SONGS=$(printf '%q' "$SONGS") ARGS=$(printf '%q' "$ARGS") bash -euo pipefail -s" <<'EOS'
cd "$HOME/$REMOTE"
BUILD="$HOME/.cache/celadon-render-build/$(basename "$REMOTE")"
mkdir -p "$BUILD" "$HOME/.cache/celadon-locks" "$HOME/$IN"
exec 9>"$HOME/.cache/celadon-locks/render-$(basename "$REMOTE").lock"
flock 9
IFS='|' read -ra LIST <<< "$SONGS"
for s in "${LIST[@]}"; do ln -sf "$HOME/$SONGS_SRC/$s.wav" "$HOME/$IN/$s.wav"; done
nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
nice -n 10 cmake --build "$BUILD" --target celadon_render 2>&1 | tail -3
nice -n 10 "$BUILD/celadon_render" --selftest 2>&1 | tail -3
eval "set -- $ARGS"
# 每首一个进程并行
pids=()
for s in "${LIST[@]}"; do
  mkdir -p "$HOME/$IN/one-$s"
  ln -sf "$HOME/$SONGS_SRC/$s.wav" "$HOME/$IN/one-$s/$s.wav"
  nice -n 10 "$BUILD/celadon_render" --in "$HOME/$IN/one-$s" --out "$HOME/$OUT/$s" --metrics-only ${TRACE:+--trace} "$@" 2> "$HOME/$OUT.$s.log" &
  pids+=($!)
done
st=0
for p in "${pids[@]}"; do wait "$p" || st=1; done
mkdir -p "$HOME/$OUT"
for s in "${LIST[@]}"; do mv "$HOME/$OUT.$s.log" "$HOME/$OUT/$s/stderr.log" 2>/dev/null || true; done
rm -rf "$HOME/$IN"
exit $st
EOS
mkdir -p "$OUT_LOCAL"
rsync -rt -e "${SSH[*]}" "$HOST:$REMOTE_OUT/" "$OUT_LOCAL/"
"${SSH[@]}" "$HOST" "rm -rf $(printf '%q' "$REMOTE_OUT")"
echo "done → $OUT_LOCAL"
