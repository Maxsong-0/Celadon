#!/usr/bin/env bash
# T-045 诊断 2（不进仓库）：在诊断变体上再加保护层介入占比、方案受限占比、限幅 GR P95 三列；编旧预算（4/3/6）与新预算（6/5/8）两个。DEBUG-T045。
set -euo pipefail
B=$HOME/.cache/celadon-t045-diag
for label in old new; do
  D=$B/src-$label
  rm -rf "$D" && mkdir -p "$D"
  cp -r "$B/Celadon" "$D/Celadon"; ln -sfn "$(readlink -f "$B/JUCE")" "$D/JUCE"
  python3 - "$D/Celadon/tools/render/RenderMain.cpp" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
def rep(a,b):
    global s
    assert s.count(a)==1, a[:60]; s=s.replace(a,b,1)
rep("""        double grMeanDb = 0.0;             // 限幅 GR：各块读数按块长加权平均
        double grMaxDb = 0.0;
""","""        double grMeanDb = 0.0;             // 限幅 GR：各块读数按块长加权平均
        double grMaxDb = 0.0;
        double diagProtectSamples = 0.0, diagLimitedSamples = 0.0, diagSamples = 0.0;   // DEBUG-T045
        std::vector<double> diagGr;                                                      // DEBUG-T045
""")
rep("""            stats.addBlock (*chain, length, result);

            if (recordReductions)
                result.reductions.push_back ({ static_cast<std::int64_t> (position) - latency, length, stats.reduction });

            if (trace)""","""            stats.addBlock (*chain, length, result);
            result.diagSamples += length;                                                               // DEBUG-T045
            if (stats.status.protectionDb < -0.01f) result.diagProtectSamples += length;                // DEBUG-T045
            if (stats.status.planLimited) result.diagLimitedSamples += length;                          // DEBUG-T045
            result.diagGr.push_back (stats.reduction);                                                  // DEBUG-T045

            if (recordReductions)
                result.reductions.push_back ({ static_cast<std::int64_t> (position) - latency, length, stats.reduction });

            if (trace)""")
open(p,'w').write(s)
PY
  grep -c "DEBUG-T045" "$D/Celadon/tools/render/RenderMain.cpp"
done
