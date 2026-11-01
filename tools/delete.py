#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""delete.py -- 删除设备 /books 下的文件。

用法：
    python tools/delete.py --name 心流.epg --port COM4
    python tools/delete.py --name /books/test.epg --port COM4
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from push import wait_ready


def safe_print(text):
    try:
        print(text)
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "utf-8"
        print(text.encode(enc, "replace").decode(enc, "replace"))


def main():
    ap = argparse.ArgumentParser(description="删除设备 /books 下的文件")
    ap.add_argument("--name", required=True, help="文件名，如 test.epg")
    ap.add_argument("--port", required=True, help="COM4 / /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    name = args.name.strip()
    if name.startswith("/books/"):
        path = name
    elif name.startswith("/"):
        sys.exit("只允许删除 /books 下的文件")
    else:
        path = "/books/" + name
    if ".." in path or "\\" in path or not path.endswith(".epg"):
        sys.exit("文件名不合法: %s" % path)

    import serial

    with serial.Serial(args.port, args.baud, timeout=10.0) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()
        wait_ready(ser)
        ser.write(("D|%s\n" % path).encode("utf-8"))
        ser.flush()

        deadline = time.time() + 10.0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            safe_print("  " + line)
            if line.startswith("OK delete"):
                return
            if line.startswith("ERR"):
                sys.exit("删除失败: %s" % line)
        sys.exit("等待设备确认超时")


if __name__ == "__main__":
    main()
