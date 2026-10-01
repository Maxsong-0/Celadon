# T-038 调研 B：聆听窗口不代表整首歌——技术路线与可行性

> 2026-10-01 · 调研代理 B · 范围：ARA2、宿主拍点、门控学习、段落 / drop 检测、可落地方案。A（商业产品的学习区段做法）与 C（曲风目标、平台数值）不在本文范围。
> 本文分两个文件：本文件放结论摘要和第 1–3 节；第 4–5 节、缺口清单和全部出处（[B1]…[B60] 的链接）在同目录 `B-technical-2.md`。
> 标注约定：「推断」是我的推理；「二手」是非官方文章；「社区」是论坛帖；其余都是一手资料（官方文档、SDK 源码、标准原文、论文原文）。

## 结论摘要

1. **ARA 插件挂在主输出 / 母带总线上拿不到整首混音。** ARA 插件没有实时输入，只能随机读取宿主里的"音频素材"，而且必须排在这条轨（或这个事件）处理链的第一位 [B1][B8][B10][B7]。想用 ARA 读整首，只有一种用法：把混好的立体声成品作为片段放在一条音频轨上，Celadon 插在这条轨的第一个槽位。在混音工程里挂 master bus 的用法用不上 ARA。
2. ARA 宿主覆盖面：Logic（Apple Silicon 上要用 Rosetta 打开）、Cubase / Nuendo、Studio One（Fender Studio Pro）、REAPER（只认 VST3）、Cakewalk Sonar、Luna、DP 支持；Ableton Live 不支持；FL Studio 不支持（依据是二手和社区资料，没有官方声明）；Bitwig 没找到支持的记录 [B4][B9][B14][B16][B17]。JUCE 从 7.0.0 起支持 ARA（VST3 和 AU 都行）；JUCE 9.0.2 要求 ARA SDK 2.3.0；SDK 用 Apache-2.0 许可 [B23][B24][B25][B2]。不过在 ARA 模式下，插件得自己渲染片段音频，也不应再用宿主参数 [B1]，Celadon 要做的改动很大。
3. 宿主拍点：JUCE 的 PositionInfo 能拿到 BPM、PPQ、上一小节起点、拍号、是否播放、是否循环、采样位置，但每一项都可能缺失（Optional）；isNonRealtime 来自 VST3 的 kOffline 或 AU 的 OfflineRender [B28][B30][B31][B29]。可靠性有已知缺口：Live 11 的 VST3 导出时不报离线；FL 要在插件包装器里勾选选项才报；部分宿主不给上一小节起点 [B36][B35][B34]。按小节定窗做得到，但**工程速度不一定等于音频速度**（推断），必须拿音频本身的节拍做交叉验证。
4. 门控学习有标准依据可循：BS.1770 的相对门、Tech 3342 的 95 分位、R128 s1 的最大短期响度、ReplayGain 1.0 的"取 95% 处" [B39][B43][B41][B44]。Tech 3341 还规定，LRA 测量的前 60 s 必须标"尚不稳定" [B42]，所以 15–30 s 学出来的 LRA 本来就不可信。没找到专门讲"只用最响段学母带画像"的论文。
5. 现有 drop / 段落检测论文都是离线、看整首的：Yadati 2014（容差 3 s 时 F1 为 0.61）、Zehren 2020（精度 86%、召回 49%，用 8 小节棋盘核）[B46][B47]。Foote 新颖度要看未来半个核长的音频 [B48]。改成只看过去的因果版本做得到，代价是确认要晚约 4 小节（124 BPM 下约 7.7 s，推断）。
6. 推荐排序：① 门控画像 + 认出满编段后重学（所有宿主可用，不增加延迟，CPU 开销可忽略）> ② 按小节和曲风设最低聆听长度，没听到满编段不锁定 > ③ 离线渲染两遍（顺带出俱乐部版和流媒体版两版）> ④ 整首预分析（先做拖入成品文件，ARA 留到以后再说）。

## 1 ARA2：能不能拿到整轨，挂在哪里才有用

