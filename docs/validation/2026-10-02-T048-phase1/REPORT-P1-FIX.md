# T-048 阶段 1 · P1F 报告：VAL1 四条 minor 的修复与补用例

> - 时间：2026-10-04 04:58～06:45（本机 PDT）。代理 P1F（Opus 5.5 max），夜间任务。
> - 工作树：`.claude/worktrees/wf_17f280af-4d7-1`。分支 `claude/t048-p1-fix`，从 `claude/t048-realtime-glaze` 的 `a0663e3` 起。
> - 提交：
>   - `2fe0c19`：修复与用例；
>   - `57fd7cd`：兜底用例的「整份」一段另把段响度、低频占比写成 1e30；
>   - `207800a`：整链用例里，分析器真交出来的状态要读得回。
> - 没有推送、合并，也没开 PR。
> - 本报告由主进程落盘：子代理写报告文件被工具拦下了。
> - 机器：NVIDIA DGX Spark，aarch64，GCC 13.3，20 核；Release + Ninja。统一用 `~/.cache/celadon-spark/bin/spark-test.sh`，编译并行 8。107 / 109 连不上，没用。
> - 锁：测试与计时都在全机锁里串行。两次 sanitizer 长跑、真歌读回核对和渲染对比没进锁，那段时间锁由我自己的 sanitizer 主运行占着，没有别人在计时。
> - 临时文件放在 `~/.cache/celadon-spark/tmp/P1F/`，收尾已删。命令和数字都抄在本报告里。

## 0. 结论

| 项 | 结果 |
|---|---|
| 1 先算派生量、后校验（VAL1 F5） | **已修。** 读档每段分四步：<br>1. 按格式读；<br>2. 查量级；<br>3. 换算采样率；<br>4. 前三步都过了才 `deriveEntry`。<br>同类实例一起修了两处：<br>- 采样率换算前先查换算后放不放得进整数；<br>- 格号换算改成先在浮点里钳、再转整数（`clampedBin`）。实时入口也走这条路。<br>UBSan 反向验证：<br>- 撤掉修复后，新用例「试算块数 −1e9」报 `LiveProfile.cpp:1053:98: signed integer overflow: -1000000000 * 7`；<br>- 修好后 0 条 |
| 2 只查有限、不查量级（VAL1 F1） | **两层都做了。**<br>- 读档：逐段的量、计数器、印记与时钟，各有物理上界和相互关系（第 3 节）；<br>- 发布端兜底：非有限的读数按 MusicProfile 的约定当「没有」发布。<br>正常存档照样逐位读回：<br>- 现有往返用例全过；<br>- 整链用例里，分析器真交出来的状态在 48k / 44.1k / 96k 都读得回；<br>- 临时补丁核对：45 首真歌 × 两个主段模型 × 7 个采样率，读回 **743204 次**。被挡 0 次，非有限 0 次；同采样率比了 156368 次，0 次不同 |
| 3 窗口满 + 队列满 | 已补 `testFullQueueWithFullWindow`。VAL1 的变异体 MU8（≥ queueCapacity + 1）下有 2 条断言失败 |
| 4 ⑥ 过门后回落 | 已补 `testQuietCountFallsBackAfterRelease`。素材：响 → 轻 28 dB → 响 → 再轻 28 dB。<br>- 正确代码：第二次各道门放开的时刻与第一次相差 ≤ 2.4 s；<br>- 「只增不减」变异体：第二次早 17～19 s 就放开，用例失败 |
| 编译 | 全新构建两个目标都是 5 条警告，与 a0663e3 逐条相同。**新增 0 条** |
| 定向 5 类 | **4057 过 / 0 败**，87 个用例。a0663e3 是 3915 过 / 0 败，83 个用例 |
| ASan + UBSan | - 实时画像 + 段落账：460 过 / 1 败，**runtime error 0 条**。败的那条是 sanitizer 下的耗时护栏，任务允许；<br>- VAL1 的两组随机对照：163 过 / 0 败，**runtime error 0 条**。P4 的 400 份随机损坏里收下 96 份，之后读数非有限的 **0 份** |
| 全套 | **148945 过 / 5 败**，670 个用例。5 条是已知的 aarch64 专有项。与 a0663e3（148803 / 5，666 个用例）逐用例比，**没有新增失败** |
| 声音与读数不变 | - `--selftest-live` 摘要 **c423806b6b033d96**，与 a0663e3 相同，整份自检输出逐字相同；<br>- 5 首 Jamendo × 两种口径：30 个追踪文件与 a0663e3 **逐字节相同** |
| 耗时 | 兜底在发布那一步里多查约 270 个数是否有限。<br>与 a0663e3 交替各跑 3 遍：每片均值、P99 差 ≤ 0.03 µs；P99.9 都 ≤ 15 µs |

