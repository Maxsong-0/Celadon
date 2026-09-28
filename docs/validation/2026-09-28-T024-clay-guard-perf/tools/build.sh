#!/usr/bin/env bash
# T-024：编「新旧 ClayModule 逐位对照」与「修坯一级 x86 微基准」两个小工具（Linux 容器 / 服务器用，不进插件）。
#
# 新 = 工作树的 Source/dsp/ClayModule.* 与 LinearPhaseLowpass.*；
# 旧 = 提交 $REF_COMMIT（默认 d25f6da = T-024 改前）里的同名文件，类名换成 ClayModuleRef / LinearPhaseLowpassRef 后并排编进同一个程序。
#
# 用法：tools/build.sh <输出目录> [配置...]
#   配置：gcc      GCC -O3，-ffp-contract=off（与 CMakeLists 的 Linux 测试构建相同）
#         gcc-v3   同上 + -march=x86-64-v3（目标有 FMA；GCC 仍关掉乘加融合）
#         clang-v3 clang -O3 -march=x86-64-v3，乘加融合保持 clang 默认（on：只合并同一表达式）——x86 上最接近
#                  Xcode 在 Apple Silicon 上的正式构建（有 FMA、clang 默认 on）
#         clang    clang -O3，x86-64 基线（无 FMA）
#   默认四个都编。需要 JUCE 9.0.2 在 $CELADON_JUCE_DIR（默认仓库同级的 JUCE），与 libceladon_juce.a（用 CMake 测试构建编好的，
#   见 $CELADON_JUCE_LIB_GCC / $CELADON_JUCE_LIB_CLANG；两个工具只用到 JUCE 的头文件内联部分与 juce_core 的少量符号）。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../../../.." && pwd -P)"
OUT="${1:?用法：build.sh <输出目录> [gcc|gcc-v3|clang-v3|clang ...]}"
shift
CONFIGS=("$@")
[ "${#CONFIGS[@]}" -eq 0 ] && CONFIGS=(gcc gcc-v3 clang-v3 clang)

REF_COMMIT="${REF_COMMIT:-d25f6da}"
JUCE_DIR="${CELADON_JUCE_DIR:-$ROOT/../JUCE}"
JUCE_LIB_GCC="${CELADON_JUCE_LIB_GCC:?设 CELADON_JUCE_LIB_GCC=GCC 编的 libceladon_juce.a}"
JUCE_LIB_CLANG="${CELADON_JUCE_LIB_CLANG:-$JUCE_LIB_GCC}"

mkdir -p "$OUT/ref"

# 旧实现：从改前提交取出，类名换掉（整词），头文件名跟着换；LinearPhaseLowpass 也用旧的，两边完全独立。
for name in ClayModule LinearPhaseLowpass; do
  for ext in h cpp; do
    git -C "$ROOT" show "$REF_COMMIT:Source/dsp/$name.$ext" \
      | sed -e 's/\bClayModule\b/ClayModuleRef/g' -e 's/\bLinearPhaseLowpass\b/LinearPhaseLowpassRef/g' \
      > "$OUT/ref/${name}Ref.$ext"
  done
done
git -C "$ROOT" rev-parse "$REF_COMMIT" > "$OUT/ref/COMMIT"

DEFINES=(-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0
         -DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1
         -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 -DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DNDEBUG=1)
INCLUDES=(-I"$ROOT/Source/dsp" -I"$OUT/ref" -isystem "$JUCE_DIR/modules")
SOURCES=("$ROOT/Source/dsp/ClayModule.cpp" "$ROOT/Source/dsp/LinearPhaseLowpass.cpp"
         "$OUT/ref/ClayModuleRef.cpp" "$OUT/ref/LinearPhaseLowpassRef.cpp")

for config in "${CONFIGS[@]}"; do
  case "$config" in
    gcc)      CXX=g++;     FLAGS=(-O3 -ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1);                    LIB="$JUCE_LIB_GCC" ;;
    gcc-v3)   CXX=g++;     FLAGS=(-O3 -march=x86-64-v3 -ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1);   LIB="$JUCE_LIB_GCC" ;;
    clang-v3) CXX=clang++; FLAGS=(-O3 -march=x86-64-v3);                                                  LIB="$JUCE_LIB_CLANG" ;;
    clang)    CXX=clang++; FLAGS=(-O3);                                                                   LIB="$JUCE_LIB_CLANG" ;;
    *) echo "未知配置 $config" >&2; exit 2 ;;
  esac

  for tool in guard_compare guard_bench; do
    "$CXX" -std=gnu++17 "${FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" -Wall -Wextra \
      "$HERE/$tool.cpp" "${SOURCES[@]}" "$LIB" -lpthread -ldl -lrt -o "$OUT/$tool-$config"
  done
  echo "built $config"
done
