"""按 JUCE 9 的 portable 度量估一行字的宽度（不含字距调整，是上限）。

JUCE：em 像素 = 字体高度 / (ascent + descent)，ascent / descent 取 HarfBuzz 的 h_extents
（OS/2 的 USE_TYPO_METRICS 位开着用 typo，否则用 hhea）。宽度 = Σ hmtx 前进宽 / upem × em 像素。
用法：python3 -I measure_font.py <字体文件> <家族名> <样式名> <JUCE 高度> <文本>...
"""
import sys
from fontTools.ttLib import TTCollection, TTFont


def open_face(path, family, style):
    fonts = TTCollection(path).fonts if path.lower().endswith(".ttc") else [TTFont(path)]
    for font in fonts:
        name = font["name"]
        fam = name.getDebugName(16) or name.getDebugName(1)
        sub = name.getDebugName(17) or name.getDebugName(2)
        if fam == family and sub == style:
            return font
    raise SystemExit(f"找不到 {family} {style}：" + ", ".join(
        f"{f['name'].getDebugName(1)}/{f['name'].getDebugName(2)}" for f in fonts))


def em_pixels(font, height):
    upem = font["head"].unitsPerEm
    os2 = font["OS/2"]
    if os2.fsSelection & (1 << 7):
        ascent, descent = os2.sTypoAscender, os2.sTypoDescender
    else:
        ascent, descent = font["hhea"].ascent, font["hhea"].descent
    return height / ((abs(ascent) + abs(descent)) / upem), upem


def width(font, text, em):
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    upem = font["head"].unitsPerEm
    missing = [ch for ch in text if ord(ch) not in cmap]
    total = sum(hmtx[cmap[ord(ch)]][0] for ch in text if ord(ch) in cmap)
    return total / upem * em, missing


def main():
    path, family, style, height = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    font = open_face(path, family, style)
    em, _ = em_pixels(font, height)
    print(f"{family} {style}: JUCE 高度 {height} → em {em:.2f} px")
    for text in sys.argv[5:]:
        w, missing = width(font, text, em)
        print(f"{w:7.1f} px  {len(text):3d} 字  {text}" + (f"  缺字 {missing}" if missing else ""))


if __name__ == "__main__":
    main()
