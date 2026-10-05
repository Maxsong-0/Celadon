#!/usr/bin/env bash
# check-svg-text.mjs 的反向验证：在站点的临时副本里把 wrapWords / figureBudget 改坏七种，看检查脚本是不是每次都变红。
# 用法：mutation_check_svg_text.sh <装好依赖的站点目录（含 node_modules）>   只改 src/lib 里两个文件，跑完还原。
set -eu
S=$1
cd "$S"
cp src/lib/wrapWords.ts /tmp/wrapWords.orig.ts; cp src/lib/figureBudget.ts /tmp/figureBudget.orig.ts
run() { node scripts/check-svg-text.mjs 2>&1 | head -2 | cut -c1-200; }
restore() { cp /tmp/wrapWords.orig.ts src/lib/wrapWords.ts; cp /tmp/figureBudget.orig.ts src/lib/figureBudget.ts; }
echo "=== M0 原样"; run
echo "=== M1 标点可落行首（NO_LINE_START 置空）"; sed -i 's#^const NO_LINE_START = .*#const NO_LINE_START = /(?!)/;#' src/lib/wrapWords.ts; run; restore
echo "=== M2 左括号可落行尾（NO_LINE_END 置空）"; sed -i 's#^const NO_LINE_END = .*#const NO_LINE_END = /(?!)/;#' src/lib/wrapWords.ts; run; restore
echo "=== M3 中日韩不能按字断（CJK 置空）"; sed -i 's#^const CJK = .*#const CJK = /(?!)/;#' src/lib/wrapWords.ts; run; restore
echo "=== M4 行尾不补空格"; sed -i 's#lines.push(space ? `${line} ` : line);#lines.push(line);#' src/lib/wrapWords.ts; run; restore
echo "=== M5 数字与单位不粘住"; sed -i "s#const glued = text.replace(.*#const glued = text;#" src/lib/wrapWords.ts; run; restore
echo "=== M6 LIMIT 图注只留 1 行"; sed -i 's#limit: { x: 16, maxLines: 2 }#limit: { x: 16, maxLines: 1 }#' src/lib/figureBudget.ts; run; restore
echo "=== M7 页眉说明预算砍到 300"; sed -i 's#noteSlack: 20#noteSlack: 260#' src/lib/figureBudget.ts; run; restore
echo "=== 还原后"; run
