# T-076 发布 0.1.8.8 · 交接包

> 交出方：会话「继续完成剩余任务」（DGX Spark，Sonnet 5.5） · 交接时间 2026-10-05 13:3x PDT（Spark 的钟）
> 原因：Max 2026-10-05「把任务交给别人，交接文档要推送给 github」。
> 本文件所在分支 `claude/t076-handoff`（基于 main `8bcf8da`），**不开 PR**（纯文字单开 PR 会多跑两遍全套 CI）。任务收尾后这条远端分支删不删由 Max 定，别自己删（删远端分支属难撤回清单）。
> 字段照 max-core `handoff.md` 的交接包；做完后把文末「回执包」填好，写进 vault 的 T-076 行与当日日志。
> 这是 13:3x 的快照：CI、Max 的答复可能已变，**开工第一步先按 §11 重读现状**，别信本文的「当前」。

## 1 目标

把已经做好的 0.1.8.8（三个安装包 + 官网内容）上线到生产站 112，并在公网核对与发布目录一致，让 Max 能说「完成」。这一棒要走完：

1. 拿到 Max 对 §4 六个问题的答复；
2. 合并 PR [#52](https://github.com/Maxsong-0/Celadon/pull/52)（草稿，复查后的措辞修正）；
3. 从合并后的 main 重建官网并核对；
4. 把部署命令交给 Max（**只有他能跑**）；
5. 公网核对；
6. 收尾档案（§10）。

## 2 已定事项（接棒方不得重开）

1. 版本 0.1.8.8，三个包已打好、已核对：`/home/maxsong/All-Project/Celadon/artifacts/release-0.1.8.8-2026-10-05/`（**只在 Spark 上，不入 git**）：`Celadon-0.1.8.8-macOS-AppleSilicon.pkg`（7,355,206 B，`2018dc3f…`）、`Celadon-0.1.8.8-macOS-Intel.pkg`（8,215,728 B，`b5d0cd94…`）、`Celadon-0.1.8.8-Windows-x64-Setup.exe`（7,542,830 B，`721c8af7…`）、`SHA256SUMS`（3 行，307 B）。
2. Mac 编译由 Max 本人手动做（ADR-002）。Mac 包从 main `a32a3db` 编，约 10:49 PDT，**在 PR #28 合并之前**。`auval -v aufx Cldn Maxs` arm64 与 Rosetta x86_64 各一次 `AU VALIDATION SUCCEEDED.`（Max 贴的输出）。
3. **生产部署只有 Max 在终端跑**：max-core 钩子连 `deploy.sh check` 也拦（命令里只要带 `deploy.sh` 就拦）。你负责把 dist 备好、核对好、把命令给他。
4. 完成标准（Max「全按推荐」）见 §6；**不含**宿主里试听、PR #28 的实际效果。
5. 更新日志 5 条的题材与条数 Max 点过头；T-045（三种风格限幅预算放宽）**不写进**更新日志（Max 还没试听）。
6. 站点这次会带上线上一直缺的 T-061 / T-062 / T-066 ～ T-069 的文字与图修复（线上是手工补丁的 0.1.8.7）。
7. 112 上已做整份备份：`~/celadon-site.bak-20261005-before-0.1.8.8`（66 MB）。回滚：`systemctl --user stop celadon-site`，把备份换回 `~/celadon-site`，再 `start`。
8. 「曲风 / 交付」说明页是 T-077（分支 `claude/t077-site-genre-delivery-docs`，`f47b996`，33 个文件，**未开 PR、未部署**，等 Max 通读），不在 #51 / #52 里；它与这次发布的先后见 §4 Q5。
9. 独立复查做过一轮：2 个只读代理（`wf_50c24038-abb`），26 条 = 17 个根因，全部有裁决；#52 就是照它改的。**没有再派第二轮**（见 Q6）。

## 3 未定事项（留给接棒方）

- #52 合并的时机：Max 对 §4 Q1 ～ Q3 点头之后；用普通 merge commit（#28、#51 都是这么合的）。
- 合并后 main 若多了别的提交（例如 T-077 先合），dist 一律**以最终 main 重建**再核对，再给 Max 部署命令。
- 公网核对遇到 Cloudflare 缓存没刷新时怎么办（先看 `cf-cache-status`，别擅自操作 Cloudflare）。
- 收尾档案的具体措辞。

## 4 待用户拍板（Max 还没答；开工先看他有没有已答，没答再问，Grill 格式，推荐写在前）

> 提问时写明「回『可以』= 采纳全部推荐」（Max 答「可以」语义不固定，见 memory `ke-yi-answer-is-ambiguous.md`）。

❓ **Q1** - **T-045 的说法**：0.1.8.8 里同时带着 T-045（三种风格的限幅预算放宽，GLAZE 自己的自动方案更响，「通用 + 标准」在新实例和重新聆听后实测 +0.3 ～ +1.2 LU，不再与 0.1.8.7 逐位相同）。更新日志原来写「通用 / 标准沿用原来的方式」，读者会以为声音没变。选：(a) 收窄成不会被 T-045 推翻的说法（学法与音色目标照旧、仍按所选风格定响度），不提 T-045——**#52 已按这个改**；(b) 加一条 T-045 并合并其他条目（超过每版 5 条，且他没试听）；(c) 不改。
➡️ 我的推荐：**(a)**。可信度：高。最大坏处：用户升级后觉得通用档变响了，日志没提，以为是 bug（版本档「注意」已写，他试听后下一版再补）。最强反方：日志本该如实报告声音变化，(b) 才诚实——回应：他没试听前不对外承诺具体变化，(a) 的措辞不含「没变」的断言。依据：ADR-029「后果」段、`Source/dsp/GlazeConstants.cpp:78-80、101-103、124-126`、复查质量轴 Q3 / 规格轴 S1。

❓ **Q2** - **首页 5 行并入这次发布吗**：`site/src/i18n/locales/{zh,en}/home.json` 的 41 / 116 / 150 行（「听 15～30 秒」补了「选了 House 曲风会听久一点 / 要听到主段落」）、130 行（目标响度补上「交付」）、217 行（「GLAZE 为 0 时逐位相同」补前提「曲风通用、交付标准」，因为流媒体 −2 dBTP 封顶不看 GLAZE）。
➡️ 我的推荐：**并入**（#52 已含）。可信度：高。最大坏处：首页对外措辞多改一轮，要他再过目。最强反方：首页是卖点位，加限定词冲淡卖点——回应：旧句与代码不符（House 要听到主段落，没有拍点时最长约 124 s）才是更大的风险。依据：复查规格轴 S4、`GlazeConstants.h:107-110`、`CeladonChain.cpp:207-211`。

❓ **Q3** - **改写后的公开文字再点一次头**（完成标准 ④）：更新日志 5 条中英（`site/src/content/changelog.ts` 第 37–79 行）与首页 5 行中英，逐字看 PR #52 的 diff。
➡️ 我的推荐：他看过再合并；不满意的地方他给意见、接棒方改后再给他看。可信度：高。最大坏处：多一轮往返。最强反方：复查已逐条核过依据，不必再看——回应：公开文字上一轮就有 3 处说大，规矩是他点头才部署。依据：全局 CLAUDE.md、本任务的完成标准 ④。

❓ **Q4** - **PR #28 与已打好的包不一致**：Max 在复查期间合并了 #28（AU 注册版本改写成 `JucePlugin_VersionCode`，0.1.8.8 = `0x10808`；`scripts/au-version.sh` + `scripts/package-mac.sh`）。但已打好的两个 .pkg 是合并前编的，AU 版本整数仍是 264（只编码前三段 0.1.8），和 0.1.8.7 相同——Logic 可能沿用旧验证结果（`killall -9 AudioComponentRegistrar` 后重开即可）。选：(a) 照现有包发，#28 从下一个包起生效；(b) 回 Mac 重编重打（重存工程 → xcodebuild → `package-mac.sh` → auval → 校验和 → 重填 `release.ts` → 重建 → 重核对，Mac 约 25～40 分钟，之前的核对全作废）。
➡️ 我的推荐：**(a)**。可信度：中。最大坏处：发出去的 0.1.8.8 与 main 不是同一个构建配方（版本档要如实写），升级的 Logic 用户可能要手动清缓存。最强反方：重编一次让「0.1.8.8 = main」更干净，#28 本来就为这类升级问题——回应：重编让 auval、校验和、`release.ts`、构建核对全部重做；TASKS 里记的 #28 合并前提是「真 Xcode + auval + 宿主加载验过」，我不知道他合并前验过没有。依据：TASKS T-076 行、`v0.1.8.8.md`「注意」。
（若选 (b)：随本包附了两个小脚本，见 §11；Mac 部分只能 Max 本人做。）

❓ **Q5** - **T-077 说明页与这次发布谁先上线**：说明页写「0.1.8.8 起」，所以不能先于 0.1.8.8 上线。选：(a) 先部署这次发布（不含说明页），说明页通读合并后再单独部署一次；(b) 等 T-077 通读合并后一起部署；(c) 把 T-077 并入这次发布。
➡️ 我的推荐：**(a)**。可信度：高。最大坏处：两次手动部署，且在两次之间插件里有曲风 / 交付、官网没有说明页（插件悬停气泡有说明）。最强反方：一次部署更省事——回应：0.1.8.8 的包和日志已齐、已等了半天，说明页还没通读，不该拖住它。依据：版本档「官网」段、T-077 裁决档第二轮第 1 条。

❓ **Q6** - **#52 要不要再派一轮只读复查**：#52 的改动是照复查建议改的文字与注释，已在 107 构建并核对产物，但没有第二个人看过；这类公开文字上一轮就查出 3 处说大。需要 Max 同意才能开 Workflow（memory `workflow-needs-user-approval.md`）。
➡️ 我的推荐：**要，1 个只读代理**（逐条对照复查原文与 #52 diff，只看有没有改对、有没有改出新问题，约 30 万 token）。可信度：中。最大坏处：多花约半小时。最强反方：改动全是照建议改的，自己 grep 过产物，够了——回应：「改了代码或配置，说代码改完之前先派独立复查」是 max-core 的硬要求。依据：max-core 输出风格第 4 节。

## 5 必读文件（路径 + 版本）

GitHub（`Maxsong-0/Celadon`）：

| 路径 | 版本 | 为什么读 |
|---|---|---|
| `site/src/content/changelog.ts`、`site/src/i18n/locales/{zh,en}/home.json`、`site/src/content/algorithms/{zh,en}/verification.md`、`docs/BUILDING.md`、`installer/Celadon.iss`、`scripts/windows-wine.sh` | 分支 `claude/t076-review-fixes` @ `09c5410`（PR #52） | 这次要点头的措辞与说明 |
| `site/src/content/release.ts` | main `2aca139` 起（真值，三个包） | 下载页的数据源，部署前核对对象 |
| `site/deploy/deploy.sh`（144 行） | main | 部署做什么：本机检查 → 上传包 → 同步 dist → 重启服务 → 删旧包 → 112 自检 |
| `site/scripts/build-linux.sh` | main | 在 107 上构建站点并把 `dist/` 拉回当前 worktree |
| `.github/workflows/ci.yml` | main | CI：PR 走 `pull_request`，push 只 main，同名运行自动取消 |
| `docs/validation/2026-10-05-T073-peak-off-dirty-region/README.md`、`docs/validation/2026-10-05-T074-guard-recovery-indicator/README.md` | main | 测试数 148,728 与界面检查 114 项的出处 |

vault（Obsidian，**不在 GitHub 上**，只在 Spark 与 Max 的 Mac；根目录 `/home/maxsong/Documents/Max's Secret Place/项目/Celadon/`）：

| 路径 | 为什么读 |
|---|---|
| `04-协调/TASKS.md` 里 T-076 行 | 认领、进展、踩到的坑（行很长，用 `grep -n "T-076"`） |
| `04-协调/2026-10-05-T-076-独立复查裁决.md`（交接时新写） | 26 条发现 → 17 个根因的逐条裁决与同类扫描 |
| `05-版本/v0.1.8.8.md` | 版本档（状态「进行中」，收尾时改） |
| `04-协调/归档/2026-10-05-T-077-独立复查裁决.md` | T-077 对首页 / 更新日志旧句的点名（D21–D24、D53–D56） |
| `03-记忆/ADR-002-Mac编译由用户手动跑.md`、`ADR-029-三种风格限幅预算一起放宽.md` | 为什么 Mac 由 Max 手动编；T-045 的后果 |

memory（`/home/maxsong/.claude/projects/-home-maxsong-All-Project-Celadon/memory/`）：`ke-yi-answer-is-ambiguous.md`、`workflow-needs-user-approval.md`、`sandbox-ssh-and-vault-writes.md`、`worktree-session-harness-limits.md`、`spark-local-build-and-test.md`、`celadon-git-github.md`。

## 6 验收标准（二元可判；全部为「是」才算完成）

1. 发布目录里 `sha256sum -c SHA256SUMS` 三项 OK，且最终 `site/dist` 里内嵌的文件名、字节数、SHA-256 与发布目录逐项一致（§11 第 5 步的脚本输出全 ✔）。
2. 部署用的 dist 是**从合并后的最终 main 构建**的；dist 里新措辞在、旧措辞不在（同一脚本）。
3. Max 对 Q3 的公开文字点头；#52 已合并（或 Max 明确定了别的合并方式）。
4. 112 部署后自检全过：Max 贴出的 `deploy.sh` 输出里第 4/4 步全是 ✔、没有 ✘。
5. 公网核对（§11 第 8 步）：`/downloads/SHA256SUMS` 与发布目录逐字相同；三个新包返回 200 且字节数一致；0.1.8.7 的三个旧包返回 404；更新日志所在的块里有 0.1.8.8 标题。
6. Max 说「完成」。
7. 收尾档案（§10）全部更新。

不在验收内：宿主里试听、PR #28 的实际效果。

## 7 禁区

1. 命令里带 `deploy.sh` 的一律不跑（含 `check`）；不碰 112 上的服务与文件（只读公网 curl 可以）。
2. 不改 `Source/`、`tests/`、`Celadon.jucer`；不在 Mac 上跑任何东西（Mac 是 Max 的手，ADR-002）；本机不跑测试，站点构建走 107。
3. 不碰 T-077、T-078、T-079 名下的文件与分支：`site/src/content/docs/**`、`site/src/content/algorithms/**` 里除 `verification.md` 以外的页、`site/src/i18n/glossary.md`、`site/src/i18n/locales/*/docs.json`、`figures.json`、`claude/t077-site-genre-delivery-docs` 分支。
4. 公开文字（更新日志、首页、下载页）Max 没点头不合并 #52；点头之后再改字要重新点头。
5. 不强推、不删远端分支、不改 `.github/` 与 runner。
6. 不重启 107 上的任何共享服务，不 kill 别人的进程。
7. 开 Workflow：卡片带 `ultracode`，可以开，模型和 effort 不用问（Opus 5.5 + max）；但按 memory `workflow-needs-user-approval.md`，范围与代理数先报 Max、点头再开，做得完的活主进程自己做。
8. 真实密钥不写进任何文件、日志、提交；`site/dist`、`artifacts/` 不入 git。
9. 命令写法：`rm -rf "$VAR/.."` 会被拦，用字面绝对路径；Bash 里带 git 的复合命令容易被拒，拆平（memory `worktree-session-harness-limits.md`）。

## 8 约束（引用，不复制正文）

全局 `~/.claude/CLAUDE.md`：核心行为准则 1（中文）、2（最严谨修复，同类扫全）、7（范围外问题上报）；工作纪律 2（证据先于断言）、3（共享服务）、4（临时产物）、6（CI 与服务器）、7（凭据）；「并发 SOP」（开工清单、认领带租约、收尾清单）。max-core：第 1 节三件事拍板权、难撤回清单（生产部署、发布）、第 4 节状态词、第 5 节回复顺序与单词表。

## 9 当前状态与验证矩阵

整体：**代码改完·部分已实际验证**。距「你确认完成」还差：Q1 ～ Q6 的答复、#52 的 CI 与合并、dist 重建、Max 部署、公网核对。

| 项 | 结果 | 谁 / 怎么 |
|---|---|---|
| 三包 `sha256sum -c SHA256SUMS` | 全 OK | Spark，发布目录 |
| 两个 .pkg 拆包（版本 0.1.8.8、各自单一架构、安装位置同 0.1.8.7、AU 版本整数 264） | 通过 | 交出方，附带 `inspect_pkg.py` |
| Windows 包（109 真 MSVC，Wine 冒烟：安装 / 只选 VST3 重装 / 卸载） | 通过 | 交出方 |
| `auval` arm64 + x86_64 | 各 `AU VALIDATION SUCCEEDED.` | Max 在 Mac 上跑并贴输出 |
| PR #51 的 CI（x86-64 与 x86-64-v3 两遍） | SUCCESS，已合并 `2aca139` | GitHub |
| 107 站点构建（#52 版）：`check-i18n`、`check-svg-text`、`tsc --noEmit`、`vite build` | 全过，301 个文件 | 交出方，`build-linux.sh` |
| dist 与发布目录对账（#52 版） | 三包文件名 / 字节数 / SHA-256 都内嵌；新文字 12 处在、旧文字 8 处不在；0.1.8.7 包名已无 | 交出方（没用 `deploy.sh`，用 grep） |
| `bash -n scripts/windows-wine.sh` | 通过 | 交出方 |
| **PR #52 的 CI** | **13:27 时 2 项 pending，MERGEABLE / UNSTABLE** | 用 `get_status` 看，别轮询 |
| 没跑 | `codesign --verify`（只能在 Mac）、宿主里试听、macOS 的 BSD `stat` 与 openrsync 实跑、第二轮复查、112 部署、公网核对 | — |

## 10 占用资源与收尾清单

- **电脑：DGX Spark（max-spark）**。接棒方必须在 Spark 上：发布目录与 112 / 107 / 109 的 SSH 钥匙都在这里。
- 分支（都已推送）：`claude/t076-review-fixes`（`09c5410`，PR #52 草稿）、`claude/t076-handoff`（本文）、`claude/t076-release-0.1.8.8`（#51 的分支，已合并，保留）、`claude/t077-site-genre-delivery-docs`（别碰）。
- worktree：交出方的 `/home/maxsong/All-Project/Celadon/.claude/worktrees/complete-remaining-tasks-cdaccd`（在 #52 的分支上，**留着到 #52 合并**，之后已合并且干净可删；里面的 `site/dist` 是忽略文件）。接棒方用自己的 worktree，别动这个。
- 服务器：107（站点构建，共享）、109（本棒用不上）、112（生产，部署只由 Max）；Mac 在 `100.86.217.51` 这个 Tailscale 地址能连 Spark（用 `max-spark` 主机名时 rsync 曾卡住）。
- 编号：任务号已用到 T-079；本棒不需要新号，要挂卡片先去 TASKS「占用与编号登记」占 T-080。
- 收尾清单（vault 都用 Edit 增量改，TASKS / INDEX 不许整份覆盖）：
  1. `05-版本/v0.1.8.8.md`：状态行改最终状态；写 #51（`2aca139`）、#28（`8bcf8da`）、#52 的合并提交号；链接复查台账；按 Q4 的选择写 AU 版本整数一条；写部署时间与核对结果。
  2. `04-协调/TASKS.md`：T-076 行移到 ④ 已完成（关键文件 + 一句验证结论）；占用与编号登记不用动。
  3. `00-总览/INDEX.md`（第 7 行当前版本、最近日志）与 `05-版本/roadmap.md`：刷新 0.1.8.8。
  4. `02-日志/2026-10-05.md`：追加 T-076 一节（只追加；先看一眼别的会话有没有同时在写）。
  5. 踩坑三篇放 `03-记忆/`：Mac 主检出的 `Builds/` 是旧工程、缺新文件、版本号对上也不代表新；rsync ≥ 3.2.4 对远端路径的转义与 `?` 通配；Tailscale SSH 的连接记录在 `journalctl -u tailscaled`、不在 sshd。
  6. `04-协调/2026-10-05-T-076-独立复查裁决.md` 改名归档到 `04-协调/归档/`（同一文件改名，不另写一份）。
  7. 清理：自己产生的临时文件；交出方的 worktree（#52 合并后）；告诉 Max Mac 上 `/tmp/celadon-win-out` 可删；远端分支是否删由 Max 定。
  8. **通知 T-079 会话**：它（会话名「订正官网与档案里『受限』等同类旧表述」，id `local_fd22ba90-18b0-4c30-9e27-e749bc1e0bc1`，用 `SendMessage` 找）的 D 组要改 `home.json` 与更新日志 0.1.8.8 的旧句，**等 T-076 在 TASKS 移到 ④ 才动**——这是它认的开工信号。#52 已经改了首页 5 行与更新日志 5 条（见 §4 Q1 ～ Q2 与复查裁决台账），收尾时告诉它最终版本、让它把 D 组范围缩到还没改的部分，免得两线做同一件事。T-079 的交接包也放在 `docs/handoff/`（分支 `claude/t079-site-stale-claims`），约定一致。

## 11 接续命令

```bash
# 1 读现状（任意 worktree；不要用裸 git pull）
git fetch origin claude/t076-handoff claude/t076-review-fixes main
gh pr view 52 --repo Maxsong-0/Celadon --json state,isDraft,mergeable,mergeStateStatus,statusCheckRollup

# 2 Max 对 Q1～Q3 点头（Q6 若要复查先做完）后：
gh pr ready 52 --repo Maxsong-0/Celadon
gh pr merge 52 --repo Maxsong-0/Celadon --merge        # 普通 merge commit；Max 若说他自己合就等他

# 3 在你自己的 worktree 里切到最终 main，在 107 上重建 dist（拉回当前 worktree 的 site/dist）
git fetch origin main && git switch --detach origin/main
site/scripts/build-linux.sh
```

```bash
# 4 对账：dist 与发布目录（不用 deploy.sh）
R=/home/maxsong/All-Project/Celadon/artifacts/release-0.1.8.8-2026-10-05
(cd "$R" && sha256sum -c SHA256SUMS)
while read -r sum name; do name="${name#\*}"; size=$(stat -c%s "$R/$name")
  echo "$name | 文件名 $(grep -rlF -- "$name" site/dist | wc -l) | 哈希 $(grep -rlF -- "$sum" site/dist | wc -l) | 字节数 $(grep -rlE -- "(^|[^0-9])$size([^0-9]|$)" site/dist/assets | wc -l)"   # 每格应 ≥ 1
done < "$R/SHA256SUMS"
grep -rlF 'Celadon-0.1.8.7-' site/dist | wc -l    # 应为 0
grep -rlF '曲风与交付，修坯更谨慎' site/dist | wc -l   # 应 ≥ 1
```

```bash
# 5 把部署命令交给 Max（他在 Spark 的终端里跑；dist 必须是第 3 步在这个 worktree 里构建的）
cd "$(git rev-parse --show-toplevel)" && site/deploy/deploy.sh
# 成功标志：「✔ 本机检查通过」，第 4/4 步全是 ✔、没有 ✘。请他把输出整段贴回来。
```

```bash
# 6 公网核对（只读 curl）
B=https://celadonaudio.com; R=/home/maxsong/All-Project/Celadon/artifacts/release-0.1.8.8-2026-10-05
curl -s $B/downloads/SHA256SUMS | diff - "$R/SHA256SUMS" && echo "SHA256SUMS 与发布目录一致"
for f in $(awk '{print $2}' "$R/SHA256SUMS"); do
  echo "$f 公网 $(curl -sI $B/downloads/$f | tr -d '\r' | sed -n 's/^content-length: //Ip') 字节 / 本地 $(stat -c%s "$R/$f") 字节"; done
for old in Celadon-0.1.8.7-macOS-AppleSilicon.pkg Celadon-0.1.8.7-macOS-Intel.pkg Celadon-0.1.8.7-Windows-x64-Setup.exe; do
  echo "$old $(curl -s -o /dev/null -w '%{http_code}' $B/downloads/$old)"; done   # 期望 404
CH=$(basename "$(grep -rlF '曲风与交付，修坯更谨慎' site/dist/assets | head -1)")   # 更新日志所在的块（块名带内容哈希）
curl -s $B/assets/$CH | grep -c '曲风与交付，修坯更谨慎'   # 期望 ≥ 1
```

（线上现在仍是 0.1.8.7：`SHA256SUMS` 三行是 0.1.8.7 的包，13:2x 读过。）

若选 Q4 (b) 重编：`inspect_pkg.py <包> [--names]`（没有 xar 也能拆 .pkg，核对版本 / 架构 / 安装位置）与 `patch_release_ts.py <发布目录> --dry-run`（照发布目录重写 `release.ts` 的 `RELEASE` 块，先核对再写）就放在本目录。

## 12 接续提示词（可直接粘进新会话）

你接手 Celadon 的 T-076「发布 0.1.8.8」后半段。先在仓库里 `git fetch origin claude/t076-handoff`，读 `git show origin/claude/t076-handoff:docs/handoff/2026-10-05-T-076-release-0.1.8.8/README.md` 全文，按它的「已定事项 / 禁区 / 验收标准」做。第一件事：读 vault 的 TASKS T-076 行与 `04-协调/2026-10-05-T-076-独立复查裁决.md`，登记你的认领（刷新最后心跳，租约 48 小时），再向 Max 问 §4 里还没答的问题（Grill 格式，推荐在前，写明「回『可以』= 采纳全部推荐」）。生产部署只有 Max 能跑，你把命令给他；公开文字他点头前不合并。

## 13 回执包（接棒方交回来时填）

| 字段 | 写什么 |
|---|---|
| 状态 | completed / failed / stopped / abandoned |
| 产物 | 路径（部署输出、公网核对输出、更新后的版本档） |
| 冲突 | 和别的认领、别的实现撞了什么 |
| 没做完的 | 每条带原因 |
| 待用户拍板 | 碰到三件事没自己定的，逐条列出 |
