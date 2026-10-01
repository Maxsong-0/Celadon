# R1 · Limit 限幅链深度审计（LimiterModule / TruePeak / HalfBandOversampler / Saturator::SoftClip / ReleaseSafety）

日期 2026-09-29 · 基线 main `e6b445b`（v0.1.8.5）· 只读审计，本机未编译未测试；数值实验在 107（Ubuntu，AMD EPYC 74F3，GCC 15.2，python 3.14 + numpy 2.5.3 / scipy 1.18.1）上做，实验目录 `~/.cache/celadon-t032/R1/` 已删。

## 0. 一句话结论

**Limit 链没有 P0 正确性缺陷。** 前瞻 / 保持段 / 检测器延迟 / 两级攻击整形的对齐数学逐样本核实精确（64 个抽头样本恰好拿到同一增益）；释放包络、短过载松开、低频周期释放、Clip 空闲跳过、半带过采样器的缓冲边界、ReleaseSafety 的 TPDF 抖动、实时安全与 NaN 处理都核过，未发现算法错误。可落地的是 1 条 P1（x86 真峰值核有 3× 的逐位相同提速）和 5 条 P2（滑动最小值算法、一个潜在静音陷阱、检测器精度分解、采样率倍数归一、Clip 阈值与 0.35 dB 余量不一致）。

## 1. 范围与方法

- 先读档案：`设计规格-Limit.md`、ADR-004、ADR-013、BUG-001、BUG-002、`docs/reference-chain-algorithm.md`（Limit 段）、`docs/design/0.1.9-mastering-controls.md`、`docs/validation/2026-09-28-mastering-controls/REPORT.md`；再逐行读 `Source/dsp/{LimiterModule,TruePeak,HalfBandOversampler,Saturator,ReleaseSafety}.{h,cpp}`、`DelayLine.h`、`CeladonChain.cpp`（Limit / ReleaseSafety 调用段）、`PluginProcessor.cpp`（LookaheadHold、延迟上报）、`tests/{LimiterTests,DspPartsTests,AdvancedMasteringTests}.cpp` 与 `TestHelpers.h` 的 16× 参考实现；JUCE 9.0.2 的 `SmoothedValue` 与 `designFIRLowpassHalfBandEquirippleMethod` 源码。
- 三组数值实验（第 4 节）：E1 真峰值检测器对近理想带限重建的低读分解与候选设计对比；E2 前瞻对齐逐样本核实；E5 独立 C++ 微基准（TruePeak SSE2 核 vs 无 FMA 的 AVX2 核 vs 标量参照、SlidingMinimum 三种实现）。
- 不重报（任务第 5 条）：Clay 预算、55 Hz 纯 Side 474 % 尖峰、mastering-bench 96k/32 最大值、Color NEON 融合乘加、界面主线程占用、鼓组密度、AAC/MP3 编码实测、AU 版本号、BUG-001～004。

## 2. 已核实无缺陷的项（有证据的「阴性结论」）

