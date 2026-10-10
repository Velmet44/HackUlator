#!/usr/bin/env python3
"""Drive HackUlator over the caster wire: unlock, open BLE attacks, pick a
name list, run the ADV flood, confirm the counter advances, stop it, confirm
it stops, and relock. Screenshots to test_advflood/. Exit 0 = flow OK.

Probes are lit-pixel counts in fixed layout bands (128x64 mono UI, 4x9
font): title y0-9, mode rows y10-45 (inverted selection), status y46-54,
counters y55-63.

What this CANNOT verify: whether the flood actually degrades anything nearby.
Bluedroid gives no per-packet feedback (see svc_ble_flood_est in
svc_ble.h), so the on-device counter is an elapsed/interval estimate, not a
capture. Proving effect needs a second receiver watching a victim's BLE
discovery. This test only proves the device-side flow.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from hk_test import Dev, POS, WHITE, BLACK, W, H  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), "..", "test_advflood")
STATUS_Y = 46   # status row set by ui_bleatk.c layout
STATS_Y = 55    # counters row (est / name list)


def count(d, y0, y1, x0=0, x1=W):
    return d.count_white(y0, y1, x0, x1)


def band(d, y):
    return bytes(d.fb[((y - 2) * W * 2):((y + 9) * W * 2)])


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    os.makedirs(OUT, exist_ok=True)
    fails = []

    def check(name, cond):
        print(("PASS " if cond else "FAIL ") + name, flush=True)
        if not cond:
            fails.append(name)

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

    def shot(n):
        d.shot(os.path.join(OUT, n))

    def at_menu():
        return d.has_bar()

    def at_bleatk():
        # header + an idle status row
        return count(d, 0, 9) >= 8 and count(d, STATUS_Y, STATUS_Y + 9) >= 8

    # 1. boot -> calculator
    d.pump(3.0)
    check("boots to calculator", d.px(127, 63) == BLACK)
    shot("1_boot.png")

    # 2. unlock 4+6=
    goto("C")
    cur = [0, 0]
    typ("4+6=")
    d.pump(0.5)
    check("unlocked (menu)", at_menu())
    shot("2_menu.png")

    # 3. BLE attacks is menu item 4: DOWN x3 from the top entry.
    for _ in range(3):
        d.key("s")
        d.pump(0.3)
    d.key("e")
    d.pump(2.0)
    check("BLE attacks screen", at_bleatk())
    shot("3_bleatk.png")

    # 4. OK opens the wordlist picker (the flood asks for a name list first,
    #    exactly like WiFi beacon spam asks for an SSID list).
    d.key("e")
    d.pump(1.0)
    check("name list picker opens", count(d, 0, 9) >= 6)
    shot("4_picker.png")

    # 5. move to GARBAGE and confirm -> flood starts
    d.key("s")
    d.pump(0.4)
    shot("5_picker_garbage.png")
    d.key("e")
    d.pump(2.5)
    check("flood running", at_bleatk())
    shot("6_running.png")

    # 6. the counters band must advance while it runs
    seen = set()
    for _ in range(12):
        d.pump(0.5)
        seen.add(band(d, STATS_Y))
    check("counter advances", len(seen) > 1)
    shot("7_counter.png")

    # 7. BACK stops the flood and returns to the menu
    d.key("b")
    d.pump(1.2)
    check("back at menu after exit", at_menu())
    shot("8_menu_after.png")

    # 8. re-enter to prove the counter is frozen, not still counting
    for _ in range(3):
        d.key("s")
        d.pump(0.3)
    d.key("e")
    d.pump(2.0)
    d.key("e")      # picker
    d.pump(1.0)
    d.key("e")      # start
    d.pump(2.5)
    frozen = band(d, STATS_Y)
    d.pump(1.5)
    check("counter advanced again on rerun", band(d, STATS_Y) != frozen)
    shot("9_rerun.png")

    # 9. relock must tear the radio down (svc_ble_stop stops advertising)
    d.key("b")
    d.pump(0.8)
    d.key("b"); d.key("b")
    d.pump(1.0)
    check("relocked to calculator", d.px(127, 63) == BLACK)
    shot("10_relocked.png")

    print("RESULT: " + ("ALL PASS" if not fails else "FAILURES: %s" % fails))
    print("NOTE: device-side flow only. Effect on nearby BLE devices needs a "
          "second receiver - see this file's docstring.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())