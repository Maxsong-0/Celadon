# R6 · GLAZE 决策与常数（GlazePlanner / GlazeConstants）深度审计

- 日期：2026-09-29 22:10 PDT（T-032 研究阶段，只读审计；本报告是本代理唯一写入的文件）
- 工作树：`.claude/worktrees/fable-deep-research-634a5e`（分支自 main `e6b445b`，v0.1.8.5；算法 7 / 标定快照 7）
- 范围：`Source/dsp/GlazePlanner.{h,cpp}`、`Source/dsp/GlazeConstants.{h,cpp}`、`Source/dsp/GlazeTypes.h`，以及编排器里直接消费方案字段的几处（`GlazeController.cpp` 的 `limitedByManual` 门、实时 Tame 许可）。
- 读过的档案：`01-框架/设计规格-GLAZE.md`（含第 7 节 Codex 修订）、`设计规格-GLAZE-决策与校准.md`、`设计规格-GLAZE-分析器.md`、`设计规格-GLAZE-实现修订.md`、`设计规格-高级母带控制.md`、ADR-006 / 008 / 020；仓库 `docs/one-knob-mastering-review.md`、`docs/adaptive-mastering-review.md`、`docs/heat-and-balance-revision.md`、`docs/intelligent-mastering-revision.md`；`tests/GlazePlannerTests.cpp` 全部用例名与相关断言；`docs/validation/2026-09-26-{one-knob,intelligent}/tests-*.log` 里的 GlazeBehaviourReport 表。
- 实验方法：在 107（Ubuntu，g++ 15.2）把仓库**原样**的 `GlazePlanner.cpp` + `GlazeConstants.cpp` 与 juce_core 单独编成小程序（不编整个工程），用出厂常数直接调用 `makePlan` / `resolveAutomaticWeights` / `blendConstants`；再对一份**打了两处拟议修法**的副本重编，前后对照。全部数字见附录；实验目录 `~/.cache/celadon-t032/R6/` 已删。
- 不重报（档案已登记）：Glue 阈值够不到、四种风格 Glue 推到 100（实现修订第 6 节）；Heat「非线性程度」约束无模块统计可核对（同节）；起音密度偏低；性能预算等。

## 0. 结论总表（按 优先级 × 置信度）

| id | 严重度 | 优先级 | 一句话 | 文件:行 | 改后输出 | 工作量 | 置信 |
|---|---|---|---|---|---|---|---|
| R6-01 | high | P0 | 自动 Heat 退化为「风格驱动上限 × 强度」：残差 Heat 换算表几乎不削峰，`HeatRow::inverse` 在目标达不到时返回表的最大 Heat 量，于是只要超限估计 > 0，Heat 就等于 70 / 50 / 20，与音乐无关；在 E = 0 处跳变 58 个单位；底量 12 与 `shareHeat`、`heatPeakReductionCapDb` 三个常数全部失效 | GlazePlanner.cpp:253、912、921；GlazeConstants.h:180–195、221 | 变（新实例 / 重新聆听；旧快照可按算法版本门保持不变） | M | 0.95 |
| R6-02 | high | P0 | 手动 Heat / Glue 超出**染色预算**就把 `limitedByManual` 置真，编排器据此**整体关掉**保护层、校准、约束反馈与画像失配，而自动 Gain / Glue / Clip 照常推响：通透风格手动 Heat 21、−10 LUFS 素材手动 Glue 60 就触发 | GlazePlanner.cpp:583–585、1447、1482；GlazeController.cpp:1588、1621、1633、1845、1922、2160、2576 | 变（仅那些手动组合） | M | 0.9 |
| R6-03 | medium | P1 | 「自动」风格权重把 LRA「不知道」当成「不动态」：温润的持续型权重 = (1 − transient)(1 − ramp(LRA)·lraTrust)，而画像在 15 s 提交时 LRA 置信度只有 0.25（60 s 才满，提交后停止学习），持续型 / 古典素材被锁成温润为主（0.51 vs 通透 0.37），违反分析器第 6 节「不知道不当 0」与决策第 1 节「置信不足偏向通透」 | GlazePlanner.cpp:1253、1259–1262；MusicAnalyzer.cpp:69、2180、2544；GlazeController.cpp:1201–1215、1226、1235 | 变（只影响新提交的画像；已保存的锁定权重不变） | S | 0.85 |
| R6-04 | low | P2 | 实时 Tame 许可 = tameCap × GLAZE × 证据，不乘置信度 κ、不乘段可信度，且未学习 / 首次聆听中也生效；方案里的自动 Tame 用 GLAZE × κ（未学习 κ = 0.35），两条路口径不一致，与规格 7.2「学习结束后」不符 | GlazeController.cpp:2110–2124 vs GlazePlanner.cpp:1527、1565 | 变（未学习 / 低置信时） | S | 0.6 |
| R6-05 | low | P2 | 3 dB 增响底量不受 `maxPreferredLufs`（−7）约束：−5 LUFS 的成品母带在 GLAZE 100 被请求推到 −2 LUFS（35 % 时 +1 dB），与决策第 2 节「已经够响的素材不会为了凑风格范围被调轻/推响」和 L_pref 的 −7 上钳矛盾（Codex 第 1 轮加的底量，Max 未试听拍板） | GlazePlanner.cpp:1417–1418；GlazeConstants.h:90、218 | 变（I > −10 LUFS 的热素材） | S | 0.5 |
| R6-06 | low | P2 | 满强度增益不受 Limit Gain 24 dB 量程约束：I ≤ −36 LUFS 时 `effectiveGainDb` 宣称 26～30 dB，实际 Gain 钳 24，目标显示与校准目标都不可达（校准只能追 3 dB 上限 → 永远「部分校准」） | GlazePlanner.cpp:772、791、895、1571 | 变（仅 I < −34 LUFS） | S | 0.85 |

