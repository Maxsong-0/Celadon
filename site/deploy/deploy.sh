#!/usr/bin/env bash
# 把 site/dist/（在 107 上构建、已拉回本机）与这一版的安装包同步到 112，装用户态单元，重启静态服务，健康检查。
#
# 用法：site/deploy/deploy.sh          # 先 site/scripts/build-linux.sh 构建好 dist/
#       site/deploy/deploy.sh check    # 只做本机检查（dist、发布目录、校验和、站点里写的安装包信息），不连服务器
# 环境：CELADON_SITE_HOST 覆盖目标主机（默认 maxsong@192.168.100.112）；
#       CELADON_ARTIFACTS 覆盖安装包根目录（默认主仓库的 artifacts/：在 worktree 里跑也去主仓库找，artifacts/ 不入 git）。
#
# 安装包：版本号照 build-linux.sh 从 Celadon.jucer 读，取 artifacts/release-<版本>-<日期>/ 里 SHA256SUMS 列的文件，
# 连同 SHA256SUMS 同步到 112 的 ~/celadon-site/downloads/（静态服务在 /downloads/ 下提供）。
# 这一版的发布目录不在、校验和对不上、或 dist 里 src/content/release.ts 写的文件名与校验值和发布目录不一致，都停下报错，不部署半截。
# 顺序：先加新安装包 → 再换站点并重启 → 最后删旧安装包；任何时刻线上页面里的下载链接都有文件。
set -euo pipefail
HOST="${CELADON_SITE_HOST:-maxsong@192.168.100.112}"
SITE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
MODE="${1:-deploy}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

die() {
  echo "✘ $*" >&2
  exit 1
}
case "$MODE" in
  deploy | check) ;;
  *) die "用法：$0 [check]" ;;
esac

# ---------- 本机检查（check 与 deploy 都做） ----------

[[ -f "$SITE/dist/index.html" ]] || die "缺少 $SITE/dist/index.html：先跑 site/scripts/build-linux.sh"
VERSION="$(sed -n 's/^ *version="\([0-9.]*\)".*/\1/p' "$SITE/../Celadon.jucer" | head -1)"
[ -n "$VERSION" ] || die "Celadon.jucer 里读不到 version"

# 发布目录在主仓库的 artifacts/ 下；worktree 里经 git 的公共目录（<主仓库>/.git）找回主仓库根。
if [ -n "${CELADON_ARTIFACTS:-}" ]; then
  ARTIFACTS="$CELADON_ARTIFACTS"
else
  COMMON="$(cd "$SITE" && cd "$(git rev-parse --git-common-dir)" && pwd -P)" || die "找不到 git 仓库：设 CELADON_ARTIFACTS 指向 artifacts/"
  [ "$(basename "$COMMON")" = ".git" ] || die "git 公共目录 $COMMON 不是 <仓库>/.git：设 CELADON_ARTIFACTS 指向 artifacts/"
  ARTIFACTS="$(dirname "$COMMON")/artifacts"
fi
RELEASES=()
for d in "$ARTIFACTS"/release-"$VERSION"-*/; do
  [ -d "$d" ] && RELEASES+=("${d%/}")
