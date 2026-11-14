# T-114 · 0.1.8.11 发版总线的集成验证

2026-11-13 起，会话「完成剩余任务并发布 0.1.8.11」（DGX，集成分支 `claude/complete-tasks-release-0-1-8-11-9a03ec`）。
计划与进度在 vault `04-协调/2026-11-13-T-114-发布0.1.8.11-计划.md`；本文件只记验证命令与结果，随 T-114 的 PR 进 main。

## 0. 怎么跑（CI runner 离线时的替代）

- **108（testbox `x86-1`）**：打包目录 = `git archive <提交>` 解到 `Celadon/` + JUCE 9.0.2 的 `modules/` 与 `LICENSE.md` + 入口脚本 `run-ci.sh`
  （照 `ci.yml`：Release、只编 `celadon_tests`、全套或 `--only`），`testbox submit --path <打包目录> --arch x86_64 …`。
  要编界面库的目标（截图工具、处理器测试）用 `run-gui.sh`：沙盒自带的 `testbox-apt install` 把 freetype / fontconfig / X11 的 -dev 包
  解到 `/cache/apt-root`，再做两处修正——`.pc` 的 `prefix` / `includedir` / `libdir` 指到解包目录，-dev 包里指向运行库的相对链接
  改指宿主 `/usr/lib/x86_64-linux-gnu/` 的运行库（运行库宿主已有，testbox-apt 不重复下）。
- **DGX（aarch64）**：`~/.cache/celadon-spark/bin/spark-test.sh <源码副本> build|test|run …`（全机锁）。渲染、截图都在这里。
- **108 主机 MSVC（Wine）**：`CELADON_WIN_HOST=maxsong@100.84.85.68 bash scripts/windows-wine.sh test|installer`，
  VS2022 工程用 DGX 上自编的 Linux 版 Projucer `--resave` 生成（它写的两个 JuceLibraryCode 头只差十六进制大小写，换回仓库原样）。

## 1. 开工基线（main `93af3e0` = T-108 合入后）

| 项 | 结果 | 证据 |
|---|---|---|
| 108 全量 base | 134978 过 / 0 败 | testbox `20261113-211322-c63f` |
| 108 全量 x86-64-v3 | 134978 过 / 0 败 | testbox `20261113-211322-84e0` |
| DGX 全量（aarch64） | 134720 过 / 5 败 = 已知 5 条（ColorModule NEON 4 + BalancePerformance 1） | `~/.cache/celadon-spark/logs/t114/base-tests.log` |
| 20 首 × GLAZE 0 / 35 / 70 / 100 × Clay auto / off（`--style auto --trace`） | 80 / 80 渲完；`gate_check.py` FAILS 93（与 T-108 当时的基线同一状态，比「不新增」） | `~/.cache/celadon-t114/base/songs/` |
| 起播检查（合成 House / Tech 124，从第 1 / 33 小节） | 全部通过 | `~/.cache/celadon-t114/base/listen/check.txt` |
| 截图 `--only check` | 全过 | `~/.cache/celadon-t114/base/ui-check/` |
| 官网 `pnpm build` | 通过（check-i18n、check-svg-text 37 项、check-dial-motion 110 项、tsc、vite） | 临时副本 |
| Windows：`windows-wine.sh installer`（`17486de`） | 本工程源码 0 警告、JUCE 0 警告；装 / 两种重装 / 卸载四步冒烟全过 | 108 主机 `~/.cache/celadon-win-build/Celadon-b4c458909ae6/msbuild.log` |

## 2. T-109（`27e33d0` + `e018f4d`）

| 项 | 结果 | 证据 |
|---|---|---|
| 108 GCC 定向 `--only OutputLevelTracker` | 482 过 / 0 败 | `20261113-211323-d56f` |
| 108 MSVC 全量 | 134987 过 / 0 败（修前 3 败） | `windows-wine.sh test` |
| 反证：撤修复留用例，MSVC 定向 | 475 过 / 7 败（旧 3 条 + 新用例两组 4 条） | `windows-wine.sh test --only OutputLevelTracker` |
| 反证：同上，GCC 15 定向 | 482 过 / 0 败：GCC 上撤掉修复看不出区别，防回退靠头文件 `static_assert` | `20261113-212104-8711` |

## 3. T-095 + T-110（L1，清单见 `docs/validation/2026-10-11-T095-offline-monitoring/DGX.md`）

被测树 `<集成>` = `a8b48ca`（`93af3e0` + T-109 + T-114 脚本 + L1 三个提交 + `tests/GlazeTests.cpp` 参照表一行）。都在 108 base 上跑。

