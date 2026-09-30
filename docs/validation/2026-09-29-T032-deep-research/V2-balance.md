# V2 · Balance 动态频谱平衡 · 对 R2 的反驳核验

日期 2026-09-29 22:35 PDT（107 上 2026-09-30 05:35 UTC）。核验代理 V2，只读；基线 main `e6b445b`。核对对象：`docs/validation/2026-09-29-T032-deep-research/R2-balance.md` 的 8 条结论。

## 0. 方法与总判

- 逐条重读 `Source/dsp/BalanceModule.cpp` 的相关行（常量 :40-160、prepare :253-345、processChunk :563-621、designPeak / updateCoefficients :854-907、拆片 :940-990、accumulatePower :1018-1027、updateTrackers :1105-1303、谐波保护 :1305-1420、updateCorrectionTargets :1422-1763）、`ReferenceSpectrum.cpp:145-372`、`MusicAnalyzer.cpp:1699-1800`、`GlazePlanner.cpp`（对 Balance 的引用）、`CeladonChain.cpp:110-140`；测试 `tests/BalanceTests.cpp`（用例清单、:1097-1140、:1330-1393、:1001-1048、:1630-1660、:2376-2481）、`tests/AdvancedMasteringTests.cpp:640-745`、`tests/BalancePerformanceTests.cpp`（门槛清单）。
- 档案：`设计规格-Balance.md`（:23-51、:116-138）、`设计规格-高级母带控制.md`（:18-54、:55-71）、ADR-013、ADR-020、`04-协调/归档/2026-09-24-v0.3-Balance实现计划.md`（细节表）、TASKS.md（T-032 行与占用登记）、09-29 两篇日志。仓库 `docs/reference-chain-algorithm.md`、`docs/heat-and-balance-revision.md`、`docs/design/0.1.9-mastering-controls.md`。
- 数值复算：107 `~/.cache/celadon-t032/V2/`（numpy 2.5.3，已删）独立重写了 4 组实验（串扰矩阵、刺耳阶跃 / 鼓点、55 / 41 Hz 低音的 2048 对 8192 段读数与稳态 Tame、掩蔽算术），结果见第 2 节，与 R2 的数字一致到小数点后 1 ～ 2 位。

| # | 判决 | 严重度（R2 → V2） | 优先级（R2 → V2） | 修法 | 一句话 |
|---|---|---|---|---|---|
| R2-01 | **confirmed** | high → high | P0 → P0 | 可用 | 代码 :1727 确如所述；规格 §4 那行是 T-029 按 Codex 代码整理的（ADR-020 待 Max 复核），不是 Max 拍板的取舍；ADR-020「备选方案」自己写着「逐段 Welch 平滑会把电平阶跃当成倾斜」 |
| R2-02 | **confirmed**（机理） | medium → medium | P1 → P2 | 不能照搬 | 串扰真实且复算一致，但这是规格「增益 = G[b]」与代码 :86-87 注释里写明的已知性质；「GLAZE 规划器拿到偏小的数」被驳回（规划器按 Recover / Tame 不改电平计，段增益只喂界面）；预失真会让滤波器增益超出 ±8/+6 封顶（交替型目标到 −10.9/+9.7 dB），且改变所有 Balance 声音，需 ADR + 重校准 |
| R2-03 | **confirmed**（现象） | medium → low | P1 → P2 | 方向错 | 自参考 −2.5 × t 复算成立；但同一泄漏在温暖 / 均衡 / 明亮预设下同样产生 −2.5 × t（R2 没说），根治是把实时侧低段做干净（R2-04），不是把参考侧改成同样脏的 2048 点 |
| R2-04 | **confirmed** | high → high | P1 → P1 | 方向对、两处要改 | 泄漏与 Tame 决策复算一致；修法里「192k 每 4 hop 一次长 FFT」会是 32768 点落进一片、砸穿 192k/32 最坏块预算（要抽取或分摊）；「τ 不变」与代码 :327-335 的公式不符（等效带宽从 35 Hz 降到 9 Hz → τ 4 倍长或估计噪声 2 倍），要 Max 选 |
| R2-05 | **confirmed** | low → low | P1 → P2 | 可用 | 算术无误、d=4 分支无副作用、改 3 逐位不变；但「docs/reference-chain-algorithm.md 也写 1 ～ 4 段」不实（该文只写「邻段、间隙 ≤ 16 dB」），只有规格 :123 写了 |
| R2-06 | **confirmed** | low → low | P2 → P2 | 可用 | 五处全部在代码里核到；powerSpectrum 读取点全在段 bin + 3 以内；注意「逐位摘要用例」只打印摘要不断言，验证要人工比对前后日志 |
| R2-07 | **confirmed**（低效） | low → low | P2 → P2 | 可用 | 常量 :44、hop 随采样率 :260-266、拆片间距 :956、同一样本上片先于重算 :583-590 都核到；规格 :40 与 v0.3 计划表写的是「64 样本」，要一起改成时间口径；只改 ≥ 88.2k 的输出 |
| R2-08 | **confirmed**（机理）/ 可闻性 uncertain | low → low | P2 → P2 | 主修法错 | 硬乘确在 :1738；但目标下降走的是**释放** 200 ms 系数（:1734），删掉硬乘会让起音闪避慢 4 倍、违背规格 §4「已在施加的削减也立刻乘 1 − p」；只能给恢复侧加短平滑 |

