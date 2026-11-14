# T-112 · 界面包 B：字号与对比度

2026-11-13，T-114 发版总线第二波 L6（工作树 `wf_6fe0d95d-c56-2`，分支 `t114/l6-t112-type-contrast`，基点 `f4741f5`）。
依据：T-105 体检 4-01（真机上字偏小、数字比旁边的字大两成、等宽串里的中文更小）、4-19 后半（旁通的样子）、4-20（三级灰字与紫字对比度不够），
Max 2026-11-12 拍板：**正文 em 11；新装默认大小 = min(125 %, 屏幕放得下的最大值)；用户已经存过的大小不动**。

> 本线按约束**没有编译、没有跑测试、没有出图**：宽度是用 Python + 字体文件按 JUCE 同样的前进宽算的（第 4 节写了口径），
> 对比度按 WCAG 2.x 公式算；第 8 节是交给主会话在 108 / DGX 上跑的命令。

## 1. 改了什么（一句话一项）

| 项 | 做法 | 代码 |
|---|---|---|
| B1 按 em 定字号 | `makeFont` 用 `FontOptions::withPointHeight`（点高 = em 像素），不再按 JUCE 字高（升部 + 降部）给 | `CeladonLookAndFeel.cpp` `makeFont` |
| B1 等宽串里的中文 | JUCE 换回退字体时拷贝整个 `Font`、保留点高，回退出来的中文与数字同 em（原来按 JUCE 字高给，回退字体升降部大，中文更小） | 同上；⑪d 检查守住 |
| B2 六档字号 | `TextSize` 七个值（micro 9 / caption 10 / body 11 / value 11 / title 13 / display 22 / displayLarge 32），数只写在 `typescale` 一处；`uiFont` / `monoFont` 只收档位、不收数字，分段选择器 `setFont` 换成 `setTextStyle (档, 字重, 等宽?)` | `CeladonLookAndFeel.h`、`Controls.h` |
| B3 三级灰 | ink3 `#637780` → `#566A73` | `palette::ink3` |
| B3 紫字 | 新增 `palette::violetText #5438D9`：紫色的**字**一律用它（`violet #6A4DFF` 只画弧线、圆点、出力条）。左侧板原来文件内私有的同值常量删掉 | 分段的「自动」、CLAY 自动、GLAZE 加量、胶囊自动风格、增益道说明 |
| B3 橙字 | 橙色的字一律 `warningText #C2410C`（`warning` 只画图形）；逐处核对过，没有用 `warning` 画字的 | — |
| B3 增益道说明 | 紫字不再压到 0.8 透明（对增益道底只剩 4.26） | `MiddleLayer.cpp` |
| B3 侧板「等待」 | ink4 → ink3（ink4 对频谱底 2.66，这是要读的字） | `MiddleLayer.cpp` |
| 4-19 旁通 | 透明度常量集中到 `bypassLook::opacity = 0.55`（卡片 T-111 已是 0.55；左侧板旁通行原来 0.35 一并改）；`bypassLook::saturation = 0`：小旋钮数值弧、彗尾、GLAZE 紫色加量与圆点、滑条填充与圆圈描边、左侧板出力条与紫字去色。卡片按电源键给控件 `setModuleBypassed` | `ModuleCards.cpp`、`Controls.cpp`、`Dial.cpp`、`MiddleLayer.cpp` |
| 4-19 图例 | 关着时空心色块 + 三级灰字：T-111 已做，本线只换了字号档 | `SpectrumView.cpp` |
| B4 新装默认 125 % | `UiSettings` 的默认大小改成 `std::optional`：设置文件里没有这一项 = 新装，用 `firstRunZoom = 1.25`；存过的（哪怕就是 1）照用；工程里存过的优先。打开窗口再按屏幕收小（`fitZoomToScreen`，原样） | `UiSettings.h/.cpp`、`PluginEditor.cpp readStoredZoom` |
| 截断检查 | 单行字一律经 `drawTextLine` 画；截图工具打开 `textfit` 记录时每次先量字宽、比框宽就记下（插件里从不打开） | `CeladonLookAndFeel.h/.cpp`、`SnapshotMain.cpp` ⑪c |
| 版面 | 放大后放不下的几处改框或改字（第 4 节） | 见下 |

