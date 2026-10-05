# T-078 交接包：插件界面文字与注释跟上「曲风 / 交付」

> 写于 2026-10-05 13:2x（PDT）· 会话「插件文字跟上曲风交付」（DGX Spark，Sonnet 5.5 主进程）。
> 放在 GitHub 是 Max 2026-10-05 的要求：**把任务交给别人，交接文档要推送给 GitHub**——DGX 上的 vault（Obsidian）没有同步到别的机器，接棒方读不到；vault 里的 TASKS / 日志只留指向本文的指引。
> 本文随提案进展更新（同一路径、同一分支再推）。**当前状态：提案阶段，一个插件字符串都没改。**

## 1. 目标

让 Celadon 插件里**写给使用者看的文字**（悬停气泡、左下角提示、状态行）与**代码注释**，跟上 0.1.8.8 的「曲风 / 交付」两个选择，以及 T-077 两轮独立核对证实的事实；改完在官网对应处同步（官网各卡片页的表格自称「就是插件悬停气泡的文案」，气泡改了表格要一起改）。

第一步只出**提案列表**：每条写「现状原文（中英）+ 建议新文 + 理由 + 受影响的官网页 + 会连带改的测试」，**等 Max 点头才改插件字符串**。

## 2. 已定事项（接棒方不得重开）

九条都是 T-077 两轮只读独立核对（`wf_74d96e24-d37`、`wf_2e0f94f5-b05`，共 192 条发现全部裁决）用代码证实的。行号是核对当时的；本会话已在 `f47b996` 上逐处读过，**与下面写的一致**（事实本身沿用 T-077 的证实，没有重新核）。

1. `Source/ui/HelpText.cpp:43 / 46`：Ceiling 气泡建议「上流媒体用 −1.0；想更保险用 −1.5」（英文 `Use −1.0 for streaming; −1.5 to be extra safe.`）与交付「流媒体」把有效上限封在 −2 dBTP（`Source/dsp/CeladonChain.cpp:205-212`）并存，读者会困惑。官网 `site/src/content/docs/{zh,en}/limit.md` 第 8 行照抄这句，`interface.md` 承诺「各卡片页表格 = 气泡文案」。
2. `Source/ui/Localisation.cpp:95`：左下角「GLAZE 为 0：全部手动」（`GLAZE at 0: fully manual`）不准——GLAZE 为 0 时流媒体封顶与 House 限幅释放跟拍仍生效（`CeladonChain.cpp:205-212、236-239`）。
3. `HelpText.cpp:239-243`：重新聆听气泡写「再听 15～30 秒」，House 类曲风下要重新凑够 64 小节、通常两分钟上下（`Source/dsp/GlazeController.cpp:1394-1404、1531-1539`）。
4. `Source/PluginEditor.cpp:1369`：House 锁定后左下角第二行固定写「已聆听 15 秒」（用 `status.requiredSeconds`；`GlazeController.cpp:3317-3320` 在不聆听时恒设为 `minListenSeconds`），实际已听 ≥ 64 小节。
5. `HelpText.cpp:951-955`：House 聆听气泡写「在那之前方案是暂定的」，重新聆听（旧方案照常生效）时也会显示（`Source/ui/GlazeStatusText.cpp:488-491`）。
6. 过期注释：`HelpText.cpp:219`、`PluginEditor.cpp:215-216` 仍写两个选择在「主旋钮左右」，实际在 GLAZE 标题行左右（`PluginEditor.cpp:482-493`，Max 2026-10-01 选定）。
7. `PluginEditor.cpp:1209`：显示用的 Ceiling = 参数 − 编码余量，没算流媒体的 −2 dBTP 封顶，所以 OUT 表的上限线、响度小窗的 Ceiling、「超过 Ceiling」橙色提示（`PluginEditor.cpp:1243、1254、1412-1426`）在流媒体档按用户 Ceiling 算，而限幅器实际按 −2 封顶。
8. `HelpText.cpp:223 / 231`（英文 226 / 234 行；以及 T-076 的 0.1.8.8 更新日志，那是 T-076 的文件，只报告）写「照原来 / as before」：0.1.8.8 同时带着 T-045（三种风格限幅预算放宽，ADR-029），「通用 + 标准」的听感已不同。
9. 反相提示亮着时整行占住 GLAZE 状态行（`GlazeStatusText.cpp:329-341`），阶段标签又把「待校准」也画成已锁定（`PluginEditor.cpp:1328-1331`）：低频反相的 House 曲在 drop 里看不到校准是否完成。

