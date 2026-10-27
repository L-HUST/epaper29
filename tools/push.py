#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
push.py —— 上位机：把文字/图片/整本书渲染成 1bpp 位图，通过串口推给墨水屏

支持三种屏（用 --model 选，要和固件 board_config.h 里的 EPD_MODEL 一致）：
  29  : 2.9寸  296x128   -> 4736 字节/屏
  42  : 4.2寸  400x300   -> 15000 字节/屏   （默认）
  426 : 4.26寸 800x480   -> 48000 字节/屏

常见用法（前三种不需要接硬件）：

  1) 看排版效果：
       python tools/push.py --text "INK READER|2026-01-01|26.5C 60%" --preview out.png

  2) 图片转位图：
       python tools/push.py --image photo.jpg --preview out.png --raw frame.bin

  3) 读书（自动分页 + 渲染成图，不需要设备）：
       python tools/push.py --book novel.txt --page 3 --preview page3.png
       # 第一次会在 novel.txt 旁边生成 novel.txt.pages.json 页码索引
       # 串口翻页请用 tools/read.py（带 上一页/下一页 和残影计数）

  4) 推到设备：
       python tools/push.py --port COM5 --text "HELLO|world"
       python tools/push.py --port COM5 --book novel.txt --page 3

依赖：pip install pillow pyserial
      （只有第 4 步需要 pyserial）

位图格式（和固件一致）：
  1bpp；每行 = 宽度/8 字节；1 = 白，0 = 黑；字节内高位在前（MSB = 左边像素）。
  注意：宽度必须是 8 的整数倍，所以 2.9 寸用 296、4.2 寸用 400。
