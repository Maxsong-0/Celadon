#!/usr/bin/env bash
# 对插件源文件做 -fsyntax-only 检查（只查语法与警告，不链接、不产出插件；ADR-002 修订）。
# 与 scripts/test-local.sh 共用同一把锁：同一工作树同一时刻只有一组编译进程，别的会话在编译时先等。
#
# 用法：scripts/check-syntax.sh                      # 查 Source/ 下全部 .cpp
#       scripts/check-syntax.sh Source/ui/Knob.cpp     # 只查指定文件（可多个）
# 通过标准：每个文件 exit=0，且没有 Source/ 下文件的 warning / error。
# macOS 上每个文件查两遍（-arch arm64、-arch x86_64）：Intel Mac 那一半走 SSE2 分支，也要用 Apple 的工具链查到。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"

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

cd "$ROOT"
DEFS=(-DDEBUG=1 -D_DEBUG=1 -DJUCE_PROJUCER_VERSION=0x90002 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
      -DJUCE_VST3_CAN_REPLACE_VST2=0 -DJUCE_STRICT_REFCOUNTEDPOINTER=1)
for m in audio_basics audio_devices audio_formats audio_plugin_client audio_processors audio_processors_headless \
         audio_utils core data_structures dsp events graphics gui_basics gui_extra; do
  DEFS+=(-DJUCE_MODULE_AVAILABLE_juce_$m=1)
done

if [ "$#" -eq 0 ]; then
  FILES=()
  while IFS= read -r f; do FILES+=("$f"); done < <(find Source -name '*.cpp' | sort)
else
  FILES=("$@")
fi

# 其他系统（Linux 上的 clang）不认 -arch：只按本机架构查一遍。
ARCHS=("")
if [ "$(uname -s)" = "Darwin" ]; then
  ARCHS=(arm64 x86_64)
fi

status=0
for f in "${FILES[@]}"; do
  for arch in "${ARCHS[@]}"; do
    label="$f${arch:+ [$arch]}"
    code=0
    out="$(nice -n 10 clang++ ${arch:+-arch "$arch"} -std=c++17 -fsyntax-only "${DEFS[@]}" \
            -include JuceLibraryCode/JucePluginDefines.h -I JuceLibraryCode -isystem ../JUCE/modules \
            -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Woverloaded-virtual "$f" 2>&1)" || code=$?
    own="$(printf '%s\n' "$out" | grep -E "(error|warning)" | grep -v "JUCE/modules" || true)"
    if [ "$code" -ne 0 ]; then
      # clang 非零退出一律算失败，哪怕错误行都在 JUCE 里（多半是我们的用法触发了 JUCE 头文件里的错误）。
      echo "FAIL $label (clang exit $code)"
      printf '%s\n' "$out" | grep -E "(error|warning)" | head -40
      status=1
    elif [ -n "$own" ]; then
      echo "== $label"
      printf '%s\n' "$own" | head -40
      status=1
    else
      echo "OK $label"
    fi
  done
done
exit "$status"
