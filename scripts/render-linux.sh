#!/usr/bin/env bash
# 在 Linux x86_64 测试服务器上编译并运行真歌离线对照工具 celadon_render（设计规格-Clay-分析决策与验收 第 7 节；
# 用法与输出见 tools/render/README.md），再把结果拉回本机。默认本机（Mac）不跑构建与渲染（ADR-002 修订 4；服务器故障时以 Max 当场指令为准，修订 5）。
# 做法照 scripts/test-linux.sh：rsync 工作树与 ../JUCE 到服务器，远端 cmake + ninja，本机 mkdir 锁 + 远端 flock。
#
# 用法：scripts/render-linux.sh <本地歌曲目录> [celadon_render 的额外参数…]
#   例：scripts/render-linux.sh ~/Music/对照歌
#       scripts/render-linux.sh ~/Music/对照歌 --glaze 0,50 --style auto,warm --match-lufs -14
#   --in / --out 由本脚本给，不要再传。歌曲目录只取第一层的 wav / wave / bwf / aif / aiff / aifc / flac。
# 环境：CELADON_TEST_HOST   目标主机（默认 maxsong@192.168.100.107，与 test-linux.sh 同一台，需在家里网络或 VPN 下）
#       CELADON_RENDER_OUT  本机结果目录（默认 <仓库>/artifacts/render-<日期时间>/）
#       CELADON_RENDER_SKIP_SELFTEST=1  跳过渲染前的 celadon_render --selftest（默认先跑：指标算法或延迟对齐坏了就不渲染真歌）
#
# 远端目录与 test-linux.sh 分开：两边各自 rsync --delete 互不删对方的东西，渲染跑十几分钟也不挡测试。
#   ~/.cache/celadon-render/<工作树名>-<哈希>/Celadon、JUCE    源码（不传 artifacts/：里面是几百 MB 的预览包与渲染结果）
#   ~/.cache/celadon-render-build/<工作树名>-<哈希>/           构建目录（第一次要编 JUCE 模块，之后增量）
#   ~/.cache/celadon-render/songs-<歌曲目录哈希>/               歌曲缓存（按大小 + 修改时间增量同步，同一目录再跑不重传）
#   ~/.cache/celadon-render/<工作树名>-<哈希>/out-<运行号>/     本次输出：拉回本机后删除（拉回失败时保留）
# 歌曲缓存不自动删；要清：ssh <主机> 'rm -rf ~/.cache/celadon-render/songs-*'
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
用法：scripts/render-linux.sh <本地歌曲目录> [celadon_render 的额外参数…]
  例：scripts/render-linux.sh ~/Music/对照歌 --glaze 0,50 --style auto,warm
  --in / --out 由脚本给；结果拉回 $CELADON_RENDER_OUT（默认 <仓库>/artifacts/render-<日期时间>/）。
  目标主机 $CELADON_TEST_HOST（默认 maxsong@192.168.100.107）。渲染前先跑 celadon_render --selftest，
  CELADON_RENDER_SKIP_SELFTEST=1 可跳过。详见 tools/render/README.md。
USAGE
}

if [ "$#" -lt 1 ]; then
  usage
  exit 2
fi

case "$1" in
  -h|--help) usage; exit 0 ;;
esac

if [ ! -d "$1" ]; then
  echo "render-linux.sh：歌曲目录不存在：$1" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
JUCE_DIR="$(cd "$ROOT/../JUCE" && pwd -P)"
HOST="${CELADON_TEST_HOST:-maxsong@192.168.100.107}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

SONGS_DIR="$(cd "$1" && pwd -P)"
shift

# 其余参数原样交给 celadon_render（远端用 eval "set -- …" 还原成数组）。空参数时不能用 printf：它会产出一个空字符串参数。
# macOS 自带 bash 3.2 在 set -u 下展开空的 "$@" 也不安全，所以一律先判断个数。
ARGS=""
if [ "$#" -gt 0 ]; then
  for arg in "$@"; do
    case "$arg" in
      --in|--out)
        echo "render-linux.sh：--in / --out 由脚本给，不要再传" >&2
        exit 2
        ;;
    esac
  done
  ARGS="$(printf '%q ' "$@")"
