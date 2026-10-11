#!/usr/bin/env bash
# 把工作树（或某个提交的干净树）送到 DGX Spark，并在那边调 spark-test.sh（Max 2026-10-04 定：Celadon 全部去 DGX 编、测、渲）。
# 以前每个 Mac 会话各自手写 rsync + ssh，这里收成一个入口。DGX 上的 spark-test.sh 本体在 ~/.cache/celadon-spark/bin/，
# 参考副本 scripts/dgx/spark-test.sh（2026-10-11 原样拷入，以 DGX 上那份为准）；子命令与两把锁的含义见 docs/BUILDING.md「DGX Spark」一节。
#
# 用法（--dry-run / --name 放在子命令前面；放在后面会报错，不会悄悄当成远端参数）：
#   scripts/spark.sh [--dry-run] [--name 名] sync              把当前工作树同步到 DGX ~/.cache/celadon-spark/src/<名>/（目录不存在就建）
#   scripts/spark.sh [--dry-run] [--name 名] archive <提交>    把 <提交> 的干净树（git archive，不含未提交改动）放到同一位置，
#                                                             并在那里写 .celadon-archived-commit 记提交号：基线用
#   scripts/spark.sh [--dry-run] [--name 名] build|test|run|dir [参数…]
#                                                             调 DGX 上 spark-test.sh <源码目录> <子命令> [参数…]（目录要先 sync 或 archive 建好；
#                                                             test / run 走 DGX 全机锁 run.lock）
#   scripts/spark.sh [--dry-run] ssh <命令…>                    直接在 DGX 上跑命令，不进锁、不查归属（看日志、列目录用；不转发 stdin）
# 参数逐个原样转义后交给远端：$变量（含 $BUILD）、通配符、管道都不会展开，要用就包一层 bash -c，路径从远端家目录起，例如
#   scripts/spark.sh run bash -c '"$BUILD/celadon_render" --help'
#   scripts/spark.sh ssh bash -c 'ls .cache/celadon-spark/src | head'
#   scripts/spark.sh ssh bash -c 'df -h ~'
# 别依赖 ~：macOS 自带的 bash 3.2 不转义词首的 ~（远端会展开），bash 5 会转义（远端不展开）。
# Ctrl-C 结束本机的 ssh 并放开本机锁；远端进程收不到信号，通常在下一次写输出时因连接已断而结束（输出丢失）；输出全部重定向到文件的
# run 命令会跑完并占着 run.lock。重跑前先 scripts/spark.sh ssh pgrep -af 'celadon_tests|ninja' 看一眼。长任务照旧后台跑 + 轮询日志：
#   scripts/spark.sh ssh bash -c 'setsid nohup <命令> > <日志> 2>&1 < /dev/null &'
# 名字默认是工作树目录名，只许 [A-Za-z0-9._-]、首字符是字母或数字；给基线起 t096-base 这类带任务号前缀的名，用完删。
# 归属：sync / archive / build / test / run 只碰「本机 + 本工作树」自己建的远端目录——目录里的 .celadon-spark-owner 第 1 行记主人；
# sync / archive 遇到目录不存在就建并写标记，主人不是自己或没有标记一律拒绝（exit 3），检查与写入在同一条远端命令里完成；
# build / test / run 要求目录已由本工作树建好。dir、ssh 不查归属、不排队。
# 本机锁：同一个名字的 sync / archive / build / test / run 排队，远端跑完才放。
# 退出码：用法错 2；归属拒绝 3；连不上 DGX 1；其余沿用 rsync / 远端命令的退出码。
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
REMOTE_CACHE_REL='.cache/celadon-spark'                         # 相对远端家目录；远端脚本体里写成 "$HOME/…"，rsync 目标直接用相对路径
OWNER_MARK='.celadon-spark-owner'
ARCHIVED_MARK='.celadon-archived-commit'
DRY=0
NAME="$(basename "$ROOT")"
LOCK=""

