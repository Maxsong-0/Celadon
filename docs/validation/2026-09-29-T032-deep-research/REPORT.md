# T-032 · 总报告（2026-10-01）

全项目算法与设计缺陷深度研究，以及当晚开始的落地。研究 10 个子系统共 76 条结论，反驳核验确认 67、驳回 6、存疑 3（`SYNTHESIS.md`）。
11 条实现线（A1～A4、A6～A11，没有 A5）与主进程的构建 / 脚本改动已全部合进 main：PR #10，`f12d092`。

## 1. 各线

| 线 | 子系统 | 主要条目 | 实现报告 | 验证在 | 各线自己的全套 |
|---|---|---|---|---|---|
| A1 | Balance | R2-01 推响不刺误判（P0）、R2-05 / 06 / 07 / 08 | `IMPL-A1-balance.md` | 107 | 见报告 |
| A2 | Color | R3-01 Glue mid 检测器伪像（P1）、R3-04、Color 守卫（R8-02） | `IMPL-A2-color.md` | 107 | 见报告 |
| A3 | Limit | SSE2 真峰值核共用预广播系数表、van Herk 滑动最小值、Ceiling 延迟线重置、Limit 守卫（输出逐位不变） | `IMPL-A3-limit.md` | 107 | 见报告 |
| A6 | GLAZE 编排器 | R5-01（P0）、R5-02（P0）、R5-03 / 04 / 08、R6-04、R5-06 | `IMPL-A6-glaze-controller.md` | 107 | 133151 / 0 |
| A7 | GLAZE 决策 | R6-01（P0，算法 8）、R6-02 一部分、R6-06 | `IMPL-A7-glaze-planner.md` | 107 | 133144 / 0；并入后 133750 / 0 |
| A11 | GLAZE（R6-02 收尾，P0） | 保护层与上游监视只核对自动部分（`Plan::manualBudgetMask`）；手动超预算的状态变了就清窗；其余 8 处门改看 `automaticLevelWithdrawn` | `IMPL-A11-r602-guard-auto-share.md` | 云端 | 默认 133834 / 1（当时 10 % 的计时预算）；v3 133835 / 0 |
| A4 | 链路 / 处理器 / 表 / 发行安全 / 参数真源 / 渲染工具 | R8-01 + R10-03、R8-03、R8-04、R7-04、R10-08、R10-01（P0）、R10-02、R8-02、R10-06 | `IMPL-A4-chain.md` | 云端 | 133388 / 2（环境）；试合并 134266 / 1 |
| A8 | Clay | R4-01 守护回收加宽（P0，A' 拆分增量）、R4-02 | `IMPL-A8-clay.md` | 云端 | 133092 / 2（环境）；合并预演 5816 / 0 |
| A9 | 界面：粒子带 / 旋钮 / 瓷面 / 定时器 | R9-01②、R9-02、R9-05 / 06 / 07 / 08、R9-12 / 13 / 14、M-3、M-4 | `IMPL-A9-ui-band.md` | 云端 | 132979 / 2（环境）；截图检查 76 / 76 |
| A10 | 界面：频谱仪 | R9-04 ①②③④、R7-05 / R9-10、R9-11、关 IN 时不做 IN 的 FFT | `IMPL-A10-ui-spectrum.md` | 云端 | 133037 / 2（环境） |
| 主进程 | 构建 / 脚本 | R10-04 CMake 配置期核对 `.jucer` 源文件清单；R10-05 `test-linux.sh` 透传 `CELADON_CXX_FLAGS`、按标志分构建目录 | 本报告 | — | — |

- 「云端」= 2026-10-01 的云端容器：4 核，GCC 13.3（107 是 GCC 15.2），JUCE 9.0.2，Release + Ninja。界面线在 xvfb 下用软件渲染。那天 107 / 109 / 112 与 Mac 都连不上。
- 「环境」= 这台容器上两条计时用例在未改动的 main 上也不过：`ColorModule` 的「Color GLAZE 100 … (limit 1.2 %)」（main 1.296 %），`BalancePerformance` 的「@96000 mean … exceeds the 10 % audio-thread budget」（main 11.353 %）。后一条的预算已由 Max 定为 15 %（PR #8），各线合进 main 后不再报败。
- A4、A8、A11 的实施代理在写完报告前中断。报告里的几处最终数字由主进程按它们留下的运行日志补齐，各报告开头有说明。

## 2. 集成

集成分支（PR #10 的头 `3a17ae9`）按下面的顺序在 main（`f654d1f`）上做：

