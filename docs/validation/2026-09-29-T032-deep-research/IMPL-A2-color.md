# IMPL-A2-color · T-032 Color 染色实现报告

- 日期：2026-09-29 起草（Fable 5.1 · max，额度中断，半成品由主进程保全为 WIP `c32cd07`）；2026-09-30 接续审查、复测、收尾（Opus 5.5 · max）。本轮 107 上的全部复测在 2026-09-30 14:4x ～ 14:59 UTC。
- 工作树 `/Users/maxsong/All-Project/Celadon/.claude/worktrees/wf_abb55518-7ae-2`，分支 `worktree-wf_abb55518-7ae-2`，底 = main `e6b445b`（v0.1.8.5）。提交链：`c32cd07`（前任 WIP）→ `9e42106`（接续审查修订 WIP）→ 收尾提交（本报告随它提交）。
- 任务书 `BRIEF-A2-color.md`；证据 `R3-color.md` / `V3-color.md` / `R8-chain-juce.md` / `V8-chain-juce.md`（同目录，在主工作树的 `38f0c42`）。
- 改的文件：`Source/dsp/ColorModule.{h,cpp}`、`Source/dsp/Saturator.{h,cpp}`、`tests/ColorTests.cpp`、`docs/reference-chain-algorithm.md`（Glue 段 :50 起、验证段 :84 起）、本报告；档案 `01-框架/设计规格-Color.md` 末尾「2026-09-29 修订（T-032）」一节（全文 177 行）。没动 CMakeLists / scripts，没新建源文件。

## 接续审查：前任改动的结论与改正

逐条对照任务书「核验调整后的修法」审了 `c32cd07` 的全部 diff：三条的代码都照任务书做了，逐位论证成立，没有功能错误；按最终用例复跑改前基线，两条新用例都能抓到缺陷（见「测试」）。改正 / 补齐的是：

1. **常数注释说过头**（`ColorModule.cpp:66` 起）。原写「低 / 高两路检测器是 Butterworth（|H| ≤ 1），稳态读数不会比整段深，所以 glueBandDeltaMinDb 只在瞬态振铃时触发」——这是研究报告原话，核验没驳，但只对正弦成立。107 上按源码的 RBJ 系数与转置 II 型用 numpy 移植核对（侧链 100 Hz 高通后再过 180 Hz 低通 / 5 kHz 高通，取 1 ～ 2 s 稳态峰值，相对整段侧链峰值）：1 kHz 锯齿的高频路 **+0.75 dB**、1 kHz 方波 +0.21 dB；50 / 100 Hz 方波、200 Hz 锯齿与 60 Hz / 1 kHz / 10 kHz 正弦都 ≤ 0 dB（正弦按 |H| ≤ 1 本就不会超过）。改成准确说法：稳态正弦不会比整段高，Δ 只因攻击时间不同略有出入；Δ < 0 要本路峰值高过整段（瞬态、振铃、陡边沿）；并补上剩余相位凹陷的说明。档案规格同步写了这一句。
2. **同类漏改**：`processGlue` 里「联动峰值与三路峰值共用这一条静态曲线」（现 `:1199`）改为低 / 高两路。
3. **注释跟进**：`ColorModule.h` 头注释「Tube 整段调用 processTubeBlock」改为残差版 `processTubeResidualBlock`（与「两声道打包」一句拆开）；`ColorModule.cpp` 残差 makeup 标定处、`Saturator.h` 成员注释补上 `processTubeResidualBlock`；`ColorTests`「packed == scalar」用例注释补 Heat M/S 高通。
4. **措辞不准**：用例注释与档案规格里「中频包络比联动深约 1 dB、再被 −0.6 dB 钳位」——核验 V3 已指出 Glue 100 纯音 Δmid ≈ −0.53 dB 未到钳位、Glue 40 才钉在钳位，两处都改；用例注释「剩下的偏差只来自分裂相位凹陷」不全（60 ～ 150 Hz 的 +0.1 dB 来自低频路攻击 36 ms 比联动 20 ms 慢、包络略轻），改准；规格补「±Inf 原来被 `jlimit` 钳到边界，现在同 NaN 回默认」；性能数字换成本轮同批实测。
5. **任务书「保留双音用例并记录实际数字」落到用例注释**：`testMultibandGlueLetsLowSitAboveMid` 头上原先挂的是「验收 4」的注释（`2de9024`，2026-09-28 插入双音用例时错位），归位到 `testGlueGainReduction`，双音用例换成自己的说明并写上改前改后实测数字。只动注释。
6. **前任报告与事实不符两处**：「基线 12 项失败」是它改用例前的数（按最终用例，施加增益用例在改前是 10 项失败）；「远端同步 / 构建目录已删」——两个目录其实都在（全套被额度中断，没跑完）。本轮复用后在「清理」里删除。
7. **全套从没跑完**：本轮在最终提交态上重编、跑完全套；另补一组「中间态」（只含 R3-01）对照，把 R3-04 / R8-02 的逐位不变证据补到 Glue 开的场景。

