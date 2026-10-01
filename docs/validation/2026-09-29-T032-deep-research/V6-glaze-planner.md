# V6 · GLAZE 决策与常数（GlazePlanner / GlazeConstants）反驳核验

- 日期：2026-09-29 22:30 PDT（T-032 阶段 B，只读核验；本文件是本代理唯一写入的文件）
- 被核验报告：`R6-glaze-planner.md`（同目录）
- 方法：逐条在工作树 `.claude/worktrees/fable-deep-research-634a5e`（main `e6b445b`，算法 7 / 标定 7）里重读代码与测试（文件:行号），对照档案 `01-框架/设计规格-GLAZE*.md`、`03-记忆/ADR-013`、`06-BUG`、`04-协调/TASKS.md`，以及仓库 `docs/` 下 Codex 的报告与 `docs/validation/2026-09-26-*` 的行为日志。**没有在 107 跑数值实验**：R6-01 的 HeatRow 算术、R6-03 的权重算术都按出厂常数手算复现（R6-03 三个权重与研究报告一致到小数点后三位），其余以代码与仓库自带日志 / 存档为证。本机没编译、没跑测试（纪律 6）。
- 判决口径：`confirmed` = 我自己在代码里找到了同样的证据且不是规格已定的取舍；`refuted` = 不是缺陷或已由规格 / 测试定为有意行为；`uncertain` = 证据不足。

## 0. 判决总表

| id | 研究判定 | 核验判决 | 调整后 严重度 / 优先级 | 修法可用？ | 一句话 |
|---|---|---|---|---|---|
| R6-01 | high / P0 | **confirmed** | high / P0 | 可用，需 5 处补强 | 出厂表 crest 列 ≤ 0.00023 dB，`inverse` 达不到就给 100 → `full.heat = heatCap`；底量、shareHeat、heatPeakReductionCapDb 三个常数在出厂表下确实失效。饱和其实从算法 3（第 2 轮并联表）就开始 |
| R6-02 | high / P0 | **confirmed** | high / P0 | 可用，需补序列化与 limitPlanTo | 七处门全部核实；实现修订 §3 那句「不施加偏差也不施加保护」写于 F-02 逐模块钳住之前，前提已失效 |
| R6-03 | medium / P1 | **confirmed**（缺陷成立） | medium / **P2（待 Max）** | **不可用** | 缺陷真实；但研究的修法在标准 15 s 提交下（LRA 置信 ≤ 0.25）会让「自动」风格几乎永远选不到温润，重新造成规格 7.1 第 2 轮明文消除的问题 |
| R6-04 | low / P2 | **confirmed** | low / P2 | 可用，需核一条既有测试 | `liveTame` 不乘 κ、门在 `toSlot().valid`（未学习也为真）；`updateLiveTone` 两样都做了，口径确实不一致 |
| R6-05 | low / P2 | **refuted**（不是缺陷） | low / P2（试听待定项） | **不可用** | 规格 7.1 明文「增响请求下限 3 dB，已达到风格偏好响度的歌也保留」、7.5「验收 3 被取代」、「本节与子文档冲突以本节为准」；既有测试断言 −6 LUFS 母带请求 = 3 dB × GLAZE |
| R6-06 | low / P2 | **confirmed** | low / P2 | 部分可用 | `full.gainDb = gain`（895）不受 24 dB 量程约束属实；但研究把影响范围说窄了——手动 Gain + 自动请求 > 24 时同样虚报，修法要按 `24 − 手动 Gain` 钳 |

---

## 1. R6-01 自动 Heat 退化为「风格上限 × 强度」 — confirmed（high / P0）

### 我找到的同样证据

