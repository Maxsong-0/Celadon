# T-047 主题 A 调研报告：同类产品的「不锁定、连续自适应」做法与控制设计

> 2026-10-02 · 调研代理 A。只读调研：没改仓库任何源码 / 测试 / 脚本，没在本机跑测试或编译，没派子代理。
> 证据标签：**[一手]** 厂商手册、官方文档、专利、标准、同行评议论文；**[二手]** 评测、新闻、搜索摘要；**[推断]** 我从证据推出的设计，不是任何厂商公开的做法。
> 凡标 [推断] 的数字只是「建议初值」，必须用 T-038 / T-045 的门槛和 22 首曲子在服务器上扫参后才能定。「出处」行里的 S 编号对应文末清单（含 URL）；拿不到一手出处的写「未核实」。
> 注：Write 工具被框架拦截（提示子代理应以文本返回报告），本文件按任务书第 6 条回退为 report_markdown 返回，未落盘。

## 结论

1. **「无学习期、持续自适应」只在频谱 / 音色一侧有成熟先例，响度 / 动态预算一侧没有。** 实时派：Gullfoss（听觉模型约 1000 次/s、EQ 约 300 次/s、无学习，[S1](https://www.soundtheory.com/static/Gullfoss%20Operation%20Manual.pdf)）、soothe（相对邻域的自适应阈值，[S2](https://oeksound.com/manuals/soothe3)）、Bloom、Ozone Stabilizer / Clarity（[S10](https://docs.izotope.com/ozone11/en/stabilizer/index.html)、[S11](https://docs.izotope.com/ozone11/en/clarity/index.html)）、Pro-Q 4 自动动态 EQ（[S18](https://www.fabfilter.com/downloads/pdf/help/ffproq4-manual.pdf)）。响度 / 动态一侧，音乐母带插件全是「先学后固定」或「用户指定」：smart:EQ 4 / smart:comp 2 / smart:limit 学一次后固定（[S7](https://www.sonible.com/wp-content/uploads/2024/01/manual-smEQ4_EN.pdf)、[S9](https://www.sonible.com/wp-content/uploads/2022/05/manual-smartlimit_download.pdf)）；Ozone Master Assistant 至少听 8 s、最多 60 s，要用户放「最响最满的段」，Relearn 才重算（[S12](https://docs.izotope.com/ozone11/en/master-assistant/index.html)、[S16](https://docs.izotope.com/ozone12/en/master-assistant.html)）。持续自动的响度控制只存在于广播处理器（Orban、Jünger Level Magic、Dolby Volume Leveler），产品层面的时间常数都不公开（只有专利与标准给示例值）。→ Celadon 的「不锁定的目标响度」没有现成答案，只能借广播 AGC 的结构自己组合。
2. **防抽吸 / 防「骑」音乐 / 防漂移，有一手出处的共同套路只有四条：** (a) 慢环管平均、快环管峰值、前瞻限幅，多级不同速度串联（[A/85](https://www.atsc.org/wp-content/uploads/2026/07/A85-2026-07.pdf)；[Level Magic](https://www.tvtechnology.com/miscellaneous/jnger-audio-level-magic)）；(b) 低于门限就「冻结」增益而不是往回拉（[Orban Silence Gate](https://www.lost-manuals.com/pdf/orban--optimod-studio-8200st--service-user--ID9854.pdf)、[Kotelnikov RMS 释放冻结](https://docs.tokyodawn.net/kotelnikov-ge-manual/)、BS.1770 相对门）；(c) 事件门控：Dolby 专利让同一听觉事件内增益近似恒定，变化集中在事件边界（[US 8,144,881](https://patents.google.com/patent/US8144881B2/en)）；(d) 限权 + 响度中性：Stabilizer 最多 +9 dB、Clarity ±6 dB，Gullfoss 提削并用以保持感知响度。
3. **「只让主段落决定目标」在同类里全靠用户解决，算法化的只有相对门与分位数。** smart:limit / smart:comp 要求在「相对最响的段（副歌）」上学，Master Assistant 要「最响最满的段」；唯一算法化的是 BS.1770 相对门（−10 LU，A/85 明说它「倾向聚焦最响的部分、忽略其余」）和 EBU LRA 的 10～95 分位（[Tech 3342](https://tech.ebu.ch/docs/tech/tech3342.pdf)）。→ 实时版要用「上分位 + 相对门冻结」替代「用户选段」[推断]。
4. **smart:limit 的控制变量选择值得抄：以「动态」为目标，响度是结果。** 学习时以 PSR（峰值对短时响度）中位数为目标并明确忽略交付响度，理由是「响度是动态的结果」，交付目标只由 Quality Check 给提示（S9）。这与 Celadon「风格管动态预算、交付管响度与真峰值」同向；建议把 GR 预算当硬约束、响度目标当软目标：g = min(g_响度, g_预算) [推断]。
5. **开机即用：没有任何一家的文档写「预热渐入」。** 做法是三类：(i) 无长记忆（Gullfoss 决策窗约 20 ms，默认全 0 = 直通，静音时曲线回平）；(ii) 相对检测，免电平校准（soothe Soft、Spiff、Bloom）；(iii) 一次性工具要听 5～10 s（Maximizer 5 s、smart:EQ 最短约 6 s、Master Assistant ≥ 8 s）。可靠估计所需的数据量本身就在 5～10 s 量级，所以 Celadon 的「置信度渐入」取同量级 [推断]；smart:limit 的「置信区」是现成的界面语言。
6. **延迟与 CPU（官方数据）：** Gullfoss 约 21 ms（Live 约 2 ms，代价是「要预测未来」）；soothe3 常规 2304 样本（48 k 下 48 ms）、低延迟 0 样本（代价：没有前瞻，「反应更晚」）；Bloom 低延迟 64 样本（1.33 ms）；Pro-MB 前瞻 20 ms；Pro-L 2 真峰值额外约 5 ms；L3 固定 80 ms；Pro-Q 4 / smart:EQ 4 线性相位约 70 ms 起。Celadon 的 Balance（零延迟 + 分析晚 10～40 ms）与 soothe3 低延迟档是同一取舍；总延迟 ≤ 10 ms 有同行先例。官方几乎不公布 CPU 百分比（只有 Gullfoss「Best ≈ Normal 的 4 倍」这类相对量），20 % 预算无法用同行数字校准。
7. **界面：同类没有一家用「聆听 / 锁定」阶段文案描述实时处理**（这类文案只出现在先学后固定派的进度条 / 脉冲录音图标上）。实时派用：连续运动的曲线 + 汇总表（Gullfoss）、动态增益条与「A」标记（Pro-Q 4）、滚动增益轨迹（Elevate、Ozone）、Delta、响度匹配 A/B、Ride / Idle 状态（Vocal Rider）、置信区 + 响度直方图（smart:limit）。→ 状态行换成「在做什么 + 为什么不动」两类读数。
8. **总体回路建议（详见「给 Celadon 的具体建议」）：时间尺度分离的五层回路。** L0 样本～5 ms 峰值；L1 10 ms～1 s 的 Balance / Glue / 限幅释放；L2 1～10 s 的参考轮廓、GR 预算反馈、份额；L3 10～90 s 的节目响度与目标增益（慢到几乎不动：dg/dt ≤ 0.1～0.2 dB/s，冻结门，上分位）；L4 ≥ 60～120 s 的风格权重（再慢一档）；曲风目标与交付档是常量。外环必须比内环慢一个量级以上，且受内环约束（抗饱和）[推断]。

## 正文

### 1. 同类产品总表：实时 vs 先学后固定

| 产品 | 模式 | 检测量与节奏 | 控制设计要点 | 延迟（官方） | 出处 |
|---|---|---|---|---|---|
| Gullfoss（Standard / Master / Live） | 实时持续，无学习 | 听觉模型约 1000 次/s，EQ 约 300 次/s；决策需约 20 ms 时间窗（2018 新闻稿写「> 100 次/s」，1.11 手册写约 300） | Recover（抬被掩蔽的）+ Tame（压主导的）同时升降，保持感知响度；默认全 0 = 直通；旁通时引擎照跑；静音时曲线回平 | 约 20～21 ms；Live 约 2 ms（要预测未来，影响把打击乐从其余信号分开） | S1 S41 S42 |
| soothe2 / soothe3 / Soothe Live | 实时持续 | 相对邻近频率的自适应阈值；Soft「与输入电平无关」，Hard 更依赖电平；速度与频率相关（高频攻击更快），Detail 越高时间常数越慢；Soothe Live 比 soothe2 更依赖电平 | Delta 试听；无学习 | soothe3：常规 2304 / 线性相位 3456 / 低延迟 0 样本（≤ 48 k；低延迟「没有前瞻，反应更晚」）；Soothe Live 算法延迟 64 样本 | S2 S3 S4 |
| Spiff / Bloom | 实时持续 | Spiff「与输入电平无关」；Bloom「修正量随输入不断变化」，Amount 0～7 电平无关、> 7 才开始压缩 | Bloom 攻 / 释分别管上行 / 下行电平变化 | Bloom 低延迟 64 样本（48 k 下 1.33 ms） | S5 S6 |
| Accentize SpectralBalance | 实时持续（Dynamic 模式） | 每秒 > 50 次自适应；可自定义目标 | 官方称神经网络「CPU 占用低」（无数字） | 未核实 | S25 |
| iZotope Stabilizer | 实时持续 | 朝体裁 / 参考目标自适应；Speed、Smoothing、Sensitivity、Amount（最多 +9 dB） | Shape 模式等量提 / 削 → 响度中性；Tame Transients 对瞬态即时修正 | 未公布 | S10 S44 |
| iZotope Clarity | 实时持续 | 数百个频带朝粉噪（Tilt 可调）靠；Amount 最大 ±6 dB；攻 / 释毫秒可调；作用区 300 Hz～20 kHz | 官方称「同一积分响度下感知更响」 | 未公布 | S11 |
| Pro-Q 4 动态 EQ「Auto」/ 谱动态 | 实时持续 | 阈值「不断调整到带通后触发信号的电平」；攻 / 释 / 拐点随节目、频段、当前动态范围而变 | Auto Gain 只是按 EQ 设置「估算」，不是测量闭环 | 零延迟 / Natural Phase；线性相位低分辨率 3072 样本（44.1 k ≈ 70 ms） | S18 |
| Pro-MB / Pro-L 2 | 实时；攻 / 释「与节目、频段有关」 | Pro-L 2：前瞻「瞬态级」+ 随平均动态的「释放级」；声道链接：瞬态 < 100 %、释放 100 % | Pro-MB Dynamic Phase：零延迟、无预振铃 | Pro-MB 前瞻最多 20 ms；Pro-L 2 真峰值额外约 5 ms | S19 S20 |
| TDR Kotelnikov / Nova / Limiter 6 GE | 实时 | Kotelnikov：峰值路径 + RMS 路径并行；RMS 窗长按频率自动；峰值释放在高动态时自动加快；**RMS 释放「冻结」防底噪上升与抽吸**；Nova：Auto Release，前瞻 0.5 ms；Limiter 6 Mk II 自动检测最优释放 | 手册批评「按阈值 / 比率估算的自动补偿忽略时间参数」 | 未公布 | S21 S22 S23 |
| Newfangled Elevate | 实时 | 26 个听觉（MEL）频带；Adaptive Gain（各带 GR 之间最大差 dB）；Adaptive Speed（按带自适应攻 / 释 / 前瞻） | 可减频带数省 CPU；Match Level / Gain Lock 防响度偏见 | 未公布 | S24 S46 |
| Leapwing DynOne / LimitOne | 实时 | DynOne：每带 crest factor 在 min～max 攻（0～800 ms）/ 释（5～800 ms）间插值；LimitOne：频域自适应 + 时域瞬态两套算法（二手） | — | DynOne：Ultra 4096 样本 / Low Latency 65 样本且 CPU 少得多 | S47 S51 |
| Waves L3 | 实时 | ARC（自适应释放）：手动释放值只是方程的一项；Warm / Scaled 给低 / 高频不同自适应度 | 线性相位分频 | 固定 80 ms | S30 |
| sonible smart:EQ 4 | **先学后固定**（可选连续） | 学习最短约 6 s、约 10 s 出结果；「Adaptive」> 0 才持续自适应（手册截图为 0） | 学习阶段有进度条 | 线性相位约 70 ms；最小相位 0 | S7 |
| smart:comp 2 / smart:limit / pure:limit | **先学后固定** | 建议在「相对最响的段（副歌）」学；学完才解锁 Spectral Compression / 输入骑行 / 声音塑形；smart:comp 2 约听 10 s（二手） | smart:limit 以动态（PSR 中位数）为目标；输入骑行有 Speed / Intensity 上限；置信区 | smart:comp 2 需「一点前瞻」，数值未核实 | S8 S9 S49 S50 S53 |
| iZotope Master Assistant / Maximizer | **一次性** | ≥ 8 s、最长 60 s，「最响最满的段」；Relearn；Learn Input Gain 5 s | IRC：快对瞬态、慢对稳态低音；IRC 3 / 4 / 5「高延迟、非常耗 CPU」 | 未公布 | S12 S13 S15 S16 S17 S45 |
| Waves WLM | 测量 + 手动 Trim | Trim = 目标与长期响度之差，手册建议「测完再用，不要边播边调」 | 限幅器不带补偿增益 | — | S31 |
| Dolby Volume Leveler / IEQ | 实时持续 | 心理声学模型持续监听响度 / 频谱并与目标比较；听觉场景分析避免在「自然衰减的音符」里调增益；分声道分频带防抽吸 | IEQ：低频已够就不动，现代录音「不处理」 | — | S26 S35 S36 |
| Jünger Level Magic / Orban Optimod | 实时持续（广播） | Level Magic：慢 AGC + 瞬态处理 + 前瞻限幅同时跑；Optimod：平滑 leveler「慢骑增益」+ 更快的第二控制器 + Silence Gate 冻结 | 时间常数、dB/s 均未公开 | 未公布 | S27 S28 S29 |

对 Celadon 的意义：表里「实时派」都是频谱 / 瞬态 / 释放这类**相对量**的快环；响度目标一侧要么用户定、要么一次性学。Celadon 把两者合一，必须自己把「慢量」与「快量」分层（见「给 Celadon 的具体建议」）。

### 2. 实时派的控制回路拆解

#### 2.1 检测量：相对，而不是绝对
**怎么做：** soothe 把共振与邻域比较，Soft 档阈值自适应、电平无关；Pro-Q 4 自动阈值持续跟随带通后的触发信号；Spiff / Bloom 电平无关；Gullfoss 手册称「基于信号里的信息，而不是某种谱功率」；Dolby DEQ 用目标谱 / 测得谱之比，测得谱先做时间平滑再做频带平滑；BS.1770 的相对门（−10 LU）让测量只看最响的部分。
**出处：** [S2](https://oeksound.com/manuals/soothe3) [S5](https://oeksound.com/manuals/spiff/) [S6](https://oeksound.com/manuals/bloom/) [S18](https://www.fabfilter.com/downloads/pdf/help/ffproq4-manual.pdf) [S1](https://www.soundtheory.com/static/Gullfoss%20Operation%20Manual.pdf) [S36](https://patents.google.com/patent/US8090120B2/en) [S32](https://www.atsc.org/wp-content/uploads/2026/07/A85-2026-07.pdf)
**对 Celadon：** Balance「偏差 = 快跟踪 − 参考轮廓」已是相对设计，方向对。目标响度这一侧过去是绝对量（画像的 I），实时版必须改成「相对于本曲自己的主段落」。

#### 2.2 时间常数：快 / 慢多回路
**怎么做：**
- 听觉自带的时间尺度：Glasberg–Moore 模型的短时响度攻 / 释约 22 / 50 ms，长时响度攻 / 释约 100 / 2000 ms；EBU：Momentary 0.4 s、Short-term 3 s（刷新 ≥ 10 Hz），积分响度用 400 ms 块、75 % 重叠并门控。
- 助听器文献把增益控制分两类：快速（攻 0.5～20 ms、释 5～200 ms）与慢速 AVC（恢复 0.5～20 s）；「双前端 AGC」= 一路长、一路短的两个增益信号并用；快速压缩会压平调制深度、可听出「呼吸 / 抽吸」。
- 广播：多级不同速度串联，让峰值与平均电平分开控制；前瞻把检测与施加分开（A/85）；Level Magic = 慢 AGC + 瞬态处理 + 前瞻限幅同时跑；Optimod = 平滑 leveler「慢骑增益」+ 更快的第二控制器。
- AC-3 的 DRC 把「输入响度 − 平滑响度」分成四档时间常数：大幅超出走快攻；小幅超出走慢攻；小幅低于走「很慢的释放（软段里听不见）」；大幅低于走中速释放（响段结束后更快恢复）。
- Dolby 专利：增益平滑攻 10 ms、释 500 ms（半衰期），事件控制信号 250 ms；DEQ 的积分时间取低频 150～175 ms、高频 40～60 ms。
- 产品里的频率相关常数：soothe 高频攻击更快、Detail 高则更慢；L3 的 ARC 与 DynOne 的 crest factor 插值；Gullfoss 约 1000 次/s 模型 + 约 300 次/s EQ；SpectralBalance > 50 次/s。
- 在 dB 域做增益平滑可得平滑包络、无攻击滞后，论文推荐前馈结构（S39，仅核实到摘要级）。
**出处：** [S38](https://pmc.ncbi.nlm.nih.gov/articles/PMC5318944) [S33](https://tech.ebu.ch/docs/tech/tech3341.pdf) [S37](https://pmc.ncbi.nlm.nih.gov/articles/PMC4111434) [S32](https://www.atsc.org/wp-content/uploads/2026/07/A85-2026-07.pdf)（正文第 9.3 节与附录 F） [S28](https://www.tvtechnology.com/miscellaneous/jnger-audio-level-magic) [S27](https://www.lost-manuals.com/pdf/orban--optimod-studio-8200st--service-user--ID9854.pdf) [S35](https://patents.google.com/patent/US8144881B2/en) [S36](https://patents.google.com/patent/US8090120B2/en) [S2](https://oeksound.com/manuals/soothe3) [S30](https://assets.wavescdn.com/pdf/plugins/l3-multimaximizer.pdf) [S47](https://www.mixonline.com/technology/review-leapwing-audio-dynone-version-2) [S25](https://accentize.com/spectralbalance/) [S39](https://www.academia.edu/31481096/Digital_Dynamic_Range_Compressor_Design_A_Tutorial_and_Analysis)
**对 Celadon：** Balance 现状（10 ms hop、每 hop ≤ 1 dB、零延迟）已落在 Gullfoss / SpectralBalance 的节奏范围（100 次/s）。缺的不是「更快」，而是把**慢**的决策（目标响度、风格权重）显式放到 10～120 s 这一层并与快环隔离。

#### 2.3 迟滞与死区
**怎么做：** A/85：门控「可与其他级的阈值一起形成无处理空档」；Stabilizer 的 Sensitivity 0 = 允许过量共振、100 = 压一切偏离目标；soothe 的 Selectivity 只打最突出的共振；Dolby IEQ「低频已够就不动」。产品文档没有一家写「迟滞」本身；标准里的相对门（BS.1770 −10 LU、LRA −20 LU）是「阈值型死区」。
**出处：** S32 [S10](https://docs.izotope.com/ozone11/en/stabilizer/index.html) [S3](https://oeksound.com/manuals/soothe2/) [S26](https://professional.dolby.com/siteassets/pdfs/dolby-pc-entertainment-experience-v4-white-paper.pdf) S33 S34
**对 Celadon：** 现有 ±1 dB 死区是对的。离散决策（风格切换、Clay 开关、冻结状态）要加迟滞（进出阈值差 ≥ 1 LU / dB）和最短驻留 [推断]。

#### 2.4 感知加权（响度模型、掩蔽）
**怎么做：** Gullfoss 用计算听觉感知模型，Recover 抬被掩蔽的、Tame 压主导的；Tell 说滤波器的前 / 后振铃要按时间掩蔽设计（预振铃比后振铃更易听见）；Dolby 用特定响度（临界频带）+ 听觉场景分析；Elevate 用 26 个 MEL 听觉频带；smart:comp 2 谱压缩 > 2000 带；Clarity 数百带朝「粉噪」靠；Pro-Q 4 谱动态默认先加 3 dB/oct 倾斜再触发；响度测量用 K 加权 + 门控。
**出处：** S1 [S43](https://www.mixonline.com/blog/gullfoss-inventor-andreas-tell-tells-all) S26 S36 [S24](https://www.fullcompass.com/common/files/32095-EventideElevateUserGuide.pdf) [S8](https://www.sonible.com/wp-content/uploads/2022/07/manual-sonible_smartcomp2.pdf) S11 S18 S33
**对 Celadon：** 27 个 1/3 倍频程 + A 计权 + 上行掩蔽已处在业界「够用」档（Elevate 26 带）。可加的是「感知上的存在门」：对感知上不存在的频段不做 Recover（Gullfoss 静音时回平；Dolby 特定响度在静阈为 0）[推断]。

#### 2.5 防抽吸
**怎么做（均有一手出处）：**
1. 释放自适应：IRC I「对瞬态快、对稳态低音慢」；Pro-L 2「瞬态级 + 随平均动态的释放级」；Kotelnikov 峰值释放在高动态时自动加快、RMS 释放可冻结；DynOne crest factor；Elevate Adaptive Speed。
2. 分频带 / 分声道：A/85 讲宽带 AGC 会让一个频段被不相关的高能频段牵着「抽」；Dolby Volume Leveler 对单个声道与频带分别调整；Dolby DEQ 专利说各带独立应用增益会「被撕开」，所以对增益做频带平滑。
3. 事件门控（Dolby 专利）：同一听觉事件内增益近似恒定、变化集中在事件边界；专利例子里和弦衰减时传统 DRC 会把衰减抬起来，「极不自然」。
4. 前瞻把检测与施加分开（A/85）；Pro-MB 前瞻最长 20 ms。
5. 速度与深窄切口：soothe 的 Detail 越高越慢；Stabilizer 的 Speed / Smoothing「越快越准但可能有伪影」。
6. 节拍同步：Ozone Impact 的 Envelope 可按毫秒或「拍分」与宿主速度同步。
**出处：** [S17](https://s3.amazonaws.com/izotopedownloads/docs/ozone9/en/maximizer/index.html) [S19](https://www.fabfilter.com/downloads/pdf/help/ffprol2-manual.pdf) [S21](https://docs.tokyodawn.net/kotelnikov-ge-manual/) S47 S24 S32 S26 S36 S35 [S20](https://www.fabfilter.com/downloads/pdf/help/ffpromb-manual.pdf) S2 S10 [S14](https://docs.izotope.com/ozone11/en/impact/index.html)
**对 Celadon：** 已有的自适应释放、Tame 起音保护、House「一拍 ÷ 3」方向一致。可补两点 [推断]：(a) 稳态低音期间限幅释放放慢（IRC I 思路）；(b) 慢变量（Tone、份额、增益）只在事件边界附近更新，稀疏事件段（breakdown）自然冻结。

#### 2.6 防「骑」音乐（含 breakdown → drop）
**怎么做：**
1. 慢：AVC 恢复 0.5～20 s；Dolby 的 leveling amount「少调平允许更大的响度差」。
2. 限权：Stabilizer 最多 +9 dB、Clarity ±6 dB；Vocal Rider 范围默认 −6～+6 dB，有「Idle」位置并显示 Ride / Idle；smart:comp 2 输入骑行有 Speed 与 Intensity（最大偏移），且必须先学；Elevate Adaptive Gain 限制各带 GR 之差。
3. 响度中性：Gullfoss 提削并用；Stabilizer Shape「等量提 / 削」；Pro-Q 4 的 Auto Gain 只按设置估算；Kotelnikov 批评按阈值 / 比率估算的自动补偿「忽略时间参数」。
4. 冻结而不是往回拉：Orban Silence Gate「低于门限就冻结电平控制器的增益」，防止安静段底噪冲高、停顿中呼吸被夸大；Kotelnikov RMS 释放冻结。
5. 主段落由谁定：smart:limit / smart:comp「让它在相对最响的段（如副歌）上学」；Master Assistant「最响最满的段，如最后一段副歌」——全靠用户。算法化的只有 BS.1770 相对门与 LRA 的 10～95 分位（下分位防淡出主导、上分位防个别极响事件主导）。smart:limit 的界面直接画短时响度直方图，多峰 = 多个段落。
6. 控制变量：smart:limit 以 PSR 中位数为学习目标，忽略交付响度（S9 第 12、18 页）。
**出处：** S37 S26 S10 S11 [S48](https://josephnilo.com/blog/waves-vocal-rider-explained/) S8 S24 S1 S18 S21 S27 [S9](https://www.sonible.com/wp-content/uploads/2022/05/manual-smartlimit_download.pdf) S12 S16 S32 S34
**对 Celadon：** 这是 T-038「breakdown → drop 最多少 1.2 LU」门槛的核心。同行里没有「实时、不靠用户选段」的现成实现，只能用「冻结 + 上分位」替代「用户选段」（见建议 #1 #2）。

#### 2.7 防来回漂移
**怎么做：** (1) 不对称时间常数：快攻慢释（AC-3 四档；AVC）；(2) 死区 + 冻结（2.3、2.6）；(3) 限权（2.6）；(4) 平滑：Stabilizer Smoothing 100 时修正曲线像 3～4 个滤波器、0 时像一排滤波器；Dolby DEQ 时间 + 频带双平滑；(5) 幂等：Dolby IEQ 对已达标内容「不处理」；(6) 时间尺度分离（外环比内环慢得多）是通用控制工程做法，上述产品文档均未明说，倍数未核实 [推断]。
**出处：** S32 S37 S10 S36 S26
**对 Celadon：** 现有 Guard（3 s 窗、1 s 触发、1 dB/s 退让、0.3 dB/s 回升、安全 5 s）已是「中环」；新增的目标响度环必须比 Guard 再慢一个量级（≥ 15～30 s），并受 Guard 约束（抗饱和），否则两环会互相拉扯 [推断]。

### 3. 开机即用（没有学习期）
**怎么做：**
- Gullfoss：决策窗约 20 ms，默认全 0 = 直通，静音时曲线回平（评测）；旁通时引擎照跑（CPU 不降，但解除旁通时状态是热的）。
- 相对检测免校准：soothe Soft、Spiff、Bloom 0～7、Pro-Q 4 自动阈值。
- 一次性工具「最短听多久」：Maximizer Learn Input Gain 5 s；smart:EQ 最短约 6 s（第 7 页写约 10 s 出结果）；Master Assistant ≥ 8 s（最长 60 s）；smart:comp 2 约 10 s（二手）。
- 界面：smart:limit 的「置信区」——十字准线的间距表示测量置信度，重启测量或改参数后会「扩散」再收拢，不是「学习中 / 已完成」两态。
**出处：** S1 [S41](https://www.soundonsound.com/reviews/soundtheory-gullfoss) S2 S5 S6 S18 [S15](https://docs.izotope.com/ozone12/en/maximizer.html) S7 S12 S16 [S53](https://www.soundonsound.com/reviews/sonible-smartcomp-2) S9
**对 Celadon：** 没有同行文档写「预热渐入」，这块只能靠设计 [推断]：(1) 无长记忆的环（Balance）天然开机即用；(2) 有长记忆的环（目标响度、风格权重）用置信度 c 渐入，τ_c 取 6～8 s，与上面「最短听多久」同量级，c 以「通过门的音乐时长」计而不是墙钟；(3) 先验 + 证据收缩，权重 n/(n+n0)；(4) 死区随不确定度放大：偏差 < max(1 dB, k·标准误) 不动；(5) 旁通时引擎继续跑，A/B 切换不重爬。

### 4. 延迟、前瞻与 CPU（官方数据优先）
**怎么做 / 数据：**

| 产品 / 模式 | 官方数字 | 官方说的代价 |
|---|---|---|
| Gullfoss / Master | 约 20～21 ms（需要约 20 ms 窗）；采样率 16～384 kHz | Best 质量约 Normal 的 4 倍 CPU；Master 更高 |
| Gullfoss Live | 约 2 ms | 「要预测未来」，影响快速变化与把打击乐分离的能力 |
| soothe3 | 常规 2304、线性相位 3456、低延迟 0 样本（≤ 48 k）；88.2 / 96 k：4704 / 7008 / 96 | 低延迟档没有前瞻，「反应更晚」（如颤音） |
| Bloom 低延迟 | 48 k：64 样本（1.33 ms）；192 k：448 样本（2.33 ms） | 未写 |
| smart:EQ 4 | 线性相位约 70 ms；最小相位 0 | 线性相位适合整曲；最小相位适合现场 / 瞬态强的信号 |
| Pro-Q 4 | 零延迟 / Natural Phase；线性相位 3072（≈ 70 ms）、5120（≈ 116 ms）、9216（≈ 209 ms）@44.1 k；谱动态强制线性相位 | 线性相位有预振铃；动态 EQ 只到 High 分辨率 |
| Pro-MB | 前瞻最长 20 ms；Dynamic Phase 零延迟 | Dynamic Phase「相位变化只在增益改变时出现」 |
| Pro-L 2 | 真峰值额外约 5 ms；4× 过采样 + ≥ 0.1 ms 前瞻 → 样本间峰值在 0.1 dB 内 | 前瞻 < 0.1 ms 近似硬削波；部分算法需更多延迟 |
| Waves L3 | 固定 80 ms（5 段线性相位） | — |
| DynOne | Ultra 4096 样本（44.1 k）/ Low Latency 65 样本 | Low Latency CPU 少得多 |
| Ozone Maximizer | 各 IRC 数字未公布；IRC 3 / 4 / 5「高延迟、非常耗 CPU」；SOS 测 Unlimiter 85 ms（二手） | — |

**出处：** S1 S2 S6 S7 S18 S20 S19 S30 S47 S13 S15 S45 [S40](https://www.soundonsound.com/reviews/soundtheory-gullfoss-v110)
**对 Celadon：** 实时自适应在业界「质量档」的延迟就是 20～50 ms，「低延迟档」都明说有代价。Balance（零延迟 + 分析晚 10～40 ms）与 soothe3 低延迟档同一取舍，合理。总延迟 ≤ 10 ms 有先例（Pro-L 2 真峰值 +5 ms 量级、Bloom 1.3 ms），代价集中在 Clay 线性相位与限幅前瞻（交给延迟主题）。FabFilter 的「Dynamic Phase」说明「增益为 1 时相位平直、不引入延迟」的多段处理有人做到，但实现未公开。新增的慢环（直方图 + 积分）CPU 估算 ≪ 0.1 %（每 100 ms 一次直方图衰减、每秒一次分位，几百次乘加）[推断]，不是预算问题。

### 5. 界面怎样说明「实时在做什么」
**怎么做：**
- Gullfoss：EQ 图连续运动，手册说用它「理解 Gullfoss 想修什么」；Recover / Tame 表只显示正 / 负增益，Bias 表显示谁处理更大的频率范围，亮度表显示相对输入的亮度偏差；红色阴影表示处理强度。
- Pro-Q 4：动态增益变化 = 增益环里的黄条；阈值滑块显示触发信号电平，顶端「A」= 自动。
- Ozone：Stabilizer 的 Difference Meter + Delta；Master Assistant / Impact 的滚动波形 + 增益轨迹。
- Elevate：滚动 GR 表（分带），手册称它「最能说明 Elevate 在时间 × 频率上做什么」；Match Level / Gain Lock 保证公平 A/B。
- smart:limit：响度 & 动态网格（十字准线 + 置信区 + 短时响度直方图 + 推荐区变色）、Quality Check 箭头建议、Instant Impact Prediction（改参数立即反映在读数里）、Constant gain / Delta。
- Vocal Rider：「Ride / Idle」活动显示，只说**为什么现在不动**。
- 先学后固定派才有「学习中」进度条与脉冲录音图标（smart:*）。
**出处：** S1 S18 S10 S12 S14 S24 S9 S48 S7
**对 Celadon：** 取消「聆听 / 锁定」后，状态行改成两类读数：(1)「在做什么」：短时响度对目标的差、慢增益 g、各模块预算占用、Balance 实时曲线；(2)「为什么不动」：跟随 / 保持·本段较轻 / 保持·静音 / 受限·预算 / 受限·交付峰值。另配滚动轨迹、短时响度直方图（让「主段落」可见）、置信区、Delta 与响度匹配 A/B；不出现 x / y s 进度条 [推断]。

## 给 Celadon 的具体建议

### 总体：实时控制回路（时间尺度分离）[推断]
```
输入 → Balance → Clay → Color → Gain → Clip → 前瞻限幅 → 输出
   L1 10 ms hop   L2/L3   L2 份额   L3 g    L0 峰值 + L1 释放
共享一份「实时谱 + 门控响度直方图」；慢环 L3：S(3 s) → 带遗忘直方图 → 上分位 L_prog → 慢积分 g（死区 + 冻结）
中环 L2：GR 预算占用 → g_预算；g = min(g_响度, g_预算)    置信度 c = 1 − exp(−T_gated / 6～8 s)
```

| 层 | 决策 | 检测量 / 估计器 | 时间常数 | 死区 / 迟滞 | 速率限制 | 依据 |
|---|---|---|---|---|---|---|
| L0 样本～5 ms | 真峰值前瞻限幅瞬态级、Clay 峰值守护、Clip | 过采样峰值 | 前瞻 1～5 ms，攻击 ≤ 1～5 ms | 无 | 硬保证 | Pro-L 2 瞬态级；A/85 前瞻 |
| L1 10 ms～1 s | Balance 增益、Glue 释放、限幅释放 | 27 段 hop 功率；起音事件 | Balance 攻 20～40 ms / 释 150～400 ms（高频更快）；Glue 释 50～500 ms；限幅释 100～800 ms（House：一拍 ÷ 3 ≈ 156 ms @128 BPM） | Balance ±1 dB（现状） | ≤ 1 dB / hop（现状） | Dolby 专利、DEQ；soothe |
| L2 1～10 s | 参考轮廓形状、GR 预算反馈（Guard）、Clay 守护、Glue / Heat / Clip 份额 | 3 s 窗 GR 统计；短时响度 S；低频相关度 | 形状 1～4 s；Guard 现状（3 s 窗、触发 1 s、退让 1 dB/s、回升 0.3 dB/s、安全 5 s）；份额 τ 3～10 s | 预算占用进 105 % / 退出 90 % | 份额变化 ≤ 0.1 / s | EBU S = 3 s；AVC；A/85 多级 |
| L3 10～90 s | 节目响度 L_prog → 目标增益 g；Tone 跟随 | 带遗忘直方图（S，τ_h 60～90 s）的上分位 P75～P90 | g 积分 τ ≈ 15～30 s；Tone 现状 10～25 s | 死区 ±0.5 LU；S 比 L_prog 低 > 8 LU 则冻结 | g 变化 ≤ 0.1～0.2 dB/s（向上更慢） | BS.1770 相对门；LRA；AVC；Orban 冻结 |
| L4 ≥ 60～120 s | 自动风格权重 | 长遗忘直方图 + 特征 | τ ≥ 90～120 s；1 Hz 更新 | 切换须驻留 ≥ 2 小节（≈ 3.75 s @128 BPM） | 权重变化 ≤ 1～2 % / s | 无同行先例 |
| 常量 | 曲风目标频谱形状、低切目标、Tone 斜率、交付档 LUFS / 真峰值 | — | 不随内容变 | — | — | Stabilizer / IEQ 的目标 = 预设 |

### 具体建议表

| # | 模块 / 决策 | 建议做法 | 预期效果 | 延迟 / CPU 代价 | 风险 |
|---|---|---|---|---|---|
| 1 | 节目响度估计 L_prog（取代画像的 I / LRA） | K 加权 400 ms 块 → 3 s 短时响度 S（≥ 10 Hz 刷新）→ −70 LUFS 绝对门 → 带遗忘直方图（0.25～0.5 LU 一格，τ_h ≈ 60～90 s，按样本计数）→ 取 P75～P90；比 L_prog 低 > 8 LU 的 S 不入账 [推断；依据 S32 S33 S34] | breakdown 不拉低、drop 能拉升；不需要「聆听」；块长无关 | 延迟 0；CPU ≪ 0.1 % | q、门限、τ_h 要扫参；起始几十秒估计不稳（靠 #3）；换歌 / 跳播污染（靠 #11） |
| 2 | 目标增益 g（Limit Gain 折线的输入） | 在限幅后测 L_out（同一估计器）与目标 T 比较做慢积分：死区 ±0.5 LU，冻结条件同 #1，dg/dt 幅值 ≤ 0.1～0.2 dB/s（向上更慢）；g ≤ g_预算（#4）；积分抗饱和 [推断] | 无锁定也能收敛；自动吸收 Glue / Clip / Limit 的实际 GR，取代 GlazeCalibrator 的前馈补偿 | 延迟 0；CPU 可忽略 | 限幅饱和时 L_out 对 g 不敏感（必须抗饱和）；起始收敛 30～60 s；T-038 门槛要专测 |
| 3 | 冷启动 / 置信度 c(t) | c = 1 − exp(−T_gated / τ_c)，τ_c ≈ 6～8 s，T_gated 只计通过门的音乐时长；g_applied = c·g；Balance 目标 = 先验 + 证据收缩 n/(n+n0)；死区 = max(1 dB, k·标准误) [推断] | 开机即用、无状态切换；前约 10 s 响度略低是因果系统的必然 | 无 | 跳播后重新爬升；τ_c 与一次性工具「最短听 5～10 s」对齐（S7 S12 S15） |
| 4 | GR 预算反馈（现 GlazeGuard） | 保留为中环并升级成连续的「预算占用」：3 s / 10 s 双窗；退让 ≤ 1 dB/s、回升 ≤ 0.3 dB/s（现状）；进 105 % / 出 90 % 迟滞；g = min(g_响度, g_预算)：预算硬、响度软（对应 smart:limit，S9）[推断] | 首个 drop 被多压几秒的最坏情形有界；T-045 风格预算（2 / 4.5 / 7 等）继续有效 | 延迟 0；已有 | 与 #2 互相拉扯 → 外环须慢一个量级 |
| 5 | Balance 增益环 | 保持零延迟与 10 ms hop；每段攻 20～40 ms / 释 150～400 ms、高频更快；净增益按 A 计权拉回 0（Gullfoss / Stabilizer 响度中性）；近静音 / 感知上不存在的段不 Recover [推断] | 抽吸与「骑」更少；对「已达标」内容近似不动 | 延迟 0；CPU 基本不变 | 释放放慢会拖尾；须用 T-032 / T-045 的 Balance 门槛回归 |
| 6 | Tone 跟随 / Glue·Heat·Clip 份额 | Tone 保持现状（起 0.10 / s、回 0.04 / s）；份额由「风格预算占用 × c(t)」驱动，τ 3～10 s，变化 ≤ 0.1 / s；只在事件边界或节拍内更新，稀疏事件段冻结 [推断；依据 S35] | 避免与快环相互作用；breakdown 里不乱动 | 无 | 事件检测误判 |
| 7 | 限幅器（与延迟主题交叉） | 两级：前瞻瞬态级 + 随平均动态的释放级（Pro-L 2）；释放 100～800 ms 自适应；稳态低音期间放慢（IRC I）；House：一拍 ÷ 3；声道链接：瞬态级 < 100 %、释放级 100 % | 方向与现状一致；失真 / 抽吸更少 | 前瞻 1～5 ms + 真峰值约 5 ms 量级（Pro-L 2） | 与「延迟 ≤ 10 ms」目标共同决定 |
| 8 | Clay 自动值 | 保留迟滞 + 保持（现状：相位攻 0.1 s、保持 2 s、释 2 s；宽度攻 0.2 s、释 2 s）；取消「先试后用」，改证据收缩 + 死区 | 无学习期仍稳定 | 无 | 缺了先试后用的代价量 → 需要在线代价代理（超出本主题） |
| 9 | 自动风格权重 | 同行无先例（Ozone / smart:* / Stabilizer / Dolby IEQ 全是用户选目标）：τ ≥ 60～120 s，1 Hz 更新，变化 ≤ 1～2 % / s，切换驻留 ≥ 2 小节；默认让位用户显式选择 [推断] | 不闪不漂；「自动」只是慢慢偏向 | 无 | 先例缺失，可能不如「固定风格」稳 |
| 10 | 曲风目标 / 交付档 | 当常量：曲风目标频谱形状、低切目标、Tone 斜率不随内容变；交付 LUFS / 真峰值由用户选（同行的目标都是预设或参考，S10 S26） | 减少需要估计的量，漂移面更小 | 无 | — |
| 11 | 换歌 / 跳播 / 循环 | 软遗忘：静音 ≥ 0.5 s、transport 重新开始或定位跳变 → 直方图权重 × 0.5、c 回落一半，不清零；旁通时引擎继续跑（Gullfoss）[推断] | 旧曲统计不污染新曲，又没有「重新聆听」感 | 无 | 宿主不报 transport 时只能靠输入不连续判断 |
| 12 | 界面 | 见正文第 5 节：常驻读数 +「不动的原因」标签 + 滚动轨迹 + 短时响度直方图 / 置信区 + Balance 实时曲线 + Delta + 响度匹配 A/B；不出现「聆听 / 锁定」或 x / y s | 用户看得见「在做什么、为什么不动」 | UI 线程，不占音频线程 | 读数过多反而乱；版式需 Max 拍板 |
| 13 | 验证指标（服务器上跑，本机不跑） | 在 T-038 / T-045 门槛之外新增：g(t) 总变差、dg 变号次数（来回拉锯）、±6 LU / 20 s 响度阶跃下 g 的最大变化、breakdown → drop 期间 g 变化 ≤ 0.5 dB、任意块长逐位一致 [推断] | 把「不骑 / 不漂 / 不抽」变成可回归的数 | 测试成本在服务器 | 阈值需 Max 认可 |

## 风险与未知

1. **同类没有「实时、持续、不靠用户选段」的目标响度先例。** 广播 AGC（Level Magic、Orban、TC ALC）的时间常数不公开，只能借结构；本报告所有标 [推断] 的数值都是初值。
2. **因果性的硬限制：** 听到「最响的段」之前不可能知道它有多响。第一个 drop 可能比稳定后多压几秒；渐入 + 预算反馈只能把它压到有界，不能消除。T-038「breakdown → drop」若以曲子里**第一个** drop 测，会比第二个更难过。
3. **长记忆在换歌 / 跳播 / 循环里会污染新内容；** 宿主不一定报告 transport，软遗忘的阈值（0.5 s、×0.5）未经实测。
4. **自动风格无先例。** 同行全是用户选目标；连续推断风格权重可能不如固定风格稳。
5. **上分位 q 因曲风而异**（Deep House 长 breakdown vs Tech House 平稳）；需按曲风参数化，与「多曲风逐类优化」交叉。
6. **响度匹配 A/B：** 旁通 / 生效切换若不补偿响度，用户会把「更响」当「更好」；同行都有 Match / Constant gain（Elevate、smart:limit）。
7. **评测数字随版本变：** Gullfoss 2018 年评测写 40 ms，1.11 手册写约 21 ms；本报告以手册为准。
8. **未核实清单：** (a) TC Loudness Pilot 的「校正按百分比朝目标靠」「Stridency Reduction」只见搜索摘要，未见一手手册；(b) Level Magic / Orban AGC 的时间常数与 dB/s 未公开；(c) Gullfoss 是否 FFT / 串联滤波器组只见搜索摘要，一手资料没写；(d) Ozone Maximizer 各 IRC 延迟未公布；(e) smart:comp 2 / smart:limit 的延迟数字没找到；(f) Pro-Q 4 自动动态的时间常数未公布；(g) Leapwing LimitOne 双算法只见新闻稿；(h) 任务书提到的 Leapwing「CeilingEQ」没查到这个产品，改查了 DynOne / LimitOne；(i) Perez-Gonzalez & Reiss（WASPAA 2009）「累计统计 + 迟滞响度门」只核实到摘要级；(j) Giannoulis 等（JAES 2012）「dB 域平滑 / 推荐前馈」只核实到摘要级。
9. **官方几乎不公布 CPU 百分比，** 20 % 预算只能靠自家测量。
10. **「实时」口径不同：** Gullfoss 是几十毫秒窗的近似无记忆，Stabilizer / Clarity 是时间常数可调的持续自适应；我把它们并成一类只为对照「有无学习期」。

## 出处清单

一手：产品手册 / 官方文档
- S1 [一手] Soundtheory Gullfoss Operation Manual 1.11（rev 1.3, 2021-11-16）— https://www.soundtheory.com/static/Gullfoss%20Operation%20Manual.pdf
- S2 [一手] oeksound soothe3 manual — https://oeksound.com/manuals/soothe3
- S3 [一手] oeksound soothe2 manual — https://oeksound.com/manuals/soothe2/
- S4 [一手] oeksound Soothe Live manual — https://oeksound.com/manuals/soothe-live/
- S5 [一手] oeksound spiff manual — https://oeksound.com/manuals/spiff/
- S6 [一手] oeksound Bloom manual — https://oeksound.com/manuals/bloom/
- S7 [一手] sonible smart:EQ 4 manual（EN）— https://www.sonible.com/wp-content/uploads/2024/01/manual-smEQ4_EN.pdf
- S8 [一手] sonible smart:comp 2 manual — https://www.sonible.com/wp-content/uploads/2022/07/manual-sonible_smartcomp2.pdf
- S9 [一手] sonible smart:limit manual — https://www.sonible.com/wp-content/uploads/2022/05/manual-smartlimit_download.pdf
- S10 [一手] iZotope Ozone 11 Help · Stabilizer — https://docs.izotope.com/ozone11/en/stabilizer/index.html
- S11 [一手] Ozone 11 Help · Clarity — https://docs.izotope.com/ozone11/en/clarity/index.html
- S12 [一手] Ozone 11 Help · Master Assistant — https://docs.izotope.com/ozone11/en/master-assistant/index.html
- S13 [一手] Ozone 11 Help · Maximizer — https://docs.izotope.com/ozone11/en/maximizer/index.html
- S14 [一手] Ozone 11 Help · Impact — https://docs.izotope.com/ozone11/en/impact/index.html
- S15 [一手] Ozone 12 Help · Maximizer — https://docs.izotope.com/ozone12/en/maximizer.html
- S16 [一手] Ozone 12 Help · Master Assistant — https://docs.izotope.com/ozone12/en/master-assistant.html
- S17 [一手] Ozone 9 Help · Maximizer（IRC I 的描述）— https://s3.amazonaws.com/izotopedownloads/docs/ozone9/en/maximizer/index.html
- S18 [一手] FabFilter Pro-Q 4 Help（PDF）— https://www.fabfilter.com/downloads/pdf/help/ffproq4-manual.pdf
- S19 [一手] FabFilter Pro-L 2 Help（PDF）— https://www.fabfilter.com/downloads/pdf/help/ffprol2-manual.pdf
- S20 [一手] FabFilter Pro-MB Help（PDF）— https://www.fabfilter.com/downloads/pdf/help/ffpromb-manual.pdf
- S21 [一手] TDR Kotelnikov GE manual — https://docs.tokyodawn.net/kotelnikov-ge-manual/
- S22 [一手] TDR Nova GE manual — https://docs.tokyodawn.net/nova-ge-manual/
- S23 [一手] TDR Limiter 6 GE manual — https://docs.tokyodawn.net/limiter-6-ge-manual/
- S24 [一手] Eventide / Newfangled Audio Elevate User Guide — https://www.fullcompass.com/common/files/32095-EventideElevateUserGuide.pdf
- S25 [一手] Accentize SpectralBalance 产品页 — https://accentize.com/spectralbalance/
- S26 [一手] Dolby PC Entertainment Experience v4 白皮书（Volume Leveler / Intelligent Equalizer）— https://professional.dolby.com/siteassets/pdfs/dolby-pc-entertainment-experience-v4-white-paper.pdf
- S27 [一手：手册页，第三方托管] Orban OPTIMOD-Studio 8200ST（Silence Gate、smooth leveler）— https://www.lost-manuals.com/pdf/orban--optimod-studio-8200st--service-user--ID9854.pdf
- S28 [二手：厂商新闻稿转载] Jünger Audio Level Magic（TV Technology）— https://www.tvtechnology.com/miscellaneous/jnger-audio-level-magic
- S29 [二手：厂商新闻稿转载] Jünger Audio Level Magic（Advanced Television）— https://www.advanced-television.com/?p=11664
- S30 [一手] Waves L3 Multimaximizer 手册（ARC、80 ms）— https://assets.wavescdn.com/pdf/plugins/l3-multimaximizer.pdf
- S31 [一手] Waves WLM Loudness Meter 手册（Trim）— https://assets.wavescdn.com/pdf/plugins/wlm-plus-loudness-meter.pdf

一手：标准 / 专利 / 论文
- S32 [一手] ATSC A/85:2026-07 Techniques for Establishing and Maintaining Audio Loudness for Digital Television — https://www.atsc.org/wp-content/uploads/2026/07/A85-2026-07.pdf
- S33 [一手] EBU Tech 3341 (2023) Loudness metering: EBU Mode — https://tech.ebu.ch/docs/tech/tech3341.pdf
- S34 [一手] EBU Tech 3342 (2023) Loudness Range — https://tech.ebu.ch/docs/tech/tech3342.pdf
- S35 [一手] US 8,144,881 B2（Dolby；Crockett, Seefeldt）Audio gain control using specific-loudness-based auditory event detection — https://patents.google.com/patent/US8144881B2/en
- S36 [一手] US 8,090,120 B2（Dolby；Seefeldt）Calculating and adjusting the perceived loudness and/or the perceived spectral balance of an audio signal — https://patents.google.com/patent/US8090120B2/en
- S37 [一手] Moore (2008) The choice of compression speed in hearing aids, Trends in Amplification — https://pmc.ncbi.nlm.nih.gov/articles/PMC4111434
- S38 [一手] Moore, Glasberg, Varathanathan, Schlittenlacher (2016) A loudness model for time-varying sounds incorporating binaural inhibition, Trends in Hearing — https://pmc.ncbi.nlm.nih.gov/articles/PMC5318944
- S39 [一手，仅核实到摘要级] Giannoulis, Massberg, Reiss (2012) Digital Dynamic Range Compressor Design — A Tutorial and Analysis, JAES 60(6) — https://www.academia.edu/31481096/Digital_Dynamic_Range_Compressor_Design_A_Tutorial_and_Analysis

二手：评测 / 新闻 / 搜索摘要
- S40 [二手] Sound On Sound · Soundtheory Gullfoss v1.10 — https://www.soundonsound.com/reviews/soundtheory-gullfoss-v110
- S41 [二手] Sound On Sound · Soundtheory Gullfoss（「静音时才回平」）— https://www.soundonsound.com/reviews/soundtheory-gullfoss
- S42 [二手] audioXpress 2018 · Gullfoss 新闻稿（> 100 次/s、patent-pending）— https://audioxpress.com/news/soundtheory-unveils-gullfoss-intelligent-automatic-equalizer
- S43 [二手] Mix Online · Andreas Tell 访谈（前后振铃与时间掩蔽）— https://www.mixonline.com/blog/gullfoss-inventor-andreas-tell-tells-all
- S44 [二手] Production Expert · Ozone 10 First Look（Stabilizer「持续更新响应曲线」）— https://www.production-expert.com/production-expert-1/izotope-ozone-10-mastering-suite-first-look
- S45 [二手] Sound On Sound · iZotope Ozone 12 Advanced（Analysis Time 60 s、Unlimiter 85 ms）— https://www.soundonsound.com/reviews/izotope-ozone-12-advanced
- S46 [二手] Sound On Sound · Newfangled Audio Elevate — https://www.soundonsound.com/reviews/newfangled-audio-elevate
- S47 [二手] Mix Online / Barry Rudolph · Leapwing DynOne v2 评测（crest factor、65 / 4096 样本）— https://www.mixonline.com/technology/review-leapwing-audio-dynone-version-2
- S48 [二手] Joseph Nilo · Waves Vocal Rider explained — https://josephnilo.com/blog/waves-vocal-rider-explained/
- S49 [二手] MusicRadar · sonible pure:limit（搜索摘要）— https://www.musicradar.com/news/sonible-pure-limit-ai-plugin
- S50 [二手] sonible 帮助中心 · Why does smart:comp 2 have a processing latency（仅搜索摘要，原页 403）— https://help.sonible.com/hc/en-us/articles/6841495343762-Why-does-smart-comp-2-have-a-processing-latency
- S51 [二手] rekkerd.org · Leapwing LimitOne（搜索摘要）— https://rekkerd.org/leapwing-audio-releases-limitone-hybrid-limiter-effect-plugin/
- S52 [二手] Perez-Gonzalez & Reiss (2009) Automatic gain and fader control for live mixing, WASPAA（仅摘要级）— https://ieeexplore.ieee.org/document/5346498
- S53 [二手] Sound On Sound · sonible smart:comp 2（约听 10 s，搜索摘要）— https://www.soundonsound.com/reviews/sonible-smartcomp-2
