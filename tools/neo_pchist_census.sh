#!/bin/bash
# neo_pchist_census.sh -- where the 68000 spends its cycles, game by game.
#
#   tools/neo_pchist_census.sh <out dir> <rom dir> [frames] [game ...]
#
# Runs the host build (./gngeo_host, `make host`) unthrottled (NEO_FAST)
# from power-on for <frames> (default 3600, the attract mode) with
# NEO_PCHIST, all games in parallel, then prints each game's top 6 PCs:
# 68000 cycles, share of the run, and the code words at that PC (to read
# the loop). The out dir needs the BIOS (neogeo.zip) or a bios/neogeo/;
# the games are copied there. Without game names, every .zip in <rom dir>
# but neogeo.zip. Encrypted sets need decrypting first (README).
set -u
OUT=${1:?out dir}; ROMS=${2:?rom dir}; FRAMES=${3:-3600}; shift $(( $# < 3 ? $# : 3 ))
HOST=$(cd "$(dirname "$0")/.." && pwd)/gngeo_host
mkdir -p "$OUT"
if [ $# -gt 0 ]; then GAMES=("$@"); else
  GAMES=(); for z in "$ROMS"/*.zip; do b=$(basename "$z" .zip); [ "$b" = neogeo ] || GAMES+=("$b"); done
fi
cd "$OUT" || exit 1
for g in "${GAMES[@]}"; do [ -f "$g.zip" ] || cp "$ROMS/$g.zip" . ; done
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
for g in "${GAMES[@]}"; do
  ( NEO_PCHIST=1 NEO_PCHIST_TOP=8 NEO_FAST=1 NEO_EXIT_FRAME="$FRAMES" \
      timeout 300 "$HOST" "$g.zip" > "$g.log" 2>&1 ) &
done
wait
for g in "${GAMES[@]}"; do
  if grep -q "^PCHIST: 68000" "$g.log"; then
    echo "== $g $(grep '^PCHIST: 68000' "$g.log" | sed 's/PCHIST: //')"
    grep "^PCHIST: [0-9a-f]" "$g.log" | head -6
  else
    echo "== $g: no histogram (did not boot? see $OUT/$g.log)"
  fi
done