| # | 项 | 证据 |
|---|---|---|
| A | **前瞻对齐**：检测器延迟 32（`TruePeak.h:44`）、`holdSamples = 63`（`LimiterModule.h:74`）、滑动最小值窗 L + 63（`LimiterModule.cpp:322`）、音频延迟 L + 62（`.h:182`）、两级滑动平均 L₁ + L₂ − 1 = L（`.cpp:978-987`）。E2 逐样本模拟：对 L = 44 / 48 / 96 / 97 / 480，需求 r[m+32] 一个样本的衰减恰好落在输入 m−31 … m+32 这 **64 个样本**上（= 插值器窗口），斜坡在 m−(L+30) 起、m+(L+31) 止；释放在峰值最后一个抽头出去的下一个样本开始，没有多余保持。 |
| B | **释放包络**：向下瞬时、向上一阶（`.cpp:84-91`），double 计算并用 `settle` 落回精确 1.0（`.cpp:79-82`，`.h:222-223`），Auto / 手动切换从当前包络接续（`.cpp:427-433`）；`gain returns exactly to 1.0` 用例覆盖。 |
| C | **短过载松开**（`.cpp:1006-1028`）：窗口结束那一拍 envSlow == envFast，之后 slow 以自己的释放接着走，`min(fast, slow)` 连续无跳变；连续压满 70 ms 取消（`:1012-1013`）。 |
| D | **低频周期释放**（`.cpp:892-976`）：150 Hz 两级一阶、过零周期、3 周期一致性、120 ms 持续门、能量占比门都按采样率归一（`:240-244, :896, :964`）；double 状态每 32 样本冲零（`:952-959`）；只读 1× 控制路不加延迟。 |
| E | **Clip 空闲跳过**（`.cpp:541-601, :757-814`）：空闲条件含「阈值不在滑动」；历史环长 = 预滚长 = 256 ≥ 过采样器记忆 121（`:197-199` 有 assert）；预滚用空闲期恒定阈值（`:578, :786`），与一直在跑逐位相同的推理成立；重置状态自洽（`:287-295`）。 |
| F | **HalfBandOversampler 边界**：每段缓冲按 4 补齐再加 16 余量（`HalfBandOversampler.cpp:56, :314-324`），核最多多读 / 多写 3 + n 个（`:157-171, :186-200`），下采样原地写回自己的输出缓冲无别名冲突（`:467-484`）；`getMemoryInSamples` 推导（`:513-523`）与滤波器跨度一致。 |
| G | **ReleaseSafety**（`ReleaseSafety.cpp:54-121`）：TPDF = 两个 24 位均匀差（±1 LSB），每声道独立 xorshift32，double 里加抖动再 `std::round`，钳到 [−1, 1 − LSB]；16 / 24 位网格值都能被 float 精确表示；关闭且增益归一时在取写指针前返回（逐位旁通）；无分配、无锁。链里放在两块表与 Listen 之后（`CeladonChain.cpp:294-296`）。 |
| H | **实时安全**：`process` / `setParams` 无分配、无锁、无系统调用；表读数走 `atomicMax` / `exchange`（`AtomicHelpers.h:17-25`，`.cpp:377-380`）；`ScopedNoDenormals` 在处理器（`PluginProcessor.cpp:571`）。 |
| I | **NaN / Inf**：检测器取大忽略 NaN 候选（`TruePeak.cpp:132-139`）；Inf 峰 → required = 0 → 输出 0 或 NaN → 链置零重启（`CeladonChain.cpp:256-263`）；`no NaN / Inf for extreme input` 用例覆盖 1e20。 |
| J | **延迟上报**：`pendingLatency` 在处理器层写、30 Hz 消息线程定时器上报（`PluginProcessor.cpp:494-500`）；`LookaheadHold` 状态机（`:434-489`）在同规格重启、宿主 Reset、快自动化三种情形下核对无漏洞；`getLatencySamples` 与限幅器换前瞻同块生效。 |
| K | 硬钳位用的 Ceiling 比检测用的 Ceiling 老 32 个样本（`.cpp:830, :866`，延迟 L+62 对 L+30）：20 ms 乘法平滑下差 < 0.1 dB，被 0.35 dB 检测余量覆盖，钳位不会误触发。 |
| L | 8 相 × 64 抽头 Kaiser β = 9 的插值相在 0.45 fs 以内平坦（E1(a)：0.000 dB），优于 BS.1770-4 附录 2 的 4×/48 抽头；表与限幅器同一检测器（`LoudnessMeter.cpp:220, :236`）。 |

## 3. 发现（按 优先级 × 置信度 排序）

### R1-01 · x86 真峰值核只有 SSE2：不用 FMA 的 AVX2 核快 3.0× 且逐位相同（P1 · medium · 置信 0.9）

