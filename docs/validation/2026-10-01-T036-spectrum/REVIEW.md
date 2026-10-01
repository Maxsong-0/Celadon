# T-036 频谱仪改动独立评审（2026-10-01）

> 评审对象：`git diff 88ff5b6..577ded0`（分支 `claude/plugin-spectrum-optimization-789521`，10 个文件）。只读评审，没改任何源码。
> 方法：逐行审读 + 对照 JUCE 9.0.2 源码（`juce_VBlankAttachment`、`NSViewComponentPeer` / `PerScreenDisplayLinks`、
> `juce_VBlank_windows.cpp`、`CoreGraphicsContext::drawImage`、`Direct2DPixelData`、`StandardCachedComponentImage`）+ 107 上实测
> （本机未编译、未跑任何测试）。探针源码不进仓库，放在 107 的 `~/.cache/celadon-review-t036/probe/`（见文末「附」）。

## 汇总

| 编号 | 严重度 | 一句话 |
|---|---|---|
| R-01 | 重要 | 图例 / 斜率 / 响度小窗用了 JUCE 的 `setBufferedToImage`，小数像素比下每帧双线性重采样、字发糊——T-032 A9 明确弃用过它 |
| R-02 | 次要 | 曲线静止时换斜率，路径在 paint 里重建、`curveArea` 没跟上；紧接着 OUT 清零（A / B、采样率变）会留残影（Windows / Linux） |
| R-03 | 次要 | 悬停时读数文字每帧变 → 读数框缓存图每帧重新分配并重画（Direct2D 上还每次回读显存）；macOS 的包围盒因此每帧是整块面板 |
| R-04 | 次要 | 没有帧率上限：144 / 165 / 240 Hz 屏按刷新率跑；ADR-025「退路是封顶 60 帧（改一个常数）」在代码里不存在 |
| R-05 | 建议 | 窗口被挡住时垂直同步照来（mac 按屏、Windows 按显示器），频谱仍按 120 帧算 FFT、建路径；注释「窗口被挡住时由定时器推」不成立 |
| R-06 | 建议 | `advanceForSnapshot` 的确定性靠「advanceSpectrum 不超过 15 ms」这一墙钟条件，慢机 / Debug 下会多推一帧 |
| R-07 | 建议 | 每帧的小分配与可省的绘制（含实测收益） |
| R-08 | 建议 | 测试没直接守住「插值不低于两端较小者」与「子组件缓存对齐」两条关键性质 |
| R-09 | 建议 | 注释 / 档案与代码不一致的几处 |

---

## R-01 重要：图例 / 斜率 / 响度小窗的组件缓存用错了类型，小数像素比下发糊

- 位置：`Source/ui/SpectrumView.cpp:473-476`（`child->setBufferedToImage (true)`），说明在 `Source/ui/SpectrumView.h:33-35`。
- 问题：JUCE 的 `StandardCachedComponentImage::paint`（`juce_StandardCachedComponentImage.h`）把缓存图按 `compBounds * scale` 取整生成，
  贴回时 `drawImageTransformed (scale (comp / image))`，用父上下文的默认插值、平移带小数——界面缩放不是 100 %、或 Windows 125 / 150 %
  时每帧双线性重采样，字与细线糊半个像素，且图尺寸取整后宽高比例还有微小拉伸。另外它只按图尺寸判断要不要重生成，像素比变了但取整后
  尺寸不变（缩放滑杆的细步进）会贴旧比例的图。T-032 已为同一问题专门写了 `AlignedComponentCache` / `setBufferedToAlignedImage`
  （`Controls.h:111-134`），`IMPL-A9-ui-band.md` 2.1 节与 `GlazeKnob.h:17` 都写明「不用 setBufferedToImage」，画布里 8 个文字小部件
  （`PluginEditor.cpp:303-305`）也是这么挂的；T-036 在频谱仪里又用回了 JUCE 自带的那个。
- 失败场景：Windows 笔记本 1920 × 1080 @ 150 %（`fitZoomToScreen` 把窗口缩到约 78 %，像素比 ≈ 1.17）、或用户把窗口拖到 70 % / 115 %、
  或 mac 上非 100 % 缩放 → 响度小窗的大号 LUFS 数字、三个图例开关、斜率选择的字变虚。