"""
import argparse
import json
import os
import struct
import sys
import time
import zlib

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow:  python -m pip install pillow")

# 串口协议（见 firmware/src/main.cpp）
MAGIC = b"\xAA\x55"
ACK, NAK = 0x06, 0x15


class Model(object):
    def __init__(self, key, name, w, h):
        self.key = key
        self.name = name
        self.w = w
        self.h = h
        self.stride = w // 8
        self.frame_len = self.stride * h

    def __str__(self):
        return "%s (%dx%d, %d 字节/屏)" % (self.name, self.w, self.h, self.frame_len)


MODELS = {
    "29":  Model("29",  "2.9inch",  296, 128),
    "42":  Model("42",  "4.2inch",  400, 300),
    "426": Model("426", "4.26inch", 800, 480),
}
DEFAULT_MODEL = "29"      # 本项目主线是 2.9 寸；换屏加 --model 42


# ------------------------------------------------------------------ 字体
FONT_CANDS = [
    "C:/Windows/Fonts/msyh.ttc",           # 微软雅黑（中文）
    "C:/Windows/Fonts/simhei.ttf",         # 黑体
    "C:/Windows/Fonts/simsun.ttc",         # 宋体
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/arial.ttf",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/PingFang.ttc",
]


# 指定字体：命令行 --font 会覆盖这里的候选列表
FONT_OVERRIDE = None

# 墨水屏只有黑白，抗锯齿的灰度最后必须二值化；13px 中文的细笔画在灰度里
# 只有 40% 覆盖率，判定时整根丢掉——这就是"字不全 / 断笔"的来源。
# 做法：直接用 FreeType 的单色（hinting 后）位图写字，横竖都落在像素格上。
def binarize(img, threshold=128):
    return img.point(lambda v: 255 if v > threshold else 0).convert("1")


def paste_text(canvas, xy, text, font):
    """用单色（hinted）位图写字，避免抗锯齿灰度在二值化时丢笔画。"""
    try:
        m = font.getmask(text, mode="1")
        w, h = m.size
        data = bytes(m)
        if len(data) == w * h:
            ink = Image.frombytes("L", (w, h), data)
        else:
            ink = Image.frombytes("1", (w, h), data).convert("L")
        ink = Image.eval(ink, lambda v: 255 - v)
        bb = font.getbbox(text)
        canvas.paste(ink, (int(xy[0]) + bb[0], int(xy[1]) + bb[1]))
    except Exception:
        ImageDraw.Draw(canvas).text(xy, text, fill=0, font=font)


def _font_cands(family_size, bold=False):
    """按“排版用的字号”挑字体族；family_size 与绘制字号分开，
    是为了超采样时 2 倍字号也仍然用同一个字体族。"""
    if bold:
        return ["C:/Windows/Fonts/msyhbd.ttc"] + FONT_CANDS
    if family_size and family_size <= 18:
        # 小字号优先黑体：笔画粗细均匀，二值化后最不容易断笔
        # （13px 微软雅黑会把横画吃掉一半，屏上看着像"字不全"）
        return ["C:/Windows/Fonts/simhei.ttf"] + FONT_CANDS
    return FONT_CANDS


def load_font(size, bold=False, family_size=None):
    if FONT_OVERRIDE:
        try:
            return ImageFont.truetype(FONT_OVERRIDE, size)
        except Exception as e:
            print("  字体加载失败（%s），回退到默认候选：%s" % (FONT_OVERRIDE, e))
    fam = size if family_size is None else family_size
    for p in _font_cands(fam, bold):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def fit_font(model, lines, size=None, pad=12, line_k=1.4, max_size=72):
    """选一个字号，让所有行都能放进屏幕。返回 (font, size, line_h)"""
    if size:
        f = load_font(size)
        return f, size, int(size * line_k)
    s = max_size
    while s > 8:
        f = load_font(s)
        lh = int(s * line_k)
        widest = max((f.getlength(ln) for ln in lines), default=0)
        if lh * len(lines) <= model.h - pad * 2 and widest <= model.w - pad * 2:
            return f, s, lh
        s -= 1
    f = load_font(8)
    return f, 8, int(8 * line_k)


def text_to_image(model, lines, size=None, bold=False, border=True, align="left",
                  pad=10):
    font, sz, lh = fit_font(model, lines, size=size, pad=pad)
    img = Image.new("L", (model.w, model.h), 255)
    total = lh * len(lines)
    y = max(pad, (model.h - total) // 2)
    for ln in lines:
        w = font.getlength(ln)
        if align == "center":
            x = max(2, int((model.w - w) / 2))
        elif align == "right":
            x = max(2, int(model.w - w - pad))
        else:
            x = pad
        paste_text(img, (x, y), ln, font)
        y += lh
    if border:
        ImageDraw.Draw(img).rectangle([0, 0, model.w - 1, model.h - 1], outline=0)
    return binarize(img)



def image_to_bitmap(model, img, dither=True, invert=False, fit=True, margin=2):
    """PIL 图 -> 字节串（1=白 0=黑）"""
    img = img.convert("L")
    if fit and img.size != (model.w, model.h):
        k = min((model.w - 2 * margin) / float(img.width),
                (model.h - 2 * margin) / float(img.height))
        nw, nh = max(1, int(img.width * k)), max(1, int(img.height * k))
        small = img.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("L", (model.w, model.h), 255)
        canvas.paste(small, ((model.w - nw) // 2, (model.h - nh) // 2))
        img = canvas
    # 不用 Image.Dither 枚举：老版本 Pillow(<9.1) 没有，直接用整型常量
    bw = img.convert("1", dither=1 if dither else 0)
    if invert:
        bw = bw.point(lambda v: 0 if v else 255).convert("1")

    px = bw.load()
    out = bytearray()
    for y in range(model.h):
        for bx in range(model.stride):
            b = 0
            for bit in range(8):
                if px[bx * 8 + bit, y]:
                    b |= 0x80 >> bit
            out.append(b)
    assert len(out) == model.frame_len, len(out)
    return bytes(out)


def bitmap_to_image(model, data):
    """反向：字节流 -> PIL 图（用于校验/预览）"""
    img = Image.new("1", (model.w, model.h), 1)
    px = img.load()
    for y in range(model.h):
        base = y * model.stride
        for x in range(model.w):
            px[x, y] = 1 if (data[base + (x >> 3)] & (0x80 >> (x & 7))) else 0
    return img


# ------------------------------------------------------------------ 分页（读书）
def wrap_text(text, font, max_w):
    """按字符（中文）或单词（英文）折行，返回行列表。"""
    out = []
    for para in text.split("\n"):
        para = para.rstrip()
        if not para.strip():
            out.append("")
            continue
        line = ""
        i = 0
        while i < len(para):
            ch = para[i]
            # ASCII 单词整体处理，避免在单词中间断行
            if ch.isascii() and (ch.isalnum() or ch in "-'"):
                j = i
                while j < len(para) and para[j].isascii() and (para[j].isalnum() or para[j] in "-'"):
                    j += 1
                token = para[i:j]
            else:
                token = ch
                j = i + 1
            if font.getlength(line + token) <= max_w or not line:
                line += token
            else:
                out.append(line)
                line = token.lstrip()
            i = j
        out.append(line)
    return out


# 小屏（2.9 寸 296x128）专用版式：边框贴屏幕边缘、页眉 13px、下划线紧跟页眉、
# 不要页脚——省下来的高度全给正文，同样的 15px 字号能从 2 行变 5 行。
SMALL_H = 160
SMALL_PAD = 4
SMALL_HDR_SZ = 13


def layout(model, size=None, line_k=1.35, pad=12, header=None, footer=None,
           fill=True):
    """统一的版面计算，分页和渲染共用，避免两边算不一致。

    size=None 且 fill=True 时，自动挑一个"能填满一页"的字号：
    屏高确定的条件下，字号越大每页字越少；为了让一页尽可能多放内容，
    从小到大试，取仍能满足目标行数的最大字号。
    """
    small = model.h <= SMALL_H
    if small:
        pad = SMALL_PAD
        footer = None                      # 小屏不要页脚，那 24px 留给正文
    hdr_lh = max(10, int(round((SMALL_HDR_SZ if small else (size or 15)) * 1.25)))

    def metrics(sz):
        f = load_font(sz, family_size=sz)
        lh = max(8, int(round(sz * line_k)))
        hl = max(10, int(round((SMALL_HDR_SZ if small else sz) * 1.25)))
        y = pad
        if header:
            y = pad + hl + 4              # 页眉行高 + 下划线及其间距
        bottom = model.h - pad - ((lh + 4) if footer else 0)
        return f, lh, y, bottom, hl

    if size is None and fill:
        # 字号下限 13px：低于这个值中文字形会糊成一团
        MIN_SZ = 13
        if small:
            # 2.9 寸 296x128：实物对比过 13/15/16 三档，
            # 13px 每页 3 行但复杂字会掉笔画，15px 笔画舒展 → 默认 15
            size = 15
        else:
            # 大屏（4.2/4.26）目标行数跟着屏高走
            target_rows = max(3, min(10, model.h // 30))
            best = None
            for sz in range(MIN_SZ, 40):
                f, lh, y, bottom, _ = metrics(sz)
                if (bottom - y) // lh >= target_rows:
                    best = sz
            size = best if best is not None else MIN_SZ

    font, lh, body_y, bottom, hdr_lh = metrics(size)
    rows = max(1, (bottom - body_y) // lh)
    return {"font": font, "size": size, "lh": lh, "rows": rows,
            "max_w": model.w - 2 * pad, "body_y": body_y, "pad": pad,
            "bottom": bottom, "hdr_lh": hdr_lh, "small": small}


def paginate(model, text, size=None, line_k=1.35, pad=12, header=None, footer=None):
    """把整本书切成页，返回 ([(起始偏移, 行列表), ...], 版面参数)"""
    L = layout(model, size, line_k, pad, header, footer)
    rows, max_w, font = L["rows"], L["max_w"], L["font"]

    pages = []
    buf = []
    offset = 0
    for para in text.split("\n"):
        for ln in wrap_text(para + "\n", font, max_w):
            if not ln.strip():
                continue          # 空行不占版面：2.9 寸每页只有 5 行，浪费不起
            if len(buf) >= rows:
                pages.append((offset, buf))
                offset += sum(len(x) for x in buf)
                buf = []
            buf.append(ln)
    if buf:
        pages.append((offset, buf))
    return pages, L


def render_page(model, lines, L, header=None, footer=None):
    img = Image.new("L", (model.w, model.h), 255)
    pad, lh = L["pad"], L["lh"]
    font = load_font(L["size"], family_size=L["size"])
    small = L.get("small", False)

    if header:
        hsz = SMALL_HDR_SZ if small else L["size"]
        hfont = load_font(hsz, family_size=hsz) if hsz != L["size"] else font
        paste_text(img, (pad, (pad - 2) if small else max(2, pad - 4)), header, hfont)

    y = L["body_y"]
    for ln in lines:
        if y + lh > L["bottom"] + 1:
            break
        paste_text(img, (pad, y), ln, font)
        y += lh

    if footer and not small:            # 小屏不显示页脚（百分比）
        paste_text(img, (pad, model.h - pad - lh + 4), footer, font)

    d2 = ImageDraw.Draw(img)
    if header:
        uy = L["body_y"] - (3 if small else 5)     # 小屏下划线紧贴页眉
        d2.line([pad, uy, model.w - 1 - pad if small else model.w - pad, uy], fill=0)
    d2.rectangle([0, 0, model.w - 1, model.h - 1], outline=0)
    return binarize(img)



def book_pages(model, path, size, line_k, pad):
    """带缓存的分页：第一次跑要排版整本书，之后读 json 缓存。
    size 传 None 表示按屏高自动定字号。"""
    cache = path + ".pages.json"
    # 改排版/清洗逻辑时把这个版本号 +1，旧缓存自动失效（否则会读到旧的错误排版）
    CACHE_VER = 6   # 6: 小屏版式（pad4/页眉13px/无页脚），旧分页结果作废
    key = "v%d|%s|%s|%d|%d|%d" % (CACHE_VER, model.key, size, int(line_k * 100), pad,
                                  os.path.getmtime(path))
    if os.path.exists(cache):
        try:
            with open(cache, "r", encoding="utf-8") as f:
                data = json.load(f)
            if data.get("key") == key:
                return data["pages"], data["total_lines"], data.get("size")
        except Exception:
            pass

    # utf-8-sig：自动吃掉 BOM。Windows 记事本/很多下载站的书都带 BOM，
    # 用 utf-8 读会把 U+FEFF 当成正文第一个字符，渲染成黑方块（每本书第一页都有）
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        text = f.read()
    text = text.replace("\ufeff", "").replace("\r\n", "\n").replace("\r", "\n")
    # 分页时按"有页眉页脚"的最坏情况算行数，实际渲染也一致，避免两边错位
    pages, L = paginate(model, text, size, line_k, pad, header="H", footer="F")
    out = []
    n = 0
    for start, lines in pages:
        out.append({"line": n, "lines": lines})
        n += len(lines)
    # 缓存写不进去（目录只读、在共享盘上、被占用）不算错误，顶多下次重新排版
    try:
        with open(cache, "w", encoding="utf-8") as f:
            json.dump({"key": key, "total_lines": n, "size": L["size"],
                       "pages": out}, f, ensure_ascii=False)
    except OSError as e:
        print("  （分页缓存写不进去，忽略：%s）" % e)
    return out, n, L["size"]


# ------------------------------------------------------------------ 串口
def crc16_ccitt(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def wait_ready(port, timeout=15.0):
    """打开串口会把板子复位，固件启动完后会打一行 READY.；等它出现再发数据。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        ln = port.readline().decode("ascii", "replace").strip()
        if ln.startswith("READY"):
            return True
    return False