## 1. 逐条判决

### R2-01 · 刺耳超出量混用 fastLevel 与 quickLevel —— confirmed · high · P0 · 修法可用

**代码证据**（与 R2 相同）：`BalanceModule.cpp:1725-1727`

```cpp
const auto left = quickLevelDb[b - 3], right = quickLevelDb[b + 2];
const auto support = juce::jmin (gateWeight[b - 3], gateWeight[b + 2]);
const auto excess = juce::jmax (0.0, fastLevelDb[b] - 0.5 * (left + right) - 1.5);
```

`fastLevelDb` 的链路：功率域 Welch 平均（:1119-1126，τ = 200 / (4·带宽)，4 kHz 段 54 ms）→ dB → 攻击 80 / 释放 400 ms（:1144-1145）；`quickLevelDb` 是全段同一个 60 ms 对称平均（:1170-1178）。两者只在稳态相等。

**不是规格取舍**：`设计规格-高级母带控制.md:47` 确实写「本段 Lfast」，但该规格是「T-029 按代码整理」（ADR-020 状态「已采纳（待 Max 复核）」，背景写明「设计只写在仓库 docs、没进档案、没问过 Max」）。ADR-020「备选方案」第 4 条对目标频谱写着「用 Balance 逐段的 Welch 平滑电平 → 不用：会把电平阶跃当成频谱倾斜」，同一原则刺耳判据没有遵守；同规格 :60 也写「测量用每段的 60 ms 快功率，不用逐段 Welch 平滑」。所以这是 Codex 实现里的不一致，不是 Max 定的设计。

**复算**（107，Mid，4 kHz 段，理想平谱）：阶跃 −6 / −10 / −20 dB → 峰值 1.57 / 2.99 / 3.00 dB（>1 dB 持续 0.39 / 0.70 / 1.06 s）；+10 dB → 0；鼓点式脉冲（250 ms 周期，50 ms −12 / 其余 −30 dBFS，p = 0.5）→ 均值 1.87、最大 2.94 dB、100 % 时间 >1 dB；改本段为 quick 后全部精确 0；真持续局部峰（+6 dB）两式稳态同为 2.925 dB。与 R2 一致。

**影响面**：新实例默认「自动」（ADR-020 第 3 条，GLAZE 35 % → h = 0.175）→ 每个 decay / 鼓点间隙把 2 ～ 8 kHz 压暗 ≤ 0.52 dB，系统性、每一拍都发生；手动模式 h = 1 → 1 ～ 3 dB。high 成立。

**修法核验**：
- 逐位契约：h = 0 时整个 mastering 块被 `if (frameParams.harshAmount > 0.0f || targetAmount > 0.0f || masteringHasTail)`（:1690）跳过、刺耳分支又受 `harshAmount > 0.0f`（:1720）守，所以 harsh = 0 逐位不变；旧工程读入刺耳「关」（ADR-020 第 3 条）→ 声音不变；GLAZE 0 → h = 0 → 中性。ADR-013 管的是路径间逐位（打包 / 标量、拆片 / 一口气、编译器），不受影响。
- 现有用例：「persistent 3.15 kHz peak」稳态相同；「transient-preserve changes burst attack samples」的 ×3 突发（+9.5 dB 全段）——现在的代码在突发**上升沿** fast 落后 → 超出量反而变小、削减变少，修后突发期间超出量不变、削减保持，未保护对保护的能量差只会更大，断言 `> 1.002` 更稳；「GLAZE zero bit-identical」h = 0 不变。（本机不跑测试，需在 107 复跑。）
- 副作用要写进验证：quick 是 60 ms，判据会对 60 ～ 200 ms 的亮瞬态（镲片）更快反应（原来 fast 的 80 ms 攻击 + Welch 拖慢了上升沿），55 ms 起音保持只盖住前 55 ms（p 默认 0.5 只放一半）。建议验证加一条「镲片式 150 ms 突发」用例看尾巴是否被削；若削，再考虑像自动 Tame 那样加持续时间门槛，但那是另一个设计决定。
- `masteringPreviousPower`（:1733）只在块启用时更新的问题属实（grep 全部读写点：:423 reset、:1722 读、:1733 写）；挪到 :1488 旁每 hop 更新对 h = 0 逐位不变（那时不读它）。
- 规格 :47 改「本段 60 ms 快电平」；ADR-020 不必新开，在「决定」里补一句即可。

