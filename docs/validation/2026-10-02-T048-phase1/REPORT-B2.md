# T-048 阶段 1 修订 · 代理 B2 报告：实时段落账与学习脱钩、advanceTo、关实时模式补帧、锚定取整

> 2026-10-03 · 代理 B2（Opus 5.5 max）· 工作树 `.claude/worktrees/wf_2d6085c2-811-2`，分支 `worktree-wf_2d6085c2-811-2`（开工时快进到 `a2daf2b`）· 服务器 109
> 本报告本应写到 `docs/validation/2026-10-02-T048-phase1/REPORT-B2.md`；子代理写报告文件被工具拦下，按 BRIEF 放在返回结果里，请主进程落盘。
> 逐位口径照旧：实时模式关着时一切与 `a2daf2b` 逐位相同；实时模式开着时编排器能读到的一切也与关着时逐位相同（B 立住的两条，本线没退）。

## 0. 结论

| 项 | 结果 |
|---|---|
| 1 三道相对门与学习脱钩 | 完成。实时段落账的帧门、子块有效门、起音组有效门改用它自己的三份长期参考（按进来的音频遗忘，τ = 180 s）。House 那一份照旧。口径写进 `MusicAnalyzer.cpp` 第「七」节 ①～⑤ |
| 2 advanceTo | 完成。processInput、processSpectrum 末尾各报一次。T = 实时段落账**交齐的位置**：end ≤ T 的段都已交出，之后交来的段 end > T。段与 T 都在单调的「实时时间轴」上，表 / 分析器 reset、prepare 之后接着往后数 |
| 3 关实时模式不丢最后一块的帧 | 完成。`setLiveMode (false, &balance)` 先把 Balance 里还没交的帧**只交给实时段落账**（帧流状态用副本），再就地定稿。编排器与链都已直通。渲染工具的「补 0 样本块」可以去掉（cross_file_requests 1） |
| 4 从第 N 小节起播的 1 样本碎段 | 原因是锚定取整（见 4 节），已在 `SegmentLedger.cpp anchor` 修好。House 那一份同受影响（只在块起点离小节线不到半个样本时），列进待拍板 4 |
| 5 测试 | 109 上：`MusicAnalyzerLive,SegmentLedger` 245 passed / 0 failed（含新增 8 个用例）；只改代码、还没加用例时，原有的 `MusicAnalyzerLive,SegmentLedger,ClayTrials` 203 / 0；编译 0 警告 |
| 6 a / b 对照、其余定向测试、性能 | **未做**。09:4x 起本机到 107 / 109 的 VPN 断了（ssh 超时，路由回到家用网关），见 6、7 节与待拍板 8 |

## 1. 三道门的新参考与理由（第 1 条）

### 1.1 原来的问题（REPORT-C 2.4、2.5、7.1）

House 那一份段落账与通用画像的三道相对门都参考学习区间：

- 帧门 = 学习区间的帧均值（`learnTotalPowerSum / learnFrames`）。读档后不学习，`learnFrames = 0`，门就没了。
- 子块有效门 = 学习中用学习区间的 I，不学习时用画像的 I。
- 起音组有效门 = 学习区间的平均功率。

实时段落账原来照抄这三道门。学习在宿主块边界上停，帧又晚一块才交，所以交来的段随块长变、随学不学习变。

另一处同类依赖：实时口径的先试后用块是否有效（`liveValid`），用的也是起音组那道门。

### 1.2 新口径（`MusicAnalyzer.cpp` 第「七」节 ①～⑤）

门的形状与学习区间那一份相同，只换参考：

| 门 | 判据 | 参考（实时段落账自己的） |
|---|---|---|
| 帧门 | 总功率 ≥ −70 dBFS，且 ≥ 参考 − 20 dB | `LiveMean`：绝对门之上的帧总功率的长期均值 |
| 子块有效门 | 400 ms 响度 ≥ −70 LUFS，且 ≥ 参考 − 20 LU | `LiveIntegral`：绝对门之上的 400 ms 门块的 BS.1770 门控积分（−10 LU 相对门；0.1 LU 一格的直方图，按格中心判门，同学习区间的 I） |
| 起音组有效门 | 慢包络 ≥ −70 dBFS，且 ≥ 参考 − 20 dB | `LiveMean`：慢包络在绝对门之上的组的平均功率的长期均值 |

