# T-097 守护族按规格最小修 · 验收报告

> 分支 `claude/t097-guards-by-spec`，对照 main `4be946b`（2026-10-23 并入 main 后的同机基线；开工基线 `3a7a367` 见 [BASELINE.md](BASELINE.md)）。
> 机器：2026-10-24 起单元测试先交 108（x86_64、GCC 15.2、Release，经 testbox 的节点 x86-1；JUCE 在仓库外，打包时和源码拼进同一个临时 git 仓库带进沙盒，只带 `LICENSE.md` 与 `modules/`），再在 DGX max-spark（aarch64、GCC 13、Release，全机锁 `~/.cache/celadon-spark/bin/spark-test.sh`）跑全量。在这之前的测试，以及 20 首、House、CPU，都在 DGX。
> 规格：vault《设计规格-实时GLAZE-主段与增益》第 4 节与修订注 (a)～(e)、(a′)；决定：ADR-036；计划：vault `04-协调/2026-10-11-T-097-守护族按规格修计划.md`。

## 0. 结论

- 代码按规格与 Max 的拍板（Q2～Q16）写完，定向测试、20 首、CPU、闭环用例都过。
- **有一项不满足完成标准，是已知的，Max 定了先合**：House 合成曲「从第 33 小节起播、再从头导出」的 6 行里新增 5 处失败（③ 落差 4 处、② GR 预算 1 处）。根因是规格内的 G_trim 慢积分（长记忆主段电平 + ±3 dB 钳位）在这个场景里横跨整首慢滑约 6 dB；main 在同场景也滑约 3 dB，只是方向相反，③ 碰巧过。T-097 的实现没有偏离规格（见第 4 节），所以没有绕过去改。Max 2026-10-24 定 A：「House 走 A：现在就合，G_trim 另外修，作为 0.1.8.11 发版前必须过的一关」——这 5 处不挡这次合并，归 G_trim 条件积分（R5-03b）去修；修好、这 6 行对 main 不新增失败，是 0.1.8.11 的发版门槛（vault TASKS 待认领、ADR-036「后果」）。
- 状态：已实际验证（全量 108 135589 / 0、DGX 135331 / 5 都没有新增失败；第 1、2 轮复查与外部代理第二意见已改完，第 3 轮没有独立复查、主会话自查；House 那 5 处已知失败 Max 定先合、归 G_trim 条件积分）。

## 1. 完成标准对照

| 完成标准（计划第 2 节） | 结果 | 依据 |
|---|---|---|
| G_trim 冻结（任一守门退让 < 0 / 本步刚改退让 / 1 s 内改参数，位模式不变；全部放回且 1 s 没动后恢复） | 过 | `GlazeControllerGuardFamily`（预算环推热、改参数 1 s；预算环放回 +0 的那一块仍冻结、下一块恢复积分，复查 R2-F4）；`GlazeControllerGuardScenarios` 两个闭环场景里上一块结束时已有落差 / PSR 退让的块（块长 480 那遍连起退那一块）逐位不变（复查 F4 后改成断言）。前提是 G_trim 离 ±3 dB 钳位远（R2-F10）：PSR 10331 / 10332 块；落差两轮 G_trim 在第一次退让前就顶到 +3，另把它改成 +0 读回再跑 10 s，938 / 938 块 |
| PSR 闭环（Q9：首次退让 20 s 后输出 3 s PSR ≥ F − 0.5 的占比 ≥ 95 %、最低 ≥ F − 1.0） | 过：96.3 %，最低 6.49 dB（流媒体 F = 7） | 第 7 节；改前（`4be946b` + 同一场景）守门不动、0.0 %、最低 4.98 dB |
| PSR 回程回差（≥ 门槛 + 0.5 满 5 s 才以 0.15 dB/s 回） | 过 | `GuardFamilyTests`「between the trip and trip + 0.5」「at or above trip + 0.5 for 5 s」 |
| 落差守门两轮（Q10 / Q16：12 段、最后一对 breakdown → drop ≤ 1.5 LU；② 预算放宽 ×4） | 过：① 1.35 LU、② 0.81 LU | 第 7 节；改前（`4be946b` + 同一场景）① 3.68 LU、② 3.36 LU，② 预算环没退 |
| 按曲风开关（通用两守门恒 0；House 会动；House → 通用以 0.05 dB/s 放回、不跳变） | 过 | 落差用例末尾（G_trim 归零读回那 10 s 之后）：−3.800 → −2.800 dB / 20 s，200 步、最大一步 0.005 dB |
| 块长无关（Q12 两层） | 过 | 纯逻辑同一串子块逐位；真链块长 {1, 7, 32, 64, 480, 512, 4096} 子块数与终点一致、每子块只推进一次 |
| 旧档兼容（读 0.1.8.10 的 LIVE 不冷启动；新档往返逐位；新档写占位给 0.1.8.10） | 过 | `GuardFamily` 存读三组与界那一组的存档段；`GlazeControllerGuardFamily` 旧 LIVE 读回（读档后下限 −24）；新档写给 0.1.8.10 的两项按它的不变量写：floorDb 恒 −3、reliefDb 写 max(−3, 退让)，真实退让写 reliefDbExact（复查 F11、第 2 轮 R2-F1）。0.1.8.10 降级打开时底一直是 −3，退让最多 3 dB（比 −3 深的在载入那一刻变浅）；退让 −5 / −4 时旧字段 −3 / −3、reliefDbExact −5 / −4、往返逐位，去掉 reliefDbExact 按 reliefDb 读成 −3 |
| ADR-034 A 组：GLAZE 0 逐位、真峰值 ≤ Ceiling + 0.01、0 分配 0 锁 | 过 | 20 首 GLAZE 0 的 40 行音频哈希逐字相同；真峰值最高 −1.35 dBTP；never-allocate 用例过 |
| ADR-034 A 组：22 首 gate_check + house_gate 对 main 同机不新增失败 | **20 首过；House 新增 5 处：已知，Max 定先合，归 G_trim 条件积分（0.1.8.11 发版门槛）** | 第 3、4 节 |
| CPU：只报 DGX 48 k / 32 最慢块对 main 的变化 | 报了：快 0.11～0.14 个百分点 | 第 6 节 |
| ADR-034 B 组对照表 | 填了 B1、B2；其余「工具未建（R5-01 未立项）」 | 第 5 节 |
| 全量测试只算新增失败（2026-10-24 起先 108、再 DGX） | 过：108 135589 / 0、DGX 135331 / 5，都没有新增失败 | main `4be946b` 基线：108 134977 过 / 0 败；DGX 134719 过 / 5 败（aarch64 已知） |
| 更新日志草稿（ADR-035，进 0.1.8.11） | 草稿在第 9 节 | — |
| 档案（规格修订注、接口草图、ADR-036、TASKS / 日志 / INDEX） | 规格与 ADR 已改；收尾项待合并后做 | vault |
| 独立复查通过后才说「代码改完」 | 第 1、2 轮已处理；第 3 轮**没有独立复查**（Max 2026-10-24 定不开 Workflow，主会话对照完成标准自查） | 第 8 节 |

