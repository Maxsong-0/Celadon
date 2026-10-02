# T-038 调研 C（续）：House 子类数据、曲风档案与交付开关建议

> 2026-10-01 · 调研代理 C · 接 `C-genre-delivery.md`（结论摘要与 §1–§4 在那份）。本文件含 §5–§7、未查到一手资料的问题、全部出处。
> 证据标记同前：【一手】【厂商自测】【二手】【推断】【本代理统计】。

## §5 House 子类（Deep / Tech / Melodic-Progressive）的母带特征

### 5.1 能找到的资料与等级

| 资料 | 样本 | 给了什么 | 等级 |
|---|---|---|---|
| Xu 等，arXiv:2509.11474（2025-09 提交，2026-03 修订）| Beatport 35 个曲风各取 Top 100，共 3500 首（2025-03），用 2 分钟 320 kbps 试听片段；特征含峰值因数、RMS 峰度、谐波 / 打击能量比等 | 聚类只找到 17–20 个自然声学族群，比 35 个商业曲风少近一半；House 各类落在同一个「Groove & Breaks」大簇；**Tech House 与 Minimal、Progressive House 与 Melodic House & Techno 在结构上合并**，作者认为这些区分多出于文化而非声学。**不给 LUFS** | 【一手（预印本）】 |
| Caparrini 等，JNMR 2020 的公开数据集（OpenML 43690）| 2018 年 Beatport 29 个曲风各 Top 100（数据集名写 201802，描述写 2018 年 11 月）；每首随机取 2 分钟，用 pyAudioAnalysis + Essentia 提特征 | 原始特征，含 Deep House / House / Tech House / Progressive House / Techno 等，**没有 Melodic House & Techno 类**。下面 5.2 是本代理对它做的统计 | 数据【一手】；统计【本代理统计】 |
| iZotope 博客（Ian Stewart，2025-04-08）| Billboard Hot Dance/Electronic 榜 10 首，从 Qobuz 买无损文件，不经归一 | 积分响度 −6.2 到 −11.3 LUFS，正文说平均约 −8.3；LRA 3.5–13.8 LU；多首真峰值 > 0 dBTP（最高 +1.50）。偏 House 的几首：CHRYSTAL《The Days》−10.3 LUFS / LRA 3.5；Adam Port《Move》−9.1 / 4.2；Dom Dolla《Dreamin》−7.7；John Summit《Focus》−7.0 / 5.4；Anyma《Hypnotized》（旋律 Techno）−7.0 / 6.6。哪首算哪个子类是本代理的判断 | 【厂商自测】 |
| MTM「Mastering With LEVELS」| 未说明 | 各曲风 LRA 均值：House 5.3 LU、EDM 5.9、Electronic 7.2 | 【厂商自测，方法未披露】 |
| TrackSensei 博客（2026-06-12）| 563 首已发行曲目：house 54、tech house 38、melodic techno 56 等 | 积分响度中位数 house −11.35、tech house −12.05、melodic techno −12.25 LUFS；真峰值中位数 +1.08 / +1.23 / +0.68 dBTP；20–60 Hz 与 60–250 Hz 能量占比 house 34.8% / 43.1%、tech house 35.0% / 44.9%、melodic techno 46.6% / 42.0%；单声道损失 0.46 / 0.33 / 0.31 LU；低频相关系数中位数 1.0（全单声道）。**和 iZotope 实测差了 3–4 LU，数据未公开、未经第三方核验，可信度低** | 【厂商自测，低可信】 |

没找到的：母带工程师或厂商发布的、**按 Deep / Tech / Melodic 分别给响度、动态、低频、宽度数值**的一手指南；以 LUFS / LRA / PLR 计、带公开数据的大样本同行评审研究。网上「Deep / Melodic 应做到 −8 到 −10、限幅 3–4 dB」这类说法都是二手经验谈，没有数据，本文不采用。

### 5.2 本代理统计：2018 Beatport Top 100 特征（每类 n = 100，中位数 [P25, P75]）

