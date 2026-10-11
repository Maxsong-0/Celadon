#!/usr/bin/env bash
# 把工作树（或某个提交的干净树）送到 DGX Spark，并在那边调 spark-test.sh（Max 2026-10-04 定：Celadon 全部去 DGX 编、测、渲）。
# 以前每个 Mac 会话各自手写 rsync + ssh，这里收成一个入口。DGX 上的 spark-test.sh 本体在 ~/.cache/celadon-spark/bin/，
# 参考副本 scripts/dgx/spark-test.sh（DGX 可达后入库，以 DGX 上那份为准）；子命令与两把锁的含义见 docs/BUILDING.md「DGX Spark」一节。
#
# 用法（选项都放在子命令前面；放在后面会报错，不会悄悄当成远端参数）：
#   scripts/spark.sh [--dry-run] [--name 名] sync              把当前工作树同步到 DGX ~/.cache/celadon-spark/src/<名>/
#   scripts/spark.sh [--dry-run] [--name 名] archive <提交>    把 <提交> 的干净树（git archive，不含未提交改动）放到同一位置，
#                                                             并在那里写 .celadon-archived-commit 记提交号：基线用
#   scripts/spark.sh [--dry-run] [--name 名] build|test|run|dir [参数…]
#                                                             调 DGX 上 spark-test.sh <源码目录> <子命令> [参数…]
#                                                             （test / run 走 DGX 全机锁 run.lock；run 的命令里可用 $BUILD，要单引号）
#   scripts/spark.sh [--dry-run] ssh <命令…>                    直接在 DGX 上跑命令，不进锁、不查归属（看日志、列目录用）
# 参数逐个原样转义：远端不展开 ~ 和通配符、不认管道；要用就包一层 bash -c，路径从远端家目录起，例如
#   scripts/spark.sh ssh bash -c 'ls .cache/celadon-spark/src | head'
#   scripts/spark.sh run bash -c '"$BUILD/celadon_render" --help'
# Ctrl-C 只停本机的 ssh；远端已经开始的编译 / 测试会跑完，并占着它的锁。
# 名字默认是工作树目录名，只许 [A-Za-z0-9._-]、首字符是字母或数字；给基线起 t096-base 这类带任务号前缀的名，用完删。
# 归属：sync / archive / build / test / run 只碰「本机 + 本工作树」自己建的远端目录——目录里的 .celadon-spark-owner 记着主人，
# 不存在就建并写标记；主人不是自己、或没有标记，一律拒绝（exit 3），不会把别的会话的目录 rsync --delete 掉。
# 本机锁：同一个名字的 sync / archive / build / test / run 排队，远端跑完才放。dir、ssh 不排队、不查归属。
# 环境：CELADON_SPARK_HOST      ssh 别名（默认 max-spark，复用 ~/.ssh/config 与 known_hosts 里的 HostKeyAlias）
#       CELADON_SPARK_HOSTNAME  覆盖 HostName（默认 192.168.109.100：max-spark.local 跨网段解析不到；走 Tailscale 时填它的
#                               100.x 地址；设成空串 = 不覆盖、用 ssh 配置里自己的 HostName）
#       SPARK_JOBS、CELADON_CXX_FLAGS  原样传给远端 spark-test.sh（几个会话同时编时 SPARK_JOBS 给 4）
# --dry-run：只打印要跑的命令（与真跑同一份字符串，打到 stderr），不碰网络、不碰远端、不拿锁。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "$ROOT/scripts/paths.sh"

HOST="${CELADON_SPARK_HOST:-max-spark}"
HOSTNAME_OVERRIDE="${CELADON_SPARK_HOSTNAME-192.168.109.100}"   # 不带冒号：显式设空就是「不覆盖」
REMOTE_CACHE_REL='.cache/celadon-spark'                         # 相对远端家目录；远端命令里写成 "$HOME/…"，rsync 目标直接用相对路径
OWNER_MARK='.celadon-spark-owner'
ARCHIVED_MARK='.celadon-archived-commit'
DRY=0
NAME="$(basename "$ROOT")"
LOCK=""

usage() { awk 'NR > 1 && !/^#/ { exit } NR > 1 { print }' "$0"; }   # 打印开头连续的注释行，不写死行号
die() { echo "spark.sh: $*" >&2; exit 2; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1; shift ;;
    --name) [ "$#" -ge 2 ] || die "--name 后面要跟名字"; NAME="$2"; shift 2 ;;
    --name=*) NAME="${1#--name=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    --*) die "不认识的选项 '$1'" ;;
    *) break ;;
  esac
