# T-038 调研 C：按曲风做母带的产品做法 + 俱乐部 / 流媒体交付数值

> 2026-10-01 · 调研代理 C（Opus 5.5 · max）· 本文件含 §1–§4；§5–§7、缺口清单、全部出处见同目录 `C-genre-delivery-2.md`。
> 范围：曲风档案该含哪些量、交付开关的档位与数值。学习区段做法归 A，段落 / drop 检测与 ARA 归 B，本文只在边界处一句带过。
> 证据标记：【一手】官方文档 / 帮助中心 / 手册 / 标准原文 / 论文 / 源码；【厂商自测】厂商博客公布的实测；【二手】第三方文章或社区；【推断】本代理推断；【本代理统计】本代理对公开数据集做的二次计算。

## 结论摘要

1. 各产品的「曲风」都只换**目标**、不换流程。核心是一条带容差的频谱目标曲线（Ozone、TBC、smart:EQ 4、MTM 都是）；Ozone 的曲风目标还含分段宽度、微动态、响度和人声平衡；smart:limit 的曲风**只定动态区间**，手册明说学习时不拿响度当目标。
2. 主流做法把**响度跟曲风拆开**：Ozone 10 所有出厂曲风统一 −10 LUFS，交付另选 Streaming / DJ Player；smart:limit 把发布目标和曲风分成两个控件；LANDR 的响度档和 BandLab 的强度档都独立于风格。
3. 没有产品做 Deep / Tech / Melodic House 子目标（Ozone 12 只有 House、Techno 等，smart:limit 有「Electronic – House」）。2025 年一篇 arXiv 论文（Beatport 数据）发现 Tech House↔Minimal、Progressive House↔Melodic House & Techno 在声学上分不开。
4. 本代理统计了 2018 年 Beatport Top 100 的特征数据：House 各子类的动态代理量几乎一样（约 11 dB），区别在频谱：Deep 和 Progressive 更暗、拍点低频占比更高，Tech House 更亮、高频打击乐更多。
5. 写清楚官方数值的平台只有 Spotify（−14 LUFS，Loud −11 / Quiet −19，母带响于 −14 时真峰值 < −2 dBTP）和 SoundCloud（−14，同样 −1 / −2 dBTP）。Apple、YouTube、TIDAL、Amazon、Deezer、Bandcamp 都没公布数值；Beatport 官方明说不归一。
6. 标准：AES TD1008（2021）建议音乐单曲归一到 −16、专辑最响曲到 −14 LUFS，真峰值 ≤ −1 dBTP。EBU R128 的 −23 LUFS 是广播口径，不管音乐母带做多响。
7. DJ 软件：Serato Auto Gain 的参考值可在 89–98 dB 间选，默认 92 dB；rekordbox 和 Traktor 只说增益来自分析，参考值和算法都没公开；VirtualDJ 指向 ReplayGain 1.0；Engine DJ 没有 Auto Gain。DJ 一开 Auto Gain，俱乐部版多出来的响度会被抵消。
8. 建议：曲风档案 = 频谱目标（倾斜 + 分段隧道）+ 低频结构 + 微动态区间 + 分段宽度 + 结构先验 + 混合权重，**不放绝对响度**。交付分两档：流媒体（响段 −10 LUFS、−2 dBTP）和俱乐部（响段 −7 LUFS、−0.5 dBTP、硬上限 −5）。切换交付或曲风都在已锁定的画像上重算，不重新聆听（细节见 -2 文件 §6）。

---

## §1 各产品的「曲风」到底改了哪些量

### 1.1 iZotope Ozone Master Assistant（各版本对照，全部【一手】）

