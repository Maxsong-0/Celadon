# T-099 存读档与音频线程互斥 · 验证记录（2026-11-13）

> 状态：代码在分支 `t114/l2-t099`（基点 `27e33d0`）。**写这份记录的代理没有编译、没有跑测试**；第 5 节的命令由主会话在 108（testbox）上跑，
> 第 6 节的数字跑完回填。决定落在 vault `03-记忆/ADR-040-运行中读档参数照换画像保留.md`。
> 来源：体检报告 `docs/validation/2026-11-12-T105-plugin-review/2-features.md` 的 2-01、2-03、2-10；Max 2026-11-12 拍板第 4、11 项。

## 1. 改了什么

| 问题 | 改法 | 位置 |
|---|---|---|
| `getStateInformation` / `setStateInformation` 直接调编排器的存读档，读写音频线程每块在改的量（体检 C-T099） | GLAZE 那一段在 `getCallbackLock()` 里做；JUCE 9.0.2 的 VST3 / AU / AAX / Standalone 处理每一块时都持这把锁，互斥成立 | `Source/PluginProcessor.cpp` getStateInformation、setStateInformation |
| 运行中的实例读档（换宿主预设、撤销）会把实时画像换成存档里的 | 运行中 → 只换参数与参考频谱，GLAZE 存档整份不读（拍板第 4 项） | 同上，`keepsLiveStateOnLoad` |
| Lookahead 可自动化 = 播放中改延迟（体检 2-10） | `withAutomatable (false)`（拍板第 11 项） | `Source/Parameters.cpp` |
| 两处注释描述已删的交接机制（体检时 `:900`、`:925-926`） | 改成现在的线程约定与读档顺序 | `Source/PluginProcessor.cpp`、`.h` 文件头 |
| 处理器层没有自动测试（体检 2-03） | 新目标 `celadon_processor_tests`（含 TSan 版）进 CI | `tests/processor/`、`CMakeLists.txt`、`.github/workflows/ci.yml` |
| 108 / CI runner 没有界面开发头，编不了插件源码 | `scripts/fetch-gui-dev.sh`：官方 -dev 包解到用户目录 | `scripts/` |

## 2. 锁的范围

| 步骤 | 在锁里？ | 为什么 |
|---|---|---|
| 存档：`parameters.copyState()`、写格式号 | 外 | APVTS 自己加锁复制，参数值是原子量 |
| 存档：`chain.createGlazeStateXml()` | **里** | 读编排器与实时画像的运行状态。编排器在这一步里就把 LIVE 压缩（zlib 9）+ base64 了，不改 `GlazeController` 拆不出锁外 |
| 存档：参考频谱、挂到根元素、`copyXmlToBinary` | 外 | 参考频谱自带票号 + 逐字原子发布（`ReferenceSpectrum.h`） |
| 读档：`getXmlFromBinary`（外层解压、解析）、摘节点、`replaceState` | 外 | 同上；参数先换 |
| 读档：判断要不要读 GLAZE（`keepsLiveStateOnLoad`）+ `chain.restoreGlazeStateXml` | **里** | 判断读的是音频线程的量；落位写的是音频线程的量。编排器在这一步里解压 LIVE、重算画像，拆不出锁外 |
| 读档：参考频谱 | 外 | 自带同步 |

`prepareToPlay` 与存读档不加锁互斥：VST3 / AU 约定这几个调用都在消息线程，不会同时进行。

## 3. 运行中读档的判定

「运行中」= 处理过至少一块非空音频（`processedAudio`，音频线程置位、只置不清）**且**实时画像已发布的那一份里有过有效音频（`LiveDiagnostics::activeSeconds > 0`）。在锁里判断。

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
| `ProcessorStateLockBench`（`--bench`） | 放 1084 s 合成音乐把回看窗口填满（512 段），量存档大小、单线程存档耗时、按真实时间节拍（48 kHz / 64）播放时存档 / 运行中读档 / 第一次读档让音频线程等锁的最长时间与掉块数 |

旧工程夹具不另存：按 `addMissingParameterNodes` 与 `LegacyState.h` 认的差别，从当前版本存出来的档改出来。GLAZE 格式 1 的真实夹具在 `tests/LegacyFixtureTests.cpp`（链层，不复制）。

TSan「修前报」靠的是第 ② ③ 条与第 ① 条：去掉锁后，① 的存档与音频线程同时读写 LiveProfile / 编排器，② 的落位与音频线程同时写，③ 的判断读与音频线程同时写——都应该报。

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

