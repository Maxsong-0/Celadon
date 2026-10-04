#!/usr/bin/env python3
# 只在 main 的临时副本里用：把评审 R-02 的修复（867b454）撤掉，验证 T-051 的增量重画检查抓得到。
#   ① paint 里在路径重建时把画出的区域并进 curveArea；② curveRegion 不论开关都算峰值线 / IN 线。
# 用法：revert_r02.py <SpectrumView.cpp> [all|paint-only]
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) > 2 else 'all'
s = p.read_text(encoding='utf-8')

paint_fixed = """        if (! curvePathsValid)
        {
            // 在这里重建（换了斜率、曲线静止时）：这一帧画出的区域并进「旧区域」，下一次只重画新旧包围盒时不漏掉它（评审 R-02）。
            buildCurvePaths();
            curveArea = curveArea.getUnion (curveRegion());
        }
"""
paint_reverted = """        if (! curvePathsValid)
            buildCurvePaths();
"""
region_fixed = """        auto area = outputFill.getBounds().getUnion (strokeArea (outputLine, 0.8f)).getUnion (strokeArea (averageLine, 1.4f))
                                          .getUnion (strokeArea (peakLine, 1.0f));

        if (! inputLine.isEmpty())
            area = area.getUnion (strokeArea (inputLine, 1.1f));
"""
region_reverted = """        auto area = outputFill.getBounds().getUnion (strokeArea (outputLine, 0.8f)).getUnion (strokeArea (averageLine, 1.4f));

        if (peakToggle.isOn())
            area = area.getUnion (strokeArea (peakLine, 1.0f));

        if (inputToggle.isOn() && inputReady)
            area = area.getUnion (strokeArea (inputLine, 1.1f));
"""
assert s.count(paint_fixed) == 1 and s.count(region_fixed) == 1, '源码与 main 的 R-02 修复后版本对不上'
s = s.replace(paint_fixed, paint_reverted)
if mode == 'all':
    s = s.replace(region_fixed, region_reverted)
p.write_text(s, encoding='utf-8')
print('已撤掉 R-02 修复：' + ('① paint 并区域 + ② curveRegion 不论开关' if mode == 'all' else '只撤 ① paint 并区域'))
