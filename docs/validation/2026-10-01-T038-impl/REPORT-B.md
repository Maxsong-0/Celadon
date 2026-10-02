# T-038 阶段 2 报告 B：House 学习方式

> 2026-10-01 · 实现代理 B（Opus 5.5 · max，workflow `wf_de8d9b0c-06a`）· 分支 `worktree-wf_de8d9b0c-06a-2` · 最终提交 `7a3059deef`。
> 代理环境不允许子代理写报告文件，本文由主进程按代理的结构化返回原样整理（只调格式、不改内容），2026-10-01 集成时落盘。
> 改动文件：Source/dsp/SegmentLedger.{h,cpp}、MusicAnalyzer.{h,cpp}、GlazeController.{h,cpp}、tests/{SegmentLedger,MusicAnalyzer,GlazeController}Tests.cpp。

## 摘要

1. 规格第 2～5、7 节和第 9 节的置位部分已全部实现，代码在 6 个 WIP 提交里，止于 7a3059d，工作树干净。开工时工作树在 88199cb，不是 c50ccd3；前者是后者的祖先，已先快进到 c50ccd3 再做。
2. 段落账（SegmentLedger）：一段 = 一小节，小节起点优先用 ppqLastBarStart；拿不到拍点时每 2 秒一段，有宿主位置就按位置分格。段边界 = 锚点 + round(n × 每小节样本数)，只在速度、拍号、不连续点或拍点偏差超过 50 ms 时重新锚定。重放同一位置就覆盖旧值。容量 512 段，在 prepare 里分配，约 1.8 MB。四路数据都越过段尾才定稿。两道门（S_ref 取第 95 百分位，低频参考同样取第 95 百分位）都已实现，另有「新响段」与定稿事件。
3. 分析器：House 档三路数据按各自起点归段。画像为格式 3、填 houseGate；I、PLR、真峰值、频谱、起音、立体声、次声、先试后用代价只用满编段，LRA、有效时长、代表性用全部段；代表性另取 min(原值, 已听 ÷ 64)。通用档不进任何新代码，原有代码只改了 5 行，都是加 House 分支。
4. 编排器：锁定要三条同时满足；暂定方案每次门控重算后更新，没锁定前不开校准轮。新响段触发时，先把正在走的过渡按进度连续压到 0.5 s 内走完，再接一次 0.5 s 过渡。锁定后连续 4 小节「更响更满」就重开，每份画像最多 2 次、变轻不重开。换曲风、旧格式画像、重新聆听、读档清空段落账都按规格处理；Status 的 house* 字段全部填上，每块调用 updateHouseHint。
5. 109 定向测试全过后跑了一次全套：135018 过 / 0 败 / 0 编译警告。
6. 新增 26 个测试用例：SegmentLedger 11 个，分析器 House 6 个，编排器 House 9 个。
7. 关键数字（合成素材）：「32 小节纯鼓前奏 → drop」上，House 档 I = −12.01 LUFS，与只听 drop 的结果相同；通用档 I = −15.63。House 锁定发生在 23.32 s，12 小节约 23.23 s；drop 后 2.03 s 触发快过渡；两次重开，I 依次为 −24.05 → −18.99 → −13.97 LUFS。
8. 性能（109）：House 档学习中每 512 样本一块平均 24.98 µs，通用档 24.88 µs；满 512 段时一次画像重算 65 µs。
9. 任务书要求的报告文件没写，内容见下面几个字段：会话规则不允许子代理写报告文件。
10. 规格里没写死、由我自己定的细节和遗留问题列在 openIssues，需要改的别人文件列在 crossFileRequests。

## 服务器上实际跑的命令与结果

- 均在 109 上，CELADON_TEST_HOST=maxsong@192.168.100.109。沙箱不认 "$BUILD" 这种运行期变量，改用同一个远端构建目录的字面路径 /home/maxsong/.cache/celadon-build/wf_de8d9b0c-06a-2-dc9e9ba6aff7；每次跑前先 ssh uptime 看负载（0.0～3.4）
- scripts/test-linux.sh bash -c 'cmake -S Celadon -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build <build> --target celadon_tests'（阶段 1 基线，预热构建目录）→ 编译通过
- WIP 1 之后：celadon_tests --only MusicAnalyzer,SegmentLedger,MusicAnalyzerClay → 3568 过 / 0 败，编译 0 警告
- WIP 2 之后：celadon_tests --only GlazeController,MusicAnalyzer,SegmentLedger,GlazeCalibration,LegacyFixture → 6704 过 / 0 败，编译 0 警告
- celadon_tests --only SegmentLedger → 63 过 / 0 败
- celadon_tests --only MusicAnalyzerHouse → 41 过 / 0 败，用时 20.2 s，其中性能护栏的满账本填充约 12 s
- celadon_tests --only GlazeControllerHouse → 58 过 / 0 败，用时 13.9 s
- celadon_tests --only MusicAnalyzerHouse,MusicAnalyzer → 3446 过 / 0 败。通用档学习中每块平均 24.88 µs（改前 24.96～25.03 µs）；House 档 24.98 µs，最坏一块 31 µs；满 512 段一次重算 65 µs
- 收尾跑一次全套 scripts/test-linux.sh（不带参数）→ 135018 过 / 0 败，编译 0 警告（日志里有 7 处 warning: 字样，都是测试名，不是编译警告）
- 没跑：CELADON_CXX_FLAGS=-march=x86-64-v3 全套（没改 ADR-013 名单里的文件，也没改编译选项）；107 全套与 109 Windows（MSVC + Wine）全套，属验收第 7 节，留给主进程集成时做