1. `Source/dsp/GlazePlanner.cpp:191–194`：`crest[j] = 累计 max(nonNegative(loudness − peak))`，`reduction[j] = 累计 max(nonNegative(−peak))`。出厂表（`GlazeConstants.h:180–195`）`heatLoudnessDb` 全为 0 ～ −0.053，`heatPeakDb` 在 −12 ～ +6 dBTP 四行为 0 ～ +0.0027（正）→ `loudness − peak < 0` → crest 整列 0；只有 −18 dBTP 一行 Heat 100 处 `−0.00006 − (−0.00029) = 0.00023`。手算与研究报告的实验 A0 一致。
2. `GlazePlanner.cpp:236–254` `inverse`：target > column[0] 且整列都 < target → 走 253 行返回 `amount[8] = 100`。`GlazePlanner.cpp:708–714` `heatCap`：`inverse(reduction, 2.0)`（reduction 列 ≤ 0.0004）同样返回 100 → `heatCap = min(heatDriveCap, 100, 100) = heatDriveCap`。
3. `GlazePlanner.cpp:909–913`：`excess > 0` → `full.heat = min(100, heatDriveCap) = 70 / 50 / 20`；`:918–921` 底量 `max(peakHeat = 70, min(12, 70)) = 70`，底量此时确实是死代码。
4. `GlazePlanner.cpp:1452–1456`：`heatAllowance = heatCap − 手动 Heat`；`:950–953` `autoSettingsAt`：`heat = min(full.heat × s, allowance)` → `inc.colorHeat = 上限 × 强度`。
5. `check()`（`:578–585`）的 Heat 约束只看 `heatAmount > cap + 0.5` 与 `heatPeakReductionDb > cap`，后者在出厂表下（≤ 0.0004 dB）永远不触发——`heatPeakReductionCapDb` 三个常数确实无作用；`shareHeat` 只进 `inverse` 的 target，target 多大都达不到，也无作用。
6. **仓库自证**（研究报告引的两份之外我又核了两份）：
   - `docs/validation/2026-09-26-intelligent/tests-release.log:591–643`（算法 6）：类音乐 / 粉噪 / 热类音乐三种素材，温润 / 冲击 / 通透 100 % 的 Heat 恰为 70.0 / 50.0 / 20.0，50 % 为 35.0 / 25.0 / 10.0；正弦混合（无超限）为 12.0 / 6.0（底量）。
   - `docs/validation/2026-09-26-one-knob/tests-final.log:482–534`（算法 2，旧削峰型表）：同素材 Heat 3.0 ～ 8.1。
   - `docs/validation/2026-09-26-heat-revision/tests-third.log:506–511`（**算法 3**，第 2 轮并联表）：温润 100 % 已是 **70.0**，冲击 47.4、通透 12.9；`2026-09-26-adaptive-mastering/tests-second.log:547–552`（算法 4/5）同样 70.0 / 47.4 / 12.9。
   - `tests/LegacyFixtureTests.cpp:56`：v0.1.6.2 真实存档（通透、GLAZE 60）的 `colorHeat="413ff400"` = 11.997 = 通透上限 20 × 0.6（落网格）。
7. 规格侧：`设计规格-GLAZE.md` 7.1「自动 Heat 12（底量）」、7.4「Heat 的削峰份额改为可选贡献」；`决策与校准` §4「约束是上限，不是必须花完的额度」、§1 对 v0.4「按固定比例加偏移、与音乐无关」的批评；Codex `docs/heat-and-balance-revision.md`「并联 Heat 的削峰份额改为可选贡献，其能力不足不再锁死限幅器」。五份报告没有一处说「Heat 应顶到驱动上限」。所以「Heat = 上限 × GLAZE × κ、与画像无关」不是规格已定的取舍，而是 `capacityAt`（`:857–875`，算法 ≥ 3 分支已改成 `row.at(row.crest, heatCap)`）与参数换算（`:912`）两处口径不一致遗留下来的。

### 对研究报告的两处修正（不改结论）

