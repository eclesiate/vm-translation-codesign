#!/bin/bash
# Launch one detached Virtuoso/Sniper simulation (survives terminal/agent exit).
# Usage: run_sim.sh NAME CONFIG ICOUNT [extra run-sniper args...]
#   NAME    -> results/NAME/ (output dir) and results/NAME.log
#   CONFIG  -> path relative to virtuoso-lab/, e.g. config/virtuoso_configs/virtuoso_reservethp.cfg
#   ICOUNT  -> instructions to simulate (stop-by-icount)
# Env: TRACE (default traces/randacc_8gb.sift), PERIODIC (snapshot interval, default 100000; 0 = off)
# Example:
#   run_sim.sh t11_frag1.0 config/virtuoso_configs/virtuoso_reservethp.cfg 20000000 \
#       -g --perf_model/reserve_thp_allocator/target_fragmentation=1.0
set -e
LAB6="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM="$LAB6/virtuoso-lab"
NAME=$1; CONFIG=$2; ICOUNT=$3; shift 3
TRACE=${TRACE:-traces/randacc_8gb.sift}
PERIODIC=${PERIODIC:-100000}
OUT="$LAB6/results/$NAME"
mkdir -p "$LAB6/results"
rm -rf "$OUT" "$OUT.log"
ARGS=(-c "$CONFIG" -d "$OUT" --genstats -s "stop-by-icount:$ICOUNT")
[ "$PERIODIC" != "0" ] && ARGS+=(-s "periodicins-stats:$PERIODIC")
ARGS+=(--traces="$TRACE" "$@")
cd "$SIM"
setsid nohup /usr/bin/time -f "wall=%e s maxrss=%M KB exit=%x" ./run-sniper "${ARGS[@]}" > "$OUT.log" 2>&1 < /dev/null &
echo "started $NAME (pid $!) -> $OUT.log"
