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
- Release 会顺手把 VST3 / AU 拷进 `~/Library/Audio/Plug-Ins/`（Projucer 的 plugin copy step），产物本身在
  `Builds/MacOSX/build/Release/`。
- 检查：`lipo -archs Builds/MacOSX/build/Release/Celadon.vst3/Contents/MacOS/Celadon` 应打印 `x86_64 arm64`
  （`Celadon.component`、`Celadon.app` 同样）；AU 两个架构都过一遍 `auval -v aufx Cldn Maxs` 与
  `arch -x86_64 auval -v aufx Cldn Maxs`（后者走 Rosetta）。

测试与性能：

- `scripts/test-local.sh`：本机架构的全套单元测试；`CELADON_TEST_ARCH=x86_64 scripts/test-local.sh` 在 Apple Silicon 上编
  x86_64 版、经 Rosetta 跑全套（Intel 那一半走 SSE2 路径、x86_64 的 vDSP 与 libm，发布前要跑一遍）。
- `scripts/check-syntax.sh` 在 Mac 上每个文件按 arm64 与 x86_64 各查一遍。
- `CELADON_PERF_ARCH=x86_64 scripts/perf-local.sh`：经 Rosetta 粗看 x86_64 版的开销，**不能**当 Intel 真机的验收读数。
  x86_64 没有 NEON 那几条快路径，同一段处理大约是 arm64 的 2.3 ～ 3 倍 CPU；192 kHz、32 ～ 64 样本缓冲在老的 Intel 笔记本上
  有爆音风险——对外承诺之前先在 Intel 真机上跑一遍性能网格。
- 不要提高 x86_64 的指令集基线（`-mavx`、`-mavx2`、`-mfma`、x86_64h）：macOS 12 ～ 14 的 Rosetta 没有 AVX，Rosetta 里跑的宿主
  会直接崩（SIGILL）；FMA 还会破坏逐位契约（ADR-013）。

## Windows：x64，Visual Studio 2022

需要 Visual Studio 2022「使用 C++ 的桌面开发」（MSVC v143 与 Windows 10 / 11 SDK）。

```bat
msbuild Builds\VisualStudio2022\Celadon.sln /p:Configuration=Release /p:Platform=x64 /m
```

- 编整个 `.sln`，不要只编 `Celadon_VST3.vcxproj`：生成 `moduleinfo.json` 的「VST3 Manifest Helper」（Win32）是解决方案级依赖。
- 产物：`Builds\VisualStudio2022\x64\Release\VST3\Celadon.vst3\`（整个文件夹就是插件）与
  `Builds\VisualStudio2022\x64\Release\Standalone Plugin\Celadon.exe`。
- 安装：把 `Celadon.vst3` 文件夹拷进 `C:\Program Files\Common Files\VST3\`（要管理员权限，所以工程里关掉了 Projucer 的
  plugin copy step），或者用下面的安装包。

工程里已经设好、不要改的几项：

- `/utf-8`：源码是不带 BOM 的 UTF-8，中文 Windows 的默认代码页是 GBK（936），不加这条中文字符串会乱码甚至编不过。
- 静态运行库（`/MT`）：用户机器上不需要另装 VC++ 运行库。
- `/fp:precise`：不许 `/fp:fast`（逐位契约的守卫会 `#error`，ADR-013）。VS2022 的 v143 在 `/fp:precise` 下不合并乘加；
  更早的工具集配 `/arch:AVX2` 可能合并，所以几个 DSP 文件里还写死了 `#pragma fp_contract (off)`。
- 工具集要换成 ClangCL 的话，同时加 `/clang:-ffp-contract=off`：clang-cl 默认合并乘加，守卫会报错。

测试：`scripts\test-windows.ps1`（CMake 的 Visual Studio 2022 生成器，编 `celadon_tests` 并运行；`-Only 类名,类名`、
`-Config Debug`）。根目录的 CMake 也认 clang-cl（自动加 `/clang:-ffp-contract=off`）。

## 跨平台的数值一致性

同一台机器上的同一种构建里，所有逐位契约都成立（SIMD 与标量、任意块长、空闲跳过、预热……）。跨平台的成品在最后几位上
本来就不同：arm64 的 TruePeak / Balance 用融合乘加、x86 是分开的乘和加；Mac 的 FFT 是 vDSP、Windows / Linux 是 JUCE 自带的；
Windows 的 UCRT 还按 CPU 是否支持 FMA3 换超越函数的实现。所以 `tools/render` 的 `audio_hash` 只在同一台机器的同一种构建
之间可比，跨平台对照用 `gate_check.py` 的 dB / LU 门槛（见 `tools/render/README.md`）。
