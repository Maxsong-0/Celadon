# 构建：macOS（Apple Silicon + Intel）与 Windows（x64）

v0.1.8.0 起插件同时支持 Apple Silicon Mac、Intel Mac 与 64 位 Windows。插件本体用 Projucer 生成的 Xcode / Visual Studio
工程编；单元测试与几个工具（`tools/render`、`tools/ui-snapshot`）用根目录的 CMake。

## 目录与 Projucer

- JUCE 9.0.2 放在仓库旁边的 `../JUCE`：`Celadon.jucer` 的模块路径与 CMake 的 `CELADON_JUCE_DIR` 默认值都是它。
- `Builds/` 由 Projucer 生成，不进版本库，**`git pull` 不会更新它**：拉了新提交（加 / 删源文件、改版本号）或改了 `Celadon.jucer` 之后都要重新生成，
  否则 Xcode / VS 工程还是旧的——新源文件不进编译，链接报「找不到符号」（2026-10-05 踩过：旧工程不认 `SegmentLedger.cpp`）。
  Projucer 9.0.2 里「Save Project」（先关掉开着的旧 Projucer 窗口、不保存，再重新打开 `Celadon.jucer`：它内存里是旧工程，一保存就会把刚拉下来的 `.jucer` 冲掉），或命令行（在仓库根目录跑；**Projucer 不在 PATH 里，要写全路径**）：
  `../JUCE/Projucer.app/Contents/MacOS/Projucer --resave Celadon.jucer`（JUCE 下载包自带 `Projucer.app`；自己编的见下一条）。
  重新生成后 `JuceLibraryCode/` 应与仓库里的逐字节相同（`git status` 干净）；核对：
  `grep -o 'JucePlugin_Version=[0-9.]*' Builds/MacOSX/Celadon.xcodeproj/project.pbxproj | sort -u` 应与 `Celadon.jucer` 的 `version` 一致（对不上就是旧工程）。
  **版本号对上不代表工程是新的**：只加源文件、不改版本号的提交（例如 `SegmentLedger.cpp` 进 `.jucer` 那次）查不出来；再看一眼
  `[ Celadon.jucer -nt Builds/MacOSX/Celadon.xcodeproj/project.pbxproj ] && echo 旧工程`（`.jucer` 比工程新就是旧工程；`scripts/windows-wine.sh` 对 VS 工程用的是同一种判断）。
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
- AU 版本：JUCE 写 AU 注册版本（`Info.plist` 的 `AudioComponents → version`）时只取版本号前三段，0.1.8.8 成了 0.1.8（0x108），
  比 v0.6.1 的 0x601 还低；同一个 `aufx Cldn Maxs` 装了几份时系统只认版本最高的，宿主会一直加载旧 AU。所以 `Celadon.jucer` 的
  构建后脚本（Post-Build Shell Script）跑 `scripts/au-version.sh`，在签名和拷贝之前把它改成 `JucePlugin_VersionCode` 的编码：
  0.1.8.8 → 0x10808，宿主与 `auval` 里显示 1.8.8，以后四段号每升一次都跟着升。`auval -v` 输出开头的 Component Version
  应是 `1.8.8 (0x10808)`。脚本也改 Projucer 生成的 `Builds/MacOSX/Info-AU.plist`（原因写在脚本开头），Projucer 重存会写回，下次构建再改。

安装包（给别的 Mac 用）：编好 Release 通用版后跑 `scripts/package-mac.sh`，在 `installer/Output/` 出两个 .pkg——
`Celadon-<版本>-macOS-AppleSilicon.pkg`（arm64）与 `Celadon-<版本>-macOS-Intel.pkg`（x86_64）。脚本用 `ditto --arch` 从通用二进制
拆出单架构副本、重新 ad-hoc 签名（AU 签名前再过一遍 `scripts/au-version.sh`，构建后已改过的不动），每个包三个可选组件：
VST3 → `/Library/Audio/Plug-Ins/VST3`、AU → `/Library/Audio/Plug-Ins/Components`、
独立版 → `/Applications`（系统级，要管理员密码）。安装器「自定」里这三项默认都勾上，可以取消任意一项（没有 VST2）。
组件关掉了「可搬移」，免得安装器把新版装到机器上已有的同 ID 副本那里。
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

