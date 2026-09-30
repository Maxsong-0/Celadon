#!/usr/bin/env bash
# 在 107 上装依赖、类型检查、构建站点（ADR-002 / 全局纪律：本机 Mac 不装、不编、不测）。
# 做法照仓库 scripts/test-linux.sh：把 site/ rsync 到服务器，在那里 pnpm install + build，再把 dist/ 与 pnpm-lock.yaml 拉回来。
#
# 用法：site/scripts/build-linux.sh            # install + typecheck + build，拉回 dist 与 lock
#       site/scripts/build-linux.sh serve      # 同上，然后在服务器上用 python 起静态服务（端口 $SITE_PORT，默认 8787）供浏览器验收
#       site/scripts/build-linux.sh stop       # 停掉上面那个静态服务
# 环境：CELADON_TEST_HOST 覆盖目标主机（默认 maxsong@192.168.100.107）。
set -euo pipefail
SITE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
HOST="${CELADON_TEST_HOST:-maxsong@192.168.100.107}"
PORT="${SITE_PORT:-8787}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
KEY="$(printf '%s' "$SITE" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-site/$(basename "$(dirname "$SITE")")-$KEY"
MODE="${1:-build}"

if [ "$MODE" = "stop" ]; then
  "${SSH[@]}" "$HOST" "pkill -f '[s]tatic-server.py --root dist --bind 0.0.0.0 --port $PORT' || true; echo stopped"
  exit 0
fi

"${SSH[@]}" "$HOST" "mkdir -p '$REMOTE'"

# 只传源码：node_modules、dist 不过网；--checksum 按内容判断。
rsync -rlpD --checksum --delete --delete-excluded \
  --exclude 'node_modules/' --exclude 'dist/' --exclude '.DS_Store' \
  -e "${SSH[*]}" "$SITE"/ "$HOST:$REMOTE/"

set +e
"${SSH[@]}" "$HOST" "CELADON_REMOTE=$(printf '%q' "$REMOTE") bash -euo pipefail -s" <<'REMOTE_SCRIPT'
cd "$HOME/$CELADON_REMOTE"
export CI=1
if [ -f pnpm-lock.yaml ]; then
  pnpm install --frozen-lockfile --prefer-offline 2>&1 | tail -n 5 || pnpm install --prefer-offline 2>&1 | tail -n 5
else
  pnpm install --prefer-offline 2>&1 | tail -n 5
fi
echo "--- typecheck ---"
pnpm exec tsc --noEmit
echo "--- build ---"
pnpm exec vite build 2>&1 | tail -n 40
echo "--- dist ---"
du -sh dist; find dist -type f | wc -l
REMOTE_SCRIPT
status=$?
set -e
if [ "$status" -ne 0 ]; then
  echo "远端构建失败（退出码 $status）" >&2
  exit "$status"
fi

# 拉回产物与锁文件（锁文件入库，产物不入库）。
rsync -rlpD --delete -e "${SSH[*]}" "$HOST:$REMOTE/dist/" "$SITE/dist/"
rsync -lpD -e "${SSH[*]}" "$HOST:$REMOTE/pnpm-lock.yaml" "$SITE/pnpm-lock.yaml"
echo "dist/ 与 pnpm-lock.yaml 已拉回 $SITE"

if [ "$MODE" = "serve" ]; then
  # SPA：深链接要回落到 index.html，用 deploy/static-server.py（随站点一起同步过去，--no-cache）。
  # 端口已在监听就复用（dist/ 原地更新，旧进程直接服务新文件），不 kill；要停用 stop。
  "${SSH[@]}" "$HOST" "cd '$REMOTE' && if ss -ltn 'sport = :$PORT' | grep -q LISTEN; then echo 'serve already listening'; else (setsid nohup python3 deploy/static-server.py --root dist --bind 0.0.0.0 --port $PORT --no-cache > serve.log 2>&1 < /dev/null &); sleep 0.8; fi; ss -ltn 'sport = :$PORT' | grep -q LISTEN && echo \"serving http://\$(hostname -I | awk '{print \$1}'):$PORT\" || (echo 'serve failed'; cat serve.log; exit 1)"
fi