## 2. 六档字号与每个调用点

档位定义（`CeladonLookAndFeel.h`）：

| 档 | em（100 %） | 用在 |
|---|---|---|
| micro | 9 | 刻度、单位（LUFS-I、dB/oct、70% / 200%）、序号、历史图说明 |
| caption | 10 | 说明、图例、副标题、表头、小标签、角落提示、版本 / 延迟、CLAY 小图标签 |
| body | 11 | 控件名、分段文字、胶囊、气泡与浮层正文、状态行（Max 定正文 em 11） |
| value | 11 | 数值（等宽） |
| title | 13 | 卡片名、浮层标题、品牌、GLAZE 标题（半粗） |
| display | 22 | 右侧板整体响度 |
| displayLarge | 32 | 主旋钮百分比（旋钮自己的 224 px 设计坐标，画布上整颗缩到 200 px ≈ em 28.6）、缩放提示 |

改前改后各平台的实际 em（正文那一档，原来多数写 11.5 ～ 12 的 JUCE 字高）：

| 平台 / 字体 | em ÷ JUCE 字高 | 原来 12 的 em | 现在 body |
|---|---|---|---|
| macOS 苹方 | 1 / 1.40 | 8.6 | 11 |
| Windows 微软雅黑 UI | 1 / 1.27 | 9.4 | 11 |
| Linux DejaVu Sans（截图工具） | 1 / 1.164 | 10.3 | 11 |
| 等宽 Menlo / DejaVu Sans Mono | 1 / 1.164 | 10.3 | 11 |

所以 Mac 上字变大最多（约 +28 %），Linux 截图上只大约 +7 %——**截图看着变化不大不代表真机也一样**，真机以 Mac / Windows 为准。
数字与旁边的字原来差两成（苹方 1.40 vs Menlo 1.164），现在同 em。

调用点 → 档（脚本从源码扫出来的，`uiFont` / `monoFont` / `setTextStyle` 共 115 处；「画什么」是代码里的表达式）：

