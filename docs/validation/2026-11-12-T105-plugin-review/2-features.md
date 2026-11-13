# T-105 ② 线：功能、工作流与宿主稳健性体检

- 档位 · 模型 · effort：用户点名 · Fable 5.2 · max
- 基线：工作树 HEAD = main `a768b8a`（版本 0.1.8.10）；写于 2026-11-12 15:0x PST
- 性质：只读审视。没改任何代码、档案；没跑任何测试和编译（需要跑的命令写在各条「怎么验证」里，交主会话按项目规定的测试机跑）
- 路径口径：`Source/…`、`tests/…`、`docs/…` 相对仓库根；JUCE 是 `/home/maxsong/All-Project/JUCE`（9.0.2，只读核对）；vault 指 `项目/Celadon-Docs`

---

## 0. 总评

1. 处理器层的宿主集成做得比同体量的插件扎实：同规格 `prepareToPlay` 只复位不重建、延迟只随 Lookahead 变并在消息线程上报、宿主旁通走同一条链、NaN / 非规格化数 / 坏档 / 旧工程缺参都有兜底，音频线程路径上没找到分配、加锁、日志。27 个小项里 23 个「查过，没问题」，3 个有问题（C9、C17、C27），1 个缺实测数（C26）。
2. 有两件事会直接伤到成品或稳定性，而且都已经有人看见、却都停在半路：**T-095（离线导出还带着等响 / 听原声 / 试听差值）写完一个月没合**，0.1.8.10 是带着这个缺陷发的；**T-099（存读档与音频线程的数据竞争）确认存在**，读档一侧按代码推演可以越界写内存。
3. 这两件事之所以能漏到现在，根子是同一个：**处理器层（`PluginProcessor`）没有任何自动测试**，CI 只跑链级无头用例，没有 pluginval、没有线程检查器。
4. 功能面不缺「大件」。和 Pro-L 2、smart:limit、Ozone 比，缺的是监听与解释这一圈的收口（等响不该进工程、A/B 只能比原声、PSR / 落差读数没露出来），都在 T-086 的范围里。预设、撤销栈、参考曲播放器、相关表这些不建议做。
5. 先后顺序建议：T-095 合入 → T-099 修 + 处理器层测试 → T-098 第 8 条（换曲风）按「播放中切」定 → T-086 → T-087 → T-088。

## 1. 最该先做的 5 条

| 序 | 编号 | 一句话 | 成本 | 把握 |
|---|---|---|---|---|
| 1 | 2-02 | 把 T-095 变基、验证、合入：离线导出自动旁通三种监听 | S | 高 |
| 2 | 2-01 | T-099：存读档与音频线程互斥（先保正确，再做无锁） | M | 高（竞争存在）/ 中（崩溃后果是推演） |
| 3 | 2-03 | 给处理器层补自动测试：无头处理器用例 + 线程检查器 + 发版时跑 pluginval | M | 高 |
| 4 | 2-04 | T-098 第 8 条建议选 2：播放中换曲风要真的切主段模型，否则听到的和导出的可能不是一回事 | M | 高（代码）/ 中（听感差多少） |
| 5 | 2-05 | T-086 开工前钉死两条边界：等响不进工程；A/B 快照与宿主预设只管「设置」，不带 LIVE 画像 | M | 中 |

---

## A. 功能盘点表

来源：`Source/ParameterIds.h:32-82`、`Source/Parameters.cpp`（全文）、`Source/ParameterDefaults.h:29-78`、`Source/ParameterRanges.h`、`Source/PluginProcessor.cpp`、`Source/PluginEditor.cpp`、`Source/ui/Popovers.h:44-166`、`Source/ui/UiSettings.cpp:17-19`、`site/src/content/docs/zh/{meters,listening}.md`。

「自动化」= 宿主能不能画自动化曲线；「进工程」= 会不会随 DAW 工程保存。

### A1. 参数（38 个，全部进工程）

| 模块 | 功能（参数 ID） | 入口 | 自动化 | 默认值 |
|---|---|---|---|---|
| GLAZE | 总量 `glaze_amount` | 中央大旋钮 | 能 | 35 % |
| GLAZE | 风格 `glaze_style`（自动 / 温润 / 冲击 / 通透） | 顶部风格选择 | 能 | 自动 |
| GLAZE | 曲风 `glaze_genre`（通用 / Deep House / Tech House / Melodic-Progressive） | 曲风选择 | **不能**（`Parameters.cpp:221-223`） | 通用 |
| GLAZE | 交付 `glaze_delivery`（标准 / 流媒体 / 俱乐部） | 交付选择 | **不能**（`Parameters.cpp:224-226`） | 标准 |
| BALANCE | Recover / Tame / Brighten | 卡片旋钮 | 能 | 0 / 0 / 0 |
| BALANCE | 速度 `balance_speed`（Slow / Mid / Fast） | 卡片分段 | 能 | Mid |
| BALANCE | 旁通 `balance_bypass` | 卡片开关 | 能 | 关 |
| CLAY | 低切：模式 + 频率（10～60 Hz） | 卡片行 | 能 | 自动 / 25 Hz |
| CLAY | 收单：模式 + 频率（40～300 Hz） | 卡片行 | 能 | 自动 / 120 Hz |
| CLAY | 加宽：模式 + 量（−3～+3 dB） | 卡片行 | 能 | 自动 / 0 dB |
| CLAY | 旁通 `clay_bypass` | 卡片开关 | 能 | 关 |
| COLOR | Tone（±3 dB）/ Glue / Heat / Mix / Output（±6 dB） | 卡片旋钮 | 能 | 0 / 0 / 0 / 100 % / 0 |
| COLOR | 旁通 `color_bypass` | 卡片开关 | 能 | 关 |
| LIMIT | Gain（0～24 dB）/ Ceiling（−3～0 dBTP） | 卡片旋钮 | 能 | 0 dB / −1.0 dBTP |
| LIMIT | Release（10～1000 ms）+ Auto Release | 卡片 | 能 | 100 ms / 开 |
| LIMIT | Lookahead（1～10 ms，步进 0.5） | 卡片 | 能（有副作用，见 2-10） | 2 ms |
| LIMIT | Clip | 卡片旋钮 | 能 | 0 |
| LIMIT | **等响** `limit_listen` | 顶部「等响」按钮（`PluginEditor.cpp:269`） | 能 | 关（**进工程**，见 2-05） |
| LIMIT | 旁通 `limit_bypass` | 卡片开关 | 能 | 关 |
| 高级 | 推响不刺：模式（自动 / 关 / 手动）+ 量 | 高级面板 | 能 | 自动 / 50 % |
| 高级 | 瞬态保留 `master_transient` | 高级面板 | 能 | 50 % |
| 高级 | 目标频谱：模式（关 / 自动 / 暖 / 中性 / 亮 / 参考）+ 量 | 高级面板 | 能 | 自动 / 35 % |
| 高级 | 编码余量 `master_codec_margin_db`（0～1.5 dB） | 高级面板 | 能 | 0 dB |
| 高级 | 导出抖动 `master_dither`（关 / 16 bit / 24 bit） | 高级面板 | 能 | 关 |
| 高级 | 归一化预听目标 `master_preview_lufs`（−24～−8） | 高级面板 | 能 | −14 LUFS |

### A2. 不是参数的功能

