#!/usr/bin/env bash
# DGX（max-spark，aarch64）上跑可移植 CPU 基准 celadon_bench_portable 的入口（T-096f；计划 §13 Q10 / Q12）。
# 做的事：记录调速器 / 频率 / 负载 / 核号 / 二进制指纹 → 另起编译活动观察进程（每 2 s 一次）→ taskset 绑核，基线与当前二进制
#         交替各 N 轮 → 结果落 <输出目录>/run-<UTC 时刻>/。观察日志非空（或一分钟负载超上限）= 本次作废：整个目录改名
#         contaminated-<UTC 时刻>/，退出码 3，要重跑（vault 踩坑-本机基准与他人编译撞车）。
# 必须在全机锁里跑（spark-test.sh 的锁不管编译，所以才要观察进程）：
#   ~/.cache/celadon-spark/bin/spark-test.sh <源码目录> run bash <源码目录>/scripts/perf-dgx.sh --base <基线二进制> --out <目录> -- <基准参数…>
# 用法：
#   scripts/perf-dgx.sh [--dry-run] [--base <基线二进制>] [--cur <当前二进制>] --out <输出目录>
#                       [--rounds N] [--core N] [--no-lock-check] [-- <celadon_bench_portable 参数…>]
#   --cur    默认 $BUILD/celadon_bench_portable（spark-test.sh run 里有 $BUILD）。
#   --base   不给就只跑当前（每轮一次）；给了就按 基线 1 → 当前 1 → 基线 2 → 当前 2 … 交替。
#   --rounds 默认 2（计划 §13 Q10：交替 ≥ 2 轮）。--core 默认 5（Cortex-X925，T-058 先例）。
#   --dry-run 只检查并打印计划（机器状态只读），不建目录、不起观察进程、不跑基准。
#   -- 之后的参数原样交给基准（--out 由本脚本给，不许再传）。对比：python3 tools/perf/compare_perf.py <run 目录>。
# 退出码：0 干净跑完；1 基础设施错（缺二进制 / 缺命令 / 基准返回非 0 / 不在锁里 / 观察进程没在跑）；2 用法错；3 本次作废。
# 只写自己的输出目录，不改别的文件，不 kill 任何进程（观察进程是自己起的，靠停止标记自己退出）。
set -euo pipefail

# ---- 常量（集中一处）----
DEFAULT_CORE=5                 # Cortex-X925 3.9 GHz（docs/validation/2026-10-04-T058-spectrum-peak-off/round2-bench-before.txt）
DEFAULT_ROUNDS=2               # 基线与当前交替各 ≥ 2 轮
OBSERVE_INTERVAL_SECONDS=2     # 编译活动观察间隔
LOAD1_MAX=""                   # 一分钟负载上限：空 = 只记录不判；起点值由主会话首跑后填（计划 §13 Q12「事先定的阈值」）
EXPECTED_GOVERNOR=performance  # 不是它只在 VERDICT 里提示，不作废（要不要作废待定）
BENCH_NAME=celadon_bench_portable
# 观察的进程名（ps 的 comm，最长 15 字符）：编译器驱动、GCC 真正干活的 cc1plus、链接器、构建系统、别的会话的测试与渲染。
COMPILE_PROCESS_NAMES="xcodebuild clang clang++ gcc g++ gcc-13 g++-13 c++ cc cc1 cc1plus collect2 as ld ld.bfd ld.gold ld.lld mold ninja cmake make cargo rustc celadon_tests celadon_render"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