- 起点不是第 4 轮：温润在**算法 3**（第 2 轮并联表）就已顶到 70；冲击 / 通透当时表里还有一点 crest（47.4 / 12.9），第 4 轮残差表把 crest 清到 0 之后三种风格才**都**精确顶到上限。
- 「E = 0 处跳变 58 个单位」在代码上成立（`excess > 0` 是硬阈值），但实践中 E = 0 只出现在「请求增益推不过 Ceiling」的低 PLR 安静素材；3 dB 底量让绝大多数素材 E > 0，所以主要症状是「Heat 恒为上限 × 强度」，跳变是次要症状。首次聆听中候选画像每秒更新时会撞到这个边界，1 s 过渡只是掩盖。

### 修法核验（可用，需补强 5 处）

研究的换算改法（只承担 `row.at(row.crest, cap)` 真能削掉的量，达不到 0.1 dB 就交给底量）与 `capacityAt` 的口径一致，E 跨 0 连续，我按出厂表手算：所有 (风格, I, TP) 下 `full.heat = 12`，`inc.colorHeat = 12 × 强度`。补强：

1. **版本门必须是唯一开关**：研究说「`heatUsefulCrestDb` 缺省读 0 = 旧行为」——不成立。阈值为 0 时新分支在 −18 dBTP 行会用 0.00008 dB 的「可达 crest」反查出 Heat 70（`inverse` 在 crest[7]=0.00003 → crest[8]=0.00023 之间插值到 70）。所以新规则只能挂 `ctx.c->algorithmVersion >= 8`（与 `:866` 的 `>= 3` 同一写法，看的是**快照**的算法版本），常数 `heatUsefulCrestDb = 0.1` 写进 v8 出厂值并 `optionalField` 序列化；旧快照永远不进新分支，阈值缺省值无所谓。
2. **`oldestCompatibleAlgorithmVersion` 保持 6 是对的，但要改注释**：`GlazeTypes.h:28–33` 的字面规则说「改了既有增量的行为就把本常数跟着升」。这里行为按快照版本分流：v6 / v7 存档重开时 `committed.constants` 就是存档里的快照（`GlazeController.cpp:2749–2759` 只在没画像时换成 live 常数；`:3075` 的 Heat 迁移只针对算法 < 6），重算方案走旧分支、增量逐位同存档，所以「行为相同」成立。核过升版不会连带丢校准 / 执行快照：`calibrationApplicable`（`:1521`）只比参考条件；`sameCalibrationPlan`（`:158–170`）只在 `adoptExecution`（存档 vs 存档）与 `getStatus` 用；`executionHeld` 只在参考条件变时释放（`:2326–2340`）。
3. **`testStyleDirections`（`tests/GlazePlannerTests.cpp:884`）必改**：「Heat 温润 > 冲击 > 通透」现在靠上限饱和成立，改后三者都是 12 会失败。建议把 `automaticHeatFloor` 挪成 `StyleConstants` 字段走 `blendConstants`（出厂先都填 12、断言改比 `heatDriveCap`），让「风格 Heat 差异」变成 Max 试听时可标定的常数（研究建议的 12 / 8 / 4 只是候选值，**不要今晚替 Max 定**）。这与 `决策与校准` §1「温润 Heat / Glue 为主」不冲突——差异由常数体现而不是由 bug 体现。
4. 同步：`GlazeConstants.cpp:46–51` 头注释加 v8 一行、`GlazeTypes.h:26` 注释、`设计规格-GLAZE.md` 7.4 版本段；`tests/LegacyFixtureTests` 与 GlazePlannerTests 全套在 107 跑（本机不跑）。
5. 风险声明要写进日志：新实例 / 重新聆听下温润 GLAZE 100 的 Heat 从 70 降到 12、默认 35 % 从 24.5 降到 4.2，是可闻变化；Max 对两种都没试听过（规格 7.5 验收 7）。这是把实现拉回规格（底量 12），不是新的产品决定，但要标「待试听」。

