# V7 · 对 R7（分析与计量子系统）的反驳核验

> T-032（2026-09-29）核验代理 V7，Fable 5.1。只读；行号以工作树 `e6b445b`（v0.1.8.5）为准。
> 自己核对的文件：`Source/ui/SpectrumAnalyser.{h,cpp}`、`tests/SpectrumAnalyserTests.cpp`、`Source/dsp/MusicAnalyzer.{h,cpp}`
> （子块流 979–1099、起音 1308–1380、帧流 1715–1835、证据 1912–2009、画像 2122–2325）、`tests/MusicAnalyzerTests.cpp`（用例表、1403–1461）、
> `Source/dsp/LoudnessMeter.{h,cpp}`、`Source/dsp/TruePeak.h`、`Source/AnalyzerTap.h`、`Source/dsp/CeladonChain.h`、`Source/PluginEditor.cpp`
> （984–1075）、`Source/PluginProcessor.cpp`（596–633）、`Source/dsp/GlazeController.cpp`（1197–1216、2644–2650）、
> `docs/validation/2026-09-28-T021-perf-acceptance/{modules-after-T020.log,clay-profile-top.txt,REPORT.md}`；档案：分析器规格第 2 节、
> 时间线与保存第 2 节、实现修订 F-01、界面 V4 融合版第 3 节、ADR-004、ADR-013、ADR-021、踩坑-聆听窗口不代表整首歌。
> 数值核验在 107 `~/.cache/celadon-t032/V7/`（venv numpy），逐字复刻 `SpectrumAnalyser::analyse` 与显示 / 平均线弹道，做完已删。

## 0. 判决一览

| 编号 | 判决 | 严重度（研究 → 核验） | 优先级（研究 → 核验） | 修法可用 | 一句话 |
|---|---|---|---|---|---|
| R7-01 | confirmed | medium → medium | P1 → P1 | 否（要改） | 缺陷成立且我自己复算到同样的数；但「混合估计」修法按研究自己的数就会把既有 1 kHz 正弦用例（±1 dB）打红，口径要 Max 拍板 |
| R7-02 | confirmed | medium → medium | P1 → P2 | 部分 | 因果门成立、聆听 15.1 s 提前结束成立；影响面窄（前奏比正文轻 ≥ 20 LU）、改画像与聆听时长要 Max 认可、第 1 步不治频谱证据 |
| R7-03 | uncertain | low → low | P2 → P2 | 待拍板 | CPU 数字对；「高采样率降相位」是口径决定，且 ADR-004 的 8× 理由（Clip 削出的近奈奎斯特尖峰）在 96 k 仍成立，标准附件条文本轮没核到原文 |
| R7-04 | confirmed（结构）/ uncertain（幅度） | low → low | P2 → P2 | 部分 | 两遍 800 格与 IN 表无人读的读数都属实；尖峰幅度只是估算（≈ 2～4 µs）；只建议做 ① 的「按 topBin 截断」（逐位不变），② 不值得加 API |
| R7-05 | confirmed | low → low | P2 → P2 | 是 | 每帧 86 k 次 pow 属实；预计算同一算式显示逐位不变 |
| R7-06 | confirmed | low → low | P2 → P2 | 部分 | 平均线偏高 +2～3 dB（低频）/ +0.5～0.8 dB（高频）复算属实；「峰值线改取原始测量」会让噪声类素材的峰值线明显抬高，建议峰值线照旧 |

## 1. 逐条判决

### R7-01 · 频谱仪「段内取最大」估计量 —— confirmed · medium · P1 · 修法要改

**自己找到的证据**