| 子类 | PAPR（dB，越大越没压扁）| 谱质心（归一到奈奎斯特）| 拍点能量 0–200 Hz 占比 | 拍点能量 3.2–22 kHz 占比 | BPM（Essentia）|
|---|---|---|---|---|---|
| Deep House | 11.2 [10.2, 12.6] | 0.194 [0.165, 0.227] | 0.82 [0.65, 0.94] | 0.035 [0.004, 0.144] | 122.1 [120.0, 123.0] |
| House | 11.4 [10.6, 12.2] | 0.266 [0.237, 0.290] | 0.58 [0.42, 0.81] | 0.171 [0.032, 0.363] | 123.9 [122.1, 125.0] |
| Tech House | 11.2 [10.2, 12.1] | 0.270 [0.228, 0.304] | 0.57 [0.33, 0.80] | 0.244 [0.062, 0.508] | 124.0 [123.0, 125.0] |
| Progressive House | 10.8 [9.8, 11.9] | 0.214 [0.177, 0.239] | 0.82 [0.60, 0.94] | 0.033 [0.013, 0.174] | 123.0 [122.0, 123.9] |
| Techno（对照）| 9.5 [8.6, 10.7] | 0.185 [0.160, 0.215] | 0.83 [0.72, 0.92] | 0.070 [0.014, 0.178] | 126.0 [125.0, 128.0] |

两两比较的效应量（AUC：0.5 = 完全分不开，越接近 0 或 1 越分得开）：
- PAPR：Deep vs Tech 0.53，Deep vs Prog 0.59，Tech vs Prog 0.57，House vs Tech 0.52，**Tech House vs Techno 0.79**。
- 谱质心：Deep vs Tech **0.19**（81% 的配对里 Tech 更亮），Tech vs Prog **0.78**，Deep vs Prog 0.44。
- 拍点低频占比：Deep vs Tech **0.73**，Tech vs Prog **0.28**，Deep vs Prog 0.51。
- 拍点高频占比：Deep vs Tech **0.25**，Tech vs Prog **0.74**。

