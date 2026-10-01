# R2 · Balance 动态频谱平衡（BalanceModule / ReferenceSpectrum / MasteringFeatures）深度审计

日期 2026-09-29 夜（服务器时间 2026-09-30 05:00 UTC）。代理 R2。基线 main `e6b445b`（v0.1.8.5）。只读审计，未改仓库任何已有文件。

## 0. 摘要

读了 `设计规格-Balance.md`、`设计规格-高级母带控制.md`、`ADR-020`、`docs/reference-chain-algorithm.md`、`docs/design/0.1.9-mastering-controls.md`、`docs/heat-and-balance-revision.md`，逐行读了 `Source/dsp/BalanceModule.{h,cpp}`（1780 行）、`ReferenceSpectrum.{h,cpp}`、`MasteringFeatures.h`，以及 `CeladonChain.cpp` / `PluginProcessor.cpp` 里的调用点，对照 `tests/{BalanceTests,BalancePerformanceTests,AdvancedMasteringTests}.cpp` 的用例清单；在 107 上用 numpy 复现了 Balance 的分析口径与跟踪链，做了 5 组数值实验（第 3 节）。

**8 条结论，按 优先级 × 置信度 排序**：

| # | 严重度 | 优先级 | 一句话 | 改后逐位不变 | 工作量 |
|---|---|---|---|---|---|
| R2-01 | high | P0 | 推响不刺的超出量把本段 **fastLevel（释放 400 ms）** 与邻段 **quickLevel（60 ms）** 混用：宽带电平一降就算成「刺耳」，鼓点素材 100 % 时间被多削 1 ～ 3 dB（h = 1） | 否（只在 harsh > 0 时变） | S |
| R2-02 | medium | P1 | 27 个 Q ≈ 4.3 峰值滤波器直接用 G[b] 当增益，带间串扰行和 ≈ 1.6：连片修正实现值比目标深 40 ～ 60 %，Tame 8 dB 封顶实际 12.3 dB；单段 −8 dB 时邻段中心被拉到 −1.7 dB | 否 | M |
| R2-03 | medium | P1 | 参考学习用 8192 点非重叠帧，实时目标测量用 2048 点：口径不同 → 参考模式自我不一致，同一首歌当参考也会在 80 Hz 段产生 −2.5 dB × t 修正 | 否（只在参考模式变） | M |
| R2-04 | high | P1 | 2048 点 Hann 主瓣 ±47 Hz 比 40 ～ 125 Hz 各段都宽：强低音（41 ～ 55 Hz）把 63 / 80 Hz 段测量抬 12 ～ 19 dB，Tame 100 稳态把并无内容的 80 Hz 段压满 −8 dB、而真正的低音段只压 −1.6 ～ −5.3 dB | 否 | L |
| R2-05 | low | P1 | 上行掩蔽的距离 4 永远触发不了（需要间隙 ≥ 18.7 dB，上限 16 dB）：白跑一轮、与规格「1 ～ 4 段」不符 | 是（改 maxDistance = 3） | S |
| R2-06 | low | P2 | 每 hop 的重复超越函数（`pow(10, C/10)` 在段循环里算 27 次、`10·log10(quickPower)` 与已有 `quickLevelDb` 重算 46 次、A 计权权重每 hop 现算、4 个 `exp` 系数每 hop 现算）与功率谱累加多做 25 %（48 k）～ 81 %（192 k）的无用 bin | 是 | S |
| R2-07 | low | P2 | 系数重算间隔固定 64 样本不随采样率缩放：192 kHz 下每秒 81 k 次 `designPeak`（pow + 5 次除法），时间粒度也比 48 k 细 4 倍、没有必要 | 否（仅 ≥ 88.2 k） | S |
| R2-08 | low | P2 | 起音保留把「已施加的削减」硬乘 (1 − p)、再硬乘回来，没有平滑：真实鼓点每秒触发 9 ～ 10 次，h = 1、p = 0.5 时 2 ～ 8 kHz 的削减在 50 % ↔ 100 % 之间以 5 ～ 10 Hz 来回跳（最大 1.5 dB 台阶） | 否（只在 harsh > 0 且有起音时变） | S |

**核对过、没有发现缺陷的项**（写出来免得别人再查）：音频线程无分配（JUCE 9.0.2 回退 FFT 引擎 `maxFFTScratchSpaceToAlloca = 256 KB`，8192 点只用 64 KB alloca，`juce_FFT.cpp:134-146`；ReferenceSpectrum 自带无分配 FFT；`PerBand` 全是栈数组）；音频线程无锁、无系统调用（`restoreMutex` 只在 `restoreStateXml`，`yield()` 只在 `createStateXml`）；NaN / Inf：输入置零由处理器做（`PluginProcessor.cpp:595`）、输出非有限整链 reset（`CeladonChain.cpp:255-257`），`measureBandPowers` / `measureBandCross` 归零；`Recover = Tame = Brighten = 0` 逐位直通的推导（`designPeak` 在 A = 1 时 b0 = a0/a0 = 1.0、b1 ≡ a1、b2 ≡ a2）成立；SSE2 / NEON 打包路径与标量逐位一致的前提（`-ffp-contract=off`、`__FMA__` 守卫）在源码里齐全；时间常数全部按 `hopSeconds` 折算、fftSize 随采样率翻倍，44.1 / 48 / 96 / 192 k 一致；旁通零延迟、上报 0 正确；参考 XML 往返用 `serialiseDouble` 全精度、范围与功率和校验齐全；起音判据（相邻帧功率比 1.7）在平稳白 / 粉噪上误触发率很低（2 kHz 段 0.6 ～ 0.9 次/s，其余段 ≈ 0，见 3.3），不构成缺陷。

## 1. 范围与方法

- 规格 → 代码 → 测试的顺序读；每条结论都给 文件:行号 与代码引用。
- 数值实验在 107（`~/.cache/celadon-t032/R2/`，已删除）用 numpy 2.5.3 / scipy 1.18.1 跑，脚本见附录 A，输出原文见第 3 节。实验复现的是源码里的口径：2048 点周期 Hann、hop 512、`spectrumScale = 2/(N·Σw²)`、段 → bin 按重叠宽度计权（`BalanceModule.cpp:308-325`）、Welch 时间常数 τ = 自由度 / (4 · 等效带宽)（`:327-335`）、攻击 / 释放（`:63-66`）、60 ms 快电平（`:158`）、RBJ 峰值滤波器（`:854-870`）。
- 不报已知待认领项（Clay 预算、55 Hz Side 尖峰、96k/32 最大值、NEON Linux ARM、界面 21 %、起音密度、编码余量、AU 版本、BUG-001 ～ 004）。

## 2. 结论清单

