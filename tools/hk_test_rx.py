#!/usr/bin/env python3
"""Verify the passive RX monitor: unlock, open it, prove counters climb.

Reuses hk_test.py's caster decoder and keypad map. The acceptance test is
that the on-screen counters MOVE while sitting near traffic - a static
screen would mean promiscuous RX never engaged.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS, W, H  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                   "test_out")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    os.makedirs(OUT, exist_ok=True)
    d = Dev(port, 460800)
    fails = []

    def shot(name):
        p = os.path.join(OUT, name)
        d.shot(p)
        if os.path.exists(p):
            print("  shot ->", p, flush=True)
        else:
            print("  shot FAILED (no Pillow?):", p, flush=True)

    def check(name, cond):
        print(("  PASS " if cond else "  FAIL ") + name, flush=True)
        if not cond:
            fails.append(name)

    def fingerprint():
        """Whole-screen pixel signature: changes when anything redraws."""
        h = 0
        for y in range(0, H, 2):
            for x in range(0, W, 2):
                h = (h * 31 + (1 if d.px(x, y) else 0)) & 0xFFFFFFFF
        return h

    print("== unlock ==", flush=True)
    d.pump(3.0)
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

    goto("C")
    cur = [0, 0]
    for ch in "4+6=":
        goto(ch)
    d.pump(0.5)
    check("unlocked to menu", d.has_bar())
    shot("rx_00_menu.png")

    print("== navigate to WiFi passive (item 5) ==", flush=True)
    for _ in range(4):
        d.key("s")
    d.pump(0.4)
    shot("rx_01_selected.png")
    check("menu reached item 5", d.has_bar())

    print("== open WiFi passive submenu ==", flush=True)
    d.key("e")
    d.pump(1.0)
    check("submenu opened", d.has_bar())
    shot("rx_02_submenu.png")

    print("== enter RX monitor ==", flush=True)
    d.key("e")
    d.pump(2.0)
    shot("rx_03_enter.png")

    # The device draws the screen once on entry, then repaints the counter
    # block every 500 ms. Sample twice: a live counter must differ.
    print("== counters must move ==", flush=True)
    seen = set()
    for i in range(6):
        d.pump(1.2)
        fp = fingerprint()
        seen.add(fp)
        print("   sample %d sig=%08x" % (i, fp), flush=True)
        if i in (1, 3):
            shot("rx_04_live_%d.png" % i)
    check("screen repaints repeatedly (%d/%d distinct)"
          % (len(seen), 6), len(seen) >= 2)

    print("== BACK returns to WiFi passive submenu ==", flush=True)
    d.key("b")
    d.pump(1.0)
    check("back to submenu", d.has_bar())
    shot("rx_05_submenu_back.png")

    print("== BACK again returns to top menu ==", flush=True)
    d.key("b")
    d.pump(1.0)
    check("back to menu", d.has_bar())
    shot("rx_06_menu.png")

    print("== BLE passive (item 6, needs menu scroll) ==", flush=True)
    # show_menu() resets the cursor to item 0 on every menu entry, so count
    # DOWN presses from zero. Item 6 is off-window, which also exercises the
    # scrolling list.
    for _ in range(5):
        d.key("s")
    d.pump(0.4)
    shot("rx_07_menu_scrolled.png")
    d.key("e")          # BLE passive submenu (does NOT start scanning)
    d.pump(1.0)
    check("BLE passive submenu", d.has_bar())
    shot("rx_08_blepassive_menu.png")
    d.key("e")          # Adv monitor
    d.pump(4.0)
    shot("rx_09_bleadv.png")
    d.pump(2.5)
    shot("rx_10_bleadv_live.png")
    d.key("b")
    d.pump(1.5)
    check("back to BLE submenu", d.has_bar())
    d.key("b")
    d.pump(1.5)
    check("back to menu", d.has_bar())

    print("")
    if fails:
        print("RESULT: FAILURES: %s" % fails)
        return 1
    print("RESULT: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())