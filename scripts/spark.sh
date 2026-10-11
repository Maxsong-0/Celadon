#!/usr/bin/env bash
# 把工作树（或某个提交的干净树）送到 DGX Spark，并在那边调 spark-test.sh（Max 2026-10-04 定：Celadon 全部去 DGX 编、测、渲）。
# 以前每个 Mac 会话各自手写 rsync + ssh（T-048 的 dgx.sh 没入库），这里收成一个入口。DGX 上的 spark-test.sh 本体在
# ~/.cache/celadon-spark/bin/，参考副本在 scripts/dgx/spark-test.sh；它的子命令与锁的含义见 docs/BUILDING.md「DGX Spark」一节。
#
# 用法：scripts/spark.sh [--dry-run] [--name 名] sync                 把当前工作树同步到 DGX ~/.cache/celadon-spark/src/<名>/
#       scripts/spark.sh [--dry-run] [--name 名] archive <提交>       把 <提交> 的干净树（git archive，不含未提交改动）放到同一位置：基线用
#       scripts/spark.sh [--dry-run] [--name 名] build|test|run|dir [参数…]
#                                                                    调 DGX 上 spark-test.sh <源码目录> <子命令> [参数…]
#                                                                    （test / run 走 DGX 全机锁 run.lock；run 的命令里可用 $BUILD，要单引号）
#       scripts/spark.sh [--dry-run] ssh <命令…>                       直接在 DGX 上跑命令，不进锁（看日志、列目录用）
# 名字默认是工作树目录名；主进程给基线起 t096-base 这类带任务号前缀的名字，用完删。同步 / 归档只写自己建的目录：
# 远端目录已存在却没有本脚本留下的 .celadon-spark-owner 标记时拒绝覆盖（别的会话的源码目录不能被 rsync --delete 抹掉）。
# 环境：CELADON_SPARK_HOST      ssh 别名（默认 max-spark，复用 ~/.ssh/config 与 known_hosts 里的 HostKeyAlias）
#       CELADON_SPARK_HOSTNAME  覆盖 HostName（默认 192.168.109.100：max-spark.local 跨网段解析不到；走 Tailscale 时填它的 100.x 地址）
#       SPARK_JOBS、CELADON_CXX_FLAGS  原样传给远端 spark-test.sh（几个会话同时编时 SPARK_JOBS 给 4）
# --dry-run：只打印要跑的命令，不碰网络、不碰远端。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "$ROOT/scripts/paths.sh"

HOST="${CELADON_SPARK_HOST:-max-spark}"
HOSTNAME_OVERRIDE="${CELADON_SPARK_HOSTNAME:-192.168.109.100}"
REMOTE_CACHE='$HOME/.cache/celadon-spark'   # 单引号：留给远端 shell 展开
OWNER_MARK='.celadon-spark-owner'
DRY=0
NAME="$(basename "$ROOT")"
LOCK=""

usage() { sed -n '2,18p' "$0"; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1; shift ;;
    --name) [ "$#" -ge 2 ] || { echo "spark.sh: --name 后面要跟名字" >&2; exit 2; }; NAME="$2"; shift 2 ;;
    --name=*) NAME="${1#--name=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) break ;;
  esac
