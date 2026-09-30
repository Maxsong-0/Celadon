# V10 · 对 R10（跨模块整体设计）九条结论的反驳核验

- 任务：T-032 阶段 B 核验代理 V10（Fable 5.1 · max），2026-09-29 夜。对象：`docs/validation/2026-09-29-T032-deep-research/R10-design.md`。
- 方法：逐条在工作树 `.claude/worktrees/fable-deep-research-634a5e`（main `e6b445b`）里重新找同样的证据（文件:行号都是本次自己核过的），
  再对档案（设计规格-总体 / -Limit / -Color / -高级母带控制 / -GLAZE-时间线与保存、ADR-002 / 004 / 013 / 016 / 020、TASKS）与
  `docs/validation/` 里的 T-014 / T-021 / T-022 报告核。只读；本机没编译没跑测试；没在 107 做数值实验（每条都能靠读码定案）。
- 判决口径：**confirmed** = 缺陷真实、不是规格已定的取舍、未修，且我自己在代码里找到了同样的证据；**refuted** = 不是缺陷（误读 / 规格已定 / 已修）；
  **uncertain** = 事实成立但是否算缺陷、或修法收益要靠 Max 拍板或实测。

## 0. 总表

| 编号 | 研究判定 | 核验判决 | 严重度（调后） | 优先级（调后） | 修法可用 | 一句话 |
|---|---|---|---|---|---|---|
| R10-01 | medium · P0 | **confirmed** | medium | P0（仅当 T-032 今晚要重跑门槛；否则 P1） | 是，需补两处 | 工具默认缺 mastering 八项属实；`effective.mastering` 是原始值属实；**漏了编码余量会被重放减两次**（同根因） |
| R10-02 | medium · P1 | **confirmed**（事实）但降级 | low | P2 | 是，只做前半 | 没有格式号属实；但「参数只加不删不改语义」「DSP 升级不在兼容承诺内」是规格已定，GLAZE 节点已有版本号，不是缺陷级 |
| R10-03 | medium · P1 | **confirmed** | medium | P1 | 是 | 规格写「增益 50 ms 平滑」，Listen 关着退预听却硬 reset 到 1；既有测试只测 Listen 开的交接 |
| R10-04 | low · P2 | **confirmed** | low | P2 | 是 | 两份清单现在一致（20 + 4 逐个核过），确无自动核对；tools/perf 用 GLOB，没有第三份清单 |
| R10-05 | medium · P2 | **confirmed**（部分夸大） | low | P2 | (a) 是，两处要改 | 无 CI、107 无 FMA 属实；但「没有任何脚本 / 文档命令」不对——T-014 的 `run4-final.sh` 经 `test-linux.sh bash -c` 就能跑 |
| R10-06 | low · P2 | **confirmed**（归因存疑） | low | P2 | 是（渲染工具部分） | 工具 / 测试确无 FTZ、`:571` 确是冗余；但把 ADR-016 的 −600 dBFS 切块差异归到非规格化数**站不住**（差 138 dB） |
| R10-07 | medium · P2 | **uncertain** | low | P2 | 否（需 Max + 实测） | 事实全对（固定 8× / 4× / 8 相、192k 内部 1.536 MHz），但这是规格明写的取舍、性能在预算内，是优化提案不是缺陷；实验是理想抽取的简化 |
| R10-08 | low · P2 | **confirmed** | low | P2 | 部分，两处要改 | 链与表确不设防、NaN 进 K 加权与直方图后永不自愈属实；修法把处理器守卫拿掉会让频谱仪 IN 吃到 NaN |
| R10-09 | medium · P2 | **refuted**（作为缺陷） | low | P2 | 否（需 Max） | 「Lookahead 决定延迟 + 0.3 s 去抖」是 v0.5 R9/R9b 拍板、规格明写的取舍；且延迟算错（602 不是 682，代价 +8.0 ms 不是 9.7） |

漏报（missed）见第 10 节：① 编码余量经 `getEffectiveParams()` 回喂会减两次；② `tools/perf/bench.mm` 的链级参数同样落后参数表；
③ 整条链在 −600 dBFS 量级随切块方式变（T-022 REPORT:24 已实测，与重放无关）是「块长无关」契约族的一处未定位破口；④ `LoudnessMeter::finishSubBlock` 对 NaN 做 float→int 转换是未定义行为。