模式（同一个入口脚本）：`release`、`tsan`、`tsan-before`、`bench`。**先单独交 `release`**：它第一次在 `/cache` 装 cmake / ninja、解界面开发头（`/cache/celadon-guidev`），并行首装会互相覆盖；装好之后其余三项可以各用一个打包目录并行交。`bench` 要独占机器。

| 模式 | 期望 |
|---|---|
| `release` | `celadon_ui_snapshot` 编得过；`==== celadon_processor_tests: N passed, 0 failed ====` |
| `tsan` | 同上 0 failed，且「TSan 报告份数：0」 |
| `tsan-before` | 「修前 TSan 报告份数：K（期望 > 0）」，K > 0，脚本退出 0；报告的栈里应有 `GlazeController` / `LiveProfile` / `CeladonAudioProcessor::getStateInformation`/`setStateInformation` |
| `bench` | 打印 `T099-BENCH …` 几行（见第 6 节）；窗口段数检查 = 512 |

全量 `celadon_tests` 照 `run-ci.sh base` / `x86-64-v3` 跑，与基点 `27e33d0` 比不新增失败（这条线没改 DSP，`celadon_tests` 的源码清单也没动）。

TSan 有额外报告时：先看栈里有没有 `celadon::` 或 `CeladonAudioProcessor`——有就是真问题，别压；只在 JUCE 内部或系统库的，确认与存读档无关后加进 `tests/processor/tsan.supp` 并在这里记一笔。

## 6. 持锁时间

「锁里编码」一栏 = 本节方案之前的实现（编排器在锁里压缩 / 解压），2026-11-13 22:5x 在 108 独占 64 核实测：集成分支 `6be145f`，
`run-processor-tests.sh bench`，testbox `20261113-225225-4f6b`（块长 64 样本 = 1.333 ms，48 kHz）。

| 量 | 锁里编码（`6be145f`） | 拆锁之后 |
|---|---|---|
| 最坏画像（512 段）的存档大小：整份 / GLAZE 节点 / 解开后的 LIVE | 728175 B / 726594 B / 1737099 B（窗口 512 段，pending 0） | 待 108 回填 |
| 单线程 `getStateInformation` 一次 | 中位 138.2 ms（137.7 ～ 140.3） | 待 108 回填 |
| 播放中存档：音频线程等锁最长 / 单块最长 / 掉块数（对照：空跑） | **138.6 ms / 138.7 ms / 10 次存档掉 10 块**；空跑 0.000 ms / 0.128 ms / 0 | 待 108 回填 |
| 运行中的实例读档（只判断）：等锁最长 | 0.001 ms（`setStateInformation` 中位 0.088 ms），掉块 0 | 待 108 回填 |
| 第一次读档（落位）：等锁最长 | **35.2 ms**（`setStateInformation` 中位 40.1 ms），5 次读档掉 5 块 | 待 108 回填 |

结论：锁里编码时，窗口满了以后播放中每存一次档，宿主的音频线程整整停约 137 ms（远超任何常用块长，整个宿主一起断，不只这个插件）。
计划第 2 节对 T-099 的要求本来就是「解析、压缩在锁外」，所以下面方案 1、2 在 T-114 第二波做（L4），做完重跑本节填「拆锁之后」一栏。

估计量级（按代码推，没量过）：一段在 LIVE 里约 2 KB（27 段频谱的几张 double 表 + 门块功率 + 直方图），窗口满 512 段 ≈ 1 MB 二进制 → base64 ≈ 1.4 MB → 编排器里 zlib 9 级压缩再 base64。存档那一下的锁预计**数十毫秒**，读档落位（解压、解析、重算）预计**十几到几十毫秒**；运行中读档只判断，应在微秒级。

判据：一块的时长 48 kHz / 512 = 10.7 ms，/ 64 = 1.33 ms，96 kHz / 32 = 0.33 ms，192 kHz / 32 = 0.17 ms。持锁超过当前块长，那一块就掉。窗口要播十几分钟才满，前几分钟的存档小得多；第一次读档通常在宿主还没开始放音时（打开工程），掉块不出声。真正会听到的是「长时间播放中、宿主存档」——自动保存，或 REAPER 每改一次参数取一次状态做撤销点。

若实测存档持锁明显超过常用块长（几毫秒以上），后续方案（第一波 L2 不能动 `GlazeController` / `LiveProfile`，留到第二波；实测确实超了，见上）：

