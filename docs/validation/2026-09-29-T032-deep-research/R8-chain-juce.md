# R8 · 链路与 JUCE 集成 深度研究报告（T-032，2026-09-29）

> 代理编号 R8。子系统：`CeladonChain` / `PluginProcessor` / `Parameters` / `ParameterRanges` / `AtomicHelpers` / `AnalyzerTap` / `.jucer` / `CMakeLists.txt`。
> 基线：本工作树（分支自 main `e6b445b`，v0.1.8.5），只读；实验在 107 上做（见第 5 节），实验目录已删除。
> 写于 2026-09-29 22:2x PDT。

## 0. 一句话

链路与 JUCE 集成层整体是健康的：音频线程零分配、零锁（只 try-lock）、延迟上报走消息线程、同规格 `prepareToPlay` 只 reset、超大宿主块与单声道都有对策、存档往返与旧工程迁移的特判正确。**实证到的缺陷有 4 条**，全部在「三种临时监听（预听 / 原声 / 差值）与表」这一 2026-09-28 新加的交接面上，和「非有限参数拦截不全」：

| 编号 | 一句话 | 严重度 | 优先级 | 工作量 | 置信度 |
|---|---|---|---|---|---|
| R8-01 | Listen 关着时关掉预听、或预听中切到原声 / 宿主旁通 / 试听差值：监听增益单样本跳 6 dB（实测 0.499 → 1.0） | medium | P1 | S | 0.9 |
| R8-02 | Limit / Color 参数不拦 NaN；NaN 能经 XML 与 APVTS 原样进模块：释放时间 NaN 让限幅器退化成硬削波，Tone NaN 进入逐块 NaN 重启循环 | medium | P1 | S | 0.85 |
| R8-03 | 听原声 / 宿主旁通期间 OUT 表照常积分：整体响度把原声混进母带读数（实测 −14.00 → −16.03 LUFS，18 s 后仍 −14.71），预听归一化也跟着偏 | medium | P1 | M | 0.85 |
| R8-04 | 预听中点表的 Reset：整体响度变「未知」→ 预听增益 50 ms 内回到 0 dB、保持 400 ms 再落回（监听 +6 dB 爆响 0.4 s） | low | P2 | S | 0.8 |

不报纯风格；已知待认领项（修坯预算、474 % 尖峰、mastering-bench 最大值、NEON、界面 21 %、起音密度、编码余量实测、AU 版本整数）没有重报。

## 1. 范围、读过的材料与方法

- 档案：`01-框架/设计规格-总体.md`（含 2026-09-28 修订）、`设计规格-高级母带控制.md`、`设计规格-GLAZE-时间线与保存.md`、`设计规格-Limit.md` 末节；ADR-001 / 004 / 007 / 010 / 013 / 018 / 020；BUG-004；`04-协调/TASKS.md` 待认领区与 T-032 计划。
- 代码：`Source/dsp/CeladonChain.{h,cpp}`、`ChainParams.h`、`AtomicHelpers.h`、`DelayLine.h`、`Source/PluginProcessor.{h,cpp}`、`Parameters.{h,cpp}`、`ParameterIds.h`、`ParameterRanges.h`、`AnalyzerTap.h`、`Celadon.jucer`、`CMakeLists.txt`、`JuceLibraryCode/JucePluginDefines.h`；为核实链的假设另读了四个模块的 `prepare / reset / setParams / process` 入口、`LoudnessMeter.cpp` 全文、`ReleaseSafety.{h,cpp}`、`ReferenceSpectrum.{h,cpp}` 的线程交换部分、`GlazeController.{h,cpp}` 的锁与存档部分、`PluginEditor.cpp` 与处理器交互的部分。
- 测试：`tests/ChainTests.cpp` 用例清单与 Listen / 零分配 / 同规格重置用例、`AdvancedMasteringTests.cpp` 监听交接与预听用例、`LegacyFixtureTests.cpp`、`AnalyzerTapTests.cpp`、`TestMain.cpp`；`tools/perf/bench.mm` 只看驱动方式。
- JUCE 9.0.2 源码（`/Users/maxsong/All-Project/JUCE/modules`）：`ProcessSpec` 的 `operator!=`、`NormalisableRange::convertTo0to1 / convertFrom0to1`、`jlimit` / `roundToInt`、`CharacterFunctions::readDoubleValue` 的 NaN 解析、`AudioProcessorValueTreeState::replaceState / setNewState / ParameterAdapter::setDenormalisedValue`、`AudioParameterFloat::setValue`、`AudioProcessor::setLatencySamples / processBlockBypassed / reset`、VST3 包装器的旁通与 `setProcessing`、AU 包装器的 `BypassEffect → Reset`。
- 方法：先按规格列出集成层的契约（实时安全、线程交换、延迟、旁通、存档、声道、块长、NaN），逐条对照代码与 JUCE 源码；对怀疑点写一个 218 行的驱动程序，链接 107 上本工作树的 Release 目标文件（`Source/dsp/*.o` + `libceladon_juce.a`，源码 SHA-256 与本地逐一相同），直接量链的行为（第 5 节）。

## 2. 结论（按 优先级 × 置信度 排序）

### R8-01 预听退出时监听增益单样本阶跃（Listen 关着 / 切原声 / 宿主旁通 / 试听差值）

