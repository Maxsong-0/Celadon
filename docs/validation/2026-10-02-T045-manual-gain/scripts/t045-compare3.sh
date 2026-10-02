#!/usr/bin/env bash
set -euo pipefail
B=$HOME/.cache/celadon-t045-diag
CAL=$HOME/.cache/celadon-t038-cal
python3 - "$CAL/manifest.tsv" > "$B/cmp3-jobs.txt" <<'PY'
import csv, sys
for r in csv.DictReader(open(sys.argv[1]), delimiter='\t'):
    if float(r['fold_peak']) < 5.0 or not (100.5 <= float(r['bpm']) <= 139.5):
        continue
    for style in ('warm', 'punchy', 'clean'):
        print('new3', r['class'], r['track'], r['bpm'], style, 'general', 0)
    for gain in (2, 4, 6, 8, 10):
        print('new3', r['class'], r['track'], r['bpm'], 'auto', r['class'], gain)
PY
wc -l < "$B/cmp3-jobs.txt"
xargs -P 12 -L 1 bash -c 'out="'"$B"'/cmp/$0-$2-$4-$5-g$6"; rm -rf "$out"; mkdir -p "$out"; CELADON_DIAG_LIMIT_GAIN=$6 nice -n 5 "'"$B"'/src-$0/build/celadon_render" --in "'"$CAL"'/$1/$2" --out "$out" --glaze 100 --style "$4" --clay auto --genre "$5" --delivery standard --bpm "$3" --metrics-only --no-replay > "$out.log" 2>&1 || echo "FAIL $*"' < "$B/cmp3-jobs.txt"
echo done
