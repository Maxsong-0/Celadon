#!/usr/bin/env bash
# T-072 新检查 probeTimerPush 的红绿验证（DGX Spark 本机）：
#   新   = 改后的编辑器（PluginEditor.{h,cpp} 带 snapshotDriven）+ 新检查            → --only check 应全过（112 / 0）
#   改坏 = 同一份截图工具 + main 的 PluginEditor.{h,cpp}（没有 snapshotDriven，= 改前）→ 应恰好败这一条（111 / 1）
# 用法：run_t072_check.sh <输出目录> <改后源码 tar（含新检查）> <改前源码 tar（main）>
set -u
OUT=$1; NEW_TAR=$2; OLD_TAR=$3
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRCROOT=$HOME/.cache/celadon-spark/src
mkdir -p "$OUT"

build_tree() {   # build_tree <标签> <源码 tar> <是否换回 main 的 PluginEditor 1|0>
  local tag=$1 tarball=$2 revert=$3 src=$SRCROOT/t072-$1
  rm -rf "$src"; mkdir -p "$src"
  tar -x -C "$src" -f "$tarball"
  if [ "$revert" = 1 ]; then
    tar -x -C "$src" -f "$OLD_TAR" Source/PluginEditor.h Source/PluginEditor.cpp
  fi
  SPARK_JOBS=${SPARK_JOBS:-8} "$SPARK" "$src" build celadon_ui_snapshot > "$OUT/$tag.build.txt" 2>&1 || { echo "!! $tag 编译失败，见 $OUT/$tag.build.txt"; return 1; }
  echo "$tag 编好（换回 main 的 PluginEditor：$revert，警告 $(grep -c 'warning:' "$OUT/$tag.build.txt")）"
}

build_tree chk-new "$NEW_TAR" 0 || exit 1
build_tree chk-old "$NEW_TAR" 1 || exit 1
BIN_NEW=$("$SPARK" "$SRCROOT/t072-chk-new" dir)
BIN_OLD=$("$SPARK" "$SRCROOT/t072-chk-old" dir)

cat > "$OUT/check-runs.sh" <<RUNS
#!/usr/bin/env bash
set -u
OUT="$OUT"
for pair in "new:$BIN_NEW" "old:$BIN_OLD"; do
  tag=\${pair%%:*}; bin=\${pair#*:}
  home="\$OUT/home-\$tag"; mkdir -p "\$home/.config"
  HOME="\$home" XDG_CONFIG_HOME="\$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "\$bin/celadon_ui_snapshot" --only check > "\$OUT/check-\$tag.txt" 2>&1
  echo "\$tag 退出码 \$? PASS=\$(grep -c '^PASS' "\$OUT/check-\$tag.txt") FAIL=\$(grep -c '^FAIL' "\$OUT/check-\$tag.txt")"
  grep '^FAIL' "\$OUT/check-\$tag.txt" | cut -c1-400
done
echo "完成 \$(date +%T)"
RUNS
chmod +x "$OUT/check-runs.sh"
"$SPARK" "$SRCROOT/t072-chk-new" run bash "$OUT/check-runs.sh" 2>&1 | tee "$OUT/check-runs.out"
