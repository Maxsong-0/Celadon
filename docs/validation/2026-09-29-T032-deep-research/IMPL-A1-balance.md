# T-032 实施报告 · A1-balance · Balance 动态频谱平衡

> 2026-09-30（PDT），实施代理 Opus 5.5 · max，接续因额度中断的前任（Fable 5.1）。
> 工作树 `.claude/worktrees/wf_abb55518-7ae-1`，分支 `worktree-wf_abb55518-7ae-1`。前任半成品 = WIP `34728fd`；改前的底 = `34728fd~1`（`38f0c42`，源码与 main `e6b445b` 相同）。
> 任务书 `BRIEF-A1-balance.md`，研究 / 核验 `R2-balance.md` / `V2-balance.md`（同目录）。只在 107 编译测试，本机 Mac 未编译、未跑任何测试。

## 1. 结论一览

| 条目 | 做了什么 | 输出 | 证据 |
|---|---|---|---|
| R2-01 刺耳超出量 | 本段改用 60 ms 快电平；起音比较改用每 hop 更新的 `previousBandPower`（删 `masteringPreviousPower`）；保持计时在 h 关掉时照常走完 | 只在 h > 0 时变 | 阶跃峰值 3.000 → 0.000 dB；鼓点均值 1.820 → 0.018 dB；打开第一 hop 0.260 → 1.042 dB |
| R2-05 掩蔽距离 | `maskingMaxDistance` 4 → 3 | 逐位不变 | 四个采样率摘要（R2-07 单独退回的中间构建）与改前相同 |
| R2-06 重复计算 | pow 提出循环、均值读 `quickLevelDb`、系数与 A 计权权重 prepare 时算、功率谱只到末段 + 3 bin；**逐段 `measured` 保留原式** | 逐位不变 | GCC 与 clang + FMA 两套构建改前改后摘要相同；照任务书改 `measured` 则 clang + FMA 下不同 |
| R2-07 重算间隔 | 间隔 = 64 << (order − 11) = hop / 8，拆片间距取一半 | 44.1 / 48 k 不变，≥ 88.2 k 变 | 44.1 / 48 k 摘要相同；96 / 192 k 只有输出变、G 与帧不变；发布时刻钉死在 hop / 8 |
| R2-08 起音恢复 | 起音侧仍立刻乘 1 − p；保持结束后乘数 20 ms 一阶爬回 1；余尾等乘数回到 1 才停 | 只在 h > 0 时变 | 保持结束第一 hop 升幅 1.437 → 0.638 dB |
| 核验漏项 | 镲片 150 ms 突发只记录；Mast 哈希不加盐，规格写明 h > 0 旧校准需重新聆听 | — | 见第 3、7 节 |

测试：107 GCC 15 Release 全套 **133019 过 / 0 败**，干净重编 **0 警告**（改前同样 0）。

## 2. 逐条与关键改动位置

行号指提交后的 `Source/dsp/BalanceModule.cpp`。

### R2-01 推响不刺的超出量（high / P0）

- `:1792` `excess = max (0, quickLevelDb[b] − ½ (left + right) − 1.5)`：本段与两邻段同一个 60 ms 动态，整体变化相消。
- `:1538-1554`（段循环里、`previousBandPower[b] = bandPower[b]` 之前）：起音判定挪到这里，与自动 Tame 共用每 hop 更新的上一帧功率；条件与原来相同（h > 0、2 ～ 8 kHz 窗口内、门上）。`masteringPreviousPower` 删除（grep 全部读写点：reset、母带块读、母带块写三处，都已改掉）。
- `:1547` 的 else 分支：h 关掉（含余尾）或本段落到门下时，保持计时照常递减到 0，不再冻结。原来冻结的剩余时长会在下次打开时让头几 hop 按起音处理——与陈旧的上一帧功率是同一类问题，一并修。
- h = 0 起的实例：保持计时恒为 0、母带块整段跳过，逐位不变（`AdvancedMasteringChain`「GLAZE zero … bit-identical」与 `BalanceCrossPower` 摘要均相同）。
- 测试（`tests/AdvancedMasteringTests.cpp`）：
  - `testHarshIgnoresLevelStep`（任务书指定）：粉噪 −20 dBFS、h = 1，6 s 处 −10 dB；断言阶跃后 2 s 峰值 < 0.3 dB。改后 0.000（阶跃前 2 s 0.031）；改前 3.000、> 1 dB 共 0.747 s（红）。
  - `testHarshIgnoresDrumPulses`（任务书指定）：250 ms 周期、50 ms −12 / 其余 −30 dBFS；断言读数均值 < 0.3 dB、2 ～ 8 kHz 各段 G 均值 > −0.3 dB。改后 0.018 / −0.011；改前 1.820 / −1.761（两条都红）。
  - `testHarshSwitchOnHasNoStaleOnset`（本线新增，覆盖上一帧功率与保持计时两处改动）：持续 3.15 kHz 峰素材，第 281 块起 h = 1；第 468 块一次 ×3 突发开起音保持，第 472 块关 h，余尾归零后第 750 块再开。断言两次打开的第一 hop 读数 > 0.7 dB，阳性对照（从没开时读数精确 0、关之前两 hop 确实被闪避、再开前余尾精确 0）。改后 1.042 / 1.042；改前 0.260 / 0.260（两条都红）。
  - 原有「persistent 3.15 kHz peak」「transient-preserve changes burst attack samples」照过。

