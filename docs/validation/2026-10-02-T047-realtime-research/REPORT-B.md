# T-047 报告 B：实时频谱驱动的模块算法、延迟预算与 20 % CPU

日期 2026-10-02 · 调研代理 B。只调研：未改仓库、未 git add / commit、未在本机跑任何测试 / 编译 / 构建
（只用 numpy 做了几处公式算术和一个玩具仿真，脚本在会话临时目录、不入库）。
说明：Write 工具拒绝写文件（「Subagents should return findings as text」），本报告以全文返回；目标路径 docs/validation/2026-10-02-T047-realtime-research/REPORT-B.md 由主进程落盘。

标注约定：`[S#]` 外部出处（URL 见文末）；`[P#]` Celadon 头文件 / BRIEF（路径见文末）；`[推]` 我按公式或代码结构推算、**未实测**；
`[仿]` 我自写的玩具仿真（不是 Celadon 代码，只用来检查控制逻辑，不是证据）；**未核实** = 没拿到一手出处。
数字默认 48 kHz；ms 口径的设计随采样率换算，样本口径的项（插值器抽头等）单独说明。

## 结论

1. **延迟：48 kHz 总延迟可从 1384 样本（28.8 ms）降到 ≤ 9 ms 而不放弃线性相位（方案 A，409 样本 = 8.5 ms）；Clay 收单改最小相位后约 4.6 ms（方案 B）；拿掉守护的下限约 1.6 ms。**
   三个大头各有明确来源：Clay D = 574 完全由「手动收单下限 40 Hz」决定，D = 2(round(0.24·fs/fc) − 1)，下限提到 120 Hz 即 190 [P3][推]；
   Clay 守护 G = 528：「g ≤ g_req」的数学保证与窗长 W、A 的取值无关，W、A 只改守护动作的频度与平滑度，可缩到 144 [P2]；Limit 的保持窗 62 样本来自 64 抽头/相的真峰值插值器 [P4][P5]。
   线性相位的群延迟是滤波器自身属性，分区卷积（Gardner）只消得掉块延迟、消不掉它 [S25][S23]。
2. **Clay 最小相位收单可行，但不是白给**：只对 S 路做 2 阶高通（不是 1 − LP 互补，后者在 fc 处凸起 +1.8 dB；也不要给 M 路加全通），相位偏移在可闻阈值以下 [S20][S21][推]；
   但纯相位处理本身就能让峰值变化 0 ～ 3.5 dB [S19]，与项目自测 ADR-012 的 1 ～ 4 dB 一致 [P2]。所以它必须先过 Clay 峰值代价格（T-032 / T-043 / T-044 同口径）才能当默认，建议做成 prepare 时选定的两档（线性相位 / 低延迟）。
3. **响度控制 = 慢回路（响段分位数参考 + 门控更新 + 速率限制）+ 快保护（按实测 GR 收增益），不骑音乐。** 参考电平取 3 s 短期响度（EBU 的 S [S1]）直方图的 P80，时间轴只走「门开」帧、门深 6 LU、门闭时冻结并慢泄漏——
   与 Omnia / Orban 的 AGC 门控冻结同思路 [S4]，与 Dolby 的「长时 + 短时双模块、0.1 dB 迟滞、±4 dB 空带、速率限制」同结构 [S5]。玩具仿真：朴素慢 AGC 在 30 s breakdown 里把增益抬 +7.7 dB、落差损失 4.4 ～ 4.9 LU；分位数 + 门控后增益漂移 0 dB、落差损失 0.8 ～ 1.3 LU [仿]。
4. **落差损失 ≈ drop 与 breakdown 的平均 GR 之差**（增益冻结时增益项抵消）[推][仿]。T-038 的 1.2 LU 门槛因此等价于「drop 里 Glue + Clip + Limit 的响度加权平均 GR 只比 breakdown 多约 1.2 dB」，
   而 T-045 的均值预算在 2 dB 量级——两个口径不同，需主进程确认（开放问题 3）。建议加「落差守门」：输入 / 输出各一份短期响度直方图，P90 − P25 的差缩小超过门槛（1.2 LU；俱乐部 1.5）就收 G_budget。
5. **Tone = 24 段加权最小二乘斜率 → 迟滞死区 → 速率限制**：在 Tone 输入端测（前馈、无闭环），时间轴只走门开帧，τ ≈ 6 s，死区 ±0.2 dB/oct（迟滞 0.1），常态速率 0.03 dB/s，大偏差持续 10 s 才提速。
6. **Glue / Heat / Clip 用「实时特征 → 目标 + 慢 servo」取代锁定后的方案**：Glue 阈值相对响段侧链 P90、servo 到目标平均 GR；Heat 用「比目标暗多少」与残差功率比预算；Clip 用分位数 servo 把「被削样本占比」钉在目标上。全部 ≤ 0.1 dB/s 级速率、带迟滞。
7. **过采样与真峰值**：Clip / Heat 改「只对非线性残差做 ADAA + 2× 过采样」（线性区残差恒为 0；整段 ADAA 在 1× 下 20 kHz 处 −11.7 dB、2× 仍 −2.0 dB，所以不能整段用 [S13][S14][推]），2× 多相 IIR 或短最小相位 FIR，延迟 60 / 64 → 约 6 [推]；
   真峰值检测倍率随采样率降（BS.1770：96 kHz 用 2× 够 [S3]）；保持窗 63 → 16 左右需实测。
8. **CPU 不是平均值问题，是最慢块问题**：新的实时控制层合计 < 10 µs / 100 ms [推]，比旧学习期（56 µs / 512 块 ≈ 0.52 % [P1]）还便宜；真正要守的是 48 kHz / 32 的最慢块（20 % ≈ 133 µs）和高采样率。
   规则：每个 hop 最多一片、每片 ≤ 15 µs、片按绝对样本位置调度、任何归约按绝对对齐的固定块累加，保证与宿主块长无关。

## 1. 总延迟预算（48 kHz）

| 模块 | 现在 | 方案 A（≤ 10 ms，保线性相位） | 方案 B（≈ 4.6 ms，建议目标） | 下限（≈ 1.6 ms） | 怎么来 |
|---|---|---|---|---|---|
| Clay 收单 D | 574（11.96 ms） | 190（3.96 ms） | 0 | 0 | A：分频下限 40 → 120 Hz [P3]；B：S 路最小相位高通 |
| Clay 守护 G = W + A | 528（8 + 3 ms） | 144（2 + 1 ms） | 144 | 0 | 缩 W、A；保证不变 [P2]；下限 = 去掉守护 |
| Heat 过采样 | 64 | ≈ 6 | ≈ 6 | ≈ 6 | ADAA + 2× IIR 或短最小相位 FIR [推] |
| Clip 过采样 | 60 | ≈ 6 | ≈ 6 | ≈ 6 | 同上 [推] |
| Limit 前瞻 | 96（2 ms） | 48（1 ms） | 48 | 48 | 默认前瞻 2 → 1 ms；留 2 ms 档 |
| Limit 保持（hold − 1） | 62 | 15 | 15 | 15 | 插值器 64 → 16 抽头/相 [推] |
| **合计** | **1384（28.8 ms）** | **409（8.5 ms）** | **219（4.6 ms）** | **75（1.6 ms）** | 前瞻留 2 ms 则各列 +48；W 取 3 ms 则 A / B 列 +48 |