- **文件**：`Source/dsp/TruePeak.cpp:233`（`computePeakGroups` SSE2 版）；`:38-42` 平台选择只有 NEON / SSE2 / 标量。
- **证据**：`:232` 注释「x86-64 只有 16 个 XMM 寄存器：一次 4 个输出 × 7 相」；BUG-001 实测 SSE2 29.30 ns / 样本 / 声道，NEON 9.88 ns。E5（107，`g++ -O3 -ffp-contract=off`，两次 -O3 一次 -O2）：标量参照 219～227 ns，**SSE2 核 28.80～28.95 ns，AVX2（`_mm256_mul_ps` + `_mm256_add_ps`，不用 `fmadd`）9.62～9.83 ns**；480 000 个输出（含 1e-39 非规格化与 3.0 大值）与标量参照 **0 失配**。链里三个检测器（限幅 + IN / OUT 表，`LoudnessMeter.cpp:220, :236`）都走这个核。
- **机理**：SSE2 版每个 64 抽头循环喂 7 个 4 路累加器，受发射 / 延迟限制；8 路向量用同样 7 个累加器覆盖 8 个输出，每条 lane 仍是「先乘后加、各自舍入」，所以与标量参照、SSE2 核逐位相同（ADR-013 的前提照旧成立）。
- **修法**：新增 `computePeakGroupsAVX2`：每 8 个输出一组，`x = _mm256_loadu_ps(window + k)`，7 个 `_mm256_broadcast_ss(tapsByTap + 8k + p)`，`a_p = _mm256_add_ps(a_p, _mm256_mul_ps(x, t_p))`，取大写 `_mm256_max_ps(candidate, peak)`（操作数顺序与 SSE2 版一致，DAZ 语义相同）；`numGroups × 12` 个输出先按 8 个一组算，余 4 个用现有 SSE2 quad（两者逐位相同，`line` / `peakScratch` 现有余量够用：末组最多读到 `line[302] < 315`）。运行时按 `juce::SystemStats::hasAVX2()` 分派；GCC / clang 用 `__attribute__((target("avx2")))` 或单独 TU 加 `-mavx2 -ffp-contract=off`；MSVC 单独 TU 加 `/arch:AVX2` 并保留 `#pragma fp_contract(off)`。**禁止**换成 `_mm256_fmadd_ps`（那就不再与参照逐位相同）。HalfBandOversampler 与 Saturator 的 4 路 SSE2 核同理可做 8 路无 FMA 版。
- **改后输出**：逐位不变（changes_output = false）。
- **验证**：`DspPartsTests` 「processBlock / processBlockMax are bit-exact with processSample」扩到 AVX2 核（4 种 MXCSR × 9 种信号 × 7 种分块）；性能护栏用例报核名 "AVX2"；由主进程在 107 跑全套，Windows 版按 ADR-019 在 MSVC 上编并跑。
- **收益估算**：x86 三个检测器 48 / 96 / 192 kHz 立体声 ≈ 0.83 % / 1.7 % / 3.3 % 一个核 → 0.28 % / 0.56 % / 1.1 %。
- **工作量** M。

### R1-02 · SlidingMinimum 每样本 2～3 次整数取模 + 数据相关循环：van Herk 块算法快 3.4～4.3×、逐位相同（P2 · low · 置信 0.9）

- **文件**：`Source/dsp/LimiterModule.cpp:109-138`（`push`）。
- **证据**：`:116` `(head + count - 1) % capacity`、`:124` `(head + count) % capacity`、`:132` `head = (head + 1) % capacity`；capacity = maxLookahead + 64（`:97, :223`）不是 2 的幂。E5（W = 159 = 2 ms @ 48 k + 63）：类增益输入（70 % 为 1.0）**9.5～10.7 ns / 样本**，掩码版 5.8 ns，**van Herk 2.8 ns**；随机输入 13.0～13.6 ns 对 2.8～3.1 ns；两种替代各 4 M 样本 **0 失配**。
- **机理**：整数除法 20～40 周期；两个 while 循环在噪声般的增益上分支预测差。van Herk / Gil-Werman：当前块前缀最小值 1 次 min，上一块后缀最小值每 W 样本算一次（均摊 1 次 min），合并 1 次 min，无分支，与窗长无关。
- **修法**：`struct SlidingMinimum { std::vector<float> block, suffix; int W, pos; float prefix; }`：`push(v)`：`block[pos] = v; prefix = pos ? min(prefix, v) : v; r = min(prefix, suffix[pos + 1]); if (++pos == W) { suffix[W] = 1; for i = W−1…0: suffix[i] = min(suffix[i+1], block[i]); pos = 0; } return r;`。初始 suffix 全 1.0 = 「无穷多个 1.0 的历史」，与现在 `clear()` 后的行为相同；`setWindow` 后 `clear()` 照旧。float `min` 对相等值对称，与现在弹出相等元素的语义等价（实测 0 失配）。
- **改后输出**：逐位不变。
- **验证**：现有分块无关、精确回 1.0、null、16× 用例；新增随机单元测试：窗长在 1 … maxLookahead + 63 间随机切换并与暴力 `std::min_element` 对照。
- **工作量** S。

