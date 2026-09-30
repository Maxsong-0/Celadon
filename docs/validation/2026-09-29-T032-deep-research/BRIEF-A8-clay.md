# T-032 实现任务书 · A8-clay · Clay 修坯峰值守护

> 生成于 2026-09-29 23:0x，来源：研究代理 R*.md 与核验代理 V*.md（同目录）。逐条以「核验调整后的修法」为准。

## 你负责的文件（只许改这些）

Source/dsp/ClayModule.{h,cpp}；tests/ClayTests.cpp、tests/ClayGlazeTests.cpp；Source/ui/HelpText.cpp（只改守护相关三处文案 414 / 437 / 463 附近）、tests/HelpTextTests.cpp（若文案断言需要）；档案 01-框架/设计规格-Clay-根治修订.md §6（末尾加「2026-09-29 修订（T-032）」小节）。ADR-014 的修订由主进程写。

## 主进程补充要求

- R4-01 做 A'（拆分增量：守护只管①低切②收单，③加宽的增量在守护之后加回），**必须按核验的两处细节**：第 6 步写成 delta != 0.0 ? guarded + delta : guarded（保住 ±0 逐位）；delta = shelfWasIdle ? 0.0 : wideSide − monoSide，纯延迟分支显式置 0（避免 ±Inf 相减 NaN）；processed 含 delta != 0；Frame 的 delta 用 float；旁通 delta·(1−bypass)；processMono 恒 0。验收门：widthDb = 0 全场景（含特殊值、随机块长）新旧逐位对照 0 差异——在改之前先用现版本把一批固定种子输入的散列跑出来存进报告，改后再比。
- R4-02 阈值按固定种子夹具记录现值 ± 余量钉死，注释写明素材相关；加宽用例按 A' 后期望 widthDb ± 0.3 dB，现版本阳性对照必须失败（先在改前跑一遍确认）。
- 核验漏项：同一机理对手动收单一样成立（78 % 样本 g<1、效果只剩 22 %），A' 修不到——不改，报告里列「待 Max：② 是否也拆出守护」。压力格对效果丢失失明、R4-03 指示灯——不做，报告列待认领。

## 逐条结论