| 文件:行 | 档 | em | 字体 | 字重 | 画什么（代码里的表达式） |
|---|---|---|---|---|---|
| `PluginEditor.cpp:355` | body | 11 | 分段·界面 | medium | `styleSelector` |
| `PluginEditor.cpp:362` | body | 11 | 分段·界面 | medium | `abSelector` |
| `PluginEditor.cpp:416` | value | 11 | 等宽 | regular | `valueEntry.setFont (monoFont (TextSize::value));` |
| `ui/CeladonLookAndFeel.cpp:155` | body | 11 | 界面 | medium | `font` |
| `ui/CeladonLookAndFeel.cpp:155` | body | 11 | 界面 | regular | `font` |
| `ui/CeladonLookAndFeel.cpp:370` | body | 11 | 界面 | regular | `return isJuceDefault ? uiFont (TextSize::body) : r` |
| `ui/CeladonLookAndFeel.cpp:375` | body | 11 | 界面 | regular | `return uiFont (TextSize::body);` |
| `ui/Controls.cpp:488` | body | 11 | 界面 | regular | `name` |
| `ui/Controls.cpp:524` | value | 11 | 等宽 | regular | `getValueText (true)` |
| `ui/Controls.cpp:624` | value | 11 | 等宽 | medium | `text` |
| `ui/Controls.cpp:675` | body | 11 | 分段·界面 | medium | `setTextStyle (TextSize::body, Weight::medium);` |
| `ui/Controls.cpp:1065` | caption | 10 | 界面 | semibold | `return (int) std::ceil (textWidth (withLetterSpaci` |
| `ui/Controls.cpp:1077` | caption | 10 | 界面 | semibold | `text` |
| `ui/Controls.cpp:1128` | body | 11 | 界面 | regular | `name` |
| `ui/Controls.cpp:1145` | value | 11 | 等宽 | regular | `shownText` |
| `ui/Dial.cpp:433` | body | 11 | 界面 | medium | `name` |
| `ui/Dial.cpp:487` | value | 11 | 等宽 | medium | `valueFont` |
| `ui/Dial.cpp:488` | caption | 10 | 等宽 | regular | `autoFont` |
| `ui/GlazeKnob.cpp:267` | caption | 10 | 界面 | medium | `tr (Text::fine` |
| `ui/GlazeKnob.cpp:350` | displayLarge | 32 | 界面 | light | `numberFont` |
| `ui/GlazeKnob.cpp:351` | caption | 10 | 界面 | regular | `percentFont` |
| `ui/LevelMeters.cpp:193` | caption | 10 | 界面 | semibold | `titleFont` |
| `ui/LevelMeters.cpp:233` | micro | 9 | 等宽 | regular | `g.setFont (monoFont (TextSize::micro));` |
| `ui/LevelMeters.cpp:244` | caption | 10 | 等宽 | medium | `shown.peakText` |
| `ui/LevelMeters.cpp:292` | caption | 10 | 等宽 | medium | `shown.peakText` |
| `ui/MiddleLayer.cpp:98` | title | 13 | 界面 | semibold | `titleFont` |
| `ui/MiddleLayer.cpp:99` | body | 11 | 界面 | regular | `subtitleFont` |
| `ui/MiddleLayer.cpp:100` | caption | 10 | 界面 | medium | `chipFont` |
| `ui/MiddleLayer.cpp:196` | body | 11 | 界面 | medium | `font` |
| `ui/MiddleLayer.cpp:222` | body | 11 | 界面 | medium | `font` |
| `ui/MiddleLayer.cpp:301` | caption | 10 | 界面 | regular | `line1` |
| `ui/MiddleLayer.cpp:322` | caption | 10 | 界面 | regular | `font` |
| `ui/MiddleLayer.cpp:333` | caption | 10 | 界面 | regular | `font` |
| `ui/MiddleLayer.cpp:401` | caption | 10 | 界面 | medium | `uiFont (TextSize::caption` |
| `ui/MiddleLayer.cpp:411` | micro | 9 | 等宽 | regular | `numberFont` |
| `ui/MiddleLayer.cpp:412` | caption | 10 | 界面 | semibold | `nameFont` |
| `ui/MiddleLayer.cpp:413` | value | 11 | 等宽 | regular | `valueFont` |
| `ui/MiddleLayer.cpp:449` | caption | 10 | 界面 | regular | `row.badge` |
| `ui/MiddleLayer.cpp:472` | body | 11 | 界面 | regular | `values.statusLine` |
| `ui/MiddleLayer.cpp:481` | caption | 10 | 等宽 | regular | `monoFont (TextSize::caption)` |
| `ui/MiddleLayer.cpp:561` | value | 11 | 等宽 | medium | `monoFont (TextSize::value` |
| `ui/MiddleLayer.cpp:570` | display | 22 | 界面 | light | `big` |
| `ui/MiddleLayer.cpp:578` | micro | 9 | 等宽 | regular | `"LUFS-I"` |
| `ui/MiddleLayer.cpp:599` | caption | 10 | 界面 | regular | `uiFont (TextSize::caption)` |
| `ui/MiddleLayer.cpp:601` | value | 11 | 等宽 | medium | `column.value` |
| `ui/MiddleLayer.cpp:642` | micro | 9 | 等宽 | regular | `unicodeMinus() + "14"` |
| `ui/MiddleLayer.cpp:646` | caption | 10 | 等宽 | regular | `mono` |
| `ui/MiddleLayer.cpp:672` | micro | 9 | 界面 | regular | `tr (Text::sideAutoGain` |
| `ui/MiddleLayer.cpp:684` | caption | 10 | 界面 | regular | `tr (Text::sideWaiting` |
| `ui/MiddleLayer.cpp:765` | micro | 9 | 界面 | regular | `tr (Text::sideHistory` |
| `ui/ModuleCards.cpp:117` | value | 11 | 等宽 | regular | `number` |
| `ui/ModuleCards.cpp:124` | title | 13 | 界面 | semibold | `title` |
| `ui/ModuleCards.cpp:129` | caption | 10 | 等宽 | regular | `noteFont` |
| `ui/ModuleCards.cpp:132` | caption | 10 | 界面 | regular | `getDescription()` |
| `ui/ModuleCards.cpp:163` | body | 11 | 分段·界面 | medium | `row.modes->setTextStyle (TextSize::body, Weight::m` |
| `ui/ModuleCards.cpp:326` | body | 11 | 界面 | regular | `tr (names[i]` |
| `ui/ModuleCards.cpp:384` | caption | 10 | 等宽 | regular | `labelFont` |
| `ui/ModuleCards.cpp:464` | body | 11 | 分段·界面 | medium | `speed` |
| `ui/ModuleCards.cpp:685` | body | 11 | 界面 | regular | `tr (Text::speed` |
| `ui/Popovers.cpp:99` | body | 11 | 分段·界面 | medium | `languageSelector` |
| `ui/Popovers.cpp:164` | caption | 10 | 界面 | regular | `juce::Font SettingsPanel::reduceMotionNoteFont()  ` |
| `ui/Popovers.cpp:183` | title | 13 | 界面 | semibold | `tr (Text::settings` |
| `ui/Popovers.cpp:191` | body | 11 | 界面 | medium | `tr (Text::windowSize` |
| `ui/Popovers.cpp:194` | caption | 10 | 等宽 | regular | `pxFont` |
| `ui/Popovers.cpp:200` | value | 11 | 等宽 | semibold | `percentText (zoom)` |
| `ui/Popovers.cpp:214` | caption | 10 | 等宽 | medium | `percentText (presets[i])` |
| `ui/Popovers.cpp:236` | micro | 9 | 等宽 | regular | `"70%"` |
| `ui/Popovers.cpp:269` | caption | 10 | 界面 | regular | `trFormat (Text::screenSize` |
| `ui/Popovers.cpp:276` | caption | 10 | 界面 | medium | `tr (Text::tooBig` |
| `ui/Popovers.cpp:288` | caption | 10 | 界面 | regular | `tr (Text::sizeNote` |
| `ui/Popovers.cpp:300` | body | 11 | 界面 | medium | `tr (Text::reduceMotion` |
| `ui/Popovers.cpp:322` | body | 11 | 界面 | medium | `tr (Text::language` |
| `ui/Popovers.cpp:332` | caption | 10 | 等宽 | regular | `tr (Text::version` |
| `ui/Popovers.cpp:441` | body | 11 | 界面 | regular | `tr (lines[i]` |
| `ui/Popovers.cpp:459` | title | 13 | 界面 | semibold | `tr (Text::helpTitle` |
| `ui/Popovers.cpp:467` | body | 11 | 界面 | regular | `bulletFont` |
| `ui/Popovers.cpp:548` | body | 11 | 分段·界面 | medium | `tabs` |
| `ui/Popovers.cpp:549` | body | 11 | 分段·界面 | medium | `harshMode` |
| `ui/Popovers.cpp:550` | body | 11 | 分段·界面 | medium | `targetMode` |
| `ui/Popovers.cpp:551` | body | 11 | 分段·界面 | medium | `dither` |
| `ui/Popovers.cpp:699` | title | 13 | 界面 | semibold | `tr (Text::advanced` |
| `ui/Popovers.cpp:707` | value | 11 | 等宽 | medium | `trFormat (Text::harshReduction` |
| `ui/Popovers.cpp:720` | caption | 10 | 界面 | medium | `tr (Text::targetCurveTitle` |
| `ui/Popovers.cpp:724` | body | 11 | 界面 | medium | `referenceStatusText()` |
| `ui/Popovers.cpp:734` | body | 11 | 界面 | medium | `tr (Text::ditherLabel` |
| `ui/Popovers.cpp:743` | value | 11 | 等宽 | regular | `trFormat (Text::previewAttenuation` |
| `ui/Popovers.cpp:792` | micro | 9 | 等宽 | regular | `"+6"` |
| `ui/Popovers.cpp:798` | micro | 9 | 界面 | regular | `tr (Text::lowBand` |
| `ui/Popovers.cpp:830` | value | 11 | 等宽 | regular | `w` |
| `ui/Popovers.cpp:833` | caption | 10 | 界面 | regular | `w += 6.0f + textWidth (uiFont (TextSize::caption),` |
| `ui/Popovers.cpp:844` | value | 11 | 等宽 | regular | `text` |
| `ui/Popovers.cpp:851` | caption | 10 | 界面 | regular | `labelFont` |
| `ui/Popovers.cpp:882` | body | 11 | 界面 | regular | `title` |
| `ui/Popovers.cpp:886` | displayLarge | 32 | 界面 | light | `percentText (zoom)` |
| `ui/Popovers.cpp:890` | value | 11 | 等宽 | regular | `pixelText (zoom) + " px"` |
| `ui/SpectrumView.cpp:76` | micro | 9 | 等宽 | regular | `juce::Font frequencyLabelFont()   { return monoFon` |
| `ui/SpectrumView.cpp:237` | caption | 10 | 界面 | regular | `return (int) std::ceil (8.0f + 10.0f + 5.0f + text` |
| `ui/SpectrumView.cpp:270` | caption | 10 | 界面 | regular | `text` |
| `ui/SpectrumView.cpp:280` | caption | 10 | 分段·等宽 | regular | `choices` |
| `ui/SpectrumView.cpp:301` | caption | 10 | 界面 | regular | `return (int) std::ceil (8.0f + textWidth (uiFont (` |
| `ui/SpectrumView.cpp:302` | micro | 9 | 等宽 | regular | `+ 6.0f + textWidth (monoFont (TextSize::micro), "d` |
| `ui/SpectrumView.cpp:307` | caption | 10 | 界面 | regular | `labelWidth` |
| `ui/SpectrumView.cpp:320` | caption | 10 | 界面 | regular | `label` |
| `ui/SpectrumView.cpp:324` | micro | 9 | 等宽 | regular | `"dB/oct"` |
| `ui/SpectrumView.cpp:350` | micro | 9 | 等宽 | regular | `font` |
| `ui/SpectrumView.cpp:392` | value | 11 | 等宽 | semibold | `readoutWidth` |
| `ui/SpectrumView.cpp:628` | caption | 10 | 界面 | regular | `font` |
| `ui/SpectrumView.cpp:934` | micro | 9 | 等宽 | regular | `dbFont` |
| `ui/SpectrumView.cpp:955` | value | 11 | 等宽 | semibold | `shownReadout` |
| `ui/SpectrumView.cpp:1224` | caption | 10 | 界面 | regular | `label` |
| `ui/TopBar.cpp:27` | body | 11 | 界面 | regular / medium | `pillFont (medium)`（状态胶囊） |
| `ui/TopBar.cpp:28` | title | 13 | 界面 | semibold | `juce::Font brandFont()              { return withL` |
| `ui/TopBar.cpp:29` | caption | 10 | 界面 | regular | `juce::Font subtitleFont()           { return withL` |
| `ui/TopBar.cpp:30` | body | 11 | 界面 | regular | `juce::Font advancedFont()           { return uiFon` |
| `ui/TopBar.cpp:291` | body | 11 | 等宽 | regular | `"?"` |

