# T-038 调研 B（续）：段落 / drop 检测、可落地方案、出处

> 接 `B-technical.md`（结论摘要与第 1–3 节）。标注约定相同：「推断」是我的推理，「二手」是非官方文章，「社区」是论坛帖。

## 4 段落 / drop 检测：算法要点、要多长上下文、能不能只看过去

### 4.1 专门针对 EDM 的研究
**Yadati、Larson、Liem、Hanjalic（ISMIR 2014）** [B46]
- drop 的定义：build 结束、**bassline 重新进入**的那一刻；一首歌里可能有好几个 drop。
- 方法分两步。① 先做无监督分段找候选点：用 Serrà 等人 2014 年的结构特征法，特征是 12 维 chroma，窗长 209 ms、步长 139 ms。分出的段界离真正 drop 平均 2.5 s，训练集里漏掉的 drop 不到 8%，平均段长 16.5 s。② 在每个段界附近取固定长度的窗提特征（频谱统计、13 维 MFCC（50 ms 窗、重叠 50%）、节奏特征），再用线性 SVM 判断是不是 drop。
- 数据：SoundCloud 上 100 首（dubstep、electro、house），按 60/20/20 分训练、验证、测试。F1：容差 3 s 时 0.61，容差 15 s 时 0.71；表现最差的那首只有 0.2–0.52，错误主要是误报，以及段界没对准 drop。
- 需要的上下文：整首歌；**只能离线做，要用到未来的音频**。

**Zehren、Alunno、Bientinesi（arXiv 2020，找 EDM 的 DJ 切入点）** [B47]
- 规则：EDM 几乎都是 4/4 拍，乐句长度是 2 的倍数个小节，**每 4 小节都可能开始新的段落**；结构上的变化总落在乐句的第一拍；切入后的那段音乐要能"单独撑得住"。
- 实现：用 7 个特征（kick / snare / hihat 的起音、HPSS 分出的谐波与打击能量、CQT、PCP），按强拍（第 1、3 拍）汇总：强拍是作者用 madmom 能可靠提取的最粗时间单位；EDM 里第 1 拍和第 3 拍很难可靠区分，所以没按小节汇总。用自相似矩阵 + **8 小节棋盘核**（前后各 4 小节）算新颖度；峰值要求是 4 小节窗口内的局部最大值，而且大于最大值的 0.3 倍；再挑出最合适的 4 小节周期相位；"撑得住"的判定是：之后 4 小节的谐波能量不低于最大值的 0.4 倍。
- 数据：134 首，平均时长 7 分 25 秒，速度 99–147 BPM。结果：精度 86%，召回 49%；MSAF（OLDA + PCP）和"把所有周期点都当候选"的精度都在 30% 左右；Mixed In Key 是 65%。漏检的原因里，45% 是周期估错，36% 是曲子本身不守 4 小节周期。
- 需要的上下文：整首歌；判断一个点要看**它之后的 4 小节**；离线方法。

### 4.2 通用的结构分段方法
- **Foote 新颖度**（Müller 的 FMP 讲义）：Δ(n) = Σ K(k,ℓ)·S(n+k, n+ℓ)，其中 k、ℓ 取 −L 到 L。核小，曲线噪声多、假峰多；核大，适合找大段落的边界；两端要补零，或者直接把开头和结尾各 L 帧置零 [B48]。所以第 n 帧的新颖度要等到第 n+L 帧才算得出来，**改成只看过去的版本，会晚半个核长**（由公式直接推出）。
- **MSAF**：开源框架，内置棋盘核（即 Foote 法）、OLDA、Laplacian、Convex NMF、结构特征、SI-PLCA、约束聚类等方法；默认用节拍同步的特征；看整首、离线；评测通常用 0.5 s 或 3 s 的命中窗 [B49]。
- **All-In-One**（WASPAA 2023）：一次给出速度、拍点、强拍和功能段落（intro / outro / break / bridge / verse / chorus 等）。输入是先做音源分离、再算出的各分轨频谱，模型用邻域注意力。在 RTX 4090 上处理 10 首共 33 分钟用了 73 s。训练数据是 Harmonix 数据集，标签里没有 drop 或 build 这一类。只能离线 [B54]。