usage() {   # 打印本文件头注释（第 2 ～ 17 行）
  sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die_usage() { echo "用法错：$*" >&2; usage >&2; exit 2; }
die() { echo "错误：$*" >&2; exit 1; }

# ---- 参数 ----
DRY_RUN=0
NO_LOCK_CHECK=0
BASE_BIN=""
CUR_BIN=""
OUT=""
ROUNDS=$DEFAULT_ROUNDS
CORE=$DEFAULT_CORE
BENCH_ARGS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --no-lock-check) NO_LOCK_CHECK=1; shift ;;
    --base) [ "$#" -ge 2 ] || die_usage "--base 缺值"; BASE_BIN=$2; shift 2 ;;
    --cur) [ "$#" -ge 2 ] || die_usage "--cur 缺值"; CUR_BIN=$2; shift 2 ;;
    --out) [ "$#" -ge 2 ] || die_usage "--out 缺值"; OUT=$2; shift 2 ;;
    --rounds) [ "$#" -ge 2 ] || die_usage "--rounds 缺值"; ROUNDS=$2; shift 2 ;;
    --core) [ "$#" -ge 2 ] || die_usage "--core 缺值"; CORE=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; BENCH_ARGS=("$@"); break ;;
    *) die_usage "不认识的参数：$1" ;;
  esac
done

[ -n "$OUT" ] || die_usage "要给 --out <输出目录>"
case "$ROUNDS" in ''|*[!0-9]*) die_usage "--rounds 要是正整数：$ROUNDS" ;; esac
[ "$ROUNDS" -ge 1 ] || die_usage "--rounds 要 ≥ 1"
case "$CORE" in ''|*[!0-9]*) die_usage "--core 要是非负整数：$CORE" ;; esac
for arg in ${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"}; do
  case "$arg" in
    --out|--list|--selftest|--help|-h) die_usage "基准参数里不能有 $arg（--out 由本脚本给）" ;;
  esac
done

if [ -z "$CUR_BIN" ]; then
  [ -n "${BUILD:-}" ] || die_usage "没给 --cur，也没有 \$BUILD（不在 spark-test.sh run 里）"
  CUR_BIN="$BUILD/$BENCH_NAME"
fi

# ---- 只读的机器状态 ----
cpu_file() { cat "/sys/devices/system/cpu/cpu$CORE/cpufreq/$1" 2>/dev/null || echo unknown; }
read_load1() { awk '{print $1}' /proc/loadavg 2>/dev/null || echo unknown; }
cpu_part() { awk -v c="$CORE" '/^processor/{p=$3} p==c && /^CPU part/{print $4; exit}' /proc/cpuinfo 2>/dev/null || true; }
sha_of() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1 || echo unknown; }

check_binary() {   # check_binary <名字> <路径>
  [ -f "$2" ] || die "$1 二进制不存在：$2"
  [ -x "$2" ] || die "$1 二进制不可执行：$2"
}

# ---- 检查（预演与真跑都做）----
for cmd in taskset ps awk sha256sum; do
  command -v "$cmd" >/dev/null 2>&1 || die "缺命令 $cmd"
done
check_binary 当前 "$CUR_BIN"
[ -z "$BASE_BIN" ] || check_binary 基线 "$BASE_BIN"
[ -d "/sys/devices/system/cpu/cpu$CORE" ] || die "没有 CPU $CORE"

LABELS=(cur)
[ -z "$BASE_BIN" ] || LABELS=(base cur)
binary_for() { if [ "$1" = base ]; then echo "$BASE_BIN"; else echo "$CUR_BIN"; fi; }

STAMP="$(date -u +%Y%m%d-%H%M%SZ)"
RUN="$OUT/run-$STAMP"