## 3. 颜色与对比度

口径：WCAG 2.x 相对亮度；半透明的底按实际叠在什么上面合成（侧板 = 白 0.86 叠画布、频谱小框 = 白 0.85 叠频谱底、增益道 = 浅紫 0.7 叠侧板）。
小字（em < 14 px，本界面除 display 两档外全部）要 ≥ 4.5 : 1。

| 字色 | 白 `#FFFFFF` | 画布 `#EDF2F4` | 分段底槽 `#EEF3F5` | 语言底槽 `#E3EBEE` | 频谱底 `#F3F8F9` | 关着的胶囊 `#F3F6F7` | 侧板 `#FCFDFD` | 频谱小框 `#FDFEFE` |
|---|---|---|---|---|---|---|---|---|
| ink3 原 `#637780` | 4.69 | **4.15** | **4.19** | **3.88** | **4.38** | **4.32** | 4.60 | 4.64 |
| **ink3 新 `#566A73`** | 5.67 | 5.02 | 5.07 | 4.69 | 5.29 | 5.22 | 5.56 | 5.61 |
| ink2 `#4B5E66` | 6.79 | 6.02 | 6.07 | 5.62 | 6.34 | — | 6.66 | 6.72 |
| ink `#0D1B21` | 17.56 | 15.56 | — | — | — | — | 17.23 | — |
| ink4 `#8A9CA3`（只画图形） | 2.85 | — | — | — | 2.66 | — | — | — |

