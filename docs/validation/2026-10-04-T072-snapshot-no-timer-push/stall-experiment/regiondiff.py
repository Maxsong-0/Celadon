"""逐张比两个目录里的 PNG，只数频谱仪面板（编辑器逻辑坐标 x 84..1116、y 80..300，按图宽换算缩放）里不同的像素。
裁切图（图宽不是 1200 / 840 / 1800）不含频谱仪，跳过。用法：regiondiff.py <目录 A> <目录 B>"""
import sys, pathlib
import numpy as np
from PIL import Image
a_dir, b_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
SCALE_BY_WIDTH = {1200: 1.0, 840: 0.7, 1800: 1.5}
total_img = diff_img = skipped = 0
worst = 0
for f in sorted(a_dir.glob("*.png")):
    A = np.asarray(Image.open(f).convert("RGB")).astype(int)
    B = np.asarray(Image.open(b_dir / f.name).convert("RGB")).astype(int)
    scale = SCALE_BY_WIDTH.get(A.shape[1])
    if scale is None or A.shape != B.shape:
        skipped += 1
        continue
    x0, x1, y0, y1 = int(84 * scale), int(1116 * scale), int(80 * scale), int(300 * scale)
    d = np.abs(A[y0:y1, x0:x1] - B[y0:y1, x0:x1]).max(axis=2)
    n = int((d > 0).sum())
    total_img += 1
    if n:
        diff_img += 1
        worst = max(worst, int(d.max()))
        print("%-24s %6d px differ in the spectrum panel (max channel diff %d)" % (f.name, n, int(d.max())))
print("%d full-size images compared (%d crops skipped): %d differ in the spectrum panel" % (total_img, skipped, diff_img))