- ① 每份参考只由这一路自己的数据按先后逐项更新（帧、门块、起音组各按自己的时间戳），所以与宿主切块无关，也不读学习状态。
- ② 先按参考判这一项的门，再把它并进参考（因果，同学习区间）。还没有参考时只用绝对门（同学习区间的冷启动）。
- ③ 并进参考的是绝对门之上、窗口不早于实时段落账起点的项，**不看相对门**。学习区间的 I 与起音组本来就这样。帧门原来只并过了相对门的帧，这里不跟：否则比参考轻 20 dB 以上的新素材永远进不了参考，也永远过不了门。
- ④ 按进来的音频遗忘：每并进一项，旧的全部乘 exp(−这一项的时长 ÷ 180 s)（常数 `liveGateMemorySeconds`）。
  - 静音不推进遗忘。
  - 参考代表「这首歌的主体」，不随一两个段落起落：32 小节的 breakdown 只把帧均值拉低约 1.4 dB，离 20 dB 的门很远。时间常数与实时画像参考域的遗忘（180 s）同量级。
  - 代价：换成一首**轻 20 dB 以上**的节目时，要过一阵才算有效音频。按功率平均算，轻 28 dB 时帧门约 1.85 τ ≈ 5.5 分钟、子块门约 4.25 τ ≈ 13 分钟。轻不到 20 dB 的立即有效。
  - 原来按学习区间的门在这种情况下一直不放（直到重新聆听），新口径只是慢。列进待拍板 3。
- ⑤ 参考随实时段落账一起清（打开实时模式、reset、prepare），不在不连续点清（跳播、回绕还是同一首歌）。
- 实时口径的先试后用（`noteTrialGroup` 的第二个参数）与实时段的起音事件也改用实时的组有效门。实时模式关着时这个门恒为「否」，不影响任何别的收方（ClayTrials 里它只进 `liveValid`）。
- House 门控那一份照旧用学习区间的参考，House 画像逐位不变。

### 1.3 频谱证据的分发（为什么动了帧流的结构）

原来 `accumulateEvidence` 只在学习口径的帧门（`music`）之上调用，实时段的证据跟着它走——因为原来的实时帧门蕴含 `music`。新的实时帧门不再蕴含它，所以：

- 帧流的运行状态（1 s 平均、谱通量参考与滑动均值）收成 `FrameRun`，由 `advanceFrameRun` 推进，算式与先后原样搬自 `consumeFrame`。
- 入口核对与游标收成 `FrameClock`（`checkFrameTimeline`）。
- 一帧的证据只算一次（`computeEvidence`：两遍轮廓拟合，算式不变），再按各自的门分给各收方：
  - 学习区间、短期视图、House 段照旧在 `music` 之上、已稳定时才收；
  - 实时段在实时帧门之上、已稳定时收。
- `restartSpectrum` = `FrameRun::restart` + `restartLiveView`（原来是同一个函数里的两半）。
- 逐位不变的依据：
  - 各收方的累计顺序与算式不变。同一帧里各收方之间的先后变了，但它们互不依赖。
  - B 的「实时模式开 / 关时编排器读到的一切逐位相同」（三个脚本 + 链级对照）、块长 7～4096 段序列逐位相同，这些用例照旧通过（5 节）。

## 2. advanceTo 的时间轴（第 2 条）

- **报什么**：T = 实时段落账此刻**交齐的位置**，也就是定稿用的「四路都越过的时刻」（`completeBefore`），再与报过的值取大。
  - 保证：报 T 时，end ≤ T 的段都已交出；之后交来的段 end > T。
  - 所以接收者在 T 之前（含 T）的绝对网格点上做的事，只取决于段序列与 T，与宿主块长、调用次数无关。
  - 就地定稿（不连续点、reset、prepare、关实时模式）之后再报一次此刻：数据只送到这里，开着的段都截到这里交出去了。
- **为什么不直接报 now**：
  - 段要等四路数据（子块、帧、起音、先试后用的块交齐）都越过段尾才定稿，比 now 晚约 0.1～0.3 s，再加一块（帧晚一块才交）。宿主块越大，晚得越多。
  - 报 now 的话，接收者在 now 之前的网格点上看到的段集合随块长变，「结果只取决于段序列与时间」就做不到。
  - 交齐位置 ≤ now，只是晚一点。按 now 理解写的接收者照样能用（单调、只是晚）；反过来不行。
  - 列进待拍板 1，并写进 cross_file_requests 2 给 A2。
