# T-069 · 官网示意图加固：T-062 评审回修 + Firefox / WebKit 实测（2026-10-04 夜，DGX Spark 本机）

夜间评审（workflow `wf_3e784f42-2fa`，代理 `abce83fb8c220302d`）对 T-062（首页示意图页眉 / 图注 / 标签重叠）的 9 条发现，逐条核实后处理；评审点出的「Safari / Firefox 没实测」被当成真问题查——
**在 Spark 上装了 Playwright 的 Firefox 153 与 WebKit 26.5 实测，WebKit 里有一处 main 里就有的重叠**（CLAY 帧页眉的标题压在说明上），本任务一并修掉。

## 新发现：WebKit 的 `getComputedTextLength()` 不算 `letter-spacing`

`Frame`（T-062）按「标题宽 + 说明宽」判断同一行放不放得下，宽度用 `getComputedTextLength()` 量。三个引擎量同一段字（CLAY 帧标题 `CLAY · STEREO WIDTH vs FREQUENCY`，32 个字、字距 0.18em = 2.16 单位，1440 宽）：

| 引擎 | `getComputedTextLength()` | `getBBox().width` | 页眉的实际行为 |
|---|---|---|---|
| Chromium 151 | 299.53 | 299.53 | 放不下 → 标题上移一行（y = 26） |
| Firefox 153 | 299.71 | 299.71 | 同上 |
| **WebKit 26.5** | **230.40**（少算 32 × 2.16 ≈ 69 单位） | 297.38 | **以为放得下 → 不上移（y = 44），标题与说明重叠 17.3 个单位**（320 / 375 / 768 / 1440 宽都是） |

`getBBox().width` 三个引擎一致（WebKit 比另两家少末字后面那一份字距，约 2 个单位，在 `FRAME_GAP` 的余量之内；无字距的文字三家逐位相同）。改：新增 `site/src/lib/svgMeasure.ts` 的 `measureTextWidth`，`Frame` 与 `WrappedText` 都用它。
截图 `screenshots/webkit-clay-{old,new}-{1440,375}.png`：改前标题 `…FREQUENCY` 与说明 `Centre line = mono…` 叠在一起（「FREQUENCYentre line」），改后标题上移一行。

## 评审 9 条

| # | 严重度 | 核实 | 处理 |
|---|---|---|---|
| F1 | low（plausible） | ✔ T-062 README 写「Safari / Firefox 无影响」没实测、没有根据 | **实测**：Firefox 153 命中 0；WebKit 26.5 一处真重叠（上节）→ 修；T-062 README 那句划掉并更正 |
| F2 | info | ✔ React 19 里 ResizeObserver / 字体回调里的 `setState` 默认优先级，晚一帧生效 | `WrappedText`、`Frame` 的回调里用 `flushSync`（layout effect 里首次测量不包，否则 React 警告）；dev 构建三个引擎滚动 + 缩放窗口的控制台里没有任何 React 警告 |
| F3 | info | ✔ `wrapWords` 只在 ASCII 空白处断，没有空格的中文整段算一个词、会越界 | 折行逻辑搬到纯函数 `lib/wrapWords.ts`：中日韩按字断；句读 / 右括号 / 引号 / 破折号 / 省略号 / 间隔号不落行首、左括号 / 引号不落行尾；数字与单位仍粘住 |
| F4 | info | ✔ `grow="up"` 的图注没有行数上限 | `WrappedText` 加 `maxLines`（超限开发期告警）；每张图留的行数写在 `lib/figureBudget.ts`（BALANCE 1 行、LIMIT / COLOR 各 2 行），构建时 `check-svg-text.mjs` 按它核对 i18n 文案（估算折行数） |
| F5 | info | ✔ 页眉说明自己比整条页眉还宽就没有兜底 | 同上文件里页眉说明的宽度预算（整条页眉 − 20，Inter 按 0.55 em 估、实测约 0.51），构建时核对四条说明 × 中英文 |
| F6 | info | ✔ 横扫的「贴近」只查同一行左右间距 | `sweep.mjs` 加 `near-shape`（文字离图形 < 6 个单位，含不到 1.5 的压入）；AlgoTeaser COLOR 图注离两条曲线 4.18 / 4.08 → COLOR / LIMIT 图注基线下移 2 个单位（108 → 110），**不再出现**（> 6）；设计上就贴着自己参考线的标签（收单 / 低切 / 上限标注，4.5 ～ 5.7）原样保留 |
| F7 | info | ✔ 次声标签被低切虚线（x = 70）在「处理前 / 后各 50 %」的半程状态下穿过 | 标签 x 60 → 76（虚线右边）；`--half` 半程横扫：改前 2 处「次声 × 低切虚线」压入 4.99，改后 0（半程总命中 12 → 10，余下 10 处是两态的同一标签在原地交叉淡入的固有重叠：LIMIT 的 LUFS 标签、BALANCE 的「+」） |
| F8 | info | ✔ 折行后各行 tspan 之间没有空白，复制 / 查找连成一个词 | 在空格处断开的行行尾带那个空格（中日韩之间断开的不加）：三个引擎里复制出来都是「two-stage average」「per-band gain after the split」「Low cut 25 Hz」，改前是「two-stageaverage」「gainafter」「Low cut25 Hz」（`selection-copy.txt`） |
| F9 | info | ✔ `useFitsOneRow` 的注释过时（`geometricPrecision` 之后字宽不随缩放比变）；`WrappedText` effect 依赖没有 style | 注释改准、依赖补 `fontSize` / `fontFamily` / `letterSpacing`；docstring 写明 `\n` 不当手动换行 |