usage() { awk 'NR > 1 && !/^#/ { exit } NR > 1 { print }' "$0"; }   # 打印开头连续的注释行，不写死行号
die() { echo "spark.sh：$*" >&2; exit 2; }

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
# 只拦本脚本自己的两个选项；远端命令自己的 -h / --help 不管（要用 --dry-run 这种词就包 bash -c）。
for arg in "$@"; do
  case "$arg" in
    --dry-run|--name|--name=*) die "选项 '$arg' 要放在子命令 '$CMD' 前面" ;;
  esac
done
# 名字直接拼进远端路径与 rm -rf，所以只收白名单字符；默认名（工作树目录名）不合格就要求给 --name。
case "$NAME" in
  ''|[!A-Za-z0-9]*|*[!A-Za-z0-9._-]*) die "名字只许字母、数字、. _ -，且以字母或数字开头：'$NAME'（用 --name 另起一个）" ;;
esac

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30 -o ServerAliveCountMax=4)
if [ -n "$HOSTNAME_OVERRIDE" ]; then SSH+=(-o "Hostname=$HOSTNAME_OVERRIDE"); fi
REMOTE_SRC_REL="$REMOTE_CACHE_REL/src/$NAME"       # 远端路径只在这里拼一次：脚本体用 "$HOME/$1"，rsync 目标用相对家目录
# 主人身份：本机名（LocalHostName 不随 DHCP 变）+ 工作树绝对路径。
OWNER_ID="mac:$(scutil --get LocalHostName 2>/dev/null || hostname -s):$ROOT"

# 远端脚本体都是单引号字面量，变量经位置参数传入（$1 = 源码目录相对家目录，$2 = 标记文件名，$3 = 主人，$4 / $5 见 archive），
# 发到远端时写成 bash -c <脚本体> _ <参数…>，每一段都用 %q 转义，本文件里不用手写 \" 和 \$。
# 探针：目录不存在 → CELADON_STATE=absent；没标记 → unmarked；有标记 → marked + CELADON_OWNER=<第 1 行>。
PROBE_BODY='d="$HOME/$1"; if [ ! -e "$d" ]; then echo CELADON_STATE=absent; elif [ ! -f "$d/$2" ]; then echo CELADON_STATE=unmarked; else echo CELADON_STATE=marked; printf "CELADON_OWNER=%s\n" "$(head -n1 "$d/$2")"; fi'
# 建目录 + 写标记（sync 用）：主人不是自己就 exit 3，检查与写入同一条命令、中间没有窗口；标记第 1 行主人、第 2 行时间。
MARK_BODY='d="$HOME/$1"; m="$d/$2"; if [ -e "$d" ] && [ "$(head -n1 "$m" 2>/dev/null)" != "$3" ]; then echo "CELADON_REFUSE" >&2; exit 3; fi; mkdir -p "$d" && printf "%s\n%s\n" "$3" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$m"'
# 归档（archive 用，stdin 是 tar 流）：同样先核归属，再 rm -rf、建目录、写标记、解包（-m：解包时间作 mtime，ninja 一定重编）、记提交号（$4 = 提交，$5 = 提交标记文件名）。
ARCHIVE_BODY='d="$HOME/$1"; m="$d/$2"; if [ -e "$d" ] && [ "$(head -n1 "$m" 2>/dev/null)" != "$3" ]; then echo "CELADON_REFUSE" >&2; exit 3; fi; rm -rf "$d" && mkdir -p "$d" && printf "%s\n%s\n" "$3" "$(date -u +%Y-%m-%dT%H:%MZ)" > "$m" && tar -x -m -C "$d" && echo "$4" > "$d/$5"'
remote_script() {   # remote_script <脚本体> <参数…> → 一段远端 shell：bash -c '<脚本体>' _ <参数…>
  local body="$1"; shift
  printf 'bash -c %q _' "$body"; printf ' %q' "$@"; printf '\n'
}

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
run_remote() {   # $1 是一整段远端 shell；-n：不读本机 stdin
  trace "$(printf '%q ' "${SSH[@]}" -n "$HOST" "$1")"
  if [ "$DRY" -eq 0 ]; then "${SSH[@]}" -n "$HOST" "$1"; fi
}
capture_remote() {   # 同 run_remote，但把远端 stdout 收进变量 REMOTE_OUT；连不上以 1 退出并给中文提示
  local cmd="$1"
  trace "$(printf '%q ' "${SSH[@]}" -n "$HOST" "$cmd")"
  if ! REMOTE_OUT="$("${SSH[@]}" -n "$HOST" "$cmd")"; then
    echo "spark.sh：连不上 $HOST（HostName=${HOSTNAME_OVERRIDE:-按 ssh 配置}）：跨网段改 CELADON_SPARK_HOSTNAME，走 Tailscale 要先在网页授权" >&2
    exit 1
  fi
}