- **严重度 / 优先级 / 类别**：medium / P1 / 设计缺陷（监听路径连续性，属工单重点「旁通与 A/B 的爆音」）。
- **文件与行号**：`Source/dsp/CeladonChain.cpp:143-158`（`setParams` 的预听交接）；`Source/dsp/ReleaseSafety.cpp:24-29`（`reset()` 把预听增益直接落到 1.0）；`Source/dsp/CeladonChain.cpp:294-296`（干路 / 差值时不再调 `releaseSafety.process`）；`Source/dsp/CeladonChain.cpp:383-393, 479`（`applyListenGain` 在所有路径上都会跑，可承接斜坡）。
- **证据**：

  ```cpp
  // CeladonChain.cpp:147-156
  if (nextPreview && ! previewActive) { releaseSafety.setCurrentPreviewGain ((double) listenGain.getCurrentValue()); listenGain.setCurrentAndTargetValue (1.0f); }
  else if (previewActive && ! nextPreview && releaseAllowed && ! offline && newParams.limit.listen)   // ← 只有 Listen 开着才交接
      listenGain.setCurrentAndTargetValue ((float) releaseSafety.getCurrentPreviewGain());
  if (! releaseAllowed || (previewActive && ! nextPreview)) releaseSafety.reset();                    // ← 其余情形直接落到 1.0
  ```

  实测（第 5 节 R8-01）：稳态预听 −6.00 dB 时，四种退出方式在切换样本处的增益跳变都是 0.499（即 0.501 → 1.000，6.00 dB），只有「Listen 开着」这一种走了斜坡（每对比样本 ≤ 0.0057）。
- **机理**：预听增益由 `ReleaseSafety::previewGain` 承载。退出预听时代码只在 `newParams.limit.listen` 为真时把当前增益交给 `listenGain` 再由它 20 ms 平滑到目标；Listen 关着、或退出原因是 `dryPath`（原声 / 宿主旁通）或 `auditionDifference`（`releaseAllowed = false`）时不交接，`releaseSafety.reset()` 把增益瞬间置 1，而且干路 / 差值路径上根本不再调 `releaseSafety.process`，所以输出在块边界从 0.5× 直接变 1.0×。离线（`offline`）那一支按规格就该立即归一（验收用例「offline transition removes preview from its first sample」），本条不动它。
- **修法**（`CeladonChain::setParams`，两处）：
  1. 把交接条件从 `releaseAllowed && ! offline && newParams.limit.listen` 改为 `! offline`：只要 `previewActive && ! nextPreview && ! offline`，先 `listenGain.setCurrentAndTargetValue ((float) releaseSafety.getCurrentPreviewGain())`，再 `releaseSafety.reset()`；随后已有的 `updateListenTarget()` 会让 `listenGain` 在 20 ms 内平滑到目标（Listen 关或干路 = 1.0；Listen 开 = 等响增益）。`applyListenGain` 在干路、差值路径上照样执行（`processListen` 无条件调用），斜坡能落地。
  2. 保留 `offline` 分支的立即归一（导出不能带尾巴）。
  伪码：
  ```cpp
  const bool leavingPreview = previewActive && ! nextPreview;
  if (leavingPreview && ! offline)
      listenGain.setCurrentAndTargetValue ((float) releaseSafety.getCurrentPreviewGain());
  if (! releaseAllowed || leavingPreview) releaseSafety.reset();
  ```
- **改后输出是否逐位不变**：否——只在退出预听后的 ≤ 20 ms 监听信号上变（母带、表、GLAZE 不变）；预听从未打开时逐位不变。
- **验证**：在 `AdvancedMasteringTests::testMonitoringHandover` 里补四个用例（Listen 关着关预听 / 切原声 / 宿主旁通 / 试听差值），沿用 `ChainTests` 「Listen switch must ramp」的判据：受测链与参照链（从未预听、同步做同样切换）逐样本增益比的相邻跳变 < 0.01；今晚实测为 0.499。
- **工作量 / 置信度**：S / 0.9。

### R8-02 非有限参数没有在链入口统一拦截：NaN 经 XML → APVTS → `readParams` 原样进 Limit / Color

- **严重度 / 优先级 / 类别**：medium / P1 / 算法正确性（边界与极端参数，工单 4(a)「NaN/Inf」）。
- **文件与行号**：`Source/PluginProcessor.cpp:352-367`（`readParams` 对 Color / Limit 直接 `load()`，不判有限）；`Source/dsp/CeladonChain.cpp:105, 112`（`manual = newParams; adjusted = newParams;` 不清洗）；`Source/dsp/LimiterModule.cpp:338-361`（`setParams` 无 `isfinite`：345 行 `decibelsToGain (params.gainDb)`、361 行 `timeConstantCoeff (sampleRate, params.releaseMs)`）；`Source/dsp/ColorModule.cpp:609-613`（`jlimit` 对 NaN 原样放行）。对照已做守卫的：`BalanceModule.cpp:450-468`、`ClayModule.cpp:379-386`、`CeladonChain.cpp:114, 131`（mastering 的 `unit` / margin）、`PluginProcessor.cpp:441, 458`（Lookahead）。
- **JUCE 侧证据**（9.0.2）：`juce_CharacterFunctions.h:257-260` 把文本 "nan" 解析成 `quiet_NaN`；`juce_NormalisableRange.h:136-139, 160-162` 的 `clampTo0To1` 就是 `jlimit`，`juce_MathsFunctions.h:551-560` 的 `jlimit` 对 NaN 返回 NaN；`juce_AudioProcessorValueTreeState.cpp:101-105` `setDenormalisedValue` 只做 `approximatelyEqual` 判等（对 NaN 恒假）后直接写；`juce_AudioParameterFloat.cpp:98` `setValue` 不钳。实测（第 5 节 R8-02a）：`juce::String (NaN)` 写出 "nan"、读回 isnan；`logRange` 与线性范围的 `convertTo0to1 / convertFrom0to1 / snapToLegalValue` 对 NaN 全部 isnan。也就是说，任何一次上游写坏（宿主自动化 / 界面 / 手改工程文件）都会随工程保存并在重开时原样回到 `getRawParameterValue`。
- **链的表现**（第 5 节 R8-02b，+24 dB、Ceiling −1 dBTP、1 kHz −20 dBFS）：`limit.releaseMs = NaN`（手动释放）→ 0 个非有限块、峰值恰好 0.8913（= Ceiling）、RMS 0.7727（正常 0.6053）——前瞻增益级失效、只剩最后的硬钳位在工作，正弦被削成方头（硬削波失真）；`limit.gainDb / ceilingDb / color.outputDb = NaN` → 永久静音（`decibelsToGain (NaN)` 返回 0）；`color.toneDb = NaN` → 187 / 375 块非有限，链每隔一块清零重启，输出静音、界面警告计数不停涨。Balance / Clay 的 NaN 按守卫回中性、输出正常（0.6053 / 0.8561）。
- **机理**：规格「错误处理与安全」把参数范围交给 APVTS 钳制、模块「不再信任外部值」，但 APVTS 只钳有限值，模块守卫又只做了一半（Balance / Clay / mastering / Lookahead 有，Limit / Color 没有）。`timeConstantCoeff (fs, NaN)` 给出 NaN 系数，包络比较全为假，限幅器不再压增益；`designShelf (NaN)` 让 Tone 滤波器输出 NaN。
- **修法**（三处，都是纯函数式钳制，对有限且在范围内的值是恒等）：
  1. `LimiterModule::setParams`：`gainDb / ceilingDb / releaseMs / lookaheadMs / clip` 各自 `std::isfinite ? jlimit (范围) : 默认`（范围用头文件 `Params` 注释里的 0～24 / −3～0（编码余量后允许到 −4.5，钳到 −4.5～0） / 10～1000 / 1～10 / 0～100）；
  2. `ColorModule::setParams`：五个 `jlimit` 前加 `isfinite`，非有限取 `Params {}` 默认；
  3. `CeladonAudioProcessor::readParams`（防御层）：对每个 `load()` 结果加一个 `finiteOr (value, 默认)`。
  逐位契约：对有限输入 `jlimit` 与现状同一表达式，不改任何位；GLAZE 校准参考条件的哈希输入不变。
