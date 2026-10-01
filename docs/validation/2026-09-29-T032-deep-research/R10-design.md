# R10 · 跨模块整体设计（信号链 / 延迟 / 过采样 / 增益结构 / 状态 / 测试策略）深度审计

- 任务：T-032 阶段 A 研究代理 R10（Fable 5.1 · max），2026-09-29 夜。
- 对象：工作树 `.claude/worktrees/fable-deep-research-634a5e`，分支自 main `e6b445b`（v0.1.8.5）。
- 方法：先读档案（`01-框架/设计规格-总体`、`-Limit`、`-高级母带控制`、`-GLAZE-时间线与保存`，ADR-001/004/006/008/009/012/013/014/016/018/020，三份踩坑），再读 `Source/dsp/CeladonChain.{h,cpp}`、全部 `Source/dsp/*.h`、`PluginProcessor.{h,cpp}`、`Parameters.cpp`、`ParameterRanges.h`、`LoudnessMeter.cpp`、`TruePeak.cpp`、`ReleaseSafety.cpp`、`ReferenceSpectrum.cpp`（局部）、`LimiterModule.cpp` / `ColorModule.cpp` / `BalanceModule.cpp` / `ClayModule.cpp` 的 prepare / process 入口、`tests/ChainTests.cpp`、`TestHelpers.h`、`TestMain.cpp`、`tools/render/RenderMain.cpp`（头部、出厂默认、重放）、`CMakeLists.txt`、`Celadon.jucer`、`scripts/test-linux.sh`、`docs/*.md` 五份评审、T-021 报告、`04-协调/TASKS.md`（已登记待认领项已剔除）。
- 只读；唯一写入是本文件。数值实验在 107（numpy，`~/.cache/celadon-t032/R10/`，做完已删）。本机没编译、没跑测试。
- 结论按「优先级 × 置信度」排序；每条都给文件:行号。已知且登记的项（修坯预算、474 % 尖峰、mastering-bench 最大值、Color NEON 在 Linux ARM、界面 21 %、高温、起音密度、AAC/MP3、AU 整数、BUG-001～004）不重报。

## 0. 摘要

| 编号 | 结论 | 严重度 | 优先级 | 工作量 | 置信度 | 今晚能做 |
|---|---|---|---|---|---|---|
| R10-01 | 离线对照工具的出厂默认落后于参数表（0.1.9.0 的 8 个高级母带参数全缺），门槛与 A/B 渲染的是「推响不刺 / 目标频谱都关」的链；`getEffectiveParams().mastering` 是原始值不是实际值，补上默认后同决策重放会走样 | medium | P0 | M | 0.90 | 能 |
| R10-02 | 工程存档没有格式版本号，迁移只靠「缺哪个参数」推断，无法区分老工程做兼容 | medium | P1 | S | 0.85 | 能 |
| R10-03 | Listen 关着时退出「归一化预听」，监听增益从预听衰减瞬间跳回 0 dB（硬 reset，无斜坡） | medium | P1 | S | 0.75 | 能 |
| R10-04 | CMake 与 .jucer 两份源文件清单没有自动核对（现在一致，纯靠人） | low | P2 | S | 0.90 | 能 |
| R10-05 | 逐位契约的验证矩阵与发布平台脱节：无 CI，107 只跑无 FMA 的 x86-64 基线，NEON 内核只靠 Max 手动跑 | medium | P2 | S（a）/ 待 Max（b） | 0.80 | (a) 能 |
| R10-06 | 渲染工具与测试没设 FTZ/DAZ，数值环境与插件（`ScopedNoDenormals`）不一致 | low | P2 | S | 0.80 | 能 |
| R10-07 | 过采样倍率（Heat 8×、Clip 4×）与真峰值插值倍率不随采样率缩放，192 kHz 下内部速率 1.536 MHz 纯浪费；实验证明 384 kHz 内部速率下三种采样率混叠底逐位相同 | medium | P2 | L | 0.75 | 需 Max 拍板 |
| R10-08 | 非有限输入的保护放在处理器、链自身不设防：无头工具 / 测试喂 NaN 会把 IN 表 K 加权状态永久污染 | low | P2 | S | 0.70 | 能 |
| R10-09 | 「Lookahead 决定延迟」的设计代价：换档 = 宿主重启 + 限幅器静音重置 + 0.3 s 去抖废掉自动化 + 离线导出错位；固定按最大前瞻上报可全部消掉，代价 +8 ms | medium | P2 | M～L | 0.60 | 需 Max 拍板 |

核实过、**不报**的项见附录 A（GLAZE 对模块统计的 NaN 清洗、无锁读取有界、限幅器 prepare 按新采样率重算前瞻、超块切块、延迟求和、尾巴常数、单声道口径、Guard 切片开销、Listen 交接等）。

---

## R10-01 · 离线对照工具的出厂默认与实际参数契约（P0 · medium · M · 0.90）

**类别**：测试策略 / 模块间契约（(b)(d)）。

**证据**

- `tools/render/RenderMain.cpp:976-1020` `makeFactoryParams()`：逐项手写 Balance / Color / Limit / GLAZE / Clay 的出厂默认，注释「参数表改了默认值，这里同步改」；**没有一行 `p.mastering`**（全文 grep `mastering|harsh|targetMode` 为 0 处），于是沿用 `MasteringSettings` 的结构默认：`Source/dsp/MasteringFeatures.h:12-20` `harshMode = off`、`targetMode = off`、`codecMarginDb = 0`、`dither = off`。
- 插件的新实例默认（`Source/Parameters.cpp:196-207`）：`master_harsh_mode` 选项 0 = Auto、`master_harsh_amount` 50、`master_transient` 50、`master_target_mode` 选项 1 = Auto、`master_target_amount` 35。GLAZE 默认 35（`:159-160`）。
- 链里这两项随 GLAZE 联动：`Source/dsp/CeladonChain.cpp:115-121` `h = unit(harshAmount) × glaze`、`t = unit(targetAmount) × glaze`；设计规格-高级母带控制 第 3 节实测：新实例 GLAZE 35 时每段最多约 0.53 dB（刺耳）与 ±0.31 dB（目标）。
- 同决策重放（ADR-016）把 `getEffectiveParams()` 逐块记下再喂回 GLAZE 0 的链：`RenderMain.cpp:1281`、`:1392-1416` `replayParams()` 直接 `auto p = effective` 只改 Clay 三态；而 `CeladonChain.cpp:135` `effective.mastering = mastering;` 写回的是**原始**手动设置，`:136-141` 写进 `effective.balance.*` 的才是解析后的量。重放链 GLAZE 0 → `:117-121` 再算一遍 `automatic × 0 = 0`。

