# T-032 实现任务书 · A9-ui-band · 界面：粒子带 / 主旋钮 / 瓷面 / 编辑器定时器 / 状态胶囊

> 生成于 2026-09-29 23:0x，来源：研究代理 R*.md 与核验代理 V*.md（同目录）。逐条以「核验调整后的修法」为准。

## 你负责的文件（只许改这些）

Source/ui/ParticleField.{h,cpp}、Source/ui/GlazeKnob.{h,cpp}、Source/ui/Porcelain.{h,cpp}、Source/ui/TopBar.{h,cpp}、Source/ui/MiddleLayer.{h,cpp}、Source/ui/Controls.{h,cpp}、Source/ui/Popovers.{h,cpp}、Source/PluginEditor.{h,cpp}、tools/ui-snapshot/SnapshotMain.cpp（可加计时 / 缓存命中打印）。**不新建源文件**（避免改 .jucer / CMake）。SpectrumView / SpectrumAnalyser 归 A10。

## 主进程补充要求

- 核验 M-1 / M-2 / M-4：macOS 默认 CoreGraphics 只拿脏矩形包围盒，「缩小 repaint 矩形」类修法在主平台不省——一律按「缓存图 + 整数对齐贴图」做：R9-01 只做 ②（GlazeTitle / Segmented / PillButton / CornerNote / ColourLegend / monitoringButton setBufferedToImage）+ 主旋钮自管整颗缓存图（键 = 值量化 × comparing × 像素比，用 R9-07 的 addTransform(scale(1/pixelScale)) + lowResamplingQuality + drawImageAt 整数对齐；comparing 时贴图乘 0.55 顺带解决 R9-14）；R9-05 柔光图缓存同一套对齐；核验 M-3：lineImage 去掉 2.5 像素比封顶、按上下文实际像素比生成、整数对齐贴图；R9-06 maxFrameRate = 30；R9-13 圆点做成独立子组件或文字 setBufferedToImage。
- R9-02 做 ①（base0/base1 按像素比缓存、每次 depth 混合，验收 ≤ 2 LSB）+ ②（只缓存 createStrokedPath 轮廓、渲染时 fillPath；与 t 无关的积釉 / 釉光 / 反光 / 外环 / 帽子缓存成静态覆盖层）；不做后台线程。
- R9-08：阴影按逻辑分辨率只生成一次缓存（14 张小图或 7 张合成）；颗粒改 256² 随机瓦片平铺；点阵保留；renderStaticLayer 打 log 计时。R9-12 按核验补两点（relearnButton.setDot 与语言切换也触发 layoutBottomRow；tr() 返回静态表里预解码的 juce::String）。
- 验证：107 已装 freetype2 / fontconfig / X11 开发包与 Noto CJK 字体。在你的工作树用 scripts/test-linux.sh 的自定义命令形式编 celadon_ui_snapshot（cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_ui_snapshot -j 3），改前先出一套基线 PNG + `--only check` 结果（xvfb-run -a "$BUILD/celadon_ui_snapshot" --out <目录>），改后再出一套：58 项交互检查必须全 PASS；截图与基线逐像素比对，按核验允许的差异（R9-02 ≤ 2 LSB、对齐吸附 ≤ 0.5 px、R9-08 颗粒瓦片）逐张解释，其余必须相同；在 SnapshotMain 里加每帧 paint 计时打印，报告改前改后每帧毫秒（软件渲染口径，注明 Mac 需 Max 实测）。注意 rsync 排除了 JuceLibraryCode/，若 ui_snapshot 需要 JucePluginDefines.h，看 CMakeLists.txt:270-295 它怎么生成到 ui-snapshot-include，必要时把该头文件内容一起同步（用自定义命令前先 scp）。

## 逐条结论

