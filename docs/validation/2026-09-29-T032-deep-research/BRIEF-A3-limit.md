# T-032 实现任务书 · A3-limit · Limit 限幅链（TruePeak / LimiterModule）

> 生成于 2026-09-29 23:0x，来源：研究代理 R*.md 与核验代理 V*.md（同目录）。逐条以「核验调整后的修法」为准。

## 你负责的文件（只许改这些）

Source/dsp/TruePeak.{h,cpp}、Source/dsp/LimiterModule.{h,cpp}；tests/LimiterTests.cpp、tests/DspPartsTests.cpp；档案 01-框架/设计规格-Limit.md（末尾加「2026-09-29 修订（T-032）」小节）

## 主进程补充要求

- **R1-01 的当晚替代（核验漏项 (a)）**：不做 AVX2（要运行时分派 + 三工具链单独 TU + 109 今晚连不上没法验 MSVC）。改做 SSE2 预广播表：tapsByTap 改成每抽头 7×4 个相同 float 的预广播表（7 KB），SSE2 核每抽头 7 次 _mm_load_ps 代替 2 次 load + 7 次 shufps，运算序列不变；核验在 107 实测 28.11 → 17.99 ns（1.56×）、0 失配。DspPartsTests 逐位矩阵（4 种 MXCSR × 信号 × 分块）必须照过，性能护栏用例更新数字。NEON / 标量路径不动。AVX2 写进报告「后续」。
- R8-02 只做 Limit 部分：LimiterModule::setParams 各字段 isfinite 守卫（Ceiling 下限 −4.5；lookaheadMs 只 isfinite 回默认不再钳）；处理器与 Color 部分归别的代理；链级 NaN 参数用例归 A4。Limit 级用例写在 LimiterTests。
- R1-02 必须附「随机窗长 vs std::min_element 逐样本对照」用例；R1-03 没有黑盒可观测差异，靠现有用例逐位兜底并在报告写明。

## 逐条结论