| 字色 | 对哪些底 |
|---|---|
| violet `#6A4DFF`（改为只画图形） | 白 5.10、**浅紫 `#EEEAFF` 4.33**、侧板 5.00 |
| **violetText `#5438D9`** | 白 7.13、浅紫 6.06、侧板 7.00、增益道 `#F2F0FE` 6.34（原 0.8 透明时 4.26） |
| warningText `#C2410C` | 白 5.18、浅橙 `#FFF4EC` 4.78、画布 4.59、分段底槽 4.63、侧板 5.08 |
| warning `#E8590C`（只画图形） | 白 3.58 |
| c600 `#127F90`（卡片序号、侧板序号） | 白 4.70、侧板 4.62 |
| c700 `#0E6473`（操作提示） | 白 6.80、c100 5.74、画布 6.02 |
| c800 `#0A4A57` | 白 9.86、c100 8.32、侧板 9.67、频谱小框 9.76 |
| ink 0.6 透明（不可调时的滑条数值） | 白 4.63 |

例外（写清楚、不算漏）：

- **旁通的模块**：内容整体 0.55 透明（Max 4-19 定的数）。ink 字对白 3.92、对侧板 3.90，ink3 2.29——不到 4.5。原来 0.35 时 ink 只有 2.22。
  旁通表示「这一块现在不处理」，WCAG 1.4.3 对「不活动的界面组件」不作要求；它照样能调，所以选了能读的 0.55，而不是更淡。
