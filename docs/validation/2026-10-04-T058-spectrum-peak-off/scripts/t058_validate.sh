#!/usr/bin/env bash
# T-058 最终态验证（在 spark-test.sh run 的全机锁里一次跑完）。
# 用法：t058_validate.sh <输出目录> <最终工具构建目录> <纯 main 工具构建目录> <main 源码 + 新工具构建目录>
# 产物：final-check-{1,2}.txt、base-check.txt、before-check.txt、det-diff.txt（逐字节比对结果）、bench-{after,before}.txt
# HOME / XDG_CONFIG_HOME 指向输出目录里的临时目录：截图工具会改写设置文件，不碰 ~/.config/Celadon。
set -u
OUT=$1; FINAL=$2; BASE=$3; BEFORE=$4
mkdir -p "$OUT"

run() {   # run <名字> <工具> <参数…>：每次独立的 HOME，虚拟屏 1280x1024
  local name=$1 tool=$2; shift 2
  local home="$OUT/home-$name"
  mkdir -p "$home/.config"
  HOME="$home" XDG_CONFIG_HOME="$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "$tool" "$@"
}

echo "== 最终工具 --only check ×2"
for i in 1 2; do
  run "final$i" "$FINAL/celadon_ui_snapshot" --only check > "$OUT/final-check-$i.txt" 2>&1
  echo "final check $i 退出码 $?  PASS=$(grep -c '^PASS' "$OUT/final-check-$i.txt") FAIL=$(grep -c '^FAIL' "$OUT/final-check-$i.txt")"
done

echo "== 纯 main 工具 --only check"
run base "$BASE/celadon_ui_snapshot" --only check > "$OUT/base-check.txt" 2>&1
echo "base check 退出码 $?  PASS=$(grep -c '^PASS' "$OUT/base-check.txt") FAIL=$(grep -c '^FAIL' "$OUT/base-check.txt")"

echo "== main 源码 + 新工具 --only check（红的那一半：S9 应败）"
run before "$BEFORE/celadon_ui_snapshot" --only check > "$OUT/before-check.txt" 2>&1
echo "before check 退出码 $?  PASS=$(grep -c '^PASS' "$OUT/before-check.txt") FAIL=$(grep -c '^FAIL' "$OUT/before-check.txt")"

echo "== --deterministic 出图（全部状态，1 倍），最终 vs 纯 main"
mkdir -p "$OUT/det-final" "$OUT/det-base"
run detfinal "$FINAL/celadon_ui_snapshot" --out "$OUT/det-final" --deterministic > "$OUT/det-final.log" 2>&1; echo "det final 退出码 $?"
run detbase "$BASE/celadon_ui_snapshot" --out "$OUT/det-base" --deterministic > "$OUT/det-base.log" 2>&1; echo "det base 退出码 $?"
{
  nf=0; nd=0
  for f in "$OUT"/det-base/*.png; do
    b=$(basename "$f")
    if cmp -s "$f" "$OUT/det-final/$b"; then nf=$((nf+1)); else nd=$((nd+1)); echo "DIFF $b"; fi
  done
  echo "逐字节相同 $nf 张，不同 $nd 张；final 共 $(ls "$OUT"/det-final/*.png | wc -l) 张，base 共 $(ls "$OUT"/det-base/*.png | wc -l) 张"
} | tee "$OUT/det-diff.txt"

echo "== 基准 --only spectrum-peak-bench（绑 CPU 5）：改后 / 改前交替各一次"
for round in 1 2; do
  run "benchA$round" taskset -c 5 "$FINAL/celadon_ui_snapshot" --only spectrum-peak-bench > "$OUT/bench-after-$round.txt" 2>&1
  run "benchB$round" taskset -c 5 "$BEFORE/celadon_ui_snapshot" --only spectrum-peak-bench > "$OUT/bench-before-$round.txt" 2>&1
done
echo "bench 完成"
echo "全部完成 $(date '+%F %T')"
