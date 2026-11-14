# T-114 发版交接 · 0.1.8.11 合 main 之后（2026-11-14）

> 交出方：会话「完成剩余任务并发布 0.1.8.11」（Opus 5.5 · max，DGX，工作树 `blissful-lovelace-4f5fd6`，集成分支 `claude/complete-tasks-release-0-1-8-11-9a03ec`）。
> 这个会话开着「PR 合并后自动归档」，上下文也很长；PR [#93](https://github.com/Maxsong-0/Celadon/pull/93) 合进 main 之后，余下的发版步骤和 M8 收尾照本档接着做（新会话，或这个会话被重新打开时）。
> 计划正本：[[2026-11-13-T-114-发布0.1.8.11-计划]]（第 6 节 M7 / M8、第 8 节进度、第 10 节决定、第 11 节回滚）。仓库里同一份在 `docs/handoff/2026-11-14-T114-release-handoff.md`。

## 目标

把 0.1.8.11 发出去：Mac 两个包和 Windows 包放进同一个发布目录并写 `SHA256SUMS`；`release.ts`（日期不是 11-14 时连同 `changelog.ts` 的日期）合进 main；Max 部署 112；从公网下载三个包，按 `SHA256SUMS` 校验。然后做 M8 收尾。

## 已定事项（不重开）

- Max 11-14 10:4x「全按推荐」定了 6 件：没设过默认大小的老用户，新开实例改 125 %；更新日志按现稿；整个集成分支一个 PR、用 merge commit；CPU 阈值起点值即终值；ADR-041 里 L3 的措辞与橙色规则例外按现稿；T-112 两句英文短提示按现稿。仓库验证记录第 14 节。
- 听感批三项（1-05 / 1-06 / 1-07）这版都不进，出厂常数全关（[[ADR-042-听感批预算环主段门冷启动与默认档音色]]「已否决（这版）」）。
- 插件源码冻结在 PR #93 合进 main 的那棵树；发版只改 `site/src/content/release.ts`（必要时加 `changelog.ts` 的 `date`）。插件源码再有改动就等于新版本，要重跑全部验证、重打三个包。
- 版本号：`Celadon.jucer` 0.1.8.11，AU `0x1080B` = 67595（提交 `77c5ca3`）。

## 未定事项

- 待用户拍板：没有。
- 接棒方自己定：发布目录的日期（按 Max 编完 Mac 包的那天）。

## 必读文件（版本）

- vault 计划 [[2026-11-13-T-114-发布0.1.8.11-计划]]（11-14 10:5x 版）第 6、8、10、11 节。
- vault [[v0.1.8.10]]「构建与安装」「注意」：上一版的发布目录、拆包核对 6 个组件、`deploy.sh` 在同一版本有多个目录时取名字最新的那个。
- 仓库 `docs/validation/2026-11-13-T114-release-0.1.8.11/README.md` 第 11 节（最终验证）、第 14 节（Max 拍板）。
- 仓库 `site/src/content/release.ts`、`site/src/content/changelog.ts`（0.1.8.11 那条 `date: '2026-11-14'`）。
- 脚本：`scripts/package-mac.sh`、`site/scripts/build-linux.sh`、`site/deploy/deploy.sh`。

## 当前状态（11-14 12:0x）

- 代码：已实际验证（整批独立复查 3 轮做完；108 两项全量 136195 / 0、MSVC 136195 / 0、DGX 135937 / 5 已知、20 首网格 GLAZE 0 逐位不变）。
- PR #93：用 merge commit 合 main（推 main 会弹确认窗，Max 点同意）；合完核对 main 的树与集成分支顶端相同。合并提交号看 `git log origin/main --merges -1`；main 里还没有这个合并提交，就是还没合，先问 Max。
- Windows 包：**已实际验证**。11:50～11:59 在 108 主机用 `e67663a` 的树跑 `windows-wine.sh all`：Wine 里 `celadon_tests` 136195 过 / 0 败，msbuild 本工程 0 警告、JUCE 模块 0 警告，冒烟四步（安装 / 只装独立版 / 只装 VST3 / 卸载）都过。
  - `Celadon-0.1.8.11-Windows-x64-Setup.exe`：7,555,247 字节，SHA-256 `4df84f97767167ba8190c89a4b0d94b5f8c410097d8339ea60b74551ff5dbbb3`。
  - 另有 `Celadon.exe`（`841ce3a3e52db505063fd1162ad0807bcf8442d649b47edf90ad189158e9ef47`）、`Celadon.vst3`（`Contents/x86_64-win/Celadon.vst3`，`965bf4b791294701782d0097b09d88351a4edb660402f2fdd2b6331919e3750f`）。
  - 取回 DGX `~/.cache/celadon-t114/release-win/`，三个文件与 108 原件（`~/.cache/celadon-win/Celadon-b4c458909ae6/Celadon/`）哈希相同；安装包已发给 Max。
  - 之前 11:11、11:14 两次从 DGX 连 108 被 Tailscale SSH 的 check 认证拦下（要 Max 本人在浏览器确认，链接只在连接挂着时有效，第二次约 90 秒就超时），Max 11:4x 认证后通过。以后再遇到，照样请 Max 认证，不许为了躲认证改走别的地址或账号。
- Mac 两个包、`release.ts`、部署、公网校验：没开始。

## Windows 包怎么打（要重打时；DGX 上，108 的 Tailscale 认证过了以后）

做法同 `docs/BUILDING.md`「没有 Windows 机器：在 Linux 服务器上用真 MSVC 编」；`REV` 用合并后的 main（它的树应当与 `e67663a` 相同，先 `git diff --stat e67663a origin/main` 核一下，只该多交接文档）。

```bash
REPO=/home/maxsong/All-Project/Celadon            # 或你的工作树
REV=origin/main
ST=$HOME/.cache/celadon-spark/win/stage
rm -rf "$ST/Celadon" && mkdir -p "$ST/Celadon"
git -C "$REPO" archive "$REV" | tar -x -C "$ST/Celadon"
# Linux 版 Projucer 重新生成 Builds/VisualStudio2022（暂存目录不在 git 里，每次都要生成）
PJ=$HOME/.cache/celadon-spark/win/projucer-build/extras/Projucer/Projucer_artefacts/Release/Projucer
H=$(mktemp -d); (cd "$ST/Celadon" && HOME="$H" xvfb-run -a "$PJ" --resave Celadon.jucer); rm -rf "$H"
# Linux Projucer 写的十六进制是小写，两个头文件换回仓库原样
for f in JuceLibraryCode/JuceHeader.h JuceLibraryCode/JucePluginDefines.h; do git -C "$REPO" show "$REV:$f" > "$ST/Celadon/$f"; done
cd "$ST/Celadon"
CELADON_WIN_HOST=maxsong@100.84.85.68 bash scripts/windows-wine.sh all          # 约 10～15 分钟：MSVC 全量测试、插件、安装包、冒烟四步
CELADON_WIN_HOST=maxsong@100.84.85.68 bash scripts/windows-wine.sh fetch ~/.cache/celadon-t114/release-win/
sha256sum ~/.cache/celadon-t114/release-win/Celadon-0.1.8.11-Windows-x64-Setup.exe   # 再 ssh 到 108 对同一个文件算一次，两边必须相同
```

验收：`celadon_tests: 136195 passed, 0 failed`（与 `d588fd6` 那次相同）、msbuild 本工程 0 警告、冒烟四行（安装 / 只装独立版 / 只装 VST3 / 卸载）都在、最后 `rc=0`。上一次成功的输出格式见验证记录第 11 节 MSVC 一行。取回的安装包发给 Max，放进 Mac 上的发布目录。

## 接续步骤

1. **Max（Mac）**：拉 main → Projucer 9.0.2 重存 `Celadon.jucer` → Xcode 编 Release 通用版 → `scripts/package-mac.sh`。拆包核对照 [[v0.1.8.10]]（架构单一且对、版本 0.1.8.11、AU 67595、`codesign -v --strict`、`relocatable="false"`、`hostArchitectures`）。在 Mac 上看一眼 T-112 的字号（中英、70 %、125 %）。两个 pkg 和 Windows 安装包放进 `artifacts/release-0.1.8.11-<日期>/`，在那个目录里 `shasum -a 256 Celadon-0.1.8.11-* > SHA256SUMS`。
2. **接棒会话**：拿到三个包的文件名、字节数、SHA-256 → 改 `site/src/content/release.ts`（`version`、`date`、三个文件）；发版日期不是 11-14 就同改 `changelog.ts` 0.1.8.11 那条的 `date` → 官网构建先交 108（check-i18n、check-svg-text、check-dial-motion、tsc、vite）→ 开 PR、合 main（推 main 会弹确认窗）。
3. **Max（Mac）**：`site/scripts/build-linux.sh` → `site/deploy/deploy.sh check` → `site/deploy/deploy.sh`（生产部署，只由 Max 跑）。
4. **接棒会话**：从公网 `celadonaudio.com` 下载三个包，`shasum -a 256 -c SHA256SUMS` 全 OK（计划完成标准 12）。
5. **M8 收尾**：
   - TASKS：T-114 那行移到「已完成」，写关键文件与一句验证结论；释放「占用与编号登记」第 58 行的资源。
   - [[ADR-041-界面命名橙色规则与字号]] 状态改「已采纳」（它写着「等 Max 看图认可、T-114 合入 main 后改」，两件都到了）。
   - 写版本档 `05-版本/v0.1.8.11.md`，照 [[v0.1.8.10]] 的结构（新增 / 修复 / 变更 / 构建与安装 / 注意 / 回滚）。材料：`changelog.ts` 0.1.8.11 那 5 条；上一版之后已合 main 的 T-097（#82 `a768b8a`，后被 T-108 删掉的守门）、T-106（#90 `0601dcd`，G_trim 换误差口径）、T-107（#91 `fa381f2`，面板重构包 A）、T-108（#92 `93af3e0`）；这次 PR #93 的任务表（PR 说明）；听感批三项不进与原因（ADR-042、T-113 记录第 8 节）；0.1.8.10 → 0.1.8.11 B 组变化只报不拦（T-114 记录第 8 节）。
   - 刷新 INDEX；写当日日志；计划同一文件改名归档进 `04-协调/归档/`。
   - 经验分流进 `03-记忆/`：统计数从入库数据算（复查 R2-F8）；写「某版本里有什么」先查那个版本的发版提交（两扇守门在 0.1.8.9 / 0.1.8.10 对所有曲风生效）；跨架构的证据只当推断（x86 指令数推不出 aarch64 布局）；压缩或暂停后写时刻取文件修改时间与提交时间（已进记忆 `log-times-from-clock`）。
   - 清理（见下「占用资源」）。

## 验收标准

- 三个包和完整的 `SHA256SUMS` 在同一个 `release-0.1.8.11-*` 目录；部署后从公网下载的三个包 `shasum -a 256 -c SHA256SUMS` 全 OK。
- `release.ts` 已合 main；TASKS 的 T-114 在「已完成」；版本档 v0.1.8.11 写好；ADR-041「已采纳」；计划已归档。

## 禁区

- 不改插件源码。
- 生产部署只由 Max 跑；不强推、不删远程分支（集成分支合并后保留）。
- 本机 Mac 不跑测试与编译检查；官网构建先交 108、其次 DGX。

## 占用资源 / 要清理的

- 集成分支 `claude/complete-tasks-release-0-1-8-11-9a03ec`：合并后保留远程分支；工作树 `blissful-lovelace-4f5fd6` 合并后干净就可以删。
- DGX：`~/.cache/celadon-spark/src/t114-*`、`~/.cache/celadon-t113/`、`~/.cache/celadon-t114/`（Windows 包取回的 `release-win/` 留到发版确认后）、`~/.cache/celadon-spark/win/stage/`。
- 108 主机：`~/.cache/celadon-win*/Celadon-b4c458909ae6/` 下 `installer/Output/` 里旧的 0.1.8.10 安装包。
- TASKS「占用与编号登记」第 58 行（DGX 全机锁、testbox 108、108 主机 MSVC）。

## 接续提示词

```
读 vault `项目/Celadon-Docs/04-协调/2026-11-14-T-114-发版交接.md`，接着做 Celadon 0.1.8.11 的发版：先确认 PR #93 已合进 main（`git log origin/main --merges -1`），再按「接续步骤」从第一个没做完的一步做起；要 Max 做的（Mac 两个包、部署）先问他做到哪了。插件源码不改。
```
