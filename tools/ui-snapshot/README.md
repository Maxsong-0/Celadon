# celadon_ui_snapshot · 界面截图与交互检查（Linux / macOS）

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
| `zh-audition.png` | Gain +8 dB 时 LIMIT 的耳机「试听差值」（只听限幅削掉的部分） |
| `zh-truepeak.png` | 限幅器旁通 + Color 输出 +6 dB → 真峰值超限的橙色提示 |
| `zh-settings.png` / `en-help.png` | 设置面板 / 帮助面板 |
| `zh-advanced.png` / `en-advanced.png` | 「高级」的推响不刺页（真实参数控制） |
| `zh-target.png` | 目标频谱页，学习参考缺侧链时的等待状态 |
| `en-release.png` / `zh-preview.png` | 发行安全页 / 主窗口的归一化预听提示 |
| `zh-glaze90.png` / `zh-glaze0.png` | GLAZE 90 %（开片密、釉色深、粒子多）/ 0 %（无开片、全手动） |
| `zh-zoom70.png` / `zh-zoom150.png` | 界面大小 70 % / 150 %（含缩放提示） |
| `zh-reduced.png` | 减少动效（没有粒子与波纹） |
| `zh-input.png` | 频谱仪 IN 虚线开着（默认关）。T-051 改 IN 虚线的建法时用 `--deterministic` 改前改后逐字节比过 |
| `zh-house-layout.png` / `en-house-layout.png` | T-038：曲风 Deep House、交付 俱乐部，两个选择在 GLAZE 标题行左右（Max 2026-10-01 选定）；House 暂定、23 / 64 小节（只存上半，y < 560） |
| `zh-house-status-bars.png` … `en-house-status-seconds.png` | 状态行三种文字（还没听到主段落 / 主段落已听 8 小节 / 宿主没给拍点），只截顶栏胶囊 |
| `zh-house-tip.png` / `en-house-tip.png` | House 暂定期间 GLAZE 状态的悬停气泡（第 3 行「从 drop 开始播放能学得更快」） |
| `zh-house-hint.png` / `en-house-hint.png` | House 反相提示：状态胶囊琥珀 + CLAY 卡片两行提示 |

T-038 那几张的 House 状态（`Status` 的 `house*` 字段）由编排器填；截图工具把处理器的状态快照改一改再交给编辑器
（`setGlazeStatusSource`，见 `injectStatus`），验的是界面怎么按字段画，不是编排器什么时候置这些字段。

`--only check` 另跑一组交互检查：直接给真实控件发鼠标事件（拖动、⌘ 精调、双击回默认、Alt 单击输入（含中文输入法的全角数字、
输入框开着时按键抬起不外传——Windows 上松开 Alt 会激活宿主菜单栏）、滚轮、滑条、
分段选择、曲风 / 交付两个选择（T-038：点选、说明格不可点、位置不压旋钮与左下说明）、CLAY 手动值、电源键、AUTO、等响、A / B、试听差值与 A / B 互斥、LIMIT 旁通时退出试听、「高级」面板开合、参考学习/取消、归一化预听互斥/离线/关窗清理、语言切换），
逐项打印 PASS / FAIL，任何一项失败退出码为 1；同时存下 `check-drag.png`（拖动中的数值气泡）、`check-original.png`、
`check-audition.png`、`check-advanced.png`、`check-english.png`。

倒数第三、四条是 T-041（BUG-008）加的小数像素比检查：编辑器保持 1 倍，只把离屏画布的像素比设成 0.7 ～ 2.5（含 T-039 实测的
1.03293），看状态胶囊呼吸与对齐缓存（`AlignedComponentCache`）的局部作废是不是只重画作废区、结果与整块重画逐字节相同；
1 倍及以上呼吸帧一段字都不排版，低于 1 倍最多排第一段（设备像素比逻辑像素大，圆点区取整后可能与它的字形余量落进同一列）。

末尾两条是 T-051 的频谱仪检查：

- **IN 虚线**：频谱仪自己切段的 `createDashedStrokeReusing`（中线段与虚线都写进成员路径，播放中每帧重建不再新建路径；
  评审 R-07）与 JUCE 的 `PathStrokeType::createDashedStroke` 逐元素逐位相同——84 条随机折线（乱跳、缓变、贴顶贴底、带重复点）。
  JUCE 升级后改了切段算法，这一条会先报出来。
