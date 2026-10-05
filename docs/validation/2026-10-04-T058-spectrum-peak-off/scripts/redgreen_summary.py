#!/usr/bin/env python3
"""把 redgreen_run.sh 的输出汇成红绿表（redgreen.txt 的正文）。用法：redgreen_summary.py <redgreen 输出目录>"""
import pathlib, re, sys

NAMES = {   # 变体 -> 说明
    "V0": "T-058", "V1": "T-058 撤 ①（paint 里重建后不并旧区域）", "V2": "T-058 撤 ②（curveRegion 只在开着时算峰值线 / IN 线）",
    "V3": "T-058 撤 ② 与打开时的重建", "V4": "T-058 撤打开时的重建", "V5": "T-058 撤 PEAK 关时每帧拷峰值",
    "V6": "T-058 照旧比峰值线", "V7": "T-058 撤「重建先换成此刻的值」（= 主进程原拟的 P + X1）", "V8": "T-058 撤 repaintCurves 并旧区域",
    "V9": "T-058 curveRegion 不算峰值线", "V10": "T-058 curveRegion 不按描边外扩", "V11": "T-058 撤「悬停读数用此刻的值」",
    "M0": "main 218789a 的频谱仪 + 新工具", "M1": "main 撤 ②", "M2": "main 撤 ①", "M3": "main 撤 ① + ②（= T-051 revert_r02.py all）",
}
COLS = [("S8", r"\(S8:"), ("S9残影", r"\(S9: GLAZE 0, peak off"), ("S9零重建", r"rebuilds no curves"),
        ("播放", r"\(B1 playback"), ("S10a", r"\(S10a:"), ("S10b", r"\(S10b:"), ("S10c", r"\(S10c:")]

def status(lines, pat):
    for ln in lines:
        if re.search(pat, ln):
            return "败" if ln.startswith("FAIL") else "过", ln
    return "?", ""

def main():
    out = pathlib.Path(sys.argv[1])
    print("变体".ljust(58) + "  ".join(c for c, _ in COLS))
    fails = []
    for key in sorted(NAMES, key=lambda k: (k[0] != "V", int(k[1:]))):
        f = out / "check" / f"{key}.txt"
        if not f.exists():
            print(f"{key:4s}（没有输出）"); continue
        lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
        cells = []
        for col, pat in COLS:
            st, ln = status(lines, pat)
            cells.append(st)
            if st == "败":
                fails.append((key, col, ln))
        print((f"{key:4s}{NAMES[key]}").ljust(58) + "    ".join(cells))
    print("\n败的检查原文：")
    for key, col, ln in fails:
        print(f"  {key} {col}: {ln[:330]}")

main()
