#!/usr/bin/env python3
# T-070 红绿验证用：只在临时副本里用，把 SpectrumView.cpp 改成某种「改坏」的变体，看截图工具 --only check 的哪条检查变红。
# 每种改法都先断言源码与预期文字逐字对得上（对不上就停，不会悄悄改错地方）。T-058 的 11 种改法在 ../../2026-10-04-T058-spectrum-peak-off/scripts/variants_t058.py。
#
# 用法：variants_t070.py <SpectrumView.cpp> <改法>
#   no-repaint-on-off      setPeakShown：只在打开时 repaint（关掉时峰值线不再被整面重画擦掉）                         —— 评审 T058-F1
#   no-copy-output         setSpectrum 不再拷 OUT 的此刻值（latestOutput 永远是底线）                                   —— 评审 F2
#   no-copy-average        不再拷平均线
#   no-copy-peak           不再拷峰值线
#   no-copy-input          不再拷 IN
#   no-build-peak          buildCurvePaths 不再建峰值线路径
#   no-compare-average     「看没看变化」少比平均线                                                                    —— 评审 F3
#   no-compare-output      「看没看变化」少比 OUT
#   no-dashes-invalidate   buildCurvePaths 不再作废展开好的 IN 虚线                                                    —— 评审 F4
#   no-tilt-invalidate     updateTilt 不再置 curvePathsValid = false                                                    —— 评审 F7
#   differs-no-tilt        「看没看变化」按未加斜率的纵坐标比（撤 T-036 评审 R-07）                                     —— 评审 F8
#   hover-peak             悬停读数取峰值线的值（两处）                                                                  —— 评审 F6
#   hover-average          悬停读数取平均线的值（两处）
#   rebuild-every-frame    setSpectrum 每帧无条件重建路径（只在判变了时才重画）                                          —— 评审 F5
#   dot-latest             悬停光标的圆点取此刻的值（latestOutput），不取画出来的那组（output）                           —— 评审 F6
import pathlib
import sys

SET_PEAK_FIXED = """        if (shouldShow)
            curvePathsValid = false;

        repaint();
    }
"""
SET_PEAK_BROKEN = """        if (shouldShow)
        {
            curvePathsValid = false;
            repaint();
        }
    }
"""
COPY_OUT = "        latestOutput  = analyser.getDisplayDb (SpectrumAnalyser::Stream::output);\n"
COPY_IN = "        latestInput   = analyser.getDisplayDb (SpectrumAnalyser::Stream::input);\n"
COPY_AVG = "        latestAverage = analyser.getAverageDb();\n"
COPY_PEAK = "        latestPeak    = analyser.getPeakDb();\n"
BUILD_PEAK = "        buildEnvelope (peak, peakLine);\n"
CHANGED_FIXED = """        const auto changed = nextInputReady != inputReady
                          || differs (output, latestOutput) || (nextInputReady && differs (input, latestInput))
                          || differs (average, latestAverage) || (peakToggle.isOn() && differs (peak, latestPeak));
"""
CHANGED_NO_AVG = """        const auto changed = nextInputReady != inputReady
                          || differs (output, latestOutput) || (nextInputReady && differs (input, latestInput))
                          || (peakToggle.isOn() && differs (peak, latestPeak));
"""
CHANGED_NO_OUT = """        const auto changed = nextInputReady != inputReady
                          || (nextInputReady && differs (input, latestInput))
                          || differs (average, latestAverage) || (peakToggle.isOn() && differs (peak, latestPeak));
"""
DASHES = "        inputDashesValid = false;   // IN 虚线由 inputLine 展开：线一重建就作废（原来写在 setSpectrum 与 updateTilt 里）\n"
TILT_FIXED = "        curvePathsValid = false;   // paint 里按此刻的值重建（IN 虚线随 buildCurvePaths 作废）\n"
DIFFERS_FIXED = "                if (std::abs (yOf (shown[i] + tilt[i]) - yOf (next[i] + tilt[i])) > 0.1f)\n"
DIFFERS_BROKEN = "                if (std::abs (yOf (shown[i]) - yOf (next[i])) > 0.1f)\n"
HOVER_FIXED = "hoverDb = curveDbAt (latestOutput, *hoverFrequency);"
DOT_FIXED = "const auto db = curveDbAt (output, *hoverFrequency) + tiltAt (*hoverFrequency);"
CHANGED_BLOCK_FIXED = """        if (changed)
        {
            // inputReady 只在这里更新"""
CHANGED_BLOCK_BROKEN = """        buildCurvePaths();   // 变体：每帧无条件重建

        if (changed)
        {
            // inputReady 只在这里更新"""


def once(text, old, new, what):
    assert text.count(old) == 1, f'{what}：源码里找不到（或不止一处）预期的文字（{text.count(old)} 处），副本与预期的版本对不上'
    return text.replace(old, new)


def twice(text, old, new, what):
    assert text.count(old) == 2, f'{what}：预期两处，实际 {text.count(old)} 处'
    return text.replace(old, new)


def apply(text, mode):
    if mode == 'no-repaint-on-off':
        return once(text, SET_PEAK_FIXED, SET_PEAK_BROKEN, mode)
    if mode == 'no-copy-output':
        return once(text, COPY_OUT, '', mode)
    if mode == 'no-copy-input':
        return once(text, COPY_IN, '', mode)
    if mode == 'no-copy-average':
        return once(text, COPY_AVG, '', mode)
    if mode == 'no-copy-peak':
        return once(text, COPY_PEAK, '', mode)
    if mode == 'no-build-peak':
        return once(text, BUILD_PEAK, '', mode)
    if mode == 'no-compare-average':
        return once(text, CHANGED_FIXED, CHANGED_NO_AVG, mode)
    if mode == 'no-compare-output':
        return once(text, CHANGED_FIXED, CHANGED_NO_OUT, mode)
    if mode == 'no-dashes-invalidate':
        return once(text, DASHES, '', mode)
    if mode == 'no-tilt-invalidate':
        return once(text, TILT_FIXED, '', mode)
    if mode == 'differs-no-tilt':
        return once(text, DIFFERS_FIXED, DIFFERS_BROKEN, mode)
    if mode == 'hover-peak':
        return twice(text, HOVER_FIXED, 'hoverDb = curveDbAt (latestPeak, *hoverFrequency);', mode)
    if mode == 'hover-average':
        return twice(text, HOVER_FIXED, 'hoverDb = curveDbAt (latestAverage, *hoverFrequency);', mode)
    if mode == 'dot-latest':
        return once(text, DOT_FIXED, DOT_FIXED.replace('curveDbAt (output,', 'curveDbAt (latestOutput,'), mode)
    if mode == 'rebuild-every-frame':
        return once(text, CHANGED_BLOCK_FIXED, CHANGED_BLOCK_BROKEN, mode)
    raise SystemExit('未知改法：' + mode)


def main():
    if len(sys.argv) != 3:
        raise SystemExit('用法：variants_t070.py <SpectrumView.cpp> <改法>（改法见文件头）')
    path = pathlib.Path(sys.argv[1])
    path.write_text(apply(path.read_text(encoding='utf-8'), sys.argv[2]), encoding='utf-8')
    print('已改成变体：' + sys.argv[2])


if __name__ == '__main__':
    main()
