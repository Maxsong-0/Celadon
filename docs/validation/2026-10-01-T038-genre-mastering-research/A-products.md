# T-038 调研 A：同类产品怎样决定「分析哪一段、什么时候学」

> 调研代理 A · 2026-10-01 · 只看产品层面的「分析区段 / 学习时机」。技术路线（ARA2、段落与 drop 检测、门控学习）归 B；曲风目标曲线、平台与俱乐部数值归 C，本文只一句带过。
> 标注：【文档】厂商一手资料明写；【推断】本文推断；【二手】非厂商资料。方括号 [Sx] 为文末出处编号，全部链接于 2026-10-01 抓取核对；LANDR / BandLab / eMastered / sonible / Serato 的 Zendesk 帮助中心对脚本返回 403，正文经各自官方 help_center API 读取，所列为文章原址。

## 结论摘要

1. 实时插件（Ozone Master Assistant、sonible smart / pure / learn 系列、LANDR 插件、Mastering The Mix 系列）都只听「用户正在播的那几秒」，选段责任交给用户：官方指引清一色是「放最响 / 最满的一段（副歌、drop），不够长就循环」；**没有一家文档声称能自动跳过长前奏或 breakdown**。[S1–S6][S21–S31][S41][S59–S63]
2. Ozone 学习时长：8/9 代约 30 s → 10/11/12 代至少 8 s；12 代 Custom 新增 Analysis Time（最长 60 s，可按小节、跟宿主速度），并明写「只分析了一部分，最终整合 LUFS 可能不同」。Ozone 8 文档早就警告：在安静段学 Maximizer，响段可能失真——与 Celadon 的前奏坑同构。[S1][S5]
3. 「自动挑代表段」只出现在拿得到整文件的场景：Ozone 10–12 对参考曲只学「最响的 8 秒」并据此生成目标（10 代明写以这 8 秒的整合响度作 Maximizer 滑块中点）；MTM LIMITER 则把响度目标直接定义为「最响段的短时响度」。[S3–S5][S59][S60]
4. 整首分析：Logic Mastering Assistant 默认分析整个工程、设了 cycle 只分析 cycle 内（Apple 还建议可只分析最响部分）；LANDR / BandLab / eMastered / Waves 在线服务都处理整文件，**均未提供「选参考段」选项**。[S33–S35][S38–S48]
5. 学完一般锁成静态参数 + 「重学」按钮；持续适应单列为可调量（smart:EQ 的 Dynamic、smart:comp 2 的 Input Riding、pure:level、Ozone Stabilizer）。Ozone Maximizer 的学习从 9 代「持续到手动关」改为 10 代起「听 5 秒自动关」。[S9–S14][S23–S25][S31]
6. 计量工具有「按时间位置记账」的先例：Youlean 的 Time Code 模式、NUGEN VisLM 的 ReMEM（锁宿主时间码、随工程保存、局部重放只更新那一段）——实时插件可用 PPQ 把听过的各段拼成整首画像。[S64][S65]
7. DJ 软件里只有 rekordbox 有官方的整轨乐句分析（Intro / Up / Down / Chorus / Bridge / Verse / Outro，依赖节拍网格，导入时离线完成）；Traktor / Serato / Engine DJ 官方文档未见同类功能。按小节认 drop 在离线整轨上已是商用能力，实时插件只能事后确认。[S49–S58]
8. 对 GLAZE 的启发【推断】：学习窗从「开播起 15–30 s」改成「按小节对齐、只收满编窗、以最响 / 最满的 N 小节为代表」，按 PPQ 记位置累积；俱乐部版与流媒体版从同一份画像派生，不重学。

## 1 iZotope

### 1.1 Ozone Master Assistant 各代对比