| 版本 | 曲风目标来源 / 数量 | 谁来选 | 曲风改的量 | 响度怎么定 | 交付 / 输出 | 分析时长 |
|---|---|---|---|---|---|---|
| Ozone 9（文档 9.13.0）| 先把大量曲目归成 10 类曲风，再分析各类频谱特征 | 自动：按与各类的相似度百分比**加权混合**，得到这首歌专属的目标曲线 | EQ 目标曲线；Dynamics 模块按低频峰值因数决定开不开（全量程 −1~14，理想 3~10；偏 Orchestral 时改用全频带算） | 和曲风无关：Intensity 低 / 中 / 高 = −14 / −12 / −11 LUFS；参考曲模式按参考曲响度 | Streaming 天花板 −1.0 dB，CD −0.3 dB | ≥ 30 s，放全曲最响的部分 |
| Ozone 10（文档 10.4.0）| 10 个曲风目标，取自榜单热门曲 | 用户选目标 | 每个目标含 4 个子目标：Tonal Balance（频谱能量分布）、Stereo Width（各频段 M/S 功率比）、Dynamics（微动态，极短时 / 短时响度比，分 4 段由 Impact 匹配）、Loudness | **出厂曲风一律固定 −10 LUFS**；参考曲模式按参考曲最响的 8 s 匹配 | Streaming：−1 dB、开真峰值、推子中点 −10 LUFS；**DJ Player：−0.1 dB、关真峰值、推子范围整体上移 2 dB** | ≥ 8 s，放最响的部分 |
| Ozone 11（文档 11.0.0）| 同上，10 个 | 用户选，也可「检测到的曲风」 | 加 Vocal Balance（人声与伴奏的响度差）；Clarity 按曲风设倾斜（Tilt）；Imager 的 Recover Sides 用量随目标变；**不 Stereoize，低频本来就窄就不去加宽** | 文档写的是「达到所检测曲风的目标响度」，各曲风具体数值没公开 | Full Scale：−0.1 dB、关真峰值；Streaming：−1 dB、开真峰值 | ≥ 8 s |
| Ozone 12（文档 © 2026；官方博客 2025-09-09）| 博客列出约 35 个目标，含 House、Techno、EDM、Dance Pop、Drum & Bass、Dubstep、Future Bass 等，**没有 Deep / Tech / Melodic House**；文档概述仍写「十个」，两处不一致 | 分析完自动识别曲风（博客），用户可随时换 | 子目标同 11 代。Intensity 五档只缩放 EQ 增益、Imager 宽度、Impact、Clarity、Stabilizer、Soft Clip、Upward Compression，**不动响度** | Loudness 是一个单独的目标值，只在 Analysis Time 那一段上用 Maximizer 增益去对。**文档自己写明：只分析了一部分，整曲积分 LUFS 可能不同** | Output Level（dBFS）与真峰值开关可自设 | 可设到 60 s，单位选秒或小节，选小节时跟随宿主速度；建议分析最响最满的段落 |