| 清单项 | 结果 | testbox |
|---|---|---|
| §1 定向 `--only CeladonChainMonitoring,CeladonChainGuards,GlazeChain,ChainDigest,AdvancedMasteringChain,CeladonChain` | 1163 过 / 0 败 | `20261113-214252-c5b5` |
| §2 反证：`93af3e0` 产品码 + 新 `ChainTests.cpp` | 531 过 / 22 败：T-095 的离线断言全部失败；T-110 的「realtime」「offline export」两场景失败，第三个场景在旧码上通过（旧码听原声离线也暂停，盖住了宿主旁通，清单已照实更正） | `20261113-214254-e307` |
| §3 改坏 `step` | 5 败：「returning to realtime ramps the monitor gain」+ 预听退出的 4 条斜坡用例（预期） | `20261113-214256-fb78` |
| §3 改坏 `difference` | 1 败：「no difference crossfade tail in the offline span」 | `20261113-214258-5169` |
| §3 改坏 `integration` | 1 败：「offline original integrates the master bit-exactly」；T-110 三场景与 GlazeChain 照常过 | `20261113-214301-da53` |
| §3 改坏 `host-bypass` | 31 败：T-110 三场景各 2 条 + GlazeChain「OUT integrated」24 种配置；T-095「offline original integrates …」照常过 | `20261113-214303-5091` |
| §4 ChainDigest 对比 | 七个场景只有 `preview-listen-exit` 的 audio（T-095）与 `monitor-toggles` 的 integrated（T-110）不同，其余逐字相同 | `20261113-214543-1f97`（集成）/ `-214544-c48b`（`93af3e0`） |
| §4 全套两项 | 见第 6 节（第一波四条线都合入后统一跑一次） | — |

## 4. T-099（L2，清单见 `docs/validation/2026-11-13-T099-state-lock/README.md`）

被测：L2 的 4 个提交摘到 `e790f1d` 之上的临时工作树，加主会话 3 笔——`fetch-gui-dev.sh` 在 testbox 沙盒改用 `testbox-apt` 取包（沙盒没有 apt 包列表）、
运行中读档不读 GLAZE 时清掉上一次读档点亮的旧工程提示（`GlazeController::keepStateOnLoad` + 两侧用例）、验证记录第 8 节。
标签里的 `11bed28` / `e1c2d73` 是临时工作树里的提交；摘进集成分支后是 `6b9122f` / `9d3b840`（内容相同，合并树见第 6 节）。
入口 `run-processor-tests.sh <模式>`，都在 108 跑。

| 项 | 结果 | testbox |
|---|---|---|
| `release`（L2 原样 + 沙盒取包） | 201 过 / 0 败；`celadon_ui_snapshot` 编得过 | `20261113-222747-e95a` |
| `release`（补 `keepStateOnLoad` 后） | 208 过 / 0 败 | `20261113-223328-9767` |
| `tsan` | 208 过 / 0 败；TSan 报告 0 份 | `20261113-223328-d48c` |
| `tsan-before`（去掉存读档的两处回调锁，只跑 `ProcessorConcurrency`） | 修前 TSan 报告 61 份（全是 data race），栈里有 `getStateInformation` / `setStateInformation` / `LiveProfile` / `GlazeController::Impl::endBlock`：对照成立 | `20261113-223329-aef4` |
| `bench`（独占 108 的 64 核） | 见 T-099 记录第 6 节 | `20261113-225225-4f6b` |

## 5. T-111（L3）与 T-096（L5）

**T-111**：L3 的 5 个提交 + 主会话 2 笔（测试里的旧工程提示改取界面本地化表，中英两句都查、补阳性对照；预读气泡与官网 LIMIT 页写明不进宿主自动化）。

| 项 | 结果 | testbox |
|---|---|---|
| 定向 `--only HelpText,HelpTextClay,HelpTextGlaze,Localisation,GlazeStatusText,DialMotion` | 6212 过 / 0 败 | `20261113-222747-c86f` |
| 108 沙盒编 `celadon_ui_snapshot` | 8 G 内存被 OOM 杀掉（`-222748-6f28`，退出 143）；16 G 重跑编过 | `20261113-223329-b87e` |
| 官网 `pnpm build` | 通过（check-i18n、check-svg-text 37 项、check-dial-motion 110 项、tsc、vite） | `20261113-223006-6dbd` |

**T-096**：L5 的 4 个提交 + 主会话 1 笔（可移植基准那段 CMake 注明为什么读截图工具的源清单、不用 T-099 的插件源清单：要能原样追加进基线树）。

| 项 | 结果 | testbox |
|---|---|---|
| `celadon_render --selftest` / `--selftest-transport` / `--selftest-live`、`celadon_bench_portable --selftest` / `--list` | 全过（「T-048 自检全部通过」「SELFTEST,OK,0 failure(s)」） | `20261113-222750-82ca` |
| 同一任务 `--only LimiterModule,CeladonChain,CeladonChainGuards,CeladonChainMonitoring,GlazeChain,ChainDigest` | 2692 过 / 0 败；七个场景的 `digest` 行与集成分支（L1 合入后）逐字相同（插件输出没变） | 同上 |
| Python 8 条自检（`tb/run-py.sh`：measure、master_check、quality_metrics、quality_gate、blind_test、heat_ab_report、compare_perf、`run_heat_ab.sh --dry-run`） | 全过 | `20261113-222750-9ddb` |