done
[ ${#RELEASES[@]} -gt 0 ] || die "找不到 $ARTIFACTS/release-$VERSION-*/：$VERSION 的安装包还没出，不部署（站点与安装包要一起上）"
REL="${RELEASES[${#RELEASES[@]}-1]}" # 名字里带日期、按名字排序：有多个时取最新的
[ ${#RELEASES[@]} -eq 1 ] || echo "⚠ 有 ${#RELEASES[@]} 个 release-$VERSION-* 目录，用最新的 $(basename "$REL")"
[ -f "$REL/SHA256SUMS" ] || die "$REL 里没有 SHA256SUMS"

# 清单 = SHA256SUMS 列的文件：文件名要合规（静态服务只认这种名字）、要是这一版的、要真在目录里。
FILES=()
SUMS=()
while read -r sum name || [ -n "${sum:-}" ]; do
  [ -n "${sum:-}" ] || continue
  name="${name#\*}" # shasum -b 写出的 * 前缀
  [[ "$sum" =~ ^[0-9a-f]{64}$ ]] || die "SHA256SUMS 里有一行不是 SHA-256：$sum"
  [[ "$name" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] || die "SHA256SUMS 里的文件名不合规：$name"
  [[ "$name" == Celadon-"$VERSION"-* ]] || die "$name 不是 $VERSION 的安装包"
  [ -f "$REL/$name" ] || die "SHA256SUMS 列了 ${name}，发布目录里却没有"
  FILES+=("$name")
  SUMS+=("$sum")
done <"$REL/SHA256SUMS"
[ ${#FILES[@]} -gt 0 ] || die "$REL/SHA256SUMS 是空的"
out="$(cd "$REL" && shasum -a 256 -c SHA256SUMS 2>&1)" || die "$(basename "$REL") 里的安装包与 SHA256SUMS 对不上：
$out"

# 站点里写死的安装包信息（src/content/release.ts，编进下载页的 JS）必须正好是这批文件：两个方向都核对。
CHUNK="$(grep -rlF -- "${SUMS[0]}" "$SITE/dist/assets" | head -1 || true)"
[ -n "$CHUNK" ] || die "dist 里找不到 ${FILES[0]} 的 SHA-256：src/content/release.ts 与 $(basename "$REL") 不一致，先改 release.ts 再构建"
for i in "${!FILES[@]}"; do
  grep -qF -- "${SUMS[$i]}" "$CHUNK" || die "下载页里没有 ${FILES[$i]} 的 SHA-256：先改 src/content/release.ts 再构建"
  grep -qF -- "${FILES[$i]}" "$CHUNK" || die "下载页里没有文件名 ${FILES[$i]}：先改 src/content/release.ts 再构建"
done
for linked in $(grep -oE 'Celadon-[0-9][0-9.]*-[A-Za-z0-9._+-]+' "$CHUNK" | sort -u); do
  found=0
  for name in "${FILES[@]}"; do [ "$name" = "$linked" ] && found=1; done
  [ "$found" = 1 ] || die "下载页链接了 ${linked}，但它不在 $(basename "$REL")/SHA256SUMS 里"
done

echo "✔ 本机检查通过：版本 ${VERSION}，发布目录 $(basename "$REL")，${#FILES[@]} 个安装包 + SHA256SUMS，与下载页一致"
for i in "${!FILES[@]}"; do
  # 字节数：GNU stat（Linux，Spark / 服务器）用 -c%s，BSD stat（macOS）用 -f%z；两种写法互不认，先试前一种。
  printf '    %-44s %9s 字节  %s…\n' "${FILES[$i]}" "$(stat -c%s "$REL/${FILES[$i]}" 2>/dev/null || stat -f%z "$REL/${FILES[$i]}")" "${SUMS[$i]:0:12}"
done
[ "$MODE" = check ] && exit 0

# ---------- 部署 ----------

# 只传清单里的文件；第 4 步再加 --delete-excluded，把 downloads/ 里清单以外的旧文件删掉。
# 不带 -p / --chmod：权限按服务器 umask 定（本机 openrsync 的 --chmod 写法与 GNU rsync 不通用），服务本身就是文件属主。
FILTER=(--include=/SHA256SUMS)
for name in "${FILES[@]}"; do FILTER+=("--include=/$name"); done
FILTER+=(--exclude='*')
RSYNC_DL=(rsync -rt --checksum -e "${SSH[*]}")

echo "▶ 1/4 上传安装包到 $HOST:~/celadon-site/downloads/"
"${SSH[@]}" "$HOST" 'mkdir -p ~/celadon-site/dist ~/celadon-site/deploy ~/celadon-site/downloads ~/.config/systemd/user'
"${RSYNC_DL[@]}" "${FILTER[@]}" "$REL/" "$HOST:celadon-site/downloads/"
"${SSH[@]}" "$HOST" 'cd ~/celadon-site/downloads && sha256sum --quiet -c SHA256SUMS' || die "112 上的安装包校验不过（上传不完整？）：站点还没换，重跑即可"

echo "▶ 2/4 同步 dist/ 与 deploy/ 到 $HOST:~/celadon-site/"
rsync -rlpD --checksum --delete -e "${SSH[*]}" "$SITE/dist/" "$HOST:celadon-site/dist/"
rsync -rlpD --checksum --delete -e "${SSH[*]}" "$SITE/deploy/" "$HOST:celadon-site/deploy/"

echo "▶ 3/4 装单元并重启静态服务"
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

echo "▶ 4/4 删掉旧版安装包，服务器本地验收"
"${RSYNC_DL[@]}" --delete --delete-excluded "${FILTER[@]}" "$REL/" "$HOST:celadon-site/downloads/"
"${SSH[@]}" "$HOST" "FILES=$(printf '%q' "${FILES[*]}") bash -s" <<'REMOTE' || die "部署后自检没过（见上面的 ✘）"
set -uo pipefail
base=http://127.0.0.1:8090
fail=0
expect() { # 说明 期望 实际
  if [ "$2" = "$3" ]; then echo "  ✔ $1：$3"; else echo "  ✘ $1：期望 $2，实际 $3"; fail=1; fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
header() { printf '%s\n' "$1" | sed -n "s/^$2: //Ip" | head -1; }
expect "首页" 200 "$(code "$base/")"
expect "深链接 /docs/install" 200 "$(code "$base/docs/install")"
expect "下载页 /download" 200 "$(code "$base/download")"
expect "首页缓存头" no-cache "$(header "$(curl -sI "$base/" | tr -d '\r')" cache-control)"
expect "缺失的旧脚本 /assets/…js" 404 "$(code "$base/assets/celadon-deploy-check-missing.js")"
for f in $FILES SHA256SUMS; do
  h="$(curl -sI "$base/downloads/$f" | tr -d '\r')"
  expect "HEAD /downloads/$f" 200 "$(printf '%s\n' "$h" | sed -n '1s/^HTTP\/[0-9.]* \([0-9]*\).*/\1/p')"
  expect "  附件下载" "attachment; filename=\"$f\"" "$(header "$h" content-disposition)"
  expect "  大小" "$(stat -c%s ~/celadon-site/downloads/"$f")" "$(header "$h" content-length)"
done
set -- $FILES
expect "断点续传（Range 0-1023）" 206 "$(code -r 0-1023 "$base/downloads/$1")"
exit $fail
REMOTE
echo "✔ 部署完成：站点与 $VERSION 的安装包都已上线（隧道单元若已启用无需动：cloudflared 只转发，不缓存）"
