#!/bin/bash
# Show progress of all simulations under results/ (instructions simulated so far, done/running).
LAB6="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for d in "$LAB6"/results/*/; do
  [ -f "$d/sim.cfg" ] || continue   # not a simulation dir (e.g. plots/)
  n=$(basename "$d")
  hb="$d/heartbeat.txt"
  last=$(grep -o "Periodic at [0-9]*" "$hb" 2>/dev/null | tail -1 | awk '{print $3}')
  if [ -f "$d/sim.stats" ]; then st="DONE"; elif pgrep -f -- "lib/sniper.*results/$n( |/|$)" >/dev/null; then st="running"; else st="stopped"; fi
  printf "%-28s %-8s %12s instr  %s\n" "$n" "$st" "${last:-0}" "$(tail -1 "$LAB6/results/$n.log" 2>/dev/null | cut -c1-60)"
done
free -h | awk 'NR==2{print "mem used " $3 " / " $2}'
