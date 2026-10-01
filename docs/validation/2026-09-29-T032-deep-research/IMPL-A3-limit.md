# T-032 实现报告 · A3-limit · Limit 限幅链（TruePeak / LimiterModule）

> 2026-09-30，Opus 5.5 · max 接续前任（Fable 5.1，额度中断，主进程保全为 WIP 提交 `8abdf05`）。
> 工作树 `.claude/worktrees/wf_abb55518-7ae-3`，分支 `worktree-wf_abb55518-7ae-3`，改前的底 = `8abdf05~1` = `e6b445b`。
> 任务书：`BRIEF-A3-limit.md`（研究 `R1-limit.md` / `R8-chain-juce.md`，核验 `V1-limit.md` / `V8-chain-juce.md`，在 `fable-deep-research-634a5e` 工作树同名目录）。
> 验证全部在 107（AMD EPYC 74F3，GCC，Release，`-j 3`）上用本工作树的 `scripts/test-linux.sh` 跑；本机 Mac 未编译、未测试；109 连不上，未验证 MSVC。

## 结论一览

| 条目 | 做了什么 | 改输出 | 新测试 | 状态 |
|---|---|---|---|---|
| R1-01 当晚替代：SSE2 预广播系数表 | SSE2 核改读所有检测器共用的静态预广播表（7 KB、16 字节对齐），系数设计挪进共用表只算一次 | 否（逐位不变） | `TruePeakDetector` 输出摘要（4 种冲零模式） | 完成；29.23 → 18.97 ns（1.54×） |
| R1-02 van Herk 滑动最小值 | 单调队列换成两缓冲 van Herk；+∞ 哨兵；改窗同时清空 | 否（逐位不变） | 随机窗长 vs 暴力 `std::min_element` vs 旧实现、块中途缩窗、不分配 | 完成 |
| R1-03 ceilingDelay 重置填法 | `resetLimiterState` 填当前 Ceiling，`reset()` 平滑器落位后再填 | 否（逐位不变，无黑盒差异） | 白盒：各种重置后 Ceiling 延迟线里的值（测试钩子） | 完成 |
| R8-02（Limit 部分）参数守卫 | `setParams` 各字段 `isfinite ? jlimit : 默认`；Ceiling 下限 −4.5；Lookahead 只拦非有限 | 范围内有限值逐位不变 | 5 字段 × NaN / ±Inf × 2 种时机与默认逐位相同；越界钳到边界；−4.5 真的生效 | 完成 |

测试数字：定向（`LimiterModule, TruePeakDetector, Saturator, LoudnessMeter`）82937 过 / 0 失败；全套 **133184 过 / 0 失败**（2026-09-30 15:01–15:05 UTC）；先 `clean` 再全量编译 celadon_tests（58 步，含 JUCE 库）**编译警告 0 条**（日志里 7 行带「warning:」的是 ClayGuard / ClayGlaze 用例标题里的「phase warning:」，不是编译器输出）。

## 验证

### 命令

```bash
# 定向（增量编译）
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests" --only LimiterModule,TruePeakDetector,Saturator,LoudnessMeter'
# 全套（先 clean 再全量编译，数警告）
scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target clean && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests"'
```

### 逐位证据：改前 / 改后同机对照

改前构建放在 107 的 `~/.cache/celadon-t032/a3-base/`（用完已删）：四个源文件取 `e6b445b` 原文件（远端 sha256 与 `git show e6b445b:…` 逐一核对相同），测试文件用本线最终版本（LimiterTests 去掉两个只能在新接口上编译的用例），同一组 `--only` 类。输出指纹是 FNV-1a 64 位、按每个样本的 32 位位模式累进，只打日志不断言（`LimiterTests.cpp` 的 `Fingerprint`、`DspPartsTests.cpp` 的 `testOutputDigests`）。