done
[ "$#" -ge 1 ] || { usage >&2; exit 2; }
CMD="$1"; shift
for arg in "$@"; do
  case "$arg" in
    --dry-run|--name|--name=*|-h|--help) die "选项 '$arg' 要放在子命令 '$CMD' 前面" ;;
  esac
done
# 名字直接拼进远端 shell 命令与 rm -rf，所以只收白名单字符；默认名（工作树目录名）不合格就要求给 --name。
case "$NAME" in
  ''|[!A-Za-z0-9]*|*[!A-Za-z0-9._-]*) die "名字只许字母、数字、. _ -，且以字母或数字开头：'$NAME'（用 --name 另起一个）" ;;
esac

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=4)
if [ -n "$HOSTNAME_OVERRIDE" ]; then SSH+=(-o "Hostname=$HOSTNAME_OVERRIDE"); fi
# 远端路径：带双引号的字面量，$HOME 留给远端展开；NAME 已过白名单，放进双引号里安全。
REMOTE_SRC_Q="\"\$HOME/$REMOTE_CACHE_REL/src/$NAME\""
REMOTE_BIN_Q="\"\$HOME/$REMOTE_CACHE_REL/bin/spark-test.sh\""
# 主人身份：本机名（LocalHostName 不随 DHCP 变）+ 工作树绝对路径。
OWNER_ID="mac:$(scutil --get LocalHostName 2>/dev/null || hostname -s):$ROOT"

trace() { printf '+ %s\n' "$*" >&2; }

# 本机锁：同一个名字的 sync / archive / build / test / run 排队，远端跑完才放，后一次的 rsync --delete 不会在前一次编译到
# 一半时改写源码（写法同 test-linux.sh）。macOS 没有 flock，用 mkdir 锁；持锁进程已不在就清掉残留锁。预演不拿锁。
take_lock() {
  if [ "$DRY" -eq 1 ]; then return 0; fi
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

# 打印的命令就是真跑的命令（%q 转义，照着复制到终端能重放）；预演只打印。
run_local() {
  trace "$(printf '%q ' "$@")"
  if [ "$DRY" -eq 0 ]; then "$@"; fi
}
run_remote() {   # $1 是一整段远端 shell（远端 bash 解析，$HOME / $BUILD 在那边展开）；-n：不读本机 stdin
  trace "$(printf '%q ' "${SSH[@]}" -n "$HOST" "$1")"
  if [ "$DRY" -eq 0 ]; then "${SSH[@]}" -n "$HOST" "$1"; fi
}

# 远端目录要么不存在、要么标记里的主人就是自己，其余一律拒绝（test 出错、.bashrc 有输出、ssh 失败都算拒绝）。
# 远端只认带 CELADON_OWNER= 前缀的那一行，登录 shell 的杂音混不进来。
ensure_owned() {
  local probe out line owner
  probe="d=$REMOTE_SRC_Q; if [ ! -e \"\$d\" ]; then echo CELADON_OWNER=absent; elif [ -f \"\$d/$OWNER_MARK\" ]; then printf 'CELADON_OWNER=%s\\n' \"\$(head -n1 \"\$d/$OWNER_MARK\")\"; else echo CELADON_OWNER=unmarked; fi"
  if [ "$DRY" -eq 1 ]; then
    trace "（预演：真跑前会先查远端目录归属）$(printf '%q ' "${SSH[@]}" -n "$HOST" "$probe")"
    return 0
  fi
  out="$("${SSH[@]}" -n "$HOST" "$probe")"
  line="$(printf '%s\n' "$out" | grep '^CELADON_OWNER=' | tail -n1 || true)"
  owner="${line#CELADON_OWNER=}"
  if [ -z "$line" ]; then
    echo "spark.sh: 远端归属检查没有返回结果（输出：$out），不动远端" >&2; exit 3
  fi
  case "$owner" in
    absent|"$OWNER_ID") return 0 ;;
    unmarked) echo "spark.sh: 远端 src/$NAME 已存在且没有标记（不是本脚本建的），不覆盖；换个 --name，或确认后手动删它" >&2; exit 3 ;;
    *) echo "spark.sh: 远端 src/$NAME 的主人是「$owner」，不是本机本工作树（$OWNER_ID），不覆盖；换个 --name，或确认后手动删它" >&2; exit 3 ;;
  esac
}
# 建目录并写标记（第 1 行主人，第 2 行时间），放在传数据之前：中途失败也不会留下「没标记、再也进不去」的目录。
mark_owned_cmd() { printf 'mkdir -p %s && printf %s %q "$(date -u +%%Y-%%m-%%dT%%H:%%MZ)" > %s/%s' "$REMOTE_SRC_Q" "'%s\\n%s\\n'" "$OWNER_ID" "$REMOTE_SRC_Q" "$OWNER_MARK"; }

