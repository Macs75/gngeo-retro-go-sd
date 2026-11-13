#!/usr/bin/env python3
"""neo_dis68k.py -- disassemble part of a Neo Geo game's 68000 program.

    tools/neo_dis68k.py <game.zip> <start hex> <end hex> [more ranges ...]

Reads the P ROM from a MAME zip (the *-p1.p1 member, plus -p2 when the
address is in the $200000 bank window, bank 0) and disassembles with
capstone. A 2 MiB P1 is stored half-swapped (MAME ROM_LOAD16_WORD_SWAP at
$100000 + ROM_CONTINUE at 0): its second half is $000000-$0FFFFF. Words
are stored little-endian in the file. For the idle-skip work: the exact
instructions of a wait loop (NEO_IDLE_LOG=1 gives its addresses).
"""
import sys, zipfile
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2:
        print(__doc__); return 2
    z = zipfile.ZipFile(sys.argv[1])
    p1 = [n for n in z.namelist() if '-p1' in n.lower() or n.lower().endswith('.p1')][0]
    d = z.read(p1)
    if len(d) == 0x200000:
        d = d[0x100000:] + d[:0x100000]
    be = bytearray(len(d)); be[0::2] = d[1::2]; be[1::2] = d[0::2]
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    for k in range(2, len(sys.argv), 2):
        a0, a1 = int(sys.argv[k], 16), int(sys.argv[k + 1], 16)
        for i in md.disasm(bytes(be[a0:a1]), a0):
            print("%06x  %-10s %s" % (i.address, i.mnemonic, i.op_str))
        print()
    return 0

if __name__ == '__main__':
    sys.exit(main())
