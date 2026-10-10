#!/usr/bin/env python3
"""PowerKill end-to-end check: select mode 5, arm the meter, run, watch the
phase escalate from A to B and the victim traffic fall.

The interesting number is `v f/s` before vs during: if PowerKill works, the
target's traffic drops. Everything else (phase tag, counters) only proves
frames went out.
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

    print("== scan + arm target ==", flush=True)
    d.key("e")
    d.pump(14.0)
    d.key("e")
    d.pump(1.0)
    d.key("d")          # RIGHT = arm target
    d.pump(1.0)
    shot("pk_00_armed.png")
    d.key("b")
    d.pump(0.6)
    d.key("b")
    d.pump(0.8)

    print("== WiFi attacks, select mode 5 (PowerKill) ==", flush=True)
    for _ in range(2):
        d.key("s")
    d.pump(0.4)
    d.key("e")
    d.pump(1.5)
    for _ in range(4):   # down to index 4 = PowerKill
        d.key("s")
    d.pump(0.8)
    shot("pk_01_mode_list.png")

    print("== RIGHT: measurement on (baseline before any attack) ==", flush=True)
    d.key("d")
    d.pump(6.0)
    shot("pk_02_baseline.png")

    print("== OK: launch PowerKill ==", flush=True)
    d.key("e")
    # The failure message shows for only ~1.2 s before redirecting to the
    # scan page, so sample tightly instead of walking past it.
    d.pump(1.0)
    shot("pk_03a_launch_1s.png")
    d.pump(0.8)
    shot("pk_03b_launch_2s.png")
    d.pump(1.2)
    shot("pk_03c_launch_3s.png")

    # The meter panel REPLACES the mode list, so while it is up the attack
    # counters band and the phase tag are hidden. Alternate RIGHT off/on so
    # each phase gets one shot of the TX counters and one of the traffic.
    def tx_shot(name):
        d.key("d")            # meter off -> counters band + phase tag
        d.pump(1.0)
        shot(name)
        d.key("d")            # meter back on
        d.pump(0.5)

    print("== phase A: TX counters ==", flush=True)
    d.pump(2.0)
    tx_shot("pk_04_phaseA_tx.png")
    print("== phase A: victim traffic ==", flush=True)
    d.pump(6.0)
    shot("pk_05_phaseA_traffic.png")
    print("== crossing 20s into phase B ==", flush=True)
    d.pump(11.0)
    tx_shot("pk_06_phaseB_tx.png")
    print("== phase B: victim traffic ==", flush=True)
    d.pump(7.0)
    shot("pk_07_phaseB_traffic.png")
    tx_shot("pk_08_late_tx.png")
    d.pump(6.0)
    shot("pk_09_late_traffic.png")

    print("== stop ==", flush=True)
    d.key("e")
    d.pump(1.5)
    shot("pk_10_stopped.png")
    d.key("b")
    d.pump(1.0)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())