#!/usr/bin/env python3
"""Decode text out of a captured HackUlator screen, or a live caster frame.

The OLED font is a fixed 4x9 cell, one byte per row, MSB-first across the four
columns, and main/font_oled.h is plain text - so a captured framebuffer can be
matched back to glyphs without guessing at shapes.

  python tools/hk_ocr.py shot.png        OCR a PNG written by a test
  python tools/hk_ocr.py --live COM5     OCR whatever is on screen now

Text is drawn at arbitrary y (1, 10, 46, 55 ...), so the row alignment is
searched rather than assumed: every offset is scored against the font and only
local maxima are reported.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "main", "font_oled.h")

GLYPH_RE = re.compile(r"/\*\s*'(.)'\s*\((\d+)\)\s*\*/\s*([0x0-9A-Fa-f,\s]+)")


def load_font():
    """char -> list of per-row bytes, parsed from main/font_oled.h."""
    with open(FONT, "r", encoding="utf-8", errors="replace") as f:
        txt = f.read()
    font = {}
    for m in GLYPH_RE.finditer(txt):
        ch = m.group(1)
        font[ch] = [int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})",
                                                   m.group(3))]
    return font


def lit(col, row, data):
    """Is column `col` of `row` lit? MSB-first across the 4 columns."""
    if row >= len(data):
        return 0
    return (data[row] >> (7 - col)) & 1


def match_band(fb, W, y0, xoff, font, cw=4, chh=9):
    """Best glyph per cell at one alignment -> (text, avg_score)."""
    chars, total, cells = [], 0, 0
    for x0 in range(xoff, W, cw):
        best, bestscore = " ", -1
        for ch, data in font.items():
            score = 0
            for r in range(chh):
                for c in range(cw):
                    score += 1 if lit(c, r, data) == fb.get((x0 + c, y0 + r), 0) \
                        else 0
            if score > bestscore:
                best, bestscore = ch, score
        chars.append(best)
        total += bestscore
        cells += 1
    return "".join(chars).rstrip(), (total / cells if cells else 0.0)


def ocr(fb, W, H, font):
    """Every text band at its best alignment.

    Both offsets matter: text is drawn at arbitrary y (1, 10, 46, 55) AND
    centred text starts at an arbitrary x - draw_msg puts a 13-character
    line at x=38, which is not a multiple of the 4px cell. Searching only
    y misses every centred message in the UI.
    """
    per = []
    for y0 in range(0, H - 9 + 1):
        text, avg = max((match_band(fb, W, y0, xoff, font) for xoff in range(4)),
                        key=lambda t: t[1])
        n = sum(fb.get((x, y), 0) for y in range(y0, y0 + 9) for x in range(W))
        per.append((y0, text, avg, n))
    out = []
    for i, (y0, text, avg, n) in enumerate(per):
        if n == 0 or avg < 0.85 or not text.strip():
            continue
        # keep only the FIRST alignment of a run that reads the same text
        if i > 0 and per[i - 1][1] == text:
            continue
        out.append((y0, text.strip()))
    return out


def fb_from_png(path):
    from PIL import Image
    im = Image.open(path).convert("L")
    w, h = im.size
    raw = im.get_flattened_data() if hasattr(im, "get_flattened_data") \
        else im.getdata()
    px = list(raw)
    return {(x, y): (1 if px[y * w + x] > 127 else 0)
            for y in range(h) for x in range(w)}, w, h


def fb_from_dev(port):
    sys.path.insert(0, HERE)
    from hk_test import Dev
    d = Dev(port, 460800)
    d.pump(2.0)
    W, H = 128, 64
    fb = {}
    for y in range(H):
        for x in range(W):
            o = (y * W + x) * 2
            fb[(x, y)] = 1 if (d.fb[o] | (d.fb[o + 1] << 8)) == 0xFFFF else 0
    return fb, W, H


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    font = load_font()
    if args[0] == "--live":
        fb, W, H = fb_from_dev(args[1] if len(args) > 1 else "COM5")
    else:
        fb, W, H = fb_from_png(args[0])
    for y, text in ocr(fb, W, H, font):
        print("y%-3d |%s" % (y, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())