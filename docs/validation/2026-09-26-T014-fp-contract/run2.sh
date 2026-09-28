#!/usr/bin/env bash
# T-014 第二轮：备选修法的效果（只编对照程序，不重编测试）。
#   a. GCC -ffp-contract=on（只在同一表达式内合并，与 clang 默认相同）；
#   b. 三个文件在 #include 之后加 #pragma GCC optimize ("fp-contract=off")（代码级，不靠构建选项）；
#   c. 守卫宏：-ffp-contract=off 时 __FP_FAST_FMAF 是否仍定义（决定守卫能不能只看编译器宏）。
set -uo pipefail
W="$BUILD/t014"
TOP="$PWD"
DEFS="-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 \
-DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 \
-DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DNDEBUG=1"
LIB="$BUILD/base/libceladon_juce.a"

# b 的源码：原文件复制一份，在最后一个 #include / #endif 之后的首个 namespace 前插 pragma。
P="$W/pragma-src/Source/dsp"
mkdir -p "$P"
cp Celadon/Source/dsp/*.h "$P/"
for f in HalfBandOversampler Saturator TruePeak; do
  awk 'BEGIN { done = 0 } /^namespace celadon/ && !done { print "#pragma GCC optimize (\"fp-contract=off\")"; done = 1 } { print }' \
    "Celadon/Source/dsp/$f.cpp" > "$P/$f.cpp"
  grep -n 'pragma GCC optimize' "$P/$f.cpp" | sed "s/^/  $f.cpp:/"
done
mkdir -p "$W/pragma-src/t014" && cp Celadon/t014/dump.cpp "$W/pragma-src/t014/"

dump_cfg () {
  local name="$1" src="$2" flags="$3"
  local d="$W/dump-$name"
  rm -rf "$d"; mkdir -p "$d/out"
  for f in HalfBandOversampler Saturator TruePeak; do
    g++ -std=gnu++17 -O3 $DEFS -isystem "$TOP/JUCE/modules" $flags -c "$src/Source/dsp/$f.cpp" -o "$d/$f.o" 2> "$d/$f.warn" || { cat "$d/$f.warn"; return 1; }
    [ -s "$d/$f.warn" ] && { echo "  $f.cpp 编译输出："; head -5 "$d/$f.warn" | sed 's/^/    /'; }
  done
  g++ -std=gnu++17 -O3 $DEFS -isystem "$TOP/JUCE/modules" $flags -c "$src/t014/dump.cpp" -o "$d/dump.o" || return 1
  g++ "$d"/*.o "$LIB" -lpthread -ldl -lrt -o "$d/dump" || return 1
  echo "### dump [$name] g++ $flags"
  "$d/dump" "$d/out" | sed 's/^/  /'
  for f in HalfBandOversampler Saturator TruePeak; do
    objdump -d --no-show-raw-insn -C "$d/$f.o" > "$d/$f.dis"
    printf '    %-20s 融合乘加合计 %4d\n' "$f" "$(grep -c -E $'\tvfn?m(add|sub)' "$d/$f.dis")"
    awk '/^[0-9a-f]+ <.*>:$/ { fn=$0; sub(/^[0-9a-f]+ </,"",fn); sub(/>:$/,"",fn) }
         /\tvfn?m(add|sub)/ { c[fn]++ }
         END { for (k in c) printf "        %4d  %s\n", c[k], k }' "$d/$f.dis" | sort -k2 | cut -c1-160
  done
  echo "  相对 gcc-base："
  python3 Celadon/t014/compare.py "$W/dump-gcc-base/out" "$d/out"
}

dump_cfg gcc-v3-contract-on Celadon "-march=x86-64-v3 -ffp-contract=on"
dump_cfg gcc-v3-pragma      "$W/pragma-src" "-march=x86-64-v3"

echo "### -ffp-contract=off 时的预定义宏（守卫能否只靠编译器宏判断）："
echo | g++ -march=x86-64-v3 -ffp-contract=off -x c++ -dM -E - | grep -E '__FP_FAST_FMAF?\b|__FMA__|CONTRACT' || true
echo "### GCC -march=x86-64-v3 默认 -mtune："
g++ -march=x86-64-v3 -Q --help=target 2>/dev/null | grep -E '^\s+-mtune=' | head -2
exit 0