检查过、**不算缺陷**的项见第 8 节。

---

## 1. R6-01 自动 Heat 退化为风格上限（P0 · high · 置信 0.95）

### 证据

`GlazePlanner.cpp:236–254`（`HeatRow::inverse`）：

```cpp
// 单调列（crest / reduction）的反查：达到 target 所需的最小 Heat 量；达不到返回表的最大 Heat 量。
float inverse (const std::array<float, (size_t) heatTableAmounts>& column, float target) const noexcept
{
    if (! (target > column[0])) return amount[0];
    for (int j = 1; j < heatTableAmounts; ++j) { ... if (hi >= target) return lerp(...); }
    return amount[(size_t) heatTableAmounts - 1];          // ← 253：达不到 → 100
}
```

`GlazePlanner.cpp:909–913`：

```cpp
if (shareHeat > 0.0f && excess > 0.0f)
{
    const HeatRow row (*ctx.c, ctx.truePeakDb + gluePeakDb (ctx, full.glue));
    full.heat = juce::jmin (row.inverse (row.crest, shareHeat * excess), heatCap (sc, row));   // ← 912
}
```

`GlazeConstants.h:180–195` 出厂 Heat 表（算法 6 起的「残差 Heat」实测）：`heatLoudnessDb` 最大幅度 −0.053 dB（+6 dBTP、Heat 100），`heatPeakDb` 全部落在 −0.0004 ～ +0.0027 dB。实验 A0 用出厂常数直接算每行 crest / reduction 列的最大值：

```
level -18 dBTP: crest max 0.00023 dB, peak reduction max 0.00029 dB
level -12 dBTP: crest max 0.00000 dB, peak reduction max 0.00040 dB
level  -6 dBTP: crest max 0.00000 dB, peak reduction max 0.00023 dB
level  +0 dBTP: crest max 0.00000 dB, peak reduction max 0.00000 dB
level  +6 dBTP: crest max 0.00000 dB, peak reduction max 0.00000 dB
```

于是 `row.inverse(row.crest, shareHeat × excess)` 的 target（例如 0.10 × 4.5 = 0.45 dB）永远高于整列（≤ 0.00023 dB）→ 走 253 行返回 100；`heatCap` 里 `row.inverse(row.reduction, 2.0)` 同理返回 100 → `heatCap = heatDriveCap`；所以 **`full.heat = heatDriveCap`（70 / 50 / 20），只要 excess > 0**。921 行的底量 `max(peakHeat, min(12, cap))` 此时是死代码（peakHeat 已是 70）。

实验 A（原版二进制，出厂常数，κ = 1）节选：

| 风格 | I / TP | excess | Heat@100 | cap | Heat@35 |
|---|---|---|---|---|---|
| warm | −24 / −16 | 0.50 | **70.00** | 70 | 24.49 |
| warm | −18 / −6 | 4.50 | **70.00** | 70 | 24.49 |
| warm | −7 / 0 | 3.96 | **70.00** | 70 | 24.49 |
| punchy | −24 / −16 | 0.00 | 12.00 | 50 | 4.20 |
| punchy | −24 / −12 | 4.00 | **50.00** | 50 | 17.49 |
| clean | −18 / −10 | 0.00 | 12.00 | 20 | 4.20 |
| clean | −18 / −6 | 3.00 | **20.00** | 20 | 7.00 |

实验 I（不连续性，warm，I −24）：TP −16.6（E = 0）→ Heat 12.00；TP −16.4（E = 0.10）→ Heat **70.00**。0.2 dB 的峰值差让自动 Heat 跳 58 个单位；GLAZE 35 时 4.20 → 24.49。

实验 E（新实例默认 GLAZE 35、自动风格、几种典型锁定权重）：Heat = 混合上限 × 0.35 = 10.5 ～ 18.3，而底量应给的是 12 × 0.35 = 4.2。

