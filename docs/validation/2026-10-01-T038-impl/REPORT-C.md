# T-038 阶段 2 报告 C：限幅跟拍、反相提示、界面

> 2026-10-01 · 实现代理 C（Opus 5.5 · max，workflow `wf_de8d9b0c-06a`）· 分支 `worktree-wf_de8d9b0c-06a-3` · 最终提交 `396b25b308`。
> 代理环境不允许子代理写报告文件，本文由主进程按代理的结构化返回原样整理（只调格式、不改内容），2026-10-01 集成时落盘。
> 改动文件：Source/dsp/LimiterModule.cpp、ClayGuard.cpp、Source/ui/{GlazeStatusText,ModuleCards,Controls,HelpText,Localisation}.*、Source/PluginEditor.*、tools/ui-snapshot/*、5 个测试文件；截图 screens-C/。

## 摘要

1. 限幅跟拍（LimiterModule.cpp）：Auto 释放的慢段取 min(自适应值, max(100, beatReleaseCapMs))。上限 ≤0、非有限或 ≥800 ms 时走原表达式；LimiterModule 原有 16 条输出指纹改前改后逐行相同。手动 Release、低频周期下限、短过载都不受影响。中途开关上限，逐样本看增益没有台阶；上限值本身没做时间平滑（头文件不在 C 的清单里）。
2. House 反相提示（ClayGuard::updateHouseHint）：eligible 且慢路 ρ<0.6 连续满 requiredSeconds 才亮，与块长无关。条件消失后保持 2 s（复用反相保护的常数）；不 eligible 立即熄灭并清零。损失按 10·log10((1+ρ)/2) 算，亮起后做 1 s 平滑。只提示，不进 ClayGuardState。
3. 界面：主旋钮左右各加一个选择：「曲风」「交付」（Segmented，新加灰字说明格）。状态行加 House 三种文字（照规格原文）和琥珀色反相提示。暂定期间悬停气泡和左下角写「从 drop 开始播放能学得更快」。CLAY 卡片提示亮时显示两行琥珀字。HelpText 两个参数文案定稿，Localisation 加 10 个键；houseActive=false 时与通用档逐字相同。
4. 截图工具：加 14 种 House 状态（状态字段是注入的）和局部截图，交互检查补了曲风、交付两个选择。screens-C/ 下 14 张图，含默认位置和备选位置，给 Max 比较。
5. 107 结果：全套 145448 过 / 0 败；x86-64-v3 全套 145448 过 / 0 败、0 编译警告；clean 后整份重编 0 警告；截图检查 85 过 / 0 败。新测试类名 HelpTextHouse，--only 时要带上。
6. REPORT-C.md 被 harness 拦下没写成；内容在最后提交的说明里，偏离规格的地方见 openIssues。

## 服务器上实际跑的命令与结果

- 107 改前基线（scripts/test-linux.sh 调远端脚本，celadon_tests --only LimiterModule,ClayGuard,GlazeStatusText,HelpText,Localisation）：8797 过 / 0 败，记下 16 条输出指纹
- 107 中间一轮 --only LimiterModule,ClayGuard：2564 过 / 2 败。两处都是测试写错：跟拍步长界把直流阶跃过冲后快段放回的那 7 ms 也算进去了；分配用例里断言文字的 juce::String 被计进了分配数。两处已修
- 107 改后 --only LimiterModule,ClayGuard：2567 过 / 0 败；原有 16 条输出指纹与改前逐行相同（diff 里只多了新用例的 1 条）。日志：124 BPM 上限持续限幅后 300 ms 处 −1.41 dB，不封顶 −4.79 dB；切换附近每样本增益增量 6.6e-5，界 2.1e-4
- 107 --only GlazeStatusText,HelpText,HelpTextClay,HelpTextHouse,Localisation：16936 过 / 0 败（那次构建有 1 条 -Wshadow，已改名）
- 107 xvfb-run celadon_ui_snapshot --only check：85 PASS / 0 FAIL；clean 重编后再跑一次，还是 85 / 0
- 107 xvfb-run celadon_ui_snapshot 出 House 截图（1 倍 8 种 + 状态行 2 倍 6 种）：全部写出，已拉回 screens-C/
- 107 全套 scripts/test-linux.sh（默认编译选项）：celadon_tests 145448 过 / 0 败
- 107 全套 CELADON_CXX_FLAGS=-march=x86-64-v3 scripts/test-linux.sh（ADR-013，全新构建目录）：145448 过 / 0 败，构建 0 警告
- 107 默认构建目录 cmake --build --target clean 后整份重编 celadon_tests + celadon_ui_snapshot：0 警告

## 跨文件请求（主进程集成时处理，处理结果见 INTEGRATION.md）

1. Source/dsp/ClayGuard.h：houseHintPendingSeconds 的注释改成「没亮时 = 条件已连续成立的有效音频秒数（资格期）；亮着时 = 剩余保持秒数」。HouseHint::monoLossDb 注释补上「亮起时按公式算，之后 1 s 一阶平滑，保持期不更新，ρ 钳到 [−0.99, 1]」。updateHouseHint 的说明补上「条件消失后先保持 clayGuardPhaseHoldSeconds（出厂 2 s）再熄灭」
2. （可选）Source/dsp/LimiterModule.h：如果要上限值本身渐变，加一个 double 状态：每 32 样本按 500 ms 一阶走向「有效上限或 800」，到 800 就回原表达式。不加也满足「无增益台阶」，BPM 连续变化时上限本来就连续
3. 给 B / 集成（GlazeController 等，不是 C 的文件）：每块调一次 clayGuard.updateHouseHint (view, seconds, eligible, requiredSeconds)。eligible = House 档且此刻在满编段；requiredSeconds = 4 小节的秒数，拿不到拍点时用 house.phaseHintFallbackSeconds。再把 getHouseHint() 写进 Status::housePhaseHint / housePhaseMonoLossDb。界面读 houseActive / houseLockPending / houseSegmentMode / houseHeardSegments / houseTargetSegments / houseFullSegments / houseHeardSeconds / houseTargetSeconds；如果未学习时也置 houseLockPending，播放前就会显示「从 drop 开始播放」
4. 档案（C 不能改）：V4 界面规格第 3 节布局表补上两个选择的位置（等 Max 看截图定）。设计规格-曲风-House学习方式：第 9 节补「保持 2 s、损失 1 s 平滑」；第 7 节确认左下角也写提示，以及按秒模式的文字

## 遗留问题与口径

- REPORT-C.md 没写成文件：Write 被 harness 拦下（子代理须以结构化结果返回），我没有绕过。完整内容在最后提交 396b25b 的说明和本次返回里；如果要落盘，请主进程代写
- 两个选择的最终位置等 Max 定。默认放在主旋钮左右：screens-C/zh-house-layout.png、en-house-layout.png。备选放在 GLAZE 标题行左右：zh-house-layout-alt.png，只在截图工具里挪了位置。规格写的是「风格旁边」，但底排放不下：三个分段选择器加原有按钮约 1080 px，会压到左下说明和右下图例
- 偏离规格，请确认：① 限幅上限值本身没做时间平滑，只保证增益无台阶；② 反相提示加了 2 s 保持和 1 s 损失平滑；③ 状态行里，提示压住 House 进度和已锁定，但压不过手动设置、方案受限、画像失配（规格没写优先级）；④ 按秒模式照规格原文，不写主段落进度；⑤ 暂定期间左下第二行也写「从 drop 开始播放能学得更快」；⑥ CLAY 卡片提示亮时，小图让出位置；⑦ 选择器上的 House 子类写作 Deep / Tech / Melodic，全名在悬停气泡里
- 截图里的 House 状态是注入的（B 还没合入），集成后要按真实流程再截一次。截图字体是 107 上的 Noto，Mac 上的效果还要看
- 用户原话里的「顺手清理 T-032 和 T-018」不在 C 的任务书和文件清单里，C 没有动。C 的文件里没有 T-018 的遗留代码，T-032 只有历史注释。请主进程处理
- 109（真 MSVC + Wine）没跑（任务书只要 107）；Mac 编译和 DAW 实操都没做
- screens-C/ 下共 14 张，约 2.3 MB：zh-house-layout、en-house-layout、zh-house-layout-alt、zh-house-hint（整张）、en-house-hint-pill、en-house-hint-clay、zh-house-tip、en-house-tip、zh/en-house-status-bars、zh/en-house-status-main、zh/en-house-status-seconds（最后 6 张是胶囊 2 倍局部）
