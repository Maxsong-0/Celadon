# T-032 实现任务书 · A10-ui-spectrum · 界面：频谱仪视图与计算

> 生成于 2026-09-29 23:0x，来源：研究代理 R*.md 与核验代理 V*.md（同目录）。逐条以「核验调整后的修法」为准。

## 你负责的文件（只许改这些）

Source/ui/SpectrumView.{h,cpp}、Source/ui/SpectrumAnalyser.{h,cpp}；tests/SpectrumAnalyserTests.cpp。**不新建源文件；不改 SnapshotMain（A9 在改）；不改估计量口径（R7-01 / R9-03 待 Max）**。

## 主进程补充要求

- R9-04 做 ①（按像素比生成竖纹瓦片 + setFillType 平铺 fillPath；渐隐改 setGradientFill + fillPath；去掉 reduceClipRegion 与 516 次 fillRect）、②（面板渐变、网格、频率与 dB 标签、读数框底缓存成按像素比的 Image，语言 / 斜率 / 修坯标记变化时失效）、④（output 只建一次路径）；③ 改为读数框与光标点做成独立子组件（读数框 setBufferedToImage）或 paint 里按 g.clipRegionIntersects 跳过整块。
- R7-05 / R9-10 同一件事：setSampleRate 时用同样的 double 表达式按同样顺序算好每点 {f, first, last, interpolate, t, 覆盖 bin 数}，analyse 只剩 max / 插值与 log10，显示逐位不变（改前把 480 点结果存成快照断言相等）；SpectrumView 在斜率 / 采样率变时预算 tilt[i] 与 x[i]。顺带：IN 虚线关着时跳过 IN 那路的加窗 + FFT。
- R9-11 取修法②：FFT 阶数封顶 14（16384），192k 接受 85 ms 窗；现有 96k 用例照过，加 192k 用例。
- 验证：celadon_tests --only SpectrumAnalyser（107，scripts/test-linux.sh 自定义命令）；再编 celadon_ui_snapshot（107 已装依赖，-j 3）出改前 / 改后 PNG 与 `--only check`，逐像素比对频谱区域并解释差异（瓦片相位 ≤ 1 px）。

## 逐条结论