## 验证

- **构建**：`pnpm run build`（`check-i18n` + **`check-svg-text`（新，37 项）** + `tsc --noEmit` + `vite build` + 站点地图 + 中文外壳）全过。`check-svg-text` 随构建跑：`wrapWords` 的折行规则 23 项（行宽、中日韩按字断、标点禁则、数字与单位粘住、行尾空格、拼回原文）+ 文案预算 14 项（四条页眉说明 × 中英文、三张图注 × 中英文的折行数）。
- **反向验证（`mutation_check_svg_text.sh`，输出 `mutation-check-svg-text.txt`）**：把 `wrapWords` / `figureBudget` 改坏七种（标点可落行首、左括号可落行尾、中日韩不能按字断、行尾不补空格、数字与单位不粘住、LIMIT 图注只留 1 行、页眉说明预算砍到 300），**七种都变红**，还原后通过。
- **三引擎横扫**（`sweep.mjs --engine`，Playwright 1.62.1；Chromium 全站 46 路由 × 13 屏宽 = 598 页 + 减少动效 26 页 + 半程 26 页；Firefox / WebKit 46 路由 × 屏宽 320 / 375 / 768 / 1440 = 184 页；汇总 `sweeps/sweep-summary.txt`，瘦身 JSON 在 `sweeps/*.slim.json`）：

| 运行 | 页 | SVG / `<text>` | 命中 | 贴近 |
|---|---|---|---|---|
| Chromium 改前（main `6ba86be`） | 598 / 598 | 468 / 3973 | 0 | 94 |
| **Chromium 改后** | 598 / 598 | 468 / 3973 | **0** | 90 |
| Chromium 改后 · 减少动效 | 26 / 26 | 312 / 1456 | 0 | 19 |
| Chromium 半程：改前 → 改后 | 26 / 26 | 234 / 1456 | 12 → 10（均为交叉淡入的固有重叠） | 41 → 39 |
| Firefox 153 改前 / **改后** | 184 / 184 | 144 / 1224 | 0 / **0** | 92 / 88 |
| WebKit 26.5 改前 | 184 / 184 | 144 / 1224 | **1（CLAY 帧标题 × 说明，17.31）** | 92 |
| WebKit 改后（只换折行、没换量宽） | 184 / 184 | 144 / 1224 | 1（同一处） | 91 |
| **WebKit 改后（换成 `getBBox`）** | 184 / 184 | 144 / 1224 | **0** | 89 |

  「贴近」里改前 → 改后有变化的组：AlgoTeaser COLOR 图注 4.18 → 无、Infrasonic 5.52 → 无（半程 5.5）、「Low cut」标签挪位（4.5 不变，文字多了行尾空格所以组名带空格）——四个引擎一致。
- **开发模式控制台**（`dev-console.txt`）：Chromium / Firefox / WebKit 各开 `/`、`/zh` × 1440 / 375 宽、滚动、缩小窗口触发 ResizeObserver：没有 React 警告、没有 `[WrappedText]` 超行告警；只有与本任务无关的 WebGL 驱动信息（Chromium 软件 GL 的 GPU stall、Firefox 的「WebGL context was lost」与 scroll-linked 提示）。
- 截图（`screenshots/`）：WebKit CLAY 帧页眉改前 / 改后（1440 与 375 宽）；Firefox、Chromium 改后；AlgoTeaser COLOR 小图英文改前 / 改后（3 倍）。

## 没做 / 限制

- Playwright 的 WebKit 不是 Safari：引擎相同，但真实 Safari（macOS / iOS）的字体渲染、缩放与最小字号策略仍可能有差别；iPhone Safari 没有设备可测。已知的差别（letter-spacing 的量法）已按「用三家都一致的量法」处理。
- 横扫只量布局几何（重叠、越界、压图形、贴近），不判美观；贴近 0 不代表所有标签都舒服。
- `Bento`、`docs/Figures` 等其他用固定坐标排版的 SVG：Chromium / Firefox / WebKit 下当前文案都有余量（横扫命中 0），没有改它们的排法。
- **部署 112**：Spark 没有 112 的 SSH 私钥；线上现在仍是手工补丁的 0.1.8.7 构建，没有 T-061 / T-062 / T-066 / T-069 的文字与图修复（见 TASKS 阻塞清单）。