### R1-02 · SlidingMinimum 每样本 2～3 次整数取模 + 数据相关循环：van Herk 块算法快 3.4～4.3× 且逐位相同
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/dsp/LimiterModule.cpp:109`
- 证据：LimiterModule.cpp:116 (head+count-1) % capacity、:124 (head+count) % capacity、:132 head = (head+1) % capacity；capacity = maxLookahead+64（:97、:223）不是 2 的幂。E5（107，W=159 = 2 ms@48k + 63）：类增益输入 9.5～10.7 ns/样本（取模）、5.8 ns（掩码）、2.8 ns（van Herk）；随机输入 13.0～13.6 对 2.8～3.1 ns；两种替代各 4 M 样本 0 失配。
- 机理：整数除法 20～40 周期；两个 while 循环在噪声般的增益序列上分支预测差。van Herk/Gil-Werman：当前块前缀最小值 1 次 min，上一块后缀最小值每 W 样本算一次（均摊 1 次），合并 1 次，无分支、与窗长无关。
- 研究修法：用两缓冲 van Herk 流式最小值替换 SlidingMinimum：push(v)：block[pos]=v; prefix = pos ? min(prefix,v) : v; r = min(prefix, suffix[pos+1]); if (++pos==W){ suffix[W]=1; for i=W-1…0: suffix[i]=min(suffix[i+1],block[i]); pos=0;} return r。初始 suffix 全 1.0 = 无穷 1.0 历史，与现在 clear() 后行为相同；setWindow 后照旧 clear()。float min 对相等值对称，与现在弹出相等元素的语义等价（实测 0 失配）。折中方案：capacity 取 2 的幂用掩码代替 %（5.8 ns）。
- 核验理由：LimiterModule.cpp:116/:124/:132 三处 % capacity，capacity = maxLookahead + 64（:97、:223）非 2 的幂，:114-122 与 :130-134 两个数据相关 while；required ≤ 1.0 恒成立（:840）所以 van Herk 用 1.0 当无穷历史与 clear() 等价。107 独立复现：60 次随机窗长 × 20000 样本、增益状与随机输入，现有实现 vs van Herk 0 失配、两者 vs 暴力 min 0 失配（120 万样本）；W = 159 基准 9.36 → 2.76 ns（3.4×）、随机 13.68 → 2.76 ns（5.0×）。
- **核验调整后的修法（以此为准）**：修法可用（两缓冲 van Herk，setWindow 后照旧 clear，suffix 尺寸 maxWindow+1 放 1.0 哨兵），必须附研究说的随机窗长 vs std::min_element 用例。但要说明收益：控制路每样本一次，48k 省约 0.32 ms/s = 0.03 % 一个核，192k 0.13 %；属「顺手改」级别，不值得单独立项；折中方案 capacity 取 2 的幂用掩码是一行改动（研究测 5.8 ns）。
- 风险：极低：float min 是选择运算、逐位可验证；van Herk 每 W 样本一次 ≤ 543 次 min 的小突发（< 1 μs），无实时风险。
- 验证：现有 output independent of block size、gain returns exactly to 1.0、null、16× 参考用例；新增随机单元测试：窗长在 1…maxLookahead+63 间随机切换并与暴力 std::min_element 逐样本对照。

### R1-03 · ceilingDelay 重置时填 0 而不是当前 Ceiling：今天被延迟线同步清零掩盖，是潜在静音陷阱
- 严重度 / 优先级：low / P2 → 核验 low / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/dsp/LimiterModule.cpp:318`
- 证据：LimiterModule.cpp:317-318 ceilingDelay.setDelay(coreDelay); ceilingDelay.clear();（DelayLine.h:33-36 clear = fill(0)）；同一函数 :319-320 gainDelay 却 fill(gainSmoothed.getCurrentValue()) 并有注释，reset() 里 :269 再填一次；:866 ceilingAtOutput = ceilingDelay.pushPop(ceiling)，:872 jlimit(-ceilingAtOutput, ceilingAtOutput, delayed*gain) → 每次 resetLimiterState 后前 coreDelay 个样本钳位是 ±0、输出被强制为 0。今天无害只因 :315 coreDelays 同处清零、:303-309 restartWithFadeIn 又静音整个延迟长度。
- 机理：两条参数延迟线处理不对称；任何把「换前瞻不清音频延迟线」或改重置顺序的后续改动都会在这里静默丢掉 coreDelay 个样本，测试只会看到「多静音一段」。另 reset() 里 :262 的 resetLimiterState 先于 :270 的 ceilingSmoothed 落位，所以要像 gainDelay 一样两段式填。
- 研究修法：resetLimiterState() 里改为 ceilingDelay.fill(ceilingSmoothed.getCurrentValue())；reset() 里在 ceilingSmoothed.setCurrentAndTargetValue(ceiling) 之后再 ceilingDelay.fill(ceiling)。
- 核验理由：resetLimiterState() :317-318 ceilingDelay.setDelay + clear()（DelayLine.h:33-36 clear = fill(0)），同函数 :319-320 gainDelay 却 fill(gainSmoothed.getCurrentValue()) 且 reset() :269 二次填；:866 ceilingAtOutput = ceilingDelay.pushPop(ceiling)，:872 jlimit(±ceilingAtOutput, delayed×gain)。ceilingDelay 全文只有 :214、:317-318、:866 三处，清零只发生在 resetLimiterState，同处 :315 把 coreDelays 也清零且同长——前 coreDelay 个输出 delayed = 0，钳到 ±0 与 ±c 都是 +0，所以今天逐位无差、是潜在不对称；研究说的两个掩盖条件（延迟线同步清零、restartWithFadeIn 静音整个延迟长度 :303-309）属实。
- **核验调整后的修法（以此为准）**：修法可用且输出逐位不变：resetLimiterState() 里 ceilingDelay.fill(ceilingSmoothed.getCurrentValue())、reset() 里 :270 之后再 fill(ceiling)（首次 prepare 时 Multiplicative SmoothedValue 默认初值 1.0，juce_SmoothedValue.h:246，随后被二次填覆盖）。但研究给的验证用例无效：重置同时清空 coreDelays，前 coreDelay 个输出的 delayed 是 0，无论钳到 ±0 还是 ±Ceiling 都是 0，修前修后一样——这条没有黑盒可观测差异，只能靠代码评审 + 现有用例（LimiterTests.cpp:1013 restartWithFadeIn、:940 lookahead change、:639 reset == fresh prepare、:917 分块无关）保持逐位兜底。
- 风险：几乎无：纯代码卫生，防将来「换前瞻不清音频延迟线」类改动静默丢 coreDelay 个样本。
- 验证：现有 restartWithFadeIn / lookahead change / reset == fresh prepare 用例保持逐位（0×g 钳到 ±0 与 ±c 都是 +0）；新增用例在测试构建里把 fadeInHoldSamples 置 0 后紧接重置送 +6 dB 阶跃，前 coreDelay 个输出应钳在 ±Ceiling 而不是 0。