### R2-01 · 推响不刺的超出量混用两种时间常数：宽带电平一降就误判「刺耳」

- 严重度 **high** · 优先级 **P0** · 类别 算法正确性（时间常数不匹配）· 工作量 **S** · 置信度 **0.9** · 改后输出：只在 `harshAmount > 0` 时改变，为 0 时逐位不变。
- 文件：`Source/dsp/BalanceModule.cpp:1725-1727`。

**证据**：

```cpp
// :1725
const auto left = quickLevelDb[b - 3], right = quickLevelDb[b + 2];
const auto support = juce::jmin (gateWeight[b - 3], gateWeight[b + 2]);
const auto excess = juce::jmax (0.0, fastLevelDb[b] - 0.5 * (left + right) - 1.5);
```

`fastLevelDb[b]` 是经过 Welch 功率平均（4 kHz 段 τ = 54 ms，Mid）再经 **攻击 80 ms / 释放 400 ms**（Slow 800 ms）的跟踪（`:1144-1145`）；`quickLevelDb` 是所有段同一个 **60 ms** 对称功率平均（`:1170-1178`）。两者只在稳态相等。同一文件里目标频谱那段的注释自己写明了原则：「Use the common-rate input tracker: per-band Welch time constants would confuse a level step with spectral tilt」（`:1692`），刺耳判据违反了它。

**机理**：宽带电平下降（副歌结束、鼓点之间的间隙、任何 decay）时，邻段的 quick 在 60 ms 内跟下去，本段的 fast 还挂在释放曲线上 → `excess` 出现正值 → `harsh = min(3, 0.65·excess)·h`，包络 25 ms 攻击立刻把它做上去。上升沿相反（quick 比 fast 快）不触发，所以这是单向的：只在「变轻」时削 2 ～ 8 kHz，也就是把每个 decay / 尾巴 / 间隙的高中频压暗。

**实验（3.2，理想平谱 27 段同步变化、真实刺耳超出量恒为 0）**：

| 场景（Mid） | appliedHarsh 峰值 h = 1 | 新实例默认 h = 0.175 | > 1 dB 持续 |
|---|---|---|---|
| 宽带阶跃 −6 dB | 1.57 dB | 0.27 dB | 0.39 s |
| 宽带阶跃 −10 dB | 2.99 dB | 0.52 dB | 0.70 s |
| 宽带阶跃 −20 dB | 3.00 dB（封顶） | 0.52 dB | 1.06 s（Slow 1.89 s） |
| 宽带阶跃 +10 dB | 0.00 dB | 0 | — |
| 鼓点式脉冲（250 ms 周期、50 ms −12 dBFS / 其余 −30 dBFS） | 均值 1.84、最大 2.91 dB，**100 % 时间 > 1 dB** | 均值 0.32 dB | 全程 |

同一模型把本段改成 `quickLevelDb[b]` 后，以上全部精确为 0；真正持续的局部峰（本段比邻段高 6 dB）两种写法稳态都是 2.925 dB（h = 1）。

**修法**（一处）：

```cpp
// :1727  本段与邻段用同一种 60 ms 快电平，三者动态完全一致，电平共同变化时相消
const auto excess = juce::jmax (0.0, quickLevelDb[b] - 0.5 * (left + right) - 1.5);
```

备选：三者都用 `fastLevelDb`（与自动 Tame 的 `fastLevelDb − smoothedLevelDb` 同口径，`:1473`），更平滑但对刺耳段的起止反应慢（释放 400 ms），且各段 Welch τ 不同（2 kHz 108 ms / 8 kHz 27 ms）仍会留下 ≤ 80 ms 的小残差；推荐 quick。顺带：`masteringPreviousPower` 只在该块启用时更新（`:1733`），刺耳从关到开的第一 hop 会拿陈旧值比较、可能白触发一次 55 ms 起音保持——把 `masteringPreviousPower[b] = bandPower[b]` 挪到 `previousBandPower` 旁边（`:1488`）每 hop 更新即可（逐位不变：只影响 harsh > 0 的第一帧）。

**验证**：新增 `AdvancedMasteringTests`：粉噪 −20 dBFS、`harshAmount = 1`，6 s 处整体 −10 dB 阶跃，断言阶跃后 2 s 内 `getHarshReductionDb()` 峰值 < 0.3 dB（现在 2.99）；鼓点式脉冲（上表）断言 2 ～ 8 kHz 段 G 的均值 > −0.3 dB；保留现有「persistent 3.15 kHz peak」用例证明真刺耳仍被削。规格 `设计规格-高级母带控制.md` 第 4 节「超出量 = 本段 Lfast − …」同步改为「本段 60 ms 快电平」。

### R2-02 · 滤波器组带间串扰未补偿：连片修正实现值比目标深 40 ～ 60 %

- 严重度 **medium** · 优先级 **P1** · 类别 算法精度（执行级）· 工作量 **M** · 置信度 **0.9** · 改后输出：改变（任何 G ≠ 0 时）。
- 文件：`Source/dsp/BalanceModule.cpp:295`（Q）、`:854-870`（designPeak）、`:872-907`（updateCoefficients 直接用插值后的 G 设计滤波器）。

**证据**：`filterQ = sqrt(2^(1/3)) / (2^(1/3) − 1) ≈ 4.318`，RBJ peakingEQ 的带宽定义在 **半增益** 处，相邻段中心只差 1/3 倍频程 = 一个带宽，于是每个滤波器在邻段中心还有 20 % 的增益、隔一段 5.5 %（3.1 实测矩阵）。`updateCoefficients` 把 `hopStartGainDb + (targetGainDb − hopStartGainDb) · fraction` 直接交给 `designPeak`，没有任何交互补偿；`publishGains` 给界面的也是这个 G。

**实验（3.1，48 kHz；192 kHz 数字相同）**：

| 目标 | 实现（段中心） |
|---|---|
| 单段 1 kHz +6 dB | 1 kHz +6.00，**800 Hz +1.23、1.25 kHz +1.23**，630 Hz / 1.6 kHz +0.35 |
| 单段 1 kHz −8 dB（Tame 封顶） | 邻段中心 **−1.68 dB** |
| 3 段连片 −6 dB | 中心 **−8.45**（偏差 −2.45），连片外第 1 段 −1.73 |
| 5 段连片 −8 dB | 中心 **−12.34**（偏差 −4.34），段边界 −10.0，连片外第 1 段 −2.56 |
| 5 段连片 +6 dB（Recover 封顶） | 中心 **+9.16** |
| 全 27 段 +3 dB | 1 kHz +4.83（行和 1.59 ～ 1.61，端点 1.09 / 1.29） |
| Brighten 满档（目标 100 Hz −3 / 10 kHz +3，跨度 6.0） | 实现 −4.76 / +3.89，跨度 **8.65**（`docs/heat-and-balance-revision.md` 粉噪实测 7.42，同向） |

