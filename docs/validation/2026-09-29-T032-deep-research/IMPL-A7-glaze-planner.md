# T-032 实现报告 · A7-glaze-planner（GLAZE 决策 / 常数 / 方案序列化）

- 日期：2026-09-30（PDT，本机时钟 08:10 开始验证、08:3x 收尾）
- 执行：Opus 5.5（max），接续 Fable 5.1 因额度中断留下的半成品（主进程 WIP 提交 `6a2e7f6`）
- 工作树 / 分支：`.claude/worktrees/wf_abb55518-7ae-6` / `worktree-wf_abb55518-7ae-6`；改前底版 `6a2e7f6~1` = `38f0c42`（源码同 main `e6b445b`）
- 任务书：`BRIEF-A7-glaze-planner.md`（同目录），逐条以「核验调整后的修法」为准
- 验证全部在 107 上（`scripts/test-linux.sh`，`-j 3`），本机未编译、未跑测试

## 0. 结论一览

| 项 | 状态 | 一句话 |
|---|---|---|
| R6-01 自动 Heat 只承担真能削的峰 | 完成 | 算法 / 标定 7 → 8，版本门看快照；`heatUsefulCrestDb` 0.1 dB；底量挪成风格常数（出厂 12 / 12 / 12）；出厂常数下满强度自动 Heat 70 / 50 / 20 → 12（**可闻变化，待试听**） |
| R6-02 「受手动限制」分层 | **部分完成（有意偏离任务书）** | `Plan::automaticLevelWithdrawn` + 序列化 + limitPlanTo 取或都做了；七处门只改了画像失配一处。其余六处与两个掩码第 16 位若按任务书改看新标志，107 实测会把自动增益磨光（第 3 节），保留 `limitedByManual`，等保护层与上游监视改成按自动部分核对（跨文件） |
| R6-06 满强度增益受量程约束 | 完成 | 量程 = 24 − 手动 Gain；放不进时 G_full′ 回算成量程顶值能交付的净增益、标受约束；修了前任的「同一次尝试内二次调用」缺陷 |
| 核验漏项 heatBudget 语义 | 未改（按任务书） | 列待主进程 / Max |
| R6-03 LRA 未知偏温润 | 未改（按任务书） | 列待 Max，附核验的两条候选 |
| 逐位契约 | 保住 | 旧快照（算法 7 / 6）各 534,600 份方案、GLAZE 0 的 59,400 份方案、无超限且量程内的 1,600 份方案改前改后摘要逐位相同；LegacyFixture 通过 |
| 107 全套 | **133144 过 / 0 败**（主进程 2026-09-30 08:4x 代跑最终树 `c480b7d`） | 编译警告 0 |

## 1. 前任半成品的审查与改正

前任（WIP `6a2e7f6`）的方向大体对，逐条核过后保留：版本门与 `heatUsefulCrestDb`、底量挪进 `StyleConstants` 与 `blendConstants`（三者逐位相同时直接取原值，保旧快照逐位）、快照读写（写三个风格字段 + 旧的全局字段取三者最小；读时风格字段缺省取全局值）、`xmlio` 的布尔 `optionalField`、`Plan::automaticLevelWithdrawn` 及其缺省取 `limitedByManual`、`limitPlanTo` 取或、R6-06 的量程顶值与回算。改正与补齐的：