### R4-01 · 峰值守护把加宽几乎全部回收：局部峰值样本上界零余量，g_req 恰为 0
- 严重度 / 优先级：high / P0 → 核验 high / P0；工作量 M；改输出：是；核验判决：confirmed
- 位置：`Source/dsp/ClayModule.cpp:1129`
- 证据：ClayModule.cpp:1129 bound = max(localPeak, max(τ·env, F))；:1149-1152 limit = (bound − sgn(d)·x)/|d|——当 k 是 ±8 ms 窗的最大值样本时 bound = |x| → limit = 0；:1197-1221 100 ms 线性释放 + 3 ms 滑动平均。真 ClayModule 实测（107，工作树源码 md5 一致，48 kHz/512，20 s，> 4 kHz 侧声道能量增益 out/dry）：宽密粉噪 −12 dBFS 手动 +3 dB → +0.36 dB（同素材 −44 dB 时 +2.94）；+1.5 → +0.17；+0.5 → +0.05；窄（S/M −14 dB）+1.5 → +0.22；纯 S 16 kHz 幅度 0.5 +3 dB → 0.00 dB、+0.5 dB → 0.00 dB；g < 1 样本 99.9 %、最小 g 0；压力格均值 0.011～0.131 dB < 0.2 门槛。独立模型给出相同数字。T-024 REPORT:130「加宽 +1 dB 时 100 % 样本 g < 1」当时只当性能备注。测试：ClayTests.cpp:266-275 quietScale −44 dB、:382-395 measureSide 压到地板下、:1594 守护用例把加宽只当阳性对照；ClayGlazeTests.cpp:1471 只断言 s.clayWidthDb > 0 不量输出。文案 HelpText.cpp:414/437/463「峰值处自动让一点」「修坯从不让峰值变高」；ADR-014 第 26 行「手动设置在峰值瞬间也会让一点」；根治修订第 14 行原意「加宽……交给先试后用」。
- 机理：E(k) = 干路两声道 |x| 在 [k−W, k+W] 的最大值；凡本身就是窗最大值的样本 k*，B = E = |x(k*)|。加宽让 L = M + γS 在 S、M 同号处变大，而 |L| 成为局部最大时 S 与 M 几乎总同号，于是 |y(k*)| > B 且 g_req = 0/|d| = 0，与增加多少无关。宽带信号每约 16 ms 出现一个局部最大值，g 从 0 回到 1 要 100 ms，g 从不回到 1（平均 0.10～0.14），加宽只剩 5～14 %。压力格量 max|y|/B 很小，压力反馈不降档，界面照亮「宽」灯并显示 +3 dB。低切 / 收单是去能量、越界只来自相位错开，回收小得多（收单 −20.4→−20.4 dB，低切 25 Hz 对 15 Hz 次声 −9.0→−7.8 dB），守护对它们合理。
- 研究修法：推荐 A'（拆分增量，守护只管①②，管线与延迟不变）：processStereo（ClayModule.cpp:697-725）算出 wideSide 后 sideDelta = wideSide − monoSide（shelfWasIdle 时精确 0）；y12 = delayedMid ± monoSide；旁通淡化作用在 y12（y12 += (dry − y12)·bypass）与 delta = sideDelta·(1 − bypass)（bypass ≥ 1 → 0）；io 写 y12，按段暂存 deltaL = +delta、deltaR = −delta（std::array<double,64>）。PeakGuard::Frame 加 std::array<double,2> delta，process 多收 const double* const* delta，processBlock 第 1 步存环、第 6 步 out = guarded + atJ.delta[c]（delta = 0 → x + 0 逐位 = x），processed = !sameBits(x,y12) || delta != 0。processMono delta 恒 0。逐位契约保留：widthDb = 0 → 与现版逐位相同；三项为 0 → 逐位纯延迟 D+G；旁通 → delta 0。文档：ADR-014 补记守护只管①②、③靠先试后用代价 + 相关度上限 + ≤ +1.5 dB；ClayModule.h 文件头与 HelpText 三处文案改。放弃的替代：给 E 加 0.5～1 dB 余量——加宽恢复但低切守护同时削弱（低音密素材 25 Hz 低切 g<1 80 % → 31 %、压力 0.49 → 0.10 dB，压力反馈不再降档，正是 ADR-014 要防的 WTF 场景）。属修正 ADR-014 一条假设，请主进程报 Max 点头后落地。
- 核验理由：独立复现（107，真 ClayModule，源码 md5 与工作树一致）：宽粉噪 −12 dBFS 手动加宽 +3 dB 标称 +2.97、实际 +0.25 dB；+1.5 → +0.12；窄歌 +1.5 → +0.19；纯 S 16 kHz +3 → 0.00 dB；g<1 占 99.9 %、最小 g 0。代码逐行对上：ClayModule.cpp:1129 bound = max(E, τP, F)、:1151 limit = (B − sgn(d)x)/|d| 在自窗最大值样本上恰为 0、:943 释放 100 ms、:1152 压力只量 max|y|/B。ADR-014 预设「峰值处让一点」与实测量级不符，属假设错误而非拍板取舍；测试全在 −44 dB 地板下（ClayTests.cpp:266-275/382-395/1594/1694、ClayGlazeTests.cpp:1471）核实；HelpText 414/437/463 文案核实。
- **核验调整后的修法（以此为准）**：A' 方向正确（③ 增量在解码后可分离；根治修订 §1 原意；ADR-014 弃项「只管自动态」是按模式不是按级，不冲突），但须报 Max 点头（改决定 1「|输出| ≤ B」为「|输出 − ③增量| ≤ B」）。落地必须改两处报告写错的细节：(1) 第 6 步不能无条件 guarded + delta——(−0.0)+(+0.0)=+0.0，ClayTests.cpp:280-301 特殊值含交替 ±0、:796/:1074/:1379/:1721 断言逐位纯延迟，要写成 delta != 0.0 ? guarded + delta : guarded；(2) delta 写成 shelfWasIdle ? 0.0 : wideSide − monoSide，纯延迟分支（ClayModule.cpp:707-711）显式置 0，否则 ±Inf 样本 Inf − Inf = NaN。其余照报告：processed 含 delta != 0、Frame 的 delta 用 float（环足迹不翻倍）、旁通 delta·(1−bypass)、processMono 恒 0；widthDb = 0 全场景（含特殊值、随机块长）新旧逐位对照 0 差异为验收门；改 ADR-014 决定 1 与后果、ClayModule.h 文件头、HelpText 三处、根治修订 §6。
- 风险：只改 widthDb ≠ 0 的输出；22 首门槛自动加宽从未触发、GLAZE 0 与现有逐位契约不变，回归面小。风险在实现细节：−0.0 / Inf 不按上面处理会打破逐位用例；不守护 ③ 后手动 +3 dB 在宽歌上峰值最多 +0.5 dB（我的数据），自动 ≤ +1.5 dB 只在窄歌 +0.04 dB，由 widthPeakCostDb 兜。
- 验证：新用例 ClayTests：−12 dBFS 宽粉噪 + 手动加宽 +3/+1.5/−3 dB，延迟对齐后 > 4 kHz 侧声道能量增益 = widthDb ± 0.3 dB（阳性对照：现版本 +0.36/+0.17 必须失败）；纯 S 16 kHz 幅度 0.5 +3 dB → 输出侧声道 +3.0 ± 0.1 dB（现版本 0.00）。现有守护上界用例（ClayTests.cpp:1579-1660）改为对①②路断言（params width 置 0 或断言 |out − delta| ≤ B），阳性对照仍由低切 60 Hz + 收单提供。ClayGlazeTests GLAZE 60 用例（1436-1520）加「自动链输出高频侧声道能量比全关高 ≥ 0.6 × widthDb」。widthDb = 0 的全部现有用例与 T-024 guard_compare 思路并排逐位对照必须 0 差异。