出处：[Ozone 9 文档](https://docs.izotope.com/ozone9/en/master-assistant/index.html)、[Ozone 10 文档](https://docs.izotope.com/ozone10/en/master-assistant/index.html)、[Ozone 11 文档](https://docs.izotope.com/ozone11/en/master-assistant/index.html)、[Ozone 12 文档](https://docs.izotope.com/ozone12/en/master-assistant.html)、[iZotope 博客：Ozone 12 Master Assistant](https://www.izotope.com/community/blog/how-to-use-master-assistant-in-ozone)。

对 Celadon 的含义：
- Ozone 一个「曲风目标」= 频谱 + 分段宽度 + 分段微动态 + 人声平衡 + 响度。其中**响度在 10 代对所有曲风都一样**，交付档只改天花板、真峰值开关和推子范围（DJ 档比流媒体档高 2 dB）。这就是现成的「曲风 × 交付」两轴做法。【一手】
- Ozone 9 按相似度加权混合各曲风曲线、不做硬分类，可以照搬来做 House 子类之间的软混合。【一手 + 推断】
- 「只在分析段上对响度、整曲积分会不同」是 iZotope 自己在文档里承认的限制，和 Celadon 前奏问题同源（区段怎么选归 A/B）。【一手】

### 1.2 iZotope Tonal Balance Control（TBC）

- TBC 1：出厂只有 3 条目标曲线（Orchestral / Bass Heavy / Modern），由大量分曲风曲目的频谱统计得来。曲线不是一条线，而是「典型变化范围」（蓝色隧道）。Broad View 分 4 段：20–250 Hz、250 Hz–2 kHz、2–8 kHz、8–20 kHz。另有一块**只看低频的峰值因数（Crest Factor）表**，偏左表示低频动态太大，偏右表示压过头。可以拿单个文件或整个文件夹生成自定义目标。【一手】[TBC 1 文档](https://downloads.izotope.com/docs/tonal-balance-control/meters-and-target-curves/index.html)
- TBC 2（2.6.0）：结构与 1 代相同，有多条出厂曲线（文档没列数量），低频 Crest Factor 表保留。【一手】[TBC 2 文档](https://s3.amazonaws.com/izotopedownloads/docs/tbc2/en/targets-and-views/index.html)
- TBC 3：30+ 个曲风和子曲风目标，由数百首专业母带分析而来；新增 Target Blender（任两条目标混合）；表头从频谱扩到四块：Tonal Balance、Vocal Balance、Dynamics、Stereo Width。【一手（厂商产品页）】[TBC 3 产品页](https://www.izotope.com/en/products/tonal-balance-control-3)
- 含义：目标应该带「中心 + 典型范围」而不是一条硬线。低频动态是单独的一块。子类之间允许混合。

### 1.3 sonible smart:EQ 4 / smart:limit（另带一句 smart:comp 3）

- smart:EQ 4：Profile 分「乐器」和「整混（Mix Profiles）」两类，整混这一类按曲风分组；默认 Universal；也可以从参考曲生成 Profile。【一手】[smart:EQ 4 手册（日文版 2026-07）](https://www.sonible.com/wp-content/uploads/2026/07/manual-smEQ4_JP.pdf)、[smart:EQ 4 产品页](https://www.sonible.com/smarteq4/)。官方博客把 Mix Profile 描述为让整混在流媒体、电台、俱乐部都站得住。【一手】[sonible 博客](https://www.sonible.com/blog/everything-new-smarteq4/)。最少学 6 s 有效音频，最长可调，可以暂停后换到别的段落接着学（区段问题归 A）。【一手】[帮助中心](https://help.sonible.com/hc/en-us/articles/11847248626972-How-can-I-change-the-learning-time-for-smart-EQ-4)
- smart:limit：手册写明 Profile 是「基于曲风及其动态」；选 Profile 就设定了响度 / 动态格上的**推荐动态区域**。动态值 = 全部 PSR（峰值对短时响度比）的**中位数**，厂商说这比 PLR 更能跟住短时动态。学习时**拿动态值当目标，不拿响度**，选的发布目标在学习时被忽略，响度只是动态的结果。发布目标单独一栏，只带参考响度和推荐真峰值，供 Quality Check 提示用。【一手】[smart:limit 手册（EN，2023-03）](https://www.sonible.com/wp-content/uploads/2023/03/manual-sonible-smartlimit_EN.pdf)、[帮助中心：学习时为何忽略发布目标](https://help.sonible.com/hc/en-us/articles/4412322979730-Why-does-smart-limit-ignore-my-selected-loudness-publishing-target-during-learning)
- smart:limit 的曲风列表「相当丰富」但官方没公布全表【一手】[帮助中心](https://help.sonible.com/hc/en-us/articles/4412294276754-What-if-I-can-t-find-a-suitable-genre-profile-for-smart-limit)。官方教程里给 EDM 歌选的是「Electronic – House」Profile，说明 House 就是电子类下的一个子项。【一手】[sonible 博客：smart:limit 教程](https://www.sonible.com/blog/smartlimit-loudness-dynamics/)
- 流媒体参考区的口径：平台参考响度当作**下限**，往上都算合格；比参考更响只是被调低，不算问题。【一手】[帮助中心](https://help.sonible.com/hc/en-us/articles/6564156404764-What-does-the-loudness-reference-zone-displayed-for-streaming-platforms-like-Spotify-tell-me)
- smart:comp 3：**换 Profile 不需要重新学**，学到的信号观察不变，Profile 只是换一种方式解读。【一手】[帮助中心](https://help.sonible.com/hc/en-us/articles/24338893428508-When-do-I-have-to-re-start-the-learning-process)
- 含义：曲风只决定「动态该在哪个区间」，响度交给发布目标。换曲风等于对同一份画像重新解读，不必重听。

### 1.4 LANDR

- Mastering Styles 三种：Warm（偏复古、压缩更软）、Balanced（重平衡与清晰）、Open（现代、强调冲击与临场）。【一手】[LANDR 帮助：Mastering Styles](https://support.landr.com/hc/en-us/articles/360019272934-What-are-Mastering-Styles)
- 响度三档 Low / Medium / High。官方说 High 常被 EDM 和电子音乐人选，代价是动态；Low 最适合流媒体。**三档各对应多少 LUFS 没有公开。**【一手】[LANDR 帮助：loudness options](https://support.landr.com/hc/en-us/articles/115009557127-What-are-the-LANDR-loudness-options)
- 曲风不让用户选，引擎内部做「micro-genre detection」并逐帧调整（区段做法归 A）。【一手】[LANDR 帮助：What is LANDR Mastering](https://support.landr.com/hc/en-us/articles/115009725688-What-is-LANDR-Mastering)

### 1.5 Mastering The Mix（MTM）

- LEVELS：预设按**播放场景**分，不按曲风分（Mastering 下有 Apple Music、CD、Club、Composer、MFiT、Soundcloud、Spotify、Youtube、Crazy Loud）。各预设的响度阈值在 −6 到 −12 LUFS 之间，官方说这是「越过可能有害的阈值」而不是目标值。Club 预设说明里写：较响的俱乐部曲 drop 段短时 LUFS 在 −4 到 −6，作者给客户做到 −6 到 −8。另有按曲风统计的 LRA 均值表：House 5.3 LU、EDM 5.9、Electronic 7.2、Pop 6.4、Hip Hop 4.0（方法未说明）。【一手（厂商）】[Mastering With LEVELS](https://www.masteringthemix.com/pages/mastering-with-levels)
- EXPOSE 2：两个独立选择，先选播放平台预设（Spotify、YouTube、CD 等），再选音色平衡档（曲风预设或参考曲）。曲风预设取自商业成功曲目；偏差 ±3 dB 内算接近，±6 dB 以上算明显。音色平衡**自动取最响的段落（通常是副歌）**来分析。【一手】[EXPOSE 手册](https://www.masteringthemix.com/pages/expose-manual)
- REFERENCE 3：手册里没有曲风预设，靠用户自己的参考曲库，按 Tonal Balance、Stereo Width、Dynamics、Loudness 四项描述参考曲，自动循环参考曲最响的段落。【一手】[REFERENCE 手册](https://www.masteringthemix.com/pages/reference-manual)
- MIXROOM / BASSROOM：用曲风预设给出 EQ 目标。BASSROOM 固定 5 段（0–20、20–40、40–80、80–160、160–320 Hz），目标是低频**相对整混**的平衡，算法会扣掉响度差。自建目标时建议框选 drop 或副歌。有积分目标（累积）和短时目标（最近 3 s）两种。【一手】[MIXROOM 手册](https://www.masteringthemix.com/pages/mixroom-manual)、[BASSROOM 手册](https://www.masteringthemix.com/pages/bassroom-manual)
- 含义：MTM 也把「平台 / 场景」和「曲风音色」做成两个轴。低频目标用相对整混的比例，不用绝对电平。

### 1.6 BandLab Mastering

- 8 个预设：Universal、Fire、Clarity、Tape、Natural、Spatial、Cinematic、Punch。按**音色性格**分，不按曲风分；只有个别说明里提到曲风（Cinematic 对应管弦 / 影视，Punch 对应嘻哈 / 说唱）。另有 Intensity 三档（Light / Normal / Heavy）、Mastering EQ（低 / 中 / 高）和 Reference Mastering（匹配参考曲的音色、频谱平衡和动态）。**没有公开任何 LUFS 数值。**【一手】[BandLab 帮助：Using Mastering](https://help.bandlab.com/hc/en-us/articles/360001374513-Using-BandLab-Mastering-on-your-songs)、[BandLab Mastering FAQ](https://help.bandlab.com/hc/en-us/articles/55678885417113-BandLab-Mastering-FAQ)

### 1.7 汇总：曲风到底改了哪些量

| 产品 | 频谱目标 | 动态 | 立体声宽度 | 响度 | 人声平衡 | 谁来选 | 交付是否与曲风分开 |
|---|---|---|---|---|---|---|---|
| Ozone 10 | ✔ | ✔ 微动态 4 段 | ✔ M/S 4 段 | 曲风统一 −10（不随曲风变） | — | 用户 | ✔ Streaming / DJ Player |
| Ozone 11/12 | ✔ 另加 Clarity 倾斜 | ✔ | ✔ 不加宽低频 | 按曲风取目标（数值未公开） | ✔ | 自动识别 + 用户改 | ✔ Full Scale / Streaming |
| TBC 1/2/3 | ✔ 带隧道 | 低频峰值因数（3 代另有 Dynamics 表）| 3 代 ✔ | — | 3 代 ✔ | 用户，3 代可混合两条 | 不涉及 |
| smart:EQ 4 | ✔ | — | — | — | — | 用户（默认 Universal） | 不涉及 |
| smart:limit | — | ✔ 只管这个（PSR 中位数区间） | — | 学习时不以响度为目标 | — | 用户 | ✔ 发布目标单独选 |
| LANDR | 内部 | 内部 | 内部 | Low/Med/High（数值未公开） | — | 自动（micro-genre） | 风格与响度分开 |
| MTM EXPOSE/MIXROOM/BASSROOM | ✔（低频相对整混） | 仅表头显示 | 仅表头显示 | 预设按平台给阈值 | — | 用户 | ✔ 平台预设与曲风预设分开 |
| BandLab | 预设性格 | 预设性格 | Spatial 预设 | Intensity（数值未公开） | — | 用户 | 不涉及 |

结论：**频谱目标是共识核心**，动态和宽度是第二层；**没有一家把「绝对响度」做成曲风的主属性**（Ozone 11/12 有按曲风的响度目标但不公开数值，且与交付档分开）。【一手归纳】

---

## §2 平台响度归一：官方数值

| 平台 | 归一参考（官方原话转述） | 官方真峰值建议 | 抬升安静曲目吗 | 专辑 / 单曲 | 一手程度与出处 |
|---|---|---|---|---|---|
| Spotify | Normal −14 LUFS（ITU 1770），Loud −11，Quiet −19（Premium 可选）。Loud 档对安静、动态大的曲目加限幅器：−1 dB（采样值）起作用，攻击 5 ms，释放 100 ms | 建议母带 −14 LUFS、真峰值 < −1 dBTP；**母带响于 −14 时真峰值 < −2 dBTP** | 会，但留 1 dB 余量给有损编码（例：−20 LUFS、真峰值 −5 dB 的曲子只抬到 −16） | 整张专辑一起归一；随机播放或跨专辑播放时逐曲归一。网页播放器和第三方设备（电视、音箱）不归一 | 【一手】[Spotify for Artists：Loudness normalization](https://support.spotify.com/us/artists/article/loudness-normalization/) |
| Apple Music | Sound Check 把歌曲调到大致相同响度，数值写在元数据里，可按单曲或按专辑。**官方没给 LUFS 目标值** | 建议留至少 1 dB 余量，防止过采样回放时削波 | 未写明 | 支持按专辑 | 【一手】[Apple 支持：Sound Check](https://support.apple.com/en-us/109331)、[Apple Digital Masters 技术简报](https://www.apple.com/apple-music/apple-digital-masters/docs/apple-digital-masters.pdf)；−16 LUFS 的说法只见于【二手】[MeterPlugs 2022](https://www.meterplugs.com/blog/2022/03/23/apple-switch-to-lufs.html) |
| YouTube | 官方只说会自动调整音量和混音，另有 Stable volume 等增强。**没给数值** | 未写明 | 未写明 | — | 【一手】[YouTube 帮助](https://support.google.com/youtube/answer/16619284)；「Stats for nerds 里的 content loudness = 高出参考多少、只往下调」见【二手】[Production Advice 2017](https://productionadvice.co.uk/stats-for-nerds/) |
| TIDAL | **官方帮助中心查不到归一说明** | — | — | — | 【二手】[audioXpress 2017](https://audioxpress.com/news/tidal-implements-album-loudness-normalization-and-activates-it-by-default-for-mobile-players)：把专辑最响的一首归一到 −14 LUFS（BS.1770-4），不抬升安静曲目，移动端默认开；依据是 Eelco Grimm 对 420 万张专辑的研究，80% 受试者偏好专辑归一 |
| Amazon Music | 各 App 有 Loudness Normalization 开关，打开后所有歌同一响度。**数值未公开** | 未写明 | 未写明 | — | 【一手】[Amazon 帮助](https://www.amazon.com/gp/help/customer/display.html?nodeId=TgG1d8mPjGwf885hRP) |
| Deezer | 设置里有「Normalise volume / Normalize audio」开关。**数值未公开** | 未写明 | 未写明 | — | 【一手】[Deezer 支持](https://support.deezer.com/hc/en-gb/articles/360005369938-Volume-Normalization-on-Deezer) |
| SoundCloud | 播放时做响度归一；母带建议 −14 LUFS 积分 | < −1 dBTP；**响于 −14 时 < −2 dBTP**（因为转码成 Ogg/Vorbis、AAC） | 未写明 | — | 【一手】[SoundCloud 帮助（2026-09 更新）](https://help.soundcloud.com/hc/en-us/articles/360053660014-Will-SoundCloud-play-my-track-at-the-level-it-s-mastered) |
| Beatport | **不归一、不重新母带、不做任何处理**，文件就是厂牌交的 WAV | — | — | — | 【一手】[Beatport 支持](https://support.beatport.com/hc/en-us/articles/4412309674260-What-should-I-do-if-I-have-a-problem-with-a-track-that-I-purchased)（Beatport 流媒体服务是否归一：未查到） |
| Bandcamp | **未查到官方说明**；帮助中心只讲上传无损文件 | — | — | — | 【一手，但不涉及归一】[Bandcamp 帮助](https://get.bandcamp.help/en/articles/15263231-how-and-why-should-i-upload-lossless-files) |

对交付的含义：
- 流媒体版只要**高于** −14（Spotify / SoundCloud / TD1008 专辑最响曲）就不会被抬升或加限幅；更响只会被线性调低，听感上没好处。真正的硬约束是真峰值：响于 −14 时，两家平台官方都要求 < −2 dBTP。【一手 + 推断】
- 俱乐部版的主要去处（Beatport 下载）**不归一**，DJ 拿到的就是母带原样的响度。【一手】

---

## §3 DJ 软件自动增益：参考电平与算法

| 软件 | 官方写了什么 | 参考电平 | 算法 | 一手程度与出处 |
|---|---|---|---|---|
| rekordbox 7.x（下载页当前手册为 7.2.18） | 偏好设置 > Controller > Mixer > Auto Gain 打开后，自动调整载入 deck 的曲目音量；数值来自曲目分析，可在 GRID EDIT 手动改。DUAL PLAYER 布局说的是「载入分析时得到的**衰减量**」 | **未公开** | **未公开** | 【一手】[rekordbox 手册](https://cdn.rekordbox.com/files/20260807093645/rekordbox7.2.18_manual_EN.pdf) |
| Serato DJ | 建波形概览时计算每首的 Auto Gain：把曲目的**感知响度**拉到参考值所需的增益 | 下拉选 **89 dB（较轻）到 98 dB（较响），默认并推荐 92 dB** | 未写明（2013 年官方博客也没说） | 【一手】[Serato：DJ Preferences](https://support.serato.com/hc/en-us/articles/223363407-DJ-Preferences)、[Track Gain Adjust（±24 dB）](https://support.serato.com/hc/en-us/articles/234774728-Track-Gain-Adjust)、[Serato 博客 2013](https://serato.com/latest/blog/15577/auto-gain-whats-all-that-about) |
| Traktor Pro 3（本代理读的是 3.2 版手册文本） | Autogain 默认开，分析时存进曲目；总增益 = 通道增益 + Autogain；开了 Analysis Lock 就不能再改 | **未公开** | **未公开**；NI 社区有人请求「改按 BS1770 / EBU R128」，标题看现行算法不是 R128（推断，帖子内容没读到） | 【一手】[NI 支持](https://support.native-instruments.com/support/solutions/articles/69000879529-traktor-setting-up-channel-gain-and-auto-gain)、[Traktor Pro 3 手册](https://www.native-instruments.com/fileadmin/ni_media/downloads/manuals/traktor/TRAKTOR_PRO_3_Manual_English.pdf)；【二手】[NI 社区帖](https://community.native-instruments.com/discussion/comment/21777/) |
| Engine DJ | v5.0.0 用户指南全文**没有** Auto Gain 或 Track Gain 条目 | 不适用 | 不适用 | 【一手】[Engine DJ 用户指南 v5.0.0](https://cdn.inmusicbrands.com/Software/ENDJ5/Engine%20DJ%20-%20User%20Guide%20-%20v5.0.0.pdf)；【二手】[社区 2024-06：仍是功能请求](https://community.enginedj.com/t/is-auto-gain-available/56797) |
| VirtualDJ | autoGain：载入时把增益设到让整首歌的总体音量为 0 dBA（也会记住手动改过的值）；zeroDB 项额外留余量，防止多轨混在一起时失真 | 0 dBA（含义见算法列） | 2019 年论坛里，VirtualDJ CTO 回复「怎么算的」时直接贴了 ReplayGain 1.0 规范 | 【一手】[VirtualDJ Options](https://virtualdj.com/manuals/virtualdj/appendix/optionslist.html)、[VirtualDJ 论坛（CTO 回复）](https://virtualdj.com/forums/227791/VirtualDJ_Technical_Support/How_does_VirtualDJ_determine_the_AutoGain-Values%3F.html) |

对「俱乐部版做多响才好接歌」的含义：
- DJ 开着 Auto Gain（Serato / rekordbox / Traktor / VirtualDJ）时，各曲都被拉到同一参考，**更响的母带会被调低，只剩被压掉的动态**。DJ 不开 Auto Gain 时（例如 Engine DJ，或用外接调音台手动推增益），也是靠 trim 把各曲推平。所以「好接歌」的意思是：跟同一箱曲子的常见响度和频谱平衡接近，不是越响越好。【推断，依据上表各家文档】
- 各家都没公开参考值和 LUFS 的换算。ReplayGain 2.0 的参考是 −18 LUFS（见开源实现 [rsgain README](https://github.com/complexlogic/rsgain)），但 Serato 的「92 dB」和 VirtualDJ 的「0 dBA」怎么换成 LUFS，查不到一手资料，**本文不做换算**。

---

## §4 标准与推荐

| 文件 | 和音乐交付相关的条款（转述） | 出处 |
|---|---|---|
| AES TD1004.1.15-10（2015）| 针对「电台式」连续流：流的目标响度不高于 −16 LUFS、不低于 −20 LUFS；峰值不超过 −1.0 dB TP（防有损编码削波）；编码前限幅建议最多 1 dB 起步 | 【一手】[AES TD1004](https://aes.org/wp-content/uploads/2024/01/AESTD1004_1_15_10.pdf) |
| AES TD1008.1.21-9（2021-09-24，取代 TD1004）| 音乐：逐曲归一 −16 LUFS（上容差 +0.2 LU）；专辑归一时专辑最响的一首 −14 LUFS（上容差 +0.2），**强烈建议默认用专辑归一**，因为专辑里较轻的曲子本来就是按与最响曲的比例做的；所有内容在有损编码器输入端真峰值 ≤ −1 dBTP；文件**明确不对内容制作提建议**；长期目标是随设备进步再整体下调 6 LU，与 −23/−24 对齐 | 【一手】[AES TD1008](https://aes.org/wp-content/uploads/2024/01/20210924_TD1008_v3.13.pdf) |
| EBU R 128-2023 | 节目响度 −23.0 LUFS，容差 ±0.5 LU（直播 ±1.0）；真峰值 ≤ −1 dBTP（不同分发系统和码率可能要更低，见 Tech 3344）；**整段节目整体测量，不偏重语音、音乐等前景元素**；LRA 按 Tech 3342 测，短于 1 分钟不建议用 | 【一手】[EBU R 128](https://tech.ebu.ch/docs/r/r128.pdf) |
| EBU R 128 s2-2021《Loudness in Streaming》| 推荐以 −23 LUFS 原样串流并带响度元数据；过渡期分发响度可放在 −20 到 −16 LUFS；音乐服务可用元数据做「专辑归一 / 锚点归一」，保留曲目之间的艺术关系；给出 PLR（真峰值与节目响度之差，微观动态）和 LRA（宏观动态）的定义 | 【一手】[EBU R 128 s2](https://tech.ebu.ch/docs/r/r128s2.pdf) |

对 Celadon 的含义：
- 这些标准规定的是**分发端**怎么归一（−14 / −16 / −23）和真峰值上限（−1 dBTP），**都不规定音乐母带做多响**。所以流媒体版的响度是审美选择，标准层面的硬约束只有真峰值。【一手 + 推断】
- 归一测的是**整曲积分**（ITU-R BS.1770，R128 写明整段测量）。House 前奏、breakdown 会把整曲积分压低，所以「学习窗测到的响度」不等于「平台看到的响度」。Ozone 12 文档也承认这一点（§1.1）。这条直接影响交付档怎么定控制量，见 -2 文件 §6.2。【一手 + 推断】