def send_frame(port, data, timeout=10.0):
    port.reset_input_buffer()
    port.write(MAGIC)
    port.flush()
    # 等 ACK：中间可能混进开机日志，所以逐字节找 0x06
    acked = False
    got = b""
    deadline = time.time() + 3.0
    while time.time() < deadline:
        b = port.read(1)
        if b and b[0] == ACK:
            acked = True
            break
        got += b
    if not acked:
        raise IOError("设备没有回 ACK，检查是否烧了固件/串口是否被占用 (收到 %r)"
                      % got[:40])

    head = bytearray(512)
    struct.pack_into("<I", head, 0, len(data))
    # USB-CDC 没有流控，一口气灌 5KB 会丢字节；分块发，每块停顿一下
    payload = bytes(head) + bytes(data) + struct.pack(">H", crc16_ccitt(data))
    CHUNK = 256
    for i in range(0, len(payload), CHUNK):
        port.write(payload[i:i + CHUNK])
        port.flush()
        time.sleep(0.003)
    port.flush()

    # 刷屏要 2.3 秒，先吐 [ep29] refresh 一行，再吐 OK frame。
    # 一直读到 OK/NAK 为止，别把开机时那行 refresh 当成回复。
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        ln = port.readline().decode("ascii", "replace").strip()
        if not ln:
            continue
        lines.append(ln)
        if ln.startswith("OK") or ln.startswith("NAK"):
            break
    return " / ".join(lines) if lines else "(无回复)"


