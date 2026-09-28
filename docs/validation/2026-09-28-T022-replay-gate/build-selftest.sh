#!/usr/bin/env bash
# T-022：同步工作树到 109，编 celadon_render（统计警告），跑 --selftest。
set -euo pipefail
ROOT=/Users/maxsong/All-Project/Celadon
JUCE_DIR=/Users/maxsong/All-Project/JUCE
HOST=maxsong@192.168.100.109
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-render/$(basename "$ROOT")-$KEY"
"${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE'"
rsync -rlpD --checksum --delete --delete-excluded \
  --exclude 'Builds/' --exclude 'JuceLibraryCode/' --exclude 'build/' --exclude 'artifacts/' --exclude 'docs/' \
  --exclude '.DS_Store' --exclude '.claude/' -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"
rsync -a --delete --delete-excluded --include '/LICENSE.md' --include '/modules/***' --exclude '*' \
  -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"
"${SSH[@]}" "$HOST" "REMOTE=$(printf '%q' "$REMOTE") bash -euo pipefail -s" <<'EOS'
cd "$HOME/$REMOTE"
BUILD="$HOME/.cache/celadon-render-build/$(basename "$REMOTE")"
mkdir -p "$BUILD" "$HOME/.cache/celadon-locks"
exec 9>"$HOME/.cache/celadon-locks/render-$(basename "$REMOTE").lock"
flock 9
nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
LOG=$(mktemp)
if ! nice -n 10 cmake --build "$BUILD" --target celadon_render > "$LOG" 2>&1; then cat "$LOG"; rm -f "$LOG"; exit 1; fi
echo "编译警告行数：$(grep -ciE 'warning' "$LOG" || true)"; grep -iE -A3 'warning' "$LOG" | head -40 || true; rm -f "$LOG"
nice -n 10 "$BUILD/celadon_render" --selftest
EOS