**机理**

1. 22 首门槛、A/B 试听包、`--trace` 诊断渲染的都是「高级母带两项关」的链。用户新实例 GLAZE > 0 听到的却是两项「自动」开着的链（ADR-020 待 Max 复核的正是这个默认）。TASKS 待认领里已注明「0.1.9.0 新实例默认…GLAZE > 0 时也会动声音，需要重跑门槛」——用现在的工具重跑，跑的仍是旧链，结论对不上用户听到的东西。这就是 `Parameters.cpp` 与 `RenderMain.cpp` 两份默认值「同步改」承诺失效的实例。
2. 一旦补上默认，重放阳性对照（`RenderMain.cpp:1914-1920`，要求最大样本差 ≤ −180 dBFS）会在每首 GLAZE > 0 的歌上 mismatch：自动链有 h、t，重放链为 0。而且不能靠「手动模式 + 把 h×100 当 amount」复现——`unit(a)·g` 与 `unit((a·g·100))` 不逐位相同，−180 dBFS 的容差撑不住。
3. 根子是 `getEffectiveParams()` 的契约（`CeladonChain.h:109-111`「上一次交给四个模块的实际参数」）对 `mastering` 字段不成立：它既不是实际值，也不能无损地喂回去。

**修法**（三处，全部今晚可做）

1. 出厂默认收成一处真源：新建 `Source/ParameterDefaults.h`（与 `ParameterRanges.h` 同一做法，只依赖 juce_core），提供 `celadon::param::defaults::factoryChainParams()`（返回填满的 `ChainParams`，含 `mastering` 八项与 `glazePercent = 35`）和各标量常量；`Parameters.cpp::createParameterLayout()` 的默认值改引用这些常量；`RenderMain.cpp::makeFactoryParams()` 改为 `return celadon::param::defaults::factoryChainParams();`。处理器 Debug 构造里已有「范围与决策器钳位一致」的核对（`PluginProcessor.cpp:134-235`），同处再加一段：对每个参数 `getDefaultValue()` 反归一后与 `factoryChainParams()` 对应字段逐项 `approximatelyEqual`，不等就 `jassert`——以后再漂就在 Debug 启动时断住。
2. 给实际参数一个无损通道：`MasteringSettings` 加 `float harshResolved = -1.0f, targetResolved = -1.0f;`（负 = 未设）与 `TargetMode targetResolvedMode = TargetMode::off`。`CeladonChain::setParams`：`if (mastering.harshResolved >= 0) advanced.harshAmount = mastering.harshResolved; else 照旧算`；target 同理（`targetResolvedMode` 优先于自动解析；`reference` 且无参考 → 解析为 off）。回写 `effective.mastering`：`harshResolved = advanced.harshAmount`、`targetResolved = advanced.targetAmount`、`targetResolvedMode = mode`、`harshMode/targetMode` 改成 manual / 解析出的显式模式，其余原样。这样 `effective.mastering` 喂回去逐位同值（同一个 float、不再经乘法），插件路径（从不设 resolved）逐位不变。
3. 渲染工具：`replayParams` 不必改（已整份拷贝）；`expressible` 判定（`:1914`、`:2329`）加「任一块 `targetResolvedMode == reference`」→ 记 n/a（重放链没有学过的参考）；`--selftest` 加一组「高级母带默认开着（GLAZE 35、harsh auto 50、target auto 35）→ 重放 ok」的阳性对照；`metrics.md` 头部打印本次渲染的 mastering 设置。顺带把 `tools/render/README.md` 的「出厂默认手写一份」改成指向真源。

**逐位影响**：插件输出逐位不变（新增字段默认不生效；`unit(...)` 的算式不动）。渲染工具成品**会变**（多了两项默认开着的处理——这正是目的）；GLAZE 0 的成品不变（h、t 都乘 0）。

**验证**

- `celadon_render --selftest`：新增阳性对照 ok；既有「GLAZE 0 逐位直通」不变。
- 任选 1 首歌 `--glaze 35 --clay auto`：`replay_check = ok`、`replay_max_diff_dbfs ≤ −180`；`metrics.csv` 里成品响度 / 真峰值与未补默认前不同（阳性）。
- 单元测试（`tests/AdvancedMasteringTests.cpp` 加用例）：链 A（自动 h/t、GLAZE 35）跑 3 s，逐块把 `getEffectiveParams()` 喂给链 B（GLAZE 0）→ 输出逐位相同（memcmp）；链 C 不带 resolved 字段与改前逐位相同（回归）。
- Debug 处理器构造的默认值核对（bench statecheck 或 celadon_ui_snapshot 在 Linux 上编处理器即可触发）。

---

## R10-02 · 工程存档没有格式版本号（P1 · medium · S · 0.85）

**类别**：参数 / 存档往返、版本迁移策略（(b)）。

**证据**