bench_command() {   # bench_command <标签> <轮次>：打印将要执行的命令（预演用）
  local bin; bin="$(binary_for "$1")"
  printf 'taskset -c %q %q' "$CORE" "$bin"
  for arg in ${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"}; do printf ' %q' "$arg"; done
  printf ' --out %q > %q 2>&1\n' "$RUN/$1-$2.tsv" "$RUN/$1-$2.log"
}

if [ "$DRY_RUN" = 1 ]; then
  echo "预演（不建目录、不起观察进程、不跑基准）"
  echo "  核 $CORE：CPU part $(cpu_part)，调速器 $(cpu_file scaling_governor)，频率 $(cpu_file scaling_cur_freq) / $(cpu_file cpuinfo_max_freq) kHz"
  echo "  一分钟负载 $(read_load1)；上限 ${LOAD1_MAX:-（未定，只记录）}"
  echo "  全机锁：$([ -n "${BUILD:-}" ] && echo "在 spark-test.sh run 里（\$BUILD=$BUILD）" || echo "不在 spark-test.sh run 里——真跑会拒绝（除非 --no-lock-check）")"
  echo "  当前二进制 $CUR_BIN（sha256 $(sha_of "$CUR_BIN")）"
  [ -z "$BASE_BIN" ] || echo "  基线二进制 $BASE_BIN（sha256 $(sha_of "$BASE_BIN")）"
  echo "  将建 $RUN/，观察进程每 ${OBSERVE_INTERVAL_SECONDS} s 查一次：$COMPILE_PROCESS_NAMES"
  echo "  顺序："
  for round in $(seq 1 "$ROUNDS"); do
    for label in "${LABELS[@]}"; do
      printf '    %s ' "$label-$round:"; bench_command "$label" "$round"
    done
  done
  exit 0
fi

if [ -z "${BUILD:-}" ] && [ "$NO_LOCK_CHECK" != 1 ]; then
  die "不在 spark-test.sh run 里（没有 \$BUILD），全机锁没拿到；要么放进 spark-test.sh <源码目录> run，要么明知故犯加 --no-lock-check"
fi

# ---- 真跑 ----
mkdir -p "$RUN"
STOP_FILE="$RUN/.observer-stop"
OBS_PID=""

record_machine() {
  echo "date_utc=$(date -u +%FT%TZ)"
  echo "date_local=$(date +%FT%T%z)"
  echo "host=$(hostname)"
  echo "uname=$(uname -a)"
  echo "nproc=$(nproc 2>/dev/null || echo unknown)"
  echo "core=$CORE"
  echo "core_cpu_part=$(cpu_part)"
  echo "core_governor=$(cpu_file scaling_governor)"
  echo "core_cur_freq_khz=$(cpu_file scaling_cur_freq)"
  echo "core_max_freq_khz=$(cpu_file cpuinfo_max_freq)"
  echo "loadavg=$(cat /proc/loadavg 2>/dev/null || echo unknown)"
  echo "load1_max=${LOAD1_MAX:-unset}"
  echo "rounds=$ROUNDS"
  echo "labels=${LABELS[*]}"
  echo "cur_bin=$CUR_BIN"
  echo "cur_sha256=$(sha_of "$CUR_BIN")"
  if [ -n "$BASE_BIN" ]; then
    echo "base_bin=$BASE_BIN"
    echo "base_sha256=$(sha_of "$BASE_BIN")"
  fi
  printf 'bench_args='
  for arg in ${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"}; do printf '%q ' "$arg"; done
  echo
  echo "spark_build=${BUILD:-unset}"
  echo "script_root=$ROOT"
  echo "script_git_head=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo 'unknown（源码目录不是 git 仓库：提交号由主会话记进报告）')"
  if command -v lscpu >/dev/null 2>&1; then
    echo "---- lscpu ----"
    lscpu
  fi
}

observe_compiles() {   # observe_compiles <日志> <停止标记> <心跳文件>：日志只在看到编译类进程时才有内容
  local log=$1 stop=$2 ticks_file=$3 ticks=0 now
  : > "$log"
  while [ ! -e "$stop" ]; do
    now="$(date -u +%FT%TZ)"
    ps -eo pid=,etimes=,comm=,args= 2>/dev/null \
      | awk -v names="$COMPILE_PROCESS_NAMES" -v now="$now" '
          BEGIN { n = split(names, list, " "); for (i = 1; i <= n; i++) watch[list[i]] = 1 }
          ($3 in watch) { print now "\t" $0 }' >> "$log" || true
    ticks=$((ticks + 1))
    echo "$ticks" > "$ticks_file"
    sleep "$OBSERVE_INTERVAL_SECONDS"
  done
}

stop_observer() {
  if [ -n "$OBS_PID" ]; then
    : > "$STOP_FILE"
    wait "$OBS_PID" 2>/dev/null || true
    rm -f "$STOP_FILE"
    OBS_PID=""
  fi
}
trap stop_observer EXIT
trap 'exit 130' INT TERM

record_machine > "$RUN/machine.txt"
printf 'label\tround\tstart_utc\tend_utc\tload1_before\tload1_after\texit\ttsv\n' > "$RUN/runs.tsv"

observe_compiles "$RUN/compile-activity.log" "$STOP_FILE" "$RUN/observer.ticks" &
OBS_PID=$!

LOAD_REASONS=()

run_one() {   # run_one <标签> <轮次>
  local label=$1 round=$2 bin start end load_before load_after status
  bin="$(binary_for "$label")"
  load_before="$(read_load1)"
  if [ -n "$LOAD1_MAX" ] && awk -v a="$load_before" -v b="$LOAD1_MAX" 'BEGIN { exit !(a + 0 > b + 0) }'; then
    LOAD_REASONS+=("$label-$round 开跑前一分钟负载 $load_before > $LOAD1_MAX")
  fi
  start="$(date -u +%FT%TZ)"
  echo "== $label-$round 开始 $start（负载 $load_before）"
  set +e
  taskset -c "$CORE" "$bin" ${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"} --out "$RUN/$label-$round.tsv" > "$RUN/$label-$round.log" 2>&1
  status=$?
  set -e
  end="$(date -u +%FT%TZ)"
  load_after="$(read_load1)"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$round" "$start" "$end" "$load_before" "$load_after" "$status" "$label-$round.tsv" >> "$RUN/runs.tsv"
  if [ "$status" -ne 0 ]; then
    # 半截目录不能被当成干净结果：先写明失败再退出（观察进程由 EXIT 陷阱停）。
    printf 'verdict=failed\nreason=%s 基准返回 %s（见 %s.log）\n' "$label-$round" "$status" "$label-$round" > "$RUN/VERDICT.txt"
    echo "错误：$label-$round 基准返回 $status，见 $RUN/$label-$round.log" >&2
    exit 1
  fi
}

for round in $(seq 1 "$ROUNDS"); do
  for label in "${LABELS[@]}"; do
    run_one "$label" "$round"
  done
done

kill -0 "$OBS_PID" 2>/dev/null && OBSERVER_ALIVE=1 || OBSERVER_ALIVE=0   # 只查自己的子进程还在不在，不发信号
stop_observer
TICKS="$(cat "$RUN/observer.ticks" 2>/dev/null || echo 0)"

REASONS=()
[ -s "$RUN/compile-activity.log" ] && REASONS+=("跑基准期间有编译类进程（compile-activity.log 非空，$(wc -l < "$RUN/compile-activity.log") 行）")
[ "$OBSERVER_ALIVE" = 1 ] && [ "$TICKS" -ge 1 ] || REASONS+=("观察进程没在跑（心跳 $TICKS 次）：干不干净无法确认")
REASONS+=(${LOAD_REASONS[@]+"${LOAD_REASONS[@]}"})

NOTES=()
GOVERNOR="$(cpu_file scaling_governor)"
[ "$GOVERNOR" = "$EXPECTED_GOVERNOR" ] || NOTES+=("核 $CORE 调速器是 $GOVERNOR，不是 $EXPECTED_GOVERNOR")

{
  if [ "${#REASONS[@]}" -eq 0 ]; then echo "verdict=clean"; else echo "verdict=contaminated"; fi
  for r in ${REASONS[@]+"${REASONS[@]}"}; do echo "reason=$r"; done
  for n in ${NOTES[@]+"${NOTES[@]}"}; do echo "note=$n"; done
  echo "observer_ticks=$TICKS"
  echo "finished_utc=$(date -u +%FT%TZ)"
} > "$RUN/VERDICT.txt"

if [ "${#REASONS[@]}" -ne 0 ]; then
  BAD="$OUT/contaminated-$STAMP"
  mv "$RUN" "$BAD"
  echo "本次作废（结果不算，要重跑）：" >&2
  printf '  %s\n' "${REASONS[@]}" >&2
  echo "日志留在 $BAD/" >&2
  exit 3
fi

echo "干净跑完：$RUN/"
[ "${#NOTES[@]}" -eq 0 ] || printf '提示：%s\n' "${NOTES[@]}"
if [ -n "$BASE_BIN" ]; then
  echo "对比：python3 $ROOT/tools/perf/compare_perf.py $RUN"
fi
exit 0