### 1.1 ARA 解决什么问题（文档原文写明）
- ARA 的首要目标是让插件**不依赖渲染回调**、直接从宿主读取音频。宿主提供三样东西：素材的随机读取、编排信息（哪段素材在什么时间播放）、音乐上下文（速度、和声）[B1 §1.2]。
- 读取接口是同步调用，可能阻塞，适合在主线程或**后台分析任务**里用，不能在实时线程里调。实时渲染需要的样本，由插件自己的非实时缓存线程提前读好 [B1 §2.10]。
- 读取和分析是"离线"的，渲染仍然实时。ARA 文档专门拿它跟 AudioSuite / Offline AU 做过对比 [B1 §1.2]。
- Celemony 帮助中心：在 ARA 模式下，Melodyne 总是分析 region 引用的**整个**音频文件，即使 region 已被裁短 [B6][B7]。非 ARA 插件只能看到正在播放的那一小段，Celemony 自己比喻成"隔着钥匙孔看"，所以只能靠 Transfer 边播边录 [B5]。Celadon 现在就是这个处境。

### 1.2 挂在哪里才有用：母带总线不行
ARA 文档写明的约束：
- ARA 插件**没有实时输入**，它靠随机读取一份"概念上静态"的输入（音频源），适合放在轨道处理线程上 [B1 §2.8]。
- 它必须是**串行渲染链里的第一个插件**，排在 region 的淡入淡出、音量、静音之前。几个 ARA 效果不能串着用，除非先冻结或 bounce [B1 §2.8]。
- 播放渲染器不处理实时输入，**实时参数变化也算在内**。ARA 激活时，不应再使用伴随 API（VST3 / AU）的参数 [B1 §2.9]。
- "可选的侧链和实时输入"被列为**以后**才可能加的功能 [B1 §3.13]。

宿主文档：
- Logic：在**音频通道条的第一个 Audio Effect 插槽**里选插件的"(ARA)"版本。插入后该轨的 Flex 被禁用；要先按一次播放，宿主才把信息传给插件 [B8]。
- Cubase / Nuendo：作为"扩展"激活在音频事件或音频轨上，音频 part 会被忽略 [B10]。
- REAPER：可以放在 Track FX 或 Item FX。ARA 插件必须占第一个槽位，插到别的位置会被自动挪到第一位 [B7]。REAPER 只支持 VST3 格式的 ARA [B11]。
- 社区（二手）：JUCE 论坛 2022 年的一条回答说，ARA 插件永远排在效果链第一位，拿不到效果链末端的音频；回答者也没见过哪个宿主能对整个工程提供 ARA [B19]。

**结论**：master 或母带总线上流的是多轨实时相加后的信号，不是"音频源"，ARA 规范里根本没有这路输入。这一点是从上面几份一手文档推出来的，把握很大。Celadon 挂在 master bus 上时，在任何宿主里都只会当普通 VST3 / AU 插件用。想用 ARA 读整首，唯一的办法是：**把成品立体声文件作为片段放在音频轨上，Celadon 以 ARA 方式插在这条轨的第一位**，也就是单独开一个母带工程的那种做法。

### 1.3 对 Celadon 意味着什么
- ARA 模式下，Celadon 自己就成了这个片段的播放渲染器：输出得由插件按 playback region 从音频源读出样本、再自己生成。JUCE 提供了 `ARAAudioSourceReader`（一般包在 BufferingAudioReader 里，衔接实时渲染）和 `ARAPlaybackRegionReader` [B26]。渲染接口带一个 `realtime` 标志；实时模式下样本没能及时读到，渲染就可能失败 [B27]。
- 宿主可能给每个 playback region 单独建一个渲染实例（Cubase / Nuendo 就是这样）；Studio One 早期则是一个实例兼任所有角色。插件两种情况都要支持，播放渲染也可能离线、也可能实时进行 [B1 §2.9]。
- 参数：ARA 模式下不应使用宿主参数 [B1 §2.9]。所以 GLAZE 旋钮、风格选择、"重新聆听"这些状态，得改存到 ARA 的文档归档里（推断）。
- Logic + Apple Silicon：Logic 原生运行时 AU 插件跑在 Logic 进程外，这种方式"还不完全支持" ARA，要改用 Rosetta 打开 Logic [B4][B6]。Apple 自己也说，在 Apple silicon 上用 Rosetta 打开 Logic 的唯一理由就是用 ARA 模式的第三方插件；而在 Rosetta 下，Stem Splitter、ChromaGlow 和大部分 Mastering Assistant 预设都用不了 [B9]。所以要走这条路，Celadon 得带 x86_64 版本（推断）。Logic 更新记录一直写到 12.3.1（2026-09），其中有 ARA 的修复和 Rosetta 模式的修复，但没写原生 Apple silicon 已支持 ARA [B56]。由此推断，到 12.x 仍然要用 Rosetta。
- ARA 文档要求插件在多个 ARA 宿主上反复测，因为各宿主的调用顺序差别很大 [B1 §3.1.4]。

