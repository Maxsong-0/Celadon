# T-048 阶段 1 收口（本分支）

Max 2026-10-04 拍板的 ①～④ 落在这条分支上。阶段 2（编排器持续重算、取消听写锁）没有开始。实时路径默认关闭，插件出声不走 LiveProfile。

## 合入 main

`origin/main` 当时的尖端是 `75a97ba`（PR #52，版本 0.1.8.8）。用 merge 合进 `cursor/t048-phase1-closeout-c841`，没有冲突。自动合并过的文件包括 `Celadon.jucer`、`CeladonChain.h`、`GlazeConstants.h`、`GlazeController.cpp` / `.h`、`tools/render/README.md`、`tools/render/RenderMain.cpp`。实时入口（`setLiveSegmentSink`、`--live-trace`、`LiveConstants`）还在；站点与插件版本跟 main 的 0.1.8.8。

## ① 规格口径（库内镜像）

设计规格两份在库外（Obsidian）。库内这次对齐的是注释、渲染说明和 `calibrate_house.py`：

- 第 7 节第 3 条的验收改成跟踪误差 \|I\_{d+3} − D4\| ≤ 1 LU。D4 是第一个 drop 起 4 个已听 drop 小节的输入段响度能量平均。旧口径 \|I\_{d+3} − I_end\| 与「歌自己变了」\|D4 − I_end\| 仍写出来对照。跟不住的约 10 首是规格第 5 节的已知限制，脚本不放宽门槛。
- M2 本来就是「第 16 小节起、逐首中位再汇总」，没有改算法。
- M0 的 4/4 型说明改成 6 小节（见 ②）。

## ② 常数 A

`LiveModelConstants::confidenceBars` 与 `LiveConstants::fourOnFloor` 的出厂值 8 → 6。只动 c。响度型 / 被动型的 `confidenceBars` 仍是 0（用秒）。参考分位、门宽等没有动。

## ③ 安静门断言

三道门「一直放开」的时刻收到 (18 s, 60 s]。另加「之前响 300 s、再轻 28 dB」的场景（块长 512）。门的时间常数没改。

## ④ 四条边界

- 段落账在同一段重新锚定时，这一次实例最长两小节（拍点外是两段）。超了就截断、另开新段。正常走带一段一小节，碰不到。
- 实时入口用读档同一套 `entryWithinBounds`。越界整段当坏段；段尾本身越界就收到起点，免得后面的发布时刻被顶走。发布端兜底还在。
- `positive` 时间常数 ≥ 1 ms（`minimumLiveTimeConstantSeconds`）。+∞ 仍合法。`--live-constants` 与 `sanitiseLiveConstants` 共用这张表。
- `LiveProfile::prepare` 只收 8 kHz～768 kHz 的有限采样率，之外按 48 kHz。

B2 的半样本锚定留在共用的 `SegmentLedger::anchor` 里（House 与实时是同一类的两个实例）。从第 N 小节起播的 22 首重定基线不在这次。

## 这次没做（Spark / DGX）

- ⑤ DGX aarch64 上重跑因果重放，按跟踪误差出报告。M0 改成 6 之后，live-trace 的摘要会变，要重测。
- ⑥ 曲目拷到 DGX，对 main 逐位。从第 N 小节起播的那组重定基线。库注：Ansun - Faultless、KINOSEI - DREAMER 原件已不在 Mac，DGX 上跑 20 首。
- ⑦ DGX 收尾全套 0 警告。
- x86 / v3 / MSVC 与 x86 分片耗时等 107 / 109。补完这些才关阶段 1。
