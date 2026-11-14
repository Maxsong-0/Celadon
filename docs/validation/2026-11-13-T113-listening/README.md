# T-113 听感批 · 设计、候选与盲听方案（2026-11-14）

> 状态：**代码改完·未验证**。写这批的代理没有编译、没有跑任何测试与脚本；第 5 节的命令由主会话在 108（testbox）/ DGX 上跑，
> 结果回填第 8 节。分支 `t114/l7-t113-listening`，基点 `3f43ea0`（T-114 集成分支，第二波前段 L4 / L6 合入后）。
> 来源：体检报告 `docs/validation/2026-11-12-T105-plugin-review/1-sound.md` 的 1-05 / 1-06 / 1-07；1-06 的做法取自 T-093 R5-04 A「上行快通道」。
> 决定落在 vault `03-记忆/ADR-042-听感批预算环主段门冷启动与默认档音色.md`（主会话落盘）。

## 1. 三项改了什么

**出厂常数下三项全关，插件的声音与 0.1.8.10 / `3f43ea0` 逐位相同**（GLAZE 0 与 GLAZE > 0 都是）；候选只在渲染工具里用 `--candidate` 切。
新常数不进 `Constants`（不进标定快照与存档、不参加 `bitwiseEqual`），没有参数、没有界面，存档格式不变。

| 项 | 问题（体检报告） | 做法 | 出厂（= 现状） | 候选 |
|---|---|---|---|---|
| 1-05 预算环主段门 | 预算环的安全计时只看「有效音乐」，breakdown 又轻又安全，5 s 后把退让放回去；下一个 drop 头上再多压、再退一次 | 编排器每块把 `currentMainness`（Limit Gain「非主段只降不升」用的同一个主段度）交给预算环：低于门槛的块不放回退让、安全计时停住（不清零）；超出照样退让 | `ListeningConstants::guardRecoverMinMainness = 0`（不看主段） | `c105`：0.5 |
| 1-06 冷启动第一个 drop | 冷启动时前奏当了主段，I 停在前奏的电平；第一个 drop 响几 LU，I 按 90 s 遗忘要几十秒才跟上，drop 头上多压 | 实时画像的上行快通道：本段与紧挨着往前的一串段都比 I 响 1 LU 以上、进账 m ≥ 0.5，累计 ≥ 4 s，且歌开头 180 s 有效音频以内 → 两个事件时钟各加「本段有效时长 × ln2 ÷ 半衰期」，更老的段（含前奏）的权重按半衰期减半；和换歌软遗忘同一套时钟，不新增存档字段 | `LiveConstants::catchUpHalfLifeSeconds = +∞`（关） | `c106`：2 s |
| 1-07 默认 35 % 音色 | 音色类自动动作按 GLAZE 线性给，默认 35 % 时几乎听不出 | 音色类强度按 f(GLAZE) 取：过 (0, 0)、(0.35, 倍数 × 0.35)、(1, 1) 的两段直线。管 Tone、Recover、Tame、推响不刺、目标频谱、Heat；**不管** Limit Gain（推响）、G_trim、Glue、Clip、Clay | `ListeningConstants::timbreScaleAtKnee = 1`（f(g) = g） | `c107x15`：1.5（f(0.35) = 0.525）；`c107x2`：2（f(0.35) = 0.7） |

组合候选：`c105-106`（1-05 + 1-06）、`all-x15` / `all-x2`（三项一起）。

f 的性质（`GlazeConstants.h` 的 `timbreStrength`，单测 `GlazePlanner / timbreStrength mapping`）：f(0) = 0、f(1) = 1、单调不减、
GLAZE 100 时与现状逐位相同（不超过现上限）、倍数 1 时原样返回（逐位不变）；坏常数（非有限、≤ 0、拐点不在 (0, 1)、倍数 × 拐点 > 1）按出厂处理。
Heat 的上限照旧是风格的 `heatDriveCap` 与计划的 Heat 余量（`heatAllowance`），倍数只放大到上限为止。

### 改到的文件

- `Source/dsp/GlazeConstants.h`：`LiveConstants` 加 `catchUp*` 四项；新结构 `ListeningConstants` 与 `timbreStrength`。
- `Source/dsp/LiveProfile.{h,cpp}`：键表加四个键（`--live-constants` 与清洗用同一张表，共 23 个）；`noteCatchUp`；读档的参考时钟上界加
  「有效秒 × 694」一项（与读档这一刻的常数无关：开着快通道存的档，出厂常数也读得回）。
