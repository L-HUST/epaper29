#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
gen_mockups.py -- 2.9 inch 296x128 UI mockups (docs only)

This script does NOT touch firmware. It renders a first-pass UI proposal
so the layout can be reviewed before implementation.

Run:
    ..\..\.venv\Scripts\python.exe gen_mockups.py

Outputs (in this folder):
    01-home.png / -3x.png
    02-reader.png / -3x.png
    03-menu.png / -3x.png
    04-settings.png / -3x.png
    05-transfer.png / -3x.png
    06-sleep.png / -3x.png
    00-contact-sheet.png
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, TOOLS)

from push import MODELS, paginate, render_page  # noqa: E402

W, H = 296, 128
SCALE = 3

CN_FONT = "C:/Windows/Fonts/simhei.ttf"
UI_FONT = "C:/Windows/Fonts/consola.ttf"
UI_BOLD = "C:/Windows/Fonts/consolab.ttf"
INK = 0
PAPER = 255


def _font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()


def cn(size):
    return _font(CN_FONT, size)


def ui(size, bold=False):
    return _font(UI_BOLD if bold and os.path.exists(UI_BOLD) else UI_FONT, size)


def pix():
    """Pillow's built-in bitmap font: closest to the firmware 5x7 glyphs."""
    return ImageFont.load_default()


def text_mask(s, font):
    """Return an L mask: 255 where ink, 0 elsewhere.

    mode "L" + threshold keeps small UI text readable; mode "1" on some
    Windows fonts drops parts of the glyph at 8-13 px.
    """
    m = font.getmask(s, mode="L")
    w, h = m.size
    data = bytes(m)[:w * h]
    ink = Image.frombytes("L", (w, h), data)
    return ink.point(lambda v: 255 if v >= 80 else 0)


def draw_text(canvas, xy, s, font, fg=INK):
    """Draw hinted 1-bit text; fg=255 for white text on a black bar."""
    ink = text_mask(s, font)
    bb = font.getbbox(s)
    canvas.paste(fg, (int(xy[0]) + bb[0], int(xy[1]) + bb[1]), ink)


def canvas_new():
    img = Image.new("L", (W, H), PAPER)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W - 1, H - 1], outline=INK)
    return img, d


def header(img, d, left, right=None, right_font=None):
    draw_text(img, (6, 3), left, ui(13, bold=True))
    if right:
        draw_text(img, (W - 8 - int(right_font.getlength(right)) if right_font else 220, 5),
                  right, right_font or ui(9))
    d.line([4, 18, W - 5, 18], fill=INK)


def footer(img, d, s):
    d.line([4, 112, W - 5, 112], fill=INK)
    draw_text(img, (6, 113), s, pix())


def battery(img, d, pct=82, x=246, y=4):
    draw_text(img, (x, y), "%d%%" % pct, pix())
    bx = x + 24
    d.rectangle([bx, y, bx + 22, y + 11], outline=INK)
    d.rectangle([bx + 23, y + 3, bx + 24, y + 8], fill=INK)
    bars = int(round(pct / 25.0))
    for i in range(bars):
        d.rectangle([bx + 2 + i * 5, y + 2, bx + 4 + i * 5, y + 9], fill=INK)


def row(img, d, y, title, meta, selected=False, title_font=None, meta_font=None):
    title_font = title_font or cn(15)
    meta_font = meta_font or pix()
    if selected:
        d.rectangle([3, y, W - 4, y + 21], fill=INK)
        draw_text(img, (9, y + 3), title, title_font, fg=PAPER)
        draw_text(img, (W - 8 - int(meta_font.getlength(meta)), y + 5),
                  meta, meta_font, fg=PAPER)
    else:
        d.line([8, y + 20, W - 9, y + 20], fill=INK)
        draw_text(img, (9, y + 3), title, title_font)
        draw_text(img, (W - 8 - int(meta_font.getlength(meta)), y + 5),
                  meta, meta_font)


# ---------------------------------------------------------------- screens
def screen_home():
    img, d = canvas_new()
    header(img, d, "INK READER")
    battery(img, d, 82, x=246, y=5)
    items = [
        ("夜航船", "p3/128", True),
        ("三体", "p1/512", False),
        ("银河系漫游指南", "p42/260", False),
        ("浮生六记", "p8/96", False),
    ]
    y = 22
    for title, meta, sel in items:
        row(img, d, y, title, meta, sel)
        y += 22
    footer(img, d, "OK=OPEN   NEXT=DOWN   POWER=SLEEP")
    return img


