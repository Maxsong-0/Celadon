# T-038 阶段 2 报告 D：工具与回归

> 2026-10-01 · 实现代理 D（Opus 5.5 · max，workflow `wf_de8d9b0c-06a`）· 分支 `worktree-wf_de8d9b0c-06a-4` · 最终提交 `07d4d6464d`。
> 代理环境不允许子代理写报告文件，本文由主进程按代理的结构化返回原样整理（只调格式、不改内容），2026-10-01 集成时落盘。
> 改动文件：tools/render/{RenderMain.cpp,README.md,house_gate.py,make_synthetic_house.py}、tests/LegacyFixtureTests.cpp。

## 摘要

1. 分到的工作树起点是 main 88199cb，不是 c50ccd3。我只把自己的分支快进到 c50ccd3（git merge --ff-only）。同批另外三个工作树当时也在 88199cb，请主进程核对。
2. celadon_render 新增：--genre / --delivery（网格的两维）；--bpm / --meter 模拟宿主走带（不给就与改动前逐位相同）；--segments-csv 逐段统计，含段响度、段末短期响度 S、低频占比（段 0～4 ÷ 27 段，与 lowShare 同口径）、逐段限幅 GR。另外为门槛 ① 加了 --listen-from-bar。
3. 只有用了新选项，metrics.csv 才在末尾追加 33 列。--selftest 原样不动，新选项的自检单独放在 --selftest-house（13 项）。
4. 新脚本 make_synthetic_house.py（House / Tech 两首合成曲 + 段落标注）和 house_gate.py（五条门槛 + 门控对照，只用标准库）。LegacyFixture 加了 2 段测试，含阳性对照。
5. 109 结果：编译 0 警告；--selftest、--selftest-house 全部通过；--only LegacyFixture 28 过 / 0 败；全套 134874 过 / 0 败。
6. 逐位回归（22 首门槛歌 × 8 组设置，main 88199cb 对比本工作树）：176 个 wav 逐字节相同，sha256 清单一致，metrics.csv 与 metrics.md 都是 22/22 相同，没有任何差异样本。另外给 --bpm 124（通用档）跑一遍，176 行 audio_hash 与 main 全部相同。
7. 合成曲上试跑门槛脚本（B、C 都还没合入，结果只证明脚本能跑通）：① House 差 −2.19 dB、Tech 差 −0.73 dB，判 FAIL，这正是 T-038 要修的问题；③④ 全部 PASS；⑤ 俱乐部档 FAIL，原因是交付目标还没进决策器。
8. 只改了清单里的 5 个文件。服务器上的临时目录 ~/.cache/celadon-t038-D 和本机 scratchpad/D 都已删除。
9. 你说的「顺手清理 T-032 和 T-018」不在 D 能改的文件范围内，我没有处理，留给主进程。

## 服务器上实际跑的命令与结果

- 渲染工具编译：CELADON_TEST_HOST=maxsong@192.168.100.109 scripts/test-linux.sh cmake --build /home/maxsong/.cache/celadon-build/wf_de8d9b0c-06a-4-1732505adef7 --target celadon_render → 0 警告（-Wall -Wextra -Wshadow -Wconversion，GCC 15.2 Release）
- 原有自检：scripts/test-linux.sh <B>/celadon_render --selftest → 17 项全部通过，数值与改动前相同（例：同决策重放 1911 块逐位相同）
- 新自检：scripts/test-linux.sh <B>/celadon_render --selftest-house → 13 项全部通过。PPQ 4 / 12.5 精确；124 BPM 下 600 个段边界 0 个归错；段响度 −19.993 LUFS；段末 S −21.754 / −19.993；60 Hz + 1 kHz 低频占比 0.4962；通用档给拍点成品哈希不变；跟拍上限 161.290 ms；流媒体有效 Ceiling −2.00、真峰值 −2.350 dBTP；从第 3 小节起播，追踪首行 4.0000 s
- 测试编译：scripts/test-linux.sh cmake --build <B> --target celadon_tests → 0 警告
- scripts/test-linux.sh <B>/celadon_tests --only LegacyFixture → 28 passed, 0 failed
- 全套（收尾）：CELADON_TEST_HOST=maxsong@192.168.100.109 scripts/test-linux.sh → 134874 passed, 0 failed，0 编译警告（阶段 1 在 107 是 134859，多出的 15 个是 LegacyFixture 新增检查）
- main 构建：git archive 88199cb 经 ssh 解到 ~/.cache/celadon-t038-D/main，JUCE 用同一份，cmake Release 编 celadon_render → 0 警告，--selftest 全部通过
- 逐位回归：两份构建各渲染 22 首 T-022 门槛歌（songs-9a355b99857c，每首一个进程，12 路并行），参数 --glaze 0,35,70 --style auto（修坯 auto/off、带同决策重放、写 wav），20:41:49～20:44:27 → 成品 wav 176 对，逐位相同 176 对；sha256 清单 176 个一致；metrics.csv 22/22 逐字节相同（198 行）；metrics.md 去掉生成时间后 22/22 相同；第一个差异样本：无；重放阳性对照 44 组全部 ok
- 附加核对：工作树版加 --bpm 124 --metrics-only 跑同样 22 首（通用 + 标准），20:45:09～20:46:49 → 176/176 行 audio_hash 与 main（不给拍点）相同
- 合成曲：~/.venvs/audio/bin/python tools/render/make_synthetic_house.py → synthetic-house-124 / synthetic-tech-124，各 217.27 s、112 小节、峰值 −3 dBFS，附标注 JSON
- 合成曲渲染（4 路）：celadon_render --glaze 100 --clay auto --genre deep|tech --delivery standard,streaming,club --bpm 124 --listen-from-bar 1|33 --segments-csv --metrics-only --no-replay → 全部 EXIT 0
- 门槛脚本：python3 tools/render/house_gate.py <4 个输出目录> --annotations <synth> → 12 行，16 项 FAIL（B/C 未合入，详见 openIssues）。门控对照：House 前奏 0/32 过门、drop 64/64、breakdown 0/16；Tech 前奏 32/32 过门
- 不给 --bpm 的 --segments-csv：按 2 秒分段，217.27 s 分成 109 段；house_gate 提示「不是按小节分的」并跳过