### R9-01 · 粒子带整条 1200×260 每帧 repaint()，把主旋钮（非整数比例 720² 贴图）与中层全部文字部件拖进每一帧
- 严重度 / 优先级：high / P0 → 核验 high / P0；工作量 M；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/ParticleField.cpp:225`
- 证据：ParticleField.cpp:225-229 `if (animating && …) { lastRepaint = timestampSeconds; repaint(); }` 整个组件标脏（带 = 画布 y 300 起高 260，ParticleField.h:34-35）；PluginEditor.cpp:128-135 主旋钮 (500,322) 200×200 与带重叠，:130-131/:155/:171/:342-367 GlazeTitle、监听提示、左下说明、底排、图例都在带内；GlazeKnob.cpp:159-171 每次 paint 用 drawImageTransformed(scale 1/pixelsPerUnit) 贴瓷面。JUCE 对脏矩形内所有相交组件按 z 序重画（juce_Component.cpp:1975-2010），只有裁剪区为空才跳过 paint（:1961-1963）。实测（JUCE 9.0.2 软件渲染器）：瓷面贴图 1.22 ms @1.79× / 4.71 ms @3.57×，轨道弧 0.12～0.22 ms，文本每次 6～43 µs × 约 25 次。
- 机理：粒子只在两侧条带（流线终点 x ≤ 503.6 / ≥ 696.4）和 r 106～236 波纹环上动，但每帧把整条带标脏，主旋钮、GlazeTitle、风格/AB Segmented、三个 PillButton、CornerNote、ColourLegend 和整条带的静态底图每帧（≤45 次/s）都重画，估算 2～6 ms/帧 ≈ 单核 9～27 %（软件口径，macOS 约 1/3～1/2），GLAZE 不动时全是白做。
- 研究修法：① ParticleField::onVBlank：亮度/GLAZE 没在变时改为 repaint(0,0,500,bandHeight)、repaint(700,0,500,bandHeight)、repaint(500,0,200,22)、repaint(500,222,200,38)（波纹包围盒去掉主旋钮矩形 (500,322)-(700,522) 后的四块，组件坐标 y 已减 top）；brightness 还在追目标或 setGlaze 改值时才整条带 repaint()。② GlazeTitle、Segmented（风格/A-B）、PillButton、CornerNote、ColourLegend、monitoringButton 各 setBufferedToImage(true)（缓存按物理像素生成，juce_StandardCachedComponentImage.h:45-50）。
- 核验理由：ParticleField.cpp:225-229 每帧整组件 repaint()；PluginEditor.cpp:129/131/135/155/171 与 layoutBottomRow 证实主旋钮、标题、三个按钮、说明、图例都在 300～560 的带内；juce_Component.cpp:1974-2011 与裁剪区相交的子组件按 z 序全部调 paint（带变换的 1988-1996 照画）。但修法 ① 在 macOS 上无效：默认 CoreGraphics 路径（juce_graphics.h:123；MULTIPLE_PAINT_CALLS 未定义）只调一次 renderRect，juce_NSViewComponentPeer_mac.mm:999-1003 注释明写 10.13 起 getRectsBeingDrawn 永远 1 个矩形、CoreGraphics 合并分散矩形；子组件跳过看 CGContextGetClipBoundingBox（juce_CoreGraphicsContext_mac.mm:514-527）= 包围盒，两侧条带的包围盒就是整条带。报告引的 getClipRects(1876-1891) 只在非 CG 软件图路径用。另外波纹环 r106～141 穿过旋钮矩形四角，排除 200² 会留冻结弧段；流线终点实际到 x≈522（build() u=±0.5）。JUCE 只光栅裁剪，paint() 里文字整形照做。
- **核验调整后的修法（以此为准）**：保留 ②：GlazeTitle / Segmented / PillButton / CornerNote / ColourLegend / monitoringButton setBufferedToImage(true)（它们的状态改动全走 repaint，缓存失效正确）。主旋钮不用 setBufferedToImage（缓存贴图走 drawImageTransformed 小数平移慢路径，juce_StandardCachedComponentImage.h:95-96），改为 GlazeKnob 自管整颗缓存图（影子/轨道/瓷面/弧/指针/把手/数字，键 = 值量化×comparing×像素比，值变才重生成），每帧用 R9-07 的 addTransform(scale(1/pixelScale)) + lowResamplingQuality + drawImageAt 整数对齐贴一次；comparing 时缓存图贴图乘 0.55（顺带解决 R9-14）；粒子上限改 30 fps（R9-06）。验证口径改看缓存命中 / ComponentPaintDiagnostics::readFromCache，绝对占比由 Max 在 Mac 用活动监视器量。
- 风险：P0 的绝对收益建立在软件渲染器代理值上，V4 界面在 macOS 无实测；自管缓存要保证 parameterValueChanged / setComparing / 淡入淡出定时器都触发重生成，否则显示滞后；非整数缩放下对齐吸附 ≤0.5 px。
- 验证：① JUCE_ENABLE_REPAINT_DEBUGGING=1 的 Debug 版看闪烁区域；② tools/ui-snapshot 加 --bench：给每个子组件挂 ComponentListener::componentPainted（JUCE 9 ComponentPaintDiagnostics::totalPaintDuration，juce_Component.cpp:1980-1989），喂音乐 300 帧打印每组件累计 paint 时长，改后主旋钮在 GLAZE 不动时应为 0；③ Mac Activity Monitor 看消息线程占比（Max 手动）。

### R9-07 · 瓷面贴图落在非整数设备像素与 ≠1 的比例上，每次 paint 走重采样慢路径
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/GlazeKnob.cpp:166`
- 证据：GlazeKnob.cpp:164-170 `transform = scale(1/pixelsPerUnit).translated(centreXY − imageHalf, …)` 后 drawImageTransformed；Porcelain.cpp:404-405 size = round(180·scale)，pixelsPerUnit = size/180 → 上下文缩放 × 1/pixelsPerUnit = pixelScale·180/round(180·pixelScale) ≠ 1（例如 1.786 → 1.00125），平移 22 逻辑像素落在小数设备坐标。JUCE 只对纯平移且偏移在 ±1/8 px 内的贴图走整块拷贝（juce_RenderingHelpers.h:1882-1888）。实验 3 B4：drawImageTransformed 1.218 vs drawImageAt 0.248 ms（1.79×）、4.705 vs 0.978 ms（3.57×）。
- 机理：净比例不等于 1 和小数平移都触发逐像素双线性重采样，主旋钮每次重画多付 1～3.7 ms；macOS CGContextDrawImage 在非整数变换时同样插值。
- 研究修法：drawGlaze 里 ScopedSaveState；g.addTransform(AffineTransform::scale(1.0f/pixelScale)) 把坐标系换回设备像素；g.setImageResamplingQuality(Graphics::lowResamplingQuality)（强制整块拷贝路径，≤0.5 px 位置误差在柔边瓷面上不可见）；drawImageAt(image, roundToInt(dx), roundToInt(dy))，dx = (centreXY − imageHalf)·pixelScale；淡入淡出两张叠画同样处理。
- 核验理由：GlazeKnob.cpp:164-170 scale(1/pixelsPerUnit).translated(22,22) 后 drawImageTransformed；Porcelain.cpp:404-405。整块拷贝判定在 juce_RenderingHelpers.h:2555-2595 renderImage（报告引的 1882-1888 是 clipToImageAlpha 的同一条规则，位置引错）：缩放误差 <0.002 且（低质量插值 或 平移小数 <1/8 px）才 renderImageUntransformed。核算 1.786× 时 size=321、净比例 1.0015 在容差内，触发慢路径的主要是 22 逻辑单位×像素比的小数平移与非整数缩放的画布位置，报告把主因归到「比例≠1」不准但结论不变。CoreGraphics 下 lowResamplingQuality → kCGInterpolationNone（juce_CoreGraphicsContext_mac.mm:625）同样只做最近邻。
- **核验调整后的修法（以此为准）**：按报告做（ScopedSaveState；addTransform(scale(1/pixelScale)) 换回设备像素；setImageResamplingQuality(lowResamplingQuality)；drawImageAt(image, round(dx), round(dy))，淡入淡出两张同法），补：rendered.pixelScale 与当前 pixelScale 不一致（缩放后 150 ms 内）时退回现在的变换贴图；dx 无法含上下文自身的小数平移，靠低质量插值吸附（≤0.5 px）。这套对齐代码同时是 R9-01 主旋钮缓存、R9-05、M-3 的贴图基础。
- 风险：macOS 上收益未实测；对齐吸附 ≤0.5 px 在柔边瓷面不可见但截图断言要放宽。
- 验证：截图对比（允许 ≤0.5 px 平移）；ui-snapshot --bench 中 GlazeKnob 每次 paint −1～−3.7 ms。