### R2-05 上行掩蔽距离（low / P2）

- `:115` `maskingMaxDistance = 3`（d = 4 需间隙 ≥ 18.7 dB，上限 16 dB，永远 `continue`，那一轮全是局部量、无副作用）。
- 「upward masking lifts a buried higher neighbour …」改前改后读数相同（both −7.55 / −6.68 dB，tame only −8.00 / −8.00，recover only 0.00 / 0.00）。逐位证据见第 4 节（摘要用例 Recover / Tame 100，素材 500 / 1000 Hz 事件让距离 1 ～ 3 的掩蔽与等响锚定都在工作）。

### R2-06 每 hop 的重复计算（low / P2）

- `:1281` 共同电平的 `pow` 提到 27 段循环前（第 6 步重定中心在循环之后，循环内 C 不变）。
- `:1774` 目标频谱加权均值改读 `quickLevelDb[b]`：「均值 += w · 电平」两边乘加形态相同（乘积两个因子逐位相同），合并与否都一样。
- **`:1821` 逐段 `measured` 保留原式 `10·log10 (…) − 均值`（偏离任务书，见第 5 节第 1 条）**：clang 默认 `-ffp-contract=on`（Xcode 的 Apple Silicon 正式构建）把「乘 10 再减均值」合并成一次乘加、只舍入一次；改读已舍入的 `quickLevelDb` 再相减会差最后一位，t > 0 时不再逐位。核验「对函数调用不合并」的理由不成立——合并的是乘 10 与相减，不是 log10。代价：只在 t > 0 时每 hop 最多 23 次 log10。
- `:380-398` prepare 时算好：掩蔽提升 0.18 s、刺耳攻击 25 ms / 释放 200 ms、目标 2 s / 0.15 s 两份一阶系数（与原来每 hop 的 `std::exp (-hopSeconds / τ)` 同一表达式），A 计权掩蔽权重 `maskingWeight`（同一函数），刺耳频率权重 `harshBandWeight`（同一表达式，窗口外为 0 兼作「不参与」标记）。
- `:366` `analysedBinCount = min (fftSize/2 + 1, 末段首 bin + 末段 bin 数 + 3)`；`:996` 清零与 `:1067` 累加只到它。`powerSpectrum` 的全部读点（段求和、谐波保护的峰 ± 3）都在界内，更高的 bin 自 reset 起恒为 0、无人读。

### R2-07 系数重算间隔（low / P2）

- `:286` `coefficientUpdateSamples = 64 << (order − 11)` = hop / 8（44.1 / 48 k 仍 64，96 k 128，192 k 256）；`:640` 用它；`:1002` 拆片间距 = 一半。三片落在 hop 后 0 / 1/16 / 1/8 hop，末片仍在第一次用到新目标的那次重算的同一样本上、先执行 → 拆片 = 一口气仍逐位。
- 测试（`tests/BalanceTests.cpp`）：`publishDelayFor` = hop / 8，`phaseAfterHop` 新增；拆片用例的阳性对照收紧为「拆片版最晚一个还读到上一帧的读数正好在相位 hop / 8」（`:1651`）。它与「窗外读数全部逐位相同」合起来把发布时刻钉死在 1/8 hop：改前代码与「R2-07 单独退回」的中间构建在 96 / 192 k 读到 64，红；改后过。「output independent of block size at 192 kHz」照过。
- 性能（107，同机不同时刻，有噪声）：192 k / 32 一口气均值 2.693 → 2.659 %（R/T 0）、3.237 → 3.185 %（R/T 40）；拆片最坏块 149 → 146 µs、150 → 148 µs，比值 0.50 / 0.51 不变（门槛 0.65）；48 k 噪声 Balance 60 / 60 0.691 → 0.688 %，梳状 0.822 → 0.814 %。只降不升。

