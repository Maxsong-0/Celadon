# V9 · 界面与 JUCE GUI 研究结论的反驳核验（对 R9-ui.md）

> T-032 阶段 B 核验代理 V9，2026-09-29 晚（PDT）。工作树 `.claude/worktrees/fable-deep-research-634a5e`（main `e6b445b`，v0.1.8.5）。
> 只读；本文件是唯一写入的文件。数值复刻在 107 `~/.cache/celadon-t032/V9/` 做（`~/.venvs/audio` 的 numpy），跑完已删。
> 核验标准：每条 confirmed 都自己在源码 / JUCE 9.0.2 源码里找到了同样的证据（文件:行号），并核对了档案与 T-032 计划。

## 0. 一句话结论

15 条结论里 **14 条缺陷本身成立、1 条（R9-14）成立但修法会改画面**；没有一条是误读代码或规格已定的取舍，也没有已修的。
但 **R9-01 的核心修法（缩小脏矩形绕开主旋钮）在 macOS 上无效**——CoreGraphics 路径下 JUCE 只拿到所有脏矩形的**包围盒**（`juce_NSViewComponentPeer_mac.mm:999-1003` 的注释写明 10.13 起 `getRectsBeingDrawn` 永远只有 1 个矩形；工程没开 `JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS`），两侧条带的包围盒就是整条带；而且 JUCE 只在光栅层裁剪，`paint()` 里的文字整形、渐变查表照做。因此「缩小脏区」类修法（R9-01 ①、R9-04 ③、R9-13）在主平台上不省，真正有效的是**把带内每个部件变成便宜的整数对齐贴图**（缓存图 + R9-07 的对齐技巧 + 30 fps）。
另一个共性问题：报告全部毫秒数来自 JUCE **软件渲染器**，而两个出货平台都不用它（macOS 固定 CoreGraphics，`juce_graphics.h:123`；Windows 默认 Direct2D，`juce_Windowing_windows.cpp:4685-4686`，只有 Wine 下才回退 GDI），V4 界面在 macOS 上至今没有主线程实测（v0.6.1 的表是改版前的）——所以 P0/P1 的排序建立在代理数字上，E 阶段必须由 Max 在 Mac 上量一次。

## 1. 核了什么

- 代码：`Source/ui/{ParticleField,GlazeKnob,Porcelain,SpectrumAnalyser,SpectrumView,TopBar,MiddleLayer,Controls,Popovers,GlazeStatusText,Localisation}.{h,cpp}` 相关段、`Source/PluginEditor.{h,cpp}`、`Source/AnalyzerTap.h`、`tests/SpectrumAnalyserTests.cpp`、`tools/ui-snapshot/SnapshotMain.cpp`、`CMakeLists.txt`。
- JUCE 9.0.2（`/Users/maxsong/All-Project/JUCE`）：`juce_Component.cpp`（重绘传播 1853-1911、子组件裁剪与跳过 1961-2011、透明度 2079-2084、默认无障碍处理器 3307-3310）、`juce_StandardCachedComponentImage.h`（45-97）、`juce_RenderingHelpers.h`（`renderImage` 整块拷贝判定 2555-2595、透明图层 2694-2710）、`juce_NSViewComponentPeer_mac.mm`（998-1029、1031-1075、1740-1747、205-235）、`juce_CoreGraphicsContext_mac.mm`（393-399、514-527、564-575、846-874）、`juce_Windowing_windows.cpp`（4685-4687、5502-5514）、`juce_FFT.cpp`（引擎只有 vDSP 有条件编译）、`juce_DropShadowEffect.cpp`（60-85）、`juce_Colour.cpp`（388-402）、`juce_ColourGradient.cpp`（241-269）、`juce_PerScreenDisplayLinks_mac.h`（CVDisplayLink 按显示器名义刷新率）。
- 档案：`01-框架/设计规格-界面-V4融合版.md`（第 1 节「界面刷新 30 Hz」、第 7 节主旋钮与粒子、第 8 节验收）、`设计规格-界面.md:116`（21 % 的出处）、`05-版本/v0.6.1.md`（高温测量表）、`04-协调/TASKS.md:150-151,162`、`03-记忆/ADR-013`（逐位契约只管音频路径与编译选项，界面不涉及）、`ADR-021`（V4 的 8 条决定里没有帧率、频谱聚合、无障碍）、T-032 计划（已知不重报项）。

## 2. 独立数值复刻（107，numpy；脚本见本会话 scratchpad `v9/bias.py`）

