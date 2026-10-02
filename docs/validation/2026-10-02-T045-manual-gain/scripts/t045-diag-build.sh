#!/usr/bin/env bash
# T-045 诊断（不进仓库）：临时副本里给 celadon_render 加 CELADON_DIAG_LIMIT_GAIN（手动 Limit Gain，dB）与两列方案标志。标记 DEBUG-T045。
set -euo pipefail
B=$HOME/.cache/celadon-t045-diag
SRC=$HOME/.cache/celadon-tests/house-mastering-workflow-67aa29-48064f395b0a
rm -rf "$B/Celadon" "$B/build" && mkdir -p "$B"
cp -r "$SRC/Celadon" "$B/Celadon"; ln -sfn "$SRC/JUCE" "$B/JUCE"
python3 - "$B/Celadon/tools/render/RenderMain.cpp" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
def rep(a,b):
    global s
    assert s.count(a)==1, a[:50]; s=s.replace(a,b,1)
rep("""        p.glazeDelivery = combo.delivery;
""","""        p.glazeDelivery = combo.delivery;
        if (const char* diagGain = std::getenv ("CELADON_DIAG_LIMIT_GAIN"))   // DEBUG-T045
            p.limit.gainDb = (float) std::atof (diagGain);
""")
rep('''               "segments_file";''','''               "segments_file,plan_limited_by_manual,plan_level_withdrawn";   // DEBUG-T045''')
rep("""            row.segmentsFile,
        };""","""            row.segmentsFile,
            planned ? std::string (c.plan.limitedByManual ? "1" : "0") : empty,          // DEBUG-T045
            planned ? std::string (c.plan.automaticLevelWithdrawn ? "1" : "0") : empty,  // DEBUG-T045
        };""")
if '#include <cstdlib>' not in s:
    s = '#include <cstdlib>   // DEBUG-T045\n' + s
open(p,'w').write(s)
PY
cmake -S "$B/Celadon" -B "$B/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$B/cmake.log" 2>&1
nice -n 5 cmake --build "$B/build" --target celadon_render > "$B/build.log" 2>&1 || { tail -30 "$B/build.log"; exit 1; }
"$B/build/celadon_render" --selftest-house 2>&1 | tail -1
grep -c "DEBUG-T045" "$B/Celadon/tools/render/RenderMain.cpp"
