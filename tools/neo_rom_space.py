#!/usr/bin/env python3
"""neo_rom_space.py -- where a Neo Geo set's flash space goes: padding and
duplicated data, region by region, to see what a big set could save.

  tools/neo_rom_space.py <game.zip>      (a plain or decrypted MAME zip)

Per region (P, M, V, S, C as the core caches them: C pairs byte-
interleaved into 128-byte tiles):
  - uniform 4 KiB blocks (one byte value: unprogrammed EPROM space, 0x00/
    0xFF), and the longest such run;
  - 512 KiB pieces (neo_pieces.h) that are uniform, or identical to an
    earlier piece (what caching identical pieces once would save);
  - C only: blank tiles (all zero: fully transparent), and duplicate tiles
    (what a per-tile index would save).
"""
import re, sys, zipfile, hashlib
from collections import Counter

PIECE = 512 * 1024
MIB = 1 << 20

def chip_index(name):
    m = re.search(r'(\d+)\.[a-z0-9]+$', name)
    return int(m.group(1)) if m else 0

def regions(z):
    names = z.namelist()
    def pick(letter):
        pat = re.compile(r'[-_]%s[a-z]*\d+\.[a-z]*%s?\d*$' % (letter, letter), re.I)
        return sorted([n for n in names if pat.search(n)], key=chip_index)
    r = {}
    for letter in "pmvs":
        lst = pick(letter)
        if lst:
            r[letter.upper()] = b"".join(z.read(n) for n in lst)
    cs = pick("c")
    c = bytearray()
    for i in range(0, len(cs) - 1, 2):
        a, b = z.read(cs[i]), z.read(cs[i + 1])
        t = bytearray(len(a) * 2)
        t[0::2], t[1::2] = a, b
        c += t
    if c:
        r["C"] = bytes(c)
    return r

def uniform(b):
    return b.count(b[0]) == len(b)

def report(name, d):
    blocks = [d[i:i + 4096] for i in range(0, len(d), 4096)]
    uni = [uniform(b) for b in blocks]
    run = best = 0
    for u in uni:
        run = run + 1 if u else 0
        best = max(best, run)
    pieces = [d[i:i + PIECE] for i in range(0, len(d), PIECE)]
    seen, dup, uni_p = set(), 0, 0
    for p in pieces:
        h = hashlib.sha1(p).digest()
        if uniform(p):
            uni_p += 1
        if h in seen:
            dup += len(p)
        seen.add(h)
    print(f"{name}: {len(d) / MIB:6.2f} MiB | uniform 4K blocks {sum(uni) * 4096 / MIB:5.2f} MiB "
          f"(longest run {best * 4096 / MIB:.2f} MiB) | 512K pieces: {len(pieces)}, "
          f"{uni_p} uniform, repeats of an earlier piece {dup / MIB:.2f} MiB")
    if name == "C":
        tiles = Counter(d[i:i + 128] for i in range(0, len(d), 128))
        n = len(d) // 128
        blank = tiles.get(bytes(128), 0)
        print(f"   tiles: {n}, blank (all zero) {blank} = {blank * 128 / MIB:.2f} MiB, "
              f"distinct {len(tiles)}, duplicates {(n - len(tiles)) * 128 / MIB:.2f} MiB")

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    r = regions(zipfile.ZipFile(sys.argv[1]))
    total = sum(len(v) for v in r.values())
    print(f"{sys.argv[1]}: {total / MIB:.2f} MiB in P/M/V/S/C")
    for k in "PMVSC":
        if k in r:
            report(k, r[k])

main()
