# AU 版本号倒退 · 档案待贴文字（云端会话写不到档案，由 Max 或本机会话照贴 / 改写）

档案：`/Users/maxsong/Documents/Max's Secret Place/项目/Celadon`。证据都在仓库 `docs/validation/2026-10-04-au-version/`（PR #28）。
格式按档案里现有的写法调整即可，任务编号用 TASKS 的下一个空号（下面写作 T-0xx），内容以 REPORT.md 为准。

## 04-协调/TASKS.md

**已知项「AU 版本号」移到已完成（Mac 上核对通过后）**

- T-0xx · AU 版本号倒退（新版 AU 装上后宿主仍加载 v0.6.1）· 2026-10-04 · 完成（代码，PR #28），Mac 核对待做。
  - 原因：JUCE 9.0.2 写 AU 注册版本（`Info.plist` 的 `AudioComponents → version`）只取版本号前三段，0.1.8.8 写成 0x108，
    低于 v0.6.1 的 0x601；同一个 `aufx Cldn Maxs` 装了几份时系统只认版本最高的。CFBundleVersion 不是原因（安装器的版本检查早已关掉）。
  - 修法（Max 2026-10-04 选「构建后改写」）：`scripts/au-version.sh` 把 AU 注册版本改成 `JucePlugin_VersionCode` 的编码
    （0.1.8.8 → 0x10808，宿主显示 1.8.8）；`Celadon.jucer` 的 Post-Build Shell Script 在 JUCE 签名与安装之前调用，
    `scripts/package-mac.sh` 在 AU 签名前再过一遍。版本号方案（ADR-010）不变，不用手删旧 AU。
  - 验证：Projucer 9.0.2 重存后 `JuceLibraryCode/` 逐字节不变；脚本 27 个用例在 bash 3.2.57 / 5.2 下全过。
  - 证据：`docs/validation/2026-10-04-au-version/REPORT.md`。

**新开 / 待认领**

- Mac 核对 AU 版本（PR #28 合并前）：Projucer 重存 → 编 Release 通用版 → `auval -v aufx Cldn Maxs` 的 Component Version 是
  `1.8.8 (0x10808)` → Logic 加载的是新版、旧工程正常打开。步骤见 REPORT「Mac 上的核对」。

## 日志（2026-10-04）

AU 版本号倒退（云端会话）：

- 查清原因：JUCE 9.0.2 的 AU 注册版本只取前三段（`juce_PlistOptions.cpp:114`），0.1.8.8 → 0x108，低于 v0.6.1 的 0x601，
  系统选了旧 AU；四段号的第四段进不了 AU 版本，0.1.8.5 ～ 0.1.8.8 在宿主眼里是同一版。
- 三个修法里 Max 选「构建后改写」（另两个是跳到 0.7.0、手删旧 AU）。
- PR #28：`scripts/au-version.sh`，接进 jucer 构建后脚本与 `package-mac.sh`。云端用 Projucer 9.0.2 与 macOS 同源的 bash 3.2 验过，Mac 上待核对。

## 00-总览/INDEX.md

- 验证证据：`docs/validation/2026-10-04-au-version/`（AU 注册版本改写；在 Linux 上编 Projucer 9.0.2 和 macOS 同源 bash 3.2 的做法可复用）。

## 03-记忆/ADR-010（后果补一条）

> JUCE 写 AU 注册版本时只取前三段：四段号的第四段进不了 AU 版本，0.1.x.y 还会低于 v0.6.x。2026-10-04 起构建后把 AU 注册版本
> 改写成 `JucePlugin_VersionCode` 的编码（`scripts/au-version.sh`），因此四段号每一段都不能大于 255。
