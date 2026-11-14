# T-099 存读档与音频线程互斥 · 验证记录（2026-11-13）

> 状态：第一版（锁里整份编码）在分支 `t114/l2-t099`（基点 `27e33d0`），已并入集成分支；T-114 第二波 L4 拆锁（分支
> `t114/l4-t099-t098`，基点 `f4741f5`）：锁里只拷快照 / 换入暂存，编码与解档在锁外。**写这两版的代理都没有编译、没有跑测试**；
> 第 5 节的命令由主会话在 108（testbox）上跑，第 6 节的数字跑完回填。决定落在 vault `03-记忆/ADR-040-运行中读档参数照换画像保留.md`。
> 来源：体检报告 `docs/validation/2026-11-12-T105-plugin-review/2-features.md` 的 2-01、2-03、2-10；Max 2026-11-12 拍板第 4、11 项。

## 1. 改了什么

| 问题 | 改法 | 位置 |
|---|---|---|
| `getStateInformation` / `setStateInformation` 直接调编排器的存读档，读写音频线程每块在改的量（体检 C-T099） | GLAZE 那一段碰运行状态的那一步在 `getCallbackLock()` 里做；JUCE 9.0.2 的 VST3 / AU / AAX / Standalone 处理每一块时都持这把锁，互斥成立 | `Source/PluginProcessor.cpp` getStateInformation、setStateInformation |
| 第一版锁里整份编码：窗口满时播放中存档让音频线程等 138.6 ms（第 6 节） | 存读档拆两步（第二波 L4）：锁里只拷快照 / 换入暂存，拼 XML、zlib、base64、解压、解析、校验、重算画像都在锁外 | `GlazeController`（StateSnapshot / StagedRestore）、`LiveProfile`（StateSnapshot / StagedState）、`GlazeGuard` / `OutputLevelTracker`（stageStateXml / adoptRestoredState）、`CeladonChain` 转发 |
| 运行中的实例读档（换宿主预设、撤销）会把实时画像换成存档里的 | 运行中 → 只换参数与参考频谱，GLAZE 存档整份不读（拍板第 4 项） | 同上，`keepsLiveStateOnLoad` |
| Lookahead 可自动化 = 播放中改延迟（体检 2-10） | `withAutomatable (false)`（拍板第 11 项） | `Source/Parameters.cpp` |
| 两处注释描述已删的交接机制（体检时 `:900`、`:925-926`） | 改成现在的线程约定与读档顺序 | `Source/PluginProcessor.cpp`、`.h` 文件头 |
| 处理器层没有自动测试（体检 2-03） | 新目标 `celadon_processor_tests`（含 TSan 版）进 CI | `tests/processor/`、`CMakeLists.txt`、`.github/workflows/ci.yml` |
| 108 / CI runner 没有界面开发头，编不了插件源码 | `scripts/fetch-gui-dev.sh`：官方 -dev 包解到用户目录 | `scripts/` |

## 2. 锁的范围

第二波 L4 起（拆锁之后）。接口见 `GlazeController.h`「存读档拆两步」、`LiveProfile.h` 文件头第三节最后一条。