## 2. 单元测试

| 跑什么 | 代码 | 结果 |
|---|---|---|
| DGX main 基线全量 | `4be946b` | 134719 过 / 5 败（4 条 ColorModule NEON 融合乘加 + 1 条 BalancePerformance 计时，aarch64 已知，同 `3a7a367`） |
| 定向（T-097 三组 + Phase2Orchestrator、GlazeController、GlazeChain、CeladonChain、CeladonChainGuards、LegacyFixture、ChainDigest、GlazeStatusText、GlazeControllerHouse） | `31c6083` | 1702 过 / 0 败 |
| 定向，复查第 1 轮改完后（同上 12 组，T-097 三组改名 `GuardFamily`、`GlazeControllerGuardFamily`、`GlazeControllerGuardScenarios`）：`spark-test.sh t097-merged test --only GuardFamily,GlazeControllerGuardFamily,GlazeControllerGuardScenarios,Phase2Orchestrator,GlazeController,GlazeChain,CeladonChain,CeladonChainGuards,LegacyFixture,ChainDigest,GlazeStatusText,GlazeControllerHouse` | 第 1 轮修正 | 1723 过 / 0 败；闭环场景冻结断言：PSR 10332 块、落差 ① 24696 块 / ② 24734 块有退让，0 块没冻结、0 块 G_trim 动过 |
| 108 main 基线全量（x86_64，testbox） | `4be946b` | 134977 过 / 0 败（没有 aarch64 那 5 条）；编译警告只有 `MusicAnalyzerTests.cpp` 4 条未用函数 |
| 108 定向，第 2 轮改完后（同上 12 组） | 第 2 轮处理 | 第 1 次 1779 过 / 11 败，全在落差回差带用例：夹具只改了 reliefDb 去注入退让，读档改读 reliefDbExact 后夹具没跟上（夹具两处补 reliefDbExact，代码不动）；外部审查后又改两次，第 4 次 1850 过 / 0 败，编译 0 新警告 |
| 108 改前证据（`4be946b` + 第 2 轮改后的场景段，只换差异块；副本 3、4 不提交） | — | 两次都 15 过 / 0 败，数字与 DGX 那次相同 |
| 108 全量（x86_64，testbox） | `cf4dff9` | 135589 过 / 0 败；对 108 基线 134977 / 0 没有新增失败（多出的 612 条是本任务新加的检查）；编译警告同基线（`MusicAnalyzerTests.cpp` 4 条未用函数），0 新增 |
| DGX 全量（aarch64，`spark-test.sh`，同 134719 / 5 那份基线的入口） | `cf4dff9` | 135331 过 / 5 败：5 条就是基线那 5 条（ColorModule NEON 融合乘加 4 条 + BalancePerformance 计时 1 条，aarch64 已知），没有新增失败；编译警告同基线 |

## 3. 20 首（通用 / 标准，GLAZE 0 / 35 / 70 / 100 × CLAY auto / off）