---

## 1. R10-01 · 渲染工具出厂默认落后 + `getEffectiveParams().mastering` 不是实际值 —— confirmed

**自己找到的证据**

- `tools/render/RenderMain.cpp:976-1020` `makeFactoryParams()` 逐项手写 Balance / Color / Limit / GLAZE / Clay，**全文 grep `mastering|harsh|targetMode` 为 0 处**（本次复核）；
  `tools/render/README.md:248-249` 也写明「出厂默认值在 makeFactoryParams 手写一份，参数表变了要同步」——承诺已失效。
- 结构默认 `Source/dsp/MasteringFeatures.h:12-20`：`harshMode = off`、`targetMode = off`，注释「New-instance defaults live in Parameters.cpp」。
- 参数表默认 `Source/Parameters.cpp:196-207`：Harshness Mode 选项 0 = Auto、amount 50、transient 50、Spectrum Target 选项 1 = Auto、amount 35；GLAZE 默认 35（`:159-160`）。
- 链里联动 `Source/dsp/CeladonChain.cpp:117-121`：`h = unit(harshAmount) × glaze`、`t = unit(targetAmount) × glaze`（自动模式）；设计规格-高级母带控制:40「新实例默认 GLAZE 35 %：h = 0.175（每段最多约 0.53 dB），t ≈ 0.12（±0.31 dB）」。
- 同决策重放：`RenderMain.cpp:1281` 逐块记 `getEffectiveParams()`；`:1395` `replayParams` 里 `p.glazePercent = 0.0f`；`CeladonChain.cpp:135` `effective.mastering = mastering;`（原始手动设置），`:136-141` 写进 `effective.balance.*` 的才是解析量。
  重放链 `setParams` 会**无条件**用 `mastering` 重算 `advanced.harshAmount / targetAmount`（`:117-121`），把喂进来的 `balance.harshAmount` 覆盖掉 → 自动模式 × glaze 0 = 0。
  ADR-016 决定 1 的前提「GLAZE 0 的链：实际参数逐位等于喂进去的值」对 mastering 两项不成立。阳性对照容差 `replayTolerance = 1e-9`（`:1354`），h 0.175 对 0 的差远超。
- TASKS.md:172 已登记「0.1.9.0 新实例默认…GLAZE > 0 时也会动声音，**现在需要重跑**门槛」，但没有登记「工具默认缺这八项」——按现工具重跑仍是两项关着的链，R10-01 是新发现，不是已登记项。
- `CeladonChain.h:109-111` 契约注释「上一次 setParams 交给四个模块的实际参数」——对 `mastering` 字段确实不成立。

**判决**：confirmed。严重度 medium 合理；P0 只在「T-032 今晚要重跑 22 首门槛 / 出 A/B 包」这个前提下成立（跑了也是错的链），否则 P1。

**修法核验**

- ① 单一真源 `Source/ParameterDefaults.h`：可行，不改声音；Debug 处理器构造的 `jassert` 核对能在 Linux `celadon_ui_snapshot` 目标上触发（`CMakeLists.txt:282-294` 强制包含 `JucePluginDefines.h`，能编处理器）。
- ② `MasteringSettings` 加 `harshResolved / targetResolved / targetResolvedMode`：插件从不设 resolved → `if (resolved >= 0)` 分支不走，`unit(...) × glaze` 算式不动 → **插件逐位不变**成立。回喂同一个 float，逐位同值成立。
- ③ `expressible` 加 `targetResolvedMode == reference → n/a`：正确（重放链没有学过的参考，`:126-128` 会把 targetAmount 置 0）。
- **必须补的两处**（见第 10 节 ①②）：(a) 编码余量：`:132` 把 `adjusted.limit.ceilingDb − margin` 写进 `effective.limit.ceilingDb`（`:142`），而 `effective.mastering.codecMarginDb` 仍是原始 margin → 回喂时再减一次。修法 ② 要一并把 `effective.mastering.codecMarginDb` 写成 0（或同样加 resolved 标记）。现在默认 0、工具也没有开关，所以门槛没炸，但契约同样是破的。(b) `tools/perf/bench.mm:274-289` `chainParamsFor` 也是手写的链级参数（mastering 走结构默认 = 关），而同一基准的处理器级走 APVTS 默认（= 自动）——同根因的第三个实例，要一起指向真源。
- 重跑门槛前要先让 Max 定 ADR-020「待复核」第 1 条（默认两项自动保持与否），研究报告附录 C 已说，同意。

