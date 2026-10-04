#!/usr/bin/env python3
# 比两组截图：整张是否逐字节相同、频谱仪那一块（编辑器坐标 84, 60, 1032 × 238，按 --scale 放大）像素是否完全相同。
# 用法：compare_crops.py <目录 A> <目录 B> <倍数>
import pathlib
import sys

from PIL import Image, ImageChops

a_dir, b_dir, scale = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), float(sys.argv[3])
box = tuple(int(round(v * scale)) for v in (84, 60, 84 + 1032, 60 + 238))
same_file = same_crop = total = 0
for a in sorted(a_dir.glob('*.png')):
    b = b_dir / a.name
    total += 1
    file_equal = a.read_bytes() == b.read_bytes()
    ia, ib = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')   # 不能用 RGBA：getbbox 默认只看透明度
    crop_equal = False
    note = ''
    if ia.size == ib.size and ia.size[0] >= box[2] and ia.size[1] >= box[3]:
        diff = ImageChops.difference(ia.crop(box), ib.crop(box))
        crop_equal = diff.getbbox() is None
        if not crop_equal:
            note = f' 频谱仪区域不同，差异包围盒 {diff.getbbox()}'
    else:
        note = f' （尺寸 {ia.size}，不含完整频谱仪区域，只比整张）'
        crop_equal = file_equal
    same_file += file_equal
    same_crop += crop_equal
    print(f"{a.name:32} 整张{'相同' if file_equal else '不同'}  频谱仪{'相同' if crop_equal else '不同'}{note}")
print(f'共 {total} 张：整张逐字节相同 {same_file}，频谱仪那一块相同 {same_crop}')
