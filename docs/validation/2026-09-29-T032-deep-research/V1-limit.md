# V1 · Limit 限幅链反驳核验（对 R1-limit 六条结论逐条判决）

日期 2026-09-29 · 基线 main `e6b445b`（v0.1.8.5）· 只读核验；本机未编译未测试。数值复核在 107（AMD EPYC 74F3，GCC 15.2，`-O3 -ffp-contract=off`；numpy 2.5）`~/.cache/celadon-t032/V1/` 做，做完已删，脚本留在本会话 scratchpad（`v1_truepeak.cpp`、`v1_slidingmin.cpp`、`v1_sse2pre.cpp`、`v1_e1a.py`、`v1_e1b2.py`）。

## 0. 一句话结论

六条里 **3 条成立、3 条不成立**。成立的都是「输出逐位不变」的优化 / 卫生项：R1-01（AVX2 无 FMA 核，我独立实现后 4 种 MXCSR × 9 种信号 × 172.8 万个输出 0 失配、3.04×）、R1-02（van Herk 滑动最小值，120 万样本对现有实现与暴力参照都 0 失配、3.4～5×，但绝对收益只有约 0.03 % 一个核）、R1-03（`ceilingDelay` 清零与 `gainDelay` 不对称，今天无害，修法逐位不变——但研究给的验证用例查不出这个修改）。不成立的三条都是 **ADR-004 / 规格 Codex 修订已明文接受的取舍**，而且修法都会改输出：R1-04（0.35 dB 余量的组成，ADR-004「口径补充」已写明宽带 / 高频素材读低 0.1～0.66 dB 是既有性质）、R1-05（按采样率减相——实测 4 相在削波素材上最坏读低从 0.41 dB 恶化到 1.03 dB，与 8× 的选型理由正相反）、R1-06（Clip 阈值不随余量收紧——规格修订 1、`LimiterModule.h:21`、`GlazeConstants.h:197` 三处明文「保持原值」，而且 GLAZE 的 Clip 模型常数是按现映射拟合的）。

另外发现研究漏掉的一个零风险替代：**SSE2 核只把系数表预先广播（去掉 7 条 shufps）就快 1.56×、逐位相同、不需要运行时分派与构建改动**（107 实测 28.11 → 17.99 ns）。

## 1. 逐条判决

### R1-01 · x86 真峰值核只有 SSE2，AVX2（无 FMA）核 3× 且逐位相同 —— **confirmed**（medium · P1 · 修法可用，需补三件事）

**代码证据**：`TruePeak.cpp:38-42` 平台宏只分 NEON / SSE2 / 标量；`:232-271` SSE2 核每个抽头做 2 次 `_mm_loadu_ps` + 7 次 `broadcast<lane>`（`_mm_shuffle_ps`）+ 7 乘 + 7 加；`:135` 标量取大是 `_mm_max_ss (candidate, peak)`、`:223` 向量是 `_mm_max_ps (candidate, peak)`。三个检测器共用：`LimiterModule.cpp:823`、`LoudnessMeter.cpp:220 / :236`。x86 版**已在售**：INDEX「当前版本」写 v0.1.8.5 有 Windows / Mac Intel 安装包，`.jucer` `xcodeValidArchs="arm64,x86_64"`，T-025 已合 main。

**独立复现**（107，`v1_truepeak.cpp`：系数生成、参照、SSE2 核逐行照抄源码；AVX2 核按 R1-01 写法：8 路 `_mm256_mul_ps` + `_mm256_add_ps`、`_mm256_broadcast_ss (tapsByTap + 8k + p)`、`_mm256_max_ps (candidate, peak)`，余 4 个用 SSE2 quad；反汇编 0 条 `vfmadd`）：