要点：① Balance 零延迟、本身已是实时，本报告不改它（只建议高采样率下分析抽取，见第 8 节）；② 方案 A 不改听感结构（仍是线性相位、仍有守护），只改两个常数，风险最小，是第一步；
③ 方案 B 额外换掉收单滤波器，要过 Clay 峰值代价格；④ 对照：搜索摘要称 Gullfoss 延迟 22 ms（未在原文核实），FabFilter 线性相位 EQ 最低档 3072 样本 @ 44.1 kHz ≈ 70 ms [S23]；
⑤ 所有 ms 口径项随采样率线性换算；高采样率下过采样倍率应下调（第 7 节），否则「样本口径」的延迟会按 fs 放大。

## 2. 实时响度与增益控制

**怎么做**

- 测量沿用 LoudnessMeter 的 100 ms K 加权子块环 [P10]：M = 最近 4 个子块（400 ms，矩形窗），S = 最近 30 个（3 s），都不门控，更新 ≥ 10 Hz [S1]。
  EBU 明确写了「实时表的相对门限要在每次更新时用存下的块响度重算」[S1 第 2.3 节注]，所以用固定档数直方图（沿用现有 0.1 LU 档直方图，LoudnessMeter 一次扫描约 820 次格访问 [P10]）：增量累计、一次扫描约 1 ～ 2 µs [推]，不会有重算尖峰。
- 参考电平 L_ref = 直方图的 P_q（q 默认 0.80，曲风可调）。直方图只收「门开」帧；遗忘按「被收的帧」计（τ_f ≈ 45 s 的响段时间），breakdown 不会让历史老化。
  门：S ≥ L_ref − 6 LU 为开，迟滞 1.5 LU；门闭连续超过 T_hold（45 s）后，L_ref 以 ≤ 0.05 dB/s 向当前 S 泄漏（防「新曲更安静」时永远死锁；Omnia 的 RTZ 速度是同类设计 [S4]）。
  依据：LRA 同样用「绝对 −70 LUFS + 相对 −20 LU」级联门、再取 P10 / P95 分位数，目的就是「安静段不决定统计」[S2]；Mansbridge 等的自动推子用「迟滞响度门 + 选择性平滑」避免调整音乐的有意动态 [S6]。
- 设定点：C_set = 交付档目标 + c_曲风，c_曲风 = 参考集里 P80(S) − I 的中位数（用 22 首 Jamendo 标定）。这样「I ≈ 目标」在多数歌里近似成立，又不必靠整首积分骑增益。GLAZE 旋钮 a ∈ [0, 1] 缩放 G_tgt 与所有预算。
- 目标增益 G_tgt = a·(C_set − L_ref)，限幅到 [−6, +18] dB。

```
// 每 100 ms tick（按绝对样本计数触发；S、M 来自子块环）
open  = (S >= Lref - gateDepth + (wasOpen ? 0 : hyst))             // gateDepth = 6 LU, hyst = 1.5 LU
if (open && S > -70) { hist.add(bin(S)); W = W*lam + 1 }           // 时间轴只走门开帧
closedT = open ? 0 : closedT + 0.1
Lref  = hist.percentile(q)                                          // q = 0.80
if (closedT > Thold) Lref += clamp(S - Lref, -leak*dt, +leak*dt)   // Thold = 45 s, leak = 0.05 dB/s
Gtgt  = clamp(a*(Cset - Lref), -6, +18)
// 慢回路：迟滞死区 + 不对称速率限制
e = deadzone(Gtgt - Gslow, on 0.3, off 0.1)     // |e| < 0.3 不动；一旦动起来，到 |e| < 0.1 才停
if (e > 0) { if (open) Gslow += min(e, up * dt) }   // up = 0.2 dB/s；证据 W < W0(约 15 s 门开音频) 时减半
else       Gslow += max(e, -dn * dt)                // dn = 1 dB/s
// 快保护：独立积分器，取 min 选择器 = 天然抗积分饱和
GRm  = 0.5 s 均值(Limit GR + Clip GR 当量 + Glue GR);  over = max(GRm - budgetFast, k*(loss - lossBudget))
Gbud += (over > 0) ? -clamp(0.8*over, 0.3, 6)*dt : ((slack && open) ? +0.15*dt : 0)
Gbud  = min(Gbud, Gslow);   Gcmd = Gbud                       // 100 ms 一个目标，逐样本线性插值
```

- 落差守门：输入侧 S_in 与输出侧 S_out 各一份直方图（OUT 表已有 [P10]），loss = (P90 − P25)_in − (P90 − P25)_out；loss > 1.2 LU（俱乐部 1.5）→ `over` 里加一项，G_budget 以 ≤ 0.1 dB/s 回收。
  这是 T-038「落差处理后最多少 1.2 LU」的实时版：直接闭环在被考核的量上，而不是靠预算间接保证。
- 新曲 / 大跳变：|S − L_ref| > 10 LU 持续 20 s，或音频内容判定为新程序 → W ← 0.2·W、T_hold 缩短 → 参考加速收敛。**不要用宿主走带信息触发**：块起点的 playhead 是宿主相关量，会破坏「任意切块逐位相同」。

**出处**：S / M 窗长与门限 [S1][S3]；LRA 级联门与分位数 [S2]；AGC 门控冻结 / RTZ [S4]；Dolby 双模块 + 迟滞 + 空带（专利示例值：50 次/秒更新、0.1 dB 迟滞、±4 dB 空带，数值是示例不是规范）[S5]；
自动推子的迟滞响度门 [S6]；压缩器阈值取相对 RMS 的固定偏移（−12 dB）与按 LRA 控制压缩量的学术先例 [S7]；前馈 + 对数域检测器更稳定、无攻击滞后 [S8]（据摘要）。

**对 Celadon 的意义**：取代 MusicAnalyzer 的整段画像 + GlazePlanner 的响度目标 + 校准器；「先试后用」的响度部分、校准轮、画像失配提示全部消失；GlazeGuard 的 1 dB/s 退让改成按超出量比例（0.3 ～ 6 dB/s）。

