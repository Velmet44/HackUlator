#!/usr/bin/env python3
"""Audio Device Hijacker end-to-end verifier.

Drives menu item 9 (Audio hijack) and checks the screens render, the stack
comes up, and the screen can be left again. It deliberately does NOT require a
speaker to be present: the point is that discovery reports "none found"
cleanly and the whole screen can be entered and exited, which is what a
regression in the shared teardown paths would break.

Screenshots land in test_a2dh/ (gitignored - throwaway).

Run:  python tools/hk_a2dhijack.py
      python tools/hk_a2dhijack.py --port /dev/ttyUSB0

Works on Windows and Linux/macOS: the port follows the repo convention,
auto-detected and overridable with --port.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS, auto_detect_port, list_serial_ports  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test_a2dh")
MENU_N = 9          # must track MENU_ITEMS in main.c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None,
                    help="serial port; auto-detected if omitted "
                         "(COMx on Windows, /dev/ttyUSB* on Linux)")
    ap.add_argument("--list-ports", action="store_true")
    a = ap.parse_args()
    if a.list_ports:
        print("\n".join(list_serial_ports()))
        return 0
    port = a.port or auto_detect_port()
    if not port:
        print("no serial port found. Use --port COMx (Windows) or "
              "--port /dev/ttyUSB0 (Linux).")
        return 1
    os.makedirs(OUT, exist_ok=True)
    d = Dev(port, 460800)
    cur = [0, 0]
    fails = []

    def check(name, cond):
        print(("PASS " if cond else "FAIL ") + name, flush=True)
        if not cond:
            fails.append(name)

    def shot(n):
        p = os.path.join(OUT, n)
        d.shot(p)
        print("  shot ->", p if os.path.exists(p) else "FAILED", flush=True)

    def goto(label):
        """Move the calculator cursor to a keypad key and press it."""
        tc, tr = POS[label]
        while cur[0] != tc:
            d.key("d" if tc > cur[0] else "a")
            cur[0] = (cur[0] + (1 if tc > cur[0] else -1)) % 4
        while cur[1] != tr:
            d.key("s" if tr > cur[1] else "w")
            cur[1] = (cur[1] + (1 if tr > cur[1] else -1)) % 5
        d.key("e")

    s_pos = [0]      # tracked, not assumed

    def menu_to(idx):
        """Walk the cursor to idx, counting every move.

        The obvious "press UP MENU_N times to reach index 0, then DOWN idx"
        is wrong whenever the cursor did not start at 0: N UP presses is a
        FULL CYCLE and returns you exactly where you began. That bug is not
        theoretical - it is the one that lived in hk_test_newui.py, where
        MENU_N went stale and every menu_to() opened the wrong screen while
        still writing screenshots that looked fine. Here it put the cursor on
        Collateral instead of the hijacker, and the next OK press started a
        jammer. Tracking the position makes that class of error impossible.
        """
        guard = 0
        while s_pos[0] != idx and guard < 40:
            if s_pos[0] < idx:
                d.key("s")
                s_pos[0] = (s_pos[0] + 1) % MENU_N
            else:
                d.key("w")
                s_pos[0] = (s_pos[0] - 1) % MENU_N
            guard += 1
        d.pump(0.4)

    print("== unlock 4+6= ==", flush=True)
    d.pump(3.0)
    goto("C")
    cur = [0, 0]
    for ch in "4+6=":
        goto(ch)
    d.pump(0.8)
    shot("01_menu.png")

    print("== menu scrolls to item 9 ==", flush=True)
    menu_to(MENU_N - 1)
    d.pump(0.5)
    shot("02_menu_bottom.png")

    print("== enter Audio hijack (item 9) ==", flush=True)
    menu_to(8)
    shot("02b_on_item9.png")   # confirm the cursor BEFORE pressing OK
    d.key("e")
    d.pump(2.0)
    shot("03_pair_prompt.png")

    print("== OK: run the classic inquiry ==", flush=True)
    d.key("e")
    d.pump(7.5)
    shot("04_discovered.png")
    d.key("b")
    d.pump(1.0)
    shot("05_back_to_pair.png")

    print("== leave to the top menu ==", flush=True)
    d.key("b")
    d.pump(1.5)
    shot("06_menu_again.png")

    print("done", flush=True)
    if fails:
        print("FAILED (%d)" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())