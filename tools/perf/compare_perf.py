#!/usr/bin/env python3
"""对比 celadon_bench_portable 的 TSV：基线 / 当前每轮各自相减，并看各轮一致性（T-096f；计划 §13 Q10 / Q12）。

写法沿用 docs/validation/2026-09-28-T021-perf-acceptance/compare.py：只用标准库、每轮各自相减、各轮都列出来。
只读输入、只往标准输出打 markdown，不改任何文件。DGX 上判「同机不退步」；预算（48k/32 最慢块 ≤ 15 %、96k/64 ≤ 20 %、
48k/512 均值 ≤ 6 %）只在 Mac 读数上判，这里不判。

用法：
  python3 tools/perf/compare_perf.py <run 目录>        # scripts/perf-dgx.sh 的输出：base-N.tsv 与 cur-N.tsv 按 N 配对
  python3 tools/perf/compare_perf.py --base b1.tsv b2.tsv --cur c1.tsv c2.tsv
  python3 tools/perf/compare_perf.py --selftest
退出码：0 没有「退步」/ FAIL；1 基础设施错（文件读不到、列缺、行重复、非数字、两边轮数对不上）；3 有「退步」或 FAIL
（基线有值而当前空、当前缺行）。
"""

import argparse
import csv
import io
import os
import re
import sys

# ---- 阈值（单位：百分点 = 块预算百分比之差）。None = 只列差值、不标；起点值由主会话首跑后填（计划 §13 Q10）----
STRUCT_WORST_DELTA_PP = None   # 主判：每轮「当前 − 基线」的 structWorst 都超过它 → 「退步」；只有部分轮超 → 「不一致」
AVG_DELTA_PP = None            # 均值，同上
ROUND_SPREAD_PP = None         # 同一二进制各轮 structWorst 的极差超过它 → 「各轮不一致」（这一格读数不稳）
INVOL_SWITCHES_MAX = None      # 任一轮 invol_switches 超过它 → 「有抢占」（这一格读数可疑）

REPORT_ONLY_CONFIGS = {(192000, 32)}   # 192k / 32 只报不判（计划 §13 Q10）
GATE_ESTIMATE_ROWS = False             # 估计行（分析器 + 控制层 ≈ GLAZE > 0 − GLAZE 0）是估计，只报不判

KEY_COLUMNS = ("scenario", "row_kind", "layer", "sr", "bs")
GATED_METRICS = ("structWorst", "avg")
REPORTED_METRICS = ("p99.9", "structWorst_1s", "structWorst_5ms", "thread_cpu_worst", "invol_switches")
REQUIRED_COLUMNS = KEY_COLUMNS + GATED_METRICS + REPORTED_METRICS + ("na_reason",)
ROUND_FILE = re.compile(r"^(base|cur)-(\d+)\.tsv$")

FLAG_REGRESSION = "退步"
FLAG_FAIL = "FAIL"


class InfraError(Exception):
    """基础设施错（Q12）：报错退出码 1。"""


# ---------------------------------------------------------------- 纯逻辑：解析

def parse_tsv(text, source):
    """TSV 文本 → {键: 行}。键 = (scenario, row_kind, layer, sr 整数, bs 整数)。"""
    reader = csv.DictReader(io.StringIO(text), delimiter="\t")
    header = reader.fieldnames or []
    missing = [c for c in REQUIRED_COLUMNS if c not in header]
    if missing:
        raise InfraError(f"{source}：缺列 {', '.join(missing)}")
    rows = {}
    for line_number, row in enumerate(reader, start=2):
        try:
            key = (row["scenario"], row["row_kind"], row["layer"], int(float(row["sr"])), int(row["bs"]))
        except (TypeError, ValueError) as error:
            raise InfraError(f"{source}:{line_number}：sr / bs 不是数字（{error}）") from error
        if key in rows:
            raise InfraError(f"{source}:{line_number}：重复的行 {key}")
        rows[key] = row
    return rows


def number(row, column, source="?"):
    """格子 → float；空格子（算不出，原因在 na_reason）→ None。"""
    if row is None:
        return None
    text = (row.get(column) or "").strip()
    if text == "":
        return None
    try:
        return float(text)
    except ValueError as error:
        raise InfraError(f"{source}：{column} 不是数字：{text!r}") from error