| 功能 | 入口 | 自动化 | 进工程 | 说明 |
|---|---|---|---|---|
| A/B：母带 / 原声 | 顶部 A/B（`PluginEditor.cpp:758`） | 不能 | 不进 | 界面状态；关窗口自动回母带（`PluginEditor.cpp:816-826`） |
| 试听差值 | LIMIT 卡片耳机 | 不能 | 不进 | 同上 |
| 归一化预听 | 高级面板按钮 | 不能 | 不进 | 读档时关掉（`PluginProcessor.cpp:950`），离线时关掉（`CeladonChain.cpp:166`） |
| 学习参考曲频谱（侧链） | 高级面板：学习 / 取消 / 清除 | 不能 | **进**（`REFERENCE_SPECTRUM` 节点，`PluginProcessor.cpp:913`） | 侧链总线默认关（`PluginProcessor.cpp:74`） |
| GLAZE 实时画像（LIVE） | 无入口，自动 | — | **进**（`GLAZE_STATE`，`PluginProcessor.cpp:910`） | 窗口满 512 段时未压缩 1.52 MB（见 2-06） |
| 计量：IN / OUT 真峰值、GR、GLUE GR、整体响度、短期 S、TP 最大、PLR、归一预估、重新计量 | 竖表 + 响度小窗 | — | 不进 | 见 B 节第 1 项 |
| 频谱仪（OUT 填充 / 平均 / 峰值保持 / IN 虚线，斜率 0 / 3 / 4.5） | 中部 | — | 显示开关存在哪没核实 | 采样口在链出口 |
| BALANCE 修正曲线、CLAY 三灯、GLAZE 状态行、实际值淡弧 | 各卡片 | — | — | 可解释性的现有四件 |
| 界面缩放 70～200 % | 拖角 / 设置面板 | — | **进**（根属性 `uiZoom`，`PluginEditor.cpp:1159`）+ 用户默认 | 打开时按屏幕收缩（`PluginEditor.cpp:1125-1154`） |
| 语言（中 / 英）、减少动效 | 设置面板 | — | 不进（用户设置文件，`UiSettings.cpp:17-19`） | |
| 延迟读数（毫秒） | 设置面板 | — | — | |
| 预设 / 程序 | 没有 | — | — | 只报 1 个空程序（`PluginProcessor.cpp:366-391`） |
| 撤销 / 重做 | 没有 | — | — | APVTS 没接 UndoManager（`PluginProcessor.cpp:82`） |

---

## B. 与专业母带工具通行能力对照

对照对象的事实只取公开页面（链接见文末）。标「搜索摘要」的是搜索引擎摘要里的说法，没有逐页打开核对。

| # | 能力 | Celadon 现状 | 判定 | 通行做法（公开资料） | 该补 / 不该做 |
|---|---|---|---|---|---|
| 1 | 响度与峰值计量 | 整体 I、短期 S、TP 最大、PLR、IN / OUT 真峰值、GR、GLUE GR、归一预估；**没有**瞬时 M、LRA、PSR 读数、GR 历史 | 有但浅 | Pro-L 2：M / S / I 三档、LRA、最大真峰值、目标预设（−9 / −14 / −23 / −24 / 自定）、暂停 / 复位、开播自动复位。smart:limit：响度、响度范围、平均峰响比 | **补两项现成读数**（PSR、落差损失，DSP 里已经在算，见 2-08）；M 与 LRA 可选；GR 历史并进 T-086 的时间线 |
| 2 | 相关表 / 声像仪 | 没有（CLAY 有低频反相提示） | 没有 | 多数限幅器不带，属于独立表头插件的活 | **不做** |
| 3 | A/B 与等响 | A/B 只能比「母带 / 原声」；等响是参数、进工程、1 s 窗；另有试听差值 | 有但浅 | Pro-L 2：Unity Gain、Audition Limiting、A/B + Copy（两套设置互比）。Ozone：Gain Match 配合 Bypass（搜索摘要） | **补**（T-086）：等响不进工程、改长窗、A/B 两套设置；见 2-05 |
| 4 | 参考曲对比 | 侧链学习参考曲频谱 → 目标频谱「参考」模式；不能切过去听参考曲 | 有但浅 | Ozone 主插件可载入并切换参考曲（搜索摘要）；smart:limit 可上传参考曲给学习用 | **不做**参考曲播放器；现有「学习」够用 |
| 5 | 预设与快照 | 没有 | 没有 | Pro-L 2 有出厂与自存预设 | 预设**不做**（roadmap「以后再议」；自适应插件的预设价值低）；A/B 快照在 T-086 做 |
| 6 | 撤销 / 重做 | 没有；宿主参数级撤销可用 | 没有 | Pro-L 2 顶栏有 Undo / Redo | **不单做**撤销栈；A/B 快照 + 双击回默认已覆盖主要场景 |
| 7 | 交付目标与检查 | 交付三档、归一化预听、编码余量、TP 上限变橙、离线冷导出提示 | 有但浅 | smart:limit：按发布目标与曲风给文字提示（Quality Check） | 合入 T-095；状态行加一条「监听还开着」（并进 T-086）；**不做**检查面板 |
| 8 | 可解释性 | 状态行（取最要紧一条）、实际值淡弧、修正曲线、三灯、目标曲线 | 有但浅 | smart:limit 的响度 / 动态网格；Pro-L 2 的实时电平显示带 GR 峰值标注 | T-086（时间线、预算条） |
| 9 | 抖动与位深 | 关 / 16 / 24 bit，重启后随机序列回固定种子（`CeladonChain.cpp:127-129`） | 有且够好 | Pro-L 2 带抖动与噪声整形；Ozone 用 MBIT+（搜索摘要） | 不动；整形抖动 T-093 已否（R6-07） |
| 10 | 过采样选项 | Clip 级 4×、真峰值检测 8×、Heat 过采样，全部固定（`LimiterModule.h:9-10`） | 有且够好 | Pro-L 2 把过采样倍数交给用户 | **不做**选项，固定是对的 |
| 11 | 延迟模式 | 固定 1384 样本 @ 48 kHz / Lookahead 2 ms（约 28.8 ms），没有低延迟档（`CeladonChain.cpp:696-703`） | 没有 | Pro-L 2 没有零延迟模式，延迟随风格 / 前瞻 / 过采样变（论坛例子约 10.8 ms，搜索摘要） | T-088，排最后（见 2-13） |
| 12 | 单声道检查 / M-S 监听 | 没有 | 没有 | 多放在监听控制器或宿主里 | **不做**独立监听级；T-086 合并监听时最多加一个「单声道听」，T-093 R1-03 仍是存疑 |
| 13 | 自动化友好度 | 38 个参数里 36 个可自动化；手势有 begin / end；每块读一次参数 | 有，有两个坑 | Pro-L 2 支持任意参数的 MIDI Learn | Lookahead 与等响不该可自动化（2-10、2-05）；MIDI Learn **不做**（宿主有） |
| 14 | 界面缩放 | 70～200 % 等比，随工程存，按屏幕收缩 | 有且够好 | Pro-L 2 有多档尺寸、紧凑模式、全屏 | 不动 |
| 15 | 键盘与无障碍 | Alt 点击输入数值、双击回默认、滚轮、精调；没有键盘焦点、读屏标题、宿主右键菜单 | 有但浅 | JUCE 自带读屏支持，但自绘控件要自己给标题与数值接口 | 小补（2-11，P3） |
| 16 | 旧工程兼容 | 参数版本号、缺参特判、存档格式号、LIVE 格式版本、ADR-035 | 有且够好 | — | 逻辑不动；补处理器层的旧存档夹具测试（2-03） |