1. **R6-06 的二次调用缺陷**（`GlazePlanner.cpp` `makeFullPlan` / `completeFullPlan`）：同一次尝试里 `completeFullPlan` 可能调两次（先带 Heat 底量、失败后撤回底量再调）。前任只在每次尝试开头把 `full.gainDb` 复位，第二次调用会从第一次回算过的 G_full′ 起算；第一次置的 `constrained = true` 也会留下。改为：`completeFullPlan` 显式接收容量约束后的 G_full′、每次调用都复位 `gainDb` 与新的 `rangeLimited`，尝试循环结束后才把 `rangeLimited` 并进 `constrained`。算法 ≤ 7 时两者恒为原值，逐位不变（摘要证据见第 6 节）。
2. **R6-02 的七处门**：前任按任务书把七处门与两个掩码第 16 位全改看新标志。我先保留它跑了一轮新写的控制器用例（第 3 节表），实测会把自动增益磨光，于是把 `GlazeController.cpp` 恢复到改前版本，只改画像失配一处门，另把任务书点名的两处注释（原 2159–2161、2573–2575）改成真实理由。
3. **两个用例只有调用没有定义**：`testHeatOnlyClaimsRealCrestReduction`、`testGainRangeBoundsEffectiveGain` 在 `runTest` 里调用却没写，编不过。已写全（第 2、4 节）。
4. **注释与实际不符**：`GlazeTypes.h`、`GlazePlanner.h`、`GlazePlanner.cpp`、`GlazeProfile.cpp` 里「编排器的校准、保护层、约束反馈与画像失配只看 automaticLevelWithdrawn」按实际语义改写；`completeFullPlan` 注释里的举例数字按实测改正（I −40 时 Glue 补偿承担了一部分，过报没有「30 对 24」那么多，改用 I −46 的例子）。
5. **`GlazeTests::testStylesLeanOnTheirModules` 没跟着改**：它仍断言「温润 Heat 最多、通透 Heat 最少」，这两条同样靠 Heat 顶到上限成立。改成与 `testStyleDirections` 同一口径：上限顺序温润 > 冲击 > 通透，各风格自动 Heat ≤ 自己的底量 × 强度。

## 2. R6-01 自动 Heat 只承担真能削的峰（算法 8）

**改动位置**

- `GlazePlanner.cpp` `makeFullPlan`：`ctx.c->algorithmVersion >= 8` 时，`achievable = row.at (row.crest, cap)`，达到 `heatUsefulCrestDb` 才用 `inverse (crest, min (shareHeat × E, achievable))`，否则不为削峰开 Heat、交给风格底量；门槛 ≤ 0 或非有限按「削不了」。算法 ≤ 7 走原式。底量改读 `sc.automaticHeatFloor`。`blendConstants` 对三种风格逐位相同的底量直接取原值。
- `GlazeConstants.h`：`StyleConstants::automaticHeatFloor`（默认 0 = 中性占位）、`Constants::heatUsefulCrestDb = 0.1f`；去掉全局 `Constants::automaticHeatFloor`。`xmlio` 加布尔可缺省字段。
- `GlazeConstants.cpp`：出厂 `version = 8`、三种风格底量 12；头注释表加一行；快照读写 `visitHeatFloor`（写：全局 `automaticHeatFloor` = 三者最小 + 三个风格字段；读：全局缺省 0，风格字段缺省取全局）；`heatUsefulCrestDb` 可缺省（旧快照读 0，但它们永远不进新分支）。
- `GlazeTypes.h`：`currentAlgorithmVersion` 7 → 8，注释写明按快照版本分流、`oldestCompatibleAlgorithmVersion` 保持 6 的理由。

**测试**（`GlazePlannerTests::testHeatOnlyClaimsRealCrestReduction`，107 结果）

1. 性质扫描 3 风格 × I {−24, −18, −14, −10, −7} × PLR {8, 12, 16} × 置信度 {1, 0.5} × GLAZE {35, 100} = 180 份：自动 Heat 全部 ≤ 底量 × GLAZE × κ（最大 12.000）；72 份两边都不受约束的方案与「换算表响度 / 峰值两列清零」逐位相同；底量都给了。
2. 连续：温润 I −24，TP −16.8 → −16.2 每 0.01 dB 一步，GLAZE 35 / 100 相邻两步 Heat 最大变化 0.0000（修前在 −16.5 处 12 → 70）。
3. 版本门：同一份出厂常数标成算法 7，Heat 照旧顶到上限；旧分支不看门槛；算法 8 的坏门槛（0、−1、NaN、+Inf）都按「削不了」。
4. 削峰型表（峰值随 Heat 线性下降、Heat 100 削 1 dB）：新规则 Heat 51.896 = 旧规则 51.896（逐位），不是一律只给底量；门槛抬到 5 dB → 12。
5. 底量是风格常数：标成 12 / 8 / 4 时三种风格分别给 12 / 8 / 4，半温润半通透给 8。
6. `testStyleDirections`（前任改）与 `testConstantsRoundTrip`（前任加 v7 快照读回、风格底量往返、全局字段取最小）照过；`GlazeTests::testStylesLeanOnTheirModules` 按第 1 节第 5 条改写后照过（四种风格自动 Heat 都是 12.0）。

