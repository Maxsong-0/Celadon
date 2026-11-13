#!/usr/bin/env python3
"""把改前 / 改后两张截图上下拼成一张对照图，每张上面一条标签。

用法：compose.py <改前.png> <改后.png> <输出.png> <改前标签> <改后标签>
只读两张输入图、写一张输出图；不改别的文件。
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
LABEL_HEIGHT = 44
GAP = 16
BACKGROUND = (232, 237, 239)
INK = (13, 27, 33)


def labelled(image, text, font):
    canvas = Image.new("RGB", (image.width, image.height + LABEL_HEIGHT), BACKGROUND)
    canvas.paste(image.convert("RGB"), (0, LABEL_HEIGHT))
    ImageDraw.Draw(canvas).text((16, 8), text, fill=INK, font=font)
    return canvas


def main(before_path, after_path, out_path, before_label, after_label):
    before = Image.open(before_path)
    after = Image.open(after_path)
    font = ImageFont.truetype(FONT_PATH, 22, index=2)  # 合集里第 3 个是简体中文
    top = labelled(before, before_label, font)
    bottom = labelled(after, after_label, font)
    width = max(top.width, bottom.width)
    out = Image.new("RGB", (width, top.height + GAP + bottom.height), BACKGROUND)
    out.paste(top, (0, 0))
    out.paste(bottom, (0, top.height + GAP))
    out.save(out_path, optimize=True)


if __name__ == "__main__":
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    main(*sys.argv[1:])