**机理**：这是级联峰值式图示均衡器的经典问题（gain-matrix 校正的由来）：实现响应 ≈ M · G，M 是 27 × 27 交互矩阵（单段 +1 dB 在各段中心的 dB 响应），行和 ≈ 1.6、条件数 2.3。后果：① 规格与界面里所有以 dB 表述的封顶 / 强度（Tame 8、Recover 6、刺耳 3、目标 ±2.5、掩蔽 1.25、倾斜 3 dB/十倍频）对宽于 1 段的偏差都被放大 1.4 ～ 1.6 倍，对孤立单段不放大——**有效强度取决于偏差有多宽**，试听调出来的常数因此互相牵连；② 验收 4「相邻段 |G| < 1 dB（定位准确）」只看目标 G，实际邻段中心已被 −1.7 dB；③ GLAZE 规划器按 Balance 的 dB 量预算响度 / 校准时拿到的是偏小的数。

**修法**（全部在 BalanceModule 内）：

1. `prepare` 里算 M：对每段 j 用 `designPeak (j, 1.0)` 的系数求 |H_j(e^{jω_i})|（ω_i = 各段中心，与 `cosW0` 同一套钳位），`M[i][j] = 20·log10|H_j|`，做一次 LU（或直接求逆）存进固定 `std::array<double, 27 * 27>`；无分配。
2. 每 hop 在限速之后（`:1760-1763`）多一步 `filterGainDb = M⁻¹ · targetGainDb`（729 次乘加），`hopStartGainDb` / 插值 / `designPeak` 改用 `filterGainDb`；`targetGainDb` 语义保持「各段中心希望实现的增益」，界面曲线仍显示它。全 0 时 M⁻¹ · 0 精确为 0，`Recover = Tame = Brighten = 0` 仍逐位直通。
3. 实验核对：一次线性求解后 1 / 3 / 5 段各例中心误差 ≤ 0.12 dB（RBJ 响应对增益略非线性）；再迭代一次 `g' += M⁻¹(g − realised(g'))` 到 1e-4 dB——但 realised 要按真实非线性响应算，每 hop 不值得，一次求解够用。预失真后单段 −8 需要邻段 +1.6 dB 的**滤波器**增益（实现值仍为 0），工作的滤波器数会从 1 变成约 5，CPU 略增。
4. 同时更新规格：「增益 = G[b]」改为「滤波器增益 = M⁻¹ G，使各段中心实现 G」。

**验证**：新增测试，用脉冲响应实测 27 段级联在各段中心的响应（`juce::dsp::FFT` 或直接把 `designPeak` 的系数复用），对 1 / 3 / 5 段目标断言 |实现 − 目标| < 0.2 dB（现在 5 段 −8 差 4.3 dB）；粉噪收敛、扫频、电平阶跃、逐位直通四条老验收照跑。要重跑 GLAZE 校准相关的听感门槛（ADR-016）。

### R2-03 · 参考学习与实时目标测量口径不同：参考模式自我不一致

- 严重度 **medium** · 优先级 **P1** · 类别 设计（模块间一致性）· 工作量 **M** · 置信度 **0.85** · 改后输出：只在 `TargetMode::reference` 下改变。
- 文件：`Source/dsp/ReferenceSpectrum.cpp:150-153`（fftSize：2^k ≥ fs / 6，48 k → 8192）、`:215-224`（非重叠整帧）、`:186-202`（段权重）；`BalanceModule.cpp:260-266`（2048 × fs / 48 k）、`:1704-1705`、`:1744`（目标测量用 `quickPower`，即 2048 点、hop 512 的 60 ms 均值）。

**证据**：两边的段边界与 bin 计权公式相同（ReferenceSpectrum 注释「Same exact third-octave edges as BalanceModule」），但 **FFT 长度差 4 倍**（bin 5.86 Hz 对 23.4 Hz，Hann 主瓣 ±11.7 Hz 对 ±47 Hz），参考还是非重叠帧、实时是 75 % 重叠。目标修正把两者各自去中心后相减（`:1744-1747`），差值过 1 dB 死区 × 0.35、封顶 ±2.5。

**实验（3.4，去中心后 live − reference，b = 3 ～ 25）**：

| 信号 | 最大偏差 | 只因口径差产生的目标修正（t = 1） |
|---|---|---|
| 粉噪 | +0.36 dB @ 80 Hz | 0 |
| 粉噪经 80 Hz 4 阶高通 | −0.51 dB @ 80 Hz | 0 |
| 粉噪 + 90 Hz 以下 +12 dB（低音重） | +0.86 dB @ 160 Hz | 0 |
| 粉噪 + 3 kHz 正弦 −15 dBFS | +0.36 dB | 0 |
| **粉噪 + 55 Hz 正弦 −10 dBFS** | **+17.68 dB @ 80 Hz**，其余 19 段偏 −1.0 ～ −1.2 | **−2.50 dB @ 80 Hz** |
| **粉噪 + 41 Hz 正弦 −10 dBFS** | **+11.42 dB @ 80 Hz** | **−2.50 dB @ 80 Hz** |

也就是说，把一首低音突出的歌自己当参考再送进主输入，目标频谱会在 80 Hz 段产生 −2.5 dB × t 的修正（默认 t = 0.35 → −0.9 dB）——「参考 = 自己」应当是零修正，这是参考模式的自洽性缺陷。根因是 R2-04 的泄漏只出现在实时一侧；对**相对**匹配来说，两边口径一致比各自绝对准确更重要。

**修法**：让参考学习用与实时目标测量**完全相同**的段测量：把 Balance 的「加窗 → FFT → 功率 → 段求和（含 spectrumScale）」抽成一个可复用的分析器（或让 `ReferenceSpectrum::prepare` 取 `fftSize = 2048 · 2^ceil(log2(fs / 48000))`、hop = N / 4、同一 Hann、同一权重表），累计 10 s 的段功率均值 / 总功率；分片预算（每样本 16 个工作单元）对 2048 点更宽裕。存档格式不变（仍是 27 段相对 dB），但老存档是 8192 口径——`format` 升到 2，读到 format 1 时保留但提示重新学习（或按 R2-04 落地后再定）。若 R2-04 的多分辨率分析落地，参考学习同样跟随它的低频口径。

**验证**：新增测试「自参考零修正」：任意素材（含 55 Hz 强低音）学成参考后再作为主输入送 8 s，断言 `getTargetCorrectionDb()` 全段 |值| < 0.15 dB（现在 80 Hz 段 −0.9 × … 至 −2.5）。

### R2-04 · 低频段比 Hann 主瓣窄：强低音泄漏进 63 / 80 Hz 段，Tame 削空段、漏削真低音

