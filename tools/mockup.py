# -*- coding: utf-8 -*-
"""按真实毫米比例画出 4.2 寸墨水屏阅读器的形态示意（尺寸图，不是照片）。

输出：mockup_reader.png
用法：在仓库根目录执行  python epaper29/tools/mockup.py
依赖根目录的 preview_p1.png（由 tools/push.py 生成）
"""
import os
from PIL import Image, ImageDraw, ImageFont

S = 4.6             # 1mm = 4.6px
SS = 2              # 超采样倍数
W, H = 1500, 1170   # 约 326 x 254 mm
VSC = 2.0           # 侧视剖面垂直放大倍数（薄层才看得见）

C_INK    = (32, 36, 42)
C_SUB    = (108, 118, 130)
C_LINE   = (150, 160, 172)
C_PLATE  = (244, 246, 249)
C_PLATEE = (184, 192, 203)
C_PCB    = (40, 52, 60)
C_PCBE   = (88, 106, 118)
C_GOLD   = (208, 172, 94)
C_BATT   = (62, 92, 78)
C_CASE   = (232, 236, 241)
C_CASEE  = (148, 158, 170)
C_ACC    = (192, 60, 56)
C_DIM    = (118, 130, 144)


def font(size, bold=False):
    cands = ["C:/Windows/Fonts/msyhbd.ttc"] if bold else []
    cands += ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
              "C:/Windows/Fonts/simsun.ttc"]
    for p in cands:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, int(size * SS))
            except Exception:
                pass
    return ImageFont.load_default()


F_TITLE = font(30, True)
F_SEC   = font(19, True)
F_BODY  = font(15)
F_SM    = font(13)
F_TINY  = font(12)
F_DIM   = font(12)

img = Image.new("RGB", (W * SS, H * SS), (255, 255, 255))
d = ImageDraw.Draw(img)


def X(v):
    return v * SS


def T(mx, my, s, f=F_BODY, col=C_INK, anchor="la"):
    d.text((X(mx * S), X(my * S)), s, font=f, fill=col, anchor=anchor)


class Block(object):
    def __init__(self, ox_mm, oy_mm):
        self.ox, self.oy = ox_mm, oy_mm

    def px(self, mx, my):
        return (X((self.ox + mx) * S), X((self.oy + my) * S))

    def rect(self, mx, my, mw, mh, **kw):
        x1, y1 = self.px(mx, my)
        x2, y2 = self.px(mx + mw, my + mh)
        d.rectangle([x1, y1, x2, y2], **kw)

    def rrect(self, mx, my, mw, mh, r_mm, **kw):
        x1, y1 = self.px(mx, my)
        x2, y2 = self.px(mx + mw, my + mh)
        d.rounded_rectangle([x1, y1, x2, y2], radius=X(r_mm * S), **kw)

    def text(self, mx, my, s, f=F_BODY, col=C_INK, anchor="la"):
        x, y = self.px(mx, my)
        d.text((x, y), s, font=f, fill=col, anchor=anchor)

    def lw(self, mm=0.8):
        return max(1, int(mm * S * SS))


def draw_dim(b, kind, a, c, off, label, col=C_DIM):
    if kind == "h":
        x1, y1 = b.px(a, off)
        x2, _ = b.px(c, off)
        d.line([x1, y1, x2, y1], fill=col, width=b.lw(0.35))
        for xx, sgn in ((x1, 1), (x2, -1)):
            d.polygon([(xx, y1), (xx + sgn * X(6), y1 - X(3)),
                       (xx + sgn * X(6), y1 + X(3))], fill=col)
        if label:
            d.text(((x1 + x2) / 2.0, y1 - X(10)), label, font=F_DIM,
                   fill=col, anchor="md")
    else:
        x1, y1 = b.px(off, a)
        _, y2 = b.px(off, c)
        d.line([x1, y1, x1, y2], fill=col, width=b.lw(0.35))
        for yy, sgn in ((y1, 1), (y2, -1)):
            d.polygon([(x1, yy), (x1 - X(3), yy + sgn * X(6)),
                       (x1 + X(3), yy + sgn * X(6))], fill=col)
        if label:
            d.text((x1 - X(6), (y1 + y2) / 2.0), label, font=F_DIM,
                   fill=col, anchor="rm")


# ============================================================ 标题
T(10, 6.5, "4.2 寸墨水屏阅读器 · 做出来长什么样", F_TITLE)
T(10, 15.5, "按真实毫米比例绘制。左：接下来一个月桌上的原型；右：装壳后的成品；右下：16mm 厚度是怎么堆出来的。",
  F_SM, C_SUB)