优先级：P0 维持。理由：它是决策器的核心映射，且 R6-02 的触发面（`check()` 的 `heatBudget = fullPlan.heat > 0`，`:1447`）也由它决定。

---

## 2. R6-02 染色预算触发的「受手动限制」关掉整套安全层 — confirmed（high / P0）

### 我找到的同样证据

1. `GlazePlanner.cpp:1447` `withdrawn = check(ctx, manualChain, fullPlan.neutral, fullPlan.heat > 0)`；`:583–585` `v.heat = heatBudget && heatAmount > cap + 0.5 || …`；`:1482` `limitedByManual = withdrawn.any() || ctx.limitBypassed`；`:1484–1487` 只有 `withdrawn.limit || limitBypassed` 才 `strength = 0`。Heat / Glue / Clip 被撤回时 `autoSettingsAt`（`:950–957`）只把该模块置 0，Gain 照 `limitGainAtStrength` 展开。
2. **规划器自己的测试就断言了这一点**：`tests/GlazePlannerTests.cpp:1207–1213` heavyHeat 用例：`limitedByManual` 为真 **且** `limitGainDb > 0`（注释「其它模块照常工作」）。
3. 编排器七处门逐一核实：`GlazeController.cpp:1588`（子窗作废）、`:1621`（不开校准轮）、`:1633`（`biasTarget` 返回 0）、`:1845`（失配不判）、`:1922`（约束反馈不做）、`:2160`（不施加偏差与保护，注释「自动增量本来就撤回了」）、`:2576`（保护层不跑、`guard.reset()`，注释「没有自动加上去的增益可收回」）。`:2170–2175` 保护层能收的正是 `afterIncrement − manualGain` 这份自动 Limit Gain——在 heavyHeat 场景里它 > 0，却被门挡在外面。
4. 控制器测试只覆盖手动 Gain 20 dB（`tests/GlazeControllerTests.cpp:2789–2799`，`withdrawn.limit` 路径），没有「Heat / Glue 单独超预算」的用例。
5. 档案侧：`设计规格-GLAZE-实现修订.md` §3「受手动设置限制时，不施加偏差，也不施加保护」是 2026-09-25 阶段 0～3 写的；同页 §5 F-02（同日晚验收）把 Heat / Glue 改成「逐模块钳住、不再拖动共用强度」——§3 那句的前提（受限 = 自动全撤）从 F-02 起就不成立，但 §3 与编排器都没跟着改。`决策与校准` §7「学习、过渡、校准期间保护层同样生效」是硬要求。所以这不是规格已定取舍，是 F-02 修订漏改的下游。
6. 触发面：通透 `heatDriveCap = 20`（`GlazeConstants.cpp:96`），自动风格按权重混合常在 30 ～ 50；出厂表下 `heatBudget` 几乎恒为真（底量 12 > 0，Color 开、Mix > 0 即可）。手动 Heat 21（通透）就触发，属于正常操作范围。

### 修法核验（可用，需补 3 处）

研究的方向（`Plan` 加 `automaticLevelWithdrawn = withdrawn.limit || limitBypassed || maxGain < manualNetDb`，七处门改看它，`limitedByManual` 留给界面）正确。补：

1. **序列化**：`Plan` 经 `Source/dsp/GlazeProfile.cpp:150–171` 写进存档（`limitedByManual` 在 168 行）。新字段用 `optionalField`，**缺省值取 `limitedByManual`**（旧语义 = 保守），不是 false：`savedPlanMode`（算法 < 6 的存档）照用保存的方案，缺省 false 会让保护层 / 校准对一份「自动全撤」的旧方案照常开工。兼容算法的存档下一块就重算，缺省值无影响。
2. `limitPlanTo`（`GlazePlanner.cpp:1622`）要把新标志一并取或。
3. 诊断掩码 `calibrationInterruptMask` / `calibrationGateMask` 第 16 位（`:1586`、`:1615`）改看新标志；grep 测试里对这两个掩码位的引用一并核。`GlazePlanner.h:17`、`GlazeController.cpp:2159–2161` 与 `:2573–2575` 注释、`实现修订` §3 措辞改「电平类自动全部撤回时」。

