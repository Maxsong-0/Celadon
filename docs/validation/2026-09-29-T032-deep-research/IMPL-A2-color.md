# IMPL-A2-color · T-032 Color 染色实现报告

- 日期 2026-09-29（107 上的验证在 2026-09-30 06:0x ～ 06:4x UTC 完成）；代理 A2-color（Fable 5.1 · max）。
- 工作树 `/Users/maxsong/All-Project/Celadon/.claude/worktrees/wf_abb55518-7ae-2`，分支 `worktree-wf_abb55518-7ae-2`，自 main `e6b445b`（v0.1.8.5）。
- 任务书 `BRIEF-A2-color.md`；证据 `R3-color.md` / `V3-color.md` / `R8-chain-juce.md` / `V8-chain-juce.md`（同目录，在主工作树的 `38f0c42`）。
- 改的文件：`Source/dsp/ColorModule.{h,cpp}`、`Source/dsp/Saturator.{h,cpp}`、`tests/ColorTests.cpp`、`docs/reference-chain-algorithm.md`（Glue 段与验证段）、档案 `01-框架/设计规格-Color.md`（末尾追加「2026-09-29 修订（T-032）」，177 行）。没动 CMakeLists / scripts，没新建源文件。

## 逐条

### R3-01 · Glue 中频改用联动增益（confirmed，high / P1）——已做，改输出（只在 Glue 开着且 Δ ≠ 0 的样本）

- 做了什么（当晚 S 修法，按核验调整后的版本）：删掉 mid 检测器与其包络——`analyseGlueBands` 打包路径（`ColorModule.cpp:1094` 附近）与标量路径（`:1121` 附近）不再算 `midPeak`；`processGlue` 删 `midReductionDb` 推进、`midDb`、`midGain`，`useSplit = deltaLow != 0.0 || deltaHigh != 0.0`（`:1251`），中频样本乘联动 `gain`（`:1275`）；`ColorModule.h` 删 `glueMidPeaks` / `midReductionDb` 成员，`prepare` 的 `assign`、`reset` / `restartGlueFromOff` 的清零同步删；常数注释（`:65-70`）改写：中频直接用联动增益、`glueBandDeltaMinDb` 此后只在瞬态振铃触发（低 / 高检测器是 Butterworth，|H| ≤ 1，稳态读数不会比整段深）。头文件信号流注释同步。
- 没做（写进「待 Max」）：LR4 + 全通 / 一阶互补分裂、`glueBandDeltaMaxDb` 3.2 → 1.5。
- 新用例 `ColorTests`「glue applied gain tracks the linked static curve」：48 kHz、−6 dBFS 正弦 2 s，取第 141 ～ 186 块（46 × 512 ≈ 0.49 s）：输出 / 输入基波幅度（相干投影）与「补偿 − 按块长加权的 `getBlockStats().glueGrMeanDb`」之差；Glue 100 阈值 0.75 dB、Glue 40（GLAZE 100 对应值）0.30 dB；Glue 40 的 60 / 90 Hz 被侧链 100 Hz 高通挡在拐点下（联动 GR 0 / 0.12 dB），只核偏差不核「在压」。按核验建议没用 `getAndResetGainReductionDb`（窗口最大值，纹波把偏差读大）。
- 107 实测（改前 = 未改源码的同一用例；单位 dB，偏差 = 施加 − 联动曲线）：

| f (Hz) | 联动 GR | Glue 100 改前 | Glue 100 改后 | Glue 40 改前 | Glue 40 改后 |
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

  与研究（300 Hz −1.13 / 2.5 kHz −1.11；修法后最坏 −0.65）、核验（Glue 40 −0.62 ～ −0.64；修法后 ≤ 0.14）逐一吻合。改前该用例 12 项失败（含 Glue 40 的 2 项「在压」误判，已改）。
- 双音用例「glue compresses a loud mid tone more than a quieter low tone」保留、记录实际数字：改前 low +0.66 / mid −2.92，改后 **+0.68 / −2.59**（研究 mode 7 预测 +0.68 / −2.59）。低音保护不变，中频少挨了伪像多压的 0.33 dB。
- 逐位契约：Glue 关 / Δ = 0 由「glue 100 → 0 bit-exact」「idle skipping bit-identical」「stereo L = R == mono」「packed == scalar」等既有用例保住（全绿）；输出指纹里 Glue 关的三个场景改前改后逐位相同（见下）。
- 成本：每样本少 1 次 log10 + 1 次 exp。107 上 Color GLAZE 100 0.645 % → 0.570 %（默认 0.179 % → 0.165 %，同批参照 juce Oversampling 2.842 % → 2.819 %）。

