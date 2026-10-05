#!/usr/bin/env bash
# 在 Linux 上跑 scripts/au-version.sh 的用例：用法 test-au-version.sh <要测的 bash>（bash 3.2 = macOS 的 /bin/bash）。
# 前提：Projucer 9.0.2 重存过工程（要 Builds/MacOSX/Info-AU.plist 与 project.pbxproj），
# 且 PlistBuddy-shim.py 装在 /usr/libexec/PlistBuddy（Python plistlib 写的替身，只模拟 Print / Set）。
set -uo pipefail
SH="$1"
REPO="${REPO:-$(cd "$(dirname "$0")/../../../.." && pwd)}"
SCRIPT="$REPO/scripts/au-version.sh"
GEN="$REPO/Builds/MacOSX/Info-AU.plist"     # real Projucer 9.0.2 output for 0.1.8.8
W="$(mktemp -d)"
pass=0; fail=0
ok()   { pass=$((pass + 1)); echo "PASS $1"; }
bad()  { fail=$((fail + 1)); echo "FAIL $1"; }
pb()   { /usr/libexec/PlistBuddy -c "$1" "$2"; }
setver() { python3 - "$1" "$2" <<'EOF'
import plistlib, sys
p, v = sys.argv[1], sys.argv[2]
raw = open(p, "rb").read()
fmt = plistlib.FMT_BINARY if raw.startswith(b"bplist") else plistlib.FMT_XML
d = plistlib.loads(raw); d["CFBundleShortVersionString"] = v
open(p, "wb").write(plistlib.dumps(d, fmt=fmt))
EOF
}
tobinary() { python3 -c "import plistlib,sys; p=sys.argv[1]; d=plistlib.load(open(p,'rb')); open(p,'wb').write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY))" "$1"; }

# 1. XML plist, 0.1.8.8, 264 -> 67592, one log line
cp "$GEN" "$W/a.plist"
out="$("$SH" "$SCRIPT" "$W/a.plist")"; rc=$?
[ $rc -eq 0 ] && [ "$(pb 'Print :AudioComponents:0:version' "$W/a.plist")" = 67592 ] && echo "$out" | grep -q '264 → 67592（0x10808，0.1.8.8）' && ok "xml 264->67592: $out" || bad "xml rewrite rc=$rc out=$out"
# other keys untouched
python3 - "$GEN" "$W/a.plist" <<'EOF' && ok "only AudioComponents.version changed" || bad "other keys changed"
import plistlib, sys
a = plistlib.load(open(sys.argv[1], "rb")); b = plistlib.load(open(sys.argv[2], "rb"))
assert a["AudioComponents"][0]["version"] == 264 and b["AudioComponents"][0]["version"] == 67592
a["AudioComponents"][0]["version"] = 67592
assert a == b
EOF

# 2. second run: no output, file not rewritten
before="$(stat -c '%Y %i %s' "$W/a.plist")"; sleep 1
out="$("$SH" "$SCRIPT" "$W/a.plist")"; rc=$?
[ $rc -eq 0 ] && [ -z "$out" ] && [ "$(stat -c '%Y %i %s' "$W/a.plist")" = "$before" ] && ok "idempotent, untouched" || bad "second run rc=$rc out=$out"

# 3. binary plist
cp "$GEN" "$W/b.plist"; tobinary "$W/b.plist"
"$SH" "$SCRIPT" "$W/b.plist" >/dev/null && [ "$(pb 'Print :AudioComponents:0:version' "$W/b.plist")" = 67592 ] && head -c 6 "$W/b.plist" | grep -q bplist && ok "binary plist" || bad "binary plist"

# 4. three-segment versions keep JUCE's value
cp "$GEN" "$W/c.plist"; setver "$W/c.plist" 0.6.1; python3 -c "import plistlib,sys; p=sys.argv[1]; d=plistlib.load(open(p,'rb')); d['AudioComponents'][0]['version']=0x601; plistlib.dump(d, open(p,'wb'))" "$W/c.plist"
out="$("$SH" "$SCRIPT" "$W/c.plist")"; [ $? -eq 0 ] && [ -z "$out" ] && [ "$(pb 'Print :AudioComponents:0:version' "$W/c.plist")" = 1537 ] && ok "0.6.1 stays 0x601" || bad "0.6.1 out=$out"

# 5. leading zeros are decimal, not octal
cp "$GEN" "$W/d.plist"; setver "$W/d.plist" 0.1.08.09
"$SH" "$SCRIPT" "$W/d.plist" >/dev/null && [ "$(pb 'Print :AudioComponents:0:version' "$W/d.plist")" = $(( (1<<16) + (8<<8) + 9 )) ] && ok "leading zeros 0.1.08.09 -> 0x10809" || bad "leading zeros"

# 6. future versions keep increasing
expect() { cp "$GEN" "$W/e.plist"; setver "$W/e.plist" "$1"; "$SH" "$SCRIPT" "$W/e.plist" >/dev/null && [ "$(pb 'Print :AudioComponents:0:version' "$W/e.plist")" = "$2" ] && ok "$1 -> $2" || bad "$1 expected $2 got $(pb 'Print :AudioComponents:0:version' "$W/e.plist")"; }
expect 0.1.8.9 $((0x10809)); expect 0.1.9.0 $((0x10900)); expect 0.2.0.0 $((0x20000)); expect 1.0.0.0 $((0x1000000)); expect 0.1.8 264

