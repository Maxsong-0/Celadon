# T-062 · 官网首页示意图的页眉、图注与标签重叠 / 被裁（2026-10-04）

## 结论

Playwright 全站运行时横扫（46 路由 × 13 屏宽 320 ～ 1920，598 页；减少动效模式另测 26 页），改前 main `fbe91f3` 命中 **9** 处（8 个位置，次声标签中英各一）+ 贴近 3 处，
改后 **命中 0、贴近 0**（正常模式 468 个带字 SVG、3973 段 `<text>`；减少动效 312 个 SVG、1456 段）。构建 `check-i18n` + `tsc --noEmit` + `vite build` 全过，不动 i18n 文案。

| 位置（英文页，中文页无此问题除注明） | 改前 | 根因 | 改法 |
|---|---|---|---|
| 首页「四道工序」CLAY 帧页眉：标题与说明 | 重叠 49.9（375 宽）… 6.8 个 SVG 单位 | 页眉把两段可变长文字钉在同一基线的两端 | `Frame` 按实际渲染宽度排：留得出 24 个单位同一行，否则标题上移一行 |
| 同上 COLOR 帧 | 重叠 113 ～ 180 | 同上 | 同上 |
| AlgoTeaser LIMIT / COLOR 小图的图注 | 被 viewBox 右边裁掉半句（超出 113 / 48 个单位） | 单行图注固定坐标，名义宽度就大于 viewBox | `WrappedText`：放不下按词折行，向上长 |
| LIMIT 帧底部链路说明 | 320 宽超出 8.8 | **字形步进被取整**（见下） | `svg text { text-rendering: geometricPrecision }` |
| Bento ArtRef 图注 | 375 / 414 宽超出 2.8 / 1.9 | 同上 | 同上 |
| CLAY 帧「Low cut 25 Hz」被收单虚线穿过 | 13 个屏宽都越线（最深 25 个单位） | 标签只预留了 68 个单位，英文名义宽 78 | `WrappedText` 折成「Low cut / 25 Hz」（数字与单位粘住） |
| CLAY 帧「Infrasonic / 次声」被频带下沿描边穿过（**中英都有**） | 压入 4.5（中）/ 4.7（英）个单位 | 纵向固定坐标没有避让数据驱动的图形 | 标签挪到频带左上角之上 |

## 根因：Chromium 把 SVG 字形步进按屏幕像素取整

同一段字在 SVG 用户单位下的宽度随缩放比浮动（`geom-probe.out`，`geom-probe.mjs` 量 6 个屏宽 × 14 段字）：

| 文字 | 默认（320 / 375 / 414 / 768 / 1024 / 1440 宽） | `geometricPrecision` |
|---|---|---|
| COLOR 帧说明 | 453.7 / 481.5 / 465.0 / 468.0 / 448.9 / 459.0 | 458.25（全部相同） |
| LIMIT 链路说明 | **588.8** / 493.7 / 443.7 / 462.9 / 471.8 / 460.4 | 498.11（全部相同） |
| AlgoTeaser LIMIT 图注 | 350.0 / 367.5 / 367.5 / 377.0 / 377.0 / 377.0 | 378.02（全部相同） |

默认模式下最多 +18 %（320 宽）。`index.css` 里给 `svg text` 加 `geometricPrecision` 后，14 段字的宽度在所有屏宽下**逐位恒定**，固定坐标从此靠得住；
`Frame` 与 `WrappedText` 仍按量出来的宽度排版（语言不同宽度差一倍以上）。

## 改动

- `site/src/components/celadon/ProcessSection.tsx`：`Frame` 页眉按实际宽度排（原会话 [d0056553] 的修法）；Low cut 标签用 `WrappedText`；Infrasonic 标签位置；
  `data-progress` 的宽度改用常量 `FRAME_RIGHT - FRAME_LEFT`。
- `site/src/components/celadon/SvgText.tsx`（新）：`wrapWords`（纯函数，贪心折行、数字和单位粘住；`wrap.mjs` 用假的量宽度函数验过）+ `WrappedText`（量宽度的隐藏 `<text>`；字体加载完、SVG 尺寸变了都重排）。
- `site/src/components/celadon/AlgoTeaser.tsx`：三张小图的图注改用 `WrappedText`。
- `site/src/index.css`：`svg text { text-rendering: geometricPrecision }`。

## 验证（DGX Spark 本机，Chromium 151 / Playwright 1.62）

- 横扫：`sweep.mjs`（原会话 [d0056553] 写的，量每个 `<text>` 的字形盒，找重叠 / 越界 / 压图形 / 贴近）。改前 `sweep-before.json`（原会话量）、改后 `sweep-after.json`；减少动效 `sweep-{before,after}-reduced.json`。
  改后行数比改前多 39 行（2 条英文图注 + Low cut 各 13 个屏宽的折行），正好等于预期。
- 截图 `screenshots/`：英文 CLAY 帧（1440 / 375 宽）、次声标签、AlgoTeaser 英文三张图改前 / 改后；`clay-zh-…` 是中文帧。
- 中文页「观感不变」：`geometricPrecision` 只让字形步进有亚像素级差别，改前 / 改后 16 张中文帧（4 帧 × 1440 / 375 宽 × 处理前 / 后）0.55 ～ 1.6 % 的像素有差、差 ≥ 40 灰度的占 0.36 ～ 0.9 %，全在字形边缘，肉眼一致（不再逐字节相同）。

## 没做 / 限制

- 只在 Linux Chromium 量过；Safari / Firefox 没有这个取整问题（本来按未取整的步进排），`geometricPrecision` 对它们无影响，但没实测。
- **部署 112**：先问 Max；Spark 现在连不上 112。
- 复现：`sweep.mjs` 里 Playwright 的路径写死成本机 `~/.npm/_npx/…`，源码对位读 `../base/site`；`shoot-*.mjs` 同。
