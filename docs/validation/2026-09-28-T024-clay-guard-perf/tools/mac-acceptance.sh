#!/bin/bash
# T-024 · Mac 整插件性能验收（REPORT 第 7 节）：搭三棵树、编、按轮交替跑 grid 与 modules、出对比表。
#
# 为什么是三棵树（2026-10-04 改）：main 现在是 0.1.8.8，v0.1.6.2 之后除了修坯，GLAZE、分析器、限制器、段账本等都改过，
# 整插件 grid 对 v0.1.6.2 的差已经不全是修坯了。所以：
#   base = v0.1.6.2（Celadon-releases 里的源码快照 + T-021 的 baseline-bench.mm，搭法同 T-021 REPORT 第 6 节）
#   t024 = 提交 c362f74（= v0.1.7.1 + T-024；v0.1.6.2 → 它只动了修坯，与 T-021 同口径）→ t024 − base 就是修坯的整插件增量
#   head = 本分支（= main 代码，只多了文档与本脚本）→ head − base 是今天整插件的总增量（含非修坯的改动，只作参考）；
#          修坯自 T-024 以来又变了多少（T-032 按段、T-043 混合损失测量、T-044），看 modules 模式 clay 一列 head 对 t024。
#
# 用法（在 head 树里跑，head 树由 Max 先 git worktree add 出来，见 REPORT 第 7 节）：
#   mac-acceptance.sh setup     # 建 t024 worktree、拷 base 树、放 JUCE 链接（已有就跳过）
#   mac-acceptance.sh build     # 三棵树各编一次 celadon_bench（短跑一格，之后的正式轮不再编译）
#   mac-acceptance.sh run       # 正式轮：grid 两轮 base→t024→head 交替，modules 两轮 t024→head 交替；全程记编译活动
#   mac-acceptance.sh compare   # 出对比表（也可以单独重跑）
#   mac-acceptance.sh all       # 以上四步依次
# 可用环境变量改路径：WORK、CELADON_REPO、BASELINE_SRC、JUCE_DIR、LOG（默认见下）。
# 已经建好的树、链接会跳过，所以中途失败后修好再跑 all 即可。
#
# 注意 macOS 自带的 bash 3.2：「$名字」后面直接跟中文（全角）时，会把中文的首字节也当成变量名，set -u 下直接退出
# （2026-10-05 Max 实跑撞上「t? unbound variable」，同 d3ddb35）。本文件里变量后面紧跟非 ASCII 字符的一律写 ${名字}。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
VALDIR="$(cd "$HERE/.." && pwd -P)"
HEAD_TREE="$(cd "$VALDIR/../../.." && pwd -P)"

WORK="${WORK:-$(cd "$HEAD_TREE/.." && pwd -P)}"
CELADON_REPO="${CELADON_REPO:-/Users/maxsong/All-Project/Celadon}"
BASELINE_SRC="${BASELINE_SRC:-/Users/maxsong/All-Project/Celadon-releases/source-baseline-2026-09-26-v0.1.6.2}"
JUCE_DIR="${JUCE_DIR:-/Users/maxsong/All-Project/JUCE}"
T024_COMMIT="c362f74"
T021_DIR="$HEAD_TREE/docs/validation/2026-09-28-T021-perf-acceptance"
LOG="${LOG:-$VALDIR/mac-$(date +%Y-%m-%d)}"

if [ -z "${WORK_SET_BY_USER:-}" ] && [ "$(basename "$HEAD_TREE")" != head ]; then
  echo "请在 <WORK>/head 这棵 worktree 里跑本脚本（现在是 ${HEAD_TREE}）；确实要放别处就设 WORK=… WORK_SET_BY_USER=1"
  exit 1
fi

BASE_TREE="$WORK/base"
T024_TREE="$WORK/t024"

# 三棵树的 celadon_bench 各编到 WORK 下自己的目录（T-021 给基线树也是显式指定的；v0.1.6.2 的 perf-local.sh 同样认这个变量）。
build_dir_of() { echo "$WORK/build-$1"; }

GRID_ARGS=(grid all 44100,48000,96000,192000 32,64,512 20 10 5)
MODULES_ARGS=(modules all 48000,192000 32,512 20 10 5)

say() { printf '\n== %s  %s\n' "$(date +%H:%M:%S)" "$*"; }

tree_of() {
  case "$1" in
    base) echo "$BASE_TREE" ;;
    t024) echo "$T024_TREE" ;;
    head) echo "$HEAD_TREE" ;;
  esac
}

