#!/usr/bin/env bash
# T-074：真歌渲染第二轮（第一轮的三首歌里修坯从没动过：低切全程 0，守护没动作，证明不了什么）。
#   阶段 1：Jamendo 45 首 × 改前 / 改后，--glaze 70 --clay auto,lowcut --no-replay --metrics-only --trace：metrics.csv 与逐块 trace 逐字节比；
#   阶段 2：从改后的 trace 里挑出守护真动作过的歌（peak_guard_samples > 0），改前 / 改后各再渲染一遍出 wav，逐文件 sha256 比。
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
W=/home/maxsong/All-Project/Celadon/.claude/worktrees/complete-remaining-tasks-cdaccd
P=$OUT/progress10.txt; : > $P
BASEBUILD=$("$SPARK" $SRC/t074-base dir)
NEWBUILD=$("$SPARK" "$W" dir)

rm -rf $OUT/songs2 $OUT/render2-base $OUT/render2-new $OUT/render2-jobs.txt
mkdir -p $OUT/songs2
for f in ~/.cache/celadon-t048-cal/*/track_*/track_*.wav; do
  n=$(basename "$f" .wav); mkdir -p $OUT/songs2/$n; ln "$f" $OUT/songs2/$n/
done
echo "songs=$(ls $OUT/songs2 | wc -l) $(date +%T)" >> $P

for tree in base new; do for n in $(ls $OUT/songs2); do echo "$tree $n" >> $OUT/render2-jobs.txt; done; done

job() {   # job <tree> <song>：阶段 1 的一个渲染
  local tree=$1 n=$2 bin
  if [ "$tree" = base ]; then bin=$BASEBUILD/celadon_render; else bin=$NEWBUILD/celadon_render; fi
  mkdir -p $OUT/render2-$tree/$n
  nice -n 5 $bin --in $OUT/songs2/$n --out $OUT/render2-$tree/$n --glaze 70 --clay auto,lowcut --no-replay --metrics-only --trace > $OUT/render2-$tree/$n.log 2>&1
}
export -f job
export OUT BASEBUILD NEWBUILD

"$SPARK" "$W" run bash -c "cat $OUT/render2-jobs.txt | xargs -P 10 -L 1 bash -c 'job \$0 \$1'"
echo "stage 1 rendered exit=$? $(date +%T)" >> $P

same=0; diff=0; missing=0; list=""
for n in $(ls $OUT/songs2); do
  for f in $(cd $OUT/render2-base/$n && find . -type f -name '*.csv' | sort); do
    if [ ! -f "$OUT/render2-new/$n/$f" ]; then missing=$((missing+1)); list="$list $n/$f(missing)"; continue; fi
    if cmp -s "$OUT/render2-base/$n/$f" "$OUT/render2-new/$n/$f"; then same=$((same+1)); else diff=$((diff+1)); list="$list $n/$f"; fi
  done
done
echo "stage 1 compare: csv files identical=$same different=$diff missing=$missing${list:+: $list}" >> $P

# 阶段 2：守护动作过的歌
python3 - $OUT <<'PY' >> $P
import csv, glob, os, sys
out = sys.argv[1]
active = []
rows_total = low_total = guard_total = 0
for d in sorted(glob.glob(f"{out}/render2-new/track_*")):
    if not os.path.isdir(d):
        continue
    name = os.path.basename(d)
    rows = low = gb = 0
    gs = 0
    for path in glob.glob(f"{d}/trace/*.csv"):
        if path.endswith("__listen.csv"):
            continue
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                rows += 1
                try:
                    if float(r["clay_lowcut_hz"]) > 0:
                        low += 1
                    n = int(float(r["peak_guard_samples"]))
                    if n > 0:
                        gb += 1
                        gs += n
                except Exception:
                    pass
    rows_total += rows; low_total += low; guard_total += gb
    if gb > 0:
        active.append((name, rows, low, gb, gs))
print(f"stage 1 activity: blocks {rows_total}, low cut on {low_total}, guard acted {guard_total}; songs with guard action: {len(active)}")
for a in active:
    print(f"  active {a[0]} blocks {a[1]} lowcut>0 {a[2]} guard-blocks {a[3]} guard-samples {a[4]}")
with open(f"{out}/render2-active.txt", "w") as f:
    for a in active:
        f.write(a[0] + "\n")
PY

rm -rf $OUT/render3-base $OUT/render3-new $OUT/render3-jobs.txt
for tree in base new; do for n in $(cat $OUT/render2-active.txt); do echo "$tree $n" >> $OUT/render3-jobs.txt; done; done
if [ -s $OUT/render3-jobs.txt ]; then
  job3() {
    local tree=$1 n=$2 bin
    if [ "$tree" = base ]; then bin=$BASEBUILD/celadon_render; else bin=$NEWBUILD/celadon_render; fi
    mkdir -p $OUT/render3-$tree/$n
    nice -n 5 $bin --in $OUT/songs2/$n --out $OUT/render3-$tree/$n --glaze 70 --clay auto,lowcut --no-replay > $OUT/render3-$tree/$n.log 2>&1
  }
  export -f job3
  "$SPARK" "$W" run bash -c "cat $OUT/render3-jobs.txt | xargs -P 10 -L 1 bash -c 'job3 \$0 \$1'"
  echo "stage 2 rendered exit=$? $(date +%T)" >> $P
  same=0; diff=0; missing=0; list=""
  for n in $(cat $OUT/render2-active.txt); do
    for f in $(cd $OUT/render3-base/$n && find . -type f -name '*.wav' | sort); do
      if [ ! -f "$OUT/render3-new/$n/$f" ]; then missing=$((missing+1)); list="$list $n/$f(missing)"; continue; fi
      if cmp -s "$OUT/render3-base/$n/$f" "$OUT/render3-new/$n/$f"; then same=$((same+1)); else diff=$((diff+1)); list="$list $n/$f"; fi
    done
  done
  echo "stage 2 compare: wav files identical=$same different=$diff missing=$missing${list:+: $list}" >> $P
  ( cd $OUT/render3-new && find . -name '*.wav' | sort | xargs sha256sum | cut -c1-16,65- > $OUT/render3-new.sha256 )
  ( cd $OUT/render3-base && find . -name '*.wav' | sort | xargs sha256sum | cut -c1-16,65- > $OUT/render3-base.sha256 )
  cmp -s $OUT/render3-base.sha256 $OUT/render3-new.sha256 && echo "stage 2 sha256 lists identical ($(wc -l < $OUT/render3-new.sha256) wavs)" >> $P || echo "stage 2 sha256 lists DIFFER" >> $P
fi
echo done >> $P
