#!/usr/bin/env python3
"""Write a scripted input file for the host's NEO_INPUT_RAW.

    neo_input_script.py OUT [frames] [seed] [--coin]

One little-endian uint16 per loop frame, the core's input word (after the
button mapping): A 0x01, B 0x02, C 0x04, D 0x08, up 0x10, down 0x20,
left 0x40, right 0x80, START 0x100, SELECT/coin 0x200. This writes: rest
to 599, (with --coin: SELECT for 6 frames at 600, for MVS), START for 6
frames at 800 and again at 1000 (menus), then from 1200 runs of 10-60
frames of one of the 8 directions or none, with A held through half the
runs and B/C/D tapped (4 on, 4 off) through a third. Record it with

    NEO_INPUT_RAW=OUT NEO_RECORD=1 NEO_EXIT_FRAME=N ./gngeo_host game.zip

which writes game.rec next to the ROM (the core's own header).
"""
import random
import struct
import sys

DIRS = [0x00, 0x10, 0x90, 0x80, 0xA0, 0x20, 0x60, 0x40, 0x50]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    out = args[0]
    frames = int(args[1]) if len(args) > 1 else 7200
    rnd = random.Random(int(args[2]) if len(args) > 2 else 1990)
    coin = '--coin' in sys.argv
    w = [0] * frames
    for f in range(frames):
        if coin and 600 <= f < 606:
            w[f] = 0x200
        if 800 <= f < 806 or 1000 <= f < 1006:
            w[f] = 0x100
    f = 1200
    while f < frames:
        n = min(rnd.randint(10, 60), frames - f)
        d = rnd.choice(DIRS)
        hold_a = rnd.randrange(2) == 0
        tap = rnd.choice([0, 0, 0x02, 0x04, 0x08]) if rnd.randrange(3) == 0 else 0
        for k in range(n):
            v = d | (0x01 if hold_a else 0)
            if tap and (k % 8) < 4:
                v |= tap
            w[f + k] = v
        f += n
    with open(out, 'wb') as fh:
        fh.write(struct.pack('<%dH' % frames, *w))


if __name__ == '__main__':
    main()