- **改后输出是否逐位不变**：是（有限输入）。
- **验证**：headless 测试：对 `ChainParams` 每个 float 字段分别喂 NaN / ±Inf，`process` 全程不报非有限、输出非静音、真峰值 ≤ Ceiling + 0.01 dB（+24 dB 推响），并与「该字段取默认值」的链逐位相同；`readParams` 那一层不在 `celadon_tests` 里（处理器只进 `celadon_ui_snapshot`），所以真正的守卫放模块，处理器只是第二道。
- **工作量 / 置信度**：S / 0.85。

### R8-03 听原声 / 宿主旁通期间 OUT 表继续积分：整体响度把原声混进母带的读数

- **严重度 / 优先级 / 类别**：medium / P1 / 设计缺陷（表与规格不一致）。
- **文件与行号**：`Source/dsp/CeladonChain.cpp:263`（`outputMeter.process (io);` 不看 `dryPath`）；`CeladonChain.cpp:107-109, 173-176`（`dryPath = hostBypassed || compareOriginal` 时四个模块全旁通，出口就是延迟后的原声）；`Source/dsp/LoudnessMeter.cpp:271-310`（`finishSubBlock` 无条件把门块记进直方图）；`CeladonChain.cpp:295`（预听归一化读的正是这个整体响度）；`Source/PluginEditor.cpp:1033-1043`（界面听原声时只把真峰值最大值排除，整体响度照读）。
- **规格依据**：`设计规格-总体` 2026-09-28 修订：「原声、差值、预听三种临时监听互斥，**只改送给宿主的监听信号，表与 GLAZE 照常按母带算**」；`CeladonChain.h:121-123`：「OUT 的短期 / 整体响度（界面右侧读数）：Listen 监听补偿之前的**母带输出**」。差值与预听确实满足（测点在换信号之前），原声（与宿主旁通）不满足——干路上没有母带可测，表测到的是原声。
- **实测**（第 5 节 R8-03，+6 dB，1 kHz −20 dBFS）：母带 6 s 整体 −14.00 LUFS；切到原声（或宿主旁通）6 s 后 −16.03；回到母带再 6 s −15.27，再 18 s 仍 −14.71。原声只比母带轻 6 LU，落在 BS.1770 相对门（−10 LU）以内，全部计入；母带只比原声响 ≤ 10 LU 是常态（GLAZE 默认目标就在这个区间），所以几乎每次 A/B 都会把整体读数拉低，且长时间不回来。预听归一化衰减 = 目标 − 整体响度，也随之偏 1～2 dB。
- **修法**（M）：
  1. `LoudnessMeter` 加一个音频线程标志 `setIntegrationPaused (bool)`：暂停时 K 加权滤波器、子块环（GLAZE 配对用的 `SubBlock` 网格与 `processedSamples`）、短期窗口照常推进，只跳过 `finishSubBlock` 里门块进直方图那一步（`histogramEnergy / histogramCount / updateIntegrated`）；真峰值照常。
  2. `CeladonChain::setParams` 里 `outputMeter.setIntegrationPaused (dryPath)`。短期响度仍显示此刻听到的信号（与界面「OUT 是原声」的口径一致），整体响度与预听归一化保持母带的值。
  3. 规格补一句：整体响度只积分母带，临时监听与宿主旁通期间冻结。
  校准器读的是子块环，不受影响；`Status` / 执行快照不变。
- **改后输出是否逐位不变**：是（只改表的读数，不碰音频）。
- **验证**：把第 5 节 R8-03 写成测试：母带 6 s 后切原声 6 s，整体响度与切换前相差 < 0.01 LU；短期响度在原声期间等于原声的读数；宿主旁通同。另跑 `GlazeCalibration` 保存读取用例确认子块配对不变。
- **待 Max 确认的口径**：是否希望宿主旁通期间也冻结（本报告建议冻结：宿主旁通就是宿主那边的 A/B）。
- **工作量 / 置信度**：M / 0.85（缺陷 0.9，口径 0.8）。

### R8-04 预听中点表的 Reset：预听增益先回 0 dB 保持 400 ms 再落回