def pair_round_files(names):
    """目录里的文件名 → ([base 各轮], [cur 各轮])，按轮次号排序；没有 base 时基线为空列表。"""
    found = {"base": {}, "cur": {}}
    for name in names:
        match = ROUND_FILE.match(name)
        if match:
            found[match.group(1)][int(match.group(2))] = name
    cur_rounds = sorted(found["cur"])
    base_rounds = sorted(found["base"])
    if not cur_rounds:
        raise InfraError("没有 cur-N.tsv")
    if base_rounds and base_rounds != cur_rounds:
        raise InfraError(f"基线轮次 {base_rounds} 与当前轮次 {cur_rounds} 对不上")
    return [found["base"][r] for r in base_rounds], [found["cur"][r] for r in cur_rounds]


# ---------------------------------------------------------------- 纯逻辑：比较

def default_thresholds():
    return {
        "structWorst": STRUCT_WORST_DELTA_PP,
        "avg": AVG_DELTA_PP,
        "round_spread": ROUND_SPREAD_PP,
        "invol_max": INVOL_SWITCHES_MAX,
    }


def is_gated(key):
    scenario, row_kind, layer, sr, bs = key
    if (sr, bs) in REPORT_ONLY_CONFIGS:
        return False
    if row_kind != "measured" and not GATE_ESTIMATE_ROWS:
        return False
    return True


def ordered_keys(base_rounds, cur_rounds):
    """基线第一轮的出现顺序在前，只在当前出现的键排在后面。"""
    keys, seen = [], set()
    for rows in base_rounds + cur_rounds:
        for key in rows:
            if key not in seen:
                seen.add(key)
                keys.append(key)
    return keys


def spread(values):
    present = [v for v in values if v is not None]
    return max(present) - min(present) if len(present) >= 2 else None


def compare(base_rounds, cur_rounds, thresholds):
    """每个键一条结果：各轮基线 / 当前的行、各指标逐轮差、标记。base_rounds 与 cur_rounds 等长（按轮配对）。"""
    if len(base_rounds) != len(cur_rounds):
        raise InfraError(f"基线 {len(base_rounds)} 轮、当前 {len(cur_rounds)} 轮，对不上")
    results = []
    for key in ordered_keys(base_rounds, cur_rounds):
        base = [rows.get(key) for rows in base_rounds]
        cur = [rows.get(key) for rows in cur_rounds]
        flags = []
        gated = is_gated(key)
        if not gated:
            flags.append("只报")

        if any(b is not None for b in base) and any(c is None for c in cur):
            flags.append(f"{FLAG_FAIL}：当前缺这一行")
        if all(b is None for b in base):
            flags.append("基线没有这一行")

        deltas = {}
        for metric in GATED_METRICS + REPORTED_METRICS:
            per_round = []
            for b_row, c_row in zip(base, cur):
                b_value, c_value = number(b_row, metric, "基线"), number(c_row, metric, "当前")
                if b_value is not None and c_row is not None and c_value is None:
                    flags.append(f"{FLAG_FAIL}：{metric} 当前空（基线有值）")
                per_round.append(c_value - b_value if b_value is not None and c_value is not None else None)
            deltas[metric] = per_round

        for metric in GATED_METRICS:
            threshold = thresholds.get(metric)
            present = [d for d in deltas[metric] if d is not None]
            if not gated or threshold is None or not present:
                continue
            exceeded = [d > threshold for d in present]
            if all(exceeded) and len(present) == len(deltas[metric]):
                flags.append(f"{FLAG_REGRESSION}：{metric}")
            elif any(exceeded):
                flags.append(f"不一致：{metric}")

        spread_limit = thresholds.get("round_spread")
        if spread_limit is not None:
            for label, rows in (("基线", base), ("当前", cur)):
                s = spread([number(r, "structWorst", label) for r in rows])
                if s is not None and s > spread_limit:
                    flags.append(f"各轮不一致（{label}）")

        invol_limit = thresholds.get("invol_max")
        if invol_limit is not None:
            invol = [number(r, "invol_switches") for r in base + cur]
            if any(v is not None and v > invol_limit for v in invol):
                flags.append("有抢占")

        flags = list(dict.fromkeys(flags))   # 同一标记各轮可能各报一次：去重、保留先后
        results.append({"key": key, "base": base, "cur": cur, "deltas": deltas, "flags": flags, "gated": gated})
    return results


def exit_code_for(results):
    bad = any(f.startswith(FLAG_FAIL) or f.startswith(FLAG_REGRESSION) for r in results for f in r["flags"])
    return 3 if bad else 0


# ---------------------------------------------------------------- 纯逻辑：输出