### 4.3 拍点和强拍跟踪（用于定窗和对齐）
- **madmom**：`RNNBeatProcessor(online=True)` 配 `DBNBeatTrackingProcessor(online=True)`（用前向算法代替 Viterbi）可以实时跑 [B50]。强拍（downbeat）模块的处理器没有 online 参数、用 Viterbi 解码，所以只能离线（据源码推断）[B51]。
- **BTrack**：只看过去的拍点跟踪，C++ 写的，GPLv3 许可（商用要注意许可）[B52]。
- **BeatNet**（ISMIR 2021）：只看过去的 CRNN，加两个粒子滤波，能实时同时跟拍点、强拍和拍号；在 GTZAN 上胜过其他实时系统，接近离线基线（我只读到了摘要）[B53]。

### 4.4 实时变点检测
- **BOCPD**（Adams 与 MacKay 2007）：实时、精确地估计"距离上一个变点已经过了多久"的后验分布，适合逐小节的特征序列（用到音频上是推断）[B55]。

### 4.5 汇总
| 方法 | 需要多长上下文 | 能否只看过去 | 计算量 | 常见误判 |
|---|---|---|---|---|
| Yadati 2014 两步法 | 整首 | 否 | 中（chroma 自相似矩阵 + SVM） | 段界没对准 drop；误报 |
| Zehren 2020 规则法 | 整首；每个点要看之后 4 小节 | 改造后可以，至少晚 4 小节（推断） | 中（鼓件转录 + 多特征自相似矩阵） | 周期或相位估错；不守 4 小节的曲子 |
| Foote 新颖度 | 前后各半个核 | 可以，晚半个核 | 低（按小节算特征时） | 核小时假峰多 |
| MSAF / All-In-One | 整首 | 否 | 中 / 高（要 GPU 和音源分离） | 找到的段界不是 DJ 关心的那个点（Zehren 实测精度约 30%） |
| BOCPD + 小节特征 | 只用过去 | 是 | 低 | 渐变、没有台阶的 build 很难判 |

推断（没查到论文实证）：House 的满编段或 drop，实时检测时最稳的信号是：**按小节对齐的低频能量占比和短期响度同时跳上一个台阶**，至少要等 2–4 小节才能确认。Progressive 那种慢慢推上去的 build 没有台阶，会漏检；只有 1 小节的过门会造成误报，所以必须要求变化持续一段时间。

## 5 能在 Celadon 实时插件里落地的方案

### A 门控画像（只用接近最响段的帧来学）
- 依据：BS.1770 的相对门 [B39]、Tech 3342 的 95 分位 [B43]、ReplayGain 1.0 的取 95% [B44]、R 128 s1 的最大短期响度 [B41]、Tech 3341 的"LRA 前 60 s 不稳定" [B42]。
- 实现：见 §3.2；把现在的"累计 15–30 s 有效音频"改成"累计**过了门的**有效音频"。
- 代价：不增加延迟，CPU 开销可忽略。
- 风险：门只能在已经听到的音频里挑。用户只播了前奏就锁定，门控也救不了，所以要配合方案 B 的锁定条件。ΔG 需要标定：太窄，样本太少；太宽，前奏又会混进来。
- 适用宿主：全部（任意总线、任意格式）。

### B 按小节和曲风定最低聆听长度，没听到满编段不锁定
- 依据：PositionInfo 的各个字段 [B28][B30][B31]；EDM 的 4 小节周期 [B47]。
- 实现：House 模式下，把"校准两轮后锁定"改成：至少听满 N 小节（N 按 C 报告给出的前奏长度定，例如 64），**并且**已经检测到满编段（见方案 C），然后才锁定。BPM 拿不到、或与音频节拍对不上时，退回按秒计的下限（例如 120 s，推断）。
- 代价：学习时间变长（124 BPM 下 64 小节约 124 s）；CPU 开销可忽略。
- 风险：§2.3 列的四条。用户只试听局部时会一直锁不住，所以界面上要提示"还没听到主段落"。
- 适用宿主：依赖宿主给的拍点信息；拿不到时自动退回按秒计。