同一算法（Hann 8192 @ 48 kHz、480 点、低频功率插值 / 高频取最大、显示动态升 0.6 落 0.15）、150 帧、后 100 帧平均、相对 100 Hz：

| 信号 | 聚合 | 0.5k | 1k | 2k | 5k | 10k | 16k | 20k |
|---|---|---|---|---|---|---|---|---|
| 白噪 | 最大（现状） | +1.7 | +3.3 | +3.5 | +4.4 | +5.4 | +5.2 | +5.7 |
| 白噪 | 均值 | −0.4 | +0.4 | +0.2 | +0.4 | +0.2 | −0.0 | +0.1 |
| 粉噪 + 3 dB/oct 显示斜率 | 最大（现状） | +2.4 | +2.8 | +3.1 | +4.9 | +5.6 | +6.2 | +6.2 |
| 粉噪 + 3 dB/oct | 均值 | +0.7 | +0.4 | +0.3 | +0.3 | −0.1 | −0.1 | −0.2 |

−6 dBFS 正弦：取最大各频率都读 −6.6 dB；取均值 1 k / 5 k / 10 k / 16 k 读 −11.3 / −16.0 / −18.6 / −20.5 dB——与 R9 报告的正弦数字**逐位一致**，白噪 / 粉噪的偏差量级一致（我的实现没有报告 2b 里 100 Hz 参考点的 dB 域动态差异，所以低频差 1～2 dB）。R9-03 的机理成立。

## 3. 逐条判决

### R9-01 · 粒子带整条每帧重画拖累主旋钮与中层文字 —— **confirmed**，high · P0；**修法 ① 驳回，② 成立并要扩到主旋钮**

- 自己找到的证据：`ParticleField.cpp:225-229` 整个组件 `repaint()`；`PluginEditor.cpp:129` 带 = (0,300) 1200×260；`:131` GlazeTitle (360,305)、`:135` 主旋钮变换 (500,322) 200²、`:155` 监听提示 (24,452)、`:171` 左下说明 (24,494)、`layoutBottomRow` 底排 y 524～554、图例 y 504——全在 300～560 内。`juce_Component.cpp:1974-2011`：与裁剪区相交的子组件按 z 序全部调 `paint`（带变换的子组件 1988-1996 加变换后照画），只有 `getCurrentComponentBounds` 为空才跳过；`internalRepaint` 1869-1909 逐级上传到 peer。
- 报告细节两处不准：① 流线终点不是 `x ≤ 503.6`——`build()` 里 `ex = 600 − 96.43·cos(π − u·1.25)`，u = ±0.5 时 x ≈ 521.8（`ParticleField.cpp:103-105`），最后约 5 % 路径在旋钮矩形内，左侧包络最高约 25 %、右侧起点 (`sx ≥ 678`) 前 22 px 包络升到约 30 %，不是「≤ 4 px、≤ 10 %」；② 波纹环 r 106～236 穿过旋钮矩形 (500,322)-(700,522) 的四个角（角到圆心 141 px）——修法 ① 把 200² 整块排除后，四角里的弧段会**冻结成残影**。
- **修法 ① 在 macOS 上根本不生效**：`juce_NSViewComponentPeer_mac.mm:998-1027`，默认（`USE_COREGRAPHICS_RENDERING` = 1，`juce_graphics.h:123`；`JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS` 未定义，工程 CMake 也没开）只调一次 `renderRect (cg, r)`，注释明写「Apple 在 10.13 前后拿掉了 `getRectsBeingDrawn` 的能力，numRects 永远是 1，CoreGraphics 会把分散的矩形合并」；子组件是否跳过看 `g.getClipBounds()` = `CGContextGetClipBoundingBox`（`juce_CoreGraphicsContext_mac.mm:514-527`）= 包围盒。报告引用的 `getClipRects`（1876-1891）只在**非** CoreGraphics 的软件图路径（1043-1075）里用。左右两条带的包围盒就是整条带 → 主旋钮、标题、按钮每帧照画。Windows Direct2D 有 `deferredRepaints` 列表（`juce_Windowing_windows.cpp:5293-5316`），分块能生效，但那不是主平台。
- 即便裁剪区小，JUCE 只在光栅层裁：`paint()` 里的 `drawText` 仍走 HarfBuzz 整形、`setGradientFill` 仍建查表——**省不到 CPU，只省像素**。
- 修法 ② 成立：`StandardCachedComponentImage`（`juce_StandardCachedComponentImage.h:45-97`）按物理像素缓存、只在自身 `repaint()` 时重画；GlazeTitle / PillButton / CornerNote / ColourLegend / Segmented 的全部状态改动都走 `repaint()`（`MiddleLayer.cpp:46-53,123-148,222-231,247-251`；`Controls.cpp:731-740,925-943`；`ClickableComponent` 悬停 `Controls.h:431-438`），缓存失效正确。
- 调整后的修法：**② + 主旋钮自管整颗缓存图**（影子、轨道、瓷面、弧、指针、把手、数字合成一张，键 = 值量化 × comparing × 像素比；值变才重生成），每帧用 R9-07 的整数对齐 + `lowResamplingQuality` 贴一次；comparing 时缓存图本身按 0.55 合成（顺带解决 R9-14）；粒子上限改 30 fps（R9-06）。不要用 `setBufferedToImage` 缓存主旋钮：缓存图的贴图是 `drawImageTransformed (scale (comp/image))`（`juce_StandardCachedComponentImage.h:95-96`），用父上下文的默认插值、平移含小数 → 400² 的缓存每帧仍走重采样慢路径（按 B4 折算约 1.9 ms 软件口径），收益被吃掉。
- 验证口径要改：macOS 上主旋钮 `paint` 不可能「为 0」，应改看 `ComponentPaintDiagnostics::readFromCache` / 自管缓存的命中计数；`ui-snapshot --bench` 量的是软件渲染器，只能看相对值；绝对值要 Max 在 Mac 上用活动监视器量（V4 界面至今无此数据，`TASKS.md:162`）。
- 严重度 high 成立（「打开就高温」是 Max 报告的问题，`02-日志/2026-09-26.md:3`）；P0 保留，但前提是 E 阶段的 Mac 实测把它坐实。