- `Source/dsp/GlazeGuard.{h,cpp}`：`process (stats, recoveryHeld)`；单参数版本 = `process (stats, false)`，逐位同旧。
- `Source/dsp/GlazeController.{h,cpp}`：主段门、Tone 跟随 / 上限 / Tame 用 f(GLAZE)、`PlanInputs::listening`；测试 / 渲染入口
  `setListeningConstantsForTesting`、`setLiveConstantsForTesting`、`getListeningConstants`。
- `Source/dsp/GlazePlanner.{h,cpp}`：Tone / Recover / Tame 的强度与 Heat 按 f(GLAZE) 放大；Limit Gain、Glue、Clip 不动。
- `Source/dsp/CeladonChain.cpp`：推响不刺 / 目标频谱「自动」的量按 f(GLAZE)。
- 测试：`GlazeCalibration`（主段门）、`LiveProfile`（快通道、键表 23）、`GlazePlanner`（映射）、`CeladonChainMonitoring`（候选在链上的契约）。
- `tools/render/RenderMain.cpp`：`--candidate`、`--cold`；`tools/render/drop_report.py`（新）；`tools/render/README.md`。
- `PluginProcessor` 不用改：出厂常数是编排器的默认值，插件从不调测试入口。

## 2. 硬约束怎么守

| 约束 | 怎么守 | 怎么验 |
|---|---|---|
| GLAZE 0 逐位直通 | f(0) = 0；主段门与快通道只改预算环退让与画像，GLAZE 0 的链本来就走逐位直通、不用它们（新用例实测把关，不靠推理） | `CeladonChainMonitoring` 新用例：所有候选全开，GLAZE 0 与不开逐位相同；20 首网格 GLAZE 0 行 `audio_hash` |
| 出厂常数下 GLAZE > 0 也逐位相同 | 出厂值走的分支与旧代码同一串运算：主段门 `0 > 0` 为假；半衰期 +∞ 第一行返回；倍数 1 时 `timbreStrength` 原样返回、Heat 那一行不进 | 20 首网格 240 行 `audio_hash` 对 `int2` 基线 0 行不同 |
| 存档格式不变 | 不新增节点 / 属性；快通道的「一串」从窗口里的段现算；印记随段存（原有字段） | `LiveProfile` 新用例：开着快通道存的档，出厂常数读回逐位相同；`LegacyFixture`、`ProcessorState` 全过 |
| 0.1.8.10 工程能读 | 同上；读档上界只放宽、不收紧 | `LegacyFixture` |
| 音频线程 0 分配 0 锁 | 新代码只有标量运算、读已有的段（`entryAt`） | `CeladonChainGuards` 的分配 / 锁检查；`celadon_bench_portable` 对比 |

## 3. 候选与「换成出厂」

候选表在 `tools/render/RenderMain.cpp` 开头（`renderCandidates` 与四个 `candidate*` 常数）。某个候选通过盲听、要进版时，**只改
`Source/dsp/GlazeConstants.h` 的默认值**（候选表不用动，那时 `current` 就等于它）：

| 进版的项 | 改哪一行 | 跟着改的测试 |
|---|---|---|
| 1-05 | `float guardRecoverMinMainness = 0.0f;` → `0.5f` | `GlazeCalibration` 新用例里「出厂 = 不看主段」的对照改成显式传 0 的常数 |
| 1-06 | `double catchUpHalfLifeSeconds = std::numeric_limits<double>::infinity();` → `2.0` | `LiveProfile / catch-up` 里「出厂 = 关」那一段改成显式传 +∞；`--selftest-live` 的追踪摘要会变（重记） |
| 1-07 | `float timbreScaleAtKnee = 1.0f;` → `1.5f` 或 `2.0f` | `GlazePlanner / timbreStrength mapping` 里「nullptr 与出厂相同」不变，「倍数 1 原样返回」改成显式传 1；`CeladonChainMonitoring / listening candidates` 的出厂对照同理 |

进版后 GLAZE > 0 的旧工程声音会变（ADR-035 允许，GLAZE 0 不变），版本档与更新说明照 ADR-035 写；20 首网格、门槛、B 组要对新基线重跑。

## 4. 量什么