### C 认出 drop 或满编段后重学（只看过去的检测）
- 依据：drop 就是 bassline 重新进入 [B46]；4 小节周期和新颖度 [B47]；新颖度晚半个核 [B48]；BOCPD [B55]。
- 实现：每小节记录 K 加权响度、低频（约 40–150 Hz）能量占比、频谱通量、高频噪声能量；只在 4 小节或 8 小节的分界处做判断；用"最近 4 小节对比之前 8–16 小节"的只看过去的半边棋盘新颖度，或者用 BOCPD 来触发。触发条件：响度跳升至少 ΔU，且低频占比明显上升，并持续至少 4 小节。触发后清空画像累计，重新校准两轮。只允许朝"更满、更响"的方向重学，每首重学次数设上限；换方案时在 2–4 小节内平滑过渡。
- 代价：确认大约晚 4 小节（124 BPM 下约 7.7 s）；CPU 开销可忽略。
- 风险：慢慢推上去的 build 会漏检（由方案 A 兜底）；过门、假 drop 可能误触发（靠持续条件挡住）；Tech House 从头就有 bass，不需要重学；重学会让处理方案中途改变，有可能听得出来。这里只改画像和方案、不做跟随式增益，和现有"不做段落响度补偿"的原则不冲突（推断）。
- 适用宿主：全部；有拍点信息时更准，没有时改用音频节拍跟踪或固定秒数分块。

### D 整首预分析（拖入成品文件 / ARA）
- 依据：ARA 能读完整个音频源、在后台分析 [B1][B26]；不支持 ARA 的宿主里，插件只能"隔着钥匙孔看" [B5]。
- 实现：两个入口共用同一套离线分析。① 把**成品 WAV / AIFF 拖到插件界面**上，后台线程读完整首、算出全曲画像。这条不依赖 ARA，任何宿主、任何总线都能用（推断可行，还要逐个宿主验证插件窗口能不能接收文件拖放）。② ARA 模式（即 §1.3 讲的那套改造）。
- 代价：分析耗时远短于实时播放（推断）；播放时没有额外延迟；① 工程量中等，② 工程量大。
- 风险：① 分析的文件可能和实际送进插件的信号不一致，比如混音改过了却没重新拖入。② 挂 master bus 时无效、Logic 在 Apple Silicon 上要用 Rosetta、Live / FL / Bitwig 都没有 ARA、ARA 模式下不应使用宿主参数。
- 适用宿主：① 全部；② 见 §1.5。

### E 离线渲染两遍
- 依据：isNonRealtime 的来源 [B29][B30][B31]；可靠性上的缺口 [B35][B36][B37]。
- 实现：一次离线渲染如果从头到尾完整跑完，就用整首的统计覆盖画像，并提示"画像已更新，请再导出一次"；第一遍导出的音频作废。出俱乐部版和流媒体版两版时，第一遍正好可以当学习遍用（推断）。另外提供一个"手动标为学习遍"的按钮，给拿不到离线标志的宿主用。
- 代价：要多导出一次；离线渲染时 CPU 不受实时限制。
- 风险：Live 11 的 VST3 导出不报离线 [B36]，FL 要在包装器里勾选选项 [B35]；离线时不能改插件延迟，Logic 在 bounce 时不会重新查询延迟 [B58]；从中间开始的离线渲染要能识别出来；用户可能忘了导第二遍，直接交出用旧画像做的成品。
- 适用宿主：大部分可用；Live 这类宿主靠手动按钮。

### 推荐排序
1. **A + C 合在一起做核心**：门控画像 + 认出满编段后重学。所有宿主都能用，没有许可问题，CPU 开销可忽略，直接解决"拿前奏定画像"的问题。
2. **B 作为锁定条件**：House 模式下"至少听满 N 小节、并且已听到满编段"才锁定，界面上给出提示。
3. **E 作为交付时的兜底**：要出两版母带时，顺带跑一遍学习遍。
4. **D 放到以后**：先做"拖入成品文件"。ARA 只有在确定要走"单独开母带工程"的工作流之后再评估——它挂在 master bus 上无效，宿主覆盖不全，改动量又大。