| 用例（日志里的指纹 / 摘要） | 改前 e6b445b | 改后 |
|---|---|---|
| true peak <= ceiling + 0.01 dB（540 次运行） | 58485cff3e2a01cd | 58485cff3e2a01cd |
| independent 16x reconstruction（48 例） | 3b627ebcd17f24d0 | 3b627ebcd17f24d0 |
| output independent of block size | b57c3b466bf799a9 | b57c3b466bf799a9 |
| lookahead change（前后两段） | 408fcc3fa89cdc29 | 408fcc3fa89cdc29 |
| gain returns exactly to 1.0（manual / auto） | a10e0e36739d4a12 / bd8c7487b3ab4082 | 相同 |
| restartWithFadeIn | 7f1305acf5eaab95 | 7f1305acf5eaab95 |
| clip idle skip（44.1 / 48 / 96 / 192 kHz × 块 64 / 512，8 个） | 4e043f67… cf727d24… e1999853… d69f46be… f2adea9b… 13b00102… 88e605ca… 58e43d22… | 8 个全部相同 |
| LF release：分块 + reset == fresh prepare | a5734da41af039ad | a5734da41af039ad |
| 检测器参照输出：IEEE / FTZ / DAZ / FTZ+DAZ | 945a462d… / 25c7f441… / b504ea4b… / b504ea4b… | 4 个全部相同 |

16 个限幅器输出指纹 + 4 个检测器参照摘要全部相同。检测器摘要证明的是「参照输出本身」没变（系数设计挪进共用表、逐位拷回实例后，参照实现读到的 float 与改前一样）；块路径 = 参照由 DspPartsTests 的 4 种 MXCSR × 9 种信号 × 7 种分块矩阵断言（0 失配），摘要用例另断言块路径摘要 = 参照摘要。

性能（同机同日，`TruePeakDetector` 性能护栏，立体声块 512，15 遍取最短）：改前 SSE2 核 29.23 ns → 改后 18.97 ns / 样本 / 声道（1.54×；核验报的是 28.11 → 17.99）。护栏门槛 80 → 50 ns。

### 测试真能抓住缺陷（基线 + 变异）

- **R8-02**：同一份守卫用例放在改前代码上跑，54 条失败（其余 82866 条照过）：Gain NaN / ±Inf 输出出现非有限或静音、Ceiling NaN / −Inf 中途变坏后静音、Ceiling +Inf 真峰值超限、Release NaN 真峰值超限（限幅器失效只剩硬钳位）、Lookahead NaN / ±Inf 延迟变成 123 / 602 / 170 样本、Clip 非有限与默认不逐位相同、7 个越界值与边界值不逐位相同。改后 0 失败。
- **变异 A**（同一轮三处互不干扰的变异，只动 .cpp，跑完按备份还原、sha256 核对）：
  - M1 SSE2 核第 7 相误读第 6 相系数 → DspPartsTests 矩阵 210 条「processBlock differs」+ 90 条「processBlockMax differs」、交替调用 7 条、摘要用例 4 条失败；
  - M3 Ceiling 延迟线退回清零（即改前写法）→ R1-03 用例 4 处全部失败（prepare / restartWithFadeIn / 带新 Ceiling 的 reset 后 158 个值全错，换前瞻后 301 个值全错）；
  - M4 releaseMs 不拦非有限 → 只有 releaseMs 的 8 条失败，其他字段 0 条（守卫用例可按字段归因）。
- **变异 B**：van Herk 取上一块后缀差一位 → 对照用例失配 2374（增益状）/ 5723（随机），块中途缩窗 240。
- **变异 C**：`setWindow` 不清空（前任的写法，清空交给调用方）→ 现有用例「reset == fresh prepare」失败，随后缩窗用例越界写坏堆、进程 abort（`malloc(): unaligned tcache chunk detected`）。107 上没有留下 core / crash 文件。

## 逐条说明

### R1-01 当晚替代：SSE2 预广播系数表

