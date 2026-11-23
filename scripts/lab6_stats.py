#!/usr/bin/env python3
"""Lab 6 stats helper.

Usage:
  lab6_stats.py summary RESULT_DIR [RESULT_DIR ...] [--csv out.csv]
      One row per run: IPC, L2 TLB hit rate / MPKI, page-walk cycles,
      page-size mix, DRAM row-buffer stats.
  lab6_stats.py reach RESULT_DIR [--csv out.csv]
      L2 TLB memory-reach time series from periodicins-* snapshots
      (requires the run to use: -s periodicins-stats:<interval>).

Notes:
  * SubsecondTime stats (latencies) are in femtoseconds; converted to cycles
    with the core frequency read from sim.cfg.
  * mmu.total_table_walk_latency is SUMMED over all walks; walks overlap
    (4 walkers + OoO), so it can exceed total cycles.
"""
import argparse
import configparser
import csv
import os
import re
import sqlite3
import sys

L2TLB = "mmu_TLB_L2_1"


def read_stats(path):
    stats = {}
    with open(os.path.join(path, "sim.stats")) as f:
        for line in f:
            m = re.match(r"^(\S+)\s*=\s*(\S+)", line)
            if m:
                try:
                    stats[m.group(1)] = float(m.group(2))
                except ValueError:
                    pass
    return stats


def core_freq_ghz(path):
    try:
        txt = open(os.path.join(path, "sim.cfg")).read()
        m = re.search(r"\[perf_model/core\][^\[]*?frequency\s*=\s*([\d.]+)", txt)
        if m:
            return float(m.group(1))
    except OSError:
        pass
    return 2.9


def frag_of(path):
    try:
        txt = open(os.path.join(path, "sim.cfg")).read()
        m = re.search(r"target_fragmentation\s*=\s*([\d.]+)", txt)
        return float(m.group(1)) if m else None
    except OSError:
        return None


def summarize(path):
    s = read_stats(path)
    g = lambda k: s.get(k, 0.0)
    ghz = core_freq_ghz(path)
    fs_to_cyc = lambda fs: fs * ghz * 1e-6  # fs * (GHz*1e9) / 1e15
    instr = g("performance_model.instruction_count")
    cycles = g("performance_model.cycle_count")
    acc, hit, miss = g(f"{L2TLB}.accesses"), g(f"{L2TLB}.hits"), g(f"{L2TLB}.misses")
    hits_dram = g("ddr.page-hits")
    miss_dram = g("ddr.page-miss")
    empty_dram = g("ddr.page-empty")
    dram_total = hits_dram + miss_dram + empty_dram
    # allocator stats: whichever allocator was active
    alloc = {}
    for pre in ("reserve_thp_allocator", "page_table_aware_allocator"):
        if f"{pre}.total_allocations" in s:
            alloc = {k.split(".", 1)[1]: v for k, v in s.items() if k.startswith(pre + ".")}
            break
    return {
        "run": os.path.basename(os.path.normpath(path)),
        "frag": frag_of(path),
        "instructions": int(instr),
        "cycles": int(cycles),
        "ipc": instr / cycles if cycles else 0,
        "l2tlb_accesses": int(acc),
        "l2tlb_hits": int(hit),
        "l2tlb_misses": int(miss),
        "l2tlb_hit_rate": hit / acc if acc else 0,
        "l2tlb_mpki": miss / instr * 1000 if instr else 0,
        "walk_cycles_sum": int(fs_to_cyc(g("mmu.total_table_walk_latency"))),
        "translation_cycles_sum": int(fs_to_cyc(g("mmu.total_translation_latency"))),
        "avg_walk_cycles": fs_to_cyc(g("mmu.total_table_walk_latency")) / miss if miss else 0,
        "page_faults": int(g("mmu.page_faults")),
        "alloc_4k": int(alloc.get("four_kb_allocated", 0)),
        "alloc_2m_promoted": int(alloc.get("two_mb_promoted", 0)),
        "alloc_2m_reserved": int(alloc.get("two_mb_reserved", 0)),
        "l2tlb_reach_kb_end": int(g(f"{L2TLB}.memory_reach_kb")),
        "dram_rb_hit_rate": hits_dram / dram_total if dram_total else 0,
        "dram_page_hits": int(hits_dram),
        "dram_page_miss": int(miss_dram),
        "dram_page_empty": int(empty_dram),
        # where page-walk loads were served (Task 1.3)
        "ptw_loads_L2": int(g("L1-D.loads-where-page-table-L2")),
        "ptw_loads_llc": int(g("L1-D.loads-where-page-table-nuca-cache")),
        "ptw_loads_dram": int(g("L1-D.loads-where-page-table-dram-local")),
        "ptw_llc_hit_rate": (g("L1-D.loads-where-page-table-nuca-cache")
                             / max(1.0, g("L1-D.loads-where-page-table-nuca-cache") + g("L1-D.loads-where-page-table-dram-local"))),
        # NB: Sniper's loads-where-<level> counts ALL loads incl. page-walk loads;
        # the data-only figure is total minus the page-table subset.
        "data_loads_llc": int(g("L1-D.loads-where-nuca-cache") - g("L1-D.loads-where-page-table-nuca-cache")),
        "data_loads_dram": int(g("L1-D.loads-where-dram-local") - g("L1-D.loads-where-page-table-dram-local")),
        "data_llc_hit_rate": ((g("L1-D.loads-where-nuca-cache") - g("L1-D.loads-where-page-table-nuca-cache"))
                              / max(1.0, g("L1-D.loads-where-nuca-cache") - g("L1-D.loads-where-page-table-nuca-cache")
                                    + g("L1-D.loads-where-dram-local") - g("L1-D.loads-where-page-table-dram-local"))),
        "llc_writeback_misses": int(g("nuca-cache.write-misses")),
        "l2tlb_hits_2m": int(g(f"{L2TLB}.hits_2m")),
        "l2tlb_hits_4k": int(g(f"{L2TLB}.hits_4k")),
        "bloom_hinted_walk_frac": g("pte_bloom.hinted_walks") / g("pte_bloom.walks") if g("pte_bloom.walks") else 0,
        # DRAM row-buffer outcome per request type (Task 2.3; requires the per-type stats build)
        **{f"rb_hit_rate_{t}": (g(f"ddr.page-hits-{t}") / g(f"ddr.requests-{t}")) if g(f"ddr.requests-{t}") else None
           for t in ("pt", "data")},
        **{f"rb_conflict_rate_{t}": ((g(f"ddr.page-miss-{t}") + g(f"ddr.page-closing-{t}")) / g(f"ddr.requests-{t}"))
           if g(f"ddr.requests-{t}") else None for t in ("pt", "data")},
        **{f"dram_avg_ns_{t}": (g(f"ddr.total-latency-{t}") * 1e-6 / g(f"ddr.requests-{t}")) if g(f"ddr.requests-{t}") else None
           for t in ("pt", "data")},
        "dram_requests_pt": int(g("ddr.requests-pt")),
        "dram_requests_data": int(g("ddr.requests-data")),
    }


