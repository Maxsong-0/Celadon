# T-068 · 夜间评审（措辞、渲染工具说明两份）的回修（2026-10-04，DGX Spark 本机）

夜间任务：对今晚合入 main 的 T-058 / T-059 / T-061 / T-062 / T-065 / T-066 做只读对抗评审（workflow `wf_3e784f42-2fa`，5 个代理，Opus 5.5 与 Sonnet 5.5 · xhigh）。
本目录处理其中两份——**T-061 / T-066 措辞**（8 条发现）与 **T-059 / T-065 渲染工具说明**（12 条发现）。另外两份拆成独立任务：T-062 官网图 → T-069，T-058 截图检查 → T-070。
每条发现都先对着代码 / 实测数据核实，再改；**只改文案、注释与 `celadon_render` 写进 metrics.md 的口径句 / n/a 备注**，判定逻辑、数值与声音不变。

## 措辞评审（8 条）

| # | 严重度 | 核实 | 处理 |
|---|---|---|---|
| F1 | medium | ✔ 手动低切 / 收单被峰值守护回收的量远不止「让开一点」：`V4-clay.md` 实测收单 120 Hz 在中等宽低音上 78 % 样本被回收（标称 −22.6 → 实际 −4.9 dB），低切 25 Hz 在宽密素材上 59 %（−17.4 → −8.6 dB）；`ClayModule.h` 头注释同结论；`Source` 里没有「守护回收」的指示灯，界面全绿 | **改**：官网 `docs/{zh,en}/clay.md` 手动行、`algorithms/{zh,en}/clay.md` 守护段与三态语义，「让开一点」→「会被收回，手动值本身不变；低音宽而不相关、峰值密集的歌上收回的可能不少，界面不会另行提示」。「是否把收单也拆出守护」仍是 Max 的待定项（`ClayTests` 注释），本次只让文字说真话 |
| F2 | low | ✔ 宽度保护（`ClayGuard`，盯相关度，真改声音、点亮加宽琥珀灯）首句没提 | **改**：官网首句与插件灯条目（`HelpText.cpp`）「加宽不经峰值守护，自动加宽由先试后用把关，运行中还有宽度保护盯着相关度」（英文同）；`HelpTextTests` 原来钉着「加宽不经守护」「width is not guarded」，同步改并新增一条钉宽度保护 |
| F3 | low | ✔ `clayPhaseGuardActive = active && 收单自动 && phaseActive`，`active = GLAZE > 0` | **改** `glaze.md`（中英）：「（GLAZE 为 0 时连提醒也不亮）」。同根扫描：`clay.md` 自动行已写「GLAZE 为 0 时不处理」、FAQ / quickstart / 控件行 / 算法页表格的主题不是 GLAZE 0，页面上方已交代，**不逐处加** |
| F4 | low | ✔ 首页「只动 S：单声道回放完全不变」读起来像整个 CLAY；低切会改 L / R 里的次声 | **改** `home.json`：「收单与加宽只动 S：单声道回放不变（低切只删次声）」（英文同）；中文 lead 用分号拆开「自动先试」与「低切 / 收单会抬峰值就让开」 |
| F5 | info | ✔ 首页 CLAY 示意图的竖线位置不按 10 Hz ～ 20 kHz 对数轴 | **不改**：整张图下方已有图注「示意图：处理前 → 处理后」（`process.caption`），本来就不是按比例的图 |
| F6 | info | ✔ 三处注释过期（`HelpText.cpp` 419 / 478、`HelpText.h` 63-64） | **改**：419 写成「低切 / 加宽显示此刻实际值（为 0 显示「未启用」），收单一行恒显示「未启用」」；478 去掉「同源底稿」（没有任何机制保证同步）；`HelpText.h` 灯色改成实际的灰圈 / 紫圈 / 青色实心 / 橙色实心与真有的 `palette` 名 |
| F7 | info | ✔ 低切频率行建议列比插件气泡少末句，页脚却写「一致」 | **改**：两版说明页补末句「让得太频繁会削弱低音，所以自动会把频率压低或不切」；页脚不动 |
| F8 | low | ✔ 英文 `Auto does not mono` 把 mono 当动词 | **改**：「Auto never folds the lows to mono, so no mono frequency is shown here」，插件 + `en/clay.md` + `HelpTextTests` 断言三处同改（测试名不动，免得日志里按名对照的记录对不上） |

## 渲染工具说明评审（12 条）

