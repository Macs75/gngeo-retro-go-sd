#!/usr/bin/env python3
"""gwemu_neogeo.py -- run the Neo Geo core in gwemu (the full G&W firmware,
headless): install it on a copy of a gwemu run directory's SD card, boot,
drive the launcher with key presses, keep the firmware's log, take
screenshots.

    tools/gwemu_neogeo.py <work dir> [--prepare <gwemu run dir>]
        [--core <dir with gngeo.bin, gngeo.ro>] [--rom <game.zip> ...]
        [--rec <game.rec> ...] [--bios <dir: uni-bios.rom sfix.sfx 000-lo.lo>]
        [--keys "8:right,1:x,..."] [--seconds 120] [--shots 10]

--prepare copies the run dir's firmware banks and flash image and its SD
image (sparse) into <work dir> (once). --core/--rom/--rec/--bios install
files on the copy's card (/cores, /roms/neogeo, /bios/neogeo). --tab NAME
(e.g. neogeo): once the launcher is up, press up until NAME's list is the
one shown (the log's last "Resized list" line), then A, and A again 4 s
later (the game's "New game"), before --keys. --keys:
comma-separated <seconds after the previous one>:<qcode> presses (gwemu
keys: x = A, z = B, arrows = d-pad, ret = START, ...). The run stops after
--seconds or when the log reports a fault. The log goes to
<work dir>/run.log, screenshots every --shots seconds to <work dir>/shot_N.ppm.
gwemu must not already be running on the QMP ports (4490, 4491).
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, tempfile, time

GWEMU = os.path.expanduser("~/retro/gwemu/gwemu.AppImage")
LOG_ADDR = 0x30000004          # log_idx, then the 4K ring (.persistent in AHB SRAM)
RING = 4096
P_CTL, P_LOG = 4490, 4491
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")
FAULT = re.compile(r"[Mm]emfault|HardFault|MemManage|BusFault|UsageFault|Fault at|crash", re.I)


def mt(*a, check=True):
    return subprocess.run(list(a), env=ENV, capture_output=True, text=True, check=check)


def qmp(port, tries=60):
    for _ in range(tries):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=30)
            break
        except OSError:
            time.sleep(0.5)
    else:
        raise RuntimeError("no QMP on %d" % port)
    f = s.makefile("rw")
    json.loads(f.readline())

    def cmd(c, a=None):
        m = {"execute": c}
        if a:
            m["arguments"] = a
        f.write(json.dumps(m) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                return r
    cmd("qmp_capabilities")
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--prepare")
    ap.add_argument("--core")
    ap.add_argument("--rom", action="append", default=[])
    ap.add_argument("--rec", action="append", default=[])
    ap.add_argument("--bios")
    ap.add_argument("--keys", default="")
    ap.add_argument("--tab")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--shots", type=float, default=10)
    a = ap.parse_args()
    w = a.work
    os.makedirs(w, exist_ok=True)
    if a.prepare:
        for f in ("qemu_bank1.bin", "qemu_bank2.bin", "extflash.bin"):
            shutil.copy(os.path.join(a.prepare, f), w)
        subprocess.run(["cp", "--sparse=always", os.path.join(a.prepare, "sdcard.img"),
                        os.path.join(w, "sdcard.img")], check=True)
    img = os.path.join(w, "sdcard.img") + "@@1M"
    for d in ("::/roms/neogeo", "::/bios/neogeo"):
        mt("mmd", "-i", img, d, check=False)
    if a.core:
        for f in ("gngeo.bin", "gngeo.ro"):
            mt("mcopy", "-o", "-i", img, os.path.join(a.core, f), "::/cores/")
    for z in a.rom + a.rec:
        mt("mcopy", "-o", "-i", img, z, "::/roms/neogeo/")
    if a.bios:
        for f in os.listdir(a.bios):
            mt("mcopy", "-o", "-i", img, os.path.join(a.bios, f), "::/bios/neogeo/")

    keys = []
    for k in [k for k in a.keys.split(",") if k]:
        t, q = k.split(":")
        keys.append((float(t), q))
    logf = os.path.join(w, "run.log")
    open(logf, "wb").close()
    p = subprocess.Popen([GWEMU, "-M", "gnw-h7b0",
                          "-global", "gnw-h7b0-soc.bank1-image=qemu_bank1.bin",
                          "-global", "gnw-h7b0-soc.bank2-image=qemu_bank2.bin",
                          "-global", "gnw-h7b0-soc.extflash-image=extflash.bin",
                          "-drive", "if=sd,format=raw,file=sdcard.img", "-display", "none",
                          "-qmp", "tcp:127.0.0.1:%d,server,nowait" % P_CTL,
                          "-qmp", "tcp:127.0.0.1:%d,server,nowait" % P_LOG],
                         cwd=w, stdout=open(os.path.join(w, "gwemu.out"), "w"), stderr=subprocess.STDOUT)
    try:
        ctl = qmp(P_CTL)
        log = qmp(P_LOG)
        ctl("cont")
        tmp = os.path.join(tempfile.mkdtemp(), "ring.bin")
        last, text, t0 = None, b"", time.time()
        tab_state = 0 if a.tab else 9     # 0 wait launcher, 1 moving, 2 A pressed, 9 done
        t_tab = 0
        next_key = 1e18 if a.tab else t0 + (keys[0][0] if keys else 1e9)
        ki, shot, next_shot = 0, 0, t0 + a.shots
        while time.time() - t0 < a.seconds:
            log("pmemsave", {"val": LOG_ADDR, "size": 4 + RING, "filename": tmp})
            d = open(tmp, "rb").read()
            idx = int.from_bytes(d[:4], "little")
            ring = d[4:]
            if idx <= RING:
                if last is None:
                    new = ring[:idx]
                elif idx >= last:
                    new = ring[last:idx]
                else:
                    end = ring.find(b"\0", last)
                    new = ring[last:end if end >= 0 else RING] + ring[:idx]
                last = idx
                if new:
                    text += new
                    with open(logf, "ab") as f:
                        f.write(new)
            now = time.time()
            s_all = text.decode("latin-1")
            if tab_state < 9:
                lists = re.findall(r"Resized list '([^']+)'", s_all)
                if tab_state == 0 and lists and now - t0 > 25:
                    tab_state, t_tab = 1, now
                elif tab_state == 1 and now - t_tab > 1.5:
                    if lists and lists[-1] == a.tab:
                        ctl("send-key", {"keys": [{"type": "qcode", "data": "x"}], "hold-time": 120})
                        print("%6.1fs on tab %s: A" % (now - t0, a.tab), flush=True)
                        tab_state, t_tab = 2, now
                    else:
                        ctl("send-key", {"keys": [{"type": "qcode", "data": "up"}], "hold-time": 120})
                        t_tab = now
                elif tab_state == 2 and now - t_tab > 4:
                    if "Starting game" not in s_all and "app_main" not in s_all:
                        ctl("send-key", {"keys": [{"type": "qcode", "data": "x"}], "hold-time": 120})
                        print("%6.1fs A (New game)" % (now - t0), flush=True)
                    tab_state = 9
                    next_key = now + (keys[0][0] if keys else 1e9)
            if ki < len(keys) and now >= next_key:
                ctl("send-key", {"keys": [{"type": "qcode", "data": keys[ki][1]}], "hold-time": 120})
                print("%6.1fs key %s" % (now - t0, keys[ki][1]), flush=True)
                ki += 1
                next_key = now + (keys[ki][0] if ki < len(keys) else 1e9)
            if now >= next_shot:
                ctl("screendump", {"filename": os.path.join(w, "shot_%d.ppm" % shot)})
                shot += 1
                next_shot = now + a.shots
            if FAULT.search(new.decode("latin-1") if idx <= RING and last is not None else ""):
                print("fault in the log", flush=True)
                time.sleep(2)
                break
            time.sleep(0.3)
        ctl("screendump", {"filename": os.path.join(w, "shot_end.ppm")})
    finally:
        p.terminate()
        try:
            p.wait(20)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    main()