- 严重度 **high** · 优先级 **P1** · 类别 算法正确性（分析分辨率 / 窗泄漏）· 工作量 **L** · 置信度 **0.85** · 改后输出：改变（有低音内容时段 0 ～ 5 的 G 变）。
- 文件：`Source/dsp/BalanceModule.cpp:46-50`（2048 点）、`:308-325`（比 bin 还窄的段 = 盖住它那格 × 段宽 / Δ）、`:1194-1215`（参考轮廓核 ±3 段）、`:1534-1544`（Tame 判断）；`tests/BalanceTests.cpp:1102`（「Hann 窗旁瓣到相邻段已低于 −45 dB，正弦不会『漏』进邻段」——只对 ≥ 160 Hz 的段成立）。

**证据 / 机理**：48 k / 2048 点的 bin 23.4 Hz，周期 Hann 主瓣 ±2 bin = ±47 Hz。40 ～ 125 Hz 六个段的带宽只有 9 ～ 29 Hz，一个 55 Hz 正弦（2.35 bin）在 bin 3（58.6 ～ 82 Hz，偏 0.65 bin → −2.5 dB）与 bin 4（−19 dB）都有主瓣能量，而 bin 3 有 46 % 的宽度归 80 Hz 段——80 Hz 段的读数被抬到接近正弦本身。v0.3 修订只讨论了 1-bin 段的**方差**（自由度、Welch 平均），没有讨论**泄漏**；而现代母带里 40 ～ 60 Hz 的 kick / bass 基音常是全曲最响的成分。

**实验（3.5，稳态 R = 抹平后的 Lfast，Tame 100，D = L − R，G = −min(2E, 8)）**：

| 信号 | 80 Hz 段 L(2048) / L(8192) | G_tame(2048) / G_tame(8192) @ 80 Hz | 真低音段 G(2048) / G(8192) |
|---|---|---|---|
| 粉噪 −20 + 55 Hz −10 dBFS | −17.1 / −35.7（**+18.6**） | **−8.00 / 0.00** | 50 Hz −1.57 / −8.00，63 Hz −5.30 / −8.00 |
| 粉噪 −20 + 55 Hz −20 dBFS | −26.6 / −36.0（+9.4） | **−3.87 / 0.00** | 50 Hz −0.64 / −8.00 |
| 粉噪 −20 + 41 Hz −10 dBFS | −24.1 / −36.0（+11.9） | −1.64 / 0.00；63 Hz −3.79 / 0.00 | 40 Hz −0.28 / −8.00 |
| 粉噪 −20 + 48 Hz −14 dBFS | −23.8 / −36.0（+12.2） | **−4.35 / 0.00**；63 Hz −3.36 / 0.00 | 50 Hz −3.86 / −8.00 |

两个方向都错：① 并无内容的 80 Hz 段被压 4 ～ 8 dB（Q 4.3 的 80 Hz 滤波器同时削掉真实的 71 ～ 90 Hz 内容——kick 的箱体、41 Hz 低音的二次谐波 82 Hz——并按 R2-02 的串扰把 63 / 100 Hz 也带下去）；② 真正突出的低音段因为能量被抹到 4 个段上，轮廓被抬平，只压 −0.3 ～ −5 dB 而不是 −8。同样的帧快照（`bandPower[0..5]`、`bandCross`）也交给 GLAZE 分析器与 Clay（单声道 / 低切决策）——R1 / R3 请注意低段读数含此泄漏。

**修法**（多分辨率分析，BalanceModule 内闭环，不动帧快照契约）：

1. 低段（b = 0 ～ 5，≤ 125 Hz）改用 4 倍长窗（48 k → 8192 点，bin 5.86 Hz，主瓣 ±11.7 Hz；3.4 / 3.5 的对照列证明它对 41 ～ 55 Hz 正弦已干净）：环形缓冲扩到 4 · fftSize；长 FFT **每 4 hop 做一次**（低段的 Welch τ ≥ 0.4 s，1.4 s @ 40 Hz，4 hop = 43 ms 的更新间隔毫无影响），CPU 与现在一次 2048 点 / hop 持平；也可先按 8 抽取（1 阶 + 2 阶抗混叠 IIR）再做 1024 点，更省。
2. 新增 `trackedPower[b]`：b ≤ 5 取长窗结果、其余 = `bandPower`；`updateTrackers` / `updateCorrectionTargets` / 谐波保护读 `trackedPower`，`publishFrameSnapshot` 仍发布 2048 点的 `bandPower` / `bandCross`（GLAZE、Clay 契约不变；将来是否切换另开 ADR）。
3. 低段 τ 公式里的 `hannBinEquivalentBandwidth · binHz` 用新 bin 宽（5.86 Hz）——等效带宽下降、τ 不变（仍由段宽决定）。
4. 规格「分析」第 1 条与 v0.3 修订第 1 条补一句「低段用 4 倍长窗，只为压泄漏」。

**验证**：新增测试：粉噪 −20 dBFS + 55 Hz 正弦 −10 dBFS，Tame 100，稳态断言 80 Hz 段 G > −1 dB（现在 −8）、50 / 63 Hz 段 G < −5 dB（现在 −1.6 / −5.3）；41 Hz 版本同理；粉噪收敛验收照跑。

### R2-05 · 上行掩蔽的距离 4 永远触发不了

- 严重度 **low** · 优先级 **P1** · 类别 算法 / 规格不一致 + 无用循环 · 工作量 **S** · 置信度 **0.95** · 改后输出：**逐位不变**（把 `maskingMaxDistance` 改 3）。
- 文件：`Source/dsp/BalanceModule.cpp:112-120`、`:1606-1614`。

**证据**：

```cpp
constexpr int    maskingMaxDistance    = 4;
constexpr double maskingFloorOffsetDb  = 2.0;
constexpr double maskingUpwardSlopeDb  = 4.0;
constexpr double maskingMinBuriedDb    = 0.70;
constexpr double maskingMaxGapDb       = 16.0;
// :1608  if (gap <= 0.0 || gap > maskingMaxGapDb || …) continue;
// :1611  const auto buried = gap - maskingFloorOffsetDb - maskingUpwardSlopeDb * (double) distance;
// :1613  if (buried < maskingMinBuriedDb) continue;
```

距离 d 要求 gap ≥ 2 + 4d + 0.7：d = 1 → 6.7，d = 2 → 10.7，d = 3 → 14.7，**d = 4 → 18.7 > 16 = 上限**，空集。规格（`设计规格-Balance.md` 2026-09-28 节）与 `docs/reference-chain-algorithm.md` 都写「往上 1 ～ 4 段」。

