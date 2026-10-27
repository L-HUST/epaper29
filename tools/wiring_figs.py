# -*- coding: utf-8 -*-
"""生成三张接线图：面包板原理 / 电源接线 / S3 引脚位置。"""
import os
from PIL import Image, ImageDraw, ImageFont

SS = 2

C_BG = (255, 255, 255)
C_INK = (30, 34, 40)
C_SUB = (110, 120, 132)
C_HOLE = (203, 209, 216)
C_RED = (198, 58, 52)
C_BLU = (42, 100, 175)
C_GRN = (32, 135, 85)
C_ORG = (222, 133, 35)
C_PUR = (126, 76, 166)
C_BLACK = (55, 55, 60)
C_BOARD = (38, 50, 60)
C_PANEL = (246, 248, 251)
C_EDGE = (172, 182, 194)


def font(size, bold=False):
    cands = ["C:/Windows/Fonts/msyhbd.ttc"] if bold else []
    cands += ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf"]
    for p in cands:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, int(size * SS))
            except Exception:
                pass
    return ImageFont.load_default()


class Cv:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("RGB", (w * SS, h * SS), C_BG)
        self.d = ImageDraw.Draw(self.img)

    def x(self, v):
        return v * SS

    def rect(self, x1, y1, x2, y2, **kw):
        self.d.rectangle([self.x(x1), self.x(y1), self.x(x2), self.x(y2)], **kw)

    def rrect(self, x1, y1, x2, y2, r, **kw):
        self.d.rounded_rectangle([self.x(x1), self.x(y1), self.x(x2), self.x(y2)],
                                 radius=self.x(r), **kw)

    def line(self, pts, **kw):
        self.d.line([(self.x(a), self.x(b)) for a, b in pts], **kw)

    def circle(self, cx, cy, r, **kw):
        self.d.ellipse([self.x(cx - r), self.x(cy - r),
                        self.x(cx + r), self.x(cy + r)], **kw)

    def text(self, x, y, s, size=16, col=C_INK, anchor="la", bold=False):
        self.d.text((self.x(x), self.x(y)), s, font=font(size, bold),
                    fill=col, anchor=anchor)

    def save(self, path):
        self.img.resize((self.w, self.h), Image.LANCZOS).save(path)
        print("saved", path)


# ==================================================== 图 1：面包板原理
def fig_breadboard():
    c = Cv(1150, 760)
    c.text(40, 28, "面包板内部是怎么连的", 30, C_INK, bold=True)
    c.text(40, 72, "看懂了这张，就知道为什么「插在同一组的 5 个孔里」就等于接在一起",
           15, C_SUB)

    bx1, by1, bx2, by2 = 60, 150, 810, 640
    c.rrect(bx1, by1, bx2, by2, 12, fill=C_PANEL, outline=C_EDGE, width=3)

    # 电源轨
    for yy, lab, col in [(185, "+", C_RED), (212, "−", C_BLACK),
                         (592, "+", C_RED), (619, "−", C_BLACK)]:
        c.line([(bx1 + 30, yy), (bx2 - 30, yy)], fill=col, width=4)
        c.text(bx1 + 10, yy, lab, 17, col, anchor="lm", bold=True)

    # 主区
    x0, y0, dy, cols = 105, 256, 32, 30
    dx = 23
    for r in range(10):
        for k in range(cols):
            c.circle(x0 + k * dx, y0 + r * dy, 5.5, fill=C_HOLE)

    # 凹槽
    c.rect(x0 - 18, 391, x0 + (cols - 1) * dx + 18, 409, fill=(222, 227, 234))
    c.text(x0 + (cols - 1) * dx / 2, 400, "凹　槽（把上下两半断开）", 12, (120, 128, 140),
           anchor="mm")

    # 高亮一列
    k = 6
    cx = x0 + k * dx
    for r in range(5):
        c.circle(cx, y0 + r * dy, 9, outline=C_ORG, width=3)
    c.text(cx, 232, "这 5 个孔连通", 14, C_ORG, anchor="md", bold=True)
    c.line([(cx, 238), (cx, 250)], fill=C_ORG, width=2)

    for r in range(5, 10):
        c.circle(cx, y0 + r * dy, 9, outline=C_BLU, width=3)
    c.text(cx, 566, "下半是另一组", 14, C_BLU, anchor="ma")

    # 右侧说明
    rx = 850
    c.text(rx, 150, "三条规则", 21, C_INK, bold=True)
    notes = [
        ("①  同一列 5 个孔是通的", C_ORG),
        ("    元件插在这 5 个孔里，就等于接在一起。", C_SUB),
        ("    但左右相邻两列互不相通。", C_SUB),
        ("", C_SUB),
        ("②  中间凹槽把上下断开", C_BLU),
        ("    芯片跨在凹槽上，左右两排脚才不会短路。", C_SUB),
        ("", C_SUB),
        ("③  电源轨整条连通", C_RED),
        ("    `+` 和 `−` 两条轨，整条横向都通，", C_SUB),
        ("    专门用来分电：一根线进去，到处能取。", C_SUB),
        ("", C_SUB),
        ("本项目只用 ③", C_GRN),
        ("把 3V3 和 GND 引到两条轨上，两只电容跨在", C_SUB),
        ("轨之间，就自动并联好了。", C_SUB),
        ("", C_SUB),
        ("注意", C_RED),
        ("有些面包板的轨在中间断开，插之前用万用表", C_SUB),
        ("通断档量一下整条轨是否导通。", C_SUB),
    ]
    yy = 190
    for s, col in notes:
        if s:
            c.text(rx, yy, s.replace("`", ""), 14, col)
        yy += 24

    c.save("fig1_breadboard.png")


