#!/usr/bin/env bash
# 在 107 上生成分享卡片 public/og.png（中文）或 og-en.png（英文，--lang en），1200 × 630，再拉回本机（全局纪律：本机 Mac 不装、不跑）。
# 画法见 make-og.py 开头；这里只管同步、venv 与拉回。
#
# 用法：site/scripts/make-og.sh                 # 默认参数
#       site/scripts/make-og.sh --lang en       # 英文版 public/og-en.png（ADR-024）
#       site/scripts/make-og.sh --value 0.6     # 参数原样传给 make-og.py（--lang / --value / --seed / --scale）
# 前提：先跑过一次 site/scripts/build-linux.sh——远端目录里要有 node_modules（用站点自带的 Fraunces 与 JetBrains Mono），
#       中文字用服务器上的 Noto Serif CJK SC（fonts-noto-cjk），英文版另用站点自带的 Inter。
# 环境：CELADON_TEST_HOST 覆盖目标主机（默认 maxsong@192.168.100.107）。
set -euo pipefail
SITE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
HOST="${CELADON_TEST_HOST:-maxsong@192.168.100.107}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
KEY="$(printf '%s' "$SITE" | shasum -a 256 | cut -c1-12)"
# 与 build-linux.sh 同一个远端目录（按工作树路径区分，多个工作树互不干扰）
REMOTE=".cache/celadon-site/$(basename "$(dirname "$SITE")")-$KEY"
VENV=".venvs/celadon-og"

if ! "${SSH[@]}" "$HOST" "test -d '$REMOTE/node_modules/@fontsource-variable/fraunces'"; then
  echo "远端还没有 node_modules：先跑 site/scripts/build-linux.sh" >&2
  exit 1
fi

ARGS=""
if [ $# -gt 0 ]; then ARGS="$(printf '%q ' "$@")"; fi
# 产物名跟语言走（与 make-og.py 的默认一致）
OUT="og.png"
prev=""
for a in "$@"; do
  if [ "$prev" = "--lang" ] && [ "$a" = "en" ]; then OUT="og-en.png"; fi
  if [ "$a" = "--lang=en" ]; then OUT="og-en.png"; fi
  prev="$a"
done

rsync -lpD --checksum -e "${SSH[*]}" "$SITE/scripts/make-og.py" "$HOST:$REMOTE/scripts/make-og.py"
"${SSH[@]}" "$HOST" "set -e
test -x ~/$VENV/bin/python || python3 -m venv ~/$VENV
~/$VENV/bin/python -m pip install --quiet --disable-pip-version-check numpy pillow scipy fonttools brotli
cd '$REMOTE' && nice ~/$VENV/bin/python scripts/make-og.py --out public/$OUT $ARGS"
rsync -lpD -e "${SSH[*]}" "$HOST:$REMOTE/public/$OUT" "$SITE/public/$OUT"
echo "public/$OUT 已拉回 $SITE"
