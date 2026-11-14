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
