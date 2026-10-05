#!/usr/bin/env python3
# T-058 交叉核对（只在 main 218789a 的临时副本里用，不进源码）：给频谱仪与截图工具的 spectrum-peak-bench 加探针打印，
# 核对「基准里的曲线帧」与「setSpectrum 的重建」一一对应、PEAK 关时有几次重建只是因为峰值在变、那几帧屏上到底变没变。
#   SpectrumView.cpp：setSpectrum 每次调用打一行 PROBE set（第一处判变化的曲线 first、PEAK 开关、比较 / 拷贝 / 重建 / 标脏 / 曲线部分合计的
#     墙钟 ms——绑单核跑，墙钟≈CPU；steady_clock 走 vDSO，探针自身开销远小于 1 µs；计时段里不打印——buildCurvePaths 只累加计数，
#     随 PROBE set / paintRebuild 带出 builds=）；paint 里重建、修坯标记 / 图例开关 / 斜率引起的整块重画、读数框与响度小窗的重画各打一行
#     （都在计时段外）。比较顺序与原来的 || 链相同（短路一致）。
#   SnapshotMain.cpp（同一副本里 T-058 版的截图工具）：spectrum-peak-bench 每帧打一行 PROBE frame（有没有推、脏不脏、是不是曲线帧、
#     这一帧重画前后频谱仪那一块变了几个设备像素与最大通道差）。
# 用法：probe_peak_rebuilds.py <副本根目录>
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def patch(path, pairs):
    p = root / path
    s = p.read_text(encoding='utf-8')
    for old, new in pairs:
        assert s.count(old) == 1, f'{path}: 锚点不是正好一处：{old[:80]!r}'
        s = s.replace(old, new)
    p.write_text(s, encoding='utf-8')


view = 'Source/ui/SpectrumView.cpp'
patch(view, [
    ('#include <array>\n',
     '#include <array>\n#include <chrono>\n#include <iostream>\n'),
    ('namespace celadon::ui\n{\n',
     'namespace celadon::ui\n{\n'
     '    namespace { double probeMs() { return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now().time_since_epoch()).count(); } '
     'long long probeBuilds = 0; }   // [T058 probe]\n'),
    # setSpectrum：第一处判变化的曲线（与原来的 || 链同序、同短路）+ 分段计时
    ('''        // IN 虚线关着时让计算器跳过 IN 那路的加窗 + FFT（T-032 R7-05）。刚打开时它要到下一次 update 才有 IN 的曲线：这之前不画 IN。
        analyser.setInputSpectrumEnabled (inputToggle.isOn());''',
     '''        const auto probeStart = probeMs();   // [T058 probe]
        // IN 虚线关着时让计算器跳过 IN 那路的加窗 + FFT（T-032 R7-05）。刚打开时它要到下一次 update 才有 IN 的曲线：这之前不画 IN。
        analyser.setInputSpectrumEnabled (inputToggle.isOn());'''),
    ('''        const auto changed = nextInputReady != inputReady
                          || differs (output, nextOutput) || differs (input, nextInput)
                          || differs (average, analyser.getAverageDb()) || differs (peak, analyser.getPeakDb());

        if (changed)
        {
            inputReady = nextInputReady;
            output  = nextOutput;
            input   = nextInput;
            average = analyser.getAverageDb();
            peak    = analyser.getPeakDb();
            inputDashesValid = false;
            buildCurvePaths();
            repaintCurves();
        }
''',
     '''        const char* probeFirst = nextInputReady != inputReady ? "inputReady"
                               : differs (output, nextOutput) ? "output"
                               : differs (input, nextInput) ? "input"
                               : differs (average, analyser.getAverageDb()) ? "average"
                               : differs (peak, analyser.getPeakDb()) ? "peak" : nullptr;   // [T058 probe]
        const auto changed = probeFirst != nullptr;
        const auto probeCompared = probeMs();
        auto probeCopied = probeCompared, probeBuilt = probeCompared, probeMarked = probeCompared;   // 读数部分（refreshReadout 会打印）不计

        if (changed)
        {
            inputReady = nextInputReady;
            output  = nextOutput;
            input   = nextInput;
            average = analyser.getAverageDb();
            peak    = analyser.getPeakDb();
            inputDashesValid = false;
            probeCopied = probeMs();
            buildCurvePaths();
            probeBuilt = probeMs();
            repaintCurves();
            probeMarked = probeMs();
        }
'''),
    ('''            if (hoverFrequency.has_value())
                hoverDb = curveDbAt (nextOutput, *hoverFrequency);

            refreshReadout();
        }
    }
''',
     '''            if (hoverFrequency.has_value())
                hoverDb = curveDbAt (nextOutput, *hoverFrequency);

            refreshReadout();
        }

        std::cout << "PROBE set first=" << (probeFirst != nullptr ? probeFirst : "none") << " peakOn=" << (peakToggle.isOn() ? 1 : 0)
                  << " compareMs=" << (probeCompared - probeStart) << " copyMs=" << (probeCopied - probeCompared)
                  << " buildMs=" << (probeBuilt - probeCopied) << " markMs=" << (probeMarked - probeBuilt)
                  << " curvesMs=" << (probeMarked - probeStart) << " builds=" << probeBuilds << std::endl;   // [T058 probe]
    }
'''),
    ('''        buildColumnLevels();
        curvePathsValid = true;
    }
''',
     '''        buildColumnLevels();
        curvePathsValid = true;
        ++probeBuilds;   // [T058 probe]（计时段里不打印）
    }
'''),
    ('''            buildCurvePaths();
            curveArea = curveArea.getUnion (curveRegion());
        }
''',
     '''            buildCurvePaths();
            std::cout << "PROBE paintRebuild builds=" << probeBuilds << std::endl;   // [T058 probe]
            curveArea = curveArea.getUnion (curveRegion());
        }
'''),
    ('''            toggle->onClick = [this, toggle] { toggle->setOn (! toggle->isOn()); repaint(); };''',
     '''            toggle->onClick = [this, toggle] { std::cout << "PROBE wholeRepaint toggle" << std::endl; toggle->setOn (! toggle->isOn()); repaint(); };'''),
    ('''        slopeControl.onChange = [this] { updateTilt(); updatePeakReadout (output); refreshReadout(); repaint(); };''',
     '''        slopeControl.onChange = [this] { std::cout << "PROBE wholeRepaint slope" << std::endl; updateTilt(); updatePeakReadout (output); refreshReadout(); repaint(); };'''),
    ('''        readoutString = std::move (text);
        repaint (layerArea (Layer::readout));''',
     '''        readoutString = std::move (text);
        std::cout << "PROBE readoutRepaint" << std::endl;   // [T058 probe]
        repaint (layerArea (Layer::readout));'''),
    ('''        shownKey = key;
        repaint();''',
     '''        shownKey = key;
        std::cout << "PROBE hudRepaint" << std::endl;   // [T058 probe]
        repaint();'''),
    ('''        clayActive = active;
        updateMarkerLabels();
        repaint();''',
     '''        clayActive = active;
        updateMarkerLabels();
        std::cout << "PROBE wholeRepaint clay" << std::endl;   // [T058 probe]
        repaint();'''),
])