| 项 | 结果 |
|---|---|
| 逐位对照（IEEE / FTZ / DAZ / FTZ+DAZ × 白噪、0.45 fs 正弦、近非规格化、最小非规格化、含 NaN / Inf / ±3 的极值、稀疏脉冲、方波、直流、1e-20 噪声，各 48000 样本，按 240 长的段 + 63 历史与 `processChunk` 同布局） | 1 728 000 个输出：参照 vs SSE2 **0**、参照 vs AVX2 **0**、SSE2 vs AVX2 **0** |
| 标量参照 / SSE2 / AVX2（ns / 样本，best of 15） | 203.1 / 27.88 / **9.18** → SSE2/AVX2 = **3.04×** |

与研究的 E5（28.8 / 9.6～9.8 ns）一致；BUG-001 记录的 29.30 ns 也复现。`vmaxps` 的 DAZ 语义与 `maxps` 相同（DAZ 单开、非规格化信号 0 失配），BUG-001 的坑没有再踩。边界：`line` 长 63 + 240 + 12 = 315，8 路末组最远读到 `line[244 + 7 + 63] = line[314]`，够。

**修法评估**：方向正确、逐位契约（ADR-013）成立。但研究的「验证」只写了「扩到 AVX2 核」，缺三件事：① **运行时分派必须带测试钩子**（如 `setBlockKernelForTesting`），否则 DspPartsTests 只测机器上被选中的那条路径，SSE2 回退路径从此没人测；`getBlockKernelName()` 要能报 "AVX2"，性能护栏（`DspPartsTests.cpp:516-518`）加一档；② **三条工具链的落地方式**：GCC / clang 可用 `__attribute__((target ("avx2")))`（我的复现即此，全局 `-ffp-contract=off` 照样生效），MSVC 不支持函数级 target，只能单独 TU 加 `/arch:AVX2`——`.jucer` 是插件唯一真源（ADR-003），要用 Projucer 的 compiler-flag scheme 给该文件加旗标，CMake 用 `set_source_files_properties`，两边同步；建议三种工具链统一用「单独 TU」，少一种写法；③ **测试矩阵**：107（GCC，有 AVX2）、109（真 MSVC + Wine，需确认 Wine 暴露 AVX2）、Mac 的 x86_64 切片走 Rosetta——macOS 15 起 Rosetta 支持 AVX2（会真的跑到新核），更早系统 `hasAVX2()` 为假走 SSE2，两种都要过。`juce::SystemStats::hasAVX2()` 在 JUCE 9.0.2 存在（`juce_SystemStats.h:242`）。

**严重度 / 优先级**：medium / P1 可以维持（x86 已发货、三个检测器、192 k 下差 2.2 个百分点），但它是纯性能项，今晚做与否取决于上面三件事的工作量；**若只想零风险拿一半收益，先做下面「漏项 (a)」的 SSE2 预广播表**。

### R1-02 · SlidingMinimum 取模 + 数据相关循环，van Herk 逐位相同且 3.4～4.3× —— **confirmed**（low · P2 · 修法可用，收益很小）

**代码证据**：`LimiterModule.cpp:116 / :124 / :132` 三处 `% capacity`，`capacity = maxLookahead + 64`（`:97`、`:223`）非 2 的幂；`:114-122`、`:130-134` 两个 while。`required ≤ 1.0` 恒成立（`:840`），所以 van Herk 用 1.0 当「无穷历史」与 `clear()` 后的行为等价。

**独立复现**（107，`v1_slidingmin.cpp`）：60 次随机窗长（1 … 543）× 20000 样本、增益状与随机两种输入：现有实现 vs van Herk **0 失配**，两者 vs 暴力 `min` **0 失配**（1 200 000 样本）。基准 W = 159：增益状输入 9.36 → 2.76 ns（3.4×），随机 13.68 → 2.76 ns（5.0×）。

**评估**：算法正确、逐位相同、修法 S。但要说清收益：控制路每样本只调一次，48 k 下省 (9.4 − 2.8) ns × 48 000 ≈ 0.32 ms/s = **0.03 % 一个核**，192 k 也只有 0.13 %；van Herk 每 W 个样本有一次 W 次 min 的小突发（≤ 543 次，< 1 μs），无实时风险。折中方案（capacity 取 2 的幂用掩码）研究测得 5.8 ns，一行改动。P2 合适，「顺手改」级别，不值得单独立项；改了必须加研究说的随机窗长 vs `std::min_element` 用例。