### R9-05 · 粒子每帧重画 400×400 径向渐变柔光（软件渲染 3～12 ms/帧）
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/ParticleField.cpp:272`
- 证据：ParticleField.cpp:270-278 每次 paint 建 ColourGradient glow 并 fillEllipse 400×400；实验 3 B3：柔光单项 3.01 ms @2× / 12.02 ms @4×，而 170 实心 + 30 渐变粒子 + 3 圈只要 0.79 / 1.85 ms。柔光只随 glaze（不透明度 0.1+0.4·glaze）变，形状固定。
- 机理：径向渐变按像素求值且每次 setGradientFill 重建查找表（juce_ColourGradient.cpp:241-267），在最多 45 Hz 的粒子帧里成了最贵的单项。
- 研究修法：与 lineImage 同法缓存一张全不透明度柔光图（renderLines 时一并生成或单独 glowImage，按像素比 keyed），每帧 setOpacity(0.1f + 0.4f·glaze) 后 drawImageTransformed(glowImage, …)；配合 R9-01 后只在两侧条带相交部分贴图。
- 核验理由：ParticleField.cpp:270-278 每次 paint 新建 ColourGradient 并 fillEllipse 400²；juce_ColourGradient.cpp:241-269 每次填充建查表（半径 200×像素比×3 项）。开销属实，但修法写的 drawImageTransformed(glowImage,…) 是小数平移的重采样贴图：800²@2× 按 B4 面积折算约 7 ms（软件口径），比渐变本身还慢；只有整数对齐贴图（约 1.5 ms）才有收益，净省约一半而非 −2.5 ms。「配合 R9-01 只贴两侧」在 macOS 上因包围盒不成立。
- **核验调整后的修法（以此为准）**：缓存一张全不透明度柔光图（按上下文实际像素比 keyed），每帧 setOpacity(0.1+0.4·glaze) 后用 R9-07 的 addTransform(scale(1/pixelScale)) + lowResamplingQuality + drawImageAt 整数对齐贴图；与 lineImage（M-3）用同一套对齐代码。
- 风险：未对齐时反而更慢；柔光图 800²～1600² ARGB 内存 2.5～10 MB 可接受。
- 验证：截图对比（渐变查找表量化 ±1 LSB）；ui-snapshot --bench 中 ParticleField 的 paint 时长每帧 −2.5 ms @2×（软件口径）。

### R9-06 · 粒子动画上限 45 帧/s，与规格「界面刷新 30 Hz」不一致，粒子带全部开销多付三分之一
- 严重度 / 优先级：low / P1 → 核验 low / P1；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/ParticleField.cpp:38`
- 证据：ParticleField.cpp:38 `constexpr double maxFrameRate = 45.0;`，:225 按它节流；ParticleField.h:14 写「最多约 45 帧/s」；规格《设计规格-界面-V4融合版》第 1 节写「界面刷新 30 Hz」；其它一切（表、频谱、读数）都是 30 Hz（PluginEditor.cpp:23,659）。
- 机理：1～3 px 的慢速粒子 30 与 45 帧看不出差别，但 R9-01/05 的每帧开销按 45/30 多付 1/3。
- 研究修法：maxFrameRate = 30.0（VBlank 60 Hz 时隔帧重画）；若 Max 要保留 45，则在规格第 1 节补一句。
- 核验理由：ParticleField.cpp:38 maxFrameRate = 45.0、:225 节流；ParticleField.h:14；规格 设计规格-界面-V4融合版.md:14「界面刷新 30 Hz」；ADR-021 无帧率决定；advance() :210-217 截图时自己按 30 fps 步进。机理要修正：节流按「距上次 ≥1/45 s」而 VBlank 按显示器名义刷新率（juce_PerScreenDisplayLinks_mac.h:100-103）：60 Hz 屏要等 2 个 VBlank → 实际已是 30 fps；120 Hz ProMotion（Max 的 M2 Pro）3 个 → 40 fps；90 Hz → 45。多付的是 1/4（120 Hz）而非 1/3。
- **核验调整后的修法（以此为准）**：maxFrameRate = 30.0（60/120 Hz 屏都落到 30 fps），ParticleField.h 注释同步改；不必改规格。单独看是 P2，但零风险一行且直接缩 R9-01 的每帧开销，随 R9-01 一起做。
- 风险：无；高刷屏上粒子略不如 40 fps 顺滑，1～3 px 粒子肉眼难辨。
- 验证：ui-snapshot --bench 帧计数；肉眼。