- **严重度 / 优先级 / 类别**：low / P2 / 设计缺陷（监听爆响；现行为符合规格字面，属规格层取舍）。
- **文件与行号**：`Source/dsp/ReleaseSafety.cpp:61-72`（读数 ≤ −90 LUFS 或非有限 → 目标增益 1.0，50 ms 平滑）；`Source/dsp/LoudnessMeter.cpp:98-110`（`clearMeasurement` 把整体读数置 `noReading = −100`）与 `:288-289`（`recentCount < 4` 前不进直方图：Reset 后至少 400 ms 没有整体读数）；`Source/dsp/CeladonChain.h:125-130`（界面 Reset 同时清两块表）；`CeladonChain.cpp:295`。
- **实测**（第 5 节 R8-04）：稳态预听 −6.00 dB；点 Reset 后 50 ms 内预听增益到 0.00 dB，持续 8 × 50 ms，400 ms 处整体读数回来（−7.99）后再 50 ms 落回 −6.01 dB。监听电平 +6 dB 爆响 0.4 s。
- **机理**：规格《高级母带控制》§6 写「读数未知（≤ −90 LUFS 或非有限）时回到 1」，是为「从未有读数」的场景设计的安全回退；对「曾经有读数、短暂丢失」的场景（Reset、同规格重建后的前 400 ms）它变成了爆响源。
- **修法**（S）：`ReleaseSafety` 记住最近一次有效目标增益与「自上次有效读数以来的样本数」：读数未知且此前有过有效读数时，保持上次目标不动，直到新读数到来或超过约 3 s（3 s 仍未知再回 1.0）；从未有过读数、显式 `reset()` / 关闭预听 / 离线仍立即归一。规格 §6 同步改一句。
- **改后输出是否逐位不变**：否（只在 Reset / 重建后的 ≤ 3 s 监听信号上变；母带不变）。
- **验证**：把第 5 节 R8-04 写成测试：`requestMeterReset()` 后 2 s 内预听增益最大值 ≤ 稳态 + 0.5 dB；再测「从未有读数」仍为 1.0、「读数 3 s 不回来」回到 1.0。
- **工作量 / 置信度**：S / 0.8。

## 3. 已核对、没有问题的项（给核验代理省时间）

按工单重点逐条给出结论与出处；这些不在 finding 里。

- **processBlock 实时安全**：`processBuses / processChain` 全程无堆分配、无锁：`getBusBuffer` 与 0 声道 `AudioBuffer` 视图用预分配的通道表（≤ 32 声道）；`readParams` 只读 36 个原子；`readHostInfo` 用 `Optional` 值语义；`AnalyzerTap::push` 只拷贝（`AnalyzerTap.h:47-76`）；GLAZE 发布与接收存档只 `tryEnter`（`GlazeController.cpp:2706, 2736`）；参考谱是全原子字的 seqlock（`ReferenceSpectrum.cpp:39-70`）；预听 / 抖动无分配。`ChainTests` 「never allocate」用例（`ChainTests.cpp:940-1021`）覆盖链，处理器层靠评审。
- **消息线程交互**：`setLatencySamples` 只在 `prepareToPlay` 与 30 Hz 定时器里调（`PluginProcessor.cpp:430, 492-500`）；`getTailLengthSeconds` 只读两个原子（`:279-288`）；`getStateInformation` 在 SpinLock 里拷贝后再序列化（`GlazeController.cpp:3000-3020`），`ReferenceSpectrum::createStateXml` 的自旋只在消息线程（`ReferenceSpectrum.cpp:466-473`）。
- **延迟上报路径**：`LookaheadHold`（`PluginProcessor.cpp:434-490`）逐条走过：整体重建直接采用；同规格重启保留正在用的值、第一块受 `samplesSinceCommit ≥ settleSamples` 约束；非有限值当「没变」。VST3 包装器收到延迟变化后 `restartComponent (kLatencyChanged)`、`setProcessing (false)` 会调 `reset()`（`juce_audio_plugin_client_VST3.cpp:1540-1555, 3474-3477`），`prepareToPlay` 把这个 pending reset 清掉（`:426`），与 v0.5 验收 V2-2 一致。`LimiterModule::getLatencySamples` 在 `setParams` 后立即反映新前瞻（`LimiterModule.cpp:363-375`），与 `pendingLatency` 同块一致。
- **prepareToPlay 顺序**：整体重建时先 `setParams` 再 `prepare`，四个模块在 `prepare → reset` 里都按新采样率重算参数相关量（Limiter `:246-247`、Color `:593-596`、Clay `:355-357`、Balance `:364, 443`），第一块 `setParams` 再算一遍；`ProcessSpec` 的 `operator!=` 存在（`juce_ProcessContext.h:66`）。
- **超大宿主块**：模块按 `maxBlockSize` 分段（Limiter `:435`、Color `:765`、Balance `:559`），Clay 逐样本环（`ClayModule.cpp:542-544`），两块表、`AnalyzerTap`、参考谱不受块长限制，差值缓冲按 prepare 块长切片（`CeladonChain.cpp:202-226`），校准器忽略块长（`GlazeCalibrator.cpp:357`）。
- **声道**：`isBusesLayoutSupported` 只允许主路 1 进 1 出 / 2 进 2 出，侧链 关 / 单 / 双（`PluginProcessor.cpp:512-536`）；单声道时模块按 1 声道处理（Clay 有 `processMono`，`:558`），表按双单声道计能量（`LoudnessMeter.cpp:247`），侧链单声道时右声道取左（`ReferenceSpectrum.cpp:401`）。
- **NaN / Inf 音频**：输入非有限整块清零计数（`PluginProcessor.cpp:595-599`）；输出非有限清零 + 整链淡入重启（`CeladonChain.cpp:255-261`）；侧链逐样本 sanitize（`ReferenceSpectrum.cpp:410-415`）；`ScopedNoDenormals` 在位（`PluginProcessor.cpp:553, 571`，重复一次无害）。
- **旁通**：VST3 只在 `getBypassParameter() == nullptr` 时调 `processBlockBypassed`（`VST3.cpp:3728-3731`，本插件正是这种），AU 打开旁通时会 `Reset (0, 0)`（`AU_1.mm:772-788`），处理器的 2 块宽限把它吃掉（`PluginProcessor.cpp:579-590`）；链内旁通 = 各模块延迟补偿式交叉淡化，`ChainTests` 「host bypass is bit-exact delayed input」覆盖。
- **存档 / 迁移**：GLAZE 与参考节点先摘出再 `replaceState`（`PluginProcessor.cpp:790-820`；`replaceState` 在 APVTS 自己的锁里，`juce_AudioProcessorValueTreeState.cpp:396-404`）；缺参补节点的特判（GLAZE 0、刺耳关 = 索引 1、目标 / 余量 / 抖动 0）与 ADR-020 一致，`AudioParameterChoice` 在状态树里存的是下标，`1.0f` 确实是 Off（`:850-857`）；参考谱 XML 逐项校验（`ReferenceSpectrum.cpp:487-526`）；`LegacyFixtureTests` 用 v0.1.6.2 真实存档验证逐位。
- **等响（Listen）配对**：`processListen` 的整块 + 小数子块配对公式推导正确（输出子块 [E−B, E) ↔ 输入 [E−L−B, E−L)，与 `current` 重叠 (1−f)·B、与 `previous` 重叠 f·B，`CeladonChain.cpp:418-433`）；静音门、边缘瞬态门、EMA 都合理；`ChainTests.cpp:593-671` 覆盖冻结 / 编辑 / 切换斜坡 / 旁通 / 重置 / 延迟变化 / 环丢失。
- **试听差值**：测点全在换信号之前，`differenceMix` 20 ms 线性交叉淡化，声道数不匹配时 `skip` 不卡住（`CeladonChain.cpp:248-292, 317-343`）；`AdvancedMasteringTests` 「never receive dither」覆盖三种监听不加抖动。
- **JUCE 9 API 用法**：`AudioPlayHead::getPosition` 新接口、`AudioParameterFloatAttributes`、`ParameterID` 版本号递增（mastering = 8）、`copyXmlToBinary`、`Timer`、`isNonRealtime`（原子）——没有过时或误用；`.jucer` 无 `pluginChannelConfigs`，`isBusesLayoutSupported` 生效；Release 才复制安装、Debug −O1（ADR-007）。

