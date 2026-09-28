# celadon_ui_snapshot · 界面截图与交互检查（Linux）

> v0.1.8.0 界面改版（V4 设计稿「融合版」）引入。Mac 上照旧用 Xcode 编真插件看；这个工具让没有 Mac 的环境（CI、云端会话）
> 也能看到整个编辑器画出来是什么样、控件的手感对不对。

## 它做什么

把整个插件（`PluginProcessor` + `PluginEditor` + `Source/ui/*` + `Source/dsp/*`）连同 JUCE 的界面模块编成一个命令行程序：

1. 建处理器、`prepareToPlay (48 kHz, 512)`，建编辑器；
2. 喂 30 秒合成的音乐（120 BPM 的鼓、贝斯、和弦，未做母带的电平），每约 33 ms 让界面刷新一帧——GLAZE 听完、表与频谱稳定；
3. 按几种状态离屏渲染编辑器成 PNG：

| 文件 | 状态 |
|---|---|
| `zh-default.png` / `en-default.png` | 默认（GLAZE 45 %、自动风格），中文 / 英文 |
| `zh-original.png` | A / B 听原声（B）+ 等响 |
| `zh-truepeak.png` | 限幅器旁通 + Color 输出 +6 dB → 真峰值超限的橙色提示 |
| `zh-settings.png` / `en-help.png` | 设置面板 / 帮助面板 |
| `zh-glaze90.png` / `zh-glaze0.png` | GLAZE 90 %（开片密、釉色深、粒子多）/ 0 %（无开片、全手动） |
| `zh-zoom70.png` / `zh-zoom150.png` | 界面大小 70 % / 150 %（含缩放提示） |
| `zh-reduced.png` | 减少动效（没有粒子与波纹） |

`--only check` 另跑一组交互检查：直接给真实控件发鼠标事件（拖动、⌘ 精调、双击回默认、Alt 单击输入、滚轮、滑条、
分段选择、CLAY 手动值、电源键、AUTO、等响、A / B、语言切换），逐项打印 PASS / FAIL，任何一项失败退出码为 1；
同时存下 `check-drag.png`（拖动中的数值气泡）、`check-original.png`、`check-english.png`。

## 用法

```bash
# 依赖：freetype2 / fontconfig / X11（含 XInput2、Xcursor、Xinerama、Xrandr）的开发包；中文字体（Noto Sans CJK）
cmake -S . -B build-ui -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=../JUCE
cmake --build build-ui --target celadon_ui_snapshot

xvfb-run -a build-ui/celadon_ui_snapshot --out shots                    # 全部状态，1 倍
xvfb-run -a build-ui/celadon_ui_snapshot --out shots --scale 2          # 2 倍（看 Retina 上的细节）
xvfb-run -a build-ui/celadon_ui_snapshot --out shots --only zh-default,en-help
xvfb-run -a build-ui/celadon_ui_snapshot --out shots --only check       # 交互检查
```

`EXCLUDE_FROM_ALL`：默认构建（`celadon_tests`）不编它，工具坏了也不挡测试。它会改写用户设置文件里的语言 / 动效 / 默认大小
（`~/.config/Celadon/Celadon.settings`），跑完会留下最后一种状态的设置。