- `Source/PluginProcessor.cpp:763-781` `getStateInformation`：`parameters.copyState().createXml()` + GLAZE 节点 + `REFERENCE_SPECTRUM` 节点 → `copyXmlToBinary`。根元素只有 APVTS 的类型名与界面缩放属性，**没有任何版本 / 格式属性**（grep `formatVersion|stateVersion|setAttribute.*[Vv]ersion` 在 PluginProcessor.cpp 为 0 处）。
- `:824-860` `addMissingParameterNodes`：按「状态树里缺哪个 paramID」补节点，并对 `glaze_amount`（→ 0）、`master_harsh_mode`（→ Off）、`master_target_mode / master_codec_margin_db / master_dither`（→ 0）逐个特判。
- 对比：GLAZE 节点自带 `formatVersion`（`GlazeProfile.cpp:356`、`:396`）与 `algorithmVersion` / `oldestCompatibleAlgorithmVersion`（`GlazeTypes.h:26-37`），有完整的兼容策略；参数树没有。
- 已经发生过三次「老工程声音全局改变、无法按存档区分」：Limit 需求增益预留 0.35 dB（设计规格-Limit 2026-09-26 修订第 1 条「这是全局改动…不再与 v0.6.1 逐位相同」）、0.1.7.2 攻击沿两级滑动平均（同规格 2026-09-28 修订第 1 条「只要限幅器在压…GLAZE 0 也一样」）、Heat 曲线换代（heat-and-balance-revision「旧工程不能保证逐位或听感相同」）。

**机理**

「按缺参推断」只能表达「加了参数」这一种变化；参数语义变了、DSP 变了、范围变了（例如以后把 `limit_ceiling` 下限从 −3 放到 −4.5 与编码余量合并）都无法从旧工程识别。`master_harsh_mode` 的特判本身就是在用「缺参」代替「版本 < 0.1.9.0」，以后每加一个默认开着的功能都得再加一条特判，且对「用户在 0.1.9.0 里把该参数删掉再存」这类边角无法区分。

**修法**

1. `getStateInformation`：根元素 `xml->setAttribute ("celadonStateFormat", 1); xml->setAttribute ("celadonVersion", JucePlugin_VersionString);`。
2. `setStateInformation`：`const auto format = xml->getIntAttribute ("celadonStateFormat", 0);`，传给 `addMissingParameterNodes (newState, format)`；`format == 0`（≤ 0.1.8.5 的存档）沿用现有按缺参特判（行为不变）；`format ≥ 1` 起改为「按 format 查表决定缺席参数的默认」，不再逐 ID 特判。把这个号也交给 `chain.restoreGlazeStateXml` 的调用方留作以后 DSP 兼容开关（例如 `dspCompat`）的锚点。
3. 档案：设计规格-总体「参数」段补一句「存档根元素带 `celadonStateFormat`；改语义必升号」；`ParameterIds.h` 版本号常量旁注明与存档格式号的关系。

**逐位影响**：无（只多两个 XML 属性；读旧存档路径不变）。

**验证**：`tools/perf/bench.mm statecheck`（Mac）加两条：存出的 XML 含两个属性；读入不带属性的 0.1.8.5 存档 → 与现在逐项相同。Linux 上 `celadon_ui_snapshot` 目标能编处理器，可加同样的检查跑一次。

---

## R10-03 · Listen 关着时退出归一化预听会硬跳（P1 · medium · S · 0.75）

**类别**：监听链路状态机（(a) 增益平滑不连续）。

**证据**

- `Source/dsp/CeladonChain.cpp:147-156`：
  - `:152-153` 预听关掉且 `newParams.limit.listen` 为真 → `listenGain.setCurrentAndTargetValue (releaseSafety.getCurrentPreviewGain())`（交接给 Listen，之后由 Listen 的 20 ms 平滑接住）；
  - `:155` `if (! releaseAllowed || (previewActive && ! nextPreview)) releaseSafety.reset();` —— **Listen 关着时同样走 reset**。
- `Source/dsp/ReleaseSafety.cpp:24-29` `reset()`：`previewGain.setCurrentAndTargetValue (1.0)`；`:75-80` 之后 `process` 直接返回（精确旁通）。
- 对照 `ReleaseSafety::process` `:61-72`：`previewEnabled = false` 时 `targetGain = 1.0`，`previewGain.setTargetValue (1.0)` 本来会按 50 ms 斜坡回 1（`prepare` 里 `previewGain.reset (fs, 0.05)`）。是 `:155` 的硬 reset 把斜坡跳过了。
- 既有测试 `tests/AdvancedMasteringTests.cpp:794-804` 只断言关掉后前 50 ms 的峰值「不比之前更高」（`peak <= max(before, after) * 1.03`），瞬间跳回 0 dB 也能过，所以没被抓到。

**机理**：预听衰减 = min(0, 目标 LUFS − 整体响度)，常见 −4～−12 dB。用户在界面点掉预听（Listen 没开）→ 同一块内增益从 −8 dB 直接变 0 dB，是一个可听的电平台阶（母带信号上 +8 dB 的瞬跳，可能伴随咔哒）。注释「Monitoring must be neutral from the first bypass/offline sample」的本意是宿主旁通 / 离线 / 听原声 / 差值这些**必须立即中性**的场景（`! releaseAllowed || offline`），并不需要覆盖「用户实时点掉预听」。

**修法**：`:155` 改为
```cpp
const auto handedOverToListen = previewActive && ! nextPreview && releaseAllowed && ! offline && newParams.limit.listen;
if (! releaseAllowed || offline || handedOverToListen) releaseSafety.reset();
```
Listen 关着时不 reset，让 `ReleaseSafety::process` 自己按 50 ms 斜坡回到 1（它在斜坡走完后会精确落到 1 并转入逐位旁通，`ReleaseSafety.cpp:71-72`、`:116-117`）。交接给 Listen 的情况必须仍 reset（否则两条增益叠加）。

**逐位影响**：只在「预听关闭后的 50 ms」监听信号不同；成品、表、GLAZE 不受影响（预听本来就在所有测点之后）。