### R9-02 · 瓷面每 1 % GLAZE 步进都全量重算逐像素底层（4× 像素比 23 ms/次），拖动期间按最大节流反复生成
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 M；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/Porcelain.cpp:424`
- 证据：Porcelain.cpp:424-462 每像素 3 次梯度噪声 + 2 次开方 + 3 次 lerp + 预乘量化；:477-488 最多 392 条裂纹逐条 strokePath；GlazeKnob.cpp:86-96 needsRender 比例差 > 0.004 或裂纹数变即重渲染，:129-156 节流 gap = max(60 ms, 3×上次耗时)，:186-188 首帧同步渲染；Porcelain.cpp:370-374 visibleCrackCount = round(392·t^0.9) 中段每 1 % 变 3～4 条。实验 1（同一算法复刻，g++ -O2）：全量底层 2.03 / 6.75 / 9.93 / 13.6 / 23.0 ms（1×/2×/2.5×/3×/4×），两张基底图混合 0.17～2.65 ms。
- 机理：底层颜色对 depth = t^0.75 是线性的（g0/g1/g2 各是两色按 depth 线性混合，再按半径 gt 线性插值，噪声与 t 无关，Porcelain.cpp:412-452），所以 base(t) = (1−depth)·base0 + depth·base1，两张基底图按像素比只需生成一次；现状每步都重算。拖一次旋钮在 2× 约 16 次/s × (6.8+2～3) ms ≈ 15 % 单核，4× 约 11 次/s × 30 ms ≈ 33 %。
- 研究修法：① renderGlaze 拆出 renderBase(pixelScale, depth∈{0,1}) 缓存两张 ARGB（按像素比 keyed），每次渲染 = drawImageAt(base0) + setOpacity(depth) drawImageAt(base1)（或定点逐像素混合）；② crackNetwork() 生成时用 PathStrokeType(width, curved, rounded).createStrokedPath 把每条裂纹描边轮廓缓存成 Path（逻辑坐标、与像素比无关），渲染时 fillPath(strokedPath, toImage) 代替 strokePath；t 增大只在上一张图上追加新增裂纹（visible 单调），减小才从基底重画；③ 可选（L）：整张瓷面渲染放 ThreadPool 后台线程，完成后消息线程 swap。
- 核验理由：Porcelain.cpp:400-462 逐像素 3 次噪声+开方+lerp+预乘量化，:477-488 逐条 strokePath，:370-374 round(392·t^0.9) 中段每 0.26% 加一条裂纹；GlazeKnob.cpp:86-96 裂纹数或比例差>0.004 即重渲染，:129-156 节流 max(60ms,3×上次)，:186-188 首帧同步。实验 1 是纯 C++ 循环、与渲染器无关，数字可迁移到 Mac。线性性成立：g0/g1/g2 经 Colour::interpolatedWith（juce_Colour.cpp:388-402，比例量化 1/255）对 depth 线性，variation/coverage 与 t 无关。但 ② 后半「t 增大只追加新裂纹」错误：每条裂纹 alpha = opacity·(0.8+0.2t)（:486）随 t 变，且积釉/釉光/反光/外环/帽子画在裂纹之后（:490-547），往成图追加破坏叠放顺序。③ 默认 Image 是 NativeImageType，Windows 上为 Direct2D 图（PluginEditor.cpp:486-487 注释），不能跨线程画。
- **核验调整后的修法（以此为准）**：① 按像素比缓存 base0/base1，每次 depth 混合（验收阈值 ≤2 LSB，因双重 8 位量化）；② 只取「缓存 createStrokedPath 轮廓、渲染时 fillPath」，去掉增量追加；另把与 t 无关的积釉、釉光、反光、两圈外环、帽子缓存成一张静态覆盖层，每次 = base 混合 + 裂纹（缓存轮廓）+ 覆盖层，再 desaturate；③ 若做后台渲染必须显式 juce::SoftwareImageType，swap 与 repaint 回消息线程；做了 ①② 后 ③ 可不做。
- 风险：混合后与现图有 ≤2 LSB 差，ui-snapshot 逐像素断言要放宽；覆盖层与裂纹叠放顺序要与现在逐层核对（裂纹在积釉之前）。
- 验证：① 离线对比新旧 renderGlaze 图像逐像素差 ≤ 1 LSB（ui-snapshot 加检查）；② GlazeKnob::lastRenderCostMs 打 log，4× 目标 ≤ 5 ms、2× ≤ 1.5 ms；③ 拖动 GLAZE 时 Activity Monitor 消息线程占比。

### R9-08 · 静态底图每次重生成约 200～350 ms（7 个 DropShadow、逐像素颗粒、5000 个小圆）同步阻塞消息线程
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 M；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/PluginEditor.cpp:548`
- 证据：PluginEditor.cpp:488-571（:520-525 5000 个 1.6 px fillEllipse；:528-542 逐像素颗粒；:547-553 7 个 DropShadow::drawForPath），:403-404 首帧同步生成，:457-466 缩放稳定 150 ms 后重生成。实验 3 B6：5000 圆 19.1 ms、单个频谱面板阴影 38.3 ms（7 个 ≈ 100+ ms）；实验 1 颗粒循环 2× 43.6 ms。
- 机理：打开编辑器与每次拖角缩放停下都同步付一次，期间界面与音频取样口都不动。
- 研究修法：阴影按逻辑分辨率只生成一次、缓存成 7 张小图（DropShadow 本就在逻辑分辨率渲染再放大）；颗粒用一张 256×256 随机瓦片 setTiledImageFill 低透明度铺满（视觉等价）；点阵可保留（瓦片填充只省 5 ms）。
- 核验理由：PluginEditor.cpp:488-571：:520-525 5000 个 1.6 px 圆、:528-542 逐像素颗粒、:547-553 七块面板各调 drawPanelShadow，而 :88-94 每次是两个 DropShadow（半径 20 与 3）= 14 次（报告写 7）；:403-404 首帧同步、:457-466 缩放稳定 150 ms 后重生成。juce_DropShadowEffect.cpp:60-85 在逻辑分辨率画路径+盒模糊再 drawImageAt 放大，报告「本就在逻辑分辨率」属实，阴影可缓存一次不随像素比重做。绝对时长是软件口径（Mac 更快），但同步阻塞属实。
- **核验调整后的修法（以此为准）**：阴影按逻辑分辨率只生成一次缓存成 14 张小图（或 7 张合成图）；颗粒改 256² 随机瓦片 setTiledImageFill 低透明度平铺；点阵保留。renderStaticLayer 打 log 计时。
- 风险：颗粒瓦片周期 256 px 在 3.5% 幅度下不可见；截图逐像素会变，需重新落基准。
- 验证：Time::getMillisecondCounterHiRes 包住 renderStaticLayer 打 log（目标 2× < 60 ms）；截图对比（颗粒瓦片周期 256 px 不可见）。