**玩具仿真**（合成短期响度轨迹，128 BPM：intro 16 小节 −22、build、drop −12、breakdown −20、build、drop −11.5、outro；GR = max(0, S + G − (−1 − PLR))，PLR drop 9 / 其余 10.5 ～ 12 dB；设定点 −9 LUFS；
**纯逻辑检查，不是实测，不含 Glue / Clip 的真实行为**）[仿]：

| 方案 | 30 s breakdown 内 G 变化 | 落差损失 drop1 / drop2（LU） | 60 s breakdown：G 变化 / 落差损失 | 第一个 drop 的前 20 s 最大 GR（30 s 场景；括号为 60 s 场景） |
|---|---|---|---|---|
| A 朴素慢 AGC（3 s 平滑、±0.5 dB/s） | +7.74 dB | 4.44 / 4.94 | +8.00 / 4.77、5.23 | 4.34 dB（4.79） |
| B 分位数 P80（无门控） | +0.26 dB | 0.90 / 1.41 | +1.19 / 1.09、1.56 | 5.70 dB（6.47） |
| C 分位数 + 门控 + 冻结 + 泄漏 | 0.00 dB | 0.80 / 1.31 | 0.00 / 0.77、1.24 | 5.70 dB（6.47） |
| D = C + 快保护（0.5 s 均值，≤ 6 dB/s） | 0.00 dB | 0.80 / 1.31 | 0.00 / 0.78、1.24 | 3.99 dB（4.43） |

读法：门控 + 冻结把「骑增益」整项消掉，剩下的 0.8 ～ 1.3 LU 全是 drop 里 GR 比 breakdown 多出来的（结论 4）；第一个 drop 的过载（build 期间增益被抬高 2 ～ 3 dB）是因果系统的固有弱点，快保护能把峰值 GR 压到约 4 dB，但无法归零。

## 3. 实时 Tone / 频谱倾斜

**怎么做**

- 输入：Balance 每 10 ms 的 27 段原始功率快照 P_b（含 bandCross）[P7] 与 Balance 当前修正量 G_b。Tone 之前的谱 ≈ P_b·10^(G_b/10)（Clay 只改 < 300 Hz 的 S，可忽略），所以 **零新增 FFT**，且是前馈（测量点在 Tone 之前），不存在 Tone 与测量互相追逐的闭环。
  Balance 的 targetSlope / targetReference 生效时，Tone 看到的已是修正后的谱，回路自然趋向 0，两者不会打架。Balance 本身（27 段、10 ms hop、零延迟）已是实时的，保留。
- 平滑：对 63 Hz ～ 12.5 kHz 的 24 段，在**线性功率域**做时间常数 τ_tilt ≈ 6 s 的指数平均（长时平均谱 LTAS 的标准做法），帧权重只给门开帧（同第 2 节的门），breakdown 的稀疏谱不进估计。
- 斜率：x_b = log2(f_b / 1 kHz)，y_b = 10·log10(P̄_b)；加权最小二乘斜率 s = Σw(x − x̄)(y − ȳ) / Σw(x − x̄)²；IRLS 2 次、Huber（δ = 3 dB）压制孤立低频 / 高频段（与 MIR 的「谱斜率 = 线性回归」同定义 [S32]，只是横轴改成对数频率）。
- 控制律：e = s − s_ref（曲风参考：House 三类 −2.25 dB/oct [P1]）；迟滞死区 on 0.20 / off 0.10 dB/oct；
  Tone 的 ±1 dB 搁架对（100 Hz / 8 kHz，约 6.3 个倍频程）≈ 0.32 dB/oct 每 dB，故 tone_tgt = clamp(−κ·e_d / 0.32, −T_暗, +T_亮)，κ = 0.5（只纠一半偏差，稳）；e < 0（比参考更暗）→ tone_tgt > 0（提亮）；House 的 T_亮 = 0.4 dB [P1]。
  速率限制 0.03 dB/s（1 dB 要 33 s）；|e| > 1 dB/oct 持续 10 s 才提到 0.15 dB/s；变化量 < 0.1 dB 不更新（迟滞，同 Dolby 的 0.1 dB [S5]）。

**出处**：LTAS / 目标曲线思路：长期谱收敛到行业的目标 EQ 曲线且分曲风 [S30]（只据摘要，摘要里没有 dB/oct 数字；搜索摘要里出现过「约 5 dB/oct」的说法，**未核实**）；谱斜率回归定义 [S32]（据摘要）；Gullfoss 每秒更新频响 100 次以上，说明 10 ms 级更新是同类产品的常态 [S31]。

**对 Celadon 的意义**：取代 updateLiveTone 围绕「方案值」微调的结构——没有方案值了，直接围绕曲风参考；开机 tone = 0 即「中性」，之后以 ≤ 0.03 dB/s 漂移，没有可见的学习期。

## 4. Glue / Heat / Clip / Limit 的量：特征 → 目标 → 慢 servo

每 100 ms 的特征（全部来自已有测点）：PLR_S = 3 s 内真峰值最大值 − S（PLR 的定义：节目响度与最大真峰值之差 [S29]）；起音率（取 Balance 的起音检测，EWMA 8 s）；LF 占比（≤ 120 Hz 段功率 / 全段）；LF 相关度（ClayGuard 已有 [P12]）；
Glue / Limit / Clip 的 BlockStats（grMean、grMax、clipActiveFraction、clipDepthMaxDb [P5][P6]）。

| 量 | 目标（连续映射，起点值待标定） | servo / 预算 | 备注 |
|---|---|---|---|
| Glue 深度 | depth = sat((PLR_S − 8) / (14 − 8)) × depthMax(曲风)；GR_tgt = 2 dB × depth（起点） | 阈值 = 响段侧链 P90（门开帧、τ 10 s）− off；off 以 ≤ 0.1 dB/s 推到「平均 GR = GR_tgt」 | 阈值相对响段而非绝对：量纲无关（Maddams 的 RMS − 12 dB 思路 [S7]）；落差守门触发时 depth ×0.9 / 10 s |
| Heat 深度 | heat = heatMax(曲风) × (0.35 + 0.65·dark) × (1 − 0.6·dense)；dark = sat((s_ref − s)/0.5)，dense = sat((10 − PLR_S)/3) | 残差功率比 R_h = P(残差) / P(干) ≤ −26 dB（起点）：测到超就降；HF 保护沿用 heatGuard [P6] | 越暗越多、越密越少；起点值是方向性建议 |
| Clip 阈值 | T_clip ← T_clip + η·(f_测 − f_目标)（分位数追踪：随机逼近，单位 dB）；f_目标 2 ～ 4 %（曲风）；η ≈ 0.05 dB/s 每 1 % 偏差 | 夹在 [Ceiling − 3, Ceiling − 0.5] dB；削波深度 ≤ 1.5 dB；LF 占比高则深度上限 ×(1 − 0.5·lf) | f_测 = clipActiveFraction [P5]；P² 是另一种流式分位数 [S27]，但直方图 + 遗忘更易逐位复现 |
| Limit | 前瞻 1 ms、保持窗、真峰值倍率见第 6 节；释放沿用双包络 + 一拍 ÷ 3 封顶 [P5] | 预算（均值 / P95 / 峰值 GR）进第 2 节快保护 | 起音率高 → 快释放下限随之变短（想法，未验证） |