---

## C. 宿主稳健性代码审查

每项写「查了什么 / 结论 / 证据」。

| # | 小项 | 查了什么 | 结论 | 证据 |
|---|---|---|---|---|
| C1 | `prepareToPlay` 顺序与幂等 | 同规格重复调用会不会整体重建、清表、清画像 | **查过，没问题**。规格没变只 `chain.reset()`，两块表不清；先交参数再 prepare / reset，平滑器直接落位 | `Source/PluginProcessor.cpp:487`、`:496`、`:499-509` |
| C2 | 采样率 / 块长 / 声道变化 | 整体重建时 LIVE 画像会不会丢 | **查过，没问题**。先存 LIVE、各模块重建、再读回。附注：这一趟走 XML + zlib，耗时没量（2-06） | `Source/dsp/CeladonChain.cpp:82-86`、`:103-104` |
| C3 | 先读档、后第一次 prepare | 打开工程的常见顺序下画像按哪个采样率折算 | **查过，没问题**。prepare 之前留原件，prepare 后再读；有用例 | `Source/dsp/GlazeController.cpp:1551-1554`、`Source/dsp/CeladonChain.cpp:85-86`、`tests/GlazeControllerTests.cpp:1340-1346` |
| C4 | 可变块长、超大块 | 宿主给的块比 prepare 时大 | **查过，没问题**。各模块按 `maxBlockSize` 分段；链的切片用引用构造，不分配 | `Source/dsp/BalanceModule.cpp:605`、`ColorModule.cpp:790`、`LimiterModule.cpp:487`、`CeladonChain.cpp:421` |
| C5 | 0 长度块 | 空块会不会除零、推进计数 | **查过，没问题**。链与各模块入口都有守卫；Lookahead 计数只在 `numSamples > 0` 时动。链级没有空块用例（并进 2-03） | `CeladonChain.cpp:394`、`BalanceModule.cpp:601`、`ColorModule.cpp:783`、`LimiterModule.cpp:469`、`ClayModule.cpp:598`、`ReleaseSafety.cpp:72`、`PluginProcessor.cpp:566` |
| C6 | 没 prepare 就 process | 采样率还是 0 时 | **查过，没问题**。各模块 `sampleRate <= 0.0` 直接返回 | 同 C5 各行、`LoudnessMeter.cpp:206` |
| C7 | 总线布局 | 支持哪些布局、侧链怎么取 | **查过，没问题**。主总线单声道或立体声且进出相同；侧链关 / 单 / 立体声；先读侧链再碰主输出。链级用例只有立体声（`tests/ChainTests.cpp:39`），单声道只在模块级测过（并进 2-03） | `PluginProcessor.cpp:70-78`、`:600-624`、`:639-655` |
| C8 | 双精度 | 宿主要 64 位处理时 | **查过，没问题**。没覆盖 `supportsDoublePrecisionProcessing`（全文无），按 JUCE 默认只报 32 位 | `Source/PluginProcessor.cpp`（grep 无匹配） |
| C9 | 离线渲染 | 离线时哪些东西自动关 | **有问题 → 2-02**。离线只关归一化预听；等响、听原声、试听差值照常生效 | `PluginProcessor.cpp:726`、`CeladonChain.cpp:165-166`、`:216`、`:303-305`、`:367`、`:381` |
| C10 | 延迟上报 | 在哪个线程报、什么时候变 | **查过，没问题**。prepare 时直接报；播放中由 30 Hz 定时器在消息线程报；Lookahead 停稳 0.3 s 才换。附注：Lookahead 可自动化（2-10） | `PluginProcessor.cpp:513-518`、`:543-578`、`:580-588`、`:713`、`CeladonChain.cpp:696-703` |
| C11 | 尾巴长度 | 静音暂停类宿主会不会截尾 | **查过，没问题**。报「链延迟 + 余振」，只读两个原子量 | `PluginProcessor.cpp:355-364` |
| C12 | 宿主旁通 | 延迟是否保持、有没有咔哒 | **查过，没问题**。走同一条链的干路，延迟不变，20 ms 交叉淡化；AU 旁通切换附带的 Reset 有 2 块宽限。待定一处：宿主旁通时整体响度要不要暂停积分（2-12） | `PluginProcessor.cpp:633-637`、`:667-680`、`:696-703`、`CeladonChain.cpp:216`、`:357-360`、`:373-374` |
| C13 | `reset()` | 宿主在任意线程调 | **查过，没问题**。只立原子旗，下一块在音频线程里重启并淡入 | `PluginProcessor.cpp:590-593`、`:677-678`、`:710-711` |
| C14 | `releaseResources()` | 之后再 prepare | **查过，没问题**。空实现，规格记着，同规格再 prepare 只复位（内存不释放，是有意的） | `PluginProcessor.cpp:595-597`、`:487` |
| C15 | 存档版本兼容 | 新旧版本互读 | **查过，没问题**。根元素写格式号与版本；缺参按特判补；LIVE 版本更高或旧格式 → 冷启动并提示一次 | `PluginProcessor.cpp:906-908`、`:952-958`、`:968-1012`、`GlazeController.cpp:1562-1583` |
| C16 | 坏档 | 字段缺失、非有限数、长度不对 | **查过，没问题**。各层都先读到临时量、校验过再落位；任何一层不过 → 整份冷启动 | `LiveProfile.cpp:3032-3038`、`:3074-3078`、`PsrGuard.cpp:164-166`、`ContrastGuard.cpp:251-259`、`GlazeGuard.cpp:574-635`、`PluginProcessor.cpp:927-929` |
| C17 | **存读档与音频线程（T-099）** | 宿主线程存读档时音频线程在不在改同一批量 | **确认有数据竞争 → 2-01**（下面 C-T099 专节） | 见专节 |
| C18 | 实时安全：分配 / 锁 / 日志 | 音频线程路径上有没有 | **查过，没问题**（有保留）。锁只有一把，在参考频谱读档里、非音频线程；`make_unique` 都在存读档与构造里；`DBG` 只在界面线程。保留：`MusicAnalyzer.cpp`、`LiveProfile.cpp` 只做了 grep 与重点段落，没逐行通读 | `ReferenceSpectrum.cpp:523`、`GlazeController.cpp:1107-1154`、`:1554`、`PluginEditor.cpp:719`、`PluginProcessor.cpp:650` |
| C19 | 非规格化数 | | **查过，没问题** | `PluginProcessor.cpp:641` |
| C20 | NaN / Inf | 输入、输出、参数 | **查过，没问题**。输入坏块清零计数；输出坏了链自己清空并淡入；参数非有限值有兜底 | `PluginProcessor.cpp:683-687`、`:715-717`、`:528-530`、`:546` |
| C21 | 界面读数的线程安全 | 界面读音频线程的数 | **查过，没问题**。表读数是原子量，GLAZE 状态走无锁快照，频谱仪走单写单读队列，参考频谱走票号 | `GlazeController.cpp:545`、`ReferenceSpectrum.h:83-91`、`PluginProcessor.cpp:702-703` |
| C22 | 编辑器开着时读档 | 状态树被换掉时界面怎么跟 | **查过，没问题**。两个回调都只投递异步更新。小瑕疵：宿主在非消息线程读旧工程时，「旧聆听结果已丢弃」那条提示不弹 | `PluginEditor.cpp:1192-1207`、`PluginProcessor.cpp:882-893` |
| C23 | 多实例 | 有没有跨实例共享的可变状态 | **查过，没问题**。静态量都是只读表。每实例实时画像约 2.3 MB | `LiveProfile.h:47`、`ColorModule.cpp:389-446`（压缩前读过） |
| C24 | 停播 / 跳播 / 循环 | 时间线不连续时 | **查过，没问题**。分析器断开短窗、开着的段就地定稿，LIVE 统计不清 | `PluginProcessor.cpp:723-799`、`GlazeController.h:106-109` |
| C25 | 自动化密集写入 | 每块都改参数 | **查过，没问题**。每块读一次参数；目标方案每次 `endBlock` 最多重算一次 | `PluginProcessor.cpp:698`、`GlazeController.cpp:1196-1226`、`:1394-1416` |
| C26 | 192 kHz × 块长 32 | 实时画像的重算会不会挤爆一块 | **设计上没问题，没有实测数 → 2-09**。重算按 5 ms 网格分片，每片估计 8 µs | `LiveProfile.h:231-232`、`LiveProfile.cpp:1117-1129` |
| C27 | 播放中换曲风 | 主段模型什么时候切 | **有问题 → 2-04**。只在 `prepareToPlay` 切 | `GlazeController.h:114-116`、`PluginProcessor.cpp:511` |

