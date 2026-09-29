# T-027 反向验证（在 107 的同步副本里跑，经 scripts/test-linux.sh 传入；本机不编译不运行）：
#   R1 缓存键去掉约束反馈上限；R2 缓存键从 samePlanningManual 缩成只比 Clay 三态设置；R3 缓存键忽略 v0.1.9.0 的「Mast」段各项。
# 三种都必须让 T-027 1 失败；最后把副本改回原样并重编，构建目录回到真实源码。
# 用法：scripts/test-linux.sh bash -c "$(cat docs/validation/2026-09-29-T027-reference-cache/reverse.sh)"
set -uo pipefail
F=Celadon/Source/dsp/GlazeController.cpp
ORIG="$(mktemp)"
cp "$F" "$ORIG"

run_variant ()
{
  echo "==== $1"
  grep -n "manualConditionsKnown ||" -A1 "$F"
  cmake --build "$BUILD" --target celadon_tests > /dev/null || { echo "build failed"; return; }
  "$BUILD/celadon_tests" --only GlazeController | grep -E "T-027|!!! Test|FAILED|blocks checked|celadon_tests:" | cut -c1-400
}

sed -i 's/|| ! sameBits (feedbackMaxGainDb, manualConditionsFeedback))/)/' "$F"
run_variant "R1: key without the constraint cap"

cp "$ORIG" "$F"
sed -i 's/! samePlanningManual (m, manualConditionsInput)/! sameClaySettings (m.claySettings, manualConditionsInput.claySettings)/' "$F"
run_variant "R2: key compares only the Clay settings"

cp "$ORIG" "$F"
python3 - "$F" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
old = "! samePlanningManual (m, manualConditionsInput)"
new = ("! samePlanningManual ([&] { auto x = m; auto& b = x.balance; const auto& c = manualConditionsInput.balance; "
       "b.harshAmount = c.harshAmount; b.transientPreserve = c.transientPreserve; b.targetAmount = c.targetAmount; "
       "b.targetSlopeDbPerOct = c.targetSlopeDbPerOct; b.hasTargetReference = c.hasTargetReference; "
       "b.targetReferenceDb = c.targetReferenceDb; return x; }(), manualConditionsInput)")
assert text.count(old) == 1
open(path, "w", encoding="utf-8").write(text.replace(old, new))
PY
run_variant "R3: key ignores the v0.1.9.0 advanced-mastering fields (the Mast segment)"

cp "$ORIG" "$F"
rm -f "$ORIG"
cmake --build "$BUILD" --target celadon_tests > /dev/null && echo "==== restored and rebuilt"
sha256sum "$F"