| 步骤 | 在锁里？ | 为什么 |
|---|---|---|
| 存档：`parameters.copyState()`、写格式号 | 外 | APVTS 自己加锁复制，参数值是原子量 |
| 存档：建快照 `GlazeController::StateSnapshot`（分配并写满初值：回看窗口 544 段约 1.7 MB、预算环与出口电平跟踪器各一份） | 外 | 分配与第一次碰页面都在拿锁之前；每次存档一份，不做成员（有的宿主在别的线程存档，两次存档可能同时） |
| 存档：`chain.captureGlazeState`：实时画像的计数、时钟与窗口里的段，编排器的标量，预算环 / 出口电平跟踪器整份值拷贝，Clay 守护状态 | **里** | 读的是音频线程每块在改的量。只做定长拷贝：不分配、不调宿主、不拿别的锁 |
| 存档：`GlazeController::encodeState`（拼 XML、段的 base64、LIVE 外层 zlib 9 + base64） | 外 | 只读快照、不碰实例 |
| 存档：参考频谱、挂到根元素、`copyXmlToBinary` | 外 | 参考频谱自带票号 + 逐字原子发布（`ReferenceSpectrum.h`） |
| 读档：`getXmlFromBinary`（外层解压、解析）、摘节点、`replaceState` | 外 | 同上；参数先换 |
| 读档：第一次判断 `keepsLiveStateOnLoad`；运行中就 `keepGlazeStateOnLoad`（清旧工程提示）、收尾 | **里** | 判断读的是音频线程的量。运行中（换宿主预设、撤销，最常见的读档）不解档 |
| 读档：`chain.stageGlazeRestore`：解开 LIVE（zlib + base64 + XML）、读各节点、校验；实时画像在另一份工作区（`LiveProfile::StagedState`）里读回并重算；预算环 / 出口电平跟踪器读进暂存对象 | 外 | 只读消息线程才改的配置（是否已 prepare、采样率、实时画像常数与冻结参考），不碰运行状态 |
| 读档：第二次判断 `keepsLiveStateOnLoad`；运行中 `keepGlazeStateOnLoad`，否则 `chain.commitGlazeRestore` | **里** | 第一次判断到这里之间实例可能已经开始放音乐了。commit 只做定长拷贝与指针交换（实时画像两份工作区换指针、提交记录留原来的），坏档 / 旧格式 / 空存档时冷启动——都不分配、不释放 |
| 读档：释放暂存（换下来的旧画像工作区、旧的「prepare 之前读进的原件」） | 外 | 暂存出了作用域才析构 |
| 读档：参考频谱 | 外 | 自带同步 |

`prepareToPlay` 与存读档不加锁互斥：VST3 / AU 约定这几个调用都在消息线程，不会同时进行。stage 读的配置也只在这几个调用里改。

重算放在哪：实时画像的重算只依赖读进来的段与配置，在暂存的工作区里锁外算完（与就地读档同一段代码、同一份配置，读回的画像逐位相同）；
没有要读运行状态的重算——目标方案本来就在读档后第一块 GLAZE > 0 的 endBlock 由音频线程重算，读档只把它标成「没有」。

## 3. 运行中读档的判定

「运行中」= 处理过至少一块非空音频（`processedAudio`，音频线程置位、只置不清）**且**实时画像已发布的那一份里有过有效音频（`LiveDiagnostics::activeSeconds > 0`）。在锁里判断，
读档时判断两次（第 2 节）：第一次为真就不解档；为假则锁外解档，换入之前再判断一次。

| 情形 | 判定 | GLAZE 存档 | 用例 |
|---|---|---|---|
| 宿主打开工程：新实例先读档、再 prepare | 没处理过音频 | 读 | ProcessorState「round trip」 |
| 第一块音频之前连读两份（有的宿主打开时先给默认状态） | 没处理过音频 | 都读，留后一份 | 「two loads before audio」 |
| prepare 之后、第一块之前读档 | 没处理过音频 | 读 | 「load after prepare」 |
| 读工程之前宿主就在送静音块 | 画像里没有有效音频 | 读 | 「silence only」、ProcessorConcurrency 第 2 条 |
| 播放中换宿主预设 / 撤销 / 读别的工程 | 运行中 | **不读**，参数照换 | ProcessorState「running instance」、ProcessorConcurrency 第 3 条 |
| 运行中的实例读一份带画像的工程（例如 Ableton 把另一首歌的预设热换进来） | 运行中 | **不读**（拍板：画像保留） | 同上（工程 A 带画像） |

「不读」指整个 GLAZE_STATE：实时画像、已施加增量、G_trim、各守护与出口电平、Clay 守护状态都接着用。目标方案按新参数重算（编排器每块 `beginBlock` 发现 GLAZE / 风格或影响方案的手动值变了就重算）。

## 4. 测试（`tests/processor/`）

