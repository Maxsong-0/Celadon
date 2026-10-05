# T-071 · v0.1.8.0 界面改版遗留的过期叫法与几处与代码不符的注释（2026-10-04 夜，DGX Spark 本机）

T-060 拍板档（`04-协调/2026-10-04-T-060-反相提示注释清理待Max拍板.md`）第 6 条「v0.1.8.0 界面改版遗留」与「范围外顺带发现」几处——夜间任务不挂卡片，本会话直接做。**只改注释**（`tools/render/README.md` 一处文档），不动代码、不动字符串。

## 改了什么（47 条，脚本 `apply_edits.py`，逐条断言原文恰好出现一次）

| 类 | 内容 |
|---|---|
| 「抽屉」叫法（v0.1.7.0 时 Clay 设置在「高级」抽屉里，v0.1.8.0 起在 CLAY 卡片上） | 三项的默认设置「抽屉默认」统一叫**「三态默认」**（`ClayGlazeTests` ×12、`ChainTests` ×4、`ClayGuardTests`、`HelpTextTests`、`GlazePlannerTests`、`ClayModule.h`、`CeladonChain.h`、`ClayGuard.cpp`、`RenderMain.cpp` ×2、`tools/render/README.md`）；「「高级」抽屉里的设置」→「CLAY 卡片上的设置」（`PluginProcessor.{h,cpp}`、`ChainParams.h`、`CeladonChain.h`、`Parameters.cpp` ×2）；「抽屉的实际值小圆点」→「CLAY 卡片每行的灯和数值」（`GlazeController.cpp`、`GlazeTypes.h`、`ClayGuard.cpp`）。**不动**：带版本说明的（`ParameterIds.h` 两处「v0.1.7.0 在抽屉里，v0.1.8.0 起……」）、引号里引用设计规格原文的（`ClayModule.h:77`、`ClayGlazeTests.cpp:1332`、`ChainTests.cpp:1360`、`RenderMain.cpp:2639`） |
| 色名 / 调色板名已不存在 | `HelpText.h`「实际值非 0 → 梅子青」→「亮（「自动」紫圈、「手动」青色实心，画法见 `ModuleCards.cpp`）」，`HelpTextTests.cpp` 两处同；`GlazeStatusText.h` 颜色类别注释写的 `palette::textDim / accent / reduction` 全不存在——改成实际的用法：琥珀 → 状态胶囊警告样式（`PluginEditor` 的 `toneFor (kind) == amber`）、梅子青那三种（聆听中 / 校准中 / House 聆听中）→ 圆点呼吸（`pill.pulse`）、灰青 = 常规样式 |
| 与代码不符 | `HelpText.cpp`「再接保护状态」→ 实际接的是低频反相提示与宽度保护两段（亮着才写）；`ModuleCards.h`「Clay 实际值与保护状态」同；`HelpText.h` 代价说明模板没跟上 T-043（含低切时写「会抬高峰值或削弱低音」，`clayCostLine` 实现里就是这样）；`ClayGuard.cpp`「比出厂更敏感」只说对一半——门 0 比出厂 0.1 **更不爱触发**，最短持续 0 比出厂 0.15 s 更爱触发；`ClayGlazeTests`「五个代价」实为六个→「各个代价」 |
| 注释放错位置 | `GlazeControllerTests.cpp` 的「Clay 5」那一段说的是 `testClayGuardThroughController`，却压在 `testClayTransitionBlend` 末尾、隔着「Clay 6」「Clay 6b」；挪到它自己的函数上面。第二个「Clay 6. 执行快照」改叫「Clay 7」（编号用了两次） |

## 验证

- **只改注释的证明**：21 个改动文件里 20 个 C++ 文件，去注释后与 main（`6ba86be`）**逐 token 相同**（`g++ -fpreprocessed -dD -E`，复用 T-060 的 `comment_only_check.py`，输出 `comment-only-check.txt`）；预处理器没有任何 stderr；新增行没有行尾反斜杠、没有 `//` 行里夹 `/*`（这两条是去注释比较的盲区，另查）。挪注释块的 `GlazeControllerTests.cpp` 行数不变（4995 / 4995）也相同。
- 在 Spark 上全新构建目录里编 `celadon_tests` + `celadon_render`：83 步、**0 警告**，两个可执行文件的大小与 main 上编出来的相同（8036808 / 4695296 字节；大小相同不算证明，证明靠上一条的逐 token 相同）。
  不跑全套测试——只改注释，目标码不变（同 T-060 / T-057 的做法）。

## 没做

- **字符串字面量里的旧叫法**：`RenderMain.cpp:484` 的 `--help` 行「auto = 抽屉默认」、`GlazePlannerTests.cpp:4449` 的测试消息「手动值与抽屉设置不改变……」——不是注释，改它们会改目标码 / 输出，不属于「只改注释」；留到顺手改字符串时一起（`--help` 行宽已核过，「三态默认」同宽）。
- `MusicAnalyzer.cpp:2494`「保护层」叫法：T-048 的文件，T-048 合 main 之后才能碰。
- T-060 拍板档第 4、5 条（重算 numpy 数字、模块级「实时保护 / Clay 保护」叫法）：按档里建议不做。