## 跨文件请求（主进程集成时处理，处理结果见 INTEGRATION.md）

1. GlazeTypes.h：Status 加 bool houseLedgerFull = false;。规格第 2.1 节要求「满了以后不再收新段，状态行写明」；段落账已提供 isStoreFull()。字段加上后，在编排器（我的文件）makeStatusPayload 的 House 段补一行 s.houseLedgerFull = analyzer.getSegmentLedger().isStoreFull(); 即可。状态行文案归 D
2. （可选）ClayGuard（C 的文件）：现在 reset() 会连 House 提示一起清掉，而 GLAZE ≤ 0 时编排器每块都调 resetClayRuntime()。若希望 GLAZE 0 时 House 反相提示也能工作，需让 reset() 不清 House 提示，或另给一个只清反相保护的入口。现在的实现与现有反相指示灯一致：只在 GLAZE > 0 且方案源支持自动 Clay 时提示
3. 规格回写（档案只读，由主进程做）：openIssues 里标「口径」的几条是我对规格未写死部分的取法，需要时写进设计规格-曲风-House学习方式

## 遗留问题与口径

- 工作树基点：本工作树开工时在 88199cb，不是任务书说的 c50ccd3（88199cb 是 c50ccd3 的祖先），已用 git merge --ff-only 快进。同批的 wf_de8d9b0c-06a-1 / -3 / -4 当时也在 88199cb，请主进程核对 A / C / D 是否也快进了
- 口径：低频参考的第 95 百分位也按段的有效时长加权（规格只写了 S_ref 按时长加权）。理由是同一口径，播放从小节中途开始的半小节不与整小节等权
- 口径：一段算「已听」= 段内有效音频 ≥ 标称长度的一半；一个有效子块都没有的段不存进账本（预备拍静音、宿主停着时的静音都不计、也不会把账本塞满）
- 口径：2 秒退路按时间轴每 2 s 切一段（有宿主位置时按位置切），「按有效音频样本计」体现在计数上（已听秒数 = 有效音频秒数），不是攒满 2 s 有效音频再切段
- 口径：新响段可能在 drop 的前两小节各触发一次。第二小节到来时 drop 只占全部时长的约 1/33，第 95 百分位还落在前奏上；两次快过渡接连发生，无害
- 口径：新响段的快过渡会先把正在走的过渡按进度连续压缩（hurryTransition，进度 t 不跳），不按 requestSource 原有的「排队、取较长时长」，否则 drop 要等 3 s 的暂定过渡走完
- 口径：House 画像的 I 用满编段的门块逐个与 BS.1770 相对门比较；通用画像按 0.1 LU 格中心比较，两者差 ≤ 0.05 LU
- 口径：重开时先试后用代价的来源。锁定后候选不跑，新段没有候选块：满编段的块不够 10 s 时，分析器改用全部段里有候选块的段；仍不够时（例如刚读档），编排器沿用已提交画像的代价
- 口径：暂定期的信心打折按任务书做在代表性上，rep = min(原值, 已听 ÷ 64)。confidenceScale 会把代表性除以 0.8 再封顶 1，所以大约听到 51 小节起就不再打折；规格原文是 confidenceScale 乘 min(1, 已听 ÷ 64)
- 口径：重新聆听、锁定后等待重开期间，houseLockPending 也为真
- 反相提示的端到端行为未测：本工作树里 ClayGuard::updateHouseHint 还是阶段 1 的占位（恒不提示，由 C 实现）。编排器这边只验证了接线、不分配。合并 C 之后建议补一例：满编段低频相关度 < 0.6 持续 4 小节 → Status.housePhaseHint 变真
- 最坏块开销：满 512 段时一次门控加画像合并 65 µs（109 实测），约占 192 kHz / 32 样本块预算 167 µs 的 39%。只在锁定后重开（每份画像最多 2 次），或首次聆听拖得很长还没锁定时出现；平常每定稿一段只要几 µs。若要压平，需要把合并摊到多块里做，并加一个「画像就绪」的代次握手
- 内存：每个插件实例在 prepare 时多分配约 1.8 MB 段落账，通用档也分配。原因是曲风参数可以在音频线程上随时切到 House，那时不能再分配
- 现有 10 LU 画像失配保护的过渡仍按 requestSource 排队（照规格保留未改）。House 暂定期的过渡是 3 s，失配那次 0.5 s 的降上限可能排在它后面；drop 处新响段的快过渡会顺带把它加快
- 门控阈值（3 LU、0.6）、锁定条件（64 小节 / 8 小节）、重开条件（3 LU × 4 小节）都还是起点值，待 Max 用曲库标定（规格第 6 节）
- 性能护栏用例（MusicAnalyzerHouse）要把账本喂满，需 1037 s 素材，在 109 上约 12 s，全套测试因此变长
- 报告文件 docs/validation/2026-10-01-T038-impl/REPORT-B.md 没写（会话规则不允许子代理写报告文件），所以最后一个提交里也没有报告。报告内容就是本次返回的各字段