- **单调**：
  - 分析器时间轴（IN 表的累计样本数）在表被 reset、分析器 reset / prepare 之后会从 0 重新数。
  - 交给接收者之前统一加一个偏移（`LiveAxis`）：分析器时间轴从 a 重新数时，把偏移加大到「a + 偏移 ≥ 报过的最晚时刻」。
  - 所以实时时间轴在分析器的一生里单调不减，而且首尾相接。
  - 段在交出去时才换算，段落账自己仍在分析器时间轴上。偏移为 0 时原样转交，不拷贝。
  - 关 / 开实时模式、换接收者都不清这条时间轴。
- 没设接收者时只记下来、不调用；实时模式关着时一次都不调。

## 3. 关实时模式时不丢最后一块的帧（第 3 条）

- **问题**：链在下一块开头才把上一块的 Balance 帧交给分析器（`CeladonChain::process` 第一行 `analyzeSpectrum`），所以关的那一刻，最后一块的帧还在 Balance 里。
- **做法**：
  - 新接口 `MusicAnalyzer::setLiveMode (false, const BalanceModule* pendingFrames)`。`pendingFrames` 非空时先调 `deliverPendingFramesToLive`：帧流的核对与运行状态用**副本**（`FrameClock` / `FrameRun`），走同一套函数（`checkFrameTimeline` / `advanceFrameRun` / `takeLiveFrame` / `computeEvidence`），只交给实时段落账。
  - 然后 `liveEndEpoch (now)` 就地定稿，报到此刻。
  - `GlazeController::setLiveSegmentSink (nullptr, pendingFrames)` 转交；`CeladonChain::setLiveSegmentSink` 交自己的 `balance`，工具不用改调用。
- **为什么不整份提前交**（例如关之前直接调一次 `analyzeSpectrum`）：
  - 编排器下一块的 `beginBlock` 会读分析器的候选画像（`desiredSource`）与短期视图（`updateClay`）。
  - 整份提前交，编排器就看到了本该下一块才有的帧；关实时模式之后接着播，就不再与「从没开过实时模式」逐位相同。
  - 只交给实时段落账，别的收方下一块开头照常收这些帧（游标也是副本），一个不碰。
- Balance 在上一次交帧之后重新 prepare 过（hop / 窗长与分析器记的不同）时不交：那时它的环里只有 prepare 之后的空状态。
- **用例 5d**（宿主块 4096）：
  - 链 `setLiveSegmentSink (nullptr)` 交来的段，与「先补一个 0 样本块再关」（渲染工具原来的变通）逐字段相同；
  - 阳性对照：直接调编排器的 `setLiveSegmentSink (nullptr)`（不给 Balance）时，前面的段相同，最后一段只有 77 帧（修后 84 帧）。

## 4. 从第 N 小节起播的 1 样本碎段（第 4 条）

- **原因：锚定取整**。
  - 渲染工具从第 N 小节起播的起点是 `round((N − 1) × 每小节样本数)`。48 kHz、124 BPM 一小节 92903.2258 样本（小数部分 7/31）。第 33 小节线在 2972903.2258，起点 2972903 在它前面 0.23 个样本。
  - `anchor` 先按 `floor(PPQ ÷ 4 + 1e-9)` 取到第 32 小节，偏移 = 92903.0 样本；再钳到「每小节样本 − 1」= 92902.2258，取整得 92902。原点被拉晚了 1 个样本。
  - 结果：第 33 小节线落到起点之后 1 个样本，第一段成了第 32 小节的 [0, 1)，第 33 小节从样本 1 起。
  - 碎段还按「起点归属」收走了第 33 小节的第一个子块、第一帧和第一个起音组。
  - 凡是块起点离下一条小节线不到半个样本（照就近取整，这条小节线本该落在块起点上），都会这样。按工具的起点取法，约一半的小节号会碰上（小数部分 < 0.5 的那些）。
- **修法**（`SegmentLedger.cpp anchor`，12 行）：
  - 精确偏移大于「每小节样本 − 0.5」时，从下一条小节线算起：小节起点 + 一小节、偏移 − 一小节，再照原样钳、取整。
  - 离小节线半个样本及以上的块起点照旧：那几个样本就近取整确实属于上一小节，第一段是上一小节的 1 个或几个样本。这是对的口径，用例里作为对照钉住。
