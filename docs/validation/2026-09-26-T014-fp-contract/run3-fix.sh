#!/usr/bin/env bash
# T-014 第三轮 · 补丁预跑：打了 fix.patch 的临时副本上
#   1. 全套（默认 Release，与 scripts/test-linux.sh 相同）；
#   2. 全套（-march=x86-64-v3，GCC 已由 CMakeLists 强制 -ffp-contract=off）；
#   3. 守卫绊线：三个 .cpp 在 6 种编译选项下 -fsyntax-only，看该报错的报错、该通过的通过。
set -uo pipefail
TOP="$PWD"

full_suite () {
  local name="$1" flags="$2" B="$BUILD/$1"
  cmake -S Celadon -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_C_FLAGS="$flags" > "$BUILD/cmake-$name.log" 2>&1 || { tail -20 "$BUILD/cmake-$name.log"; return 1; }
  cmake --build "$B" --target celadon_tests > "$BUILD/build-$name.log" 2>&1 || { echo "### [$name] 构建失败"; tail -30 "$BUILD/build-$name.log"; return 1; }
  echo "### 补丁 · [$name] CXX_FLAGS='$flags'   警告数 $(grep -c 'warning:' "$BUILD/build-$name.log")"
  ( cd "$B" && ninja -t commands celadon_tests | grep 'Source/dsp/TruePeak.cpp' | head -1 | grep -o -E -- '-march=[^ ]+|-ffp-contract=[a-z]+|-DCELADON_FP_CONTRACT_OFF=1' | tr '\n' ' ' | sed 's/^/  TruePeak.cpp 的相关选项：/'; echo )
  "$B/celadon_tests" > "$BUILD/tests-$name.log" 2>&1
  echo "  退出码 $?；$(grep '==== celadon_tests' "$BUILD/tests-$name.log")"
  grep '!!! Test' "$BUILD/tests-$name.log" | head -20
}

full_suite fix-base ""
full_suite fix-v3 "-march=x86-64-v3"

echo "### 守卫绊线（-fsyntax-only，三个文件各编一次；期望：报错 = #error 生效，通过 = 不误伤）"
DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 \
-DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 \
-DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DNDEBUG=1"
trip () {
  local expect="$1" cxx="$2" flags="$3"
  for f in HalfBandOversampler Saturator TruePeak; do
    out="$($cxx -std=gnu++17 -O3 $DEFS -isystem "$TOP/JUCE/modules" $flags -fsyntax-only "Celadon/Source/dsp/$f.cpp" 2>&1)"
    status=$?
    if [ $status -ne 0 ]; then got="报错"; else got="通过"; fi
    msg="$(printf '%s\n' "$out" | grep -m1 -o '#error.*' | cut -c1-90)"
    [ "$got" = "$expect" ] && verdict="✓" || verdict="✗ 期望$expect"
    printf '  %s %-4s %-8s %-62s %s %s\n' "$verdict" "$got" "$cxx" "$flags" "$f" "$msg"
  done
}
trip 报错 g++     "-march=x86-64-v3"
trip 通过 g++     "-march=x86-64-v3 -ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1"
trip 通过 g++     ""
trip 报错 g++     "-ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1 -ffast-math"
trip 通过 clang++ "-march=x86-64-v3"
trip 报错 clang++ "-ffast-math"
exit 0
