#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
upload.py -- 通过串口把 .epg 页包传到设备内部 LittleFS

用法：
    python tools/upload.py --file firmware/data/books/demo.epg --port COM4
    python tools/upload.py --file novel.epg --name novel.epg --port COM4

协议（固件 main.cpp）：
    U|/books/<name>|<size>|<crc16 hex>\n
    <- READY ...
    <binary size bytes>
    <- OK upload ... / ERR ...
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from push import crc16_ccitt, wait_ready


def safe_print(text):
    try:
        print(text)
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "utf-8"
        print(text.encode(enc, "replace").decode(enc, "replace"))


def main():
    ap = argparse.ArgumentParser(description="串口上传 .epg 到设备")
    ap.add_argument("--file", required=True, help="本地 .epg")
    ap.add_argument("--port", required=True, help="COM4 / /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--name", help="设备上的文件名，默认用本地文件名")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    if not data:
        sys.exit("文件为空")
    name = args.name or os.path.basename(args.file)
    if not name.endswith(".epg"):
        name += ".epg"
    if "/" in name or "\\" in name or ".." in name or len(name.encode("utf-8")) > 48:
        sys.exit("文件名不合法: %s" % name)

    try:
        import serial
    except ImportError:
        sys.exit("需要 pyserial:  python -m pip install pyserial")

    path = "/books/" + name
    crc = crc16_ccitt(data)
    print("上传 %s -> %s  %d 字节  CRC=%04X" %
          (args.file, path, len(data), crc))

    with serial.Serial(args.port, args.baud, timeout=10.0) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()
        if wait_ready(ser):
            print("设备已就绪")
        else:
            print("没等到 READY，继续尝试")

        ser.write(("U|%s|%d|%04X\n" % (path, len(data), crc)).encode("utf-8"))
        ser.flush()

        line = ser.readline().decode("utf-8", "replace").strip()
        while line and not (line.startswith("READY") or line.startswith("ERR")):
            safe_print("  " + line)
            line = ser.readline().decode("utf-8", "replace").strip()
        if not line.startswith("READY"):
            sys.exit("设备拒绝: %s" % (line or "无回复"))
        print("设备接收中...")

        sent = 0
        t0 = time.time()
        last_pct = -1
        while sent < len(data):
            chunk = data[sent:sent + 4096]
            ser.write(chunk)
            debug = b""
            while True:
                ack = ser.read(1)
                if not ack:
                    sys.exit("块确认超时 at %d" % sent)
                if ack == b"\x06":
                    break
                debug += ack
                if ack in (b"\n", b"\r"):
                    txt = debug.decode("utf-8", "replace").strip()
                    if txt:
                        safe_print("  " + txt)
                    if txt.startswith("ERR"):
                        sys.exit("上传失败: %s" % txt)
                    debug = b""
            sent += len(chunk)
            pct = int(sent * 100 / len(data))
            if pct >= last_pct + 5 or sent == len(data):
                last_pct = pct
                print("\r上传 %3d%%  %.2f / %.2f MB" %
                      (pct, sent / 1048576.0, len(data) / 1048576.0),
                      end="", flush=True)
        print()

        deadline = time.time() + 40.0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            safe_print("  " + line)
            if line.startswith("OK upload"):
                print("完成，用时 %.1f 秒" % (time.time() - t0))
                return
            if line.startswith("ERR"):
                sys.exit("上传失败: %s" % line)
        sys.exit("等待设备确认超时")


if __name__ == "__main__":
    main()
