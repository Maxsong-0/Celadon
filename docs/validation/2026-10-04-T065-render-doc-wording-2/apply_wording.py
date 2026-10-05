#!/usr/bin/env python3
"""T-065：tools/render 的第二批说明文字（Max 2026-10-04 19:4x 同意）：① 代价限来源写全；② ADR-016「逐位」口径改成「最大样本差 ≤ −180 dBFS」；
③ 第 6 项 --trace 的修坯列说明（措辞取核实 workflow wf_8b34caaf-a79 的 proposals 11～15）。
每条替换都先断言 old 在文件里恰好出现一次（对不上就停，一个文件都不写）；--dry-run 只报匹配情况。只改文字与注释，判定逻辑与 metrics.csv 不变。"""
import pathlib, sys

dry = "--dry-run" in sys.argv
R = pathlib.Path("tools/render")
README, MAIN = R / "README.md", R / "RenderMain.cpp"

changes = []   # (文件, 名字, old, new)

# ---------- ① 代价限来源写全（先试后用 / 守护混合损失封顶 / 压力反馈降档三处都会置位 clay_cost_limited）----------
changes.append((README, "① README:138 代价限来源",
    "先试后用（ADR-012）：成品末尾哪几项因「试过会抬高峰值」被减量 / 否决（`lowcut` / `width`，两项都有写 `lowcut+width`，空 = 没有；ADR-015 起自动不收单，不再出现 `mono`）。",
    "成品末尾哪几项「自动」修坯因代价被减量 / 否决（`lowcut` / `width`，两项都有写 `lowcut+width`，空 = 没有；ADR-015 起自动不收单，不再出现 `mono`）。三处会置位：先试后用试出会抬高峰值（ADR-012）、低切按守护混合损失封顶（ADR-027）、压力反馈降过档（ADR-014 / ADR-028）。"))
changes.append((MAIN, "① RenderMain:2250 costLimitedText 注释",
    "    // 先试后用（ADR-012）：成品末尾哪几项因「试过会抬高峰值」被减量 / 否决（Status::clayCostLimitedMask）。空 = 没有。",
    "    // 成品末尾哪几项「自动」修坯因代价被减量 / 否决（Status::clayCostLimitedMask）。空 = 没有。三处会置位：先试后用试出会抬高峰值\n"
    "    // （ADR-012）、低切按守护混合损失封顶（ADR-027）、压力反馈降过档（ADR-014 / ADR-028）。"))
changes.append((MAIN, "① RenderMain:2516 metrics.md 口径",
    "是成品末尾因代价被减量 / 否决的「自动」修坯项（lowcut、width 或 lowcut+width；metrics.csv 的 clay_cost_limited）。\\n\"",
    "是成品末尾因代价被减量 / 否决的「自动」修坯项（lowcut、width 或 lowcut+width；metrics.csv 的 clay_cost_limited）："
    "先试后用试出会抬高峰值、低切按守护混合损失封顶、压力反馈降过档，三处任一都会置位。\\n\""))

# ---------- ② ADR-016「逐位」口径：阳性对照的判据是最大样本差 ≤ −180 dBFS，不要求逐位 ----------
changes.append((MAIN, "② RenderMain:53 文件头注释",
    "同时做阳性对照（修坯「手动」照填自动链实际值重放，应逐位相同），结论在 metrics.csv 的 replay_check。",
    "同时做阳性对照（修坯「手动」照填自动链实际值重放，应复现自动链：最大样本差 ≤ −180 dBFS，不要求逐位），结论在 metrics.csv 的 replay_check。"))
changes.append((MAIN, "② RenderMain:55 --selftest 注释",
    "GLAZE 0 逐位直通、同决策重放逐位复现），不读歌；",
    "GLAZE 0 逐位直通、同决策重放复现（最大样本差 ≤ −180 dBFS）），不读歌；"))
changes.append((MAIN, "② RenderMain:1671 重放注释",
    "→ 应与自动链逐位相同（阳性对照：证明重放本身不走样，",
    "→ 应复现自动链（最大样本差 ≤ −180 dBFS，见下；阳性对照：证明重放本身不走样，"))
changes.append((MAIN, "② RenderMain:494 --help",
    "出一行 clay-replay-off，并做逐位阳性对照；ADR-016）\\n\"",
    "出一行 clay-replay-off，并做阳性对照；ADR-016）\\n\"\n"
    "                     \"                      （阳性对照的判据：重放与自动链最大样本差 ≤ −180 dBFS，不要求逐位）\\n\""))
changes.append((MAIN, "② RenderMain:2524 metrics.md 口径",
    "应与「自动」逐位相同（metrics.csv 的 replay_check：ok / mismatch / n/a）。\"",
    "应复现「自动」（最大样本差 ≤ −180 dBFS，不要求逐位；metrics.csv 的 replay_check：ok / mismatch / n/a）。\""))

# ---------- ③ 第 6 项：--trace 的修坯列 ----------
changes.append((README, "③ README:14",
    "- **修坯「自动」**= 高级抽屉的默认（低切 / 收单 / 加宽三项都「自动」，由 GLAZE 决策 + 实时保护给值）；",
    "- **修坯「自动」**= 高级抽屉的默认（低切 / 收单 / 加宽三项都「自动」：低切与加宽由 GLAZE 决策 + 实时保护给值；收单 ADR-015 起恒为 0，低频反相时只亮提示、不改声音）；"))