### R8-02 · Limit / Color 参数不拦非有限值：NaN 经 XML "nan"→APVTS→readParams 原样进模块，释放时间 NaN 让限幅器退化成硬削波、Tone NaN 逐块 NaN 重启、Gain/Ceiling/Output NaN 永久静音
- 严重度 / 优先级：medium / P1 → 核验 medium / P2；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`Source/dsp/LimiterModule.cpp:361`
- 证据：LimiterModule.cpp:338-361 setParams 无 isfinite（345 `decibelsToGain (params.gainDb)`、361 `manualReleaseCoeff = timeConstantCoeff (sampleRate, params.releaseMs)`）；ColorModule.cpp:609-613 用 jlimit 钳制而 jlimit 对 NaN 原样返回（juce_MathsFunctions.h:551-560）；PluginProcessor.cpp:352-367 readParams 对 Color/Limit 直接 load()；CeladonChain.cpp:105,112 不清洗。对照已守卫的 BalanceModule.cpp:450-468、ClayModule.cpp:379-386、CeladonChain.cpp:114/131、PluginProcessor.cpp:441/458。JUCE 9.0.2：juce_CharacterFunctions.h:257-260 把 "nan" 解析成 quiet_NaN；juce_NormalisableRange.h:136-139/160-162 只 clampTo0To1（=jlimit）；juce_AudioProcessorValueTreeState.cpp:101-105 setDenormalisedValue 只 approximatelyEqual 判等（NaN 恒假）后直写；juce_AudioParameterFloat.cpp:98 setValue 不钳。107 实测：juce::String(NaN) 写出 "nan" 读回 isnan；logRange 与线性范围 to0to1/from0to1/snapToLegalValue 对 NaN 全 isnan；链上 +24 dB、Ceiling −1 dBTP：limit.releaseMs=NaN（手动释放）→ 0 非有限块、峰值 0.8913（= Ceiling）、RMS 0.7727（正常 0.6053，只剩硬钳位在削）；limit.gainDb / ceilingDb / color.outputDb=NaN → RMS 0 永久静音；color.toneDb=NaN → 187/375 块非有限、逐块清零重启；Balance/Clay 的 NaN 按守卫回中性、输出正常。
- 机理：规格把范围钳制交给 APVTS、模块「不再信任外部值」，但 APVTS 各层只钳有限值，模块守卫只做了一半（Balance/Clay/mastering/Lookahead 有，Limit/Color 没有）。timeConstantCoeff(fs, NaN) 给 NaN 系数、包络比较全假→前瞻增益级失效只剩硬钳位；designShelf(NaN) 让 Tone 输出 NaN 触发链级 NaN 保护循环；decibelsToGain(NaN) 返回 0 → 静音。NaN 一旦被上游写坏（宿主自动化 / 界面 / 手改工程文件）会随工程保存并在重开时原样回来。
- 研究修法：三处纯函数式钳制（对有限且在范围内的值恒等）：① LimiterModule::setParams 对 gainDb / ceilingDb / releaseMs / lookaheadMs / clip 各自 `std::isfinite ? jlimit (范围) : Params{} 默认`（范围 0～24 / −4.5～0（编码余量后下限）/ 10～1000 / 1～10 / 0～100）；② ColorModule::setParams 五个 jlimit 前加 isfinite，非有限取默认；③ CeladonAudioProcessor::readParams 每个 load() 结果套 finiteOr(value, 默认) 作第二道。GLAZE 校准参考条件哈希输入不变（有限值逐位同前）。
- 核验理由：PluginProcessor.cpp:352-368 readParams 对 Color / Limit 直接 load()，:783-818 setStateInformation 不看值；LimiterModule.cpp:338-370 无 isfinite；ColorModule.cpp:606-614 用 jlimit 而 juce_MathsFunctions.h:551-560 对 NaN 原样返回（:607 注释「不信任外部值」与实现不符）；对照 Balance :447-468、Clay :375-386、链 :114/131、LookaheadHold :441/458 都守了。JUCE 9.0.2 各层放行：CharacterFunctions.h:254-260 读 "nan"；NormalisableRange.h:259-268 clampTo0To1 = jlimit；RangedAudioParameter.cpp:48-58；APVTS ParameterAdapter :101-105 / :171-177 → AudioProcessorParameter.cpp:59-63 → AudioParameterFloat.cpp:98 不钳。机理比研究更精确：timeConstantCoeff(fs,NaN)=NaN（:72-75）→ followWithRelease 首次释放 settle(NaN)（:79-91）→ envelopeManual 永远 NaN（:993）→ MovingAverage::push :169 的 jmin(1.0, NaN) 返回 1.0 → 限幅器静默失效、只剩 :872 硬钳位，正是实测 0 非有限块、峰值 = Ceiling 的来源；decibelsToGain(NaN)=0 → 静音；toneDb NaN → 链 :255-261 逐块清零重启。不改声音：GlazeController.cpp:2182 / 2145 已钳实际 gainDb / toneDb，链 :131 Ceiling 下限 −4.5，lookaheadSamplesFor :382-386 已钳；tests/ tools/ 全部 Limit / Color 取值 grep 过都在范围内（1.0e7f 只喂 GlazeControllerTests.cpp:4116 的纯函数）；校准哈希读链的 manual（:3251）不受影响；tests/ 里没有把 NaN 喂给 Limit / Color Params 的用例。优先级下调：触发要上游先写坏（宿主自动化是归一值、界面出不了 NaN，只剩手改 / 损坏工程与外部工具预设），是补齐已做一半的守卫，不该排在 R8-01 / R8-03 前。
- **核验调整后的修法（以此为准）**：按研究三处做（LimiterModule::setParams / ColorModule::setParams 各字段 std::isfinite ? jlimit(范围) : Params{} 默认；readParams 加 finiteOr 作第二道），注意：① Ceiling 钳位下限必须是 −4.5（编码余量后链会交 −4.5，按头文件注释的 −3 会破坏「codec reserve actually lowers delivered peak level」用例）；② lookaheadMs 只加 isfinite 回默认，不再钳（lookaheadSamplesFor 已钳，别动取整）；③ 真正守卫放模块，处理器层只是第二道（PluginProcessor 不在 celadon_tests）。新增用例：ChainParams 每个 float 字段分别喂 NaN / ±Inf，+24 dB 下不报非有限、非静音、真峰值 ≤ Ceiling + 0.01 dB，且与该字段取默认值的链逐位相同。
- 风险：极低。对有限且在范围内的值恒等，逐位不变；GLAZE 校准哈希输入不变；现有测试取值全在范围内。
- 验证：headless 测试：对 ChainParams 每个 float 字段分别喂 NaN / ±Inf，+24 dB 推响下 process 全程不报非有限、输出非静音、真峰值 ≤ Ceiling + 0.01 dB，并与「该字段取默认值」的链逐位相同；现有 ChainTests 零分配 / 逐位用例全绿证明有限输入不变。readParams 那一层不在 celadon_tests（处理器只进 celadon_ui_snapshot），所以真正守卫放模块。
