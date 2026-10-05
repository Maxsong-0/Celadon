#!/usr/bin/env python3
"""把 redgreen_run.sh 的输出汇成 T-070 的红绿表。用法：redgreen_summary_t070.py <redgreen 输出目录>
列 = --only check 里的 25 条频谱仪检查（T-051 的 S8，T-058 的 S9 ×2 斜率、播放、S10a/b/c，T-070 新增的 S9b ×2、S11、S12a/b/c/d、四个探针 + 两个悬停读数）。"""
import pathlib
import re
import sys

NAMES = {
    "V0": "T-070（改坏之前）",
    "V1": "T-058 撤 ①（paint 里重建后不并旧区域）", "V2": "T-058 撤 ②（curveRegion 只在开着时算峰值线 / IN 线）",
    "V3": "T-058 撤 ② 与打开时的重建", "V4": "T-058 撤打开时的重建", "V5": "T-058 撤 PEAK 关时每帧拷峰值",
    "V6": "T-058 照旧比峰值线", "V7": "T-058 撤「重建先换成此刻的值」", "V8": "T-058 撤 repaintCurves 并旧区域",
    "V9": "T-058 curveRegion 不算峰值线", "V10": "T-058 curveRegion 不按描边外扩", "V11": "T-058 撤「悬停读数用此刻的值」",
    "N1": "撤 setPeakShown 关掉时的整面重画", "N2": "不再拷 OUT 的此刻值", "N3": "不再拷平均线", "N4": "不再拷峰值线", "N5": "不再拷 IN",
    "N6": "不再建峰值线路径", "N7": "「看没看变化」少比平均线", "N8": "「看没看变化」少比 OUT", "N9": "重建时不作废展开好的 IN 虚线",
    "N10": "换斜率不置 curvePathsValid = false", "N11": "「看没看变化」按未加斜率的纵坐标比", "N12": "悬停读数取峰值线", "N13": "悬停读数取平均线",
    "N14": "每帧无条件重建路径", "N15": "悬停光标的圆点取此刻的值、不取画出来的那组",
}
COLS = [
    ("S8", r"\(S8:"),
    ("S9残0", r"\(S9: GLAZE 0, peak off, slope 0,"), ("S9建0", r"rebuilds no curves .*\(S9, slope 0,"),
    ("S9残4", r"\(S9: GLAZE 0, peak off, slope 4\.5,"), ("S9建4", r"rebuilds no curves .*\(S9, slope 4\.5,"),
    ("S9b0", r"\(S9b: GLAZE 0, slope 0,"), ("S9b4", r"\(S9b: GLAZE 0, slope 4\.5,"),
    ("S11", r"\(S11:"), ("播放", r"\(B1 playback"),
    ("S10a", r"\(S10a:"), ("S10b", r"\(S10b:"), ("S10c", r"\(S10c:"),
    ("S12a关", r"\(S12a: .*peak off"), ("S12a开", r"\(S12a: .*peak on"), ("S12b关", r"\(S12b: .*peak off"), ("S12b开", r"\(S12b: .*peak on"),
    ("S12c", r"\(S12c:"),
    ("S12d0", r"\(S12d: .*slope 0,"), ("S12d4", r"\(S12d: .*slope 4\.5,"),
    ("探OUT", r"S12 probe OUT fill"), ("探均", r"S12 probe average"), ("探峰", r"S12 probe peak"), ("探IN", r"S12 probe IN"),
    ("读0", r"S12 probe hover readout, slope 0"), ("读4", r"S12 probe hover readout, slope 4\.5"),
]


def status(lines, pattern):
    for line in lines:
        if re.search(pattern, line):
            return ("败" if line.startswith("FAIL") else "过"), line
    return "?", ""


def main():
    out = pathlib.Path(sys.argv[1])
    print("变体".ljust(46) + " ".join(c.rjust(5) for c, _ in COLS))
    fails = []
    per_variant = []   # (key, [败的列])
    never = {c: True for c, _ in COLS}   # 每个检查有没有被某个变体打红过
    for key in sorted(NAMES, key=lambda k: (k[0] != "V", int(k[1:]))):
        f = out / "check" / f"{key}.txt"
        if not f.exists():
            print(f"{key:4s}（没有输出）")
            continue
        lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
        cells = []
        red_cols = []
        for col, pattern in COLS:
            st, line = status(lines, pattern)
            cells.append(st.rjust(5))
            if st == "败":
                fails.append((key, col, line))
                red_cols.append(col)
                if key != "V0":
                    never[col] = False
        print((f"{key:4s}{NAMES[key]}").ljust(46) + " ".join(cells))
        per_variant.append((key, red_cols))
    print("\n每个变体打红了哪几条检查（0 条 = 变体幸存）：")
    for key, cols in per_variant:
        print(f"  {key:4s}{NAMES[key]}".ljust(50) + f"败 {len(cols)}：" + " ".join(cols))
    print("\n没有任何变体能打红的检查（V0 除外）：" + ("、".join(c for c, v in never.items() if v) or "无"))
    print("\n败的检查原文：")
    for key, col, line in fails:
        print(f"  {key} {col}: {line[:300]}")


main()
