# T-022 · 修坯门槛改为同决策重放对照（ADR-016）实现证据（2026-09-28）

档案：`项目/Celadon/03-记忆/ADR-016-修坯门槛改为同决策重放对照.md`、日志 `02-日志/2026-09-28.md`。
改前备份：`Celadon-releases/source-baseline-2026-09-28-T-022-before/`（`tools/render/RenderMain.cpp`、`README.md`、旧门槛脚本，含 SHA256SUMS）。

## 改了什么

- `tools/render/RenderMain.cpp`：
  - 第二遍渲染可逐块记下 `getEffectiveParams()`；新 `replayPass`：GLAZE 0 的链逐块照搬，修坯 `off`（重放全关）或 `copy`（「手动」照填，阳性对照）；
  - 每组 GLAZE > 0 的「自动」之后多一行 / 一个成品 `clay-replay-off`，「自动」行记 `replay_check` 与 `replay_max_diff_dbfs`；
  - `Measurement` 加有效音乐时长（K 加权 100 ms 子块、−70 LUFS 门，按 10 s 一段读子块），CSV 列 `music_seconds`，metrics.md 表头也写；
  - `--no-replay` 关掉重放；成品遍 / 重放遍共用 `PassStats`、`prepareIo`，量 / 对齐 / 写共用 `measureAlignWrite`；
  - 自检新增「同决策重放复现自动链」「重放全关的延迟与长度」两项；文件头 `--selftest` 那句的旧注释「282 + Clay 的 D」改成 D + G（T-017 余项之一）。
- `tools/render/gate_check.py`（新）：ADR-016 口径的门槛检查，见 README。
- `tools/render/README.md`：网格、选项、输出、新列、自检、「同决策重放（ADR-016）与门槛检查」一节、盲听建议。

## 验证

- 109（GCC 15.2，Release）编 `celadon_render` **0 警告**；`--selftest` 全部通过（`selftest.log`）。重放对照在合成信号上**逐位相同**：
  1911 块里 GLAZE 每块都改过 Limit Gain、自动低切每块都开着——GLAZE 0 实际参数 = 喂进去的值，不切片也不改变输出。
- 真歌试跑（WTF、l_kskdhjf、ooaa，GLAZE 0 / 70 × 自动 / 全关；`trial-*`）。**注意：当时工作树里有 T-018（去掉自动收单）与 T-020（分摊预热）
  的未完成改动，这组数字只验证工具，不是门槛结论**：
  - 阳性对照 3 首都 ok：WTF 逐位相同，l_kskdhjf / ooaa 最大差 −604 / −618 dBFS，只出现在歌尾余振（第一个不同样本在 202.16 s / 45.35 s）；
    临时诊断（已删）确认重放链自己把每块切两段处理也差同样的量——整条链在 −600 dBFS 量级随切块方式不同而不同，与重放无关；
  - WTF g70：「自动 vs 重放全关」入口峰值差 **+0.000 dB**、响度 +0.000 LU；跨链参考差 +0.747 dB / +0.348 LU——混淆因素一目了然；
  - ooaa 有效音乐 18.6 s，锁不定被列「时长不够」、不算失败；`trial-gate-check.txt`：0 失败。

## 22 首门槛全量（08:00～08:06，109；指标表 `artifacts/render-gate4-2026-09-28-T022/snapA|snapB/`）

T-018 / T-020 的源码当时都已「改完」（两条线在改测试 / 做验证），为免跑到一半被存成半截，用两份独立快照：

| 快照 | 源码 | 用途 |
|---|---|---|
| B | 当前工作树，把 T-018 改过的 GlazePlanner / ClayGuard（取 T-018 改前备份）、T-020 改过的 ClayModule / LinearPhaseLowpass（取 T-019 改前备份）、GlazeTypes.h（取 T-015 改前备份；差异只是两行注释）换回改前 = v0.1.7.0 + T-015 + T-022 | 与 09-27 门槛（出 3 项跨链误报）同一份代码，逐行对照 |
| A | 07:56 的工作树原样（含 T-018 删自动收单、T-020 分摊预热的源码），`snapA-source-sha256.txt` | 最接近最终版 |

- **快照 B**：154 行非重放成品与 09-27 `render-gate3` **逐位相同**（同一份代码确认无误）。新口径下 09-27 的 3 项跨链误报全部消失：
  WTF g70 入口峰值差 +0.000（跨链 +0.747）、l_kskdhjf g70 −0.027（跨链 +0.444）。**新口径抓到 1 项真问题**：l_kskdhjf g70 同决策下
  修坯让响度降 0.145 LU、入口峰值没跟着降——自动收单 0.95 把宽低音的侧声道能量收掉了，正是 T-018 要删的功能。
  阳性对照 ok 19 组、n/a 25 组（自动收单比例在 0、1 之间，「手动」表达不了）。`gate4-snapB-check.txt`：1 失败。
- **快照 A**：**0 失败**（`gate4-snapA-check.txt`）。阳性对照 44 组全部 ok（自动收单删掉后没有 n/a）；修坯只剩低切在 emotapes / somggggg /
  woqu 上起作用，入口峰值都下降；woqu 响度 −0.15 / −0.18 LU 与入口峰值同降（切掉 16 Hz 以下的隆隆声），列说明。
  不锁定的只有有效音乐 < 30 s 的歌（emomax、fule、ooaa、sh_t、woqu g70 自动、untitled），全部列「时长不够」。
- **独立复核**：T-018 主进程 07:52 用同一份源码另跑了一遍（`artifacts/render-gate4-2026-09-28-T018/`，它当时用旧跨链脚本判出 WTF g70 +0.747 dB）。两份指标表 198 行 `audio_hash` **全部逐位相同**；用 `tools/render/gate_check.py` 判它那份也是 0 失败——旧脚本报的 +0.747 就是同一份数据里的跨链参考列。T-018 08:05 用 `snapA-source-sha256.txt` 复核 84 个文件与 v0.1.7.1 源码一致，这轮即 v0.1.7.1 的门槛结论。

## 没做的

- T-018 已声明不再改插件源码；T-020 若再改插件源码（不含测试），需在它收尾后重跑一次（同一套命令：`calib-loop.sh` 显式带 `--glaze 0,35,70 --clay auto,off`
  → 合并 metrics.csv → `tools/render/gate_check.py`）。
- 重放照搬自动链的切片（要给 CeladonChain 加测试钩子）：切片只在 −600 dBFS 量级有影响，容差已覆盖，暂不做。
