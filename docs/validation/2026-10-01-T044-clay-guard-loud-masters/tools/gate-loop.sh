#!/usr/bin/env bash
# T-044 门槛回路（照 T-043 的 t043.sh）：
#   build <标签> <源码目录>   同步到 109 ~/.cache/celadon-t044/src-<标签>、编 celadon_render、自检 → bin/celadon_render.<标签>
#   run <标签> <本地输出目录> [celadon_render 额外参数…]
#                             22 首门槛歌 + 两首 Jamendo 参考曲，每首一个进程并行（--metrics-only），拉回、合并 metrics.csv、跑 gate_check
set -euo pipefail
JUCE_DIR=/Users/maxsong/All-Project/JUCE
HOST=maxsong@192.168.100.109
BASE=.cache/celadon-t044
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
WT=/Users/maxsong/All-Project/Celadon/.claude/worktrees/confident-pare-6d1304
source "$WT/scripts/paths.sh"
cmd="$1"; shift
case "$cmd" in
build)
  LABEL="$1"; SRC="$2"
  "${SSH[@]}" $HOST "mkdir -p '$BASE/src-$LABEL/Celadon' '$BASE/JUCE' '$BASE/bin' && ln -sfn ../JUCE '$BASE/src-$LABEL/JUCE'"
  rsync -rlpD --checksum --delete --delete-excluded "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" -e "${SSH[*]}" "$SRC"/ "$HOST:$BASE/src-$LABEL/Celadon/"
  rsync -a --delete --include '/LICENSE.md' --include '/modules/***' --exclude '*' -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$BASE/JUCE/"
  "${SSH[@]}" $HOST "LABEL=$LABEL BASE=$BASE bash -euo pipefail -s" <<'R'
cd "$HOME/$BASE/src-$LABEL"
B="$HOME/$BASE/build-$LABEL"; mkdir -p "$B"
nice -n 10 cmake -S Celadon -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
nice -n 10 cmake --build "$B" --target celadon_render 2>&1 | grep -E "error|warning" -A4 | head -20 || true
"$B/celadon_render" --selftest 2>&1 | tail -2
cp "$B/celadon_render" "$HOME/$BASE/bin/celadon_render.$LABEL"
R
  ;;
run)
  LABEL="$1"; OUT="$2"; shift 2
  ARGS=""   # 不带额外参数时不能用 printf：它会产出一个空字符串参数（T-042 踩坑）
  if [ "$#" -gt 0 ]; then ARGS="$(printf '%q ' "$@")"; fi
  "${SSH[@]}" $HOST "LABEL=$LABEL BASE=$BASE ARGS=$(printf '%q' "$ARGS") bash -euo pipefail -s" <<'R'
cd "$HOME/$BASE"
P="per-$LABEL"; rm -rf "$P" "out-$LABEL"; mkdir -p "$P" "out-$LABEL"
for f in "$HOME"/.cache/celadon-render/songs-9a355b99857c/*.wav "$HOME/$BASE"/in/*.wav; do
  n="$(basename "$f" .wav)"; mkdir -p "$P/$n"; ln -sfn "$f" "$P/$n/$(basename "$f")"
done
ls "$P" | xargs -P 12 -I{} sh -c "nice -n 10 \"$HOME/$BASE/bin/celadon_render.$LABEL\" --in \"$P/{}\" --out \"out-$LABEL/{}\" --metrics-only $ARGS > \"out-$LABEL/{}.log\" 2>&1 || echo \"FAIL {}\""
R
  mkdir -p "$OUT"
  rsync -rt -e "${SSH[*]}" "$HOST:$BASE/out-$LABEL/" "$OUT/"
  python3 - "$OUT" <<'PY'
import csv, glob, os, sys
out = sys.argv[1]; rows = []; header = None
for f in sorted(glob.glob(os.path.join(out, '*', 'metrics.csv'))):
    with open(f, newline='') as h:
        r = list(csv.reader(h))
        header = header or r[0]; rows += r[1:]
with open(os.path.join(out, 'all.csv'), 'w', newline='') as h:
    w = csv.writer(h); w.writerow(header); w.writerows(rows)
print(len(rows), 'rows')
PY
  python3 "$WT/tools/render/gate_check.py" "$OUT/all.csv" > "$OUT/gate.txt" 2>&1 && echo "gate: pass" || echo "gate: FAIL (see $OUT/gate.txt)"
  ;;
esac
