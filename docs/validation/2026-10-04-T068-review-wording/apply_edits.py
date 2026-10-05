#!/usr/bin/env python3
"""T-068：夜间 5 代理评审（wf_3e784f42-2fa）里「T-061 / T-066 措辞」与「T-059 / T-065 渲染工具说明」两份评审的回修。
每条改动都先断言原文在目标文件里恰好出现一次才套用；用法：在仓库根执行 `python3 docs/validation/2026-10-04-T068-review-wording/apply_edits.py`。
只动文案、注释与一处运行时说明字符串（metrics.md 的口径句、n/a 备注）；判定逻辑、数值与声音不变。"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
EDITS = []


def sub(path, old, new):
    EDITS.append((path, old, new))


# ───────────── 官网说明页：手动低切 / 收单被峰值守护回收的量（F1）+ 加宽的两层把关（F2）+ 低切频率末句（F7）+ 英文动词（F8）
ZC, EC = "site/src/content/docs/zh/clay.md", "site/src/content/docs/en/clay.md"
sub(ZC, "加宽不经守护，自动加宽由先试后用把关。",
        "加宽不经峰值守护，自动加宽由先试后用把关，运行中还有宽度保护盯着相关度。")
sub(ZC, "（峰值守护照常：手动的低切 / 收单在会抬峰值的瞬间让开一点，手动值本身不变；加宽不经守护）。「手动」时数值可拖 |",
        "（峰值守护照常：手动的低切 / 收单在会抬峰值的瞬间会被收回，手动值本身不变；低音宽而不相关、峰值密集的歌上收回的可能不少，界面不会另行提示；加宽不经峰值守护）。「手动」时数值可拖 |")
sub(ZC, "抬高峰值的瞬间峰值守护会把低切让回去，不让限幅器多压。 |",
        "抬高峰值的瞬间峰值守护会把低切让回去，不让限幅器多压；让得太频繁会削弱低音，所以自动会把频率压低或不切。 |")
sub(EC, "Width doesn't go through the guard, and automatic Width is kept in check by the trial run.",
        "Width doesn't go through the peak guard; automatic Width is kept in check by the trial run and, while playing, by a width guard that watches correlation.")
sub(EC, "(the peak guard still applies: manual Low cut and Mono step aside at moments that would raise peaks, without changing your value; Width is not guarded)",
        "(the peak guard still applies: manual Low cut and Mono are pulled back at moments that would raise peaks while your value stays as set — on dense material with wide, uncorrelated lows a lot of the effect can be pulled back, and the interface does not flag it; Width does not go through the peak guard)")
sub(EC, "the peak guard yields the low cut so the limiter does not work harder. |",
        "the peak guard yields the low cut so the limiter does not work harder; yielding too often thins the low end, so Auto lowers the frequency or skips the cut. |")
sub(EC, "(Auto does not mono, so no mono frequency is shown here)",
        "(Auto never folds the lows to mono, so no mono frequency is shown here)")

# 算法页：峰值守护段与三态语义（F1）
AZ, AE = "site/src/content/algorithms/zh/clay.md", "site/src/content/algorithms/en/clay.md"
sub(AZ, "守护对手动的低切 / 收单也生效（手动值不被改写，只在峰值处让一点）。",
        "守护对手动的低切 / 收单也生效（手动值不被改写，只在会抬峰值处收回它的效果；峰值密集、低音宽而不相关的歌上收回的可能不少，界面不会另行提示）。")
sub(AZ, "（峰值守护照常：手动的低切 / 收单在会抬峰值的瞬间让开一点，手动值本身不变）。",
        "（峰值守护照常：手动的低切 / 收单在会抬峰值的瞬间会被收回，手动值本身不变；密集素材上收回的可能不少）。")
sub(AE, "(the manual value isn't rewritten; it just yields a little at the peaks)",
        "(the manual value isn't rewritten; the guard pulls its effect back at moments that would raise a peak — on dense material with wide, uncorrelated lows that can be a lot, and the interface does not flag it)")
sub(AE, "(the peak guard still applies: manual Low cut and Mono step aside at moments that would raise peaks, without changing your value).",
        "(the peak guard still applies: manual Low cut and Mono are pulled back at moments that would raise peaks while your value stays as set; on dense material that can be a lot).")

# GLAZE 为 0 时反相提示也不亮（F3）
sub("site/src/content/docs/zh/glaze.md", "收单在「自动」下从不收，只在低频反相时亮琥珀灯提醒；「手动」时",
    "收单在「自动」下从不收，只在低频反相时亮琥珀灯提醒（GLAZE 为 0 时连提醒也不亮）；「手动」时")
sub("site/src/content/docs/en/glaze.md", "it only lights an amber warning when they are out of phase; on Manual they work even at GLAZE 0",
    "it only lights an amber warning when they are out of phase (at GLAZE 0 even the warning stays off); on Manual they work even at GLAZE 0")

# 首页修坯卡片：「只动 S」只对收单与加宽成立；中文 lead 拆开两层意思（F4）
sub("site/src/i18n/locales/zh/home.json", "自动的改动先在这首歌上试一遍，低切与收单会抬峰值就自动让开。",
    "自动的改动先在这首歌上试一遍；低切与收单一旦会抬高峰值，会自动让开。")
sub("site/src/i18n/locales/zh/home.json", "\"只动 S：单声道回放完全不变\",",
    "\"收单与加宽只动 S：单声道回放不变（低切只删次声）\",")
sub("site/src/i18n/locales/en/home.json", "\"Side channel only: mono playback is untouched\",",
    "\"Mono and Width touch only the side channel: mono playback is unchanged (Low cut removes only sub-bass)\",")

# ───────────── 插件：灯条目首句、英文动词（与官网同步）+ 注释
HT = "Source/ui/HelpText.cpp"
sub(HT, "加宽不经守护，自动加宽由先试后用把关。\",",
        "加宽不经峰值守护，自动加宽由先试后用把关，运行中还有宽度保护盯着相关度。\",")
sub(HT, "width is not guarded, and automatic width is kept in check by the trial.\",",
        "width is not run through the peak guard; automatic width is kept in check by the trial and, while playing, by a width guard that watches correlation.\",")
sub(HT, "(Auto does not mono, so no mono frequency is shown here)",
        "(Auto never folds the lows to mono, so no mono frequency is shown here)")
sub(HT, "// 「自动」时显示此刻的实际值、手动值不生效。三态语义",
        "// 「自动」时低切 / 加宽显示此刻的实际值（为 0 显示「未启用」）、收单一行恒显示「未启用」（自动不收单），手动值都不生效。三态语义")
sub(HT, "// 界面上从不显示（只作官网说明页首句的同源底稿，T-061）；第 3 行照常显示",
        "// 界面上从不显示（官网说明页首句手工与它保持一致，T-061，没有自动核对）；第 3 行照常显示")
sub("Source/ui/HelpText.h",
    "// 一盏灯的颜色类别：暗（35% 灰青，没在处理）/ 亮（梅子青，实际值非 0）/ 保护（琥珀，该级的宽度保护正在介入或低频反相提示亮着）。\n"
    "    // 界面按 palette::textDim × comingSoonAlpha / accent / reduction 画（琥珀与 GR 表同色：「在压 / 保护中 / 提示中」都表示要留意）。",
    "// 一盏灯的颜色类别：暗（灰圈，没在处理）/ 亮（实际值非 0：「自动」紫圈、「手动」青色实心）/ 保护（橙色实心，该级的宽度保护正在介入或低频反相提示亮着）。\n"
    "    // 界面按 palette::lampIdle / violet / c600 / warning 画（ModuleCards.cpp 的 paint）；橙色表示要留意：「保护中 / 提示中」。")
sub("Source/dsp/GlazeController.h",
    "ClayPlan clayPlanned;            // 上一块方案（含过渡混合）给的「自动」态值（保护之前；方案源不支持 Clay 时为中性）",
    "ClayPlan clayPlanned;            // 上一块方案（含过渡混合与压力反馈缩放）给的「自动」态值（保护之前；方案源不支持 Clay 时为中性）")
sub("Source/dsp/GlazeController.h",
    "Feature liveLowCorrelation;          // 分析器实时视图的低频相关度慢路 / 快路（上一块交给 ClayGuard 的那一份；诊断追踪用）",
    "Feature liveLowCorrelation;          // 分析器实时视图的低频相关度慢路 / 快路（读取此刻的读数：处理完这一块之后，ClayGuard 下一块才用它；诊断追踪用）")

# ───────────── 测试：文案断言跟着改（旧断言钉着「加宽不经守护」与英文 width is not guarded）
HTT = "tests/HelpTextTests.cpp"
sub(HTT, "中英文都要讲到「加宽不经守护」「低切 / 收单在会抬峰值的瞬间让开」。",
         "中英文都要讲到「加宽不经峰值守护」「低切 / 收单在会抬峰值的瞬间让开」。")
sub(HTT, "&& utf8Contains (utf8 (indicator->effect), \"加宽不经守护\"),\n"
         "                            \"indicator: the guard covers low cut and mono, width is not guarded\");",
         "&& utf8Contains (utf8 (indicator->effect), \"加宽不经峰值守护\"),\n"
         "                            \"indicator: the guard covers low cut and mono, width is not run through the peak guard\");")
sub(HTT, "&& juce::String (indicator->effectEn).contains (\"width is not guarded\")",
         "&& juce::String (indicator->effectEn).contains (\"width is not run through the peak guard\")")
sub(HTT, "                            \"indicator: only the automatic width goes through the trial (a manual width is what you set)\");\n",
         "                            \"indicator: only the automatic width goes through the trial (a manual width is what you set)\");\n"
         "                    // T-068（评审 F2）：自动加宽运行中还有宽度保护（盯相关度，真会改声音、会点亮琥珀加宽灯），首句不能只提先试后用。\n"
         "                    expect (utf8Contains (utf8 (indicator->effect), \"运行中还有宽度保护盯着相关度\")\n"
         "                              && juce::String (indicator->effectEn).contains (\"while playing, by a width guard that watches correlation\"),\n"
         "                            \"indicator: automatic width also has the live width guard\");\n")
sub(HTT, "juce::String (entry->effectEn).contains (\"Auto does not mono, so no mono frequency is shown here\")",
         "juce::String (entry->effectEn).contains (\"Auto never folds the lows to mono, so no mono frequency is shown here\")")

# ───────────── 渲染工具：注释与 metrics.md 口径句（F2、F3、F4、F5、F12）
RM = "tools/render/RenderMain.cpp"
sub(RM, "包括 GLAZE 0 不切片这一点）。手动收单固定是「比例 1」，自动收单比例在 0、1 之间的块表达不了，对照记「不适用」。",
        "包括 GLAZE 0 不切片这一点）。手动收单固定是「比例 1」，收单比例在 0、1 之间的块「手动」态表达不了（ADR-015 起自动收单恒为 0，\n"
        "    //     这一种已不出现）；另一种原因是目标频谱用了「参考」形状、重放链没有侧链参考（本工具的目标频谱固定是出厂「自动」，也走不到）。\n"
        "    //     两种都记「不适用」。")
sub(RM, "+ std::string (! clayExpressible ? \"自动收单比例在 0、1 之间，「手动」态表达不了（手动收单固定是 1）\"",
        "+ std::string (! clayExpressible ? \"有块的修坯实际值「手动」态表达不了（收单比例不是 0 或 1，或低切 / 加宽越界；手动收单固定是 1）\"")
# 余振秒数与重放容差在口径句里从常量生成（同一处真源，常量改了说明不会漂）
sub(RM, "\"（已去掉链延迟、含 0.35 s 余振）。\\n\"",
        "\"（已去掉链延迟、含 \" + fixed (ringOutSeconds, 2) + \" s 余振）。\\n\"")
sub(RM, "应复现「自动」（最大样本差 ≤ −180 dBFS，不要求逐位；metrics.csv 的 replay_check：ok / mismatch / n/a）。\"",
        "应复现「自动」（最大样本差 ≤ −\" + fixed (-toDbfs (replayTolerance), 0) + \" dBFS，不要求逐位；metrics.csv 的 replay_check：ok / mismatch / n/a）。\"")
sub(RM, "\"后从头渲染第二遍作成品；丢掉开头的链延迟、末尾多冲 0.35 s 余振，成品与原曲逐样本对齐\\n\";",
        "\"后从头渲染第二遍作成品；丢掉开头的链延迟、末尾多冲 \" + fixed (ringOutSeconds, 2) + \" s 余振，成品与原曲逐样本对齐\\n\";")
sub(RM, "\" dBFS（允许 ≤ −180 dBFS）\"))",
        "\" dBFS（允许 ≤ −\" + fixed (-toDbfs (replayTolerance), 0) + \" dBFS）\"))")
sub(RM, "clayWidthDb（上一块交给 ClayModule 的实际值）；低切「关」= 0；收单量为 0 时收单 Hz 无意义。\\n\"",
        "clayWidthDb（上一块交给 ClayModule 的实际值）；低切「关」= 0；收单量为 0 时收单 Hz 无意义（本工具不用手动收单，ADR-015 起收单 Hz 恒为 80、收单量恒为 0）。\\n\"")
sub(RM, "第二遍里反相提示亮着（收单灯琥珀；ADR-015 起只提示、不改声音）/ 宽度保护介入的时长占比（每块读一次状态）。\"",
        "第二遍里反相提示亮着（收单灯琥珀；ADR-015 起只提示、不改声音；只在收单为「自动」的组计：auto、mono 组，其余组与重放行恒为 0）/ \"\n"
        "               \"宽度保护介入的时长占比（只在加宽为「自动」的组计；每块读一次状态）。\"")

GC = "tools/render/gate_check.py"
sub(GC, "CEILING_DBTP = -1.0\n",
        "CEILING_DBTP = -1.0\n"
        "# 同决策重放阳性对照的容差（dBFS）：与 RenderMain.cpp 的 replayTolerance（1e-9 = −180 dBFS）同值，改一处要同步改另一处\n"
        "REPLAY_TOLERANCE_DBFS = -180\n")
sub(GC, "最大样本差 {diff} dBFS，允许 ≤ −180），修坯门槛对照不可信\")",
        "最大样本差 {diff} dBFS，允许 ≤ −{abs(REPLAY_TOLERANCE_DBFS)}），修坯门槛对照不可信\")")

# ───────────── tools/render/README.md（F1、F4、F5、F6、F7、F8、F9、F10）
RD = "tools/render/README.md"
sub(RD, "把 3～5 首风格各异的真歌（至少一首低音很宽 / 有反相内容）按设置网格过整条链离线渲染，",
        "把 3～5 首风格各异的真歌（至少一首低频有反相段，用来核对反相提示误亮与门槛里的豁免；最好再有一首低音很宽的，用来看低切与加宽）按设置网格过整条链离线渲染，")
sub(RD, "低切 / 收单 / 加宽三项都「自动」：低切与加宽由 GLAZE 决策 + 实时保护给值；收单 ADR-015 起恒为 0，低频反相时只亮提示、不改声音）；",
        "低切 / 收单 / 加宽三项都「自动」：低切由 GLAZE 决策给值（含代价封顶与压力反馈缩放），加宽由决策 + 宽度保护给值；收单 ADR-015 起恒为 0，低频反相时只亮提示、不改声音）；")
sub(RD, "、校准状态；另写 `trace/<成品名>__listen.csv`：第一遍逐块的校准状态（诊断用；列见下「逐块追踪的修坯列」「逐块追踪的校准列」；一首 3 分钟的歌两份合计约 4 MB） |",
        "、校准状态（列见下「逐块追踪的修坯列」「逐块追踪的校准列」）；另写 `trace/<成品名>__listen.csv`：第一遍逐块的校准状态（诊断用；只有 `t_sec`、`limit_gain_db` 和校准列，见下「逐块追踪的校准列」；一首 3 分钟的歌两份合计约 4 MB） |")
sub(RD, "；低切 0 =「关」；收单量 0 时收单 Hz 无意义 |",
        "；低切 0 =「关」；收单量 0 时收单 Hz 无意义（本工具不用手动收单，ADR-015 起收单 Hz 恒为 80、收单量恒为 0） |")
sub(RD, "第二遍里反相提示亮着（收单灯琥珀；ADR-015 起只提示、不改声音）/ 宽度保护介入的时长占比；`phase_guard_pct` 沿用旧列名（改名会断旧指标表） |",
        "第二遍里反相提示亮着（收单灯琥珀；ADR-015 起只提示、不改声音；只在收单为「自动」的组计：`auto`、`mono` 组，其余组与重放行恒为 0）/ 宽度保护介入的时长占比（只在加宽为「自动」的组计）；`phase_guard_pct` 沿用旧列名（改名会断旧指标表） |")
sub(RD, "诊断：试用只看聆听的前 15 ～ 30 s） |",
        "诊断：试用只看聆听的前 15 ～ 30 s；`trial_mono*` 四列 ADR-015 起照测照存、决策不用，仅作诊断） |")
sub(RD, "| `guard_seconds` | 这一块交给 ClayGuard 推进的有效音乐秒数（= 上一块的有效音乐时长）；为 0 时（静音、宿主旁通、停播，以及读回存档后的第一块）反相提示与宽度保护都停在原处 |",
        "| `guard_seconds` | 这一块交给 ClayGuard 推进的有效音乐秒数（= 上一块的有效音乐时长）。为 0 时：静音、宿主旁通、停播，以及读回存档后的第一块——反相提示与宽度保护的状态停在原处；GLAZE 0 的组、或方案源不支持自动 Clay（旧画像 / 没有画像）每块都是 0，且 ClayGuard 不是停住而是回到中性（`guard_*` 与 `planned_*` / `live_*` 全是中性值，哪怕 `rho_slow` 明明 < −0.2） |")
sub(RD, "ADR-015 起「自动」不收单：本工具各组的 `planned_mono_amount`、`live_mono_amount`、`clay_mono_amount` 恒为 0，`planned_mono_hz` 恒为 80。\n",
        "ADR-015 起「自动」不收单：本工具各组的 `planned_mono_amount`、`live_mono_amount`、`clay_mono_amount` 恒为 0，`planned_mono_hz` 恒为 80。\n"
        "\n"
        "低切不经 ClayGuard 改动，所以没有 `live_lowcut_hz` 列：`planned_lowcut_hz` 就是交给三态合成的自动低切。`guard_phase_hold_s` 在反相条件成立期间保持为整段保持期（出厂 2 s），条件消失后才开始倒数。\n"
        "峰值守护与压力反馈的列（`peak_guard_samples`、`peak_guard_min_gain`、`blend_intended_energy`、`blend_output_energy`、`clay_pressure_scale`、`clay_peak_cost_db`、`clay_peak_cost_blocks`、`clay_blend_loss_db`）不在这一节，口径见上面 `--trace` 一行与指标表里的同名列。\n")


def main():
    by_file = {}
    for path, old, new in EDITS:
        by_file.setdefault(path, []).append((old, new))
    failures = []
    updated = {}
    for path, edits in by_file.items():
        text = (ROOT / path).read_text(encoding="utf-8")
        for old, new in edits:
            n = text.count(old)
            if n != 1:
                failures.append(f"{path}: 原文出现 {n} 次（应为 1）：{old[:70]!r}")
                continue
            text = text.replace(old, new)
        updated[path] = text
    if failures:
        print("\n".join(failures))
        sys.exit(1)
    for path, text in updated.items():
        (ROOT / path).write_text(text, encoding="utf-8")
    print(f"套用 {len(EDITS)} 条改动，涉及 {len(updated)} 个文件：")
    for path in updated:
        print("  ", path)


main()
