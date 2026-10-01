#!/usr/bin/env bash
# 把 Cloudflare 隧道 token 从本机 wrangler OAuth 直接写进 112 的 ~/.config/cloudflared/celadon.env（600）。
# token 只在 curl → ssh 管道里流转，不落本机磁盘、不进日志 / 对话。用法：site/deploy/push-tunnel-token.sh [tunnel_id]
set -euo pipefail
HOST="${CELADON_SITE_HOST:-maxsong@192.168.100.112}"
ACCOUNT="a5f4ec4bf026935dd8eacb970d85c503"
TUNNEL_ID="${1:-${CELADON_TUNNEL_ID:-}}"
[[ -n "$TUNNEL_ID" ]] || { echo "用法：push-tunnel-token.sh <tunnel_id>（或设 CELADON_TUNNEL_ID）"; exit 1; }
CFG="$HOME/Library/Preferences/.wrangler/config/default.toml"

wrangler whoami >/dev/null 2>&1 || true          # 顺手刷新 OAuth（约 1 小时过期）
OAUTH="$(sed -n 's/^oauth_token = "\(.*\)"/\1/p' "$CFG")"
[[ -n "$OAUTH" ]] || { echo "读不到 wrangler oauth_token，请先 wrangler login"; exit 1; }

curl -fsS -H "Authorization: Bearer $OAUTH" \
  "https://api.cloudflare.com/client/v4/accounts/$ACCOUNT/cfd_tunnel/$TUNNEL_ID/token" \
  | jq -re '.result' \
  | ssh -o BatchMode=yes "$HOST" 'umask 077; mkdir -p ~/.config/cloudflared; { printf "TUNNEL_TOKEN="; cat; } > ~/.config/cloudflared/celadon.env.tmp && mv ~/.config/cloudflared/celadon.env.tmp ~/.config/cloudflared/celadon.env && stat -c "%a %n %s bytes" ~/.config/cloudflared/celadon.env'
echo "✔ 隧道 token 已写入 112；启动：ssh $HOST systemctl --user enable --now cloudflared-celadon"
