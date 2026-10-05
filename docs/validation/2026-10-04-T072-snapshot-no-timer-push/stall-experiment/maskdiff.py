"""逐张比两个目录里的 PNG：整张逐像素比，但全尺寸图把两条电平表的列（逻辑坐标 x < 62 与 x >= 1122，按图宽换算缩放）遮掉——
电平表按墙钟下落，是 --deterministic 文档里写明的例外。裁切图（图宽不是 1200 / 840 / 1800）整张比。用法：maskdiff.py <目录 A> <目录 B>"""
import sys, pathlib
import numpy as np
from PIL import Image
a_dir, b_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
SCALE_BY_WIDTH = {1200: 1.0, 840: 0.7, 1800: 1.5}
same = differ = 0
for f in sorted(a_dir.glob("*.png")):
    A = np.asarray(Image.open(f).convert("RGB")).astype(int)
    B = np.asarray(Image.open(b_dir / f.name).convert("RGB")).astype(int)
    if A.shape != B.shape:
        print("%-24s shape differs" % f.name); differ += 1; continue
    d = np.abs(A - B).max(axis=2)
    scale = SCALE_BY_WIDTH.get(A.shape[1])
    if scale is not None:
        d[:, : int(62 * scale)] = 0
        d[:, int(1122 * scale):] = 0
    n = int((d > 0).sum())
    if n:
        differ += 1
        ys, xs = np.nonzero(d)
        print("%-24s %6d px differ outside the meter columns, bbox x %d..%d y %d..%d" % (f.name, n, xs.min(), xs.max(), ys.min(), ys.max()))
    else:
        same += 1
print("%d images identical (meter columns masked on full-size ones), %d differ" % (same, differ))