- **House 那一份**：
  - 同一个类，同样受影响，但只在「锚定时块起点离下一条小节线不到半个样本」时：从第 N 小节起播的那些小节号，或播放中重新锚定（跳播、换曲风、变速）恰好落在这半个样本里。
  - 影响：不再开那个碎段；第一小节多收回第一个子块、第一帧、第一个起音组；之后的小节线整体早 1 个样本。
  - 从头播（PPQ 0）、锚点不在这半个样本里时，逐位不变。
  - 本来要用对照渲染（合成 House 从第 33 小节起播，基线 vs HEAD）量这个影响，服务器断了没跑成（6 节）。
  - 列进待拍板 4。要撤回，就把 `Source/dsp/SegmentLedger.cpp` 退回 `a2daf2b`（本线只改了这一处），并删掉两条 5f 用例。
- **用例**：`SegmentLedger` 新用例（账本一级，五种起点 × 块长 512 / 4096 / 随机）；`MusicAnalyzerLive` 5f（分析器一级，从第 33 / 3 / 4 小节起播，块长 512 / 4096）。

## 5. 测试

命令（109）：`CELADON_TEST_HOST=maxsong@192.168.100.109 scripts/test-linux.sh bash Celadon/b2-check.sh <类名,…>`。临时脚本做的事：

- 没配置过就先 `cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release`；
- `cmake --build "$BUILD" --target celadon_tests`，统计 `warning:`；
- `"$BUILD/celadon_tests" --only …`。

| 批次 | 结果 |
|---|---|
| 只改代码、还没加用例：`MusicAnalyzerLive,SegmentLedger,ClayTrials` | 203 passed, 0 failed（= B 报告的 48 + 155），编译 0 警告 |
| 加了用例：`MusicAnalyzerLive,SegmentLedger` | 245 passed, 0 failed，编译 0 警告 |
| 任务书 6c 的其余定向类（`MusicAnalyzer,MusicAnalyzerClay,MusicAnalyzerHouse,ClayGlaze,GlazeController,GlazeControllerHouse`）与 `LiveProfile` | **未跑**（服务器断） |
| 新用例在改前的代码上是否真的失败（基线源码 + 新用例） | **未跑**（服务器断；脚本已备好，见 6 节） |

新增用例：`tests/MusicAnalyzerTests.cpp` 的 `MusicAnalyzerLive` 类尾追加 7 个，`tests/SegmentLedgerTests.cpp` 的 `SegmentLedger` 类尾追加 1 个；原有用例与工具一行没改。

- **5a 分析器一级**：
  - 学习从 0 开始；各块长在 12.345 s 之后的第一个**真实块边界**上停（不是各块长的公倍数）。
  - 块长 7 / 32 / 480 / 512 / 4096 交来的段，除学习期专用三项外逐字段相同。通用档与 Deep House 各跑一遍。
  - 素材（`makeGatedTrack`）有一段压低 26 dB 的段落和 8 s 淡出。阳性对照：压低那一段被实时参考的相对门挡住；各块长停学习的样本确实不同。
- **5a 链一级**：`CeladonChain` 开实时模式，编排器自己在提交（通用 GLAZE 100）/ 锁定（Deep House GLAZE 70）时停学习；宿主块 7 / 32 / 480 / 512 / 4096 交来的段，除学习期专用三项外逐字段相同。
- **5b 分析器一级**：同一段音频，「从头学习到底」与「从不学习、第一块之前来一次宿主 reset（读档时编排器就这样做）」两种状态：
  - 交来的段除学习期专用三项外逐字段相同，连续时间段号也相同；
  - 不学习时 trialBlocks 处处为 0，实时相对门照样挡住压低那一段；
  - 阳性对照：House 那一份（参考学习区间）在两种状态下确实不同。
- **5b 链一级**（REPORT-C 2.5 的场景）：第一遍聆听、提交、存档；新链读档，第二遍不学习。两遍交来的段除学习期专用三项外逐字段相同，连续时间段号也相同。
- **5c advanceTo**：随机块长跑 36 s，中间有跳播、两次表与 Balance reset（时间轴回退）、分析器 reset、同规格 prepare，最后关实时模式。结果：
  - 每次 processInput / processSpectrum 都报（849 块报了 1708 次），单调不减；
  - 报 T 时，此前交来的段 end ≤ T，之后交来的段 end > T；
  - 段在实时时间轴上首尾相接，时间轴回退处也接上（最后报到 1728777，正好是输入总长）；
  - 最后一次报数 = 最后一段的 end。
