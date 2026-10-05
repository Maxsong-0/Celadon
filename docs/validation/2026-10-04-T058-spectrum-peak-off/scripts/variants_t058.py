#!/usr/bin/env python3
# T-058 红绿验证用：只在临时副本里用，把 SpectrumView.cpp 改成某种「撤掉一条」的变体，看截图工具 --only check 的哪条检查失败。
# 每种改法都先断言源码与预期的文字逐字对得上（对不上就停，不会悄悄改错地方）。几种改法可以用 + 连起来。
#
# 用法：variants_t058.py <SpectrumView.cpp> <改法>[+<改法>…]
#   region-only          撤 R-02 ②：curveRegion 只在 PEAK / IN 开着时算峰值线 / IN 线（同 T-051 revert_r02.py 的 ② 那一半）。main 与 T-058 都适用
#   paint-only           撤 R-02 ①：paint 里重建路径后不把画出的区域并进 curveArea。main（T-051 的注释）与 T-058（改过的注释）都适用
#   no-force-rebuild     撤「打开 PEAK 时强制重建」（setPeakShown 不再置 curvePathsValid = false）。只适用 T-058
#   no-peak-copy         撤「PEAK 关时每帧拷峰值线」：latestPeak 只在 PEAK 开着时拷。只适用 T-058
#   compare-peak-always  撤「PEAK 关时不比峰值线」：照旧每帧比。只适用 T-058
#   no-latest            撤「任何重建都先换成此刻的值」：退回主进程原拟的 P + X1——画出来的那组只在判变了时整组拷（同 main），
#                        PEAK 关时每帧只把峰值拷进画出来的那组，paint 里重建用画出来的那组；换斜率、悬停的读数也照 main 用它。只适用 T-058
#   no-old-area          repaintCurves 只重画新区域、不并旧区域（curveArea）。main 与 T-058 都适用
#   no-peak-region       curveRegion 完全不算峰值线（不论开关）。main 与 T-058 都适用
#   no-stroke-margin     curveRegion 里各条线不再按描边外扩（只用路径的包围盒；paint 里判相交的那份不动）。main 与 T-058 都适用
#   hover-drawn          撤「悬停的读数用此刻的值」：鼠标移动时读数照 main 用画出来的那组。只适用 T-058
import pathlib
import sys

REGION_FIXED = """        auto area = outputFill.getBounds().getUnion (strokeArea (outputLine, 0.8f)).getUnion (strokeArea (averageLine, 1.4f))
                                          .getUnion (strokeArea (peakLine, 1.0f));

        if (! inputLine.isEmpty())
            area = area.getUnion (strokeArea (inputLine, 1.1f));
"""
REGION_ONLY = """        auto area = outputFill.getBounds().getUnion (strokeArea (outputLine, 0.8f)).getUnion (strokeArea (averageLine, 1.4f));

        if (peakToggle.isOn())
            area = area.getUnion (strokeArea (peakLine, 1.0f));

        if (inputToggle.isOn() && inputReady)
            area = area.getUnion (strokeArea (inputLine, 1.1f));
"""
NO_PEAK_REGION = """        auto area = outputFill.getBounds().getUnion (strokeArea (outputLine, 0.8f)).getUnion (strokeArea (averageLine, 1.4f));

        if (! inputLine.isEmpty())
            area = area.getUnion (strokeArea (inputLine, 1.1f));
"""

PAINT_FIXED_MAIN = """        if (! curvePathsValid)
        {
            // 在这里重建（换了斜率、曲线静止时）：这一帧画出的区域并进「旧区域」，下一次只重画新旧包围盒时不漏掉它（评审 R-02）。
            buildCurvePaths();
            curveArea = curveArea.getUnion (curveRegion());
        }
"""
PAINT_FIXED_T058 = """        if (! curvePathsValid)
        {
            // 在这里重建（换了斜率、打开了 PEAK，按此刻的值，T-058）：这一帧画出的区域并进「旧区域」，下一次只重画新旧包围盒时
            // 不漏掉它（评审 R-02 ①）。
            buildCurvePaths();
            curveArea = curveArea.getUnion (curveRegion());
        }
"""
PAINT_REVERTED = """        if (! curvePathsValid)
            buildCurvePaths();
"""

MARGIN_FIXED = """        // 描边超出路径包围盒的上限同 paint：尖角接头最多伸出 2 × 线宽，再加 1 像素抗锯齿。
        const auto strokeArea = [] (const juce::Path& path, float thickness) { return path.getBounds().expanded (2.0f * thickness + 1.0f); };
"""
MARGIN_REVERTED = """        // 描边超出路径包围盒的上限同 paint：尖角接头最多伸出 2 × 线宽，再加 1 像素抗锯齿。
        const auto strokeArea = [] (const juce::Path& path, float) { return path.getBounds(); };
"""