def reach_series(path):
    db = sqlite3.connect(os.path.join(path, "sim.stats.sqlite3"))
    names = {}
    wanted = {
        ("performance_model", "instruction_count"): "instructions",
        (L2TLB, "memory_reach_kb"): "reach_kb",
        (L2TLB, "valid_entries_4k"): "entries_4k",
        (L2TLB, "valid_entries_2m"): "entries_2m",
        (L2TLB, "accesses"): "accesses",
        (L2TLB, "misses"): "misses",
    }
    for nameid, obj, met in db.execute("select nameid, objectname, metricname from names"):
        if (obj, met) in wanted:
            names[nameid] = wanted[(obj, met)]
    if "reach_kb" not in names.values():
        sys.exit("memory_reach_kb not in this run's stats (simulator built before the stat was added?)")
    prefixes = [(pid, p) for pid, p in db.execute("select prefixid, prefixname from prefixes")
                if p.startswith("periodicins-")]
    if not prefixes:
        sys.exit("no periodicins-* snapshots; rerun with -s periodicins-stats:100000")
    rows = []
    for pid, pname in sorted(prefixes, key=lambda x: int(x[1].split("-")[1])):
        row = {"snapshot": pname}
        for nameid, core, value in db.execute(
                "select nameid, core, value from `values` where prefixid=?", (pid,)):
            if nameid in names and core == 0:
                row[names[nameid]] = value
        rows.append(row)
    # per-interval L2 TLB MPKI from cumulative counters
    prev = None
    for r in rows:
        if prev and r.get("instructions", 0) > prev.get("instructions", 0):
            di = r["instructions"] - prev["instructions"]
            r["interval_mpki"] = (r.get("misses", 0) - prev.get("misses", 0)) / di * 1000
        prev = r
    return rows


def emit(rows, csv_path, cols=None):
    cols = cols or list(rows[0].keys())
    if csv_path:
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {csv_path}")
    widths = {c: max(len(c), *(len(fmt(r.get(c))) for r in rows)) for c in cols}
    print("  ".join(c.rjust(widths[c]) for c in cols))
    for r in rows:
        print("  ".join(fmt(r.get(c)).rjust(widths[c]) for c in cols))


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.4f}" if abs(v) < 100 else f"{v:.0f}"
    return str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["summary", "reach"])
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--csv")
    a = ap.parse_args()
    if a.mode == "summary":
        emit([summarize(d) for d in a.dirs], a.csv)
    else:
        rows = reach_series(a.dirs[0])
        emit(rows, a.csv, ["snapshot", "instructions", "reach_kb", "entries_4k", "entries_2m", "interval_mpki"])


if __name__ == "__main__":
    main()