### R2-02 · 27 个峰值滤波器无带间串扰补偿 —— confirmed（机理）· medium · P2 · 修法不能照搬

**代码证据**：`filterQ = sqrt(2^(1/3)) / (2^(1/3) − 1)`（:295）；`designPeak` RBJ（:854-870）；`updateCoefficients` 把插值后的 G 直接交给 designPeak（:879-885）；分析是**前馈**（`pushIntoAnalysisRing` :623 注释「分析的是本模块的输入…没有反馈回路」），所以串扰不会被回路自动抵消——机理成立。

**复算**（48 k）：单段 +1 dB → 邻段中心 +0.197、隔段 +0.055；行和 1.090 / 1.592 / 1.608，cond 2.31；单段 −8 → 邻段 −1.681；5 段 −8 → 中心 −12.344、连片外 −2.556；预失真一次求解后中心误差 ≤ 0.124 dB。全部与 R2 一致。解析核对：RBJ 的 Q 定义带宽在半增益（dB）处，Q = 4.318 时段边界（±1/6 倍频程）正好半增益、邻段中心（一个带宽外）≈ 20 %。

**反驳点**：
1. **不是未知缺陷**：规格 :39「增益 = `G[b]`」、代码 :86-87「这是各滤波器的目标；相邻峰值滤波器响应会重叠，不等于整条链的精确频响」、`docs/heat-and-balance-revision.md:26` 记录 Brighten 满档实测 7.42 dB（目标 6）并视为正常、v0.3 计划细节表按规格定 Q 与增益语义。它是「已知、写在注释里、没人评估过后果」的性质，不是误读，也不是已修。
2. **「GLAZE 规划器拿到偏小的数」不成立**：`getBandGainsDb` 只经 `CeladonChain::getBalanceBandGainsDb`（:501-505）→ `PluginProcessor` → `PluginEditor.cpp:1092-1094` → 界面 `BalanceCard`；`GlazePlanner.cpp:355` 写明「Tone / Recover / Tame 按不改变整体电平计」，规划器只设 Recover / Tame 的百分比、证据来自 MusicAnalyzer 画像，校准（ADR-016）按重放对照——串扰早已隐含在校准结果里。
3. **修法的漏洞**：M⁻¹ 放大交替型增益向量（M 对交替向量的特征值 ≈ 0.72）。复算：目标 −8 / +6 相间（梳状素材、死区 + 限速下会出现）→ 预失真后滤波器增益 [−10.87, +9.68] dB，超出 −8 / +6 封顶；「Tame 只削」在滤波器层面也不再成立（单段 −8 需邻段 +1.6 dB 的提升滤波器）。要么给滤波器增益另设钳位，要么把封顶重新定义为「段中心实现值」——都是规格改动。
4. **改变一切**：任何 Recover / Tame / Brighten > 0 的工程声音都变（ADR-020 列的约束「旧工程打开声音不变」）；试听调出的常数（Tame 8、Recover 6、掩蔽 1.25、倾斜 3 dB/十倍频……）的**有效强度**整体变小（宽偏差从 1.4 ～ 1.6 倍回到 1 倍）；GLAZE 校准缓存（T-026 / T-027）与 ADR-016 的 22 首门槛全部要复跑。这不是「今晚一个提交」的活。

**结论**：机理 confirmed，严重度 medium 保留（它让所有 dB 常数的语义随偏差宽度漂移，但没有用户反馈、且校准已隐含它），优先级降到 P2：先落 ADR 让 Max 选「G 是滤波器增益（现状，规格补一句『实现响应 ≈ 1.6 × 连片目标』）」还是「G 是段中心实现值（预失真 + 滤波器增益钳位 + 重校准）」。

### R2-03 · 参考学习 8192 点与实时 2048 点口径不同 —— confirmed（现象）· low · P2 · 修法方向错

