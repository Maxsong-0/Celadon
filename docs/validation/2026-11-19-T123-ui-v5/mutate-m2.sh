#!/usr/bin/env bash
# T-123 M2 的改坏变体：每次只改坏一处、编一遍、跑 --only deck，看对应的检查有没有变红；跑完把文件换回去。
# 在 testbox 打包目录里跑（与 run.sh 同级）：./mutate-m2.sh
set -uo pipefail
cd "$(dirname "$0")"
export BUILD_ROOT="${BUILD_ROOT:-/cache/celadon-t123-mut}"
printf '%s\n' '! --out deck-mut --only deck' > mut.list

mutate() {   # mutate <名字> <文件> <原文> <换成>
  local name="$1" file="$2" old="$3" new="$4"
  cp "Celadon/$file" "/tmp/mutate-backup.$$"
  python3 - "Celadon/$file" "$old" "$new" <<'PY' || { echo "::mutation:: $name 没改成"; return; }
import sys
path, old, new = sys.argv[1], sys.argv[2].encode().decode('unicode_escape').encode('latin-1').decode('utf-8'), sys.argv[3].encode().decode('unicode_escape').encode('latin-1').decode('utf-8')
text = open(path, encoding='utf-8').read()
assert text.count(old) == 1, text.count(old)
open(path, 'w', encoding='utf-8').write(text.replace(old, new))
PY
  echo "::mutation-begin:: $name"
  ./run.sh ui mut.list > "/tmp/mutate-out.$$" 2>&1
  echo "run.sh 退出码 $?（清单里写了期望失败：0 = 检查按预期变红）"
  grep -E '^FAIL|deck checks|error:' "/tmp/mutate-out.$$" | cut -c1-420
  echo "PASS 行数：$(grep -c '^PASS' "/tmp/mutate-out.$$")"
  echo "::mutation-end:: $name"
  cat "/tmp/mutate-backup.$$" > "Celadon/$file"   # 换回原文；用 cat 写回，修改时间是现在，下一遍会重编
}

mutate 'A 卡片测试台不打开取样口（应红：really receives audio）' tools/ui-snapshot/SnapshotMain.cpp \
  '            tap.setActive (true);\n\n            auto card' '\n            auto card'
mutate 'B paint 里重建后不把画出的区域并进旧区域（应红：S8 增量重画）' Source/ui/SpectrumView.cpp \
  '            curveArea = curveArea.getUnion (curveRegion());\n' ''
mutate 'C 矮档照画 dB 刻度横线（应红：deck 114 的 dB scale）' Source/ui/SpectrumView.cpp \
  '                if (! compact)   // \xe7\x9f\xae\xe6\xa1\xa3\xe4\xb8\x8d\xe7\x94\xbb dB \xe5\x88\xbb\xe5\xba\xa6\xe7\x9a\x84\xe6\xa8\xaa\xe7\xba\xbf\xef\xbc\x88\xe8\xa7\x84\xe6\xa0\xbc V5 2.3\xef\xbc\x89\n' ''
mutate 'D 换高度时不作废曲线路径（应红：S12e deck resize）' Source/ui/SpectrumView.cpp \
  '        curvePathsValid = false;\n        curveArea = {};\n    }\n\n    void SpectrumView::layoutControls()' '        curveArea = {};\n    }\n\n    void SpectrumView::layoutControls()'
echo "::mutations-done::"
