#!/usr/bin/env python3
"""Screenshot the new surfaces: the 7-item menu, the attack list scroll, the
CSAonly mode, the RX monitor's third panel, and the channel survey.

The regression suite proves nothing broke; it does not prove new screens
render correctly. These are exactly the surfaces AGENTS.md warns about when
a list gains an entry, so they get looked at.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS  # noqa: E402

# Must track MENU_ITEMS in main.c - index 0..6.
MENU_N = 7
MENU_WIFI_SCAN = 0
MENU_WIFI_ATK = 2
MENU_WIFI_PASSIVE = 4

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test_out")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    os.makedirs(OUT, exist_ok=True)
    d = Dev(port, 460800)
    cur = [0, 0]

    def shot(n):
        p = os.path.join(OUT, n)
        d.shot(p)
        print("  shot ->", n, flush=True)

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
    d.pump(0.8)
    shot("nw_00_menu.png")          # 7 items - does it scroll to the last?

    print("== scroll menu to the bottom ==", flush=True)
    for _ in range(6):
        d.key("s")
    d.pump(0.5)
    shot("nw_01_menu_bottom.png")   # is item 7 actually reachable?

    print("== enter Channel survey (item 7) ==", flush=True)
    d.key("e")
    d.pump(1.2)
    shot("nw_02_survey_empty.png")
    print("== OK: run the 13-channel sweep ==", flush=True)
    d.key("e")
    d.pump(1.0)
    shot("nw_03_survey_scanning.png")
    d.pump(9.0)
    shot("nw_04_survey_done.png")
    d.key("b")
    d.pump(1.0)

    # Navigate by MENU index with DOWN presses, not by the calculator keypad
    # grid. The earlier version reused goto(), whose stray UP/DOWN presses
    # only reached the right screen by wrapping - it worked by accident.
    def menu_to(idx):
        # The cursor wraps, so walk up to index 0 first, then down to idx.
        for _ in range(MENU_N):
            d.key("w")
        for _ in range(idx):
            d.key("s")
        d.pump(0.4)

    print("== WiFi attacks (menu item 3) ==", flush=True)
    menu_to(2)
    d.key("e")
    d.pump(1.2)
    shot("nw_05_atk_top.png")
    print("== scroll attack list to the last mode (CSAonly) ==", flush=True)
    for _ in range(5):
        d.key("s")
    d.pump(0.6)
    shot("nw_06_atk_scrolled.png")  # mode 6 must be reachable
    d.key("b")
    d.pump(0.8)

    print("== WiFi passive -> RX monitor (menu item 5) ==", flush=True)
    menu_to(4)
    d.key("e")
    d.pump(1.0)
    shot("nw_07_passive_submenu.png")
    d.key("e")                      # open the RX monitor
    d.pump(2.0)
    shot("nw_08_rx_default.png")
    d.key("e")
    d.pump(0.5)
    shot("nw_09_rx_diag.png")
    d.key("e")
    d.pump(3.0)
    shot("nw_10_rx_live.png")       # airtime / clients / deauth
    d.key("b")
    d.pump(0.6)
    d.key("b")
    d.pump(0.8)
    shot("nw_11_done.png")
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())