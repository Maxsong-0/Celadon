#!/usr/bin/env bash
# T-107：改前（main a768b8a）与改后（本分支）各出一组截图 + 改后跑 --only check，全部在全机测试锁里跑（截图工具会改用户设置文件）。
# 用法：shots.sh <输出目录>
set -euo pipefail
OUT="$1"
CACHE="$HOME/.cache/celadon-spark"
SPARK="$CACHE/bin/spark-test.sh"
STATES="zh-default,en-default,zh-glaze90,zh-zoom70,zh-glaze0,zh-original,zh-truepeak,zh-legacy-notice,zh-advanced"
mkdir -p "$OUT/before" "$OUT/after" "$OUT/after-x2"

"$SPARK" "$CACHE/src/t107-main" run bash -c "xvfb-run -a \"\$BUILD/celadon_ui_snapshot\" --deterministic --out '$OUT/before' --only '$STATES'"
"$SPARK" "$CACHE/src/t107-work" run bash -c "xvfb-run -a \"\$BUILD/celadon_ui_snapshot\" --deterministic --out '$OUT/after' --only '$STATES,zh-default-wide,en-default-wide'"
"$SPARK" "$CACHE/src/t107-work" run bash -c "xvfb-run -a \"\$BUILD/celadon_ui_snapshot\" --deterministic --scale 2 --out '$OUT/after-x2' --only 'zh-default,zh-default-wide'"
"$SPARK" "$CACHE/src/t107-work" run bash -c "xvfb-run -a \"\$BUILD/celadon_ui_snapshot\" --out '$OUT/check-after' --only check" > "$OUT/check-after.log" 2>&1 || echo "check-after exit $?" >> "$OUT/check-after.log"
"$SPARK" "$CACHE/src/t107-main" run bash -c "xvfb-run -a \"\$BUILD/celadon_ui_snapshot\" --out '$OUT/check-before' --only check" > "$OUT/check-before.log" 2>&1 || echo "check-before exit $?" >> "$OUT/check-before.log"
echo "shots done"