**验证**：`AdvancedMasteringTests` 加用例：Listen 关、预听开、稳定后关预听，逐样本增益（输出 / 母带）相邻步长 ≤ 50 ms 斜坡的理论步长（正弦过零处不相除，写法同 `ChainTests::testListenSilenceAutomationAndBypass`）；再加一条：宿主旁通瞬间 `previewGainDb == 0`（保住原意）。

---

## R10-04 · CMake 与 .jucer 双源文件清单无自动核对（P2 · low · S · 0.90）

**证据**：`CMakeLists.txt:9`「Source/dsp/ 的源文件列表必须与 Celadon.jucer 的 dsp 组保持一致；加文件时两边一起改」；`:104-124` `CELADON_DSP_SOURCES` 20 个 .cpp；`Celadon.jucer:27-113` dsp 组 20 个 `compile="1"` 的 .cpp（本次逐个核对一致）；`:127-131` `CELADON_HEADLESS_UI_SOURCES` 4 个 ui .cpp 也要在 .jucer ui 组里。历史上 T-026 就漏过一次 .jucer 条目（TASKS：「待 T-025 在 Mac 编 main 时 Projucer 重存（.jucer 加了头文件条目）」）。

**机理**：漏在 .jucer 里的 .cpp 会让 Mac 插件链接失败或——更糟——旧实现留在插件、新实现只在测试里；漏在 CMake 里的文件则测试跑不到。现在没有任何东西拦。

**修法**：CMake 配置期核对（纯 CMake，零依赖）：
```cmake
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Celadon.jucer" _jucer)
string(REGEX MATCHALL "file=\"Source/dsp/[^\"]+\\.cpp\"" _jucer_dsp "${_jucer}")
list(TRANSFORM _jucer_dsp REPLACE "file=\"(.*)\"" "\\1")
list(SORT _jucer_dsp)
set(_cmake_dsp ${CELADON_DSP_SOURCES}) ; list(SORT _cmake_dsp)
if(NOT _jucer_dsp STREQUAL _cmake_dsp)
  message(FATAL_ERROR "Source/dsp 清单不一致：.jucer=${_jucer_dsp} / CMake=${_cmake_dsp}")
endif()
```
ui 组同法核 `CELADON_HEADLESS_UI_SOURCES` 是 .jucer ui 组的子集。`.jucer` 里 `compile="1"` 与 `file=` 不在同一行，正则按 `file="…cpp"` 匹配即可（.jucer 里 .cpp 一律 compile=1）。

**逐位影响**：无。**验证**：故意从 CMake 列表删一个文件 → 配置失败；恢复 → 通过。

---

## R10-05 · 逐位契约的验证矩阵与发布平台脱节（P2 · medium · S(a) / 待 Max(b) · 0.80）

**类别**：测试策略 / 构建（(d)）。

**证据**

- 仓库没有 `.github/`（`ls .github` 不存在）；ADR-017 / 全局 CI 纪律里说的自托管 runner 在本仓库尚未接入。
- `scripts/test-linux.sh:5,38-42`：只在 107（x86_64）`cmake -DCMAKE_BUILD_TYPE=Release` 默认 `-march=x86-64`——**没有 FMA**，`-ffp-contract=off` 与守卫宏在这台机器上永远不会被真正考验。
- ADR-013 决定 5 与后果第 3 条：守卫「只是绊线」，「以后加逐位契约代码、改编译选项（尤其 -march / -mfma），必须在那种构建上重跑全套」——但没有任何脚本入口、环境变量或文档命令让人这么做（T-014 当时是手工 `-march=x86-64-v3` 跑的一次）。
- NEON 内核（`TruePeak.cpp:163-218`、`HalfBandOversampler` NEON 路径、`BalanceModule::filterChannelPairPacked`、Color 打包）只在 Max 的 Mac 上人工跑过（T-025 于 2026-09-29 跑过一次 arm64 全套）；ADR-002 与全局纪律禁止代理在本机跑测试。
- BUG-001（SSE2 在 DAZ 下不逐位）、BUG-002（GCC FMA）都是「平台 / 编译器特定」的逐位破坏，恰是这类矩阵盲区里长出来的。

**机理**：项目把大量正确性建立在「三条 SIMD 路径与块长无关、逐位相同」的契约上，而自动化只覆盖 SSE2 基线一条；发布平台（Apple Silicon NEON + clang -O3 + LTO）的契约完全没有自动门槛。

**修法**

(a) 今晚可做：`scripts/test-linux.sh` 透传编译选项（`CELADON_CXX_FLAGS` 环境变量 → `cmake … -DCMAKE_CXX_FLAGS="$CELADON_CXX_FLAGS"`，构建目录名带选项哈希以免与基线混用），`docs/BUILDING.md` 与 `01-框架/使用方式` 写明：改 `HalfBandOversampler / Saturator / TruePeak / BalanceModule / ColorModule / ClayModule / LinearPhaseLowpass / LimiterModule` 或任何编译选项 → 必须再跑一次 `CELADON_CXX_FLAGS=-march=x86-64-v3 scripts/test-linux.sh`；T-032 收尾时先跑一次留证据。
(b) 待 Max 拍板：NEON 的自动门槛只有两条路——一台**非工作机**的 Mac 自托管 runner（不违反「本机不跑测试」），或 109 上 aarch64 交叉 + qemu-user 只跑 `DspPartsTests / ColorTests / BalanceTests / ClayTests`（慢；且 Linux aarch64 GCC 会撞上已知的 Color NEON 融合乘加差异，需要 clang）。

**逐位影响**：无。**验证**：(a) 用 T-014 修前的备份源码在 v3 上跑必须失败、当前源码必须通过。

---

## R10-06 · 渲染工具与测试没设 FTZ/DAZ（P2 · low · S · 0.80）