- 107 实测（软件渲染器，`review_ui_probe sharp`，同一状态下三种挂法各出一帧，与「直接画」比边缘能量 = 相邻像素亮度差平方和）：
  整数像素比（1、2、1.5 × 2）三者相同；小数像素比下 JUCE 标准缓存的边缘能量只剩直接画的 **0.30 ～ 0.78**（例：100 % × 1.25 时斜率选择
  0.42、响度小窗 0.76；70 % × 2 时响度小窗 0.54），`AlignedComponentCache` 为 **0.81 ～ 1.26**（多数 0.95 ～ 1.05，差异来自 ≤ 0.5 px 的整体吸附）。
- 建议修法：把那一行换成 `setBufferedToAlignedImage (*child);`（`SpectrumView.h` 已 include `Controls.h`），注释同步改。
  可在截图工具 `--only check` 里照 ⑭ 加一条：频谱仪的 5 个子组件挂的是 `AlignedComponentCache`。
- 把握：确认。已在 107 实测。

## R-02 次要：曲线静止时换斜率，之后 OUT 被瞬间清零，脏区漏掉旧峰顶

- 位置：`Source/ui/SpectrumView.cpp:468`（`slopeControl.onChange` 只 `updateTilt` + 整面 `repaint`）、`:1094-1095`（paint 里重建路径）、
  `:611-617`（`repaintCurves` 用上一次 `setSpectrum` 记下的 `curveArea` 当「旧区域」）。
- 问题：播放中换斜率没事——下一个垂直同步里 `setSpectrum` 先于 paint 跑，`curveArea` 已换成新斜率下的区域。但曲线静止时（`differs`
  全不超过 0.02 dB，`setSpectrum` 直接 return）换斜率，新斜率下的路径是在 paint 里建的，`curveArea` 仍是旧斜率的区域；若下一次变化是
  「最高的曲线瞬间消失」（`restartOutput`：A / B、试听差值、发行预览；采样率变了 `reset()`），新区域很小、旧区域又低于屏幕上真实画着的
  峰顶，峰顶那一截不在任何脏矩形里。
- 失败场景（107 复现，`review_ui_probe dirty` 的 S8）：GLAZE 0、峰值线关、6 kHz −12 dBFS 稳态正弦 12 s（最后 1 s 0 帧重画，确认静止）
  → 斜率 0 → 4.5（6 kHz 抬高约 11.6 dB）→ 切到 B → 下一帧增量图与整帧重画在 6 kHz 峰顶处差 **44 px（×1，最大 79 LSB）/ 86 px（×2）**，
  即一截 2 px 宽、23 px 高的旧峰尖留在屏上，直到之后某帧的区域再盖到那里。矩形列表口径（Windows Direct2D、Linux）会出现；
  包围盒口径（macOS）这一帧碰巧被别的脏矩形盖住、没出现。其余 8 个场景（播放、悬停进出移动、峰值线关时播放中换斜率、静止后换斜率再变轻、
  静音中峰值线开、IN / OUT 开关 + A / B + 换语言、渐弱到底、静止后换斜率再静音）× 3 种口径（×1 矩形列表、×1 包围盒、×2 矩形列表）
  共 3 600+ 次比较 0 残影。
- 建议修法（两条都很小）：① paint 里 `if (! curvePathsValid) { buildCurvePaths(); curveArea = curveArea.getUnion (curveRegion()); }`
  ——凡在 paint 里重建过路径，就把它画出的区域并进「旧区域」；② `curveRegion()` 不论 PEAK / IN 开关都并上峰值线与（有 IN 时）IN 线，
  这样开关切换那一帧也不靠描边外扩的 3 px 余量兜底（峰值线一帧落得多于 3 px 只在看门狗 dt 很大时出现，眼下没复现）。
- 把握：确认（触发条件少见：要先有静止曲线，多见于用测试音调斜率）。已在 107 实测。

## R-03 次要：悬停时读数框缓存图每帧重建；macOS 上悬停 = 每帧整块面板重画

- 位置：`Source/ui/SpectrumView.cpp:1086-1090`（文字一变就 `invalidate (Layer::readout)`）、`:1025`（`prepareLayers` 每次 `new juce::Image`）、
  `:619-623`（每帧 `repaint (cursorBounds())` 与读数框）。
