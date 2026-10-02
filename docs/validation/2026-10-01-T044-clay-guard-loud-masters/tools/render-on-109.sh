#!/usr/bin/env bash
# T-044 helper: sync worktree to 109, build celadon_render, run on ~/.cache/celadon-t044/<in> -> out-<tag>
# usage: r109.sh <tag> <indir-under-t044> [celadon_render args...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd -P)"
JUCE_DIR=/Users/maxsong/All-Project/JUCE
HOST=maxsong@192.168.100.109
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
TAG="$1"; IN="$2"; shift 2
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-render/$(basename "$ROOT")-$KEY"
source "$ROOT/scripts/paths.sh"
"${SSH[@]}" $HOST "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE'"
rsync -rlpD --checksum --delete --delete-excluded "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"
rsync -a --delete --delete-excluded --include '/LICENSE.md' --include '/modules/***' --exclude '*' -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"
ARGS="$(printf '%q ' "$@")"
"${SSH[@]}" $HOST "REMOTE=$REMOTE TAG=$TAG IN=$IN ARGS=$(printf '%q' "$ARGS") bash -euo pipefail -s" <<'R'
cd "$HOME/$REMOTE"
BUILD="$HOME/.cache/celadon-render-build/$(basename "$REMOTE")"
mkdir -p "$BUILD" "$HOME/.cache/celadon-locks"
exec 9>"$HOME/.cache/celadon-locks/render-$(basename "$REMOTE").lock"; flock 9
eval "set -- $ARGS"
nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
nice -n 10 cmake --build "$BUILD" --target celadon_render 2>&1 | tail -3
OUT="$HOME/.cache/celadon-t044/out-$TAG"; rm -rf "$OUT"
nice -n 10 "$BUILD/celadon_render" --in "$HOME/.cache/celadon-t044/$IN" --out "$OUT" "$@" 2>&1 | tail -5
R