**证据**：插件在 `PluginProcessor.cpp:553` 与 `:571` 两处 `juce::ScopedNoDenormals`（嵌套重复，第二处冗余）；`tools/render/RenderMain.cpp`、`tests/TestMain.cpp`、`tests/ChainTests.cpp` 全文无 `ScopedNoDenormals`（grep 只有 `tools/perf/bench.mm:551,732`）。`ColorModule.h:100-104` 明写：开着 FTZ/DAZ 且输入含非规格化数时，空闲跳过与不跳过的差别 < 1.2e-38。ADR-016 后果：「整条链在歌尾 −600 dBFS 量级随切块方式不同而不同（疑似某个模块的空闲跳过门限），未追查」——这正是没有 FTZ 时非规格化数在延迟线 / 双二阶里慢慢衰减、又被各级跳过门限按不同切法截断的典型现象。

**机理**：22 首门槛与试听包出自一个数值环境与插件不同的进程；差别在 −600 dBFS 量级，不影响听感与指标，但 (1) 让「工具 = 插件」这条前提不严格；(2) x86 上非规格化算术走微码辅助，歌尾长静音段变慢。

**修法**：`RenderMain.cpp` 的 `renderPass / replayPass / --selftest` 每遍开头 `juce::ScopedNoDenormals nd;`（整遍一次即可，同线程）；`ChainTests` 的 `runChain` 助手同样加（DspPartsTests 自己切 FTZ/DAZ 组合的用例不受影响，它们自己保存 / 恢复 MXCSR）。顺手删掉 `PluginProcessor.cpp:571` 的重复声明。

**逐位影响**：插件否；渲染工具与链级测试的输出在 < 1.2e-38 量级会变（歌尾）。**验证**：同一首歌前后各渲染一次，除尾巴 −600 dBFS 段外逐位相同；ADR-016 提到的切块差异应消失或缩小。

---

## R10-07 · 过采样倍率与真峰值插值倍率不随采样率缩放（P2 · medium · L · 0.75，需 Max 拍板）

**类别**：优化 / 无谓的过采样（(c)）。

**证据**

- Heat：`ColorModule.cpp:466-471` 固定三级半带 8×，`:473-475` 延迟 64「与采样率无关」；空闲近似链 `:507-510` 一级。Clip：`LimiterModule.cpp:186-193` 固定两级 4×、延迟 60。真峰值：`TruePeak.h:42-44` 固定 8 相 × 64 抽头。Balance 分析：`BalanceModule.cpp:258-266` FFT 点数随采样率翻倍（bin 宽恒 23.4 Hz），每 hop 的 FFT 代价随 fs 线性增长。
- 于是 192 kHz 下 Heat 湿路跑在 **1.536 MHz**、Clip 在 768 kHz、三个真峰值检测器各 1.536 MHz 等效插值。T-021：整插件 192k/32 结构性最坏 17～19 %、one-knob 评审 192k/32 平均 5.1 %（48k 的 4 倍），与「每样本代价不随 fs 缩放」一致。

**实验（107，numpy，理想砖墙抽取，脚本与全部输出见附录 B）**

| 非线性 | fs_base × L | 内部速率 | 带内（20 Hz～20 kHz）混叠 rms | 最大混叠谱线 |
|---|---|---|---|---|
| Heat Tube（drive 2、asym 0.04；15 k + 19 kHz 各 0.5） | 48k × 8 | 384 kHz | −145.2 dB | −147.6 dB |
| | 96k × 4 | 384 kHz | −145.2 dB | −147.6 dB |
| | 192k × 2 | 384 kHz | −145.2 dB | −147.6 dB |
| | 192k × 8（现状） | 1536 kHz | −265.9 dB | −273.7 dB |
| | 192k × 1 | 192 kHz | −70.6 dB | −72.2 dB |
| Clip SoftClip（T = 0.891，10 kHz 1.6 推进拐点） | 48k × 4（现状） | 192 kHz | −60.1 dB | −62.0 dB |
| | 96k × 2 | 192 kHz | −60.1 dB | −62.0 dB |
| | 192k × 1 | 192 kHz | −60.1 dB | −62.0 dB |
| | 192k × 2 | 384 kHz | −83.8 dB | −84.0 dB |

**机理**：混叠只由内部速率决定。Heat 在 384 kHz 内部速率上的混叠底（−145 dB）已在 float 噪底量级，192k × 8 把它推到 −266 dB 毫无意义；Clip 现状在 48 kHz 就只有 −60 dB（这是 R1 范围的独立观察：Clip 100 极限驱动下 4× 不够，可考虑 48k 用 8×），192k × 1 与之完全相同、192k × 2 反而更好。真峰值检测同理：ADR-004 选 8 × 64 是为了 0.45 fs 处不低读，192 kHz 下音乐内容 ≤ 0.1 fs，2 相 × 64 抽头同样精确。

**修法（分三档，按 round(fs / 48000) 取 2 的幂 k）**

1. Heat：级数 = 3 − log2 k（48k 三级 8×、96k 两级 4×、192k 一级 2×）；Clip：级数 = 2 − log2 k（192k 零级，`HalfBandOversampler` 已支持 0 级 = 倍率 1 的拷贝路径，`.h:113`）。级数变则延迟变（三级 64、两级 60、一级 49、零级 0；`ColorModule.cpp:507` 注释与 `getLatencyInSamples` `.cpp:488-500` 都是整数），链按各模块上报求和（`CeladonChain.cpp:482-489`），`ChainTests.cpp:1757` 写死的 96k = 2584 与文档里的「64 / 60 与采样率无关」要跟着改。`heatPrimeLength` / `clipPrerollLength` 仍按 `getMemoryInSamples()` 的断言校核；Heat 空闲近似链（现在固定一级）在 192k 与主链同为一级，`proxyAlignment` 变 0。
2. 真峰值：`TruePeakDetector` 加「相位数」模板参数（8 / 4 / 2），`tapsPerPhase` 不变（延迟 32 不变，链延迟不受影响）；向量核为 7 相写死（`TruePeak.cpp:170-218`），要给 3 相 / 1 相各出一个变体，或退回标量参照实现（192k 下每样本 64 MAC，比现在 448 还少）。这一项属 R7 / R1 范围，这里只标出。
3. 可选：Balance 分析前 4:1 / 2:1 抽取到 48 kHz（只复用半带下采样级），FFT 永远 2048 点，分析代价与采样率无关、频率分辨率不变——这是 192k/32 结构性最坏块的主要成分（T-021 §4）。改位，单独议。