口径与局限：
- PAPR = −10·log10(帧能量均值)。pyAudioAnalysis 提特征前会把整段去直流、按**采样峰值**归一到 ±1，并转成单声道，所以帧能量均值就是均方值对峰值²之比，PAPR 是基于采样峰值的峰均功率比。它**不是 LUFS，也不是 PLR**，只能横向比。【一手源码】[pyAudioAnalysis ShortTermFeatures.py](https://github.com/tyiannak/pyAudioAnalysis/blob/master/pyAudioAnalysis/ShortTermFeatures.py)
- 拍点频带占比来自 Essentia 的 BeatsLoudness。streaming 版默认频带边界是 0 / 200 / 400 / 800 / 1600 / 3200 / 22000 Hz，与数据集的 6 列吻合，**所以按默认边界解读，这一点是推断**。【一手源码】[beatsloudness.h](https://github.com/MTG/essentia/blob/master/src/algorithms/rhythm/beatsloudness.h)、[MusicRhythmDescriptors.cpp](https://github.com/MTG/essentia/blob/master/src/essentia/utils/extractor_music/MusicRhythmDescriptors.cpp)
- 每首只取随机 2 分钟，可能落在前奏或 breakdown 上；数据是 2018 年的，Beatport 分类之后改过；谱质心换算成 Hz 要假设 44.1 kHz 采样（这样 Deep 约 4.3 kHz、Tech 约 6.0 kHz，属推断）。

### 5.3 结论

1. **子类之间动态（压缩程度）基本没差别**：Deep、House、Tech、Progressive 的 PAPR 都在 11 dB 左右，AUC 0.52–0.59；Techno 明显更压（低约 2 dB，AUC 0.79）。没有数据支持「Melodic / Progressive 要留更多动态」，Progressive 这一类反而略压一点。【本代理统计】
2. **差别在频谱**：Deep 和 Progressive 更暗、拍点上低频占比更高；Tech House（以及泛 House）更亮、拍点高频（踩镲 / 打击乐）多。这和 arXiv 论文「House 子类声学上大面积重叠、只靠细节区分」一致。【本代理统计 + 一手】
3. **响度**：一手层面没有子类响度数据。iZotope 的个例（深一点的 −10.3 / −9.1，偏 Tech 的 −7.0 / −7.7）提示 Deep 往往做得轻一些，但只有几首，不足以定数值。【厂商自测 + 推断】
4. **宽度与低频单声道**：没有按子类区分的一手数据。Ozone「低频本来就窄就不去加宽」、TBC 单独看低频峰值因数、MTM 提醒俱乐部常以单声道收听，可以合并成一条「所有 House 子类低频保持单声道」的规则。【一手 + 推断】
5. BPM 区间重叠很大（Deep 约 120–123，Tech 约 123–125），**不能靠 BPM 分子类**，只能用来粗判「是不是 House」（Techno 约 125–128）。【本代理统计】

---

## §6 归纳：Celadon 的曲风档案与交付开关

### 6.1 曲风档案建议包含的量（不含绝对响度）

| 字段 | 内容 / 单位 | House 三子类的初值方向 | 依据 |
|---|---|---|---|
| ① 频谱目标 | 宽带倾斜（dB/oct）+ 分段目标「中心 ± 典型范围」，按**相对整混**的平衡表示，与响度无关 | Deep：最暗；Melodic/Prog：偏暗；Tech：最亮。绝对曲线建议照 TBC 的办法，用每子类几十首参考曲生成 | Ozone 子目标 Tonal Balance 与按曲风的 Clarity 倾斜；TBC 的隧道和自建目标；MTM 的「相对整混、扣掉响度差」；5.2 的谱质心 |
| ② 低频结构 | 低频带（如 <250 Hz，或 20–60 / 60–250 Hz 两段）占比目标；低频峰值因数区间；低频单声道策略（只收窄不加宽）| Deep、Prog：低频占比目标高于 Tech；三类都要求低频单声道 | TBC 低频 Crest Factor 表；Ozone 9 峰值因数 3–10 的「理想区」；Ozone 不加宽窄低频；BASSROOM 5 段相对目标；5.2 拍点低频占比 |
| ③ 微动态区间 | 以 PSR 中位数（smart:limit 口径）或极短时 / 短时响度比（Ozone 口径）给区间，可分 4 段 | **三子类共用一个 House 区间**；Techno 若以后加入，区间收紧约 2 dB | smart:limit：Profile 只管这个；Ozone Dynamics 子目标；5.2 的 PAPR |
| ④ 分段宽度 | 4 段 M/S 能量比目标；低频段只许变窄 | 三类相同（没有子类数据） | Ozone Width 子目标；TBC 3 Stereo Width；REFERENCE 3 的宽度描述 |
| ⑤ 结构先验（给学习门控用）| BPM 区间；前奏典型长度（小节）；有没有 breakdown → drop | Deep / Tech：鼓前奏 16–32 小节（需求方给的前提，本代理未查证）；Melodic/Prog：长 breakdown → drop | Ozone 12 的分析时长可按小节并跟随宿主速度；各厂商都要求分析最响最满的段落（drop / 副歌）；5.2 的 BPM。门控算法本身归 B |
| ⑥ 混合权重 | 允许「所选子类 + House 通用」按权重软混合，不硬切 | 例如所选子类 70%、通用 30%（数值为推断，需听测）| Ozone 9 按相似度加权混合；TBC 3 Target Blender；arXiv 显示子类声学重叠 |
| ⑦ 人声平衡（可选，不进第一期）| 人声对伴奏的响度差目标 | — | Ozone 11/12 Vocal Balance；TBC 3 |
| ⑧ 响度提示（不是目标）| 只给「本曲风常见的响段区间」，做越界提示；交付值由 §6.2 决定 | 不按子类分（证据不足）| Ozone 10 所有曲风同为 −10；smart:limit 响度由动态决定；LANDR、BandLab 响度 / 强度与风格分开 |

选择方式：第一期由用户选子类，默认「House 通用」；自动识别最多只给建议。依据：Ozone 也只在大类上自动识别（House / Techno 级别），没有子类目标；arXiv 显示 Tech↔Minimal、Prog↔Melodic 在声学上分不开，自动分类不可靠。【一手 + 推断】

不做的事：曲风档案**不带段落响度补偿**，和 Celadon 现有约束一致；没有任何产品文档把「按段落改增益」放进曲风目标。【一手归纳】

### 6.2 交付开关：两档 + 自定义

控制量：交付目标对的是**门控后学习窗的积分响度**，可以理解为「响段响度」，口径同 Ozone 12（在分析段上匹配，并写明整曲积分会不同）。整曲积分响度和真峰值在完整播放一遍后作为**结果**显示，因为平台按整曲积分归一（Spotify 按 ITU 1770，EBU R128 整段测）。【一手 + 推断】

| 档 | 默认响段目标 | 允许范围 | 真峰值上限 | 依据 |
|---|---|---|---|---|
| 流媒体（默认档） | **−10 LUFS** | −12 到 −8 | **−2.0 dBTP**（目标响于 −14 时）；可选 −1.0 | Ozone 10 出厂曲风目标 −10，Streaming 档 −1 dB 并开真峰值；Spotify 与 SoundCloud 官方：响于 −14 时 < −2 dBTP；AES TD1008 ≤ −1 dBTP；平台参考值 −14 / −16，再响也只是被调低。整曲积分会因前奏、breakdown 低于这个值，仍高于 −14，不会被抬升 |
| 俱乐部 | **−7 LUFS** | −9 到 −5（**硬上限 −5**）| **−0.5 dBTP**（推断）；确定只交 WAV 给 DJ 时可放到 −0.1 | Ozone 10 DJ Player = 流媒体档 +2 dB（即 −8）、天花板 −0.1 dB 关真峰值；MTM Club：给客户做到 drop 短时 −6 到 −8，更响的 −4 到 −6；iZotope 2025 舞曲榜积分平均 −8.3、最响一首 −6.2，短时冲到 −4.5 的那首能听出失真；Beatport 不归一；DJ 开 Auto Gain 会把更响抵消掉。−0.5 dBTP 取在 Ozone 的 −0.1 与 AES / Apple 建议的 1 dB 余量之间 |
| 自定义 | 用户填目标与天花板 | 同决策器钳位 | 用户填 | smart:limit 支持自定义参考响度；Ozone 12 可设 Output Level 与真峰值开关 |

和现有 GLAZE 的衔接（都是推断，需实现方核对）：
1. **切换交付或曲风都不重新聆听**：在已锁定的画像上重算增益和天花板，EQ、Clay、Color 等决策两版保持一致，保证同一首歌两版的音色相同。依据：sonible 说换 Profile 不必重学，Profile 只是换一种解读；Ozone 换交付档只改天花板、真峰值和推子范围。
2. **决策器钳位要按档分开**：现有钳位 −14 到 −7 LUFS 装不下俱乐部档。建议流媒体档用 −12 到 −8，俱乐部档用 −9 到 −5。
3. **俱乐部版的危险在「学错段」**：如果学习窗落在没有 bass 的鼓前奏，按 −7 推增益，到 drop 会被限幅器猛压。这正是 Ozone 12 文档承认的「分析段 ≠ 整曲」问题，所以交付档更依赖 B 的门控结果。
4. 现有「风格」（温润 / 冲击 / 通透）的偏好响度 −8.5 / −9 / −10，可以改成**交付档内的相对偏移**（比如 ±1 LU），不再当绝对值，避免和交付档打架。

---

## §7 与代理 A / B 的边界（一句话带过）

- A（各产品学习区段做法）：本文提到的学习时长和取段（Ozone ≥8 s、可设 ≤60 s 并按小节；smart:EQ 4 最少 6 s，可暂停后换段；EXPOSE / REFERENCE 自动取最响段；LANDR 逐帧调整）只作佐证，细节以 A 的报告为准。
- B（ARA2 / 段落与 drop 检测 / 门控学习）：本文只给「结构先验」字段和「俱乐部档依赖门控」的结论，算法以 B 的报告为准。

---

## 未查到一手资料的问题

1. Apple Music Sound Check 的目标数值：官方没给，−16 LUFS 只见二手。
2. YouTube 的归一参考值：官方只说会自动调整音量。
3. TIDAL：官方帮助中心没有归一说明，−14 LUFS 专辑归一来自 2017 年行业媒体对其委托研究的报道。
4. Amazon Music、Deezer 的归一数值：官方只有开关。
5. Bandcamp 是否归一：没有官方说明。Beatport 流媒体服务是否归一：没查到。
6. rekordbox、Traktor 的 Auto Gain 参考电平与算法；Serato 的算法以及 89–98 dB 与 LUFS 的换算；VirtualDJ 的「0 dBA」与 LUFS 的换算（只有 CTO 一条指向 ReplayGain 1.0 的论坛回复）。
7. Engine DJ 有没有 Auto Gain：手册里没有，属于由缺失推出的结论，加上社区功能请求佐证，没有官方明确声明。
8. sonible smart:limit / smart:EQ 4 的完整曲风 Profile 列表，以及每个 Profile 的动态区间数值。
9. LANDR Low / Medium / High 各对应多少 LUFS；BandLab 各预设与 Intensity 的数值。
10. Ozone 11 / 12 各曲风的目标响度数值；Ozone 12 文档说「十个」、博客列约 35 个，以哪份为准。
11. House 子类（Deep / Tech / Melodic-Progressive）以 LUFS / LRA / PLR 计、带公开数据的大样本一手实测。现有的只有厂商自测（iZotope 10 首、TrackSensei 563 首、MTM 分曲风 LRA）和本代理对 2018 特征数据集的二次统计（不是 LUFS）。
12. 「House 开头通常是 16–32 小节纯鼓 DJ 前奏」的统计依据：没查到，按需求方前提处理。
13. 俱乐部版真峰值上限的一手推荐：没有标准或平台给出，−0.5 dBTP 是推断。

---

## 全部出处（两份文件合计）

iZotope
1. 【一手】Ozone 9 Master Assistant 文档：https://docs.izotope.com/ozone9/en/master-assistant/index.html
2. 【一手】Ozone 10 Master Assistant 文档：https://docs.izotope.com/ozone10/en/master-assistant/index.html
3. 【一手】Ozone 11 Master Assistant 文档：https://docs.izotope.com/ozone11/en/master-assistant/index.html
4. 【一手】Ozone 12 Master Assistant 文档：https://docs.izotope.com/ozone12/en/master-assistant.html
5. 【一手·厂商】iZotope 博客 How to use Master Assistant in Ozone 12（2025-09-09）：https://www.izotope.com/community/blog/how-to-use-master-assistant-in-ozone
6. 【一手】Tonal Balance Control 1 文档：https://downloads.izotope.com/docs/tonal-balance-control/meters-and-target-curves/index.html
7. 【一手】Tonal Balance Control 2 文档：https://s3.amazonaws.com/izotopedownloads/docs/tbc2/en/targets-and-views/index.html
8. 【一手·厂商】Tonal Balance Control 3 产品页：https://www.izotope.com/en/products/tonal-balance-control-3
9. 【厂商自测】iZotope：A LUFS and tonal analysis of 10 top-charting dance and electronic songs（2025-04-08）：https://www.izotope.com/community/blog/dance-electronic-analysis

sonible
10. 【一手】smart:limit 手册（EN，2023-03）：https://www.sonible.com/wp-content/uploads/2023/03/manual-sonible-smartlimit_EN.pdf
11. 【一手】smart:limit 产品页：https://www.sonible.com/smartlimit/
12. 【一手】sonible 博客 smart:limit 响度与动态教程：https://www.sonible.com/blog/smartlimit-loudness-dynamics/
13. 【一手】帮助中心：smart:limit 学习时为何忽略发布目标：https://help.sonible.com/hc/en-us/articles/4412322979730-Why-does-smart-limit-ignore-my-selected-loudness-publishing-target-during-learning
14. 【一手】帮助中心：找不到合适的曲风 Profile：https://help.sonible.com/hc/en-us/articles/4412294276754-What-if-I-can-t-find-a-suitable-genre-profile-for-smart-limit
15. 【一手】帮助中心：流媒体响度参考区：https://help.sonible.com/hc/en-us/articles/6564156404764-What-does-the-loudness-reference-zone-displayed-for-streaming-platforms-like-Spotify-tell-me
16. 【一手】帮助中心：smart:EQ 4 学习时长：https://help.sonible.com/hc/en-us/articles/11847248626972-How-can-I-change-the-learning-time-for-smart-EQ-4
17. 【一手】帮助中心：何时需要重新学习（smart:comp 3）：https://help.sonible.com/hc/en-us/articles/24338893428508-When-do-I-have-to-re-start-the-learning-process
18. 【一手】smart:EQ 4 产品页：https://www.sonible.com/smarteq4/
19. 【一手】sonible 博客 Everything new in smart:EQ 4：https://www.sonible.com/blog/everything-new-smarteq4/
20. 【一手】smart:EQ 4 手册（日文版，2026-07）：https://www.sonible.com/wp-content/uploads/2026/07/manual-smEQ4_JP.pdf

LANDR / Mastering The Mix / BandLab
21. 【一手】LANDR：What are Mastering Styles?：https://support.landr.com/hc/en-us/articles/360019272934-What-are-Mastering-Styles
22. 【一手】LANDR：What are the LANDR loudness options?：https://support.landr.com/hc/en-us/articles/115009557127-What-are-the-LANDR-loudness-options
23. 【一手】LANDR：What is LANDR Mastering?：https://support.landr.com/hc/en-us/articles/115009725688-What-is-LANDR-Mastering
24. 【一手·厂商】MTM Mastering With LEVELS（含 Club 预设说明与分曲风 LRA 表）：https://www.masteringthemix.com/pages/mastering-with-levels
25. 【一手】MTM EXPOSE 手册：https://www.masteringthemix.com/pages/expose-manual
26. 【一手】MTM REFERENCE 手册：https://www.masteringthemix.com/pages/reference-manual
27. 【一手】MTM MIXROOM 手册：https://www.masteringthemix.com/pages/mixroom-manual
28. 【一手】MTM BASSROOM 手册：https://www.masteringthemix.com/pages/bassroom-manual
29. 【一手】BandLab：Using BandLab Mastering on your songs：https://help.bandlab.com/hc/en-us/articles/360001374513-Using-BandLab-Mastering-on-your-songs
30. 【一手】BandLab Mastering FAQ：https://help.bandlab.com/hc/en-us/articles/55678885417113-BandLab-Mastering-FAQ

平台
31. 【一手】Spotify for Artists：Loudness normalization：https://support.spotify.com/us/artists/article/loudness-normalization/
32. 【一手】Apple 支持：Sound Check：https://support.apple.com/en-us/109331
33. 【一手】Apple Digital Masters 技术简报（PDF）：https://www.apple.com/apple-music/apple-digital-masters/docs/apple-digital-masters.pdf
34. 【二手】MeterPlugs：Apple 改用 LUFS（2022-03-23）：https://www.meterplugs.com/blog/2022/03/23/apple-switch-to-lufs.html
35. 【一手】YouTube 帮助：Video & audio quality enhancements：https://support.google.com/youtube/answer/16619284
36. 【二手】Production Advice：Stats for nerds（2017-09-29）：https://productionadvice.co.uk/stats-for-nerds/
37. 【二手】audioXpress：TIDAL 专辑归一（2017-11-27）：https://audioxpress.com/news/tidal-implements-album-loudness-normalization-and-activates-it-by-default-for-mobile-players
38. 【一手】Amazon 帮助：Loudness Normalization：https://www.amazon.com/gp/help/customer/display.html?nodeId=TgG1d8mPjGwf885hRP
39. 【一手】Deezer 支持：Volume Normalization：https://support.deezer.com/hc/en-gb/articles/360005369938-Volume-Normalization-on-Deezer
40. 【一手】SoundCloud 帮助：是否按母带电平播放：https://help.soundcloud.com/hc/en-us/articles/360053660014-Will-SoundCloud-play-my-track-at-the-level-it-s-mastered
41. 【一手】Beatport 支持：所购曲目有问题怎么办（含「不归一」声明）：https://support.beatport.com/hc/en-us/articles/4412309674260-What-should-I-do-if-I-have-a-problem-with-a-track-that-I-purchased
42. 【一手，不涉及归一】Bandcamp 帮助：上传无损文件：https://get.bandcamp.help/en/articles/15263231-how-and-why-should-i-upload-lossless-files

DJ 软件
43. 【一手】rekordbox 7.2.18 手册：https://cdn.rekordbox.com/files/20260807093645/rekordbox7.2.18_manual_EN.pdf
44. 【一手】Serato：DJ Preferences（Use Auto Gain）：https://support.serato.com/hc/en-us/articles/223363407-DJ-Preferences
45. 【一手】Serato：Track Gain Adjust：https://support.serato.com/hc/en-us/articles/234774728-Track-Gain-Adjust
46. 【一手】Serato 博客 Auto Gain（2013-03-04）：https://serato.com/latest/blog/15577/auto-gain-whats-all-that-about
47. 【一手】NI 支持：Traktor 通道增益与 Auto Gain：https://support.native-instruments.com/support/solutions/articles/69000879529-traktor-setting-up-channel-gain-and-auto-gain
48. 【一手】Traktor Pro 3 手册：https://www.native-instruments.com/fileadmin/ni_media/downloads/manuals/traktor/TRAKTOR_PRO_3_Manual_English.pdf
49. 【二手·社区】NI 社区：AutoGain 按 BS1770/EBU R128（只读到标题）：https://community.native-instruments.com/discussion/comment/21777/
50. 【一手】Engine DJ 用户指南 v5.0.0：https://cdn.inmusicbrands.com/Software/ENDJ5/Engine%20DJ%20-%20User%20Guide%20-%20v5.0.0.pdf
51. 【二手·社区】Engine DJ 社区：Is Auto Gain available?（2024-06）：https://community.enginedj.com/t/is-auto-gain-available/56797
52. 【一手】VirtualDJ Options 列表：https://virtualdj.com/manuals/virtualdj/appendix/optionslist.html
53. 【一手·厂商员工论坛回复】VirtualDJ 论坛 AutoGain 怎么算（2019）：https://virtualdj.com/forums/227791/VirtualDJ_Technical_Support/How_does_VirtualDJ_determine_the_AutoGain-Values%3F.html
54. 【一手·开源实现】rsgain README（ReplayGain 2.0 默认 −18 LUFS）：https://github.com/complexlogic/rsgain

标准
55. 【一手】AES TD1004.1.15-10：https://aes.org/wp-content/uploads/2024/01/AESTD1004_1_15_10.pdf
56. 【一手】AES TD1008.1.21-9：https://aes.org/wp-content/uploads/2024/01/20210924_TD1008_v3.13.pdf
57. 【一手】EBU R 128-2023：https://tech.ebu.ch/docs/r/r128.pdf
58. 【一手】EBU R 128 s2-2021 Loudness in Streaming：https://tech.ebu.ch/docs/r/r128s2.pdf

数据与论文
59. 【一手·预印本】Xu 等，Acoustic Overspecification in Electronic Dance Music Taxonomy，arXiv:2509.11474：https://arxiv.org/abs/2509.11474
60. 【一手·论文】Caparrini, Arroyo, Pérez-Molina, Sánchez-Hernández（2020），Automatic subgenre classification in an electronic dance music taxonomy，JNMR：https://doi.org/10.1080/09298215.2020.1761399
61. 【一手·数据】OpenML 数据集 43690（Beatport Top 100 特征，2018-11）：https://www.openml.org/d/43690
62. 【一手·源码】pyAudioAnalysis ShortTermFeatures.py：https://github.com/tyiannak/pyAudioAnalysis/blob/master/pyAudioAnalysis/ShortTermFeatures.py
63. 【一手·源码】Essentia beatsloudness.h：https://github.com/MTG/essentia/blob/master/src/algorithms/rhythm/beatsloudness.h
64. 【一手·源码】Essentia MusicRhythmDescriptors.cpp：https://github.com/MTG/essentia/blob/master/src/essentia/utils/extractor_music/MusicRhythmDescriptors.cpp
65. 【厂商自测·低可信】TrackSensei：We analyzed 563 techno and house tracks（2026-06-12）：https://tracksensei.com/blog/we-analyzed-563-techno-house-tracks
