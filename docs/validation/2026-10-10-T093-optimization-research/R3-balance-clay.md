# R3 · Balance 与 Clay：动态频谱平衡、共振、低频与声像（T-093）
**一句话结论**：Balance 的 1/3 倍频程模型在 200 Hz 以下比听觉滤波器还细、1 kHz 以上只有听觉分辨率的一半（ERB ÷ 1/3 oct 带宽从 50 Hz 的 2.6 降到 4 kHz 的 0.49），最值得做的是「高频加一层更窄的、低频合并得更粗」——在 Balance 现有 FFT 上加 bin 级共振检测 + 8 个零延迟动态窄陷波（R3-01），配三条低价修正；Clay 守护回收 60～80 % 在已限幅成品上是物理上该发生的事，正确做法是让自动低切及早退档（R3-03），不是放松守护。
**前 5 条一览**（按价值）：
1. R3-01 共振层：bin 级检测 + 8 个动态窄陷波（零延迟，Balance 内，位置交 R6）· 代价 L · P1
2. R3-02 推响不刺的起音保留只认「宽带起音」，踩镲不再反复放开削减 · S · P1
3. R3-03 守护保留比 κ 进压力反馈当第三触发量（只管自动低切）· S · P1
4. R3-04 低频冲击保留度量：40～120 Hz 包络调制深度，链前后对比 · S · P1
5. R3-05 目标频谱自动形状先对齐 Tone 的中性点，再换 27 段「中位 ± 容差」隧道 · S / M · P2

> 2026-10-10，调研代理 R3，只读；唯一写入是本文件。本机没编译、没跑测试与渲染工具。文中带「numpy」字样的数字是 scratchpad 里按源码常数写的纯算术（RBJ 公式、核 [1,2,3,4,3,2,1]/16、Butterworth 解析式），不是插件实测；复核按各条「怎么验」在 DGX / 107 上重跑。
> 已做 / 挂起项只引用不重提：R2-02 串扰补偿、R2-03 / R2-04 低段长窗与参考口径、谐波根 bin（T-032 待拍板）；收单拆出守护（Max 2026-10-05 答「不拆」，ADR-032）；自动收单（ADR-015）；T-047 / T-088 延迟三步；T-086 可解释性。

## 1. 读了什么、怎么做的

- 源码：`Source/dsp/BalanceModule.{h,cpp}` 全文（重点 :40-200 常数、:271-410 prepare、:899-970 执行、:1157-1352 跟踪、:1479-1844 判断）、`ReferenceSpectrum.{h,cpp}`、`ClayModule.h`（文件头 ④ 守护与 T-032 / T-043 / T-044 修订）、`ClayModule.cpp:40-70 / :1490-1525`、`ClayGuard.h`（压力监视器 :145-216、保留比监视器 :232-276）、`ClayTrials.h`、`LinearPhaseLowpass.h`、`GlazePlanner.cpp:1142-1370 / :1830-1870`、`CeladonChain.cpp:30-50 / :225-295`、`MusicAnalyzer.cpp:2480-2504`。
- 规格 / ADR：vault `01-框架/设计规格-{Balance,Clay,Clay-根治修订,高级母带控制,曲风与交付,实时GLAZE-曲风}.md`，`03-记忆/ADR-{009,014,032}`；T-032 的 R2 / V2 / R4 / V4；T-038 REPORT；渲染工具 README 指标表。
- 外部一手：Gullfoss 1.11 操作手册（PDF 全文读过）、soothe2 手册、FabFilter Pro-Q 4 / Pro-DS 在线帮助、Softube Weiss DEESS 手册、iZotope Ozone 产品页与 Ozone 9 帮助、sonible smart:EQ 4 产品页与帮助中心、Wavesfactory Spectre 产品页；心理声学文献（Glasberg & Moore 1990、Zwicker & Terhardt 1980、Terhardt 1979、Schroeder 等 1979、ISO 532-1 / 532-2）。Baby Audio Smooth Operator 官方页打不开，只有零售商转载的厂商文案，只作旁证。

## 2. 七个问题的结论速览

**听觉分辨率对照**（numpy；ERB = 24.7·(4.37 f/kHz + 1)【一手 Glasberg & Moore 1990】）：

| 段中心 | 1/3 oct 带宽 | ERB | ERB ÷ 带宽 | ±1 oct 抹平核覆盖的 ERB 数 |
|---|---|---|---|---|
| 40 Hz | 9.1 Hz | 28.9 Hz | 3.18 | 2.0 |
| 100 Hz | 23.0 | 35.4 | 1.54 | — |
| 200 Hz | 45.9 | 46.1 | 1.00 | — |
| 1 kHz | 231.6 | 132.6 | 0.57 | 10.4 |
| 4 kHz | 926 | 457 | 0.49 | 12.1 |
| 8 kHz | 1853 | 888 | 0.48 | 12.5 |

含义：200 Hz 以下 Balance 按 1/3 oct 逐段判断比耳朵还细（40～125 Hz 六段只占约 3 个 ERB），1 kHz 以上一个 1/3 oct 段约等于 2 个听觉滤波器，比它窄的尖峰耳朵听得出、Balance 管不到。

| 角度要求 | 结论 | 提案 |
|---|---|---|
| 1 分辨率与模型 | 加一层 bin 级共振检测，用零延迟 IIR 窄陷波执行；不做 STFT 增益掩模（线性相位 ⇒ 延迟 = 窗长）；箱音 200～500 Hz 要等低段长窗 / 抽取分析 | R3-01 |
| 2 参考轮廓 | 不上真遮蔽 / 响度模型；可取的轻量改进是「剔孤立单段离群」与低频按 ERB 合并判断，Bark 斜率替换掩蔽常数低优先 | R3-06、R3-08、R3-10 |
| 3 Bias / 目标曲线 | Gullfoss 的 Bias 是 Recover / Tame 的分类分界，不是目标曲线，不建议加旋钮；目标频谱的自动形状与 Tone 中性点不一致，先对齐再换隧道 | R3-05、第 4 节 |
| 4 低频管理 | 守护回收是物理后果，不放松守护；让自动低切在保留比低时退档；先建低频冲击度量；2 阶低切只作实验 | R3-03、R3-04、R3-09、R3-11 |
| 5 声像 | 只做 4 段 S/M 只读画像 + 通用档加宽架频率按缺口选，House 系数 0 不变 | R3-12 |
| 6 推响不刺与起音 | 检测带宽太宽（窄峰被稀释一半）交 R3-01；起音保留被踩镲反复触发，按起音宽度门控 | R3-01、R3-02 |
| 7 怎么验 | 每条给合成夹具 + 22 首 / Jamendo 45 首指标；新增「尖峰指数」「非陷波区能量损失」「低频调制深度」「Balance 净倾斜」四个度量 | 各条 |

## 3. 提案

### R3-01 · 共振层：在 Balance 现有 FFT 上做 bin 级共振检测，用 8 个零延迟动态窄陷波执行