**修法**：二选一：(a) `maskingMaxDistance = 3`，规格改成「1 ～ 3 段」，逐位不变（推荐，今晚可做）；(b) 若确实想让 4 段外也接收，`maskingUpwardSlopeDb = 3.0`（d = 4 需 gap ≥ 14.7）——改变输出，要重跑掩蔽测试。**验证**：现有「upward masking lifts a buried higher neighbour…」用例 + 逐位摘要用例（`BalanceTests.cpp:2376`）在 (a) 下必须逐位相同。

### R2-06 · 每 hop 热路径的重复超越函数与无用 bin 累加（全部逐位不变）

- 严重度 **low** · 优先级 **P2** · 类别 优化 · 工作量 **S** · 置信度 **0.9** · 改后输出：**逐位不变**（同一表达式、同一输入、同一 libm）。

| 位置 | 现状 | 改法 |
|---|---|---|
| `:1250` `const auto commonPower = std::pow (10.0, commonLevelDb * 0.1);` 在 27 段循环体内；`:1240` 锁定分支同样 | 27 次 pow 算同一个值 | 循环前算一次 |
| `:1705`、`:1744` `10.0 * std::log10 (juce::jmax (powerFloor, quickPower[b]))` | 与 `:1177` 的 `quickLevelDb[b]` 表达式逐字相同，`updateTrackers` 先于 `updateCorrectionTargets` 执行 | 直接读 `quickLevelDb[b]`，省 46 次 log10 |
| `:127-137` `maskingLoudnessWeight (band)`：pow + sqrt + log10 + pow | 锚定时每 hop 对 ≤ 21 段现算 | `prepare` 时算进 `PerBand<double> maskingWeight` |
| `:1627`、`:1713-1715` 四个 `std::exp (-hopSeconds / τ)` | 每 hop 现算，τ 是常数 | `prepare` 时算好（同 `attackCoeff`） |
| `:950` `std::fill (powerSpectrum…)`、`:1021-1026` `accumulatePower` 遍历 `fftSize / 2 + 1` 个 bin | 段只覆盖到 bin ≈ 770（17.96 kHz，与采样率无关，bin 都是 23.4 Hz），谐波保护最多读到 +3；48 k 多做 25 %，192 k 多做 81 %（4097 对 ≈ 773） | 上限改为 `bandFirstBin[26] + bandNumBins[26] + 3`（prepare 时存一份），fill 同 |

量级诚实说：48 k 每 hop 约 160 次超越函数 ≈ 2 ～ 4 µs / 10.7 ms ≈ 0.03 % 单核；192 k 的累加省约 3.3 k × 2 声道次乘加 ≈ 3 ～ 6 µs，落在最坏块（分析片 1 / 2）里。都不大，但零风险、半小时的活。**验证**：现有逐位摘要用例（`BalanceTests.cpp:2376` 「logged digests」、拆片 = 一口气、打包 = 标量）全部逐位相同即可。

### R2-07 · 系数重算间隔固定 64 样本，不随采样率缩放

- 严重度 **low** · 优先级 **P2** · 类别 优化（可跳过的计算）· 工作量 **S** · 置信度 **0.7** · 改后输出：44.1 / 48 k 逐位不变，≥ 88.2 k 改变（增益台阶从 1/32 hop 变回 1/8 hop）。
- 文件：`Source/dsp/BalanceModule.cpp:44`（`coefficientUpdateInterval = 64`）、`:872-907`、`:956`（拆片间隔 32）。

**证据 / 机理**：hop = fftSize / 4 随采样率翻倍（48 k 512 → 192 k 2048），重算间隔不变，于是 192 k 下每 hop 重算 32 次而不是 8 次：修正在动时 27 段每次都走 `designPeak`（1 次 pow + 5 次除法 + 状态归零检查），192 k 每秒 3000 × 27 = 81 k 次 ≈ 3 ～ 4 ms/s ≈ 0.3 ～ 0.4 % 单核——与 Balance 在 48 k 的整块开销（0.208 %，v0.5 实测）同量级，而 1/32 hop 的增益粒度（每步 ≤ 0.03 dB）没有可闻收益。

**修法**：`coefficientUpdateInterval = 64 << (order − baseFftOrder)`、拆片间隔 = 其一半（都仍整除 hop，hop 边界仍落在重算网格上；拆片的三片仍在 hop 后第 0 / ¼ / ½ 个间隔）。**验证**：「output independent of block size at 192 kHz」「sliced = one-shot」「packed = scalar」用例照跑；性能用例 `analysis slicing: worst block at 192 kHz / 32 samples` 的门槛复核。

### R2-08 · 起音保留是无平滑的硬开关

- 严重度 **low** · 优先级 **P2** · 类别 算法（增益不连续）· 工作量 **S** · 置信度 **0.65** · 改后输出：只在 harsh > 0 且有起音时改变。
- 文件：`Source/dsp/BalanceModule.cpp:1728`、`:1738-1739`。

**证据**：

```cpp
const auto onsetPermit = masteringOnsetHold[b] > 0.0 ? 1.0 - (double) frameParams.transientPreserve : 1.0;   // :1728
…
const auto appliedHarsh = harshEnvelopeDb[b] * (masteringOnsetHold[b] > 0.0 ? 1.0 - (double) frameParams.transientPreserve : 1.0);   // :1738
```

包络本身有 25 / 200 ms 攻击释放，但 `appliedHarsh` 在起音保持开始与 55 ms 后结束的那一 hop 各跳一次 (1 − p) 倍：h = 1、p = 0.5、包络 3 dB 时是 ±1.5 dB 台阶，随后只受每 hop 1 dB 限速与 64 样本系数台阶约束。3.3 的鼓点脉冲显示真实起音让 `P > 1.7·P_prev` 每秒触发 9 ～ 10 次（4 次真实起音各带 2 ～ 3 个重叠帧），所以在「持续刺耳 + 鼓点」的素材上，2 ～ 8 kHz 的削减会以 5 ～ 10 Hz 在 50 % ↔ 100 % 之间来回——这是可闻的颤音式调制，不是「保留起音」。R2-01 修后，纯鼓点素材上包络本身接近 0，本条只剩真刺耳素材。

**修法**：把 (1 − p) 只放进包络的目标（`:1730` 已有 `onsetPermit`），删掉 `:1738` 的第二次硬乘，让 25 ms 攻击 / 200 ms 释放去塑形——起音到来时目标瞬间降到 (1 − p) 倍，包络以「攻击」速度（25 ms）跟下去，比现在的硬切慢约两个 hop，仍然快于任何鼓的 decay；或给 `onsetPermit` 单独一个 5 ～ 10 ms 一阶平滑。**验证**：现有「transient-preserve changes burst attack samples」用例保留；新增断言：`getHarshReductionDb()` 相邻 hop 差 ≤ 0.6 dB（现在可到 1.5）。

