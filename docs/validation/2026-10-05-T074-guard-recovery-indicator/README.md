# T-074 · 修坯守护回收指示（Max 2026-10-05「1，可以」，DGX Spark 本机）

手动低切 / 收单被峰值守护回收，界面毫无提示（T-032 V4 实测：手动收单 120 Hz 在低音侧声道与中声道不相关的素材上 78 % 的样本 g < 1、效果 −22.6 → −4.9 dB；手动低切 25 Hz 在宽密素材上 59 %、−17.4 → −8.6 dB；灯全绿）。
Max 对夜间收尾报告第 1 条（「要不要把收单拆出守护，或加一个指示？」）答「可以」。「可以」没说清是哪一个：本任务做**指示**（设计稿 R4-03 已写定，V4 核验改定度量）；「把收单拆出守护」会改声音，另问 Max（编号 T-075 留着）。设计与取舍见 vault 的 ADR-032。

## 结论

- **改动**
  - `ClayModule`：守护在输出样本处顺手累计两份能量——改动量 Σd²（d = ①② 的 y − x）与守护之后剩下的 Σ(g·d)²（快路 g = 1 与慢路都算；只读 y、x、g，输出逐位不变；默认关，链里那一份打开，分析器的先试候选共用同一份 PeakGuard、不开，不多花 CPU）。
  - 编排器：新类 `ClayGuardKeepMonitor`（`ClayGuard.h`）把两份能量按 2 s 指数平滑（片长折算，与块怎么切无关），保留比 κ = kept / change；κ < 75 % 连续 1 s 亮、≥ 85 % 连续 1 s 熄；另有 0.3 s 的「有没有可听的改动」快门（每样本均方 < 1e-6，约 −60 dBFS，就把慢的两份清零、κ 记 1），停播 / 关掉 / 旁通后几秒内就熄。手动 / 自动都报，只说①②（加宽不经守护）。`Status::clayPeakGuardActive / clayPeakGuardKept`。
  - 界面：CLAY 卡片低切、收单两盏灯与数值变琥珀（加宽不动）；灯的气泡第 2 行末尾写「峰值守护回收中：低切 / 收单实际只剩约 N%」（保留比取整到 5 %，中英文）；灯的说明文案补一句。
  - 官网：说明页与算法页（中英文四份）把 T-068 写的「界面不会另行提示」改成这个指示的说明；`pnpm run build` 全过。
  - 截图工具：出图状态 `zh / en-guard-recovering`、`zh / en-guard-tooltip`（`png/`）；新检查 `runClayGuardChecks`（`--only check` 113 → **114**）。
- **阈值按实测改了**（设计稿 R4-03 写 50 % / 60 % / 300 ms 平滑）：V4 的典型坏例子（夹具 B：M / S 各自独立的粉噪，手动收单 120 Hz）保留比实测 0.554，50 % 会漏掉它；300 ms 平滑下噪声素材的 κ 在 0.54 ～ 0.98 之间来回，「连续 1 s」凑不齐，真歌上会闪。定为 75 % / 85 %、2 s 平滑 + 0.3 s 快门：夹在坏例子（0.554）与不该亮的情形（低切 25 Hz 夹具 0.978、偏置低音过 20 Hz 低切 0.917、合成音乐上低切 30 Hz + 收单 120 Hz 0.974）之间。
- **音频逐位不变**（全部在 Spark 本机，见「验证」）：全套测试与基线逐条相同；截图 `--deterministic` 30 / 30 逐字节相同；Jamendo 45 首 × 改前 / 改后的 metrics.csv 与逐块 trace 225 个文件逐字节相同；守护真动作过的 2 首（84 % ～ 87 % 的块里守护在动）成品 wav 4 / 4 逐位相同。
- **真歌上的指示**（见下表）：45 首里 43 首从不亮（低切 0、守护没动），**2 首（自动低切在做、守护回收约 2 / 3）全程亮**（99.7 % ～ 99.8 % 的块），每首只切换一次（头一次亮在第 1.0 s），没有闪烁。这 2 首里亮的是**自动**项——要不要只对手动项亮，见「待 Max」。

## 红绿矩阵（`variants_t074.py`、`scripts/t074-matrix*.sh`；输出 `outputs/redgreen.md`）