| 代 | 分析对象 · 时长 | 官方「放哪段」 | 学完 · 重学 | 区段 / 参考曲 / 目的地 | 出处 |
|---|---|---|---|---|---|
| Ozone 8（MA 首发，2017） | 实时输入；点 Next 后先听约 5 s 生成目标曲线，整个分析约需 30 s 音频 | 最响部分；要分析的段不足 30 s 先在 DAW 里循环 | Accept 保留 / Cancel 还原；可从 Undo 历史撤回 | 只能靠 DAW 循环选段；Reference 目标直接取参考曲的响度与 EQ 曲线（未说明取哪段） | [S1][S7] |
| Ozone 9 | 至少播放 30 s | 最响部分；不足 30 s 循环 | Accept / Cancel | 同上 | [S2] |
| Ozone 10 | 至少 8 s | 最响部分；不足 8 s 循环 | 新增 Relearn：重新分析，为「新测到的那一段」重建处理 | 参考曲自动只学「最响的 8 秒」；Streaming 目的地把 Maximizer 滑块中点设为流派目标（固定 −10 LUFS）或参考曲最响 8 s 的整合响度；DJ Player 目的地：ceiling −0.1 dB、关真峰值、响度区间整体上移 2 dB | [S3] |
| Ozone 11 | 至少 8 s | 最响部分 | Relearn | 分析期测整合响度并据此设 Maximizer Gain；参考曲仍为最响 8 s；目的地改为 Full Scale（−0.1 dB、关 TP）/ Streaming（−1 dB、开 TP） | [S4] |
| Ozone 12（2025） | One-Click（博客称 Auto-Master）至少 8 s；Custom 的 Analysis Time 最长 60 s，可切秒 / 小节，小节随宿主速度 | 文档原话 "the loudest and fullest section of the mix, such as a final chorus"；博客：通常是副歌 | Relearn；官方博客提醒分析期间输出会有响度跳变 | 文档明写：只分析了 Analysis Time 那段，最终整合 LUFS 可能偏离目标，合规请用 RX Loudness Control；参考曲仍为最响 8 s；目标库含 House / Techno / EDM 等约 35 个 | [S5][S6][S8] |

- 【文档】Ozone 8 的「重要提示」：务必放最响部分；在安静段学到的 Maximizer Threshold 只为那段把响度推满，响段就可能失真 [S1]。
- 【推断】这正是 Celadon 的前奏坑；iZotope 五代都靠用户指引或用户设定（12 代的 Analysis Time）解决，文档未见自动避开前奏的机制。124 BPM 下 Ozone 12 的 60 s 上限约 31 小节，恰好约等于一段 32 小节 DJ 前奏——用户若从第 1 小节开始播，Ozone 12 也只学到前奏。
- 【文档】10–12 代参考曲目标的响度取「最响 8 s」而不是全曲整合 [S3][S4][S5]；流派目标响度在 10 代文档写明固定为 −10 LUFS [S3]。

### 1.2 Maximizer 等模块「学习按钮」的代际变化

- 【文档】Ozone 9 Learn Threshold：开启后持续按输入更新阈值，直到手动关闭；注明不得用于响度合规 [S9]。
- 【文档】Ozone 10 起改为「听 5 s 定阈值后自动关」；11 / 12 改名 Learn Input Gain，仍是 5 s 后自动关 [S10][S11][S12]。【推断】iZotope 把「持续学」收敛为「一次性学」，与 Celadon「校准后锁定」同向。
- 【文档】Ozone 12 其它学习：Unlimiter 的 Learn Threshold 取学习期间测到的最高样本峰值；Impact 的 Learn 把分频点挪到频谱极小值后自停；Stabilizer 是持续自适应 EQ，用 Speed 控制反应快慢 [S13][S14]。

### 1.3 Match EQ 与 Tonal Balance Control

- 【文档】Match EQ（9–12 代一致）：Reference 与 Apply To 两个快照都由用户点 Capture 开始、点 Stop 冻结，区段完全由用户决定；Reference 快照随预设保存，Apply To 不保存 [S15][S16]。
- 【文档】TBC 2：表头是持续累积的计量，Reset 清零；Options 可调频谱 Average Time；可由单个文件或整个文件夹（合成一条平均曲线）建目标 [S17][S18]。
- 【文档】TBC 3：插件内 Capture 录 DAW 实时音频，文档要求录到「一段有代表性的段落」再点 Stop，3.5 分钟自动停；由文件建目标时取哪一段，文档未写 [S19][S20]。

## 2 sonible（以下手册均取自官方手册索引 [S32]）