- 问题：悬停时读数是光标处 OUT 的 dB（两位小数），播放中几乎每帧都变 → 每帧：新分配一张读数框图（×2 约 0.13 MB）、重画影子 / 白底 /
  描边 / 字。T-032 时文字是直接画的，只有框是缓存。Windows 上 `Direct2DPixelData` 默认开备份（`sync = true`），每画一次图在
  `Context::~Context` 里 `createPersistentBackup` 回读显存——每帧一次 GPU 同步（`CachedLayer` 都没调 `setBackupEnabled (false)`，
  `AlignedComponentCache` 调了）。macOS 只拿脏区包围盒：读数框在面板左上、光标列满高，悬停时包围盒每帧都是整块面板。
- 107 实测（软件渲染器，`review_ui_probe cost`，120 帧放音乐，增量重画；此口径里每帧还含 30 Hz 定时器那一份读数的重画，绝对值偏大）：
  悬停比不悬停每帧多 **0.35 ms（×1）/ 0.54 ms（×2）**；频谱仪内脏区包围盒平均占面板 **66 % → 100 %**。Direct2D 回读的代价没法在 107 量。
- 建议修法：① 悬停读数的 dB 数值节流（与「最响的峰」同一 8 / 30 s 节奏，或 ≤ 15 Hz）——两位小数每秒变 120 次本来也读不清；
  ② `prepareLayers` 尺寸不变时复用图（清成透明再画），所有 `CachedLayer` 图 `setBackupEnabled (false)`（内容随时能重画，同 `AlignedComponentCache`）；
  或退回 T-032 的做法：框进缓存、字每次直接画。
- 把握：分配与包围盒确认；Direct2D 回读很可能（按 JUCE 源码推断，未在 Windows 实测）。部分在 107 实测。

## R-04 次要：没有帧率上限；ADR 写的「改一个常数退回 60 帧」并不存在

- 位置：`Source/PluginEditor.cpp:876-889`（每个垂直同步都 `advanceSpectrum`）、`:22-27`（常数里没有帧率）；
  档案 `03-记忆/ADR-025` 后果第 1 条「退路是封顶 60 帧（改一个常数）」。
- 问题：Max 拍板的是「120 Hz 屏 120 帧、CPU 约为 60 帧两倍」。mac 外接 144 / 165 Hz、Windows 144 / 240 Hz 屏上 `CVDisplayLink` /
  `WaitForVBlank` 按屏幕刷新率回调，FFT、建路径、重画都跟着翻到 2.4 ～ 4 倍（相对 60 帧），没有任何封顶；ADR 的退路要写新逻辑才有。
- 107 实测：`push + update` 每帧 **0.195 ms（只 OUT）/ 0.369 ms（OUT + IN）**（48 k，JUCE 回退 FFT，Windows 同为回退 FFT）；
  `--only spectrum-bench` 整面频谱 paint ×2 每帧 3.92 ms → 120 帧 470 ms/s、按 240 帧推算约 940 ms/s（软件渲染器口径）。
- 建议修法：加 `constexpr double maxSpectrumFps = 120.0;`，`spectrumFrame` 里在更新 `lastVBlankMs` 之后、若上一次也是垂直同步且
  `timestampSeconds - lastVBlankSeconds < 0.9 / maxSpectrumFps` 就 return（不动 `lastVBlankSeconds`，时长自然累到下一帧）。
  这样 ADR 的「改一个常数」成立；也可让「减弱动效」开着时取 60（或 30），给怕发热的用户一个开关。
- 把握：确认（代码事实）；高刷屏实际 CPU 未实测。部分在 107 实测。

## R-05 建议：窗口被挡住 / 最小化时仍按刷新率算

- 位置：`Source/PluginEditor.cpp:876-912`；注释 `PluginEditor.h:119-120`、`PluginEditor.cpp:1198`「垂直同步……没来（窗口被挡住……）」。
- 问题：JUCE 9 的 mac 垂直同步是按屏的 `CVDisplayLink`，只按「窗口所在的屏」过滤（`NSViewComponentPeer` 的 `AsyncRepainter`），
  窗口被别的窗口挡住照样每帧回调；Windows 是按显示器的 `WaitForVBlank` 线程，同样不管遮挡。于是插件窗口开着但压在宿主后面时，频谱仍按
  120 帧做 FFT、建路径、发重画请求（原来是 30 Hz）。
