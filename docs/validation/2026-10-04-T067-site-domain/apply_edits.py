#!/usr/bin/env python3
"""T-067：官网域名跟上线上的 celadonaudio.com（Max 2026-10-02 注册并迁移，线上手工改过；仓库一直没跟）。
把站点源码里写死的 https://celadon.maxsong.me 全改成 https://celadonaudio.com，并在 check-i18n 里加「各处与 ORIGIN 一致」的核对，
以后再有人只改一处，构建就败。每条先断言 old 恰好出现一次（对不上就停，一个文件都不写）；--dry-run 只报匹配。用法（仓库根）：python3 <本文件> [--dry-run]"""
import pathlib, sys

dry = "--dry-run" in sys.argv
OLD, NEW = "https://celadon.maxsong.me", "https://celadonaudio.com"
S = pathlib.Path("site")
files = {p: p.read_text(encoding="utf-8") for p in (
    S / "scripts/site-data.mjs", S / "index.html", S / "public/robots.txt", S / "src/components/celadon/Nav.tsx",
    S / "scripts/make-og.py", S / "deploy/cloudflared-celadon.service", S / "scripts/check-i18n.mjs")}
log = []

def sub(path, name, old, new, count=1):
    n = files[path].count(old)
    log.append((n == count, f"{path.name}: {name}（匹配 {n} 处，要求 {count}）"))
    if n == count:
        files[path] = files[path].replace(old, new)

sub(S / "scripts/site-data.mjs", "ORIGIN", f"export const ORIGIN = '{OLD}';",
    f"// 站点的规范地址（canonical / hreflang / og:url / 站点地图都用它）。2026-10-02 起是 celadonaudio.com（Max 注册并迁移，旧域名 celadon.maxsong.me 301 过来）；\n"
    f"// 写死在这里之外的几处（index.html、robots.txt、Nav.tsx、make-og.py）由 check-i18n 核对与它一致。\n"
    f"export const ORIGIN = '{NEW}';")
sub(S / "index.html", "origin 与 og / twitter 地址", OLD, NEW, count=4)
sub(S / "public/robots.txt", "站点地图", OLD, NEW)
sub(S / "src/components/celadon/Nav.tsx", "SITE_ORIGIN", f"const SITE_ORIGIN = '{OLD}';", f"const SITE_ORIGIN = '{NEW}';   // 与 scripts/site-data.mjs 的 ORIGIN 一致（check-i18n 核对）")
sub(S / "scripts/make-og.py", "卡片上的域名", "'celadon.maxsong.me'", "'celadonaudio.com'")
sub(S / "deploy/cloudflared-celadon.service", "单元描述", "隧道 celadon → celadon.maxsong.me）", "隧道 celadon → celadonaudio.com；旧域名 celadon.maxsong.me 在 Cloudflare 上 301 到它）")

# check-i18n：各处写死的站点地址与 ORIGIN 一致
sub(S / "scripts/check-i18n.mjs", "import 加 ORIGIN",
    "import { LOCALES, SITE, allPaths, read, readJson, readNav } from './site-data.mjs';",
    "import { LOCALES, ORIGIN, SITE, allPaths, read, readJson, readNav } from './site-data.mjs';")
sub(S / "scripts/check-i18n.mjs", "5b 各处写死的站点地址与 ORIGIN 一致",
    "const html = read('index.html');",
    "const html = read('index.html');\n"
    "\n"
    "// ---- 5b. 写死的站点地址与 ORIGIN（site-data.mjs）一致：index.html 的 origin / og:url / og:image / twitter:image、robots.txt 的站点地图、\n"
    "//         Nav.tsx 的 SITE_ORIGIN、make-og.py 画在分享卡片上的域名。线上迁域名时只改了线上的文件、仓库没跟，下次部署就把域名改回去过——\n"
    "//         现在谁漏改一处，构建就败。\n"
    "{\n"
    "  const host = new URL(ORIGIN).host;\n"
    "  const expectOrigin = [\n"
    "    [html.match(/var origin = '([^']+)'/)?.[1], 'index.html 的 var origin'],\n"
    "    [html.match(/property=\"og:url\" content=\"([^\"]+?)\\/?\"/)?.[1], 'index.html 的 og:url'],\n"
    "    [html.match(/property=\"og:image\" content=\"([^\"]+?)\\/og-en\\.png\"/)?.[1], 'index.html 的 og:image'],\n"
    "    [html.match(/name=\"twitter:image\" content=\"([^\"]+?)\\/og-en\\.png\"/)?.[1], 'index.html 的 twitter:image'],\n"
    "    [read('public/robots.txt').match(/^Sitemap: (\\S+?)\\/sitemap\\.xml/m)?.[1], 'public/robots.txt 的 Sitemap'],\n"
    "    [read('src/components/celadon/Nav.tsx').match(/const SITE_ORIGIN = '([^']+)'/)?.[1], 'Nav.tsx 的 SITE_ORIGIN'],\n"
    "  ];\n"
    "  for (const [got, label] of expectOrigin) if (got !== ORIGIN) fail(`${label} 是 ${got ?? '（找不到）'}，应与 site-data.mjs 的 ORIGIN 一致：${ORIGIN}`);\n"
    "  const og = read('scripts/make-og.py').match(/draw_tracked\\(draw, x, 548 \\* S, '([^']+)'/)?.[1];\n"
    "  if (og !== host) fail(`make-og.py 画在分享卡片上的域名是 ${og ?? '（找不到）'}，应为 ${host}（改完要重新生成 public/og.png 与 og-en.png）`);\n"
    "}")

bad = [m for ok, m in log if not ok]
for ok, m in log:
    print(("OK  " if ok else "*** ") + m)
if bad:
    sys.exit(f"{len(bad)} 条对不上，一个文件都没写")
if not dry:
    for p, t in files.items():
        p.write_text(t, encoding="utf-8")
    print(f"已写入 {len(files)} 个文件")