| 用例类（`--only` 名） | 内容 |
|---|---|
| `ProcessorState` | Lookahead 不可自动化、其余可自动化；存过的 Lookahead 值照读；存读档往返（参数逐个相同，GLAZE 节点读一遍后是不动点，段数不丢）；运行中读档参数照换、GLAZE 节点逐字不变；三种「第一次读档」照常读；0.1.8.10 工程（带默认曲风 / 交付节点）、≤ 0.1.8.5 工程（无格式号、缺高级母带与 Clay 参数、GLAZE 格式 1）、v0.4 之前工程（无 GLAZE 参数） |
| `ProcessorConcurrency` | 音频线程照包装层持回调锁连续 `processBlock`（块长 256），主线程同时：① 存档 24 次，每份的 LIVE_PROFILE 都读得回、画像非空；② 还没听到音乐时来回读两份空画像的预设 24 次、再读带画像的工程，每次参数都换过来、最后画像读回；③ 运行中来回读预设 D 与工程 A 24 次，参数每次都换、画像段数只增不减 |
| `ProcessorStateLockBench`（`--bench --only ProcessorStateLockBench`） | 放 1084 s 合成音乐把回看窗口填满（512 段），量存档大小、单线程存档耗时、按真实时间节拍（48 kHz / 64）播放时存档 / 运行中读档 / 第一次读档让音频线程等锁的最长时间与掉块数 |
| `ProcessorStateDigest`（`--bench --only ProcessorStateDigest`，第二波 L4） | 拆锁前后存档逐字节相同的证据：empty / partial（60 s）/ full（窗口满 512 段）三种画像，加「新实例读这份档再存」两种与「读 partial 再放 10 s 再存」，各打印 `state-digest <场景>: <十六进制>`（整份存档）与 `state-digest <场景>/glaze: …`（GLAZE 节点），同一场景跑两遍摘要必须相同。只用拆锁之前就有的接口，同一份文件放进基点 `f4741f5` 的树里再跑，两边逐行比 |

链层另有 `GlazeChain` 的「state split」用例（`tests/GlazeTests.cpp` testStateSplitMatchesWholeCalls，第二波 L4）：快照编码 = `createGlazeStateXml`；
stage 不碰链、stage + commit = `restoreGlazeStateXml`；坏档、旧格式的暂存 commit 时与整步读档相同（旧格式 stage 时提示不亮、commit 才亮）。

旧工程夹具不另存：按 `addMissingParameterNodes` 与 `LegacyState.h` 认的差别，从当前版本存出来的档改出来。GLAZE 格式 1 的真实夹具在 `tests/LegacyFixtureTests.cpp`（链层，不复制）。

TSan「修前报」靠的是第 ② ③ 条与第 ① 条：去掉锁后（`remove-state-lock.patch`，拆锁后是三处：存档拷快照、读档第一次判断、换入之前的判断与换入），
① 拷快照与音频线程同时读写 LiveProfile / 编排器，② 的换入与音频线程同时写，③ 的判断读与音频线程同时写——都应该报。

## 5. 怎么跑（108 testbox）

打包照 T-106 / T-107（memory `celadon-tests-on-testbox-108`）：

```bash
SHA=<t114/l2-t099 或集成分支的提交>
PKG=<一个新目录>
mkdir -p "$PKG/Celadon" "$PKG/JUCE"
git -C /home/maxsong/All-Project/Celadon archive "$SHA" | tar -x -C "$PKG/Celadon"
cp -a /home/maxsong/All-Project/JUCE/LICENSE.md /home/maxsong/All-Project/JUCE/modules "$PKG/JUCE/"
printf 'build/\nCeladon-before/\n' > "$PKG/.gitignore"
git -C "$PKG" init -q && git -C "$PKG" add -A && git -C "$PKG" commit -qm "T-099 package"
testbox submit --path "$PKG" --arch x86_64 --tier full --memory 16G --cpu 24 \
  --cmd 'JOBS=24 Celadon/docs/validation/2026-11-13-T099-state-lock/run-processor-tests.sh release'
```

