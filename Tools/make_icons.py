#!/usr/bin/env python3
"""Generates the launcher-icon bitmaps for the task lists app.

Writes into ../Rsc (next to tasklists.rcp):

  large icon (22x22 low density, 44x44 double density)
    icon22.bmp            1-bit   (existing file, not rewritten)
    icon22_gray.bmp       2-bit   (stored as a 4-bit BMP, levels 0/5/10/15)
    icon22_color.bmp      8-bit   (existing file, not rewritten)
    icon44.bmp            1-bit   double density
    icon44_gray.bmp       2-bit   double density
    icon44_color.bmp      8-bit   double density
  small icon (15x9 low density, 30x18 double density)
    icon15x9.bmp / icon15x9_color.bmp   existing, not rewritten
    icon15x9_gray.bmp, icon30x18.bmp, icon30x18_gray.bmp, icon30x18_color.bmp

Design: orange rounded tile with three black list rows (bullet + bar).
On 1-bit screens the tile is black with white rows (as before), on 2-bit
screens the tile is light gray with black rows.

The 8-bit files reuse the palette of the existing icon22_color.bmp so the
colors match what PilRC already accepted.  Pass --all to also rewrite the
existing low-density files (they should come out identical).

Run:  python make_icons.py
"""
import os
import struct
import sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
RSC = os.path.join(HERE, "..", "Rsc")

# Shapes in 1x pixel units, (x0, y0, x1, y1) inclusive.
LARGE = dict(
    w=22, h=22,
    tile=(0, 0, 21, 21),
    cut1=[(0, 0), (1, 0), (0, 1)],
    bullets=[(3, 5, 4, 6), (3, 10, 4, 11), (3, 15, 4, 16)],
    bars=[(7, 5, 18, 6), (7, 10, 18, 11), (7, 15, 13, 16)],
)
SMALL = dict(
    w=15, h=9,
    tile=(0, 0, 14, 8),
    cut1=[(0, 0)],
    bullets=[(2, 2, 2, 2), (2, 4, 2, 4), (2, 6, 2, 6)],
    bars=[(4, 2, 11, 2), (4, 4, 11, 4), (4, 6, 8, 6)],
)

WHITE, TILE, ROW = 0, 1, 2          # logical pixel classes


def render(spec, s):
    """Returns rows of logical classes at scale s (1 or 2)."""
    w, h = spec["w"] * s, spec["h"] * s
    px = [[WHITE] * w for _ in range(h)]

    def fill(box, cls):
        x0, y0, x1, y1 = box
        for y in range(y0 * s, (y1 + 1) * s):
            for x in range(x0 * s, (x1 + 1) * s):
                px[y][x] = cls

    fill(spec["tile"], TILE)
    # round the corners: 1 px cut at 1x, 3 px radius-ish at 2x
    cut = spec["cut1"] if s == 1 else [(0, 0), (1, 0), (0, 1), (2, 0), (0, 2), (1, 1)]
    for (dx, dy) in cut:
        for (cx, cy) in ((dx, dy), (w - 1 - dx, dy), (dx, h - 1 - dy),
                         (w - 1 - dx, h - 1 - dy)):
            px[cy][cx] = WHITE
    for b in spec["bullets"] + spec["bars"]:
        fill(b, ROW)
    return px


def write_bmp(name, w, h, bpp, palette, rows):
    row_bytes = ((w * bpp + 31) // 32) * 4
    ncolors = 1 << bpp
    pal = list(palette) + [(0, 0, 0)] * (ncolors - len(palette))
    pal_bytes = b"".join(struct.pack("<BBBB", b, g, r, 0) for (r, g, b) in pal)
    data = bytearray()
    for row in reversed(rows):
        line = bytearray(row_bytes)
        for x, idx in enumerate(row):
            bit = x * bpp
            if bpp == 8:
                line[x] = idx
            else:
                line[bit // 8] |= idx << (8 - bpp - (bit % 8))
        data += line
    off = 14 + 40 + len(pal_bytes)
    hdr = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, 0, len(data),
                       2835, 2835, ncolors, 0)
    with open(os.path.join(RSC, name), "wb") as f:
        f.write(hdr + info + pal_bytes + bytes(data))


GRAY16 = [(255 - i * 17,) * 3 for i in range(16)]    # 0 white .. 15 black


def write_mono(name, px):
    # tile black, rows white
    m = {WHITE: 0, TILE: 1, ROW: 0}
    write_bmp(name, len(px[0]), len(px), 1, [(255, 255, 255), (0, 0, 0)],
              [[m[c] for c in r] for r in px])


def write_gray(name, px):
    # 2-bit levels 0 white, 1 light, 2 dark, 3 black -> BMP levels 0/5/10/15
    m = {WHITE: 0, TILE: 5, ROW: 15}
    write_bmp(name, len(px[0]), len(px), 4, GRAY16,
              [[m[c] for c in r] for r in px])


def load_color_palette():
    im = Image.open(os.path.join(RSC, "icon22_color.bmp"))
    pal = im.getpalette()
    pal = [tuple(pal[i:i + 3]) for i in range(0, len(pal), 3)]
    pal += [(0, 0, 0)] * (256 - len(pal))
    # indices used by the existing image for white / orange / black
    idx = {}
    for c, raw in zip(im.convert("RGB").getdata(), im.getdata()):
        idx[c] = raw
    return pal, idx[(255, 255, 255)], idx[(255, 102, 0)], idx[(0, 0, 0)]


def write_color(name, px, pal, iw, it, ir):
    m = {WHITE: iw, TILE: it, ROW: ir}
    write_bmp(name, len(px[0]), len(px), 8, pal, [[m[c] for c in r] for r in px])


def main():
    redo = "--all" in sys.argv
    pal, iw, it, ir = load_color_palette()
    for spec, base, s1, s2 in ((LARGE, "icon22", "icon22", "icon44"),
                               (SMALL, "icon15x9", "icon15x9", "icon30x18")):
        lo, hi = render(spec, 1), render(spec, 2)
        if redo:
            write_mono(s1 + ".bmp", lo)
            write_color(s1 + "_color.bmp", lo, pal, iw, it, ir)
        write_gray(s1 + "_gray.bmp", lo)
        write_mono(s2 + ".bmp", hi)
        write_gray(s2 + "_gray.bmp", hi)
        write_color(s2 + "_color.bmp", hi, pal, iw, it, ir)
    print("done")


if __name__ == "__main__":
    main()
