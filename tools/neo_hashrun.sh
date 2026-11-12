#!/bin/bash
# neo_hashrun.sh -- REC/hash and REC/pic lines of a host build, game by game,
# to compare two builds of the core (a change that must not alter the
# emulation or the picture).
#
#   tools/neo_hashrun.sh <host binary> <dir> <out dir> <frames> <game> [game ...]
#   diff -r <out dir A> <out dir B>        # empty = the same machine and picture
#
# In <dir> (the games' zips, the BIOS: neogeo.zip or bios/neogeo/), runs the
# binary unthrottled from power-on for <frames> per game, all in parallel:
# with <dir>/<game>.rec it replays it, else it hashes the attract mode (no
# input; then <game>.rec is written in a private copy of the directory).
# Writes <out dir>/<game>.txt (the REC lines, host-only pic= included).
set -u
BIN=$(readlink -f "${1:?host binary}"); DIR=$(readlink -f "${2:?dir}"); OUT=${3:?out dir}; FRAMES=${4:?frames}
shift 4
mkdir -p "$OUT"; OUT=$(readlink -f "$OUT")
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEO_FAST=1 NEO_EXIT_FRAME="$FRAMES"
for g in "$@"; do
  w="$OUT/.run_$g"; mkdir -p "$w"
  for f in "$g.zip" neogeo.zip; do [ -e "$DIR/$f" ] && ln -sf "$DIR/$f" "$w/$f"; done
  [ -d "$DIR/bios" ] && ln -sfn "$DIR/bios" "$w/bios"
  if [ -e "$DIR/$g.rec" ]; then cp "$DIR/$g.rec" "$w/$g.rec"; mode=NEO_REPLAY; else mode=NEO_RECORD; fi
  ( cd "$w" && env "$mode=1" timeout 600 "$BIN" "$g.zip" 2>&1 | grep -E '^REC/(hash|pic)' > "$OUT/$g.txt" ) &
done
wait
for g in "$@"; do echo "$g: $(grep -c REC/hash "$OUT/$g.txt") hash lines"; done