## 6. 第一波合并树（集成分支 `6be145f`）

被测树 = `b9fb212` + L2 7 笔 + L3 7 笔 + L5 4 笔 + `bbb1971`（CMake 注释）+ `b32ee36` / `7c406cd`（`BUILDING.md`：Windows 一节改到 108；界面开发头与处理器测试的跑法）+ `6be145f`。
全量与 DGX 跑在 `7c406cd`；`6be145f` 只改 `tests/Phase2Contract.h`，用它的两个用例类定向重跑。

| 项 | 结果 | 证据 |
|---|---|---|
| 108 全量 base | 136213 过 / 1 败：`Phase2Orchestrator`「the notice check misses a notice written into the tree」（中文阳性对照，见下） | `20261113-223843-0764` |
| 108 全量 x86-64-v3 | 136213 过 / 1 败（同一条） | `20261113-223844-3a2a` |
| DGX 全量（aarch64） | 135955 过 / 6 败 = 已知 5 条 + 同一条 | `~/.cache/celadon-spark/logs/t114/int1-tests.log` |
| 108 MSVC（Wine）全量 | 136213 过 / 1 败（同一条） | `windows-wine.sh test` |
| 修后定向（`6be145f`）`--only Phase2Orchestrator,LegacyFixture,HelpText,HelpTextClay,HelpTextGlaze,Localisation,GlazeStatusText` | 108：6221 过 / 0 败；MSVC `--only Phase2Orchestrator,LegacyFixture`：119 过 / 0 败 | `20261113-224836-f644`；`windows-wine.sh test --only …` |
| 处理器测试 `release` / `tsan` | 208 / 0；208 / 0、TSan 报告 0 份 | `20261113-223845-6733` / `-223845-91a0` |
| 官网 `pnpm build` | 通过 | `20261113-223846-71f9` |
| 20 首 × GLAZE 0 / 35 / 70 / 100 × Clay auto / off | 240 行对开工基线：`audio_hash` 0 行不同，其余共有列 0 列不同（比较时忽略 T-096 新填的 `confidence`、`current_mainness`、`cold_export`、`guard_cause` 四列） | `~/.cache/celadon-t114/int1/songs/cmp-base.txt` |
| `gate_check.py` | FAILS 93 → 21，见下 | `~/.cache/celadon-t114/int1/songs/gate_check.txt` |
| 起播检查（合成 House / Tech 124，从第 1 / 33 小节） | 全部通过 | `~/.cache/celadon-t114/int1/listen/` |
| 截图 `--only check`（DGX）/ Wine 界面检查 | 全过 / all interaction checks passed | `~/.cache/celadon-t114/int1/ui-check/` |
| 截图前后对比 | 改前 15 态（开工基线构建）、改后 17 态（`--deterministic`） | `~/.cache/celadon-t114/{base/shots-before,int1/shots-after}/` |
| T-096 冒烟：一首歌 `--replay-override color.heat=0`、`master_check` | 都跑通（`metrics.csv` 有 `replay-heat0` 一行；`master_check` 退出 0） | `~/.cache/celadon-t114/int1/smoke/` |

**那一条失败**：T-111 给「中文提示没写进存档」补的阳性对照在中文上失败，抓出的是旧检查本身的空洞——`XmlElement::toString()` 把非 ASCII 字符写成 `&#x…;` 实体，
整段搜中文原句永远搜不到，这条检查对中文一直是空的。`6be145f` 改成逐节点比解码后的属性值与文字，修后 108 / MSVC 定向 0 败。

**门槛 93 → 21**：开工基线的 93 条 = 72 条「有效音乐 ≥ 30 s，成品末尾却是 unlearned / 校准无效」+ 21 条反相提示误触发。前 72 条是渲染器没填置信度列、
`gate_check` 只能按旧口径判；T-096 把置信度列填上后改按 c ≥ 0.5 判，这 72 条不再出现（有效音乐不到 30 s 的照旧记在 NOTES，不算失败）。
剩下 21 条与基线**逐字相同**：7 首歌（WTF、Will We Be OKay (Extended Mix)、emmmayiyu、l_kskdhjf、untitled、wildtom、will we be okay）× GLAZE 35 / 70 / 100，
「反相提示时长占比 > 5 %，而插件慢路 ρ 从未 < −0.2（误触发）」——Clay 低频反相提示灯的误亮检查，改动前就有，不是本批引入。完成标准第 8 条「20 首门槛 0 失败」
在本版按「对合并树 FAILS 21 不新增」判（计划第 10 节记了这处偏离）。