## 跨文件请求（主进程集成时处理，处理结果见 INTEGRATION.md）

1. tools/render/gate_check.py（不是我的文件）：配对键目前是（歌, GLAZE, 风格），建议在有这些列时加上 genre / delivery / listen_from_bar。否则多值曲风 / 交付 / 起播网格的指标表不能直接喂给它。低优先级，README 已写明修坯门槛照旧用默认运行判
2. Source/dsp/MusicAnalyzer.cpp 匿名命名空间里的几个常数，在 RenderMain.cpp 里照抄了一份并注明出处：lowShareLastBand = 4、frameAbsoluteGatePower = 1e-7、frameRelativeGateRatio = 0.01、absoluteGateLufs = −70、effectiveRelativeGateLu = −20、shortTermSubBlocks = 30。建议挪到公开头文件（MusicAnalyzer.h 或 GlazeConstants.h），工具改成引用，免得两边漂移
3. 给 B（SegmentLedger）：有两点请与工具对齐或告诉我改成哪种。① 段响度的有效音频门参考：工具用整段音频的 I，插件用学习区间的 I。② Balance 帧归段：工具按分析窗起点 endSample − 窗长。B 合入后，建议在两首合成曲上拿 segments/*__input.csv 与插件自己的逐段门控结果逐小节对照一次
4. scripts/render-linux.sh（不是我的文件）：渲染前只跑 --selftest。带 T-038 新选项渲染时可以顺带跑 --selftest-house。低优先级

## 遗留问题与口径

- 工作树起点：A/B/C 三个工作树（wf_de8d9b0c-06a-1..3）在我检查时都停在 main 88199cb，看不到阶段 1。请主进程确认它们是否基于 c50ccd3
- 报告文件写不了：REPORT-D.md 被工具拒绝（子代理不许写报告 .md）。主进程若要归档报告，可以用这次的返回内容或提交 07d4d64 的说明
- 你说的「顺手清理 T-032 和 T-018」：D 的任务书限定只改 5 个工具 / 测试文件、不碰档案库和别的分支，所以我没处理，留给主进程
- 偏离任务书 1：加了 --listen-from-bar（任务书没列），门槛 ① 要「从 drop 播」才算得出来。默认 1 = 从头，不影响现有输出
- 偏离任务书 2：新列只在用了新选项时才追加；新自检单独放在 --selftest-house。这样「现有输出与 --selftest 保持不变」字面成立：默认运行的 metrics.csv 与 main 逐字节相同
- 逐段统计与插件口径的差别（已写进 README）：① 有效音频门和低频帧门的相对参考用整段音频，插件用学习区间；② 不给 --bpm 时 2 秒段按样本时间切，插件按有效音频样本推进；③ Balance 帧按分析窗起点归段
- 门槛口径：② 用块内最大 GR，比保护层按 10 ms 时隙的均值偏严，P95 按「高于上限的块 ≤ 5 %」判；③ 和 ⑤ 用段落响度（段内有效子块的 K 加权能量平均）；⑤ 标准档 N/A，GLAZE < 100 时按 drop 输入 + GLAZE × (目标 − drop 输入) 折算；规格常数（−10 / −7、范围、风格 preferred、LRA 6 LU · 1/3）独立写在脚本里
- 合成曲门槛结果是阶段 1 代码上的：① 标准档 House −2.19 dB、Tech −0.73 dB，流媒体档 −1.90 / −1.16 dB，都 FAIL，这正是前奏被当成整首的问题；② 从头播的几行 FAIL，原因是方案偏温润、预算更紧（均值 0.95 / P95 2.34 / 最大 3.68 dB），实测最大 4.13 dB、超 P95 上限的块 6.2 %；⑤ 俱乐部 FAIL，因为方案目标还是 −8.94 而规格要 −6.94。B/C 合入后要在合成曲上重跑；真正的门槛要等 Max 的 10～20 首 House 和标注
- 合成曲第一版作废：底鼓衰减太长，前奏与 drop 段响度只差约 1 LU、低频占比也一样，门控分不开。现用第二版：House 前奏 −21.07 / drop −17.24 / breakdown −25.59 LUFS
- 范围外观察，未处理：合成 House 曲的 breakdown（只有加宽的 pad，低频几乎没内容）里，修坯反相保护指示亮了 14.2 % 的时长，正好等于 breakdown 占的 16/112 小节。很可能是低频段几乎空时慢路相关度不可靠导致误亮，要不要查请主进程定
- 小节边界取整：段表用 round(k × 每小节样本数)，与规格 2.1 节一致；宿主口径的 floor(PPQ) 在向下取整的边界样本上会差 1 个样本（例：124 BPM 第 92903 样本，段表算第 2 小节，PPQ 3.99999 仍在第 1 小节）。对统计没有影响，B 实现时知道即可
- 109 上保留了本工作树的标准测试缓存：~/.cache/celadon-tests/wf_de8d9b0c-06a-4-1732505adef7 和 ~/.cache/celadon-build/wf_de8d9b0c-06a-4-1732505adef7（test-linux.sh 的常规目录，主进程复跑可直接用）。回归用的临时目录 ~/.cache/celadon-t038-D 已删
