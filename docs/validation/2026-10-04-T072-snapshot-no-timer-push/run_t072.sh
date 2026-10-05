#!/usr/bin/env bash
# T-072 验证（DGX Spark 本机）：慢帧注入下，截图模式里定时器看门狗的补推改前会改出图、改后不会。
# 用法：run_t072.sh <输出目录> <改前源码 tar> <改后源码 tar> [停顿毫秒，默认 130] [每几次推频谱仪停一次，默认 40]
#   （--deterministic 每张图要推约 900 帧：每帧都停要好几个小时，所以每 40 次推里有一次耗时 130 ms——够让看门狗看到「垂直同步 ≥ 100 ms 没来」）
#   两个 tar 先在工作树里用 `git archive <提交> -o <tar>` 取好（本脚本不调 git）。
# 做法：
#   1. 三个源码快照各编一份 celadon_ui_snapshot：改前 + 注入停顿、改后 + 注入停顿（inject_stall.py）、改后原样（不注入）。
#   2. 在全机锁里（spark-test.sh run）各跑 --deterministic 出 30 张图，逐字节比：
#        R0 改前+注入、不设环境变量（= 改前的真实行为）   R1 改前+注入、设停顿（每帧 ≥ 100 ms）
#        R2 改后+注入、不设环境变量                       R3 改后+注入、设停顿
#        R5 改前原样（final-main 构建目录，不注入）        R6 改后原样（不注入）
#      期望：R1 ≠ R0（改前慢帧会被看门狗多推）；R2 = R0、R3 = R0（改后与快慢无关）；R6 = R5（改后正常行为不变）。
#   3. 改后原样的 --only check（应 111 / 0）。
set -u
OUT=$1; BASE_TAR=$2; FIX_TAR=$3; STALL=${4:-130}; EVERY=${5:-40}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRCROOT=$HOME/.cache/celadon-spark/src
mkdir -p "$OUT"

build_tree() {   # build_tree <标签> <源码 tar> <是否注入 1|0>
  local tag=$1 tarball=$2 inject=$3 src=$SRCROOT/t072-$1
  rm -rf "$src"; mkdir -p "$src"
  tar -x -C "$src" -f "$tarball"
  if [ "$inject" = 1 ]; then
    python3 "$HERE/inject_stall.py" "$src/Source/PluginEditor.cpp" > "$OUT/$tag.inject.txt" || return 1
  fi
  SPARK_JOBS=${SPARK_JOBS:-8} "$SPARK" "$src" build celadon_ui_snapshot > "$OUT/$tag.build.txt" 2>&1 || { echo "!! $tag 编译失败，见 $OUT/$tag.build.txt"; return 1; }
  echo "$tag 编好（源码 $(basename "$tarball")，注入 $inject，警告 $(grep -c 'warning:' "$OUT/$tag.build.txt")）"
}

build_tree base-inj  "$BASE_TAR" 1 || exit 1
build_tree fix-inj   "$FIX_TAR"  1 || exit 1
build_tree fix-clean "$FIX_TAR"  0 || exit 1

BIN_BASE_INJ=$("$SPARK" "$SRCROOT/t072-base-inj" dir)
BIN_FIX_INJ=$("$SPARK" "$SRCROOT/t072-fix-inj" dir)
BIN_FIX_CLEAN=$("$SPARK" "$SRCROOT/t072-fix-clean" dir)
BIN_BASE_CLEAN=$("$SPARK" "$SRCROOT/final-main" dir)   # 最终集成用的 main 构建目录（43d788b，不注入）
echo "二进制：$BIN_BASE_INJ $BIN_FIX_INJ $BIN_FIX_CLEAN $BIN_BASE_CLEAN" > "$OUT/binaries.txt"

# 以下在全机锁里串行跑（run_one 由 spark-test.sh run 调起）
cat > "$OUT/runs.sh" <<RUNS
#!/usr/bin/env bash
set -u
OUT="$OUT"; STALL=$STALL; EVERY=$EVERY
export CELADON_T072_STALL_EVERY=\$EVERY
run_det() {   # run_det <标签> <二进制目录> <停顿毫秒，空 = 不设>
  local tag=\$1 bin=\$2 stall=\$3 home="\$OUT/home-\$1"
  mkdir -p "\$home/.config" "\$OUT/\$tag"
  local s=\$(date +%s)
  if [ -n "\$stall" ]; then export CELADON_T072_STALL_MS=\$stall; else unset CELADON_T072_STALL_MS; fi
  HOME="\$home" XDG_CONFIG_HOME="\$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "\$bin/celadon_ui_snapshot" --out "\$OUT/\$tag" --deterministic > "\$OUT/\$tag.log" 2>&1
  echo "\$tag 退出码 \$? 用时 \$((\$(date +%s)-s)) s，图 \$(ls "\$OUT/\$tag"/*.png 2>/dev/null | wc -l) 张"
}
cmpdir() {   # cmpdir <说明> <目录 A> <目录 B>
  local n=0 d=0 list=""
  for f in "\$2"/*.png; do b=\$(basename "\$f"); if cmp -s "\$f" "\$3/\$b"; then n=\$((n+1)); else d=\$((d+1)); list="\$list \$b"; fi; done
  echo "\$1：相同 \$n 张，不同 \$d 张\${list:+：\$list}"
}
run_det r5-base-clean       "$BIN_BASE_CLEAN" ""
run_det r6-fix-clean        "$BIN_FIX_CLEAN"  ""
run_det r0-base-inj-nostall "$BIN_BASE_INJ"   ""
run_det r2-fix-inj-nostall  "$BIN_FIX_INJ"    ""
run_det r1-base-inj-stall   "$BIN_BASE_INJ"   "\$STALL"
run_det r3-fix-inj-stall    "$BIN_FIX_INJ"    "\$STALL"
echo "================ 逐字节对比 ================"
cmpdir "R6 改后原样 vs R5 改前原样（正常行为不变）" "\$OUT/r6-fix-clean" "\$OUT/r5-base-clean"
cmpdir "R0 改前注入不设停顿 vs R5 改前原样（注入本身无副作用）" "\$OUT/r0-base-inj-nostall" "\$OUT/r5-base-clean"
cmpdir "R2 改后注入不设停顿 vs R0" "\$OUT/r2-fix-inj-nostall" "\$OUT/r0-base-inj-nostall"
cmpdir "R1 改前 + 停顿 \${STALL} ms（每 \${EVERY} 次一回）vs R0（预期：不同）" "\$OUT/r1-base-inj-stall" "\$OUT/r0-base-inj-nostall"
cmpdir "R3 改后 + 停顿 \${STALL} ms（每 \${EVERY} 次一回）vs R0（预期：相同）" "\$OUT/r3-fix-inj-stall" "\$OUT/r0-base-inj-nostall"
echo "================ 改后原样 --only check ================"
home="\$OUT/home-check"; mkdir -p "\$home/.config"
HOME="\$home" XDG_CONFIG_HOME="\$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "$BIN_FIX_CLEAN/celadon_ui_snapshot" --only check > "\$OUT/check-fix-clean.txt" 2>&1
echo "check 退出码 \$? PASS=\$(grep -c '^PASS' "\$OUT/check-fix-clean.txt") FAIL=\$(grep -c '^FAIL' "\$OUT/check-fix-clean.txt")"
echo "完成 \$(date +%T)"
RUNS
chmod +x "$OUT/runs.sh"
"$SPARK" "$SRCROOT/t072-fix-clean" run bash "$OUT/runs.sh" 2>&1 | tee "$OUT/runs.out"
