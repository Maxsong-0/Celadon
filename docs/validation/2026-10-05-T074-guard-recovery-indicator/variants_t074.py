#!/usr/bin/env python3
"""T-074 红绿验证用（只在临时副本里用，不进仓库的源码）：把守护回收指示的实现改坏成各种样子，看新用例哪条变红。
每种改法先断言原文恰好出现一次，对不上就停。用法：variants_t074.py <源码树根目录> <改法>
列出所有改法：variants_t074.py --list
"""
import pathlib
import sys

CM = "Source/dsp/ClayModule.cpp"
CG = "Source/dsp/ClayGuard.cpp"
CGH = "Source/dsp/ClayGuard.h"
CH = "Source/dsp/CeladonChain.cpp"
GC = "Source/dsp/GlazeController.cpp"
HT = "Source/ui/HelpText.cpp"

SLOW_KEEP = """                    if (keepMeasuring)
                    {
                        const auto change = static_cast<double> (y) - static_cast<double> (atJ.dry[static_cast<std::size_t> (c)]);
                        const auto energy = change * change;
                        keepChange += energy;
                        keepKept += gain < 1.0 ? energy * gain * gain : energy;
                    }

"""
FAST_KEEP = """
                    // 守护保留比统计（T-074）：快路里 g = 1，改动量原样保留，两份能量同加。
                    if (keepMeasuring)
                    {
                        const auto change = static_cast<double> (atJ.wet[static_cast<std::size_t> (c)])
                                          - static_cast<double> (atJ.dry[static_cast<std::size_t> (c)]);
                        const auto energy = change * change;
                        keepChange += energy;
                        keepKept += energy;
                    }
"""