验证按研究报告：控制器新用例（热素材 / 通透 / GLAZE 100 / 手动 Heat 25 → 状态受限为真，`executedGuardDb < 0`、`calibrationBegins ≥ 1`），手动 Gain 20 用例 0 / 0 不变；规划器 heavyHeat 补 `!automaticLevelWithdrawn`，heavyGain / bypassed 补 `== true`。LegacyFixture 存档 `limitedByManual="0"`，不受影响。

优先级：P0 维持——安全层被一个常见的手动设置静默关掉，修法只影响「手动超预算 + GLAZE > 0」的组合，不动逐位契约。

---

## 3. R6-03 自动权重把「LRA 未知」当「不动态」 — confirmed（缺陷成立），修法不可用（medium / P2，待 Max）

### 我找到的同样证据

1. `GlazePlanner.cpp:1253` `dynamic = ramp(LRA, 5, 12) × lraTrust`；`:1259` `raw.warm = base + (1 − transient)(1 − dynamic) × 0.7 + …`。`:1249` 注释明写「特征不可信时按『没有这个特征』算（冲劲 0、动态 0、不偏亮）」——「动态 0」进 `(1 − dynamic)` 就变成温润的满额证据。出厂常数（`GlazeConstants.h:128–138`）base 0.15、sustain 0.7、LRA 斜坡 5 → 12。
2. LRA 置信度 = `min(代表性, rangeSeconds / 60)`（`MusicAnalyzer.cpp:69、2138、2180、2544`）；聆听结束条件不含 LRA（`GlazeController.cpp:1201–1215`）；`commitCandidate`（`:1219–1235`）锁权重后 `stopLearning`。LegacyFixture 存档里 15 s 提交的 `loudnessRangeLu_confidence="3e4e81b5"` = 0.20，比研究说的 0.25 还低。
3. 手算复现：持续型（transient = 0）、LRA 15、置信 0.25 → lraTrust 0.3125、dynamic 0.3125 → raw = 0.63125 / 0.15 / 0.4625 → 归一 0.5075 / 0.1206 / 0.3719，与研究报告 0.508 / 0.121 / 0.372 一致。LRA 无效 → 0.739 / 0.130 / 0.130。
4. 规格：`分析器` §6「『不知道』不当 0 … 短时间测得的低 LRA 不作为强证据」；`决策与校准` §1「置信不足时偏向通透」。既有测试（`tests/GlazePlannerTests.cpp:1295–1375`）的 `sustained` 用例 LRA 3 / 置信 1、`dynamic` 用例 LRA 15 / 置信 1，确实没有覆盖「持续型 + LRA 未满」。

缺陷成立：实现把「不知道」折成「不动态」，与分析器 §6 相反，且锁定后永久。

### 为什么研究的修法不可用

研究把持续项拆成 `sustain × (1 − dynamicRamp) × lraTrust`（归温润）与 `sustain × (1 − lraTrust)`（归通透）。在标准时间线上 LRA 置信度在提交时**结构性地 ≤ 0.25**（有播放头 15 s；无播放头 30 s 也只有 0.5），lraTrust ≤ 0.31（0.625），于是：

- 参考类音乐素材（`tests-release.log:593`：起音 0.95 次/s、**测得 LRA 0.5**、置信 ~0.2）：现在锁定 0.48 / 0.14 / 0.38（温润主导）；按研究公式手算 → 约 0.33 / 0.33 / 0.55（通透主导）。测得的 LRA 确实很小、素材确实持续型，权重却因「置信度没攒满」翻成通透。
- 研究自己的表（7.4 节第一行）：LRA 3 / 置信 0.25 → 0.321 / 0.130 / 0.549 通透。