# 纯逻辑：把探针输出判成 absent / mine / unmarked / empty / other:<主人> / noresult，不碰网络，可离线核对。
owner_verdict() {   # owner_verdict <探针输出> <自己的主人串>
  local out="$1" me="$2" state owner
  state="$(printf '%s\n' "$out" | grep '^CELADON_STATE=' | tail -n1 || true)"; state="${state#CELADON_STATE=}"
  owner="$(printf '%s\n' "$out" | grep '^CELADON_OWNER=' | tail -n1 || true)"; owner="${owner#CELADON_OWNER=}"
  case "$state" in
    absent|unmarked) echo "$state" ;;
    marked) if [ "$owner" = "$me" ]; then echo mine; elif [ -z "$owner" ]; then echo empty; else echo "other:$owner"; fi ;;
    *) echo noresult ;;
  esac
}
# 先探一次归属：给出清楚的拒绝理由（sync / archive 的写入命令里还会再核一次，防两次 ssh 之间被人抢建）。
ensure_owned() {   # ensure_owned <目录不存在时怎么办：create | require>
  local probe verdict
  probe="$(remote_script "$PROBE_BODY" "$REMOTE_SRC_REL" "$OWNER_MARK")"
  if [ "$DRY" -eq 1 ]; then
    trace "（预演：真跑前会先查远端目录归属）$(printf '%q ' "${SSH[@]}" -n "$HOST" "$probe")"
    return 0
  fi
  capture_remote "$probe"
  verdict="$(owner_verdict "$REMOTE_OUT" "$OWNER_ID")"
  case "$verdict" in
    mine) return 0 ;;
    absent) if [ "$1" = create ]; then return 0; fi
            die "远端还没有 src/$NAME：先 sync 或 archive" ;;
    unmarked) echo "spark.sh：远端 src/$NAME 已存在且没有标记（不是本脚本建的），不碰；换个 --name，或确认后手动删它" >&2; exit 3 ;;
    empty) echo "spark.sh：远端 src/$NAME 的标记是空文件，不碰；确认后手动删它再来" >&2; exit 3 ;;
    other:*) echo "spark.sh：远端 src/$NAME 的主人是「${verdict#other:}」，不是本机本工作树（$OWNER_ID），不碰；换个 --name" >&2; exit 3 ;;
    *) echo "spark.sh：远端归属检查没有返回结果（输出：$REMOTE_OUT），不动远端" >&2; exit 3 ;;
  esac
}
refused() { echo "spark.sh：远端 src/$NAME 在检查与写入之间归属变了，没动它" >&2; exit 3; }