环境备注：本代理启动时会话工作树被钩子认作 `fable-deep-research-634a5e`，编辑本工作树被拦；用 `EnterWorktree` 的 path 模式把本代理切进 `wf_abb55518-7ae-2` 后再改（只影响本代理），没碰其他工作树。

## 逐条

### R3-01 · Glue 中频改用联动增益（confirmed，high / P1）——已做，改输出（只在 Glue 开且 Δ ≠ 0 的样本）

- 做了什么（当晚 S 修法）：删中频检测器与其包络——`analyseGlueBands` 打包路径（`ColorModule.cpp:1101` 附近）与标量路径不再算 `midPeak`；`processGlue` 删 `midReductionDb` 推进与 `midDb` / `midGain`，`useSplit = deltaLow != 0.0 || deltaHigh != 0.0`（`:1258`），中频样本乘联动 `gain`（`:1283`）；`lowGain` / `highGain` 在各自 Δ = 0 时复用 `gain`（与原「! useSplit || Δ == 0」等价：现在 `! useSplit` 蕴含两个 Δ 都为 0）。`ColorModule.h` 删 `glueMidPeaks` / `midReductionDb`，`prepare` 的 `assign`、`reset` / `restartGlueFromOff` 的清零同步删。常数注释、头文件信号流注释、`docs/reference-chain-algorithm.md:50,52,85`、档案规格同步。
- 新用例「glue applied gain tracks the linked static curve」（`tests/ColorTests.cpp:2500`）：48 kHz、−6 dBFS 正弦 {60, 90, 150, 229, 300, 500, 1000, 2500, 3969, 7000, 10000} Hz 各 2 s，取第 141 ～ 186 块（× 512，约 1.50 ～ 1.99 s）：输出 / 输入基波幅度（相干投影）与「补偿 − 按块长加权的 `getBlockStats().glueGrMeanDb`」之差。按核验不用 `getAndResetGainReductionDb`（窗口最大值）。门槛 Glue 100 0.75 dB（任务书）、Glue 40（GLAZE 100 对应值）0.30 dB；Glue 40 下 60 / 90 Hz 被侧链 100 Hz 高通挡在拐点下（联动 GR 0 / 0.12 dB），只核偏差，≥ 150 Hz 另断言联动在压（> 0.5 dB）。
- 107 实测（偏差 = 施加 − 联动曲线，dB；改前 = `c32cd07~1` 源码跑同一用例）：

| f (Hz) | 联动 GR（G100 / G40） | Glue 100 改前 | Glue 100 改后 | Glue 40 改前 | Glue 40 改后 |
|---:|---:|---:|---:|---:|---:|
| 60 | 3.81 / 0.00 | +0.329 | +0.120 | +0.014 | +0.014 |
| 90 | 6.55 / 0.12 | +0.355 | +0.146 | +0.073 | +0.054 |
| 150 | 8.15 / 0.96 | −0.082 | +0.100 | −0.100 | +0.049 |
| 229 | 8.46 / 1.24 | −0.833 | −0.237 | −0.617 | −0.095 |
| 300 | 8.51 / 1.29 | **−1.142** | −0.566 | −0.632 | **−0.149** |
| 500 | 8.53 / 1.31 | −0.774 | −0.481 | −0.328 | −0.090 |
| 1000 | 8.53 / 1.32 | −0.414 | −0.260 | −0.169 | −0.042 |
| 2500 | 8.53 / 1.31 | −1.107 | **−0.645** | −0.495 | −0.119 |
| 3969 | 8.53 / 1.31 | −0.824 | −0.223 | **−0.637** | −0.111 |
| 7000 | 8.53 / 1.32 | +0.076 | +0.050 | +0.065 | +0.043 |
| 10000 | 8.54 / 1.31 | +0.206 | −0.041 | +0.097 | −0.041 |

  与研究（300 Hz −1.13、2.5 kHz −1.11；修后最坏 −0.65）、核验（Glue 40 −0.62 ～ −0.64；修后 ≤ 0.14）一致。改前该用例 10 项失败（两档各 229 / 300 / 500 / 2500 / 3969 Hz），改后 0。
