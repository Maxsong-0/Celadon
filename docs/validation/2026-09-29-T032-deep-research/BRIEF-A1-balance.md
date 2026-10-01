# T-032 实现任务书 · A1-balance · Balance 动态频谱平衡

> 生成于 2026-09-29 23:0x，来源：研究代理 R*.md 与核验代理 V*.md（同目录）。逐条以「核验调整后的修法」为准。

## 你负责的文件（只许改这些）

Source/dsp/BalanceModule.{h,cpp}；tests/BalanceTests.cpp、tests/BalancePerformanceTests.cpp、tests/AdvancedMasteringTests.cpp；档案 01-框架/设计规格-Balance.md、01-框架/设计规格-高级母带控制.md（末尾加「2026-09-29 修订（T-032）」小节）

## 主进程补充要求

- R2-08 按核验的调整修法做：保留起音侧硬乘（立刻闪避），只给恢复侧加 10～20 ms 一阶平滑；h=0 逐位不变。
- R2-07 只在 ≥ 88.2 kHz 改变输出，44.1/48k 必须逐位不变（用 BalanceTests「logged digests」的摘要行改前改后人工比对，把改前摘要先跑出来存到报告里）。
- R2-05 取 maskingMaxDistance = 3（逐位不变），规格 :123 改「1～3 段」。
- 核验漏项：GLAZE 校准参考条件的 Mast 段没有算法版本盐，R2-01/R2-08 改算法后 h>0 的旧校准仍被视为有效——不改哈希，在报告与规格里写明「推响不刺 h>0 的旧校准需重新聆听」。
- 核验漏项：R2-01 修后 quick（60 ms）对 60～200 ms 亮瞬态（镲片）更快反应，加一条镲片式 150 ms 突发用例记录尾巴（logMessage 即可，不做门槛）。

## 逐条结论