仓库自己的记录印证同一现象：`docs/validation/2026-09-26-intelligent/tests-release.log` 第 591–643 行（算法 6 的 GlazeBehaviourReport），三种素材上 warm / punchy / clean 100 % 的「Glue/Heat」列末项恰为 **70.0 / 50.0 / 20.0**，50 % 时 35.0 / 25.0 / 10.0；只有无超限的正弦混合是 12.0（底量）。对照算法 2（`docs/validation/2026-09-26-one-knob/tests-final.log` 第 482–534 行，旧 Heat 表有真实削峰）同素材 Heat 只有 3.0 ～ 8.1。也就是说第 4 轮换成残差 Heat 表后，自动 Heat 从「按削峰需要 3～8」变成「上限 20～70」，五份 Codex 报告都没有提到这一跳变；`docs/heat-and-balance-revision.md` 开头记录的用户反馈正是「Heat 持续高频刺耳、砂砾感和挤压」。

### 机理

`inverse` 的「达不到就给最大」语义是为旧的削峰型 Heat 设计的（Heat 能削几 dB 峰，给满是合理兜底）。残差 Heat（算法 6）几乎不改变峰值，crest 列整列为 0，这个分支从「罕见兜底」变成「每次必走」。`capacityAt`（863–875）用的是 `row.at(row.crest, heatCap)`（= 上限处真能削的量 ≈ 0），所以容量计算没有把 Heat 当成削峰手段——只有**参数换算**这一处还假定 Heat 承担 `shareHeat × excess` 的削峰并因此开到上限。结果：

- Heat 量与音乐无关（只由风格权重决定），违反规格「按这首歌的画像决定处理量」与「Heat：驱动上限 + 标定后的非线性程度约束」的本意；
- `automaticHeatFloor = 12`（GlazeConstants.h:221）、`shareHeat`（GlazeConstants.cpp:68 / 90 / 112）、`heatPeakReductionCapDb`（56 / 78 / 100）在出厂表下**全部不起作用**；
- 画像 → Heat 的映射在 E = 0 处不连续（画像失配 / 首次聆听中候选画像每秒更新时，Heat 会在 12 ↔ 70 之间来回，只靠 3 s 过渡掩盖）；
- 与 `ColorTests` 的实测对照（Heat 100 在 −6 dBFS 输入 1.48 % THD、−0.41 dB RMS；−0.5 dBFS 2.69 %、−0.94 dB）：warm 100 % 时 Heat 70 叠在 Glue 补偿抬高后的信号上，是可闻的染色量，而不是「轻微 Heat 底量」。

### 修法

1. `GlazePlanner.cpp:909–913` 只让 Heat 承担它在该电平**真能削掉**的那部分（与 `capacityAt` 同一口径），削不了就不为削峰开 Heat，交给 921 行的底量：

   ```cpp
   const HeatRow row (*ctx.c, ctx.truePeakDb + gluePeakDb (ctx, full.glue));
   const auto cap = heatCap (sc, row);
   const auto achievable = row.at (row.crest, cap);                 // 上限处累计能削的峰均比
   const auto wanted = juce::jmin (shareHeat * excess, achievable);
   full.heat = achievable >= heatUsefulCrestDb /* 0.1 dB */ ? juce::jmin (row.inverse (row.crest, wanted), cap) : 0.0f;
   ```

   等价的另一写法：给 `HeatRow::inverse` 加 `bool& reachable` 输出，调用方不可达时按 0 处理；`heatCap` 那一处保留「不可达 = 不设约束」语义（那里返回最大是对的）。
2. 按项目既有的版本门做法（`ctx.c->algorithmVersion >= 3` 那一分支）把新规则挂在 `algorithmVersion >= 8` 上：`currentAlgorithmVersion` 7 → 8、出厂 `Constants::version` 7 → 8；`oldestCompatibleAlgorithmVersion` 保持 6。旧工程随工程保存的快照 `algorithmVersion` 是 7，方案逐位不变；新实例与「重新聆听」用出厂快照 8。`GlazeTypes.h:26–33` 的注释与 `设计规格-GLAZE.md` 7.4「版本」段同步。
3. 顺手把 `heatUsefulCrestDb`（0.1 dB）写进 `Constants`（可缺省读 0 = 旧行为不变）。

改后（实验 A / E / I，修法副本）：所有风格、所有 (I, TP) 下 Heat = 12 × 强度，E 跨 0 处连续；Gain / Glue / Clip 与净增益几乎不变（Heat 对响度的贡献本来就 ≤ 0.05 dB）：warm −18 / −6 的 Gain 5.33 → 5.32，effG 8.63 不变。

### 验证 / 测试

- 新性质测试（GlazePlannerTests）：出厂常数下，对 3 风格 × (I ∈ {−24, −18, −14, −10, −7}) × (PLR ∈ {8, 12, 16}) × GLAZE ∈ {35, 100}，`inc.colorHeat ≤ automaticHeatFloor × GLAZE/100 × κ + 1e-3`，且只要 Heat 表的 crest 列在该电平 < 0.1 dB，Heat 增量与「表清零」时逐位相同；
- 连续性测试：同一画像 TP 从 −16.6 扫到 −16.4，Heat 增量变化 ≤ 0.01；
- 现有 `testGentleColorWithHeadroom`（表填 1 dB 的人为正向表）与 `parallel Heat with no crest capacity` 用例照过；`testStyleDirections` 的「Heat 温润 > 冲击 > 通透」在三者都等于底量时会失败——那条断言本身就建立在上限饱和上，应改为比较 `heatDriveCap` 或删去（风格差异由 Glue / Clip / Limit 体现，规格第 1 节「温润 Heat、Glue 为主」在残差 Heat 下需要 Max 试听重定 Heat 的风格差异，例如把底量做成风格常数 12 / 8 / 4）；
- 兼容：`tests/LegacyFixtureTests` 的 v0.1.6.2 存档（算法 6 快照）方案逐位不变。