### R1-03 · `ceilingDelay` 重置时填 0 而非当前 Ceiling：今天被延迟线同时清零掩盖，是潜在静音陷阱（P2 · low · 置信 0.85）

- **文件**：`Source/dsp/LimiterModule.cpp:318`。
- **证据**：`:317-318` `ceilingDelay.setDelay(coreDelay); ceilingDelay.clear();`，`DelayLine.h:33-36` `clear()` = `fill(0)`；同一函数里 `gainDelay` 却是 `fill(gainSmoothed.getCurrentValue())` 并有注释解释（`:319-320`，`reset()` 里 `:269` 再填一次）；`:866` `ceilingAtOutput = ceilingDelay.pushPop(ceiling)`，`:872` `jlimit(−ceilingAtOutput, ceilingAtOutput, delayed × gain)` → 每次 `resetLimiterState` 之后前 coreDelay 个样本的钳位是 ±0，输出被强制为 0。
- **机理**：今天无害只是因为 `coreDelays` 在同一处清零（`:315`）、`restartWithFadeIn` 又静音整个延迟长度（`:303-309`）；任何把「换前瞻不清音频延迟线」或改重置顺序的后续改动都会在这里静默丢掉 coreDelay 个样本，测试只会看到「多静音一段」。
- **修法**：`resetLimiterState()` 里 `ceilingDelay.fill(ceilingSmoothed.getCurrentValue())`；`reset()` 里在 `ceilingSmoothed.setCurrentAndTargetValue(ceiling)` 之后再 `ceilingDelay.fill(ceiling)`（与 gainDelay 同样的两段式，因为 `:262` 的 resetLimiterState 先于 `:270` 的平滑器落位）。
- **改后输出**：逐位不变（0 × g 钳到 ±0 与钳到 ±c 都是 +0）。
- **验证**：现有 restartWithFadeIn / lookahead change / reset == prepare 用例保持逐位；新增用例在测试构建里把 `fadeInHoldSamples` 置 0 后紧接重置送 +6 dB 阶跃，前 coreDelay 个输出应钳在 ±Ceiling 而不是 0。
- **工作量** S。

### R1-04 · 检测器低读分解：带限素材由 8× 相位密度主导（≤ 0.076 dB，抛物线顶点修正 → 0.004 dB），近奈奎斯特杂散由 64 抽头过渡带主导（≤ 0.39 dB）；固定 0.35 dB 余量对前者过保守、对后者不保证（P2 · low · 置信 0.75）

- **文件**：`Source/dsp/TruePeak.cpp:142-159`（`directFormPeak` 只在 8 个离散点取大）；`LimiterModule.cpp:59`（固定 0.9605）。
- **证据**（E1，64× FFT 带限重建为理想值，正 = 检测器读低）：
  - (a) 插值相幅频响应（最小相）：0.45 fs 0.000 dB，0.46 −0.017，0.47 −0.300，0.48 −1.671，0.49 −6.13；8× 密度最坏 20·log10 cos(πf/8)：0.40 fs −0.108，0.45 −0.136。
  - (c) 逐样本、±32 样本保持窗内取大（贴近限幅器实际用法）：噪声 ≤ 0.45 fs：当前 max 0.076 dB，**+抛物线 0.004**；≤ 0.40 fs：0.057 → 0.002；4× 软削波粉噪：0.136 → 0.118（滤波器主导）；20 kHz @ 44.1 k 门控正弦：0.396 → 0.348（滤波器主导）；全带白噪：1.07（任何有限余量都盖不住，也不是 DAC 会重建的内容）。
  - (b) 分段最大：8×/128 抽头把 20 kHz 门控正弦从 0.394 压到 0.194、硬削波粉噪 0.180 → 0.112、4× 软削波 0.108 → 0.068，代价 2× 乘加与 +32 样本延迟；把截止从 fs/2 上移（cutoff 1.02 / 1.04）在 0.5 fs 附近抬 3～5 dB，对 4× 软削波反而更差（0.108 → 0.162 / 0.270），**不可取**。
  - 测试自己的参考（`tests/TestHelpers.h:196`，16×/128 β = 10）对同一理想值也读低：4× 软削波 0.065 dB、全带白噪 0.68 dB，所以 `tests/LimiterTests.cpp:732` 网格的「≤ Ceiling + 0.01」相对理想重建约有 0.06 dB 的乐观。
