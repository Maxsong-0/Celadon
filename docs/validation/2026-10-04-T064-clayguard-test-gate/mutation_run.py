#!/usr/bin/env python3
"""T-064 反向验证（变异测试）：在一棵独立源码树里逐个改坏产品码 / 测试码，增量编译、跑 ClayGuard 定向测试，记录哪些断言变红。
用法：mutation_run.py <源码树> <日志目录> <旧 ClayGuardTests.cpp（main 版）> [变体名 ...]（不给变体名 = 全部）
每个变体先把被改的文件还原成源码树 HEAD 的样子、再套改法（改法先断言原文恰好出现一次）；编译与运行都走 spark-test.sh（全机锁里运行）。
新测试 = 源码树里的 tests/ClayGuardTests.cpp；「旧测试」= 参数 3 给的文件（main 上的版本），对同一个改坏的产品码再跑一遍，看旧测试是不是看不出来。"""
import pathlib, subprocess, sys, os, re

TREE, LOGS, OLD_TEST = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
ONLY = sys.argv[4:]
LOGS.mkdir(parents=True, exist_ok=True)
SPARK = os.path.expanduser("~/.cache/celadon-spark/bin/spark-test.sh")
ENV = dict(os.environ, SPARK_JOBS="16")

CG = pathlib.Path("Source/dsp/ClayGuard.cpp")
GCH = pathlib.Path("Source/dsp/GlazeConstants.h")
TEST = pathlib.Path("tests/ClayGuardTests.cpp")
pristine = {p: (TREE / p).read_text(encoding="utf-8") for p in (CG, GCH, TEST)}

def restore():
    for p, t in pristine.items():
        (TREE / p).write_text(t, encoding="utf-8")

def mutate(path, old, new):
    t = (TREE / path).read_text(encoding="utf-8")
    assert t.count(old) == 1, f"{path}: 原文出现 {t.count(old)} 次：{old[:60]!r}"
    (TREE / path).write_text(t.replace(old, new, 1), encoding="utf-8")

# 变体：名字 -> (说明, [(文件, old, new)], 是否也用旧测试再跑)
VARIANTS = {
 "A-gate-boundary": ("快路门比较 < 改成 <=（门本身也算门内）",
    [(CG, "slow.value < tuning.phaseFastGate && fast.valid", "slow.value <= tuning.phaseFastGate && fast.valid")], True),
 "C-phase-reacts-to-wide-highs": ("反相提示在「高频很宽」的相关低音上也亮（P_S > 0.5）",
    [(CG, "        return PhaseInput::clear;\n    }\n\n    void ClayGuard::advancePhase",
          "        if (view.liveHighValid && view.liveHighSidePower > 0.5f)\n            return PhaseInput::inverted;\n\n        return PhaseInput::clear;\n    }\n\n    void ClayGuard::advancePhase")], True),
 "D-mono-depends-on-block-size": ("极短的块（< 0.5 ms）里输出收单偏 1e-4（小于旧测试的 2e-3 容差）",
    [(CG, "        guarded.monoAmount = plannedMono;", "        guarded.monoAmount = plannedMono + ((seconds > 0.0 && seconds < 0.0005) ? 1.0e-4f : 0.0f);")], True),
 "D2-mono-off-plan-in-17-sample-blocks": ("只在 17 样本一块（0.3 ～ 0.4 ms）里输出收单偏 1e-4：旧测试的「对参考轨迹的差」比 2e-3 容差小、别处也没有这么短的块，看不出来",
    [(CG, "        guarded.monoAmount = plannedMono;", "        guarded.monoAmount = plannedMono + ((seconds > 0.0003 && seconds < 0.0004) ? 1.0e-4f : 0.0f);")], True),
 "E-mono-follows-phase": ("反相提示亮着时把收单拉高（ADR-015 之前的行为）",
    [(CG, "        guarded.monoAmount = plannedMono;", "        guarded.monoAmount = plannedMono + 0.5f * state.phaseFloor * (1.0f - plannedMono);")], True),
 "F-test-gate-back-to-0.3": ("测试里 specConstants() 的快路门退回 0.3（旧值）",
    [(TEST, "c.clayGuardPhaseFastGate = 0.1f;", "c.clayGuardPhaseFastGate = 0.3f;")], False),
 "B-factory-gate-0.3": ("出厂常数的快路门退回 0.3（GlazeConstants.h，重编大半）",
    [(GCH, "float clayGuardPhaseFastGate = 0.1f;", "float clayGuardPhaseFastGate = 0.3f;")], False),
}

def run(tag, old_tests=False):
    if old_tests:
        (TREE / TEST).write_text(OLD_TEST.read_text(encoding="utf-8"), encoding="utf-8")
    b = subprocess.run([SPARK, str(TREE), "build", "celadon_tests"], capture_output=True, text=True, env=ENV)
    (LOGS / f"{tag}.build.txt").write_text(b.stdout + b.stderr, encoding="utf-8")
    if b.returncode != 0:
        return None, f"编译失败，见 {tag}.build.txt"
    r = subprocess.run([SPARK, str(TREE), "test", "--only", "ClayGuard"], capture_output=True, text=True, env=ENV)
    out = r.stdout + r.stderr
    (LOGS / f"{tag}.run.txt").write_text(out, encoding="utf-8")
    m = re.search(r"==== celadon_tests: (\d+) passed, (\d+) failed", out)
    fails = [l.strip() for l in out.splitlines() if l.startswith("!!! ")]
    return (int(m.group(1)), int(m.group(2)), fails) if m else None, ""

names = ONLY or list(VARIANTS)
summary = []
for name in names:
    desc, edits, also_old = VARIANTS[name]
    print(f"=== {name}：{desc}", flush=True)
    restore()
    for path, old, new in edits:
        mutate(path, old, new)
    res, err = run(name + ".new-tests")
    summary.append((name, "新测试", res, err))
    print("   新测试：", (f"{res[0]} 过 / {res[1]} 败" if res else err), flush=True)
    if also_old:
        restore()
        for path, old, new in edits:
            mutate(path, old, new)
        res, err = run(name + ".old-tests", old_tests=True)
        summary.append((name, "旧测试", res, err))
        print("   旧测试：", (f"{res[0]} 过 / {res[1]} 败" if res else err), flush=True)

restore()
print("\n==== 汇总 ====")
for name, kind, res, err in summary:
    if res:
        print(f"{name:34s} {kind}  过 {res[0]:6d}  败 {res[1]:3d}")
        for f in res[2][:12]:
            print("      ", f[:200])
    else:
        print(f"{name:34s} {kind}  {err}")