## 4. 看到但不报的（价值低或已被设计接受）

- 延迟真正变化到宿主得知之间最多一个定时器周期（≤ 33 ms）PDC 偏 ≤ 9 ms：ADR / 规格明确接受「上报的永远是实际延迟」的取舍，母带轨上不可闻。
- `AnalyzerTap::push` 在 FIFO 快满时只写前一部分、丢掉后一部分（`AnalyzerTap.h:53-76`）：只在界面卡住 > 170 ms（192 kHz）时让频谱仪闪一帧。
- `applyListenGain` 每样本每声道调一次 `getWritePointer`（`CeladonChain.cpp:387-392`）：只在 Listen 起作用时跑，微秒级。
- 只因宿主块长变化就整体重建（`PluginProcessor.cpp:401`）：会清整体响度与 Listen 估计，代价 1.5～3.7 ms；正常播放中宿主不改块长。
- 8 个 `master_*` 参数的范围写在 `Parameters.cpp:198-206` 而不是 `ParameterRanges.h`：都是带步进的线性范围，往返稳定，不触发 ADR-018 的问题；只是维护口径不一。
- `PluginProcessor` 不在 `celadon_tests` 里（`LookaheadHold`、缺参补节点、旁通宽限、侧链路由都只靠 `celadon_ui_snapshot` / 评审）：本报告的修法都放在链 / 模块层以便被现有 headless 测试覆盖；处理器级测试夹具是以后的事。

## 5. 实验数据

- 环境：107（`githubci`，Ubuntu，12 核，g++ 15.2.0），负载均值 18（其他代理在跑），所以驱动用 `nice -n 10` 编、只编一个文件；链接 `~/.cache/celadon-build/fable-deep-research-634a5e-d38dda489a84/` 里主进程为本工作树编出的 Release（`-O3 -DNDEBUG`）目标文件 `CMakeFiles/celadon_tests.dir/Source/dsp/*.o`（20 个）与 `libceladon_juce.a`；远端 `Source/dsp/CeladonChain.{h,cpp}`、`ReleaseSafety.cpp`、`ParameterRanges.h` 的 SHA-256 与本地逐一相同（`ac10bd87…`、`3f8d0dfc…`、`cd2d0e5e…`、`b84e0a3f…`）。编译选项与 CMake 一致（`-std=c++17 -ffp-contract=off -DCELADON_FP_CONTRACT_OFF=1 -DJUCE_*`）。
- 驱动：`r8probe.cpp`（第 6 节全文），48 kHz、块 256、立体声；受测链与参照链喂同一输入、同步做同样的切换，逐样本比增益（|参照| > 0.02 才比）。
- 实验目录 `~/.cache/celadon-t032/R8/` 做完已删除；没有跑 `scripts/test-linux.sh`，没有改远端同步树。

完整输出：

