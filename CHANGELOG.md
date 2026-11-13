# Changelog

## [Unreleased]

### Fixed

- Garbled sprites from zips of the carts whose 2 MiB sprite chips are
  mapped in two halves (Viewpoint, Fatal Fury 2, Art of Fighting, 3 Count
  Bout, King of the Monsters 2, Sengoku 2, Super Sidekicks, World Heroes):
  the zip loader laid every chip back to back; it now places the halves
  as MAME does (second half 0x400000 above the first, gaps empty). A copy
  cached by an older core is cached again on first launch (its size
  differs).
- Metal Slug X no longer stops at the copyright warning: its cart's
  protection chip is emulated as in current MAME (prot_mslugx.cpp, on
  $2FFFE0-$2FFFEF). The old GnGeo ROM patch never ran in this port, and
  could not: the program runs from read-only flash. Savestates are NEOS04
  (+ the chip's two registers); NEOS03 states still load. mslugx (63.2
  MiB) does not fit a stock 60 MiB flash cache.
- A missing .gno file is reported as "ROM file not found", a failed
  flash cache as "ROM caching failed" (both said "ROM too large for
  flash").
- One picture dropped once per session, ~70-106 s in (every device log
  longer than that: neodrift, fightfev, pspikes2, mslug2, sengoku3; a
  window ~14 ms short with vskip=1, never load). The
  firmware's pacer (common_emu_frame_loop) sums the measured frame time
  minus frame_time_10us and, past one frame, sets skip_frames, which also
  skips that frame's audio wait; the core set frame_time_10us to a rounded
  1679 for a 1679.40 frame (AES), so the sum drifted one frame in ~4,200
  frames. It now gets whole units chosen frame by frame whose mean is the
  exact frame time (simulated: under 4 units after 420,000 frames).
  Device, sengoku3_2: 8,760 of 8,760 frames drawn over 151 s.
- The sprite tiles and the YM2610 samples are cached as 512 KiB pieces,
  each a blob of its own (keys "<region>.<i>"; gw-arcade-core's
  rom_xform_chunks for the same reason), and the program, the largest blob
  still cached whole, is stored first. The flash cache stores a blob
  contiguously, and a big set left no run long enough for a whole region
  (preisle2: the 48 MiB sprites were placed, then no 6 MiB run for the
  samples, with 59.35 of 59.9 MiB needed). A run too short for the next
  piece stays empty, so the piece size bounds the waste:
  tools/neo_flash_fit.py simulates the firmware's placement for every .ro
  position and write pointer, preisle2 fails in 84% of them with 4 MiB
  pieces, 43% with 1 MiB, 0.0% with 512 KiB (~110 of the cache's 256
  entries). The renderer and the YM2610 find a byte through a table of
  piece addresses (src/porting/neo_pieces.h; the loader stores straight
  into it); a .gno, cached whole, fills it with addresses 512 KiB apart.
  Cached pieces are looked up in order up to the first missing one, and
  the rest are rebuilt. Device code: 5 instructions more per sprite tile
  drawn (~0.4% of a tile), the pixel loops and the YM2610 loop unchanged,
  ITCM +40 B. Host: tiles and samples as read (NEO_BLOB_SUM=1, READSUM),
  and 15 state/picture/audio hashes, identical to the previous build on
  pspikes2 (.neo), sengoku3_dec, preisle2_dec (96 + 12 pieces), viewpoin,
  mslug2, kotm2. The host flash cache holds 256 blobs like the firmware's
  (it held 64).
- A sprite or sample piece of one byte value throughout (unprogrammed
  EPROM space) is cached once for all games ("neogeo/fill/<value>.<size>")
  and shared; a region with such pieces gets a small map blob
  ("<key>.map") saying which, for the launches after. A piece is only
  written once a byte differs, so a fill piece costs no flash write.
  mslug4 (decrypted): 70.0 -> 58.1 MiB (25 sprite pieces of 0x00/0xFF,
  12.5 MiB, and one of its samples), so it fits a 60 MiB cache; preisle2
  59.35 -> 55.4 MiB. tools/neo_rom_space.py reports a set's padding and
  duplicates (mslug4: no non-blank aligned block repeats, even at 4 KiB;
  its other 5 MiB of duplicate tiles are single tiles).
  The size check before the first copy counts the sprites and the samples
  as one piece each (whether their pieces are shared is only known once
  the data is read): it refused mslug4 at 73.5 MB. A set that does not fit
  after all fails in the copy, with the same error.
- The fix layer is converted and stored in 8 KiB blocks (it was staged
  whole in RAM): the CMC sets' 512 KiB fix layer did not fit.
- Host: NEO_LOAD_TWICE=1 loads the game a second time from the cache just
  filled (the path of every launch after the first). Old and new builds
  read identical tiles and samples, and 15 state/picture/audio hashes, on
  pspikes2 (.neo), sengoku3_dec, preisle2_dec, mslug4_dec, viewpoin,
  mslug2, kotm2, both loads.
- The first launch's sprite copy reads the chips in blocks (up to 16 KiB
  of each chip at a time, sized to the RAM left) and writes each block to
  flash at once; it read 64 bytes from one chip then the other per tile.
  A zip stored uncompressed (as tools/decrypt_neogeo_zip writes them) reads
  straight from the card, so every tile cost two seeks: sengoku3_dec on the
  host, 537,216 reads + 524,315 seeks -> 10,880 + 2,075. The .neo loader
  reads blocks too (it read a tile at a time). Cached data identical
  (host NEO_BLOB_SUM=1: every blob of pspikes2 .neo, sengoku3_dec,
  viewpoin, mslug2, kotm2; the same sprites from zip and .neo; a flipped
  byte changes the sum).