# ============================================================ ① 桌面原型
P = Block(12, 36)
P.text(0, -13, "① 桌面原型（阶段 1~4）", F_SEC)

P.rrect(-6, -6, 140, 130, 3, fill=C_PLATE, outline=C_PLATEE, width=P.lw(0.6))
P.rrect(8, 6, 103, 78.5, 1.5, fill=C_PCB, outline=C_PCBE, width=P.lw(0.7))
P.rect(8 + 6.45, 6 + 1.4, 90.1, 77, outline=(120, 142, 156), width=P.lw(0.5))

pg = Image.open("preview_p1.png").convert("RGB")
vw, vh = int(84.8 * S * SS), int(63.6 * S * SS)
pg = pg.resize((vw, vh), Image.LANCZOS)
vx, vy = P.px(8 + 9.1, 6 + 8.0)
img.paste(pg, (int(vx), int(vy)))
P.rect(8 + 9.1, 6 + 8.0, 84.8, 63.6, outline=(92, 102, 112), width=P.lw(0.5))

for i in range(8):
    P.rect(8 + 6 + i * 2.54, 6 + 75.0, 1.8, 1.8, fill=C_GOLD,
           outline=(152, 124, 64))

P.rrect(8, 92, 63, 26, 1.2, fill=C_PCB, outline=C_PCBE, width=P.lw(0.7))
for i in range(22):
    P.rect(9 + i * 2.54, 92.5, 1.6, 1.6, fill=C_GOLD, outline=(152, 124, 64))
    P.rect(9 + i * 2.54, 92 + 23.9, 1.6, 1.6, fill=C_GOLD, outline=(152, 124, 64))
P.rect(60.5, 98, 6, 8, fill=(178, 184, 192), outline=(122, 130, 140))
P.text(9.5, 100.5, "ESP32-S3-DevKitC", F_SM, (228, 234, 240))

P.rrect(76, 88, 50, 33, 2.5, fill=C_BATT, outline=(36, 56, 46), width=P.lw(0.7))
P.text(88.5, 92.5, "锂电 1000mAh", F_TINY, (236, 243, 238))
P.text(93.5, 99.5, "50×35×6", F_TINY, (200, 216, 208))

for i in range(8):
    p0 = P.px(8 + 6 + i * 2.54 + 0.9, 6 + 76.5)
    p2 = P.px(9 + i * 7.2 + 0.8, 92)
    p1 = ((p0[0] + p2[0]) / 2.0 + X((i - 3.5) * 1.6),
          (p0[1] + p2[1]) / 2.0 + X(8))
    pts = []
    for k in range(27):
        t = k / 26.0
        pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t ** 2 * p2[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t ** 2 * p2[1]))
    d.line(pts, fill=(178, 134, 98), width=P.lw(0.9))

draw_dim(P, "h", -6, 134, 132, "140 mm（底板，可不要）")
P.text(-6, 137, "屏模块 103×78.5 ｜ 开发板 63×26 ｜ 电池 50×35×6 ｜ 8pin: VCC GND DIN CLK CS DC RST BUSY",
       F_SM, C_SUB)
P.text(-6, 143, "整套摊开约一张 A6 纸（105×148）的大小，图上按实际尺寸摆放", F_SM, C_SUB)

# ============================================================ ② 装壳成品
Q = Block(176, 36)
Q.text(0, -13, "② 装壳成品（阶段 5，换 4.2 寸裸屏）", F_SEC)

Q.rrect(2.5, 3, 96, 92, 4, fill=(226, 230, 236))
Q.rrect(0, 0, 96, 92, 4, fill=C_CASE, outline=C_CASEE, width=Q.lw(0.7))
Q.rect(2.95, 3, 90.1, 77, outline=(152, 162, 174), width=Q.lw(0.5))
pg2 = Image.open("preview_p1.png").convert("RGB").resize((vw, vh), Image.LANCZOS)
vx2, vy2 = Q.px(5.6, 5.0)
img.paste(pg2, (int(vx2), int(vy2)))
Q.rect(5.6, 5.0, 84.8, 63.6, outline=(92, 102, 112), width=Q.lw(0.5))

for bx in (38, 58):
    cxp, cyp = Q.px(bx, 85.5)
    d.ellipse([cxp - X(3.2 * S), cyp - X(3.2 * S),
               cxp + X(3.2 * S), cyp + X(3.2 * S)],
              fill=(126, 134, 144), outline=(88, 96, 106), width=Q.lw(0.4))
Q.text(48, 74.2, "上页　　下页", F_TINY, C_SUB, anchor="md")

draw_dim(Q, "h", 0, 96, 99, "96 mm")
draw_dim(Q, "v", 0, 92, -8, "92")

