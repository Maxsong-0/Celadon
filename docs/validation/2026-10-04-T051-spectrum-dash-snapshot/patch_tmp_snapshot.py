#!/usr/bin/env python3
# 只在 main 的临时副本里用：main 没有 createDashedStrokeReusing，把 IN 虚线那条检查里的调用换成 JUCE 自己的，让文件能编
# （这一条在临时副本里不验东西；要验的是同一文件里的 S8 增量重画检查）。用法：patch_tmp_snapshot.py <SnapshotMain.cpp>
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
s = p.read_text(encoding='utf-8')
old = "createDashedStrokeReusing (stroke, line, dashes, 2, centre, reused);"
assert s.count(old) == 1, '没找到要换的那一行'
s = s.replace(old, "stroke.createDashedStroke (reused, line, dashes, 2); juce::ignoreUnused (centre);   // 临时副本：main 没有 createDashedStrokeReusing")
p.write_text(s, encoding='utf-8')
print('已替换')
