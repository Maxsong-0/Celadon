#!/usr/bin/env python3
"""把矩阵的 progress 行（t074-matrix.sh 写的）+ variants_t074.py 里的改法说明 + 各改法的完整测试输出 → 红绿表（markdown）。
用法：make_redgreen.py <variants_t074.py 所在目录> <mut 输出目录> <progress 文件> [<progress 文件> ...]"""
import importlib.util
import os
import re
import sys

here, logs = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("variants_t074", f"{here}/variants_t074.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
VARIANTS = mod.VARIANTS


def first_failure(name):
    path = os.path.join(logs, f"{name}.test.txt")
    if not os.path.exists(path):
        return ""
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("!!! "):
            bare = re.match(r"^!!! Test (\d+) failed$", line.rstrip("\n"))
            if bare:
                return f"第 {bare.group(1)} 条断言（断言本身没写说明文字）"
            text = re.sub(r"^!!! Test \d+ failed: ", "", line.rstrip("\n"))
            text = text.replace("|", "\\|")
            return text if len(text) <= 120 else text[:117] + "…"
    return ""


rows = []
for path in sys.argv[3:]:
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line and line != "done":
            rows.append(line)

print("| 改法 | 做了什么 | 四个测试类（过 / 败） | 截图检查（过 / 败） | 头一条失败的断言 |")
print("|---|---|---|---|---|")
seen = {}
names = [line.split(" | ")[0].strip() for line in rows]
for line in rows:
    parts = [p.strip() for p in line.split(" | ")]
    name = parts[0]
    seen[name] = seen.get(name, 0) + 1
    label = name
    if names.count(name) > 1:
        label = f"{name}（第 {seen[name]} 批{'，原断言' if seen[name] == 1 and name != 'control' else ''}{'，加强断言后' if seen[name] == 2 and name != 'control' else ''}{'：加强了两处断言后' if seen[name] == 2 and name == 'control' else ''}）"
    what = "原样源码（对照）" if name == "control" else VARIANTS.get(name, ("", "", "", "", ""))[3]
    summary = next((p for p in parts if p.startswith("==== celadon_tests")), "")
    m = re.search(r"(\d+) passed, (\d+) failed", summary)
    tests = f"{m.group(1)} / **{m.group(2)}**" if m else "（没跑出结果）"
    snap = next((p for p in parts if p.startswith("截图检查")), "")
    m2 = re.search(r"PASS=(\d+) FAIL=(\d+)", snap)
    snap = f"{m2.group(1)} / **{m2.group(2)}**" if m2 else ""
    print(f"| `{label}` | {what} | {tests} | {snap} | {first_failure(name) if seen[name] == names.count(name) else (first_failure(name) if names.count(name) == 1 else '（第一批原断言下没有失败）' if name != 'control' else '')} |")
