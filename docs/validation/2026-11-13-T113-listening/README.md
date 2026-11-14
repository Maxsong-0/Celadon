# T-113 听感批 · 设计、候选与盲听方案（2026-11-14）

> 状态：**已实际验证**（第 5 节 ①～⑤ 都跑了；2026-11-14 主会话）。结论：三项这版都不进，出厂常数保持关，见第 8 节。
> 写这批的代理没有编译、没有跑任何测试与脚本；第 5 节的命令由主会话在 108（testbox）/ DGX 上跑，结果回填第 8 节。
> 分支 `t114/l7-t113-listening`，基点 `3f43ea0`（T-114 集成分支，第二波前段 L4 / L6 合入后）。
> 来源：体检报告 `docs/validation/2026-11-12-T105-plugin-review/1-sound.md` 的 1-05 / 1-06 / 1-07；1-06 的做法取自 T-093 R5-04 A「上行快通道」。
> 决定落在 vault `03-记忆/ADR-042-听感批预算环主段门冷启动与默认档音色.md`（主会话落盘）。

## 1. 三项改了什么

**出厂常数下三项全关，插件的声音与 `3f43ea0`（本线基点）逐位相同**（GLAZE 0 与 GLAZE > 0 都是）；候选只在渲染工具里用 `--candidate` 切。
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

渲染器都是 `c311586`（测试树 `wt/int-l7` 里 L7 交回的提交）编的 `celadon_render`，sha256 `9df34676…a17e000`；它和集成分支 `8c22147` 的 C++ 只差注释措辞与
3 处局部变量改名（`git diff c311586 8c22147 -- Source`）。产物在 DGX `~/.cache/celadon-t113/`。

| 项 | 结果 | 证据 |
|---|---|---|
| 108 定向 / 全量 | 定向 7728 / 0；全量 136195 / 0；改名（3 处局部名遮住类成员）后定向 5933 / 0 | testbox `20261114-010941-3438` / `-010944-65e6`；T-114 记录第 9 节 |
| MSVC 定向 | 5933 / 0，本工程 0 警告（改名后复核） | 108 主机 Wine |
| DGX aarch64 全量 | 135937 / 5；5 条是已知的 ColorModule 打包立体声快路径，没有新增 | `spark-test.sh test` |
| 渲染工具三条自检、`drop_report.py --selftest`、基准 | 渲染器三条与 `celadon_bench_portable --selftest` 过；`drop_report.py` 10 / 0（修了「三项 dB 量没有元素时写成整数 0」之后） | `20261114-010951-08b5`；`20261114-020956-965a` |
| ① 出厂 20 首网格 `audio_hash` 对 int2 | 240 行 0 行不同，全列 0 不同；`gate_check` FAILS 0 | `factory/` |
| ② 候选网格 `gate_check` / GLAZE 0 行哈希 | 7 个候选 FAILS 0；GLAZE 0 的 40 行与出厂 0 不同；GLAZE > 0 的 180 行里不同：c105 52、c106 63、c105-106 67、c107x15 120、c107x2 120、all-x15 145、all-x2 145（c107 两个不变的 60 行正是 GLAZE 100，f(1) = 1） | `<候选>/` |
| ③ 逐 drop 报告（合成 + 6 首） | 第 8.1 节 | `drops-jamendo-*`、`drops-syn-*` |
| ④ B 组 `quality_gate`（各候选对「现行」） | 第 8.2 节 | |
| ⑤ CPU 对比（出厂常数 `c311586` 对 `3f43ea0`） | 判干净；structWorst 没有两轮同向超过噪声的格；avg 两轮同向高 0.3～1.2 %（相对），指令数只多 +0.0001～0.0006 %，是代码布局。详见第 8.3 节 | DGX `perf-dgx.sh`；testbox `20261114-031651-a698` |
| 批 1 / 批 2 盲听 | 两批都没开：批 1 是预期没过（第 8.1 节），批 2 是 B 组两个强度都有新失败（第 8.4 节）。三项这版都不进，出厂常数保持关 | T-114 计划第 10 节 |

### 8.1 逐 drop 报告与批 1 不开

Jamendo 第一批 6 首 13 个 drop + 合成 House / Tech 124 各 2 个 drop，两侧都 `--cold --trace`；格子是「current → 候选」，报告全文在本目录 `drops-*`。