- **问题 / 现状**：执行是 27 个 RBJ 峰值滤波器、Q ≈ 4.318（`BalanceModule.cpp:313`）；推响不刺按 1/3 oct 段功率判、按段削（`:1790-1796`）。一条 1/12 oct 宽、高出周围 10 dB 的窄峰落在一个 1/3 oct 段里，段功率只高 5.1 dB → h = 1 时只削 2.35 dB；峰在段边缘时那里只削到 1.12 dB；±1 oct 内非共振内容被带走 0.39 dB（粉噪口径）。换成 Q 17、−6 dB 的陷波：峰处 −6 dB，非共振内容只损失 0.16 dB（numpy，RBJ 公式）。耳朵在 1 kHz 以上的分辨率约 1/6 oct（第 2 节表），这类尖峰（合成器尖哨、踩镲单频刺、人声谐振）听得出、现结构管不到。300 Hz 处 1/12 oct 只有 0.74 个 bin（48 k / 2048 点），箱音段现 FFT 分不出。
- **提案**（放 Balance 内、27 个峰值滤波器之后的同一采样循环；最终位置交 R6）：
  1. 输入：每 hop 已累加好的两声道功率谱 `powerSpectrum`（`:1062`，只读、不新增 FFT），先做 bin 级一阶时间平均（τ ≈ 40 ms，约 4 个 hop）。
  2. 包络：对数频率上 ±1/6 oct（不小于 ±150 Hz）窗内的功率均值，剔掉中心 ±2 bin（Hann 主瓣），前缀和 O(K)，K ≈ 770 bin、与采样率无关（`analysedBinCount`，`:366`）。
  3. 候选：超出量 e = 10·log10(P/包络) 的局部极大且 e ≥ e_sel（出厂 6 dB）；频率 600 Hz ～ 14 kHz（±1/6 oct ≥ 3 倍主瓣要求 f ≥ 1.2 kHz，600 ～ 1200 Hz 用固定 ±150 Hz 窗、选择性降级）。
  4. 频率与宽度：对数幅度三点抛物线插值求精确频率；−3 dB 宽度给 Q = f / bw，钳 6 ～ 20；宽于 1/4 oct 的峰留给原段路径（Tame / 推响不刺），窄的进本层。
  5. 跟踪：最多 8 条轨迹，±1 bin 关联；连续存在 150 ms 才生效（与自动 Tame 的 180 ms 同量级，`:100-110`）、消失 300 ms 才退；0.5 s 内频率漂移 > 0.3 bin 的判为乐音（滑音 / 颤音）不处理；复用谐波保护（`:1354`）。
  6. 深度：d = clamp(0.5·(e − e_sel / 2), 0, 6 dB) × 强度；扣掉同频处 Balance 段滤波器已施加的削减，不叠削；起音保留沿用推响不刺的 p 与 R3-02 的宽带判据。
  7. 执行：8 个 RBJ 峰值双二阶（double、转置直接 II 型，与 `processBiquad` 同一写法），增益按 hop 线性插值、每 1/8 hop 重算系数（同 `:917`）；频率只在轨迹重锁时做 20 ms 对数滑移；增益与状态都为 0 时整段跳过（同 `activeBands` 机制）。
  8. 控制两种接法（待拍板）：**A** 升级推响不刺——窄峰走本层、宽峰走原段路径，h / p 语义不变、不加旋钮；**B** 新增「共振」强度参数（追加在参数表末尾，旧工程读入 = 关）。
- **依据**：
  - soothe2 手册：Sharpness 控制「cuts 有多深多窄」、Selectivity 越高只削越突出的共振、Attack 的实际时间随频率变、Release 加长可避免快速移动的深切带来可闻的相位失真【一手 oeksound】→ 本提案的 e_sel、Q 钳位、释放 ≥ 150 ms、频率锁定对应同一组权衡（不声称复刻其算法，手册没公开检测原理）。
  - Pro-Q 4：「Spectral EQing requires linear phase processing」，延迟随分辨率变【一手 FabFilter】→ bin 级增益掩模必带窗长延迟；48 k 下 ≤ 10 ms 只容得下 512 点窗（bin 93.75 Hz），分辨率不够，所以选 IIR 陷波执行、0 延迟，代价是同时最多 8 个。
  - Ozone 12 Stabilizer 分「Shape 自适应音色平衡」与「Cut 自动共振抑制」两种模式【一手 iZotope 产品页】；Weiss DEESS 的低段专管中频共振、滤波器可选 Notch，低延迟模式用非线性相位滤波器（96 k 下 51 样本）【一手 Softube 手册】→ 宽带平衡与窄共振分层、母带级窄陷波用最小相位，都是公开做法。
  - Gullfoss 手册：约 1000 次感知模型更新 / 秒、约 300 次均衡更新 / 秒、延迟约 21 ms（p.2、p.18）【一手】；本层的更新率仍是 Balance 的 hop（约 94 次 / 秒）与 1/8 hop 系数网格，0 延迟。
  - 曾考虑用局部谱峭度（Antoni 2006，MSSP 20(2):282-307【一手】）做检测：持续乐音与稳定共振都接近定幅正弦、峭度都近 −1，分不开，所以改用「频率稳定 + 持续 + 超出包络」三条【推断】。
- **对硬约束与 ADR**：强度 = 0（旧工程、GLAZE ≤ 0 时的自动态、手动 0）时 8 个滤波器不跑 → 输出逐位不变；帧快照不变；检测在 hop 网格、系数在从 reset 起的 1/8 hop 网格 → 块长无关；打包 / 标量要照抄 `processBiquad` 里 NEON `std::fma` 那一行的写法（`:226-250` 注释）；ADR-013 不受影响；方案 A 改变 h > 0 工程的声音（ADR-020 只保证「旧工程读入 = 关」不变）。延迟 0。CPU：按 Balance 实测 22 段 0.208 %（48k/512，设计规格-Balance v0.5 修订第 4 条）折算每段约 0.0095 %，8 个陷波约 +0.08 %；检测每 hop O(770)，估计 2 ～ 4 µs。**192k/32 是硬风险**：T-021 结构性最坏约 19 %，门槛 20 %，检测必须单独成一片（拆片 3），≥ 96 kHz 可隔 hop 检测；T-032 R10-07（过采样随采样率）若落地可腾出预算。
- **代价 / 风险**：L。风险一：把 House 循环里反复出现的主音当共振——靠频率漂移判据、谐波保护、6 dB 上限与 150 ms 持续门兜，仍要试听；风险二：陷波移动的相位伪影——频率锁定、释放 ≥ 150 ms。
- **怎么验**：
  - 合成（新增，沿用 BalanceTests 的粉噪生成器）：(a) 粉噪 −20 dBFS + 3.2 kHz 二阶谐振器（Q 30、+12 dB）→ 陷波中心误差 < 1 %、峰处衰减 ≥ 4 dB、峰 ±1/12 oct 以外 ±1 oct 以内能量变化 < 0.2 dB（现推响不刺 h = 1 约 0.4 dB）；(b) 1.2 kHz 五次谐波 + 5 Hz ±30 音分颤音的合成主音 → 不生效；(c) 8 kHz 50 ms 噪声突发按 16 分音符 → 不进本层（持续门）；(d) 现验收 4 素材（1 kHz 纯音 + 粉噪）同开 Tame 与本层 → 合计削减不超过任一层自己的上限；(e) 强度 0 逐位直通、块长 {1, 7, 32, 64, 480, 512, 4096} 逐位一致、0 分配；(f) BalancePerformanceTests 同口径的 192k/32 最坏块。
  - 真歌（DGX / 107：渲染工具 + Python 后处理）：22 首门槛 + Jamendo 45 首 × GLAZE 35 / 70：①「尖峰指数」= 每帧 > 600 Hz 的 bin 级超出量 e 的 P95，输入对输出，期望降 ≥ 2 dB；②「非陷波区能量损失」= 陷波 ±1/12 oct 之外各 1/3 oct 段的能量变化，|Δ| < 0.2 dB；③ 同 Limit Gain 下 LUFS 变化 ≤ 0.1 LU；④ gate_check 0 失败；⑤ Max 等响 A/B，需要本层的差值监听（交 R5 / T-086）。