### R2-01 · 推响不刺的超出量混用本段 fastLevel（释放 400 ms）与邻段 quickLevel（60 ms）：宽带电平一降就误判「刺耳」
- 严重度 / 优先级：high / P0 → 核验 high / P0；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/BalanceModule.cpp:1727`
- 证据：`const auto left = quickLevelDb[b - 3], right = quickLevelDb[b + 2];` … `const auto excess = juce::jmax (0.0, fastLevelDb[b] - 0.5 * (left + right) - 1.5);`（:1725-1727）。fastLevelDb 经 Welch 平均 + 攻击 80 / 释放 400 ms（:1144-1145），quickLevelDb 是 60 ms 对称平均（:1170-1178）。同文件 :1692 目标频谱注释自己写明「Use the common-rate input tracker: per-band Welch time constants would confuse a level step with spectral tilt」。实验（理想平谱、真实刺耳超出量恒为 0）：Mid 宽带阶跃 −10 dB → appliedHarsh 峰值 2.99 dB（h=1）/ 0.52 dB（默认 h=0.175），>1 dB 持续 0.70 s（Slow 1.24 s）；鼓点式脉冲（250 ms 周期、50 ms −12 / 其余 −30 dBFS）→ 均值 1.84 dB、100 % 时间 >1 dB；+10 dB 阶跃为 0（单向）。改本段也用 quickLevelDb 后全部精确为 0，真持续局部峰稳态两式同为 2.925 dB。
- 机理：电平下降时邻段 quick 在 60 ms 内跟下去，本段 fast 挂在 400 ms 释放曲线上，两者之差被当作频谱突出量；25 ms 攻击包络立刻把削减做上去。于是每个 decay / 鼓点间隙 / 段落变轻都把 2 ～ 8 kHz 压暗 1 ～ 3 dB（h=1）或 0.3 ～ 0.5 dB（新实例默认），与「只削持续局部峰」的规格相悖。
- 研究修法：:1727 改为 `const auto excess = juce::jmax (0.0, quickLevelDb[b] - 0.5 * (left + right) - 1.5);`（三者同一 60 ms 动态，共同电平变化相消）。备选：三者都用 fastLevelDb（与自动 Tame :1473 同口径，但各段 Welch τ 不同仍有 ≤80 ms 残差，且反应慢）。顺带把 `masteringPreviousPower[b] = bandPower[b]`（:1733）挪到 :1488 旁每 hop 更新，避免刺耳从关到开的第一 hop 拿陈旧值白触发起音保持。规格《设计规格-高级母带控制》第 4 节「超出量 = 本段 Lfast − …」同步改为「本段 60 ms 快电平」。
- 核验理由：BalanceModule.cpp:1725-1727 确用 fastLevelDb[b]（Welch 平均 + 攻击 80 / 释放 400 ms，:1119-1145）对 quickLevelDb 邻段（60 ms，:1170-1178）。规格《高级母带控制》:47 虽写「本段 Lfast」，但该节是 T-029 按 Codex 代码整理、ADR-020 状态「待 Max 复核」，且 ADR-020「备选方案」与规格 :60 自己写明「逐段 Welch 平滑会把电平阶跃当成倾斜」——不是 Max 拍板的取舍。107 独立复算：阶跃 −6/−10/−20 dB 峰值 1.57/2.99/3.00 dB、鼓点 100 % 时间 >1 dB、改 quick 后精确 0、真持续峰两式同 2.925 dB。新实例默认「自动」h=0.175 → 每拍 ≤0.52 dB 系统性压暗，手动 1～3 dB。
- **核验调整后的修法（以此为准）**：:1727 改 quickLevelDb[b]；masteringPreviousPower 挪到 :1488 旁每 hop 更新（h=0 逐位不变，grep 全部读写点 :423/:1722/:1733 核过）；规格 :47 改「本段 60 ms 快电平」、ADR-020 决定里补一句。验证除 R2 两条新用例外加「镲片式 150 ms 突发」观察尾巴（quick 对 60～200 ms 亮瞬态更快反应，55 ms 起音保持只盖前 55 ms）。
- 风险：h=0 / 旧工程 / GLAZE 0 逐位不变（:1690、:1720 双重守卫）；ADR-013 不涉及。行为变化：判据对短亮瞬态更敏感；h>0 的 GLAZE 校准缓存（Mast 哈希无算法版本）会静默失效，仅 0.1.9.0 本机会话。「transient-preserve」用例应更稳（突发期削减保持）。
- 验证：新增 AdvancedMasteringTests：粉噪 −20 dBFS、harshAmount=1，6 s 处整体 −10 dB 阶跃，断言其后 2 s 内 getHarshReductionDb() 峰值 < 0.3 dB（现 2.99）；鼓点式脉冲断言 2 ～ 8 kHz 段 G 均值 > −0.3 dB；保留「persistent 3.15 kHz peak」用例证明真刺耳仍被削。harshAmount = 0 时逐位摘要用例必须不变。

### R2-05 · 上行掩蔽的距离 4 永远触发不了（需间隙 ≥ 18.7 dB，上限 16 dB）：白跑一轮、与规格「1 ～ 4 段」不符
- 严重度 / 优先级：low / P1 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/dsp/BalanceModule.cpp:1611`
- 证据：`maskingMaxDistance = 4; maskingFloorOffsetDb = 2.0; maskingUpwardSlopeDb = 4.0; maskingMinBuriedDb = 0.70; maskingMaxGapDb = 16.0;`（:112-120）；`if (gap <= 0.0 || gap > maskingMaxGapDb …) continue;`（:1608）；`buried = gap − 2 − 4·distance`（:1611）；`if (buried < maskingMinBuriedDb) continue;`（:1613）。d=1 需 gap ≥ 6.7，d=2 ≥ 10.7，d=3 ≥ 14.7，d=4 ≥ 18.7 > 16 → 空集。规格《设计规格-Balance》2026-09-28 节与 docs/reference-chain-algorithm.md 都写「往上 1 ～ 4 段」。
- 机理：算术上 d=4 分支永远 continue：每个施压段每 hop 多跑一轮无效循环，规格声称的 4 段接收范围实际是 3 段。
- 研究修法：(a) 推荐：`maskingMaxDistance = 3`，规格改「1 ～ 3 段」——逐位不变；(b) 若确实要 4 段外接收，`maskingUpwardSlopeDb = 3.0`（d=4 需 gap ≥ 14.7），改变输出，需重跑掩蔽测试。
- 核验理由：常量 :112-120、循环 :1594-1620：d=4 需 gap ≥ 18.7 > 16（:1608 已 continue），该轮从 band 到 buried 全是局部量无副作用，改 3 逐位不变（算术复算 6.7/10.7/14.7/18.7）。更正：docs/reference-chain-algorithm.md:36-38 没写「1～4 段」，只有设计规格-Balance.md:123（T-029 按代码补记）。
- **核验调整后的修法（以此为准）**：maskingMaxDistance = 3 + 规格 :123 改「1～3 段」；可随 R2-01 顺手提交。「upward masking」用例照跑；逐位需比对 logged digests 前后日志（该用例不断言黄金值）。
- 风险：无（逐位不变）。
- 验证：(a) 下「upward masking lifts a buried higher neighbour…」用例与逐位摘要用例（BalanceTests.cpp:2376「logged digests」）必须逐位相同。

