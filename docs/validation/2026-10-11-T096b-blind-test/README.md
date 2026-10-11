# T-096b 轻量盲测脚本（R5-02 + R1-05 离线归一化 A/B）

2026-10-11 · 脚本 `tools/render/blind_test.py` · 规则出处 ADR-034 决定 4、T-096 计划 §13 Q13 / Q14 · 状态：代码改完·未验证（验收命令见末尾）

一句话：在 DGX 上把 A / B 两套渲染切成盲测包、事先登记规则、算统计；Max 在 Mac 上只播放、填答卷（工作流，不属「Mac 不跑测试」）。
`key.json`（答案）和 `registration.json` 留在 DGX，答卷交回前不下发；音频不进仓库。

## 协议数字（都是参数；登记文件记下生效值）

| 项 | 默认 | 出处 |
|---|---|---|
| 模式 | 2AFC 偏好 + 「无偏好」（`none`）；争议时 `--mode abx` | ADR-034 决定 4 |
| 片段 | 每首 3 个、12 小节（`--bars` 8～16）、含 breakdown→drop、两端 25 ms 升余弦淡入淡出（≥ 20 ms） | 同上 |
| 一组试次 | 2AFC 18 次，拆 2 个会话（9 + 9）；ABX 40 次，拆 3 个会话；每会话 ≤ 15 次，估时 > 30 min 会警告 | 同上 + BS.1116 |
| 等响 | 整曲匹配（渲染时 `--match-lufs`，或本脚本 `--normalize-lufs`），不用短窗；每个片段两侧响度差照量不误，> 0.1 LU 记「超容差」、> 0.3 LU「敏感」、> 1 LU「警告」，**都不拒绝出包** | 同上 + Q12 |
| 通过线 | 2AFC：排除「无偏好」后，偏好候选 ≥ 65 % 且单侧精确二项检验 p < 0.05；ABX：单侧 p < 0.05（40 次要 26 次） | 同上 |
| B 组不退步 | **不在脚本里判**，由完成报告对照 R5-01 最小指标集给出 | 同上 |

2AFC 有效试次（排除「无偏好」后）达到通过线所需的候选胜场：14 次 → 11、15 → 12、16 → 12（p = 0.038）、17 → 13、18 → 13、19 → 14、20 → 15；
完整表在 `registration.json` 的 `min_wins_by_valid_trials`。ABX 40 次：26/40 → p = 0.040，25/40 → p = 0.077 不过线。

## 流程

| # | 在哪 | 做什么 |
|---|---|---|
| 1 | DGX | 渲染 A、B 两套（每首歌各带自己的 `--genre` / `--bpm` / `--delivery`，**加 `--no-replay`**，两套 `--match-lufs` 取同一个值） |
| 2 | DGX | `blind_test.py build … --dry-run`：看每首歌挑到的窗口、跳过的歌与原因、覆盖缺口、会话估时 |
| 3 | DGX | `build --aa …` 出一个 A/A 包（B 侧读 A 的同一批文件）；出包时自动跑包体检查，必须全 ok（两侧逐字节相同、命名无规律、位置均衡） |
| 4 | DGX | `build …` 出真包；摘要里看响度差档位、0 dBFS 警告、包体检查 |
| 5 | DGX → 仓库 | **先把 `registration.json` 提交进 `docs/validation/<日期>-T096b-blind-<批次>/`**（git 提交时间 = 事先登记时间），再往下 |
| 6 | DGX → Mac | 只拉 `package/` 到 Mac 的 `artifacts/blind-<批次>/`（`.gitignore` 已排除）：`scp -r <DGX>:~/.cache/celadon-blind/<批次>/package artifacts/blind-<批次>` |
| 7 | Mac | 按 `LISTEN.txt` 在 DAW（FL Studio / Ableton）或 foobar2000 里一个会话一个会话听，填 `answers.csv` 的 `answer` 列；会话间休息 ≥ 会话时长；监听先拉低 6 dB |
| 8 | Mac → DGX | 只把填好的 `answers.csv` 拷回 DGX（不拷别的） |
| 9 | DGX | `blind_test.py score --batch ~/.cache/celadon-blind/<批次> --answers <answers.csv> --out <结果目录>` → `result.md` / `result.json` |
| 10 | DGX → 仓库 | 入库（见「结果入库」）；听感结论另写进版本档 |