# 7. bad versions fail loudly and leave the file alone
for v in 0.1.256.0 0.1 0.1.8.8.1 0.1.8.x ""; do
  cp "$GEN" "$W/f.plist"; setver "$W/f.plist" "$v"; sum="$(md5sum < "$W/f.plist")"
  err="$("$SH" "$SCRIPT" "$W/f.plist" 2>&1 >/dev/null)"; rc=$?
  [ $rc -ne 0 ] && [ "$(md5sum < "$W/f.plist")" = "$sum" ] && ok "rejects '$v': $err" || bad "bad version '$v' rc=$rc"
done

# 8. non-AU plist and missing file
cp "$REPO/Builds/MacOSX/Info-VST3.plist" "$W/g.plist"
err="$("$SH" "$SCRIPT" "$W/g.plist" 2>&1)"; [ $? -ne 0 ] && ok "rejects non-AU plist: $err" || bad "non-AU accepted"
err="$("$SH" "$SCRIPT" "$W/nope.plist" 2>&1)"; [ $? -ne 0 ] && ok "rejects missing file: $err" || bad "missing accepted"
cp "$GEN" "$W/i.plist"; python3 -c "import plistlib,sys; p=sys.argv[1]; d=plistlib.load(open(p,'rb')); del d['CFBundleShortVersionString']; plistlib.dump(d, open(p,'wb'))" "$W/i.plist"
err="$("$SH" "$SCRIPT" "$W/i.plist" 2>&1)"; [ $? -ne 0 ] && [ -n "$err" ] && ok "rejects plist without version: $err" || bad "no-version accepted err=$err"

# 9. two AudioComponents entries both rewritten
cp "$GEN" "$W/h.plist"; python3 -c "import plistlib,sys,copy; p=sys.argv[1]; d=plistlib.load(open(p,'rb')); d['AudioComponents'].append(copy.deepcopy(d['AudioComponents'][0])); plistlib.dump(d, open(p,'wb'))" "$W/h.plist"
out="$("$SH" "$SCRIPT" "$W/h.plist")"; [ "$(pb 'Print :AudioComponents:0:version' "$W/h.plist")$(pb 'Print :AudioComponents:1:version' "$W/h.plist")" = 6759267592 ] && [ "$(echo "$out" | wc -l)" -eq 2 ] && ok "two components" || bad "two components out=$out"

# 10. Xcode post-build mode, as the generated phase runs it: sh -c with Xcode's environment
mkdir -p "$W/proj/Builds/MacOSX/build/Release/Celadon.component/Contents" "$W/proj/scripts"
cp "$SCRIPT" "$W/proj/scripts/"; cp "$GEN" "$W/proj/Builds/MacOSX/Info-AU.plist"; cp "$GEN" "$W/proj/Builds/MacOSX/build/Release/Celadon.component/Contents/Info.plist"
phase="$(python3 -c "
import re; t=open('$REPO/Builds/MacOSX/Celadon.xcodeproj/project.pbxproj').read()
s=re.search(r'name = \"Run Post-Build Script\";.*?shellScript = \"((?:[^\"\\\\]|\\\\.)*)\";', t, re.S).group(1)
print(s.encode().decode('unicode_escape'))")"
echo "generated phase script: $phase"
xenv() { env -i PATH="$(dirname "$SH"):/usr/bin:/bin" PROJECT_DIR="$W/proj/Builds/MacOSX" SRCROOT="$W/proj/Builds/MacOSX" \
  TARGET_BUILD_DIR="$W/proj/Builds/MacOSX/build/Release" INFOPLIST_PATH=Celadon.component/Contents/Info.plist INFOPLIST_FILE=Info-AU.plist "$@"; }
for ext in "" a vst3 app; do
  out="$(xenv WRAPPER_EXTENSION="$ext" /bin/sh -c "$phase" 2>&1)"; rc=$?
  [ $rc -eq 0 ] && [ -z "$out" ] && [ "$(pb 'Print :AudioComponents:0:version' "$W/proj/Builds/MacOSX/Info-AU.plist")" = 264 ] && ok "skips WRAPPER_EXTENSION='$ext'" || bad "ext '$ext' rc=$rc out=$out"
done
out="$(xenv WRAPPER_EXTENSION=component /bin/sh -c "$phase" 2>&1)"; rc=$?
echo "$out"
[ $rc -eq 0 ] && [ "$(pb 'Print :AudioComponents:0:version' "$W/proj/Builds/MacOSX/build/Release/Celadon.component/Contents/Info.plist")" = 67592 ] \
  && [ "$(pb 'Print :AudioComponents:0:version' "$W/proj/Builds/MacOSX/Info-AU.plist")" = 67592 ] && [ "$(echo "$out" | wc -l)" -eq 2 ] && ok "component: product + Info-AU.plist" || bad "component rc=$rc"
out="$(xenv WRAPPER_EXTENSION=component /bin/sh -c "$phase" 2>&1)"; [ $? -eq 0 ] && [ -z "$out" ] && ok "component rerun is a no-op" || bad "rerun out=$out"
mv "$W/proj/Builds/MacOSX/build/Release/Celadon.component/Contents/Info.plist" "$W/proj/Info.plist.moved"
out="$(xenv WRAPPER_EXTENSION=component /bin/sh -c "$phase" 2>&1)"; [ $? -ne 0 ] && ok "missing product plist fails the phase: $out" || bad "missing product plist passed"

echo "== $pass passed, $fail failed ($("$SH" --version | head -1))"
[ "$fail" -eq 0 ]
