#!/bin/bash
# Lab 6 Task 1.1: build and record the pointer-chase microbenchmark (microbench.cpp).
#
#   ./record_microbench_trace.sh [footprint_GiB=8.5] [steps=25000000] [record_instrs=30000000]
#
# Output: ../traces/pchase_<GiB>gb.sift  (used with run-sniper --traces=...)
#
# Notes:
#  * The program really allocates and touches the whole footprint natively while
#    being recorded: 8.5 GiB needs ~9 GB of free RAM (peak RSS measured 9.0 GB).
#  * --sde-arch=tgl: Intel SDE 9.44 aborts with "Unexpected invalid CPUID leaf"
#    under WSL2/Hyper-V unless it emulates an explicit CPU model (tgl = Tiger Lake,
#    the host CPU of the machine this was recorded on; skl/hsw also work).
#  * --roi: only the chase loop between SimRoiStart()/SimRoiEnd() is recorded.
#    -d: number of instructions to record (>= 20M simulated per the handout).
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
GIB=${1:-8.5}; STEPS=${2:-25000000}; INSTRS=${3:-30000000}
make
mkdir -p ../traces
../record-trace --sde-arch=tgl -d "$INSTRS" -o "../traces/pchase_${GIB}gb" --roi -- ./microbench "$GIB" "$STEPS"
ls -la "../traces/pchase_${GIB}gb.sift"