def fmt(value, signed=False):
    if value is None:
        return "—"
    return f"{value:+.3f}" if signed else f"{value:.3f}"


def fmt_rounds(values):
    return " / ".join(fmt(v, signed=True) for v in values)


def key_cells(key):
    scenario, row_kind, layer, sr, bs = key
    return [scenario, row_kind, layer, str(sr), str(bs)]


def mentions(flag, metric):
    """标记里是否点名了这个指标（整词：structWorst 不算点名 structWorst_1s）。"""
    return re.search(rf"(?<![\w.]){re.escape(metric)}(?![\w.])", flag) is not None


def flags_for_metric(result, metric):
    """这张表该显示的标记：点名本指标的，加上不点名任何指标的（缺行、抢占、只报……）。"""
    all_metrics = GATED_METRICS + REPORTED_METRICS
    return [f for f in result["flags"] if mentions(f, metric) or not any(mentions(f, m) for m in all_metrics)]


def metric_table(results, metric):
    lines = [
        f"| 场景 | 行 | 层 | sr | bs | 基线 r1 | 当前 r1 | Δ 各轮 | 极差 基线 / 当前 | 标记 |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        b1 = number(r["base"][0], metric) if r["base"] else None
        c1 = number(r["cur"][0], metric) if r["cur"] else None
        spreads = f"{fmt(spread([number(x, metric) for x in r['base']]))} / {fmt(spread([number(x, metric) for x in r['cur']]))}"
        flags = "；".join(flags_for_metric(r, metric))
        lines.append("| " + " | ".join(key_cells(r["key"]) + [fmt(b1), fmt(c1), fmt_rounds(r["deltas"][metric]), spreads, flags]) + " |")
    return lines


def reported_table(results):
    lines = [
        "| 场景 | 行 | 层 | sr | bs | Δ p99.9 | Δ structWorst_1s | Δ structWorst_5ms | Δ thread_cpu_worst | invol 基线 / 当前（各轮） |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        invol_base = " / ".join(fmt(number(x, "invol_switches")) for x in r["base"])
        invol_cur = " / ".join(fmt(number(x, "invol_switches")) for x in r["cur"])
        cells = [fmt_rounds(r["deltas"][m]) for m in ("p99.9", "structWorst_1s", "structWorst_5ms", "thread_cpu_worst")]
        lines.append("| " + " | ".join(key_cells(r["key"]) + cells + [f"{invol_base} ‖ {invol_cur}"]) + " |")
    return lines


def render_markdown(results, base_names, cur_names, thresholds):
    def shown(value):
        return "未定（只列不判）" if value is None else f"{value} 个百分点"

    lines = [
        "# CPU 基线对比（tools/perf/compare_perf.py）",
        "",
        f"- 基线：{', '.join(base_names)}",
        f"- 当前：{', '.join(cur_names)}",
        f"- 轮数：{len(cur_names)}；Δ = 当前 − 基线（百分点，按轮配对各自相减）；单位：块预算的百分比",
        f"- 阈值：structWorst {shown(thresholds.get('structWorst'))}；avg {shown(thresholds.get('avg'))}；"
        f"各轮极差 {shown(thresholds.get('round_spread'))}；invol_switches 上限 "
        f"{'未定' if thresholds.get('invol_max') is None else thresholds.get('invol_max')}",
        f"- 只报不判：{', '.join(f'{sr}/{bs}' for sr, bs in sorted(REPORT_ONLY_CONFIGS))}；估计行"
        f"{'也判' if GATE_ESTIMATE_ROWS else '（row_kind = estimate）'}；p99.9、按控制层网格分组、线程 CPU、切换次数",
        "",
        "## 主判：structWorst（结构性最坏块，按 Balance hop 相位）",
        "",
        *metric_table(results, "structWorst"),
        "",
        "## avg（每块均值）",
        "",
        *metric_table(results, "avg"),
        "",
        "## 只报不判",
        "",
        *reported_table(results),
        "",
        "## 标记汇总",
        "",
    ]
    flagged = [r for r in results if any(f != "只报" for f in r["flags"])]
    if not flagged:
        lines.append("没有标记。")
    for r in flagged:
        lines.append(f"- {' / '.join(key_cells(r['key']))}：{'；'.join(f for f in r['flags'] if f != '只报')}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- 读文件（副作用）

def load_tsv(path):
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as error:
        raise InfraError(f"读不到 {path}：{error}") from error
    return parse_tsv(text, path)


def files_from_run_dir(run_dir):
    try:
        names = os.listdir(run_dir)
    except OSError as error:
        raise InfraError(f"读不到目录 {run_dir}：{error}") from error
    base, cur = pair_round_files(names)
    if not base:
        raise InfraError(f"{run_dir} 里没有 base-N.tsv（只跑了当前？对比要基线）")
    return [os.path.join(run_dir, n) for n in base], [os.path.join(run_dir, n) for n in cur]


# ---------------------------------------------------------------- 自检（已知答案的合成用例）

SELFTEST_HEADER = "\t".join(KEY_COLUMNS + GATED_METRICS + REPORTED_METRICS + ("na_reason",))


def synthetic_tsv(rows):
    """rows：[(scenario, row_kind, layer, sr, bs, structWorst, avg, invol)]，其余只报列填 1.0。"""
    lines = [SELFTEST_HEADER]
    for scenario, row_kind, layer, sr, bs, struct_worst, avg, invol in rows:
        cells = [scenario, row_kind, layer, str(sr), str(bs), struct_worst, avg, "1.0", "1.0", "1.0", "1.0", invol, ""]
        lines.append("\t".join(cells))
    return "\n".join(lines) + "\n"


def run_selftest():
    failures = []

    def check(ok, name):
        print(f"SELFTEST,{'PASS' if ok else 'FAIL'},{name}")
        if not ok:
            failures.append(name)

    def rows_for(round_index, side):
        # 每个键：(基线 r1, 基线 r2, 当前 r1, 当前 r2) 的 structWorst；avg 固定 1.0；invol 固定 0（I 例外）。
        table = {
            ("A", "measured", "chain", 48000, 32): ("10.0", "10.2", "10.7", "10.8"),   # Δ 0.7 / 0.6 → 退步
            ("B", "measured", "chain", 48000, 512): ("1.0", "1.0", "1.1", "1.0"),     # Δ 0.1 / 0.0 → 无
            ("C", "measured", "chain", 96000, 64): ("5.0", "5.0", "5.7", "5.1"),      # Δ 0.7 / 0.1 → 不一致
            ("D", "measured", "chain", 48000, 64): ("3.0", "3.0", "", "3.0"),         # 当前空 → FAIL
            ("F", "measured", "chain", 192000, 32): ("15.0", "15.0", "17.0", "17.0"), # 192k/32 只报
            ("G", "estimate", "chain", 48000, 32): ("1.0", "1.0", "3.0", "3.0"),      # 估计行只报
            ("H", "measured", "processor", 48000, 32): ("10.0", "11.0", "10.0", "11.0"),  # 基线各轮极差 1.0
            ("I", "measured", "processor", 96000, 32): ("4.0", "4.0", "4.0", "4.0"),  # invol 50 → 有抢占
        }
        column = {("base", 0): 0, ("base", 1): 1, ("cur", 0): 2, ("cur", 1): 3}[(side, round_index)]
        out = []
        for (scenario, row_kind, layer, sr, bs), values in table.items():
            invol = "50" if scenario == "I" and side == "cur" else "0"
            out.append((scenario, row_kind, layer, sr, bs, values[column], "1.0", invol))
        if side == "base":
            out.append(("E", "measured", "chain", 44100, 32, "2.0", "1.0", "0"))   # 当前没有这一行 → FAIL
        return parse_tsv(synthetic_tsv(out), f"{side}-{round_index + 1}")

    base = [rows_for(0, "base"), rows_for(1, "base")]
    cur = [rows_for(0, "cur"), rows_for(1, "cur")]
    thresholds = {"structWorst": 0.5, "avg": 0.1, "round_spread": 0.5, "invol_max": 10}
    results = {r["key"][0]: r for r in compare(base, cur, thresholds)}

    def has(name, prefix):
        return any(f.startswith(prefix) for f in results[name]["flags"])

    check(has("A", f"{FLAG_REGRESSION}：structWorst"), "both rounds over threshold -> regression")
    check(abs(results["A"]["deltas"]["structWorst"][0] - 0.7) < 1e-9 and abs(results["A"]["deltas"]["structWorst"][1] - 0.6) < 1e-9,
          "deltas are paired per round (cur r - base r)")
    check(not any(f != "只报" for f in results["B"]["flags"]) and results["B"]["flags"] == [], "small deltas -> no flag")
    check(has("C", "不一致：structWorst") and not has("C", FLAG_REGRESSION), "one round over threshold -> inconsistent, not regression")
    check(has("D", f"{FLAG_FAIL}：structWorst 当前空"), "baseline has a value but current is empty -> FAIL (Q12)")
    check(has("E", f"{FLAG_FAIL}：当前缺这一行"), "row missing in current -> FAIL")
    check("只报" in results["F"]["flags"] and not has("F", FLAG_REGRESSION), "192k/32 is report-only")
    check("只报" in results["G"]["flags"] and not has("G", FLAG_REGRESSION), "estimate rows are report-only")
    check(has("H", "各轮不一致（基线）") and has("H", "各轮不一致（当前）"), "round spread over limit is flagged for each side")
    check(has("I", "有抢占"), "involuntary switches over limit are flagged")
    check(exit_code_for(list(results.values())) == 3, "FAIL or regression -> exit code 3")

    untouched = compare(base, cur, {"structWorst": None, "avg": None, "round_spread": None, "invol_max": None})
    check(not any(f.startswith(FLAG_REGRESSION) or f.startswith("不一致") for r in untouched for f in r["flags"]),
          "thresholds left as None only list the deltas")
    clean = compare([base[0]], [base[0]], thresholds)
    check(exit_code_for(clean) == 0 and all(r["deltas"]["structWorst"][0] in (0.0, None) for r in clean),
          "a file compared with itself has zero deltas and exit code 0")

    try:
        parse_tsv("scenario\trow_kind\tlayer\tsr\tbs\n", "bad")
        check(False, "missing columns raise an infrastructure error")
    except InfraError:
        check(True, "missing columns raise an infrastructure error")
    try:
        duplicate = synthetic_tsv([("A", "measured", "chain", 48000, 32, "1", "1", "0")] * 2)
        parse_tsv(duplicate, "dup")
        check(False, "duplicate rows raise an infrastructure error")
    except InfraError:
        check(True, "duplicate rows raise an infrastructure error")

    check(pair_round_files(["machine.txt", "cur-2.tsv", "base-1.tsv", "cur-1.tsv", "base-2.tsv"])
          == (["base-1.tsv", "base-2.tsv"], ["cur-1.tsv", "cur-2.tsv"]), "run directory files pair by round number")
    try:
        pair_round_files(["base-1.tsv", "cur-1.tsv", "cur-2.tsv"])
        check(False, "unmatched rounds raise an infrastructure error")
    except InfraError:
        check(True, "unmatched rounds raise an infrastructure error")

    check(mentions("FAIL：structWorst_1s 当前空（基线有值）", "structWorst_1s")
          and not mentions("FAIL：structWorst_1s 当前空（基线有值）", "structWorst")
          and mentions("退步：structWorst", "structWorst"), "flags are attributed to metrics by whole word")

    text = render_markdown(list(results.values()), ["base-1", "base-2"], ["cur-1", "cur-2"], thresholds)
    check("## 主判：structWorst" in text and "| 场景 |" in text and f"{FLAG_REGRESSION}：structWorst" in text,
          "markdown report has the main table and the flags")

    print(f"SELFTEST,{'OK' if not failures else 'FAILED'},{len(failures)} failure(s)")
    return 0 if not failures else 1


# ---------------------------------------------------------------- 入口

def main(argv):
    parser = argparse.ArgumentParser(description="对比 celadon_bench_portable 的 TSV（基线 / 当前）")
    parser.add_argument("run_dir", nargs="?", help="scripts/perf-dgx.sh 的 run 目录（base-N.tsv / cur-N.tsv）")
    parser.add_argument("--base", nargs="+", help="基线各轮 TSV（按轮次顺序）")
    parser.add_argument("--cur", nargs="+", help="当前各轮 TSV（按轮次顺序）")
    parser.add_argument("--selftest", action="store_true", help="跑已知答案的合成用例")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    try:
        if args.run_dir and (args.base or args.cur):
            raise InfraError("run 目录与 --base / --cur 二选一")
        if args.run_dir:
            base_paths, cur_paths = files_from_run_dir(args.run_dir)
        elif args.base and args.cur:
            base_paths, cur_paths = args.base, args.cur
        else:
            parser.print_usage(sys.stderr)
            return 1
        base = [load_tsv(p) for p in base_paths]
        cur = [load_tsv(p) for p in cur_paths]
        thresholds = default_thresholds()
        results = compare(base, cur, thresholds)
    except InfraError as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1

    sys.stdout.write(render_markdown(results, base_paths, cur_paths, thresholds))
    return exit_code_for(results)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
