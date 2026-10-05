#!/usr/bin/env python3
"""生成分享卡片 site/public/og.png（中文，1200 × 630）与 og-en.png（英文，--lang en）。

不在 Mac 上跑（全局纪律）：用 site/scripts/make-og.sh，它把站点同步到 107、在 venv 里跑本脚本、再把 og.png 拉回。
依赖：numpy、Pillow（带 raqm）、scipy、fontTools + brotli（把站点自带的 woff2 字体解成 TTF）。

画面：纸色底；左边大字「Celadon」「青瓷」与一句「一挂总轨，就好听。」；右边一枚豆青瓷旋钮。
英文版（ADR-024）：「青瓷」换成 Smart mastering plugin（Inter），那句换成首屏英文大标题的两行（Fraunces），旋钮不变。
旋钮照设计规格-官网第 5 节：中心是同釉的浅凹窝加一圈细刻线（没有深色旋钮帽），指针是釉面边缘刻出的短槽，
带两层开片、左上棚灯反光与细高光弧、下沿一圈透光的浅色，投影带釉色。
全部在 2× 画布上逐像素算（高度场 → 法线 → 光照），最后缩到 1× 抗锯齿。开片、云斑用固定种子，每次生成一样。
"""
from __future__ import annotations

import argparse
import math
import os
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFilter, ImageFont, features
from scipy.spatial import cKDTree

SITE = Path(__file__).resolve().parent.parent

# 纸、墨、釉青取自 src/index.css；釉面、开片、投影取自设计规格-官网第 5 节（豆青，从插件界面 2× 图取色）。
PAPER = '#F3F1EA'
INK = '#0D1B21'
INK2 = '#4B5E66'
INK3 = '#586C74'
INK4 = '#6E8189'
LINE = '#D9E2E4'
LINE2 = '#C7D3D6'
C300 = '#67CAD7'
C600 = '#127F90'
C700 = '#0E6473'
C800 = '#0A4A57'
GLAZE_HI = '#F1F5F0'
GLAZE_RAMP = [(0.0, '#D5E2DA'), (0.40, '#B4CBC0'), (0.70, '#96B3A8'), (1.0, '#3D6258')]
RIM_LIGHT = '#E6EEE9'
CRACKLE = '#2F564C'
SHADOW = '#2E4A42'

W, H = 1200, 630
# 旋钮（1× 坐标）：圆心、釉面半径、轨道半径、刻度点半径
KX, KY, KR = 900, 315, 160
TRACK_R, TICK_R = KR + 30, KR + 47
SWEEP0, SWEEP = -135.0, 270.0  # 从 12 点方向顺时针量角，与站点旋钮一致