另外（推断）：画像描述的是歌曲本身，所以俱乐部版和流媒体版应该共用同一份锁定好的画像，只换目标响度和限幅设置（目标数值见 C 报告）。

## 6 没查到一手资料的问题
- Logic、Studio One、Bitwig、Cakewalk、Luna、DP 在拍点字段和离线标志上是否可靠：没有一手资料（Logic 只有社区帖）；Live 12 是否已经修好"导出不报离线"也没查到。
- Ableton、Bitwig、Image-Line 三家关于 ARA 的官方表态：都没找到（现有判断依据是 Celemony 的兼容列表、厂商博客和社区帖）。
- Cakewalk Sonar、Luna、DP 里 ARA 插件挂在哪里：没有一手文档（UA 帮助中心拒绝抓取）。
- 在 master 总线上用 ARA 做整首母带画像的商业实例、"只用最响段学母带画像"的论文、只看过去的 EDM drop 检测论文及评测数据：都没找到。
- House 曲目前奏与满编段之间的短期响度差（LU）和低频占比差：没有公开统计，需要用自己的曲库测。
- Vande Veire 与 De Bie 2018 年的 Drum and Bass 自动 DJ 论文：全文被 Springer 拦截，没读到；BeatNet 只读到摘要；AAX 文档只找到公开的 2.1.1 旧版。
- JUCE 是否正式支持 Windows 上的 ARA：官方没有专门写明（目前只能从插件格式推断）；Logic 何时在 Apple Silicon 原生下支持 ARA：Celemony 只说正与 Apple 合作。