**逐位影响**：44.1 / 48 kHz **逐位不变**；≥ 88.2 kHz 输出改变（混叠底不变或更好，但滤波器通带纹波与延迟不同）。设计规格-总体「过采样」与 -Color / -Limit 的延迟表述要改，属于推翻 Max 已拍板文字 → **需拍板**。

**验证**：`celadon_bench modules` 在 96k / 192k 逐级前后对比；`ColorTests` / `LimiterTests` 的混叠回归（现有「4.5 kHz 热输入 13.5 kHz 谐波」类用例）在三种采样率各跑一遍并断言不劣于 48k；ChainTests 延迟用例改成按模块上报之和断言（脉冲实测已有）。

---

## R10-08 · 非有限输入的保护放在处理器、链自身不设防（P2 · low · S · 0.70）

**证据**：`PluginProcessor.cpp:594-599` 输入含 NaN/Inf → 整块清零 + 计数（在链之前）；`CeladonChain::processSlice`（`CeladonChain.cpp:234-235`）先 `inputMeter.process (io)`、`controller.analyzeInput`，再进模块，链自己不查输入；`LoudnessMeter.cpp` 全文没有 `isfinite`（K 加权双二阶 `:118-125`、能量累加 `:262-263`）；`MusicAnalyzer.cpp:963-964` 只是**跳过**非有限的子块。`tools/render/RenderMain.cpp` 直接把 wav 样本送进链（读的是 32-bit float wav，坏文件可含 NaN）；`ChainTests` 也没有 NaN 输入用例（grep `quiet_NaN|infinity()` 在 ChainTests 为 0）。

**机理**：一个 NaN 进 IN 表的 K 加权状态后永不自愈（转置直接 II 型状态与 NaN 相乘还是 NaN），之后每个子块 `meanSquare` 都是 NaN → 分析器全部跳过 → GLAZE 永远「聆听中」、Listen 永不更新、整体响度读数 `noReading`，直到 `prepare`。插件靠处理器挡住了，但同一条链被渲染工具、测试、基准直接调用时不设防——「无头工具 = 插件」这条前提在错误输入上不成立；且 `restartWithFadeIn` 有意不清两块表（`:254`），所以内部 NaN 恢复路径也依赖「表从没吃过 NaN」。

**修法**：把输入守卫挪进链：`CeladonChain::process` 开头 `if (containsNonFinite (io)) { io.clear(); ++inputNonFiniteBlocks; }`（原子计数，供处理器读），处理器只保留计数读取；`LoudnessMeter::process` 末尾加一道自愈：任一声道状态非有限 → 该声道状态清零（`ChannelState{}`），避免二次防线失效时永久污染（每块 4 次 `isfinite`，可忽略）。渲染工具读到 NaN 时在 stderr 报一次。

**逐位影响**：有限输入逐位不变（守卫只在非有限时动作；处理器原本就会清零同一块）。**验证**：ChainTests 加用例：中途一块含 NaN → 该块输出 0、之后 1 s 内整体响度读数恢复、GLAZE 分析继续推进（`listenedSeconds` 递增）；对照改前：读数停在 `noReading`。

---

## R10-09 · 「Lookahead 决定延迟」的设计代价（P2 · medium · M～L · 0.60，需 Max 拍板）

**证据**

- 设计规格-Limit 参数表：Lookahead「决定延迟；参数停稳 0.3 s 后才换前瞻…来回快于 0.3 s 的自动化不生效」。
- `PluginProcessor.h:160-227` `LookaheadHold`（去抖状态机，约 60 行 + 6 个状态量 + 「提前采用」两种例外）；`PluginProcessor.cpp:492-500` 30 Hz 消息线程定时器 `setLatencySamples`；`:611`、`:625`。
- `LimiterModule.cpp:394-400` 每次换前瞻整体重置 + 静音一个延迟长度 + 10 ms 淡入（`:303-309`）。
- `LimiterModule.cpp:204-214`：`coreDelays / bypassDryDelays / ceilingDelay / gainDelay` 本来就按 `maxLookahead`（10 ms）分配。

**机理**：这一设计选择带来四个后果：① 每换一档宿主收到延迟变化 → VST3 `restartComponent`，FL 里挡住音频线程（v0.5 R9 记录「拖一次旋钮宿主重启 18～34 次」，去抖后 1 次，仍有 1 次）；② 离线导出时定时器按墙钟、宿主按音频位置推进，两者错位 → 换前瞻之后一段成品用的是旧的延迟补偿（错位 = 前瞻差，毫秒级）；③ Lookahead 自动化被去抖废掉；④ 换档那一刻母带总线静音约 30 ms（延迟 + 淡入）。

**替代设计**：延迟固定 = `clip(60) + round(0.010 fs) + 62`（48k 682 + Clay 1102 + Color 64 = 1848；现默认 1384，多 464 样本 ≈ 9.7 ms）。前瞻 < 10 ms 时在核心延迟线多垫 `maxLookahead − lookahead`（缓冲已存在）；换档只重置内部滑动窗（仍可保留 10 ms 淡入，或用两套核 20 ms 交叉淡化彻底消掉静音），**不再上报延迟、不再需要 LookaheadHold 与定时器上报**，`pendingLatency` 在 prepare 后恒定；`getTailLengthSeconds` 不变。

**逐位影响**：内容逐位不变、整体平移（延迟常数变）；测试里写死的 1384 / 2584 / 4984 与全部档案数字要改。母带插件的 +9.7 ms 由 DAW 全额补偿，只影响通过总线实时监听的场景。推翻 Max 已拍板的参数表文字 → **需拍板**。