| 产品（手册年份） | 官方「学哪段」 | 学多久 | 学完 | 持续适应 · 分段 | 出处 |
|---|---|---|---|---|---|
| smart:limit（2021；链接为 2023 版） | "relatively loud sections of your track (e.g. the refrain)"，让它看到最关键的动态段 | 手册无秒数；产品页称几秒 | 自动设 input gain、limit、attack 等并解锁音色工具；可一键回到「smart state」 | Auto Release 随输入自适应；Quality Check 持续对照目标给提示 | [S21][S22] |
| smart:EQ 3 | 只要求有输入、按录制键 | 未写 | 应用 smart:filter，绿色权重曲线控强度 | Dynamic Adaption：开启后在整首长度上持续分析并修正；8 个 States 可存「不同段落各自的 smart:filter」 | [S23] |
| smart:EQ 4（2026 版手册） | 同上 | 最短约 6 s，约 10 s 出结果；设置页可设更长「最大学习时长」，期间随时 Get result | 自动应用 | Dynamic >0 即持续适应；States 存不同段；可设「插件加载即自动学习」 | [S24] |
| smart:comp 2（2022） | 相对响的段（如副歌） | 未写 | 设 ratio / threshold / attack / release，开 Spectral Compression 与 Input Riding | Input Riding：动态加减输入增益，抹平响段与静段的电平差 | [S25] |
| smart:comp 3（2025-12） | 须在播放中启动学习 | 未写 | 不给单一设置，而是生成 Compression Matrix（该素材可选的压缩特性范围） | — | [S26] |
| pure:limit（2022） | 相对响的段（如副歌）；想换段就从那里开始播放再按 learn | 未写 | 设定并解锁全部参数 | — | [S27] |
| learn:limit（2025） | 最好从响段（如副歌）开始播放 | 按钮内进度条，未写秒数 | 设增益与限幅速度并开调音模块；随时重学可换段；换 Profile 不必重学（按原分析重算，但会清掉手动调整） | 响度表可 Restart，以便从头到尾量整首 | [S28] |
| learn:EQ / smart:chain / pure:level（2025–2026，混音向） | 「代表性段落」：完整乐句或密集段；避开只有静音的段；代表该轨的典型电平 | — | smart:chain 的 States 连同「学到的观测」一起保存；输入变了就重学 | learn:EQ 有 Dynamic；pure:level 学完平均电平后持续骑电平 | [S29][S30][S31] |

- 【文档】smart:limit 的学习目标是「动态」（短时 PSR 的中位数）而不是响度，所选发布目标在学习时被忽略 [S21]。
- 【文档】smart:limit 手册：整合响度只有长观测（>60 s）才算有效；编曲改变时应重置整合测量；短时响度直方图出现两个以上的峰，表示曲中有明显不同的段落 [S21]。【推断】双峰直方图可直接当作「学习窗里混进了前奏和 drop」的报警信号。
- 【文档】Instant Impact Prediction：改参数后响度 / 动态表即时反映，不必重放整首 [S21][S22]。【推断】说明学一次后可以「预测」不同设置的结果，与「一份画像派生两版母带」相通。
- 【推断】sonible 的措辞随产品类型分化：限幅 / 压缩（2021–2025）一律「学响段」；EQ / 通道条 / 电平工具（2025–2026）改说「代表性段落」。

## 3 Apple Logic Pro Mastering Assistant（10.8 起）

- 【文档】插到立体声输出即触发分析；"analyzes the whole project unless you set a cycle"，设了 cycle 只分析 cycle 内；iPad 版同一说法 [S33][S37]。
- 【文档】Reanalyze 按钮：工程有改动时重分析；设了 cycle 时按钮名变成 Reanalyze Section，空工程加内容后变成 Analyze [S34]。
- 【文档】Apple 支持文章：设定位符可只分析那一段，例如只分析最响的部分；插入后会「立即」分析整个工程（含软件乐器和效果）[S35]。
- 【文档】Loudness 旋钮居中时输出约 −14 LUFS-I；处理后限到 −1 dBFS 真峰值；Auto EQ 0–100 %；Width 只在分析认为过窄 / 过宽时偏离中位 [S34]。10.8 发行说明列为新功能 [S36]。
- 拿整首的办法：不设（或关掉）cycle 再按 Reanalyze。【推断】文档没说分析是否离线快速渲染、各段如何加权；从「插入即分析、无需播放」看应是离线渲染。