## 1. 两类问题的全部实例（任务第 1 步）

### 1.1 本分支新代码（LiveProfile；行号是 a0663e3 的 `Source/dsp/LiveProfile.cpp`）

| # | 实例 | 位置 | 类别 | 处理 |
|---|---|---|---|---|
| 1 | readEntry 末尾不管 `in.ok` 都调 `deriveEntry`。负的试算块数让 `(int) trialSeries * jmin (trialBlocks, 8)` 溢出（VAL1 F5）。计数坏时，`2 * gatingCount`、`lowRangeLast − lowRangeFirst + 1` 及几项之和同样可能溢出 | 2706（溢出点 993） | 派生在校验前 | 修：readEntry 只读、不派生；restoreState 里格式、量级、换算全过了才 deriveEntry |
| 2 | deriveEntry 里段响度、低频占比的格号「先转 int 再钳」。量级荒谬时 float → int 越界（未定义行为）。读档经 2706 到这里；实时入口经 makeEntry 918 / 983 到这里，入口只查有限 | 286–294 | 派生在校验前 | 修：改用 `clampedBin`，先在浮点里钳再转；真峰值、试算的两个格号也一起改。范围内的结果与原来逐位相同 |
| 3 | 读档时换算采样率：`llround` / `roundToInt` 不查越界（时间戳接近 int64 两端、此刻的采样率荒谬时）；`onsetSamples × 比值`可能溢出成 ∞ | 2794–2803 | 派生在校验前 | 修：`convertEntryRate` 先查换算后放得进 int64 / int，放不进就算坏数据 |
| 4 | 逐段的量只查有限、非负。涉及谱表、门块与短期能量、帧数、通量、起音、次声与全频能量、试算块数、混合损失能量、有效时长、段响度、低频占比、进账因子……（VAL1 F1：Tame 和写成 1e300，下一份的 tameEvidenceDb 变成 ∞；转 float 溢出发生在 2156） | 2586–2708 | 只查有限 | 修：`entryWithinBounds`（第 3 节） |
| 5 | 印记与时钟的关系不查。印记 > 时钟时，全局因子 exp(−(时钟 − 印记)) 会乘出 ∞ | 读：2697–2698；用：1460、1567 | 只查有限 | 修：窗口里的印记不减；最新一段的印记等于时钟 |
| 6 | 计数器只查有限、非负；采样率只查 > 0；换歌次数不查 | 2772–2774 | 只查有限 | 修：加 `countersWithinBounds`；存档采样率限定 8k～768k |
| 7 | 实时入口 `usableSegment` 只查有限、非负。调用方交来荒谬但有限的段时，计数器或主段质量会溢出，画像靠兜底变空，直到 reset | 372–415 | 只查有限 | **没改**：属于实时入口的设计（分析器交来的量有物理上限），见待拍板 1。发布端兜底保证不出 ∞ |
| 8 | LiveConstants 时间常数的取值域「positive」接受任意小的正数。noMainDecaySeconds = 1e-300 时主段时钟溢出成 ∞，之后 ∞ − ∞ = NaN | 1440；LiveProfile.h:104–134 | 取值域只查正 | **没改**：常数表与渲染工具 `--live-constants` 共用，见待拍板 2 |
| 9 | configure 接受任何正有限采样率 | 800 | 只查有限 | **没改**：列入范围外 |

