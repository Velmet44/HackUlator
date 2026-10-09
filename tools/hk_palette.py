#!/usr/bin/env python3
"""Mono OLED palette: the 128x64 UI is 1-bit (white text/bars on black),
mirrored over the caster wire as RGB565 white/black.
Usage: python tools/hk_palette.py"""
WHITE = 0xFFFF
BLACK = 0x0000

print(f"WHITE  0x{WHITE:04X}  rgb(255,255,255)")
print(f"BLACK  0x{BLACK:04X}  rgb(0,0,0)")
print()
print("PY constants for tests:")
print(f"WHITE = 0x{WHITE:04X}")
print(f"BLACK = 0x{BLACK:04X}")