- `gate_check.py`：main 与 T-097 都是 FAILS 93，逐条相同（72 条是阶段 2 指标表没有 confidence 列的工具口径问题，21 条是反相提示误触发，都与守门无关，见 BASELINE.md）。没有新增。
- GLAZE 0：40 行音频哈希逐字相同。
- GLAZE > 0（每档 60 行）：响度变化均值 +0.014 / −0.005 / −0.007 LU（35 / 70 / 100），范围 −0.35 ～ +0.95 LU；真峰值一行都没变，最高 −1.35 dBTP。变化最大的是 emmmayiyu GLAZE 100（+0.80 ～ +0.95 LU）：通用档 PSR 守门出厂关掉后，以前读入口误退的那部分不再退（ADR-036「后果」第 1 条，Max Q2 A 已认）。

## 4. House 合成曲（Deep，GLAZE 100，标准 / 流媒体 / 俱乐部 × 起播第 1 / 33 小节）

`make_synthetic_house.py` 两首（house-124、tech-124；intro 1–32、drop1 33–64、breakdown 65–80、drop2 81–112 小节，124 BPM），`celadon_render --listen-from-bar 1,33 --segments-csv --metrics-only --no-replay` + `house_gate.py`。总数：main 15 处失败 → T-097 20 处。

| 歌 | 交付 | 起播小节 | ② GR 预算（drop1 / drop2 均值 dB）main → T-097 | ③ 落差损失 LU（正 = 变小）main → T-097 | ⑤ drop1 / drop2 与实际目标之差 LU：main → T-097 |
|---|---|---|---|---|---|
| house-124 | club | 1 | PASS（0.27 / 0.16）→ PASS（0.52 / 0.16） | −0.09 PASS → −0.07 PASS | −1.24 / −1.75 → −0.54 / −1.76 |
| house-124 | club | 33 | PASS（0.17 / 0.95）→ PASS（1.81 / 0.25） | −0.94 PASS → +2.06 FAIL **新增失败** | −1.69 / +0.21 → +1.01 / −1.29 |
| house-124 | standard | 1 | PASS（0.49 / 0.14）→ PASS（0.37 / 0.14） | +0.23 PASS → −0.01 PASS | — |
| house-124 | standard | 33 | PASS（0.20 / 1.36）→ PASS（1.77 / 0.24） | −0.46 PASS → +2.11 FAIL **新增失败** | — |
| house-124 | streaming | 1 | PASS（0.37 / 0.14）→ PASS（0.30 / 0.14） | −0.00 PASS → −0.11 PASS | −0.78 / −1.95 → −1.02 / −1.93 |
| house-124 | streaming | 33 | PASS（0.19 / 1.36）→ PASS（1.73 / 0.23） | −0.46 PASS → +2.05 FAIL **新增失败** | −1.53 / +0.67 → +0.98 / −1.39 |
| tech-124 | club | 1 | PASS（0.33 / 2.03）→ PASS（0.19 / 1.29） | +0.11 PASS → −0.71 PASS | −0.92 / +1.14 → −1.74 / +0.65 |
| tech-124 | club | 33 | PASS（0.23 / 0.59）→ FAIL（2.37 / 1.04）**新增失败** | −0.06 PASS → +3.37 FAIL **新增失败** | −1.56 / −0.29 → +1.20 / +0.27 |
| tech-124 | standard | 1 | PASS（0.56 / 0.57）→ PASS（0.56 / 0.57） | −0.44 PASS → −0.44 PASS | — |
| tech-124 | standard | 33 | PASS（0.33 / 0.93）→ PASS（0.93 / 0.08） | +1.38 FAIL → +2.13 FAIL | — |
| tech-124 | streaming | 1 | PASS（0.64 / 0.65）→ PASS（0.64 / 0.65） | −0.35 PASS → −0.34 PASS | +0.88 / +0.83 → +0.88 / +0.83 |
| tech-124 | streaming | 33 | PASS（0.38 / 0.93）→ PASS（0.95 / 0.09） | +1.59 FAIL → +2.06 FAIL | +0.10 / +1.22 → +1.30 / −1.96 |

① 方案增益、④ 真峰值、⑤ 目标响度两边的通过与否完全相同（① 六行 FAIL 是「没锁定」的工具口径，⑤ 是方案受预算限制，都在 main 上就有）。从第 1 小节起播的 6 行没有任何新增。

两边的 `house_gate.txt` 都对两首提示「画像没有 House 门控结果（house_gate_valid ≠ 1）——House 学习方式没有生效」（渲染日志里聆听遍末都是「未学习」）。main 与 T-097 是同条件比；落差守门按曲风开关（Deep 开），不看 House 学习有没有生效。

### 4.1 机制（诊断副本追踪，不提交）

诊断副本把渲染工具 `--trace` 的占位校准列换成守门读数（落差退让、loss、判定前提、G_trim、冻结标记）。以 house-124 · 俱乐部 · 起播 33 为例（`--listen-from-bar 33`：第一遍从第 33 小节听到尾，第二遍带着状态从头导出成品）：