# ==================================================== 图 2：电源接线
def fig_power():
    c = Cv(1400, 700)
    c.text(40, 28, "电源怎么接（4 根线 + 2 个电容）", 30, C_INK, bold=True)
    c.text(40, 72, "6 根信号线不经过面包板，见第三张图", 15, C_SUB)

    ROW_P = 300   # 3.3V 那一行
    ROW_N = 430   # GND 那一行

    # 屏
    c.rrect(60, 170, 380, 520, 10, fill=C_BOARD, outline=(92, 106, 120), width=3)
    c.text(220, 200, "墨水屏模块", 19, (240, 244, 248), anchor="ma", bold=True)
    c.text(220, 230, "只接这 2 根电源脚", 13, (176, 192, 206), anchor="ma")
    c.rect(372, ROW_P - 12, 412, ROW_P + 12, fill=(206, 172, 94), outline=(150, 122, 60))
    c.text(360, ROW_P, "VCC", 16, (250, 210, 200), anchor="rm", bold=True)
    c.rect(372, ROW_N - 12, 412, ROW_N + 12, fill=(206, 172, 94), outline=(150, 122, 60))
    c.text(360, ROW_N, "GND", 16, (215, 220, 226), anchor="rm", bold=True)

    # 面包板
    c.rrect(560, 240, 940, 500, 12, fill=C_PANEL, outline=C_EDGE, width=3)
    c.text(750, 208, "面包板", 19, C_INK, anchor="ma", bold=True)
    c.line([(578, ROW_P), (922, ROW_P)], fill=C_RED, width=6)
    c.line([(578, ROW_N), (922, ROW_N)], fill=C_BLACK, width=6)
    c.text(586, ROW_P - 26, "+ 轨（3.3V）", 14, C_RED, bold=True)
    c.text(586, ROW_N + 10, "− 轨（GND）", 14, C_BLACK, bold=True)

    for cx, top, lab1, lab2, col in [(660, ROW_P, "10µF", "电解", C_ORG),
                                     (780, ROW_P, "0.1µF", "瓷片", C_PUR)]:
        c.line([(cx, ROW_P), (cx, ROW_N)], fill=col, width=4)
        c.rect(cx - 13, 352, cx + 13, 378, fill=col, outline=col)
        c.text(cx, 470, lab1, 14, col, anchor="ma")
        c.text(cx, 488, lab2, 13, col, anchor="ma")

    # S3
    c.rrect(1080, 170, 1330, 520, 10, fill=C_BOARD, outline=(92, 106, 120), width=3)
    c.text(1205, 200, "ESP32-S3", 19, (240, 244, 248), anchor="ma", bold=True)
    c.text(1205, 230, "DevKitC-1", 15, (176, 192, 206), anchor="ma")
    c.rect(1078, ROW_P - 12, 1118, ROW_P + 12, fill=(206, 172, 94), outline=(150, 122, 60))
    c.text(1132, ROW_P, "3V3", 16, (250, 210, 200), anchor="lm", bold=True)
    c.rect(1078, ROW_N - 12, 1118, ROW_N + 12, fill=(206, 172, 94), outline=(150, 122, 60))
    c.text(1132, ROW_N, "GND", 16, (215, 220, 226), anchor="lm", bold=True)

    # 连线（全水平，不绕）
    c.line([(412, ROW_P), (560, ROW_P)], fill=C_RED, width=5)
    c.line([(940, ROW_P), (1078, ROW_P)], fill=C_RED, width=5)
    c.line([(412, ROW_N), (560, ROW_N)], fill=C_BLACK, width=5)
    c.line([(940, ROW_N), (1078, ROW_N)], fill=C_BLACK, width=5)
    for px, py in [(560, ROW_P), (940, ROW_P), (560, ROW_N), (940, ROW_N)]:
        c.circle(px, py, 4, fill=C_BG)

    c.text(700, 560, "红色 = 3.3V　黑色 = GND", 15, C_SUB, anchor="ma")
    c.text(700, 610, "先别插屏：万用表量 S3 的 3V3 与 GND，应该是 3.3V 左右",
           17, C_RED, anchor="ma", bold=True)
    c.text(700, 642, "量对了再接屏，这一步能省掉一半返工", 14, C_SUB, anchor="ma")

    c.save("fig2_power.png")