### R2-06 · 每 hop 热路径的重复超越函数（pow 在段循环里算 27 次、log10 与已有 quickLevelDb 重算 46 次、A 计权权重与 4 个 exp 系数每 hop 现算）与功率谱累加多做 25 %（48 k）～ 81 %（192 k）的无用 bin
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/dsp/BalanceModule.cpp:1250`
- 证据：`const auto commonPower = std::pow (10.0, commonLevelDb * 0.1);` 在 27 段循环体内（:1250，锁定分支 :1240 同）；`10.0 * std::log10 (juce::jmax (powerFloor, quickPower[b]))`（:1705、:1744）与 :1177 的 quickLevelDb[b] 表达式逐字相同且 updateTrackers 先执行；`maskingLoudnessWeight (band)`（:127-137，pow+sqrt+log10+pow）在锚定时每 hop 对 ≤21 段现算（:1656）；`std::exp (-hopSeconds / …)` 每 hop 现算（:1627、:1713-1715）；`std::fill (powerSpectrum…)`（:950）与 accumulatePower（:1021-1026）遍历 fftSize/2+1 个 bin，而段只覆盖到 bin ≈ 770（17.96 kHz，各采样率 bin 都 23.4 Hz），谐波保护最多读到 +3：48 k 1025 对 773，192 k 4097 对 773。
- 机理：同一输入的确定性表达式重复求值；累加循环处理永远不会被读的 bin。48 k 每 hop 约 160 次超越函数 ≈ 2 ～ 4 µs（0.03 % 单核）；192 k 累加省 ≈ 3.3 k × 2 声道次乘加 ≈ 3 ～ 6 µs，落在最坏块（分析片 1/2）里。量级不大，但零风险。
- 研究修法：commonPower 移到循环前算一次；:1705/:1744 直接读 quickLevelDb[b]；prepare 时算 PerBand<double> maskingWeight 与 harsh/target/boost 三组 hop 系数（同 attackCoeff 的做法）；prepare 时存 `analysedBinCount = bandFirstBin[26] + bandNumBins[26] + 3`，fill 与 accumulatePower 上限改用它。
- 核验理由：五处全核到：:1250/:1240 pow 在段循环内且 commonLevelDb 循环内不变（重定中心在 :1279 之后）；:1705/:1744 log10 与 :1177 逐字同、runNextAnalysisSlice :983-989 先 updateTrackers 且中间无人写 quickPower；maskingLoudnessWeight :1656 每 hop 现算；exp :1627/:1713-1715 每 hop 现算；powerSpectrum 读取点 grep 全在段 bin+3 内（:1037、:1322-1333，measureBandCross 读帧缓冲），上限 bandFirstBin[26]+bandNumBins[26]+3 安全。量级估计合理。
- **核验调整后的修法（以此为准）**：按 R2 做；targetCoeff 预算两份（2.0 s / 0.15 s）。验证注意 BalanceTests:2376「logged digests」只 logMessage 不断言黄金值，要人工比对改前改后摘要或临时加黄金值。
- 风险：无（同表达式同 libm 逐位相同；clang -ffp-contract=on 对函数调用不合并）。
- 验证：逐位摘要用例（BalanceTests.cpp:2376）、拆片 = 一口气、打包 = 标量、bandCross 逐位用例全部逐位相同；BalancePerformanceTests 的 192 k/32 最坏块数字只降不升。

### R2-07 · 系数重算间隔固定 64 样本不随采样率缩放：192 kHz 每秒 81 k 次 designPeak（pow + 5 次除法），增益粒度也比 48 k 细 4 倍而无收益
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/BalanceModule.cpp:44`
- 证据：`constexpr int coefficientUpdateInterval = 64;`（:44）；hop = fftSize/4 随采样率翻倍（:260-266，192 k → 2048）而间隔不变，updateCoefficients（:872-907）每次对增益有变的段调 designPeak（1 次 pow + 5 次除法 + 状态归零检查）；拆片间隔 = 间隔/2（:956）。修正在动时 27 段每次都变：192 k 每秒 3000 × 27 = 81 k 次 ≈ 3 ～ 4 ms/s ≈ 0.3 ～ 0.4 % 单核，与 Balance 在 48 k 的整块开销（v0.5 实测 0.208 %）同量级；每步 ≤ 0.03 dB 的粒度无可闻收益。
- 机理：时间粒度应随采样率保持 1.33 ms 不变，现在按样本数固定，高采样率下系数重算频率与拆片间隔都按倍数上涨。
- 研究修法：`coefficientUpdateInterval = 64 << (order − baseFftOrder)`（prepare 时定，成员变量），拆片间隔取其一半；两者仍整除 hop、hop 边界仍落在重算网格上，三片仍在 hop 后第 0 / ¼ / ½ 个间隔。44.1/48 k 逐位不变，≥ 88.2 k 改变。
- 核验理由：coefficientUpdateInterval=64 :44、hop 随采样率 :260-266、designPeak 1 pow+5 除 :854-870 只在增益变时调 :879-885、拆片间距 :956、同一样本上片先于重算 :583-590 全核到；192k 每秒 81k 次约 0.3～0.5 % 单核估计合理。规格 :40 与 v0.3 计划细节表写「64 样本」，只按 48k 考虑，不是 Max 单独拍板；属低效非缺陷。
- **核验调整后的修法（以此为准）**：interval = 64 << (order − baseFftOrder) 存成员，拆片间距取一半（仍整除 hop，末片与首次用新目标的重算同样本且先执行，192k/32 三片仍各落一块）；规格 :40 改「每 1/8 hop（48k 下 64 样本）」。
- 风险：≥88.2k 输出改变（增益台阶 0.33→1.33 ms，与 48k 相同）；44.1/48k 逐位不变；性能用例门槛复核。
- 验证：「output independent of block size at 192 kHz, including tiny blocks」「analysis sliced … bit-exact with one-shot」「packed … bit-exact with scalar」用例照跑；性能用例「analysis slicing: worst block at 192 kHz / 32 samples <= 65 %」门槛复核；48 k 逐位摘要不变。