| 候选 | 范围 | 退让次数 | 增益总变差 dB | 前段增益 dB | 非主段放退让 dB |
|---|---|---|---|---|---|
| c105 | Jamendo 全部 drop | 15 → 14 | 15.89 → 15.01 | 1.21 → 1.20 | 0.58 → 0.00 |
| c105 | Jamendo 第一个 drop | 6 → 6 | 7.58 → 7.58 | 1.21 → 1.21 | |
| c105 | 合成 全部 / 第一个 | 0 → 0 | 4.42 → 4.42 / 3.35 → 3.35 | 1.81 → 1.81 / 2.00 → 2.00 | 0 → 0 |
| c106 | Jamendo 全部 drop | 15 → 18 | 15.89 → 14.65 | 1.21 → 1.26 | 0.58 → 0.48 |
| c106 | Jamendo 第一个 drop | 6 → 9 | 7.58 → 7.22 | 1.21 → 1.24 | |
| c106 | 合成 全部 / 第一个 | 0 → 0 | 4.42 → 7.23 / 3.35 → 4.32 | 1.81 → 1.73 / 2.00 → 1.92 | 0 → 0 |
| c105-106 | Jamendo 全部 drop | 15 → 19 | 15.89 → 14.58 | 1.21 → 1.25 | 0.58 → 0.00 |
| c105-106 | Jamendo 第一个 drop | 6 → 9 | 7.58 → 7.22 | 1.21 → 1.24 | |
| c105-106 | 合成 全部 / 第一个 | 0 → 0 | 4.42 → 7.23 / 3.35 → 4.32 | 1.81 → 1.73 / 2.00 → 1.92 | 0 → 0 |

对照第 5 节写好的预期：
- c105「非主段放退让 = 0」过（0.58 → 0），「drop 里的退让不多于 current」过（15 → 14）；但只改到 1 个 drop（`track_0241941` 第 2 个），批 1 的 13 个片段里 11 个与 current 逐位相同。
- c106 / c105-106「第一个 drop 前段增益高于 current、增益总变差不大于 current」：Jamendo 上前段增益只高 0.03 dB、总变差少 0.36 dB，退让反而多 3 次；
  合成曲上前段增益低 0.08 dB、总变差多 0.97 dB，方向与设计相反。
- c105-106「退让不多于 current」没过（15 → 19）。

批 1（`b113a`，c105-106 对 current）的 13 个片段逐个比两侧成品：约 4 个片段差信号在 −19～−31 dB（够得着听见），其余低于 −46 dB 或逐位相同。
2AFC 过线要 10 / 13（≥ 65 %、单侧 p = 0.046），9 个片段两侧几乎一样、只能猜，统计上过不了。

**决定（T-114 计划第 10 节，2026-11-14 02:0x）：批 1 不开，1-05、1-06 这版不进**，出厂常数保持关，代码不用动。依据是出包前写好的预期（「只决定值不值得拿去盲听」），
没有为了凑盲听改片段规则。后续：1-06 回到设计（为什么多退让、合成曲上为什么反向；打开前还要先解决存档向下兼容，见 `GlazeConstants.h` 常数旁的说明）；
1-05 换能触发它的素材（breakdown 里放回退让的歌）再测。

### 8.2 B 组（`quality_gate` 各候选对「现行」）

网格同「现行」那一行（T-114 记录第 8 节）：22 首（DGX 上 20 首）× GLAZE 0 / 35 / 70，Jamendo 45 首 × GLAZE 35 / 100，风格自动、修坯自动、`--trace`；
渲染器同上加 `--candidate <候选>`（包装脚本），对新 B9 口径的「现行」（`~/.cache/celadon-t114/quality/current/b9r`，入库那两份）判。产物 `quality/<候选>/`。

| 候选 | 22 首 A / B 退步 | 45 首 A / B 退步 | 退步最多的项（22 首 + 45 首） |
|---|---|---|---|
| c105-106 | 0 / 22 | 0 / 91 | B7b S/M 30、B3 PSR 29、B2b LRA 21、B9a 10、B2a 10、B9b 8 |
| c107x2 | 0 / 23 | 0 / 38 | B7b S/M 23、B9a 增益总变差 12、B3 PSR 9、B2b LRA 5、B4 泵感 4 |
| c107x15 | 0 / 21 | 0 / 32 | B7b S/M 21、B9a 增益总变差 11、B3 PSR 8、B2b LRA 5、B4 泵感 3 |

