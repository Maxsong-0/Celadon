# R7 · 分析与计量子系统深度研究（MusicAnalyzer / LoudnessMeter / AnalyzerTap / SpectrumAnalyser）

> T-032（2026-09-29）研究代理 R7，Fable 5.1。只读审计：读了 `01-框架/设计规格-GLAZE-分析器.md`、`设计规格-GLAZE.md`（含 7 节 Codex 修订）、`设计规格-GLAZE-实现修订.md`、`设计规格-GLAZE-时间线与保存.md`、`设计规格-GLAZE-决策与校准.md`、`设计规格-总体.md`、`设计规格-界面-V4融合版.md`、ADR-004 / 012 / 013 / 014，仓库 `Source/dsp/{LoudnessMeter,MusicAnalyzer,TruePeak}.*`、`Source/AnalyzerTap.h`、`Source/ui/SpectrumAnalyser.*`、`Source/dsp/{CeladonChain,GlazeController,GlazeCalibrator,BalanceModule}.*` 相关段、`Source/Plugin{Processor,Editor}.cpp` 接线、四个测试文件、`docs/validation/2026-09-28-T021-perf-acceptance/` 的模块基准。数值实验在 107（`~/.cache/celadon-t032/R7/`，numpy 2.5.3 / scipy 1.18.1，做完已删）。
> 行号以工作树 `e6b445b`（v0.1.8.5）为准。

## 0. 结论一览

| 编号 | 严重度 | 优先级 | 一句话 | 工作量 | 置信度 |
|---|---|---|---|---|---|
| R7-01 | medium | P1 | 频谱仪「段内取最大 bin」让噪声类素材在 ≥ 406 Hz 出现 +1.5 dB/oct 假倾斜、400→800 Hz 有 +4.6 dB 台阶（白噪 20 kHz 比 400 Hz 高 8.3 dB，粉噪读 −1.5 dB/oct） | M | 0.85 |
| R7-02 | medium | P1 | 分析器的有效时长 / 起音 / 频谱证据用「当时」的学习区间 I 做门，安静前奏一旦计入不再剔除：−40 LUFS × 8 s 前奏接 −10 LUFS 时 15.1 s 就凑满「15 s 有效」（追溯门要 23 s），tame / recover 证据 8/15 来自前奏 | M | 0.70 |
| R7-03 | low | P2 | 两块表的 8× / 64 抽头真峰值占整插件 CPU 约 22 %（48k/512）、192k 下同比例；BS.1770-4 在 ≥ 96 kHz 不要求 4× 以上，可按采样率降相位数（口径决定，需 Max 与 R1） | L | 0.55 |
| R7-04 | low | P2 | 100 ms 子块边界的结构性尖峰：表每 100 ms 两遍 800 格全扫 + IN 表算了没人读的 S / I 读数；分析器同一块再扫 ≤ 1600 格 | S | 0.60 |
| R7-05 | low | P2 | SpectrumAnalyser::analyse 每帧每点重算 3 次 pow（约 86 k 次 / s），bin 区间与插值系数可在 setSampleRate 预计算 | S | 0.80 |
| R7-06 | low | P2 | 频谱仪平均线 / 峰值线从已加非对称弹道的显示曲线取、在 dB 域平均：白噪平均线比真实均值高约 2 dB；时间常数按帧计数不按经过时间 | S | 0.75 |

核查通过、不立项的部分见第 3 节（K 加权按采样率重算、门限与 400 ms / 3 s 块、LRA、真峰值口径、时间轴对齐、实时安全、NaN / 极端参数）。

## 1. 缺陷与修法

### R7-01 · 频谱仪的「段内取最大 bin」估计量对噪声类素材有系统性倾斜与台阶（medium · P1）

**证据（文件:行号）**