**出处 / 意义**：特征驱动的压缩器参数先例 [S7]（用响度与 LRA 控制压缩量）；程序相关释放是限幅器通行做法（iZotope IRC 的快 / 慢分段 [S11]）。
意义：「画像 → 方案 → 预测 GR → 逐项核对预算」整条离线链被「实时特征 + 实测 GR 的闭环」取代，不再需要前向链模型；GlazeGuard 的预算常数（T-045）原样复用为 servo 的上限。

## 5. Clay：低延迟替代与守护前瞻

**现状的数字来源** [P2][P3]：D = 2(L_max − 1)，L_max = round(0.24·48000 / 40) = 288 → 574；G = round(0.008·fs) + round(0.003·fs) = 384 + 144 = 528。D 随分频下限变化（48 kHz，算术核对过）：

| fc_min | 40 | 60 | 80 | 100 | 120 | 150 | 200 | 300 |
|---|---|---|---|---|---|---|---|---|
| D 样本（ms） | 574（11.96） | 382（7.96） | 286（5.96） | 228（4.75） | 190（3.96） | 152（3.17） | 114（2.38） | 74（1.54） |

规律 D(ms) ≈ 0.48 / fc(kHz)：低通越低越长是时间 - 带宽的必然，不是实现问题（线性相位 FIR 的群延迟 = (N − 1)/2，这是对称冲激响应的固有性质；换窗 / 换分区方式都改不了）[P3]。

**守护 G 能不能缩**（读 [P2] 的数学）：g = 「g_req 在 [n, n + A] 的最小值 → 线性释放 → 长 A + 1 滑动平均」，数学上 g(n) ≤ g_req(n)，所以 **「不抬过界 B」的保证与 W、A 取值无关**；W、A 只决定
(i) E = ±W 窗内 |x| 最大值对「低切相位前移的峰」覆盖多少（越小越保守、守护越常动作）；(ii) g 的台阶多陡。
W 的依据是低切的相位前移时间 t(f) = φ(f) / (360·f)（我算的）：4 阶 25 Hz 在 40 Hz 处 6.97 ms、60 Hz 处 2.97 ms；2 阶 25 Hz 在 40 Hz 处 3.85 ms、60 Hz 处 1.64 ms [推]。
所以 W = 8 ms 是「4 阶 + 25 Hz」的结果；降阶或把角频率压到 ≤ 20 Hz，W ≈ 2 ～ 3 ms 就覆盖 50 Hz 以上（2 阶 20 Hz：40 Hz 处 3.0 ms、50 Hz 处 1.9 ms [推]）。代价是同一个旋钮的另一端：2 阶 20 Hz 在 10 Hz 处只有 −12 dB，现在 4 阶 25 Hz 是 −32 dB [推]，需核对 House 成品里次声是否仍被压住。
A：1 ms 起步——限幅器同类设计里「攻击 = 前瞻」[S9]，< 0.1 ms 近似硬削波 [S10]；g 调制的是 < 300 Hz 的 S 增量，1 ms 平滑足够 [推]。**复测门槛**：T-032 / T-043 / T-044 的压力格与 g < 1 占比（现 70 ～ 83 % [P2]）。

**收单替代方案**

| 方案 | 延迟 | 相位 / 预振铃 | 低频单声道化听感 | 峰值风险 | 判断 |
|---|---|---|---|---|---|
| 现状：线性相位（延迟版 S 减 LP）+ 守护 | D 574 + G 528 | 严格线性；补偿高通的「前响」= −LP（B 样条、紧支撑 ≤ D）且 < 300 Hz，被低频能量掩蔽 [推] | M 不动；与 Lipshitz–Vanderkooy「线性相位交叉 = 延迟输入减低通」同构 [S22] | 低 | 基线 |
| A：同上，fc_min 提到 120 Hz + 缩 G | 190 + 144 | 同上 | 同上；手动下限不再到 40 Hz | 低（守护更常动作，需复测） | **第一步** |
| B1：S 路 2 阶 Butterworth 高通（直接做，不用 1 − LP） | 0 + G | 120 Hz 处 +90°、群延迟 1.88 ms；M 不动 [推] | 与黑胶「椭圆均衡」同类（模拟二阶最小相位）[S33]；Linkwitz 自己更正过「低频群延迟可闻」的旧判断 [S21] | 中：纯相位处理可使峰值 0 ～ 3.5 dB 变化 [S19]；ADR-012 实测 +1 ～ 4 dB [P2] | **须过 Clay 压力格** |
| B2：LR4 分频 + M 也过同一全通 | 0 | LR4 的 HP / LP 输出在所有频率同相（相差 360°）[S21]，二者之和是幅度平坦的全通（标准结论，未取原文）；S/M 相对相位保持，但整体等于 phase rotator（4 极点 200 Hz，语音峰值 −3 ～ 4 dB）[S19]，kick / bass 与高频的对齐被改 | 变 | 高 | 不建议 |
| C：混合相位 / 分区卷积 | 分区卷积只消块延迟 [S25]；群延迟照旧 | FabFilter Natural Phase：近似模拟相位、无明显前振铃（实现未公开，**未核实**）[S23] | — | — | 对 D 没有增益，不做 |

数值（2 阶 / 4 阶 Butterworth，我算的 [推]）：S − LP2(S) 在 fc 附近 +1.8 dB 凸起、60 Hz 只衰 2.8 dB（60 Hz 处相位 +66°）；S − LP4(S) 凸起 +4.7 dB——**互补式最小相位不能用**；
直接 2 阶高通 120 Hz：60 Hz −12.3 dB、240 Hz −0.26 dB，群延迟 2.2 / 1.9 / 0.55 ms（60 / 120 / 240 Hz）。
听感阈值：脉冲类测试信号的群延迟最小可闻阈值约 0.6 ms（500 Hz ～ 4 kHz 范围内最敏感）；较早研究在 250 Hz 处给出约 4.7 ms [S20]；更低频率的可信数据很少（搜索摘要，未核实）。S 路 < 300 Hz、非脉冲，且 M 不动，所以相位偏移本身不是问题，**峰值才是**。

**建议**：先做 A（改两个常数 + 手动下限提到 100 ～ 120 Hz，或「低延迟 / 低频精修」两档，档位在 prepare 时选定、延迟固定）；B1 作为可选低延迟档，验收 = Clay 压力格不退步（P99 峰值比、g < 1 占比、混合损失 getLastProcessBlend）。

