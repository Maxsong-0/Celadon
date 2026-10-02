# 构建：macOS（Apple Silicon + Intel）与 Windows（x64）

v0.1.8.0 起插件同时支持 Apple Silicon Mac、Intel Mac 与 64 位 Windows。插件本体用 Projucer 生成的 Xcode / Visual Studio
工程编；单元测试与几个工具（`tools/render`、`tools/ui-snapshot`）用根目录的 CMake。

## 目录与 Projucer

- JUCE 9.0.2 放在仓库旁边的 `../JUCE`：`Celadon.jucer` 的模块路径与 CMake 的 `CELADON_JUCE_DIR` 默认值都是它。
- `Builds/` 由 Projucer 生成，不进版本库。改了 `Celadon.jucer` 之后先重新生成：Projucer 9.0.2 里「Save Project」，
  或命令行 `Projucer --resave Celadon.jucer`。重新生成后 `JuceLibraryCode/` 应与仓库里的逐字节相同（`git status` 干净）。
- Projucer 从 JUCE 源码编：
  `cmake -S ../JUCE -B ../JUCE/build -DJUCE_BUILD_EXTRAS=ON && cmake --build ../JUCE/build --target Projucer --config Release`，
  产物在 `../JUCE/build/extras/Projucer/Projucer_artefacts/Release/`。

## macOS：一个包同时跑在 Apple Silicon 与 Intel 上

最低系统 macOS 12（Monterey）：Intel Mac 大致是 2015 年及以后、能装 macOS 12 的机型。

发布版必须编成通用二进制（arm64 + x86_64）。工程本身已经是（`xcodeValidArchs="arm64,x86_64"`，Release 不限当前架构），
但**编的方式**决定结果：

```bash
xcodebuild -project Builds/MacOSX/Celadon.xcodeproj -scheme "Celadon - All" -configuration Release \
           -destination 'generic/platform=macOS' ONLY_ACTIVE_ARCH=NO build
```

- 不要加 `ARCHS=arm64` / `ONLY_ACTIVE_ARCH=YES`；用 scheme 编时不写 `-destination`，xcodebuild 会选「My Mac (arm64)」，
  同样只出 arm64（`docs/validation/` 里记录的发布构建都是这样编的，只有 arm64，Intel Mac 与 Rosetta 下的宿主都加载不了）。
- Xcode 界面里编：运行目标选「Any Mac (Apple Silicon, Intel)」。
- Release 会顺手把 VST3 / AU 拷进 `~/Library/Audio/Plug-Ins/`（Projucer 的 plugin copy step，脚本阶段里写的是
  `${HOME}/Library/Audio/Plug-Ins/…`），产物本身在 `Builds/MacOSX/build/Release/`。只想验证、不想覆盖系统里已装的插件时，
  命令行再加一个构建设置 `HOME=<临时目录>`：只改脚本阶段看到的 HOME，拷贝落进临时目录，Xcode 其他行为不变。
  **写在命令后面**（`xcodebuild … build HOME=/tmp/x`）；写在命令前面当环境变量（`HOME=/tmp/x xcodebuild …`）不改构建设置，照样装进系统
  （2026-10-02 踩过）。动手前用 `xcodebuild … -showBuildSettings HOME=/tmp/x | grep INSTALL_PATH` 核对。
- 检查：`lipo -archs Builds/MacOSX/build/Release/Celadon.vst3/Contents/MacOS/Celadon` 应打印 `x86_64 arm64`
  （`Celadon.component`、`Celadon.app` 同样）；AU 两个架构都过一遍 `auval -v aufx Cldn Maxs` 与
  `arch -x86_64 auval -v aufx Cldn Maxs`（后者走 Rosetta）。

安装包（给别的 Mac 用）：编好 Release 通用版后跑 `scripts/package-mac.sh`，在 `installer/Output/` 出两个 .pkg——
`Celadon-<版本>-macOS-AppleSilicon.pkg`（arm64）与 `Celadon-<版本>-macOS-Intel.pkg`（x86_64）。脚本用 `ditto --arch` 从通用二进制
拆出单架构副本、重新 ad-hoc 签名，每个包三个可选组件：VST3 → `/Library/Audio/Plug-Ins/VST3`、AU → `/Library/Audio/Plug-Ins/Components`、
独立版 → `/Applications`（系统级，要管理员密码）；组件关掉了「可搬移」，免得安装器把新版装到机器上已有的同 ID 副本那里。
没有 Apple Developer ID：包不签名、不公证，别人第一次打开会被 Gatekeeper 拦，到「系统设置 → 隐私与安全性」点「仍要打开」。
本机用 Xcode 装过的（`~/Library/Audio/Plug-Ins/`）不要再装 pkg，否则宿主里会有两份。

