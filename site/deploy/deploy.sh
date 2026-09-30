#!/usr/bin/env bash
# 把 site/dist/（在 107 上构建、已拉回本机）同步到 112，装用户态单元，重启静态服务，健康检查。
# 用法：site/deploy/deploy.sh            # 先 site/scripts/build-linux.sh 构建好 dist/
# 环境：CELADON_SITE_HOST 覆盖目标主机（默认 maxsong@192.168.100.112）。
set -euo pipefail
HOST="${CELADON_SITE_HOST:-maxsong@192.168.100.112}"
SITE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -f "$SITE/dist/index.html" ]] || { echo "缺少 $SITE/dist/index.html：先跑 site/scripts/build-linux.sh"; exit 1; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

echo "▶ 1/3 同步 dist/ 与 deploy/ 到 $HOST:~/celadon-site/"
"${SSH[@]}" "$HOST" 'mkdir -p ~/celadon-site/dist ~/celadon-site/deploy ~/.config/systemd/user'
rsync -rlpD --checksum --delete -e "${SSH[*]}" "$SITE/dist/" "$HOST:celadon-site/dist/"
rsync -rlpD --checksum --delete -e "${SSH[*]}" "$SITE/deploy/" "$HOST:celadon-site/deploy/"

echo "▶ 2/3 装单元并重启静态服务"
"${SSH[@]}" "$HOST" bash -s <<'REMOTE'
set -euo pipefail
cp ~/celadon-site/deploy/celadon-site.service ~/.config/systemd/user/celadon-site.service
cp ~/celadon-site/deploy/cloudflared-celadon.service ~/.config/systemd/user/cloudflared-celadon.service
systemctl --user daemon-reload
systemctl --user enable celadon-site >/dev/null
systemctl --user restart celadon-site
sleep 1
systemctl --user is-active celadon-site
REMOTE

echo "▶ 3/3 服务器本地验收"
"${SSH[@]}" "$HOST" 'curl -fsS -o /dev/null -w "root %{http_code}\n" http://127.0.0.1:8090/ && curl -fsS -o /dev/null -w "deep %{http_code}\n" http://127.0.0.1:8090/docs/install && curl -fsSI http://127.0.0.1:8090/ | grep -i "cache-control"'
echo "✔ 部署完成（隧道单元若已启用无需动：cloudflared 只转发，不缓存）"