- ink4 只用在图形（缩放把手、关着的电源圈），不画要读的字；侧板「等待」原来用它，已改 ink3。

## 4. 放不下的检查与版面调整

**口径**：JUCE `drawText` 放不下时按框宽截掉末尾（不带省略号的就是悄悄丢字）。这里按 JUCE 同样的前进宽（字体文件 hmtx，
字距 = 系数 × JUCE 字高）算。**Linux 截图上的数是 Mac / Windows 的上界**：按 em 定字号后，汉字在各字体里都是 1 em；拉丁字母
同一句按 em 算 DejaVu Sans 最宽（例：「Turns off particles…」DejaVu 22.1 em、苹方 20.7、雅黑 UI 21.0、Noto 20.4）。所以截图工具 ⑪c
在 Linux 上过了，Mac / Windows 也放得下。界面缩放是整体变换，逻辑宽度在 70 / 100 / 125 % 下相同。

放大后量出来放不下、改了的：

| 位置 | 量值（Linux DejaVu，放大后） | 改法 |
|---|---|---|
| GLAZE 标题行（英文「GLAZE · Smart mastering · Manual limit」） | 约 257 / 260 px | 标题框 (470, 305, 260) → (420, 305, 360)，中心仍 600，不碰两侧板（404 / 796）；三段的框都夹到组件宽以内，超了 textfit 照样记 |
| 左侧板 COLOR 行「Tone −1.5 · Glue +12 · Heat +20」 | 约 205 px（列宽 176，压到 0.86 也不够） | 出力条 96 → 84 px、文字列 left + 188 → left + 176（列宽 188 → 200） |
| 左下角提示 | 原来固定 330 宽、伸进风格选择底下；英文旧工程提示 321 / 334 px | 宽度在 `layoutBottomRow` 里收到风格选择左边 8 px（最窄约 298 px）；英文两句改短：「Old warm-up result discarded, so the sound will change」约 279 px、「Genre/delivery reset: General + Standard, sound changes」约 291 px |
| 设置「屏幕 …」与「超出屏幕」同一行 | 屏幕标签框 160 与右边「超出屏幕」框 100 相撞；英文「Larger than screen」中等字重约 98（DejaVu 没有 Medium，按常规；苹方 Medium 更窄） | 屏幕标签 160 → 150、「超出屏幕」100 → 104 |
| 设置最下一行 版本 / 延迟 | 两个 160 的框在 336 宽里重叠 | 两边各 140 |
| 高级「刺耳音抑制 … dB」 | 右对齐框左缘 326 起不够 | 改从 316 起 |
| 高级标题 | 放到标题档后与右上 ✕ 相近 | 框右边让出 40 |
| 频谱斜率标签 | 标签框原来固定宽 | 框宽到斜率选项左边 8 px |
| OUT / GR 数值 | 「−59.9」等宽说明档中等字重约 28.6 / 30 px | 原样，字距 −0.3 留余量 |
| 主旋钮帽「100%」 | 约 68.6 px，帽子直径 72 | 原样（注释写明） |
| CLAY「Manual」分段 | body 中等字重 40.0（DejaVu 落到常规）/ 段宽 41.3 | 原样，余量最小的一处，⑪c 守着 |