这正是 `设计规格-GLAZE.md` 7.1（第 2 轮，「本节与子文档冲突以本节为准」）明文消除的「自动风格被没攒满的 60 s LRA 置信度整体压向通透」：修法之后「自动」在正常时间线上几乎选不到温润，温润只剩手动风格。研究报告把根治（提交后继续累计 LRA 到 60 s、置信首次达 0.8 时重算一次权重）列为「留给 Max」，但把公式改动列为可落地项——两者拆开落地就是回归。

### 调整后的建议

- 今晚**不改公式**；列「待 Max」。两条候选一并给 Max 拍板（都挂算法 8 版本门）：
  - (a) 中性先验：`dynamicEff = lerp(0.5, ramp(LRA), lraTrust)` 同时进温润项 `(1 − dynamicEff)` 与通透项 `dynamicEff`——「不知道」= 一半一半，既不当 0 也不当 1。手算：类音乐 LRA 0.5 / 置信 0.2 → 温润仍略占先（约 0.39 / 0.24 / 0.37）；古典 LRA 15 / 置信 0.25 → 通透主导（约 0.24 / 0.09 / 0.67）。既有 10 组断言我逐条核过仍成立（`sustained` / `dynamic` 都是置信 1，不受影响；`half` 用例 warm 略降、clean 略升，方向与断言一致）。
  - (b) 时间线层：提交后分析器继续累计 LRA，置信首次 ≥ 0.8 时重算 f(LRA) 与权重一次（属完成同一次学习，不是重学）——改 `设计规格-GLAZE-时间线与保存`，必须 Max 拍板。
- 严重度 medium 维持；优先级降 P2：R6-01 修掉后，本项的可闻后果只剩 L_style 约 0.9 dB 与 Glue / Limit 上限的风格差，不再叠加「Heat 49」。

---

## 4. R6-04 实时 Tame 许可不乘 κ、未学习也生效 — confirmed（low / P2）

### 我找到的同样证据

1. `GlazeController.cpp:2110` 条件 `active && !savedPlanMode && !manual.balance.bypass && toSlot().valid`；`:2120–2121` `liveTame = tameCap × glaze × clamp(evidence / tameEvidenceFullDb)`，没有 κ。
2. `toSlot().valid` 在任何一次 `evaluate` 后为真（`:1080–1100`），包括 `profile == nullptr` 的未学习保守方案。
3. 对照同一文件 `updateLiveTone`（`:2049–2076`）：门在 `committed.hasProfile`，强度乘 `confidenceScale(&committed.profile, c)`。方案侧 `GlazePlanner.cpp:1527、1565` 的 Tame 增量乘 `glaze × κ`。同一物理量三条路，只有实时 Tame 一条既不看学没学过也不乘 κ。
4. 实时证据 `liveTameEvidenceDb`（`MusicAnalyzer.cpp:1899–1909`）只要过了 settleFrames 就有效、置信恒 1，与是否已提交画像无关。
5. 规格 7.2「学习结束后继续用 Balance 帧算短期突出证据」——未学习就放许可是偏离。

### 修法核验（可用，需核一条既有测试）

乘 `confidenceScale(committed.hasProfile ? &committed.profile : nullptr, snapshot)` 并把 `committed.hasProfile` 加进 2110 行条件是对的；`restoredTamePending` 恢复路径（`:2126–2130`）用执行快照里的实际值，不变。注意 `tests/GlazeControllerTests.cpp:2315–2380`「locked profile: equal-loudness spectral change increases live Tame」在 fastConstants 下只学 4 s 后断言 `highest > baseline + 2.0`：若该画像的 κ < 1，乘 κ 后余量会缩，实现时要在 107 跑这一条确认；上界断言 `≤ 0.5 × clean.tameCap` 不受影响。

优先级 P2 维持，可今晚做也可推后；Tame 是收敛型处理，风险低。