**可闻变化（待试听）**：新实例或重新聆听后，出厂常数下满强度自动 Heat 由 70 / 50 / 20（温润 / 冲击 / 通透）变为 12 / 12 / 12；GLAZE 35 时由 24.49 / 17.49 / 7.00 变为 4.198（摘要程序实测，手动中性、置信度满的网格最大值）。旧工程（算法 6 / 7 快照）不变。

**潜在注意点（不改，留给将来重测换算表的人）**：门槛是硬开关。出厂残差表的 crest 容量最多 0.00023 dB，永远碰不到 0.1 dB，所以没有跳变；将来若重测出一张「能削一点峰」的表，而某个电平的 crest 容量正好跨过 0.1 dB，Heat 会在底量与削峰份额之间跳。到时候要么把门槛换成斜坡，要么核一下各电平的容量离门槛够远。

## 3. R6-02 「受手动设置限制」分两层（部分完成）

**已做**

- `GlazeTypes.h` `Plan::automaticLevelWithdrawn`：手动已让限幅器超约束、Limit 旁通、约束反馈上限低于手动链净增益三种情况为真（`GlazePlanner.cpp` `makePlan` 填值）；`limitPlanTo` 取或。
- `GlazeProfile.cpp` `visitPlan`：可缺省读写，缺属性取同一方案的 `limitedByManual`（旧语义，保守）；在但不是 0 / 1 拒收。已提交版本与执行快照共用这张字段表。
- `GlazeController.cpp`：**只改画像失配一处门**（原 1845 行 `updateMismatch`）改看 `automaticLevelWithdrawn`；原 2159–2161、2573–2575 两处注释改成真实理由。其余 1588 / 1621 / 1633 / 1922 / 2160 / 2576 与两个掩码第 16 位**逐字未动**，仍看 `limitedByManual`；`GlazeController.h` 的掩码位说明因此仍准确，无需改。

**为什么没按任务书改全七处（107 实测）**

保护层（`GlazeGuard`）按 Glue 均值 / p95、Limit、Clip 的**总量**与风格上限核对；上游监视（`UpstreamMonitor`）按实际 Heat 驱动与 Glue GR 的**总量**核对。两者都分不出哪一份是手动造成的。手动单独超某一项预算时若照常开工，就会把手动的超出当成自动的超限：保护层白白退让自动 Gain（Glue 在 Limit Gain 上游，退了也收不回），约束反馈每「过渡 + 3 s」降 1 dB 净增益上限，直到降到手动链——上限只降不升、随工程保存，自动增益被永久磨光。研究与核验都漏了这一层。

同一组新用例（`GlazeTests` 第 9 组，实时链、fastConstants 时长）在两种门下的结果：

| 场景 | 任务书写法（七处门全改，前任代码） | 本次写法（只改画像失配） |
|---|---|---|
| 通透、GLAZE 100、手动 Heat 25（上限 20），学完后 30 s | 学完时上限已被降到 2.71 dB，30 s 里上游监视再触发 10 次、上限降到 0；自动 Limit Gain 1.66 → 0 dB；校准开了 5 轮 | 上限不变；自动 Limit Gain 3.71 dB 不变；反馈 0 次；保护层 0 dB；校准 0 轮（与改前同） |
| 同上，再放一段响 12 dB 的音乐 | 画像失配 0 次（上限已被磨到底，降不动了） | 画像失配 1 次（改前代码这道门看 `limitedByManual`，直接不判） |
| 通透、GLAZE 100、手动 Glue 100，学完后 20 s | 保护层退让；上限 2.12 → 1.26；自动 Limit Gain 降到 0；上游 / 方案受限反馈各 1 次 | 保护层 0 dB；上限不变；自动 Limit Gain 0.49 dB 不变 |

所以本次交付的行为变化只有：**手动单独超某一项预算时画像失配照常判定**（响段照样压）。保护层、校准、约束反馈仍在「受手动设置限制」时停，与修前相同，不引入新问题。要把它们也打开，先要让保护层与上游监视只核对自动部分（第 8 节跨文件需求），之后把那六处门与两个掩码位改看 `automaticLevelWithdrawn` 即可（数据与存档这次都已备好）。