### R2-08 · 起音保留把已施加的削减硬乘 (1 − p) 再硬乘回来、无平滑：真实鼓点每秒触发 9 ～ 10 次，h=1、p=0.5 时 2 ～ 8 kHz 削减以 5 ～ 10 Hz 在 50 % ↔ 100 % 之间跳（1.5 dB 台阶）
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/BalanceModule.cpp:1738`
- 证据：`const auto onsetPermit = masteringOnsetHold[b] > 0.0 ? 1.0 - transientPreserve : 1.0;`（:1728，进包络目标）；`const auto appliedHarsh = harshEnvelopeDb[b] * (masteringOnsetHold[b] > 0.0 ? 1.0 - transientPreserve : 1.0);`（:1738-1739，绕过包络直接乘）。实验（Balance 口径的段功率）：粉噪鼓点式脉冲 4 次/s 真实起音 → `P > 1.7·P_prev` 每秒触发 10.08 / 9.42 / 9.25 次（2 k / 4 k / 8 kHz 段，每次起音带 2 ～ 3 个重叠帧）；平稳白/粉噪误触发 2 kHz 段 0.6 ～ 0.9 次/s、其余 ≈ 0。包络 3 dB 时台阶 ±1.5 dB，只受每 hop 1 dB 限速与 64 样本系数台阶约束。
- 机理：包络本身有 25/200 ms 攻击释放，但 appliedHarsh 在保持开始与 55 ms 后结束的那一 hop 各跳一次倍数；持续刺耳 + 鼓点的素材上形成 5 ～ 10 Hz 的颤音式调制，不是「保留起音」。R2-01 修后纯鼓点素材包络接近 0，本条只剩真刺耳素材。
- 研究修法：删掉 :1738 的第二次硬乘，只保留 :1728 进包络目标的 onsetPermit，让 25 ms 攻击 / 200 ms 释放塑形（起音到来时目标瞬降到 (1−p) 倍、包络以攻击速度跟下去，比硬切慢约两个 hop，仍快于任何鼓的 decay）；或给 onsetPermit 单独 5 ～ 10 ms 一阶平滑。
- 核验理由：硬乘确在 :1738-1739，保持结束一 hop 内乘数 (1−p)→1 跳约 1.3 dB（h=1、p=0.5、包络 3 dB）。但主修法方向反：目标下降走释放 200 ms 系数（:1734 harsh>env ? attack : release），删硬乘后 55 ms 内包络只掉 24 %，起音闪避近乎失效，违背代码注释 :1736 与规格 §4 :50「已在施加的削减也立刻乘 1−p」。台阶后还有每 hop 1 dB 限速 :1760 与 64 样本插值 :879，实际斜率 ≤1 dB/10.7 ms 非硬切；「5～10 Hz 可闻颤音」无听感证据（可闻性 uncertain）。
- **核验调整后的修法（以此为准）**：保留起音侧硬乘（立刻闪避），只给恢复侧加一阶 10～20 ms 平滑（乘数从 1−p 爬回 1），或对乘数做「降瞬时、升 10～20 ms」的单独平滑；h=0 逐位不变；保留「transient-preserve」用例，新增相邻 hop 差 ≤0.6 dB 断言。R2-01 修后本条只剩持续刺耳+密集鼓点素材。
- 风险：若按 R2 主修法删硬乘：起音保留功能削弱 4 倍（用例 1.002 门槛可能仍过，掩盖退化）。
- 验证：保留「transient-preserve changes burst attack samples while retaining steady protection」用例；新增断言 getHarshReductionDb() 相邻 hop 差 ≤ 0.6 dB（现可到 1.5）；harshAmount = 0 时逐位不变。
