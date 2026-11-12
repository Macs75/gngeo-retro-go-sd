#!/usr/bin/env python3
"""Summarise a device PROF log (make PROF=1), optionally against another.

    neo_prof_sum.py LOG [BASELINE_LOG] [--from N]

Reads the PROF/ blocks (one per 120 frames: PROF/frame, PROF/budget,
PROF/emu, PROF/video, PROF/idle) and prints, over the windows from the
N-th on (default 2: the first two carry the boot's one-off loading cost),
the median and worst of busy (cycles per frame, and % of the 60 Hz
budget), the median of each phase, frames drawn and skipped by the
firmware. With a baseline, the same for it and the change. Windows of a
REPLAY line up by index between two logs (same frames), so the per-window
busy difference is printed too.
"""
import re
import statistics as st
import sys

KEYS = ('busy', 'z80', 'm68k', 'irq', 'audio', 'framectl', 'draw', 'present', 'wait', 'vwait', 'skipped')


def load(path):
    wins, cur = [], None
    for line in open(path, errors='replace'):
        if not line.startswith('PROF/'):
            continue
        kv = {k: int(v, 16) if k == 'head' else int(v)
              for k, v in re.findall(r'(\w+)=([0-9a-f]+)', line) if k == 'head' or v.isdigit()}
        if line.startswith('PROF/frame:'):
            cur = dict(kv)
            wins.append(cur)
        elif cur is not None:
            cur.update(kv)
    return [w for w in wins if 'budget' in w]


def summary(wins, frm):
    use = wins[frm:] or wins
    s = {'n': len(use), 'budget': use[0]['budget']}
    for k in KEYS:
        vals = [w[k] for w in use if k in w]
        if vals:
            s[k] = st.median(vals)
            s[k + '_max'] = max(vals)
    s['drawn'] = sum(w['drawn'] for w in use)
    s['frames'] = sum(w['frames'] for w in use)
    s['fw_skip'] = sum(w['fw_skip'] for w in use)
    s['heaviest'] = max(w['heaviest'] for w in use)
    return s


def show(name, s):
    b = s['budget']
    print('%s: %d windows, busy median %.2fM (%.0f%%), worst window %.2fM (%.0f%%), heaviest frame %.2fM'
          % (name, s['n'], s['busy'] / 1e6, 100 * s['busy'] / b, s['busy_max'] / 1e6,
             100 * s['busy_max'] / b, s['heaviest'] / 1e6))
    print('   drawn %d of %d, firmware skips %d' % (s['drawn'], s['frames'], s['fw_skip']))
    print('   median per frame: ' + ' '.join('%s=%.2fM' % (k, s[k] / 1e6) for k in KEYS[1:] if k in s))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    frm = int(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 2
    if '--from' in sys.argv:
        args = [a for a in args if a != str(frm)]
    a = load(args[0])
    sa = summary(a, frm)
    show(args[0], sa)
    if len(args) > 1:
        b = load(args[1])
        sb = summary(b, frm)
        show(args[1], sb)
        print('change, median busy: %+.2fM (%+.1f%% of the budget)'
              % ((sa['busy'] - sb['busy']) / 1e6, 100 * (sa['busy'] - sb['busy']) / sa['budget']))
        n = min(len(a), len(b))
        d = [a[i]['busy'] - b[i]['busy'] for i in range(frm, n)]
        if d:
            print('per window (same index): median %+.2fM, range %+.2fM .. %+.2fM over %d windows'
                  % (st.median(d) / 1e6, min(d) / 1e6, max(d) / 1e6, len(d)))


if __name__ == '__main__':
    main()