### R3-04 · Heat 1× 热路径两处逐位不变的优化（confirmed，low / P2）——已做，逐位不变

- ① `prepareHeatInput` 立体声分支改 `StereoBiquad` 打包（Mid lane0 / Side lane1，`ColorModule.cpp:1332` 起）：M/S 先在 float 里算好（与标量同一表达式），再各自转 double 过同一组系数的双二阶，无跨通道运算——论证与 Tone 相同；单声道与 > 2 声道仍走标量，`setPackedStereoForTesting (false)` 仍走原标量 M/S 分支。
- ② `Saturator::processTubeResidualBlock`（`Saturator.cpp:270`，声明与说明 `Saturator.h:66`）：4 路里 `vSub (vMul (vSub (tanh4 (…), offset), inverse), x)`，与「processTubeBlock 后再逐样本减原样本」是同一 IEEE 单精度减法、同一对操作数；`ColorModule::processResidualBlock`（`:1390`）改调它，删 `heatResidualScratch` 成员与 `prepare` 里的 `assign`（8× 域每声道少一次拷贝与一遍循环）。`processTubeBlock` 保留（残差 makeup 标定与测试在用）。
- 新用例「Saturator::processTubeResidualBlock is bit-identical to processTubeBlock minus the input」：`saturatorTestInputs()`（±0、非规格化、±1e30、±Inf、NaN、−3 … 3 网格、随机）与 4096 随机块；整段与 1 / 2 / 3 / 5 / 7 / 13 切段；驱动量取现行 Heat 0 / 5 / 30 / 60 / 100 的 Mid 与 Side 映射 + 旧版 6.4 / 0.09；`memcmp` 全等。
- 逐位证据（新用例「output fingerprint」，FNV-1a 64 位，块长 333，类音乐 1.0 ～ 1.6 s；改前 = 未改源码的树）：

| 场景 | 改前 | 改后 |
|---|---|---|
| heat 100 / tone +2 / glue off | 2e251842f04bd3d1 | 2e251842f04bd3d1（相同） |
| heat 60 / tone −1.5 / mix 70 / out +3 / glue off | 51a73649a480409c | 51a73649a480409c（相同） |
| heat 0/60 every 0.4 s / glue off（预热、近似链） | 3af17f347e191a9e | 3af17f347e191a9e（相同） |
| glaze 100（tone 0.75 / glue 40 / heat 30） | e713502ae6a6bbcf | e34aed8dc331cfeb（R3-01 有意改变） |
| glue 100 / heat 0 | d3c34c41e8dc2285 | acf5739f2b62ba0c（R3-01 有意改变） |

  既有「stereo L = R == mono」「packed == scalar（L ≠ R，剧本含 Heat 30 / 60 / 100）」「idle skipping bit-identical」全绿，覆盖打包 M/S 高通与残差路径。

### R8-02 · `ColorModule::setParams` 拦非有限值（confirmed，medium / P2；只做 Color 部分）——已做，有限值逐位不变

- `clampOrDefault` 助手（`ColorModule.cpp:109`）：`std::isfinite ? jlimit (范围) : 默认`；`setParams`（`:622-627`）五个 float 字段各自用它，默认取 `Params {}`（`constexpr`），范围不变；`bypass` 照旧。`juce::jlimit` 对 NaN 两个比较都为假、原样返回，修前 Tone = NaN → 搁架 NaN → 链逐块清零重启；Output = NaN → `decibelsToGain` 返回 0 → 永久静音；Heat = NaN → `heatCompensationFor` 的 `(int) NaN` 越界读（基线上没敢把这条用例跑在旧源码，只按代码推断）。
- 新用例「non-finite parameters fall back to defaults and finite ones still clamp」：五个字段 × NaN / +Inf / −Inf × {prepare 前设，播放中途 setParams}——输出有限、非静音，并与「该字段取默认值」的模块逐位相同；越界有限值（Tone ±10、Glue 150 / −20、Heat 250 / −5、Mix 130 / −1、Output ±9）与钳到边界的模块逐位相同。
- Limit 的守卫归 A3、处理器 `readParams` 的第二道归 A4（任务书分工）；链级「ChainParams 每个 float 字段喂 NaN 与取默认值的链逐位相同」用例应由 A4 在 ChainTests 里做（我只做了 Color 模块级）。
- 有限值逐位不变：所有既有用例全绿 + 上表指纹（参数全有限）相同。