模式（同一个入口脚本）：`release`、`tsan`、`tsan-before`、`bench`、`digest`。**先单独交 `release`**：它第一次在 `/cache` 装 cmake / ninja、解界面开发头（`/cache/celadon-guidev`），并行首装会互相覆盖；装好之后其余三项可以各用一个打包目录并行交。`bench` 要独占机器。

| 模式 | 期望 |
|---|---|
| `release` | `celadon_ui_snapshot` 编得过；`==== celadon_processor_tests: N passed, 0 failed ====` |
| `tsan` | 同上 0 failed，且「TSan 报告份数：0」 |
| `tsan-before` | 「修前 TSan 报告份数：K（期望 > 0）」，K > 0，脚本退出 0；报告的栈里应有 `GlazeController` / `LiveProfile` / `CeladonAudioProcessor::getStateInformation`/`setStateInformation` |
| `bench` | 打印 `T099-BENCH …` 几行（见第 6 节）；窗口段数检查 = 512。拆锁之后：播放中存档、第一次读档的等锁最长 < 1.333 ms、掉块 0 |
| `digest` | 打印 `state-digest …` 12 行，0 failed；与基点 `f4741f5` 的树里跑同一份 `ProcessorStateDigestTests.cpp`（CMakeLists 的 `celadon_processor_tests` 源清单加这一行，脚本用本分支的）打印的逐行相同 |

全量 `celadon_tests` 照 `run-ci.sh base` / `x86-64-v3` 跑，不新增失败：第一版与基点 `27e33d0` 比（那一版没改 DSP，`celadon_tests` 的源码清单也没动）；
第二波 L4 改了 `Source/dsp` 的存读档（声音处理不动），与基点 `f4741f5` 比，另多一个 `GlazeChain` 的「state split」用例。

TSan 有额外报告时：先看栈里有没有 `celadon::` 或 `CeladonAudioProcessor`——有就是真问题，别压；只在 JUCE 内部或系统库的，确认与存读档无关后加进 `tests/processor/tsan.supp` 并在这里记一笔。

## 6. 持锁时间

「锁里编码」一栏 = 第一版（编排器在锁里压缩 / 解压），2026-11-13 22:5x 在 108 独占 64 核实测：集成分支 `6be145f`，
`run-processor-tests.sh bench`，testbox `20261113-225225-4f6b`（块长 64 样本 = 1.333 ms，48 kHz）。「拆锁之后」一栏 = 第二波 L4（方案 1、2），
主会话在 108 独占跑同一个模式回填。

| 量 | 锁里编码（`6be145f`） | 拆锁之后 |
|---|---|---|
| 最坏画像（512 段）的存档大小：整份 / GLAZE 节点 / 解开后的 LIVE | 728175 B / 726594 B / 1737099 B（窗口 512 段，pending 0） | 待 108 回填（应与左栏逐字节相同：存档格式没变，见 `digest` 模式） |
| 单线程 `getStateInformation` 一次 | 中位 138.2 ms（137.7 ～ 140.3） | 待 108 回填 |
| 播放中存档：音频线程等锁最长 / 单块最长 / 掉块数（对照：空跑） | **138.6 ms / 138.7 ms / 10 次存档掉 10 块**；空跑 0.000 ms / 0.128 ms / 0 | 待 108 回填 |
| 运行中的实例读档（只判断）：等锁最长 | 0.001 ms（`setStateInformation` 中位 0.088 ms），掉块 0 | 待 108 回填 |
| 第一次读档（落位）：等锁最长 | **35.2 ms**（`setStateInformation` 中位 40.1 ms），5 次读档掉 5 块 | 待 108 回填 |

结论：锁里编码时，窗口满了以后播放中每存一次档，宿主的音频线程整整停约 137 ms（远超任何常用块长，整个宿主一起断，不只这个插件）。
计划第 2 节对 T-099 的要求本来就是「解析、压缩在锁外」，所以下面方案 1、2 在 T-114 第二波做了（L4），重跑本节填「拆锁之后」一栏。