**风险**：改 `MasteringFeatures.h` 结构体（Codex 的 `AdvancedMasteringTests` 直接构造它）——只加带默认值的字段，现有用例不受影响；`GlazeController` 全文不引用 `mastering`（grep 0 处），校准参考条件的「Mast」段读的是 manual，不受 resolved 影响。

---

## 2. R10-02 · 工程存档没有格式版本号 —— confirmed（事实），降为 low / P2

**自己找到的证据**

- `Source/PluginProcessor.cpp:763-781` `getStateInformation`：`copyState().createXml()` + GLAZE 节点 + `REFERENCE_SPECTRUM` 节点；根元素没有任何版本属性（本次 grep `setAttribute` 在该文件 0 处）。
- `:824-860` `addMissingParameterNodes`：按缺席 paramID 补节点，对 `glaze_amount → 0`、`master_harsh_mode → 1 (Off)`、`master_target_mode / codec / dither → 0` 特判（`:850-857`）。
- GLAZE 节点自带 `formatVersion`（`GlazeProfile.cpp:356`、`:396`）与 `GLAZE_CONTROLLER formatVersion`（`GlazeController.cpp:3014`）；`GlazeTypes.h:26-37` 算法版本与兼容策略。

**反驳点（为什么不是缺陷级）**

- 设计规格-总体:73：「以后加参数只加不删、不改语义，追加在参数表末尾」——参数侧的迁移策略就是「append-only + 缺参回默认」，规格已定；`ParameterIds.h:22-29` 每批参数带 JUCE ParameterID 版本号。
- 设计规格-GLAZE-时间线与保存:56：「DSP 模块本身的算法升级（例如 Heat 曲线变了）不在此承诺内，届时单独说明」——报告举的三次「全局改声」（0.35 dB 余量、攻击沿、Heat 换代）都是**明知并接受**的全局改动，不是「无法区分」造成的事故。
- ADR-020 后果：「以后再加新参数，读旧工程的特判要跟着加」——特判是被接受的维护方式。
- `GLAZE_STATE` 节点自 v0.4 起每次存档都写（`createGlazeStateXml` 无条件返回节点），其 `formatVersion` / `algorithmVersion` 已能粗分「哪一代存的」。

**判决**：事实成立、是有价值的加固，但严重度 low、P2。修法前半（根元素加 `celadonStateFormat` 与 `celadonVersion` 两个属性、读时取出）可做，零风险：`ValueTree::fromXml` 会把它们当根属性（与 `uiScaleProperty` 同一位置），`replaceState` 不在乎；`JucePlugin_VersionString` 在插件与 `celadon_ui_snapshot`（强制包含 `JucePluginDefines.h`）里都可见。**后半「format ≥ 1 按 format 查表决定缺席参数默认」不要现在做**——现在没有第二个格式，查表是空架子；等真有语义变更再建。验证用 `bench statecheck`（`tools/perf/bench.mm:11`、`:1171` 存在）可行。

---

## 3. R10-03 · Listen 关着时退出归一化预听硬跳 —— confirmed

**自己找到的证据**

- `Source/dsp/CeladonChain.cpp:145` `nextPreview = releaseAllowed && ! offline && previewRequested`；`:152-153` 只在 `newParams.limit.listen` 为真时把当前预听增益交给 `listenGain`；
  `:155` `if (! releaseAllowed || (previewActive && ! nextPreview)) releaseSafety.reset();` —— Listen 关着同样 reset。