## 测试命令与数字（全部在 107，本机 Mac 未编未测）

远端步骤脚本（临时，已删）：`bash /home/maxsong/.cache/celadon-a2/run.sh <标签> [--only …]` = `cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" --target celadon_tests -j 3 && "$BUILD/celadon_tests" …`，由 `scripts/test-linux.sh bash /home/maxsong/.cache/celadon-a2/run.sh …` 调用（沙盒不让本机命令串里带 `$BUILD`，才落成脚本；`-j 3` 照任务书）。

| 阶段 | 命令 | 结果 |
|---|---|---|
| 基线（未改源码 + 指纹 / 施加增益两条新用例） | `… run.sh baseline --only ColorModule` | 编译警告 0；708 过 / 12 败（12 项全是新用例对旧实现的预期失败，见 R3-01 表） |
| 定向（改后） | `… run.sh directed --only ColorModule,Saturator` | 编译警告 0；**81563 过 / 0 败** |
| 全套（改后） | `… run.sh full` | 见下一行（跑完后补） |

全套：待补。

## 没做的与原因

- R3-01 跟进（LR4 + 全通三路分频 / 一阶互补分裂）与 `glueBandDeltaMaxDb` 3.2 → 1.5：任务书明确「写进待 Max」——都改听感（全通常驻或淡出瞬间的相位跳变；上限降到 1.5 把双音低音保护从 +0.68 砍到约 +0.3）。
- R3-02（4 路多项式 log / exp）、R3-03（Heat 过采样按采样率）、R3-05、R3-06：不在任务书清单。
- R8-02 的 Limit / 处理器部分：归 A3 / A4。

## 跨文件需求

- 无必须由别人改的文件。建议（可选）：A4 在 `tests/ChainTests.cpp` 加链级 NaN 参数用例时把 `color.*` 五个字段一起喂进去（Color 模块级已覆盖）。

## 档案待补（主进程）

- `02-日志/2026-09-29*.md`：T-032 A2 一段（本报告的「逐条」摘要 + 数字）。
- `05-版本/`：下一版版本档写「Glue 中频改用联动增益（改输出）、Heat 打包 M/S 高通 + 残差直接相减（逐位不变）、Color 参数守卫」。
- `04-协调/TASKS.md`：T-032 行进度；`GlazeConstants.h:151-153` Glue 实测常数列进「22 首门槛重跑时复核」。
- `00-总览/INDEX.md` 刷新。

## 待 Max

1. 分频施压的结构与常数（180 Hz / 5 kHz / 36 ms / 8 ms / +3.2 / −0.6）从没过耳朵；剩余的相位凹陷（Glue 100 最坏 −0.65 dB @ 2.5 kHz；高频比中频轻 12.8 dB 以上时 2 ～ 4 kHz 也会挖 0.5 ～ 0.7 dB，V3「漏掉的问题」3）要不要用 LR4 + 全通（每声道约 11 个双二阶、Glue 开时输出带固定全通、`splitIdle` 切回时需交叉淡化）或一阶互补分裂（无凹陷但选择性变差）消除，或先把 `glueBandDeltaMaxDb` 降到 1.5（凹陷 −0.34 dB，低音保护 +0.66 → 约 +0.3）——试听后定。
2. `GlazeConstants.h:151-153`（`glueLoudnessPerGrDb` 1.05 / `gluePeakPerGrDb` 0.95 / `glueP95OffsetDb` 1.5）是分频前量的，本次又改了同样联动 GR 下的实际响度损失：22 首门槛重跑时复核（GLAZE 规划器直接用它们推响度目标）。
3. Mac 整插件性能验收（ADR-007）与 22 首门槛 / A/B 本次都没跑（合并后主进程统一）。

## 清理

- 107：临时脚本目录 `~/.cache/celadon-a2/` 已删；本工作树的远端同步目录 `~/.cache/celadon-tests/wf_abb55518-7ae-2-32cda3ef5055` 与构建目录 `~/.cache/celadon-build/wf_abb55518-7ae-2-32cda3ef5055` 已删（合并后主进程在自己的工作树重跑）。
- 本机：只用了会话 scratchpad（`…/scratchpad/a2/*.log`），工作树里没有临时文件。