def report(model, bw):
    img = bitmap_to_image(model, bw)
    black = sum(1 for y in range(model.h) for x in range(model.w)
                if not img.getpixel((x, y)))
    ratio = black * 100.0 / (model.w * model.h)
    print("屏 %s  帧长 %d 字节  黑点占比 %.1f%%" % (model, len(bw), ratio))
    if ratio > 65:
        print("  提醒：黑点太多，全黑区域刷新慢且容易残留")
    if ratio < 0.5:
        print("  提醒：几乎全白，检查内容是不是没画上去")
    return img.convert("L")     # 转成 L 再存，避免有些 Pillow 版本写 1 位 PNG 报错


def main():
    ap = argparse.ArgumentParser(description="墨水屏上位机推送工具")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--text", help="用 | 分行的文字")
    src.add_argument("--text-file", help="从文本文件读，每行一行")
    src.add_argument("--image", help="图片路径（jpg/png/bmp）")
    src.add_argument("--book", help="整本书 txt，配合 --page 使用（自动分页）")

    ap.add_argument("--font", help="指定字体文件，如 C:/Windows/Fonts/simhei.ttf")

    ap.add_argument("--model", choices=list(MODELS.keys()), default=DEFAULT_MODEL,
                    help="屏型号，要和固件一致（默认 42）")
    ap.add_argument("--page", type=int, default=1, help="页号，从 1 开始")
    ap.add_argument("--size", type=int, default=None,
                    help="字号；--book 不填则按屏高自动定（推荐）")
    ap.add_argument("--line-k", type=float, default=1.35, help="行距系数")
    ap.add_argument("--no-header", action="store_true", help="读书时不画页眉页脚")
    ap.add_argument("--align", choices=["left", "center", "right"], default="left")
    ap.add_argument("--bold", action="store_true")
    ap.add_argument("--no-border", action="store_true")
    ap.add_argument("--no-dither", action="store_true")
    ap.add_argument("--invert", action="store_true")

    ap.add_argument("--port", help="串口号，如 COM5（不填就只生成本地文件）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--preview", help="结果存成 PNG")
    ap.add_argument("--raw", help="4736/15000 字节原始位图存成文件")
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args()

    global FONT_OVERRIDE
    if a.font:
        if not os.path.exists(a.font):
            sys.exit("字体文件不存在：%s" % a.font)
        FONT_OVERRIDE = a.font
        print("  字体：%s" % a.font)

    model = MODELS[a.model]

    if a.image:
        bw = image_to_bitmap(model, Image.open(a.image), dither=not a.no_dither,
                             invert=a.invert)
    elif a.book:
        hdr = None if a.no_header else "H"
        ftr = None if a.no_header else "F"
        pages, total, fitted = book_pages(model, a.book, a.size, a.line_k, 12)
        if not 1 <= a.page <= len(pages):
            sys.exit("页号超范围：这本书共 %d 页" % len(pages))
        p = pages[a.page - 1]
        L = layout(model, fitted, a.line_k, 12, header=hdr, footer=ftr)
        img = render_page(model, p["lines"], L,
                          header=None if a.no_header else
                          "%s  p%d/%d" % (os.path.basename(a.book), a.page, len(pages)),
                          footer=None)          # 不再显示底部百分比，空间留给正文
        bw = image_to_bitmap(model, img, dither=False, invert=a.invert, fit=False)
        print("全书 %d 行 / %d 页，本页第 %d~%d 行，字号 %s，每页 %d 行"
              % (total, len(pages), p["line"] + 1, p["line"] + len(p["lines"]),
                 fitted, L["rows"]))
        if L["rows"] < 2:
            print("  警告：这块屏每页只有 %d 行，读书体验很差（屏太矮）。"
                  "建议换 4.2 寸以上（--model 42）" % L["rows"])
    else:
        if a.text_file:
            with open(a.text_file, "r", encoding="utf-8-sig") as f:
                lines = [ln.rstrip("\n") for ln in f if ln.strip()]
        else:
            lines = a.text.split("|")
        img = text_to_image(model, lines, size=a.size, bold=a.bold,
                            border=not a.no_border, align=a.align)
        bw = image_to_bitmap(model, img, dither=False, invert=a.invert, fit=False)

    img = report(model, bw)
    if a.preview:
        img.save(a.preview)
        print("预览图：", a.preview)
    if a.raw:
        with open(a.raw, "wb") as f:
            f.write(bw)
        print("原始位图：", a.raw, " zlib crc32=%08X" % (zlib.crc32(bw) & 0xFFFFFFFF))

    if a.port:
        try:
            import serial
        except ImportError:
            sys.exit("需要 pyserial:  python -m pip install pyserial")
        import time
        with serial.Serial(a.port, a.baud, timeout=10.0) as port:
            # 打开串口会把 DTR/RTS 拉起来，ESP32-S3 的自动复位电路会因此重启，
            # 结果就是"推图抢在开机前面"。先把这两根线放掉再发。
            port.dtr = False
            port.rts = False
            time.sleep(0.2)
            port.reset_input_buffer()
            if wait_ready(port):
                print("设备已就绪（板子被串口复位过，等它启动完）")
            else:
                print("没等到 READY，直接发")
            print("设备返回：", send_frame(port, bw))
            port.dtr = False
            port.rts = False


if __name__ == "__main__":
    main()
