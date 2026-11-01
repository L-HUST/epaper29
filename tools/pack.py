#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pack.py -- txt / epub -> .epg 页包（2.9 寸 296x128，1bpp）

页包结构见 docs/10-架构与接口.md：
  128B 头 + 每页 8B 页表 + 页数据（PackBits 可选压缩）

用法：
    python tools/pack.py --book tools/sample_book.txt --title "样书"
    python tools/pack.py --book novel.txt --title "书名" --out firmware/data/books/demo.epg
    python tools/pack.py --book novel.txt --no-compress

默认输出：firmware/data/books/demo.epg（给 pio uploadfs 用）
"""
import argparse
import os
import posixpath
import re
import struct
import sys
import tempfile
import zipfile
import zlib
from html.parser import HTMLParser
from multiprocessing import Pool, cpu_count
from urllib.parse import unquote
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from push import (MODELS, book_pages, layout, render_page, image_to_bitmap,
                  crc16_ccitt)

MODEL = MODELS["29"]
PAGE_BYTES = MODEL.frame_len       # 4736
HEADER_SIZE = 128
TABLE_ENTRY = 8


def packbits(data):
    """PackBits: 0..127 -> literal n+1; 129..255 -> repeat 257-n; 128 = no-op."""
    out = bytearray()
    n = len(data)
    i = 0
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 3:
            out.append(257 - run)
            out.append(data[i])
            i += run
            continue

        start = i
        i += run
        while i < n and i - start < 128:
            run = 1
            while i + run < n and data[i + run] == data[i] and run < 128:
                run += 1
            if run >= 3:
                break
            if i - start + run > 128:
                break
            i += run
        lit = data[start:i]
        out.append(len(lit) - 1)
        out += lit
    return bytes(out)


def _local(tag):
    return tag.rsplit("}", 1)[-1]


class _EpubText(HTMLParser):
    """HTML/XHTML -> 纯文本；保留段落和换行，丢掉 head/script/style。"""

    BLOCK = {"p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6",
             "li", "tr", "section", "article", "blockquote", "pre"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.skip = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("head", "script", "style"):
            self.skip += 1
        elif not self.skip and tag in self.BLOCK:
            self.parts.append("\n")

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_endtag(self, tag):
        if tag in ("head", "script", "style"):
            if self.skip:
                self.skip -= 1
        elif not self.skip and tag in self.BLOCK:
            self.parts.append("\n")

    def handle_data(self, data):
        if not self.skip:
            self.parts.append(data)


def epub_to_text(path):
    """解析 EPUB 的 OPF spine，按顺序提取正文。返回 (text, title, author)。"""
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        raise SystemExit("EPUB 打不开: %s" % e)

    with z:
        names = set(z.namelist())
        if "META-INF/encryption.xml" in names:
            raise SystemExit("这个 EPUB 带 DRM/加密，暂不支持")

        try:
            container = ET.fromstring(z.read("META-INF/container.xml"))
            rootfile = None
            for el in container.iter():
                if _local(el.tag) == "rootfile":
                    rootfile = el.attrib.get("full-path")
                    break
            if not rootfile:
                raise ValueError("没有 rootfile")
            opf = ET.fromstring(z.read(rootfile))
        except Exception as e:
            raise SystemExit("EPUB 结构损坏: %s" % e)

        title = ""
        author = ""
        manifest = {}
        spine = []
        for el in opf.iter():
            tag = _local(el.tag)
            if tag == "title" and not title:
                title = (el.text or "").strip()
            elif tag == "creator" and not author:
                author = (el.text or "").strip()
            elif tag == "item":
                href = el.attrib.get("href", "")
                manifest[el.attrib.get("id", "")] = href
            elif tag == "itemref":
                if el.attrib.get("linear", "yes") != "no":
                    spine.append(el.attrib.get("idref", ""))

        base = posixpath.dirname(rootfile)
        hrefs = []
        for idref in spine:
            href = manifest.get(idref)
            if href:
                hrefs.append(posixpath.normpath(
                    posixpath.join(base, unquote(href))))
        if not hrefs:
            hrefs = [posixpath.normpath(posixpath.join(base, unquote(h)))
                     for h in manifest.values()
                     if h.lower().endswith((".xhtml", ".html", ".htm"))]

        parts = []
        for href in hrefs:
            if href not in names:
                continue
            raw = z.read(href)
            try:
                doc = raw.decode("utf-8-sig")
            except UnicodeDecodeError:
                doc = raw.decode("utf-8", "replace")
            p = _EpubText()
            p.feed(doc)
            txt = "".join(p.parts)
            txt = txt.replace("\xa0", " ")
            txt = re.sub(r"[ \t\f\v]+", " ", txt)
            txt = re.sub(r" *\n *", "\n", txt)
            txt = re.sub(r"\n{3,}", "\n\n", txt).strip()
            if txt:
                parts.append(txt)

        text = "\n\n".join(parts).strip()
        if not text:
            raise SystemExit("EPUB 里没有提取到正文")
        return text, title, author


def _init_render(model, L, short_title, total, codec):
    global _R_MODEL, _R_L, _R_TITLE, _R_TOTAL, _R_CODEC
    _R_MODEL, _R_L, _R_TITLE, _R_TOTAL, _R_CODEC = (
        model, L, short_title, total, codec)


def _render_one(task):
    i, lines = task
    img = render_page(_R_MODEL, lines, _R_L,
                      header="%s  p%d/%d" % (_R_TITLE, i, _R_TOTAL))
    fr = image_to_bitmap(_R_MODEL, img, dither=False, invert=False, fit=False)
    if len(fr) != PAGE_BYTES:
        raise ValueError("页长错误: %d" % len(fr))
    blob = packbits(fr) if _R_CODEC == "packbits" else fr
    return fr, blob


def build(book, title, author, out_path, compress=True, codec="packbits", jobs=0):
    is_epub = book.lower().endswith(".epub")
    tmp_txt = None
    if is_epub:
        print("  解析 EPUB...", flush=True)
        text, etitle, eauthor = epub_to_text(book)
        if not title:
            title = etitle
        if not author:
            author = eauthor
        fd, tmp_txt = tempfile.mkstemp(
            prefix=".epg_epub_", suffix=".txt",
            dir=os.path.dirname(os.path.abspath(book)))
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
            f.write("\n")
        pagesource = tmp_txt
    else:
        if not title:
            title = os.path.splitext(os.path.basename(book))[0]
        pagesource = book

    print("  正在分页/读取缓存...", flush=True)
    pages, total_lines, fitted = book_pages(MODEL, pagesource, None, 1.35, 12)
    if tmp_txt:
        for p in (tmp_txt, tmp_txt + ".pages.json"):
            try:
                os.remove(p)
            except OSError:
                pass
        tmp_txt = None
    if not pages:
        raise SystemExit("没有分页结果")
    if len(pages) > 100000:
        raise SystemExit("页数太多: %d" % len(pages))

    title = title or os.path.splitext(os.path.basename(book))[0]
    short_title = title[:10]
    L = layout(MODEL, fitted, 1.35, 12, header="H", footer=None)

    blobs = []
    frames = []
    step = max(1, len(pages) // 100)
    L2 = {k: v for k, v in L.items() if k != "font"}
    if not compress:
        codec = "none"
    njobs = jobs if jobs and jobs > 0 else min(8, max(1, cpu_count()))
    if njobs > 1 and len(pages) >= 64:
        jobs_list = [(i, page["lines"]) for i, page in enumerate(pages, 1)]
        with Pool(njobs, initializer=_init_render,
                  initargs=(MODEL, L2, short_title, len(pages), codec)) as pool:
            for i, (fr, blob) in enumerate(
                    pool.imap(_render_one, jobs_list, chunksize=16), 1):
                frames.append(fr)
                blobs.append(blob)
                if i == len(pages) or i % step == 0:
                    print("\r  排版 %d/%d (%d%%)" %
                          (i, len(pages), i * 100 // len(pages)),
                          end="", flush=True)
    else:
        for i, page in enumerate(pages, 1):
            lines = page["lines"]
            img = render_page(MODEL, lines, L,
                              header="%s  p%d/%d" % (short_title, i, len(pages)))
            fr = image_to_bitmap(MODEL, img, dither=False, invert=False, fit=False)
            if len(fr) != PAGE_BYTES:
                raise SystemExit("页长错误: %d" % len(fr))
            frames.append(fr)
            if codec == "packbits":
                blobs.append(packbits(fr))
            else:
                blobs.append(fr)
            if i == len(pages) or i % step == 0:
                print("\r  排版 %d/%d (%d%%)" %
                      (i, len(pages), i * 100 // len(pages)),
                      end="", flush=True)
    print()

    flags = 1 if codec == "packbits" else 0
    page_count = len(frames)
    data_offset = HEADER_SIZE + page_count * TABLE_ENTRY
    layout_hash = zlib.crc32(("epg1|29|%s|%d" % (fitted, int(1.35 * 100))).encode())
    source_crc = zlib.crc32(open(book, "rb").read()) & 0xFFFFFFFF

    h = bytearray(HEADER_SIZE)
    struct.pack_into("<4s6H6I", h, 0,
                     b"EPG1", 1, HEADER_SIZE,
                     MODEL.w, MODEL.h, MODEL.stride, flags,
                     PAGE_BYTES, page_count,
                     HEADER_SIZE, data_offset,
                     layout_hash & 0xFFFFFFFF, source_crc)
    tb = short_title.encode("utf-8")[:63]
    ab = author.encode("utf-8")[:19]
    h[40:40 + len(tb)] = tb
    h[104:104 + len(ab)] = ab

    table = bytearray(page_count * TABLE_ENTRY)
    off = data_offset
    raw_total = 0
    comp_total = 0
    for i, (fr, blob) in enumerate(zip(frames, blobs)):
        struct.pack_into("<IHH", table, i * TABLE_ENTRY, off, len(blob),
                         crc16_ccitt(fr))
        off += len(blob)
        raw_total += len(fr)
        comp_total += len(blob)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(h)
        f.write(table)
        for blob in blobs:
            f.write(blob)

    ratio = raw_total / float(comp_total) if comp_total else 0
    print("页包: %s" % out_path)
    print("  书名: %s | 页数: %d | 字号: %s | 行数: %d" %
          (short_title, page_count, fitted, total_lines))
    print("  头/页表/数据: %d / %d / %d 字节" %
          (HEADER_SIZE, len(table), comp_total))
    print("  原始 %.1f KB -> 存储 %.1f KB，压缩比 %.2fx，flags=%d" %
          (raw_total / 1024.0, comp_total / 1024.0, ratio, flags))
    return page_count


def main():
    ap = argparse.ArgumentParser(description="txt / epub -> .epg 页包")
    ap.add_argument("--book", required=True, help="输入 .txt 或 .epub")
    ap.add_argument("--title", default=None, help="书名，默认取文件名/EPUB 元数据")
    ap.add_argument("--author", default="", help="作者（最多 19 字节）")
    ap.add_argument("--out", default=os.path.join(ROOT, "firmware", "data",
                                                  "books", "demo.epg"))
    ap.add_argument("--no-compress", action="store_true",
                    help="不压缩（默认 PackBits）")
    ap.add_argument("--jobs", type=int, default=0,
                    help="并行渲染进程数；0=自动（最多 8）")
    a = ap.parse_args()

    codec = "none" if a.no_compress else "packbits"
    build(a.book, a.title, a.author, a.out,
          compress=(codec != "none"), codec=codec, jobs=a.jobs)


if __name__ == "__main__":
    main()
