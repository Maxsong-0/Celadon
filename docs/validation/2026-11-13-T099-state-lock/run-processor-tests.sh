#!/usr/bin/env bash
# T-099 验收入口：在 testbox 的打包目录（Celadon/ = 仓库 git archive，JUCE/ = JUCE 9.0.2 的 LICENSE.md 与 modules/）里跑。
# 用法（工作目录 = 打包目录）：
#   Celadon/docs/validation/2026-11-13-T099-state-lock/run-processor-tests.sh <模式>
# 模式：
#   release      Release 编 celadon_processor_tests 与 celadon_ui_snapshot（后者只编不跑：确认改了源清单之后照常编得过），跑全部用例
#   tsan         ThreadSanitizer 编（RelWithDebInfo），跑全部用例；期望 0 份 TSan 报告、0 个失败
#   tsan-before  「修前」：复制一份源码、做 remove-state-lock.patch 那处改动（存读档不拿回调锁），TSan 编，只跑 ProcessorConcurrency；
#                期望有 TSan 报告（GlazeController / LiveProfile 上的数据竞争）。退出码 0 = 报了（对照成立），1 = 一份没报
#   bench        Release 编，跑 --bench（最坏画像的持锁时间）；要独占机器数才准
# 环境：JOBS 并行编译数（默认 nproc）；GUIDEV 界面开发头目录（默认 /cache/celadon-guidev，跨任务复用）。
# 沙盒里没有 cmake / ninja：照 T-106 / T-107 的 run-ci.sh，用 testbox 自带的 python3.12 装进 /cache（第一次装，之后复用）。
set -euo pipefail

MODE="${1:-}"
ROOT="$PWD"
SRC="$ROOT/Celadon"
JUCE_DIR="$ROOT/JUCE"
JOBS="${JOBS:-$(nproc)}"
GUIDEV="${GUIDEV:-/cache/celadon-guidev}"

[ -d "$SRC" ] && [ -d "$JUCE_DIR/modules" ] || { echo "工作目录要是打包目录（里面有 Celadon/ 与 JUCE/modules）" >&2; exit 2; }

export PYTHONUSERBASE=/cache/local
export PATH="/cache/local/bin:$PATH"
if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
  python3.12 -m pip install --user cmake ninja
fi

eval "$("$SRC/scripts/fetch-gui-dev.sh" "$GUIDEV" | grep '^export ')"

TSAN_ENV="suppressions=$SRC/tests/processor/tsan.supp halt_on_error=0 second_deadlock_stack=1"

configure_and_build() {   # <源码目录> <构建目录> <构建类型> <ON|OFF> <目标…>
  local src="$1" build="$2" type="$3" tsan="$4"; shift 4
  cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE="$type" -DCELADON_TSAN="$tsan" -DCELADON_JUCE_DIR="$JUCE_DIR"
  grep -q celadon_processor_tests "$build/build.ninja" || { echo "没生成 celadon_processor_tests：界面开发头没找到" >&2; exit 1; }
  cmake --build "$build" -j "$JOBS" --target "$@"
}

run_tsan() {   # <可执行文件> <参数…>：setarch -R 关地址随机化（新内核上 TSan 的影子内存会撞映射）
  local exe="$1"; shift
  TSAN_OPTIONS="$TSAN_ENV" setarch "$(uname -m)" -R "$exe" "$@"
}

case "$MODE" in
  release)
    configure_and_build "$SRC" "$ROOT/build/processor-release" Release OFF celadon_processor_tests celadon_ui_snapshot
    "$ROOT/build/processor-release/celadon_processor_tests"
    ;;
  tsan)
    configure_and_build "$SRC" "$ROOT/build/processor-tsan" RelWithDebInfo ON celadon_processor_tests
    log="$ROOT/build/processor-tsan.log"
    status=0
    run_tsan "$ROOT/build/processor-tsan/celadon_processor_tests" 2>&1 | tee "$log" || status=$?
    echo "TSan 报告份数：$(grep -c '^WARNING: ThreadSanitizer' "$log" || true)"
    exit "$status"
    ;;
  tsan-before)
    before="$ROOT/Celadon-before"
    rm -rf "$before"
    cp -a "$SRC" "$before"
    # 与 remove-state-lock.patch 同一处改动（沙盒里不一定有 patch / git，用 sed 改、再核对正好改了两处）。
    file="$before/Source/PluginProcessor.cpp"
    [ "$(grep -c 'const juce::ScopedLock audioStopped (getCallbackLock());' "$file")" = 2 ] || { echo "PluginProcessor.cpp 里的两处锁对不上，修前对照做不了" >&2; exit 1; }
    sed -i 's#^\( *\)const juce::ScopedLock audioStopped (getCallbackLock());#\1// T-099 修前演示：存读档不拿回调锁#' "$file"
    [ "$(grep -c 'ScopedLock audioStopped' "$file")" = 0 ] || { echo "去锁没去干净" >&2; exit 1; }
    configure_and_build "$before" "$ROOT/build/processor-tsan-before" RelWithDebInfo ON celadon_processor_tests
    log="$ROOT/build/processor-tsan-before.log"
    run_tsan "$ROOT/build/processor-tsan-before/celadon_processor_tests" --only ProcessorConcurrency 2>&1 | tee "$log" || true
    reports="$(grep -c '^WARNING: ThreadSanitizer' "$log" || true)"
    echo "修前 TSan 报告份数：$reports（期望 > 0）"
    [ "${reports:-0}" -gt 0 ]
    ;;
  bench)
    configure_and_build "$SRC" "$ROOT/build/processor-release" Release OFF celadon_processor_tests
    "$ROOT/build/processor-release/celadon_processor_tests" --bench
    ;;
  *)
    sed -n 2,14p "$0" >&2
    exit 2
    ;;
esac