- `Source/dsp/ReleaseSafety.cpp:24-29` `reset()`：`previewGain.setCurrentAndTargetValue (1.0)`（瞬间到 1，随机序列也重置）；`:20` `previewGain.reset (fs, 0.05)` = 50 ms 斜坡；`:61-72` `previewEnabled = false` 时 `setTargetValue (1.0)` 本会按 50 ms 走；`:75-80` 增益已是 1 且不在平滑时逐位旁通。
- `:295` `releaseSafety.process` 在 `releaseAllowed && ! differenceInvolved() && ! nonFinite` 时每块都调——不 reset 的话斜坡自然走完。
- 既有用例 `tests/AdvancedMasteringTests.cpp:782-804` `testMonitoringHandover`：`params.limit.listen = true`，测的是交接给 Listen 的路径；`:800-804` 关预听后只断言 `peak <= max(before, after) * 1.03`。**没有 Listen 关着退预听的用例**（grep `setReleasePreviewEnabled` 全测试只有 :771 / :794 / :800 三处）。
- 规格：设计规格-高级母带控制:76「衰减 = min(0, 目标 − OUT 整体响度)…**增益 50 ms 平滑**；读数未知时回到 1」；ADR-020 决定 6「归一化预听只衰减…关掉编辑器就退出」。退出路径 `PluginEditor.cpp:161 / 670 / 728 / 748` 都走 `setReleasePreviewEnabled (false)` → 同一条硬 reset。现实现与「50 ms 平滑」不符。

**判决**：confirmed；medium / P1 合理（可听的电平台阶，只在监听、不进成品）。

**修法核验**：`handedOverToListen = previewActive && ! nextPreview && releaseAllowed && ! offline && newParams.limit.listen; if (! releaseAllowed || offline || handedOverToListen) reset();` 逐情形推过：
- Listen 关 + 用户点掉预听（在线、releaseAllowed）→ 不 reset → `setParams (false, …)` → `process` 里 target 1 → 50 ms 斜坡 → 走完 `setCurrentAndTargetValue (1.0)`（`:71-72`、`:116-117`）→ 逐位旁通。✓
- 离线（`offline`）→ reset ✓，既有用例 `:768-778`「offline transition removes preview from its first sample」仍过（它要求第一个样本就与 master 逐位相同）。
- 宿主旁通 / 听原声 / 差值（`! releaseAllowed`）→ reset ✓。交接给 Listen → reset ✓（否则两条增益叠加）。
- 副作用：不 reset 时 TPDF 随机状态不再重置——只是抖动序列不重头，无害。
建议用例照报告写；再加「Listen 关 + 预听开 + 宿主旁通那一块 `previewGainDb == 0`」保住原意。

---

## 4. R10-04 · CMake 与 .jucer 双清单无自动核对 —— confirmed

- `CMakeLists.txt:9` 那句承诺、`:104-124` 20 个 dsp .cpp、`:127-131` 4 个 ui .cpp；`Celadon.jucer` dsp 组 20 个 .cpp（`:29-110`，本次逐个数过：LimiterModule、LoudnessMeter、Saturator、TruePeak、ReferenceSpectrum、ReleaseSafety、BalanceModule、CeladonChain、ColorModule、HalfBandOversampler、GlazeConstants、GlazeProfile、GlazePlanner、MusicAnalyzer、GlazeCalibrator、GlazeGuard、GlazeController、ClayModule、ClayGuard、LinearPhaseLowpass），ui 组含那 4 个。`.jucer` 里没有 `compile="0"` 的 .cpp（grep 0 处）。
- `tools/perf/CMakeLists.txt:37` 用 `file(GLOB …)`，没有第三份手写清单——核对只需主 CMake 对 .jucer。
- 修法的 CMake 片段核过：`file(READ)` + `string(REGEX MATCHALL "file=\"Source/dsp/[^\"]+\\.cpp\"")` 能同时匹配 `file=` 在同一行（`:46`、`:48`）与换行（`:29`）两种写法；`list(TRANSFORM … REPLACE)` 需 CMake ≥ 3.12（工程要求 3.22）；排序后 `STREQUAL` 比较可行。
- 判决：confirmed，low / P2，修法可用。

---

## 5. R10-05 · 逐位契约验证矩阵与发布平台脱节 —— confirmed，但严重度降为 low

**成立的部分**：仓库无 `.github/`（本次 `ls` 不存在）；`scripts/test-linux.sh:38-42` 只 `-DCMAKE_BUILD_TYPE=Release`，默认 x86-64 无 FMA，`-ffp-contract=off`（`CMakeLists.txt:22-25`）与守卫宏在 107 上永远不被考验；ADR-013 后果第 3 条要求改契约代码 / 编译选项后在有 FMA 的构建上重跑全套；NEON 只由主进程在 Max 同意下本机跑（TASKS:64，2026-09-29 arm64 全套一次；ADR-002）。BUG-001 / BUG-002 都是平台特定。

