# R9 · 界面与 JUCE GUI 深度研究（PluginEditor / Source/ui/**）

> T-032 阶段 A 研究代理 R9，2026-09-29 22:2x PDT。工作树 `.claude/worktrees/fable-deep-research-634a5e`（main `e6b445b`，v0.1.8.5）。
> 只读；本报告是唯一写入的文件。数值实验在 107（`~/.cache/celadon-t032/R9/`，做完已删）上做，脚本副本在本会话 scratchpad `r9/`。

## 0. 一句话结论

界面没有会崩、会漏、会撕裂的硬伤（参数附着、手势 / 撤销、无锁交换、悬垂指针、缩放往返都核过，见第 4 节），但**每帧的重绘范围与位图用法**有明显浪费，而且**频谱仪的频点聚合算法有 +5～+7 dB 的频率相关偏差**（宽带信号高频显示偏高，会误导母带的频谱平衡判断）。15 条结论按「优先级 × 置信度」排序：1 条 P0、6 条 P1、8 条 P2；全部只改界面，音频输出逐位不变。

与档案里「不重报」项的关系：拖动 GLAZE 时主线程 21 % ↔ R9-02 / R9-07 给出机理与修法；「打开就高温」↔ R9-01 / R9-05 / R9-06 / R9-04 是它的构成。本报告只给机理与可量化的修法，不复述症状。

## 1. 读了什么

- 档案：`01-框架/设计规格-界面-V4融合版.md`、`设计规格-界面.md` 第 9 节、`ADR-021`、`ADR-005`、`ADR-009`、`04-协调/2026-09-29-T-032 计划`。
- 代码：`Source/PluginEditor.{h,cpp}` 全文；`Source/ui/` 全部 32 个文件；`Source/AnalyzerTap.h`；`PluginProcessor.h` 与 `.cpp` 的界面接口段（`processChain`、`getGlazeEffectiveParamsForUi`、`createEditor`、`timerCallback`）；`dsp/CeladonChain.{h,cpp}` 的状态接口、`dsp/GlazeController.cpp` 的 `SeqLockSnapshot` 与 `getStatus`、`dsp/ReferenceSpectrum.h` 的 `Publication`、`dsp/BalanceModule.cpp` 的三个发布接口；`tests/SpectrumAnalyserTests.cpp`；`tools/ui-snapshot/SnapshotMain.cpp`。
- JUCE 9.0.2 源码（`/Users/maxsong/All-Project/JUCE`）：`juce_Component.cpp`（重绘、带变换子组件、空裁剪早退、默认无障碍处理器）、`juce_StandardCachedComponentImage.h`、`juce_Path.cpp`（弧线按 0.05 rad 折线化）、`juce_RenderingHelpers.h`（贴图对齐快路径）、`juce_GlyphArrangement.{h,cpp}`（HarfBuzz 整形）、`juce_NSViewComponentPeer_mac.mm`（按脏矩形列表裁剪；macOS peer 固定用 `CoreGraphicsContext`）、`juce_ColourGradient.cpp`（每次渐变填充建查找表）。

## 2. 实验数据（107：AMD EPYC 74F3，g++ 15 -O2；JUCE 9.0.2 软件渲染器离屏渲染）

软件渲染器 = Linux 与 Windows 非 Direct2D 路径的真实开销；macOS 用 CoreGraphics（CPU 光栅、部分 SIMD），绝对值约低 2～5 倍，**相对倍数可迁移**。

### 实验 1 · 瓷面逐像素底层复刻（Porcelain.cpp:424-462 同一算法）与静态底图颗粒循环（PluginEditor.cpp:529-542）

| 像素比 | 图尺寸 | 全量底层渲染 | 每像素 | 两张基底图混合 |
|---|---|---|---|---|
| 1.0 | 180² | 2.03 ms | 62.7 ns | 0.17 ms |
| 2.0 | 360² | 6.75 ms | 52.0 ns | 0.66 ms |
| 2.5 | 450² | 9.93 ms | 49.0 ns | 1.03 ms |
| 3.0 | 540² | 13.6 ms | 46.8 ns | 1.49 ms |
| 4.0（200 % × Retina） | 720² | **23.0 ms** | 44.3 ns | 2.65 ms |

静态底图颗粒循环：1× 1.0 Mpx 10.9 ms；2× 3.9 Mpx **43.6 ms**（不含 5000 个点、7 个阴影，见实验 3 B6）。

### 实验 2 · 频谱仪频点聚合偏差（SpectrumAnalyser.cpp:160-207 同一算法，Hann 8192 @ 48 kHz）

2a. 200 帧逐帧 dB 平均、相对 100 Hz 读数（白噪声）：

| 频率 | 每点 bin 数 | 取最大（现状） | 取均值 |
|---|---|---|---|
| 500 Hz | 1.23 | +3.66 dB | +1.49 |
| 1 kHz | 2.46 | +4.93 | +1.74 |
| 5 kHz | 12.3 | +6.70 | +2.02 |
| 10 kHz | 24.6 | +7.62 | +2.13 |
| 20 kHz | 49.2 | **+8.72** | +2.42 |

