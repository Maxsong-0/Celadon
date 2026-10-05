#!/usr/bin/env bash
# T-058 红绿验证第 3 轮（最终源码）：建 16 个变体的截图工具二进制。编译不进全机锁；--only check 另由 redgreen_run.sh 在锁里一次跑完。
# 用法：redgreen_build.sh <variants_t058.py> <T-058 源码快照> <main + 新工具源码快照> <输出目录>
# 做法：每个变体先把 SpectrumView.cpp 还原成原样、再用 variants_t058.py 改成「撤掉一条」的样子（脚本先断言源码与预期文字逐字对上）、
# 增量编译 celadon_ui_snapshot、把二进制拷到 <输出目录>/bin/<名字>。两棵树各用自己的构建目录，一直增量。
set -eu
VARIANTS=$1; T058=$2; MAINTREE=$3; OUT=$4
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
mkdir -p "$OUT/bin" "$OUT/logs" "$OUT/pristine"
cp "$T058/Source/ui/SpectrumView.cpp" "$OUT/pristine/t058.SpectrumView.cpp"
cp "$MAINTREE/Source/ui/SpectrumView.cpp" "$OUT/pristine/main.SpectrumView.cpp"

build_variant() {   # build_variant <树名 t058|main> <变体名> <改法 或 none>
  local tree=$1 name=$2 spec=$3 src
  if [ "$tree" = t058 ]; then src=$T058; else src=$MAINTREE; fi
  cp "$OUT/pristine/$tree.SpectrumView.cpp" "$src/Source/ui/SpectrumView.cpp"
  if [ "$spec" != none ]; then
    python3 "$VARIANTS" "$src/Source/ui/SpectrumView.cpp" "$spec" > "$OUT/logs/$name.apply.txt" 2>&1 || { echo "!! $name：改法没套上，见 $OUT/logs/$name.apply.txt"; return 1; }
  fi
  SPARK_JOBS=4 "$SPARK" "$src" build celadon_ui_snapshot > "$OUT/logs/$name.build.txt" 2>&1 || { echo "!! $name：编译失败，见 $OUT/logs/$name.build.txt"; return 1; }
  local b; b=$("$SPARK" "$src" dir)
  cp "$b/celadon_ui_snapshot" "$OUT/bin/$name"
  echo "$name 编好（警告 $(grep -c 'warning:' "$OUT/logs/$name.build.txt")，sha256 $(sha256sum "$OUT/bin/$name" | cut -c1-12)）"
}

build_variant t058 V0  none
build_variant t058 V1  paint-only
build_variant t058 V2  region-only
build_variant t058 V3  region-only+no-force-rebuild
build_variant t058 V4  no-force-rebuild
build_variant t058 V5  no-peak-copy
build_variant t058 V6  compare-peak-always
build_variant t058 V7  no-latest
build_variant t058 V8  no-old-area
build_variant t058 V9  no-peak-region
build_variant t058 V10 no-stroke-margin
build_variant t058 V11 hover-drawn
build_variant main M0  none
build_variant main M1  region-only
build_variant main M2  paint-only
build_variant main M3  paint-only+region-only
# 还原两棵树的 SpectrumView.cpp
cp "$OUT/pristine/t058.SpectrumView.cpp" "$T058/Source/ui/SpectrumView.cpp"
cp "$OUT/pristine/main.SpectrumView.cpp" "$MAINTREE/Source/ui/SpectrumView.cpp"
echo "全部变体编完 $(date '+%F %T')"
