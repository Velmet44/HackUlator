#!/usr/bin/env python3
"""Measure the caster wire: which rects are sent and when, while the device
sits on the WiFi attacks screen running deauth.
Usage: python tools/hk_wire.py COM5 [seconds]
Drives: unlock -> wifi scan -> select target -> wifi attacks -> start.
"""
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")
from hk_test import Dev, POS  # noqa: E402

SYNC = b"\x7A\xA5\xE1PKC"
port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0


def raw_rects(d, t0, label):
    """Parse any PKC frames sitting in the OS buffer right now."""
    out = []
    buf = bytes(d.ser.read(max(1, d.ser.in_waiting)))
    i = 0
    while True:
        j = buf.find(SYNC, i)
        if j < 0 or len(buf) < j + 15:
            break
        x, y, w, h = struct.unpack_from("<HHHH", buf, j + 6)
        # walk rows to find the true payload length (RLE or raw)
        total = 15
        ok = True
        for _ in range(h):
            if len(buf) < j + total + 2:
                ok = False
                break
            (elen,) = struct.unpack_from("<H", buf, j + total)
            total += 2 + elen
        if not ok:
            break
        out.append((round(time.time() - t0, 2), x, y, w, h, total))
        i = j + total
    return out


def main():
    d = Dev(port, 460800)
    cur = [0, 0]

    def goto(label):
        tc, tr = POS[label]
        while cur[0] != tc:
            d.key("d" if tc > cur[0] else "a")
            cur[0] = (cur[0] + (1 if tc > cur[0] else -1)) % 4
        while cur[1] != tr:
            d.key("s" if tr > cur[1] else "w")
            cur[1] = (cur[1] + (1 if tr > cur[1] else -1)) % 5
        d.key("e")

    def typ(s):
        for ch in s:
            goto(ch)

    d.pump(3.0)
    goto("C"); cur = [0, 0]
    typ("4+6=")
    d.pump(0.5)
    d.key("e"); d.pump(9.0)          # wifi scan
    d.key("e"); d.pump(0.8)          # detail
    d.key("d"); d.pump(0.8)          # select
    d.key("b"); d.pump(0.5)
    d.key("b"); d.pump(0.5)
    d.key("s"); d.pump(0.3); d.key("s"); d.pump(0.3)
    d.key("e"); d.pump(12.0)         # wifi attacks
    print("on attacks screen", flush=True)

    d.ser.reset_input_buffer()
    t0 = time.time()
    d.key("e")                        # start deauth
    print("sent OK (start), watching wire %.1fs" % secs, flush=True)
    while time.time() - t0 < secs:
        r = raw_rects(d, t0, "run")
        for t, x, y, w, h, n in r:
            print(f"  t={t:5.2f}s rect=({x},{y},{w},{h}) bytes={n}")
        time.sleep(0.05)
    print("done watching", flush=True)


if __name__ == "__main__":
    main()