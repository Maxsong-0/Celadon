#!/usr/bin/env bash
# T-014 第三轮 · 范围：原始源码（未打补丁）在 -march=x86-64-v3 下跑全套，看除三个测试类以外还有哪些契约被破坏。
# 经 scripts/test-linux.sh 在服务器上跑（当前目录 = 同步目录，$BUILD 是本副本独占的构建目录）。
set -uo pipefail
B="$BUILD/v3-full"
cmake -S Celadon -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS=-march=x86-64-v3 -DCMAKE_C_FLAGS=-march=x86-64-v3 > "$BUILD/cmake-v3-full.log" 2>&1 || { tail -20 "$BUILD/cmake-v3-full.log"; exit 1; }
cmake --build "$B" --target celadon_tests > "$BUILD/build-v3-full.log" 2>&1 || { tail -30 "$BUILD/build-v3-full.log"; exit 1; }
echo "### 原始源码 · -march=x86-64-v3 · 全套   警告数 $(grep -c 'warning:' "$BUILD/build-v3-full.log")"
"$B/celadon_tests" > "$BUILD/tests-v3-full.log" 2>&1
echo "退出码 $?；$(grep '==== celadon_tests' "$BUILD/tests-v3-full.log")"
echo "--- 按测试类统计失败数"
awk '/^Starting tests in: / { s=$0; sub(/^Starting tests in: /,"",s); split(s,a," / "); cls=a[1] } /^!!! Test/ { c[cls]++ } END { for (k in c) printf "  %6d  %s\n", c[k], k }' "$BUILD/tests-v3-full.log" | sort -k2
echo "--- 按用例统计失败数（三个已知类以外）"
awk '/^Starting tests in: / { s=$0; sub(/^Starting tests in: /,"",s) } /^!!! Test/ { c[s]++ } END { for (k in c) printf "  %6d  %s\n", c[k], k }' "$BUILD/tests-v3-full.log" \
  | grep -v -E ' (TruePeakDetector|Saturator|ColorModule) / ' | sort -k2 | cut -c1-200