- **机理**：8 个子样本时真峰值离最近评估点最远 1/16 样本；带限峰局部是抛物线，经过最大的三个相邻 |值| 的抛物线顶点几乎无偏（p50 −0.000 dB）；过渡带（0.455～0.545 fs）则只能靠更长的窗压窄。
- **修法（不建议今晚动）**：(a) 在 8× 流上做顶点修正：`am, a0, ap` 为相邻三点 |值|（跨样本边界取 n−1 的第 7 相与 n+1 的第 0 相），`denom = am − 2a0 + ap < 0 && |δ| ≤ 0.5` 时 `peak = max(a0, a0 − (am − ap)·δ/4)`，每样本约 10 flop，向量核可按输出并行；(b) `tapsPerPhase` 64 → 128（延迟 64、holdSamples 127、coreDelay +32）。任一项做完都要按 ADR-004 的 540 组矩阵重标 `detectorSafetyGain`，并把重标的参考换成 FFT 带限插值（对有限信号精确）而不是 16×/128。
- **改后输出**：变（检测读数与限幅量都变）。
- **验证**：重跑 `LimiterTests.cpp:657` 与 `:732` 两个网格、DspParts 逐位矩阵；对比重标前后 GLAZE 0 手动链的响度差。
- **工作量** M。

### R1-05 · 8× 真峰值插值与 4× Clip 过采样不随采样率归一：96 k / 192 k 下做 2× / 4× 的无效工作（P2 · low · 置信 0.7）

- **文件**：`Source/dsp/TruePeak.h:42`（`oversamplingFactor = 8` 常量）；`LimiterModule.cpp:187-188`（两级半带固定，不看 `spec.sampleRate`）。
- **证据**：密度误差只与 f / fs 有关（E1(a) 表）：48 k 下 20 kHz 在 0.42 fs、8× 给 −0.12 dB；96 k 下同一 20 kHz 在 0.21 fs，**4× 就是 −0.12 dB，8× 是 −0.03 dB**；192 k 下 2× 即等价。三个检测器成本随 fs 线性：NEON 48 k 0.12 %（规格）→ 192 k 约 0.48 %；x86 SSE2 按 28.9 ns 算 48 / 96 / 192 k ≈ 0.83 / 1.7 / 3.3 %。Clip 级 4× 在 192 k 内部跑 768 kHz。
- **机理**：设计按 48 k 的 0.45 fs 音频带宽定倍数；高采样率下可听带只占 0.21 / 0.10 fs，同样精度只需一半 / 四分之一的相；Clip 级谐波在 192 k 基线奈奎斯特已是 96 kHz。
- **修法**：`TruePeakDetector::prepare(numChannels, sampleRate)`：fs < 88.2 k 用 8 相，< 176.4 k 用 4 相，否则 2 相，`tapsPerPhase` 仍 64（延迟 32、holdSamples 63 不变，对齐数学不动）；核里未用的相把 `tapsByTap` 置 0 并跳过对应累加器（或另写 3 相 / 1 相核）。Clip 级在 ≥ 88.2 k 只留第 1 级半带（延迟由 60 变为该级单独的值，`getLatencySamples` 与 218 / 378 的测试常数随之改）。
- **改后输出**：≥ 88.2 kHz 时变；44.1 / 48 k 逐位不变。
- **验证**：96 k / 192 k 下重跑真峰值 ≤ Ceiling + 0.01、16× 参考、延迟 == 上报、分块无关；DspParts 按相数分别做逐位矩阵。
- **工作量** M。

### R1-06 · Clip 阈值没随 0.35 dB 检测余量收紧：Clip = 100 时限幅器对每个削波峰都再压 ≥ 0.35 dB，与规格「限幅器只兜底」不符（P2 · low · 置信 0.6）