### R1-03 · `ceilingDelay` 重置填 0 而非当前 Ceiling —— **confirmed（潜在，今天无害）**（low · P2 · 修法逐位不变；研究给的验证用例无效）

**代码证据**：`resetLimiterState()` `:317-318` `ceilingDelay.setDelay (coreDelay); ceilingDelay.clear();`（`DelayLine.h:33-36` clear = fill (0)），同一函数 `:319-320` `gainDelay.fill (gainSmoothed.getCurrentValue())` 并在 `reset()` `:269` 二次填；`:866` `ceilingAtOutput = ceilingDelay.pushPop (ceiling)`，`:872` `jlimit (−ceilingAtOutput, ceilingAtOutput, delayed × gain)`。`ceilingDelay` 全文只有 `:214`（prepare）、`:317-318`、`:866` 三处，清零只发生在 `resetLimiterState`，而 `:315` 同处把 `coreDelays` 也清零、长度相同——前 coreDelay 个输出 `delayed = 0`，钳到 ±0 与钳到 ±c 都是 +0，所以今天逐位无差。研究说的两个掩盖条件属实。

**修法评估**：`resetLimiterState()` 里 `fill (ceilingSmoothed.getCurrentValue())`、`reset()` 里 `:270` 之后再 `fill (ceiling)`——与 gainDelay 同构，输出逐位不变（首次 prepare 时 `SmoothedValue<…, Multiplicative>` 默认初值 1.0，`juce_SmoothedValue.h:246`，随后 reset 二次填覆盖）。**但研究的验证用例查不出这个修改**：「fadeInHoldSamples 置 0 后紧接重置送 +6 dB 阶跃，前 coreDelay 个输出应钳在 ±Ceiling」——重置同时清空了 `coreDelays`，前 coreDelay 个输出的 `delayed` 是 0，乘增益后无论钳到 ±0 还是 ±Ceiling 都是 0，修前修后一样。这条只能靠代码评审 + 现有用例（restartWithFadeIn `LimiterTests.cpp:1013`、lookahead change `:940`、reset == fresh prepare `:639`、分块无关 `:917`）保持逐位来兜底，不存在黑盒可测的行为差。严重度 low 合适，本质是代码卫生。

### R1-04 · 检测器低读分解，固定 0.35 dB 余量「对带限过保守、对近奈奎斯特不保证」 —— **refuted（不是缺陷，是 ADR-004 已记录的既有性质；分析本身正确）**（low · P2 · 不动）

**数值复核**：`v1_e1a.py` 逐字重现 E1(a)：8×/64 β9 各相最小响应 0.40 / 0.45 / 0.46 / 0.47 / 0.48 / 0.49 fs = 0.000 / 0.000 / −0.017 / −0.300 / −1.671 / −6.132 dB，密度界 cos(πf/8) = −0.108 … −0.162 dB，与研究表格逐位一致。`v1_e1b2.py`（按群延迟对齐 + ±32 样本滑动最大后逐样本比较，对研究说的段边界伪影免疫）：8×/64 相对测试用的 16×/128 β10 参考，全带白噪 p99 / max 读低 0.502 / 0.674 dB，4× 软削波粉噪 0.075 / 0.411 dB——与 ADR-004「口径补充」写的 0.1～0.66 dB 吻合。

**为什么不算缺陷**：ADR-004「后果 · 口径补充（2026-09-25 v0.6 验收 F-04）」原文：宽带 / 高频瞬态素材上 16× 参考比插件 8× 表高 0.1～0.66 dB，「是 8 相 × 64 抽头检测在这类素材上低读的既有性质。验收口径以插件自己的表为准；交付 0 dBTP 时留余量」；规格 Codex 修订 1、`LimiterModule.h:20-21`、`LimiterModule.cpp:56-58` 都写「经验余量，不保证任意波形」。研究的结论与档案一致，只是量化得更细，且自己也写「不建议今晚动」。修法（顶点修正 / 128 抽头）都改输出、要按 ADR-004 的 540 组重标、还要换参考——不是今晚的事。**建议的处置**：把 E1 的分解（密度 vs 过渡带各占多少、顶点修正的收益、16× 参考自身对理想值读低 0.065 / 0.68 dB）写进 `03-记忆/` 作领域知识或 ADR-004 补充，将来真要收余量时先做顶点修正。

