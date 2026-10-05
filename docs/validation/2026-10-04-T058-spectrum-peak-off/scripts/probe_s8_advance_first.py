#!/usr/bin/env python3
# T-058 探针（只在临时副本里用，不进源码）：核对截图工具 S8 换斜率之后「先推一帧」还抓不抓得到 R-02 ①，以及那一帧
# setSpectrum 第一处判「变了」的是哪条曲线（T-051 时是 PEAK 关着也照比的峰值保持数据露出底边）。
#   ① SpectrumView.cpp：每次 updateTilt 之后的第一次 setSpectrum 打印第一处看得出变化的曲线（按 changed 的同一顺序与门槛）；
#   ② SnapshotMain.cpp：S8 换斜率后的「只重画一帧」改成「推一帧」。
# 先用 variants_t058.py 撤掉 ①（paint-only），再用本脚本加探针，编译后跑 --only check、grep T058PROBE。
# 用法：probe_s8_advance_first.py <副本根目录>
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

view = root / 'Source/ui/SpectrumView.cpp'
s = view.read_text(encoding='utf-8')
assert s.count('#include <initializer_list>\n') == 1
s = s.replace('#include <initializer_list>\n',
              '#include <initializer_list>\n#include <iostream>\nnamespace { bool probeAfterTilt = false; }\n')
tilt_line = '        curvePathsValid = false;   // paint 里按此刻的值重建（IN 虚线随 buildCurvePaths 作废）\n'
assert s.count(tilt_line) == 1, 'updateTilt 与 T-058 的版本对不上'
s = s.replace(tilt_line, tilt_line + '        probeAfterTilt = true;\n')
changed_head = '        if (changed)\n        {\n            // inputReady 只在这里更新'
assert s.count(changed_head) == 1, 'setSpectrum 与 T-058 的版本对不上'
probe = '''        if (probeAfterTilt)
        {
            probeAfterTilt = false;
            const auto firstDiff = [this] (const char* name, const std::vector<float>& shown, const std::vector<float>& next)
            {
                for (size_t i = 0; i < shown.size(); ++i)
                    if (std::abs (yOf (shown[i] + tilt[i]) - yOf (next[i] + tilt[i])) > 0.1f)
                    {
                        std::cerr << "T058PROBE first setSpectrum after tilt: " << name << " i " << i << " f " << pointFrequency[i] << " shown " << shown[i]
                                  << " next " << next[i] << " tilt " << tilt[i] << " (peak toggle " << (peakToggle.isOn() ? "on" : "off") << ")" << std::endl;
                        return true;
                    }
                return false;
            };
            if (! (firstDiff ("output", output, latestOutput) || (nextInputReady && firstDiff ("input", input, latestInput))
                   || firstDiff ("average", average, latestAverage) || (peakToggle.isOn() && firstDiff ("peak", peak, latestPeak))))
                std::cerr << "T058PROBE first setSpectrum after tilt: nothing visible differs (changed " << changed << ")" << std::endl;
        }

'''
s = s.replace(changed_head, probe + changed_head)
view.write_text(s, encoding='utf-8')

tool = root / 'tools/ui-snapshot/SnapshotMain.cpp'
t = tool.read_text(encoding='utf-8')
repaint_only = '                  run.repaintOnly ("slope 4.5 repaint");\n'
assert t.count(repaint_only) == 1, 'S8 与 T-058 的版本对不上'
t = t.replace(repaint_only, '                  run.frame ("slope 4.5 advance-first", 0, source);   // T-058 探针：先推一帧\n')
tool.write_text(t, encoding='utf-8')
print('已加探针：updateTilt 后第一次 setSpectrum 打印第一处变化；S8 换斜率后改成先推一帧')