---

## 2. R6-02 染色预算触发的「受手动限制」关掉了整套安全层（P0 · high · 置信 0.9）

### 证据

`GlazePlanner.cpp:578–597` `check()`：`v.heat = heatBudget && heatAmount > 0 && (heatAmount > sc.heatDriveCap + 0.5 || …)`；`v.glue` 按 `glueCap` 模型。`1447`：`withdrawn = check (ctx, manualChain, fullPlan.neutral, fullPlan.heat > 0.0f)`；`1482`：`bool limitedByManual = withdrawn.any() || ctx.limitBypassed;` —— 但 `1484–1487` 只在 `withdrawn.limit || limitBypassed` 时把 `strength` 置 0；Heat / Glue / Clip 被撤回时其余自动增量照常展开。

编排器把这个标志当成「自动全部撤回」：

- `GlazeController.cpp:2160–2161` `computeEffective`：`if (toSlot().plan.limitedByManual) return;` → 校准偏差与保护偏移都不施加（注释「自动增量本来就撤回了」）；
- `2576` 保护层不运行并 `guard.reset()`；`1588` 校准子窗作废；`1621` 不开校准轮；`1633` `biasTarget` 返回 0；`1845` 画像失配不判；`1922` 约束反馈不做。

实验 D（原版，出厂常数，GLAZE 100，通透）：

```
clean, manual Heat 20: limitedByManual 0  incGlue 80.39 incGain 4.79 effGain 8.00
clean, manual Heat 21: limitedByManual 1  incGlue 80.39 incGain 4.79 effGain 8.00   ← 自动 Glue / Gain 一点没少
clean, I -10 manual Glue 50: limitedByManual 0 incGlue 6.25 incGain 1.58
clean, I -10 manual Glue 60: limitedByManual 1 incGlue 0.00 incGain 1.58 effGain 4.31
clean, I -14 manual Glue 80: limitedByManual 1 …；I -18 manual Glue 90: limitedByManual 1 …
```

### 机理

「手动本身已超约束 → 自动先撤回自己的增量、界面提示」（决策第 8 节）是**逐模块**的语义；但 `Plan::limitedByManual` 是一个总标志，编排器又把它解释成「没有任何自动加上去的增益可收回」（2160、2576 的注释）。Heat 驱动上限、Glue GR 上限是**染色预算**，越过它们不会让限幅器停下；此时自动 Limit Gain（+4.79 dB）、Clip、Glue 仍在推响，却没有保护层退让、没有校准修正、没有约束反馈、也不判画像失配。通透风格的 Heat 上限只有 20，自动风格的混合上限也常在 30 左右；−10 LUFS 素材 Glue 上限约 56——手动 Heat 25 或 Glue 60 都是正常操作，不是极端设置。规格第 7 节「学习、过渡、校准期间保护层同样生效」在这些手动组合下被静默取消。

### 修法

1. `GlazeTypes.h` `Plan` 加一个字段 `bool automaticLevelWithdrawn`（= `withdrawn.limit || ctx.limitBypassed || maxGain < manualNetDb`，即 1484–1503 那三种「电平类自动全部撤回」的情况）；`limitedByManual` 保留为界面提示（任一模块被撤回）。
2. `GlazeController.cpp` 的 1588 / 1621 / 1633 / 1845 / 1922 / 2160 / 2576 改看 `automaticLevelWithdrawn`；界面 `Status::limitedByManual` 不变（可进一步细化成位掩码 Heat / Glue / Clip / Limit 让状态行写明是哪一项）。
3. `GlazePlanner.h` 头注释第 17 行与 `设计规格-GLAZE-实现修订.md` 第 3 节「受手动设置限制时，不施加偏差，也不施加保护」改为「电平类自动全部撤回时」。

改后输出：只有「手动 Heat / Glue / Clip 超预算 + GLAZE > 0」的组合改变（保护层、校准恢复工作）；`testManualComesFirst` 的手动 Gain 20 dB 用例仍走 `withdrawn.limit` 路径，行为不变。

### 验证 / 测试

- GlazeControllerTests：热素材、通透、GLAZE 100、手动 Heat 25 → 状态 `limitedByManual` 为真，但 `probe.executedGuardDb < 0`（保护层动作过）且 `calibrationBegins ≥ 1`；对照手动 Gain 20 dB 用例仍 0 / 0；
- GlazePlannerTests：`heavyHeat` 用例补断言 `!plan.automaticLevelWithdrawn`，`heavyGain` 与 `bypassed` 用例 `automaticLevelWithdrawn == true`。

---

## 3. R6-03 自动权重把「LRA 未知」当成「不动态」，持续型素材被锁成温润（P1 · medium · 置信 0.85）