### C-T099 专节：存读档与音频线程的数据竞争

**结论：确认存在。** `getStateInformation` 与 `setStateInformation` 在宿主线程直接读写音频线程每块都在改的普通变量，中间没有任何锁、原子量或交接。读档一侧按代码推演可以越界写内存；存档一侧会存出前后不一致的画像，下次打开整份冷启动。

**链路（都是我在 HEAD 上读过的行）**

1. 处理器层不加锁：存 `PluginProcessor.cpp:896-918`（`:910` 取 GLAZE 状态），读 `:920-966`（`:961` 交 GLAZE 状态）。
2. 链只是转发：`Source/dsp/CeladonChain.h:167-168`。
3. 编排器自己写明了约束，但没人守：`Source/dsp/GlazeController.h:47-49`「createStateXml / restoreStateXml 直接读写音频线程每块在改的量，编排器内部不加锁；其中 LiveProfile 要求自己的存读档不与音频线程接口同时调用」；`Source/dsp/LiveProfile.h:49-50` 同。
4. 宿主线程写的东西：`GlazeController.cpp:1002-1103`（`restoreBody` 逐项赋值）、`:973-1000`（冷启动，`:997` `live->reset()`）、`LiveProfile.cpp:2997-3100`（`:2999` 先冷启动，`:3053` 往环里写段，`:3081-3089` 改计数并同步重算，`:3092-3097` 再跑若干遍重算）、三个守门与出口电平（`PsrGuard.cpp:168-172`、`ContrastGuard.cpp:261-269`、`GlazeGuard.cpp:635`、`OutputLevelTracker.cpp:249`）。
5. 音频线程同时在动同一批量：`LiveProfile.cpp:1093-1149`（`advanceTo`：`:1108` 开一遍、`:1119` 跑一片、`:1127` 补完、`:1137` 发布）、`:1264-1359`（`doStep`，例如 `:1277` `referenceOne (work.size - 1 - cursor++)`、`:1332` `trialOne (cursor++)`）、`:2518-2523`（发布时 `std::swap` 两个向量）。
6. JUCE 包装层不替插件挡：VST3 的存读档直接转发（`juce_audio_plugin_client_VST3.cpp:2761-2763`、`:2783-2822`），AU 同（`juce_audio_plugin_client_AU_1.mm:835-853`、`:867-923`）；而两家的音频回调都持着 `getCallbackLock()`（VST3 `:3714`，AU `:2189`）。

**后果推演（没有实跑复现，把握「中」）**

- 读档侧：宿主线程的 `restoreState` 把 `step` 设成「在算」并调 `finishPass()`，这时音频线程的 `advanceTo` 看到 `step != idle` 也去跑 `runSlice()`。两个线程同时执行 `doStep`，共用非原子的 `cursor` 与 `work`：`work.size - 1 - cursor++` 可以变成负数再当下标；两边还可能同时 `std::swap` 同一对向量。结果是越界写或堆损坏，表现为偶发崩溃或之后的怪声。
- 存档侧：`createState`（`LiveProfile.cpp:2958-2983`）先写 `pending = arrived − published.count`，再循环到 `arrived`；中间音频线程进了一段或发布了一份，写出来的段数就和头里的计数对不上。读回时 `:3074` 的 `in.atEnd()` 校验不过 → 整份冷启动，学到的画像静默丢失。
- 什么时候会碰上：播放中存工程、宿主自动保存（存档侧）；播放中换宿主预设、宿主撤销（读档侧）。JUCE 论坛有 REAPER 每拖一下滑杆就取一次状态做撤销点的帖子，也有「processBlock 和 setStateInformation 被不同线程同时调」的竞争报告。**打开工程（先读档、后开始处理）这条最常见的路是安全的**，所以平时不容易撞上。
- 为什么现有检查抓不到：CI 只跑链级用例，没有线程检查器；pluginval 的「Background thread state」用例在后台线程存读档时并不同时跑音频（见 2-03）。

**对照：仓库里已有一个做对了的样板。** 参考频谱的读档走票号 + 逐字原子，音频线程在块边界接手（`Source/dsp/ReferenceSpectrum.h:83-91`、`ReferenceSpectrum.cpp:487-526`）。GLAZE / LIVE 这一路没有照这个做。

**修法**见 2-01。

---

## D. 已立项任务的评估与先后

| 任务 | 现状（HEAD 上核对） | 还缺什么 | 建议次序 |
|---|---|---|---|
| **T-095** 离线旁通三种监听 | 分支 `origin/claude/t095-offline-bypass-listen` 4 个提交，基点 `3a7a367`；分支里的 `docs/validation/2026-10-11-T095-offline-monitoring/DGX.md` 写着「本机未编译、未测试」，并记录 Max 10-11 15:09「全按推荐」（自写用例四项修正放行；20 ms 淡化选 A）。基点之后 main（`a768b8a`）只动了它碰过的 5 个文件（`Source/ui/Localisation.cpp` 与 faq / glaze 中英四页），改的都不是同一行；`CeladonChain.cpp` 与两个测试文件 main 没动，变基应该是干净的。提醒：本仓库本地的 `main` 引用停在 `36d4450`（10-06），比较要用 `origin/main` | 变基到 `a768b8a`；跑定向用例、main 上的反证、三项改坏验证、全套；合入；更新日志写一句「等响忘关时离线导出不再被衰减」 | **第 1**（2-02） |
| **T-099** 存读档竞争 | 待认领；本线已给出结论（确认存在） | 修（2-01）+ 能抓住它的测试（2-03） | **第 2** |
| **T-098 第 8 条** 换曲风 | 已给 Max 二选一，没回（TASKS T-098 行） | 本线建议选 2，理由见 2-04 | **第 3**（可与 T-099 共用交接机制） |
| **T-086** 可解释性与 A/B | 待开工 | 开工前钉死两条边界（2-05）；把 PSR / 落差读数（2-08）、「监听还开着」提示、等响长窗（R2-09b）一起收 | **第 4** |
| **T-087** 冻结 / 打印可复现 | 阻塞，等 Max 拍板 | 目标里补「备选版本用同一套决定」（2-07）；决定冻结存不存进工程 | **第 5** |
| **T-088** 延迟 ≤ 10 ms | 阻塞，等 Max 拍板 | 做成不可自动化的模式开关；先量再做（2-13） | **第 6** |
| T-096 (f) CPU 基线 | 没合 main | 合入后补处理器层 192 kHz / 32（2-09） | 与上面并行 |

