#!/usr/bin/env python3
"""把 sweep.mjs 的若干次输出汇成一张表，并把「瘦身」后的 JSON（只留 meta / coverage / 命中 / 贴近，去掉逐页逐 SVG 的量值）写进证据目录。
用法：sweep_summary.py <sweeps 目录> <输出目录> 名字[=标签] ... [pair=改前名字,改后名字 ...]
例：sweep_summary.py ~/sweeps ./sweeps chromium-old=Chromium·改前 chromium-final=Chromium·改后 pair=chromium-old,chromium-final"""
import json
import pathlib
import sys
from collections import Counter

src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
rows = []
interest = {}   # 关心的贴近组：名字 → {标签: 最坏值}
PAIRS_ARG = [s[5:] for s in sys.argv[3:] if s.startswith("pair=")]
LABEL = {}
for spec in [s for s in sys.argv[3:] if not s.startswith("pair=")]:
    name, _, label = spec.partition("=")
    label = label or name
    LABEL[name] = label
    d = json.load(open(src / f"{name}.json"))
    m, c = d["meta"], d["coverage"]
    kinds = Counter(g["kind"] + ("/" + g["sub"] if g.get("sub") else "") for g in d["hits"])
    near = Counter(g["kind"] for g in d["nearMisses"])
    flags = ("半程" if m.get("half") else "") + ("减少动效" if m.get("reducedMotion") else "")
    rows.append((label, f"{m.get('engine') or 'chromium'} {m.get('browserVersion') or m.get('chromium')}", flags or "—", f"{c['pagesOk']}/{c['pagesPlanned']}",
                 f"{c['svgMeasured']}", f"{c['textElementsMeasured']}", f"{len(d['hits'])}", ", ".join(f"{k}×{v}" for k, v in kinds.items()) or "—",
                 f"{len(d['nearMisses'])}"))
    for g in d["nearMisses"]:
        if g["kind"] != "near-shape":
            continue
        key = f"{g['lang']} {g['desc']['name']} {g.get('frame') or ''}｜{g['textA'][:36]}"
        interest.setdefault(key, {})[label] = g["worst"]["value"]
    slim = {"meta": m, "coverage": {k: v for k, v in c.items() if k != "skippedByFigure"}, "hits": [{**g, "occurrences": g["occurrences"][:3]} for g in d["hits"]],
            "nearMisses": [{k: v for k, v in g.items() if k not in ("occurrences", "sourceA", "sourceB")} for g in d["nearMisses"]]}
    (out / f"{name}.slim.json").write_text(json.dumps(slim, ensure_ascii=False, indent=1), encoding="utf-8")

head = ("运行", "引擎", "状态", "页", "SVG", "<text>", "命中", "命中类型", "贴近")
width = [max(len(str(r[i])) for r in rows + [head]) for i in range(len(head))]
lines = ["  ".join(str(h).ljust(w) for h, w in zip(head, width))]
lines += ["  ".join(str(v).ljust(w) for v, w in zip(r, width)) for r in rows]
lines += ["", "贴近（near-shape，文字离图形 < 6 个单位，最坏值）在「改前 → 改后」两次运行（覆盖范围相同）之间有变化的组（GlazeKnob 的读数在动画、每次不同，不列）："]
for pair in PAIRS_ARG:
    a, b = pair.split(",")
    la, lb = LABEL[a], LABEL[b]
    lines.append(f"  [{la} → {lb}]")
    shown = 0
    for k, v in sorted(interest.items()):
        if "GlazeKnob" in k:
            continue
        if v.get(la) != v.get(lb):
            lines.append(f"    {k}：{v.get(la)} → {v.get(lb)}")
            shown += 1
    if not shown:
        lines.append("    （无变化）")
(out / "sweep-summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