### 证据

`GlazePlanner.cpp:1253、1259–1262`：

```cpp
const auto dynamic = ramp (valueOr (&p, &MusicProfile::loudnessRangeLu, 0.0f), c.autoDynamicLraLowLu, c.autoDynamicLraHighLu) * lraTrust;
const StyleWeights raw { base + (1.0f - transient) * (1.0f - dynamic) * nonNegative (c.autoWarmSustainWeight) + …bright,
                         base + transient * (1.0f - clamp01 (c.autoPunchyDynamicDamping) * dynamic),
                         base + dynamic };
```

`lraTrust = clamp01(confidence / 0.8)`；LRA 置信度 = `min(代表性, rangeSeconds / 60)`（MusicAnalyzer.cpp:69、2180、2544）。聆听结束条件不含 LRA，I / TP / PLR / 斜率 / 占比 10 s 即满、代表性 15 s 即 1（MusicAnalyzer.cpp:78），所以有播放头时**几乎每首歌都在 15 s 提交**（GlazeController.cpp:1201–1215）→ LRA 置信度 0.25 → lraTrust 0.31；无播放头时 30 s → 0.625。`commitCandidate()`（1219–1235）锁定权重（1226）后 `stopLearning()`（1235），LRA 再也不会成熟。

实验 B（原版，持续型：起音强度 2 dB、密度 0.5 次/s，均可信）：

| LRA | LRA 置信 | warm | punchy | clean | 主导 |
|---|---|---|---|---|---|
| 15 | 0.25（15 s 提交） | **0.508** | 0.121 | 0.372 | 温润 |
| 15 | 0.50（30 s 提交） | 0.308 | 0.112 | 0.579 | 通透 |
| 15 | 1.00（不可能达到） | 0.103 | 0.103 | 0.793 | 通透 |
| 无效 | — | **0.739** | 0.130 | 0.130 | 温润 |

实验 J（古典型：I −24、TP −6、LRA 测得 15、置信 0.25）：锁定权重 0.508 / 0.121 / 0.372 → 混合 L_style −9.12（纯通透 −10）、heatDriveCap 49 → 与 R6-01 叠加后 GLAZE 100 的自动 Heat 49；权重按置信度理应收向通透时反而收向温润。

### 机理

`dynamic = ramp(LRA) × lraTrust` 把「不知道」折成 0，再进 `(1 − dynamic)`，于是「LRA 未知」等价于「LRA 很小」= 持续、不动态 → 温润权重 0.7。这与分析器第 6 节「『不知道』不当 0」、决策第 1 节「置信不足时偏向通透（最保守）」相反；而权重一旦锁定就随画像保存、不再重判，错误是永久的。第 2 轮 Codex 修订「不再被 60 s LRA 置信度整体压向通透」只把 LRA 从**总置信**里拿掉，没有处理它在温润项里的这层反向偏置。既有测试（`Automatic weights sum to one and lean clean when unsure`）只覆盖了鼓点素材的早期 LRA（transient = 1 时持续项本来为 0），没有覆盖「持续型 + LRA 未满」。

### 修法

把持续型证据拆成「已知不动态」与「不知道」两份，前者归温润、后者归通透（`GlazePlanner.cpp:1259–1262`）：

```cpp
const auto dynamicRamp = ramp (valueOr (&p, &MusicProfile::loudnessRangeLu, 0.0f), c.autoDynamicLraLowLu, c.autoDynamicLraHighLu);
const auto dynamic = dynamicRamp * lraTrust;
const auto sustain = (1.0f - transient) * nonNegative (c.autoWarmSustainWeight);
const StyleWeights raw { base + sustain * (1.0f - dynamicRamp) * lraTrust + nonNegative (c.autoWarmBrightWeight) * bright,
                         base + transient * (1.0f - clamp01 (c.autoPunchyDynamicDamping) * dynamic),
                         base + dynamic + sustain * (1.0f - lraTrust) };
```

改后（实验 B / J，修法副本）：LRA 15 置信 0.25 → 0.121 / 0.121 / 0.759（通透）；LRA 无效 → 0.130 / 0.130 / 0.739；LRA 3 置信 ≥ 0.8（真的不动态）→ 0.739 温润，与原版相同；鼓点素材各行不变。既有测试全部断言仍成立（逐条核过：`drums`、`earlyDrums`、`noRange`、`dynamicDrums`、`dynamic`、`sustained`、`unsure`、`half`、`invalidOnset`、`empty`）。锁定权重随画像保存，旧工程不变；与 R6-01 一起走算法 8 的版本门也可以（`resolveAutomaticWeights` 已拿到 `Constants`）。

补充（设计层，留给 Max）：同样因为 LRA 提交后冻结，`preferred = L_style − (LRA − 6)/3`（1414–1416）永远用 15～30 s 片段的 LRA，动态歌的目标系统性偏高。若想根治，可让分析器在提交后继续累计 LRA 到 60 s、**只在其置信度首次达 0.8 时**重算 f(LRA) 与权重一次（属于完成同一次学习，不是重学）；这会改动时间线规格，需要 Max 拍板，本报告不列为独立条目。