### 1.4 JUCE 的支持情况
- JUCE **7.0.0** 的更新记录里写了"加入 ARA SDK 支持" [B23]。ARA 文档说，JUCE 7 团队审阅了 Celemony 与 SoundRadix 合作的实验分支，然后做成了正式集成 [B1 §3.2.4]。
- CMake：`juce_add_plugin(... IS_ARA_EFFECT TRUE)` 会在 **VST3 和 AU** 包装器里打开 ARA 代码路径（定义 `JucePlugin_Enable_ARA=1`）。相关参数还有 `ARA_FACTORY_ID`、`ARA_DOCUMENT_ARCHIVE_ID`、`ARA_ANALYSIS_TYPES`、`ARA_TRANSFORMATION_FLAGS`；用 `juce_set_ara_sdk_path()` 指向本地的 SDK [B25]。
- 平台：AU 只在 Mac 上有；VST3 跨平台。Windows 上能用是推断：JUCE 文档只按插件格式区分，而 Celemony 列出了好几款 Windows 上的 ARA-VST3 宿主 [B4]。
- 版本：JUCE 9.0.2 的 BREAKING_CHANGES 要求 ARA SDK **2.3.0**，用旧版会编译失败；这条要求写在 8.0.13 那一节 [B24]。SDK 2.3.0 于 2025-11-06 发布 [B3]。
- 许可：ARA SDK 用 **Apache License 2.0** 发布。公开发布的 ARA 软件应基于 SDK 带 tag 的正式版本；VST3、AU、AAX、CLAP 这些伴随 API 各有自己的许可 [B2]。因此不需要另外跟 Celemony 签许可（推断，依据是 Apache-2.0）。ARA 的 AAX 集成是 AAX SDK 里的可选部分 [B2]，与 Celadon 无关。

### 1.5 宿主矩阵（Celadon 只有 VST3 / AU）
| 宿主 | ARA | 走哪种格式 | 挂载位置 / 备注 | 出处 |
|---|---|---|---|---|
| Logic Pro | 支持 ARA 2 | AU | 音频通道条第一个效果槽；Apple Silicon 上要用 Rosetta | B8 B4 B9 |
| Cubase / Nuendo | 支持 | VST3 | 挂在音频事件或音频轨上，作为"扩展"；Celemony 测过 Cubase 15.0.20、Nuendo 14.0.40 | B4 B10 |
| Studio One / Fender Studio Pro | 支持 | VST3 | Celemony 现在的页面写作 Fender Studio Pro 8.0（它与 Studio One 的关系是推断）；社区说按 region 管理 | B4 B19 |
| REAPER | 支持（5.97 起） | 只认 VST3 | 放在 Track FX / Item FX 的第一位；在首选项里可开关，默认开 | B7 B11 |
| Pro Tools | 支持（ARA-AAX） | AAX | Celadon 没有 AAX 版，用不了；2022.9 版首次加入（二手） | B4 B15 |
| Cakewalk Sonar | 支持 | VST3 | Celemony 测的是 2025.11 版、只在 Windows；挂载细节没查到 | B4 |
| Ableton Live | 不支持 | — | Celemony 只把 Live 12.2 列为普通 VST3 宿主，Live 下要用 Transfer | B4 B5 B14 |
| FL Studio | 不支持（没有官方声明） | — | 插件厂商 2024 年的博客列为不支持（二手）；官方论坛到 2026-09 还有用户在请求加 ARA（社区）；Celemony 列表里也没有 | B14 B16 B4 |
| Bitwig Studio | 没找到支持记录 | — | 6.1 新功能页和 5.3 手册里都没有 ARA；Celemony 列表里也没有 | B17 B18 B4 |
| Luna Pro | 支持 | VST3 | Celemony 测的是 2.0.1 版；挂载细节没查到（UA 帮助中心拒绝抓取） | B4 |
| Digital Performer | 支持 | VST3 | Celemony 测的是 DP 11.34 | B4 |