---

## 5. R6-05 3 dB 增响底量绕开 −7 LUFS 上钳 — refuted（不是缺陷；low / P2 试听待定项）

### 证据

1. 代码事实成立：`GlazePlanner.cpp:1417–1418` `requestFull = max(driveFloor, nonNegative(preferred − inputLufs))`，底量不受 `[−14, −7]` 钳。
2. 但这是**规格已定、且有测试固化的行为**：
   - `设计规格-GLAZE.md` 7.1「增响请求下限 3 dB，**已达到风格偏好响度的歌也保留**」；7.5「验收 3『已够响的素材请求增益为 0』：被 3 dB 请求下限取代」；7 节开头「本节与子文档（含实现修订）冲突时，以本节为准」。研究引用的 `决策与校准` §2 上钳属于被 7.1 覆盖的子文档条款。
   - `tests/GlazePlannerTests.cpp:721–737`「Already loud material retains a bounded monotone drive request」：`makeProfile(−6, −1.5)`（比三种风格偏好都响）断言 `requestedGainDb == 3 × GLAZE/100`，并要求 `previousGain > 0`（「a hot master is not a dead region of the macro」）。研究的修法会让这条测试失败。
3. 研究报告自己写的「Codex 的产品决定，Max 未试听」属实——但「Max 未试听」对 7 节的所有条目都成立（7.5 验收 7），不构成把它单列为缺陷的理由。

### 判决

不是缺陷。「成品母带 −5 LUFS 是否还该再推 3 dB」是试听问题，放进「待 Max 拍板」清单（与 L_style、风格常数一起），不进今晚实施集。若 Max 拍板要钳，研究的 `min(driveFloor, maxPreferredLufs + 1 − I)` 写法可用，但要同步改 7.1 与那条测试。

---

## 6. R6-06 满强度增益不受 Limit Gain 24 dB 量程约束 — confirmed（low / P2），修法需扩到手动 Gain

### 我找到的同样证据

1. `GlazePlanner.cpp:772` `fullGain = min(gain, 24)` 只用于 Clip 爬坡与折点顶值（`:791`）；`:895` `full.gainDb = gain`（净增益 G_full′）没有回写；`:1571` `effectiveGainDb = manualNetDb + strength × fullPlan.gainDb`。折点 `limitGainDb[4] = nonNegative(fullGain)` 最多 24（`:791`），所以增量本身没错，错的是宣称的净增益与目标。
2. `requestFull`：`driveFloor` 钳 [0, 24]，`preferred − inputLufs` 不钳，`inputLufs` 钳 [−70, 0] → I −40 时请求 30 dB。研究实验 H 的数字与代码一致。
3. 校准器目标 = `effectiveGainDb`（`:1537` `calibrator.begin(toSlot().plan.effectiveGainDb, correctionLimitDb(), …)`），修正范围 `calibrationMaxDb × GLAZE × κ`（`:1509–1517`）→ 目标不可达时最后一轮不收敛 → `partial`（`GlazeCalibrator.cpp:13`）。机理成立。

### 研究漏掉的一半

可交付的自动 Limit Gain 不是 24，而是 **24 − 手动 Gain**：`combine()`（`GlazePlanner.cpp:960–966`）与 `applyIncrements`（`:1739`）都把「手动 + 自动」钳到 24，`feasible()` 用钳后的值核约束，但 `effectiveGainDb` 没有。于是手动 Gain 12 dB + 通透请求 14 dB（I −24）同样虚报：实际 Limit Gain 24，宣称净增益 ≈ 12 + 14 = 26。影响范围不止「I < −34」。

### 调整后修法