### 1.2 SegmentLedger、MusicAnalyzer 实时部分、ClayTrials

在 SegmentLedger.{h,cpp}、ClayTrials.{h,cpp}、MusicAnalyzer.{h,cpp} 里 grep 下面这些词，**一处也没有**：

`restoreState|fromXml|createStateXml|getDoubleAttribute|getIntAttribute|MemoryBlock|Base64|base64|XmlElement`

这三个文件没有读档，也没有从外部数据恢复状态的入口。本分支在里面只加了实时段落账、试算口径、交段与 advanceTo，输入都是音频与宿主走带。

### 1.3 main 上的老读档代码

只报不改，见第 9 节「范围外」。

## 2. 第 1 条：派生量只在全部校验之后算

`restoreState` 读每一段时走四步：

1. **readEntry**：只按格式读，挡住读不下去的数据。包括长度不对、计数或格号越界、标志不是 0 / 1、非有限值、该非负的为负、坏段的量不是空的。这一步不再派生。
2. **entryWithinBounds（存档采样率）**：查量级，以及各量之间的关系（第 3 节）。
3. **convertEntryRate**：只在采样率不同时做。先算出换算后的值，放不进 int64 / int 就算坏数据。值与原来同一公式、同一顺序，所以正常数据逐位不变。
4. 前三步都过了，才 **deriveEntry**。

全部段读完后，再查印记：窗口里不减，最新一段等于时钟。任何一步失败都冷启动并返回 false，与现有的坏数据处理一致。

另外，deriveEntry 的四个格号（段响度、低频占比、真峰值、试算块峰值）都改用 `clampedBin`：先把 ⌊x⌋ 在浮点里钳进 [0, 格数 − 1]，再转 int；NaN 落第 0 格。在范围内的结果与原来逐位相同，只是越界时不再是未定义行为。实时入口只查有限、不查量级，这一处也一起覆盖了。

## 3. 第 2 条：读档的物理上界与依据

符号：
- T = 2 × `SegmentLedger::maximumBarSeconds` = 12.8 s；
- S = T × 存档采样率，即一段最多的样本数；
- P = 1e80，即每样本、每块、每帧的功率上界：float 样本最大 3.4e38，平方 1.2e77；K 加权、滤波、段内与两声道求和的增益，留三个量级的余量。

