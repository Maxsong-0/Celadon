# T-032 实现任务书 · A11 · R6-02 收尾：保护层与上游监视只核对自动部分，放开其余六处门

> 生成于 2026-09-30 08:5x（主进程）。前置：A6（GLAZE 编排器）与 A7（GLAZE 决策）已合进本分支。先读 A7 报告
> `IMPL-A7-glaze-planner.md` 第 3、8、10 节与 A6 报告 `IMPL-A6-glaze-controller.md` 第 7 节，再读研究 `R6-glaze-planner.md` R6-02 与核验 `V6-glaze-planner.md`。

## 背景（缺陷仍在）

R6-02（P0，核验确认）：手动 Heat / Glue / Clip 某一项单独超出染色预算时 `Plan::limitedByManual` 置真，编排器据此**整体**关掉保护层、校准、约束反馈（画像失配 A7 已放开），而自动 Gain / Glue / Clip 照常推响——规格「学习、过渡、校准期间保护层同样生效」在这些组合下被静默取消。A7 已加 `Plan::automaticLevelWithdrawn`（电平类自动全部撤回才为真）、序列化与 `limitPlanTo` 取或，但**只放开了画像失配一处门**：107 实测若把其余门都改看新标志，保护层（`GlazeGuard`，Glue 均值 / p95、Limit、Clip 与风格上限比）与上游监视（`UpstreamMonitor`，Heat 驱动与 Glue GR 与上限比）按**手动 + 自动总量**核对，会把手动造成的超出当成自动超限：白白退让自动 Gain，约束反馈每「过渡 + 3 s」降 1 dB 上限，直到把自动增益永久磨光（A7 报告第 3 节表）。

## 要做（根治，按根因改全）

1. **保护层与上游监视只核对自动部分**：每项约束的有效上限 = max(风格上限, 手动单独用量)，手动单独用量由决策器按手动链模型给出（A7 建议在 `GlazePlanner.h` 加纯函数，输入手动设置 + 画像 / 上下文，输出 Glue 均值 / p95 GR、Clip 深度 / 活动、Heat 驱动等手动链单独的用量；可复用 `estimateLimit` / 手动链估计），在 `updateGuardLimits` 里合成有效上限交给 `guard.setLimits` 与 `monitor.configure`（或新增一个按项覆盖上限的接口）。手动没超预算时有效上限逐位等于风格上限（必须证明：逐位契约与既有全部用例不变）。
2. **某一项「手动超预算」状态变化时清空上游监视窗**（A7 第 3 节末段：手动 Heat 拧回上限以内那一刻，旧窗口里手动造成的超出会立即触发一次 −1 dB），保护层对应项同理不留残留退让。
3. 做完 1、2 后把 `GlazeController.cpp` 剩余六处门（约 1637 / 1639 / 1669 / 1675 / 1687 / 1984 / 2295 / 2717，含 `calibrationInterruptMask` / `calibrationGateMask` 第 16 位）改看 `Plan::automaticLevelWithdrawn`；`limitedByManual` 只留给界面状态（2809 / 2955 不动）。同步 `GlazeController.h` TestProbe 第 16 位说明（「电平类自动全部撤回」）与相关注释（2292–2295、2710–2717 附近 A7 写的「为何仍停」注释要改成新事实）。
4. 测试：`GlazeTests` 第 9 组（A7 写的 `testManualHeatOverBudget` / `testManualGlueOverBudget` / `testAutomaticLevelWithdrawn`）补 A7 第 7 节第 2 条说的断言——手动 Heat 25（通透上限 20）、GLAZE 100 热素材：保护层动作过（`executedGuardDb < 0` 或探针等价量）、校准开过（`calibrationBegins ≥ 1`），同时**不磨光**（上限与自动 Limit Gain 在 30 s 内不被约束反馈降到 0；上游反馈不因手动超出触发）；手动 Glue 100 同理；手动 Gain 20（自动全撤）仍 0 / 0；手动 Heat 从 25 拧回 15 的头若干秒不因旧窗触发反馈。决策器新纯函数的单元用例写在 `GlazePlannerTests`。改前（本分支当前 HEAD）跑一遍这些新断言应失败，改后全过。
5. 档案：`01-框架/设计规格-GLAZE-实现修订.md` 末尾 A7 写的「10. 2026-09-29 修订（T-032）」小节后追加一段「A11 收尾」（Edit 增量，文件 ≤ 300 行）；`01-框架/设计规格-GLAZE-决策与校准.md` 第 7 节相关句若与新行为冲突，同样在其末尾 T-032 小节补一句。

## 你负责的文件（只许改这些）

`Source/dsp/GlazeController.{h,cpp}`、`Source/dsp/GlazeGuard.{h,cpp}`、`Source/dsp/GlazePlanner.{h,cpp}`、`Source/dsp/GlazeTypes.h`（若需新字段）、`Source/dsp/GlazeProfile.cpp`（若新字段要存档）；`tests/GlazeTests.cpp`、`tests/GlazePlannerTests.cpp`、`tests/GlazeControllerTests.cpp`（只为适配，别删既有断言）；上面两份档案。

## 约束

- 逐位契约：GLAZE 0、旧快照（算法 6 / 7）方案、手动未超预算时的全部行为逐位不变（A7 报告第 6 节的摘要口径可复用：临时程序放 107 `~/.cache/celadon-t032/A11/`，用完删）。
- 音频线程零分配：手动单独用量在方案重算（evaluate）时算好存进方案 / 槽，不在每块算。
- 存档：若新字段进 `Plan`，经 `GlazeProfile` optionalField 读写，缺省值取「与旧行为等价」的值（A7 的 `automaticLevelWithdrawn` 缺省取 `limitedByManual` 是先例）。