**代码证据**：`ReferenceSpectrum::prepare` `order = 13; while (order < 17 && (1 << order) < sampleRate / 6.0) ++order;`（:150-153，48 k → 8192、bin 5.86 Hz；96 k → 16384、192 k → 32768，bin 不变）；非重叠整帧（`startAnalysisIfReady` :207-224 `input.swap (analysisInput)`）；段权重公式与 Balance 相同（:186-202）；Balance 侧 2048 × 倍数（:260-266）、目标测量读 `quickPower`（:1705、:1744）。

**复算**：粉噪 −20 + 55 Hz（幅度 0.316）：去中心后 |live − ref| 最大 17.0 dB @ 80 Hz 段 → 目标修正 −2.50 dB（t = 1）；41 Hz → 10.8 dB → −2.50 dB；纯粉噪最大 1.1 dB（8192 非重叠只有 ~70 帧的估计方差，落在 1 dB 死区边上）。与 R2 一致。

**反驳点**：
1. **框错了问题**：目标频谱的**预设模式**（温暖 / 均衡 / 明亮，新实例默认「自动 → 均衡」）把同一个 2048 点 quickPower 与理想斜率相减（:1744-1747），55 Hz 强低音下 80 Hz 段同样 +17 dB → 同样 −2.5 × t。也就是说这不是「参考与实时口径不一致」，而是 R2-04 的泄漏在目标频谱功能里的表现，预设模式没有「让两边一样脏」的选项。
2. **修法方向**：让参考学习改用 2048 点 = 把更准的一侧改成同样错，只能救「参考 = 自己」这一个用例，预设模式照旧，且参考存档要升 format、老参考作废。正确方向是 R2-04（实时侧低段用长窗）——落地后 ≤ 125 Hz 两侧都是 5.86 Hz bin 口径，≥ 160 Hz 两侧本来就一致（复算粉噪 ≤ 0.4 dB）。
3. **影响面**：只影响目标频谱功能（0.1.9.0，尚未对外发布），只有 80 Hz 段，条件是 < 65 Hz 的强低音；默认 t ≈ 0.12（自动）→ −0.3 dB，参考 / 显式模式 t = 0.35（匹配力度默认）→ −0.9 dB。medium 高估，low。

**结论**：现象 confirmed，优先级并入 R2-04（P2），不单独动 ReferenceSpectrum。可保留 R2 的「自参考零修正」用例作为 R2-04 的验收之一，但门槛应写成「≥ 160 Hz 段 |值| < 0.15 dB，≤ 125 Hz 段 < 0.5 dB」（低段估计方差本来就大）。

### R2-04 · 2048 点 Hann 主瓣宽于 40 ～ 125 Hz 各段 —— confirmed · high · P1 · 修法方向对、两处要改

**代码证据**：bin = 48000 / 2048 = 23.4 Hz（:277、:291）；比 bin 窄的段取覆盖格 × 段宽 / Δ（:308-325，注释只承诺「正弦落在段内时能量一个不漏」，没承诺「不漏出去」）；v0.3 修订（规格 :63、代码 :70-78）只处理单 bin 段的**方差**；`tests/BalanceTests.cpp:1102`「Hann 窗旁瓣到相邻段已低于 −45 dB」是 1 kHz 用例的注释，对 ≤ 125 Hz 不成立（主瓣 ±2 bin = ±47 Hz，比 40 ～ 125 Hz 各段 9 ～ 29 Hz 的带宽宽 2 ～ 5 倍）。

**复算**（稳态 R = 抹平后 Lfast，Tame 100）：

| 信号 | 80 Hz 段 L2048 / L8192 | G2048 / G8192 @ 80 Hz | 真低音段 |
|---|---|---|---|
| 粉噪 −20 + 55 Hz | −20.1 / −38.1（+18.0） | **−8.00 / 0.00** | 50 Hz −1.46 / −8.00，63 Hz −4.89 / −8.00 |
| 粉噪 −20 + 41 Hz | −27.0 / −38.3 | −0.56 / 0；63 Hz −3.10 / 0 | 40 Hz −0.25 / −8.00 |

与 R2 一致（±0.5 dB，粉噪生成器不同）。自动 Tame 也会中招：band 3 的支撑段 1、0（距离 2、3）都有泄漏能量，`quickPower ≥ bandPower × 1e-4` 与门权重都满足（:1459-1467），`persistentPeak` 成立。