拆锁之后的预计量级（按代码推，待 108 实测）：
- 播放中存档：锁里是一次约 1.7 MB 的定长拷贝（544 段 × 约 3.2 KB 的段条目，加预算环约 28 KB、出口电平跟踪器约 10 KB），目标页面已在拿锁前
  写过，**约 0.1 ～ 0.3 ms**；单线程 `getStateInformation` 总耗时与锁里编码时差不多（编码一样多，只是挪到锁外），约 140 ms。
- 第一次读档：锁里是提交记录约 10 KB 的拷贝、两个指针交换、预算环与出口电平的字段拷贝，**几十微秒**；`setStateInformation` 总耗时约 40 ms
  加一份 2.3 MB 工作区的分配（都在锁外）。
- 运行中读档：两次判断各几微秒（第一次为真就收尾，与改前同量级）。
- 判据：等锁最长 < 1.333 ms、掉块 0（块长 64；对照空跑的「单块最长」0.128 ms）。

判据：一块的时长 48 kHz / 512 = 10.7 ms，/ 64 = 1.33 ms，96 kHz / 32 = 0.33 ms，192 kHz / 32 = 0.17 ms。持锁超过当前块长，那一块就掉。窗口要播十几分钟才满，前几分钟的存档小得多；第一次读档通常在宿主还没开始放音时（打开工程），掉块不出声。真正会听到的是「长时间播放中、宿主存档」——自动保存，或 REAPER 每改一次参数取一次状态做撤销点。

第一版留下的三个方案与第二波的取舍：

1. 存档拆「快照」与「编码」——**做了**（第 2 节）。
2. 读档拆「解析」与「落位」——**做了**，但重算没交给音频线程：实时画像的重算只依赖读进来的段与配置，在暂存的工作区里锁外算完，锁里换指针。
   交给音频线程分片重算（`advanceTo`）会让读档之后、重算完之前那几块读不到画像（GLAZE 用冷画像跑几块），还要改 LiveProfile 的发布时序；
   锁外算完再换没有这个窗口，读回的画像与就地读档逐位相同。
3. 按代数缓存压缩好的 LIVE——**不做**：拆锁之后存档在锁里只剩一次拷贝（预计 0.1 ～ 0.3 ms），缓存省的是锁外的编码时间，不影响音频线程；
   要加「哪些量变了就作废」的代数跟踪（画像、编排器标量、预算环、出口电平每块都在变，播放中几乎每次都失效），复杂度换不来好处。

## 7. Lookahead 不可自动化：对旧工程的影响

- 插件这边：JUCE 给 VST3 的 `ParameterInfo.flags` 去掉 `kCanAutomate`（`juce_audio_plugin_client_VST3.cpp:918`），给 AU 标 `kAudioUnitParameterFlag_NonRealTime`（`juce_audio_plugin_client_AU_1.mm:1071-1072`）。参数 ID、范围、默认值、存档格式都不变：工程里存的 Lookahead 值照读（用例「a stored Lookahead value still loads」）。
- 旧工程里**已有的 Lookahead 自动化曲线**：去留由宿主决定，各家不一样。多数宿主把它从自动化列表里拿掉；已有的曲线有的不再回放（参数停在工程里存的那个值），有的照样回放（JUCE 包装层不拒收宿主送来的参数变化），回放时处理器层的 LookaheadHold 照旧挡着（停稳 0.3 s 才换、换一次报一次延迟）。没有在真宿主上验过（Cubase、REAPER、Logic、Live、FL），发版前抽一两个看。
- 版本说明一句（中 / 英）：「Lookahead 不再进宿主自动化：它决定插件延迟，播放中改会让声音挪动、断一下。旧工程里已有的 Lookahead 自动化，部分宿主不再回放，参数停在工程里存的值。」/ "Lookahead is no longer host-automatable: it sets the plug-in latency, so changing it during playback shifts and interrupts the audio. Existing Lookahead automation in old projects may no longer play back in some hosts; the parameter stays at the value saved with the project."

## 8. 已知边界

