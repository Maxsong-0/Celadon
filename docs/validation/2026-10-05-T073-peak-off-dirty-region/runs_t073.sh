#!/usr/bin/env bash
# run_t073.sh 在全机锁里调的运行段（不单独用）。
# 用法：runs_t073.sh <输出目录> <new> <base> <always> <never> <new-music> <base-music> <bench_compare.py>（后六个是各构建目录）
set -u
OUT=$1; NEW=$2; BASE=$3; ALWAYS=$4; NEVER=$5; NEW_MUSIC=$6; BASE_MUSIC=$7; COMPARE=$8
mkdir -p "$OUT/cwd"
cd "$OUT/cwd" || exit 1   # --only check 会往当前目录写 check-*.png

run_tool() {   # run_tool <标签> <构建目录> <参数 ...>
  local tag=$1 bin=$2; shift 2
  local home="$OUT/home-$tag"; mkdir -p "$home/.config"
  HOME="$home" XDG_CONFIG_HOME="$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "$bin/celadon_ui_snapshot" "$@"
}

echo "================ --only check ================"
for pair in "new:$NEW" "always:$ALWAYS" "never:$NEVER"; do
  tag=${pair%%:*}; bin=${pair#*:}
  run_tool "check-$tag" "$bin" --only check > "$OUT/check-$tag.txt" 2>&1
  echo "$tag 退出码 $? PASS=$(grep -c '^PASS' "$OUT/check-$tag.txt") FAIL=$(grep -c '^FAIL' "$OUT/check-$tag.txt")"
  grep '^FAIL' "$OUT/check-$tag.txt" | cut -c1-330
done

echo "================ --deterministic 出图（改前 vs 改后，应逐字节相同）================"
for pair in "det-base:$BASE" "det-new:$NEW"; do
  tag=${pair%%:*}; bin=${pair#*:}
  mkdir -p "$OUT/$tag"
  run_tool "$tag" "$bin" --out "$OUT/$tag" --deterministic > "$OUT/$tag.log" 2>&1
  echo "$tag 退出码 $? 图 $(ls "$OUT/$tag"/*.png 2>/dev/null | wc -l) 张"
done
same=0; diff=0; list=""
for f in "$OUT/det-base"/*.png; do
  b=$(basename "$f")
  if cmp -s "$f" "$OUT/det-new/$b"; then same=$((same+1)); else diff=$((diff+1)); list="$list $b"; fi
done
echo "改前 vs 改后：相同 $same 张，不同 $diff 张${list:+：$list}"

echo "================ --only spectrum-peak-bench（标准 12 组 + 音乐 8 组，绑 CPU 5）================"
for pair in "bench-base:$BASE" "bench-new:$NEW" "music-base:$BASE_MUSIC" "music-new:$NEW_MUSIC"; do
  tag=${pair%%:*}; bin=${pair#*:}
  taskset -c 5 bash -c "$(declare -f run_tool); OUT='$OUT'; run_tool '$tag' '$bin' --only spectrum-peak-bench" > "$OUT/$tag.txt" 2>&1
  echo "$tag 退出码 $? $(grep -c 'PEAK' "$OUT/$tag.txt") 行"
done
echo "---- 标准基准：改前 → 改后 ----"
python3 "$COMPARE" "$OUT/bench-base.txt" "$OUT/bench-new.txt"
echo "---- 音乐基准：改前 → 改后（PEAK 关各组合的设备像素与 CPU）----"
python3 - "$OUT/music-base.txt" "$OUT/music-new.txt" <<'PY'
import re, sys
def parse(path):
    rows, on = {}, False
    for ln in open(path, encoding="utf-8"):
        if ln.startswith("==== summary"):
            on = True; continue
        if ln.startswith("==== spectrum peak-off bench done"): break
        if not on: continue
        m = re.match(r"\s*(.+?)\s*\|\s*PEAK (off|on)\s*\|\s*(\d+) fps\s*\|\s*x(\d)\s*\|\s*fr\s+(\d+) dirty\s+(\d+) curves\s+(\d+)\s*\|\s*dev px\s+(\d+)\s*\|.*?paint wall\s+[\d.]+ cpu\s+([\d.]+)", ln)
        if m:
            lab, pk, fps, sc, fr, dirty, curves, dev, pcpu = m.groups()
            rows[(pk, int(fps), int(sc))] = (int(fr), int(dirty), int(curves), int(dev), float(pcpu))
    return rows
b, a = parse(sys.argv[1]), parse(sys.argv[2])
print(f"{'PEAK':5s}{'fps':>4s}{'倍':>3s} | {'标脏帧 前→后':>14s} | {'曲线帧 前→后':>14s} | {'设备像素 前 → 后':>30s} | paint CPU ms 前 → 后")
for k in sorted(b):
    if k not in a: continue
    x, y = b[k], a[k]
    print(f"{k[0]:5s}{k[1]:4d}{k[2]:3d} | {x[1]:6d} → {y[1]:<5d} | {x[2]:6d} → {y[2]:<5d} | {x[3]:12d} → {y[3]:<12d} ({100*(y[3]-x[3])/x[3]:+5.1f} %) | {x[4]:8.1f} → {y[4]:<8.1f}")
PY
echo "完成 $(date +%T)"