### R9-02 · 瓷面每步全量重算逐像素底层 —— **confirmed**，medium · P1；修法 ① 成立、② 后半驳回、③ 需改

- 证据：`Porcelain.cpp:400-462`（逐像素 3 次噪声 + 开方 + lerp + 预乘量化）、`:477-488`（逐条 `strokePath`）、`:370-374`（`round (392·t^0.9)`）；`GlazeKnob.cpp:86-96`（裂纹数变或比例差 > 0.004 就重渲染）、`:129-156`（gap = max (60 ms, 3×上次耗时)）、`:186-188`（首帧同步）。中段每 0.26 % 就多一条裂纹，拖动时每个节流点都重渲染——成立。
- 实验 1 是纯 C++ 逐像素循环，**与渲染器无关**，数字可直接迁移到 Mac（M2 Pro 单核不慢于 EPYC 74F3）：2×≈ 6.8 ms、4×≈ 23 ms 每次，这是 R9 里最可信的一组数。
- 线性性成立：`g0/g1/g2 = mixHex (a, b, depth)` → `Colour::interpolatedWith`（`juce_Colour.cpp:388-402`，比例量化到 1/255 后 `PixelARGB::tween`）对 depth 线性；`lerp` 线性；`variation`、`coverage` 与 t 无关。但两次 8 位量化（端点图各自量化 + 混合再量化）差可达 2 LSB，验收阈值应写 **≤ 2 LSB** 而非 1。
- 修法 ② 前半（缓存 `createStrokedPath` 的轮廓）成立；**后半「t 增大只在上一张图上追加新裂纹」错误**：每条裂纹的 alpha 是 `crack.opacity·(0.8 + 0.2·t)`（`Porcelain.cpp:486`），t 变时所有已画裂纹都要变；而且积釉 / 釉光 / 反光 / 外环 / 帽子都画在裂纹**之后**（`:490-547`），往成图上追加会破坏叠放顺序。正确拆法：`base0 / base1`（按像素比缓存）+ 裂纹（每次用缓存轮廓 `fillPath`，便宜）+ **一张与 t 无关的静态覆盖层**（积釉、釉光、反光、两圈外环、帽子，按像素比缓存一次），三层合成后再 `desaturate`。
- 修法 ③ 后台线程：必须显式用 `juce::SoftwareImageType`——默认 `Image (ARGB, …)` 是 `NativeImageType`，Windows 上是 Direct2D 图（`PluginEditor.cpp:486-487` 注释已证实），不能在消息线程之外画；swap 后 `repaint()` 仍在消息线程。做了 ①② 之后 ③ 可以不做。

### R9-03 · 频谱仪高频取最大使宽带材料偏高 —— **confirmed**（独立复刻见第 2 节），medium · P1；修法技术上成立，但**是显示语义的改动，要 Max 点头**

