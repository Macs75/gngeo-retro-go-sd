#!/usr/bin/env python3
"""neo_tile_flips.py -- how many distinct sprite tiles a set has when a tile
that is a mirror image of another (horizontal, vertical or both, the flips
a Neo Geo sprite applies per tile) counts as the same tile.

  tools/neo_tile_flips.py <game.zip> [...]

Tiles are decoded to 16x16 pens as the core's convert_one_tile does
(src/porting/neo_zip_flash.c): row y, left half from bytes 64 + 4y..+3,
right half from bytes 4y..+3 (planes 3,1,2,0 -> pen bits 3..0), pixel x of
a half from bit x. Prints distinct tiles plain and up to flips, and the
sizes an indexed set (tools/neo_tile_index.py) would have.
"""
import re, sys, zipfile
import numpy as np

MIB = 1 << 20

def tiles_of(path):
    z = zipfile.ZipFile(path)
    cs = sorted([n for n in z.namelist() if re.search(r'[-_]c\d+\.c\d+$', n, re.I)],
                key=lambda n: int(re.search(r'(\d+)$', n).group(1)))
    c = bytearray()
    for i in range(0, len(cs) - 1, 2):
        a, b = z.read(cs[i]), z.read(cs[i + 1])
        t = bytearray(len(a) * 2)
        t[0::2], t[1::2] = a, b
        c += t
    return np.frombuffer(bytes(c), dtype=np.uint8).reshape(-1, 128)

def pens(t):
    """(n, 128) raw tiles -> (n, 16, 16) pens"""
    n = t.shape[0]
    out = np.zeros((n, 16, 16), dtype=np.uint8)
    for half, base in ((0, 64), (1, 0)):
        rows = t[:, base:base + 64].reshape(n, 16, 4)        # y, 4 plane bytes
        for x in range(8):
            p = (((rows[:, :, 3] >> x) & 1) << 3) | (((rows[:, :, 1] >> x) & 1) << 2) | \
                (((rows[:, :, 2] >> x) & 1) << 1) | ((rows[:, :, 0] >> x) & 1)
            out[:, :, half * 8 + x] = p
    return out

def main():
    for path in sys.argv[1:]:
        t = tiles_of(path)
        n = t.shape[0]
        plain = len(np.unique(t, axis=0))
        p = pens(t).reshape(n, 256)
        g = p.reshape(n, 16, 16)
        forms = [g, g[:, :, ::-1], g[:, ::-1, :], g[:, ::-1, ::-1]]
        keys = np.stack([f.reshape(n, 256) for f in forms])     # 4, n, 256
        # canonical form: the lexicographically smallest of the four
        packed = [np.packbits(k.astype(np.uint8)[:, :, None] >> np.arange(3, -1, -1) & 1, axis=None)
                  .reshape(n, 128) for k in keys]
        best = packed[0].copy()
        for k in packed[1:]:
            # row-wise lexicographic min
            lt = np.zeros(n, dtype=bool)
            undecided = np.ones(n, dtype=bool)
            for col in range(128):
                a, b = k[:, col], best[:, col]
                lt |= undecided & (a < b)
                undecided &= (a == b)
                if not undecided.any():
                    break
            best[lt] = k[lt]
        flips = len(np.unique(best, axis=0))
        idx = n * 4
        print(f"{path.split('/')[-1]}: {n} tiles ({n * 128 / MIB:.2f} MiB); distinct {plain} "
              f"({plain * 128 / MIB:.2f} MiB); distinct up to flips {flips} ({flips * 128 / MIB:.2f} MiB); "
              f"index {idx / MIB:.2f} MiB")

main()