**影响面**：手动 Tame 100 → 80 Hz 段 −8 dB；手动 25（AdvancedMasteringTests 用的典型值）→ −1.5 dB；每首低音重的歌都会在 71 ～ 90 Hz 掏一个洞（kick 箱体、41 Hz 低音的二次谐波），而真正的低音只压 0.3 ～ 5 dB。high 成立。同一帧快照确实喂 MusicAnalyzer（`consumeFrame` :1699-1745），但 Clay 的低频相关度是低段**组和**的 X / P（:2041-2107），单声道低音的泄漏对 X 与 P 同比例，ρ 基本不受影响——R2 对 Clay 的担心可以放下；GLAZE 画像的低段读数含泄漏属实。

**修法核验**：
1. 方向对（低段用 4 倍频率分辨率、闭环在 Balance 内、快照契约不动）。
2. **错 ①**：「长 FFT 每 4 hop 做一次、CPU 与现在持平」算的是每秒均值。拆片设计（:940-956 注释、`BalancePerformanceTests.cpp:608-650`「192k/32 最坏块 ≤ 一口气的 65 %」）管的是**最坏块**：192 k 的基础 fftSize 已是 8192，4 倍 = **32768 点**，一次落进一片就是 ~100 µs，砸穿 192k/32 的块预算。必须改成 R2 自己提的抽取变体（按 fs / 6000 抽取到 6 kHz → 1024 点，任何采样率都 ~2 µs；抗混叠 IIR 每样本 2 个双二阶 × 2 声道，只在分析路上、不加音频延迟），或者像 ReferenceSpectrum 那样把 FFT 级按样本分摊。
3. **错 ②**：「低段 τ 公式的等效带宽用新 bin 宽——τ 不变（仍由段宽决定）」与代码不符。`effectiveBandwidthHz = jmax (bandwidthHz, 1.5 × binHz)`（:329-335）：现在 ≤ 125 Hz 各段的等效带宽是 1.5 × 23.4 = **35 Hz**（比段宽 9 ～ 29 Hz 都大），τ 由它决定（40 Hz 段 1.43 s，与注释「1.4 s」一致）。换成 5.86 Hz bin 后等效带宽变成段宽本身 → 40 Hz 段 τ = 200 / (4 × 9.3) = **5.4 s**（Mid），50 Hz 段 4.3 s；若强行保留 1.4 s，自由度从 200 掉到 53，估计标准差从 ±0.43 dB 变 ±0.86 dB——这是「分辨率换来的必然代价」，要 Max 在「低段慢 4 倍」与「低段抖 2 倍」之间选，并写进规格「分析」第 1 条。
4. 长窗意味着低段的时间模糊 4 倍（170 ms 窗），对低段 τ ≥ 0.4 s 的跟踪无影响，但起音检测（自动 Tame :1454、刺耳 :1722）读的是 `bandPower`，应继续读 2048 点的原始帧，不读 `trackedPower`。
5. 顺带收益：谐波保护 `harmonicMinimumRootBin = 6`（:139-141、:1361）在 2048 点下把 < 140 Hz 的基音全部排除，低音的 2、3 次谐波永远得不到保护；低段长窗后 55 Hz 是 bin 9.4，可以进候选——但那是另一个设计改动，不在本条范围。

**结论**：confirmed、high、P1，修法要按上面 2、3 改写后再做，需要 ADR（低段口径、τ 取舍、快照契约是否跟随）。工作量 L 属实，不是今晚。

### R2-05 · 上行掩蔽距离 4 永远触发不了 —— confirmed · low · P2 · 修法可用

**代码证据**：常量 :112-120；循环 :1594-1620：d = 4 时 `buried = gap − 2 − 16 = gap − 18`，要 ≥ 0.7 需 gap ≥ 18.7，而 :1608 已 `gap > 16 → continue`。d = 4 那轮从 `band` 到 `buried` 全是局部量、无副作用，`maskingMaxDistance = 3` 逐位不变（复算：d = 1/2/3/4 需 6.7 / 10.7 / 14.7 / 18.7 dB）。

**更正**：`docs/reference-chain-algorithm.md:36-38` 只写「更高、仍有内容、间隙不超过 16 dB 的邻段」，没有「1 ～ 4 段」；写了「1 ～ 4」的只有 `设计规格-Balance.md:123`（T-029 按代码补记）。规格改「1 ～ 3 段」即可。

**优先级**：无用循环每 hop 最多 27 × 1 轮、没有输出影响，P1 高估 → P2；但它是一个常数、逐位不变，可以随 R2-01 顺手提交。验证方式：`BalanceTests.cpp:492`「upward masking lifts a buried higher neighbour…」照跑；「逐位摘要」见 R2-06 的注意。

### R2-06 · 热路径重复超越函数与无用 bin —— confirmed · low · P2 · 修法可用