- 证据：`SpectrumAnalyser.cpp:185-193`（插值）、`:194-202`（`jmax`）、`:221-226`（dB 域升落 0.6/0.15）、`:258-278`（平均线与峰值线都从 `display` 派生）；`SpectrumAnalyser.h:13` 口径「0 dBFS 正弦读 0 dB」。
- 不是规格已定的取舍：V4 规格第 4 节只写「白线 = 平均、深青细线 = 峰值保持」，ADR-021 没有聚合方式；作者注释（`:196`）解释为保尖峰，属实现取舍。
- 修法（填充 / 峰值线保持取最大，平均线改功率均值 + 功率域一阶平滑）技术上成立，现有测试不受影响（`tests/SpectrumAnalyserTests.cpp:122` 只要求平均线 < loudPeak − 30 dB；正弦时均值聚合在 1 kHz 也只低 5 dB）。但后果要写清给 Max：正弦 / 谐波密集的材料在高频，白线会比填充低 5～14 dB（1 kHz −4.7、10 kHz −12 dB），与现在「白线贴着填充」的观感不同；宽带音乐则白线变平直、更接近真实平衡。要么按报告做、帮助文案说明，要么保持现状——这是设计判断。
- 修法要补一点：`restartOutput` 之后平均线现在是从显示曲线拷贝起步（`:249`），改成独立功率均值后要从均值谱起步，别再拷 `display`。
- changes_output false 成立（纯显示）。

### R9-04 · 频谱仪每帧 516 次裁剪竖纹 + 悬停整面重画 —— **confirmed**，medium · P1；修法 ①②④ 成立，③ 在 macOS 上几乎不省

- 证据：`SpectrumView.cpp:624-635`（`reduceClipRegion (fill)` 后 `for (x = 0; x < 1032; x += 2) fillRect (x, 0, 0.8, h)` = 516 次 + 整面渐变）、`:613,638,640,648,656`（5 次 `buildLine`，output 两遍）、`:712-744`（网格文字、6 个 dB 标签、读数框每帧）、`:751-762`（`mouseMove` / `mouseExit` 整面 `repaint()`）；`setSpectrum` 的 0.02 dB 阈值（`:452-470`）意味着播放中每帧都重画。
- ③「悬停只 repaint 读数框与光标列」：macOS 上读数框 (48,12)-(298,38) 与光标列的包围盒几乎是整面；且 `paint()` 的全部绘制调用照发。要省就把读数框做成独立子组件 `setBufferedToImage`，光标点 / 竖线也做成子组件，或在 `paint` 里按 `g.clipRegionIntersects` 跳过整块。
- ①：软件渲染器 `renderImage (…, tiledFillClipRegion)`（`juce_RenderingHelpers.h:2579-2581`）与 CoreGraphics `CGContextDrawTiledImage`（`juce_CoreGraphicsContext_mac.mm:870-871`）都支持瓦片填充路径，成立；竖纹相位在非整数缩放下与现状差 ≤ 1 px，截图断言只在 --scale 1 / 2 下要求逐像素相同。
- 数字是软件口径（B2 7.1 / 18.7 ms），CoreGraphics / Direct2D 下更小，但 516 次带路径裁剪的填充在任何渲染器上都是浪费。

### R9-05 · 粒子每帧重画 400² 径向渐变柔光 —— **confirmed**（开销），medium · P1；**收益高估，修法要配对齐贴图**

- 证据：`ParticleField.cpp:270-278` 每次 `paint` 建 `ColourGradient` + `fillEllipse`；`juce_ColourGradient.cpp:241-269` 每次填充建查表（半径 200 × 像素比 × 3 项）。
- 报告写「缓存后 `drawImageTransformed (glowImage, …)`」：柔光图 400² 逻辑 = 800² @2×，按 B4 折算小数平移的重采样贴图约 7 ms、对齐贴图约 1.5 ms（软件口径）——**不对齐反而更慢**。必须用 R9-07 的整数对齐 + `lowResamplingQuality`，净省约一半而非 −2.5 ms；「配合 R9-01 后只在两侧条带相交部分贴图」在 macOS 上不成立（包围盒）。
- 建议合并进「带内贴图统一对齐」一起做（见第 4 节 M-3）。

### R9-06 · 粒子 45 fps 与规格 30 Hz 不一致 —— **confirmed**，low · P1（一行改动，随 R9-01 一起做）

