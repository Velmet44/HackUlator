#!/usr/bin/env python3
"""PowerKill's real success test: watch the VICTIM, not the attacker.

Why this exists
---------------
The ESP32's own RX counter (`v f/s`) is a relative figure with a weak link -
data frames are the hardest thing to capture (AMPDU plus only 6 static RX
buffers), and the victim network's load swings on its own. A drop from 8 f/s
to 0 is indistinguishable from a victim that simply went idle.

This probe measures the thing that actually matters and is unambiguous: what
is the victim's WiFi stack doing while the attack runs? Read from the laptop's
own adapter, so it is independent of anything the ESP32 claims about itself.

The metric is TIME ON TARGET - the fraction of samples where the client is
still associated with the target AP on 2.4 GHz. That is the one number that
survives every confound this setup throws at us:
  - the client may fall through to a DIFFERENT 2.4 GHz AP (M17E ch6 is a
    saved profile in this environment) - counted as off target, and reported
    separately as a move, because "disconnected" would overstate it;
  - the client may hop to the target's own 5 GHz cell - also off target, and
    reported separately because that is the AP steering, not a kill;
  - it may sit disconnected - off target, but the weakest outcome of all.
Only being ON the target counts as surviving.

Control run
-----------
Run with --control to drive the whole sequence but launch nothing. Disconnects
happen on their own - a router rebooting, band steering, the laptop sleeping.
Without this run, one observed disconnect proves nothing. The comparison of
time-on-target with and without the attack IS the result.

Re-arm
------
If 5 GHz is left on auto-connect, Windows hops to it the instant 2.4 GHz
drops, which looks like recovery. Run once in an Administrator shell:

    netsh wlan set profileparameter name="JioFiber5G" connectionmode=manual
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_test import Dev, POS  # noqa: E402

TARGET_SSID = os.environ.get("HK_TARGET_SSID", "JioFiber")
POLL = 0.4

# Attack mode index on the WiFi attacks screen. 4 = PowerKill, 5 = CSAonly.
# The two matter separately against a PMF client: deauth is expected to be
# ignored there, so CSAonly is the mode that can actually work.
MODE_POWERKILL = 4
MODE_CSAONLY = 5

# Cloudflare's __down endpoint silently returns a 1-byte body once you ask for
# more than ~20 MB, which looks like an instant download rather than an error.
# So the request size stays at 20 MB and the sustained load comes from
# repeating it - and all of them go to ONE curl process, because relaunching
# curl per chunk leaves ~100 ms gaps that flicker the meter.
LOAD_URL = "https://speed.cloudflare.com/__down?bytes=20000000"
LOAD_CHUNKS = 60  # 20 MB each ~= 1.2 GB, ~4 minutes at a measured 5.6 MB/s

UNKNOWN = ("?", "?", "?")


def start_load():
    args = ["curl.exe", "-s"]
    for _ in range(LOAD_CHUNKS):
        args += ["-o", "NUL", LOAD_URL]
    return subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def wifi_state():
    """(connected, ssid, band, channel) as Windows sees it right now.

    While disconnected, Windows reports blank SSID/Band/Channel, so the last
    known values have to be carried by the caller - an earlier version logged
    "was ch" because it read the blanks straight out of the mid-teardown
    interface state."""
    try:
        out = subprocess.run(["netsh", "wlan", "show", "interfaces"],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return None
    if "State" not in out:
        return None

    def grab(label):
        m = re.search(r"^\s*%s\s*:\s*(.*)$" % label, out, re.M)
        return m.group(1).strip() if m else ""

    connected = "connected" in grab("State").lower()
    return connected, grab("SSID"), grab("Band"), grab("Channel")


class VictimWatcher:
    """Samples the adapter; classifies every transition and tracks how much
    time the client actually spent on the target."""

    def __init__(self, target=TARGET_SSID, phased=True):
        self.target = target
        # CSAonly never escalates, so labelling its events "phase A/B" would
        # be meaningless - and a misleading label is worse than none.
        self.phased = phased
        self.t0 = time.time()
        self.t_launch = None    # set by mark_launch()
        self.last_known = None   # (ssid, band, channel), survives a drop
        self.state = None        # (connected, last_known) - what we compare
        self.events = []
        self.samples = 0
        self.on_target = 0
        self.dropped = 0
        self.moved = 0           # fell through to a different 2.4 AP
        self.band_hop = 0        # pushed to the target's own 5 GHz cell

    def poll(self):
        st = wifi_state()
        if st is None:
            return
        now = time.time() - self.t0
        connected, ssid, band, channel = st

        # netsh blanks SSID/Band/Channel while the interface is associating.
        # That is "no information yet", NOT a new state. An earlier version
        # fed those blanks in as a state and then reported the resulting
        # empty tuple as a 5 GHz band hop - which turned three ordinary
        # reconnects into phantom hops and nearly produced a false
        # mechanism for a real effect.
        if connected and not ssid:
            return

        if connected:
            self.last_known = (ssid, band, channel)
        self.samples += 1
        if connected and ssid == self.target and band == "2.4 GHz":
            self.on_target += 1

        state = (connected, self.last_known)
        if self.state is None:
            self.state = state
            return
        if state == self.state:
            return

        (pc, pk), (nc, nk) = self.state, state
        if pc and not nc:
            self.dropped += 1
            self._note(now, "DROPPED", "was %s %s ch%s" % (pk or UNKNOWN))
        elif nc and not pc:
            if nk and nk[0] == self.target and nk[1] == "2.4 GHz":
                self._note(now, "BACK ON TARGET", "%s %s ch%s" % nk)
            elif nk and nk[1] != "2.4 GHz":
                self.band_hop += 1
                self._note(now, "BAND HOP", "-> %s %s ch%s" % nk)
            else:
                self.moved += 1
                self._note(now, "MOVED TO OTHER AP",
                           "-> %s %s ch%s" % (nk or UNKNOWN))
        else:
            self._note(now, "CHANGED", "-> %s %s ch%s" % (nk or UNKNOWN))
        self.state = state

    def mark_launch(self):
        """Stamp the attack start so every event can be reported relative to
        it. Phase A of PowerKill (the TPC/CSA beacon alone) runs for the
        first 20 s, phase B adds deauth + disassoc after that - so an event
        at T+5s is evidence for CSA and one at T+40s is not. Without this
        the two cannot be told apart."""
        self.t_launch = time.time() - self.t0
        print("  (launch stamped at T+%.1fs; phase A runs 0-20s, "
              "phase B after)" % self.t_launch, flush=True)

    def _since_label(self, t):
        if self.t_launch is None:
            return ""
        since = t - self.t_launch
        if not self.phased:
            return "  [T+%.1fs into attack]" % since
        return "  [T+%.1fs into attack, %s]" % (
            since, "phase A" if since < 20 else "phase B")

    def _note(self, t, kind, detail):
        self.events.append((t, kind, detail))
        print("  T+%6.1fs  %-18s %s%s"
              % (t, kind, detail, self._since_label(t)), flush=True)

    def report(self, label):
        pct = 100.0 * self.on_target / self.samples if self.samples else 0.0
        # netsh costs ~1 s per call, so the real sampling interval is far
        # coarser than POLL and the sample count is well below
        # duration/POLL. Say so rather than implying 0.4 s resolution.
        span = (time.time() - self.t0)
        rate = span / self.samples if self.samples else 0
        print("\n== %s ==" % label)
        for t, kind, detail in self.events:
            print("  T+%6.1fs  %-18s %s%s" % (t, kind, detail,
                                             self._since_label(t)))
        if not self.events:
            print("  (no link change at all)")
        print("  time on target : %.1f%%  (%d/%d samples)"
              % (pct, self.on_target, self.samples))
        print("  sampling       : ~%.2fs/sample (netsh is the bottleneck, "
              "not POLS)" % rate)
        print("  drops          : %d" % self.dropped)
        print("  moved to other : %d" % self.moved)
        print("  band hops 5G   : %d" % self.band_hop)
        return pct


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    control = "--control" in sys.argv
    load = "--load" in sys.argv
    port = args[0] if args else "COM5"
    run_s = int(args[1]) if len(args) > 1 else 60
    # --mode N picks the attack; --ssid NAME overrides the target.
    mode = MODE_POWERKILL
    if "--mode" in sys.argv:
        mode = int(sys.argv[sys.argv.index("--mode") + 1])
    ssid = TARGET_SSID
    if "--ssid" in sys.argv:
        ssid = sys.argv[sys.argv.index("--ssid") + 1]
    mode_name = "CSAonly" if mode == MODE_CSAONLY else "PowerKill"

    st = wifi_state()
    if not st or not st[0]:
        print("laptop is not on WiFi - connect first", file=sys.stderr)
        return 2
    if st[1] != ssid or st[2] != "2.4 GHz":
        print("laptop is on %s / %s, not %s on 2.4 GHz. Reconnect:\n"
              '  netsh wlan connect name="%s"'
              % (st[1] or "?", st[2] or "?", ssid, ssid),
              file=sys.stderr)
        return 2
    print("victim: %s %s ch%s   mode=%s/%s load=%s"
          % (st[1], st[2], st[3], "CONTROL" if control else "ATTACK",
             mode_name, "on" if load else "off"), flush=True)

    d = Dev(port, 460800)
    w = VictimWatcher(target=ssid, phased=(mode == MODE_POWERKILL))

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

    print("== unlock ==", flush=True)
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
    d.key("d")
    d.pump(1.0)

    print("== WiFi attacks -> %s (mode %d) ==" % (mode_name, mode), flush=True)
    d.key("b")
    d.pump(0.6)
    d.key("b")
    d.pump(0.8)
    for _ in range(2):
        d.key("s")
    d.pump(0.4)
    d.key("e")
    d.pump(1.5)
    for _ in range(mode):
        d.key("s")
    d.pump(0.8)
    shot = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "test_out", "victim_%s_mode.png" % mode_name)
    d.shot(shot)
    print("  armed: %s" % shot, flush=True)

    dl = None
    if load:
        # Start the load BEFORE the baseline sample so both are measured
        # under identical conditions; a baseline taken on an idle link is
        # not comparable to an attack taken under load.
        dl = start_load()
        print("== load started (~5.6 MB/s downstream) ==", flush=True)
        d.pump(5.0)

    print("== baseline: %ds ==" % 10, flush=True)
    for _ in range(int(10 / POLL)):
        w.poll()
        d.pump(POLL)

    if control:
        print("== CONTROL: no attack, just watch for %ds ==" % run_s,
              flush=True)
        t0 = time.time()
        while time.time() - t0 < run_s:
            w.poll()
            d.pump(POLL)
    else:
        print("== LAUNCH %s, watching victim ==" % mode_name, flush=True)
        w.mark_launch()
        d.key("e")
        t0 = time.time()
        while time.time() - t0 < run_s:
            w.poll()
            d.pump(POLL)
        print("== stop ==", flush=True)
        d.key("e")
        d.pump(1.5)
        d.key("b")
        d.pump(1.0)

    for _ in range(int(6 / POLL)):
        w.poll()
        d.pump(POLL)

    if dl:
        dl.terminate()
        print("== load stopped ==", flush=True)

    pct = w.report("%s (%s)" % ("CONTROL" if control else "ATTACK", mode_name))
    if control:
        print("\nSave this number. The attack run's time-on-target must be "
              "compared against it - a single disconnect means nothing "
              "without it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())