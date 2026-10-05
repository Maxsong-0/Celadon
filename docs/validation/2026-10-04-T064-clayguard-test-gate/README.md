# T-064 · ClayGuard 测试的快路门常数与断言（2026-10-04，DGX Spark 本机）

Max 2026-10-04 19:4x 同意做（T-060 拍板档第 1、2、3 条）。**只改测试**，产品码与声音不变。

## 改了什么

| 位置 | 改前 | 改后 |
|---|---|---|
| `ClayGuardTests.cpp` `specConstants()` | 快路门 0.3（根治修订 3.4 的旧值）；注释说「与出厂相同，快路门例外」 | 0.1 = 出厂值（ADR-014）；九个数与出厂逐位相同，`testConstantsAndDefaults` 里核对 |
| 门内用例 | 1b：慢路 0.25 + 快路 −0.3 触发、门本身 0.3；`blockScript` 第 7 段慢路 0.1 | 1b：0.05 / `nextafter(0.1)` 触发；0.1（门本身）/ 0.15 / 0.25（旧门之内）/ 0.35 + 快路 −0.9 不触发；第 7 段慢路 0.05（门内），第 8 段慢路 0.5 → 0.25（旧门之内、出厂门之外：门退回 0.3 时这一段会触发） |
| 出厂门边界 | 没有（`specConstants` 与出厂不一致，缺省构造的保护单元从没被测过门边界） | `testConstantsAndDefaults`：缺省构造的保护单元（不经 `specConstants`）自己守住门——0.05 亮、0.1 / 0.15 / 0.25 不亮 |
| 恒真断言 | `testWidthGuardRetractAndRelease`：`minMono == 0.3`（ADR-015 起收单恒等于方案，验不出「反相一路没动」）；`testBlockSizeConsistency`：`monoError`（本次轨迹对参考轨迹的差，两边恒等于方案） | 前者：提示强度 0、灯一块都没亮（`Extremes` 加 `maxPhaseFloor` / `phaseActiveBlocks`）；后者：每个输出的收单逐位 = 方案 |
| 字符串旧名 | `phase guard` / `guard floor` / `guard acts` / `fully protecting` / `factory gate 0.3`（事实错误）等 | 改成「提示（warning）/ 提示强度」、门 0.1；`ClayGlazeTests`、`GlazeControllerTests`、`HelpTextTests` 里各几处（测试名 2 个） |

改动脚本 `apply_edits.py`（逐条断言原文恰好出现一次才套用）；完整 diff 见提交。

## 验证（Spark 本机，`celadon_tests` 0 警告）

- 受影响的测试类 `--only ClayGuard,ClayGlaze,GlazeController,HelpText,HelpTextClay,HelpTextHouse`：**10082 过 / 0 败**（`targeted-tests-after.txt`）。
- **反向验证（变异测试）**：在独立源码树里逐个改坏产品码 / 测试码，增量编译、跑 `--only ClayGuard`，新测试对旧测试（main 上的版本）各一遍（`mutation_run.py`，结果 `mutation-summary.txt`）：

| 变异 | 新测试 | 旧测试 | 说明 |
|---|---|---|---|
| A 快路门比较 `<` → `<=` | 败 2 | 败 1 | 新多一条：缺省构造的保护单元的门边界 |
| B 出厂快路门退回 0.3（`GlazeConstants.h`） | **败 5** | — | `spec == factory`、缺省构造的门边界三条、既有的出厂值断言 |
| C 反相提示在高频很宽的相关低音上也亮 | 败 10 | 败 8 | 新多两条：「提示强度 0」（实际 1）、「灯一块都没亮」（实际 230 块亮）——旧的 `minMono == 0.3` 在这个变异下照样通过 |
| D 极短块（< 0.5 ms）里收单偏 1e-4 | 败 3 | 败 1 | 旧测试靠 1 样本一块的那条既有断言碰巧抓到 |
| **D2 只在 17 样本一块里收单偏 1e-4**（小于旧容差 2e-3） | **败 1** | **败 0（看不出）** | 新的「输出收单 = 方案」逐输出核对抓到（grid、随机块长 1 ～ 4096：3 个输出） |
| E 反相提示亮着时收单被拉高（ADR-015 之前的行为） | 败 23 | 败 14 | 新多 9 条（逐输出核对在每种块长下都败） |
| F 测试里 `specConstants()` 的门退回 0.3 | **败 7** | — | 1b 三条、`blockScript` 第 8 段三条、`spec == factory` |

D2 是本次替换 `monoError` 的理由：旧比较两边恒等于方案、差恒为 0，换成「对方案」才有检测力。

## 没做

- 全套测试：本次只动 4 个测试文件，受影响的测试类已全跑；并入夜间统一的 main 全套（见 TASKS）。
- 测试里的标识符名（`testWideBassNeverTriggersPhaseGuard`、`sawPhaseGuard` 等）不改：标识符不是用户可见文字，改了只增加噪音。