**改动前就有的编译告警**（交 T-098）：`tests/MusicAnalyzerTests.cpp` 4 个未用函数、`tools/ui-snapshot/SnapshotMain.cpp` 的 `showGlazeTooltip` 未用（GCC 15 `-Wunused-function`，`93af3e0` 上就有）。

## 7. 第二波前段：L4（T-099 拆锁 + T-098 剩余）与 L6（T-112 字号与对比度包 B）

被测树 = 集成分支 `f4741f5` + L4 两笔（`778363c` T-099 存读档拆锁、`28ba340` T-098 剩余清理）+ L6 三笔（`9bd5ff0` / `faa8cba` / `e6a27f8`），
三处无冲突；全量、处理器测试、DGX 都跑在 `e6a27f8`。主会话随后 4 笔，按改了什么定向复核：`1c7b8a6`（T-098：渲染器 NOTES 里删掉阶段 2 起
每组都记、成了噪声的「第一遍没走到已锁定」；`ParameterRanges.h` 注释不再指向已删的接口）、`4f898da`（T-112：官网英文页引用的两句角落提示
跟上插件改短后的原句）、`c56d74c`（T-099 记录第 6 节回填拆锁后的持锁时间）、`1e2ac53`（T-112：截图工具加 `zh-bypass`）。

| 项 | 结果 | 证据 |
|---|---|---|
| 108 全量 base（`e6a27f8`） | 136158 过 / 0 败 | `20261113-235614-c6eb` |
| DGX 全量（aarch64） | 135900 过 / 5 败 = 已知 5 条 | `~/.cache/celadon-spark/logs/t114/int2-tests.log` |
| DGX 编译告警 | 0 条（第 6 节末尾的 5 条未用函数：T-098 删了测试里 4 个没人调的函数，T-112 的新截图状态用上了 `showGlazeTooltip`） | `~/.cache/celadon-t114/int2/build-warnings.txt` |
| 处理器测试 `release` / `tsan` | 208 / 0；208 / 0、TSan 报告 0 份 | `20261113-235615-405f` / `-235616-c278` |
| `tsan-before`（去掉存读档的两处回调锁，只跑 `ProcessorConcurrency`） | 修前 TSan 报告 80 份（期望 > 0），栈里有 `LoudnessMeter::kWeight`、`GlazeController::Impl::endBlock`、`GlazeGuard::process`：对照成立 | `-235617-b362` |
| 存档摘要 `digest`：拆锁后 vs 拆锁前（`f4741f5` + 同一份摘要测试，只本地） | 12 行逐行相同（empty / partial / full，各自读回、读回后放音，各两行），两边各 8 过 / 0 败：存档字节没变 | `-235619-bf21` / `-235621-fd89` |
| 持锁基准 `bench`（独占 64 核） | 播放中存档等锁 138.6 → 0.001 ms、第一次读档 35.2 → 0.000 ms，25 次调用掉块 0；详见 T-099 记录第 6 节 | `20261114-000628-a50d` |
| 108 界面定向 `--only HelpText,HelpTextClay,HelpTextGlaze,Localisation,GlazeStatusText,DialMotion` + 截图 `--only check` | 6212 过 / 0 败；all interaction checks passed | `-235622-72a8` |
| DGX 截图 `--only check` | 全过，含 T-112 的 ⑪c 单行放得下（中英 × 70 / 125 / 100 %）、⑪d 六档字号的 em、⑪e 新装默认 125 % 与左下提示离风格选择 8 px | `~/.cache/celadon-t114/int2/ui-check/` |
| 20 首 × GLAZE 0 / 35 / 70 / 100 × Clay auto / off | 240 行对开工基线：`audio_hash` 0 行不同；共有列 0 列不同（另忽略 `slow_inverted_pct`：改接了真信号，见下）；`clay_peak_cost_db` / `clay_peak_cost_s` 两列随死探针字段删掉（恒为空 / 0） | `~/.cache/celadon-t114/int2/songs/cmp-base.txt` |
| `gate_check.py` | FAILS 21 → 0；NOTES 27，其中 21 条是反相豁免，见下 | `~/.cache/celadon-t114/int2/songs/gate_check.txt` |
| 起播检查（合成 House / Tech 124，从第 1 / 33 小节） | 全部通过 | `~/.cache/celadon-t114/int2/listen/` |
| T-096 冒烟（`--replay-override color.heat=0`、`master_check`） | 都跑通 | `~/.cache/celadon-t114/int2/smoke/` |
| T-112 改前改后对比图（`--deterministic`） | 改前 `f4741f5` 27 态、改后 29 态（多 GLAZE 标题气泡中英两张） | `~/.cache/celadon-t114/int2/l6-shots/{before,after}/` |
| 4-19 旁通：`zh-bypass`（BALANCE / COLOR / CLAY 旁通、LIMIT 开着），改前用 `f4741f5` + 同一状态的本地提交 `da33d6f` | 改后旁通卡片的滑条填充与圆圈由青变灰、字照样能读（这张图里三块的小旋钮都在 0，没有数值弧可看；弧线去色看上一行 L6 对比图里的 `zh-truepeak`：LIMIT 旁通，「上限」旋钮的弧由青变灰）；LIMIT 那张颜色照旧；中层左侧板旁通的三行变淡、LIMIT 行的出力条与紫字保留 | `~/.cache/celadon-t114/bypass/{before,after}/` |
| 收尾 4 笔：`celadon_render --selftest` / `--selftest-transport` / `--selftest-live`（`c56d74c`） | 三条退出码都是 0 | `20261114-001235-6e2d` |
| 收尾 4 笔：官网 `pnpm build`（`c56d74c`） | 通过（check-i18n、check-svg-text 37 项、check-dial-motion 110 项、tsc、vite、sitemap 46 条） | `20261114-001236-c846` |
| 108 MSVC（Wine）全量 + 界面检查（`c56d74c`） | 136158 过 / 0 败；界面检查 190 条 PASS、all interaction checks passed（含 T-112 的 22 条：单行放得下中英 × 70 / 125 / 100 %、六档字号 em） | `windows-wine.sh test` / `ui` |