另外，Samplitude、Sequoia、Mixcraft 也在 Celemony 的 ARA-VST3 列表里 [B4]。注意：这份列表是"Melodyne 测试过的配置"，不是各宿主厂商自己的声明。

### 1.6 用 ARA 做整段分析的实例
- NoiseWorks **DynAssist**（Pro Tools 上的 ARA 插件）：在播放前离线分析整个 clip，几秒内完成，用来自动推人声电平和做门限；它还声称能"预判"后面的响段（Avid 官方资源中心）[B13]。
- **WaveLab Pro 12** 可以作为 ARA 扩展嵌进 Cubase 13、Nuendo 13、Studio One 6.5、Reaper 6.82（官方支持列表）[B12]。它是母带软件，但嵌进去的是编辑器，不是实时母带链。
- iZotope RX（Music Rebalance / Spectral Editor）、SpectraLayers、sonible prime:vocal、Acon Acoustica 等（二手）[B14][B22]。
- **没查到**：在 master 或混音总线上用 ARA 做"整首母带画像"的商业母带插件。sonible 博客里提到的"ARA 归一化插件"只是举例设想，不是真有的产品 [B14]。

### 1.7 对照：AAX AudioSuite（Celadon 不做 AAX，仅作对照）
AAX 的 Host Processor 接口提供"类似旧 AudioSuite 架构"的离线处理。宿主对时间线上的选区，先跑一遍可选或必需的**分析遍**（AnalyzeAudio），再跑**渲染遍**（RenderAudio）。插件可以在当前处理区内随机读取时间线上的音频（GetAudio），但读不到处理区以外的样本 [B20][B21]。总结起来就是"整段选区、跑两遍、生成新的音频文件"。ARA 则是"读取和分析离线做、渲染实时、不改原文件" [B1 §1.2]。

## 2 宿主拍点信息：JUCE 的 PositionInfo

### 2.1 能拿到什么
JUCE 写明：不是所有宿主和格式都能提供全部时间信息；大部分 getter 返回 Optional，插件要做好拿不到的准备；`getPosition()` 只能在 `processBlock()` 里调用 [B28]。VST3 的 ProcessContext 里这些字段也都标为可选，靠 state 标志位声明是否有效 [B32]。

| PositionInfo 字段 | 含义 | VST3 来源（JUCE 包装器） | AU 来源（JUCE 包装器） |
|---|---|---|---|
| getBpm | 速度 | tempo，需 kTempoValid | CallHostBeatAndTempo |
| getPpqPosition | 当前位置（以四分音符计） | projectTimeMusic，需 kProjectTimeMusicValid | CallHostBeatAndTempo |
| getPpqPositionOfLastBarStart | 上一小节起点（以四分音符计） | barPositionMusic，需 kBarPositionValid | CallHostMusicalTimeLocation 给的当前小节强拍位置 |
| getTimeSignature | 拍号 | 需 kTimeSigValid | CallHostMusicalTimeLocation |
| getBarCount | 从时间线起点算的小节数 | JUCE 注释：不是所有宿主 / 格式都有；VST3 和 AU 包装器都没填（据源码） | 同左 |
| getIsPlaying / getIsRecording | 是否走带 / 录音 | kPlaying / kRecording | CallHostTransportState |
| getIsLooping / getLoopPoints | 是否循环 / 循环起止（以四分音符计） | kCycleActive / kCycleValid | CallHostTransportState |
| getTimeInSamples / Seconds | 当前采样位置 | projectTimeSamples | CallHostTransportState，失败时退回时间戳 |