## 4 离线 / 在线母带服务

| 服务 | 分析对象 | 选参考段 | 其它要点 | 出处 |
|---|---|---|---|---|
| LANDR 在线 | 整首或整张专辑；引擎「听」音乐、按微流派做逐帧（frame-by-frame）细调；可直接上传整段 DJ mix | 未查到 | 专辑模式额外统一色彩与响度；Low / Med / High 三档；有参考母带 | [S38][S39][S40] |
| LANDR Mastering Plugin | 插件提示「播放曲中最响的部分」后分析；官方博客建议检测期间用 DAW 循环框住较响段 | DAW 循环 | 改混音后按 Master 重新分析；分析在本地完成 | [S41][S42] |
| BandLab Mastering | 整首（文档未写分析方式） | 未查到 | 8 个预设 + Intensity + Auto Gain；Reference Matching 扫描参考曲后自动调参 | [S43][S44] |
| eMastered | 整首（先分析歌、再挑工具） | 未查到 | 给 EDM / House 的建议：loud / extreme 音量档，立体声宽度调小以保俱乐部低频 | [S45][S46] |
| Waves Online Mastering | 整首（5 s–12 min）；免费试听 30 s（取哪 30 s 未写），确认后处理全曲 | 未查到；也没有 LUFS 目标设置，靠参考曲引导 | 同文件同设置结果相同；参考曲须能量、速度、编制、编曲相近 | [S47][S48] |

- 【推断】在线服务拿得到整文件，所以不要求用户选段；但没有一家公开说明长前奏 / breakdown 在分析里如何加权。

## 5 DJ 软件的整轨结构分析

- 【文档】rekordbox Phrase Analysis：把曲目结构切成乐句，类别为 Intro / Up / Down / Chorus / Bridge / Verse / Outro；改节拍网格会影响乐句结果，需只勾 Phrase 重新分析；可手动改类型、合并 / 切分、清空重跑 [S49]。
- 【文档】Mood（HIGH / MID / LOW）依据速度、节奏、底鼓、声音密度分类 [S50]；Fill in 指乐句末尾的即兴变化，在 Intro / Up / Chorus 末尾检测（最多 4 拍）；重跑乐句分析会清掉手动编辑 [S51]。
- 【文档】分析在导入曲库时离线进行（可选 BPM/Grid、KEY、Phrase、Vocal）；CUE Analysis 用乐句信息自动放 Hot / Memory Cue，乐句或小节太少就放不出；灯光模式也依赖乐句 [S50][S52]。
- 【二手·逆向】Deep Symmetry 解析 rekordbox 导出文件：mood 为 high 时乐句类型是 Intro / Up / Down / Chorus / Outro（mid / low 改用 Verse / Bridge 体系），每个乐句按起始拍号记录 [S53]。题目所说的「EDM 五段」即 high mood；乐句切分算法本身官方未公开。
- 【文档】Traktor Pro：分析项为 BPM / 网格、调性、Gain（Auto-Gain）、Stem 分离；「Bars per Phrase」只是节拍计数器的用户设定，不是检测 [S54][S55]。
- Serato、Engine DJ：帮助中心站内检索 phrase / song structure，未见乐句或段落自动分析条目（只查到网格、打击垫模式等）[S56][S57]——属「未查到」，不等于确认没有。Mixed In Key 自动放最多 8 个吸附网格的 cue（用于 intro、breakdown、drop 等），算法未公开 [S58]。
- 【推断】可行性：House 有稳定 4/4 网格和 8 / 16 / 32 小节乐句，rekordbox 已把「认 Up / Chorus（≈ drop）」做成离线整轨的商用功能；实时插件拿不到未来音频，只能在 drop 出现之后确认（算法归 B）。

## 6 其他母带 / 响度工具

