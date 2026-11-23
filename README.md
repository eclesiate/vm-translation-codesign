# Hardware/OS co-design for virtual memory

Page-size-aware TLB and LLC replacement policies, and DRAM-aware page-table placement, implemented and evaluated in the
[Virtuoso](https://github.com/CMU-SAFARI/Virtuoso) (MimicOS + Sniper) simulator. This was done as a bonus lab for ETH Zürich Computer Architecture
(Fall 2025).

**Full write-up with plots: [report/REPORT.md](report/REPORT.md)**

## What's implemented
- **Page-size-aware L2 TLB replacement:** SRRIP in which 2 MB entries insert at higher priority and age more slowly,
  capped so that 4 KB entries are never starved.
- **PTE-aware LLC replacement:** page-walk lines get priority, guided by a Bloom filter of recently TLB-evicted pages,
  with a per-set cap on PTE ways.
- **DRAM bank isolation of page tables:** the MimicOS frame allocator places page-table frames in reserved banks, based
  on a reverse-engineered physical-address-to-DRAM mapping. It adds per-type (page table vs data) row-buffer statistics.
- **Pointer-chase microbenchmark:** 8.5 GiB footprint, 400 L2 TLB misses per kilo-instruction.
- **Simulator bug fixes:** the LLC block type was passed as a bool, LLC write-backs were mislabeled, and there was a
  segfault at shutdown.

## Headline results (20M instructions)
- 2 MB vs 4 KB pages: −44% cycles on randacc and −45% on the pointer chase. Average walk drops from ~200 to 15 cycles.
- Page-size-aware TLB: L2 TLB hit rate 9.6% → 12.2%.
- PTE-aware LLC: the LLC hit rate of page-walk loads roughly doubles, and cycles drop 0.6%.
- Bank isolation: the page-table row-buffer hit rate goes from 11.6% to 38.7%, but a single "hot bank" costs 1.7% IPC.
  Reserving 4 banks brings IPC back to −0.2%.

## Layout
| Path | Contents |
|---|---|
| `simulator-changes/` | Modified and new simulator files, at their paths in the source tree |
| `simulator-changes.patch` | The same changes as a unified diff against the lab's base branch |
| `scripts/` | Scripts that run experiments, parse stats (`lab6_stats.py`), aggregate results and plot |
| `jobs/` | Experiment definitions (run name, config, instructions, overrides) |
| `results/` | Aggregated CSVs and plots (raw `sim.stats` and the traces are not included because of their size) |

To reproduce, apply the changes onto a Virtuoso/Sniper tree and build with `make -j4`. Then run the job files with
`scripts/run_batch.sh`; the report's appendix has details. The traces (`randacc_8gb`) and the microbenchmark trace
(`microbenchmark/record_microbench_trace.sh`) are too large to include.
