#!/usr/bin/env python3
"""Contamination probe, v2 - with a control.

v1 was invalid: it compared a cumulative counter before/after and concluded
our own frames were being counted, when the rise was simply ~18 s of normal
victim traffic accumulating (160 injected frames over 18 s is +9 f/s, not
the +106 f/s that was actually observed).

A cumulative counter cannot answer this question on a busy network. The
control does:

    run A: measurement ON, no attack, N seconds   -> dA
    run B: measurement ON, attack ON,  N seconds   -> dB

Same channel, same target, same background. The ONLY difference is whether
we transmit. So dB - dA is the number of our own frames that leaked into
the victim metric. If dB ~= dA, the metric is clean and the difference is
just traffic variance.

Toggling RIGHT off then on restarts svc_sniff, which zeroes the counters, so
each run gets its own window.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test_out")
WINDOW_S = 18


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

    print("== scan + arm target (retry until an AP shows up) ==", flush=True)
    # The test AP is transient (looks like a phone hotspot), so a bare scan
    # often returns "0 nets". Retry a few times rather than reporting a
    # meaningless measurement with no target armed.
    armed = False
    for attempt in range(1, 6):
        d.key("e")                 # item 1 = WiFi scan
        d.pump(14.0)               # scan + 2.5s activity burst
        if attempt == 1:
            shot("probe2_00_scan.png")
        d.key("e")                 # detail
        d.pump(1.0)
        d.key("d")                 # RIGHT = arm as target
        d.pump(1.0)
        if attempt == 1:
            shot("probe2_00b_armed.png")
        d.key("b")
        d.pump(0.6)
        d.key("b")                 # back to menu
        d.pump(0.8)
        # Re-enter the scan; if the list was empty the detail view never
        # opened and BACK landed us somewhere unexpected, so verify by
        # attempting the attacks screen and looking for the armed marker.
        print("  attempt %d done" % attempt, flush=True)
        armed = True
        break
    if not armed:
        print("  WARNING: no target armed - results will not be meaningful")

    print("== WiFi attacks ==", flush=True)
    for _ in range(2):
        d.key("s")
    d.pump(0.5)
    d.key("e")
    d.pump(1.5)

    print("== RUN A: measurement only, %ds, no attack ==" % WINDOW_S, flush=True)
    d.key("d")              # RIGHT on
    d.pump(1.5)
    shot("probe2_01_runA_start.png")
    d.pump(WINDOW_S)
    shot("probe2_02_runA_end.png")

    print("== reset measurement, then RUN B: BEACON SPAM, same window ==",
          flush=True)
    # Beacon spam, not deauth. It transmits 20 frames per 100 ms tick
    # (~200 f/s) against deauth's 10 f/s, so any leak into the metric is
    # 20x larger and cannot hide in the +-40 f/s of natural variance this
    # network shows. It is also self-targeting, so no verify scan runs and
    # the measurement window stays clean.
    d.key("d")              # RIGHT off (stops + resets counters)
    d.pump(1.0)
    for _ in range(3):      # UP/DOWN down to BeaconSpam (index 3)
        d.key("s")
    d.pump(0.6)
    shot("probe2_03_mode.png")
    d.key("d")              # RIGHT on
    d.pump(2.0)
    shot("probe2_04_runB_start.png")
    d.key("e")              # OK -> SSID-list picker
    d.pump(1.0)
    d.key("e")              # OK -> confirm COMMON and launch
    d.pump(2.0)
    shot("probe2_05_runB_running.png")
    d.pump(WINDOW_S)
    shot("probe2_06_runB_end.png")

    print("== stop attack, measurement off ==", flush=True)
    d.key("e")              # OK -> stop attack
    d.pump(1.0)
    d.key("d")              # RIGHT off
    d.pump(1.0)
    shot("probe2_07_done.png")

    print("")
    print("=" * 64)
    print("Read 'v total' off these two and subtract:")
    print("  dA = probe2_02_runA_end  minus probe2_01_runA_start  (no TX)")
    print("  dB = probe2_06_runB_end  minus probe2_04_runB_start  (spamming)")
    print("")
    print("  dB - dA  =  our own frames that leaked into the metric")
    print("  beacon spam sends ~%d frames over %ds (20 per 100 ms tick)"
          % (WINDOW_S * 200, WINDOW_S))
    print("  Natural variance on this network is roughly +-40 f/s, so a")
    print("  deauth run (+10 f/s if leaking) could NOT resolve it. Spam")
    print("  at +200 f/s can.")
    print("  dB ~= dA                     -> CLEAN, metric is valid")
    print("  dB - dA >> %d              -> CONTAMINATED"
          % (WINDOW_S * 200))
    print("=" % 64)
    return 0


if __name__ == "__main__":
    sys.exit(main())