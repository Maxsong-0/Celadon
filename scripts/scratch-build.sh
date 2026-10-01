#!/usr/bin/env bash
# 编译一个临时实验程序（调常数、量数字用），链接测试构建里已经编好的 JUCE 静态库与 Source/dsp。
# 与 test-local.sh / check-syntax.sh 共用同一把锁，低优先级；只给代理和主进程做实验，不进仓库。
#
# 用法：scripts/scratch-build.sh <实验 .cpp> <输出可执行文件> [额外 .cpp ...]
#   - 自动带上 Source/dsp/*.cpp；实验程序里 #include "dsp/BalanceModule.h" 这类路径即可（-I Source）。
#   - 需要先跑过一次 scripts/test-local.sh（生成 libceladon_juce.a）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"
BUILD="${CELADON_BUILD_DIR:-$TMP_ROOT/celadon-build-$KEY}"
LIB="$BUILD/libceladon_juce.a"

if [ "$#" -lt 2 ]; then
  echo "用法：scripts/scratch-build.sh <实验 .cpp> <输出可执行文件> [额外 .cpp ...]" >&2
  exit 2
fi
if [ ! -f "$LIB" ]; then
  echo "找不到 ${LIB}：先跑一次 scripts/test-local.sh" >&2
  exit 2
fi

SRC="$1"; OUT="$2"; shift 2

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

# 编译全局串行：别的会话正在 xcodebuild / cargo / ninja 编译时先等它结束（与兄弟脚本一致）。
while pgrep -x xcodebuild >/dev/null || pgrep -x cargo >/dev/null || pgrep -x ninja >/dev/null; do
  echo "有其他编译在跑，等 10 秒……"
  sleep 10
done

nice -n 10 clang++ -std=c++17 -O2 -DNDEBUG=1 \
  -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 \
  -DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 \
  -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 -DJUCE_MODULE_AVAILABLE_juce_dsp=1 \
  -isystem "$ROOT/../JUCE/modules" -I "$ROOT/Source" -I "$ROOT/tests" \
  "$SRC" "$ROOT"/Source/dsp/*.cpp "$@" "$LIB" \
  -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio -framework CoreMIDI \
  -framework Foundation -framework IOKit -framework QuartzCore -framework Security \
  -o "$OUT"
echo "built $OUT"