- **1-05 / 1-06：`drop_report.py`**（只用标准库，口径在文件头）。基线与候选各渲一遍（都带 `--trace`），逐首逐 drop 并排：
  预算环退让次数与退让量、Limit Gain 总变差、drop 头 8 小节的平均 Limit Gain、进 drop 时的 `guard_db`；整首「非主段放退让」
  （主段度 < 0.5 时 `guard_db` 上升的总量，`c105` 时应为 0）。段落：合成曲用标注 JSON，Jamendo 用 T-038 的逐小节标签 + 速度表。
  1-06 只在冷启动出现，所以候选与基线**都加 `--cold`**（成品不读回第一遍的存档），第一个 drop 单列一行合计。
- **1-07：`quality_metrics.py`**：`net_spectrum_p95_db` / `net_spectrum_bands_db`（净谱变化，预期变大——这正是要的效果，只报不判），
  加上 B 组全套（`quality_gate.py` 对「现行」基线不退步）。另看成品追踪的 `tone_db`、`heat` 列的均值作旁证。
- **A 组硬门（ADR-034）**：真峰值 ≤ Ceiling + 0.01、硬钳位 0（`quality_gate.py` 的 A1 / A2），`gate_check.py` FAILS 0。

## 5. 验收命令（主会话跑；先 108，其次 DGX）

```bash
# ---- 108（testbox x86-1）：打包同 T-114 记录第 0 节（git archive <本分支 HEAD> + JUCE 9.0.2 modules + run-ci.sh）----
testbox submit --path <打包目录> --arch x86_64 -- ./run-ci.sh --only GlazeCalibration,LiveProfile,GlazePlanner,GlazeController,GlazeControllerTrimFreeze,GlazeControllerTrimLoop,GlazeBehaviourReport,CeladonChainMonitoring,CeladonChain,CeladonChainGuards,CeladonChainClay,ClayGlaze,GlazeChain,ChainDigest,Phase2Orchestrator,LegacyFixture,LegacyState,SegmentLedger
testbox submit --path <打包目录> --arch x86_64 -- ./run-ci.sh            # 全量：新失败 0
# 同一打包另编渲染工具与基准：celadon_render --selftest / --selftest-transport / --selftest-live（后者含 --candidate 新项、键表 23）、
# celadon_bench_portable --selftest
# Python 自检（tb/run-py.sh 那一套，加一条）：python3 tools/render/drop_report.py --selftest   # 末行「失败 0」
# MSVC：CELADON_WIN_HOST=maxsong@100.84.85.68 bash scripts/windows-wine.sh test --only GlazeCalibration,LiveProfile,GlazePlanner,CeladonChainMonitoring

# ---- DGX：渲染与报告（$CR = 本分支编出的 celadon_render；$PY = ~/.cache/celadon-spark/venv/bin/python）----
# ① 出厂常数 20 首网格：240 行 audio_hash 对 int2 基线 0 行不同、gate_check FAILS 0
$CR --in <20 首目录> --out ~/.cache/celadon-t113/factory/songs --glaze 0,35,70,100 --clay auto,off --style auto --trace
#   对 ~/.cache/celadon-t114/int2/songs/metrics.csv 逐行比 audio_hash（比法同 T-114 记录第 7 节 cmp-base.txt）
python3 tools/render/gate_check.py ~/.cache/celadon-t113/factory/songs/metrics.csv
# ② 候选网格（同 20 首，每个候选一个 --out）：A 组硬门与 gate_check
for c in c105 c106 c105-106 c107x15 c107x2 all-x15 all-x2; do
  $CR --in <20 首目录> --out ~/.cache/celadon-t113/$c/songs --glaze 0,35,70,100 --clay auto,off --style auto --trace --candidate $c
  python3 tools/render/gate_check.py ~/.cache/celadon-t113/$c/songs/metrics.csv
done
#   GLAZE 0 行的 audio_hash 必须与出厂相同（候选在 GLAZE 0 不起作用）
# ③ 逐 drop 报告（合成 House / Tech 124 + Jamendo 第一批 6 首；current 与候选都 --cold --trace）
$PY tools/render/make_synthetic_house.py ~/.cache/celadon-t113/syn
for c in current c105 c106 c105-106; do
  $CR --in ~/.cache/celadon-t113/syn --out ~/.cache/celadon-t113/drops/$c/syn --glaze 35 --clay auto --style auto --no-replay --bpm 124 --trace --cold --candidate $c
  while read cls track bpm; do
    $CR --in ~/.cache/celadon-t048-cal/$cls/$track --out ~/.cache/celadon-t113/drops/$c/$track --glaze 35 --clay auto --style auto \
        --no-replay --bpm $bpm --match-lufs -14 --trace --cold --candidate $c
  done < docs/validation/2026-10-11-T096b-blind-test/first-batch.tsv
done
for c in c105 c106 c105-106; do
  python3 tools/render/drop_report.py compare --a ~/.cache/celadon-t113/drops/current --b ~/.cache/celadon-t113/drops/$c --b-name $c \
      --labels docs/validation/2026-10-01-T038-calibration/labels-auto.tsv \
      --labels-override docs/validation/2026-10-01-T038-calibration/labels-override.tsv \
      --tempo docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv --out docs/validation/2026-11-13-T113-listening/drops-jamendo-$c
  # 合成曲另跑一次：--annotations ~/.cache/celadon-t113/syn（两份 JSON），--a / --b 指到各自的 syn 子目录
done
# ④ B 组（quality_metrics + quality_gate）：run_quality_baseline.sh 照「现行」那一行的跑法，把 $CR 换成带 --candidate 的包装脚本
#   （或改 ARGS_22 / ARGS_45 临时加 --candidate），对 int2 的「现行」quality-22.csv / quality-45.csv 跑 quality_gate.py：FAIL 0
# ⑤ CPU：scripts/perf-dgx.sh 出厂 vs 3f43ea0，tools/perf/compare_perf.py 不退步
```

