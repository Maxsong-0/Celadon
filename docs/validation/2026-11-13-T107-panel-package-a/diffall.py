#!/usr/bin/env python3
"""逐张比两组截图：同名文件逐像素比，打印「相同 / 不同（差异外接框、像素数）/ 只有一边有」。

用法：diffall.py <改前目录> <改后目录>
只读文件，不写任何东西。
"""
import pathlib
import sys

from PIL import Image, ImageChops


def compare(before, after):
    a = Image.open(before).convert("RGB")
    b = Image.open(after).convert("RGB")
    if a.size != b.size:
        return f"尺寸不同 {a.size} → {b.size}"
    diff = ImageChops.difference(a, b)
    box = diff.getbbox()
    if box is None:
        return "相同"
    changed = sum(1 for pixel in diff.get_flattened_data() if pixel != (0, 0, 0)) if hasattr(diff, "get_flattened_data") \
        else sum(1 for pixel in diff.getdata() if pixel != (0, 0, 0))
    return f"不同 框 {box} 像素 {changed}"


def main(before_dir, after_dir):
    before = {p.name: p for p in pathlib.Path(before_dir).glob("*.png")}
    after = {p.name: p for p in pathlib.Path(after_dir).glob("*.png")}
    for name in sorted(before.keys() | after.keys()):
        if name not in after:
            print(f"{name}\t只有改前")
        elif name not in before:
            print(f"{name}\t只有改后（新状态）")
        else:
            print(f"{name}\t{compare(before[name], after[name])}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
