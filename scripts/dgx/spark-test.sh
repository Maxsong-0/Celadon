#!/usr/bin/env bash
# DGX Spark 本机编译、运行 celadon_tests / celadon_render。现行顺序先交 108 testbox，要 aarch64、108 不在线或超限才用它（docs/BUILDING.md）。
# 本机是 aarch64 GCC 13：已知 Color 的 NEON 快路径写死融合乘加、与标量逐位对不上（Linux ARM 不是目标平台），
# 所以「过 / 败」一律对照同机基线，只算新增失败。
#
# 用法：spark-test.sh <源码目录> build [目标 ...]          只编译（默认目标 celadon_tests celadon_render）
#       spark-test.sh <源码目录> test [celadon_tests 参数]  编译 celadon_tests 后运行（例：--only LiveProfile,SegmentLedger）
#       spark-test.sh <源码目录> run <命令 ...>             在全机测试锁里跑任意命令（例：渲染、计时）；命令里可用 $BUILD
#       spark-test.sh <源码目录> dir                        打印构建目录（同别的子命令，先把它建好；同旧版）
#       spark-test.sh <源码目录> dry <子命令 ...>           预演：打印构建目录、JUCE 路径、锁和要跑的命令，不建目录、不编、不跑
# 环境：SPARK_JOBS 并行编译数（默认 8，几个代理同时编时别抢光 20 核）；CELADON_CXX_FLAGS 追加给 CMAKE_CXX_FLAGS（构建目录按它分开）；
#       CELADON_JUCE_DIR 覆盖 JUCE 9.0.2 的位置（默认 DGX 上的 /home/maxsong/All-Project/JUCE；换了位置用另一个构建目录，
#       默认位置不进构建目录名，旧的构建目录照用）。
# 锁：同一构建目录的编译互斥（<构建目录>.lock）；测试与 run 全机串行（locks/run.lock），让计时护栏不被别的测试干扰。编译不进全机锁。
# 只在 DGX（Linux aarch64）上用：别的机器直接报错退出（Mac 上不编不测；x86 走 108 testbox）。
set -euo pipefail
[ "$#" -ge 2 ] || { sed -n 2,14p "$0"; exit 2; }
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then
  echo "spark-test.sh：只在 DGX（Linux aarch64）上用，这台是 $(uname -s) $(uname -m)；x86 走 108 testbox（docs/BUILDING.md）" >&2
  exit 2
fi
SRC="$(cd "$1" && pwd -P)"; shift
CMD="$1"; shift
DRY=0
if [ "$CMD" = dry ]; then
  [ "$#" -ge 1 ] || { echo "spark-test.sh：dry 后面要跟子命令（build / test / run / dir）" >&2; exit 2; }
  DRY=1; CMD="$1"; shift
fi
CACHE="$HOME/.cache/celadon-spark"
DEFAULT_JUCE_DIR=/home/maxsong/All-Project/JUCE
# 带项目前缀：环境里别的项目设的 JUCE_DIR 不会被悄悄用上（复查第 2 轮 R2-F4）。
JUCE_DIR="${CELADON_JUCE_DIR:-$DEFAULT_JUCE_DIR}"
FLAGS="${CELADON_CXX_FLAGS:-}"
# 构建目录按源码与编译参数分开；JUCE 换了位置也分开（CMake 只在第一次配置时记 JUCE 路径，同一个目录换了也不生效）。
# 默认位置不进键，键与旧版逐字相同，已有的构建目录照用。
if [ "$JUCE_DIR" = "$DEFAULT_JUCE_DIR" ]; then KEY_TEXT="$SRC|$FLAGS"; else KEY_TEXT="$SRC|$FLAGS|$JUCE_DIR"; fi
KEY="$(printf '%s' "$KEY_TEXT" | sha256sum | cut -c1-10)"
BUILD="$CACHE/build/$(basename "$SRC")-$KEY"
export BUILD
export PATH="$CACHE/venv/bin:$PATH"   # ninja 与 Python 依赖（numpy / scipy / soundfile / matplotlib）都在这个 venv
# 截图工具与处理器测试要的 freetype2 / fontconfig / x11 开发头：本机没装 -dev 包（装要 sudo），用 Ubuntu 官方 .deb 解到 guidev/root
# （scripts/fetch-gui-dev.sh ~/.cache/celadon-spark/guidev 就是这个布局），运行库用系统的。
GUIDEV="$CACHE/guidev/root"
GUIDEV_PC="$GUIDEV/usr/lib/aarch64-linux-gnu/pkgconfig"
export PKG_CONFIG_PATH="$GUIDEV_PC:$GUIDEV/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
JOBS="${SPARK_JOBS:-8}"

