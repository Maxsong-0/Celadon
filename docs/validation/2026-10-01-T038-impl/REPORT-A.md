# T-038 阶段 2 报告 A：决策器与交付

> 2026-10-01 · 实现代理 A（Opus 5.5 · max，workflow `wf_de8d9b0c-06a`）· 分支 `worktree-wf_de8d9b0c-06a-1` · 最终提交 `a1c203f895`。
> 代理环境不允许子代理写报告文件，本文由主进程按代理的结构化返回原样整理（只调格式、不改内容），2026-10-01 集成时落盘。
> 改动文件：Source/dsp/GlazePlanner.{h,cpp}、tests/GlazePlannerTests.cpp。

## 摘要

1. 基线：工作树原本停在 main 的 88199cb，没有阶段 1。我在本分支 ff-only 快进到 c50ccd3 再开发。另外三个代理的工作树（wf_de8d9b0c-06a-2/3/4）也停在 88199cb，请主进程核对。
2. 曲风覆盖：新增三参数 blendConstants（GlazePlanner.cpp:1413）。House 子类在风格混合之后替换 Tone 参考斜率、低切目标、加宽系数；planningContext（:1486）改用它。通用档和越界的曲风值逐位不变。动态预算、偏好响度、toneMaxDb、Heat 底量、自动风格权重都不看曲风。
3. Tone 往亮：新增 toneBrightCapDb（:1431）。House 档满强度 Tone 钳在 [−toneMaxDb, toneMaxBrightDb]（出厂 +0.4），再同样 × GLAZE × κ。通用档返回 toneMaxDb，式子与 main 相同。
4. 交付：新增 preferredLufsFor（:1569，调用处 :1619）。标准档用原式子、原运算顺序。流媒体 / 俱乐部 = 该档目标 + (L_style − styleOffsetReferenceLufs) − f(LRA)，只钳在该档范围。越界的交付值按标准。requested / effective 目标都等于 I + 对应增益，测试逐位核对过。
5. GlazePlanner.h 只加不改：新增上面两个公开声明，让编排器复用同一个往亮上限；PlanInputs 和已有签名都没动。
6. 新测试 testGenreAndDeliveryPlanning（tests/GlazePlannerTests.cpp:2514，挂在 runTest:445），7 个子测试：覆盖值；House 只改 Tone 与 Clay（随机场景）；往亮封顶与坏快照；交付目标表（风格偏移 / LRA / 钳位 / 快照常数）；通用 + 标准不读新常数；新档位下单调、在范围内；不分配内存。
7. 107 结果：--only GlazePlanner 4955 过 / 0 败；全套 135156 过 / 0 败；编译 0 警告。变异检查得到 96 处失败，三类改坏都被抓到，跑完已还原。
8. 需要改编排器：运行中 Tone 跟随在 House 档要按同一个往亮上限封顶，见跨文件请求 1。

## 服务器上实际跑的命令与结果

- 以下命令都以 CELADON_TEST_HOST=maxsong@192.168.100.107 scripts/test-linux.sh 开头。工具拦截含 $BUILD 变量的远端命令，所以构建目录写成字面路径 /home/maxsong/.cache/celadon-build/wf_de8d9b0c-06a-1-81060e557cae（下文写作 <构建目录>）
- 基线（阶段 1 原样）：cmake -S Celadon -B <构建目录> -G Ninja -DCMAKE_BUILD_TYPE=Release，再 cmake --build <构建目录> --target celadon_tests → 配置、编译通过，0 警告
- 本改动增量编译：cmake --build <构建目录> --target celadon_tests → 通过。重编了 GlazePlanner.cpp、GlazeController.cpp、GlazePlannerTests.cpp、GlazeControllerTests.cpp、GlazeTests.cpp、GlazeBehaviourReport.cpp，warning / error 都是 0 条
- 定向：<构建目录>/celadon_tests --only GlazePlanner → 4955 passed, 0 failed。7 个新 T-038 子测试都运行并通过
- 变异检查：临时改坏三处（House 往亮不封顶、不覆盖低切目标、俱乐部再叠 maxPreferredLufs），编译后跑 --only GlazePlanner → 4859 passed, 96 failed。失败落在 4 个子测试：覆盖 37、Tone 54、交付 4、性质扫描 1。之后 git checkout 还原，工作树与提交一致
- 全套：scripts/test-linux.sh 不带参数（配置 + 编译 + 全部测试，重编了还原后的 GlazePlanner.cpp）→ 135156 passed, 0 failed，exit 0，编译 0 警告。日志里有 7 行含 warning，全是 ClayGuard / ClayGlaze 测试名里的「phase warning」字样
- 最终提交 a1c203f 的树对象与测过的 WIP 提交 96ea865 相同（323b1af），只改了提交说明

## 跨文件请求（主进程集成时处理，处理结果见 INTEGRATION.md）