### R9-04 · 频谱仪每帧 516 次裁剪竖纹 fillRect + 悬停时整面重画
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 M；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/SpectrumView.cpp:629`
- 证据：SpectrumView.cpp:624-635 `reduceClipRegion(fill)` 后 `for (x = 0; x < w; x += 2) fillRect(x, 0, 0.8, h)` 共 516 次再整面渐变；:539-546 圆角路径裁剪；:585-609 与 :613,638,640,648,656 同一帧 5 次 buildLine（输出曲线建两遍）；:751-762 mouseMove/mouseExit 整面 repaint()；:712-744 网格文字、6 个 dB 标签、读数框每帧重画。实验 3 B2：竖纹+渐隐 7.10 ms @2× / 18.73 ms @4×，改平铺填充 3.33 / 7.98 ms；主填充+两条描边 2.71 / 9.17 ms。
- 机理：每条 0.8 px 竖纹都是一次带路径裁剪的抗锯齿填充，30 Hz 播放时整面重画，悬停时再叠加 60～120 Hz 鼠标事件的整面重画；静态网格与标签也每帧重排文字。
- 研究修法：① 按像素比生成 2 逻辑像素宽的瓦片（宽 round(2·pixelScale)，画 0.8·pixelScale 宽白条），g.setFillType(FillType(tile, AffineTransform::scale(1/pixelScale))) 后 fillPath(fill)；渐隐改 setGradientFill + fillPath(fill)，去掉 reduceClipRegion(fill) 与 516 次 fillRect；② 面板渐变、网格、100 Hz/1 k/10 k 与 dB 标签、读数框底缓存成按像素比的 Image，每帧 drawImageAt 后只画曲线、标记与读数文字；③ 悬停只 repaint 读数框矩形与新旧光标列（±6 px）；④ buildLine 对 output 只建一次路径，闭合版拷贝后加两点。
- 核验理由：SpectrumView.cpp:624-635 reduceClipRegion(fill) 后 for(x=0;x<1032;x+=2) fillRect(x,0,0.8,h) = 516 次再整面渐变；:613,638,640,648,656 五次 buildLine（output 两遍）；:712-744 网格文字、6 个 dB 标签、读数框每帧；:751-762 mouseMove/mouseExit 整面 repaint；:452-470 的 0.02 dB 阈值使播放中每帧重画。瓦片填充路径软件渲染器 juce_RenderingHelpers.h:2579-2581 与 CoreGraphics CGContextDrawTiledImage（juce_CoreGraphicsContext_mac.mm:870-871）都支持。③ 在 macOS 上读数框 (48,12)-(298,38) 与光标列的包围盒近乎整面，且 paint() 全部绘制调用照发，几乎不省。
- **核验调整后的修法（以此为准）**：做 ①（按像素比生成竖纹瓦片 + setFillType 平铺 fillPath；渐隐改 setGradientFill+fillPath）、②（面板渐变、网格、频率与 dB 标签、读数框底缓存成按像素比的 Image）、④（output 只建一次路径）；③ 改为把读数框与光标点做成独立子组件（读数框 setBufferedToImage），或在 paint 里按 g.clipRegionIntersects 跳过整块，不要只缩小 repaint 矩形。截图断言只在 --scale 1/2 下要求逐像素相同，其余允许 ±1 px 相位与 ±1 LSB。
- 风险：瓦片相位在非整数缩放下与现状差 ≤1 px；静态缓存要在语言/斜率/修坯标记变化时失效。
- 验证：ui-snapshot --bench 按组件 paint 时长（目标每帧 −4 ms @2×、−10 ms @4× 软件口径）；截图逐像素对比（竖纹相位一致时逐像素相同，允许抗锯齿 ±1 LSB）。

### R7-05 · SpectrumAnalyser::analyse 每帧每点重算循环不变量（约 86 k 次 pow/s）
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/SpectrumAnalyser.cpp:178`
- 证据：SpectrumAnalyser.cpp:178–182 每帧每点调 frequencyAt(i)（:52–56，一次 std::pow）与 std::pow(2.0, ±halfStep) 两次（与 i 无关），:188–198 再做 floor/ceil/jlimit；两路 × 480 点 × 30 帧/s ≈ 86 k 次 pow/s，全部只依赖采样率与点号。
- 机理：每点的 bin 区间、插值系数、覆盖 bin 数都是采样率的函数，却在 30 Hz 定时器里每帧重算，白占主线程。
- 研究修法：setSampleRate 里算好每点的 {first, last, t, interpolate, H_n}（与 R7-01 共用一张表），analyse 只剩 max/mean 与一次 log10。
- 核验理由：SpectrumAnalyser.cpp:180–182 每点每帧 3 次 std::pow（frequencyAt :55 一次 + 两次与 i 无关的 2^±halfStep）、:188–198 floor/ceil/jlimit 属实；PluginEditor.cpp:23 refreshHz = 30 → 两路 × 480 × 30 = 86.4 k 次 pow/s ≈ 2～4 ms/s 消息线程时间，与两路 8192 点 FFT 同量级但绝对量小（约 0.3 % 一核）。
- **核验调整后的修法（以此为准）**：setSampleRate 里用同一算式算好每点 {f, low, high, interpolate, below/above/t 或 first/last} 存表，analyse 只做 max/插值与 log10，显示逐位不变；既有 SpectrumAnalyserTests 照过。顺带更大的省法：IN 虚线关着时跳过 IN 那路的加窗 + FFT。
- 风险：无（同一算式同一舍入）；若与 R7-01 合并改则按新口径验证。
- 验证：既有 SpectrumAnalyserTests 通过（同一算式同一 double 舍入，显示逐位不变；若与 R7-01 一起改则按新口径）；主线程 profile 里 analyse 自身时间下降。