2b. 按真实显示动态（升 0.6 / 落 0.15，dB 域）150 帧稳态、相对 100 Hz：

| 信号 | 聚合 | 0.5k | 1k | 2k | 5k | 10k | 16k | 20k |
|---|---|---|---|---|---|---|---|---|
| 白噪 | 最大（现状） | +2.4 | +2.0 | +3.4 | +4.8 | **+6.6** | **+7.0** | +5.9 |
| 白噪 | 均值 | +3.2 | −1.5 | +0.1 | −0.2 | −0.8 | −1.1 | −1.0 |
| 粉噪 + 3 dB/oct 显示斜率 | 最大（现状） | +3.0 | +2.9 | +5.3 | +5.7 | **+7.5** | **+7.8** | +6.7 |
| 粉噪 + 3 dB/oct | 均值 | −4.3 | −2.7 | −4.0 | −5.8 | −5.2 | −5.4 | −4.8 |

−6 dBFS 正弦读数：取最大 −6.65 dB（各频率一致）；取均值 1 kHz −11.3、5 kHz −16.0、10 kHz −18.6、16 kHz −20.5 dB（bin 数 4 / 14 / 26 / 41）。
→ 「取最大」让宽带材料从 500 Hz 起逐步偏高，10～16 kHz 偏高 +6.6～+7.8 dB；单纯换成「取均值」又会把正弦压低 5～14 dB，并因 dB 域非对称动态的方差效应在低频（插值区）多出 +3～+5 dB 的峰偏。修法见 R9-03。

### 实验 3 · JUCE 9.0.2 图形微基准（每次 paint 的毫秒数）

```
B1  GLAZE ring @1.79x (401 px): track+31 stroked segs 0.121 ms | pie fills 0.107 ms | cached blit 0.385 ms
B1  GLAZE ring @3.57x (800 px): track+31 stroked segs 0.220 ms | pie fills 0.203 ms | cached blit 1.523 ms
B1b ParamKnob arc @2x: 0.047 ms | @4x: 0.065 ms  (x7 联动旋钮/帧: 0.33 / 0.45 ms)
B2  spectrum @2x: 516 clipped stripe fillRects + fade 7.10 ms | tiled fill + fade 3.33 ms | (fill+2 strokes 2.71 ms; IN dashed 0.37 ms)
B2  spectrum @4x: 516 clipped stripe fillRects + fade 18.73 ms | tiled fill + fade 7.98 ms | (fill+2 strokes 9.17 ms; IN dashed 0.63 ms)
B3  particles @2x: glow gradient alone 3.01 ms | 170 solid + 30 gradient blobs + rings 0.79 ms | with sprite blobs 1.01 ms
B3  particles @4x: glow gradient alone 12.02 ms | 170 solid + 30 gradient blobs + rings 1.85 ms | with sprite blobs 2.60 ms
B4  glaze image 322 px @1.79x: drawImageTransformed (fractional) 1.218 ms | drawImageAt (aligned) 0.248 ms
B4  glaze image 643 px @3.57x: drawImageTransformed (fractional) 4.705 ms | drawImageAt (aligned) 0.978 ms
B6  static layer @2x: 5000 dot fillEllipse 19.1 ms | tiled fill 13.7 ms | one spectrum-panel DropShadow 38.3 ms
```

两点修正我原先的猜测：① 数值弧描边很便宜（0.05～0.22 ms），改扇形填充没意义，**不列结论**；② 贵的是大面积贴图（尤其非整数比例）与径向渐变。

### 实验 4 · 文本整形（HarfBuzz，Liberation Sans 12 px；107 无中文字体，中文只会更贵）

`getStringWidth` 7 字 6.3 µs / 47 字 24.1 µs；`drawText` @2× 7 字 8.5 µs / 47 字 42.8 µs；`Font` 构造 0.07 µs；`withExtraKerningFactor` 0.09 µs；`TextLayout` 47 字换行 19.3 µs。→ 每帧几十次文本操作只有零点几毫秒，文字类优化是次要的。

### 实验 5 · JUCE 回退 FFT（Windows 默认无 vDSP / IPP 时的路径）

8192 点 0.356 ms；16384 点 0.710 ms；32768 点 **1.730 ms** → 两路 × 30 Hz = 2.1 % / 4.3 % / **10.4 %** 单核（48 k / 96 k / 192 k）。

## 3. 结论（按优先级 × 置信度；P2 内按置信度排，R9-15 无障碍项置信度虽高但严重度低、工作量 L，放最后）

### R9-01 · 粒子带整条 1200 × 260 每帧重画，把主旋钮（非整数比例的 720² 贴图）和中层全部文字部件一起拖进每一帧