**验证**：ChainTests 延迟用例改为「与前瞻无关、等于 max 前瞻公式」；脉冲实测；Lookahead 每块换档的自动化用例：输出有限、无宿主延迟变化、每次换档的静音 ≤ 一个限幅器延迟。

---

## 附录 A · 核实过、不报的项（给核验代理省时间）

1. **GLAZE 对模块统计的 NaN**：`GlazeGuard.cpp:186-190` `cleanDb / cleanFraction` 清洗；`ClayPressureMonitor::update` 非有限按 0；`MusicAnalyzer.cpp:963-964` 跳过；`GlazeCalibrator.cpp:50` 判有限。链在 `io.clear()` 之后才 `outputMeter.process`（`CeladonChain.cpp:257-263`）。无缺陷。
2. **无锁读取有界**：`ReferenceSpectrum::Publication::read` 有 `snapshotReadAttempts` 上限、失败返回默认快照（目标 EQ 那一块按无参考，2 s 平滑吃掉）；`GlazeController` 音频线程只 `tryEnter`（`:2706`、`:2736`）。无缺陷。
3. **限幅器 prepare 按新采样率重算前瞻**：`LimiterModule.cpp:279` `lookaheadSamples = lookaheadSamplesFor (params.lookaheadMs)` 再 `reset()`。`prepareToPlay` 先 `setParams` 后 `prepare` 的顺序（`PluginProcessor.cpp:404-423`）在采样率变化时不会留下过期前瞻。无缺陷。
4. **超过 prepare 块长的宿主块**：Balance / Color / Limit 按 `maxBlockSize` 切块（`BalanceModule.cpp` `process`、`ColorModule.cpp:755-766`、`LimiterModule.cpp:435-436`），Clay 逐样本 + 64 网格（`ClayModule.cpp` `process`），链的差值缓冲只在试听期间切片（`CeladonChain.cpp:203-206`），LoudnessMeter 不限块长。无缺陷。
5. **延迟求和与上报**：`getLatencySamples()` = 各模块之和，Lookahead 变更在 `setParams` 后立即反映；`restartWithFadeIn` 的静音保持 = 前三级延迟 + 限幅器自身延迟（`CeladonChain.cpp:81-84`、`LimiterModule.cpp:303-309`）；脉冲实测用例覆盖 3 采样率 × 3 前瞻。无缺陷（设计代价见 R10-09）。
6. **尾巴 0.35 s**：Clay 10 Hz 4 阶 ≈ 0.29 s、Balance 40 Hz Q 4.3 ≈ 0.24 s、Heat 5 Hz 一阶隔直 ≈ 0.22 s、线性相位低通 4(L−1) 已在延迟内、释放包络在静音下不产生输出。常数够用。
7. **单声道口径**：LoudnessMeter 单声道能量 ×2、Clay 只做①、Color 不生成 Side、分析器 `bandCross = bandPower`（ρ = 1 → 不收单，加宽对 S = 0 无作用）、ReleaseSafety / 差值试听支持 1 声道。没找到缺陷；链级没有单声道用例（`ChainTests.cpp:33` 固定 2 声道）——按任务口径「只在真有缺陷时报测试缺口」不列为结论，建议补一条「单声道链 == 双单声道链的左声道（容差 1e-6）」的性质测试。
8. **Guard 10 ms 切片**：48k / 44.1k 的 512 块每块切 2 片、各模块入口开销翻倍，是有意的块长无关设计，T-019 基准已同口径量过。不报。
9. **Listen ↔ 预听 ↔ 听原声的交接**：`CeladonChain.cpp:143-158`、`:370-381` 除 R10-03 外都有斜坡；只衰减、不抬高是 Max 定的口径。
10. **GLAZE 的分析取点**：入口分析在模块之前（`:234-235`），Balance 帧取自其入口 = 链入口，出口统计在 Listen / 预听 / 抖动之前（`:263`、`:286-296`），校准器按链延迟配对（`GlazeController.cpp:2530`）。位置正确。限幅器统计相对入口时隙晚约 24 ms（Clay D+G + Color 64），对 3 s 窗的保护层无影响。
11. **存档节点顺序**（参数先换、GLAZE 后交，`PluginProcessor.cpp:786-789` 有理由）与 `REFERENCE_SPECTRUM` 节点摘除：正确。
12. **构建选项**：`.jucer` Release `optimisation="3"`（-O3）+ LTO，Debug `"4"`（-O1）；未开 fast-math；源码守卫 `TruePeak.cpp:61-73` 会拦 `-ffast-math / /fp:fast / clang-cl 合并`。正确。
13. **Clip 级在 48 kHz 只有 −60 dB 混叠底**（本报告实验的副产物）：属 R1（Limit）范围，这里只记录，不作为 R10 结论。
14. **Balance 最低几段的 FFT 分辨率**（bin 23.4 Hz vs 40 Hz 段宽 9.3 Hz）：属 R2 范围，不在本报告。

## 附录 B · 实验（107，Python 3.14 + numpy 2.5.3，venv 与目录做完已删）

脚本 `oversampling_vs_fs.py`（原文）：