## 3. 实验数据（107，numpy 2.5.3，脚本见附录 A）

### 3.1 E1 · 滤波器组串扰矩阵（原文）

```
==== fs = 48000 ====
串扰矩阵 M（单段 +1 dB 时其它段中心处的 dB），以 1 kHz 段（j=14）为例：
  距离 -4 段（    397 Hz）: +0.012 dB
  距离 -3 段（    500 Hz）: +0.023 dB
  距离 -2 段（    630 Hz）: +0.055 dB
  距离 -1 段（    794 Hz）: +0.197 dB
  距离 +0 段（   1000 Hz）: +1.000 dB
  距离 +1 段（   1260 Hz）: +0.197 dB
  距离 +2 段（   1587 Hz）: +0.055 dB
  距离 +3 段（   2000 Hz）: +0.023 dB
  距离 +4 段（   2520 Hz）: +0.012 dB
行和（全段同时 +1 dB 时各段中心实现增益）: min 1.090 中位 1.592 max 1.608
矩阵条件数 cond(M) = 2.31
单段 1 kHz +6 dB：1 kHz 实现 +6.000，邻段 800 Hz +1.228，1.25 kHz +1.225，630 Hz +0.353，1.6 kHz +0.351
单段 1 kHz −8 dB：1 kHz 实现 -8.000，邻段 800 Hz -1.681，1.25 kHz -1.678
3 段连片 -6 dB：1 kHz 中心实现 -8.453 dB（目标 -6，偏差 -2.453），1 kHz 段边界处 -6.598/-6.591，连片最外段中心 -7.579，连片外第 1 段 -1.731
5 段连片 -6 dB：1 kHz 中心实现 -9.156 dB（目标 -6，偏差 -3.156），1 kHz 段边界处 -7.432/-7.422，连片最外段中心 -7.807，连片外第 1 段 -1.851
5 段连片 -8 dB：1 kHz 中心实现 -12.344 dB（目标 -8，偏差 -4.344），1 kHz 段边界处 -9.995/-9.982，连片最外段中心 -10.494，连片外第 1 段 -2.556
5 段连片 +6 dB：1 kHz 中心实现 +9.156 dB（目标 +6，偏差 +3.156），1 kHz 段边界处 +7.432/+7.422，连片最外段中心 +7.807，连片外第 1 段 +1.851
9 段连片 +3 dB：1 kHz 中心实现 +4.740 dB（目标 +3，偏差 +1.740），1 kHz 段边界处 +3.916/+3.909，连片最外段中心 +3.916，连片外第 1 段 +0.919
全 27 段 +3 dB：1 kHz 实现 +4.834，40 Hz +3.925，16 kHz +3.275
Brighten 满档倾斜目标 40 Hz -4.21 / 16 kHz +3.61（跨度 7.83）→ 实现 -5.36 / +3.89（跨度 9.25）
-- 修法核对：g' = solve(M, g_target)，再按真实 RBJ 响应算实现值 --
  1 段 +6 dB：预失真后各段中心实现误差 max|err| = 0.055 dB，预失真增益范围 [-1.19, +6.48]
  3 段 -6 dB：预失真后各段中心实现误差 max|err| = 0.057 dB，预失真增益范围 [-5.21, +1.32]
  5 段 -8 dB：预失真后各段中心实现误差 max|err| = 0.124 dB，预失真增益范围 [-6.85, +1.81]
  5 段 +6 dB：预失真后各段中心实现误差 max|err| = 0.051 dB，预失真增益范围 [-1.36, +5.14]
  5 段 −8 dB 迭代 2 次后 max|err| = 0.0001 dB，预失真范围 [-6.84, +1.89]
==== fs = 192000 ====（矩阵与各例数字与 48 k 相差 < 0.02 dB；16 kHz 端因无双线性压缩略高：全段 +3 → 16 kHz +3.872，Brighten 跨度 9.87）
```

补充（E5 尾）：Brighten 满档目标 100 Hz −3.00 / 10 kHz +3.00（跨度 6.00）→ 27 段级联实现 −4.76 / +3.89（跨度 8.65；文档粉噪实测 7.42）。

### 3.2 E2 · 刺耳判据的时间常数不匹配（原文）

```
hop = 10.67 ms；刺耳段 b=17..23（2 k ～ 8 kHz）；报告 4 kHz 段（b=20，freqWeight 1）与全窗最大值
[mid ] 宽带阶跃 -6 dB：4 kHz 段 appliedHarsh 峰值 1.57 dB（h=1）/ 0.27 dB（新实例默认 h=0.175），全窗峰值 1.79，>1 dB 持续 0.39 s，>0.5 dB 持续 0.59 s，excess 峰值 2.46 dB
[mid ] 宽带阶跃 -10 dB：4 kHz 段 appliedHarsh 峰值 2.99 dB（h=1）/ 0.52 dB（新实例默认 h=0.175），全窗峰值 3.00，>1 dB 持续 0.70 s，>0.5 dB 持续 0.87 s，excess 峰值 4.84 dB
[mid ] 宽带阶跃 -20 dB：4 kHz 段 appliedHarsh 峰值 3.00 dB（h=1）/ 0.52 dB（新实例默认 h=0.175），全窗峰值 3.00，>1 dB 持续 1.06 s，>0.5 dB 持续 1.21 s，excess 峰值 9.82 dB
[mid ] 宽带阶跃 +10 dB：4 kHz 段 appliedHarsh 峰值 0.000 dB
[slow] 宽带阶跃 -6 dB：4 kHz 段 appliedHarsh 峰值 2.16 dB（h=1）/ 0.38 dB，全窗峰值 2.31，>1 dB 持续 0.77 s，>0.5 dB 持续 1.03 s，excess 峰值 3.35 dB
[slow] 宽带阶跃 -10 dB：4 kHz 段 appliedHarsh 峰值 3.00 dB（h=1）/ 0.52 dB，全窗峰值 3.00，>1 dB 持续 1.24 s，>0.5 dB 持续 1.50 s，excess 峰值 6.49 dB
[slow] 宽带阶跃 -20 dB：4 kHz 段 appliedHarsh 峰值 3.00 dB（h=1）/ 0.52 dB，全窗峰值 3.00，>1 dB 持续 1.89 s，>0.5 dB 持续 2.13 s，excess 峰值 13.98 dB
[slow] 宽带阶跃 +10 dB：4 kHz 段 appliedHarsh 峰值 0.000 dB
[fast] 宽带阶跃 -6 dB：4 kHz 段 appliedHarsh 峰值 0.26 dB（h=1）/ 0.05 dB，全窗峰值 0.56，>1 dB 持续 0.00 s，>0.5 dB 持续 0.00 s，excess 峰值 0.44 dB
[fast] 宽带阶跃 -10 dB：4 kHz 段 appliedHarsh 峰值 0.68 dB（h=1）/ 0.12 dB，全窗峰值 1.28，>1 dB 持续 0.00 s，>0.5 dB 持续 0.21 s，excess 峰值 1.08 dB
[fast] 宽带阶跃 -20 dB：4 kHz 段 appliedHarsh 峰值 0.63 dB（h=1）/ 0.11 dB，全窗峰值 2.15，>1 dB 持续 0.00 s，>0.5 dB 持续 0.27 s，excess 峰值 1.07 dB
[fast] 宽带阶跃 +10 dB：4 kHz 段 appliedHarsh 峰值 0.000 dB

-- 鼓点式宽带脉冲：周期 250 ms，前 50 ms −12 dBFS、其余 −30 dBFS（全段同步，理想平谱） --
[mid ] p=0.5 h=1：4 kHz 段 appliedHarsh 均值 1.84 dB，最大 2.91，击打期均值 1.24，间隙均值 2.00；>1 dB 时间占比 100 %；默认 h=0.175 时均值 0.32 dB
[mid ] p=0.0 h=1：4 kHz 段 appliedHarsh 均值 2.15 dB，最大 2.91，击打期均值 2.49，间隙均值 2.06；>1 dB 时间占比 100 %；默认 h=0.175 时均值 0.38 dB
[mid ] 修法（本段也用 quickLevelDb）：均值 0.0000 dB，最大 0.0000 dB
[slow] p=0.5 h=1：均值 1.81 dB，最大 2.90，击打期均值 1.24，间隙均值 1.96；>1 dB 时间占比 100 %；默认 h=0.175 时均值 0.32 dB
[slow] 修法：均值 0.0000 dB，最大 0.0000 dB
[fast] p=0.5 h=1：均值 0.65 dB，最大 0.89，击打期均值 0.38，间隙均值 0.72；>1 dB 时间占比 0 %；默认 h=0.175 时均值 0.11 dB
[fast] 修法：均值 0.0000 dB，最大 0.0000 dB

-- 对照：真正的持续局部峰（4 kHz 段比邻段高 6 dB，电平不变），两种公式稳态一致 --
稳态 harsh（h=1）：code 公式 2.925 dB，quick 公式 2.925 dB（相同）
```

