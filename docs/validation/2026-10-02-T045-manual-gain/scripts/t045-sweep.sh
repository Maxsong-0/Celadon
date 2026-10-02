#!/usr/bin/env bash
set -euo pipefail
B=$HOME/.cache/celadon-t045-diag
CAL=$HOME/.cache/celadon-t038-cal
BIN=$B/build/celadon_render
rm -rf "$B/out" && mkdir -p "$B/out"
python3 - "$CAL/manifest.tsv" > "$B/jobs.txt" <<'PY'
import csv, sys
for r in csv.DictReader(open(sys.argv[1]), delimiter='\t'):
    if float(r['fold_peak']) < 5.0 or not (100.5 <= float(r['bpm']) <= 139.5):
        continue
    for gain in (0, 1, 2, 3, 4, 6, 8):
        for genre in ('general', r['class']):
            print(r['class'], r['track'], r['bpm'], gain, genre)
PY
wc -l < "$B/jobs.txt"
xargs -P 12 -L 1 bash -c 'out="'"$B"'/out/$1-g$3-$4"; mkdir -p "$out"; CELADON_DIAG_LIMIT_GAIN=$3 nice -n 5 "'"$BIN"'" --in "'"$CAL"'/$0/$1" --out "$out" --glaze 70 --style auto --clay auto --genre "$4" --delivery standard --bpm "$2" --metrics-only --no-replay > "$out.log" 2>&1 || echo "FAIL $1 $3 $4"' < "$B/jobs.txt"
echo done
