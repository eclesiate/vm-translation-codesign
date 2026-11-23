#!/bin/bash
# Block until every simulation whose name starts with PREFIX has finished, then print status.
# Usage: wait_runs.sh PREFIX   (e.g. wait_runs.sh t12_)
# "Finished" = its results/NAME.log ends with the /usr/bin/time summary line
# ("wall=... exit=..."), which run_sim.sh's launcher prints when run-sniper exits
# (success or failure). This avoids racing process start-up with pgrep.
LAB6="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sleep 3   # let just-launched runs create their logs
while true; do
  pending=0
  for log in "$LAB6"/results/"$1"*.log; do
    [ -e "$log" ] || continue
    grep -q "^wall=.*exit=\|frag=.*wall=" "$log" || pending=$((pending + 1))
  done
  [ "$pending" -eq 0 ] && break
  sleep 10
done
"$LAB6/scripts/status.sh" | grep -E "^$1|^mem"
