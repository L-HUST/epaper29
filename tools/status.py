#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""status.py -- 查询设备内存、PSRAM、文件系统和 /books 文件列表。"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "tools"))

from push import wait_ready


def safe_print(text):
    try:
        print(text, end="")
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "utf-8"
        print(text.encode(enc, "replace").decode(enc, "replace"), end="")


def main():
    ap = argparse.ArgumentParser(description="查询墨水屏设备状态")
    ap.add_argument("--port", required=True, help="COM4 / /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    import serial

    with serial.Serial(args.port, args.baud, timeout=0.6) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()
        wait_ready(ser)

        for cmd in ("M", "L"):
            ser.reset_input_buffer()
            ser.write((cmd + "\n").encode("ascii"))
            ser.flush()
            time.sleep(0.8)
            data = ser.read_all().decode("utf-8", "replace")
            safe_print(data)
            if not data.endswith("\n"):
                print()


if __name__ == "__main__":
    main()
