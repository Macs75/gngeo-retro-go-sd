#!/usr/bin/env python3
"""neo_flash_fit.py -- does a Neo Geo set fit the firmware's flash cache?

  tools/neo_flash_fit.py <cache MiB> <piece KiB or 0 = whole regions> <order> p= m= v= s= c=

Simulates gw_flash_alloc.c's placement (retrogo-sd-rg2): a ring from a write
pointer, wrapping to the base when a blob does not fit before the end,
stepping over every blob mapped this session (the core's .ro, and each blob
as it is stored); a blob is one contiguous run. Sizes in bytes. For every
.ro position and every starting write pointer (on a 256 KiB grid), places
the blobs in <order> (letters: p m v s c; s = sfix + gfix, c = tiles + spr),
then the three BIOS blobs, and prints the share of cases where something
found no room, and the first blob that failed most often.
"""
import sys

MIB = 1 << 20
ERASE = 4096
RO = 76 * 1024                       # gngeo.ro, erase-aligned

def align(n):
    return (n + ERASE - 1) // ERASE * ERASE

def place(limit, live, ptr, size):
    p = ptr
    for _ in range(2 * len(live) + 4):
        if p + size > limit:
            p = 0
        hit = None
        for a, b in live:
            if p < b and a < p + size:
                hit = b
                break
        if hit is None:
            return p
        p = align(hit)
    return None

def run(cache, piece, order, sz):
    blobs = []
    def add(name, n, split):
        if split and piece and n > piece:
            k = 0
            while n > 0:
                blobs.append((f"{name}.{k}", min(piece, n))); n -= piece; k += 1
        else:
            blobs.append((name, n))
    for o in order:
        if o == "p": add("p", sz["p"], False)
        if o == "m": add("m", sz["m"], False)
        if o == "v": add("v", sz["v"], True)
        if o == "s": add("s", sz["s"], False); add("gfix", sz["s"] >> 5, False)
        if o == "c": add("c", sz["c"], True); add("spr", (sz["c"] >> 11) * 4, False)
    blobs += [("bios", 0x20000), ("sfix", 0x20000), ("lo", 0x20000)]
    limit = cache * MIB
    step = 256 * 1024
    fails, total, first = 0, 0, {}
    for ro in range(0, limit - RO, step):
        for p0 in range(0, limit, step):
            live = [(ro, ro + RO)]
            ptr = align(ro + RO) if p0 == ro else p0
            ok = True
            for name, n in blobs:
                at = place(limit, live, ptr, n)
                if at is None:
                    ok = False
                    key = name.split(".")[0]
                    first[key] = first.get(key, 0) + 1
                    break
                live.append((at, at + n))
                ptr = align(at + n)
            total += 1
            fails += not ok
    return fails / total, first

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    cache, piece, order = float(sys.argv[1]), int(sys.argv[2]) * 1024, sys.argv[3]
    sz = dict(a.split("=") for a in sys.argv[4:])
    sz = {k: int(v, 0) for k, v in sz.items()}
    f, first = run(int(cache), piece, order, sz)
    print(f"piece {piece // 1024 or 'whole'} KiB, order {order}: fails in {100 * f:.1f}% of placements"
          + (f" (first failure: {max(first, key=first.get)})" if first else ""))

main()