其余量过、放得下的：顶栏胶囊（品牌右缘约 215、右边键约 1007，胶囊最宽 560 够）、卡片名与说明、侧板表头与三列、设置各行、
帮助、高级三页、气泡、数值气泡、缩放提示、电平表标题与刻度。

字体随平台变（与 T-102 记录相同）：JUCE 在 Linux 上只扫 `.ttf` / `.otf`，DGX 的 Noto Sans CJK 是 `.ttc`，界面字体落到 `system-ui` =
DejaVu Sans（没有 Medium / Semibold，中粗、半粗都按常规画），汉字回退 Noto Sans CJK JP，等宽是 DejaVu Sans Mono。

## 5. 截图工具（`tools/ui-snapshot`）

- **新检查**（放在旧工程那组之后，详见 README）：
  - ⑪c 单行文字放得下：中 / 英 × 70 / 125 / 100 % × 13 个场景（主界面、设置、帮助、高级三页、听原声、状态行三句、旧工程提示三种），6 条，每条列出超框的场景、文字、字宽 / 框宽；另一条查记录用完已关。
  - ⑪d 六档字号：7 条（每档界面 / 等宽 / 等宽回退到汉字的 em）+ 1 条（正文 11、各档由小到大）。
  - ⑪e 新装默认大小：6 条 `initialZoom` 纯函数 + 1 条左下提示右缘 ≤ 风格选择左边 − 8。
  - 共 22 条新 PASS 行；原有检查的期望一条都没改（字号与颜色不在原检查的断言里，⑪b 只改注释：字体换成说明档，仍是同一条逻辑）。
- **新状态** `zh-glaze-tooltip` / `en-glaze-tooltip`：`showGlazeTooltip` 原来给 T-038 的 House 气泡用，T-048（`a49e94f`）删掉那两张后没人调了；
  GLAZE 标题的气泡一直在界面上（`glazeTitle.setTooltip`），字号放大后要看它，所以接回来而不是删。锚点 (440, 326) 仍在新标题框 (420 ～ 780) 下沿以内。
  按去掉注释后的代码扫了一遍工具里 92 个函数，没被调用的只有这一个。