### R1-05 · 8× 检测 / 4× Clip 不随采样率归一 —— **refuted（设计选择；修法反向破坏 8× 的选型理由）**（low · P2 · 不做）

**代码证据属实**：`TruePeak.h:42` 常量 8、`LimiterModule.cpp:187-188` 两级固定。但「高采样率下可听带只占 0.21 / 0.10 fs，减相精度不变」的前提不成立：限幅器与两块表量的是**信号本身**的真峰值，不是可听带；96 k 下 Clip 级自己的谐波延伸到 48 kHz，16× 参考用例（`LimiterTests.cpp:732`，覆盖 44.1 / 48 / 96 k 的白噪与 Clip 50）量的也是全带。

**数值复核**（`v1_e1b2.py`，同上方法，单位是 f/fs 所以与采样率无关）：

| 素材 | 8×/64（现） | 4×/64 | 2×/64 |
|---|---|---|---|
| 全带白噪 p99 / max 读低（dB，相对 16×/128 参考） | 0.502 / 0.674 | 0.614 / 0.895 | 0.873 / 1.725 |
| 4× 软削波粉噪 | 0.075 / **0.411** | 0.117 / **1.033** | 0.338 / 1.974 |

减到 4 相后削波素材的最坏读低从 0.41 dB 变成 1.03 dB——正是 ADR-004 修订里「Clip 削出来的高频尖峰让 4× 读低」而选 8×/64 的那类素材。0.35 dB 余量是按 8× 标的；96 k 用 4 相很可能让 `:732` 用例的 96 k 白噪 / Clip 组超过 +0.01 dB（是预测，未跑 JUCE 全套），至少要按采样率分别重标余量。Clip 级只留第 1 级半带在延迟上可行（半带长 4n+3，单级 1× 延迟 = nU + nD + 1 恒为整数），但同样改输出、改 96 k 延迟常数（ChainTests `:1754` 的 378）。收益方面 NEON 192 k 三个检测器约 0.5 %，x86 走 R1-01 后 1.1 %——不值得为它改一条按 ADR-004 定型的检测精度。结论：不是缺陷；若要省高采样率 CPU，只能在保持 8 相的前提下做（R1-01）。

### R1-06 · Clip 阈值没随 0.35 dB 余量收紧 —— **refuted（三处明文的既定取舍；是文档不一致，不是代码缺陷）**（low · P2 · 只改文档）

**代码证据属实**：`LimiterModule.cpp:271-272 / :351-352` 阈值 = jmap (clip, 0, 100, +12, ceilingDb)，Clip = 100 时 T = 用户 Ceiling；`:839-840` 需求增益用 ceiling × 0.9605；SoftClip 渐近封顶在 T（`Saturator.h:70-80`），所以 Clip = 100 时限幅器必然再压 ≥ 0.35 dB。`LimiterTests.cpp:971-972` 甚至把这 0.35 dB 写进 GR 表的期望值（13.35 dB）。

