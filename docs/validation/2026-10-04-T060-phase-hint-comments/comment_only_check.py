#!/usr/bin/env python3
"""证明「只改了注释」：对每个改动文件，把改前（BASE 版）与改后（工作区版）各用 g++ -fpreprocessed -dD -E 去掉注释，
归一空白、去空行后逐行比；任一文件不同、或预处理器有任何 stderr 输出（如 unterminated comment）都算失败。
用法：comment_only_check.py <BASE 提交> [文件 ...]   不给文件时取 git diff --name-only BASE（含已暂存）。
纯函数部分（normalize）与读文件 / 起子进程分开，便于单测。"""
import subprocess, sys, re

def normalize(text: str) -> list[str]:
    out = []
    for ln in text.splitlines():
        if ln.startswith("# "):          # 行标记
            continue
        ln = re.sub(r"\s+", " ", ln).strip()
        if ln:
            out.append(ln)
    return out

def stripped(src: str) -> tuple[list[str], str]:
    r = subprocess.run(["g++", "-fpreprocessed", "-dD", "-E", "-x", "c++", "-"],
                       input=src.encode("utf-8"), capture_output=True)
    # 头文件从标准输入喂进去时 g++ 必报「#pragma once in main file」，与改动无关，滤掉；其他任何 stderr 都算异常
    err = "\n".join(l for l in r.stderr.decode("utf-8", "replace").splitlines() if "#pragma once in main file" not in l)
    return normalize(r.stdout.decode("utf-8", "replace")), err + (f"[exit {r.returncode}]" if r.returncode else "")

def main() -> int:
    base = sys.argv[1]
    files = sys.argv[2:] or subprocess.run(["git", "diff", "--name-only", base], capture_output=True, text=True).stdout.split()
    bad = 0
    for f in files:
        before = subprocess.run(["git", "show", f"{base}:{f}"], capture_output=True).stdout.decode("utf-8", "replace")
        after = open(f, encoding="utf-8").read()
        (a, ea), (b, eb) = stripped(before), stripped(after)
        same = a == b and not ea and not eb
        bad += 0 if same else 1
        print(f"{'SAME' if same else 'DIFF'}  {f:36s} {len(a)}/{len(b)} 行" + (f"  stderr: {ea.strip()[:80]} | {eb.strip()[:80]}" if (ea or eb) else ""))
        if a != b:
            import difflib
            for d in list(difflib.unified_diff(a, b, lineterm="", n=0))[:12]:
                print("      ", d[:200])
    print("全部只改注释" if bad == 0 else f"有 {bad} 个文件不只是注释")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
