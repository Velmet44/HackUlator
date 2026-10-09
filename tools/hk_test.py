#!/usr/bin/env python3
"""HACKULATOR headless verifier: drives the device over the caster wire
(UART0 @ 460800 baud, PKC protocol), sends remote keys, saves screenshots,
and asserts screen state by lit-pixel counts (128x64 mono UI: white text
bars on black; a full-width inverted selection bar marks menu/list rows).

Checks:
  1. boot frames arrive + calculator renders       (caster + calc render)
  2. typing 2+3*4= stays locked, no selection bar  (math works, no unlock)
  3. typing 4+6= shows the menu selection bar      (unlock works)
  4. 3x BACK returns to calculator                 (relock works)

Usage: hk_test.py [--port COM5] [--out test_out]
  Windows example: python tools\\hk_test.py --port COM5
  Linux example:   python tools/hk_test.py --port /dev/ttyUSB0
Exit code 0 = all checks pass.
"""
import argparse
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial required:  pip install pyserial")

SYNC = b"\x7A\xA5\xE1"
TAG = b"PKC"
LOOK = SYNC + TAG
W, H = 128, 64

WHITE = 0xFFFF   # lit OLED pixel (text / selection bars)
BLACK = 0x0000   # unlit OLED pixel (background)
BG_PX = (127, 63)  # margin pixel: always BLACK on calculator


def crc8(blob):
    crc = 0
    for b in blob:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def list_serial_ports():
    """Cross-platform serial port list (Windows COMx + POSIX tty)."""
    try:
        from serial.tools import list_ports
        return [p.device for p in list_ports.comports()]
    except Exception:
        pass
    import glob
    import os as _os
    ports = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if _os.name == "nt":
        # Fallback when pyserial list_ports is unavailable.
        for i in range(1, 65):
            ports.append(f"COM{i}")
    return ports


def auto_detect_port():
    """Pick the most likely ESP32 serial port, or None."""
    try:
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        if ports:
            def score(p):
                d = (p.description or "").lower()
                h = (p.hwid or "").lower()
                s = 0
                for kw in ("cp210", "ch340", "ch341", "ftdi", "ft232",
                           "silabs", "esp32", "espressif", "usb serial"):
                    if kw in d or kw in h:
                        s += 10
                if "bluetooth" in d or "bluetooth" in h:
                    s -= 20
                return s
            ports.sort(key=score, reverse=True)
            return ports[0].device
    except Exception:
        pass
    import glob
    cands = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    return cands[0] if cands else None


class Parser:
    HDR = 15

    def __init__(self):
        self.buf = b""
        self.rect = None
        self.row_len = 0
        self.rows = []
        self.row_i = 0

    def _take(self, n):
        if len(self.buf) < n:
            return None
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    @staticmethod
    def _dec_row(data, w):
        if len(data) == w * 2:
            return data
        out = bytearray()
        i, n, lim = 0, len(data), w * 2
        while i + 4 <= n:
            cnt, pix = struct.unpack_from("<HH", data, i)
            i += 4
            if cnt * 2 + len(out) > lim:
                return None
            out += bytes((pix & 0xFF, (pix >> 8) & 0xFF)) * cnt
        return bytes(out) if i == n else None

    def feed(self, chunk):
        self.buf += chunk
        rects = []
        junk = b""
        while True:
            if self.rect is None:
                idx = self.buf.find(LOOK)
                if idx < 0:
                    junk += self.buf[:-5]
                    self.buf = self.buf[-5:]
                    break
                junk += self.buf[:idx]
                self.buf = self.buf[idx:]
                hdr = self._take(self.HDR)
                if hdr is None:
                    break
                x, y, w, h = struct.unpack_from("<HHHH", hdr, 6)
                if (hdr[14] != crc8(hdr[6:14]) or w <= 0 or h <= 0 or
                        x + w > W or y + h > H):
                    junk += hdr[:1]
                    self.buf = self.buf[1:]
                    continue
                self.rect = (x, y, w, h)
                self.row_i, self.row_len, self.rows = 0, 0, []
            x, y, w, h = self.rect
            if self.row_len == 0:
                raw = self._take(2)
                if raw is None:
                    break
                (self.row_len,) = struct.unpack("<H", raw)
                if self.row_len < 1 or self.row_len > w * 2:
                    junk += raw[:1]
                    self.rect = None
                    continue
            data = self._take(self.row_len)
            if data is None:
                break
            dec = self._dec_row(data, w)
            self.row_len = 0
            if dec is None or len(dec) != w * 2:
                self.rect = None
                continue
            self.rows.append(dec)
            self.row_i += 1
            if self.row_i == h:
                rects.append((x, y, w, h, b"".join(self.rows)))
                self.rect = None
        return rects, junk


