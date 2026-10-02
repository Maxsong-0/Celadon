# T-047 调研 C：多种曲风的实时母带策略（保留并扩展曲风层）

> 2026-10-02 · 调研代理 C（Sonnet 5.5 · max——主进程更正：工作流记录 `claude-sonnet-5-5` · max，代理原文自称 Opus 有误；只调研、没改仓库、没跑任何代码）· 主题：曲风层在「无聆听、无锁定、全程实时」下怎样保留并扩展，含不锁定的段落办法。
> 证据标记：【一手】论文 / 标准 / 官方文档 / 源码，本代理读过原文；【转述】经他人论文转述；【厂商自测】厂商博客自测、方法披露有限；【二手】；【读图】从论文图读出的近似值（约 ±0.5 dB）；【换算】本代理换算；【库内】Celadon 仓库 T-038 / T-045 的实测与常数；【推断】本代理推理；【未核实】。
> 参数可信度（§7 表里用）：① 库内数据直接支持；② 文献只支持方向、幅度是推断；③ 设计起点，必须试听 / 实测标定。
> 不重复 T-038 已做的：产品做法、平台数值、DJ 软件自动增益、House 子类统计见 `docs/validation/2026-10-01-T038-genre-mastering-research/C-genre-delivery*.md`；离线 drop 检测、ARA、宿主拍点见同目录 `B-technical*.md`。本文只补「多曲风」与「不锁定的段落办法」。
> 事故记录：WebFetch 的摘要模型对二进制 PDF 曾编出整张「各 EDM 曲风 crest / RMS 表」（Xu 等），已弃用；本文所有 PDF 数字都用 `pdftotext` / 读图从原文取。

## 结论（先看这 8 条）