| 量 | 上界 | 依据 |
|---|---|---|
| 存档采样率 | 8k ～ 768k | 宿主的 PCM 采样率；同 ReferenceSpectrum::prepare 认的范围 |
| 段起止 | end ≥ start，end − start ≤ S + 1 | 段落账一段最长一小节 6.4 s（= maxSubBlocks × 100 ms；超出就按 2 秒段，2 秒段同样封顶 6.4 s）。<br>变速时同一小节重新锚定，整段走完仍 ≤ 一小节。<br>跳播、循环回绕由编排器 `handleDiscontinuity` 就地定稿。<br>留一倍余量，给边界取整和读档换算的取整 |
| 标称长度 | ≤ S + 1 | 同上 |
| 子块长、帧移 | ≤ S | 分别约 100 ms、约 10 ms |
| 有效时长 | ≤ T | 有效子块都在段里 |
| 段响度 | \|L\| ≤ 3300 LUFS | 有限的正 double 均方，换成 LUFS 落在 [−3234, +3082] |
| 低频占比 | [0, 1] | 低频功率是同一串非负段功率的前缀和，逐位 ≤ 全频 |
| 真峰值、门块功率 | float 本身（≤ 3.4e38） | 已查有限、非负，不另设 |
| 3 s 短期功率之和 | ≤ 个数 × P | 每个 ≤ P |
| 帧数、通量帧数 | ≤ S | 一帧至少一个样本，起点都在段里 |
| 段功率之和 | ≤ 帧数 × P | 帧快照是 float |
| 有效权重、立体声权重、低频组 / 高频组权重 | ≤ 帧数 | 静音门权重 ≤ 1；证据只在实时帧门之上的帧里记，真跑与关实时模式时的补交都一样 |
| 缺失权重 | ≤ 有效权重 | 同一串权重的子集，舍入单调 |
| Tame / Recover 之和 | ≤ 有效权重 × 1e5 dB | 每帧偏离 = 电平 − 轮廓。电平在 [−300, +800] dB；轮廓是 ±3 段加权直线拟合在本段处的截距，系数有界。留百倍余量 |
| 立体声功率之和 | ≤ 立体声权重 × P | |
| \|交叉功率之和\| | ≤ 立体声功率之和 | Balance 每帧把交叉钳在 ±P，分析器读帧时再钳一次；乘同一个门权重再求和，舍入单调，逐位成立 |
| 低频格计数之和 | ≤ 帧数 | 每帧一格 |
| 通量事件 | ≤ 通量帧数 | |
| 起音有效样本 | ≤ S + 128 | 按组起点归段，一组 ≤ 128 样本 |
| 起音事件 | ≤ T ÷ 50 ms + 1 = 257 | 50 ms 不应期（分析器另把它与强度格一起封顶在 128） |
| 次声 / 全频能量 | ≤ 起音样本 × P | 跨采样率换算后，每样本能量最多放大 768k ÷ 8k = 96 倍，仍在余量里 |
| 试算块数 | ≤ 130 | 一块 100 ms（整数个起音组，与 0.1 s 相差 < 0.2 %） |
| 混合损失两份能量 | ≤ 块数 × S × P | 跨采样率后，存档采样率下的块可能只剩原来的 1/96，所以每块按一段的样本数封顶 |
| 进账两个因子、m | [0, 1] | 都是斜坡；钉死的主段度也钳过 |
| 换歌起伏 | [0, 6600] LU | 两个段响度之差 |
| 印记 | ≥ 0；窗口里不减；最新一段 = 时钟 | admitNewest 把进账之后的时钟记成印记；已发布那一份的时钟正是最新一段进账之后的 |
| 进账过的有效音频 | ≤ 段数 × T | |
| 距上次主段、本串换歌的有效音频 | ≤ 进账过的有效音频 | 同一串加法的子序列，舍入单调 |
| 换歌次数 | ≤ 段数 | 每段至多一次 |
| 参考时钟 | ≤ 换歌次数 × 800 | 只由换歌推进，每次 ≤ maximumLogKeep |
| 主段时钟 | ≥ 参考时钟；0 段时 = 0 | 没有上界：衰减按 noMainDecaySeconds 折算，常数可以很小。它只以「时钟 − 印记」进 exp，印记 ≤ 时钟时权重在 [0, 1] |

上界都写在 `LiveProfile.cpp` 匿名命名空间里、`stateSegmentSeconds` 那组常数旁。文件头第六节加了「量级」一段；`LiveProfile.h` 的 restoreStateXml 注释也同步了。

## 4. 发布端兜底（guardPublishedResult）

在 finalizeProfile 末尾、写提交记录之前调。沿用 MusicProfile 现有的约定，没加字段：

- **主段质量 M 或 c 不是有限值**（整份都不可信）：整份换成冷启动（窗口空）时的那一份。特征全无效，长期电平 noDataDb，其余为 0，M = c = 0。
- **特征**：value、effectiveSeconds、confidence 里有一个不是有限值，就换成 `Feature {}`（无效）。
- **逐段数组**，按「这一段没有统计」处理：
  - 长期电平非有限 → noDataDb；
  - Tame / Recover / bandConfidence 任一非有限 → 三个都置 0；
  - S/M、相关度、立体声置信度任一非有限 → 三个都置 0。
- **份额**：oldestMainShare、oldestReferenceShare 非有限 → 0。

兜底只管这一份画像，以及跟着它走的 M、c、份额，即 getProfile、提交记录和 getDiagnostics 里的这几项。计数器（activeSeconds 等）不在这里管：读档时它们有上界；实时入口只有在调用方交来荒谬的段时才会溢出，见待拍板 1。正常数据下全是有限值，兜底一项都不改。

