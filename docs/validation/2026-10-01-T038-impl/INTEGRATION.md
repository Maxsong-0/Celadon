# T-038 阶段 3 集成记录（2026-10-01，主进程）

> 四份代理报告：[REPORT-A.md](REPORT-A.md)（决策器与交付）、[REPORT-B.md](REPORT-B.md)（House 学习方式）、[REPORT-C.md](REPORT-C.md)（限幅跟拍 / 提示 / 界面，截图在 `screens-C/`）、[REPORT-D.md](REPORT-D.md)（工具与回归）。
> 本文记录主进程怎样合并、怎样处理每条跨文件请求和遗留问题、集成后的验收结果。

## 1 合并

- 四个代理工作树（workflow `wf_de8d9b0c-06a`）开出来时都停在 main `88199cb`，不是任务书说的阶段 1 `c50ccd3`。四个代理都自己发现了，先 `git merge --ff-only` 快进到 `c50ccd3` 再开发；主进程用 `git merge-base --is-ancestor` 逐个核过，四个分支都含阶段 1。
- 改动文件互不重叠（A 3 个、B 9 个、C 21 个加 14 张截图、D 5 个），都在各自的清单里。
- 按 A → C → D → B 合进 `claude/house-mastering-workflow-67aa29`：`ff1df29`、`c19fd5d`、`4872aff`、`51f2c84`，没有冲突。

## 2 跨文件请求与遗留问题的处理

| 来源 | 内容 | 处理 |
|---|---|---|
| A 请求 1 | 编排器运行中的 Tone 跟随、实际 Tone 钳位用的是对称的 ±toneMaxDb，House 档往亮 +0.4 在运行时守不住 | **已改**（`6b2cc1c`）：两处上界改用 `toneBrightCapDb`（方案源的曲风）；通用档返回 toneMaxDb，逐位同改前 |
| A 请求 2 | 保护层 / 上游监视用两参数 blendConstants | 不改：曲风不覆盖动态预算，两版逐位相同 |
| A 请求 3 | 分析器「先试后用」的低切候选是 20 Hz，House 低切目标是 25 Hz，代价偏乐观 | **未改，待评审 / Max 定**：运行时有 ClayGuard 峰值守护（ADR-014）兜底 |
| A 请求 4 | 注释 | **已改**（toneMaxDb、requestedTargetLufs） |
| A 遗留「需 Max 定」 | 最少推响量 3 dB 会把流媒体 / 俱乐部的目标推过该档上限 | **已改**：规格写的是「允许范围（决策器钳位）」，按硬上限处理——非标准交付下满强度请求 ≤ max(0, 档上限 − 输入)；标准档照旧。新测试 1 组 |
| A 遗留 | 暂定方案的信心折扣 | 采用 B 的做法（代表性 = min(原值, 已听 ÷ 64)）；因 confidenceScale 先除以 0.8，约 51 小节起就不再打折，与规格原文「乘 min(1, 已听 ÷ 64)」有差，规格改写成实际做法 |
| A 遗留 | styleOffsetReferenceLufs（−9）与冲击的 preferredLufs（−9）是两个常数 | 记进标定注意事项：改其一要一起改 |
| B 请求 1 | 段落账满了要在状态行写明 | **已改**：Status 加 `houseLedgerFull`，编排器填，状态行末尾「段落账已满，新段不再计入」/「segment memory full」，新测试 3 条 |
| B 请求 2 | GLAZE ≤ 0 时 ClayGuard 复位，House 反相提示不工作 | 不改：与现有反相指示灯一致（只在 GLAZE > 0、方案源支持自动 Clay 时提示）；写进规格 |
| B 遗留 | 合并 C 后补 House 反相提示端到端 | **已补**：`GlazeControllerHouse` 新用例（宽低音亮、同相不亮、通用档不亮） |
| B 遗留 | 满 512 段时一次画像重算 65 µs（192 kHz / 32 块预算的 39 %） | 交评审：只在锁定后重开或长时间未锁定时出现 |
| B 遗留 | 每个实例多分配约 1.8 MB（通用档也分配，因曲风可在音频线程切到 House） | 接受，写进规格 |
| C 请求 1 | ClayGuard.h 注释（资格期 / 保持期、损失平滑） | **已改** |
| C 请求 2 | 限幅上限值本身做时间平滑 | 不改：上限随 BPM 连续变化，C 已验证切换处增益无台阶 |
| C 请求 4 | 规格回写（提示保持 2 s、损失 1 s 平滑、状态行优先级、选择器位置） | 规格已更新；选择器位置等 Max 看截图定 |
| D 请求 1 | gate_check.py 的配对键加 genre / delivery / listen_from_bar | 低优先，挂进 TASKS 待认领 |
| D 请求 2 | RenderMain.cpp 照抄了 MusicAnalyzer 的 6 个常数 | 低优先，挂进 TASKS 待认领（挪到公开头文件） |
| D 请求 3 | 工具的逐段统计与插件段落账口径对照 | 集成后在合成曲上对照（见第 3 节） |
| D 范围外 | 合成曲 breakdown（低频几乎为空）里修坯反相保护指示误亮 14.2 % 时长 | 范围外，报 Max 决定要不要另开任务 |

## 3 集成后的验收（服务器上）

见本文末尾的结果表（随跑随填）。
