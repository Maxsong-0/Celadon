# T-060 · 反相提示注释清理的验证（2026-10-04）

任务：Source / tests 里把反相「保护」写成会改声音的过时注释，改成 ADR-015 口径；**只改注释**。清单与待 Max 拍板的事见 vault `04-协调/2026-10-04-T-060-反相提示注释清理待Max拍板.md`。

## 怎么证明「只改了注释」

`comment_only_check.py <基点>`：对每个改动文件，把改前（基点版）与改后（工作区版）各用 `g++ -fpreprocessed -dD -E -x c++ -` 去掉注释，
归一空白、去空行后逐行比；任一文件不同、或预处理器除「`#pragma once in main file`」（头文件从标准输入喂进去必报，与改动无关）外有任何 stderr 都算失败。

```bash
python3 docs/validation/2026-10-04-T060-phase-hint-comments/comment_only_check.py origin/main   # 基点 = main b8e2d6f
```

结果见 `comment_only_check.out`：11 个文件全部 SAME（去注释后行数一一相同、内容逐行相同）。

补两项 `-fpreprocessed` 看不到的检查：

- **行尾反斜杠**：`-fpreprocessed` 不做「反斜杠换行」拼接，`//` 注释以 `\` 结尾会吞掉下一行代码而检查看不出。
  `git diff -U0 | grep '^+' | grep -E '\\\s*$'` 无输出。
- **`-Wcomment`**（块注释里嵌 `/*`）：改前改后 11 个文件都是 0 警告。

## 与原 workflow 的关系

原会话 [8e5f90] 的 workflow `wf_616406d7-1fa`：5 组改动代理（G1 dsp / G2 ui / G3 guard 测试 / G4 glaze 测试 / G5 controller 测试）→ 每组机械、语义两个复核代理 → 修订 → 复核。
两处 fail（`ClayGuard.cpp:26` 资格期理由仍是现在时、`ClayGuardTests.cpp:1087` 计数量级说错）都已修，复核 pass。终审（critic）被 18:31 桌面端重启打断，
收尾由 [cdaccd] 接手：残留短语复查（`反相保护` 只剩「原反相保护 / ADR-012 的反相保护」这类明写历史的说法；`收单拉到 1` 无；`快路门 0.3` 只剩注明历史值的几处）、
`ClayGuard.cpp` 新注释与它描述的代码（`phaseActive = phaseFloor > 0`、`widthActive = widthCapDb < plannedWidth`）逐行对过。

## 没做

编译与全套：只改注释、去注释后逐 token 相同，编译结果不会变；并入「本轮 PR 都合完之后在 main 上统一跑的那一次」（DGX Spark 本机，对照基线 148335 过 / 5 败）。