**门槛 21 → 0**：第 6 节剩下的 21 条（7 首 × GLAZE 35 / 70 / 100，「反相提示时长 > 5 %，而插件慢路 ρ 从未 < −0.2」）是量法的问题，不是插件误触发：
`metrics.csv` 的 `slow_inverted_pct` 从阶段 2 起读的是一个没人再写的探针字段，恒为 0，于是凡是反相提示亮过的歌都被判成「误触发」。L4（T-098）把这一列
改接分析器实时视图的 `liveLowCorrelation`——ClayGuard 判反相用的同一个信号，阈值同为 `clayGuardPhaseCorrelation`（−0.2）。改接后这 7 首慢路 ρ < −0.2
的块占 8.0 ～ 19.8 %，按 `gate_check` 的规则记成「有真反相段」的豁免（NOTES），FAILS 0。插件声音没变（上一行 `audio_hash` 0 行不同）。
豁免条件是「ρ < −0.2 的块 > 0」，偏宽：这一批最小也有 7.98 %，判法宽窄不影响结果；要不要收紧成按比例比，记作后续。

## 8. 质量基线两行（M4 第 1 步）与 B9 第二项的口径修正

ADR-034 决定 2 的两行：同一网格、同一指标脚本，只换渲染器。DGX（aarch64），全机锁里跑（`run_quality_baseline.sh`）。

| 行 | 渲染器源码 | `celadon_render` sha256 | 渲染（UTC） |
|---|---|---|---|
| 基线 | `1452a91`（`origin/claude/t096-step1` = `3a7a367` + T-096 工具；插件源码只多 `LimiterModule` 的只读计数与 `getLimiterForTesting`） | `b4909461…05d4` | 2026-11-14 08:38～08:52 |
| 现行 | `3f43ea0`（第二波前段合并后、L7 之前） | `c400a7af…9ba2` | 2026-11-14 08:23～08:38 |

- 网格：22 首门槛曲（DGX 上 20 首）× GLAZE 0 / 35 / 70，风格自动、修坯自动、`--trace`；Jamendo 45 首 × GLAZE 35 / 100，另加 `--segments-csv --bpm`。两行渲染失败 0。
- **基线这一行的声音就是已发布的 0.1.8.10**：0.1.8.10 的版本号提交 `e0d509c` 是 `3a7a367` 的祖先，两者之间没有 DSP 提交。所以两行的差 = 0.1.8.10 → 0.1.8.11（听感批之前）
  的声音变化，来自 T-097（守护族）、T-106（G_trim 换误差口径）、T-108（删曲风与交付）。
- 新口径（第 8.2 节）的两行表入库 `docs/validation/2026-10-11-T096a-metrics/baseline/2026-11-14-T114/`（`quality-22.csv` / `quality-45.csv` 各两份与 `README.md`），
  以后的候选都对「现行」那两份判；渲染产物不入库，在 DGX `~/.cache/celadon-t114/quality/{base3a7,current}/`。

### 8.1 旧口径的结果，与 B9b 的量具问题

旧口径（B9b = `gain_sign_changes_per_min`）下，现行对现行 0 / 0 / 0；基线 → 现行 A 组越线 0，B 组 22 首 20 条、45 首 170 条，全在 GLAZE > 0（GLAZE 0 的 20 行各项逐位相同）：