测试与性能：

- `scripts/test-local.sh`：本机架构的全套单元测试；`CELADON_TEST_ARCH=x86_64 scripts/test-local.sh` 在 Apple Silicon 上编
  x86_64 版、经 Rosetta 跑全套（Intel 那一半走 SSE2 路径、x86_64 的 vDSP 与 libm，发布前要跑一遍）。
- `scripts/check-syntax.sh` 在 Mac 上每个文件按 arm64 与 x86_64 各查一遍。
- `CELADON_PERF_ARCH=x86_64 scripts/perf-local.sh`：经 Rosetta 粗看 x86_64 版的开销，**不能**当 Intel 真机的验收读数。
  v0.1.8.0 时 x86_64 缺 Balance / Color 的两声道打包快路径，同一段处理大约是 arm64 的 2.3 ～ 3 倍 CPU；v0.1.9.0（T-025）起
  所有 NEON 快路径都有了 SSE2 版（Balance、Color 的双二阶两声道打包成 `__m128d`，与 x64 标量逐位相同），这个倍数要重新量。
  192 kHz、32 ～ 64 样本缓冲在老的 Intel 笔记本上仍有爆音风险——对外承诺之前先在 Intel 真机上跑一遍性能网格。
- 不要提高 x86_64 的指令集基线（`-mavx`、`-mavx2`、`-mfma`、x86_64h）：macOS 12 ～ 14 的 Rosetta 没有 AVX，Rosetta 里跑的宿主
  会直接崩（SIGILL）；FMA 还会破坏逐位契约（ADR-013）。
- 但测试要额外考验一次有 FMA 的目标（ADR-013 后果第 3 条）：改了 `HalfBand` / `Saturator` / `TruePeak` / `Balance` / `Color` / `Clay` /
  `LinearPhaseLowpass` / `Limiter` 或任何编译选项之后，除基线全套外再跑
  `CELADON_CXX_FLAGS=-march=x86-64-v3 scripts/test-linux.sh`（独立构建目录，不污染基线；只是测试构建，插件仍按上一条不提基线）。

## Windows：x64，Visual Studio 2022

需要 Visual Studio 2022「使用 C++ 的桌面开发」（MSVC v143 与 Windows 10 / 11 SDK）。

```bat
msbuild Builds\VisualStudio2022\Celadon.sln /p:Configuration=Release /p:Platform=x64 /m
```

- 编整个 `.sln`，不要只编 `Celadon_VST3.vcxproj`：生成 `moduleinfo.json` 的「VST3 Manifest Helper」（Win32）是解决方案级依赖。
- 产物：`Builds\VisualStudio2022\x64\Release\VST3\Celadon.vst3\`（整个文件夹就是插件）与
  `Builds\VisualStudio2022\x64\Release\Standalone Plugin\Celadon.exe`。
- 安装：把 `Celadon.vst3` 文件夹拷进 `C:\Program Files\Common Files\VST3\`（要管理员权限，所以工程里关掉了 Projucer 的
  plugin copy step），或者用安装包（见下面「安装包」）。

工程里已经设好、不要改的几项：

- `/utf-8`：源码是不带 BOM 的 UTF-8，中文 Windows 的默认代码页是 GBK（936），不加这条中文字符串会乱码甚至编不过。
- 静态运行库（`/MT`）：用户机器上不需要另装 VC++ 运行库。
- `/fp:precise`：不许 `/fp:fast`（逐位契约的守卫会 `#error`，ADR-013）。VS2022 的 v143 在 `/fp:precise` 下不合并乘加；
  更早的工具集配 `/arch:AVX2` 可能合并，所以几个 DSP 文件里还写死了 `#pragma fp_contract (off)`。
- `/Zc:lambda`：C++17 下 MSVC 默认用旧的 lambda 处理器，lambda 不捕获就读外层的局部 `constexpr` 会报 C3493
  （标准允许，GCC / clang 照编；C++20 起 MSVC 默认就是新处理器）。根 CMake 对 cl.exe 同样加了。
- 工具集要换成 ClangCL 的话，同时加 `/clang:-ffp-contract=off`：clang-cl 默认合并乘加，守卫会报错。

测试：`scripts\test-windows.ps1`（CMake 的 Visual Studio 2022 生成器，编 `celadon_tests` 并运行；`-Only 类名,类名`、
`-Config Debug`）。根目录的 CMake 也认 clang-cl（自动加 `/clang:-ffp-contract=off`）。

### 安装包（Inno Setup）