- **对 Max 口味**：全自动、方案 A 不加旋钮最贴「大旋钮按音乐自动做」；GLAZE 35 时 h = 0.175，本层最深约 1 dB，保守起步；「不自然、有点假」的风险点在陷波追乐音，必须试听定。

### R3-02 · 推响不刺的起音保留只认「宽带起音」

- **问题 / 现状**：起音 = 本段功率 > 上一帧 1.7 倍（`:160`、`:1544`），每段各判各的；一触发就开 55 ms 保持，削减乘 (1 − p)，之后 20 ms 爬回（`:1793`、`:1804`）。124 BPM 的 16 分音符踩镲间隔 121 ms，若每下都让 4 ～ 8 kHz 段触发，保持占空 45 %，p = 0.5（默认）时推响不刺平均只剩 69 % 的力度，并以 8.3 Hz 调制；8 分音符剩 85 %（numpy，含 20 ms 爬回）。而起音保留的原意是「冲击（起音）保留」（设计规格-Clay 第 1 节子项目 2、高级母带控制第 4 节）——冲击来自底鼓、军鼓这类宽带起音，踩镲恰恰是 2 ～ 8 kHz 刺耳最常见的来源【推断】。
- **提案**：两步。① 先量（S）：Python 复刻 Balance 段功率（2048 点 Hann、hop 512、段边界与 bin 权重同 `:319-360`）与起音逻辑，在 22 首 + Jamendo 45 首上统计每个刺耳段的保持占空，以及同一 hop 里 125 Hz ～ 1.6 kHz（段 6 ～ 16）同时上升 ≥ 1.7 倍的段数 n_low，求「纯高频起音」（n_low ≤ 1）的占比。② 只有「4 ～ 8 kHz 段占空 > 30 % 且纯高频起音 > 50 %」才改：p_eff = p · clamp((n_low − 1) / 3, 0, 1)——宽带起音照旧保留，纯高频起音不放开；自动 Tame 的起音保护（`:106`）共用同一判据。
- **依据**：Pro-DS 的 Allround 模式「triggering only depends on the frequency range」，母带上配 Split Band 当高频限制器用【一手 FabFilter Pro-DS 帮助】；Weiss DEESS 比较峰值与 RMS 选释放【一手 Softube】；Ozone Low End Focus 的 Punchy / Smooth 只在动作区内区分瞬态与持续【一手 Ozone 9 帮助】——公开工具都不在高频瞬态处整体放手。零延迟下 25 ms 攻击加分析滞后本来抓不到踩镲的头，本条改的是「踩镲密时整段刺耳削减被反复放开」【推断】。
- **对硬约束与 ADR**：h = 0 逐位不变；只改 h > 0（新实例默认自动，GLAZE 35 时 h = 0.175）。延迟 0，CPU 每 hop 11 次比较。与 T-032 留给 Max 的「镲片式 150 ms 突发要不要加持续时间门槛」（高级母带控制 2026-09-29 修订第 1 条）是另一个轴，一起报。
- **代价 / 风险**：S。风险：低中频能量弱的军鼓 / 拍手被当纯高频——n_low 门槛按①的分布定。
- **怎么验**：①的统计表；合成：粉噪 + 8 kHz 段持续 +6 dB 峰 + 16 分音符纯高频突发 → 改后 2 ～ 8 kHz 段 G 的时间标准差下降、均值回到无突发时的 ≥ 95 %；底鼓 + 军鼓宽带突发照旧开保持窗（`AdvancedMasteringTests`「transient-preserve changes burst attack samples」1.002 门槛照过）；真歌：2 ～ 8 kHz 段 G 序列在 5 ～ 12 Hz 的调制能量下降，同增益下 LUFS ±0.1 LU。
- **对 Max 口味**：全自动、不加旋钮；推响时少刺耳、不动底鼓冲击。

### R3-03 · 守护保留比 κ 进压力反馈（第三触发量，只管自动低切）

- **问题 / 现状**：ADR-032「后果」节实测：Max 自己的 20 首里 5 首（woqu、emotapes、somggggg 与 mp3 版、fule）自动低切一直在做，守护在 70 ～ 87 % 的块里动作，保留比中位 0.26 ～ 0.62，「被回收的主要是 20 Hz 以下听不见的那一段」——低切要去掉的东西大半又被放了回来；Jamendo 45 首里 2 首同样（中位 0.31 ～ 0.38）。现有两个触发量（100 ms 块峰值 P99 比，ADR-028；混合损失，ADR-027；`ClayGuard.h:155-169`）都没把它们降下来。g 跟着约 16 ms 一个的局部峰值在 0 ～ 1 之间被调制，ADR-014 3a 自己写了「低切的相位残差被深度调制，有可闻风险」。
- **提案**：`ClayPressureMonitor` 加第三触发量：只在自动低切 > 0 时，读 `ClayGuardKeepMonitor` 已有的 2 s 平滑 κ（`ClayGuard.h:251-276`），κ < κ_step（起点 0.5，按 7 首数据定）连续 4 s → 降一档（1 → 0.5 → 0.25 → 0），与前两条共用 4 s 冷却与档位，`scaleClayPlan` 不变。手动低切 / 收单不受影响（照 ADR-032 只亮灯）。
- **依据**：数据【一手 ADR-032、`ClayGuard.h:243-258`】。ADR-014 3a 弃用「回收量 mean(1 − g)」是因为它「夸大伤害」——本条说的不是伤害而是「效果没了」，κ 正是 V4 / ADR-032 认定的「你拧的量剩多少」的度量；3a 关于温和低音的判断已被 ADR-027 推翻。κ 长期低时关掉低切，下游多看到的次声只是本来被放回之外的那一小部分，换来守护调制与混合损失残余一起消失【推断】。
- **对硬约束与 ADR**：只改「自动低切且 κ 长期低」的歌；GLAZE 0、手动、全关逐位不变；延迟与 CPU 0。ADR-032 写 κ「只影响界面、不进标定快照」——阈值留作 `ClayPressureMonitor` 内常量即可仍不进 Constants，ADR-032 / ADR-014 需各补一句。
- **代价 / 风险**：S。风险：κ 是不加权能量比、对 20 Hz 以下最敏感——这正是低切的目的；按中位 0.26 ～ 0.62 估计约 3 ～ 5 首会退档。
- **怎么验**：22 首 + Jamendo 45 首 × GLAZE 35 / 70：gate_check 0 失败；受影响的歌 `clay_pressure_steps` 增、`peak_guard_pct` 降、`clay_blend_loss_db` 降或空、同决策重放 LUFS 不低于改前，记录 `infra_share_lt16hz_db` 的输出值变化；其余歌 `audio_hash` 不变；Max A/B 上述 4 首。
- **对 Max 口味**：修坯「只清理、不帮倒忙」；这几首上低切本来就基本没效果。