- **5d 关实时模式补帧**：见 3 节。
- **5e**：原有的「实时模式开 / 关输出逐位相同」（分析器三个脚本、链级 69 s）照旧通过（见上表）。
- **5f**：见 4 节。

「除学习期专用三项外」：段里的 `trialBlocks / trialTop / blendIntended / blendOutput` 按 `SegmentLedger.h` 的约定只记学习期间（学习区间口径），本来就随学习的起止变；实时画像只读 `live*`。见待拍板 2。

## 6. 109 上的对照（第 6 条）——未做

09:4x 起本机到 107 / 109 都不通：`nc -z` 超时，ssh 报 `Operation timed out`，到 192.168.100.x 的路由回到了家用网关 192.168.86.1，VPN 断了。按任务书「不通就停下、不要等」，下面几项没跑：

- **6a**：`a2daf2b` 与 HEAD 的 `celadon_render` 不带新选项各渲染一遍（合成 House / Tech、Jamendo tech 0268597 与 melodic 1062232，GLAZE 70 / 100，通用与 Deep），比输出是否逐字节相同；另加合成两首从第 33 小节起播，量第 4 条对 House 的影响。
- **6b**：`calibrate_house.py render-live` 跑 45 首 × `b2-default` / `b2-general` × 块长 512 / 4096，逐首比较段读数（应 45 / 45 两组都相同）；另与 C 的出厂结果（`live/default`、`live/general`）比，报段读数变了的首数。
- 其余定向类、`LiveProfile`、新用例在改前代码上的失败验证、性能。

预期（未验证，不作结论）：

- 6a 不带新选项时应逐字节相同：三道门、advanceTo、补帧只在实时模式开着时起作用；第 4 条只在锚点离小节线不到半个样本时起作用，从头播不受影响。
- 6b 原来的块长差别来源（帧门参考学习区间）已经去掉。剩下的风险是开始学习时候选从零重启（B 待拍板 3），只在学习中途重开时才会让 live* 随块长变。

复跑做法（工作树根目录的临时脚本，没提交）：

- `b2-base.sh build`：把 `git archive a2daf2b`（`b2-baseline.tar`）解到 `~/.cache/celadon-t048-cal/b2/base-src`，单独编基线的 `celadon_render` / `celadon_tests`；再在 `$BUILD` 里编 HEAD 的 `celadon_render`。
- `b2-base.sh swap`：把 `b2-oldtests/` 里的新用例换进基线源码，看它们是否失败。
- `b2-render.sh`：基线与 HEAD 各渲染一遍。
- `b2-compare.py tree|live`：比较两棵输出树 / 两个标签的追踪。

## 7. 性能——未测

分析器每块均值与最坏块（`MusicAnalyzerLive` 的性能用例，与 REPORT-B 第 7 节对照）要在 109 上与基线交替测量，服务器断了没测。

工作量估计（不是实测）：

- 每帧：多一次实时帧门判断与一次均值更新；证据仍只算一次。
- 每个子块（100 ms）：多一次 `LiveIntegral` 更新（只扫有数的格，遗忘 + 求和，约几百次乘加）。
- 每组（0.33 ms）：多一次均值更新。
- 每块：多两次虚调用（advanceTo）。
- 内存：Impl 多约 18 KB（直方图 2 × 800 个 double、换算用的一份 Segment、帧流副本用的结构）。

## 8. cross_file_requests

1. `tools/render/RenderMain.cpp`、`README.md`（A2）：
   - `finish` 里「补一个 0 样本块再 `setLiveSegmentSink (nullptr)`」的变通可以去掉。链的 `setLiveSegmentSink (nullptr)` 现在会先把最后一块的帧只交给实时段落账，再定稿（用例 5d：两者逐字段相同）。
   - README「T-048 做法」第 3 条，与自检里「不补块时 4096 与 512 的追踪不同」那一项，要跟着改。
2. `Source/dsp/LiveProfile.cpp`（A2）：
   - `advanceTo (T)` 按本线实现是「交齐位置」：end ≤ T 的段都已交出，之后交来的段 end > T；单调不减。
   - 与段的 start / end 在同一条实时时间轴上；分析器 reset / prepare / 表 reset 之后接着往后数，不回到 0，首尾相接。
   - 若 A2 是按「processInput 末尾的 now」设计的网格，需要对齐（待拍板 1）。
   - 实时段落账的段里 `trial*` 三项仍随学习变，只读 `live*`。