- 严重度 high · **P0** · 类别 重绘范围 / 浪费 · 工作量 M · 置信度 0.85 · changes_output false
- 文件：`Source/ui/ParticleField.cpp:219-230`（`onVBlank` → `repaint()` 整个组件）、`:38`（45 帧 / s）；`Source/ui/ParticleField.h:34-35`（带 = 画布 y 300 起高 260）；`Source/PluginEditor.cpp:128-135`（粒子带与主旋钮 `(500,322)` 200 × 200 重叠）、`:130-131`（GlazeTitle 在带内）、`:155`、`:171`、`:342-367`（监听提示、左下说明、底排、图例都在带内）；`Source/ui/GlazeKnob.cpp:159-171`（`drawGlaze` 用 `drawImageTransformed(scale 1/pixelsPerUnit)`）。
- 证据（代码）：
  ```cpp
  // ParticleField.cpp:225-229
  if (animating && (lastRepaint < 0.0 || timestampSeconds - lastRepaint >= 1.0 / maxFrameRate))
  { lastRepaint = timestampSeconds; repaint(); }
  ```
  JUCE 对同一脏矩形内按 z 序重画所有相交的组件（`juce_Component.cpp:1975-2010`），主旋钮带变换、不在 opaque 层内，无法被跳过；只有裁剪区为空时 `paint` 才不调（`juce_Component.cpp:1961-1963`）。
- 机理：粒子只在两侧条带（流线终点 x ≤ 600 − 96.43 与 ≥ 696.4，`ParticleField.cpp:34,104-121`）和 r 106～236 的波纹环上动，但每帧把整条带标脏 → 每帧（≤ 45 次 / s）都重画：主旋钮（B4：瓷面贴图 1.2 ms @1.79× / 4.7 ms @3.57×，加轨道弧 0.12～0.22 ms、影子 / 把手两次径向渐变、两次文本）、GlazeTitle（6 次文本操作）、风格 Segmented（4 项 × 宽度 + 绘制 ≈ 10 次）、重新聆听 / 等响 / 监听提示三个 PillButton、CornerNote、ColourLegend（6 次）、静态底图整条带的贴图。估算每帧 2～6 ms（软件渲染器口径），45 Hz 下 90～270 ms/s = 单核 9～27 %；macOS 约为其 1/3～1/2。
- 修法（两步）：
  1. `ParticleField::onVBlank`：亮度 / GLAZE 没在变时不再 `repaint()`，改为 `repaint (0, 0, 500, bandHeight)`、`repaint (700, 0, 500, bandHeight)`、`repaint (500, 0, 200, 22)`、`repaint (500, 222, 200, 38)`（即波纹包围盒去掉主旋钮矩形 (500,322)-(700,522) 后的四块，组件坐标 y 已减 `top`）；`brightness` 还在追目标（流线不透明度跟它）或 `setGlaze` 改了值时才整条带重画。主旋钮矩形内 ≤ 4 px 的流线末端粒子（包络 ≤ 10 %）不再刷新，肉眼不可见。macOS peer 按脏矩形列表裁剪（`juce_NSViewComponentPeer_mac.mm:1880-1887`），Windows / Linux 同理。
  2. GlazeTitle、Segmented（风格 / A-B）、PillButton、CornerNote、ColourLegend、`monitoringButton` 各 `setBufferedToImage (true)`（缓存按物理像素生成，`juce_StandardCachedComponentImage.h:45-50`），它们只在自己 `repaint()` 时重排文字。
- 收益：主旋钮与文字部件在音乐播放时零重绘；每帧只剩两侧条带的粒子（B3：0.8～1.9 ms）+ 条带内静态底图贴图。估计省 2～5 ms / 帧。
- 验证：① 建一份 `JUCE_ENABLE_REPAINT_DEBUGGING=1` 的 Debug 版看闪烁区域；② `tools/ui-snapshot` 加 `--bench`：给每个子组件挂 `ComponentListener::componentPainted`（JUCE 9 `ComponentPaintDiagnostics::totalPaintDuration`，`juce_Component.cpp:1980-1989`），喂音乐 300 帧，打印每组件累计 paint 时长；改前改后对比，主旋钮在 GLAZE 不动时应为 0；③ Mac Activity Monitor 看插件进程消息线程占比（Max 手动）。

### R9-02 · 瓷面每 1 % GLAZE 步进都全量重算逐像素底层（4× 像素比 23 ms / 次），拖动期间按最大节流反复生成

