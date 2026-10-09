#!/usr/bin/env python3
"""Report lit pixel coordinates of a colour in a PNG (anchor finder).
Usage: python tools/hk_anchor.py <png> <r,g,b> [y0] [y1]"""
import sys

from PIL import Image

path = sys.argv[1]
want = tuple(int(x) for x in sys.argv[2].split(","))
y0 = int(sys.argv[3]) if len(sys.argv) > 3 else 0
y1 = int(sys.argv[4]) if len(sys.argv) > 4 else 320

im = Image.open(path).convert("RGB")
w, h = im.size
px = im.load()
hits = []
for y in range(min(y0, h), min(y1, h)):
    for x in range(w):
        if px[x, y] == want:
            hits.append((x, y))
if not hits:
    print("no exact match for", want)
else:
    xs = [p[0] for p in hits]
    ys = [p[1] for p in hits]
    print(f"{len(hits)} px  x {min(xs)}-{max(xs)}  y {min(ys)}-{max(ys)}")
    print("first 10:", hits[:10])