| 项 | 22 首 | 45 首 |
|---|---|---|
| B2a 落差 | 1 | 12 |
| B2b LRA | 3 | 23 |
| B3 PSR | 2 | 22 |
| B4 泵感 | 1 | 10 |
| B5 低频峰均 | 2 | 12 |
| B7a 低频相关 | 0 | 2 |
| B7b S/M | 3 | 37 |
| B9b 增益变号 | 8 | 52 |

B9b 那 60 条是量具的问题，不是控制变差：trace 的 `limit_gain_db` 只记 3 位小数，增益停住时在舍入边界上 0.001 dB 来回，逐块数正负翻转数到的大多是这个。
现行的增益更稳（B9a 总变差的逐行中位差：22 首 −2.10 / −2.15、45 首 −0.88 / −0.73 dB 每分钟）、停住的时间更长，翻转反而更多。按各组 GLAZE 档读 trace 重算（`b9_sensitivity.py`，只读）：

| 组 · GLAZE | 旧「变号」 | 死区 0.01 dB 的变号 | 回差 0.05 dB | **回差 0.1 dB（采用）** | 回差 0.3 dB | 回差 1.0 dB |
|---|---|---|---|---|---|---|
| 22 首 · 35 | 4.14 → 4.44（4.97 → 15.12） | 0 → 0 | 3.31 → 0（3.09 → 0.95） | **2.25 → 0（2.65 → 0.73）** | 0.95 → 0（1.68 → 0.50） | 0 → 0（0.26 → 0.12） |
| 22 首 · 70 | 1.78 → 3.25（5.10 → 5.06） | 0 → 0 | 1.78 → 0.87（3.26 → 1.67） | **1.54 → 0（2.76 → 1.06）** | 1.54 → 0（1.81 → 0.72） | 0 → 0（0.25 → 0.23） |
| 45 首 · 35 | 0.83 → 9.26（1.55 → 10.26） | 0 → 0 | 0.44 → 0.28（0.84 → 0.47） | **0.31 → 0（0.71 → 0.31）** | 0.21 → 0（0.51 → 0.15） | 0 → 0（0.15 → 0.02） |
| 45 首 · 100 | 1.98 → 4.49（2.74 → 6.89） | 0 → 0 | 1.52 → 1.35（1.92 → 1.63） | **1.43 → 1.19（1.65 → 1.36）** | 0.93 → 0.51（1.13 → 0.71） | 0.34 → 0.22（0.41 → 0.33） |

单位：次 / 主段分钟；格子里是「基线 → 现行」的中位数，括号里是均值。翻转的大小：45 首 GLAZE 35 现行 1984 次里 1915 次（96.5 %）前后两步都只有一个量化步（0.001 dB），
超过 0.01 dB 的只有 11 次（最大 0.025 dB）；基线 345 次里这个比例是 39.4 %。回差取 0.05 ～ 1.0 dB 结论都同向（现行不多于基线），0.1 dB 是量化步长的 100 倍、
又远低于听得出的电平变化，T-113 1-05 那种几 dB 的拉锯照样一次不漏。

### 8.2 修正：`449340a`

B9 第二项改数回差拐点：每段连续主段里，增益从上一个极值反向走够 `GAIN_REVERSAL_DB`（0.1 dB）才算一次（第一次走够只定方向；隔着非主段不连），
字段改名 `gain_reversals_per_min`（`quality_gate.py` 读不到旧列就报缺列，新旧口径的表比不到一起）。新增自检：1 dB 起伏叠 0.001 dB 抖动只数 2 个拐点（逐块翻转 250 次）、
0.08 dB 起伏叠抖动数 0 个。108：八条 Python 自检全过（`20261114-010423-f691`，`quality_metrics` 14 / 14、`quality_gate` 12 / 12）。

### 8.3 新口径重算（不重渲，只重算指标）

不重渲，只用 `449340a` 的 `quality_metrics.py` 重算两行的指标（`dgx-remetric.sh`，2026-11-14 09:08～09:36 UTC），再跑门槛：

| 比较 | 22 首 | 45 首 |
|---|---|---|
| 现行对现行 | A 0、B 0、缺行 0 | A 0、B 0、缺行 0 |
| 基线 → 现行，A 组越线 | 0 | 0 |
| 基线 → 现行，B 组退步 | 20 → **12**（B9b 8 → 0） | 170 → **119**（B9b 52 → 1） |

其余各列的条数与旧口径逐条相同（只换了 B9b 一列）。新口径下 B9b 的中位数：22 首 GLAZE 35 / 70 是 2.25 → 0、1.54 → 0，45 首 GLAZE 35 / 100 是
0.31 → 0、1.43 → 1.19 次 / 分。剩下的 1 条是 `track_1033705` GLAZE 35：拐点 1.10 → 2.19 次 / 分（线 1.64）。这一条是真的来回——现行主段里
12 个拐点、摆幅 0.5～1.5 dB，多是预算环退一下、几秒到一分钟后放回（例：275.6 s 2.39 dB → 277.6 s 1.21 dB → 337.8 s 2.39 dB）；基线只有 6 个拐点，
但摆得更大（主段增益范围 0～3.35 dB，现行 0.83～2.39 dB）。这正是 T-113 1-05 要治的形状，`c105` 的逐 drop 报告会给对照。