- 严重度 medium · **P1** · 类别 热路径重复计算 / 可缓存中间量 · 工作量 M · 置信度 0.85 · changes_output false（像素级 ±1 LSB，音频无关）
- 文件：`Source/ui/Porcelain.cpp:400-463`（逐像素：3 次梯度噪声 + 2 次开方 + 3 次 lerp + 预乘量化）、`:477-488`（最多 392 条裂纹逐条 `strokePath`）；`Source/ui/GlazeKnob.cpp:86-96`（`needsRender`：比例差 > 0.004 或裂纹数变就重渲染）、`:129-156`（节流 `gap = max(60 ms, 3 × 上次耗时)`）、`:186-188`（首帧同步渲染）；`Source/ParameterRanges.h:58`（GLAZE 0～100 步 0.1）。
- 证据：`visibleCrackCount = round(392 × t^0.9)`（`Porcelain.cpp:370-374`）中段每 1 % 变 3～4 条 → 每一步都触发渲染；实验 1：全量底层 2.0 / 6.8 / 23 ms（1× / 2× / 4×）。拖一次旋钮（≈ 1 s，几十步）在 2× 下节流到约 16 次 / s × (6.8 + 裂纹 2～3) ms ≈ 15 % 单核，4× 下约 11 次 / s × 30 ms ≈ 33 %。
- 机理：底层颜色对 `depth = t^0.75` **是线性的**（`g0/g1/g2` 各是两色按 depth 的线性混合，再按半径 gt 线性插值，噪声项与 t 无关，`Porcelain.cpp:412-452`），所以 `base(t) = (1 − depth)·base0 + depth·base1`，两张基底图按像素比只需生成一次。
- 修法：
  1. `renderGlaze` 拆成 `renderBase (pixelScale, depth∈{0,1})` 缓存两张 ARGB（按像素比 keyed，静态或 GlazeKnob 成员），每次渲染改为：新建图 → `drawImageAt (base0)` → `setOpacity (depth); drawImageAt (base1)`（或逐像素定点混合，实验 1：0.17～2.65 ms）；
  2. 裂纹：`crackNetwork()` 生成时顺便用 `PathStrokeType (width, curved, rounded).createStrokedPath` 把每条裂纹的**描边轮廓**缓存成 `Path`（逻辑坐标、与像素比无关），渲染时 `fillPath (strokedPath, toImage)` 代替 `strokePath`（省掉每次的描边生成）；再进一步，t 增大时只在上一张图上追加新增裂纹（`visible` 单调），t 减小才从基底重画；
  3. 可选（L）：把整张瓷面渲染放到 `juce::ThreadPool` 后台线程，完成后在消息线程 swap。
- 收益：每次渲染 23 ms → 约 3～5 ms（4×）、6.8 → 1.5 ms（2×）；拖动期间消息线程占比降到原来的 1/4～1/5。
- 验证：① 离线对比新旧 `renderGlaze` 图像逐像素差 ≤ 1 LSB（可写成 ui-snapshot 里的检查）；② `GlazeKnob::lastRenderCostMs` 打 log；③ 拖动 GLAZE 时 Activity Monitor。

### R9-03 · 频谱仪高频「取最大 bin」使宽带材料显示偏高 +5～+7 dB（500 Hz → 10 kHz），平均线继承同一偏差

- 严重度 medium · **P1** · 类别 算法正确性（显示） · 工作量 M · 置信度 0.8 · changes_output false（只改显示，不改音频）
- 文件：`Source/ui/SpectrumAnalyser.cpp:178-206`（`:185-193` 低频按功率插值；`:194-202` 高频 `power = max(bins)`）、`:209-238`（显示曲线在 dB 域做非对称升落）、`:240-279`（平均线与峰值线都从显示曲线派生）；`SpectrumAnalyser.h:13`（设计口径「0 dBFS 正弦读 0 dB」即每 bin 口径）。
- 证据：实验 2b（真实动态稳态）白噪声相对 100 Hz：1 kHz +2.0、5 kHz +4.8、10 kHz +6.6、16 kHz +7.0 dB；粉噪声加 3 dB/oct 显示斜率后本应平直，实际 10～16 kHz +7.5～+7.8 dB。原因是 K 个瑞利分布 bin 取最大的期望值随 K 增大（K = 2.5 → 49），而 500 Hz 以下用插值（≈ 单 bin）。
- 机理：这是母带插件，用户靠这张图判断音色平衡与 Balance 的效果；+6 dB 的高频虚高会系统性地让人觉得高频过亮。作者注释选「取最大」是为保住谐波尖峰（实验 2 证实：换均值后正弦在 10 kHz 读低 12.6 dB），所以不能简单换均值。
- 修法（保住尖峰、平均线无偏）：
  1. 显示曲线（填充 / 轮廓）与峰值保持线维持「取最大」（尖峰可见）；
  2. **平均线**改为独立估计：每帧另算一条按功率**均值**聚合的谱 `meanPower[i]`（同一 FFT 结果，`:194-202` 里多累加一个 `sum / count`），在**功率域**做约 0.8 s 的一阶平滑（`avgPower += (meanPower − avgPower) × 0.04`），再转 dB、沿频率平滑（现有 `smooth`）。功率域平均消除 dB 域非对称动态在插值区的 +3～+5 dB 峰偏，白噪声平均线平直、粉噪声 −3 dB/oct；
  3. 峰值读数框（`SpectrumView.cpp:484-506`）继续用显示曲线；帮助文案 / 头文件注释写明「填充与峰值线保尖峰、平均线是无偏功率平均」。