- 建议修法：`advanceSpectrum` 开头 `if (! canvas->spectrum.isShowing())` 时只把取样口抽干、不 push / update / setSpectrum（最小化时
  `isShowing()` 为假）；遮挡要更进一步得在 mac 上看 `NSWindow.occlusionState`，可列待办。注释里「窗口被挡住」改成「最小化、离屏、宿主不给」。
- 把握：很可能（按 JUCE 源码；遮挡下的实际 CPU 未在 Mac 实测）。未实测。

## R-06 建议：截图工具的确定性依赖墙钟里的 15 ms

- 位置：`Source/PluginEditor.cpp:1117-1124` 与 `:1199`。
- 问题：`advanceForSnapshot` 先 `advanceSpectrum (seconds, now)`，再 `timerCallback()`；后者「不再推」靠 `now - lastSpectrumAdvanceMs < 15 ms`，
  而这段时间里包含整个 `advanceSpectrum`（两路 FFT、建路径）。慢机、Debug 构建或负载高时超过 15 ms，看门狗就按墙钟再推一帧，
  `--deterministic` 两次出图不再相同。
- 107 实测：Release 下 `--deterministic --only zh-default,zh-zoom70` 跑两次，PNG 逐字节相同（`advance` 平均 0.23 ms，离 15 ms 很远）。
- 建议修法：`advanceForSnapshot` 里先 `lastVBlankMs = juce::Time::getMillisecondCounterHiRes();`（让看门狗认为垂直同步还活着），
  或加一个 `spectrumDrivenExternally` 标志让 `timerCallback` 跳过推频谱。
- 把握：确认（逻辑），眼下环境不触发。已在 107 实测。

## R-07 建议：每帧的小分配与能省的绘制（附 107 实测）

- 每帧堆分配（播放中 `changed` 每帧为真）：`SpectrumView.cpp:753` `outputFill = outputLine`（`juce::Path` 拷贝赋值 = 新分配 + 释放，约 12 KB）；
  `readoutText()` 每帧在 `repaintCurves` 与 `paint` 里各拼一次串（十几个 `juce::String` 临时量）；`paint` 里 `ColourGradient` 渐隐（原来就有）；
  IN 开着时 `:1109` `createDashedStroke` 每帧生成一条新路径。建议：`outputFill` 用 `buildLine (output, outputFill)` 直接建再闭合
  （`clear()` 保留容量）；读数串按（悬停频率 / 峰频率、四舍五入后的 dB）做键缓存；IN 虚线改为自己按 3 / 2 px 切出中线子路径再 `strokePath`。
  分析器侧每帧无分配（`vector` 同尺寸赋值复用容量），没有 O(N²)。
- 绘制各图元的代价（`review_ui_probe prims`，软件渲染器，1033 点，ms / 帧，×1 / ×2）：OUT 平铺行图填充 0.73 / 2.63（纯色参照 0.43 / 1.48）、
  渐隐渐变 0.54 / 1.90、OUT 轮廓 0.31 / 0.49、平均线（`curved` + `rounded`，`:1165`）0.43 / 0.59、峰值线 0.17 / 0.31、IN 虚线 0.44 / 0.66。
  平均线改 `beveled` + `butt` 为 0.23 / 0.41——1.4 px 的线、相邻点 2 px，接头样式肉眼难辨，每帧省约 0.2 ms（120 帧约 20 ～ 24 ms/s）。
  两次 OUT 填充占大头；渐隐在 y < 40 处全透明，可先 `reduceClipRegion` 到 y ≥ 40 再填（曲线很响时省十几 %，量小）。
- `differs`（`:559-569`）按未加斜率的 dB 比较：面板底边以下（加斜率后 < −84 dB）的起伏也会触发重建与重画。按加斜率、钳位后的像素 y
  （阈值约 0.1 px）比，安静段落能整帧跳过。收益只在很安静 / 底噪段，量小。
- 把握：确认。图元代价在 107 实测（Mac CoreGraphics 的相对比例可能不同）。

## R-08 建议：测试没直接守住两条关键性质

- 位置：`tests/SpectrumAnalyserTests.cpp:235-272`。
- 问题：「不过冲、单调」那条用正好落在 bin 上的正弦，峰 bin 两侧对称、斜率本来就是 0；检查范围 7.2 ～ 10.8 bin，不含零点两侧的段——
  把局部最高 bin 以外也用不限幅的 Catmull-Rom（ADR 说会「冲到 0 以下画出假深缺口」）这条也照样过。现在只有宽带摘要那条
  「每个点都高于底线 20 dB」间接挡着。视图层（R-01、R-02 那类）没有自动检查。