## 5. 用例与反向验证

### 5.1 新增 / 扩充的用例

| 用例 | 内容 |
|---|---|
| LiveProfile `testBadState`（扩充；名字加了 out-of-bounds magnitudes） | 坏数据表加了 32 种「有限但越界」的量：<br>- 试算块数 −1e9（VAL1 F5）、INT_MIN、+1e9；<br>- Tame 和 1e300（VAL1 F1）；<br>- 有效时长 1e300 和 13 s；帧数、段功率、短期能量、起音样本、起音事件、全频能量、混合损失能量各写成 1e300；<br>- 有效权重、通量事件超过帧数；\|交叉\| 超过功率；段响度 1e30；低频占比 1.5；<br>- 段长、标称长度 2^40 样本；段尾早于段头；帧移 2^30；<br>- 进账 m = 2；印记 1e300；印记大于下一段的；主段时钟 ≠ 最新印记；没有换歌却有参考时钟；<br>- 采样率 1e300 和 1 Hz；activeSeconds 1e300；secondsSinceMain > activeSeconds；换歌次数 > 段数。<br>每种都检查三件事：返回 false、冷启动；之后再交段，与全新实例逐位相同；之后的读数全是有限值。<br>阳性对照：没改的状态读得回。<br>字节位置用 `layoutOfEntry` 按 writeEntry 的布局、从 blob 里的计数逐项走出来 |
| `testCorruptStateStaysFinite`（新） | 把 VAL1 的 P4 固化进仓库。400 份随机损坏：随机字节、8 字节极端值、截短、改计数属性、改标量属性。出厂常数与「事件常发」各一半。<br>读回 false 的必须是冷启动；读回 true 的之后每个读数有限，置信度在 [0, 1]。<br>修好后收下 83 份、挡下 317 份 |
| `testPublishGuard`（新） | 不经读档，由实时入口交荒谬但有限的段。<br>局部：<br>- 第 15 段 Tame 和 1e300（比值转 float 溢出）；<br>- 第 3 段交叉 −1e300（S/M 溢出）；<br>- 两段 10 Hz 本该能量 1e308（相加溢出）。<br>出事的逐段数组按「没有统计」，对应特征无效；其余读数与对照实例逐位相同。<br>整份：<br>- 遗忘全关，两段有效时长 1e308（M 溢出），这两段的段响度与低频占比也写成 1e30；<br>- 每份发布都有限；M 溢出之后整份是冷启动画像，提交记录也是兜底后的值；<br>- reset 之后与全新实例逐位相同 |
| `testFullQueueWithFullWindow`（新） | 用 VAL1 的思路：窗口满 520 段后，不调 advanceTo、连交 80 段。<br>- 就地发布 ≥ 48 份；<br>- 连交完那一刻，已发布窗口的判断与画像，同「逐段喂到同样段数」的对照实例一致；<br>- 每一份发布（含就地发布的）逐位相同 |
| `testAnalyzerToProfileBlockSizes`（扩充） | 分析器真交出来的状态：48k 读回逐位相同，44.1k / 96k 也读得回 |
| MusicAnalyzerLive `testQuietCountFallsBackAfterRelease`（新） | 测 ⑥ 过门后净计回落。素材：响 60 s → 轻 28 dB 75 s → 响 60 s → 再轻 28 dB 72 s。<br>断言：<br>- 两次轻下来，头 18 s 都没有有效子块；<br>- 三道门 65 s 内都放开；<br>- 第二次与第一次放开的时刻相差 ≤ 8 s。<br>实测（帧 / 有效子块 / 有效起音，单位 s）：第一次 44.5 / 52.3 / 40.6，第二次 46.9 / 52.7 / 41.1 |

### 5.2 反向验证

在临时副本里撤掉某一处修复，或打上 VAL1 的变异体，编译后跑对应的类。撤修复用脚本 `revpatch.py`，每次都从修好的那一份重新开始。