1. **聆听遍（61.9 s 起）**：自动增量 25 s 内爬到方案值，G_trim 以 0.05 dB/s 从 0 积到 +3.00 钳位（122 s；main 的追踪在这段逐点相同到小数后 4 位，与 T-097 无关）。直方图约 141 s 满 60 s，breakdown 里 loss 升到 3.4 LU > 1.5，152 s 起以 0.1 dB/s 退，188 s 退到 −3.47 dB（G_trim 从 157 s 起冻结）。
2. **导出遍（从第 1 小节起）**：开头带着落差退让 −3.47 dB、G_trim +3.00 dB。loss 低于门槛 − 0.3（开头几秒判定前提不成立，按规格同样算低于门槛），回程计时从聆听遍接着算，退让在前奏里以 0.05 dB/s 放回，约 73 s 回到 0；这期间按规格（Q11「有退让量就冻结」，修订注 (e)）G_trim 冻在 +3.00。
3. 退让回 0 后 G_trim 恢复积分。L_out 是主段电平的长期积分（规格第 4 节第 3 条「同一组段边界、同一个 m 算主段电平」），刚响过的 drop1 让它一直高于目标，G_trim 以 0.05 dB/s 从 +3.00 一路滑到 −3.00（约 193 s），横跨 drop1 → breakdown → drop2。drop2 实际已比目标低 1.3 LU，G_trim 仍停在 −3.00 钳位上。

各段 Limit Gain 均值（dB，括号里是 G_trim 均值）：

| | intro | drop1 | breakdown | drop2 |
|---|---|---|---|---|
| main | 4.47（−0.81） | 2.36（−2.92） | 3.34（−1.93） | 5.28（0.00） |
| T-097 | 6.20（+3.00） | 7.17（+1.93） | 4.93（−0.34） | 2.85（−2.43） |

breakdown → drop2 的增益在 T-097 下差 −2.1 dB，正好是 ③ 少掉的 2.06 LU。main 同场景 G_trim 也摆了约 3 dB（先降到 −3.00，再升回 +0.68），方向是往 drop2 升，所以 ③ 反而「多了」0.94 LU。其余 5 行（含 tech-124）是同一个形状：导出遍开头 G_trim +3.00、退让约 −3.5 dB，前奏里放回，之后 G_trim 慢滑 4～5 dB。tech-124 俱乐部那行 drop1 被推得更响，GR 高于 P95 上限 5.5 dB 的块占 5.3 %（上限 5 %），② 也跟着失败。

### 4.2 为什么不当实现错改

- 守门（判定、退、回、界）、G_trim 冻结、L_out 的口径，逐条对过规格第 4 节与修订注，都按规格与 Max 的拍板（Q11、Q15 A）走。
- 让这次失败的是两件规格内的事叠在一起：G_trim 的长记忆 + 钳位积分会在「导出前听过中段」时攒下几 dB 的旧偏差；冻结把这份偏差原样带过了守门回程，回程结束才开始慢慢还。T-097 只改变了「什么时候还」。
- 计划第 4 节把「条件积分」列在范围外（R5-03b 统一调度，等 R5-01 指标），这正是该修 G_trim 的地方。在 T-097 里换冻结口径（比如回程中允许积分）会和 Q11 相反，还可能变成积分器反过来抵消守门的回程，属于需求层面的改动。
- 试过一种 T-097 内的改法（诊断副本 C，不提交）：落差守门**回程中**不冻结 G_trim（在退、两线之间停住仍冻结，退让往下变的那一块仍冻结）。House 12 行总失败 13 处（main 15、T-097 20），③ 落差损失：

  | 起播 33 | main | T-097 | C |
  |---|---|---|---|
  | house-124 俱乐部 | −0.94 | +2.06 | −0.39 |
  | house-124 标准 | −0.46 | +2.11 | +2.11 |
  | house-124 流媒体 | −0.46 | +2.05 | +2.05 |
  | tech-124 俱乐部 | −0.06 | +3.37 | +0.43 |
  | tech-124 标准 | +1.38 | +2.13 | +0.71 |
  | tech-124 流媒体 | +1.59 | +2.06 | +0.57 |

  俱乐部两行修好，tech 标准 / 流媒体反而比 main 好（main 上本来就失败的两处转过），但 house-124 标准 / 流媒体两行一点没变：那两行退让放回时误差为正，G_trim 已钳在 +3、积不动，放回结束后照样慢滑。对 main 仍新增 2 处，不满足完成标准；起播第 1 小节的 6 行与 T-097 相同。

## 5. ADR-034 B 组对照表

| 项 | main `4be946b` | T-097 | 说明 |
|---|---|---|---|
| B1 drop 目标（house_gate ⑤，drop1 / drop2 与实际目标之差，LU） | 见第 4 节表最后一列 | 同左 | 起播第 1 小节的 6 行两边相差 ≤ 0.8 LU；起播 33 的差别来自第 4.1 节的 G_trim 慢滑 |
| B2 落差损失（house_gate ③，LU） | 起播 1：−0.44 ～ +0.23 | 起播 1：−0.71 ～ −0.01 | 起播 33 见第 4 节 |
| 其余 B 组项 | 工具未建（R5-01 未立项） | 同左 | — |

## 6. CPU（DGX，链层，48 k / 32，基线与当前交替各 2 轮，核 5，观察进程干净）

`scripts/perf-dgx.sh --base <main 基准> -- --scenario house-club-100,general-standard-100 --sr 48000 --bs 32 --layer chain`；基准工具取自 `origin/claude/t096f-cpu-baseline`，原样叠到 main 与 T-097 两棵树（`CELADON_BENCH_PORTABLE_CHAIN_ONLY` 默认开，只编链层）。单位：块预算的百分比。