### 8.4 0.1.8.10 → 现行，B 组还剩什么（只报不拦）

剩下的是 0.1.8.10 → 0.1.8.11（听感批之前）真实的声音变化，来自 T-097、T-106、T-108；三笔各占多少没有分开量。45 首是 Jamendo House，
T-108 删了 House 曲风的整套处理，45 首的变化比 22 首多得多，像是主要来自它——这是推测，没量过（要分开，再量一行 `0601dcd`，即 T-108 之前）。
按计划第 10 节的决定只报不拦：这些改动合进 main 时还没有 B 组量尺，ADR-034 的门管的是之后的候选对「现行」。

Δ = 现行 − 基线，取逐行差的中位数；括号里是变差的那些行的变差幅度 P90。F = 门槛判的退步条数。

| 项（好的方向） | 22 首 F（35 / 70） | 45 首 F（35 / 100） | 45 首 GLAZE 35：Δ 中位（变差 P90） | 45 首 GLAZE 100：Δ 中位（变差 P90） |
|---|---|---|---|---|
| B2a 落差 LU（越小越好） | 1 / 0 | 6 / 6 | +0.098（1.454） | −0.037（1.407） |
| B2b LRA 变化 LU（越大越好） | 2 / 1 | 9 / 14 | −0.263（1.593） | +0.027（1.680） |
| B3 主段 PSR dB（越大越好） | 1 / 1 | 15 / 7 | −0.249（0.462） | +0.002（1.190） |
| B4 泵感 dB（越小越好） | 0 / 1 | 5 / 5 | +0.040（0.195） | −0.004（0.330） |
| B5 低频峰均变化 dB（越大越好） | 2 / 0 | 5 / 7 | −0.048（0.358） | +0.002（0.606） |
| B7a 低频相关变化（越大越好） | 0 / 0 | 1 / 1 | 0.000（0.007） | 0.000（0.019） |
| B7b S/M 变化 dB（越小越好） | 2 / 1 | 17 / 20 | +0.021（0.461） | −0.003（0.219） |
| B9a 增益总变差 dB / 分（越小越好） | 0 / 0 | 0 / 0 | −0.875（0.225） | −0.734（1.426） |
| B9b 增益拐点 次 / 分（越小越好） | 0 / 0 | 1 / 0 | 0.000（0.679） | −0.219（1.825） |

读法：
- 45 首默认 GLAZE 35 压得稍多：PSR 逐行中位少 0.25 dB、LRA 少 0.26 LU，少数歌更多（变差那部分 P90：PSR 0.46 dB、LRA 1.59 LU）；GLAZE 100 时中位基本不动。
- 增益明显更稳：总变差逐行中位每分钟少 0.7～2.2 dB（22 首 −2.10 / −2.15，45 首 −0.88 / −0.73）。
- B7b 的 37 条多是贴线：45 首 GLAZE 35 的 S/M 变化中位基线 0.508、现行 0.514 dB，线是 0.5 dB，逐行中位差只有 +0.021 dB。
- 22 首的变化小得多：只有 12 条；除 B9a 外各项逐行中位差都在 ±0.12 以内（B2a 只有 2 首算得出，不计），主段 PSR 中位还略升（GLAZE 35 / 70：+0.08 / +0.12 dB）。
- 这一段写进 0.1.8.11 版本档的「声音变化」，按 ADR-035 说明 GLAZE > 0 的旧工程声音会变。

## 9. 听感批 L7（T-113）的集成（M4 第 2 步）

- 摘进集成分支 6 笔：`c338d1f` / `603aa52` / `e17aecb` / `8518377` / `5b81e5d` / `8c22147`（= 测试树 `wt/int-l7` 的 `27278ed`～`073f13c`，含主会话补的
  `drop_report.py` 空序列求和写成整数 0、3 处局部名遮住类成员的改名），推送 `9aa18f7..8c22147`。
- 108：定向 7728 / 0（`20261114-010941-3438`）、全量 136195 / 0（`20261114-010944-65e6`）、渲染器 `--selftest` / `--selftest-transport` / `--selftest-live`
  与 `celadon_bench_portable --selftest` 过（`20261114-010951-08b5`）；改名后定向 5933 / 0，MSVC 5933 / 0、本工程 0 警告。
- DGX aarch64 全量 135937 / 5，5 条就是已知的 ColorModule 打包立体声快路径（第 1 节），没有新增。
- 出厂网格、候选网格、逐 drop 报告、B 组、CPU 与两批盲听的结果在 `docs/validation/2026-11-13-T113-listening/README.md` 第 8 节。