### 验证 / 测试

GlazePlannerTests 新用例：持续型画像（起音 2 dB / 0.5 次/s 可信）在 LRA 置信度 0.25 / 0.5 下 `clean` 权重最大；LRA 无效同样偏通透；LRA 3 置信 1.0 仍偏温润；权重和为 1、随 LRA 置信度单调（置信升高时 warm 不减）。

---

## 4. R6-04 实时 Tame 许可不乘置信度、未学习也生效（P2 · low · 置信 0.6）

### 证据

`GlazeController.cpp:2110–2124`：

```cpp
if (active && ! savedPlanMode && ! manual.balance.bypass && toSlot().valid)
{ … const auto glaze = jlimit (0, 1, manual.glazePercent * 0.01f);
  const auto liveTame = tameCap * glaze * jlimit (0, 1, evidence.value / snapshot.tameEvidenceFullDb);
  effective.balance.tame = jlimit (0, 100, manual.balance.tame + jmax (increments.balanceTame, liveTame)); … }
```

方案里的同一量：`GlazePlanner.cpp:1527` `spectralStrength = glaze × kappa`，`1565` `inc.balanceTame = max(底量, 证据 × bandTrust) × spectralStrength`。未学习时 `toSlot().valid` 也为真（保守方案照样评估），κ = 0.35。实验 F：未学习 GLAZE 100 方案的 Tame 增量 8.4，而实时路径在同一状态下上限为 45（通透 tameCap）× 1 × 证据。

### 机理 / 修法

两条路对「同一物理量」用了不同的强度口径；规格 7.2 写的是「学习结束后继续用 Balance 帧算短期突出证据」。修法（S）：`liveTame` 乘 `confidenceScale(committed.hasProfile ? &committed.profile : nullptr, snapshot)`（与 `updateLiveTone` 同一写法），并把 `committed.hasProfile` 列入 2110 行条件（或至少 `learning != Learning::firstListen`）。执行快照里的 `liveTame` 是实际值，恢复路径不变。

### 验证

GlazeControllerTests：未学习 + GLAZE 100 + 有强 Tame 证据的素材，`effective.balance.automaticTame ≤ tameCap × 0.35 + 1e-3`；学习完成后（κ = 1）与现行为一致。

---

## 5. R6-05 3 dB 增响底量不受偏好响度上钳约束（P2 · low · 置信 0.5）

### 证据

`GlazePlanner.cpp:1417–1418`：`requestFull = max (driveFloor, nonNegative (preferred − ctx.inputLufs))`，`preferred` 已钳在 [−14, −7]（GlazeConstants.h:89–90），但底量绕开了它。实验 C（出厂常数，TP −0.3）：

| I | 风格 | GLAZE | requestedTarget | effectiveTarget | 受约束 |
|---|---|---|---|---|---|
| −7 | clean | 100 | −4.02 | −4.02 | 否 |
| −5 | clean | 100 | −2.02 | −2.02 | 否 |
| −5 | warm | 100 | −2.02 | −2.09 | 是（仅 0.07 dB） |
| −5 | 任一 | 35 | −3.97 | −3.97 | 否 |

### 机理 / 修法

决策第 2 节把 L_pref 钳到 −7 是「最响也就到这」的偏好；第 1 轮 Codex 加的「增响请求下限 3 dB，已达到风格偏好响度的歌也保留」（规格 7.1）把这个上钳作废了，成品母带（−5 ～ −7 LUFS）会被再推 3 dB、限幅峰值 GR 约 3.7 dB，而模块约束（通透 limitGrPeakCap 6 dB）拦不住。这是 Codex 的产品决定，Max 没试听过，所以只列 P2 供拍板。修法（S）：底量只在输入还没到上钳前生效，例如 `requestFull = max (min (driveFloor, nonNegative (maxPreferredLufs + 1 − ctx.inputLufs)), nonNegative (preferred − ctx.inputLufs))`（成品母带最多再 +1 dB），常数进快照。现有 `Already loud material retains a bounded monotone drive request` 用例中 I −10 / −12 的断言不受影响。

---

## 6. R6-06 满强度增益不受 Limit Gain 量程约束（P2 · low · 置信 0.85）

### 证据

`GlazePlanner.cpp:772` `fullGain = jmin (gain, 24)` 只钳了折点，`895` `full.gainDb = gain` 与 `1571` `effectiveGainDb = manualNetDb + strength × fullPlan.gainDb` 用的仍是未钳的 G_full′；`applyIncrements`（1739）再钳到 24。实验 H（clean，GLAZE 100，PLR 12）：

```
I -34: requestedGain 24.00 effectiveGain 24.00 gain increment 18.48 actual Limit Gain 18.48
I -36: requestedGain 26.00 effectiveGain 26.00 gain increment 20.48 actual Limit Gain 20.48
I -40: requestedGain 30.00 effectiveGain 30.00 gain increment 24.00 actual Limit Gain 24.00   ← 宣称 30，能给 24
```

### 机理 / 修法

