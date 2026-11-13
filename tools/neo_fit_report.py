#!/usr/bin/env python3
"""neo_fit_report.py -- does a Neo Geo set fit the G&W's flash cache, and
with which scheme: as is, blank pieces shared, indexed tiles, indexed
half-tiles.

  tools/neo_fit_report.py [--cache MIB] <game.zip> [...]

<game.zip>: a plain MAME set or one decrypted by tools/decrypt_neogeo_zip.
Sizes in MiB of flash, regions as the core caches them:
  as is       P + M + V + S + fix usage + C + sprite usage
  blank       512 KiB pieces of C and V of one byte value stored once
              (the core's shared fill pieces)
  tiles       C indexed (tools/neo_tile_index.py): distinct tiles + a
              4-byte index entry per tile; V with blank pieces shared
  half-tiles  C as distinct 8x16 halves + two 3-byte index entries per tile
              (not implemented: what it would take)
--cache: the space a game may use (default 59.4: the 60 MiB cache less
the BIOS and the core's .ro). A set fits when its scheme's size is below.
The default is a little conservative: rbff2, reported at 59.5 with blank
pieces, cached and ran on the device as is. Try a borderline set as is
before indexing it.
"""
import re, sys, zipfile
import numpy as np

MIB = 1 << 20
PIECE = 512 * 1024

def chip_index(n):
    m = re.search(r'(\d+)\.[a-z0-9]+$', n, re.I)
    return int(m.group(1)) if m else 0

def regions(path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    def pick(letter):
        pat = re.compile(r'[-_]%s[a-z]*\d+\.[a-z]*%s?\d*$' % (letter, letter), re.I)
        return sorted([n for n in names if pat.search(n)], key=chip_index)
    r = {}
    for letter in "pmvs":
        r[letter] = b"".join(z.read(n) for n in pick(letter))
    cs = pick("c")
    c = bytearray()
    for i in range(0, len(cs) - 1, 2):
        a, b = z.read(cs[i]), z.read(cs[i + 1])
        t = bytearray(len(a) * 2)
        t[0::2], t[1::2] = a, b
        c += t
    r["c"] = bytes(c)
    return r

def blank_repeats(d):
    """bytes of 512 KiB pieces of one value beyond the first of each value"""
    seen, rep = set(), 0
    for i in range(0, len(d), PIECE):
        p = d[i:i + PIECE]
        if p.count(p[0]) == len(p):
            key = (p[0], len(p))
            if key in seen:
                rep += len(p)
            seen.add(key)
    return rep

def main():
    args = sys.argv[1:]
    cache = 59.4
    if args[:1] == ["--cache"]:
        cache, args = float(args[1]), args[2:]
    if not args:
        sys.exit(__doc__)
    print(f"{'game':10s} {'as is':>6s} {'blank':>6s} {'tiles':>6s} {'half':>6s}   fits {cache} MiB with")
    for path in args:
        r = regions(path)
        p, m, v, s, c = (len(r[k]) for k in "pmvs") + (len(r["c"]),) if False else \
            (len(r["p"]), len(r["m"]), len(r["v"]), len(r["s"]), len(r["c"]))
        base = p + m + s + s // 32
        tiles = c // 128
        spr = (tiles >> 4) * 4
        v_blank = v - blank_repeats(r["v"])
        as_is = base + v + c + spr
        blank = base + v_blank + c - blank_repeats(r["c"]) + spr
        t = np.frombuffer(r["c"], dtype=np.uint8).reshape(-1, 128)
        u = np.unique(t, axis=0)
        uspr = ((len(u) + 15) >> 4) * 4
        indexed = base + v_blank + len(u) * 128 + tiles * 4 + uspr
        halves = np.unique(np.concatenate([u[:, :64], u[:, 64:]]), axis=0)
        half = base + v_blank + len(halves) * 64 + tiles * 2 * 3 + uspr
        sizes = [("as is", as_is), ("blank pieces", blank), ("tiles", indexed), ("half-tiles", half)]
        fit = next((name for name, sz in sizes if sz / MIB <= cache), "nothing")
        name = re.sub(r'(_dec)?\.zip$', '', path.split('/')[-1])
        print(f"{name:10s} {as_is / MIB:6.1f} {blank / MIB:6.1f} {indexed / MIB:6.1f} {half / MIB:6.1f}   {fit}",
              flush=True)

main()
