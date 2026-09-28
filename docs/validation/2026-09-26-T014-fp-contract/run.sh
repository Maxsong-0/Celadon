#!/usr/bin/env bash
# T-014 远端取证（经 scripts/test-linux.sh 在服务器上跑；当前目录 = 同步目录，Celadon/ 与 JUCE/ 同级，$BUILD 是本副本独占的构建目录）。
#   1. 三套 celadon_tests 构建（基线 / -march=x86-64-v3 / v3 + -ffp-contract=off），各跑 TruePeakDetector,Saturator,ColorModule；
#   2. 对照程序按 7 种编译选项编，逐位比较输出；
#   3. 反汇编数三个文件里的融合乘加指令；
#   4. GCC 头文件里 _mm_add_ps 的写法、GCC 默认 fp-contract；clang 交叉编 aarch64 小样本看默认是否合并。
set -uo pipefail
W="$BUILD/t014"
rm -rf "$W"
mkdir -p "$W"
TOP="$PWD"
ONLY="TruePeakDetector,Saturator,ColorModule"

echo "### $(hostname)  $(uname -m)  $(g++ --version | head -1)  |  $(clang++ --version | head -1)"
echo "### GCC 默认 fp-contract（C++17 ISO / gnu++17）："
echo | g++ -std=c++17 -x c++ -Q --help=common - 2>/dev/null | grep 'ffp-contract'
echo | g++ -std=gnu++17 -x c++ -Q --help=common - 2>/dev/null | grep 'ffp-contract'
echo "### GCC -march=x86-64-v3 是否定义 __FP_FAST_FMAF / __FMA__（基线对照）："
echo | g++ -x c++ -dM -E - | grep -E '__FP_FAST_FMAF?\b|__FMA__' || echo "  基线：都没有"
echo | g++ -march=x86-64-v3 -x c++ -dM -E - | grep -E '__FP_FAST_FMAF?\b|__FMA__'
echo "### GCC xmmintrin.h 里 _mm_add_ps / _mm_mul_ps 的实现："
XMM="$(dirname "$(g++ -print-libgcc-file-name)")/include/xmmintrin.h"
grep -A3 -E '^_mm_(add|mul)_ps \(' "$XMM"

build_tests () {
  local name="$1" flags="$2"
  echo "### celadon_tests [$name] CXX_FLAGS='$flags'"
  cmake -S Celadon -B "$BUILD/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_C_FLAGS="$flags" > "$W/cmake-$name.log" 2>&1 || { echo "cmake 失败"; tail -20 "$W/cmake-$name.log"; return 1; }
  if ! cmake --build "$BUILD/$name" --target celadon_tests > "$W/build-$name.log" 2>&1; then
    echo "构建失败"; tail -30 "$W/build-$name.log"; return 1
  fi
  echo "  警告数：$(grep -c 'warning:' "$W/build-$name.log")"
  ( cd "$BUILD/$name" && ninja -t commands celadon_tests | grep 'Source/dsp/HalfBandOversampler.cpp' | head -1 | sed 's/^/  编译命令：/' )
  "$BUILD/$name/celadon_tests" --only "$ONLY" > "$W/tests-$name.log" 2>&1
  echo "  退出码 $?；$(grep '==== celadon_tests' "$W/tests-$name.log")"
  grep -n -i -E '!!! Test|FAILED' "$W/tests-$name.log" | head -40
}

build_tests base ""
build_tests v3 "-march=x86-64-v3"
build_tests v3-nocontract "-march=x86-64-v3 -ffp-contract=off"

# ---- 对照程序：JUCE 静态库用基线构建的那份，被测三个文件 + 对照程序按各配置编 ----
DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 \
-DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 \
-DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DNDEBUG=1"
LIB="$BUILD/base/libceladon_juce.a"

dump_cfg () {
  local name="$1" cxx="$2" flags="$3"
  local d="$W/dump-$name"
  mkdir -p "$d/out"
  for f in HalfBandOversampler Saturator TruePeak; do
    $cxx -std=gnu++17 -O3 $DEFS -isystem "$TOP/JUCE/modules" $flags -c "Celadon/Source/dsp/$f.cpp" -o "$d/$f.o" || return 1
  done
  $cxx -std=gnu++17 -O3 $DEFS -isystem "$TOP/JUCE/modules" $flags -c Celadon/t014/dump.cpp -o "$d/dump.o" || return 1
  g++ "$d"/*.o "$LIB" -lpthread -ldl -lrt -o "$d/dump" || return 1
  echo "### dump [$name] $cxx $flags"
  "$d/dump" "$d/out" | sed 's/^/  /'
  echo "  融合乘加指令数（vfmadd / vfmsub / vfnmadd / vfnmsub）："
  for f in HalfBandOversampler Saturator TruePeak; do
    objdump -d --no-show-raw-insn -C "$d/$f.o" > "$d/$f.dis"
    printf '    %-20s 合计 %4d\n' "$f" "$(grep -c -E $'\tvfn?m(add|sub)' "$d/$f.dis")"
    awk '/^[0-9a-f]+ <.*>:$/ { fn=$0; sub(/^[0-9a-f]+ </,"",fn); sub(/>:$/,"",fn) }
         /\tvfn?m(add|sub)/ { c[fn]++ }
         END { for (k in c) printf "        %4d  %s\n", c[k], k }' "$d/$f.dis" | sort -k2 | cut -c1-160
  done
}

dump_cfg gcc-base          g++     ""
dump_cfg gcc-v3            g++     "-march=x86-64-v3"
dump_cfg gcc-v3-nocontract g++     "-march=x86-64-v3 -ffp-contract=off"
dump_cfg gcc-scalar        g++     "-DCELADON_FORCE_SCALAR_SIMD"
dump_cfg gcc-scalar-v3     g++     "-DCELADON_FORCE_SCALAR_SIMD -march=x86-64-v3"
dump_cfg clang-v3          clang++ "-march=x86-64-v3"
dump_cfg clang-v3-fast     clang++ "-march=x86-64-v3 -ffp-contract=fast"

echo "### 逐位比较（相对 gcc-base）"
for c in gcc-v3 gcc-v3-nocontract gcc-scalar gcc-scalar-v3 clang-v3 clang-v3-fast; do
  echo "gcc-base vs $c"
  python3 Celadon/t014/compare.py "$W/dump-gcc-base/out" "$W/dump-$c/out"
done
echo "gcc-v3 vs gcc-scalar-v3（开 FMA 后 SSE2 路径与标量回退是否还相同）"
python3 Celadon/t014/compare.py "$W/dump-gcc-v3/out" "$W/dump-gcc-scalar-v3/out"

echo "### clang 交叉编 aarch64 小样本（NEON 封装写法 / 两条语句 / 一个表达式）"
for mode in "" "-ffp-contract=fast" "-ffp-contract=off"; do
  echo "  clang --target=aarch64-linux-gnu -O3 ${mode:-（默认）}"
  clang++ --target=aarch64-linux-gnu -ffreestanding -std=c++17 -O3 $mode -S -o - Celadon/t014/arm-snippet.cpp 2>&1 \
    | awk '/^_Z[A-Za-z0-9_]*:/ { fn=$1 } /\t(fmla|fmadd|fmul|fadd)\t/ { printf "    %-45s %s\n", fn, $1 }'
done
exit 0
