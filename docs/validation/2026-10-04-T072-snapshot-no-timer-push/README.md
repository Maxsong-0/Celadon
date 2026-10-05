# T-072 · 截图模式下编辑器定时器的看门狗不再按墙钟补推频谱仪（T-070 评审 F9，2026-10-04 夜，DGX Spark 本机）

夜间评审（`wf_3e784f42-2fa`）对 T-058 截图检查的第 9 条发现（F9，info / plausible，评审自己标「范围外」）：`advanceForSnapshot` 的注释说「慢机、Debug 下也不多推（评审 R-06）」，
但 `lastVBlankMs` 是在 `advanceSpectrum` **之前**取的，那一帧耗时达到 100 ms 时 `timerCallback` 里的看门狗会再按墙钟推一次频谱仪。T-070 里我先按「这里没有慢机、复现不了」记成待认领；
后来想到可以**注入停顿**来复现，就做了——问题属实，已修。

## 结论

- **F9 属实**：给 `advanceSpectrum` 注入停顿（每 40 次推里有一次耗时 130 ms，超过看门狗的 100 ms 门）后，`--deterministic` 的 30 张出图里**改前 24 张变了**（22 张全尺寸图的频谱仪面板全部变，
  6 百 ～ 4.5 万个像素不同，通道差最大 242：OUT 填充与曲线被多推的一步拉低、悬停读数也变，见 `stall-experiment/zh-default-spectrum-R0-R1-R3.png`）。
- **改后 30 / 30 张逐字节相同**（只遮掉两条电平表的列，见下）；不注入停顿时改前改后 30 / 30 张逐字节相同（正常行为不变）。
- 改动只有 `Source/PluginEditor.{h,cpp}` 三处（一个 `bool` 成员、`advanceForSnapshot` 里打标志、看门狗条件加 `! snapshotDriven`）。`advanceForSnapshot` 只有截图工具调用，**插件 / 宿主里的行为不变**。
- 新检查 `probeTimerPush`（`--only check` 111 → **112**）：6 kHz 正弦 0.5 s（每帧 `advanceForSnapshot`）后让消息循环跑 300 ms，频谱仪路径重建次数不得增加。**红绿**：改后 112 / 0；
  同一份工具换回 main 的 `PluginEditor`：111 / 1，败的恰是这一条（300 ms 里重建 2 次）。

## 原因与改法

编辑器平时由垂直同步推频谱仪；垂直同步 100 ms 没来（离屏、宿主 / 系统不给）就由 30 Hz 的 `timerCallback` 里的看门狗按墙钟接着推（T-036）。截图工具离屏、用 `advanceForSnapshot (seconds)` 自己按给定时长推，
所以原来写成：先把 `lastVBlankMs` 记成现在（让看门狗以为垂直同步还活着）→ `advanceSpectrum` → `timerCallback()`。但 `timerCallback` 里的 `now` 是**之后**才取的：
`advanceSpectrum` 本身（或 `timerCallback` 前半段、或机器被占满时线程被切走）耗时 ≥ 100 ms，`now - lastVBlankMs >= 100` 就成立，看门狗按墙钟再推一次，OUT / 峰值保持多走一步，
`--deterministic` 的出图随机器快慢变。

改成一个标志：`advanceForSnapshot` 打上 `snapshotDriven`，看门狗条件加 `! snapshotDriven &&`——截图工具每一帧都自己给时长，定时器不再补推，**与这一帧、定时器各自耗时多久无关**。
（评审给的另一种改法是把 `lastVBlankMs` 挪到 `advanceSpectrum` 之后再取；那只把窗口缩到 `timerCallback` 前半段，机器被占满时线程被切走 100 ms 照样会漏，所以用标志。）

## 验证

### 注入停顿的实验（`run_t072.sh`、`inject_stall.py`；输出在 `stall-experiment/`）