class Dev:
    def __init__(self, port, baud):
        try:
            self.ser = serial.Serial(port, baud, timeout=0.05)
        except serial.SerialException as e:
            sys.exit(f"cannot open {port} @ {baud}: {e}\n"
                     f"Available: {', '.join(list_serial_ports()) or 'none'}\n"
                     f"(Windows: use --port COMx and close Arduino Monitor / PuTTY first.)")
        self.ser.setDTR(False)
        self.ser.setRTS(True)
        time.sleep(0.1)
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        time.sleep(0.4)
        self.p = Parser()
        self.fb = bytearray(W * H * 2)
        self.nrect = 0

    def pump(self, secs):
        t0 = time.time()
        while time.time() - t0 < secs:
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            rects, junk = self.p.feed(chunk)
            if junk:
                clean = bytes(b for b in junk
                              if b in (0x0A, 0x0D) or 0x20 <= b < 0x7F)
                if clean:
                    try:
                        sys.stdout.write(clean.decode("ascii"))
                    except Exception:
                        pass
                    sys.stdout.flush()
            for x, y, w, h, pay in rects:
                for r in range(h):
                    s = r * w * 2
                    d = (y + r) * W * 2 + x * 2
                    self.fb[d:d + w * 2] = pay[s:s + w * 2]
                self.nrect += 1

    def key(self, ch, settle=0.35):
        self.ser.write(ch.encode())
        self.pump(settle)

    def px(self, x, y):
        o = (y * W + x) * 2
        return self.fb[o] | (self.fb[o + 1] << 8)

    def has_color(self, c):
        for v, in struct.iter_unpack("<H", self.fb):
            if v == c:
                return True
        return False

    def count_white(self, y0, y1, x0=0, x1=W):
        # lit pixels in a screen band (text / selection-bar detection)
        n = 0
        for y in range(max(0, y0), min(y1, H)):
            base = y * W * 2
            for x in range(max(0, x0), min(x1, W)):
                o = base + x * 2
                if (self.fb[o] | (self.fb[o + 1] << 8)) == WHITE:
                    n += 1
        return n

    def has_bar(self, y0=12, y1=48):
        # full-width inverted selection bar anywhere in the band: a menu,
        # scan list or attack list is on screen (calculator never has one;
        # its keypad cursor cell is only 30px wide).
        for y in range(max(0, y0), min(y1, H) - 8):
            if self.count_white(y, y + 9) >= 600:
                return True
        return False

    def has_color_in(self, y0, y1, c, x0=0, x1=W):
        # color in a screen region (unlocked-menu vs keypad disambiguation,
        # scroll-indicator detection, ...)
        for y in range(max(0, y0), min(y1, H)):
            base = y * W * 2
            for x in range(max(0, x0), min(x1, W)):
                o = base + x * 2
                if (self.fb[o] | (self.fb[o + 1] << 8)) == c:
                    return True
        return False

    def shot(self, path):
        try:
            from PIL import Image
            out = bytearray(W * H * 3)
            o = 0
            for v, in struct.iter_unpack("<H", self.fb):
                r = ((v >> 11) << 3); r |= r >> 5
                g = (((v >> 5) & 0x3F) << 2); g |= g >> 6
                b = ((v & 0x1F) << 3); b |= b >> 5
                out[o], out[o + 1], out[o + 2] = r, g, b
                o += 3
            Image.frombytes("RGB", (W, H), bytes(out)).save(path)
        except ImportError:
            print("PIL missing, skipping png")


POS = {  # keypad label -> (col, row)
    "C": (0, 0), "(": (1, 0), ")": (2, 0), "/": (3, 0),
    "7": (0, 1), "8": (1, 1), "9": (2, 1), "*": (3, 1),
    "4": (0, 2), "5": (1, 2), "6": (2, 2), "-": (3, 2),
    "1": (0, 3), "2": (1, 3), "3": (2, 3), "+": (3, 3),
    "0": (0, 4), ".": (1, 4), "<": (2, 4), "=": (3, 4),
}