```python
# R10 实验：过采样倍率不随采样率缩放 → 高采样率下内部速率过高（无谓）。
# 方法：把非线性（Color Heat 的 Tube；Limit Clip 的 SoftClip）跑在「内部速率 R = fs_base × L」上，再理想抽取回 fs_base；
#      与「3.072 MHz 参照（视作连续时间）→ 理想抽取到 fs_base」比较，差值 = 只由内部速率决定的混叠误差。
#      理想抽取 = FFT 砖墙（不含半带 FIR 本身的通带纹波），所以量到的是纯粹的折返能量。
import numpy as np

T = 0.2            # 秒；所有频率取 5 Hz 的整数倍 → 整周期
F_REF = 3072000    # 参照速率（48k × 64）

def tones(fs, comps):
    n = int(round(fs * T)); t = np.arange(n) / fs
    return sum(a * np.sin(2*np.pi*f*t) for f, a in comps)

def tube(x, drive=2.0, asym=0.04):        # Heat 100：drive 1+h=2、asym 0.04h（ColorModule.h 文件头）
    return (np.tanh(drive*x + asym) - np.tanh(asym)) / (drive*(1 - np.tanh(asym)**2))

def softclip(x, threshold=0.891):         # Saturator::SoftClip：0.7T 以下线性，之后 tanh 拐点，渐近封顶 T（Ceiling −1 dBTP）
    knee = 0.7*threshold; width = threshold - knee
    m = np.abs(x)
    shaped = np.where(m <= knee, m, knee + width*np.tanh((m-knee)/width))
    return np.sign(x)*shaped

def decimate_ideal(y, fs_from, fs_to):
    n_from = len(y); n_to = int(round(fs_to * T))
    Y = np.fft.rfft(y)
    k = n_to//2 + 1                                   # 保留 |f| < fs_to/2 的 bin（5 Hz 分辨率，两边 bin 数一致）
    Yd = Y[:k].copy() * (n_to / n_from)
    return np.fft.irfft(Yd, n=n_to)

def alias_db(nl, comps, fs_base, L):
    R = fs_base * L
    y_ref = decimate_ideal(nl(tones(F_REF, comps)), F_REF, fs_base)
    y_R   = decimate_ideal(nl(tones(R, comps)), R, fs_base)
    e = y_R - y_ref
    E = np.fft.rfft(e); Yr = np.fft.rfft(y_ref); f = np.fft.rfftfreq(len(e), 1/fs_base)
    band = (f >= 20) & (f <= 20000)
    rms_e = np.sqrt(np.sum(np.abs(E[band])**2)); rms_y = np.sqrt(np.sum(np.abs(Yr[band])**2))
    peak_e = np.max(np.abs(E[band])); peak_y = np.max(np.abs(Yr))
    return 20*np.log10(rms_e/rms_y + 1e-300), 20*np.log10(peak_e/peak_y + 1e-300)

cases = [(48000,8),(96000,4),(192000,2),(96000,8),(192000,8),(192000,4),(192000,1),(48000,4),(96000,2)]
print("非线性 = Heat Tube（drive 2、asym 0.04），信号 = 15 kHz 0.5 + 19 kHz 0.5")
for fs, L in cases:
    r, p = alias_db(tube, [(15000,0.5),(19000,0.5)], fs, L)
    print(f"{fs:>8} {L:>3} {fs*L:>10} {r:>18.1f} {p:>14.1f}")
print("非线性 = Clip SoftClip（T = 0.891），信号 = 10 kHz 1.6")
for fs, L in [(48000,4),(96000,2),(192000,1),(192000,2),(192000,4),(96000,4),(48000,8)]:
    r, p = alias_db(softclip, [(10000,1.6)], fs, L)
    print(f"{fs:>8} {L:>3} {fs*L:>10} {r:>18.1f} {p:>14.1f}")
```

输出（原样）：

```
非线性 = Heat Tube（drive 2、asym 0.04），信号 = 15 kHz 0.5 + 19 kHz 0.5（峰值 1.0，最坏情况：Heat 支路实际只吃 150 Hz 高通后的残差、湿路 25%）
 fs_base   L       内部速率       带内混叠 rms(dB)       最大谱线(dB)
   48000   8     384000             -145.2         -147.6
   96000   4     384000             -145.2         -147.6
  192000   2     384000             -145.2         -147.6
   96000   8     768000             -259.4         -269.0
  192000   8    1536000             -265.9         -273.7
  192000   4     768000             -259.4         -269.0
  192000   1     192000              -70.6          -72.2
   48000   4     192000              -70.6          -72.2
   96000   2     192000              -70.6          -72.2

非线性 = Clip SoftClip（T = 0.891，Clip 100 → 阈值 = Ceiling），信号 = 10 kHz 1.6（推高 +5 dB 进拐点）
 fs_base   L       内部速率       带内混叠 rms(dB)       最大谱线(dB)
   48000   4     192000              -60.1          -62.0
   96000   2     192000              -60.1          -62.0
  192000   1     192000              -60.1          -62.0
  192000   2     384000              -83.8          -84.0
  192000   4     768000             -108.0         -109.1
   96000   4     384000              -83.8          -84.0
   48000   8     384000              -83.8          -84.0
```

读法：同一内部速率的三行逐位相同（−145.2 / −147.6；−60.1 / −62.0），说明混叠底只由内部速率决定；192k × 8 的 −266 dB 远低于 float 的 −150 dB 噪底，是纯浪费。理想抽取不含半带 FIR 的阻带（−75 / −70 dB），实际链的混叠底 = 两者取劣。

## 附录 C · 需要 Max 拍板的项

- R10-07：≥ 88.2 kHz 改过采样级数（改高采样率的声音、改「延迟与采样率无关」的规格文字）。
- R10-09：延迟改为按最大前瞻固定上报（+9.7 ms，推翻「Lookahead 决定延迟」）。
- R10-05(b)：NEON 逐位契约的自动门槛要一台非工作机的 Mac runner。
- R10-01 修法 1 的「出厂默认单一真源」不改声音，不需拍板；但它会让重跑的门槛成品包含 ADR-020 待复核的两项默认——重跑前最好先让 Max 定 ADR-020 的三条。

## 附录 D · 今晚落地顺序建议

R10-01（M）→ R10-03（S）→ R10-02（S）→ R10-04（S）→ R10-06（S）→ R10-08（S）→ R10-05(a)（S）；R10-07 / R10-09 等拍板。全部改完后 107 全套一次 + `CELADON_CXX_FLAGS=-march=x86-64-v3` 一次 + `celadon_render --selftest`。
