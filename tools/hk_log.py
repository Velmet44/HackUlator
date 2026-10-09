#!/usr/bin/env python3
"""Capture one device boot log over UART0 @115200 (log UART). Usage:
python tools/hk_log.py COM5 [seconds]"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

ser = serial.Serial(port, 115200, timeout=0.1)
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setDTR(False)
ser.setRTS(False)

t0 = time.time()
buf = bytearray()
while time.time() - t0 < secs:
    chunk = ser.read(max(1, ser.in_waiting))
    if chunk:
        buf += chunk
ser.close()

clean = bytes(b for b in buf if b in (0x0A, 0x0D) or 0x20 <= b < 0x7F)
sys.stdout.write(clean.decode("ascii", "replace"))