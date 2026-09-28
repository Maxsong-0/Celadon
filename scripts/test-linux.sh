#!/usr/bin/env bash
# 在 Linux x86_64 服务器上原生编译并运行 celadon_tests（ADR-002：本机 Mac 不跑任何测试与编译检查）。
# 做法照 Studiolo 的 scripts/test-linux.sh：把工作树与 JUCE rsync 到服务器，在那里 cmake + ninja，不占 CI runner 队列。
#
# 用法：scripts/test-linux.sh      # 配置 + 编译 + 跑全部测试
#       scripts/test-linux.sh bash -c 'cmake --build "$BUILD" --target celadon_tests && "$BUILD/celadon_tests"'
#       （带参数时命令在远端同步目录里执行，Celadon/ 与 JUCE/ 同级；构建目录在环境变量 $BUILD 里，要用单引号让远端展开）
# 环境：CELADON_TEST_HOST 覆盖目标主机（默认 maxsong@192.168.100.107，需在家里网络或 VPN 下）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
JUCE_DIR="$(cd "$ROOT/../JUCE" && pwd -P)"
HOST="${CELADON_TEST_HOST:-maxsong@192.168.100.107}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

# 每个工作树一个远端目录：并发会话互不覆盖。Celadon 与 JUCE 同级摆放，CMake 里的 ../JUCE 才找得到。
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-tests/$(basename "$ROOT")-$KEY"

# 本机锁：罩住「同步 + 远端构建 + 运行」整段，同一工作树的两次调用排队，
# 后一次的 rsync --delete 不会在前一次编译到一半时改写源码。macOS 没有 flock，用 mkdir 锁；
# 锁里记 pid，持锁进程已经不在（比如被 kill -9）就清掉残留锁。
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"
LOCK="$TMP_ROOT/celadon-test-$KEY.lock"
until mkdir "$LOCK" 2>/dev/null; do
  if [ -f "$LOCK/pid" ] && ! kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; then
    rm -rf "$LOCK"
    continue
  fi
  sleep 2
done
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT

if [ "$#" -eq 0 ]; then
  RUN='
    cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" --target celadon_tests
    "$BUILD/celadon_tests"
  '
else
  RUN="$(printf '%q ' "$@")"
fi

"${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE'"

# 只同步源码：Projucer 生成物、编译产物、系统垃圾文件不过网。
# 不用 -a（它带 -t 保留 Mac 端修改时间）：改用 --checksum 按内容判断，只重写内容变了的文件，
# 重写后修改时间是服务器当前时间，ninja 一定会重编；否则较旧的 mtime 可能让 ninja 以为 .o 已是最新。
rsync -rlpD --checksum --delete --delete-excluded \
  --exclude 'Builds/' --exclude 'JuceLibraryCode/' --exclude 'build/' --exclude '.DS_Store' --exclude '.claude/' --exclude 'artifacts/' --exclude 'docs/' \
  -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"

# JUCE 只传 modules/（测试直接编模块源码）；examples、docs、Mac 上的 .app 都不传。JUCE 树不会改，保留 -a。
rsync -a --delete --delete-excluded \
  --include '/LICENSE.md' --include '/modules/***' \
  --exclude '*' \
  -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"

# 远端 flock 再挡一层：其他工作树的会话不会用到同一个构建目录，但同一目录的并发仍要排队。
# 不能用 exec：exec 之后 EXIT trap 不会执行，本机锁就释放不了。
set +e
"${SSH[@]}" "$HOST" \
  "CELADON_REMOTE=$(printf '%q' "$REMOTE") CELADON_RUN=$(printf '%q' "$RUN") bash -euo pipefail -s" <<'REMOTE_SCRIPT'
cd "$HOME/$CELADON_REMOTE"
export BUILD="$HOME/.cache/celadon-build/$(basename "$CELADON_REMOTE")"
mkdir -p "$BUILD" "$HOME/.cache/celadon-locks"
exec 9>"$HOME/.cache/celadon-locks/$(basename "$CELADON_REMOTE").lock"
flock 9
nice -n 10 bash -euo pipefail -c "$CELADON_RUN"
REMOTE_SCRIPT
status=$?
set -e
exit "$status"