1. **曲风不该换「压多狠」，只该换「目标 + 预算 + 结构模型」；响度上限用（真峰值上限 C，PSR 下限 F）两个数统一表达：T_main = C − F。** 7200 首主流流行 / 摇滚曲里，限幅量（HLSD）更取决于发行年而非曲风（ANOVA F 274.7 对 8.6），LRA 与年和曲风都弱相关（不含 EDM 与古典）[Deruty & Pachet 2015](https://www.ismir2015.uma.es/articles/136_Paper.pdf)；Dynameter 指南：任何曲风响段的 3 s PSR 都不宜低于 8，EDM / 金属只是 PSR 波动更小 [Dynameter 指南](https://www.meterplugs.com/files/dynameter-guide.pdf)；MTM 只给「响车道 −6 / 动态车道 −9～−11 LUFS 短期」两条、不按曲风分 [MTM](https://www.masteringthemix.com/pages/how-loud-should-you-master)。现行流媒体档（−10 LUFS，有效 Ceiling ≤ −2 dBTP）恰好是 F = 8；俱乐部档（−7 LUFS；常数里没有单独的俱乐部真峰值上限，取用户 Ceiling，默认 −1.0，T-038 报告推断 −0.5）对应 F ≈ 6 ～ 6.9，下文取 6.5【库内常数，本代理换算】。
2. **曲风间的频谱差异主要是「打击乐占比」，不是标签。** 12,345 首流行曲的长期平均谱：90 Hz～4.5 kHz 斜率不随打击乐占比变（PSD 口径 4.53 dB/oct，换成本库「粉噪 = 0」口径约 −1.5【换算】），而 <100 Hz 与 >2 kHz 两端同升同降（相对中位组：低频约 −12 ～ +6.5 dB，10 kHz 以上约 −11 ～ +5 dB【读图】）；作者结论是曲风间差异主要是打击乐占比的副作用 [Elowsson & Friberg 2017](http://www.diva-portal.org/smash/get/diva2:1108529/FULLTEXT02)。→ 目标 = 「中频斜率锚 + 两端宽容带」，两端只压超标、不补不足，再用实时打击乐占比代理微调。T-038 的教训同源：二手说法「Deep 暗 / Tech 亮」在 Jamendo 45 首上不成立（三类中位都是 −2.2）【库内】。
3. **曲风清单：保留 4 项、新增 9 项，共 13 项，按 6 个处理原型共用常数**（四四拍 / 大 drop / 低频断拍 / 人声主导节拍 / 乐队 / 动态保留），标定做 6 套而不是 13 套。依据：EDM 商业曲风声学上过度细分（35 个 Beatport 曲风只有 17～20 个自然族群，Tech House≈Minimal、Progressive≈Melodic）[Xu 等](https://arxiv.org/abs/2509.11474)；Ozone 12 的 34 个曲风目标（另有 All-purpose）里 Pop 系 8 个、EDM 系只有 House / Techno / EDM / DnB / Dubstep / Future Bass [iZotope 博客](https://www.izotope.com/community/blog/how-to-use-master-assistant-in-ozone)。
4. **不聆听、不锁定也能只让主段落决定目标：** T-038 的相对门（S_ref = 已听小节响度的 P95、ΔG 3 LU、低频门 0.6）本来就是「始终在线」的统计，要拿掉的只是「定稿 / 锁定 / 校准」状态机。做法 = 门控常在线 + 衰减记忆（τ = 1.5 × 32 小节 ≈ 93 s @124 BPM）+ 连续置信度 + 增益棘轮（主段度 m < 0.5 时指令增益只降不升）。45 首 Jamendo 上门控画像与人工标的 drop 响度之差：中位 0.15 / P90 0.60 / 最大 0.92 LU（不门控 0.75 / 1.40 / 3.07）【库内，离线整首口径；实时因果口径需复验】。
5. **breakdown 的实时线索因曲风而异：** 4/4 家族用三门（响度 + 底鼓与低频 + 中高频密度）——「去掉底鼓再让它回来」几乎每首 EDM 都有（Butler 2006，经 [Rocha 等 2013](https://eprints.illc.uva.nl/id/document/1170)），结构以 4 小节为周期 [Zehren 等](https://arxiv.org/abs/2007.08411)，T-038 打标也是这三个读数；Hip-Hop / Pop / Rock 没有「底鼓缺席」这条线索，只用响度相对门（ΔG 5～6 LU、P90）；原声 / 爵士 / 古典 / 氛围走「被动」，不做自动电平。
6. **交付档必须被曲风钳住：** 动态保留型曲风的 −10 / −7 档不适用，取 min(档目标, 曲风上限)：古典约 −16（AES TD1008 单曲上限）、原声 / 爵士约 −12、氛围约 −13。EBU Tech 3343 §9.3.2 指出宽 LRA 的古典与高压缩的摇滚现场听众习惯多放 2～3 LU，但不建议为音乐单设更高目标，且有意更轻的乐章 / 抒情曲应保留相对关系 [Tech 3343](https://tech.ebu.ch/docs/tech/tech3343.pdf)。另一个提醒：Spotify 归一到 −14 LUFS [Spotify](https://support.spotify.com/us/artists/article/loudness-normalization/)，所以流媒体档 −10（PSR≈8）没有响度收益，Dynameter 的说法是 PSR < 10 会被所有平台调低——可考虑把流媒体档默认放到 −11～−12，由 Max 定。
7. **开机即用靠「先验 → 证据」的连续过渡，不是学习期：** t = 0 起 L_ref 与置信度 c 由数据驱动，c 低时指令增益向保守先验收拢（沿用现有 `unlearned*` 常数的思路）；首个 drop 的过冲是因果系统的固有限度，由现有 GlazeGuard 兜底。工业先例同构：WebRTC AGC2 速率限制 6 dB/s、只在语音活动帧更新电平、带初始先验 [WebRTC audio_processing.h](https://webrtc.googlesource.com/src/+/main/api/audio/audio_processing.h)。
8. **成本：全部是控制面，不加延迟；CPU 估 < 0.2 % 单核【估算，未实测】。** 新读数（密度、底鼓调制、打击乐代理）都从 Balance 每 hop 已有的 27 段谱算，主段电平是 240 格直方图（每段读一次分位数）。

## 正文

### 1 曲风清单：保留 4 项、新增 9 项、按 6 个处理原型共用常数

| 菜单项 | 状态 | 原型 | 并入 / 拆分的理由与证据 |
|---|---|---|---|
| 通用 | 保留 | — | 不覆盖音色；新设计下改用「只看响度」的结构模型（§4）。 |
| Deep House | 保留 | A 四四拍 | T-038 现状。 |
| Tech House | 保留 | A | Tech House≈Minimal 声学上分不开，不再细分 [Xu 等]。 |
| Melodic / Progressive | 保留，并入 Afro / Organic House、旋律 Techno | A | Progressive≈Melodic House & Techno；Peak Techno 与 Psy-Trance 同属「Melodic & Atmospheric」枝 [Xu 等]。 |
| Techno | 新增 | A | 2018 Beatport Top 100 的 PAPR 中位 9.5 dB，比 Tech House 低约 1.7 dB（AUC 0.79）【库内 T-038 C-2 §5.2，本代理统计】：更密；EDMProd 手工分析 100 首称 75 % 的 Techno 一上来就是 drop [EDMProd]【二手·小样本】→ 结构更扁平。 |
| EDM / Trance（大 drop） | 新增 | B 大 drop | Mainstage、Dance Pop、Trance 同属「Commercial & Mainstage」簇 [Xu 等]；Ozone 12 有 EDM / Dance Pop / Future Bass 目标；这类 breakdown→drop 落差最大，对比保护最严。 |
| Drum & Bass / Bass Music | 新增（DnB + Dubstep + 重低音） | C 低频断拍 | DnB、Dubstep、140 / Grime、Hard Techno、Hardcore 在树里先合成最紧的一族，Trap 与半速 Dubstep 同簇 [Xu 等]；electronic / DnB / techno 低频突出，masters 里「严重削波」多见于 DnB 与 house [Mourgela 等 2024]；DnB 速度 160～190 BPM（Zehren 引 Vande Veire）。 |
| Hip-Hop / Trap | 新增 | D 人声主导节拍 | 低频（808）与人声居中是主轴；Pestana 等经 Elowsson 转述：嘻哈、摇滚、流行、电子的低频（≤150 Hz）与高频（≥5 kHz）都比爵士 / 民谣响【转述】。 |
| Pop / R&B | 新增 | D | 响度中位数几乎相同：Pop −9.5、R&B −9.6、Hip-Hop −9.7 LUFS [FB]；TBC 2 有「R&B / Hip-Hop」一条曲线【二手·搜索摘要，未读原页】。 |
| Rock / Metal | 新增 | E 乐队 | Metal −8.4、Rock −9.2（差 0.8 LU，用偏移项）[FB]；orchestral 与 metal 高频有峰 [Mourgela 等 2024]；宽度因曲风而异（Punk 窄、Post-rock 宽）[iZotope 博客]。 |
| Acoustic / Jazz | 新增 | F 动态保留 | Jazz −12.8、Singer-Songwriter −10.6、Country / Folk −9.6 [FB]：离散大，按 PSR 下限与上限钳位而不是一个固定响度。 |
| Classical / Orchestral | 新增 | F | 宽 LRA、不该被推响 [Tech 3343 §9.3.2]；Instrumental / Score 中位 −12.9 [FB]；Pro-L 2 的「Safe」风格专为古典 [FabFilter]。 |
| Ambient / Lo-fi | 新增 | F | 持续音天然 PSR 低、不必当「压过头」[Dynameter 指南]；Ambient 中位 −14.1 [FB]；Ozone 12 有 Ambient、LoFi 目标。 |

不单列：Hyperpop / K-pop / Latin / Reggae / Country 并入 Pop / Hip-Hop / Rock；Future Bass 并入 EDM；Hard Techno 并入 DnB / Bass（145+ BPM 与工业失真让它更靠近 Hardcore 而非中速 Techno [Xu 等]）。每个曲风项只带「对原型的偏移」，原型之间不共享常数。

### 2 频谱目标：用「枢轴」代替「整条斜率」

**怎么做 / 出处。**
- 事实：12,345 首流行曲、4096 点 STFT 的平均 PSD，在 1/6 倍频程平滑后：平均斜率 −2.35（200 Hz）、−3.67（400 Hz）、−4.99（800 Hz）、−6.30（1.6 kHz）、−7.62（3.2 kHz）、−8.94 dB/oct（6.4 kHz）；曲间标准差约 10 dB（60～80 Hz）→ 2.7～3 dB（200 Hz～1.2 kHz）→ 4.5～5 dB（2～4 kHz）→ 约 6 dB（10 kHz）→ ≥8 dB（15 kHz）【一手 + 读图】。另：低频比均值低 10 dB「并不罕见」，作者说此时低于均值「不那么值得担心」。
- 口径换算（PSD → 本库「粉噪 = 0」功率口径，+3 dB/oct）：200 Hz +0.65、800 Hz −1.99、3.2 kHz −4.62、6.4 kHz −5.94；不变区间 89 Hz～4.5 kHz 为 −1.53；Pestana 等的「约 5 dB/oct（100 Hz～4 kHz）」约 −2.0【换算】。本库 House 的 Jamendo 中位 −2.2（全频带回归）量级一致【库内】。
- 机理：按打击乐占比（HPSS 的 Lperc，均值 ≈ −15 dB、范围 −30 ～ −5 dB）分 11 组，低频与高频随 Lperc 单调同升同降，中频收敛；用 Lperc 条件化目标，平均误差从 3.94 dB 降 0.61 dB，且增益集中在 <100 Hz 与 >2 kHz [Elowsson & Friberg 2017]。
- 推断：Jamendo 三个 House 子类「斜率相同、谱质心却不同」与「斜率对两端同向变化不敏感」一致。结论：按曲风设不同的**全频斜率**，证据不足（T-038 已吃过一次亏）；曲风差异应放在**两端的先验位置与宽容度**。

**对 Celadon 的意义（规则）。**

| 频段 | 曲间 σ【读图】 | 打击乐占比摆幅（最低组 → 最高组，相对中位组）【读图】 | 规则 |
|---|---|---|---|
| < 80 Hz | ≈ 10 dB | 约 −12 → +6.5 dB（≈ 90 Hz 处） | 两端：只压超标（Tame），不补不足（该段 Recover 上限 = 0） |
| 100～150 Hz | 6 → 4 dB | 过渡 | 同上 |
| 200 Hz～1.2 kHz | 2.7～3 dB | 各组收敛到约 ±2 dB 内 | 锚区：自参照，死区 ±1 dB（现状不变） |
| 2～4 kHz | 4.5～5 dB | 开始分岔 | 过渡 |
| 6 kHz 以上 | 6 → ≥ 8 dB | 10 kHz 以上约 −11 → +5 dB | 同两端 |

- **实时打击乐占比代理**（Lperc_proxy，10·log10 的能量占比，与 HPSS 的 Lperc 同口径）：Σ_b Σ_t max(0, E_b(t) − S_b(t)) ÷ Σ E，E_b 取 Balance 每 hop 的 27 段功率，S_b 是该段的因果「持续底」（120 ms 最小值保持或 3 帧中值），8 s 泄漏平均。离线对 HPSS Lperc 标定，相关系数 r ≥ 0.8 才启用，否则只用曲风先验。
- 曲风先验（§7b 的「两端先验」）取 Fig.7 分组读数的**一半幅度**（保守）：高打击乐曲风 +2 ～ +4 dB，Pop 0，原声 / 爵士 −4 / −3，古典 −6 / −5，氛围 −8 / −7。方向有据、幅度待标。
- 备选（不是必须）：Tone 增加一个与现有倾斜（两个搁架反向）正交的「同向」自由度（低 / 高搁架同升同降），可直接承接这个「微笑」维度；不做也可以，用上面的宽容带实现。

### 3 响度与动态：交付档 × 曲风 = (C, F)

| 曲风 | 整曲积分响度 中位（IQR）[FB] | LRA | 响段 PSR / PLR 参照 | 出处与标记 |
|---|---|---|---|---|
| House | −9.1（−10.4 ～ −8.0） | 5.3（MTM）；榜单偏 House 的 3 首 3.5～5.4 | PAPR 11.2～11.4（2018） | [FB][MTM][iZotope 榜单]【厂商自测】【库内】 |
| Dance / EDM | −8.7（−10.4 ～ −7.4） | 5.9（MTM）；榜单样本 3.5～13.8（中位 5.8，n = 9） | 样本 PLR 6.4～11.2，最响三首（积分 −6.2 ～ −7.0）PLR 6.4～6.7，均值积分 −8.3 | 同上 |
| Techno | 无单列；Electronic −9.3（−11.0 ～ −8.0） | 未核实 | PAPR 9.5（比 Tech House 低约 1.7 dB） | [FB]【库内】 |
| Hip-Hop / R&B / Pop | −9.7 / −9.6 / −9.5（IQR 约 −11.2 ～ −8.1） | Hip-Hop 4.0、Pop 6.4（MTM） | Uptown Funk：PLR 11、min PSR 8 | [FB][MTM][Dynameter 指南] |
| Rock / Metal | −9.2（−11.0 ～ −7.8） / −8.4（−9.7 ～ −7.3） | 未核实 | DR ≥ 8 算可以；Death Magnetic min PSR 3 / PLR 5（反例）；Steven Wilson《Routine》典型 PSR 10（偶尔 7）、PLR 15 | [DR 数据库 FAQ][Dynameter 指南] |
| Jazz / Singer-Songwriter | −12.8（−15.2 ～ −10.8） / −10.6（−12.4 ～ −9.2） | 高，数值未核实 | Steely Dan《Aja》典型 PSR 12（min 10）、PLR 17 | [FB][Dynameter 指南] |
| Classical（Instrumental / Score） | −12.9（−16.5 ～ −10.1） | 高，数值未核实 | 古典 / 爵士的动态范围远大于现代流派、现代流派彼此聚拢（无具体 dB） | [FB][Kirchberger & Russo 2016] |
| Ambient | −14.1（−17.4 ～ −11.4） | 未核实 | 持续音天然低 PSR | [FB][Dynameter 指南] |

注：[FB] 是 31.5 万首的厂商测量，积分响度由模型投影、自述「约保守 1 dB」【厂商自测，方法披露有限】；[iZotope 榜单] 只有 10 首 Billboard 舞曲（−6.2 ～ −11.3 LUFS、真峰值最高 +1.5 dBTP，偏 Tech 的几首有可听失真）；Mourgela 等 15 万首 masters 的数据集在 [Zenodo](https://doi.org/10.5281/zenodo.13683186)（24.8 MB CSV，本代理未下载，需 Max 点头）。

**规则：T_main = clamp(C − F, T_min, T_max)。** 响段短期响度的上限 = 真峰值上限 − PSR 下限，C 与 F 由「交付档 × 曲风」给出；不需要新增测量（限幅器已有真峰值，短期响度已有），只是让各档各曲风的上限有统一物理含义。需要补一条保护：GlazeGuard 加「3 s PSR 实测 < F − 0.5 且持续 > 3 s → 以 0.5 dB/s 退让」【推断，数值③】。对照：学界的曲风目标是「短时电平分布的均值 / 方差」，但都是离线、拿整曲（[Hilsamer & Herzog 2014](https://www.dafx14.fau.de/papers/dafx14_marcel_hilsamer_a_statistical_approach_to.pdf)）；实时版的对应物就是 L_ref 与 PSR 分布。LRA 修正（现有 `lraSlopeDbPerLu` 1/3）要等 ≥ 60 s 才稳定（Tech 3341：前 60 s 必须标「尚不稳定」）[Tech 3341](https://tech.ebu.ch/docs/tech/tech3341.pdf)，故先用曲风先验 LRA，60～120 s 内线性交给实测。

### 4 段落结构：不锁定的实时办法

**4.1 为什么不需要聆听期。** T-038 的账本是「按小节记账 + 相对门」：S_ref = 已听小节响度的 P95（按有效时长加权），低频参考 = 过响度门各小节 lowShare 的 P95，两门都过 = 满编段；「门是相对的：drop 一来 S_ref 与低频参考上移，前奏小节自动掉出门外，不用清空重听」（`Source/dsp/SegmentLedger.h` 第四节）。标准先例：P95 防单次巨响（Tech 3342 的 LRA 用 P10～P95、−20 LU 相对门；Tech 3341 的 −10 LU 相对门，且明说实时表要从存下的分块响度重算阈值）[Tech 3342](https://tech.ebu.ch/docs/tech/tech3342.pdf)；FFmpeg ebur128 用能量直方图而不是无限增长的列表 [ebur128.c](https://ffmpeg.org/doxygen/5.0/ebur128_8c.html)。要拿掉的只是「定稿 / 锁定 / 校准」状态机。

**4.2 读数**（都来自 Balance 每 hop 的 27 段谱与 LoudnessMeter，已有；新增密度与调制两个）：
- L_seg：段响度（小节；没有拍点退回 2 s 段，同 T-038），L_ST：3 s 短期（≥ 10 Hz 更新）；lowShare：≤ 100 Hz 功率占比（现有）。
- K（底鼓在场）：40～130 Hz 包络的 P90 − P10（2 s 窗，不依赖 BPM；有 BPM 时可换成「每拍峰谷差」，即 T-038 打标的「每拍底鼓起伏」）；D（密度）：300 Hz～8 kHz 能量（T-038 打标的第三个读数）。

**4.3 主段度 m ∈ [0, 1]（软与，不是硬门，避免门边抖动）：** m = s_L · s_K · s_D，各 s 是以阈值为中心、固定宽度 w 的斜坡 ramp_w(x) = clamp(0.5 + x / w, 0, 1)：
- s_L = ramp_1.5(L_ST − (L_ref − ΔG))；s_K = ramp_0.15(lowShare ÷ lowRef − 0.6) × ramp_0.2(K ÷ K_ref − 0.6)；s_D = ramp_0.2(D ÷ D_ref − 0.6)。
- ref 都是「按 m 加权的衰减 P95」，与 L_ref 同一套直方图。ΔG = 3 LU、系数 0.6 是 T-038 标定值【库内①】，其余宽度与密度门是起点【③】。
- 退路：连续 60 s 内 m < 0.5 且 60 s 内 P95 − P10 < 3 LU（说明没有对比可保、多半是无底鼓的曲子）→ 该段退回 L 型（忽略 s_K、s_D），避免卡死在「永远保守」。

**4.4 主段电平 L_ref：** 定长直方图（0.1 LU 一格，与账本 / 分析器同格，约 240 格）+ 全局指数缩放法衰减（O(1) 更新，不逐格乘，不分配），权重 = m × 段有效时长，读 P95（L 型读 P90）；记忆 τ_mem = 1.5 × 32 小节 = 11520 ÷ BPM 秒（124 BPM ≈ 93 s，无 BPM 取 90 s；依据：EDM 的段落是 4 的倍数小节、常见 8 / 16 / 32 小节——Rocha 等：节奏循环按 2、4、8、16、32、64 分组；Zehren 等：每 4 小节是一个潜在新段）。关键性质：**均匀衰减不改变分位数**，所以 breakdown（m ≈ 0，不加权）期间 L_ref 纹丝不动；下一段主段进来，只需占衰减后质量的 5 % 就把 P95 推上去（τ = 93 s、约 28 个有效小节 → 至多约 1.4 小节 ≈ 2.7 s）。这就是「不聆听也能很快认出 drop」的原因。

**4.5 置信度与先验：** c = 1 − exp(−M ÷ M0)，M = 衰减后的有效主段小节数，M0 = 8 小节（T-038 的 `requiredFullSegments`；L 型取 20 s）。c 只做一件事：下游把「指令增益」在保守先验与「按 L_ref 算的值」之间插值（现有 `unlearnedInputLufs` / `unlearnedStrength` 就是先验）。**这不是学习期**：没有状态机、没有提交、没有 UI 阶段；t = 0 就工作，c 随证据连续上升，状态行改显「主段 ●/○」。

**4.6 给增益控制器的合同（棘轮，使「不把安静段推响」可测；与主题 A 对接）：**
- I1：m < 0.5 时，指令增益只降不升（上升斜率 ≤ +0.02 dB/s 的泄漏，仅在 m < 0.5 持续 > 60 s 且 L_ST > −60 LUFS 时才允许，防止换曲后卡死）。
- I2：breakdown→drop 落差处理后 ≥ 输入落差 − 1.2 LU（俱乐部 1.5 LU，T-038 门槛不变；L 型曲风取 1.0 LU，被动型取 0.3 LU【③】）。
- I3：任意宿主块长逐位相同（读数按 hop 样本计数；PPQ 小节只作对齐提示，沿用 SegmentLedger 的整数样本锚定）。
- 非主段的 Balance：Recover × (0.4 + 0.6·m)，稀疏频谱下不把被掩蔽判据用满【推断③】。

**4.7 各原型的变体：** K 型（A、B）三门全开；C 型（DnB / Bass）底鼓调制门放宽到 0.4（底鼓不均匀）并按 BPM 折叠（< 100 → ×2）；L 型（D、E、通用）只用 s_L（ΔG 5～6 LU、斜坡 3 LU、P90），因为主歌 / 副歌没有「底鼓缺席」这条线索，且需要保留的是副歌 > 主歌的对比；P 型（F）关闭自动电平。可选读数「上行铺垫」（底鼓缺席 + 中高频密度持续上升 = build / riser，Yadati 等：drop 出现在 build 之后并伴随 bassline 重入）可用来在 drop 前冻结，不是必需。

**4.8 验收（全用 T-038 现成资产，不需要新语料）：** 45 首 Jamendo + 人工标签（`labels-auto.tsv` / `labels-override.tsv`）+ 合成 House 曲做**因果重放**：(a) 第 16 小节起 L_ref 与标签 drop 响度之差中位 ≤ 0.3 LU、P90 ≤ 1.0 LU（T-038 离线口径 0.15 / 0.60 / 0.92）；(b) breakdown 期 L_ref 漂移 ≤ 0.5 LU；(c) 首个 drop 后 4 小节内 L_ref 与最终差 ≤ 1 LU；(d) 合成曲落差门槛；(e) 块长无关逐位相同。

**4.9 已知限度：** 首个 drop 之前没有证据，必然有一段过冲（由 Guard 兜底：1 s 触发、1 dB/s 退让）；T-038 实测整首离线口径下，每小节门控只挡住 60 % 的前奏小节、通过 85 % 的 drop 小节、挡住 80 % 的 breakdown 小节——DJ 前奏常常底鼓与低音都齐、只比 drop 轻 1～3 LU，**分类做不准，但「代表 drop 的电平」做得准**，所以本设计只依赖后者【库内】。

### 5 各原型的实时处理侧重

| 原型 | 瞬态（底鼓 / 军鼓） | 低频与立体声 | Glue / Clip / Limit | 限幅释放 | 依据与标记 |
|---|---|---|---|---|---|
| A 四四拍（Deep / Tech / Techno / Melodic-Prog） | 底鼓是最大瞬态：Glue 攻击 ≥ 20 ms（现状）让底鼓先过，侧链 100 Hz 高通（现状）；Clip 4× 软削底鼓尖峰 | 低切 25 Hz（Techno 28）、收单转折 120 Hz、不自动加宽（T-038）；很多 PA 把低频合并成单声道 [SOS 低频] | 沿用 T-045 风格预算（Glue 均值 1～2.5；Limit 均值 1.8～2.5 / P95 4～5.5 / 峰 6～9 dB） | 跟拍：一拍 ÷ 3，BPM 窗 [100,140] 扩到 [100,150]（Techno、Trance 到 140+） | ①现行；窗与收单频率②③ |
| B 大 drop（EDM / Trance） | Pro-L 2 的「Aggressive」= 近削波式限幅，厂商称适合 EDM / trance [FabFilter] → Clip 深度上限取「冲击」风格 6 dB / 活动 3 % | 同 A；pad / lead 允许加宽 0.3，低频不动 | 俱乐部档不再叠预算（T-045 已放宽）；breakdown→drop 门槛最严 | 同 A；risers 不进 L_ref（低频门挡） | ③ |
| C 低频断拍（DnB / Bass） | 军鼓 / 底鼓都是瞬态：Glue 攻击 12 ms ③；严重削波常见（Mourgela 等）→ Clip 用「冲击」上限 | 低切 28 Hz；收单 150 Hz；Glue 侧链高通 130 Hz（防 sub 触发 Glue）③ | 同 B | BPM 窗 [160,190] 或半速折叠（< 100 → ×2）；一拍 ÷ 3 ≈ 115 ms @174 BPM | ③ |
| D 人声主导节拍（Hip-Hop / Trap、Pop / R&B） | 人声居中：加宽只作用 S 的 2 kHz 高架（0.2～0.5），不动 M；808 → 低切 20 Hz、收单 100～120 Hz | 侧链高通 130 Hz（Hip-Hop）③ | Clip 中等（4 dB / 2 %）；Limit 同风格预算 | 自适应 100～800 ms，不跟拍（半速 / 不规则底鼓）；段落只用响度门 | ③ |
| E 乐队（Rock / Metal） | 鼓 + 吉他：Glue 攻击 10～15 ms、侧链高通 80 Hz ③；FabFilter：rock 用 Transparent / Dynamic（瞬态优先）[FabFilter] | 宽度 0.6（Metal 0.4）；收单 80 Hz | Clip 轻（3 dB / 1 %）；PSR 下限 8（Metal 7.5）：Death Magnetic min PSR 3 是反例 [Dynameter 指南] | 自适应 | ② |
| F 动态保留（Acoustic / Jazz、Classical、Ambient / Lo-fi） | 不动瞬态 | 宽度 0（古典：Ozone 11「不 Stereoize、窄低频不加宽」，T-038 C）；收单 70 Hz，古典关；低切 16～20 Hz | 无自动电平；Glue ≤ 0.5 dB、Clip 0、Limit 仅保峰值（均值 ≤ 0.5 / P95 ≤ 2 / 峰 ≤ 4 dB，古典 0.2 / 1 / 3）；Heat 古典 0 | 600～1500 ms；Pro-L 2「Safe」= 零失真、给古典 [FabFilter]；Katz K-20 给古典与宽动态 [FabFilter 手册] | ② |

交叉提醒（主题 B）：Clay 核心延迟 D 由「手动收单下限 40 Hz」固定、与参数无关（`ClayModule.h`），所以上表 ≥ 70 Hz 的收单转折不改延迟；若 B 为降延迟抬高该下限，低于新下限的曲风收单转折（原声 70、Rock 80、Pop 100）要同步抬高。

### 6 交付档在各曲风下的差异（T / C / F = 响段 ST 上限 LUFS / 真峰值 dBTP / PSR 下限 dB）

| 曲风 | 标准档（响段 ST 上限） | 流媒体档 | 俱乐部档 | 备注 |
|---|---|---|---|---|
| A 四四拍 | 沿用风格偏好（−8.5 ～ −10，现状） | −10 / −2 / 8（现状） | −7 / 用户 Ceiling（默认 −1；T-038 推断 −0.5）/ ≈ 6.5（现状；范围 −9 ～ −5） | 榜单 10 首：积分 −6.2 ～ −11.3，最响三首 PLR 6.4～6.7 [iZotope 榜单] |
| B EDM / Trance | −9 ③ | −10 / −2 / 8 | −6.5 / 同上 / 6 ③（范围 −8.5 ～ −5） | 更响更易失真：iZotope 指出 −6.2 / −7.7 的曲有可听失真 |
| C DnB / Bass | −9 ③ | −10 / −2 / 8 | −6.5 / 同上 / 6 ③ | 削波已多，不再加预算 |
| D Hip-Hop / Trap | −9.5 ③ | −10 / −2 / 8 | −8 / 同上 / 7.5 ③（DJ 版；要不要由 Max 定） | MTM 响车道 −6 ST；中位 −9.7 [FB] |
| D Pop / R&B | −10 ③ | −10 / −2 / 8 | −8 / 同上 / 7.5 ③（舞曲混音版） | 中位 −9.5 / −9.6 [FB] |
| E Rock / Metal | −9.5；Metal −8.5 ③ | −10 / −2 / 8（Metal 7.5） | 不适用，钳到标准档 | 俱乐部非主战场 |
| F Acoustic / Jazz | −12 ② | −12 / −1 / 11 | 不适用 | Jazz 中位 −12.8；《Aja》PSR 12 |
| F Classical | −16 ②（单曲上限，不推响，只在峰值余量允许时上抬 ≤ 3 dB） | −16 / −1 / 15 | 不适用 | AES TD1008 单曲 −16、真峰值 ≤ −1 dBTP [AES TD1008]；Spotify 抬安静母带时留 1 dB 余量、普通模式不用限幅 [Spotify] |
| F Ambient / Lo-fi | −13 ③ | −13 / −1 / 12 | 不适用 | Ambient 中位 −14.1 |

- 不适用的档：UI 灰显或提示「本曲风不适用」，运行时钳到曲风上限。
- 现状数值 −10 / −7 来自 T-038 / T-045（Max 已拍板），本表对 A 类不改；B / C / D 的俱乐部数值只是起点，试听定。
- 俱乐部档的最大风险是「DJ 开 Auto Gain 就抵消」（Serato 默认参考 92 dB，rekordbox / Traktor 不公开）——见 T-038 C §3，本文不重复。

### 7 曲风 × 参数总表（起点值）

**7a 结构与置信度**（模型：K = 底鼓三门；L = 仅响度相对门；P = 被动、不做自动电平）

| 曲风 | 结构模型 | ΔG（LU） | 低频门 / 密度门系数 | 分位 | τ_mem | M0 | 主段度保持线 m_hold | 标记 |
|---|---|---|---|---|---|---|---|---|
| 通用 | L | 6（斜坡 3） | — | P90 | 90 s | 20 s | 0.3 | ③ |
| Deep / Tech / Melodic-Prog | K | 3.0 | 0.6 / 0.6 | P95 | 11520 ÷ BPM（无 BPM 90 s） | 8 小节 | 0.5 | ① ΔG、低频门、P95；③ 其余 |
| Techno | K | 3.0 | 0.6 / 0.6 | P95 | 同上 | 8 小节 | 0.5 | ③ |
| EDM / Trance | K | 4.0 | 0.6 / 0.6 | P95 | 同上 | 8 小节 | 0.5 | ③ |
| DnB / Bass | K（底鼓调制门 0.4） | 3.0 | 0.6 / 0.6 | P95 | 66 s（@174 BPM） | 8 小节 | 0.5 | ③ |
| Hip-Hop / Trap · Pop / R&B | L | 5.0（斜坡 3） | — | P90 | 90 s | 20 s | 0.3 | ③ |
| Rock / Metal | L | 6.0（斜坡 3） | — | P90 | 90 s | 20 s | 0.3 | ③ |
| Acoustic / Jazz · Classical · Ambient | P | — | — | — | — | — | — | ② |

**7b 音色与立体声**（Δ全频斜率：相对 −2.25 dB/oct，本库粉噪 = 0 口径；**首发保守方案 = 全部 Δ 取 0，直到分曲风 Jamendo 标定完成**）

| 曲风 | Δ全频斜率 | 往亮上限（dB） | 往暗上限（dB） | 低切（Hz） | 收单转折（Hz） | 加宽系数 | 两端先验 低 / 高（dB） | 风格先验 温 / 冲 / 透 |
|---|---|---|---|---|---|---|---|---|
| 通用 | 沿用风格值 | 沿用 | 沿用 | 沿用 | 沿用 | 沿用 | 0 / 0 | 沿用自动 |
| Deep House | 0 ① | +0.4 ① | 0.75 | 25 ① | 120 ③ | 0 ① | +3 / 0 ③ | 0.5 / 0.2 / 0.3 ③ |
| Tech House | 0 ① | +0.4 ① | 0.75 | 25 ① | 120 ③ | 0 ① | +2 / +2 ③ | 0.2 / 0.5 / 0.3 ③ |
| Melodic / Progressive | 0 ① | +0.4 ① | 0.75 | 25 ① | 120 ③ | 0 ① | +2 / +1 ③ | 0.35 / 0.2 / 0.45 ③ |
| Techno | −0.25 ② | +0.4 | 0.75 | 28 ③ | 120 ③ | 0 | +3 / +1 ③ | 0.15 / 0.65 / 0.2 ③ |
| EDM / Trance | 0 ③ | +0.5 ③ | 0.75 | 25 | 120 ③ | 0.3 ③ | +2 / +3 ③ | 0.1 / 0.6 / 0.3 ③ |
| DnB / Bass | −0.25 ② | +0.4 | 0.75 | 28 ③ | 150 ③ | 0 | +4 / +2 ③ | 0.1 / 0.7 / 0.2 ③ |
| Hip-Hop / Trap | −0.25 ② | +0.4 | 0.75 | 20 | 120 ③ | 0.2 ③ | +3 / +2 ③ | 0.3 / 0.5 / 0.2 ③ |
| Pop / R&B | 0 ③ | +0.5 ③ | 0.75 | 20 | 100 ③ | 0.5 ③ | 0 / 0 | 0.3 / 0.3 / 0.4 ③ |
| Rock / Metal | +0.35 ② | +0.6 ③ | 0.75 | 30 ③ | 80 ③ | 0.6 / Metal 0.4 ③ | +1 / +3 ③ | 0.2 / 0.5 / 0.3 ③ |
| Acoustic / Jazz | −0.35 ③ | +0.3 | 0.5 | 20 | 70 ③ | 0.3 ③ | −4 / −3 ② | 0.3 / 0 / 0.7 ③ |
| Classical / Orchestral | −0.75 ③ | +0.2 | 0.4 | 16 | 关 | 0 | −6 / −5 ② | 0 / 0 / 1 ③ |
| Ambient / Lo-fi | −0.75 ③ | 0 | 0.5 | 20 | 80 ③ | 0.8 ③ | −8 / −7 ② | 0.6 / 0 / 0.4 ③ |

**7c 动态与释放**（预算沿用风格表；F 类给绝对值；释放规则见 §5）

| 曲风 | 限幅释放 | Clip 深度 / 活动占比上限 | Limit GR 上限 均值 / P95 / 峰（dB） | PSR 下限 标准 / 流 / 俱乐部 |
|---|---|---|---|---|
| Deep / Tech / Melodic-Prog / Techno | 跟拍 ÷3，BPM [100,150] | 风格表 | 风格表（1.8～2.5 / 4～5.5 / 6～9） | 7.5～9（由风格偏好换算）/ 8 / 6.5 ① |
| EDM / Trance | 跟拍 ÷3，BPM [100,150] | 6 dB / 3 % ③ | 风格表 | 8.5 / 8 / 6 ③ |
| DnB / Bass | 跟拍 ÷3，BPM [160,190] 或折叠 | 6 dB / 3 % ③ | 风格表 | 8.5 / 8 / 6 ③ |
| Hip-Hop / Trap · Pop / R&B | 自适应 100～800 ms | 4 dB / 2 % ③ | 风格表 | 9 / 8 / 7.5 ③ |
| Rock / Metal | 自适应 | 3 dB / 1 % ③ | 风格表 | 9 / 8 / —（Metal 7.5）③ |
| Acoustic / Jazz | 600～1500 ms | 0 | 0.5 / 2 / 4 ③ | 11 / 11 / — ② |
| Classical | 600～1500 ms | 0 | 0.2 / 1 / 3 ③ | 15 / 15 / — ② |
| Ambient / Lo-fi | 600～1500 ms | 0 | 0.5 / 2 / 4 ③ | 12 / 12 / — ② |

### 8 哪些值必须试听 / 实测标定

- **全部 ③**（见 7a～7c）：新曲风的斜率偏移、两端先验、密度门、τ_mem、M0、m_hold、B / C / D 的俱乐部目标与 PSR 下限、F 类预算、Glue 攻击与侧链高通、风格先验。
- **标定协议（复用 T-038 流水线）：** (1) MTG-Jamendo `autotagging_genre.tsv` 按曲风标签确定性取样，每类 15～40 首（T-038 已用到 deephouse / house / techno / progressive / trance 标签；hiphop / rock / metal / jazz / classical / ambient 等标签名以 `data/tags/genre.txt` 为准【未核实：本代理没列出 87 个标签】）[MTG-Jamendo](https://github.com/MTG/mtg-jamendo-dataset)，非商业研究用途；(2) 在 109 上用 `tools/render` 出：Balance 口径斜率、两端相对锚线的偏差、PSR / PLR 分布、逐小节的 L_seg / lowShare / K / D；(3) 按中位数与四分位定常数；(4) 用 librosa HPSS 算 Lperc，对打击乐代理做相关检验（r ≥ 0.8）；(5) 因果重放段落引擎（§4.8）；(6) 试听：每个原型 1 晚、每晚 3～5 首，A / B 对比开 / 关曲风偏移。
- **Jamendo ≠ 商业成品**（T-038 已声明）：响度与 PSR 类常数要用 Max 提供的商业参考曲交叉验证，Jamendo 只定音色与结构类常数。

## 给 Celadon 的具体建议

| 模块或决策 | 建议做法 | 预期效果 | 延迟 / CPU 代价 | 风险 |
|---|---|---|---|---|
| 曲风清单 | 13 项 / 6 原型，原型共用常数，曲风项只带偏移 | 标定量 6 套而不是 13 套；覆盖 EDM 近亲与非 EDM 主流 | 0 / 0 | 原型内差异被抹平，靠偏移项补 |
| 响度上限 | T_main = clamp(C − F)；曲风给 F 与上限；Guard 加 PSR 保护 | 动态型曲风自动不被推响；现行两档数值不变 | 0 / 可忽略（复用真峰值与短期响度） | 流媒体档 F = 8 处在 Dynameter「Limited」底线；俱乐部 F ≈ 6.5 低于其「任何曲风 ≥ 8」 |
| 主段电平 | SegmentLedger 常在线化 + 衰减记忆 + 置信度 + 棘轮（§4） | 无聆听期；drop 代表性误差预期同量级 0.15 LU（需因果重放复验）；breakdown 不被推响 | 0 延迟；CPU ≈ +0.05～0.1 %【估】 | 首个 drop 过冲；非 4/4 曲风信号弱 |
| 第三道门（密度） | 300 Hz～8 kHz 密度 ≥ 0.6 × 参考 | 补足「底鼓 + 低音齐」的 DJ 前奏（T-038 标签靠它） | 0 / 忽略 | 阈值未标定 |
| 底鼓在场读数 | 40～130 Hz 包络 P90 − P10，相对 K_ref 打分 | 不依赖宿主 BPM；Techno rumble 调制浅也能用相对值 | 0 / 忽略 | 无底鼓曲子靠 60 s 退路回 L 型 |
| 频谱目标 | 中频锚 + 两端宽容带；两端只压不补；打击乐占比代理微调 | 不再对低打击乐曲目错误「补低 / 补高」 | 0 / 忽略 | 代理与 HPSS Lperc 的相关性未验证 |
| 立体声与低频 | 曲风给收单转折 70～150 Hz；古典关；加宽系数按原型 | 俱乐部低频单声道、原声保持自然 | 沿用 Clay（D 由 40 Hz 下限固定，延迟见主题 B） | 若 B 抬高收单下限，70～100 Hz 的曲风转折要同步改 |
| 限幅释放 | 跟拍规则扩 BPM 窗 [100,150]，DnB 折叠；非 4/4 不跟拍；前瞻不随曲风变（Pro-L 手册推荐前瞻 < 0.5 ～ > 1 ms，由主题 B 定） | 各速度不双重抽吸 | 0 / 0 | 宿主 BPM 缺失时退回固定上限（T-038 B） |
| 交付档 × 曲风 | 动态型曲风钳制并灰显俱乐部 | 不会把爵士推到 −10 | 0 / 0 | UI 复杂度 |
| 风格先验 | 曲风给（温 / 冲 / 透）先验 + 实时特征证据（先验强度 λ，3 s～30 s 平滑） | 「自动」风格开机即合理 | 0 / 忽略 | 先验数值全是 ③ |
| 标定与验收 | §8 协议；§4.8 因果重放；合成曲门槛 | 全程可测 | 服务器上跑，本机不跑 | Jamendo 非商业成品 |

分期建议：一期 A / B / C（K 型，直接复用 T-038 的 45 首标签与合成曲验收）；二期 D / E（L 型）；三期 F（被动，改动最小）。

## 风险与未知

1. **证据质量分层很不均：** 频谱与动态的统计骨架来自同行评审（Elowsson、Deruty、Kirchberger、Mourgela），但都不是 EDM / 古典专门样本（Elowsson 数据集偏流行 / 民谣、含老录音与 MP3；Deruty 只含主流流行 / 摇滚）；各曲风响度中位数来自厂商博客（方法披露有限），榜单实测只有 10 首。**所有 ③ 数值都是起点，不是结论。**
2. **Pestana 等 2013 全文受限未取得**，其曲风结论只经 Elowsson 转述；「约 5 dB/oct」是读转述再换算【未核实原文口径】。
3. **打击乐占比代理未验证**：HPSS Lperc 是离线的 CQT + 中值滤波；因果的 27 段近似可能与它相关性不够，届时退回「只用曲风先验」。
4. **非 4/4 曲风的主段信号弱：** 只有响度一条线索，副歌 / 主歌的对比靠 ΔG 5～6 LU 保留；如果某首的「主歌」比「副歌」响（少见），L_ref 会跟着主歌，行为仍然合理（始终只跟最响的那一类）。
5. **首个 drop 过冲不可避免**（因果系统）；Guard 兜底，但 1 s 触发 + 1 dB/s 退让意味着 drop 开头 1～2 s 可能多 1～3 dB。
6. **宿主拍点不全**（T-038 B：Live 11 的 VST3 导出不报离线、FL 要勾选项、部分宿主缺上一小节起点）；块长无关性要求 PPQ 只作对齐提示、读数按样本计数。
7. **PSR 下限与外部观点冲突：** Dynameter 把「任何曲风 ≥ 8」当作母带底线，并说 PSR < 10 会被所有平台调低（指南写于 2016～2017，平台现状可能变）；本库流媒体档正好在底线、俱乐部档（F ≈ 6～6.9）低于它。这是用户选择，但应让 Max 知情。
8. **EBU Tech 3343 §9.3.2 的措辞**：「宽 LRA 古典与高压缩摇滚被听得更响约 2～3 LU」是广播经验，不是母带规范；仅作「不要为音乐单设更高目标、保留相对关系」的旁证。
9. **本代理没下载 Zenodo 数据集（24.8 MB）**，没有跑任何统计；若 Max 同意下载，可对 15 万首 masters 按 30 个用户标注曲风出积分响度 / 真峰值 / 削波率（但用户多为业余、masters 集中在 −14 LUFS 附近，用途有限）。
10. **Ozone 12 的曲风清单来自博客**（与文档「十个」不一致，T-038 已记）；本文只用它支持「清单粒度」的判断。
11. **F 类跳过过采样只省算力、不省延迟**：延迟必须恒定（宿主按上报延迟补偿），Heat / Clip 为 0 时最多跳过 8× / 4× 滤波器的计算。

## 出处清单

标记：【一手】本代理读过原文；【读图】用 pdftoppm 渲染后读图；【厂商自测】；【二手】；【库内】Celadon 仓库文件。

1. 【一手·读图】[Elowsson 2017] Elowsson & Friberg，《Long-term Average Spectrum in Popular Music and its Relation to the Level of the Percussion》，AES 142 #9762，12,345 首：http://www.diva-portal.org/smash/get/diva2:1108529/FULLTEXT02 ；AES 页 https://www.aes.org/publications/elibrary-page/?id=18638
2. 【一手·仅摘要】[Pestana 2013] Pestana 等，《Spectral characteristics of popular commercial recordings 1950-2010》，AES 135 #8960（全文受限）：https://repositorio.ucp.pt/entities/publication/814f2b66-7ebc-4815-99c4-73aaa1f0d533
3. 【一手】[Deruty & Pachet 2015] 《The MIR perspective on the evolution of dynamics in mainstream music》，ISMIR 2015：https://www.ismir2015.uma.es/articles/136_Paper.pdf
4. 【一手·无具体 dB】[Kirchberger & Russo 2016] 《Dynamic Range Across Music Genres and the Perception of Dynamic Compression in Hearing-Impaired Listeners》，Trends in Hearing：https://pmc.ncbi.nlm.nih.gov/articles/PMC4753356/
5. 【一手】[Mourgela 2024] Mourgela 等，《Exploring trends in audio mixes and masters》，AES 157 #10186（150,217 masters、30 曲风）：https://arxiv.org/abs/2412.03373 ；数据集 https://doi.org/10.5281/zenodo.13683186
6. 【一手】[Xu 等] 《Acoustic Overspecification in Electronic Dance Music Taxonomy》，arXiv:2509.11474（v2，2026-03）：https://arxiv.org/abs/2509.11474
7. 【一手】[Rocha 2013] Rocha、Bogaards、Honingh，《Segmentation and timbre- and rhythm-similarity in Electronic Dance Music》：https://eprints.illc.uva.nl/id/document/1170 （引 Butler 2006《Unlocking the Groove》）
8. 【一手】[Zehren 等] 《Automatic Detection of Cue Points for the Emulation of DJ Mixing》，arXiv:2007.08411：https://arxiv.org/abs/2007.08411
9. 【一手】[Yadati 2014] 《Detecting Drops in Electronic Dance Music》，ISMIR 2014：https://archives.ismir.net/ismir2014/paper/000297.pdf
10. 【一手】[Tech 3341] EBU Tech 3341（2023，EBU Mode）：https://tech.ebu.ch/docs/tech/tech3341.pdf
11. 【一手】[Tech 3342] EBU Tech 3342（2023，LRA）：https://tech.ebu.ch/docs/tech/tech3342.pdf
12. 【一手】[Tech 3343] EBU Tech 3343（2023，§3.3 LRA、§9.3.2 音乐响度偏移）：https://tech.ebu.ch/docs/tech/tech3343.pdf
13. 【一手】[Dynameter 指南] MeterPlugs / Ian Shepherd：https://www.meterplugs.com/files/dynameter-guide.pdf
14. 【一手】[Spotify] Spotify for Artists，Loudness normalization：https://support.spotify.com/us/artists/article/loudness-normalization/
15. 【一手·经 T-038 C 读过】[AES TD1008] https://aes.org/wp-content/uploads/2024/01/20210924_TD1008_v3.13.pdf ；摘要页 https://productionadvice.co.uk/td1008/
16. 【厂商自测】[iZotope 榜单] 《A LUFS and tonal analysis of 10 top-charting dance and electronic songs》：https://www.izotope.com/en/learn/dance-electronic-analysis
17. 【厂商自测】[FB] Freshly Baked Studios，《Average LUFS by Genre》（314,876 首）：https://freshlybakedstudios.com/blog/average-lufs-by-genre
18. 【一手·厂商】[MTM] Mastering The Mix：https://www.masteringthemix.com/pages/how-loud-should-you-master （两条车道）；https://www.masteringthemix.com/pages/mastering-with-levels （分曲风 LRA 表，T-038 C 记录，方法未披露）
19. 【一手·厂商】[iZotope 博客] Ozone 12 Master Assistant（34 个曲风目标）：https://www.izotope.com/community/blog/how-to-use-master-assistant-in-ozone
20. 【一手·厂商】[TBC] Tonal Balance Control（3 条原始曲线；TBC 3 有 30+ 目标）：https://downloads.izotope.com/docs/tonal-balance-control/meters-and-target-curves/index.html ；https://www.izotope.com/en/products/tonal-balance-control-3
21. 【一手·厂商】[FabFilter] Pro-L 2 的 8 种限幅风格：https://www.fabfilter.com/help/pro-l/using/advancedsettings ；Pro-L 手册（四种风格与 K-20 / K-14 / K-12）https://fabfilter.com/help/ffprol-manual.pdf
22. 【一手】[SOS 低频] Sound on Sound《Mixing Bass》（「many PAs sum low frequencies to mono」，未给 Hz 数）：https://www.soundonsound.com/techniques/mixing-bass
23. 【一手】[DR 数据库 FAQ] 「rock/metal DR ≥ 8 算可以；electronic DR 5 也可以」：https://dr.loudness-war.info/faq
24. 【一手·源码】[WebRTC] AGC2 配置默认值（headroom 5 dB、max gain 50 dB、initial 15 dB、max gain change 6 dB/s）：https://webrtc.googlesource.com/src/+/main/api/audio/audio_processing.h ；常数 https://webrtc.googlesource.com/src/+/main/modules/audio_processing/agc2/agc2_common.h
25. 【二手】[EDMProd] Beatport Top 100 手工分析（结构与开场）：https://www.edmprod.com/beatport-analysis/
26. 【一手·源码】[ebur128.c] FFmpeg libavfilter 能量直方图门控：https://ffmpeg.org/doxygen/5.0/ebur128_8c.html
27. 【一手】Hilsamer & Herzog，《A statistical approach to automated offline dynamic processing in the audio mastering process》，DAFx-14：https://www.dafx14.fau.de/papers/dafx14_marcel_hilsamer_a_statistical_approach_to.pdf
28. 【一手·仅摘要】Stark、Davies、Plumbley，《Real-time beat-synchronous analysis of musical audio》，DAFx-09（BTrack，宿主没有 BPM 时的备选）：https://dafx.de/paper-archive/details/XiEJjIc0a2Hb_KTJon0SaA
29. 【一手】[MTG-Jamendo] 数据集（Jamendo 流派标签、非商业研究用途）：https://github.com/MTG/mtg-jamendo-dataset
30. 【二手】Yamaha Hub 引用 Kirchberger & Russo 的「古典 20～32 dB、爵士 13～23 dB」（未能在原文核到）：https://hub.yamaha.com/audio/music/what-is-dynamic-range-and-why-does-it-matter
31. 【库内】T-038 调研：`docs/validation/2026-10-01-T038-genre-mastering-research/{REPORT,C-genre-delivery,C-genre-delivery-2,B-technical}.md`（产品做法、平台数值、DJ 自动增益、House 子类统计、宿主拍点）
32. 【库内】T-038 标定：`docs/validation/2026-10-01-T038-calibration/{REPORT,analysis}.md`、`docs/validation/2026-10-01-T038-reference/README.md`（Jamendo 流程、门控扫描、画像代表性）
33. 【库内】`Source/dsp/GlazeConstants.{h,cpp}`（风格与曲风常数、交付档、`unlearned*`）、`Source/dsp/SegmentLedger.h`（按小节记账与相对门）、`Source/dsp/ClayModule.h`（延迟 D 由 40 Hz 收单下限固定）
