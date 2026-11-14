# Changelog

## [v0.0.7] - 2026-11-14

### Fixed

- Scrambled sprites in Art of Fighting, Viewpoint, Fatal Fury 2, King of
  the Monsters 2, Sengoku 2, 3 Count Bout, Super Sidekicks and World
  Heroes from romsets other than MAME's (e.g. NeoRAGE's): their sprite
  chips come with the two halves already in place, and v0.0.6 moved them
  again. The halves are now moved only for MAME's chips, recognised by
  their CRC; any other dump is laid out as it is. A scrambled copy left
  in the flash cache is not reused.

### Install

Release assets (attached to this GitHub release):

- `gngeo-vx.x.x.zip` — SD install archive. Unzip onto the **root** of the SD
  card (creates `/cores/gngeo.bin` and `/cores/gngeo.ro`).

Also required on the SD card (not in the zip):

- ROMs: `/roms/neogeo/*.gno` (XIP dumps via `tools/make_gno_xip.py`), `.neo`
  carts and/or `/roms/neogeo/*.zip` (MAME sets, or other dumps such as
  NeoRAGE's). Encrypted MAME sets must be decrypted first on a PC with
  `tools/decrypt_neogeo_zip` (e.g. `mslug5_dec.zip`); sets too big for the
  flash cache can then be indexed with `tools/neo_tile_index.py` (e.g.
  `mslug5_idx.zip`).
- BIOS (shared): `/bios/neogeo/uni-bios.rom` (or `uni-bios_4_0.rom` / `sp-s2.sp1`),
  `sfix.sfix` (or `sfix.sfx`), and `000-lo.lo`.

Firmware must match the ABI recorded in `SDK_VERSION` in this repository
(`FIRMWARE_ABI_VERSION=2` at this release).

## [v0.0.6] - 2026-11-13

### Added

- **Big games now fit the 60 MiB flash cache.** Sprites and sound samples
  are cached in 512 KiB pieces, and pieces that are only padding (one byte
  value throughout) are stored once and shared. Fit this way: Metal Slug 4,
  The Last Blade, Prehistoric Isle 2, Real Bout Fatal Fury 2.
- **Indexed sprites** for sets that are still too big: on a PC,
  `tools/neo_tile_index.py` rewrites a zip so each distinct sprite tile is
  stored once, with an index. Fit this way: Metal Slug X, Metal Slug 5,
  Strikers 1945 Plus, Rage of the Dragons. `tools/neo_fit_report.py` tells
  which method a given set needs.
- **Record and replay input** from the pause menu ("Record input" /
  "Replay input"): the game restarts from power-on and the inputs are saved
  next to the ROM (`<game>.rec`); a replay plays them back exactly, for
  reproducing a problem or comparing performance.
- The YM2610's own audio rate (18518 Hz) with a firmware that supports it
  (sylverb/game-and-watch-retro-go-sd#128); 18000 Hz otherwise.
- 512 KiB fix layers (Metal Slug 4 and other CMC50 sets).
- `tools/decrypt_neogeo_zip` writes compressed zips (about 60% smaller).
- From sylverb's repository after the v0.0.5 release: TerraOnion / NeoSD
  `.neo` carts, and configurable A/B and C/D button swaps (julenvitoria).

### Changed

- Real Neo Geo timing: the real number of CPU cycles per frame (202,752 for
  the 68000, 67,584 for the Z80; it was 200,000 and 73,333 at 60 Hz), at a
  frame rate close to the real System's (AES 59.6 Hz, MVS 59.19 Hz), with
  the LCD refresh matched to it.
- The sound CPU (Z80) is kept in step with the main CPU. Games that wait
  for the sound CPU no longer stutter: Strikers 1945 Plus now runs smoothly.
- Faster emulation: the wait loops of both CPUs are skipped, the 68000
  interpreter is about 25% faster (Metal Slug 2), sprite drawing up to 14%
  faster (opaque rows, zoomed sprites, sprites at the screen edges), and the
  YM2610 sound chip is faster too.

### Fixed

- Garbled sprites in Viewpoint and the other carts whose sprite chips are
  mapped in two halves (Fatal Fury 2, Art of Fighting, 3 Count Bout, King
  of the Monsters 2, Sengoku 2, Super Sidekicks, World Heroes).
- Metal Slug X stopping at the copyright warning (its protection chip is
  now emulated).
- Big games failing with "ROM too large for flash" although they fit
  (e.g. Sengoku 3).
- Clearer errors: "ROM file not found" and "ROM caching failed" (both said
  "ROM too large for flash"), and a message for encrypted zips telling how
  to decrypt them (it said "zip missing p/m/s/v/c").
- Robocop: black screen after the options menu (sylverb, after v0.0.5).

### Install

Release assets (attached to this GitHub release):

- `gngeo-vx.x.x.zip` — SD install archive. Unzip onto the **root** of the SD
  card (creates `/cores/gngeo.bin` and `/cores/gngeo.ro`).

Also required on the SD card (not in the zip):

- ROMs: `/roms/neogeo/*.gno` (XIP dumps via `tools/make_gno_xip.py`), `.neo`
  carts and/or MAME `/roms/neogeo/*.zip`. Encrypted MAME sets must be
  decrypted first on a PC with `tools/decrypt_neogeo_zip` (e.g.
  `mslug5_dec.zip`); sets too big for the flash cache can then be indexed
  with `tools/neo_tile_index.py` (e.g. `mslug5_idx.zip`).
- BIOS (shared): `/bios/neogeo/uni-bios.rom` (or `uni-bios_4_0.rom` / `sp-s2.sp1`),
  `sfix.sfix` (or `sfix.sfx`), and `000-lo.lo`.

Firmware must match the ABI recorded in `SDK_VERSION` in this repository
(`FIRMWARE_ABI_VERSION=2` at this release).

## [v0.0.5]

### Added

- Nothing.

### Changed

- Nothing.

### Fixed

- Move PVC code/data to .ro to free some ram to fit PVC data (fix Metal Slug 5 & other PVC games).

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
