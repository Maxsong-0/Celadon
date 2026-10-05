import sys, pathlib
import numpy as np
from PIL import Image
a_dir, b_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
rows = []
for f in sorted(a_dir.glob("*.png")):
    g = b_dir / f.name
    A = np.asarray(Image.open(f).convert("RGB")).astype(int)
    B = np.asarray(Image.open(g).convert("RGB")).astype(int)
    d = np.abs(A - B).max(axis=2)
    ys, xs = np.nonzero(d)
    if len(xs) == 0:
        continue
    rows.append((f.name, len(xs), int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()), int(d.max())))
for r in rows:
    print("%-26s %7d px  bbox x %4d..%4d  y %4d..%4d  max %3d" % (r[0], r[1], r[2], r[4], r[3], r[5], r[6]))
print(len(rows), "differing images")
