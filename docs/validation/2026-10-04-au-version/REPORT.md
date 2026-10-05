# AU 版本号倒退：构建后改写 AU 注册版本（2026-10-04）

Max 2026-10-04 在三个修法里选了「构建后改写」：版本号不动，不用手删旧 AU，构建与打包时把 AU 注册版本改成 `JucePlugin_VersionCode` 的编码。

## 原因

- AU 注册版本是 `Info.plist` 里 `AudioComponents → version`，宿主和系统按它比新旧。JUCE 9.0.2 写这个字段时只取版本号前三段
  （`extras/Build/juce_build_tools/utils/juce_PlistOptions.cpp:114-119` 的 `getAUVersionAsHexInteger`，第 340 行写入），
  `JucePlugin_VersionCode`（0x10808）不用在这里。
- 实测：用 JUCE tag 9.0.2 源码编出的 Projucer 重存 main `9c18ff9`，生成的 `Builds/MacOSX/Info-AU.plist` 里
  0.1.8.8 的 `version` 是 264（0x108，即 0.1.8）；v0.6.1 是 0x601（1537）。同一个 `aufx Cldn Maxs` 装了几份时系统只认版本最高的，
  所以新版装上了，宿主照样加载 v0.6.1。四段号的第四段进不了 AU 版本：0.1.8.5 ～ 0.1.8.8 都是 0x108；本机装过的 0.1.9.0 开发版是 0x109。
- 不是 CFBundleVersion：字符串比较确实是 0.1.8.8 < 0.6.1，但只有 macOS 安装器看它，`scripts/package-mac.sh` 已关掉 `BundleIsVersionChecked`。
  CFBundleShortVersionString 只用来显示。

## 改动

- `scripts/au-version.sh`：按 CFBundleShortVersionString 算 `JucePlugin_VersionCode` 的编码（a.b.c.d → a×2^24 + b×2^16 + c×2^8 + d），
  写进每个 `AudioComponents → version`；已经对的文件不写。0.1.8.8 → 67592（0x10808），宿主与 `auval` 显示 1.8.8。
  三段号算出的值与 JUCE 写的相同；某段大于 255、不是三段或四段、没有 AudioComponents 都直接报错。
- `Celadon.jucer`：Xcode 导出的 Post-Build Shell Script 调这个脚本（不带参数时只在 `WRAPPER_EXTENSION = component` 的目标里动手），
  改构建出的 `Celadon.component/Contents/Info.plist` 和 Projucer 生成的 `Builds/MacOSX/Info-AU.plist`。为什么两个都改写在脚本开头。
- `scripts/package-mac.sh`：AU 副本签名前再过一遍（Xcode 构建后已改过的不动），旧构建打包也对。
- `docs/BUILDING.md`：macOS 一节补 AU 版本一条与核对方法。

## 本机验证（Linux 容器，不是 Mac）

1. Projucer 9.0.2 从 JUCE tag 9.0.2 源码编出（`7278278`），`--resave` main 与本分支：
   - 两边 `JuceLibraryCode/` 都与仓库逐字节相同（`git status` 干净），`Celadon.jucer` 重存后就是本分支这一处改动，没被改格式。
   - Visual Studio 2022 工程两边逐字节相同。
   - Xcode 工程见 `xcode-build-phases.txt`：AU 目标是 Resources → Sources → Frameworks → Run Post-Build Script → Sign Target → Install Target，
     改写在 JUCE 的签名与拷进 `~/Library/Audio/Plug-Ins/Components` 之前。Standalone、VST3、Shared Code 也会跑这一步，脚本直接退出。
2. 脚本用例 27 个（`script-tests/`）：用 macOS `/bin/bash` 同源的 bash 3.2.57（apple-oss-distributions/bash 的 `bash-3.2`，在 Linux 上编）
   和 bash 5.2.21 各跑一遍，全过。用例包括真 Projucer 输出的 XML 与二进制 plist、重跑不动文件（修改时间与 inode 不变）、
   0.6.1 / 0.1.8 保持 JUCE 的值、前导零按十进制、以后的版本（0.1.8.9 / 0.1.9.0 / 0.2.0.0 / 1.0.0.0）递增、各种坏版本号报错且不动文件、
   两个 AudioComponents、按生成的 Xcode 阶段原文 `/bin/sh -c` 带 Xcode 环境变量跑（非 AU 目标跳过；AU 目标两个文件都改；产物缺失时报错）。
   PlistBuddy 用 Python plistlib 写的替身（`PlistBuddy-shim.py`，只模拟 Print / Set）。

没验证、要在 Mac 上做的：真 Xcode 构建、真 PlistBuddy、codesign、`auval`、宿主加载、`package-mac.sh` 整个跑一遍。

## Mac 上的核对

1. Projucer「Save Project」（或 `--resave`）后编 Release 通用版：构建日志里 AU 目标应有两行
   `AU 版本 264 → 67592（0x10808，0.1.8.8）`（产物和 `Info-AU.plist`）；再编一次应一行都没有。
2. `defaults read ~/Library/Audio/Plug-Ins/Components/Celadon.component/Contents/Info.plist AudioComponents` 里 `version = 67592`。
3. `auval -v aufx Cldn Maxs` 与 `arch -x86_64 auval -v aufx Cldn Maxs`：Component Version 是 `1.8.8 (0x10808)`，且通过。
4. 宿主里加载的是新版（Logic 的插件管理器里版本 1.8.8，会重新验证一次），旧工程照常打开。宿主仍显示旧版时，
   `killall -9 AudioComponentRegistrar` 后重开宿主。旧 AU 可以留着（版本更低，系统不再选它），想清掉就删旧的 `Celadon.component`。

已知限制：Xcode 不保证「生成 Info.plist」排在构建后脚本之前（swift-build 只保证它在准备阶段之后），JUCE 自己的签名步骤也靠同一个前提。
稳定后每次构建都不写文件；只有 Projucer 重存后的头一两次构建要重新生成 Info.plist，第 2、3 步能发现万一的错序。