## 3. 未定事项（**待 Max 拍板**，接棒方不要自己定）

1. 每一条改成什么措辞（提案列表里给建议，Max 点头才改）。
2. 第 7、9 条要不要改界面行为——属完成标准 / 模块边界（是否新增 / 改变接口、改哪些测试断言与截图基线）。
3. 范围外的两条候选要不要并入 T-078（范围变大归 Max 定）：
   - 第 10 条：「受限」气泡 `HelpText.cpp:247-251`「受约束表示已自动保护动态；想保留更多起伏就调低 GLAZE」。顶栏阶段标签「受限」有两个来源（`PluginEditor.cpp:1326-1327` 把 `planLimited` 与 `limitedByManual` 画成同一个标签），气泡只对前一个成立，且与状态行用词不一致（「受约束」对「受限」）。T-079 会话提出，措辞要与它的提案 A 组定稿对齐。
   - 第 11 条：`tests/GlazePlannerTests.cpp:2900` 注释「两版母带的音色决策一致，规格第 4.3 节」——该测试只断言 Tone / Clay / Recover / Tame / 风格权重相同，没断言 Glue / Heat / Clip（T-077 台账：Glue / Heat / Clip 随增益与上限重新分摊）；2916 行断言消息「交付不改音色决策」同句。插件侧同根因只此一处（本会话 grep 过 `Source/ tests/ tools/ docs/`）。
4. 合并方式与顺序：T-077（分支 `claude/t077-site-genre-delivery-docs`，`f47b996`，未合 main、未部署）→ T-078 / T-079 串行 merge，后合方 rebase；文档写「0.1.8.8 起」而 main 的 `release.ts` 仍是 0.1.8.7，页面先于安装包上线的问题归 Max（T-077 台账「交 Max 的清单」第 1 项）。

## 4. 必读文件（路径 + 版本）

**仓库**（分支 `claude/t078-plugin-text-genre-delivery` = main `a32a3db` + T-077 的 33 个官网文件 `f47b996` + 本交接提交；插件源码与 main 相同）：

- 插件：`Source/ui/HelpText.cpp`（40-46、219、221-235、237-243、245-251、944-959）、`Source/ui/Localisation.cpp:95`、`Source/PluginEditor.cpp`（215-216、482-493、1209、1243、1254、1317-1332、1359-1371、1412-1426）、`Source/ui/GlazeStatusText.cpp`（324-342、488-491）、`Source/dsp/CeladonChain.cpp`（197-212、236-239）、`Source/dsp/GlazeController.cpp`（1394-1404、1531-1539、3300-3330）、`Source/dsp/GlazeTypes.h`（399-413 的 `house*` 状态字段）。
- 钉着这些字符串的测试：`tests/HelpTextTests.cpp`（`HelpText` / `HelpTextClay` / `HelpTextHouse`）、`tests/LocalisationTests.cpp`（236-264 曲风 / 交付与 `cornerHouseTip`）、`tests/GlazeStatusTextTests.cpp`；截图检查 `tools/ui-snapshot/SnapshotMain.cpp`。
- 官网（T-077 版）：`site/src/content/docs/{zh,en}/{limit,interface,glaze,meters,quickstart}.md`、`site/src/content/algorithms/{zh,en}/{glaze,limit}.md`、`site/src/i18n/glossary.md`；用 `git diff a32a3db HEAD -- site` 看 T-077 现在怎么写。我 grep 到的、与气泡同文或引用它们原文的位置（**待核对，不保证列全**）：`limit.md:8`（Ceiling 表格）、`interface.md`（zh 约 22 行引用「GLAZE 为 0：全部手动」、约 44 行「表格就是这些文案」）、`glaze.md`（zh 约 131 行同引用；en 约 35 行重新聆听「15–30」；两语约 64 行曲风表「照原来 / works as before」）、`quickstart.md`（en 23 行已写 House 要重新凑 64 小节）、`algorithms/{zh,en}/glaze.md`（约 90、101 行的「标准档照原来 / as before」）。
- 另见一处同根因（第 6 条）：`tools/ui-snapshot/SnapshotMain.cpp:661` 注释「两个选择在主旋钮左右（代码里的默认位置）与备选位置（挪到 GLAZE 标题行）」——要判断它是不是历史对比说明。

