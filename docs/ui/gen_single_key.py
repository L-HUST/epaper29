#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Single-key zoom for the 400-point breadboard (docs only)."""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 1400, 900

BG = (246, 246, 242)
BOARD = (252, 252, 250)
HOLE = (55, 55, 55)
INK = (20, 20, 20)
GREY = (115, 115, 115)
RED = (205, 55, 50)
BLUE = (40, 95, 185)
GND = (25, 25, 25)
SIG = (30, 105, 195)


def font(size):
    for p in ("C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/msyh.ttc"):
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()


F_T = font(30)
F_S = font(18)
F_L = font(20)
F_M = font(16)


def text(d, xy, s, f, fill=INK):
    d.text(xy, s, font=f, fill=fill)


def tag(d, xy, s, color=INK, f=F_M):
    x, y = xy
    bb = d.textbbox((0, 0), s, font=f)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.rounded_rectangle([x - 4, y - 2, x + w + 4, y + h + 4], radius=3,
                        fill=(255, 255, 255), outline=color, width=1)
    d.text((x, y), s, font=f, fill=color)


def wire(d, pts, color, width=6):
    d.line(pts, fill=(255, 255, 255), width=width + 6, joint="curve")
    d.line(pts, fill=color, width=width, joint="curve")


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    text(d, (40, 24), "单键放大：以 PREV 为例（每一根线怎么走）", F_T)
    text(d, (40, 64), "开关脚插 e2/f2；线插 e3/f3——同一列、同一个 5 孔组，电气相通。", F_S, GREY)

    d.rounded_rectangle([80, 150, 900, 700], radius=12, fill=BOARD,
                        outline=(205, 205, 200), width=2)

    # rails
    for x, c, lab in ((120, RED, "+"), (160, BLUE, "GND")):
        d.line([(x, 190), (x, 660)], fill=c, width=8)
        text(d, (x - 10, 160), lab, F_S, c)

    # columns
    cols = {"d": 300, "e": 380, "f": 560, "g": 640}
    for name, x in cols.items():
        text(d, (x - 6, 185), name, F_L, GREY)
        for r in range(1, 6):
            y = 230 + (r - 1) * 80
            d.rectangle([x - 7, y - 7, x + 7, y + 7], fill=HOLE)
            text(d, (x + 16, y - 9), "%s%d" % (name, r), F_M, GREY)

    # rows labels
    for r in range(1, 6):
        y = 230 + (r - 1) * 80
        text(d, (220, y - 9), "row %d" % r, F_M, GREY)

    # switch at row 2
    y2 = 230 + 80
    d.rounded_rectangle([280, y2 - 30, 660, y2 + 30], radius=8,
                        fill=(225, 225, 220), outline=(80, 80, 80), width=2)
    for x in cols.values():
        d.ellipse([x - 8, y2 - 8, x + 8, y2 + 8], fill=(200, 200, 200),
                  outline=(60, 60, 60), width=2)
    text(d, (400, y2 - 12), "PREV", F_L)
    tag(d, (300, y2 - 62), "row 2: 开关脚 d2 / e2 / f2 / g2", INK)

    # dotted same-column links
    y3 = 230 + 2 * 80
    for x in (cols["e"], cols["f"]):
        for yy in range(y2 + 10, y3 - 8, 8):
            d.ellipse([x - 2, yy - 2, x + 2, yy + 2], fill=(140, 140, 140))
    tag(d, (320, y2 + 38), "e2 = e3", GREY, F_S)
    tag(d, (580, y2 + 38), "f2 = f3", GREY, F_S)

    # wire holes
    d.ellipse([cols["e"] - 9, y3 - 9, cols["e"] + 9, y3 + 9], outline=GND, width=3)
    d.ellipse([cols["f"] - 9, y3 - 9, cols["f"] + 9, y3 + 9], outline=SIG, width=3)
    tag(d, (330, y3 + 14), "e3", GND)
    tag(d, (580, y3 + 14), "f3", SIG)

    # W1: e3 -> GND rail
    wire(d, [(cols["e"], y3), (160, y3)], GND)
    d.ellipse([154, y3 - 7, 166, y3 + 7], fill=GND)
    tag(d, (180, y3 - 34), "W1 → 左轨 GND", GND)

    # W2: f3 -> S3 GPIO4
    wire(d, [(cols["f"], y3), (980, y3), (980, 300), (1100, 300)], SIG)
    tag(d, (700, y3 - 34), "W2 → GPIO4", SIG)

    # header
    d.rounded_rectangle([1090, 240, 1350, 380], radius=10, fill=(250, 250, 246),
                        outline=(175, 175, 170), width=2)
    text(d, (1110, 252), "ESP32-S3", F_L)
    d.ellipse([1100, 300 - 9, 1118, 300 + 9], fill=SIG, outline=INK)
    text(d, (1130, 288), "GPIO4", F_L, SIG)
    d.ellipse([1100, 350 - 9, 1118, 350 + 9], fill=GND, outline=INK)
    text(d, (1130, 338), "GND", F_L, GND)

    # notes
    notes = [
        "1. 开关 4 个脚都插进 d2/e2/f2/g2；e2/f2 是要用的两个脚。",
        "2. 杜邦线不要和开关脚挤同一个孔：插在下一行 e3/f3。",
        "3. e2 与 e3 在同一列、同一个 5 孔组，内部相通；f2/f3 同理。",
        "4. 同侧两脚本来相通：左侧 e 接 GND，右侧 f 接 GPIO，各一根线即可。",
        "5. 100nF 可选：一脚接 f3 的 GPIO 线，一脚接 GND，靠近 S3 或开关。",
        "6. 红轨 + 不接；只接蓝轨 GND。",
        "7. S3 的 GND 由全图 W10 接到左轨 GND。",
    ]
    y = 715
    for n in notes:
        text(d, (40, y), n, F_M)
        y += 24

    out = os.path.join(HERE, "breadboard_single_key.png")
    img.save(out)
    print("saved", out)


if __name__ == "__main__":
    main()