中间方案也试算过、没有采用：只对「手动 Heat 超预算」打开保护层与校准（保护层不核对 Heat）、约束反馈仍关。问题是上游监视在这段时间里照样累计手动造成的 Heat 超出；手动 Heat 拧回上限以内的那一刻反馈门打开，旧窗口立即触发一次 −1 dB（只降不升）。门本身消不掉这个残留，必须改监视窗，所以一并列进跨文件需求。

**测试**

- `GlazePlannerTests::testManualLimits`：heavyHeat / heavyGlue `! automaticLevelWithdrawn`，heavyGain / 旁通 / 约束反馈上限低于手动链三种为真（前任写，照过）；新加 limitPlanTo 取或：手动 Glue 90 时低置信手动链净增益 4.752 dB、置信拉满 5.400 dB，上限取两者之间 → 低置信方案自己不撤回、封顶方案撤回，合起来两个标志都置、自动 Limit Gain 0。
- `GlazePlannerTests::testBrokenDataRejected`：已提交版本缺属性取 `limitedByManual`、在但非 0 / 1 拒收（前任写）；新加执行快照的 PLAN 缺属性同样取 `limitedByManual`。
- `GlazeTests` 第 9 组三个用例（`testManualHeatOverBudget` / `testManualGlueOverBudget` / `testAutomaticLevelWithdrawn`）：见上表；另断言手动 Heat 拧回 15 的头 0.9 s 不因旧监视窗触发反馈；手动 Gain 20 时偏差、保护、校准、画像失配都不动作、Limit Gain 恰为手动值。

## 4. R6-06 满强度增益受 Limit Gain 量程约束（算法 8）

**改动位置**：`GlazePlanner.cpp` `Context::manualGainDb`（`makePlan` 在满强度方案之前填手动 Gain）；`completeFullPlan`：量程 `capGain = 24 − 手动 Gain`，定点迭代按量程顶值算超出量，`gain > capGain` 时 `full.gainDb = estimateLimit (ctx, color, capGain, clip).netDb − 中性链`（只降不升）、置 `rangeLimited`，顶折点 = 量程顶值；算法 ≤ 7 原样。判定「放不进」与不钳量程时的结论相同（定点序列越过 capGain 之前逐位一样，越过之后单调变大）。

**测试**（`GlazePlannerTests::testGainRangeBoundsEffectiveGain`，107 结果）。「能交付的净增益」取 GLAZE 0 的方案、把实际交给模块的参数当手动值喂进去（只用公开接口）：

- 6 组素材 × 3 风格 = 18 份，全部用满 24 dB、标受约束、目标 = 能交付。改前改后的实测（摘要程序同一对二进制，温润 / 冲击 / 通透）：

  | 素材 | 改前目标 dB（受约束） | 改后目标 dB（受约束） | 改后自动 Gain 增量 |
  |---|---|---|---|
  | I −40 / TP −28，手动 0（任务书例） | 30.63 / 31.00 / 30.00（1 / 0 / 0） | 29.72 / 29.76 / 29.64（全 1） | 24 |
  | I −46 / TP −34，手动 0 | 36.63 / 37.00 / 36.00 | 30.00 ×3 | 24 |
  | I −52 / TP −40，手动 0 | 42.63 / 43.00 / 42.00 | 30.00 ×3 | 24 |
  | I −30 / TP −26，手动 12 | 33.50 / 33.00 / 32.00（全 0；自动增量 21.5 / 21 / 20，叠加后被钳到 24） | 24.00 ×3 | 12 |
  | I −36 / TP −32，手动 6 | 33.50 / 33.00 / 32.00（增量 24，叠加后钳到 24） | 24.00 ×3 | 18 |
  | I −28 / TP −26，手动 18 | 37.50 / 37.00 / 36.00（增量 19.5 / 19 / 18） | 24.00 ×3 | 6 |

  改前「能交付」与改后目标相同（同一前向模型），所以改前过报 0.4～13 dB，且手动 Gain 那三组连受约束都不标。