| 撤掉 / 变异 | 结果 |
|---|---|
| 量级校验<br>（entryWithinBounds / countersWithinBounds 恒真，印记不查，采样率只查 > 0） | 282 过 / 60 败：坏数据表 30 种的「false and cold」和「half-read」都败。<br>试算块数 −1e9 / INT_MIN 两种被原有的 ≥ 0 检查挡下，所以没败；它们触发的派生量溢出只在 UBSan 下看得出，见本表倒数第二行。<br>「读数有限」那条没败，因为兜底还在 |
| 发布端兜底 | 335 过 / 7 败：兜底用例 7 条全败。例：特征 11 value inf；publication 5 seconds inf |
| 两层都撤（≈ a0663e3） | 270 过 / 72 败：<br>- Tame 1e300 之后 band 15 tame inf，复现 VAL1 F1；<br>- \|交叉\| 越界、起音事件 1e300、印记 1e300 之后，同样出 ∞ / −∞；<br>- 400 份随机损坏收下 224 份，其中 2 份之后出 ∞。第一份是 trial 0、mode 3：1e47 写进字节 60625，第 13 段 tame inf |
| makeRoomInQueue 改成 ≥ queueCapacity + 1（VAL1 MU8） | 340 过 / 2 败：就地发布只有 47 份；连交完那一刻的窗口判断与对照不同 |
| ⑥「过门就减」改成「只增不减」（VAL1 mu19） | 12 过 / 1 败：第二次轻下来 25.6 / 33.4 / 21.8 s 就放开，第一次是 42.6 / 52.3 / 40.6 s。原有 ⑥ 用例照过，与 VAL1 说的一致 |
| 读档先算派生量（原来的写法）<br>（ASan + UBSan，另加 float-cast-overflow，只跑 testBadState） | 142 过 / 0 败，但 UBSan 报：<br>`LiveProfile.cpp:1053:98: runtime error: signed integer overflow: -1000000000 * 7 cannot be represented in type 'int'`<br>调用链 deriveEntry ← readEntry ← restoreState，由新用例「trial blocks -1e9 (VAL1 F5)」触发。<br>修好后，主 sanitizer 运行里同一个用例 0 条 |
| 格号先转整数再钳（原来的写法）<br>（float-cast-overflow，只跑 testPublishGuard） | 14 过 / 0 败，但报两条：<br>- `LiveProfile.cpp:346:73: runtime error: 1e+31 is outside the range of representable values of type 'int'`（段响度）；<br>- `:351:72` 1e+33（低频占比）。<br>修好后 0 条 |

## 6. 验证

### 6.1 编译与警告

a0663e3 与 207800a 各自全新构建两个目标，都是同样 5 条警告，逐条相同：
- `stl_algobase.h:437` 的 -Wstringop-overflow ×2，celadon_tests、celadon_render 各一条；
- -Wrange-loop-construct ×3：`ChainTests.cpp:168`、`MusicAnalyzerTests.cpp:3509`、`SegmentLedgerTests.cpp:536`。

**新增 0 条。**

### 6.2 定向测试

`--only LiveProfile,SegmentLedger,MusicAnalyzerLive,MusicAnalyzer,MusicAnalyzerHouse`：
- 207800a：**4057 过 / 0 败**，87 个用例；
- a0663e3：3915 过 / 0 败，83 个用例。

### 6.3 ASan + UBSan

设置：
- `CELADON_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'`（CMAKE_CXX_FLAGS 也带进链接）；
- `ASAN_OPTIONS=detect_leaks=0`，`UBSAN_OPTIONS=print_stacktrace=1`。

两次运行：
- **`--only LiveProfile,SegmentLedger`**（2fe0c19 的用例）：460 过 / 1 败，38 个用例，**runtime error 0 条、ASan 报告 0 条**，耗时 40 分 10 秒。
  - 败的 1 条是 4/4 耗时护栏「means within the 200 us guard」：sanitizer 下每片均值 233 µs，平时约 4 µs。任务允许。
  - 400 份随机损坏：收下 83、挡下 317，与普通构建相同。