# ==================================================== 图 3：S3 引脚
def fig_pins():
    pins = [("1", "3V3", C_RED), ("2", "3V3", C_RED), ("3", "RST", None),
            ("4", "4", None), ("5", "5", None), ("6", "6", None),
            ("7", "7", C_GRN), ("8", "15", None), ("9", "16", None),
            ("10", "17", None), ("11", "18", None), ("12", "8", C_BLU),
            ("13", "3", None), ("14", "46", None), ("15", "9", C_ORG),
            ("16", "10", C_PUR), ("17", "11", C_GRN), ("18", "12", C_BLU),
            ("19", "13", None), ("20", "14", None), ("21", "5V", None),
            ("22", "G", (215, 220, 226))]

    c = Cv(1180, 900)
    c.text(40, 26, "ESP32-S3-DevKitC-1 引脚位置（只用 J1 这一排）", 26, C_INK, bold=True)
    c.text(40, 66, "8 根线全在同一排，不用跨到板子另一侧", 15, C_SUB)

    bx1, by1, bx2, by2 = 70, 110, 520, 860
    c.rrect(bx1, by1, bx2, by2, 12, fill=C_BOARD, outline=(92, 106, 120), width=3)
    c.text(295, 126, "ESP32-S3-DevKitC-1", 16, (235, 240, 246), anchor="ma")
    c.text(295, 148, "（俯视示意，USB 在下方）", 12, (170, 185, 200), anchor="ma")

    y0, dy = 190, 31
    for i, (no, name, col) in enumerate(pins):
        yy = y0 + i * dy
        act = col is not None
        c.rect(bx1 - 26, yy - 8, bx1 - 3, yy + 8,
               fill=(206, 172, 94) if act else (146, 156, 168),
               outline=(118, 128, 140))
        c.text(bx1 - 34, yy, no, 12, C_SUB, anchor="rm")
        c.text(bx1 + 16, yy, name, 16 if act else 14,
               col if act else (168, 180, 192), anchor="lm", bold=act)
        if act:
            c.rect(bx1 + 6, yy - 12, bx2 - 40, yy + 12, outline=col, width=2)

    c.text(bx2 - 16, 76, "另一侧那排（J3）这次不用", 12, C_SUB, anchor="rm")

    lx = 600
    c.text(lx, 110, "要用的 8 根", 21, C_INK, bold=True)
    leg = [
        ("1", "3V3", "→ 屏 VCC（先到面包板 + 轨）", C_RED),
        ("22", "G", "→ 屏 GND（先到面包板 − 轨）", (90, 90, 96)),
        ("17", "11", "→ 屏 DIN（数据）", C_GRN),
        ("18", "12", "→ 屏 CLK（时钟）", C_BLU),
        ("16", "10", "→ 屏 CS（片选）", C_PUR),
        ("15", "9", "→ 屏 DC（命令/数据）", C_ORG),
        ("12", "8", "→ 屏 RST（复位）", C_BLU),
        ("7", "7", "→ 屏 BUSY（忙信号）", C_GRN),
    ]
    yy = 148
    for no, nm, desc, col in leg:
        c.text(lx, yy, "第 %s 根" % no, 14, C_SUB)
        c.text(lx + 66, yy, nm, 17, col, bold=True)
        c.text(lx + 118, yy, desc, 14, C_SUB)
        yy += 40

    c.text(lx, yy + 8, "怎么认", 18, C_INK, bold=True)
    tips = [
        "板子每个针脚旁边都印了名字：3V3 / G / 7 / 11 …",
        "图上的「第几根」是从 3V3 那一端数起；",
        "两者对不上时，一律以板上丝印为准。",
        "",
        "小技巧：GPIO 7~12 全在这一排，",
        "所以 6 根信号线都从同一侧引出，",
        "不会绕到背面。",
        "",
        "板子另一侧那排（J3）这次一根都不用。",
    ]
    yy += 42
    for t in tips:
        if t:
            c.text(lx, yy, t, 14, C_SUB)
        yy += 25

    c.save("fig3_pins.png")


if __name__ == "__main__":
    fig_breadboard()
    fig_power()
    fig_pins()