### R9-12 · 编辑器定时器每帧做无谓的字符串拼接与文本排版（layoutBottomRow 每帧 10 次 getStringWidth 等）
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/PluginEditor.cpp:1151`
- 证据：PluginEditor.cpp:1151 每帧 layoutBottomRow() → :344-347 四个 getNaturalWidth() ≈ 10 次 getStringWidth（实验 4：6～25 µs/次，中文更贵）；:1002 每帧 advancedPanel.setStatus（Popovers.cpp:526-539：4 次 String(float,1)、2 次 tr、27 次取整）面板不可见时也做；:1234-1236 每帧 TopBar::setLatency 拼字符串（TopBar.cpp:297）；:1183/:1213 每帧 segmentsFor 拼 StringArray（GlazeStatusText.cpp:303-327，头文件承诺的「View 没变就不重拼」没落实）；:1079-1081 → Controls.cpp:313-330 每帧 7 个旋钮各拼 autoAmountText；SpectrumView.cpp:92-107 每帧拼 5 段 String(float,1) 当比较键；Localisation.cpp:267-286 tr() 每次 UTF-8 解码 + 分配。
- 机理：30 Hz × 约 20～30 次分配与十几次 HarfBuzz 整形，全部结果与上一帧相同；约 0.3～0.6 ms/帧、600～900 次/s 堆分配。
- 研究修法：layoutBottomRow 只在 setText/setLanguage 改变文字时调（PillButton::setText 返回是否变化或在 timerCallback 比较文本）；setStatus 在 advancedPanel 不可见时只存状态、显示时刷新；setLatency 先比 double 再拼；segmentsFor 以 view 相等缓存；LoudnessHud::setValues 比较量化到 0.1 的整数；tr() 返回静态表里预先解码的 juce::String。
- 核验理由：PluginEditor.cpp:1151 每帧 layoutBottomRow → :344-347 四个 getNaturalWidth（MiddleLayer.cpp:150-154 一次、Controls.cpp:774-783 每项一次、MiddleLayer.cpp:253-262 三次）≈9～10 次 textWidth；:1002 每帧 advancedPanel.setStatus（Popovers.cpp:526-539：4 次 String(float,1)、2 次 referenceStatusText、targetCurve.setValues）面板不可见照做；:1234-1236 → TopBar.cpp:295-305 先拼串再比；:1183/:1213 每帧 segmentsFor（GlazeStatusText.cpp:303-327），StatusPill::setContent 的 operator== 挡住了重画但拼串照做；:1079-1081 → Controls.cpp:313-330 每帧 7 次 autoAmountText；SpectrumView.cpp:97-100 每帧拼 5 段键；Localisation.cpp:267-275 + .h:28-31 每次 UTF-8 解码。量级按实验 4 每帧约 0.1～0.5 ms。
- **核验调整后的修法（以此为准）**：按报告做，补两点：layoutBottomRow 的触发还要含 relearnButton.setDot（getNaturalWidth 含 dot?12:0，MiddleLayer.cpp:153）与语言切换；tr() 返回静态表里预解码的 juce::String（引用计数拷贝）。setStatus 在 advancedPanel 不可见时只存状态、显示时刷新；setLatency 先比 double；segmentsFor 以 View 相等缓存；LoudnessHud::setValues 比较量化到 0.1 的整数。
- 风险：LocalisationTests / GlazeStatusTextTests 需保持通过；漏掉某个触发条件会让底排布局不更新。
- 验证：代码评审 + ui-snapshot --bench 总时长；LocalisationTests / GlazeStatusTextTests 不变。

### R9-13 · 状态胶囊呼吸动画每 33 ms 重画整个胶囊（6～8 次文本操作）
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/TopBar.cpp:97`
- 证据：TopBar.cpp:93-98 timerCallback → repaint() 整个 StatusPill；:115-174 每次 paint 做 6～8 次 textWidth/drawText（实验 4 每次 6～43 µs）。聆听/校准中可达 15 s 以上。
- 机理：只有圆点透明度在变，文字整形每秒白做约 200 次。
- 研究修法：呼吸只 repaint(dotBounds.expanded(4))；或把文字部分单独成子组件 setBufferedToImage。
- 核验理由：TopBar.cpp:93-98 timerCallback → 整个胶囊 repaint()；:115-174 每次 paint 1 次 sepWidth + 每段 textWidth+drawText，3 段时约 8 次文本操作；:73-74 聆听/校准中 startTimerHz(30)。但「只 repaint(dotBounds.expanded(4))」不省整形：JUCE 只光栅裁剪，paint() 照跑全部 textWidth/drawText，且圆点 x 在 paint 里才算出（:136-147）。
- **核验调整后的修法（以此为准）**：把呼吸圆点做成 StatusPill 的独立子组件（自己 repaint 小矩形），或文字部分 setBufferedToImage，或 paint 里按 g.clipRegionIntersects(文字区) 跳过文字段；仅缩小 repaint 矩形无效。
- 风险：无；macOS 上这是单个小矩形，没有包围盒问题。
- 验证：ui-snapshot --bench；肉眼。