- 验证（写进 `tests/SpectrumAnalyserTests.cpp`）：白噪声 150 帧 → `getAverageDb()` 在 100 Hz～16 kHz 内相对 1 kHz 的偏差 ≤ ±1.5 dB；粉噪声 → 斜率 −3 ± 0.7 dB/oct；−6 dBFS 10 kHz 正弦 → 峰值线 −6 ± 1.5 dB（现有行为不退化）。

### R9-04 · 频谱仪每帧 516 次裁剪竖纹填充 + 悬停时整面重画

- 严重度 medium · **P1** · 类别 热路径 / 重绘范围 · 工作量 M · 置信度 0.8 · changes_output false
- 文件：`Source/ui/SpectrumView.cpp:624-635`（`reduceClipRegion (fill)` 后 `for (x = 0; x < w; x += 2) fillRect (x, 0, 0.8, h)` 共 516 次，再一次整面渐变）、`:539-546`（圆角路径裁剪）、`:585-609` + `:613,638,640,648,656`（同一帧 5 次 `buildLine`，输出曲线建两遍）、`:751-762`（`mouseMove` / `mouseExit` 整面 `repaint()`）、`:712-744`（网格文字、dB 标签 6 个圆角标签、读数框每帧重画）。
- 证据：B2 竖纹 + 渐隐 7.1 ms @2× / 18.7 ms @4×；改平铺填充 3.3 / 8.0 ms；主填充 + 两条描边 2.7 / 9.2 ms。30 Hz 播放时整面重画，悬停时再叠加鼠标事件频率（60～120 Hz）的整面重画。
- 修法：
  1. 竖纹：按像素比生成一张 2 逻辑像素宽的瓦片（`Image` 宽 `round(2·pixelScale)`，画 0.8·pixelScale 宽白条），`g.setFillType (FillType (tile, AffineTransform::scale (1/pixelScale)))` 后 `fillPath (fill)`；渐隐改 `setGradientFill` + `fillPath (fill)`，去掉 `reduceClipRegion (fill)` 与 516 次 `fillRect`；
  2. 静态部分（面板渐变、网格、100 Hz/1 k/10 k 与 dB 标签、读数框底）缓存成按像素比的 `Image`，每帧 `drawImageAt` 后只画曲线、标记与读数文字；
  3. 悬停只 `repaint` 读数框矩形与新旧光标列（±6 px），不整面重画；
  4. `buildLine` 对 output 建一次路径，闭合版用 `Path` 拷贝再加两点。
- 收益：每帧约 −4 ms @2×（软件渲染口径）、−10 ms @4×；悬停时的尖峰基本消失。
- 验证：ui-snapshot `--bench` 按组件 paint 时长；截图逐像素对比（竖纹相位一致时应逐像素相同，允许抗锯齿 ±1 LSB）。

### R9-05 · 粒子每帧重画 400 × 400 径向渐变柔光

- 严重度 medium · **P1** · 类别 可缓存中间量 · 工作量 S · 置信度 0.75 · changes_output false
- 文件：`Source/ui/ParticleField.cpp:270-278`。
- 证据：B3 柔光单项 3.0 ms @2× / 12.0 ms @4×，而 200 个粒子 + 3 圈只要 0.8 / 1.9 ms。柔光只随 `glaze`（不透明度 `0.1 + 0.4·glaze`）变，形状固定。
- 修法：与 `lineImage` 同法缓存一张全不透明度的柔光图（`renderLines` 时一并生成，或单独 `glowImage`），每帧 `setOpacity (0.1 + 0.4f * glaze); drawImageTransformed (glowImage, …)`；配合 R9-01 后只在两侧条带相交的部分贴图。
- 收益：约 −2.5 ms @2×（软件口径）；macOS 上 CoreGraphics 的径向渐变同样按像素求值，收益同量级偏小。
- 验证：截图对比（渐变查找表量化，±1 LSB）；`--bench`。

### R9-06 · 粒子动画上限 45 帧 / s，规格写的是「界面刷新 30 Hz」

- 严重度 low · **P1** · 类别 与规格不一致 / 浪费 · 工作量 S · 置信度 0.7 · changes_output false
- 文件：`Source/ui/ParticleField.cpp:38`（`maxFrameRate = 45.0`）、`:225`；`ParticleField.h:14`；规格 `设计规格-界面-V4融合版.md` 第 1 节「界面刷新 30 Hz」。
- 机理：1～3 px 的慢速粒子 30 与 45 帧看不出差别；其它一切（表、频谱、读数）都是 30 Hz。粒子带每帧的全部开销（R9-01/05 的余量）按 45/30 多付三分之一。
- 修法：`maxFrameRate = 30.0`（VBlank 60 Hz 时隔帧重画）。若 Max 要保留 45，则在规格第 1 节补一句。
- 验证：`--bench` 帧计数；肉眼。

### R9-07 · 瓷面贴图落在非整数设备像素与 ≠ 1 的比例上，走重采样慢路径

