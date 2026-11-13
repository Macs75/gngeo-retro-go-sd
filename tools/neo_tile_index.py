#!/usr/bin/env python3
"""neo_tile_index.py -- rewrite a Neo Geo zip with its sprite tiles indexed:
each distinct 128-byte tile stored once, and an index from tile number to
stored tile. For sets too big for the G&W's flash cache whose sprites
repeat tiles (rotd: 524,288 tiles, 244,259 distinct, 64 -> 31.3 MiB).

  tools/neo_tile_index.py <in.zip> <out.zip>

<in.zip>: a plain MAME set or one decrypted by tools/decrypt_neogeo_zip.
<out.zip>: every member except the sprite chips, plus
  tiles.uniq   the distinct tiles in order of first use, 128 bytes each, in
               the layout the core's loader builds from the chips (pair
               (c1,c2), (c3,c4)... byte-interleaved, pairs one after the
               other), before its tile conversion
  tiles.index  one uint32 (little-endian) per tile number: its tile in
               tiles.uniq
The core's zip loader recognises the two and draws through the index
(src/porting/neo_pieces.h). Sets whose sprite chips are 2 MiB are refused:
some of those carts map their chips in halves (the loader's
s_crom_split), a layout this tool does not reproduce, and none is big.
"""
import re, struct, sys, zipfile

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    zin = zipfile.ZipFile(sys.argv[1])
    names = zin.namelist()
    cs = []
    for n in names:
        m = re.search(r'[-_]c(\d+)\.c\d+$', n, re.I)
        if m:
            cs.append((int(m.group(1)), n))
    cs.sort()
    if len(cs) < 2 or len(cs) % 2:
        sys.exit(f"{sys.argv[1]}: {len(cs)} sprite chips, need pairs")
    if [i for i, _ in cs] != list(range(1, len(cs) + 1)):
        sys.exit(f"{sys.argv[1]}: sprite chips not numbered 1..{len(cs)}")
    tiles = bytearray()
    for k in range(0, len(cs), 2):
        a, b = zin.read(cs[k][1]), zin.read(cs[k + 1][1])
        if len(a) != len(b):
            sys.exit(f"pair {cs[k][1]}/{cs[k + 1][1]}: sizes differ")
        if len(a) == 0x200000:
            sys.exit(f"{cs[k][1]}: 2 MiB chips may be mapped in halves; not supported")
        t = bytearray(len(a) * 2)
        t[0::2], t[1::2] = a, b
        tiles += t
    first, uniq, index = {}, bytearray(), bytearray()
    for i in range(0, len(tiles), 128):
        x = bytes(tiles[i:i + 128])
        u = first.get(x)
        if u is None:
            u = first[x] = len(first)
            uniq += x
        index += struct.pack("<I", u)
    cnames = {n for _, n in cs}
    with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zout:
        for n in names:
            if n not in cnames:
                zout.writestr(n, zin.read(n))
        zout.writestr("tiles.uniq", bytes(uniq))
        zout.writestr("tiles.index", bytes(index))
    t, u = len(tiles) // 128, len(first)
    print(f"{sys.argv[2]}: {t} tiles, {u} distinct: sprites {len(tiles) / 1048576:.2f} MiB -> "
          f"{len(uniq) / 1048576:.2f} MiB + index {len(index) / 1048576:.2f} MiB")

main()