- **文件**：`Source/dsp/LimiterModule.cpp:351-352`（`reset()` 里 `:271-272` 同样）。
- **证据**：阈值 = `decibelsToGain(jmap(clip, 0, 100, +12, params.ceilingDb))`，即 Clip = 100 时 T = 用户 Ceiling；而需求增益用 `ceiling × 0.9605`（`:839-840`，`:59`）。规格「处理流程 2」写「Clip = 100 时阈值 = Ceiling，削波器承担大部分峰值控制，限幅器只兜底」，Codex 修订 1 又写「用户 Ceiling、Clip 阈值与输出硬钳位保持原值」——两句在 0.35 dB 余量加入后互相矛盾。E1 显示 4× 软削波素材的 8× 读数与理想只差 ≤ 0.11 dB，所以削波器封顶在 T 的每一段都以 ≥ 0.35 dB 超过检测上限，限幅器必然介入。
- **机理**：削波器把峰整形到 T，下采样后回长 δ（0.1～0.5 dB），限幅器再按 0.35 + δ dB 骑增益（30 ms 快释放）；`getAndResetGainReductionDb`、`BlockStats.grMaxDb / grMeanDb`（`CeladonChain.cpp:274-278` 送 GLAZE）都带这 0.35 dB 常量偏置。听感上接近一个准静态的 0.35 dB 衰减，影响小；问题是设计意图失效与 GR 读数偏置。
- **修法**：把 Clip 终点映射到检测上限：`jmap(params.clip, 0.0f, 100.0f, clipOffThresholdDb, params.ceilingDb + detectorSafetyDb)`，`constexpr float detectorSafetyDb = −0.35f`（与 `detectorSafetyGain` 同源，两处：`:271-272`、`:351-352`）；硬钳位仍用用户 Ceiling。输出真峰值不变（都是 Ceiling − 0.35），变的只是「谁在干活」。
- **改后输出**：Clip > 0 时变；Clip = 0 路径与 null 用例不受影响。
- **验证**：新用例：0 dBFS 100 Hz 方波、Gain +6、Clip 100、Auto → 修后 `getAndResetGainReductionDb()` 只剩下采样回长（< 0.5 dB），修前 ≥ 0.35 + 回长；`true peak ≤ ceiling + 0.01` 网格、空闲跳过逐位、分块无关照跑。是否采纳属产品决定（Max 拍板）。
- **工作量** S。

## 4. 实验数据

### E1 · 真峰值检测器（107，numpy，`e1_truepeak.py`，5 s）

方法：按 `TruePeak.cpp:283-340` 复现 8 相 × 64 抽头 Kaiser β = 9、各相直流归一、第 0 相 = 原样本；理想值 = 信号两端补 256 个 0 后 FFT 零填充 64× 的带限重建；候选设计：cutoff 1.02 / 1.04、β 7、8×/96、8×/128、16×/64、16×/128 β 10（= 测试参考）、当前 + 抛物线顶点。信号各 102 400 样本：全带白噪；白噪低通 0.45 / 0.40 / 0.35 fs；硬削波粉噪（±0.7）；4× 软削波粉噪（tanh，再低通抽取）；稀疏脉冲低通 0.45；100 Hz 方波 @48 k；18 kHz @48 k 与 20 kHz @44.1 k 的 3 ms 门控正弦。

(a) 插值相最小幅频响应（dB）：

| 设计 | 0.40 | 0.45 | 0.46 | 0.47 | 0.48 | 0.49 |
|---|---|---|---|---|---|---|
| 当前 8×/64 β9 | 0.000 | 0.000 | −0.017 | −0.300 | −1.671 | −6.132 |
| 8×/96 β9 | 0.000 | 0.000 | 0.000 | 0.000 | −0.300 | −3.285 |
| 8×/128 β9 | 0.000 | 0.000 | 0.000 | 0.000 | −0.017 | −1.671 |
| 16×/128 β10（测试参考） | 0.000 | 0.000 | 0.000 | 0.000 | −0.044 | −1.937 |
| 8× 密度最坏 cos(πf/8) | −0.108 | −0.136 | −0.142 | −0.149 | −0.155 | −0.162 |

直流归一与否响应差 < 0.001 dB（D0 vs D0n 全表一致）。

(b) 每 256 样本段的低读（理想 / 检测，dB；p99 / max）：

