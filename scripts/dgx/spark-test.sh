#!/usr/bin/env bash
# DGX Spark 本机编译、运行 celadon_tests / celadon_render（Max 2026-10-03 23:5x 定：107 / 109 连不上时改在本机跑）。
# 本机是 aarch64 GCC 13：已知 Color 的 NEON 快路径写死融合乘加、与标量逐位对不上（Linux ARM 不是目标平台），
# 所以「过 / 败」一律对照同机基线，只算新增失败。
#
# 用法：spark-test.sh <源码目录> build [目标 ...]          只编译（默认目标 celadon_tests celadon_render）
#       spark-test.sh <源码目录> test [celadon_tests 参数]  编译 celadon_tests 后运行（例：--only LiveProfile,SegmentLedger）
#       spark-test.sh <源码目录> run <命令 ...>             在全机测试锁里跑任意命令（例：渲染、计时）；命令里可用 $BUILD
#       spark-test.sh <源码目录> dir                        打印构建目录
# 环境：SPARK_JOBS 并行编译数（默认 8，几个代理同时编时别抢光 20 核）；CELADON_CXX_FLAGS 追加给 CMAKE_CXX_FLAGS（构建目录按它分开）。
# 锁：同一构建目录的编译互斥（<构建目录>.lock）；测试与 run 全机串行（locks/run.lock），让计时护栏不被别的测试干扰。编译不进全机锁。
set -euo pipefail
[ "$#" -ge 2 ] || { sed -n 2,11p "$0"; exit 2; }
SRC="$(cd "$1" && pwd -P)"; shift
CMD="$1"; shift
CACHE="$HOME/.cache/celadon-spark"
JUCE_DIR="/home/maxsong/All-Project/JUCE"
FLAGS="${CELADON_CXX_FLAGS:-}"
KEY="$(printf '%s|%s' "$SRC" "$FLAGS" | sha256sum | cut -c1-10)"
BUILD="$CACHE/build/$(basename "$SRC")-$KEY"
export BUILD
export PATH="$CACHE/venv/bin:$PATH"   # ninja 与 Python 依赖（numpy / scipy / soundfile / matplotlib）都在这个 venv
# 截图工具要的 freetype2 / fontconfig / x11 开发头：本机没装 -dev 包（装要 sudo），用 Ubuntu 官方 .deb 解到 guidev/root，运行库用系统的。
GUIDEV="$CACHE/guidev/root"
export PKG_CONFIG_PATH="$GUIDEV/usr/lib/aarch64-linux-gnu/pkgconfig:$GUIDEV/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
mkdir -p "$CACHE/locks" "$BUILD"

build() {
  (
    flock 9
    if [ ! -f "$BUILD/build.ninja" ]; then
      cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCELADON_JUCE_DIR="$JUCE_DIR" -DCMAKE_CXX_FLAGS="$FLAGS"
    fi
    nice -n 10 cmake --build "$BUILD" -j "${SPARK_JOBS:-8}" --target "$@"
  ) 9>"$BUILD.lock"
}

case "$CMD" in
  dir)   echo "$BUILD" ;;
  build) if [ "$#" -eq 0 ]; then build celadon_tests celadon_render; else build "$@"; fi ;;
  test)  build celadon_tests
         ( flock 8; "$BUILD/celadon_tests" "$@" ) 8>"$CACHE/locks/run.lock" ;;
  run)   ( flock 8; "$@" ) 8>"$CACHE/locks/run.lock" ;;
  *)     echo "spark-test.sh：未知子命令 $CMD" >&2; exit 2 ;;
esac