1. 1【需要改】GlazeController.cpp 运行中的 Tone 跟随，House 档要一起封顶。两处现在都用对称的 ±toneMaxDb：updateLiveTone 的 :2256（target = jlimit (−caps.toneMaxDb, caps.toneMaxDb, base + outside × k) − base），computeEffective 的 :2343（tone = jlimit (−cap × strength, cap × strength, …)）。House 档方案 Tone ≤ +0.4，但跟随能把总量推到 +0.75，规格的往亮上限在运行时守不住。建议：:2256 的上界改成 toneBrightCapDb (caps, c, toSlot().source.genre)；:2337 之后加 const auto bright = toneBrightCapDb (blendedCaps(), snapshot, toSlot().source.genre)，:2343 的上界改成 bright * strength。曲风用方案源的（与 toSlot().source.constants、plan.weights 同一来源）。通用档返回 toneMaxDb，逐位不变。active 时 strength > 0，不会出现 inf × 0；上界 ≥ 0 > 下界，jlimit 的顺序成立。GlazeControllerTests 建议补一条：House 档、暗画像、跟随偏移拉满时，实际 Tone ≤ 0.4 × GLAZE × κ。
2. 2【不用改，说明】GlazeController.cpp 保护层 / 上游监视的上限（:1791 releaseManualBudget (blendConstants (…))），以及 computeEffective 里 blendedCaps 给 Tame 的上限：只用动态预算和 tameCap，曲风不覆盖这些字段，两参数版与三参数版逐位相同。想统一口径可以换成三参数版，行为不变。
3. 3【需决定】MusicAnalyzer.cpp :242：先试后用的低切候选是 20 Hz 4 阶高通（注释写「自动低切目标的上限」），House 低切目标是 25 Hz，量到的低切峰值代价会比 25 Hz 真切下去时偏小（偏乐观）。规格写「先试后用和峰值守护照旧」，所以我没提改动；运行时还有 ClayGuard 峰值守护（ADR-014）兜底，ClayGuard / ClayModule 也不把自动低切钳在 20 Hz，25 Hz 能原样流过去。要不要在 House 档按 25 Hz 测代价，请主进程或 Max 定；要改的话属于分析器和画像的范围。
4. 4【文档类，低优先】GlazeConstants.h:61 toneMaxDb 的注释补一句「House 曲风往亮改用 GenreConstants::toneMaxBrightDb（GlazePlanner toneBrightCapDb）」；GlazeTypes.h:229 requestedTargetLufs 的注释「风格与 GLAZE 想要的响度」改成「风格、交付与 GLAZE 想要的响度」。

## 遗留问题与口径

- 报告文件未落盘：代理环境拒绝写报告 .md，内容在本返回各字段里。请主进程写到 docs/validation/2026-10-01-T038-impl/REPORT-A.md 再提交；本分支最后一个提交里没有报告。
- 工作树基线：四个代理工作树创建时都在 main 的 88199cb，不是任务书说的 c50ccd3。我已快进；B / C / D 是否也处理过，请主进程核对，否则它们是在没有阶段 1 的代码上开发。
- 解释口径，请确认：① toneMaxDb ≤ 0 时 House 也不往亮。我保留原来的整项开关 toneMaxDb > 0，理由是按 GlazeConstants.h 的约定，toneMaxDb ≤ 0 = 自动不许动 Tone 这一项，曲风只换往亮的边界，不重开风格关掉的 Tone。② toneMaxBrightDb 大于 toneMaxDb 时照用：任务书写「改用」，我理解为替换，不取两者较小。③ toneMaxBrightDb ≤ 0 或 NaN = 不许往亮，+∞ = 不设上限（最后还有参数范围 ±3 dB）。④ 越界的曲风 / 交付枚举值按通用 / 标准。四条都有测试；出厂值下都不影响结果。
- GlazePlanner.h 在我的文件清单里，但硬约束第 1 条又把它列为共享头文件，两处有冲突。我按「只加不改」处理：新增三参数 blendConstants 与 toneBrightCapDb 两个声明。主进程如果不要新接口，可以把它们改成 GlazePlanner.cpp 里的内部函数，编排器直接用 genreConstantsFor (c, genre)->toneMaxBrightDb。
- 【需 Max 定】非标准交付下最小推响量照样生效：满强度请求 = max(minimumFullDriveDb 3 dB, L_pref − I)。按代码推算，输入已经比该档目标响时请求仍是 +3 dB，例如流媒体档、I −9 时请求目标是 −6，越过该档上限 −8；有效增益还要受风格动态预算约束，实际推多少未实测。标准档本来就这样（已够响仍可温和推响）。任务书只要求改 preferred，我没动这条。
- House 暂定方案的置信度系数没做：学习方式规格第 3.2 节要求锁定前「置信度再乘 min(1, 已听小节 ÷ 64)，乘在现有的 confidenceScale 上」。confidenceScale 在 GlazePlanner.cpp，但这条不在本任务书里，请主进程分派。如果放进决策器，建议在 confidenceScale 里按 profile->houseGate 乘：按小节分段时用 heardSegments ÷ lockSegments，分 2 秒段时用 heardSeconds ÷ fallbackLockSeconds，只在 valid 时乘。这样方案、校准额度、运行中的 Tone / Tame 用同一个 κ；通用档画像 houseGate.valid = false，结果逐位不变。
- 风格偏移的参考 styleOffsetReferenceLufs（−9）与冲击的 preferredLufs（−9）是两个独立常数。标定时只改了冲击的偏好，冲击的偏移就不再是 0，两个数要一起改。
- 规格第 7 节验收里的 House 门槛（真歌）、切换交付后立即导出这两项，都不在决策器单元测试范围内，本代理没做。
- 107 上我这棵工作树的构建缓存还留着（~/.cache/celadon-build/wf_de8d9b0c-06a-1-81060e557cae，同步目录 ~/.cache/celadon-tests/wf_de8d9b0c-06a-1-81060e557cae），复验可以直接用，不需要时可删。本机 scratchpad/A 已删。