## 出处
**ARA 与宿主**
- [B1] Celemony，ARA API Documentation v2.3.0（PDF，§1.2、§2.8–2.10、§3.1.4、§3.2.4、§3.13）：https://github.com/Celemony/ARA_API/raw/main/ARA_API.pdf
- [B2] Celemony，ARA_SDK README（Apache-2.0、伴随 API、AAX 集成说明）：https://github.com/Celemony/ARA_SDK
- [B3] Celemony，ARA_SDK tags（2.0.0 于 2021-05-03 … 2.3.0 于 2025-11-06）：https://github.com/Celemony/ARA_SDK/tags
- [B4] Celemony，Melodyne Editions and technical matters / DAW compatibility：https://www.celemony.com/en/melodyne/editions-and-technical-matters
- [B5] Celemony 帮助中心，Transferring audio（Live 环境）：https://helpcenter.celemony.com/M5/doc/melodyneEssential5/en/M5tour_TransferARA?env=live
- [B6] Celemony 帮助中心，Logic 中插入 Melodyne（ARA）：https://helpcenter.celemony.com/M5/doc/melodyneStudio5/en/M5tour_LogicARA_InsertVorbereitungen?env=logic
- [B7] Celemony 帮助中心，Reaper 中插入 Melodyne（ARA）：https://helpcenter.celemony.com/M5/doc/melodyneEssential5/en/M5tour_ReaperARA_InsertVorbereitungen?env=reaper
- [B8] Apple，Support for ARA 2 plug-ins in Logic Pro（Logic Pro 11.2 用户手册；按同一页面编号打开当前 12.x 手册会跳回首页，没找到 12.x 版的这一页）：https://support.apple.com/guide/logicpro/support-for-ara-2-compatible-plug-ins-lgcp58ce340b/11.2/mac/15.0
- [B9] Apple，Turn off Rosetta for Logic Pro for Mac（2024-11-13）：https://support.apple.com/en-us/121464
- [B10] Steinberg，Cubase / Nuendo：Activating Extensions for Audio Events / Tracks：https://steinberg.help/nuendo/v12/en/cubase_nuendo/topics/ara_integration/ara_activating_an_ara_plug_in_in_cubase_t.html
- [B11] REAPER User Guide v7.81（2026-09，§10.12）：https://www.reaper.fm/userguide/ReaperUserGuide781c.pdf
- [B12] Steinberg，WaveLab Pro 12：WaveLab as an ARA Extension：https://archive.steinberg.help/wavelab_pro/v12/en/wavelab/topics/wavelab_exchange/wavelab_ara_extension_c.html
- [B13] Avid，NoiseWorks DynAssist Lite ARA Plugin for Pro Tools（2025-04-22）：https://www.avid.com/resource-center/dynassist-ara
- [B14] sonible 博客，What is ARA（厂商博客，2024，列表部分属二手）：https://www.sonible.com/blog/what-is-ara/
- [B15] Production Expert，ARA Plugins in Pro Tools（二手）：https://www.production-expert.com/production-expert-1/ara-plugins-in-pro-tools-every-supported-tool-explained
- [B16] Image-Line 论坛，ARA in FL Studio（社区）：https://forum.image-line.com/viewtopic.php?t=280599
- [B17] Bitwig，What's New（6.1）：https://www.bitwig.com/whats-new/
- [B18] Bitwig Studio 5.3 User Guide（PDF）：https://www.bitwig.com/media/bitwig_userguide/pdf/Bitwig_Studio_User_Guide_English_XfuP7Nz.pdf
- [B19] JUCE 论坛，Questions about ARA（社区，2022）：https://forum.juce.com/t/questions-about-ara/52583
- [B20] Avid，AAX SDK 2.1.1：AAX_IACFHostProcessor：https://learn-cdn.avid.com/AAX_SDK_2p1p1/Documentation/Doxygen/output/html/a00053.html
- [B21] Avid，AAX SDK 2.1.1：AAX_IHostProcessorDelegate：https://learn-cdn.avid.com/AAX_SDK_2p1p1/Documentation/Doxygen/output/html/a00076.html
- [B22] Gearnews，The Best ARA Plugins（二手，2025-10）：https://www.gearnews.com/the-best-ara-plugins-for-vocals-and-more/
- [B56] Apple，Logic Pro for Mac release notes（写到 12.3.1，页面日期 2026-09-29）：https://support.apple.com/en-us/109503

**JUCE 与 VST3 SDK**
- [B23] JUCE CHANGE_LIST.md（7.0.0 起加入 ARA）：https://github.com/juce-framework/JUCE/blob/master/CHANGE_LIST.md
- [B24] JUCE BREAKING_CHANGES.md（9.0.2；ARA SDK 2.3.0 的要求）：https://github.com/juce-framework/JUCE/blob/9.0.2/BREAKING_CHANGES.md
- [B25] JUCE docs/CMake API.md（IS_ARA_EFFECT 等）：https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md
- [B26] JUCE juce_ARAAudioReaders.h：https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_formats/format/juce_ARAAudioReaders.h
- [B27] JUCE juce_ARAPlugInInstanceRoles.h（9.0.2）：https://github.com/juce-framework/JUCE/blob/9.0.2/modules/juce_audio_processors_headless/utilities/ARA/juce_ARAPlugInInstanceRoles.h
- [B28] JUCE juce_AudioPlayHead.h：https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_basics/audio_play_head/juce_AudioPlayHead.h
- [B29] JUCE juce_AudioProcessor.h（isNonRealtime）：https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors_headless/processors/juce_AudioProcessor.h
- [B30] JUCE VST3 包装器：https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp
- [B31] JUCE AU 包装器：https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_plugin_client/juce_audio_plugin_client_AU_1.mm
- [B32] Steinberg VST3 SDK ivstprocesscontext.h：https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivstprocesscontext.h
- [B33] Steinberg VST3 SDK ivstaudioprocessor.h（ProcessModes）：https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivstaudioprocessor.h
- [B34] Steinberg 论坛，Get current Bars/Beats position（社区，2017）：https://forums.steinberg.net/t/get-current-barsbeatsposition/201675
- [B35] Image-Line，FL Studio 手册：Plugin Wrapper：https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/wrapper.htm
- [B36] JUCE 论坛，Live 11 离线渲染时插件收不到离线标志（社区，2021）：https://forum.juce.com/t/while-offline-rendering-in-albetron-live-11-plugin-didnt-informed-about-it/48571
- [B37] JUCE 论坛，Are there known hosts in 2020 that don't reliably set isNonRealtime（社区）：https://forum.juce.com/t/are-there-known-hosts-in-2020-that-dont-reliably-set-isnonrealtime-flag/40692
- [B38] JUCE 论坛，Trouble with isNonRealtime for offline render（社区，2016）：https://forum.juce.com/t/trouble-with-isnonrealtime-for-offline-render/18564
- [B57] JUCE 论坛，Issue syncing a sequencer with Logic / BPM reporting（社区，2023）：https://forum.juce.com/t/issue-syncing-a-sequencer-with-logic-bpm-reporting/58358
- [B58] JUCE 论坛，AU/Logic: Different latency for offline rendering（社区，2016）：https://forum.juce.com/t/au-logic-different-latency-for-offline-rendering/16372
- [B59] JUCE 论坛，Getting accurate time from DAW with tempo changes（社区，2024）：https://forum.juce.com/t/getting-accurate-time-from-daw-with-tempo-changes/64426

