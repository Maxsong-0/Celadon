| 改法 | 做了什么 | 四个测试类（过 / 败） | 截图检查（过 / 败） | 头一条失败的断言 |
|---|---|---|---|---|
| `control（第 1 批）` | 原样源码（对照） | 6656 / **0** | 114 / **0** |  |
| `no-fast-keep` | 快路（g = 1）的样本不进两份能量 | 6651 / **5** |  | a low cut that is on changes the signal |
| `no-slow-keep` | 慢路（g < 1 可能）的样本不进两份能量 | 6647 / **9** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `kept-uses-g` | 保留量用 g 不用 g²（能量要平方） | 6651 / **5** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `report-not-reset` | 每次 process() 开头不清零（报告变成累计） | 6640 / **16** |  | the reported keep ratio is below the 75 % line |
| `keep-always-on` | 不看 reportKeep（默认关失效，分析器的先试候选也累计） | 6652 / **4** |  | reporting is off by default: no change energy -- Expected value: 0, Actual value: 38550.9 |
| `gate-missing` | 监测器没有「有没有可听的改动」的快门（低于地板也照算） | 6653 / **3** |  | mono off: the indicator clears and the keep ratio returns to 1 |
| `on-below-50` | 亮灯线退回设计稿的 50 %（会漏掉 V4 的典型坏例子） | 6653 / **3** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `no-hysteresis` | 没有滞回（熄灯线 = 亮灯线） | 6652 / **4** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `share-missing` | 平滑不按片长折算（稳态值随片长偏大，块长相关） | 6654 / **2** |  | smoothed change energy is the same for 1 ms and 10 ms slices -- Expected value  within 1e-09 of: 1, Actual value: 0.9… |
| `nan-not-sanitised（第 1 批，原断言）` | 监测器不拦非有限 / 负的能量（一个 NaN 切片永远污染平滑量） | 6656 / **0** |  | （第一批原断言下没有失败） |
| `no-validity-guard` | 监测器不拦无效的片长 / 采样率 | 6655 / **1** |  | invalid seconds / sampleRate leave the state alone |
| `prepare-no-reset` | prepare 不清监测器（链重建后带着旧的「亮」继续） | 6655 / **1** |  | a rebuilt chain (prepare) starts the guard statistics over |
| `chain-no-enable` | 链里不打开统计 | 6653 / **3** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `chain-no-copy` | 链不把统计交给编排器 | 6653 / **3** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `status-not-filled` | 状态里不填「回收中」 | 6654 / **2** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `controller-gated-by-glaze` | 只在 GLAZE 活跃时更新（GLAZE 0 的手动项不报） | 6653 / **3** |  | lit: the guard keeps well under 75 % of the manual mono (GLAZE 0 reports too) |
| `lamp-ignores-guard` | 低切灯不因守护回收变琥珀 | 6653 / **3** | 113 / **1** | 第 4 条断言（断言本身没写说明文字） |
| `readout-gated-by-auto` | 读数按三态筛（手动不报） | 6653 / **3** |  | 第 17 条断言（断言本身没写说明文字） |
| `live-line-missing` | 中文气泡不写「峰值守护回收中」 | 6652 / **4** |  | Expected value: 低切 25 Hz · 120 Hz 以下收单 80% · 高频加宽 +0.8 dB · 峰值守护回收中：低切 / 收单实际只剩约 40%, Actual value: 低切 25 Hz · 120 Hz… |
| `percent-not-rounded` | 百分数不取整到 5 % | 6651 / **5** |  | Expected value: 低切 25 Hz · 120 Hz 以下收单 80% · 高频加宽 +0.8 dB · 峰值守护回收中：低切 / 收单实际只剩约 40%, Actual value: 低切 25 Hz · 120 Hz… |
| `equality-ignores-guard` | 读数相等判断漏了守护字段（亮灭变了界面不重画） | 6654 / **2** | 113 / **1** | 第 31 条断言（断言本身没写说明文字） |
| `control（第 2 批：加强了两处断言后）` | 原样源码（对照） | 6658 / **0** | 114 / **0** |  |
| `live-line-missing-en` | 英文气泡不写「Peak guard pulling back」 | 6657 / **1** |  | Expected value: Low cut 25 Hz · Mono 80% below 120 Hz · High width +0.8 dB · Peak guard pulling back low cut / mono: … |
| `mono-lamp-ignores-peak-guard` | 收单灯不因守护回收变琥珀（只认反相提示） | 6655 / **3** | 113 / **1** | 第 5 条断言（断言本身没写说明文字） |
| `width-lamp-follows-peak-guard` | 加宽灯也跟着守护回收变琥珀（加宽不经守护，不该变） | 6657 / **1** | 113 / **1** | 第 6 条断言（断言本身没写说明文字） |
| `guard-without-stage` | 低切与收单都没在做时也报回收中（没有东西可回收） | 6657 / **1** | 114 / **0** | Expected value: 未低切 · 未收单 · 高频加宽 +0.8 dB, Actual value: 未低切 · 未收单 · 高频加宽 +0.8 dB · 峰值守护回收中：收单实际只剩约 30% |
| `percent-nonfinite` | 保留比非有限时不回 100（会写「只剩 0」之类） | 6657 / **1** |  | Expected value: 100, Actual value: 0 |
| `lamp-help-zh-missing` | 灯说明（中文）没讲新灯色 | 6657 / **1** |  | the lamp help explains the amber low cut / mono lamps |
| `lamp-help-en-missing` | 灯说明（英文）没讲新灯色 | 6657 / **1** |  | the English lamp help explains the amber low cut / mono lamps |
| `nan-not-sanitised（第 2 批，加强断言后）` | 监测器不拦非有限 / 负的能量（一个 NaN 切片永远污染平滑量） | 6657 / **1** |  | a bad slice does not poison the gate: the monitor keeps working afterwards |
