#!/usr/bin/env python3
"""HACKULATOR — serial display viewer (cross-platform: Win/macOS/Linux).

Parses the "PKC" dirty-rect stream a HACKULATOR casts over UART0
(460800 baud). The ESP32 streams redraw regions with per-row RLE; the
mirror expands the mono OLED pixels to RGB565 white/black, so the picture
is pixel-identical to the 128x64 OLED. Any non-frame bytes (early boot
logs, before the caster starts) received in between are echoed to the
terminal (disable with --no-log).

Protocol (v2, little-endian):
  header (15 B): 7A A5 E1 'P' 'K' 'C'  x, y, w, h (u16 each)  crc8(x,y,w,h)
  payload: per row — enc_len:u16, then that many bytes:
      enc_len == w*2  -> raw RGB565
      enc_len smaller -> RLE runs of [count:u16][pixel:u16]

Also forwards keyboard events to the ESP32 as single control bytes so you
can drive the 6-button UI remotely:
    W w / A a / S s / D d   -> up / left / down / right
    E e / Enter              -> OK
    B b / Escape             -> BACK

Usage:
    Windows: python hacku_viewer.py [--port COM5] [--baud 460800]
    Debian:  python3 hacku_viewer.py [--port /dev/ttyUSB0] [--baud 460800]
             [--width 128] [--height 64]
    Deps: pip install pyserial pillow (Debian also: sudo apt install python3-tk)
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
W, H = 128, 64   # HACKULATOR OLED; overridable via --width/--height
KEYMAP = {
    "Up": "w", "Left": "a", "Down": "s", "Right": "d",
    "Return": "e", "KP_Enter": "e", "Escape": "b",
    "w": "w", "a": "a", "s": "s", "d": "d", "e": "e", "b": "b",
    "W": "w", "A": "a", "S": "s", "D": "d", "E": "e", "B": "b",
}


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
    ports = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if os.name == "nt":
        # Fallback when pyserial list_ports is unavailable: probe COM1..COM64.
        for i in range(1, 65):
            ports.append(f"COM{i}")
    return ports


def auto_detect_port():
    """Pick the most likely ESP32 serial port, or None."""
    try:
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        if not ports:
            return None
        # Prefer USB-serial adapters (CP210x / CH340 / FTDI / Espressif VID).
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
    cands = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if cands:
        return sorted(cands)[0]
    return None


class RectParser:
    """Incremental parser for the PKC dirty-rect protocol. feed() returns
    decoded rects as (x, y, w, h, payload_bytes) plus any junk (logs)."""

    HDR_LEN = 15

    def __init__(self):
        self.buf = b""
        self.rect = None          # (x, y, w, h) while rows are being read
        self.row_i = 0
        self.row_len = 0          # bytes still pending for the current row
        self.rows = []

    def _take(self, n):
        if len(self.buf) < n:
            return None
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    @staticmethod
    def _decode_row(data, w):
        if len(data) == w * 2:
            return data
        out = bytearray()
        i = 0
        n = len(data)
        limit = w * 2
        while i + 4 <= n:
            count, pix = struct.unpack_from("<HH", data, i)
            i += 4
            if count * 2 + len(out) > limit:
                return None          # run overruns the row width: corrupt
            out += bytes((pix & 0xFF, (pix >> 8) & 0xFF)) * count
        if i != n:
            return None              # trailing partial RLE pair: corrupt
        return bytes(out)

    def _abort_rect(self):
        self.rect = None
        self.row_len = 0
        self.rows = []
        self.row_i = 0

    def feed(self, chunk):
        self.buf += chunk
        rects = []
        junk = b""

        while True:
            if self.rect is None:
                idx = self.buf.find(LOOK)
                if idx < 0:
                    keep = self.buf[-5:]
                    junk += self.buf[:-5]
                    self.buf = keep
                    break
                junk += self.buf[:idx]
                self.buf = self.buf[idx:]
                hdr = self._take(self.HDR_LEN)
                if hdr is None:
                    break
                if len(hdr) != self.HDR_LEN:
                    self.buf = hdr
                    break
                crc = hdr[14]
                x, y, w, h = struct.unpack_from("<HHHH", hdr, 6)
                if (crc != crc8(hdr[6:14]) or w <= 0 or h <= 0 or
                        x < 0 or y < 0 or x + w > W or y + h > H):
                    junk += hdr[:1]
                    continue
                self.rect = (x, y, w, h)
                self.row_i = 0
                self.row_len = 0
                self.rows = []

            x, y, w, h = self.rect
            if self.row_len == 0:
                raw = self._take(2)
                if raw is None:
                    break
                (self.row_len,) = struct.unpack("<H", raw)
                if self.row_len < 1 or self.row_len > w * 2:
                    junk += raw[:1]
                    self._abort_rect()
                    continue
            data = self._take(self.row_len)
            if data is None:
                break
            if len(data) != self.row_len:
                self._abort_rect()
                continue
            dec = self._decode_row(data, w)
            if dec is None or len(dec) != w * 2:
                self._abort_rect()          # corrupt row: drop rect, resync
                continue
            self.rows.append(dec)
            self.row_len = 0
            self.row_i += 1
            if self.row_i == h:
                rects.append((x, y, w, h, b"".join(self.rows)))
                self._abort_rect()

        return rects, junk


def _rgb565_table():
    tab = bytearray(65536 * 3)
    for v in range(65536):
        r = (v >> 11) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        tab[v * 3] = r | (r >> 5)
        tab[v * 3 + 1] = g | (g >> 6)
        tab[v * 3 + 2] = b | (b >> 5)
    return bytes(tab)


def run_viewer(port=None, baud=460800, echo_log=True, no_reset=False,
               width=128, height=64):
    global W, H
    W, H = width, height
    if port is None:
        port = auto_detect_port()
        if not port:
            sys.exit("no serial port found.\n"
                     "Use --port COMx (Windows, see --list-ports) or "
                     "--port /dev/ttyUSB0 (Linux/macOS).")
    try:
        ser = serial.Serial(port, baud, timeout=0.05)
    except serial.SerialException as e:
        sys.exit(f"cannot open {port} @ {baud}: {e}\n"
                 f"Available: {', '.join(list_serial_ports()) or 'none'}\n"
                 f"(Windows: close Arduino Monitor / PuTTY / IDF Monitor first.)")
    if not no_reset:
        # esptool-style hard reset: assert /EN through RTS, then release so the
        # ESP32 cold-boots into caster mode (single-wire setup, no flash needed).
        ser.setDTR(False)   # IO0 = HIGH (normal boot)
        ser.setRTS(True)    # /EN = LOW -> chip in reset
        time.sleep(0.1)
        ser.setDTR(False)
        ser.setRTS(False)   # /EN = HIGH -> chip runs
        time.sleep(0.4)
    parser = RectParser()

    import tkinter as tk
    try:
        from PIL import Image, ImageTk
    except Exception:
        ImageTk = None

    root = tk.Tk()
    root.title(f"HACKULATOR — {port} @ {baud}")
    root.configure(bg="black")
    root.geometry(f"{W*4}x{H*4 + 24}")
    root.minsize(W, H)

    status = tk.Label(root, text="waiting for frames...", fg="#8f8", bg="black",
                      font=("Consolas", 9), anchor="w")
    status.pack(side="bottom", fill="x")
    lbl = tk.Label(root, bg="black")
    lbl.pack(expand=True, fill="both")

    fb = bytearray(W * H * 2)             # full RGB565 buffer
    backend = {"raw": None, "img": None, "pil": ImageTk is not None,
               "w": W, "h": H, "last": 0, "cw": 0, "ch": 0, "dirty": False}

    def apply_rect(x, y, w, h, payload):
        for row in range(h):
            src = row * w * 2
            dst = (y + row) * W * 2 + x * 2
            fb[dst:dst + w * 2] = payload[src:src + w * 2]
        backend["dirty"] = True

    def redraw():
        raw = backend["raw"]
        if raw is None:
            return
        avail_w = max(100, root.winfo_width())
        avail_h = max(100, root.winfo_height() - status.winfo_reqheight())
        w, h = raw.size
        s = min(avail_w / w, avail_h / h)
        if s <= 0:
            return
        # Integer scale keeps 1px OLED glyphs crisp: fractional NEAREST
        # makes some source pixels 1 and others 2 display px wide, which
        # turns the 4px font into mush. Fall back to fractional only when
        # the window is smaller than the native frame.
        si = int(s)
        if si >= 1:
            s = si
        nw = max(1, int(w * s))
        nh = max(1, int(h * s))
        im = raw.resize((nw, nh), Image.NEAREST)
        photo = ImageTk.PhotoImage(im)
        lbl.configure(image=photo)
        backend["img"] = photo
        backend["cw"], backend["ch"] = nw, nh

    pix_tab = _rgb565_table()
    outpix = bytearray(W * H * 3)

    def convert():
        nonlocal outpix
        o = 0
        mv = memoryview(outpix)
        for v in struct.iter_unpack("<H", fb):
            t = v[0] * 3
            mv[o], mv[o + 1], mv[o + 2] = pix_tab[t], pix_tab[t + 1], pix_tab[t + 2]
            o += 3
        backend["raw"] = Image.frombuffer("RGB", (W, H), bytes(outpix),
                                          "raw", "RGB", 0, 1)
        backend["dirty"] = False

    stats = {"fps": 0.0, "samples": 0, "last_key": "-",
             "t0": time.monotonic(), "crc": 0}
    last_conv = [0.0]
    last_resize = [0, 0]

    def on_resize(ev):
        if (ev.width, ev.height) != (last_resize[0], last_resize[1]):
            last_resize[0], last_resize[1] = ev.width, ev.height
            redraw()

    def pump():
        try:
            chunk = ser.read(max(1, ser.in_waiting))
            rects, junk = parser.feed(chunk)
            if echo_log and junk:
                # filter to printable ASCII + newlines only (drops binary noise
                # from the 115200 boot ROM before caster re-bauds to 1M)
                clean = bytes(b for b in junk if b in (0x0A, 0x0D) or 0x20 <= b < 0x7F)
                if clean:
                    sys.stdout.write(clean.decode("ascii"))
                    sys.stdout.flush()
            for x, y, w, h, payload in rects:
                apply_rect(x, y, w, h, payload)
                stats["samples"] += 1
            now = time.monotonic()
            if backend["dirty"] and now - last_conv[0] >= 0.1:
                convert()
                last_conv[0] = now
                backend["last"] = now
                redraw()
            el = now - stats["t0"]
            if el >= 1.0:
                stats["fps"] = stats["samples"] / el
                stats["samples"] = 0
                stats["t0"] = now
                status.configure(text="%dx%d   rects/s %.1f   display %dx%d   last key %s"
                                  % (W, H, stats["fps"],
                                     backend["cw"] or W, backend["ch"] or H,
                                     stats["last_key"]))
        except Exception as e:
            print("\r[viewer] error: %r" % e, file=sys.stderr, flush=True)
        root.after(16, pump)

    def on_key(ev):
        ch = KEYMAP.get(ev.keysym)
        if ch:
            try:
                ser.write(ch.encode())
                stats["last_key"] = ev.keysym
                print("\r[key] %-8s sent to %s  " % (ev.keysym, port), flush=True)
            except Exception as e:
                print("\r[key] send failed: %r" % e, file=sys.stderr, flush=True)

    root.bind("<KeyPress>", on_key)
    root.bind("<Button-1>", lambda e: root.focus_set())
    root.bind("<Configure>", on_resize)
    root.after(0, pump)
    root.update_idletasks()
    root.focus_force()
    root.mainloop()
    ser.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None,
                    help="serial port (e.g. COM5 on Windows, /dev/ttyUSB0 on Linux; "
                         "auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--no-log", action="store_true", help="disable log echo to terminal")
    ap.add_argument("--no-reset", action="store_true", help="do NOT pulse a hardware reset on connect")
    ap.add_argument("--width", type=int, default=128, help="display width")
    ap.add_argument("--height", type=int, default=64, help="display height")
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    a = ap.parse_args()
    if a.list_ports:
        for p in list_serial_ports():
            print(p)
        sys.exit(0)
    run_viewer(port=a.port, baud=a.baud, echo_log=not a.no_log, no_reset=a.no_reset,
               width=a.width, height=a.height)