- `SpectrumAnalyser.cpp:174` `halfStep = 0.5·log2(1000)/479 = 0.010403 oct`；`:181–182` 点区间 `[f·2^−h, f·2^h)/binWidth`；`:185` `high − low < 1.0` 走 `:188–192` 的相邻 bin 功率线性插值；否则 `:197–201` 对 `floor(low)…ceil(high)` 取最大。
- 48 k / 8192（`:71` 的 order 13）bin 宽 5.859 Hz；`high − low = f·0.014421/5.859 = 1` 在 **406 Hz**（107 复算切换点 407.4 Hz，即第一个走 max 的点）；20 kHz 一点覆盖 **52** 个 bin（含 floor / ceil 多取的两端），H_52 = 4.54 → 期望偏置 +6.6 dB。96 k / 192 k 的 FFT 加倍（order 14 / 15），bin 宽不变，切换点不变。
- `SpectrumAnalyser.h:13` 只承诺「0 dBFS 正弦读 0 dB」；`:196` 的注释写明取最大是为了「保住谐波的尖峰；取平均会把正弦压低几个 dB」——这是有意的取舍，但只考虑了正弦，没有考虑噪声口径。`设计规格-界面-V4融合版.md:37` 只写显示斜率 0 / 3 / 4.5 dB/oct（默认 4.5），档案里（02-日志、ADR-021、04-协调）没有任何关于噪声口径或「取最大」的决定记录。
- `tests/SpectrumAnalyserTests.cpp` 只有正弦（1 kHz、−6 dBFS、48 k / 96 k）、泄漏、RMS、落下、restartOutput、频率轴五组用例，没有噪声平坦性用例。

**107 复算（白噪 / 粉噪 −20 dBFS，hop 1600、400 帧、丢前 100 帧；「原始测量」= 逐帧 dB 的均值）**

| 估计量 | 白噪 400 Hz→16 kHz 斜率 | 白噪 200→800 Hz 台阶 | 粉噪斜率（应 −3.0） | −6 dBFS 正弦 1 kHz 读数（用例要求 −6 ± 1） |
|---|---|---|---|---|
| 现行 max | **+1.49 dB/oct** | **+3.18 dB**（400→800 单段 +3.58） | −1.59 dB/oct | −6.65 ✓ |
| 研究的混合版 | +0.31 | −0.01 | −2.66 | **−9.01 ✗** |
| 段内 mean | +0.28 | +0.31 | −2.68 | −11.54 ✗ |
| 我试的「≥ 9 bin 邻域判音调」变体 | +0.33 | +0.02 | −2.79 | −7.45 ✗（差 0.45 dB） |

研究的白噪 +1.47 / 粉噪 −1.56 / 正弦 −6.65 与我逐字复刻的结果一致（台阶口径不同：研究报 +4.58 是含 100 Hz 的量法，我用 200→800 得 +3.18、400→800 得 +3.58，量级一致）。白噪从 400 Hz 到 20 kHz 抬 8.3 dB、粉噪高频斜率只剩一半——**缺陷成立**，且任何噪声类素材都会看到。注意「原始测量」的 +0.3 dB/oct 残余是 dB 域求均值的对数偏置（单 bin 点 −2.5 dB、多 bin 点趋近 0），不是估计量的问题，这一点归 R7-06。

**修法核验 —— 不能照抄**

1. 研究自己列的数（1 kHz −9.53 dB）和我的复算（−9.01 dB）都说明混合估计在 1 kHz（一点只有 4 个 bin，Hann 主瓣 4 bin 宽正好填满）把正弦压低约 3 dB，`SpectrumAnalyserTests.cpp:58` `|atTone + 6| < 1` 直接打红，也违反 `SpectrumAnalyser.h:13` 的标定承诺。研究给的退路「最小版 p = mx / H(n_frac)」正弦读 −10～−12.6 dB，更差。
2. 正弦标定与噪声平坦在「段内统计」层面互斥，只有把「音调 / 噪声」判别放到比一点更宽的邻域（主瓣 4 bin 之外正弦几乎为 0、噪声不为 0）才能两全；我用 ≥ 9 bin 邻域的粗试到 −7.45 dB 仍差 0.45 dB，说明需要真正设计（例如用邻域中位数作噪声基准、把权重映射做陡），不是当晚顺手能定的。
3. 因此修法建议改为：**(a)** 先做 R7-05 的每点预计算表（无口径争议、逐位不变）；**(b)** 由 Max 二选一：A = 保持现行「按正弦标定、峰值保持」口径，只补噪声口径说明并消除 406 Hz 台阶（例如把插值分支也换成同型统计，让两侧连续）；B = 改「噪声平坦（PSD）+ 音调保峰」的估计量，验收同时要求既有 1 kHz 用例照过、白噪功率域平均线 400 Hz→16 kHz 斜率 |·| ≤ 0.5 dB/oct、200/400/800 台阶 ≤ 1 dB、粉噪 −3 ± 0.5；**(c)** 无论 A / B，平均线必须按 R7-06 改成功率域，否则新的估计量在 dB 域平均下会再引入约 −2 dB 的跨带偏置（107：mean 估计量 + 现行 dB 域平均线 = 100 Hz +2.6 dB、20 kHz +0.4 dB）。
4. 音频逐位不变（纯界面）——属实；不涉及 ADR-013。

