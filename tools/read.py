#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
read.py —— 连续阅读：键盘翻页，自动管理残影

用法：
    python tools/read.py --port COM5 --book novel.txt
    python tools/read.py --port COM5 --book novel.txt --from 12     # 从第 12 页开始
    python tools/read.py --book novel.txt --render-only --pages 1-3 # 不接设备，只出图看效果

按键：
    n / 空格 / →   下一页
    p / ←          上一页
    f              强制全刷一次（清残影）
    s              让设备休眠
    q              退出

关于刷新方式（重要）：
    本工具的翻页走的是"整屏推图 + 全刷"，每次都是干净的全屏刷新，
    所以不会有残影累积问题，也就不需要自动插全刷。
    代价是每页约 5 秒（全刷）+ 传输时间（15000 字节 @115200 ≈ 1.3 秒）。
    想更快翻页请用固件的局刷文本命令（仅 ASCII，见 README 步骤 8），
    那条路才需要 --refresh-every 这种残影管理策略。

依赖：pip install pillow pyserial
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from push import (MODELS, DEFAULT_MODEL, layout, render_page, image_to_bitmap,
                  book_pages, MAGIC, ACK, crc16_ccitt)


def send_frame(port, data):
    port.reset_input_buffer()
    port.write(MAGIC)
    port.flush()
    r = port.read(1)
    if not r or r[0] != ACK:
        raise IOError("设备没回 ACK（收到 %r）" % r)
    head = bytearray(512)
    struct.pack_into("<I", head, 0, len(data))
    port.write(bytes(head))
    port.write(data)
    port.write(struct.pack(">H", crc16_ccitt(data)))
    port.flush()
    return port.readline().decode("ascii", "replace").strip()


def send_cmd(port, ch):
    port.write(ch.encode())
    port.flush()
    return port.readline().decode("ascii", "replace").strip()


def render(model, pages, total, page_no, size, line_k, no_header, invert, path):
    p = pages[page_no - 1]
    hdr = None if no_header else "H"
    ftr = None if no_header else "F"
    L = layout(model, size, line_k, 12, header=hdr, footer=ftr)
    pct = (p["line"] + len(p["lines"])) * 100 // max(1, total)
    img = render_page(model, p["lines"], L,
                      header=None if no_header else
                      "%s  p%d/%d" % (os.path.basename(path), page_no, len(pages)),
                      footer=None if no_header else "%d%%" % pct)
    return image_to_bitmap(model, img, dither=False, invert=invert, fit=False)


def get_key():
    """读一个键：Windows 用 msvcrt，其他平台退回整行输入"""
    try:
        import msvcrt
    except ImportError:
        try:
            return sys.stdin.readline().strip()[:1].lower()
        except KeyboardInterrupt:
            return "q"
    while True:
        ch = msvcrt.getch()
        if ch in (b"\x00", b"\xe0"):          # 方向键：吃掉第二个字节
            ch2 = msvcrt.getch()
            return {b"M": "n", b"K": "p"}.get(ch2, "")
        if ch in (b"\r", b"\n"):
            return "n"
        try:
            return ch.decode("ascii").lower()
        except Exception:
            return ""


def main():
    ap = argparse.ArgumentParser(description="墨水屏连续阅读器")
    ap.add_argument("--book", required=True, help="txt 文件")
    ap.add_argument("--port", help="串口号；不填则只出图不推设备")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--model", choices=list(MODELS.keys()), default=DEFAULT_MODEL)
    ap.add_argument("--from", dest="start", type=int, default=1, help="起始页")
    ap.add_argument("--size", type=int, default=None, help="字号（默认自动）")
    ap.add_argument("--line-k", type=float, default=1.35)
    ap.add_argument("--no-header", action="store_true")
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--refresh-every", type=int, default=0,
                    help="仅当配合局刷使用时才需要；整屏推图模式默认 0（不需要）")
    ap.add_argument("--render-only", action="store_true", help="只渲染成 PNG，不连设备")
    ap.add_argument("--pages", help="配合 --render-only，例如 1-3,7")
    args = ap.parse_args()

    model = MODELS[args.model]
    pages, total, fitted = book_pages(model, args.book, args.size, args.line_k, 12)
    print("屏 %s | 全书 %d 行 / %d 页 | 字号 %s" % (model, total, len(pages), fitted))

    if args.render_only:
        want = []
        if args.pages:
            for part in args.pages.split(","):
                if "-" in part:
                    a, b = part.split("-")
                    want += list(range(int(a), int(b) + 1))
                else:
                    want.append(int(part))
        else:
            want = [args.start]
        for n in want:
            if not 1 <= n <= len(pages):
                print("跳过越界页", n)
                continue
            bw = render(model, pages, total, n, fitted, args.line_k,
                        args.no_header, args.invert, args.book)
            out = "page_%04d.png" % n
            from push import bitmap_to_image
            bitmap_to_image(model, bw).convert("L").save(out)
            print("已生成", out)
        return

    if not args.port:
        sys.exit("要么给 --port，要么加 --render-only")

    try:
        import serial
    except ImportError:
        sys.exit("需要 pyserial:  python -m pip install pyserial")

    page = max(1, min(args.start, len(pages)))
    since_full = 0
    cache = {}

    with serial.Serial(args.port, args.baud, timeout=10.0) as port:
        print("已连上 %s，n=下一页 p=上一页 f=全刷 s=休眠 q=退出" % args.port)
        while True:
            if page not in cache:
                cache[page] = render(model, pages, total, page, fitted, args.line_k,
                                     args.no_header, args.invert, args.book)
            bw = cache[page]
            # 每 refresh_every 页插一次全刷：先推图（设备收到就全刷，不带局刷）
            r = send_frame(port, bw)
            print("第 %d/%d 页 -> %s" % (page, len(pages), r))

            k = get_key()
            if k == "q":
                break
            if k == "n":
                if page < len(pages):
                    page += 1
                    since_full += 1
            elif k == "p":
                if page > 1:
                    page -= 1
                    since_full += 1
            elif k == "f":
                print(" ", send_cmd(port, "F"))
                since_full = 0
                continue
            elif k == "s":
                print(" ", send_cmd(port, "S"))
                break
            else:
                continue

            if args.refresh_every and since_full >= args.refresh_every:
                print(" ", send_cmd(port, "F"), "(清残影)")
                since_full = 0
                cache.clear()

    print("退出")


if __name__ == "__main__":
    main()