在临时副本里把实现改坏成 28 种，每种增量编译后跑 `ClayModule` / `ClayGuard` / `HelpTextClay` / `GlazeController` 四个测试类（两个改动界面灯色的再跑截图 `--only check`），再还原。对照（原样源码）：6656 过 / 0 败，截图检查 114 / 0；第二批把两处断言加强后重跑对照 6658 / 0。**第一批 21 种里 20 种红、1 种漏网**：`nan-not-sanitised`（监测器不拦非有限能量）——一个 NaN 切片会把快门包络永远毒成 NaN（比较恒假，「没有可听的改动」恒成立，灯再也亮不起来），但原来的断言只查「状态仍有限」，被快门清零的分支蒙混过去了；补了一条「坏切片之后再喂 4 s 正常切片仍要亮」，这一种随后变红。第二批 7 种（英文气泡、三行灯各自的归属、读数的前提、百分数的非有限、两种语言的灯说明）全红。

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

## 验证

| 项 | 结果 |
|---|---|
| 编译 | 改后三个目标全新目录 152 步 0 警告 0 错误；改前（main `44f7730`）三个目标、各改坏变体：0 警告 0 错误 |
| 全套测试（Spark，aarch64） | **148470 过 / 5 败**（全新目录，改后最终树；改前 main `44f7730` 是 148360 / 5）。5 条败是本机基线的同一批：ColorModule「L = R 立体声与单声道逐位相同」4 条（aarch64 NEON 写死融合乘加）+ BalancePerformance 立体声耗时比 1.7x（门槛 1.40）1 条，与本任务无关 |
| 截图 `--only check` | **114 过 / 0 败**（全新目录；113 + 新的 `runClayGuardChecks`：同一个编辑器、同一份「低切 25 Hz、收单 120 Hz 在做」的状态，守护标志 关 → 开 → 关，画 CLAY 卡片、数灯列的琥珀灯色与数值列的偏橙像素：开时低切 / 收单行各 34 个灯像素、72 / 107 个字像素，加宽行 0；关着全 0）；两个灯色改坏的变体与「读数相等判断漏了守护字段」都让它变红 |
| 截图 `--deterministic` 出图 | 改前（main `44f7730`）vs 改后：**原有 30 张逐字节相同**（`outputs/runs.out`）；另有 4 张新图（`png/`） |
| 真歌渲染（Jamendo 45 首，GLAZE 70 × 修坯 自动 / 只留低切，`--metrics-only --trace`） | 改前 vs 改后 225 个 csv（metrics.csv + 逐块 trace：含 `peak_guard_samples`、`peak_guard_min_gain`、混合损失能量、修坯实际值）**逐字节相同**（`outputs/runs.out`） |
| 真歌渲染里守护真动作过的歌（`track_0046047`、`track_0726856`） | 成品 wav 4 / 4 **逐位相同**（sha256）；这两首里低切全程在做、守护在 84 % ～ 87 % 的块里动（2537 万 / 3286 万个样本） |
| 官网 `pnpm run build` | 过（`check-i18n` + `check-svg-text` 37 项 + `tsc --noEmit` + `vite build` + 站点地图 + 中文外壳）；四份 clay.md 里「界面不会另行提示」已清除 |
| 性能（`ClayModule::process` 每立体声帧耗时，绑 CPU 5，改前 / 改后交替 3 轮取中位数；每轮内 12 遍取最快） | 改后·统计开（链里的实际状态）每帧比改前多 0.44 ～ 0.59 ns（+2.3 ～ +3.0 %）；48 kHz 下每秒多约 21 ～ 28 µs（单核的 0.0021 ～ 0.0028 %）；中性（idle 快路）也多 0.4 ns——每样本多一个已定的判断。数据只在 Spark（aarch64）上量，同机对照 |

| 场景（20 s 立体声，48 kHz，块 512；3 轮取中位数） | 改前 ns/帧 | 改后·统计关 | 改后·统计开（链里的实际状态） | 开 vs 改前 |
|---|---|---|---|---|
| offset bass, low cut 20 Hz (guard pulls back most of the time) | 21.04 | 21.33（+1.4 %） | 21.60（+2.6 %） | +0.55 ns（48 kHz 下每秒 27 µs） |
| offset bass, low cut 25 Hz + mono 120 Hz (manual-style, guard heavy) | 20.98 | 21.27（+1.4 %） | 21.56（+2.8 %） | +0.59 ns（48 kHz 下每秒 28 µs） |
| programme, all stages (30 Hz / 150 Hz x 0.8 / +2 dB) | 23.85 | 24.09（+1.0 %） | 24.40（+2.3 %） | +0.55 ns（48 kHz 下每秒 26 µs） |
| programme, all stages neutral (idle fast path) | 14.86 | 15.29（+2.9 %） | 15.30（+3.0 %） | +0.44 ns（48 kHz 下每秒 21 µs） |

## 真歌上的指示是什么样（临时树里给渲染工具的 trace 多写了两列 `clay_peak_guard_active / _kept`，只在临时树里，没进仓库）