changes.append((README, "③ README:78 ①",
    "修坯方案与保护状态、分析器慢路 / 快路低频相关度",
    "修坯方案值 / 宽度保护后的值 / 实际值与反相提示状态、分析器慢路 / 快路低频相关度"))
changes.append((README, "③ README:78 ②",
    "（诊断用；列见下「逐块追踪的校准列」；",
    "（诊断用；列见下「逐块追踪的修坯列」「逐块追踪的校准列」；"))
changes.append((README, "③ README 新增「逐块追踪的修坯列」一节",
    "### 逐块追踪的校准列（`--trace`）",
    "### 逐块追踪的修坯列（`--trace`）\n"
    "\n"
    "只在第二遍那份（`trace/<成品名>.csv`）里，一行 = 一块：\n"
    "\n"
    "| 列 | 含义 |\n"
    "|---|---|\n"
    "| `planned_lowcut_hz` / `planned_mono_amount` / `planned_mono_hz` / `planned_width_db` | 决策器给「自动」态的值（含方案过渡混合与压力反馈缩放，ClayGuard 之前） |\n"
    "| `live_mono_amount` / `live_width_db` | ClayGuard 之后、三态合成之前的自动值：宽度保护只会把加宽往小里改，收单原样透传 |\n"
    "| `guard_phase_floor` / `guard_phase_hold_s` / `guard_phase_active` | 反相提示的强度（0～1，含攻击 / 释放）、条件消失后剩下的保持秒数、是否亮着（= 强度 > 0）。列名沿用旧名，ADR-015 起只提示、不改声音；触发条件与时间常数见 `Source/dsp/ClayGuard.h` 文件头。这是 ClayGuard 自己的状态、不看三态：收单不在「自动」的组（`off` / `lowcut` / `width`）也可能是 1，收单灯与 `phase_guard_pct` 只算收单「自动」的组 |\n"
    "| `guard_seconds` | 这一块交给 ClayGuard 推进的有效音乐秒数（= 上一块的有效音乐时长）；为 0 时（静音、宿主旁通、停播，以及读回存档后的第一块）反相提示与宽度保护都停在原处 |\n"
    "| `rho_slow` / `rho_fast` | 分析器慢路 / 快路低频相关度（反相提示的判据；无效时空）。这是本块处理后的读数，ClayGuard 下一块才用它 |\n"
    "| `clay_lowcut_hz` / `clay_mono_amount` / `clay_mono_hz` / `clay_width_db` | 三态合成后交给 ClayModule 的实际值（同指标表的 Clay 四列） |\n"
    "\n"
    "ADR-015 起「自动」不收单：本工具各组的 `planned_mono_amount`、`live_mono_amount`、`clay_mono_amount` 恒为 0，`planned_mono_hz` 恒为 80。\n"
    "\n"
    "### 逐块追踪的校准列（`--trace`）"))
changes.append((MAIN, "③ RenderMain:162-163 注释",
    "        bool trace = false;         // --trace：每组设置另写 trace/<成品名>.csv（第二遍逐块：限幅 GR、修坯方案 / 保护 / 实时相关度、校准状态）\n"
    "                                    // 与 trace/<成品名>__listen.csv（第一遍逐块的校准状态），诊断用",
    "        bool trace = false;         // --trace：每组设置另写 trace/<成品名>.csv（第二遍逐块：限幅 GR、修坯方案值 / 宽度保护后 / 实际值、\n"
    "                                    // 反相提示、实时相关度、校准状态）与 trace/<成品名>__listen.csv（第一遍逐块的校准状态），诊断用"))
changes.append((MAIN, "③ RenderMain:489-490 --help",
    "\"  --trace             另写 trace/<成品名>.csv：第二遍逐块的限幅 GR、修坯方案 / 保护 / 实时低频相关度、校准状态；\\n\"\n"
    "                     \"                      trace/<成品名>__listen.csv：第一遍逐块的校准状态（诊断用）\\n\"",
    "\"  --trace             另写 trace/<成品名>.csv：第二遍逐块的限幅 GR、修坯方案值 / 宽度保护后 / 实际值、反相提示、\\n\"\n"
    "                     \"                      实时低频相关度、校准状态；trace/<成品名>__listen.csv：第一遍逐块的校准状态（诊断用）\\n\""))

texts = {}
bad = 0
for path, name, old, new in changes:
    text = texts.setdefault(path, path.read_text(encoding="utf-8"))
    n = text.count(old)
    print(f"{'OK ' if n == 1 else '*** '} {name}（匹配 {n} 处）")
    if n != 1:
        bad += 1
        continue
    texts[path] = text.replace(old, new, 1)
if bad:
    sys.exit(f"{bad} 条对不上，一个文件都没写")
if not dry:
    for path, text in texts.items():
        path.write_text(text, encoding="utf-8")
    print("已写入：", ", ".join(str(p) for p in texts))