do_setup() {
  say "WORK=$WORK  head=$HEAD_TREE"
  [ -d "$JUCE_DIR/modules" ] || { echo "找不到 JUCE：$JUCE_DIR/modules"; exit 1; }
  [ -d "$BASELINE_SRC/Source" ] || { echo "找不到 v0.1.6.2 源码快照：$BASELINE_SRC"; exit 1; }

  # 三棵树同级放一个 JUCE 链接（tools/perf/CMakeLists 找 <树>/../JUCE/modules）。
  if [ ! -e "$WORK/JUCE" ]; then
    ln -s "$JUCE_DIR" "$WORK/JUCE"
  fi

  if [ ! -d "$T024_TREE" ]; then
    git -C "$CELADON_REPO" worktree add --detach "$T024_TREE" "$T024_COMMIT"
  fi

  if [ ! -d "$BASE_TREE" ]; then
    # 同 T-021 REPORT 第 6 节：v0.1.6.2 的 Source/、tools/perf/、scripts/，bench.mm 换成 T-021 的 baseline-bench.mm；
    # JuceLibraryCode 取 v0.1.7.1 的（T-021 当时用的就是它；与 DSP 开销无关）。
    mkdir -p "$BASE_TREE/tools"
    cp -R "$BASELINE_SRC/Source" "$BASE_TREE/Source"
    cp -R "$BASELINE_SRC/tools/perf" "$BASE_TREE/tools/perf"
    cp -R "$BASELINE_SRC/scripts" "$BASE_TREE/scripts"
    cp "$T021_DIR/baseline-bench.mm" "$BASE_TREE/tools/perf/bench.mm"
    git -C "$CELADON_REPO" archive "$T024_COMMIT" JuceLibraryCode | tar -x -C "$BASE_TREE"
  fi

  say "三棵树："
  echo "  base  $BASE_TREE"
  echo "  t024  $T024_TREE  ($(git -C "$T024_TREE" rev-parse --short HEAD))"
  echo "  head  $HEAD_TREE  ($(git -C "$HEAD_TREE" rev-parse --short HEAD))"
}

do_build() {
  for t in base t024 head; do
    say "编 ${t}（短跑一格，只为编译）"
    CELADON_PERF_BUILD_DIR="$(build_dir_of "$t")" "$(tree_of "$t")/scripts/perf-local.sh" XcodeRelease grid default 48000 512 1 1 1
  done
}

# 一轮：<树名> <日志名> <bench 参数…>
run_one() {
  local name="$1" logname="$2" rc=0
  shift 2
  local tree logfile
  tree="$(tree_of "$name")"
  logfile="$LOG/${logname}.log"
  say "${name} → ${logname}"
  # 花括号不开子 shell，rc 带得出来；perf-local 失败时照样写 END，再把日志末尾打到终端并停下。
  {
    echo "START $(date +%H:%M:%S)"
    echo "TREE ${name} ${tree}"
    if [ "$name" != base ]; then echo "COMMIT $(git -C "$tree" rev-parse HEAD)"; fi
    CELADON_PERF_BUILD_DIR="$(build_dir_of "$name")" "$tree/scripts/perf-local.sh" XcodeRelease "$@" || rc=$?
    echo "END $(date +%H:%M:%S) rc=${rc}"
  } > "$logfile" 2>&1
  grep -E '^POWER|^END' "$logfile" || true
  if [ "$rc" -ne 0 ]; then
    say "${logname} 失败（退出码 ${rc}），日志末尾："
    tail -n 30 "$logfile"
    exit "$rc"
  fi
}

WATCHER=""
stop_watcher() {
  if [ -n "$WATCHER" ]; then
    kill "$WATCHER" 2>/dev/null || true
    WATCHER=""
  fi
}

do_run() {
  mkdir -p "$LOG"
  say "日志目录 $LOG"
  say "开跑前确认：接电源、不是低电量模式、本机没有别的编译、TASKS ② 已登记「本机性能基准」占用"

  # 每 2 s 记一次编译进程（T-021 同做法）；有内容的那一轮作废重跑。perf-local 自己的增量检查不会起 clang。
  # 没有作业控制时后台进程不理 Ctrl-C，所以退出、Ctrl-C 都要显式停掉它；disown 免得停它时 bash 打一行「Terminated」。
  ( while true; do
      p="$(pgrep -lf 'xcodebuild|swift-frontend|cargo|/clang |clang\+\+|ld64' 2>/dev/null || true)"
      [ -n "$p" ] && printf '%s\n%s\n' "$(date +%H:%M:%S)" "$p"
      sleep 2
    done ) > "$LOG/compile-activity.log" 2>&1 &
  WATCHER=$!
  disown "$WATCHER" 2>/dev/null || true
  trap stop_watcher EXIT
  trap 'stop_watcher; exit 130' INT TERM

  for r in 1 2; do
    for t in base t024 head; do
      run_one "$t" "$t-grid-$r" "${GRID_ARGS[@]}"
    done
  done
  for r in 1 2; do
    for t in t024 head; do
      run_one "$t" "$t-modules-$r" "${MODULES_ARGS[@]}"
    done
  done

  stop_watcher
  if [ -s "$LOG/compile-activity.log" ]; then
    say "注意：compile-activity.log 不是空的，期间有编译，看时间对上哪一轮、那一轮重跑"
  else
    say "期间没有编译（compile-activity.log 为空）"
  fi
}

do_compare() {
  PYTHONIOENCODING=utf-8 python3 "$HERE/compare_t024.py" "$LOG" "$T021_DIR/modules-after-T020.log" | tee "$LOG/compare.txt"
  say "对比表：${LOG}/compare.txt"
  if [ -s "$LOG/compile-activity.log" ]; then
    echo "编译活动：${LOG}/compile-activity.log（不是空的，一并贴回）"
  else
    echo "编译活动：没有"
  fi
}

case "${1:-}" in
  setup)   do_setup ;;
  build)   do_build ;;
  run)     do_run ;;
  compare) do_compare ;;
  all)     do_setup; do_build; do_run; do_compare ;;
  *) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 1 ;;
esac