**严重度 / 优先级**：medium 合理（母带插件主视图的频谱倾斜偏 1.5 dB/oct、400～800 Hz 假鼓包，用户会拿它判断明暗）；P1 保留，但当晚只能落 (a) 与口径提问，(b) 等拍板。

### R7-02 · 有效时长 / 起音 / 频谱证据用因果门 —— confirmed · medium · P1 → P2 · 修法部分可用

**自己找到的证据**

- `MusicAnalyzer.cpp:1013–1029`：`reference = learnIntegratedLufs`（学习中）或 `profile.integratedLufs.value`（不学习），`effective = gatingLufs ≥ −70 && (!haveReference || gatingLufs ≥ reference − 20)`；`:1070–1075` 满足即 `effectiveSamples += length; cover()`，无回退。`MusicAnalyzer.h:28–29` 明文「还没有 I 时只用绝对门」——实现者知道并写明了因果行为，但没有写明「前奏计入后不剔除」的后果。
- 起音 `:1336–1347` 相对门 = 0.01 × 运行均值功率，`onsetEffectiveSamples` 只增；`:1368` 事件按当时 `effective` 计。
- 帧门 `:1749–1751` 用 `learnTotalPowerSum / learnFrames` 运行均值；`:1816–1830` 过门即 `++learnFrames`、`powerSum`、`lowRangeCount`；`:1988–1996` `tameSum / recoverSum / deficitWeight / validWeight` 按帧累计；`:2304–2310` 除以 `validWeight`。
- `:2126–2135` `measuredIntegrated = effectiveSeconds / 10`；`GlazeController.cpp:1201–1216` `listeningComplete`：`effectiveSeconds ≥ 15` 且 I / 真峰值 / PLR / 斜率 / 低高频占比置信度 ≥ 0.8（`GlazeConstants.h:94`）。前奏 8 s 已 `cover()`，代表性 = 1；帧流的 `measuredSpectrum = learnFrames·hop / 10` 同样把前奏帧算进去 → 15.1 s 时六项全「sure」，聆听结束。**研究的时序推演成立**。
- 对照档案：分析器规格 `:27` 的「历史块可随相对门限变化重新参与判定」只写在 I / LRA 一句里；`:30` 有效时长「按新增音频样本计」；时间线 `:20` 「累计有效音频样本（静音与门下的块不算）」——「门下」按 `MusicAnalyzer.h:29` 是「学习区间的 I − 20 LU」，这个 I 只有区间结束才定，所以规格意义上的门本就是追溯的，实现是因果近似。实现修订 F-01（`:53`）只在运行期加了画像失配上限与「建议重新聆听」提示，且它针对的是 18 s 前奏（整段聆听都落在前奏里，任何门都救不了）。**不是已定取舍、不是已修**。

**对研究结论的修正**

