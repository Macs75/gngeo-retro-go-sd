# Changelog

## [Unreleased]

### Fixed

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
