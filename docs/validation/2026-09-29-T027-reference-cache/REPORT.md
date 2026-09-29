# T-027 · 校准参考条件手动参数部分的缓存 —— 验证记录（2026-09-29）

档案：vault `项目/Celadon/04-协调/TASKS.md`（T-027）、计划 `04-协调/2026-09-29-T-027-参考条件缓存计划.md`、日志 `02-日志/2026-09-29.md`。
底 = main `af88861`（PR #2 T-025 与 v0.1.9.0、PR #3 T-026 都已合入）。本线先后以 T-026 的 `6cdfd51`、`7ccc703` 为底做过两轮，
T-026 合进 main 后换到 `af88861`，全部重跑。编译、测试、基准都在服务器上跑：107（Ubuntu，GCC 15.2，Release）和 109（真 MSVC +
Wine）。本机 Mac 没编译、没跑测试。

## 1. 改了什么

`Source/dsp/GlazeController.cpp`：`Impl` 里加一份 mutable 缓存（手动参数部分的参考条件 + 输入 `ChainParams` + 约束反馈上限 + 有效位）。
`makeReference` 改为从 `manualConditionsFor(m)` 的整份参考起步，再覆盖画像号、风格、GLAZE、Ceiling、Lookahead、采样率。

- 键 = `samePlanningManual(m, 缓存的输入)` 且 `sameBits(feedbackMaxGainDb, 缓存的上限)`。`setManualConditions` / `emitConditions`
  读的每一项，`samePlanningManual` 都比到了：
  - Balance 5 项、Color 6 项、Limit Gain / Release / Auto / Clip / Bypass、Clay 三态 7 项，浮点按位比；
  - v0.1.9.0 的「Mast」段（去刺耳量、瞬态保留、目标量、有无参考、参考频谱、目标斜率），由 main 已有的 `sameAdvancedBalance` 比，
    比较条件和它们进哈希的条件一致。其中参考频谱按值比：按值相等但位不同的只有 ±0，两者的量化码相同。
  - 另外还比 Ceiling、Lookahead。多比不影响结果，只是多重算一次。

  所以命中时拷贝的结果和重算的逐位相同。换底时逐次核对过：
  - `7ccc703`：哈希改写成 `emitConditions` 模板，读的项没变；
  - `af88861`：`emitConditions` 末尾多了 Mast 段，`samePlanningManual` 已经带上 `sameAdvancedBalance`；
  - 两次换底，`CalibrationReference` 和 `setManualConditions` 都没变。
- `setManualConditions` 另外两个调用点不动：稳定计时用的是不限上限的参考；`upgradeLegacyConditions` 只在读回工程后的第一块
  和 `prepare` 里算。
- 线程：缓存和 `manual` 一样，只由音频线程读写。另两个调用方是 `prepare`（JUCE 保证 `prepareToPlay` 不和 `processBlock`
  同时跑）和 `getProbeForTesting`（只给测试和渲染工具用）。缓存是固定大小的拷贝，不分配内存。

## 2. 全套

| 平台 | 改前（main `af88861`，T-026 的 `suite-merged-main-*.log`） | 改后 |
|---|---|---|
| 107 Linux GCC 15.2 | 132969 过 / 0 败 | `suite-after.log`：**132981 过 / 0 败**，0 个编译警告 |
| 109 真 MSVC 19.44（VS2022）+ Wine | 132969 过 / 0 败 | `suite-msvc-wine.log`：**132981 过 / 0 败**，0 条 MSVC 警告 |

- 多出的 12 项就是新用例 T-027 1 的 12 个断言。`warning:` 在 Linux 日志里出现几次，都是测试名里的 "phase warning"。
- 除本线改的 2 个文件外，改前改后的源码逐个一致（`git diff af88861 -- Source tests`）。所测源码散列见 `source-sha256.txt`（110 个文件）。
- 新用例 T-027 1：44 步正向改，再原路改回，外加单独改约束反馈上限，共 182 块，每块都核对了探针和发布的执行快照。
  Mast 那 10 步（去刺耳手动 / 量 / 瞬态保留 / 自动 × GLAZE、目标中性 / 量 / 暖 / 自动、关）另有断言：每一步都必须真的改变参考哈希。