- **c105-106 的条数混着量具区间的变化**：22 首里有 GLAZE 0 的行也判了退步（12223、l_kskdhjf、wildtom），可这些成品与出厂逐位相同（第 ② 项）。
  量具按 trace 的 `current_mainness ≥ 0.5` 划主段（`quality_metrics.py` ⑤），1-06 改了实时画像的 I，主段度跟着变：GLAZE 0 读数变了的 4 首（这三首加 emmmayiyu）
  正是 `main_s` 变了的，主段各项是在另一段区间上量的。c105-106 本来就不进版，只记下来；以后比动分析器的候选，主段区间应当固定取现行那一侧的。
- **c107 两个强度只动 GLAZE 35 / 70**：GLAZE 0 与 GLAZE 100 的行与现行逐列相同（22 首 GLAZE 0 的 20 行、45 首 GLAZE 100 的 45 行，核过），1-07 不碰分析器，主段区间不变。
- c107x2 在 GLAZE 35 的方向很一致（45 首，逐行差中位）：净频谱变化（B8，只报）+0.24 dB——音色动作确实放大了；同时 S/M 变化 +0.03 dB（30 / 45 行变大，
  变大那部分 P90 0.22 dB）、泵感 +0.025 dB（40 / 45）、限幅差值 +0.21 dB（36 / 45）、低频峰均 −0.04 dB（36 / 45）。PSR、LRA 基本不动。
  也就是动作越大，「处理了多少」这类读数越大；「对现行不退步」对一个本来就要让动作更明显的候选是很紧的门。
- c107x15 同样只动 GLAZE 35 / 70（GLAZE 0 的 20 行、GLAZE 100 的 45 行与现行逐列相同，核过），退步的项与 c107x2 同一批，条数略少（21 + 32 对 23 + 38）。

### 8.3 CPU

DGX，`scripts/perf-dgx.sh` 日常子集（`--scenario default,manual,auto100 --sr 48000,96000,192000`：四个场景含孪生 `default-g0` × 3 个采样率
× 块长 32 / 64 / 512 × 两层 = 72 个实测配置，另有 36 行估计行），基线 `3f43ea0`（`t114-q-current`，`celadon_bench_portable` sha256 `e5bf4fd1…24fb704ad`）
与当前 `c311586`（出厂常数，sha256 `818ee16d…5e3300253`）交替各 2 轮，绑核 5（Cortex-X925 3.9 GHz、调速器 performance）。
11-14 02:33～03:11；编译观察 0 条、心跳 1112，**判干净**。四轮开跑时的一分钟负载 8.38 / 2.02 / 1.39 / 1.39（第一个是上一个渲染任务的尾巴，
那一轮结束时已降到 2.02；它的读数对第 2 轮没有系统偏高：64 行 structWorst 两轮之差均值 +0.002 个百分点、31 行为正）。
原始数据与对比表入库在本目录 `cpu-20261114/`：`run-20261114-103343Z/` 整目录、`compare.md`（跑完当时、阈值还空着）、
`compare-thresholds-20261114.md`（填上下面的阈值后重比）。

**结论：出厂常数下的 L7 代码不拖慢最慢块；平均占用稳定高一点点，但在噪声级。** 单位都是「块预算的百分点」，Δ = 当前 − 基线，按轮配对：

| 量 | 门槛内 64 行（不含 192k / 32 与估计行） | 192k / 32（只报） |
|---|---|---|
| structWorst 每轮 Δ | 中位 0.012、p99 0.152、最大 0.172（192k / 64 处理器层 `default-g0`）；两轮同为正的格里较小那轮最大 0.065 | 最大 0.518 |
| avg 每轮 Δ | 最大 0.073（192k / 64 链层）；**72 个实测格两轮同向偏高，相对 +0.3～+1.2 %**（GLAZE 0 的 `default-g0` 也一样） | 最大 0.058 |
| 同一二进制两轮 structWorst 极差 | 最大 0.187（基线 0.187、当前 0.154） | 最大 0.567 |
| invol_switches（每配置一轮） | 48k ≤ 81、96k ≤ 156、192k ≤ 292 | ≤ 315 |

avg 那一点点不是多做了事：在 108 上用 cachegrind 数两版（同样两个提交、x86_64 GCC Release）链层跑一段的指令数（只计指令，与计时和代码布局无关；
testbox `20261114-031651-a698`），四个配置当前比基线只多 +0.0001 %～+0.0006 %：