- 位置：`Source/dsp/TruePeak.cpp` `SharedCoefficients`（:119）、`sharedCoefficients()`（:184）、SSE2 `computePeakGroups`（:312）、构造函数（:361）、`processChunk` 的调用（:433）；`Source/dsp/TruePeak.h` `tapsBroadcast`（:91，改成指针）。
- SSE2 核每个抽头：1 次窗口加载 + 7 次 `_mm_load_ps`（直接是 4 个相同的系数）+ 7 乘 + 7 加，去掉原来的 2 次系数加载与 7 次 `shufps`。乘法的两个操作数（x 与同一个系数）、加进哪个累加器、按 k 的顺序都没变，所以与参照实现、与旧核逐位相同。
- 表放在静态存储而不是检测器成员（这是对前任写法的改正，见下）：`SharedCoefficients` 在第一次构造检测器时算一次（函数内静态量，线程安全初始化），之后只读；检测器都在音频线程之外构造（链与两块表的成员随处理器构造，测试与离线工具的局部量），音频线程只经实例里的指针读表。结构体大小 9216 = 16 × 576、对齐成员在偏移 2048，没有补齐，不会触发 C4324。
- 系数设计从构造函数原样搬进 `SharedCoefficients`（公式与运算顺序不变），构造时把 `reversedPhases` 逐位拷进实例：参照实现、NEON 核（读实例的 `tapsByTap`）照旧，SSE2 核读共用表——两边是同一批 float 由构造保证，不靠「每个实例各算一遍结果相同」。NEON / 标量路径的核代码一行没动。
- 性能护栏：`DspPartsTests.cpp:579` SSE2 门槛 80 → 50 ns（实测 18.97，约 2.6 倍余量）。
- **后续（没做）**：AVX2（无 FMA）核，核验测得比旧 SSE2 快 3.04×（约比预广播版再快 2×）。要做需补：运行时按 `hasAVX2()` 分派 + 能强制走每条核的测试钩子（否则 SSE2 回退路径没人测）；三套工具链的单独编译单元（MSVC 只能按文件加 `/arch:AVX2`，`.jucer` 与 CMake 同步）；109 的 MSVC + Wine、Mac x86_64 切片在 Rosetta 下两种情况都要验。

### R1-02 van Herk 滑动最小值

- 位置：`Source/dsp/LimiterModule.cpp` `SlidingMinimum::prepare / setWindow / clear / push`（:123–:170）、`resetLimiterState` 调用（:358）；`LimiterModule.h` `struct SlidingMinimum`（:149，挪到 public 只为测试直接对照）。
- 两缓冲 van Herk：`prefix` = 当前块前缀最小值，`suffix[j]` = 上一块从 j 到块尾的后缀最小值，块满时从后往前算一遍（每 window 个样本一次、≤ 543 次取小，< 1 μs）；每样本恒定 2 次取小，没有整数取模、没有数据相关的循环。
- 哨兵用 +∞ 而不是任务书写的 1.0：+∞ 让「窗口里还没被填满的部分不参与」，与旧实现「只看已推入的样本」严格一致，不依赖需求增益 ≤ 1 这个外部不变量；在需求增益 ≤ 1 的实际输入上两者结果相同。
- `setWindow` 改成钳进容量并清空（对前任的改正）：块算法的状态只对原窗长有意义，前任的 `setWindow` 只赋值、清空交给调用方——若改窗时块内位置已 ≥ 新窗长而调用方没 clear，`position` 永远等不到 `window`，一路越界写（变异 C 实测 abort）。现在 `0 ≤ position < window ≤ 容量` 恒成立，`resetLimiterState` 里多余的 `clear()` 去掉。
- 冲零模式：DAZ 开着时 `minss` 把非规格化操作数当 0 返回、旧实现返回原位；下游第一步是转 double（`cvtss2sd` 同受 DAZ 管）或比较，两边都是 0，输出不变。需求增益要非规格化，峰值得超过约 1e38，实际不会出现。
- 收益照核验：控制路每样本一次，48 kHz 约省 0.03 % 个核，192 kHz 约 0.13 %，属顺手改；没有另做「容量取 2 的幂用掩码」的折中（van Herk 已替代）。
- 测试 `testSlidingMinimumMatchesReferences`（`LimiterTests.cpp:716`）：旧实现逐行照抄作参照；窗长 1、2、543 + 57 个随机，增益状 / 随机两种输入各 240000 样本，与暴力、与旧实现逐样本逐位对照；奇数轮只调 `setWindow` 不另调 `clear`；块中途缩窗（位置 300 时改成 7）；`push / setWindow / clear` 不分配。