### R9-14 · 听原声时主旋钮每次 paint 开一层 400²～800² 透明图层
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/GlazeKnob.cpp:195`
- 证据：GlazeKnob.cpp:194-195 `if (comparing) g.beginTransparencyLayer(comparingOpacity);` … :275-276 endTransparencyLayer；瓷面已在 render 时去色（:107）。
- 机理：每次 paint 分配并合成一张裁剪区大小的 ARGB 图层，只为整体 55 % 不透明度；各元素直接乘 α 即可等价。
- 研究修法：drawGlaze(…, opacity × 0.55)，轨道/指针/把手/文字颜色 withMultipliedAlpha(0.55)，去掉透明图层。
- 核验理由：GlazeKnob.cpp:194-195、275-276 beginTransparencyLayer(0.55)；软件渲染器按裁剪区分配 ARGB 层（juce_RenderingHelpers.h:2694-2710），CoreGraphics CGContextBeginTransparencyLayer（juce_CoreGraphicsContext_mac.mm:564-569）同样开层——开销属实。但「各元素直接乘 α 即可等价」错误：图层是先合成再整体 55%，逐元素乘 α 会让影子透过瓷面、灰轨道透过把手白点、帽子透过指针与数字，差异远超 ±1 LSB；setAlpha(0.55) 未缓存时 JUCE 同样开层（juce_Component.cpp:2079-2084）。
- **核验调整后的修法（以此为准）**：与 R9-01 的主旋钮自管缓存合并：整颗旋钮合成进缓存图，贴图时乘 0.55（等价于图层合成，StandardCachedComponentImage 也是 setColour(black.withAlpha(owner.getAlpha())) 后贴，juce_StandardCachedComponentImage.h:94），层只在缓存重生成时开一次。
- 风险：无独立风险；依赖 R9-01 的缓存实现。
- 验证：截图对比（α 乘法与图层合成在 8 位下 ±1 LSB）；B 状态下 GlazeKnob paint 时长 −0.5～−2 ms。