- 建议：加一条性质测试——随机信号（多正弦、强弱相邻正弦、噪声、脉冲、直流）× 各采样率，逐点核「插值 ≥ min (p_b, p_b+1)、
  隆起 ≤ 0.65 dB、下标在 `binMagnitude` 内」（要一个只给测试用的取 bin 幅度 / 斜率的口子，或把插值抽成可单测的静态函数）；
  本评审探针 A1 就是这么做的（8 个采样率 × 400 个信号，0 违例、最大隆起 0.60 dB）。截图工具 `--only check` 加 R-01 的缓存类型检查，
  并可把 B1 的「增量重画 vs 整帧」做成一条回归检查（S8 场景即可抓到 R-02）。
- 把握：确认。

## R-09 建议：注释 / 档案与代码不一致

- `Source/ui/SpectrumView.h:17-18`（T-032 段）仍写「dB 底框……与读数文字每次照画」，T-036 起两者都进了缓存（T-036 段写了，但前段没改）。
- `Source/AnalyzerTap.h:33-34`「垂直同步停了时 30 Hz 定时器接着取，33 ms」：切换时最长间隔 = 100 ms 超时 + 一个定时器周期 ≈ 133 ms
  （192 kHz 下 2.56 万样本，仍在 32768 容量内，结论不变，只是数字不对）。
- `PluginEditor.h:119-120`、`PluginEditor.cpp:1198`「窗口被挡住」见 R-05。
- 档案 `04-协调/2026-10-01-T-036-频谱仪顺滑与响应计划.md` 方案第 3、4 条（bin 先换 dB、Steffen 单调插值；每 4 像素一处色标的横向渐变）
  与实现不同（幅度上的保形三次 + Catmull-Rom 峰；每逻辑像素 ±2 px 取色的一行图）。ADR-025 已按实现写，计划文件建议加一句「实施时改为……见 ADR-025」。
- ADR-025「改一个常数」见 R-04。

---

## 查过、没问题

- **插值保形**（探针 A1，Release 与 ASan / UBSan 各一遍）：44.1 / 48 / 88.2 / 96 / 176.4 / 192 / 32 / 22.05 kHz × 400 个随机信号（含强弱相邻正弦、
  正弦 + 直流、脉冲），0 个点低于两端较小者，最大隆起 0.58 ～ 0.60 dB，下标 0 越界（含 32 / 22.05 kHz 时 20 kHz 超过奈奎斯特、
  192 kHz 低频用 bin 1 ～ 2），测量值与按公式重算逐位一致。手推：局部最高 bin 的 Catmull-Rom 斜率 |m| < 0.5·(p_b − 较低邻居)，
  相邻段下界恒 ≥ 0；单调段 Fritsch–Carlson 3 倍限幅成立。
- **峰顶读数**（A10）：0 dBFS 正弦在 bin 9 ～ 11 间以 0.005 bin 滑动，屏上峰顶 −0.93 ～ 0 dB（Hann 扇贝损失原为 1.42 dB），相邻步最大跳 0.023 dB
  （在 9.5 bin 两 bin 相等的平台处），没有可见跳变。
- **动态边角**：dt = 0（有新样本时测量更新、显示不动）、NaN、±inf、负数、> 0.25 s 钳位均正确（A2 + 现有单测）；1e20 的巨大有限样本一窗后
  不留 NaN / inf（A7）；采样率中途变化 × `restartOutput` × IN 开关交错 60 轮 ASan / UBSan 干净（A11）；IN 没人取时 RMS 累计到 2^24 重起、
  不溢出（A12）；峰值保持在 30 / 60 / 120 / 144 / 240 帧下 1.50 ～ 1.53 s，与帧率无关（A13）。`restartOutput` 期间平均 / 峰值线留底线、
  攒满一窗从显示曲线起步；IN 关 → 开在下一次有 IN 新样本时取测量值，宿主停着时不出半截曲线——逻辑与单测一致。