### 3.3 E3 · 起音判据在平稳噪声上的误触发率（原文，核对为「非缺陷」）

```
== 白噪（30 s，两声道不相关）==
  段 17（  2000 Hz，21 bin）：ratio>1.7 每秒  0.60 次，ratio>4.0 每秒  0.00 次，10log10(ratio) 标准差 0.95 dB
  段 18（  2520 Hz，26 bin）：ratio>1.7 每秒  0.37 次，ratio>4.0 每秒  0.00 次，10log10(ratio) 标准差 0.85 dB
  段 19（  3175 Hz，32 bin）：ratio>1.7 每秒  0.17 次，ratio>4.0 每秒  0.00 次，10log10(ratio) 标准差 0.78 dB
  段 20（  4000 Hz，41 bin）：ratio>1.7 每秒  0.03 次，ratio>4.0 每秒  0.00 次，10log10(ratio) 标准差 0.69 dB
  段 21 ～ 23：0.00 次/s，标准差 0.62 / 0.55 / 0.48 dB
  段 17：起音保持窗口覆盖时间占比 3.8 %；段 20：0.2 %；段 23：0.0 %
== 粉噪（30 s，两声道不相关）==
  段 17：ratio>1.7 每秒 0.90 次；段 18：0.17；段 19：0.03；段 20 ～ 23：0.00；起音保持覆盖 段 17 5.8 %，段 20 / 23 0.0 %
== 粉噪鼓点式脉冲（4 次/s 真实起音）==
  段 17：ratio>1.7 每秒 10.08 次（真实起音 4 次/s）；段 20：9.42 次；段 23：9.25 次
```

### 3.4 E4 · 参考学习（8192 点非重叠）与实时测量（2048 点 hop 512）的口径差（原文）

```
== 粉噪 ==
  去中心后 live − reference（b=3..25）: +0.36 +0.07 +0.09 +0.05 +0.09 -0.17 -0.04 -0.01 +0.03 -0.05 -0.04 -0.02 -0.05 -0.05 -0.01 -0.05 -0.05 -0.04 -0.04 -0.01 -0.02 -0.02 -0.02
  最大偏差 +0.36 dB @ 段 3（79 Hz）；|偏差| > 1 dB 的段数 0 → 参考模式下只因口径差就产生的目标修正（t=1）：最大 -0.00 dB
== 粉噪 + 55 Hz 正弦 −10 dBFS ==
  去中心后 live − reference（b=3..25）: +17.68 +3.65 -0.64 -0.93 -0.92 -1.18 -1.05 -1.02 -0.98 -1.06 -1.05 -1.03 -1.06 -1.06 -1.02 -1.06 -1.06 -1.05 -1.05 -1.02 -1.03 -1.03 -1.03
  最大偏差 +17.68 dB @ 段 3（79 Hz）；|偏差| > 1 dB 的段数 19 → 目标修正（t=1）：最大 -2.50 dB @ 段 3
  段 0..6 live dBFS:  -16.5  -15.5  -15.9  -17.1  -31.4  -35.7  -35.9
  段 0..6 ref  rel :  -23.9   -2.7   -4.2  -26.2  -26.4  -26.5  -26.4
== 粉噪 + 41 Hz 正弦 −10 dBFS ==
  去中心后 live − reference: +11.42 +0.03 -0.39 -0.46 …（其余 −0.44 ～ −0.70）；最大 +11.42 dB @ 段 3 → 目标修正 -2.50 dB @ 段 3
  段 0..6 live dBFS:  -16.1  -15.1  -19.4  -24.1  -35.5  -36.0  -35.9
  段 0..6 ref  rel :   -1.6   -7.9  -26.1  -26.5  -26.5  -26.6  -26.4
== 粉噪经 80 Hz 4 阶高通（自然滚降） ==   最大偏差 -0.51 dB @ 段 3；目标修正 0
== 粉噪 + 90 Hz 以下 2 阶低通份额 +12 dB（低音很重） ==   最大偏差 +0.86 dB @ 段 6（157 Hz）；目标修正 0
== 粉噪 + 3 kHz 正弦 −15 dBFS ==   最大偏差 +0.36 dB @ 段 3；目标修正 0
```

### 3.5 E5 · 低频泄漏换算成 Tame 的稳态决策（原文）