极安静素材（未归一化的现场录音、古典）下 `effectiveTargetLufs` 显示 −10 而实际约 −16；校准器以 `effectiveGainDb` 为目标（GlazePlanner.h 第 23 行）、修正范围 3 dB × GLAZE × κ，永远追不上 → 每首都结束为「部分校准」。修法（S）：`completeFullPlan` 里若 `gain > 24`，用 `estimateLimit (ctx, color, 24, clip).netDb − full.neutral.netDb` 回算可交付的 G_full′ 写回 `full.gainDb`、置 `constrained = true`；或在 1418 行把 `requestFull` 钳到 `24 − manualSettings.gainDb`（更粗）。改后只影响 I < −34 LUFS 的输入。

### 验证

GlazePlannerTests：I −40 / TP −28 / clean / GLAZE 100 → `effectiveGainDb ≤ applyIncrements(...).limit.gainDb − manual + 手动链净增益 + 0.5`（即目标不超过 Gain 24 能交付的净增益）、`limitedByConstraints == true`。

---

## 7. 实验数据（107，g++ 15.2，-O2 -ffp-contract=off，出厂常数，κ = 1 除注明外）

原版 = 仓库原样 `GlazePlanner.cpp`；修法 = 只改 R6-01（909–913）与 R6-03（1259–1262）两处的副本。

### 7.1 实验 A：自动 Heat / Glue / Gain / Clip 随 (风格, I, TP, GLAZE)

原版（Heat 列 = 风格上限 × 强度，只要 excess > 0）：

```
style     I     TP  GLAZE | excess   heat   cap   glue  gain  clip  effG constr
warm   -24.0 -16.0   100 |  0.50  70.00  70.0  97.5  9.82   0.0 15.50  no
warm   -24.0 -12.0   100 |  4.50  70.00  70.0 100.0  9.57  71.8 14.63  yes
warm   -18.0  -6.0   100 |  4.50  70.00  70.0  85.9  5.33  71.0  8.63  yes
warm   -18.0  -6.0    35 |  0.00  24.49  70.0  30.0  1.22  41.3  3.02  yes
warm   -14.0   0.0   100 |  6.46  70.00  70.0  43.9  0.62  72.2  2.59  yes
warm    -7.0   0.0   100 |  3.96  70.00  70.0  40.1  2.03  71.3  2.59  yes
punchy -24.0 -16.0   100 |  0.00  12.00  50.0   0.0 15.00   0.0 15.00  no
punchy -24.0 -12.0   100 |  4.00  50.00  50.0 100.0  9.88  79.2 15.00  no
punchy -18.0  -6.0   100 |  4.00  50.00  50.0  77.5  5.38  79.1  9.00  no
punchy -18.0  -6.0    35 |  0.00  17.49  50.0  27.1  1.52  57.8  3.15  no
clean  -24.0 -16.0   100 |  0.00  12.00  20.0   0.0 14.00   0.0 14.00  no
clean  -18.0  -6.0   100 |  3.00  20.00  20.0  78.3  4.27   0.0  8.00  no
clean  -18.0  -6.0    35 |  0.00   7.00  20.0  27.4  1.16   0.0  2.80  no
clean  -10.0   0.0   100 |  3.96  20.00  20.0  47.1  1.73   0.0  2.96  no
```

修法（Heat 恒为底量 12 × 强度；其余列几乎不变）：

```
warm   -24.0 -16.0   100 |  0.50  12.00  70.0  97.5  9.82   0.0 15.50  no
warm   -18.0  -6.0   100 |  4.50  12.00  70.0  85.9  5.32  71.0  8.63  yes
warm   -18.0  -6.0    35 |  0.00   4.20  70.0  30.0  1.22  41.4  3.02  yes
punchy -18.0  -6.0   100 |  4.00  12.00  50.0  77.5  5.38  79.1  9.00  no
clean  -18.0  -6.0   100 |  3.00  12.00  20.0  78.3  4.27   0.0  8.00  no
clean  -10.0   0.0   100 |  3.96  12.00  20.0  47.1  1.73   0.0  2.96  no
```

### 7.2 实验 I：E = 0 处的不连续（warm，I −24）

```
TP      excess  Heat@100(原) Heat@35(原) | Heat@100(修) Heat@35(修)
-17.0    0.00     12.00        4.20     |   12.00        4.20
-16.6    0.00     12.00        4.20     |   12.00        4.20
-16.4    0.10     70.00       24.49     |   12.00        4.20
-16.0    0.50     70.00       24.49     |   12.00        4.20
```

### 7.3 实验 E：新实例默认（GLAZE 35、自动风格）

```
权重 warm/punchy/clean   I    Heat(原)  混合cap  cap×0.35  底量×0.35  Heat(修)
0.12/0.88/0.00        -18    18.30     52.3     18.31      4.20       4.20
0.50/0.00/0.50        -18    15.74     45.0     15.75      4.20       4.20
0.17/0.05/0.78        -18    10.50     30.0     10.50      4.20       4.20
```

### 7.4 实验 B：自动权重 vs LRA 置信度（持续型素材）