- `Source/ui/SpectrumAnalyser.cpp:174`：`halfStep = 0.5·log2(1000)/479 ≈ 0.0104 oct`，每点宽 ≈ 1.44 %；`:181–182` 点的 bin 区间 `[low, high)`；`:185` `if (high - low < 1.0)` 走相邻两 bin 功率插值，否则 `:197–201` 对 `floor(low)…ceil(high)` 各 bin **取最大**。
- 48 kHz / 8192 点 bin 宽 5.86 Hz：`high − low = 1` 落在 **406 Hz**；96 k / 192 k 用 16384 / 32768 点（`:71`），bin 宽不变，切换点也在 406 Hz。20 kHz 处一点覆盖约 50 个 bin。
- `SpectrumAnalyser.h:13` 给出的标定承诺是「0 dBFS 正弦读 0 dB」，没有说明噪声口径；`设计规格-界面-V4融合版.md:37` 的显示斜率 0 / 3 / 4.5 dB/oct（默认 4.5）是按「粉噪在 3 dB/oct 下为平」的母带习惯设的。
- `tests/SpectrumAnalyserTests.cpp` 只测正弦读数、泄漏与 RMS，没有噪声平坦性用例。

**机理**：n 个独立 bin 的功率（噪声下服从指数分布）取最大，期望是均值的 H_n（调和数）：n = 2 → +1.76 dB，5 → +3.6 dB，10 → +4.7 dB，50 → +6.5 dB；再加 `floor / ceil` 多取的边界 bin。于是一条平的白噪从 406 Hz 起逐步抬高，粉噪的 −3 dB/oct 在高频被吃掉一半；406 Hz 两侧两种估计量直接切换，形成台阶。

**实验（107，逐字复刻算法，含 0.6 / 0.15 弹道与 0.04 平均线；白噪 / 粉噪 −20 dBFS，400 帧，丢前 100 帧）**

| 素材 | 量 | 400 Hz | 800 Hz | 2 kHz | 8 kHz | 20 kHz | 400 Hz→16 kHz 斜率 | 200/400/800 台阶 |
|---|---|---|---|---|---|---|---|---|
| 白噪 | 原始测量 | −53.35 | −49.56 | −48.28 | −46.26 | −45.08 dB | **+1.47 dB/oct** | **+4.58 dB** |
| 白噪 | 显示曲线 | −51.36 | −48.06 | −47.14 | −45.44 | −44.31 | +1.22 | +3.59 |
| 白噪 | 平均线 | −49.87 | −48.01 | −47.12 | −45.57 | −44.50 | +0.97 | +3.22 |
| 粉噪 | 原始测量 | −45.95 | −45.10 | −48.34 | −52.32 | −55.24 | **−1.56 dB/oct**（应 −3.0） | +2.83 |
| 粉噪 | 100→400 Hz（插值区） | | | | | | −3.01 dB/oct（正确） | |

即：白噪 20 kHz 比 400 Hz 高 8.3 dB；粉噪在默认 4.5 dB/oct 显示斜率下不会像预期那样呈 +1.5 dB/oct，而是约 +3 dB/oct，且 400～800 Hz 有一个 3～4.6 dB 的鼓包——任何噪声类（即绝大多数）音乐素材都会看到。

**修法（`SpectrumAnalyser.cpp`，只改界面计算，不碰音频）**

1. `setSampleRate` 里为每个点预计算：`first`、`last`、插值系数 `t`、是否插值、`n = last − first + 1`、`H_n`（查表 `H[1..64]`：`H[n] = Σ 1/k`，点宽 < 1 时 H = 1）。这同时是 R7-05 的修法。
2. `analyse` 的宽点分支改成「混合估计」：
   ```
   mx = max(power[first..last]);  mn = mean(power[first..last]);
   r  = mx / max(mn, tiny);        w = clamp((r − H_n) / H_n, 0, 1);   // 0 = 噪声样，1 = 纯音
   p  = mx / (1 + (H_n − 1)·(1 − w));                                 // 噪声除以 H_n，纯音不除
   ```
   107 复算（同一素材、同一弹道）：白噪斜率 +0.13 dB/oct、台阶 +0.64 dB；粉噪 −2.97 dB/oct；三正弦 −6 dBFS 读数 1 kHz −9.5 / 3 kHz −6.0 / 10 kHz −6.65 dB（现行 −6.65 / −6.0 / −6.65；1 kHz 点只有 2～3 个 bin，主瓣填满整点无法与噪声区分，读低 3 dB——若 Max 更看重正弦标定，退一步只做「`p = mx / H(n_frac)`」的最小版：白噪 +0.01 dB/oct、台阶 +0.40，但正弦读 −10.2 / −10.5 / −12.6 dB）。