- 严重度 medium · **P1** · 类别 JUCE API 用法 · 工作量 S · 置信度 0.7 · changes_output false
- 文件：`Source/ui/GlazeKnob.cpp:164-170`（`scale (1/pixelsPerUnit).translated (22, 22)`）；`Source/ui/Porcelain.cpp:404-405`（`size = round(180·scale)`，`pixelsPerUnit = size/180`）。
- 证据：上下文缩放 × (1/pixelsPerUnit) = pixelScale·180/round(180·pixelScale) ≠ 1（例如 1.786 → 1.00125），且平移 22 逻辑像素落在小数设备坐标上。JUCE 只对纯平移且偏移在 ±1/8 px 内的贴图走整块拷贝（`juce_RenderingHelpers.h:1882-1888`），否则逐像素重采样：B4 1.22 vs 0.25 ms（1.79×）、4.71 vs 0.98 ms（3.57×）。macOS `CGContextDrawImage` 同样在非整数变换时插值。
- 修法：`drawGlaze` 里 `ScopedSaveState`；`g.addTransform (AffineTransform::scale (1.0f / pixelScale))` 把坐标系换回设备像素；`g.setImageResamplingQuality (Graphics::lowResamplingQuality)`（强制整块拷贝路径，≤ 0.5 px 位置误差在柔边瓷面上不可见）；`drawImageAt (image, roundToInt (dx), roundToInt (dy))`，其中 `dx = (centreXY − imageHalf) × pixelScale`。淡入淡出两张叠画同样处理。
- 收益：每次主旋钮重画 −1～−3.7 ms；与 R9-01 叠加后只在 GLAZE 变化时付出。
- 验证：截图对比（允许 ≤ 0.5 px 平移）；`--bench`。

### R9-12 · 编辑器定时器每帧的无谓字符串与排版工作

- 严重度 low · **P2** · 类别 热路径重复计算 · 工作量 S · 置信度 0.8 · changes_output false
- 文件与证据：
  - `Source/PluginEditor.cpp:1151` 每帧 `layoutBottomRow()` → `:344-347` 四个 `getNaturalWidth()` ≈ 10 次 `getStringWidth`（实验 4：6～25 µs / 次，中文更贵）；
  - `:1002` 每帧 `advancedPanel.setStatus`（`Popovers.cpp:526-539`：4 次 `String(float,1)`、2 次 `tr`、27 次取整）——面板不可见时也做；
  - `:1234-1236` 每帧 `TopBar::setLatency` 拼一次字符串（`TopBar.cpp:297`）；
  - `:1183 / :1213` 每帧 `segmentsFor`（`GlazeStatusText.cpp:303-327` 拼 StringArray）；头文件承诺的「View 没变就不重拼」在编辑器里没落实；
  - `:1079-1081` → `Controls.cpp:313-330` 每帧 7 个旋钮各拼一次 `autoAmountText`；
  - `SpectrumView.cpp:92-107` 每帧拼 5 段 `String(float,1)` 当比较键；
  - `Localisation.cpp:267-286` `tr()` 每次 UTF-8 解码 + 分配。
- 修法：`layoutBottomRow` 只在 `setText` / `setLanguage` 改变文字时调（让 `PillButton::setText` 返回是否变化，或在 timerCallback 里比较文本）；`setStatus` 在 `advancedPanel.isVisible()` 为假时只存状态、显示时刷新；`setLatency` 先比 double 再拼字符串；`segmentsFor` 以 `view` 相等缓存；`LoudnessHud::setValues` 比较量化到 0.1 的整数；`tr()` 返回静态表里预先解码的 `juce::String`（表构造一次）。
- 收益：约 0.3～0.6 ms / 帧、600～900 次 / s 的堆分配归零。
- 验证：代码评审 + `--bench` 总时长；`LocalisationTests` 不变。

### R9-10 · 频谱仪每帧重算几千次 pow / log

- 严重度 low · **P2** · 类别 可缓存中间量 · 工作量 S · 置信度 0.8 · changes_output false
- 文件：`Source/ui/SpectrumAnalyser.cpp:52-56`（`frequencyAt` 用 `pow`）、`:174-182`（每点两次 `pow(2, ±halfStep)` 都是常数）、`:205`（每点 `log10`）；`Source/ui/SpectrumView.cpp:444-447`（`tiltAt` 用 `log2`）、`:594`（5 条曲线各 480 点每点 `frequencyAt` + `tiltAt`）、`:489-496`。
- 证据：每帧 2 路 × 480 × 3 次 `pow` + 5 × 480 × (`pow` + `log2`) ≈ 7.7 k 次超越函数 ≈ 0.15～0.3 ms。
- 修法：`setSampleRate` 时预算每点的 `[first, last]` bin 区间与插值位置（`std::vector<PointRange>`）；`SpectrumView` 在斜率或语言变化时预算 `tilt[i]` 与 `x[i]`；`buildLine` 只做加法。
- 验证：`SpectrumAnalyserTests` 现有用例逐位不变（先存改前的 480 点结果，再断言相等）。

### R9-13 · 状态胶囊呼吸动画每 33 ms 重画整个胶囊

