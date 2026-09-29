#!/usr/bin/env bash
# 在本机（Mac）编译并运行 celadon_tests。
# 背景：107 测试服务器故障期间，Max 2026-09-24 当场指令「在本机跑测试」（ADR-002 修订）。
# 为了不拖垮整机：低优先级（nice）、最多 4 个编译进程、同一工作树排队；有别的会话在编译时先等。
#
# 用法：scripts/test-local.sh                          # 全套
#       scripts/test-local.sh --only MusicAnalyzer      # 只跑指定测试类（逗号分隔多个；编译仍是整个测试程序的增量编译）
# 环境：CELADON_BUILD_DIR 覆盖构建目录（默认放在系统临时目录，不进代码目录）。
#       CELADON_TEST_ARCH=x86_64 在 Apple Silicon 上编 x86_64 版、经 Rosetta 跑：Intel Mac 那一半（SSE2 路径、x86_64 的
#       vDSP 与 libm）发布前也跑一遍全套；构建目录另起一份，不与本机架构的混用。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"
ARCH="${CELADON_TEST_ARCH:-}"
BUILD="${CELADON_BUILD_DIR:-$TMP_ROOT/celadon-build${ARCH:+-$ARCH}-$KEY}"

# 同一工作树的两次调用排队（macOS 没有 flock，用 mkdir 锁；持锁进程已不在就清掉残留锁）。
LOCK="$TMP_ROOT/celadon-test-$KEY.lock"
until mkdir "$LOCK" 2>/dev/null; do
  if [ -f "$LOCK/pid" ] && ! kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; then
    rm -rf "$LOCK"
    continue
  fi
  sleep 2
done
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT

# 编译全局串行：别的会话正在 xcodebuild / cargo / ninja 编译时先等它结束。
while pgrep -x xcodebuild >/dev/null || pgrep -x cargo >/dev/null || pgrep -x ninja >/dev/null; do
  echo "有其他编译在跑，等 10 秒……"
  sleep 10
done

if [ ! -f "$BUILD/build.ninja" ]; then
  nice -n 10 cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release ${ARCH:+-DCMAKE_OSX_ARCHITECTURES=$ARCH}
fi

nice -n 10 cmake --build "$BUILD" --target celadon_tests -j 4
nice -n 10 ${ARCH:+arch -$ARCH} "$BUILD/celadon_tests" "$@"