3. 平均线改为功率域 EMA（见 R7-06）。
4. 头注释与 `设计规格-界面-V4融合版.md` 补一句噪声口径（「噪声按功率密度平、粉噪在 3 dB/oct 斜率下为平」）。

**改后输出**：音频逐位不变（纯界面）。**验证**：`SpectrumAnalyserTests` 新增：白噪 −20 dBFS 喂 12 s，`getAverageDb()` 在 400 Hz→16 kHz 的斜率 |·| ≤ 0.5 dB/oct、200/400/800 Hz 台阶 |·| ≤ 1 dB；粉噪斜率 −3 ± 0.5；−6 dBFS 的 3 kHz 与 10 kHz 正弦读 −6 ± 1 dB。工作量 M（算法 + 预计算 + 测试约 2 h）。置信度 0.85。

### R7-02 · 有效时长、起音与频谱证据的门是因果的，安静前奏计入后不再剔除（medium · P1）

**证据**

- `Source/dsp/MusicAnalyzer.cpp:1013–1029`：有效音频门 `reference = learnIntegratedLufs`（学习区间**到目前为止**的 I），`effective = gatingLufs ≥ −70 && gatingLufs ≥ reference − 20`；`:1070–1075` 满足即 `effectiveSamples += length; cover(...)`，之后永不重判。
- `:1335–1353`：起音的相对门 `relativeGate = 0.01 × onsetPowerSum / onsetPowerGroups`（运行均值），`onsetEffectiveSamples`、`subEnergy / midEnergy` 同样只增不改；`:1368` 事件按当时的 `effective` 计数。
- `:1747–1751`：帧门 `relativeOk = total ≥ 0.01 × learnTotalPowerSum / learnFrames`（运行均值）；`:1988–1997` `tameSum / recoverSum / validWeight` 按帧等权累计（dB 域时间平均，`:2304`、`:2309` 除以 `validWeight`）；`:1827` 低频电平直方图同理。
- `:2126–2135`：`measuredIntegrated = effectiveSeconds / 10 s` 直接决定 I / 真峰值 / PLR 的置信度；`设计规格-GLAZE-时间线与保存.md` 第 2 节：聆听「满 15 s 有效音频且主要特征置信度够才结束」。
- 对照 I 本身：`:1043–1055` + `:1079–1099` 用直方图，`设计规格-GLAZE-分析器.md` 第 2 节明文「历史块可随相对门限变化重新参与判定」；`MusicAnalyzer.h:28–29` 把有效音频门定义为「不低于**学习区间的 I** 20 LU 以上（与 LRA 同一相对门）」——学习区间的 I 只有在区间结束时才确定，因果实现与这句定义不一致。
- 症状已在验收里出现过：`设计规格-GLAZE-实现修订.md` 第 5 节 F-01「18 s 安静前奏接响段」只加了运行期的画像失配保护，没有从学习侧根治。

**机理与数值（107，100 ms 子块模型，I 按 BS.1770 门限由直方图算）**

| 前奏 → 响段 | 40 s 后有效时长 因果 / 追溯 | 凑满 15 s 的时刻 因果 / 追溯 |
|---|---|---|
| −40 LUFS × 8 s → −10 LUFS | 40.0 s / 32.0 s | **15.1 s / 23.0 s** |
| −45 × 12 s → −12 | 40.0 / 28.0 | 15.1 / 27.0 |
| −32 × 10 s → −10 | 40.0 / 30.0 | 15.1 / 25.0 |
| −28 × 10 s → −10（差 18 LU，在门内） | 40.0 / 40.0 | 15.1 / 15.0 |

前奏比正文轻 20 LU 以上时，聆听在 15.1 s 就可能结束，而此时 tame / recover 证据、起音密度与强度、低频电平范围、谱通量统计里有一半以上来自随后被 I 的相对门排除的前奏；代表性、置信度也按这 15 s 算满。I、真峰值、PLR 不受影响（I 是直方图门，真峰值取最大），`powerSum` 类线性功率和被响段主导、影响小。