**夸大的部分**：「没有任何脚本入口、环境变量或文档命令」不对——`docs/validation/2026-09-26-T014-fp-contract/run4-final.sh` 就是可复用脚本（基线 + `-march=x86-64-v3` 独立目录全套 + 守卫绊线），注释写明经 `scripts/test-linux.sh bash -c "$(cat run4-final.sh)"` 跑；`test-linux.sh` 本来支持任意远端命令。缺的是**一等入口与「何时必须跑」的纪律文字**，不是能力。

**判决**：confirmed；low / P2（测试策略缺口，不是产品缺陷）。修法 (a) 可做，两处要注意：① v3 构建必须用独立目录（`run4-final.sh` 的做法），别把 `-march` 写进共享缓存；② `01-框架/使用方式:148` 现写「测试 / 渲染构建不要加 `-march` / `-mfma`」，要改成「基线不加；契约改动后另起目录跑一次 v3」，否则两条纪律打架。(b) 需 Max。T-032 今晚动了大量 DSP 文件，按 ADR-013 收尾本就该跑一次 v3。

---

## 6. R10-06 · 渲染工具与测试没设 FTZ/DAZ —— confirmed（事实），归因存疑

**成立的部分**：`Source/PluginProcessor.cpp:553`（`processBuses`）与 `:571`（`processChain`）两处 `ScopedNoDenormals`；`processChain` 是私有成员、只被 `processBuses` 调（grep 全仓库：其余引用都是注释）→ `:571` 冗余，可删。`tools/render/RenderMain.cpp`、`tests/ChainTests.cpp`、`tests/TestMain.cpp` 无 `ScopedNoDenormals`（grep 只在 bench.mm、ColorTests、DspPartsTests、BalancePerformanceTests）。`ColorModule.h:100-104` 那段注释属实。RenderMain 单线程（grep `std::thread|ThreadPool` 0 处），每遍一个作用域够用。

**站不住的部分**：把 ADR-016 后果「整条链在歌尾 −600 dBFS 量级随切块方式不同而不同」归到非规格化数。−600 dBFS ≈ 1e-30，是正常 float；非规格化数在 |x| < 1.2e-38（−758 dBFS），差 138 dB；两者相乘的中间量（系数 ~1e-3）也仍是正常数。FTZ 开关改不了 1e-30 量级的值。`docs/validation/2026-09-28-T022-replay-gate/REPORT.md:24` 已实测「重放链自己把每块切两段处理也差同样的量」——这是某处**按块的**状态更新（空闲门限 / 每块一次的平滑步进）随切法变，与数值环境无关。所以「修后 ADR-016 的差异应消失或缩小」这条验证预期大概率不会兑现，别把它当验收条件。

**判决**：confirmed；low / P2。修法：渲染工具三处加 `ScopedNoDenormals` 可做（与插件对齐，只在歌尾 < −758 dBFS 量级变）；删 `:571` 可做。ChainTests 的 `runChain`（`tests/ChainTests.cpp:72-95`）要不要加：加了只让 ChainTests 一个文件变 FTZ 模式、其他链级测试文件仍是 IEEE，覆盖反而不一致——要么全部测试入口（`TestMain.cpp`）统一决定，要么不加；不作为今晚必做。

---

## 7. R10-07 · 过采样倍率不随采样率缩放 —— uncertain（事实全对，但是规格已定的取舍与优化提案）

**核过的事实**：`ColorModule.cpp:466-471` 三级固定 8×、`:465` 注释「FIR 阶数只由归一化过渡带决定，所以这个延迟与采样率无关」、`:507-510` 空闲近似链一级（延迟 49）；`LimiterModule.cpp:186-193` 两级固定 4×、延迟 60；`TruePeak.h:42-44` 8 相 × 64 抽头写死；`TruePeak.cpp:170-218` NEON 核 7 相具名累加器写死；`BalanceModule.cpp:258-266` FFT 点数随 fs 翻倍；`HalfBandOversampler.cpp:327/361/408/459` 确有 0 级拷贝路径。T-021 REPORT:46「192k / 32 结构性最坏约 19 %」、:12 修坯项超标 2～4 倍——但整插件绝对值仍小，Max 在 T-021 后选的是「改预算 + 优化守护」，没把高采样率过采样列为问题。