来源：[B28][B30][B31][B32]。JUCE 注释还特别写明，上一小节起点在某些宿主里拿不到，例如 Pro Tools [B28]。

**isNonRealtime**：为真表示正在离线渲染；宿主没说明时按实时处理；在 prepareToPlay 之前可能不准，而且每次 prepareToPlay 都可能变 [B29]。VST3 包装器在 setupProcessing 和每次 process 时，按 `processMode == kOffline` 来设置它 [B30]。VST3 还有一种 kPrefetch 模式 [B33]，JUCE 不把它算作离线。AU 包装器在宿主设置 `kAudioUnitProperty_OfflineRender` 时切换，并重新调用 prepareToPlay [B31]。

### 2.2 各宿主是否可靠（只列有出处的）
| 宿主 | 观察到的情况 | 资料性质 | 出处 |
|---|---|---|---|
| 多个宿主 | 有的宿主不给 barPositionMusic；用 PPQ 推算小节号的代码，得假设上一小节以来拍号和速度都没变 | 社区（Steinberg 论坛上的开发者，2017） | B34 |
| Ableton Live 11（VST3） | 导出时插件收不到离线标志；JUCE 团队成员认定是 Live 的 VST3 实现问题，说没有已知的绕过办法 | 社区（2021） | B36 |
| FL Studio | 要在插件包装器里勾选"Notify about rendering mode"，插件才知道正在渲染；循环位置由包装器自己算，FL 至少循环一次后才有 | 官方手册 | B35 |
| REAPER | 默认在离线渲染时设离线标志，用户可以手动改掉；2016 年有人得先打开某个设置才生效 | 社区（2020 / 2016） | B37 B38 |
| Cubase 8 | 2016 年有报告说 bounce 结束后它才变成真（版本很老） | 社区 | B38 |
| Pro Tools | 上一小节起点可能拿不到 | JUCE 源码注释 | B28 |
| Logic | 报给插件的 BPM 只保留 3 位小数（120.1234 变成 120.1230）；同一帖里开发者说 PPQ 和上一小节起点准确 | 社区（2023） | B57 |
| Logic | 离线 bounce 时会设 OfflineRender，但不会重新查询插件延迟，所以离线时改延迟会导致不同步 | 社区（2016） | B58 |
| 多个宿主（含 REAPER） | 有速度变化时，用 ppq / bpm 换算秒数会跳变；开始播放时宿主可能先预卷；在 processBlock 里自己累加采样数最可靠 | 社区（2024） | B59 |

Logic 只找到社区资料。Studio One、Bitwig、Cakewalk、Luna、DP 这几家在拍点字段和离线标志上可不可靠：**没查到一手资料**。

### 2.3 按"小节数"定聆听长度、在小节边界切窗，做不做得到
- 先算量级：124 BPM、4/4 拍时一小节是 1.935 s。GLAZE 现在的 15–30 s 约合 7.8–15.5 小节，**整段都落在 16–32 小节（31–62 s）的 DJ 前奏里**。64 小节约 124 s。
- 做法（推断）：每个处理块用 PPQ 和拍号算出当前在第几小节、小节内什么位置（优先用"上一小节起点"；拿不到时，假设从 0 开始拍号不变，直接用 PPQ 推）；特征按 1 / 4 / 8 小节汇总；聆听长度改用小节数表达，House 模式下至少要盖过前奏。
- 风险（推断）：
  1. **工程速度 ≠ 音频速度**：新建工程、导入成品做母带、又没设工程速度时，PPQ 算出的小节和音乐里的小节对不上。只有挂在混音工程 master bus 上时（工程速度就是歌曲速度），这些信息才天然可靠。对策：用音频本身的节拍跟踪（例如能实时跑的 BTrack [B52]）估出速度，和宿主 BPM 比对，对不上就退回按秒计。
  2. 中途变速或变拍号：按 PPQ 推出的小节号会错位 [B34]。
  3. 循环播放：同一段会被反复累计。isLooping 为真、并且走过循环终点之后，不应再重复计入。
  4. 没在播放（isPlaying 为假）时不累计；聆听时长用 processBlock 里自己累加的采样数来算，不用 ppq / bpm 去换算 [B59]；还要识别从歌曲中间开始的离线渲染（起点不在 0）。