case "$CMD" in
  sync)
    [ "$#" -eq 0 ] || die "sync 不接参数（多出来的：$*）"
    take_lock
    ensure_owned create
    # 远端展开后：bash -c '<MARK_BODY>' _ .cache/celadon-spark/src/<名> .celadon-spark-owner mac:<主机>:<ROOT>
    if ! run_remote "$(remote_script "$MARK_BODY" "$REMOTE_SRC_REL" "$OWNER_MARK" "$OWNER_ID")"; then
      rc=$?; [ "$rc" -eq 3 ] && refused; exit "$rc"
    fi
    # 排除项与 test-linux.sh / render-linux.sh 共用 paths.sh 的那组（JuceLibraryCode/ 不在排除项里、照常同步，BUG-005）；被 .gitignore
    # 忽略的产物不在那组里，会一起过网。另排根上的 .git（工作树里它是个文件，不能写成 '.git/'）；标记文件用 P 规则保住，
    # --delete-excluded 不会删它；.celadon-archived-commit 不在源里，同步后会被 --delete 删掉（这个目录已不是干净树）。
    # 不用 -a（它带 -t 保留 Mac 端修改时间）：--checksum 按内容判断，只重写内容变了的文件，重写后 mtime 是远端当前时间，ninja 一定重编。
    # rsync 的远端路径写成相对家目录：新版 rsync 会转义远端参数，"$HOME" 不会在那边展开。
    run_local rsync -rlpD --checksum --delete --delete-excluded \
      "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" --exclude '/.git' --filter "P /$OWNER_MARK" \
      -e "${SSH[*]}" "$ROOT/" "$HOST:$REMOTE_SRC_REL/"
    ;;
  archive)
    [ "$#" -eq 1 ] || die "用法：spark.sh [--name 名] archive <提交>"
    COMMIT="$(git -C "$ROOT" rev-parse --verify "$1^{commit}")"
    take_lock
    ensure_owned create
    # 远端展开后：bash -c '<ARCHIVE_BODY>' _ .cache/celadon-spark/src/<名> .celadon-spark-owner mac:<主机>:<ROOT> <提交> .celadon-archived-commit
    REMOTE_CMD="$(remote_script "$ARCHIVE_BODY" "$REMOTE_SRC_REL" "$OWNER_MARK" "$OWNER_ID" "$COMMIT" "$ARCHIVED_MARK")"
    trace "$(printf '%q ' git -C "$ROOT" archive --format=tar "$COMMIT") | $(printf '%q ' "${SSH[@]}" "$HOST" "$REMOTE_CMD")"
    if [ "$DRY" -eq 0 ]; then
      if ! git -C "$ROOT" archive --format=tar "$COMMIT" | "${SSH[@]}" "$HOST" "$REMOTE_CMD"; then
        rc=$?; [ "$rc" -eq 3 ] && refused; exit "$rc"
      fi
      echo "spark.sh：远端 src/$NAME 现在是 $COMMIT 的干净树（$ARCHIVED_MARK 记着提交号）" >&2
    fi
    ;;
  build|test|run|dir)
    if [ "$CMD" != dir ]; then
      take_lock
      ensure_owned require
    fi
    ENV_PREFIX=""
    if [ -n "${SPARK_JOBS:-}" ]; then ENV_PREFIX+="SPARK_JOBS=$(printf '%q' "$SPARK_JOBS") "; fi
    if [ -n "${CELADON_CXX_FLAGS:-}" ]; then ENV_PREFIX+="CELADON_CXX_FLAGS=$(printf '%q' "$CELADON_CXX_FLAGS") "; fi
    ARGS=""
    if [ "$#" -gt 0 ]; then ARGS="$(printf ' %q' "$@")"; fi
    # 远端展开后：[SPARK_JOBS=…] "$HOME/.cache/celadon-spark/bin/spark-test.sh" "$HOME/.cache/celadon-spark/src/<名>" <子命令> <参数…>
    run_remote "${ENV_PREFIX}\"\$HOME/$REMOTE_CACHE_REL/bin/spark-test.sh\" \"\$HOME/$REMOTE_SRC_REL\" $CMD$ARGS"
    ;;
  ssh)
    [ "$#" -ge 1 ] || die "用法：spark.sh ssh <命令…>"
    run_remote "$(printf '%q ' "$@")"
    ;;
  *)
    echo "spark.sh：不认识的子命令 '$CMD'" >&2
    usage >&2
    exit 2
    ;;
esac