- 两个零分配用例（GLAZE 各路径、Clay 各路径）照样过：缓存那条路径在音频线程上不经 `operator new`。
- 以前的轮次：底 `6cdfd51` 是 131193 → 131204，底 `7ccc703` 是 131207 → 131218，都是 0 败 0 警告。

## 3. 反向验证（`reverse.sh`，107；经 `scripts/test-linux.sh` 在服务器副本里改，跑完改回原样并重编）

| 变体 | T-027 1 的结果 | 其他 |
|---|---|---|
| R1 缓存键去掉约束反馈上限 | 失败 3 项：改上限后 6 块不符，两个上限给出的哈希也相同了 | — |
| R2 缓存键只比 Clay 三态设置 | 失败 3 项：从「balance recover」起 118 块不符，Mast 段也没测到变化 | T-026 3（两种扫描速度）失败 4 项 |
| R3 缓存键忽略 Mast 段各项 | 失败 3 项：从「de-harsh manual」起 38 块不符，Mast 段没测到变化 | — |

日志见 `reverse.log`。以前的轮次见 `reverse-base6cdfd51.log`、`reverse-base7ccc703.log`（只有 R1、R2，结论相同）。
跑完副本已改回原样并重编，`GlazeController.cpp` 的散列与工作树一致。

## 4. 开销（`run-bench.sh`，107，GCC 15.2 与测试同一套编译选项，绑在一个核上）

`refbench.cpp`：整条链 `CeladonChain`，192 kHz / 块 32，GLAZE 60，合成的 8 s 循环素材。先播放 90 s 让 GLAZE 聆听、校准到锁定，
再计时 60 s（36 万块）。两个版本只差 `GlazeController.o`（改前 = 父提交 `af88861`，改后 = 本线），其余目标文件相同。
改前、改后交替跑 5 轮，看中位数和逐轮成对差值。日志见 `bench.log`。

| 场景 | 每块调 makeReference | 改前中位数 | 改后中位数 | 成对差值中位数（最小～最大） | 省下 |
|---|---|---|---|---|---|
| 常态播放（锁定） | 2 次 | 22712 ns（13.627 %） | 22534 ns（13.520 %） | 296 ns（122～325），5 轮全为正 | **0.178 个百分点** |
| 读回存档、执行快照沿用中 | 3 次 | 22900 ns（13.740 %） | 22545 ns（13.527 %） | 357 ns（313～471），5 轮全为正 | **0.214 个百分点** |

以前的轮次（`bench-base6cdfd51.log`、`bench-base7ccc703.log`）：常态省 0.107 / 0.102 个百分点，读回场景省 0.190 / 0.230 个百分点，
每轮都是 5 轮全为正。整链成对差值单轮能差 ±100 ns 以上，所以三轮合起来看：**常态每块省 0.1 ～ 0.18 个百分点，读回场景
每块省约 0.2 个百分点**。

`callbench.cpp` 量单次调用。重算用的是真 `glaze::setManualConditions`；命中路径的键比较，是从 `7ccc703` 时的 `samePlanningManual`
逐字照抄的，不含 Mast 段的比较（Mast 段关着时它只多比两个浮点，几 ns），外加整份拷贝：

| 手动设置 | 重算（底 `af88861`） | 重算（`7ccc703`） | 重算（`6cdfd51`） | 命中 |
|---|---|---|---|---|
| Clay 默认（最常见） | 110 ns | 109 ns | 121 ns | 6.6 ns |
| Clay 三项都「手动」 | 162 ns | 165 ns | 174 ns | 6.6 ns |

按单次开销推算：常态每块省 2 × 104 ≈ 207 ns，读回场景 3 × 104 ≈ 311 ns，和整链实测同一量级。
48 kHz / 512（每秒 94 块）下每秒只省约 20 µs，约 0.002 个百分点，可以忽略；这项优化只在小块、高采样率下有意义。