**修法（分两步；第 1 步当晚，第 2 步可后续）**

1. **子块流改追溯门（S–M）**：新增 `std::array<uint32, histogramBins> effectiveCount`，`consumeSubBlock` 里对每个 `subBlockInside` 的子块按 `histogramBinOf(gatingLufs)` 计一格（不再直接累加 `effectiveSamples`）；`updateLoudnessProfile` 里 `effectiveSamples = subBlockLength × Σ_{bin ≥ firstBinAtOrAbove(learnIntegratedLufs − 20)} effectiveCount[bin]`（与 LRA 同一遍历，O(800)/100 ms）。`cover()` 的位图无法回退，改为只在「当前 I 已有效且子块在门上」时打点（前奏时 I 还没形成，位图会多记，代表性只可能偏高一点，可接受并写明）。起音同法：`onsetTimeCount[280]`、`onsetEventCount[280]`（0.5 dB 一格的慢包络电平直方图）替代 `onsetEffectiveSamples / onsetEvents` 的裸累加，密度 = Σ_{格 ≥ 均值 − 20 dB} 事件 / Σ 时间；强度中位数直方图保持因果（次要）。
2. **帧流改追溯门（M–L）**：`powerSum / tameSum / recoverSum / deficitWeight / validWeight`（27 段 × 5）和 `lowRangeCount` 按帧总功率的 1 dB 格（−80…−20 dBFS，60 格）分桶累计，画像取 Σ_{格 ≥ 均值 − 20 dB}；内存 27 × 5 × 60 × 8 B ≈ 65 KB（Impl 现约 40 KB + 向量），每帧多一次索引。

**改后输出**：前奏与正文相差 < 20 LU 的歌逐位不变；有安静前奏的歌画像与聆听结束时刻改变 → 方案与音频改变（这正是目的）。**验证**：`MusicAnalyzerTests` 新增「8 s −40 LUFS 粉噪 + 12 s −10 LUFS 粉噪」：`profile.effectiveSeconds ≈ 12 ± 0.5`（不是 20），I / 真峰值置信度到 t ≈ 23 s 才达 0.8；「前奏带 1 kHz 缺口、正文平」：`recoverEvidenceDb[14] < 0.5 dB`。既有「the profile is bit-identical for any host block size」与「learning interval」用例照跑。工作量 M（第 1 步）。置信度 0.70（缺陷成立的把握高；修法要 Max 认可「聆听可能变长到 23～27 s」）。

### R7-03 · 两块表的 8× 真峰值是子系统最大的 CPU 项，高采样率下可降相位数（low · P2 · 口径决定）

**证据**：`docs/validation/2026-09-28-T021-perf-acceptance/modules-after-T020.log:18`（M2 Pro，Release，48k/512 default）：`meterIn_avg% = 0.147, meterOut_avg% = 0.147`，合计 0.294 % = 整插件 1.024 % 的 **28.8 %**；192k/512：0.577 + 0.578 = 1.155 % of 4.008 %（28.8 %）；`clay-profile-top.txt` 里 `TruePeakDetector::processChunk` 3166 样本 vs `LoudnessMeter::process`（自身）933，表内约 3/4 是真峰值。`TruePeak.h:42–44` 固定 8× / 64 抽头、与采样率无关；`TruePeak.h:14–15` 的 4× 低估实测是 48 kHz、0.4 fs 以上做的。BS.1770-4 附件 2 表：48 k 用 4×，96 k 用 2×，192 k 不需要过采样。

**机理**：每样本每声道 7 相 × 64 抽头 = 448 次乘加，不随采样率减少；192 kHz 下带内 0.4 fs = 77 kHz 以上没有音乐内容，8× 的余量全是浪费。

