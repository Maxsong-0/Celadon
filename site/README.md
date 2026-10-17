# Celadon 站点（宣传 · 使用说明 · 公开算法）

静态站：Vite 8 + React 19 + TypeScript + Tailwind CSS v4 + GSAP 3（@gsap/react、ScrollTrigger、SplitText）+ React Bits 免费组件（`src/components/react-bits/`，MIT + Commons Clause，见该目录 README）。

三条路由：`/` 宣传页；`/docs/*` 使用说明；`/algorithms/*` 公开算法。正文是 `src/content/**/*.md`，按页懒加载；示意图在 `src/components/docs/Figures.tsx`，Markdown 里用 `![说明](figure:key)` 引用。

## 构建（不在 Mac 上）

```bash
site/scripts/build-linux.sh          # 107 上 pnpm install + tsc + vite build，拉回 dist/ 与 pnpm-lock.yaml
site/scripts/build-linux.sh serve    # 同上，并在 107 起静态服务 http://192.168.100.107:8787 供浏览器验收
site/scripts/build-linux.sh stop     # 停掉静态服务
```

部署：把 `dist/` 放到任何静态托管即可。`public/_redirects` 让 Cloudflare Pages 把深链接回落到 `index.html`；部署到子路径时 `SITE_BASE=/celadon/ vite build`。

## 内容真源

- 控件文案：`Source/ui/HelpText.cpp`（三行：名字 · 作用 · 建议）、`Source/ui/Localisation.cpp`。
- 算法：仓库 `docs/*.md`、`docs/design/`、档案 `01-框架/设计规格-*`。数字只引已有验证记录。
- 调色：`Source/ui/CeladonLookAndFeel.h`（c100 ～ c900、紫 = GLAZE 自动、橙 = 需要注意）。
- 旋钮手感：`Source/ui/DialMotion.h`（常数）与 `Source/ui/Dial.cpp`（外圈 / 中间的拖法）。网站复刻在 `src/components/celadon/dialMotion.ts`、`useDialControl.ts`，逐条对应；插件改了手感常数，同步 `dialMotion.ts` 后跑 `pnpm run check:dial`（复现插件 `tests/DialMotionTests.cpp`，也随 build 跑）。
