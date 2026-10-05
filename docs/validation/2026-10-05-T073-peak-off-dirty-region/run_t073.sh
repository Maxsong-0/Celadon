#!/usr/bin/env bash
# T-073 验证（DGX Spark 本机）：PEAK 关着时看不见的峰值线不进脏区（T-070 评审 SV-2）。
# 用法：run_t073.sh <输出目录> <改后源码 tar> <改前源码 tar（main）>
#   两个 tar 先在工作树里用 `git archive <提交> --format=tar -o <tar>` 取好（本脚本不调 git）。
# 做法：
#   1. 六份源码快照各编一份 celadon_ui_snapshot（0 警告）：
#        new          改后                      base         改前（main）
#        always       改后 + 峰值线不论开关都算进脏区（= T-073 之前，variants_t073.py always-peak）
#        never        改后 + 峰值线永远不算进脏区（收过头，variants_t073.py never-peak）
#        new-music / base-music  改后 / 改前各套上 T-058 的 probe_music_dirty.py（--only spectrum-peak-bench 换成「放音乐 8 s」一个场景）
#   2. 全机锁里串行跑 runs_t073.sh：--only check（new / always / never）、--deterministic 出图逐字节比（base vs new）、
#      标准基准与音乐基准（base vs new，绑 CPU 5；帧数 / 标脏帧 / 曲线帧 / 设备像素每次运行都相同，CPU 只作参考）。
set -u
OUT=$1; NEW_TAR=$2; BASE_TAR=$3
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRCROOT=$HOME/.cache/celadon-spark/src
mkdir -p "$OUT"

build_tree() {   # build_tree <标签> <源码 tar> <改法 none|always-peak|never-peak> <音乐探针 0|1>
  local tag=$1 tarball=$2 variant=$3 music=$4 src=$SRCROOT/t073-$1
  rm -rf "$src"; mkdir -p "$src"
  tar -x -C "$src" -f "$tarball"
  if [ "$variant" != none ]; then
    python3 "$HERE/variants_t073.py" "$src/Source/ui/SpectrumView.cpp" "$variant" > "$OUT/$tag.variant.txt" 2>&1 || { echo "!! $tag：改法没套上，见 $OUT/$tag.variant.txt"; return 1; }
  fi
  if [ "$music" = 1 ]; then
    python3 "$src/docs/validation/2026-10-04-T058-spectrum-peak-off/scripts/probe_music_dirty.py" "$src" > "$OUT/$tag.music-patch.txt" 2>&1 || { echo "!! $tag：音乐探针没套上，见 $OUT/$tag.music-patch.txt"; return 1; }
  fi
  SPARK_JOBS=${SPARK_JOBS:-8} "$SPARK" "$src" build celadon_ui_snapshot > "$OUT/$tag.build.txt" 2>&1 || { echo "!! $tag 编译失败，见 $OUT/$tag.build.txt"; return 1; }
  echo "$tag 编好（改法 $variant，音乐探针 $music，警告 $(grep -c 'warning:' "$OUT/$tag.build.txt")）"
}

build_tree new        "$NEW_TAR"  none        0 || exit 1
build_tree base       "$BASE_TAR" none        0 || exit 1
build_tree always     "$NEW_TAR"  always-peak 0 || exit 1
build_tree never      "$NEW_TAR"  never-peak  0 || exit 1
build_tree new-music  "$NEW_TAR"  none        1 || exit 1
build_tree base-music "$BASE_TAR" none        1 || exit 1

bin() { "$SPARK" "$SRCROOT/t073-$1" dir; }
"$SPARK" "$SRCROOT/t073-new" run bash "$HERE/runs_t073.sh" "$OUT" \
  "$(bin new)" "$(bin base)" "$(bin always)" "$(bin never)" "$(bin new-music)" "$(bin base-music)" \
  "$SRCROOT/t073-new/docs/validation/2026-10-04-T058-spectrum-peak-off/scripts/bench_compare.py" 2>&1 | tee "$OUT/runs.out"