```
== 粉噪 −20 dBFS + 55.0 Hz 正弦 -10 dBFS ==
  段  中心   L(2048)  L(8192)  Δ     D(2048)  D(8192)  G_tame(2048) G_tame(8192)
   0     39    -16.5    -33.5 +16.9    -0.39   -10.04      +0.00        +0.00
   1     50    -15.5    -12.3  -3.2    +1.79   +10.64      -1.57        -8.00
   2     62    -15.9    -13.8  -2.0    +3.65   +11.13      -5.30        -8.00
   3     79    -17.1    -35.7 +18.6    +5.71    -7.06      -8.00        +0.00
   4     99    -31.4    -36.0  +4.7    -3.94    -4.35      +0.00        +0.00
   5    125    -35.7    -36.0  +0.3    -4.28    -1.38      +0.00        +0.00
  → 只因 2048 点分析泄漏而多削的量：80 Hz 段 -8.00 dB，63 Hz 段 +2.70 dB（漏削）
== 粉噪 −20 dBFS + 55.0 Hz 正弦 -20 dBFS ==
   1     50    -25.1    -22.2  -3.0    +1.32    +6.63      -0.64        -8.00
   2     62    -25.5    -23.6  -1.9    +2.22    +6.28      -2.44        -8.00
   3     79    -26.6    -36.0  +9.4    +2.93    -4.05      -3.87        +0.00
== 粉噪 −20 dBFS + 41.2 Hz 正弦 -10 dBFS ==
   0     39    -16.1    -11.2  -4.9    +1.14    +9.32      -0.28        -8.00
   1     50    -15.1    -17.5  +2.4    +4.17    +7.09      -6.34        -8.00
   2     62    -19.4    -36.0 +16.6    +2.89    -6.98      -3.79        +0.00
   3     79    -24.1    -36.0 +11.9    +1.82    -3.87      -1.64        +0.00
== 粉噪 −20 dBFS + 48.0 Hz 正弦 -14 dBFS ==
   1     50    -18.8    -14.5  -4.3    +2.93   +11.74      -3.86        -8.00
   2     62    -21.4    -35.1 +13.8    +2.68    -5.37      -3.36        +0.00
   3     79    -23.8    -36.0 +12.2    +3.18    -3.65      -4.35        +0.00
```

## 4. 落地顺序建议（今晚）

1. R2-01（一行）+ R2-05（一个常数，逐位不变）+ R2-06（逐位不变）→ 一次提交，跑全套 + 逐位摘要用例。
2. R2-02（矩阵预失真）单独一个提交，附新测试；它会改变所有 Balance 参与的声音，要提醒 GLAZE 校准与 ADR-016 门槛复跑。
3. R2-03、R2-04 一起设计（参考学习跟随实时口径；低段长窗），需要一份 ADR（帧快照契约是否切换低段口径要 R1 / R3 会签）。
4. R2-07、R2-08 择日。

## 附录 A · 实验脚本（在 107 上跑，已按要求删除；这里保留以便复现）

### A.1 e1_crosstalk.py

```python
import numpy as np
NB = 27; Q = np.sqrt(2 ** (1 / 3)) / (2 ** (1 / 3) - 1)
centres = 1000.0 * 2.0 ** ((np.arange(NB) - 14) / 3.0)
def coeffs(fc, g, fs):
    w0 = 2 * np.pi * min(fc, 0.45 * fs) / fs; alpha = np.sin(w0) / (2 * Q); A = 10 ** (g / 40); a0 = 1 + alpha / A
    return (1 + alpha * A) / a0, -2 * np.cos(w0) / a0, (1 - alpha * A) / a0, -2 * np.cos(w0) / a0, (1 - alpha / A) / a0
def resp_db(fc, g, f, fs):
    b0, b1, b2, a1, a2 = coeffs(fc, g, fs); z = np.exp(-1j * 2 * np.pi * np.asarray(f) / fs)
    return 20 * np.log10(np.abs((b0 + b1 * z + b2 * z ** 2) / (1 + a1 * z + a2 * z ** 2)))
def realised(gains, f, fs):
    return sum(resp_db(centres[b], gains[b], f, fs) for b in range(NB) if gains[b] != 0.0)
for fs in (48000.0, 192000.0):
    M = np.array([[resp_db(centres[j], 1.0, centres[i], fs) for j in range(NB)] for i in range(NB)])
    # 行和、cond、单段 / 连片 / 全段 / 倾斜各例：见第 3.1 节输出；修法核对：gp = np.linalg.solve(M, g) 再 realised(gp, centres, fs)
```

### A.2 e2_harsh_mismatch.py（核心循环）

```python
# 每 hop：P = 10^(L/10) 全段相同；avg = P + (avg − P)·pre[b]（pre = exp(−hop / (dof/(4·effBW))))；lev = 10log10(avg)
# fast = lev + (fast − lev)·(att if lev > fast else rel)；qp = P + (qp − P)·exp(−hop/0.06)；ql = 10log10(qp)
# rising = P[b] > 1.7·prevP[b]；hold = 0.055 if rising else max(0, hold − hop)
# own = fast[b]（源码）或 ql[b]（修法）；excess = max(0, own − 0.5·(ql[b−3] + ql[b+2]) − 1.5)
# harsh = min(3, 0.65·excess)·h·fw·((1−p) if hold>0 else 1)；env = harsh + (env − harsh)·(exp(−hop/0.025) if harsh > env else exp(−hop/0.2))
# applied = env·((1−p) if hold>0 else 1)
```

### A.3 e3_onset_rate.py / A.4 e4_reference_bias.py / A.5 e5_lowband_leak.py（共用的段测量）

```python
def make_bands(N):   # 与 BalanceModule.cpp:308-325 / ReferenceSpectrum.cpp:186-202 相同
    binHz = fs / N
    for b in range(27):
        low, high = centres[b] / 2**(1/6), centres[b] * 2**(1/6)
        first = max(1, int(np.floor(low / binHz - 0.5)) + 1); last = min(N // 2, int(np.ceil(high / binHz + 0.5)) - 1)
        ks = np.arange(first, last + 1)
        w = np.maximum(0, np.minimum(high, (ks + 0.5) * binHz) - np.maximum(low, (ks - 0.5) * binHz)) / binHz
# 实时：周期 Hann 2048、hop 512、两声道 |X|² 平均 × 2/(N·Σw²)，段功率长期均值 → dB
# 参考：周期 Hann 8192、非重叠、两声道 |X|² 之和（Nyquist × 0.5）、Σ_{k≥1} 为 total，10log10(Σband / Σtotal)
# E5 稳态：smoothed = [1,2,3,4,3,2,1]/16 核（门权重全 1），D = L − smoothed，E = max(0, D − 1)，G = −min(E + min(E, E), 8)
# 粉噪：Paul Kellet 7 极点（与 tests/BalanceTests.cpp 同一组系数）
```