1. 频谱证据的残差是「本帧 1 s 平均电平 − 同帧邻段轮廓」（`:1920`、`:1933–1934`、`:1986`），即前奏贡献的是**它自己的谱形**，不是电平差；再者逐段静音门 `bandGateDb = −60 dBFS`、6 dB 淡入（`:101–102`、`:1921`）会把 −40 LUFS 前奏的多数段权重压低——研究表里 −40 × 8 s 那行的「证据 8/15 来自前奏」偏高；前奏在 −30～−38 LUFS（比正文轻 20～28 LU）时门几乎不咬，那时 8/15 才成立。
2. 影响面：只有前奏比正文轻 ≥ 20 LU 且长几秒的歌；差 < 20 LU 的前奏本来就在门内、画像合法地是混合。这类歌存在但不是多数。
3. `effectiveSeconds` 改追溯后会**变小**（I 抬高时前奏被剔除）：`GlazeController.cpp:2646–2649` 的 `listenedSeconds / requiredSeconds` 会倒退，界面进度条与「满 15 s」逻辑要接受非单调，研究没提。

**修法核验**：第 1 步（`effectiveCount[800]` 按 `histogramBinOf(gatingLufs)` 计格、在 `updateLoudnessProfile` 里与 LRA 同一遍历求门上时长）与块长无关，`the profile is bit-identical for any host block size`（测试 `:1754`、`:2643`）应照过；`:1403–1427` 两个既有用例（静音 5 s、后接 −30 dB 段）的期望值（10.0 / 5.0 s）在追溯门下不变。但 `cover()` 位图不可回退（研究已承认代表性只会偏高）；起音改 280 格直方图属新设计；**第 1 步不动帧流**，tame / recover 的前奏份额只是从 8/15 变成 8/23，主要症状要等第 2 步（65 KB 新状态）。改动改变有安静前奏的歌的画像与聆听时长（可能到 23～27 s），必须 Max 拍板。

**严重度 / 优先级**：medium 保留；P1 → **P2**（影响面窄、F-01 已兜住响度侧、要拍板、当晚只能做半截）。当晚可做的是把「前奏 −40 × 8 s + 正文 −10 × 12 s」用例先写成**记录型**（logMessage 现状 effectiveSeconds ≈ 20），等拍板后改断言。

### R7-03 · 8× 真峰值在高采样率降相位 —— uncertain · low · P2 · 待拍板

- 数字核对：`modules-after-T020.log:18`（48 k / 512 default）meterIn 0.147 + meterOut 0.147 = 0.294 % / 1.024 % = 28.7 %；`:20`（192 k / 512）0.577 + 0.578 = 1.155 % / 4.008 % = 28.8 %；`clay-profile-top.txt:2/5` processChunk 3166 vs LoudnessMeter::process 自身 933 → 表内真峰值约 77 % → 整插件约 22 %。研究标题的 22 % 与正文的 28.8 % 是两个口径（真峰值部分 / 两表整体），都对。
- `TruePeak.h:42–44` 固定 8× / 64 抽头与采样率无关——属实。`ADR-004` 修订段：4×24 在 0.42 fs 以上读低（0.45 fs 处 −1.37 dB）、**Clip 削出来的高频尖峰**让 16× 参考最坏 +1.18 dB，所以升到 8×64；`ADR-004` 后果段已写明「三个检测器每样本每声道 448 次乘加，48 kHz 约占一个核的 1 %」——这是 Max 已接受的代价。
- 反驳点：研究说「192 kHz 下 0.4 fs = 77 kHz 以上没有音乐内容」，但 Clip 级 4× 过采样后再下采样到 1×，半带滤波通带到约 0.45 fs（ADR-004 背景段），削波谐波在 96 k 下可延伸到 43 kHz、192 k 下到 86 kHz——ADR-004 选 8× 的那个理由（近奈奎斯特的削波尖峰）在高采样率下**仍然存在**，只是能量更小；降到 2× / 1× 前必须在 96 k / 192 k 用 Clip 100 % 的素材重做 ADR-004 那套 16× 参考对照，不能只凭 BS.1770-4 附件的建议。附件 2 的「96 k 用 2×、192 k 不需过采样」我本轮没有核到原文，按 uncertain 记。
- 修法改变 ≥ 88.2 k 的表读数与限幅器检测（`设计规格-GLAZE.md:58` 验收 4「插件自己的 8× 表口径」）、跨 R1、工作量 L → 只能立项待拍板，不是当晚项。低 / P2 维持；若 Max 不在乎高采样率 CPU，可直接关闭。