# 名字 -> (文件, 原文, 新文, 说明, 预期变红的测试类)
VARIANTS = {
    "no-fast-keep": (CM, FAST_KEEP, "\n", "快路（g = 1）的样本不进两份能量", "ClayModule"),
    "no-slow-keep": (CM, SLOW_KEEP, "", "慢路（g < 1 可能）的样本不进两份能量", "ClayModule"),
    "kept-uses-g": (CM, "keepKept += gain < 1.0 ? energy * gain * gain : energy;", "keepKept += gain < 1.0 ? energy * gain : energy;",
                    "保留量用 g 不用 g²（能量要平方）", "ClayModule"),
    "report-not-reset": (CM, "        peakGuard.keepChangeEnergy = 0.0;\n        peakGuard.keepKeptEnergy = 0.0;\n\n        if (sampleRate <= 0.0",
                         "\n        if (sampleRate <= 0.0", "每次 process() 开头不清零（报告变成累计）", "ClayModule"),
    "keep-always-on": (CM, "const auto keepMeasuring = reportKeep && blendMeasuring;", "const auto keepMeasuring = blendMeasuring;",
                       "不看 reportKeep（默认关失效，分析器的先试候选也累计）", "ClayModule"),
    "gate-missing": (CG, "        if (! (gateChange > floorMeanSquare * gateSeconds * sampleRate))", "        if (false)",
                     "监测器没有「有没有可听的改动」的快门（低于地板也照算）", "ClayGuard"),
    "on-below-50": (CGH, "static constexpr double onBelow = 0.75;", "static constexpr double onBelow = 0.5;",
                    "亮灯线退回设计稿的 50 %（会漏掉 V4 的典型坏例子）", "ClayGuard,GlazeController"),
    "no-hysteresis": (CGH, "static constexpr double offAbove = 0.85;", "static constexpr double offAbove = 0.75;",
                      "没有滞回（熄灯线 = 亮灯线）", "ClayGuard"),
    "share-missing": (CG, "            change = change * decay + changeEnergy * share;\n            kept = kept * decay + keptEnergy * share;",
                      "            change = change * decay + changeEnergy;\n            kept = kept * decay + keptEnergy;",
                      "平滑不按片长折算（稳态值随片长偏大，块长相关）", "ClayGuard"),
    "nan-not-sanitised": (CG, "        if (! (std::isfinite (changeEnergy) && std::isfinite (keptEnergy) && changeEnergy >= 0.0 && keptEnergy >= 0.0))\n            changeEnergy = keptEnergy = 0.0;\n",
                          "", "监测器不拦非有限 / 负的能量（一个 NaN 切片永远污染平滑量）", "ClayGuard"),
    "no-validity-guard": (CG, "        if (! (seconds > 0.0 && std::isfinite (seconds) && sampleRate > 0.0 && std::isfinite (sampleRate)))\n            return;\n",
                          "", "监测器不拦无效的片长 / 采样率", "ClayGuard"),
    "prepare-no-reset": (GC, "        s.clayGuardKeep.reset();   // T-074：链整体重建（采样率 / 块长 / 声道数变了），守护的统计从零起\n", "",
                         "prepare 不清监测器（链重建后带着旧的「亮」继续）", "GlazeController"),
    "chain-no-enable": (CH, "        clay.setGuardKeepReporting (true);   // T-074：守护保留比统计只由链里这一份报（分析器的先试候选不开，不多花 CPU）\n", "",
                        "链里不打开统计", "GlazeController"),
    "chain-no-copy": (CH, "        stats.clayGuardChangeEnergy = clayKeep.changeEnergy;\n        stats.clayGuardKeptEnergy   = clayKeep.keptEnergy;\n", "",
                      "链不把统计交给编排器", "GlazeController"),
    "status-not-filled": (GC, "            s.clayPeakGuardActive = clayGuardKeep.active();   // T-074：手动 / 自动都报（见 ClayGuardKeepMonitor）\n", "",
                          "状态里不填「回收中」", "GlazeController"),
    "controller-gated-by-glaze": (GC, "            clayGuardKeep.update (stats.clayGuardChangeEnergy, stats.clayGuardKeptEnergy, (double) n / sampleRate, sampleRate);",
                                  "            if (active)\n                clayGuardKeep.update (stats.clayGuardChangeEnergy, stats.clayGuardKeptEnergy, (double) n / sampleRate, sampleRate);",
                                  "只在 GLAZE 活跃时更新（GLAZE 0 的手动项不报）", "GlazeController"),
    "lamp-ignores-guard": (HT, "                if (readout.peakGuard && readout.lowCutHz > 0)\n                    return ClayLamp::guard;\n\n", "",
                           "低切灯不因守护回收变琥珀", "HelpTextClay"),
    "readout-gated-by-auto": (HT, "readout.peakGuard = status.clayPeakGuardActive && (readout.lowCutHz > 0 || readout.monoPercent > 0);",
                              "readout.peakGuard = status.clayPeakGuardActive && (user.lowCutAuto || user.monoAuto) && (readout.lowCutHz > 0 || readout.monoPercent > 0);",
                              "读数按三态筛（手动不报）", "HelpTextClay"),
    "live-line-missing": (HT, '''        if (readout.peakGuard)
            parts.add (utf8 ("峰值守护回收中：") + peakGuardStagesZh (readout) + utf8 ("实际只剩约 ") + juce::String (readout.peakGuardKeptPercent) + "%");

''', "", "中文气泡不写「峰值守护回收中」", "HelpTextClay"),
    "percent-not-rounded": (HT, "            return 5 * juce::roundToInt (juce::jlimit (0.0f, 1.0f, kept) * 20.0f);",
                            "            return juce::roundToInt (juce::jlimit (0.0f, 1.0f, kept) * 100.0f);", "百分数不取整到 5 %", "HelpTextClay"),
    "equality-ignores-guard": (HT, "            && peakGuard == other.peakGuard\n            && peakGuardKeptPercent == other.peakGuardKeptPercent\n", "",
                               "读数相等判断漏了守护字段（亮灭变了界面不重画）", "HelpTextClay"),
    # ---- 第二批（矩阵跑完后补的：英文气泡、三行灯各自的归属、读数的前提、百分数的非有限）----
    "live-line-missing-en": (HT, '''            if (readout.peakGuard)
                parts.add (juce::String ("Peak guard pulling back ") + peakGuardStagesEn (readout) + ": only about "
                           + juce::String (readout.peakGuardKeptPercent) + "% left");

''', "", "英文气泡不写「Peak guard pulling back」", "HelpTextClay"),
    "mono-lamp-ignores-peak-guard": (HT, "                if (readout.phaseGuard || (readout.peakGuard && readout.monoPercent > 0))\n",
                                     "                if (readout.phaseGuard)\n", "收单灯不因守护回收变琥珀（只认反相提示）", "HelpTextClay"),
    "width-lamp-follows-peak-guard": (HT, "                if (readout.widthGuard)\n                    return ClayLamp::guard;\n\n                return readout.widthTenthsDb != 0",
                                      "                if (readout.widthGuard || readout.peakGuard)\n                    return ClayLamp::guard;\n\n                return readout.widthTenthsDb != 0",
                                      "加宽灯也跟着守护回收变琥珀（加宽不经守护，不该变）", "HelpTextClay"),
    "guard-without-stage": (HT, "readout.peakGuard = status.clayPeakGuardActive && (readout.lowCutHz > 0 || readout.monoPercent > 0);",
                            "readout.peakGuard = status.clayPeakGuardActive;", "低切与收单都没在做时也报回收中（没有东西可回收）", "HelpTextClay"),
    "percent-nonfinite": (HT, "            if (! std::isfinite (kept))\n                return 100;\n\n", "", "保留比非有限时不回 100（会写「只剩 0」之类）", "HelpTextClay"),
    "lamp-help-zh-missing": (HT, "；低切 / 收单灯琥珀 = 峰值守护把你设的量明显收回去了（气泡里写还剩约多少）", "", "灯说明（中文）没讲新灯色", "HelpTextClay"),
    "lamp-help-en-missing": (HT, "; amber low cut / mono = the peak guard is clearly pulling your setting back (the tooltip says roughly how much is left)", "",
                             "灯说明（英文）没讲新灯色", "HelpTextClay"),
}


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--list":
        for name, (path, _old, _new, what, classes) in VARIANTS.items():
            print(f"{name:26s} {classes:28s} {path}  —— {what}")
        return
    if len(sys.argv) != 3 or sys.argv[2] not in VARIANTS:
        raise SystemExit("用法：variants_t074.py <源码树根目录> <改法>（--list 看全部）")
    root = pathlib.Path(sys.argv[1])
    path, old, new, what, _classes = VARIANTS[sys.argv[2]]
    p = root / path
    text = p.read_text(encoding="utf-8")
    assert text.count(old) == 1, f"{sys.argv[2]}：{path} 里找不到（或不止一处）预期的文字（{text.count(old)} 处），副本与预期的版本对不上"
    p.write_text(text.replace(old, new), encoding="utf-8")
    print(f"已改成变体：{sys.argv[2]}（{what}）")


if __name__ == "__main__":
    main()