### R3-04 · 低频冲击保留度量（40 ～ 120 Hz 包络调制深度）

- **问题 / 现状**：门槛只量真峰值、LUFS、限幅入口峰值、守护占比、反相提示（`tools/render/README.md` 指标表）；没有任何量回答「底鼓与贝斯的分离、低频冲击在链后还剩多少」。Glue（侧链 100 Hz 高通、2:1、攻击 20 ms、低 / 高分频施压）与 Limit（Auto 双段释放、低频周期快释放下限、House 一拍 ÷ 3 上限）都可能抹平它（BRIEF 第 2 节）。俱乐部版的价值核心在这里。
- **提案**（只加工具，不改 DSP）：Python 读渲染成品与原始输入：M 声道 → 4 阶 40 ～ 120 Hz 带通 → 10 ms RMS 包络（dB）→ 每小节（Jamendo 有 `docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv`；Max 的歌用渲染工具的拍点或 2 s 窗）算 MD = P95 − P10 与低频峰值因数 CF_LF，主段（`--segments-csv` 标签）取中位；输出 ΔMD、ΔCF_LF，另算 30 ～ 60 Hz 对 60 ～ 120 Hz 能量比的变化。按交付（标准 / 流媒体 / 俱乐部）× GLAZE 35 / 70 / 100 出表；若重放工具能逐级旁通 Glue / Clip / Limit 就做逐级归因，不能就只给链前后、归因交 R2 / R4。
- **依据**：Ozone Low End Focus 的 Contrast =「Adjusts the spectral contrast between low and high level signals within the action region」，Punchy =「faster response times to emphasize transient content」，动作区 20 ～ 300 Hz【一手 iZotope Ozone 9 帮助】；T-038 §2 曲风档案建议「低频峰值因数区间」（引 TBC）【一手转引，T-038 REPORT:51】。度量定义本身【推断】。
- **对硬约束与 ADR**：无（工具）。
- **代价 / 风险**：S。风险：BPM 不准时小节窗错位，用 2 s 窗兜底。
- **怎么验**：脚本自检（合成：50 Hz 方波按拍门控、门控深度 12 dB + 稳定贝斯 → MD 读回 12 ± 0.5 dB）；22 + 45 首出分布。建议判据：俱乐部档 GLAZE 70 中位 ΔMD < −1.5 dB → 开后续任务（交 R2 限幅低频 / R4 Glue 低段），否则作为「不伤冲击」的证据记进 03-记忆。
- **对 Max 口味**：直接服务俱乐部版；不改声音。

### R3-05 · 目标频谱：自动形状先对齐 Tone 的中性点，再换成 27 段「中位 ± 容差」隧道

- **问题 / 现状**：
  - 两个斜率目标中性点不一致：Tone 参考斜率三类 House 都标定成 −2.25 dB/oct（Jamendo 45 首中位 −2.21 / −2.19 / −2.24，设计规格-曲风与交付 §3.2，Max 选定），而「目标频谱 = 自动」在 House 档仍按子类 −1.5 / −0.75 / −1.25（`CeladonChain.cpp:33-41`，规格表写「起点，待 Max 试听」），通用档「均衡」−0.75（`:252`）。两者都是「段功率对 log2 f 的斜率」口径（`MusicAnalyzer.cpp:2480-2504`、`BalanceModule.cpp:1767`），差别是拟合段：Tone 用 27 段，目标用 3 ～ 25 段并去中心。若 3 ～ 25 段口径下典型 House 也在 −2 左右，自动目标会持续往亮推：Tech 档 GLAZE 100（t = 0.35）时 12.5 kHz 约 +0.5 dB、80 Hz 约 −0.5 dB；GLAZE 35 约 ±0.2 dB（推断：误差 1.45 dB/oct × 3.67 oct − 1 dB 死区，× 0.35，× t）。设计规格-实时GLAZE-曲风第 4 行还写着「全频斜率不按曲风分（T-038 已证伪 Deep 暗 / Tech 亮）」，与代码的子类斜率矛盾。
  - 形状只能是直线或一首参考曲；直线对真实母带的低频鼓包、极高频滚降每首都给同方向修正。容差是固定 1 dB 死区、每段同权、两端（< 80 Hz、> 12.5 kHz）不参与（`:1770`、`:1820-1824`）。
- **提案**：
  - **A（S）**：用 Jamendo 45 首 + Max 20 首在目标自己的口径（3 ～ 25 段、门权重去中心、主段）重算中位斜率，House 自动目标三类改成同一个值（与 T-038 结论一致），通用档的风格差保持相对偏移。
  - **B（M）**：目标改成 27 段隧道。中心 c_b = 曲库段相对电平中位数，容差 w_b = IQR / 2（下限 1 dB）；误差 e_b = 去中心后（测量 − c_b），outside = sign · max(0, |e_b| − w_b)；分段权重 r_b = clamp((1 dB / w_b)², 0.25, 1)（曲库里越一致的段拉得越用力）；修正 = clamp(0.35 · r_b · outside, ±2.5) · t。两端（40 ～ 63 Hz、16 kHz）纳入但「只压不补」（outside > 0 才削）。参考模式沿用同一表示：单曲参考的容差取它 10 s 学习帧的时间 IQR。曲库怎么采、怎么分曲风、怎么评估交 R5；隧道作为出厂常数随 GlazeConstants 发，不是预设系统。
- **依据**：T-038 §2「频谱目标：宽带倾斜 + 分段目标『中心 ± 典型范围』」（引 Ozone / Tonal Balance Control 的目标隧道）【一手转引，T-038 REPORT:50】；Ozone 12 Stabilizer「25 new genre targets」【一手 iZotope】；smart:EQ 4 的 smart:filter「automatically balances the signal based on a target Profile」、可载参考曲建自定义 Profile【一手 sonible 产品页】，学习至少 6 s 有效音频【sonible 帮助中心，搜索摘要】；Gullfoss 手册强调不把信号「forcing … into a predefined shape」（p.5）【一手】——目标要宽容差、低强度。
- **对硬约束与 ADR**：A、B 都只改「目标 = 自动 / 显式形状」且 t > 0 的输出；GLAZE 0 时自动态 t = 0 逐位不变；旧工程读入目标「关」不变；`targetReferenceDb` 存档格式不变（仍 27 段相对 dB）。ADR-026「曲风只换目标不换流程」正好吻合。ReferenceSpectrum 的 8192 点口径问题是 R2-03 / R2-04 的事（V2：根治在实时侧低段）；隧道两端「只压」也降低低段泄漏的伤害。延迟 / CPU 可忽略。
- **代价 / 风险**：A S；B M。风险：样本少（45 + 20），IQR 粗，1 dB 下限兜底。
- **怎么验**：① A 的前提：45 + 20 首在 3 ～ 25 段口径下的中位斜率，以及「现自动目标给的逐段平均修正」——GLAZE 100 下某段平均修正 |·| > 0.3 dB 即证实系统偏差；② 改后同一统计逐段平均修正 |·| < 0.1 dB，而偏离隧道的单首仍得到修正；③ AdvancedMasteringTests 目标频谱用例与自参考零修正（V2 门槛：≥ 160 Hz < 0.15 dB）照过；④ Max 试听三类 House 各 3 首。
- **对 Max 口味**：曲风只换目标；不放绝对响度；全自动。

