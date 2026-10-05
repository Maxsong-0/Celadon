#!/usr/bin/env python3
"""T-073 红绿验证用（只在临时副本里用，不进仓库的源码）：把 SpectrumView.cpp 的 curveRegion 改成两种「改坏」的样子，
看 --only check 的哪条检查变红。每种改法先断言原文恰好出现一次，对不上就停。
用法：variants_t073.py <SpectrumView.cpp> <改法>
  always-peak   峰值线不论 PEAK 开关都算进脏区（= T-073 之前的 R-02 ②）：PEAK 关着时区域含峰值线，probeRegionWithoutPeak 的差为 0
  never-peak    峰值线永远不算进脏区（收过头）：PEAK 开着时区域也不含它，差同样为 0，PEAK 开着时峰值线高出 OUT 的那段会留残影
"""
import pathlib
import sys

FIXED = """        if (peakToggle.isOn())
            area = area.getUnion (strokeArea (peakLine, 1.0f));
"""
ALWAYS = """        area = area.getUnion (strokeArea (peakLine, 1.0f));
"""
NEVER = ""


def once(text, old, new, what):
    assert text.count(old) == 1, f"{what}：源码里找不到（或不止一处）预期的文字（{text.count(old)} 处），副本与预期的版本对不上"
    return text.replace(old, new)


def apply(text, mode):
    if mode == "always-peak":
        return once(text, FIXED, ALWAYS, mode)
    if mode == "never-peak":
        return once(text, FIXED, NEVER, mode)
    raise SystemExit("未知改法：" + mode)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("用法：variants_t073.py <SpectrumView.cpp> <always-peak|never-peak>")
    path = pathlib.Path(sys.argv[1])
    path.write_text(apply(path.read_text(encoding="utf-8"), sys.argv[2]), encoding="utf-8")
    print("已改成变体：" + sys.argv[2])


if __name__ == "__main__":
    main()
