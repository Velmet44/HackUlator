#!/usr/bin/env python3
"""Decode a captured framebuffer PNG and report the colour of glyph cells in
a given row band (finds why a text-colour probe missed).
Usage: python tools/hk_px.py <png> <y0> <y1>"""
import sys

from PIL import Image

path = sys.argv[1]
y0 = int(sys.argv[2]) if len(sys.argv) > 2 else 296
y1 = int(sys.argv[3]) if len(sys.argv) > 3 else 318

im = Image.open(path).convert("RGB")
w, h = im.size
px = im.load()

from collections import Counter
c = Counter()
runs = []
cur = None
for y in range(y0, min(y1, h)):
    for x in range(w):
        v = px[x, y]
        if v == (0, 0, 0):
            c["black"] += 1
            if cur is not None:
                runs.append(cur)
                cur = None
            continue
        c[v] += 1
        if cur is None:
            cur = [x, x, v]
        else:
            cur[1] = x
            cur[2] = v
if cur is not None:
    runs.append(cur)

print("counts:", c.most_common(6))
for a, b, v in runs:
    print(f"  x {a}-{b} ({b-a+1}px) {v}")