**为什么不算缺陷**：这是 Codex 修订 1 的明文决定——规格「2026-09-26 Codex 修订」第 1 条「用户 Ceiling、Clip 阈值与输出硬钳位保持原值」，`LimiterModule.h:21`、`LimiterModule.cpp:58` 原句相同。与「处理流程 2」的「限幅器只兜底」矛盾的是**规格旧句没跟着改**，属文档问题。更重要的是研究漏了 **GLAZE 的依赖**：`GlazeConstants.h:197-209` 把「阈值 dB = jmap (Clip, 0 → +12, 100 → Ceiling)」写成【映射】常量，`clipActivityCoeff / clipActivityExponent / clipLossFloor` 是按这套映射在 Clip 0 / 60 / 80 / 90 / 100 上实测拟合的【实测】常数；把终点移到 Ceiling − 0.35 dB 会让全部 Clip > 0 的输出变、GLAZE 的 Clip 活动 / 深度模型失准，还牵动 ADR-016 的同决策回放对照与 T-027 缓存的参考条件。研究自己也说「是否采纳属产品决定」。**建议处置**：今晚只改规格「处理流程 2」那句，写成与修订 1 一致（Clip = 100 时阈值 = 用户 Ceiling；限幅器按 Ceiling − 0.35 dB 兜底，GR 表含这 0.35 dB）；要改映射得 Max 拍板，并连同 GLAZE 常数重拟合一起立项。

## 2. 研究漏掉的项（missed）

- **(a) SSE2 零风险替代**：把 `tapsByTap` 改成每抽头 7 × 4 个相同 float 的预广播表（7 KB），SSE2 核每抽头 7 次 `_mm_load_ps` 代替 2 次 load + 7 次 `shufps`，运算序列不变。107 实测（`v1_sse2pre.cpp`）28.11 → **17.99 ns（1.56×）、0 失配**。不需要运行时分派、不改 `.jucer` / CMake、不引入新 ISA，可作为今晚的保底，也可作为 AVX2 加入后的非 AVX2 回退路径。
- **(b) R1-01 的测试钩子**：运行时按 `hasAVX2()` 分派后，逐位矩阵必须能在同一台机器上强制走每条核；研究的验证方案没写，缺了它 SSE2 回退路径就成了没人测的死角。
- **(c) R1-03 的验证用例无效**（见上）：修改没有黑盒可观测差异，只能靠评审与现有用例逐位。
- **(d) R1-06 修法对 GLAZE 常数的连锁**（见上）：`GlazeConstants.h:197-209`。
- **(e) R1-05 前提错误**：把「可听带」当作检测目标；削波素材 4 相最坏读低 1.03 dB。
- **(f) 未发现研究之外的 P0 / P1 正确性缺陷**：我另核了 `processLimiterCore` 的对齐链（held 窗 L+63、音频延迟 L+62、两级平均 L₁+L₂−1 = L，推得完整衰减恰覆盖输入 m−31 … m+32 共 64 个样本，与 E2 一致）、`SlidingMinimum` 容量（window ≤ maxWindow < capacity）、`configureAttackShape` 在 L = 1 / 480 的边界、Clip 空闲跳过的声道数条件（`:546`、`:763`）、stats 的 SSE2 比较 / 取大对 NaN 的处理（`:726-727`）——都没问题。研究第 2 节的 12 条阴性结论未逐条重验，但与我读到的代码没有冲突。

## 3. 建议今晚的落地清单（只列输出逐位不变的）

1. **R1-01 或漏项 (a)**：有精力做完整 AVX2（单独 TU + 分派 + 测试钩子 + 107 / 109 / Rosetta 三处全套）就做 R1-01；否则先做 (a)，一样逐位、1.56×。
2. **R1-03**：两处 `fill`，附现有用例逐位。
3. **R1-02**：可选，S 工作量，必须带随机窗长 vs 暴力对照用例。
4. R1-04 / R1-05 / R1-06：不动代码；R1-06 改规格一句；R1-04 的分析归档进 `03-记忆/`。

## 4. 复核产物

- 107：`~/.cache/celadon-t032/V1/` 已删。编译命令 `g++ -O3 -std=c++17 -ffp-contract=off`，AVX2 核用 `__attribute__((target ("avx2")))`，`objdump` 确认 0 条 FMA。
- 本会话 scratchpad：`v1_truepeak.cpp`（AVX2 逐位 + 基准）、`v1_slidingmin.cpp`（van Herk 逐位 + 基准）、`v1_sse2pre.cpp`（SSE2 预广播表）、`v1_e1a.py`（E1(a) 重现）、`v1_e1b2.py`（8× / 4× / 2× 相对 16× 参考的读低）。
