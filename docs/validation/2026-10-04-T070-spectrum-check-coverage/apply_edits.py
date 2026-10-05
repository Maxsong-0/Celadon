#!/usr/bin/env python3
"""T-070：把评审（wf_3e784f42-2fa）指出的截图检查覆盖缺口补上——改 tools/ui-snapshot/SnapshotMain.cpp。
每条改动先断言原文恰好出现一次；新代码块放在同目录的 new_dirty_scenarios.cpp.txt、new_s12.cpp.txt。
用法：在仓库根执行 python3 docs/validation/2026-10-04-T070-spectrum-check-coverage/apply_edits.py"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent
P = ROOT / "tools/ui-snapshot/SnapshotMain.cpp"
t = P.read_text(encoding="utf-8")


def sub(old, new, what):
    global t
    n = t.count(old)
    if n != 1:
        sys.exit(f"{what}：原文出现 {n} 次（应为 1）：{old[:80]!r}")
    t = t.replace(old, new)


sub("#include <chrono>\n#include <cmath>\n", "#include <algorithm>\n#include <chrono>\n#include <cmath>\n", "include")

# --- 头注释
sub("    //   打开 PEAK、换斜率、悬停那一刻画的是此刻的数据（T-058 的 S10，runPeakFreshness）。\n",
    "    //   打开 PEAK、换斜率、悬停那一刻画的是此刻的数据（T-058 的 S10，runPeakFreshness）。\n"
    "    //   T-070（夜间评审补覆盖）：S9 斜率 0 与 4.5 各一份（零重建数真的重建，悬停也比）、S9b（PEAK 开 → 关）、S11（IN 开着：换斜率、IN 关 / 开）、\n"
    "    //   播放加悬停；S12 与「没有历史的全新视图」逐字节对照（停播后、稳态 IN 换斜率）+ 曲线 / 读数的绝对探针（OUT 填充、平均线、峰值线、IN 虚线、\n"
    "    //   悬停读数）——前面这些都是视图对它自己，两边一起错就全过，见 SpectrumTimeline 一带的说明。\n", "header")

sub("    //   增量重画 vs 整帧（评审 R-08 建议，守 R-02 那一类「脏区漏标、旧画面留在屏上」）：T-051 的 S8（守 ①），T-058 的 S9（守 ② 与\n"
    "    //   打开 PEAK 时的重建，另一行守「PEAK 关、只剩峰值在落时不重建」）与播放（评审 B1 的第一个场景）——场景表 dirtyScenarios；\n",
    "    //   增量重画 vs 整帧（评审 R-08 建议，守 R-02 那一类「脏区漏标、旧画面留在屏上」）：T-051 的 S8（守 ①），T-058 的 S9（守打开 PEAK 时的重建；\n"
    "    //   R-02 ② 在 T-058 上成了第二道保险，单独撤它 S9 照样全过，要验 ② 只能在 main 上撤，见 T-058 证据的 M 变体；另一行守「PEAK 关、只剩峰值\n"
    "    //   在落时不重建」）与播放（评审 B1 的第一个场景）——场景表 dirtyScenarios；\n", "header S9 wording")

# --- S10：第一个有不同的后续帧（评审 F10）
sub("        int switchDifferent = 0, laterFramesDifferent = 0, worst = 0;   // 那一刻不同的像素数；之后 30 帧里有不同的帧数\n",
    "        int switchDifferent = 0, laterFramesDifferent = 0, worst = 0;   // 那一刻不同的像素数；之后 30 帧里有不同的帧数\n"
    "        int firstLaterFrame = -1;                                       // 之后 30 帧里第一个有不同的帧（从 0 数；没有 = -1）\n", "FreshnessResult first frame")
sub("            if (result.laterFramesDifferent++ == 0 && result.switchDifferent == 0)\n                result.firstAt = at;\n",
    "            if (result.laterFramesDifferent++ == 0)\n            {\n                result.firstLaterFrame = i;\n\n                if (result.switchDifferent == 0)\n                    result.firstAt = at;\n            }\n", "FreshnessResult record")
sub('                       + " at that moment, " + juce::String (result.laterFramesDifferent) + " of 30 later frames differ"\n',
    '                       + " at that moment, " + juce::String (result.laterFramesDifferent) + " of 30 later frames differ"\n'
    '                       + (result.laterFramesDifferent > 0 ? ", first at frame " + juce::String (result.firstLaterFrame) : juce::String())\n', "FreshnessResult report")

# --- DirtyRun
sub("                const auto different = countDifferences (screen.image, reference, deviceArea, worst, at);\n",
    "                const auto different = countDifferences (screen.image, reference, deviceArea, worst, at, tolerance);\n", "tolerance use")
sub("    // 两张同尺寸、同格式的图在 area（设备像素）里比：返回不同的像素数；worst = 最大的通道差，at = 第一个不同的像素。\n"
    "    // 先按行逐字节比（相同的行不拆像素），对不上的行再逐像素量差多少。\n"
    "    int countDifferences (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area, int& worst, juce::Point<int>& at)\n",
    "    // 两张同尺寸、同格式的图在 area（设备像素）里比：返回不同的像素数；worst = 最大的通道差，at = 第一个不同的像素。\n"
    "    // 先按行逐字节比（相同的行不拆像素），对不上的行再逐像素量差多少；通道差不超过 tolerance 的像素不算不同（默认零）。\n"
    "    int countDifferences (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area, int& worst, juce::Point<int>& at, int tolerance = 0)\n", "countDifferences signature")
sub("                if (d == 0)\n                    continue;\n\n                if (different++ == 0)\n                    at = { x, y };\n",
    "                if (d <= tolerance)\n                    continue;\n\n                if (different++ == 0)\n                    at = { x, y };\n", "countDifferences tolerance")

sub("        // 连推 count 帧、每帧都比；返回其中的曲线帧数（isCurveFrame）。\n"
    "        int frames (const juce::String& stage, int count, const SpectrumRig::AudioSource& source)\n"
    "        {\n"
    "            int curveFrames = 0;\n"
    "\n"
    "            for (int i = 0; i < count; ++i)\n"
    "                curveFrames += isCurveFrame (frame (stage, i, source)) ? 1 : 0;\n"
    "\n"
    "            return curveFrames;\n"
    "        }\n",
    "        // 连推 count 帧、每帧都比；返回其中的曲线帧数（isCurveFrame）。这一次调用里数到的第一个曲线帧记在 firstCurveFrame（每次调用先清空；失败时好定位）。\n"
    "        int frames (const juce::String& stage, int count, const SpectrumRig::AudioSource& source)\n"
    "        {\n"
    "            int curveFrames = 0;\n"
    "            firstCurveFrame.clear();\n"
    "\n"
    "            for (int i = 0; i < count; ++i)\n"
    "            {\n"
    "                const auto region = frame (stage, i, source);\n"
    "\n"
    "                if (! isCurveFrame (region))\n"
    "                    continue;\n"
    "\n"
    "                if (curveFrames++ == 0)\n"
    "                    firstCurveFrame = stage + \" frame \" + juce::String (i) + \", dirty \" + region.getBounds().toString();\n"
    "            }\n"
    "\n"
    "            return curveFrames;\n"
    "        }\n"
    "\n"
    "        // 跑 body，返回这期间频谱仪重建路径的次数（SpectrumView::getCurveBuildCount 的差；T-070）。\n"
    "        int countBuilds (const std::function<void()>& body)\n"
    "        {\n"
    "            const auto before = rig.view->getCurveBuildCount();\n"
    "            body();\n"
    "            return (int) (rig.view->getCurveBuildCount() - before);\n"
    "        }\n", "DirtyRun frames")

lines = t.split("\n")
idx = [i for i, l in enumerate(lines) if l.startswith("        int counted = 0;   // 场景自己数的帧")]
if len(idx) != 1:
    sys.exit(f"DirtyRun.counted：找到 {len(idx)} 行")
lines.insert(idx[0] + 1, "        int builds = 0;    // 场景自己数的路径重建次数（countBuilds；S9：PEAK 关、只剩峰值在落的 30 帧里应为 0）")
lines.insert(idx[0] + 2, "        juce::String firstCurveFrame;   // 最近一次 frames() 数到的第一个曲线帧：阶段、帧序号、脏区外框")
lines.insert(idx[0] + 3, "        int tolerance = 0;   // 逐像素比时允许的通道差（零 = 逐字节相同；只有悬停场景放 1 级，见 hoverTolerance）")
t = "\n".join(lines)

# --- DirtyOutcome
sub("        std::vector<int> counted;           // DirtyRun::counted，每种像素比一个（×1、×2）\n",
    "        std::vector<int> counted;           // DirtyRun::counted，每种像素比一个（×1、×2）\n"
    "        std::vector<int> builds;            // DirtyRun::builds，同上\n"
    "        juce::StringArray firstCurve;       // DirtyRun::firstCurveFrame，同上\n", "DirtyOutcome members")
sub("        bool countedAllZero() const\n"
    "        {\n"
    "            for (const auto value : counted)\n"
    "                if (value != 0)\n"
    "                    return false;\n"
    "\n"
    "            return error.isEmpty();\n"
    "        }\n"
    "    };\n",
    "        bool countedAllZero() const\n"
    "        {\n"
    "            for (const auto value : counted)\n"
    "                if (value != 0)\n"
    "                    return false;\n"
    "\n"
    "            return error.isEmpty();\n"
    "        }\n"
    "\n"
    "        // 「×1 / ×2」两次数到的路径重建次数\n"
    "        juce::String buildsText() const\n"
    "        {\n"
    "            juce::StringArray parts;\n"
    "\n"
    "            for (const auto value : builds)\n"
    "                parts.add (juce::String (value));\n"
    "\n"
    "            return parts.joinIntoString (\" / \");\n"
    "        }\n"
    "\n"
    "        bool buildsAllZero() const\n"
    "        {\n"
    "            for (const auto value : builds)\n"
    "                if (value != 0)\n"
    "                    return false;\n"
    "\n"
    "            return error.isEmpty();\n"
    "        }\n"
    "\n"
    "        // 数到曲线帧时，第一个曲线帧在哪（失败信息用；没有就是空串）。\n"
    "        juce::String firstCurveText() const\n"
    "        {\n"
    "            for (const auto& where : firstCurve)\n"
    "                if (where.isNotEmpty())\n"
    "                    return \" (first: \" + where + \")\";\n"
    "\n"
    "            return {};\n"
    "        }\n"
    "    };\n", "DirtyOutcome methods")
sub("                outcome.counted.push_back (0);\n", "                outcome.counted.push_back (0);\n                outcome.builds.push_back (0);\n                outcome.firstCurve.add ({});\n", "runDirtyScenario error")
sub("            outcome.counted.push_back (run.counted);\n", "            outcome.counted.push_back (run.counted);\n            outcome.builds.push_back (run.builds);\n            outcome.firstCurve.add (run.firstCurveFrame);\n", "runDirtyScenario ok")

# --- 场景表整体替换
start_marker = "    // 增量重画检查的场景表（每个场景各起会话；×1 两种口径 + ×2 矩形列表 = 评审 B1 的三种口径）。\n    std::vector<DirtyScenario> dirtyScenarios()\n"
end_marker = "    // 打开 PEAK、换斜率、悬停那一刻画的是此刻的数据（T-058 的 S10）。残影检查拿同一个视图的整帧重画作对照"
a = t.index(start_marker)
b = t.index(end_marker)
assert t.count(start_marker) == 1 and t.count(end_marker) == 1
t = t[:a] + (HERE / "new_dirty_scenarios.cpp.txt").read_text(encoding="utf-8") + t[b:]

# --- S12 harness 放在 runSpectrumChecks 之前
sub("    void runSpectrumChecks (const std::function<void (bool, const juce::String&)>& check)\n    {\n",
    (HERE / "new_s12.cpp.txt").read_text(encoding="utf-8") + "    void runSpectrumChecks (const std::function<void (bool, const juce::String&)>& check)\n    {\n", "S12 insert")

# --- runSpectrumChecks 末尾：S12 的检查行
tail_old = ("                             : \"T-058 spectrum: the hover readout shows this moment's value (S10c: peak off, slope 0, mouse to 6 kHz, readout area only\" + setting);\n"
            "        }\n"
            "    }\n"
            "}\n")
tail_new = ("                             : \"T-058 spectrum: the hover readout shows this moment's value (S10c: peak off, slope 0, mouse to 6 kHz, readout area only\" + setting);\n"
            "        }\n"
            "\n"
            "        // S12（T-070）：长期运行的视图 == 没有历史的全新视图（停播后、稳态 IN 换斜率；PEAK 关 / 开各一遍）。\n"
            "        const auto reportFresh = [&check] (const FreshEquality& result, const juce::String& what)\n"
            "        {\n"
            "            check (result.error.isEmpty() && result.differing == 0,\n"
            "                   \"T-070 spectrum: a long-running view draws what a view built from scratch draws (\" + what + \": \"\n"
            "                       + (result.error.isNotEmpty() ? result.error\n"
            "                                                    : juce::String (result.moments - result.differing) + \" / \" + juce::String (result.moments)\n"
            "                                                          + \" moments identical\" + (result.differing > 0 ? \"; first: \" + result.firstDifference : juce::String()))\n"
            "                       + \")\");\n"
            "        };\n"
            "\n"
            "        reportFresh (runFreshAfterStop (false), \"S12a: 6 kHz -12 dBFS sine 2 s, silence, peak off, slope 0 -> 4.5, hover\");\n"
            "        reportFresh (runFreshAfterStop (true), \"S12a: 6 kHz -12 dBFS sine 2 s, silence, peak on, slope 0 -> 4.5, hover\");\n"
            "        reportFresh (runFreshSteadyInput (false), \"S12b: IN on, peak off, 6 kHz -12 dBFS sine 10 s steady, slope 0 -> 3 -> 4.5\");\n"
            "        reportFresh (runFreshSteadyInput (true), \"S12b: IN on, peak on, 6 kHz -12 dBFS sine 10 s steady, slope 0 -> 3 -> 4.5\");\n"
            "        reportFresh (runFreshDecay(), \"S12c: white noise 2 s, silence, peak off, slope 4.5, tolerance 24 levels\");\n"
            "        reportFresh (runFreshSteady (0), \"S12d: 6 kHz -12 dBFS sine steady, peak off, slope 0, tolerance 24 levels\");\n"
            "        reportFresh (runFreshSteady (2), \"S12d: 6 kHz -12 dBFS sine steady, peak off, slope 4.5, tolerance 24 levels\");\n"
            "\n"
            "        // 绝对探针（T-070）：曲线 / 读数真的画在计算器给的位置，不靠任何历史。\n"
            "        const auto reportProbe = [&check] (const ProbeResult& result, const juce::String& what)\n"
            "        {\n"
            "            check (result.ok, \"T-070 spectrum draws the data: \" + what + \" (\" + result.detail + \")\");\n"
            "        };\n"
            "\n"
            "        reportProbe (probeOutputFill(), \"S12 probe OUT fill\");\n"
            "        reportProbe (probeAverage(), \"S12 probe average line\");\n"
            "        reportProbe (probePeak(), \"S12 probe peak hold line\");\n"
            "        reportProbe (probeInput(), \"S12 probe IN dashes\");\n"
            "        reportProbe (probeHoverReadout (0), \"S12 probe hover readout, slope 0\");\n"
            "        reportProbe (probeHoverReadout (2), \"S12 probe hover readout, slope 4.5\");\n"
            "    }\n"
            "}\n")
sub(tail_old, tail_new, "runSpectrumChecks tail")

P.write_text(t, encoding="utf-8")
print("SnapshotMain.cpp 已改")