### R7-04 · 100 ms 边界的直方图遍历与 IN 表无人读的读数 —— confirmed（结构）· uncertain（幅度）· low · P2

- `LoudnessMeter.cpp:318–322` 第一遍 800 格、`:336–345` 第二遍 800 格，每个 `finishSubBlock`（每 100 ms，`:266–267`）都跑；IN / OUT 两表相同——属实。
- IN 表读数：`CeladonChain.h:118–123` 只暴露 OUT 的 LUFS 与两表真峰值；`PluginProcessor.h:91–94` 同；全库只有 `GlazeControllerTests.cpp:2814–2815` 读 IN 表的 `isLoudnessEnabled / getProcessedSamples`，`GlazeController.cpp:2563/2608` 与 `CeladonChain.cpp:357/400` 读的是子块环与块峰值。**IN 表的 shortTermLufs / integratedLufs 原子量无人读——属实**。IN 表的 K 加权本身不能省（子块环 `ringEnergy` 与读数 `subBlockEnergy` 在同一循环 `:262–263`，分析器要子块）。
- 同一宿主块内的其他遍历：分析器 `refreshIntegrated`（`:1079–1099`，≤ 800）在 `consumeSubBlock` 里、`updateLoudnessProfile` 的 LRA 两次遍历（`:2147–2173`）在 `processInput` 的同一块（`:917–920`，loudnessDirty 即刷新）——结构属实。两表子块网格从各自 prepare 起计数、块长相同，边界确实重合。
- 幅度：≈ 4 k 次「double 加 + uint 加 + 比较」，M2 Pro 上估 2～4 µs；192 k / 32 一块 167 µs → 1～2 %，48 k / 32 一块 667 µs → 0.3～0.6 %。日志里 `meterIn_worst%` 在 192 k / 32 是 11.4 %，远大于此，说明表的最坏块由别的（抖动 / 缓存）主导，尖峰在数据里分辨不出来——研究自己也标了估算。T-021 REPORT 的验收口径是 `structWorst` 的整块结构性最坏，不是单模块。
- 修法：① 「第一遍改随门块累加」会改 `integratedLufs` 的求和顺序（末位可能变）；表的读数没有逐位摘要测试（摘要只在 BalanceTests），DspPartsTests 用 0.05 / 0.1 LU 容差，可接受，但**更稳的写法是两遍都只扫 `[0, topBin]` / `[firstBinAtOrAbove(gate), topBin]`**（加 0.0 不改 double 和，逐位不变、零风险，代码 10 行）。② `setReadoutEnabled` 省的是每 100 ms 约 1～2 µs（平均 < 0.002 %），要加 API、加测试、改 CeladonChain::prepare，收益不配成本——建议不做。
- low / P2 维持；当晚只做 ①-topBin 版或整条延后。

### R7-05 · analyse 每帧重算循环不变量 —— confirmed · low · P2 · 修法可用

- `SpectrumAnalyser.cpp:180–182`：每点 `frequencyAt(i)`（`:55` 一次 `std::pow`）+ 两次 `std::pow(2.0, ±halfStep)`（与 i 无关）；`:188–198` floor / ceil / jlimit。两路 × 480 × 30 fps（`PluginEditor.cpp:23` refreshHz = 30）= 86.4 k 次 pow / s ≈ 2～4 ms / s 的消息线程时间——属实但绝对量小（约 0.3 % 一个核），与两路 8192 点 FFT 同量级。
- 「显示逐位不变」：把 `(double) frequencyAt(i)`、`low`、`high`、`position / below / above / t`、`first / last` 在 `setSampleRate` 用**同一算式**算一次存表，analyse 只做 max / 插值与 log10，结果逐位相同——成立。若与 R7-01 的新估计量一起改则按新口径。
- 补充一个更大的同类省法：IN 虚线可开关（界面规格 `:37`），关着时可跳过 IN 那路的 FFT（每帧一次 8192 点变换 + 加窗），比省 pow 多。

### R7-06 · 平均线 / 峰值线取自弹道后的显示曲线、dB 域平均、按帧计时 —— confirmed · low · P2 · 修法部分可用