| # | 严重度 | 处理 |
|---|---|---|
| F1 | low | **改** README `guard_seconds`：为 0 的情形补「GLAZE 0 的组、方案源不支持自动 Clay」两种——这两种下 ClayGuard 不是停住而是回到中性（`GlazeController.cpp` `!active` 分支） |
| F2 | low | **改** `RenderMain.cpp` 重放注释：n/a 的两种原因都写全（收单比例 0 < r < 1 已因 ADR-015 不再出现；目标频谱「参考」形状本工具走不到） |
| F3 | info | **改** n/a 备注（运行时字符串）：按 `clayExpressibleAsManual` 的三个子条件写「收单比例不是 0 或 1，或低切 / 加宽越界」。眼下走不到（ADR-015、sanitize 钳范围），所以不影响任何现有输出 |
| F4 | low | **改** README 与 metrics.md 口径句：`phase_guard_pct` 只在收单为「自动」的组计（`auto`、`mono` 组），`off` / `lowcut` / `width` 组与重放行恒为 0；`width_guard_pct` 只在加宽「自动」的组计 |
| F5 | info | **改** README 与口径句：本工具不用手动收单，ADR-015 起收单 Hz 恒为 80、收单量恒为 0 |
| F6 | info | **改** README：`trial_mono*` 四列 ADR-015 起照测照存、决策不用，仅作诊断 |
| F7 | low | **改** README 第 78 行：`__listen.csv` 只有 `t_sec`、`limit_gain_db` 和校准列，修坯列只在主追踪里 |
| F8 | info | **改** README 修坯列一节补两句：低切不经 ClayGuard 所以没有 `live_lowcut_hz`；`guard_phase_hold_s` 在条件成立期间保持整段（出厂 2 s）；峰值守护 / 压力反馈的列在哪里看 |
| F9 | info | **改** README 第 14 行：低切由决策（含代价封顶与压力反馈缩放）给值，加宽由决策 + 宽度保护给值（`ClayGuard.h`：lowCutHz 原样透传，已核） |
| F10 | info | **改** README 第 9 行：选歌要求从「低音很宽 / 有反相内容」改成「至少一首低频有反相段（核对提示误亮与门槛里的豁免）；最好再有一首低音很宽的（看低切与加宽）」 |
| F11 | info | **改** `GlazeController.h` 209 / 217 两条注释：`clayPlanned` 含压力反馈缩放；`liveLowCorrelation` 读的是处理完这一块之后的此刻读数（ClayGuard 下一块才用它） |
| F12 | info | **改**：metrics.md 口径句里的「0.35 s」「−180 dBFS」与自检失败消息改由 `ringOutSeconds` / `replayTolerance` 生成（输出文字不变，常量改了说明不会漂）；`gate_check.py` 加同值常量 `REPLAY_TOLERANCE_DBFS` 并写明与 C++ 常数互指（Python 读不了 C++，这一处仍靠注释对齐）。`--help`、文件头、README 里手写的「−180」不生成（那些是自由文字，改成拼接会让排版难维护） |

## 验证

- **站点**：改前（main `fabd9a2`）/ 改后各自 `pnpm install --frozen-lockfile` + `pnpm run build`（`check-i18n` + `tsc --noEmit` + `vite build` + 站点地图 + 中文外壳）都过；产物 JS 逐句对比（`dist_sentence_diff.py`，输出 `dist-diff.txt`）：只在改前 **15** 行、只在改后 **15** 行，逐一对得上本次改的 15 处（算法页 4 + `glaze.md` 2 + 说明页整页 2 + 首页文案 2 + 说明页表格行 5）。
- **插件测试**（`celadon_tests`，0 警告）：`--only HelpText,HelpTextClay,HelpTextHouse,Localisation` 改后 **5266 过 / 0 败**（`targeted-tests-after.txt`，比 T-066 的 5265 多的 1 条就是新增的宽度保护断言）。
- **阳性对照**：main 的全部源码 + 本次的新测试文件（旧文案）编译后跑同一批：**恰好 4 条败**——灯条目「加宽不经峰值守护」、英文同范围、「运行中还有宽度保护」、收单频率英文句；其余 5262 过（`targeted-tests-old-control.txt`）。
- **只改注释的证明**：`HelpText.h`、`GlazeController.h` 去注释后逐 token **相同**（`g++ -fpreprocessed`，复用 T-060 的 `comment_only_check.py`）；`HelpText.cpp`、`RenderMain.cpp`、`HelpTextTests.cpp` 去注释后的差异**恰好是上表的字符串 / 常量生成改动、别无其他**（`token_diff.py` 全文 `token-diff.txt`）。
- **渲染工具**（`celadon_render`，0 警告）：改前（main）/ 改后各渲染 Jamendo 9 首（通用组 `--glaze 0,35,70 --clay auto,off`）+ 3 首 House：`metrics.csv` **12 份逐字节相同、0 份不同**（78 个 render 行，`audio_hash` 全在里面）；`metrics.md` 12 份都**只差那两条口径句**（通用组第 42 / 43 行、House 组第 33 / 34 行，每份 4 个差异行），「含 0.35 s 余振」「多冲 0.35 s 余振」「≤ −180 dBFS」由常量生成后文字不变；`--help` 逐字节相同；`--selftest` 全过（`render-selftest.txt`）。
  `gate_check.py`：真实指标表 FAILS 0；人造一行 `replay_check = mismatch` 仍报「允许 ≤ −180」。

## 没做 / 决定不改

- **F5（示意图不按比例）**：整张图已标「示意图」。
- **是否把收单也拆出峰值守护**（V4 R4-03 / 报告第 4 节）与「回收指示灯」：产品决定，仍待 Max；本次只把文字改成不低估。
- **`--help`、文件头、README 里手写的「−180 dBFS」**：不生成（见 F12）。
- **`trial_mono*` 四列是否退役**：画像格式的一部分，ADR-015 决定「画像照存」，不动。