- An encrypted MAME zip (sprite chips but no fix-layer chip: the CMC42/
  CMC50 sets, e.g. sengoku3, kof99, garou, mslug3) is reported as
  encrypted, with the PC tool to decrypt it (tools/decrypt_neogeo_zip)
  and the name to copy (<game>_dec.zip), before anything is written to
  flash; it said "zip missing p/m/s/v/c". The error screen fits a message
  of several lines.
- A game whose sprite chips are a large share of the flash could fail to
  cache ("ROM too large for flash") with the space free: the zip and .neo
  loaders stored the program, sound and fix chips first, and the firmware
  never erases a blob in use, so they could split the cache into runs too
  short for the sprite chips, which need one contiguous run (sengoku3:
  32 MiB after 17.4 MB, on 60 MiB). The sprite chips are stored first now,
  when only the core's own .ro is in use, and the other blobs are not
  looked up before that (a lookup marks a blob in use too: the copies a
  failed try left behind blocked the run again). Reproduced on gwemu with
  both older builds (same messages as the device), and the 32 MiB blob is
  placed from that state with the new one.

### Changed

- Real Neo Geo frame timing: 202,752 68000 cycles and 67,584 Z80 T-states
  per frame (was 200,000 / 73,333), the YM2610 timers and the calendar
  chip on the real frame length (1/59.1856 s; was 1/60 and 60 frames per
  second). The game runs at the System's frame rate: MVS 59.19 Hz, AES
  59.60 Hz, with the LCD refresh matched to it.
- Audio at the YM2610's own ADPCM-A rate, 8 MHz / 432 = 18518.5 Hz, with a
  firmware that has it (retro-go `set_audio_frequency` 18518), else
  18000 Hz; the core reads the rate the SAI really runs at to decide.
- The audio DMA starts at LCD scan line 32, just after the vertical
  blanking where a swap takes effect, so with the LCD locked to the loop
  every frame's draw has ~14 ms to swap: no session stuck at every other
  picture shown (30 fps).
- If 20 of 60 pictures are still dropped because the last swap is pending
  (anything that restarted the audio or moved the LCD clock), the core
  realigns the audio to the LCD (at most once per 600 frames).