## 3 门控学习：标准怎么定义，能不能只用最响段学画像

### 3.1 标准原文要点
- **ITU-R BS.1770-5（2023-11）**：先做 K 加权，再算各声道均方、按声道加权求和；门控块长 400 ms，相邻块重叠 75%。门限分两级：绝对门 −70 LKFS；相对门 = 过了绝对门之后的响度再减 10 [B39]。
- **EBU Tech 3341（2023，第 4 版）**：Momentary（瞬时响度）用 0.4 s 滑动矩形窗，不门控；Short-term（短期响度）用 3 s 滑动窗，不门控，实时表至少每秒刷新 10 次；Integrated（整合响度）按 BS.1770 门控。响度表必须能显示瞬时和短期响度的**最大值**；**LRA 测量的前 60 s 必须标明"尚不稳定"** [B42]。
- **EBU Tech 3342（2023）**：LRA 的输入是 3 s 短期响度序列（每秒至少 10 个值，相邻窗至少重叠 2.9 s）。门限是级联的：绝对门 −70 LUFS，相对门 −20 LU。LRA = 第 10 百分位与第 95 百分位之差。取 10% 是为了不让歌曲结尾的淡出主导结果，取 95% 是为了不让单个异常响的声音主导结果 [B43]。
- **EBU R 128 s1（2020，短节目）**：短节目的 3 s 值太少，算 LRA 没有意义，所以改用"最大短期响度"来约束，上限 −18 LUFS（即 +5 LU）[B41]。R 128-2023 正文：节目响度归一到 −23 LUFS（广播场景）[B40]。

### 3.2 能不能只用"接近最响段"的帧来学画像
- 能做，而且有现成的"只看上半部分"的统计先例：ReplayGain 1.0 提案把 50 ms 的 RMS 值排序，取 **95%** 处的值代表整首歌的响度，作者说 95% 最贴近人耳感受 [B44]；Tech 3342 取的上分位也是 95% [B43]；R 128 s1 和 Tech 3341 里有"最大短期响度" [B41][B42]；TC Electronic 的术语表把 Max Loudness 称为"第二道防线"（厂商资料）[B45]。产品方面只举一例（细节归 A 报告）：iZotope Ozone 11 的帮助文档要求至少播放 8 秒、并且播放曲目最响的部分；分析参考文件时，它自动取"最响的 8 秒"来学目标 [B60]。
- BS.1770 自带的 −10 LU 相对门本来就是为了"只看前景声"（TC 术语表的说法）[B45]，但对 House 前奏来说太宽：只缺 bass 和 pad 的纯鼓前奏，大概率仍在门内（推断，需要用自己的曲库量出前奏和满编段的短期响度差）。
- **没查到**专门研究"用最响段门控来学母带画像"的论文。
- 落地草案（推断）：
  1. 在现有的 K 加权链上每 100 ms 记一次能量，滚动算出短期响度 S(t)（3 s 窗，每秒 10 个值）。
  2. 参考值 S_ref 取目前听过的 S 值里的第 95 百分位。
  3. 画像特征（频谱斜率、低频 / 高频占比、起音密度、PLR）只用 S(t) ≥ S_ref − ΔG 的帧来累计；ΔG 先取 2–4 LU，再用曲库标定。
  4. 涉及低频的决策再加一道"低频在场门"：40–120 Hz 的能量占比要不低于目前见过最大值的某个比例，免得用纯鼓前奏来定低频。
  5. 后面出现的新段落如果持续 N 个小节以上都高于 S_ref，旧的帧会自然落到门外，相当于自动重学（这一步与第 5 节方案 C 合并）。
  6. 整合响度和 LRA 仍按标准用全部帧计算；累计不到 60 s 时，LRA 标"不稳定"（借用 Tech 3341 的规定）。
  - 代价：每帧只多几次加法，再加一个环形缓冲（每秒 10 个值 × 600 s = 6000 个），CPU 开销可忽略，不增加延迟。