def mem_loop(out, port, baud, cycles):
    """Unlock, then repeat wifi->ble scan cycles, capturing the on-screen
    heap readout mid-scan each round. Returns 0 iff every round completed
    (results screen + back to menu both ways). A device reboot or wedge
    mid-loop shows up as cascading FAILs."""
    fails = []

    def check(name, cond):
        print(("PASS " if cond else "FAIL ") + name, flush=True)
        if not cond:
            fails.append(name)

    def shot(name):
        return os.path.join(out, name)

    d = Dev(port, baud)
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

    d.pump(3.0)
    goto("C")
    cur = [0, 0]
    typ("4+6=")
    d.pump(0.5)
    check("unlocked", d.has_bar())
    if fails:
        print("RESULT: FAILURES: %s" % fails)
        return 1

    for i in range(cycles):
        # WiFi scan (menu cursor resets to item 0 on every menu entry).
        d.key("e")
        d.pump(2.0)
        d.shot(shot(f"mem_wifi_{i}.png"))
        d.pump(7.0)
        check(f"wifi {i} results", d.has_bar())
        if not d.has_bar() and d.px(*BG_PX) == BLACK:
            # Fresh calculator instead of results: silent reboot. Unlock
            # (fresh-boot state: cursor home, empty expr) to capture the
            # menu footer boot-cause tag, then carry on from the menu.
            print(f"  (cycle {i}: unexpected calc, reading boot cause)",
                  flush=True)
            cur = [0, 0]
            typ("4+6=")
            d.pump(0.5)
            d.shot(shot(f"foot_wifi_{i}.png"))
        d.key("b")
        d.pump(0.5)
        d.shot(shot(f"menu_w{i}.png"))
        check(f"wifi {i} menu", d.has_bar())
        # BLE scan.
        d.key("s")
        d.pump(0.3)
        d.key("e")
        d.pump(2.0)
        d.shot(shot(f"mem_ble_{i}.png"))
        d.pump(9.0)
        check(f"ble {i} results", d.has_bar())
        d.key("b")
        d.pump(0.5)
        d.shot(shot(f"menu_b{i}.png"))
        check(f"ble {i} menu", d.has_bar())

    d.key("b"); d.key("b"); d.key("b")
    d.pump(0.5)
    check("final relock", d.px(*BG_PX) == BLACK and
          not d.has_bar())

    print("RESULT: %s" % ("ALL PASS" if not fails else f"FAILURES: {fails}"))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None,
                    help="serial port (e.g. COM5 on Windows, /dev/ttyUSB0 on Linux; "
                         "auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--out", default="test_out")
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    ap.add_argument("--cycles", type=int, default=0,
                    help="mem-leak loop: unlock, then run N wifi->ble scan "
                         "cycles screenshotting the on-screen heap readout "
                         "each round (test_out/mem_wifi_i.png, mem_ble_i.png)")
    a = ap.parse_args()

    if a.list_ports:
        for p in list_serial_ports():
            print(p)
        return 0
    port = a.port or auto_detect_port()
    if not port:
        sys.exit("no serial port found. Use --port COMx (Windows, see --list-ports) or "
                 "--port /dev/ttyUSB0 (Linux/macOS).")

    os.makedirs(a.out, exist_ok=True)

    if a.cycles > 0:
        return mem_loop(a.out, port, a.baud, a.cycles)
    fails = []

    def check(name, cond):
        print(("PASS " if cond else "FAIL ") + name, flush=True)
        if not cond:
            fails.append(name)

    def shot(name):
        return os.path.join(a.out, name)

    d = Dev(port, a.baud)
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

    # 1. boot -> calculator
    d.pump(3.0)
    check("frames received", d.nrect > 0)
    d.shot(shot("calc_boot.png"))
    check("calculator bg pixel", d.px(*BG_PX) == BLACK)
    check("cursor highlight", d.px(5, 23) == WHITE)
    check("locked at boot", not d.has_bar())

    # 2. math that must NOT unlock: 2+3*4= -> 14
    typ("2+3*4=")
    d.pump(0.5)
    d.shot(shot("calc_math.png"))
    check("still locked after 2+3*4=", not d.has_bar())
    check("calc bg after math", d.px(*BG_PX) == BLACK)

    # 3. unlock: clear, then 4+6=  -> hack menu (orange title at top)
    goto("C")
    cur = [0, 0]
    typ("4+6=")
    d.pump(0.5)
    d.shot(shot("unlocked.png"))
    check("unlock shows menu", d.has_bar())

    # 4. relock via 3x BACK
    d.key("b"); d.key("b"); d.key("b")
    d.pump(0.5)
    d.shot(shot("relocked.png"))
    check("relock back to calc bg", d.px(*BG_PX) == BLACK)
    check("locked after relock", not d.has_bar())

    # 5. re-unlock: expr "4+6" survived, cursor still on "=", press it
    d.key("e")
    d.pump(0.5)
    d.shot(shot("unlocked2.png"))
    check("re-unlock on =", d.has_bar())

    # 6. WiFi scan: select item 1, wait out the blocking scan
    d.key("e")
    d.pump(9.0)
    d.shot(shot("scan.png"))
    check("scan screen rendered", d.has_bar())
    before = bytes(d.fb)
    d.key("e")  # OK -> detail screen for the highlighted entry
    d.pump(0.5)
    d.shot(shot("detail.png"))
    check("detail rendered", d.count_white(0, H) > 20 and
          bytes(d.fb) != before)
    d.key("b")  # back to list
    d.pump(0.5)
    check("back to list", d.has_bar())
    d.key("b")  # back to menu
    d.pump(0.5)
    check("back to menu", d.has_bar())

    # 7. BLE scan: menu item 2 (cursor still on item 1)
    d.key("s")
    d.pump(0.3)
    d.key("e")
    d.pump(11.0)
    d.shot(shot("ble.png"))
    check("ble screen rendered", d.has_bar())
    ble_before = bytes(d.fb)
    d.key("e")  # OK -> detail screen
    d.pump(0.5)
    d.shot(shot("ble_detail.png"))
    check("ble detail rendered", d.count_white(0, H) > 20 and
          bytes(d.fb) != ble_before)
    d.key("b")  # back to list
    d.pump(0.5)
    check("ble back to list", d.has_bar())
    d.key("b")  # back to menu
    d.pump(0.5)
    check("back to menu 2", d.has_bar())

    d.key("b"); d.key("b"); d.key("b")
    d.pump(0.5)
    check("final relock", d.px(*BG_PX) == BLACK and
          not d.has_bar())

    print("RESULT: %s" % ("ALL PASS" if not fails else f"FAILURES: {fails}"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
