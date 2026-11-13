#!/usr/bin/env python3
"""neo_prof_pair.py -- two device PROF logs of the same replay, window by
window, paired by the REC/hash frame that follows each window (so a log
whose first window also held a cache copy still lines up).

  tools/neo_prof_pair.py <A.log> <B.log>

Prints per window: tiles drawn (must match), sprites, draw and busy in
device cycles a frame for A and B, and the summary of B - A over the
windows with sprites.
"""
import re, sys, statistics

def windows(path):
    out, cur = {}, {}
    for line in open(path, errors="replace"):
        if line.startswith("PROF/frame:"):
            cur = {}
        for key, pat in (("busy", r"busy=(\d+)"), ("draw", r"\bdraw=(\d+)"),
                         ("spr", r"sprites=(\d+)"), ("tiles", r"full=(\d+)"),
                         ("m68k", r"m68k=(\d+)")):
            if line.startswith("PROF/"):
                m = re.search(pat, line)
                if m and key not in cur:
                    cur[key] = int(m.group(1))
        m = re.match(r"REC/hash frame=(\d+)", line)
        if m and cur:
            out[int(m.group(1))] = cur
            cur = {}
    return out

def main():
    a, b = windows(sys.argv[1]), windows(sys.argv[2])
    frames = sorted(set(a) & set(b))
    print("frame  tiles   sprites A -> B        draw A -> B          busy A -> B")
    ds, dd, db = [], [], []
    for f in frames:
        x, y = a[f], b[f]
        if not all(k in x and k in y for k in ("spr", "draw", "busy", "tiles")):
            continue
        flag = "" if x["tiles"] == y["tiles"] else "  tiles differ!"
        print(f"{f:5d} {x['tiles']:5d}  {x['spr']:8d} -> {y['spr']:8d} {100 * (y['spr'] - x['spr']) / max(x['spr'], 1):+5.1f}%"
              f"  {x['draw']:8d} -> {y['draw']:8d}  {x['busy']:8d} -> {y['busy']:8d}{flag}")
        if x["tiles"] >= 100 and f > 240:
            ds.append((y["spr"] - x["spr"]) / x["spr"])
            dd.append(y["draw"] - x["draw"])
            db.append(y["busy"] - x["busy"])
    if ds:
        print(f"windows with >= 100 tiles, after boot: {len(ds)}; sprites B vs A: median {100 * statistics.median(ds):+.1f}% "
              f"(range {100 * min(ds):+.1f}% .. {100 * max(ds):+.1f}%), draw median {statistics.median(dd):+.0f}, "
              f"busy median {statistics.median(db):+.0f} cycles a frame")

main()