fi

# 本地先确认有歌：没有就不必同步、不必在服务器上编译。-L：歌曲可能是指向音乐库的符号链接，按链接目标算。
if [ -z "$(find -L "$SONGS_DIR" -maxdepth 1 -type f ! -name '.*' \( -iname '*.wav' -o -iname '*.wave' -o -iname '*.bwf' \
      -o -iname '*.aif' -o -iname '*.aiff' -o -iname '*.aifc' -o -iname '*.flac' \) -print -quit)" ]; then
  echo "render-linux.sh：$SONGS_DIR 第一层没有 wav / aiff / flac 文件" >&2
  exit 2
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="${CELADON_RENDER_OUT:-$ROOT/artifacts/render-$STAMP}"

# 每个工作树一个远端目录（键与 test-linux.sh 相同的算法，前缀不同）；歌曲缓存按本地歌曲目录的绝对路径分。
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-render/$(basename "$ROOT")-$KEY"
SONGS_KEY="$(printf '%s' "$SONGS_DIR" | shasum -a 256 | cut -c1-12)"
REMOTE_SONGS=".cache/celadon-render/songs-$SONGS_KEY"
REMOTE_OUT="$REMOTE/out-$STAMP-$$"

# 本机锁：罩住「同步 + 远端构建 + 渲染 + 拉回」整段，同一工作树的两次渲染排队（后一次的 rsync --delete 不会在前一次
# 编译到一半时改写源码）。与 test-linux.sh 的锁名不同：渲染与测试用的是各自的远端目录，可以同时跑。
# macOS 没有 flock，用 mkdir 锁；锁里记 pid，持锁进程已经不在（比如被 kill -9）就清掉残留锁。
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"
LOCK="$TMP_ROOT/celadon-render-$KEY.lock"
WAITING=0
until mkdir "$LOCK" 2>/dev/null; do
  if [ -f "$LOCK/pid" ] && ! kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; then
    rm -rf "$LOCK"
    continue
  fi
  if [ "$WAITING" -eq 0 ]; then
    echo "render-linux.sh：同一工作树的另一轮渲染还在跑，排队等它结束…" >&2
    WAITING=1
  fi
  sleep 2
done
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT

if ! "${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE' '$REMOTE_SONGS'"; then
  echo "render-linux.sh：连不上 $HOST（服务器宕机，或不在家里网络 / VPN 下）" >&2
  exit 1
fi

echo "render-linux.sh：同步源码到 $HOST …" >&2
# 排除项与 test-linux.sh 共用 paths.sh 的 CELADON_SOURCE_RSYNC_EXCLUDES：不传 artifacts/（几百 MB 的预览包，以及渲染结果本身）
# 与 docs/（验证日志，与编译无关；审核 V3-03）；JuceLibraryCode/ 照传（BUG-005）。
# 不用 -a（它带 -t 保留 Mac 端修改时间）：改用 --checksum 按内容判断，只重写内容变了的文件，
# 重写后修改时间是服务器当前时间，ninja 一定会重编。
rsync -rlpD --checksum --delete --delete-excluded \
  "${CELADON_SOURCE_RSYNC_EXCLUDES[@]}" \
  -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"

# JUCE 只传 modules/（工具直接编模块源码）；JUCE 树不会改，保留 -a。
rsync -a --delete --delete-excluded \
  --include '/LICENSE.md' --include '/modules/***' \
  --exclude '*' \
  -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"