```text
== R8-01: monitoring gain continuity when the release preview stops ==
  [preview off, Listen off] steady preview gain = -6.00 dB
  preview off, Listen off            max single-sample gain step = 0.4990 (6.00 dB equiv) at sample 2400; gain range 0.5010 .. 1.0000
  [preview off, Listen on (handover)] steady preview gain = -6.00 dB
  preview off, Listen on (handover)  max single-sample gain step = 0.0057 (6.00 dB equiv) at sample 2406; gain range 1.0000 .. 1.9947
  [A/B original while previewing] steady preview gain = -6.00 dB
  A/B original while previewing      max single-sample gain step = 0.4990 (6.00 dB equiv) at sample 2400; gain range 0.5010 .. 1.0000
  [host bypass while previewing] steady preview gain = -6.00 dB
  host bypass while previewing       max single-sample gain step = 0.4990 (6.00 dB equiv) at sample 2400; gain range 0.5010 .. 1.0000
  [difference audition while previewing] steady preview gain = -6.00 dB
  difference audition while previewing max single-sample gain step = 0.4990 (6.00 dB equiv) at sample 2400; gain range 0.5010 .. 1.0000
  [offline while previewing (spec: immediate)] steady preview gain = -6.00 dB
  offline while previewing (spec: immediate) max single-sample gain step = 0.4990 (6.00 dB equiv) at sample 2400; gain range 0.5010 .. 1.0000

== R8-02a: NaN survives text round trip and range conversion (juce_core only) ==
  juce::String(NaN) = "nan"; String("nan").getFloatValue() isnan = 1; String("nan").getFloatValue() isnan = 1
  logRange(10..1000): to0to1(NaN) isnan=1, from0to1(NaN) isnan=1, snapToLegalValue(NaN) isnan=1
  linear(0..24 step .01): to0to1(NaN) isnan=1, from0to1(NaN) isnan=1, snapToLegalValue(NaN) isnan=1
  jlimit(0,1,NaN) isnan=1; roundToInt(NaN)=0; approximatelyEqual(NaN, 1.0f)=0
  Decibels::decibelsToGain(NaN) = 0

== R8-02b: NaN parameters handed to CeladonChain::setParams ==
  limit.releaseMs = NaN (autoRelease off)    nonFinite blocks =   0 / 375, second-half RMS = 0.7727, peak = 0.8913 (ceiling -1 dBTP = 0.8913), finite=1
  limit.releaseMs = NaN (autoRelease on)     nonFinite blocks =   0 / 375, second-half RMS = 0.6053, peak = 0.8561 (ceiling -1 dBTP = 0.8913), finite=1
  limit.gainDb = NaN                         nonFinite blocks =   0 / 375, second-half RMS = 0.0000, peak = 0.0000 (ceiling -1 dBTP = 0.8913), finite=1
  limit.ceilingDb = NaN                      nonFinite blocks =   0 / 375, second-half RMS = 0.0000, peak = 0.0000 (ceiling -1 dBTP = 0.8913), finite=1
  color.toneDb = NaN                         nonFinite blocks = 187 / 375, second-half RMS = 0.0000, peak = 0.0000 (ceiling -1 dBTP = 0.8913), finite=1
  color.outputDb = NaN                       nonFinite blocks =   0 / 375, second-half RMS = 0.0000, peak = 0.0000 (ceiling -1 dBTP = 0.8913), finite=1
  balance.recover = NaN (guarded)            nonFinite blocks =   0 / 375, second-half RMS = 0.6053, peak = 0.8561 (ceiling -1 dBTP = 0.8913), finite=1
  claySettings.lowCutHz = NaN, manual        nonFinite blocks =   0 / 375, second-half RMS = 0.6053, peak = 0.8561 (ceiling -1 dBTP = 0.8913), finite=1

== R8-03: OUT integrated LUFS while listening to the original / host bypass ==
  A/B original master 6 s = -14.00 LUFS; after 6 s of dry path = -16.03; back on master +6 s = -15.27; +18 s = -14.71 (input tone alone = -20.00)
  host bypass  master 6 s = -14.00 LUFS; after 6 s of dry path = -16.03; back on master +6 s = -15.27; +18 s = -14.71 (input tone alone = -20.00)

== R8-04: preview gain after a meter Reset (requestMeterReset) while previewing ==
  steady preview gain = -6.00 dB, integrated = -8.00 LUFS
    t =   50 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  100 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  150 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  200 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  250 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  300 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  350 ms: preview gain   0.00 dB, integrated -100.00 LUFS
    t =  400 ms: preview gain  -0.18 dB, integrated   -7.99 LUFS
    t =  450 ms: preview gain  -6.01 dB, integrated   -7.99 LUFS
    t =  500 ms: preview gain  -6.01 dB, integrated   -7.99 LUFS
    ...（其后一直 -6.01 dB）
  after Reset: max preview gain = 0.00 dB; 8 x 50 ms segments within 0.5 dB of unity
```

读法：R8-01 里「gain range 0.5010 .. 1.0000」= 切换前一块（仍预听）比值 0.501，切换后 1.000，跳变落在切换样本 2400（切换前录了 50 ms）；Listen 开着那组比值从 2 降到 1 是因为参照链一直开着 Listen（−12 dB），受测链把 −6 dB 的预听增益交给 Listen 再平滑到 −12 dB。R8-02b 里 0.6053 / 0.8561 是这条参数组合的正常值（+24 dB 推响、限幅后）；0.7727 / 0.8913 表示只剩硬钳位在削。

## 6. 实验驱动源码（可复现；链接方法见第 5 节）