- `:241` `output = channels[output].display`；`:221–226` 显示曲线已做升 0.6 / 落 0.15 的 dB 域非对称平滑；`:260` 平均线再做 0.04 的 dB 域 EMA；`:262–274` 峰值线也从 display 取；`:17–23` 常数按帧；`update()` 无 dt 参数，而 `PluginEditor.cpp:993/1034–1035` 给电平表传了 `elapsed`（钳 0.25 s）——全部属实。
- 107 复算（白噪、现行 max）：平均线 − 原始测量 = 100 Hz +3.0 dB、400 Hz +2.05、8 kHz +0.83、20 kHz +0.55——偏置成立且**随频率变化**（单 bin 点起伏大 → 升快落慢的偏置大），它恰好抵掉一部分 R7-01 的倾斜（+1.49 → 平均线 +1.29），所以两条要一起改。
- 修法：平均线改为对**原始测量功率**做功率域 EMA 再转 dB、按 dt 计时——可用，且是 R7-01 的前提（见 R7-01 修法 (c)）。**「峰值线直接对 measured 取大」不建议**：原始测量逐帧起伏（单 bin 点 ±5 dB 以上），1.5 s 保持内取最大会把噪声类素材的峰值线抬到起伏上包络（估 +5～7 dB），与设计稿「峰值保持」的观感不同；峰值线照旧从显示曲线取即可。`update(double dt)` 改签名要同步改 `SpectrumAnalyserTests.cpp` 的 14 处 `update()` 调用与 restartOutput 用例（它按样本数不按帧，不受影响）。

## 2. 漏项（missed）

1. **低频分辨率（设计）**：48 k 固定 8192 点 → bin 5.86 Hz，20～60 Hz 只有 3～10 个 bin，一个 30 Hz 音的 Hann 主瓣占 18～42 Hz；母带频谱仪常用 16 k～32 k 点或多分辨率来看低音。修坯的低切 / 收单频率画在这张图上（界面规格 `:37`），低端糊掉会影响判读。低严重度，需 Max 定（16384 点 = 341 ms 窗、慢一倍的响应）。
2. **频谱仪 OUT 含等响监听增益、响度小窗不含**：`PluginProcessor.cpp:631–632` 把 Listen 打开时的监听增益推进频谱仪，`CeladonChain.h:117–123` 的表读数在监听补偿之前——两处读数在等响时差一个 Listen 差值。已在代码注释里写明是有意的（「频谱仪显示的就是此刻听到的」），不算缺陷，但界面规格 `:37` 没写这一句，建议补口径说明（文档项）。
3. R7-02 的同类：三条流的「有效」参考各不相同（子块流用 K 加权 I − 20 LU、帧流与起音用线性功率运行均值 − 20 dB），同一段音乐在三条流里可能门判不一致；研究的第 2 步若做，应顺手统一为同一口径（都用 I 的直方图门）。

## 3. 数值附录（107，逐字复刻，已删）

- 切换点 407.4 Hz；20 kHz 一点 52 个 bin，H_52 = 4.54。
- 白噪 −20 dBFS 现行算法：原始测量 100 / 200 / 400 / 800 / 2 k / 8 k / 16 k / 20 k = −53.21 / −52.93 / −53.33 / −49.75 / −48.24 / −46.25 / −45.41 / −45.08 dB；平均线 −50.20 / −50.86 / −51.28 / −48.02 / −46.98 / −45.42 / −44.42 / −44.53。
- 粉噪现行：原始测量 400 Hz→16 kHz −1.59 dB/oct；100→400 Hz（插值区）−2.97 dB/oct。
- 正弦 −6 dBFS 显示读数（60 帧 hop 1600，displayDbAt）：max 1 k / 3 k = −6.65 / −6.02；混合版 −9.01 / −6.02；段内 mean −11.54 / −14.16。10 kHz 一栏我读到 −11.3 是 `displayDbAt` 在两点间按 dB 插值、且 10 kHz 落在点 431 的 0.96 处、点 430 读的是泄漏——研究报的 −6.65 应是直接取点值；两种量法都合法，不影响结论。