- **生命周期**：`VBlankAttachment` 移动赋值先 `cleanup` 再接管，析构里 `= {}` 先于 `canvas.reset()`；对等体换 / 删用 `isValidPeer` 防悬空；
  编辑器构造时没有对等体、加到桌面后经 `componentParentHierarchyChanged` 接上（`internalHierarchyChanged` 递归到孙组件）。
  mac 的 `AsyncRepainter` 合并同屏事件时保留较早时间戳、Windows 保留最新时间戳，按时间戳差算 dt 都是可伸缩求和，总时长不丢；
  换屏时时间戳倒退会落回墙钟。看门狗与垂直同步交替：只在垂直同步断 100 ms 后推，恢复时第一帧按墙钟、之后按时间戳，最多差一帧。
- **脏区**：见 R-02，除 S8 外所有场景 0 残影；悬停时 rect-list 口径有 ≤ 2 LSB 的抗锯齿差（光标列边缘，T-032 起就有，看不出）。
- **缓存作废时机**：back（尺寸 / 低切斜线 / 像素比）、dbLabels、frequencyText（像素比）、readout（文字 / 语言 / 像素比）、outputRow
  （`buildColumnLevels` / 像素比）、stripeRow（像素比 / 颜色）都有触发点；`setBufferedToImage` 的失效链路（Segmented 悬停、开关悬停、
  小窗悬停与 `setValues`）经 `internalRepaintUnchecked` 逐级作废，正确——问题只在贴图方式（R-01）。
- **跨平台填充**：JUCE 9 CoreGraphics 的平铺图填充一律走 `CGContextDrawTiledImage`（没有逐行手工平铺的慢路径），`CGImage` 由
  `CFDataCreate` 拷贝像素，配合 `drawsAsynchronously` 安全；绝对开销要 Max 在 Mac 上实测。Direct2D：`outputRow` 整图 `writeOnly`
  不触发回读，只每帧上传 8 KB；`stripeRow` 只在像素比变时回读一次。小数像素比下行图按 `lowResamplingQuality` 吸附（≤ 0.5 px），软件渲染器出图正常。
- **按响度上色**：预乘 over 合成公式正确；`levelColour` 静态表线程安全；OUT 关时 `setOpacity (0.12)` 在三套渲染器都有对应实现。
- **粒子电平口径**：`takeRecentRmsDb` 在 30 Hz 定时器里取，等价于原来「这一 tick 拉到的全部样本」；额外的 `timerCallback`（语言、控件提示）
  提前取走的情形与改前相同。取样口 32768 容量在最坏 133 ms 间隔下够用。
- **回归**：`celadon_tests --only SpectrumAnalyser` 全过（146 项）；`--deterministic` 两次出图逐字节相同；`--only check` 6 轮中 5 轮 76 / 76 全过，
  第 1 轮有 2 条 T-032 检查失败（GLAZE 标题带内缓存读 0 次；状态胶囊呼吸帧排版了 3 次字），之后 5 轮复现不出，基线 `88ff5b6` 6 轮全过。
  失败项不在频谱仪里，倾向是计时敏感的偶发；属范围外，建议另开一项查这两条检查在负载下的稳定性。

## 附：107 上怎么复跑

- 探针源码：`~/.cache/celadon-review-t036/probe/`（`ReviewAnalyserProbe.cpp`、`ReviewUiProbe.cpp`、`cmake-append.txt`、`run-probes.sh`），
  本机副本在评审会话的 scratchpad。`run-probes.sh` 把同步过去的工作树拷到 `~/.cache/celadon-review-t036/src`、追加三个目标、在独立构建目录编，
  不碰 `$BUILD`。
- 例：`scripts/test-linux.sh bash -c 'bash "$HOME/.cache/celadon-review-t036/probe/run-probes.sh" review_ui_probe; R=$HOME/.cache/celadon-review-t036; mkdir -p "$R/run" "$R/home/.config"; cd "$R/run"; HOME="$R/home" XDG_CONFIG_HOME="$R/home/.config" xvfb-run -a "$R/build/review_ui_probe" dirty'`
  （参数 `dirty` / `sharp` / `cost` / `prims`；分析器探针目标 `review_analyser_probe`、`review_analyser_probe_asan`，直接运行）。
  `sharp` 的三种挂法是探针强制切换的（与源码里挂哪种无关），修完 R-01 后看「aligned cache」一列即新行为；修完 R-02 后 `dirty` 的 S8 应为 0。