- 【文档】MTM LIMITER：先「监听最响段」再点 analyze；输入增益目标针对「最大短时响度」；LUFS 表显示短时值，要求最响段落在预设目标区间内 [S59]。官方博客：俱乐部版最响段约 −6 LUFS-S，流媒体版约 −9 LUFS-S（数值由 C 复核）[S60]。
- 【文档】MTM FASTER MASTER：播放最响段再 Analyze；refresh 只重算响度、不动其它设置 [S61]。
- 【文档】MTM BASSROOM / MIXROOM：监听 drop 或副歌；Integrated Target 基于累计听到的全部音频，Short-Term Target 基于最近 3 s；导入参考曲时建议在波形上框选 drop / 副歌；MIXROOM 约 3 s 后可出建议；都可 Reset [S62][S63]。
- 【文档】Youlean Loudness Meter 2（v2.4.x，2021-01 手册）：直方图模式有 Continuous、Elapsed（随宿主暂停）、Time Code（数据按时间码位置存，开启 loudness overdub，完整 3 s 缓冲的值覆盖旧值）、System Time；可单独暂停整合与 LRA；可设「宿主重新播放即复位」；拖入文件即整首分析 [S64]。
- 【文档】NUGEN VisLM：ReMEM 锁宿主时间码，记忆最多 24 小时响度数据；局部修改或重录只触发整体值更新、无需从头重量；数据随工程保存；需宿主提供时间码 [S65]。LM-Correct：文件式（独立版或 AudioSuite）快于实时地评估并修正整个文件 [S66]。
- 【文档】Waves WLM Plus 两遍法：先完整播放测长期响度 → 测完后（不在播放中）点 Trim，把「目标 − 长期响度」写进 Gain → Reset → 开真峰值限幅 → 再放一遍复测；Follow Transport 让测量随宿主走停 [S67]。
- 未查到任何产品对「长 DJ 前奏」「breakdown → drop」给出专门的学习说明。

## 7 归纳：「分析区段」的七种模式与对实时插件的启发

| 模式 | 代表 | 对 Celadon / GLAZE 的启发（均为推断） |
|---|---|---|
| ① 用户指引的短窗：「放最响段」 | Ozone 8–12、sonible、LANDR 插件、MTM | 成本最低但全靠用户；GLAZE 现在「开播即学」且没有指引，正落在 Ozone 8 警告的坑里 → 至少要提示，并别默认从第 1 小节收 |
| ② 显式定长 / 框选（秒或小节） | Ozone 12 Analysis Time（≤60 s、可按小节）、Logic cycle、LANDR 循环、Match EQ / TBC Capture | 用 PPQ 把学习窗定义成「N 小节」并对齐乐句边界；宿主在循环播放时，视为用户指定了区段 |
| ③ 整段扫描后自动挑代表段 | Ozone 10–12 参考曲「最响 8 秒」 | 实时版：持续维护「迄今最响 / 最满的 N 小节」候选，出现更满的窗就替换早先的前奏窗（替换要有界、在锁定前完成） |
| ④ 整首离线 | Logic 默认、在线服务、LM-Correct、rekordbox | 插件做不到；可退一步：PPQ 覆盖率够高（整首播过一遍）才定稿，否则保持「暂定」 |
| ⑤ 按位置累积（时间码 / PPQ 为键） | Youlean Time Code、VisLM ReMEM、BASSROOM 累计 + 3 s 短时 | 画像按小节分桶存，重放同段覆盖、新段补齐，随工程保存——用户跳着播也能拼出整首 |
| ⑥ 学后锁定 + 可选有界自适应 | Ozone MA + Stabilizer、smart:EQ Dynamic、smart:comp Input Riding、pure:level | 与 Celadon「锁定 + 有界保护」一致；行业把持续适应做成独立可调量，不和「学」混在一起 |
| ⑦ 目标量纲避开前奏 | MTM（最响段短时响度）、smart:limit（以动态而非响度为学习目标）、Ozone 12 的免责声明 | 俱乐部 / 流媒体目标若按「主段短时响度」定义，就不受前奏长短影响；整合 LUFS 只作结果核对 |

