#!/usr/bin/env python3
"""两份站点产物（dist/assets/*.js）逐句对比：把 JS 里的转义换行还原、按行拆开、归一化文件名哈希，求「只在改前」「只在改后」的行。
用法：dist_sentence_diff.py <改前 site 目录> <改后 site 目录>  （要 site/dist 已构建）。输出两边各有多少行不同、并逐行列出（截 170 字）。"""
import glob, re, sys

def lines(site):
    out = {}
    for f in sorted(glob.glob(f"{site}/dist/assets/*.js")):
        t = open(f, encoding="utf-8", errors="replace").read().replace("\\n", "\n")
        for ln in t.split("\n"):
            ln = re.sub(r"[A-Za-z0-9_-]{8}\.(js|css)", "H.\\1", ln).strip()
            if len(ln) >= 8:
                out[ln] = f
    return out

b, a = lines(sys.argv[1]), lines(sys.argv[2])
only_b, only_a = sorted(set(b) - set(a)), sorted(set(a) - set(b))
print(f"只在改前 {len(only_b)} 行，只在改后 {len(only_a)} 行")
for l in only_b:
    print("  -", l[:170])
for l in only_a:
    print("  +", l[:170])
