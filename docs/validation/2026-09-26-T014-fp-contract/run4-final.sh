#!/usr/bin/env bash
# T-014 最终验收（2026-09-27，补丁已进真仓库）：经 scripts/test-linux.sh bash -c "$(cat run4-final.sh)" 在 107 上跑。
#   1. 基线全套：与 test-linux.sh 默认流程相同，用共享构建目录 $BUILD（不传任何 CMAKE_*_FLAGS，免得写进共享缓存）；
#   2. -march=x86-64-v3 全套：独立目录 $BUILD-v3，跑完删；
#   3. 守卫绊线：三个文件 × 6 种选项 -fsyntax-only。
# 日志放 $BUILD-t014，取回后删。
set -uo pipefail
TOP="$PWD"
L="${BUILD:?}-t014"
rm -rf "$L"; mkdir -p "$L"
echo "### $(hostname)  $(g++ --version | head -1)"

summary () {
  local name="$1" dir="$2"
  echo "### [$name] 警告数 $(grep -c 'warning:' "$L/build-$name.log")"
  ( cd "$dir" && ninja -t commands celadon_tests | grep 'Source/dsp/TruePeak.cpp' | head -1 \
      | grep -o -E -- '-march=[^ ]+|-ffp-contract=[a-z]+|-DCELADON_FP_CONTRACT_OFF=1' | tr '\n' ' ' | sed 's/^/  TruePeak.cpp 的相关选项：/'; echo )
  "$dir/celadon_tests" > "$L/tests-$name.log" 2>&1
  echo "  退出码 $?；$(grep '==== celadon_tests' "$L/tests-$name.log")"
  grep '!!! Test' "$L/tests-$name.log" | head -20
}

# 1. 基线全套
cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$L/cmake-base.log" 2>&1 || { tail -20 "$L/cmake-base.log"; exit 1; }
grep -E '^CMAKE_(CXX|C)_FLAGS:STRING=' "$BUILD/CMakeCache.txt" | sed 's/^/  共享缓存：/'
cmake --build "$BUILD" --target celadon_tests > "$L/build-base.log" 2>&1 || { echo "基线构建失败"; tail -40 "$L/build-base.log"; exit 1; }
summary base "$BUILD"

# 2. -march=x86-64-v3 全套
V="${BUILD:?}-v3"
cmake -S Celadon -B "$V" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS=-march=x86-64-v3 -DCMAKE_C_FLAGS=-march=x86-64-v3 > "$L/cmake-v3.log" 2>&1 || { tail -20 "$L/cmake-v3.log"; exit 1; }
cmake --build "$V" --target celadon_tests > "$L/build-v3.log" 2>&1 || { echo "v3 构建失败"; tail -40 "$L/build-v3.log"; rm -rf "$V"; exit 1; }
summary v3 "$V"
rm -rf "$V"

# 3. 守卫绊线
echo "### 守卫绊线（-fsyntax-only；期望：报错 = #error 生效，通过 = 不误伤）"
DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 \
-DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 \
-DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DNDEBUG=1"
ok=0; bad=0
trip () {
  local expect="$1" cxx="$2" flags="$3" f out got
  for f in HalfBandOversampler Saturator TruePeak; do
    if out="$($cxx -std=gnu++17 -O3 $DEFS -isystem "$TOP/JUCE/modules" $flags -fsyntax-only "Celadon/Source/dsp/$f.cpp" 2>&1)"; then got="通过"; else got="报错"; fi
    if [ "$got" = "$expect" ]; then ok=$((ok + 1)); v="对"; else bad=$((bad + 1)); v="错（期望$expect）"; fi
    printf '  %s %s %-8s %-62s %s %s\n' "$v" "$got" "$cxx" "$flags" "$f" "$(printf '%s\n' "$out" | grep -m1 -o '#error.*' | cut -c1-60)"
  done
}
trip 报错 g++     "-march=x86-64-v3"
trip 通过 g++     "-march=x86-64-v3 -ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1"
trip 通过 g++     ""
trip 报错 g++     "-ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1 -ffast-math"
trip 通过 clang++ "-march=x86-64-v3"
trip 报错 clang++ "-ffast-math"
echo "  绊线合计：对 $ok / 错 $bad"
exit 0