- 运行中的实例读档跳过 GLAZE 时，清掉上一次读档点亮的旧工程提示（`Status::legacySessionDiscarded`；界面文字在 `Source/ui`）：
  那条提示说的是那次读档冷启动了，这次没有冷启动。入口 `GlazeController::keepStateOnLoad`（T-114 集成时补，主会话定）。
  运行中读入格式 1 的旧预设也不提示（画像保留了，没有冷启动）。用例：`ProcessorState` 的「a running load clears the legacy notice
  an earlier load raised」「a running load of a format-1 project raises no notice」；另一侧（刚建的实例读格式 1 → 提示）是
  「<= 0.1.8.5 project …」。
- 互斥靠「宿主经 JUCE 包装层、处理每一块持回调锁」。不经包装层直接调 `processBlock` 的宿主（自写的测试台、离线工具）要自己持锁——测试台架照做了。
- 锁里只调编排器的 capture / commit / keep 与 `keepsLiveStateOnLoad`，不回调宿主、不拿别的锁（APVTS 的锁在锁外用完），不分配、不释放，不会和音频线程互相等。
- 锁外的 stage 只读消息线程才改的配置。宿主若违反约定、在别的线程与 `prepareToPlay` 同时读档，stage 读到的采样率可能是旧的——
  与改前一样不在保护范围内（第 2 节最后一段）。
- 读档的两次判断之间（锁外解档期间）实例开始放音乐：第二次判断为真，暂存丢掉、参数照换、画像保留——同「运行中读档」。
  这条没有确定性的用例（要在 stage 与 commit 之间插一块音频），由 ProcessorConcurrency 第 ② 条的交错覆盖到一部分。
- 还没 prepare 时读坏档（LIVE_PROFILE 读得回、别的节点坏）：第一版先把 LIVE_PROFILE 读进实时画像再冷启动编排器（还没 prepare 时冷启动
  不清画像），拆锁之后画像停在读档之前的内容。prepare 时链会拿原件再读一次（照样冷启动），之后两版完全相同；只差在 prepare 之前
  `createStateXml` 读到的画像，GlazeController.h 已写明那时的画像不保证。
- 持锁时间见第 6 节；「拆锁之后」一栏回填之前，「播放中存档还掉不掉块」只有估计。

## 9. 我替你定了什么

- 「运行中」的判据 = 处理过音频 **且** 画像里有有效音频（第 3 节）；只看其一都有误判（静音预热的宿主、打开工程时连读两份）。
- 「画像保留」保留的是整个 GLAZE 运行状态（不只画像本身）：增量、G_trim、守护与出口电平都是从同一段音乐里来的，拆开换会互相对不上。
- 参考频谱（用户点「学习」得来的）算设置，运行中读档照换。
- 外层 XML 的解压解析、参数替换、参考频谱放在锁外；第一版编排器内部的压缩 / 解压在锁里（第一波不动 `GlazeController`），
  第二波 L4 拆开：锁里只拷快照 / 换入暂存（第 2 节）。
- 拆锁的做法（第二波 L4）：快照是值类型，按最大窗口在拿锁前分配；读档在另一份完整的画像工作区里读回、重算，锁里换两份工作区的指针；
  预算环、出口电平跟踪器各加一对 stage / adopt（存档里有的字段换进来，其余留原对象的）；createStateXml / restoreStateXml 改成两步连着调，
  所以「两步拆开」与「连着做」不可能存出不同的字节。方案 3（按代数缓存）不做，理由见第 6 节。
- 读档判断两次：第一次为真就不解档（省掉运行中读档那份 40 ms 的解档）；第二次防解档期间开始放音乐。
- 存档快照每次新建、不做处理器成员：宿主可能在别的线程存档，两次存档同时进行时共用一份会互相覆盖。
- `prepareToPlay` 不进这把锁（宿主约定它与存读档同在消息线程）。
- TSan 构建把界面版 JUCE 一起插桩（只插测试源码会有假报告）；TSan 进 CI（理由见 `ci.yml` 注释）。
- 旧工程夹具从当前存档改出来，不另存二进制夹具。