1. 存档拆「快照」与「编码」：锁里只把原始量拷出来（LiveProfile 的回看窗口是构造时分配好的定长条目，512 段约 1 MB 的内存拷贝，应在 0.1 ms 级；编排器的标量几百字节），base64、XML、zlib 全在锁外。
2. 读档拆「解析」与「落位」：锁外解压、解析、校验到暂存（LiveProfile 已经是「先读到临时量、校验过再落位」），锁里只换入暂存；同步重算改成标「待重算」，交给音频线程已有的分片重算（`advanceTo`）。
3. 体检 2-06 的缓存：按已发布代数缓存压缩好的 LIVE，代数没变（停播时拧参数、REAPER 的撤销点）直接复用，存档几乎不碰锁。

## 7. Lookahead 不可自动化：对旧工程的影响

- 插件这边：JUCE 给 VST3 的 `ParameterInfo.flags` 去掉 `kCanAutomate`（`juce_audio_plugin_client_VST3.cpp:918`），给 AU 标 `kAudioUnitParameterFlag_NonRealTime`（`juce_audio_plugin_client_AU_1.mm:1071-1072`）。参数 ID、范围、默认值、存档格式都不变：工程里存的 Lookahead 值照读（用例「a stored Lookahead value still loads」）。
- 旧工程里**已有的 Lookahead 自动化曲线**：去留由宿主决定，各家不一样。多数宿主把它从自动化列表里拿掉；已有的曲线有的不再回放（参数停在工程里存的那个值），有的照样回放（JUCE 包装层不拒收宿主送来的参数变化），回放时处理器层的 LookaheadHold 照旧挡着（停稳 0.3 s 才换、换一次报一次延迟）。没有在真宿主上验过（Cubase、REAPER、Logic、Live、FL），发版前抽一两个看。
- 版本说明一句（中 / 英）：「Lookahead 不再进宿主自动化：它决定插件延迟，播放中改会让声音挪动、断一下。旧工程里已有的 Lookahead 自动化，部分宿主不再回放，参数停在工程里存的值。」/ "Lookahead is no longer host-automatable: it sets the plug-in latency, so changing it during playback shifts and interrupts the audio. Existing Lookahead automation in old projects may no longer play back in some hosts; the parameter stays at the value saved with the project."

## 8. 已知边界

- 运行中的实例读档跳过 GLAZE 时，清掉上一次读档点亮的「旧的聆听结果已丢弃」提示（`Status::legacySessionDiscarded`）：
  那条提示说的是那次读档冷启动了，这次没有冷启动。入口 `GlazeController::keepStateOnLoad`（T-114 集成时补，主会话定）。
  运行中读入格式 1 的旧预设也不提示（画像保留了，没有冷启动）。用例：`ProcessorState` 的「a running load clears the legacy notice
  an earlier load raised」「a running load of a format-1 project raises no notice」；另一侧（刚建的实例读格式 1 → 提示）是
  「<= 0.1.8.5 project …」。
- 互斥靠「宿主经 JUCE 包装层、处理每一块持回调锁」。不经包装层直接调 `processBlock` 的宿主（自写的测试台、离线工具）要自己持锁——测试台架照做了。
- 锁里只调编排器的存读档，不回调宿主、不拿别的锁（APVTS 的锁在锁外用完），不会和音频线程互相等。
- 持锁时间见第 6 节；数字出来之前，「播放中存档会不会掉块」是未知数。

## 9. 我替你定了什么

- 「运行中」的判据 = 处理过音频 **且** 画像里有有效音频（第 3 节）；只看其一都有误判（静音预热的宿主、打开工程时连读两份）。
- 「画像保留」保留的是整个 GLAZE 运行状态（不只画像本身）：增量、G_trim、守护与出口电平都是从同一段音乐里来的，拆开换会互相对不上。
- 参考频谱（用户点「学习」得来的）算设置，运行中读档照换。
- 外层 XML 的解压解析、参数替换、参考频谱放在锁外；编排器内部的压缩 / 解压在锁里（不动 `GlazeController`）。
- `prepareToPlay` 不进这把锁（宿主约定它与存读档同在消息线程）。
- TSan 构建把界面版 JUCE 一起插桩（只插测试源码会有假报告）；TSan 进 CI（理由见 `ci.yml` 注释）。
- 旧工程夹具从当前存档改出来，不另存二进制夹具。