def screen_reader():
    text = """第一章 夜航
江面上没有月光，只有船尾一条白线
他把大衣裹紧，听见远处汽笛长鸣
像有人在雾里咳嗽
他想起很多年前，也是这样的夜
船靠岸时天边已经泛白"""
    pages, L = paginate(MODELS["29"], text, None, 1.35, 4, header="H")
    return render_page(MODELS["29"], pages[0][1], L, header="夜航  p3/128").convert("L")


def screen_menu():
    img, d = canvas_new()
    header(img, d, "MENU", "夜航", cn(11))
    items = [
        ("Contents", "4 chapters", True),
        ("Bookmarks", "2 saved", False),
        ("Reading settings", "20 pages", False),
        ("Back to library", "", False),
    ]
    y = 22
    for title, meta, sel in items:
        row(img, d, y, title, meta, sel, title_font=ui(13), meta_font=pix())
        y += 22
    footer(img, d, "OK=SELECT   POWER=BACK")
    return img


def screen_settings():
    img, d = canvas_new()
    header(img, d, "SETTINGS", "v1.0", ui(8))
    items = [
        ("Refresh every", "20 pages", True),
        ("Sleep after", "5 min", False),
        ("Battery", "82%", False),
        ("About", "2.9 inch", False),
    ]
    y = 22
    for title, meta, sel in items:
        row(img, d, y, title, meta, sel, title_font=ui(13), meta_font=pix())
        y += 22
    footer(img, d, "OK=CHANGE   POWER=BACK")
    return img


def screen_transfer():
    img, d = canvas_new()
    header(img, d, "TRANSFER MODE", "WiFi", ui(8))
    lines = [
        ("WiFi", "InkReader-A1B2"),
        ("Pass", "12345678"),
        ("Open", "http://192.168.4.1"),
        ("Upload", ".epg via browser"),
    ]
    y = 24
    for k, v in lines:
        draw_text(img, (8, y), k, ui(10, bold=True))
        draw_text(img, (58, y), v, ui(10))
        y += 20
    footer(img, d, "POWER=EXIT   5 min auto-off")
    return img


def screen_sleep():
    img, d = canvas_new()
    d.rectangle([4, 4, W - 5, H - 5], outline=INK)
    draw_text(img, (10, 8), "SLEEP", ui(12, bold=True))
    title = "夜航"
    draw_text(img, ((W - int(cn(24).getlength(title))) // 2, 38), title, cn(24))
    sub = "p3 / 128"
    draw_text(img, ((W - int(ui(11).getlength(sub))) // 2, 76), sub, ui(11))
    hint = "Press any key to wake"
    draw_text(img, ((W - int(ui(9).getlength(hint))) // 2, 104), hint, ui(9))
    return img


# ---------------------------------------------------------------- output
SCREENS = [
    ("01-home", screen_home),
    ("02-reader", screen_reader),
    ("03-menu", screen_menu),
    ("04-settings", screen_settings),
    ("05-transfer", screen_transfer),
    ("06-sleep", screen_sleep),
]


def save_one(name, img):
    one = img.convert("1")
    one.save(os.path.join(HERE, name + ".png"))
    big = one.convert("L").resize((W * SCALE, H * SCALE), Image.NEAREST)
    big.save(os.path.join(HERE, name + "-3x.png"))


def contact_sheet():
    cell_w, cell_h = W * 2, H * 2
    gap, label_h = 18, 24
    cols, rows = 2, 3
    sheet = Image.new("L", (cols * cell_w + (cols + 1) * gap,
                            rows * (cell_h + label_h) + gap), PAPER)
    d = ImageDraw.Draw(sheet)
    for i, (name, _) in enumerate(SCREENS):
        img = Image.open(os.path.join(HERE, name + ".png")).convert("L")
        img = img.resize((cell_w, cell_h), Image.NEAREST)
        c, r = i % cols, i // cols
        x = gap + c * (cell_w + gap)
        y = gap + r * (cell_h + label_h + gap)
        sheet.paste(img, (x, y))
        d.rectangle([x, y, x + cell_w - 1, y + cell_h - 1], outline=INK)
        label = name.split("-", 1)[1].upper()
        d.text((x + 2, y + cell_h + 4), label, font=ui(12, bold=True), fill=INK)
    sheet.save(os.path.join(HERE, "00-contact-sheet.png"))


def main():
    for name, fn in SCREENS:
        img = fn()
        save_one(name, img)
        print("saved", name)
    contact_sheet()
    print("saved 00-contact-sheet")


if __name__ == "__main__":
    main()