- 双音用例保留，实际数字：改前 low +0.66 / mid −2.92 dB，改后 **+0.68 / −2.59 dB**（研究 mode 7 预测相同）——低音保护不变，中频少挨了伪像多压的约 0.33 dB。验收 4（1 kHz −6 dBFS 的 GR 表）改前改后都是 8.54 dB。
- 逐位契约：Glue 关 / Δ = 0 由「glue 100 → 0 bit-exact」「idle skipping bit-identical」「stereo L = R == mono」「packed == scalar」等既有用例保住（全绿）；指纹里 Glue 关的 3 个场景改前改后逐位相同（见「逐位证据」）。
- 成本：每样本少 1 次 log10 + 1 次 exp。107 同批实测 Color GLAZE 100 0.641 % → 0.571 %（最终态复跑 0.579 %）、默认 0.177 % → 0.165 %；参照 juce Oversampling 2.848 % → 2.823 %，与参照之比 GLAZE 100 0.225 → 0.202。共享服务器，只看量级。

### R3-04 · Heat 1× 热路径两处逐位不变的优化（confirmed，low / P2）——已做，逐位不变

- ① `prepareHeatInput` 立体声分支改 `StereoBiquad` 打包（`ColorModule.cpp:1339` 起，Mid lane0 / Side lane1）：M/S 先在 float 里按原表达式算好，再各自转 double 过同一组系数的双二阶，无跨通道运算（论证同 Tone）；单声道、> 2 声道与 `setPackedStereoForTesting (false)` 仍走原标量分支。
- ② `Saturator::processTubeResidualBlock`（`Saturator.cpp:270`，声明与说明 `Saturator.h:62-66`）：4 路里 `vSub (vMul (vSub (tanh4 (…), offset), inverse), x)`——`Tube (x)` 先舍入成 float 再减同一个原样本，与「processTubeBlock 后逐样本 `data[i] -= 原样本`」同一 IEEE 单精度运算、同一对操作数；尾巴补 0 的 lane 得 0、丢弃。`ColorModule::processResidualBlock`（`:1397`）改调它，删 `heatResidualScratch` 成员与 `prepare` 里的 `assign`（8× 域每声道少一次拷贝与一遍循环；三处调用点：分段整形、近似链、预热全走它）。`processTubeBlock` 保留（残差 makeup 标定与测试在用）。
- 新用例「Saturator::processTubeResidualBlock is bit-identical to processTubeBlock minus the input」（`:2591`）：`saturatorTestInputs()`（±0、非规格化、±1e30、±Inf、NaN、−3 … 3 网格、随机）与 4096 随机块；整段与 1 / 2 / 3 / 5 / 7 / 13 切段；驱动取现行 Heat 0 / 5 / 30 / 60 / 100 的 Mid 与 Side 映射 + 旧版 6.4 / 0.09；`memcmp` 全等。
- 逐位证据见下节：改前 → 最终，Glue 关 3 个场景指纹相同；中间态（只含 R3-01）→ 最终，5 个场景（含 Glue 开）指纹全同。既有「stereo L = R == mono」「packed == scalar（L ≠ R，剧本含 Glue 开、Heat 60 / 100）」也在同一构建里直接拿打包 M/S 高通对照原标量写法。
- 平台说明：本轮只在 x86_64（107，SSE2 打包）实测。ARM 上与 Tone 同一前提（同一表达式同样融合：标量 b0·x + s1 融合、打包写 vfmaq；M/S 的 float 表达式两边一字不差），由上述两条既有用例覆盖，要在 Mac 上跑到才算实测。

### R8-02 · `ColorModule::setParams` 拦非有限值（confirmed，medium / P2；只做 Color 部分）——已做，有限值逐位不变

