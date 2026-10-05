#!/usr/bin/env bash
# T-074：官网构建（第二次）：临时目录里没有 ../Celadon.jucer，版本号经 CELADON_VERSION 给（与 build-linux.sh 同一做法）
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
W=/home/maxsong/All-Project/Celadon/.claude/worktrees/complete-remaining-tasks-cdaccd
D=$T/site-t074
P=$OUT/progress9.txt; : > $P
VERSION=$(tr '\n' ' ' < $W/Celadon.jucer | grep -o '<JUCERPROJECT[^>]*' | grep -o ' version="[0-9][0-9.]*"' | head -n 1 | grep -o '[0-9][0-9.]*')
echo "version=$VERSION $(date +%T)" >> $P
rsync -a --delete --exclude node_modules --exclude dist $W/site/ $D/
cd $D || exit 1
CELADON_VERSION=$VERSION pnpm run build > $OUT/site-build.txt 2>&1
echo "build exit=$? $(date +%T)" >> $P
echo done >> $P
