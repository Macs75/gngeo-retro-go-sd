#!/usr/bin/env python3
"""neo_tile_runs.py -- do a set's duplicate sprite tiles come in runs?

  tools/neo_tile_runs.py <game.zip> [...]

For each sprite tile (C pairs byte-interleaved, 128 bytes) that repeats an
earlier tile, finds the run of consecutive tiles that repeats a run of
consecutive earlier tiles (same distance back). Blank tiles (all zero,
fully transparent: never read by the renderer) are counted apart. Prints
how the duplicate bytes split by run length, and for runs of at least 16
tiles, whether source and copy sit at the same offset within 2 KiB /
64 KiB blocks (what a block table at that size could share).
"""
import re, sys, zipfile
from collections import defaultdict

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
    return [bytes(c[i:i + 128]) for i in range(0, len(c), 128)]

def main():
    for path in sys.argv[1:]:
        t = tiles_of(path)
        first, src = {}, [-1] * len(t)
        for i, x in enumerate(t):
            if x in first:
                src[i] = first[x]
            else:
                first[x] = i
        uni = [x.count(x[0]) == 128 for x in t]
        nblank = sum(1 for i in range(len(t)) if src[i] >= 0 and uni[i])
        # runs: i..j-1 all duplicates with src[k] = src[i] + (k - i)
        runs = []
        i = 0
        while i < len(t):
            if src[i] < 0 or uni[i]:
                i += 1
                continue
            j = i + 1
            while j < len(t) and src[j] == src[i] + (j - i) and not uni[j]:
                j += 1
            runs.append((i, j - i, src[i]))
            i = j
        dup = sum(r[1] for r in runs)
        buckets = defaultdict(int)
        for _, n, _ in runs:
            b = 1 if n < 2 else 2 if n < 16 else 16 if n < 64 else 64 if n < 256 else 256
            buckets[b] += n
        print(f"{path.split('/')[-1]}: {len(t)} tiles, duplicates {(dup + nblank) * 128 / MIB:.2f} MiB "
              f"(uniform {nblank * 128 / MIB:.2f}, other {dup * 128 / MIB:.2f}) in {len(runs)} runs")
        for b, lab in ((1, "single tiles"), (2, "runs of 2-15"), (16, "runs of 16-63"),
                       (64, "runs of 64-255"), (256, "runs of 256+")):
            print(f"   {lab:15s} {buckets[b] * 128 / MIB:6.2f} MiB")
        for blk in (16, 512):     # tiles per block: 2 KiB, 64 KiB
            same = sum(n for s, n, f in runs if n >= 16 and (s - f) % blk == 0)
            whole = 0
            for s, n, f in runs:
                if n >= 16 and (s - f) % blk == 0:
                    a = -(-s // blk) * blk
                    e = (s + n) // blk * blk
                    whole += max(0, e - a)
            print(f"   runs >= 16 at the same offset in {blk * 128 // 1024} KiB blocks: "
                  f"{same * 128 / MIB:.2f} MiB, whole blocks of it {whole * 128 / MIB:.2f} MiB")

main()