- `clampOrDefault`（`ColorModule.cpp:115`）：`std::isfinite ? jlimit (范围) : 默认`；`setParams`（`:623` 起）五个 float 字段各自用它，默认取 `constexpr Params {}`，范围不变，`bypass` 照旧。行为变化只在非有限值：NaN 原来原样放行，±Inf 原来被 `jlimit` 钳到边界，现在都回默认（任务书要求）。
- 新用例「non-finite parameters fall back to defaults and finite ones still clamp」（`:2660`）：五个字段 × NaN / +Inf / −Inf × {prepare 前设、播放中途 setParams}——输出有限、非静音，并与「该字段取默认值」的模块逐位相同；越界有限值（Tone ±10、Glue 150 / −20、Heat 250 / −5、Mix 130 / −1、Output ±9）与钳到边界的模块逐位相同。
- 改前基线上该用例 27 项失败（基线变体跳过 Heat = NaN：旧代码 `heatCompensationFor` 的 `(int) NaN` 越界读，会崩）：Tone = NaN、Mix = NaN 输出非有限，Output = NaN 静音，±Inf 被钳到边界而非默认（边界恰是默认值的 Glue −Inf、Heat −Inf、Mix +Inf 三项除外）；Glue = NaN 在旧代码上碰巧等同关掉，没失败。改后 0 失败。
- 有限值逐位不变：所有既有用例全绿 + 中间态 → 最终 5 个指纹全同。
- Limit 的守卫归 A3、处理器 `readParams` 第二道与链级「ChainParams 每个 float 字段喂 NaN / ±Inf」用例归 A4（任务书分工）。

## 测试命令与数字（全部在 107，本机 Mac 未编未测）

命令（工作树脚本，远端目录按工作树路径分；构建一律 `-j 3`）：

    scripts/test-linux.sh bash -c 'cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests" [--only 类名,类名]'

| 阶段 | 源码 | 范围 | 编译警告 | 结果 |
|---|---|---|---:|---|
| 基线 | `c32cd07~1` 的四个源文件；测试 = 最终 ColorTests 去掉残差用例（旧代码没这个 API）、NaN 用例跳过 Heat = NaN | `--only ColorModule` | 0 | 758 过 / 37 败——全是新用例对旧实现的预期失败（施加增益 10、非有限参数 27） |
| 定向 | 最终（`9e42106`） | `--only ColorModule,Saturator` | 0 | 81563 过 / 0 败 |
| 全套 | 同上 | 全部 | （同一构建） | 133176 过 / 0 败 |
| 中间态 | 最终撤掉 R3-04 / R8-02（= 改前 + R3-01）；测试同基线变体 | `--only ColorModule` | – | 只取指纹与数字；27 项失败 = 非有限参数用例对旧 `setParams`，预期 |
| **最终全套** | **收尾提交态**（比 `9e42106` 只多测试注释归位） | 全部 | **0**（重编 18 步） | **133176 过 / 0 败** |

注：全套日志里有 7 行含「warning:」，是 ClayGuard 用例名「phase warning: …」，不是编译警告（按 `文件:行: warning:` 计为 0）。

## 逐位证据（ColorTests「output fingerprint」，FNV-1a 64 位，类音乐 1.0 ～ 1.6 s，块长 333）

| 场景 | 改前 `c32cd07~1` | 中间态（只 R3-01） | 最终 |
|---|---|---|---|
| heat 100 / tone +2 / glue off | 2e251842f04bd3d1 | 2e251842f04bd3d1 | 2e251842f04bd3d1 |
| heat 60 / tone −1.5 / mix 70 / out +3 / glue off | 51a73649a480409c | 51a73649a480409c | 51a73649a480409c |
| heat 0/60 every 0.4 s / glue off（预热、近似链） | 3af17f347e191a9e | 3af17f347e191a9e | 3af17f347e191a9e |
| glaze 100（tone 0.75 / glue 40 / heat 30） | e713502ae6a6bbcf | e34aed8dc331cfeb | e34aed8dc331cfeb |
| glue 100 / heat 0 | d3c34c41e8dc2285 | acf5739f2b62ba0c | acf5739f2b62ba0c |

