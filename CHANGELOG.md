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