| 场景 | structWorst 基线 → 当前（第 1 轮） | Δ 两轮 | avg Δ 两轮 |
|---|---|---|---|
| house-club-100 | 4.406 → 4.267 | −0.139 / −0.113 | −0.156 / −0.155 |
| general-standard-100 | 4.272 → 4.147 | −0.125 / −0.125 | −0.141 / −0.142 |
| house-club-g0 | 3.362 → 3.348 | −0.014 / −0.009 | +0.005 / −0.000 |
| general-standard-g0 | 3.372 → 3.348 | −0.024 / −0.039 | +0.004 / −0.002 |

GLAZE 100 变快约 0.1 个百分点（去掉了落差守门每块两次 `std::sort`），GLAZE 0 不变。处理器层没测（DGX 上编界面库要关 `CHAIN_ONLY`，本任务只报链层）。一分钟负载 5.1～6.3（只记录，阈值未定）。

## 7. 闭环用例实测（`GlazeControllerGuardScenarios`）

- **PSR（Q9）**：输入 S −20 LUFS、3 s PSR 10.1～10.5 dB，流媒体、GLAZE 100，PSR 守门强开。首次退让 39.8 s，最深 −2.25 dB，预算环一次没退；59.8 s 起 902 个读数 96.3 % ≥ 6.5 dB，最低 6.49、平均 6.59 dB。
- **落差（Q10 / Q16，12 段，House 俱乐部 GLAZE 100）**：最后一对 breakdown → drop 损失 ① 出厂 1.35 LU、② 预算放宽 ×4 0.81 LU；最深退让 −4.49 / −5.04 dB。
- **改前（复查 F2）**：DGX 临时副本 = `git archive 4be946b` + 本分支最终的场景段，只换版本差异块（不提交，`~/.cache/celadon-spark/src/t097-before2`，日志 `before2-scenarios.log`），15 过 / 0 败；第 2 轮改完后按新的场景段重建、在 108 上跑两次（副本 3、4），都 15 过 / 0 败、下面的数字一位不差：
  - PSR：守门一次没退（它读入口，入口 PSR 健康），预算环没退；60 s 起 901 个读数 0.0 % ≥ 6.5 dB，最低 4.98、平均 5.12、最高 5.28 dB。
  - 落差：① 出厂最后一对 3.68 LU（只记录）；② 七项 × 4 时预算环没退（前提成立），六对损失 3.36～3.39 LU，最后一对 3.36 LU > 1.5。轨迹里 main 的落差守门最深只到 −0.85 dB、多数时刻为 0（main 没有退让探针，只能从状态行近似），G_trim 从约 90 s 起一直钳在 +3 dB。
- **落差那半的冻结实证（复查 R2-F10）**：两轮里 G_trim 在第一次退让之前就顶到 +3 dB 钳位（`movable` 为 0），「逐位不变」在那里证明不了冻结。第②轮放完后存一份档、把 CONTROL 的 gTrim 改成 +0、读回同一个链，在 House 接着跑 10 s：938 块全都有落差退让（读回时已在回程，−4.15 → −3.80 dB），938 块离钳位都远，0 块没冻结、0 块 G_trim 动过。对照（muse 第二意见）：同一份档再把两个子块守门的退让清 0 读回、先跑同样 10 s，没有一块有退让，G_trim 938 块都动了（0 → +0.50 dB，0.05 dB/s 的上限），证明读回后积分门开着、上面的「不动」是冻结挡住的。PSR 场景的前提另断言 > 0（10331 / 10332 块；块长 480 那遍 501 / 501）。
- **House → 通用**（接在上一条那 10 s 之后）：退让 −3.800 → −2.800 dB / 20 s，200 步回程、0 步停、最大一步 0.005 dB。

## 8. 复查与外部代理

- muse（Wonder Full，只读，428 s）：两条。① 界为 0 时仍报 engaged → 采纳（`31c6083`）；② 重新 prepare 后第一块算「改参数」→ 不采纳：跨 prepare 真改了参数，按规格就该冻结 G_trim 1 s。
- Codex（`gpt-6.1-sol` · `ultra`，只读，220 s；第一次因登录令牌被撤销没跑成，Max 重新登录后重派）：三条。
  1. GLAZE 0 清掉退让与计时 → 不采纳：Max Q6 定「GLAZE 0 仍立即归零」，代码照它；规格修订注 (a) 与 ADR-036 第 5 条把 GLAZE 0 也写进了「保持」，是档案写错，2026-10-23 已更正。
  2. 换交付档清掉持续计时后，守门还在底上、读数也仍超，engaged 却空 10 s → 采纳（`31c6083`）：到底以后 engaged 看本步读数，持续只管要不要再退（同预算环 `planLimited && lastOver`）。
  3. 停播时跳播不触发断窗 → 范围外：跳播检测 `setHostInfo` 停播时不看位置，从首个提交（2026-09-28）就这样，分析器同样漏；对守门的影响是恢复播放后至多 3 s 的窗口混着跳播前的素材。上报 Max，不在本任务改。