tool = 'tools/ui-snapshot/SnapshotMain.cpp'
patch(tool, [
    ('''                auto clip = rig.takeDirty();
                clip.clipTo (rig.area);

                if (! clip.isEmpty())
                {
                    const auto started = Clock::now();
                    paintRegion (editor, screen, scale, clip);   // 画的时候又标脏的留在 recorder 里：下一帧画
                    painted = started.elapsed();
                }

                segment.addFrame (advanced, painted, clip, scale, clip.containsPoint (bottomCentre));''',
     '''                auto clip = rig.takeDirty();
                clip.clipTo (rig.area);
                int probeChanged = 0, probeWorst = 0;   // [T058 probe]

                if (! clip.isEmpty())
                {
                    const auto probeArea = rig.deviceArea (scale);
                    const auto probeBefore = screen.getClippedImage (probeArea).createCopy();
                    const auto started = Clock::now();
                    paintRegion (editor, screen, scale, clip);   // 画的时候又标脏的留在 recorder 里：下一帧画
                    painted = started.elapsed();
                    juce::Point<int> probeAt;
                    probeChanged = countDifferences (probeBefore, screen.getClippedImage (probeArea),
                                                     { 0, 0, probeBefore.getWidth(), probeBefore.getHeight() }, probeWorst, probeAt);
                }

                segment.addFrame (advanced, painted, clip, scale, clip.containsPoint (bottomCentre));
                std::cout << "PROBE frame advanced=" << (advance ? 1 : 0) << " dirty=" << (clip.isEmpty() ? 0 : 1)
                          << " curves=" << (clip.containsPoint (bottomCentre) ? 1 : 0) << " changedPx=" << probeChanged
                          << " worst=" << probeWorst << std::endl;'''),
])
print('已加探针：' + view + '、' + tool)
