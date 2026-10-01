# T-033 · 官网合并后的逐页走查（2026-10-01）

对象是 PR #9 合进 main 的 `site/`，内容与分支 `claude/website-optimization-design-ec62ba` 的 `c17176f` 相同（含 T-031 / PR #6 的站点）。
走查在云端容器上做：Chromium 1194，Playwright。站点在本机构建（`pnpm install --frozen-lockfile`、`tsc --noEmit`、
`CELADON_VERSION=0.1.8.5 vite build`），用 `site/deploy/static-server.py` 提供，与 112 的生产服务同一套 SPA 回退与缓存头。

## 结果

| 项 | 结果 |
|---|---|
| 路由 × 视口 | 25 个路由 × 桌面 1440×900 / 手机 390×844，共 50 次：控制台错误 0、页面异常 0、失败请求（≥ 400 或网络错误）0、手机宽度横向溢出 0；每页正文都在 |
| 路由清单 | `/`；`/docs` 与 12 个子页；`/algorithms` 与 6 个子页；`/algorithms/references`（已删的那页，跳到 `/algorithms`）；`/download`；`/changelog`；一个不存在的地址（显示 404 页「这一页没烧出来」） |
| 站内链接 | 页面上出现的 27 个站内链接逐个打开，都不是 404。`/downloads/` 下的 4 个（三个 0.1.8.5 安装包与 `SHA256SUMS`）本机没有安装包目录、没测：部署到 112 后由 `~/celadon-site/downloads/` 提供，`deploy.sh` 会核对文件名与校验和 |
| ⌘K / Ctrl+K 搜索 | 对话框打开、输入框拿到焦点，输入「GLAZE」出 80 条结果，Esc 关闭 |
| 本页目录 | 使用说明页「GLAZE 与风格」有 6 个锚点 |
| 分享信息 | `og:image`、`og:title` 都在 |
| 等宽字体 | `870efad` 关连字生效：等宽元素 `font-variant-ligatures: none`（`font-feature-settings` 为 `"kern", "palt"`） |
| 文字审查 | `src/`、`public/`、`index.html` 与构建产物里没有 Gullfoss / God Particle / Invisible Limiter / Soundtheory / Cradle / A.O.M. / sonible / pure:limit / 对标 / 模仿 / 商业插件 / 三款；内部任务编号只出现在两处源码注释里（`src/content/listen.ts`、`src/components/react-bits/README.md`），页面上没有；显示的版本号为 0.1.8.5 |

## 合并前的审查修正（都在 PR #9 里）

- `d8f82cc`：首屏渐变字在 Chrome 里不显示、试听节隐藏时的死链、搜索关闭后焦点丢失等
- `c367c79`：算法页与常见问题去掉内部编号
- `e47e52a`：思源字体的 `@font-face` 改为异步插入，不再挡首次渲染
- `6eeea7f`：会变的读数改用等宽数字格（Fraunces 没有 tnum）
- `1645dfd`：跑马灯停手后回到向左
- `c17176f`：导航「工艺」与带锚点的链接滚不到位

## 还没做的

- 部署到 112（只能在 Mac 上做，见下）。
- 试听节在 Max 放曲目进 `artifacts/site-ab/source/` 并跑 `site/scripts/make-listen.sh` 之前不显示（设计如此）。
- 第二批设计升级时留下的三条：跑马灯方向已修（`1645dfd`）；Fraunces 没有等宽数字，会变的读数已改用等宽数字格（`6eeea7f`）；
  中文字体 CSS 改成了异步插入、不再挡首次渲染（`e47e52a`），但体积本身（约 209 KB）没减，要减得改字体子集的切法。

## 部署（Mac）

1. `site/scripts/build-linux.sh`：在 107 上构建，把 `dist/` 拉回本机。
2. `site/deploy/deploy.sh check`：只做本机检查，不连服务器——dist、发布目录、校验和，以及站点里写的安装包信息。
3. `site/deploy/deploy.sh`：同步到 112，重启静态服务，做健康检查。第一次部署会建 `~/celadon-site/downloads/`，开始对外提供 0.1.8.5 的安装包。顺序是先加新安装包、再换站点、最后删旧安装包。
