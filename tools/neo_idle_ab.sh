#!/bin/bash
# neo_idle_ab.sh -- an idle skip against no skip, game by game.
#
#   tools/neo_idle_ab.sh <dir> <frames> <game> [game ...]
#
# In <dir> (the games' zips and the BIOS, neogeo.zip or bios/neogeo/),
# runs the host build (./gngeo_host) unthrottled twice per game, with
# NEO_NOIDLE=1 and without, from power-on for <frames>: with a recording
# <game>.rec in <dir> it replays it (NEO_REPLAY), else it hashes the
# attract mode (NEO_RECORD, no input; it writes <dir>/<game>.rec, so keep
# a real recording elsewhere or replay it). Per game: REC/hash lines that
# differ (must be 0), the 68000 cycles executed both ways (NEO_PCHIST) and
# the share saved. Exit status 1 if any game's hashes differ.
#
# AB_CPU=z80: the Z80 idle skip instead (off: NEO_NOZ80IDLE, cycles: the
# Z80's executed T-states, NEO_Z80HIST). Default AB_CPU=68k.
set -u
DIR=${1:?dir}; FRAMES=${2:?frames}; shift 2
HOST=$(cd "$(dirname "$0")/.." && pwd)/gngeo_host
cd "$DIR" || exit 1
mkdir -p ab_off ab_on
if [ "${AB_CPU:-68k}" = z80 ]; then
  OFF=NEO_NOZ80IDLE; HIST=NEO_Z80HIST; TOTAL='Z80HIST: T-states total=[0-9]*'
else
  OFF=NEO_NOIDLE; HIST=NEO_PCHIST; TOTAL='PCHIST: 68000 cycles total=[0-9]*'
fi
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NEO_FAST=1 NEO_EXIT_FRAME="$FRAMES" NEO_PCHIST_TOP=1
export "$HIST=1"
for g in "$@"; do
  for m in off on; do
    for f in "$g.zip" neogeo.zip "$g.rec"; do [ -e "$f" ] && ln -sf "../$f" "ab_$m/$f"; done
    [ -d bios ] && ln -sfn ../bios "ab_$m/bios"
  done
  mode=NEO_RECORD; [ -e "$g.rec" ] && mode=NEO_REPLAY
  ( cd ab_off && env "$mode=1" "$OFF=1" timeout 600 "$HOST" "$g.zip" > "$g.log" 2>&1 ) &
  ( cd ab_on && env "$mode=1" timeout 600 "$HOST" "$g.zip" > "$g.log" 2>&1 ) &
done
wait
bad=0
printf "%-10s %8s %10s %10s %7s\n" game differ off_Mcyc on_Mcyc saved
for g in "$@"; do
  a=$(grep -c REC/hash "ab_off/$g.log")
  d=$(diff <(grep REC/hash "ab_off/$g.log") <(grep REC/hash "ab_on/$g.log") | grep -c '^<')
  x=$(grep -o "$TOTAL" "ab_off/$g.log" | head -1 | grep -o '[0-9]*$')
  y=$(grep -o "$TOTAL" "ab_on/$g.log" | head -1 | grep -o '[0-9]*$')
  if [ -z "$x" ] || [ -z "$y" ] || [ "$a" -eq 0 ]; then
    echo "$g: no result (see $DIR/ab_off/$g.log, $DIR/ab_on/$g.log)"; bad=1; continue
  fi
  [ "$d" -ne 0 ] && bad=1
  awk -v g="$g" -v d="$d" -v a="$a" -v x="$x" -v y="$y" \
    'BEGIN { printf "%-10s %4d/%-3d %10.1f %10.1f %6.1f%%\n", g, d, a, x/1e6, y/1e6, 100*(x-y)/x }'
done
exit $bad
