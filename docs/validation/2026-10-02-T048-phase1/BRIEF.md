# T-048 阶段 1 共享背景（主进程 2026-10-02 写；四个代理共用，只读这份与各自点名的源码片段）

## 0. 这是什么

Celadon（JUCE 9.0.2 VST3 / AU 母带插件，C++）要把 GLAZE 从「学习 → 锁定 → 校准」改成全程实时（ADR-030）。阶段 1 只加**实时画像**与工具，
**插件声音不变**：新代码在插件里默认关着，只在测试和渲染工具里打开。阶段 2 才让编排器用它。

- 已批的规格（Max 的 Obsidian 档案，需要时按节读，不要整份读）：`/Users/maxsong/Documents/Max's Secret Place/项目/Celadon/01-框架/`
  `设计规格-实时GLAZE.md`（入口）、`设计规格-实时GLAZE-主段与增益.md`（本阶段的核心：第 1～3、6、7 节）。
- 分支 `claude/t048-realtime-glaze`，基于 main `eb35cfc`；开工基线：107 全套 148582 / 0（`eb35cfc`）。
- 脚手架提交（主进程写，接口冻结）：`Source/dsp/LiveProfile.h`（实时画像的公开接口）、`Source/dsp/SegmentLedger.h` 新增的
  `SegmentSink` 接口与 `setSink`、`Segment` 新增的 `liveTrialBlocks / liveTrialTop / liveBlendIntended / liveBlendOutput`、
  `Source/dsp/GlazeConstants.h` 的 `LiveModelConstants` / `LiveConstants`；占位文件 `LiveProfile.cpp`、`ClayTrials.{h,cpp}`、
  `tests/{LiveProfile,ClayTrials}Tests.cpp`（已登记进 `CMakeLists.txt` 与 `Celadon.jucer`，不用再登记）。

## 1. 所有代理必须遵守（硬约束）

1. 说中文：报告、注释、提交说明一律中文；代码标识符英文。代码风格照周围的代码（中文注释、写「为什么」、一个函数做一件事、常量集中放）。
2. **本机（Mac）不跑任何编译、测试、构建**（cmake / ninja / make / clang / xcodebuild / pytest 都不行）。编译与测试只在服务器上跑：
   - 107：`scripts/test-linux.sh`；109：`CELADON_TEST_HOST=maxsong@192.168.100.109 scripts/test-linux.sh`。
   - 带命令时：把远端命令写进**工作树根目录下的临时脚本**（例如 `agent-a-check.sh`，不提交、用完删），用 `scripts/test-linux.sh bash Celadon/agent-a-check.sh` 跑。
     脚本里构建目录是 `$BUILD`：`cmake --build "$BUILD" --target celadon_tests`，`"$BUILD/celadon_tests" --only 类名,类名`（类名 = UnitTest 构造时的名字）。
     渲染工具目标 `celadon_render`（默认不编，要 `--target celadon_render`）。直接把带 `$BUILD` 的命令写在 `bash -c` 里会被会话隔离检查拒绝，所以用脚本文件。
   - ssh / rsync 报 `Operation not permitted` 是本机沙箱：同一条命令开 `dangerouslyDisableSandbox` 重试。
   - 只跑和你改动相关的定向测试；全量测试、三套编译（GCC / x86-64-v3 / MSVC）与 CI 由主进程最后统一跑。