3. `Source/dsp/SegmentLedger.h`（主进程；头文件本线只许为第 4 条加私有成员，没动）：
   - `Segment` 的 T-048 注释可补一句「实时段落账的三道相对门参考它自己的长期参考（MusicAnalyzer.cpp 第「七」节），不参考学习区间」；
   - `SegmentSink::advanceTo` 的注释可写明「T = 交齐位置」。
4. `tools/render/README.md`（A2）：「块长」一段写的「Jamendo 45 首 4/4 型 43 / 45、响度型 30 / 45 首逐字节相同……来源在分析器」在本线合并后会过时，待 6b 跑完确认后再改。

## 9. pending_user_decisions

1. advanceTo 报「交齐位置」而不是「processInput 末尾的 now」（理由见 2 节）。接口注释写的是「时间轴走到了哪里」，请确认这个语义，并让 A2 按它对齐。
2. 实时段落账的 `trial*` 三项照 `SegmentLedger.h` 只记学习期间，所以「学习中 vs 读档后」「不同块长」的逐字段相同只对其余字段成立。要不要让实时段落账干脆不记 `trial*`？（要改头文件注释，不归本线）
3. 三道门参考的遗忘时间常数 180 s（`liveGateMemorySeconds`）：换成轻 20 dB 以上的节目时，帧门约 1.85 τ、子块门约 4.25 τ 之后才算有效音频（轻 28 dB 时约 5.5 / 13 分钟）。要不要更短，或加「持续远低于参考时加快遗忘」？
4. 第 4 条改了 `SegmentLedger::anchor`，House 那一份同受影响（只在锚点离下一条小节线不到半个样本时；从第 N 小节起播约一半的小节号会碰上）。保留，还是只给实时那一份（要动公开接口）？另外，离小节线半个样本到一个样本之间的起点照旧开 1 个样本的段（就近取整的口径），要不要也并进下一小节（「落在第一个样本里的小节线就从块起点算」）？
5. 「上一次断开之后还没收过数据」的断开（读档时编排器在第一块之前做的宿主 reset），不再给实时段落账加连续时间段号——这样读档后的第二遍才能与第一遍逐字段相同。B 原来的口径是每次断开都加。
6. 开始学习（含重新聆听、House 中途开一轮聆听）时候选照旧从零重启（B 待拍板 3 仍在）：重启落在宿主块边界上，实时 `live*` 在重启处丢的块随块长变。本线用例只在学习从 0 开始时验了块长无关；要消掉它得多跑一套候选。
7. 关实时模式的补帧只交给实时段落账（帧流状态用副本），不整份提前交（理由见 3 节）。请确认这个做法。
8. 6a / 6b 对照、其余定向测试与 `LiveProfile`、性能、新用例在改前代码上的失败验证都没做（服务器断）。由主进程恢复网络后复跑（6 节的脚本），还是重开本线接着做？

## 10. 改动的文件与提交

- 文件：
  - `Source/dsp/MusicAnalyzer.{h,cpp}`；
  - `Source/dsp/SegmentLedger.cpp`（只改 `anchor`）；
  - `Source/dsp/GlazeController.{h,cpp}`、`Source/dsp/CeladonChain.h`（只为第 3 条）；
  - `tests/MusicAnalyzerTests.cpp`（`MusicAnalyzerLive` 类尾追加）、`tests/SegmentLedgerTests.cpp`（`SegmentLedger` 类尾追加）。
  - 没动 LiveProfile、tools/render、SegmentLedger.h。
- 提交：`d2462e2`（代码）、`b3c60bf`（用例）、`449e72a`（收尾）。
- 清理：
  - 109 上没建任何 b2 的东西（基线脚本第一条 ssh 就超时了）。C 的 `live/default`、`live/general`、WAV、manifest 都没碰。
  - `scripts/test-linux.sh` 自己的每工作树缓存（`~/.cache/celadon-tests`、`celadon-build` 下本工作树那两个目录）照常保留。
  - 本机工作树根目录的临时文件 `b2-check.sh`、`b2-base.sh`、`b2-render.sh`、`b2-compare.py`、`b2-baseline.tar`、`b2-oldtests/` 没提交，留着给复跑用；对照做完后要删。