`installer/Celadon.iss`：VST3 装进 `C:\Program Files\Common Files\VST3\Celadon.vst3`（固定），独立运行版（可选，默认装）进
`C:\Program Files\Celadon\`，开始菜单一个快捷方式；「设置 → 应用」里能卸载；向导按 Windows 界面语言出中文或英文
（简体中文文件见 `installer/Languages/README.md`）。在 Windows 上先编出 Release x64，再：

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.9.0 installer\Celadon.iss
```

产物 `installer\Output\Celadon-<版本>-Windows-x64-Setup.exe`（不进版本库）。没有代码签名，第一次运行时 SmartScreen 会提示
「未知发布者」：点「更多信息 → 仍要运行」。

### 没有 Windows 机器：在 Linux 服务器上用真 MSVC 编（msvc-wine）

Mac 上不编、不测（ADR-002）。测试服务器上用 [msvc-wine](https://github.com/mstorsjo/msvc-wine) 把 Visual Studio 2022 的生成工具
（MSVC v143、Windows SDK、MSBuild）装进 Wine：cl.exe、link.exe、MSBuild 与 Windows 上是同一套，编的就是 Projucer 生成的 `.sln`。

- 装一次：`ssh maxsong@192.168.100.109 'bash -s' < scripts/setup-windows-wine.sh`（Wine、msitools、Xvfb、中文字体
  fonts-noto-cjk 要 sudo apt；下载 VS 工具即接受微软的 Visual Studio / Windows SDK 许可，装出来的编译器不可再分发）。
- 日常：先在 Mac 上 `Projucer --resave Celadon.jucer`（`Builds/VisualStudio2022` 随源码同步过去），然后
  - `scripts/windows-wine.sh plugin`：msbuild 编 Release x64（VST3 + Standalone），本工程源码有任何警告或错误都算失败；
  - `scripts/windows-wine.sh test [--only 类名]`：CMake + Ninja + cl.exe 编 `celadon_tests`（静态运行库），在 Wine 里跑；
  - `scripts/windows-wine.sh installer`：plugin + 安装包，并在单独的 Wine 前缀里静默安装、逐字节核对、只选 VST3 重装
    （独立版要被清掉）、静默卸载；
  - `scripts/windows-wine.sh ui`：cl.exe 编界面截图工具，在单独的 Wine 前缀里借 Xvfb 跑交互检查（`--only check`）。服务器上
    必须有中文字体：JUCE 的字体回退给 `IDWriteFontFallback::MapCharacters` 传 `nullptr`，Wine 的 DWrite 在「没有任何字体含这个字」
    时解引用它、界面直接崩（Windows 上这样传是正常用法，系统也自带中文字体）；
  - `scripts/windows-wine.sh fetch <本机目录>`：把 `Celadon.vst3`、`Celadon.exe`、安装包取回来。
- 几个坑（脚本里都处理了）：msvc-wine 的 msbuild 包装只给 VS 18 设了工具集目录，VS2022 要补 `VCToolsInstallDir_170`；
  MSBuild 的多节点（`/m`）在 Wine 里会中途无声退出，用 `/m:1`（cl.exe 的 `/MP` 照样并行）；「VST3 Manifest Helper」是 Win32 工程，
  要装 x86 目标的工具与库；MSBuild 是 .NET 程序，要 Wine Mono；Inno Setup 的安装程序即使静默也要建窗口，Wine 的图形驱动设成 null。
- Wine 不是 Windows：这里验证的是「编得过、测试在 MSVC + UCRT 下通过、界面交互在 Wine 的 Direct2D / DirectWrite 下跑得通、
  安装包装得上卸得掉」。宿主里加载、界面手感、DPI、字体的真实效果仍要在 Windows 真机上看一遍。

## 跨平台的数值一致性

同一台机器上的同一种构建里，所有逐位契约都成立（SIMD 与标量、任意块长、空闲跳过、预热……；Balance / Color 的两声道打包与
逐声道标量由 `setPackedFilteringForTesting` / `setPackedStereoForTesting` 在左右不同的立体声上对照）。跨平台的成品在最后几位上
本来就不同：arm64 的 TruePeak / Balance 用融合乘加、x86 是分开的乘和加；Mac 的 FFT 是 vDSP、Windows / Linux 是 JUCE 自带的；
Windows 的 UCRT 还按 CPU 是否支持 FMA3 换超越函数的实现。所以 `tools/render` 的 `audio_hash` 只在同一台机器的同一种构建
之间可比，跨平台对照用 `gate_check.py` 的 dB / LU 门槛（见 `tools/render/README.md`）。