- 证据：`ParticleField.cpp:38` `maxFrameRate = 45.0`，`:225` 节流；`ParticleField.h:14`；规格 `设计规格-界面-V4融合版.md:14`「界面刷新 30 Hz」；ADR-021 没有帧率决定；`advance()`（`:210-217`）截图时按 30 fps 步进——实现自己也把 30 当基准。
- 机理要修正：节流是「距上次 ≥ 1/45 s」，VBlank 按显示器名义刷新率来（`juce_PerScreenDisplayLinks_mac.h:100-103`）：60 Hz 显示器要等 2 个 VBlank（33.3 ms）→ 实际 **30 fps**；120 Hz ProMotion（Max 的 M2 Pro MacBook Pro）3 个 VBlank（25 ms）→ **40 fps**；90 Hz → 45 fps。所以「多付 1/3」只在高刷屏成立，Max 机器上是多付 1/4。改 30.0 后 60 / 120 Hz 都落到 30 fps。
- 单独看是 P2，但它是零风险一行，并直接缩 R9-01 的每帧开销，建议随 R9-01 做。

### R9-07 · 瓷面贴图非整数对齐走重采样慢路径 —— **confirmed**，medium · P1；修法成立，两处要补

- 证据：`GlazeKnob.cpp:164-170`（`scale (1/pixelsPerUnit).translated (22, 22)`）；`Porcelain.cpp:404-405`。JUCE 的整块拷贝判定在 `juce_RenderingHelpers.h:2555-2595`（报告引的 1882-1888 是 `clipToImageAlpha` 的同一条规则，位置引错）：缩放误差 < 0.002 **且**（低质量插值 或 平移小数部分 < 1/8 px）才 `renderImageUntransformed`。核算：1.786× 时 `size = round (321.5) = 321`，净比例 1.0015 **在 0.002 容差内**，触发慢路径的主要是平移——22 逻辑单位 × 1.786 = 39.3 px 的小数部分，以及非整数缩放下画布位置的小数；报告把原因主要归到「比例 ≠ 1」不够准，结论不变。
- 修法成立：`addTransform (scale (1/pixelScale))` 后净比例精确为 1（均匀缩放，软件 `(|a|+|d|)/2` 与 CoreGraphics `sqrt|det|` 都给 s）；`lowResamplingQuality` 让软件路径无视小数平移直接整块拷贝（≤ 0.5 px 吸附），CoreGraphics 下 `kCGInterpolationNone`（`juce_CoreGraphicsContext_mac.mm:625`）同样只做最近邻。要补：① `rendered.pixelScale ≠ 当前 pixelScale`（缩放窗口后 150 ms 内）时退回现在的变换贴图；② `dx` 无法包含上下文自身的小数平移（JUCE 不暴露当前变换），非整数缩放下靠低质量插值吸附，可接受。
- macOS 上的收益未量（报告承认）；在 R9-01 改成自管缓存后，这条技巧同时用于缓存图的每帧贴图，是那条修法的前提。

### R9-08 · 静态底图重生成约 200～350 ms 同步阻塞 —— **confirmed**，low · P2；修法成立

- 证据：`PluginEditor.cpp:488-571`（`:520-525` 5000 个点、`:528-542` 逐像素颗粒、`:547-553` 7 块面板 × `drawPanelShadow` 每块 **2 次** `DropShadow`（`:88-94`，半径 20 与 3）= 14 次，报告写 7 次）、`:403-404` 首帧同步、`:457-466` 缩放稳定后重生成。`juce_DropShadowEffect.cpp:60-85`：在逻辑分辨率画路径 + 盒模糊后 `drawImageAt` 放大——报告的「本就在逻辑分辨率」属实，阴影可按逻辑分辨率缓存一次、不随像素比重做。
- 颗粒 256² 瓦片 3.5 % 幅度平铺，周期不可见，成立。绝对时长是软件口径（Mac 更快），但打开编辑器与每次拖角缩放都同步付一次属实；低优先级合理。

### R9-09 · 静态底图封顶 2× 时每帧重采样 —— **confirmed**（机理），low · P2；**修法的「整块拷贝」部分驳回**

- 证据：`PluginEditor.cpp:25,394,406-408`。4× 上下文（200 % × Retina）净比例 2、120 % 缩放净比例 1.2 → `renderImage` 缩放容差 0.002 不满足 → `renderImageTransformed`（`juce_RenderingHelpers.h:2597-2612`）。
- `setImageResamplingQuality (lowResamplingQuality)` **不会**让比例 ≠ 1 的贴图走整块拷贝，只把逐像素采样从双线性降成最近邻（同一函数的分支顺序可证）；省的是每像素的常数，不是路径。
- 更有效的做法：把带内 (0,300,1200,260) 的背景单独按**上下文实际像素比**缓存一张（4× 时 4800×1040 RGB ≈ 15 MB，可接受），每帧对齐整块贴；其余区域保持 2× 封顶。或干脆允许底图 4×（RGB 三通道约 47 MB，报告说的 60 MB 按 ARGB 算）。