**Clay 三项自动值的实时规则**（取代锁定后的三项）：
- 低切：次声占比 r_sub = P(< 20 Hz) / P(20 ～ 120 Hz)（新增两个双二阶，约 0.01 % CPU [推]）；r_sub > −20 dB 持续 3 s 才开（−26 dB 关），角频率取曲风目标（25 Hz），平滑沿用 20 ms 对数域 [P2]。
- 收单：对 ≤ 200 Hz 的段，S/M = P_S / P_M = (P − X) / (P + X)（Balance 的 bandCross [P7]）；a = 各段 smoothstep(S/M_dB, −24, −12) 的最大值，τ 2 s；f_m 取 S/M > −18 dB 的最高段，量化到 {80, 100, 120, 150} Hz，换档最短间隔 1 s（沿用现有两套低通交叉淡化 [P2]）。
- 加宽：House 三类不自动加宽 [P1]；通用沿用宽度保护 [P12]。

**意义**：Clay 占现延迟的 80 %（1102 / 1384），方案 A 一步拿回约 16 ms（1102 → 334 样本）。

## 6. 限幅器前瞻与真峰值

**怎么做**

- 前瞻结构不变（滑动最小 + 平滑；Signalsmith 的公式化版本：峰值保持 = 滑动最小、有限平滑短于保持窗则数学上不过冲、攻击 = 前瞻 [S9]；项目的 van Herk 滑动最小约 2.8 ns/样本 [P5]）。
  前瞻默认 2 → 1 ms：FabFilter 的说法是 < 0.1 ms 近似硬削波并带来失真与更多样本间峰值，短前瞻保瞬态、更响但有失真风险，长前瞻更稳但更不响 [S10]；
  攻击失真被峰值起点掩蔽的依据：后向掩蔽（pre-masking）约 20 ms 量级（二手来源）[S28]。保留 2 ms 档给用户。
- 保持窗 hold = 64 − 1 = 63（覆盖插值器全部抽头 [P4][P5]）→ 核心延迟 = 前瞻 + hold − 1。缩短选项（误差列是 BS.1770 最坏情形公式 under-read = 20·log10 cos(π·fnorm / n)，我用公式核对过 BS.1770 的表 [S3]）：

| 检测器 | 理论最坏低估（fnorm 0.45 / 0.5） | hold | 备注 |
|---|---|---|---|
| 4×、12 抽头/相（BS.1770 示例，48 阶总长） | 0.554 / 0.688 dB | 11 | 项目实测 4×/24 抽头对 Clip 削出的高频尖峰低估 0.4 ～ 1.3 dB [P4]，需更大余量 |
| 8×、32 抽头/相 | 0.136 / 0.169 dB（再加滤波器纹波，未测） | 31 | 中间档 |
| 8×、64 抽头/相（现状） | ≈ 0.1 dB [P4] | 63 | 现状 |
| 96 kHz 下 4× / 192 kHz 下 2×（20 kHz 内容） | 0.12 / 0.12 dB [推] | 按抽头 | BS.1770：高采样率「按比例减少过采样」，96 kHz 用 2× 够 [S3] |

- 另一条路：4× 网格上对最大点及左右各一点做三点抛物线细化（Essentia 描述了重采样与抛物线两种策略 [S26]），能把 4× 的低估压回；精度**未核实**，需实测。
- 保持窗不必等于插值器全长：sinc 核在 ±16 样本外的贡献已很小，gain 平缓时可缩到 ≈ 16，由硬钳位兜底 [推，需实测]。
- 与 ADR-004「表与限幅器同源」冲突：限幅器改短检测器后，它的读数会比表（仍用 8×/64）低 0.2 ～ 0.5 dB；对策 = 余量 0.35 → 0.5 dB，或接受不同源（开放问题 5）。

**出处 / 意义**：S3、S9、S10 如上；iZotope 在商用最大化器里同样提供「最低延迟」档（IRC LL），而 IRC III 在 > 48 kHz 无法实时使用、IRC IV 延迟随采样率升高 [S11]——说明「低延迟 vs 最优质」二选一是行业常态，Celadon 的两档思路并不特殊。
意义：这一节把 Limit 的 218 降到约 69（Clip 6 + 前瞻 48 + hold 15）。

## 7. 过采样器（Clip 4×、Heat 8×）

**怎么做**

- 现状：线性相位等纹波半带 FIR，Clip 两级 4× 延迟 60、Heat 三级 8× 延迟 64，FIR 有限记忆 → 「空闲跳过 + 预滚」逐位等价 [P8]。
- 选项：
  a. **短最小相位 FIR 半带**（由线性相位原型经倒谱法转最小相位 [S24]）：保持有限记忆（现有逐位契约不变），延迟约 4 ～ 8 样本；失去半带零系数与对称折叠，MAC 约 ×2 [推]——保守档。
  b. **多相全通 IIR 半带**：JUCE 的结论是 IIR「Nyquist 附近相位受损、延迟最小」[S12]；椭圆设计系数少但相位非线性，quasi-linear 设计要更多系数 [S17]；HIIR 有 Neon / SSE 实现 [S18]。
     上 + 下一对的延迟按 ≤ 8 个 1× 样本估 [推，未核对具体设计]。**无限记忆**，空闲跳过不再逐位等价，需常开（IIR 约 50 flops/样本 [推]，CPU 无所谓）。
  c. **ADAA**：1 阶 ADAA 是对非线性做连续时间卷积，引入半样本延迟，线性区变成 (x_n + x_{n−1})/2 的 2 点平均 [S13][S14]——48 kHz 下 20 kHz 处 1× −11.7 dB、2× −2.0 dB、4× −0.47 dB [推]。
     所以**只能对非线性残差 r(x) = f(x) − x 做 ADAA**：Heat 本来就是并联残差；Clip 改写成 `out = x_延迟 + down(ADAA(r(up(x))))`，软拐点线性区 r ≡ 0 → 线性区残差恒为 0（FIR 路径逐位直通；IIR 路径要等状态衰减 / 清零后才逐位）。
     混叠：2× 过采样对 1/f 谱只降混叠 6 dB、4× 降 12 dB；ADAA + 4× 对硬削波「几乎不是问题」，ADAA 与低倍过采样配合最好 [S13][S15][S14]；DAFx-24 讨论了更好的插值滤波器与 ADAA 的半样本延迟 [S16]。
     实现：tanh 拐点的原函数是 ln cosh（每样本一次 log1p / exp，估 0.5 % CPU @ 48 kHz [推]）；改用代数 sigmoid f(z) = z/√(1 + z²)（原函数 √(1 + z²)，一阶式写成 (x_n + x_{n−1})/(√(1+x_n²) + √(1+x_{n−1}²))，无除零）更省，谐波强度与 tanh 无显著差别 [S13]，**但会改音色，需 A/B 试听**。
  d. **按采样率降倍率**：内部目标速率约 192 kHz（48 kHz 4×、96 kHz 2×、192 kHz 不过采样 + ADAA）[推]。