渲染示例（DGX；`$CR` = `celadon_render`，每首一条，A / B 的参数换成本批要比的东西，这里以 GLAZE 35 对 70 举例）：

```bash
COR=~/.cache/celadon-t048-cal            # 45 首 Jamendo：<子类>/<曲>/<曲>.wav，44.1 kHz，已裁到小节线
RA=~/.cache/celadon-blind/b1-render-A; RB=~/.cache/celadon-blind/b1-render-B
while read cls track bpm delivery; do    # 清单见下表
  for side in A:35 B:70; do
    $CR --in $COR/$cls/$track --out ~/.cache/celadon-blind/b1-render-${side%%:*}/$track --glaze ${side##*:} \
        --style auto --clay auto --no-replay --genre $cls --delivery $delivery --bpm $bpm --match-lufs -14
  done
done < docs/validation/2026-10-11-T096b-blind-test/first-batch.tsv
python tools/render/blind_test.py build --out ~/.cache/celadon-blind/b1 --batch b1 --seed 20261011 \
    --a $RA --glob-a '*/*.wav' --b $RB --glob-b '*/*.wav' --a-desc '基线：…提交号' --b-desc '候选：…提交号' \
    --labels docs/validation/2026-10-01-T038-calibration/labels-auto.tsv \
    --labels-override docs/validation/2026-10-01-T038-calibration/labels-override.tsv \
    --tempo docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv  --dry-run
```

`--batch` 会写进下发的 `LISTEN.txt`：取中性名字（`b1`），别写 A / B 的设置。A、B 渲染必须记提交号与二进制 sha256（`--a-desc` / `--b-desc`），
守护族修复合并前后的渲染不要混比。要把 A、B 放在同一个目录里，用不同的 `--glob-a` / `--glob-b` 区分。

## 第一批：Jamendo 6 首

选法（确定性，不看任何结果）：对每个子类，把 `tempo-manifest.tsv` 里的歌按曲目号升序，取前 2 首同时满足——
速度可信（`fold_peak ≥ 5.0` 且 `100.5 ≤ BPM ≤ 139.5`，同 `calibrate_house.reliable_tempo`）；叠上 `labels-override.tsv` 后至少有 1 个
breakdown→drop 边界放得下 12 小节窗口（两侧各 ≥ 2 个核心小节）。第 1 首渲染 `--delivery streaming`，第 2 首渲染 `--delivery club`。
下表由本地 awk 按同一规则筛出（awk 复写了改标与交界规则），**以 `build --dry-run` 的输出为准**。

| 子类 | 曲 | BPM | 交付 | 边界（breakdown 小节 → drop 小节） | 预期窗口起点（12 小节） |
|---|---|---|---|---|---|
| deep | track_0155328 | 128.000 | streaming | 74–80 → 83–119；122–133 → 136–151 | 77、126、130 |
| deep | track_0155923 | 131.998 | club | 122–149 → 152–257 | 142、144、146 |
| tech | track_0003518 | 132.992 | streaming | 80–86 → 89–134 | 80、83、85 |
| tech | track_0107869 | 135.000 | club | 82–110 → 113–150 | 103、105、107 |
| melodic | track_0241941 | 129.998 | streaming | 100–129 → 132–203 | 122、124、126 |
| melodic | track_0647576 | 128.274 | club | 73–94 → 97–178 | 87、89、91 |

**要先知道的事实**：T-038 的 45 首里，每首最多 2 个 breakdown→drop 边界，没有任何一首有 3 个。所以「每首 3 个片段」只能在同一边界上把起点错开
≥ 2 小节来凑（脚本先每个边界取 1 个居中窗口，不够再错开补；登记里记 `boundaries_used`，`result.md` 写「取自 N 个不同边界」）。
首批 6 首共 7 个不同边界、18 个窗口，其中 11 个是同边界错开的窗口：它们是同一段音乐的重复测量，不是独立样本，统计只能当「个人的重复判断」读。
要更独立的证据：给 22 首门槛曲或 Max 自己的混音提供 BPM 与 breakdown→drop 区间，用 `--segments <JSON>`（`{"歌名": [[起点秒, 终点秒], …]}`）。

