#!/usr/bin/env python3
"""T-072 验证用（只在临时副本里用，绝不进仓库的源码）：给 PluginEditor.cpp 的 advanceSpectrum 注入一个可由环境变量控制的停顿，
模拟「这一帧耗时很长」（慢机、Debug、机器被占满）：CELADON_T072_STALL_MS 毫秒的停顿，每 CELADON_T072_STALL_EVERY 次推频谱仪里有一次
（默认每次；--deterministic 每张图要推约 900 帧，每帧都停要好几个小时）。CELADON_T072_STALL_MS 没设时什么都不做。
先断言锚点文字恰好出现一次，对不上就停。用法：inject_stall.py <PluginEditor.cpp>"""
import pathlib
import sys

ANCHOR = "    lastSpectrumAdvanceMs = nowMs;\n"
INJECT = ("    if (const auto* stall = std::getenv (\"CELADON_T072_STALL_MS\"))   // 验证副本专用：每隔几次有一次耗时很长\n"
          "    {\n"
          "        static int calls = 0;\n"
          "        const auto* every = std::getenv (\"CELADON_T072_STALL_EVERY\");\n"
          "\n"
          "        if (++calls % (every != nullptr ? std::max (1, std::atoi (every)) : 1) == 0)\n"
          "            juce::Thread::sleep (std::atoi (stall));\n"
          "    }\n")
INCLUDE_ANCHOR = "#include \"PluginEditor.h\"\n"

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
assert text.count(ANCHOR) == 1, f"锚点出现 {text.count(ANCHOR)} 次（应为 1）"
assert text.count(INCLUDE_ANCHOR) == 1, f"include 锚点出现 {text.count(INCLUDE_ANCHOR)} 次（应为 1）"
text = text.replace(ANCHOR, ANCHOR + INJECT)
text = text.replace(INCLUDE_ANCHOR, INCLUDE_ANCHOR + "#include <cstdlib>\n")
path.write_text(text, encoding="utf-8")
print("已注入停顿（环境变量 CELADON_T072_STALL_MS，毫秒）")
