# T-102：设置浮层「减少动效」说明补上旋钮（2026-10-18）

T-100（ADR-037）之后，「减少动效」还让 10 颗旋钮不拖彗尾、不回弹、快甩不多滑（`Source/ui/Dial.cpp` 里 `reduceMotion` 的分支），
设置浮层那行说明没提。T-102 把中英两句补上旋钮：

| | 改前 | 改后 |
|---|---|---|
| 中文 | 关闭粒子、波纹和裂纹生长动画 | 关掉粒子、波纹与裂纹淡入，旋钮也不拖彗尾、不回弹 |
| 英文 | Turns off particles, ripples and crack growth | No particle, ripple, crack or knob animation |

- 中文与官网 `site/src/content/docs/zh/interface.md`（T-101）逐字相同。「裂纹生长动画」改叫「裂纹淡入」：减少动效关的是主旋钮换瓷面时的
  交叉淡入（`GlazeKnob::render` 的 `crossfade`），与官网、`Popovers.h` 文件头一致。
- 英文官网原句（…and the knobs' comet tail and spring）单行放不下。第一稿 `No particles, ripples, crack fade-ins, knob tails or spring`
  在 Mac / Windows 放得下，但 DGX 上实测 263.7 px、⑪b 报失败（见下）；改成现在这句，把旋钮的彗尾、回弹、快甩多滑合称 knob animation。

## 放不放得下

说明是单行画的（`Popovers.cpp`：`drawText (…, useEllipsesIfTooBig = true)`），框宽 242 px = 卡片 324 − 两边 16 − 开关 50
（`SettingsPanel::reduceMotionNoteWidth`），字体 `uiFont (11)`（`SettingsPanel::reduceMotionNoteFont`）。界面缩放是整个画布的变换
（`PluginEditor.cpp` 的 `canvas->setTransform`），排版在逻辑坐标里做，截不截断与界面大小无关：70 % 与 100 % 是同一个结论。

界面字体随平台（`CeladonLookAndFeel.cpp` 的 `findInterfaceTypefaceName`）：macOS 苹方、Windows 微软雅黑 UI；Linux 本该是 Noto Sans CJK SC，
但 JUCE 在 Linux 上扫字体只认 `.ttf` / `.otf`（`juce_Fonts_freetype.cpp` 的 `hasFileExtension ("ttf;otf")`），DGX 的 Noto Sans CJK 只有 `.ttc`
合集，JUCE 的字体列表里没有它（诊断打印：150 个字体名里没有任何 Noto Sans CJK），于是落到 `system-ui` = DejaVu Sans，汉字由系统回退到
Noto Sans CJK JP。这是改动前就有的事（Linux 截图里的英文一直是 DejaVu），T-102 不改，单独上报。

| 字体 | 改后中文 | 改后英文 | 第一稿英文 | 官网英文原句 |
|---|---|---|---|---|
| 苹方 PingFang SC（macOS） | 188.6 | 158.5 | 207.0 | 305.5 |
| 微软雅黑 UI（Windows） | 207.9 | 179.8 | 229.6 | 340.5 |
| DGX：英文 DejaVu Sans、中文 Noto Sans CJK JP | 182.3 | 205.4 | 263.7 | 391.3 |

单位 px，上限 242。最紧的是 Windows 中文（余 34 px）与 DGX 英文（余 37 px）。

`measure_font.py` 按 JUCE 9 的 portable 度量换算（em 像素 = 字体高度 ÷ (ascent + descent)，取 HarfBuzz 用的 hhea 或 OS/2 typo 值），
宽度 = 各字 hmtx 前进宽之和，不含字距调整。DGX 上它算的与截图工具里真 JUCE 排版量的逐位对得上（中文 182.3、第一稿英文 263.7、
改后英文 205.4），说明换算可信；官网英文原句算的 391.3、实测 391.6（差的 0.3 px 应是脚本不算的字距调整），不影响结论。
微软雅黑 UI 用的是 Office for Mac 自带的 `msyh.ttc`，Windows 系统里的版本可能略有出入。

## 验证（DGX Spark，`spark-test.sh` 全机锁）

源码树：`t102-main` = `git archive 300e637`（开工基线），`t102-final` = `git archive 8eeeb38`（本分支最后一个代码提交，之后只加了本目录）。
日志在 DGX `~/.cache/celadon-spark/logs/t102/`。`S=~/.cache/celadon-spark/bin/spark-test.sh`。

| 项 | 命令 | 结果 |
|---|---|---|
| 编译 | `$S <树> build celadon_tests celadon_ui_snapshot` | 都过；改后的警告与 main 相同（5 条原有的未用函数，只差行号） |
| 本地化定向 | `$S t102-final test --only Localisation` | 1148 过 / 0 败 |
| 全套 | `$S <树> test` | main 与改后都是 134719 过 / 5 败，失败条目相同：ColorModule 立体声对单声道逐位 4 条（aarch64 的 NEON 融合乘加）、BalancePerformance 立体声 1.72 倍单声道 1 条（aarch64 没有打包双声道快路径） |
| 交互检查 | `$S <树> run xvfb-run -a $BUILD/celadon_ui_snapshot --only check` | main 141 条全过；改后 143 条全过，多的 2 条是 ⑪b：zh 182.3 / 242、en 205.4 / 242 |
| ⑪b 两侧都测 | 两个只在 DGX 上的变体，只编截图工具跑 `--only check`：mutA 把英文换成官网原句；mutB 把框宽的 `− 50` 改成 `− 100`（192 px） | mutA：zh 过，en 391.6 / 242 失败（「drawText cuts it short」）；mutB：zh 182.3 / 192 过，en 205.4 / 192 失败 |
| 截图 | `… --deterministic --only zh-settings,en-settings,zh-settings-zoom70,en-settings-zoom70`；另 `--scale 2 --only zh-settings,en-settings` | 都出图。本目录的 `*-card.png` 是裁出的设置卡片（`before-zh-settings-card.png` 是 main 的）：中英、100 % 与 70 % 都整行显示，没有省略号 |
| 逐像素比 | main 与改后的 `zh-settings.png`（都是 `--deterministic`） | 726 个像素不同，全在说明行框里（x 879 ～ 1053、y 425 ～ 432）；卡片里说明行以外 0、卡片外 0 |

第一稿英文（`…knob tails or spring`）那一轮的交互检查：⑪b en 263.7 / 242 失败、其余全过——就是它让英文换成现在这句。

## 文件

| 文件 | 内容 |
|---|---|
| `measure_font.py` | 按字体文件估一行字的宽度（JUCE portable 度量，不含字距调整） |
| `measure-output.txt` | Mac 上量苹方、微软雅黑 UI 的输出 |
| `measure-output-dgx.txt` | DGX 上量 DejaVu Sans、Noto Sans CJK JP 的输出 |
| `{zh,en}-settings-card.png`、`{zh,en}-settings-zoom70-card.png`、`{zh,en}-settings-x2-card.png` | 改后的设置卡片：100 %、70 %、2 倍像素比（从截图工具的整张图裁出，四周留 8 px） |
| `before-zh-settings-card.png` | main（`300e637`）的同一张，对照用 |