## 10. 独立复查第 1 轮（M6，`wf_e09be6cb-b17`）

- 范围 `93af3e0...8c22147`（去掉 csv / tsv），深档：3 个找问题代理（规格 / 质量 / 边界）+ 3 个核实代理，Sonnet 5.5 · medium；
  8 条必查项：锁、出厂逐位、GLAZE 0、存档兼容、界面、CI 与脚本、量具、删死代码。
- 16 条发现，合并重复后：要改 1、小毛病 9、改动前就有 0、核实否掉 1。
- 提前到 M5 之前做（计划第 10 节 01:4x）：DGX 长跑的时间里并行；复查改的提交和别的改动一起在 M5 统一验证。

| 编号 | 问题 | 裁决 | 提交 |
|---|---|---|---|
| F10（= F1、F5） | 质量门 A 组（真峰值、硬钳位）新表和基线都没有读数时判 N/A、退出码 0：没量过当通过 | 改：A 组没有读数一律失败；同类 `gate_check.py` 成品行没有真峰值读数也失败；两边自检各加用例 | `f74ee29` |
| F2 | 官网首页的等响演示与说明还写「A / B」「B 原声」「A 等响后」 | 改：中英 13 处改成「等响对比」「原声」「母带 · 等响后」；演示里按 B 键切换不动 | `43099cc` |
| F6 | COLOR 卡片英文副标题改成 Colour 后，官网英文界面表还是 Tone | 改官网表格与词汇表；插件里的 Colour 是 T-111 有意的（不和第一个旋钮 Tone 同名），不改 | `43099cc` |
| F4、F14 | 9 处描述现状的注释还把已删的「重新聆听」当现行功能，A / B 旧叫法 | 改（只改注释）；指历史的照 T-098 的约定保留 | `4949166` |
| F11 | 1-06 上行快通道打开后，推进的事件时钟写进存档，0.1.8.10 读档的上界不认，整份实时画像冷启动 | 1-06 这版不进（出厂关，见 T-113 记录第 8 节），常数旁写明打开前要先解决向下兼容 | `e0cfbd7` |
| F8 | Windows 工具链的 cmake / ninja 只按版本号从 PyPI 装，同一脚本别的下载都有校验 | 改：按 PyPI 公布的轮子 sha256 `--require-hashes` 安装；108 上正确哈希装上 cmake 4.4.4 / ninja 1.13.2，错哈希报 Hash mismatch | `2fa0013` |
| F15 | `fetch-gui-dev.sh` 把目录里全部 .deb 依次解开，同一个包新旧两版并存时可能旧的盖新的 | 改：只解清单上的包，每个包要正好 1 个 .deb，否则报错；DGX 正常 21 个包 rc 0，放一份重复的 rc 1 | `9e1aa22` |
| F16 | DGX 入口 `spark-test.sh` 路径写死、没有预演、没有平台检查 | 改：`dry` 预演、只在 Linux aarch64 上跑、`JUCE_DIR` 可改、缺界面开发头先说清；`dir` 输出与旧版逐字相同；DGX 在用的那份原子替换 | `7bc39ef` |
| F3 | 读档拆成锁外 stage、锁里 commit 后，keepOriginal 在锁外按 `! prepared` 定下；两步之间 prepare 先完成，暂存的原件没人取走 | 本版不改：stage / commit 之间的窗口是 T-099 拆锁新带来的，底下「prepare 与读档没有互斥」改动前就有（`93af3e0` 里 `prepared` 与 `stateRestoredBeforePrepare` 也不加锁），设计前提是两者都在消息线程串行；记后续，连同互斥一起做 | — |
| F7、F13 | 0.1.8.10 默认大小 1.0 不写进设置文件，从没设过默认大小的老用户升级后新开的实例变 125 %（编辑器开过的旧工程照旧：0.1.8.10 打开编辑器时 `resized()` 就把大小写进工程的 uiZoom） | 交 Max 定；推荐接受，更新日志已写明「没设过默认大小的，新开的实例改为 125 %」。这类老用户认不出来（0.1.8.10 没碰过设置就不写设置文件） | — |
| F12（= F9，否掉） | `compare_perf.py` 阈值与 `perf-dgx.sh` 的 `LOAD1_MAX` 留空，CPU 比较永远不标退步 | 文件头写明的占位（首跑后填）；照计划按 CPU 实测填，见 T-113 记录第 8 节 | — |

验证（`7bc39ef` 与 108 验过的快照 `7575ecf` 树相同）：Python 自检 `20261114-020956-965a` 全过（`quality_gate` 14 / 14、`phase2_gate_selfcheck` 含
「missing true peak fails」、`drop_report` 10 / 0、`blind_test` 122 / 0）；C++ 定向 `20261114-021135-9ef6` 15309 / 0。