# 要编界面模块的目标（截图工具、处理器测试）没有开发头时先说清，免得 CMake 配置阶段报一串看不懂的 pkg-config 错。
needs_guidev() {
  local target
  for target in "$@"; do
    case "$target" in celadon_ui_snapshot|celadon_processor_tests) return 0 ;; esac
  done
  return 1
}

build() {
  [ -d "$JUCE_DIR/modules" ] || { echo "spark-test.sh：JUCE 不在 $JUCE_DIR（设 CELADON_JUCE_DIR 指到 JUCE 9.0.2 的根目录）" >&2; exit 2; }
  local cached
  cached="$(cached_juce_dir)"
  if [ -n "$cached" ] && [ "$cached" != "$JUCE_DIR" ]; then
    echo "spark-test.sh：$BUILD 配置时用的 JUCE 是 $cached，这次要 $JUCE_DIR；删掉这个构建目录再编" >&2
    exit 2
  fi
  if needs_guidev "$@" && [ ! -d "$GUIDEV_PC" ]; then
    echo "spark-test.sh：没有界面开发头 $GUIDEV_PC；先跑 scripts/fetch-gui-dev.sh $CACHE/guidev" >&2
    exit 2
  fi
  mkdir -p "$BUILD"
  (
    flock 9
    if [ ! -f "$BUILD/build.ninja" ]; then
      cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCELADON_JUCE_DIR="$JUCE_DIR" -DCMAKE_CXX_FLAGS="$FLAGS"
    fi
    nice -n 10 cmake --build "$BUILD" -j "$JOBS" --target "$@"
  ) 9>"$BUILD.lock"
}

# 已配置的构建目录里 CMake 记下的 JUCE 路径；还没配置时为空。
cached_juce_dir() {
  [ -f "$BUILD/CMakeCache.txt" ] || return 0
  sed -n 's/^CELADON_JUCE_DIR:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | head -1
}

# 预演：只打印，不建目录、不拿锁、不编、不跑。
preview() {
  local targets=("$@") state
  echo "源码：$SRC"
  if [ -f "$BUILD/build.ninja" ]; then state="已配置，缓存里的 JUCE：$(cached_juce_dir)"
  elif [ -d "$BUILD" ]; then state="已有、还没配置"
  else state="还没有"; fi
  echo "构建目录：$BUILD（$state）"
  echo "JUCE：$JUCE_DIR（$([ -d "$JUCE_DIR/modules" ] && echo 在 || echo 不在)）；CMAKE_CXX_FLAGS：${FLAGS:-（空）}；并行 $JOBS"
  echo "界面开发头：$GUIDEV_PC（$([ -d "$GUIDEV_PC" ] && echo 在 || echo 不在)）"
  case "$CMD" in
    dir)   echo "会打印：$BUILD" ;;
    build|test)
      # 用 if 不用「&&」：函数最后一条是不成立的 && 时返回 1，set -e 会让预演以失败退出。
      if [ "$CMD" = test ]; then targets=(celadon_tests); fi
      if [ "${#targets[@]}" -eq 0 ]; then targets=(celadon_tests celadon_render); fi
      if [ ! -f "$BUILD/build.ninja" ]; then
        echo "会配置：cmake -S $SRC -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=$JUCE_DIR -DCMAKE_CXX_FLAGS=$FLAGS"
      fi
      echo "会编译（拿 $BUILD.lock）：nice -n 10 cmake --build $BUILD -j $JOBS --target ${targets[*]}"
      if [ "$CMD" = test ]; then echo "会运行（拿全机锁 $CACHE/locks/run.lock）：$BUILD/celadon_tests $*"; fi ;;
    run)   echo "会运行（拿全机锁 $CACHE/locks/run.lock）：$*" ;;
    *)     echo "spark-test.sh：未知子命令 $CMD" >&2; exit 2 ;;
  esac
}

if [ "$DRY" -eq 1 ]; then
  preview "$@"
  exit 0
fi

# 真跑：同旧版，先建好锁目录与构建目录（dir、run 之后就能直接用 $BUILD；复查第 2 轮 R2-F3）。
mkdir -p "$CACHE/locks" "$BUILD"

case "$CMD" in
  dir)   echo "$BUILD" ;;
  build) if [ "$#" -eq 0 ]; then build celadon_tests celadon_render; else build "$@"; fi ;;
  test)  build celadon_tests
         mkdir -p "$CACHE/locks"
         ( flock 8; "$BUILD/celadon_tests" "$@" ) 8>"$CACHE/locks/run.lock" ;;
  run)   mkdir -p "$CACHE/locks"
         ( flock 8; "$@" ) 8>"$CACHE/locks/run.lock" ;;
  *)     echo "spark-test.sh：未知子命令 $CMD" >&2; exit 2 ;;
esac