GLAZE 35 / 70 × 修坯 自动 / 只留低切，`--glaze 35,70 --clay auto,lowcut`（`scripts/t074-real*.{sh,py}`）：

| 歌 | 产品 | 块数 | 低切 > 0 | 守护动作 | 灯亮的块 % | 保留比 最小 / 中位 / 最大 | 亮灭切换 | 头一次亮 |
|---|---|---|---|---|---|---|---|---|
| track_0046047 | g35_clay-auto_auto | 30652 | 30652 | 25264 | 99.7 | 0.120 / 0.375 / 1.000 | 1 | 1.0 s |
| track_0046047 | g35_clay-lowcut_auto | 30652 | 30652 | 25264 | 99.7 | 0.120 / 0.375 / 1.000 | 1 | 1.0 s |
| track_0046047 | g70_clay-auto_auto | 30652 | 30652 | 25822 | 99.7 | 0.095 / 0.331 / 1.000 | 1 | 1.0 s |
| track_0046047 | g70_clay-lowcut_auto | 30652 | 30652 | 25822 | 99.7 | 0.095 / 0.331 / 1.000 | 1 | 1.0 s |
| track_0155328 | g35_clay-auto_auto | 27691 | 0 | 0 | 0.0 | 1.000 / 1.000 / 1.000 | 0 | — |
| track_0155328 | g35_clay-lowcut_auto | 27691 | 0 | 0 | 0.0 | 1.000 / 1.000 / 1.000 | 0 | — |
| track_0155328 | g70_clay-auto_auto | 27691 | 0 | 0 | 0.0 | 1.000 / 1.000 / 1.000 | 0 | — |
| track_0155328 | g70_clay-lowcut_auto | 27691 | 0 | 0 | 0.0 | 1.000 / 1.000 / 1.000 | 0 | — |
| track_0726856 | g35_clay-auto_auto | 38348 | 38348 | 33265 | 99.8 | 0.007 / 0.311 / 1.000 | 1 | 1.0 s |
| track_0726856 | g35_clay-lowcut_auto | 38348 | 38348 | 33265 | 99.8 | 0.007 / 0.311 / 1.000 | 1 | 1.0 s |
| track_0726856 | g70_clay-auto_auto | 38348 | 38348 | 33236 | 99.8 | 0.007 / 0.307 / 1.000 | 1 | 1.0 s |
| track_0726856 | g70_clay-lowcut_auto | 38348 | 38348 | 33236 | 99.8 | 0.007 / 0.307 / 1.000 | 1 | 1.0 s |

- `track_0155328`（低切 0、守护没动）：指示从不亮、保留比恒为 1——没处理就不亮。
- 两首有自动低切的歌（次声 / 偏置，低切全程在做）：保留比中位数 0.31 ～ 0.38，**灯全程亮**，每首只切换一次（亮灯线 75 % 下面连续 1 s），没有闪烁。这是「守护把方案给的低切收回了大半」的如实报告；被收回的主要是 20 Hz 以下听不见的那一段（κ 是不加权的能量比）。

## 没做 / 限制 / 待 Max

- **107 / 109（x86、MSVC、22 首门槛曲）没跑**：Max 说 107 / 108 连上了——Spark 到 192.168.100.107 / .108 / .109 的 22 端口通了，但 Spark 没有任何授权的私钥（`~/.ssh` 里只有 `authorized_keys`、`config`、`known_hosts`），`BatchMode` 登录报 `Permission denied (publickey,password)`（服务器只收公钥 / 口令；没有私钥可用，口令不该由代理去输，所以没试）。本任务动的数值路径只多了只读累计，没改任何输出；x86 / MSVC 的验收等能连时补（`scripts/test-linux.sh`、`scripts/windows-wine.sh`）。
- Mac（CoreGraphics）上的灯色没实看；Windows / macOS 的字体下气泡折行位置可能不同（中文气泡在宽度限制处按字折行，会把「低切」拆在两行，生成的 `zh-guard-tooltip.png` 里就是这样，是气泡排版的通性，不是这一条改的）。
- **待 Max**：① 自动项要不要也亮（现按设计稿「手动 / 自动都报」；改成只报手动项要几行：读数里的 `peakGuard` 按低切 / 收单各自的三态（`user.lowCutAuto / monoAuto`）分别筛，灯与气泡跟着改）。② T-075「把收单拆出守护」要不要做（改声音：手动收单真生效、也可能抬峰值；`ClayTests` 的夹具钉着现值）——我的建议是先不拆，用这个指示攒一阵真实使用，看手动收单亮灯的频率再定。
