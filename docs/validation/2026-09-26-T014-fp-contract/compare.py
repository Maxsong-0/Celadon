#!/usr/bin/env python3
# T-014：两个配置目录里同名 .bin 逐位比较（float32 小端）。输出：不同的个数、最大 ulp 差、最大绝对差。
import struct, sys, os

def load(path):
    with open(path, "rb") as f:
        raw = f.read()
    n = len(raw) // 4
    return struct.unpack("<%dI" % n, raw), struct.unpack("<%df" % n, raw)

def ordered(u):
    # float 位型映射成单调整数，ulp 差 = 两者之差
    return (0x80000000 - (u & 0x7fffffff)) if (u & 0x80000000) else (0x80000000 + u)

a_dir, b_dir = sys.argv[1], sys.argv[2]
for name in sorted(os.listdir(a_dir)):
    if not name.endswith(".bin"):
        continue
    ua, fa = load(os.path.join(a_dir, name))
    ub, fb = load(os.path.join(b_dir, name))
    diff = maxulp = 0
    maxabs = 0.0
    for x, y, p, q in zip(ua, ub, fa, fb):
        if x != y:
            diff += 1
            maxulp = max(maxulp, abs(ordered(x) - ordered(y)))
            if p == p and q == q:
                maxabs = max(maxabs, abs(p - q))
    print("  %-22s %8d / %-8d differ   max %6d ulp   max |diff| %.3g" % (name, diff, len(ua), maxulp, maxabs))