### R1-03 ceilingDelay 重置填法

- 位置：`LimiterModule.cpp:354`（`resetLimiterState`：`fill (ceilingSmoothed.getCurrentValue())`）、`:303`（`reset()`：平滑器落位后 `fill (ceiling)`），与 `gainDelay` 同构。
- 逐位不变的理由：重置同时清空了等长的 `coreDelays`，头 coreDelay 个输出 `delayed = +0`，`jlimit (±c, +0)` 与 `jlimit (±0, +0)` 都返回 +0（上表 restartWithFadeIn、lookahead change、reset == fresh prepare、分块无关等指纹与改前相同）。
- 没有黑盒可观测差异（核验已指出研究给的黑盒用例无效），所以加了白盒用例 `testHardClipCeilingDelayFollowsResets`（`LimiterTests.cpp:877`），经只读测试钩子 `getPendingHardClipCeilingsForTesting()`（`LimiterModule.h:135` / `.cpp:412`，在 DelayLine 副本上推、不改对象，会分配、只给测试用）查延迟线接下来要交出的值：prepare、restartWithFadeIn 之后全是 −1 dB 的增益；带新 Ceiling 的 reset 之后全是 −2.5（验证 reset 的第二次填）；换前瞻的那一块开头平滑器还停在 −2.5，前 coreDelay′ − 1 个是 −2.5、最新一个是滑向 −1 的第一步（验证 resetLimiterState 的填法）。改前写法下这 4 处全错（变异 A-M3）。

### R8-02（Limit 部分）参数守卫

- 位置：`LimiterModule.cpp:100–:119`（`finiteInRange`、`sanitised`）、`:375`（`setParams` 入口）；`LimiterModule.h` `Params` 注释。
- Gain 0～24、Ceiling −4.5～0（链扣掉编码余量后会交 −4.5，钳到 −3 会破坏编码余量）、Release 10～1000、Clip 0～100，非有限回 `Params{}` 默认；Lookahead 只拦非有限，范围与取整仍由 `lookaheadSamplesFor` 负责。范围与 APVTS（`ParameterRanges.h`）一致，JUCE 的 `snapToLegalValue` 对有限值本来就钳在范围内，GLAZE 写 Gain / Clip 时也已钳（`GlazeController.cpp:2182`、`GlazePlanner.cpp:1739-1740`），所以范围内的值是恒等（`jlimit` 原样返回同一位模式），GLAZE 校准哈希读链的 `manual`，不受影响。
- `prepare` / `reset` 读的都是 `setParams` 存下的 `params`，没有别的入口绕过守卫。
- 测试 `testParamsGuardNonFiniteAndOutOfRange`（`LimiterTests.cpp:939`）：基准参数每个字段都不取默认值（Gain 24、Ceiling −2、Release 250、Lookahead 5、Clip 30），5 字段 × NaN / +Inf / −Inf × 两种时机（prepare 时就坏、中途换回正常；先正常、中途变坏），与「同一时刻该字段取默认值」的限幅器逐位相同、延迟相同、输出有限、尾段不静音、真峰值 ≤ Ceiling + 0.01 dB，共 30 例；10 个越界值与边界值逐位相同；Ceiling −4.5 实测交出 −4.850 dBTP（不被改成 −3 或 −1）。
- 链级 NaN 用例归 A4，Color 与处理器 readParams 的第二道归别的线（任务书）。

## 前任改动里我改正了什么