**档案库**（在 DGX 的 vault `/home/maxsong/Documents/Max's Secret Place/项目/Celadon/`；别的机器读不到时，要点都已写进本文）：`04-协调/归档/2026-10-05-T-077-独立复查裁决.md`（证据台账与「交 Max 的清单」）、`03-记忆/ADR-026-曲风与交付拆成两个开关.md`、`ADR-029-三种风格限幅预算一起放宽.md`、`踩坑-官网说明页的绝对说法要对着代码核对.md`、`01-框架/设计规格-曲风与交付.md`、`设计规格-曲风-House学习方式.md`、`04-协调/2026-10-05-T-079-官网与档案旧表述提案待Max拍板.md`（T-079 的提案初稿，A 组是「受限」，未经独立核对）。

## 5. 验收标准（二元可判）

1. Max 点头的每条已改，`HelpText` / `HelpTextClay` / `HelpTextHouse` / `Localisation` / `GlazeStatusText` 等相关测试在 DGX 上过（`spark-test.sh`，走全机锁；只算相对基线的新失败）。
2. 官网对应表格同步改，`site/scripts/build-linux.sh` 过（含 `check-i18n` 的 ADR-023 与站内链接 / 锚点）。
3. 完成报告用四档状态词（没开始 / 代码改完·未验证 / 已实际验证 / 你确认完成），逐条列没验证的和跳过的。

## 6. 禁区

- 不碰 T-076 拥有的文件：`site/src/content/{release,changelog}.ts`、`site/src/components/celadon/Stats.tsx`、`site/src/content/algorithms/*/verification.md`、`site/src/i18n/locales/*/home.json`（只报告）。
- 与 T-079（官网旧表述订正，另一会话）已划界：它在 `glaze.md` 只动旋钮表 / 状态表的「受限」行与「交付」节「目标是方向，不是保证」一条；与气泡同文的表格、`limit.md`、`interface.md`、左下角提示、重新聆听 / 导出相关句、曲风 / 交付表里「照原来」两行都归 T-078。两线都基于 `f47b996`，后合方 rebase。
- 不在 Mac 编译；不合 main；不部署官网（部署 112 只有 Max 能跑）；开 PR 前先问 Max（CI 每个 PR 与每次 main push 各跑两遍全套，约 12 分钟 × 2，纯文字别单开）；推送自己的分支不触发 CI；不改 T-077 分支本身（T-078 只是叠在它上面）。

## 7. 约束（引用，不复制）

全局规则里的：永远说中文、验证后才声称完成、范围外问题上报不擅改、三件事（完成标准 / 模块边界 / 出错时谁负责）由用户拍板。写法原则（T-077 教训，`踩坑-官网说明页的绝对说法要对着代码核对`）：绝对说法（唯一 / 永远 / 一定 / 完全 / 都 / 不会 / 只有）先回代码找反例；不写「相对过去」的说法（照原来 / as before / 与 0.1.8.7 一样），改写成「它是什么」；不提 T-045 本身、不写「更响多少」（预算还没试听）；步骤完成标志写成正向状态；不出现对标商业插件名（ADR-023）；英文用英式拼写，术语照 `site/src/i18n/glossary.md`；符号沿用现有字符串（减号 U+2212「−」、全角标点、「～」）。

## 8. 当前状态与验证矩阵

状态：**没开始**改字符串；提案列表还没写。已做：认领 T-078（TASKS ①）、开分支、与 T-079 划界、启动只读核对 workflow、跑了基线。

| 项 | 结果 |
|---|---|
| 基线编译 `celadon_tests`（Spark，`f47b996`） | exit 0，0 警告（只有 GCC 的 C++17 传参 ABI 提示 note） |
| 基线定向测试 `--only HelpText,HelpTextClay,HelpTextHouse,Localisation,GlazeStatusText` | **16970 过 / 0 败** |
| 九条的现行行号 | 已在 `f47b996` 上读过，与卡片一致；事实沿用 T-077 的证实，**没有重新核** |
| 核对 workflow `wf_b6e0f78c-737` | 13:11 起在 DGX 后台跑，13:25 时 4 个起草代理还没交出；**没有结果** |
| 提案文字 | 还没写；写好后放 `PROPOSALS.md`（同目录）再推一次 |
| 全套测试 / 官网构建 / 截图检查 | **没跑**（改完统一跑） |