- PROF/budget is the real frame time (was 1/60 s).
- 68000 instruction and extension-word fetches go through a cached page
  (gw-arcade-core's fetch-page cache): one compare instead of the page
  map lookup and handler test, page 0 included (its handler only
  overlays the vector table, data the CPU never executes). Device,
  mslug2 replay paired by window: 68000 -14 to -17% in every gameplay
  window (e.g. 2.60M -> 2.21M cycles a frame), the frame 91% -> 84% of
  budget at its heaviest; state hashes identical.
- YM2610: the silent-FM-channel test (all four slots in EG_OFF) once per
  update call instead of every sample (gw-arcade-core f5511ed); output
  identical (audio hashes, 10 games; a control that mutes one channel
  changes them all).
- YM2610 envelopes: the operator slots that are off (without SSG-EG) are
  set once per call and skipped by every envelope tick, and a channel
  whose four slots are off skips its ticks altogether. ADPCM-A is
  rendered channel by channel over the whole call (its state in
  registers) into two speaker sums the sample loop adds. Output
  identical (10 games; controls on each path change the audio).
- Sprite tiles (full width): each row's two words are loaded once, with
  word loads when the tile data is 4-byte aligned (it was 8 byte loads a
  row, for .gno payloads that may not be); a row with no transparent pixel
  skips the per-pixel test and stores two pixels at a time where aligned.
  Each path walks its own copy of the words with shifts behind an empty
  asm: GCC had computed all 16 pixels of both paths before the opaque
  test and spilled 13 of them every row (the first version of the fast
  path drew 17% SLOWER on the device for that reason; mslug2_11). Picture
  identical (10 games; controls on the pair, unaligned, per-pixel and
  flip paths change it). Device, mslug2 replay paired by window
  (mslug2_12): sprites -24% against the first version, drawing -8%
  against the build before any fast path (1,032K -> 941K a frame in
  gameplay, median), heaviest window 81% -> 80%; hashes identical.
- Narrow (x-zoomed) sprite tiles: the original per-position code (a
  skipped position two instructions, a kept one eight), on a row loaded
  once (word loads when aligned; it was 8 byte loads and 6 ORs a row),
  and empty rows skipped. Two faster-looking versions lost on the device
  and are not used: a list of the kept pixels (+20-25% on neodrift's
  narrow tiles, neodrift_7: a serial load -> shift -> load chain per
  pixel, a taken branch each) and a mask in a register (GCC predicated it:
  eight instructions for a skipped position too). Picture identical
  (neodrift 8,077 frames, 10 games; flip controls change it). Device,
  neodrift paired by window (neodrift_8 vs _6): sprites -5.0% median in
  the 33 windows with over 200 narrow tiles (every one -2.3..-6.9%), no
  change without them; hashes identical.
- The y-zoom table of a vertically shrunk tile is built by its own small
  function with the state in registers (9 instructions a step, each entry
  stored once); in draw_screen the counters sat on the stack with a byte
  load-add-store per step (~15 a step, ~240 a tile). Same arithmetic;
  picture identical (neodrift, 10 games; a control changes it). Device,
  neodrift_9 vs _8: sprites -4.3% median over the race windows (-6.0% in
  the narrow-heavy ones, per-window spread wide), heaviest window 74% ->
  72%; hashes identical.
- Sprite tiles crossing the screen edge are clipped in place (rows
  outside skipped, a column test per drawn pixel). They were drawn whole
  into a 16x16 scratch filled with a marker colour and copied out pixel
  by pixel: ~3,950-4,400 device cycles a tile against ~1,300 for a normal
  one (fitted on viewpoint_1 and neodrift_9; Viewpoint draws ~71 a
  frame). The old copy also dropped opaque pixels whose colour equalled
  the marker (0xF81F): neodrift shows 2,472 such pixels over its replay,
  now drawn. Host: NEO_CLIPTEST=1 compares old and new code on every
  clipped tile and on sampled tiles forced over all four edges of a test
  surface (the picture hash cannot see clipped tiles, and the host never
  clips left or right: its buffer has a margin). Identical apart from the
  marker pixels (neodrift, Viewpoint, mslug2; 12M forced draws); six
  controls fail it. Device, Viewpoint attract paired by window (tile
  counts identical in all 29): sprites -14.2% in the windows with clipped
  tiles (every one -10.9..-16.5%), ~1,370 cycles saved per clipped tile;
  68000 unchanged.
- PROF/vsplit: draw_screen's clear, sprites and fix layer per drawn frame.
- PROF/tiles (host NEO_TILESTAT=1): tiles per frame by path (full width,
  y-zoomed, narrow, clipped).
- ITCM: GnGeo's transparency-pack tile variants (50%/25% blended, an
  emulator add-on this port never loads) are compiled out (NEO_TRANSPACK=1
  restores them), and the YM2610/Z80 init and reset functions moved to RAM
  (.ram_cold): ITCM 65,312 -> 53,784 bytes, 11.7K free.
- PROF/ym: the YM2610 loop split (eg, fm, ssg, adpcmb, adpcma, mix), every
  other call instrumented so the log also shows the clean cost.
- PROF/cpi: device cycles per emulated 68000 and Z80 instruction
  (mslug2 gameplay: 68000 ~131 at -Os, Z80 ~104).
- The 68000 interpreter (m68kcpu.o) is built at -O2, the rest of the core
  stays -Os (gw-arcade-core catalogue §15a); +50.7K of RAM_EMU code
  (mslug2 boots with 44,216 B free). Device, mslug2 replay paired by
  window: 68000 median -12% over the 31 gameplay windows, 130.7 -> 114.9
  device cycles per instruction, the Z80 beside it -1.6%, the heaviest
  window 86% -> 82% of budget; state hashes identical.
- Recordings are version 2 and carry the audio rate; version 1
  recordings are refused (they predate the timing change).

