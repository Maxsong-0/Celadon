#!/usr/bin/env bash
# T-027 微基准：在 107 上编两份 refbench（只差 GlazeController.o：改前 = $BASE（默认 T-027 的父提交 HEAD^）/ 改后工作树）与 callbench，交替跑。
# 前提：本工作树已用 scripts/test-linux.sh 在同一台机器上编过一次（复用那次的编译选项与其余目标文件）。本机不编译、不运行。
# 用法（在工作树根目录）：docs/validation/2026-09-29-T027-reference-cache/run-bench.sh [轮数，默认 5]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd -P)"
HERE="$ROOT/docs/validation/2026-09-29-T027-reference-cache"
HOST="${CELADON_TEST_HOST:-maxsong@192.168.100.107}"
ROUNDS="${1:-5}"
BASE="$(git -C "$ROOT" rev-parse --short "${BASE:-HEAD^}")"   # 改前 = T-027 的父提交
echo "base commit: $BASE"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

# 与 scripts/test-linux.sh 同一套远端目录命名。
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
NAME="$(basename "$ROOT")-$KEY"
BENCH=".cache/celadon-t027-bench"

"${SSH[@]}" "$HOST" "rm -rf '$BENCH' && mkdir -p '$BENCH/base'"
scp -q -o BatchMode=yes "$HERE/refbench.cpp" "$HERE/callbench.cpp" "$HOST:$BENCH/"
git -C "$ROOT" show "$BASE":Source/dsp/GlazeController.cpp | "${SSH[@]}" "$HOST" "cat > '$BENCH/base/GlazeController.cpp'"

"${SSH[@]}" "$HOST" "NAME=$(printf '%q' "$NAME") BENCH=$(printf '%q' "$BENCH") ROUNDS=$ROUNDS bash -euo pipefail -s" <<'REMOTE_SCRIPT'
SRC="$HOME/.cache/celadon-tests/$NAME/Celadon"
BUILD="$HOME/.cache/celadon-build/$NAME"
cd "$HOME/$BENCH"

# 测试目标里 GlazeController.cpp 的编译命令 → 去掉输出 / 依赖文件参数，留下选项。
LINE="$(ninja -C "$BUILD" -t commands celadon_tests | grep -F -- "-c $SRC/Source/dsp/GlazeController.cpp" | tail -1)"
COMPILER="$(printf '%s\n' "$LINE" | awk '{print $1}')"
FLAGS="$(printf '%s\n' "$LINE" | sed -E 's#^[^ ]+ ##; s# -o [^ ]+##; s# -c [^ ]+##; s# -MD##; s# -MT [^ ]+##; s# -MF [^ ]+##')"
echo "compiler: $COMPILER"; "$COMPILER" --version | head -1
echo "flags: $FLAGS"

OBJDIR="$BUILD/CMakeFiles/celadon_tests.dir/Source/dsp"
OTHERS="$(ls "$OBJDIR"/*.cpp.o | grep -v '/GlazeController.cpp.o$' | tr '\n' ' ')"
NEWOBJ="$OBJDIR/GlazeController.cpp.o"

cd "$BUILD"
# shellcheck disable=SC2086
"$COMPILER" $FLAGS -I"$SRC/Source/dsp" -c "$HOME/$BENCH/base/GlazeController.cpp" -o "$HOME/$BENCH/base.o"
# shellcheck disable=SC2086
"$COMPILER" $FLAGS -I"$SRC/Source/dsp" -c "$HOME/$BENCH/refbench.cpp" -o "$HOME/$BENCH/refbench.o"
# shellcheck disable=SC2086
"$COMPILER" $FLAGS -I"$SRC/Source/dsp" -c "$HOME/$BENCH/callbench.cpp" -o "$HOME/$BENCH/callbench.o"
cd "$HOME/$BENCH"

LIBS="$BUILD/libceladon_juce.a -lpthread -ldl -lrt"
# shellcheck disable=SC2086
"$COMPILER" -O3 refbench.o $OTHERS base.o $LIBS -o refbench_base
# shellcheck disable=SC2086
"$COMPILER" -O3 refbench.o $OTHERS "$NEWOBJ" $LIBS -o refbench_new
# shellcheck disable=SC2086
"$COMPILER" -O3 callbench.o $OTHERS "$NEWOBJ" $LIBS -o callbench
sha256sum base/GlazeController.cpp "$SRC/Source/dsp/GlazeController.cpp"

echo "== load before: $(cut -d' ' -f1-3 /proc/loadavg)"
CORE=5
for mode in steady reopen; do
  for r in $(seq 1 "$ROUNDS"); do
    printf 'round %s base: ' "$r"; taskset -c "$CORE" ./refbench_base 90 60 "$mode"
    printf 'round %s new : ' "$r"; taskset -c "$CORE" ./refbench_new 90 60 "$mode"
  done
done
echo "== callbench"
taskset -c "$CORE" ./callbench
echo "== load after: $(cut -d' ' -f1-3 /proc/loadavg)"
REMOTE_SCRIPT