1. `2abc69e` 合并 `claude/fable-deep-research-634a5e`：研究证据、A1 / A2 / A3 / A6 / A7、R10-04 / R10-05。
2. `67b9da9` 合并 A11。
3. 依次合并 A4（`b37632c`）、A8（`fd82d2f`）、A9（`63b5075`）、A10（`0eea963`），都没有冲突。
4. `df48815` 打开 A4 整链守卫里等 A2 / A3 的 9 个字段：`colorLimitGuardsMerged = true`。
5. `3a17ae9` 合进当时的 main（T-033 官网，只有 `site/` 与 `.gitignore`），不进 C++ 构建。

合并中两处自动合并的文件都核过：
- `scripts/test-linux.sh`：T-034 的同步排除项与 R10-05 的 `CELADON_CXX_FLAGS` 并存，`bash -n` 通过。
- `tests/BalancePerformanceTests.cpp`：15 % 预算与 R2-07 的注释并存。

## 3. 集成后的验证（云端容器，树 = `df48815`）

| 项 | 结果 |
|---|---|
| 全套，默认标志 | 134522 过 / 1 败：`ColorModule` 计时项「Color GLAZE 100 1.224 %（限 1.2 %）」，当时同机还在跑另一轮全套与截图工具的编译 |
| 同一项，机器空闲时复跑 | `--only ColorModule` 集成版两遍 844 / 0（1.179 %、1.149 %）；T-032 合并前的 `3ec4648` 交替两遍 1.115 %、1.141 %；Color 相关源码与 `3ec4648` 逐文件相同——环境，不是回归 |
| 全套，`-march=x86-64-v3`（ADR-013：改了 DSP 文件要加跑） | **134523 过 / 0 败**；已核对 `build.ninja` 带了这个标志 |
| 编译警告 | 只有既有两条：`ColorModule.cpp:404` 经 stl 的 `-Wstringop-overflow`、`ChainTests.cpp:168` 的 `-Wrange-loop-construct`；新增 0 |
| 界面截图工具（A9 + A10 合在一起） | 编过，警告只有上面那条既有的；`--only check` 76 / 76 |
| 离线对照工具 `celadon_render` | `--selftest` 17 / 17；合成歌 GLAZE 0 / 35 × 修坯 自动 / 全关 渲染完成 |

离线对照工具另有一处改前就有的失败，与 T-032 无关，列「待 Max」：
- 失败的是：合成歌、GLAZE 35、修坯「自动」那一组的同决策重放阳性对照，最大差 −37.6 dBFS（ADR-016 要求 ≤ −180 dBFS）。
- 在 `e6b445b`、A4 改后、集成树上数值完全相同。详见 `IMPL-A4-chain.md`「待 Max」第 5 条。

## 4. 待 Max

不在 T-032 范围、留给 Max 拍板的条目见 `SYNTHESIS.md`「待 Max 拍板」。各线报告里的「待 Max」：

- **A1 / A2 / A3 / A6 / A7**：见各报告。
- **A4**：
  - 宿主旁通期间是否也冻结 OUT 整体响度。开关 `CeladonChain::pauseIntegrationOnHostBypass` 现为 false；改成 true 时 `GlazeTests` 参照表要跟着加一行，A4 试合并验过。
  - 规格 §6 预听口径。
  - 门槛与试听包按新工具重跑。
  - 性能基准 modules 模式口径变了。
  - 上节那处重放对照失败（待认领）。
- **A8**：手动收单是否也拆出守护；R4-03 守护回收指示、R4-04 重标定待认领。
- **A9**：静态层阴影成本的做法；72 / 100 Hz 屏上 36 / 33 fps 是否接受；瓷面变锐、非整数缩放下 ≤ 0.5 px 的对齐；角注是否保留缓存。
- **A10**：像素差异是否接受；估计量口径（R7-01 / R9-03）；176.4 / 192 kHz 的窗约 93 / 85 ms。
- **A11**：手动 Clip 单独超预算时，自动 Gain 继续推深削波是否合意；第 6 节两种口径差要不要处理。
- **本轮没能做的**：
  - Mac 构建与 Mac 上的实测（主线程占用、CoreGraphics 下的界面耗时）。
  - 109 上的 MSVC 构建。
  - 22 首门槛与 A/B 重跑。
  - 档案（Obsidian）同步：各报告末尾「需要同步到档案的内容」，以及 ADR、日志、INDEX、TASKS。
