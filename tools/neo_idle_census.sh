#!/bin/bash
# neo_idle_census.sh -- which 68000 wait loops each game spends its idle
# time in, and whether the idle skip jumps over them or runs their passes.
#
#   tools/neo_idle_census.sh <rom dir> <bios dir> <frames> <game> [game ...]
#
# Runs the host build unthrottled from power-on (attract mode, no input) with
# NEO_IDLESTAT=1 NEO_IDLE_LOG=1, all games in parallel, in a temporary
# directory; a game that has not finished after NEO_CENSUS_TIMEOUT seconds
# (default 120: encrypted sets never boot) is stopped. Per game and loop (BIOS loops at c1xxxx left out): the 68000
# cycles jumped over and run in place per frame, and the loop's
# instructions (tools/neo_dis68k.py on the traced addresses).
set -u
ROMS=$(readlink -f "${1:?rom dir}"); BIOS=$(readlink -f "${2:?bios dir}"); FRAMES=${3:?frames}; shift 3
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEO_FAST=1 NEO_EXIT_FRAME="$FRAMES" NEO_IDLESTAT=1 NEO_IDLE_LOG=1
for g in "$@"; do
  mkdir -p "$T/$g"; ln -sf "$ROMS/$g.zip" "$T/$g/$g.zip"
  ( cd "$T/$g" && timeout "${NEO_CENSUS_TIMEOUT:-120}" "$HERE/gngeo_host" --bios "$BIOS" "$g.zip" > log.txt 2>&1 ) &
done
wait
for g in "$@"; do
  echo "== $g"
  grep -a "^IDLESTAT" "$T/$g/log.txt" | grep -v "head c[0-9a-f]" | while read -r _ _ head _ lap _ _ jumped _ _ _ _ ran _; do
    awk -v h="$head" -v l="$lap" -v j="$jumped" -v r="$ran" -v f="$FRAMES" \
      'BEGIN { printf "  loop %s (%d cycles a pass): jumped %.0fK, run in place %.0fK 68000 cycles a frame\n", h, l, j/f/1000, r/f/1000 }'
    if [ "$ran" != 0 ]; then
      grep -a "IDLE: traced $head:" "$T/$g/log.txt" | head -1 | sed 's/.*pass: //' | tr ' ' '\n' | while read -r a; do
        python3 -I "$HERE/tools/neo_dis68k.py" "$ROMS/$g.zip" "$a" "$(printf %x $((0x$a + 10)))" 2>/dev/null | head -1 | sed 's/^/      /'
      done
    fi
  done
done
rm -rf "$T"