- 缓存检查里的测试子组件原来 `uiFont (12.0f)`，改成正文档（`uiFont` 不再收数字）；这一组比的是缓存与整块重画逐字节相同，字号不影响结论。
- 出图仍按默认大小 1 倍（每个状态之前 `setDefaultZoom (1.0)`），新装 125 % 不影响改前改后逐像素对比；检查开跑前 `resetUiSettings` 照旧写 1.0。

## 6. 存档与兼容

- 工程（APVTS 状态树 `uiZoom`）：读法不变——有就用（钳到 0.7 ～ 2.0，非有限按 1）。0.1.8.10 存的工程照常读。
- 用户设置 `Celadon.settings`：存过 `defaultZoom` 的照用（包括 1.0），只有从没存过的新装才按 125 %。拖动缩放时照旧写盘。
- 只动界面：`Source/dsp/**`、处理器、参数表一行没改，GLAZE 0 的输出逐位不变。

## 7. 没做 / 留给别人

- 官网：英文 FAQ、GLAZE 页、安装页、界面页与术语表引用了两句旧的英文角落提示原句（`site/src/content/docs/en/faq.md:12,14`、`glaze.md:59,71`、`install.md:62`、`interface.md:23`、`site/src/i18n/glossary.md:30`），要同步改成第 4 节的新句；界面截图要按新字号重拍。不在本线写域。
- `celadon_tests` 不编 `UiSettings.cpp`，新装默认的逻辑由截图工具 ⑪e 直接调纯函数 `initialZoom` 守。
- 屏幕放不下 125 % 时的收小（`fitZoomToScreen`）与平台的屏幕有关，没有加自动检查，靠真机看。

## 8. 验收命令（主会话跑；先 108、其次 DGX）

```bash
# 1) 108（testbox x86-1，run-gui.sh 打包方式见 docs/validation/2026-11-13-T114-release-0.1.8.11/README.md 第 0 节）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=../JUCE
cmake --build build --target celadon_tests celadon_ui_snapshot
build/celadon_tests --only HelpText,HelpTextClay,HelpTextGlaze,Localisation,GlazeStatusText,DialMotion
xvfb-run -a -s "-screen 0 1920x1080x24" build/celadon_ui_snapshot --out shots-check --only check     # 全部 PASS、退出码 0

# 2) DGX：同上 --only check 一遍（aarch64），再出改前改后对比图（--deterministic，1 倍）
STATES=zh-default,en-default,zh-settings,en-settings,zh-settings-zoom70,en-settings-zoom70,zh-help,en-help,zh-advanced,en-advanced,zh-target,en-release,zh-original,en-original,zh-truepeak,zh-glaze0,zh-zoom70,zh-zoom150,zh-status-follow,en-status-guard,en-status-export,zh-legacy-notice,en-legacy-notice,en-genre-reset,en-genre-reset-both,zh-guard-recovering,en-guard-tooltip
xvfb-run -a build/celadon_ui_snapshot --deterministic --out shots-before --only "$STATES"                                     # f4741f5 的副本
xvfb-run -a build/celadon_ui_snapshot --deterministic --out shots-after  --only "$STATES,zh-glaze-tooltip,en-glaze-tooltip"   # 本分支

# 3) 108 主机 Wine（MSVC）：界面目标编得过、--only check 全过
CELADON_WIN_HOST=maxsong@100.84.85.68 bash scripts/windows-wine.sh ui
```

对比图要看的点：字整体变大、数字与旁边的字一样大；英文 GLAZE 标题行不截；左侧板 COLOR 行不截、出力条短了 12 px；左下英文旧工程提示
不压到风格选择；设置「屏幕 …」与「超出屏幕」不相撞；关掉一个模块后弧线 / 滑条变灰、字能读；三级灰字深了一点；「自动」的紫字深了一点。
`zh-glaze-tooltip` / `en-glaze-tooltip` 只在改后有。
