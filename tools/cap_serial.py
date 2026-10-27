# -*- coding: utf-8 -*-
"""Reset the board over USB-CDC and capture the vendor demo's serial output."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 50
OUT = sys.argv[3] if len(sys.argv) > 3 else "vendor_demo_log.txt"

ser = serial.Serial(PORT, 115200, timeout=0.3)
ser.dtr = False
ser.rts = False
time.sleep(0.5)
ser.reset_input_buffer()

# hard-reset pulse for USB-CDC / USB-JTAG-Serial
ser.rts = True
time.sleep(0.15)
ser.rts = False
time.sleep(0.1)
ser.reset_input_buffer()

t0 = time.time()
lines = []
while time.time() - t0 < SECS:
    raw = ser.readline()
    if not raw:
        continue
    txt = raw.decode("utf-8", "replace").rstrip("\r\n")
    line = "[%6.2f] %s" % (time.time() - t0, txt)
    lines.append(line)
    print(line, flush=True)
ser.close()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print("---- captured %d lines -> %s" % (len(lines), OUT))