### R9-10 · 频谱仪每帧重算约 7.7 k 次 pow/log（点频率、bin 区间、斜率）
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/SpectrumAnalyser.cpp:180`
- 证据：SpectrumAnalyser.cpp:52-56 frequencyAt 用 pow；:174-182 每点两次 pow(2, ±halfStep)（都是常数）；:205 每点 log10；SpectrumView.cpp:444-447 tiltAt 用 log2，:594 5 条曲线各 480 点每点 frequencyAt + tiltAt，:489-496 updatePeakReadout 同样。每帧 2 路 × 480 × 3 pow + 5 × 480 × (pow + log2) ≈ 7.7 k 次超越函数 ≈ 0.15～0.3 ms。
- 机理：点到 bin 的映射只随采样率变，斜率项只随斜率变，却每帧重算。
- 研究修法：setSampleRate 时预算每点的 [first,last] bin 区间与插值位置（std::vector<PointRange>）；SpectrumView 在斜率或语言变化时预算 tilt[i] 与 x[i]；buildLine 只做加法。
- 核验理由：SpectrumAnalyser.cpp:52-56 frequencyAt 用 pow；:180-182 每点两次 pow(2,±halfStep)（常数）；:205 log10；SpectrumView.cpp:444-447 tiltAt 用 log2；:594 每条曲线每点 frequencyAt+tiltAt；:489-496 updatePeakReadout。2 路×480×3 + ≤5×480×2 ≈ 7.7k 次超越函数属实。
- **核验调整后的修法（以此为准）**：setSampleRate 时预算每点 [first,last] bin 区间与插值位置（用同样的 double 表达式按同样顺序算一次，结果确定性相同）；SpectrumView 在斜率或采样率变时预算 tilt[i] 与 x[i]。用现有测试 + 改前 480 点快照断言相等。
- 风险：无（界面不在 ADR-013 逐位契约范围内，快照断言只是回归保护）。
- 验证：SpectrumAnalyserTests 现有用例逐位不变（先存改前 480 点结果再断言相等）。

### R9-11 · 96 k/192 k 下界面 FFT 长度翻到 16 k/32 k，Windows 默认回退 FFT 每帧 1.4～3.5 ms（192 k 时单核 10 %）
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 M；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/ui/SpectrumAnalyser.cpp:71`
- 证据：SpectrumAnalyser.cpp:66-79 ≤50 k 用 8192、≤100 k 用 16384、其余 32768；:160-170 每帧两路各一次全长加窗拷贝 + FFT。实验 5（JUCE 9.0.2 回退引擎，x86-64）：8192 点 0.356 ms、16384 点 0.710 ms、32768 点 1.730 ms → 两路 × 30 Hz = 单核 2.1 % / 4.3 % / 10.4 %。macOS vDSP 快 5～10 倍不受影响。
- 机理：为保持约 170 ms 窗口，FFT 随采样率加长；显示上限 20 kHz，高于 24 kHz 的内容本就不显示，却全程参与 FFT。
- 研究修法：取样口之后先按 2/4 抽取到 ≤50 kHz（界面线程做 8 阶半带 FIR），FFT 固定 8192；或至少上限钉在 16384 并接受 192 k 时 85 ms 的窗。
- 核验理由：SpectrumAnalyser.cpp:71,78-79 按采样率选 13/14/15 阶；:163-170 两路各一次全长加窗+FFT；juce_FFT.cpp:439-443 只有 (JUCE_MAC||JUCE_IOS)&&JUCE_USE_VDSP_FRAMEWORK 一条加速引擎，Windows 无 MKL/FFTW 时确是 FFTFallback。但修法 ①「8 阶半带 FIR 抽取」不成立：短半带过渡带绕 fs/4 很宽，96k 时 fs/4=24 kHz，20 kHz 显示端会明显下垂、24～28 kHz 内容折叠进 20～24 kHz。
- **核验调整后的修法（以此为准）**：取修法 ②：FFT 阶数封顶 14（16384），192k 接受 85 ms 窗；若坚持抽取，须 ≥31～63 抽头（可复用 dsp/HalfBandOversampler 系数）并让现有 96k 用例（1 kHz ±1 dB、泄漏 <−60 dB）与新增 192k 用例通过。
- 风险：封顶后 192k 频率分辨率减半（低频点仍插值，显示差别很小）。
- 验证：SpectrumAnalyserTests 现有 96 k 用例 + 新增 192 k 用例（正弦读数 ±1 dB、泄漏 < −60 dB）；Windows 上 Task Manager 对比。