- 推荐：Clip 与 Heat 先走 (c) + (b)；若不愿放弃 FIR 契约则 (a)。延迟 60 / 64 → 约 6；CPU 估 ↓ 2 ～ 4×（现 Clip 约 200 flops/样本、Heat 更多；(b) + (c) 约 130）[推，结构估算]。
  Heat 的干路：把干路也过同一条过采样链（残差 = 链内差值），就没有「干路与残差对不齐」的问题，只剩一个很小的 HF 相位偏移 [推]。
- 必验（服务器上跑）：与现 4× / 8× 同口径的混叠门槛；Heat = 0 / Clip = 0 的直通契约；TP 低估（ADR-004）；IIR 的非规格化数尖峰（FTZ / DAZ 开着，另加固定网格上的小量级清零，与块长无关）。

## 8. CPU 预算与调度、块长无关

**预算（建议值，均为目标不是实测）**：48 kHz / 512 平均 ≤ 6 %；48 kHz / 32 最慢块 ≤ 15 %（对 20 % 留 5 点余量）；96 kHz / 64 最慢块 ≤ 20 %；192 kHz 靠降过采样倍率与分析抽取守住。
基线 [P1]：v0.1.6.2 在 M2 Pro 上 48 kHz / 32 平均 1.28 %、最慢块 13 %；192 kHz / 32 平均 5.1 %、最慢块 51 %；0.1.7.2 x86 虚拟机 48 / 96 / 192 kHz 均值 3.3 / 6.4 / 13.0 %。

| 项目 | 变化 | 估算 | 调度 |
|---|---|---|---|
| 实时控制层（直方图 ×2、倾斜回归、特征、servo、落差守门） | 新增 | 合计 < 10 µs / 100 ms（≈ 0.01 %）[推]：直方图更新 + 扫描 1 ～ 2 µs，回归 24 段 < 1 µs，其余 < 1 µs | 100 ms tick 拆 4 ～ 5 片，每个 hop 最多一片 |
| 旧分析器学习期 | 删除 | −56 µs / 512 块（−0.52 %）[P1] | — |
| K 加权 + 子块环 | 已有 | 4 双二阶 / 样本 [P10] | 逐样本 |
| Heat / Clip 过采样 | 变化 | ↓ 2 ～ 4× [推] | 逐样本，无尖峰 |
| 真峰值检测 | 变化 | 8×/64 → 8×/16 ～ 32：线性 ↓ 2 ～ 4× [推] | 逐块 |
| 高采样率分析 | 变化 | Balance 的 hop 与 FFT 随 fs 增长，192 kHz / 32 最慢块 51 % 的主因很可能是每 hop 绝对耗时 ×4 ～ 5 而块时长 ÷ 4（推测，需确认）→ 分析路径先半带抽取到 ≈ 48 kHz 等效 | 抽取滤波每样本摊平 |

**最慢块怎么守**（Bencina 的原则：音频线程只有最坏情况有意义；我没能抓到原文，**未核实**，见 [S34]）：
1. 块时长：48 kHz / 32 = 667 µs，20 % = 133 µs；192 kHz / 32 = 167 µs，20 % = 33 µs。**每片 ≤ 15 µs、每个 hop（10 ms）最多一片**——Balance 已用「hop 后第 0 / 1/16 / 1/8 hop 三片」[P7]，新片放到 100 ms 周期内其余 hop 的空位，不叠在 Balance 的片上。
2. 不允许 O(历史) 重算：直方图固定档数；滑动和用「固定网格精确重算」（LinearPhaseLowpass 的 65536 样本网格 [P3]）也要把一次重算摊成小段。
3. 系数重算放固定网格（现 64 样本一次 [P7]）；一次只重算一部分段，不要 27 个一起。
4. 遗忘用「全局缩放 + 周期性重规范」代替每帧乘 800 档，避免非规格化数；非规格化统一靠 FTZ / DAZ + 网格清零。
5. 超越函数（log10 / pow）只在 tick 上用；逐样本路径只用有理式（Saturator::fastTanh 已是 [P9]）。

**结果与宿主块长无关的写法**：
- 所有控制更新按「从 reset 起的绝对样本计数」触发，块内遇到触发点就切开循环（Balance 的做法 [P7]）；片调度同理，**不看块计数、不看墙钟、不看宿主走带**；
- 增益 / 参数 = tick 之间逐样本线性插值，不是逐块；
- 任何归约（能量和、直方图计数）按**绝对对齐的固定长度块**累加，不按宿主块累加（向量化的横向求和顺序会随分块变）；
- 浮点：不用 fast-math、不依赖 FMA（GCC 全工程 -ffp-contract=off，ADR-013 [P3]）；
- 验证：同一信号用 {1, 7, 32, 64, 480, 512, 4096} 切块逐位对照，**在服务器上跑**。

## 9. 曲风层的参数钩子（本主题范围内；起点值方向性，待标定）

| 钩子 | 通用 | Deep House | Tech House | Melodic / Progressive |
|---|---|---|---|---|
| 参考分位 q / 门深 | 0.75 / 6 LU | 0.80 / 7 LU（breakdown 深而长） | 0.80 / 5 LU（段间落差小） | 0.75 / 8 LU（长 build、长 breakdown） |
| T_hold / 泄漏 | 45 s / 0.05 dB/s | 60 s / 0.04 | 45 s / 0.05 | 75 s / 0.04 |
| 慢回路上 / 下行 | 0.2 / 1.0 dB/s | 0.15 / 1.0 | 0.25 / 1.0 | 0.15 / 0.8 |
| Tone 参考斜率 / 往亮上限 | 沿用 [P1] | −2.25 dB/oct / +0.4 dB [P1] | 同 | 同 |
| Glue GR 目标 | 1.5 dB | 1.0 dB | 1.5 dB | 1.0 dB（保动态） |
| Heat 上限 | 中 | 中低（偏温润） | 低（偏干净） | 中 |
| Clip f_目标 | 3 % | 2 % | 4 % | 2 % |
| 限幅释放 | 自动 | 一拍 ÷ 3 封顶 [P1] | 同 | 同 |
| 收单 fc 下限 / 加宽 | 120 Hz / 保护 | 120 Hz / 不自动加宽 | 120 Hz / 不自动加宽 | 120 Hz / 不自动加宽 [P1] |

「只让主段落决定目标、breakdown 不拉低方案」在新设计里 = 第 2 节的 P_q + 门控更新 + 冻结 / 泄漏（取代 House 的门控画像与「听满 64 小节才锁定」）。
「满编段」判据可再叠加一条频带覆盖条件（LF、中、高三区同时在各自响段 P80 的 X dB 内），那属于分析主题（报告 A）。

