#!/usr/bin/env bash
# 在本机编整插件性能基准（tools/perf）并运行：与 scripts/test-local.sh 共用同一把锁，低优先级，构建目录放系统临时目录。
# 性能验收一律用它（ADR-007）：测的是与 Xcode 同编译参数的整个 CeladonAudioProcessor，不是无头模块测试。
#
# 用法：scripts/perf-local.sh [FLAVOUR] [celadon_bench 参数...]
#   FLAVOUR 默认 XcodeRelease（= 装进系统的那一版的编译参数）；也可 XcodeDebug / DebugO1 / ReleaseO2NoLto
#   参数默认：grid all 44100,48000,96000,192000 32,64,512 2 4 3
#            （模式 场景 采样率列表 块长列表 空跑秒 每遍秒 遍数；模式另有 modules / parts / gui / ecore）
# 读数口径：单个性能核、单线程、先空跑再测、多遍取最短；输出前后各打印一次供电状态——
#           电池 + 低电量模式下同一程序约慢 1.45 倍，验收门槛按接电源定。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"

FLAVOUR="${1:-XcodeRelease}"
[ "$#" -gt 0 ] && shift
if [ "$#" -eq 0 ]; then
  set -- grid all 44100,48000,96000,192000 32,64,512 2 4 3
fi
BUILD="${CELADON_PERF_BUILD_DIR:-$TMP_ROOT/celadon-perf-$FLAVOUR-$KEY}"

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

while pgrep -x xcodebuild >/dev/null || pgrep -x cargo >/dev/null || pgrep -x ninja >/dev/null; do
  echo "有其他编译在跑，等 10 秒……"
  sleep 10
done

if [ ! -f "$BUILD/build.ninja" ]; then
  nice -n 10 cmake -S "$ROOT/tools/perf" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=None -DFLAVOUR="$FLAVOUR"
fi
nice -n 10 cmake --build "$BUILD" --target celadon_bench -j 4

"$ROOT/tools/perf/power.sh"
"$BUILD/celadon_bench" "$@"
"$ROOT/tools/perf/power.sh"