```
LRA  conf |  原版 warm/punchy/clean | 修法 warm/punchy/clean
 3.0 0.25 |  0.739 0.130 0.130 WARM | 0.321 0.130 0.549 clean
 3.0 0.80 |  0.739 0.130 0.130 WARM | 0.739 0.130 0.130 WARM
 8.0 0.25 |  0.635 0.126 0.239 WARM | 0.231 0.126 0.643 clean
 8.0 0.50 |  0.538 0.122 0.340 WARM | 0.325 0.122 0.553 clean
12.0 0.25 |  0.508 0.121 0.372 WARM | 0.121 0.121 0.759 clean
15.0 0.25 |  0.508 0.121 0.372 WARM | 0.121 0.121 0.759 clean
15.0 0.50 |  0.308 0.112 0.579 clean| 0.112 0.112 0.776 clean
15.0 1.00 |  0.103 0.103 0.793 clean| 0.103 0.103 0.793 clean
LRA 无效  |  0.739 0.130 0.130 WARM | 0.130 0.130 0.739 clean
```

### 7.5 实验 J：古典型录音 15 s 提交的连带后果

```
LRA conf | 权重 (原)           L_style  heatCap  reqTarget  Heat@100 | 权重 (修)           L_style  heatCap  Heat@100
0.25     | 0.508/0.121/0.372   -9.12    49.0     -12.12     49.0     | 0.121/0.121/0.759   -9.70    29.6     12.0
0.50     | 0.308/0.112/0.579   -9.43    38.8     -12.43     38.8     | 0.112/0.112/0.776   -9.72    29.0     12.0
```

### 7.6 实验 C（热素材底量）、D（limitedByManual）、F（未学习）、H（极安静）、G（Heat 随 GLAZE）

见第 5、2、4、6 节引用；G：warm、I −18 / TP −5，原版 Heat 随 GLAZE 0 → 100 线性 0 → 70.00（每 12.5 % 加 8.75），修法 0 → 12.00。F：未学习 GLAZE 100 → 净增益 1.40 dB、Heat 4.20、Recover 2.80、Tame 8.40、Tone 0（与实现修订第 2 节「约 +1.4 dB」相符）。

## 8. 查过、不算缺陷或不重报的项

- 模块映射与真实模块一致：Glue 阈值 −24 × Glue/100、2:1、拐点 6 dB、补偿 −阈值 × 0.5 × 0.5（ColorModule.cpp:54–57、695–696）；Clip 阈值 jmap(0 → +12, 100 → Ceiling)（LimiterModule.cpp:271、351）；SoftClip 0.7 拐点 tanh 形状（Saturator.h:70–80）与 `softClipReductionDb` 同式；参数范围（ParameterRanges.h）与 `applyIncrements` 钳位一致。
- Ceiling：编码余量在 `CeladonChain::setParams`（131–132）里先减掉再交 `controller.beginBlock(adjusted)`，决策器与校准参考条件看到的都是实际交给限幅器的 Ceiling，无量纲错位。
- Tone 符号：参考斜率 − 画像斜率 > 0（偏暗）→ 正 Tone（提亮），与 Tone 参数「低搁架 −x、高搁架 +x」一致；死区 0.35、0.5 dB/(dB/oct)、±0.75 上限均按规格。
- 置信度收拢方向、F-05 的「拉满置信度逐项取小」、强度网格 4096 / 128 与二分单调性、NaN / Inf 兜底、GLAZE ≤ 0 逐位直通（`addClamped`）都核过，未发现反例。
- 采样率 / 块长：决策器无时间常数、无采样率量；Heat 表按 48 kHz 实测但数值近零，采样率差异无影响。
- `limitLossDb` 的 0.04 E² 在 E > 14 dB 时定点迭代斜率 > 1，但容量约束把 E 限在 ≤ 6 dB 左右，只有关掉全部限幅上限的坏快照才会到那里（已有 `Odd calibration snapshots still give finite plans` 兜底），不列。
- 已登记待认领、不重报：Glue 阈值够不到 / 四风格 Glue 推到 100（实验 J 也见 Glue 100）；Heat「非线性程度」无可核对统计（本报告 R6-01 顺带让 `heatPeakReductionCapDb` 重新有意义）；起音检测密度偏低。

## 9. 建议的落地顺序（当晚）

1. R6-01（算法 8 版本门 + 常数 `heatUsefulCrestDb` + 测试改造）与 R6-03（同一版本门下改权重公式 + 新用例）一起做，都在 `GlazePlanner.cpp` / `GlazeConstants.{h,cpp}` / `GlazeTypes.h` / `GlazePlannerTests.cpp`；`testStyleDirections` 的 Heat 断言按第 1 节说明改；`设计规格-GLAZE.md` 7.4 版本段与 `GlazeConstants.cpp` 头表补一行。
2. R6-02：`GlazeTypes.h` 加字段、`GlazePlanner.cpp` 1482 附近填值、`GlazeController.cpp` 七处改门、控制器测试一例。
3. R6-06（S）随手做；R6-04、R6-05 写进 TASKS 待 Max 拍板后做。