- 两版母带：Ozone 10 用「目的地」开关在同一次分析上切 DJ Player 与 Streaming（ceiling、真峰值、响度区间不同）；learn:limit 换 Profile 不用重学、按原分析重算 [S3][S28]。【推断】GLAZE 学一次画像即可，俱乐部 / 流媒体两版只换目标与上限。
- 【推断】最小改动组合：① 学习窗改为小节对齐（PPQ）；② 只收「满编」窗（低频与短时响度接近迄今最大值才计入，门控细节归 B）；③ 用最响 N 小节作代表并按位置累积；④ UI 明示「请播放 drop / 已学到第几小节」作兜底。

## 8 未查到一手资料的问题

- Ozone 12 One-Click 的默认分析时长；Custom 的 Analysis Time 默认值。
- Ozone 12 文档仍写「ten genre targets」，与官方博客列出的约 35 个目标不一致，以哪份为准未核实。
- TBC 2 / 3「由文件建目标」时用整首还是某一段。
- Logic Mastering Assistant 是否离线渲染、各段如何加权、是否自动避开静音或前奏。
- LANDR 在线 / BandLab / eMastered / Waves Online Mastering 如何对前奏、breakdown 加权；Waves 30 s 试听取哪一段。
- sonible smart:limit、pure:limit、learn:limit 的具体学习秒数。
- rekordbox 乐句切分依据的特征与算法（官方只公开 Mood 的依据）；mood → 乐句类型映射只有逆向资料。
- Traktor、Serato、Engine DJ 有无乐句 / 段落自动识别（官方文档未见）；Mixed In Key 自动 cue 的算法依据。
- 任何产品针对「长 DJ 前奏」「breakdown → drop」的专门学习说明。
- NUGEN LM-Correct / VisLM 手册 PDF 的官方地址（本文改引产品页）。

## 出处

