#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
genfont.py —— 生成固件用的 5x7 ASCII 点阵字库头文件

输出：firmware/include/font_ascii.h
（字库和屏幕尺寸无关，2.9 / 4.2 / 4.26 寸共用这一份）

为什么要这么绕：直接把 TrueType 渲染到 5x7 网格，字形会被裁掉；
按整块画布降采样又会糊成色块。正确做法是先把字符渲染出来、
取出真实外框，再按比例缩放到 5x7 内并居中，最后二值化。

用法：
  python tools/genfont.py                    # 默认 Consolas size=9
  python tools/genfont.py --size 10 --check  # 换字号并输出对照图
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow:  python -m pip install pillow")

FIRST, LAST = 0x20, 0x7E
W, H = 5, 7
THRESH = 110


def glyph(ch, font, oy=0, ox=0):
    """返回 5 个字节：每字节一列，bit0=顶行(y=0) .. bit6=底行(y=6)。"""
    tmp = Image.new("L", (48, 48), 0)
    ImageDraw.Draw(tmp).text((2, 2), ch, fill=255, font=font)
    bb = tmp.getbbox()
    if bb is None:
        return [0] * W
    g = tmp.crop(bb)
    gw, gh = g.size
    s = min(W / float(gw), H / float(gh))
    nw = max(1, int(round(gw * s)))
    nh = max(1, int(round(gh * s)))
    g = g.resize((nw, nh), Image.LANCZOS)
    src = g.load()
    px = [[0] * H for _ in range(W)]
    bx = max(0, (W - nw) // 2) + ox
    by = (H - nh) + oy
    for x in range(nw):
        for y in range(nh):
            X, Y = bx + x, by + y
            if 0 <= X < W and 0 <= Y < H and src[x, y] >= THRESH:
                px[X][Y] = 1
    return [sum(px[x][y] << y for y in range(H)) for x in range(W)]


def pick_font(size, path=None):
    cands = ([path] if path else []) + [
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    ]
    for p in cands:
        if p and os.path.exists(p):
            return ImageFont.truetype(p, size), p
    return ImageFont.load_default(), "<Pillow default>"


def emit_header(table, path):
    L = ["// 本文件由 tools/genfont.py 自动生成，请勿手改。",
         "// 5x7 ASCII 点阵字库；和屏幕型号无关，所有屏型共用。",
         "// 0x%02X..0x%02X 共 %d 个字符，每字符 5 列，" % (FIRST, LAST, LAST - FIRST + 1),
         "// 列内 bit0 = 顶行(y=0)，bit6 = 底行(y=6)，bit7 恒 0。",
         "#pragma once",
         "#include <stdint.h>",
         "",
         "#define FONT_ASCII_W 5",
         "#define FONT_ASCII_H 7",
         "#define FONT_ASCII_FIRST 0x%02Xu" % FIRST,
         "",
         "static const uint8_t FONT_ASCII[] = {"]
    for i, g in enumerate(table):
        c = FIRST + i
        show = chr(c) if chr(c) not in ('\\', '"') else '?'
        L.append("    %s// 0x%02X '%s'" % ("".join("0x%02X, " % b for b in g), c, show))
    L.append("};")
    L.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))
    return path


def emit_check(table, path, scale=6, per_row=16):
    rows = (len(table) + per_row - 1) // per_row
    cw, chh = (W + 1) * scale, (H + 2) * scale
    img = Image.new("RGB", (per_row * cw, rows * chh), "white")
    d = ImageDraw.Draw(img)
    for i, g in enumerate(table):
        cx, cy = (i % per_row) * cw, (i // per_row) * chh
        for x in range(W):
            for y in range(H):
                if g[x] >> y & 1:
                    d.rectangle([cx + x * scale, cy + (y + 1) * scale,
                                 cx + x * scale + scale - 1, cy + (y + 2) * scale - 1],
                                fill="black")
    img.save(path)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=None)
    ap.add_argument("--size", type=int, default=9)
    ap.add_argument("--oy", type=int, default=0)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    font, fpath = pick_font(a.size, a.font)
    table = [glyph(chr(c), font, a.oy) for c in range(FIRST, LAST + 1)]
    print("font  :", fpath, "size=%d oy=%d" % (a.size, a.oy))
    print("header:", emit_header(table, os.path.join(root, "firmware", "include", "font_ascii.h")))
    if a.check:
        print("check :", emit_check(table, os.path.join(root, "tools", "font_check.png")))


if __name__ == "__main__":
    main()
