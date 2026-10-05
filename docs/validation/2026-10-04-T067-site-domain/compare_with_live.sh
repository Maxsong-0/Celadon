#!/usr/bin/env bash
# 把本地构建的 site/dist 与线上（https://celadonaudio.com）逐个比：只读 GET，不写线上任何东西。
# 比的是 HTML 外壳、robots.txt、sitemap.xml、_redirects、og 图与 favicon；HTML 里 assets/ 的文件名（带内容哈希、线上手工补丁还加了
# -domain-20261002-celadonaudio 后缀）统一成 X，线上 Cloudflare 注入的 <script>window.__CF$cv$params…</script> 也不算差别。
# 用法（在 site/ 下，先 pnpm run build）：bash ../docs/validation/2026-10-04-T067-site-domain/compare_with_live.sh [线上根地址]
set -u
LIVE=${1:-https://celadonaudio.com}
D=dist; T=$(mktemp -d)
norm() { sed -E 's#assets/[^"]+#assets/X#g; s#<script>window\.__CF[^<]*</script>##g' "$1"; }
fetch() { curl -sL --max-time 30 "$LIVE$1" -o "$T/$2" ; }
fetch / index.html; fetch /zh/ index.zh.html; fetch /robots.txt robots.txt; fetch /sitemap.xml sitemap.xml; fetch /_redirects _redirects
fetch /og-en.png og-en.png; fetch /og.png og.png; fetch /favicon.svg favicon.svg
for f in index.html index.zh.html robots.txt sitemap.xml _redirects; do
  if diff <(norm "$D/$f") <(norm "$T/$f") >/dev/null; then echo "相同  $f"; else echo "不同  $f"; diff <(norm "$D/$f") <(norm "$T/$f") | head -8; fi
done
for f in og-en.png og.png favicon.svg; do
  if cmp -s "$D/$f" "$T/$f"; then echo "相同  $f"; else echo "不同  $f（本地 $(stat -c %s "$D/$f") 字节，线上 $(stat -c %s "$T/$f") 字节）"; fi
done
rm -rf "$T"
