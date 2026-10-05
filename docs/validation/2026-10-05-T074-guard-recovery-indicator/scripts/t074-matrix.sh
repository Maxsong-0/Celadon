#!/usr/bin/env bash
# T-074 验证第四阶段·红绿矩阵：在临时副本（t074-mut）里把守护回收指示的实现改坏成各种样子（variants_t074.py），
# 每个改法：套上 → 增量编译 → 跑 ClayModule / ClayGuard / HelpTextClay / GlazeController 四个测试类（UI 两个改法再跑截图 --only check）→ 还原。
# 先跑一遍对照（原样，应全绿）。输出：progress7.txt（每个改法一行）、mut/<改法>.*.txt。
set -u
T=/tmp/claude-1000/n73
OUT=$T/t074
SPARK=$HOME/.cache/celadon-spark/bin/spark-test.sh
SRC=$HOME/.cache/celadon-spark/src
W=/home/maxsong/All-Project/Celadon/.claude/worktrees/complete-remaining-tasks-cdaccd
V=$W/docs/validation/2026-10-05-T074-guard-recovery-indicator/variants_t074.py
MUT=$SRC/t074-mut
PRIS=$SRC/t074-mut-pristine
P=$OUT/progress7.txt; : > $P
mkdir -p $OUT/mut
MUTBUILD=$("$SPARK" $MUT dir)
CLASSES=ClayModule,ClayGuard,HelpTextClay,GlazeController

restore() {   # 把六个可能被改坏的文件还原成原样
  for f in Source/dsp/ClayModule.cpp Source/dsp/ClayGuard.cpp Source/dsp/ClayGuard.h Source/dsp/CeladonChain.cpp Source/dsp/GlazeController.cpp Source/ui/HelpText.cpp; do
    cmp -s $PRIS/$f $MUT/$f || cp $PRIS/$f $MUT/$f
  done
}

run_snapshot_check() {   # run_snapshot_check <标签>：截图 --only check，返回 PASS / FAIL 数
  local tag=$1
  rm -rf $OUT/mut/cwd-$tag $OUT/mut/home-$tag; mkdir -p $OUT/mut/cwd-$tag $OUT/mut/home-$tag/.config
  "$SPARK" $MUT run bash -c "cd $OUT/mut/cwd-$tag && HOME=$OUT/mut/home-$tag XDG_CONFIG_HOME=$OUT/mut/home-$tag/.config xvfb-run -a -s '-screen 0 1280x1024x24' $MUTBUILD/celadon_ui_snapshot --only check > $OUT/mut/$tag.check.txt 2>&1"
  echo "PASS=$(grep -ac '^PASS' $OUT/mut/$tag.check.txt) FAIL=$(grep -ac '^FAIL' $OUT/mut/$tag.check.txt)"
}

one() {   # one <名字>
  local name=$1 targets="celadon_tests"
  case $name in lamp-ignores-guard|equality-ignores-guard|control) targets="celadon_tests celadon_ui_snapshot";; esac
  if [ "$name" != control ]; then
    python3 $V $MUT $name > $OUT/mut/$name.apply.txt 2>&1 || { echo "$name | 套不上：$(tail -n 1 $OUT/mut/$name.apply.txt | cut -c1-160)" >> $P; restore; return; }
  fi
  SPARK_JOBS=${SPARK_JOBS:-12} "$SPARK" $MUT build $targets > $OUT/mut/$name.build.txt 2>&1
  if [ $? -ne 0 ]; then echo "$name | 编译失败（见 mut/$name.build.txt）" >> $P; restore; return; fi
  # 对照（原样）这一份测试二进制就是改后的干净版，另存给性能对照用（含临时性能对照源文件）。
  [ "$name" = control ] && cp "$MUTBUILD/celadon_tests" $T/bench-new-tests
  "$SPARK" $MUT run "$MUTBUILD/celadon_tests" --only $CLASSES > $OUT/mut/$name.test.txt 2>&1
  local code=$?
  local summary; summary=$(grep -a '^==== celadon_tests' $OUT/mut/$name.test.txt | tail -n 1)
  local firstfails; firstfails=$(grep -a '^!!! ' $OUT/mut/$name.test.txt | head -n 3 | cut -c1-150 | tr '\n' '|')
  local snap=""
  case $name in lamp-ignores-guard|equality-ignores-guard|control) snap=" | 截图检查 $(run_snapshot_check $name)";; esac
  echo "$name | 退出码 $code | $summary$snap | $firstfails" >> $P
  restore
}

one control
for name in $(python3 $V --list | awk '{print $1}'); do
  one $name
done
restore
echo done >> $P