### R9-10 · 频谱仪每帧几千次 pow / log —— **confirmed**，low · P2；修法成立

- 证据：`SpectrumAnalyser.cpp:52-56`（`frequencyAt` 用 `pow`）、`:180-182`（每点两次 `pow (2, ±halfStep)`，常数）、`:205`（`log10`）；`SpectrumView.cpp:444-447`（`log2`）、`:594`（每条曲线每点 `frequencyAt` + `tiltAt`）、`:489-496`。2 路 × 480 × 3 + ≤ 5 × 480 × 2 ≈ 7.7 k 次，成立。
- 「现有用例逐位不变」可以做到：预算表用同样的 double 表达式按同样顺序算一次即可（确定性），采样率变时重算。界面不在 ADR-013 的逐位契约范围内，这只是回归保护。

### R9-11 · 96 k / 192 k 下 FFT 加长，Windows 回退 FFT 变贵 —— **confirmed**，low · P2；**修法 ① 驳回，② 成立**

- 证据：`SpectrumAnalyser.cpp:71,78-79`（13 / 14 / 15 阶）、`:163-170`（两路各一次全长加窗 + FFT）；`juce_FFT.cpp:439-443` 只有 `(JUCE_MAC || JUCE_IOS) && JUCE_USE_VDSP_FRAMEWORK` 一条加速引擎，Windows 无 MKL / FFTW 选项时确是 `FFTFallback`。
- 修法 ①「8 阶半带 FIR 抽取到 ≤ 50 kHz」不成立：8 阶半带的过渡带绕 fs/4 宽得多，96 k 时 fs/4 = 24 kHz，显示上限 20 kHz 会明显下垂、24～28 kHz 内容折叠到 20～24 kHz；要做抽取得用 ≥ 31～63 抽头（可直接复用 `dsp/HalfBandOversampler` 的系数），并让现有 96 k 用例（1 kHz ±1 dB、泄漏 < −60 dB）继续过。修法 ②（封顶 16384、接受 192 k 时 85 ms 窗）安全、简单，优先它。

### R9-12 · 编辑器定时器每帧无谓拼串与排版 —— **confirmed**，low · P2；修法基本成立，两处补

- 证据逐条核实：`PluginEditor.cpp:1151` 每帧 `layoutBottomRow()` → `:344-347` 四个 `getNaturalWidth()`（`MiddleLayer.cpp:150-154` 1 次、`Controls.cpp:774-783` 每项 1 次、`MiddleLayer.cpp:253-262` 3 次）≈ 9～10 次 `textWidth`；`:1002` 每帧 `advancedPanel.setStatus`（`Popovers.cpp:526-539`：4 次 `String (float, 1)`、2 次 `referenceStatusText()`、`targetCurve.setValues`）面板不可见时照做；`:1234-1236` → `TopBar.cpp:295-305` 每帧先拼串再比较；`:1183 / :1213` 每帧 `segmentsFor`（`GlazeStatusText.cpp:303-327` 拼 `StringArray`）——`StatusPill::setContent` 用 `Content::operator==` 挡住了重画（`TopBar.cpp:66-79`），所以头文件承诺的「不重画」落实了、「不重拼」没落实；`:1079-1081` → `Controls.cpp:313-330` 每帧 7 次 `autoAmountText()`；`SpectrumView.cpp:97-100` 每帧拼 5 段键；`Localisation.cpp:267-275` + `.h:28-31` 每次 UTF-8 解码。
- 量级：按报告实验 4（文本操作几到几十 µs），每帧约 0.1～0.5 ms，低优先级合理。
- 补：`layoutBottomRow` 触发条件还要含 `relearnButton.setDot`（`getNaturalWidth` 里 `dot ? 12 : 0`，`MiddleLayer.cpp:153`）与语言切换；`tr()` 返回静态表里的 `juce::String`（引用计数拷贝）安全，`LocalisationTests` 不受影响。

### R9-13 · 状态胶囊呼吸每 33 ms 重画整个胶囊 —— **confirmed**，low · P2；修法要改