1. **TruePeak 的对齐成员**：前任把预广播表做成检测器成员 `alignas (16) std::array`，并在头文件里用 `JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4324)` 包住类。成员对齐会把 16 字节对齐要求传给 `LimiterModule`、`LoudnessMeter`、`CeladonChain` 与处理器，这些类在 MSVC /W4（CMake 与 `.jucer` 都是 4 级）下可能各报 C4324，而那对 pragma 只罩得住 `TruePeakDetector` 自己；今晚 109 连不上、验证不了。改成静态共用表，检测器不再带对齐要求，顺带限幅器与两块表的三个检测器共用一份缓存。
2. **`SlidingMinimum::setWindow` 不清空**：见 R1-02，改窗必须清空，否则可能越界写（变异 C 实测 abort）。
3. **R8-02 用例的三个盲点**：前任的「中途再喂一次」喂的是同一个坏值（中途是空操作），基准参数的 Ceiling / Release / Lookahead 恰好等于默认值（换成默认值看不出变化），「不静音」检查的是整段输出（前半段响会掩盖后半段静音）。改成基准全不取默认值、两种时机、只查尾段、并比延迟。
4. **补 R1-03 白盒用例与测试钩子**（前任没有 R1-03 的测试）。
5. **补检测器输出摘要用例**（`DspPartsTests.cpp:355`）：只靠「块路径 = 参照」矩阵证明不了系数本身没变。
6. 注释改准：`roundToInt (NaN)` 不是「未定义」而是得到无意义的整数（x86 上 0，前瞻变 1 个样本）；SlidingMinimum 注释里的「无分支」改成「无数据相关的循环」；性能数字换成同机实测。前任注释里的「矩阵 0 失配」「29.09 → 18 ns」是引核验独立复现的数、在插件代码上还没跑完（前任中断在定向测试），本线已在插件代码上同机实测确认。

## 没做的与原因

- AVX2 核：任务书明确当晚不做（运行时分派 + 三工具链单独编译单元 + 109 连不上验不了 MSVC），写在上面「后续」。
- R8-02 的 Color 部分、处理器 `readParams` 的 `finiteOr` 第二道、链级 NaN 参数用例：任务书划给别的线（A2 / A4）。
- 范围外的边角（报告、未改）：`LimiterModule::prepare` 的前瞻上限 `roundToInt (10 ms × fs)` 在采样率低于 50 Hz 时为 0，而 `lookaheadSamplesFor` 至少给 1，窗长会比滑动最小值的容量多 1（旧实现同样超界）。现在 `setWindow` 把窗长钳进容量，不会越界；真实采样率（≥ 8 kHz）不受影响，所以没动 `prepare` 的取法。
- MSVC 与 NEON 构建：109 连不上、Mac 不许编，均未验证。风险评估：NEON 路径的核代码没动，只是构造函数改为从共用表拷系数（数值相同）；SSE2 核只在 x86 编；`SharedCoefficients` 无补齐，不该有 C4324。仍建议 109 恢复后用 msvc-wine 编一次 celadon_tests。

## 跨文件需求

- （建议，非必须）`tests/TestHelpers.h`：LimiterTests 的 `Fingerprint`、DspPartsTests 的摘要 lambda 与 BalanceTests 的 `Fnv1a` 是三份相同的 FNV-1a，可合成一个公共助手。本线不能改 TestHelpers.h。

## 档案待补（主进程收尾）

- 已做：`01-框架/设计规格-Limit.md` 末尾加「2026-09-29 修订（T-032）」小节（Edit 增量追加，文件 127 行）。
- 待补（不在本线文件清单内）：`02-日志/2026-09-30.md` 记一笔、`04-协调/TASKS.md` 移行、`00-总览/INDEX.md` 刷新；若随版本发布，`05-版本/` 的版本档写上「Limit 输出逐位不变、x86 真峰值核提速 1.54×」。

## 待 Max

- 要不要排 AVX2 核（x86 版三个检测器再省约一半 CPU）：需要 109 与一台跑 Rosetta 的 Mac 做验证。
- CI 的 109 runner 若 CPU 明显慢于 107，SSE2 护栏 50 ns 的余量会变小（107 实测 19 ns）；如 CI 报这一条，请先看 109 上的实测数再定门槛。
