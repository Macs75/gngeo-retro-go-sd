#!/usr/bin/env python3
"""neo_idle_frames.py -- per game, how much of each frame its 68000 wait
loops take, frame by frame, to see what is left in the BUSIEST frames
(the attract-mode demo play) rather than the average over title screens.

  tools/neo_idle_frames.py <rom dir> <bios dir> <frames> <game> [game ...]

Runs the host build unthrottled from power-on with NEO_IDLESTAT=1 (one
"IDLEFRAME <run in place> <jumped>" line per frame, the game's own loops
only, BIOS loops left out), all games in parallel in a temporary
directory, each stopped after NEO_CENSUS_TIMEOUT seconds (default 120).

Frames counted: from the first frame the game's loop is seen. A frame with
no loop at all is busy for the whole frame or waits in a loop the skip does
not recognise (it can't tell which); it is counted apart. For the frames
with a loop, the share of the frame's 202,752 68000 cycles run in place
(the runner's cost) at the median, and at the 10% and 1% of frames where
the most is left for the game (smallest loop time = busiest frames).
"""
import os, subprocess, sys, tempfile

FRAME = 202752

def run(roms, bios, frames, games):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               NEO_FAST="1", NEO_EXIT_FRAME=str(frames), NEO_IDLESTAT="1")
    tmo = os.environ.get("NEO_CENSUS_TIMEOUT", "120")
    tmp = tempfile.mkdtemp()
    procs = []
    for g in games:
        d = os.path.join(tmp, g)
        os.mkdir(d)
        os.symlink(os.path.join(roms, g + ".zip"), os.path.join(d, g + ".zip"))
        out = open(os.path.join(d, "log.txt"), "w")
        procs.append((g, d, out, subprocess.Popen(
            ["timeout", tmo, os.path.join(here, "gngeo_host"), "--bios", bios, g + ".zip"],
            cwd=d, env=env, stdout=out, stderr=subprocess.STDOUT)))
    res = []
    for g, d, out, p in procs:
        p.wait()
        out.close()
        res.append((g, open(os.path.join(d, "log.txt"), errors="replace").read()))
        subprocess.run(["rm", "-rf", d])
    os.rmdir(tmp)
    return res

def pct(sorted_vals, q):
    return sorted_vals[min(len(sorted_vals) - 1, int(q * len(sorted_vals)))]

def report(g, log):
    fr = []
    for line in log.splitlines():
        if line.startswith("IDLEFRAME "):
            _, ran, jumped = line.split()
            fr.append((int(ran), int(jumped)))
    first = next((i for i, (r, j) in enumerate(fr) if r or j), None)
    if first is None:
        print(f"{g:10s} no game loop seen ({len(fr)} frames)")
        return
    after = fr[first:]
    withloop = [r + j for r, j in after if r or j]
    ran = sorted(r for r, j in after if r or j)
    none = len(after) - len(withloop)
    # busiest frames = least loop time in total (run + jumped)
    order = sorted(range(len(withloop)), key=lambda i: withloop[i])
    rl = [r for r, j in after if r or j]
    def busy(q):
        k = max(1, int(q * len(order)))
        return sum(rl[i] for i in order[:k]) / k
    print(f"{g:10s} frames {len(after):5d}  no loop {100*none/len(after):3.0f}%  "
          f"run in place: median {100*pct(ran,0.5)/FRAME:3.0f}%  "
          f"busiest 10% {100*busy(0.10)/FRAME:3.0f}%  busiest 1% {100*busy(0.01)/FRAME:3.0f}%")

def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    roms, bios, frames = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), int(sys.argv[3])
    for g, log in run(roms, bios, frames, sys.argv[4:]):
        report(g, log)

main()