- 证据：`TopBar.cpp:93-98`（timer → `repaint()`）、`:115-174`（每次 paint 1 次 `sepWidth` + 每段 1 次 `textWidth` + 1 次 `drawText`，3 段时约 8 次文本操作）；`setContent` 在聆听 / 校准中 `startTimerHz (30)`（`:73-74`）。
- 「只 `repaint (dotBounds.expanded (4))`」不省整形：`paint()` 照样跑完全部 `textWidth` / `drawText`（JUCE 只光栅裁剪），且圆点的 x 在 `paint` 里才算出（`:136-147`）。要省就把圆点做成独立子组件（它自己 `repaint`），或文字部分 `setBufferedToImage`，或 `paint` 里按 `g.clipRegionIntersects` 跳过文字段。macOS 上这条是单个小矩形，没有包围盒问题。

### R9-14 · 听原声时主旋钮每次 paint 开透明图层 —— **confirmed**（开销）但**修法驳回**，low · P2

- 证据：`GlazeKnob.cpp:194-195, 275-276`；软件渲染器 `beginTransparencyLayer` 按裁剪区分配一张 ARGB（`juce_RenderingHelpers.h:2694-2710`），CoreGraphics `CGContextBeginTransparencyLayer`（`juce_CoreGraphicsContext_mac.mm:564-569`）同样开层。
- 「各元素直接乘 α 即可等价」不成立：图层是**先合成再整体 55 %**，逐元素乘 α 会让被盖住的元素透出来——影子椭圆透过瓷面、灰轨道透过把手白点、帽子透过指针与数字；差异远不止 ±1 LSB。`setAlpha (0.55)` 也不行：未缓存时 JUCE 同样开层（`juce_Component.cpp:2079-2084`）。**等价且省层的做法**：整颗旋钮合成进缓存图，贴图时乘 α（`StandardCachedComponentImage` 正是 `setColour (black.withAlpha (owner.getAlpha()))` 后贴，`juce_StandardCachedComponentImage.h:94`；自管缓存同理）——这与 R9-01 调整后的「主旋钮自管缓存」是同一件事，合并做。

### R9-15 · 无无障碍值接口与键盘操作 —— **confirmed**（设计缺口），low · P2；方向成立，键盘部分有宿主风险，**待 Max 定要不要投**

- 证据：`grep -rn "AccessibilityHandler\|keyPressed\|setWantsKeyboardFocus\|setTitle\|setHelpText" Source/` 只命中 `Controls.cpp:301,715` 的 `setRepaintsOnMouseActivity`；`Controls.h:89-154` `ParameterControl` 无相关虚函数；JUCE 默认给 `AccessibilityRole::unspecified`（`juce_Component.cpp:3307-3310`）。V4 规格与 ADR-021 均未提无障碍，属未定的设计范围，不是违反规格。
- 修法风险：插件编辑器一旦 `setWantsKeyboardFocus (true)` 且点击就取焦点，宿主的走带快捷键（空格等）在插件有焦点时可能被吞——JUCE 插件界面惯例是不抢键盘焦点；实现时 `keyPressed` 对未处理的键必须返回 false，并在真实 DAW 里由 Max 实测（Logic / FL / Live 行为各异）。`AccessibilityHandler` + `ValueInterface`（用 `setValueAsCompleteGesture`，`Controls.h:129`）与 `setTitle / setHelpText` 风险低。工作量 L、用户未提需求 → 建议列「待 Max」，不进今晚实施集。

## 4. 漏掉的、更重要的同类问题（missed）