临时副本里（**不进仓库源码**）给 `advanceSpectrum` 注入停顿：环境变量 `CELADON_T072_STALL_MS=130`、`CELADON_T072_STALL_EVERY=40`（每 40 次推频谱仪里有一次睡 130 ms；每帧都睡要好几个小时——
`--deterministic` 每张图要推约 900 帧，第一版每帧都睡的实验跑了 2 分钟还一张图没出，停掉改成了这个）。三份构建（改前 main `43d788b` + 注入、改后 + 注入、改后原样）各 0 警告，
全机锁里各跑 `--deterministic` 出 30 张图再逐字节比（`stall-experiment/runs.out`）：

| 比较 | 结果 |
|---|---|
| R6 改后原样 vs R5 改前原样 | **30 / 30 相同**（正常行为不变） |
| R0 改前 + 注入不设停顿 vs R5 | 30 / 30 相同（注入本身无副作用） |
| R2 改后 + 注入不设停顿 vs R0 | 30 / 30 相同 |
| **R1 改前 + 停顿 vs R0** | **6 相同 / 24 不同**；`regiondiff-R1-vs-R0.txt`：22 张全尺寸图的**频谱仪面板 22 / 22 全变**（`maskdiff-R1-vs-R0.txt`：遮掉电平表列后仍 24 张不同） |
| **R3 改后 + 停顿 vs R0** | 整张比 10 相同 / 20 不同，但**不同处全在两条电平表的列**（`diffbox-R3-vs-R0.txt`：x 32 ～ 39 与 x 1137 ～ 1170，正是 `inputMeter`（x = 16）与 `outputMeter`（x = 1124）；电平表按墙钟下落，`--deterministic` 的说明里写明的例外）；**频谱仪面板 0 / 22 变、遮掉两条电平表的列后 30 / 30 张逐字节相同**（`regiondiff-R3-vs-R0.txt`、`maskdiff-R3-vs-R0.txt`） |

`stall-experiment/zh-default-spectrum-R0-R1-R3.png`：`zh-default` 的频谱仪面板，上 = 改前不停顿（参照）、中 = 改前 + 停顿（曲线被多推一步拉低，悬停读数 −39.46 → −39.19 dB）、下 = 改后 + 停顿（与参照相同）。

### 新检查的红绿（`run_t072_check.sh`；输出在 `check-red-green/`）

| 编辑器 | `--only check` |
|---|---|
| 改后（`snapshotDriven`） | **112 过 / 0 败**（`check-new.txt`）——原有 111 条没有一条依赖看门狗补推 |
| 同一份截图工具 + main 的 `PluginEditor.{h,cpp}`（= 改前） | **111 过 / 1 败**（`check-old.txt`）：`T-072 spectrum: an editor driven by advanceForSnapshot is not advanced by its own timer (… 2 path rebuilds in 300 ms of message loop after the last advanceForSnapshot, needs 0)` |

### 其余

- 编译：三份实验构建 + 两份检查构建各 0 警告。全套测试与最终集成见 TASKS 与 [[2026-10-04]] 日志末节（合入后在 main 上统一跑一遍）。
- 红绿表（T-070 的 27 变体）没重跑：本任务没碰 `SpectrumView`，新检查只加一条；原有 111 条在改后全过。

## 没做 / 限制

- **电平表仍按墙钟下落**：慢帧会让 `--deterministic` 出图里两条电平表的列变（上表 R3），这是 `--deterministic` 文档早写明的例外，不在 F9 里，没动。
- 垂直同步那条路（`spectrumFrame`）不受标志约束：离屏编辑器没有对等体、没有垂直同步回调，截图工具里不会走到；真窗口里 `advanceForSnapshot` 不会被调。
- 非确定性模式（不带 `--deterministic`）：`dispatchEvents` 期间定时器照常触发，但看门狗不再补推频谱仪，频谱仪只由 `advanceForSnapshot` 推——这是预期（原来 `dispatchEvents` 够长时，定时器落在哪一帧就多推哪一步）。
- Mac / Windows 没实跑；`probeTimerPush` 用 `dispatchEvents (300)`（Mac 是 CFRunLoop），`scripts/windows-wine.sh` 跑的 `--only check` 也会带上它。
- 注入停顿只在临时副本里做，仓库里没有任何注入代码。
