#!/usr/bin/env python3
"""Walk both scan detail views to the new activity fields and capture them.

WiFi: pages UP/DOWN to the page holding the two new fields (10 and 11).
BLE: pages to the new advert-count field.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test_out")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    os.makedirs(OUT, exist_ok=True)
    d = Dev(port, 460800)

    def shot(n):
        p = os.path.join(OUT, n)
        d.shot(p)
        print("  shot ->", p if os.path.exists(p) else "FAILED", flush=True)

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

    d.pump(3.0)
    goto("C")
    cur = [0, 0]
    for ch in "4+6=":
        goto(ch)
    d.pump(0.6)

    print("== WiFi scan (now includes the 2.5s sample) ==", flush=True)
    d.key("e")
    d.pump(14.0)   # scan + burst
    shot("act_01_scan.png")
    d.key("e")      # detail
    d.pump(1.0)
    shot("act_02_detail_p1.png")
    d.key("s")     # page 2
    d.pump(0.8)
    shot("act_03_detail_p2.png")
    d.key("s")     # page 3 - new activity fields
    d.pump(0.8)
    shot("act_04_detail_activity.png")
    d.pump(1.5)
    shot("act_05_detail_activity2.png")
    d.key("b")
    d.pump(0.6)
    d.key("b")
    d.pump(0.8)

    print("== BLE scan ==", flush=True)
    d.key("s")
    d.pump(0.4)
    d.key("e")
    d.pump(13.0)
    shot("act_06_ble.png")
    d.key("e")
    d.pump(1.0)
    shot("act_07_ble_detail1.png")
    d.key("s")
    d.pump(0.6)
    shot("act_08_ble_detail2.png")
    d.key("s")
    d.pump(0.6)
    shot("act_09_ble_detail3.png")
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())