**修法**：`TruePeakDetector` 按采样率选相位数（≥ 88.2 k 用 4× / 32 抽头，≥ 176.4 k 用 2×），向量核加一套 3 相 / 1 相版本；限幅器共用同一类，改动须与 R1 同步，且改变 `设计规格-GLAZE.md` 验收 4「插件自己的 8× 表口径」在高采样率下的含义 → **先请 Max 拍板**。48 kHz 逐位不变；≥ 88.2 kHz 表读数与限幅器检测变（≤ 0.1 dB 量级）。**验证**：`DspPartsTests` 已有的 processSample / processBlock 逐位对照按相位数参数化；性能护栏加 96k / 192k 行。工作量 L。置信度 0.55。

### R7-04 · 100 ms 子块边界的结构性尖峰与 IN 表的无人读数（low · P2）

**证据**：`Source/dsp/LoudnessMeter.cpp:312–348` `updateIntegrated()` 每 100 ms 两遍遍历全部 800 格（第一遍求绝对门以上总能量 / 计数，第二遍求相对门以上），且不分 IN / OUT；`CeladonChain.h:122–123` 只暴露 OUT 的 `getShortTermLufs / getIntegratedLufs`，`Source/` 内没有任何 `getInputMeter().get*Lufs()` 调用（grep 全库，只有测试用 `isLoudnessEnabled`）——IN 表的 `finishSubBlock` 短期求和、直方图、`updateIntegrated` 与两个原子读数完全没人读。同一块里分析器再做 `refreshIntegrated`（`MusicAnalyzer.cpp:1091–1095`，≤ 800）与 `updateLoudnessProfile` 的 LRA 扫描（`:2147–2173`，≤ 1600）。合计每 100 ms 约 4 k 次格遍历，落在同一个宿主块上；192k/32 时一块只有 167 µs，几 µs 就是 1～2 个百分点的「结构性最坏」（T-021 的验收口径正是最坏块 ≤ 0.5 pp）。

**修法**：① `updateIntegrated` 第一遍改用随门块累加的 `absoluteEnergy / absoluteCount`（分析器已这么做），第二遍只扫 `[firstBinAtOrAbove(gate), topBin]`；② `LoudnessMeter` 加 `setReadoutEnabled(bool)`（默认 true），false 时跳过 `finishSubBlock` 的短期 / 直方图 / `updateIntegrated`、两个读数恒 `noReading`，子块环照常；`CeladonChain::prepare` 对 `inputMeter` 关掉。**改后输出**：音频逐位不变；OUT 表 I 读数因求和顺序可能差末位（测试容差 0.05 LU），IN 表读数本来无人读。**验证**：`LoudnessMeterTests` 加「readout off：子块逐位同 readout on、读数为 noReading」；perf 护栏看 192k/32 最坏块。工作量 S。置信度 0.60（尖峰幅度是估算，未在本机量）。

### R7-05 · SpectrumAnalyser::analyse 的循环不变量每帧重算（low · P2）

**证据**：`Source/ui/SpectrumAnalyser.cpp:178–182`：每帧每点调 `frequencyAt(i)`（`:52–56`，一次 `std::pow`）与 `std::pow(2.0, ±halfStep)` 两次（与 i 无关），两路 × 480 点 × 30 帧/s ≈ 86 k 次 `pow` / s，外加 `:188–198` 的 `floor / ceil / jlimit`；这些都只依赖采样率与点号。**修法**：`setSampleRate` 里算好每点的 `{first, last, t, interpolate, H_n}`（与 R7-01 共用），`analyse` 只剩 `max / mean` 与一次 `log10`。**改后输出**：显示逐位不变（同一算式、同一 double 舍入；若与 R7-01 一起改则按新口径）。**验证**：既有 SpectrumAnalyserTests 通过；主线程 profile 里 `analyse` 自身时间下降。工作量 S。置信度 0.80。

### R7-06 · 平均线 / 峰值线取自已加非对称弹道的显示曲线，且按帧数计时（low · P2）