### R2-08 起音保留的恢复侧（low / P2）

- `:1804-1812`：`duckTarget` < 当前乘数时立刻降（起音侧硬乘保留）；否则以 20 ms 一阶爬回，差 1e-6 内归 1（乘数为 1 时与原运算逐位相同）。`harshDuck` 在 reset 时为 1。
- `:1830` 余尾条件加 `harshDuck[b] != 1.0`：h 回 0 时若正在保持里，乘数走完爬升再停块，不把陈旧的闪避留给下次打开。
- 测试 `testHarshOnsetRecoveryIsSmoothed`：持续峰 + 每 0.5 s 一次 ×3 突发，h = 1、p = 0.5。改后保持结束后最大单 hop 升幅 0.638 dB（改前 1.437，红）；阳性对照稳态 3.000 dB、起音闪避最深 1.266 dB（起音侧仍立刻闪避）。门槛 0.75 dB 的来历见第 5 节第 2 条。

## 3. 核验漏项

- **镲片式 150 ms 突发**（`testHarshCymbalBurstObservation`，只记录）：粉噪 −20 dBFS 底 + 每 0.5 s 一次 150 ms 的 3.5 ～ 9 kHz 突发（4 ～ 8 kHz 各段抬约 14 dB），h = 1。改后 4.0 s 那次：突发中峰值 2.03 dB（+55 ms 0.47、+110 ms 1.26、+150 ms 2.03），突发后 150 ms 内峰值 2.11 dB，随后按 200 ms 释放；逐 hop 读数 `0.82 0.78 0.74 0.36 0.38 0.44 0.47 0.48 0.49 0.50 0.94 1.26 1.51 1.74 1.92 2.03 2.10 2.11 2.11 2.09 2.07 …`。改前同一素材整段压在约 3 dB（突发后电平下降被误判），`3.00 3.00 3.00 1.45 … 2.98`。结论：修后亮瞬态仍会被削到约 2 dB（h = 1），是否加持续时间门槛待 Max。
- **GLAZE 校准「Mast」段没有算法版本盐**：按主进程要求不改哈希。规格（设计规格-高级母带控制 第 3 节与末节）写明「推响不刺 h > 0 时存下的旧校准仍会被当成有效，需重新聆听」。

## 4. 逐位证据（FNV-1a 64 位摘要，107）

摘要取自各次运行日志（十六进制，省去 0x）。「改前」= 基线 `BalanceModule.{h,cpp}`（`34728fd~1`）+ 本次的测试文件，在 `~/.cache/celadon-t032/` 的独立副本里构建；「中间」= 本次代码但 R2-07 单独退回（间隔固定 64）。三者同一台机器、同一编译器、同一选项。

**`BalanceCrossPower`「logged digests」**（h = t = 0；Recover / Tame 100、自动 40 / 60、Brighten 30、Fast），GCC 15：

| 采样率 | 量 | 改前 | 中间 | 改后 |
|---|---|---|---|---|
| 44.1 k | output / G / frames | 190a1fa966e72f14 / ba2bb069d22d087a / 45d12dda70ff6864 | 相同 | 相同 |
| 48 k | output / G / frames | c87627af5830ecef / 71b19b314bda7ed8 / dbdfcd8964e75864 | 相同 | 相同 |
| 96 k | output | 61bf8f290a34c3e1 | 61bf8f290a34c3e1 | 56fa3f05a67d2a5（日志原样，高位 0 省略；R2-07） |
| 96 k | G / frames | 60f8f86b61efb1a3 / b0dbe04d986364ab | 相同 | 相同 |
| 192 k | output | 247d7b8435955cee | 247d7b8435955cee | d2935f8ce0ba1c10（R2-07） |
| 192 k | G / frames | fd0d1975aff066b1 / fce40f86b289e845 | 相同 | 相同 |