```cpp
// R8 实验驱动（T-032）：链接已有的 Release 目标文件，量三件事：
//   1. 预听（归一化预听）关闭 / 切到干路 / 试听差值时，送给宿主的监听增益是否有单样本阶跃（R8-01）；
//   2. NaN 参数经 juce::String 往返、NormalisableRange 换算是否原样穿透（R8-02）；
//   3. NaN 参数交给 CeladonChain::setParams 后链的表现（R8-02）；另有 R8-03 / R8-04（见 main 末尾）。
#include "dsp/CeladonChain.h"
#include "ParameterRanges.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
using namespace celadon;
namespace
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    struct Sig { std::vector<float> l, r; };
    Sig sine (double fs, double f, float amp, double seconds, double phase = 0.0)
    {
        Sig s; const auto n = (size_t) std::lround (fs * seconds); s.l.resize (n); s.r.resize (n);
        for (size_t i = 0; i < n; ++i) { const auto v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * f * (double) i / fs + phase); s.l[i] = v; s.r[i] = v; }
        return s;
    }
    Sig run (CeladonChain& chain, const ChainParams& p, bool hostBypass, bool compare, bool audition, const Sig& in, int block, int* nonFinite = nullptr)
    {
        Sig out; juce::AudioBuffer<float> buf (2, block);
        for (size_t pos = 0; pos < in.l.size(); pos += (size_t) block)
        {
            const auto n = (int) std::min ((size_t) block, in.l.size() - pos);
            buf.setSize (2, n, false, false, true);
            for (int i = 0; i < n; ++i) { buf.setSample (0, i, in.l[pos + (size_t) i]); buf.setSample (1, i, in.r[pos + (size_t) i]); }
            chain.setParams (p, hostBypass, compare, audition);
            const auto bad = chain.process (buf);
            if (nonFinite != nullptr && bad) ++*nonFinite;
            for (int i = 0; i < n; ++i) { out.l.push_back (buf.getSample (0, i)); out.r.push_back (buf.getSample (1, i)); }
        }
        return out;
    }
    void gainTrace (const Sig& test, const Sig& ref, const char* name)   // gain_i = test_i / ref_i（|ref| > 0.02），报最大相邻跳变
    {
        double maxStep = 0.0, gMin = 1e9, gMax = -1e9, prev = std::numeric_limits<double>::quiet_NaN(); size_t stepAt = 0;
        for (size_t i = 0; i < test.l.size(); ++i)
        {
            if (std::abs (ref.l[i]) < 0.02f) continue;
            const auto g = (double) test.l[i] / (double) ref.l[i];
            if (! std::isnan (prev) && std::abs (g - prev) > maxStep) { maxStep = std::abs (g - prev); stepAt = i; }
            prev = g; gMin = std::min (gMin, g); gMax = std::max (gMax, g);
        }
        std::printf ("  %-34s max single-sample gain step = %.4f (%.2f dB equiv) at sample %zu; gain range %.4f .. %.4f\n",
                     name, maxStep, 20.0 * std::log10 (std::max (1e-9, gMax / std::max (1e-9, gMin))), stepAt, gMin, gMax);
    }
    Sig concat (const Sig& a, const Sig& b) { Sig c = a; c.l.insert (c.l.end(), b.l.begin(), b.l.end()); c.r.insert (c.r.end(), b.r.begin(), b.r.end()); return c; }
    void prepare (CeladonChain& chain, double fs, int block, const ChainParams& p) { chain.setParams (p, false); chain.prepare ({ fs, (juce::uint32) block, 2 }); }
}
int main()
{
    constexpr double fs = 48000.0; constexpr int block = 256;
    std::printf ("== R8-01 ==\n");
    {
        ChainParams p; p.limit.gainDb = 12.0f; p.limit.ceilingDb = -1.0f; p.mastering.previewTargetLufs = -14.0f;
        const auto settle = sine (fs, 1000.0, 0.10f, 4.0);
        const auto pre    = sine (fs, 1000.0, 0.10f, 0.05, juce::MathConstants<double>::halfPi);   // 切换前 50 ms（仍在预听）
        const auto probe  = sine (fs, 1000.0, 0.10f, 0.50, juce::MathConstants<double>::halfPi);   // 余弦起步：边界样本 |ref| = 0.1
        struct Case { const char* name; bool listen; int leave; };   // 0 关预听 / 1 原声 / 2 宿主旁通 / 3 试听差值 / 4 离线
        const Case cases[] = { { "preview off, Listen off", false, 0 }, { "preview off, Listen on (handover)", true, 0 },
                               { "A/B original while previewing", false, 1 }, { "host bypass while previewing", false, 2 },
                               { "difference audition while previewing", false, 3 }, { "offline while previewing (spec: immediate)", false, 4 } };
        for (const auto& c : cases)
        {
            auto params = p; params.limit.listen = c.listen;
            CeladonChain test, ref; prepare (test, fs, block, params); prepare (ref, fs, block, params);
            run (test, params, false, false, false, settle, block); run (ref, params, false, false, false, settle, block);
            test.setReleasePreviewEnabled (true);
            run (test, params, false, false, false, settle, block); run (ref, params, false, false, false, settle, block);
            const auto preA = run (test, params, false, false, false, pre, block), preB = run (ref, params, false, false, false, pre, block);
            std::printf ("  [%s] steady preview gain = %.2f dB\n", c.name, test.getMasteringStatus().previewGainDb);
            const bool bypass = c.leave == 2, compare = c.leave == 1, audition = c.leave == 3;
            if (c.leave == 0) test.setReleasePreviewEnabled (false);
            if (c.leave == 4) { glaze::HostInfo off; off.offline = true; test.setHostInfo (off); ref.setHostInfo (off); }
            const auto a = run (test, params, bypass, compare, audition, probe, block), b = run (ref, params, bypass, compare, audition, probe, block);
            gainTrace (concat (preA, a), concat (preB, b), c.name);
        }
    }
    std::printf ("\n== R8-02a ==\n");
    {
        const auto nan = kNaN; const juce::String text (nan);
        std::printf ("  juce::String(NaN) = \"%s\"; String(\"nan\").getFloatValue() isnan = %d\n", text.toRawUTF8(), (int) std::isnan (juce::String ("nan").getFloatValue()));
        auto rel = param::ranges::limitRelease(); auto lin = param::ranges::limitGain();
        std::printf ("  logRange: to0to1(NaN) isnan=%d, from0to1(NaN) isnan=%d, snap(NaN) isnan=%d\n", (int) std::isnan (rel.convertTo0to1 (nan)), (int) std::isnan (rel.convertFrom0to1 (nan)), (int) std::isnan (rel.snapToLegalValue (nan)));
        std::printf ("  linear:   to0to1(NaN) isnan=%d, from0to1(NaN) isnan=%d, snap(NaN) isnan=%d\n", (int) std::isnan (lin.convertTo0to1 (nan)), (int) std::isnan (lin.convertFrom0to1 (nan)), (int) std::isnan (lin.snapToLegalValue (nan)));
        std::printf ("  jlimit(0,1,NaN) isnan=%d; roundToInt(NaN)=%d; approximatelyEqual(NaN,1)=%d; decibelsToGain(NaN)=%g\n",
                     (int) std::isnan (juce::jlimit (0.0f, 1.0f, nan)), juce::roundToInt (nan), (int) juce::approximatelyEqual (nan, 1.0f), juce::Decibels::decibelsToGain (nan));
    }
    std::printf ("\n== R8-02b ==\n");
    {
        struct Case { const char* name; void (*poke) (ChainParams&); };
        const Case cases[] = {
            { "limit.releaseMs = NaN (autoRelease off)", [] (ChainParams& p) { p.limit.autoRelease = false; p.limit.releaseMs = kNaN; } },
            { "limit.releaseMs = NaN (autoRelease on)",  [] (ChainParams& p) { p.limit.autoRelease = true; p.limit.releaseMs = kNaN; } },
            { "limit.gainDb = NaN",                      [] (ChainParams& p) { p.limit.gainDb = kNaN; } },
            { "limit.ceilingDb = NaN",                   [] (ChainParams& p) { p.limit.ceilingDb = kNaN; } },
            { "color.toneDb = NaN",                      [] (ChainParams& p) { p.color.toneDb = kNaN; } },
            { "color.outputDb = NaN",                    [] (ChainParams& p) { p.color.outputDb = kNaN; } },
            { "balance.recover = NaN (guarded)",         [] (ChainParams& p) { p.balance.recover = kNaN; } },
            { "claySettings.lowCutHz = NaN, manual",     [] (ChainParams& p) { p.claySettings.lowCutMode = ClayMode::manual; p.claySettings.lowCutHz = kNaN; } } };
        const auto tone = sine (fs, 1000.0, 0.10f, 2.0);
        for (const auto& c : cases)
        {
            ChainParams p; p.limit.gainDb = 24.0f; c.poke (p);
            CeladonChain chain; prepare (chain, fs, block, p);
            int bad = 0; const auto out = run (chain, p, false, false, false, tone, block, &bad);
            double rms = 0.0, peak = 0.0; bool finite = true;
            for (size_t i = out.l.size() / 2; i < out.l.size(); ++i) { rms += (double) out.l[i] * out.l[i]; peak = std::max (peak, (double) std::abs (out.l[i])); finite = finite && std::isfinite (out.l[i]); }
            rms = std::sqrt (rms / (double) (out.l.size() / 2));
            std::printf ("  %-42s nonFinite blocks = %3d / %3zu, second-half RMS = %.4f, peak = %.4f, finite=%d\n", c.name, bad, tone.l.size() / block, rms, peak, (int) finite);
        }
    }
    std::printf ("\n== R8-03 ==\n");
    {
        ChainParams p; p.limit.gainDb = 6.0f; p.limit.ceilingDb = -1.0f;
        const auto tone = sine (fs, 1000.0, 0.10f, 6.0);
        for (int mode = 0; mode < 2; ++mode)
        {
            CeladonChain chain; prepare (chain, fs, block, p);
            run (chain, p, false, false, false, tone, block);                const auto master = chain.getOutputIntegratedLufs();
            run (chain, p, mode == 1, mode == 0, false, tone, block);        const auto during = chain.getOutputIntegratedLufs();   // 原声 / 宿主旁通 6 s
            run (chain, p, false, false, false, tone, block);                const auto after = chain.getOutputIntegratedLufs();
            run (chain, p, false, false, false, tone, block); run (chain, p, false, false, false, tone, block);
            std::printf ("  %-12s master = %.2f; after dry = %.2f; back +6 s = %.2f; +18 s = %.2f\n", mode == 0 ? "A/B original" : "host bypass", master, during, after, chain.getOutputIntegratedLufs());
        }
    }
    std::printf ("\n== R8-04 ==\n");
    {
        ChainParams p; p.limit.gainDb = 12.0f; p.limit.ceilingDb = -1.0f; p.mastering.previewTargetLufs = -14.0f;
        const auto tone = sine (fs, 1000.0, 0.10f, 4.0);
        CeladonChain chain; prepare (chain, fs, block, p);
        run (chain, p, false, false, false, tone, block); chain.setReleasePreviewEnabled (true); run (chain, p, false, false, false, tone, block);
        std::printf ("  steady preview gain = %.2f dB, integrated = %.2f LUFS\n", chain.getMasteringStatus().previewGainDb, chain.getOutputIntegratedLufs());
        chain.requestMeterReset();
        const auto probe = sine (fs, 1000.0, 0.10f, 0.05); double maxGainDb = -100.0; int blocksAtUnity = 0;
        for (int i = 0; i < 40; ++i)
        {
            run (chain, p, false, false, false, probe, block);
            const auto g = chain.getMasteringStatus().previewGainDb; maxGainDb = std::max (maxGainDb, (double) g); if (g > -0.5f) ++blocksAtUnity;
            std::printf ("    t = %4d ms: preview gain %6.2f dB, integrated %7.2f LUFS\n", (i + 1) * 50, g, chain.getOutputIntegratedLufs());
        }
        std::printf ("  after Reset: max preview gain = %.2f dB; %d x 50 ms segments within 0.5 dB of unity\n", maxGainDb, blocksAtUnity);
    }
    return 0;
}
```