期望：①～⑤ 全过；③ 的 `c105` / `c105-106` 非主段放退让 = 0，drop 里的退让次数不多于 current；`c106` / `c105-106` 第一个 drop 的
前段增益高于 current、增益总变差不大于 current（这两条是**预期**，不是门槛：它们只决定值不值得拿去盲听）。

## 6. 盲听方案（两批，每批 2 个会话、每会话 9 次约 12～15 分钟；协议 ADR-034 决定 4：2AFC，偏好候选 ≥ 65 % 且单侧 p < 0.05）

素材：Jamendo 第一批 6 首（`docs/validation/2026-10-11-T096b-blind-test/first-batch.tsv`：deep `track_0155328` / `track_0155923`，
tech `track_0003518` / `track_0107869`，melodic `track_0241941` / `track_0647576`；DGX `~/.cache/celadon-t048-cal/<子类>/<曲>/<曲>.wav`，44.1 kHz）。
两侧都 GLAZE 35（默认）、风格自动、修坯自动、`--no-replay --match-lufs -14 --bpm <速度>`；包里再按 `--normalize-lufs` 不另对齐（两侧已对到 −14）。

**批 1（`b113a`）：1-05 + 1-06 一起，`c105-106` 对 `current`，两侧都 `--cold`**。理由：两项改的是同一件事（drop 头上多压、再退），
单独听各自差别更小；合在一起过了再看 `drop_report` 分清各自的份。片段 = 每首 3 个 12 小节窗口（drop 前 4 小节 + 后 8 小节，
第一个 drop 必在），用 `drop_report.py windows` 只按段落标签算，不看任何渲染结果：

```bash
python3 tools/render/drop_report.py windows --labels docs/validation/2026-10-01-T038-calibration/labels-auto.tsv \
    --labels-override docs/validation/2026-10-01-T038-calibration/labels-override.tsv \
    --tempo docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv \
    --tracks track_0155328,track_0155923,track_0003518,track_0107869,track_0241941,track_0647576 \
    --out docs/validation/2026-11-13-T113-listening/b113a-segments.json
# 渲染：上面 ③ 的 Jamendo 那部分（current 与 c105-106 两侧）就是批 1 的素材，--trace 不改成品（--selftest 有阳性对照）
python tools/render/blind_test.py build --out ~/.cache/celadon-blind/b113a --batch b113a --seed 20261114 \
    --a ~/.cache/celadon-t113/drops/current --glob-a '*/*.wav' --b ~/.cache/celadon-t113/drops/c105-106 --glob-b '*/*.wav' \
    --a-desc 'current --cold，<提交号>，<celadon_render sha256>' --b-desc 'c105-106 --cold，<提交号>，<sha256>' \
    --segments docs/validation/2026-11-13-T113-listening/b113a-segments.json --tracks track_0155328,track_0155923,track_0003518,track_0107869,track_0241941,track_0647576 \
    --tempo docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv --dry-run      # 先预演：18 个片段、2 会话 × 9
# 去掉 --dry-run 正式出包 → 先把 registration.json 提交进 docs/validation/2026-11-13-T113-blind-b113a/（听之前）
python tools/render/blind_test.py build --out ~/.cache/celadon-blind/b113a-aa --batch b113a-aa --seed 20261114 --aa \
    --a ~/.cache/celadon-t113/drops/current --glob-a '*/*.wav' --segments … --tempo …                    # A/A 自检包：体检全 ok
```