## 9. 占用资源

- 任务编号 T-078（TASKS ① 已登记；最后心跳 2026-10-05 13:2x，租约到 2026-10-07）；T-079 是另一会话的号。
- 分支 `claude/t078-plugin-text-genre-delivery`：推送前只在 DGX 本地；推送后在 `origin`。
- 工作树 `/home/maxsong/All-Project/Celadon/.claude/worktrees/heuristic-lewin-8f3e56`（DGX Spark，**只在那台**）；Spark 构建目录 `~/.cache/celadon-spark/build/heuristic-lewin-8f3e56-*`（`celadon_tests` 已编）。
- 后台：只读 workflow `wf_b6e0f78c-737`（9 个 Opus 5.5 · max 代理：4 簇起草 → 4 个对抗核实 → 1 个仲裁查漏；脚本存在本目录 `proposal-workflow.js`，可用 `Workflow({scriptPath})` 重跑）。结果在 DGX 会话目录 `~/.claude/projects/-home-maxsong-All-Project-Celadon--claude-worktrees-heuristic-lewin-8f3e56/7bed7d5e-efeb-4283-8c38-f12a966125b2/subagents/workflows/wf_b6e0f78c-737/journal.jsonl`，不在 GitHub；原会话会在结果回来后写进 `PROPOSALS.md` 再推。
- 没占 107 / 109；官网构建要用 107（`site/scripts/build-linux.sh` 经 ssh）时先在 TASKS ② 登记。

## 10. 接续命令

```bash
# 取分支（别的机器 / 会话）
git fetch origin claude/t078-plugin-text-genre-delivery
git worktree add -b claude/t078-plugin-text-genre-delivery ../t078 origin/claude/t078-plugin-text-genre-delivery

# DGX 上的基线定向测试（只算新失败；基线 16970 过 / 0 败）
~/.cache/celadon-spark/bin/spark-test.sh "$PWD" test --only HelpText,HelpTextClay,HelpTextHouse,Localisation,GlazeStatusText

# 官网构建（点头改完官网表格之后）
site/scripts/build-linux.sh
```

接续顺序：① 读 `PROPOSALS.md`（若已存在）；没有就读 journal 或重跑 workflow；② 把「待 Max 拍板」逐题问 Max（先 7、9 两条）；③ 点头后按条改插件字符串与测试，同步官网表格；④ DGX 跑定向测试，107 构建官网；⑤ 让只读代理逐句对代码再核一遍；⑥ 完成报告。

## 11. 接续提示词（可直接粘进新会话）

```
【开卡规矩】本会话开着 Ultracode，用开卡时选的模型直接开工，不自查模型、不停下来问。要派多个代理一律用 Workflow，直接开，模型和 effort 用 Opus 5.5 + max，不每次问用户。

【任务】接手 T-078：让 Celadon 插件里写给使用者看的文字与代码注释跟上 0.1.8.8 的「曲风 / 交付」，改完在官网对应处同步。先读仓库分支 claude/t078-plugin-text-genre-delivery 里的 docs/validation/2026-10-05-T078-plugin-text-genre-delivery/HANDOFF.md（目标、九条已定事项、待 Max 拍板、禁区、验收标准、接续命令都在里面），再读同目录 PROPOSALS.md（若已存在）。九条已定事项不重开；每条改成什么措辞、第 7、9 条要不要改界面行为、第 10、11 条候选要不要并入，都等 Max 点头，不要自己定。不碰 T-076 的文件；不在 Mac 编译；不合 main、不部署官网；测试在 DGX 用 spark-test.sh，官网构建用 site/scripts/build-linux.sh。开工先读 vault 项目/Celadon/04-协调/TASKS.md 查重（vault 在别的机器读不到时以本文为准），再把自己的认领写进去。
```

## 12. 回执（接棒方交回来时填）

状态（completed / failed / stopped / abandoned）｜产物路径｜和别的认领撞了什么｜没做完的（每条带原因）｜待用户拍板（碰到三件事没自己定的，逐条列）。
