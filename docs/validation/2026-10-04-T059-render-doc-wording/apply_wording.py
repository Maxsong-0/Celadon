#!/usr/bin/env python3
"""T-059：把核实 workflow（wf_8b34caaf-a79）汇总代理给的「确定要改」措辞（proposals 0～10）套到 tools/render 上。
每条先断言 current 在文件里恰好出现一次（对不上就停，不会悄悄改错地方）；--dry-run 只报匹配情况不写文件。
第 6 项（proposals 11～15，Max 同意才改）不在这里。0.35 s 两条按任务范围「只改说明文字」写死 0.35，不改成由常数生成（待 Max 定）。"""
import json, sys, pathlib

dry = "--dry-run" in sys.argv
proposals = json.load(open(sys.argv[1], encoding="utf-8"))["proposals"][:11]
files = {}
bad = 0
for i, p in enumerate(proposals):
    path = pathlib.Path(p["file"])
    text = files.setdefault(path, path.read_text(encoding="utf-8"))
    cur, new = p["current"], p["proposed"]
    if i in (5, 6):   # 0.3 s 余振 → 0.35 s，写死（任务范围只改文字；由 ringOutSeconds 生成留给 Max 定）
        new = cur.replace("0.3 s", "0.35 s")
        assert new != cur
    n = text.count(cur)
    print(f"[{i}] {p['file']}  匹配 {n} 处  {'OK' if n == 1 else '*** 不是恰好 1 处 ***'}")
    if n != 1:
        bad += 1
        continue
    files[path] = text.replace(cur, new, 1)
if bad:
    sys.exit(f"{bad} 条对不上，一个文件都没写")
if not dry:
    for path, text in files.items():
        path.write_text(text, encoding="utf-8")
    print("已写入：", ", ".join(str(p) for p in files))