## 给 Celadon 的具体建议

| 模块或决策 | 建议做法 | 预期效果 | 延迟 / CPU 代价 | 风险 |
|---|---|---|---|---|
| 响度设定点与参考 | 3 s 短期响度直方图 P80（门开帧、遗忘按被收帧）+ c_曲风 | 开机即用；breakdown 不拉高增益；增益漂移 ≈ 0 [仿] | 控制层 < 10 µs / 100 ms；无延迟 | 首个 drop 过载（build 期间增益被抬）；c_曲风 与参考集偏差（Jamendo 是成品，非未母带混音） |
| 增益回路结构 | G_slow（0.2 up / 1.0 down dB/s、迟滞死区）+ G_budget（比例速率 0.3 ～ 6 dB/s、min 选择器） | 无锁定；保护更快；不饱和 | 同上 | 速率值是起点，需用 22 首 + 合成曲标定 |
| 落差守门 | 输入 / 输出各一份 S 直方图，P90 − P25 之差超 1.2 LU（俱乐部 1.5）收 G_budget | T-038 门槛实时闭环 | 多一份直方图 ≈ 1 µs / tick | 口径与 T-038 测量脚本需对齐 |
| Tone | 24 段 IRLS 斜率 → 迟滞死区 → 0.03 dB/s | 无追逐、无抽吸；在 Tone 输入端测 | 零新增 FFT | 与 Balance target 重叠时需看回路是否趋 0（设计上趋 0） |
| Glue | 阈值 = 响段侧链 P90 − off；off servo 到 GR_tgt | 量纲无关、预算可控 | 可忽略 | 阈值相对响段会让 breakdown 少压，落差损失 ≈ ΔGR |
| Heat | dark / dense / 残差功率比预算 | 越暗越多、越密越少 | 见过采样行 | 系数是起点 |
| Clip | 分位数 servo 钉 clipActiveFraction | 削波量稳定、不随曲子漂 | 可忽略 | f_目标 与听感未验证 |
| Limit 前瞻 / 保持 | 前瞻 2 → 1 ms；hold 63 → ≈ 16 | −1.0 ms，−1.0 ms | 检测 CPU ↓ | ADR-004 同源性；真峰值余量 0.35 → 0.5 dB |
| Clay 收单 D（方案 A） | fc_min 40 → 120 Hz（或两档） | D 574 → 190（−8 ms） | 无 | 手动下限被抬高；需产品确认 |
| Clay 守护 G | W 8 → 2 ms、A 3 → 1 ms；低切降阶 / 降角频率 | G 528 → 144（−8 ms） | 无 | 守护更常动作；次声压制变弱；须复跑压力格 |
| Clay 最小相位档（方案 B1） | S 路 2 阶高通，M 不动；prepare 时选档 | D → 0，总延迟 ≈ 4.6 ms | 极小 | ADR-012 峰值 +1 ～ 4 dB；混合损失；须过压力格 |
| Heat / Clip 过采样 | 残差 ADAA + 2× 多相 IIR 或短最小相位 FIR；倍率随 fs | 延迟 −112 样本；CPU ↓ 2 ～ 4× | 见第 7 节 | 混叠门槛、直通契约、IIR 无限记忆与空闲跳过 |
| 真峰值检测倍率 | 随 fs 降（96 kHz 4×、192 kHz 2×） | 高采样率 CPU 大降 | 负 | 余量与表同源 |
| 控制层调度 | 100 ms tick 拆 4 ～ 5 片，每 hop 最多一片，≤ 15 µs | 最慢块不被新增逻辑拉高 | — | 片位置需与 Balance 三片错开 |
| 高采样率分析 | 分析路径抽取到 ≈ 48 kHz 等效 | 192 kHz / 32 最慢块 51 % 大降（推测） | 少量滤波 | 根因为推测，须先量 |
| 验收（服务器上跑） | 块长逐位对照；混叠；直通；Clay 压力格；T-038 落差；48 k / 32 最慢块 | 回归可拦 | — | 本机一律不跑 |

## 风险与未知

1. **第一个 drop 的过载是因果系统的固有弱点**：仿真里 build 期间增益被抬高 2 ～ 3 dB，drop 起点最大 GR 约 4 ～ 6.5 dB，快保护能压到约 4 dB 但不能归零 [仿]；不能用宿主走带 / 整首信息补救（块长无关）。
2. **标定集偏差**：Jamendo 22 / 45 首是成品，输入响度与 PLR 都偏离「待母带的混音」；c_曲风、PLR 阈值（8 / 14 dB）、Glue / Heat 起点值都需要用未母带的混音补充标定。
3. **T-038 门槛与 T-045 预算口径**：落差损失 ≈ ΔGR（我推的），1.2 LU 门槛 ≠ 2 dB 均值预算；需要确认门槛的测量定义（按段平均响度还是动量曲线）。
4. **ADR-004 同源性**与 **IIR 过采样的无限记忆 vs 现有逐位契约**：两者都是用延迟换契约，需 Max 决定哪边让步。
5. **最小相位收单的峰值代价**无法在本次调研里验证；我只有 ADR-012 的数字与外部文献的方向性证据（0 ～ 3.5 dB）。
6. **门控死锁 / 新曲检测**：门闭 + 泄漏的参数（45 s / 0.05 dB/s）是依据 House 16 ～ 32 小节 breakdown（30 ～ 60 s @ 128 BPM）选的；其他曲风的段长未核实。
7. **所有 CPU / 延迟数字都是推算**：包括「ADAA + IIR 约 6 样本」「控制层 < 10 µs」「过采样 CPU ↓ 2 ～ 4×」；M2 / 服务器实测前不要当结论。仿真是我自写的玩具模型，只检查控制逻辑。
8. **出处缺口**：Bencina 原文抓取失败（连接被拒）；AES e-Library 页面返回 403，S6 / S8 / S22 / S25 只据搜索摘要；Gullfoss 22 ms 延迟与 Pestana 的「5 dB/oct」只在搜索摘要里出现，**未核实**；FabFilter Natural Phase 的实现未公开。
9. **Lookahead 默认值改动**会改变上报延迟，老工程的宿主补偿需要重新协商；采样率切换时延迟变化也同理。
10. **Balance 与 Tone 的耦合**（Brighten 倾斜、targetSlope 与 Tone 同时存在）：设计上回路趋 0，但需要在同一首歌上验证不互相抵消或叠加。

## 出处清单