echo "render-linux.sh：同步歌曲 $SONGS_DIR …" >&2
# 只取第一层的音频文件（扩展名不分大小写；隐藏文件如 Mac 的 ._xxx.wav 不传）。-t 保留修改时间：按「大小 + 修改时间」
# 增量，同一目录第二次起不重传。-L：符号链接按目标文件传。--delete-excluded：远端缓存与本地目录里的音频一一对应。
rsync -rtL --delete --delete-excluded \
  --exclude '.*' \
  --include '*.[wW][aA][vV]' --include '*.[wW][aA][vV][eE]' --include '*.[bB][wW][fF]' \
  --include '*.[aA][iI][fF]' --include '*.[aA][iI][fF][fF]' --include '*.[aA][iI][fF][cC]' \
  --include '*.[fF][lL][aA][cC]' \
  --exclude '*' \
  -e "${SSH[*]}" "$SONGS_DIR"/ "$HOST:$REMOTE_SONGS/"

# 远端 flock 再挡一层：同一构建目录的并发（例如另一台 Mac 上同名工作树）也要排队。
# 不能用 exec：exec 之后 EXIT trap 不会执行，本机锁就释放不了。
SKIP_SELFTEST="${CELADON_RENDER_SKIP_SELFTEST:-0}"
set +e
"${SSH[@]}" "$HOST" \
  "CELADON_REMOTE=$(printf '%q' "$REMOTE") CELADON_SONGS=$(printf '%q' "$REMOTE_SONGS") CELADON_OUT=$(printf '%q' "$REMOTE_OUT") CELADON_ARGS=$(printf '%q' "$ARGS") CELADON_SKIP_SELFTEST=$(printf '%q' "$SKIP_SELFTEST") bash -euo pipefail -s" <<'REMOTE_SCRIPT'
cd "$HOME/$CELADON_REMOTE"
BUILD="$HOME/.cache/celadon-render-build/$(basename "$CELADON_REMOTE")"
mkdir -p "$BUILD" "$HOME/.cache/celadon-locks"   # 输出目录由 celadon_render 自己建：编译 / 自检失败时就没有，也就不用拉回
exec 9>"$HOME/.cache/celadon-locks/render-$(basename "$CELADON_REMOTE").lock"
flock 9
eval "set -- $CELADON_ARGS"
nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >&2
nice -n 10 cmake --build "$BUILD" --target celadon_render >&2
# 先跑自检（合成信号上的阳性对照，几秒钟）：指标算法、延迟对齐或 GLAZE 0 逐位直通坏了，真歌的对照结果就不可信，不渲染。
if [ "$CELADON_SKIP_SELFTEST" != "1" ]; then
  nice -n 10 "$BUILD/celadon_render" --selftest
fi
nice -n 10 "$BUILD/celadon_render" --in "$HOME/$CELADON_SONGS" --out "$HOME/$CELADON_OUT" "$@"
REMOTE_SCRIPT
status=$?
set -e

# 拉回：工具中途失败也可能已经写出一部分成品与指标表，照样拉回。拉回成功才删远端的本次输出。
mkdir -p "$OUT_DIR"
pull=0
if "${SSH[@]}" "$HOST" "test -d $(printf '%q' "$REMOTE_OUT")"; then
  echo "render-linux.sh：拉回结果到 $OUT_DIR …" >&2
  rsync -rt -e "${SSH[*]}" "$HOST:$REMOTE_OUT/" "$OUT_DIR/" || pull=$?
  if [ "$pull" -eq 0 ]; then
    case "$REMOTE_OUT" in
      .cache/celadon-render/*/out-*) "${SSH[@]}" "$HOST" "rm -rf $(printf '%q' "$REMOTE_OUT")" ;;
    esac
  else
    echo "render-linux.sh：拉回失败（rsync 返回 $pull），服务器上的结果保留在 ~/$REMOTE_OUT" >&2
  fi
fi
rmdir "$OUT_DIR" 2>/dev/null || true   # 什么都没拉回（例如编译失败）时不留空目录

if [ "$status" -ne 0 ]; then
  echo "render-linux.sh：远端构建、自检或渲染失败（返回 $status），看上面的输出" >&2
  exit "$status"
fi

if [ "$pull" -ne 0 ]; then
  exit "$pull"
fi

echo "render-linux.sh：完成，结果在 $OUT_DIR（先盲听、再看 metrics.md；盲听做法见 tools/render/README.md）" >&2
