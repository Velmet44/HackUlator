#!/usr/bin/env python3
"""Cross-check the RX monitor's counters against an independent reference.

The device's own scan is the reference: it enumerates APs and their primary
channel. A real AP beacons ~10x/s (100 TU). So on the channel the monitor is
parked on:

    expected mgmt frames/s  ~  (APs on that channel) * 10

Comparing observed vs expected gives a capture ratio - the honest answer to
"is my counter lying to me". It also gives the user a number they can watch
improve if they ever raise CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM.

Prints the screenshots and the arithmetic; the channel and the AP counts are
read off the images by the operator (or a human), so this is a
report-generating harness, not a closed assertion.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS, W, H  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                   "test_out")

BEACON_HZ = 10.0  # 100 TU = 102.4 ms, the near-universal default


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    settle = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    os.makedirs(OUT, exist_ok=True)
    d = Dev(port, 460800)

    def shot(name):
        p = os.path.join(OUT, name)
        d.shot(p)
        if os.path.exists(p):
            print("  shot ->", p)
        else:
            print("  shot FAILED:", p)

    def goto(label):
        cur = getattr(goto, "cur", [0, 0])
        tc, tr = POS[label]
        while cur[0] != tc:
            d.key("d" if tc > cur[0] else "a")
            cur[0] = (cur[0] + (1 if tc > cur[0] else -1)) % 4
        while cur[1] != tr:
            d.key("s" if tr > cur[1] else "w")
            cur[1] = (cur[1] + (1 if tr > cur[1] else -1)) % 5
        d.key("e")
        goto.cur = cur

    print("== boot + unlock ==")
    d.pump(3.0)
    goto("C")
    goto.cur = [0, 0]
    for ch in "4+6=":
        goto(ch)
    d.pump(0.6)
    shot("validate_01_unlocked.png")

    # Reference pass: the device's own scan. This enumerates APs; we then
    # need the per-channel split, so walk the list and read each detail.
    print("== scan (reference) ==")
    d.key("e")          # WiFi scan (item 1)
    d.pump(9.0)
    shot("validate_02_scan_list.png")

    # Read the channel of every listed AP via its detail page.
    print("== per-AP channels (detail pages) ==")
    for i in range(8):
        d.key("e")      # OK -> detail for the highlighted row
        d.pump(0.7)
        shot("validate_03_detail_%d.png" % i)
        d.key("b")      # back to list
        d.pump(0.4)
        d.key("s")      # next row
        d.pump(0.3)
    d.key("b")          # back to menu
    d.pump(0.6)

    # Measurement pass: RX monitor, hopping to the target channel.
    print("== RX monitor (measurement, %.0fs) ==" % settle)
    for _ in range(4):
        d.key("s")      # item 5 = WiFi passive
    d.pump(0.4)
    d.key("e")          # submenu
    d.pump(0.8)
    d.key("e")          # RX monitor
    d.pump(2.0)
    shot("validate_04_rx_ch1.png")

    target = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    if target:
        print("== hop to channel %d ==" % target)
        for _ in range(target - 1):
            # Remote keys: w=UP s=DOWN a=LEFT d=RIGHT. DOWN ('s') hops +1 ch.
            d.key("s")
            d.pump(0.35)
        d.pump(1.5)

    for k in range(3):
        d.pump(settle / 3.0)
        shot("validate_05_rx_t%d.png" % k)

    # OK toggles the beacon self-check panel.
    print("== self-check panel ==")
    d.key("e")
    d.pump(2.0)
    shot("validate_06_diag.png")
    d.pump(2.0)
    shot("validate_07_diag2.png")

    print("")
    print("=" * 62)
    print("Read these off the screenshots:")
    print("  validate_02_scan_list.png   -> how many APs, SSIDs")
    print("  validate_03_detail_N.png    -> each AP's CHANNEL field")
    print("  validate_05_rx_tN.png       -> the monitor's ch N and mgmt/s")
    print("")
    print("Then, for the channel the monitor sat on:")
    print("  expected mgmt/s = (APs on that channel) * %.0f" % BEACON_HZ)
    print("  capture ratio   = observed mgmt/s / expected mgmt/s")
    print("")
    print("  ~1.0  -> counters are trustworthy")
    print("  <<1.0 -> frames are being dropped; raise")
    print("           CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM in")
    print("           sdkconfig.defaults (currently 6) and re-measure.")
    print("=" * 62)
    return 0


if __name__ == "__main__":
    sys.exit(main())