case "$CMD" in
  sync)
    [ "$#" -eq 0 ] || die "sync 不接参数（多出来的：$*）"
    take_lock
    ensure_owned
    run_remote "$(mark_owned_cmd)"
    # 排除项与 test-linux.sh / render-linux.sh 共用 paths.sh 的那组（含 JuceLibraryCode/，BUG-005）；被 .gitignore 忽略的产物
    # 不在那组里，会一起过网。另排根上的 .git（工作树里它是个文件，不能写成 '.git/'）；标记文件用 P 规则保住，
    # --delete-excluded 不会删它；.celadon-archived-commit 不在源里，同步后会被 --delete 删掉（这个目录已不是干净树）。
    # 不用 -a（它带 -t 保留 Mac 端修改时间）：--checksum 按内容判断，只重写内容变了的文件，重写后 mtime 是远端当前时间，ninja 一定重编。
    # rsync 的远端路径写成相对家目录：新版 rsync 会转义远端参数，"$HOME" 不会在那边展开。
    run_local rsync -rlpD --checksum --delete --delete-excluded \
      "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" --exclude '/.git' --filter "P /$OWNER_MARK" \
      -e "${SSH[*]}" "$ROOT/" "$HOST:$REMOTE_CACHE_REL/src/$NAME/"
    ;;
  archive)
    [ "$#" -eq 1 ] || die "用法：spark.sh [--name 名] archive <提交>"
    COMMIT="$(git -C "$ROOT" rev-parse --verify "$1^{commit}")"
    take_lock
    ensure_owned
    # 干净树：不含未提交改动、不含 .git。tar -m：解包时间作修改时间（git archive 给的是提交时间，比已有 .o 旧，ninja 会以为不用重编）。
    # 同一段远端命令既打印又执行；标记先于解包写下。
    REMOTE_CMD="d=$REMOTE_SRC_Q; rm -rf \"\$d\" && $(mark_owned_cmd) && tar -x -m -C \"\$d\" && echo $COMMIT > \"\$d/$ARCHIVED_MARK\""
    trace "$(printf '%q ' git -C "$ROOT" archive --format=tar "$COMMIT") | $(printf '%q ' "${SSH[@]}" "$HOST" "$REMOTE_CMD")"
    if [ "$DRY" -eq 0 ]; then
      git -C "$ROOT" archive --format=tar "$COMMIT" | "${SSH[@]}" "$HOST" "$REMOTE_CMD"
      echo "spark.sh: 远端 src/$NAME 现在是 $COMMIT 的干净树（$ARCHIVED_MARK 记着提交号）" >&2
    fi
    ;;
  build|test|run|dir)
    if [ "$CMD" != dir ]; then
      take_lock
      ensure_owned
    fi
    ENV_PREFIX=""
    if [ -n "${SPARK_JOBS:-}" ]; then ENV_PREFIX+="SPARK_JOBS=$(printf '%q' "$SPARK_JOBS") "; fi
    if [ -n "${CELADON_CXX_FLAGS:-}" ]; then ENV_PREFIX+="CELADON_CXX_FLAGS=$(printf '%q' "$CELADON_CXX_FLAGS") "; fi
    ARGS=""
    if [ "$#" -gt 0 ]; then ARGS="$(printf ' %q' "$@")"; fi
    run_remote "${ENV_PREFIX}$REMOTE_BIN_Q $REMOTE_SRC_Q $CMD$ARGS"
    ;;
  ssh)
    [ "$#" -ge 1 ] || die "用法：spark.sh ssh <命令…>"
    run_remote "$(printf '%q ' "$@")"
    ;;
  *)
    echo "spark.sh: 不认识的子命令 '$CMD'" >&2
    usage >&2
    exit 2
    ;;
esac
