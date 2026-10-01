Celadon 是自用插件，私人分发、不商用。拿到对应平台的包后照下面装，装完在宿主里**重新扫描插件**。

## macOS

- 系统：macOS 12 Monterey 及以上。
- 按芯片选安装包：左上角苹果菜单 → 关于本机，「芯片」写 Apple M… 选 `Celadon-<版本>-macOS-AppleSilicon.pkg`；写「处理器 Intel」选 `Celadon-<版本>-macOS-Intel.pkg`。
- 双击 .pkg 安装，「自定」里可以只勾需要的格式：

| 格式 | 装到 |
|---|---|
| AU `Celadon.component` | `/Library/Audio/Plug-Ins/Components/` |
| VST3 `Celadon.vst3` | `/Library/Audio/Plug-Ins/VST3/` |
| 独立运行 `Celadon.app` | `/Applications/`（不开宿主时试界面用） |

- AU 身份：`aufx Cldn Maxs`（Max Song: Celadon）。两种芯片的 AU 都过 `auval`。
- 在 Apple Silicon 上用 Rosetta 打开的宿主是 x86_64 进程，只能加载 Intel 包里的插件；这种情况装 Intel 包（系统会提示装 Rosetta）。
- 第一次打开 .pkg 若提示「来自身份不明的开发者」（没有 Apple 开发者签名）：去「系统设置 → 隐私与安全性」，找到 Celadon 那一条点「仍要打开」。
- 自己从 Xcode 编过、装在个人目录 `~/Library/Audio/Plug-Ins/` 的，不要再装安装包，否则宿主里会出现两份。

## Windows

- 系统：Windows 10 / 11，64 位。
- 安装包 `Celadon-<版本>-Windows-x64-Setup.exe`：VST3 装到 `C:\Program Files\Common Files\VST3\Celadon.vst3`（固定），独立运行版（可选，默认装）到 `C:\Program Files\Celadon\`，开始菜单一个快捷方式；「设置 → 应用」里能卸载。向导按 Windows 界面语言出中文或英文。
- 静态运行库：不需要另装 VC++ 运行库。

> ⚠ 安装包没有代码签名。第一次运行 SmartScreen 会提示「未知发布者」：点「更多信息 → 仍要运行」。

## 宿主

- 实测：FL Studio 2026、Ableton Live 12 Suite。其他支持 VST3 / AU 的宿主原则上都能用。
- 不做 AAX（Pro Tools）。
- 更新版本后宿主可能仍缓存旧二进制：保存工程后重启宿主或重新扫描插件目录。
- 侧链：目标频谱的「学习参考」只读插件的可选立体声侧链输入「Reference」；把参考曲接到 DAW 的外部侧链上（各宿主叫法不同）。

## 延迟

宿主自动补偿。总延迟 = BALANCE 0 + CLAY（线性相位收单 D + 峰值守护前瞻 G）+ COLOR 64 + LIMIT（Clip 过采样 60 + Lookahead + 62）。

![48 kHz、Lookahead 2 ms 时的延迟构成](figure:latency)

| 采样率 | 总延迟（Lookahead 2 ms） |
|---|---|
| 44.1 kHz | 1287 样本 ≈ 29.2 ms |
| 48 kHz | 1384 样本 ≈ 28.8 ms |
| 96 kHz | 2584 样本 ≈ 26.9 ms |
| 192 kHz | 4984 样本 ≈ 26.0 ms |

改 Lookahead 时，参数停稳 0.3 s 后才换前瞻并重新上报延迟；拖动与自动化中延迟不变。每个模块的旁通都保留自己的延迟量，切换不错位。

## 旧工程

- 0.1.8.5 新增的推响不刺、目标频谱、编码余量、抖动，读入旧工程时一律关，声音与从前一样；新实例才默认「自动」。
- 旧工程存档里没有 GLAZE 时恢复为 0（严格手动）；显式保存的 0 保持 0。
- 窗口比例：v0.1.7 及以前的 `uiScale` 不再读，旧工程按「设置」里的默认大小打开。

## 设置文件（跟人走，不跟工程走）

语言、减少动效、新窗口默认大小存在 `Celadon.settings`：macOS `~/Library/Application Support/Celadon/`，Windows `%APPDATA%\Celadon\`。同一进程里所有实例共用，一个窗口切语言，别的窗口同时换。

## 性能预期

单个音频线程的预算占用（不是整机 CPU）：Apple M2 Pro 上 GLAZE 100、界面关，48 kHz 约 1.3 %、96 kHz 约 2.6 %、192 kHz 约 5 %。192 kHz 配 32 ～ 64 样本缓冲在老 Intel 笔记本上有爆音风险；不要给 x86_64 提高指令集基线（Rosetta 没有 AVX）。数字出处见[性能与验证](/algorithms/verification)。