- agy：DGX 上未登录、版本 1.4.7 与实测的 1.4.0 不同（脚本会拒），没派。
- 独立复查第 1 轮（Workflow `max-core:independent-review`，规格 / 质量 / 边界三轴 + 核实投票，对 `0ef5308`）：要改 6 条、小毛病 10 条、改动前就有 1 条，没有待用户拍板的。处理：
  1. 「到底仍超」engaged 用错了量（中）→ 已在 `31c6083` 改（复查时在工作区）。
  2. 改前证据没在最终场景上跑（中）→ 补跑，见第 7 节「改前」：前提都成立、判据反向都过。
  3. 新档 floorDb 写 −24，0.1.8.10 降级打开会退到自动部分全没（中）→ 存档写 min(−3, 退让)，纯逻辑与真链用例断言。
  4. 真链只验了预算环的 G_trim 冻结（低）→ 两个闭环场景里落差 / PSR 退让 < 0 的每块断言冻结、逐位不变。
  5. 提交 `50add76` 的信息是英文（低）→ 不改已推送的历史；合并用 squash，提交信息写中文。
  6. 这一步没有读数时 engaged 被清成 false（低）→ 没有读数时沿用上一步、界回到 0 照样清；补纯逻辑用例。
  - 小毛病 10 条都改了：头文件「退到 floorDb」、测试 Q16 口径与【待上机核】、施工说明、站点算法页（落差读 IN + OUT）与 FAQ（PSR 一句）、BASELINE 补 `4be946b`、计划第 2 节旁注、三个测试类改驼峰名、旧档夹具 floorDb 用 −3 并断言读档后 −24、工具里四处切片说明、渲染说明读档一句、`GuardSwitches` 挪到 House 注释之前、两条名不副实的直方图断言说明。
  - 改动前就有：状态行「已退 x dB」显示的是守门请求的量，不是 `applyLimitExtras` 实际减掉的量（不超过自动部分）。Q15 A 允许比界深而保持，这个差会更常见。不在本任务范围，上报 Max，挂了卡片。