**没人提过的缺口**（在 TASKS、T-093 报告、vault 的 ADR / 规格里都没搜到）：

1. 处理器层没有自动测试，这是 T-095 与 T-099 两类问题的共同漏点（2-03）。
2. pluginval 的现成用例抓不到 T-099 这种竞争，必须自己写「一边跑音频一边存读档」的用例（2-03）。
3. 换曲风不切模型的真正代价是「听到的和导出的可能不是一回事」：AU 宿主切离线渲染时包装层会重新 `prepareToPlay`，模型在导出开头才切（2-04）。T-098 的二选一里没有写到这一层。
4. LIVE 存档在「每动一下参数就取一次状态」的宿主里的体积与耗时（2-06）。
5. 冻结的另一个用途：伴奏版、无人声版、干净版要用和主版本同一套处理（2-07）。
6. 宿主旁通时整体响度照旧积分，代码里写着等 Max 确认，但 TASKS 里没有对应条目（2-12）。

---

## E. 不建议做（守住极简定位）

1. **预设系统**：自适应插件的参数本来就少，曲风 + 交付 + 风格已经是「预设」；预设还会带着别的歌的画像乱跑。
2. **插件内撤销 / 重做栈**：A/B 快照加双击回默认覆盖主要场景，宿主也有参数级撤销；自己做一套要和宿主撤销对账，容易两套打架。
3. **参考曲播放器 / 响度匹配对比**：是独立工具的活；现有「学习参考曲频谱」已经把参考曲用在该用的地方。
4. **相关表、声像仪、矢量示波器**：表头不是这个插件的卖点；低频反相已有提示。
5. **过采样倍数、限幅风格之类的专家选项**：固定是优点；一开放就变成另一个 Pro-L。
6. **Quality Check 式的检查面板**：状态行已经是「最要紧的一条」，加一条「监听还开着」就够。
7. **MIDI Learn**：宿主都有。
8. **独立的 M / S 监听级**：T-093 R1-03 仍存疑；最多在 T-086 合并监听时给一个「单声道听」。
9. **只靠 pluginval 当稳健性的门**：它测不到存读档与音频并发，会给出虚假的安全感；要配自写的并发用例。
10. **为了无锁而无锁**：T-099 先用「短暂互斥」把正确性拿到手，量过耗时再决定要不要做镜像与指针交换。
11. T-093 已否的几项维持：自动收单、ARA、两遍渲染、硬件建模、整形抖动、入口 trim。

---

## F. Findings 明细

### 2-01 · P0 · T-099：存读档与音频线程的数据竞争确认存在

- **证据**：见 C-T099 专节第 1～6 条。
- **为什么要紧**：读档侧可以越界写内存（播放中换宿主预设、宿主撤销）；存档侧会让工程里的画像作废（播放中保存）。两种都是偶发、难复现、用户无法自查的那一类。
- **建议**
  - S 档（先保正确）：`setStateInformation` 把解压、解析、校验放在锁外，只把「落位」那一小段包进 `const juce::ScopedLock lock (getCallbackLock());`（两家包装层的音频回调本来就持这把锁，所以互斥成立）；`getStateInformation` 在锁内只把原始状态拷成一份快照，出锁后再拼 XML、压缩。为此 `LiveProfile::restoreState` 要拆成「读到暂存」与「提交」两步，暂存区在宿主线程分配。
  - M 档（无锁，量过再定）：实时画像每发布一份就把增量写进一份存档镜像，宿主线程读镜像；读档用暂存 + 块边界指针交换，照参考频谱的票号做法。
  - 同一批改掉两处过时注释：`PluginProcessor.cpp:900`「已提交版本与独立执行快照」、`:925-926`「下一块接到存档时整体直接落到位」，它们描述的是已经删掉的交接机制，会让读代码的人以为这里是安全的。
  - 修的范围要覆盖全部直接读写点：编排器 Impl、LiveProfile、预算 / 落差 / PSR 三个守门、出口电平、ClayGuard。
- **怎么验证**：新增处理器级用例「一个线程连续 `processBlock`（48 kHz / 64，GLAZE 100，带音乐素材让窗口有几十段），另一个线程循环 `getStateInformation` → `setStateInformation`」，在线程检查器下跑：修之前应报竞争，修之后 0 报告；再加「播放中存出的档读回不冷启动」。命令（主会话在项目规定的测试机上跑，目标名是本线起的，要新建）：`cmake -S <src> -B <build>/tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" && cmake --build <build>/tsan --target celadon_processor_tests && <build>/tsan/celadon_processor_tests`。
- **成本** M · **把握** 高（竞争存在是代码事实）；崩溃后果是推演，中 · **风险**：锁会让音频线程在存读档那一刻等零点几毫秒，192 kHz / 32 下可能掉一块，所以要量持锁时间；存档格式不变。

### 2-02 · P0 · T-095 没合：0.1.8.10 离线导出仍带等响 / 听原声 / 试听差值

- **证据**：`Source/dsp/CeladonChain.cpp:165-166`（离线只关预听）、`:216`（听原声 = 干路，不看离线）、`:367`（等响不看离线）、`:381`（差值不看离线）；分支 `origin/claude/t095-offline-bypass-listen`（`13ca706`、`6987e38`、`d420a83`、`3b1e0fc`）；分支内 `docs/validation/2026-10-11-T095-offline-monitoring/DGX.md` 首行「本机未编译、未测试」；vault `04-协调/2026-10-11-T-095-离线旁通监听交接.md:86`（新用例在 main 上三种离线情形全败，证明缺陷真实）。
- **为什么要紧**：等响是进工程的参数，关了窗口也还开着，离线导出的成品会被整体衰减；窗口停在 B 导出，成品是原声；差值开着导出，成品是差值。都是无声无息地交出错的文件。修复已经写完、Max 已全按推荐，只差一次变基和一次验证。
- **建议**：变基到 `a768b8a`（基点之后 main 只动了它碰过的 5 个文件，都不是同一行；`CeladonChain.cpp` 没动，不用手工对代码）；按分支里的清单跑；合入；发一个补丁版本并在更新日志写明。
- **怎么验证**：分支清单里的四步：定向用例 `CeladonChainMonitoring` 全过；同一组用例放到 main 代码上应失败；三项改坏验证各自让对应断言失败；全套与 main 基线比没有新增失败，链摘要只有 `preview-listen-exit` 一条不同。
- **成本** S · **把握** 高 · **风险**：接受「离线 + 等响开」的导出结果变化（ADR-035 口径，Max 已定）；不报离线的宿主仍然挡不住，要靠 2-05 的「等响不进工程」。