窗口不够 3 个的歌（`windows` 会打印「只放得下 N 个」）：照收，试次相应变少；总数 < 18 时 `--trials` 写实际片段数、`--sessions 2`。

**批 2（`b113b`）：1-07，一个强度对 `current`**。两个强度各听一批会让 Max 的会话翻倍；拆半（各 9 次）每边只有 9 次，
7 / 9 的 p ≈ 0.09，过不了线。所以**先用 B 组筛、只听一个**，规则在出包前写进登记、不看听感结果：
1. `c107x2` 与 `c107x15` 都跑第 5 节 ④（B 组 + A 组）。
2. `c107x2` 的 B 组对「现行」没有新失败 → 批 2 听 `c107x2`（差别最大，最能回答「默认档要不要听得出」）；
   `c107x2` 有新失败而 `c107x15` 没有 → 听 `c107x15`；两个都有 → 批 2 不开，回到设计。
3. 两侧都**读回存档**（不加 `--cold`：音色是稳态的事，冷启动只会混进 1-06 的差别），片段用 `blind_test.py` 自己的
   breakdown→drop 窗口（`--labels` + `--labels-override` + `--tempo`，同 T-096b 第一批），种子 `20261115`，同样先登记、做 A/A。
4. 批 2 选中的强度通过 → 进版这个强度；没通过 → 1-07 不进版（不回头再听另一个强度，免得多重比较）。

两批都要在**同一个提交**的渲染之间比（`--a-desc` / `--b-desc` 写提交号与二进制 sha256）；批 1 与批 2 的候选互不叠加
（批 2 的两侧都不开 1-05 / 1-06）。听感没过的候选不进版，照实写进版本档（T-114 完成标准）。

## 7. 我替你定了什么

- 1-05 的门槛取 0.5（插件里「算主段」的同一条线），在非主段**暂停**安全计时而不是清零：breakdown 结束回到主段时接着数，不会因为一段 breakdown 把 5 s 重新数一遍。
- 1-06 放在实时画像里做（事件时钟），不在编排器里另加一条增益通道：I 本身跟上，方案、上限、预算环都照常用它；只管歌开头 180 s 有效音频（冷启动），
  之后的 drop 照旧慢；门槛 1 LU、连续 4 s 取 T-093 R5-04 的 δ 与 k = 2 段。半衰期候选 2 s：8 小节 drop 约 15 s，前奏的权重降到约 1 / 180，I 基本到位。
- 1-06 的「一串」用此刻的 I 判往前的段（不存每段当时的门槛）：I 被往上拉时偏保守，可能少触发，不会多触发；换来的是存档格式不变。
- 1-07 只放大音色类（Tone、Recover、Tame、推响不刺、目标频谱、Heat），不碰推响（Limit Gain、G_trim）与动态（Glue、Clip）、Clay：
  默认档「听得出」应当是音色，不是更响；更响会和响度对齐的盲听混在一起。
- 1-07 用两段直线而不是幂函数：拐点处倍数准确等于候选（1.5 / 2），GLAZE 100 准确等于现值，出厂（倍数 1）走 `return glaze` 逐位不变。
- 渲染工具加 `--cold`：1-06 只在冷启动出现，默认的「读回第一遍存档再渲」看不到它。
- 批 1 合听 1-05 + 1-06、批 2 先用 B 组筛出一个强度再听（理由见第 6 节）。

## 8. 结果（主会话回填）

| 项 | 结果 | 证据 |
|---|---|---|
| 108 定向 / 全量 | | |
| MSVC 定向 | | |
| 渲染工具三条自检、`drop_report.py --selftest`、基准 | | |
| ① 出厂 20 首网格 `audio_hash` 对 int2 | | |
| ② 候选网格 `gate_check` / GLAZE 0 行哈希 | | |
| ③ 逐 drop 报告（合成 + 6 首） | | |
| ④ B 组 `quality_gate`（各候选对「现行」） | | |
| ⑤ CPU 对比 | | |
| 批 1 / 批 2 盲听 | | |
