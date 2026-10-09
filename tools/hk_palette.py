#!/usr/bin/env python3
"""Print the RGB565 values of the HackUlator palette exactly as gfx.h's
C_RGB macro computes them (so tests can compare framebuffer pixels).
Usage: python tools/hk_palette.py"""
def C(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

PAL = {
    "C_BLACK": 0x0000, "C_WHITE": 0xFFFF,
    "C_RED": C(255, 0, 0), "C_GREEN": C(0, 200, 0),
    "C_BLUE": C(60, 120, 255), "C_YELLOW": C(255, 200, 0),
    "C_ORANGE": C(255, 140, 0),
    "C_GRAY25": C(64, 64, 64), "C_GRAY50": C(128, 128, 128),
    "C_LTGRAY": C(200, 200, 200),
    "CALC_BG": C(24, 26, 32), "CALC_DISP": C(16, 18, 22),
    "CALC_KEY": C(48, 52, 62), "CALC_KEY_FN": C(255, 140, 0),
    "CALC_CURSOR": C(255, 200, 0), "CALC_TEXT": C(255, 255, 255),
    "CALC_DIM": C(150, 155, 165),
}

for k, v in PAL.items():
    r = (v >> 11) << 3
    r |= r >> 5
    g = ((v >> 5) & 0x3F) << 2
    g |= g >> 6
    b = (v & 0x1F) << 3
    b |= b >> 5
    print(f"{k:12s} 0x{v:04X}  rgb({r},{g},{b})")

print()
print("PY constants for tests:")
for k in ("C_RED", "C_GREEN", "C_ORANGE", "CALC_DIM", "CALC_BG"):
    print(f"{k} = 0x{PAL[k]:04X}")