| 配置（链层，`--warm` + 一遍） | 基线指令数 | 当前 − 基线 |
|---|---|---|
| `default-g0` 48k / 512（4 s + 8 s） | 9,056,081,306 | +13,079（+0.0001 %） |
| `default` 48k / 512（4 s + 8 s） | 14,553,916,637 | +56,298（+0.0004 %） |
| `manual` 192k / 64（2 s + 4 s） | 27,582,606,459 | +179,009（+0.0006 %） |
| `default-g0` 192k / 64（2 s + 4 s） | 27,580,562,424 | +58,543（+0.0002 %） |

比 DGX 上 avg 的 +0.3～+1.2 % 小三个数量级。L7 在出厂常数下每块多的只是几次比较与一次乘 1（`timbreStrength`、`recoveryHeld`、
`noteCatchUp` 第一行就返回），所以 DGX 上那点差是两份二进制的代码与结构体布局不同（DGX 是 GCC 13、无 LTO；`GlazeController::Impl` 多了
两份常数、`LiveConstants` 多了 4 个 double），同一份源码换个布局也会有这么大的差，不算退步。

**阈值起点**（复查第 1 轮 F12 的后续；终值由 Max 定，T-096 计划 §13 Q10 / Q12）。取法统一：这次实测里对应量的最大值 × 2，往上取到 0.05 的整倍数
（切换次数取到 50）。× 2 是因为这只是一次 2 轮、64 行的干净跑，最大值低估了噪声的上沿；再大就抓不住小退步。

| 阈值 | 值 | 依据（这次实测） | 能抓到多大的退步 |
|---|---|---|---|
| `STRUCT_WORST_DELTA_PP` | 0.35 | 每轮 Δ structWorst 最大 0.172 | 两轮都涨 0.35 个百分点：48k / 32（约 4.2 %）相对 +8 %，96k / 32（约 10.9 %）+3 % |
| `AVG_DELTA_PP` | 0.15 | 每轮 Δ avg 最大 0.073 | 48k / 512（约 1.3 %）要 +12 %，192k / 64 链层（约 7.3 %）+2 % |
| `ROUND_SPREAD_PP` | 0.4 | 同一二进制两轮极差最大 0.187 | 门槛内的格这次都在 0.19 以内；192k / 32 有 4 行超过（只报的格，本来就不稳，所以只报） |
| `INVOL_SWITCHES_MAX` | 650 | 全部行最大 315（192k / 32） | 只抓得到严重的抢占：48k 的格平常 ≤ 81，要多到 8 倍才标；按每秒归一要改工具，等 Max 定 |
| `LOAD1_MAX`（`scripts/perf-dgx.sh`） | 10 | DGX 20 核的一半；这次第 1 轮开跑 8.38 没有可见影响（见上） | 开跑前一分钟负载 > 10 就作废重跑 |

用这组阈值重比这次的数据：退出码 0，没有「退步」「不一致」与 FAIL；只有 192k / 32 的 4 行（2 行实测、2 行估计）标「各轮不一致」——它们本来就只报不判。
阈值是绝对百分点：占用低的格灵敏度低（上表最后一列）；要按相对值判得改 `compare_perf.py` 的判法，不在这版范围，列进给 Max 的待定项。

### 8.4 批 2 不开，1-07 这版不进

按第 6 节出包前写好的规则：`c107x2` 有新失败（22 首 23 条、45 首 38 条），`c107x15` 也有（21 条、32 条）→ **两个都有，批 2 不开，回到设计**。
1-07 的出厂倍数保持 1（`timbreScaleAtKnee`），代码不用动。决定时间 2026-11-14 02:3x（T-114 计划第 10 节）；规则是听之前定的，没有看过结果再改门槛。

三项都没进版：1-05、1-06（第 8.1 节）与 1-07 的代码都留在里面，出厂常数下与加这三项之前逐位相同（第 ①② 项），渲染工具的 `--candidate` / `--cold` 与
`drop_report.py` 照常可用。ADR-042 状态随之改。后续（回到设计时先做）：
- 1-07：查清 S/M、泵感、限幅差值是哪个音色动作放大后带出来的（Tone、Recover、Tame、推响不刺、目标频谱、Heat 逐个开），只放大不带副作用的那几个；
  并在出包前定好「本来就要多处理一点」的候选 B 组怎么判（例如 S/M 用绝对线而不是「不比现行差」），写进登记再量。
- 1-06：先弄清 c106 为什么多退让、合成曲上为什么反向，再解决存档向下兼容（第 8.1 节）。
- 1-05：换 breakdown 里真会放回退让的素材再量；Jamendo 第一批 13 个 drop 里只碰到 1 个。