### R3-06 · 参考轮廓：剔除孤立单段离群后再抹平（两遍）

- **问题 / 现状**：参考 = Lfast 在 dB 域按 [1,2,3,4,3,2,1]/16 抹平，本段自己也在核里（`:80`、`:1243-1262`）。高出周围 T dB 的孤立单段峰：本段偏差只剩 0.75T，邻段被算成 −0.1875T、隔一段 −0.125T。T = 20 dB（验收 4 的素材）时手动 Recover 100 把两侧邻段各提 2.75 dB、隔一段 1.5 dB（numpy 稳态）；规格验收 4 注记里评审 R1-07 实测 800 Hz +3.6、1.25 kHz +4.1 dB，并写「留给试听判断是否收窄核」。镜像问题：孤立凹口让邻段被 Tame 0.25 dB。自动 Recover 有两侧支撑门（`:1580`）不吃亏；手动 Recover 与自动 Tame 的局部峰判据（`:1608`，fastLevel − smoothedLevel）都吃。
- **提案**：`updateTrackers` 第 3 步改两遍：第 1 遍照旧；标出「孤立离群段」= |Lfast − 第 1 遍抹平值| > 2 dB 且两侧邻段都 < 1 dB（只认单段，宽于一段的鼓包仍是「歌自己的轮廓」）；第 2 遍重算抹平时把这些段的权重置 0（离群段自己的参考由其余邻段给）。共同电平 C、时间平滑、重定中心都不动。
- **依据**：numpy 稳态：单段 6 / 12 / 20 dB 峰的邻段假 Recover 从 0.12 / 1.25 / 2.75 dB 降到 0；3 段 4 dB、5 段 4 dB 鼓包与现版逐项相同；纯倾斜不变【推断】。也算过「去中心 + Huber」的稳健平滑：对 3 段 6 dB 鼓包外侧邻段反而更糟（−2.25 → −3.0 dB），所以只剔孤立单段。soothe2 的 Selectivity「higher values cut only the most prominent resonances」【一手】是同一思路：先判谁是峰，再看周围。
- **对硬约束与 ADR**：只在 R 或 T > 0 且有孤立离群段时改输出；R = T = B = 0 逐位不变；帧快照不变。孤立峰的 Tame 驱动从 0.75T 变 T（+33 %）：手动 Tame 有 8t 封顶、大峰不变，自动 Tame 的 2.5 dB 新增封顶不变。CPU 每 hop 多一遍 27 × 7。GLAZE 校准与 ADR-016 门槛要复跑。
- **代价 / 风险**：S。风险：低段泄漏（R2-04）造出的「孤立峰」会被剔掉、反而让真低音段更突出——落地顺序放在 R2-04 / R3-08 之后。
- **怎么验**：验收 4 素材（1 kHz 纯音 + 粉噪）同开 Recover 100 → 邻段 |G| < 1 dB（现 3.6 / 4.1）；「upward masking lifts …」「automatic Tame …」系列用例、粉噪收敛、扫频、电平阶跃照过；22 首 gate_check。
- **对 Max 口味**：去掉「Recover 把乐音两旁抬亮」这种不自然；全自动下主要体现在自动 Tame 对孤立峰的判断。

### R3-07 · Balance 亮度中性：先量净倾斜，再决定是否把倾斜交还 Tone

- **问题 / 现状**：Tame / Recover / 推响不刺的净修正可能带系统倾斜（2 ～ 8 kHz 削、低频补 → 偏暗），而规划器的 Tone 只看输入画像斜率（`GlazePlanner.cpp:1835-1844`），不知道 Balance 隐性做了多少倾斜；Brighten 是独立固定倾斜（`BalanceModule.cpp:85`）；掩蔽转移只做 A 计权响度锚定、不管亮度（`BalanceModule.cpp:1709-1738`）。
- **提案**：① 先量（S）：渲染工具每块记 `getBandGainsDb`，算「Balance 净倾斜」= 以 A 计权段功率加权、G 先做 ±1 oct 平滑后对 log2 f 的回归斜率（扣掉 Brighten 那部分），22 + 45 首出分布。② 只有中位 |斜率| > 0.15 dB/oct、或 > 0.3 dB/oct 的歌超过 20 % 才改：限速前把动态修正（不含 Brighten / 目标频谱）的这个回归斜率减掉，让 Balance 只管局部；明暗一律交给 Tone（它有目标斜率与往亮上限）。
- **依据**：Gullfoss 手册：Brighten 是「tells the unmasking mechanism … to prefer lower or higher frequencies」、按「perceived brightness」参数化，界面有「输出相对输入的感知亮度偏差」表；Recover 与 Tame 都既提又削、保持感知响度（p.7-10）【一手】——商用做法是把去掩蔽与明暗解耦。
- **对硬约束与 ADR**：②只改 R / T > 0 的输出；R = T = 0 逐位不变；CPU 每 hop 约 60 次乘加。不冲突 ADR。
- **代价 / 风险**：S + S。风险：「只有一段刺耳」的削减被回归摊成全频一点点提升——先 ±1 oct 平滑再回归、只去掉宽于 ±2 oct 的倾斜分量来缓解。
- **怎么验**：①的分布表；②后同一统计中位接近 0，Tone 自动值分布与改前对比（Tone 接手了原先 Balance 隐性做的倾斜）；Max A/B。
- **对 Max 口味**：明暗由一个能在界面上解释的控制负责，合 T-086 可解释性方向。

### R3-08 · 低频段决策按 ERB 粒度合并（R2-04 的低价替代或叠加）

