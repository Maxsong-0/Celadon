#!/usr/bin/env bash
set -euo pipefail
B=$HOME/.cache/celadon-t045-diag
for label in old new; do
  D=$B/src-$label
  python3 - "$D/Celadon/tools/render/RenderMain.cpp" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
def rep(a,b):
    global s
    assert s.count(a)==1, a[:60]; s=s.replace(a,b,1)
rep("""        double grMeanDb = 0.0, grMaxDb = 0.0;
""","""        double grMeanDb = 0.0, grMaxDb = 0.0;
        double diagProtectPct = 0.0, diagLimitedPct = 0.0, diagGrP95 = 0.0;   // DEBUG-T045
""")
rep("""            row.grMeanDb = render.grMeanDb;
""","""            row.grMeanDb = render.grMeanDb;
            if (render.diagSamples > 0.0)                                                                     // DEBUG-T045
            {
                row.diagProtectPct = 100.0 * render.diagProtectSamples / render.diagSamples;
                row.diagLimitedPct = 100.0 * render.diagLimitedSamples / render.diagSamples;
                auto gr = render.diagGr;
                std::sort (gr.begin(), gr.end());
                row.diagGrP95 = gr.empty() ? 0.0 : gr[(size_t) (0.95 * (double) (gr.size() - 1))];
            }
""")
rep('''"segments_file,plan_limited_by_manual,plan_level_withdrawn";   // DEBUG-T045''','''"segments_file,plan_limited_by_manual,plan_level_withdrawn,diag_protect_pct,diag_plan_limited_pct,diag_limit_gr_p95_db";   // DEBUG-T045''')
rep("""            planned ? std::string (c.plan.automaticLevelWithdrawn ? "1" : "0") : empty,  // DEBUG-T045
""","""            planned ? std::string (c.plan.automaticLevelWithdrawn ? "1" : "0") : empty,  // DEBUG-T045
            render ? fixed (row.diagProtectPct, 2) : empty,                                // DEBUG-T045
            render ? fixed (row.diagLimitedPct, 2) : empty,                                // DEBUG-T045
            render ? fixed (row.diagGrP95, 3) : empty,                                     // DEBUG-T045
""")
if '#include <algorithm>' not in s:
    s = '#include <algorithm>   // DEBUG-T045\n' + s
open(p,'w').write(s)
PY
  if [ "$label" = new ]; then
    F=$D/Celadon/Source/dsp/GlazeConstants.cpp
    sed -i 's/warm.limitGrPeakCapDb = 4.0f;/warm.limitGrPeakCapDb = 6.0f;/; s/punchy.limitGrPeakCapDb = 3.0f;/punchy.limitGrPeakCapDb = 5.0f;/; s/clean.limitGrPeakCapDb = 6.0f;/clean.limitGrPeakCapDb = 8.0f;/' "$F"
    grep -c "limitGrPeakCapDb = [568].0f" "$F"
  fi
  cmake -S "$D/Celadon" -B "$D/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$D/cmake.log" 2>&1
  nice -n 5 cmake --build "$D/build" --target celadon_render > "$D/build.log" 2>&1 || { tail -20 "$D/build.log"; exit 1; }
  "$D/build/celadon_render" --selftest-house 2>&1 | tail -1
done