## R1-05：原混音对两版成品，三方归一化到 −14

```bash
python tools/render/blind_test.py build --out ~/.cache/celadon-blind/b2 --batch b2 --seed 20261012 --pair O:A \
    --original <原曲目录> --glob-o '*/*/*.wav' --a <成品目录> --glob-a '*/*.wav' --normalize-lufs -14 --labels … --tempo … --dry-run
```

`--pair 基线:候选`，条件 `A` / `B` 是两套渲染，`O` 是原曲；含 `O` 时必须给 `--normalize-lufs`（每个条件的整曲用 pyloudnorm 量响度、只乘一个增益，
双向对齐，不是只衰减；「只衰减」的平台真实行为只作数字记录，不做盲听）。原曲与成品逐样本对齐（成品多 0.35 s 余振），同一组秒数切三方。
一次只比一对（`O:A`、`O:B`、`A:B` 各出一个包，同一个 `--normalize-lufs` 保证可比）。
> 注意 `--glob-o` 要避开渲染输出子目录（`*/*/*.wav` 只匹配 `<子类>/<曲>/<曲>.wav`，不碰 `…/out/*.wav`）。

## 输出

| 位置 | 文件 | 下发 Mac？ |
|---|---|---|
| `<批次>/` | `registration.json`（决策规则、种子、窗口、等响容差、来源 sha256、脚本 sha256）、`key.json`（每个文件位对应哪一侧、带符号响度差、每文件 sha256） | **否** |
| `<批次>/package/` | `s1/ s2/ …/t01_1.wav, t01_2.wav`（ABX 是 `_a _b _x`）、`trials.csv`、`sessions.csv`、`answers.csv`（空模板）、`LISTEN.txt` | 是 |

公开的 `trials.csv` 只有 `trial / session / song / segment / start_s / end_s / bars / loudness_diff_abs_lu / loudness_flag / na_reason / file_*`：
没有基线 / 候选信息，响度差只给绝对值。文件是脚本自己写的 32-bit float WAV（不带 libsndfile 的 PEAK 块和时间戳），所以同一音频字节相同、重跑逐字相同。
ABX 的 `x` 文件是 `a` 或 `b` 的字节复制，**不要比校验和**。

## 结果入库

`docs/validation/<日期>-T096b-blind-<批次>/`：`registration.json`（出包后先提交）、`key.json`、`trials.csv`、`sessions.csv`、`answers.csv`（填好的）、
`result.md`、`result.json`、`REPORT.md`（听感结论、A/B 的提交号与二进制 sha256、对照 B 组指标得出的「通过 / 不通过」）。**不放音频**。
`result.*` 不带时间戳，同输入重跑逐字相同。

## 局限

- 一个人的结论只代表 Max（自用插件正是目标）；疲劳靠会话纪律控制，不靠脚本。
- A/A 包的自动检查证明「管线不泄漏」，不证明耳朵没偏好；Max 自己听 A/A 40 次只在想校准耳朵时自愿做（`score` 会对 A/A 包报「无泄漏 / 疑似泄漏」，不套通过线）。
- 真 A/B 里两侧峰值本来就可能不同（版本差异），脚本只查元数据与位置泄漏，峰值 / 响度差照实记在 `key.json`。
- 32-bit float 片段可能超过 0 dBFS，出包摘要会列出哪些试次（不说哪一侧）；监听先拉低 6 dB。

## 验收命令（DGX，由主会话跑；本文件对应的代码尚未运行过）

```bash
~/.cache/celadon-spark/venv/bin/python tools/render/blind_test.py selfcheck        # 末行「selfcheck：N 项，失败 0」，退出码 0
~/.cache/celadon-spark/venv/bin/python tools/render/blind_test.py build … --dry-run   # 6 首各 3 个窗口、覆盖 3 子类 × 2 交付、不写文件
~/.cache/celadon-spark/venv/bin/python tools/render/blind_test.py build … --aa --out …  # 包体检查全 ok（含「两侧逐字节相同」）
~/.cache/celadon-spark/venv/bin/python tools/render/blind_test.py score …           # 对自检答卷连跑两次，result.md / result.json 逐字相同
```