- **M-1 · macOS 脏矩形包围盒**：见 R9-01。所有「缩小脏区」修法（R9-01 ①、R9-04 ③、R9-13）在 macOS 上不省，且 JUCE 只在光栅层裁剪、`paint()` 的工作照做。这改变了实施集的形态：把带内部件变成对齐贴图（缓存图 + R9-07）才是根治，而不是切矩形。
- **M-2 · 全部毫秒数来自软件渲染器，两个出货平台都不用它**：macOS 固定 CoreGraphics（`juce_graphics.h:123`、peer 1744），Windows 默认 Direct2D（`juce_Windowing_windows.cpp:4685-4687`，只有 Wine 回退 GDI）。V4 界面在 macOS 上没有任何主线程实测（v0.6.1 表是改版前，`TASKS.md:162` 写明只有离屏截图）。P0/P1 排序是代理值；E 阶段要 Max 在 Mac 上量三种情况（不开窗 / 开窗静止 / 开窗播放），否则不能声称「高温」已改善。相对结论（重画范围、可缓存量）不受影响；R9-02 的逐像素循环是纯 CPU，数字可直接迁移。
- **M-3 · 流线缓存图 `lineImage` 每帧整条带贴图，比例封顶 2.5 且平移含小数**（报告未列）：`ParticleField.cpp:267` 把像素比钳到 2.5，`:286` `drawImageTransformed (lineImage, scale (1/lineImageScale).translated (0, 300))`。4×（200 % × Retina）时净比例 1.6 → 一张 3000×650 的图每帧逐像素重采样（按 B4 面积折算软件口径约 20 ms，是带内最贵的一项）；2× 且缩放 100 % 时平移 (0, 600) 整数、走整块拷贝，但任何非整数缩放（115 % 等）都掉进慢路径。修法与 R9-05/07 同一套：按上下文实际像素比生成（去掉 2.5 封顶或改成两张只含流线的侧图 500×260，中间 192 px 本来是空的）、整数对齐 + 低质量插值贴图。
- **M-4 · `setBufferedToImage` 对主旋钮无效**：见 R9-01 末段——缓存图的贴图本身走小数平移的重采样路径，主旋钮要自管缓存 + 对齐贴图。文字小部件用 `setBufferedToImage` 没问题（图小）。
- **M-5 · R9-02 ③ 与 R9-11 ① 的实现陷阱**：后台渲染必须 `SoftwareImageType`（Direct2D 图不能跨线程）；短半带抽取会让 20 kHz 端下垂并折叠——两处已写在各自判决里，列在这里是提醒综合时别照抄。
- **M-6 · 多个不同步的重画源**（低）：编辑器 30 Hz、Canvas 10 Hz、StatusPill 30 Hz、GlazeKnob 60 Hz 定时器与 ParticleField 的 VBlank 各自 `repaint`，在 120 Hz 显示器上会落在不同的 VBlank 里各触发一次画布重画（每次都重贴带内背景）。做完 M-3 / R9-01 后每次重画便宜了，这条自然缓解；若要再省，可让 ParticleField 的重画由编辑器 30 Hz 定时器驱动而不是 VBlank。

## 5. 判决汇总

| 编号 | 判决 | 严重度 | 优先级 | 修法 | 备注 |
|---|---|---|---|---|---|
| R9-01 | confirmed | high | P0 | ① 驳回（macOS 包围盒 + 四角残影），② 成立并扩到主旋钮自管缓存 | 需 Mac 实测坐实 |
| R9-02 | confirmed | medium | P1 | ① 成立；② 缓存轮廓成立、增量追加驳回；③ 需 SoftwareImageType | 数字可迁移 Mac |
| R9-03 | confirmed（复刻） | medium | P1 | 技术成立；显示语义改动待 Max | restart 起步要改 |
| R9-04 | confirmed | medium | P1 | ①②④ 成立；③ macOS 不省 | — |
| R9-05 | confirmed | medium | P1 | 必须对齐贴图，收益约减半 | 并入 M-3 |
| R9-06 | confirmed | low | P1 | 成立 | 60 Hz 屏已是 30 fps，120 Hz 屏 40 fps |
| R9-07 | confirmed | medium | P1 | 成立，补像素比不一致时的回退 | 行号引错，规则同 |
| R9-08 | confirmed | low | P2 | 成立 | 14 次 DropShadow 非 7 |
| R9-09 | confirmed | low | P2 | 「整块拷贝」驳回；改带内背景按实际像素比单独缓存 | — |
| R9-10 | confirmed | low | P2 | 成立 | — |
| R9-11 | confirmed | low | P2 | ① 驳回（短半带下垂 / 折叠），② 成立 | — |
| R9-12 | confirmed | low | P2 | 成立，补 setDot 与语言切换触发 | — |
| R9-13 | confirmed | low | P2 | 小矩形不省整形；改子组件 / 缓存 / clip 判断 | — |
| R9-14 | confirmed（开销） | low | P2 | 逐元素乘 α 驳回；缓存图贴图乘 α | 并入 R9-01 |
| R9-15 | confirmed（缺口） | low | P2 | 方向成立；键盘焦点有宿主风险；待 Max | 不进今晚 |

建议今晚的界面实施集（只改 `Source/ui/**` 与 `PluginEditor.cpp`，音频输出逐位不变）：**R9-01②+主旋钮自管缓存（含 R9-07、R9-14）→ R9-06 → M-3 + R9-05（带内贴图统一对齐）→ R9-02①②（分层缓存）→ R9-04①②④ → R9-10 → R9-12**；R9-03 与 R9-15 列「待 Max」；R9-08 / 09 / 11② / 13 视时间。验收：`ui-snapshot` 截图逐像素（允许对齐吸附 ≤ 0.5 px 与 ≤ 2 LSB）、`SpectrumAnalyserTests` / `LocalisationTests` / `GlazeStatusTextTests` 不变、107 全套；**macOS 主线程占比由 Max 用活动监视器量**。