3. 只改「你的文件」清单里的文件。要动清单外的文件：不动，写进返回结果的 `cross_file_requests`。
4. 提交：只 `git add` 你改的文件（禁止 `git add -A` / `git add .`），在你自己的工作树分支上提交，说明用中文、结尾加一行
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`；不推送、不合并、不切分支、不碰 `git stash`。可以分几次提交。
5. 完成标准、模块边界、出错时谁负责这类问题不自己定，写进 `pending_user_decisions`；不问用户问题；不写单词表、不写夜间日志。
6. 实时与逐位约束：音频线程 0 分配、0 锁；结果与宿主怎么切块无关；浮点不用 fast-math、不依赖 FMA（ADR-013，CMake 已给 GCC `-ffp-contract=off`）；
   「不知道」用 `valid = false`，不用 0 冒充（`GlazeTypes.h` 约定）。
7. 报告写到 `docs/validation/2026-10-02-T048-phase1/REPORT-<你的字母>.md` 并提交；Write 被拦写不了，就把全文放进返回结果的 `report_markdown`。

## 2. 关键代码地图（只读你需要的片段）

- `Source/dsp/SegmentLedger.h`（全文 ~330 行，头注释讲清了分段、记账、定稿、门控）：`Segment` 结构是每段的全部累计量。
  分析器是唯一使用者；House 档时由分析器往开着的段里累加（`openSegmentAt`），三路数据越过段尾后 `finalizeBefore` 定稿（`finalizeOldest` → `deriveSegment` → `commit` → `recomputeGate`）。
- `Source/dsp/MusicAnalyzer.cpp`（4036 行，单个 `Impl`，按「一～六」分节）：
  - 帮助函数 346～415（`energyToLufs`、`histogramBinOf`、`binCentreLufs`、`setFeature`、`sideToMidDbOf`、`correlationOf`）；
  - 先试候选（「五」节）：成员 576～635；`restartTrial` 976、`runOnsets` 1341（`trialRunning = learning && ...` 在 1348）、`runTrial` 1633、
    `snapTrialStates` 1867、`noteTrialGroup` 1892、`countTrialWrite` 1927、`updateTrialProfile` 2870、`percentileOfTop` 2912；
  - House 档（「六」节）：成员 650～703；`clearHouse` 3016、`houseEndEpoch` 3033、`houseCompleteBefore` 3047（3063 是试算交齐的 `learning && ...` 判定）、`houseAdvance` 3074、
    `noteHouseSubBlock` 3098、`noteHouseMusicFrame` 3130、`noteHouseGroup` 3183、`noteHouseEvent` 3196、`noteHouseTrial` 3211、
    `clearHouseSums` 3236、`mergeHouseTrial` 3268、`computeHouseProfile` 3323、`houseLoudnessProfile` 3459、`houseSpectrumProfile` 3541、
    `houseStereoGroup` 3718、`houseOnsetProfile` 3745、`houseTrialProfile` 3786、`houseSubProfile` 3844、`houseConfidences` 3858。
    **这一族函数就是「一批段 → 一份画像」的现行口径**，实时画像要按同一口径加权、加遗忘地算。
- `Source/dsp/GlazeTypes.h`：`MusicProfile`、`Feature`、`HouseGateSummary`、`SegmentMode`。
- `Source/dsp/GlazePlanner.h` 头注释：决策器读画像的哪些量（`confidenceScale` = 输入响度、真峰值、代表性三者里最没把握的那个）。
- 测试工具：`tests/TestHelpers.h`（`allocationCountOnThisThread()` 前后各读一次 = 没经 operator new 分配）；
  `tests/MusicAnalyzerTests.cpp` 3302 `makeHouseTrack (sampleRate, introBars, dropBars, extraSeconds, seed)` 合成 House 曲（3388 起有用法）。

## 3. 实时画像的规则（规格「主段与增益」第 1～3 节的摘录，数值在 `LiveConstants`）

- **段 = 记账单位**：有拍点一小节、否则 2 s（沿用 SegmentLedger）。段定稿时由 `SegmentSink::segmentFinalized` 交给 `LiveProfile`。
- **主段度 m**：ramp_w(x) = clamp(0.5 + x / w, 0, 1)。4/4 型：m = ramp_{loudnessRampLu}(L_seg − (S_ref − ΔG)) × ramp_{lowRampWidth}(lowShare ÷ lowRef − lowGateFactor)；
  响度型：m = 响度因子；被动型：有效音频 m = 1。S_ref = 全部段段响度（按有效时长加权）的 `referencePercentile` 分位；lowRef = 过响度门（响度因子 ≥ 0.5）
  各段 lowShare 的同分位。两者用 0.1 LU / 0.001 一格的直方图（同 SegmentLedger），按有效音频时长遗忘（τ = `referenceMemorySeconds`）。
  还没有参考时（冷启动第一段）的口径由实现定，写进报告。
- **并入**：w = m × 段有效时长；先把主段累计 × exp(−w ÷ W_mem)，再加这一段 × m（「按进账遗忘」：breakdown 里 w ≈ 0，分位数与置信度都不动）。
  W_mem：按小节分段 = `memoryBars` × 小节秒数（段的标称长度），否则 `memorySeconds`。
- **I（喂给决策器的输入响度）**：4/4 型 = 主段加权的 BS.1770 门控积分（门块用段里的 `gatingPower`，−70 绝对门、−10 LU 相对门，同 House 画像）；
  响度型 / 被动型 = 全部段（权重 = 有效时长，不看 m）的同一积分，遗忘按有效音频时长 τ = W_mem。
- **LRA**：全部段的 `rangeBin`（3 s 短期响度）直方图，−20 LU 相对门，P95 − P10；按有效音频遗忘 τ = `referenceMemorySeconds`；有效 < 60 s 时 confidence 按观测充分度打折。
- **其余特征**（PLR、真峰值、27 段长期电平与 Tame / Recover 证据、斜率、低 / 高频占比、低频起伏、起音、立体声、次声、试算代价与混合损失）：主段加权、按进账遗忘，
  口径逐项同 House 画像那一族函数。**试算只读 `liveTrial*` / `liveBlend*` 四项**（不是学习期专用的 `trial*` / `blend*`）。
- **置信度** c = 1 − exp(−M ÷ M0)，M = 衰减后的主段质量（秒），M0 同 W_mem 的换算（`confidenceBars` / `confidenceSeconds`）；各特征 confidence = min(c, 观测充分度)，
  `representativeness` = c，`effectiveSeconds` = M。
- **长时间无主段**：连续 `noMainSeconds` 有效音频没有 m ≥ `mainThreshold` 的段 → 主段累计改按有效音频时长衰减（τ = `noMainDecaySeconds`），直到再出现主段。
- **换歌软遗忘**：段响度与 I 相差超过 `newProgramLu` 的段累计满 `newProgramSeconds` 有效音频（中间夹一段不超的就清零重计）→ 全部累计质量 × `newProgramKeep`。不看宿主走带。

## 4. 分工与文件归属

| 代理 | 做什么 | 你的文件 | 服务器 |
|---|---|---|---|
| A 实时画像 | 实现 `LiveProfile`（按第 3 节）、状态 XML 往返、提交记录环形缓冲；测试 | `Source/dsp/LiveProfile.cpp`、`tests/LiveProfileTests.cpp`；`LiveProfile.h` 只许加私有成员 / 注释（公开接口要改走 `cross_file_requests`）；`GlazeConstants.h` 的 `LiveConstants` 只许改默认值或加字段（写明理由） | 107 |
| B 分析器实时模式 | 先试候选抽成 `ClayTrials`（逐位不变）；分析器实时模式（账本所有曲风都跑、候选一直跑并填 `liveTrial*`）；SegmentLedger 定稿时调 sink；编排器 / 链的直通接口；测试 | `Source/dsp/ClayTrials.{h,cpp}`、`Source/dsp/MusicAnalyzer.{h,cpp}`、`Source/dsp/SegmentLedger.cpp`、`Source/dsp/GlazeController.{h,cpp}`（只加直通）、`Source/dsp/CeladonChain.{h,cpp}`（只加直通）、`tests/ClayTrialsTests.cpp`、`tests/MusicAnalyzerTests.cpp`（只加用例） | 109 |
| C 渲染工具（A、B 合并之后） | `celadon_render --live-trace` 与常数覆盖；因果重放的批跑与分析（复用 `calibrate_house.py`） | `tools/render/*` | 109 |
| D 评审（C 之后） | 对抗评审阶段 1 全部改动 | 只读（可在服务器跑定向测试求证） | 107 |