**响度标准**
- [B39] ITU-R BS.1770-5（2023-11）：https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf
- [B40] EBU R 128-2023：https://tech.ebu.ch/docs/r/r128.pdf
- [B41] EBU R 128 s1-2020：https://tech.ebu.ch/docs/r/r128s1.pdf
- [B42] EBU Tech 3341-2023：https://tech.ebu.ch/docs/tech/tech3341.pdf
- [B43] EBU Tech 3342-2023：https://tech.ebu.ch/docs/tech/tech3342.pdf
- [B44] D. Robinson，ReplayGain 提案：Statistical Processing（Wayback 2018 快照）：http://web.archive.org/web/20180211170923/http://replaygain.hydrogenaud.io:80/proposal/statistical_process.html
- [B45] TC Electronic，Loudness Literature and Glossary（厂商资料）：https://www.tcelectronic.com/loudness-literature-and-glossary
- [B60] iZotope，Ozone 11 Help：Master Assistant（与 A 报告交界，只引一处）：https://docs.izotope.com/ozone11/en/master-assistant/index.html

**段落 / 节拍 / 变点**
- [B46] Yadati 等，Detecting Drops in Electronic Dance Music（ISMIR 2014）：https://archives.ismir.net/ismir2014/paper/000297.pdf
- [B47] Zehren、Alunno、Bientinesi，Automatic Detection of Cue Points for DJ Mixing（arXiv 2020）：https://arxiv.org/abs/2007.08411
- [B48] M. Müller，FMP 讲义 C4S4 Novelty-Based Segmentation：https://www.audiolabs-erlangen.de/resources/MIR/FMP/C4/C4S4_NoveltySegmentation.html
- [B49] Nieto、Bello，Systematic Exploration of Computational Music Structure Research（ISMIR 2016）：https://ccrma.stanford.edu/~urinieto/MARL/publications/ISMIR2016-NietoBello.pdf；代码：https://github.com/urinieto/msaf
- [B50] madmom beats.py：https://github.com/CPJKU/madmom/blob/main/madmom/features/beats.py
- [B51] madmom downbeats.py：https://github.com/CPJKU/madmom/blob/main/madmom/features/downbeats.py
- [B52] BTrack README：https://github.com/adamstark/BTrack
- [B53] Heydari、Cwitkowitz、Duan，BeatNet（ISMIR 2021，仅读摘要）：https://arxiv.org/abs/2108.03576
- [B54] Kim、Nam，All-In-One（WASPAA 2023）：https://github.com/mir-aidj/all-in-one；论文：https://arxiv.org/abs/2307.16425
- [B55] Adams、MacKay，Bayesian Online Changepoint Detection（2007）：https://arxiv.org/abs/0710.3742