### 2-03 · P1 · 处理器层没有任何自动测试

- **证据**：`CMakeLists.txt:183`（`celadon_tests` 不含 `PluginProcessor.cpp`）、`:338-343`（只有截图工具 `celadon_ui_snapshot` 编了它）；`tests/` 里搜 `CeladonAudioProcessor` 无匹配；`.github/workflows/ci.yml`（只编、只跑 `celadon_tests`）；`docs/BUILDING.md:42-43`（`auval` 只在发版时手跑）；vault `03-记忆/ADR-003-用Projucer不用CMake.md:17`（pluginval「尚未接入，待 Max 定」）；`tests/ChainTests.cpp:39`（链级只测立体声）。
- **为什么要紧**：总线布局、存读档、缺参补齐、旁通、离线旗、延迟上报、Reset 宽限这些宿主契约全在处理器层，现在只靠人读代码。T-095 与 T-099 都是这一层的问题。
- **建议**：三层。① 新建无头目标 `celadon_processor_tests`（源文件清单照 `celadon_ui_snapshot`），用例：存 → 读 → 再存相等；旧版本存档夹具经 `setStateInformation` 读入后参数与预期一致；单声道 / 立体声 / 带侧链三种布局各跑一段；`processBlockBypassed` 与输入按上报延迟对齐；0 长度块、超过 prepare 块长的块；同规格 / 换规格重复 `prepareToPlay`；播放中 `reset()`。② CI 加一个线程检查器构建，只跑并发那几条（2-01）。③ 发版清单里在 `auval` 旁边加 pluginval（Mac 与 Windows 包各跑一遍，严格度 8 以上）。
- **注意**：pluginval 抓不到 2-01。它的「Background thread state」（严格度 7）在后台线程存读档，但不同时跑音频；「Parameter thread safety」（严格度 7）才是一边改参数一边 `processBlock`。所以并发用例必须自己写。
- **怎么验证**：新目标进 CI 后，故意把 `addMissingParameterNodes` 的一个特判删掉、把 `isBusesLayoutSupported` 放宽，应各有用例变红。
- **成本** M · **把握** 高 · **风险**：处理器构造会起定时器，无头环境要有消息管理器（截图工具已经证明能编能跑）。

### 2-04 · P1 · 播放中换曲风不切主段模型：建议 T-098 第 8 条选 2

- **证据**：`Source/dsp/GlazeController.h:113-115`（「播放中换曲风，主段模型要到下一次 prepareToPlay 才切」）；`GlazeController.cpp:1201`（每块只记下 `pendingModel`）、`:1698-1702`（`syncLiveModel` 才真正交给 LiveProfile）；`CeladonChain.cpp:68-72`、`:106`；`PluginProcessor.cpp:511`（只有 `prepareToPlay` 调）；`LiveProfile.h:49-50`（`setConstants` 会同步重算已发布的画像）；`juce_audio_plugin_client_AU_1.mm:794-812`（AU 宿主切离线渲染时，包装层重新 `prepareToPlay`）。
- **为什么要紧**：用户边听边换曲风是最自然的用法。现在换了以后分析器与规划器立刻跟，但「哪些段算主段」还按旧曲风算，一直到宿主下一次 `prepareToPlay`。很多宿主停播 / 开播不会重新 prepare，而导出前会：AU 宿主一切到离线渲染，模型当场切换、画像当场重算。于是**试听时听的是旧模型，导出的是新模型**。T-098 给 Max 的二选一里，选 1（维持现状 + 界面提示）解决不了这一条。
- **建议**：选 2，播放中几秒内切完。实现上复用 2-01 的交接机制：消息线程备好新常数，音频线程在块边界接手，重算走现有的 5 ms 分片，不在一块里同步算完。
- **怎么验证**：新用例「播到第 k 段时换曲风，再过一遍重算后的已发布画像 = 一开始就用新曲风 prepare、喂同样 k 段的画像」；再加「换曲风后不 prepare 与 prepare 一次，随后 10 s 输出逐位相同」。
- **成本** M · **把握** 高（代码事实）；两种模型在真实曲子上差多少没量，中 · **风险**：切换那几秒目标方案会滑动；按 ADR-034 要附最小指标集对照。

### 2-05 · P1 · T-086 开工前要钉死的两条边界

- **证据**：`Source/Parameters.cpp:115`（等响是普通布尔参数，进工程、可自动化）；`Source/PluginEditor.cpp:269`（按钮直接写参数）；`Source/PluginProcessor.cpp:900-901`（注释说 GLAZE 状态「不该让宿主的撤销 / 预设机制去碰它」）对 `:910-914`（实际上附在同一份宿主会拿去做预设与撤销的状态里）；`LiveProfile.h:23-24`、`:27-28`（换歌只有软遗忘）；公开资料：Pro-L 2 的 A/B + Copy 是两套设置互比。
- **为什么要紧**：① 等响进工程是 T-095 之后仍然留着的洞：实时导出、以及不报离线的宿主，成品照样被衰减。② T-086 要做 A/B 快照。如果快照沿用现在的整份状态，切一次 A/B 就会把实时画像也换回去（还会走 2-01 那条读档路）；宿主预设同理，会把上一首歌的画像带进下一首。
- **建议**：① 等响改成界面监听状态：不进工程、不可自动化、关窗口自动关；旧工程里的 `limit_listen` 读到也当关；参数 ID 留着不删（删了宿主自动化会错位）。② 快照只存参数值，不存 LIVE，切换时画像继续跟。③ 宿主在「已经在跑」的实例上读档时怎么对待 LIVE，要 Max 定（见 G 节待拍板 1）。
- **怎么验证**：处理器级用例：存档里 `limit_listen = 1` 读入后等响为关；切快照前后 `getGlazeStatus()` 的画像置信度不回落；链摘要在等响关时逐位不变。
- **成本** M · **把握** 中 · **风险**：旧工程里有人给等响画过自动化的会失效（概率很低）；要写进更新日志。

### 2-06 · P2 · LIVE 存档的体积与取状态成本没量过

- **证据**：`docs/validation/2026-10-05-T048-phase2/INTERFACE-SKETCH.md:214`（未压缩 40 段约 119 KB，窗口满 512 段 1.52 MB）；`LiveProfile.h:12`（512 段约 16.5 分钟 @ 124 BPM）；`LiveProfile.cpp:2976-2981`（每次存档把窗口里所有段重写一遍）；`GlazeController.cpp:1105-1156`（每次都重新拼 XML 再压缩，没有缓存）；`CeladonChain.cpp:82-86`、`:103-104`（换规格的 prepare 也走一趟存 + 读）；JUCE 论坛帖（REAPER 每动一下滑杆取一次状态，状态大就是白烧 CPU）。
- **为什么要紧**：播了十几分钟以后，每次取状态都要在宿主线程拼 1.5 MB 的文字、压缩、转码。在按参数改动存撤销点的宿主里，这会变成拖旋钮时界面卡顿和撤销历史膨胀。压缩后多大、一次多久，没有数据。
- **建议**：先量（S）：窗口满时压缩后字节数、`getStateInformation` 一次耗时、换规格 prepare 的往返耗时。超过约 5 ms 或约 300 KB 再做：按「已发布代数」缓存压缩好的那一块，代数没变直接复用（停播时调参零成本，播放中最多每段重建一次）。
- **怎么验证**：处理器级基准用例，喂满 512 段后计时并打印字节数；缓存做完后「代数不变时两次存档字节相同且第二次耗时 < 0.2 ms」。
- **成本** S（量）+ M（缓存）· **把握** 中 · **风险**：缓存要和 2-01 的快照机制一起设计，别做两套。

