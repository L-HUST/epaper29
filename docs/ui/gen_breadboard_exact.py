#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Exact 400-point breadboard wiring for the 4 keys (docs only).

Every wire is numbered and has both endpoints written on the drawing.
No firmware changes.

Output: breadboard_wiring_exact.png
"""

import math
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 2000, 1320

BG = (246, 246, 242)
BOARD = (252, 252, 250)
HOLE = (55, 55, 55)
INK = (20, 20, 20)
GREY = (115, 115, 115)
RED = (205, 55, 50)
BLUE = (40, 95, 185)
GND = (25, 25, 25)
C_PREV = (30, 105, 195)
C_NEXT = (35, 145, 75)
C_OK = (210, 115, 25)
C_PWR = (145, 75, 185)


def font(size):
    for p in ("C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/msyh.ttc"):
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()


F_TITLE = font(30)
F_SUB = font(17)
F_LABEL = font(18)
F_SMALL = font(15)
F_TINY = font(13)
F_WIRE = font(14)
F_LEGEND = font(14)


def text(d, xy, s, f, fill=INK):
    d.text(xy, s, font=f, fill=fill)


def tag(d, xy, s, color=INK, f=F_WIRE):
    x, y = xy
    bb = d.textbbox((0, 0), s, font=f)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.rounded_rectangle([x - 3, y - 2, x + w + 3, y + h + 3], radius=3,
                        fill=(255, 255, 255), outline=color, width=1)
    d.text((x, y), s, font=f, fill=color)


def wire(d, pts, color, width=5):
    # white halo makes the wire readable over holes and other lines
    d.line(pts, fill=(255, 255, 255), width=width + 5, joint="curve")
    d.line(pts, fill=color, width=width, joint="curve")


def hop(d, x, y, color, width=5):
    d.ellipse([x - 13, y - 13, x + 13, y + 13], fill=BOARD)
    d.arc([x - 13, y - 13, x + 13, y + 13], 180, 360, fill=color, width=width)


def arrow(d, x1, y1, x2, y2, color):
    d.line([(x1, y1), (x2, y2)], fill=color, width=5)
    ang = math.atan2(y2 - y1, x2 - x1)
    for a in (ang + 2.6, ang - 2.6):
        d.line([(x2, y2), (x2 + 14 * math.cos(a), y2 + 14 * math.sin(a))],
               fill=color, width=5)


def main():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    text(d, (40, 22), "400 孔面包板 · 4 键逐根接线图（每根线都有编号）", F_TITLE)
    text(d, (40, 62), "对应你的照片：左半 a–e、右半 f–j，开关跨中间缝；只用蓝色 GND 轨，红色 + 不接。", F_SUB, GREY)

    # board
    bx0, by0, bx1, by1 = 60, 150, 1180, 1010
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=14, fill=BOARD,
                        outline=(205, 205, 200), width=2)

    # rails
    ry0, ry1 = 190, 990
    for x, c, lab in ((100, RED, "+"), (140, BLUE, "-"),
                      (820, RED, "+"), (860, BLUE, "-")):
        d.line([(x, ry0), (x, ry1)], fill=c, width=8)
        text(d, (x - 7, ry0 - 30), lab, F_LABEL, c)

    # columns
    xs_left = [210, 260, 310, 360, 410]
    xs_right = [560, 610, 660, 710, 760]
    for i, x in enumerate(xs_left):
        text(d, (x - 5, 176), "abcde"[i], F_LABEL, GREY)
    for i, x in enumerate(xs_right):
        text(d, (x - 5, 176), "fghij"[i], F_LABEL, GREY)

    # holes + row numbers
    for r in range(1, 21):
        y = 210 + (r - 1) * 40
        text(d, (170, y - 8), str(r), F_TINY, GREY)
        text(d, (790, y - 8), str(r), F_TINY, GREY)
        for x in xs_left + xs_right:
            d.rectangle([x - 5, y - 5, x + 5, y + 5], fill=HOLE)

    # center channel
    d.rectangle([440, 186, 535, 990], fill=(238, 238, 234))
    text(d, (440, 988), "中间缝：开关跨这里", F_SMALL, GREY)

    # switches at rows 2, 7, 12, 17 (each in its own 5-hole group)
    keys = [
        (2, "PREV", C_PREV),
        (7, "NEXT", C_NEXT),
        (12, "OK", C_OK),
        (17, "POWER", C_PWR),
    ]
    for row, name, _ in keys:
        y = 210 + (row - 1) * 40
        d.rounded_rectangle([350, y - 27, 630, y + 27], radius=8,
                            fill=(225, 225, 220), outline=(80, 80, 80), width=2)
        for px in (360, 410, 560, 610):
            d.ellipse([px - 7, y - 7, px + 7, y + 7], fill=(200, 200, 200),
                      outline=(60, 60, 60), width=2)
        tag(d, (165, y - 12), "%s (row %d)" % (name, row), INK, F_SMALL)

    # wires: key GND (e3/e8/e13/e18 -> left blue rail)
    gnd_pts = [(3, "W1"), (8, "W3"), (13, "W5"), (18, "W7")]
    for row, wid in gnd_pts:
        y = 210 + (row - 1) * 40
        wire(d, [(410, y), (140, y)], GND)
        d.ellipse([134, y - 6, 146, y + 6], fill=GND)
        tag(d, (250, y - 26), wid, GND)

    # wires: key signal (f3/f8/f13/f18 -> S3 header)
    sig_pts = [
        (3, "W2", "GPIO4", C_PREV, 1200, 270),
        (8, "W4", "GPIO5", C_NEXT, 1215, 370),
        (13, "W6", "GPIO6", C_OK, 1230, 470),
        (18, "W8", "GPIO15", C_PWR, 1245, 570),
    ]
    for row, wid, gpio, color, lane, hy in sig_pts:
        y = 210 + (row - 1) * 40
        wire(d, [(560, y), (lane, y), (lane, hy), (1300, hy)], color)
        for rx in (820, 860):
            hop(d, rx, y, color)
        d.ellipse([554, y - 6, 566, y + 6], fill=color)
        tag(d, (690, y - 26), wid, color)
        tag(d, (1250, hy - 30), wid, color)

    # optional GND rail bridge (only if the left rail is split in two)
    y14 = 210 + 13 * 40
    y16 = 210 + 15 * 40
    wire(d, [(140, y14), (120, y14), (120, y16), (140, y16)], GND, width=3)
    tag(d, (45, y14 - 10), "W9", GND)

    # S3 GND: left rail row 20 -> header GND
    y20 = 210 + 19 * 40
    yg = 670
    wire(d, [(140, y20), (155, y20), (155, 1032), (1260, 1032), (1260, yg), (1300, yg)], GND)
    tag(d, (160, 1000), "W10", GND)

    # header
    hx0, hy0, hx1, hy1 = 1300, 210, 1720, 760
    d.rounded_rectangle([hx0, hy0, hx1, hy1], radius=12, fill=(250, 250, 246),
                        outline=(175, 175, 170), width=2)
    text(d, (1320, 220), "到 ESP32-S3（母转公杜邦线）", F_LABEL)

    pins = [
        ("GPIO4", C_PREV, 270),
        ("GPIO5", C_NEXT, 370),
        ("GPIO6", C_OK, 470),
        ("GPIO15", C_PWR, 570),
        ("GND", GND, 670),
    ]
    for name, color, y in pins:
        d.ellipse([1310, y - 9, 1328, y + 9], fill=color, outline=INK)
        text(d, (1340, y - 13), name, F_LABEL, color)
        # wire number at header end
        wid = {"GPIO4": "W2", "GPIO5": "W4", "GPIO6": "W6",
               "GPIO15": "W8", "GND": "W10"}[name]
        tag(d, (1250, y - 30), wid, color)

    # wire legend
    d.rounded_rectangle([60, 1045, 1240, 1305], radius=10, fill=(255, 255, 255),
                        outline=(205, 205, 200), width=2)
    text(d, (80, 1053), "逐根线清单（左端 → 右端）", F_LABEL)
    legend_left = [
        ("W1", "e3 → 左轨GND 第3行", "PREV GND"),
        ("W2", "f3 → GPIO4", "PREV 信号"),
        ("W3", "e8 → 左轨GND 第8行", "NEXT GND"),
        ("W4", "f8 → GPIO5", "NEXT 信号"),
        ("W5", "e13 → 左轨GND 第13行", "OK GND"),
    ]
    legend_right = [
        ("W6", "f13 → GPIO6", "OK 信号"),
        ("W7", "e18 → 左轨GND 第18行", "POWER GND"),
        ("W8", "f18 → GPIO15", "POWER 信号"),
        ("W9", "左轨第14行 → 第16行", "建议直接接"),
        ("W10", "左轨第20行 → S3 GND", "公共地"),
    ]
    y = 1082
    for wid, path, note in legend_left:
        text(d, (80, y), wid, F_LEGEND, GND)
        text(d, (130, y), path, F_LEGEND)
        text(d, (400, y), note, F_LEGEND, GREY)
        y += 24
    y = 1082
    for wid, path, note in legend_right:
        text(d, (640, y), wid, F_LEGEND, GND)
        text(d, (690, y), path, F_LEGEND)
        text(d, (960, y), note, F_LEGEND, GREY)
        y += 24

    # notes
    notes = [
        "关键：开关脚在 d2/e2/f2/g2，线不要硬塞同一个孔；",
        "线插在下一行的 e3/f3（同一列、同一个 5 孔组，电气相通）。",
        "开关同侧两脚内部相通：左侧接 GND，右侧接 GPIO，各接一个脚即可。",
        "红轨 + 全程不接；只接蓝轨GND。",
        "中间缝太宽跨不过：把针稍微掰到隔 2 孔，或焊 4 根短线引出。",
        "W9 建议直接接：桥接左轨上下两段，保证都通。",
        "100nF 可选：一脚接 GPIO 线、一脚接 GND，靠近 S3 端或开关端。",
        "现在固件不读按键，接好后没反应是正常的。",
    ]
    y = 820
    for n in notes:
        text(d, (1300, y), n, F_SMALL, INK if not n.startswith("现在") else RED)
        y += 26

    out = os.path.join(HERE, "breadboard_wiring_exact.png")
    img.save(out)
    print("saved", out)


if __name__ == "__main__":
    main()