- **增量重画 vs 整帧**（评审 R-08 的建议，守 R-02 那一类「脏区漏标、旧画面留在屏上」）：给编辑器挂一个只记账的缓存图，
  记下每帧所有 `repaint` 的区域，按窗口系统的口径只把脏区画进一张一直留着的「屏幕」图——×1 矩形列表（Windows Direct2D、Linux）、
  ×1 包围盒（macOS）、×2 矩形列表，再与同一时刻整帧重画的频谱仪那一块逐字节比，有一个像素不同就失败。场景照评审 B1 的 S8：
  GLAZE 0、峰值线关、斜率 0，6 kHz −12 dBFS 稳态正弦 12 s（最后 1 s 频谱仪必须 0 帧被标脏：确认已静止）→ 斜率换 4.5 →
  切到 B（OUT 清零重来）→ 再跑 1 s，每帧都比。与评审时有一处不同：换斜率之后先「只重画、不推频谱」一帧（点击引起的重画
  先于显示器的帧回调到达，两者谁先到不固定）——T-036 修订（评审 R-07）改按加斜率后的纵坐标判变化之后，换到 4.5 时压在底边下、
  还在往下落的峰值保持数据会露出来，先推一帧的话旧区域当帧就跟上了，照原样走抓不到 R-02。验证过有效：在 main 的临时副本里撤掉
  R-02 的修复，这一条失败（×1、×2 矩形列表各 6 帧有残影，切 B 后第一帧 6 kHz 峰顶 80 / 243 个像素）；放回去通过
  （做法与输出见 `docs/validation/2026-10-04-T051-spectrum-dash-snapshot/`）。

## 用法

```bash
# 依赖：freetype2 / fontconfig / X11（含 XInput2、Xcursor、Xinerama、Xrandr）的开发包；中文字体（Noto Sans CJK）
cmake -S . -B build-ui -G Ninja -DCMAKE_BUILD_TYPE=Release -DCELADON_JUCE_DIR=../JUCE
cmake --build build-ui --target celadon_ui_snapshot

xvfb-run -a build-ui/celadon_ui_snapshot --out shots                    # 全部状态，1 倍
xvfb-run -a build-ui/celadon_ui_snapshot --out shots --scale 2          # 2 倍（看 Retina 上的细节）
xvfb-run -a build-ui/celadon_ui_snapshot --out shots --only zh-default,en-help
xvfb-run -a build-ui/celadon_ui_snapshot --out shots --only check       # 交互检查
xvfb-run -a taskset -c 5 build-ui/celadon_ui_snapshot --only spectrum-bench --seconds 4   # 频谱仪 30 / 60 / 120 帧基准（T-036）
```

`--only spectrum-bench`（T-036）：音乐按帧率在放，每帧处理 1 / fps 秒音频、`advanceForSnapshot (1 / fps)`、只重画频谱仪那一块，
30 / 60 / 120 帧 × 1 / 2 倍各跑 `--seconds` 秒，打印每帧与每秒的 CPU（软件渲染器口径，绑单核跑更稳）。只用改前改后都有的接口，
能在改前的树上量基线。每帧重画整块频谱仪是上限：真机上只重画曲线新旧包围盒。

Windows 上也能编（CMake + MSVC 或 clang-cl，GUI 目标在 Windows 上不需要额外的依赖包），用法相同。

`EXCLUDE_FROM_ALL`：默认构建（`celadon_tests`）不编它，工具坏了也不挡测试。它会改写用户设置文件里的语言 / 动效 / 默认大小
（`~/.config/Celadon/Celadon.settings`），跑完会留下最后一种状态的设置。`check` / `bench` / `digest` 开跑前都先放回同一基线
（中文、不减少动效、默认大小 1），不受上一次留下的值影响（T-039：之前 `check` 漏了默认大小，跑过 `zh-zoom70` / `zh-zoom150` 后
再跑检查，编辑器按 0.7 / 1.5 倍打开，T-032 的两条缓存检查必然失败，看起来像偶发）。`check` 要求编辑器正好 1200 × 820：
虚拟屏幕至少 1240 × 900（`xvfb-run` 默认 1280 × 1024 够），否则第一条就报失败并停下。

## macOS 验证说明

消息泵支持 CoreFoundation，可将工具链接到同一轮 Xcode Release 的 `libCeladon.a`，验证实际发布代码中的处理器与编辑器。
macOS 验证时必须为工具单独编译 `UiSettings.cpp` 副本，把设置目录改到专用临时目录；不能让截图操作覆盖日常插件偏好。
`--only check` 会直接运行检查并退出；生成各状态图片请另一次调用，不要把 `check` 与图片名称放在同一列表。
新增检查还包括主/参考使用不同信号的总线隔离、参考工程保存和重启，以及实际 Apple Release 库的差值试听精确静音回归。