### 2-07 · P2 · T-087「冻结」的目标里补一条：备选版本用同一套决定

- **证据**：`Source/dsp/GlazeController.h:111-112`（「先播一遍再导出才从头到位，重复导出不保证逐位相同」）；`Source/ui/GlazeStatusText.cpp:257-260`、`:318`（冷导出只在导出当时的状态行提示，导完就没了）；TASKS T-087 行（目标只写了「打印可复现」）；在 vault 的 `01-框架`、`03-记忆`、`05-版本` 与 T-093 报告里搜「伴奏 / instrumental / 专辑」无相关条目；公开资料：多家母带工作室要求备选版本（伴奏、无人声、干净版）与主版本用同一套设置（搜索摘要）。
- **为什么要紧**：实时自适应的处理器给伴奏版算出来的方案和带人声的主版本不一样（少了人声，频谱和响度都变），两个文件放在一起就对不上。这是「冻结」最实际的用途，比「两次导出逐位相同」更容易被用户撞到。这一条是我对用法的推断，不是代码事实。
- **建议**：T-087 拍板时一并定：冻结住的是哪些量（已施加增量、G_trim、风格权重、Balance 目标、Clay 自动值；三个守门照常工作）；冻结存不存进工程（要给备选版本用就得存）；界面上是一个「锁定当前方案」的小开关。另外不管 T-087 做不做，冷导出提示应该留到下次播放（现在导完就消失，用户看不到）。
- **怎么验证**：冻结开着时，换一段素材，`getGlazeEffectiveParamsForUi()` 的 7 项逐位不变；同一素材冻结后离线渲两遍，输出逐位相同。
- **成本** M · **把握** 中 · **风险**：要改写 ADR-030 决定 6，属于 Max 的决定。

### 2-08 · P2 · 计量：两项现成读数没露出来

- **证据**：`Source/dsp/PsrGuard.cpp:119-126`（`getLastPsrDb`，出口 PSR 每 100 ms 都在算）；`Source/dsp/ContrastGuard.cpp:114-123`（入口 / 出口的段落起伏与损失）；`Source/ui/Localisation.cpp:76`（界面上动态类读数只有 PLR）；`Source` 里搜 LRA / momentary 无界面读数；`site/src/content/docs/zh/meters.md`「响度小窗」表；公开资料：Pro-L 2 有 M / S / I 与 LRA，smart:limit 有响度范围与峰响比。
- **为什么要紧**：状态行说「受限」时，用户看不到是哪个量撞线了。PSR 与落差损失正是两个守门的判据，露出来就是最便宜的可解释性。
- **建议**：并进 T-086：响度小窗加「PSR」与「落差损失」两行（或做成状态行悬停的第二行），数值直接取守门的读数，不新增 DSP。瞬时 M 与 LRA 排在后面，可做可不做。
- **怎么验证**：`GlazeStatusText` 的无头用例加两项读数的文字与单位；截图工具加一张状态。
- **成本** S · **把握** 高（事实）/ 中（价值）· **风险**：界面多两行字，要和 ③④ 线的面板结论对一下。

### 2-09 · P2 · 实时 GLAZE 之后没有处理器层的高采样率小块 CPU 数

- **证据**：`docs/validation/2026-10-11-T097-guards/REPORT.md:118-129`（只测了链层 48 kHz / 32；「处理器层没测」）；`docs/validation/2026-09-28-T021-perf-acceptance/REPORT.md:46`（实时 GLAZE 之前的数，压缩前读过）；vault TASKS:220（96 kHz / 32 单块 474 % 尖峰未归因）；`LiveProfile.h:231-232`（分片预算是估计值）；`LiveProfile.cpp:1125-1129`（欠片时一次补完）。
- **为什么要紧**：192 kHz / 32 一块只有 167 µs。设计上分片够用，但「补完欠的片」那条路在实时小块下会不会被走到、走到要多久，没有实测。
- **建议**：T-096 (f) 合入后加两格：处理器层 96 kHz / 32 与 192 kHz / 32，GLAZE 100，报最坏单块占块预算的百分比，并把 474 % 那个尖峰归因（调度抢占还是真超时）。
- **怎么验证**：`scripts/perf-dgx.sh … --sr 192000 --bs 32 --layer processor`（参数名以 T-096 (f) 分支为准），基准机独占。
- **成本** S · **把握** 中 · **风险**：无。

### 2-10 · P3 · Lookahead 可自动化：曲中换前瞻 = 限幅器重置 + 延迟重报

- **证据**：`Source/Parameters.cpp:102-110`（注释：每换一档都要重置限幅器并重新上报延迟；没有标不可自动化）；`Source/PluginProcessor.cpp:543-578`（停稳 0.3 s 才换）、`:580-588`（延迟由消息线程定时器报）；先例 `Parameters.cpp:221-226`（曲风、交付已标不可自动化）。
- **为什么要紧**：曲中自动化 Lookahead 会让输出在换档那一刻整体挪几毫秒并断一下；离线导出时消息线程若被宿主占着，新延迟还报不上去。正常人不会这么用，但宿主的「随机化参数」「全部参数录自动化」会碰到。
- **建议**：标成不可自动化（和曲风、交付一样）；或至少在帮助气泡里写一句。
- **怎么验证**：`Parameters` 的无头用例断言三者 `isAutomatable() == false`；旧工程夹具读入后值不变。
- **成本** S · **把握** 中 · **风险**：旧工程里已有的 Lookahead 自动化曲线会失效，要 Max 点头（待拍板 4）。

### 2-11 · P3 · 宿主右键菜单与读屏标题缺位

- **证据**：`Source/ui`、`Source/PluginEditor.cpp` 里搜 `getHostContext`、`getContextMenuForParameter`、`AccessibilityHandler`、`setTitle`、`keyPressed`、`setWantsKeyboardFocus` 全无匹配；唯一的键盘输入是 Alt 点击后的数值框（`PluginEditor.cpp:587`）；控件全是自绘 + `ParameterAttachment`（`Source/ui/Controls.cpp:225-424`，压缩前读过）；公开资料：JUCE 自带读屏支持，但自绘控件要自己提供标题与数值。
- **为什么要紧**：VST3 宿主用户习惯在旋钮上右键调出「显示自动化 / 学习控制器」；读屏用户现在读不到任何控件名。两件都不大，但都是「细节够不够完美」里专业用户会注意到的。
- **建议**：① 旋钮与开关右键时调宿主的参数菜单（有就弹，没有就算了）；② 10 颗旋钮与各开关给标题和数值接口。不做键盘焦点导航。
- **怎么验证**：截图工具的 `--only check` 加一条「每个带参数的控件都有非空标题」；右键菜单只能在真宿主里手测（Cubase / REAPER）。
- **成本** S（右键）+ M（读屏）· **把握** 中 · **风险**：右键现在如果另有用途要先对一下（归 ④ 线）。