逐项核到：
- `:1250` `std::pow (10.0, commonLevelDb * 0.1)` 在 27 段循环体内，`:1240` 锁定分支同；`commonLevelDb` 在第 5 步循环内不变（第 6 步重定中心在循环之后 :1279-1301）→ 提到循环前逐位相同。
- `:1705`、`:1744` 的 `10.0 * std::log10 (juce::jmax (powerFloor, quickPower[b]))` 与 `:1177` 逐字相同；`runNextAnalysisSlice`（:983-989）先 `updateTrackers` 后 `updateCorrectionTargets`，中间没人写 `quickPower` → 直接读 `quickLevelDb[b]` 逐位相同。
- `maskingLoudnessWeight (band)`（:127-137）在 :1656 每 hop 现算（只在锚定时）；`std::exp` 在 :1627、:1713-1715 每 hop 现算（`targetCoeff` 有两个取值，预算两份）。同一表达式、同一 libm → 逐位相同。
- `powerSpectrum` 的全部读取点（grep）：`measureBandPowers` :1037（段 bin）、谐波保护 :1322-1333（≤ 段末 bin + 3，且 `lastBin = fftSize/2 − 3`）；`measureBandCross` 读帧缓冲不读它。上限改成 `bandFirstBin[26] + bandNumBins[26] + 3`（48 k ≈ 770，44.1 k ≈ 838）安全；未累加的高 bin 自 prepare / reset 起恒为 0、无人读。
- 量级：R2 的「0.03 % 单核」「192 k 省 3 ～ 6 µs」合理，都是零风险小活。

**注意**：`BalanceTests.cpp:2376`「logged digests」用例**只打印**摘要（`logMessage`），断言的是「拆片 = 一口气」「翻转 R 后 G 不变」，没有黄金值。R2-05 / R2-06「逐位摘要用例逐位相同」要靠人工比对改前改后的日志摘要（或临时加黄金值），不能只看用例通过。

### R2-07 · 系数重算间隔固定 64 样本 —— confirmed（低效）· low · P2 · 修法可用（要改规格）

**代码证据**：`coefficientUpdateInterval = 64`（:44）；hop = fftSize / 4 随采样率翻倍（:260-266）；`updateCoefficients` 对增益有变的段调 `designPeak`（:879-885：1 次 pow + 5 次除法）；拆片间距 = 间隔 / 2（:956）；同一样本上「片 0 → 到点的片 → 重算」（:577-590 与注释）。192 k 下每 hop 32 次重算、修正在动时 27 段每次都变 → 每秒 81 k 次 designPeak，按 60 ns 一次约 0.5 % 单核，与 R2 的估计同量级；48 k 下 1/8 hop 的时间粒度 1.33 ms 在 192 k 变成 0.33 ms，没有可闻收益。

**规格**：`设计规格-Balance.md:40`「增益每 64 样本重算一次系数」与 v0.3 计划细节表都写「64 样本」——写时只考虑了 48 k（hop 512）。改成「每 1/8 hop（48 k 下 64 样本）」要同步改规格；不是 Max 单独拍过的数。

**修法核验**：`interval = 64 << (order − baseFftOrder)` 仍整除 hop、hop 边界仍在重算网格上（512·2^k / 64·2^k = 8）；拆片间距 = 间隔 / 2，三片仍在 hop 后 0 / ¼ / ½ 个间隔、末片与第一次用到新目标的重算同一样本且先执行 → 拆片 = 一口气仍逐位；192k/32 最坏块用例里三片仍各落一块（间距 128 > 32），比值不变；44.1 / 48 k 逐位不变、≥ 88.2 k 改变（增益台阶变粗到 1.33 ms，与 48 k 相同）。「output independent of block size at 192 kHz」用例（:1630）比较的是同一配置下不同块长，网格仍从 reset 起固定 → 照过。可用，P2。

### R2-08 · 起音保留硬乘 (1 − p) 无平滑 —— confirmed（机理）· low · P2 · 主修法错

**代码证据**：`:1728` `onsetPermit` 进目标；`:1738-1739` `appliedHarsh = harshEnvelopeDb[b] × (hold > 0 ? 1 − p : 1)` 绕过包络直接乘；`:1734` `harshCoeff = (harsh > harshEnvelopeDb[b] ? harshAttackCoeff : harshReleaseCoeff)`。

