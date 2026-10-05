#!/usr/bin/env python3
"""去注释后的逐行差异全文（comment_only_check.py 只打印前 12 行）：用来核对「非注释的差异恰好是预期的字符串 / 常量生成改动，别无其他」。
用法：token_diff.py <BASE 提交> 文件 ...；输出 unified diff（-U0）。"""
import difflib, importlib.util, pathlib, subprocess, sys

spec = importlib.util.spec_from_file_location("coc", pathlib.Path(__file__).resolve().parents[1] / "2026-10-04-T060-phase-hint-comments" / "comment_only_check.py")
coc = importlib.util.module_from_spec(spec); spec.loader.exec_module(coc)

base, files = sys.argv[1], sys.argv[2:]
for f in files:
    before = subprocess.run(["git", "show", f"{base}:{f}"], capture_output=True).stdout.decode("utf-8", "replace")
    after = open(f, encoding="utf-8").read()
    (a, ea), (b, eb) = coc.stripped(before), coc.stripped(after)
    print(f"##### {f}（去注释后 {len(a)} → {len(b)} 行；预处理器 stderr：{(ea + eb).strip() or '无'}）")
    for d in difflib.unified_diff(a, b, lineterm="", n=0):
        if d.startswith(("---", "+++")):
            continue
        print(d)