- **问题 / 现状**：40 ～ 125 Hz 六段带宽 9 ～ 29 Hz，只有当地 ERB（29 ～ 38 Hz）的 1/3.2 ～ 1/1.3，对耳朵是约 3 个听觉滤波器、不是 6 个独立音色区（第 2 节表）；而 2048 点 Hann 主瓣 ±47 Hz 又把低音泄漏进邻段（R2-04 / V2 复算：粉噪 + 55 Hz 低音让 80 Hz 段被 Tame 压满 −8 dB、真低音段只 −1.5 dB）。R2-04 的根治（低段 4 倍长窗或抽取到 6 kHz 做 1024 点）工作量 L，还有「低段慢 4 倍或抖 2 倍」的取舍待 Max（V2 第 1 节 R2-04）。
- **提案**：段 0 ～ 5（35.6 ～ 141 Hz，约 3.1 个 ERB 数）的 Tame / Recover 偏差按组算：组功率 = 6 段功率和，组参考 = 6 段参考功率和，组偏差过死区后按原式得 G_grp，6 段同用 G_grp；执行可改成一个 100 Hz 低搁架，免得 6 个 Q 4.3 峰值叠出 1.6 倍行和的中间凸起（R2-02 的数字）。推响不刺、目标频谱不涉及这几段；帧快照照发 2048 点逐段功率（GLAZE / Clay 契约不变）。
- **依据**：组内泄漏只是在 6 段之间搬能量，组功率不变，所以泄漏不再造成「掏 80 Hz 洞」【推断】；组带宽约 105 Hz，同样 200 自由度的 Welch 时间常数从 40 Hz 段的 1.43 s 降到约 0.48 s（τ = 自由度 / (4 · 带宽)，`BalanceModule.cpp:69-75` 常量注释）【推断】；ERB【一手 Glasberg & Moore 1990】；泄漏数字【一手转引 R2 / V2 复算表】。
- **对硬约束与 ADR**：只改 R / T > 0 时段 0 ～ 5 的 G；R = T = 0 逐位不变。失去的能力：组内「次低音 vs 底鼓箱体」的相对平衡不再由 Balance 管（交目标频谱 / Tone；R2-04 落地后可拆成两组）。CPU 可忽略。
- **代价 / 风险**：S ～ M。风险：贝斯音符在组内移动不再触发逐段修正——这是本意（耳朵本来分不开）。
- **怎么验**：V2 的泄漏用例（粉噪 −20 + 55 Hz −10 dBFS、Tame 100）：80 Hz 段 G 不再单独 −8，组内 6 段 G 相同且等于组超出量按 Tame 式的值；41 / 48 Hz 同理；粉噪收敛照过；22 首门槛；与 R2-04 长窗在同一套 numpy 复刻里并排比较。
- **对 Max 口味**：低频「自然」，不追着单个贝斯音符做动态 EQ。

### R3-09 · 守护界随末级限幅的实际动作放宽（先用现有 trace 诊断）

- **问题 / 现状**：B = max(E, τ·P, F)，τ 固定 −6 dB（`ClayModule.cpp:63`、`:1496`），理由是「GLAZE 的限幅 GR 最大在 5 dB 以内」（ADR-014 决定 1）。流媒体 / 标准档轻推时限幅器 GR 可能远小于 5 dB，守护仍按「近峰 6 dB 内都要保」拦。
- **对任务里三个思路的正面评估**：
  1. 「守护只在限幅器真会动作的电平区生效」——对**已限过幅的成品**无效：那类素材 8 ms 局部最大 E 与 1 s 包络 P 几乎相等，任何 τ < 0 都不放宽；回收 60 ～ 80 % 的正是这类歌（ADR-028 背景、ADR-032 后果），它们交给 R3-03。对 Max 自己有动态的粗混可能有效，值不值要先量。
  2. 「按响度中性守护」——不成立：低切去的是 K 加权几乎不计的次声，按响度判几乎永远放行，峰值照抬、限幅器多压，伤害转到限幅之后；链后的伤害已经由混合损失（ADR-027）与 P99 峰值比（ADR-028）在量。
  3. 「低切挪到 Balance 前」——ADR-009 的理由仍成立（Balance 分析帧与画像会看到 Clay 自己的输出、闭环）；也解决不了「切掉被成品限幅器算进去的次声就冒峰」（ADR-028 背景）；唯一收益是 Balance 40 Hz 段少看到一点次声泄漏，用 R2-04 / R3-08 更直接。不建议。
- **提案**（先诊断 S，再定是否实现 M）：用渲染工具 `--trace` 的逐块列（守护样本数与限幅 GR）在 Max 20 首 + Jamendo 45 首 × 三档交付上算「守护在动作的块里、末级 GR = 0 的占比」。> 40 % 才实现：τ_eff = clamp(−(上一个 3 s 固定网格窗的 GR_p99 + 1.5 dB), −9, −2) dB；否则不做。
- **依据**：ADR-014 决定 1 的 τ 推理【一手】；本评估【推断】。
- **对硬约束与 ADR**：实现时只改守护在动作的歌；三项为 0 / GLAZE 0 默认逐位不变；引入 Limit → Clay 的慢反馈（跨模块状态，块长无关要求 GR 统计落在固定网格），链结构上能否接受交 R6。
- **代价 / 风险**：诊断 S、实现 M；反馈环（放宽 → 峰升 → GR 升 → 收紧）稳定但慢。
- **怎么验**：诊断数字本身；实现后 gate_check、限幅入口峰值增加 ≤ 0.3 dB、κ 上升。
- **对 Max 口味**：中性。

### R3-10 · 上行掩蔽的阈值斜率改按 Bark 距离

- **问题 / 现状**：被埋深度 = 间隙 − 2 − 4·d（`:118-119`、`:1677`），每隔一个 1/3 oct 一律 4 dB。1/3 oct 对应的 Bark 距离在 62 Hz 只有 0.16、1 kHz 1.52（差近 10 倍，Zwicker 公式）；Terhardt 的上行斜率 24 + 230/f − 0.2·L（dB/Bark）折成每段，80 dB SPL 时 62 Hz 1.9 dB、250 Hz 5.5、1 kHz 12.5、4 kHz 10.7 dB（numpy）。现常数在中高频把「被埋」判得比心理声学宽得多（4 对 10 ～ 12 dB/段），低频略窄。
- **提案**：把 4·d 换成 Σ（相邻段 Bark 距离 × S_u），S_u 取 80 dB SPL 的 Terhardt 值（插件不知道回放声压，按 −14 LUFS 回放约 80 dB SPL 假定【推断】）；偏移 2 dB 在 250 ～ 500 Hz 重新对齐，使那里行为不变；prepare 时算好表。
- **依据**：Zwicker & Terhardt 1980（JASA 68(5):1523）Bark 公式、Terhardt 1979（Hearing Research 1:155-182）斜率、Schroeder 等 1979（JASA 66(6):1647）扩散函数【一手】。规格已声明这条路径「不是 Gullfoss 的听觉模型」、总体非目标是「真·听觉遮蔽模型」——本条只是把一个常数换成随频率变的表。
- **对硬约束与 ADR**：只在 R、T 同时 > 0 时变；GLAZE 下自动底量 Recover 8 / Tame 24 × 强度默认就开着（`GlazeConstants.h:337-338`），但接收段还要自动缺口许可，实际触发少。R 或 T 为 0 逐位不变。
- **代价 / 风险**：S；改变 0.1.7.2 已试听过的行为，收益没有听感证据。
- **怎么验**：「upward masking lifts a buried higher neighbour …」用例改成分频段期望；扫频、粉噪照过；22 首里统计掩蔽提升触发段的分布前后对比；Max 试听。
- **对 Max 口味**：中性，低优先。

### R3-11 · 自动低切改 2 阶（手动保持 4 阶）——先在 7 首歌上对照