**证据**：`SpectrumAnalyser.cpp:241`、`:258–275`：`average` 与 `peak` 都从 `channels[output].display`（`:221–226` 已做「升 0.6 / 落 0.15」非对称 dB 域平滑）再做 0.04 的 dB 域 EMA；`:17–23` 的常数（`peakHoldLength = 45` 帧、`fallAmount`、`idleFallDb`）都假定 30 fps；`PluginEditor.cpp:1034–1035` 给电平表传了 `elapsed`，给频谱仪没有。**数值**（R7-01 同一实验）：白噪 100 Hz 原始测量 −53.13 dB，显示曲线 −50.96，平均线 −50.69——「约 1 s 慢速平均」比真实均值高 2.2～2.4 dB，并继承显示曲线在 406 Hz 的台阶（平均线 400 Hz −49.87 vs 200 Hz −50.98 vs 800 Hz −48.01）。**修法**：`analyse` 同时留每点线性功率 `measuredPower`；`average` 改为功率域 EMA `avgPow += (measuredPower − avgPow) · (1 − exp(−dt / 0.8 s))` 再转 dB，`peak` 直接对 `measured`（dB）取大；`update(double dtSeconds)` 由编辑器传经过时间，`peakHold` 用秒。107 复算：功率域平均线的白噪 400 Hz→16 kHz 斜率与台阶在 R7-01 修法下为 +0.23 dB/oct / −0.33 dB（现行 +1.30 / +2.80）。**改后输出**：音频逐位不变。**验证**：并入 R7-01 的噪声用例（平均线与原始均值差 ≤ 0.5 dB）。工作量 S。置信度 0.75。

## 2. 实验数据

### 2.1 K 加权系数按采样率重算（`kweight.py`）

`LoudnessMeter.cpp:34–65` 的模拟原型换算（与 libebur128 同一公式）：

- 48 kHz：搁架 b 与标准表 1 最大差 8.9e-16、a 2.2e-16；高通 a 2.2e-16；10 Hz～20 kHz 频响差 3.2e-11 dB（即逐位就是标准表）。
- 相对 48 kHz 标准响应（同一物理频率，20 Hz～16 kHz）：44.1 k 最大 0.0053 dB；88.2 k 0.026；96 k 0.028；176.4 k 0.039；192 k 0.040 dB（1 kHz 处 −0.017～−0.027 dB）。远小于 EBU Tech 3341 的 ±0.1 LU 容差。**通过**。

### 2.2 频谱仪估计量（`spectrum_bias.py`、`spectrum_hybrid.py`、`spectrum_fix.py`）

见 R7-01 / R7-06 表格。补充：三种估计量在同一弹道下的显示曲线——

| 素材 | 现行 max | 最小版 max/H(n) | 混合版 |
|---|---|---|---|
| 白噪 斜率 / 台阶 | +1.20 dB/oct / +3.59 dB | +0.01 / +0.40 | +0.13 / +0.64 |
| 粉噪 斜率 / 台阶 | −1.86 / +2.22 | −3.06 / −0.96 | −2.97 / −0.80 |
| −6 dBFS 正弦 1 k / 3 k / 10 k | −6.65 / −6.02 / −6.65 | −10.23 / −10.54 / −12.55 | −9.53 / −6.02 / −6.65 |

混合版帧间起伏（无弹道）：白噪 100 Hz 3.0 dB、1 kHz 1.8 dB、8 kHz 1.3 dB（现行 max 在高频的起伏更小，因为取最大本身是偏置的平滑）。

### 2.3 有效时长门（`effective_gate.py`）

见 R7-02 表格。

## 3. 核查通过项（有证据，不立项）