- 量程以内（I −30 / TP −24、手动中性，请求 20 dB）：不受约束、目标与请求逐位相等。
- 手动 Gain 0 → 16 dB 每 0.25 dB 扫描（I −30 / TP −26，请求 20）：目标相邻两步最大变化 0.25 dB、单调不减，跨量程边界不跳。
- 任务书第二例「I −24 / 手动 Gain 12」只记录不断言：真实峰值（TP −14）下手动 Gain 已把峰值推进限幅器，强度由既有的约束核对压低，满强度用不到量程顶（实际 Limit Gain 15.76），改前改后逐位相同（18.42 / 20.20 / 18.45）；目标 18.45 与能交付 17.26 之差 1.19 dB 来自「手动链净增益 + 强度 × G_full′」的叠加近似（校准器收口），与量程无关。另外超限估计 > 0 时自动 Glue 的补偿会承担一部分请求（I −40 时约 6 dB），所以任务书 I −40 一例改前只过报 0.4～1.8 dB，不是「宣称 30、只给 24」；过报随素材变安静或手动 Gain 变大而变大。

## 5. 测试与验证（107，`-j 3`）

| 轮 | 代码 | 命令（`scripts/test-linux.sh bash -c '…'`） | 结果 | 警告 |
|---|---|---|---|---|
| 1 | 前任七处门 + 新用例 | `--only GlazePlanner,GlazeChain,LegacyFixture` | 4700 过 / 8 败（全是第 9 组新用例，即第 3 节左列的磨光证据） | 0（全量编译） |
| 2 | 本次写法 | `--only GlazePlanner,GlazeChain,LegacyFixture,GlazeController,ClayGlaze,GlazeCalibration` | 7943 过 / 1 败（新写的 Glue 用例阈值拍错：稳态自动 Gain 0.49 < 0.5，改成以学完时为基准） | 0 |
| 3 | 最终代码（`c480b7d`，主进程代跑） | 全套（不带 `--only`，`-j 4`） | 133144 过 / 0 败 | 0（`grep "warning:"` 7 行全是测试名里的 "phase warning" 等，排除后 0） |

构建命令：`cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3`，警告数 = `grep -c "warning:"` 构建日志。

## 6. 逐位证据（改前改后摘要对照）

临时程序（不进仓库，107 上 `~/.cache/celadon-t032/A7-digest/`，用完已删）：把改前（`38f0c42`）与改后两份 `GlazePlanner.cpp` + `GlazeConstants.cpp` 各编一个程序，编译选项照工程（`-std=c++17 -ffp-contract=off`、Release）。网格：I 9 档（−52 ～ −5）× PLR 5 档 × LRA 2 × 置信度 2 × 有无立体声特征 2 × 风格 5（含锁定混合权重）× 手动 11 种（中性、Gain 12 / 20、Heat 25、Glue 60、Clip 50、Limit 旁通、Color 旁通、Mix 50 + Output 2、组合、Balance / Tone）× GLAZE 9 档（0 ～ 100）× 约束反馈上限 3 档。方案公共字段按 IEEE 位做 FNV-1a 64（`algorithmVersion` 是代码版本号，按设计不同，不进摘要）。改后 `GlazePlanner.cpp` 的 SHA-256 前缀 `035048b35e72e784`，与提交的一致。

| 类 | 输入 | 份数 | 改前 | 改后 |
|---|---|---|---|---|
| 旧快照 · 算法 7 | 出厂常数标成 version / algorithm 7 | 534,600 | `46127efdfba39743` | `46127efdfba39743` |
| 旧快照 · 算法 6 | 标成 6（v0.1.6.2） | 534,600 | `647fdb73ee94f8c3` | `647fdb73ee94f8c3` |
| GLAZE 0 | 出厂常数（改前 7、改后 8；常数版本号不进摘要） | 59,400 | `fa8e07c34272a903` | `fa8e07c34272a903` |
| 无超限、量程内 | 出厂常数、手动中性、置信度满、输入峰值与满强度请求都不过 Ceiling、请求 < 20 dB | 1,600 | `7a1657d51d1a214f` | `7a1657d51d1a214f` |

整链层面的逐位契约由既有用例覆盖、本次都过：GlazeChain「GLAZE 0 与手工串起来的模块逐位相同（90 个场景）」、ClayGlaze「GLAZE 0 逐位」与旧工程、LegacyFixture「v0.1.6.2 存档前 20 块 7 个实际参数逐位」。

## 7. 没做的与原因

