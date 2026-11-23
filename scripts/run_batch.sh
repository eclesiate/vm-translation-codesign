#!/bin/bash
# Run a file of simulation jobs, at most P at a time, fully detached.
# Usage: run_batch.sh JOBFILE [P=8]
# JOBFILE: one job per line: NAME CONFIG ICOUNT [extra run-sniper args...]  (# comments ok)
# Each job writes results/NAME/ and results/NAME.log (ending in a "wall=... exit=" line).
# Env: TRACE (default traces/randacc_8gb.sift), PERIODIC (default 0 = no snapshots).
LAB6="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS=$(realpath "$1"); P=${2:-8}
export TRACE=${TRACE:-traces/randacc_8gb.sift} PERIODIC=${PERIODIC:-0} LAB6
run_one() {
  NAME=$1; CONFIG=$2; ICOUNT=$3; shift 3
  OUT="$LAB6/results/$NAME"; rm -rf "$OUT" "$OUT.log"
  ARGS=(-c "$CONFIG" -d "$OUT" --genstats -s "stop-by-icount:$ICOUNT")
  [ "$PERIODIC" != "0" ] && ARGS+=(-s "periodicins-stats:$PERIODIC")
  ARGS+=(--traces="$TRACE" "$@")
  cd "$LAB6/virtuoso-lab" && /usr/bin/time -f "wall=%e s maxrss=%M KB exit=%x" ./run-sniper "${ARGS[@]}" > "$OUT.log" 2>&1
}
export -f run_one
# pre-create logs so wait_runs.sh sees queued jobs as pending
grep -v '^\s*#' "$JOBS" | grep -v '^\s*$' | awk '{print $1}' | while read n; do : > "$LAB6/results/$n.log"; done
setsid nohup bash -c "grep -v '^\s*#' '$JOBS' | grep -v '^\s*$' | xargs -P $P -L 1 bash -c 'run_one \"\$@\"' _" > "$LAB6/results/batch_$(basename "$JOBS").out" 2>&1 < /dev/null &
echo "batch started: $(grep -cv '^\s*#\|^\s*$' "$JOBS") jobs, $P parallel"