**反驳主修法**：R2 说「删掉 :1738 的硬乘，让 25 ms 攻击去塑形——起音到来时目标瞬降、包络以攻击速度跟下去」。**方向反了**：目标下降时 `harsh < env`，走的是 **释放 200 ms** 系数，55 ms 保持窗内包络只掉 24 %，起音闪避从「立刻」变成「几乎没有」。代码注释 :1736「Duck the additional reduction immediately at an onset, rather than carrying its release through the attack」与规格 §4 :50「已在施加的削减也立刻乘 1 − p，不把释放拖进起音」都写明硬乘是有意为之。现有用例「transient-preserve changes burst attack samples」（`AdvancedMasteringTests.cpp:684-708`，门槛 1.002）可能仍过，但功能被削弱 4 倍。

**机理与可闻性**：保持结束那一 hop 乘数从 (1 − p) 跳回 1 属实（h = 1、p = 0.5、包络 3 dB → ≈ 1.3 dB 台阶）；但台阶落在每 hop 目标上，后面还有每 hop 1 dB 限速（:1760-1763）与 64 样本线性插值（:879），实际增益斜率 ≤ 1 dB / 10.7 ms，不是硬切。真实鼓点每秒 9 ～ 10 次触发是 75 % 重叠帧下的正常现象（每次起音 2 ～ 3 帧连续满足 1.7 倍）。「5 ～ 10 Hz 颤音式可闻调制」没有听感证据，只能判 uncertain；R2-01 修后纯鼓点素材包络接近 0，本条只剩「持续刺耳 + 密集鼓点」。

**可用的修法**：只给**恢复侧**加平滑——保持结束后乘数用一阶 10 ～ 20 ms 从 (1 − p) 爬回 1（起音侧仍立刻），或对 `appliedHarsh` 的乘数做单独一阶平滑但攻击（降）瞬时、释放（升）10 ～ 20 ms。h = 0 逐位不变；「transient-preserve」用例照过；新增断言相邻 hop `getHarshReductionDb()` 差 ≤ 0.6 dB 可以保留。

## 2. 复算原文（107，numpy 2.5.3，脚本已删）

```
=== E1 (R2-02) RBJ peaking crosstalk, Q = 4.3185, fs 48k ===
1 kHz +1 dB -> 800 Hz 0.197, 1.25 kHz 0.197, 630 Hz 0.055; row sums min/med/max 1.090/1.592/1.608; cond 2.31
1 band -8 dB: neighbour centre -1.681 dB
5 bands -8 dB: centre -12.344 dB, first outside band -2.556 dB
  pre-distorted filter gains range [-6.85, 1.81]; realised centre error max 0.124 dB
alternating -8/+6 target: pre-distorted filter gains range [-10.87, 9.68] (caps are -8/+6)

=== E2 (R2-01) harsh excess under broadband level change, 4 kHz band, Mid ===
step -6 dB: code peak 1.57 dB (>1 dB for 0.39 s); fix (own = quick) peak 0.0000
step -10 dB: code peak 2.99 dB (>1 dB for 0.70 s); fix (own = quick) peak 0.0000
step -20 dB: code peak 3.00 dB (>1 dB for 1.06 s); fix (own = quick) peak 0.0000
step +10 dB: code peak 0.00 dB (>1 dB for 0.00 s); fix (own = quick) peak 0.0000
drum pulses (250 ms, 50 ms at -12 / rest -30): code mean 1.87 max 2.94, >1 dB 100%; fix mean 0.0000
persistent +6 dB local peak steady harsh: code 2.925, fix 2.925

=== E3 (R2-04 / R2-03) 55 Hz sine (amp 0.316) + pink noise -20 dBFS: band levels 2048/hop512 vs 8192 ===
tone 55.0 Hz:   band centre  L2048  L8192  G2048  G8192
  0  39  -19.6  -35.1  -0.00  -0.00
  1  50  -18.5  -15.4  -1.46  -8.00
  2  62  -18.9  -16.9  -4.89  -8.00
  3  79  -20.1  -38.1  -8.00  -0.00
  4  99  -33.7  -37.0  -0.00  -0.00
  5 125  -37.1  -37.4  -0.00  -0.00
  self-reference: max |live-ref| 17.02 dB at band 3; correction (t=1) at band 3: -2.50 dB
tone 41.2 Hz:
  0  39  -19.2  -14.3  -0.25  -8.00
  1  50  -18.2  -20.6  -6.03  -8.00
  2  62  -22.4  -37.9  -3.10  -0.00
  3  79  -27.0  -38.3  -0.56  -0.00
  self-reference: max |live-ref| 10.77 dB at band 3; correction (t=1) at band 3: -2.50 dB
pink only: max |live-ref| 1.12 dB; tame G2048 bands0-5 max |G| 0.00

=== E4 (R2-05) masking distance arithmetic ===
d=1 needs gap >= 6.7 dB; d=2 >= 10.7; d=3 >= 14.7; d=4 >= 18.7 (cap 16)  <- unreachable
```