- 严重度 low · **P2** · 类别 重绘范围 · 工作量 S · 置信度 0.8 · changes_output false
- 文件：`Source/ui/TopBar.cpp:93-98`（timer → `repaint()`）、`:115-174`（每次 paint 6～8 次文本宽度 / 绘制）。
- 修法：呼吸只改圆点：`repaint (dotBounds.expanded (4))`；或把文字部分 `setBufferedToImage`。聆听 / 校准中（可达 15 s+）每秒省约 200 次文本整形。
- 验证：`--bench`。

### R9-11 · 96 k / 192 k 下界面 FFT 长度翻到 16 k / 32 k，Windows 默认 FFT 路径每帧 1.4～3.5 ms

- 严重度 low · **P2** · 类别 采样率依赖 / 优化 · 工作量 M · 置信度 0.75 · changes_output false
- 文件：`Source/ui/SpectrumAnalyser.cpp:66-79`（≤ 50 k 用 8192，≤ 100 k 用 16384，其余 32768）、`:160-170`（每帧两路各一次全长加窗拷贝 + FFT）。
- 证据：实验 5，JUCE 回退引擎 32768 点 1.73 ms / 次 → 两路 × 30 Hz = 单核 10.4 %（192 k）、4.3 %（96 k）；macOS vDSP 快 5～10 倍，不受影响。
- 修法：取样口之后先按 2 / 4 抽取到 ≤ 50 kHz（界面线程做一个 8 阶半带 FIR，20 kHz 以上本就不显示），FFT 固定 8192；或至少把上限钉在 16384 并接受 192 k 时 85 ms 的窗。
- 验证：`SpectrumAnalyserTests` 现有 96 k 用例 + 新增 192 k 用例（正弦读数 ±1 dB、泄漏 < −60 dB）。

### R9-08 · 静态底图每次重生成约 200～350 ms（软件渲染口径）阻塞消息线程

- 严重度 low · **P2** · 类别 优化 · 工作量 M · 置信度 0.7 · changes_output false
- 文件：`Source/PluginEditor.cpp:488-571`（`:520-525` 5000 个 1.6 px 圆、`:528-542` 逐像素颗粒、`:547-553` 7 个 `DropShadow`）、`:403-404`（首帧同步生成）、`:457-466`（缩放稳定 150 ms 后重生成）。
- 证据：B6 5000 圆 19 ms、单个频谱面板阴影 38 ms（7 个 ≈ 100+ ms）、颗粒循环 44 ms（实验 1）。打开编辑器与每次拖角缩放停下都付一次。
- 修法：阴影按逻辑分辨率只生成一次、缓存成 7 张小图（`DropShadow` 本就在逻辑分辨率渲染再放大，`juce_DropShadowEffect.cpp`）；颗粒用一张 256 × 256 随机瓦片 `setTiledImageFill` 低透明度铺满（视觉等价）；点阵可保留（瓦片填充只省 5 ms）。
- 验证：`Time::getMillisecondCounterHiRes` 包住 `renderStaticLayer` 打 log；截图对比（颗粒瓦片周期 256 px 不可见）。

### R9-14 · 听原声时主旋钮每次 paint 开一层透明图层

- 严重度 low · **P2** · 类别 JUCE API 用法 · 工作量 S · 置信度 0.7 · changes_output false
- 文件：`Source/ui/GlazeKnob.cpp:194-195`、`:275-276`（`beginTransparencyLayer (0.55)` … `endTransparencyLayer`）。
- 机理：每次 paint 分配并合成一张裁剪区大小（400²～800²）的 ARGB 图层；瓷面已经在 `render` 时去色（`:107`），55 % 只需在各元素上乘：`drawGlaze (…, opacity × 0.55)`、颜色 `withMultipliedAlpha (0.55)`。
- 收益：B 状态下每次主旋钮重画 −0.5～−2 ms；与 R9-01 叠加后只影响拖动。
- 验证：截图对比（α 乘法与图层合成在 8 位下 ±1 LSB）。

### R9-09 · 静态底图封顶 2× 像素比，高于 2× 时每次重画都重采样整块背景

- 严重度 low · **P2** · 类别 优化 · 工作量 S · 置信度 0.6 · changes_output false
- 文件：`Source/PluginEditor.cpp:25`（`maxStaticLayerScale = 2.0f`）、`:394`、`:406-408`（`drawImageTransformed`）。
- 机理：200 % × Retina 时上下文 4×、底图 2× → 每个脏矩形的背景贴图走 2 倍双线性重采样（B4 的 4～5 倍差）；120 % 等非整数缩放同理（净比例 ≠ 1）。作者封顶是怕 4× 底图 60 MB。
- 修法：先做 R9-01（脏区变小，代价按面积缩）；再给这一处 `setImageResamplingQuality (lowResamplingQuality)`（背景是柔光 + 点阵 + 颗粒，最近邻放大不可见），走整块拷贝路径。
- 验证：`--bench` 在 `--scale 4` 下比较 Canvas 的 paint 时长。