**`AdvancedMasteringChain`「T-032 digests」target**（t = 1、温暖 −1.5 dB / 倍频程、h = 0），GCC 15：

| 采样率 | 改前 output / G | 中间 | 改后 output / G |
|---|---|---|---|
| 44.1 k | bf8dfe2653a79a74 / c9594ebb14ac12b9 | 相同 | 相同 |
| 48 k | f5d21e6e4ee4bcd9 / bfcfb9cf9daaae05 | 相同 | 相同 |
| 96 k | 6bdf9475815e6a87 / 3602c18836c1f12a | 相同 | 979d1c87d85bd898（R2-07）/ G 相同 |
| 192 k | 43cdfcae4e75df85 / 9f5f39f2dff316e5 | 相同 | 2a60b5c25704189b（R2-07）/ G 相同 |

**clang 21 + `-mfma`**（107 的 EPYC 支持 FMA；clang 默认 `-ffp-contract=on`，与 Xcode 在 Apple Silicon 上的合并规则相同）。「前任写法」= 本次代码但 `measured` 改读 `quickLevelDb[b] − 均值`：

| 摘要 | 改前 | 改后 | 前任写法 |
|---|---|---|---|
| target 44.1 k output | 9670ce075c619780 | **相同** | bc87376612cd7d93（不同） |
| target 48 k output | fae4d55992bd8e42 | **相同** | d3b6968dee5167df（不同） |
| logged digests 44.1 / 48 k output | 29c312b015d7311d / 74b8f65f45844e11 | 相同 | 相同 |
| 96 / 192 k | 只有 output 随 R2-07 变，G / frames 三者相同 | | |

harsh / both 两组摘要按设计改变（R2-01 / R2-08），只作记录，见测试日志。

## 5. 前任改动里改正了什么

1. **逐段 `measured` 退回原式**：前任照任务书改读 `quickLevelDb`，GCC（`-ffp-contract=off`）上看不出，但 clang + FMA 下目标频谱 t > 0 的输出变了（第 4 节最后一表）——Apple Silicon 正式构建会破坏「R2-06 逐位不变」。
2. **R2-08 门槛与来历**：任务书的「≤ 0.6 dB」与主进程规定的「10 ～ 20 ms 平滑」不相容：单是乘数一项就是 p·E·(1 − e^(−10.67/20)) = 0.62 dB；保持期间包络也在往 (1 − p)·E 释放，保持结束时 e0 在 0.5E ～ 0.86E，第一 hop 升幅 e1·(1 − p·c) − e0·(1 − p) 在 0.64 ～ 0.68 dB（改前 1.27 ～ 1.44）。前任已取 0.75 dB 但没写清来历；本线在用例注释里写出推导，平滑常数按主进程取 20 ms（区间上限）。若 Max 要 ≤ 0.6 dB，需把平滑放到约 25 ms。
3. **余尾不停在闪避里**：前任的乘数只在母带块运行时更新，h 回 0 且余尾先于乘数归零时会停在 1 以下；已把 `harshDuck != 1` 并进余尾条件。
4. **拆片用例的阳性对照**：前任只断言「窗内有读到上一帧的读数」，注释说「窗口写大了靠这条红」并不成立（窗口写大、或模块实际只晚 64 样本，它照样过）。已改为钉死最晚相位 = hop / 8，并用改前与中间两个构建确认它在 96 / 192 k 会红。
5. **补测试**：上一帧功率每 hop 更新、保持计时不冻结两处改动原来没有任何用例，新增 `testHarshSwitchOnHasNoStaleOnset`（改前红、改后过）。
6. **注释更正**：粉噪用例里「各段与邻段均值差约 −0.5 dB」改为期望约 0 dB（粉噪每个 1/3 倍频程功率相同）；起音计时、目标块测量、头文件 T-032 说明按实际改动重写。
7. **档案规格其实没改**：前任声称改了两份规格，核实时两份文件修改时间仍是 2026-09-29 10:50、没有任何 T-032 字样。本线补上（第 7 节）。
8. 任务书给的 `--only Balance,…` 类名不存在（会报错退出），实际类名是 `BalanceModule, BalanceCrossPower, BalancePerformance, AdvancedMastering, AdvancedMasteringChain`。

