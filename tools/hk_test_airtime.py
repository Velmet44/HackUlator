#!/usr/bin/env python3
"""Check the airtime ESTIMATE against arithmetic, on purpose rather than by eye.

The point of this is that `svc_sniff_airtime_permille()` is a model, not a
measurement: it charges data frames at a nominal 54 Mbit/s and management at
6 Mbit/s and ignores preamble, inter-frame spacing and contention. A model
that has never been compared against a load it should predict is not a
metric, it is a number that looks like one.

So: drive a known download onto a channel, let the monitor run a fixed
window, then predict what the estimate SHOULD read from the bytes actually
observed, and compare. If it is systematically off, the fix is a better rate
model - not a fudge factor bolted on to make the number agree.

Run:  python tools/hk_test_airtime.py COMx [secs]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev  # noqa: E402

# Must match svc_sniff_airtime_permille()'s model in svc_sniff.c.
DATA_MBIT = 54.0
MGMT_MBIT = 6.0

LOAD_URL = "https://speed.cloudflare.com/__down?bytes=20000000"
# 20 MB is the largest size Cloudflare actually serves - above that it returns
# a 1-byte body instead of erroring, which looks like an instant download.
# So the load is 60 repeats of the max, ~1.2 GB, comfortably longer than any
# window. Two chunks ran out in 7 s and left the rest of the window empty.
LOAD_CHUNKS = 60


def wifi_state():
    import re
    try:
        out = subprocess.run(["netsh", "wlan", "show", "interfaces"],
                             capture_output=True, text=True, timeout=8).stdout
    except Exception:
        return None
    m = re.search(r"^\s*SSID\s*:\s*(.*)$", out, re.M)
    b = re.search(r"^\s*Band\s*:\s*(.*)$", out, re.M)
    if not m or not b:
        return None
    return m.group(1).strip(), b.group(1).strip()


def adapter_bytes():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "(Get-NetAdapterStatistics -Name 'Wi-Fi').ReceivedBytes"],
                       capture_output=True, text=True, timeout=15)
    try:
        return int(r.stdout.strip())
    except ValueError:
        return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 12

    st = wifi_state()
    if not st:
        print("cannot read adapter state", file=sys.stderr)
        return 2
    print("victim: %s %s" % st, flush=True)

    dl = subprocess.Popen(
        ["curl.exe", "-s"] + ["-o", "NUL", LOAD_URL] * LOAD_CHUNKS,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("load running (~5.6 MB/s), settling 3s", flush=True)
    time.sleep(3)

    d = Dev(port, 460800)

    # Enter: unlock -> WiFi passive -> RX monitor
    from hk_test import POS
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
    d.pump(0.8)

    def menu_to(idx):
        for _ in range(7):
            d.key("w")
        for _ in range(idx):
            d.key("s")
        d.pump(0.4)

    menu_to(4)                    # WiFi passive
    d.key("e")
    d.pump(1.0)
    d.key("e")                    # open RX monitor
    d.pump(1.5)
    print("monitor up, running %ds window" % secs, flush=True)

    # Do NOT hop channels here. An earlier version pressed DOWN three times
    # and landed on ch4 - which the survey had already shown to be flat - so
    # the monitor measured an empty band and reported 0 KB. That is a correct
    # reading of nothing, and it says nothing at all about the model. The
    # monitor defaults to ch1, which is where the traffic is.

    t0 = time.time()
    pc0 = adapter_bytes()
    while time.time() - t0 < secs:
        d.pump(0.5)
    elapsed = time.time() - t0
    pc1 = adapter_bytes()

    # Third panel: airtime / clients / deauth
    d.key("e")
    d.pump(0.4)
    d.key("e")
    d.pump(1.0)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "test_out", "air_live.png")
    d.shot(out)
    print("shot -> %s" % out, flush=True)

    d.key("b")
    d.pump(0.6)
    d.key("b")
    d.pump(0.8)
    dl.terminate()

    # What the estimate SHOULD read, given the bytes the PC really moved.
    if pc0 is None or pc1 is None:
        print("could not read adapter counters - manual reading required")
        return 1
    pc_bytes = pc1 - pc0
    pc_mbit_s = pc_bytes * 8 / elapsed / 1e6

    # The ESP32 sees only a fraction of those bytes (6 static RX buffers, and
    # this is its own measured capture - not the wire truth). What we CAN
    # check is that its figure is self-consistent: it must lie between the
    # airtime of what it received and the airtime of the whole link.
    predicted_full = pc_mbit_s / DATA_MBIT * 100.0
    print("\n== arithmetic check ==")
    print("  window            : %.1f s" % elapsed)
    print("  PC received       : %.2f MB  (%.1f Mbit/s)"
          % (pc_bytes / 1e6, pc_mbit_s))
    print("  full-link airtime : %.0f%%  <- ceiling, the ESP32 cannot exceed"
          " this" % predicted_full)
    print("  ESP32 estimate    : read it off the screenshot above")
    print("\nA value near 0%% means the monitor caught almost nothing.")
    print("A value near or above the ceiling means the model over-charges.")
    print("Either way it is an ESTIMATE - the exact figures on the default")
    print("panel (bytes, frames/s) are the trustworthy ones.")
    return 0


if __name__ == "__main__":
    sys.exit(main())