### 2-12 · P3 · 宿主旁通时整体响度照旧积分，等 Max 一句话

- **证据**：`Source/dsp/CeladonChain.h:86`（`pauseIntegrationOnHostBypass = false`）、`:42`（「等 Max 确认口径」）；`CeladonChain.cpp:373-374`（界面上听原声会暂停积分，宿主旁通不会）。
- **为什么要紧**：用宿主的旁通键做 A/B 的人，整体响度读数会被干信号拉偏，和界面 A/B 的行为不一致。
- **建议**：改成 `true`（两种旁通同一口径）。一行常量，但会改表的读数，属于 Max 的决定。
- **怎么验证**：链级用例「宿主旁通 10 s 前后整体响度读数不变」。
- **成本** S · **把握** 高 · **风险**：有人就是想量干信号的响度（可以用 IN 表）。

### 2-13 · P3 · T-088 延迟：排最后，做成模式而不是参数

- **证据**：`Source/dsp/CeladonChain.cpp:696-703`（48 kHz / 2 ms：282 + 1102 = 1384 样本）；`Source/dsp/CeladonChain.h:26-27`（Clay 固定延迟，与参数、旁通无关）；T-093 报告（自动用法下收单 0/9 首生效，Clay 占整链延迟约 80 %）；公开资料：Pro-L 2 也没有零延迟模式。
- **为什么要紧**：28.8 ms 在母带总线上有宿主延迟补偿，不影响混音回放；只在带着插件录音、弹软音源时碍事。同类产品也都有十毫秒量级的延迟。所以它排在监听与冻结之后。
- **建议**：要做就做成一个不可自动化的「低延迟」模式开关（切一次重报一次延迟），不要让延迟跟着收单开关动；先在 T-096 的指标管线上量最小相位收单的代价。
- **怎么验证**：模式切换前后 `getLatencySamples()` 与实测冲激延迟一致；高质量档与现状逐位相同。
- **成本** L · **把握** 中 · **风险**：改 Clay 的相位特性，要过 ADR-034 的门。

---

## G. 三张清单

### 我替你定了什么

1. 「竞争存在」按代码与 JUCE 包装层源码判为确认；崩溃后果是推演，没有实跑复现（本线不能跑测试）。
2. 优先级口径：会无声改坏成品或可能崩溃 = P0；破坏「听到的 = 导出的」或缺自动校验 = P1；其余 P2 / P3。
3. 范围：ADR-033 没实现、守护族、算法听感归 ① 线；面板布局与像素细节归 ③④ 线。本线只在盘点表里提到，不出条目。
4. 竞品对照只用公开的官方帮助页、官方博客、开源仓库、论坛；不比价格与口碑。
5. `TASKS.md` 与 `INDEX.md` 只 grep，没通读。
6. T-098 第 8 条给了明确倾向（选 2），因为找到了 T-098 没写到的后果。
7. 验证命令是建议稿；新测试目标名 `celadon_processor_tests` 是本线起的。
8. pluginval 的定位：发版检查的一项，不当作 T-099 的验证手段。

### 待用户拍板

1. 宿主在「已经在跑」的实例上读档（换宿主预设、宿主撤销）时，实时画像要不要跟着换回去。建议：参数照换，画像保留当前的；只有「还没开始处理」时才恢复画像。
2. T-098 第 8 条：本线建议选 2（播放中切）。
3. T-087 冻结要不要把「备选版本用同一套决定」算进目标（决定冻结存不存进工程）。
4. Lookahead 改成不可自动化（旧工程里已有的自动化曲线会失效）。
5. 宿主旁通时整体响度暂停积分（`pauseIntegrationOnHostBypass` 改 `true`）。
6. T-095 合入后怎么发版：main 上已经有还没发的 T-097（`a768b8a`，会改 GLAZE > 0 的声音；0.1.8.10 是从 `f01696f` 打的包）。要么两个一起发，要么从 0.1.8.10 的发版点单独拉一个只含 T-095 的补丁。

### 没核实的

1. T-099 的崩溃后果没有实跑复现；下标越界那一步是读代码推出来的。
2. LIVE 压缩后的实际字节数、`getStateInformation` 一次耗时、换规格 prepare 的往返耗时。
3. 各宿主调存读档的线程与频率：只有 JUCE 论坛帖佐证 REAPER；Live、Logic、Cubase、FL、Studio One 没查到一手说明。
4. 各宿主离线导出是否都报离线；VST3 宿主导出前是否都会重新 `prepareToPlay`（AU 包装层那条路在 JUCE 源码里核对过）。
5. 处理器层 192 kHz / 32 的 CPU；96 kHz / 32 的 474 % 尖峰。
6. `MusicAnalyzer.cpp`、`LiveProfile.cpp` 的音频路径只做了 grep 与重点段落，没逐行通读。
7. 44.1 kHz 以下、192 kHz 以上采样率的行为（测试覆盖 32 / 44.1 / 48 / 96 / 192 kHz）。
8. 两种主段模型在真实曲子上的听感差多少。
9. Windows、Intel Mac 真机与各宿主实测：没有数据。
10. Pro-L 2 的 Unity Gain / Audition Limiting 具体行为、Ozone 的各项、smart:limit 的 constant gain / delta：只核到功能名，没逐字核行为。
11. T-095 分支没编译没跑（分支自述），本线也没法跑。
12. pluginval 别的测试文件里有没有「超过 prepare 块长」「实时线程分配」用例：只看了 `BasicTests.cpp`。
13. 频谱仪显示开关存在哪。

---

## H. 公开资料

- FabFilter Pro-L 2 帮助 · 总览：https://www.fabfilter.com/help/pro-l/using/overview
- FabFilter Pro-L 2 帮助 · 响度计量：https://www.fabfilter.com/help/pro-l/using/loudnessmetering
- FabFilter Pro-L 2 帮助 · 过采样：https://www.fabfilter.com/help/pro-l/using/oversampling
- sonible smart:limit 主要功能（官方博客）：https://www.sonible.com/blog/smartlimit-main-features/
- iZotope Ozone 功能页：https://www.izotope.com/en/products/ozone/features
- pluginval 仓库说明：https://github.com/Tracktion/pluginval
- pluginval 基本用例源码：https://github.com/Tracktion/pluginval/blob/master/Source/tests/BasicTests.cpp
- JUCE 论坛 · 拖滑杆时宿主反复取状态：https://forum.juce.com/t/how-to-prevent-getstateinformation-being-called-on-slider-movement/57791
- JUCE 论坛 · processBlock 与 setStateInformation 同时被调的竞争报告：https://forum.juce.com/t/data-race-in-juce-compressor/47897
- JUCE 论坛 · 存读档在哪个线程：https://forum.juce.com/t/audioprocessor-get-setstateinformation-thread/13465
- JUCE 论坛 · 后台线程读档的做法：https://forum.juce.com/t/best-practices-for-handling-setstateinformation-on-background-thread/62842
- JUCE 论坛 · 无障碍支持：https://forum.juce.com/t/juce-accessibility-on-develop/45142
- 母带交付的备选版本（工作室公开说明）：https://studios301.com/faq/mastering 、https://www.audioanimals.co.uk/news/faq/stereo-audio-deliverables-explained