- **问题 / 现状**：低切是 4 阶 Butterworth（`ClayModule.cpp:45-46`），2fc 处相位 78°，fc = 20 Hz 时 50 Hz 群延迟 3.6 ms；2 阶同处 43°、2.0 ms（解析式）。根治修订 §1：零相位低切峰值只变 −0.3 ～ +0.4 dB、IIR +0.1 ～ +4.4 dB——峰值上抬的主因是相位；`docs/validation/2026-09-26-clay/render/findings.md:17`：Ansun 4 阶 17.26 Hz → +4.13 dBFS，2 阶同频 → +2.71 dBFS。
- **提案**：先实验：ClayTrials 加一个 2 阶 20 Hz 低切候选（同块网格），在守护常亮的 7 首（Max 5 + Jamendo 2）上比较两种阶数的峰值代价、κ、混合损失与 < 16 Hz 去除量；只有 2 阶把 κ 中位提到 > 0.75 且 < 16 Hz 去除 ≥ 10 dB 才把自动低切改 2 阶（手动保持 4 阶，旧工程不改声音）。
- **依据**：上述出处【一手 根治修订 §1、findings.md:17】；相位 / 群延迟【推断，解析式】。2 阶在 fc/2 只衰减 12.3 dB（4 阶 24.1 dB）——对 < 16 Hz 的真次声 / 直流够用，对 House 25 Hz 目标偏软。
- **对硬约束与 ADR**：只改自动低切；低切 0 逐位不变；候选数 +1（实时试算每样本 +2 个双二阶）。Ansun 的例子说明 2 阶只能缓解、不能替代守护（ADR-014 仍需要）。
- **代价 / 风险**：M；R3-03 落地后受影响的歌可能已退档，本条收益要在 R3-03 之后重估。
- **怎么验**：上面的对照表；若改，gate_check、ClayTests 低切用例按新阶数更新期望。
- **对 Max 口味**：中性；更少「守护拉扯」。

### R3-12 · 声像最小可行版：4 段 S/M 与相关度只读进画像；通用档加宽架频率按缺口选

- **问题 / 现状**：加宽只有 2 kHz 高架（`ClayModule.h` `widthShelfHz`），自动只在 2 ～ 16 kHz 侧中比 < −10 dB 时给（`GlazePlanner.cpp:1333-1350`），House 档系数 0（设计规格-曲风与交付表）；Balance 帧快照已有逐段交叉功率（`BalanceModule.h` `FrameSnapshot::bandCross`）。
- **提案**：① 画像加 4 段（< 150 Hz、150 Hz ～ 1 kHz、1 ～ 5 kHz、> 5 kHz）P_S / P_M 与 ρ，只读，复用 bandCross，给界面与 T-086 解释用；② 通用档自动加宽的需要度改成「相对曲库中位的侧中比缺口」（与 R3-05 同一套曲库数据），在 2 / 4 / 8 kHz 三个架频率里选缺口最大的一段，中低频不碰；把「低频段任何自动处理只许收窄」写进测试（现状已满足）。
- **依据**：T-038 §2「分段宽度：4 段 M/S 能量比，低频段只许变窄」（引 Ozone / TBC）【一手转引，T-038 REPORT:53】；Ozone Imager「Add natural stereo width to narrow mixes in Stereoize mode or enhance mono with Recover Sides」【一手 iZotope 产品页】。
- **对硬约束与 ADR**：①无声音变化；②只在通用档且加宽 > 0 时改架频率（参数不变、只改自动值），House 不变。
- **代价 / 风险**：S；低。
- **怎么验**：画像字段单测；Jamendo 45 首 4 段侧中比分布；通用档窄素材上加宽后 4 段比值趋向中位、高频相关度 ≥ 0.25（`clayMinCorrelationAfterWidth`）。
- **对 Max 口味**：「窄的歌才适度加宽、母带不拉宽」——House 不动，通用档只补真缺口。

## 4. 评估后不建议做的

- **STFT bin 级增益掩模**（soothe / Smooth Operator / Pro-Q 4 spectral 那一类）：要线性相位重叠相加，延迟 = 窗长（Pro-Q 4 帮助原话）；≤ 10 ms 目标下只容得下 512 点窗（bin 93.75 Hz），1 kHz 以下分不出窄峰。改用 R3-01 的 IIR 窄陷波。
- **真·遮蔽 / 响度模型当参考**（ISO 532-1 Zwicker、ISO 532-2 Moore-Glasberg）：总体规格的非目标；ISO 532 算的是整体（时变）响度，不直接给「哪段被埋」，作参考轮廓要大改；轻量部分已由 R3-08（ERB 粒度）与 R3-10（Bark 斜率）取走。
- **Gullfoss 式 Bias 旋钮**：手册里 Bias 是「Recover / Tame 的分类分界」（p.8），Celadon 的分类由「歌自己的平滑轮廓 ± 1 dB 死区」决定，自动份额已按证据分别给 Recover / Tame（`GlazePlanner.cpp:1855-1870`），再加 Bias 是同一件事的第二个旋钮。Gullfoss 的 Boost（等响曲线随音量变化、单位 dB、保持感知响度，p.9）在 Celadon 对应 Tone + 交付，不另做。
- **低切挪到 Balance 前、按响度中性守护**：见 R3-09 的评估。
- **Balance 低频只对 M 提升**（「低频只收不放」的激进版）：等于隐性自动收窄低频，违背 ADR-015 的精神，不做。
- **Recover 用谐波增强代替线性提升**：Wavesfactory Spectre 是「sonic enhancer + graphical parametric EQ」的并联饱和【一手 产品页】，属 Color / Heat 范畴，交 R4 评估。

## 5. 建议 Max 重新考虑

无。本轮没有找到能推翻已否决项（自动收单、ARA、两遍渲染、硬件建模、预设）或已拍板「收单不拆出守护」的新证据。

## 待用户拍板

1. **R3-01 的接法**：A 升级「推响不刺」（窄峰走陷波、宽峰走原段路径，不加旋钮，但 h > 0 的现有工程声音会变）；还是 B 新增「共振」强度参数（追加参数、旧工程读入 = 关）。新实例默认是否随 GLAZE 自动（同推响不刺）。
2. **R3-01 的模块边界**：共振层放 Balance 内（共用分析、零额外 FFT）还是单独一级——R6 给位置建议，属模块边界，由 Max 定。
3. **R3-03**：接受「自动低切在守护保留比长期低于约 50 % 时退档」吗？约 3 ～ 5 首 Max 自己的歌（woqu / emotapes / somggggg / fule 一类）上自动低切会关掉。
4. **R3-05**：「目标频谱 = 自动」的 House 子类斜率 −1.5 / −0.75 / −1.25 是否作废、改成与 Tone 同一中性点（A）；是否进一步换成曲库隧道（B）。设计规格-实时GLAZE-曲风第 4 行与代码矛盾，以哪个为准。
5. **低频段口径**：R2-04 低段长窗（L，待拍板中）、R3-08 ERB 合并判断（S），二选一还是都做。
6. **R3-06**：接受手动 Recover / Tame 在孤立峰附近的声音改变，并复跑 GLAZE 校准与 ADR-016 门槛。
7. **R3-02**：「瞬态保护」改成只保护宽带起音（踩镲不再放开削减）是否符合你对这个旋钮的理解；与 T-032 遗留的「镲片突发持续时间门槛」一起定。
8. **R3-07 / R3-09 / R3-11** 都是「先量、过线才改」——量完的数字出来后是否立项，由你看数字定。

## 范围外发现（只报不改）