def rgb(hex_color: str) -> np.ndarray:
    h = hex_color.lstrip('#')
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def smoothstep(e0: float, e1: float, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def ramp(t: np.ndarray, stops) -> np.ndarray:
    """按 (位置, 颜色) 表插值；t 形状 (h, w)，返回 (h, w, 3)。"""
    t = np.clip(t, 0.0, 1.0)
    out = np.zeros(t.shape + (3,))
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        k = np.clip((t - p0) / (p1 - p0), 0.0, 1.0)[..., None]
        seg = (t >= p0) & (t <= p1)
        out[seg] = (rgb(c0) * (1 - k) + rgb(c1) * k)[seg]
    return out


def mix(a: np.ndarray, b, k: np.ndarray) -> np.ndarray:
    return a * (1 - k[..., None]) + np.asarray(b) * k[..., None]


def value_noise(u: np.ndarray, v: np.ndarray, rng: np.random.Generator, octaves=3, freq=2.2) -> np.ndarray:
    """几组随机相位的正弦叠出来的低频起伏，范围约 −1 ～ 1（云斑与釉厚薄用）。"""
    acc = np.zeros_like(u)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        f = freq * (2 ** o)
        for _ in range(3):
            a = rng.uniform(0, math.pi)
            ph = rng.uniform(0, 2 * math.pi)
            acc += amp * np.sin(f * (u * math.cos(a) + v * math.sin(a)) + ph)
            total += amp
        amp *= 0.5
    return acc / total * 1.7


def poisson_disk(rng: np.random.Generator, n: int, rmin: float, reach: float) -> np.ndarray:
    pts: list[tuple[float, float]] = []
    tries = 0
    while len(pts) < n and tries < 40000:
        tries += 1
        p = rng.uniform(-reach, reach, 2)
        if math.hypot(*p) > reach:
            continue
        if all(math.hypot(p[0] - q[0], p[1] - q[1]) >= rmin for q in pts):
            pts.append((float(p[0]), float(p[1])))
    return np.array(pts)


def crackle_lines(u: np.ndarray, v: np.ndarray, R: float, rng: np.random.Generator, n: int, rmin: float, half_px: float, keep: float, lo: float) -> np.ndarray:
    """开片：Voronoi 边界（坐标先轻轻扭一下，线才不死直）。返回 0 ～ 1 的线强度。"""
    seeds = poisson_disk(rng, n, rmin, 1.12)
    ph = rng.uniform(0, 2 * math.pi, 6)
    wu = u + 0.042 * np.sin(3.1 * v + ph[0]) + 0.016 * np.sin(7.3 * v + 5.1 * u + ph[1]) + 0.006 * np.sin(17.0 * v - 11.0 * u + ph[2])
    wv = v + 0.042 * np.sin(2.7 * u + ph[3]) + 0.016 * np.sin(6.1 * u - 4.7 * v + ph[4]) + 0.006 * np.sin(15.0 * u + 13.0 * v + ph[5])
    pts = np.stack([wu.ravel(), wv.ravel()], 1)
    dist, idx = cKDTree(seeds).query(pts, k=2)
    s1, s2 = seeds[idx[:, 0]], seeds[idx[:, 1]]
    gap = np.linalg.norm(s2 - s1, axis=1)
    edge_px = (dist[:, 1] ** 2 - dist[:, 0] ** 2) / (2 * gap) * R
    # 线宽沿途有粗有细
    half = (half_px * (0.8 + 0.3 * np.sin(9.1 * u + ph[0]) * np.sin(7.7 * v + ph[3]))).ravel()
    line = 1 - smoothstep(half * 0.25, half, edge_px)
    # 每条边一个随机强度，一部分边干脆不出现（真开片不是完整的网）
    a, b = np.minimum(idx[:, 0], idx[:, 1]), np.maximum(idx[:, 0], idx[:, 1])
    h = np.sin(a * 12.9898 + b * 78.233) * 43758.5453
    r = h - np.floor(h)
    strength = np.where(r < keep, lo + (1 - lo) * (r / keep), 0.0)
    return (line * strength).reshape(u.shape)


def arc_sdf(x: np.ndarray, y: np.ndarray, cx: float, cy: float, r: float, a0: float, a1: float) -> np.ndarray:
    """到一段圆弧（圆头端点）的距离；角度从 12 点顺时针量，单位度。"""
    dx, dy = x - cx, y - cy
    ang = np.degrees(np.arctan2(dx, -dy))
    ring = np.abs(np.hypot(dx, dy) - r)
    p0 = (cx + r * math.sin(math.radians(a0)), cy - r * math.cos(math.radians(a0)))
    p1 = (cx + r * math.sin(math.radians(a1)), cy - r * math.cos(math.radians(a1)))
    ends = np.minimum(np.hypot(x - p0[0], y - p0[1]), np.hypot(x - p1[0], y - p1[1]))
    return np.where((ang >= a0) & (ang <= a1), ring, ends)


def over(dst: np.ndarray, color, alpha: np.ndarray) -> None:
    """把单色按 alpha 盖到 dst（h, w, 3）上。"""
    dst[:] = dst * (1 - alpha[..., None]) + np.asarray(color) * alpha[..., None]


def render_knob(canvas: np.ndarray, S: int, value: float, seed: int) -> None:
    rng = np.random.default_rng(seed)
    R = KR * S
    cx, cy = KX * S, KY * S

    # —— 轨道、数值弧、刻度点（整块画布上的距离场，自带抗锯齿）——
    m = int(TICK_R * S + 8 * S)
    y0, y1, x0, x1 = int(cy - m), int(cy + m), int(cx - m), int(cx + m)
    ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float64) + 0.5
    region = canvas[y0:y1, x0:x1]
    a_end = SWEEP0 + SWEEP * value
    track = arc_sdf(xs, ys, cx, cy, TRACK_R * S, SWEEP0, SWEEP0 + SWEEP)
    over(region, rgb(LINE), np.clip(1.0 * S - track + 0.5, 0, 1))
    val = arc_sdf(xs, ys, cx, cy, TRACK_R * S, SWEEP0, a_end)
    ang = np.degrees(np.arctan2(xs - cx, -(ys - cy)))
    k = np.clip((ang - SWEEP0) / max(a_end - SWEEP0, 1e-6), 0, 1)
    val_col = ramp(k, [(0.0, C300), (0.55, C600), (1.0, C700)])
    cov = np.clip(1.0 * S - val + 0.5, 0, 1)
    region[:] = region * (1 - cov[..., None]) + val_col * cov[..., None]
    for i in range(11):
        a = math.radians(SWEEP0 + SWEEP * i / 10)
        tx, ty = cx + TICK_R * S * math.sin(a), cy - TICK_R * S * math.cos(a)
        rr = (2.1 if i % 5 == 0 else 1.35) * S
        dot = np.clip(rr - np.hypot(xs - tx, ys - ty) + 0.5, 0, 1)
        over(region, rgb(C600 if i / 10 <= value + 1e-6 else LINE2), dot)

    # —— 投影：带釉色，一层柔的远影 + 一层贴地的近影 ——
    for (ox, oy, blur, alpha) in ((7, 18, 20, 0.26), (2, 6, 6, 0.16)):
        mask = Image.new('L', (canvas.shape[1], canvas.shape[0]), 0)
        ImageDraw.Draw(mask).ellipse([cx - R + ox * S, cy - R + oy * S, cx + R + ox * S, cy + R + oy * S], fill=255)
        mask = mask.filter(ImageFilter.GaussianBlur(blur * S))
        over(canvas, rgb(SHADOW), np.asarray(mask, dtype=np.float64) / 255.0 * alpha)

    # —— 釉面：高度场 → 法线 → 光照 ——
    b = int(R + 3 * S)
    y0, y1, x0, x1 = int(cy - b), int(cy + b), int(cx - b), int(cx + b)
    ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float64) + 0.5
    u, v = (xs - cx) / R, (ys - cy) / R
    d = np.hypot(u, v)
    cover = np.clip(R * (1 - d) + 0.5, 0, 1)

    # 缓的穹面：顶上平缓、边上一圈圆肩（太鼓就像玻璃珠）
    P, A = 3.6, 0.26
    dc = np.minimum(d, 1.0)
    dome = A * R * np.power(np.clip(1 - np.power(dc, P), 0, 1), 1 / P)
    # 中心浅凹窝 + 一圈细刻线
    DR = 0.205
    dimple_m = np.where(d < DR, (1 - (d / DR) ** 2) ** 2, 0.0)
    ring_m = np.clip(1 - np.abs(d - 0.228) / 0.0065, 0, 1)
    # 指针：釉面边缘刻出的短槽（圆头）
    phi = math.radians(SWEEP0 + SWEEP * value)
    dx, dy = math.sin(phi), -math.cos(phi)
    t = np.clip(u * dx + v * dy, 0.70, 0.86)
    gd = np.hypot(u - t * dx, v - t * dy)
    groove_m = np.clip(1 - (gd / 0.017) ** 2, 0, 1)
    height = dome - 0.022 * R * dimple_m - 0.0035 * R * ring_m - 0.008 * R * groove_m
    gy, gx = np.gradient(height)
    n = np.stack([-gx, -gy, np.ones_like(gx)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)

    # 釉的厚薄：边缘与下方积得厚，凹窝、刻线、槽里积釉；再叠一层很淡的云斑
    mottle = value_noise(u, v, rng)
    thick = 0.12 + 0.46 * smoothstep(0.55, 0.99, d) + 0.06 * v + 0.05 * mottle
    thick += 0.24 * dimple_m + 0.30 * ring_m + 0.38 * groove_m
    body = ramp(thick, GLAZE_RAMP)

    # 瓷是半哑光：包裹式漫射（暗面不死黑），明暗反差比玻璃小
    L = np.array([-0.48, -0.62, 0.62])
    L /= np.linalg.norm(L)
    ndl = n @ L
    wrap = 0.7
    diff = np.clip((ndl + wrap) / (1 + wrap), 0, 1)
    col = body * (0.70 + 0.36 * diff)[..., None]

    # 开片：粗一层 + 细一层（细线、半透明，像釉下的冰裂）；避开凹窝、刻线与最外一圈
    fade = smoothstep(0.26, 0.31, d) * (1 - smoothstep(0.95, 0.985, d)) * (1 - groove_m)
    base_alpha = 0.22 + 0.5 * value
    primary = crackle_lines(u, v, R, rng, 30, 0.27, 0.62 * S, 0.93, 0.5)
    secondary = crackle_lines(u, v, R, rng, 130, 0.11, 0.42 * S, 0.7, 0.3)
    over(col, rgb(CRACKLE), np.clip(primary * base_alpha * 0.8 + secondary * base_alpha * 0.38, 0, 1) * fade)

    # 左上棚灯：反射方向落进一块柔边矩形里就亮（柔光，不是玻璃那种硬高光）
    rx, ry = 2 * n[..., 2] * n[..., 0], 2 * n[..., 2] * n[..., 1]
    qx = np.abs(rx + 0.19) - 0.12 + 0.05
    qy = np.abs(ry + 0.24) - 0.075 + 0.05
    sd = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - 0.05
    softbox = 1 - smoothstep(-0.035, 0.035, sd)
    sheen = 1 - smoothstep(-0.03, 0.30, sd)
    # 凹窝里不给高光：窝壁反出一个亮点就成了「眼球里的反光」
    shield = smoothstep(0.21, 0.26, d)
    over(col, rgb(GLAZE_HI), np.clip(0.66 * softbox + 0.12 * sheen, 0, 1) * shield)
    # 细高光弧：左上边缘一线
    ang = np.arctan2(v, u)
    arc_w = np.exp(-(((d - 0.952) / 0.0085) ** 2)) * smoothstep(0.30, 0.0, np.abs(ang + 2.22) - 0.45)
    over(col, rgb(GLAZE_HI), np.clip(0.62 * arc_w, 0, 1))
    # 下沿一圈透光的浅色
    rim = smoothstep(0.93, 0.99, d) * smoothstep(0.05, 0.75, v)
    over(col, rgb(RIM_LIGHT), 0.5 * rim)

    region = canvas[y0:y1, x0:x1]
    region[:] = region * (1 - cover[..., None]) + np.clip(col, 0, 1) * cover[..., None]


def woff2_to_ttf(path: Path, tmp: Path) -> Path:
    out = tmp / (path.stem + '.ttf')
    font = TTFont(str(path))
    font.flavor = None
    font.save(str(out))
    return out


def variable_font(path: Path, size: int, axes: dict[str, float]) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(path), size, layout_engine=ImageFont.Layout.RAQM)
    tags = [a.axisTag for a in TTFont(str(path))['fvar'].axes]
    defaults = {a.axisTag: a.defaultValue for a in TTFont(str(path))['fvar'].axes}
    font.set_variation_by_axes([axes.get(t, defaults[t]) for t in tags])
    return font