- 第 2 轮前自查（`3f77588` 之后）：F11 的改法带进一处窄回归——守门读档时把存档里的 floorDb（现在写 −3）当成自己的下限；`restoreBody` 里落差守门先读成功、后面的节点读失败时整份冷启动，`coldStart` 只 `clear()`、不重设下限，落差守门会停在 −3 的底上直到下一次 `prepare`。根因是读档采纳了只写给旧版的值，而下限是编排器的配置：改成守门读档只校验 floorDb、不采纳；编排器读档后照旧再设一次 −24（顺带按它夹一次读回的退让）。补两条用例：守门层读 −5 / −4 的档后下限仍 −24；真链里 PSR_GUARD 的 reliefDb 写成 NaN、整份冷启动后两个下限仍 −24。
- Codex 第 2 轮（`gpt-6.1-sol` · `ultra`，只读，308 s，对 `0ef5308..3f77588`）：两条。① 读档失败后兼容下限漏进运行时 → 与上一条自查是同一个问题，`1d04372` 已修（它派活期间原目录有变化，是本会话自己在改、提交这几处文件）。② `TrimFreezeCheck` 在退让从 0 变负的那一块不核逐位 → 采纳：块长 512 时块内还按 10 ms 时隙切片，起退那一块在守门退下去之前的几片照规格可以积分，所以那里核不了；`TrimFreezeCheck` 加 `checkOnset`，PSR 闭环场景用同一份素材与设置再跑一遍块长 480（一块一片）：39.80 s 起退，起退前 G_trim 在 3979 块里积分过，之后 501 块有退让、0 块没冻结、0 块 G_trim 动过。main 版副本换上新场景段重跑，结果不变（15 过 / 0 败）。
- 第 2 轮独立复查（深档，对 `0ef5308..3f77588`）：要改 6 条（中 1、低 5）、小毛病 5 条、核实否掉 0。R2-F2 即 `1d04372` 已修的那处；其余 10 条全部采纳（裁决见 [`docs/handoff/2026-10-24-T097-review-round2.md`](../../handoff/2026-10-24-T097-review-round2.md)），2026-10-24 由接手会话改完：
  1. R2-F1（中）：退让比 −3 深时存档的 floorDb 也写成那个更深的值，0.1.8.10 降级打开会一直停在更深的底上。改成给 0.1.8.10 的两项按它的不变量写：floorDb 恒 −3、reliefDb 写 max(−3, 退让)；真实退让另写可选属性 `reliefDbExact`，读档有它读它、没有读 reliefDb；它坏了（NaN、不是十六进制）整个节点拒收、对象不动。用例：PSR 退让 −2 → 旧字段 −3 / −2、reliefDbExact −2；退让 −5 → −3 / −3 / −5，读回 −5、往返逐位，去掉 reliefDbExact 读成 −3；落差 −1 / −4 同形。
  2. R2-F3（低）：守门关着、又没有读数时 engaged 一直亮着。没有读数时只在「上一步 engaged、守门开着、底 < 0」时保持；用例：两个守门到界 −1 且 engaged → 关掉、无音乐 20 步 → 退让 −1、原因不变、engaged 灭；再开、有音乐 → engaged 又亮。
  3. R2-F4（低）：用例 1a 接一段 sourceGain 回 1：预算环 20.96 s 放回 +0，那一块仍冻结，下一块（0.01 s 后）解冻、G_trim 动了（0.13200 dB）。
  4. R2-F5（低）：TASKS 的 T-097 行补登记别线登记过的几处文件（只改注释与散文、核过不重叠），T-095、T-096 交接档末尾留言（两条线都被 Max 叫停在交接状态，不唤醒会话）。
  5. R2-F10（低）：`TrimFreezeCheck` 加前提 `movable`（该核的块里 G_trim 离 ±3 dB 钳位超过 0.05 dB 的块数），PSR 场景断言 > 0；落差两轮另加「G_trim 改成 +0 读回再跑 10 s」，见第 7 节。
  6. 小毛病 5 条：R2-F6 用例 1b 注释、场景头改「落差 / PSR 退让 < 0 的块」、本报告第 1 节改「上一块结束时已有退让的块」；R2-F7 规格 (a′) 与 ADR-036「后果」补「没有读数时 engaged 保持、界回到 0 或守门关掉时清」，更新日志草稿改「看最近几分钟」；R2-F8 底不被采纳的断言改成读档前先设 −24、读档后比 `getFloorDb()`，两组拒收用例同比；R2-F14 engaged 判定、旧字段写法、−3 / −24 两个下限改成共用（`ProfileMath.h` 的 `guardEngagedAfterStep`、`legacyGuardSavedReliefDb`、`legacyGuardFloorDb`，`GlazeConstants.h` 的 `guardSafetyFloorDb`），测试引用具名常量；R2-F19 断窗后先断言前提：落差直方图秒数 29 步不变、第 30 步变，PSR 29 步没有读数、第 30 步有，「界回到 0」两条先断言那一步没读数。
  - Codex 第 3 次（`gpt-6.1-sol` · `ultra`，只读，对第 2 轮处理的增量；Codex 已自动升到 0.164.0，脚本按 0.162.1 实测，照常派）：三条。① 读档时正的退让被收下（中）→ 采纳：`reliefDbExact` 写成 +FLT_MAX 会盖过合法的 reliefDb −3，退让停在正数上、之后往下退要先走回 0（FLT_MAX 减一步还是 FLT_MAX，等于再也退不了）。main 读 reliefDb 也只查是不是有限数，同一个口子；哪一版都写不出正的退让，所以两个守门读档时两项退让任一 > 0 都当损坏拒收，拒收用例 PSR 补「reliefDbExact 为正」「旧形状 reliefDb 为正」、落差补「reliefDbExact 为正」「旧节点 reliefDb 为正」。执行侧本来就只用 min(0, 退让)，正数不会变成加增益。② 落差守门起退那一块没有在 G_trim 能动时核过 → 不采纳：冻结条件是三个守门同一个式子（`GlazeController.cpp:1435-1437`，退让 < 0 或这一块退让的位模式变了），起退那一块的路径由 PSR 块长 480 那遍核过；要落差单独出错，得有代码把三个守门分开对待，现在没有。③ `ContrastGuard.h` 写「0.1.8.10 降级打开退让照旧」没限定 → 采纳，补「比 −3 深的读成 −3」。
  - muse 第 3 次（Wonder Full，只读，同一份增量；muse 已自动升到 1.4.12，脚本按 1.4.10 实测，照常派）：六条。① 正的 `reliefDbExact` 被收下、再存会写进旧字段 reliefDb（中）→ 与 Codex ① 同一处，已改。② 归零读回段只证明 G_trim 离钳位远，没证明这时它本来会积分（中，测试）：积分门还要主段电平有效、有目标、没撤回、误差不为 0，门关着时「不动」什么也证明不了 → 采纳：同一份档再把两个子块守门的退让清 0 读回、跑同样 10 s 当对照，断言对照里没有退让、G_trim 动过，再读回带退让的那份核冻结；`TrimFreezeCheck` 的注释改掉「只要误差不为 0 就一定会动」。③ 拒收用例只比存档与下限，没核不存档的 3 s 窗和上一读数（低，测试）→ 采纳：两组拒收后断言上一读数还在、下一个子块照样有读数 / 进账。④ 落差拒收组缺「新节点 reliefDb 是 NaN、reliefDbExact 好的」（低，测试）→ 采纳，补上。⑤ 头文件「退让照旧」→ 与 Codex ③ 同一处，已改。⑥ 读回段注释「10 s 只回 0.5 dB」没算读档后约 3 s 没有读数（小）→ 采纳，改成「至多 0.5 dB」并写明原因。engaged 等价性两家都没找到问题。
  - 第 2 轮处理里替你定的：属性名 `reliefDbExact`；共用函数放 `ProfileMath.h`（两个守门都已包含它，不新加头文件）；落差那半的逐位实证用「存档改 G_trim 再读回」，不换素材（换素材会动 Q16 定下的 12 段和阈值）；给暂停的 T-095 / T-096 写交接档留言，不发会话消息；正的退让当损坏、整个节点拒收（不夹成 0：这种值只会来自坏档，和别的损坏一样走冷启动）。

