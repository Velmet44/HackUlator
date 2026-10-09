#!/usr/bin/env python3
"""Drive HackUlator over the caster wire: unlock, set a WiFi target, run
deauth, confirm the frame counter climbs, stop it, confirm it stops, and
relock. Screenshots to test_deauth/. Exit 0 = flow OK.

Colour probes are region *counts* (>=N pixels of a colour inside a band),
not exact-pixel anchors: RLE frame stitching can leave stale pixels and a
single-pixel probe is brittle. The bands are derived from the fixed UI
layout (font 11x22 at scale 1): headings y=8, target line y=44,
status line y=136, footer y=296.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from hk_test import Dev, POS  # noqa: E402

# RGB565 values exactly as gfx.h's C_RGB computes them
# (tools/hk_palette.py prints the full palette).
ORANGE = 0xFC60   # headings / CALC_KEY_FN (C_ORANGE)
GREEN = 0x0640    # target line / "selected" (C_GREEN)
RED = 0xF800      # running status (C_RED)
DIM = 0x94D4      # status bar + BACK hint (CALC_DIM)
CALC_BG = 0x18C4  # calculator background
OUT = os.path.join(os.path.dirname(__file__), "..", "test_deauth")


def count(d, color, y0, y1, x0=0, x1=240):
    n = 0
    for y in range(max(0, y0), min(y1, 320)):
        base = y * 240 * 2
        for x in range(max(0, x0), min(x1, 240)):
            o = base + x * 2
            if (d.fb[o] | (d.fb[o + 1] << 8)) == color:
                n += 1
    return n


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

    # heading band (title text), target band, status band, footer band
    def at_menu():
        return count(d, ORANGE, 8, 30) >= 10

    def at_scan():
        return count(d, ORANGE, 8, 30) >= 10

    def at_attacks_idle():
        return (count(d, ORANGE, 8, 30) >= 10 and
                count(d, GREEN, 44, 66) >= 20 and
                count(d, DIM, 130, 160) >= 20)

    def at_attacks_running():
        return (count(d, ORANGE, 8, 30) >= 10 and
                count(d, RED, 130, 160) >= 10)

    def at_selected_footer():
        return count(d, GREEN, 296, 318, 140, 236) >= 30

    # 1. boot -> calculator
    d.pump(3.0)
    check("boots to calculator", d.px(239, 319) == CALC_BG)
    shot("1_boot.png")

    # 2. unlock 4+6=
    goto("C")
    cur = [0, 0]
    typ("4+6=")
    d.pump(0.5)
    check("unlocked (menu)", at_menu())
    shot("2_menu.png")

    # 3. WiFi scan (menu item 1)
    d.key("e")
    d.pump(9.0)
    check("wifi scan results", at_scan())
    shot("3_scan.png")

    # 4. detail -> select target (RIGHT)
    d.key("e")
    d.pump(0.8)
    shot("4_detail.png")
    d.key("d")            # RIGHT = select for attacks
    d.pump(0.8)
    check("target selected (green 'selected')", at_selected_footer())
    shot("5_selected.png")

    # 5. back to menu
    d.key("b"); d.pump(0.5)   # detail -> list
    d.key("b"); d.pump(0.5)   # list -> menu
    check("back at menu", at_menu())
    shot("6_menu_target.png")

    # 6. WiFi attacks (menu item 3)
    d.key("s"); d.pump(0.3)
    d.key("s"); d.pump(0.3)
    d.key("e")
    d.pump(12.0)   # entry does a verifying rescan (blocking ~2-3s)
    check("wifi attacks screen (target verified)", at_attacks_idle())
    shot("7_attacks.png")

    # 7. start deauth
    d.key("e")
    d.pump(1.5)
    check("deauth running (red status)", at_attacks_running())
    shot("8_running.png")

    # 8. frame counter advances. The tick repaints the status band every
    # ~500 ms; sample it repeatedly and require at least one distinct band.
    band = lambda: bytes(d.fb[(130 * 240 * 2):(160 * 240 * 2)])
    seen = set()
    for _ in range(14):
        d.pump(0.5)
        seen.add(band())
    check("frame counter advances", len(seen) > 1)
    shot("9_running_later.png")

    # 9. stop
    d.key("e")
    d.pump(1.0)
    stopped = bytes(d.fb)
    stopped_band = bytes(d.fb[(130 * 240 * 2):(160 * 240 * 2)])
    shot("10_stopped.png")
    check("stopped (idle status)", count(d, DIM, 130, 160) >= 20)
    d.pump(1.5)
    check("counter frozen after stop",
          bytes(d.fb[(130 * 240 * 2):(160 * 240 * 2)]) == stopped_band)

    # 10. BACK -> menu
    d.key("b")
    d.pump(0.8)
    check("back at menu after exit", at_menu())

    # 11. relock
    d.key("b"); d.key("b"); d.key("b")
    d.pump(0.8)
    check("relocked to calculator", d.px(239, 319) == CALC_BG)
    shot("11_relocked.png")

    print("RESULT: " + ("ALL PASS" if not fails else "FAILURES: %s" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())