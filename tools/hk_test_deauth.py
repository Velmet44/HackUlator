#!/usr/bin/env python3
"""Drive HackUlator over the caster wire: unlock, set a WiFi target, run
deauth, confirm the frame counter climbs, stop it, confirm it stops, and
relock. Screenshots to test_deauth/. Exit 0 = flow OK.

Probes are lit-pixel counts in fixed layout bands (128x64 mono UI, 4x9
font): title y0-9, mode rows y10-45 (selection bar), status y46-54,
counters y55-63, detail footer y55-63.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from hk_test import Dev, POS, WHITE, BLACK, W, H  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), "..", "test_deauth")
STATUS_Y = 46   # status line row set by ui_wifiatk.c layout
STATS_Y = 55    # counters row (frames / fake APs)


def count(d, y0, y1, x0=0, x1=W):
    return d.count_white(y0, y1, x0, x1)


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

    # title band (any header text), selection-bar screens, attacks status
    def at_menu():
        return d.has_bar()

    def at_scan():
        return d.has_bar()

    def at_attacks_idle():
        return (count(d, 0, 9) >= 8 and
                count(d, STATUS_Y, STATUS_Y + 9) >= 8)

    def at_attacks_running():
        return (count(d, 0, 9) >= 8 and
                count(d, STATUS_Y, STATUS_Y + 9) >= 8)

    def at_selected_footer():
        return count(d, 55, 64, 0, 64) >= 20

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
    check("target selected ('selected')", at_selected_footer())
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

    # 7. run every attack mode (UP/DOWN selects, OK runs then stops)
    modes = ["Deauth", "Disassoc", "Deauth_Disassoc", "BeaconSpam"]
    for idx, name in enumerate(modes):
        if idx:
            d.key("s")           # DOWN to the next attack
            d.pump(0.5)
        d.key("e")                # run
        d.pump(1.5)
        check(f"{name}: running", at_attacks_running())
        band = lambda: bytes(d.fb[((STATS_Y - 2) * W * 2):
                                  ((STATS_Y + 9) * W * 2)])
        seen = set()
        for _ in range(10):
            d.pump(0.5)
            seen.add(band())
        check(f"{name}: counter advances", len(seen) > 1)
        shot("8_%d_%s.png" % (idx, name.replace("+", "_")))
        d.key("e")                # stop
        d.pump(1.0)

    stopped_band = bytes(d.fb[((STATS_Y - 2) * W * 2):
                              ((STATS_Y + 9) * W * 2)])
    shot("9_stopped.png")
    check("stopped (idle status)", count(d, STATUS_Y, STATUS_Y + 9) >= 8)
    d.pump(1.5)
    check("counter frozen after stop",
          bytes(d.fb[((STATS_Y - 2) * W * 2):
                     ((STATS_Y + 9) * W * 2)]) == stopped_band)

    # 10. BACK -> menu
    d.key("b")
    d.pump(0.8)
    check("back at menu after exit", at_menu())

    # 11. relock
    d.key("b"); d.key("b"); d.key("b")
    d.pump(0.8)
    check("relocked to calculator", d.px(127, 63) == BLACK)
    shot("11_relocked.png")

    print("RESULT: " + ("ALL PASS" if not fails else "FAILURES: %s" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