编译（在与 `Celadon/` 同级放 `JUCE/` 的同步目录里，`$B` 为 Release 构建目录）：

```bash
g++ -std=c++17 -O2 -ffp-contract=off -DNDEBUG=1 -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DJUCE_MODULE_AVAILABLE_juce_core=1 -DJUCE_MODULE_AVAILABLE_juce_audio_basics=1 \
    -DJUCE_MODULE_AVAILABLE_juce_audio_formats=1 -DJUCE_MODULE_AVAILABLE_juce_dsp=1 -DCELADON_FP_CONTRACT_OFF=1 \
    -isystem JUCE/modules -I Celadon/Source r8probe.cpp "$B"/CMakeFiles/celadon_tests.dir/Source/dsp/*.o "$B"/libceladon_juce.a -lpthread -ldl -lrt -o r8probe
```

## 7. 建议的落地顺序（给综合阶段）

1. R8-01 与 R8-02 各 ≤ 1 h、都能用现有 headless 测试类验，且互不相干（`CeladonChain::setParams` 交接分支 vs `LimiterModule / ColorModule::setParams` 守卫 + `readParams`），可分两个代理并行；
2. R8-03 要动 `LoudnessMeter` 接口 + 链 + 规格一句，与 R7（分析器与计量）协调文件归属；
3. R8-04 改 `ReleaseSafety` 与规格 §6 一句，等 Max 确认「保持上次增益」这个口径后做。