`installer/Celadon.iss`：组件页可以多选，默认两项都装。VST3 进
`C:\Program Files\Common Files\VST3\Celadon.vst3`，独立运行版进 `C:\Program Files\Celadon\`，开始菜单一个快捷方式。
两项都能取消；一项都不选时向导会拦住。没有 AU，也没有 VST2。「设置 → 应用」里能卸载；向导按 Windows 界面语言出中文或英文
（简体中文文件见 `installer/Languages/README.md`）。静默安装不带 `/TYPE` 或 `/COMPONENTS` 时两项都装
（`/TYPE=vst3` 只装插件，`/TYPE=standalone` 只装独立版）。在 Windows 上先编出 Release x64，再：

```bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=<Celadon.jucer 里的 version> installer\Celadon.iss
```

产物 `installer\Output\Celadon-<版本>-Windows-x64-Setup.exe`（不进版本库）。没有代码签名，第一次运行时 SmartScreen 会提示
「未知发布者」：点「更多信息 → 仍要运行」。

### 没有 Windows 机器：在 Linux 服务器上用真 MSVC 编（msvc-wine）

Mac 上不编、不测（ADR-002）。测试服务器上用 [msvc-wine](https://github.com/mstorsjo/msvc-wine) 把 Visual Studio 2022 的生成工具
（MSVC v143、Windows SDK、MSBuild）装进 Wine：cl.exe、link.exe、MSBuild 与 Windows 上是同一套，编的就是 Projucer 生成的 `.sln`。

- 装一次：`ssh maxsong@192.168.100.109 'bash -s' < scripts/setup-windows-wine.sh`（Wine、msitools、Xvfb、中文字体
  fonts-noto-cjk 要 sudo apt；下载 VS 工具即接受微软的 Visual Studio / Windows SDK 许可，装出来的编译器不可再分发）。
- 日常：先在 Mac 上重新生成工程（命令见上面「目录与 Projucer」；`Builds/VisualStudio2022` 随源码同步过去），然后
  - `scripts/windows-wine.sh plugin`：msbuild 编 Release x64（VST3 + Standalone），本工程源码有任何警告或错误都算失败；
  - `scripts/windows-wine.sh test [--only 类名]`：CMake + Ninja + cl.exe 编 `celadon_tests`（静态运行库），在 Wine 里跑；
  - `scripts/windows-wine.sh installer`：plugin + 安装包，并在单独的 Wine 前缀里静默安装（默认两项都装）、逐字节核对、
    只选独立版重装（VST3 要被清掉）、只选 VST3 重装（独立版要被清掉）、静默卸载；
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

## DGX Spark（max-spark）：Mac 会话的编译、测试、渲染与基准都在这里跑

Max 2026-10-04 定：Celadon 的编译、测试、渲染一律去 DGX（NVIDIA DGX Spark，aarch64 Cortex-X925 × 10 + A725 × 10，20 核 / 121 GB，
Ubuntu 24.04，GCC 13.3，Release + Ninja，没有 LTO），Mac 上不编不测；107 / 109 只在家里网络或 VPN 下可达，CI 仍在 107。

- **连法**：`ssh max-spark`（`~/.ssh/config` 的别名，HostKeyAlias 同名）。别名写的 `max-spark.local` 跨网段解析不到，Mac 在别的网段时
  用 `ssh -o Hostname=192.168.109.100 max-spark`（保留别名才复用 known_hosts；直接 `maxsong@192.168.109.100` 会报 Host key verification failed）。
  Tailscale 里它叫 `max-spark`（100.x 地址），但 Tailscale SSH 要先在网页上授权一次（授权后有一段有效期）。Mac 这边 ssh / rsync 在 Claude 的沙箱里会被拦，要绕过沙箱。
- **一个入口**：`scripts/spark.sh`（`sync` 同步工作树、`archive <提交>` 放干净树、`build|test|run|dir` 转给远端、`ssh` 直跑；都有 `--dry-run`，
  选项放在子命令前面）。它只碰「本机 + 本工作树」自己建的远端目录：目录里的 `.celadon-spark-owner` 记着主人，主人不是自己或没有标记就拒绝，
  别的会话的源码目录不会被 `rsync --delete` 抹掉；`build` / `test` / `run` 同样只对自己的目录，同名操作在本机排队。`ssh` / `run` 的参数逐个
  转义，远端不展开 `~` 和通配符、不认管道，要用就包一层 `bash -c '…'`，路径从远端家目录起；Ctrl-C 只停本机的 ssh，远端已开始的测试会跑完并占着锁。
- **远端脚本**：`~/.cache/celadon-spark/bin/spark-test.sh <源码目录> build|test|run|dir`（参考副本 `scripts/dgx/spark-test.sh`，DGX 可达后入库，以 DGX 上那份为准）。
  源码放 `~/.cache/celadon-spark/src/<名>/`，构建目录 `~/.cache/celadon-spark/build/<名>-<源码路径与标志的散列>/`（`dir` 子命令打印）。
  `build` 默认目标 `celadon_tests celadon_render`；`test` 编 `celadon_tests` 后运行（可带 `--only 类名,…`）；`run <命令…>` 在锁里跑任意命令，
  命令里可用 `$BUILD`（单引号留给远端展开）。`SPARK_JOBS` 并行编译数（默认 8，几个会话同时编给 4）；`CELADON_CXX_FLAGS` 同 `test-linux.sh`。
- **锁**：同一构建目录的编译互斥；`test` / `run` 走**全机串行锁** `~/.cache/celadon-spark/locks/run.lock`（别的会话的测试也排这把锁，计时护栏
  不被干扰）；**编译不进全机锁**——跑 CPU 基准前要在 vault TASKS ② 登记独占、给在跑的会话发消息、另起进程观察编译活动，日志为空才算干净。
- **环境**：Python 用 venv 的全路径 `~/.cache/celadon-spark/venv/bin/python`。截至 2026-10-11 装了 numpy 2.5.3 / scipy 1.18.1 / soundfile 0.14.0 /
  matplotlib 3.11.2，**pyloudnorm 还没装**（`venv/bin/pip install pyloudnorm==0.2.0`，装好改这句）；完整清单以 (c) 线入库的 `tools/render/requirements.txt`
  为准。`/usr/bin/ffmpeg` 6.1.1（aac / libopus / libmp3lame / ebur128，无 libfdk_aac）；截图工具要的 freetype / fontconfig / x11 开发头 2026-10-04 起由
  系统包提供（`~/.cache/celadon-spark/guidev/` 是 10-03 的旧绕法）；`taskset`、`curl` 有。
- **语料**（都在 `~/.cache/`，不进 git、不出 DGX）：`celadon-t048-22songs/`（Max 的门槛曲：`wav/` 目前 20 首，缺的 2 首见 TASKS ②；`batch.sh` + `manifest.tsv`）、
  `celadon-t048-cal/<deep|tech|melodic>/<track>/<track>.wav`（Jamendo 45 首，44.1 kHz 24 bit，已裁到小节线；标签见
  `docs/validation/2026-10-01-T038-calibration/`）、`celadon-t048-cal-syn/`（合成 House）。谁占着哪个目录看 vault TASKS ②。
- **口径**：已知固定失败 5 条——ColorModule 4 条是 NEON 快路径写死了融合乘加、与标量逐位对不上（Linux ARM 不是目标平台）；BalancePerformance
  1 条是立体声 / 单声道耗时比超出门槛。过 / 败一律对照同机基线只算新增；`audio_hash` 只在同机同构建内可比（见上一节）；x86-64-v3 与 MSVC 仍分别在 CI（107）与 109 上跑。

## CI（GitHub Actions，Studiolo 的自托管 runner）

`.github/workflows/ci.yml`：每个 PR、每次合进 main，都在 Studiolo 的自托管 CI 服务器上原生编 `celadon_tests`，跑两遍全套——基线
（与不带参数的 `scripts/test-linux.sh` 相同）与 `-march=x86-64-v3`（有 FMA，即上面「测试与性能」里改了契约代码要补跑的那一套；CI 每次都跑）。
JUCE 9.0.2 由工作流按提交钉死、取到 `Celadon/` 旁边；构建目录留在 runner 的工作区里（`~ghrunner/runners/celadon/_work/Celadon/Celadon/build/`，
每遍约 30 MB），下一次只重编改过的文件。

- runner：`githubci`（192.168.100.107）上只属于本仓库的实例 `celadon`（`actions-runner@celadon.service`，用户 `ghrunner`，
  标签 `self-hosted,linux,x64,celadon`），与 Studiolo 的实例互不影响；个人账号下 runner 只能按仓库注册。机器本身的搭法见 Studiolo
  仓库的 `deploy/ci-runner/README.md`。
- 只走自托管：私有仓库的 GitHub 托管分钟数整个账号共用（Studiolo 2026-09 两天就烧光过）。runner 离线时作业排队等它（GitHub 最多等
  24 小时），不会改去托管机器；确要临时用托管机器，把 `runs-on` 改成 `ubuntu-24.04`。
- 仓库必须保持私有：公开仓库的外部 PR 能在这台机器上执行任意代码。
- 一个实例一次只跑一个作业：两遍全套排着跑，所有会话的 PR 共用这一条队列。推送前照旧用 `scripts/test-linux.sh` 验（它不占 runner）。
- 作业一直排队或 runner 离线：`gh api repos/Maxsong-0/Celadon/actions/runners --jq '.runners[] | "\(.name) \(.status) busy=\(.busy)"'`
  看状态并告诉 Max；确认没有作业在跑才重启：`ssh maxsong@192.168.100.107 'sudo /usr/local/sbin/ci-runner-service restart celadon'`。
- 编译工具用机器上的系统包：`cmake`（≥ 3.22）、`ninja`、`g++`。作业的 PATH 由实例目录里的 `.path` 给出，不含 `/snap/bin`。

装实例与注册（在 Mac 上执行；注册令牌只由 Max 本人经 stdin 送入，不回显、不落盘，1 小时有效；agent 不经手、不打印）。
`V` / `SHA` 是 [actions/runner](https://github.com/actions/runner/releases) 最新发布的版本与发布说明里 linux-x64 包的 sha256，
`install-runner.sh` 下载后按它核对，不符不解包。块里不写注释：macOS 的 zsh 默认不认交互式的 `#` 注释。

```bash
H=192.168.100.107
V=$(gh release view -R actions/runner --json tagName -q .tagName | sed 's/^v//')
SHA=$(gh release view -R actions/runner --json body -q .body | grep -F "linux-x64-$V.tar.gz" | grep -oE '[0-9a-f]{64}' | head -1)
ssh ghrunner@$H "~/install-runner.sh celadon $V $SHA"
gh api -X POST repos/Maxsong-0/Celadon/actions/runners/registration-token --jq .token \
  | ssh ghrunner@$H '~/runners/celadon/register.sh Maxsong-0/Celadon'
ssh maxsong@$H 'sudo /usr/local/sbin/ci-runner-service enable-now celadon'
```

再加一台机器进同一个池（比如 109 `github-2`）：把 `H` 换掉，整段再跑一遍。实例名仍叫 `celadon`、标签相同即同池，runner 名是
`<主机名>-celadon`，不会撞名；池里的机器都要支持 x86-64-v3（AVX2 + FMA），否则那一遍全套会因非法指令崩掉。