**为什么不是 confirmed**：
- 设计规格-总体:83 与 -Color:87 明写「Clip 级 4×（两级，整数延迟 60）、Heat 8×（三级，整数延迟 64）…与 Heat 取值、采样率无关」，是 Max 拍板的规格文字；改它 ≥ 88.2 kHz 的声音会变（研究报告自己也承认需拍板）。
- 实验用理想砖墙抽取，只证明「混叠底由内部速率决定」这个教科书结论；真实链的混叠底是半带 FIR 阻带（−75 / −70 / −55 dB）与之取劣，192k × 2 一级的真实数字没量过；ColorModule.cpp:460 注释里的历史实测（4× −56.7 dB、8× −107 dB）都是真链数字，与理想抽取的 −70.6 / −265.9 不在一个口径。
- 收益也没量：192k 下 Heat / Clip 过采样占整链多少（T-021 说 192k/32 最坏块的主成分是修坯与 Balance 分析），省下来能换多少百分点，报告没有数据。
- 真峰值那一项（改相数）会改 192k 下表的读数与「限的峰 = 量的峰」（ADR-004 验收口径），跨 R1 / R7，风险不小。

**判决**：uncertain；low / P2；不在今晚做。要变成 confirmed 需要：真链 96k / 192k 逐级 CPU（`celadon_bench modules`）与 ColorTests / LimiterTests 混叠用例在三种采样率的真实数字，以及 Max 对「延迟与采样率无关」改口的拍板。修法本身（按 k 取级数、延迟按模块上报求和）技术上可行，44.1 / 48 kHz 逐位不变的论断成立（级数不变）；Clip 零级时 `clipLatency = 0`、`clipDryDelays.prepare (0)` 等边角要单独测。

---

## 8. R10-08 · 非有限输入的保护只在处理器、链不设防 —— confirmed（修法要改两处）

**自己找到的证据**：`PluginProcessor.cpp:595-599` 输入 NaN/Inf → 整块清零 + `nonFiniteBlocks` 计数，在 `chain.setParams` / `chain.process` 之前；`CeladonChain.cpp:234-235` `processSlice` 先 `inputMeter.process` 再 `controller.analyzeInput`，链不查输入；`LoudnessMeter.cpp` 全文无 `isfinite`（grep 0 处）；`:255-263` K 加权转置 II 型状态与能量累加；`:271-311` `finishSubBlock`：NaN 能量 → `blockLufs` NaN → `blockLufs < histogramMinLufs` 为假 → `:305` `(int) std::floor (NaN)`（**float→int 转换 NaN 是未定义行为**，x86 实际给 INT_MIN 再被 jlimit 钳到 0）→ `:306` `histogramEnergy[bin] += NaN` → 整体响度从此 NaN 直到 `clearMeasurement`；`MusicAnalyzer.cpp:961-967` 只对非有限子块 `clearWindows()`，表永久 NaN 就永远填不满窗口。渲染工具直接把 wav 送进链（无 NaN 用例，ChainTests grep `quiet_NaN|infinity()` 0 处）。参考侧链另有清洗（`ReferenceSpectrum.cpp:410-415` `sanitize`），不在此列。

**判决**：confirmed；low / P2（插件路径已挡住，只影响无头工具 / 测试 / 基准）。

**修法两处要改**：
1. 「输入守卫挪进链，处理器只保留读计数」**不行**：`PluginProcessor.cpp:601-602` 频谱仪 IN 的 `analyzerTap.push` 在链之前、注释明写「NaN 保护之后」——守卫挪走后界面频谱仪会吃到 NaN。要么处理器守卫保留（链里再扫一遍，代价一次遍历，可接受），要么把链的守卫放进 `process` 开头并把 tap 挪到链之后（改了 IN 频谱仪的测点语义，不建议）。
2. 表的自愈只清 `ChannelState` 不够：`subBlockEnergy / ringEnergy / recentSubBlocks / histogramEnergy` 都可能已污染。最省事的是在 `finishSubBlock` 开头 `if (! std::isfinite (subBlockEnergy)) { 状态清零 + 丢掉本子块; return; }`，直方图就永远不会吃到 NaN；再在 `publishSubBlock` 前同样判 `ringEnergy`。有限输入逐位不变成立（只多一个分支）。
   渲染工具最简单是读 wav 后先清洗一遍（一次 `isfinite` 扫描，stderr 报一次），不必改链。

