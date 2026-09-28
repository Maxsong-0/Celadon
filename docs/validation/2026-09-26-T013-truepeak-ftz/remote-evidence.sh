#!/usr/bin/env bash
# BUG-001 取证（在 x86 服务器上跑，只读现有构建目录、只写 ~/.cache/celadon-ftz-repro/）：
#   1. 反汇编现有构建里（改前源码编出的）TruePeak.cpp.o，看参照实现与 SSE2 核各用什么指令取大；
#   2. 编译并运行独立复现程序（g++ -O2 / -O0，有 clang++ 也跑一遍）。
set -uo pipefail
W="$HOME/.cache/celadon-ftz-repro"
mkdir -p "$W"
cd "$W"

echo "### host: $(hostname)  $(uname -m)  $(g++ --version | head -1)"

B="$(ls -d "$HOME"/.cache/celadon-build/Celadon-* 2>/dev/null | head -1)"
OBJ="$(find "$B" -name 'TruePeak.cpp.o' 2>/dev/null | head -1)"
echo "### object: $OBJ  (mtime $(stat -c %y "$OBJ" 2>/dev/null))"
if [ -n "$OBJ" ]; then
  objdump -d --no-show-raw-insn -C "$OBJ" > truepeak-old.dis
  echo "--- functions and max/cmp/blend instruction counts"
  awk '/^[0-9a-f]+ <.*>:$/ { fn=$0 } /maxss|maxps|cmpltps|cmpltss|comiss|ucomiss|andnps|orps/ { split($0,a,"\t"); ins=a[2]; sub(/ .*/,"",ins); count[fn" :: "ins]++ } END { for (k in count) print count[k], k }' truepeak-old.dis | sort -k2 | grep -E "directFormPeak|computePeakGroups|processSample|processChunk"
  echo "--- compile flags"
  grep -o '\-O[0-3s]\|-march=[^ ]*\|-m[a-z0-9.]*\|-ffp-contract=[a-z]*' "$B/build.ninja" 2>/dev/null | sort | uniq -c | head
fi

echo "### repro g++ -O2"
g++ -std=c++17 -O2 -o repro-O2 repro.cpp && ./repro-O2
echo "### repro g++ -O0"
g++ -std=c++17 -O0 -o repro-O0 repro.cpp && ./repro-O0
if command -v clang++ >/dev/null; then
  echo "### repro clang++ -O2"
  clang++ -std=c++17 -O2 -o repro-clang repro.cpp && ./repro-clang
fi