1. **M / S / I / 门限**：`MusicAnalyzer.cpp:979–1010` M = 最近 4 子块均方平均、S = 30 子块；`:1043–1055`、`:1079–1099` I 门块 400 ms / 75 % 重叠、绝对门 −70、相对门 = 绝对门以上均值 − 10 LU，直方图 0.1 LU、格中心判定（≤ 0.05 LU）；`LoudnessMeter.cpp:287–347` 同口径。`≥` 与标准的 `>` 只差测度零集。测试 `MusicAnalyzerTests.cpp:919–954` 对照 Tech 3341 构造、`DspPartsTests.cpp:546–557`。
2. **LRA**：`:2141–2176` 相对门 −20 LU 取绝对门以上 S 的功率均值、P10 / P95 下标 `(n − 1)·p + 0.5` 截断 = libebur128；至少 10 个 S 才给数；60 s 才满置信。测试 `:959–983` 对照 Tech 3342 四个构造 ±1 LU、与直方图参照 ±0.1。
3. **真峰值口径**：两块表与限幅器共用 `TruePeakDetector`（8× / 64 抽头，NEON / SSE2 / 标量三条路径逐位，BUG-001 / 002 已修）；子块峰值按段最大折进（`LoudnessMeter.cpp:211–231`），与不切段逐位（测试 `DspPartsTests.cpp:697–723`、`MusicAnalyzerTests.cpp:577–643`）。检测器延迟 32 样本使子块峰值描述早 32 样本的一段，已注释，对 3 s 窗的 PSR 与整区间 PLR 无影响。
4. **采样率归一**：子块 `round(0.1·fs)`（44.1 k 恰 4410）、起音组 `round(fs / 3000)` 封顶 128、包络 / 不应期 / 预热按组秒数折算、帧常数按 Balance 的 hop 折算（`MusicAnalyzer.cpp:568–649`）；先试后用的块 = 整数个组（44.1 / 48 / 96 / 192 k 恰 100 ms，88.2 k 为 8816 样本、与子块网格无关）。
5. **时间轴对齐**：`processInput` 用表的累计样本数核对（`:871–890`），Balance 单独 reset 或漏调时在块起点断开（`:1636–1653`）；跨不连续点的子块 / 帧 / 包络丢弃、累计不动（`:846–853`）；宿主位置按切片修正（`GlazeController.cpp:2971–2981` 的 `hostInputOffset`）。「块长无关」有专门用例（`:1754`、`:2643`）。
6. **实时安全**：`MusicAnalyzer::Impl` 全定长、`allocate()` 只在 prepare（`:2596–2601`）；`LoudnessMeter` 原子读数 + `atomicMax` CAS；`AnalyzerTap` 用 `AbstractFifo` 单生产单消费、满则丢、只拷贝（`AnalyzerTap.h:47–76`）；两处「不分配」用例（`:1785`、`:2697`、`:2978`）。`readSubBlocksSince` 在表 reset 后返回 0 而非负数的「−0」情形已在测试 `:745–752` 写明并由 `hostReset → invalidateCurrentWindow`（`GlazeController.cpp:1363`）另行覆盖。
7. **NaN / 极端值**：处理器入口非有限块置零（`PluginProcessor.cpp:595–599`），链输出非有限置零重启（`CeladonChain.cpp:255–261`），两表只吃有限值；分析器逐处 `isfinite` 与 `snapToZero`（`:961–968`、`:1310–1314`、`:1721–1725`、`:1408–1422`）；`ScopedNoDenormals` 常开（`PluginProcessor.cpp:553、571`）。
8. **频谱证据口径**：Balance 帧 = 段内总功率（`BalanceModule.cpp:270–325`，bin 视作均匀铺格、边界按重叠计权、`spectrumScale = 2/(N·Σw²)`），两声道功率平均（`:1032`），单 bin 段按段宽 / Δ 计——粉噪为平、白噪 +3 dB/oct 有用例（`MusicAnalyzerTests.cpp:1033`，实测斜率 −0.02 / +3.01）。
9. **起音**：快 10 ms / 慢 400 ms 功率包络、+6 dB 触发、+3 dB 回差、50 ms 不应期、持续低频反例用例齐全（`:1307–1369`）。鼓组密度偏低约一半属已登记项（不重报）。

## 4. 未做 / 边界

- 没有在本机或服务器编译工程、没有跑 celadon_tests（硬约束）；性能数字引自 T-021 基准日志与 profile 文件，R7-04 的尖峰幅度是按遍历次数估算。
- R7-03 属口径与跨子系统（TruePeakDetector 与限幅器共用），只给方向，需 Max 与 R1 决定。
- 频谱仪估计量的「正确」没有唯一标准（正弦标定与噪声平坦互斥），R7-01 给的是量化后的折中；若 Max 明确只要正弦标定，应至少消除 406 Hz 台阶并在界面写明噪声口径。