- **VAL1 的两组随机对照**：把 fuzz_method.txt（P2 / P3 / P4 / P5）和 fuzz_method2.txt（P7 / P8 / P9）插进临时副本的 LiveProfileTests，与坏数据表、400 份随机损坏、兜底用例（57fd7cd 版）一起跑。结果 **163 过 / 0 败，runtime error 0 条、ASan 报告 0 条**，56 分 46 秒。
  - P2 / P5：80 次，0 失败；
  - P3：60 次，0 失败；
  - **P4：400 份随机损坏，收下 96 份，之后读数非有限 0 份**（VAL1 在原代码上收下 202 份，其中 1 份出 inf，还撞到 deriveEntry 的溢出）；
  - P7 / P8 / P9：坏读数 0 个。另起一份副本单独跑也是 0 个，31 s。

### 6.4 全套（207800a）

- **148945 过 / 5 败**，670 个用例，3 分 28 秒。
- 败的 5 条都是已知的 aarch64 专有项：ColorModule「stereo with L = R is bit-identical to mono」4 条；BalancePerformance「stereo balance costs 1.72 x mono」1 条。
- 与同机同日跑的 a0663e3（148803 / 5，666 个用例，3 分 23 秒）逐用例比：
  - 原有用例都在（testBadState 改了名），失败数都没变；
  - 新增 4 个用例全过。

### 6.5 耗时

在全机锁里，a0663e3 与修复交替各跑 3 遍 LiveProfile 类，看每片读数：

| | a0663e3 | 修复 |
|---|---|---|
| 4/4 型 均值 | 4.00～4.02 µs | 4.00～4.01 µs |
| 4/4 型 P99 | 10.77～10.80 µs | 10.78～10.82 µs |
| 4/4 型 P99.9 | 10.85～11.71 µs | 10.88～10.94 µs |
| 响度型 均值 | 3.82～3.85 µs | 3.82～3.84 µs |
| 响度型 P99 | 12.34～12.35 µs | 12.35～12.37 µs |
| 响度型 P99.9 | 12.42～13.60 µs | 12.43～13.38 µs |

兜底的开销在噪声以内。P99.9 都 ≤ 15 µs；每一遍都在发布时刻之前做完。

## 7. 声音与读数不变

**自检。** `celadon_render --selftest-live` 在 a0663e3 和最终代码上都显示「T-048 自检全部通过」：
- 块长 512 / 4096 / 480 的摘要都是 **c423806b6b033d96**；
- 通用档的摘要都是 a5194b2a49ae8cec；
- 整份自检输出逐字相同（diff 为空）。

**真歌追踪。** 曲目与口径：
- 5 首 Jamendo：deep track_0873861（103 BPM）、deep track_1089928、tech track_0080793（100 BPM）、tech track_0130611、melodic track_1062232；
- 两种口径：各首自己的 House 子类（4/4 型），以及 general（响度型）。

命令同 calibrate_house.py 的 `render_live_one`：

`--glaze 0 --clay auto --style auto --delivery standard --bpm <曲速> --block 512 --segments-csv --metrics-only --no-replay --live-trace --live-only --live-tag p1f`

结果：
- 追踪、__final、__judgements 共 **30 个文件，sha256 两边逐个相同**；
- 10 份 metrics.csv 和 10 份输入逐段统计也相同；metrics.md 只差第 3 行的生成时间。

**二进制。** 渲染工具的 sha256：a0663e3 是 0a7c8fa4…，最终代码是 8ed69a93…。2fe0c19 之后只改了用例，ninja 对渲染工具无事可做，所以 207800a 用的是同一个二进制。输出都放在临时目录，收尾已删；校准目录 `~/.cache/celadon-t048-cal/live/` 下没写过 p1f-*。

## 8. 真歌存档读回核对（临时补丁，不进仓库）

做法：在渲染工具副本里给 LiveTracer 打补丁。每处理 23 块存一次状态，读回到 7 个新实例：此刻的采样率、8k、44.1k、48k、96k、192k、768k。歌尾 flush 之后再做一次。