复算口径与源码一致：周期 Hann、`spectrumScale = 2/(N·Σw²)`、段 → bin 按重叠宽度计权（:308-325）、Welch τ（:327-335）、攻击 / 释放（:63-66）、60 ms 快电平（:158）、RBJ（:854-870）、刺耳链（:1720-1739）、稳态 Tame `G = −min(E + min(E, E_local), 8)`（:1531-1537，t = 1）。

## 3. R2 漏掉的同类问题（missed）

1. **目标频谱的预设模式同样吃 R2-04 的泄漏**：温暖 / 均衡 / 明亮（新实例默认自动 → 均衡）用同一个 2048 点 quickPower 对理想斜率（:1744-1747），55 Hz 强低音下 80 Hz 段 −2.5 × t 与参考无关。R2-03 把它框成「参考模式自洽性」并建议把参考改成 2048 点，方向反了；根治只在实时侧（R2-04）。
2. **R2-04 修法的两个硬伤**（第 1 节已展开）：① 192 k 下 4 倍窗 = 32768 点，一片做完砸穿 192k/32 最坏块预算，必须抽取到 6 kHz 做 1024 点或按样本分摊；② `effectiveBandwidthHz = max(带宽, 1.5·binHz)`（:329-335）意味着长窗后低段 τ 从 1.4 s 变 5.4 s（或保留 τ 则自由度 200 → 53、噪声 ±0.43 → ±0.86 dB），R2 写「τ 不变」是错的，要 Max 选。
3. **谐波保护对低音基音天然失效**：`harmonicMinimumRootBin = 6`（:139-141）在 2048 点下排除 < 140 Hz 的基音，低音的 2、3 次谐波（80 ～ 420 Hz）得不到保护；这是注释里写明的取舍，不算缺陷，但 R2-04 的低段长窗落地后可以顺带解除（55 Hz = bin 9.4），应在同一 ADR 里决定。
4. **R2-01 修法的行为副作用**：quick（60 ms）比 fast（Welch + 80 ms 攻击）对 60 ～ 200 ms 的亮瞬态更快反应，55 ms 起音保持只盖前 55 ms、p 默认 0.5 只放一半——验证要加「镲片式 150 ms 突发」用例看尾巴；若可闻，再考虑像自动 Tame 的 180 ms 持续门槛（另一个设计决定）。
5. **「逐位摘要用例」不是黄金值测试**：`BalanceTests.cpp:2376` 只 `logMessage` 摘要，断言的是路径间一致；R2-05 / R2-06 / R2-07（44.1 / 48 k）的「逐位不变」要靠比对改前改后日志里的摘要，或临时加黄金值；否则「用例全过」证明不了逐位。
6. **校准缓存没有算法版本盐**：GLAZE 校准参考条件的「Mast」段只带 h / p / t / 目标形状（`设计规格-高级母带控制.md:42`），R2-01 / R2-08 改了算法后，h > 0 的旧校准仍被当成有效（ADR-016 是同决策重放对照，重放结果会变）。现在只有 0.1.9.0 本机会话受影响、可忽略，但落地时应在日志里写明「h > 0 的校准要重做」，或给哈希加算法版本。
7. **R2-05 的引用不实**：`docs/reference-chain-algorithm.md` 没写「1 ～ 4 段」；只有 `设计规格-Balance.md:123`。改规格一处即可。

## 4. 对「今晚落地」的建议顺序

1. **今晚**：R2-01（一行 + `masteringPreviousPower` 每 hop 更新 + 规格 :47）、R2-05（常数 + 规格 :123）、R2-06（五处，逐位不变）——一次提交，107 跑全套并比对「logged digests」前后摘要（R2-01 只在 h > 0 变，摘要用例 h = 0 应逐位相同）；再加 R2-01 的两条新用例（阶跃、鼓点）与镲片突发观察。
2. **今晚可选**：R2-07（改常量为成员 + 规格 :40 改时间口径），192 k 用例照跑；R2-08 只做恢复侧平滑（不删硬乘）。
3. **不在今晚**：R2-04（+ R2-03 并入）需要 ADR（低段口径、τ 取舍、快照契约、谐波根 bin）与抽取 / 分摊的实现；R2-02 需要 ADR 决定 G 的语义，任何一种选择都要重跑 GLAZE 校准与 ADR-016 门槛。