外部（按文中编号）：
- [S1] EBU Tech 3341（2023，EBU Mode：M 400 ms、S 3 s、门限、实时表重算门限）https://tech.ebu.ch/docs/tech/tech3341.pdf
- [S2] EBU Tech 3342（LRA：3 s 窗、−70 LUFS 绝对门、−20 LU 相对门、P10 / P95）https://tech.ebu.ch/docs/tech/tech3342.pdf
- [S3] ITU-R BS.1770-5（Annex 2 真峰值：4× 48 阶、under-read 表）https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I!!PDF-E.pdf
- [S4] Telos / Omnia Volt「Wideband AGC Gating」（低于门限则冻结、RTZ 速度）https://docs.telosalliance.com/docs/adjusting-the-volts-wideband-agc-gating
- [S5] Dolby US 9,806,688（响度整平：短时 / 长时模块、门控、迟滞、空带；示例值）https://patents.google.com/patent/US9806688
- [S6] Mansbridge / Finn / Reiss, AES 132, 2012（迟滞响度门 + 选择性平滑；据搜索摘要，页面 403）https://aes2.org/publications/elibrary-page/?id=16226
- [S7] Maddams / Finn / Reiss, DAFx-12（RMS − 12 dB 阈值、LRA 控制压缩）https://www.dafx.de/paper-archive/2012/papers/dafx12_submission_6.pdf
- [S8] Giannoulis / Massberg / Reiss, JAES 60(6), 2012（压缩器设计教程；据搜索摘要，页面 403）https://www.academia.edu/31481096/Digital_Dynamic_Range_Compressor_Design_A_Tutorial_and_Analysis
- [S9] Signalsmith Audio, Designing a straightforward limiter（2022）https://signalsmith-audio.co.uk/writing/2022/limiter/
- [S10] FabFilter Pro-L 2 Help, Advanced settings（前瞻取舍）https://FABFILTER.COM/help/pro-l/using/advancedsettings
- [S11] iZotope Ozone 9 Maximizer Help（IRC LL / III / IV 与延迟、CPU）https://downloads.izotope.com/docs/ozone9/en/maximizer/index.html
- [S12] JUCE dsp::Oversampling 文档（FIR 线性相位延迟最大、IIR 相位受损延迟最小）https://docs.juce.com/master/classdsp_1_1Oversampling.html
- [S13] Vicanek, Note on Alias Suppression in Digital Distortion（ADAA 半样本延迟、与过采样的关系、代数 sigmoid）https://www.vicanek.de/articles/AADistortion.pdf
- [S14] Holters, Antiderivative Antialiasing for Stateful Systems, Applied Sciences 10(1), 2020 https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2020/10/applsci-10-00020-v2.pdf
- [S15] Parker / Zavalishin / Le Bivic, DAFx-16 https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA
- [S16] Zheleznov / Bilbao, Interpolation Filters for Antiderivative Antialiasing, DAFx-24 https://dafx.de/paper-archive/2024/papers/DAFx24_paper_33.pdf
- [S17] MathWorks designHalfbandIIR（椭圆 vs quasi-linear 相位）https://www.mathworks.com/help/dsp/ref/designhalfbandiir.html
- [S18] HIIR（Laurent de Soras；unevens 分支，含 Neon / SSE）https://github.com/unevens/hiir
- [S19] Välimäki / Fierro / Schlecht / Backman, Audio Peak Reduction Using Ultra-Short Chirps, JAES 70(6), 2022 https://acris.aalto.fi/ws/portalfiles/portal/86964673/Audio_Peak_Reduction_Using_Ultra_Short_Chirps.pdf
- [S20] Liski / Mäkivirta / Välimäki, Audibility of Group-Delay Equalization, IEEE/ACM TASLP 29, 2021 https://acris.aalto.fi/ws/portalfiles/portal/66449704/Audibility_of_Group_Delay_Equalization.pdf
- [S21] Linkwitz Lab, Active crossover filters（LR2 / LR4、对低频群延迟的更正）https://www.linkwitzlab.com/filters.htm
- [S22] Lipshitz / Vanderkooy, JAES 31(1/2), 1983（据搜索摘要）https://aes2.org/publications/elibrary-page/?id=4462
- [S23] FabFilter Pro-Q Help, Processing mode（线性相位延迟、Natural Phase）https://fabfilter.com/help/pro-q/using/processingmode
- [S24] J. O. Smith, Minimum Phase Filter Design（倒谱法）https://ccrma.stanford.edu/~jos/sasp/Minimum_Phase_Filter_Design.html
- [S25] Gardner, Efficient convolution without input-output delay, JAES 43(3), 1995（据搜索摘要）https://aes2.org/publications/elibrary-page/?id=7957
- [S26] Essentia, Interpeak detection strategies https://essentia.upf.edu/tutorial_audioproblems_interpeak_detection_estrategies.html
- [S27] Jain / Chlamtac, The P² algorithm, CACM 1985 https://www.cs.wustl.edu/~jain/papers/psqr.htm
- [S28] Wikipedia, Auditory masking（后向 / 前向掩蔽约 20 / 100 ms；二手）https://en.wikipedia.org/wiki/Auditory_masking
- [S29] Wikipedia, EBU R 128（PLR 定义；据搜索摘要）https://en.wikipedia.org/wiki/EBU_R_128
- [S30] Pestana / Ma / Reiss / Barbosa / Black, AES 135, 2013（只取摘要）https://ciencia.ucp.pt/en/publications/spectral-characteristics-of-popular-commercial-recordings-1950-20/
- [S31] audioXpress, Gullfoss（频响更新 > 100 次 / 秒）https://audioxpress.com/news/soundtheory-unveils-gullfoss-intelligent-automatic-equalizer
- [S32] Wikipedia, Spectral slope（回归定义；据搜索摘要）https://en.wikipedia.org/wiki/Spectral_slope
- [S33] Violet Recording, Mastering for vinyl（低于约 100 ～ 150 Hz 并单声道、椭圆均衡；二手，据搜索摘要）https://violetrecording.com/mastering-for-vinyl/
- [S34] Bencina, Real-time audio programming 101（抓取失败，未核实内容）http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing

项目内（只读）：[P1] docs/validation/2026-10-02-T047-realtime-research/BRIEF.md；[P2] Source/dsp/ClayModule.h；[P3] Source/dsp/LinearPhaseLowpass.h；[P4] Source/dsp/TruePeak.h；
[P5] Source/dsp/LimiterModule.h；[P6] Source/dsp/ColorModule.h；[P7] Source/dsp/BalanceModule.h；[P8] Source/dsp/HalfBandOversampler.h；[P9] Source/dsp/Saturator.h；
[P10] Source/dsp/LoudnessMeter.h；[P11] Source/dsp/GlazeGuard.h；[P12] Source/dsp/ClayGuard.h。
仿真与算术脚本：会话临时目录 scratchpad/calc（d_table.py、phase.py、lowcut.py、adaa.py、loop_sim*.py），不入库。