- [S1] iZotope Ozone 8 Help – Master Assistant：https://s3.amazonaws.com/izotopedownloads/docs/ozone801/en/master-assistant/index.html
- [S2] Ozone 9 Help – Master Assistant（9.13.0）：https://docs.izotope.com/ozone9/en/master-assistant/index.html
- [S3] Ozone 10 Help – Master Assistant（10.4.0）：https://docs.izotope.com/ozone10/en/master-assistant/index.html
- [S4] Ozone 11 Help – Master Assistant（11.0.0）：https://docs.izotope.com/ozone11/en/master-assistant/index.html
- [S5] Ozone 12 user guide – Master Assistant：https://docs.izotope.com/ozone12/en/master-assistant.html
- [S6] iZotope 博客 How to use Master Assistant in Ozone 12（2025-09-09）：https://www.izotope.com/community/blog/how-to-use-master-assistant-in-ozone
- [S7] iZotope Ozone 12 产品页（含 MA 2017 首发说明）：https://www.izotope.com/products/ozone-advanced
- [S8] iZotope 博客 10 steps to a quick master in iZotope Ozone（2025-10-06）：https://www.izotope.com/community/blog/10-steps-to-a-quick-master-in-ozone
- [S9] Ozone 9 Help – Maximizer：https://docs.izotope.com/ozone9/en/maximizer/index.html
- [S10] Ozone 10 Help – Maximizer：https://docs.izotope.com/ozone10/en/maximizer/index.html
- [S11] Ozone 11 Help – Maximizer：https://docs.izotope.com/ozone11/en/maximizer/index.html
- [S12] Ozone 12 user guide – Maximizer：https://docs.izotope.com/ozone12/en/maximizer.html
- [S13] Ozone 12 user guide – Unlimiter / Impact：https://docs.izotope.com/ozone12/en/unlimiter.html ；https://docs.izotope.com/ozone12/en/impact.html
- [S14] Ozone 12 user guide – Stabilizer：https://docs.izotope.com/ozone12/en/stabilizer.html
- [S15] Ozone 9 Help – Match EQ：https://s3.amazonaws.com/izotopedownloads/docs/ozone9/en/match-eq/index.html
- [S16] Ozone 12 user guide – Match EQ：https://docs.izotope.com/ozone12/en/match-eq.html
- [S17] TBC 2 Help – Target Curves and Views：https://s3.amazonaws.com/izotopedownloads/docs/tbc2/en/targets-and-views/index.html
- [S18] TBC 2 Help – Getting Started / Options：https://s3.amazonaws.com/izotopedownloads/docs/tbc2/en/getting-started/index.html ；https://s3.amazonaws.com/izotopedownloads/docs/tbc2/en/options/index.html
- [S19] TBC 3 user guide – Header（Capture）：https://docs.izotope.com/tbc3/en/header.html
- [S20] TBC 3 user guide – Target library / Getting started：https://docs.izotope.com/tbc3/en/target-library.html ；https://docs.izotope.com/tbc3/en/getting-started.html
- [S21] sonible smart:limit manual（EN）：https://www.sonible.com/wp-content/uploads/2023/03/manual-sonible-smartlimit_EN.pdf
- [S22] sonible 博客 smart:limit main features / 产品页：https://www.sonible.com/blog/smartlimit-main-features/ ；https://www.sonible.com/smartlimit/
- [S23] sonible smart:EQ 3 manual（EN）：https://www.sonible.com/wp-content/uploads/2023/03/manual-sonible-smartEQ3_EN.pdf
- [S24] sonible smart:EQ 4 manual（EN）：https://www.sonible.com/wp-content/uploads/2026/07/manual-smEQ4_EN.pdf
- [S25] sonible smart:comp 2 manual（EN）：https://www.sonible.com/wp-content/uploads/2023/03/manual-sonible-smartcomp2_EN.pdf
- [S26] sonible smart:comp 3 manual：https://www.sonible.com/wp-content/uploads/2025/12/manual-smartComp3.pdf
- [S27] sonible pure:limit manual（EN）：https://www.sonible.com/wp-content/uploads/2023/03/manual-sonible-purelimit_EN.pdf
- [S28] sonible learn:limit manual：https://www.sonible.com/wp-content/uploads/2025/08/sonible_manual-learnLimit.pdf
- [S29] sonible learn:EQ manual：https://www.sonible.com/wp-content/uploads/2025/08/sonible_manual-learnEQ.pdf
- [S30] sonible smart:chain manual：https://www.sonible.com/wp-content/uploads/2026/08/manual-smartChain.pdf
- [S31] sonible pure:level manual：https://www.sonible.com/wp-content/uploads/2026/03/manual-pureLevel.pdf
- [S32] sonible 帮助中心手册索引（以上 PDF 均取自此页）：https://help.sonible.com/hc/en-us/articles/360017065019-Where-do-I-find-the-latest-manual-for-my-plug-in
- [S33] Logic Pro User Guide for Mac – Mastering Assistant overview：https://support.apple.com/guide/logicpro/mastering-assistant-overview-lgcp7f94da0b/mac
- [S34] Logic Pro User Guide for Mac – Mastering Assistant parameters：https://support.apple.com/guide/logicpro/mastering-assistant-parameters-lgcpa4b810ba/mac
- [S35] Apple 支持 – Use Mastering Assistant in Logic Pro：https://support.apple.com/en-us/108294
- [S36] Logic Pro for Mac 10.8 release notes：https://support.apple.com/en-us/120134
- [S37] Logic Pro User Guide for iPad – Mastering Assistant overview：https://support.apple.com/guide/logicpro-ipad/mastering-assistant-overview-lpip7e08f598/ipados
- [S38] LANDR – What is LANDR Mastering? / 响度三档 / 参考母带：https://support.landr.com/hc/en-us/articles/115009725688-What-is-LANDR-Mastering ；https://support.landr.com/hc/en-us/articles/115009557127-What-are-the-LANDR-loudness-options ；https://support.landr.com/hc/en-us/articles/360049844534-What-is-reference-mastering
- [S39] LANDR 在线母带页：https://www.landr.com/online-audio-mastering
- [S40] LANDR – 专辑与单曲母带的区别：https://support.landr.com/hc/en-us/articles/360005148834-What-s-the-difference-between-mastering-an-album-and-a-single-track-using-LANDR
- [S41] LANDR – How do I use LANDR Mastering Plugin?：https://support.landr.com/hc/en-us/articles/18342996756247-How-do-I-use-LANDR-Mastering-Plugin
- [S42] LANDR 官方博客 How to master a song in your DAW：https://blog.landr.com/how-to-master-a-song-in-your-daw/
- [S43] BandLab – Using BandLab Mastering on your songs / Mastering FAQ：https://help.bandlab.com/hc/en-us/articles/360001374513-Using-BandLab-Mastering-on-your-songs ；https://help.bandlab.com/hc/en-us/articles/55678885417113-BandLab-Mastering-FAQ
- [S44] BandLab – Reference Mastering：https://help.bandlab.com/hc/en-us/articles/60815174563737-Reference-Mastering
- [S45] eMastered – How does eMastered work?：https://help.emastered.com/hc/en-us/articles/5614958439309-How-does-eMastered-work
- [S46] eMastered – What Setting Should I Use For EDM / Dance / House Music?：https://help.emastered.com/hc/en-us/articles/5572453910797-What-Setting-Should-I-Use-For-EDM-Dance-House-Music
- [S47] Waves Online Mastering – Getting Started：https://www.waves.com/support/waves-online-mastering-getting-started
- [S48] Waves Online Mastering – FAQ：https://www.waves.com/online-mastering/faq
- [S49] rekordbox FAQ（operation hint）乐句分析三条：https://rekordbox.com/en/support/faq/operation-hint/#faq-q500257 ；#faq-q500256 ；#faq-q500255
- [S50] rekordbox 7 FAQ – What is [Mood]? / CUE Analysis 与乐句 / 灯光需乐句分析：https://rekordbox.com/en/support/faq/rekordbox7/#faq-4980 ；#faq-70961 ；#faq-4926
- [S51] rekordbox 7.0.5 PHRASE EDIT Operation Guide：https://cdn.rekordbox.com/files/20241203210634/rekordbox7.0.5_Phrase_Edit_operation_guide_EN.pdf
- [S52] rekordbox 7.2.18 Instruction Manual：https://cdn.rekordbox.com/files/20260807093645/rekordbox7.2.18_manual_EN.pdf
- [S53]【二手·逆向】Deep Symmetry – rekordbox ANLZ 文件分析（PSSI 乐句标签）：https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/anlz.html
- [S54] Traktor Pro 手册 – Preferences（Analyze Options、Bars per Phrase）：https://docs.native-instruments.com/ni-tech-manuals/traktor-pro-manual/en/preferences
- [S55] Traktor Pro 手册 – Managing your Track Collection（手动分析选项）：https://docs.native-instruments.com/ni-tech-manuals/traktor-pro-manual/en/managing-your-track-collection
- [S56] Serato 帮助中心（站内检索 phrase / song structure，2026-10-01）：https://support.serato.com/hc/en-us
- [S57] Engine DJ 帮助中心（站内检索 phrase）及 Prime 4 G2 Pad 模式：https://support.enginedj.com/support/home ；https://support.enginedj.com/support/solutions/articles/69000882261-denon-dj-prime-4-g2-performance-pad-modes
- [S58] Mixed In Key – Cue Points / 工作流页：https://mixedinkey.com/wiki/how-cue-points-can-improve-your-djing/ ；https://mixedinkey.com/workflows/how-mixed-in-key-helps-djs/
- [S59] Mastering The Mix – LIMITER manual：https://www.masteringthemix.com/pages/limiter-manual
- [S60] Mastering The Mix 博客 A guide to using a smart limiter（2024）：https://www.masteringthemix.com/blogs/learn/a-guide-to-using-a-smart-limiter
- [S61] Mastering The Mix – FASTER MASTER manual：https://www.masteringthemix.com/pages/fastermaster-manual
- [S62] Mastering The Mix – BASSROOM manual：https://www.masteringthemix.com/pages/bassroom-manual
- [S63] Mastering The Mix – MIXROOM manual：https://www.masteringthemix.com/pages/mixroom-manual
- [S64] Youlean Loudness Meter 2 manual（v2.4.x，2021-01）：https://youlean.co/wp-content/uploads/2021/02/Youlean-Loudness-Meter-2-MANUAL.pdf
- [S65] NUGEN VisLM 产品页（ReMEM）：https://nugenaudio.com/vislm/
- [S66] NUGEN LM-Correct 产品页：https://nugenaudio.com/lm-correct/
- [S67] Waves WLM Plus Loudness Meter 用户手册：https://assets.wavescdn.com/pdf/plugins/wlm-plus-loudness-meter.pdf
