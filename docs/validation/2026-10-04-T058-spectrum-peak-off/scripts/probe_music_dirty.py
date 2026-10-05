#!/usr/bin/env python3
# T-058 探针（只在临时副本里用，不进源码）：给 --only spectrum-peak-bench 换成「放音乐」一个场景，量 PEAK 关着放音乐时
# 评审 R-02 ② 让看不见的峰值线进脏区多花的像素（评审 T-058 B 的 H-05）。配合 variants_t058.py region-only 跑两遍逐行比：
# 帧数、标脏帧、曲线帧、设备像素每次运行都相同（不受负载影响），时间只作参考。
#   场景：GLAZE 0、斜率 4.5（默认）、合成音乐（同 --only check 的「播放」）8 s × PEAK 关 / 开 × 60 / 120 帧 × 1 / 2 倍。
# 用法：probe_music_dirty.py <副本根目录>
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
tool = root / 'tools/ui-snapshot/SnapshotMain.cpp'
t = tool.read_text(encoding='utf-8')


def replace_once(old, new):
    global t
    assert t.count(old) == 1, '截图工具与 T-058 的版本对不上：' + old[:60]
    t = t.replace(old, new)


replace_once('        enum class Scenario { s8, stopSlope0, stopSlope45 };\n',
             '        enum class Scenario { s8, stopSlope0, stopSlope45, music };\n')
replace_once('''            const auto title = juce::String (scenario == Scenario::s8 ? "S8" : scenario == Scenario::stopSlope0 ? "stop, slope 0" : "stop, slope 4.5")''',
             '''            const auto title = juce::String (scenario == Scenario::music ? "music" : scenario == Scenario::s8 ? "S8" : scenario == Scenario::stopSlope0 ? "stop, slope 0" : "stop, slope 4.5")''')
replace_once('''            rig.slope->setSelected (scenario == Scenario::stopSlope45 ? 2 : 0, true);''',
             '''            rig.slope->setSelected (scenario == Scenario::stopSlope45 || scenario == Scenario::music ? 2 : 0, true);''')
replace_once('''            if (scenario == Scenario::s8)
            {
                const auto steady = play ("steady", 12, std::ref (sine));''',
             '''            if (scenario == Scenario::music)
            {
                auto& music = rig.session->music;
                play ("music", 8, [&music] (juce::AudioBuffer<float>& chunk) { music.render (chunk, chunk.getNumSamples()); });
            }
            else if (scenario == Scenario::s8)
            {
                const auto steady = play ("steady", 12, std::ref (sine));''')
replace_once('''        for (const auto scenario : { Scenario::s8, Scenario::stopSlope0, Scenario::stopSlope45 })''',
             '''        for (const auto scenario : { Scenario::music })''')
tool.write_text(t, encoding='utf-8')
print('已把 --only spectrum-peak-bench 换成「放音乐 8 s」一个场景')
