#!/usr/bin/env bash
# T-070 红绿验证：建 V0 + T-058 的 11 个变体 + T-070 的 15 个变体共 27 个截图工具二进制。编译不进全机锁；--only check 另由
# ../../2026-10-04-T058-spectrum-peak-off/scripts/redgreen_run.sh 在锁里一次跑完。
# 用法：redgreen_build_t070.sh <源码快照目录（git archive 解出来的 T-070 提交）> <输出目录>
# 只补建某几个变体：ONLY="N15" redgreen_build_t070.sh <源码快照目录> <另一个输出目录>（不设 = 全建）。
# 做法：每个变体先把 SpectrumView.cpp 还原成原样、再用 variants_t058.py / variants_t070.py 改成「改坏」的样子（脚本先断言源码与预期文字逐字对上）、
# 增量编译 celadon_ui_snapshot、把二进制拷到 <输出目录>/bin/<名字>。快照树自己一个构建目录，一直增量。
set -eu
SRC=$1; OUT=$2
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V058="$HERE/../../2026-10-04-T058-spectrum-peak-off/scripts/variants_t058.py"
V070="$HERE/variants_t070.py"
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
mkdir -p "$OUT/bin" "$OUT/logs" "$OUT/pristine"
cp "$SRC/Source/ui/SpectrumView.cpp" "$OUT/pristine/SpectrumView.cpp"

build_variant() {   # build_variant <名字> <脚本 058|070|none> <改法>
  local name=$1 which=$2 spec=$3
  if [ -n "${ONLY:-}" ] && [[ " $ONLY " != *" $name "* ]]; then return 0; fi
  cp "$OUT/pristine/SpectrumView.cpp" "$SRC/Source/ui/SpectrumView.cpp"
  if [ "$which" != none ]; then
    local script=$V058; [ "$which" = 070 ] && script=$V070
    python3 "$script" "$SRC/Source/ui/SpectrumView.cpp" "$spec" > "$OUT/logs/$name.apply.txt" 2>&1 || { echo "!! $name：改法没套上，见 $OUT/logs/$name.apply.txt"; return 1; }
  fi
  SPARK_JOBS=${SPARK_JOBS:-8} "$SPARK" "$SRC" build celadon_ui_snapshot > "$OUT/logs/$name.build.txt" 2>&1 || { echo "!! $name：编译失败，见 $OUT/logs/$name.build.txt"; return 1; }
  local b; b=$("$SPARK" "$SRC" dir)
  cp "$b/celadon_ui_snapshot" "$OUT/bin/$name"
  echo "$name 编好（警告 $(grep -c 'warning:' "$OUT/logs/$name.build.txt")，sha256 $(sha256sum "$OUT/bin/$name" | cut -c1-12)）"
}

build_variant V0  none none
build_variant V1  058 paint-only
build_variant V2  058 region-only
build_variant V3  058 region-only+no-force-rebuild
build_variant V4  058 no-force-rebuild
build_variant V5  058 no-peak-copy
build_variant V6  058 compare-peak-always
build_variant V7  058 no-latest
build_variant V8  058 no-old-area
build_variant V9  058 no-peak-region
build_variant V10 058 no-stroke-margin
build_variant V11 058 hover-drawn
build_variant N1  070 no-repaint-on-off
build_variant N2  070 no-copy-output
build_variant N3  070 no-copy-average
build_variant N4  070 no-copy-peak
build_variant N5  070 no-copy-input
build_variant N6  070 no-build-peak
build_variant N7  070 no-compare-average
build_variant N8  070 no-compare-output
build_variant N9  070 no-dashes-invalidate
build_variant N10 070 no-tilt-invalidate
build_variant N11 070 differs-no-tilt
build_variant N12 070 hover-peak
build_variant N13 070 hover-average
build_variant N14 070 rebuild-every-frame
build_variant N15 070 dot-latest
cp "$OUT/pristine/SpectrumView.cpp" "$SRC/Source/ui/SpectrumView.cpp"
echo "全部变体编完 $(date '+%F %T')"