1. **规格与代码矛盾**：vault `01-框架/设计规格-实时GLAZE-曲风.md` 第 4 行写「全频斜率不按曲风分（T-038 已证伪 Deep 暗 / Tech 亮）」，而 `Source/dsp/CeladonChain.cpp:33-41` 的 `houseTargetSlopeFor` 仍按 House 子类给 −1.5 / −0.75 / −1.25（设计规格-曲风与交付表标「起点，待试听」）。R3-05 已把它当提案依据，这里另记为文档不一致。
2. **规格主表过时**：`设计规格-Balance.md`「参数」表仍写 Tame 封顶 6 dB、Brighten「按 Tame / Recover 缩放」，现行代码是 8 dB 与独立 3 dB / 十倍频（`BalanceModule.cpp:85`、`:89-90`）；后文修订节有更正，但主表没改，初读会误解。
3. **注释语言**：`Source/dsp/ReferenceSpectrum.{h,cpp}` 的注释是英文（Codex 写的），与全局「注释说明一律中文」不符。
4. **ADR-032 的表述将随 R3-03 失效**：它写 κ「只影响界面，不影响声音」；若 R3-03 立项，需要同步改 ADR-032 / ADR-014 3a 的措辞（提醒，不是缺陷）。

## 证据索引

**源码（本工作树）**
- `Source/dsp/BalanceModule.cpp`：常数 :80（核）、:85（倾斜 3 dB/十倍频）、:88-90（死区 / 封顶）、:100-110（自动 Tame 时序与起音比 4.0）、:118-119（掩蔽偏移 2 dB / 斜率 4 dB）、:124（转移 0.35）、:145（谐波根 bin 6）、:160-161（推响不刺起音比 1.7 / 保持 55 ms）；:226-250 processBiquad 与 NEON fma；:313 Q ≈ 4.318；:319-360 段 → bin 权重；:366 analysedBinCount；:396 刺耳频率权重；:899-970 designPeak / updateCoefficients；:1062 accumulatePower；:1243-1262 参考抹平；:1354 谐波保护；:1521 自动 Tame 持续峰；:1544 刺耳起音；:1580 自动 Recover 两侧支撑；:1608 局部峰超出；:1677 被埋深度；:1767-1824 目标频谱；:1790-1796 推响不刺超出量；:1804 起音闪避。
- `Source/dsp/BalanceModule.h` 文件头（各版本修订、FrameSnapshot::bandCross）。
- `Source/dsp/ClayModule.cpp`：:45-46 低切 Q；:58-64 守护 W / A / 释放 / τ / F；:1496 界 B；:1518 g_req。`ClayModule.h` 文件头（④ 守护、T-032 / T-043 / T-044）。
- `Source/dsp/ClayGuard.h`：:155-216 ClayPressureMonitor；:251-276 ClayGuardKeepMonitor（0.75 / 0.85、2 s）。
- `Source/dsp/GlazePlanner.cpp`：:1270-1369 planClay；:1333-1350 自动加宽；:1835-1844 Tone；:1848-1870 Balance 自动份额。`GlazeConstants.h:337-338`（自动底量 8 / 24）。
- `Source/dsp/CeladonChain.cpp`：:33-41 houseTargetSlopeFor；:225-256 推响 / 目标强度与形状。`Source/dsp/MusicAnalyzer.cpp:2480-2504` 频谱斜率拟合。
- `tools/render/README.md` 指标表与 `--trace` 列；`docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv`。

**档案与前研究**
- vault `01-框架/设计规格-Balance.md`（验收 4 注记、v0.5 性能数字、T-032 修订）、`设计规格-高级母带控制.md` §4 / §5 / 2026-09-29 修订、`设计规格-Clay.md` §11 / §12、`设计规格-Clay-根治修订.md` §1 表与 §6 各修订、`设计规格-曲风与交付.md` §3 表与 §3.2、`设计规格-实时GLAZE-曲风.md` 第 4 行。
- vault `03-记忆/ADR-009`（Clay 放 Balance 后的理由）、`ADR-014`（决定 1、3a）、`ADR-032`（κ 定义与真歌数据）。
- `docs/validation/2026-09-29-T032-deep-research/R2-balance.md`（R2-02 / R2-03 / R2-04）、`V2-balance.md`（复算表、R2-04 两处修正）、`R4-clay.md`、`V4-clay.md`；`docs/validation/2026-10-01-T038-genre-mastering-research/REPORT.md:13-14、:50-53`；`docs/validation/2026-09-26-clay/render/findings.md:17`；`docs/validation/2026-09-28-T021-perf-acceptance/REPORT.md:40-56`（整插件 CPU）。

**外部一手**
- Soundtheory, Gullfoss 1.11 Operation Manual rev 1.3（p.2 规格、p.5 引言、p.7 Recover & Tame、p.8 Bias & Brighten、p.9 Boost、p.10 表、p.18 延迟）：https://www.soundtheory.com/static/Gullfoss%20Operation%20Manual.pdf
- oeksound, soothe2 manual：https://www.oeksound.com/manuals/soothe2
- FabFilter, Pro-Q 4 · Spectral dynamics：https://www.fabfilter.com/help/pro-q/using/spectral-dynamics
- FabFilter, Pro-DS · Basic controls / 首页：https://www.fabfilter.com/help/pro-ds/using/basiccontrols ，https://www.fabfilter.com/help/pro-ds
- Softube, Weiss DEESS manual：https://www.softube.com/manuals/weiss-deess
- iZotope, Ozone Stabilizer / Imager 产品页：https://www.izotope.com/en/products/ozone/features/stabilizer ，https://www.izotope.com/en/products/ozone/features/imager
- iZotope, Ozone 9 Help · Low End Focus：https://s3.amazonaws.com/izotopedownloads/docs/ozone9/en/low-end-focus/index.html
- sonible, smart:EQ 4 产品页与帮助中心（学习时间）：https://www.sonible.com/smarteq4/ ，https://help.sonible.com/hc/en-us/articles/11847248626972-How-can-I-change-the-learning-time-for-smart-EQ-4
- Wavesfactory, Spectre：https://www.wavesfactory.com/audio-plugins/spectre/
- Baby Audio Smooth Operator：官方页未取到，仅零售商转载的厂商文案（如 https://www.sweetwater.com/store/detail/SmoothOperator--baby-audio-smooth-operator-intelligent-spectral-balancer-plug-in ），只作旁证。
- Glasberg, B. R. & Moore, B. C. J. (1990). Derivation of auditory filter shapes from notched-noise data. Hearing Research 47:103-138.
- Zwicker, E. & Terhardt, E. (1980). Analytical expressions for critical-band rate and critical bandwidth as a function of frequency. JASA 68(5):1523-1525.
- Terhardt, E. (1979). Calculating virtual pitch. Hearing Research 1:155-182.
- Schroeder, M. R., Atal, B. S. & Hall, J. L. (1979). Optimizing digital speech coders by exploiting masking properties of the human ear. JASA 66(6):1647-1652.
- Antoni, J. (2006). The spectral kurtosis: a useful tool for characterising non-stationary signals. MSSP 20(2):282-307.
- ISO 532-1:2017（Zwicker 法）、ISO 532-2:2017（Moore-Glasberg 法）。