done
[ "$#" -ge 1 ] || { usage; exit 2; }
CMD="$1"; shift
case "$NAME" in
  ''|*/*|.|..) echo "spark.sh: --name 只能是一个目录名，不能带 /：'$NAME'" >&2; exit 2 ;;
esac

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
if [ -n "$HOSTNAME_OVERRIDE" ]; then SSH+=(-o "Hostname=$HOSTNAME_OVERRIDE"); fi
REMOTE_SRC="$REMOTE_CACHE/src/$NAME"

# 本机锁：同一个名字的「同步 + 远端操作」排队，后一次的 rsync --delete 不会在前一次编译到一半时改写源码（做法同 test-linux.sh）。
# macOS 没有 flock，用 mkdir 锁；持锁进程已不在就清掉残留锁。
take_lock() {
  LOCK="$(celadon_tmp_root)/celadon-spark-$NAME.lock"
  until mkdir "$LOCK" 2>/dev/null; do
    if [ -f "$LOCK/pid" ] && ! kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; then
      rm -rf "$LOCK"
      continue
    fi
    sleep 2
  done
  echo $$ > "$LOCK/pid"
  trap 'rm -rf "$LOCK"' EXIT
}

# 预演只打印；真跑先打印再执行。打印用 %q，照着复制到终端就能重放。
run_local() {
  printf '+'; printf ' %q' "$@"; printf '\n'
  if [ "$DRY" -eq 0 ]; then "$@"; fi
}
run_remote() {   # $1 是一整段远端 shell（远端 bash 解析，$HOME / $BUILD 在那边展开）
  printf '+ ssh %s %q\n' "$HOST" "$1"
  if [ "$DRY" -eq 0 ]; then "${SSH[@]}" "$HOST" "$1"; fi
}

# 远端目录要么不存在、要么是本脚本建的（有标记），否则拒绝：别的会话的 t097-main 之类不能被我们抹掉。
ensure_owned() {
  if [ "$DRY" -eq 1 ]; then return 0; fi
  local verdict
  verdict="$("${SSH[@]}" "$HOST" "if [ -d $REMOTE_SRC ] && [ ! -e $REMOTE_SRC/$OWNER_MARK ]; then echo OTHER; else echo OK; fi")"
  if [ "$verdict" != "OK" ]; then
    echo "spark.sh: 远端 $REMOTE_SRC 已存在且不是本脚本建的（没有 $OWNER_MARK），不覆盖；换个 --name，或确认后手动删它" >&2
    exit 3
  fi
}
mark_owned() { run_remote "echo mac:$NAME:\$(date -u +%Y-%m-%dT%H:%MZ) > $REMOTE_SRC/$OWNER_MARK"; }

case "$CMD" in
  sync)
    take_lock
    ensure_owned
    run_remote "mkdir -p $REMOTE_SRC"
    # 只同步入库的源码（含 JuceLibraryCode/，BUG-005）；排除项与 test-linux.sh / render-linux.sh 共用 paths.sh 的那组，另排 .git/。
    # 不用 -a（它带 -t 保留 Mac 端修改时间）：--checksum 按内容判断，只重写内容变了的文件，远端 ninja 不会因时间戳全量重编。
    # --exclude 的标记文件在 --delete 下不会被删（没开 --delete-excluded）。
    run_local rsync -rlc --delete "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" --exclude '.git/' --exclude "$OWNER_MARK" \
      -e "${SSH[*]}" "$ROOT/" "$HOST:.cache/celadon-spark/src/$NAME/"
    mark_owned
    ;;
  archive)
    [ "$#" -eq 1 ] || { echo "用法：spark.sh [--name 名] archive <提交>" >&2; exit 2; }
    COMMIT="$(git -C "$ROOT" rev-parse --verify "$1^{commit}")"
    take_lock
    ensure_owned
    # 干净树：不含未提交改动、不含 .git；记下提交号，基线报告直接读它。
    printf '+ git -C %q archive --format=tar %s | ssh %s %q\n' "$ROOT" "$COMMIT" "$HOST" \
      "rm -rf $REMOTE_SRC && mkdir -p $REMOTE_SRC && tar -x -C $REMOTE_SRC"
    if [ "$DRY" -eq 0 ]; then
      git -C "$ROOT" archive --format=tar "$COMMIT" \
        | "${SSH[@]}" "$HOST" "rm -rf $REMOTE_SRC && mkdir -p $REMOTE_SRC && tar -x -C $REMOTE_SRC && echo $COMMIT > $REMOTE_SRC/.celadon-archived-commit"
    fi
    mark_owned
    ;;
  build|test|run|dir)
    ENV_PREFIX=""
    if [ -n "${SPARK_JOBS:-}" ]; then ENV_PREFIX+="SPARK_JOBS=$(printf '%q' "$SPARK_JOBS") "; fi
    if [ -n "${CELADON_CXX_FLAGS:-}" ]; then ENV_PREFIX+="CELADON_CXX_FLAGS=$(printf '%q' "$CELADON_CXX_FLAGS") "; fi
    ARGS=""
    if [ "$#" -gt 0 ]; then ARGS="$(printf ' %q' "$@")"; fi
    run_remote "${ENV_PREFIX}$REMOTE_CACHE/bin/spark-test.sh $REMOTE_SRC $CMD$ARGS"
    ;;
  ssh)
    [ "$#" -ge 1 ] || { echo "用法：spark.sh ssh <命令…>" >&2; exit 2; }
    run_remote "$(printf '%q ' "$@")"
    ;;
  *)
    echo "spark.sh: 不认识的子命令 '$CMD'" >&2
    usage
    exit 2
    ;;
esac