在 `completeFullPlan` 里以 `capGain = 24 − manualSettings.gainDb`（需把手动 Gain 放进 `Context`，`makeFullPlan(ctx, requestFull)` 目前拿不到手动值）为自动 Limit Gain 顶值：`gain > capGain` 时 `full.gainDb = estimateLimit(ctx, color, capGain, clip).netDb − full.neutral.netDb`、`constrained = true`，折点顶值同改。研究的第一种写法（固定 24）漏了手动 Gain；第二种「`requestFull` 钳到 24 − 手动 Gain」在净增益与 Limit Gain 之间少了损失项，只是近似。测试加两例：I −40 / TP −28 / 通透 / GLAZE 100，以及 I −24 / 手动 Gain 12；都要求 `effectiveGainDb ≤ 可交付净增益 + 0.5` 且 `limitedByConstraints`。I −30 用例数值不变。

严重度 low / P2 维持（极安静素材或手动大 Gain 才碰到；只影响显示与校准结束状态，不影响输出安全）。

---

## 7. 漏掉的 / 需要补的项（missed）

1. **R6-01 起点修正**：Heat 顶到上限从算法 3（第 2 轮并联表）就开始（`heat-revision/tests-third.log:507` 温润 70.0），残差表（算法 6）只是让三种风格都精确顶满；LegacyFixture 的 v0.1.6.2 存档 `colorHeat` ≈ 12.0 = 20 × 0.6 是第四份自证。落地时日志按此写。
2. **R6-01 修法的隐患**：「`heatUsefulCrestDb` 缺省 0 = 旧行为」不成立（−18 dBTP 行会从 0.00008 dB 的 crest 反查出 Heat 70）；版本门是唯一开关，实现时不得把阈值当兼容手段。
3. **R6-02 修法的三处遗漏**：`Plan` 新字段要进 `GlazeProfile.cpp` 序列化（缺省 = `limitedByManual`）、`limitPlanTo` 取或、两个诊断掩码第 16 位改看新标志。
4. **R6-03 修法会回归规格 7.1**（见第 3 节）：不能拆开落地，今晚只列待 Max。
5. **R6-06 范围扩大**：手动 Gain + 自动请求 > 24 dB 时同样虚报，修法要按 `24 − 手动 Gain` 钳。
6. **`heatBudget` 语义（低优先级设计问题，待主进程裁决）**：`GlazePlanner.cpp:1447` 用 `fullPlan.heat > 0` 判「自动在用 Heat」，出厂表下底量让它恒为真，于是手动 Heat 只要超过风格驱动上限（通透 20）就撤回底量并亮「受手动设置限制」——即便方案只想给 12 的底量。R6-01 修掉之后可考虑改成 `peakHeat > 0`（自动真的拿 Heat 削峰才算预算），手动超上限时 `heatAllowance` 本来就是 0（`:1455`），不会多给。这会减少 R6-02 的触发面，但改了 `limitedByManual` 的界面语义，需要主进程定。
7. **本核验没做的事**：没在 107 重跑研究代理的数值实验（R6-01 / R6-03 的关键算术已手算复现，其余以代码与仓库日志为证）；GlazePlannerTests / GlazeControllerTests / LegacyFixtureTests 在修改后必须在 107 跑全套——特别是 R6-01 的 `testStyleDirections`、R6-04 的 live Tame 用例。

## 8. 建议的今晚实施集（供主进程综合）

1. R6-01（算法 8 版本门 + `heatUsefulCrestDb` + 底量挪进 StyleConstants 出厂 12/12/12 + `testStyleDirections` 改比上限 + 规格 7.4 / GlazeTypes 注释）。
2. R6-02（`Plan::automaticLevelWithdrawn` + 序列化 + limitPlanTo + 七处门 + 掩码 + 控制器新用例 + 实现修订 §3 措辞）。
3. R6-06（按 `24 − 手动 Gain` 钳 + 两条测试）与 R6-04（乘 κ、门 hasProfile、核 2315 行用例）可随手做。
4. R6-03、R6-05 写进 TASKS「待 Max 拍板」，附本报告第 3、5 节的候选方案与数字。