OLD_AREA_FIXED = """        repaint (area.getUnion (curveArea).getSmallestIntegerContainer());
"""
OLD_AREA_REVERTED = """        repaint (area.getSmallestIntegerContainer());
"""

FORCE_FIXED = """        if (shouldShow)
            curvePathsValid = false;

        repaint();
"""
FORCE_REVERTED = """        repaint();
"""

COPY_FIXED = """        latestPeak    = analyser.getPeakDb();
"""
COPY_REVERTED = """        if (peakToggle.isOn())
            latestPeak = analyser.getPeakDb();
"""

COMPARE_FIXED = """|| (peakToggle.isOn() && differs (peak, latestPeak));
"""
COMPARE_REVERTED = """|| differs (peak, latestPeak);
"""

LATEST_BUILD_FIXED = """        output  = latestOutput;
        input   = latestInput;
        average = latestAverage;
        peak    = latestPeak;
        inputDashesValid = false;"""
LATEST_BUILD_REVERTED = """        inputDashesValid = false;"""
LATEST_CHANGED_FIXED = """            inputReady = nextInputReady;
            buildCurvePaths();
            repaintCurves();
        }
"""
LATEST_CHANGED_REVERTED = """            inputReady = nextInputReady;
            output  = latestOutput;
            input   = latestInput;
            average = latestAverage;
            peak    = latestPeak;
            buildCurvePaths();
            repaintCurves();
        }
        else if (! peakToggle.isOn())
        {
            peak = latestPeak;   // 主进程原拟的 P：PEAK 关时只把峰值拷进画出来的那组，不比
        }
"""
LATEST_SLOPE_FIXED = "updateTilt(); updatePeakReadout (latestOutput); refreshReadout(); repaint(); };"
LATEST_SLOPE_REVERTED = "updateTilt(); updatePeakReadout (output); refreshReadout(); repaint(); };"
LATEST_HOVER_FIXED = """        if (hoverFrequency.has_value())
            hoverDb = curveDbAt (latestOutput, *hoverFrequency);

        refreshReadout();
    }
"""
LATEST_HOVER_REVERTED = """        if (hoverFrequency.has_value())
            hoverDb = curveDbAt (output, *hoverFrequency);

        refreshReadout();
    }
"""


def replace_once(text, old, new, what):
    assert text.count(old) == 1, f'{what}：源码里找不到（或不止一处）预期的文字，副本与预期的版本对不上'
    return text.replace(old, new)


def apply(text, mode):
    if mode == 'region-only':
        return replace_once(text, REGION_FIXED, REGION_ONLY, mode)
    if mode == 'no-peak-region':
        return replace_once(text, REGION_FIXED, NO_PEAK_REGION, mode)
    if mode == 'paint-only':
        fixed = PAINT_FIXED_T058 if PAINT_FIXED_T058 in text else PAINT_FIXED_MAIN
        return replace_once(text, fixed, PAINT_REVERTED, mode)
    if mode == 'no-old-area':
        return replace_once(text, OLD_AREA_FIXED, OLD_AREA_REVERTED, mode)
    if mode == 'no-stroke-margin':
        return replace_once(text, MARGIN_FIXED, MARGIN_REVERTED, mode)
    if mode == 'hover-drawn':
        return replace_once(text, LATEST_HOVER_FIXED, LATEST_HOVER_REVERTED, mode)
    if mode == 'no-force-rebuild':
        return replace_once(text, FORCE_FIXED, FORCE_REVERTED, mode)
    if mode == 'no-peak-copy':
        return replace_once(text, COPY_FIXED, COPY_REVERTED, mode)
    if mode == 'compare-peak-always':
        return replace_once(text, COMPARE_FIXED, COMPARE_REVERTED, mode)
    if mode == 'no-latest':
        text = replace_once(text, LATEST_BUILD_FIXED, LATEST_BUILD_REVERTED, mode + '（buildCurvePaths）')
        text = replace_once(text, LATEST_CHANGED_FIXED, LATEST_CHANGED_REVERTED, mode + '（setSpectrum）')
        text = replace_once(text, LATEST_SLOPE_FIXED, LATEST_SLOPE_REVERTED, mode + '（换斜率的读数）')
        return replace_once(text, LATEST_HOVER_FIXED, LATEST_HOVER_REVERTED, mode + '（悬停的读数）')
    raise SystemExit('未知改法：' + mode)


def main():
    if len(sys.argv) != 3:
        raise SystemExit('用法：variants_t058.py <SpectrumView.cpp> <改法>[+<改法>…]（改法见文件头）')
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding='utf-8')
    modes = sys.argv[2].split('+')
    for mode in modes:
        text = apply(text, mode)
    path.write_text(text, encoding='utf-8')
    print('已改成变体：' + ' + '.join(modes))


if __name__ == '__main__':
    main()
