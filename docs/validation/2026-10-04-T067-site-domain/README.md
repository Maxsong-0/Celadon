# T-067 · 官网域名跟上线上的 celadonaudio.com（2026-10-04 夜，DGX Spark 本机）

## 起因

Max 2026-10-02 注册了 **celadonaudio.com**（日记 2026-10-02），线上站点随后手工迁了过去：`https://celadon.maxsong.me/` 现在 301 到 `https://celadonaudio.com/`，线上 HTML 的 canonical / og / sitemap / robots 都已是新域名，
JS 文件名带 `-domain-20261002-celadonaudio` 后缀（手工替换域名后改名防缓存）。**仓库没有跟**：站点源码里写死的 `https://celadon.maxsong.me` 还在 7 处，档案里也没有迁域名的记录。
后果：下次从 main 构建并部署（`site/scripts/build-linux.sh` + `site/deploy/deploy.sh`）会把线上的规范地址、分享卡片、站点地图全部改回旧域名。

## 改了什么

| 位置 | 改动 |
|---|---|
| `site/scripts/site-data.mjs` | `ORIGIN` → `https://celadonaudio.com`（站点地图、中文 HTML 外壳读它）并写明来由 |
| `site/index.html` | 小脚本的 `origin` + `og:url` / `og:image` / `twitter:image` 共 4 处 |
| `site/public/robots.txt` | `Sitemap:` 行 |
| `site/src/components/celadon/Nav.tsx` | `SITE_ORIGIN`（站内跳转时 canonical / hreflang 跟着改用它） |
| `site/scripts/make-og.py` | 画在分享卡片上的域名；`public/og.png`、`og-en.png` 用它重新生成 |
| `site/deploy/cloudflared-celadon.service` | 描述（隧道 celadon → celadonaudio.com，旧域名 301） |
| `site/scripts/check-i18n.mjs` | **新增核对 5b**：index.html 的 4 处、robots.txt、`Nav.tsx`、`make-og.py` 里的域名都必须与 `ORIGIN` 一致，漏改一处构建就败（构建链第一步） |

分享卡片：在 Spark 上先用旧域名文字重生成英文卡，**与仓库里的 `og-en.png` 逐字节相同**（生成链确定）；中文卡只差 83 个像素（一个 CJK 字形的抗锯齿，字体版本不同）。
改成新域名后与原图的差别：英文卡 1230 个像素、全在左下角域名那一行（x 160～271，y 534～553）；中文卡 1313 个像素，其中 83 个是上面那个字形。

## 验证

- 构建：`check-i18n` + `tsc --noEmit` + `vite build` + 站点地图 + 中文外壳全过。
- 阳性对照：把旧域名分别放回 `robots.txt`、`index.html`（origin）、`Nav.tsx`、`make-og.py` 一处，`check-i18n` 各报一条并败（输出在日志里）；放回后通过。
- **对线上（只读 GET，`compare_with_live.sh`，输出 `compare-with-live.txt`）**：新构建的 `index.html`、`index.zh.html`（assets 文件名统一、线上 Cloudflare 注入的脚本不算）、`robots.txt`、`sitemap.xml`（184 处域名）、`_redirects`、`favicon.svg` **与线上逐字相同**；
  只有两张分享卡不同——正是本次重新生成、画上新域名的那一行。结论：从 main 构建部署不会再把线上域名改回去。

## 没做 / 限制

- **部署 112**：Spark 上没有 112 的 SSH 私钥（公钥认证被拒）；且 `deploy.sh` 要求 `artifacts/release-0.1.8.8-*/`（现有只到 0.1.8.7），自带的保险会拒绝部署。要部署：在有私钥的机器（Mac）上 `site/scripts/build-linux.sh` → `site/deploy/deploy.sh`，
  或为这次仅文字 / 域名的更新给 `deploy.sh` 一个「沿用线上已有安装包」的模式（本次没改部署脚本）。
- 线上现在的 JS 是手工补丁版（0.1.8.7 时的构建，没有 T-061 / T-062 / T-066 的文字与图修复）；部署后这些才会上线。