1. R6-02 的六处门与两个掩码第 16 位没改看新标志：理由与证据见第 3 节，需先做第 8 节第 1 条。
2. 任务书 R6-02 的控制器用例「手动 Heat 25 → executedGuardDb < 0、calibrationBegins ≥ 1」没写成断言：本次写法下保护层与校准仍停（有意），改成断言「不磨光、画像失配照判、拧回时不误触发」。第 8 节第 1 条做完后应补上这两条断言。
3. `Status::limitedByManual` 细化成模块位掩码（研究的「可再」项）：不在核验调整后的修法里，要动界面，没做。
4. 核验漏项 heatBudget 语义、R6-03：任务书明定不改，见第 10 节。
5. 我负责的文件之外的档案（日志、TASKS、INDEX）没写：不在允许范围，见第 9 节。

## 8. 跨文件需求

1. **（GlazeController.cpp / GlazeGuard，A6 或主进程）保护层与上游监视只核对自动部分**：上限取「风格上限」与「手动单独用量」的大者（Glue 均值 / p95、Clip 深度 / 活动、Heat 驱动；手动单独用量可由决策器按手动链模型给出，我可以在 `GlazePlanner.h` 加一个纯函数供 `updateGuardLimits` 调用），并在某一项的「手动超预算」状态变化时清空上游监视窗。做完后把 1588 / 1621 / 1633 / 1922 / 2160 / 2576 与 `calibrationInterruptMask` / `calibrationGateMask` 第 16 位改看 `Plan::automaticLevelWithdrawn`，同步 `GlazeController.h` TestProbe 第 16 位说明，并在 `GlazeTests` 第 9 组补「保护层动作过、校准开过」两条断言。
2. **（主进程）档案**：`02-日志/2026-09-30.md`、`04-协调/TASKS.md`、`00-总览/INDEX.md` 的 T-032 A7 记录（第 9 节）。

## 9. 档案

已改（Edit 增量，行数 135 / 100）：

- `01-框架/设计规格-GLAZE.md` 7.4「版本」段：版本演进写到 8，加「8（T-032）」五条（Heat 规则、风格底量、量程、按快照分流、可闻变化待试听）。
- `01-框架/设计规格-GLAZE-实现修订.md`：§3「不施加偏差也不施加保护」补上两种原因；末尾加「10. 2026-09-29 修订（T-032）」（受手动限制的分层、画像失配、保护层 / 校准 / 反馈为何仍停与 107 实测、待办、存档字段）。

待主进程补：当日日志（做了什么、R6-02 偏离任务书的证据、可闻变化待试听）、TASKS（A7 行移已完成；新增待认领「保护层 / 上游监视按自动部分核对，之后放开 R6-02 六处门」；待 Max 四条）、INDEX「当前重点」。

## 10. 待 Max / 待主进程

1. **试听 R6-01 的可闻变化**：温润 GLAZE 100 自动 Heat 70 → 12、默认 35 % 24.5 → 4.2；冲击 50 → 12、通透 20 → 12。
2. **风格 Heat 底量要不要分风格**：出厂先都填 12；研究给的候选 12 / 8 / 4（温润 / 冲击 / 通透）只作起点，改了标定版本 + 1。
3. **R6-03（LRA 未知偏温润）**：核验的两条候选都挂算法 8 之后的版本门——(a) 中性先验 `dynamicEff = lerp (0.5, ramp(LRA), lraTrust)` 同时进温润项与通透项（类音乐 LRA 0.5 / 置信 0.2 → 约 0.39 / 0.24 / 0.37，古典 LRA 15 / 置信 0.25 → 约 0.24 / 0.09 / 0.67）；(b) 提交后继续累计 LRA，置信首次 ≥ 0.8 时重算一次权重（要改时间线与保存规格）。研究原式会让「自动」在标准时间线上几乎选不到温润，不可用。
4. **heatBudget 语义（待主进程 / Max）**：`makePlan` 用 `fullPlan.heat > 0` 判「自动在用 Heat」，出厂表下底量让它恒为真，手动 Heat 只要超风格上限（通透 20）就撤回底量并亮「受手动设置限制」。可改成只在自动真拿 Heat 削峰时才算预算，但会改界面语义。
5. **R6-02 剩余部分的范围**：要不要本轮就扩 A6 的范围做第 8 节第 1 条。
