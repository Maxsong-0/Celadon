#!/usr/bin/env bash
# T-116：108（x86_64）上照 .github/workflows/ci.yml 编 celadon_tests 跑全套；
# 另外比对 ColorModule.cpp.o 改前改后的 sha256（x86_64 上改动全在 CELADON_COLOR_NEON 宏里，目标码应当逐字节相同）。
# 用法：JOBS=24 ./run-ci.sh <base|x86-64-v3>
# 打包目录：Celadon/（git archive 出来的被测提交）、JUCE/（LICENSE.md + modules/）、ColorModule.main.cpp（main 上的原文件）。
set -euo pipefail

tier="${1:?用法：run-ci.sh <base|x86-64-v3>}"
jobs="${JOBS:-8}"
root="$PWD"

# 沙盒里没有 cmake / ninja：用 testbox 自带的 python3.12 装进 /cache，下次复用。
export PYTHONUSERBASE=/cache/local
export PATH=/cache/local/bin:$PATH
if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
    python3.12 -m pip install --user --quiet cmake ninja
fi

flags=""
if [ "$tier" = "x86-64-v3" ]; then flags="-march=x86-64-v3"; fi

build="$root/build/$tier"
mkdir -p "$build"
cmake -S "$root/Celadon" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=$flags" \
      "-DCELADON_JUCE_DIR=$root/JUCE" > "$build/configure.log" 2>&1 || { tail -40 "$build/configure.log"; exit 1; }

# 目标码比对：同一个构建目录、同一个源码路径，只换 ColorModule.cpp 的内容（路径不变，__FILE__ 之类不会带来差别）。
object="$(ninja -C "$build" -t targets all | cut -d: -f1 | grep -E '/ColorModule\.cpp\.o$' | grep celadon_tests | head -1 || true)"
if [ -z "$object" ]; then
    object="$(ninja -C "$build" -t targets all | cut -d: -f1 | grep -E '/ColorModule\.cpp\.o$' | head -1)"
fi
echo "比对的目标文件：$object"

source_file="$root/Celadon/Source/dsp/ColorModule.cpp"
cp "$source_file" "$build/ColorModule.new.cpp"

cp "$root/ColorModule.main.cpp" "$source_file"
ninja -C "$build" -j "$jobs" "$object" > "$build/object-before.log" 2>&1 || { tail -30 "$build/object-before.log"; exit 1; }
before="$(sha256sum "$build/$object" | cut -d' ' -f1)"
cp "$build/$object" "$build/ColorModule.before.o"

sleep 1   # 让换回来的源文件修改时间一定晚于刚编出的目标文件
cp "$build/ColorModule.new.cpp" "$source_file"
ninja -C "$build" -j "$jobs" "$object" > "$build/object-after.log" 2>&1 || { tail -30 "$build/object-after.log"; exit 1; }
after="$(sha256sum "$build/$object" | cut -d' ' -f1)"

echo "ColorModule.cpp.o（$tier）改前 sha256 = $before"
echo "ColorModule.cpp.o（$tier）改后 sha256 = $after"
object_status=0
if [ "$before" = "$after" ]; then
    echo "目标码比对（$tier）：相同"
else
    echo "目标码比对（$tier）：不同"
    object_status=1
    objdump -d "$build/ColorModule.before.o" > "$build/before.dis" 2>/dev/null || true
    objdump -d "$build/$object" > "$build/after.dis" 2>/dev/null || true
    echo "反汇编差异行数：$(diff "$build/before.dis" "$build/after.dis" | grep -c '^[<>]' || true)"
fi

# 全套：与 ci.yml 相同的目标与跑法。
cmake --build "$build" --target celadon_tests -j "$jobs" > "$build/build.log" 2>&1 || { tail -40 "$build/build.log"; exit 1; }
grep -c -i 'warning:' "$build/build.log" | sed 's/^/编译警告行数：/' || true

set +e
"$build/celadon_tests" > "$build/tests.log" 2>&1
test_status=$?
set -e

sed -n 's/^==== \(celadon_tests: .*\) ====$/结果（'"$tier"'）：\1/p' "$build/tests.log"
awk '/^Starting tests in: /{c=$0;s=0} /^!!! Test / && n<100 {if(!s){print c;s=1} print "    " $0; n++}' "$build/tests.log"
echo "celadon_tests（$tier）退出码 $test_status"

[ "$test_status" -eq 0 ] && [ "$object_status" -eq 0 ]