### Added

- tools/decrypt_neogeo_zip writes a compressed zip (deflate; it stored
  the files): sengoku3_dec 51.0 -> 20.8 MB, preisle2_dec 62.1 -> 26.7 MB.
  The cached data is the same (host NEO_BLOB_SUM=1: all 97 blobs of
  sengoku3_dec identical, and 15 state/picture/audio hashes).
- `make host` and `make decrypt-zip` build with Ubuntu's gcc as they are:
  fortify is off in both (the repo's src/porting/strings.h #defines bcopy,
  which glibc's strings_fortified.h cannot compile; it took
  HOST_CC="gcc -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"), and the tool gets
  _GNU_SOURCE for nftw's FTW_DEPTH/FTW_PHYS.
- Host wait-loop census: NEO_IDLESTAT=1 prints, per 68000 wait loop, the
  cycles the idle skip jumped over and the cycles it ran pass by pass
  (IDLESTAT, at exit) and the game's own loops per frame (IDLEFRAME).
  tools/neo_idle_census.sh runs it over a ROM folder (loops and their
  code), tools/neo_idle_frames.py gives the share left in the busiest
  frames, tools/neo_dis68k.py disassembles a zip's P ROM.

### Parked: wait loops in closed form (measured, not implemented)

When a wait loop's pass changes the machine, the idle skip runs the
passes one by one (~125 device cycles per 68000 instruction, like the
interpreter). Census, 138 games, 2 minutes of attract each: 101 wait in a
game loop after boot, 88 of them are jumped over at once, 12 run in place
(33-124K of the 202,752 68000 cycles a frame):

- Counter loops, a closed form is possible (N passes = one pass plus N
  times a constant on the counter): aof, aof2, fightfev, turfmast (a
  constant added to a memory word, then a flag test), sengoku2, kotm2 (the
  counter in a register), doubledr (a second counter ends the loop after
  N passes), mslug (plus a byte cleared each pass). pspikes2's pass has no
  counter, only stores of the same value and two calls: the existing
  check could jump over it if it accepted them.
- Loops that must be iterated (a random-number step each pass): neodrift
  and ctomaday (x += rol(x,2) + $3711), flipshot (another formula).

Device gameplay, recorded (AES, 340 MHz):

- fightfev_1: the counter loop runs through every fight, 1.1-1.5M device
  cycles a frame (20-26% of the frame); the closed form would take busy
  from ~68% to ~45%. But no frame is dropped (4,319 of 4,320 drawn,
  worst window 72%, heaviest frame 88%).
- pspikes2_1: the census loop (000220) runs in the menus only. In the
  match the main loop (0004ea) calls the game's whole state routine
  (2aea2: joypads, a jump table, link/movem, nested calls) ~100 times a
  frame until a counter changes; the skip refuses it (~100 rejects a
  window) and the 68000 costs 1.97M a frame. No frame dropped (6,839 of
  6,840, worst window 81%, heaviest frame 96%).

Neither game is short of time, so the closed form would only add
headroom. Worth doing for a game that drops frames while one of these
loops is running in place; the census and tools above find it.

## [v0.0.5]

### Added

- Load TerraOnion / NeoSD `.neo` carts from `/roms/neogeo/*.neo` (same QSPI
  cache path as MAME zip).

### Changed

- Nothing.

### Fixed

- Move PVC code/data to .ro to free some ram to fit PVC data (fix Metal Slug 5 & other PVC games).
- Mirror sprite tile numbers into the C ROM size (MAME-style) instead of
  skipping out-of-range tiles — fixes black screen after Robocop’s options
  menu (1 MiB C cart).

### Install

Release assets (attached to this GitHub release):

- `gngeo-vx.x.x.zip` — SD install archive. Unzip onto the **root** of the SD
  card (creates `/cores/gngeo.bin` and `/cores/gngeo.ro`).

Also required on the SD card (not in the zip):

- ROMs: `/roms/neogeo/*.gno` (XIP dumps via `tools/make_gno_xip.py`) and/or
  unencrypted MAME `/roms/neogeo/*.zip`. Encrypted MAME sets must be decrypted
  first with `tools/decrypt_neogeo_zip` (e.g. `mslug5_dec.zip`).
- BIOS (shared): `/bios/neogeo/uni-bios.rom` (or `uni-bios_4_0.rom` / `sp-s2.sp1`),
  `sfix.sfix` (or `sfix.sfx`), and `000-lo.lo`.

Firmware must match the ABI recorded in `SDK_VERSION` in this repository
(`FIRMWARE_ABI_VERSION=2` at this release).