### R9-15 · 无无障碍值接口与键盘操作

- 严重度 low · **P2** · 类别 设计缺口（无障碍 / 键盘） · 工作量 L · 置信度 0.9 · changes_output false
- 文件：`Source/ui/Controls.h:89-154`（`ParameterControl` 没有 `createAccessibilityHandler` / `keyPressed` / `setWantsKeyboardFocus`）；全 `Source/ui/**` 与 `PluginEditor.cpp` grep 不到 `AccessibilityHandler`、`setTitle`、`setDescription`、`keyPressed`；JUCE 默认给每个组件一个 `AccessibilityRole::unspecified` 的处理器（`juce_Component.cpp:3307-3310`）→ VoiceOver / Narrator 只见到一堆无名无值的元素；Tab / 方向键完全不可用。
- 修法：`ParameterControl::createAccessibilityHandler` 返回 `AccessibilityHandler (*this, AccessibilityRole::slider, AccessibilityActions{}, AccessibilityHandler::Interfaces { std::make_unique<ValueInterface>() })`，`ValueInterface` 用 `parameter.getText / getValueForText / setValueAsCompleteGesture`；`setTitle (name)`、`setHelpText (help::tooltipFor (…))`；`setWantsKeyboardFocus (true)`，`keyPressed` 处理 ↑↓←→（一步 = 参数 interval 或 1 %，⌘ / Ctrl 精调）、Home/End、Return 打开输入框；Segmented / PowerButton / ChipToggle 同法（`toggleButton` / `radioButton` 角色）。
- 验证：`tools/ui-snapshot` 加检查：每个带参数 ID 的组件 `getAccessibilityHandler()->getValueInterface() != nullptr` 且 `getTitle()` 非空；Max 用 VoiceOver 走一遍。

## 4. 核过没有问题的（不列结论）

- 参数附着与手势：所有可调控件经 `juce::ParameterAttachment`，拖动 = begin / set… / end 一次手势，双击 / 滚轮 / 输入 = `setValueAsCompleteGesture`（`Controls.cpp:170-294`）；双击在手势中回默认走 `setValueAsPartOfGesture` 且随后 `mouseUp` 收尾（`:251-270`）；`SegmentedAttachment` / `PowerButton` / `ChipToggle` 的析构顺序都在控件之前（成员声明顺序）；`ParameterAttachment` 的音频线程回调经 AsyncUpdater 落到消息线程。
- 音频 ↔ 界面交换：`AnalyzerTap` 单生产者单消费者 `AbstractFifo`，满了丢新样本、不分配不等待（`AnalyzerTap.h:47-99`）；GLAZE 状态用逐字原子的单写者序列锁（`GlazeController.cpp:515-562`），参考谱同法（`ReferenceSpectrum.h:57-70`）；Balance 27 段与目标修正各是原子量；`GlazeDeltaForUi` 7 个原子量。没有锁、没有在音频线程的系统调用。
- 悬垂与泄漏：`bubbleOwner` 只比较不解引用；`entryCommit` 捕获 `SafePointer`；`CornerDragListener` 在析构中移除；`Canvas` 析构先置 `destroying` 再停定时器；`SharedResourcePointer<UiSettings>` 引用计数；`crackNetwork` 静态一次。
- 缩放往返：`uiZoom` 在根节点，`valueTreeRedirected` 与 `writingZoom` 守卫正确；「为放进屏幕缩小的那一次不写回」成立；旧 `uiScale` 不读。
- 线程 / 时序：延迟上报在消息线程；`keyStateChanged` 只在输入框可见时吞按键抬起；`TooltipWindow` 作为画布子组件随画布缩放，气泡不糊。
- 线程安全的小瑕疵但不构成缺陷：`getMasteringStatus` 由多个原子量拼成、不是一致快照；`getGlazeEffectiveParamsForUi` 的 7 个差量可能来自相邻两块——都只影响一帧显示。

## 5. 已知项与本报告的对应

| 档案里的已知项 | 本报告 |
|---|---|
| 拖动 GLAZE 时界面主线程约 21 % | R9-02（瓷面全量重算，实测 2× 6.8 ms / 4× 23 ms 每次）+ R9-07（非整数贴图）是机理；修完预计降到 1/4～1/5 |
| 「打开就高温」（待 Max 给数字） | R9-01 / R9-05 / R9-06 / R9-04 是播放时的稳态构成；ui-snapshot `--bench` 可给出组件级数字 |

## 6. 附

- 实验脚本：scratchpad `r9/porcelain_bench.cpp`、`spectrum_bias.py`、`spectrum_bias2.py`、`gfx_bench.cpp`、`text_bench.cpp`、`fft_bench.cpp`（JUCE 微基准编译参数：`-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_FONTCONFIG=0`，链接 freetype2 / pthread / dl / rt）。107 上的 `~/.cache/celadon-t032/R9/` 已删除。
- 未做：macOS CoreGraphics 口径的实测（本机禁编）；中文文本整形的实测（107 无中文字体）。