def cjk_serif() -> tuple[str, int]:
    env = os.environ.get('OG_CJK_SERIF')
    if env:
        file, _, index = env.partition(':')
        return file, int(index or 0)
    out = subprocess.run(['fc-match', '-f', '%{file}:%{index}', 'Noto Serif CJK SC:style=Regular'], capture_output=True, text=True, check=True).stdout
    file, _, index = out.partition(':')
    if 'CJK' not in file:
        raise SystemExit(f'没找到 Noto Serif CJK SC（fc-match 给的是 {file}）；装 fonts-noto-cjk 或设 OG_CJK_SERIF=路径:序号')
    return file, int(index or 0)


def draw_tracked(draw: ImageDraw.ImageDraw, x: float, y: float, text: str, font, fill: str, tracking: float) -> float:
    """逐字画（Pillow 没有字距参数），返回画完后的 x。"""
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill, anchor='ls')
        x += font.getlength(ch) + tracking
    return x


# 左栏文字的右边界（1×）：旋钮刻度点最左约在 KX − TICK_R ≈ 693，再留出投影
TEXT_RIGHT = 650


def render(out: Path, S: int, value: float, seed: int, lang: str) -> None:
    if not features.check('raqm'):
        raise SystemExit('Pillow 没带 raqm（字距与连字要它）；换官方 wheel')
    canvas = np.empty((H * S, W * S, 3))
    canvas[:] = rgb(PAPER)
    render_knob(canvas, S, value, seed)

    img = Image.fromarray((np.clip(canvas, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')
    draw = ImageDraw.Draw(img)
    nm = SITE / 'node_modules' / '@fontsource-variable'
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        fraunces = woff2_to_ttf(Path(os.environ.get('OG_FRAUNCES', nm / 'fraunces' / 'files' / 'fraunces-latin-full-normal.woff2')), tmp)
        mono = woff2_to_ttf(Path(os.environ.get('OG_MONO', nm / 'jetbrains-mono' / 'files' / 'jetbrains-mono-latin-wght-normal.woff2')), tmp)

        # 与站点 .display 同一组轴：opsz 144、SOFT 30、WONK 1
        title = variable_font(fraunces, 120 * S, {'opsz': 144, 'wght': 430, 'SOFT': 30, 'WONK': 1})
        url = variable_font(mono, 15 * S, {'wght': 420})

        x = 92 * S
        draw.text((x - 4 * S, 256 * S), 'Celadon', font=title, fill=INK, anchor='ls')
        # 一小段釉青细线，接住下面那句话
        draw.rectangle([x, 382 * S, x + 40 * S, 382 * S + max(1, S) - 1], fill=C600)
        if lang == 'zh':
            cjk_file, cjk_index = cjk_serif()
            name_cn = ImageFont.truetype(cjk_file, 50 * S, index=cjk_index, layout_engine=ImageFont.Layout.RAQM)
            slogan = ImageFont.truetype(cjk_file, 36 * S, index=cjk_index, layout_engine=ImageFont.Layout.RAQM)
            draw_tracked(draw, x, 330 * S, '青瓷', name_cn, INK3, 0.22 * 50 * S)
            after = draw_tracked(draw, x, 452 * S, '一挂总轨，', slogan, INK2, 0.04 * 36 * S)
            draw_tracked(draw, after, 452 * S, '就好听。', slogan, C700, 0.04 * 36 * S)
        else:
            # 拉丁字整行画（保留字偶距调整）；与首屏英文大标题同一句（home.json hero）
            inter = woff2_to_ttf(Path(os.environ.get('OG_INTER', nm / 'inter' / 'files' / 'inter-latin-wght-normal.woff2')), tmp)
            kind = variable_font(inter, 28 * S, {'wght': 450})
            slogan = variable_font(fraunces, 38 * S, {'opsz': 48, 'wght': 400, 'SOFT': 30, 'WONK': 0})
            lines = [('One insert on the master bus.', INK2, 432), ('It just sounds right.', C700, 482)]
            widest = max(slogan.getlength(t) for t, _, _ in lines) / S
            if 92 + widest > TEXT_RIGHT:
                raise SystemExit(f'英文那句排到 {92 + widest:.0f} px，压到旋钮（右边界 {TEXT_RIGHT}）')
            draw.text((x, 330 * S), 'Smart mastering plugin', font=kind, fill=INK3, anchor='ls')
            for text, fill, y in lines:
                draw.text((x, y * S), text, font=slogan, fill=fill, anchor='ls')
        draw_tracked(draw, x, 548 * S, 'celadonaudio.com', url, INK4, 0.06 * 15 * S)

    img = img.resize((W, H), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, 'PNG', optimize=True)
    print(f'wrote {out} ({out.stat().st_size / 1024:.0f} KB)')


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--lang', choices=['zh', 'en'], default='zh', help='zh → og.png；en → og-en.png（ADR-024）')
    ap.add_argument('--out', type=Path, default=None, help='默认 public/og.png 或 public/og-en.png')
    ap.add_argument('--scale', type=int, default=2, help='超采样倍数（默认 2）')
    ap.add_argument('--value', type=float, default=0.56, help='旋钮指到的 GLAZE（0 ～ 1），也决定开片深浅')
    ap.add_argument('--seed', type=int, default=11, help='开片与云斑的随机种子')
    args = ap.parse_args()
    out = args.out or SITE / 'public' / ('og.png' if args.lang == 'zh' else 'og-en.png')
    render(out, args.scale, args.value, args.seed, args.lang)


if __name__ == '__main__':
    main()