### R4-02 · 测试缺口：三级经守护后的正常电平效果没有任何用例（机制用例全在 −44 dB 地板下）
- 严重度 / 优先级：medium / P1 → 核验 medium / P1；工作量 S；改输出：否（逐位不变）；核验判决：confirmed
- 位置：`tests/ClayTests.cpp:269`
- 证据：ClayTests.cpp:266-275 quietScale = 10^(−44/20)，注释「量到的是③本身的增益（ADR-014）」；:382-395 measureSide 全部压到地板下；:850-885 收单用例、:1203-1224 加宽用例同样；ClayGlazeTests.cpp:1436-1520 只断言 Status（:1471）；ClayGuardTests 全是 ClayGuard 单元不含守护。R4-01 这种「状态说在做、输出没做」在 129k 条测试里零命中；EXP-C2 还量到手动收单 120 Hz 在中等宽低音（S/M −10 dB）上标称 −22.2 dB、实际 −11.7 dB（守护 22 % 样本），无用例记录。
- 机理：ADR-014 落地时为让核心机制用例不受守护干扰，把素材整体压到 −40 dBFS 地板之下，等于把守护从所有端到端用例里拿掉；守护只剩「上界不越」与「稳态正弦不动」两条自证，无法发现守护对某一级的系统性回收。
- 研究修法：加一组「正常电平特性用例」（−12 dBFS，20 s，真模块）：加宽（同 R4-01）；低切（低音密素材 25 Hz 对 15 Hz 次声：实际衰减 ≥ 标称 − 1.5 dB，记录现值 −7.8/−9.0 dB）；收单（中等宽低音 120 Hz：实际 ≥ 标称的 50 %，记录 −11.7/−22.2 dB）。每条打印标称 / 实际 / g<1 占比，阈值按 R4 报告第 5.3～5.4 节数据定，作回归护栏。
- 核验理由：核实：机制用例全走 quietScale（ClayTests.cpp:266-275/382-395/850-885/1203-1224）；ClayGlazeTests.cpp:1436-1500 只断言 Status；ClayGuardTests 无 PeakGuard 引用；唯一正常电平过守护的端到端用例 ClayGlazeTests.cpp:2205-2234 量的是限幅 GR（伤害）不是效果。
- **核验调整后的修法（以此为准）**：加正常电平特性用例对；但报告的收单阈值「实际 ≥ 标称的 50 %」是其自家素材的数——我的素材（S/M −10 dB、侧声道与中声道独立）上只有 22 %（−4.9/−22.6 dB），不能写成通用规则。阈值按固定种子夹具记录现值、按现值 ± 余量钉死，注释写明素材相关；加宽用例按 A' 后期望 widthDb ± 0.3 dB、现版本阳性对照必须失败。
- 风险：低；用例本身。阈值若写成通用比例会在换素材时误报。
- 验证：用例本身；先在现版本跑一遍确认加宽用例失败、其余两条通过并记下数值，再随 R4-01 一起上服务器全套。