- 第 3 轮：**没有独立复查**。Max 2026-10-24 定不开 Workflow，由主会话对照计划第 2 节的完成标准自查 `8f2dd2e..cf4dff9`（源码逐行、测试逐段）：
  - 源码：engaged 判定换成共用函数后，对两个守门逐个情况对过旧写法，只有「守门关着、又没有读数」这一种结果变了（R2-F3 要的）；关着且有读数时，关着那一支只往回放、不置「仍超 / 仍低」，结果仍是 false。存档：`xmlio::Writer::optionalField` 总是写、Reader 缺了才取默认值，与用例对得上；0.1.8.10 再存时整个节点重建、不留 reliefDbExact，回到新版按 reliefDb 读。正的退让运行时写不出来（退时夹在界 / 底上、回程夹在 0、`snapRelief` 写 0），所以拒收不会误伤正常存档。engaged 只进状态行（`publishStatus`），不进音频路径。
  - 测试：每条新断言都核过能不能失败——1a 要求「放回 0 的下一块就解冻」；对照段要求没有退让、G_trim 动过；拒收后要求上一读数还在、下一个子块有读数；断窗前提逐步核 29 / 30。108 上闭环场景的数字与 DGX 逐位相同（PSR 39.8 s 起退、96.3 %、最低 6.49 dB；落差 ① 1.35、② 0.81 LU）。
  - 自查改了两处，都不碰代码：场景头注释没提对照段（`cf4dff9`）；ADR-036 决定 6 补「两项退让任一 > 0 当损坏拒收」。
  - 20 首、House、CPU 是在 `31c6083` 上测的，之后没重跑。之后的源码改动只碰存档、读档（下限不采纳存档值，成功读档后编排器照旧设 −24）、状态行 engaged、探针字段和常量放的位置；退让、G_trim、Limit Gain 合成这些音频路径都没动。

## 9. 0.1.8.11 更新日志草稿（ADR-035：GLAZE > 0 的旧工程会变声，须写明）

| 类别 | 中文 | English |
|---|---|---|
| sound | House 曲风（Deep / Tech / Melodic）的「保对比」改为看最近几分钟的响度分布：听满约 70 秒音乐才开始判断，对比被压掉超过门槛 10 秒才退增益，退下的量不再被慢速电平修正抵消，drop 的冲击能留住。通用曲风不再启用「保对比」。 | In the House genres (Deep / Tech / Melodic), Keep contrast now judges the loudness spread over the last few minutes: it starts after about 70 seconds of music, backs off gain only after the contrast has been squeezed past its threshold for 10 seconds, and the slow level trim no longer cancels what it backed off, so drops keep their impact. Keep contrast is no longer active in the General genre. |
| sound | PSR 保护出厂关闭（它以前看输入，对很密的素材会误退增益）。很响、很密的输入在 GLAZE > 0 时可能比 0.1.8.10 响，最多约 3 dB。 | The PSR guard is now off by default (it used to read the input and could back off gain on dense material by mistake). Very loud, dense inputs may come out up to about 3 dB louder than in 0.1.8.10 when GLAZE is above 0. |
| sound | 任何保护在退增益时，慢速电平修正暂停；改参数后的 1 秒内也暂停。 | The slow level trim now pauses whenever a guard is backing off gain, and for 1 second after you change a setting. |
| compat | 0.1.8.10 的工程照常打开、保护量照旧；用 0.1.8.11 存的工程在 0.1.8.10 里也能打开（在 0.1.8.10 里「保对比」最多退 3 dB：存档时退得更深的，打开那一刻变浅）。 | Projects from 0.1.8.10 open as before with their protection intact, and projects saved in 0.1.8.11 still open in 0.1.8.10 (there, Keep contrast backs off at most 3 dB: a deeper back-off saved by 0.1.8.11 is made shallower when the project loads). |
| ui | 状态行「受限 · 保对比」最多退回全部自动增益（以前封在 3 dB）；保护退到底仍超标时照常显示「受限」。 | The status line's Limited · Keep contrast can now take back all of the automatic gain (it used to stop at 3 dB), and still shows Limited while a guard sits at its limit and is still over. |

（第 4 节的问题 Max 定 A：G_trim 条件积分在 0.1.8.11 发版前修好，这份草稿到时按它的改法补一条。）

## 10. 局限与范围外

- `CeladonChain.h:121` 的注释留给 T-095 / T-098（不归本任务写域）。
- PSR 下限表（代码 8 / 7 / 6 对规格 8.5 / 8 / 6.5）按 ADR-034 第 6 条另议；PSR 守门出厂四个曲风都关。
- 规格第 4 节第 3 条写「GLAZE ≤ 0 时冻结 G_trim」，代码（T-048 起）在 GLAZE ≤ 0 时把 G_trim 清 0：改动前就有，本任务没动，上报。
- 停播时跳播不断窗（第 8 节 Codex 第 3 条）：改动前就有，上报。
- 状态行「已退 x dB」是守门请求的量、不是实际执行的量（第 8 节复查「改动前就有」）：上报。
- G_trim 的长记忆积分在「先听中段再从头导出」时慢滑数 dB（第 4 节）：规格内行为，归 R5-03b「条件积分」；Max 2026-10-24 定为 0.1.8.11 发版门槛。