结果：
- 45 首 Jamendo × 两种口径 = 90 遍，**106172 个存档点，读回 743204 次**；
- **被挡 0 次**，读回的画像出非有限值 **0 次**；
- 同采样率、没有待发布段时逐位比较 156368 次，**0 次不同**。44.1k 在采样率列表里出现两次，所以比较次数多于存档点数。

这些真歌的窗口最大约 250 段，没到 512 段；窗口满 512 段的读回由 testStateRoundTrip 的 600 小节覆盖。

## 9. 范围外、待拍板、没验证的

### 范围外（只报，不改）

1. **GlazeProfile.cpp:511–552 `fromXml (MusicProfile)`**（main 上的老代码）
   - 问题：特征的 value / effectiveSeconds / confidence、四个逐段数组、effectiveSeconds、representativeness 都按位读回，既不查有限，也不查量级。
   - 影响：决策器的 trustOf / valueOr（GlazePlanner.cpp:1117–1135）会把非有限当无效，但有限而荒谬的值会照用。
   - 复现思路：已提交状态里，把 `integratedLufs_value` 改成 `7149f2ca`（1e30f），读档照收。
2. **GlazeProfile.cpp:582–624 `fromXml (CommittedState)`**
   - 问题：方案和校准的浮点字段既不查有限，也不查量级。执行快照那一份查了有限和一部分量级。
   - 复现思路：PLAN 的 effectiveGainDb 写成 `7f800000`（+∞）或 1e30，fromXml 返回 true。
3. **GlazeProfile.cpp:720–764 `fromXml (ExecutionState)`**
   - 只查有限、不查量级的字段：requestedTargetLufs、effectiveTargetLufs、requestedGainDb、effectiveGainDb、estimatedExcessDb；reference 的 ceilingDb、lookaheadMs、limitReleaseMs、clayLowCutHz、clayMonoHz、sampleRate。
   - clayGuard.phaseHoldSeconds 和 widthCapDb 只查 ≥ 0。
4. **GlazeController.cpp:3779–3784**：feedbackMaxGainDb 只查有限。
5. **LiveProfile 实时路径**（本分支，没改）：
   - `configure` 接受任何正有限采样率；
   - usableSegment 与常数取值域的问题见待拍板 1、2。
6. **sanitizer 下的耗时**：LiveProfile 类约 40 分钟，其中 400 份随机损坏用例约占 30 分钟，普通构建只要 0.8 s。

### 待拍板

1. **实时入口要不要也按读档的物理上界拒收荒谬的段？**
   - 现状：usableSegment 只查有限、非负。荒谬的段会让计数器 / M 溢出，画像靠兜底变成冷启动画像，直到 reset。
   - 分析器交来的段在物理上碰不到这些上界。
   - 改的话，「坏段」的定义会扩到实时路径，属于模块边界。
2. **LiveConstants 时间常数要不要设下限？** 例如 noMainDecaySeconds = 1e-300 时主段时钟会溢出成 ∞，之后变 NaN。这张表与渲染工具 `--live-constants` 共用。
3. **读档只收存档采样率 8k～768k，是否接受？** LiveProfile::prepare 本身接受任何正有限采样率，所以在这个范围外存下的状态读不回，会冷启动。
4. **一段时长上界 12.8 s 的前提是否成立？** 它依赖「跳播 / 循环回绕由编排器就地定稿」。如果阶段 2 有不经编排器、可能交出更长段的来源，要先放宽这个上界。

### 没验证

1. x86（107 / 109）没跑。
2. 12.8 s 上界在病态宿主下的情形（例如走带中 PPQ 冻住、却不报跳播）没有专门造过。
3. Tame / Recover 偏离的上界 1e5 dB 是按系数有界估的，没有做穷举证明。
4. 207800a 加的整链读回检查只在普通构建里跑过，没在 sanitizer 下跑。
5. 老代码的范围外几条是看代码得出的，没写用例复现。