- 改前 → 中间态：只有 Glue 开的两个场景变——R3-01 只改 Glue 开时的输出，Glue 关逐位不变。
- 中间态 → 最终：五个场景全同——R3-04（打包 M/S 高通、残差直接相减）与 R8-02（有限值）在 Glue 开 / 关、Heat 开 / 关 / 切换（预热、近似链、分段整形三处残差调用都走到）下都没改任何一位。
- 施加增益表与双音数字在中间态与最终完全相同，同样说明输出变化全部来自 R3-01。

## 没做的与原因

- R3-01 跟进（LR4 + 全通三路分频 / 一阶互补分裂）与 `glueBandDeltaMaxDb` 3.2 → 1.5：任务书明确「写进待 Max」——都改听感。
- R3-02 / R3-03 / R3-05 / R3-06：不在任务书清单。
- R8-02 的 Limit / 处理器部分：归 A3 / A4。

## 跨文件需求

- 无必须由别人改的文件。建议：A4 在 `tests/ChainTests.cpp` 做链级 NaN / ±Inf 参数用例时把 `color.*` 五个字段一起喂（模块级已覆盖）；注意 Color 对 ±Inf 现在回默认，不再钳到边界。

## 档案待补（主进程）

- 规格已改：`01-框架/设计规格-Color.md` 末尾「2026-09-29 修订（T-032）」（本轮复核数字、改准措辞、补中间态证据与 ±Inf 行为）。
- `02-日志/2026-09-30.md`：T-032 A2 一段（本报告「逐条」摘要 + 数字；接续审查改正的 7 点）。
- `05-版本/`：下一版版本档写「Glue 中频改用联动增益（Glue 开时改输出）、Heat 打包 M/S 高通 + 残差直接相减（逐位不变）、Color 参数非有限值守卫（有限值逐位不变）」。
- `04-协调/TASKS.md`：T-032 A2 行进度；`GlazeConstants.h:151-153` Glue 实测常数列进「22 首门槛重跑时复核」；「分频施压保留与否 / 常数取多少」列为待 Max。
- `00-总览/INDEX.md` 刷新。

## 待 Max

1. 分频施压整组常数（180 Hz / 5 kHz / 36 ms / 8 ms / +3.2 / −0.6）从没过耳朵。剩余的相位凹陷：−6 dBFS 纯音 Glue 100 最坏 −0.65 dB（2.5 kHz），且凡高频比中频轻 12.8 dB 以上，2 ～ 4 kHz 都会挖 0.5 ～ 0.7 dB（V3 漏项 3：150 Hz −8 + 2.5 kHz −16 的低音主导双音修后仍是 −1.72 / −2.61 dB，更轻的中频反被压得比响的低音深）。三条路要选：LR4 + 全通（每声道约 11 个双二阶、Glue 开时输出带固定全通、`splitIdle` 切回原信号时要交叉淡化或全通常驻）、一阶互补分裂（无凹陷但选择性变差）、或先把 `glueBandDeltaMaxDb` 降到 1.5（凹陷约 −0.34 dB，双音低音保护 +0.68 → 约 +0.3）——试听后定。
2. `GlazeConstants.h:151-153`（`glueLoudnessPerGrDb` 1.05 / `gluePeakPerGrDb` 0.95 / `glueP95OffsetDb` 1.5）是分频前量的，分频施压与本次 R3-01 都改了同样联动 GR 下的实际响度损失：22 首门槛重跑时复核（GLAZE 规划器直接用它们推响度目标）。
3. Mac 整插件性能验收（ADR-007）、ARM 上的打包逐位实测、22 首门槛 / 等响试听本次都没跑（合并后由主进程统一）。

## 清理

- 107：数值核对脚本 `~/.cache/celadon-t032/a2-peaks.py` 跑完即删；本工作树的远端同步目录 `~/.cache/celadon-tests/wf_abb55518-7ae-2-32cda3ef5055` 与构建目录 `~/.cache/celadon-build/wf_abb55518-7ae-2-32cda3ef5055` 在收尾提交后删除（合并后主进程在自己的工作树重跑）。没动别人的进程与目录。
- 本机：只用了会话 scratchpad（基线测试变体、中间态源码、日志），收尾后删除；基线 / 中间态两次临时换入的源码都已按 `git checkout HEAD --` 与最终副本复原，并核对 `git status` 只剩本次要提交的改动。