| 信号 | 当前 | +抛物线 | 8×/128 | 16×/128 β10 |
|---|---|---|---|---|
| 白噪全带 | 0.647 / 0.980 | 0.622 / 0.973 | 0.463 / 0.705 | 0.483 / 0.677 |
| 噪声 ≤ 0.45 fs | 0.060 / 0.067 | 0.002 / 0.003 | 0.060 / 0.067 | 0.013 / 0.017 |
| 硬削波粉噪 | 0.147 / 0.180 | 0.140 / 0.178 | 0.104 / 0.112 | 0.100 / 0.116 |
| 4× 软削波粉噪 | 0.075 / 0.108 | 0.066 / 0.108 | 0.062 / 0.068 | 0.052 / 0.065 |
| 稀疏脉冲 ≤ 0.45 | 0.041 / 0.197 | 0.001 / 0.197 | 0.041 / 0.197 | 0.010 / 0.050 |
| 100 Hz 方波 | 0.025 | 0.005 | 0.020 | 0.006 |
| 18 kHz @48 k 门控 | 0.042 / 0.043 | 0.042 / 0.043 | 0.011 / 0.012 | 0.013 / 0.013 |
| 20 kHz @44.1 k 门控 | 0.384 / 0.394 | 0.338 / 0.339 | 0.185 / 0.194 | 0.161 / 0.165 |

注：分段统计有边界伪影（一段末尾区间的理想峰被检测器归到下一段第 0 相），0.35 fs 噪声那行的「0.36 dB」即此，已排查不是低读，故不列。(c) 逐样本 ±32 保持窗统计见 R1-04。

### E2 · 前瞻对齐（107，`e2_align.py`）

单个需求样本 r[m+32] = 0.5，滑动最小值窗 L+63，包络瞬时跟随，两级滑动平均 L₁ = L − ⌊L/2⌋ + 1、L₂ = ⌊L/2⌋，音频延迟 L+62：

| L | L₁ / L₂ | 拿到完整衰减的输入样本（相对 m） | 斜坡起 / 止 |
|---|---|---|---|
| 44 | 23 / 22 | −31 … +32（64 个） | −74 / +75 |
| 48 | 25 / 24 | −31 … +32（64 个） | −78 / +79 |
| 96 | 49 / 48 | −31 … +32（64 个） | −126 / +127 |
| 97 | 50 / 48 | −31 … +32（64 个） | −127 / +128 |
| 480 | 241 / 240 | −31 … +32（64 个） | −510 / +511 |

（L = 1920 那行脚本 cumsum 的浮点容差给出 70 个，属脚本精度，不是代码问题。）

### E5 · 微基准（107，`e5_bench.cpp`，`g++ -O3 -std=c++17 -ffp-contract=off`，三次运行）

| 项 | 结果 |
|---|---|
| TruePeak 标量参照（先乘后加、maxss） | 219.5 / 221.4 / 227.0 ns / 样本 |
| TruePeak SSE2 核（照 `TruePeak.cpp:233-271`） | 28.95 / 28.82 / 28.80 ns（BUG-001 记录 29.30，复现可信） |
| TruePeak AVX2 无 FMA 核 | 9.62 / 9.83 / 9.64 ns；对标量参照 0 失配 / 480 000 |
| SlidingMinimum 取模（`LimiterModule.cpp:109-138`）| 类增益输入 10.65 / 9.54 / 9.53 ns；随机输入 12.98 / 13.03 / 13.56 ns |
| SlidingMinimum 2 的幂掩码 | 5.80 / 5.80 / 5.93 ns，0 失配 / 4 M |
| SlidingMinimum van Herk | 2.82 / 2.83 / 2.80 ns（随机 3.06 / 3.08 / 2.82），0 失配 / 4 M |

## 5. 未列为发现、但值得知道的观察

- 释放在线性增益域做一阶（`.cpp:84-91`）：深衰减在 dB 域前快后慢，与 dB 域释放的限幅器手感不同；是设计选择，不是缺陷。
- `Saturator::softClip` 逐样本 `std::tanh`（`Saturator.h:70-80`，头注释明确「v0.5 起保持逐位不变」）：Clip 重时约 8 × 15 ns / 立体声帧，换 `fastTanh` 会改输出，本次不建议。
- `LimiterModule::setParams` 每块 5 次 `pow` + 1 次 `exp`（`.cpp:345-361`）：48 k / 64 块约 0.02 %，不值得改。
- 两个 Celadon 实例的抖动随机序列种子相同（`ReleaseSafety.h:46-49`）：并行抖动会相关，正常用法只有末级一个实例抖动，不报。

## 6. 复现

实验脚本在本会话 scratchpad（`e1_truepeak.py`、`e2_align.py`、`e5_bench.cpp`），方法与参数已在第 4 节写全，可按描述重写；服务器目录 `~/.cache/celadon-t032/R1/` 已删除，`~/.cache/celadon-t032/venv/` 为各代理共用未动。