---

## 9. R10-09 · 「Lookahead 决定延迟」的设计代价 —— refuted（作为缺陷）；可作提案

**核过的事实**：`PluginProcessor.h:160-227` `LookaheadHold` 与其长注释（v0.5 研究 P5 R9 / R9b：拖一次旋钮宿主重启 18～34 次 → 去抖后 1 次）；`PluginProcessor.cpp:492-500` 30 Hz 定时器上报；`:611-612` 每块 `lookaheadHold.next`；`LimiterModule.cpp:394-400` 换前瞻整体重置 + 淡入；`:204-214` 延迟线按 `maxLookahead` 分配（`holdSamples = 63`，`coreDelay = lookahead + 62`）。四条后果①③④都真实存在。

**为什么 refuted**：设计规格-Limit:23（参数表）「前瞻长度（决定延迟）；参数停稳 0.3 s 后才换前瞻…来回快于 0.3 s 的自动化不生效；宿主 Reset、挂起后恢复的第一块直接采用（v0.5）」与 :68「Lookahead 去抖…拖动中不再出现静音段」是 Max 拍板并验收（V2-2）的规格文字，代价当时就写明。按核验口径「规格里已定的取舍」不算缺陷。后果②（离线导出定时器错位）只在「导出中自动化 Lookahead 且停稳 > 0.3 s」时发生，任何改延迟的插件都如此，且规格已把快自动化定义为不生效。

**数字错误**：固定按最大前瞻的限幅器延迟 = Clip 60 + round(0.010 × 48000) = 480 + 62 = **602**，不是 682；整链 602 + 1102 + 64 = **1768**，比现默认 1384 多 **384 样本 ≈ 8.0 ms**（报告摘要表写 +8 ms、JSON 写 +9.7 ms / +464 样本，自相矛盾，以 8.0 ms 为准）。

**判决**：refuted；若 Max 愿意重新权衡，可作 P2 提案（+8 ms 换掉 LookaheadHold、定时器上报与换档静音），今晚不做。

---

## 10. 漏报（missed）

1. **编码余量经 `getEffectiveParams()` 回喂会被减两次**（R10-01 同根因，报告没点出）：`CeladonChain.cpp:130-132` 算 margin 并把 `adjusted.limit.ceilingDb − margin` 钳到 [−4.5, 0]，`:142` 写进 `effective.limit.ceilingDb`；`:135` `effective.mastering = mastering` 里 `codecMarginDb` 仍是原始 margin。重放链拿到 `effective` 后再减一次。现在默认 0、渲染工具没有开关所以没触发；R10-01 修法 ② 必须一并把回写的 `codecMarginDb` 置 0（ceiling 已含余量）。
2. **`tools/perf/bench.mm:274-289` `chainParamsFor`** 手写链级参数，mastering 走结构默认（关），而同一基准的处理器级用 APVTS 默认（自动）——0.1.9.0 起「模块 vs 处理器」两级基准不再同参数；与 R10-01 同类，应一起指向真源。
3. **整条链的输出随切块方式在 −600 dBFS 量级变**（T-022 REPORT:24 实测：同一条链每块切两段也差同样的量）：这是 HalfBand / Color / Limiter / LinearPhaseLowpass「块长无关」契约族的一处未定位破口，ADR-016 记为「疑似某个模块的空闲跳过门限，未追查」。R10-06 把它归到非规格化数是错的（见第 6 节）；值得的做法是用渲染工具的 `--trace` 或链级测试逐模块二分定位（成本 S），不必今晚。
4. **`LoudnessMeter::finishSubBlock:305` 对 NaN 做 `(int) std::floor(...)`** 是未定义行为（属 R10-08 范围，报告只说「永久污染」没说 UB）。

## 11. 给主进程的落地顺序建议（只列可做的）

R10-01（含 missed ①②）→ R10-03 → R10-04 → R10-08（按第 8 节改法）→ R10-06（只改渲染工具 + 删 `:571`）→ R10-05(a)（含 `使用方式:148` 改口）→ R10-02（只加两个属性）。R10-07 / R10-09 交 Max。收尾按 ADR-013 在 107 另起目录跑一次 `-march=x86-64-v3` 全套。
