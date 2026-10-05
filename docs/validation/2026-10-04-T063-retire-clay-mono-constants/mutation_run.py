#!/usr/bin/env python3
"""T-063 反向验证：在独立源码树（已按 T-063 的三个文件覆盖）里把「退役」改坏，看新用例是不是变红。
用法：mutation_run.py <源码树> <日志目录> <T-063 的工作树>（把 GlazeConstants.{h,cpp}、GlazePlannerTests.cpp 从那里拷进源码树）
变体 R1：字段表里又出现 7 个退役名字——全都借道同一个现有成员（clayLowCutMinK）读写，等于「新存档又写了它们、读旧存档时它们的值被读进别的字段」，
        同时覆盖「不再写」「残留读时忽略」「名字不得复用」三件事。"""
import os, pathlib, re, shutil, subprocess, sys

TREE, LOGS, WORK = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
LOGS.mkdir(parents=True, exist_ok=True)
SPARK = os.path.expanduser("~/.cache/celadon-spark/bin/spark-test.sh")
ENV = dict(os.environ, SPARK_JOBS="16")
FILES = ["Source/dsp/GlazeConstants.h", "Source/dsp/GlazeConstants.cpp", "tests/GlazePlannerTests.cpp"]
for f in FILES:
    shutil.copyfile(WORK / f, TREE / f)

CPP = TREE / "Source/dsp/GlazeConstants.cpp"
src = CPP.read_text(encoding="utf-8")
anchor = '            v.optionalField ("clayCostFullDb", c.clayCostFullDb, 0.0f);\n'
assert src.count(anchor) == 1
names = ["clayMonoBandSideRatioDb", "clayMonoBandCorrelation", "clayMonoMinHz", "clayMonoMaxHz", "clayMonoDefaultHz", "clayMonoLossLowDb", "clayMonoLossHighDb"]
extra = "".join(f'            v.optionalField ("{n}", c.clayLowCutMinK, 0.0f);\n' for n in names)
CPP.write_text(src.replace(anchor, extra + anchor), encoding="utf-8")

def sh(args):
    return subprocess.run(args, capture_output=True, text=True, env=ENV)

b = sh([SPARK, str(TREE), "build", "celadon_tests"])
(LOGS / "R1.build.txt").write_text(b.stdout + b.stderr, encoding="utf-8")
if b.returncode != 0:
    sys.exit("编译失败，见 R1.build.txt")
r = sh([SPARK, str(TREE), "test", "--only", "GlazePlanner"])
out = r.stdout + r.stderr
(LOGS / "R1.run.txt").write_text(out, encoding="utf-8")
m = re.search(r"==== celadon_tests: (\d+) passed, (\d+) failed", out)
print("R1（7 个退役名字又进了字段表）：", f"{m.group(1)} 过 / {m.group(2)} 败" if m else "没有汇总行")
for l in out.splitlines():
    if l.startswith("!!! "):
        print("   ", l[:210])