## 6. 测试命令与数字

- 定向（改后）：`scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests" --only BalanceModule,BalanceCrossPower,BalancePerformance,AdvancedMastering,AdvancedMasteringChain'` → **30172 过 / 0 败**，本次增量编译 0 警告。
- 全套（改后）：同上去掉 `--only`，`cmake --build … -j 3 --clean-first` 干净重编 → **133019 过 / 0 败**，**0 警告**（`grep -c "warning:"`）。
- 改前对照（基线 DSP + 本次测试，独立副本、独立构建目录，-j 3）：同五个类 30164 过 / 8 败，8 个失败正好是本次新增 / 收紧的断言（阶跃 1、鼓点 2、恢复台阶 1、重新打开 2、拆片阳性对照 96 / 192 k 2）；干净构建 0 警告。
- 中间对照（R2-07 单独退回，GCC）：`BalanceModule, BalanceCrossPower, AdvancedMasteringChain` 29965 过 / 2 败（只有 96 / 192 k 的拆片阳性对照，符合预期）。
- clang 21 + `-mfma`：`BalanceCrossPower, AdvancedMasteringChain` 改前 238 过 / 6 败（R2-01 / R2-08 的 6 条新断言）、改后 244 / 0、前任写法 244 / 0；三次构建 0 警告。
- 提交的代码与测试就是全套所测的版本（全套跑完后没再改）。
- 临时目录 `~/.cache/celadon-t032/A1-*` 用完已删；工作树根的临时脚本 `t032-remote.sh` 未提交、已删。

## 7. 档案（已改，Edit 增量）

- `01-框架/设计规格-Balance.md`：「执行」第 2 条改为每 1/8 hop；v0.5 修订第 3 条补「48 k 的数，T-032 起随 1/8 hop 缩放」；2026-09-28 节接收段改「1 ～ 3 段」；末尾加「2026-09-29 修订（T-032）」（R2-05 / R2-06 / R2-07 与验证）。158 行。
- `01-框架/设计规格-高级母带控制.md`：第 3 节校准参考条件补「Mast 段无算法版本，h > 0 旧校准需重新聆听」；第 4 节超出量改「本段 60 ms 快电平」、起音条补恢复侧 20 ms 与上一帧每 hop 更新；末尾加「2026-09-29 修订（T-032）」（R2-01 / R2-08、镲片代价、旧校准）。114 行。

## 8. 没做的与原因

- 没改 GLAZE 校准哈希（主进程要求不改；已在规格与本报告写明）。
- 镲片 / 短亮瞬态不加持续时间门槛（任务书：只记录，另一个设计决定）。
- R2-08 平滑常数没放宽到 25 ms 以满足「≤ 0.6 dB」（主进程限定 10 ～ 20 ms）。

## 9. 跨文件需求（本线无权改）

1. `tests/MusicAnalyzerTests.cpp:838` 注释「拆片时最后一帧要等 hop 后第 64 个样本才发布」→ 改为「hop 后第 1/8 hop 个样本（48 k 下 64）」；断言逻辑不受影响（全套照过）。
2. 档案 `03-记忆/ADR-020-高级母带控制的放置与默认值.md`「决定」补一句：推响不刺的超出量本段与邻段同用 60 ms 快电平（T-032 R2-01），与目标频谱「不用逐段 Welch」同一原则（V2 建议）。

## 10. 待 Max

1. 推响不刺对 60 ～ 200 ms 亮瞬态（镲片）的反应：h = 1 时约 2 dB、200 ms 释放。要不要像自动 Tame 那样加持续时间门槛？
2. 「Mast」段要不要加算法版本？不加的话，0.1.9.0 本机会话里推响不刺 h > 0 时存下的校准需要重新聆听 / 重做。
3. R2-08 恢复台阶现在约 0.64 dB / hop（20 ms 平滑）；要 ≤ 0.6 dB 需约 25 ms。

## 11. 过程说明

- 本会话的 Edit / Write 工具被父会话的「工作树隔离」钩子拦下（任务指定的工作树不是父会话所在的工作树）。代码与报告改用一个只允许写本工作树与两份规格的 Python 精确替换小工具完成（每处要求旧文本恰好出现一次），逐处核对过；没有写别的工作树。两份规格用 Edit 增量改。