ax = 104
notes = [("可视区 84.8 × 63.6", C_ACC),
         ("＝ 真正能显示字的区域", C_SUB),
         ("", C_SUB),
         ("屏外形 90.1 × 77.0", C_SUB),
         ("屏下边框 11.4 mm", C_SUB),
         ("→ 正好放两个按键", C_SUB),
         ("", C_SUB),
         ("机身厚 ≈ 16 mm", C_INK),
         ("（开发板 + 杜邦线）", C_SUB)]
for i, (s, col) in enumerate(notes):
    Q.text(ax, 5 + i * 6.2, s, F_SM, col)

lx, ly = Q.px(5.6 + 84.8, 25)
d.line([lx, ly, X((Q.ox + 102) * S), X((Q.oy + 8) * S)], fill=C_ACC,
       width=Q.lw(0.4))
d.ellipse([lx - X(1.6), ly - X(1.6), lx + X(1.6), ly + X(1.6)], fill=C_ACC)

ccx, ccy = Q.px(10, 100)
d.ellipse([ccx, ccy, ccx + X(25 * S), ccy + X(25 * S)], outline=C_LINE,
          width=Q.lw(0.45))
Q.text(10 + 27.5, 107, "⌀25mm 的一元硬币，比一下大小", F_SM, C_SUB, anchor="lm")

# ============================================================ ③ 侧视剖面
R = Block(176, 172)
R.text(0, -13, "③ 侧视剖面：16mm 厚度从哪来", F_SEC)

layers = [(1, "前壳 / 亚克力", 1.2, (238, 242, 247)),
          (2, "裸屏（含偏光片）", 1.2, (250, 250, 252)),
          (3, "驱动板（升压+电平转换）", 1.6, C_PCB),
          (4, "主控板 + 排针", 4.6, C_PCB),
          (5, "电池 1000mAh", 6.0, C_BATT),
          (6, "后壳", 1.5, (222, 228, 235))]
yy = 0.0
for num, name, t, col in layers:
    R.rect(0, yy * VSC, 96, t * VSC, fill=col, outline=(126, 134, 146),
           width=R.lw(0.4))
    dark = col in (C_PCB, C_BATT)
    R.text(2.4, (yy + t / 2.0) * VSC, str(num), F_TINY,
           (240, 244, 248) if dark else C_INK, anchor="lm")
    yy += t

draw_dim(R, "v", 0, yy * VSC, -9, "")
R.text(-9, yy * VSC + 3, "合计 ≈ %.1f mm" % yy, F_BODY, C_ACC)

leg = [(1, "前壳", 1.2), (2, "裸屏", 1.2), (3, "驱动板", 1.6),
       (4, "主控板 + 排针", 4.6), (5, "电池", 6.0), (6, "后壳", 1.5)]
for i, (num, name, t) in enumerate(leg):
    col_i, row_i = i % 3, i // 3
    R.text(col_i * 46, yy * VSC + 12 + row_i * 6.0,
           "%d  %s  %.1f mm" % (num, name, t), F_SM, C_SUB)

R.text(0, yy * VSC + 27,
       "（垂直方向放大 2 倍）想更薄：主控换模组贴片省 3mm，电池换 4mm 超薄款。",
       F_SM, C_SUB)

# ============================================================ 尺寸一览
B = Block(12, 196)
B.text(0, 0, "尺寸一览", F_SEC)
rows = [("可视区（能显示字的地方）", "84.8 × 63.6 mm"),
        ("屏外形（含边框）", "90.1 × 77.0 × 1.18 mm"),
        ("屏模块（含驱动板）", "103.0 × 78.5 mm"),
        ("成品机身（裸屏方案）", "96 × 92 × 16 mm"),
        ("一屏字数（16px 中文）", "约 250 字 / 11 行")]
for i, (k, v) in enumerate(rows):
    col_i, row_i = i // 3, i % 3
    B.text(col_i * 82, 10 + row_i * 7.4, k, F_SM, C_SUB)
    B.text(col_i * 82 + 46, 10 + row_i * 7.4, v, F_SM, C_INK)

B.text(0, 40, "注：这是按毫米比例画的尺寸示意图，不是照片。外形轮廓、按键位置、厚度均为估算，实物以到手的物料为准。",
       F_SM, C_SUB)
B.text(0, 46, "16mm 的前提是「开发板 + 排针 + 杜邦线」；直接上 S3 模组贴片能到 10~11mm，那属于打板阶段。",
       F_SM, C_SUB)

img = img.resize((W, H), Image.LANCZOS)
img.save("mockup_reader.png")
print("saved mockup_reader.png %dx%d" % img.size)
